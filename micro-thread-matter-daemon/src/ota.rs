//! Manually authorized, temporary OTA image provider. The private control socket
//! never accepts arbitrary paths outside MicroHAD's temporary upload namespace.
use crate::catalog::{Catalog, Phase};
use core::num::NonZeroU8;
use rs_matter::{
    crypto::{Crypto, CryptoSensitive, Digest},
    dm::clusters::{
        decl::{
            basic_information::BasicInformationClient,
            ota_software_update_requestor::{
                AnnouncementReasonEnum, OtaSoftwareUpdateRequestorClient,
            },
        },
        ota_prov::*,
    },
    error::{Error, ErrorCode},
    ota::{OtaImageHeader, OTA_IMAGE_DIGEST_SHA256},
    transport::exchange::Exchange,
    Matter,
};
use crate::ota_wire;
use std::{
    cell::RefCell,
    fs::{File, OpenOptions},
    io::{Read, Write},
    os::unix::{
        fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
        net::{UnixListener, UnixStream},
    },
    path::PathBuf,
    time::{Duration, Instant},
};

// One browser response at a time; firmware payload is never written to disk.
struct BrowserStream {
    id: String,
    pending: Option<(u64, usize, Instant)>,
    bytes: Option<Vec<u8>>,
    hash: sha2::Sha256,
    offset: u64,
}

