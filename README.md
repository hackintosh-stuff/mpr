# MPR — Fusion-MPT SAS3 (SAS3008) kext for macOS

Date: 2026-11-13. Driver version: 0.20.12.

MPR is an IOKit block-storage driver for the LSI/Broadcom SAS3008
(`PCI 0x1000:0x0097`, MPI 2.5) on x86_64 macOS. It publishes every eligible
independent disk through `IOBlockStorageDriver` as its own whole-disk
`IOMedia`.

## Status

Built, linked, signed ad hoc and loaded from the internal ESP on the test machine
(macOS 26.7.2); the running version is confirmed with `kmutil showloaded` and
`MPRBuild`.

The driver is qualified in two configurations: the test SATA disk attached
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
read in flight on one target and with a write in flight on the other. The dropped target is retired with TARGET_RESET, a drain of its aborted
commands and SAS IO Unit Control REMOVE_DEVICE, all naming that handle; its
medium goes offline, the other target keeps serving I/O, and the request in
flight fails with a device error instead of completing silently. A returned disk
is re-probed and republished, matched by SAS address, so the same disk number
comes back rather than a second medium for the same drive. Reinsertion behind
this expander is not announced, so the driver spends at most two bounded IOC
resets (8 s and 45 s after the removal) to re-run fabric discovery; each takes
seconds to about 25 s, during which the other targets are briefly offline. The
exFAT volume that was being written verified clean after the pull.

Sleep and wake with both targets attached is exercised. Sleep flushes each
published target; wake brings both media back at their own disk numbers in under
25 s with byte-exact readback on both. A disk that is
still becoming ready is retried in place instead of being counted as a failed
probe. The wake rebuilds the controller before the first topology read and
gives config/event and PortEnable replies a bounded settling budget, so fabric
settling produces no protocol fault and no extra IOC reset. One planned
controller rebuild per wake remains mandatory after S3; the power-state and
fault counters behind these claims are listed in [DEBUG.md](DEBUG.md).

Not covered: SAS/SSP write paths are not verified on hardware; multipath,
multiple LUNs, enclosure services, long duration or concurrent stress, and
throughput tuning are out of scope.
The long-run result recorded in [PLAN.md](PLAN.md) belongs to an earlier
serial-command prototype, not to the current asynchronous scheduler.
Wire-format and large-LBA cases are checked on the host, not on this hardware.

## Capabilities

- IOC initialization and IOC-Facts-constrained queues; interrupt-driven replies
  with a bounded polling fallback; event subscription and EVENT_ACK.
- Asynchronous request scheduling over four command windows shared by
  concurrent requests, with client completions delivered from a separate
  serialized context.
- Device-visible scatter/gather and bounded IEEE chains, with resident
  contiguous staging when a mapping cannot be represented.
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
outside this driver. Client buffers are copied through bounded resident DMA
windows; direct zero-copy client DMA and multiple hardware reply queues are not
implemented.

These limits are not additional hardware requirements.

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
`kmutil showloaded` and `MPRBuild`. The build and local checks above neither
deploy nor reboot; deployment is a separate, explicitly authorised step.
