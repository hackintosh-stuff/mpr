# FreeBSD port coverage and validation

Date: 2026-11-13. Driver version: 0.18.1.

## Comparison boundary

The comparison uses the fixed FreeBSD source already in
`reference/freebsd/sys/dev/mpr`; source provenance is in PLAN section 2.
It covers the hardware/controller mechanisms and the disk-level equivalents
needed by this IOKit driver. FreeBSD's CAM/newbus/bus_dma/sysctl interfaces
are platform glue; their exact API names are not macOS requirements.

This is a single published independent target on SAS3008, LUN 0.
The test configuration is one backplane and one SATA disk. A backplane does
not by itself establish whether the attachment contains an expander.

## Coverage matrix

| Mechanism | FreeBSD reference | Current implementation |
|---|---|---|
| Request ownership/completion | `mpr_command`, `mpr_complete_command`, `mpr_intr_locked` | Explicit per-SMID ownership, checked replies, independent sense, quarantined timeouts and cancelled reset completions |
| Asynchronous dispatch | CAM submission and command callbacks | IOKit request queue, four cross-request command windows, separate completions and sequence fences |
| DMA scatter/gather and chain | `mpr_add_chain`, `mpr_push_ieee_sge`, IOC Facts | Device-visible resident mappings, bounded IEEE chains, explicit contiguous fallback |
| Queue resources | `mpr_resize_queues`, high-priority command pool | Facts constrained queue geometry and reserved management/TMF/ACK resources |
| SCSI data contract | `mprsas_action_scsiio` plus CAM disk policy | READ/WRITE10/16, exact block transfers, bounded sense, SYNC10/16, FUA fallback |
| Thin provisioning | CAM's disk/UNMAP policy | B0/B2 capability probe, full input validation, bounded descriptor/block batching |
| Discovery | SAS config pages and discovery events | GET_NEXT_HANDLE enumeration; one terminal independent disk may be behind a backplane/expander |
| IR membership protection | IR configuration handling and RAID_COMPONENT filtering | Enumerate RAID Configuration Page0; refuse volumes/members/spares/expansion disks and unknown configuration |
| Persistent disk identity | SATA IDENTIFY / SCSI identifiers / mapping | ATA model+serial or validated VPD identifier, capacity/block-size comparison and media generations |
| Target removal | `mprsas_prepare_remove`, `mprsas_remove_device` | Guarded TARGET_RESET/drain/REMOVE_DEVICE with handle-reuse checks and bounded IOC fallback |
| Recovery | abort/LUN/target/IOC reset and watchdog | Credit-aware TMF escalation, explicit ownership proof, no replay of uncertain writes, IOC state monitoring |
| Sleep and shutdown | FreeBSD suspend/resume return EINVAL; shutdown path separate | Native IOKit PM/shutdown quiesce and wake revalidation; requires runtime qualification |
| Builds | OS-specific build system | All four translation units included by CLI and Xcode; input/version/line-ending validation |

The changes also fix protocol and integration defects in the existing port:
extended configuration-header byte offsets, a shared sense DMA area,
non-fast-path SCSI request descriptor type, partial-batch draining,
incorrect success/byte accounting on failed submissions/copies, and the
Xcode omission of the block-device translation unit.

## Capabilities still outside this configuration

These are not silently marked as implemented:

| FreeBSD capability | Current boundary |
|---|---|
| Multiple target objects, target-local scheduling and fault isolation | One published target; more than one independent candidate is refused |
| Multiple LUNs and general CAM SCSI passthrough | LUN 0 block disk interface |
| DPM persistent OS target numbering and multipath | Stable disk identity is checked; no FreeBSD target-number or multipath contract |
| IR RAID logical volumes and RAID management | Protection/filtering only; no volume publication or management |
| SES/enclosure controls, SMP passthrough, firmware update and user ioctl tools | Not implemented |
| NVMe/PCIe/Tri-Mode, SAS2 and GEN35 hardware families | SAS3008 MPI 2.5 only |
| Direct DMA into client buffers, multi-queue/MSI-X scaling and advanced performance tuning | Resident copies, four command windows and one reply consumer |

