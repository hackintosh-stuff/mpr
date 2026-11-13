# DEBUG — implementation notes for MPR

Date: 2026-11-13. Driver version: 0.21.0.

This document describes source behavior and diagnostics. What has been
qualified at runtime, and what has not, is in [README.md](README.md); scope and
remaining work are in [PLAN.md](PLAN.md). Diagnostic output is opt-in; see
section 7. The current data path is built natively with the Command Line Tools,
linked, signed ad hoc and loaded from the internal ESP. On one target it is
qualified for read-only block I/O, a small write/flush cycle and two sleep/wake
cycles. The UNMAP, removal, reinsertion and shutdown observations in this
document were made with the preceding build and remain a regression baseline.

## 1. Concurrency and ordering

`MPRBlockDev::doAsyncReadWrite` copies the completion and retains the descriptor
through `submitAsyncBlocks`. A successful return means the bounded queue has
accepted responsibility for exactly one completion. A rejected request gets
an error return and no callback.

The scheduler has 64 request records and at most 32 independent 64 KiB command
windows. It rotates among eligible requests, harvests every ready window and
refills free windows without an array-order wait or a whole-batch drain.
Request admission and completion remain bounded. Queue exhaustion returns
`kIOReturnNoResources`, with no callback for the rejected request.

Each command keeps its original 15-second post-time deadline. A sequence ticket
sampled before inspecting replies prevents a completion between the inspection
and sleep from being lost. Interrupts wake the worker; a 20 ms maximum wait
also polls when an interrupt is lost. No fixed 1 ms sleep is charged to each
completion. The transport mutex is released between scheduler sweeps and before
this wait. Out-of-order successes beyond a failed chunk are not reported as a
contiguous successful prefix.

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
| `fIOLock` | Scheduler sweeps and mapping ownership, management staging, discovery, recovery and PM |
| `fReplyLock` | Command ownership, reply slots, event queue and topology latches |
| `fPostLock` | The two halves of an MMIO request descriptor |
| `fReplyWaitLock` | Reply activity sequence and atomic sleep/wakeup |

The permitted inner lock order is reply then post. No wait or allocation
occurs under either simple lock. No client notification is issued while
holding the transport lock. Reply activity is notified only after dropping the
simple locks; the wait mutex is never acquired under a simple lock. Diagnostic
snapshots use a transport try-lock so the reply workloop cannot block behind a
worker that is waiting for that same workloop.

One hardware reply ring remains. Its consumer retires at most 32 descriptors
per ownership-lock acquisition, publishes the current host index under that
same lock, then drops the lock between batches. An empty drain does not write
the host-index register. A call can continue for up to one ring depth so a
budget boundary does not strand already-posted replies. There are no parallel
consumers of the same descriptor and no claim of hardware multi-queue support.

## 2. DMA, SGL and ownership

Each asynchronous window owns a client `IODMACommand`, an independently mapped
64 KiB resident fallback and optional 4 KiB chain storage. The mapper belongs
to the PCI provider when available. Client pages are prepared once per accepted
request, and each command attaches that descriptor with automatic preparation
disabled, then prepares only its own offset/length. Segment generation starts
at zero relative to that prepared subrange, including for multi-megabyte client
requests. The DMA command has no whole-descriptor transfer cap; the scheduler
still limits each posted command to 64 KiB.

Data addresses are restricted to 63 bits. The SGL encoder checks the complete
address range, exact total length, request-frame geometry, chain depth and
storage capacity before writing a list. Each command has independent mapping
and chain lifetime. A mapping or representability failure before posting
falls back to its private mapped staging buffer; no partial SGL is submitted.
A failed pre-submit read mapping is detached without copying invalid bounce
data back. Successful read DMA is synchronized before the client is completed.
IODMACommand may itself bounce on some platforms; direct mode removes the
mandatory driver copy, not every possible IOKit copy.

IEEE request ChainOffset and NextChainOffset are in 16-byte elements.
Only the final data element has END_OF_LIST. A chain's length describes the
actual target elements, including its next chain element when present.

