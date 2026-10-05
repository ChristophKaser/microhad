//! Operational DNS-SD over OpenThread's userspace SRP/DNS path.
use crate::{catalog::Catalog, openthread::{DnsAnswer, OpenThread}};
use embassy_time::{with_timeout, Duration as AsyncDuration, Timer};
use rs_matter::{
    error::{Error, ErrorCode},
    transport::network::{
        mdns::{DottedName, MdnsRemoteService},
        IpAddr, MatterRemoteService,
    },
    Matter,
};
use std::{
    net::Ipv6Addr,
    time::{Duration, Instant},
};

const CACHE_SIZE: usize = 16;
const MAX_TTL: u32 = 120;
#[derive(Clone, Copy, PartialEq, Eq)]
struct Key {
    fabric: u64,
    node: u64,
}
impl Key {
    fn from_service(service: &MatterRemoteService) -> Option<Self> {
        match service {
            MatterRemoteService::Operational {
                compressed_fabric_id,
                node_id,
            } => Some(Self {
                fabric: *compressed_fabric_id,
                node: *node_id,
            }),
            _ => None,
        }
    }
    fn label(self) -> String {
        format!("{:016X}-{:016X}", self.fabric, self.node)
    }
}
struct Record {
    key: Key,
    port: u16,
    addresses: Vec<IpAddr>,
    txt: Vec<(String, String)>,
    expires: Instant,
}
fn invalid() -> Error {
    ErrorCode::InvalidData.into()
}
fn parse_txt(bytes: &[u8]) -> Result<Vec<(String, String)>, Error> {
    let mut result = Vec::new();
    let mut offset = 0;
    let mut fields = 0;
    while offset < bytes.len() {
        let size = usize::from(bytes[offset]);
        offset += 1;
        let data = bytes.get(offset..offset + size).ok_or_else(invalid)?;
        offset += size;
        fields += 1;
        if fields > 32 {
            return Err(invalid());
        }
        let split = data
            .iter()
            .position(|byte| *byte == b'=')
            .unwrap_or(data.len());
        // Ignore unknown TXT extensions, including binary values; known MRP/T and
        // ICD mode fields are bounded numeric ASCII and duplicate keys are invalid.
        let Ok(key) = std::str::from_utf8(&data[..split]) else {
            continue;
        };
        if !["SII", "SAI", "SAT", "T", "ICD"]
            .iter()
            .any(|known| key.eq_ignore_ascii_case(known))
        {
            continue;
        }
        if result
            .iter()
            .any(|(prior, _): &(String, String)| prior.eq_ignore_ascii_case(key))
        {
            return Err(invalid());
        }
        let value = data.get(split + 1..).ok_or_else(invalid)?;
        if value.is_empty() || value.len() > 10 || !value.iter().all(u8::is_ascii_digit) {
            return Err(invalid());
        }
        let value = std::str::from_utf8(value).map_err(|_| invalid())?;
        let number = value.parse::<u32>().map_err(|_| invalid())?;
        if key.eq_ignore_ascii_case("SAT") && number > u32::from(u16::MAX) {
            return Err(invalid());
        }
        if key.eq_ignore_ascii_case("ICD") && number > 1 {
            return Err(invalid());
        }
        result.push((key.to_owned(), value.to_owned()));
    }
    Ok(result)
}
impl Record {
    fn parse(key: Key, answer: DnsAnswer, now: Instant) -> Result<Self, Error> {
        if answer.port == 0
            || answer.ttl == 0
            || answer.count == 0
            || answer.count > 4
            || usize::from(answer.txt_length) > answer.txt.len()
        {
            return Err(invalid());
        }
        let txt = parse_txt(&answer.txt[..usize::from(answer.txt_length)])?;
        let mut addresses = Vec::new();
        for bytes in &answer.addresses[..usize::from(answer.count)] {
            let ip = Ipv6Addr::from(*bytes);
            if ip.is_unspecified()
                || ip.is_multicast()
                || ip.is_loopback()
                || ip.is_unicast_link_local()
            {
                continue;
            }
            let address = IpAddr::V6(ip);
            if !addresses.contains(&address) {
                addresses.push(address);
            }
        }
        if addresses.is_empty() {
            return Err(invalid());
        }
        Ok(Self {
            key,
            port: answer.port,
            addresses,
            txt,
            expires: now + Duration::from_secs(u64::from(answer.ttl.min(MAX_TTL))),
        })
    }
    fn deposit(&self, matter: &Matter<'_>) {
        // rs-matter's backend-neutral resolve contract expects .local labels;
        // the on-wire SRP/DNS domain is default.service.arpa. Identity is unchanged.
        let name = format!("{}._matter._tcp.local", self.key.label());
        let answer = MdnsRemoteService {
            instance_name: DottedName(name.as_str()),
            port: Some(self.port),
            addrs: self.addresses.iter().copied(),
            txt: self
                .txt
                .iter()
                .map(|(key, value)| (key.as_str(), value.as_str())),
            scope_id: 0,
        };
        matter.transport().try_deposit_mdns_resolve(&answer, &[]);
    }
}
#[derive(Default)]
struct Cache {
    records: Vec<Record>,
}
impl Cache {
    fn get(&mut self, key: Key, now: Instant) -> Option<&Record> {
        self.records.retain(|record| record.expires > now);
        self.records.iter().find(|record| record.key == key)
    }
    fn put(&mut self, record: Record, now: Instant) {
        self.records
            .retain(|prior| prior.key != record.key && prior.expires > now);
        if self.records.len() == CACHE_SIZE {
            self.records.remove(0);
        }
        self.records.push(record);
    }
}
/// One resolver future on the same event loop as OT, Matter and the coordinator IPC.
pub async fn run(matter: &Matter<'_>, thread: &OpenThread) -> Result<(), Error> {
    let mut cache = Cache::default();
    loop {
        let service = matter.transport().wait_mdns_resolve_request().await;
        let Some(key) = Key::from_service(&service) else {
            continue;
        };
        // Attachment may finish while the gateway commissioner is already at CASE.
        // Keep this request alive within rs-matter's five-second resolve budget.
        let attaching_deadline = Instant::now() + Duration::from_millis(4900);
        while !thread.ready()
            && matter.transport().mdns_resolve_in_flight()
            && Instant::now() < attaching_deadline
        {
            cache.records.clear();
            Timer::after_millis(100).await;
        }
        if !thread.ready() || !matter.transport().mdns_resolve_in_flight() {
            cache.records.clear();
            continue;
        }
        if let Some(record) = cache.get(key, Instant::now()) {
            record.deposit(matter);
            continue;
        }
        for delay in [100, 250, 500, 1000] {
            if !matter.transport().mdns_resolve_in_flight() {
                break;
            }
            if !thread.ready() {
                cache.records.clear();
                break;
            }
            if let Ok(Ok(answer)) = with_timeout(
                AsyncDuration::from_millis(4200),
                thread.resolve_service(&key.label()),
            )
            .await
            {
                let now = Instant::now();
                if thread.ready() {
                    if let Ok(record) = Record::parse(key, answer, now) {
                        record.deposit(matter);
                        cache.put(record, now);
                        break;
                    }
                } else {
                    cache.records.clear();
                    break;
                }
            }
            Timer::after_millis(delay).await;
        }
    }
}

