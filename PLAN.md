# PLAN — Fusion-MPT SAS3 (SAS3008) driver for macOS

Status: source 0.21.0 (2026-11-13).

Current state: client-buffer DMA, a credit-bounded command pool, harvest-and-refill
on completion, and batched single-ring reply retirement with event wakeups are
implemented, compiled natively with the Command Line Tools, linked, signed ad hoc
and loaded from the internal ESP. On one target this source is qualified for
read-only correctness, boundary and error paths, command storms (63.7 M commands
in one 30-minute run), mixed read/write stress, a small write and flush cycle,
two sleep/wake cycles, and four recovery variants (ABORT, LUN RESET, TARGET RESET
and whole-IOC reset). The variants exposed one behaviour that is recorded but
deliberately left unchanged: a task-management request that actually reaches the
target costs a SAS removal event and tens of seconds of unavailability, while a
whole-IOC rebuild does not. UNMAP, physical replug, dual targets and shutdown have
not been re-run on this source; the earlier dual-target read/write, removal,
recovery and PM results are regression baseline only.

Build, host checks and installation are in [README.md](README.md). Implementation,
capacity and memory accounting, locking, ABI formats, IORegistry properties and
diagnostics are in [DEBUG.md](DEBUG.md).

## 1. Goal and scope

### 1.1 Target hardware and platform

- HBA: Broadcom/Avago SAS3008, PCI `0x1000:0x0097`, MPI 2.5, 12 Gb/s SAS3,
  firmware `09.00.00.00`; `IOCCapabilities` reports `INTEGRATED_RAID`.
- Current configuration: one SATA SSD, PCCOOLER SSD T200 240 GB
  (468,862,128 × 512 B), as the terminal disk behind an Adaptec AEC-82885T
  expander and one backplane. The exFAT volume `testdisk` on it carries the
  bounded write tests.
- Baseline-only configurations: the preceding build was also qualified with the
  same disk attached directly to the HBA, and with a second SSP disk
  (TOSHIBA MG04SCA60EE 6 TB) behind the same backplane. That disk has been
  removed; nothing in the current configuration exercises two targets or direct
  attach.
- Platform: macOS Tahoe 26.7.2 (`25G332`, `Darwin 25.6.0`), x86_64, MSI X570 test
  machine; IOKit kext built against MacKernelSDK.
- Hardware facts: BAR0 is I/O space and MMIO lives in BAR1/BAR2 (config offset
  `0x14`); addresses are re-assigned at boot and are never hard-coded.

Device identity for any hardware step is verified on the spot (model, serial,
capacity, current device node), never a remembered `diskN`.

### 1.2 Scope

1. A purpose-built `IOService` (provider = `IOPCIDevice`) that implements the HBA:
   PCI enable, BAR mapping, IOC reset and initialization, queues, interrupts,
   event subscription and acknowledgement.
2. Every eligible independent disk (STP/SATA or SSP) published through the system
   `IOBlockStorageDriver` as its own whole-disk `IOMedia`, with per-target
   identity, geometry and medium lifetime; every published target is writable and
   writes are counted per target.
3. Block I/O: READ/WRITE(10)/(16), SYNCHRONIZE CACHE, FUA, auto sense, timeout and
   TMF recovery, hotplug; UNMAP only when the device advertises it and the
   reported limits pass validation.
4. Power and shutdown: sleep/wake and the quiet path of `systemWillShutdown`.
5. Diagnostics: IORegistry properties plus opt-in fault-injection boot arguments.

### 1.3 Out of scope

- IR/RAID logical volumes and their member management, SES enclosure services,
  SMP passthrough, firmware update, booting from this driver's disks.
- Multiple LUNs, multipath, and hardware verification of writes to SAS/SSP disks.
- NVMe/PCIe/Tri-Mode, SAS2 (`mps`/`0x0072`), the GEN35/SEA branches.
- Apple Silicon, dext/DriverKit, multi-queue/MSI-X expansion, a FreeBSD user
  ioctl compatibility layer.
