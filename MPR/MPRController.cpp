#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <kern/task.h>
#include <string.h>
#include "MPRController.h"
#include "MPRBlockDev.h"
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

/* M1-c polls the reply queue instead of taking interrupts. */
#define MPR_M1C_TRIES           2000
#define MPR_M1C_PAGE_BYTES      128

/* M2-a waits for the reply interrupt, then falls back to one poll pass. */
#define MPR_M2_REPLY_TIMEOUT_MS 2000

/* M2-c config page staging buffer (SAS IO Unit Page0 with 8+ phys). */
#define MPR_PAGE_BUF_BYTES      512

/* M3-a: sense buffer lives in the upper half of the page staging buffer. */
#define MPR_SENSE_OFFSET        0x100

/* M3-c: bulk transfer staging buffer. */
#define MPR_DATA_BUF_BYTES      65536

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

    /* Unmask only the reply-descriptor interrupt; every other cause stays
     * masked so a stuck status bit cannot become an interrupt storm. */
    if (fInterrupt != NULL) {
        writeReg(MPI2_HOST_INTERRUPT_MASK_OFFSET, ~MPI2_HIM_REPLY_INT_MASK);
    }
    setProperty("MPRM1bState", "ok");
    IOLog("MPR M1b: IOC OPERATIONAL, freeDepth=%u postDepth=%u requests=%u\n",
          (unsigned int)fFqDepth, (unsigned int)fPqDepth,
          (unsigned int)fRequestCount);
    return true;
}

/* ------------------------------------------------------------------------ */
/* M1-c: one synchronous config request (Manufacturing Page0) submitted      */
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
/* M2-a: reply-descriptor interrupt delivered through a filtered event        */
/* source. The filter only samples and clears the cause; the reply queue is   */
/* drained on the work loop (PLAN section 5, rule 4).                        */

bool
MPRController::setupInterrupts(void)
{
    int type = -1;

    fWorkLoop = IOWorkLoop::workLoop();
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
    if (fWorkLoop != NULL) {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
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
    UInt8 *queue, *post, *replyBase;
    UInt32 i, baddr, replyBytes;
    UInt64 entry, clear;
    UInt8  flags;

    if (fQueueBuffer == NULL || fReplyBuffer == NULL) {
        return;
    }
    queue = (UInt8 *)fQueueBuffer->getBytesNoCopy();
    replyBase = (UInt8 *)fReplyBuffer->getBytesNoCopy();
    if (queue == NULL || replyBase == NULL) {
        return;
    }
    post = queue + fFqDepth * 4;
    replyBytes = fReplyFrameSize * fFqDepth;

    for (i = 0; i < fPqDepth; i++) {
        memcpy(&entry, post + fReplyPostIndex * 8, sizeof(entry));
        flags = (UInt8)(entry & MPI2_RPY_DESCRIPT_FLAGS_TYPE_MASK);
        if (flags == MPI2_RPY_DESCRIPT_FLAGS_UNUSED ||
            ((entry >> 32) & 0xFFFFFFFFULL) == 0xFFFFFFFFULL) {
            break;
        }
        baddr = (UInt32)(entry >> 32);
        clear = 0xFFFFFFFFFFFFFFFFULL;
        memcpy(post + fReplyPostIndex * 8, &clear, sizeof(clear));
        fReplyPostIndex = (fReplyPostIndex + 1) % fPqDepth;
        if (flags == MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY &&
            baddr >= (UInt32)fReplyPhys &&
            baddr < (UInt32)fReplyPhys + replyBytes) {
            UInt8 *replyFrame = replyBase + (baddr - (UInt32)fReplyPhys);

            if (replyFrame[3] == MPI2_FUNCTION_EVENT_NOTIFICATION) {
                MPREventNotifyReply *event = (MPREventNotifyReply *)replyFrame;

                /* Asynchronous firmware event (also carries the reply to the
                 * mask-setting request itself). */
                memcpy(fLastEventData, replyFrame, sizeof(fLastEventData));
                fLastEvent = event->Event;
                fLastEventDataLength = event->EventDataLength;
                fLastEventAckRequired = event->AckRequired;
                fLastEventContext = event->EventContext;
                if (fLastEvent < 128) {
                    fEventSeen[fLastEvent / 32] |= (1U << (fLastEvent % 32));
                }
                fEventCount++;
            } else {
                memcpy(fLastReply, replyFrame, sizeof(fLastReply));
                fLastReplySmid = (UInt16)((entry >> 16) & 0xFFFF);
                fLastReplyKind = 1;
                fReplyCount++;
            }
        } else if (flags == MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS ||
                   flags == MPI25_RPY_DESCRIPT_FLAGS_FAST_PATH_SCSI_IO_SUCCESS) {
            /* A successful SCSI IO carries no reply frame; only the SMID. */
            fLastReplySmid = (UInt16)((entry >> 16) & 0xFFFF);
            fLastReplyKind = 2;
            memset(fLastReply, 0, sizeof(fLastReply));
            fScsiSuccess++;
            fReplyCount++;
        }
    }
    writeReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET, fReplyPostIndex);
}

bool
MPRController::waitForReply(UInt32 timeoutMs)
{
    UInt32 i;

    for (i = 0; i < timeoutMs; i++) {
        if (fReplyCount > 0) {
            fReplyViaIrq++;
            return true;
        }
        IOSleep(1);
    }
    /* Fallback when the interrupt source could not be installed. */
    processReplyQueue();
    return fReplyCount > 0;
}

/* ------------------------------------------------------------------------ */
/* M2-b: install the event mask and enable the SAS port. Firmware events are  */
/* collected asynchronously by processReplyQueue().                          */