The implementation caps chains at four frames per window and caps each
frame at 1024 bytes while honoring lower IOC limits. The frame in use on this
hardware reports eight 16-byte chain elements, so a 64 KiB client chunk whose
mapping spans more than one inline element is described by two or three chained
frames. With direct mapping enabled every client chunk in the measured runs was
described this way while `MPRDirectMapFailures` stayed zero, which is the
observed proof that the chain encoder and its storage bound are large enough
for a page-fragmented 64 KiB mapping; a mapping that needed more than the
window's chain storage would fall back to the private staging buffer and be
counted as a mapping failure instead. An unrepresentable
mapping uses the private contiguous resident staging window. At maximum depth,
the fallback payload pool is 2 MiB and chain storage is at most 128 KiB, plus
mapping/request metadata and the unchanged synchronous/probe pools. These
buffers are allocated during initialization, not on the completion hot path.

Asynchronous commands use SMIDs 32-63. SMIDs 1-9 remain management/probes,
10-25 retain the synchronous bulk path, 26-28 are TMF, 29 is flush, 30 is
UNMAP, and 31 is EVENT_ACK. The effective asynchronous count is the minimum of
allocated slots, 32, available request frames/credits above 32, and each reply
queue depth minus one unused entry and eight reserved entries. Sense/owner
arrays have 128 entries. Every IOC reinitialization recomputes this bound;
a zero result fails queue initialization rather than overlapping reservations.

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

If hardware cannot be proven quiescent, resident resources and active client
mappings remain retained. A direct request keeps its prepared descriptor,
request record, buffer/controller references and pending callback until a late
completion or a proved DMA stop makes it safe. A barrier returns NotReady when
this held state blocks it; PM still attempts hardware quiescence instead of
waiting indefinitely for an unsafe callback. The watchdog can reap a held
request after a later proof. Stop releases and error-completes it only after
quiescence; an unproved final stop deliberately retains it instead of risking
DMA into recycled client pages. This can leave an I/O unresolved and requires
controlled recovery. The resident-only fallback may error-complete its client
while retaining the quarantined private buffer, because that DMA cannot reach
the client's pages. No timed-out write is replayed.

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

A snapshot is matched to the table by SAS address first, then by DevHandle. After
a controller reset the IOC can hand one device a different handle and can swap
two devices' handles; matching by handle first would then publish one disk twice
and orphan the other's medium. A handle that moved for the same SAS address is
rebound in place (generation advances, because queued I/O still carries the old
handle), a handle that now names a different address is invalidated, and the same
address listed twice in one snapshot is adopted once (`MPRTopologyDuplicate`).

Every eligible handle from GET_NEXT_HANDLE is recorded in a bounded target
table (`MPRTargetSlot`) and published as `MPRTargetCount` plus per-target
`MPRTargetNHandle`, `MPRTargetNDeviceInfo`, `MPRTargetNSasAddress`,
`MPRTargetNPresent`, `MPRTargetNProbed`, `MPRTargetNOnline`,
`MPRTargetNBlocks`, `MPRTargetNBlockSize`,
`MPRTargetNIdentity`, `MPRTargetNModel`, `MPRTargetNSerial`,
`MPRTargetNRemovalState`, `MPRTargetNGeneration`,
`MPRTargetNProbeState`, `MPRTargetNProbeStatus`, `MPRTargetNProbeRetries`,
`MPRTargetNWriteCount` and `MPRTargetNFailureCount` properties.
Each target is probed through a non-mutating management chain and gets its own
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

Every published target is writable. The driver keeps no write allowlist and no
per-target write-protection state, so the nub forwards writes, cache
synchronisation and UNMAP to the controller without a local gate.
`IOBlockStorageDevice` declares `reportWriteProtection()` pure virtual, so the
nub still implements it and simply reports the media as not write-protected.
The nub records `MPRBlockDevHandle`; the controller records the per-target
properties above and its per-target write accounting.

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

