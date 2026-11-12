# MPR — Fusion-MPT SAS3 (SAS3008) kext for macOS

Date: 2026-11-13. Driver version: 0.19.0.

MPR is an IOKit block-storage driver for the LSI/Broadcom SAS3008
(`PCI 0x1000:0x0097`, MPI 2.5) on x86_64 macOS. It publishes one independent
disk through `IOBlockStorageDriver` as a whole-disk `IOMedia`.

## Status

Built, linked, signed ad hoc and loaded from the internal ESP on the test machine
(macOS 26.7.2); the running version is confirmed with `kmutil showloaded` and
`MPRBuild`.

The driver is qualified in two configurations: the test SATA disk attached
directly to the SAS3008, and the same disk as the only terminal disk behind an
Adaptec AEC-82885T expander and one backplane. Both publish one whole-disk
`IOMedia` and pass bounded read, write, flush and UNMAP with byte-exact
readback, including concurrent reads. Direct attach additionally passed unplug
and replug, including unplug with I/O in flight; reply-timeout injection through
ABORT_TASK, LUN RESET, TARGET RESET and controller reset; sleep/wake; and
shutdown.

Multi-target discovery records every eligible device in a bounded target table
and publishes per-device handles, device info and SAS addresses in the
IORegistry. A topology with more than one disk is not published yet, so no
arbitrary disk is chosen. Access is fail-closed: a nub is read-only unless an
explicit `MPRWritableDevices` rule in the kext personality matches the disk's
model and serial; an unidentified disk therefore cannot receive a write.

Not covered: more than one target, SAS/SSP drives, multipath, multiple LUNs,
enclosure services, physical unplug/replug and power management behind the
expander, long duration or concurrent stress, and throughput tuning.
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
- Per-command sense and completion ownership; bounded TMF escalation and
  controller recovery; no replay of a write with uncertain completion.
- Media offline/online lifecycle, guarded TARGET_RESET / REMOVE_DEVICE, and
  bounded full-controller recovery.
- Registered power states: quiesce, flush and DMA stop for sleep and shutdown;
  rebuild and revalidate on wake.

Implementation, memory ownership, locking and diagnostic details are in
[DEBUG.md](DEBUG.md). Scope, reference baseline, milestone status and remaining
work are in [PLAN.md](PLAN.md).

## Scope

The storage path is single-target and LUN 0. If more than one eligible
independent target is discovered, publication is refused rather than choosing an
arbitrary disk.

IR RAID volumes, multi-target isolation, multipath, multi-LUN devices, SES/SMP
management, NVMe/Tri-Mode, firmware flashing and FreeBSD user ioctl
compatibility are outside this driver. Client buffers are copied through
bounded resident DMA windows; direct zero-copy client DMA and multiple hardware
reply queues are not implemented.

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
`kmutil showloaded` and `MPRBuild`. This patch does not deploy or reboot.