static void
mprSetEventBit(UInt32 *masks, UInt32 event)
{
    masks[event / 32] |= (1U << (event % 32));
}

bool
MPRController::sendEventNotification(void)
{
    UInt8 *frame;
    MPREventNotifyRequest *request;
    UInt32 value32;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 2 * fRequestFrameSize;     /* SMID 2 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPREventNotifyRequest *)frame;

    request->Function = MPI2_FUNCTION_EVENT_NOTIFICATION;
    mprSetEventBit(request->EventMasks, MPI2_EVENT_STATE_CHANGE);
    mprSetEventBit(request->EventMasks, MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE);
    mprSetEventBit(request->EventMasks, MPI2_EVENT_SAS_DISCOVERY);
    mprSetEventBit(request->EventMasks, MPI2_EVENT_SAS_BROADCAST_PRIMITIVE);
    mprSetEventBit(request->EventMasks, MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST);
    mprSetEventBit(request->EventMasks, MPI2_EVENT_LOG_ENTRY_ADDED);
    mprSetEventBit(request->EventMasks, MPI2_EVENT_SAS_DEVICE_DISCOVERY_ERROR);

    setProperty("MPREventMask0", (unsigned long long)request->EventMasks[0], 32);
    setProperty("MPREventMask1", (unsigned long long)request->EventMasks[1], 32);

    value32 = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE | (2U << 16);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, value32);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);
    return true;
}

bool
MPRController::sendPortEnable(void)
{
    UInt8 *frame;
    MPRPortEnableRequest *request;
    UInt32 value32;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 3 * fRequestFrameSize;     /* SMID 3 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRPortEnableRequest *)frame;
    request->Function = MPI2_FUNCTION_PORT_ENABLE;
    request->PortFlags = 0;

    fReplyCount = 0;
    value32 = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE | (3U << 16);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, value32);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);
    if (!waitForReply(MPR_M2_REPLY_TIMEOUT_MS)) {
        return false;
    }
    {
        MPRPortEnableReply *reply = (MPRPortEnableReply *)fLastReply;

        setProperty("MPRPortEnableStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRPortEnableLogInfo",
                    (unsigned long long)reply->IOCLogInfo, 32);
    }
    return true;
}

bool
MPRController::runM2b(void)
{
    bool notified, enabled;

    notified = sendEventNotification();
    setProperty("MPREventNotify", notified ? "posted" : "failed");
    if (!notified) {
        setProperty("MPRM2bState", "failed-notify");
        return false;
    }

    enabled = sendPortEnable();
    setProperty("MPRPortEnable", enabled ? "ok" : "failed");
    if (!enabled) {
        setProperty("MPRM2bState", "failed-portenable");
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
    setProperty("MPRM2bState", "ok");
    IOLog("MPR M2b: events=%u last=0x%04x len=%u ack=%u\n",
          (unsigned int)fEventCount, (unsigned int)fLastEvent,
          (unsigned int)fLastEventDataLength, (unsigned int)fLastEventAckRequired);
    return true;
}

/* ------------------------------------------------------------------------ */
/* M2-c: extended config page read, used to build the SAS target table.      */

bool
MPRController::readExtConfigPage(UInt8 extPageType, UInt32 pageAddress,
                                 UInt8 *buffer, UInt32 bufferBytes,
                                 UInt32 *actualBytes)
{
    MPRConfigRequest *config;
    MPRConfigReply *reply;
    UInt8 *frame;
    UInt32 value32, headerDwords;

    if (bufferBytes == 0 || bufferBytes > MPR_PAGE_BUF_BYTES ||
        fRequestBuffer == NULL || fPageBuffer == NULL) {
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 4 * fRequestFrameSize;     /* SMID 4 */

    /* Step 1: page header without a data SGE. */
    memset(frame, 0, fRequestFrameSize);
    config = (MPRConfigRequest *)frame;
    config->Function = MPI2_FUNCTION_CONFIG;
    config->Action = MPI2_CONFIG_ACTION_PAGE_HEADER;
    config->Header.PageType = MPI2_CONFIG_PAGETYPE_EXTENDED;
    config->Header.PageNumber = 0;
    config->Header.PageLength = 0;
    config->ExtPageType = extPageType;
    config->ExtPageLength = 0;
    config->PageAddress = pageAddress;

    fReplyCount = 0;
    value32 = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE | (4U << 16);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, value32);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);
    if (!waitForReply(MPR_M2_REPLY_TIMEOUT_MS)) {
        setProperty("MPRConfigExtFailure", "page header timeout");
        return false;
    }
    reply = (MPRConfigReply *)fLastReply;
    setProperty("MPRConfigExtStatus",
                (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        setProperty("MPRConfigExtFailure", "page header status");
        return false;
    }
    headerDwords = reply->ExtPageLength;
    if (headerDwords == 0 || (headerDwords * 4) > bufferBytes) {
        headerDwords = bufferBytes / 4;
    }
    setProperty("MPRConfigExtLength", (unsigned long long)headerDwords, 16);

    /* Step 2: page data through the SGE, same SMID. */
    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_PAGE_BUF_BYTES);
    memset(frame, 0, fRequestFrameSize);
    config->Function = MPI2_FUNCTION_CONFIG;
    config->Action = MPI2_CONFIG_ACTION_PAGE_READ_CURRENT;
    config->Header.PageType = MPI2_CONFIG_PAGETYPE_EXTENDED;
    config->Header.PageNumber = 0;
    config->Header.PageLength = 0;
    config->ExtPageType = extPageType;
    config->ExtPageLength = (UInt16)headerDwords;
    config->PageAddress = pageAddress;
    config->SgeFlagsLength =
        ((UInt32)(MPI2_SGE_FLAGS_SIMPLE_ELEMENT | MPI2_SGE_FLAGS_LAST_ELEMENT |
                  MPI2_SGE_FLAGS_END_OF_BUFFER | MPI2_SGE_FLAGS_64_BIT_ADDRESSING)
         << MPI2_SGE_FLAGS_SHIFT) |
        (bufferBytes & MPI2_SGE_LENGTH_MASK);
    config->SgeAddress = fPagePhys;

    fReplyCount = 0;
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, value32);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);
    if (!waitForReply(MPR_M2_REPLY_TIMEOUT_MS)) {
        setProperty("MPRConfigExtFailure", "page data timeout");
        return false;
    }
    reply = (MPRConfigReply *)fLastReply;
    setProperty("MPRConfigExtDataStatus",
                (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        setProperty("MPRConfigExtFailure", "page data status");
        return false;
    }

    memcpy(buffer, fPageBuffer->getBytesNoCopy(), bufferBytes);
    if (actualBytes != NULL) {
        *actualBytes = headerDwords * 4;
    }
    return true;
}

