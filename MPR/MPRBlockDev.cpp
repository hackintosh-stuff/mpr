#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include "MPRBlockDev.h"
#include "MPRController.h"

#define super IOBlockStorageDevice
OSDefineMetaClassAndStructors(MPRBlockDev, IOBlockStorageDevice)

static char kMPRDeviceInfo[] = "MPR SAS3008 STP/SATA";

bool
MPRBlockDev::init(OSDictionary *properties)
{
    fController = NULL;
    fControllerLock = NULL;
    fLastMediaPresent = false;
    fDevHandle = 0;
    fReadOnly = true;
    bzero(fVendor, sizeof(fVendor));
    bzero(fProduct, sizeof(fProduct));
    bzero(fRevision, sizeof(fRevision));
    if (!super::init(properties)) return false;
    fControllerLock = IOLockAlloc();
    return fControllerLock != NULL;
}

void
MPRBlockDev::free(void)
{
    setController(NULL);
    if (fControllerLock != NULL) {
        IOLockFree(fControllerLock);
        fControllerLock = NULL;
    }
    super::free();
}

MPRController *
MPRBlockDev::copyController(void)
{
    if (fControllerLock == NULL) return NULL;
    IOLockLock(fControllerLock);
    MPRController *controller = fController;
    if (controller != NULL) controller->retain();
    IOLockUnlock(fControllerLock);
    return controller;
}

void
MPRBlockDev::refreshIdentity(void)
{
    IOLockLock(fControllerLock);
    if (fController != NULL) {
        strlcpy(fVendor, fController->getVendorString(), sizeof(fVendor));
        strlcpy(fProduct, fController->getProductString(), sizeof(fProduct));
        strlcpy(fRevision, fController->getRevisionString(), sizeof(fRevision));
    }
    IOLockUnlock(fControllerLock);
}

IOReturn
MPRBlockDev::notifyMediaState(UInt32 state)
{
    if (state == kIOMediaStateOnline) refreshIdentity();
    /* Deliver to the matched storage driver and preserve its actual result.
     * messageClients() does not expose each recipient's failure. In
     * particular, ONLINE is rejected while the old IOMedia still exists.
     * The controller must call this with no transport lock held. */
    IOBlockStorageDriver *driver = NULL;
    OSIterator *iter = getClientIterator();
    if (iter != NULL) {
        OSObject *object;
        while ((object = iter->getNextObject()) != NULL) {
            driver = OSDynamicCast(IOBlockStorageDriver, object);
            if (driver != NULL) {
                driver->retain();
                break;
            }
        }
        iter->release();
    }
    if (driver == NULL) {
        return kIOReturnNotAttached;
    }
    /* Initial matching may have accepted the new media before a deferred
     * ONLINE notification. OFFLINE is always delivered explicitly. */
    if (state == kIOMediaStateOnline &&
        driver->getMediaState() == kIOMediaStateOnline) {
        driver->release();
        return kIOReturnSuccess;
    }
    IOReturn result = messageClient(kIOMessageMediaStateHasChanged, driver,
                                    (void *)(uintptr_t)state);
    driver->release();
    return result;
}

bool
MPRBlockDev::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    /* IOBlockStorageDriver matches on this property; set it explicitly as
     * well as through IOBlockStorageDevice::init so the nub cannot lose it
     * when a future SDK changes the base implementation. */
    setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
    setProperty("Physical Interconnect", "SAS");
    setProperty("MPRTransport", "MPRController");
    return true;
}

void
MPRBlockDev::stop(IOService *provider)
{
    setController(NULL);
    super::stop(provider);
}

void
MPRBlockDev::setController(MPRController *controller, UInt16 handle, bool readOnly)
{
    if (fControllerLock == NULL) return;
    if (controller != NULL) controller->retain();
    IOLockLock(fControllerLock);
    MPRController *previous = fController;
    fController = controller;
    if (controller != NULL) {
        fDevHandle = handle;
        fReadOnly = readOnly;
    } else {
        fReadOnly = true;
    }
    IOLockUnlock(fControllerLock);
    if (previous != NULL) previous->release();
    if (controller != NULL) {
        refreshIdentity();
        setProperty("MPRBlockDevReadOnly", fReadOnly);
        setProperty("MPRBlockDevHandle", (unsigned long long)fDevHandle, 16);
    }
}

IOReturn
MPRBlockDev::doEjectMedia(void)
{
    return kIOReturnUnsupported;
}

IOReturn
MPRBlockDev::doFormatMedia(UInt64 byteCapacity)
{
    return kIOReturnUnsupported;
}

UInt32
MPRBlockDev::doGetFormatCapacities(UInt64 *capacities,
                                  UInt32 capacitiesMaxCount) const
{
    return 0;
}

char *
MPRBlockDev::getVendorString(void)
{
    return fVendor;
}

char *
MPRBlockDev::getProductString(void)
{
    return fProduct;
}

char *
MPRBlockDev::getRevisionString(void)
{
    return fRevision;
}

char *
MPRBlockDev::getAdditionalDeviceInfoString(void)
{
    return kMPRDeviceInfo;
}

