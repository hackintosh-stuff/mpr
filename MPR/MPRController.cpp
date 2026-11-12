#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/OSByteOrder.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "MPRController.h"
#include "MPRBlockDev.h"
#include "MPI/mpi2_min.h"
#include "MPI/mpi2_target.h"
#include "MPRCompletion.h"

#define super IOService
OSDefineMetaClassAndStructors(MPRController, IOService)

#define MPR_VENDOR_DEVICE 0x00971000U

/* First memory BAR on this card is BAR1 (config register 0x14), 64 KB MMIO.
 * The physical address is assigned at boot and differs between macOS and
 * Linux, so always map by register offset and never hardcode an address. */
#define MPR_MMIO_BAR_REG 0x14

/* Hard bounds for every hardware wait. start() runs in the boot path, so
 * bring-up must finish quickly instead of using the 300 s timeouts of
 * the FreeBSD driver. */
#define MPR_DIAG_UNLOCK_TRIES   20
#define MPR_DIAG_SETTLE_TRIES   100
#define MPR_READY_TRIES         60
#define MPR_ACK_ITERATIONS      4000
#define MPR_DBINT_ITERATIONS    4000

/* Reply free/post queue geometry. The queues must hold one unused entry
 * and be a multiple of 16 (mpr.c:1379-1380); IOC Init advertises the same
 * depths and the request frame size in 32-bit words (mpr.c:1264-1266). */
#define MPR_REPLY_FRAMES    128
#define MPR_REQUEST_FRAMES  128
#define MPR_IOC_STATE_TRIES     1000
#define MPR_DMA_LOW_MASK        0x00000000FFFFFFFFULL

/* Config-page reads poll the reply queue instead of taking interrupts. */
#define MPR_CONFIG_TRIES           2000
#define MPR_CONFIG_PAGE_BYTES      128

/* A reply wait uses the interrupt first, then falls back to one poll pass. */
#define MPR_REPLY_TIMEOUT_MS 2000

/* PORT_ENABLE restarts firmware discovery and is answered only once that
 * discovery finishes, so it scales with the topology behind the HBA instead
 * of with a register handshake. mpt3sas allows 300 s for the same command
 * after a host reset; the 2 s management budget is only valid for config
 * pages and IOC Init. */
#define MPR_PORT_ENABLE_TIMEOUT_MS 30000

/* topology config page staging buffer (SAS IO Unit Page0 with 8+ phys). */
#define MPR_PAGE_BUF_BYTES      4096

/* Each SMID owns its own sense DMA area, including concurrently active IO. */
#define MPR_SENSE_BYTES         96U
#define MPR_SENSE_POOL_BYTES    (MPR_REQUEST_FRAMES * MPR_SENSE_BYTES)

/* bulk transfer staging buffers: one window per in-flight command so a single
 * request can keep several chunk commands outstanding instead of paying the
 * per-command round trip serially. fDataBytes stays the window size. */
#define MPR_DATA_BUF_BYTES      65536
#define MPR_IO_SLOTS            4
#define MPR_DATA_POOL_BYTES     (MPR_DATA_BUF_BYTES * MPR_IO_SLOTS)
#define MPR_SM_EVENT_ACK        31

enum {
    kMPRProtocolAckOverflow = 1, kMPRProtocolAckReply,
    kMPRProtocolManagementTimeout, kMPRProtocolEventLength,
    kMPRProtocolManagementBusy, kMPRProtocolManagementReply,
    kMPRProtocolAckTimeout,
    kMPRProtocolInterruptUnavailable, kMPRProtocolUnexpectedCompletion,
    kMPRProtocolIocNotOperational
};

/* Two registered PM states drive setPowerState(): drain/flush/quiesce before
 * power removal, then rebuild and revalidate before accepting requests. */
enum {
    kMPRPowerOff = 0,
    kMPRPowerOn
};
static const unsigned long kMPRPowerStateCount = 2;

static IOPMPowerState gMPRPowerStates[kMPRPowerStateCount] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable,
      kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

struct MPRRegisterInfo
{
    const char *name;
    UInt32      offset;
};

static const MPRRegisterInfo kMPRRegisters[] = {
    { "Doorbell",              MPI2_DOORBELL_OFFSET },
    { "WriteSequence",         MPI2_WRITE_SEQUENCE_OFFSET },
    { "HostDiagnostic",        MPI2_HOST_DIAGNOSTIC_OFFSET },
    { "DiagRWData",            MPI2_DIAG_RW_DATA_OFFSET },
    { "DiagRWAddressLow",      MPI2_DIAG_RW_ADDRESS_LOW_OFFSET },
    { "DiagRWAddressHigh",     MPI2_DIAG_RW_ADDRESS_HIGH_OFFSET },
    { "HostInterruptStatus",   MPI2_HOST_INTERRUPT_STATUS_OFFSET },
    { "HostInterruptMask",     MPI2_HOST_INTERRUPT_MASK_OFFSET },
    { "DCRData",               MPI2_DCR_DATA_OFFSET },
    { "DCRAddress",            MPI2_DCR_ADDRESS_OFFSET },
    { "ReplyFreeHostIndex",    MPI2_REPLY_FREE_HOST_INDEX_OFFSET },
    { "ReplyPostHostIndex",    MPI2_REPLY_POST_HOST_INDEX_OFFSET },
    { "HCBSize",               MPI2_HCB_SIZE_OFFSET },
    { "HCBAddressLow",         MPI2_HCB_ADDRESS_LOW_OFFSET },
    { "HCBAddressHigh",        MPI2_HCB_ADDRESS_HIGH_OFFSET },
    { "Scratchpad0",           MPI2_SCRATCHPAD0_OFFSET },
    { "Scratchpad1",           MPI2_SCRATCHPAD1_OFFSET },
    { "Scratchpad2",           MPI2_SCRATCHPAD2_OFFSET },
    { "Scratchpad3",           MPI2_SCRATCHPAD3_OFFSET },
    { "RequestDescriptorPostLow",  MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET },
    { "RequestDescriptorPostHigh", MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET },
    { "AtomicRequestDescriptorPost", MPI2_ATOMIC_REQUEST_DESCRIPTOR_OFFSET },
};

UInt32
MPRController::readReg(UInt32 offset)
{
    return fRegs[offset / sizeof(UInt32)];
}

void
MPRController::writeReg(UInt32 offset, UInt32 value)
{
    fRegs[offset / sizeof(UInt32)] = value;
}

void
MPRController::postRequestDescriptor(UInt32 low, UInt32 high)
{
    if (fPostLock == NULL || fRegs == NULL) {
        return;
    }
    /* Reply processing can submit an ACK while a normal command submits.
     * Keep the two MMIO halves indivisible. Lock order: reply -> post only. */
    IOSimpleLockLock(fPostLock);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, low);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, high);
    IOSimpleLockUnlock(fPostLock);
}

UInt64
MPRController::senseAddress(UInt32 smid) const
{
    if (fSenseBuffer == NULL || smid == 0 || smid >= fRequestCount ||
        smid >= MPR_REQUEST_FRAMES) return 0;
    return fSensePhys + (UInt64)smid * MPR_SENSE_BYTES;
}

bool
MPRController::prepareScsiSense(UInt32 smid)
{
    if (fReplyLock == NULL || fSenseBuffer == NULL || senseAddress(smid) == 0)
        return false;
    UInt8 *sense = (UInt8 *)fSenseBuffer->getBytesNoCopy();
    if (sense == NULL) return false;
    IOSimpleLockLock(fReplyLock);
    MPRCommandOwner *owner = &fCommandOwners[smid];
    bool ok = !fStopping && !fProtocolRecoveryPending &&
        (owner->state == kMPRCommandFree || owner->state == kMPRCommandReserved);
    if (ok) {
        owner->state = kMPRCommandReserved;
        owner->function = MPI2_FUNCTION_SCSI_IO_REQUEST;
        owner->handle = (UInt16)fAttachedDevHandle;
        owner->epoch = fTransportEpoch;
        fReplySlots[smid].valid = 0;
        memset(sense + (UInt64)smid * MPR_SENSE_BYTES, 0, MPR_SENSE_BYTES);
    }
    IOSimpleLockUnlock(fReplyLock);
    return ok;
}

bool
MPRController::commandDmaSafe(UInt32 smid)
{
    if (fReplyLock == NULL || smid == 0 || smid >= MPR_REQUEST_FRAMES)
        return false;
    IOSimpleLockLock(fReplyLock);
    UInt8 state = fCommandOwners[smid].state;
    bool safe = state != kMPRCommandPosted && state != kMPRCommandQuarantined;
    IOSimpleLockUnlock(fReplyLock);
    return safe;
}

void
MPRController::cancelUnpostedCommand(UInt32 smid)
{
    if (fReplyLock == NULL || smid == 0 || smid >= MPR_REQUEST_FRAMES) return;
    IOSimpleLockLock(fReplyLock);
    if (fCommandOwners[smid].state == kMPRCommandReserved) {
        fCommandOwners[smid].state = kMPRCommandFree;
        if (smid < 10) fManagementBusy[smid] = false;
    }
    IOSimpleLockUnlock(fReplyLock);
}

bool
MPRController::postOwnedRequestLocked(UInt32 smid, UInt32 flags, UInt32 high)
{
    if (smid == 0 || smid >= fRequestCount || smid >= MPR_REQUEST_FRAMES ||
        fRequestBuffer == NULL || fStopping || fProtocolRecoveryPending ||
        fCommandOwners[smid].state != kMPRCommandReserved) return false;
    UInt8 *base = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (base == NULL) return false;
    UInt8 *frame = base + (UInt64)smid * fRequestFrameSize;
    MPRCommandOwner *owner = &fCommandOwners[smid];
    owner->function = frame[3];
    owner->handle = 0;
    if (frame[3] == MPI2_FUNCTION_SCSI_IO_REQUEST ||
        frame[3] == MPI2_FUNCTION_SCSI_TASK_MGMT ||
        frame[3] == MPI2_FUNCTION_SATA_PASSTHROUGH)
        owner->handle = (UInt16)((UInt16)frame[0] | ((UInt16)frame[1] << 8));
    else if (frame[3] == MPI2_FUNCTION_SAS_IO_UNIT_CONTROL)
        owner->handle = (UInt16)((UInt16)frame[4] | ((UInt16)frame[5] << 8));
    owner->epoch = fTransportEpoch;
    owner->state = kMPRCommandPosted;
    fReplySlots[smid].valid = 0;
    postRequestDescriptor(flags | (smid << 16), high);
    return true;
}

bool
MPRController::storeCommandReplyLocked(UInt32 smid, UInt8 kind,
                                      const UInt8 *reply, UInt32 bytes)
{
    if (smid == 0 || smid >= fRequestCount || smid >= MPR_REQUEST_FRAMES) {
        fUnexpectedCompletionCount++;
        recordProtocolFaultLocked(kMPRProtocolUnexpectedCompletion, smid);
        return false;
    }
    MPRCommandOwner *owner = &fCommandOwners[smid];
    if ((owner->state != kMPRCommandPosted &&
         owner->state != kMPRCommandQuarantined) ||
        owner->epoch != fTransportEpoch || fReplySlots[smid].valid != 0) {
        /* Never overwrite an unconsumed completion, even when the SMID is
         * numerically valid. A software epoch alone cannot tag IOC replies. */
        fUnexpectedCompletionCount++;
        recordProtocolFaultLocked(kMPRProtocolUnexpectedCompletion, smid);
        return false;
    }
    bool valid = kind == 2 ? owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST
        : kind == 1 && reply != NULL && bytes >= 4 &&
          reply[3] == owner->function;
    if (valid && kind == 1 &&
        (owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST ||
         owner->function == MPI2_FUNCTION_SCSI_TASK_MGMT)) {
        const UInt32 minimum = owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST
            ? MPR_SCSIIO_REPLY_MIN_BYTES : sizeof(MPRScsiTaskMgmtReply);
        UInt16 handle = (UInt16)((UInt16)reply[0] | ((UInt16)reply[1] << 8));
        valid = bytes >= minimum && handle == owner->handle;
        if (valid && owner->function == MPI2_FUNCTION_SCSI_TASK_MGMT) {
            UInt8 *request = (UInt8 *)fRequestBuffer->getBytesNoCopy() +
                (UInt64)smid * fRequestFrameSize;
            valid = reply[5] == request[5];
        }
    }
    if (valid && kind == 1 && owner->function == MPI2_FUNCTION_SATA_PASSTHROUGH) {
        valid = bytes >= sizeof(MPRSataPassthroughReply) &&
            (UInt16)((UInt16)reply[0] | ((UInt16)reply[1] << 8)) == owner->handle;
    }
    if (valid && kind == 1 && owner->function == MPI2_FUNCTION_SAS_IO_UNIT_CONTROL) {
        valid = bytes >= sizeof(MPRSasIoUnitControlReply) &&
            (UInt16)((UInt16)reply[4] | ((UInt16)reply[5] << 8)) == owner->handle;
        if (valid) {
            UInt8 *request = (UInt8 *)fRequestBuffer->getBytesNoCopy() +
                (UInt64)smid * fRequestFrameSize;
            valid = reply[0] == request[0];
        }
    }
    if (!valid) {
        fUnexpectedCompletionCount++;
        recordProtocolFaultLocked(kMPRProtocolUnexpectedCompletion, smid);
        return false;
    }
    MPRReplySlot *slot = &fReplySlots[smid];
    memset(slot->reply, 0, sizeof(slot->reply));
    if (kind == 1)
        memcpy(slot->reply, reply, bytes < sizeof(slot->reply) ? bytes : sizeof(slot->reply));
    slot->smid = (UInt16)smid;
    slot->kind = kind;
    owner->state = kMPRCommandComplete;
    slot->valid = 1;
    return true;
}

void
MPRController::recordProtocolFaultLocked(UInt32 code, UInt32 smid)
{
    if (!fProtocolRecoveryPending) {
        fProtocolFaultCode = code;
        fProtocolFaultSmid = smid;
        fTopologyGeneration++;
    }
    /* Diagnostic trail: the first fault of a recovery round, kept across the
     * resetProtocolState() that the following reinit performs. */
    fStickyFaultCode = code;
    fStickyFaultSmid = smid;
    fStickyFaultCount++;
    fProtocolRecoveryPending = true;
    fHotplugPending = true;
}

void
MPRController::recordProtocolFault(UInt32 code, UInt32 smid)
{
    if (fReplyLock == NULL) {
        return;
    }
    IOSimpleLockLock(fReplyLock);
    recordProtocolFaultLocked(code, smid);
    IOSimpleLockUnlock(fReplyLock);
}

void
MPRController::noteBadReply(UInt32 site, UInt32 value, UInt32 smid,
                            const UInt8 *frame, UInt32 bytes)
{
    /* Allocation-free: several callers hold fReplyLock. */
    fBadReplySite = site;
    fBadReplyValue = value;
    fBadReplySmid = smid;
    fBadReplyCount++;
    fBadReplyFunction = (frame != NULL && bytes > 3) ? frame[3] : 0xFF;
    fBadReplyMsgLen = (frame != NULL && bytes > 2) ? frame[2] : 0xFF;
    fBadReplyWord = 0;
    if (frame != NULL && bytes >= 4) {
        memcpy(&fBadReplyWord, frame, sizeof(UInt32));
    }
}

bool
MPRController::beginManagementRequest(UInt32 smid)
{
    bool ready = false;

    if (smid == 0 || smid >= 10 || fReplyLock == NULL ||
        fRequestBuffer == NULL || fStopping) {
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    if (!fProtocolRecoveryPending) {
        if (fManagementBusy[smid] ||
            fCommandOwners[smid].state != kMPRCommandFree) {
            recordProtocolFaultLocked(kMPRProtocolManagementBusy, smid);
        } else {
            fManagementBusy[smid] = true;
            fCommandOwners[smid].state = kMPRCommandReserved;
            fCommandOwners[smid].epoch = fTransportEpoch;
            fReplySlots[smid].valid = 0;
            ready = true;
        }
    }
    IOSimpleLockUnlock(fReplyLock);
    return ready;
}

bool
MPRController::finishManagementRequest(UInt32 smid, UInt32 timeoutMs,
                                        UInt8 *reply, UInt8 *kind)
{
    if (smid == 0 || smid >= 10 || fReplyLock == NULL) return false;
    if (!waitForSmid(timeoutMs, smid) ||
        !takeReplySlot(smid, reply, 64, kind)) {
        /* The IOC may still own this frame AND the shared DMA buffer.
         * Keep the busy bit until a diagnostic reset has stopped old DMA. */
        IOSimpleLockLock(fReplyLock);
        if (fCommandOwners[smid].state == kMPRCommandPosted)
            fCommandOwners[smid].state = kMPRCommandQuarantined;
        recordProtocolFaultLocked(kMPRProtocolManagementTimeout, smid);
        IOSimpleLockUnlock(fReplyLock);
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    fManagementBusy[smid] = false;
    bool healthy = !fProtocolRecoveryPending;
    IOSimpleLockUnlock(fReplyLock);
    return healthy;
}

void
MPRController::resetProtocolState(void)
{
    /* Caller has stopped the old IOC's DMA and disabled reply processing.
     * Never use this to reclaim a timed-out frame while the IOC is live. */
    if (fReplyLock == NULL) {
        return;
    }
    IOSimpleLockLock(fReplyLock);
    memset(fManagementBusy, 0, sizeof(fManagementBusy));
    memset(fTmfBusy, 0, sizeof(fTmfBusy));
    ++fTransportEpoch;
    for (UInt32 smid = 0; smid < MPR_REQUEST_FRAMES; ++smid) {
        bool runtime = (smid >= 10 && smid <= 25) || smid == 29 || smid == 30;
        bool occupied = fCommandOwners[smid].state != kMPRCommandFree;
        memset(&fReplySlots[smid], 0, sizeof(fReplySlots[smid]));
        if (runtime && occupied) {
            /* The caller has proved old DMA stopped. Preserve one synthetic
             * cancellation for each old waiter instead of waiting for replies
             * that this reset necessarily discarded. */
            fCommandOwners[smid].state = kMPRCommandCancelled;
            fReplySlots[smid].smid = (UInt16)smid;
            fReplySlots[smid].kind = 3;
            fReplySlots[smid].valid = 1;
        } else {
            fCommandOwners[smid].state = kMPRCommandFree;
        }
    }
    fEventAckHead = fEventAckQueued = fEventAckAge = 0;
    fEventAckBusy = false;
    fProtocolRecoveryPending = false;
    fProtocolFaultCode = fProtocolFaultSmid = 0;
    fTargetRemovalRequested = false;
    fRemovedTargetHandle = 0;
    IOSimpleLockUnlock(fReplyLock);
}

void
MPRController::serviceProtocolState(void)
{
    UInt32 queued, completed, failed, overflow, fault, smid, generation;
    bool busy, recovery;

    if (fReplyLock == NULL) {
        return;
    }
    IOSimpleLockLock(fReplyLock);
    if (!fReinitInProgress && !fStopping && fEventAckBusy &&
        !fProtocolRecoveryPending && ++fEventAckAge >= 5) {
        fEventAckFailed++;
        recordProtocolFaultLocked(kMPRProtocolAckTimeout, MPR_SM_EVENT_ACK);
    }
    queued = fEventAckQueued;
    completed = fEventAckCompleted;
    failed = fEventAckFailed;
    overflow = fEventAckOverflow;
    busy = fEventAckBusy;
    recovery = fProtocolRecoveryPending;
    fault = fProtocolFaultCode;
    smid = fProtocolFaultSmid;
    generation = fTopologyGeneration;
    IOSimpleLockUnlock(fReplyLock);
    setProperty("MPREventAckQueued", (unsigned long long)queued, 32);
    setProperty("MPREventAckInFlight", busy);
    setProperty("MPREventAckCompleted", (unsigned long long)completed, 32);
    setProperty("MPREventAckFailed", (unsigned long long)failed, 32);
    setProperty("MPREventAckOverflow", (unsigned long long)overflow, 32);
    setProperty("MPRProtocolRecoveryPending", recovery);
    setProperty("MPRProtocolFaultCode", (unsigned long long)fault, 32);
    setProperty("MPRProtocolFaultSmid", (unsigned long long)smid, 16);
    setProperty("MPRTopologyGeneration", (unsigned long long)generation, 32);
    setProperty("MPRUnexpectedCompletionCount",
                (unsigned long long)fUnexpectedCompletionCount, 32);
    /* The members below are written under the reply lock, so they are
     * published here instead of allocating inside that lock. */
    setProperty("MPRStickyFaultCode", (unsigned long long)fStickyFaultCode, 32);
    setProperty("MPRStickyFaultSmid", (unsigned long long)fStickyFaultSmid, 16);
    setProperty("MPRStickyFaultCount", (unsigned long long)fStickyFaultCount, 32);
    setProperty("MPRLastTopoHandle", (unsigned long long)fLastTopoHandle, 16);
    setProperty("MPRLastTopoReason", (unsigned long long)fLastTopoReason, 8);
    setProperty("MPRLastTopoRate", (unsigned long long)fLastTopoRate, 8);
    setProperty("MPRTopoRemovalHits", (unsigned long long)fTopoRemovalHits, 32);
    setProperty("MPRBadReplySite", (unsigned long long)fBadReplySite, 32);
    setProperty("MPRBadReplyValue", (unsigned long long)fBadReplyValue, 32);
    setProperty("MPRBadReplySmid", (unsigned long long)fBadReplySmid, 16);
    setProperty("MPRBadReplyCount", (unsigned long long)fBadReplyCount, 32);
    setProperty("MPRBadReplyFunction", (unsigned long long)fBadReplyFunction, 8);
    setProperty("MPRBadReplyMsgLen", (unsigned long long)fBadReplyMsgLen, 8);
    setProperty("MPRBadReplyWord", (unsigned long long)fBadReplyWord, 32);
    setProperty("MPRWriteCount", (unsigned long long)fWriteCount, 32);
    setProperty("MPRWriteBytes", (unsigned long long)fWriteBytes, 64);
    setProperty("MPRLastWriteLba", (unsigned long long)fLastWriteLba, 64);
    setProperty("MPRLastWriteSmid", (unsigned long long)fLastWriteSmid, 16);
}

void
MPRController::recordTopologyEventLocked(const UInt8 *data, UInt32 bytes)
{
    /* Payload is borrowed from the reply frame and parsed before recycling.
     * The diagnostics-only fLastEventData is too short for eight PHYs. */
    if (bytes < 12 || data[8] > (bytes - 12) / 4) {
        recordProtocolFaultLocked(kMPRProtocolEventLength, 0);
        return;
    }
    bool changed = false;
    for (UInt32 i = 0; i < data[8]; i++) {
        const UInt8 *phy = data + 12 + i * 4;
        UInt16 handle = (UInt16)((UInt16)phy[0] | ((UInt16)phy[1] << 8));
        UInt8 reason = phy[3] & MPI2_EVENT_SAS_TOPO_RC_MASK;
        UInt8 rate = phy[2] >> 4;
        if (handle == 0 ||
            ((phy[3] & MPI2_EVENT_SAS_TOPO_PHYSTATUS_VACANT) != 0 &&
             reason != MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING)) {
            continue;
        }
        bool affected = (handle == fAttachedDevHandle && fAttachedDevHandle != 0);
        /* A change on the phy that carries our target means the link was
         * renegotiated, which only happens after it dropped. Treating that as
         * removal is what makes a replug shorter than one poll interval still
         * retire the old medium: the firmware may report only PHY_CHANGED and
         * never TARGET_NOT_RESPONDING, and the reattached target can keep the
         * same handle with an identical device page. */
        bool removed = reason == MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING ||
                       reason == MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED;
        /* Diagnostics: the last reason that carried a target handle decides
         * which transition the driver reacts to. Plain members only; the
         * watchdog publishes them outside this lock. */
        fLastTopoHandle = handle;
        fLastTopoReason = reason;
        fLastTopoRate = rate;
        if (affected && removed) {
            /* Preserve removal even if a fast replug reuses the same handle. */
            fTopoRemovalHits++;
            fRemovalPending = true;
            fPhysicalChangePending = true;
        }
        if (affected && reason == MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING) {
            fRemovedTargetHandle = handle;
            fRemovalGeneration = fTopologyGeneration + 1;
            fTargetRemovalRequested = true;
        } else if (handle == fRemovedTargetHandle &&
                   (reason == MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED ||
                    (reason == MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED && rate >= 8))) {
            fTargetRemovalRequested = false;
        }
        if (reason == MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED &&
            !fReinitInProgress && !fStarting) {
            /* A new insertion may retry an exhausted recovery budget;
             * discovery generated by our own IOC reset must not do so. */
            fHotplugRetryRequested = true;
            fPhysicalChangePending = true;
        }
        if (reason == MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED ||
            reason == MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING ||
            reason == MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED ||
            reason == MPI2_EVENT_SAS_TOPO_RC_DELAY_NOT_RESPONDING) {
            changed = true;
        }
    }
    if (changed) {
        fTopologyGeneration++;
        fTopologyChanged = true;
        /* Cooldown delays work; it must never discard a physical event. */
        fHotplugPending = true;
    }
}

IOService *
MPRController::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci;
    UInt32       vendorDevice;

    if (super::probe(provider, score) == NULL) {
        return NULL;
    }

    pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == NULL) {
        return NULL;
    }

    vendorDevice = pci->configRead32(0);
    if (vendorDevice != MPR_VENDOR_DEVICE) {
        IOLog("MPR: probe rejected vendor/device 0x%08x\n",
              (unsigned int)vendorDevice);
        return NULL;
    }

    IOLog("MPR: probe matched SAS3008 (0x%08x)\n",
          (unsigned int)vendorDevice);
    return this;
}