bool
MPRController::readSasIOUnitPage0(void)
{
    UInt8 page[MPR_PAGE_BUF_BYTES];
    UInt32 actual = 0;
    UInt32 numPhys, i;
    char name[32];

    if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_IO_UNIT, 0,
                           page, sizeof(page), &actual)) {
        setProperty("MPRM2cFailure", "SAS IO Unit Page0 read failed");
        return false;
    }

    numPhys = page[0x0C];
    if (numPhys > 16) {
        numPhys = 16;
    }
    setProperty("MPRNumPhys", (unsigned long long)numPhys, 8);
    setProperty("MPRSasPageBytes", (unsigned long long)actual, 32);

    for (i = 0; i < numPhys; i++) {
        const UInt8 *phy = page + 0x10 + i * 0x14;
        UInt32 deviceInfo = (UInt32)phy[0x04] | ((UInt32)phy[0x05] << 8) |
                            ((UInt32)phy[0x06] << 16) | ((UInt32)phy[0x07] << 24);
        UInt32 devHandle = (UInt32)phy[0x08] | ((UInt32)phy[0x09] << 8);
        UInt32 linkRate = phy[0x03];

        snprintf(name, sizeof(name), "MPRPhy%uDevHandle", (unsigned int)i);
        setProperty(name, (unsigned long long)devHandle, 16);
        snprintf(name, sizeof(name), "MPRPhy%uDeviceInfo", (unsigned int)i);
        setProperty(name, (unsigned long long)deviceInfo, 32);
        snprintf(name, sizeof(name), "MPRPhy%uLinkRate", (unsigned int)i);
        setProperty(name, (unsigned long long)linkRate, 8);

        if (devHandle != 0 && !getProperty("MPRAttachedDevHandle")) {
            setProperty("MPRAttachedDevHandle", (unsigned long long)devHandle, 16);
            setProperty("MPRAttachedDeviceInfo", (unsigned long long)deviceInfo, 32);
            setProperty("MPRAttachedPhy", (unsigned long long)i, 8);
            setProperty("MPRAttachedPortFlags", (unsigned long long)phy[0x01], 8);
            setProperty("MPRAttachedPhyFlags", (unsigned long long)phy[0x02], 8);
            setProperty("MPRAttachedDiscoveryStatus",
                        (unsigned long long)((UInt32)phy[0x0C] |
                                             ((UInt32)phy[0x0D] << 8) |
                                             ((UInt32)phy[0x0E] << 16) |
                                             ((UInt32)phy[0x0F] << 24)), 32);
            fAttachedDevHandle = devHandle;
            readSasDevicePage0(devHandle);
        }
    }
    return true;
}

bool
MPRController::readSasDevicePage0(UInt32 devHandle)
{
    UInt8 page[MPR_PAGE_BUF_BYTES];
    UInt32 actual = 0, pageAddress, deviceInfo;
    UInt64 sasAddress = 0;
    UInt16 parentHandle, slot, enclHandle, pageDevHandle;
    UInt8 phyNum;
    char sasText[24];
    const char *type = "other";

    pageAddress = MPI2_SAS_DEVICE_PGAD_FORM_HANDLE |
                  (devHandle & MPI2_SAS_DEVICE_PGAD_HANDLE_MASK);
    if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE, pageAddress,
                           page, sizeof(page), &actual)) {
        setProperty("MPRSasDeviceFailure", "SAS Device Page0 read failed");
        return false;
    }

    memcpy(&slot, page + MPR_SASDEV0_SLOT_OFF, sizeof(slot));
    memcpy(&enclHandle, page + MPR_SASDEV0_ENCLOSURE_OFF, sizeof(enclHandle));
    memcpy(&sasAddress, page + MPR_SASDEV0_SASADDRESS_OFF, sizeof(sasAddress));
    memcpy(&parentHandle, page + MPR_SASDEV0_PARENT_OFF, sizeof(parentHandle));
    phyNum = page[MPR_SASDEV0_PHYNUM_OFF];
    memcpy(&pageDevHandle, page + MPR_SASDEV0_DEVHANDLE_OFF, sizeof(pageDevHandle));
    memcpy(&deviceInfo, page + MPR_SASDEV0_DEVICEINFO_OFF, sizeof(deviceInfo));

    if (deviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) {
        type = "SATA";
    } else if (deviceInfo & MPI2_SAS_DEVICE_INFO_STP_TARGET) {
        type = "STP";
    } else if (deviceInfo & MPI2_SAS_DEVICE_INFO_SSP_TARGET) {
        type = "SSP";
    }

    snprintf(sasText, sizeof(sasText), "%016llx", (unsigned long long)sasAddress);

    setProperty("MPRSasDeviceHandle", (unsigned long long)pageDevHandle, 16);
    setProperty("MPRSasDeviceSasAddress", sasText);
    setProperty("MPRSasDeviceInfo", (unsigned long long)deviceInfo, 32);
    setProperty("MPRSasDeviceType", type);
    setProperty("MPRSasDeviceParent", (unsigned long long)parentHandle, 16);
    setProperty("MPRSasDevicePhyNum", (unsigned long long)phyNum, 8);
    setProperty("MPRSasDeviceSlot", (unsigned long long)slot, 16);
    setProperty("MPRSasDeviceEnclosure", (unsigned long long)enclHandle, 16);
    setProperty("MPRSasDeviceAccessStatus", (unsigned long long)page[0x17], 8);
    setProperty("MPRSasDeviceFlags",
                (unsigned long long)((UInt32)page[0x20] | ((UInt32)page[0x21] << 8)), 16);
    setProperty("MPRSasDevicePageBytes", (unsigned long long)actual, 32);
    IOLog("MPR M2d: dev handle=%u sas=%s info=0x%08x type=%s\n",
          (unsigned int)pageDevHandle, sasText,
          (unsigned int)deviceInfo, type);
    return true;
}