- Subclassing `IOSCSIParallelInterfaceController` (route B in §3); kept only as a
  later evaluation.

## 2. Reproducible baseline

| Item | Content |
|---|---|
| FreeBSD reference | `freebsd/freebsd-src` `main` at `e56090afb68b6c036d6b54c7b66d762d98bd96d0` (2026-11-10): `sys/dev/mpr/` and `mpi/`, 13 files each; BSD-2-Clause |
| Linux reference | `torvalds/linux` `master` at `b3dfbde62cf1981c4638939c159dfd087b7a72e9` (2026-11-10): `drivers/scsi/mpt3sas/` 16 files plus `mpi/` 10 files; GPL-2.0, behaviour reference only |
| MacKernelSDK | submodule at `3f750085caa17ec3a7880f11c11bf4f48cd6a164` (2026-09-23): macOS 11 / Xcode 12.0 SDK headers with `xnu-8019.80.24` additions |
| Apple OSS | `apple-oss-distributions/IOSCSIParallelFamily` at `560f74ab0c6f5d9aa6c99675d8e95986b202144d`, APSL 2.0; read-only reference, not part of the build |
| Decompilation material | workspace-level `ida/BootKernelExtensions.kc` (67,600,384 B, MD5 `b9da5eafcd90e439ae7c55c1ca8ddf33`) with its IDA database and offline check scripts; used only to confirm ABI, vtables and dependencies |
| Version control | `MacKernelSDK` is a submodule; `reference/` and `ida/` are not committed and do not take part in the build; reference sources are reproduced at the commits pinned above |

Directory layout:

```
mpr/
  README.md  DEBUG.md  PLAN.md  AGENTS.md  NOTICE
  MPR/                               (MPRController, MPRBlockDev, MPRAsync, MPRTarget,
                                      MPRCompletion.h, MPRDmaSgl.h, MPRPerformance.h,
                                      MPRScsi.h, MPI/mpi2_min.h, MPI/mpi2_target.h,
                                      Info.plist)
  MPR.xcodeproj/                     (project entry point)
  tools/build-noxcode.sh             (Command Line Tools build entry point)
  tools/test_protocol.cpp            (host-side protocol tests)
  tools/validate-build-inputs.py     (line endings, versions, project membership)
  MacKernelSDK/                      (submodule, committed)
  reference/                         (gitignored, read-only)
    freebsd/sys/dev/mpr/  + mpi/
    linux/drivers/scsi/mpt3sas/  + mpi/
    IOSCSIParallelFamily/            (Apple OSS, APSL 2.0)
```

Reproduce the references by listing each pinned commit through the GitHub Contents
API and fetching files from `raw.githubusercontent.com`; take the Apple OSS tree
as a `codeload` tarball. In this workspace GitHub is reached through
`http://127.0.0.1:10808`, and HTTPS downloads run in the MCP console per the
workspace rules. A moving branch head is never a baseline.

## 3. Architecture decisions

| Route | Approach | Verdict |
|---|---|---|
| **A (adopted)** | Purpose-built `IOService` implements the HBA; each disk instantiates an `IOBlockStorageDevice` subclass nub, and the system `IOBlockStorageDriver` produces `IOMedia`/`IOMediaBSDClient` | vtable slots and SDK headers were checked offline against the target system and the driver loads and publishes `IOMedia` on hardware; block-device state, error propagation and recovery stay in this driver |
| B | Subclass `IOSCSIParallelInterfaceController` and reuse the macOS SCSI stack | static evidence is complete (SDK headers, 117 exported symbols, the vendor HBA sample), but the KPI is marked deprecated and the `SCSIParallelTask` ↔ MPI adaptation would still be new code; a minimal subclass has not been linked or loaded |
| C | `IOService` plus `IOUserClient` handing CDBs through | debugging only, cannot create a `/dev/disk`; not a main route |