A present target that has not been published yet is probed in place through the
read-only chain (TUR, INQUIRY, VPD, READ CAPACITY, MODE SENSE, READ). Every
pass is classified from the decoded SCSI status: `ok`, `not-ready` (sense 0x02,
UNIT ATTENTION, BUSY) or `failed` (illegal request, media or hardware error,
timeout, malformed or short reply). `validateScsiCompletion()` publishes its
decoded result into one word protected by `fIOLock`, so the chain needs no
allocation and no per-helper out-parameters; each stage starts with a clean
status, so an expected fallback such as RC16 -> RC10 behind a SATA bridge
cannot decide a later stage. `MPRTargetNProbeState` and
`MPRTargetNProbeStatus` report the last classification and its raw status.

A `not-ready` pass only asks the worker for another scan at its one-second
cooldown: it does not count as a failure and does not set `probeFailed`. One
per-target counter, `MPRTargetNProbeRetries` (summed in `MPRProbeRetries`),
bounds both a slow spin-up and a broken disk at 60 unsuccessful passes and
replaces the earlier separate failure/give-up limits. A definite failure
escalates to the bounded IOC reset after three passes, capped at three resets
per insertion; a device that never becomes ready is treated as failed when its
budget runs out and is then left offline. `prepareTargetMedia()` issues one read
per pass, so the same target never has two retry budgets, and no sleep is taken
under `fIOLock`. An empty fabric, including an expander with no eligible
terminal disk, is polled without an endless reset loop.

A config-page management timeout is a different condition: the IOC may still
own the timed-out request frame, so `finishManagementRequest()` quarantines it
and records a protocol fault that the worker recovers with one bounded IOC
reset. That recovery is not a probe failure and is deliberately retained.
During the wake settling window the config/event (SMIDs 1, 2 and 4) and
PortEnable (SMID 3) reply budgets are extended to 8 s and 60 s respectively,
so a slowly settling fabric does not create that fault in the first place.
SCSI probes keep their 2 s budget, so a device that never answers still
classifies as a definite failure.

Timeout recovery attempts ABORT TASK, LUN RESET and TARGET RESET, each naming
the handle recorded for the failed SMID, subject to available TMF credit and
terminated-command evidence, then a bounded IOC reset. Writes with uncertain
completion are not replayed. The legacy synchronous bulk helper retains its
one-time read retry for a whole batch that failed with UNIT ATTENTION after
DMA ended. The asynchronous scheduler returns decoded command errors; it does
not restart a partially completed multi-command request.

On this expander path a task-management request is not a local event. Every
injection whose TMF actually reached the target coincided with a SAS removal
report (`MPRTopoRemovalHits` advanced by one per injection, twelve times for
twelve), covering ABORT TASK, LUN RESET and TARGET RESET, so the IOC's fabric
report, not only the target's command queue, reacts to the reset. The driver
does what it is built to do with a removal: it invalidates the target, sets
`fHotplugPending` and re-runs discovery and media validation, and it records
`MPRRecoveryState=media-revalidation` for the interval. Twelve such events in a
row re-enumerated the target under a new BSD disk, and in that run raw requests
above 128 KiB failed with `ENXIO` while smaller ones were served until the next
boot; single-event runs did not reproduce that cap, so it is recorded without a
cause. Treat a TMF to an expander-attached SATA disk as a fabric event and
expect a re-enumeration, not a single failed request, with the medium
unavailable for tens of seconds.

An IOC-level rebuild does not disturb the fabric: with `mpr_tmf_fail=3` no TMF
is sent at all, `resetAndReinitialize()` runs (`MPRResetCount` +1,
`MPRResetInitiator=io-timeout`), `MPRTopoRemovalHits` stays unchanged, and the
target is re-published as a new BSD disk whose transfers and digests are
correct. On that path `MPRRecoveryState` is left reading `media-revalidation`
after the medium is serving again, because only the success branch rewrites it;
the property is a breadcrumb, not a live state.

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
published target through that target's own handle, disables reply delivery,
resets/quiesces the IOC and disables PCI bus mastering. Ownership is reset only
after DMA stop is proved.