void
MPRController::dumpRegisters(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(kMPRRegisters) / sizeof(kMPRRegisters[0]); i++) {
        UInt32 value = readReg(kMPRRegisters[i].offset);
        setProperty(kMPRRegisters[i].name, (unsigned long long)value, 32);
    }

    IOLog("MPR MMIO dump: Doorbell=0x%08x HostDiag=0x%08x HIS=0x%08x HIM=0x%08x\n",
          (unsigned int)readReg(MPI2_DOORBELL_OFFSET),
          (unsigned int)readReg(MPI2_HOST_DIAGNOSTIC_OFFSET),
          (unsigned int)readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET),
          (unsigned int)readReg(MPI2_HOST_INTERRUPT_MASK_OFFSET));
}

bool
MPRController::waitDoorbellAck(UInt32 iterations)
{
    UInt32 i;

    for (i = 0; i < iterations; i++) {
        UInt32 status = readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET);
        if (!(status & MPI2_HIS_SYS2IOC_DB_STATUS)) {
            return true;
        }
        if (status == 0xFFFFFFFFU) {
            return false;
        }
        if ((status & MPI2_HIS_IOC2SYS_DB_STATUS) &&
            ((readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK) ==
             MPI2_IOC_STATE_FAULT)) {
            return false;
        }
        IODelay(100);
    }
    return false;
}

bool
MPRController::waitDoorbellInt(UInt32 iterations)
{
    UInt32 i;

    for (i = 0; i < iterations; i++) {
        if (readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET) &
            MPI2_HIS_IOC2SYS_DB_STATUS) {
            return true;
        }
        IODelay(100);
    }
    return false;
}

bool
MPRController::diagReset(void)
{
    static const UInt8 magic[7] = {
        MPI2_WRSEQ_FLUSH_KEY_VALUE, MPI2_WRSEQ_1ST_KEY_VALUE,
        MPI2_WRSEQ_2ND_KEY_VALUE,   MPI2_WRSEQ_3RD_KEY_VALUE,
        MPI2_WRSEQ_4TH_KEY_VALUE,   MPI2_WRSEQ_5TH_KEY_VALUE,
        MPI2_WRSEQ_6TH_KEY_VALUE
    };
    UInt32 reg;
    unsigned int i, tries;
    bool unlocked = false;

    /* Keep interrupts masked while the IOC is being reset; the reset path polls only. */
    writeReg(MPI2_HOST_INTERRUPT_MASK_OFFSET,
             MPI2_HIM_REPLY_INT_MASK | MPI2_HIM_IOC2SYS_DB_MASK);
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);

    for (tries = 0; tries < MPR_DIAG_UNLOCK_TRIES && !unlocked; tries++) {
        for (i = 0; i < sizeof(magic); i++) {
            writeReg(MPI2_WRITE_SEQUENCE_OFFSET, magic[i]);
        }
        IOSleep(100);
        if (readReg(MPI2_HOST_DIAGNOSTIC_OFFSET) &
            MPI2_DIAG_DIAG_WRITE_ENABLE) {
            unlocked = true;
        }
    }
    if (!unlocked) {
        IOLog("MPR diag write-enable never set\n");
        return false;
    }

    reg = readReg(MPI2_HOST_DIAGNOSTIC_OFFSET);
    writeReg(MPI2_HOST_DIAGNOSTIC_OFFSET, reg | MPI2_DIAG_RESET_ADAPTER);

    IOSleep(256);
    for (tries = 0; tries < MPR_DIAG_SETTLE_TRIES; tries++) {
        reg = readReg(MPI2_HOST_DIAGNOSTIC_OFFSET);
        if (reg & MPI2_DIAG_RESET_ADAPTER) {
            IOSleep(50);
            continue;
        }
        if ((readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK) !=
            MPI2_IOC_STATE_RESET) {
            writeReg(MPI2_WRITE_SEQUENCE_OFFSET,
                     MPI2_WRSEQ_FLUSH_KEY_VALUE);
            return true;
        }
        IOSleep(50);
    }

    IOLog("MPR diag reset did not leave RESET state\n");
    return false;
}

bool
MPRController::transitionToReady(void)
{
    unsigned int tries;

    for (tries = 0; tries < MPR_READY_TRIES; tries++) {
        UInt32 doorbell = readReg(MPI2_DOORBELL_OFFSET);
        UInt32 state = doorbell & MPI2_IOC_STATE_MASK;
        UInt32 whoInit = (doorbell & MPI2_DOORBELL_WHO_INIT_MASK) >>
                         MPI2_DOORBELL_WHO_INIT_SHIFT;

        if (whoInit == MPI2_WHOINIT_PCI_PEER) {
            IOLog("MPR another PCI peer owns the IOC, refusing\n");
            return false;
        }

        if (state == MPI2_IOC_STATE_READY) {
            return true;
        }

        if (state == MPI2_IOC_STATE_OPERATIONAL) {
            /* Take ownership back without a full diagnostic reset. */
            writeReg(MPI2_DOORBELL_OFFSET,
                     MPI2_FUNCTION_IOC_MESSAGE_UNIT_RESET <<
                     MPI2_DOORBELL_FUNCTION_SHIFT);
            if (!waitDoorbellAck(MPR_ACK_ITERATIONS)) {
                return false;
            }
            IOSleep(50);
            continue;
        }

        if ((state == MPI2_IOC_STATE_FAULT) ||
            (doorbell & MPI2_DOORBELL_USED)) {
            if (!diagReset()) {
                return false;
            }
            continue;
        }

        /* RESET or an unknown state: let the firmware settle. */
        IOSleep(50);
    }

    IOLog("MPR IOC never reached READY\n");
    return false;
}

/* Synchronous doorbell message transfer (mpr.c:994-1123). The reply is
 * clocked in as 16-bit words; every word is followed by clearing HIS. */
bool
MPRController::handshake(const UInt32 *reqWords, UInt32 requestBytes,
                         UInt16 *repWords, UInt32 replyBytes)
{
    UInt32 reqCount = (requestBytes + 3) / 4;
    UInt32 maxWords = replyBytes / 2;
    UInt32 i, count, residual, iocWords;

    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    if (readReg(MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_USED) {
        return false;
    }

    writeReg(MPI2_DOORBELL_OFFSET,
             (MPI2_FUNCTION_HANDSHAKE << MPI2_DOORBELL_FUNCTION_SHIFT) |
             (reqCount << MPI2_DOORBELL_ADD_DWORDS_SHIFT));

    if (!waitDoorbellInt(MPR_DBINT_ITERATIONS) ||
        !(readReg(MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_USED)) {
        return false;
    }
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    if (!waitDoorbellAck(MPR_ACK_ITERATIONS)) {
        return false;
    }

    for (i = 0; i < reqCount; i++) {
        writeReg(MPI2_DOORBELL_OFFSET, reqWords[i]);
        if (!waitDoorbellAck(MPR_ACK_ITERATIONS)) {
            return false;
        }
    }

    if (!waitDoorbellInt(MPR_DBINT_ITERATIONS)) {
        return false;
    }
    repWords[0] = (UInt16)(readReg(MPI2_DOORBELL_OFFSET) &
                           MPI2_DOORBELL_DATA_MASK);
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);

    if (!waitDoorbellInt(MPR_DBINT_ITERATIONS)) {
        return false;
    }
    repWords[1] = (UInt16)(readReg(MPI2_DOORBELL_OFFSET) &
                           MPI2_DOORBELL_DATA_MASK);
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);

    /* MsgLength lives in byte 2 of the reply and counts 32-bit words. */
    iocWords = ((UInt8 *)repWords)[MPI2_DEFAULT_REPLY_MSG_LENGTH_OFF];
    if (iocWords == 0) {
        return false;
    }
    count = iocWords * 2;
    if (count > maxWords) {
        residual = count - maxWords;
        count = maxWords;
    } else {
        residual = 0;
    }

    for (i = 2; i < count; i++) {
        if (!waitDoorbellInt(MPR_DBINT_ITERATIONS)) {
            return false;
        }
        repWords[i] = (UInt16)(readReg(MPI2_DOORBELL_OFFSET) &
                               MPI2_DOORBELL_DATA_MASK);
        writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    }

    while (residual--) {
        if (!waitDoorbellInt(MPR_DBINT_ITERATIONS)) {
            return false;
        }
        (void)readReg(MPI2_DOORBELL_OFFSET);
        writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    }

    if (!waitDoorbellInt(MPR_DBINT_ITERATIONS)) {
        return false;
    }
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    return true;
}

bool
MPRController::getIOCFacts(void)
{
    union {
        MPRIOCFactsRequest req;
        UInt32             words[sizeof(MPRIOCFactsRequest) / 4];
    } request;
    union {
        MPRIOCFactsReply reply;
        UInt16           words[sizeof(MPRIOCFactsReply) / 2];
    } answer;
    MPRIOCFactsReply   facts;
    unsigned int       retry;
    bool               ok = false;

    bzero(&request, sizeof(request));
    request.req.Function = MPI2_FUNCTION_IOC_FACTS;

    for (retry = 0; retry < 3 && !ok; retry++) {
        bzero(&answer, sizeof(answer));
        ok = handshake(request.words, sizeof(request.words),
                       answer.words, sizeof(answer.words));
        if (!ok) {
            IODelay(1000);
        }
    }
    if (!ok) {
        setProperty("MPRControllerInitFailure", "IOC Facts handshake failed");
        return false;
    }

    /* The doorbell reply is little-endian 16-bit words; x86_64 needs no swap,
     * but copy through the union so the packed structure stays aligned. */
    memcpy(&facts, answer.words, sizeof(facts));

    setProperty("MPRFactsMsgVersion",        (unsigned long long)facts.MsgVersion, 16);
    setProperty("MPRFactsMsgLength",         (unsigned long long)facts.MsgLength, 8);
    setProperty("MPRFactsHeaderVersion",     (unsigned long long)facts.HeaderVersion, 16);
    setProperty("MPRFactsIOCStatus",         (unsigned long long)facts.IOCStatus, 16);
    setProperty("MPRFactsIOCLogInfo",        (unsigned long long)facts.IOCLogInfo, 32);
    setProperty("MPRFactsWhoInit",           (unsigned long long)facts.WhoInit, 8);
    setProperty("MPRFactsMaxChainDepth",     (unsigned long long)facts.MaxChainDepth, 8);
    setProperty("MPRFactsMaxMSIxVectors",    (unsigned long long)facts.MaxMSIxVectors, 8);
    setProperty("MPRFactsNumberOfPorts",     (unsigned long long)facts.NumberOfPorts, 8);
    setProperty("MPRFactsRequestCredit",     (unsigned long long)facts.RequestCredit, 16);
    setProperty("MPRFactsProductID",         (unsigned long long)facts.ProductID, 16);
    setProperty("MPRFactsIOCCapabilities",   (unsigned long long)facts.IOCCapabilities, 32);
    setProperty("MPRFactsFWVersion",         (unsigned long long)facts.FWVersion, 32);
    setProperty("MPRFactsRequestFrameSize",  (unsigned long long)facts.IOCRequestFrameSize, 16);
    setProperty("MPRFactsChainSegmentSize",  (unsigned long long)facts.IOCMaxChainSegmentSize, 16);
    setProperty("MPRFactsMaxReplyPostDepth", (unsigned long long)facts.MaxReplyDescriptorPostQueueDepth, 16);
    setProperty("MPRFactsReplyFrameSize",    (unsigned long long)facts.ReplyFrameSize, 8);
    setProperty("MPRFactsMaxDevHandle",      (unsigned long long)facts.MaxDevHandle, 16);
    setProperty("MPRFactsCurrentHostPageSize", (unsigned long long)facts.CurrentHostPageSize, 8);
    setProperty("MPRFactsProtocolFlags",     (unsigned long long)facts.ProtocolFlags, 16);
    setProperty("MPRFactsHighPriorityCredit",(unsigned long long)facts.HighPriorityCredit, 16);

    setProperty("MPRFactsFunction", (unsigned long long)facts.Function, 8);
    setProperty("MPRFactsStatus",
                (unsigned long long)(facts.IOCStatus & MPI2_IOCSTATUS_MASK), 16);

    IOLog("MPR IOC Facts: status=0x%04x FW=0x%08x credit=%u replyDepth=%u frameSize=%u\n",
          (unsigned int)(facts.IOCStatus & MPI2_IOCSTATUS_MASK),
          (unsigned int)facts.FWVersion,
          (unsigned int)facts.RequestCredit,
          (unsigned int)facts.MaxReplyDescriptorPostQueueDepth,
          (unsigned int)facts.IOCRequestFrameSize);

    ok = (facts.Function == MPI2_FUNCTION_IOC_FACTS) &&
         ((facts.IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS);
    if (ok) {
        /* Keep the geometry for queue initialization; both frame sizes are in 32-bit words. */
        fRequestFrameSize = (UInt32)facts.IOCRequestFrameSize * 4;
        fReplyFrameSize   = (UInt32)facts.ReplyFrameSize * 4;
        fRequestCredit      = facts.RequestCredit;
        fMaxReplyDepth      = facts.MaxReplyDescriptorPostQueueDepth;
        fHighPriorityCredit = facts.HighPriorityCredit;
        fMaxChainDepth      = facts.MaxChainDepth;
        /* MPI 2.5 defines this value in 16-byte IEEE elements; zero means
         * the legacy eight-element default used by the FreeBSD driver. */
        fChainFrameBytes    = (facts.IOCMaxChainSegmentSize ?
                              facts.IOCMaxChainSegmentSize : 8U) * 16U;
        if (fChainFrameBytes > 1024) fChainFrameBytes = 1024;
        fIocCapabilities    = facts.IOCCapabilities;
        fMaxDeviceHandle    = facts.MaxDevHandle;
        fFactsValid         = true;
    }
    return ok;
}

bool
MPRController::initializeController(void)
{
    UInt32 initialState, finalState;

    dumpRegisters();
    initialState = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    setProperty("MPRInitialIOCState", (unsigned long long)initialState, 32);

    if (!transitionToReady()) {
        setProperty("MPRControllerInitFailure", "cannot reach READY");
        setProperty("MPRControllerInitState", "failed-ready");
        return false;
    }
    setProperty("MPRReachedReady", true);

    if (!getIOCFacts()) {
        setProperty("MPRControllerInitState", "failed-facts");
        return false;
    }

    finalState = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    setProperty("MPRFinalIOCState", (unsigned long long)finalState, 32);
    setProperty("MPRControllerInitState", "ok");
    IOLog("MPR completed, IOC state after IOC Facts = 0x%08x\n",
          (unsigned int)finalState);
    return true;
}

/* ------------------------------------------------------------------------ */
/* synchronous IOC Init with a low-4GB, physically contiguous pool.    */
/* No interrupts, no config pages and no I/O: the only hardware effects are  */
/* the IOC Init message and the two reply-index registers (mpr.c:1231-1289). */

static UInt32
mprRoundUp16(UInt32 value)
{
    return (value + 15U) & ~15U;
}

bool
MPRController::allocateQueueMemory(void)
{
    IOByteCount segLength;
    UInt32      queueBytes, replyBytes, requestBytes;
    UInt8      *queue;
    UInt32     *freeQueue;
    UInt64     *postQueue;
    UInt32      i;

    if (!fFactsValid || fReplyFrameSize < MPR_SCSIIO_REPLY_MIN_BYTES ||
        fRequestFrameSize < sizeof(MPRScsiIoRequest) + 16) {
        setProperty("MPRQueueInitFailure", "no usable IOC Facts geometry");
        return false;
    }

    fFqDepth        = mprRoundUp16(MPR_REPLY_FRAMES + 1);
    if (fFqDepth > fMaxReplyDepth) fFqDepth = fMaxReplyDepth & ~15U;
    fPqDepth        = fFqDepth;
    fRequestCount   = fRequestCredit < MPR_REQUEST_FRAMES
        ? fRequestCredit : MPR_REQUEST_FRAMES;
    if (fRequestCount <= MPR_SM_EVENT_ACK || fFqDepth < 16) {
        setProperty("MPRQueueInitFailure", "insufficient management and IO credits");
        return false;
    }
    fReplyFreeIndex = fFqDepth - 1 < MPR_REPLY_FRAMES
        ? fFqDepth - 1 : MPR_REPLY_FRAMES;

    queueBytes   = fFqDepth * 4 + fPqDepth * 8;
    replyBytes   = fReplyFrameSize * fFqDepth;
    requestBytes = fRequestFrameSize * fRequestCount;

    if (fRequestCount > fRequestCredit || fFqDepth > fMaxReplyDepth) {
        setProperty("MPRQueueInitFailure", "geometry exceeds IOC credit");
        return false;
    }

    fQueueBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        queueBytes, MPR_DMA_LOW_MASK);
    fReplyBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        replyBytes, MPR_DMA_LOW_MASK);
    fRequestBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        requestBytes, MPR_DMA_LOW_MASK);
    fSenseBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_SENSE_POOL_BYTES, MPR_DMA_LOW_MASK);

    if (fQueueBuffer == NULL || fReplyBuffer == NULL || fRequestBuffer == NULL ||
        fSenseBuffer == NULL) {
        setProperty("MPRQueueInitFailure", "DMA pool allocation failed");
        return false;
    }

    fFreePhys = fQueueBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fFreePhys == 0 || segLength < queueBytes) {
        setProperty("MPRQueueInitFailure", "queue pool is not physically contiguous");
        return false;
    }
    fPostPhys = fFreePhys + (UInt64)fFqDepth * 4;

    fReplyPhys = fReplyBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fReplyPhys == 0 || segLength < replyBytes) {
        setProperty("MPRQueueInitFailure", "reply pool is not physically contiguous");
        return false;
    }

    fRequestPhys = fRequestBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fRequestPhys == 0 || segLength < requestBytes) {
        setProperty("MPRQueueInitFailure", "request pool is not physically contiguous");
        return false;
    }
    fSensePhys = fSenseBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fSensePhys == 0 || segLength < MPR_SENSE_POOL_BYTES ||
        fSensePhys > MPR_DMA_LOW_MASK ||
        MPR_SENSE_POOL_BYTES - 1 > MPR_DMA_LOW_MASK - fSensePhys ||
        fSenseBuffer->getBytesNoCopy() == NULL) {
        setProperty("MPRQueueInitFailure", "sense pool violates DMA address bounds");
        return false;
    }
    memset(fSenseBuffer->getBytesNoCopy(), 0, MPR_SENSE_POOL_BYTES);

    if ((fFreePhys + queueBytes) > (MPR_DMA_LOW_MASK + 1) ||
        (fReplyPhys + replyBytes) > (MPR_DMA_LOW_MASK + 1) ||
        (fRequestPhys + requestBytes) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRQueueInitFailure", "DMA pool crosses the 4 GB boundary");
        return false;
    }

    queue = (UInt8 *)fQueueBuffer->getBytesNoCopy();
    if (queue == NULL) {
        setProperty("MPRQueueInitFailure", "queue pool has no virtual address");
        return false;
    }

    memset(queue, 0, queueBytes);
    if (fReplyBuffer->getBytesNoCopy() != NULL) {
        memset(fReplyBuffer->getBytesNoCopy(), 0, replyBytes);
    }
    if (fRequestBuffer->getBytesNoCopy() != NULL) {
        memset(fRequestBuffer->getBytesNoCopy(), 0, requestBytes);
    }
    freeQueue = (UInt32 *)queue;
    postQueue = (UInt64 *)(queue + fFqDepth * 4);

    /* Unused post entries are 0xff; each free entry carries the low 32 bits
     * of its reply frame address (mpr.c:1691-1706). */
    memset(postQueue, 0xFF, fPqDepth * 8);
    for (i = 0; i < fFqDepth; i++) {
        freeQueue[i] = (UInt32)(fReplyPhys + (UInt64)i * fReplyFrameSize);
    }

    setProperty("MPRQueueFreeDepth", (unsigned long long)fFqDepth, 32);
    setProperty("MPRQueuePostDepth", (unsigned long long)fPqDepth, 32);
    setProperty("MPRRequestCount",   (unsigned long long)fRequestCount, 32);
    setProperty("MPRQueuePhys",      fFreePhys, 64);
    setProperty("MPRReplyPhys",      fReplyPhys, 64);
    setProperty("MPRRequestPhys",    fRequestPhys, 64);
    return true;
}

