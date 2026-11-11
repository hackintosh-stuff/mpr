#ifndef MPRController_h
#define MPRController_h

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <libkern/c++/OSMetaClass.h>

class IOPCIDevice;

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
    bool   runM3b(void);

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

#endif /* MPRController_h */