Supporting facts: the target system's storage families (`IOStorageFamily`,
`IOSCSIArchitectureModelFamily`, `IOSCSIParallelFamily`) all live inside BootKC, so
"the system has no SCSI stack" is false; `OSBundleLibraries` is chosen from the
APIs actually used and the provider's compatible range, so the existing
`IOStorageFamily = 1.0` against a 2.1 runtime is not a contradiction. FreeBSD's
suspend/resume returns `EINVAL`, so the power path has to be written here.

## 4. Capabilities and boundaries

The status column says which build the evidence belongs to. *Current* means this
source, measured on one target. *Baseline* means the preceding build; it is
regression coverage only. *Implemented* means the code exists without hardware
evidence.

| Capability | Status | Boundary |
|---|---|---|
| IOC initialization, Facts-constrained queues, interrupt replies, event subscribe/ACK | Current | one hardware reply ring; 32-descriptor index batches, host index written only when non-empty, interrupt wakeup with bounded polling |
| Asynchronous request scheduling | Current (measured depth 1/4/16/32 matched the request) | 64 request records, at most 32 command windows; degraded by IOC credits and pool capacity; SMIDs 32–63; ready commands retired before free windows are refilled; timeout counted from the original submission |
| Client DMA, SGL/chain and staging fallback | Current, read-only (direct and forced staging both byte-exact; the write tests used the direct path) | client subranges are mapped directly and copied only when direct mapping is off or unrepresentable; a request whose DMA stop is unproved keeps its mapping and completion responsibility, without callback or reuse; that branch has not been reproduced on media |
| Block I/O and cache commands | Current | READ/WRITE(10)/(16), RC10/16, TUR, INQUIRY, SYNC(10)/(16), FUA with an ordered-flush fallback |
| UNMAP | Implemented; qualified on the baseline build only | enabled after a B0/B2 probe, split to the reported limits, never extended beyond the caller's range |
| Discovery and identity | Current, one target | every eligible target enters the target table and gets its own probe/nub; a handle is only a firmware address, and reuse is decided by SAS address and stable identity |
| IR member filtering | Implemented, not verified on an array | volumes, member disks, hot spares and expansion members are refused; the firmware reports IR capability but no volume exists to test against |
| Removal and recovery | Implemented; removal is baseline, TMF escalation is current | per-target TARGET_RESET → drain → REMOVE_DEVICE, all naming that slot's handle; §6 records the expander side effect of a delivered TMF |
| Hotplug and media lifecycle | Baseline only | a pulled disk goes offline while the other target keeps serving; reinsert needs PortEnable rediscovery (two bounded resets at 8 s and 45 s, during which the other target drops out); the slot is reused by SAS address so the disk number stays |
| Sleep/wake and shutdown | Sleep/wake current (two cycles, one target); shutdown baseline only | every published target is flushed before sleep; wake rebuilds the controller before the first topology read, gives config/event and PortEnable replies a watchdog-timed settling window, and brings media back with the same disk number; one planned rebuild per wake (`MPRResetCount` +1) with the sticky/fault counters unchanged |
| Diagnostic gate | Implemented | kernel log lines and every IORegistry property are silent by default; `mpr_debug=1` enables both, including the init/probe/start phase |
| Multi-LUN, multipath, SES/SMP, NVMe/Tri-Mode | Not implemented | see §1.3 |

## 5. Milestone status

| Stage | Content | Status |
|---|---|---|
| M0 environment | Command Line Tools build, internal ESP injection, matching and loading | done |
| M1 registers and IOC | READY → OPERATIONAL, IOC Facts, DMA pools, Manufacturing Page 0 | done |
| M2 interrupts, replies and events | interrupt-driven replies, reply-frame retirement, per-command dispatch, EventMasks/ACK, PortEnable | done; the retirement path is covered by long runs, wrap count was never counted separately |
| M3 single-disk read-only | independent-disk discovery, identity and capacity, read-only nub, large sequential read consistency | done |
| M4 read/write and recovery | WRITE, cache flush/FUA, UNMAP, sense, timeout/TMF/reset | done; the fault-injected soak stopped short (§6), the serial-command prototype's 90-minute run is in §7 |
| M5 topology, hotplug and PM | nub add/remove, handle-reuse isolation, safe stop, sleep/wake | complete on the baseline build (direct attach and the dual-target backplane, including a pull with I/O in flight); the current source re-qualified sleep/wake only |
| M6 diagnostic tooling (optional) | standalone `IOUserClient`/tool, statistics export | not done; IORegistry properties and boot arguments are used instead |
| Performance data path | client DMA, bounded command pool, ready-first harvest and refill, batched single-ring replies with wakeups | done on one target: native build/link/sign/load, read-only correctness, A/B latency and throughput against the preceding build, one TMF escalation, a small write and flush cycle, two sleep/wake cycles, four recovery variants; UNMAP, physical replug, dual target and shutdown not re-run |
| M7 later | IR volumes, SES, multiple hardware queues, boot disk, signed release; route B / dext evaluation | outside the current scope |