No additional disk, backplane, SAS drive, RAID array or large-capacity drive
is made a requirement of this patch's acceptance.

## Validation that does not require more hardware

`tools/test_protocol.cpp` tests real shared wire-format helpers against
independent golden bytes and a separate chain decoder. It covers 10/16-byte
CDB boundaries, high LBAs, arithmetic overflow, legal/truncated VPD, RAID page
layouts/membership, bounded fixed/descriptor sense, chain depth/address/
length rejection, sentinel protection and randomized SGL reconstruction.
AddressSanitizer and UndefinedBehaviorSanitizer are used when available.

`tools/validate-build-inputs.py` checks LF text, bundle and runtime version
consistency, and inclusion of every driver translation unit in Xcode.
The CLI compiles every MPR source file.

Compilation against MacKernelSDK with a Darwin target can verify C++/IOKit
declarations and Mach-O objects. Full kext linking, ad hoc signing and
loading require macOS and are distinct checks.

## Acceptance on the existing backplane and SATA disk

Only run these after a deliberate build/deployment on the existing machine.
Re-identify the test disk by model, serial, capacity and current device node;
do not reuse a historic disk number.

1. Confirm one active whole-disk IOMedia, partition discovery, stable identity,
   loaded version and bounded controller resources.
2. Stream the same bounded regions twice, verifying both complete byte
   counts and hashes. Run several simultaneous reads of the same disk to
   exercise the shared command pool. No data backup is required for read hashes.
3. Where the existing test-disk write authorization applies, use a small
   dedicated region/file for WRITE -> flush -> readback. Confirm FUA policy.
   UNMAP uses only an explicitly disposable range and only if capabilities
   permit it; do not require read-zero when the disk does not promise that.
4. Use the existing disk/backplane for idle, in-flight-read and quick
   unplug/replug checks. Require old-media retirement, one new active medium,
   a completed read of the expected bytes and no unbounded reset loop.
5. Exercise already available timeout/TMF injection with this same disk.
   Check every accepted request's completion, no uncertain write replay,
   no reusable DMA window still owned by the IOC and no cross-generation
   cache/UNMAP operation.
6. When a normal recovery channel is available, check sleep/wake and shutdown
   using this same machine, then repeat the bounded reads.

High-LBA, alternate packet/error/chain shapes and malformed configuration
pages are off-device protocol cases for this hardware configuration.
They are not a request for another drive or backplane, nor proof of runtime
compatibility with those untested devices.

## Current evidence

| Check | Result |
|---|---|
| Host protocol helpers with AddressSanitizer and UndefinedBehaviorSanitizer | Passed: 136,593 assertions and 2,000 deterministic SGL cases; generated cases include intentional rejection cases |
| Build inputs and whitespace | Passed: all four translation units included, version 0.18.0 consistent, LF text and clean diff whitespace |
| Darwin-target object compilation | Passed: clang 19.1.5, project MacKernelSDK, `x86_64-apple-macos11.0`; four Mach-O x86_64 objects, no compiler diagnostics |
| Driver-internal cross-translation-unit symbols | No unresolved driver-internal symbols in the four objects; kernel linking is a separate check |
| Compiled snapshot versus delivered source | All 12 driver source/header/plist files matched by SHA-256 |
| Native macOS kext link, sign and load | Not run: the known test-machine SSH endpoints were unreachable |
| DMA/IRQ, disk I/O, hotplug, timeout recovery, sleep/wake and shutdown | Not run on hardware for this source version |

This version has not been installed, booted, stress-tested, physically
hotplug-tested or power-cycle-tested on the target. Prior version results
are historical evidence only and do not qualify the changed DMA, async,
identity, topology or PM paths. No deployment, restart or disk write was
performed during this work.
