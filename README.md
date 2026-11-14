# MPR — Fusion-MPT SAS3 / Tri-Mode kext for macOS

Date: 2026-11-14. Driver version: 0.24.0.

MPR is an IOKit block-storage driver for LSI/Broadcom MPI 2.5/2.6 Fusion-MPT
controllers on x86_64 macOS. Eligible SAS/SATA disks, firmware IR RAID volumes
and discovered direct-access logical units on PCIe NVMe targets each publish
their own whole-disk `IOMedia` through
`IOBlockStorageDriver`.

## Supported hardware

The exact PCI table follows the pinned `reference/freebsd/sys/dev/mpr/mpr_pci.c`:
29 recognized IDs, of which four SEA non-secure-mode IDs are explicitly refused
before PCI enable/MMIO, leaving 25 attachable IDs. This is implementation
coverage, not hardware qualification for every card or firmware.

| Family | Device IDs (vendor `0x1000`) | Evidence |
|---|---|---|
| SAS3004 / SAS3008 | `0x0096`, `0x0097` | SAS3008 has earlier-build hardware evidence; this version is not load-tested |
| SAS3108 Fusion-MPT variants | `0x0090`, `0x0091`, `0x0094`, `0x0095` | Source implemented; hardware unverified |
| SAS3216 / SAS3224 / SAS3316 / SAS3324 | `0x00c9`, `0x00c4`, `0x00c5`, `0x00c6`, `0x00c0`, `0x00c1` | Source implemented; hardware unverified |
| GEN35: SAS3408/3416/3508/3516/3616/3708/3716 | `0x00af`, `0x00ac`, `0x00ad`, `0x00ae`, `0x00aa`, `0x00ab`, `0x00d1`, `0x00d2`, `0x00d0` | Source implemented; hardware unverified |
| SEA: SAS3816 / SAS3916, configured/hard secure | `0x00e5`, `0x00e6`, `0x00e1`, `0x00e2` | Source implemented; hardware unverified |

Rejected SEA non-secure IDs: `0x00e4`, `0x00e7`, `0x00e0`, `0x00e3`. Matching is by exact
vendor/device pair, not by a wildcard or a marketing model name. SAS2 (`mps`)
and unrelated MegaRAID (`mrsas`) device IDs remain outside the table.
`MPRChipName`, negotiated MPI version and generation/capability diagnostics are
available with `mpr_debug=1`.

## Status

The source implements multi-LUN/namespace routing, protected-disk I/O, bounded
controller-local path selection and a native administrator control interface in
addition to the SAS/IR/NVMe data paths. Current evidence is source review and
cross-compilation of the six driver translation units against MacKernelSDK.
There is no current native link/sign/load result or hardware qualification.
Host-only ASan/UBSan protocol checks, offline CLI packet checks and build-input
validation have been executed. No hardware tests are scheduled or performed.

All runtime measurements below belong to the previously documented SAS3008
build 0.21.0, not 0.24.0 or any newly matched hardware. The running machine was
not queried or changed during this source-only work.

Qualified on 0.21.0: read-only block I/O including requests larger than
one command window, chunks at non-zero offsets, several concurrent client
requests, fragmented and page-unaligned client buffers, and byte-exact
comparison against digests recorded under the previous build.