struct Image {
    file: File,
    stream: Option<BrowserStream>,
    header: Vec<u8>,
    size: u64,
    node: u64,
    endpoint: u16,
    token: [u8; 16],
    designator: String,
    progress: u64,
    state: &'static str,
    error: String,
    touched: Instant,
    announce: bool,
}
impl Image {
    fn active(&self) -> bool {
        matches!(
            self.state,
            "waiting" | "transferring" | "awaiting_confirmation"
        )
    }
    fn metadata(&self) -> Result<OtaImageHeader<'_>, Error> {
        OtaImageHeader::decode(&self.header).map(|v| v.0)
    }
}
pub struct Provider {
    image: RefCell<Option<Image>>,
    fabric: NonZeroU8,
}
struct ProviderStatus {
    state: &'static str,
    progress: u64,
    error: String,
    version: Option<String>,
    manufacturer: Option<u16>,
    image_type: Option<u16>,
}
impl Provider {
    pub fn new(fabric: NonZeroU8) -> Self {
        Self {
            image: RefCell::new(None),
            fabric,
        }
    }
    fn authorized(&self, peer: OtaRequestorId) -> bool {
        self.image
            .borrow()
            .as_ref()
            .is_some_and(|i| i.active() && i.node == peer.node_id && peer.fab_idx == self.fabric)
    }
    pub fn peer(&self, peer: Option<(u8, u64)>) -> bool {
        peer.is_some_and(|(f, n)| {
            NonZeroU8::new(f).is_some_and(|fab_idx| {
                self.authorized(OtaRequestorId {
                    fab_idx,
                    node_id: n,
                })
            })
        })
    }
    fn fail(&self, error: &str) {
        if let Some(i) = self.image.borrow_mut().as_mut() {
            i.state = "failed";
            i.error = error.into();
        }
    }
    fn cancel(&self) {
        *self.image.borrow_mut() = None;
    }
    fn expire(&self) {
        if let Some(image) = self.image.borrow_mut().as_mut() {
            if image.active() && image.touched.elapsed() > Duration::from_secs(3600) {
                image.state = "failed";
                image.error = "Update timed out".into();
            }
        }
    }
    fn status(&self) -> ProviderStatus {
        let mut image = self.image.borrow_mut();
        if let Some(i) = image.as_mut() {
            if i.active() && i.touched.elapsed() > Duration::from_secs(3600) {
                i.state = "failed";
                i.error = "Update timed out".into();
            }
            ProviderStatus { state:i.state,progress:i.progress,error:i.error.clone(),version:Some(i.metadata().map(|m|m.software_version).unwrap_or(0).to_string()),manufacturer:Some(i.metadata().map(|m|m.vendor_id).unwrap_or(0)),image_type:Some(i.metadata().map(|m|m.product_id).unwrap_or(0)) }
        } else {
            ProviderStatus { state:"empty",progress:0,error:String::new(),version:None,manufacturer:None,image_type:None }
        }
    }
    fn start<C: Crypto>(&self, path: &str, node: u64, endpoint: u16, crypto: &C) -> Result<(), Error> {
        self.start_source(path, node, endpoint, crypto, false)
    }
    fn start_source<C: Crypto>(
        &self,
        path: &str,
        node: u64,
        endpoint: u16,
        crypto: &C,
        streaming: bool,
    ) -> Result<(), Error> {
        if self.image.borrow().as_ref().is_some_and(Image::active) {
            return Err(ErrorCode::Busy.into());
        }
        let suffix = path
            .strip_prefix("/tmp/microhad-ota-")
            .ok_or(ErrorCode::InvalidData)?;
        if suffix.len() != 6 || !suffix.bytes().all(|b| b.is_ascii_alphanumeric()) {
            return Err(ErrorCode::InvalidData.into());
        }
        let mut file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(path)
            .map_err(|_| ErrorCode::InvalidData)?;
        let meta = file.metadata().map_err(|_| ErrorCode::InvalidData)?;
        if !meta.is_file()
            || meta.uid() != unsafe { libc::geteuid() }
            || meta.mode() & 0o077 != 0
            || !(56..=16 * 1024 * 1024).contains(&meta.len())
        {
            return Err(ErrorCode::InvalidData.into());
        }
        let mut prefix = [0u8; 16];
        file.read_exact(&mut prefix)
            .map_err(|_| ErrorCode::InvalidData)?;
        let length = u32::from_le_bytes(prefix[12..16].try_into().unwrap()) as usize;
        let total = u64::from_le_bytes(prefix[4..12].try_into().unwrap());
        if length > 4096 || !(56..=16 * 1024 * 1024).contains(&total)
            || if streaming { meta.len() != (16 + length) as u64 } else { total != meta.len() } {
            return Err(ErrorCode::InvalidData.into());
        }
        let mut header = vec![0; 16 + length];
        header[..16].copy_from_slice(&prefix);
        file.read_exact(&mut header[16..])
            .map_err(|_| ErrorCode::InvalidData)?;
        let (m, offset) = OtaImageHeader::decode(&header)?;
        if m.image_digest_type != OTA_IMAGE_DIGEST_SHA256 {
            return Err(ErrorCode::InvalidData.into());
        }
        if !streaming {
            let mut hasher = crypto.hash()?;
            let mut bytes = [0; 4096];
            let mut count = offset as u64;
            loop {
                let n = file.read(&mut bytes).map_err(|_| ErrorCode::InvalidData)?;
                if n == 0 {
                    break;
                }
                hasher.update(&bytes[..n])?;
                count += n as u64;
            }
            let mut digest = CryptoSensitive::new();
            hasher.finish(&mut digest)?;
            if count != meta.len() || digest.access().as_slice() != m.image_digest.0 {
                return Err(ErrorCode::InvalidData.into());
            }
        }
        let mut token = [0; 16];
        File::open("/dev/urandom")
            .and_then(|mut f| f.read_exact(&mut token))
            .map_err(|_| ErrorCode::InvalidData)?;
        let designator = token.iter().map(|b| format!("{b:02x}")).collect();
        *self.image.borrow_mut() = Some(Image {
            file,
            stream: if streaming { Some(BrowserStream {
                id: suffix.into(), pending: None, bytes: None,
                hash: sha2::Digest::new(), offset: header.len() as u64,
            }) } else { None },
            header,
            size: total,
            node,
            endpoint,
            token,
            designator,
            progress: 0,
            state: if node == 0 { "ready" } else { "waiting" },
            error: String::new(),
            touched: Instant::now(),
            announce: node != 0,
        });
        Ok(())
    }
    fn stream_chunk(&self, data: &str) -> Result<(), Error> {
        let mut fields = data.splitn(3, ':');
        let id = fields.next().ok_or(ErrorCode::InvalidData)?;
        let offset: u64 = fields.next().ok_or(ErrorCode::InvalidData)?.parse().map_err(|_| ErrorCode::InvalidData)?;
        let hex = fields.next().ok_or(ErrorCode::InvalidData)?;
        let mut image = self.image.borrow_mut();
        let i = image.as_mut().filter(|i| i.active()).ok_or(ErrorCode::InvalidData)?;
        let stream = i.stream.as_mut().ok_or(ErrorCode::InvalidData)?;
        let (want, count, _) = stream.pending.ok_or(ErrorCode::InvalidData)?;
        if id != stream.id || offset != want || hex.len() != count * 2 || count > 480 || stream.bytes.is_some() {
            return Err(ErrorCode::InvalidData.into());
        }
        let mut bytes = Vec::with_capacity(count);
        for pair in hex.as_bytes().chunks_exact(2) {
            let digit = |c: u8| match c { b'0'..=b'9' => Some(c-b'0'), b'a'..=b'f' => Some(c-b'a'+10), _ => None };
            bytes.push((digit(pair[0]).ok_or(ErrorCode::InvalidData)? << 4) | digit(pair[1]).ok_or(ErrorCode::InvalidData)?);
        }
        stream.bytes = Some(bytes);
        Ok(())
    }
    fn pending(&self) -> (u64, u16) {
        self.image.borrow().as_ref().filter(|i| i.active())
            .and_then(|i| i.stream.as_ref()).and_then(|s| s.pending)
            .map(|(offset, count, _)| (offset, count as u16)).unwrap_or((0,0))
    }
    pub async fn announce<C: Crypto>(&self, matter: &Matter<'_>, crypto: &C) {
        loop {
            let node = {
                let mut image = self.image.borrow_mut();
                image
                    .as_mut()
                    .filter(|i| i.announce && i.active())
                    .map(|i| {
                        i.announce = false;
                        (i.node, i.token)
                    })
            };
            if let Some((node, token)) = node {
                let result = embassy_time::with_timeout(
                    embassy_time::Duration::from_secs(30),
                    self.announce_one(matter, crypto, node),
                )
                .await;
                if !matches!(result, Ok(Ok(())))
                    && self
                        .image
                        .borrow()
                        .as_ref()
                        .is_some_and(|i| i.token == token && i.active())
                {
                    self.fail("Could not announce OTA provider; wake the device and retry");
                }
            }
            embassy_time::Timer::after_millis(100).await;
        }
    }
    async fn announce_one<C: Crypto>(
        &self,
        matter: &Matter<'_>,
        crypto: &C,
        node: u64,
    ) -> Result<(), Error> {
        let vendor = Exchange::initiate(matter, crypto, self.fabric, node)
            .await?
            .basic_information()
            .vendor_id_read(0)
            .await?;
        let product = Exchange::initiate(matter, crypto, self.fabric, node)
            .await?
            .basic_information()
            .product_id_read(0)
            .await?;
        let version = Exchange::initiate(matter, crypto, self.fabric, node)
            .await?
            .basic_information()
            .software_version_read(0)
            .await?;
        let endpoint = {
            let image = self.image.borrow();
            let i = image.as_ref().ok_or(ErrorCode::InvalidData)?;
            let m = i.metadata()?;
            if !i.active()
                || i.node != node
                || !m.is_for_product(vendor, product)
                || !m.is_update_for(version)
            {
                return Err(ErrorCode::InvalidData.into());
            }
            i.endpoint
        };
        let provider = matter.with_state(|s| s.fabrics.fabric(self.fabric).map(|f| f.node_id()))?;
        Exchange::initiate(matter, crypto, self.fabric, node)
            .await?
            .ota_software_update_requestor()
            .announce_ota_provider(endpoint, |r| {
                r.provider_node_id(provider)?
                    .vendor_id(matter.dev_det().vid)?
                    .announcement_reason(AnnouncementReasonEnum::UpdateAvailable)?
                    .metadata_for_node(None)?
                    .endpoint(0)?
                    .end()
            })
            .await
    }
}
impl OtaImagesRegistry for Provider {
    async fn query<'b>(
        &self,
        peer: OtaRequestorId,
        vendor: u16,
        product: u16,
        current: u32,
        _consent: bool,
        buf: &'b mut [u8],
    ) -> OtaQueryOutcome<'b> {
        if !self.authorized(peer) {
            return OtaQueryOutcome::NotAvailable;
        }
        let mut image = self.image.borrow_mut();
        let i = image.as_mut().unwrap();
        let Ok(m) = i.metadata() else {
            return OtaQueryOutcome::NotAvailable;
        };
        if !m.is_for_product(vendor, product) || !m.is_update_for(current) {
            return OtaQueryOutcome::NotAvailable;
        }
        let version = m.software_version;
        let v = m.software_version_str.as_bytes();
        let n = v.len();
        let d = i.designator.len();
        if buf.len() < n + d + 16 {
            return OtaQueryOutcome::NotAvailable;
        }
        buf[..n].copy_from_slice(v);
        buf[n..n + d].copy_from_slice(i.designator.as_bytes());
        buf[n + d..n + d + 16].copy_from_slice(&i.token);
        i.touched = Instant::now();
        OtaQueryOutcome::Available(OtaImageMeta {
            version,
            version_str: core::str::from_utf8(&buf[..n]).unwrap(),
            file_designator: core::str::from_utf8(&buf[n..n + d]).unwrap(),
            update_token: &buf[n + d..n + d + 16],
            user_consent_needed: false,
        })
    }
    async fn apply(&self, peer: OtaRequestorId, token: &[u8], version: u32) -> OtaApplyOutcome {
        if !self.authorized(peer) {
            return OtaApplyOutcome::Discontinue;
        }
        let mut image = self.image.borrow_mut();
        let i = image.as_mut().unwrap();
        if token != i.token
            || i.metadata()
                .map(|m| m.software_version != version)
                .unwrap_or(true)
            || i.progress != i.size
        {
            return OtaApplyOutcome::Discontinue;
        }
        i.state = "awaiting_confirmation";
        i.touched = Instant::now();
        OtaApplyOutcome::Proceed { delay_secs: 0 }
    }
    async fn applied(&self, peer: OtaRequestorId, token: &[u8], version: u32) {
        if !self.authorized(peer) {
            return;
        }
        if let Some(i) = self.image.borrow_mut().as_mut() {
            if token == i.token
                && i.metadata()
                    .map(|m| m.software_version == version)
                    .unwrap_or(false)
            {
                i.state = "complete";
            }
        }
    }
}
impl OtaImages for Provider {
    async fn size(&self, fd: &[u8]) -> Option<u64> {
        self.image
            .borrow()
            .as_ref()
            .filter(|i| i.active() && i.designator.as_bytes() == fd)
            .map(|i| i.size)
    }
    async fn read(&self, fd: &[u8], offset: u64, buf: &mut [u8]) -> Result<usize, Error> {
        use std::os::unix::fs::FileExt;
        use sha2::Digest as _;
        // Never hold a RefCell borrow while waiting for browser HTTP traffic.
        loop {
            {
                let mut image = self.image.borrow_mut();
                let i = image.as_mut().filter(|i| i.active() && i.designator.as_bytes() == fd)
                    .ok_or(ErrorCode::InvalidData)?;
                if offset > i.size { return Err(ErrorCode::InvalidData.into()); }
                let len = buf.len().min((i.size - offset) as usize);
                if len == 0 { return Ok(0); }
                let n = if let Some(stream) = i.stream.as_mut() {
                    if offset == 0 {
                        // A fresh download may restart after an interrupted exchange.
                        stream.hash = sha2::Sha256::new();
                        stream.offset = i.header.len() as u64;
                        stream.pending = None; stream.bytes = None; i.progress = 0;
                    }
                    if offset < i.header.len() as u64 {
                        let n = len.min(i.header.len() - offset as usize);
                        buf[..n].copy_from_slice(&i.header[offset as usize..offset as usize+n]);
                        n
                    } else {
                        if offset != stream.offset { return Err(ErrorCode::InvalidData.into()); }
                        if let Some(bytes) = stream.bytes.take() {
                            if bytes.len() > len { return Err(ErrorCode::InvalidData.into()); }
                            stream.hash.update(&bytes);
                            stream.offset += bytes.len() as u64;
                            stream.pending = None;
                            if stream.offset == i.size {
                                let actual = stream.hash.clone().finalize();
                                let expected = OtaImageHeader::decode(&i.header)?.0.image_digest.0;
                                if actual.as_slice() != expected {
                                    i.state = "failed"; i.error = "Browser firmware digest mismatch".into();
                                    return Err(ErrorCode::InvalidData.into());
                                }
                            }
                            buf[..bytes.len()].copy_from_slice(&bytes);
                            bytes.len()
                        } else {
                            let pending = stream.pending.get_or_insert((offset, len.min(480), Instant::now()));
                            if pending.0 != offset || pending.1 > len { return Err(ErrorCode::InvalidData.into()); }
                            if pending.2.elapsed() > Duration::from_secs(30) {
                                i.state = "failed"; i.error = "Browser stream disconnected or timed out; keep the firmware page open".into();
                                return Err(ErrorCode::TxTimeout.into());
                            }
                            drop(image);
                            embassy_time::Timer::after_millis(20).await;
                            continue;
                        }
                    }
                } else {
                    i.file.read_at(&mut buf[..len], offset).map_err(|_| ErrorCode::InvalidData)?
                };
                if offset <= i.progress { i.progress = i.progress.max(offset+n as u64); }
                i.state = "transferring"; i.touched = Instant::now();
                return Ok(n);
            }
        }
    }
}
fn same_user(stream: &UnixStream) -> bool {
    use std::os::fd::AsRawFd;
    let mut cred: libc::ucred = unsafe { core::mem::zeroed() };
    let mut len = core::mem::size_of_val(&cred) as libc::socklen_t;
    unsafe {
        libc::getsockopt(
            stream.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            &mut cred as *mut _ as *mut _,
            &mut len,
        ) == 0
            && cred.uid == libc::geteuid()
    }
}
struct Client {
    stream: UnixStream,
    bytes: Vec<u8>,
    reply: Vec<u8>,
    sent: usize,
    deadline: Instant,
}
pub struct Control {
    listener: UnixListener,
    path: PathBuf,
    clients: Vec<Client>,
}
impl Control {
    pub fn bind(path: PathBuf) -> std::io::Result<Self> {
        // Match the main IPC socket's stale-path and ownership safeguards.
        if let Ok(meta) = std::fs::symlink_metadata(&path) {
            use std::os::unix::fs::FileTypeExt;
            if !meta.file_type().is_socket()
                || meta.uid() != unsafe { libc::geteuid() }
                || meta.mode() & 0o077 != 0
            {
                return Err(std::io::ErrorKind::PermissionDenied.into());
            }
            if crate::service::stale_socket(&path)? {
                std::fs::remove_file(&path)?;
            } else {
                return Err(std::io::ErrorKind::AddrInUse.into());
            }
        }
        let listener = UnixListener::bind(&path)?;
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
        listener.set_nonblocking(true)?;
        Ok(Self {
            listener,
            path,
            clients: Vec::new(),
        })
    }
    pub fn poll<C: Crypto>(&mut self, provider: &Provider, crypto: &C, catalog: &Catalog) {
        provider.expire();
        if let Ok((stream, _)) = self.listener.accept() {
            if self.clients.len() < 2 && same_user(&stream) && stream.set_nonblocking(true).is_ok()
            {
                self.clients.push(Client {
                    stream,
                    bytes: Vec::new(),
                    reply: Vec::new(),
                    sent: 0,
                    deadline: Instant::now() + Duration::from_secs(2),
                });
            }
        }
        self.clients.retain_mut(|c|{
            if Instant::now()>c.deadline{return false;}
            if c.reply.is_empty(){let mut b=[0;1024];match c.stream.read(&mut b){Ok(0)=>return false,Ok(n)=>c.bytes.extend_from_slice(&b[..n]),Err(e)if e.kind()==std::io::ErrorKind::WouldBlock=>return true,Err(_)=>return false}
                if c.bytes.len()<2{return true;}
                let length=u16::from_le_bytes([c.bytes[0],c.bytes[1]]) as usize;
                if length>ota_wire::MAX_FRAME||c.bytes.len()>length+2{return false;}
                if c.bytes.len()<length+2{return true;}
                let Some(cmd)=ota_wire::decode_request(&c.bytes) else{return false;};
                let error=match cmd.action {
                    1=>None,
                    2=>{provider.cancel();None},
                    3=>provider.start(&cmd.path,0,0,crypto).err().map(|_|"Invalid Matter OTA container or unsupported payload digest"),
                    5=>provider.start_source(&cmd.path,0,0,crypto,true).err().map(|_|"Invalid Matter streaming header"),
                    7=>provider.stream_chunk(&cmd.path).err().map(|_|"Invalid or stale browser stream chunk"),
                    4|6=>if catalog.nodes().iter().any(|n|n.id==cmd.node&&n.phase==Phase::Complete){provider.start_source(&cmd.path,cmd.node,cmd.endpoint,crypto,cmd.action==6).err().map(|_|"Invalid, incompatible, or unsupported Matter firmware image")}else{Some("Unknown Matter node")},
                    _=>Some("Invalid OTA command"),
                };
                let status=provider.status();
                let message=error.unwrap_or(&status.error);
                let Some(mut reply)=ota_wire::encode_response(if error.is_some(){""}else{status.state},message,status.progress,status.version.as_ref().and_then(|v|v.parse().ok()).unwrap_or(0),status.manufacturer.unwrap_or(0),status.image_type.unwrap_or(0)) else{return false;};
                let (offset, count) = provider.pending();
                reply.extend_from_slice(&offset.to_le_bytes());
                reply.extend_from_slice(&count.to_le_bytes());
                let len = (reply.len()-2) as u16;
                reply[..2].copy_from_slice(&len.to_le_bytes());
                c.reply=reply;
            }
            match c.stream.write(&c.reply[c.sent..]){Ok(0)=>false,Ok(n)=>{c.sent+=n;c.sent<c.reply.len()},Err(e)=>e.kind()==std::io::ErrorKind::WouldBlock}
        });
    }
}
impl Drop for Control {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

/// Accept only OTA traffic from the currently authorized secure-session peer.
/// Unsolicited reports remain owned by the existing subscription manager.
pub async fn run<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    provider: &Provider,
) -> Result<(), Error> {
    use rs_matter::{
        bdx::{Bdx, PROTO_ID_BDX},
        dm::networks::eth::EthNetwork,
        dm::{Dataver, Endpoint, Node},
        im::{IMBuffer, InteractionModel, InteractionModelState},
        persist::DummyKvBlobStore,
        respond::ExchangeHandler,
        utils::storage::pooled::PooledBuffers,
    };
    let buffers = PooledBuffers::<IMBuffer, 2>::new();
    let bdx_buffers = PooledBuffers::<BdxBuffer, 1>::new();
    let state = InteractionModelState::<_, 1, 128>::new(EthNetwork::new_default());
    let cluster = OtaProviderHandler::<&Provider>::CLUSTER;
    let clusters = [cluster];
    let endpoints = [Endpoint::new(0, &[], &clusters)];
    let node = Node {
        endpoints: &endpoints,
    };
    let kv = matter.kv(DummyKvBlobStore);
    let im = InteractionModel::new(
        matter,
        crypto,
        &buffers,
        (
            node,
            OtaProviderHandler::new(Dataver::new(0), provider).adapt(),
        ),
        &kv,
        &state,
    );
    let bdx = Bdx::new(OtaBdxHandler::new(&bdx_buffers, provider));
    loop {
        let exchange = Exchange::accept_matching(
            matter,
            rs_matter::im::PROTO_ID_INTERACTION_MODEL,
            rs_matter::im::OpCode::InvokeRequest as u8,
        );
        let bdx_exchange = Exchange::accept_matching(
            matter,
            PROTO_ID_BDX,
            rs_matter::bdx::OpCode::ReceiveInit as u8,
        );
        use embassy_futures::select::{select4, Either4};
        let sc_exchange = Exchange::accept_matching(
            matter,
            rs_matter::sc::PROTO_ID_SECURE_CHANNEL,
            rs_matter::sc::OpCode::CASESigma1 as u8,
        );
        let read_exchange = Exchange::accept_matching(
            matter,
            rs_matter::im::PROTO_ID_INTERACTION_MODEL,
            rs_matter::im::OpCode::ReadRequest as u8,
        );
        let (mut exchange, bdx_request) =
            match select4(exchange, bdx_exchange, sc_exchange, read_exchange).await {
                Either4::First(e) => (e?, false),
                Either4::Second(e) => (e?, true),
                Either4::Fourth(e) => (e?, false),
                Either4::Third(e) => {
                    let _ = rs_matter::sc::SecureChannel::new(crypto, &())
                        .handle(e?)
                        .await;
                    continue;
                }
            };
        if !provider.peer(exchange.peer_identity()?) {
            continue;
        }
        if bdx_request {
            let _ = bdx.handle(exchange).await;
        } else {
            // The OTA endpoint is transient. Grant access only for this command
            // and remove it before accepting another exchange; never persist it.
            use rs_matter::{
                acl::{AclEntry, AuthMode, Target},
                dm::Privilege,
            };
            let peer = exchange.peer_identity()?.unwrap().1;
            let index = matter.with_state(|s| {
                let mut acl =
                    AclEntry::new(Some(provider.fabric), Privilege::OPERATE, AuthMode::Case);
                acl.add_subject(peer)?;
                acl.add_target(Target::new(Some(0), Some(0x29), None))?;
                s.fabrics.fabric_mut(provider.fabric)?.acl_add(acl)
            })?;
            let _ = im.handle(&mut exchange).await;
            matter.with_state(|s| s.fabrics.fabric_mut(provider.fabric)?.acl_remove(index))?;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rs_matter::{
        crypto::{default_crypto, CryptoSensitiveRef},
        tlv::Octets,
    };
    fn ready<F: core::future::Future>(future: F) -> F::Output {
        let mut future = core::pin::pin!(future);
        let mut cx = core::task::Context::from_waker(std::task::Waker::noop());
        match future.as_mut().poll(&mut cx) {
            core::task::Poll::Ready(value) => value,
            _ => panic!("unexpected asynchronous I/O"),
        }
    }
    struct Fixture {
        path: String,
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
        }
    }
    fn fixture<C: Crypto>(crypto: &C) -> Fixture {
        fixture_payload(crypto, b"signed-vendor-image-payload")
    }
    fn fixture_payload<C: Crypto>(crypto: &C, payload: &[u8]) -> Fixture {
        let mut hash = crypto.hash().unwrap();
        hash.update(payload).unwrap();
        let mut digest = CryptoSensitive::new();
        hash.finish(&mut digest).unwrap();
        let header = OtaImageHeader {
            vendor_id: 0x1234,
            product_id: 0x5678,
            software_version: 2,
            software_version_str: "2",
            payload_size: payload.len() as u64,
            min_applicable_software_version: Some(1),
            max_applicable_software_version: None,
            release_notes_url: None,
            image_digest_type: OTA_IMAGE_DIGEST_SHA256,
            image_digest: Octets(digest.access()),
        };
        let mut bytes = [0; 512];
        let len = header.encode(&mut bytes).unwrap();
        let mut template = b"/tmp/microhad-ota-XXXXXX\0".to_vec();
        let fd = unsafe { libc::mkstemp(template.as_mut_ptr() as *mut _) };
        assert!(fd >= 0);
        use std::os::fd::FromRawFd;
        let mut file = unsafe { File::from_raw_fd(fd) };
        file.write_all(&bytes[..len]).unwrap();
        file.write_all(payload).unwrap();
        Fixture {
            path: String::from_utf8(template[..template.len() - 1].to_vec()).unwrap(),
        }
    }
    #[test]
    fn selected_peer_compatibility_tokens_and_cancellation() {
        let crypto = default_crypto(
            crate::entropy::OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[0; 32]),
        );
        let file = fixture(&crypto);
        let provider = Provider::new(NonZeroU8::new(1).unwrap());
        provider.start(&file.path, 2, 0, &crypto).unwrap();
        let peer = OtaRequestorId {
            fab_idx: NonZeroU8::new(1).unwrap(),
            node_id: 2,
        };
        let mut buf = [0; 192];
        assert!(matches!(
            ready(provider.query(peer, 0x1234, 0x5678, 2, false, &mut buf)),
            OtaQueryOutcome::NotAvailable
        ));
        assert!(matches!(
            ready(provider.query(peer, 0xabcd, 0x5678, 1, false, &mut buf)),
            OtaQueryOutcome::NotAvailable
        ));
        assert!(matches!(
            ready(provider.query(
                OtaRequestorId { node_id: 3, ..peer },
                0x1234,
                0x5678,
                1,
                false,
                &mut buf
            )),
            OtaQueryOutcome::NotAvailable
        ));
        assert!(!provider.peer(Some((2, 2))));
        assert!(!provider.peer(None));
        let (fd, token) = match ready(provider.query(peer, 0x1234, 0x5678, 1, false, &mut buf)) {
            OtaQueryOutcome::Available(m) => (
                m.file_designator.as_bytes().to_vec(),
                m.update_token.to_vec(),
            ),
            _ => panic!("image not offered"),
        };
        assert_eq!(ready(provider.size(b"unknown")), None);
        assert_eq!(
            ready(provider.apply(peer, &token, 2)),
            OtaApplyOutcome::Discontinue
        );
        let size = ready(provider.size(&fd)).unwrap();
        let mut bytes = [0; 32];
        ready(provider.read(&fd, size - 1, &mut bytes)).unwrap();
        assert_eq!(provider.status().progress, 0);
        let mut offset = 0;
        while offset < size {
            offset += ready(provider.read(&fd, offset, &mut bytes)).unwrap() as u64;
        }
        assert_eq!(
            ready(provider.apply(peer, b"wrong-token", 2)),
            OtaApplyOutcome::Discontinue
        );
        assert_eq!(
            ready(provider.apply(peer, &token, 2)),
            OtaApplyOutcome::Proceed { delay_secs: 0 }
        );
        ready(provider.applied(peer, &token, 1));
        assert_eq!(provider.status().state, "awaiting_confirmation");
        ready(provider.applied(peer, &token, 2));
        assert_eq!(provider.status().state, "complete");
        provider.cancel();
        assert_eq!(ready(provider.size(&fd)), None);
    }
    #[test]
    fn browser_stream_is_bounded_checks_digest_and_cancellation() {
        use core::future::Future;
        use core::task::{Context, Poll};
        let crypto = default_crypto(crate::entropy::OsEntropy::open().unwrap(), CryptoSensitiveRef::new(&[0;32]));
        for corrupt in [false, true] {
            let file = fixture(&crypto);
            let original = std::fs::read(&file.path).unwrap();
            let header_len = 16 + u32::from_le_bytes(original[12..16].try_into().unwrap()) as usize;
            OpenOptions::new().write(true).open(&file.path).unwrap().set_len(header_len as u64).unwrap();
            let provider = Provider::new(NonZeroU8::new(1).unwrap());
            provider.start_source(&file.path,2,0,&crypto,true).unwrap();
            let fd = provider.image.borrow().as_ref().unwrap().designator.clone();
            let id = file.path.strip_prefix("/tmp/microhad-ota-").unwrap();
            assert!(provider.stream_chunk(&format!("{id}:0:00")).is_err());
            let mut buf = [0;480];
            assert_eq!(ready(provider.read(fd.as_bytes(),0,&mut buf)).unwrap(),header_len);
            assert!(ready(provider.read(fd.as_bytes(),header_len as u64+1,&mut buf)).is_err());
            let mut read = core::pin::pin!(provider.read(fd.as_bytes(),header_len as u64,&mut buf));
            let mut cx = Context::from_waker(std::task::Waker::noop());
            assert!(matches!(read.as_mut().poll(&mut cx),Poll::Pending));
            let (offset,count) = provider.pending();
            assert_eq!(offset,header_len as u64);
            assert!(count>0&&count<=480);
            assert!(provider.stream_chunk(&format!("stale0:{offset}:00")).is_err());
            assert!(provider.stream_chunk(&format!("{id}:{}:00",offset+1)).is_err());
            let mut payload=original[header_len..].to_vec();
            if corrupt { payload[0]^=1; }
            let hex:String=payload.iter().map(|b|format!("{b:02x}")).collect();
            provider.stream_chunk(&format!("{id}:{offset}:{hex}")).unwrap();
            assert!(provider.stream_chunk(&format!("{id}:{offset}:{hex}")).is_err());
            std::thread::sleep(Duration::from_millis(25));
            let result=ready(read);
            if corrupt {
                assert!(result.is_err());assert_eq!(provider.status().state,"failed");
                assert!(provider.status().error.contains("digest"));
            } else {
                assert_eq!(result.unwrap(),payload.len());
                assert_eq!(provider.status().progress,original.len() as u64);
                assert_eq!(std::fs::metadata(&file.path).unwrap().len(),header_len as u64);
                assert_eq!(provider.pending(),(0,0));
            }
            provider.cancel();assert!(provider.stream_chunk(&format!("{id}:{offset}:{hex}")).is_err());
        }
    }
    #[test]
    fn browser_stream_multiple_chunks_and_download_restart() {
        use core::future::Future;
        use core::task::{Context, Poll};
        let crypto = default_crypto(crate::entropy::OsEntropy::open().unwrap(), CryptoSensitiveRef::new(&[0; 32]));
        let payload = vec![0x5a; 1450];
        let file = fixture_payload(&crypto, &payload);
        let original = std::fs::read(&file.path).unwrap();
        let header_len = 16 + u32::from_le_bytes(original[12..16].try_into().unwrap()) as usize;
        OpenOptions::new().write(true).open(&file.path).unwrap().set_len(header_len as u64).unwrap();
        let provider = Provider::new(NonZeroU8::new(1).unwrap());
        provider.start_source(&file.path, 2, 0, &crypto, true).unwrap();
        let fd = provider.image.borrow().as_ref().unwrap().designator.clone();
        let id = file.path.strip_prefix("/tmp/microhad-ota-").unwrap();
        let mut buf = [0; 1024];
        for restart in [false, true] {
            assert_eq!(ready(provider.read(fd.as_bytes(), 0, &mut buf)).unwrap(), header_len);
            let mut offset = header_len;
            loop {
                let mut read = core::pin::pin!(provider.read(fd.as_bytes(), offset as u64, &mut buf));
                let mut cx = Context::from_waker(std::task::Waker::noop());
                assert!(matches!(read.as_mut().poll(&mut cx), Poll::Pending));
                let (want, count) = provider.pending();
                assert_eq!(want, offset as u64); assert!(count > 0 && count <= 480);
                let hex = "5a".repeat(count as usize);
                provider.stream_chunk(&format!("{id}:{offset}:{hex}")).unwrap();
                std::thread::sleep(Duration::from_millis(25));
                offset += ready(read).unwrap();
                if !restart { break; } // Restart a partially delivered download at zero.
                if offset == original.len() { break; }
            }
            if restart { assert_eq!(provider.status().progress, original.len() as u64); }
        }
        assert_eq!(std::fs::metadata(&file.path).unwrap().len(), header_len as u64);
    }
    #[test]
    fn browser_stream_missing_browser_fails_and_cancel_wakes_reader() {
        use core::future::Future;
        use core::task::{Context,Poll};
        let crypto=default_crypto(crate::entropy::OsEntropy::open().unwrap(),CryptoSensitiveRef::new(&[0;32]));
        for cancel in [false,true] {
            let file=fixture(&crypto);let bytes=std::fs::read(&file.path).unwrap();
            let len=16+u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
            OpenOptions::new().write(true).open(&file.path).unwrap().set_len(len as u64).unwrap();
            let provider=Provider::new(NonZeroU8::new(1).unwrap());provider.start_source(&file.path,2,0,&crypto,true).unwrap();
            let fd=provider.image.borrow().as_ref().unwrap().designator.clone();let mut buf=[0;480];
            let mut read=core::pin::pin!(provider.read(fd.as_bytes(),len as u64,&mut buf));
            let mut cx=Context::from_waker(std::task::Waker::noop());assert!(matches!(read.as_mut().poll(&mut cx),Poll::Pending));
            if cancel { provider.cancel(); } else {
                provider.image.borrow_mut().as_mut().unwrap().stream.as_mut().unwrap().pending.as_mut().unwrap().2=Instant::now()-Duration::from_secs(31);
            }
            std::thread::sleep(Duration::from_millis(25));assert!(ready(read).is_err());
            if !cancel { assert!(provider.status().error.contains("timed out")); }
        }
    }
    #[test]
    fn malformed_digest_paths_and_timeout() {
        let crypto = default_crypto(
            crate::entropy::OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[0; 32]),
        );
        let file = fixture(&crypto);
        let provider = Provider::new(NonZeroU8::new(1).unwrap());
        assert!(provider.start("/etc/passwd", 2, 0, &crypto).is_err());
        provider.start(&file.path, 2, 0, &crypto).unwrap();
        assert!(provider.start(&file.path, 3, 0, &crypto).is_err());
        provider.image.borrow_mut().as_mut().unwrap().touched =
            Instant::now() - Duration::from_secs(3601);
        assert_eq!(provider.status().state, "failed");
        provider.cancel();
        let mut f = OpenOptions::new().append(true).open(&file.path).unwrap();
        f.write_all(b"corruption").unwrap();
        assert!(provider.start(&file.path, 2, 0, &crypto).is_err());
    }
}
