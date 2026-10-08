/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#![recursion_limit = "256"]

//! Synthetic accessory commissioning over the factory userspace OpenThread transport.
//! Adapted from the pinned upstream commissioning fixture; never production state.
use core::cell::{Cell, RefCell};
use core::pin::pin;
use embassy_futures::select::{select, select3, select4, Either};
use embassy_time::{Duration, Timer};
use lexra_rs_matter_probe::identity::Identity;
use lexra_rs_matter_probe::service::IcdCheckInResponder;
use lexra_rs_matter_probe::store::PrivateStore;
use lexra_rs_matter_probe::{
    ble::BleRelay, commissioning as gateway_commissioning, openthread::OpenThread,
};
use rs_matter::cert::gen::VALID_FOREVER;
use rs_matter::cert::{MAX_CERT_TLV_AND_ASN1_LEN, MAX_CERT_TLV_LEN};
use rs_matter::crypto::{default_crypto, Crypto, CryptoSensitiveRef, Rng};
use rs_matter::dm::clusters::app::level_control::{self, LevelControlHooks};
use rs_matter::dm::clusters::app::on_off::{
    self, test::TestOnOffDeviceLogic, OnOffClient, OnOffHooks,
};
use rs_matter::dm::clusters::decl::network_commissioning::{self, NetworkCommissioningStatusEnum};
use rs_matter::dm::clusters::desc::{self, ClusterHandler as _};
use rs_matter::dm::clusters::icd_mgmt::{
    ClusterHandler as _, IcdManagementClient as _, IcdModeConfig, LitIcd, LitIcdMgmtHandler,
};
use rs_matter::dm::clusters::net_comm::DummyNetworks;
use rs_matter::dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET};
use rs_matter::dm::devices::DEV_TYPE_ON_OFF_LIGHT;
use rs_matter::dm::{endpoints, Async, DataModel, Dataver, Endpoint, Handler, Node, Reply};
use rs_matter::error::Error;
use rs_matter::im::subscriptions::DEFAULT_MAX_SUBSCRIPTIONS;
use rs_matter::im::{InteractionModel, InteractionModelState};
use rs_matter::onboard::{CommissionResult, Commissioner};
use rs_matter::persist::DummyKvBlobStore;
use rs_matter::respond::DefaultResponder;
use rs_matter::sc::pase::MAX_COMM_WINDOW_TIMEOUT_SECS;
use rs_matter::tlv::{Nullable, TLVTag, TLVWrite};
use rs_matter::transport::exchange::{Exchange, MatterBuffers};
use rs_matter::transport::network::mdns::{DottedName, MdnsRemoteService};
use rs_matter::transport::network::{
    btp::Btp, Address, BtAddr, ChainedNetwork, IpAddr, MatterRemoteService, NoNetwork, SocketAddr,
    SocketAddrV6,
};
use rs_matter::utils::init::InitMaybeUninit;
use rs_matter::utils::select::Coalesce;
use rs_matter::{clusters, devices, root_endpoint, Matter, MATTER_PORT};
use static_cell::StaticCell;

fn native_test_response(
    request: lexra_rs_matter_probe::ipc::Frame,
    manager: &mut lexra_rs_matter_probe::devices::Manager<'_, impl Crypto>,
) -> Option<lexra_rs_matter_probe::ipc::Frame> {
    use lexra_rs_matter_probe::control::{DeviceRequest, Request, ThreadRequest};

    let payload = match Request::decode(request.opcode, &request.payload) {
        Ok(Request::MatterStatus) => br#"{"enabled":true,"available":true,"controller_initialized":true,"fabric_ready":true,"state":"running"}"#.to_vec(),
        Ok(Request::ThreadStatus) | Ok(Request::Thread(ThreadRequest::Status { .. })) => {
            br#"{"enabled":true,"available":true,"configured":true,"attached":true,"state":"attached","linux_ipv6_or_tun":false}"#.to_vec()
        }
        Ok(Request::Thread(ThreadRequest::Observe {
            zigbee_channel,
            zigbee_online,
            ..
        })) => {
            let valid_channel = zigbee_online && (11..=26).contains(&zigbee_channel);
            let status = if valid_channel {
                format!("{{\"enabled\":true,\"available\":true,\"configured\":true,\"attached\":true,\"state\":\"attached\",\"zigbee_channel\":{},\"dataset_channel\":{},\"linux_ipv6_or_tun\":false}}", zigbee_channel, zigbee_channel)
            } else {
                r#"{"enabled":true,"available":true,"configured":false,"attached":false,"state":"detached","linux_ipv6_or_tun":false}"#.to_owned()
            };
            format!("{{\"schema\":1,\"ok\":true,\"result\":{status}}}").into_bytes()
        }
        Ok(Request::Device(DeviceRequest::Snapshot {
            cursor, revision, ..
        })) => {
            let page = manager.page(cursor, revision);
            lexra_rs_matter_probe::responses::reply(page).unwrap_or_default()
        }
        Ok(_) => lexra_rs_matter_probe::responses::reply::<(), _>(Err("unsupported"))
            .unwrap_or_default(),
        Err(_) => lexra_rs_matter_probe::responses::reply::<(), _>(Err("malformed"))
            .unwrap_or_default(),
    };
    (!payload.is_empty()).then_some(lexra_rs_matter_probe::ipc::Frame {
        kind: 1,
        opcode: request.opcode,
        id: request.id,
        payload,
    })
}

const TEST_PASSCODE: u32 = 20202021;
type DeviceDmState = InteractionModelState<DummyNetworks, DEFAULT_MAX_SUBSCRIPTIONS, 0>;
static DEVICE_MATTER: StaticCell<Matter> = StaticCell::new();
static DEVICE_BUFFERS: StaticCell<MatterBuffers> = StaticCell::new();
static DEVICE_STATE: StaticCell<DeviceDmState> = StaticCell::new();
static CTRL_MATTER: StaticCell<Matter> = StaticCell::new();
static RESTART_MATTER: StaticCell<Matter> = StaticCell::new();
// Synthetic accessory only: accept credentials but never create a real mesh.
struct TestThreadNetwork {
    dataset: RefCell<Vec<u8>>,
    add_status: Cell<NetworkCommissioningStatusEnum>,
    connect_status: Cell<NetworkCommissioningStatusEnum>,
    connect_calls: Cell<usize>,
}
impl TestThreadNetwork {
    fn new() -> Self {
        Self {
            dataset: RefCell::new(Vec::new()),
            add_status: Cell::new(NetworkCommissioningStatusEnum::Success),
            connect_status: Cell::new(NetworkCommissioningStatusEnum::Success),
            connect_calls: Cell::new(0),
        }
    }
}
impl rs_matter::dm::NonBlockingHandler for TestThreadNetwork {}
impl Handler for TestThreadNetwork {
    fn read(
        &self,
        _: impl rs_matter::dm::ReadContext,
        _: impl rs_matter::dm::ReadReply,
    ) -> Result<(), Error> {
        Err(rs_matter::error::ErrorCode::AttributeNotFound.into())
    }
    fn bump_dataver(&self, _: impl rs_matter::dm::MatchContext) {}
    fn invoke(
        &self,
        ctx: impl rs_matter::dm::InvokeContext,
        reply: impl rs_matter::dm::InvokeReply,
    ) -> Result<(), Error> {
        let (response_id, status) = match ctx.cmd().cmd_id {
            3 => {
                let request =
                    network_commissioning::AddOrUpdateThreadNetworkRequest::new(ctx.data().clone());
                let bytes = request.operational_dataset()?.0;
                gateway_commissioning::ThreadDataset::parse(bytes, 15)?;
                let status = self.add_status.get();
                if status == NetworkCommissioningStatusEnum::Success {
                    *self.dataset.borrow_mut() = bytes.to_vec();
                }
                (5, status)
            }
            6 => {
                let request = network_commissioning::ConnectNetworkRequest::new(ctx.data().clone());
                assert_eq!(request.network_id()?.0, &[1; 8]);
                assert!(!self.dataset.borrow().is_empty());
                self.connect_calls.set(self.connect_calls.get() + 1);
                (7, self.connect_status.get())
            }
            _ => return Err(rs_matter::error::ErrorCode::CommandNotFound.into()),
        };
        let mut writer = reply.with_command(response_id)?;
        let tag = writer.tag();
        writer.writer().start_struct(tag)?;
        writer.writer().u8(&TLVTag::Context(0), status as u8)?;
        if response_id == 7 {
            writer.writer().null(&TLVTag::Context(2))?;
        } else if status == NetworkCommissioningStatusEnum::Success {
            writer.writer().u8(&TLVTag::Context(2), 0)?;
        }
        writer.writer().end_container()?;
        writer.complete()
    }
}

use rs_matter::dm::clusters::decl::globals::{MeasurementMediumEnum, MeasurementUnitEnum};
use rs_matter::dm::clusters::decl::temperature_measurement::{
    self, ClusterHandler as _, TemperatureMeasurementClient,
};
use rs_matter::dm::clusters::decl::{
    air_quality, boolean_state, carbon_dioxide_concentration_measurement as co2_measurement,
    pm_25_concentration_measurement as pm25_measurement,
    relative_humidity_measurement as humidity_measurement,
};

// Test-only sensor: exercise signed and unknown measurements over actual CASE exchanges.
struct TestTemperature {
    value: Cell<Option<i16>>,
    dataver: Cell<u32>,
}

