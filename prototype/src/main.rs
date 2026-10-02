use std::fs::File;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::thread;
use std::time::{Duration, Instant};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    assert_eq!(lexra_rust_prototype::probe_add(u32::MAX, 2), 1);
    assert_eq!(
        lexra_rust_prototype::probe_decode_le(&[1, 2, 3, 4]),
        0x04030201
    );
    println!("PASS arithmetic and explicit byte order");

    let bytes: Vec<u8> = (0..65_536).map(|i| (i % 251) as u8).collect();
    assert_eq!(bytes.iter().map(|&v| u64::from(v)).sum::<u64>(), 8_189_175);
    println!("PASS heap allocation (64 KiB)");

    let counter = Arc::new(AtomicU32::new(0));
    let workers: Vec<_> = (0..2)
        .map(|_| {
            let counter = Arc::clone(&counter);
            thread::spawn(move || {
                for _ in 0..10_000 {
                    lexra_rust_prototype::probe_atomic_increment(&counter);
                }
            })
        })
        .collect();
    for worker in workers {
        worker.join().expect("counter worker panicked");
    }
    assert_eq!(counter.load(Ordering::SeqCst), 20_000);
    println!("PASS threads and atomic read-modify-write");

    let start = Instant::now();
    thread::sleep(Duration::from_millis(20));
    assert!(start.elapsed() >= Duration::from_millis(20));
    println!("PASS monotonic timer and sleep");

    let mut random = [0u8; 32];
    File::open("/dev/urandom")?.read_exact(&mut random)?;
    // Never print entropy. This deliberately tests the old-kernel device API,
    // rather than pretending it validates Ziggurat's getrandom dependency.
    println!("PASS OS random-device read");

    let listener = TcpListener::bind("127.0.0.1:0")?;
    let addr = listener.local_addr()?;
    let echo = thread::spawn(move || -> std::io::Result<()> {
        let (mut stream, _) = listener.accept()?;
        stream.set_read_timeout(Some(Duration::from_secs(3)))?;
        stream.set_write_timeout(Some(Duration::from_secs(3)))?;
        let mut request = [0u8; 4];
        stream.read_exact(&mut request)?;
        stream.write_all(&request)?;
        Ok(())
    });
    let mut stream = TcpStream::connect_timeout(&addr, Duration::from_secs(3))?;
    stream.set_read_timeout(Some(Duration::from_secs(3)))?;
    stream.set_write_timeout(Some(Duration::from_secs(3)))?;
    stream.write_all(b"test")?;
    let mut response = [0u8; 4];
    stream.read_exact(&mut response)?;
    assert_eq!(&response, b"test");
    echo.join().expect("echo worker panicked")?;
    println!("PASS localhost TCP I/O");
    println!("Prototype completed; radio and gateway configuration untouched.");
    Ok(())
}
