#ifndef MPRController_h
#define MPRController_h

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSMetaClass.h>

class IOPCIDevice;
class MPRBlockDev;
struct IOBlockStorageDeviceExtent;

class MPRController : public IOService
{
    OSDeclareDefaultStructors(MPRController)

private:
    IOPCIDevice      *fPCIDevice;
    IOMemoryMap      *fMMIO;
    volatile UInt32  *fRegs;

    /* M1-b: IOC Init control structures, all in low 4 GB physical memory. */
    IOBufferMemoryDescriptor *fQueueBuffer;
    IOBufferMemoryDescriptor *fReplyBuffer;
    IOBufferMemoryDescriptor *fRequestBuffer;
    UInt64           fFreePhys;
    UInt64           fPostPhys;
    UInt64           fReplyPhys;
    UInt64           fRequestPhys;
    UInt32           fFqDepth;
    UInt32           fPqDepth;
    UInt32           fRequestCount;
    UInt32           fReplyFreeIndex;
    UInt32           fRequestFrameSize;
    UInt32           fReplyFrameSize;
    UInt32           fRequestCredit;
    UInt32           fMaxReplyDepth;
    bool             fFactsValid;

    /* M1-c: config page data buffer and reply consumption index. */
    IOBufferMemoryDescriptor *fPageBuffer;
    UInt64           fPagePhys;
    UInt32           fReplyPostIndex;

    /* M2-a: reply interrupt delivery. */
    IOWorkLoop                *fWorkLoop;
    IOFilterInterruptEventSource *fInterrupt;
    volatile UInt32            fReplyCount;
    volatile UInt32            fInterruptCount;
    volatile UInt32            fReplyViaIrq;
    UInt8                      fLastReply[64];

    /* M2-b: firmware event notifications. */
    volatile UInt32            fEventCount;
    UInt16                     fLastEvent;
    UInt16                     fLastEventDataLength;
    UInt8                      fLastEventAckRequired;
    UInt32                     fLastEventContext;
    UInt32                     fEventSeen[4];
    UInt8                      fLastEventData[64];

    /* M3-c: bulk data buffer for block transfers. In M3-d it stays resident
     * for the lifetime of the nub instead of being freed after one probe. */
    IOBufferMemoryDescriptor  *fDataBuffer;
    UInt64                     fDataPhys;
    UInt32                     fDataBytes;

    /* M3-d: media identity, nub, and serialization of the shared SCSI IO
     * path (one outstanding SMID, one reply slot, one staging buffer). */
    MPRBlockDev               *fBlockDev;
    IOLock                    *fIOLock;
    UInt64                     fCapacityBlocks;
    UInt32                     fBlockSize;
    UInt32                     fM3dNextSmid;
    volatile UInt32            fM3dIoCount;
    volatile UInt32            fM3dRetryCount;

    /* M4: recovery, sense decoding and the write/UNMAP feature gates. */
    UInt16                     fFrozenSmid;      /* held until TMF completes */
    UInt16                     fTimedOutSmid;    /* last waitForSmid() timeout */
    bool                       fUnmapEnabled;
    bool                       fReinitInProgress;
    bool                       fDeviceOffline;   /* recovery failed: reject new IO */
    bool                       fStopping;
    UInt32                     fHighPriorityCredit;
    volatile UInt32            fEventReplyCount;
    volatile UInt32            fEventAckCount;
    volatile UInt32            fReplyFrameReturned;
    volatile UInt32            fM4CommandCount;
    volatile UInt32            fM4TmfCount;
    volatile UInt32            fM4ResetCount;
    UInt8                      fLastSenseKey;
    UInt8                      fLastASC;
    UInt8                      fLastASCQ;

    char                       fVendor[16];
    char                       fProduct[20];
    char                       fRevision[8];

    /* M3-a: SCSI IO completion tracking. */
    UInt32                     fAttachedDevHandle;
    UInt16                     fLastReplySmid;
    UInt32                     fLastReplyKind;   /* 0 none, 1 address reply, 2 scsi success */
    UInt32                     fScsiSuccess;

