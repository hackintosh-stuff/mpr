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
#include "MPRScsi.h"

class IOPCIDevice;
class MPRBlockDev;
class IOTimerEventSource;
struct IOBlockStorageDeviceExtent;
struct IOStorageCompletion;
struct MPRAsyncState;

/* One completion slot per SMID. processReplyQueue() fills it under
 * fReplyLock; each waiter consumes only its own slot, so a late reply can no
 * longer be mistaken for the completion of another command. */
struct MPRReplySlot
{
    volatile UInt32 valid;
    UInt16          smid;
    UInt8           kind;       /* 1 = address, 2 = fast success, 3 = reset abort */
    UInt8           reserved;
    UInt8           reply[64];
};

enum { kMPREventAckQueueDepth = 64 };
/* Watchdog ticks between unconditional topology snapshots. Discovery cannot
 * rely on firmware events: an expander may reattach a disk without one. */
enum { kMPRIdleScanTicks = 5 };
struct MPREventAckEntry
{
    UInt16 event;
    UInt32 context;
};

enum MPRCommandState {
    kMPRCommandFree = 0, kMPRCommandReserved, kMPRCommandPosted,
    kMPRCommandComplete, kMPRCommandQuarantined, kMPRCommandCancelled
};

struct MPRCommandOwner {
    UInt8 state;
    UInt8 function;
    UInt16 handle;
    UInt32 epoch;
};

/* A probe pass ends in one of three states. A device that is still becoming
 * ready after a wake is retried; only a definite failure consumes the bounded
 * recovery budget. */
enum MPRProbeResult {
    kMPRProbeOk = 0,
    kMPRProbeNotReady,
    kMPRProbeFailed
};
/* One per-target budget covers both a slow spin-up and a broken disk: the
 * worker probes at a one-second cooldown, so 60 unsuccessful passes is also
 * roughly 60 seconds before the target converges to offline. */
enum { kMPRMaxProbeRetries = 60 };
/* A definite failure escalates to the bounded IOC reset after this many
 * unsuccessful passes. */
enum { kMPRProbeResetThreshold = 3 };

/* Wake settling window. After a diag reset the expander and a spun-down disk
 * can take much longer than the steady-state 2 s budget to answer a config
 * page. Quarantining that SMID turns a transient into a protocol fault and a
 * second, fault-driven IOC reset, so config/event replies get a bounded
 * longer wait while this window is open. The watchdog ticks it down: nothing
 * sleeps under fIOLock merely to time the window. */
enum {
    kMPRWakeSettleTicks = 30,
    kMPRWakeConfigTimeoutMs = 8000,
    kMPRWakePortEnableTimeoutMs = 60000
};

/* Per-target state for multi-target isolation. A DevHandle is a firmware
 * address, not a disk: it can name a different device after a replug, so each
 * slot also carries the SAS address, stable identity and medium generation it
 * was probed for. Every published target is writable. */
enum { kMPRMaxTargets = 8 };
struct MPRTargetSlot
{
    UInt16 handle;
    UInt32 deviceInfo;
    UInt16 flags;
    UInt64 sasAddress;
    bool   fastPath;
    bool   present;         /* listed by the current topology snapshot */
    UInt64 capacityBlocks;
    UInt32 blockSize;
    bool   identityValid;
    char   identity[520];
    char   model[64];
    char   serial[32];
    MPRBlockDev *nub;
    /* Bumped whenever the medium behind this slot changes, so I/O and
     * synchronous commands accepted for the old device cannot act on the new
     * one. */
    UInt32 generation;
    bool   probeValid;      /* geometry and identity probed for this device */
    char   vendor[16];
    char   product[20];
    char   revision[8];
    bool   online;          /* storage client was told the medium is online */
    bool   offlinePending;
    bool   onlinePending;
    bool   removalRequested;
    /* Last completed stage of the removal protocol, so a failure is attributed
     * to the step that produced it instead of being flattened to "requested". */
    char   removalState[24];
    UInt32 scanToken;       /* last topology scan that listed this handle */
    /* A single budget for consecutive unsuccessful probe passes, whether the
     * device was not ready or genuinely failed. Cleared by a successful probe
     * and by a fresh insertion. */
    UInt32 probeRetries;
    MPRProbeResult probeState; /* ok / not-ready / failed, last pass */
    UInt32 failureCount;
    UInt32 writeCount;
    bool   unmapEnabled;
    bool   nativeFua;
    MPRUnmapLimits unmapLimits;
};

