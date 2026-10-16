//! Gateway controller process. Owns OpenThread, Matter and IPC on a single thread.
use core::pin::pin;
use micro_thread_matter_daemon::{
    ble::BleRelay,
    catalog::Catalog,
    control::{Request, ThreadRequest},
    controller::Controller,
    entropy::OsEntropy,
    identity::Identity,
    ipc::{Frame, OP_MATTER_STATUS, OP_THREAD_STATUS},
    openthread::OpenThread,
    service::IcdCheckInResponder,
    service::IpcServer,
    store::PrivateStore,
    thread_lifecycle::{Failure, Lifecycle, Radio},
};
use rs_matter::crypto::{
    default_crypto, CanonPkcPublicKeyRef, CanonPkcSecretKeyRef, Crypto, CryptoSensitiveRef, TryRng,
};
use rs_matter::dm::clusters::{basic_info::BasicInfoConfig, dev_att::DeviceAttestation};
use rs_matter::sc::pase::{Spake2pVerifierPassword, Spake2pVerifierPasswordRef};
use rs_matter::transport::network::{Address, ChainedNetwork, NoNetwork};
use rs_matter::utils::{init::InitMaybeUninit, sync::DynBase};
use rs_matter::{BasicCommData, Matter, MATTER_PORT};
use static_cell::StaticCell;
use std::future::Future;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

static STOP: AtomicBool = AtomicBool::new(false);
static MATTER: StaticCell<Matter> = StaticCell::new();
static DETAILS: BasicInfoConfig = BasicInfoConfig {
    product_name: "Gateway Matter controller",
    device_name: "Gateway controller",
    ..BasicInfoConfig::new()
};
// A commissioner is not an accessory and has no manufacturer attestation
// credentials or open commissioning window. No device responder is installed.
struct ControllerAttestation;
impl DynBase for ControllerAttestation {}
impl DeviceAttestation for ControllerAttestation {
    fn cert_declaration(&self) -> &[u8] {
        &[]
    }
    fn pai(&self) -> &[u8] {
        &[]
    }
    fn dac(&self) -> &[u8] {
        &[]
    }
    fn dac_pub_key(&self) -> CanonPkcPublicKeyRef<'_> {
        CanonPkcPublicKeyRef::new(&[0; 65])
    }
    fn dac_priv_key(&self) -> CanonPkcSecretKeyRef<'_> {
        CanonPkcSecretKeyRef::new(&[0; 32])
    }
}
static ATTESTATION: ControllerAttestation = ControllerAttestation;
extern "C" fn stop(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}
struct Wake;
impl std::task::Wake for Wake {
    fn wake(self: std::sync::Arc<Self>) {}
}

fn validate_thread_settings(state: &std::path::Path) -> Result<(), Box<dyn std::error::Error>> {
    use std::os::unix::fs::MetadataExt;
    let settings = state.join("tmp");
    let metadata = std::fs::symlink_metadata(&settings)?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o077 != 0
    {
        return Err("unsafe Thread settings directory".into());
    }
    for entry in std::fs::read_dir(&settings)? {
        let path = entry?.path();
        let metadata = std::fs::symlink_metadata(&path)?;
        if !metadata.is_file()
            || metadata.uid() != unsafe { libc::geteuid() }
            || metadata.mode() & 0o077 != 0
            || metadata.len() > 65536
        {
            return Err("unsafe Thread settings file".into());
        }
        let bytes = std::fs::read(&path)?;
        let mut offset = 0usize;
        while offset < bytes.len() {
            if bytes.len() - offset < 4 {
                return Err("invalid Thread settings file".into());
            }
            let length = usize::from(u16::from_ne_bytes([bytes[offset + 2], bytes[offset + 3]]));
            offset += length + 4;
            if offset > bytes.len() {
                return Err("invalid Thread settings file".into());
            }
        }
    }
    Ok(())
}

