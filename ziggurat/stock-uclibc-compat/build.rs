use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=native/stock_compat.c");
    println!("cargo:rerun-if-changed=native/fault_report.c");
    assert_eq!(env::var("CARGO_CFG_TARGET_ENV").unwrap(), "uclibc");
    assert_eq!(env::var("CARGO_CFG_TARGET_ARCH").unwrap(), "mips");
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap().parent().unwrap();
    let defaults = include_str!("../../config/toolchains.mk");
    let relative = defaults
        .lines()
        .find_map(|line| line.strip_prefix("RSDK_RELATIVE="))
        .expect("shared RSDK setting");
    let prefix = defaults
        .lines()
        .find_map(|line| line.strip_prefix("TARGET_PREFIX="))
        .expect("shared target prefix");
    let rsdk = env::var_os("RSDK_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join(relative));
    let compiler = env::var_os("TARGET_CC")
        .map(PathBuf::from)
        .unwrap_or_else(|| rsdk.join("bin").join(format!("{prefix}gcc")));
    let archiver = env::var_os("TARGET_AR")
        .map(PathBuf::from)
        .unwrap_or_else(|| rsdk.join("bin").join(format!("{prefix}ar")));
    for variable in ["RSDK_ROOT", "TARGET_CC", "TARGET_AR"] {
        println!("cargo:rerun-if-env-changed={variable}");
    }
    println!("cargo:rerun-if-changed=../../config/toolchains.mk");
    let object = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("stock_compat.o");
    let status = Command::new(&compiler)
        .args(["-O2", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-c"])
        .arg(manifest.join("native/stock_compat.c"))
        .arg("-o")
        .arg(&object)
        .status()
        .expect("cannot run the stock RSDK C compiler");
    assert!(
        status.success(),
        "stock libc compatibility shim did not compile"
    );
    let archive = object.with_file_name("libstock_compat.a");
    let fault_object = object.with_file_name("fault_report.o");
    let status = Command::new(&compiler)
        .args(["-O2", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-c"])
        .arg(manifest.join("native/fault_report.c"))
        .arg("-o")
        .arg(&fault_object)
        .status()
        .expect("cannot compile the SIGILL reporter");
    assert!(status.success(), "SIGILL reporter did not compile");
    let status = Command::new(&archiver)
        .arg("rcs")
        .arg(&archive)
        .arg(&object)
        .arg(&fault_object)
        .status()
        .expect("cannot run the stock RSDK archiver");
    assert!(status.success(), "cannot archive the compatibility shim");
    println!(
        "cargo:rustc-link-search=native={}",
        archive.parent().unwrap().display()
    );
    // The shim supplies symbols referenced by std, which is linked after this
    // crate. Keep its object even before those references are encountered.
    println!("cargo:rustc-link-lib=static:+whole-archive=stock_compat");
}