Wake restores PCI access, rebuilds queues/events/topology, re-enumerates and
re-probes every present target, then reopens acceptance; each medium takes
OFFLINE -> ONLINE around that rebuild. `resumePower()` always performs that
rebuild. The PCI helper results and `fPowerQuiesced` are published as
`MPRWakeMemoryEnabled`, `MPRWakeBusMasterEnabled` and `MPRWakeQuiesced`, but
they never gate the rebuild: a quiesce that could not prove DMA stopped still
leaves an IOC that must be put back into a known state. On this machine
`setBusMasterEnable(true)` returns false after wake while the IOC is usable;
gating on it used to skip the rebuild, so the worker's first SMID 4 config read
timed out on an uninitialised IOC and forced a fault-driven reset. The outcome
of the planned rebuild is published in `MPRWakeRebuildState`.

`fWakeSettleTicks` (30 ticks) is opened before the rebuild and is only counted
down by the watchdog once the wake path has re-enabled acceptance, so it stays
open for a long discovery pass. `MPRWakeSettleTicks` reports it. While open,
config/event replies get 8 s and PortEnable gets 60 s; PortEnable has been
measured at 20-30 s on this expander, and the old 30 s ceiling turned a long
sleep into a failed rebuild plus a second fault-driven reset. The fabric also
needs a moment to settle, so the wake path waits 1.5 s after the IOC reset
before probing and ignores a non-operational doorbell for four watchdog ticks.
`fResumeGraceTicks` and that 1.5 s delay are retained.

A target that answers NOT READY at the wake probe is left to the hotplug
worker: `resumePower()` records `MPRWakeNotReadyCount` but does not invalidate
every medium or force an extra IOC reset on that account. The worker owns the
single retry budget and sets `MPRPowerState` to `on` when media recover, so a
wake that needed recovery does not keep reporting `wake-recovery-pending`.
One planned rebuild per wake is mandatory after S3, so `MPRResetCount` grows by
one; the control for "no extra reset" is that `MPRStickyFaultCount`,
`MPRFaultRecoveries` and `MPRProtocolRecoveryPending` stay unchanged.
`systemWillShutdown` follows the same quiesce path. Two sleep/wake cycles were
measured on the current source at a single target: each wake grew
`MPRResetCount` by exactly one with `MPRWakeRebuildState=ok` and
`MPRProbeRetries=0`, the medium returned as the same BSD disk, and the
read-only digests matched after each wake. Kernel power management timed the
driver's quiesce at about 1.16 s and its return at about 10.2 s. Shutdown and
the two-target PM path remain part of the earlier qualification baseline;
changed DMA ownership and PM failure paths still require their own
qualification.

Stop drains the I/O and completion workers outside the transport lock, proves
hardware DMA stop, then detaches any held client maps and completes them with
an error. Without that proof, active direct requests are not completed or freed. Scheduler/transport locks remain alive until final
service release so callers retained before stop can unwind safely.

## 7. Diagnostics

All diagnostic output is silent by default: neither kernel log lines nor any
IORegistry property below is produced unless `mpr_debug=1` is present in
boot-args. The flag is parsed from the earliest IOKit entry points
(`init`, `probe`, `start`), so bring-up logging is covered too. Every
`setProperty` / `removeProperty` call and every `IOLog` call goes through the
same gate (`MPR_DEBUG_PROP`, `MPR_DEBUG_REMOVE`, `MPR_DEBUG_LOG`). A property
that is absent on a normal boot is expected, not a failed load.

