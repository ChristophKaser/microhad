//! Isolated synthetic producer benchmark. No radio or production syslog access.
use micro_thread_matter_daemon::logging::capture::Capture;
use std::io::Write;
use std::net::{SocketAddr, UdpSocket};
use std::sync::mpsc;
use std::time::{Duration, Instant};

fn cpu_us() -> i64 {
    unsafe {
        let mut usage = std::mem::zeroed::<libc::rusage>();
        assert_eq!(libc::getrusage(libc::RUSAGE_SELF, &mut usage), 0);
        (usage.ru_utime.tv_sec as i64 + usage.ru_stime.tv_sec as i64) * 1_000_000
            + usage.ru_utime.tv_usec as i64 + usage.ru_stime.tv_usec as i64
    }
}

fn main() {
    #[cfg(all(target_arch = "mips", target_env = "uclibc"))]
    ziggurat_stock_uclibc_compat::link();
    let args: Vec<_> = std::env::args().collect();
    let mode = &args[1];
    let destination: SocketAddr = args[2].parse().unwrap();
    let count: usize = args[3].parse().unwrap();
    let pace_us: u64 = args[4].parse().unwrap();
    let mut latencies = Vec::with_capacity(count);
    let data: Vec<u8> = (0..1232).map(|n| n as u8).collect();
    let mut udp = None;
    let mut baseline = None;
    let mut worker = None;
    if mode == "udp" {
        udp = Some(Capture::start(destination, 2).unwrap());
    } else if mode == "text" {
        let (tx, rx) = mpsc::sync_channel::<Vec<u8>>(64);
        baseline = Some(tx);
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket.connect(destination).unwrap();
        worker = Some(std::thread::spawn(move || {
            for entry in rx {
                for chunk in entry.chunks(220) {
                    let mut frame = [0; 224];
                    frame[..4].copy_from_slice(b"<31>");
                    frame[4..4 + chunk.len()].copy_from_slice(chunk);
                    let _ = socket.send(&frame[..4 + chunk.len()]);
                }
            }
        }));
    } else {
        assert_eq!(mode, "off");
    }
    let start = Instant::now();
    let cpu_start = cpu_us();
    let mut rejected = 0;
    for index in 0..count {
        let len = [85, 125, 240, 510, 1232][index % 5];
        let before = Instant::now();
        if let Some(capture) = udp.as_mut() {
            let mut metadata = [0u8; 22];
            metadata[..8].copy_from_slice(&(index as u64).to_le_bytes());
            capture.record(1, &metadata, &data[..len]);
        } else if let Some(sender) = baseline.as_ref() {
            let mut line = Vec::new();
            write!(
                line,
                "TRACE matter_raw: record={index} RX peer=UDP [::1]:5540 length={len} payload={:02X?}",
                &data[..len]
            )
            .unwrap();
            if sender.try_send(line).is_err() {
                rejected += 1;
            }
        }
        latencies.push(before.elapsed().as_nanos());
        if pace_us > 0 {
            let next = start + Duration::from_micros((index as u64 + 1) * pace_us);
            if let Some(delay) = next.checked_duration_since(Instant::now()) {
                std::thread::sleep(delay);
            }
        }
    }
    drop(udp);
    drop(baseline);
    if let Some(worker) = worker {
        worker.join().unwrap();
    }
    let cpu = cpu_us() - cpu_start;
    let elapsed = start.elapsed();
    latencies.sort_unstable();
    println!(
        "{mode} count={count} cpu_us={cpu} p95_ns={} p99_ns={} elapsed_us={} text_queue_drops={rejected}",
        latencies[(count - 1) * 95 / 100],
        latencies[(count - 1) * 99 / 100],
        elapsed.as_micros(),
    );
}
