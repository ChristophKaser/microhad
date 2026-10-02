use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    #[cfg(target_env = "uclibc")]
    assert_eq!(lexra_rust_prototype::probe_add(1, 2), 3);

    println!("Starting dependency probe");
    let mut entropy = [0u8; 32];
    getrandom::fill(&mut entropy)?;
    let _random_value: u32 = rand::random();
    println!("PASS getrandom 0.4.3 and rand 0.10.3 (entropy not printed)");

    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()?;
    runtime.block_on(async {
        tokio::time::timeout(Duration::from_secs(8), probe()).await??;
        Ok::<_, Box<dyn std::error::Error>>(())
    })?;
    println!("Dependency probe completed; radio and configuration untouched.");
    Ok(())
}

async fn probe() -> Result<(), Box<dyn std::error::Error>> {
    let count = Arc::new(parking_lot::Mutex::new(0u32));
    let mut workers = Vec::new();
    for _ in 0..2 {
        let count = Arc::clone(&count);
        workers.push(tokio::spawn(async move {
            for _ in 0..1000 {
                *count.lock() += 1;
                tokio::task::yield_now().await;
            }
        }));
    }
    for worker in workers {
        worker.await?;
    }
    assert_eq!(*count.lock(), 2000);
    let start = tokio::time::Instant::now();
    tokio::time::sleep(Duration::from_millis(20)).await;
    assert!(start.elapsed() >= Duration::from_millis(20));
    println!("PASS Tokio multithread runtime, task scheduling, timers and parking_lot");

    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
    let addr = listener.local_addr()?;
    let echo = tokio::spawn(async move {
        let (mut peer, _) = listener.accept().await?;
        let mut message = [0u8; 4];
        peer.read_exact(&mut message).await?;
        peer.write_all(&message).await?;
        Ok::<_, std::io::Error>(())
    });
    let mut client = tokio::net::TcpStream::connect(addr).await?;
    client.write_all(b"test").await?;
    let mut reply = [0u8; 4];
    client.read_exact(&mut reply).await?;
    assert_eq!(&reply, b"test");
    echo.await??;
    println!("PASS Tokio/Mio loopback networking");

    // Anonymous pseudo-terminals exercise serial configuration and async I/O
    // without ever opening /dev/ttyS1 or another physical device.
    let (mut master, mut slave) = tokio_serial::SerialStream::pair()?;
    master.write_all(b"uart").await?;
    let mut serial_reply = [0u8; 4];
    slave.read_exact(&mut serial_reply).await?;
    assert_eq!(&serial_reply, b"uart");
    slave.write_all(b"echo").await?;
    master.read_exact(&mut serial_reply).await?;
    assert_eq!(&serial_reply, b"echo");
    println!("PASS tokio-serial anonymous PTY bidirectional I/O");
    Ok(())
}