void
MPRController::freeQueueMemory(void)
{
    if (fSenseBuffer != NULL) {
        fSenseBuffer->release();
        fSenseBuffer = NULL;
    }
    fSensePhys = 0;
    if (fQueueBuffer != NULL) {
        fQueueBuffer->release();
        fQueueBuffer = NULL;
    }
    if (fReplyBuffer != NULL) {
        fReplyBuffer->release();
        fReplyBuffer = NULL;
    }
    if (fRequestBuffer != NULL) {
        fRequestBuffer->release();
        fRequestBuffer = NULL;
    }
    fFreePhys = fPostPhys = fReplyPhys = fRequestPhys = 0;
}

bool
MPRController::sendIOCInit(void)
{
    union {
        MPRIOCInitRequest request;
        UInt32             words[sizeof(MPRIOCInitRequest) / 4];
    } out;
    union {
        MPRIOCInitReply reply;
        UInt16          words[sizeof(MPRIOCInitReply) / 2];
    } in;
    MPRIOCInitReply answer;
    UInt16          status;
    unsigned int    retry;
    bool            ok = false;

    bzero(&out, sizeof(out));
    out.request.WhoInit                         = MPI2_WHOINIT_HOST_DRIVER;
    out.request.Function                        = MPI2_FUNCTION_IOC_INIT;
    out.request.MsgVersion                      = MPI2_VERSION;
    out.request.HeaderVersion                   = MPI2_HEADER_VERSION;
    out.request.SystemRequestFrameSize          = (UInt16)(fRequestFrameSize / 4);
    out.request.ReplyDescriptorPostQueueDepth   = (UInt16)fPqDepth;
    out.request.ReplyFreeQueueDepth             = (UInt16)fFqDepth;
    out.request.SenseBufferAddressHigh          = (UInt32)(fSensePhys >> 32);
    out.request.SystemReplyAddressHigh          = 0;
    out.request.SystemRequestFrameBaseAddress   = fRequestPhys;
    out.request.ReplyDescriptorPostQueueAddress = fPostPhys;
    out.request.ReplyFreeQueueAddress           = fFreePhys;
    out.request.HostPageSize                    = MPR_HOST_PAGE_SIZE_4K;
    out.request.HostMSIxVectors                 = (fInterrupt != NULL) ? 1 : 0;
    out.request.TimeStamp                       = 0;

    for (retry = 0; retry < 3 && !ok; retry++) {
        bzero(&in, sizeof(in));
        ok = handshake(out.words, sizeof(out.words),
                       in.words, sizeof(in.words));
        if (!ok) {
            IODelay(1000);
        }
    }
    if (!ok) {
        setProperty("MPRQueueInitFailure", "IOC Init handshake failed");
        return false;
    }

    memcpy(&answer, in.words, sizeof(answer));
    status = (UInt16)(answer.IOCStatus & MPI2_IOCSTATUS_MASK);
    setProperty("MPRIOCInitStatus",  (unsigned long long)status, 16);
    setProperty("MPRIOCInitLogInfo", (unsigned long long)answer.IOCLogInfo, 32);
    IOLog("MPR IOC Init status=0x%04x loginfo=0x%08x\n",
          (unsigned int)status, (unsigned int)answer.IOCLogInfo);

    return status == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPRController::initializeQueues(void)
{
    UInt32 state = MPI2_IOC_STATE_RESET;
    UInt32 tries;

    if (!allocateQueueMemory()) {
        setProperty("MPRQueueInitState", "failed-alloc");
        return false;
    }
    if (!sendIOCInit()) {
        setProperty("MPRQueueInitState", "failed-iocinit");
        return false;
    }

    state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    for (tries = 0; tries < MPR_IOC_STATE_TRIES &&
                    state != MPI2_IOC_STATE_OPERATIONAL; tries++) {
        IODelay(1000);
        state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    }
    setProperty("MPRIOCState", (unsigned long long)state, 32);

    if (state != MPI2_IOC_STATE_OPERATIONAL) {
        setProperty("MPRQueueInitState", "failed-not-operational");
        return false;
    }

    /* The IOC resets these indices during the state transition, so program
     * them only after OPERATIONAL is observed (mpr.c:700-712). */
    writeReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET, fReplyFreeIndex);
    writeReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET, 0);
    setProperty("MPRReplyFreeHostIndex", (unsigned long long)fReplyFreeIndex, 32);
    setProperty("MPRReplyPostHostIndex", 0ULL, 32);

    /* Unmask only the reply-descriptor interrupt; every other cause stays
     * masked so a stuck status bit cannot become an interrupt storm. */
    if (fInterrupt != NULL) {
        writeReg(MPI2_HOST_INTERRUPT_MASK_OFFSET, ~MPI2_HIM_REPLY_INT_MASK);
    }
    setProperty("MPRQueueInitState", "ok");
    IOLog("MPR IOC OPERATIONAL, freeDepth=%u postDepth=%u requests=%u\n",
          (unsigned int)fFqDepth, (unsigned int)fPqDepth,
          (unsigned int)fRequestCount);
    return true;
}

/* ------------------------------------------------------------------------ */
/* one synchronous config request (Manufacturing Page0) submitted      */
/* through the request queue and completed by polling the reply queue.       */

static void
mprCopyField(char *dst, const UInt8 *src, UInt32 length)
{
    UInt32 i = length;

    memcpy(dst, src, length);
    dst[length] = '\0';
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\0')) {
        dst[--i] = '\0';
    }
}

/* ------------------------------------------------------------------------ */
/* reply-descriptor interrupt delivered through a filtered event        */
/* source. The filter only samples and clears the cause; the reply queue is   */
/* drained on the work loop (PLAN section 5, rule 4).                        */

bool
MPRController::setupInterrupts(void)
{
    int type = -1;

    if (fWorkLoop == NULL) {
        fWorkLoop = IOWorkLoop::workLoop();
    }
    if (fWorkLoop == NULL) {
        setProperty("MPRIntState", "no-workloop");
        return false;
    }

    if (fPCIDevice->getInterruptType(0, &type) == kIOReturnSuccess) {
        setProperty("MPRIntType", (unsigned long long)type, 32);
    } else {
        setProperty("MPRIntType", "unavailable");
    }

    fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(
        this, interruptAction, interruptFilter, fPCIDevice, 0);
    if (fInterrupt == NULL) {
        setProperty("MPRIntState", "no-source");
        return false;
    }
    if (fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
        setProperty("MPRIntState", "add-failed");
        fInterrupt->release();
        fInterrupt = NULL;
        return false;
    }
    fInterrupt->enable();
    setProperty("MPRIntState", "ok");
    return true;
}

void
MPRController::teardownInterrupts(void)
{
    if (fInterrupt != NULL) {
        fInterrupt->disable();
        if (fWorkLoop != NULL) {
            fWorkLoop->removeEventSource(fInterrupt);
        }
        fInterrupt->release();
        fInterrupt = NULL;
    }
    /* The watchdog shares this workloop. Keep it alive across IOC resets. */
}

bool
MPRController::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender)
{
    MPRController *self = (MPRController *)owner;
    UInt32 status;

    if (self == NULL || self->fRegs == NULL) {
        return false;
    }
    status = self->readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET);
    if ((status & MPI2_HIS_REPLY_DESCRIPTOR_INTERRUPT) == 0) {
        return false;
    }
    /* HostInterruptStatus is write-one-to-clear. */
    self->writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET,
                   MPI2_HIS_REPLY_DESCRIPTOR_INTERRUPT);
    self->fInterruptCount++;
    return true;
}

void
MPRController::interruptAction(OSObject *owner, IOInterruptEventSource *sender,
                               int index)
{
    MPRController *self = (MPRController *)owner;

    if (self != NULL) {
        self->processReplyQueue();
    }
}

void
MPRController::processReplyQueue(void)
{
    if (fQueueBuffer == NULL || fReplyBuffer == NULL || fReplyLock == NULL) {
        return;
    }
    UInt8 *queue = (UInt8 *)fQueueBuffer->getBytesNoCopy();
    UInt8 *replyBase = (UInt8 *)fReplyBuffer->getBytesNoCopy();
    if (queue == NULL || replyBase == NULL || fReplyFrameSize == 0) {
        return;
    }
    UInt8 *post = queue + fFqDepth * 4;
    UInt32 replyBytes = fReplyFrameSize * fFqDepth;

    /* One consumer owns descriptor retirement, reply slots, and event ACKs. */
    IOSimpleLockLock(fReplyLock);
    for (UInt32 i = 0; i < fPqDepth; i++) {
        UInt64 entry;
        memcpy(&entry, post + fReplyPostIndex * 8, sizeof(entry));
        UInt8 flags = (UInt8)(entry & MPI2_RPY_DESCRIPT_FLAGS_TYPE_MASK);
        if (flags == MPI2_RPY_DESCRIPT_FLAGS_UNUSED ||
            ((entry >> 32) & 0xFFFFFFFFULL) == 0xFFFFFFFFULL) {
            break;
        }
        UInt16 smid = (UInt16)((entry >> 16) & 0xFFFF);
        UInt32 baddr = (UInt32)(entry >> 32);
        UInt64 clear = 0xFFFFFFFFFFFFFFFFULL;
        memcpy(post + fReplyPostIndex * 8, &clear, sizeof(clear));
        fReplyPostIndex = (fReplyPostIndex + 1) % fPqDepth;
        if (flags == MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY &&
            baddr >= (UInt32)fReplyPhys &&
            baddr - (UInt32)fReplyPhys < replyBytes &&
            ((baddr - (UInt32)fReplyPhys) % fReplyFrameSize) == 0) {
            UInt8 *reply = replyBase + (baddr - (UInt32)fReplyPhys);
            UInt32 bytes = (UInt32)reply[2] * 4;
            if (bytes < 4 || bytes > fReplyFrameSize) {
                recordProtocolFaultLocked(kMPRProtocolManagementReply, smid);
            } else if (smid == MPR_SM_EVENT_ACK) {
                /* ACK completion never satisfies a config/SCSI waiter. */
                completeEventAckLocked(reply, bytes);
            } else if (reply[3] == MPI2_FUNCTION_EVENT_NOTIFICATION) {
                if (bytes < sizeof(MPREventNotifyReply)) {
                    recordProtocolFaultLocked(kMPRProtocolEventLength, smid);
                } else {
                    const MPREventNotifyReply *event =
                        (const MPREventNotifyReply *)reply;
                    UInt32 eventBytes = (UInt32)event->EventDataLength * 4;
                    memset(fLastEventData, 0, sizeof(fLastEventData));
                    memcpy(fLastEventData, reply,
                           bytes < sizeof(fLastEventData) ? bytes : sizeof(fLastEventData));
                    fLastEvent = event->Event;
                    fLastEventDataLength = event->EventDataLength;
                    fLastEventAckRequired = event->AckRequired;
                    fLastEventContext = event->EventContext;
                    if (event->Event < 128) {
                        fEventSeen[event->Event / 32] |= 1U << (event->Event % 32);
                    }
                    fEventCount++;
                    fEventReplyCount++;
                    if (smid == 2 && fManagementBusy[2]) {
                        /* Only the subscription's own SMID completes it. */
                        storeCommandReplyLocked(2, 1, reply, bytes);
                    } else if (smid != 0) {
                        recordProtocolFaultLocked(kMPRProtocolUnexpectedCompletion, smid);
                    }
                    if (eventBytes > bytes - sizeof(MPREventNotifyReply)) {
                        recordProtocolFaultLocked(kMPRProtocolEventLength, smid);
                    } else if (event->Event == MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST &&
                               (event->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS) {
                        recordTopologyEventLocked(reply + sizeof(MPREventNotifyReply),
                                                  eventBytes);
                    } else if ((event->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS &&
                               (event->Event == MPI2_EVENT_IR_CONFIGURATION_CHANGE_LIST ||
                                event->Event == MPI2_EVENT_IR_PHYSICAL_DISK ||
                                event->Event == MPI2_EVENT_IR_VOLUME)) {
                        /* Membership can change without a physical hotplug.
                         * Freeze publication until the complete config is read. */
                        if (fMediaPresent) fRemovalPending = true;
                        fTargetRemovalRequested = false;
                        fTopologyGeneration++;
                        fTopologyChanged = true;
                        fHotplugPending = true;
                    }
                    /* DEVICE_STATUS_CHANGE also reports internal reset/TMF
                     * completion and SMART. It is not a device add/remove. */
                    if (event->AckRequired == MPI2_EVENT_NOTIFICATION_ACK_REQUIRED) {
                        sendEventAck(event->Event, event->EventContext);
                    }
                }
            } else {
                storeCommandReplyLocked(smid, 1, reply, bytes);
                memset(fLastReply, 0, sizeof(fLastReply));
                memcpy(fLastReply, reply,
                       bytes < sizeof(fLastReply) ? bytes : sizeof(fLastReply));
                fLastReplySmid = smid;
                fLastReplyKind = 1;
                fReplyCount++;
            }
            releaseReplyFrame(baddr);
        } else if (flags == MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS ||
                   flags == MPI25_RPY_DESCRIPT_FLAGS_FAST_PATH_SCSI_IO_SUCCESS) {
            if (smid == MPR_SM_EVENT_ACK) {
                recordProtocolFaultLocked(kMPRProtocolAckReply, smid);
                fEventAckFailed++;
            } else {
                storeCommandReplyLocked(smid, 2, NULL, 0);
                fLastReplySmid = smid;
                fLastReplyKind = 2;
                memset(fLastReply, 0, sizeof(fLastReply));
                fScsiSuccess++;
                fReplyCount++;
            }
        } else {
            noteBadReply(1, (UInt32)flags, smid, NULL, 0);
            recordProtocolFaultLocked(kMPRProtocolManagementReply, smid);
        }
    }
    /* Publish the index while still owning it; another consumer must not
     * publish an older index after we have advanced the ring again. */
    writeReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET, fReplyPostIndex);
    IOSimpleLockUnlock(fReplyLock);
}



bool
MPRController::waitForSmid(UInt32 timeoutMs, UInt32 smid)
{
    UInt32 i;
    bool ready;

    fTimedOutSmid = 0;
    if (smid == 0 || smid >= MPR_REQUEST_FRAMES || fReplyLock == NULL) {
        return false;
    }
    /* Fault injection: with mpr_timeout_test=1 in boot-args, fail exactly one
     * SCSI command as if its reply were lost. It must land after bring-up:
     * a timeout while fStarting escalates to the bounded reset by design and
     * would consume the injection without exercising the TMF cascade. */
    if (!fStarting && smid >= 10 && smid <= 25 &&
        (fInjectTimeout || fInjectEvery != 0)) {
        bool inject = false;

        IOSimpleLockLock(fReplyLock);
        bool eligible = fCommandOwners[smid].epoch == fTransportEpoch &&
            (fCommandOwners[smid].state == kMPRCommandPosted ||
             fCommandOwners[smid].state == kMPRCommandComplete);
        if (eligible && fInjectTimeout) {
            fInjectTimeout = false;
            inject = true;
        } else if (eligible && fInjectEvery != 0) {
            if (++fInjectCounter >= fInjectEvery) {
                fInjectCounter = 0;
                inject = true;
            }
        }
        if (inject) fInjectCount++;
        IOSimpleLockUnlock(fReplyLock);

        if (inject) {
            setProperty("MPRInjectTimeout", 0ULL, 8);
            setProperty("MPRInjectCount", (unsigned long long)fInjectCount, 32);
            fTimedOutSmid = (UInt16)smid;
            return false;
        }
    }
    /* Only the slot owned by this SMID can satisfy the wait; a late reply for
     * another command is published to that command's slot instead. */
    for (i = 0; i < timeoutMs; i++) {
        if (fStopping) {
            fTimedOutSmid = (UInt16)smid;
            return false;
        }
        IOSimpleLockLock(fReplyLock);
        ready = (fReplySlots[smid].valid != 0);
        bool inactive = fCommandOwners[smid].state == kMPRCommandFree ||
                        fCommandOwners[smid].state == kMPRCommandReserved;
        bool protocolFault = fProtocolRecoveryPending;
        IOSimpleLockUnlock(fReplyLock);
        if (ready) {
            if (i >= 1000) {
                setProperty("MPRLongReplyWaitMs", (unsigned long long)i, 32);
                setProperty("MPRLongReplyWaitSmid", (unsigned long long)smid, 16);
            }
            return true;
        }
        if (inactive || protocolFault) {
            fTimedOutSmid = inactive ? 0 : (UInt16)smid;
            return false;
        }
        /* A long wait (PORT_ENABLE) must not stop the consumer: replies that
         * land in the queue while this thread sleeps still have to reach
         * their slots, and queued event ACKs have to be posted. */
        if ((i % 20) == 19) {
            processReplyQueue();
        }
        IOSleep(1);
    }
    processReplyQueue();
    IOSimpleLockLock(fReplyLock);
    ready = (fReplySlots[smid].valid != 0);
    IOSimpleLockUnlock(fReplyLock);
    if (ready) {
        return true;
    }
    /* A real timeout needs task management; a SCSI status error needs
     * sense decoding instead. */
    fTimedOutSmid = (UInt16)smid;
    return false;
}

bool
MPRController::takeReplySlot(UInt32 smid, UInt8 *reply, UInt32 replyLen,
                             UInt8 *kind)
{
    bool ok = false;

    if (smid >= 128 || reply == NULL || replyLen == 0 ||
        fReplyLock == NULL) {
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    if (fReplySlots[smid].valid &&
        (fCommandOwners[smid].state == kMPRCommandComplete ||
         fCommandOwners[smid].state == kMPRCommandCancelled)) {
        UInt32 n = replyLen < (UInt32)sizeof(fReplySlots[smid].reply)
                       ? replyLen : (UInt32)sizeof(fReplySlots[smid].reply);

        memcpy(reply, fReplySlots[smid].reply, n);
        if (kind != NULL) {
            *kind = fReplySlots[smid].kind;
        }
        fReplySlots[smid].valid = 0;
        fCommandOwners[smid].state = kMPRCommandFree;
        ok = true;
    }
    IOSimpleLockUnlock(fReplyLock);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* install the event mask and enable the SAS port. Firmware events are  */
/* collected asynchronously by processReplyQueue().                          */

static void
mprClearEventBit(UInt32 *masks, UInt32 event)
{
    masks[event / 32] &= ~(1U << (event % 32));
}

bool
MPRController::sendEventNotification(void)
{
    UInt8 *frame;
    MPREventNotifyRequest *request;
    UInt8 replyBuf[64], replyKind = 0;

    if (fRequestBuffer == NULL || !beginManagementRequest(2)) {
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        recordProtocolFault(kMPRProtocolManagementReply, 2);
        return false;
    }
    frame += 2 * fRequestFrameSize;     /* SMID 2 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPREventNotifyRequest *)frame;

    request->Function = MPI2_FUNCTION_EVENT_NOTIFICATION;
    /* A set bit DISABLES an event (mpr.c:2769-2791): mask everything first,
     * then clear the topology, diagnostic and RAID-membership events. */
    memset(request->EventMasks, 0xFF, sizeof(request->EventMasks));
    mprClearEventBit(request->EventMasks, MPI2_EVENT_STATE_CHANGE);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DISCOVERY);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_BROADCAST_PRIMITIVE);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_LOG_ENTRY_ADDED);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DEVICE_DISCOVERY_ERROR);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_IR_CONFIGURATION_CHANGE_LIST);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_IR_PHYSICAL_DISK);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_IR_VOLUME);

    setProperty("MPREventMask0", (unsigned long long)request->EventMasks[0], 32);
    setProperty("MPREventMask1", (unsigned long long)request->EventMasks[1], 32);
    setProperty("MPREventMask2", (unsigned long long)request->EventMasks[2], 32);
    setProperty("MPREventMask3", (unsigned long long)request->EventMasks[3], 32);
    setProperty("MPREventMaskCheck",
                (request->EventMasks[0] == 0x2F3F7FFBU &&
                 request->EventMasks[1] == 0xFFDFFFFCU &&
                 request->EventMasks[2] == 0xFFFFFFFFU &&
                 request->EventMasks[3] == 0xFFFFFFFFU) ? "ok" : "mismatch");

    postDefaultRequest(2);
    if (!finishManagementRequest(2, MPR_REPLY_TIMEOUT_MS, replyBuf, &replyKind)) {
        setProperty("MPREventNotifyState", "no-subscription-reply");
        return false;
    }
    MPREventNotifyReply *reply = (MPREventNotifyReply *)replyBuf;
    if (replyKind != 1 || reply->Function != MPI2_FUNCTION_EVENT_NOTIFICATION ||
        reply->MsgLength * 4U < sizeof(MPREventNotifyReply)) {
        noteBadReply(4, (UInt32)replyKind, 2, replyBuf, sizeof(replyBuf));
        recordProtocolFault(kMPRProtocolManagementReply, 2);
        setProperty("MPREventNotifyState", "unexpected-subscription-reply");
        return false;
    }
    UInt16 status = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
    setProperty("MPREventNotifyIOCStatus", (unsigned long long)status, 16);
    setProperty("MPREventNotifyState", status == MPI2_IOCSTATUS_SUCCESS ? "ok" : "failed-status");
    return status == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPRController::sendPortEnable(void)
{
    if (fRequestBuffer == NULL || !beginManagementRequest(3)) {
        return false;
    }
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        recordProtocolFault(kMPRProtocolManagementReply, 3);
        return false;
    }
    frame += 3 * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPRPortEnableRequest *request = (MPRPortEnableRequest *)frame;
    request->Function = MPI2_FUNCTION_PORT_ENABLE;
    request->PortFlags = 0;
    UInt8 replyBuf[64], replyKind = 0;
    postDefaultRequest(3);
    if (!finishManagementRequest(3, MPR_PORT_ENABLE_TIMEOUT_MS, replyBuf, &replyKind)) {
        return false;
    }
    MPRPortEnableReply *reply = (MPRPortEnableReply *)replyBuf;
    if (replyKind != 1 || reply->Function != MPI2_FUNCTION_PORT_ENABLE ||
        reply->MsgLength * 4U < sizeof(MPRPortEnableReply)) {
        noteBadReply(3, (UInt32)replyKind, 3, replyBuf, sizeof(replyBuf));
        recordProtocolFault(kMPRProtocolManagementReply, 3);
        return false;
    }
    UInt16 status = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
    setProperty("MPRPortEnableStatus", (unsigned long long)status, 16);
    setProperty("MPRPortEnableLogInfo", (unsigned long long)reply->IOCLogInfo, 32);
    return status == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPRController::enableEventsAndPort(void)
{
    bool notified, enabled;

    notified = sendEventNotification();
    setProperty("MPREventNotify", notified ? "posted" : "failed");
    if (!notified) {
        setProperty("MPREventState", "failed-notify");
        return false;
    }

    enabled = sendPortEnable();
    setProperty("MPRPortEnable", enabled ? "ok" : "failed");
    if (!enabled) {
        setProperty("MPREventState", "failed-portenable");
        return false;
    }

    /* Let the IOC raise discovery/topology events before sampling counters. */
    IOSleep(5000);

    setProperty("MPREventCount", (unsigned long long)fEventCount, 32);
    setProperty("MPRLastEvent", (unsigned long long)fLastEvent, 16);
    setProperty("MPRLastEventDataLength", (unsigned long long)fLastEventDataLength, 16);
    setProperty("MPRLastEventAck", (unsigned long long)fLastEventAckRequired, 8);
    setProperty("MPREventSeen0", (unsigned long long)fEventSeen[0], 32);
    setProperty("MPREventSeen1", (unsigned long long)fEventSeen[1], 32);
    setProperty("MPREventSeen2", (unsigned long long)fEventSeen[2], 32);
    setProperty("MPREventSeen3", (unsigned long long)fEventSeen[3], 32);
    setProperty("MPREventState", "ok");
    IOLog("MPR events=%u last=0x%04x len=%u ack=%u\n",
          (unsigned int)fEventCount, (unsigned int)fLastEvent,
          (unsigned int)fLastEventDataLength, (unsigned int)fLastEventAckRequired);
    return true;
}