class MPRController : public IOService
{
    OSDeclareDefaultStructors(MPRController)

private:
    enum { kMPRTargetMalformedReply = 0x100, kMPRTargetRemovalUncertain };
    /* Bounded asynchronous request scheduler; its resident DMA windows are
     * separate from the synchronous management/probe staging area. */
    MPRAsyncState    *fAsync;
    UInt32           fMaxChainDepth;
    UInt32           fChainFrameBytes;
    bool             fPowerQuiesced;
    bool   initializeAsync(void);
    void   stopAsync(void);
    void   releaseAsync(bool dmaStopped);
    void   destroyAsyncState(void);
    void   asyncWorker(void);
    static void asyncCall(thread_call_param_t, thread_call_param_t);
    void   asyncCompletions(void);
    static void asyncCompletionCall(thread_call_param_t, thread_call_param_t);
    IOReturn beginAsyncBarrier(void);
    void   endAsyncBarrier(void);
    void   setAsyncAcceptance(bool enabled);
    bool   quiescePower(void);
    bool   resumePower(void);

    IOPCIDevice      *fPCIDevice;
    IOMemoryMap      *fMMIO;
    volatile UInt32  *fRegs;

    /* IOC Init control structures, all in low 4 GB physical memory. */
    IOBufferMemoryDescriptor *fQueueBuffer;
    IOBufferMemoryDescriptor *fReplyBuffer;
    IOBufferMemoryDescriptor *fRequestBuffer;
    IOBufferMemoryDescriptor *fSenseBuffer;
    UInt64           fSensePhys;
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
    /* A command remains owned after timeout until its original completion
     * arrives or the IOC has demonstrably stopped DMA. */
    MPRCommandOwner             fCommandOwners[128];
    UInt32                     fTransportEpoch;
    UInt32                     fUnexpectedCompletionCount;

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
    /* Write accounting: the medium is published writable, so every completed
     * write has to be attributable to an LBA and a SMID. */
    volatile UInt32            fWriteCount;
    volatile UInt64            fWriteBytes;
    volatile UInt64            fLastWriteLba;
    volatile UInt32            fLastWriteSmid;
    /* SAS Device Page0 denied the attached handle (different handle, or a
     * non-zero access status). That is a target that needs rediscovery, not a
     * missing disk, and it must not leave the driver retrying forever. */
    bool                       fSasDeviceMismatch;
    UInt32                     fIdentityLostCount;
    /* Target identity and advertised capabilities are re-probed after reset.
     * A handle is only a firmware address, never a persistent disk identity. */
    UInt32                     fIocCapabilities;
    UInt32                     fMaxDeviceHandle;
    UInt32                     fTargetDeviceInfo;
    UInt16                     fTargetDeviceFlags;
    UInt64                     fTargetSasAddress;
    bool                       fHasExpander;
    UInt32                     fDiscoveredDiskCount;
    bool                       fNativeFuaSupported;
    MPRUnmapLimits             fUnmapLimits;
    bool                       fTargetIdentityValid;
    char                       fTargetIdentity[520];
    bool                       fPhysicalChangePending;
    volatile UInt32            fTopologyGeneration;
    volatile bool              fRemovalPending;
    bool                       fHotplugRetryRequested; /* new add outside IOC recovery */
    /* A TARGET_NOT_RESPONDING event names a handle. It is latched here under
     * fReplyLock and promoted into the target table by the hotplug worker,
     * which is the only context allowed to change that table. */
    volatile UInt16            fPendingRemovalHandle;
    /* A removal that could not be proved, and controller-level recoveries that
     * were forced by a protocol fault which no I/O waiter owned. */
    UInt32                     fRemovalFailures;
    UInt32                     fFaultRecoveries;
    UInt32                     fIdleScanTicks;
    /* Watchdog ticks after a wake during which the IOC may legitimately still
     * report a non-operational doorbell. */
    UInt32                     fResumeGraceTicks;
    /* Watchdog ticks after a wake during which config/event replies may be
     * slow while the fabric settles. Written under fIOLock, decremented by
     * the watchdog and read by the synchronous management path. */
    volatile UInt32            fWakeSettleTicks;
    /* A disk that leaves the fabric behind an expander is not necessarily
     * rediscovered when it returns: without SMP there is no per-phy reset, so
     * the IOC only re-runs discovery on PortEnable. These count the age of the
     * removed media and the bounded rediscovery resets already spent. */
    volatile UInt32            fRemovedMediaCount;
    UInt32                     fRemovedAgeTicks;
    UInt32                     fRediscoveryResets;
    bool                       fRediscoverRequest;

    /* Bulk data buffer for block transfers. It stays resident
     * for the lifetime of the nub instead of being freed after one probe. */
    IOBufferMemoryDescriptor  *fDataBuffer;
    UInt64                     fDataPhys;
    UInt32                     fDataBytes;