## 6. Not yet done

- "One wake, zero resets" is not achieved: after S3 the IOC must go through a diag
  reset and IOC Init before it is operational, so every wake still costs one
  planned rebuild and `MPRResetCount` +1. That reset now happens before the worker
  reads topology, so it is no longer triggered by a config-page timeout or a
  spin-up probe; `MPRStickyFaultCount`, `MPRFaultRecoveries` and
  `MPRProtocolRecoveryPending` stay unchanged across the wake.
- Diagnostics and retry for failures in the tail of a reset: a PortEnable or
  initialization failure is still recorded only as a generic
  `MPRReinitState=failed`, and recovery depends on the protocol-fault path forcing
  another reset. Per-stage failure points and in-place retry are missing.
- Side effect of task-management requests on this expander (recorded, not fixed, by
  decision): a TMF that really reaches the target (ABORT/LUN/TARGET) is always
  accompanied by a SAS removal report (`MPRTopoRemovalHits` 1:1), and the
  re-enumeration that follows interrupts the medium for tens of seconds (measured:
  more than 45 s without completion after a single LUN RESET). With
  `mpr_tmf_fail=3`, which rebuilds the whole IOC instead, `MPRTopoRemovalHits` does
  not change and transfers and digests are normal once the medium returns. Two
  points remain unresolved: a single event recovers correctly, but after twelve
  accumulated events raw requests above 128 KiB returned `ENXIO` (gone after a
  reboot, never reproduced with one event) with no known cause; and
  `MPRRecoveryState` is not rewritten to `ok` after a reset-based recovery, so it
  is a breadcrumb rather than a live state.
- The fault-injected soak never ran to budget: the fixture must reopen the device
  by its new BSD number after a medium rebuild, and treat expected resets and
  brief error windows as in scope.
- Remaining data-path gaps: UNMAP, physical replug, shutdown, FUA selection (not
  reachable from user space here) and dual-target isolation have not been tested on
  this source; the "DMA stop unproved" branch has only been read in source, since
  provoking uncontrolled DMA is out of bounds. Large sequential throughput
  saturates near 150–155 MiB/s from QD 4 and does not grow with depth, which is a
  property of that disk and this path rather than a scheduling limit; another disk
  or direct attach must be re-measured. One hardware reply ring, one scheduler
  thread and the legacy synchronous staging path remain; multi-queue/MSI-X is not
  implemented.
- A different-capacity disk, regression across macOS versions, signing and release.
- A minimal route B subclass build and load check, only if that route is chosen.

## 7. How the work is verified

- Host side: `tools/test_protocol.cpp` validates the protocol layer against
  independent golden bytes and an independent chain decoder (2,000 deterministic
  SGL cases, credit/SMID boundaries, out-of-order completion prefixes). It builds
  with ASan/UBSan and `-Wall -Wextra -Werror` on the target system's compiler and
  runs 158,638 assertions. `tools/validate-build-inputs.py` checks LF line endings,
  agreement between bundle, runtime and CLI versions, and that every translation
  unit is part of the Xcode project.
