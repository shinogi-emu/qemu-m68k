Shinogi 68060 integration with QEMU 11.1.3
=========================================

The ``integration/shinogi-060-qemu-11.1.3`` branch ports the ten commits
from the existing ``shinogi-060`` product branch onto the stable QEMU
11.1.3 release.  It is a candidate for platform validation; it does not
replace the shipping branch or installed binaries.

Source revisions
----------------

* Upstream release: ``v11.1.3``, commit
  ``005c61c0ef6f6ae615283440b4bb7be51b2c6ddd``.
* Product source before integration:
  ``a784ec7d4578a52960f7e827928a1bf898c8a8d6``.
* Original product base:
  ``da6c4fe60fee30dd77267764d55b38af9cb89d4b``.
* Product tests and harnesses: ``shinogi-emu/shinogi`` commit
  ``072c19656c7e340c9cef98e12b9c20e0c963d4f2``.
* Guest firmware: ``shinogi-emu/emutos`` commit
  ``6f4b4dd5268e75e3f410fb4d63df1e2563b9d6a7``, ELF SHA-256
  ``082cfdeda24cd1a82bb91564ac8a18e0b6f1da385fe83aefd691679b79545d9a``.
* FreeMiNT guest kernel SHA-256:
  ``1d8d336c87ab5d6193c072049e9935d83db563404d998b1837413af29b57b410``.

The port preserves the 68060 instruction and exception behavior, lazy
logical-shift condition codes, LPSTOP, FMOVEM control-register order,
FPIAR handling, and Cocoa surface byte order.  ``git range-diff`` reports
nine unchanged patches.  The instruction-behavior patch retains its
``accel/tcg/cpu-ldst.h`` include alongside the newer upstream
``accel/tcg/cpu-loop.h`` include; this is the only patch-context adjustment.
No uncommitted work from the older development checkout was included.

TCG scope
---------

The existing logical-shift optimization changes the m68k translator and
its condition-code helpers.  It does not modify the shared TCG optimizer
or host backends.  This integration preserves that optimization and takes
the shared TCG implementation from QEMU 11.1.3.  No new performance gain
has been measured or claimed for this integration.

Linux validation, 2026-11-08
----------------------------

A clean x86-64 build used GCC with warnings treated as errors and these
configuration options::

    ../configure --target-list=m68k-softmmu \
        --enable-virtfs --enable-sdl --disable-gtk --enable-slirp \
        --enable-plugins --audio-drv-list=pa \
        --disable-sndio --disable-jack --disable-oss --disable-docs \
        --disable-guest-agent --disable-tools --disable-vnc
    ninja -j 12
    meson test --no-rebuild --print-errorlogs --num-processes 8

Results:

* Upstream Meson tests: 191 passed, zero failed, seven skipped.  The
  skipped tests were crypto-block, crypto-cipher, crypto-pbkdf, qos-test,
  vmstate, linters, and VNC.  The first sandboxed run could not create
  required sockets and temporary directories; the complete run with
  those facilities available passed.
* ``tests/m68060``: ``M68060 TEST PASS``, including logical-shift flags,
  native and unimplemented instructions, exception frames, PCR.DFP,
  FSAVE, LPSTOP privilege/wakeup, FPSR, FMOVEM control-register order,
  and FPIAR preservation.
* ``tests/drives/check.py``: EmuTOS and FreeMiNT passed on each of three
  boots.  Host checks passed for named folders, independent handles and
  current directories, interleaved DTAs, raw disk images, both floppy
  geometries, writes, read-only contents and hashes, reopened data, and
  ``fsck.fat``.
* ``tools/check-virtio-net.py``: 48 UDP exchanges and checked 2 MiB TCP
  transfers in both directions passed both before and after bringing
  the guest interface down and up.  Host capture verified all 96 UDP
  requests and replies.  Both guest results reported zero receive and
  transmit errors.
* ``tools/run-stack.sh``: separate disposable 68040 and 68060 trees
  booted FreeMiNT and XaAES at 1024 by 768 with 16 bitplanes.  Fresh
  XaAES logs confirmed workstation creation and desktop launch on both
  CPUs; QMP screenshots were inspected.  The 68060 tree included 060SP.
* SDL initialized using its dummy video backend, accepted monitor
  commands, and exited cleanly.  This checks backend startup, not
  physical display or input behavior.

The tested Linux executable SHA-256 was
``70a68f136add19537b61e5c46fe327d52c93a49b346a494bb01b2d61a43755a2``.
All guest writes were confined to disposable copies and generated images.

Promotion requirements
----------------------

Before updating ``shinogi-060`` or shipping new packages, build and check
Windows and macOS, including actual SDL display/input, networking,
storage, and shutdown.  The preserved Cocoa patch requires a macOS
check.  Validate the Linux GTK fallback in an environment with its build
dependencies; GTK was unavailable in the local build environment.
These remaining platform checks are tracked as ``shin-cjfz`` in the
Shinogi issue tracker.
