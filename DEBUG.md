# DEBUG — implementation notes for MPR

Date: 2026-11-12. Driver version: 0.17.18.

State and capabilities are in [README.md](README.md); scope and backlog are in
[PLAN.md](PLAN.md). This file records how the driver is built and what its
documented failure modes were.

## 1. Concurrency model

- One `fIOLock` serializes block I/O, topology reads and reinitialization.
  A request queued before a medium change re-checks `fMediaGeneration` after it
  acquires the lock; a chunk loop in progress re-checks the removal latch
  before every chunk. Neither can submit across a medium replacement.
- `fReplyLock` (an `IOSimpleLock`) protects the reply slots and the topology
  and fault trail. Nothing that allocates — in particular no
  `setProperty()` — runs under it; those members are published from the
  watchdog instead.
- Management requests own one SMID each (2 events, 3 PORT_ENABLE, 4 config,
  6 TUR, 26–28 task management, 29 SYNCHRONIZE CACHE, 30 UNMAP, 31 event ACK).
  Runtime I/O uses SMIDs 10–25. A reply is delivered to its own SMID slot, not
  to a global "last reply".
- A waiting management request keeps consuming the reply queue while it waits
  (`finishManagementRequest` drains every 20 ms), because the reply it needs
  may land behind other completions.

## 2. Hotplug state machine

`fWatchdogTimer` runs once a second on the workloop. It publishes protocol
and topology diagnostics, decrements the cooldown, and enters
`fHotplugCall` when a topology event or a retry token is pending. The worker
(`hotplugThread`) runs on its own thread call with `fIOLock` held for the
duration, and releases it around any call that can re-enter the storage stack
(`takeMediaOffline`, `notifyMediaState`, `registerService`).

Decisions, in order:

1. `fStopping` or `fStarting` — exit.
2. Removal event, `fDeviceOffline`, protocol fault pending, or an unfinished
   offline notification — take the medium offline. `MPRMediaOfflineCause`
   records which of those four fired.
3. Read SAS IO Unit Page 0 and SAS Device Page 0.
   - Handle present and unchanged, medium published, not offline — online, no
     work.
   - Handle present but different, or medium not published — offline, then
     reinitialize.
   - No handle and no negotiated link on any phy — absence: poll, never reset.
   - No handle but a phy carries a link, or the device page denies the current
     handle (`fSasDeviceMismatch`, confirmed on two consecutive polls) — the
     IOC has not rediscovered this insertion: run the bounded reset below.
4. Bounded reinitialization: at most three diagnostic resets per insertion,
   after that only polling. A target that was absent in the previous snapshot
   gets a fresh budget.
5. After a successful reset, re-run discovery, identity, capacity, TUR and a
   block read, then publish (or re-notify) the medium while the lock is
   released.

A removal is latched (`fRemovalPending`) even if a fast replug reuses the same
handle, so the old medium is always taken down first. The latch is not the only
signal: a fast replug that never lets the link drop still shows up as a device
page identity mismatch, and that path also takes the medium offline before
recovery.

## 3. IORegistry diagnostics

Published by the controller: `MPRBuild`, `MPRIntState`, `MPREventState`,
`MPRHotplugState`, `MPRHotplugAttempts`, `MPRHotplugResetResult`,
`MPRHotplugHandle`, `MPRHotplugBlockDev`, `MPRReinitState`, `MPRResetCount`,
`MPRResetInitiator`, `MPRMediaPresent`, `MPRMediaReady`,
`MPRResidentBuffersReady`, `MPRMediaGeneration`, `MPRMediaBlocks`,
`MPRMediaBlockSize`, `MPRDeviceOffline`, `MPRMediaOfflineStatus`,
`MPRMediaOnlineStatus`, `MPRMediaOfflineCause`, `MPRTopologyGeneration`,
`MPRTopologyState`, `MPRLinkUpPhys`, `MPRIdentityLostCount`,
`MPREventAckQueued`/`InFlight`/`Completed`/`Failed`/`Overflow`,
`MPRProtocolRecoveryPending`, `MPRProtocolFaultCode`, `MPRProtocolFaultSmid`,
`MPRStickyFaultCode`/`Smid`/`Count`, `MPRBadReplySite`/`Value`/`Smid`/`Count`/
`Function`/`MsgLen`/`Word`, `MPRLastTopoHandle`/`Reason`/`Rate`,
`MPRTopoRemovalHits`, `MPRPhyNDevHandle`, `MPRPhyNLinkRate`, and the block
I/O counters `MPRBlockIOCount`, `MPRBlockIODoneBytes`, `MPRBlockIOFailure`,
`MPRBlockIOFailureLba`/`Smid`, `MPRBlockIORetryCount`, `MPRBlockRejectReason`.

