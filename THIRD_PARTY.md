# Third-party software and attribution

The native gateway embeds Lua (MIT, Lua.org/PUC-Rio); its pinned source and
copyright notice are included in release source bundles and gateway notices.

Original MicroHAD project code and documentation are [MIT licensed](LICENSE),
Copyright (c) 2026 MicroHAD contributors. This does not relicense dependencies,
SDK-generated files, copied upstream material, or modifications derived from
upstream code. Preserve their existing copyright headers and licenses. References
to Matter, Thread, Zigbee, Silicon Labs, Tuya, Silvercrest, Shelly and FRITZ!Box
identify compatibility or upstream projects, not endorsement or certification.

## Inventory and version sources

The parent repository's gitlinks in [.gitmodules](.gitmodules) pin dependencies;
use `git submodule status --recursive` for exact revisions. Do not substitute
versions named in old validation reports for the current pins. Lockfiles pin
package versions. [Recorded notice inputs](third-party/gateway-inputs.json)
identify the revisions and lockfile hashes used for the shipped gateway notices.

| Component / upstream | Used by | License and attribution |
| --- | --- | --- |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) and nested TF-PSA-Crypto | MicroHAD static TLS/crypto libraries | Apache-2.0 selected from Apache-2.0 OR GPL-2.0-or-later; retain applicable notices and license texts, including bundled third-party material. |
| [AceTime C](https://github.com/bxparks/acetimec) | Matter accessory clock offsets; restricted 26-zone database | MIT (Brian T. Park); IANA data public domain. Pinned submodule at `third-party/acetimec/upstream/`; checksums, generated data and the offsets-only mailbox are in `third-party/acetimec/`. |
| [nanopb](https://github.com/nanopb/nanopb) | MicroHAD protobuf IPC and RAM cache | Zlib; pinned submodule at `third-party/nanopb/`, including the generator and `LICENSE.txt`. |
| [Ziggurat](https://github.com/zigpy/ziggurat) | Zigbee stack and shared radio transport | Apache-2.0; local patch series under `ziggurat/patches/` identifies changes. Rust transitive dependencies have their own terms. |
| [rs-matter](https://github.com/project-chip/rs-matter) | Matter service | Apache-2.0; modified through `micro-thread-matter-daemon/patches/`. RustCrypto, embassy, serde and other transitive dependencies retain their individual terms. |
| [OpenThread](https://github.com/openthread/openthread) | Matter userspace IPv6/Thread | BSD-3-Clause plus nested component terms; preserve its LICENSE and NOTICE, including specification/IP and certification limitations. Local patches are under the Matter component. |
| [Gecko SDK](https://github.com/SiliconLabs/gecko_sdk) | EFR32 radio firmware | Mixed: default Silicon Labs MSLA, per-file Zlib/BSD and other exceptions. Preserve headers, SDK License.txt and applicable third-party notices. This is not uniformly MIT or Apache licensed. |
| [RTL8196E gateway project](https://github.com/jnilo1/rtl8196e-gateway) | Firmware source/configuration, boot delay, recovery references | Root MIT, Copyright (c) 2024 jnilo1; SDK-derived files, bundled images and vendor libraries retain their upstream terms. A root MIT file does not establish redistribution rights for every binary. |
| [SLC CLI](https://github.com/SiliconLabsSoftware/circuitpython_slc_cli_linux) | Firmware generation, host only | Root MIT, Copyright (c) 2023 Silicon Labs; bundled Python/Java/native dependencies retain their licenses. Do not redistribute the tool bundle using only its root license. |
| React, React DOM, scheduler, loose-envify, js-tokens | Browser bundle | MIT, with individual copyright notices. Exact production dependency closure: `microhad/webui/package-lock.json`. |
| Chart.js and @kurkle/color | Browser charts | MIT; preserve their individual copyright and license texts. |
| AndroidX / Compose, Kotlin/coroutines, Ktor, OkHttp/Okio and other resolved Android runtime libraries | Companion APK | Predominantly Apache-2.0, with dependency-specific exceptions. The build resolves the runtime graph and embeds full notices; POM labels alone do not replace license texts. |
| Firebase Messaging / Google Play services / Google code scanner | Companion APK | Mix of open-source dependencies and Google SDK/ML Kit terms. Preserve packaged third-party notices and review the applicable Google SDK terms; do not describe the complete SDK set as MIT. |
| Gradle wrapper and Android/Kotlin build plugins | Android build | Apache-2.0 and component terms; wrapper sources retain upstream headers. Build tools are not app runtime libraries. |
| Vite, TypeScript, type definitions, npm build dependencies | Web build, host only | Individual package licenses; excluded from the runtime notice bundle unless their code is actually bundled. |
| GCC, binutils, Rust/LLVM, Arm GNU toolchain, RSDK, crosstool-NG, uClibc/uClibc-ng | Build tools and linked runtimes | Mixed GPL/LGPL/permissive licenses and runtime exceptions. `config/toolchains.mk`, `rust-toolchain.toml` and `config/crosstool-ng/sources.json` record versions. The radio release uses Arm GNU runtime libraries with the GCC Runtime Library Exception and Newlib notices. |
| connectedhomeip / CHIP Tool | Optional host diagnostics | Apache-2.0 plus third-party terms; exact checkout pin and local changes are in `scripts/matter-diagnostics/build-controller.sh`. Its dependency closure differs from the gateway service. |
| ot-br-posix, OpenThread, Ubuntu and external OTBR container | Optional host diagnostics | BSD-3-Clause and component/image-specific licenses. Dockerfile pins identify inputs. Container redistribution includes OS packages and is a separate license/source audit. |
| Python cryptography, Docker, CMake, Ninja, system build/test packages | Host verification | Their own licenses; no claim that host tools are included in the gateway. |

## What attribution requires

A credits list or upstream link is not a substitute for required license text.
MIT and BSD distributions retain copyright notices, conditions and disclaimers.
Apache-2.0 distributions include the license, retain relevant notices, carry
applicable upstream NOTICE attributions, and identify modified upstream files.
Existing headers and patch series must remain intact. Do not replace an upstream
NOTICE with a MicroHAD copyright statement. No generic advertising credit or logo
is required by these permissive licenses; trademark rights are separate.

See the [Apache license, section 4](https://www.apache.org/licenses/LICENSE-2.0.html),
[Google Android notice guidance](https://developers.google.com/android/guides/opensource),
and [Silicon Labs MSLA](https://www.silabs.com/about-us/legal/master-software-license-agreement).

## Notice generation and delivery

Python 3.11 or later is required. The reviewed gateway snapshot is tracked so a
Web UI build does not require Rust caches or internet access. Its structured records and plain-text notices share the production MIPS
dependency inventory for the native daemons, Ziggurat, Matter service and Web UI.
Host tools, proc macros, tests, probes, unrelated platforms and radio firmware
are excluded. Firmware notices accompany firmware downloads separately.
The radio release builder, `firmware/scripts/build-release.py`, writes its own
`license.md` beside the application-only GBL, standalone flasher and README in
a tar.gz package. It collects SDK/per-file notices from the generated compiler
dependencies and linker inputs, together with the ARM linked-runtime license
texts. See [firmware releases](firmware/README.md#build-a-firmware-release).
Verbatim identical license texts are deduplicated, with references retaining
package identity. Distinct copyright notices remain separate.

After changing dependencies, initialize the pinned submodules, install the Web UI
lockfile (`npm ci` in its directory), and prepare/build the production Matter
service and multipan Ziggurat using the root build instructions. Refresh uses
their prepared manifests and pinned crate patches through offline, locked Cargo
dependency trees. Then run from the repository root:

```sh
python3 scripts/licensing/notices.py fetch-crates
python3 scripts/licensing/fetch-supplements.py
python3 scripts/licensing/notices.py refresh
python3 scripts/licensing/notices.py check
```

Fetching is explicit. Crate archives are verified against Cargo.lock checksums.
Some crates omit license files; reviewed supplemental texts and their immutable
upstream source revisions/checksums are recorded in
[third-party/supplements.json](third-party/supplements.json). Where upstream also
omits a license file, the declared license's standard text and declared authors
are supplied explicitly, without inventing a copyright holder. Review changes
to these exceptions before committing. Missing material must fail generation. Author credits come from package metadata
or reviewed overrides in `third-party/authors.json`, based on upstream copyright
notices, repository identity and (for Ziggurat) pinned Git author history. These
credits supplement the verbatim notices; they do not replace them.

The Web UI build creates `licenses.html`, accessible through “Open-source
licenses” at the bottom of the Gateway tab. It renders the same inventory as
the release notice file, with authors and expandable license texts. It is uploaded
only as an ordinary Web UI asset. **Do not deploy standalone license/notice files
to the gateway.** Service-only updates do not upload notice files; if dependency
versions change, rebuild and distribute the matching Web UI as well.

The Android build generates offline notices from each variant's resolved runtime
artifacts, Maven POM license declarations and embedded license/NOTICE resources.
The app exposes them before gateway connection. Test dependencies are excluded;
debug notices include debug-only runtime dependencies. Do not silently exclude
unrecognized SDK terms or inherited Maven licenses.

For complete static software releases, use `python3 scripts/build-release.py
--version VERSION`. The builder includes runtime attribution and a matching
source/rebuild package for crosstool-NG/uClibc-ng, supports paired and stock-EZSP
MicroHAD-only modes, and excludes radio firmware. See
[binary release instructions](docs/binary-releases.md). RSDK and vendor artifacts
are not published as part of this project. Radio firmware uses the separate
release process described above.

For other downloadable artifacts, accompany them with a host-side notice
bundle (never passed to deployment):

```sh
python3 scripts/licensing/notices.py release --output build/release-notices
```

Include that directory alongside every separately downloadable gateway service,
and retain the original project MIT license with APK/source releases. Radio-only
firmware uses the radio builder's `license.md`; the gateway bundle excludes
radio inputs and cannot replace it. Notices may cover more components than a
particular release uses.
