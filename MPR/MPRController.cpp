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

/* topology config page staging buffer (SAS IO Unit Page0 with 8+ phys). */
#define MPR_PAGE_BUF_BYTES      512

/* sense buffer lives in the upper half of the page staging buffer. */
#define MPR_SENSE_OFFSET        0x100

/* bulk transfer staging buffer. */
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

    if (!fFactsValid || fReplyFrameSize == 0 || fRequestFrameSize == 0) {
        setProperty("MPRQueueInitFailure", "no usable IOC Facts geometry");
        return false;
    }

    fFqDepth        = mprRoundUp16(MPR_REPLY_FRAMES + 1);
    fPqDepth        = fFqDepth;
    fRequestCount   = MPR_REQUEST_FRAMES;
    fReplyFreeIndex = MPR_REPLY_FRAMES;

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

    if (fQueueBuffer == NULL || fReplyBuffer == NULL || fRequestBuffer == NULL) {
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
                fEventReplyCount++;
                if (fLastEventAckRequired == MPI2_EVENT_NOTIFICATION_ACK_REQUIRED) {
                    sendEventAck(fLastEvent, fLastEventContext);
                }
            } else {
                memcpy(fLastReply, replyFrame, sizeof(fLastReply));
                fLastReplySmid = (UInt16)((entry >> 16) & 0xFFFF);
                fLastReplyKind = 1;
                fReplyCount++;
            }
            /* D1: each AddressReply consumes exactly one reply frame; return
             * it to the free queue (mpr_free_reply, mprvar.h:559-565). */
            releaseReplyFrame(baddr);
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

