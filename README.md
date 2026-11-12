# MPR — Fusion-MPT SAS3 (SAS3008) kext for macOS

Date: 2026-11-12. Driver version: 0.17.18.

A from-scratch IOKit block-storage driver for the LSI/Broadcom SAS3008
(`PCI 0x1000:0x0097`, MPI 2.5) on x86_64 macOS. It drives one directly
attached SATA disk through the IOC's request/reply queues and publishes it
through `IOBlockStorageDriver` as a normal whole-disk `IOMedia` with its
partition scheme.

Implementation details, IORegistry keys and the hotplug state machine are in
[DEBUG.md](DEBUG.md). Milestones, scope and the requirement backlog are in
[PLAN.md](PLAN.md).

## What works

- Controller bring-up: MMIO BAR selection, READY, IOC Facts, IOC Init,
  manufacturing page 0, MSI/MSI-x interrupt, request/reply frame pools.
- Discovery: SAS IO Unit Page 0 and SAS Device Page 0, target filtering
  (end device, SATA/STP/SSP, direct attach), fast-path capability selection.
- SCSI: TUR, INQUIRY, READ CAPACITY(10)/(16), READ/WRITE(10), SYNCHRONIZE
  CACHE, UNMAP (probed before use), sense decoding, FUA.
- Media: one published whole-disk `IOMedia`, partition rescan after a rebuild,
  `kIOMessageMediaStateHasChanged` offline/online notifications with their
  actual `IOBlockStorageDriver` return status.
- Hotplug: physical unplug and replug of the test disk, idle or with block I/O
  in flight, including replug inside a few seconds and handle reuse by the IOC.
  The old medium is always taken offline before the new one is published; the
  disk is rediscovered, re-probed (identity, capacity, TUR, block read) and
  re-published with its partitions. In-flight I/O fails bounded instead of
  hanging.
- Recovery is bounded: at most three diagnostic resets per insertion, then
  topology polling only. An empty fabric is polled without resetting.

Verified on the target machine (macOS 26.7.2, Darwin 25.6.0) with the
240 GB PCCOOLER SSD T20 (`468862128 × 512 B`) on PHY 5, over eleven physical
unplug/replug cycles: four idle (one held out for a minute), four fast
(seconds, handle reused), three with 256 MiB reads in flight. Every cycle
ended with a single active whole-disk `IOMedia`, its partition node, and a
64 MiB raw read whose SHA-256 matched the pre-cycle baseline; no panic, no
deadlock, no duplicate active medium, no unbounded reset loop.

## Explicitly not implemented

This is a single-target, directly attached prototype:

- No multi-target isolation, no per-target TARGET_RESET/REMOVE_DEVICE
  protocol, no SAS expander or RAID volume support.
- One synchronous staging buffer serialized by `fIOLock`; no asynchronous
  command pool or chaining, so throughput is bounded by one command at a time.
- No 24 h soak, no full-disk or concurrent stress, no PM sleep/wake
  qualification.

Read-only integrity is verified; write paths (WRITE, SYNCHRONIZE CACHE, UNMAP)
are exercised only in earlier small-scale tests and are not part of the
hotplug acceptance above.

## Build

On macOS with the Command Line Tools only (no Xcode), against MacKernelSDK:

```sh
MACKERNELSDK=/path/to/MacKernelSDK sh tools/build-noxcode.sh
```

Output is `build/MPR.kext`, ad-hoc signed. The script also checks
`_kmod_info`/`_start`/`_stop`, `MH_KEXT_BUNDLE`, that no driver-internal
symbol is left undefined, `plutil -lint`, and the signature.

## Install

The kext is installed on the internal SATA disk's ESP only, never in
`/Library/Extensions` and never on a system volume:

1. `EFI/OC/Kexts/MPR.kext` on the internal disk's EFI partition.
2. A `Kernel > Add` entry for `com.ling.MPR` in `EFI/OC/config.plist`.

Confirm the disk identity (model, capacity, `Device Location: Internal`)
before writing, back up `config.plist` next to itself before editing, and check
the running version after reboot with `kmutil showloaded` and the driver's
`MPRBuild` property. A file on disk is not evidence that the kernel loaded it.
