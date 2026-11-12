# MPR — Fusion-MPT SAS3 (SAS3008) kext for macOS

Date: 2026-11-13. Driver version: 0.18.1.

MPR is an IOKit block-storage driver for the LSI/Broadcom SAS3008
(`PCI 0x1000:0x0097`, MPI 2.5) on x86_64 macOS. It publishes one independent
disk through `IOBlockStorageDriver` as a whole-disk `IOMedia`.

This source version implements the disk path and controller mechanisms listed
below. Host-side protocol checks and Darwin-target object compilation have
passed; it has not been loaded or qualified on the test machine. The available
acceptance hardware is one backplane and one SATA disk. Source support for discovering
a disk behind an expander is not evidence of expander hardware qualification.

## Implemented capabilities

- Controller initialization, IOC Facts constrained queues, interrupt-driven
  replies, bounded polling fallback, event subscription and EVENT_ACK.
- An asynchronous request queue with four concurrent command windows shared
  by different requests, separate client completion delivery, and ordered
  cache/UNMAP fences.
- Device-visible scatter/gather mappings and bounded MPI 2.5 IEEE chains.
  Resident staging buffers retain a contiguous fallback when mapping or
  chain geometry cannot represent a transfer.
- READ/WRITE(10) and (16), READ CAPACITY(10)/(16), TEST UNIT READY,
  standard/extended INQUIRY, SYNCHRONIZE CACHE(10)/(16), and FUA with an
  ordered cache-flush fallback when native FUA is not advertised.
- UNMAP only after Block Limits and Logical Block Provisioning capabilities
  are verified. Large requests split to the reported limits without extending
  a caller's discard range.
- SAS device-handle enumeration, independent-disk filtering on IR-capable
  firmware, and stable disk identity with capacity/block-size revalidation.
- Per-command sense storage and completion ownership checks; bounded TMF
  escalation and IOC recovery; no automatic replay of an uncertain write.
- Offline/online media lifecycle, conservative TARGET_RESET/REMOVE_DEVICE,
  handle-reuse checks, and bounded full-controller recovery.
- Registered power-state transitions and shutdown quiesce: stop accepting
  requests, retire accepted work, flush, and stop DMA before power loss;
  rebuild and revalidate on wake.

Implementation, memory ownership and diagnostic details are in
[DEBUG.md](DEBUG.md). The FreeBSD comparison and validation boundaries are in
[PORTING.md](PORTING.md). Project scope and historical test evidence are in
[PLAN.md](PLAN.md).

## Scope

The published storage path remains single-target and LUN 0. If more than one
eligible independent target is discovered, publication is refused rather
than choosing an arbitrary disk.

IR RAID volumes, multi-target isolation, multipath, multi-LUN devices,
SES/SMP management, NVMe/Tri-Mode, firmware flashing and FreeBSD user ioctl
compatibility are outside this driver configuration. Client buffers are
copied through bounded resident DMA windows; direct zero-copy client DMA
and multiple hardware reply queues are not implemented.

These limits are not additional hardware requirements for acceptance. Tests
for this project use only the existing backplane and SATA disk. Wire-format
boundaries that the disk cannot exercise, such as LBAs above 2 TiB, are checked
off-device and are not reported as hardware results.

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

The build produces `build/MPR.kext`, validates the kmod entry points,
bundle type, driver-internal symbols and plist, then signs it ad hoc. The
Xcode project and CLI build include all driver translation units.
A cross-compiled Mach-O object is not a linked, signed or load-tested kext.

## Install

Install only on the verified internal SATA disk's ESP, under
`EFI/OC/Kexts/MPR.kext`, with its OpenCore `Kernel > Add` entry.
Never install this driver in `/Library/Extensions` or on USB install/rescue
media.

Before any write, identify the internal disk and EFI partition by their
current model, capacity, location and UUID. Back up `config.plist` beside
itself only if that configuration must change; validate any changed plist.
After a deliberate deployment and reboot, confirm the loaded version with
`kmutil showloaded` and `MPRBuild`. This patch does not deploy or reboot.
