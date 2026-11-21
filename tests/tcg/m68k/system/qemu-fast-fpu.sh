#!/bin/sh
#
# Run qemu-system-m68k with fast-fpu forced on, to put the whole m68k system
# test suite through the fast FPU path:
#
#   QEMU_FAST_FPU_BIN=<build>/qemu-system-m68k \
#       make -C <build>/tests/tcg/m68k-softmmu run QEMU=<this script>
#
# The global comes last, so it overrides a test's own fast-fpu setting.
#
# SPDX-License-Identifier: GPL-2.0-or-later

exec "${QEMU_FAST_FPU_BIN:?set QEMU_FAST_FPU_BIN to qemu-system-m68k}" \
    "$@" -global m68k-cpu.fast-fpu=on
