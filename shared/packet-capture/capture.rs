//! Binary packet capture transport shared by the gateway services.
use std::io;
use std::prelude::rust_2024::*;
use std::vec;
use std::net::{SocketAddr, UdpSocket};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender, TrySendError};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const RECORD_BYTES: usize = 4096;
const QUEUE_DEPTH: usize = 64;
const CHUNK_BYTES: usize = 1152;

struct Record {
    kind: u8,
    sequence: u64,
    timestamp: u64,
    bytes: Vec<u8>,
}

pub struct Capture {
    available: Receiver<Vec<u8>>,
    pending: Option<SyncSender<Record>>,
    dropped: Arc<AtomicU32>,
    sequence: u64,
    worker: Option<thread::JoinHandle<()>>,
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_micros() as u64
}

fn send(socket: &UdpSocket, producer: u8, run: u64, record: &Record) -> bool {
    let mut datagram = [0; 1200];
    datagram[..4].copy_from_slice(b"GPC1");
    datagram[4] = producer;
    datagram[5] = record.kind;
    datagram[8..16].copy_from_slice(&run.to_le_bytes());
    datagram[16..24].copy_from_slice(&record.sequence.to_le_bytes());
    datagram[24..32].copy_from_slice(&record.timestamp.to_le_bytes());
    datagram[32..36].copy_from_slice(&(record.bytes.len() as u32).to_le_bytes());
    for (index, chunk) in record.bytes.chunks(CHUNK_BYTES).enumerate() {
        datagram[36..40].copy_from_slice(&((index * CHUNK_BYTES) as u32).to_le_bytes());
        datagram[48..48 + chunk.len()].copy_from_slice(chunk);
        if socket.send(&datagram[..48 + chunk.len()]).is_err() {
            return false;
        }
    }
    true
}

impl Capture {
    pub fn start(destination: SocketAddr, producer: u8) -> io::Result<Self> {
        let bind = if destination.is_ipv4() {
            "0.0.0.0:0"
        } else {
            "[::]:0"
        };
        let socket = UdpSocket::bind(bind)?;
        socket.connect(destination)?;
        socket.set_nonblocking(true)?;

        let (recycle, available) = mpsc::sync_channel(QUEUE_DEPTH);
        let (pending, records) = mpsc::sync_channel::<Record>(QUEUE_DEPTH);
        for _ in 0..QUEUE_DEPTH {
            recycle.send(Vec::with_capacity(RECORD_BYTES)).unwrap();
        }
        let dropped = Arc::new(AtomicU32::new(0));
        let worker_dropped = dropped.clone();
        let run = now();
        let worker = thread::Builder::new()
            .name("packet-capture".into())
            .spawn(move || {
                let period = Duration::from_secs(1);
                let mut report_at = std::time::Instant::now() + period;
                let mut send_failures = 0u64;
                let mut report = Record {
                    kind: 0,
                    sequence: 0,
                    timestamp: 0,
                    bytes: vec![0; 16],
                };
                loop {
                    let mut shutdown = false;
                    match records.recv_timeout(
                        report_at.saturating_duration_since(std::time::Instant::now()),
                    ) {
                        Ok(record) => {
                            if !send(&socket, producer, run, &record) {
                                send_failures += 1;
                            }
                            let _ = recycle.try_send(record.bytes);
                        }
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                        Err(mpsc::RecvTimeoutError::Disconnected) => shutdown = true,
                    }
                    if shutdown || std::time::Instant::now() >= report_at {
                        report.timestamp = now();
                        report.bytes[..8].copy_from_slice(
                            &u64::from(worker_dropped.load(Ordering::Relaxed)).to_le_bytes(),
                        );
                        report.bytes[8..].copy_from_slice(&send_failures.to_le_bytes());
                        send(&socket, producer, run, &report);
                        report_at = std::time::Instant::now() + period;
                    }
                    if shutdown {
                        break;
                    }
                }
            })?;
        Ok(Self {
            available,
            pending: Some(pending),
            dropped,
            sequence: 0,
            worker: Some(worker),
        })
    }

    /// Kind zero is reserved for loss reports.
    pub fn record(&mut self, kind: u8, metadata: &[u8], payload: &[u8]) {
        self.sequence += 1;
        if kind == 0
            || metadata.len().saturating_add(payload.len()) > RECORD_BYTES
            || (metadata.is_empty() && payload.is_empty())
        {
            self.dropped.fetch_add(1, Ordering::Relaxed);
            return;
        }
        let Ok(mut bytes) = self.available.try_recv() else {
            self.dropped.fetch_add(1, Ordering::Relaxed);
            return;
        };
        bytes.clear();
        bytes.extend_from_slice(metadata);
        bytes.extend_from_slice(payload);
        let record = Record {
            kind,
            sequence: self.sequence,
            timestamp: now(),
            bytes,
        };
        if let Err(TrySendError::Disconnected(_) | TrySendError::Full(_)) =
            self.pending.as_ref().unwrap().try_send(record)
        {
            self.dropped.fetch_add(1, Ordering::Relaxed);
        }
    }
}

impl Drop for Capture {
    fn drop(&mut self) {
        self.pending.take();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

static GLOBAL: std::sync::OnceLock<std::sync::Mutex<Option<Capture>>> = std::sync::OnceLock::new();

pub struct CaptureGuard;

pub fn configure(destination: SocketAddr, producer: u8) -> io::Result<CaptureGuard> {
    let mut slot = GLOBAL.get_or_init(Default::default).lock().unwrap();
    if slot.is_some() {
        return Err(io::Error::new(io::ErrorKind::AlreadyExists, "capture already configured"));
    }
    *slot = Some(Capture::start(destination, producer)?);
    Ok(CaptureGuard)
}

impl Drop for CaptureGuard {
    fn drop(&mut self) {
        let capture = GLOBAL.get().unwrap().lock().unwrap().take();
        drop(capture);
    }
}

pub fn record(kind: u8, metadata: &[u8], payload: &[u8]) {
    if let Some(slot) = GLOBAL.get() {
        if let Some(capture) = slot.lock().unwrap().as_mut() {
            capture.record(kind, metadata, payload);
        }
    }
}

/// Format decoded records only when capture is active.
pub fn record_debug(kind: u8, arguments: std::fmt::Arguments<'_>) {
    if let Some(slot) = GLOBAL.get() {
        if let Some(capture) = slot.lock().unwrap().as_mut() {
            capture.record(kind, &[], arguments.to_string().as_bytes());
        }
    }
}
