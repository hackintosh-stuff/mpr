#include "MPRController.h"
#include "MPI/mpi2_min.h"
#include "MPI/mpi2_target.h"
#include "MPI/mpi26_devices.h"
#include "MPI/mpi2_mgmt.h"
#include "MPRDmaSgl.h"
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/task.h>
#include <string.h>

/* mpr_user.c provides the protocol reference; this is a native IOUserClient
 * ABI, not a binary-compatible FreeBSD ioctl shim. SMID 1 is serialized with
 * all config/probe work by fIOLock and with block I/O by the existing fence. */
static const UInt32 kMPRControlSmid = 1;
static UInt32 mprControlTimeout(const MPRControlInput &in)
{ return in.timeoutMs ? in.timeoutMs : 30000; }

bool MPRController::allocateControlBuffer(UInt32 bytes)
{
    if (fControlDmaBusy || !bytes || bytes > 2U * kMPRControlMaxData + 4096U)
        return false;
    if (fControlBuffer && fControlBufferBytes >= bytes) return true;
    if (fControlBuffer) {
        if (fControlBufferPrepared) fControlBuffer->complete();
        fControlBuffer->release();
        fControlBuffer = NULL;
        fControlBufferPrepared = false;
        fControlBufferBytes = 0;
        fControlPhys = 0;
    }
    IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        bytes, UINT64_C(0x00000000fffff000));
    if (!memory) return false;
    IOByteCount contiguous = 0;
    UInt64 address = memory->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    bool prepared = memory->prepare() == kIOReturnSuccess;
    if (!prepared || !address || (address & 4095) || contiguous < bytes ||
        address > UINT32_MAX || bytes - 1 > UINT32_MAX - address ||
        !memory->getBytesNoCopy()) {
        if (prepared) memory->complete();
        memory->release();
        return false;
    }
    fControlBuffer = memory;
    fControlPhys = address;
    fControlBufferBytes = bytes;
    fControlBufferPrepared = true;
    return true;
}

void MPRController::freeControlBuffers(bool dmaStopped)
{
    if (!dmaStopped) return;
    if (fControlBuffer) {
        if (fControlBufferPrepared) fControlBuffer->complete();
        fControlBuffer->release();
    }
    fControlBuffer = NULL;
    fControlBufferPrepared = fControlDmaBusy = false;
    fControlBufferBytes = 0;
    fControlPhys = 0;
    for (unsigned type = 0; type < 3; ++type) {
        DiagnosticBuffer &b = fDiagnostic[type];
        if (b.memory) {
            if (b.prepared) b.memory->complete();
            b.memory->release();
        }
        bzero(&b, sizeof(b));
    }
}

void MPRController::recordControlEventLocked(UInt16 event, UInt32 context,
                                            const UInt8 *payload, UInt32 bytes)
{
    if (event >= 128 ||
        fControlEventSequence == UINT64_MAX) return;
    UInt64 sequence = ++fControlEventSequence;
    MPRControlEvent &entry = fControlEvents[(sequence - 1) % kMPRControlEventCount];
    bzero(&entry, sizeof(entry));
    entry.sequence = sequence;
    entry.milliseconds = uptimeMilliseconds();
    entry.epoch = fTransportEpoch;
    entry.event = event;
    entry.context = context;
    entry.payloadBytes = bytes < kMPRControlEventPayload ? bytes : kMPRControlEventPayload;
    if (entry.payloadBytes && payload) memcpy(entry.payload, payload, entry.payloadBytes);
}

