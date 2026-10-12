//! Synthetic logging only: no radio, controller, filesystem state or credentials.
fn main() {
    let capture = std::env::args().nth(1).as_deref() == Some("on");
    let _guard = micro_thread_matter_daemon::logging::init(log::LevelFilter::Info, capture)
        .expect("logger initialization");
    log::info!("capture probe ready");
    log::trace!(target: "rs_matter", "general library trace must stay disabled");
    let peer = "[fd00::1234]:5540";
    micro_thread_matter_daemon::logging::packet_received(&peer, &[0x00, 0xab, 0xff]);
    micro_thread_matter_daemon::logging::packet_sent(&peer, &[0x01, 0xcd], 0);
    micro_thread_matter_daemon::logging::packet_sent(&peer, &[0x02], 4);
}
