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
    let resolve = |key: &str| -> PathBuf {
        let output = Command::new("python3")
            .arg(root.join("scripts/toolchain_config.py"))
            .arg(key).output().expect("cannot resolve shared toolchain");
        assert!(output.status.success(), "invalid shared toolchain configuration");
        PathBuf::from(String::from_utf8(output.stdout).unwrap().trim())
    };
    let compiler = resolve("TARGET_CC");
    let archiver = resolve("TARGET_AR");
    let nm = resolve("TARGET_NM");
    for variable in ["STOCK_ROOTFS", "LINK_MODE", "TOOLCHAIN", "RSDK_ROOT", "CROSSTOOL_NG_ROOT", "TARGET_PREFIX", "TARGET_CC", "TARGET_AR", "TARGET_NM"] {
        println!("cargo:rerun-if-env-changed={variable}");
    }
    println!("cargo:rerun-if-changed=../../config/toolchains.mk");
    println!("cargo:rerun-if-changed=../../scripts/toolchain_config.py");
    let dynamic = env::var("LINK_MODE").as_deref() == Ok("dynamic");
    let mut symbols = std::collections::HashSet::new();
    for library in if dynamic { ["libc.so.0", "libpthread.so.0", "librt.so.0"] } else { ["libc.a", "libpthread.a", "librt.a"] } {
        let archive = if dynamic {
            resolve("STOCK_ROOTFS").join("lib").join(library)
                .canonicalize().expect("extract stock /lib and set STOCK_ROOTFS first")
        } else {
            let output = Command::new(&compiler).arg(format!("-print-file-name={library}"))
                .output().expect("cannot resolve libc archive");
            assert!(output.status.success());
            PathBuf::from(String::from_utf8(output.stdout).unwrap().trim())
        };
        println!("cargo:rerun-if-changed={}", archive.display());
        let mut command = Command::new(&nm);
        command.arg("--defined-only");
        if dynamic { command.arg("-D"); }
        let output = command.arg(&archive)
            .output().expect("cannot inspect libc symbols");
        assert!(output.status.success(), "cannot inspect selected libc archive");
        for line in String::from_utf8(output.stdout).unwrap().lines() {
            if let Some(symbol) = line.split_whitespace().last() {
                symbols.insert(symbol.split('@').next().unwrap().to_owned());
            }
        }
    }
    let missing: Vec<_> = ["epoll_create1", "eventfd", "accept4", "clock_nanosleep", "pthread_setname_np",
                         "bcmp", "pthread_getattr_np", "pthread_attr_getstack"]
        .into_iter().filter(|symbol| !symbols.contains(*symbol)).collect();
    let defines: Vec<_> = missing.iter().map(|symbol| format!("-DCOMPAT_NEED_{symbol}")).collect();
    if missing.contains(&"pthread_getattr_np") || missing.contains(&"pthread_attr_getstack") {
        println!("cargo:warning=Experimental LinuxThreads stack inspection returns ENOSYS; overflow handling is not guaranteed");
    }
    let object = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("stock_compat.o");
    let status = Command::new(&compiler)
        .args(["-O2", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-c"])
        .args(&defines)
        .arg(manifest.join("native/stock_compat.c"))
        .arg("-o")
        .arg(&object)
        .status()
        .expect("cannot run the selected target C compiler");
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
        .expect("cannot run the selected target archiver");
    assert!(status.success(), "cannot archive the compatibility shim");
    println!(
        "cargo:rustc-link-search=native={}",
        archive.parent().unwrap().display()
    );
    // The shim supplies symbols referenced by std, which is linked after this
    // crate. Keep its object even before those references are encountered.
    println!("cargo:rustc-link-lib=static:+whole-archive=stock_compat");
}
