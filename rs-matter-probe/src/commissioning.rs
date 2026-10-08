//! Gateway commissioning policy and Thread provisioning over an authenticated PASE exchange.
use rs_matter::crypto::Crypto;
use rs_matter::dm::clusters::gen_comm::{
    CommissioningErrorEnum, GeneralCommissioningClient, RegulatoryLocationTypeEnum,
};
use rs_matter::dm::clusters::decl::network_commissioning::{
    NetworkCommissioningClient, NetworkCommissioningStatusEnum,
};
use rs_matter::error::{Error, ErrorCode};
use rs_matter::onboard::CommissionOptions;
use rs_matter::tlv::OctetStr;
use rs_matter::transport::exchange::Exchange;
use rs_matter::transport::network::Address;
use rs_matter::Matter;

/// Select the commissioning country and location before the accessory joins
/// the gateway's Thread network.
pub async fn configure_regulatory_region<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    peer: Address,
    passcode: u32,
    country_code: &str,
    location: crate::control::RegulatoryLocation,
) -> Result<(), Error> {
    let location = match location {
        crate::control::RegulatoryLocation::Indoor => RegulatoryLocationTypeEnum::Indoor,
        crate::control::RegulatoryLocation::Outdoor => RegulatoryLocationTypeEnum::Outdoor,
    };
    let response = Exchange::initiate_pase(matter, crypto, peer, passcode)
        .await?
        .general_commissioning()
        .set_regulatory_config(0, |request| {
            request
                .new_regulatory_config(location)?
                .country_code(country_code)?
                .breadcrumb(0)?
                .end()
        })
        .await?;
    let status = response.response()?.error_code()?;
    response.complete().await?;
    if status != CommissioningErrorEnum::OK {
        return Err(ErrorCode::Failure.into());
    }
    Ok(())
}

/// Application policy explicitly selected by the gateway owner.
/// This does not change upstream defaults or bypass PASE/CASE authentication.
#[derive(Clone, Copy)]
pub struct CommissioningPolicy {
    pub verify_manufacturer_attestation: bool,
}
impl Default for CommissioningPolicy {
    fn default() -> Self {
        Self {
            verify_manufacturer_attestation: false,
        }
    }
}
impl CommissioningPolicy {
    pub fn options(self) -> CommissionOptions {
        CommissionOptions {
            allow_test_attestation: !self.verify_manufacturer_attestation,
            ..CommissionOptions::default()
        }
    }
}

/// Borrowed, validated active dataset. Intentionally has no Debug/Display implementation:
/// operational dataset bytes contain network credentials.
pub struct ThreadDataset<'a> {
    bytes: &'a [u8],
    network_id: [u8; 8],
}
impl<'a> ThreadDataset<'a> {
    pub fn parse(bytes: &'a [u8], zigbee_channel: u8) -> Result<Self, Error> {
        if bytes.is_empty() || bytes.len() > 254 || !(11..=26).contains(&zigbee_channel) {
            return Err(ErrorCode::InvalidData.into());
        }
        let mut offset = 0;
        let mut seen = [false; 256];
        let mut network_id = [0; 8];
        while offset < bytes.len() {
            if bytes.len() - offset < 2 {
                return Err(ErrorCode::InvalidData.into());
            }
            let kind = bytes[offset] as usize;
            let size = bytes[offset + 1] as usize;
            offset += 2;
            if size == 0 || size > bytes.len() - offset || seen[kind] {
                return Err(ErrorCode::InvalidData.into());
            }
            seen[kind] = true;
            let value = &bytes[offset..offset + size];
            let valid = match kind {
                0 => size == 3 && value[0] == 0 && value[1] == 0 && value[2] == zigbee_channel,
                1 => size == 2 && value != [0xff, 0xff],
                2 => {
                    if size == 8 {
                        network_id.copy_from_slice(value);
                    }
                    size == 8 && value != [0; 8] && value != [0xff; 8]
                }
                3 => size <= 16 && core::str::from_utf8(value).is_ok() && !value.contains(&0),
                4 | 5 => size == 16,
                7 => size == 8 && value[0] == 0xfd,
                12 => size == 3 || size == 4,
                14 => size == 8,
                // Pending timestamps/delay timers do not belong to an active dataset.
                51 | 52 => false,
                53 => {
                    // Supported radio uses channel page 0, with a four-byte mask.
                    size == 6 && value[0] == 0 && value[1] == 4 && {
                        let mask = u32::from_be_bytes([value[2], value[3], value[4], value[5]]);
                        mask & (1u32 << zigbee_channel) != 0 && mask & !0x07fff800 == 0
                    }
                }
                _ => true,
            };
            if !valid {
                return Err(ErrorCode::InvalidData.into());
            }
            offset += size;
        }
        for required in [0, 1, 2, 3, 4, 5, 7, 12, 14, 53] {
            if !seen[required] {
                return Err(ErrorCode::InvalidData.into());
            }
        }
        Ok(Self { bytes, network_id })
    }
}