void MPRController::releaseDiagnosticEventLocked(const UInt8 *reply, UInt32 bytes)
{
    if (bytes < 20 || reply[1] >= 3 ||
        (mprControlLe16(reply + 14) & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED)
        return;
    DiagnosticBuffer &b = fDiagnostic[reply[1]];
    if (b.memory && b.owned && b.epoch == fTransportEpoch) {
        b.owned = false;
        b.valid = true;
    }
}

bool MPRController::submitControlFrame(UInt32 flags, UInt32 high, UInt32 timeout,
                                        UInt8 *reply, UInt8 *kind)
{
    const UInt8 *frame = (const UInt8 *)fRequestBuffer->getBytesNoCopy() +
                         kMPRControlSmid * fRequestFrameSize;
    const UInt8 function = frame[3];
    const bool targetIo = function == MPI2_FUNCTION_SCSI_IO_REQUEST ||
        function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH ||
        function == MPI2_FUNCTION_SATA_PASSTHROUGH || function == MPI2_FUNCTION_NVME_ENCAPSULATED;
    IOSimpleLockLock(fReplyLock);
    bool posted = !(targetIo && fRemovalPending) &&
                  postOwnedRequestLocked(kMPRControlSmid, flags, high);
    IOSimpleLockUnlock(fReplyLock);
    if (!posted) { cancelUnpostedCommand(kMPRControlSmid); return false; }
    fControlDmaBusy = true;
    ++fCommandCount;
    if (!waitForSmid(timeout, kMPRControlSmid) ||
        !takeReplySlot(kMPRControlSmid, reply, kMPRControlMaxReply, kind)) {
        IOSimpleLockLock(fReplyLock);
        if (fCommandOwners[kMPRControlSmid].state == kMPRCommandPosted)
            fCommandOwners[kMPRControlSmid].state = kMPRCommandQuarantined;
        recordProtocolFaultLocked(kMPRTargetMalformedReply, kMPRControlSmid);
        IOSimpleLockUnlock(fReplyLock);
        MPR_DEBUG_LOG("MPR control: timeout; frame/payload retained, no command replay\n");
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    fManagementBusy[kMPRControlSmid] = false;
    IOSimpleLockUnlock(fReplyLock);
    fControlDmaBusy = false;
    return true;
}

IOReturn MPRController::controlRequest(const UInt8 *bytes, UInt32 inputSize,
    UInt8 *response, UInt32 outputSize, UInt32 *produced)
{
    if (!bytes || !response || !produced || !fIOLock ||
        inputSize < sizeof(MPRControlInput)) return kIOReturnBadArgument;
    if (!fManagementEnabled) return kIOReturnNotPermitted;
    MPRControlInput in;
    memcpy(&in, bytes, sizeof(in));
    if (!mprControlInputValid(in, inputSize, outputSize)) return kIOReturnBadArgument;
    MPRControlOutput *out = (MPRControlOutput *)response;
    bzero(out, outputSize);
    out->version = kMPRControlVersion;
    out->operation = in.operation;
    const bool fence = in.operation == kMPRControlPassthrough ||
        in.operation == kMPRControlReset || in.operation == kMPRControlRescan ||
        in.operation == kMPRControlWriteRegister || in.operation == kMPRControlEventMask ||
        in.operation == kMPRControlDiagRegister ||
        in.operation == kMPRControlDiagRelease || in.operation == kMPRControlDiagUnregister;
    IOReturn status = fence ? beginAsyncBarrier() : kIOReturnSuccess;
    if (status == kIOReturnSuccess) {
        IOLockLock(fIOLock);
        out->epoch = fTransportEpoch;
        if (fStopping || fStarting || isInactive()) status = kIOReturnNotAttached;
        else if (in.operation >= kMPRControlEventMask && in.epoch != fTransportEpoch)
            status = kIOReturnNotReady;
        else status = controlDispatch(in, bytes + sizeof(in),
            bytes + sizeof(in) + in.requestBytes, out);
        out->epoch = fTransportEpoch;
        IOLockUnlock(fIOLock);
        if (fence) endAsyncBarrier();
    }
    out->status = (UInt32)status;
    if (out->dataBytes > in.dataInBytes) { out->dataBytes = 0; out->status = kIOReturnOverrun; }
    *produced = sizeof(*out) + out->dataBytes;
    /* Transport/firmware errors are explicit in the returned packet. Returning
     * success here ensures IOKit copies the checked reply and sense to the caller. */
    return kIOReturnSuccess;
}

IOReturn MPRController::controlDispatch(const MPRControlInput &in, const UInt8 *request,
    const UInt8 *dataOut, MPRControlOutput *out)
{
    UInt8 *data = (UInt8 *)(out + 1);
    if (in.operation != kMPRControlPassthrough && in.dataOutBytes) return kIOReturnBadArgument;
    if (fFirmwareDownloadActive && in.operation != kMPRControlPassthrough &&
        in.operation != kMPRControlInfo && in.operation != kMPRControlTargets &&
        in.operation != kMPRControlEvents && in.operation != kMPRControlReset)
        return kIOReturnBusy;
    switch (in.operation) {
    case kMPRControlInfo: {
        if (in.requestBytes || in.dataInBytes < sizeof(MPRControlInfo)) return kIOReturnNoSpace;
        MPRControlInfo *info = (MPRControlInfo *)data;
        info->capabilities = fIocCapabilities;
        info->requestFrameBytes = fRequestFrameSize;
        info->replyFrameBytes = fReplyFrameSize;
        info->requestCredits = fRequestCredit;
        info->targetLimit = kMPRMaxTargets;
        info->featureBits = (fNvmeProtocol ? 1U : 0U) | (fAtomicRequests ? 2U : 0U) |
            (fManagementEnabled ? 4U : 0U) | (fMaintenanceMode ? 8U : 0U) |
            (fFirmwareDownloadActive ? 16U : 0U) | (fFirmwareDownloadPoisoned ? 32U : 0U);
        info->mpiVersion = fMpiVersion;
        info->milliseconds = uptimeMilliseconds();
        memcpy(info->iocFacts, fIocFactsSnapshot, sizeof(info->iocFacts));
        info->firmware = mprControlLe32(fIocFactsSnapshot + 0x20);
        if (fPCIDevice && !fSuspended) {
            for (UInt32 offset = 0; offset < 256; offset += 4)
                mprControlPut32(info->pciConfig + offset, fPCIDevice->configRead32(offset));
            info->vendorDevice = mprControlLe32(info->pciConfig);
            info->pciRevision = info->pciConfig[8];
        }
        out->dataBytes = sizeof(*info);
        return kIOReturnSuccess;
    }
    case kMPRControlTargets: {
        if (in.requestBytes || in.argument0 > kMPRMaxTargets) return kIOReturnBadArgument;
        UInt32 capacity = in.dataInBytes / sizeof(MPRControlTarget), count = 0;
        UInt32 index = (UInt32)in.argument0;
        for (; index < kMPRMaxTargets && count < capacity; ++index) {
            const MPRTargetSlot &slot = fTargets[index];
            if (!slot.handle) continue;
            MPRControlTarget &target = ((MPRControlTarget *)data)[count++];
            target.handle = slot.handle; target.kind = (UInt8)slot.kind;
            target.flags = (slot.present ? 1 : 0) | (slot.probeValid ? 2 : 0) | (slot.online ? 4 : 0);
            target.generation = slot.generation; target.wwid = slot.sasAddress;
            target.lun = slot.lun; target.blocks = slot.capacityBlocks;
            target.pathCount = slot.pathCount;
            target.targetCapabilities = (slot.managementOnly ? 1U : 0U) |
                (slot.tlrEnabled ? 2U : 0U) | ((UInt32)slot.protectionType << 8);
            target.blockSize = slot.blockSize; target.maxTransfer = slot.maxTransferBytes;
            strlcpy(target.vendor, slot.vendor, sizeof(target.vendor));
            strlcpy(target.product, slot.product, sizeof(target.product));
            strlcpy(target.revision, slot.revision, sizeof(target.revision));
            strlcpy(target.serial, slot.serial, sizeof(target.serial));
            strlcpy(target.identity, slot.identity, sizeof(target.identity));
        }
        out->dataBytes = count * sizeof(MPRControlTarget); out->nextSequence = index;
        return capacity ? kIOReturnSuccess : kIOReturnNoSpace;
    }
    case kMPRControlEvents: {
        if (in.requestBytes) return kIOReturnBadArgument;
        UInt32 capacity = in.dataInBytes / sizeof(MPRControlEvent), count = 0;
        if (!capacity) return kIOReturnNoSpace;
        IOSimpleLockLock(fReplyLock);
        UInt64 last = fControlEventSequence;
        UInt64 first = last >= kMPRControlEventCount ? last - kMPRControlEventCount + 1 : 1;
        UInt64 next = in.argument0 ? in.argument0 : first;
        if (next < first) { next = first; out->flags |= 1; }
        while (next <= last && count < capacity) {
            ((MPRControlEvent *)data)[count++] = fControlEvents[(next - 1) % kMPRControlEventCount];
            if (next == UINT64_MAX) break;
            ++next;
        }
        IOSimpleLockUnlock(fReplyLock);
        out->dataBytes = count * sizeof(MPRControlEvent); out->nextSequence = next;
        return kIOReturnSuccess;
    }
    case kMPRControlDiagQuery: case kMPRControlDiagRead:
    case kMPRControlDiagRegister: case kMPRControlDiagRelease: case kMPRControlDiagUnregister:
        return controlDiagnostic(in, request, out);
    default: break;
    }
    if (fSuspended || fReinitInProgress || !fFactsValid || !fRegs || !fReplyLock)
        return kIOReturnNotReady;
    if (in.operation == kMPRControlReset) {
        if (!fMaintenanceMode || !(in.flags & kMPRControlAllowMutation) || in.requestBytes) return kIOReturnNotPermitted;
        bool ready = resetAndReinitialize();
        fHotplugPending = true;
        return ready ? kIOReturnSuccess : kIOReturnIOError;
    }
    /* The control channel must work on an empty HBA or a tape/SES-only
     * topology. Block-media publication is not controller transport health. */
    if (fProtocolRecoveryPending ||
        (readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK) != MPI2_IOC_STATE_OPERATIONAL)
        return kIOReturnNotReady;
    if (in.operation == kMPRControlReadRegister || in.operation == kMPRControlWriteRegister) {
        if (in.requestBytes || in.dataInBytes < 4 || !fMMIO || in.argument0 > fMMIO->getLength() ||
            fMMIO->getLength() - in.argument0 < 4 || (in.argument0 & 3)) return kIOReturnBadArgument;
        if (in.operation == kMPRControlWriteRegister) {
            /* Arbitrary descriptor/queue-address writes would bypass DMA ownership.
             * Scratchpads are writable; operational registers remain driver-owned. */
            if (!fMaintenanceMode || !(in.flags & kMPRControlAllowMutation) ||
                in.argument0 < 0xB0 || in.argument0 > 0xBC || in.argument1 > UINT32_MAX)
                return kIOReturnNotPermitted;
            writeReg((UInt32)in.argument0, (UInt32)in.argument1);
        }
        if (in.dataInBytes < 4) return kIOReturnNoSpace;
        mprControlPut32(data, readReg((UInt32)in.argument0)); out->dataBytes = 4;
        return kIOReturnSuccess;
    }
    if (in.operation == kMPRControlEventMask) {
        if (in.requestBytes != 16) return kIOReturnBadArgument;
        IOSimpleLockLock(fReplyLock);
        memcpy(fControlEventMask, request, 16);
        IOSimpleLockUnlock(fReplyLock);
        return sendEventNotification() ? kIOReturnSuccess : kIOReturnIOError;
    }
    if (in.operation == kMPRControlRescan) {
        if (in.requestBytes) return kIOReturnBadArgument;
        bool ok = discoverTopology();
        fHotplugPending = true;
        return ok ? kIOReturnSuccess : kIOReturnIOError;
    }
    if (in.operation == kMPRControlPassthrough)
        return controlPassthrough(in, request, dataOut, out);
    return kIOReturnUnsupported;
}

IOReturn MPRController::controlPassthrough(const MPRControlInput &in, const UInt8 *user,
    const UInt8 *dataOut, MPRControlOutput *out)
{
    if (in.requestBytes < 12 || user[2] || user[8] || user[9] ||
        in.requestBytes > fRequestFrameSize || !fRequestBuffer || !fSenseBuffer ||
        fControlDmaBusy) return kIOReturnBadArgument;
    const UInt8 function = user[3];
    const bool write = in.dataOutBytes != 0, read = in.dataInBytes != 0;
    const UInt32 length = write ? in.dataOutBytes : in.dataInBytes;
    bool mutation = false, ieee = false, scsi = false, nvme = false;
    UInt32 prefix = 0, descriptor = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, high = 0;
    switch (function) {
    case MPI2_FUNCTION_IOC_FACTS: case MPI2_FUNCTION_PORT_FACTS:
        prefix = 12; if (write || read) return kIOReturnBadArgument; break;
    case MPI2_FUNCTION_CONFIG: {
        prefix = 28;
        if (in.requestBytes != prefix || (write && read)) return kIOReturnBadArgument;
        UInt8 action = user[0];
        if (action > 7) return kIOReturnBadArgument;
        mutation = action == 2 || action == 3 || action == 4;
        UInt32 words = (user[23] & 15) == 15 ? mprControlLe16(user + 4) : user[21];
        if (action == 0 || action == 3) { if (length) return kIOReturnBadArgument; }
        else if (words * 4 != length || (mutation ? !write : !read)) return kIOReturnBadArgument;
        break;
    }
    case MPI2_FUNCTION_FW_DOWNLOAD: {
        prefix = 32; ieee = true; mutation = true;
        if (in.requestBytes != prefix || !write || read || !in.argument1 ||
            (user[7] & ~1U)) return kIOReturnBadArgument;
        const UInt32 total = mprControlLe32(user + 12), offset = mprControlLe32(user + 24);
        if (!total || offset >= total || length > total - offset ||
            bool(user[7] & 1) != (offset + length == total)) return kIOReturnBadArgument;
        if (fFirmwareDownloadPoisoned) return kIOReturnNotReady;
        if (fFirmwareDownloadActive) {
            if (fFirmwareToken != in.argument1 || fFirmwareImageType != user[0] ||
                fFirmwareTotal != total || fFirmwareOffset != offset) return kIOReturnBusy;
        } else if (offset) return kIOReturnBadArgument;
        break;
    }
    case MPI2_FUNCTION_FW_UPLOAD:
        prefix = 32; ieee = true;
        if (in.requestBytes != prefix || write ||
            length > UINT32_MAX - mprControlLe32(user + 24)) return kIOReturnBadArgument;
        break;
    case MPI2_FUNCTION_RAID_ACTION:
        prefix = 20;
        mutation = true; if (write && read) return kIOReturnBadArgument; break;
    case MPI2_FUNCTION_SCSI_IO_REQUEST: case MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH:
        prefix = 96; ieee = true; scsi = true;
        if (in.requestBytes != prefix || (write && read) || length > kMPRControlIoBytes)
            return kIOReturnBadArgument;
        if (!(mprControlLe16(user + 36) & 0x3F) || (mprControlLe16(user + 36) & 0x3F) > 32)
            return kIOReturnBadArgument;
        mutation = function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH || write ||
                   !mprControlReadOnlyCdb(user[64]);
        if (function == MPI2_FUNCTION_SCSI_IO_REQUEST) {
            descriptor = MPI2_REQ_DESCRIPT_FLAGS_SCSI_IO;
            high = (UInt32)mprControlLe16(user) << 16;
        }
        break;
    case MPI2_FUNCTION_SMP_PASSTHROUGH:
        prefix = 32; ieee = true;
        if (!write || !read || in.dataOutBytes > 65532 || in.dataInBytes > 65532)
            return kIOReturnBadArgument;
        mutation = dataOut[0] != 0x40 || in.dataOutBytes < 4 || dataOut[1] >= 0x80;
        break;
    case MPI2_FUNCTION_SATA_PASSTHROUGH:
        prefix = 48; ieee = true;
        if (in.requestBytes != prefix || (write && read) || length > kMPRControlIoBytes)
            return kIOReturnBadArgument;
        mutation = write || (user[30] != 0xEC && user[30] != 0xA1);
        break;
    case MPI2_FUNCTION_SAS_IO_UNIT_CONTROL:
        prefix = 44; mutation = true; if (length) return kIOReturnBadArgument; break;
    case MPI2_FUNCTION_SCSI_ENCLOSURE_PROCESSOR:
        prefix = 32;
        if (in.requestBytes != prefix || length || user[4] > 1) return kIOReturnBadArgument;
        mutation = user[4] == 0; break;
    case MPI2_FUNCTION_TOOLBOX:
        prefix = 112; ieee = true; mutation = true;
        if (user[0] != MPI2_TOOLBOX_DIAGNOSTIC_CLI_TOOL || (write && read)) return kIOReturnUnsupported;
        break;
    case MPI2_FUNCTION_NVME_ENCAPSULATED:
        prefix = 96; nvme = true; descriptor = MPI26_REQ_DESCRIPT_FLAGS_PCIE_ENCAPSULATED;
        if (!fNvmeProtocol || in.requestBytes != prefix || (write && read) ||
            length > kMPRControlIoBytes || mprControlLe16(user + 4) != 64 ||
            (user[33] & 0xC0)) return kIOReturnBadArgument;
        for (unsigned i = 48; i < 56; ++i) if (user[i]) return kIOReturnBadArgument; // MPTR
        if (mprControlLe16(user + 26) & 0x10) {
            // Queue/HMB registration installs DMA addresses whose lifetime outlives a command.
            switch (user[32]) {
            case 0x02: case 0x06: case 0x0A: mutation = write; break;
            case 0x09:
                if (!user[72] || user[72] > 0x0C) return kIOReturnUnsupported;
                mutation = true; break;
            case 0x10: case 0x11: case 0x14: case 0x15: case 0x80:
            case 0x81: case 0x82: case 0x84: mutation = true; break;
            default: return kIOReturnUnsupported;
            }
        } else {
            switch (user[32]) {
            case 0x00: case 0x01: case 0x05: case 0x08: case 0x09:
            case 0x0D: case 0x11: case 0x15: mutation = true; break;
            case 0x02: case 0x0C: case 0x0E: mutation = write; break;
            default: return kIOReturnUnsupported;
            }
        }
        break;
    case MPI2_FUNCTION_SCSI_TASK_MGMT: {
        if (in.requestBytes != sizeof(MPRScsiTaskMgmtRequest) || length ||
            (!(in.flags & kMPRControlAllowMutation) || !fMaintenanceMode)) return kIOReturnNotPermitted;
        const MPRScsiTaskMgmtRequest *task = (const MPRScsiTaskMgmtRequest *)user;
        if (task->TaskType != MPI2_SCSITASKMGMT_TASKTYPE_LUN_RESET &&
            task->TaskType != MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET) return kIOReturnUnsupported;
        UInt32 lun = 0;
        if (!mprDecodeLun(task->LUN, &lun)) return kIOReturnBadArgument;
        MPRTargetSlot *slot = findTarget(mprTargetKey(task->DevHandle, lun));
        if (!slot || slot->sasAddress != in.argument0 || slot->generation != in.generation)
            return kIOReturnNotReady;
        UInt16 status = 0xFFFF;
        bool ok = sendTaskMgmt(task->TaskType, 0, 28, task->DevHandle, &status, lun);
        mprControlPut16(out->reply + 14, status); out->replyBytes = 20;
        fHotplugPending = true;
        return ok ? kIOReturnSuccess : kIOReturnIOError;
    }
    default: return kIOReturnUnsupported;
    }
    if (fFirmwareDownloadActive && function != MPI2_FUNCTION_FW_DOWNLOAD)
        return kIOReturnBusy;
    if (!ieee && !nvme && length > 0x00FFFFFFU) return kIOReturnBadArgument;
    if (in.requestBytes != prefix || prefix > fRequestFrameSize ||
        (!nvme && (ieee || function == MPI2_FUNCTION_CONFIG || function == MPI2_FUNCTION_RAID_ACTION) &&
         prefix + (function == MPI2_FUNCTION_SMP_PASSTHROUGH ? 32U : 16U) > fRequestFrameSize))
        return kIOReturnBadArgument;
    if (mutation && (!(in.flags & kMPRControlAllowMutation) || !fMaintenanceMode))
        return kIOReturnNotPermitted;
    UInt16 handle = (scsi || nvme || function == MPI2_FUNCTION_SATA_PASSTHROUGH)
        ? mprControlLe16(user) : 0;
    UInt32 lun = 0;
    if (scsi && !mprDecodeLun(user + 52, &lun)) return kIOReturnBadArgument;
    if (nvme) {
        UInt32 nsid = mprControlLe32(user + 36);
        if (!(mprControlLe16(user + 26) & 0x10) && (!nsid || nsid > 0x1000000U))
            return kIOReturnBadArgument;
        if (nsid && nsid != UINT32_MAX) lun = nsid - 1;
    }
    MPRTargetSlot *slot = handle ? findTarget(mprTargetKey(handle, lun)) : NULL;
    if (!slot && nvme && (mprControlLe16(user + 26) & 0x10))
        slot = findTarget(handle); // Controller-scoped admin requests, not arbitrary namespace I/O.
    MPRTargetSlot member = {};
    if (!slot && handle && function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH &&
        fMaintenanceMode && !in.generation && in.argument0) {
        UInt8 page[512]; UInt32 actual = 0;
        bool isMember = false;
        if (!allocatePageBuffer() || !readRaidMembership(handle, &isMember) || !isMember ||
            !readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle, page, sizeof(page), &actual) ||
            actual < 0x24 || mprControlLe16(page + 0x18) != handle ||
            ((UInt64)mprControlLe32(page + 0x0C) | ((UInt64)mprControlLe32(page + 0x10) << 32)) != in.argument0)
            return kIOReturnNotReady;
        member.handle = handle; member.lun = lun; member.sasAddress = in.argument0;
        member.kind = kMPRTargetSas; member.present = true; member.hiddenByRaid = true;
        slot = &member;
    }
    MPRTargetSlot physicalTarget = {}; // private, never published or inserted in fTargets
    if ((scsi || nvme || function == MPI2_FUNCTION_SATA_PASSTHROUGH) &&
        (!handle || handle == 0xFFFF)) return kIOReturnBadArgument;
    if (handle) {
        if (slot) {
            if (!slot->present || slot->sasAddress != in.argument0 ||
                slot->generation != in.generation) return kIOReturnNotReady;
            if (nvme && slot->kind != kMPRTargetNvme) return kIOReturnBadArgument;
            if (nvme && slot->maxTransferBytes && length > slot->maxTransferBytes)
                return kIOReturnBadArgument;
        } else {
            /* Hidden RAID members do not have published target slots. Only an
             * explicitly authorized maintenance request with generation zero
             * may address them; re-read the exact firmware identity first. */
            if (!fMaintenanceMode || !(in.flags & kMPRControlAllowMutation) ||
                in.generation != 0 || !in.argument0 || !allocatePageBuffer())
                return kIOReturnNotReady;
            UInt8 page[512]; UInt32 actual = 0; UInt16 pageStatus = 0xFFFF;
            bool physicalNvme = nvme;
            bool found = readExtConfigPage(physicalNvme ? MPI2_CONFIG_EXTPAGETYPE_PCIE_DEVICE
                                                       : MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                (physicalNvme ? MPI26_PCIE_DEVICE_PGAD_FORM_HANDLE : MPI2_SAS_DEVICE_PGAD_FORM_HANDLE) | handle,
                page, sizeof(page), &actual, &pageStatus);
            if (!found && !physicalNvme && fNvmeProtocol &&
                pageStatus == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE && !fProtocolRecoveryPending) {
                physicalNvme = true;
                found = readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_PCIE_DEVICE,
                    MPI26_PCIE_DEVICE_PGAD_FORM_HANDLE | handle,
                    page, sizeof(page), &actual, &pageStatus);
            }
            if (!found || actual < 0x24 || mprControlLe16(page + 0x18) != handle ||
                page[0x17] != 0) return kIOReturnNotReady;
            UInt64 identity = (UInt64)mprControlLe32(page + 0x0C) |
                              ((UInt64)mprControlLe32(page + 0x10) << 32);
            UInt32 info = mprControlLe32(page + 0x1C), flags = mprControlLe16(page + 0x20);
            if (identity != in.argument0 || !(flags & 1U) || (flags & 0x8100U) ||
                (physicalNvme ? (info & MPI26_PCIE_DEVINFO_MASK_DEVICE_TYPE) != MPI26_PCIE_DEVINFO_NVME
                              : (info & 7U) != MPI2_SAS_DEVICE_INFO_END_DEVICE)) return kIOReturnNotReady;
            if (physicalNvme && function == MPI2_FUNCTION_SATA_PASSTHROUGH) return kIOReturnBadArgument;
            physicalTarget.kind = physicalNvme ? kMPRTargetNvme : kMPRTargetSas;
            physicalTarget.handle = handle; physicalTarget.lun = lun;
            physicalTarget.sasAddress = identity; physicalTarget.deviceInfo = info;
            physicalTarget.flags = (UInt16)flags;
            if (physicalNvme) {
                UInt8 limits[64]; UInt32 count = 0;
                if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_PCIE_DEVICE,
                    MPI26_PCIE_DEVICE_PGAD_FORM_HANDLE | handle,
                    limits, sizeof(limits), &count, NULL, 2) || count < 0x18 ||
                    mprControlLe16(limits + 8) != handle) return kIOReturnNotReady;
                physicalTarget.maxTransferBytes = mprControlLe32(limits + 12);
                if (nvme && physicalTarget.maxTransferBytes && length > physicalTarget.maxTransferBytes)
                    return kIOReturnBadArgument;
            }
            slot = &physicalTarget; // regular NVMe SCSI still needs the checked PRP/IEEE path
            mutation = true; // Physical-member access is never implicitly enabled.
        }
    }
    const UInt32 inputRegion = (in.dataOutBytes + 4095U) & ~4095U;
    const UInt32 dataRegion = (in.dataInBytes + 4095U) & ~4095U;
    const UInt32 required = inputRegion + dataRegion + 4096U;
    if (!allocateControlBuffer(required)) return kIOReturnNoResources;
    UInt8 *dma = (UInt8 *)fControlBuffer->getBytesNoCopy();
    bzero(dma, required);
    if (write) memcpy(dma, dataOut, in.dataOutBytes);
    const UInt64 address = fControlPhys + (read && !write ? inputRegion : 0);
    if (!beginManagementRequest(kMPRControlSmid)) return kIOReturnBusy;
    if (scsi || nvme) {
        if (!prepareScsiSense(kMPRControlSmid)) {
            cancelUnpostedCommand(kMPRControlSmid); return kIOReturnBusy;
        }
    }
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + kMPRControlSmid * fRequestFrameSize;
    bzero(frame, fRequestFrameSize); memcpy(frame, user, prefix);
    frame[2] = frame[8] = frame[9] = 0;
    /* User message flags cannot select a different sense/address space. */
    frame[7] = function == MPI2_FUNCTION_FW_DOWNLOAD ? (user[7] & 1U) : 0;
    if (scsi) {
        MPRScsiIoRequest *io = (MPRScsiIoRequest *)frame;
        io->SenseBufferLowAddress = (UInt32)senseAddress(kMPRControlSmid);
        io->SenseBufferLength = kMPRControlSenseBytes; io->SGLOffset0 = 24;
        io->SGLFlags = 0; io->SGLOffset1 = io->SGLOffset2 = io->SGLOffset3 = 0;
        io->DataLength = length; io->BidirectionalDataLength = 0;
        io->Control = write ? MPI2_SCSIIO_CONTROL_WRITE : read ? MPI2_SCSIIO_CONTROL_READ : MPI2_SCSIIO_CONTROL_NODATATRANSFER;
        /* MPI2_SCSIIO_CONTROL_ADDCDBLEN_SHIFT=26: 32-byte CDBs carry four additional dwords. */
        if ((io->IoFlags & 0x3F) > 16)
            io->Control |= (((io->IoFlags & 0x3F) - 16U + 3U) / 4U) << 26;
        io->IoFlags &= 0x3F; io->EEDPFlags = 0;
        if (!applyScsiTargetPolicy(frame, slot)) {
            cancelUnpostedCommand(kMPRControlSmid); return kIOReturnUnsupported;
        }
    }
    if (function == MPI2_FUNCTION_FW_DOWNLOAD || function == MPI2_FUNCTION_FW_UPLOAD)
        mprControlPut32(frame + 28, length);
    if (function == MPI2_FUNCTION_SATA_PASSTHROUGH) {
        mprControlPut32(frame + 24, length);
        UInt16 pt = mprControlLe16(frame + 4) & ~UInt16(3);
        mprControlPut16(frame + 4, pt | (write ? 2 : read ? 1 : 0)); frame[6] = 0;
    }
    if (function == MPI2_FUNCTION_TOOLBOX) { frame[12] = 0; mprControlPut32(frame + 16, length); }
    if (nvme) {
        MPRNvmeEncapRequest *io = (MPRNvmeEncapRequest *)frame;
        io->ErrorResponseBaseAddress = senseAddress(kMPRControlSmid);
        io->ErrorResponseAllocationLength = 16; io->DataLength = length;
        io->Flags = (io->Flags & 0x10) | (write ? 1 : read ? 2 : 0);
        bzero(io->Command + 16, 24); // MPTR and both PRPs
        if (length) {
            mprControlPut64(io->Command + 24, address);
            if (length > 4096 && length <= 8192) mprControlPut64(io->Command + 32, address + 4096);
            else if (length > 8192) {
                UInt32 prpOffset = inputRegion + dataRegion;
                for (UInt32 i = 1; i * 4096 < length; ++i)
                    mprControlPut64(dma + prpOffset + (i - 1) * 8, address + i * 4096);
                mprControlPut64(io->Command + 32, fControlPhys + prpOffset);
            }
        }
    } else if (function == MPI2_FUNCTION_SMP_PASSTHROUGH) {
        frame[0] &= ~0x80; frame[6] = 0; // MPI2.5 SMP: reserved SGLFlags; two IEEE64 SGEs.
        mprControlPut16(frame + 4, (UInt16)in.dataOutBytes);
        mprSglElement(frame + 32, fControlPhys, in.dataOutBytes, 0, 0);
        mprSglElement(frame + 48, fControlPhys + inputRegion, in.dataInBytes, 0, 0x40);
    } else if (scsi && slot && slot->kind == kMPRTargetNvme && length &&
               (!slot->maxTransferBytes || length <= slot->maxTransferBytes)) {
        MPRDmaSegment segment = {address, length}; MPRSglResult sgl = {};
        if (!mprBuildNvmeSgl(frame, fRequestFrameSize, 96, &segment, 1, length,
            dma + inputRegion + dataRegion, fControlPhys + inputRegion + dataRegion, 4096, &sgl)) {
            cancelUnpostedCommand(kMPRControlSmid); return kIOReturnNoResources;
        }
        frame[2] = sgl.requestChainOffset;
    } else if (ieee) {
        mprSglElement(frame + prefix, address, length, 0, 0x40);
    } else if (function == MPI2_FUNCTION_CONFIG || function == MPI2_FUNCTION_RAID_ACTION) {
        UInt32 sgeFlags = 0xD3000000U | (write ? 0x04000000U : 0U); // simple,last,64bit,EOB,EOL
        mprControlPut32(frame + prefix, sgeFlags | length);
        mprControlPut64(frame + prefix + 4, address);
    }
    if (function == MPI2_FUNCTION_FW_DOWNLOAD) {
        fFirmwareDownloadActive = true; fFirmwareToken = in.argument1;
        fFirmwareImageType = user[0]; fFirmwareTotal = mprControlLe32(user + 12);
        fFirmwareOffset = mprControlLe32(user + 24);
    }
    UInt8 kind = 0;
    if (!submitControlFrame(descriptor, high, mprControlTimeout(in), out->reply, &kind)) {
        if (function == MPI2_FUNCTION_FW_DOWNLOAD) fFirmwareDownloadPoisoned = true;
        return kIOReturnTimeout;
    }
    if (kind == 3) return kIOReturnAborted;
    if (kind == 1) {
        UInt32 replyBytes = out->reply[2] * 4U;
        if (replyBytes < 20 || replyBytes > kMPRControlMaxReply) return kIOReturnOverrun;
        out->replyBytes = replyBytes;
    } else if (kind != 2 && kind != 4) return kIOReturnIOError;
    UInt32 actual = in.dataInBytes;
    if (scsi && kind == 1) {
        if (out->replyBytes < MPR_SCSIIO_REPLY_MIN_BYTES) return kIOReturnUnderrun;
        const MPRScsiIoReply *reply = (const MPRScsiIoReply *)out->reply;
        actual = read ? reply->TransferCount : 0;
        if (reply->SenseCount > kMPRControlSenseBytes) return kIOReturnOverrun;
        if (reply->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_VALID) out->senseBytes = reply->SenseCount;
    } else if (nvme && kind == 1) out->senseBytes = mprControlLe16(out->reply + 20);
    else if (function == MPI2_FUNCTION_SMP_PASSTHROUGH && kind == 1)
        actual = mprControlLe16(out->reply + 4);
    else if (function == MPI2_FUNCTION_SATA_PASSTHROUGH && kind == 1) {
        if (out->replyBytes < 48) return kIOReturnUnderrun;
        UInt32 transferred = mprControlLe32(out->reply + 44);
        if (transferred > length) return kIOReturnOverrun;
        actual = read ? transferred : 0;
    }
    if (actual > in.dataInBytes || out->senseBytes > kMPRControlSenseBytes) return kIOReturnOverrun;
    if (out->senseBytes) memcpy(out->sense,
        (UInt8 *)fSenseBuffer->getBytesNoCopy() + kMPRControlSmid * kMPRControlSenseBytes, out->senseBytes);
    if (actual) memcpy(out + 1, dma + inputRegion, actual);
    out->dataBytes = actual;
    if (function == MPI2_FUNCTION_FW_DOWNLOAD) {
        const bool success = kind == 1 &&
            (mprControlLe16(out->reply + 14) & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS;
        if (!success) fFirmwareDownloadPoisoned = true;
        else {
            fFirmwareOffset += length;
            if (fFirmwareOffset == fFirmwareTotal) fFirmwareDownloadActive = false;
        }
    }
    if (mutation && !fFirmwareDownloadActive) { fTopologyChanged = true; fHotplugPending = true; }
    const UInt16 status = kind == 1 ? mprControlLe16(out->reply + 14) & MPI2_IOCSTATUS_MASK : 0;
    if (mutation || status != MPI2_IOCSTATUS_SUCCESS)
        MPR_DEBUG_LOG("MPR control: function=0x%02x handle=0x%04x mutation=%u IOC=0x%04x\n",
            function, handle, mutation, status);
    if (status != MPI2_IOCSTATUS_SUCCESS && status != MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN)
        return kIOReturnIOError;
    if ((function == MPI2_FUNCTION_SMP_PASSTHROUGH || function == MPI2_FUNCTION_SATA_PASSTHROUGH) &&
        kind == 1 && out->reply[13] != 0) return kIOReturnDeviceError;
    if (scsi && kind == 1 && out->reply[12] != MPI2_SCSI_STATUS_GOOD) return kIOReturnDeviceError;
    if (nvme && out->senseBytes &&
        (out->senseBytes < 16 || (mprControlLe16(out->sense + 14) & 0x0FFE)))
        return kIOReturnDeviceError;
    return kIOReturnSuccess;
}