fn validate_state_directory(
    state: &std::path::Path,
    administrator_vendor: u16,
) -> Result<(), &'static str> {
    validate_thread_settings(state).map_err(|_| "Thread settings")?;
    let store = PrivateStore::open_read_only(state).map_err(|_| "private store")?;
    let mut entropy = OsEntropy::open().map_err(|_| "OS entropy")?;
    let mut singleton = [0; 32];
    entropy.try_fill_bytes(&mut singleton).unwrap();
    let crypto = default_crypto(entropy, CryptoSensitiveRef::new(&singleton));
    let matter = MATTER.uninit().init_with(Matter::init(
        &DETAILS,
        BasicCommData {
            password: Spake2pVerifierPassword::new_from_ref(Spake2pVerifierPasswordRef::new(
                &[0; 4],
            )),
            discriminator: 0,
        },
        &ATTESTATION,
        MATTER_PORT,
    ));
    let identity = Identity::validate_existing(&store, &crypto, administrator_vendor)
        .map_err(|_| "controller authority")?;
    let kv = matter.kv(store.clone());
    matter.startup(&kv).map_err(|_| "Matter fabric records")?;
    identity
        .validate_installed(matter)
        .map_err(|_| "Matter fabric identity")?;
    Catalog::validate(&store, identity.fabric_id(), identity.next_node())
        .map_err(|_| "node catalog")?;
    Lifecycle::load(store).map_err(|_| "Thread dataset")?;
    Ok(())
}

fn dispatch<C: Crypto>(
    request: Frame,
    peer: u64,
    subscribers: &mut Vec<(u64, Vec<u8>)>,
    lifecycle: &mut Lifecycle,
    thread: Option<&OpenThread>,
    entropy: &mut OsEntropy,
    fabric_ready: bool,
    controller: &mut Controller<'_, C>,
    devices: &mut micro_thread_matter_daemon::devices::Manager<'_, C>,
) -> Option<Frame> {
    if request.opcode == 12 {
        return None;
    }
    let decoded = Request::decode(request.opcode, &request.payload);
    let payload = match decoded {
        Ok(Request::MatterStatus) => format!("{{\"enabled\":true,\"available\":true,\"controller_initialized\":true,\"fabric_ready\":{},\"state\":\"{}\"}}",
            fabric_ready, if thread.is_some() { "running" } else { "awaiting_coordinator" }),
        Ok(Request::ThreadStatus) => serde_json::to_string(&lifecycle.snapshot()).ok()?,
        Ok(Request::Thread(body)) => {
            let now = Instant::now();
            let radio = thread.map(|r| r as &dyn Radio);
            let result = match body {
                ThreadRequest::Status { subscribe, .. } => {
                    if subscribe && !subscribers.iter().any(|entry| entry.0 == peer) {
                        subscribers.push((peer, Vec::new()));
                    }
                    Ok(lifecycle.snapshot())
                },
                ThreadRequest::Observe { zigbee_channel, zigbee_online, .. } =>
                    lifecycle.observe(zigbee_channel, zigbee_online, radio, now),
                ThreadRequest::Create { zigbee_channel, .. } => match radio {
                    Some(radio) => lifecycle.create(zigbee_channel, entropy, radio, now),
                    None => Err(Failure::NotReady),
                },
                ThreadRequest::Import { zigbee_channel, dataset, .. } => match radio {
                    Some(radio) => lifecycle.import(dataset.bytes(), zigbee_channel, radio, now),
                    None => Err(Failure::NotReady),
                },
                ThreadRequest::Retry { .. } => match radio {
                    Some(radio) => lifecycle.retry(radio, now),
                    None => Err(Failure::NotReady),
                },
            };
            String::from_utf8(micro_thread_matter_daemon::responses::reply(result)?).ok()?
        },
        Ok(Request::CommissionStart(_)) if devices.operations_busy() =>
            String::from_utf8(micro_thread_matter_daemon::responses::reply::<(), _>(Err(
                micro_thread_matter_daemon::sessions::SessionError::Busy,
            ))?).ok()?,
        Ok(body @ (Request::CommissionStart(_) | Request::CommissionStatus(_) |
            Request::CommissionCancel(_) | Request::BleAttach(_) | Request::BleIndication(_) |
            Request::BleWriteComplete(_))) => String::from_utf8(controller.handle(
                body, peer, lifecycle, entropy, Instant::now(),
            )?).ok()?,
        Ok(Request::Device(micro_thread_matter_daemon::control::DeviceRequest::Snapshot { cursor, revision, .. })) =>
            String::from_utf8(micro_thread_matter_daemon::responses::reply(devices.page(cursor, revision))?).ok()?,
        Ok(Request::Device(body @ micro_thread_matter_daemon::control::DeviceRequest::Read { .. })) =>
            String::from_utf8(micro_thread_matter_daemon::responses::reply(devices.submit(
                Request::Device(body), lifecycle.snapshot().attached && thread.is_some(),
                controller.commissioning(Instant::now()), Instant::now(),
            ))?).ok()?,
        Ok(Request::Device(body @ micro_thread_matter_daemon::control::DeviceRequest::Bind { .. })) =>
            String::from_utf8(micro_thread_matter_daemon::responses::reply(devices.submit(
                Request::Device(body), lifecycle.snapshot().attached && thread.is_some(),
                controller.commissioning(Instant::now()), Instant::now(),
            ))?).ok()?,
        Ok(Request::Command(body @ (micro_thread_matter_daemon::control::CommandRequest::On { .. } |
            micro_thread_matter_daemon::control::CommandRequest::Off { .. } |
            micro_thread_matter_daemon::control::CommandRequest::Toggle { .. } |
            micro_thread_matter_daemon::control::CommandRequest::MoveToLevel { .. }))) =>
            String::from_utf8(micro_thread_matter_daemon::responses::reply(devices.submit(
                Request::Command(body), lifecycle.snapshot().attached && thread.is_some(),
                controller.commissioning(Instant::now()), Instant::now(),
            ))?).ok()?,
        Ok(Request::Device(micro_thread_matter_daemon::control::DeviceRequest::Job { job, .. })) =>
            String::from_utf8(micro_thread_matter_daemon::responses::reply(devices.job(job))?).ok()?,
        Err(_) if request.opcode == OP_MATTER_STATUS || request.opcode == OP_THREAD_STATUS => return None,
        Err(_) => "{\"schema\":1,\"ok\":false,\"error\":\"malformed\"}".into(),
    };
    Some(Frame {
        kind: 1,
        opcode: request.opcode,
        id: request.id,
        payload: payload.into_bytes(),
    })
}