/// Sends AddOrUpdateThreadNetwork, then ConnectNetwork only after successful configuration.
/// Must run after fail-safe/credentials setup and before commissioning completion via CASE.
/// The owner drives Matter::run concurrently and bounds the whole commissioning operation.
pub async fn provision_thread<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    peer: Address,
    passcode: u32,
    dataset: &ThreadDataset<'_>,
) -> Result<(), Error> {
    let exchange = Exchange::initiate_pase(matter, crypto, peer, passcode).await?;
    let response = exchange
        .network_commissioning()
        .add_or_update_thread_network(0, |request| {
            request
                .operational_dataset(OctetStr::new(dataset.bytes))?
                .breadcrumb(None)?
                .end()
        })
        .await?;
    let status = response.response()?.networking_status()?;
    response.complete().await?;
    if status != NetworkCommissioningStatusEnum::Success {
        return Err(ErrorCode::Failure.into());
    }
    let exchange = Exchange::initiate_pase(matter, crypto, peer, passcode).await?;
    let response = exchange
        .network_commissioning()
        .connect_network(0, |request| {
            request
                .network_id(OctetStr::new(&dataset.network_id))?
                .breadcrumb(None)?
                .end()
        })
        .await?;
    let status = response.response()?.networking_status()?;
    response.complete().await?;
    if status != NetworkCommissioningStatusEnum::Success {
        return Err(ErrorCode::Failure.into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    pub(super) fn dataset() -> Vec<u8> {
        let mut bytes = Vec::new();
        for (kind, value) in [
            (0, vec![0, 0, 15]),
            (1, vec![0x12, 0x34]),
            (2, vec![1; 8]),
            (3, b"fixture".to_vec()),
            (4, vec![0x22; 16]),
            (5, vec![0x33; 16]),
            (7, vec![0xfd, 0, 0, 0, 0, 0, 0, 1]),
            (12, vec![2, 160, 0xff, 0xf8]),
            (14, vec![0, 0, 0, 0, 0, 1, 0, 0]),
            (53, vec![0, 4, 7, 0xff, 0xf8, 0]),
        ] {
            bytes.extend([kind, value.len() as u8]);
            bytes.extend(value);
        }
        bytes
    }
    #[test]
    fn owner_policy_skips_only_manufacturer_attestation() {
        assert!(
            CommissioningPolicy::default()
                .options()
                .allow_test_attestation
        );
        assert!(
            !CommissioningPolicy {
                verify_manufacturer_attestation: true
            }
            .options()
            .allow_test_attestation
        );
    }
    #[test]
    fn validates_credentials_and_shared_channel_without_copying_keys() {
        let bytes = dataset();
        let parsed = ThreadDataset::parse(&bytes, 15).unwrap();
        assert_eq!(parsed.bytes.as_ptr(), bytes.as_ptr());
        assert_eq!(parsed.network_id, [1; 8]);
        assert!(ThreadDataset::parse(&bytes, 20).is_err());
        assert!(ThreadDataset::parse(&bytes, 27).is_err());
    }
    #[test]
    fn rejects_truncation_duplicates_missing_fields_and_pending_dataset() {
        let bytes = dataset();
        for length in 0..bytes.len() {
            assert!(ThreadDataset::parse(&bytes[..length], 15).is_err());
        }
        let mut bad = bytes.clone();
        bad.extend([0, 3, 0, 0, 15]);
        assert!(ThreadDataset::parse(&bad, 15).is_err());
        let mut bad = bytes.clone();
        bad.extend([51, 8, 0, 0, 0, 0, 0, 0, 0, 0]);
        assert!(ThreadDataset::parse(&bad, 15).is_err());
        assert!(ThreadDataset::parse(&bytes[5..], 15).is_err());
    }
}

#[cfg(test)]
pub(crate) fn fixture_dataset() -> Vec<u8> {
    tests::dataset()
}
