# MPR — Fusion-MPT SAS3 (SAS3004/SAS3008) kext for macOS

Date: 2026-11-13. Driver version: 0.22.0.

MPR is an IOKit block-storage driver for the LSI/Broadcom MPI 2.5 Fusion-MPT SAS3
family on x86_64 macOS. It publishes every eligible independent disk through
`IOBlockStorageDriver` as its own whole-disk `IOMedia`.

## Supported hardware

| Chip | PCI ID | Status |
|---|---|---|
| SAS3008 | `0x1000:0x0097` | verified on hardware |
| SAS3004 | `0x1000:0x0096` | matches; same IOC generation with four phys, not run on hardware |

Matching is by the exact vendor/device pair, so any vendor's board built on these
chips binds. Everything else stays out: the IR-capable SAS3108 variants are not
matched because this driver refuses RAID volumes and their members instead of
publishing them, and the MPI 2.6/tri-mode family and SAS2 are different IOC
generations (see [PLAN.md](PLAN.md) section 1.3). `MPRChipName` reports which
entry bound.

## Status

The current source implements client-buffer DMA, a credit-bounded command pool,
ready-first completion/refill and batched reply retirement with event wakeups.
It is built, linked and signed ad hoc for the test machine (macOS 26.7.2,
SAS3008, one SATA target). The build that machine loads from its internal ESP is
0.21.0, and every runtime result below was measured with it; 0.22.0 only extends
the PCI ID table, adds `MPRChipName` and renames the block-device info string, and
has been built and link-checked but not deployed.

Qualified for this source: read-only block I/O including requests larger than
one command window, chunks at non-zero offsets, several concurrent client
requests, fragmented and page-unaligned client buffers, and byte-exact
comparison against digests recorded under the previous build.

Also qualified for this source: a sustained command storm (21.1 million
commands in 600 s with client-buffer DMA and 10.7 million in 300 s with the
staging path, no error, no reset, flat latency), a small write and flush cycle
with byte-exact readback, and two sleep/wake cycles that each cost exactly one
planned controller rebuild and returned byte-exact reads.

Injected recovery is qualified only as a failure observation: on this expander
path each task-management request that reached the target produced a SAS
removal event, and after twelve of them in one run the medium came back as a
new BSD disk with raw transfers above 128 KiB failing until a reboot. An
IOC-level rebuild that sends no task-management request left the fabric alone.
See "Injected recovery, by escalation level" below.

Not yet qualified for this source: UNMAP, physical removal and reinsertion,
shutdown, and FUA selection (not reachable from user space here). The removal
and multi-target observations below were made with the preceding build and are
regression coverage only. Confirm the actually loaded version with
`kmutil showloaded`; `MPRBuild` and every other IORegistry property are
published only when `mpr_debug=1` is present in boot-args, and kernel log lines
follow the same switch.

### Observations from the preceding build

The following was measured with the preceding implementation, which was also
loaded from the internal ESP on this machine. It defines regression coverage
for behaviour the current source has not re-run; it is not a result for the
current source.

That build was qualified in two configurations: the test SATA disk attached
directly to the SAS3008, and the same disk as the terminal disk behind an
Adaptec AEC-82885T expander and one backplane. Both publish one whole-disk
`IOMedia` and pass bounded read, write, flush and UNMAP with byte-exact
readback, including concurrent reads. Direct attach additionally passed unplug
and replug, including unplug with I/O in flight; reply-timeout injection through
ABORT_TASK, LUN RESET, TARGET RESET and controller reset; sleep/wake; and
shutdown.

With two targets on the expander the driver publishes one nub per target with
its own geometry, INQUIRY identity and completion ownership, and routes every
command by DevHandle. Every published target is writable; the driver keeps no
per-device write allowlist. Write accounting is per target, so each completed
write is attributable to the device that received it.

Pulling either disk while the other stays published was also exercised, with a
read in flight on one target and with a write in flight on the other. The dropped
target is retired with TARGET_RESET, a drain of its aborted commands and
SAS IO Unit Control REMOVE_DEVICE, all naming that handle; its medium goes offline,
the other target keeps serving I/O, and the request in flight fails with a device
error instead of completing silently. A returned disk is re-probed and republished,
matched by SAS address, so the same disk number comes back rather than a second
medium for the same drive. Reinsertion behind this expander is not announced,
so the driver spends at most two bounded IOC resets (8 s and 45 s after the removal)
to re-run fabric discovery; each takes seconds to about 25 s, during which the other
targets are briefly offline. The exFAT volume that was being written verified clean
after the pull.

