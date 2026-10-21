#!/usr/bin/env python3
"""Collect radio-only notices from the generated build's dependency/link inputs.

This creates attribution material, not redistribution clearance for MSLA code.
The repository's firmware publication gate in THIRD_PARTY.md still applies.
"""
import hashlib
from pathlib import Path
import re
import shlex

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "firmware/dependencies/gecko-sdk"
ARM = ROOT / "tools/silabs/arm-gnu-toolchain-12.2.rel1-x86_64-arm-none-eabi"
SUPPLEMENTS = {
    "GCC-RUNTIME-EXCEPTION.txt": "28e85c5aa4af9b4f1dfe6b4817aa3eefb3eaaee7fd735045016f29ccf50276a1",
    "GPL-3.0.txt": "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903",
    "NEWLIB-COPYING.txt": "8b39f361bd0b42e7fbc92c6be8e52a3d750f646233e3f32f2e58e91b3f3c1716",
}
SDK_ARCHIVES = {
    "platform/emdrv/nvm3/lib/libnvm3_CM4_gcc.a",
    "protocol/openthread/libs/libsl_openthread_efr32mg1x_gcc.a",
    "platform/radio/rail_lib/autogen/librail_release/librail_efr32xg1_gcc_release.a",
}
ARM_INPUTS = {"libgcc.a", "libc_nano.a", "libstdc++_nano.a", "libm.a", "libnosys.a", "crt0.o", "crti.o", "crtn.o",
              "crtbegin.o", "crtend.o"}


def leading_notice(path):
    """Keep initial comments verbatim, including a second licensing block."""
    text = path.read_text(errors="strict").lstrip("\ufeff")
    end = 0
    while True:
        match = re.match(r"\s*(/\*.*?\*/|//[^\n]*(?:\n|$))", text[end:], re.S)
        if not match:
            break
        end += match.end()
    return text[:end].strip()


