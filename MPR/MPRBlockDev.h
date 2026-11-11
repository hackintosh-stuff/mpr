#ifndef MPRBlockDev_h
#define MPRBlockDev_h

#include <IOKit/storage/IOBlockStorageDevice.h>

class MPRController;

/* M3-d: transport nub for the single STP/SATA target. IOStorageFamily's
 * IOBlockStorageDriver matches on device-type = Generic, then creates the
 * IOMedia object. Every request is relayed to MPRController; the nub owns no
 * hardware state of its own. */
class MPRBlockDev : public IOBlockStorageDevice
{
    OSDeclareDefaultStructors(MPRBlockDev)

private:
    MPRController *fController;

public:
    virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    void setController(MPRController *controller);

    virtual IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doFormatMedia(UInt64 byteCapacity) APPLE_KEXT_OVERRIDE;
    virtual UInt32 doGetFormatCapacities(UInt64 *capacities,
                                         UInt32 capacitiesMaxCount) const APPLE_KEXT_OVERRIDE;
    virtual char *getVendorString(void) APPLE_KEXT_OVERRIDE;
    virtual char *getProductString(void) APPLE_KEXT_OVERRIDE;
    virtual char *getRevisionString(void) APPLE_KEXT_OVERRIDE;
    virtual char *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportBlockSize(UInt64 *blockSize) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportEjectability(bool *isEjectable) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportMaxValidBlock(UInt64 *maxBlock) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportMediaState(bool *mediaPresent,
                                      bool *changedState = 0) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportRemovability(bool *isRemovable) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportWriteProtection(bool *isWriteProtected) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer,
                                      UInt64 block, UInt64 nblks,
                                      IOStorageAttributes *attributes,
                                      IOStorageCompletion *completion) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doSynchronizeCache(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doSynchronize(UInt64 block, UInt64 nblks,
                                   IOStorageSynchronizeOptions options = 0) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doUnmap(IOBlockStorageDeviceExtent *extents,
                             UInt32 extentsCount,
                             IOStorageUnmapOptions options = 0) APPLE_KEXT_OVERRIDE;
};

#endif /* MPRBlockDev_h */