/* ------------------------------------------------------------------------ */
/* extended config page read, used to build the SAS target table.      */

/* Target protocol implementation is in MPRTarget.cpp. */

/* Target protocol implementation is in MPRTarget.cpp. */

/* Target protocol implementation is in MPRTarget.cpp. */

bool
MPRController::discoverTopology(void)
{
    bool ownPage = (fPageBuffer == NULL);
    bool ok;

    if (fProtocolRecoveryPending || !allocatePageBuffer()) {
        setProperty("MPRTopologyState", "failed-buffer-unavailable");
        return false;
    }
    ok = readSasIOUnitPage0();
    setProperty("MPRTopologyState", ok ? "ok" : "failed");

    /* A timed-out command may still DMA into the page. Recovery owns it
     * until the IOC has stopped; never free it merely because a probe ended. */
    if (ownPage && !fProtocolRecoveryPending) {
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* SCSI INQUIRY through the request/reply queues, validating the SCSI  */
/* IO path before the block-storage nub is added.                            */

/* IEEE simple SGE (16 bytes): Address, Length, Reserved1/2, Flags.
 * SCSI IO uses IEEE SGEs; only CONFIG requests use the legacy MPI format. */
static void
mprFillIeeeSge(UInt8 *sge, UInt64 address, UInt32 length)
{
    UInt32 zero = 0;
    UInt8  flags = 0x40;    /* simple element, system address, end of list */

    memcpy(sge, &address, sizeof(address));
    memcpy(sge + 8, &length, sizeof(length));
    memcpy(sge + 12, &zero, sizeof(zero));
    memcpy(sge + 15, &flags, sizeof(flags));
}

bool
MPRController::postScsiIo(UInt32 smid)
{
    if (fReplyLock == NULL || smid == 0 || smid >= 128) return false;
    /* A fast-path request descriptor carries the DevHandle in its high word,
     * and the IOC uses that copy rather than the request frame. It must come
     * from the frame of this particular command, never from the controller's
     * single default handle, or every target's I/O goes to one disk. */
    UInt8 *base = fRequestBuffer != NULL ?
        (UInt8 *)fRequestBuffer->getBytesNoCopy() : NULL;
    if (base == NULL) return false;
    const UInt8 *frame = base + (UInt64)smid * fRequestFrameSize;
    UInt16 handle = (UInt16)((UInt16)frame[0] | ((UInt16)frame[1] << 8));
    if (handle == 0) handle = (UInt16)fAttachedDevHandle;
    UInt32 flags = fFastPathEnabled
                       ? MPI25_REQ_DESCRIPT_FLAGS_FAST_PATH_SCSI_IO
                       : MPI2_REQ_DESCRIPT_FLAGS_SCSI_IO;
    IOSimpleLockLock(fReplyLock);
    bool runtime = smid >= 10 && smid <= 30;
    if (fStopping || fProtocolRecoveryPending ||
        (runtime && (fRemovalPending || !fMediaPresent || fDeviceOffline ||
                     fReinitInProgress)) || fFrozenSmid == smid) {
        IOSimpleLockUnlock(fReplyLock);
        cancelUnpostedCommand(smid);
        return false;
    }
    bool posted = postOwnedRequestLocked(smid, flags,
                                        (UInt32)handle << 16);
    IOSimpleLockUnlock(fReplyLock);
    if (!posted) cancelUnpostedCommand(smid);
    return posted;
}

bool
MPRController::scsiTur(void)
{
    MPRScsiIoRequest *request;
    MPRScsiIoReply *reply;
    UInt8 *frame;
    UInt8 replyBuf[64], replyKind = 0;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRTurFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(6)) {
        return false;
    }
    if (!prepareScsiSense(6)) {
        cancelUnpostedCommand(6);
        return false;
    }
    frame += 6 * fRequestFrameSize;     /* SMID 6 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(6);
    request->SenseBufferLength = MPR_SENSE_BYTES;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 0;
    request->IoFlags = (UInt16)(6 | (fFastPathEnabled
                                   ? MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
    request->Control = MPI2_SCSIIO_CONTROL_NODATATRANSFER;
    request->CDB[0] = 0x00;                 /* TEST UNIT READY */

    /* A zero-length IEEE SGE is still required for a data-less command. */
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 0);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_PAGE_BUF_BYTES);
    if (!postScsiIo(6)) return false;
    setProperty("MPRTurAttempts", 1ULL, 32);
    if (!finishManagementRequest(6, MPR_REPLY_TIMEOUT_MS, replyBuf, &replyKind)) {
        setProperty("MPRTurFailure", "TEST UNIT READY timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(6, replyKind, replyBuf, 0, false,
                                &decodedStatus)) {
        return false;
    }
    reply = (MPRScsiIoReply *)replyBuf;
    setProperty("MPRTurKind", (unsigned long long)replyKind, 32);
    setProperty("MPRTurSCSIState", (unsigned long long)reply->SCSIState, 8);
    if (replyKind == 1) {
        setProperty("MPRTurIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRTurScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        return (reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS &&
               reply->SCSIStatus == 0;
    }
    setProperty("MPRTurIOCStatus", 0ULL, 16);
    setProperty("MPRTurScsiStatus", 0ULL, 8);
    return true;
}

bool
MPRController::scsiInquiry(void)
{
    MPRScsiIoRequest *request;
    MPRScsiIoReply   *reply;
    UInt8 *frame;
    UInt8 *data;
    bool   posted;
    UInt8 replyBuf[64], replyKind = 0;
    char vendor[9], product[17], revision[5];

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRScsiProbeFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(5)) {
        return false;
    }
    if (!prepareScsiSense(5)) {
        cancelUnpostedCommand(5);
        return false;
    }
    frame += 5 * fRequestFrameSize;     /* SMID 5 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(5);
    request->SenseBufferLength = MPR_SENSE_BYTES;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 36;
    request->IoFlags = (UInt16)(6 | (fFastPathEnabled
                                   ? MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));   /* six-byte CDB */
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    request->CDB[0] = 0x12;                 /* INQUIRY */
    request->CDB[4] = 36;                   /* allocation length */

    /* IEEE SGE placed right after the fixed CDB area. */
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 36);

    {
        UInt64 w[4];

        memcpy(w, frame, sizeof(w));
        setProperty("MPRInquiryReqW0", w[0], 64);
        setProperty("MPRInquiryReqW1", w[1], 64);
        setProperty("MPRInquiryReqW2", w[2], 64);
        setProperty("MPRInquiryReqW3", w[3], 64);
        setProperty("MPRInquiryDevHandleUsed",
                    (unsigned long long)fAttachedDevHandle, 16);
        setProperty("MPRInquiryControl", (unsigned long long)request->Control, 32);
        setProperty("MPRInquiryIoFlags", (unsigned long long)request->IoFlags, 32);
    }

    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_PAGE_BUF_BYTES);
    if (!postScsiIo(5)) return false;
    posted = finishManagementRequest(5, 15000, replyBuf, &replyKind);
    setProperty("MPRInquiryAttempts", 1ULL, 32);
    if (!posted) {
        setProperty("MPRInquiryKind", (unsigned long long)replyKind, 32);
        setProperty("MPRInquirySmid", (unsigned long long)fLastReplySmid, 16);
        setProperty("MPRInquiryPostIdx", (unsigned long long)fReplyPostIndex, 32);
        setProperty("MPRInquiryRegPostIdx",
                    (unsigned long long)readReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET), 32);
        setProperty("MPRInquiryRegFreeIdx",
                    (unsigned long long)readReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET), 32);
        setProperty("MPRScsiProbeFailure", "INQUIRY timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(5, replyKind, replyBuf, 36, false,
                                &decodedStatus)) {
        return false;
    }


    {
        UInt64 w[4];

        memcpy(w, replyBuf, sizeof(w));
        setProperty("MPRInquiryReplyW0", w[0], 64);
        setProperty("MPRInquiryReplyW1", w[1], 64);
        setProperty("MPRInquiryReplyW2", w[2], 64);
        setProperty("MPRInquiryReplyW3", w[3], 64);
        reply = (MPRScsiIoReply *)replyBuf;
        setProperty("MPRInquirySCSIState", (unsigned long long)reply->SCSIState, 8);
        setProperty("MPRInquiryReplyFunction", (unsigned long long)reply->Function, 8);
        setProperty("MPRInquiryReplyMsgLength", (unsigned long long)reply->MsgLength, 8);
    }

    if (replyKind == 1) {
        reply = (MPRScsiIoReply *)replyBuf;
        setProperty("MPRInquiryIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRInquiryScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        setProperty("MPRInquiryTransferCount", (unsigned long long)reply->TransferCount, 32);
        setProperty("MPRInquiryResponseInfo", (unsigned long long)reply->ResponseInfo, 32);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0 || reply->TransferCount < 36U) {
            setProperty("MPRScsiProbeFailure", "INQUIRY reply status");
            return false;
        }
    } else {
        /* SCSI_IO_SUCCESS: command succeeded, no reply frame was posted. */
        setProperty("MPRInquiryIOCStatus", 0ULL, 16);
        setProperty("MPRInquiryScsiStatus", 0ULL, 8);
        setProperty("MPRInquiryTransferCount", 36ULL, 32);
        setProperty("MPRInquiryResponseInfo", 0ULL, 32);
    }
    setProperty("MPRInquiryReplyKind", (unsigned long long)replyKind, 32);

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        setProperty("MPRScsiProbeFailure", "INQUIRY data buffer unavailable");
        return false;
    }
    if ((data[0] & 0xE0) != 0 || (data[0] & 0x1F) != 0 || data[4] < 31) {
        setProperty("MPRScsiProbeFailure", "not a connected direct-access disk");
        return false;
    }
    mprCopyField(vendor, data + 8, 8);
    mprCopyField(product, data + 16, 16);
    mprCopyField(revision, data + 32, 4);

    /* Cache the identity for the block device nub's get*String() methods. */
    memcpy(fVendor, vendor, sizeof(vendor));
    memcpy(fProduct, product, sizeof(product));
    memcpy(fRevision, revision, sizeof(revision));

    setProperty("MPRInquiryDeviceType", (unsigned long long)(data[0] & 0x1F), 8);
    setProperty("MPRInquiryVendor", vendor);
    setProperty("MPRInquiryProduct", product);
    setProperty("MPRInquiryRevision", revision);
    IOLog("MPR INQUIRY type=%u vendor=%s product=%s rev=%s\n",
          (unsigned int)(data[0] & 0x1F), vendor, product, revision);
    return true;
}

bool
MPRController::scsiReadCapacity16(void)
{
    /* READ CAPACITY(16) is SERVICE ACTION IN(16). The allocation length is
     * a 32-bit field at bytes 10..13 and byte 15 is CONTROL (SBC-3); the
     * previous encoding put 0x20 in the CONTROL byte. */
    static const UInt8 cdb[16] = {
        0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0
    };
    MPRScsiIoRequest *request;
    MPRScsiIoReply *reply;
    UInt8 *frame, *data;
    UInt64 lastLba = 0;
    UInt32 blockSize, i;
    bool posted;
    UInt8 replyBuf[64], replyKind = 0;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRCapacityProbeFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(7)) {
        return false;
    }
    if (!prepareScsiSense(7)) {
        cancelUnpostedCommand(7);
        return false;
    }
    frame += 7 * fRequestFrameSize;     /* SMID 7 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(7);
    request->SenseBufferLength = MPR_SENSE_BYTES;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 32;
    request->IoFlags = (UInt16)(16 | (fFastPathEnabled
                                    ? MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, sizeof(cdb));
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 32);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_PAGE_BUF_BYTES);
    if (!postScsiIo(7)) return false;
    posted = finishManagementRequest(7, 15000, replyBuf, &replyKind);
    if (!posted) {
        setProperty("MPRCapacityProbeFailure", "READ CAPACITY(16) timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(7, replyKind, replyBuf, 32, false,
                                &decodedStatus)) {
        return false;
    }

    if (replyKind == 1) {
        reply = (MPRScsiIoReply *)replyBuf;
        setProperty("MPRScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0 || reply->TransferCount < 32U) {
            setProperty("MPRCapacityProbeFailure", "READ CAPACITY(16) status");
            return false;
        }
    }

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        return false;
    }
    for (i = 0; i < 8; i++) {
        lastLba = (lastLba << 8) | (UInt64)data[i];
    }
    blockSize = ((UInt32)data[8] << 24) | ((UInt32)data[9] << 16) |
                ((UInt32)data[10] << 8) | (UInt32)data[11];

    setProperty("MPRCapacityLastLba", lastLba, 64);
    setProperty("MPRCapacityBlockSize", (unsigned long long)blockSize, 32);
    setProperty("MPRCapacityBlocks", lastLba + 1, 64);
    IOLog("MPR READ CAPACITY(16) blocks=%llu blockSize=%u\n",
          (unsigned long long)(lastLba + 1), (unsigned int)blockSize);
    if (blockSize == 0 || lastLba == 0xFFFFFFFFFFFFFFFFULL) {
        return false;
    }
    fBlockSize = blockSize;
    fCapacityBlocks = lastLba + 1;
    return true;
}

bool
MPRController::scsiReadCapacity10(void)
{
    static const UInt8 cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    MPRScsiIoRequest *request;
    MPRScsiIoReply *reply;
    UInt8 *frame, *data;
    UInt32 lastLba, blockSize;
    bool posted;
    UInt8 replyBuf[64], replyKind = 0;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRCapacityProbeFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(8)) {
        return false;
    }
    if (!prepareScsiSense(8)) {
        cancelUnpostedCommand(8);
        return false;
    }
    frame += 8 * fRequestFrameSize;     /* SMID 8 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(8);
    request->SenseBufferLength = MPR_SENSE_BYTES;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 8;
    request->IoFlags = (UInt16)(10 | (fFastPathEnabled
                                    ? MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, sizeof(cdb));
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 8);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_PAGE_BUF_BYTES);
    if (!postScsiIo(8)) return false;
    posted = finishManagementRequest(8, 15000, replyBuf, &replyKind);
    if (!posted) {
        setProperty("MPRCapacityProbeFailure", "READ CAPACITY(10) timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(8, replyKind, replyBuf, 8, false,
                                &decodedStatus)) {
        return false;
    }

    if (replyKind == 1) {
        reply = (MPRScsiIoReply *)replyBuf;
        setProperty("MPRScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0 || reply->TransferCount < 8U) {
            setProperty("MPRCapacityProbeFailure", "READ CAPACITY(10) status");
            return false;
        }
    }

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        return false;
    }
    lastLba = ((UInt32)data[0] << 24) | ((UInt32)data[1] << 16) |
              ((UInt32)data[2] << 8) | (UInt32)data[3];
    blockSize = ((UInt32)data[4] << 24) | ((UInt32)data[5] << 16) |
                ((UInt32)data[6] << 8) | (UInt32)data[7];

    /* 0xffffffff means "capacity too large for RC10"; RC16 is mandatory. */
    if (lastLba == 0xFFFFFFFFU) {
        setProperty("MPRCapacityProbeFailure", "RC10 sentinel requires RC16");
        return false;
    }

    setProperty("MPRCapacityLastLba", (unsigned long long)lastLba, 64);
    setProperty("MPRCapacityBlockSize", (unsigned long long)blockSize, 32);
    setProperty("MPRCapacityBlocks", (unsigned long long)lastLba + 1, 64);
    IOLog("MPR READ CAPACITY(10) blocks=%llu blockSize=%u\n",
          (unsigned long long)(lastLba + 1), (unsigned int)blockSize);
    if (blockSize == 0) {
        return false;
    }
    fBlockSize = blockSize;
    fCapacityBlocks = (UInt64)lastLba + 1;
    return true;
}

/* ------------------------------------------------------------------------ */
/* multi-block READ(10)/WRITE(10) through the same fast-path SCSI IO    */
/* path, staged in the low-4GB bulk buffer.                                  */

bool
MPRController::scsiReadWrite(bool write, UInt64 lba, UInt32 blocks,
                             UInt32 blockSize, UInt32 smid, bool diagnostics,
                             bool fua, IOReturn *outStatus)
{
    UInt8 cdb[16];
    MPRScsiIoRequest *request;
    UInt8 *frame;
    UInt64 wideDataLen = (UInt64)blocks * blockSize;
    UInt32 dataLen = (UInt32)wideDataLen;
    UInt32 cdbLength = 0;

    if (outStatus != NULL) {
        *outStatus = kIOReturnNotReady;
    }

    if ((fDeviceOffline && !fReinitInProgress) || fStopping ||
        fProtocolRecoveryPending ||
        (fFrozenSmid != 0 && fFrozenSmid == (UInt16)smid)) {
        return false;
    }
    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL ||
        fPageBuffer == NULL || fDataBuffer == NULL) {
        if (diagnostics) {
            setProperty("MPRReadProbeFailure", "no device or staging buffer");
        }
        return false;
    }
    if (wideDataLen == 0 || wideDataLen > fDataBytes ||
        lba >= fCapacityBlocks || blocks > fCapacityBlocks - lba) {
        if (diagnostics) {
            setProperty("MPRReadProbeFailure", "transfer out of range");
        }
        return false;
    }

    cdbLength = mprBuildReadWriteCdb(cdb, write, lba, blocks,
                                      fua && isNativeFuaSupported());
    if (cdbLength == 0) return false;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!prepareScsiSense(smid)) {
        cancelUnpostedCommand(smid);
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
    request->SenseBufferLength = MPR_SENSE_BYTES;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)(cdbLength |
                                (fFastPathEnabled
                                     ? MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
    request->Control = write ? MPI2_SCSIIO_CONTROL_WRITE : MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, cdbLength);
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fDataPhys, dataLen);

    /* Clear the previous command's sense so an old CHECK CONDITION cannot be
     * mistaken for this command's. A write caller has already filled the
     * staging buffer; only reads need it cleared. */
    if (!write) {
        memset(fDataBuffer->getBytesNoCopy(), 0, fDataBytes);
    }
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    if (!postScsiIo(smid)) {
        if (outStatus != NULL) *outStatus = kIOReturnNoMedia;
        return false;
    }
    UInt32 actual = 0;
    IOReturn result = kIOReturnIOError;
    bool ok = scsiWaitChunk(smid, dataLen, &result, &actual);
    if (ok && write && fua && !isNativeFuaSupported()) {
        result = scsiSynchronizeCache(0, 0, 29, true);
        ok = result == kIOReturnSuccess;
    }
    if (outStatus != NULL) *outStatus = result;
    if (diagnostics) {
        setProperty("MPRReadTransferCount", (unsigned long long)actual, 32);
        setProperty("MPRReadResult", (unsigned long long)(UInt32)result, 32);
    }
    return ok;
}