def collect(build, arm=ARM):
    build, arm = build.resolve(), arm.resolve()
    linker_map = (build / "build/debug/ot-rcp-multipan.map").read_text()
    loads = re.findall(r"^LOAD (.+)$", linker_map, re.M)
    if not loads:
        raise ValueError("No linker inputs; cannot generate firmware notices")
    libraries = set()
    for item in loads:
        if item == "linker stubs":
            continue
        path = Path(item)
        path = (path if path.is_absolute() else build / path).resolve()
        if path.is_relative_to(SDK):
            name = path.relative_to(SDK).as_posix()
            if name not in SDK_ARCHIVES:
                raise ValueError("Unreviewed SDK link input: " + name)
            libraries.add(name)
        elif path.is_relative_to(arm):
            if path.name not in ARM_INPUTS:
                raise ValueError("Unreviewed ARM runtime: " + path.name)
            libraries.add("Arm GNU 12.2.rel1/" + path.name)
        elif not path.is_relative_to(build) or path.suffix != ".o":
            raise ValueError("Unreviewed linker input: " + str(path))
        if not path.is_file():
            raise ValueError("Missing linker input: " + str(path))
    dependencies = set()
    depfiles = list((build / "build/debug").rglob("*.d"))
    if not depfiles:
        raise ValueError("Missing compiler dependency files for radio license inventory")
    for depfile in depfiles:
        content = depfile.read_text().replace("\\\n", " ")
        # GCC -MP adds empty rules after the first dependency rule.
        rule = content.splitlines()[0].split(":", 1)[1]
        for item in shlex.split(rule):
            path = Path(item)
            path = (path if path.is_absolute() else build / path).resolve()
            if not path.is_file():
                raise ValueError("Missing compiled input: " + str(path))
            if not any(path.is_relative_to(root) for root in (SDK, build, arm)):
                raise ValueError("Unreviewed compiled input: " + str(path))
            if path.is_relative_to(SDK / "util/third_party") and not any(
                    path.is_relative_to(SDK / "util/third_party" / name)
                    for name in ("openthread", "mbedtls", "printf")):
                raise ValueError("Unreviewed third-party source: " + str(path))
            dependencies.add(path)
    sections = ["# EFR32 radio firmware licenses and notices\n\n"
                "This application is for the Silicon Labs EFR32MG1B232 in the supported gateway.\n"
                "Original project tooling is MIT licensed. The firmware is a combined work;\n"
                "SDK and third-party code retain their own terms. Silicon Labs material\n"
                "is subject to its SDK License.txt, per-file exceptions and the MSLA:\n"
                "https://www.silabs.com/about-us/legal/master-software-license-agreement\n\n"
                "Use Silicon Labs material only with authorized Silicon Labs applications.\n"
                "These notices do not grant additional redistribution rights. Publication\n"
                "requires the firmware review described in THIRD_PARTY.md.\n\n"
                "The generated inventory conservatively covers compiled units and headers,\n"
                "including code removed by linker garbage collection. Header comments below\n"
                "are preserved verbatim; repeated identical notices are grouped.\n\n"
                "Local changes include the gateway startup delay, multipan configuration,\n"
                "bootloader-entry support and configurable radio logging.\n\n"
                "## Linked libraries and startup objects\n\n" +
                "\n".join("- " + name for name in sorted(libraries))]
    texts = [("Project MIT license", ROOT / "LICENSE"),
             ("Gateway reference project MIT license", ROOT / "firmware/dependencies/rtl8196e-gateway/LICENSE"),
             ("Gecko SDK license and library exceptions", SDK / "License.txt"),
             ("OpenThread license", SDK / "util/third_party/openthread/LICENSE"),
             ("OpenThread NOTICE", SDK / "util/third_party/openthread/NOTICE"),
             ("Mbed TLS Apache-2.0 license", SDK / "util/third_party/mbedtls/LICENSE"),
             ("CMSIS Apache-2.0 license", SDK / "platform/CMSIS/LICENSE.txt")]
    for name, checksum in SUPPLEMENTS.items():
        path = ROOT / "firmware/release/licenses" / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise ValueError("Runtime license supplement changed; review required: " + name)
        texts.append((name, path))
    for title, path in texts:
        text = path.read_text().strip()
        if not text:
            raise ValueError("Empty license: " + str(path))
        sections.append("## " + title + "\n\n```text\n" + text + "\n```")
    headers = {}
    for path in sorted(dependencies):
        if path.is_relative_to(arm):
            name = "Arm GNU 12.2.rel1/" + path.relative_to(arm).as_posix()
        elif path.is_relative_to(SDK):
            name = "Gecko SDK/" + path.relative_to(SDK).as_posix()
        else:
            name = "Generated project/" + path.relative_to(build).as_posix()
        notice = leading_notice(path)
        # Generated configuration/main files can lack a header; they inherit
        # SDK/reference-project terms. Record that explicitly rather than MIT.
        if not notice:
            if path == SDK / "platform/service/legacy_hal/inc/platform-header.h":
                notice = "No per-file exception: Gecko SDK default Silicon Labs MSLA applies (SDK License.txt)."
            elif not path.is_relative_to(build):
                raise ValueError("No source notice; review required: " + name)
            else:
                notice = "Generated/SDK-derived project input; see Gecko SDK and gateway reference terms above."
        headers.setdefault(notice, []).append(name)
    for notice, names in sorted(headers.items(), key=lambda entry: entry[1][0]):
        sections.append("## Input notices\n\n" + "\n".join("- " + name for name in names) +
                        "\n\n```text\n" + notice + "\n```")
    sections.append("## Host serial bridge (downloaded separately)\n\n"
                    "Paul Banks, lidl-gateway-freedom, Release-1.2; GPL-3.0.\n"
                    "The bridge is downloaded from upstream, not redistributed in this archive.\n"
                    "Source and license: https://github.com/banksy-git/lidl-gateway-freedom/tree/Release-1.2\n"
                    "The host Python packages retain the licenses installed with their wheels.")
    return "\n\n".join(sections) + "\n"
