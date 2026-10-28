# Radio runtime license inputs

These texts supplement the pinned SDK's own license and per-file headers.
`firmware/scripts/release-notices.py` verifies their SHA-256 before including
them verbatim in `license.md`.

- `NEWLIB-COPYING.txt`: `COPYING.NEWLIB` from the official
  [Newlib 4.2.0 source archive](https://sourceware.org/pub/newlib/newlib-4.2.0.20211231.tar.gz),
  covering Newlib/Newlib-nano and libgloss/libnosys notices. Arm GNU 12.2.rel1
  uses Newlib 4.2.0. This intentionally includes notices for additional Newlib
  targets rather than attempting to remove individual copyright holders.
- `GCC-RUNTIME-EXCEPTION.txt`: GCC 12.2.0
  [COPYING.RUNTIME](https://github.com/gcc-mirror/gcc/blob/releases/gcc-12.2.0/COPYING.RUNTIME),
  version 3.1 of the GCC Runtime Library Exception.

The generated notices identify the components covered by each text. For GCC
runtime components they reproduce only the Runtime Library Exception, identifying
the underlying license as GPLv3 with that exception. The full `GPL-3.0.txt`
reference text is retained here but is not included in the generated bundle.

The firmware links GCC runtime/startup objects and Newlib-nano, not the host
compiler binaries. Host build tools, the Gecko SDK distribution and the
separately downloaded GPL serial bridge are not bundled into the release.