bool
MPRController::probeBlockRead(void)
{
    UInt8 *data;
    bool ok = false;
    UInt32 i;
    char name[32];

    if (fProtocolRecoveryPending || !allocateResidentBuffers()) {
        setProperty("MPRReadProbeState", "failed-buffer-unavailable");
        return false;
    }
    /* Read the first 8 blocks and decode the MBR/GPT protective entry. */
    ok = scsiReadWrite(false, 0, 8, 512, 9, true, false);
    setProperty("MPRReadProbeState", ok ? "ok" : "failed");

    data = (UInt8 *)fDataBuffer->getBytesNoCopy();
    if (ok && data != NULL) {
        UInt64 w[2];

        memcpy(w, data, sizeof(w));
        setProperty("MPRReadWord0", w[0], 64);
        setProperty("MPRReadWord1", w[1], 64);
        setProperty("MPRReadSig",
                    (unsigned long long)((UInt32)data[510] |
                                         ((UInt32)data[511] << 8)), 16);
        setProperty("MPRReadGptSig",
                    (unsigned long long)((*(const UInt64 *)(data + 512)) ==
                                         0x5452415020494645ULL), 8);

        for (i = 0; i < 4; i++) {
            const UInt8 *entry = data + 0x1BE + i * 16;
            UInt32 start = (UInt32)entry[8] | ((UInt32)entry[9] << 8) |
                           ((UInt32)entry[10] << 16) | ((UInt32)entry[11] << 24);
            UInt32 count = (UInt32)entry[12] | ((UInt32)entry[13] << 8) |
                           ((UInt32)entry[14] << 16) | ((UInt32)entry[15] << 24);

            snprintf(name, sizeof(name), "MPRPart%uType", (unsigned int)i);
            setProperty(name, (unsigned long long)entry[4], 8);
            snprintf(name, sizeof(name), "MPRPart%uStart", (unsigned int)i);
            setProperty(name, (unsigned long long)start, 32);
            snprintf(name, sizeof(name), "MPRPart%uBlocks", (unsigned int)i);
            setProperty(name, (unsigned long long)count, 32);
        }
        IOLog("MPR LBA0 read ok, sig=0x%04x part0type=0x%02x\n",
              (unsigned int)((UInt32)data[510] | ((UInt32)data[511] << 8)),
              (unsigned int)data[0x1BE + 4]);
    }

    /* These are the runtime buffers as well. Keep the same pair for nub
     * publication and recovery, including when a command has timed out. */
    return ok;
}

/* Split command submission from completion so one block request can keep
 * MPR_IO_SLOTS chunk commands outstanding. Each slot owns SMID 10..13, its own
 * request frame, 64 KiB staging window and independent bounded sense area. */
bool
MPRController::scsiPostIo(bool write, UInt64 lba, UInt32 blocks,
                          UInt32 blockSize, UInt32 smid, bool fua,
                          UInt64 dataPhys, IOReturn *outStatus)
{
    UInt8 cdb[16];
    MPRScsiIoRequest *request;
    UInt8 *frame;
    UInt64 wideDataLen = (UInt64)blocks * blockSize;
    UInt32 dataLen = (UInt32)wideDataLen;
    UInt32 cdbLength = 0;

    if (outStatus != NULL) *outStatus = kIOReturnNotReady;
    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL ||
        fDataBuffer == NULL || fPageBuffer == NULL) {
        return false;
    }
    if (wideDataLen == 0 || wideDataLen > MPR_DATA_BUF_BYTES || dataPhys == 0 ||
        lba >= fCapacityBlocks || blocks > fCapacityBlocks - lba) {
        return false;
    }

    cdbLength = mprBuildReadWriteCdb(cdb, write, lba, blocks,
                                      fua && isNativeFuaSupported());
    if (cdbLength == 0) return false;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!prepareScsiSense(smid)) {
        cancelUnpostedCommand(smid);
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;
    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
    request->SenseBufferLength = MPR_SENSE_BYTES;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)(cdbLength |
                                (fFastPathEnabled
                                     ? MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
    request->Control = write ? MPI2_SCSIIO_CONTROL_WRITE : MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, cdbLength);
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, dataPhys, dataLen);

    if (!postScsiIo(smid)) {
        if (outStatus != NULL) *outStatus = kIOReturnNoMedia;
        return false;
    }
    if (outStatus != NULL) *outStatus = kIOReturnSuccess;
    return true;
}

bool
MPRController::scsiWaitChunk(UInt32 smid, UInt32 dataLen, IOReturn *outStatus,
                             UInt32 *actualBytes, bool *unitAttention)
{
    UInt8 replyBuf[64], replyKind = 0;
    if (outStatus != NULL) *outStatus = kIOReturnIOError;
    if (actualBytes != NULL) *actualBytes = 0;
    if (unitAttention != NULL) *unitAttention = false;
    if (smid == 0 || smid >= fRequestCount || smid >= MPR_REQUEST_FRAMES ||
        fReplyLock == NULL) {
        if (outStatus != NULL) *outStatus = kIOReturnBadArgument;
        return false;
    }
    if (!waitForSmid(15000, smid)) {
        if (outStatus != NULL) *outStatus = kIOReturnTimeout;
        setProperty("MPRLastIOFailure", "reply-timeout");
        setProperty("MPRLastIOSmid", (unsigned long long)smid, 16);
        if (fTimedOutSmid == (UInt16)smid) {
            IOSimpleLockLock(fReplyLock);
            if (fCommandOwners[smid].state == kMPRCommandPosted)
                fCommandOwners[smid].state = kMPRCommandQuarantined;
            IOSimpleLockUnlock(fReplyLock);
            if (fReinitInProgress || fStarting) {
                recordProtocolFault(kMPRProtocolManagementTimeout, smid);
            } else {
                recoverTimedOutCommand(smid);
            }
        }
        /* Consume a completion/cancellation only after recovery proved this
         * command DMA-safe. An unknown-result write is still returned as an
         * error; it is never submitted again by this function. */
        if (commandDmaSafe(smid))
            takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind);
        return false;
    }
    if (!takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind)) return false;
    return validateScsiCompletion(smid, replyKind, replyBuf, dataLen, false,
                                  outStatus, actualBytes, unitAttention);
}

/* ------------------------------------------------------------------------ */
/* IOBlockStorageDevice nub, IOMedia, and the block read path.        */

IOReturn
MPRController::transferBlocks(bool write, IOMemoryDescriptor *buffer,
                              UInt64 block, UInt64 nblks, bool fua,
                              UInt64 *actualBytes)
{
    UInt64 byteCount = 0, done = 0, maxBlocks = 0;
    const UInt32 generation = fMediaGeneration;
    IOReturn status = kIOReturnNotReady;
    bool retried = false;
    if (actualBytes != NULL) *actualBytes = 0;
    if (buffer == NULL) return kIOReturnBadArgument;
    if (fIOLock == NULL) return kIOReturnNotReady;
    status = buffer->prepare();
    if (status != kIOReturnSuccess) return status;
    IOLockLock(fIOLock);
    if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending()) {
        status = kIOReturnNoMedia;
        goto out;
    }
    if (fSuspended || fDeviceOffline || fStopping || fReinitInProgress ||
        fProtocolRecoveryPending || fDataBuffer == NULL || fSenseBuffer == NULL ||
        fPageBuffer == NULL || fCapacityBlocks == 0 || fBlockSize == 0) {
        status = kIOReturnNotReady;
        goto out;
    }
    if (nblks == 0) {
        status = kIOReturnSuccess;
        goto out;
    }
    if (nblks > 0xFFFFFFFFFFFFFFFFULL / fBlockSize) {
        status = kIOReturnBadArgument;
        goto out;
    }
    byteCount = nblks * fBlockSize;
    if (buffer->getLength() < byteCount || block >= fCapacityBlocks ||
        nblks > fCapacityBlocks - block) {
        status = kIOReturnBadArgument;
        goto out;
    }
    maxBlocks = fDataBytes / fBlockSize;
    if (maxBlocks == 0 || fDataBuffer->getBytesNoCopy() == NULL) {
        status = kIOReturnNotReady;
        goto out;
    }
    if (maxBlocks > 0xFFFFULL) maxBlocks = 0xFFFFULL;
    for (UInt32 id = 10; id < 10 + MPR_IO_SLOTS; ++id) {
        /* This synchronous caller owns no current batch yet. A previous
         * caller may have returned while an unsafe IOC kept its resident
         * window; a later real reply/reset cancellation can now be retired. */
        if (!commandDmaSafe(id)) {
            recordProtocolFault(kMPRProtocolManagementTimeout, id);
            status = kIOReturnNotReady;
            goto out;
        }
        UInt8 oldReply[64], oldKind = 0;
        takeReplySlot(id, oldReply, sizeof(oldReply), &oldKind);
    }
    status = kIOReturnSuccess;
    while (done < byteCount) {
        UInt32 batch = 0, slotBlocks[MPR_IO_SLOTS];
        UInt64 slotOff[MPR_IO_SLOTS], batchBytes = 0, prefixBytes = 0;
        bool failed = false, prefixOpen = true, allFailuresUA = true;
        bool dmaSafe = true;
        IOReturn failStatus = kIOReturnSuccess;
        UInt8 *pool = (UInt8 *)fDataBuffer->getBytesNoCopy();
        if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending()) {
            status = kIOReturnNoMedia;
            break;
        }
        if (fSuspended || fStopping || fDeviceOffline || fReinitInProgress ||
            fProtocolRecoveryPending || pool == NULL) {
            status = kIOReturnNotReady;
            break;
        }
        while (batch < MPR_IO_SLOTS && done + batchBytes < byteCount) {
            const UInt64 offset = done + batchBytes;
            UInt64 blocks = (byteCount - offset) / fBlockSize;
            if (blocks > maxBlocks) blocks = maxBlocks;
            const IOByteCount bytes = (IOByteCount)(blocks * fBlockSize);
            IOReturn posted = kIOReturnNotReady;
            if (write && buffer->readBytes((IOByteCount)offset,
                    pool + (UInt64)batch * MPR_DATA_BUF_BYTES, bytes) != bytes) {
                failed = true;
                allFailuresUA = false;
                failStatus = kIOReturnUnderrun;
                break;
            }
            if (!scsiPostIo(write, block + offset / fBlockSize, (UInt32)blocks,
                            fBlockSize, 10 + batch, fua,
                            fDataPhys + (UInt64)batch * MPR_DATA_BUF_BYTES, &posted)) {
                failed = true;
                allFailuresUA = false;
                failStatus = posted == kIOReturnSuccess ? kIOReturnIOError : posted;
                break;
            }
            slotBlocks[batch] = (UInt32)blocks;
            slotOff[batch] = offset;
            batchBytes += bytes;
            ++batch;
            ++fIoCount;
        }
        /* Every posted window is harvested, even after copy errors. A caller
         * cannot release this loop while later commands still own its windows. */
        for (UInt32 i = 0; i < batch; ++i) {
            IOReturn chunkStatus = kIOReturnIOError;
            UInt32 transferred = 0;
            bool ua = false;
            const IOByteCount bytes = (IOByteCount)((UInt64)slotBlocks[i] * fBlockSize);
            bool ok = scsiWaitChunk(10 + i, (UInt32)bytes, &chunkStatus,
                                    &transferred, &ua);
            if (!commandDmaSafe(10 + i)) dmaSafe = false;
            if (ok && (generation != fMediaGeneration || !fMediaPresent ||
                       mediaRemovalPending())) {
                ok = false;
                chunkStatus = kIOReturnNoMedia;
                ua = false;
            }
            IOByteCount copied = ok ? bytes : 0;
            if (ok && !write) {
                copied = buffer->writeBytes((IOByteCount)slotOff[i],
                            pool + (UInt64)i * MPR_DATA_BUF_BYTES, bytes);
                if (copied != bytes) {
                    ok = false;
                    chunkStatus = kIOReturnUnderrun;
                    ua = false;
                }
            }
            if (prefixOpen) prefixBytes += copied;
            if (!ok) {
                prefixOpen = false;
                allFailuresUA = allFailuresUA && ua;
                if (!failed) failStatus = chunkStatus;
                failed = true;
            } else if (write) {
                ++fWriteCount;
                fWriteBytes += bytes;
                fLastWriteLba = block + slotOff[i] / fBlockSize;
                fLastWriteSmid = 10 + i;
            }
        }
        if (!dmaSafe) {
            fDeviceOffline = true;
            recordProtocolFault(kMPRProtocolManagementTimeout, fTimedOutSmid);
            allFailuresUA = false;
            failed = true;
            if (failStatus == kIOReturnSuccess) failStatus = kIOReturnTimeout;
        }
        if (failed) {
            /* Re-reading a completed sibling is harmless; re-posting an
             * unknown-result WRITE is not. Retry only explicitly decoded,
             * current UNIT ATTENTION on a read, once, after all DMA has ended. */
            if (!write && allFailuresUA && !retried && dmaSafe &&
                generation == fMediaGeneration && fMediaPresent &&
                !fProtocolRecoveryPending && !mediaRemovalPending()) {
                retried = true;
                ++fIoRetryCount;
                scsiTur();
                IOSleep(50);
                continue;
            }
            done += prefixBytes;
            status = failStatus == kIOReturnSuccess ? kIOReturnIOError : failStatus;
            break;
        }
        if (batch == 0) {
            status = kIOReturnIOError;
            break;
        }
        done += batchBytes;
    }
    setProperty("MPRBlockIODoneBytes", done, 64);
    setProperty("MPRBlockIOCount", (unsigned long long)fIoCount, 32);
    setProperty("MPRBlockIORetryCount", (unsigned long long)fIoRetryCount, 32);
out:
    IOLockUnlock(fIOLock);
    buffer->complete();
    if (actualBytes != NULL) *actualBytes = done;
    return status;
}

bool
MPRController::allocatePageBuffer(void)
{
    IOByteCount segLength;
    IOBufferMemoryDescriptor *page = fPageBuffer;
    UInt64 physical;
    bool allocated = false;

    if (page == NULL) {
        page = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
        if (page == NULL) {
            return false;
        }
        allocated = true;
    }
    physical = page->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (page->getLength() < MPR_PAGE_BUF_BYTES || physical == 0 ||
        segLength < MPR_PAGE_BUF_BYTES || physical > MPR_DMA_LOW_MASK ||
        MPR_PAGE_BUF_BYTES - 1 > MPR_DMA_LOW_MASK - physical) {
        if (allocated) {
            page->release();
        }
        return false;
    }
    fPageBuffer = page;
    fPagePhys = physical;
    return true;
}

bool
MPRController::allocateResidentBuffers(void)
{
    IOByteCount segLength;
    IOBufferMemoryDescriptor *data = fDataBuffer;
    UInt64 physical;
    bool allocated = false;
    bool ownPage = (fPageBuffer == NULL);

    if (!allocatePageBuffer()) {
        return false;
    }
    if (data == NULL) {
        data = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            MPR_DATA_POOL_BYTES, MPR_DMA_LOW_MASK);
        allocated = (data != NULL);
    }
    if (data != NULL) {
        physical = data->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
        if (data->getLength() >= MPR_DATA_POOL_BYTES && physical != 0 &&
            segLength >= MPR_DATA_POOL_BYTES && physical <= MPR_DMA_LOW_MASK &&
            MPR_DATA_POOL_BYTES - 1 <= MPR_DMA_LOW_MASK - physical) {
            fDataBuffer = data;
            fDataPhys = physical;
            /* fDataBytes remains the per-command window size: maxBlocks and the
             * probe length checks are expressed in it. */
            fDataBytes = MPR_DATA_BUF_BYTES;
            return true;
        }
    }
    if (allocated) {
        data->release();
    }
    if (ownPage && !fProtocolRecoveryPending) {
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    return false;
}

/* Target protocol implementation is in MPRTarget.cpp. */

bool
MPRController::publishBlockDevice(void)
{
    /* No DMA preparation or transport operation belongs here: matching can
     * re-enter block IO immediately. The caller publishes outside fIOLock. */
    MPRBlockDev *nub;
    if (fBlockDev != NULL) return true;
    if (!fMediaPresent || fDeviceOffline || fAttachedDevHandle == 0 ||
        fCapacityBlocks == 0 || fBlockSize == 0 || fIOLock == NULL ||
        fPageBuffer == NULL || fDataBuffer == NULL ||
        fPagePhys == 0 || fDataPhys == 0 || fDataBytes == 0) {
        setProperty("MPRBlockDeviceState", "skipped-not-ready");
        return false;
    }

    nub = new MPRBlockDev;
    if (nub == NULL) {
        setProperty("MPRBlockDeviceState", "failed-nub-alloc");
        return false;
    }
    if (!nub->init(NULL)) {
        nub->release();
        setProperty("MPRBlockDeviceState", "failed-nub-init");
        return false;
    }
    nub->setController(this, (UInt16)fAttachedDevHandle, fTargetReadOnly);
    if (!nub->attach(this)) {
        nub->release();
        setProperty("MPRBlockDeviceState", "failed-nub-attach");
        return false;
    }
    if (!nub->start(this)) {
        nub->detach(this);
        nub->release();
        setProperty("MPRBlockDeviceState", "failed-nub-start");
        return false;
    }

    fBlockDev = nub;
    nub->registerService();
    setProperty("MPRBlockDeviceState", "ok");
    setProperty("MPRMediaBlocks", fCapacityBlocks, 64);
    setProperty("MPRMediaBlockSize", (unsigned long long)fBlockSize, 32);
    IOLog("MPR nub registered, blocks=%llu blockSize=%u\n",
          (unsigned long long)fCapacityBlocks, (unsigned int)fBlockSize);
    return true;
}

bool
MPRController::publishTarget(MPRTargetSlot *slot)
{
    if (slot == NULL || slot->handle == 0 || slot->nub != NULL ||
        slot->blockSize == 0 || slot->capacityBlocks == 0 ||
        fIOLock == NULL || fPageBuffer == NULL || fDataBuffer == NULL ||
        fPagePhys == 0 || fDataPhys == 0 || fDataBytes == 0) return false;
    MPRBlockDev *nub = new MPRBlockDev;
    if (nub == NULL) return false;
    if (!nub->init(NULL)) {
        nub->release();
        return false;
    }
    nub->setController(this, slot->handle, slot->readOnly);
    if (!nub->attach(this)) {
        nub->release();
        return false;
    }
    if (!nub->start(this)) {
        nub->detach(this);
        nub->release();
        return false;
    }
    slot->nub = nub;
    if (fBlockDev == NULL) fBlockDev = nub; /* legacy single-nub paths */
    nub->registerService();
    UInt32 index = (UInt32)(slot - fTargets);
    char name[40];
    snprintf(name, sizeof(name), "MPRTarget%uReadOnly", index);
    setProperty(name, slot->readOnly);
    snprintf(name, sizeof(name), "MPRTarget%uBlocks", index);
    setProperty(name, (unsigned long long)slot->capacityBlocks, 64);
    snprintf(name, sizeof(name), "MPRTarget%uBlockSize", index);
    setProperty(name, (unsigned long long)slot->blockSize, 32);
    snprintf(name, sizeof(name), "MPRTarget%uIdentity", index);
    setProperty(name, slot->identityValid ? slot->identity : "unknown");
    return true;
}

bool
MPRController::publishTargets(void)
{
    UInt32 published = 0;
    UInt16 defaultHandle = 0;
    for (UInt32 i = 0; i < fTargetCount && i < kMPRMaxTargets; i++) {
        MPRTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0) continue;
        /* The existing probe chain is read-only (TUR, INQUIRY, VPD, READ
         * CAPACITY, MODE SENSE, READ). Its controller-level state is serialised
         * by fIOLock and copied into the slot before the next target. */
        fAttachedDevHandle = slot->handle;
        fRequireSameIdentity = false;
        fTargetIdentityValid = false;
        fCapacityBlocks = 0;
        fBlockSize = 0;
        /* Identity strings are probed per target: SSP devices have no ATA
         * IDENTIFY, so leaving the previous SATA serial in place would make
         * the write rule match the wrong disk. */
        fTargetModel[0] = 0;
        fTargetSerial[0] = 0;
        /* The topology enumeration already captured this target's device
         * page fields. Reuse them instead of re-reading config pages while
         * the start path still owns the shared management SMID. */
        fTargetDeviceInfo = slot->deviceInfo;
        fTargetDeviceFlags = slot->flags;
        fTargetSasAddress = slot->sasAddress;
        fFastPathEnabled = slot->fastPath;
        const char *stage = NULL;
        if (!probeScsiIdentity()) stage = "scsi-identity";
        else if (!probeCapacity()) stage = "capacity";
        else if (!probeBlockRead()) stage = "block-read";
        else if (!prepareTargetMedia()) stage = "media";
        if (stage != NULL) {
            char name[48];
            snprintf(name, sizeof(name), "MPRTarget%uProbe", i);
            setProperty(name, "failed");
            snprintf(name, sizeof(name), "MPRTarget%uProbeStage", i);
            setProperty(name, stage);
            continue;
        }
        slot->capacityBlocks = fCapacityBlocks;
        slot->blockSize = fBlockSize;
        slot->identityValid = fTargetIdentityValid;
        strlcpy(slot->identity, fTargetIdentity, sizeof(slot->identity));
        strlcpy(slot->model, fTargetModel, sizeof(slot->model));
        strlcpy(slot->serial, fTargetSerial, sizeof(slot->serial));
        /* Only a SATA/STP disk with an explicit identity rule may be writable.
         * SSP targets such as the ZFS array member are always read-only, even
         * if a rule would otherwise match a stale identity. */
        bool sataTarget =
            (slot->deviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) != 0;
        slot->readOnly = !sataTarget ||
                         !isWritableIdentity(slot->model, slot->serial);
        slot->generation = ++fMediaGeneration;
        slot->present = true;
        if (!publishTarget(slot)) {
            slot->present = false;
            continue;
        }
        published++;
        if (!slot->readOnly && defaultHandle == 0) defaultHandle = slot->handle;
    }
    if (defaultHandle == 0) {
        for (UInt32 i = 0; i < fTargetCount && i < kMPRMaxTargets; i++)
            if (fTargets[i].nub != NULL) { defaultHandle = fTargets[i].handle; break; }
    }
    /* Synchronous cache/UNMAP helpers still use the controller default; point
     * it at a writable target when one exists, otherwise the first target. */
    fAttachedDevHandle = defaultHandle;
    fMediaPresent = published != 0;
    setProperty("MPRMediaPresent", fMediaPresent);
    setProperty("MPRPublishedTargetCount", published, 32);
    if (published != 0) {
        for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
            MPRTargetSlot *first = &fTargets[i];
            if (first->nub == NULL) continue;
            setProperty("MPRMediaBlocks", first->capacityBlocks, 64);
            setProperty("MPRMediaBlockSize", (unsigned long long)first->blockSize, 32);
            fCapacityBlocks = first->capacityBlocks;
            fBlockSize = first->blockSize;
            break;
        }
    }
    return published != 0;
}

