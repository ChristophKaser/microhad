use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::sync::{Arc, Mutex};
use std::time::Instant;
fn main() {
    let before = Instant::now();
    let state = Arc::new(Mutex::new(vec![0u8; 128]));
    let other = state.clone();
    std::thread::Builder::new().name("stock-probe".into())
        .spawn(move || other.lock().unwrap()[0] = 42).unwrap().join().unwrap();
    assert_eq!(state.lock().unwrap()[0], 42);
    let (mut left, mut right) = UnixStream::pair().unwrap();
    left.write_all(b"x").unwrap();
    let mut data = [0]; right.read_exact(&mut data).unwrap();
    assert_eq!(&data, b"x");
    let _elapsed = before.elapsed();
    println!("PASS Rust: allocation, threads/mutex, clocks, sockets");
}
