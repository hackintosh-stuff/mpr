#ifndef MPRController_h
#define MPRController_h

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSMetaClass.h>

class IOPCIDevice;
class MPRBlockDev;
class IOTimerEventSource;
struct IOBlockStorageDeviceExtent;

/* One completion slot per SMID. processReplyQueue() fills it under
 * fReplyLock; each waiter consumes only its own slot, so a late reply can no
 * longer be mistaken for the completion of another command. */
struct MPRReplySlot
{
    volatile UInt32 valid;
    UInt16          smid;
    UInt8           kind;       /* 1 = address reply, 2 = fast-path success */
    UInt8           reserved;
    UInt8           reply[64];
};

enum { kMPREventAckQueueDepth = 64 };
struct MPREventAckEntry
{
    UInt16 event;
    UInt32 context;
};

class MPRController : public IOService
{
    OSDeclareDefaultStructors(MPRController)

private:
    IOPCIDevice      *fPCIDevice;
    IOMemoryMap      *fMMIO;
    volatile UInt32  *fRegs;

    /* IOC Init control structures, all in low 4 GB physical memory. */
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

    /* config page data buffer and reply consumption index. */
    IOBufferMemoryDescriptor *fPageBuffer;
    UInt64           fPagePhys;
    UInt32           fReplyPostIndex;

    /* reply interrupt delivery. */
    IOWorkLoop                *fWorkLoop;
    IOFilterInterruptEventSource *fInterrupt;
    volatile UInt32            fReplyCount;
    volatile UInt32            fInterruptCount;
    volatile UInt32            fReplyViaIrq;
    UInt8                      fLastReply[64];
    MPRReplySlot               fReplySlots[128];
    IOSimpleLock              *fReplyLock;
    /* Lock order is reply -> post. Never wait or take reply under post. */
    IOSimpleLock              *fPostLock;
    bool                       fManagementBusy[10];
    bool                       fTmfBusy[3];

    /* firmware event notifications. */
    volatile UInt32            fEventCount;
    UInt16                     fLastEvent;
    UInt16                     fLastEventDataLength;
    UInt8                      fLastEventAckRequired;
    UInt32                     fLastEventContext;
    UInt32                     fEventSeen[4];
    UInt8                      fLastEventData[64];
    MPREventAckEntry            fEventAckQueue[kMPREventAckQueueDepth];
    UInt32                     fEventAckHead;
    UInt32                     fEventAckQueued;
    bool                       fEventAckBusy;
    UInt32                     fEventAckAge;
    UInt32                     fEventAckCompleted;
    UInt32                     fEventAckFailed;
    UInt32                     fEventAckOverflow;
    volatile bool              fProtocolRecoveryPending;
    UInt32                     fProtocolFaultCode;
    UInt32                     fProtocolFaultSmid;
    /* Survive resetProtocolState(): the fault that forced the current
     * recovery is cleared from the live state, but the diagnosis must not be. */
    UInt32                     fStickyFaultCode;
    UInt32                     fStickyFaultSmid;
    UInt32                     fStickyFaultCount;
    /* Last decoded topology entry, published from the watchdog so no
     * allocation happens under fReplyLock. */
    UInt32                     fLastTopoHandle;
    UInt32                     fLastTopoReason;
    UInt32                     fLastTopoRate;
    UInt32                     fTopoRemovalHits;
    /* Which validation rejected a completion. Site: 1 reply-descriptor
     * flags, 2 SCSI reply frame, 3 PORT_ENABLE frame, 4 EVENT_NOTIFICATION
     * frame, 5 config page frame. Allocation-free, published by the watchdog. */
    UInt32                     fBadReplySite;
    UInt32                     fBadReplyValue;
    UInt32                     fBadReplySmid;
    UInt32                     fBadReplyCount;
    UInt32                     fBadReplyFunction;
    UInt32                     fBadReplyMsgLen;
    UInt32                     fBadReplyWord;
    /* Phy count with a negotiated link in the last SAS IO Unit Page0 read.
     * A link without an attached target means the IOC has not rediscovered,
     * which is a different state from an empty fabric. */
    UInt32                     fLinkUpPhys;
    /* SAS Device Page0 denied the attached handle (different handle, or a
     * non-zero access status). That is a target that needs rediscovery, not a
     * missing disk, and it must not leave the driver retrying forever. */
    bool                       fSasDeviceMismatch;
    UInt32                     fIdentityLostCount;
    volatile UInt32            fTopologyGeneration;
    volatile bool              fRemovalPending;
    bool                       fHotplugRetryRequested; /* new add outside IOC recovery */