bool
MPRController::probeCapacity(void)
{
    bool ownPage = (fPageBuffer == NULL);
    bool ok;

    if (fProtocolRecoveryPending || !allocatePageBuffer()) {
        setProperty("MPRCapacityProbeState", "failed-buffer-unavailable");
        return false;
    }
    ok = scsiReadCapacity16();
    if (ok) {
        setProperty("MPRCapacityKind", "rc16");
    } else if (!fProtocolRecoveryPending) {
        ok = scsiReadCapacity10();
        if (ok) {
            setProperty("MPRCapacityKind", "rc10");
        }
    }
    setProperty("MPRCapacityProbeState", ok ? "ok" : "failed");

    if (ownPage && !fProtocolRecoveryPending) {
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    return ok;
}

/* Target protocol implementation is in MPRTarget.cpp. */

bool
MPRController::readManufacturingPage0(void)
{
    IOByteCount segLength;
    UInt8      *frame;
    MPRConfigRequest *config;
    MPRConfigReply   *reply = NULL;
    MPRConfigPageMan0 *page;
    UInt32 value32;
    UInt32 baddr = 0;
    UInt8  replyCopy[64];
    bool   ok = false;
    UInt8 replyKind = 0;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_CONFIG_PAGE_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRConfigPageFailure", "page buffer allocation failed");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_CONFIG_PAGE_BYTES ||
        (fPagePhys + MPR_CONFIG_PAGE_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRConfigPageFailure", "page buffer is not low-4GB contiguous");
        return false;
    }
    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_CONFIG_PAGE_BYTES);

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        setProperty("MPRConfigPageFailure", "request frame unavailable");
        return false;
    }

    /* SMID 1 = frame 1. The request descriptor (RequestFlags/SMID) travels
     * through the 0xC0/0xC4 registers; the frame itself begins at the message
     * header because MPI2 request frames have no descriptor prefix
     * (mpr.c:1583-1587, mpr.c:3941). */
    if (!beginManagementRequest(1)) {
        return false;
    }
    frame += fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    config = (MPRConfigRequest *)frame;

    config->Function = MPI2_FUNCTION_CONFIG;
    config->Action = MPI2_CONFIG_ACTION_PAGE_READ_CURRENT;
    config->Header.PageType = MPI2_CONFIG_PAGETYPE_MANUFACTURING;
    config->Header.PageNumber = 0;
    config->Header.PageVersion = MPI2_MANUFACTURING0_PAGEVERSION;
    /* Manufacturing Page 0 is a fixed 0x4C-byte page; the request carries the
     * length in 32-bit words (mpr_config.c:141-142). */
    config->Header.PageLength = 19;
    config->PageAddress = 0;
    config->SgeFlagsLength =
        ((UInt32)(MPI2_SGE_FLAGS_SIMPLE_ELEMENT | MPI2_SGE_FLAGS_LAST_ELEMENT |
                  MPI2_SGE_FLAGS_END_OF_BUFFER | MPI2_SGE_FLAGS_64_BIT_ADDRESSING)
         << MPI2_SGE_FLAGS_SHIFT) |
        (76 & MPI2_SGE_LENGTH_MASK);
    config->SgeAddress = fPagePhys;

    /* Request descriptor: DEFAULT_TYPE plus SMID 1 in bits 16-31. */
    value32 = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE | (1U << 16);
    postDefaultRequest(1);

    {
        UInt64 fw[4];

        memcpy(fw, frame, sizeof(fw));
        setProperty("MPRConfigFrameW0", fw[0], 64);
        setProperty("MPRConfigFrameW1", fw[1], 64);
        setProperty("MPRConfigFrameW2", fw[2], 64);
        setProperty("MPRConfigFrameW3", fw[3], 64);
        setProperty("MPRConfigDescLow", (unsigned long long)value32, 32);
        setProperty("MPRConfigRequestPhys", fRequestPhys, 64);
        setProperty("MPRConfigPagePhys", fPagePhys, 64);
    }

    /* Consume only this config command's completion slot. */
    if (!finishManagementRequest(1, MPR_REPLY_TIMEOUT_MS, replyCopy, &replyKind)) {
        setProperty("MPRConfigPageFailure", "no reply descriptor received");
        return false;
    }
    reply = (MPRConfigReply *)replyCopy;
    if (replyKind != 1 || reply->Function != MPI2_FUNCTION_CONFIG ||
        reply->MsgLength * 4U < sizeof(MPRConfigReply)) {
        recordProtocolFault(kMPRProtocolManagementReply, 1);
        return false;
    }
    baddr = (UInt32)fReplyPhys;
    ok = true;

    if (ok && reply != NULL) {
        UInt64 w[4];

        memcpy(w, (const UInt8 *)reply, sizeof(w));
        setProperty("MPRConfigReplyW0", w[0], 64);
        setProperty("MPRConfigReplyW1", w[1], 64);
        setProperty("MPRConfigReplyW2", w[2], 64);
        setProperty("MPRConfigReplyW3", w[3], 64);
        setProperty("MPRConfigReplyBase", fReplyPhys, 64);
        setProperty("MPRConfigReplyAddr", (unsigned long long)baddr, 64);
        setProperty("MPRConfigHostIntStatus",
                    (unsigned long long)readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET), 32);
        setProperty("MPRConfigPostHostIndex",
                    (unsigned long long)readReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET), 32);
    }

    if (!ok || reply == NULL) {
        setProperty("MPRConfigPageFailure", "no reply descriptor received");
        return false;
    }

    setProperty("MPRConfigIOCStatus",
                (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
    setProperty("MPRConfigIOCLogInfo", (unsigned long long)reply->IOCLogInfo, 32);
    setProperty("MPRConfigPageType", (unsigned long long)reply->Header.PageType, 8);
    setProperty("MPRConfigPageNumber", (unsigned long long)reply->Header.PageNumber, 8);
    setProperty("MPRConfigPageLength", (unsigned long long)reply->Header.PageLength, 8);

    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        setProperty("MPRConfigPageFailure", "config request failed");
        return false;
    }
    if (reply->Header.PageType != MPI2_CONFIG_PAGETYPE_MANUFACTURING ||
        reply->Header.PageNumber != 0) {
        setProperty("MPRConfigPageFailure", "unexpected config page");
        return false;
    }

    page = (MPRConfigPageMan0 *)fPageBuffer->getBytesNoCopy();
    if (page == NULL) {
        setProperty("MPRConfigPageFailure", "page buffer has no virtual address");
        return false;
    }

    {
        char chip[17], revision[9], board[17], assembly[17], tracer[17];

        mprCopyField(chip, page->ChipName, sizeof(page->ChipName));
        mprCopyField(revision, page->ChipRevision, sizeof(page->ChipRevision));
        mprCopyField(board, page->BoardName, sizeof(page->BoardName));
        mprCopyField(assembly, page->BoardAssembly, sizeof(page->BoardAssembly));
        mprCopyField(tracer, page->BoardTracerNumber, sizeof(page->BoardTracerNumber));

        setProperty("MPRManPage0ChipName", chip);
        setProperty("MPRManPage0ChipRevision", revision);
        setProperty("MPRManPage0BoardName", board);
        setProperty("MPRManPage0BoardAssembly", assembly);
        setProperty("MPRManPage0BoardTracerNumber", tracer);
        IOLog("MPR ManPage0 chip=%s rev=%s board=%s\n", chip, revision, board);
    }

    return true;
}

bool
MPRController::readManufacturingInfo(void)
{
    bool ok;

    fReplyPostIndex = 0;
    ok = readManufacturingPage0();

    if (fPageBuffer != NULL && !fProtocolRecoveryPending) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    if (fPageBuffer == NULL) fPagePhys = 0;
    setProperty("MPRConfigPageState", ok ? "ok" : "failed");
    return ok;
}

bool
MPRController::init(OSDictionary *properties)
{
    fAsync = NULL;
    fMaxChainDepth = fChainFrameBytes = 0;
    fPowerQuiesced = false;
    fIOLock = NULL;
    fReplyLock = NULL;
    fPostLock = NULL;
    fIocCapabilities = fMaxDeviceHandle = fTargetDeviceInfo = 0;
    fTargetDeviceFlags = 0;
    fTargetSasAddress = 0;
    fHasExpander = false;
    fDiscoveredDiskCount = 0;
    fNativeFuaSupported = false;
    fUnmapLimits = MPRUnmapLimits{};
    fTargetIdentityValid = fMediaIdentityValid = fRequireSameIdentity = false;
    fTargetIdentity[0] = fMediaIdentity[0] = '\0';
    fIdentityCapacity = 0;
    fIdentityBlockSize = 0;
    fRemovedTargetHandle = 0;
    fRemovalGeneration = 0;
    fTargetRemovalRequested = false;
    fPhysicalChangePending = false;
    return super::init(properties);
}

void
MPRController::free(void)
{
    destroyAsyncState();
    /* stop() leaves these locks alive for already queued callers to wake,
     * observe fStopping and return. The service's final release owns them. */
    if (fIOLock != NULL) {
        IOLockFree(fIOLock);
        fIOLock = NULL;
    }
    if (fReplyLock != NULL) {
        IOSimpleLockFree(fReplyLock);
        fReplyLock = NULL;
    }
    if (fPostLock != NULL) {
        IOSimpleLockFree(fPostLock);
        fPostLock = NULL;
    }
    super::free();
}

bool
MPRController::start(IOService *provider)
{
    IOPCIDevice *pci;
    IOByteCount  length;

    if (!super::start(provider)) {
        return false;
    }

    /* OSObject instances are not guaranteed zero-filled; every member used
     * during bring-up is initialized here so a failure cannot read garbage. */
    fMMIO = NULL;
    fRegs = NULL;
    fQueueBuffer = NULL;
    fReplyBuffer = NULL;
    fRequestBuffer = NULL;
    fPageBuffer = NULL;
    fSenseBuffer = NULL;
    fSensePhys = 0;
    fDataBuffer = NULL;
    fFreePhys = fPostPhys = fReplyPhys = fRequestPhys = fPagePhys = 0;
    fDataPhys = 0;
    fDataBytes = 0;
    fFqDepth = fPqDepth = fRequestCount = fReplyFreeIndex = 0;
    fRequestFrameSize = fReplyFrameSize = fRequestCredit = fMaxReplyDepth = 0;
    fReplyPostIndex = 0;
    fFactsValid = false;
    fWorkLoop = NULL;
    fInterrupt = NULL;
    fReplyCount = 0;
    fInterruptCount = 0;
    fReplyViaIrq = 0;
    fEventCount = 0;
    fLastEvent = 0;
    fLastEventDataLength = 0;
    fLastEventAckRequired = 0;
    fLastEventContext = 0;
    fEventSeen[0] = fEventSeen[1] = fEventSeen[2] = fEventSeen[3] = 0;
    fAttachedDevHandle = 0;
    fLastReplySmid = 0xFFFF;
    fLastReplyKind = 0;
    fScsiSuccess = 0;
    fBlockDev = NULL;
    fIOLock = IOLockAlloc();
    fReplyLock = IOSimpleLockAlloc();
    fPostLock = IOSimpleLockAlloc();
    memset(fManagementBusy, 0, sizeof(fManagementBusy));
    memset(fTmfBusy, 0, sizeof(fTmfBusy));
    memset(fEventAckQueue, 0, sizeof(fEventAckQueue));
    memset(fCommandOwners, 0, sizeof(fCommandOwners));
    fTransportEpoch = 1;
    fUnexpectedCompletionCount = 0;
    fEventAckHead = fEventAckQueued = fEventAckAge = 0;
    fEventAckBusy = false;
    fEventAckCompleted = fEventAckFailed = fEventAckOverflow = 0;
    fProtocolRecoveryPending = false;
    fProtocolFaultCode = fProtocolFaultSmid = 0;
    fStickyFaultCode = fStickyFaultSmid = fStickyFaultCount = 0;
    fLastTopoHandle = fLastTopoReason = fLastTopoRate = 0;
    fTopoRemovalHits = 0;
    fBadReplySite = fBadReplyValue = fBadReplySmid = fBadReplyCount = 0;
    fBadReplyFunction = fBadReplyMsgLen = fBadReplyWord = 0;
    fLinkUpPhys = 0;
    fWriteCount = 0;
    fWriteBytes = 0;
    fLastWriteLba = 0;
    fLastWriteSmid = 0;
    fSasDeviceMismatch = false;
    fIdentityLostCount = 0;
    fTopologyGeneration = 0;
    fRemovalPending = false;
    fHotplugRetryRequested = false;
    memset(fReplySlots, 0, sizeof(fReplySlots));
    fCapacityBlocks = 0;
    fBlockSize = 0;
    fNextIoSmid = 0;
    fIoCount = 0;
    fIoRetryCount = 0;
    fFrozenSmid = 0;
    fTimedOutSmid = 0;
    fUnmapEnabled = false;
    fReinitInProgress = false;
    fCommandCount = 0;
    fTmfCount = 0;
    fResetCount = 0;
    fLastSenseKey = 0xFF;
    fLastASC = 0;
    fLastASCQ = 0;
    fDeviceOffline = false;
    fMediaPresent = false;
    fMediaGeneration = 0;
    fStarting = true;
    fMediaOfflinePending = false;
    fMediaOnlinePending = false;
    fHotplugAttempts = 0;
    fStopping = false;
    fTopologyChanged = false;
    fWatchdogTimer = NULL;
    fHotplugCall = NULL;
    fHotplugPending = false;
    fHotplugRunning = false;
    /* A startup gate, not a blind time window, prevents concurrent probes. */
    fHotplugCooldown = 0;
    fInjectTimeout = false;
    fInjectTmfFail = 0;
    fInjectEvery = 0;
    fInjectCounter = 0;
    fInjectCount = 0;
    fFastPathEnabled = false;
    fSuspended = false;
    fHighPriorityCredit = 0;
    fEventReplyCount = 0;
    fEventAckCount = 0;
    fReplyFrameReturned = 0;
    memset(fVendor, 0, sizeof(fVendor));
    memset(fProduct, 0, sizeof(fProduct));
    memset(fRevision, 0, sizeof(fRevision));
    memset(fLastReply, 0, sizeof(fLastReply));
    memset(fLastEventData, 0, sizeof(fLastEventData));

    if (fIOLock == NULL || fReplyLock == NULL || fPostLock == NULL) {
        if (fIOLock != NULL) IOLockFree(fIOLock);
        if (fReplyLock != NULL) IOSimpleLockFree(fReplyLock);
        if (fPostLock != NULL) IOSimpleLockFree(fPostLock);
        fIOLock = NULL;
        fReplyLock = fPostLock = NULL;
        super::stop(provider);
        return false;
    }

    pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == NULL) {
        super::stop(provider);
        return false;
    }
    fPCIDevice = pci;

    if (!pci->setMemoryEnable(true) || !pci->setBusMasterEnable(true)) {
        IOLog("MPR: failed to enable memory decode / bus master\n");
        super::stop(provider);
        return false;
    }

    fMMIO = pci->mapDeviceMemoryWithRegister(MPR_MMIO_BAR_REG);
    if (fMMIO == NULL) {
        IOLog("MPR: BAR 0x%02x mapping failed\n", MPR_MMIO_BAR_REG);
        super::stop(provider);
        return false;
    }

    length = fMMIO->getLength();
    fRegs = (volatile UInt32 *)fMMIO->getVirtualAddress();

    setProperty("MPRMMIOBAR", "0x14");
    setProperty("MPRMMIOLength", (unsigned long long)length, 64);
    setProperty("MPRPCIID", (unsigned long long)pci->configRead32(0), 32);
    setProperty("MPRStage", "read-write");
    setProperty("MPRBuild", "0.19.0");
    setProperty("MPRFeatures",
                "async-pool,ieee-sgl-chain,read-write16,cache16,unmap-limits,"
                "stable-identity,raid-filter,auto-sense,tmf,remove-device,pm-quiesce");
    {
        /* Opt-in fault injection for the timeout/TMF path. The kernel ignores
         * unknown boot-args, so this is inert unless explicitly requested. */
        int inject = 0;
        int tmfFail = 0;

        if (PE_parse_boot_argn("mpr_timeout_test", &inject, sizeof(inject))) {
            fInjectTimeout = (inject != 0);
        }
        if (PE_parse_boot_argn("mpr_tmf_fail", &tmfFail, sizeof(tmfFail)) &&
            tmfFail > 0) {
            fInjectTmfFail = (UInt32)tmfFail;
        }
        /* Repeatable variant for a soak: every Nth runtime command loses its
         * reply, which drives the timeout -> TMF -> reset chain over and over. */
        if (PE_parse_boot_argn("mpr_timeout_every", &tmfFail, sizeof(tmfFail)) &&
            tmfFail > 1) {
            fInjectEvery = (UInt32)tmfFail;
        }
        setProperty("MPRInjectTimeout",
                    (unsigned long long)(fInjectTimeout ? 1 : 0), 8);
        setProperty("MPRInjectTmfFail",
                    (unsigned long long)fInjectTmfFail, 8);
        setProperty("MPRInjectEvery", (unsigned long long)fInjectEvery, 32);
    }

    /* Failures during bring-up are recorded in IORegistry and never panic
     * the machine or prevent the kext from loading. Config-page reads
     * poll the reply queue and take no interrupts. */
    bool interruptsReady = setupInterrupts();
    if (fWorkLoop != NULL) {
        fWatchdogTimer = IOTimerEventSource::timerEventSource(this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                 &MPRController::watchdogTimeout));
        if (fWatchdogTimer != NULL &&
            fWorkLoop->addEventSource(fWatchdogTimer) == kIOReturnSuccess) {
            fWatchdogTimer->setTimeoutMS(1000);
            setProperty("MPRWatchdogState", "armed");
        } else {
            setProperty("MPRWatchdogState", "arm-failed");
        }
    } else {
        setProperty("MPRWatchdogState", "no-workloop");
    }
    fHotplugCall = thread_call_allocate(hotplugCall, this);
    if (!initializeAsync()) {
        setProperty("MPRAsyncState", "allocation-failed");
        stop(provider);
        return false;
    }
    if (initializeController() && initializeQueues()) {
        readManufacturingInfo();
        bool eventsReady = interruptsReady && enableEventsAndPort();
        if (!eventsReady) {
            recordProtocolFault(interruptsReady ? kMPRProtocolManagementReply :
                                kMPRProtocolInterruptUnavailable, 0);
        }
        /* A disk that is readable only through a timeout's fallback poll
         * cannot support idle hotplug. IRQ, subscription and PortEnable
         * success are mandatory before publishing a media object. */
        loadWritePolicy();
        if (eventsReady && discoverTopology() && fTargetCount != 0) {
            if (!publishTargets()) fMediaPresent = false;
        }
    } else {
        setProperty("MPRConfigPageState", "skipped-earlier-stage-failed");
        recordProtocolFault(kMPRProtocolManagementTimeout, 0);
    }

    fStarting = false;
    if (!fMediaPresent && fReplyLock != NULL) {
        IOSimpleLockLock(fReplyLock);
        fHotplugPending = true;
        IOSimpleLockUnlock(fReplyLock);
    }

    setProperty("MPRIntCount", (unsigned long long)fInterruptCount, 32);
    setProperty("MPRReplyViaIrq", (unsigned long long)fReplyViaIrq, 32);
    setProperty("MPREventCount", (unsigned long long)fEventCount, 32);
    setProperty("MPRLastEvent", (unsigned long long)fLastEvent, 16);
    /* Join the power tree only once the controller is usable, so a sleep
     * arriving during bring-up cannot race the discovery path. */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, gMPRPowerStates, kMPRPowerStateCount);
    /* Keep the controller's normal operating vote at On. Provider/system
     * sleep can still remove power; an idle/default vote must not reset a
     * just-published disk immediately after registration. */
    changePowerStateTo(kMPRPowerOn);
    setProperty("MPRPowerState", "on");
    IOLog("MPR start complete, irq=%u replyViaIrq=%u events=%u last=0x%04x\n",
          (unsigned int)fInterruptCount, (unsigned int)fReplyViaIrq,
          (unsigned int)fEventCount, (unsigned int)fLastEvent);
    return true;
}

void
MPRController::stop(IOService *provider)
{
    MPRBlockDev *nub;
    bool dmaStopped = false;

    fStopping = true;
    stopAsync();
    if (fWatchdogTimer != NULL) {
        fWatchdogTimer->cancelTimeout();
        if (fWorkLoop != NULL) {
            fWorkLoop->removeEventSource(fWatchdogTimer);
        }
        fWatchdogTimer->release();
        fWatchdogTimer = NULL;
    }
    if (fHotplugCall != NULL) {
        /* The callback may be waiting for fIOLock. Never hold that lock
         * while waiting for the callback to finish. */
        thread_call_cancel_wait(fHotplugCall);
        thread_call_free(fHotplugCall);
        fHotplugCall = NULL;
    }
    if (fIOLock != NULL) {
        IOLockLock(fIOLock);
    }
    fMediaPresent = false;
    fDeviceOffline = true;
    fMediaGeneration++;
    nub = fBlockDev;
    fBlockDev = NULL;
    if (nub != NULL) {
        nub->setController(NULL);
    }
    for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
        if (fTargets[i].nub != NULL) {
            fTargets[i].nub->setController(NULL);
            fTargets[i].nub = NULL;
        }
        fTargets[i].present = false;
    }

    /* No command submitter remains. Stop the reply consumer and the IOC
     * before releasing any host address previously supplied for DMA. */
    teardownInterrupts();
    if (fRegs != NULL) {
        dmaStopped = diagReset();
    }
    if (fPCIDevice != NULL) {
        fPCIDevice->setBusMasterEnable(false);
        UInt16 command = fPCIDevice->configRead16(kIOPCIConfigCommand);
        if (command != 0xFFFF && (command & kIOPCICommandBusMaster) == 0) {
            dmaStopped = true;
        }
    }
    if (dmaStopped) {
        resetProtocolState();
        if (fDataBuffer != NULL) {
            fDataBuffer->release();
            fDataBuffer = NULL;
        }
        if (fPageBuffer != NULL) {
            fPageBuffer->release();
            fPageBuffer = NULL;
        }
        fPagePhys = fDataPhys = 0;
        fDataBytes = 0;
        freeQueueMemory();
        setProperty("MPRStopDMAState", "stopped");
    } else if (fQueueBuffer != NULL || fReplyBuffer != NULL ||
               fRequestBuffer != NULL || fPageBuffer != NULL ||
                fDataBuffer != NULL || fSenseBuffer != NULL) {
        /* A bounded leak is safer than DMA into reallocated kernel memory.
         * Do not release these descriptors later without hardware proof. */
        setProperty("MPRStopDMAState", "buffers-retained");
        IOLog("MPR stop: DMA quiesce unconfirmed; buffers retained\n");
    }
    if (fMMIO != NULL) {
        fMMIO->release();
        fMMIO = NULL;
    }
    fRegs = NULL;
    fPCIDevice = NULL;
    fCapacityBlocks = 0;
    fBlockSize = 0;
    if (fIOLock != NULL) {
        IOLockUnlock(fIOLock);
    }
    releaseAsync(dmaStopped);

    /* Client termination can synchronously re-enter the device. The offline
     * gate is already closed and no controller lock is held here. IOKit
     * owns the eventual detach; do not detach a still-terminating child. */
    if (nub != NULL) {
        nub->terminate();
        nub->release();
    }
    if (fWorkLoop != NULL) {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    /* Leave the power tree only after the hardware is quiesced: PMstop()
     * waits for an in-flight power transition to finish. */
    PMstop();
    super::stop(provider);
}

