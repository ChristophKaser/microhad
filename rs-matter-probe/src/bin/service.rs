//! Gateway controller process. Owns OpenThread, Matter and IPC on a single thread.
use core::pin::pin;
use lexra_rs_matter_probe::{
    control::Request,
    entropy::OsEntropy,
    identity::Identity,
    ipc::{Frame, OP_MATTER_STATUS, OP_THREAD_STATUS},
    openthread::OpenThread,
    service::IpcServer,
    store::PrivateStore,
};
use rs_matter::crypto::{
    default_crypto, CanonPkcPublicKeyRef, CanonPkcSecretKeyRef, CryptoSensitiveRef, TryRng,
};
use rs_matter::dm::clusters::{basic_info::BasicInfoConfig, dev_att::DeviceAttestation};
use rs_matter::sc::pase::{Spake2pVerifierPassword, Spake2pVerifierPasswordRef};
use rs_matter::transport::network::NoNetwork;
use rs_matter::utils::{init::InitMaybeUninit, sync::DynBase};
use rs_matter::{BasicCommData, Matter, MATTER_PORT};
use static_cell::StaticCell;
use std::future::Future;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};

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

fn run() -> Result<(), Box<dyn std::error::Error>> {
    unsafe {
        libc::umask(0o077);
        libc::signal(libc::SIGTERM, stop as *const () as libc::sighandler_t);
        libc::signal(libc::SIGINT, stop as *const () as libc::sighandler_t);
        libc::signal(libc::SIGPIPE, libc::SIG_IGN);
    }
    let mut socket = PathBuf::from("/tmp/microhad-matter.sock");
    let mut state = PathBuf::from("/tuya/native/matter");
    let mut radio = None;
    let mut administrator_vendor = 0xfff1u16;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let value = args.next().ok_or("missing option value")?;
        match arg.as_str() {
            "--socket" => socket = value.into(),
            "--state-dir" => state = value.into(),
            "--radio-url" => radio = Some(value),
            "--administrator-vendor-id" => administrator_vendor = value.parse()?,
            _ => return Err("unknown option".into()),
        }
    }
    if !state.is_absolute() {
        return Err("absolute state directory required".into());
    }
    let radio = radio.ok_or("private no-reset radio URL required")?;
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
    use std::os::unix::fs::MetadataExt;
    let meta = std::fs::symlink_metadata("tmp")?;
    if !meta.is_dir() || meta.uid() != unsafe { libc::geteuid() } || meta.mode() & 0o077 != 0 {
        return Err("unsafe Thread settings directory".into());
    }
    for entry in std::fs::read_dir("tmp")? {
        let meta = std::fs::symlink_metadata(entry?.path())?;
        if !meta.is_file() || meta.uid() != unsafe { libc::geteuid() } || meta.mode() & 0o077 != 0 {
            return Err("unsafe Thread settings file".into());
        }
    }
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
    let _controller_fabric = identity.install(matter, &crypto, &store)?;
    let thread = OpenThread::init(&radio)?;
    let network = thread.bind(MATTER_PORT)?;
    let transport = matter.run(&crypto, &network, &network, NoNetwork);
    let mut transport = pin!(transport);
    let waker = std::task::Waker::from(std::sync::Arc::new(Wake));
    let mut context = std::task::Context::from_waker(&waker);
    eprintln!("Matter controller service started");
    while !STOP.load(Ordering::Relaxed) {
        if let std::task::Poll::Ready(result) = transport.as_mut().poll(&mut context) {
            result?;
            return Err("Matter transport ended".into());
        }
        thread.poll(5_000)?;
        ipc.poll(|request| {
            if request.opcode == 12 { return None; }
            let decoded = Request::decode(request.opcode, &request.payload);
            let payload = match decoded {
                Ok(Request::MatterStatus) => format!("{{\"enabled\":true,\"available\":true,\"controller_initialized\":true,\"fabric_ready\":{},\"state\":\"{}\"}}",
                    matter.has_fabrics(), if matter.has_fabrics() { "running" } else { "awaiting_fabric" }),
                Ok(Request::ThreadStatus) => format!("{{\"enabled\":true,\"available\":true,\"attached\":{},\"state\":\"{}\",\"linux_ipv6_or_tun\":false}}",
                    thread.ready(), if thread.ready() { "attached" } else { "detached" }),
                // Valid reserved operations have no behavior until their owning
                // lifecycle is implemented. Never imply job acceptance here.
                Ok(_) => "{\"schema\":1,\"ok\":false,\"error\":\"unsupported\"}".into(),
                Err(_) if request.opcode == OP_MATTER_STATUS || request.opcode == OP_THREAD_STATUS => return None,
                Err(_) => "{\"schema\":1,\"ok\":false,\"error\":\"malformed\"}".into(),
            };
            Some(Frame { kind: 1, opcode: request.opcode, id: request.id, payload: payload.into_bytes() })
        })?;
    }
    eprintln!("Matter controller service stopped");
    Ok(())
}
fn main() {
    if run().is_err() {
        // Errors deliberately omit CLI inputs, blobs and credential contents.
        eprintln!("Matter controller service failed");
        std::process::exit(1);
    }
}
