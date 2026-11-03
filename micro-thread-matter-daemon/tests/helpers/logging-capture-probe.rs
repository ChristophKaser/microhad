//! Synthetic logging only: no radio, controller, filesystem state or credentials.
fn main() {
    if std::env::args().nth(1).as_deref() == Some("panic") {
        let _guard = micro_thread_matter_daemon::logging::init(log::LevelFilter::Off, false)
            .expect("logger initialization");
        micro_thread_matter_daemon::logging::install_panic_diagnostics();
        panic!("private panic payload must not appear in syslog");
    }
    if std::env::args().nth(1).as_deref() == Some("critical-exit") {
        let _guard = micro_thread_matter_daemon::logging::init(log::LevelFilter::Off, false)
            .expect("logger initialization");
        micro_thread_matter_daemon::logging::critical_thread_diagnostic(
            "synthetic RCP timeout before exit 6",
        );
        unsafe {
            libc::_exit(6);
        }
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
    capture_session();
    let peer = "[fd00::1234]:5540";
    micro_thread_matter_daemon::logging::packet_received(&peer, &[0x00, 0xab, 0xff]);
    micro_thread_matter_daemon::logging::packet_sent(&peer, &[0x01, 0xcd], 0);
    micro_thread_matter_daemon::logging::packet_sent(&peer, &[0x02], 4);
}

// Exercise the production session-update hook and encrypt real Matter messages.
// These fixed keys and identities belong only to this synthetic host fixture.
fn capture_session() {
    use rs_matter::crypto::{test_only_crypto, CanonAeadKeyRef};
    use rs_matter::dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET};
    use rs_matter::transport::network::Address;
    use rs_matter::transport::session::{ReservedSession, SessionMode};
    use rs_matter::utils::storage::WriteBuf;
    let crypto = test_only_crypto();
    let matter = rs_matter::Matter::new(
        &TEST_DEV_DET,
        TEST_DEV_COMM,
        &TEST_DEV_ATT,
        rs_matter::MATTER_PORT,
    );
    let peer = Address::Udp("[fd00::1234]:5540".parse().unwrap());
    let rx_key = [0x31; 16];
    let tx_key = [0x72; 16];
    let local_node = 0x1122334455667788;
    let peer_node = 0x8877665544332211;
    let mut session = ReservedSession::reserve_now(&matter, &crypto).unwrap();
    session
        .update(
            local_node,
            peer_node,
            0x451b,
            6,
            peer,
            SessionMode::Case {
                fab_idx: core::num::NonZeroU8::new(1).unwrap(),
                cat_ids: [0; 3],
            },
            Some(CanonAeadKeyRef::new(&rx_key)),
            Some(CanonAeadKeyRef::new(&tx_key)),
            None,
            None,
        )
        .unwrap();
    session.complete();
    for (rx, key, sid, node) in [
        (true, rx_key, 6u16, peer_node),
        (false, tx_key, 0x451bu16, local_node),
    ] {
        let mut header = [0u8; 8];
        header[1..3].copy_from_slice(&sid.to_le_bytes());
        header[4..8].copy_from_slice(&1234u32.to_le_bytes());
        let mut buffer = [0u8; 128];
        let mut body = WriteBuf::new(&mut buffer);
        body.append(b"synthetic Matter report").unwrap();
        rs_matter::transport::proto_hdr::encrypt_in_place(
            &crypto,
            CanonAeadKeyRef::new(&key),
            0,
            1234,
            node,
            &header,
            &mut body,
        )
        .unwrap();
        let packet = [header.as_slice(), body.as_slice()].concat();
        if rx {
            micro_thread_matter_daemon::logging::packet_received(&peer, &packet);
        } else {
            micro_thread_matter_daemon::logging::packet_sent(&peer, &packet, 0);
        }
    }
}
