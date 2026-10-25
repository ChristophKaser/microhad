//! Single-owner, nonblocking, bounded Unix IPC listener. Payloads have no logging path.
use crate::ipc::{Decoder, Frame, MAX_CHUNK, MAX_PAYLOAD};
use rs_matter::error::Error;
use rs_matter::sc::{AsyncScHandler, OpCode, PROTO_ID_SECURE_CHANNEL};
use rs_matter::transport::exchange::Exchange;
use rs_matter::Matter;
use std::collections::VecDeque;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub const MAX_CLIENTS: usize = 4;
const MAX_RESPONSES: usize = 8;
const REQUEST_DEADLINE: Duration = Duration::from_secs(2);

/// Accept only established-session ICD Check-Ins. Sessionless Check-Ins are
/// captured by the transport because they have no authenticated peer identity.
/// Matching at the transport layer leaves unsolicited Interaction Model
/// reports available to the device subscription manager.
pub struct IcdCheckInResponder<'a> {
    matter: &'a Matter<'a>,
}

impl<'a> IcdCheckInResponder<'a> {
    pub const fn new(matter: &'a Matter<'a>) -> Self {
        Self { matter }
    }
}

impl IcdCheckInResponder<'_> {
    pub async fn run(&self) -> Result<(), Error> {
        loop {
            let exchange = Exchange::accept_matching(
                self.matter,
                PROTO_ID_SECURE_CHANNEL,
                OpCode::CheckIn as u8,
            )
            .await?;
            // A malformed or unauthenticated packet must not terminate the
            // long-running service task; the transport already logs failures.
            let _ = <Matter<'_> as AsyncScHandler>::check_in(self.matter, exchange).await;
        }
    }
}

fn stale_socket(path: &Path) -> io::Result<bool> {
    // A blocking connect can hang on a live listener with a full backlog.
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let result = (|| {
        if unsafe { libc::fcntl(fd, libc::F_SETFL, libc::O_NONBLOCK) } < 0 {
            return Err(io::Error::last_os_error());
        }
        let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
        address.sun_family = libc::AF_UNIX as libc::sa_family_t;
        for (dst, src) in address.sun_path.iter_mut().zip(path.as_os_str().as_bytes()) {
            *dst = *src as libc::c_char;
        }
        let connected = unsafe {
            libc::connect(
                fd,
                &address as *const _ as *const libc::sockaddr,
                std::mem::size_of_val(&address) as libc::socklen_t,
            )
        };
        if connected == 0 {
            return Ok(false);
        }
        let error = io::Error::last_os_error();
        match error.raw_os_error() {
            Some(libc::ECONNREFUSED) => Ok(true),
            Some(libc::EAGAIN) | Some(libc::EINPROGRESS) => Ok(false),
            _ => Err(error),
        }
    })();
    unsafe { libc::close(fd) };
    result
}

fn private_file(path: &Path, socket: bool) -> io::Result<fs::Metadata> {
    let meta = fs::symlink_metadata(path)?;
    if meta.uid() != unsafe { libc::geteuid() }
        || meta.mode() & 0o077 != 0
        || if socket {
            !meta.file_type().is_socket()
        } else {
            !meta.is_file()
        }
    {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "unsafe IPC path",
        ));
    }
    Ok(meta)
}

struct Client {
    id: u64,
    stream: UnixStream,
    decoder: Decoder,
    responses: VecDeque<Vec<u8>>,
    written: usize,
    deadline: Instant,
    eof: bool,
}
impl Client {
    fn new(stream: UnixStream) -> io::Result<Self> {
        stream.set_nonblocking(true)?;
        Ok(Self {
            id: 0,
            stream,
            decoder: Decoder::default(),
            responses: VecDeque::new(),
            written: 0,
            deadline: Instant::now() + REQUEST_DEADLINE,
            eof: false,
        })
    }
    fn poll(&mut self, dispatch: &mut impl FnMut(Frame) -> Option<Frame>) -> io::Result<bool> {
        if Instant::now() >= self.deadline {
            return Ok(false);
        }
        let mut bytes = [0; MAX_CHUNK];
        if !self.eof {
            match self.stream.read(&mut bytes) {
                Ok(0) => self.eof = true,
                Ok(length) => self
                    .decoder
                    .feed(&bytes[..length])
                    .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid IPC frame"))?,
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) =>
                {
                    ()
                }
                Err(error) => return Err(error),
            }
        }
        while let Some(request) = self.decoder.pop() {
            if request.kind != 0 || self.responses.len() >= MAX_RESPONSES {
                return Ok(false);
            }
            let id = request.id;
            let opcode = request.opcode;
            let Some(response) = dispatch(request) else {
                return Ok(false);
            };
            if response.kind != 1
                || response.id != id
                || response.opcode != opcode
                || response.payload.len() > MAX_PAYLOAD
            {
                return Ok(false);
            }
            let encoded = response
                .encode()
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid IPC response"))?;
            self.responses.push_back(encoded);
            // Partial input or a slow writer cannot extend this deadline.
            self.deadline = Instant::now() + REQUEST_DEADLINE;
        }
        let mut budget = MAX_CHUNK;
        while let Some(response) = self.responses.front() {
            if budget == 0 {
                break;
            }
            let end = response.len().min(self.written + budget);
            match self.stream.write(&response[self.written..end]) {
                Ok(0) => return Ok(false),
                Ok(length) => {
                    self.written += length;
                    budget -= length;
                    if self.written == response.len() {
                        self.responses.pop_front();
                        self.written = 0;
                    }
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) =>
                {
                    break
                }
                Err(error) => return Err(error),
            }
        }
        Ok(!self.eof || !self.responses.is_empty())
    }
}