bool
MPRController::runM2c(void)
{
    IOByteCount segLength;
    bool ok;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRM2cState", "failed-alloc");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_PAGE_BUF_BYTES ||
        (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRM2cState", "failed-alloc");
        return false;
    }

    ok = readSasIOUnitPage0();
    setProperty("MPRM2cState", ok ? "ok" : "failed");

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    fPagePhys = 0;
    return ok;
}

/* ------------------------------------------------------------------------ */
/* M3-a: SCSI INQUIRY through the request/reply queues, validating the SCSI  */
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

void
MPRController::postScsiIo(UInt32 smid)
{
    UInt32 low = MPI25_REQ_DESCRIPT_FLAGS_FAST_PATH_SCSI_IO | (smid << 16);

    /* The IOC has fast path enabled for this device (SAS Device Page0 Flags
     * bit 0x4000), so the target devhandle travels in descriptor word 1.
     * MPI2_SCSI_IO_REQUEST_DESCRIPTOR puts LMID at 0x04 and DevHandle at
     * 0x06, i.e. DevHandle occupies the high half of word 1. */
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, low);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET,
             (fAttachedDevHandle & 0xFFFF) << 16);
}

bool
MPRController::scsiTur(void)
{
    MPRScsiIoRequest *request;
    MPRScsiIoReply *reply;
    UInt8 *frame;
    UInt32 attempt;
    bool posted;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRTurFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 6 * fRequestFrameSize;     /* SMID 6 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)(fPagePhys + MPR_SENSE_OFFSET);
    request->SenseBufferLength = 96;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 0;
    request->IoFlags = 6 | MPI25_SCSIIO_IOFLAGS_FAST_PATH;
    request->Control = MPI2_SCSIIO_CONTROL_NODATATRANSFER;
    request->CDB[0] = 0x00;                 /* TEST UNIT READY */

    /* A zero-length IEEE SGE is still required for a data-less command. */
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 0);

    posted = false;
    for (attempt = 0; attempt < 10 && !posted; attempt++) {
        fReplyCount = 0;
        fLastReplyKind = 0;
        fLastReplySmid = 0xFFFF;
        postScsiIo(6);
        if (waitForReply(MPR_M2_REPLY_TIMEOUT_MS) && fLastReplySmid == 6) {
            posted = true;
            break;
        }
        IOSleep(1000);
    }
    setProperty("MPRTurAttempts", (unsigned long long)(attempt + 1), 32);
    if (!posted) {
        setProperty("MPRTurFailure", "TEST UNIT READY timeout");
        return false;
    }

    reply = (MPRScsiIoReply *)fLastReply;
    setProperty("MPRTurKind", (unsigned long long)fLastReplyKind, 32);
    setProperty("MPRTurSCSIState", (unsigned long long)reply->SCSIState, 8);
    if (fLastReplyKind == 1) {
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
    char vendor[9], product[17], revision[5];

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRM3aFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 5 * fRequestFrameSize;     /* SMID 5 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)(fPagePhys + MPR_SENSE_OFFSET);
    request->SenseBufferLength = 96;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 36;
    request->IoFlags = 6 | MPI25_SCSIIO_IOFLAGS_FAST_PATH;   /* six-byte CDB */
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
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    postScsiIo(5);
    posted = waitForReply(15000) && (fLastReplySmid == 5);
    setProperty("MPRInquiryAttempts", 1ULL, 32);
    if (!posted) {
        setProperty("MPRInquiryKind", (unsigned long long)fLastReplyKind, 32);
        setProperty("MPRInquirySmid", (unsigned long long)fLastReplySmid, 16);
        setProperty("MPRInquiryPostIdx", (unsigned long long)fReplyPostIndex, 32);
        setProperty("MPRInquiryRegPostIdx",
                    (unsigned long long)readReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET), 32);
        setProperty("MPRInquiryRegFreeIdx",
                    (unsigned long long)readReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET), 32);
        setProperty("MPRM3aFailure", "INQUIRY timeout");
        return false;
    }

    {
        UInt64 w[4];

        memcpy(w, fLastReply, sizeof(w));
        setProperty("MPRInquiryReplyW0", w[0], 64);
        setProperty("MPRInquiryReplyW1", w[1], 64);
        setProperty("MPRInquiryReplyW2", w[2], 64);
        setProperty("MPRInquiryReplyW3", w[3], 64);
        reply = (MPRScsiIoReply *)fLastReply;
        setProperty("MPRInquirySCSIState", (unsigned long long)reply->SCSIState, 8);
        setProperty("MPRInquiryReplyFunction", (unsigned long long)reply->Function, 8);
        setProperty("MPRInquiryReplyMsgLength", (unsigned long long)reply->MsgLength, 8);
    }

    if (fLastReplyKind == 1) {
        reply = (MPRScsiIoReply *)fLastReply;
        setProperty("MPRInquiryIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRInquiryScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        setProperty("MPRInquiryTransferCount", (unsigned long long)reply->TransferCount, 32);
        setProperty("MPRInquiryResponseInfo", (unsigned long long)reply->ResponseInfo, 32);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0) {
            setProperty("MPRM3aFailure", "INQUIRY reply status");
            return false;
        }
    } else {
        /* SCSI_IO_SUCCESS: command succeeded, no reply frame was posted. */
        setProperty("MPRInquiryIOCStatus", 0ULL, 16);
        setProperty("MPRInquiryScsiStatus", 0ULL, 8);
        setProperty("MPRInquiryTransferCount", 36ULL, 32);
        setProperty("MPRInquiryResponseInfo", 0ULL, 32);
    }
    setProperty("MPRInquiryReplyKind", (unsigned long long)fLastReplyKind, 32);

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        setProperty("MPRM3aFailure", "INQUIRY data buffer unavailable");
        return false;
    }
    mprCopyField(vendor, data + 8, 8);
    mprCopyField(product, data + 16, 16);
    mprCopyField(revision, data + 32, 4);

    /* Cache the identity for the M3-d nub's get*String() methods. */
    memcpy(fVendor, vendor, sizeof(vendor));
    memcpy(fProduct, product, sizeof(product));
    memcpy(fRevision, revision, sizeof(revision));

    setProperty("MPRInquiryDeviceType", (unsigned long long)(data[0] & 0x1F), 8);
    setProperty("MPRInquiryVendor", vendor);
    setProperty("MPRInquiryProduct", product);
    setProperty("MPRInquiryRevision", revision);
    IOLog("MPR M3a: INQUIRY type=%u vendor=%s product=%s rev=%s\n",
          (unsigned int)(data[0] & 0x1F), vendor, product, revision);
    return true;
}

