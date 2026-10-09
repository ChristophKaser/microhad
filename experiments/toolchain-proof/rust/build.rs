use std::{env, process::Command};

fn main() {
    println!("cargo:rerun-if-env-changed=PROOF_NATIVE_ARCHIVE_DIR");
    if let Ok(directory) = env::var("PROOF_NATIVE_ARCHIVE_DIR") {
        println!("cargo:rustc-link-search=native={directory}");
        println!("cargo:rustc-link-lib=static:+whole-archive=proof_compat");
        // Report selected compiler provenance without discovering any RSDK paths.
        let status = Command::new(env::var("PROOF_CC").unwrap())
            .arg("--version").status().unwrap();
        assert!(status.success());
    }
}
