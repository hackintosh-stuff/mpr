# DEBUG — implementation notes for MPR

Date: 2026-11-13. Driver version: 0.18.1.

This document describes source behavior and diagnostics. It does not claim
that the current source has passed runtime acceptance. See [PORTING.md](PORTING.md)
for the comparison and validation boundary.

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
The latter reaches a terminal disk behind an expander backplane. It does not
create a multi-target storage stack. Multiple eligible disks are refused.

On IR-capable firmware, RAID Configuration Page 0 is enumerated to exclude
volumes, member disks, hot spares and expansion members. Configuration
transport failures or malformed pages fail closed. IR change events retire
the old eligibility decision.

SATA identity prefers ATA IDENTIFY model and serial; VPD 0x83/0x80 are
fallbacks. SAS address and DevHandle are topology properties, not persistent
identity. Recovery rechecks the stable identity, block count and block size.
A physical insertion publishes a new media generation after retiring the
previous medium; a transport recovery cannot silently bind old I/O to a
different disk.

## 4. Hotplug and recovery

The watchdog monitors IOC state, event acknowledgement and topology work.
A worker retires the old IOMedia through its actual storage client, then
discovers, probes and publishes the new medium with no transport lock held
around reentrant notifications.

A real TARGET_NOT_RESPONDING event can initiate
TARGET_RESET -> command drain -> SAS IO Unit Control REMOVE_DEVICE.
The event handle and generation are preserved. A live/reused handle, a new
add event or renewed PHY activity cancels the old removal decision.
Uncertain state falls back to bounded controller recovery.

A usable independent target is probed directly before attempting an IOC
reset. Full resets remain capped at three per insertion. An empty fabric,
including an expander with no eligible terminal disk, is polled without
an endless reset loop.

Timeout recovery attempts ABORT TASK, LUN RESET and TARGET RESET subject to
available TMF credit and terminated-command evidence, then a bounded IOC
reset. Writes with uncertain completion are not replayed. The legacy
synchronous path may retry a zero-transfer UNIT ATTENTION read once.

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
is On. Sleep closes asynchronous acceptance, fences accepted I/O, flushes
the disk when usable, disables reply delivery, resets/quiesces the IOC and
disables PCI bus mastering. Ownership is reset only after DMA stop is proved.

Wake restores PCI access, rebuilds queues/events/topology, revalidates the
disk, then reopens acceptance. A failed wake retires media and requests
bounded recovery. `systemWillShutdown` follows the same quiesce path.
Sleep/wake and shutdown behavior in this version remains unqualified on
hardware; source hooks and compilation are not runtime evidence.

Stop waits for the I/O and completion workers outside the transport lock,
then tears down DMA. Scheduler/transport locks remain alive until final
service release so callers retained before stop can unwind safely.

## 7. Diagnostics

| Area | Useful IORegistry properties |
|---|---|
| Build and media | `MPRBuild`, `MPRFeatures`, `MPRMediaPresent`, `MPRMediaGeneration`, `MPRMediaReady` |
| Async and DMA | `MPRAsyncRequestLimit`, `MPRAsyncCommandLimit`, `MPRAsyncPeakRequests`, `MPRMappedCommands`, `MPRChainedCommands`, `MPRBounceCommands` |
| Identity/filter | `MPRStableIdentityValid`, `MPRStableIdentity`, `MPRIdentityState`, `MPRRaidFilterState`, `MPRStandaloneTargetCount` |
| Capabilities | `MPRNativeFuaSupported`, `MPRFuaPolicy`, `MPRUnmapEnabled`, `MPRUnmapMaxLbaCount`, `MPRUnmapMaxDescriptors`, `MPRUnmapReadsZero` |
| Recovery | `MPRHotplugState`, `MPRTargetRemovalState`, `MPRResetCount`, `MPRReinitState`, `MPRRecoveryState`, `MPRDeviceOffline` |
| Protocol | `MPRProtocolRecoveryPending`, `MPRProtocolFaultCode`, `MPRStickyFaultCount`, `MPRBadReplySite`, `MPREventAckQueued`, `MPREventAckFailed` |
| Power | `MPRPowerState`, `MPRPowerFlushStatus`, `MPRStopDMAState`, `MPRWatchdogIOCState` |
| Writes | `MPRWriteCount`, `MPRWriteBytes`, `MPRLastWriteLba`, `MPRLastWriteSmid` |

A property saying "online" is not data-integrity evidence. Always pair it with
the active IOMedia count, actual byte count and readback content.

Existing opt-in boot arguments `mpr_timeout_test`, `mpr_tmf_fail` and
`mpr_timeout_every` exercise recovery. They are not enabled by this patch.
Serial kernel logging remains off by default under the workspace rules.
