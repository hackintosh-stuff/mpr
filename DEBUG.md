# DEBUG — implementation notes for MPR

Date: 2026-11-13. Driver version: 0.20.5.

This document describes source behavior and diagnostics. What has been
qualified at runtime, and what has not, is in [README.md](README.md); scope and
remaining work are in [PLAN.md](PLAN.md).

## 1. Concurrency and ordering

`MPRBlockDev::doAsyncReadWrite` copies the completion and retains the descriptor
through `submitAsyncBlocks`. A successful return means the bounded queue has
accepted responsibility for exactly one completion. A rejected request gets
an error return and no callback.

The scheduler has 32 request records and four 64 KiB command windows. It
rotates among requests, can issue several chunks of one request when others
are absent, and harvests every posted window even if a sibling fails.
Queue exhaustion is reported as `kIOReturnNoResources`; it is not silently
accepted or allowed to grow without a bound.

The I/O worker and client-completion worker are distinct, serialized
`thread_call` objects at kernel priority. Completion delivery holds neither
the scheduler lock nor `fIOLock`. A completion may issue a synchronous flush
without preventing the I/O worker from retiring the preceding requests.

A synchronous cache operation or UNMAP installs a sequence fence. Earlier
accepted requests finish before the operation; newer requests remain queued.
The media generation is captured before waiting and checked under the
transport lock, so a request for a departed disk cannot act on its replacement.

Lock roles:

| Lock | Protected state |
|---|---|
| Scheduler IOLock | Request acceptance, sequence/fence state, completion records |
| `fIOLock` | Hardware batches, management staging, discovery, recovery and PM |
| `fReplyLock` | Command ownership, reply slots, event queue and topology latches |
| `fPostLock` | The two halves of an MMIO request descriptor |

The permitted inner lock order is reply then post. No wait or allocation
occurs under either simple lock. No client notification is issued while
holding the transport lock.

## 2. DMA, SGL and ownership

Each asynchronous window has its own resident data buffer, `IODMACommand`
mapping and bounded chain storage. The mapper belongs to the PCI provider
when available. Data addresses are restricted to 63 bits. The SGL encoder
checks the complete address range, total length, request-frame geometry,
chain depth and storage capacity before writing a list.

IEEE request ChainOffset and NextChainOffset are in 16-byte elements.
Only the final data element has END_OF_LIST. A chain's length describes the
actual target elements, including its next chain element when present.

The implementation caps chains at four frames per window and caps each
frame at 1024 bytes while honoring lower IOC limits. An unrepresentable
mapping uses the established contiguous resident staging window. This is
scatter/gather support with copies, not direct DMA into a client's buffer.

SMIDs have explicit free, reserved, posted, completed, quarantined and
cancelled ownership states. A completion must match the outstanding function
and, for SCSI/TMF, the device handle. It must not overwrite an unconsumed
completion. Software generations are not treated as tags present in hardware
replies: a timed-out SMID remains unavailable until a reply or a proved DMA
stop ends its ownership.

Sense storage is separate from configuration pages and independent per SMID.
Both fixed and descriptor sense are bounded by the actual SenseCount.
Block I/O requires an exact transfer count and valid SCSI completion state;
only commands such as INQUIRY explicitly accept a shorter legal payload.

Management, high-priority TMF and EVENT_ACK resources are reserved separately.
Queue depths are constrained by IOC Facts, and TMFs obey the reported
high-priority credit. A reset synthesizes cancelled completions for affected
runtime commands instead of making each waiter time out again.

If hardware cannot be proven quiescent, DMA-visible resident resources remain
retained. Client buffers are never the target of that DMA, so failed client
requests can still complete. Such a state requires controlled recovery and is
not a reason to reuse the quarantined window.

## 3. Disk discovery and identity

Discovery combines SAS IO Unit Page 0 with SAS Device Page 0 GET_NEXT_HANDLE.
The latter reaches a terminal disk behind an expander backplane. Each scan is
merged into a persistent target table instead of replacing it: a handle that is
still listed keeps its nub, medium and probed geometry, and only its current
device page fields are refreshed. A handle the firmware stops listing becomes a
removal candidate; a handle that reappears cancels that decision and, because the
medium behind it may be a different disk, advances that slot's generation.

GET_NEXT_HANDLE also returns SCSI Enclosure Processor (SEP/SES) devices attached
to an expander. They carry SSP end-device bits but are not block disks, so
candidates with `MPI2_SAS_DEVICE_INFO_SEP` are excluded from both the disk
count and the standalone-target count. Without this exclusion an enclosure
processor counts as a disk and is probed as if it were one.

