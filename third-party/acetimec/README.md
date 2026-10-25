# Restricted AceTime C

AceTime C: https://github.com/bxparks/acetimec

`UPSTREAM.json` pins the exact revision, the original source checksums, and the
upstream-generated `zonedb2025` input checksums (IANA 2025b). The C engine is MIT
licensed, copyright Brian T. Park. `LICENSE.iana` records the time-zone data's
public-domain status. This directory is tracked source and is included by the
normal Git source-release export; builds need no downloads or database compiler.

The whitelist is `config/matter-time-zones.txt`, matching the 26 dashboard
choices. Only their dependency closure is generated into `zones.c`. Shared
policies, eras, and offset records are deduplicated. Aliases remain accepted by
the original dashboard name without retaining the upstream alias registry.
Names live only in our lookup table; unused IDs, abbreviations, letters, and
expired rules are removed. December 2024 rules and upstream anchor records are
retained solely to seed calculations at the start of the supported UTC range,
2025-01-01 inclusive to 2100-01-01 exclusive. No earlier conversions are exposed.

`offsets-only.patch` is the only local engine modification: it disables the
abbreviation-generation step when `GATEWAY_ACETIME_OFFSETS_ONLY` is defined.
Unused formatting and local-time conversion functions are discarded by section
garbage collection. The normal one-second-resolution data format and transition
buffer sizes are unchanged. No dynamic allocation is used by the C engine.

Regenerate against a checkout of the pinned revision (no implicit downloads):

```sh
python3 micro-thread-matter-daemon/scripts/generate-acetime-subset.py /path/to/acetimec
python3 micro-thread-matter-daemon/scripts/generate-acetime-subset.py /path/to/acetimec --check
python3 micro-thread-matter-daemon/scripts/test-acetime-subset.py
```

The generator verifies upstream checksums and reproduces the vendored sources,
local engine change, production data, and the separate `Australia/Lord_Howe`
host-fixture database. Regeneration does not use the host's installed TZDB.
The offline validator compares both databases with checked-in IANA reference
values, checks whitelist parity, rejects unsupported names/dates, and verifies
that formatting and local-time conversion code is absent from the linked core.

Reference values were generated independently with `zic` from IANA 2025b at
revision `7e1145bfdb9630c127841dc8ce808a937a300938`. To reproduce them:

```sh
python3 micro-thread-matter-daemon/scripts/test-acetime-subset.py \
  --iana-source /path/to/tz-2025b --write-reference
```

Without `--write-reference`, `--iana-source` checks that regeneration matches the
saved fixture. Input-file checksums are recorded in that fixture. Reference
cases cover 2026–2028 transition instants and representative dates through 2099.
MIPS object sizes and the single processor workspace can be measured in the
Cargo build's `libgateway_timezones.a`; the standalone validator reports the
host workspace separately. Host pointer widths differ from the gateway's.