Sleep and wake with both targets attached was exercised on that baseline: sleep
flushes each published target, and wake brought both media back at their own disk
numbers in under 25 s with byte-exact readback. The current source re-ran sleep
and wake on one target only; those measurements are below.

Not covered by that baseline: SAS/SSP write paths are not verified on hardware;
multipath, multiple LUNs, enclosure services, long duration or concurrent stress.
Wire-format and large-LBA cases are checked on the host, not on this hardware.

## Capabilities

- IOC initialization and IOC-Facts-constrained queues; interrupt-driven replies
  with a bounded polling fallback; event subscription and EVENT_ACK.
- A bounded asynchronous queue with a configurable command depth, constrained
  by IOC Facts and allocated resources. Ready commands retire and free windows
  are refilled without waiting for an earlier array entry. Client completions
  use a separate serialized context.
- Direct client-buffer mappings through IODMACommand, device-visible IEEE
  scatter/gather and bounded chains. A private resident staging buffer is used
  only when direct mapping is disabled or cannot be represented.
- Batched single-ring reply retirement and sequence-based completion wakeups,
  with bounded polling when an interrupt is missing. Hardware multi-queue/MSI-X
  scaling is not implemented.
- READ/WRITE(10)/(16), READ CAPACITY(10)/(16), TEST UNIT READY, INQUIRY,
  SYNCHRONIZE CACHE(10)/(16), and FUA with an ordered cache-flush fallback.
- UNMAP after Block Limits and Logical Block Provisioning verification, split to
  the reported limits without extending the caller's discard range.
- SAS device-handle enumeration, independent-disk filtering on IR-capable
  firmware, and stable identity with capacity and block-size revalidation.
- Per-command sense and completion ownership; TMF escalation and IOC recovery
  that name the failed command's own handle; no replay of a write with uncertain
  completion.
- Per-target media offline/online lifecycle, guarded TARGET_RESET / drain /
  REMOVE_DEVICE with handle-reuse checks, and bounded full-controller recovery
  as the fallback when a removal or a definite probe failure cannot be proved.
- Every published target is writable, with no per-device allowlist or
  write-protection state; write accounting stays per target.
- Three-way probe classification (ok / not-ready / failed) with one bounded
  per-target retry budget: a device answering NOT READY, UNIT ATTENTION or BUSY
  is retried without consuming the failure budget or triggering an IOC reset.
- Registered power states: per-target flush, then DMA stop for sleep and
  shutdown; unconditional controller rebuild before the first topology read on
  wake, with a watchdog-timed settling window for config/event and PortEnable
  replies, then per-target revalidation.
- Diagnostic output is silent by default: adding `mpr_debug=1` to boot-args
  enables both the kernel log lines and all IORegistry properties.

Implementation, memory ownership, locking and diagnostic details are in
[DEBUG.md](DEBUG.md). Scope, reference baseline, milestone status and remaining
work are in [PLAN.md](PLAN.md).

## Scope

Each published target is a LUN-0 whole disk with its own medium. Discovery,
probing, removal and recovery are per target: a handle names one firmware
address, and the driver keeps the SAS address, stable identity and medium
generation that address was probed for, so a reused handle is never treated as
the same disk.

IR RAID volumes, multipath, multi-LUN devices, SES/SMP management,
NVMe/Tri-Mode, firmware flashing and FreeBSD user ioctl compatibility are
outside this driver. IOKit may still bounce through internal memory, so direct
client mapping is not a guarantee that every platform performs zero copies; the
legacy synchronous bulk/probe path keeps staging.

## Performance configuration

The defaults enable direct client DMA and request up to 32 concurrent commands.
`mpr_io_depth=N` selects 1-32 requested slots; a missing or non-positive value
uses 32, and larger values are clamped. The effective limit can be lower because
of IOC credits, reply/request capacity or allocation pressure. `mpr_direct_dma=0`
forces the staging path for comparison. These settings take effect at driver
initialization, not through a live IORegistry write.

Use `mpr_debug=1` for diagnostic runs; keep it off for production throughput
measurements and keep serial logging off. `mpr_direct_dma=0 mpr_io_depth=4`
is an in-version control, not a reproduction of the old scheduler. Counter
semantics and unproved-DMA-stop behavior are documented in [DEBUG.md](DEBUG.md).

