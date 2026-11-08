m68k throughput and software TLB coverage
=========================================

The ``perf/m68k-soft-tlb-coverage`` branch retains at least 1024 soft TLB
entries per m68k MMU mode.  This reduces direct-mapped cache conflicts
between large RAM buffers.  It improves the measured conflicting-buffer
copy workload by about 6.8 times; this is not an overall emulator speedup.
The shipping branch and installed binaries are unchanged.

The common TCG interface accepts an optional preferred minimum.  Targets
that leave it zero retain the previous defaults.  m68k's 1024-entry hint
covers 4 MiB with its 4 KiB page granule.  Initialization and subsequent
resizing honor it, so guest flushes do not undo the coverage.  Allocation
failure retains the common fallback down to the existing minimum.  No
guest MMU state, exception behavior, memory ordering or device contract is
changed.  The larger tables add 768 entries per MMU mode and increase
allocation and flush work; real application and platform testing remains
necessary before promotion.

Comparison, 2026-11-08
----------------------

The reference is the validated QEMU 11.1.3 integration at ``74b14cd1b214``;
the performance change is ``436b7a9cc96``.  The harness is
``shinogi-emu/shinogi`` commit ``3685818`` under ``tests/cpu-throughput``.
WinUAE is pinned at ``d82bc2549f27943d11dc59482d5388daf1c0f337``.
A small documented headless-build adapter supplies two missing CLI logging
globals without modifying its CPU, JIT, memory-access or FPU code.

All arms executed identical workload bytes at guest address 0x200000,
including explicit 060 PCR/CACR initialization, warmup and checked results.
Five repetitions of each arm ran on logical CPU 31 with alternating order.
Elapsed time is host monotonic time between guest console markers; startup,
initialization, validation and shutdown are excluded.  Other host activity
was not reserved.  The companion JSON retains every sample, range,
workload hash and executable hash.  All 105 samples completed successfully.

Medians in seconds, lower is better:

.. list-table::
   :header-rows: 1

   * - Workload
     - QEMU baseline
     - QEMU candidate
     - WinUAE JIT
   * - integer
     - 0.507628
     - 0.510861
     - 0.452910
   * - branch
     - 0.494330
     - 0.493640
     - 1.333869
   * - shift
     - 0.245656
     - 0.246295
     - 0.227882
   * - memory
     - 2.338686
     - 0.342778
     - 0.181288
   * - fpu
     - 1.389844
     - 1.385211
     - 1.678700
   * - memory-offset
     - 0.347295
     - 0.343983
     - 0.181287
   * - memory-flush
     - 2.323450
     - 0.349147
     - 0.181642

``memory`` copies/checksums 256 KiB per pass between buffers one MiB apart.
With 256 entries and 4 KiB pages, their corresponding pages hash to the
same TLB slot.  ``memory-offset`` changes only the destination by 4 KiB;
its faster baseline separates this conflict from sequential RAM traffic.
``memory-flush`` executes PFLUSHA before each pass.  The retained floor
preserves the improvement under that repeated-flush workload too.

QEMU leads the selected branch and exact-native-FPU arithmetic cases in
this configuration.  It remains behind on add/sub, shift and ordinary RAM
traffic.  These focused loops do not establish application-level parity.

WinUAE configuration and interpretation
---------------------------------------

The reference uses its native Linux x86-64 CPU/FPU JIT, 68060 model,
unlimited speed, 16 MiB code cache, no cycle-exact mode and no sound.
Its native FPU mode was selected; the test stays on exact finite values
and does not establish general equality of rounding or exception behavior
with QEMU's SoftFloat implementation.  A same-binary short integer control
measured 0.587 seconds with JIT disabled and 0.0228 seconds enabled,
confirming that the comparison is using accelerated execution.

This Unix build defines NATMEM_NO_JIT_DIRECT and selects indirect JIT
memory access even when backing RAM is directly mapped.  Consequently
these results describe this pinned Linux WinUAE configuration.  Native
Windows WinUAE with direct JIT memory still needs comparison; no Windows
performance-parity claim follows from this run.

Validation
----------

* Build with warnings as errors, SDL/slirp/virtfs/plugins enabled.
* Upstream Meson tests: 191 passed, zero failed, seven skipped.
* The 68060 architecture/exception/flags/FPU/LPSTOP suite passed.
* Three real guest storage boots passed, including writes, read-only
  preservation, reopening data and filesystem checks.
* Real guest traffic passed 96 checked UDP exchanges and 2 MiB TCP
  transfers in both directions before/after interface restart.
* Separate 040 and 060 guest desktop boots passed.  The 060 ABBA
  responsiveness check on logical CPU 9 measured baseline boots of
  4.06/4.20 seconds and candidate boots of 4.14/4.09 seconds; per-run median
  input-to-frame latency stayed around 22 ms and motion presentation
  around 50 Hz.  This is the headless QMP path, not physical SDL input or
  a host display/compositor measurement.
* A deliberately wrong completed guest checksum was rejected by the
  throughput runner.

The candidate executable was tested before committing the same source
tree; later builds may change version metadata.  Host perf sampling was
unavailable under the current kernel policy.  TCG IR/host-code inspection,
address-alignment controls and repeated elapsed-time measurements supplied
the evidence for this change.

Remaining native-platform checks are tracked as ``shin-cjfz``; further
throughput work and native Windows comparison are ``shin-xj7m`` in the
Shinogi issue tracker.  Preserve the guest/device correctness gates while
closing the remaining gap.