/* Diagnostic DMA survives the command that posts it. Only a successful
 * release, a checked unsolicited release notification, or a proved IOC stop
 * relinquishes firmware ownership. No close/unregister path guesses this. */
IOReturn MPRController::controlDiagnostic(const MPRControlInput &in,
    const UInt8 *request, MPRControlOutput *out)
{
    if (in.argument0 >= 3 || !fReplyLock) return kIOReturnBadArgument;
    const UInt32 type = (UInt32)in.argument0;
    DiagnosticBuffer &buffer = fDiagnostic[type];
    UInt8 *data = (UInt8 *)(out + 1);
    bool owned = false, valid = false;
    IOSimpleLockLock(fReplyLock);
    owned = buffer.owned; valid = buffer.valid;
    IOSimpleLockUnlock(fReplyLock);
    if (in.operation == kMPRControlDiagQuery) {
        if (in.requestBytes || in.dataInBytes < sizeof(MPRControlDiagInfo))
            return kIOReturnBadArgument;
        MPRControlDiagInfo *info = (MPRControlDiagInfo *)data;
        info->type = type; info->uniqueId = buffer.uniqueId; info->bytes = buffer.bytes;
        info->flags = (buffer.memory ? 1 : 0) | (owned ? 2 : 0) | (valid ? 4 : 0);
        info->epoch = buffer.epoch;
        out->dataBytes = sizeof(*info);
        return kIOReturnSuccess;
    }
    if (in.operation != kMPRControlDiagRegister &&
        (!buffer.memory || in.generation != buffer.uniqueId)) return kIOReturnNotFound;
    if (in.operation == kMPRControlDiagRead) {
        if (in.requestBytes || !buffer.memory || !valid || owned ||
            in.argument1 > buffer.bytes || in.dataInBytes > buffer.bytes - in.argument1)
            return owned ? kIOReturnBusy : kIOReturnBadArgument;
        if (in.dataInBytes) memcpy(data,
            (const UInt8 *)buffer.memory->getBytesNoCopy() + in.argument1, in.dataInBytes);
        out->dataBytes = in.dataInBytes;
        return kIOReturnSuccess;
    }
    if (!(in.flags & kMPRControlAllowMutation) || fSuspended || fReinitInProgress ||
        fProtocolRecoveryPending || fStopping || !fRequestBuffer || !fFactsValid ||
        fControlDmaBusy) return kIOReturnNotReady;
    if (in.operation == kMPRControlDiagUnregister && !owned) {
        if (in.requestBytes) return kIOReturnBadArgument;
        IOBufferMemoryDescriptor *memory = buffer.memory;
        const bool prepared = buffer.prepared;
        IOSimpleLockLock(fReplyLock); bzero(&buffer, sizeof(buffer)); IOSimpleLockUnlock(fReplyLock);
        if (prepared) memory->complete();
        memory->release();
        return kIOReturnSuccess;
    }
    if (in.operation == kMPRControlDiagRegister) {
        const UInt32 capabilities[3] = {
            MPI2_IOCFACTS_CAPABILITY_DIAG_TRACE_BUFFER,
            MPI2_IOCFACTS_CAPABILITY_SNAPSHOT_BUFFER,
            MPI2_IOCFACTS_CAPABILITY_EXTENDED_BUFFER};
        if (!(fIocCapabilities & capabilities[type])) return kIOReturnUnsupported;
        /* A 100-byte descriptor: uniqueId, extendedType, padding and 23 product
         * words are represented by the request prefix below. Addresses and
         * lengths are always generated by the driver. */
        if (in.requestBytes != 100 || !in.argument1 || in.argument1 > kMPRControlMaxDiagBytes || (in.argument1 & 3) ||
            buffer.memory || owned || !mprControlLe32(request) || (request[4] > 2) ||
            request[5] || request[6] || request[7] || fRequestFrameSize < 128)
            return kIOReturnBadArgument;
        UInt32 unique = mprControlLe32(request);
        for (unsigned other = 0; other < 3; ++other)
            if (fDiagnostic[other].memory && fDiagnostic[other].uniqueId == unique)
                return kIOReturnExclusiveAccess;
        IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            (UInt32)in.argument1, UINT64_C(0x00000000ffffffff));
        if (!memory) return kIOReturnNoMemory;
        IOByteCount segmentBytes = 0;
        UInt64 address = memory->getPhysicalSegment(0, &segmentBytes, kIOMemoryMapperNone);
        bool prepared = memory->prepare() == kIOReturnSuccess;
        if (!prepared || !address || segmentBytes < in.argument1 ||
            address > UINT32_MAX || in.argument1 - 1 > UINT32_MAX - address ||
            !memory->getBytesNoCopy()) {
            if (prepared) memory->complete();
            memory->release(); return kIOReturnNoResources;
        }
        memset(memory->getBytesNoCopy(), 0, (size_t)in.argument1);
        IOSimpleLockLock(fReplyLock);
        buffer.memory = memory; buffer.address = address;
        buffer.bytes = (UInt32)in.argument1; buffer.uniqueId = unique;
        buffer.prepared = true; buffer.epoch = fTransportEpoch;
        IOSimpleLockUnlock(fReplyLock);
    } else if (in.requestBytes || !buffer.memory) return kIOReturnBadArgument;
    if (in.operation == kMPRControlDiagRelease && !owned) return kIOReturnSuccess;
    if (!beginManagementRequest(kMPRControlSmid)) return kIOReturnBusy;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + kMPRControlSmid * fRequestFrameSize;
    bzero(frame, fRequestFrameSize);
    frame[1] = (UInt8)type;
    const bool posting = in.operation == kMPRControlDiagRegister;
    frame[3] = posting ? MPI2_FUNCTION_DIAG_BUFFER_POST : MPI2_FUNCTION_DIAG_RELEASE;
    if (posting) {
        frame[0] = request[4];
        mprControlPut64(frame + 12, buffer.address);
        mprControlPut32(frame + 20, buffer.bytes);
        memcpy(frame + 36, request + 8, 92);
        IOSimpleLockLock(fReplyLock);
        buffer.owned = true; buffer.valid = false;
        IOSimpleLockUnlock(fReplyLock);
    }
    UInt8 kind = 0;
    if (!submitControlFrame(MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0,
                            mprControlTimeout(in), out->reply, &kind)) {
        /* Conservative ownership persists even if we cannot distinguish a
         * failed post from a lost completion. Recovery owns the DMA proof. */
        return kIOReturnTimeout;
    }
    if (kind != 1 || out->reply[1] != type || out->reply[2] * 4U < 20 ||
        out->reply[2] * 4U > kMPRControlMaxReply)
        return kIOReturnIOError;
    out->replyBytes = out->reply[2] * 4U;
    UInt16 status = mprControlLe16(out->reply + 14) & MPI2_IOCSTATUS_MASK;
    IOSimpleLockLock(fReplyLock);
    if (status == MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED ||
        (!posting && status == MPI2_IOCSTATUS_SUCCESS)) {
        buffer.owned = false; buffer.valid = true;
    } else if (posting && status != MPI2_IOCSTATUS_SUCCESS) {
        buffer.owned = false; buffer.valid = false;
    }
    owned = buffer.owned;
    IOSimpleLockUnlock(fReplyLock);
    MPR_DEBUG_LOG("MPR diagnostic: type=%u operation=%u IOC=0x%04x owned=%u bytes=%u\n",
        (unsigned)type, (unsigned)in.operation, status, owned, (unsigned)buffer.bytes);
    if (status != MPI2_IOCSTATUS_SUCCESS && status != MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED)
        return kIOReturnIOError;
    if (in.operation == kMPRControlDiagUnregister && !owned) {
        IOBufferMemoryDescriptor *memory = buffer.memory;
        const bool prepared = buffer.prepared;
        IOSimpleLockLock(fReplyLock); bzero(&buffer, sizeof(buffer)); IOSimpleLockUnlock(fReplyLock);
        if (prepared) memory->complete();
        memory->release();
    }
    return kIOReturnSuccess;
}