On IR-capable firmware, RAID Configuration Page 0 is enumerated to exclude
volumes, member disks, hot spares and expansion members. Configuration
transport failures or malformed pages fail closed. IR change events retire
the old eligibility decision.

SATA identity prefers ATA IDENTIFY model and serial; VPD 0x83/0x80 are
fallbacks. SAS address and DevHandle are topology properties, not persistent
identity: each slot keeps the SAS address and stable identity it was probed for,
and compares a fresh probe against them. A change of identity, block count or
block size advances that slot's generation, so I/O and cache commands accepted
for the old medium fail instead of reaching the replacement.

Every eligible handle from GET_NEXT_HANDLE is recorded in a bounded target
table (`MPRTargetSlot`) and published as `MPRTargetCount` plus per-target
`MPRTargetNHandle`, `MPRTargetNDeviceInfo`, `MPRTargetNSasAddress`,
`MPRTargetNPresent`, `MPRTargetNProbed`, `MPRTargetNOnline`,
`MPRTargetNReadOnly`, `MPRTargetNBlocks`, `MPRTargetNBlockSize`,
`MPRTargetNIdentity`, `MPRTargetNModel`, `MPRTargetNSerial`,
`MPRTargetNRemovalState`, `MPRTargetNGeneration`,
`MPRTargetNWriteCount` and `MPRTargetNFailureCount` properties.
Each target is probed through the read-only management chain and gets its own
`MPRBlockDev` nub; `MPRPublishedTargetCount` counts successful publications.
An async request carries the originating DevHandle, and the worker resolves
geometry, admission, fast-path capability and the FUA mode from that target's
slot. The request frame supplies the DevHandle that `postOwnedRequestLocked()`
records, so completions stay bound to the owning target. `postScsiIo()` chooses
the descriptor type and builds the fast-path request descriptor from that same
target: the IOC takes the DevHandle from the descriptor's high word, so using
the controller's default handle there would send every target's commands to one
disk. A nub reports its own INQUIRY vendor/product/revision from the slot that
names its handle.

Access is fail-closed. `MPRWritableDevices` in the kext personality is an array
of Model/Serial rules; only a SATA disk whose ATA model and serial match a rule
with a concrete serial is published writable, and an SSP target is read-only
regardless of any rule. Every other target, including a disk from an unknown
array, stays read-only: `reportWriteProtection()` reports write protection,
write requests are rejected with `kIOReturnNotWritable`, UNMAP is rejected, and
cache synchronisation is a no-op that sends no command. The nub records
`MPRBlockDevReadOnly`, `MPRBlockDevHandle` and the controller records
`MPRWriteRuleCount` plus the per-target properties above.

## 4. Hotplug and recovery

The watchdog monitors IOC state, event acknowledgement and topology work, and
republishes the per-target counters. One worker serialises topology work under
the transport lock and delivers media-state changes per target with no lock
held. It also takes an unconditional snapshot every five seconds: discovery
must not depend on firmware events, because a returned disk is not always
announced.

Each pass merges the snapshot into the target table. A target the fabric dropped
is retired with TARGET_RESET -> drain of its aborted commands -> SAS IO Unit
Control REMOVE_DEVICE, all naming that slot's handle, so a reset never lands on
another target. A live handle that still carries the same SAS address cancels
the removal; a handle now naming a different device is adopted as a new target
and never receives the old removal. A removal that cannot be proved is that
target's failure (`MPRTargetNRemovalState`, `MPRRemovalFailures`), not a
controller protocol fault, and escalates to the bounded IOC reset.

Presence decides a removal: once the device page answers "absent", the medium is
unbound locally even if REMOVE_DEVICE is refused, because the device is already
gone from the fabric and a disk that was only reported absent is rediscovered by
a later snapshot. Only "commands may still be in flight" keeps the medium bound
and escalates.

Behind this expander a reinserted disk is not reliably re-listed by
GET_NEXT_HANDLE and produces no TARG_ADDED event, so enumeration alone cannot
find it: the IOC re-runs fabric discovery only on PortEnable. The watchdog
therefore spends at most two rediscovery resets, at 8 s and 45 s after a medium
was retired, and then stops. Each reset interrupts the other targets: they are
invalidated, re-probed and reported OFFLINE -> ONLINE again, which takes seconds
to about 25 s depending on the fabric. That cost is inherent to an IOC-level
rediscovery without per-phy SMP control.

A disk that returns after REMOVE_DEVICE carries a new firmware handle. It is
matched by SAS address to its retired slot, so its nub and IOMedia come back and
the same BSD disk number is republished. A retired slot whose disk does not
return is released once a fresh medium is published, so repeated replugs do not
exhaust the target table.