    /* media identity, nub, and serialization of the shared SCSI IO
     * path (one outstanding SMID, one reply slot, one staging buffer). */
    IOLock                    *fIOLock;
    UInt64                     fCapacityBlocks;
    UInt32                     fBlockSize;
    UInt32                     fNextIoSmid;
    volatile UInt32            fIoCount;
    volatile UInt32            fIoRetryCount;
    /* The probe chain is serialized by fIOLock, so it can publish the decoded
     * status of its last command in one word instead of allocating or taking
     * another lock while classification travels up to probeTarget(). */
    IOReturn                   fLastProbeStatus;
    bool                       fProbeInProgress;

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
    /* mpr_timeout_every=N injects a lost reply every Nth runtime command, so
     * a soak can drive the timeout/TMF/reset chain repeatedly instead of once. */
    UInt32                     fInjectEvery;
    UInt32                     fInjectCounter;
    UInt32                     fInjectCount;
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

    /* Target table. Model and serial are the probed identity published in
     * IORegistry; they no longer gate write access. */
    MPRTargetSlot              fTargets[kMPRMaxTargets];
    UInt32                     fTargetCount;
    UInt32                     fScanSerial;
    char                       fTargetModel[64];
    char                       fTargetSerial[32];
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
    static void hotplugCall(thread_call_param_t param0,
                            thread_call_param_t param1);
    bool   waitForSmid(UInt32 timeoutMs, UInt32 smid);
    bool   takeReplySlot(UInt32 smid, UInt8 *reply, UInt32 replyLen, UInt8 *kind);
    bool   storeCommandReplyLocked(UInt32 smid, UInt8 kind,
                                   const UInt8 *reply, UInt32 bytes);
    bool   postOwnedRequestLocked(UInt32 smid, UInt32 flags, UInt32 high);
    bool   prepareScsiSense(UInt32 smid);
    UInt64 senseAddress(UInt32 smid) const;
    bool   commandDmaSafe(UInt32 smid);
    void   cancelUnpostedCommand(UInt32 smid);
    bool   drainTaskManagedCommands(UInt16 handle, UInt16 taskMid,
                                    bool wholeTarget);
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
                             UInt32 *actualBytes, UInt16 *iocStatus = NULL);
    bool   readSasIOUnitPage0(void);
    bool   readSasDevicePage0(UInt32 devHandle, UInt32 *outDeviceInfo = NULL);
    bool   discoverTopology(void);
    bool   readRaidMembership(UInt16 handle, bool *member);
    bool   probeStableIdentity(void);
    bool   scsiSataIdentify(UInt8 identify[512]);
    bool   probeTargetCapabilities(void);
    bool   checkTargetIdentity(void);
    bool   isNativeFuaSupported(void) const { return fNativeFuaSupported; }
    IOReturn synchronizeCacheNow(UInt64 block, UInt64 nblks, bool barrier,
                                 UInt32 expectedGeneration, UInt16 handle);
    IOReturn unmapBlocksNow(const IOBlockStorageDeviceExtent *extents,
                            UInt32 extentsCount, UInt32 expectedGeneration,
                            UInt16 handle);

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
    bool   scsiPostIo(bool write, UInt64 lba, UInt32 blocks, UInt32 blockSize,
                      UInt32 smid, bool fua, UInt64 dataPhys,
                      IOReturn *status);
    bool   scsiWaitChunk(UInt32 smid, UInt32 dataLen, IOReturn *status,
                         UInt32 *actualBytes = NULL,
                         bool *unitAttention = NULL);
    bool   probeBlockRead(void);

    bool   allocatePageBuffer(void);
    bool   allocateResidentBuffers(void);
    bool   prepareTargetMedia(void);
    bool   ensureTargetsPublished(void);
    bool   publishTarget(MPRTargetSlot *slot);
    MPRProbeResult probeTarget(MPRTargetSlot *slot);
    void   beginProbeStage(void);
    void   noteProbeStatus(IOReturn status);
    void   recordProbeResult(MPRTargetSlot *slot, MPRProbeResult result);
    bool   selectTarget(UInt16 handle);
    MPRTargetSlot *allocTargetSlot(void);
    void   requestTargetRemoval(MPRTargetSlot *slot);
    void   invalidateTarget(MPRTargetSlot *slot);
    void   invalidateTargets(void);
    bool   retireTarget(MPRTargetSlot *slot);
    void   setTargetRemovalState(MPRTargetSlot *slot, const char *state);
    bool   removeTargetDevice(UInt16 handle);
    bool   targetDevicePresent(UInt16 handle, bool *present, UInt64 *sasAddress);
    UInt32 liveTargetCount(void);
    void   publishTargetProperties(void);
    void   publishTargetCounters(void);

    /* command submission, auto sense, task management and recovery. */
    void   postDefaultRequest(UInt32 smid);
    void   postHighPriorityRequest(UInt32 smid);
    void   postRequestDescriptor(UInt32 low, UInt32 high);
    void   releaseReplyFrame(UInt32 baddr);
    void   sendEventAck(UInt16 event, UInt32 context);
    void   postNextEventAckLocked(void);
    void   completeEventAckLocked(const UInt8 *reply, UInt32 replyBytes);
    IOReturn decodeSense(UInt32 smid, UInt32 senseCount, IOReturn fallback,
                         bool *unitAttention = NULL);
    bool   validateScsiCompletion(UInt32 smid, UInt8 kind,
                                  const UInt8 *reply, UInt32 dataLen,
                                  bool allowShort, IOReturn *status,
                                  UInt32 *actualBytes = NULL,
                                  bool *unitAttention = NULL);
    bool   issueScsiCommand(const UInt8 *cdb, UInt32 cdbLen, UInt32 control,
                            const UInt8 *outData, UInt32 dataLen,
                            UInt32 smid, IOReturn *status,
                            UInt32 *actualBytes = NULL,
                            bool allowShort = false,
                            bool *unitAttention = NULL);
    bool   scsiInquiryVpd(UInt8 page, UInt8 *buf, UInt32 len,
                          UInt32 *actualBytes = NULL);
    IOReturn scsiSynchronizeCache(UInt64 lba, UInt64 blocks, UInt32 smid,
                                  bool barrier);
    IOReturn scsiUnmap(const IOBlockStorageDeviceExtent *extents,
                       UInt32 extentsCount, UInt32 smid);
    bool   sendTaskMgmt(UInt8 taskType, UInt16 taskMid, UInt32 smid,
                        UInt16 handle, UInt16 *iocStatus = NULL);
    bool   recoverTimedOutCommand(UInt32 smid);
    bool   resetAndReinitialize(void);