/// Reconcile an old allocator-only identity before normal resolution or new
/// commissioning. Query failures leave the catalog absent and retry with a
/// capped delay, so a temporary Thread/DNS outage never loses node identities.
pub async fn run_with_catalog(
    matter: &Matter<'_>,
    thread: &OpenThread,
    catalog: Catalog,
    compressed_fabric: u64,
    next_node: u64,
) -> Result<(), Error> {
    let mut delay = 250;
    while catalog.legacy_pending() {
        if !thread.ready() {
            Timer::after_millis(delay).await;
            delay = (delay * 2).min(5_000);
            continue;
        }
        match with_timeout(
            AsyncDuration::from_secs(5),
            thread.browse_operational_nodes(compressed_fabric, next_node),
        )
        .await
        {
            Ok(Ok(nodes)) => {
                catalog.migrate_legacy(&nodes, next_node)?;
                break;
            }
            _ => {
                Timer::after_millis(delay).await;
                delay = (delay * 2).min(5_000);
            }
        }
    }
    run(matter, thread).await
}

#[cfg(test)]
mod tests {
    use super::*;
    fn key(node: u64) -> Key {
        Key { fabric: 17, node }
    }
    fn answer(last: u8, ttl: u32) -> DnsAnswer {
        let mut answer = DnsAnswer::empty();
        answer.count = 1;
        answer.port = 5540;
        answer.ttl = ttl;
        answer.addresses[0][0] = 0xfd;
        answer.addresses[0][15] = last;
        answer
    }
    #[test]
    fn abi_and_operational_instance_identity_match_the_sdk_contract() {
        assert_eq!(std::mem::size_of::<DnsAnswer>(), 332);
        assert_eq!(key(2).label(), "0000000000000011-0000000000000002");
        let service = MatterRemoteService::Operational {
            compressed_fabric_id: 17,
            node_id: 2,
        };
        let name = format!("{}._matter._tcp.local", key(2).label());
        assert!(service.matches_instance(&DottedName(name.as_str())));
        assert!(Key::from_service(&MatterRemoteService::Commissionable { id: 1 }).is_none());
    }
    #[test]
    fn invalid_metadata_and_unroutable_addresses_never_reach_case() {
        let now = Instant::now();
        for value in [
            DnsAnswer::empty(),
            answer(2, 0),
            {
                let mut value = answer(2, 30);
                value.port = 0;
                value
            },
            {
                let mut value = answer(2, 30);
                value.count = 5;
                value
            },
            {
                let mut value = answer(2, 30);
                value.txt_length = 257;
                value
            },
        ] {
            assert!(Record::parse(key(2), value, now).is_err());
        }
        for ip in ["::", "::1", "ff03::1", "fe80::1"] {
            let mut value = answer(2, 30);
            value.addresses[0] = ip.parse::<Ipv6Addr>().unwrap().octets();
            assert!(Record::parse(key(2), value, now).is_err());
        }
    }
    #[test]
    fn lit_advertisement_is_preserved_and_malformed_modes_are_rejected() {
        let parsed = parse_txt(b"\x05ICD=1\x07SII=300000").unwrap();
        assert_eq!(
            parsed,
            vec![("ICD".into(), "1".into()), ("SII".into(), "300000".into())]
        );
        assert!(parse_txt(b"\x05ICD=2").is_err());
        assert!(parse_txt(b"\x05ICD=x").is_err());
        assert!(parse_txt(b"\x05ICD=1\x05icd=0").is_err());
        assert!(parse_txt(b"\x05ICD=").is_err());
    }
    #[test]
    fn duplicate_and_invalid_addresses_do_not_displace_routable_candidates() {
        let mut value = answer(2, 30);
        value.count = 4;
        value.addresses[1] = value.addresses[0];
        value.addresses[2] = [0xff; 16];
        value.addresses[3] = "2001:db8::3".parse::<Ipv6Addr>().unwrap().octets();
        let record = Record::parse(key(2), value, Instant::now()).unwrap();
        assert_eq!(record.addresses.len(), 2);
    }
    #[test]
    fn known_txt_fields_are_numeric_bounded_and_duplicate_safe() {
        let fields = parse_txt(b"\x07SII=500\x07SAI=300\x08SAT=4000\x03T=0").unwrap();
        assert_eq!(fields.len(), 4);
        for value in [
            &b"\x07SII=500\x07sii=600"[..],
            &b"\x05SII=x"[..],
            &b"\x03SAT"[..],
            &b"\x09SAT=65536"[..],
            &b"\x0eSII=4294967296"[..],
            &b"\x10cut"[..],
        ] {
            assert!(parse_txt(value).is_err());
        }
        assert!(parse_txt(b"\x03X=\xff").unwrap().is_empty());
    }
    #[test]
    fn ttl_is_capped_and_cached_reads_never_extend_expiration() {
        let now = Instant::now();
        let mut cache = Cache::default();
        cache.put(Record::parse(key(2), answer(2, 10000), now).unwrap(), now);
        assert!(cache.get(key(2), now + Duration::from_secs(119)).is_some());
        assert!(cache.get(key(2), now + Duration::from_secs(120)).is_none());
        cache.put(Record::parse(key(2), answer(2, 1), now).unwrap(), now);
        assert!(cache
            .get(key(2), now + Duration::from_millis(999))
            .is_some());
        assert!(cache.get(key(2), now + Duration::from_secs(1)).is_none());
    }
    #[test]
    fn cache_is_bounded_and_new_dns_answers_replace_old_addresses() {
        let now = Instant::now();
        let mut cache = Cache::default();
        for node in 1..=17 {
            cache.put(
                Record::parse(key(node), answer(node as u8, 30), now).unwrap(),
                now,
            );
        }
        assert_eq!(cache.records.len(), CACHE_SIZE);
        assert!(cache.get(key(1), now).is_none());
        cache.put(Record::parse(key(2), answer(99, 30), now).unwrap(), now);
        assert_eq!(cache.records.len(), CACHE_SIZE);
        assert_eq!(
            cache.get(key(2), now).unwrap().addresses[0],
            "fd00::63".parse::<IpAddr>().unwrap()
        );
    }
}