bool
MPRController::scsiReadCapacity16(void)
{
    static const UInt8 cdb[16] = {
        0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20
    };
    MPRScsiIoRequest *request;
    MPRScsiIoReply *reply;
    UInt8 *frame, *data;
    UInt64 lastLba = 0;
    UInt32 blockSize, i;
    bool posted;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRM3bFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 7 * fRequestFrameSize;     /* SMID 7 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)(fPagePhys + MPR_SENSE_OFFSET);
    request->SenseBufferLength = 96;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 32;
    request->IoFlags = (UInt16)(16 | MPI25_SCSIIO_IOFLAGS_FAST_PATH);
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, sizeof(cdb));
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 32);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_PAGE_BUF_BYTES);
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    postScsiIo(7);
    posted = waitForReply(15000) && (fLastReplySmid == 7);
    if (!posted) {
        setProperty("MPRM3bFailure", "READ CAPACITY(16) timeout");
        return false;
    }
    if (fLastReplyKind == 1) {
        reply = (MPRScsiIoReply *)fLastReply;
        setProperty("MPRScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0) {
            setProperty("MPRM3bFailure", "READ CAPACITY(16) status");
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
    IOLog("MPR M3b: READ CAPACITY(16) blocks=%llu blockSize=%u\n",
          (unsigned long long)(lastLba + 1), (unsigned int)blockSize);
    if (blockSize == 0) {
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

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRM3bFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 8 * fRequestFrameSize;     /* SMID 8 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)(fPagePhys + MPR_SENSE_OFFSET);
    request->SenseBufferLength = 96;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = 8;
    request->IoFlags = (UInt16)(10 | MPI25_SCSIIO_IOFLAGS_FAST_PATH);
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, sizeof(cdb));
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fPagePhys, 8);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_PAGE_BUF_BYTES);
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    postScsiIo(8);
    posted = waitForReply(15000) && (fLastReplySmid == 8);
    if (!posted) {
        setProperty("MPRM3bFailure", "READ CAPACITY(10) timeout");
        return false;
    }
    if (fLastReplyKind == 1) {
        reply = (MPRScsiIoReply *)fLastReply;
        setProperty("MPRScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0) {
            setProperty("MPRM3bFailure", "READ CAPACITY(10) status");
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

    setProperty("MPRCapacityLastLba", (unsigned long long)lastLba, 64);
    setProperty("MPRCapacityBlockSize", (unsigned long long)blockSize, 32);
    setProperty("MPRCapacityBlocks", (unsigned long long)lastLba + 1, 64);
    IOLog("MPR M3b: READ CAPACITY(10) blocks=%llu blockSize=%u\n",
          (unsigned long long)(lastLba + 1), (unsigned int)blockSize);
    if (blockSize == 0) {
        return false;
    }
    fBlockSize = blockSize;
    fCapacityBlocks = (UInt64)lastLba + 1;
    return true;
}

/* ------------------------------------------------------------------------ */
/* M3-c: multi-block READ(10)/WRITE(10) through the same fast-path SCSI IO    */
/* path, staged in the low-4GB bulk buffer.                                  */

bool
MPRController::scsiReadWrite(bool write, UInt64 lba, UInt32 blocks,
                             UInt32 blockSize, UInt32 smid, bool diagnostics)
{
    UInt8 cdb[10];
    MPRScsiIoRequest *request;
    MPRScsiIoReply   *reply;
    UInt8 *frame;
    UInt32 dataLen = blocks * blockSize;
    bool posted;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL ||
        fPageBuffer == NULL || fDataBuffer == NULL) {
        if (diagnostics) {
            setProperty("MPRM3cFailure", "no device or staging buffer");
        }
        return false;
    }
    if (dataLen == 0 || dataLen > fDataBytes || lba > 0xFFFFFFFFULL ||
        blocks == 0 || blocks > 0xFFFF) {
        if (diagnostics) {
            setProperty("MPRM3cFailure", "transfer out of range");
        }
        return false;
    }

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? 0x2A : 0x28;           /* WRITE(10) / READ(10) */
    cdb[2] = (UInt8)(lba >> 24);
    cdb[3] = (UInt8)(lba >> 16);
    cdb[4] = (UInt8)(lba >> 8);
    cdb[5] = (UInt8)lba;
    cdb[7] = (UInt8)(blocks >> 8);
    cdb[8] = (UInt8)blocks;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)(fPagePhys + MPR_SENSE_OFFSET);
    request->SenseBufferLength = 96;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)(sizeof(cdb) | MPI25_SCSIIO_IOFLAGS_FAST_PATH);
    request->Control = write ? MPI2_SCSIIO_CONTROL_WRITE : MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, sizeof(cdb));
    mprFillIeeeSge(frame + MPR_SCSIIO_SGE_DWORDS * 4, fDataPhys, dataLen);

    /* A write caller has already filled the staging buffer; only reads need
     * it cleared. Clearing it on write would discard the payload. */
    if (!write) {
        memset(fDataBuffer->getBytesNoCopy(), 0, fDataBytes);
    }
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    postScsiIo(smid);
    posted = waitForReply(15000) && (fLastReplySmid == (UInt16)smid);
    if (!posted) {
        if (diagnostics) {
            setProperty("MPRM3cFailure", "transfer timeout");
        }
        return false;
    }
    if (fLastReplyKind == 2) {
        if (diagnostics) {
            setProperty("MPRReadIOCStatus", 0ULL, 16);
            setProperty("MPRReadScsiStatus", 0ULL, 8);
            setProperty("MPRReadTransferCount", (unsigned long long)dataLen, 32);
        }
        return true;
    }
    reply = (MPRScsiIoReply *)fLastReply;
    if (diagnostics) {
        setProperty("MPRReadIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRReadScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        setProperty("MPRReadTransferCount", (unsigned long long)reply->TransferCount, 32);
    }
    return (reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS &&
           reply->SCSIStatus == 0;
}

bool
MPRController::runM3c(void)
{
    IOByteCount segLength;
    UInt64 pageSeg = 0, dataSeg = 0;
    UInt8 *data;
    bool ok = false;
    UInt32 i;
    char name[32];

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    fDataBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_DATA_BUF_BYTES, MPR_DMA_LOW_MASK);

    if (fPageBuffer == NULL || fDataBuffer == NULL) {
        setProperty("MPRM3cState", "failed-alloc");
    } else {
        fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
        pageSeg = segLength;
        fDataPhys = fDataBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
        dataSeg = segLength;
        fDataBytes = MPR_DATA_BUF_BYTES;

        if (fPagePhys == 0 || fDataPhys == 0 ||
            pageSeg < MPR_PAGE_BUF_BYTES || dataSeg < MPR_DATA_BUF_BYTES ||
            (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1) ||
            (fDataPhys + MPR_DATA_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
            setProperty("MPRM3cState", "failed-alloc");
        } else {
            /* Read the first 8 blocks and decode the MBR/GPT protective entry. */
            ok = scsiReadWrite(false, 0, 8, 512, 9, true);
            setProperty("MPRM3cState", ok ? "ok" : "failed");

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
                IOLog("MPR M3c: LBA0 read ok, sig=0x%04x part0type=0x%02x\n",
                      (unsigned int)((UInt32)data[510] | ((UInt32)data[511] << 8)),
                      (unsigned int)data[0x1BE + 4]);
            }
        }
    }

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    if (fDataBuffer != NULL) {
        fDataBuffer->release();
        fDataBuffer = NULL;
    }
    fPagePhys = 0;
    fDataPhys = 0;
    fDataBytes = 0;
    return ok;
}