    /* Bulk data buffer for block transfers. It stays resident
     * for the lifetime of the nub instead of being freed after one probe. */
    IOBufferMemoryDescriptor  *fDataBuffer;
    UInt64                     fDataPhys;
    UInt32                     fDataBytes;

    /* media identity, nub, and serialization of the shared SCSI IO
     * path (one outstanding SMID, one reply slot, one staging buffer). */
    MPRBlockDev               *fBlockDev;
    IOLock                    *fIOLock;
    UInt64                     fCapacityBlocks;
    UInt32                     fBlockSize;
    UInt32                     fNextIoSmid;
    volatile UInt32            fIoCount;
    volatile UInt32            fIoRetryCount;

    /* recovery, sense decoding and the write/UNMAP feature gates. */
    UInt16                     fFrozenSmid;      /* held until TMF completes */
    UInt16                     fTimedOutSmid;    /* last waitForSmid() timeout */
    bool                       fUnmapEnabled;
    bool                       fReinitInProgress;
    bool                       fDeviceOffline;   /* recovery failed: reject new IO */
    bool                       fMediaPresent;    /* storage-layer publication state */
    volatile UInt32            fMediaGeneration; /* reject IO queued for an old medium */
    bool                       fStarting;
    bool                       fMediaOfflinePending;
    bool                       fMediaOnlinePending;
    UInt32                     fHotplugAttempts;
    bool                       fStopping;
    bool                       fInjectTimeout;   /* mpr_timeout_test boot-arg */
    UInt32                     fInjectTmfFail;   /* mpr_tmf_fail boot-arg */
    bool                       fFastPathEnabled; /* SAS Device Page0 flags */
    bool                       fSuspended;       /* PM sleep in progress */
    volatile bool              fTopologyChanged; /* SAS discovery event seen */
    IOTimerEventSource        *fWatchdogTimer;
    thread_call_t              fHotplugCall;
    volatile bool              fHotplugPending;
    bool                       fHotplugRunning;
    volatile UInt32            fHotplugCooldown;
    UInt32                     fHighPriorityCredit;
    volatile UInt32            fEventReplyCount;
    volatile UInt32            fEventAckCount;
    volatile UInt32            fReplyFrameReturned;
    volatile UInt32            fCommandCount;
    volatile UInt32            fTmfCount;
    volatile UInt32            fResetCount;
    UInt8                      fLastSenseKey;
    UInt8                      fLastASC;
    UInt8                      fLastASCQ;

    char                       fVendor[16];
    char                       fProduct[20];
    char                       fRevision[8];

    /* SCSI IO completion tracking. */
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
    bool   initializeController(void);

    bool   allocateQueueMemory(void);
    void   freeQueueMemory(void);
    bool   sendIOCInit(void);
    bool   initializeQueues(void);

    bool   readManufacturingPage0(void);
    bool   readManufacturingInfo(void);