| Area | Useful IORegistry properties |
|---|---|
| Build and media | `MPRBuild`, `MPRFeatures`, `MPRMediaPresent`, `MPRMediaGeneration`, `MPRMediaReady`, `MPRMediaReadyStatus`, `MPRPublishedTargetCount` |
| Targets | `MPRTargetCount`, `MPRTargetNHandle`, `MPRTargetNSasAddress`, `MPRTargetNPresent`, `MPRTargetNProbed`, `MPRTargetNProbeState`, `MPRTargetNProbeStatus`, `MPRTargetNProbeRetries`, `MPRTargetNOnline`, `MPRTargetNRemovalState`, `MPRTargetNMediaStatus`, `MPRTargetNGeneration` |
| Discovery and removal | `MPRScanCount`, `MPRRemovedMediaCount`, `MPRRemovalFailures`, `MPRRemovalStage`, `MPRRetiredSlotsReleased`, `MPRRediscoveryResets`, `MPRRediscoveryState`, `MPRTopoRemovalHits`, `MPRTopologyGeneration` |
| Async limits | `MPRAsyncRequestLimit`, `MPRAsyncRequestedDepth`, `MPRAsyncAllocatedDepth`, `MPRAsyncCommandLimit`, `MPRAsyncFirstSmid`, `MPRAsyncPeakRequests`, `MPRAsyncOutstanding`, `MPRAsyncPeakCommands` |
| Client DMA | `MPRDirectDMAEnabled`, `MPRDirectCommands`, `MPRMappedCommands`, `MPRDirectBytes`, `MPRCopiedBytes`, `MPRChainedCommands`, `MPRBounceCommands`, `MPRDirectMapFailures`, `MPRAsyncHeldDMA` |
| Reply retirement | `MPRHardwareReplyQueues`, `MPRReplyBatches`, `MPRReplyDescriptors`, `MPRReplyMaxBatch`, `MPRReplyIndexWrites` |
| Identity/filter | `MPRStableIdentityValid`, `MPRStableIdentity`, `MPRIdentityState`, `MPRRaidFilterState`, `MPRStandaloneTargetCount`, `MPRSasDevicePageState` |
| Capabilities | `MPRNativeFuaSupported`, `MPRFuaPolicy`, `MPRUnmapEnabled`, `MPRUnmapDisabled`, `MPRUnmapMaxLbaCount`, `MPRUnmapMaxDescriptors`, `MPRUnmapOptimalGranularity`, `MPRUnmapAlignment`, `MPRUnmapAlignmentValid`, `MPRUnmapReadsZero` |
| Recovery | `MPRHotplugState`, `MPRProbeRetries`, `MPRResetCount`, `MPRResetInitiator`, `MPRReinitState`, `MPRRecoveryState`, `MPRFaultRecoveries`, `MPRFaultRecoveryState`, `MPRDeviceOffline`, `MPRTmfHandle`, `MPRTargetResetStatus` |
| Protocol | `MPRProtocolRecoveryPending`, `MPRProtocolFaultCode`, `MPRProtocolFaultSmid`, `MPRStickyFaultCode`, `MPRStickyFaultSmid`, `MPRStickyFaultCount`, `MPRBadReplySite`, `MPREventAckQueued`, `MPREventAckFailed` |
| Power | `MPRPowerState`, `MPRPowerFlushStatus`, `MPRPowerFlushCount`, `MPRStopDMAState`, `MPRWatchdogIOCState`, `MPRWakeNotReadyCount`, `MPRWakeQuiesced`, `MPRWakeMemoryEnabled`, `MPRWakeBusMasterEnabled`, `MPRWakeRebuildState`, `MPRWakeSettleTicks` |
| Writes | `MPRWriteCount`, `MPRWriteBytes`, `MPRLastWriteLba`, `MPRLastWriteSmid`, `MPRTargetNWriteCount`, `MPRTargetNFailureCount` |

`mpr_io_depth=N` requests 1-32 command slots; absent/non-positive means 32 and
larger values clamp to 32. `mpr_direct_dma` defaults to 1; zero forces fallback.
The runtime limit may be lower than requested. No live tuning property exists.

`MPRDirectCommands` and its compatibility alias `MPRMappedCommands` count posted
client-mapped commands, not old resident mappings. `MPRDirectBytes` counts their
submitted bytes, not verified successful bytes. `MPRCopiedBytes` counts explicit
driver copies to/from client memory, including copies before a later failure;
it excludes IOKit internal bounce copies. Forced fallback does not count as a
mapping failure. `MPRAsyncHeldDMA` counts direct commands retained after recovery
could not prove DMA safety; zero is required after a normal drained run.
Snapshots occur once per watchdog tick when the transport try-lock succeeds.
Reply counters are 32-bit and may wrap in very long runs. The hardware queue
count remains one. Use deltas and actual client results, not counters alone.