bool
MPRController::waitForSmid(UInt32 timeoutMs, UInt32 smid)
{
    UInt32 i;

    fTimedOutSmid = 0;
    /* A reply left over from an earlier command must not be mistaken for the
     * completion of this one. Each block device command uses a rotating SMID, so the
     * SMID in the reply descriptor identifies the command unambiguously. */
    for (i = 0; i < timeoutMs; i++) {
        if (fStopping) {
            fTimedOutSmid = (UInt16)smid;
            return false;
        }
        if (fReplyCount > 0 && fLastReplySmid == (UInt16)smid) {
            return true;
        }
        IOSleep(1);
    }
    processReplyQueue();
    if (fReplyCount > 0 && fLastReplySmid == (UInt16)smid) {
        return true;
    }
    /* A real timeout needs task management; a SCSI status error needs
     * sense decoding instead. */
    fTimedOutSmid = (UInt16)smid;
    return false;
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
    UInt32 value32, before, i;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += 2 * fRequestFrameSize;     /* SMID 2 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPREventNotifyRequest *)frame;

    request->Function = MPI2_FUNCTION_EVENT_NOTIFICATION;
    /* A set bit DISABLES an event (mpr.c:2769-2791): mask everything first,
     * then clear the seven events this driver subscribes to. */
    memset(request->EventMasks, 0xFF, sizeof(request->EventMasks));
    mprClearEventBit(request->EventMasks, MPI2_EVENT_STATE_CHANGE);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DISCOVERY);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_BROADCAST_PRIMITIVE);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_LOG_ENTRY_ADDED);
    mprClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DEVICE_DISCOVERY_ERROR);

    setProperty("MPREventMask0", (unsigned long long)request->EventMasks[0], 32);
    setProperty("MPREventMask1", (unsigned long long)request->EventMasks[1], 32);
    setProperty("MPREventMask2", (unsigned long long)request->EventMasks[2], 32);
    setProperty("MPREventMask3", (unsigned long long)request->EventMasks[3], 32);
    setProperty("MPREventMaskCheck",
                (request->EventMasks[0] == 0xEF3F7FFBU &&
                 request->EventMasks[1] == 0xFFDFFFFDU &&
                 request->EventMasks[2] == 0xFFFFFFFFU &&
                 request->EventMasks[3] == 0xFFFFFFFFU) ? "ok" : "mismatch");

    /* The subscription reply is itself an EVENT_NOTIFICATION reply; wait for
     * it and check IOCStatus instead of assuming the write succeeded. */
    before = fEventReplyCount;
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    value32 = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE | (2U << 16);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, value32);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);
    for (i = 0; i < MPR_REPLY_TIMEOUT_MS && fEventReplyCount == before; i++) {
        IOSleep(1);
    }
    if (fEventReplyCount == before) {
        setProperty("MPREventNotifyState", "no-subscription-reply");
        return false;
    }
    {
        MPREventNotifyReply *reply = (MPREventNotifyReply *)fLastEventData;

        setProperty("MPREventNotifyIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPREventNotifyState",
                    ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) ==
                     MPI2_IOCSTATUS_SUCCESS) ? "ok" : "failed-status");
    }
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
    if (!waitForReply(MPR_REPLY_TIMEOUT_MS)) {
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
    if (!waitForReply(MPR_REPLY_TIMEOUT_MS)) {
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
    if (!waitForReply(MPR_REPLY_TIMEOUT_MS)) {
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
        setProperty("MPRTopologyFailure", "SAS IO Unit Page0 read failed");
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
    IOLog("MPR dev handle=%u sas=%s info=0x%08x type=%s\n",
          (unsigned int)pageDevHandle, sasText,
          (unsigned int)deviceInfo, type);
    return true;
}

bool
MPRController::discoverTopology(void)
{
    IOByteCount segLength;
    bool ok;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRTopologyState", "failed-alloc");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_PAGE_BUF_BYTES ||
        (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRTopologyState", "failed-alloc");
        return false;
    }

    ok = readSasIOUnitPage0();
    setProperty("MPRTopologyState", ok ? "ok" : "failed");

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    fPagePhys = 0;
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
        if (waitForReply(MPR_REPLY_TIMEOUT_MS) && fLastReplySmid == 6) {
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
        setProperty("MPRScsiProbeFailure", "no attached device or buffer");
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
        setProperty("MPRScsiProbeFailure", "INQUIRY timeout");
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
    setProperty("MPRInquiryReplyKind", (unsigned long long)fLastReplyKind, 32);

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        setProperty("MPRScsiProbeFailure", "INQUIRY data buffer unavailable");
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

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        setProperty("MPRCapacityProbeFailure", "no attached device or buffer");
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
        setProperty("MPRCapacityProbeFailure", "READ CAPACITY(16) timeout");
        return false;
    }
    if (fLastReplyKind == 1) {
        reply = (MPRScsiIoReply *)fLastReply;
        setProperty("MPRScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0) {
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
        setProperty("MPRCapacityProbeFailure", "no attached device or buffer");
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
        setProperty("MPRCapacityProbeFailure", "READ CAPACITY(10) timeout");
        return false;
    }
    if (fLastReplyKind == 1) {
        reply = (MPRScsiIoReply *)fLastReply;
        setProperty("MPRScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0) {
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
                             UInt32 blockSize, UInt32 smid, bool diagnostics)
{
    UInt8 cdb[10];
    MPRScsiIoRequest *request;
    MPRScsiIoReply   *reply;
    UInt8 *frame;
    UInt32 dataLen = blocks * blockSize;
    bool posted;

    if (fDeviceOffline || fStopping ||
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
    if (dataLen == 0 || dataLen > fDataBytes || lba > 0xFFFFFFFFULL ||
        blocks == 0 || blocks > 0xFFFF) {
        if (diagnostics) {
            setProperty("MPRReadProbeFailure", "transfer out of range");
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
    posted = waitForSmid(15000, smid);
    if (!posted) {
        if (diagnostics) {
            setProperty("MPRReadProbeFailure", "transfer timeout");
        } else {
            setProperty("MPRLastIOFailure", "reply-timeout");
            setProperty("MPRLastIOSmid", (unsigned long long)smid, 16);
            setProperty("MPRLastIOReplySmid",
                        (unsigned long long)fLastReplySmid, 16);
            setProperty("MPRLastIOReplyCount",
                        (unsigned long long)fReplyCount, 32);
        }
        /* A genuine timeout means the firmware may still own the request
         * frame. Freeze that SMID and abort/reset the target before any new
         * command reuses it (PLAN section 5 rule 5, risk R6). */
        if (fTimedOutSmid == (UInt16)smid) {
            recoverTimedOutCommand(smid);
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
    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS &&
        reply->SCSIStatus == 0) {
        return true;
    }
    if (!diagnostics) {
        UInt8 *sense;
        UInt64 words[4];
        IOReturn mapped;

        setProperty("MPRLastIOFailure", "scsi-status");
        setProperty("MPRLastIOSmid", (unsigned long long)smid, 16);
        setProperty("MPRLastIOReplyKind", (unsigned long long)fLastReplyKind, 8);
        setProperty("MPRLastIOIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRLastIOSCSIStatus",
                    (unsigned long long)reply->SCSIStatus, 8);
        setProperty("MPRLastIOTransferCount",
                    (unsigned long long)reply->TransferCount, 32);
        setProperty("MPRLastIOSCSIState",
                    (unsigned long long)reply->SCSIState, 8);
        setProperty("MPRLastIOSenseCount",
                    (unsigned long long)reply->SenseCount, 32);
        setProperty("MPRLastIOResponseInfo",
                    (unsigned long long)reply->ResponseInfo, 32);
        if (fPageBuffer != NULL) {
            sense = (UInt8 *)fPageBuffer->getBytesNoCopy();
            if (sense != NULL) {
                memcpy(words, sense + MPR_SENSE_OFFSET, sizeof(words));
                setProperty("MPRLastSenseW0", words[0], 64);
                setProperty("MPRLastSenseW1", words[1], 64);
                setProperty("MPRLastSenseW2", words[2], 64);
                setProperty("MPRLastSenseW3", words[3], 64);
            }
        }
        /* Auto sense: the IOC already copied the sense data into our buffer;
         * translate it into a meaningful IOReturn for the storage stack. */
        mapped = decodeSense(kIOReturnIOError);
        setProperty("MPRSenseStatus",
                    (unsigned long long)(UInt32)mapped, 32);
    }
    return false;
}

bool
MPRController::probeBlockRead(void)
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
        setProperty("MPRReadProbeState", "failed-alloc");
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
            setProperty("MPRReadProbeState", "failed-alloc");
        } else {
            /* Read the first 8 blocks and decode the MBR/GPT protective entry. */
            ok = scsiReadWrite(false, 0, 8, 512, 9, true);
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
/* IOBlockStorageDevice nub, IOMedia, and the block read path.        */

IOReturn
MPRController::transferBlocks(bool write, IOMemoryDescriptor *buffer,
                              UInt64 block, UInt64 nblks, UInt64 *actualBytes)
{
    UInt8 *staging;
    UInt64 byteCount, done = 0, maxBlocks, remaining, want;
    UInt32 chunkBlocks;
    IOByteCount copied, chunkBytes;
    IOReturn status;

    if (actualBytes != NULL) {
        *actualBytes = 0;
    }
    if (fDeviceOffline || fStopping) {
        return kIOReturnDeviceError;
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
        setProperty("MPRBlockIOFailure", "prepare");
        setProperty("MPRBlockIOFailureStatus",
                    (unsigned long long)(UInt32)status, 32);
        return status;
    }
    status = kIOReturnSuccess;

    /* One reply slot and one staging buffer are shared by the whole driver,
     * so the SCSI IO path must be serialized. The completion callback runs in
     * MPRBlockDev after this method returns and the lock is released. */
    IOLockLock(fIOLock);
    while (done < byteCount) {
        UInt32 smid, attempt;
        UInt64 lba;
        bool ok = false;
        /* A write whose result is unknown must not be replayed blindly. */
        UInt32 maxAttempts = write ? 1 : 3;

        remaining = byteCount - done;
        want = remaining / fBlockSize;
        if (want > maxBlocks) {
            want = maxBlocks;
        }
        chunkBlocks = (UInt32)want;
        chunkBytes = (IOByteCount)((UInt64)chunkBlocks * fBlockSize);
        lba = block + done / fBlockSize;

        /* Rotating SMIDs keep a late reply from an earlier command from
         * satisfying waitForSmid() for this one. SMIDs 5..9 belong to the
         * startup probes; 10..25 are the runtime IO window. */
        smid = 10 + (fNextIoSmid & 15);
        fNextIoSmid++;
        fIoCount++;

        /* A write must copy the client's data into the low-4GB staging buffer
         * before the request frame is posted. scsiReadWrite() no longer
         * clears the staging buffer on writes. */
        if (write) {
            copied = buffer->readBytes((IOByteCount)done, staging, chunkBytes);
            if (copied != chunkBytes) {
                setProperty("MPRCommandFailure", "stage-write");
                setProperty("MPRCommandDoneBytes", done, 64);
                status = kIOReturnUnderrun;
                break;
            }
        }

        for (attempt = 0; attempt < maxAttempts && !ok; attempt++) {
            ok = scsiReadWrite(write, lba, chunkBlocks, fBlockSize,
                               smid, false);
            if (!ok) {
                fIoRetryCount++;
                IOSleep(10);
            }
        }
        if (!ok) {
            setProperty("MPRCommandFailure", "scsi-io");
            setProperty("MPRBlockIOFailure", "scsi-io");
            setProperty("MPRBlockIOFailureLba", lba, 64);
            setProperty("MPRBlockIOFailureSmid", (unsigned long long)smid, 16);
            setProperty("MPRBlockIOCount",
                        (unsigned long long)fIoCount, 32);
            setProperty("MPRBlockIORetryCount",
                        (unsigned long long)fIoRetryCount, 32);
            setProperty("MPRBlockIODoneBytes", done, 64);
            status = kIOReturnDeviceError;
            break;
        }
        if (!write) {
            copied = buffer->writeBytes((IOByteCount)done, staging, chunkBytes);
            done += (UInt64)copied;
            if (copied != chunkBytes) {
                setProperty("MPRBlockIOFailure", "copy");
                setProperty("MPRBlockIOFailureCopied", (unsigned long long)copied, 64);
                setProperty("MPRBlockIODoneBytes", done, 64);
                status = kIOReturnUnderrun;
                break;
            }
        } else {
            done += (UInt64)chunkBytes;
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
MPRController::allocateResidentBuffers(void)
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
MPRController::publishBlockDevice(void)
{
    MPRBlockDev *nub;

    if (fAttachedDevHandle == 0 || fCapacityBlocks == 0 || fBlockSize == 0) {
        setProperty("MPRBlockDeviceState", "skipped-no-media");
        return false;
    }
    if (fIOLock == NULL || !allocateResidentBuffers()) {
        setProperty("MPRBlockDeviceState", "failed-alloc");
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
    nub->setController(this);
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
MPRController::probeCapacity(void)
{
    IOByteCount segLength;
    bool ok;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRCapacityProbeState", "failed-alloc");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_PAGE_BUF_BYTES ||
        (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRCapacityProbeState", "failed-alloc");
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
    setProperty("MPRCapacityProbeState", ok ? "ok" : "failed");

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    fPagePhys = 0;
    return ok;
}

bool
MPRController::probeScsiIdentity(void)
{
    IOByteCount segLength;
    bool ok;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        MPR_PAGE_BUF_BYTES, MPR_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        setProperty("MPRScsiProbeState", "failed-alloc");
        return false;
    }
    fPagePhys = fPageBuffer->getPhysicalSegment(0, &segLength, kIOMemoryMapperNone);
    if (fPagePhys == 0 || segLength < MPR_PAGE_BUF_BYTES ||
        (fPagePhys + MPR_PAGE_BUF_BYTES) > (MPR_DMA_LOW_MASK + 1)) {
        setProperty("MPRScsiProbeState", "failed-alloc");
        return false;
    }

    ok = scsiTur();
    setProperty("MPRTurStatus", ok ? "ok" : "failed");
    ok = scsiInquiry();
    setProperty("MPRScsiProbeState", ok ? "ok" : "failed");

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
    if (!waitForReply(MPR_REPLY_TIMEOUT_MS)) {
        setProperty("MPRConfigPageFailure", "no reply descriptor received");
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

    if (fPageBuffer != NULL) {
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    fPagePhys = 0;
    setProperty("MPRConfigPageState", ok ? "ok" : "failed");
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

    /* OSObject instances are not guaranteed zero-filled; every member used
     * during bring-up is initialized here so a failure cannot read garbage. */
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
    fNextIoSmid = 0;
    fIoCount = 0;
    fIoRetryCount = 0;
    fFrozenSmid = 0;
    fTimedOutSmid = 0;
    fUnmapEnabled = true;
    fReinitInProgress = false;
    fCommandCount = 0;
    fTmfCount = 0;
    fResetCount = 0;
    fLastSenseKey = 0xFF;
    fLastASC = 0;
    fLastASCQ = 0;
    fDeviceOffline = false;
    fStopping = false;
    fHighPriorityCredit = 0;
    fEventReplyCount = 0;
    fEventAckCount = 0;
    fReplyFrameReturned = 0;
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
    setProperty("MPRStage", "read-write");
    setProperty("MPRBuild", "0.14.0");
    setProperty("MPRFeatures",
                "write10,synchronize-cache10,unmap,auto-sense,tmf,diag-reset");

    /* Failures during bring-up are recorded in IORegistry and never panic
     * the machine or prevent the kext from loading. Config-page reads
     * poll the reply queue and take no interrupts. */
    setupInterrupts();
    if (initializeController() && initializeQueues()) {
        readManufacturingInfo();
        enableEventsAndPort();
        discoverTopology();
        probeScsiIdentity();
        probeCapacity();
        /* Only publish a nub after the first multi-block READ(10) proved the
         * fast-path data transfer works. */
        if (probeBlockRead()) {
            publishBlockDevice();
        }
    } else {
        setProperty("MPRConfigPageState", "skipped-earlier-stage-failed");
    }

    setProperty("MPRIntCount", (unsigned long long)fInterruptCount, 32);
    setProperty("MPRReplyViaIrq", (unsigned long long)fReplyViaIrq, 32);
    setProperty("MPREventCount", (unsigned long long)fEventCount, 32);
    setProperty("MPRLastEvent", (unsigned long long)fLastEvent, 16);
    IOLog("MPR start complete, irq=%u replyViaIrq=%u events=%u last=0x%04x\n",
          (unsigned int)fInterruptCount, (unsigned int)fReplyViaIrq,
          (unsigned int)fEventCount, (unsigned int)fLastEvent);
    return true;
}

void
MPRController::stop(IOService *provider)
{
    fStopping = true;
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

/* ------------------------------------------------------------------------ */
/* write, SYNCHRONIZE CACHE, UNMAP, auto sense, TMF and diag reset.      */
/* SMID map: 1..4 config/events, 5..9 startup probes, 10..25 runtime IO,    */
/* 26..28 task management, 29 sync cache, 30 UNMAP.                         */

#define MPR_SM_TMF_BASE         26
#define MPR_SM_SYNC_CACHE       29
#define MPR_SM_UNMAP            30
#define MPR_SM_EVENT_ACK        31
#define MPR_MAX_UNMAP_EXTENTS   32

IOReturn
MPRController::readBlocks(IOMemoryDescriptor *buffer, UInt64 block,
                          UInt64 nblks, UInt64 *actualBytes)
{
    return transferBlocks(false, buffer, block, nblks, actualBytes);
}

void
MPRController::postDefaultRequest(UInt32 smid)
{
    /* Config and EventAck messages use the default descriptor type;
     * only SCSI IO uses the fast-path descriptor (see postScsiIo). */
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET,
             MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE | (smid << 16));
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);
}

void
MPRController::postHighPriorityRequest(UInt32 smid)
{
    /* FreeBSD allocates TM requests from the high-priority pool
     * (mpr_sas.c:224-236); the descriptor type is 0x06, not 0x08. */
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET,
             MPI2_REQ_DESCRIPT_FLAGS_HIGH_PRIORITY | (smid << 16));
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, 0);
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
    UInt8 *frame;
    MPREventAckRequest *request;

    if (fRequestBuffer == NULL) {
        return;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return;
    }
    frame += MPR_SM_EVENT_ACK * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPREventAckRequest *)frame;
    request->Function = MPI2_FUNCTION_EVENT_ACK;
    request->Event = event;
    request->EventContext = context;
    fEventAckCount++;
    postDefaultRequest(MPR_SM_EVENT_ACK);
}

IOReturn
MPRController::decodeSense(IOReturn fallback)
{
    UInt8 *sense;
    UInt8 response, key = 0xFF, asc = 0, ascq = 0;

    if (fPageBuffer == NULL) {
        return fallback;
    }
    sense = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (sense == NULL) {
        return fallback;
    }
    sense += MPR_SENSE_OFFSET;
    response = sense[0] & 0x7F;

    if (response == 0x70 || response == 0x71) {         /* fixed format */
        key = sense[2] & 0x0F;
        if (sense[7] >= 6) {
            asc = sense[12];
            ascq = sense[13];
        }
    } else if (response == 0x72 || response == 0x73) {  /* descriptor format */
        key = sense[1] & 0x0F;
        asc = sense[2];
        ascq = sense[3];
    } else {
        return fallback;                                 /* no usable sense */
    }

    fLastSenseKey = key;
    fLastASC = asc;
    fLastASCQ = ascq;
    setProperty("MPRSenseKey", (unsigned long long)key, 8);
    setProperty("MPRSenseASC", (unsigned long long)asc, 8);
    setProperty("MPRSenseASCQ", (unsigned long long)ascq, 8);

    switch (key) {
        case MPR_SENSE_KEY_NO_SENSE:
        case MPR_SENSE_KEY_RECOVERED_ERROR:
            return kIOReturnSuccess;
        case MPR_SENSE_KEY_NOT_READY:
        case MPR_SENSE_KEY_UNIT_ATTENTION:
            return kIOReturnNotReady;
        case MPR_SENSE_KEY_MEDIUM_ERROR:
        case MPR_SENSE_KEY_BLANK_CHECK:
            return kIOReturnBadMedia;
        case MPR_SENSE_KEY_HARDWARE_ERROR:
            return kIOReturnDeviceError;
        case MPR_SENSE_KEY_ILLEGAL_REQUEST:
            return kIOReturnUnsupported;
        case MPR_SENSE_KEY_DATA_PROTECT:
            return kIOReturnNotWritable;
        case MPR_SENSE_KEY_ABORTED_COMMAND:
            return kIOReturnAborted;
        default:
            return fallback;
    }
}

bool
MPRController::issueScsiCommand(const UInt8 *cdb, UInt32 cdbLen,
                                UInt32 control, const UInt8 *outData,
                                UInt32 dataLen, UInt32 smid,
                                IOReturn *status)
{
    UInt8 *frame, *staging;
    MPRScsiIoRequest *request;
    MPRScsiIoReply *reply;
    IOReturn mapped;
    bool posted;

    if (status != NULL) {
        *status = kIOReturnNotReady;
    }
    if (fDeviceOffline || fStopping) {
        if (status != NULL) {
            *status = kIOReturnDeviceError;
        }
        return false;
    }
    if (fRequestBuffer == NULL || fPageBuffer == NULL || fDataBuffer == NULL ||
        cdb == NULL || cdbLen == 0 || cdbLen > 32 || smid == 0 ||
        smid >= fRequestCount || dataLen > fDataBytes ||
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
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)(fPagePhys + MPR_SENSE_OFFSET);
    request->SenseBufferLength = 96;
    request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)(cdbLen | MPI25_SCSIIO_IOFLAGS_FAST_PATH);
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
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    postScsiIo(smid);
    posted = waitForSmid(15000, smid);
    if (!posted) {
        if (status != NULL) {
            *status = kIOReturnTimeout;
        }
        if (fTimedOutSmid == (UInt16)smid) {
            recoverTimedOutCommand(smid);
        }
        return false;
    }
    if (fLastReplyKind == 2) {
        if (status != NULL) {
            *status = kIOReturnSuccess;
        }
        return true;
    }

    reply = (MPRScsiIoReply *)fLastReply;
    mapped = kIOReturnIOError;
    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        mapped = kIOReturnDeviceError;
    } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_CHECK_CONDITION) {
        mapped = ((reply->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_VALID) != 0)
                     ? decodeSense(kIOReturnIOError) : kIOReturnDeviceError;
    } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_BUSY ||
               reply->SCSIStatus == MPI2_SCSI_STATUS_TASK_SET_FULL ||
               reply->SCSIStatus == MPI2_SCSI_STATUS_RESERVATION_CONFLICT) {
        mapped = kIOReturnBusy;
    } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_TASK_ABORTED) {
        mapped = kIOReturnAborted;
    } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_GOOD) {
        mapped = kIOReturnSuccess;
    }

    setProperty("MPRLastCommandStatus",
                (unsigned long long)(UInt32)mapped, 32);
    setProperty("MPRLastSCSIStatus",
                (unsigned long long)reply->SCSIStatus, 8);
    setProperty("MPRLastIOCStatus",
                (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
    if (status != NULL) {
        *status = mapped;
    }
    return mapped == kIOReturnSuccess;
}

IOReturn
MPRController::scsiSynchronizeCache(UInt64 lba, UInt32 blocks, UInt32 smid)
{
    UInt8 cdb[10];
    IOReturn status = kIOReturnNotReady;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35;                             /* SYNCHRONIZE CACHE(10) */
    cdb[2] = (UInt8)(lba >> 24);
    cdb[3] = (UInt8)(lba >> 16);
    cdb[4] = (UInt8)(lba >> 8);
    cdb[5] = (UInt8)lba;
    cdb[7] = (UInt8)(blocks >> 8);
    cdb[8] = (UInt8)blocks;

    if (!issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_NODATATRANSFER,
                          NULL, 0, smid, &status)) {
        return status;
    }
    return kIOReturnSuccess;
}

IOReturn
MPRController::scsiUnmap(const IOBlockStorageDeviceExtent *extents,
                         UInt32 extentsCount, UInt32 smid)
{
    UInt8 cdb[10];
    UInt8 params[8 + 16 * MPR_MAX_UNMAP_EXTENTS];
    UInt32 dataLen = 8 + 16 * extentsCount;
    UInt32 i;
    IOReturn status = kIOReturnNotReady;

    if (extents == NULL || extentsCount == 0 ||
        extentsCount > MPR_MAX_UNMAP_EXTENTS) {
        return kIOReturnBadArgument;
    }
    memset(cdb, 0, sizeof(cdb));
    memset(params, 0, sizeof(params));
    cdb[0] = 0x42;                             /* UNMAP */
    cdb[7] = (UInt8)(dataLen >> 8);            /* parameter list length */
    cdb[8] = (UInt8)dataLen;

    params[0] = (UInt8)((dataLen - 2) >> 8);   /* UNMAP data length */
    params[1] = (UInt8)(dataLen - 2);
    params[2] = (UInt8)((dataLen - 8) >> 8);   /* descriptor data length */
    params[3] = (UInt8)(dataLen - 8);

    for (i = 0; i < extentsCount; i++) {
        UInt8 *d = params + 8 + 16 * i;
        UInt64 start = extents[i].blockStart;
        UInt64 count = extents[i].blockCount;

        d[0] = (UInt8)(start >> 56);
        d[1] = (UInt8)(start >> 48);
        d[2] = (UInt8)(start >> 40);
        d[3] = (UInt8)(start >> 32);
        d[4] = (UInt8)(start >> 24);
        d[5] = (UInt8)(start >> 16);
        d[6] = (UInt8)(start >> 8);
        d[7] = (UInt8)start;
        d[8] = (UInt8)(count >> 24);
        d[9] = (UInt8)(count >> 16);
        d[10] = (UInt8)(count >> 8);
        d[11] = (UInt8)count;
    }

    if (!issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_WRITE,
                          params, dataLen, smid, &status)) {
        /* R11: a failed UNMAP can drop some SATA SSD firmware. Do not retry
         * it in this session; the read path stays available. */
        fUnmapEnabled = false;
        setProperty("MPRUnmapDisabled", "first-unmap-failure");
        return status;
    }
    return kIOReturnSuccess;
}

bool
MPRController::sendTaskMgmt(UInt8 taskType, UInt16 taskMid, UInt32 smid)
{
    UInt8 *frame;
    MPRScsiTaskMgmtRequest *request;
    MPRScsiTaskMgmtReply *reply;
    bool ok = false;

    if (fRequestBuffer == NULL || fAttachedDevHandle == 0 ||
        smid == 0 || smid >= fRequestCount) {
        return false;
    }
    if (fHighPriorityCredit == 0) {
        setProperty("MPRTmfState", "no-high-priority-credit");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPRScsiTaskMgmtRequest *)frame;
    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_TASK_MGMT;
    request->TaskType = taskType;
    request->TaskMID = taskMid;

    fTmfCount++;
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    postHighPriorityRequest(smid);
    if (waitForSmid(15000, smid) && fLastReplyKind == 1) {
        reply = (MPRScsiTaskMgmtReply *)fLastReply;
        ok = ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS) &&
             (reply->ResponseCode == MPI2_SCSITASKMGMT_RSP_TM_COMPLETE ||
              reply->ResponseCode == MPI2_SCSITASKMGMT_RSP_TM_SUCCEEDED);
        setProperty("MPRTmfResponseCode",
                    (unsigned long long)reply->ResponseCode, 8);
        setProperty("MPRTmfIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        setProperty("MPRTmfTerminationCount",
                    (unsigned long long)reply->TerminationCount, 32);
    }
    setProperty("MPRTmfTaskType", (unsigned long long)taskType, 8);
    setProperty("MPRTmfState", ok ? "ok" : "failed");
    return ok;
}

bool
MPRController::recoverTimedOutCommand(UInt32 smid)
{
    static const UInt8 levels[3] = {
        MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK,
        MPI2_SCSITASKMGMT_TASKTYPE_LUN_RESET,
        MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET
    };
    UInt32 i;
    bool recovered = false;

    if (smid == 0) {
        return false;
    }
    /* Freeze the SMID until the firmware confirms the task is gone; it must
     * not write a late reply into a reused request frame (risk R6). */
    fFrozenSmid = (UInt16)smid;
    setProperty("MPRFrozenSmid", (unsigned long long)smid, 16);

    for (i = 0; i < 3 && !recovered; i++) {
        if (sendTaskMgmt(levels[i], (UInt16)smid, MPR_SM_TMF_BASE + i)) {
            recovered = true;
        } else {
            IOSleep(50);
        }
    }
    if (!recovered) {
        recovered = resetAndReinitialize();
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

bool
MPRController::resetAndReinitialize(void)
{
    bool ok = false;

    if (fReinitInProgress || fStopping) {
        return false;
    }
    fReinitInProgress = true;
    fResetCount++;

    /* Quiesce first: stop the interrupt source, then reset the IOC so it no
     * longer touches host DMA. Only afterwards release and rebuild pools. */
    teardownInterrupts();
    if (!diagReset()) {
        fReinitInProgress = false;
        setProperty("MPRReinitState", "diag-reset-failed");
        return false;
    }
    freeQueueMemory();
    fReplyPostIndex = 0;
    fReplyCount = 0;
    fLastReplySmid = 0xFFFF;
    fLastReplyKind = 0;

    setupInterrupts();
    if (initializeController() && initializeQueues()) {
        sendEventNotification();
        sendPortEnable();
        /* The firmware rebuilt its device table. Clear the cached handle
         * property so the topology read accepts the new devhandle. */
        removeProperty("MPRAttachedDevHandle");
        fAttachedDevHandle = 0;
        if (readSasIOUnitPage0()) {
            ok = (fAttachedDevHandle != 0);
        }
    }

    fReinitInProgress = false;
    setProperty("MPRReinitState", ok ? "ok" : "failed");
    setProperty("MPRResetCount", (unsigned long long)fResetCount, 32);
    return ok;
}

IOReturn
MPRController::synchronizeCache(UInt64 block, UInt64 nblks)
{
    UInt32 blocks;
    IOReturn status;

    if (fDeviceOffline || fStopping) {
        return kIOReturnDeviceError;
    }
    if (fIOLock == NULL || fDataBuffer == NULL || fPageBuffer == NULL ||
        fCapacityBlocks == 0 || fBlockSize == 0) {
        return kIOReturnNotReady;
    }
    if (block >= fCapacityBlocks) {
        return kIOReturnBadArgument;
    }
    if (nblks == 0 || nblks > fCapacityBlocks - block) {
        nblks = fCapacityBlocks - block;
    }
    /* NUMBER OF LOGICAL BLOCKS is 16-bit; zero means "to end of media". */
    blocks = (nblks > 0xFFFFULL) ? 0 : (UInt32)nblks;

    IOLockLock(fIOLock);
    status = scsiSynchronizeCache(block, blocks, MPR_SM_SYNC_CACHE);
    IOLockUnlock(fIOLock);
    return status;
}

IOReturn
MPRController::unmapBlocks(const IOBlockStorageDeviceExtent *extents,
                           UInt32 extentsCount)
{
    UInt32 i;
    IOReturn status;

    if (fDeviceOffline || fStopping) {
        return kIOReturnDeviceError;
    }
    if (!fUnmapEnabled) {
        return kIOReturnUnsupported;
    }
    if (extents == NULL || extentsCount == 0 ||
        extentsCount > MPR_MAX_UNMAP_EXTENTS || fIOLock == NULL ||
        fCapacityBlocks == 0 || fBlockSize == 0) {
        return kIOReturnBadArgument;
    }
    for (i = 0; i < extentsCount; i++) {
        UInt64 start = extents[i].blockStart;
        UInt64 count = extents[i].blockCount;

        if (count == 0 || start >= fCapacityBlocks ||
            count > fCapacityBlocks - start) {
            return kIOReturnBadArgument;
        }
    }
    IOLockLock(fIOLock);
    status = scsiUnmap(extents, extentsCount, MPR_SM_UNMAP);
    IOLockUnlock(fIOLock);
    return status;
}