### Measured read throughput

Raw-device reads of the one SATA target (no file-system cache), three repeats
each, median of the repeats, debug output and serial logging off. `MPRAsyncCommandLimit`
reported the requested depth in every configuration (1, 4, 16 and 32), so the
depth limit was not reduced by credits or allocation.

| Shape | QD | Preceding build | This source | Latency p50 |
|---|---|---|---|---|
| 4 KiB random | 1 | 0.96 k IOPS | 4.9 k IOPS | 1.03 ms -> 0.23 ms |
| 4 KiB random | 32 | 3.9 k IOPS | 31.6 k IOPS | 8.15 ms -> 0.83 ms |
| 16 KiB random | 1 | 0.95 k IOPS | 3.3 k IOPS | 1.06 ms -> 0.34 ms |
| 16 KiB random | 32 | 3.7 k IOPS | 12.9 k IOPS | 8.25 ms -> 2.26 ms |
| 64 KiB random | 1 | 58.7 MiB/s | 130.6 MiB/s | 1.05 ms -> 0.51 ms |
| 128 KiB sequential | 1 | 114.8 MiB/s | 222.7 MiB/s | 1.06 ms -> 0.55 ms |
| 1 MiB sequential | 1 | 224.9 MiB/s | 248.4 MiB/s | 4.28 ms -> 4.02 ms |
| 1 MiB sequential | 32 | 120.6 MiB/s | 154.0 MiB/s | 264 ms -> 207 ms |

Two limits belong to that path, not to the scheduler. Large-block throughput
saturates near 150-155 MiB/s above QD 1 in both builds, and a 64 KiB command
takes longer when more commands are outstanding, so extra depth does not raise
large sequential throughput. The in-version control at equal depth (`mpr_direct_dma=0`
against `mpr_direct_dma=1`, depth 4) is within 2% on every shape, so the gain
above is attributable to the scheduler and not to client-buffer DMA, which
removes the driver's explicit copy (`MPRCopiedBytes` stays 0) without changing
measured client throughput. A single target, one expander and one drive model
were measured; none of these numbers extend to another disk or path.

### Command-rate, write and power measurements

All of the following used the same single SATA target behind this expander,
`mpr_debug=1` for counter visibility, serial logging off, and raw-device or
test-volume I/O only.

| Run | Commands | Duration | Result |
|---|---|---|---|
| 4 KiB random reads, client-buffer DMA, depth 32 | 21,113,381 | 600 s | 35.2 k IOPS mean, no error, p50 889 -> 888 us across the run |
| same, repeated for a longer soak | 63,723,583 | 1800 s | 35.4 k IOPS mean, first interval 35.2 k, last 35.4 k, p50 884 us, no error |
| 4 KiB random reads, staging path, depth 32 | 10,660,485 | 300 s | 35.5 k IOPS, 43.7 GB staged through the bounce buffers, no error |
| mixed 4/64/256 KiB and 1 MiB reads, depth 32 | 62,256 | 120 s | 328 k chained descriptors built, no mapping failure, no error |
| 8 threads, 70% 4 KiB in-place writes + 30% verified reads, own 4 MiB slice each | 3,243,608 (2,269,872 writes, 6,331 fsyncs) | 300 s | 10.8 k ops/s, zero mismatched reads, final cache-disabled readback exact |
| 32-thread mixed 4 KiB/32 KiB/64 KiB/1 MiB reads | 32 threads | - | every thread matched its single-threaded read |

Across those runs reply retirement batched up to 30-32 descriptors per index
write (`MPRReplyMaxBatch` 1 -> 32; one reply-queue index write per batch), and
no run produced a sticky fault, an unexpected completion, a reset or a held
mapping. `MPRAsyncOutstanding` and `MPRAsyncHeldDMA` returned to zero after each
run.

Boundary and error paths were checked once on the raw device: the last 4 KiB is
readable and unchanged after the probes, one block past the end returns no
data, a request that overruns the end returns only the in-range part, a
zero-length request is accepted, unaligned offsets and sub-block lengths are
refused with `EINVAL`, a 16 MiB single request works, and 20000 random
single-block reads matched a second pass.

Write coverage on the dedicated exFAT test volume stayed inside one 32 MiB
file: the modelled write storm above plus one separate 32 MiB write followed by
2000 in-place 4 KiB rewrites, `fsync`, a cache-disabled readback that matched
byte for byte, and deletion. Media footprint is bounded; only the command count
grows. Whether a given write carried FUA is not observable from user space, so
FUA selection stays unverified.