- Build and load: `tools/build-noxcode.sh` produces `MPR.kext`, verifies the kmod
  entry points, bundle type, driver-internal symbols and plist, then signs it ad
  hoc. After injecting it into the internal ESP, compare that ESP's `Info.plist`,
  the artefact hash and the `kmutil showloaded` version. `MPRBuild` is published
  only with `mpr_debug=1`, so enable diagnostics before reading it. A
  cross-compiled Mach-O is not a linked, signed, loadable kext.
- Performance comparison: read the raw device (no file cache), with the same data
  window, the same load and the same warm-up; three repeats per configuration with
  the spread recorded. Shapes are 128 KiB/1 MiB sequential and 4/16/64 KiB random
  at QD 1/4/16/32. Throughput comparisons run with `mpr_debug` and serial logging
  off; depth and direct-map switches are confirmed against the reported values in
  short diagnostic runs. Compare only against an already deployed older build whose
  version and hash are recorded separately; never against the working tree.
- On hardware: every judgement that relies on an IORegistry property or a kernel
  log line requires `mpr_debug=1`; with output silent by default an absent property
  is expected and is not evidence of a failed load. Checks: two byte-identical
  1 GiB sequential reads, write followed by a byte-exact readback, remount after
  UNMAP, physical pull and pull with I/O in flight, an injected timeout escalating
  through TMF to reset, and sleep/wake. Behind the expander the same bounded
  read/write and UNMAP checks apply. With two targets, also compare LBA0 and
  high-LBA reads on both, writable-disk readback, and per-target attribution in
  `MPRTargetNWriteCount`. PM changes need `MPRResetCount` before and after for the
  old and the new build, together with `MPRResetInitiator`, `MPRStickyFaultCount`,
  `MPRFaultRecoveries`, `MPRProtocolRecoveryPending`, `MPRWakeRebuildState`,
  `MPRProbeRetries` and both media's online times; test short and long sleeps,
  because neither "online" alone nor a single reset count decides whether the wake
  path improved. Repeat the pull tests only once test media and permission to pull
  are available, and report anything not authorised as untested.
- Long run: budget by command count instead of wall-clock and watch for
  driver-side accumulation (reply frames, SMIDs, event ACKs, property dictionary,
  a degenerate recovery loop) rather than media endurance. The earlier
  serial-command prototype passed about 3.45 M commands over 90 minutes (flat p99,
  `MPRResetCount` unchanged, stable property count, unchanged reference-region
  hash). The current asynchronous scheduler ran 21.1 M 4 KiB random reads with
  direct mapping (10 min), 63.7 M (30 min) and 10.7 M with forced staging, sampling
  `MPRResetCount`, `MPRStickyFaultCount`, `MPRUnexpectedCompletionCount`,
  `MPRAsyncHeldDMA`, the property count and the `ioreg` size every minute with no
  drift. Mixed sizes (4 KiB–1 MiB, 328 k chained descriptors) and eight threads of
  mixed read/write (3.24 M operations, 2.27 M in-place 4 KiB writes, 6,331 fsyncs)
  produced zero mismatched readbacks and an exact final cache-disabled comparison.
  Boundary cases are listed separately: end of media, one block past the end,
  zero length, unaligned offsets and sub-block lengths, a 16 MiB single request,
  and 20,000 random single-block reads compared over two passes.
- Recovery variants are classified by the request that actually reaches the target:
  `mpr_tmf_fail=0/1/2` really delivers ABORT, LUN and TARGET, while
  `mpr_tmf_fail=3` skips all three and lands on a whole-IOC rebuild. Inject once
  per run (`mpr_timeout_every` at 1e6 or a one-shot switch), compare
  `MPRTopoRemovalHits`, `MPRTopologyGeneration`, `MPRResetCount`,
  `MPRResetInitiator`, target generations and BSD numbers around the event, then
  repeat the read-only digest comparison.
- Stress versus fault injection: stress runs aim at the highest command count and
  do not write large amounts of data to the medium. Write verification stays inside
  one 32 MiB file on the dedicated test volume plus 2,000 in-place 4 KiB rewrites,
  compared byte for byte with the cache disabled before deletion. Fault injection
  is read-only (`mpr_timeout_test`, `mpr_timeout_every`, `mpr_tmf_fail`) and stops
  increasing load on any panic, cross-disk transfer, duplicate callback or
  uncontrolled DMA, preserving the evidence; the current injection triggered fabric
  removal events and was stopped after twelve on that rule.
