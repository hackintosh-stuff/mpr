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
    UInt8                      fLastReply[64];

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

public:
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

#endif /* MPRController_h */
