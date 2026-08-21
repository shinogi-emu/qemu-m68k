# 68060 support in this fork

Three commits on top of upstream QEMU add a 68060 that behaves like the
part rather than like a 68040 wearing a different name. They live on the
`shinogi-060` branch and touch `target/m68k/` only — no machine, board or
device code.

| commit | subject | scope |
|---|---|---|
| `3d2caab` | complete basic 68060 system support | 88 lines |
| `d6d0717` | complete 68060 instruction and exception behavior | 482 lines |
| `3dc4786` | defer logical shift condition codes | 40 lines |

## The thing to understand first

The 68060 **removed** instructions its predecessors had. Motorola dropped
them from the silicon and expected the operating system to install a
software package that emulates them (the Motorola 060SP). On real
hardware those instructions raise an exception and the package services
it.

So when this document says an instruction is unimplemented, it means
**the chip does not implement it**. Modelling that — raising the right
exception, with the right frame, at the right time — is the work. An
emulator that quietly executed those instructions would be the inaccurate
one: it would run guest software that fails on the real part, and hide
the difference from anyone testing against it.

Upstream QEMU's `m68060` is largely a 68040 with a different feature bit,
which is enough to boot something and not enough to be told the truth by.

## What this fork models

**Integer instructions the 060 dropped.** `M68K_FEATURE_QUAD_MULDIV` is
unset for the 060, so 64-bit `mulu.l`/`muls.l`/`divu.l`/`divs.l` raise
`EXCP_UNIMP_INT` rather than executing. The 32-bit forms, including
`divsl.l` and `divul.l` with a remainder, stay in hardware — matching the
part, which kept those.

**Floating-point instructions the 060 FPU dropped.**
`m68060_fpu_unimplemented()` enumerates them: the transcendentals
(`fsin`, `fcos`, `ftan`, `fasin`, `facos`, `fatan`, `fsinh`, `fcosh`,
`ftanh`, `fatanh`, `fetox`, `fetoxm1`, `ftwotox`, `ftentox`, `flogn`,
`flognp1`, `flog10`, `flog2`), plus `fmod`, `frem`, `fscale`, `fgetexp`,
`fgetman` and `fsincos`. These raise `EXCP_060_FPU_UNIMP` through the
line-F vector, where the software package installs itself.

**Addressing modes the 060 FPU dropped.** `EXCP_UNIMP_EA` for the forms
it does not decode, including extended and packed operands with immediate
addressing, and `fmovem` with a dynamic register list.

**The FPU can be switched off.** PCR bit 1 (DFP) is honoured: with it
set, every floating-point instruction raises `EXCP_060_FPU_DISABLED`.
The state is carried into translation as `TB_FLAGS_060_DFP` so the check
costs nothing per instruction. PCR resets to `0x04300000` — revision
field populated, control bits clear.

**Exception frames.** `FPIAR` holds the faulting instruction while the
exception's PC is the following one, as on the part. That distinction is
not cosmetic: an operating system that reports "the PC" is reporting the
instruction *after* the one that faulted, and anyone debugging from that
report needs to know it.

**MOVEC.** The 060's control register set, and faults rather than silent
zeroes for the registers it does not have.

## `3dc4786`, and why an optimisation is in the same series

`defer logical shift condition codes` is performance work, not accuracy:
it defers flag computation for logical shifts the way the existing
arithmetic paths already do. It is here because the guest workload is
dominated by a 16-to-32-bit pixel loop whose translated block shrank
measurably as a result.

## Limits

- **The 060SP is not emulated, deliberately.** Raising the exceptions
  faithfully is the contract; servicing them is the guest's job. A guest
  without the package installed will fail, and that is correct — the same
  guest fails on real hardware.
- **Cache and superscalar behaviour are not modelled.** Instruction
  counts are representative; cycle counts are not, and nothing that
  depends on the 060's pairing rules or branch prediction is reproduced.
  Timing measured under emulation is not a hardware result.
- **Rebasing touches `translate.c`,** which is churn-prone upstream.
  Expect the instruction-behaviour commit to need the most attention.
