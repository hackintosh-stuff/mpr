#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "MPRController.h"

#define super IOService
OSDefineMetaClassAndStructors(MPRController, IOService)

/* PCI config 0x00: vendor in the low 16 bits, device in the high 16 bits. */
#define MPR_VENDOR_DEVICE 0x00971000U

/* First memory BAR on this card is BAR1 (config register 0x14), 64 KB MMIO.
 * BAR0 is an I/O BAR; mapping 0x10 would give the wrong address space. */
#define MPR_MMIO_BAR_REG 0x14

/* MPI2 system interface registers (FreeBSD reference/mpi/mpi2.h). */
#define MPI2_DOORBELL_OFFSET              0x00
#define MPI2_WRITE_SEQUENCE_OFFSET        0x04
#define MPI2_HOST_DIAGNOSTIC_OFFSET       0x08
#define MPI2_HOST_INTERRUPT_STATUS_OFFSET 0x30
#define MPI2_HOST_INTERRUPT_MASK_OFFSET   0x34
#define MPI2_REPLY_FREE_HOST_INDEX_OFFSET 0x48
#define MPI2_REPLY_POST_HOST_INDEX_OFFSET 0x6C

struct MPRRegisterDump
{
    const char *name;
    UInt32      offset;
    UInt32      value;
};

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
        IOLog("MPR M0: probe rejected vendor/device 0x%08x\n",
              (unsigned int)vendorDevice);
        return NULL;
    }

    IOLog("MPR M0: probe matched SAS3008 (0x%08x)\n",
          (unsigned int)vendorDevice);
    return this;
}

bool
MPRController::start(IOService *provider)
{
    IOPCIDevice      *pci;
    volatile UInt32  *regs;
    IOVirtualAddress  base;
    IOByteCount       length;
    unsigned int      i;

    static MPRRegisterDump dump[] = {
        { "Doorbell",             MPI2_DOORBELL_OFFSET,               0 },
        { "WriteSequence",        MPI2_WRITE_SEQUENCE_OFFSET,         0 },
        { "HostDiagnostic",       MPI2_HOST_DIAGNOSTIC_OFFSET,        0 },
        { "HostInterruptStatus",  MPI2_HOST_INTERRUPT_STATUS_OFFSET,  0 },
        { "HostInterruptMask",    MPI2_HOST_INTERRUPT_MASK_OFFSET,    0 },
        { "ReplyFreeHostIndex",   MPI2_REPLY_FREE_HOST_INDEX_OFFSET,  0 },
        { "ReplyPostHostIndex",   MPI2_REPLY_POST_HOST_INDEX_OFFSET,  0 }
    };

    if (!super::start(provider)) {
        return false;
    }

    pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == NULL) {
        super::stop(provider);
        return false;
    }
    fPCIDevice = pci;

    if (!pci->setMemoryEnable(true) || !pci->setBusMasterEnable(true)) {
        IOLog("MPR M0: failed to enable memory decode / bus master\n");
        super::stop(provider);
        return false;
    }

    fMMIO = pci->mapDeviceMemoryWithRegister(MPR_MMIO_BAR_REG);
    if (fMMIO == NULL) {
        IOLog("MPR M0: BAR 0x%02x mapping failed\n", MPR_MMIO_BAR_REG);
        super::stop(provider);
        return false;
    }

    base   = fMMIO->getVirtualAddress();
    length = fMMIO->getLength();
    regs   = (volatile UInt32 *)base;

    IOLog("MPR M0: BAR 0x%02x mapped, length %llu bytes\n",
          MPR_MMIO_BAR_REG, (unsigned long long)length);

    for (i = 0; i < sizeof(dump) / sizeof(dump[0]); i++) {
        dump[i].value = regs[dump[i].offset / sizeof(UInt32)];
        IOLog("MPR M0: %-22s @0x%02x = 0x%08x\n", dump[i].name,
              (unsigned int)dump[i].offset, (unsigned int)dump[i].value);
        setProperty(dump[i].name, (unsigned long long)dump[i].value, 32);
    }

    setProperty("MPRMMIOBAR", "0x14");
    setProperty("MPRMMIOLength", (unsigned long long)length, 64);
    setProperty("MPRStage", "M0");
    setProperty("MPRBuild", "0.1.0");

    IOLog("MPR M0: start complete, PCI matched and MMIO readable\n");
    return true;
}

void
MPRController::stop(IOService *provider)
{
    if (fMMIO != NULL) {
        fMMIO->release();
        fMMIO = NULL;
    }
    fPCIDevice = NULL;
    super::stop(provider);
}
