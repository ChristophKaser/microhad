# Isolated TLS CPU benchmark

`microhad/scripts/benchmark-tls.py` builds a dummy server against the selected
MicroHAD Mbed TLS libraries. It includes the existing TLS server implementation
without modifying it. It never loads gateway configuration or radio/network
state, and never deploys or restarts production services.

Run from the repository root:

```sh
# Local validation; requires OpenSSL and Node with Playwright installed.
python3 microhad/scripts/benchmark-tls.py --iterations 2 --record-iterations 1 \
  --output /tmp/tls-host.json

# Explicit device selection is required. Credentials/toolchain use workspace settings.
python3 microhad/scripts/benchmark-tls.py --device GATEWAY_ADDRESS \
  --output /tmp/tls-device.json
```

Set `PLAYWRIGHT_MODULE` to an existing Playwright module directory when it is
not available through Node's module search path. `PLAYWRIGHT_CHROMIUM` optionally
selects an installed Chromium executable. `--skip-browser` runs only the
controlled client. `--port` selects the temporary listener (default 18443).
A port collision fails without replacing the existing listener.

The device run uploads the static executable and generated test certificates
into a unique private `/tmp` directory. The dummy runs at reduced priority,
with a 180-second CPU limit per cipher, a 600-second lifetime limit, and socket
timeouts. Its SSH parent terminates only its own child before cleanup. PIDs,
start times, and HTTP/HTTPS listener inodes are compared before and after.
The dummy exposes synthetic data only; production private keys are never read.

## Measurements

The TLS profile matches the server: TLS 1.2, P-256 ECDHE, session-ID caching,
no tickets, and one HTTP response per connection. The matrix uses generated
P-256 ECDSA and RSA-2048 certificates, each with AES-128-GCM or
ChaCha20-Poly1305. Unsupported combinations are reported explicitly.

For each combination there are two warm-ups, 20 full handshakes, a separate
session seed, and 20 resumed handshakes. Python/OpenSSL's reuse indicator is
checked against the server cache callback. Initialization is timed separately.
Three transfers each at 1 KiB and 16 KiB record sizes measure record-write CPU;
each transfer writes 64 records. Headers and the handshake are outside that
measurement. Transfer wall throughput also includes socket backpressure.

CPU is measured inside the single-threaded dummy using the process CPU clock,
falling back to `getrusage`; elapsed time uses the monotonic clock. Older kernels
can quantize these measurements to 10 ms, so small differences and individual
resumed samples should not be overinterpreted. Raw samples and median/p95/mean
handshake times are retained in the JSON report.

Playwright opens two fresh Chromium contexts per combination. Each navigates
once and makes ten sequential same-origin fetches. Certificate errors are
ignored for these synthetic certificates. Browser handshakes include speculative
or aborted attempts: summaries separate successful connections, cache hits,
and failed-attempt CPU. A successful handshake with a cache hit is the browser
resumption observation; the controlled client independently validates this cache
path. Browser behavior with a trusted production certificate can differ.

Results describe the dummy and this Chromium version, not the resumption rate
of an existing production browser session. All original gateway services remain
running and can affect wall-clock timings.

## Gateway results (2026-11-03)

The 20-sample run on the gateway, with original services running, measured:

| Certificate / cipher | Full handshake CPU, median | Resumed CPU, median | Record CPU, 16 KiB blocks |
|---|---:|---:|---:|
| ECDSA P-256 / AES-128-GCM | 240 ms | 10 ms | 2.86 ms/KiB |
| ECDSA P-256 / ChaCha20-Poly1305 | 240 ms | 10 ms | 0.98 ms/KiB |
| RSA-2048 / AES-128-GCM | 1210 ms | 20 ms | 2.62 ms/KiB |
| RSA-2048 / ChaCha20-Poly1305 | 1210 ms | 20 ms | 0.99 ms/KiB |

Chromium 136.0.7103.25 resumed 20 of 22 successful connections for every
combination (two fresh contexts, ten subsequent requests each). All controlled
resumption attempts succeeded; none of the full-handshake controls resumed.
The browser also made 22–23 failed/aborted handshake attempts per combination;
these consumed another 0.59–0.61 CPU seconds with ECDSA or 2.61–2.65 CPU seconds
with RSA. The accepted-connection resumption count alone excludes that overhead.

ChaCha20 reduced large-record CPU by about 62–66% versus AES-GCM. ECDSA reduced
full-handshake CPU by about 80% versus RSA-2048. Resumption reduced handshake
CPU by roughly 96–98%. CPU and wall samples were visibly quantized to 10 ms
on this kernel, despite the CPU clock advertising finer resolution.

Production HTTP/HTTPS listener inodes and the captured service PIDs/start times
were unchanged. The first run exposed a missing stock `rmdir` applet; its empty
temporary directory was removed separately, and the runner now uses `rm -r`
only on the private directory it successfully created. Raw samples are kept
in the ignored local `diagnostics/tls-benchmark-20261103/` directory.
