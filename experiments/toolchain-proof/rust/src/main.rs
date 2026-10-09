use std::{cell::Cell, io::{Read, Write}, net::{TcpListener, TcpStream},
          sync::{Arc, atomic::{AtomicUsize, Ordering}}, thread, time::{Duration, Instant}};

thread_local! { static VALUE: Cell<usize> = const { Cell::new(0) }; }

fn main() {
    println!("START Rust compiler proof");
    let allocation = vec![0xa5u8; 65536];
    assert_eq!(allocation.iter().map(|v| *v as usize).sum::<usize>(), 0xa5 * 65536);
    let bytes = [0u8, 0x78, 0x56, 0x34, 0x12, 0];
    let value = unsafe { std::ptr::read_unaligned(bytes.as_ptr().add(1).cast::<u32>()) };
    assert_eq!(u32::from_le(value), 0x12345678);
    let numerator = std::hint::black_box(0x123456789abcdef0u64);
    assert_eq!(numerator / std::hint::black_box(17), 0x112233445566777);
    assert_eq!(std::hint::black_box(81.0f64).sqrt(), 9.0);
    assert_eq!(std::hint::black_box(81.0f64) / std::hint::black_box(16.0), 5.0625);
    println!("PASS Rust allocation, arithmetic, byte order, unaligned decoding");
    let count = Arc::new(AtomicUsize::new(0));
    let workers: Vec<_> = (1..=2).map(|id| {
        let count = count.clone();
        thread::spawn(move || {
            VALUE.with(|v| { assert_eq!(v.get(), 0); v.set(id); });
            for _ in 0..1000 {
                count.fetch_add(1, Ordering::SeqCst);
                VALUE.with(|v| assert_eq!(v.get(), id));
            }
        })
    }).collect();
    for worker in workers { worker.join().unwrap(); }
    assert_eq!(count.load(Ordering::SeqCst), 2000);
    VALUE.with(|v| assert_eq!(v.get(), 0));
    println!("PASS Rust threads, atomics, thread-local values");
    let start = Instant::now();
    thread::sleep(Duration::from_millis(20));
    assert!(start.elapsed() >= Duration::from_millis(10));
    let mut random = [0u8; 32];
    std::fs::File::open("/dev/urandom").unwrap().read_exact(&mut random).unwrap();
    println!("PASS Rust monotonic timer, sleep, random-device read");
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let mut client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
    let (mut server, _) = listener.accept().unwrap();
    client.write_all(b"TCP").unwrap();
    let mut buffer = [0u8; 3];
    server.read_exact(&mut buffer).unwrap();
    assert_eq!(&buffer, b"TCP");
    let (mut left, mut right) = std::os::unix::net::UnixStream::pair().unwrap();
    left.write_all(b"IPC").unwrap();
    right.read_exact(&mut buffer).unwrap();
    assert_eq!(&buffer, b"IPC");
    println!("PASS Rust loopback TCP and Unix sockets");
    let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(2)
        .thread_stack_size(128 * 1024).enable_all().build().unwrap();
    runtime.block_on(async {
        tokio::time::timeout(Duration::from_secs(5), async {
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
            let address = listener.local_addr().unwrap();
            let server = tokio::spawn(async move {
                use tokio::io::{AsyncReadExt, AsyncWriteExt};
                let (mut socket, _) = listener.accept().await.unwrap();
                let mut bytes = [0u8; 4];
                socket.read_exact(&mut bytes).await.unwrap();
                assert_eq!(&bytes, b"PING");
                socket.write_all(b"PONG").await.unwrap();
            });
            use tokio::io::{AsyncReadExt, AsyncWriteExt};
            let mut client = tokio::net::TcpStream::connect(address).await.unwrap();
            client.write_all(b"PING").await.unwrap();
            let mut bytes = [0u8; 4];
            client.read_exact(&mut bytes).await.unwrap();
            assert_eq!(&bytes, b"PONG");
            server.await.unwrap();
            tokio::time::sleep(Duration::from_millis(20)).await;
        }).await.unwrap();
    });
    println!("PASS Tokio multi-thread runtime, timers, epoll and async TCP");
    println!("PASS Rust compiler proof complete");
}
