//! Capability-gated operational OnOff/Level transactions. No automatic mutation replay.
use crate::{
    control::{CommandRequest, DeviceRequest, Id, Request},
    devices::Snapshot,
};
use core::num::NonZeroU8;
use rs_matter::tlv::Nullable;
use rs_matter::{
    crypto::Crypto,
    dm::clusters::app::{
        level_control::{LevelControlClient, OptionsBitmap},
        on_off::OnOffClient,
    },
    error::{Error, ErrorCode},
    transport::exchange::Exchange,
    Matter,
};
use serde::Serialize;

#[derive(Clone, Copy)]
pub enum Action {
    Read,
    ReadLevel,
    On,
    Off,
    Toggle,
    MoveToLevel {
        level: u8,
        transition_deciseconds: u16,
    },
}
#[derive(Clone, Copy)]
pub struct Operation {
    pub node: u64,
    pub endpoint: u16,
    pub action: Action,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Failure {
    Malformed,
    NotReady,
    NotFound,
    Unsupported,
    Timeout,
    Accessory,
    Busy,
    Capacity,
}
#[derive(Clone, Serialize)]
#[serde(tag = "type", content = "value", rename_all = "snake_case")]
pub enum Value {
    Bool(bool),
    U8(Option<u8>),
}
#[derive(Clone, Serialize)]
pub struct ResultValue {
    pub node: Id,
    pub endpoint: u16,
    pub cluster: u32,
    pub attribute: u32,
    pub value: Value,
}
impl Operation {
    pub fn decode(request: Request) -> Result<Self, Failure> {
        let (node, endpoint, action) = match request {
            Request::Device(DeviceRequest::Read {
                node,
                endpoint,
                cluster: 6,
                attribute: 0,
                ..
            }) => (node, endpoint, Action::Read),
            Request::Device(DeviceRequest::Read {
                node,
                endpoint,
                cluster: 8,
                attribute: 0,
                ..
            }) => (node, endpoint, Action::ReadLevel),
            Request::Command(CommandRequest::On { node, endpoint, .. }) => {
                (node, endpoint, Action::On)
            }
            Request::Command(CommandRequest::Off { node, endpoint, .. }) => {
                (node, endpoint, Action::Off)
            }
            Request::Command(CommandRequest::Toggle { node, endpoint, .. }) => {
                (node, endpoint, Action::Toggle)
            }
            Request::Command(CommandRequest::MoveToLevel {
                node,
                endpoint,
                level,
                transition_deciseconds,
                ..
            }) => (
                node,
                endpoint,
                Action::MoveToLevel {
                    level,
                    transition_deciseconds,
                },
            ),
            _ => return Err(Failure::Unsupported),
        };
        if node.0 < 2 || endpoint == u16::MAX {
            return Err(Failure::Malformed);
        }
        Ok(Self {
            node: node.0,
            endpoint,
            action,
        })
    }
    pub fn authorize(&self, snapshots: &[Snapshot]) -> Result<(), Failure> {
        let snapshot = snapshots
            .iter()
            .find(|s| s.node == self.node)
            .ok_or(Failure::NotFound)?;
        if !snapshot.online {
            return Err(Failure::NotReady);
        }
        let endpoint = snapshot
            .endpoints
            .iter()
            .find(|e| e.id == self.endpoint)
            .ok_or(Failure::NotFound)?;
        let required_cluster = match self.action {
            Action::Read | Action::On | Action::Off | Action::Toggle => 6,
            Action::ReadLevel | Action::MoveToLevel { .. } => 8,
        };
        if !endpoint.servers.contains(&required_cluster) {
            return Err(Failure::Unsupported);
        }
        Ok(())
    }
    /// Callers authorize against the current catalog/snapshot before starting.
    /// Cancellation after invocation may leave a changed accessory; do not replay.
    pub async fn run<C: Crypto>(
        self,
        matter: &Matter<'_>,
        crypto: C,
        fabric: NonZeroU8,
    ) -> Result<ResultValue, Failure> {
        self.run_for(
            matter,
            crypto,
            fabric,
            embassy_time::Duration::from_secs(30),
        )
        .await
    }
    pub async fn run_for<C: Crypto>(
        self,
        matter: &Matter<'_>,
        crypto: C,
        fabric: NonZeroU8,
        timeout: embassy_time::Duration,
    ) -> Result<ResultValue, Failure> {
        let transaction = async {
            if !matches!(self.action, Action::Read | Action::ReadLevel) {
                let exchange = Exchange::initiate(matter, &crypto, fabric, self.node).await?;
                match self.action {
                    Action::On => exchange.on_off().on(self.endpoint).await?,
                    Action::Off => exchange.on_off().off(self.endpoint).await?,
                    Action::Toggle => exchange.on_off().toggle(self.endpoint).await?,
                    Action::MoveToLevel {
                        level,
                        transition_deciseconds,
                    } => {
                        exchange
                            .level_control()
                            .move_to_level(self.endpoint, |request| {
                                request
                                    .level(level)?
                                    .transition_time(Nullable::some(transition_deciseconds))?
                                    .options_mask(OptionsBitmap::empty())?
                                    .options_override(OptionsBitmap::empty())?
                                    .end()
                            })
                            .await?
                    }
                    Action::Read | Action::ReadLevel => unreachable!(),
                }
            }
            // An invocation acknowledgement is insufficient: return the actual value.
            let exchange = Exchange::initiate(matter, &crypto, fabric, self.node).await?;
            if matches!(self.action, Action::ReadLevel | Action::MoveToLevel { .. }) {
                let value = exchange
                    .level_control()
                    .current_level_read(self.endpoint)
                    .await?;
                let value = value.as_opt_ref().copied();
                if matches!(self.action, Action::MoveToLevel { .. }) && value.is_none() {
                    return Err(ErrorCode::Failure.into());
                }
                return Ok(ResultValue {
                    node: Id(self.node),
                    endpoint: self.endpoint,
                    cluster: 8,
                    attribute: 0,
                    value: Value::U8(value),
                });
            }
            let value = exchange.on_off().on_off_read(self.endpoint).await?;
            Ok::<_, Error>(ResultValue {
                node: Id(self.node),
                endpoint: self.endpoint,
                cluster: 6,
                attribute: 0,
                value: Value::Bool(value),
            })
        };
        embassy_time::with_timeout(timeout, transaction)
            .await
            .map_err(|_| Failure::Timeout)?
            .map_err(|error| match error.code() {
                ErrorCode::RxTimeout => Failure::Timeout,
                _ => Failure::Accessory,
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unsupported_paths_and_invalid_identity_are_rejected() {
        let decode = |opcode, body: &str| {
            Operation::decode(Request::decode(opcode, body.as_bytes()).unwrap())
        };
        let level_read = decode(10, r#"{"schema":1,"action":"read","node":"0000000000000002","endpoint":1,"cluster":8,"attribute":0}"#).unwrap();
        assert!(matches!(level_read.action, Action::ReadLevel));
        assert_eq!(level_read.authorize(&[]), Err(Failure::NotFound));
        let move_to_level = decode(11, r#"{"schema":1,"command":"move_to_level","node":"0000000000000002","endpoint":1,"level":124,"transition_deciseconds":20}"#).unwrap();
        assert_eq!(move_to_level.authorize(&[]), Err(Failure::NotFound));
        assert!(matches!(
            decode(
                11,
                r#"{"schema":1,"command":"on","node":"0000000000000001","endpoint":1}"#
            ),
            Err(Failure::Malformed)
        ));
        assert!(matches!(
            decode(
                11,
                r#"{"schema":1,"command":"off","node":"0000000000000002","endpoint":65535}"#
            ),
            Err(Failure::Malformed)
        ));
        let operation = decode(
            11,
            r#"{"schema":1,"command":"toggle","node":"0000000000000002","endpoint":1}"#,
        )
        .unwrap();
        assert_eq!(operation.authorize(&[]), Err(Failure::NotFound));
        let encoded = crate::responses::reply::<_, Failure>(Ok(ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 6,
            attribute: 0,
            value: Value::Bool(false),
        }))
        .unwrap();
        assert_eq!(encoded, br#"{"schema":1,"ok":true,"result":{"node":"0000000000000002","endpoint":1,"cluster":6,"attribute":0,"value":{"type":"bool","value":false}}}"#);
        let encoded = crate::responses::reply::<_, Failure>(Ok(ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 8,
            attribute: 0,
            value: Value::U8(Some(124)),
        }))
        .unwrap();
        assert_eq!(encoded, br#"{"schema":1,"ok":true,"result":{"node":"0000000000000002","endpoint":1,"cluster":8,"attribute":0,"value":{"type":"u8","value":124}}}"#);
    }
}
