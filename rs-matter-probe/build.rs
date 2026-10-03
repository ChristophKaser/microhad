use std::env;
use std::path::PathBuf;
fn main() {
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
    let base = root.join("build/openthread-unix").join(mode);
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
                "Run scripts/build-openthread-transport.sh {mode} first; missing {}",
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
    println!("cargo:rustc-link-lib=util");
    println!("cargo:rustc-link-lib=rt");
}
