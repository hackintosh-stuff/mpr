#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <kern/task.h>
#include <string.h>
#include "MPRController.h"
#include "MPI/mpi2_min.h"

#define super IOService
OSDefineMetaClassAndStructors(MPRController, IOService)

#define MPR_VENDOR_DEVICE 0x00971000U

/* First memory BAR on this card is BAR1 (config register 0x14), 64 KB MMIO.
 * The physical address is assigned at boot and differs between macOS and
 * Linux, so always map by register offset and never hardcode an address. */
#define MPR_MMIO_BAR_REG 0x14

/* Hard bounds for every hardware wait. start() runs in the boot path, so the
 * whole M1-a probe must finish quickly instead of using the 300 s timeouts
 * of the FreeBSD driver. */
#define MPR_DIAG_UNLOCK_TRIES   20
#define MPR_DIAG_SETTLE_TRIES   100
#define MPR_READY_TRIES         60
#define MPR_ACK_ITERATIONS      4000
#define MPR_DBINT_ITERATIONS    4000

/* M1-b queue geometry. The reply free/post queues must hold one unused entry
 * and be a multiple of 16 (mpr.c:1379-1380); IOC Init advertises the same
 * depths and the request frame size in 32-bit words (mpr.c:1264-1266). */
#define MPR_M1B_REPLY_FRAMES    128
#define MPR_M1B_REQUEST_FRAMES  128
#define MPR_M1B_STATE_TRIES     1000
#define MPR_DMA_LOW_MASK        0x00000000FFFFFFFFULL

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

    IOLog("MPR M1a: MMIO dump: Doorbell=0x%08x HostDiag=0x%08x HIS=0x%08x HIM=0x%08x\n",
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

    /* Keep interrupts masked while the IOC is being reset. M1-a polls only. */
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
        IOLog("MPR M1a: diag write-enable never set\n");
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

    IOLog("MPR M1a: diag reset did not leave RESET state\n");
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
            IOLog("MPR M1a: another PCI peer owns the IOC, refusing\n");
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

    IOLog("MPR M1a: IOC never reached READY\n");
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
        setProperty("MPRM1aFailure", "IOC Facts handshake failed");
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

    setProperty("MPRM1aFactsFunction", (unsigned long long)facts.Function, 8);
    setProperty("MPRM1aFactsStatus",
                (unsigned long long)(facts.IOCStatus & MPI2_IOCSTATUS_MASK), 16);

    IOLog("MPR M1a: IOC Facts: status=0x%04x FW=0x%08x credit=%u replyDepth=%u frameSize=%u\n",
          (unsigned int)(facts.IOCStatus & MPI2_IOCSTATUS_MASK),
          (unsigned int)facts.FWVersion,
          (unsigned int)facts.RequestCredit,
          (unsigned int)facts.MaxReplyDescriptorPostQueueDepth,
          (unsigned int)facts.IOCRequestFrameSize);

    ok = (facts.Function == MPI2_FUNCTION_IOC_FACTS) &&
         ((facts.IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS);
    if (ok) {
        /* Keep the geometry for M1-b; both frame sizes are in 32-bit words. */
        fRequestFrameSize = (UInt32)facts.IOCRequestFrameSize * 4;
        fReplyFrameSize   = (UInt32)facts.ReplyFrameSize * 4;
        fRequestCredit    = facts.RequestCredit;
        fMaxReplyDepth    = facts.MaxReplyDescriptorPostQueueDepth;
        fFactsValid       = true;
    }
    return ok;
}

bool
MPRController::runM1a(void)
{
    UInt32 initialState, finalState;

    dumpRegisters();
    initialState = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    setProperty("MPRM1aInitialState", (unsigned long long)initialState, 32);

    if (!transitionToReady()) {
        setProperty("MPRM1aFailure", "cannot reach READY");
        setProperty("MPRM1aState", "failed-ready");
        return false;
    }
    setProperty("MPRM1aReachedReady", true);

    if (!getIOCFacts()) {
        setProperty("MPRM1aState", "failed-facts");
        return false;
    }

    finalState = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    setProperty("MPRM1aFinalState", (unsigned long long)finalState, 32);
    setProperty("MPRM1aState", "ok");
    IOLog("MPR M1a: completed, IOC state after IOC Facts = 0x%08x\n",
          (unsigned int)finalState);
    return true;
}

/* ------------------------------------------------------------------------ */
/* M1-b: synchronous IOC Init with a low-4GB, physically contiguous pool.    */
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

    if (!fFactsValid || fReplyFrameSize == 0 || fRequestFrameSize == 0) {
        setProperty("MPRM1bFailure", "no usable IOC Facts geometry");
        return false;
    }

    fFqDepth        = mprRoundUp16(MPR_M1B_REPLY_FRAMES + 1);
    fPqDepth        = fFqDepth;
    fRequestCount   = MPR_M1B_REQUEST_FRAMES;
    fReplyFreeIndex = MPR_M1B_REPLY_FRAMES;

    queueBytes   = fFqDepth * 4 + fPqDepth * 8;
    replyBytes   = fReplyFrameSize * fFqDepth;
    requestBytes = fRequestFrameSize * fRequestCount;

    if (fRequestCount > fRequestCredit || fFqDepth > fMaxReplyDepth) {
        setProperty("MPRM1bFailure", "geometry exceeds IOC credit");
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

    if (fQueueBuffer == NULL || fReplyBuffer == NULL || fRequestBuffer == NULL) {
        setProperty("MPRM1bFailure", "DMA pool allocation failed");
        return false;
    }

    fFreePhys = fQueueBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fFreePhys == 0 || segLength < queueBytes) {
        setProperty("MPRM1bFailure", "queue pool is not physically contiguous");
        return false;
    }
    fPostPhys = fFreePhys + (UInt64)fFqDepth * 4;

    fReplyPhys = fReplyBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fReplyPhys == 0 || segLength < replyBytes) {
        setProperty("MPRM1bFailure", "reply pool is not physically contiguous");
        return false;
    }

    fRequestPhys = fRequestBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fRequestPhys == 0 || segLength < requestBytes) {
        setProperty("MPRM1bFailure", "request pool is not physically contiguous");
        return false;
    }

    if ((fFreePhys + queueBytes) > (MPR_DMA_LOW_MASK + 1) ||
        (fReplyPhys + replyBytes) > (MPR_DMA_LOW_MASK + 1) ||
        (fRequestPhys + requestBytes) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRM1bFailure", "DMA pool crosses the 4 GB boundary");
        return false;
    }

    queue = (UInt8 *)fQueueBuffer->getBytesNoCopy();
    if (queue == NULL) {
        setProperty("MPRM1bFailure", "queue pool has no virtual address");
        return false;
    }

    memset(queue, 0, queueBytes);
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
    out.request.SenseBufferAddressHigh          = 0;
    out.request.SystemReplyAddressHigh          = 0;
    out.request.SystemRequestFrameBaseAddress   = fRequestPhys;
    out.request.ReplyDescriptorPostQueueAddress = fPostPhys;
    out.request.ReplyFreeQueueAddress           = fFreePhys;
    out.request.HostPageSize                    = MPR_HOST_PAGE_SIZE_4K;
    out.request.HostMSIxVectors                 = 0;
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
        setProperty("MPRM1bFailure", "IOC Init handshake failed");
        return false;
    }

    memcpy(&answer, in.words, sizeof(answer));
    status = (UInt16)(answer.IOCStatus & MPI2_IOCSTATUS_MASK);
    setProperty("MPRIOCInitStatus",  (unsigned long long)status, 16);
    setProperty("MPRIOCInitLogInfo", (unsigned long long)answer.IOCLogInfo, 32);
    IOLog("MPR M1b: IOC Init status=0x%04x loginfo=0x%08x\n",
          (unsigned int)status, (unsigned int)answer.IOCLogInfo);

    return status == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPRController::runM1b(void)
{
    UInt32 state = MPI2_IOC_STATE_RESET;
    UInt32 tries;

    if (!allocateQueueMemory()) {
        setProperty("MPRM1bState", "failed-alloc");
        return false;
    }
    if (!sendIOCInit()) {
        setProperty("MPRM1bState", "failed-iocinit");
        return false;
    }

    state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    for (tries = 0; tries < MPR_M1B_STATE_TRIES &&
                    state != MPI2_IOC_STATE_OPERATIONAL; tries++) {
        IODelay(1000);
        state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    }
    setProperty("MPRIOCState", (unsigned long long)state, 32);

    if (state != MPI2_IOC_STATE_OPERATIONAL) {
        setProperty("MPRM1bState", "failed-not-operational");
        return false;
    }

    /* The IOC resets these indices during the state transition, so program
     * them only after OPERATIONAL is observed (mpr.c:700-712). */
    writeReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET, fReplyFreeIndex);
    writeReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET, 0);
    setProperty("MPRReplyFreeHostIndex", (unsigned long long)fReplyFreeIndex, 32);
    setProperty("MPRReplyPostHostIndex", 0ULL, 32);
    setProperty("MPRM1bState", "ok");
    IOLog("MPR M1b: IOC OPERATIONAL, freeDepth=%u postDepth=%u requests=%u\n",
          (unsigned int)fFqDepth, (unsigned int)fPqDepth,
          (unsigned int)fRequestCount);
    return true;
}

bool
MPRController::start(IOService *provider)
{
    IOPCIDevice *pci;
    IOByteCount  length;

    if (!super::start(provider)) {
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
    setProperty("MPRStage", "M1b");
    setProperty("MPRBuild", "0.3.0");

    /* Failures inside M1-a/M1-b are recorded in IORegistry and never panic the
     * machine or prevent the kext from loading. M1-b issues IOC Init only:
     * no interrupts, no config pages and no I/O requests. */
    if (runM1a()) {
        runM1b();
    } else {
        setProperty("MPRM1bState", "skipped-m1a-failed");
    }

    IOLog("MPR M1b: start complete\n");
    return true;
}

void
MPRController::stop(IOService *provider)
{
    freeQueueMemory();
    if (fMMIO != NULL) {
        fMMIO->release();
        fMMIO = NULL;
    }
    fRegs = NULL;
    fPCIDevice = NULL;
    super::stop(provider);
}