    bool   setupInterrupts(void);
    void   teardownInterrupts(void);
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *sender,
                                int index);
    void   processReplyQueue(void);
    void   watchdogTimeout(IOTimerEventSource *sender);
    void   hotplugThread(void);
    bool   mediaRemovalPending(void);
    void   invalidateMedia(void); /* caller holds fIOLock */
    bool   takeMediaOffline(void); /* drops fIOLock for notification */
    static void hotplugCall(thread_call_param_t param0,
                            thread_call_param_t param1);
    bool   waitForSmid(UInt32 timeoutMs, UInt32 smid);
    bool   takeReplySlot(UInt32 smid, UInt8 *reply, UInt32 replyLen, UInt8 *kind);
    bool   beginManagementRequest(UInt32 smid);
    bool   finishManagementRequest(UInt32 smid, UInt32 timeoutMs,
                                    UInt8 *reply, UInt8 *kind);
    void   recordProtocolFaultLocked(UInt32 code, UInt32 smid);
    void   recordProtocolFault(UInt32 code, UInt32 smid);
    void   noteBadReply(UInt32 site, UInt32 value, UInt32 smid,
                        const UInt8 *frame, UInt32 bytes);
    void   resetProtocolState(void);
    void   serviceProtocolState(void);
    void   recordTopologyEventLocked(const UInt8 *data, UInt32 bytes);

    bool   sendEventNotification(void);
    bool   sendPortEnable(void);
    bool   enableEventsAndPort(void);

    bool   readExtConfigPage(UInt8 extPageType, UInt32 pageAddress,
                             UInt8 *buffer, UInt32 bufferBytes,
                             UInt32 *actualBytes);
    bool   readSasIOUnitPage0(void);
    bool   readSasDevicePage0(UInt32 devHandle, UInt32 *outDeviceInfo = NULL);
    bool   discoverTopology(void);

    bool   postScsiIo(UInt32 smid);
    bool   scsiTur(void);
    bool   scsiInquiry(void);
    bool   probeScsiIdentity(void);
    bool   scsiReadCapacity16(void);
    bool   scsiReadCapacity10(void);
    bool   probeCapacity(void);

    bool   scsiReadWrite(bool write, UInt64 lba, UInt32 blocks,
                         UInt32 blockSize, UInt32 smid, bool diagnostics,
                         bool fua, IOReturn *status = NULL);
    bool   probeBlockRead(void);

    bool   allocatePageBuffer(void);
    bool   allocateResidentBuffers(void);
    bool   prepareTargetMedia(void);
    bool   publishBlockDevice(void);

    /* command submission, auto sense, task management and recovery. */
    void   postDefaultRequest(UInt32 smid);
    void   postHighPriorityRequest(UInt32 smid);
    void   postRequestDescriptor(UInt32 low, UInt32 high);
    void   releaseReplyFrame(UInt32 baddr);
    void   sendEventAck(UInt16 event, UInt32 context);
    void   postNextEventAckLocked(void);
    void   completeEventAckLocked(const UInt8 *reply, UInt32 replyBytes);
    IOReturn decodeSense(IOReturn fallback);
    bool   issueScsiCommand(const UInt8 *cdb, UInt32 cdbLen, UInt32 control,
                            const UInt8 *outData, UInt32 dataLen,
                            UInt32 smid, IOReturn *status);
    bool   scsiInquiryVpd(UInt8 page, UInt8 *buf, UInt32 len);
    IOReturn scsiSynchronizeCache(UInt64 lba, UInt32 blocks, UInt32 smid,
                                  bool barrier);
    IOReturn scsiUnmap(const IOBlockStorageDeviceExtent *extents,
                       UInt32 extentsCount, UInt32 smid);
    bool   sendTaskMgmt(UInt8 taskType, UInt16 taskMid, UInt32 smid);
    bool   recoverTimedOutCommand(UInt32 smid);
    bool   resetAndReinitialize(void);

public:
    virtual bool init(OSDictionary *properties = NULL) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                            unsigned long stateNumber,
                                            IOService *whatDevice) APPLE_KEXT_OVERRIDE;
    virtual IOReturn powerStateDidChangeTo(IOPMPowerFlags capabilities,
                                           unsigned long stateNumber,
                                           IOService *whatDevice) APPLE_KEXT_OVERRIDE;

    /* MPRBlockDev relays every request through these accessors. */
    IOReturn readBlocks(IOMemoryDescriptor *buffer, UInt64 block,
                        UInt64 nblks, UInt64 *actualBytes);
    IOReturn transferBlocks(bool write, IOMemoryDescriptor *buffer,
                            UInt64 block, UInt64 nblks, bool fua,
                            UInt64 *actualBytes);
    IOReturn synchronizeCache(UInt64 block, UInt64 nblks, bool barrier);
    IOReturn unmapBlocks(const IOBlockStorageDeviceExtent *extents,
                         UInt32 extentsCount);
    UInt64   getCapacityBlocks(void) const { return fCapacityBlocks; }
    bool     isDeviceOffline(void) const { return fDeviceOffline; }
    bool     isMediaPresent(void) const { return fMediaPresent; }
    UInt32   getBlockSize(void) const { return fBlockSize; }
    char    *getVendorString(void) { return fVendor; }
    char    *getProductString(void) { return fProduct; }
    char    *getRevisionString(void) { return fRevision; }
};

#endif /* MPRController_h */
