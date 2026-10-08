#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/rust-env.sh"
mkdir -p "$experiment_root/targets"
rustc -Z unstable-options --print target-spec-json --target mips-unknown-linux-musl \
    > "$experiment_root/targets/mips-default.json"
rustc -Z unstable-options --print target-spec-json --target mips-unknown-linux-uclibc \
    > "$experiment_root/targets/mips-uclibc-default.json"
python3 - "$experiment_root/targets" <<'PY'
import json
import os
import pathlib
import sys

targets = pathlib.Path(sys.argv[1])
spec = json.loads((targets / "mips-default.json").read_text())
spec["cpu"] = "mips1"
spec["features"] = "-mips32r2,+mips1,+soft-float"
spec["metadata"] = {
    "description": "Experimental Lexra MIPS-I codegen probe, not a validated device target",
    "tier": 3,
    "host_tools": False,
    "std": True,
}
(targets / "lexra-mips1.json").write_text(json.dumps(spec, indent=2) + "\n")

stock = json.loads((targets / "mips-uclibc-default.json").read_text())
stock["cpu"] = spec["cpu"]
stock["features"] = spec["features"]
stock["metadata"] = spec["metadata"]
# The stock kernel rejects LLVM's rdhwr-based native TLS access. Use std's
# pthread-key implementation rather than compiler-generated ELF TLS.
stock["has-thread-local"] = False
# LLVM otherwise emits TEQ for division checks, which the Lexra kernel rejects.
stock["llvm-args"] = ["-mno-check-zero-division"]
stock["linker"] = os.environ["TARGET_CC"]
(targets / "lexra-stock-uclibc.json").write_text(json.dumps(stock, indent=2) + "\n")
PY