    UInt32 readReg(UInt32 offset);
    void   writeReg(UInt32 offset, UInt32 value);

    void   dumpRegisters(void);
    bool   waitDoorbellAck(UInt32 iterations);
    bool   waitDoorbellInt(UInt32 iterations);
    bool   diagReset(void);
    bool   transitionToReady(void);
    bool   handshake(const UInt32 *request, UInt32 requestBytes,
                     UInt16 *replyWords, UInt32 replyBytes);
    bool   getIOCFacts(void);
    bool   runM1a(void);

    bool   allocateQueueMemory(void);
    void   freeQueueMemory(void);
    bool   sendIOCInit(void);
    bool   runM1b(void);

    bool   readManufacturingPage0(void);
    bool   runM1c(void);

    bool   setupInterrupts(void);
    void   teardownInterrupts(void);
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *sender,
                                int index);
    void   processReplyQueue(void);
    bool   waitForReply(UInt32 timeoutMs);
    bool   waitForSmid(UInt32 timeoutMs, UInt32 smid);

    bool   sendEventNotification(void);
    bool   sendPortEnable(void);
    bool   runM2b(void);

    bool   readExtConfigPage(UInt8 extPageType, UInt32 pageAddress,
                             UInt8 *buffer, UInt32 bufferBytes,
                             UInt32 *actualBytes);
    bool   readSasIOUnitPage0(void);
    bool   readSasDevicePage0(UInt32 devHandle);
    bool   runM2c(void);

    void   postScsiIo(UInt32 smid);
    bool   scsiTur(void);
    bool   scsiInquiry(void);
    bool   runM3a(void);
    bool   scsiReadCapacity16(void);
    bool   scsiReadCapacity10(void);
    bool   runM3b(void);

    bool   scsiReadWrite(bool write, UInt64 lba, UInt32 blocks,
                         UInt32 blockSize, UInt32 smid, bool diagnostics);
    bool   runM3c(void);

    bool   allocateM3dBuffers(void);
    bool   runM3d(void);

    /* M4: command submission, auto sense, task management and recovery. */
    void   postDefaultRequest(UInt32 smid);
    void   postHighPriorityRequest(UInt32 smid);
    void   releaseReplyFrame(UInt32 baddr);
    void   sendEventAck(UInt16 event, UInt32 context);
    IOReturn decodeSense(IOReturn fallback);
    bool   issueScsiCommand(const UInt8 *cdb, UInt32 cdbLen, UInt32 control,
                            const UInt8 *outData, UInt32 dataLen,
                            UInt32 smid, IOReturn *status);
    IOReturn scsiSynchronizeCache(UInt64 lba, UInt32 blocks, UInt32 smid);
    IOReturn scsiUnmap(const IOBlockStorageDeviceExtent *extents,
                       UInt32 extentsCount, UInt32 smid);
    bool   sendTaskMgmt(UInt8 taskType, UInt16 taskMid, UInt32 smid);
    bool   recoverTimedOutCommand(UInt32 smid);
    bool   resetAndReinitialize(void);

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    /* MPRBlockDev relays every request through these accessors. */
    IOReturn readBlocks(IOMemoryDescriptor *buffer, UInt64 block,
                        UInt64 nblks, UInt64 *actualBytes);
    IOReturn transferBlocks(bool write, IOMemoryDescriptor *buffer,
                            UInt64 block, UInt64 nblks, UInt64 *actualBytes);
    IOReturn synchronizeCache(UInt64 block, UInt64 nblks);
    IOReturn unmapBlocks(const IOBlockStorageDeviceExtent *extents,
                         UInt32 extentsCount);
    UInt64   getCapacityBlocks(void) const { return fCapacityBlocks; }
    UInt32   getBlockSize(void) const { return fBlockSize; }
    char    *getVendorString(void) { return fVendor; }
    char    *getProductString(void) { return fProduct; }
    char    *getRevisionString(void) { return fRevision; }
};

#endif /* MPRController_h */