Write accounting: `MPRWriteCount`, `MPRWriteBytes`, `MPRLastWriteLba`,
`MPRLastWriteSmid`. These count completed 64 KiB staging chunks, so a write is
visible only after its data reached the device. They record writes from any
source, which is the point: while the medium is published writable, macOS
mounts the volume and writes to it, and those writes must be attributable.
A bounded verification on unallocated disk space (see section 9) showed the
counters matching the issued traffic exactly — 34 writes / 2,098,176 bytes for
the test operations plus 128 writes / 8,388,608 bytes for the final window
restore — with `MPRLastWriteLba` equal to the start LBA of the last chunk.

Property values are diagnostics, never acceptance criteria: a state string of
`online` is compatible with a medium the storage stack has already dropped, and
a medium that is present but not readable is not "recovered". Acceptance is a
raw read that returns the expected byte count with the expected content.

Sampling note: a full `ioreg -l -w0` costs about 2.5 s on the target machine,
which is far too coarse for a fast unplug. `ioreg -c MPRController -r -l -w0`
costs ~0.03 s and is the right query for a hotplug trace.

## 4. Fault trails and what they identified

Faults are recorded with a code and the SMID that caused them
(`kMPRProtocolAckOverflow` 1, `AckReply` 2, `ManagementTimeout` 3,
`EventLength` 4, `ManagementBusy` 5, `ManagementReply` 6, `AckTimeout` 7,
`InterruptUnavailable` 8). Because a reinitialization clears the live fault
state, `MPRStickyFault*` keeps the last fault across the reset, and
`MPRBadReplySite` names the validation that rejected a completion
(1 reply descriptor flags, 2 SCSI reply frame, 3 PORT_ENABLE frame,
4 EVENT_NOTIFICATION frame, 5 config page frame).

The documented failure modes this trail resolved:

- **PORT_ENABLE wait too short.** The firmware answers PORT_ENABLE about
  7.6 s (`MPRLongReplyWaitMs=7594`, `Smid=3`) after a diagnostic reset on this
  adapter, and the reference implementation allows 300 s for the same command.
  A 2 s budget failed every rebuild even though the transport was healthy. The
  wait is now 30 s and keeps draining the reply queue while it waits.
- **SCSI reply length compared against the wrong size.** `MPRScsiIoReply` is
  0x34 bytes because of the MPI 2.5 EEDP tail, but this firmware answers with
  the classic 0x30-byte SCSI IO reply. `MsgLength * 4 < sizeof(...)` therefore
  rejected every *error* reply (48 < 52) and raised a protocol fault, which
  tore the medium down and reset the IOC. `MPRBadReplySite=2`,
  `MsgLen=12`, `Function=0` is that signature. The check now covers only the
  0x20-byte prefix the driver actually reads
  (`MPR_SCSIIO_REPLY_MIN_BYTES`). A SCSI error reply is an I/O error, not a
  transport failure.
- **No rediscovery trigger.** After a replug the IOC can leave a phy with a
  negotiated link and no attached target, and it emits no further topology
  event. Treating that snapshot as "disk absent" polled forever. It now takes
  the bounded reset path, which is the only rediscovery this prototype owns.
- **Device page denial spun forever.** A fast replug can leave SAS Device
  Page 0 answering with a different handle or a non-zero access status. That
  was counted as a topology read failure and retried indefinitely, so the
  driver never converged and the old medium was never taken offline. It is now
  recognised as an identity change and routed to the same bounded recovery.

## 5. Fault injection

Two boot-args exist only to exercise the recovery chain without touching the
disk. Both are one-shot and inert unless set.