/* ------------------------------------------------------------------------ */
/* M3-d: IOBlockStorageDevice nub, IOMedia, and the block read path.        */

IOReturn
MPRController::readBlocks(IOMemoryDescriptor *buffer, UInt64 block,
                          UInt64 nblks, UInt64 *actualBytes)
{
    UInt8 *staging;
    UInt64 byteCount, done = 0, maxBlocks, remaining, want;
    UInt32 chunkBlocks;
    IOByteCount copied, chunkBytes;
    IOReturn status;

    if (actualBytes != NULL) {
        *actualBytes = 0;
    }
    if (buffer == NULL || fIOLock == NULL || fDataBuffer == NULL ||
        fPageBuffer == NULL || fCapacityBlocks == 0 || fBlockSize == 0) {
        return kIOReturnNotReady;
    }
    if (nblks == 0) {
        return kIOReturnSuccess;
    }
    if (nblks > (0xFFFFFFFFFFFFFFFFULL / fBlockSize)) {
        return kIOReturnBadArgument;
    }
    byteCount = nblks * fBlockSize;
    if (buffer->getLength() < byteCount || block >= fCapacityBlocks ||
        nblks > fCapacityBlocks - block ||
        (block + nblks - 1) > 0xFFFFFFFFULL) {
        return kIOReturnBadArgument;
    }
    staging = (UInt8 *)fDataBuffer->getBytesNoCopy();
    if (staging == NULL) {
        return kIOReturnNotReady;
    }
    maxBlocks = fDataBytes / fBlockSize;
    if (maxBlocks == 0) {
        return kIOReturnNotReady;
    }
    if (maxBlocks > 0xFFFFULL) {
        maxBlocks = 0xFFFFULL;
    }

    status = buffer->prepare();
    if (status != kIOReturnSuccess) {
        return status;
    }
    status = kIOReturnSuccess;

    /* One SMID, one reply slot and one staging buffer are shared by the whole
     * driver, so the SCSI IO path must be serialized. The completion callback
     * runs in MPRBlockDev after this method returns and the lock is released. */
    IOLockLock(fIOLock);
    while (done < byteCount) {
        remaining = byteCount - done;
        want = remaining / fBlockSize;
        if (want > maxBlocks) {
            want = maxBlocks;
        }
        chunkBlocks = (UInt32)want;
        chunkBytes = (IOByteCount)((UInt64)chunkBlocks * fBlockSize);
        if (!scsiReadWrite(false, block + done / fBlockSize, chunkBlocks,
                           fBlockSize, 9, false)) {
            status = kIOReturnDeviceError;
            break;
        }
        copied = buffer->writeBytes((IOByteCount)done, staging, chunkBytes);
        done += (UInt64)copied;
        if (copied != chunkBytes) {
            status = kIOReturnUnderrun;
            break;
        }
    }
    IOLockUnlock(fIOLock);
    buffer->complete();

    if (actualBytes != NULL) {
        *actualBytes = done;
    }
    return status;
}