struct TestLevelControl;
impl LevelControlHooks for TestLevelControl {
    const MIN_LEVEL: u8 = 1;
    const MAX_LEVEL: u8 = 254;
    const FASTEST_RATE: u8 = 50;
    const CURRENT_LEVEL: Option<u8> = Some(1);
    const PERSIST_DELAY_MS: u32 = 0;
    const CLUSTER: rs_matter::dm::Cluster<'static> =
        level_control::test::TestLevelControlDeviceLogic::CLUSTER.with_features(0);
    fn set_device_level(&self, level: u8) -> Result<Option<u8>, ()> {
        Ok(Some(level))
    }
}
impl temperature_measurement::ClusterHandler for TestTemperature {
    const CLUSTER: rs_matter::dm::Cluster<'static> = temperature_measurement::FULL_CLUSTER;
    fn dataver(&self) -> u32 {
        self.dataver.get()
    }
    fn dataver_changed(&self) {
        self.dataver.set(self.dataver.get().wrapping_add(1));
    }
    fn measured_value(&self, _: impl rs_matter::dm::ReadContext) -> Result<Nullable<i16>, Error> {
        Ok(self
            .value
            .get()
            .map(Nullable::some)
            .unwrap_or_else(Nullable::none))
    }
    fn min_measured_value(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<Nullable<i16>, Error> {
        Ok(Nullable::some(-27315))
    }
    fn max_measured_value(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<Nullable<i16>, Error> {
        Ok(Nullable::some(32767))
    }
}
struct TestHumidity {
    value: Cell<Option<u16>>,
    dataver: Cell<u32>,
}
impl humidity_measurement::ClusterHandler for TestHumidity {
    const CLUSTER: rs_matter::dm::Cluster<'static> = humidity_measurement::FULL_CLUSTER;
    fn dataver(&self) -> u32 {
        self.dataver.get()
    }
    fn dataver_changed(&self) {
        self.dataver.set(self.dataver.get().wrapping_add(1));
    }
    fn measured_value(&self, _: impl rs_matter::dm::ReadContext) -> Result<Nullable<u16>, Error> {
        Ok(self
            .value
            .get()
            .map(Nullable::some)
            .unwrap_or_else(Nullable::none))
    }
    fn min_measured_value(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<Nullable<u16>, Error> {
        Ok(Nullable::some(0))
    }
    fn max_measured_value(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<Nullable<u16>, Error> {
        Ok(Nullable::some(10_000))
    }
}
struct TestConcentration {
    value: Cell<Option<f32>>,
    dataver: Cell<u32>,
}
impl co2_measurement::ClusterHandler for TestConcentration {
    const CLUSTER: rs_matter::dm::Cluster<'static> = co2_measurement::FULL_CLUSTER;
    fn dataver(&self) -> u32 {
        self.dataver.get()
    }
    fn dataver_changed(&self) {
        self.dataver.set(self.dataver.get().wrapping_add(1));
    }
    fn measured_value(&self, _: impl rs_matter::dm::ReadContext) -> Result<Nullable<f32>, Error> {
        Ok(self
            .value
            .get()
            .map(Nullable::some)
            .unwrap_or_else(Nullable::none))
    }
    fn measurement_unit(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<MeasurementUnitEnum, Error> {
        Ok(MeasurementUnitEnum::PPM)
    }
    fn measurement_medium(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<MeasurementMediumEnum, Error> {
        Ok(MeasurementMediumEnum::Air)
    }
}
impl pm25_measurement::ClusterHandler for TestConcentration {
    const CLUSTER: rs_matter::dm::Cluster<'static> = pm25_measurement::FULL_CLUSTER;
    fn dataver(&self) -> u32 {
        self.dataver.get()
    }
    fn dataver_changed(&self) {
        self.dataver.set(self.dataver.get().wrapping_add(1));
    }
    fn measured_value(&self, _: impl rs_matter::dm::ReadContext) -> Result<Nullable<f32>, Error> {
        Ok(self
            .value
            .get()
            .map(Nullable::some)
            .unwrap_or_else(Nullable::none))
    }
    fn measurement_unit(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<MeasurementUnitEnum, Error> {
        Ok(MeasurementUnitEnum::UGM3)
    }
    fn measurement_medium(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<MeasurementMediumEnum, Error> {
        Ok(MeasurementMediumEnum::Air)
    }
}
struct TestAirQuality {
    value: Cell<air_quality::AirQualityEnum>,
    dataver: Cell<u32>,
}
struct TestBooleanState {
    value: Cell<bool>,
    dataver: Cell<u32>,
}
impl boolean_state::ClusterHandler for TestBooleanState {
    const CLUSTER: rs_matter::dm::Cluster<'static> = boolean_state::FULL_CLUSTER;
    fn dataver(&self) -> u32 {
        self.dataver.get()
    }
    fn dataver_changed(&self) {
        self.dataver.set(self.dataver.get().wrapping_add(1));
    }
    fn state_value(&self, _: impl rs_matter::dm::ReadContext) -> Result<bool, Error> {
        Ok(self.value.get())
    }
}
impl air_quality::ClusterHandler for TestAirQuality {
    const CLUSTER: rs_matter::dm::Cluster<'static> = air_quality::FULL_CLUSTER;
    fn dataver(&self) -> u32 {
        self.dataver.get()
    }
    fn dataver_changed(&self) {
        self.dataver.set(self.dataver.get().wrapping_add(1));
    }
    fn air_quality(
        &self,
        _: impl rs_matter::dm::ReadContext,
    ) -> Result<air_quality::AirQualityEnum, Error> {
        Ok(self.value.get())
    }
}
const APP_ENDPOINT: Endpoint<'static> = Endpoint::new(
    1,
    devices!(DEV_TYPE_ON_OFF_LIGHT),
    clusters!(
        desc::DescHandler::CLUSTER,
        TestOnOffDeviceLogic::CLUSTER,
        TestLevelControl::CLUSTER,
        TestTemperature::CLUSTER,
        humidity_measurement::FULL_CLUSTER,
        co2_measurement::FULL_CLUSTER,
        pm25_measurement::FULL_CLUSTER,
        air_quality::FULL_CLUSTER,
        boolean_state::FULL_CLUSTER,
    ),
);

const NODE: Node<'static> = Node {
    endpoints: &[root_endpoint!(thread), APP_ENDPOINT],
};

const NODE_ICD: Node<'static> = Node {
    endpoints: &[
        Endpoint {
            id: 0,
            device_types: devices!(rs_matter::dm::devices::DEV_TYPE_ROOT_NODE),
            clusters: clusters!(thread; LitIcdMgmtHandler::CLUSTER),
            client_clusters: &[],
            unique_id: None,
            semantic_tags: &[],
        },
        APP_ENDPOINT,
    ],
};

fn active_store_identity_node(matter: &Matter<'_>, fabric: core::num::NonZeroU8) -> Result<u64, Error> {
    matter.with_state(|state| Ok(state.fabrics.fabric(fabric)?.node_id()))
}

/// Synthetic accessory guard: reject credential installation before the
/// commissioner has armed its fail-safe and configured the selected region.
struct CommissioningOrder<H> {
    inner: H,
    enforce: bool,
    armed: Cell<bool>,
    configured: Cell<bool>,
}
impl<H: rs_matter::dm::AsyncHandler> rs_matter::dm::AsyncHandler for CommissioningOrder<H> {
    async fn read(&self, ctx: impl rs_matter::dm::ReadContext, reply: impl rs_matter::dm::ReadReply) -> Result<(), Error> {
        self.inner.read(ctx, reply).await
    }
    async fn write(&self, ctx: impl rs_matter::dm::WriteContext) -> Result<(), Error> {
        self.inner.write(ctx).await
    }
    fn bump_dataver(&self, ctx: impl rs_matter::dm::MatchContext) {
        self.inner.bump_dataver(ctx);
    }
    fn lifecycle(&self, ctx: impl rs_matter::dm::HandlerContext, op: rs_matter::dm::LifecycleOp) -> Result<(), Error> {
        self.inner.lifecycle(ctx, op)
    }
    async fn run(&self, ctx: impl rs_matter::dm::HandlerContext) -> Result<(), Error> {
        self.inner.run(ctx).await
    }
    async fn invoke(&self, ctx: impl rs_matter::dm::InvokeContext, reply: impl rs_matter::dm::InvokeReply) -> Result<(), Error> {
        let path = (ctx.cmd().cluster_id, ctx.cmd().cmd_id);
        if self.enforce {
            if path == (48, 2) {
                assert!(self.armed.get(), "regulatory configuration precedes ArmFailSafe");
                let request = rs_matter::dm::clusters::gen_comm::SetRegulatoryConfigRequest::new(ctx.data().clone());
                assert_eq!(request.country_code()?, "DE");
                assert_eq!(request.new_regulatory_config()?,
                    rs_matter::dm::clusters::gen_comm::RegulatoryLocationTypeEnum::Indoor);
            }
            if path.0 == 62 && matches!(path.1, 4 | 6 | 11) {
                assert!(self.configured.get(), "credentials precede regulatory configuration");
            }
        }
        self.inner.invoke(ctx, reply).await?;
        if path == (48, 0) {
            self.armed.set(true);
            self.configured.set(false);
        } else if path == (48, 2) {
            self.configured.set(true);
        }
        Ok(())
    }
}

fn data_model<'a>(
    mut rand: impl Rng + Copy,
    on_off: &'a on_off::OnOffHandler<'a, TestOnOffDeviceLogic, on_off::NoLevelControl>,
    level: &'a level_control::LevelControlHandler<'a, TestLevelControl, level_control::NoOnOff>,
    temperature: &'a TestTemperature,
    humidity: &'a TestHumidity,
    co2: &'a TestConcentration,
    pm25: &'a TestConcentration,
    air_quality: &'a TestAirQuality,
    boolean_state: &'a TestBooleanState,
    thread_network: &'a TestThreadNetwork,
    lit: &'a LitIcd,
    icd: bool,
    enforce_region_order: bool,
) -> impl DataModel + 'a {
    (
        if icd { NODE_ICD } else { NODE },
        CommissioningOrder {
            inner: endpoints::EthSysHandlerBuilder::new().build(rand)
            .chain(
                |e, c| e == 1 && c == desc::DescHandler::CLUSTER.id,
                Async(desc::DescHandler::new(Dataver::new_rand(&mut rand)).adapt()),
            )
            .chain(
                |e, c| e == 1 && c == TestOnOffDeviceLogic::CLUSTER.id,
                Async(on_off::HandlerAdaptor(on_off)),
            )
            .chain(
                |e, c| e == 1 && c == TestLevelControl::CLUSTER.id,
                Async(level_control::HandlerAdaptor(level)),
            )
            .chain(
                |e, c| e == 1 && c == TestTemperature::CLUSTER.id,
                Async(temperature_measurement::HandlerAdaptor(temperature)),
            )
            .chain(
                |e, c| e == 1 && c == humidity_measurement::FULL_CLUSTER.id,
                Async(humidity_measurement::HandlerAdaptor(humidity)),
            )
            .chain(
                |e, c| e == 1 && c == co2_measurement::FULL_CLUSTER.id,
                Async(co2_measurement::HandlerAdaptor(co2)),
            )
            .chain(
                |e, c| e == 1 && c == pm25_measurement::FULL_CLUSTER.id,
                Async(pm25_measurement::HandlerAdaptor(pm25)),
            )
            .chain(
                |e, c| e == 1 && c == air_quality::FULL_CLUSTER.id,
                Async(air_quality::HandlerAdaptor(air_quality)),
            )
            .chain(
                |e, c| e == 1 && c == boolean_state::FULL_CLUSTER.id,
                Async(boolean_state::HandlerAdaptor(boolean_state)),
            )
            .chain(|e, c| e == 0 && c == 49, Async(thread_network))
            .chain(
                |e, c| e == 0 && c == LitIcdMgmtHandler::CLUSTER.id,
                Async(LitIcdMgmtHandler::new(Dataver::new_rand(&mut rand), lit).adapt()),
            ),
            enforce: enforce_region_order,
            armed: Cell::new(false),
            configured: Cell::new(false),
        },
    )
}

fn dataset() -> Vec<u8> {
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
use core::convert::Infallible;
use rs_matter::crypto::{TryCryptoRng, TryRng};
use std::fs::File;
use std::io::Read;
use std::os::unix::net::UnixStream;
// Actual OS entropy; read failures abort this test rather than returning weak bytes.
struct OsEntropy(File);
impl TryRng for OsEntropy {
    type Error = Infallible;
    fn try_next_u32(&mut self) -> Result<u32, Infallible> {
        let mut bytes = [0; 4];
        self.try_fill_bytes(&mut bytes)?;
        Ok(u32::from_ne_bytes(bytes))
    }
    fn try_next_u64(&mut self) -> Result<u64, Infallible> {
        let mut bytes = [0; 8];
        self.try_fill_bytes(&mut bytes)?;
        Ok(u64::from_ne_bytes(bytes))
    }
    fn try_fill_bytes(&mut self, bytes: &mut [u8]) -> Result<(), Infallible> {
        self.0.read_exact(bytes).expect("OS entropy read failed");
        Ok(())
    }
}
impl TryCryptoRng for OsEntropy {}

async fn wait_manager_subscriptions<C: Crypto>(
    manager: &mut lexra_rs_matter_probe::devices::Manager<'_, C>,
    node: u64,
    clusters: &[u32],
) -> Result<(), Error> {
    embassy_time::with_timeout(
        Duration::from_secs(60),
        std::future::poll_fn(|context| {
            manager.poll(true, false, std::time::Instant::now(), context);
            if clusters
                .iter()
                .all(|cluster| manager.subscription_id(node, 1, *cluster, 0).is_some())
            {
                std::task::Poll::Ready(())
            } else {
                context.waker().wake_by_ref();
                std::task::Poll::Pending
            }
        }),
    )
    .await
    .map_err(|_| {
        let missing = clusters
            .iter()
            .filter(|cluster| manager.subscription_id(node, 1, **cluster, 0).is_none())
            .collect::<Vec<_>>();
        eprintln!("timed out waiting for CASE subscriptions; missing clusters: {missing:?}");
        rs_matter::error::ErrorCode::RxTimeout.into()
    })
}

async fn run(
    thread: &OpenThread,
    use_ble: bool,
    service_owner: bool,
    service_discovery: bool,
    sensor_reports: bool,
    native_ipc: bool,
    icd: bool,
) -> Result<(), Error> {
    let mut entropy =
        OsEntropy(std::fs::File::open("/dev/urandom").expect("OS entropy unavailable"));
    let mut singleton = [0; 32];
    rs_matter::crypto::TryRng::try_fill_bytes(&mut entropy, &mut singleton).unwrap();
    let crypto = default_crypto(entropy, CryptoSensitiveRef::new(&singleton));
    let device_matter = DEVICE_MATTER.uninit().init_with(Matter::init(
        &TEST_DEV_DET,
        TEST_DEV_COMM,
        &TEST_DEV_ATT,
        MATTER_PORT,
    ));

    let device_crypto = &crypto;
    let mut rand = device_crypto.rand()?;

    let device_buffers = DEVICE_BUFFERS.uninit().init_with(MatterBuffers::init());
    let device_state = DEVICE_STATE.init(InteractionModelState::new(DummyNetworks));

    let on_off_handler = on_off::OnOffHandler::new_standalone(
        Dataver::new_rand(&mut rand),
        1,
        rs_matter::persist::VENDOR_KEYS_START + 0x10,
        TestOnOffDeviceLogic::new(false),
    );
    let level_control_handler = level_control::LevelControlHandler::new_standalone(
        Dataver::new_rand(&mut rand),
        1,
        rs_matter::persist::VENDOR_KEYS_START + 0x11,
        TestLevelControl,
    );

    let temperature = TestTemperature {
        value: Cell::new(Some(2315)),
        dataver: Cell::new(1),
    };
    let humidity = TestHumidity {
        value: Cell::new(Some(5234)),
        dataver: Cell::new(1),
    };
    let co2 = TestConcentration {
        value: Cell::new(Some(758.5)),
        dataver: Cell::new(1),
    };
    let pm25 = TestConcentration {
        value: Cell::new(Some(12.75)),
        dataver: Cell::new(1),
    };
    let air_quality = TestAirQuality {
        value: Cell::new(air_quality::AirQualityEnum::Moderate),
        dataver: Cell::new(1),
    };
    let boolean_state = TestBooleanState {
        value: Cell::new(false),
        dataver: Cell::new(1),
    };
    let thread_network = TestThreadNetwork::new();
    let lit = LitIcd::new(
        4,
        IcdModeConfig {
            idle_mode_duration_s: 2,
            active_mode_duration_ms: 1_500,
            active_mode_threshold_ms: 1_000,
            user_active_mode_trigger_hint: 0,
            user_active_mode_trigger_instruction: "",
        },
        500,
    );
    let device_kv = device_matter.kv(DummyKvBlobStore);

    let dm = InteractionModel::new(
        device_matter,
        &device_crypto,
        device_buffers,
        data_model(
            rand,
            &on_off_handler,
            &level_control_handler,
            &temperature,
            &humidity,
            &co2,
            &pm25,
            &air_quality,
            &boolean_state,
            &thread_network,
            &lit,
            icd,
            service_owner,
        ),
        &device_kv,
        device_state,
    );
    device_matter.startup(&device_kv)?;
    dm.startup().await?;

    // Open commissioning window before starting the mDNS responder so the
    // `wait_mdns` signal is already set when the broadcast loop first runs.
    device_matter.open_basic_comm_window(MAX_COMM_WINDOW_TIMEOUT_SECS, &device_crypto, &())?;

    let device_net = thread.bind(5540)?;
    let ctrl_net = thread.bind(5541)?;
    let controller_operational = thread.own_address(5541)?;
    let Address::Udp(SocketAddr::V6(controller_operational)) = controller_operational else {
        unreachable!()
    };

    let ctrl_matter = CTRL_MATTER.uninit().init_with(Matter::init(
        &TEST_DEV_DET,
        TEST_DEV_COMM,
        &TEST_DEV_ATT,
        0,
    ));
    let ctrl_crypto = &crypto;
    let state_directory = std::env::var_os("MICROHAD_MATTER_TEST_STATE_DIR")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| "controller-state".into());
    let store = RefCell::new(Some(
        PrivateStore::open(&state_directory).expect("private fixture state"),
    ));
    {
        let active_store = store.borrow().as_ref().unwrap().clone();
        let kv = ctrl_matter.kv(active_store);
        ctrl_matter.startup(&kv)?;
        assert!(!ctrl_matter.has_fabrics());
    }
    let phone_connected = Cell::new(use_ble);

    let responder = DefaultResponder::new(&dm);
    let controller_responder = IcdCheckInResponder::new(ctrl_matter);
    let ble = BleRelay::new();
    let generation = ble.connect(BtAddr([0xbb; 6]), 247)?;
    let relay_generation = Cell::new(if service_owner { 0 } else { generation });
    let accessory_btp = Btp::new();
    let controller_btp = ble.network(generation)?;
    let can_btp = |address: &Address| matches!(address, Address::Btp(_));
    let device_send = ChainedNetwork::new(can_btp, &accessory_btp, &device_net);
    let device_receive = device_send.clone();
    let controller_send = ChainedNetwork::new(can_btp, controller_btp, &ctrl_net);
    let controller_receive = controller_send.clone();
    let gatt = async {
        loop {
            if !phone_connected.get() || relay_generation.get() == 0 {
                Timer::after(Duration::from_millis(10)).await;
                continue;
            }
            if let Some(packet) = ble.next_write(relay_generation.get())? {
                accessory_btp.process_incoming(Some(247), BtAddr([0xaa; 6]), &packet.bytes)?;
                ble.write_complete(relay_generation.get(), packet.sequence, true)?;
            }
            let mut bytes = [0; 512];
            let length = accessory_btp.process_outgoing(Some(247), &mut bytes)?;
            if length != 0 {
                ble.indication(relay_generation.get(), &bytes[..length])?;
            }
            Timer::after(Duration::from_millis(1)).await;
        }
        #[allow(unreachable_code)]
        Ok::<_, Error>(())
    };
    let device = async {
        let resolve_controller = async {
            if icd {
                // The ICD Check-In is sessionless and the device resolves the
                // registered controller node independently of the CASE path.
                resolve_fixture(device_matter, 0, controller_operational).await;
            } else {
                core::future::pending().await
            }
        };
        select4(
            device_matter.run(device_crypto, device_send, device_receive, NoNetwork),
            responder.run::<4, 4>(),
            dm.run(),
            resolve_controller,
        )
        .coalesce()
        .await
    };
    let controller = async {
        let active_store = store.borrow().as_ref().unwrap().clone();
        let operational = thread.own_address(5540)?;
        let Address::Udp(SocketAddr::V6(operational)) = operational else {
            unreachable!()
        };
        let peer = if use_ble {
            Address::Btp(BtAddr([0xbb; 6]))
        } else {
            thread.own_address(5540)?
        };
        let (fab, node) = if service_owner {
            assert!(use_ble);
            commission_owned(
                ctrl_matter,
                ctrl_crypto,
                &active_store,
                &ble,
                &relay_generation,
                &phone_connected,
                thread,
                operational,
                service_discovery,
            )
            .await?
        } else {
            test_commission(
                ctrl_matter,
                ctrl_crypto,
                peer,
                operational,
                &thread_network,
                &active_store,
            )
            .await?
        };
        if icd && device_matter.comm_window_state().is_open() {
            let closed = dm.close_comm_window()?;
            eprintln!("ICD fixture closed commissioning window: {closed}");
        }
        println!(
            "PASS PASE and Thread provisioning over {}; CASE and completion over OpenThread",
            if use_ble {
                "synthetic phone GATT/BTP"
            } else {
                "OpenThread"
            }
        );
        let endpoints =
            lexra_rs_matter_probe::devices::discover(ctrl_matter, ctrl_crypto, fab, node).await?;
        assert_eq!(
            endpoints
                .iter()
                .map(|endpoint| endpoint.id)
                .collect::<Vec<_>>(),
            vec![0, 1]
        );
        if icd {
            assert!(endpoints[0].servers.contains(&0x0046));
        }
        assert!(endpoints[1].servers.contains(&6));
        for cluster in [8, 91, 1026, 1029, 1037, 1066] {
            assert!(
                endpoints[1].servers.contains(&cluster),
                "synthetic endpoint missing sensor cluster {cluster}"
            );
        }
        println!("PASS bounded operational descriptor endpoint and cluster discovery over CASE");
        if service_owner {
            let persisted = Identity::open(&active_store, ctrl_crypto, ADMIN_VENDOR_ID)?;
            if sensor_reports {
                // Emulate a crash after the accessory installed its NOC but
                // before the commissioning completion checkpoint was saved.
                let mut record = [0u8; 18 + lexra_rs_matter_probe::catalog::MAX_NODES * 9];
                let size = active_store
                    .get(lexra_rs_matter_probe::catalog::CATALOG_KEY, &mut record)
                    .map_err(|_| rs_matter::error::ErrorCode::Failure)?
                    .ok_or(rs_matter::error::ErrorCode::InvalidData)?;
                if size < 27 || &record[..8] != b"TMNC\x01\0\0\0" {
                    return Err(rs_matter::error::ErrorCode::InvalidData.into());
                }
                let count = usize::from(u16::from_le_bytes([record[16], record[17]]));
                if size != 18 + count * 9 || count > lexra_rs_matter_probe::catalog::MAX_NODES {
                    return Err(rs_matter::error::ErrorCode::InvalidData.into());
                }
                let entry_index = record[18..size]
                    .chunks_exact(9)
                    .position(|entry| u64::from_le_bytes(entry[..8].try_into().unwrap()) == node)
                    .ok_or(rs_matter::error::ErrorCode::InvalidData)?;
                if record[18 + entry_index * 9 + 8]
                    != lexra_rs_matter_probe::catalog::Phase::Complete as u8
                {
                    return Err(rs_matter::error::ErrorCode::InvalidData.into());
                }
                let phase_offset = 18 + entry_index * 9 + 8;
                record[phase_offset] =
                    lexra_rs_matter_probe::catalog::Phase::RecoveryRequired as u8;
                active_store
                    .set(
                        lexra_rs_matter_probe::catalog::CATALOG_KEY,
                        Some(&record[..size]),
                    )
                    .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
            }
            let catalog = lexra_rs_matter_probe::catalog::Catalog::open(
                active_store.clone(),
                persisted.fabric_id(),
                persisted.next_node(),
            )?;
            let mut manager = lexra_rs_matter_probe::devices::Manager::new_with_subscription_limit(
                ctrl_matter,
                &ctrl_crypto,
                fab,
                catalog.clone(),
                if icd {
                    0
                } else if sensor_reports {
                    lexra_rs_matter_probe::devices::MAX_DEVICE_SUBSCRIPTIONS
                } else {
                    1
                },
            )
            .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
            if icd {
                let mode = embassy_time::with_timeout(
                    Duration::from_secs(5),
                    Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node)
                        .await?
                        .icd_management()
                        .operating_mode_read(0),
                )
                .await
                .map_err(|_| rs_matter::error::ErrorCode::RxTimeout)??;
                println!("PASS ICD Management root OperatingMode read over CASE: {mode:?}");
                let features = embassy_time::with_timeout(
                    Duration::from_secs(5),
                    Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node)
                        .await?
                        .icd_management()
                        .feature_map_read(0),
                )
                .await
                .map_err(|_| rs_matter::error::ErrorCode::RxTimeout)??;
                println!("PASS ICD Management FeatureMap read over CASE: {features:#x}");
            }
            let attempts = if icd { 1 } else { 2 };
            for attempt in 0..attempts {
                let snapshot_deadline =
                    std::time::Instant::now() + std::time::Duration::from_secs(30);
                let online = std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    if manager
                        .snapshots()
                        .iter()
                        .any(|s| s.node == node && s.online)
                    {
                        std::task::Poll::Ready(true)
                    } else if std::time::Instant::now() >= snapshot_deadline {
                        std::task::Poll::Ready(false)
                    } else {
                        std::task::Poll::Pending
                    }
                })
                .await;
                if !online {
                    eprintln!("ICD fixture did not make its CASE descriptor snapshot online");
                    return Err(rs_matter::error::ErrorCode::RxTimeout.into());
                }
                assert_eq!(manager.snapshots()[0].endpoints, endpoints);
                if sensor_reports && attempt == 0 {
                    assert!(catalog.nodes().iter().any(|entry| entry.id == node
                        && entry.phase == lexra_rs_matter_probe::catalog::Phase::Complete));
                    println!(
                        "PASS recovery-required catalog node reconciled after live CASE descriptor discovery"
                    );
                }
                if attempt == 0 && !icd {
                    let waker = std::task::Waker::noop();
                    let mut context = std::task::Context::from_waker(waker);
                    manager.poll(false, false, std::time::Instant::now(), &mut context);
                    assert!(!manager.snapshots()[0].online);
                    let body = format!("{{\"schema\":1,\"command\":\"on\",\"node\":\"{node:016x}\",\"endpoint\":1}}");
                    assert!(matches!(
                        manager.prepare(
                            lexra_rs_matter_probe::control::Request::decode(11, body.as_bytes())
                                .unwrap()
                        ),
                        Err(lexra_rs_matter_probe::operations::Failure::NotReady)
                    ));
                }
            }
            if icd {
                println!(
                    "PASS production device manager online snapshot with ICD Management cluster"
                );
            } else {
                println!("PASS production device scheduler catalog restore and radio detach refresh over CASE");
            }
            if icd {
                let registry_store = catalog.store();
                let registration_deadline =
                    std::time::Instant::now() + std::time::Duration::from_secs(12);
                let mut next_registration_check = std::time::Instant::now();
                let registered = std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    if std::time::Instant::now() >= next_registration_check {
                        let registered = lexra_rs_matter_probe::icd::ClientRegistry::open(
                            registry_store.clone(),
                        )
                        .ok()
                        .is_some_and(|registry| registry.registration(fab, node).is_some());
                        if registered {
                            return std::task::Poll::Ready(true);
                        }
                        next_registration_check =
                            std::time::Instant::now() + std::time::Duration::from_millis(100);
                    }
                    if std::time::Instant::now() >= registration_deadline {
                        std::task::Poll::Ready(false)
                    } else {
                        context.waker().wake_by_ref();
                        std::task::Poll::Pending
                    }
                })
                .await;
                if !registered {
                    eprintln!("ICD fixture production manager did not persist RegisterClient");
                    return Err(rs_matter::error::ErrorCode::RxTimeout.into());
                }
                let mode = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node)
                    .await?
                    .icd_management()
                    .operating_mode_read(0)
                    .await?;
                let registrations = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node)
                    .await?
                    .icd_management()
                    .registered_clients_read_with(0, |clients| {
                        clients.map(|clients| clients.iter().count())
                    })
                    .await??;
                eprintln!("ICD fixture accessory mode after RegisterClient response: {mode:?}, registrations={registrations}");
                if mode != rs_matter::dm::clusters::icd_mgmt::OperatingModeEnum::LIT
                    || registrations == 0
                {
                    eprintln!(
                        "ICD fixture accessory did not retain the manager's RegisterClient request"
                    );
                    return Err(rs_matter::error::ErrorCode::InvalidData.into());
                }
                let icd_deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
                let mut next_registry_check = std::time::Instant::now();
                let mut next_icd_diagnostic =
                    std::time::Instant::now() + std::time::Duration::from_secs(5);
                let mut check_in_accepted = false;
                let check_in_accepted = std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    if std::time::Instant::now() >= next_registry_check {
                        check_in_accepted = lexra_rs_matter_probe::icd::ClientRegistry::open(
                            registry_store.clone(),
                        )
                        .ok()
                        .and_then(|registry| {
                            registry
                                .registration(fab, node)
                                .map(|entry| entry.last_offset > 0)
                        })
                        .unwrap_or(false);
                        next_registry_check = std::time::Instant::now()
                            + std::time::Duration::from_millis(100);
                    }
                    if check_in_accepted {
                        std::task::Poll::Ready(true)
                    } else if std::time::Instant::now() >= icd_deadline {
                        std::task::Poll::Ready(false)
                    } else {
                        if std::time::Instant::now() >= next_icd_diagnostic {
                            let offset = lexra_rs_matter_probe::icd::ClientRegistry::open(
                                registry_store.clone(),
                            )
                            .ok()
                            .and_then(|registry| {
                                registry
                                    .registration(fab, node)
                                    .map(|entry| entry.last_offset)
                            });
                            eprintln!("ICD fixture awaiting authenticated Check-In; accepted counter offset={offset:?}");
                            next_icd_diagnostic += std::time::Duration::from_secs(5);
                        }
                        context.waker().wake_by_ref();
                        std::task::Poll::Pending
                    }
                })
                .await;
                if !check_in_accepted {
                    eprintln!("ICD fixture did not accept a Check-In within 25 seconds");
                    return Err(rs_matter::error::ErrorCode::RxTimeout.into());
                }
                eprintln!("ICD fixture authenticated Check-In accepted; dispatching a CASE read");
                println!("PASS production ICD registration and authenticated Check-In counter acceptance");

                let body = format!(
                    "{{\"schema\":1,\"action\":\"read\",\"node\":\"{node:016x}\",\"endpoint\":1,\"cluster\":6,\"attribute\":0}}"
                );
                let accepted = manager
                    .submit(
                        lexra_rs_matter_probe::control::Request::decode(10, body.as_bytes())
                            .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?,
                        true,
                        false,
                        std::time::Instant::now(),
                    )
                    .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?;
                let read_deadline =
                    std::time::Instant::now() + std::time::Duration::from_millis(900);
                let outcome = std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    match manager.job(accepted.job) {
                        Ok(
                            state @ (lexra_rs_matter_probe::devices::JobState::Complete { .. }
                            | lexra_rs_matter_probe::devices::JobState::Failed { .. }),
                        ) => std::task::Poll::Ready(Some(state)),
                        Ok(_) if std::time::Instant::now() < read_deadline => {
                            context.waker().wake_by_ref();
                            std::task::Poll::Pending
                        }
                        Ok(state) => std::task::Poll::Ready(Some(state)),
                        Err(_) => panic!("ICD wake-window job status lookup failed"),
                    }
                })
                .await;
                assert!(matches!(
                    outcome,
                    Some(lexra_rs_matter_probe::devices::JobState::Complete { .. })
                ));
                eprintln!("ICD fixture wake-window CASE read completed");
                println!("PASS queued CASE read completed during the Check-In active window");
                return Ok((fab, node, operational));
            }
            for (cluster, expected, unit) in [
                (1026, 23.15, "celsius"),
                (1029, 52.34, "percent"),
                (1037, 758.5, "ppm"),
                (1066, 12.75, "ug/m3"),
            ] {
                let body = format!("{{\"schema\":1,\"action\":\"read\",\"node\":\"{node:016x}\",\"endpoint\":1,\"cluster\":{cluster},\"attribute\":0}}");
                let accepted = manager
                    .submit(
                        lexra_rs_matter_probe::control::Request::decode(10, body.as_bytes())
                            .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?,
                        true,
                        false,
                        std::time::Instant::now(),
                    )
                    .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?;
                let outcome = std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    match manager.job(accepted.job) {
                        Ok(
                            state @ (lexra_rs_matter_probe::devices::JobState::Complete { .. }
                            | lexra_rs_matter_probe::devices::JobState::Failed { .. }),
                        ) => std::task::Poll::Ready(state),
                        Ok(_) => {
                            context.waker().wake_by_ref();
                            std::task::Poll::Pending
                        }
                        Err(_) => panic!("sensor job status lookup failed"),
                    }
                })
                .await;
                match outcome {
                    lexra_rs_matter_probe::devices::JobState::Complete { result, .. } => {
                        match result.value {
                            lexra_rs_matter_probe::operations::Value::Measurement(value) => {
                                assert_eq!(result.cluster, cluster);
                                assert_eq!(result.attribute, 0);
                                assert_eq!(value.unit, unit);
                                assert_eq!(value.quality, "measured");
                                assert!((value.value.unwrap() - expected).abs() < 0.001);
                            }
                            _ => panic!("sensor cluster {cluster} returned the wrong value type"),
                        }
                    }
                    lexra_rs_matter_probe::devices::JobState::Failed { error, .. } => {
                        panic!("sensor job failed: {error:?}")
                    }
                    _ => unreachable!(),
                }
            }
            let body = format!("{{\"schema\":1,\"action\":\"read\",\"node\":\"{node:016x}\",\"endpoint\":1,\"cluster\":91,\"attribute\":0}}");
            let accepted = manager
                .submit(
                    lexra_rs_matter_probe::control::Request::decode(10, body.as_bytes())
                        .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?,
                    true,
                    false,
                    std::time::Instant::now(),
                )
                .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?;
            let outcome = std::future::poll_fn(|context| {
                manager.poll(true, false, std::time::Instant::now(), context);
                match manager.job(accepted.job) {
                    Ok(
                        state @ (lexra_rs_matter_probe::devices::JobState::Complete { .. }
                        | lexra_rs_matter_probe::devices::JobState::Failed { .. }),
                    ) => std::task::Poll::Ready(state),
                    Ok(_) => {
                        context.waker().wake_by_ref();
                        std::task::Poll::Pending
                    }
                    Err(_) => panic!("air quality job status lookup failed"),
                }
            })
            .await;
            assert!(matches!(outcome,
                lexra_rs_matter_probe::devices::JobState::Complete { result, .. }
                    if result.cluster == 91 && result.attribute == 0 &&
                    matches!(result.value, lexra_rs_matter_probe::operations::Value::AirQuality(ref value)
                        if value.value == "moderate" && value.quality == "measured")));
            println!("PASS capability-gated signed/nullable sensor reads, units and air-quality enum over CASE");
            let first = manager
                .page(0, None)
                .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?;
            assert_eq!(first.devices[0].node.0, node);
            assert_eq!(first.devices[0].endpoint, Some(0));
            assert_eq!(first.next_cursor, Some(1));
            let last = manager
                .page(1, Some(first.revision))
                .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?;
            assert_eq!(last.devices[0].endpoint, Some(1));
            assert!(last.devices[0].online && last.devices[0].servers.contains(&6));
            assert!(last.devices[0].servers.contains(&8));
            assert_eq!(last.next_cursor, None);
            assert_eq!(
                last.devices[0].id.as_deref(),
                Some(format!("matter:{:016x}:{node:016x}:0001", persisted.fabric_id()).as_str())
            );
            println!(
                "PASS bounded revisioned device snapshot pages from encrypted CASE descriptors"
            );
            for (command, expected) in [("on", true), ("toggle", false), ("off", false)] {
                let body = format!("{{\"schema\":1,\"command\":\"{command}\",\"node\":\"{node:016x}\",\"endpoint\":1}}");
                let accepted = manager
                    .submit(
                        lexra_rs_matter_probe::control::Request::decode(11, body.as_bytes())
                            .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?,
                        true,
                        false,
                        std::time::Instant::now(),
                    )
                    .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?;
                let outcome = std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    match manager
                        .job(accepted.job)
                        .map_err(|_| rs_matter::error::ErrorCode::InvalidData)
                    {
                        Ok(
                            state @ (lexra_rs_matter_probe::devices::JobState::Complete { .. }
                            | lexra_rs_matter_probe::devices::JobState::Failed { .. }),
                        ) => std::task::Poll::Ready(Ok(state)),
                        Ok(_) => {
                            context.waker().wake_by_ref();
                            std::task::Poll::Pending
                        }
                        Err(error) => std::task::Poll::Ready(Err(error)),
                    }
                })
                .await?;
                match outcome {
                    lexra_rs_matter_probe::devices::JobState::Complete { result, .. } => {
                        match result.value {
                            lexra_rs_matter_probe::operations::Value::Bool(actual) => {
                                assert_eq!(actual, expected, "OnOff {command}")
                            }
                            _ => panic!("OnOff {command} returned the wrong value type"),
                        }
                    }
                    lexra_rs_matter_probe::devices::JobState::Failed { error, .. } => {
                        panic!("OnOff job failed: {error:?}");
                    }
                    _ => unreachable!(),
                }
            }
            for (body, opcode, expected) in [
                (format!("{{\"schema\":1,\"action\":\"read\",\"node\":\"{node:016x}\",\"endpoint\":1,\"cluster\":8,\"attribute\":0}}"), 10, 1),
                (format!("{{\"schema\":1,\"command\":\"move_to_level\",\"node\":\"{node:016x}\",\"endpoint\":1,\"level\":124,\"transition_deciseconds\":0}}"), 11, 124),
                (format!("{{\"schema\":1,\"action\":\"read\",\"node\":\"{node:016x}\",\"endpoint\":1,\"cluster\":8,\"attribute\":0}}"), 10, 124),
            ] {
                let accepted = manager
                    .submit(
                        lexra_rs_matter_probe::control::Request::decode(opcode, body.as_bytes())
                            .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?,
                        true,
                        false,
                        std::time::Instant::now(),
                    )
                    .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?;
                let outcome = std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    match manager.job(accepted.job) {
                        Ok(state @ (lexra_rs_matter_probe::devices::JobState::Complete { .. }
                            | lexra_rs_matter_probe::devices::JobState::Failed { .. })) => {
                            std::task::Poll::Ready(state)
                        }
                        Ok(_) => {
                            context.waker().wake_by_ref();
                            std::task::Poll::Pending
                        }
                        Err(_) => panic!("Level job status lookup failed"),
                    }
                })
                .await;
                assert!(matches!(outcome,
                    lexra_rs_matter_probe::devices::JobState::Complete { result, .. }
                        if result.cluster == 8 && result.attribute == 0 &&
                            matches!(result.value, lexra_rs_matter_probe::operations::Value::U8(Some(actual)) if actual == expected)));
            }
            println!("PASS capability-gated Level reads, MoveToLevel and confirmed CASE readback");
            let denied_level_read = format!(
                "{{\"schema\":1,\"action\":\"read\",\"node\":\"{node:016x}\",\"endpoint\":0,\"cluster\":8,\"attribute\":0}}"
            );
            assert!(matches!(
                manager.prepare(
                    lexra_rs_matter_probe::control::Request::decode(
                        10,
                        denied_level_read.as_bytes()
                    )
                    .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?
                ),
                Err(lexra_rs_matter_probe::operations::Failure::Unsupported)
            ));
            let denied = format!(
                "{{\"schema\":1,\"command\":\"on\",\"node\":\"{node:016x}\",\"endpoint\":0}}"
            );
            assert!(matches!(
                manager.prepare(
                    lexra_rs_matter_probe::control::Request::decode(11, denied.as_bytes())
                        .map_err(|_| rs_matter::error::ErrorCode::InvalidData)?
                ),
                Err(lexra_rs_matter_probe::operations::Failure::Unsupported)
            ));
            println!("PASS capability-gated OnOff transactions and confirmed readback over CASE");
            let subscription_id = embassy_time::with_timeout(
                Duration::from_secs(35),
                std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    if let Some(id) = manager.subscription_id(node, 1, 6, 0) {
                        std::task::Poll::Ready(id)
                    } else {
                        context.waker().wake_by_ref();
                        std::task::Poll::Pending
                    }
                }),
            )
            .await
            .map_err(|_| {
                eprintln!("timed out waiting for the OnOff report subscription ID");
                rs_matter::error::ErrorCode::RxTimeout
            })?;
            assert!(subscription_id != 0);
            // An obsolete subscription must be rejected on the wire, even
            // when its ReportData contains no changed attribute values.
            let invalid_report = async {
                use rs_matter::tlv::FromTLV;
                let mut exchange = Exchange::initiate(
                    device_matter, device_crypto, fab, active_store_identity_node(ctrl_matter, fab)?,
                ).await?;
                exchange.send_with(|_, writer| {
                    writer.start_struct(&TLVTag::Anonymous)?;
                    writer.u32(&TLVTag::Context(0), subscription_id.wrapping_add(0x80000000))?;
                    writer.u8(&TLVTag::Context(rs_matter::im::IM_REVISION_TAG), rs_matter::im::IM_REVISION)?;
                    writer.end_container()?;
                    Ok(Some(rs_matter::im::OpCode::ReportData.into()))
                }).await?;
                let rx = exchange.recv_fetch().await?;
                let response = rs_matter::im::StatusResp::from_tlv(
                    &rs_matter::tlv::TLVElement::new(rx.payload()),
                )?;
                assert_eq!(response.status, rs_matter::im::IMStatusCode::InvalidSubscription);
                Ok::<_, Error>(())
            };
            let report_pump = std::future::poll_fn(|context| {
                manager.poll(true, false, std::time::Instant::now(), context);
                std::task::Poll::<()>::Pending
            });
            match embassy_time::with_timeout(Duration::from_secs(5), select(invalid_report, report_pump)).await
                .map_err(|_| rs_matter::error::ErrorCode::RxTimeout)? {
                Either::First(result) => result?,
                Either::Second(()) => unreachable!(),
            }
            println!("PASS obsolete subscription rejected with INVALID_SUBSCRIPTION over CASE");
            if sensor_reports {
                wait_manager_subscriptions(
                    &mut manager,
                    node,
                    &[6, 8, 69, 91, 1026, 1029, 1037, 1066],
                )
                .await?;
                let priming_values: Vec<_> =
                    std::iter::from_fn(|| manager.pop_report()).collect();
                assert!(priming_values.iter().any(|value| matches!(
                    value,
                    lexra_rs_matter_probe::operations::ResultValue {
                        node: lexra_rs_matter_probe::control::Id(reported_node),
                        endpoint: 1,
                        cluster: 6,
                        attribute: 0,
                        value: lexra_rs_matter_probe::operations::Value::Bool(false),
                    } if *reported_node == node
                )));
                assert!(priming_values.iter().any(|value| matches!(
                    value,
                    lexra_rs_matter_probe::operations::ResultValue {
                        node: lexra_rs_matter_probe::control::Id(reported_node),
                        endpoint: 1,
                        cluster: 69,
                        attribute: 0,
                        value: lexra_rs_matter_probe::operations::Value::Bool(false),
                    } if *reported_node == node
                )));
                for (cluster, expected) in [(1026, 23.15), (1029, 52.34)] {
                    assert!(priming_values.iter().any(|value| {
                        value.node.0 == node
                            && value.endpoint == 1
                            && value.cluster == cluster
                            && matches!(
                                &value.value,
                                lexra_rs_matter_probe::operations::Value::Measurement(measurement)
                                    if measurement.value.is_some_and(|value| (value - expected).abs() < 0.001)
                            )
                    }), "missing initial sensor value for cluster {cluster}");
                }
                println!("PASS initial sensor and contact values delivered from subscription priming reports");
            }
            let mut report_ipc = if sensor_reports {
                let directory = if state_directory.is_absolute() {
                    state_directory.clone()
                } else {
                    std::env::current_dir()
                        .map_err(|_| rs_matter::error::ErrorCode::Failure)?
                        .join(&state_directory)
                };
                let path = std::env::var_os("MICROHAD_MATTER_TEST_IPC_SOCKET")
                    .map(std::path::PathBuf::from)
                    .unwrap_or_else(|| directory.join("report-ipc.sock"));
                let mut ipc = lexra_rs_matter_probe::service::IpcServer::bind(&path)
                    .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
                ipc.poll(|_| None)
                    .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
                Some((ipc, path))
            } else {
                None
            };
            let exchange = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node).await?;
            let toggle = exchange.on_off().toggle(1);
            let report_pump = std::future::poll_fn(|context| {
                manager.poll(true, false, std::time::Instant::now(), context);
                std::task::Poll::<()>::Pending
            });
            match select(toggle, report_pump).await {
                Either::First(result) => result?,
                Either::Second(()) => unreachable!(),
            }
            let reported_on = embassy_time::with_timeout(
                Duration::from_secs(5),
                std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    if let Some(report) = manager.pop_report() {
                        std::task::Poll::Ready(report)
                    } else {
                        context.waker().wake_by_ref();
                        std::task::Poll::Pending
                    }
                }),
            )
            .await
            .map_err(|_| {
                eprintln!("timed out waiting for first OnOff subscription report");
                rs_matter::error::ErrorCode::RxTimeout
            })?;
            assert!(matches!(
                reported_on,
                lexra_rs_matter_probe::operations::ResultValue {
                    node: lexra_rs_matter_probe::control::Id(reported_node),
                    endpoint: 1,
                    cluster: 6,
                    attribute: 0,
                    value: lexra_rs_matter_probe::operations::Value::Bool(true),
                } if reported_node == node
            ));
            let exchange = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node).await?;
            let toggle = exchange.on_off().toggle(1);
            let report_pump = std::future::poll_fn(|context| {
                manager.poll(true, false, std::time::Instant::now(), context);
                std::task::Poll::<()>::Pending
            });
            match select(toggle, report_pump).await {
                Either::First(result) => result?,
                Either::Second(()) => unreachable!(),
            }
            let reported_off = embassy_time::with_timeout(
                Duration::from_secs(5),
                std::future::poll_fn(|context| {
                    manager.poll(true, false, std::time::Instant::now(), context);
                    if let Some(report) = manager.pop_report() {
                        std::task::Poll::Ready(report)
                    } else {
                        context.waker().wake_by_ref();
                        std::task::Poll::Pending
                    }
                }),
            )
            .await
            .map_err(|_| {
                eprintln!("timed out waiting for second OnOff subscription report");
                rs_matter::error::ErrorCode::RxTimeout
            })?;
            assert!(matches!(
                reported_off,
                lexra_rs_matter_probe::operations::ResultValue {
                    node: lexra_rs_matter_probe::control::Id(reported_node),
                    endpoint: 1,
                    cluster: 6,
                    attribute: 0,
                    value: lexra_rs_matter_probe::operations::Value::Bool(false),
                } if reported_node == node
            ));
            if sensor_reports {
                if native_ipc {
                    let ready_path = std::env::var_os("MICROHAD_MATTER_TEST_NATIVE_READY_FILE")
                        .map(std::path::PathBuf::from)
                        .ok_or(rs_matter::error::ErrorCode::InvalidData)?;
                    let release_path = std::env::var_os("MICROHAD_MATTER_TEST_NATIVE_RELEASE_FILE")
                        .map(std::path::PathBuf::from)
                        .ok_or(rs_matter::error::ErrorCode::InvalidData)?;
                    std::fs::write(ready_path, b"ipc-ready")
                        .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
                    let (ipc, _) = report_ipc.as_mut().unwrap();
                    while !release_path.exists() {
                        std::future::poll_fn(|context| {
                            manager.poll(true, false, std::time::Instant::now(), context);
                            ipc.poll_with_peer(|_, request| {
                                native_test_response(request, &mut manager)
                            })
                            .unwrap();
                            std::task::Poll::Ready(())
                        })
                        .await;
                        Timer::after_millis(5).await;
                    }
                }
                for cluster in [1026, 1029, 91, 1037, 1066, 69] {
                    match cluster {
                        69 => {
                            boolean_state.value.set(true);
                            boolean_state::ClusterHandler::dataver_changed(&boolean_state);
                        }
                        1026 => temperature.value.set(Some(-525)),
                        1029 => humidity.value.set(Some(6400)),
                        91 => {
                            air_quality.value.set(air_quality::AirQualityEnum::Fair);
                            air_quality::ClusterHandler::dataver_changed(&air_quality);
                        }
                        1037 => co2.value.set(Some(800.0)),
                        1066 => pm25.value.set(Some(25.5)),
                        _ => unreachable!(),
                    }
                    dm.notify_attr_changed(1, cluster, 0);
                    let mut report_peer = if native_ipc {
                        None
                    } else if let Some((ipc, path)) = &mut report_ipc {
                        let peer = UnixStream::connect(path)
                            .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
                        ipc.poll(|_| None)
                            .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
                        Some(peer)
                    } else {
                        None
                    };
                    let payload = embassy_time::with_timeout(
                        Duration::from_secs(5),
                        std::future::poll_fn(|context| {
                            if let Some((ipc, _)) = &mut report_ipc {
                                if native_ipc {
                                    ipc.poll_with_peer(|_, request| {
                                        native_test_response(request, &mut manager)
                                    })
                                    .unwrap();
                                }
                            }
                            manager.poll(true, false, std::time::Instant::now(), context);
                            if native_ipc {
                                if let Some((ipc, _)) = &mut report_ipc {
                                    if manager.front_report().is_some()
                                        && manager.publish_reports(ipc).unwrap() != 0
                                    {
                                        ipc.poll_with_peer(|_, request| {
                                            native_test_response(request, &mut manager)
                                        })
                                        .unwrap();
                                        return std::task::Poll::Ready(Vec::new());
                                    }
                                }
                            } else if let (Some((ipc, _)), Some(peer)) =
                                (&mut report_ipc, &mut report_peer)
                            {
                                if manager.front_report().is_some() {
                                    if manager.publish_reports(ipc).unwrap() != 0 {
                                        ipc.poll(|_| None).unwrap();
                                        let mut header = [0; 10];
                                        peer.read_exact(&mut header).unwrap();
                                        let length =
                                            usize::from(u16::from_le_bytes([header[0], header[1]]));
                                        let mut bytes = header.to_vec();
                                        bytes.resize(10 + length, 0);
                                        peer.read_exact(&mut bytes[10..]).unwrap();
                                        let mut decoder =
                                            lexra_rs_matter_probe::ipc::Decoder::default();
                                        decoder.feed(&bytes).unwrap();
                                        let event = decoder.pop().unwrap();
                                        assert_eq!(
                                            (event.kind, event.opcode, event.id),
                                            (2, 12, 0)
                                        );
                                        return std::task::Poll::Ready(event.payload);
                                    }
                                }
                            } else {
                                while let Some(report) = manager.pop_report() {
                                    if report.cluster == cluster {
                                        return std::task::Poll::Ready(Vec::new());
                                    }
                                }
                            }
                            context.waker().wake_by_ref();
                            std::task::Poll::Pending
                        }),
                    )
                    .await
                    .map_err(|_| {
                        eprintln!(
                            "timed out waiting for CASE ReportData on sensor cluster {cluster}"
                        );
                        rs_matter::error::ErrorCode::RxTimeout
                    })?;
                    drop(report_peer);
                    let expected = match cluster {
                        1026 => br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1026,"attribute":0,"value":{"type":"measurement","value":{"value":-5.25,"unit":"celsius","quality":"measured"}}}}"#.as_slice(),
                        1029 => br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1029,"attribute":0,"value":{"type":"measurement","value":{"value":64.0,"unit":"percent","quality":"measured"}}}}"#.as_slice(),
                        91 => br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":91,"attribute":0,"value":{"type":"air_quality","value":{"value":"fair","quality":"measured"}}}}"#.as_slice(),
                        1037 => br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1037,"attribute":0,"value":{"type":"measurement","value":{"value":800.0,"unit":"ppm","quality":"measured"}}}}"#.as_slice(),
                        1066 => br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1066,"attribute":0,"value":{"type":"measurement","value":{"value":25.5,"unit":"ug/m3","quality":"measured"}}}}"#.as_slice(),
                        69 => br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":69,"attribute":0,"value":{"type":"bool","value":true}}}"#.as_slice(),
                        _ => unreachable!(),
                    };
                    if native_ipc {
                        assert!(payload.is_empty());
                    } else {
                        assert_eq!(payload, expected);
                    }
                }
                println!(
                    "PASS production manager sensor and contact reports reach IPC from subscribed CASE reports"
                );
                if native_ipc {
                    if let Some(path) =
                        std::env::var_os("MICROHAD_MATTER_TEST_NATIVE_REPORTS_SENT_FILE")
                    {
                        std::fs::write(path, b"six CASE reports published")
                            .map_err(|_| rs_matter::error::ErrorCode::Failure)?;
                    }
                    if let Some(path) = std::env::var_os("MICROHAD_MATTER_TEST_NATIVE_REPORTS_ACK_FILE")
                    {
                        let (ipc, _) = report_ipc.as_mut().unwrap();
                        while !std::path::Path::new(&path).exists() {
                            std::future::poll_fn(|context| {
                                manager.poll(true, false, std::time::Instant::now(), context);
                                ipc.poll_with_peer(|_, request| {
                                    native_test_response(request, &mut manager)
                                })
                                .unwrap();
                                std::task::Poll::Ready(())
                            })
                            .await;
                            Timer::after_millis(5).await;
                        }
                    }
                }
            }
            println!("PASS production manager subscription and peer-correlated OnOff reports");
        }
        if sensor_reports || icd {
            return Ok((fab, node, operational));
        }
        let exchange = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node).await?;
        assert!(!exchange.on_off().on_off_read(1).await?);
        let exchange = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node).await?;
        exchange.on_off().toggle(1).await?;
        let exchange = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node).await?;
        assert!(exchange.on_off().on_off_read(1).await?);
        println!("PASS CASE OnOff read, toggle and readback over OpenThread");
        for expected in [Some(2315), Some(-525), None] {
            temperature.value.set(expected);
            temperature.dataver_changed();
            let exchange = Exchange::initiate(ctrl_matter, ctrl_crypto, fab, node).await?;
            let actual = exchange
                .temperature_measurement()
                .measured_value_read(1)
                .await?;
            assert_eq!(actual.as_opt_ref().copied(), expected);
        }
        println!("PASS CASE signed and unknown temperature values over OpenThread");
        Ok::<_, Error>((fab, node, operational))
    };
    let controller = async {
        let (fab, node, operational) = match select3(
            ctrl_matter.run(ctrl_crypto, controller_send, controller_receive, NoNetwork),
            controller_responder.run(),
            controller,
        )
        .await
        {
            embassy_futures::select::Either3::First(result) => {
                result?;
                return Err(rs_matter::error::ErrorCode::InvalidState.into());
            }
            embassy_futures::select::Either3::Second(result) => {
                result?;
                return Err(rs_matter::error::ErrorCode::InvalidState.into());
            }
            embassy_futures::select::Either3::Third(result) => result?,
        };
        if sensor_reports || icd {
            return Ok(());
        }
        // Replace the controller stack, including all sessions, with a fresh instance.
        // The accessory and OpenThread instance keep running; the phone is detached.
        phone_connected.set(false);
        if !service_owner {
            ble.disconnect(generation)?;
        }
        let restarted = RESTART_MATTER.uninit().init_with(Matter::init(
            &TEST_DEV_DET,
            TEST_DEV_COMM,
            &TEST_DEV_ATT,
            0,
        ));
        drop(store.borrow_mut().take());
        let reopened =
            PrivateStore::open(&state_directory).expect("reopen committed fixture state from disk");
        let mut identity = Identity::open(&reopened, ctrl_crypto, ADMIN_VENDOR_ID)?;
        let kv = restarted.kv(reopened.clone());
        restarted.startup(&kv)?;
        assert_eq!(identity.install(restarted, ctrl_crypto, &reopened)?, fab);
        assert!(identity.allocate_node(&reopened)? > node);
        assert!(restarted.has_fabrics());
        println!("PASS persisted production authority reload and monotonic node allocation");
        let recovered = async {
            let exchange = Exchange::initiate(restarted, ctrl_crypto, fab, node).await?;
            assert!(exchange.on_off().on_off_read(1).await?);
            println!(
                "PASS committed flash fabric reopen, fresh CASE and OnOff readback with phone detached"
            );
            Ok::<_, Error>(())
        };
        match select3(
            restarted.run(ctrl_crypto, &ctrl_net, &ctrl_net, NoNetwork),
            recovered,
            resolve_for_probe(restarted, thread, node, operational, service_discovery),
        )
        .await
        {
            embassy_futures::select::Either3::First(result) => {
                result?;
                Err(rs_matter::error::ErrorCode::InvalidState.into())
            }
            embassy_futures::select::Either3::Second(result) => result,
            embassy_futures::select::Either3::Third(_) => unreachable!(),
        }
    };
    let peers = async {
        match select(device, controller).await {
            Either::First(result) => {
                result?;
                Err(rs_matter::error::ErrorCode::InvalidState.into())
            }
            Either::Second(result) => result,
        }
    };
    match select(peers, gatt).await {
        Either::First(result) => result,
        Either::Second(result) => {
            result?;
            Err(rs_matter::error::ErrorCode::InvalidState.into())
        }
    }
}

