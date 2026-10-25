//! Compare previous JSON-based and production typed report suppression on a Linux host.
#[cfg(feature="controller-service")]
fn main() {
    use micro_thread_matter_daemon::{control::Id,operations::{ResultValue,Value,MeasurementValue}};
    use std::hint::black_box;
    unsafe extern "C" {fn clock()->std::os::raw::c_long;}
    let value=ResultValue{node:Id(2),endpoint:1,cluster:1026,attribute:0,value:Value::Measurement(MeasurementValue{value:Some(21.125),unit:"celsius",quality:"measured"})};
    for changed in [false,true] {for json in [true,false] {
        let mut samples=Vec::new();
        for _ in 0..5 {
            let mut previous_json=None;let mut previous_typed=None;let mut next=value.clone();
            let start=unsafe{clock()};
            for i in 0..100_000 {
                if changed {next.value=Value::Measurement(MeasurementValue{value:Some(21.125+(i%2) as f32),unit:"celsius",quality:"measured"});}
                let value=black_box(&next);
                if json {
                    let encoded=serde_json::to_vec(value).unwrap();
                    if previous_json.as_ref()!=Some(&encoded){previous_json=Some(encoded);}
                } else if previous_typed.as_ref()!=Some(value){previous_typed=Some(value.clone());}
                black_box((&previous_json,&previous_typed));
            }
            samples.push((unsafe{clock()}-start) as f64/100_000.0);
        }
        samples.sort_by(f64::total_cmp);
        println!("{}",serde_json::json!({"changed":changed,"implementation":if json{"json"}else{"typed"},"cpu_us":samples[2]}));
    }}
}
#[cfg(not(feature="controller-service"))]
fn main(){panic!("requires --features controller-service");}