public:
    virtual bool init(OSDictionary *properties = NULL) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPowerState(unsigned long stateNumber,
                                   IOService *whatDevice) APPLE_KEXT_OVERRIDE;
    virtual void systemWillShutdown(IOOptionBits specifier) APPLE_KEXT_OVERRIDE;

    /* Multi-target discovery. */
    MPRTargetSlot *findTarget(UInt16 handle);
    UInt32 targetCount(void) const { return fTargetCount; }
    UInt32 getBlockSizeForHandle(UInt16 handle) {
        MPRTargetSlot *slot = findTarget(handle);
        return slot != NULL ? slot->blockSize : 0;
    }
    UInt64 getCapacityForHandle(UInt16 handle) {
        MPRTargetSlot *slot = findTarget(handle);
        return slot != NULL ? slot->capacityBlocks : 0;
    }
    /* A published target is one whose current device was probed and whose
     * medium has not been retired; a bare topology entry is not media. */
    bool isMediaPresentForHandle(UInt16 handle) {
        MPRTargetSlot *slot = findTarget(handle);
        return slot != NULL && slot->present && slot->probeValid;
    }

    /* MPRBlockDev relays every request through these accessors. Each request
     * carries its own DevHandle so completions stay bound to one target. */
    IOReturn submitAsyncBlocks(bool write, IOMemoryDescriptor *buffer,
                               UInt64 block, UInt64 nblks, bool fua,
                               const IOStorageCompletion *completion,
                               UInt16 devHandle = 0);
    IOReturn readBlocks(IOMemoryDescriptor *buffer, UInt64 block,
                        UInt64 nblks, UInt64 *actualBytes);
    IOReturn transferBlocks(bool write, IOMemoryDescriptor *buffer,
                            UInt64 block, UInt64 nblks, bool fua,
                            UInt64 *actualBytes);
    IOReturn synchronizeCache(UInt64 block, UInt64 nblks, bool barrier,
                              UInt16 handle);
    IOReturn unmapBlocks(const IOBlockStorageDeviceExtent *extents,
                         UInt32 extentsCount, UInt16 handle);
    UInt64   getCapacityBlocks(void) const { return fCapacityBlocks; }
    bool     isDeviceOffline(void) const { return fDeviceOffline; }
    bool     isMediaPresent(void) const { return fMediaPresent; }
    UInt32   getBlockSize(void) const { return fBlockSize; }
    char    *getVendorString(void) { return fVendor; }
    char    *getProductString(void) { return fProduct; }
    char    *getRevisionString(void) { return fRevision; }
    /* Identity strings are per target: every published disk has its own
     * INQUIRY response, not the response of whichever target was probed last. */
    char    *getVendorStringForHandle(UInt16 handle) {
        MPRTargetSlot *slot = findTarget(handle);
        return slot != NULL ? slot->vendor : fVendor;
    }
    char    *getProductStringForHandle(UInt16 handle) {
        MPRTargetSlot *slot = findTarget(handle);
        return slot != NULL ? slot->product : fProduct;
    }
    char    *getRevisionStringForHandle(UInt16 handle) {
        MPRTargetSlot *slot = findTarget(handle);
        return slot != NULL ? slot->revision : fRevision;
    }
};

#endif /* MPRController_h */