async fn run_commissioner<C: Crypto>(
    matter: &Matter<'_>,
    commissioner: &mut Commissioner<'_, '_, C>,
    peer: Address,
    operational: SocketAddrV6,
    crypto: &C,
    network: &TestThreadNetwork,
    node_id: u64,
) -> Result<CommissionResult, Error> {
    let result = commissioner
        .commission(
            peer,
            TEST_PASSCODE,
            &gateway_commissioning::CommissioningPolicy::default().options(),
            node_id,
            VALID_FOREVER,
        )
        .await?;
    let bytes = dataset();
    let parsed = gateway_commissioning::ThreadDataset::parse(&bytes, 15)?;
    gateway_commissioning::provision_thread(matter, crypto, peer, TEST_PASSCODE, &parsed).await?;
    assert_eq!(*network.dataset.borrow(), bytes);
    assert_eq!(network.connect_calls.get(), 1);
    let sock = operational;
    match select(
        commissioner.complete_via_case(&result),
        resolve_fixture(matter, result.device_node_id, sock),
    )
    .await
    {
        Either::First(result) => result?,
        Either::Second(_) => unreachable!(),
    }
    Ok(result)
}

// Fixture-only rendezvous; external multicast discovery is a separate gate.
// Node zero selects this run's single synthetic accessory for the owned-task test.
async fn resolve_fixture(matter: &Matter<'_>, node: u64, sock: SocketAddrV6) -> ! {
    loop {
        let service = matter.transport().wait_mdns_resolve_request().await;
        if !matches!(service, MatterRemoteService::Operational { node_id, .. } if node == 0 || node_id == node)
        {
            continue;
        }
        let mut name = heapless::String::<128>::new();
        service.instance_name(&mut name);
        matter.transport().try_deposit_mdns_resolve(
            &MdnsRemoteService {
                instance_name: DottedName(name.as_str()),
                port: Some(sock.port()),
                addrs: core::iter::once(IpAddr::V6(*sock.ip())),
                txt: core::iter::empty(),
                scope_id: 0,
            },
            &[],
        );
    }
}
// Exercise the production owner/future against the encrypted synthetic accessory.
// Only this probe provides fixture rendezvous; the service never injects addresses.
async fn commission_owned<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    store: &PrivateStore,
    ble: &BleRelay,
    generation: &Cell<u64>,
    phone_connected: &Cell<bool>,
    thread: &OpenThread,
    operational: SocketAddrV6,
    service_discovery: bool,
) -> Result<(core::num::NonZeroU8, u64), Error> {
    use lexra_rs_matter_probe::{
        control::Request, controller::Controller, entropy::OsEntropy, thread_lifecycle::Lifecycle,
    };
    let identity = Identity::open(store, crypto, ADMIN_VENDOR_ID)?;
    let fabric = identity.install(matter, crypto, store)?;
    let mut lifecycle =
        Lifecycle::load(store.clone()).map_err(|_| rs_matter::error::ErrorCode::InvalidState)?;
    let now = std::time::Instant::now();
    lifecycle
        .observe(15, true, Some(thread), now)
        .map_err(|_| rs_matter::error::ErrorCode::InvalidState)?;
    lifecycle
        .import(&dataset(), 15, thread, now)
        .map_err(|_| rs_matter::error::ErrorCode::InvalidState)?;
    if service_discovery {
        thread.start_discovery()?;
        let compressed = matter.with_state(|state| {
            Ok::<_, Error>(state.fabrics.fabric(fabric)?.compressed_fabric_id())
        })?;
        let label =
            std::ffi::CString::new(format!("{compressed:016X}-{:016X}", identity.next_node()))
                .unwrap();
        unsafe {
            assert_eq!(
                gatewayOtDiscoveryTestRegister(
                    label.as_ptr(),
                    operational.ip().octets().as_ptr(),
                    operational.port()
                ),
                0
            );
        }
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(45);
        while !unsafe { gatewayOtDiscoveryTestRegistered() } {
            assert!(
                std::time::Instant::now() < deadline,
                "Synthetic SRP registration deadline"
            );
            Timer::after(Duration::from_millis(10)).await;
        }
        // Drop a submitted query before pumping its callback. The adapter retains
        // only its generation cookie; subsequent callbacks cannot touch Rust memory.
        {
            let mut abandoned = pin!(thread.resolve_service(label.to_str().unwrap()));
            let wake = std::task::Waker::from(std::sync::Arc::new(NoopWake));
            let mut context = std::task::Context::from_waker(&wake);
            assert!(std::future::Future::poll(abandoned.as_mut(), &mut context).is_pending());
        }
        Timer::after(Duration::from_millis(50)).await;
        let reply = thread.resolve_service(label.to_str().unwrap()).await?;
        assert_eq!(reply.port, operational.port());
        assert!(reply.ttl > 0 && reply.count > 0);
        assert!(reply.addresses[..usize::from(reply.count)].contains(&operational.ip().octets()));
        let missing = format!("{compressed:016X}-{:016X}", identity.next_node() + 100);
        assert!(thread.resolve_service(&missing).await.is_err());
        assert!(thread
            .resolve_service(label.to_str().unwrap())
            .await
            .is_ok());
        let browsed = thread
            .browse_operational_nodes(compressed, identity.next_node() + 1)
            .await?;
        assert!(browsed.contains(&identity.next_node()));
        println!("PASS DNS cancelled callback isolation, missing service and subsequent valid SRV/TXT/AAAA");
        println!("PASS synthetic accessory SRP registration, operational DNS browse and resolve through userspace OpenThread");
    }
    let now = std::time::Instant::now();
    lifecycle
        .observe(15, true, Some(thread), now)
        .map_err(|_| rs_matter::error::ErrorCode::InvalidState)?;
    let mut owner = Controller::new(ble, matter, crypto, identity, store.clone(), fabric)?;
    let mut entropy = OsEntropy::open().map_err(|_| rs_matter::error::ErrorCode::InvalidState)?;
    let start = Request::decode(
        3,
        br#"{"schema":1,"discriminator":3840,"pin":20202021,"timeout_seconds":120,"country_code":"DE","regulatory_location":"indoor"}"#,
    )
    .unwrap();
    let reply = owner
        .handle(start, 1, &lifecycle, &mut entropy, now)
        .unwrap();
    let reply: serde_json::Value = serde_json::from_slice(&reply).unwrap();
    assert_eq!(reply["ok"], true);
    let session = reply["result"]["session"].as_str().unwrap();
    let attach =
        format!(r#"{{"schema":1,"session":"{session}","address":"bbbbbbbbbbbb","mtu":247}}"#);
    let reply = owner
        .handle(
            Request::decode(6, attach.as_bytes()).unwrap(),
            1,
            &lifecycle,
            &mut entropy,
            now,
        )
        .unwrap();
    let reply: serde_json::Value = serde_json::from_slice(&reply).unwrap();
    assert_eq!(reply["ok"], true);
    generation.set(reply["result"]["generation"].as_u64().unwrap());
    let status = format!(r#"{{"schema":1,"session":"{session}"}}"#);
    let work = async {
        let waker = std::task::Waker::from(std::sync::Arc::new(NoopWake));
        let mut context = std::task::Context::from_waker(&waker);
        loop {
            let now = std::time::Instant::now();
            lifecycle
                .observe(15, true, Some(thread), now)
                .map_err(|_| rs_matter::error::ErrorCode::InvalidState)?;
            owner.poll(&lifecycle, now, &mut context);
            let reply = owner
                .handle(
                    Request::decode(4, status.as_bytes()).unwrap(),
                    1,
                    &lifecycle,
                    &mut entropy,
                    now,
                )
                .unwrap();
            let reply: serde_json::Value = serde_json::from_slice(&reply).unwrap();
            assert_eq!(reply["ok"], true);
            let state = reply["result"]["state"].as_str().unwrap();
            if state == "complete" {
                phone_connected.set(false);
                let node =
                    u64::from_str_radix(reply["result"]["node"].as_str().unwrap(), 16).unwrap();
                let persisted = Identity::open(store, crypto, ADMIN_VENDOR_ID)?;
                let catalog = lexra_rs_matter_probe::catalog::Catalog::open(
                    store.clone(),
                    persisted.fabric_id(),
                    persisted.next_node(),
                )?;
                assert!(catalog.nodes().iter().any(|entry| entry.id == node
                    && entry.phase == lexra_rs_matter_probe::catalog::Phase::Complete));
                assert_eq!(
                    catalog.endpoint_id(node, 1)?,
                    format!("matter:{:016x}:{node:016x}:0001", persisted.fabric_id())
                );
                println!("PASS persisted production node catalog and stable endpoint identity");
                println!(
                    "PASS production owned commissioner PASE, provisioning and CASE completion"
                );
                return Ok::<_, Error>((fabric, node));
            }
            if matches!(state, "failed" | "timed_out" | "cancelled") {
                return Err(rs_matter::error::ErrorCode::InvalidState.into());
            }
            Timer::after(Duration::from_millis(1)).await;
        }
    };
    match select(
        work,
        resolve_for_probe(matter, thread, 0, operational, service_discovery),
    )
    .await
    {
        Either::First(result) => result,
        Either::Second(_) => unreachable!(),
    }
}

const ADMIN_VENDOR_ID: u16 = 0xFFF1;

// Use exactly the authority and allocator installed by the production service.
// The accessory, network and rendezvous remain explicitly synthetic.
async fn test_commission<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    peer_addr: Address,
    operational: SocketAddrV6,
    thread_network: &TestThreadNetwork,
    store: &PrivateStore,
) -> Result<(core::num::NonZeroU8, u64), Error> {
    let mut identity = Identity::open(store, crypto, ADMIN_VENDOR_ID)?;
    let controller_fab_idx = identity.install(matter, crypto, store)?;
    let device_node_id = identity.allocate_node(store)?;
    let mut noc_buf = [0u8; MAX_CERT_TLV_AND_ASN1_LEN];
    let mut noc_generator = identity.signer(&mut noc_buf)?;
    let mut commissioner_buf = [0u8; MAX_CERT_TLV_LEN];
    let mut commissioner = Commissioner::new(
        matter,
        crypto,
        controller_fab_idx,
        &mut noc_generator,
        &mut commissioner_buf,
    );
    let result = run_commissioner(
        matter,
        &mut commissioner,
        peer_addr,
        operational,
        &crypto,
        thread_network,
        device_node_id,
    )
    .await?;
    Ok((controller_fab_idx, result.device_node_id))
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|arg| arg == "--icd-lookup-profile") {
        let directory = std::env::var_os("MICROHAD_MATTER_TEST_STATE_DIR")
            .map(std::path::PathBuf::from)
            .expect("Set a unique disposable MICROHAD_MATTER_TEST_STATE_DIR");
        assert!(!directory.exists(), "ICD profile state path already exists");
        let crypto = default_crypto(
            lexra_rs_matter_probe::entropy::OsEntropy::open().expect("OS entropy"),
            CryptoSensitiveRef::new(&[0x61; 32]),
        );
        let trials = 16;
        let elapsed = lexra_rs_matter_probe::icd::profile_unknown_identity_lookup(
            &crypto, &directory, trials,
        )
        .expect("64-key unknown-identity ICD profile");
        println!(
            "PASS ICD unknown-identity lookup clients={} trials={} checks={} elapsed_us={} per_packet_us={}",
            lexra_rs_matter_probe::icd::MAX_ICD_CLIENTS,
            trials,
            lexra_rs_matter_probe::icd::MAX_ICD_CLIENTS * trials,
            elapsed.as_micros(),
            elapsed.as_micros() / trials as u128,
        );
        return;
    }
    let url = args
        .first()
        .expect("Pass an isolated synthetic Unix radio URL");
    // Real UART URLs and missing reset guard must be rejected before initialization.
    assert!(OpenThread::init("spinel+hdlc+uart:///dev/ttyS1?no-reset").is_err());
    assert!(OpenThread::init("spinel+hdlc+unix:///tmp/missing").is_err());
    let thread = OpenThread::init(&url).expect("OpenThread initialization");
    thread
        .set_dataset(&dataset(), 15)
        .expect("public fixture dataset");
    let use_ble = std::env::args().any(|arg| arg == "--ble");
    let service_owner = std::env::args().any(|arg| arg == "--service-owner");
    let service_discovery = std::env::args().any(|arg| arg == "--service-discovery");
    let sensor_reports = std::env::args().any(|arg| arg == "--sensor-reports");
    let native_ipc = std::env::args().any(|arg| arg == "--native-ipc");
    let icd = std::env::args().any(|arg| arg == "--icd");
    assert!(!service_owner || use_ble);
    assert!(!service_discovery || service_owner);
    assert!(!sensor_reports || service_owner);
    assert!(!native_ipc || (sensor_reports && service_discovery));
    assert!(!icd || service_owner);
    let work = async {
        run(
            &thread,
            use_ble,
            service_owner,
            service_discovery,
            sensor_reports,
            native_ipc,
            icd,
        )
        .await
        .expect("encrypted Matter fixture");
    };
    let pump = async {
        loop {
            thread.poll(0).unwrap();
            Timer::after(Duration::from_millis(5)).await;
        }
    };
    // Tiny local executor: poll the async work and timer futures while pumping OpenThread.
    // Existing probe has no desktop network/runtime dependency.
    let wake = std::sync::Arc::new(NoopWake);
    let waker = std::task::Waker::from(wake);
    let mut context = std::task::Context::from_waker(&waker);
    let mut work = pin!(work);
    let mut pump = pin!(pump);
    let started = std::time::Instant::now();
    loop {
        use std::future::Future;
        if work.as_mut().poll(&mut context).is_ready() {
            break;
        }
        let _ = pump.as_mut().poll(&mut context);
        assert!(
            started.elapsed().as_secs() < if icd { 150 } else { 90 },
            "transport smoke test timeout"
        );
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    println!(
        "PASS commissioning complete elapsed_ms={}",
        started.elapsed().as_millis()
    );
}
struct NoopWake;
impl std::task::Wake for NoopWake {
    fn wake(self: std::sync::Arc<Self>) {}
}

unsafe extern "C" {
    fn gatewayOtDiscoveryTestRegister(name: *const core::ffi::c_char, ip: *const u8, port: u16)
        -> i32;
    fn gatewayOtDiscoveryTestRegistered() -> bool;
}
async fn resolve_for_probe(
    matter: &Matter<'_>,
    thread: &OpenThread,
    node: u64,
    sock: SocketAddrV6,
    discovery: bool,
) -> ! {
    if discovery {
        lexra_rs_matter_probe::discovery::run(matter, thread)
            .await
            .expect("production operational resolver");
        unreachable!()
    } else {
        resolve_fixture(matter, node, sock).await
    }
}
