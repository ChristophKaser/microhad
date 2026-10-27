//! Synthetic logging only: no radio, controller, filesystem state or credentials.
fn main() {
    if std::env::args().nth(1).as_deref() == Some("panic") {
        let _guard = micro_thread_matter_daemon::logging::init(log::LevelFilter::Off, false)
            .expect("logger initialization");
        micro_thread_matter_daemon::logging::install_panic_diagnostics();
        panic!("private panic payload must not appear in syslog");
    }
    let capture = std::env::args().nth(1).as_deref() == Some("on");
    let _guard = micro_thread_matter_daemon::logging::init(log::LevelFilter::Info, capture)
        .expect("logger initialization");
    log::info!("capture probe ready");
    log::trace!(target: "rs_matter", "general library trace must stay disabled");
    unsafe extern "C" {
        fn gatewayOtSetDiagnosticLog(callback: extern "C" fn(u8, *const core::ffi::c_char));
        fn otPlatLog(level: i32, region: i32, format: *const core::ffi::c_char, ...);
    }
    extern "C" fn diagnostic(level: u8, message: *const core::ffi::c_char) {
        let message = unsafe { std::ffi::CStr::from_ptr(message) }.to_string_lossy();
        log::log!(target: "thread_diagnostics", if level == 2 { log::Level::Info } else { log::Level::Debug }, "{message}");
    }
    // Exercise the real C++ platform sink, including failures that never reach
    // the application registration callback. No OT instance/radio is needed.
    unsafe {
        gatewayOtSetDiagnosticLog(diagnostic);
        otPlatLog(2, 0, c"[W] SrpServer----: signature rejected".as_ptr());
        otPlatLog(4, 0, c"[I] SrpServer----: registration accepted".as_ptr());
        otPlatLog(4, 0, c"[I] Mle----------: routine stack detail".as_ptr());
    }
    let peer = "[fd00::1234]:5540";
    micro_thread_matter_daemon::logging::packet_received(&peer, &[0x00, 0xab, 0xff]);
    micro_thread_matter_daemon::logging::packet_sent(&peer, &[0x01, 0xcd], 0);
    micro_thread_matter_daemon::logging::packet_sent(&peer, &[0x02], 4);
}