pub struct IpcServer {
    listener: UnixListener,
    _lock: File,
    path: PathBuf,
    identity: (u64, u64),
    clients: Vec<Client>,
    next_client: u64,
}
impl IpcServer {
    pub fn bind(path: &Path) -> io::Result<Self> {
        if !path.is_absolute() || path.as_os_str().len() > 107 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid IPC path",
            ));
        }
        let mut lock_path = path.as_os_str().to_os_string();
        lock_path.push(".lock");
        let lock_path = PathBuf::from(lock_path);
        match fs::symlink_metadata(&lock_path) {
            Ok(_) => {
                private_file(&lock_path, false)?;
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => (),
            Err(error) => return Err(error),
        }
        let lock = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(&lock_path)?;
        private_file(&lock_path, false)?;
        if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
            return Err(io::Error::new(io::ErrorKind::AddrInUse, "IPC owner active"));
        }
        match fs::symlink_metadata(path) {
            Ok(_) => {
                private_file(path, true)?;
                if !stale_socket(path)? {
                    return Err(io::Error::new(
                        io::ErrorKind::AddrInUse,
                        "IPC listener active",
                    ));
                }
                fs::remove_file(path)?;
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => (),
            Err(error) => return Err(error),
        }
        let listener = UnixListener::bind(path)?;
        fs::set_permissions(path, fs::Permissions::from_mode(0o600))?;
        listener.set_nonblocking(true)?;
        let meta = private_file(path, true)?;
        Ok(Self {
            listener,
            _lock: lock,
            path: path.into(),
            identity: (meta.dev(), meta.ino()),
            clients: Vec::new(),
            next_client: 1,
        })
    }
    pub fn poll(&mut self, mut dispatch: impl FnMut(Frame) -> Option<Frame>) -> io::Result<()> {
        self.poll_with_peer(|_, request| dispatch(request))
    }
    /// Peer IDs are connection lifetimes, never administrator authentication.
    /// A reused IPC correlation cannot target a retired connection.
    pub fn poll_with_peer(
        &mut self,
        mut dispatch: impl FnMut(u64, Frame) -> Option<Frame>,
    ) -> io::Result<()> {
        // Bound accepts per turn as well as resident clients so a connection flood
        // cannot starve OpenThread, Matter timers, or another peer.
        for _ in 0..MAX_CLIENTS {
            match self.listener.accept() {
                Ok((stream, _)) => {
                    if self.clients.len() < MAX_CLIENTS {
                        let next = self.next_client.checked_add(1).ok_or_else(|| {
                            io::Error::new(io::ErrorKind::Other, "IPC peer IDs exhausted")
                        })?;
                        let mut client = Client::new(stream)?;
                        client.id = self.next_client;
                        self.next_client = next;
                        self.clients.push(client);
                    }
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) =>
                {
                    break
                }
                Err(error) => return Err(error),
            }
        }
        self.clients.retain_mut(|client| {
            let id = client.id;
            client
                .poll(&mut |request| dispatch(id, request))
                .unwrap_or(false)
        });
        Ok(())
    }
    pub fn has_peer(&self, peer: u64) -> bool {
        self.clients
            .iter()
            .any(|client| client.id == peer && !client.eof && Instant::now() < client.deadline)
    }
    /// Queue an event only for its explicitly selected peer. Backpressure drops
    /// that peer; it never drops another client or renews the request deadline.
    /// The caller must poll again to flush the bounded output queue.
    pub fn publish(&mut self, peer: u64, event: &Frame) -> io::Result<bool> {
        if event.kind != 2 || event.opcode != 12 || event.id != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid IPC event",
            ));
        }
        let encoded = event
            .encode()
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid IPC event"))?;
        let Some(index) = self.clients.iter().position(|client| client.id == peer) else {
            return Ok(false);
        };
        if self.clients[index].eof
            || Instant::now() >= self.clients[index].deadline
            || self.clients[index].responses.len() >= MAX_RESPONSES
        {
            self.clients.remove(index);
            return Ok(false);
        }
        self.clients[index].responses.push_back(encoded);
        Ok(true)
    }
    /// Inventory has its own paced publisher. Reserve room for replies and
    /// commissioning events and retry later instead of disconnecting a peer.
    pub fn publish_inventory(&mut self, peer: u64, event: &Frame) -> io::Result<bool> {
        if !self.clients.iter().any(|client| client.id == peer && client.responses.len() < 2) {
            return Ok(false);
        }
        self.publish(peer, event)
    }
    /// Broadcast a validated state event to every live IPC client. Clients with
    /// expired deadlines or full output queues are retired independently.
    pub fn publish_all(&mut self, event: &Frame) -> io::Result<usize> {
        if event.kind != 2 || event.opcode != 12 || event.id != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid IPC event",
            ));
        }
        let encoded = event
            .encode()
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid IPC event"))?;
        let now = Instant::now();
        let mut published = 0;
        self.clients.retain_mut(|client| {
            if client.eof || now >= client.deadline || client.responses.len() >= MAX_RESPONSES {
                return false;
            }
            client.responses.push_back(encoded.clone());
            published += 1;
            true
        });
        Ok(published)
    }
}
impl Drop for IpcServer {
    fn drop(&mut self) {
        if let Ok(meta) = fs::symlink_metadata(&self.path) {
            if (meta.dev(), meta.ino()) == self.identity && meta.file_type().is_socket() {
                let _ = fs::remove_file(&self.path);
            }
        }
        // Retain the lock file: deleting a held flock would allow a new owner
        // to create a different inode while the old service is still unwinding.
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(0);
    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "matter-ipc-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
            Self(path)
        }
        fn socket(&self) -> PathBuf {
            self.0.join("ipc.sock")
        }
    }
    impl Drop for Directory {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn request(id: u32) -> Vec<u8> {
        Frame {
            kind: 0,
            opcode: 1,
            id,
            payload: crate::wire_json::from_json(0, 1, b"").unwrap(),
        }
        .encode()
        .unwrap()
    }
    fn respond(request: Frame) -> Option<Frame> {
        Some(Frame {
            kind: 1,
            opcode: request.opcode,
            id: request.id,
            payload: crate::wire_json::from_json(1, 1, b"{\"available\":true}").unwrap(),
        })
    }
    #[test]
    fn permissions_duplicate_owner_and_cleanup() {
        let directory = Directory::new();
        let path = directory.socket();
        let server = IpcServer::bind(&path).unwrap();
        assert_eq!(fs::metadata(&path).unwrap().mode() & 0o777, 0o600);
        assert!(IpcServer::bind(&path).is_err());
        drop(server);
        assert!(!path.exists());
        assert!(IpcServer::bind(&path).is_ok());
    }
    #[test]
    fn stale_only_owned_socket_can_be_replaced() {
        let directory = Directory::new();
        let path = directory.socket();
        let listener = UnixListener::bind(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        assert!(IpcServer::bind(&path).is_err());
        drop(listener);
        let server = IpcServer::bind(&path).unwrap();
        drop(server);
        fs::write(&path, b"keep").unwrap();
        assert!(IpcServer::bind(&path).is_err());
        assert_eq!(fs::read(&path).unwrap(), b"keep");
        fs::remove_file(&path).unwrap();
        std::os::unix::fs::symlink(directory.0.join("absent"), &path).unwrap();
        assert!(IpcServer::bind(&path).is_err());
    }
    #[test]
    fn fragmented_and_coalesced_requests_preserve_ids() {
        let directory = Directory::new();
        let path = directory.socket();
        let mut server = IpcServer::bind(&path).unwrap();
        let mut peer = UnixStream::connect(&path).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        let first = request(91);
        peer.write_all(&first[..3]).unwrap();
        server.poll(respond).unwrap();
        assert!(server.clients[0].responses.is_empty());
        let mut remainder = first[3..].to_vec();
        remainder.extend(request(92));
        peer.write_all(&remainder).unwrap();
        server.poll(respond).unwrap();
        let length = respond(Frame {
            kind: 0,
            opcode: 1,
            id: 91,
            payload: crate::wire_json::from_json(0, 1, b"").unwrap(),
        })
        .unwrap()
        .encode()
        .unwrap()
        .len();
        let mut bytes = vec![0; length * 2];
        peer.read_exact(&mut bytes).unwrap();
        let mut decoder = Decoder::default();
        decoder.feed(&bytes).unwrap();
        assert_eq!(decoder.pop().unwrap().id, 91);
        assert_eq!(decoder.pop().unwrap().id, 92);
    }
    #[test]
    fn malformed_overloaded_and_idle_clients_are_closed() {
        for mode in 0..3 {
            let (mut peer, stream) = UnixStream::pair().unwrap();
            let mut client = Client::new(stream).unwrap();
            match mode {
                0 => {
                    let mut bytes = request(1);
                    bytes[2] = 2;
                    peer.write_all(&bytes).unwrap();
                }
                1 => {
                    let bytes: Vec<_> = (1..=9).flat_map(request).collect();
                    peer.write_all(&bytes).unwrap();
                }
                _ => {
                    peer.write_all(&request(1)[..1]).unwrap();
                    client.deadline = Instant::now();
                }
            }
            assert!(!client.poll(&mut respond).unwrap_or(false));
        }
    }
    #[test]
    fn client_capacity_and_response_identity_are_enforced() {
        let directory = Directory::new();
        let mut server = IpcServer::bind(&directory.socket()).unwrap();
        let _peers: Vec<_> = (0..8)
            .map(|_| UnixStream::connect(directory.socket()).unwrap())
            .collect();
        server.poll(respond).unwrap();
        server.poll(respond).unwrap();
        assert_eq!(server.clients.len(), MAX_CLIENTS);
        let (mut peer, stream) = UnixStream::pair().unwrap();
        let mut client = Client::new(stream).unwrap();
        peer.write_all(&request(4)).unwrap();
        assert!(!client
            .poll(&mut |r| {
                let mut response = respond(r).unwrap();
                response.id += 1;
                Some(response)
            })
            .unwrap());
    }
    #[test]
    fn shutdown_does_not_delete_a_replacement_path() {
        let directory = Directory::new();
        let path = directory.socket();
        let server = IpcServer::bind(&path).unwrap();
        fs::remove_file(&path).unwrap();
        fs::write(&path, b"replacement").unwrap();
        drop(server);
        assert_eq!(fs::read(&path).unwrap(), b"replacement");
    }
    #[test]
    fn targeted_events_follow_replies_without_broadcast_or_deadline_extension() {
        let directory = Directory::new();
        let mut server = IpcServer::bind(&directory.socket()).unwrap();
        let mut first = UnixStream::connect(directory.socket()).unwrap();
        let mut second = UnixStream::connect(directory.socket()).unwrap();
        first
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        second
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        first.write_all(&request(1)).unwrap();
        second.write_all(&request(2)).unwrap();
        let mut peers = vec![];
        server
            .poll_with_peer(|id, frame| {
                peers.push((frame.id, id));
                respond(frame)
            })
            .unwrap();
        assert_eq!(peers.len(), 2);
        assert_ne!(peers[0].1, peers[1].1);
        let target = peers.iter().find(|p| p.0 == 1).unwrap().1;
        let deadline = server
            .clients
            .iter()
            .find(|c| c.id == target)
            .unwrap()
            .deadline;
        let event = Frame {
            kind: 2,
            opcode: 12,
            id: 0,
            payload: crate::wire_json::from_json(
                2,
                12,
                br#"{"schema":1,"event":"thread","data":{}}"#,
            )
            .unwrap(),
        };
        assert!(server.publish(target, &event).unwrap());
        assert_eq!(
            server
                .clients
                .iter()
                .find(|c| c.id == target)
                .unwrap()
                .deadline,
            deadline
        );
        server.poll(respond).unwrap();
        let expected_reply = respond(Frame {
            kind: 0,
            opcode: 1,
            id: 1,
            payload: crate::wire_json::from_json(0, 1, b"").unwrap(),
        })
        .unwrap()
        .encode()
        .unwrap();
        let mut received = vec![0; expected_reply.len() + event.encode().unwrap().len()];
        first.read_exact(&mut received).unwrap();
        let mut decoder = Decoder::default();
        decoder.feed(&received).unwrap();
        assert_eq!(decoder.pop().unwrap().id, 1);
        let delivered = decoder.pop().unwrap();
        assert_eq!(delivered.kind, 2);
        assert_eq!(serde_json::from_slice::<serde_json::Value>(&crate::wire_json::json_bytes(
                &delivered.payload
            ))
            .unwrap(),
            serde_json::from_slice::<serde_json::Value>(&crate::wire_json::json_bytes(
                &event.payload
            )).unwrap());
        let mut second_reply = vec![0; expected_reply.len()];
        second.read_exact(&mut second_reply).unwrap();
        second.set_nonblocking(true).unwrap();
        assert_eq!(
            second.read(&mut [0; 1]).unwrap_err().kind(),
            io::ErrorKind::WouldBlock
        );
    }

    #[test]
    fn matter_readings_broadcast_to_connected_native_clients() {
        let directory = Directory::new();
        let mut server = IpcServer::bind(&directory.socket()).unwrap();
        let mut first = UnixStream::connect(directory.socket()).unwrap();
        let mut second = UnixStream::connect(directory.socket()).unwrap();
        first
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        second
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        first.write_all(&request(1)).unwrap();
        second.write_all(&request(2)).unwrap();
        server.poll(respond).unwrap();
        let event = Frame {
            kind: 2,
            opcode: 12,
            id: 0,
            payload: crate::wire_json::from_json(
                2,
                12,
                br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002"}}"#,
            )
            .unwrap(),
        };
        assert_eq!(server.publish_all(&event).unwrap(), 2);
        server.poll(respond).unwrap();
        let reply_len = respond(Frame {
            kind: 0,
            opcode: 1,
            id: 1,
            payload: crate::wire_json::from_json(0, 1, b"").unwrap(),
        })
        .unwrap()
        .encode()
        .unwrap()
        .len();
        for (peer, id) in [(&mut first, 1), (&mut second, 2)] {
            let mut bytes = vec![0; event.encode().unwrap().len() + reply_len];
            peer.read_exact(&mut bytes).unwrap();
            let mut decoder = Decoder::default();
            decoder.feed(&bytes).unwrap();
            assert_eq!(decoder.pop().unwrap().id, id);
            assert_eq!(serde_json::from_slice::<serde_json::Value>(&crate::wire_json::json_bytes(
                    &decoder.pop().unwrap().payload
                ))
                .unwrap(),
                serde_json::from_slice::<serde_json::Value>(&crate::wire_json::json_bytes(
                    &event.payload
                )).unwrap());
        }
    }

    #[test]
    fn event_backpressure_retired_peers_and_expiry_are_isolated() {
        let directory = Directory::new();
        let mut server = IpcServer::bind(&directory.socket()).unwrap();
        let _first = UnixStream::connect(directory.socket()).unwrap();
        let _second = UnixStream::connect(directory.socket()).unwrap();
        server.poll(respond).unwrap();
        let first = server.clients[0].id;
        let second = server.clients[1].id;
        let event = Frame {
            kind: 2,
            opcode: 12,
            id: 0,
            payload: crate::wire_json::from_json(
                2,
                12,
                br#"{"schema":1,"event":"thread","data":{}}"#,
            )
            .unwrap(),
        };
        for _ in 0..MAX_RESPONSES {
            assert!(server.publish(first, &event).unwrap());
        }
        assert!(!server.publish(first, &event).unwrap());
        assert!(!server.has_peer(first));
        assert!(server.has_peer(second));
        let _replacement = UnixStream::connect(directory.socket()).unwrap();
        server.poll(respond).unwrap();
        assert!(server.clients.iter().any(|c| c.id > second));
        assert!(!server.publish(first, &event).unwrap());
        let mut malformed = Frame {
            kind: 2,
            opcode: 12,
            id: 1,
            payload: crate::wire_json::from_json(0, 1, b"").unwrap(),
        };
        assert!(server.publish(second, &malformed).is_err());
        malformed.id = 0;
        malformed.payload = crate::protobuf::MessageBody {
            choice: Some(crate::protobuf::MessageBodyChoice::Event(
                crate::protobuf::EventNotification {
                    event: Some("x".repeat(MAX_PAYLOAD + 1)),
                    ..Default::default()
                },
            )),
        };
        assert!(server.publish(second, &malformed).is_err());
        assert!(server.has_peer(second));
        server
            .clients
            .iter_mut()
            .find(|c| c.id == second)
            .unwrap()
            .deadline = Instant::now();
        assert!(!server.has_peer(second));
        assert!(!server.publish(second, &event).unwrap());
        server.next_client = u64::MAX;
        let _exhausted = UnixStream::connect(directory.socket()).unwrap();
        assert!(server.poll(respond).is_err());
        assert_eq!(server.next_client, u64::MAX);
    }
}