A target that is present but not yet published is probed in place. Probing is
bounded per target: after three failed attempts the disk is left offline until
it is physically reinserted, and the remaining targets keep their media. A
repeated probe failure or a removal that cannot be proved escalates to the
bounded IOC reset, capped at three per insertion. An empty fabric, including an
expander with no eligible terminal disk, is polled without an endless reset
loop.

Timeout recovery attempts ABORT TASK, LUN RESET and TARGET RESET, each naming
the handle recorded for the failed SMID, subject to available TMF credit and
terminated-command evidence, then a bounded IOC reset. Writes with uncertain
completion are not replayed. A read whose whole batch failed with a decoded
UNIT ATTENTION is retried once, after all DMA for that batch has ended.

## 5. Cache, FUA and UNMAP

READ/WRITE selects 10- or 16-byte CDBs from the entire requested range.
SYNCHRONIZE CACHE uses the corresponding width; a range larger than the
16-byte block-count field is satisfied by a stronger whole-medium flush.
FUA uses the native capability when known, otherwise an ordered flush after
the request's successful writes.

UNMAP requires B0 Block Limits and B2 Logical Block Provisioning/LBPU.
Every input extent is validated before the first discard. The generated
batches obey the maximum block count, descriptor count and per-descriptor
32-bit block field. Optimal granularity and alignment are recorded as hints;
the driver never rounds outward into data the caller did not discard.
A failure disables UNMAP while leaving ordinary I/O subject to the normal
device/recovery state. Read-zero behavior is claimed only when advertised.

## 6. Power and shutdown

`setPowerState` is the registered PM entry point. The normal operating vote
is On. Sleep closes asynchronous acceptance, fences accepted I/O, flushes each
writable target through that target's own handle, disables reply delivery,
resets/quiesces the IOC and disables PCI bus mastering. Ownership is reset only
after DMA stop is proved.

Wake restores PCI access, rebuilds queues/events/topology, re-enumerates and
re-probes every present target, then reopens acceptance; each medium takes
OFFLINE -> ONLINE around that rebuild. A failed wake retires media and requests
bounded recovery. `systemWillShutdown` follows the same quiesce path.
Sleep/wake and shutdown are qualified on the directly attached disk only; they
have not been repeated with several targets behind an expander.

Stop waits for the I/O and completion workers outside the transport lock,
then tears down DMA. Scheduler/transport locks remain alive until final
service release so callers retained before stop can unwind safely.

## 7. Diagnostics

| Area | Useful IORegistry properties |
|---|---|
| Build and media | `MPRBuild`, `MPRFeatures`, `MPRMediaPresent`, `MPRMediaGeneration`, `MPRMediaReady`, `MPRPublishedTargetCount` |
| Targets | `MPRTargetCount`, `MPRTargetNHandle`, `MPRTargetNSasAddress`, `MPRTargetNPresent`, `MPRTargetNProbed`, `MPRTargetNOnline`, `MPRTargetNRemovalState`, `MPRTargetNMediaStatus`, `MPRTargetNGeneration` |
| Discovery and removal | `MPRScanCount`, `MPRRemovedMediaCount`, `MPRRemovalFailures`, `MPRRemovalStage`, `MPRRetiredSlotsReleased`, `MPRRediscoveryResets`, `MPRRediscoveryState`, `MPRFaultRecoveries`, `MPRFaultRecoveryState` |
| Async and DMA | `MPRAsyncRequestLimit`, `MPRAsyncCommandLimit`, `MPRAsyncPeakRequests`, `MPRMappedCommands`, `MPRChainedCommands`, `MPRBounceCommands` |
| Identity/filter | `MPRStableIdentityValid`, `MPRStableIdentity`, `MPRIdentityState`, `MPRRaidFilterState`, `MPRStandaloneTargetCount`, `MPRSasDevicePageState` |
| Capabilities | `MPRNativeFuaSupported`, `MPRFuaPolicy`, `MPRUnmapEnabled`, `MPRUnmapMaxLbaCount`, `MPRUnmapMaxDescriptors`, `MPRUnmapReadsZero` |
| Recovery | `MPRHotplugState`, `MPRResetCount`, `MPRReinitState`, `MPRRecoveryState`, `MPRDeviceOffline`, `MPRTmfHandle`, `MPRTmfResetStatus` |
| Protocol | `MPRProtocolRecoveryPending`, `MPRProtocolFaultCode`, `MPRStickyFaultCount`, `MPRBadReplySite`, `MPREventAckQueued`, `MPREventAckFailed` |
| Power | `MPRPowerState`, `MPRPowerFlushStatus`, `MPRPowerFlushCount`, `MPRStopDMAState`, `MPRWatchdogIOCState` |
| Writes | `MPRWriteCount`, `MPRWriteBytes`, `MPRLastWriteLba`, `MPRLastWriteSmid`, `MPRTargetNWriteCount`, `MPRTargetNFailureCount` |

