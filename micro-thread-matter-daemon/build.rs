use std::env;
use std::path::PathBuf;
fn main() {
    let fields = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap())
        .join("../config/thermostat-attributes.def");
    println!("cargo:rerun-if-changed={}", fields.display());
    let mut generated = String::from("pub const FIELDS: &[Field] = &[\n");
    for line in std::fs::read_to_string(fields)
        .unwrap()
        .lines()
        .filter(|line| line.starts_with("THERMOSTAT_ATTRIBUTE("))
    {
        let args: Vec<_> = line
            .trim_start_matches("THERMOSTAT_ATTRIBUTE(")
            .trim_end_matches(')')
            .split(',')
            .map(str::trim)
            .collect();
        assert_eq!(args.len(), 10);
        let float = |v: &str| {
            let v = if v.starts_with('.') {
                format!("0{v}")
            } else if v.starts_with("-.") {
                v.replacen("-.", "-0.", 1)
            } else {
                v.to_owned()
            };
            format!("{v}f64")
        };
        generated.push_str(&format!("Field {{ name: {}, cluster: {}, attribute: {}, kind: {}, scale: {}, minimum: {}, maximum: {}, step: {}, writable: {} }},\n",
            format!("\"{}\"", args[0]), args[1], args[2], args[3], float(args[4]), float(args[5]), float(args[6]), float(args[7]), args[8]));
    }
    generated.push_str("];\n");
    std::fs::write(
        PathBuf::from(env::var("OUT_DIR").unwrap()).join("thermostat_fields.rs"),
        generated,
    )
    .unwrap();
    println!("cargo:rerun-if-env-changed=CARGO_FEATURE_OPENTHREAD");
    if env::var_os("CARGO_FEATURE_OPENTHREAD").is_none() {
        return;
    }
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap())
        .parent()
        .unwrap()
        .to_path_buf();
    let mode = if env::var("CARGO_CFG_TARGET_ARCH").unwrap() == "mips" {
        "mips"
    } else {
        "host"
    };
    println!("cargo:rerun-if-env-changed=TOOLCHAIN");
    let backend = env::var("TOOLCHAIN").unwrap_or_else(|_| "rsdk".into());
    assert!(
        backend == "rsdk" || backend == "crosstool-ng",
        "invalid TOOLCHAIN"
    );
    println!("cargo:rerun-if-env-changed=LINK_MODE");
    let link_mode = env::var("LINK_MODE").unwrap_or_else(|_| "static".into());
    let base = if mode == "mips" && link_mode == "dynamic" {
        root.join("build/dynamic/openthread-unix").join(mode)
    } else if mode == "mips" && backend == "crosstool-ng" {
        root.join("build/crosstool-ng/openthread-unix").join(mode)
    } else {
        root.join("build/openthread-unix").join(mode)
    };
    if env::var_os("CARGO_FEATURE_CONTROLLER_PROBE").is_some() {
        let fixture = base.join("src/posix/libgateway-thread-discovery-fixture.a");
        assert!(
            fixture.is_file(),
            "Build the separate discovery probe fixture archive first"
        );
        println!("cargo:rerun-if-changed={}", fixture.display());
        println!(
            "cargo:rustc-link-search=native={}",
            fixture.parent().unwrap().display()
        );
        println!("cargo:rustc-link-lib=static=gateway-thread-discovery-fixture");
    }
    let archives = [
        "src/posix/libgateway-thread-transport.a",
        "src/posix/platform/libopenthread-posix.a",
        "src/core/libopenthread-ftd.a",
        "src/lib/hdlc/libopenthread-hdlc.a",
        "src/lib/spinel/libopenthread-radio-spinel.a",
        "src/lib/spinel/libopenthread-spinel-rcp.a",
        "third_party/tcplp/libtcplp-ftd.a",
        "third_party/mbedtls/repo/library/libmbedtls.a",
        "third_party/mbedtls/repo/library/libmbedx509.a",
        "third_party/mbedtls/repo/library/libmbedcrypto.a",
        "third_party/mbedtls/repo/3rdparty/everest/libeverest.a",
        "third_party/mbedtls/repo/3rdparty/p256-m/libp256m.a",
        "src/lib/platform/libopenthread-platform.a",
        "src/lib/url/libopenthread-url.a",
    ];
    // The pinned SDK archives have circular references; repeat them to resolve those with the stock linker.
    for _ in 0..3 {
        for relative in archives {
            let path = base.join(relative);
            assert!(
                path.is_file(),
                "Run micro-thread-matter-daemon/scripts/build-openthread-transport.sh {mode} first; missing {}",
                path.display()
            );
            println!("cargo:rerun-if-changed={}", path.display());
            println!(
                "cargo:rustc-link-search=native={}",
                path.parent().unwrap().display()
            );
            let name = path
                .file_name()
                .unwrap()
                .to_str()
                .unwrap()
                .trim_start_matches("lib")
                .trim_end_matches(".a");
            println!("cargo:rustc-link-lib=static={name}");
        }
    }
    println!("cargo:rustc-link-lib=stdc++");
    if mode == "mips" && link_mode == "dynamic" {
        // The stock image has no libutil.so; include forkpty/openpty locally.
        println!("cargo:rustc-link-lib=static=util");
    } else {
        println!("cargo:rustc-link-lib=util");
    }
    println!("cargo:rustc-link-lib=rt");
    println!("cargo:rustc-link-arg=-Wl,--wrap=otPlatSettingsGet");
}