/* Sleep/shutdown must quiesce all accepted work before the PCI provider
 * removes power. setPowerState runs on the PM thread, not the reply workloop. */
bool
MPRController::quiescePower(void)
{
    if (fSuspended) return fPowerQuiesced;
    setAsyncAcceptance(false);
    IOReturn fence = beginAsyncBarrier();
    if (fence != kIOReturnSuccess) return false;
    IOLockLock(fIOLock);
    IOReturn flush = kIOReturnSuccess;
    if (fMediaPresent && !fDeviceOffline && !fProtocolRecoveryPending &&
        !mediaRemovalPending()) flush = scsiSynchronizeCache(0, 0, 29, true);
    setProperty("MPRPowerFlushStatus", (unsigned long long)(UInt32)flush, 32);
    fSuspended = true;
    teardownInterrupts();
    bool stopped = fRegs != NULL && diagReset();
    if (fPCIDevice != NULL) {
        fPCIDevice->setBusMasterEnable(false);
        UInt16 command = fPCIDevice->configRead16(kIOPCIConfigCommand);
        if (command != 0xFFFF && (command & kIOPCICommandBusMaster) == 0)
            stopped = true;
    }
    if (stopped) resetProtocolState();
    fPowerQuiesced = stopped;
    setProperty("MPRPowerState", stopped ? "quiesced" : "quiesce-failed");
    IOLockUnlock(fIOLock);
    endAsyncBarrier();
    return stopped;
}

bool
MPRController::resumePower(void)
{
    if (!fSuspended) return true;
    IOLockLock(fIOLock);
    const bool published = fMediaPresent;
    const UInt64 oldCapacity = fIdentityCapacity;
    const UInt32 oldBlockSize = fIdentityBlockSize;
    char oldIdentity[sizeof(fMediaIdentity)];
    strlcpy(oldIdentity, fMediaIdentity, sizeof(oldIdentity));
    bool ready = !fStopping && fPowerQuiesced && fPCIDevice != NULL &&
        fPCIDevice->setMemoryEnable(true) && fPCIDevice->setBusMasterEnable(true);
    if (ready) ready = resetAndReinitialize();
    IOSimpleLockLock(fReplyLock);
    bool replacement = fRemovalPending || fPhysicalChangePending;
    IOSimpleLockUnlock(fReplyLock);
    /* A disk inserted while asleep must take OFFLINE -> ONLINE, even if
     * reset discovery itself succeeded. Never attach it to the old medium. */
    if (ready && (!published || replacement || !fMediaIdentityValid ||
        strcmp(oldIdentity, fMediaIdentity) != 0 ||
        oldCapacity != fIdentityCapacity || oldBlockSize != fIdentityBlockSize))
        ready = false;
    fSuspended = false;
    fPowerQuiesced = false;
    if (!ready) {
        invalidateMedia();
        IOSimpleLockLock(fReplyLock);
        fHotplugPending = true;
        IOSimpleLockUnlock(fReplyLock);
    }
    setProperty("MPRPowerState", ready ? "on" : "wake-recovery-pending");
    IOLockUnlock(fIOLock);
    setAsyncAcceptance(true);
    return ready;
}

IOReturn
MPRController::setPowerState(unsigned long stateNumber, IOService *whatDevice)
{
    (void)whatDevice;
    if (!fIOLock || fStarting || fStopping) return kIOPMAckImplied;
    if (stateNumber == kMPRPowerOff) quiescePower();
    else if (stateNumber == kMPRPowerOn) resumePower();
    return kIOPMAckImplied;
}

void
MPRController::systemWillShutdown(IOOptionBits specifier)
{
    if (!fStopping && fIOLock != NULL) quiescePower();
    super::systemWillShutdown(specifier);
}

/* ------------------------------------------------------------------------ */
/* write, SYNCHRONIZE CACHE, UNMAP, auto sense, TMF and diag reset.      */
/* SMID map: 1..4 config/events, 5..9 startup probes, 10..25 runtime IO,    */
/* 26..28 task management, 29 sync cache, 30 UNMAP.                         */

#define MPR_SM_TMF_BASE         26
#define MPR_SM_SYNC_CACHE       29
#define MPR_SM_UNMAP            30
#define MPR_MAX_UNMAP_EXTENTS   32

IOReturn
MPRController::readBlocks(IOMemoryDescriptor *buffer, UInt64 block,
                          UInt64 nblks, UInt64 *actualBytes)
{
    return transferBlocks(false, buffer, block, nblks, false, actualBytes);
}

void
MPRController::postDefaultRequest(UInt32 smid)
{
    /* Config and EventAck messages use the default descriptor type;
     * only SCSI IO uses the fast-path descriptor (see postScsiIo). */
    /* ACK submission is already inside the reply lock. */
    if (smid == MPR_SM_EVENT_ACK) {
        postOwnedRequestLocked(smid, MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0);
    } else {
        IOSimpleLockLock(fReplyLock);
        bool posted = postOwnedRequestLocked(smid, MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0);
        if (!posted) recordProtocolFaultLocked(kMPRProtocolManagementBusy, smid);
        IOSimpleLockUnlock(fReplyLock);
    }
}

void
MPRController::postHighPriorityRequest(UInt32 smid)
{
    /* FreeBSD allocates TM requests from the high-priority pool
     * (mpr_sas.c:224-236); the descriptor type is 0x06, not 0x08. */
    IOSimpleLockLock(fReplyLock);
    bool posted = postOwnedRequestLocked(smid, MPI2_REQ_DESCRIPT_FLAGS_HIGH_PRIORITY, 0);
    if (!posted) recordProtocolFaultLocked(kMPRProtocolManagementBusy, smid);
    IOSimpleLockUnlock(fReplyLock);
}

void
MPRController::releaseReplyFrame(UInt32 baddr)
{
    UInt32 *freeQueue;
    UInt32  offset;

    if (fQueueBuffer == NULL || fFqDepth == 0 || fReplyFrameSize == 0 ||
        fReplyPhys == 0) {
        return;
    }
    if (baddr < (UInt32)fReplyPhys ||
        baddr >= (UInt32)fReplyPhys + fReplyFrameSize * fFqDepth) {
        return;
    }
    offset = baddr - (UInt32)fReplyPhys;
    if ((offset % fReplyFrameSize) != 0) {
        return;
    }
    freeQueue = (UInt32 *)fQueueBuffer->getBytesNoCopy();
    if (freeQueue == NULL) {
        return;
    }
    /* Mirrors mpr_free_reply(): pre-increment the index, write the frame
     * address back, then publish the new host index. */
    fReplyFreeIndex = (fReplyFreeIndex + 1) % fFqDepth;
    freeQueue[fReplyFreeIndex] = baddr;
    writeReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET, fReplyFreeIndex);
    fReplyFrameReturned++;
}

void
MPRController::sendEventAck(UInt16 event, UInt32 context)
{
    /* Called only while fReplyLock is held. Keep each context until its
     * command completes; never overwrite an IOC-owned request frame. */
    if (fEventAckQueued == kMPREventAckQueueDepth) {
        fEventAckOverflow++;
        recordProtocolFaultLocked(kMPRProtocolAckOverflow, MPR_SM_EVENT_ACK);
        return;
    }
    UInt32 tail = (fEventAckHead + fEventAckQueued) % kMPREventAckQueueDepth;
    fEventAckQueue[tail].event = event;
    fEventAckQueue[tail].context = context;
    fEventAckQueued++;
    postNextEventAckLocked();
}

void
MPRController::postNextEventAckLocked(void)
{
    if (fEventAckBusy || fEventAckQueued == 0 || fRequestBuffer == NULL ||
        fProtocolRecoveryPending || fStopping) {
        return;
    }
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL || MPR_SM_EVENT_ACK >= fRequestCount) {
        recordProtocolFaultLocked(kMPRProtocolAckReply, MPR_SM_EVENT_ACK);
        return;
    }
    if (fCommandOwners[MPR_SM_EVENT_ACK].state != kMPRCommandFree) {
        recordProtocolFaultLocked(kMPRProtocolManagementBusy, MPR_SM_EVENT_ACK);
        return;
    }
    fCommandOwners[MPR_SM_EVENT_ACK].state = kMPRCommandReserved;
    frame += MPR_SM_EVENT_ACK * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPREventAckRequest *request = (MPREventAckRequest *)frame;
    request->Function = MPI2_FUNCTION_EVENT_ACK;
    request->Event = fEventAckQueue[fEventAckHead].event;
    request->EventContext = fEventAckQueue[fEventAckHead].context;
    fEventAckBusy = true;
    fEventAckAge = 0;
    fEventAckCount++;
    postDefaultRequest(MPR_SM_EVENT_ACK);
}

void
MPRController::completeEventAckLocked(const UInt8 *reply, UInt32 replyBytes)
{
    UInt16 status = replyBytes >= 16
        ? (UInt16)((UInt16)reply[14] | ((UInt16)reply[15] << 8)) : 0xFFFF;
    if (!fEventAckBusy || fEventAckQueued == 0 || replyBytes < 20 ||
        fCommandOwners[MPR_SM_EVENT_ACK].state != kMPRCommandPosted ||
        reply[3] != MPI2_FUNCTION_EVENT_ACK ||
        (status & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        fEventAckFailed++;
        recordProtocolFaultLocked(kMPRProtocolAckReply, MPR_SM_EVENT_ACK);
        return;
    }
    fEventAckBusy = false;
    fCommandOwners[MPR_SM_EVENT_ACK].state = kMPRCommandFree;
    fEventAckAge = 0;
    fEventAckHead = (fEventAckHead + 1) % kMPREventAckQueueDepth;
    fEventAckQueued--;
    fEventAckCompleted++;
    postNextEventAckLocked();
}

IOReturn
MPRController::decodeSense(UInt32 smid, UInt32 senseCount, IOReturn fallback,
                            bool *unitAttention)
{
    if (unitAttention != NULL) *unitAttention = false;
    if (smid == 0 || smid >= MPR_REQUEST_FRAMES || fSenseBuffer == NULL ||
        senseCount > MPR_SENSE_BYTES) return fallback;
    const UInt8 *base = (const UInt8 *)fSenseBuffer->getBytesNoCopy();
    if (base == NULL) return fallback;
    const UInt8 *sense = base + (UInt64)smid * MPR_SENSE_BYTES;
    MPRSenseFields fields;
    if (!mprDecodeSenseFields(sense, senseCount, &fields)) return fallback;
    fLastSenseKey = fields.key;
    fLastASC = fields.asc;
    fLastASCQ = fields.ascq;
    setProperty("MPRSenseKey", (unsigned long long)fields.key, 8);
    setProperty("MPRSenseASC", (unsigned long long)fields.asc, 8);
    setProperty("MPRSenseASCQ", (unsigned long long)fields.ascq, 8);
    UInt8 response = sense[0] & 0x7f;
    if (unitAttention != NULL)
        *unitAttention = fields.key == MPR_SENSE_KEY_UNIT_ATTENTION &&
                         (response == 0x70 || response == 0x72);
    switch (fields.key) {
        case MPR_SENSE_KEY_NO_SENSE:
        case MPR_SENSE_KEY_RECOVERED_ERROR: return kIOReturnSuccess;
        case MPR_SENSE_KEY_NOT_READY:
        case MPR_SENSE_KEY_UNIT_ATTENTION: return kIOReturnNotReady;
        case MPR_SENSE_KEY_MEDIUM_ERROR:
        case MPR_SENSE_KEY_BLANK_CHECK: return kIOReturnBadMedia;
        case MPR_SENSE_KEY_HARDWARE_ERROR: return kIOReturnDeviceError;
        case MPR_SENSE_KEY_ILLEGAL_REQUEST: return kIOReturnUnsupported;
        case MPR_SENSE_KEY_DATA_PROTECT: return kIOReturnNotWritable;
        case MPR_SENSE_KEY_ABORTED_COMMAND: return kIOReturnAborted;
        default: return fallback;
    }
}

bool
MPRController::validateScsiCompletion(UInt32 smid, UInt8 kind,
                                      const UInt8 *replyBuf, UInt32 dataLen,
                                      bool allowShort, IOReturn *status,
                                      UInt32 *actualBytes, bool *unitAttention)
{
    IOReturn result = kIOReturnIOError;
    UInt32 transferred = 0;
    if (actualBytes != NULL) *actualBytes = 0;
    if (unitAttention != NULL) *unitAttention = false;
    if (status != NULL) *status = result;
    if (kind == 3) {
        if (status != NULL) *status = kIOReturnAborted;
        return false;
    }
    if (smid == 0 || smid >= MPR_REQUEST_FRAMES) return false;
    if (kind == 2) {
        result = kIOReturnSuccess;
        transferred = dataLen;
    } else {
        if (kind != 1 || replyBuf == NULL ||
            replyBuf[3] != MPI2_FUNCTION_SCSI_IO_REQUEST ||
            replyBuf[2] * 4U < MPR_SCSIIO_REPLY_MIN_BYTES) {
            recordProtocolFault(kMPRProtocolManagementReply, smid);
            return false;
        }
        const MPRScsiIoReply *reply = (const MPRScsiIoReply *)replyBuf;
        const UInt16 ioc = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
        transferred = reply->TransferCount;
        setProperty("MPRLastIOSmid", (unsigned long long)smid, 16);
        setProperty("MPRLastIOIOCStatus", (unsigned long long)ioc, 16);
        setProperty("MPRLastIOSCSIStatus", (unsigned long long)reply->SCSIStatus, 8);
        setProperty("MPRLastIOTransferCount", (unsigned long long)transferred, 32);
        setProperty("MPRLastIOSCSIState", (unsigned long long)reply->SCSIState, 8);
        setProperty("MPRLastIOSenseCount", (unsigned long long)reply->SenseCount, 32);
        if (reply->DevHandle != fCommandOwners[smid].handle) {
            recordProtocolFault(kMPRProtocolUnexpectedCompletion, smid);
            result = kIOReturnDeviceError;
        } else if (transferred > dataLen) {
            result = kIOReturnOverrun;
        } else if (reply->SenseCount > MPR_SENSE_BYTES ||
                   (reply->SCSIState & (MPI2_SCSI_STATE_NO_SCSI_STATUS |
                                       MPI2_SCSI_STATE_AUTOSENSE_FAILED)) != 0) {
            result = kIOReturnDeviceError;
        } else if ((reply->SCSIState & MPI2_SCSI_STATE_TERMINATED) != 0) {
            result = kIOReturnAborted;
        } else if ((reply->SCSIState & MPI2_SCSI_STATE_RESPONSE_INFO_VALID) != 0 &&
                   (reply->ResponseInfo & MPI2_SCSI_RI_MASK_REASONCODE) != 0) {
            result = kIOReturnDeviceError;
        } else if (ioc != MPI2_IOCSTATUS_SUCCESS &&
                   ioc != MPI2_IOCSTATUS_SCSI_RECOVERED_ERROR &&
                   ioc != MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN) {
            result = ioc == MPI2_IOCSTATUS_SCSI_DATA_OVERRUN
                ? kIOReturnOverrun : kIOReturnDeviceError;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_CHECK_CONDITION) {
            result = (reply->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_VALID)
                ? decodeSense(smid, reply->SenseCount, kIOReturnDeviceError,
                              unitAttention) : kIOReturnDeviceError;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_BUSY ||
                   reply->SCSIStatus == MPI2_SCSI_STATUS_TASK_SET_FULL ||
                   reply->SCSIStatus == MPI2_SCSI_STATUS_RESERVATION_CONFLICT) {
            result = kIOReturnBusy;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_TASK_ABORTED) {
            result = kIOReturnAborted;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_GOOD) {
            result = kIOReturnSuccess;
        }
        /* An underrun can carry CHECK CONDITION (for example UNIT ATTENTION
         * or ILLEGAL REQUEST), so decode its sense before applying the exact
         * length rule. Only explicit short-read probes may succeed short. */
        if (result == kIOReturnSuccess && !allowShort &&
            (transferred != dataLen || ioc == MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN))
            result = kIOReturnUnderrun;
    }
    if (actualBytes != NULL && transferred <= dataLen) *actualBytes = transferred;
    if (status != NULL) *status = result;
    setProperty("MPRLastCommandStatus", (unsigned long long)(UInt32)result, 32);
    return result == kIOReturnSuccess;
}

bool
MPRController::issueScsiCommand(const UInt8 *cdb, UInt32 cdbLen,
                                UInt32 control, const UInt8 *outData,
                                UInt32 dataLen, UInt32 smid,
                                IOReturn *status, UInt32 *actualBytes,
                                bool allowShort, bool *unitAttention)
{
    UInt8 *frame, *staging;
    MPRScsiIoRequest *request;
    bool posted;
    UInt8 replyBuf[64], replyKind = 0;

    if (status != NULL) *status = kIOReturnNotReady;
    if (actualBytes != NULL) *actualBytes = 0;
    if (unitAttention != NULL) *unitAttention = false;
    if ((fDeviceOffline && !fReinitInProgress) || fStopping ||
        fProtocolRecoveryPending) {
        if (status != NULL) {
            *status = kIOReturnDeviceError;
        }
        return false;
    }
    if (fRequestBuffer == NULL || fPageBuffer == NULL || fDataBuffer == NULL ||
        cdb == NULL || cdbLen == 0 || cdbLen > 32 || smid == 0 ||
        smid >= fRequestCount || smid >= 128 || smid == MPR_SM_EVENT_ACK ||
        fReplyLock == NULL || dataLen > fDataBytes ||
        (allowShort && control != MPI2_SCSIIO_CONTROL_READ) ||
        fAttachedDevHandle == 0) {
        return false;
    }
    if (fFrozenSmid != 0 && fFrozenSmid == (UInt16)smid) {
        if (status != NULL) {
            *status = kIOReturnAborted;
        }
        return false;
    }

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    staging = (UInt8 *)fDataBuffer->getBytesNoCopy();
    if (frame == NULL || staging == NULL) {
        return false;
    }
    if (smid < 10) {
        if (!beginManagementRequest(smid)) {
            return false;
        }
    } else if (commandDmaSafe(smid)) {
        /* Runtime management callers are serialized by fIOLock. No waiter
         * from this caller exists yet, so retire an orphaned late completion. */
        UInt8 oldReply[64], oldKind = 0;
        takeReplySlot(smid, oldReply, sizeof(oldReply), &oldKind);
    }
    if (!prepareScsiSense(smid)) {
        cancelUnpostedCommand(smid);
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
    request->SenseBufferLength = MPR_SENSE_BYTES;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)(cdbLen | (fFastPathEnabled
                                          ? MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
    request->Control = control;
    memcpy(request->CDB, cdb, cdbLen);

    if (dataLen != 0) {
        if (outData != NULL) {
            memcpy(staging, outData, dataLen);
        } else {
            memset(staging, 0, dataLen);
        }
        mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fDataPhys, dataLen);
    } else {
        /* A zero-length IEEE SGE is still required for a data-less command. */
        mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 0);
    }

    fCommandCount++;
    if (!postScsiIo(smid)) {
        if (status != NULL) *status = kIOReturnNoMedia;
        return false;
    }
    posted = smid < 10
        ? finishManagementRequest(smid, 15000, replyBuf, &replyKind)
        : (waitForSmid(15000, smid) &&
           takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind));
    if (!posted) {
        if (status != NULL) {
            *status = kIOReturnTimeout;
        }
        if (smid >= 10 && fTimedOutSmid == (UInt16)smid) {
            if (fReinitInProgress || fStarting) {
                recordProtocolFault(kMPRProtocolManagementTimeout, smid);
            } else {
                recoverTimedOutCommand(smid);
            }
        }
        if (smid >= 10 && commandDmaSafe(smid))
            takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind);
        return false;
    }
    return validateScsiCompletion(smid, replyKind, replyBuf, dataLen,
                                  allowShort, status, actualBytes, unitAttention);
}

/* Target protocol implementation is in MPRTarget.cpp. */

/* Target protocol implementation is in MPRTarget.cpp. */

/* Target protocol implementation is in MPRTarget.cpp. */

