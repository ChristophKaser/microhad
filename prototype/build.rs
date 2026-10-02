use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=native/stock_compat.c");
    println!("cargo:rerun-if-changed=native/fault_report.c");
    if env::var_os("CARGO_FEATURE_STOCK_COMPAT").is_none() {
        return;
    }
    assert_eq!(env::var("CARGO_CFG_TARGET_ENV").unwrap(), "uclibc");
    assert_eq!(env::var("CARGO_CFG_TARGET_ARCH").unwrap(), "mips");
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let compiler_dir = manifest
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129/bin");
    let object = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("stock_compat.o");
    let status = Command::new(compiler_dir.join("mips-linux-gcc"))
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
    let status = Command::new(compiler_dir.join("mips-linux-gcc"))
        .args(["-O2", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-c"])
        .arg(manifest.join("native/fault_report.c"))
        .arg("-o")
        .arg(&fault_object)
        .status()
        .expect("cannot compile the SIGILL reporter");
    assert!(status.success(), "SIGILL reporter did not compile");
    let status = Command::new(compiler_dir.join("mips-linux-ar"))
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
