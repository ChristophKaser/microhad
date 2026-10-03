//! Synthetic-radio transport smoke test; no production dataset or real device.
use core::pin::pin;
use embassy_time::{Duration, Timer};
use lexra_rs_matter_probe::openthread::OpenThread;
use rs_matter::transport::network::{IpAddr, NetworkMulticast, NetworkReceive, NetworkSend};

fn dataset() -> Vec<u8> {
    let mut bytes = Vec::new();
    for (kind, value) in [
        (0, vec![0, 0, 15]),
        (1, vec![0x12, 0x34]),
        (2, vec![1; 8]),
        (3, b"fixture".to_vec()),
        (4, vec![0x22; 16]),
        (5, vec![0x33; 16]),
        (7, vec![0xfd, 0, 0, 0, 0, 0, 0, 1]),
        (12, vec![2, 160, 0xff, 0xf8]),
        (14, vec![0, 0, 0, 0, 0, 1, 0, 0]),
        (53, vec![0, 4, 7, 0xff, 0xf8, 0]),
    ] {
        bytes.extend([kind, value.len() as u8]);
        bytes.extend(value);
    }
    bytes
}
fn main() {
    let url = std::env::args()
        .nth(1)
        .expect("Pass an isolated synthetic Unix radio URL");
    // Real UART URLs and missing reset guard must be rejected before initialization.
    assert!(OpenThread::init("spinel+hdlc+uart:///dev/ttyS1?no-reset").is_err());
    assert!(OpenThread::init("spinel+hdlc+unix:///tmp/missing").is_err());
    let thread = OpenThread::init(&url).expect("OpenThread initialization");
    thread
        .set_dataset(&dataset(), 15)
        .expect("public fixture dataset");
    let matter = thread.bind(5540).expect("Matter internal UDP socket");
    let mdns = thread.bind(5353).expect("mDNS internal UDP socket");
    assert!(thread.bind(7777).is_err());
    println!("PASS OpenThread init, dataset, two bounded sockets and UART/reset guards");
    let mut send = &matter;
    let mut recv = &matter;
    let mut dns_send = &mdns;
    let mut dns_recv = &mdns;
    let work = async {
        for length in [4, 256, 1232, 0] {
            println!("SEND payload_bytes={length}");
            let payload: Vec<u8> = (0..length).map(|index| index as u8).collect();
            send.send_to(&payload, thread.own_address(5540).unwrap())
                .await
                .unwrap();
            recv.wait_available().await.unwrap();
            let mut buffer = [0; 1500];
            let (received, peer) = recv.recv_from(&mut buffer).await.unwrap();
            println!("RECEIVED payload_bytes={received}");
            assert_eq!(received, length);
            assert_eq!(&buffer[..received], payload);
            assert_eq!(peer, thread.own_address(5540).unwrap());
        }
        assert!(send
            .send_to(&[0; 1233], thread.own_address(5540).unwrap())
            .await
            .is_err());
        let multicast = IpAddr::V6("ff02::fb".parse().unwrap());
        dns_send.join(multicast).await.unwrap();
        dns_send
            .send_to(b"synthetic-dns-packet", thread.own_address(5353).unwrap())
            .await
            .unwrap();
        dns_recv.wait_available().await.unwrap();
        let mut buffer = [0; 1500];
        let (length, _) = dns_recv.recv_from(&mut buffer).await.unwrap();
        assert_eq!(&buffer[..length], b"synthetic-dns-packet");
        dns_send.leave(multicast).await.unwrap();
        println!("PASS rs-matter send/receive traits, datagram boundaries and multicast API");
    };
    let pump = async {
        loop {
            thread.poll(0).unwrap();
            Timer::after(Duration::from_millis(5)).await;
        }
    };
    // Tiny local executor: poll the async work and timer futures while pumping OpenThread.
    // Existing probe has no desktop network/runtime dependency.
    let wake = std::sync::Arc::new(NoopWake);
    let waker = std::task::Waker::from(wake);
    let mut context = std::task::Context::from_waker(&waker);
    let mut work = pin!(work);
    let mut pump = pin!(pump);
    let started = std::time::Instant::now();
    loop {
        use std::future::Future;
        if work.as_mut().poll(&mut context).is_ready() {
            break;
        }
        let _ = pump.as_mut().poll(&mut context);
        assert!(
            started.elapsed().as_secs() < 15,
            "transport smoke test timeout"
        );
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    println!(
        "PASS transport complete elapsed_ms={}",
        started.elapsed().as_millis()
    );
}
struct NoopWake;
impl std::task::Wake for NoopWake {
    fn wake(self: std::sync::Arc<Self>) {}
}