bool
MPRController::allocateM3dBuffers(void)
{
    IOByteCount segLength;
    UInt64 pageSeg, dataSeg;

    /* Every earlier stage releases its staging buffers before returning. */
    if (fPageBuffer != NULL || fDataBuffer != NULL) {
        return false;
    }
    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    fDataBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_DATA_BUF_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL || fDataBuffer == NULL) {
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    pageSeg = segLength;
    fDataPhys = fDataBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    dataSeg = segLength;
    fDataBytes = MPR_DATA_BUF_BYTES;

    if (fPagePhys == 0 || fDataPhys == 0 ||
        pageSeg < MPR_PAGE_BUF_BYTES || dataSeg < MPR_DATA_BUF_BYTES ||
        (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1) ||
        (fDataPhys + MPR_DATA_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        return false;
    }
    return true;
}

bool
MPRController::runM3d(void)
{
    MPRBlockDev *nub;

    if (fAttachedDevHandle == 0 || fCapacityBlocks == 0 || fBlockSize == 0) {
        setProperty("MPRM3dState", "skipped-no-media");
        return false;
    }
    if (fIOLock == NULL || !allocateM3dBuffers()) {
        setProperty("MPRM3dState", "failed-alloc");
        return false;
    }

    nub = new MPRBlockDev;
    if (nub == NULL) {
        setProperty("MPRM3dState", "failed-nub-alloc");
        return false;
    }
    if (!nub->init(NULL)) {
        nub->release();
        setProperty("MPRM3dState", "failed-nub-init");
        return false;
    }
    nub->setController(this);
    if (!nub->attach(this)) {
        nub->release();
        setProperty("MPRM3dState", "failed-nub-attach");
        return false;
    }
    if (!nub->start(this)) {
        nub->detach(this);
        nub->release();
        setProperty("MPRM3dState", "failed-nub-start");
        return false;
    }

    fBlockDev = nub;
    nub->registerService();
    setProperty("MPRM3dState", "ok");
    setProperty("MPRM3dMediaBlocks", fCapacityBlocks, 64);
    setProperty("MPRM3dMediaBlockSize", (unsigned long long)fBlockSize, 32);
    IOLog("MPR M3d: nub registered, blocks=%llu blockSize=%u\n",
          (unsigned long long)fCapacityBlocks, (unsigned int)fBlockSize);
    return true;
}

bool
MPRController::runM3b(void)
{
    IOByteCount segLength;
    bool ok;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRM3bState", "failed-alloc");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_PAGE_BUF_BYTES ||
        (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRM3bState", "failed-alloc");
        return false;
    }

    ok = scsiReadCapacity16();
    if (ok) {
        setProperty("MPRCapacityKind", "rc16");
    } else {
        ok = scsiReadCapacity10();
        if (ok) {
            setProperty("MPRCapacityKind", "rc10");
        }
    }
    setProperty("MPRM3bState", ok ? "ok" : "failed");

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    fPagePhys = 0;
    return ok;
}

bool
MPRController::runM3a(void)
{
    IOByteCount segLength;
    bool ok;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRM3aState", "failed-alloc");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_PAGE_BUF_BYTES ||
        (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRM3aState", "failed-alloc");
        return false;
    }

    ok = scsiTur();
    setProperty("MPRM3aTur", ok ? "ok" : "failed");
    ok = scsiInquiry();
    setProperty("MPRM3aState", ok ? "ok" : "failed");

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    fPagePhys = 0;
    return ok;
}

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

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_M1C_PAGE_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRM1cFailure", "page buffer allocation failed");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_M1C_PAGE_BYTES ||
        (fPagePhys + MPR_M1C_PAGE_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRM1cFailure", "page buffer is not low-4GB contiguous");
        return false;
    }
    memset(fPageBuffer->getBytesNoCopy(), 0, MPR_M1C_PAGE_BYTES);

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        setProperty("MPRM1cFailure", "request frame unavailable");
        return false;
    }

    /* SMID 1 = frame 1. The request descriptor (RequestFlags/SMID) travels
     * through the 0xC0/0xC4 registers; the frame itself begins at the message
     * header because MPI2 request frames have no descriptor prefix
     * (mpr.c:1583-1587, mpr.c:3941). */
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
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, value32);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);

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

    /* Wait for the reply interrupt; waitForReply falls back to one poll pass
     * if the event source could not be installed. */
    fReplyCount = 0;
    if (!waitForReply(MPR_M2_REPLY_TIMEOUT_MS)) {
        setProperty("MPRM1cFailure", "no reply descriptor received");
        return false;
    }
    memcpy(replyCopy, fLastReply, sizeof(replyCopy));
    reply = (MPRConfigReply *)replyCopy;
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
        setProperty("MPRM1cFailure", "no reply descriptor received");
        return false;
    }

    setProperty("MPRConfigIOCStatus",
                (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
    setProperty("MPRConfigIOCLogInfo", (unsigned long long)reply->IOCLogInfo, 32);
    setProperty("MPRConfigPageType", (unsigned long long)reply->Header.PageType, 8);
    setProperty("MPRConfigPageNumber", (unsigned long long)reply->Header.PageNumber, 8);
    setProperty("MPRConfigPageLength", (unsigned long long)reply->Header.PageLength, 8);

    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        setProperty("MPRM1cFailure", "config request failed");
        return false;
    }
    if (reply->Header.PageType != MPI2_CONFIG_PAGETYPE_MANUFACTURING ||
        reply->Header.PageNumber != 0) {
        setProperty("MPRM1cFailure", "unexpected config page");
        return false;
    }

    page = (MPRConfigPageMan0 *)fPageBuffer->getBytesNoCopy();
    if (page == NULL) {
        setProperty("MPRM1cFailure", "page buffer has no virtual address");
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
        IOLog("MPR M1c: ManPage0 chip=%s rev=%s board=%s\n", chip, revision, board);
    }

    return true;
}