IOReturn
MPRBlockDev::reportBlockSize(UInt64 *blockSize)
{
    UInt64 size;

    if (blockSize == NULL) return kIOReturnBadArgument;
    MPRController *controller = copyController();
    if (controller == NULL) return kIOReturnNoMedia;
    size = controller->getBlockSizeForHandle(fDevHandle);
    controller->release();
    if (size == 0) {
        return kIOReturnNoMedia;
    }
    *blockSize = size;
    return kIOReturnSuccess;
}

IOReturn
MPRBlockDev::reportEjectability(bool *isEjectable)
{
    if (isEjectable == NULL) {
        return kIOReturnBadArgument;
    }
    *isEjectable = false;
    return kIOReturnSuccess;
}

IOReturn
MPRBlockDev::reportMaxValidBlock(UInt64 *maxBlock)
{
    UInt64 blocks;

    if (maxBlock == NULL) return kIOReturnBadArgument;
    MPRController *controller = copyController();
    if (controller == NULL) return kIOReturnNoMedia;
    blocks = controller->getCapacityForHandle(fDevHandle);
    controller->release();
    if (blocks == 0) {
        return kIOReturnNoMedia;
    }
    *maxBlock = blocks - 1;
    return kIOReturnSuccess;
}

IOReturn
MPRBlockDev::reportMediaState(bool *mediaPresent, bool *changedState)
{
    if (mediaPresent == NULL) {
        return kIOReturnBadArgument;
    }
    MPRController *controller = copyController();
    bool present = controller != NULL &&
                   controller->isMediaPresentForHandle(fDevHandle);
    if (controller != NULL) controller->release();

    *mediaPresent = present;
    if (changedState != NULL) {
        *changedState = (present != fLastMediaPresent);
    }
    fLastMediaPresent = present;
    return kIOReturnSuccess;
}

IOReturn
MPRBlockDev::reportRemovability(bool *isRemovable)
{
    if (isRemovable == NULL) {
        return kIOReturnBadArgument;
    }
    *isRemovable = false;
    return kIOReturnSuccess;
}

IOReturn
MPRBlockDev::reportWriteProtection(bool *isWriteProtected)
{
    if (isWriteProtected == NULL) {
        return kIOReturnBadArgument;
    }
    /* A target is writable only when an explicit MPRWritableDevices rule
     * matched its identity. Everything else stays read-only for its whole
     * lifetime, so a newly attached ZFS member can never receive a write. */
    MPRController *controller = copyController();
    *isWriteProtected = fReadOnly || controller == NULL ||
                        !controller->isMediaPresent();
    if (controller != NULL) controller->release();
    return kIOReturnSuccess;
}

IOReturn
MPRBlockDev::doAsyncReadWrite(IOMemoryDescriptor *buffer,
                              UInt64 block, UInt64 nblks,
                              IOStorageAttributes *attributes,
                              IOStorageCompletion *completion)
{
    IOReturn status;
    bool    write, fua;

    if (buffer == NULL || completion == NULL || completion->action == NULL)
        return kIOReturnBadArgument;
    if (buffer->getDirection() != kIODirectionIn &&
        buffer->getDirection() != kIODirectionOut) return kIOReturnBadArgument;
    MPRController *controller = copyController();
    if (controller == NULL) return kIOReturnNoMedia;

    /* A descriptor with kIODirectionOut carries data from memory to the
     * device; kIODirectionIn carries data read from the device. */
    write = (buffer->getDirection() & kIODirectionOut) != 0;
    fua = (attributes != NULL &&
           (attributes->options & kIOStorageOptionForceUnitAccess) != 0);
    /* Defence in depth: even if a storage client ignores reportWriteProtection,
     * no write descriptor is accepted for a read-only target. */
    if (write && fReadOnly) {
        controller->release();
        return kIOReturnNotWritable;
    }

    /* Acceptance retains the buffer and copies the completion. Commands from
     * different requests share the bounded pool; callback runs outside locks
     * in a separate context, permitting reentrant reads or cache fences. */
    status = controller->submitAsyncBlocks(write, buffer, block, nblks,
                                           fua, completion, fDevHandle);
    controller->release();
    return status;
}

IOReturn
MPRBlockDev::doSynchronizeCache(void)
{
    /* Deprecated entry point, kept for older IOStorageFamily clients. */
    if (fReadOnly) return kIOReturnSuccess; /* nothing was written: no command */
    MPRController *controller = copyController();
    if (controller == NULL) return kIOReturnNoMedia;
    IOReturn status = controller->synchronizeCache(0, 0, false);
    controller->release();
    return status;
}

IOReturn
MPRBlockDev::doSynchronize(UInt64 block, UInt64 nblks,
                           IOStorageSynchronizeOptions options)
{
    if (fReadOnly) return kIOReturnSuccess;
    bool barrier = (options & kIOStorageSynchronizeOptionBarrier) != 0;

    MPRController *controller = copyController();
    if (controller == NULL) return kIOReturnNoMedia;
    IOReturn status = controller->synchronizeCache(block, nblks, barrier);
    controller->release();
    return status;
}

IOReturn
MPRBlockDev::doUnmap(IOBlockStorageDeviceExtent *extents,
                     UInt32 extentsCount, IOStorageUnmapOptions options)
{
    if (fReadOnly) return kIOReturnNotWritable;
    MPRController *controller = copyController();
    if (controller == NULL) return kIOReturnNoMedia;
    IOReturn status = controller->unmapBlocks(extents, extentsCount);
    controller->release();
    return status;
}