A property saying "online" is not data-integrity evidence. Always pair it with
the active IOMedia count, actual byte count and readback content.

Opt-in boot arguments `mpr_timeout_test=1`, `mpr_tmf_fail=N` and
`mpr_timeout_every=N` exercise recovery; none is set by default. Serial kernel
logging remains off by default under the workspace rules.

## 8. Coverage against the FreeBSD driver

The comparison uses the fixed FreeBSD source in
`reference/freebsd/sys/dev/mpr` (commit in PLAN section 2) and covers the
controller mechanisms plus the disk-level equivalents this driver needs.
FreeBSD's CAM/newbus/bus_dma/sysctl interfaces are platform glue; their API
names are not macOS requirements.

| Mechanism | FreeBSD reference | Implementation |
|---|---|---|
| Request ownership and completion | `mpr_command`, `mpr_complete_command`, `mpr_intr_locked` | Per-SMID ownership states, checked replies, independent sense, quarantined timeouts and cancelled reset completions |
| Asynchronous dispatch | CAM submission and command callbacks | Request queue, four shared command windows, separate completion context, sequence fences |
| DMA, scatter/gather and chain | `mpr_add_chain`, `mpr_push_ieee_sge`, IOC Facts | Device-visible resident mappings, bounded IEEE chains, explicit contiguous fallback |
| Queue resources | `mpr_resize_queues`, high-priority command pool | Facts-constrained geometry, reserved management/TMF/ACK resources |
| SCSI data contract | `mprsas_action_scsiio` plus CAM disk policy | READ/WRITE(10)/(16), exact block transfers, bounded sense, SYNC(10)/(16), FUA fallback |
| Thin provisioning | CAM disk and UNMAP policy | B0/B2 capability probe, input validation, bounded descriptor and block batching |
| Discovery | SAS config pages and discovery events | GET_NEXT_HANDLE enumeration merged into a per-handle table; SEP/SES devices excluded; every eligible target published |
| IR membership protection | IR config handling, RAID_COMPONENT filtering | RAID Configuration Page0 enumeration; volumes, members, spares and expansion disks refused |
| Persistent identity | SATA IDENTIFY, SCSI identifiers | Per-target ATA model+serial or validated VPD identifier, capacity/block-size comparison, per-target media generations |
| Target removal | `mprsas_prepare_remove`, `mprsas_remove_device` | Per-target guarded TARGET_RESET/drain/REMOVE_DEVICE naming the removed handle, with handle-reuse checks and bounded fallback |
| Recovery | abort/LUN/target/IOC reset and watchdog | TMF escalation for the failed command's handle, ownership proof, no replay of uncertain writes, IOC state monitoring |
| Sleep and shutdown | FreeBSD suspend/resume return EINVAL; separate shutdown path | Native IOKit PM with per-target flush and wake revalidation |
| Builds | OS-specific build system | All driver translation units in the CLI and Xcode builds |

Outside this configuration, and not silently treated as implemented: sleep and
wake with several targets behind an expander; multiple LUNs and general SCSI
passthrough; DPM persistent numbering and multipath; IR logical volumes and RAID
management; SES/enclosure control, SMP passthrough, firmware update and
user-space tools; NVMe/PCIe/Tri-Mode, SAS2 and GEN35 hardware; direct DMA into
client buffers, multi-queue/MSI-X scaling and tuning.

## 9. Host-side checks

`tools/test_protocol.cpp` exercises the shared wire-format helpers against
independent golden bytes and a separate chain decoder: 10- and 16-byte CDB
boundaries, high LBAs, arithmetic overflow, legal and truncated VPD, RAID page
layouts and membership, bounded fixed and descriptor sense, chain
depth/address/length rejection, sentinel protection and randomized SGL
reconstruction. AddressSanitizer and UndefinedBehaviorSanitizer are used when
available: 136,593 assertions and 2,000 deterministic SGL cases pass, including
the intentional rejection cases.

`tools/validate-build-inputs.py` checks LF text, bundle and runtime version
consistency, and inclusion of every driver translation unit in the Xcode
project; the CLI build compiles every source file. Against MacKernelSDK with a
Darwin target, compilation verifies C++/IOKit declarations and produces Mach-O
objects with no diagnostics under `-Wall -Wextra -Werror`, and the driver has no
unresolved driver-internal symbols. Linking, ad hoc signing and loading are
separate checks that require macOS; all three pass on the test machine.