A property saying "online" is not data-integrity evidence. Always pair it with
the active IOMedia count, actual byte count and readback content.

`MPRIntCount` and `MPRReplyViaIrq` are written once, when `start()` returns, and
are not refreshed afterwards; `MPRReplyViaIrq` has no increment site at all in
this source. Neither of them is evidence about interrupt delivery during I/O.
The reply path's own progress evidence is `MPRReplyDescriptors` and
`MPRReplyBatches`; a working reply interrupt is not observable through a
property here.

`mpr_timeout_test=1`, `mpr_tmf_fail=N` and `mpr_timeout_every=N` exercise
recovery; none of these boot arguments is set by default. Serial kernel logging
remains off by default under the workspace rules.

## 8. Coverage against the FreeBSD driver

The comparison uses the fixed FreeBSD source in
`reference/freebsd/sys/dev/mpr` (commit in PLAN section 2) and covers the
controller mechanisms plus the disk-level equivalents this driver needs.
FreeBSD's CAM/newbus/bus_dma/sysctl interfaces are platform glue; their API
names are not macOS requirements.

| Mechanism | FreeBSD reference | Implementation |
|---|---|---|
| Request ownership and completion | `mpr_command`, `mpr_complete_command`, `mpr_intr_locked` | Per-SMID ownership states, checked replies, independent sense, quarantined timeouts and cancelled reset completions |
| Asynchronous dispatch | CAM submission and command callbacks | 64 request records, up to 32 credit-bounded independent windows, ready-first harvest/refill, separate completion context and fences |
| DMA, scatter/gather and chain | `mpr_map_command`, `mpr_add_chain`, `mpr_push_ieee_sge`, IOC Facts | Client-buffer DMA mappings, bounded IEEE chains and explicit private staging fallback |
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

Sleep and wake, physical pull and pulls with I/O in flight have earlier
qualification evidence with two targets behind this expander; the current source
re-ran sleep and wake only, so those two-target regressions are still owed. Scope
limits are in PLAN section 1.3. Hardware multi-queue/MSI-X scaling remains
unimplemented. Direct client DMA, ready-first refill and batched reply retirement
are qualified for read-only I/O, a small write/flush cycle and sleep/wake on one
target; the removal, UNMAP and dual-target paths that share their ownership rules
have not been re-run.

The batching/empty-drain policy also uses the local Linux
`mpt3sas_base.c::_base_process_reply_queue` as a behavior reference; the Linux
implementation is not copied into the kext. Its per-vector queues are not
simulated by multiple threads competing for this driver's one ring.

## 9. Host-side checks

`tools/test_protocol.cpp` contains golden CDB/VPD/sense tests and an independent
IEEE chain decoder with 2,000 deterministic SGL cases. The performance helper
coverage includes requested-depth clamping, insufficient credits/queue entries,
SMID separation and reset-time lower limits, signed wide-arithmetic resource
oracles, and all completion permutations for a partially failing four-chunk
request. The suite is built with ASan/UBSan at `-O1 -Wall -Wextra -Werror` on
the target system's compiler and passes 158,638 assertions over its 2,000
deterministic SGL cases. It does not simulate IOKit mapping, IRQ interleaving or
real DMA-stop evidence.

`tools/validate-build-inputs.py` checks LF text, plist/runtime/CLI/Xcode version
consistency and inclusion of all four driver translation units in the Xcode
project. It also checks host-test source line endings. Its successful result is
not a C++ compilation or a kernel ABI/link validation.

`tools/build-noxcode.sh` compiles the four translation units with the
MacKernelSDK headers, links them with `ld -kext`, checks the kmod entry points
and bundle type, rejects unresolved driver-internal symbols and signs the bundle
ad hoc. The resulting kext is what the test machine loads from the ESP. Every
symbol it leaves undefined was checked against the kernel collections on that
machine; `___chkstk_darwin` is the only one not listed there, and the same
reference was present in the previously qualified build. The Xcode project entry
point cannot be exercised on this machine because only the Command Line Tools
are installed, so that build path is unverified.
