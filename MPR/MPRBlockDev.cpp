#include <IOKit/IOLib.h>
#include "MPRBlockDev.h"
#include "MPRController.h"

#define super IOBlockStorageDevice
OSDefineMetaClassAndStructors(MPRBlockDev, IOBlockStorageDevice)

static char kMPREmptyString[] = "";
static char kMPRDeviceInfo[] = "MPR SAS3008 STP/SATA";

bool
MPRBlockDev::init(OSDictionary *properties)
{
    fController = NULL;
    return super::init(properties);
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
    fController = NULL;
    super::stop(provider);
}

void
MPRBlockDev::setController(MPRController *controller)
{
    fController = controller;
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
    return fController != NULL ? fController->getVendorString() : kMPREmptyString;
}

char *
MPRBlockDev::getProductString(void)
{
    return fController != NULL ? fController->getProductString() : kMPREmptyString;
}

char *
MPRBlockDev::getRevisionString(void)
{
    return fController != NULL ? fController->getRevisionString() : kMPREmptyString;
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

    if (blockSize == NULL || fController == NULL) {
        return kIOReturnBadArgument;
    }
    size = fController->getBlockSize();
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

    if (maxBlock == NULL || fController == NULL) {
        return kIOReturnBadArgument;
    }
    blocks = fController->getCapacityBlocks();
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
    *mediaPresent = (fController != NULL) &&
                    (fController->getCapacityBlocks() != 0) &&
                    (fController->getBlockSize() != 0);
    if (changedState != NULL) {
        *changedState = false;
    }
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
    /* M3 is read-only; WRITE and UNMAP belong to M4. */
    *isWriteProtected = true;
    return kIOReturnSuccess;
}

IOReturn
MPRBlockDev::doAsyncReadWrite(IOMemoryDescriptor *buffer,
                              UInt64 block, UInt64 nblks,
                              IOStorageAttributes *attributes,
                              IOStorageCompletion *completion)
{
    UInt64  actualBytes = 0;
    IOReturn status;

    if (buffer == NULL || completion == NULL || fController == NULL) {
        return kIOReturnBadArgument;
    }

    /* A descriptor with kIODirectionOut carries data from memory to the
     * device. Reject it here rather than issuing a WRITE(10) in M3. */
    if ((buffer->getDirection() & kIODirectionOut) != 0) {
        return kIOReturnNotWritable;
    }

    /* MPRController::readBlocks() completes synchronously; the completion is
     * invoked after the controller lock has been released, so a client that
     * immediately issues another read cannot deadlock on that lock. */
    status = fController->readBlocks(buffer, block, nblks, &actualBytes);
    if (completion->action != NULL) {
        completion->action(completion->target, completion->parameter,
                           status, actualBytes);
    }
    return kIOReturnSuccess;
}