Sleep/wake was run twice (about 20 s and about 100 s of sleep). Each wake cost
exactly one controller rebuild (`MPRResetCount` +1, `MPRWakeRebuildState=ok`,
`MPRProbeRetries=0`), the medium returned as the same BSD disk, and the
read-only checks passed byte for byte afterwards. Kernel power-management
timing recorded the driver taking about 1.16 s to quiesce for sleep and about
10.2 s to come back on wake.

### Injected recovery, by escalation level

`mpr_timeout_test=1`, `mpr_timeout_every=N` and `mpr_tmf_fail=N` make the driver
treat one command per N as having lost its reply, and optionally make the first
N task-management levels look unsuccessful so the next level is really sent.
Four variants were run, each read-only, each with a storm in flight:

| Injection | Task-management request that reached the target | IOC reset | SAS removal events | Medium afterwards |
|---|---|---|---|---|
| `mpr_tmf_fail=2`, one-shot at boot | TARGET RESET | none | +1 per injection | returned, digests matched |
| `mpr_tmf_fail=0`, every 100000, repeated | ABORT TASK | none | +1 per injection (12 for 12) | re-enumerated as a new BSD disk after 12 events |
| `mpr_tmf_fail=1`, every 1000000 | LUN RESET | none | +1 for the one injection | re-enumerated as a new BSD disk, unavailable for over 45 s |
| `mpr_tmf_fail=3`, every 1000000 | none (all three levels skipped) | +1, `MPRResetInitiator=io-timeout` | **0** | re-enumerated as a new BSD disk, transfers and digests fine |

So the fabric removal follows a task-management request that actually reaches
the target, not the escalation level and not the driver's reset. An IOC-level
rebuild alone leaves the fabric alone (`MPRTopoRemovalHits` unchanged) and the
target comes back usable; a TMF that reaches the drive costs the medium for
tens of seconds while the expander re-discovers it. In every variant the
in-flight requests failed with an error instead of hanging, `MPRStickyFaultCount`
and `MPRFaultRecoveries` stayed zero, `MPRAsyncHeldDMA` returned to zero, and
every read-only digest still matched after recovery.

Two observations are unexplained and are recorded as such. In the 12-event run
the medium came back with raw requests above 128 KiB failing with `ENXIO` while
smaller ones worked, until a reboot; the single-event runs did not reproduce it.
That run also left the attached volume's exFAT allocation bitmap inconsistent
because the volume was mounted when the target disappeared; re-mounting and
cleanly unmounting restored consistency and a targeted `fsck_exfat` then had
nothing to repair, with a 1 MiB write, readback and delete verifying allocation.

`MPRRecoveryState=media-revalidation` is written when a target is invalidated
and, on the reset-based path, is never rewritten afterwards — it stayed
`media-revalidation` while the medium was serving full-size requests again. It
is therefore not evidence of a stuck medium; use `MPRTarget0Online`,
`MPRMediaReady` and actual I/O instead.

## Build and local checks

On a host with Python 3 and a C++17 compiler:

```sh
python3 tools/validate-build-inputs.py
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined tools/test_protocol.cpp -o /tmp/mpr-protocol
/tmp/mpr-protocol
```

On macOS with the Command Line Tools and MacKernelSDK:

```sh
MACKERNELSDK=/path/to/MacKernelSDK sh tools/build-noxcode.sh
```

The build produces `build/MPR.kext`, validates the kmod entry points, bundle
type, driver-internal symbols and plist, then signs it ad hoc. The Xcode project
and CLI build include all driver translation units. A cross-compiled Mach-O
object is not a linked, signed or load-tested kext.

## Install

Install only on the verified internal SATA disk's ESP, under
`EFI/OC/Kexts/MPR.kext`, with its OpenCore `Kernel > Add` entry.
Never install this driver in `/Library/Extensions` or on USB install/rescue
media.

Before any write, identify the internal disk and EFI partition by their current
model, capacity, location and UUID. Back up `config.plist` beside itself only if
that configuration must change, and validate any changed plist. After a
deliberate deployment and reboot, confirm the loaded version with
`kmutil showloaded`; reading `MPRBuild` additionally requires `mpr_debug=1`.
The build and local checks above neither
deploy nor reboot; deployment is a separate, explicitly authorised step.