- Destructive writes are limited to a test disk whose identity was re-verified on
  the spot, and the written window is restored byte for byte afterwards. An
  IORegistry "online" property never replaces byte counts and readback content.

## 8. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | A system update shifts vtables, symbols or load behaviour | re-extract KC and re-run the offline checks before upgrading, then load and re-run the read-only regression; never extrapolate old conclusions |
| R2 | Multi-target coverage is two terminal disks on one backplane, with one disk pulled at a time and one in-flight request, and it is baseline evidence | re-run removal, hotplug and PM on the current source before claiming them; do not extrapolate to other expanders or concurrent pulls |
| R3 | The firmware can do IR while filtering coverage is limited | refuse to publish when an independent identity cannot be established; do not generalise single-disk results to IR configurations |
| R4 | Reinsert behind this expander depends on PortEnable rediscovery and two bounded IOC resets | the other target drops out during those resets and that is recorded; do not present the combination as general expander support |
| R5 | The current DMA and scheduling ownership rules are qualified for read-only, write/flush and PM on one target only; removal, UNMAP and dual-target paths share them without re-qualification, and one reply ring caps scaling | keep the unproved-DMA-stop rule (no unmap, no callback), compare against forced staging, validate data and recovery before throughput, and evaluate multiple hardware queues separately |
| R6 | Route B's SPI KPI is deprecated | stay on route A; complete a build and load check before switching |
| R7 | The ESP holds a file that was not loaded, or an older build is still running | ESP-only deployment; verify the running version after reboot; back up and validate any config change |

## 9. Licensing

1. FreeBSD `mpr` is BSD-2-Clause. The MPI wire definitions transcribed into
   `MPR/MPI/mpi2_min.h` and `MPR/MPI/mpi2_target.h` keep the Broadcom copyright and
   its three-clause redistribution terms; source and licence inventory are in
   `NOTICE`.
2. Linux `mpt3sas` (GPL-2.0) is a behaviour and protocol reference only; no code is
   copied into the driver, and `reference/` is neither committed nor built.
3. MPI structures are sourced exclusively from FreeBSD `mpi/*.h`; differences from
   the Linux version are noted in `MPI/` comments only, never mixed.
4. `MacKernelSDK` headers are used for compilation under their own licence and are
   not copied into the driver tree.
5. The KC and IDA database under `ida/` are used only to confirm interfaces,
   vtables, symbols and dependencies; no Apple implementation is copied and no KC
   address is written into the driver.
6. `reference/IOSCSIParallelFamily/` (APSL 2.0) is referenced for headers, ABI and
   dependency keys only; if its constants or structures are ever used, keep the
   APSL notice and list source and commit in `NOTICE`.

## 10. Operating constraints

- The driver is installed only into `EFI/OC/Kexts/MPR.kext` on the verified
  **internal SATA disk's** ESP and injected through OpenCore `Kernel > Add`. Never
  write `/Library/Extensions`, another system volume or a USB EFI, and never treat
  an AuxKC rebuild as a deployment step.
- Back up `config.plist` beside itself before editing; after editing run
  `plutil -lint`/`ocvalidate` and confirm the only difference is the intended one.
  A kext-only replacement leaves the config unchanged.
- Uploaded text files must be LF; re-check after every edit instead of relying on an
  earlier check of an older copy.
- Serial debugging stays off by default (`debug=0x100`, no `serial=5`); enable it
  only to capture kernel logs and disable it again immediately afterwards.
- Any backup expected to exceed 1 GB needs its scope, size and location stated and
  an explicit permission for that backup beforehand; acceptance runs use streaming
  checksums and never create a whole-disk image on their own.
- Hardware writes, deployment and reboots happen within the specific scope
  authorised in the session; write tests are limited to a test disk whose identity
  was re-verified.