bool MPRController::shutdownDevices(void)
{
    bool clean = true;
    const UInt64 deadline = uptimeMilliseconds() + 60000;
    bool volume = false;
    for (UInt32 i = 0; i < kMPRMaxTargets; ++i)
        if (fTargets[i].handle && fTargets[i].present && fTargets[i].kind == kMPRTargetRaidVolume) volume = true;
    if (volume && (fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID) &&
        fRequestBuffer && fRequestFrameSize >= 32 && beginManagementRequest(kMPRControlSmid)) {
        UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + kMPRControlSmid * fRequestFrameSize;
        bzero(frame, fRequestFrameSize); frame[0] = MPI2_RAID_ACTION_SYSTEM_SHUTDOWN_INITIATED;
        frame[3] = MPI2_FUNCTION_RAID_ACTION;
        mprControlPut32(frame + 20, 0xD3000000U);
        UInt8 reply[kMPRControlMaxReply] = {}, kind = 0;
        clean = submitControlFrame(MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0, 5000, reply, &kind) &&
            kind == 1 && reply[2] * 4U >= 20 && (mprControlLe16(reply + 14) & MPI2_IOCSTATUS_MASK) == 0;
        MPR_DEBUG_LOG("MPR: IR shutdown notification=%u\n", clean);
    }
    for (UInt32 i = 0; i < kMPRMaxTargets && !fProtocolRecoveryPending; ++i) {
        MPRTargetSlot *slot = &fTargets[i];
        if (!slot->handle || !slot->present || !slot->probeValid || slot->lun || slot->managementOnly ||
            slot->kind != kMPRTargetSas || !(slot->deviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) ||
            !fShutdownSsuPolicy || (fShutdownSsuPolicy == 1 && !slot->solidState) ||
            (fShutdownSsuPolicy == 2 && (!slot->rotationKnown || slot->solidState))) continue;
        if (uptimeMilliseconds() >= deadline) { clean = false; break; }
        if (!selectTarget(slot->key())) { clean = false; continue; }
        UInt8 cdb[6] = {0x1B, 0, 0, 0, 0, 0};
        IOReturn status = kIOReturnNotReady;
        if (!issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_NODATATRANSFER,
                              NULL, 0, 9, &status)) clean = false;
        MPR_DEBUG_LOG("MPR: shutdown SSU handle=0x%04x SSD=%u status=0x%x\n",
                      slot->handle, slot->solidState, (unsigned)status);
    }
    return clean && !fProtocolRecoveryPending;
}
