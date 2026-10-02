"""Make disposable patched crate copies without editing Cargo's registry."""
import difflib
import os
from pathlib import Path
import shutil
import sys

root = Path(__file__).resolve().parent.parent
registry = Path(os.environ["CARGO_HOME"]) / "registry" / "src"
changes = {
    "getrandom-0.4.3": (
        "src/backends.rs",
        '        // Rust supports Android API level 19 (KitKat) [0] and the next upgrade targets',
        '        // Stock MIPS/uClibc may run Linux before getrandom(2) existed.\n'
        '        all(target_os = "linux", target_env = "uclibc"),\n'
        '        // Rust supports Android API level 19 (KitKat) [0] and the next upgrade targets',
    ),
    "nix-0.26.4": (
        "src/sys/signal.rs",
        '        SA_NOCLDWAIT;',
        '        SA_NOCLDWAIT as SaFlags_t;',
    ),
    "radium-0.7.0": (
        "build.rs",
        '    if atomics.has_8 {',
        '    // Use compiler capabilities for custom target names as well.\n'
        '    if let Ok(widths) = env::var("CARGO_CFG_TARGET_HAS_ATOMIC") {\n'
        '        let widths: Vec<_> = widths.split(",").collect();\n'
        '        atomics.has_8 = widths.contains(&"8");\n'
        '        atomics.has_16 = widths.contains(&"16");\n'
        '        atomics.has_32 = widths.contains(&"32");\n'
        '        atomics.has_64 = widths.contains(&"64");\n'
        '        atomics.has_ptr = widths.contains(&"ptr");\n'
        '    }\n\n'
        '    if atomics.has_8 {',
    ),
}
for crate in sys.argv[1:]:
    relative, before, after = changes[crate]
    matches = list(registry.glob(f"*/{crate}"))
    if len(matches) != 1:
        raise SystemExit(f"Fetch {crate} first; expected one registry source, found {len(matches)}")
    source = matches[0]
    original = (source / relative).read_text()
    if original.count(before) != 1:
        raise SystemExit(f"Unexpected source for {crate}/{relative}; refusing to patch")
    modified = original.replace(before, after, 1)
    if crate == "nix-0.26.4":
        duplicate_arm = '#[cfg(any(all(target_os = "linux", target_env = "musl"), target_arch = "mips"))]'
        corrected_arm = '#[cfg(any(all(target_os = "linux", target_env = "musl"), all(target_arch = "mips", not(target_env = "uclibc"))))]'
        if modified.count(duplicate_arm) != 1:
            raise SystemExit("Unexpected nix SIGEV_THREAD_ID cfg; refusing to patch")
        modified = modified.replace(duplicate_arm, corrected_arm, 1)
    destination = root / "build" / "patched-crates" / crate
    shutil.copytree(source, destination, dirs_exist_ok=True)
    (destination / relative).write_text(modified)
    patch = "".join(difflib.unified_diff(
        original.splitlines(keepends=True), modified.splitlines(keepends=True),
        fromfile=f"a/{relative}", tofile=f"b/{relative}",
    ))
    (root / "patches" / f"{crate}.patch").write_text(patch)
    if crate == "getrandom-0.4.3":
        fallback = "src/backends/linux_android_with_fallback.rs"
        old_fallback = (source / fallback).read_text()
        new_fallback = old_fallback.replace(
            '#[cfg(not(target_env = "musl"))]',
            '#[cfg(not(any(target_env = "musl", target_env = "uclibc")))]',
            1,
        ).replace(
            '    #[cfg(target_env = "musl")]\n    let raw_ptr = {',
            '    // Static stock uClibc has no usable dynamic symbol lookup.\n'
            '    #[cfg(target_env = "uclibc")]\n'
            '    let raw_ptr = core::ptr::null_mut();\n'
            '    #[cfg(target_env = "musl")]\n    let raw_ptr = {',
            1,
        )
        if new_fallback == old_fallback:
            raise SystemExit("Unexpected getrandom fallback source; refusing to patch")
        (destination / fallback).write_text(new_fallback)
        with (root / "patches" / f"{crate}.patch").open("a") as output:
            output.write("".join(difflib.unified_diff(
                old_fallback.splitlines(keepends=True), new_fallback.splitlines(keepends=True),
                fromfile=f"a/{fallback}", tofile=f"b/{fallback}",
            )))
    print(f"Prepared {crate}")
