//! Synthetic host-only benchmark: cargo run --release --example benchmark-ipc -- 3000.
use micro_thread_matter_daemon::ipc::{Decoder, Frame};
use std::hint::black_box;
use std::os::raw::c_long;
unsafe extern "C" { fn clock() -> c_long; }
// POSIX clock() measures process CPU in microseconds on the Linux host.
fn cpu() -> f64 { unsafe { clock() as f64 / 1_000_000.0 } }
fn main() {
    let iterations: usize = std::env::args().nth(1).map(|n| n.parse().unwrap()).unwrap_or(3000);
    assert!(iterations>0);
    let vectors: serde_json::Value = serde_json::from_str(include_str!("../../protocol/matter-wire-vectors.json")).unwrap();
    for name in ["empty-status", "max-ble-bytes", "max-dataset-bytes", "measurement-event", "thermostat-schedule-job", "repeated-512"] {
        let v=vectors.as_array().unwrap().iter().find(|v|v["name"]==name).unwrap();
        let frame=Frame{kind:v["kind"].as_u64().unwrap() as u8,opcode:v["opcode"].as_u64().unwrap() as u8,id:v["id"].as_u64().unwrap() as u32,payload:serde_json::to_vec(&v["body"]).unwrap()};
        let wire=frame.encode().unwrap();
        let mut results=Vec::new();
        for mode in 0..2 {
            let mut samples=Vec::new();
            for _ in 0..5 {
                let start=cpu();
                for _ in 0..iterations {
                    if mode==0 {assert_eq!(black_box(frame.encode().unwrap()),wire);} else {
                        let mut decoder=Decoder::default();decoder.feed(&wire).unwrap();let decoded=black_box(decoder.pop().unwrap());
                        assert_eq!((decoded.kind,decoded.opcode,decoded.id),(frame.kind,frame.opcode,frame.id));
                    }
                }
                samples.push((cpu()-start)*1e6/iterations as f64);
            }
            samples.sort_by(f64::total_cmp);results.push(samples[2]);
        }
        println!("{}",serde_json::json!({"name":name,"iterations":iterations,"encode_cpu_us":results[0],"decode_cpu_us":results[1]}));
    }
}