bool
MPRController::sendTaskMgmt(UInt8 taskType, UInt16 taskMid, UInt32 smid)
{
    if (fRequestBuffer == NULL || fAttachedDevHandle == 0 ||
        smid < MPR_SM_TMF_BASE || smid >= MPR_SM_TMF_BASE + 3 ||
        smid >= fRequestCount || fReplyLock == NULL || fProtocolRecoveryPending)
        return false;
    UInt8 *base = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (base == NULL) return false;
    IOSimpleLockLock(fReplyLock);
    UInt32 outstanding = 0;
    for (UInt32 i = 0; i < 3; ++i) if (fTmfBusy[i]) ++outstanding;
    bool available = outstanding < fHighPriorityCredit &&
        !fTmfBusy[smid - MPR_SM_TMF_BASE] &&
        fCommandOwners[smid].state == kMPRCommandFree;
    if (available) {
        fTmfBusy[smid - MPR_SM_TMF_BASE] = true;
        fCommandOwners[smid].state = kMPRCommandReserved;
        fCommandOwners[smid].epoch = fTransportEpoch;
        fReplySlots[smid].valid = 0;
    }
    IOSimpleLockUnlock(fReplyLock);
    if (!available) {
        setProperty("MPRTmfState", "no-free-high-priority-credit");
        return false;
    }
    UInt8 *frame = base + (UInt64)smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPRScsiTaskMgmtRequest *request = (MPRScsiTaskMgmtRequest *)frame;
    const UInt16 handle = (UInt16)fAttachedDevHandle;
    request->DevHandle = handle;
    request->Function = MPI2_FUNCTION_SCSI_TASK_MGMT;
    request->TaskType = taskType;
    request->MsgFlags = MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET;
    request->TaskMID = taskType == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK
        ? taskMid : 0;
    fTmfCount++;
    postHighPriorityRequest(smid);
    UInt8 replyBuf[64], replyKind = 0;
    bool completed = waitForSmid(15000, smid) &&
        takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind);
    if (!completed) {
        IOSimpleLockLock(fReplyLock);
        if (fCommandOwners[smid].state == kMPRCommandPosted)
            fCommandOwners[smid].state = kMPRCommandQuarantined;
        IOSimpleLockUnlock(fReplyLock);
        setProperty("MPRTmfState", "reply-timeout-credit-retained");
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    fTmfBusy[smid - MPR_SM_TMF_BASE] = false;
    IOSimpleLockUnlock(fReplyLock);
    const MPRScsiTaskMgmtReply *reply = (const MPRScsiTaskMgmtReply *)replyBuf;
    if (replyKind != 1 || replyBuf[3] != MPI2_FUNCTION_SCSI_TASK_MGMT ||
        replyBuf[2] * 4U < sizeof(MPRScsiTaskMgmtReply) ||
        reply->DevHandle != handle || reply->TaskType != taskType) {
        recordProtocolFault(kMPRProtocolUnexpectedCompletion, smid);
        return false;
    }
    bool ok = (reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS &&
        (reply->ResponseCode == MPI2_SCSITASKMGMT_RSP_TM_COMPLETE ||
         reply->ResponseCode == MPI2_SCSITASKMGMT_RSP_TM_SUCCEEDED);
    setProperty("MPRTmfResponseCode", (unsigned long long)reply->ResponseCode, 8);
    setProperty("MPRTmfIOCStatus",
                (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
    setProperty("MPRTmfTerminationCount", (unsigned long long)reply->TerminationCount, 32);
    setProperty("MPRTmfTaskType", (unsigned long long)taskType, 8);
    setProperty("MPRTmfState", ok ? "ok" : "failed");
    return ok;
}

bool
MPRController::drainTaskManagedCommands(UInt16 handle, UInt16 taskMid,
                                        bool wholeTarget)
{
    /* A TMF reply does not consume the original SCSI reply. Do not recycle
     * its SMID or sense/data mapping until that completion has been observed. */
    for (UInt32 elapsed = 0; elapsed < 1000 && !fStopping; ++elapsed) {
        processReplyQueue();
        IOSimpleLockLock(fReplyLock);
        bool pending = false;
        for (UInt32 id = 1; id < fRequestCount && id < MPR_REQUEST_FRAMES; ++id) {
            const MPRCommandOwner *owner = &fCommandOwners[id];
            if (owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST &&
                owner->handle == handle && (wholeTarget || id == taskMid) &&
                (owner->state == kMPRCommandPosted ||
                 owner->state == kMPRCommandQuarantined)) pending = true;
        }
        bool fault = fProtocolRecoveryPending;
        IOSimpleLockUnlock(fReplyLock);
        if (!pending && !fault) return true;
        if (fault) return false;
        IOSleep(1);
    }
    return false;
}

bool
MPRController::recoverTimedOutCommand(UInt32 smid)
{
    static const UInt8 levels[3] = {
        MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK,
        MPI2_SCSITASKMGMT_TASKTYPE_LUN_RESET,
        MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET
    };
    UInt32 i, injectFail;
    const UInt16 handle = (UInt16)fAttachedDevHandle;
    bool recovered = false;
    bool controllerReset = false;

    if (smid == 0) {
        return false;
    }
    /* Freeze the SMID until the firmware confirms the task is gone; it must
     * not write a late reply into a reused request frame (risk R6). */
    fFrozenSmid = (UInt16)smid;
    setProperty("MPRFrozenSmid", (unsigned long long)smid, 16);

    /* mpr_tmf_fail=N makes the first N levels look unsuccessful, so the next
     * level is really submitted. N=1 exercises LUN_RESET, N=2 TARGET_RESET,
     * N=3 falls through to resetAndReinitialize(). It applies once. */
    injectFail = fInjectTmfFail;
    fInjectTmfFail = 0;
    if (injectFail != 0) {
        setProperty("MPRTmfInjectedFail", (unsigned long long)injectFail, 8);
    }

    for (i = 0; i < 3 && !recovered; i++) {
        if (injectFail > i) {
            IOSleep(50);
            continue;
        }
        if (sendTaskMgmt(levels[i], (UInt16)smid, MPR_SM_TMF_BASE + i) &&
            drainTaskManagedCommands(handle, (UInt16)smid, i != 0)) {
            recovered = true;
        } else {
            IOSleep(50);
        }
    }
    if (!recovered) {
        controllerReset = true;
        setProperty("MPRResetInitiator", "io-timeout");
        recovered = resetAndReinitialize();
    }
    bool removed;
    IOSimpleLockLock(fReplyLock);
    removed = fRemovalPending;
    IOSimpleLockUnlock(fReplyLock);
    if (controllerReset || removed || !recovered) {
        /* Do not resume an old IO across a global reset or a physical
         * removal. The unlocked worker performs OFFLINE -> ONLINE before
         * clients issue new requests against the new media generation. */
        invalidateMedia();
        IOSimpleLockLock(fReplyLock);
        fHotplugPending = true;
        IOSimpleLockUnlock(fReplyLock);
        if (!recovered) {
            recordProtocolFault(kMPRProtocolManagementTimeout, smid);
        }
        setProperty("MPRRecoveryState", recovered ? "media-revalidation" : "failed");
        return false;
    }
    if (recovered) {
        fFrozenSmid = 0;
        fDeviceOffline = false;
        setProperty("MPRFrozenSmid", 0ULL, 16);
        setProperty("MPRDeviceOffline", false);
    } else {
        /* Recovery failed: keep the SMID frozen and the device offline. The
         * firmware may still own the original request frame (risk R6). */
        fDeviceOffline = true;
        setProperty("MPRDeviceOffline", true);
    }
    setProperty("MPRRecoveryState", recovered ? "ok" : "failed");
    return recovered;
}

void
MPRController::watchdogTimeout(IOTimerEventSource *sender)
{
    if (fStopping) return;
    if (!fStarting && !fReinitInProgress && !fSuspended && fRegs != NULL) {
        UInt32 state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
        setProperty("MPRWatchdogIOCState", (unsigned long long)state, 32);
        if (state != MPI2_IOC_STATE_OPERATIONAL)
            recordProtocolFault(kMPRProtocolIocNotOperational, 0);
    }
    serviceProtocolState();
    bool schedule = false;
    if (fReplyLock != NULL) {
        IOSimpleLockLock(fReplyLock);
        if (fHotplugCooldown != 0) fHotplugCooldown--;
        if (!fStarting && !fReinitInProgress && !fHotplugRunning &&
            !fSuspended && fHotplugPending && fHotplugCooldown == 0 &&
            fHotplugCall != NULL) {
            /* Consume only this scheduling token. An event arriving while
             * the worker runs sets it again and must survive completion. */
            fHotplugPending = false;
            fHotplugRunning = true;
            schedule = true;
        }
        IOSimpleLockUnlock(fReplyLock);
    }
    setProperty("MPRWatchdogState", fProtocolRecoveryPending ? "protocol-recovery" :
                fTopologyChanged ? "topology-change" :
                !fMediaPresent ? "offline" : "ok");
    if (schedule) thread_call_enter(fHotplugCall);
    if (sender != NULL) sender->setTimeoutMS(1000);
}

void
MPRController::hotplugCall(thread_call_param_t param0,
                           thread_call_param_t param1)
{
    (void)param1;
    ((MPRController *)param0)->hotplugThread();
}

bool
MPRController::mediaRemovalPending(void)
{
    IOSimpleLockLock(fReplyLock);
    bool pending = fRemovalPending;
    IOSimpleLockUnlock(fReplyLock);
    return pending;
}

void
MPRController::invalidateMedia(void)
{
    /* Caller holds fIOLock. Transport failure and publication state are
     * separate: setting DeviceOffline must never suppress a later OFFLINE. */
    if (fMediaPresent) {
        fMediaPresent = false;
        fMediaGeneration++;
        fMediaOfflinePending = (fBlockDev != NULL);
    }
    fMediaOnlinePending = false;
    fDeviceOffline = true;
    setProperty("MPRDeviceOffline", true);
    setProperty("MPRMediaPresent", false);
    setProperty("MPRMediaReady", false);
}

bool
MPRController::takeMediaOffline(void)
{
    /* Caller holds fIOLock; return with it held. Storage notifications can
     * re-enter the transport, so never send them while holding this lock. */
    invalidateMedia();
    if (!fMediaOfflinePending || fBlockDev == NULL) return true;
    MPRBlockDev *nub = fBlockDev;
    nub->retain();
    IOLockUnlock(fIOLock);
    IOReturn status = nub->notifyMediaState(kIOMediaStateOffline);
    nub->release();
    IOLockLock(fIOLock);
    setProperty("MPRMediaOfflineStatus", (unsigned long long)(UInt32)status, 32);
    /* Before matching there is no old IOBlockStorageDriver/IOMedia to retire.
     * Its eventual initial probe will observe fMediaPresent == false. */
    if (status == kIOReturnSuccess || status == kIOReturnNotAttached) {
        fMediaOfflinePending = false;
        return true;
    }
    return false;
}

void
MPRController::hotplugThread(void)
{
    UInt32 generation = 0, oldHandle = 0;
    bool removal = false, retry = false, resetOk = false;
    bool retryRequested = false;
    bool directProbe = false;
    bool physicalChange = false;
    const char *state = "stopped";
    IOReturn notification = kIOReturnSuccess;

    if (fIOLock == NULL) return;
    IOLockLock(fIOLock);
    IOSimpleLockLock(fReplyLock);
    generation = fTopologyGeneration;
    if (fStopping || fStarting || fSuspended) {
        /* A queued worker may run after PM quiesced the device. Preserve
         * every physical/IR latch until wake can actually examine it. */
        if (!fStopping) {
            fHotplugPending = true;
            retry = true;
        }
        IOSimpleLockUnlock(fReplyLock);
        goto done;
    }
    removal = fRemovalPending;
    fRemovalPending = false;
    retryRequested = fHotplugRetryRequested;
    fHotplugRetryRequested = false;
    physicalChange = fPhysicalChangePending;
    fPhysicalChangePending = false;
    IOSimpleLockUnlock(fReplyLock);

    /* Per-target removal and recovery are not wired yet. With several
     * published targets the single-target rebuild below would tear down every
     * medium and republish one arbitrary disk, so keep the steady state. */
    if (fTargetCount > 1 && !removal && !retryRequested &&
        !fDeviceOffline && !fProtocolRecoveryPending && !fMediaOfflinePending) {
        state = "multi-target-steady";
        goto done;
    }

    oldHandle = fAttachedDevHandle;
    if (physicalChange || retryRequested) fRequireSameIdentity = false;
    else if (fMediaPresent) fRequireSameIdentity = fMediaIdentityValid;
    if (removal || retryRequested) fHotplugAttempts = 0;
    if (removal || fDeviceOffline || fProtocolRecoveryPending ||
        fMediaOfflinePending) {
        /* Which of the four independent reasons tore the medium down. */
        setProperty("MPRMediaOfflineCause",
                    removal ? "removal-event" :
                    fMediaOfflinePending ? "offline-not-completed" :
                    fProtocolRecoveryPending ? "protocol-recovery" :
                    "device-offline");
        if (!takeMediaOffline()) {
            state = "offline-notification-failed";
            retry = true;
            goto done;
        }
    }
    if (fStopping) goto done;

    if (removal && !fProtocolRecoveryPending && !retireRemovedTarget()) {
        /* Any uncertain old ownership or reused handle goes through the
         * existing bounded IOC recovery, never an unconditional remove. */
        recordProtocolFault(kMPRTargetRemovalUncertain, oldHandle);
    }

    if (!fProtocolRecoveryPending) {
        if (!allocatePageBuffer() || !readSasIOUnitPage0()) {
            state = "topology-read-failed";
            retry = true;
            goto done;
        }
        if (fAttachedDevHandle == 0 && (fLinkUpPhys == 0 || fHasExpander)) {
            /* A successful empty snapshot with no negotiated link anywhere is
             * absence; a failed read above is not. Poll it without resetting. */
            retry = true;
            state = takeMediaOffline() ? "removed" : "offline-notification-failed";
            fCapacityBlocks = 0;
            fBlockSize = 0;
            fUnmapEnabled = false;
            if (fDiscoveredDiskCount == 0) fRequireSameIdentity = false;
            fHotplugAttempts = 0;
            goto done;
        }
        if (fAttachedDevHandle == 0) {
            /* A phy negotiates a link but the IOC attaches no target, or the
             * handle it listed no longer names that target. Both need the
             * bounded diagnostic reset below; a single mismatch sample is
             * given one poll to settle first. */
            if (fSasDeviceMismatch && fIdentityLostCount < 2) {
                state = "target-identity-pending";
                retry = true;
                goto done;
            }
            state = fSasDeviceMismatch ? "target-identity-lost" :
                                        "link-without-target";
        } else if (fMediaPresent && !removal && oldHandle == fAttachedDevHandle &&
                   !fDeviceOffline) {
            if (!fMediaOnlinePending) {
                state = "online";
                goto done;
            }
            goto notifyOnline;
        } else if (oldHandle == 0) {
            /* A target that was absent before this snapshot is a new
             * insertion: it gets a full recovery budget. */
            fHotplugAttempts = 0;
        }
        if (!takeMediaOffline()) {
            state = "offline-notification-failed";
            retry = true;
            goto done;
        }
        if (fAttachedDevHandle != 0 && fHotplugAttempts < 3) {
            bool dmaSafe = true;
            for (UInt32 smid = 10; smid <= 30; smid++) {
                if (smid >= 26 && smid <= 28) continue;
                if (!commandDmaSafe(smid)) { dmaSafe = false; break; }
            }
            /* A terminal device discovered after removal needs a fresh probe,
             * not an unconditional reset of the controller and its fabric. */
            if (dmaSafe) {
                fReinitInProgress = true;
                resetOk = probeScsiIdentity() && probeCapacity() && prepareTargetMedia();
                fReinitInProgress = false;
            } else {
                recordProtocolFault(kMPRTargetRemovalUncertain, oldHandle);
            }
            if (resetOk && !fProtocolRecoveryPending && !fStopping) {
                directProbe = true;
                setProperty("MPRHotplugResetResult", "not-needed");
                goto targetReady;
            }
        }
    }

    /* Single-target prototype: a diagnostic reset is the bounded fallback
     * for reappearing targets. Do not reset endlessly while a disk is gone
     * or while firmware repeatedly reports its own discovery events. */
    if (fHotplugAttempts >= 3) {
        state = "recovery-exhausted";
        /* Keep the snapshot polled. A target that finally appears clears the
         * budget above; an empty fabric is never reset again. */
        retry = true;
        goto done;
    }
    fHotplugAttempts++;
    setProperty("MPRResetInitiator", "hotplug");
    resetOk = resetAndReinitialize();
    setProperty("MPRHotplugResetResult", resetOk ? "ok" : "failed");
    if (!resetOk) {
        invalidateMedia();
        if (fAttachedDevHandle == 0 && !fProtocolRecoveryPending) {
            state = "removed";
            fHotplugAttempts = 0;
            retry = true;
        } else {
            state = "recovery-failed";
            retry = fHotplugAttempts < 3;
        }
        goto done;
    }
targetReady:
    /* A new physical removal observed during reset must be processed before
     * any ONLINE notification. Do not erase it with a final pending=false. */
    IOSimpleLockLock(fReplyLock);
    removal = fRemovalPending;
    if (directProbe && generation != fTopologyGeneration) {
        removal = true;
    }
    IOSimpleLockUnlock(fReplyLock);
    if (removal || fProtocolRecoveryPending || fStopping) {
        invalidateMedia();
        state = "topology-changed-during-recovery";
        retry = !fStopping;
        goto done;
    }
    fDeviceOffline = false;
    fMediaPresent = true;
    fMediaGeneration++;
    fMediaOnlinePending = true;

notifyOnline:
    {
        UInt32 publication = fMediaGeneration;
        MPRBlockDev *nub = fBlockDev;
        if (nub != NULL) nub->retain();
        IOLockUnlock(fIOLock);
        if (nub != NULL) {
            notification = nub->notifyMediaState(kIOMediaStateOnline);
            nub->release();
        } else {
            notification = publishBlockDevice() ? kIOReturnSuccess : kIOReturnNoMemory;
        }
        IOLockLock(fIOLock);
        setProperty("MPRMediaOnlineStatus",
                    (unsigned long long)(UInt32)notification, 32);
        if (fStopping || publication != fMediaGeneration || fDeviceOffline ||
            fProtocolRecoveryPending || mediaRemovalPending()) {
            state = "publication-interrupted";
            retry = !fStopping;
            goto done;
        }
        if (notification == kIOReturnNotAttached) {
            state = "awaiting-storage-driver";
            retry = true;
        } else if (notification != kIOReturnSuccess) {
            invalidateMedia();
            state = "online-notification-failed";
            retry = true;
        } else {
            fMediaOnlinePending = false;
            fHotplugAttempts = 0;
            state = "online";
        }
    }

done:
    setProperty("MPRHotplugState", state);
    setProperty("MPRMediaPresent", fMediaPresent);
    setProperty("MPRDeviceOffline", fDeviceOffline);
    setProperty("MPRMediaGeneration", (unsigned long long)fMediaGeneration, 32);
    setProperty("MPRHotplugAttempts", (unsigned long long)fHotplugAttempts, 32);
    setProperty("MPRHotplugHandle", (unsigned long long)fAttachedDevHandle, 16);
    setProperty("MPRHotplugBlockDev", fBlockDev != NULL);
    IOSimpleLockLock(fReplyLock);
    if (generation == fTopologyGeneration && !fHotplugPending && !retry) {
        fTopologyChanged = false;
    }
    if (retry) {
        fHotplugPending = true;
        fHotplugCooldown = 1;
    }
    fHotplugRunning = false;
    IOSimpleLockUnlock(fReplyLock);
    IOLockUnlock(fIOLock);
}

bool
MPRController::resetAndReinitialize(void)
{
    /* Called with fIOLock held. Only restore transport and target readiness;
     * storage notifications and registration belong to the unlocked worker. */
    bool ok = false;
    const char *state = "failed";
    if (fReinitInProgress || fStopping) return false;
    IOSimpleLockLock(fReplyLock);
    if (fMediaPresent) fRequireSameIdentity = fMediaIdentityValid;
    if (fPhysicalChangePending || fHotplugRetryRequested) fRequireSameIdentity = false;
    IOSimpleLockUnlock(fReplyLock);
    fReinitInProgress = true;
    fDeviceOffline = true;
    fUnmapEnabled = false;
    fResetCount++;
    setProperty("MPRMediaReady", false);

    teardownInterrupts();
    if (!diagReset()) {
        state = "diag-reset-failed";
        /* Old requests can still own DMA; do not free their buffers. */
        recordProtocolFault(kMPRProtocolManagementTimeout, fTimedOutSmid);
        goto finished;
    }
    resetProtocolState();
    /* A timed-out startup manufacturing probe used a smaller temporary
     * page. It is safe to replace only after this successful IOC reset. */
    if (fPageBuffer != NULL && fPageBuffer->getLength() < MPR_PAGE_BUF_BYTES) {
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    freeQueueMemory();
    fReplyPostIndex = fReplyCount = 0;
    fLastReplySmid = 0xFFFF;
    fLastReplyKind = 0;
    fFrozenSmid = fTimedOutSmid = 0;
    fAttachedDevHandle = 0;
    fCapacityBlocks = 0;
    fBlockSize = 0;

    /* IOC Init advertises the installed interrupt vector and initializeQueues
     * unmasks reply interrupts only when the source already exists. */
    if (!setupInterrupts()) {
        state = "interrupt-setup-failed";
        recordProtocolFault(kMPRProtocolInterruptUnavailable, 0);
        goto finished;
    }
    if (!initializeController() || !initializeQueues()) goto finished;
    if (!allocatePageBuffer() || !sendEventNotification() || !sendPortEnable())
        goto finished;
    for (UInt32 retry = 0; retry < 5 && !fStopping; retry++) {
        IOSleep(200);
        if (!readSasIOUnitPage0()) {
            if (fProtocolRecoveryPending) goto finished;
            continue;
        }
        if (fAttachedDevHandle != 0) break;
    }
    if (fAttachedDevHandle == 0) {
        state = "no-media";
        goto finished;
    }
    if (!probeScsiIdentity() || !probeCapacity() || !prepareTargetMedia())
        goto finished;
    ok = !fProtocolRecoveryPending && !fStopping;
    state = ok ? "ok" : "failed";

finished:
    fReinitInProgress = false;
    fDeviceOffline = !ok;
    setProperty("MPRReinitState", state);
    setProperty("MPRResetCount", (unsigned long long)fResetCount, 32);
    setProperty("MPRResidentBuffersReady",
                fPageBuffer != NULL && fPagePhys != 0 &&
                fDataBuffer != NULL && fDataPhys != 0 && fDataBytes != 0);
    return ok;
}

/* Target protocol implementation is in MPRTarget.cpp. */

/* Target protocol implementation is in MPRTarget.cpp. */
