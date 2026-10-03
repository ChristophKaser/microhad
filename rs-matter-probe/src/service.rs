//! Single-owner, nonblocking, bounded Unix IPC listener. Payloads have no logging path.
use crate::ipc::{Decoder, Frame, MAX_CHUNK, MAX_PAYLOAD};
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
        })
    }
    pub fn poll(&mut self, mut dispatch: impl FnMut(Frame) -> Option<Frame>) -> io::Result<()> {
        // Bound accepts per turn as well as resident clients so a connection flood
        // cannot starve OpenThread, Matter timers, or another peer.
        for _ in 0..MAX_CLIENTS {
            match self.listener.accept() {
                Ok((stream, _)) => {
                    if self.clients.len() < MAX_CLIENTS {
                        self.clients.push(Client::new(stream)?);
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
        self.clients
            .retain_mut(|client| client.poll(&mut dispatch).unwrap_or(false));
        Ok(())
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
            payload: vec![],
        }
        .encode()
        .unwrap()
    }
    fn respond(request: Frame) -> Option<Frame> {
        Some(Frame {
            kind: 1,
            opcode: request.opcode,
            id: request.id,
            payload: b"{\"available\":true}".to_vec(),
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
            payload: vec![],
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
}