fn publish_thread(
    ipc: &mut IpcServer,
    subscribers: &mut Vec<(u64, Vec<u8>)>,
    lifecycle: &Lifecycle,
) -> Result<(), std::io::Error> {
    subscribers.retain(|entry| ipc.has_peer(entry.0));
    if subscribers.is_empty() {
        return Ok(());
    }
    let snapshot = lifecycle.snapshot();
    let event = micro_thread_matter_daemon::responses::event(
        micro_thread_matter_daemon::responses::Event::Thread(&snapshot),
    )
    .ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::InvalidData, "Thread event unavailable")
    })?;
    for (peer, previous) in subscribers.iter_mut() {
        if *previous != event.payload && ipc.publish(*peer, &event)? {
            *previous = event.payload.clone();
        }
    }
    Ok(())
}

fn run(
    log_guard: &mut Option<micro_thread_matter_daemon::logging::WorkerGuard>,
) -> Result<(), Box<dyn std::error::Error>> {
    unsafe {
        libc::umask(0o077);
        let no_core = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        if libc::setrlimit(libc::RLIMIT_CORE, &no_core) != 0 {
            return Err("core limit unavailable".into());
        }
        libc::signal(libc::SIGTERM, stop as *const () as libc::sighandler_t);
        libc::signal(libc::SIGINT, stop as *const () as libc::sighandler_t);
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }
    let mut socket = PathBuf::from("/tmp/microhad-matter.sock");
    let mut state = PathBuf::from("/tuya/native/matter");
    let mut radio = None;
    let mut administrator_vendor = 0xfff1u16;
    let mut validate_only = false;
    let mut log_level = log::LevelFilter::Info;
    let mut raw_frames = false;
    let mut diagnostic_on_mesh_prefix = false;
    let mut diagnostic_external_routes = false;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--raw-frames" {
            raw_frames = true;
            continue;
        }
        if arg == "--diagnostic-on-mesh-prefix" {
            diagnostic_on_mesh_prefix = true;
            continue;
        }
        if arg == "--diagnostic-external-routes" {
            diagnostic_external_routes = true;
            continue;
        }
        let value = args.next().ok_or("missing option value")?;
        match arg.as_str() {
            "--log-level" => log_level = micro_thread_matter_daemon::logging::parse_level(&value)?,
            "--socket" => socket = value.into(),
            "--state-dir" => state = value.into(),
            "--validate-state-dir" => {
                state = value.into();
                validate_only = true;
            }
            "--radio-url" => radio = Some(value),
            "--administrator-vendor-id" => administrator_vendor = value.parse()?,
            _ => return Err("unknown option".into()),
        }
    }
    *log_guard = Some(
        micro_thread_matter_daemon::logging::init(log_level, raw_frames)
            .map_err(|_| "logger initialization failed")?,
    );
    if !state.is_absolute() {
        return Err("absolute state directory required".into());
    }
    if validate_only {
        if radio.is_some() {
            return Err("state validation does not accept a radio".into());
        }
        match validate_state_directory(&state, administrator_vendor) {
            Ok(()) => eprintln!("Matter state validated"),
            Err(stage) => {
                eprintln!("Matter state validation failed ({stage})");
                return Err("Matter state validation failed".into());
            }
        }
        return Ok(());
    }
    let radio = radio.ok_or("private no-reset radio URL required")?;
    if !radio.starts_with("spinel+hdlc+unix:///")
        || radio.contains('\0')
        || radio.split_once('?').map(|(_, query)| query) != Some("no-reset")
    {
        return Err("private no-reset radio URL required".into());
    }
    let mut ipc = IpcServer::bind(&socket)?;
    let store = PrivateStore::open(&state)?;
    // OpenThread's pinned POSIX settings path is relative "tmp". Keep it
    // beneath the private service state directory rather than the launcher cwd.
    std::env::set_current_dir(&state)?;
    match std::fs::create_dir("tmp") {
        Ok(()) => (),
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => (),
        Err(error) => return Err(error.into()),
    }
    // Reject malformed POSIX records before OpenThread can truncate the file.
    validate_thread_settings(&state)?;
    let mut entropy = OsEntropy::open()?;
    let mut singleton = [0; 32];
    entropy.try_fill_bytes(&mut singleton).unwrap();
    let crypto = default_crypto(entropy, CryptoSensitiveRef::new(&singleton));
    let matter = MATTER.uninit().init_with(Matter::init(
        &DETAILS,
        BasicCommData {
            password: Spake2pVerifierPassword::new_from_ref(Spake2pVerifierPasswordRef::new(
                &[0; 4],
            )),
            discriminator: 0,
        },
        &ATTESTATION,
        MATTER_PORT,
    ));
    let identity = Identity::open(&store, &crypto, administrator_vendor)?;
    let kv = matter.kv(store.clone());
    matter.startup(&kv)?;
    let controller_fabric = identity.install(matter, &crypto, &store)?;
    let relay = BleRelay::new();
    let mut controller = Controller::new(
        &relay,
        matter,
        &crypto,
        identity,
        store.clone(),
        controller_fabric,
    )?;
    let mut lifecycle = Lifecycle::load(store.clone()).map_err(|_| "Thread state unavailable")?;
    let (compressed_fabric, next_node) = controller.discovery_identity();
    let address_refresh = micro_thread_matter_daemon::discovery::RefreshRequests::default();
    let mut devices = micro_thread_matter_daemon::devices::Manager::new_with_address_refresh(
        matter,
        &crypto,
        controller_fabric,
        controller.catalog(),
        micro_thread_matter_daemon::devices::MAX_DEVICE_SUBSCRIPTIONS,
        compressed_fabric,
        address_refresh.clone(),
    )?;
    #[cfg(feature = "controller-service-test-hooks")]
    if let Some(node) = std::env::var("MICROHAD_MATTER_TEST_SEED_SENSOR_REPORT_NODE").ok() {
        let node = u64::from_str_radix(node.trim_start_matches("0x"), 16)
            .map_err(|_| "invalid test report node")?;
        devices
            .seed_host_test_sensor_reports(node)
            .map_err(|_| "test report fixture unavailable")?;
    }
    let mut configuration_entropy = OsEntropy::open()?;
    let mut subscribers = Vec::new();
    // Do not initialize OT (and restore POSIX settings) until a fresh protected
    // native coordinator observation agrees with the persisted dataset.
    log::info!("Matter controller service started");
    let channel = loop {
        if STOP.load(Ordering::Relaxed) {
            log::info!("Matter controller service stopped");
            return Ok(());
        }
        lifecycle
            .tick(None, Instant::now())
            .map_err(|_| "Thread state unavailable")?;
        ipc.poll_with_peer(|peer, request| {
            dispatch(
                request,
                peer,
                &mut subscribers,
                &mut lifecycle,
                None,
                &mut configuration_entropy,
                matter.has_fabrics(),
                &mut controller,
                &mut devices,
            )
        })?;
        publish_thread(&mut ipc, &mut subscribers, &lifecycle)?;
        if let Some(channel) = lifecycle.boot_channel(Instant::now()) {
            break channel;
        }
        std::thread::sleep(Duration::from_millis(5));
    };
    let thread = OpenThread::init_on_channel(&radio, channel)?;
    lifecycle
        .restore(&thread, Instant::now())
        .map_err(|_| "Thread restore failed")?;
    thread.configure_diagnostic_routes(diagnostic_on_mesh_prefix, diagnostic_external_routes)?;
    thread.start_discovery()?;
    let discovery = micro_thread_matter_daemon::discovery::run_with_catalog_and_refresh(
        matter,
        &thread,
        controller.catalog(),
        compressed_fabric,
        next_node,
        address_refresh,
    );
    let mut discovery = pin!(discovery);
    let network = thread.bind(MATTER_PORT)?;
    let route_btp = |address: &Address| matches!(address, Address::Btp(_));
    let send = ChainedNetwork::new(route_btp, relay.transport(), &network);
    let receive = send.clone();
    let transport = matter.run(&crypto, send, receive, NoNetwork);
    let mut transport = pin!(transport);
    let responder = IcdCheckInResponder::new(matter);
    let mut responder = pin!(responder.run());
    let waker = std::task::Waker::from(std::sync::Arc::new(Wake));
    let mut context = std::task::Context::from_waker(&waker);
    while !STOP.load(Ordering::Relaxed) {
        if let std::task::Poll::Ready(result) = transport.as_mut().poll(&mut context) {
            result?;
            return Err("Matter transport ended".into());
        }
        if let std::task::Poll::Ready(result) = responder.as_mut().poll(&mut context) {
            result?;
            return Err("Matter ICD Check-In responder ended".into());
        }
        thread.set_binding_diagnostics(controller.commissioning(Instant::now()));
        if let std::task::Poll::Ready(result) = discovery.as_mut().poll(&mut context) {
            result?;
            return Err("Operational discovery ended".into());
        }
        thread.poll(5_000)?;
        lifecycle
            .tick(Some(&thread), Instant::now())
            .map_err(|_| "Thread radio stop failed")?;
        ipc.poll_with_peer(|peer, request| {
            dispatch(
                request,
                peer,
                &mut subscribers,
                &mut lifecycle,
                Some(&thread),
                &mut configuration_entropy,
                matter.has_fabrics(),
                &mut controller,
                &mut devices,
            )
        })?;
        controller.poll(&lifecycle, Instant::now(), &mut context);
        devices.poll(
            lifecycle.snapshot().attached,
            controller.commissioning(Instant::now()),
            Instant::now(),
            &mut context,
        );
        devices.publish_reports(&mut ipc)?;
        controller.publish(&mut ipc, Instant::now())?;
        publish_thread(&mut ipc, &mut subscribers, &lifecycle)?;
        if controller.fatal() {
            return Err("Matter catalog storage failed".into());
        }
        if lifecycle.fatal() {
            return Err("Thread radio stop failed".into());
        }
    }
    log::info!("Matter controller service stopped");
    Ok(())
}
fn main() {
    let mut log_guard = None;
    if run(&mut log_guard).is_err() {
        // Errors deliberately omit CLI inputs, blobs and credential contents.
        if !micro_thread_matter_daemon::logging::initialized() {
            // CLI failures happen before the logger is installed.
            eprintln!("Matter controller service failed");
        } else {
            log::error!("Matter controller service failed");
        }
        drop(log_guard);
        std::process::exit(1);
    }
}
