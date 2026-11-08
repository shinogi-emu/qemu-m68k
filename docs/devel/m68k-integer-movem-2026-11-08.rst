m68k integer and MOVEM throughput follow-up, 2026-11-08
=====================================================

The retained changes improve long quick-arithmetic flag handling and remove
redundant StoreLoad fences between consecutive guest reads. This is incremental
to the earlier soft-TLB coverage optimization. The baseline is public commit
8c2975f8ce2, with that optimization already enabled. No installed executable,
shipping branch or installer was replaced.

Changes and architectural behavior
----------------------------------

* Fix an existing synchronization defect: after a CCR read has synchronized
  env->cc_op, an inline flag flush must invalidate that encoding before another
  instruction changes individual flags. The new CCR-read/BTST/TB-boundary checks
  fail on the older baseline and pass after the fix.
* For ADDQ.L/SUBQ.L #1, keep the source operand implicit in the lazy condition-code
  encoding. This removes a repeated cc_v store. Eager X stays a canonical boolean;
  N/Z, carry, overflow and preservation of X retain their architectural meaning.
  Existing enum values and migration fields are unchanged. New encodings are
  converted to canonical CCR state before saving, so older receivers can load them.
* Track whether a pure sequential StoreLoad fence has already ordered preceding
  stores in the current basic block. Repeated fences of exactly that type can be
  removed across loads. Guest stores, arbitrary host stores, helper calls and basic
  block boundaries clear the tracking state. Other barrier masks remain intact.
  Every memory access still uses the normal soft-TLB, MMIO, watchpoint, dirty-page,
  fault and self-modifying-code machinery. This does not relax guest page ordering.

The instruction semantics were checked against Motorola's M68000 Programmer's
Reference Manual, ADDQ (PDF page 115) and SUBQ (PDF page 285), obtained through the
local manual index. The MC68060 User's Manual section 5.4.2 describes precise and
imprecise ordering. The retained optimization preserves the existing requested
StoreLoad ordering; it does not assume all RAM is imprecisely ordered.

Method and 68060 results
------------------------

Harness commit 1651f58 on shinogi's perf/m68k-copy-kernels branch adds the scalar
and MOVEM copy kernels, a 68040 bootstrap, flag checks, emitted-code fence checks,
and migration checks. All timed workloads warm up, emit B/E markers, validate
results, and require P before a sample is accepted. Copy destinations are cleared
between warmup and measurement and fully checksummed after timing. Scalar and
MOVEM kernels each copy 1 GiB through the same 256 KiB source/destination buffers.

Five alternating-order samples per case and engine ran on host logical CPU 31.
The host is an AMD Ryzen 9 9950X3D2. Numbers below are median host seconds; larger
baseline/new ratios mean greater throughput. The adjacent JSON contains all raw
samples, commands, hashes, iteration counts and ranges. Hardware perf counters
were unavailable; these are elapsed-time and emitted-code observations.

.. list-table:: 68060 median seconds
   :header-rows: 1

   * - Workload
     - QEMU baseline
     - QEMU new
     - Linux WinUAE JIT
     - Baseline/new
   * - integer
     - 0.487355
     - 0.441104
     - 0.439029
     - 1.105x
   * - branch
     - 0.490401
     - 0.465309
     - 1.315544
     - 1.054x
   * - shift
     - 0.243300
     - 0.223227
     - 0.225224
     - 1.090x
   * - memory
     - 0.338906
     - 0.338965
     - 0.180241
     - 1.000x
   * - fpu
     - 1.381309
     - 1.377219
     - 1.666838
     - 1.003x
   * - memory-offset
     - 0.341934
     - 0.341717
     - 0.181466
     - 1.001x
   * - memory-flush
     - 0.346784
     - 0.347038
     - 0.181271
     - 0.999x
   * - memory-copy
     - 1.315756
     - 1.320813
     - 0.585197
     - 0.996x
   * - memory-movem
     - 1.105656
     - 0.282607
     - 0.484884
     - 3.912x

Integer throughput is within about one percent of this Linux WinUAE JIT run.
MOVEM copying is about 1.7 times faster than that reference. Scalar copying is
unchanged by the fence optimization and still trails WinUAE. These are focused
synthetic kernels, not a claim of general application parity. The pinned Unix
WinUAE d82bc2549f27943d11dc59482d5388daf1c0f337 uses indirect JIT memory
(NATMEM_NO_JIT_DIRECT), a 16 MiB cache and native JIT FPU. Native Windows WinUAE
with direct JIT memory remains a separate comparison. Exact finite FPU operands
do not establish general floating-point semantic equivalence.

68040 results
-------------

The same public executable was run as m68040 against separately built images
using --bootstrap-cpu m68040, omitting the 060-only PCR setup. Timed work and result
checks are otherwise the same. Five alternating samples per case used CPU 31.

.. list-table:: 68040 median seconds
   :header-rows: 1

   * - Workload
     - QEMU baseline
     - QEMU new
     - Baseline/new
   * - integer
     - 0.488476
     - 0.442593
     - 1.104x
   * - branch
     - 0.491283
     - 0.467504
     - 1.051x
   * - shift
     - 0.245042
     - 0.222625
     - 1.101x
   * - memory
     - 0.339476
     - 0.339682
     - 0.999x
   * - fpu
     - 1.378579
     - 1.373266
     - 1.004x
   * - memory-copy
     - 1.305258
     - 1.305907
     - 1.000x
   * - memory-movem
     - 1.102617
     - 0.280953
     - 3.925x

Validation and responsiveness
-----------------------------

The final build passes 191 Meson tests (zero failures, seven skips), the existing
34-case 68060 architectural guest, and 1,320 new result/CCR/condition/X checks on
each of m68060 and m68040. The x86 emitted-code regression verifies one StoreLoad
fence for consecutive MOVEM reads and retained fences after a guest store, helper
call and control-flow boundary; it also executes MMIO reads. This is a regression
check, not an exhaustive concurrent litmus proof or a weak-host validation.

All four ADDQ/SUBQ carry/overflow edge states migrate successfully from the new
build to the older baseline while DBRA loops preserve the pending flags. A real
wrong-checksum workload is rejected. Disposable MiNT/EmuTOS storage and virtio-net
checks exercise write/read-only/reopen/fsck paths, link restart, 96 UDP requests
and bidirectional 2 MiB TCP transfers.

An ABBA 060 desktop comparison on host CPU 9 uses fresh guest copies and confirms
XaAES launched the shell. Baseline boots take 4.184/4.176 seconds; candidate boots
3.833/3.811 seconds. Input-to-frame medians are 21.845/21.021 ms for baseline and
20.927/17.113 ms for candidate. Motion stays about 50 Hz and idle CPU stays low.
The final 040 desktop also boots, with a measured 18.581 ms input-frame median.
These headless QMP measurements exclude physical display/compositor and host
input latency. The short ABBA sample supports no responsiveness regression,
not a precise universal latency improvement.

Rejected experiments
--------------------

ADD/SUB carry generation through add2/sub2, equality-based carry for quick one,
and replacing MOVBE with MOV+BSWAP did not improve the target arithmetic/scalar
copy workloads reliably. They are absent from the retained implementation.