- `mpr_timeout_test=1` fails a single SCSI command (SMID 10–25) as if its reply
  were lost. It only fires after bring-up: while `fStarting` a timeout
  escalates straight to the bounded reset by design, so allowing the injection
  there would consume it without exercising anything. Consumed injection leaves
  `MPRInjectTimeout=0` and `MPRLastIOFailure=reply-timeout`.
- `mpr_tmf_fail=N` makes the first N task-management levels look unsuccessful,
  so recovery really submits the next level: N=1 skips `ABORT_TASK` and sends
  `LUN_RESET`, N=2 skips both and sends `TARGET_RESET`, N=3 skips all three and
  falls through to `resetAndReinitialize`. `MPRTmfInjectedFail` records N, and
  `MPRTmfTaskType` records what was actually sent (1 `ABORT_TASK`, 5
  `LUN_RESET`, 3 `TARGET_RESET`). It needs `mpr_timeout_test=1` to have
  something to recover from.

Observed with 0.17.19: N absent → `ABORT_TASK` succeeded with
`TerminationCount=1`, `MPRRecoveryState=ok`, no IOC reset; N=1 → `LUN_RESET`
ok, no reset; N=2 → `TARGET_RESET` ok, no reset; N=3 → `MPRResetInitiator`
`io-timeout`, one further reset from the worker, media republished with its
partitions and readable. A timeout that lands during bring-up instead records
`kMPRProtocolManagementTimeout` and lets the worker run its bounded reset; that
path also ends with the medium readable.

## 6. Choosing an immutability reference

The first 64 MiB of this disk is **live exFAT metadata**, not a stable
reference: the root directory, allocation bitmap and upcase table sit at
partition offsets 8–13 MiB, and macOS rewrites parts of that region when it
mounts or repairs the volume. Per-boot comparisons showed changes confined to
those clusters plus one data region, while the main and backup boot regions
stayed byte-identical, `VolumeFlags` stayed `0x0000`, `fsck_exfat -n` reported
the volume OK, and repeated reads inside one boot were identical. Use a
data-area range — skip 1024 MiB, 64 MiB — as the immutability anchor; that
range has been byte-stable across every round, and unmount the volume before
hashing.

## 7. Buffer lifetime

`fPageBuffer` and `fDataBuffer` are resident for the lifetime of the
controller object and are never freed by a probe. A probe that allocates a
temporary page only releases it when no protocol recovery is pending; a frame
the firmware may still own is never recycled on the strength of a timeout
alone. Reinitialization replaces a small temporary page only after a successful
IOC reset. `fResidentBuffersReady` records that the resident pair exists, and
block I/O refuses to start without it.

## 8. Verification method

Per cycle: capture identity and driver state, start a `ioreg -c MPRController`
sampler at 5 Hz, perform the physical action, and afterwards compare
`MPRMediaGeneration`, `MPRMediaOfflineStatus`, `MPRMediaOnlineStatus`,
`MPRResetCount`, handle and topology generation. Then confirm exactly one
active whole-disk `IOMedia` with its partition child, and read 64 MiB from the
raw device under an external 60 s deadline, recording exit status, stderr,
byte count and SHA-256, plus a fixed-offset 64 MiB reference range that must
hash identically before and after the cycle. In-flight cycles run continuous
256 MiB reads so the unplug lands inside one of them; that read must terminate
with an error rather than a timeout hang.

Write verification uses a region with no filesystem of its own — partition 1,
the Microsoft Reserved area — so no file data is at risk, and it runs as
save → write → readback → restore → readback: the restored bytes must hash
identically to the saved copy, and the region's fingerprint must match before
and after. Sizes 512 B, 4 KiB, 64 KiB, 1 MiB, 4 MiB and 8 MiB all round-trip
byte-exactly; a write at the capacity boundary is rejected with `EIO` and the
region is unchanged. Open the raw node once with `O_RDWR | O_EXCL` for the
whole test: a mounted volume makes every later write-open fail with `EBUSY`,
which is the operating system protecting a filesystem it believes it owns.

Offsets in `dd` are in `bs` units, so `bs=1m seek=34` is byte 34 MiB, not LBA
34. Use `oflag=seek_bytes`/`iflag=skip_bytes`, or byte offsets through
`pread`/`pwrite`, whenever the target is an LBA.