Also qualified on 0.21.0: a sustained command storm (21.1 million
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

Not yet qualified on 0.21.0: UNMAP, physical removal and reinsertion,
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
numbers in under 25 s with byte-exact readback. The 0.21.0 build re-ran sleep
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
- Typed SAS/SATA, IR volume and PCIe NVMe enumeration, with RAID member/spare
  hiding, volume-state validation and type-plus-WWID identity. Up to 64 targets.
- Memory-BAR discovery, MPI 2.5/2.6 negotiation, GEN35/SEA identification, SEA zero-register-read
  retry and capability-gated atomic request posting.
- NVMe native PRP lists on direct/staging paths, MDTS-bounded asynchronous
  transfers, PCIe protocol-level resets and native DSM for advertised UNMAP.
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
- REPORT LUNS, per-LUN command/flush/TMF ownership and namespace-aware NVMe DSM.
- Type-1/type-3 EEDP, TLR for supported sequential-access targets, native
  management-only endpoint classification and identity-checked path selection.
- Administrator control, diagnostic buffer ownership, event history and
  maintenance-isolated firmware/RAID operations; IR shutdown and SATA SSU.
- Diagnostic output is silent by default: adding `mpr_debug=1` to boot-args
  enables both the kernel log lines and all IORegistry properties.

Implementation, memory ownership, locking and diagnostic details are in
[DEBUG.md](DEBUG.md). Scope, reference baseline, milestone status and remaining
work are in [PLAN.md](PLAN.md).

## Scope

Each published direct-access LUN has its own geometry, stable identity and
media generation. REPORT LUNS supports peripheral, flat and extended-flat SAM
addresses; NVMe DSM maps LUN to NSID = LUN + 1. The bounded table holds 64 logical
targets, including management-only anchors. Tape/enclosure/other connected SCSI
endpoints remain available for administrator passthrough, not as block disks.

Up to eight firmware handles for the same `(kind, WWID, LUN)` share one logical
target within a controller. A complete discovery snapshot retains a present
active path or selects a surviving path, invalidates its old media generation
and re-probes before publication. This is discovery-driven failover, not
cross-controller MPIO/ALUA, round-robin load balancing or uncertain-write replay.
Different SAS-port addresses are not automatically equated without a common
firmware identity. Native WWID/LUN locations do not guarantee a fixed BSD disk
number across boots or write FreeBSD CAM target-ID mappings into firmware DPM.

EEDP covers supported type-1/type-3 protected READ/WRITE commands; unsupported
protection formats are refused rather than exposed through an unprotected
READ CAPACITY fallback. Supported sequential-access endpoints can use TLR via
the control interface. Native macOS tape/optical class drivers, a SCSI target
mode, FreeBSD binary ioctl compatibility, Apple Silicon and hardware multi-queue
are not implemented. SCSI bidirectional/raw persistent-DMA commands and arbitrary
queue-register writes are deliberately not exposed by the native control ABI.

## Administrator control

`tools/mprctl.py` uses the native `IOUserClient`, not a FreeBSD ioctl emulation.
Management is disabled by default. `mpr_mgmt=1` enables an administrator-
only connection for information, targets, events, configuration reads, read-only
SCSI/SATA/NVMe/SMP and enclosure requests, and controlled diagnostics/reset.
`mpr_debug=1` is independent: it enables logs and diagnostic IORegistry properties.

`mpr_maintenance=1` also enables management but suppresses all block-media
publication. Raw operations that can change media, arrays or firmware require
that maintenance boot AND the CLI's explicit `--allow-mutation` option. The CLI
requires an explicit `--registry-id` for acknowledged state-changing requests;
target-specific requests additionally carry the current WWID and generation.
The driver generates every DMA address and refuses stale transport epochs.
No utility changes boot arguments, deploys a kext, mounts a volume or reboots.

The native interface includes configuration pages, RAID actions/member
passthrough, firmware upload/segmented download, SCSI/SATA/NVMe/SMP passthrough,
SEP enclosure control, diagnostic CLI, register reads, scratchpad writes,
controller reset/rescan, event masks/history and diagnostic buffer management.
These are protocol primitives, not a vendor-specific array-management GUI.

```sh
python3 tools/mprctl.py list
sudo python3 tools/mprctl.py info --registry-id <registry-id>
sudo python3 tools/mprctl.py targets --registry-id <registry-id>
sudo python3 tools/mprctl.py events --registry-id <registry-id> --after <sequence>
sudo python3 tools/mprctl.py request request.json --registry-id <registry-id>
```

A request JSON uses an `operation` (default `passthrough`), a named MPI `function`
or `requestHex`, and optional `dataOutFile`/`dataOutHex`, `readBytes`, `timeoutMs`,
`wwid`, `generation` and function-specific fields. Epoch is queried when omitted;
it is never silently refreshed and retried after an operation fails. Paths in a
JSON specification are relative to that JSON file. `--encode-only packet.bin`
serializes without opening IOKit; `--data-output file` saves the returned payload.
Detailed fields, bounds and source references are in [DEBUG.md](DEBUG.md).

Shutdown flushes published logical units, notifies IR firmware of shutdown, and
optionally sends SATA START STOP UNIT. `mpr_ssu` follows the reference policy:
0 disables SSU, 1 selects identified SSDs (default), 2 selects rotational disks,
3 selects all eligible SATA disks. Sleep does not send the shutdown notification.

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

| Shape | QD | Preceding build | 0.21.0 baseline | Latency p50 |
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