bool
MPRController::runM1c(void)
{
    bool ok;

    fReplyPostIndex = 0;
    ok = readManufacturingPage0();

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    fPagePhys = 0;
    setProperty("MPRM1cState", ok ? "ok" : "failed");
    return ok;
}

bool
MPRController::start(IOService *provider)
{
    IOPCIDevice *pci;
    IOByteCount  length;

    if (!super::start(provider)) {
        return false;
    }

    /* OSObject instances are not guaranteed zero-filled; every member used by
     * M1-a/M1-b/M1-c is initialized here so a failure cannot read garbage. */
    fMMIO = NULL;
    fRegs = NULL;
    fQueueBuffer = NULL;
    fReplyBuffer = NULL;
    fRequestBuffer = NULL;
    fPageBuffer = NULL;
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
    fCapacityBlocks = 0;
    fBlockSize = 0;
    memset(fVendor, 0, sizeof(fVendor));
    memset(fProduct, 0, sizeof(fProduct));
    memset(fRevision, 0, sizeof(fRevision));
    memset(fLastReply, 0, sizeof(fLastReply));
    memset(fLastEventData, 0, sizeof(fLastEventData));

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
    setProperty("MPRStage", "M3d");
    setProperty("MPRBuild", "0.12.0");

    /* Failures inside M1-a/M1-b/M1-c are recorded in IORegistry and never
     * panic the machine or prevent the kext from loading. M1-c reads one
     * config page through the queues and still takes no interrupts. */
    setupInterrupts();
    if (runM1a() && runM1b()) {
        runM1c();
        runM2b();
        runM2c();
        runM3a();
        runM3b();
        /* Only publish a nub after the first multi-block READ(10) proved the
         * fast-path data transfer works. */
        if (runM3c()) {
            runM3d();
        }
    } else {
        setProperty("MPRM1cState", "skipped-earlier-stage-failed");
    }

    setProperty("MPRIntCount", (unsigned long long)fInterruptCount, 32);
    setProperty("MPRReplyViaIrq", (unsigned long long)fReplyViaIrq, 32);
    setProperty("MPREventCount", (unsigned long long)fEventCount, 32);
    setProperty("MPRLastEvent", (unsigned long long)fLastEvent, 16);
    IOLog("MPR M2b: start complete, irq=%u replyViaIrq=%u events=%u last=0x%04x\n",
          (unsigned int)fInterruptCount, (unsigned int)fReplyViaIrq,
          (unsigned int)fEventCount, (unsigned int)fLastEvent);
    return true;
}

void
MPRController::stop(IOService *provider)
{
    teardownInterrupts();
    if (fBlockDev != NULL) {
        fBlockDev->setController(NULL);
        fBlockDev->terminate();
        fBlockDev->detach(this);
        fBlockDev->release();
        fBlockDev = NULL;
    }
    if (fDataBuffer != NULL) {
        fDataBuffer->release();
        fDataBuffer = NULL;
    }
    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    freeQueueMemory();
    if (fMMIO != NULL) {
        fMMIO->release();
        fMMIO = NULL;
    }
    fRegs = NULL;
    fPCIDevice = NULL;
    fCapacityBlocks = 0;
    fBlockSize = 0;
    if (fIOLock != NULL) {
        IOLockFree(fIOLock);
        fIOLock = NULL;
    }
    super::stop(provider);
}
