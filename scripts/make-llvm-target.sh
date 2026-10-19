#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
mkdir -p "$experiment_root/targets"
python3 - "$experiment_root/targets" <<'PY'
import json
import os
import pathlib
import sys
import subprocess
import tempfile

targets = pathlib.Path(sys.argv[1])
def upstream(name):
    return json.loads(subprocess.check_output([
        "rustc", "+" + os.environ["RUST_TOOLCHAIN"], "-Z", "unstable-options",
        "--print", "target-spec-json", "--target", name], text=True))


def write(name, value):
    path = targets / name
    content = json.dumps(value, indent=2) + "\n"
    if path.exists() and path.read_text() == content:
        return
    with tempfile.NamedTemporaryFile(mode="w", dir=targets, delete=False) as output:
        output.write(content)
        temporary = output.name
    os.replace(temporary, path)


spec = upstream("mips-unknown-linux-musl")
write("mips-default.json", spec)
spec["cpu"] = "mips1"
spec["features"] = "-mips32r2,+mips1,+soft-float"
spec["metadata"] = {
    "description": "Experimental Lexra MIPS-I codegen probe, not a validated device target",
    "tier": 3,
    "host_tools": False,
    "std": True,
}
write("lexra-mips1.json", spec)

stock = upstream("mips-unknown-linux-uclibc")
write("mips-uclibc-default.json", stock)
stock["cpu"] = spec["cpu"]
stock["features"] = spec["features"]
stock["metadata"] = spec["metadata"]
# The stock kernel rejects LLVM's rdhwr-based native TLS access. Use std's
# pthread-key implementation rather than compiler-generated ELF TLS.
stock["has-thread-local"] = False
# LLVM otherwise emits TEQ for division checks, which the Lexra kernel rejects.
stock["llvm-args"] = ["-mno-check-zero-division"]
if os.environ["LINK_MODE"] == "dynamic":
    stock["crt-static-default"] = False
    stock["relocation-model"] = "pic"
stock["linker"] = os.environ["TARGET_CC"]
write(os.environ["TARGET_SPEC_NAME"] + ".json", stock)
PY
