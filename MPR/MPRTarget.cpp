#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <string.h>
#include "MPRController.h"
#include "MPRBlockDev.h"
#include "MPI/mpi2_target.h"

/* Discovery and disk-level SCSI policy. The MPI wire definitions come from
 * FreeBSD; CAM's disk-level duties are implemented here for IOBlockStorage. */
enum {
    kMPRTargetConfigBytes = 4096,
    kMPRUnmapBatchDescriptors = 32
};

static UInt16 mprReadLe16(const UInt8 *p)
{ return (UInt16)(p[0] | ((UInt16)p[1] << 8)); }
static UInt32 mprReadLe32(const UInt8 *p)
{ return (UInt32)p[0] | ((UInt32)p[1] << 8) |
         ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24); }
static UInt64 mprReadLe64(const UInt8 *p)
{ return (UInt64)mprReadLe32(p) | ((UInt64)mprReadLe32(p + 4) << 32); }

bool
MPRController::readExtConfigPage(UInt8 extPageType, UInt32 pageAddress,
                                 UInt8 *buffer, UInt32 bufferBytes,
                                 UInt32 *actualBytes, UInt16 *iocStatus)
{
    if (actualBytes != NULL) *actualBytes = 0;
    if (iocStatus != NULL) *iocStatus = 0xFFFF;
    if (fProtocolRecoveryPending || buffer == NULL ||
        bufferBytes < sizeof(MPRExtPageHeader) || bufferBytes > kMPRTargetConfigBytes ||
        fRequestBuffer == NULL || fPageBuffer == NULL ||
        fPageBuffer->getLength() < kMPRTargetConfigBytes) return false;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) return false;
    frame += 4 * fRequestFrameSize;
    MPRConfigRequest *config = (MPRConfigRequest *)frame;
    UInt8 replyBuf[64], replyKind = 0;
    UInt32 pageBytes = 0;
    UInt8 pageVersion = 0;

    for (UInt32 phase = 0; phase < 2; phase++) {
        if (!beginManagementRequest(4)) return false;
        memset(frame, 0, fRequestFrameSize);
        config->Function = MPI2_FUNCTION_CONFIG;
        config->Action = phase == 0 ? MPI2_CONFIG_ACTION_PAGE_HEADER
                                   : MPI2_CONFIG_ACTION_PAGE_READ_CURRENT;
        config->Header.PageType = MPI2_CONFIG_PAGETYPE_EXTENDED;
        config->Header.PageNumber = 0;
        config->Header.PageVersion = pageVersion;
        config->ExtPageType = extPageType;
        config->ExtPageLength = (UInt16)(pageBytes / 4);
        config->PageAddress = pageAddress;
        if (phase != 0) {
            memset(fPageBuffer->getBytesNoCopy(), 0, kMPRTargetConfigBytes);
            config->SgeFlagsLength =
                ((UInt32)(MPI2_SGE_FLAGS_SIMPLE_ELEMENT | MPI2_SGE_FLAGS_LAST_ELEMENT |
                          MPI2_SGE_FLAGS_END_OF_BUFFER | MPI2_SGE_FLAGS_64_BIT_ADDRESSING)
                 << MPI2_SGE_FLAGS_SHIFT) | pageBytes;
            config->SgeAddress = fPagePhys;
        }
        postDefaultRequest(4);
        if (!finishManagementRequest(4, 2000, replyBuf, &replyKind)) return false;
        const MPRConfigReply *reply = (const MPRConfigReply *)replyBuf;
        if (replyKind != 1 || reply->Function != MPI2_FUNCTION_CONFIG ||
            reply->MsgLength * 4U < sizeof(*reply) || reply->Action != config->Action) {
            recordProtocolFault(kMPRTargetMalformedReply, 4);
            return false;
        }
        UInt16 status = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
        if (iocStatus != NULL) *iocStatus = status;
        if (status != MPI2_IOCSTATUS_SUCCESS) return false;
        UInt32 bytes = (UInt32)reply->ExtPageLength * 4;
        if ((reply->Header.PageType & 0x0F) != MPI2_CONFIG_PAGETYPE_EXTENDED ||
            reply->Header.PageNumber != 0 || reply->ExtPageType != extPageType ||
            bytes < sizeof(MPRExtPageHeader) || bytes > bufferBytes ||
            (phase != 0 && bytes != pageBytes)) return false;
        pageBytes = bytes;
        pageVersion = reply->Header.PageVersion;
    }
    const MPRExtPageHeader *header =
        (const MPRExtPageHeader *)fPageBuffer->getBytesNoCopy();
    if (header->PageNumber != 0 ||
        (header->PageType & 0x0F) != MPI2_CONFIG_PAGETYPE_EXTENDED ||
        header->ExtPageType != extPageType || header->PageVersion != pageVersion ||
        (UInt32)header->ExtPageLength * 4 != pageBytes) return false;
    memset(buffer, 0, bufferBytes);
    memcpy(buffer, header, pageBytes);
    if (actualBytes != NULL) *actualBytes = pageBytes;
    return true;
}

bool
MPRController::readRaidMembership(UInt16 handle, bool *member)
{
    if (member == NULL) return false;
    *member = false;
    if ((fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID) == 0)
        return true;
    UInt8 page[kMPRTargetConfigBytes], seen[32] = {};
    UInt32 cursor = 0xFF;
    for (UInt32 n = 0; n < 256; n++) {
        UInt32 actual = 0;
        UInt16 status = 0xFFFF;
        if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_RAID_CONFIG,
                               MPI2_RAID_PGAD_FORM_GET_NEXT_CONFIGNUM | cursor,
                               page, sizeof(page), &actual, &status)) {
            /* INVALID_PAGE is the documented end of GET_NEXT enumeration,
             * never a generic permission to expose a disk after an error. */
            if (status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE &&
                !fProtocolRecoveryPending) return true;
            setProperty("MPRRaidFilterState", "configuration-unavailable");
            return false;
        }
        bool contains = false;
        if (!mprRaidConfigContains(page, actual, handle, &contains)) {
            setProperty("MPRRaidFilterState", "malformed-configuration");
            return false;
        }
        UInt8 number = page[0x0B];
        if ((seen[number / 8] & (1U << (number % 8))) != 0) return false;
        seen[number / 8] |= (UInt8)(1U << (number % 8));
        cursor = number;
        if (contains) { *member = true; return true; }
    }
    setProperty("MPRRaidFilterState", "configuration-enumeration-limit");
    return false;
}

bool
MPRController::readSasDevicePage0(UInt32 devHandle, UInt32 *outDeviceInfo)
{
    UInt8 page[512];
    UInt32 actual = 0;
    if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                           MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | devHandle,
                           page, sizeof(page), &actual)) return false;
    if (actual < 0x22 || mprReadLe16(page + 0x18) != devHandle ||
        page[0x17] != 0) {
        fSasDeviceMismatch = true;
        return false;
    }
    UInt16 flags = mprReadLe16(page + 0x20);
    if ((flags & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) == 0 ||
        (flags & (MPI2_SAS_DEVICE0_FLAGS_UNSUPPORTED_DEVICE |
                  MPI2_SAS_DEVICE0_FLAGS_UNAUTHORIZED_DEVICE)) != 0) return false;
    UInt32 info = mprReadLe32(page + 0x1C);
    if (outDeviceInfo != NULL) *outDeviceInfo = info;
    fTargetDeviceInfo = info;
    fTargetDeviceFlags = flags;
    fTargetSasAddress = mprReadLe64(page + 0x0C);
    fFastPathEnabled = (flags & MPI25_SAS_DEVICE0_FLAGS_FAST_PATH_CAPABLE) != 0 &&
                      (flags & MPI25_SAS_DEVICE0_FLAGS_ENABLED_FAST_PATH) != 0;
    char address[24];
    snprintf(address, sizeof(address), "%016llx", (unsigned long long)fTargetSasAddress);
    setProperty("MPRSasDeviceSasAddress", address);
    setProperty("MPRSasDeviceHandle", (unsigned long long)devHandle, 16);
    setProperty("MPRSasDeviceInfo", (unsigned long long)info, 32);
    setProperty("MPRSasDeviceFlags", (unsigned long long)flags, 16);
    setProperty("MPRSasDeviceParent", (unsigned long long)mprReadLe16(page + 0x14), 16);
    setProperty("MPRSasDeviceSlot", (unsigned long long)mprReadLe16(page + 0x08), 16);
    setProperty("MPRSasDeviceEnclosure", (unsigned long long)mprReadLe16(page + 0x0A), 16);
    setProperty("MPRSasDevicePhyNum", (unsigned long long)page[0x16], 8);
    setProperty("MPRFastPathEnabled", fFastPathEnabled);
    return true;
}

bool
MPRController::readSasIOUnitPage0(void)
{
    UInt8 page[512];
    UInt32 actual = 0, links = 0, selected = 0, candidates = 0, cursor = 0xFFFF;
    UInt32 disks = 0;
    bool expander = false;
    if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_IO_UNIT, 0,
                           page, sizeof(page), &actual) || actual < 0x10) return false;
    UInt32 phys = page[0x0C];
    if (phys > (actual - 0x10) / 0x14) return false;
    for (UInt32 i = 0; i < phys; i++) {
        const UInt8 *phy = page + 0x10 + i * 0x14;
        if ((phy[3] & 0x0F) >= 8) links++;
        char name[32];
        snprintf(name, sizeof(name), "MPRPhy%uDevHandle", (unsigned)i);
        setProperty(name, (unsigned long long)mprReadLe16(phy + 8), 16);
        snprintf(name, sizeof(name), "MPRPhy%uLinkRate", (unsigned)i);
        setProperty(name, (unsigned long long)(phy[3] & 0x0F), 8);
    }
    setProperty("MPRNumPhys", (unsigned long long)phys, 8);
    fSasDeviceMismatch = false;

    /* GET_NEXT_HANDLE reaches the terminal disk behind a backplane/expander.
     * IOC PHY handles alone can name the expander, not its attached disk.
     * The scan is merged into the persistent target table instead of replacing
     * it: a target that stays present keeps its nub and its medium, and only
     * its current device page fields are refreshed. */
    bool finished = false;
    const UInt32 token = ++fScanSerial;
    UInt32 bound = fMaxDeviceHandle != 0 ? fMaxDeviceHandle : 0xFFFE;
    for (UInt32 n = 0; n <= bound; n++) {
        UInt16 status = 0xFFFF;
        if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                               MPI2_SAS_DEVICE_PGAD_FORM_GET_NEXT_HANDLE | cursor,
                               page, sizeof(page), &actual, &status)) {
            if (status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE &&
                !fProtocolRecoveryPending) { finished = true; break; }
            return false;
        }
        if (actual < 0x22) return false;
        UInt16 handle = mprReadLe16(page + 0x18);
        if (handle == 0 || handle == 0xFFFF || handle > bound ||
            (cursor != 0xFFFF && handle <= cursor)) return false;
        cursor = handle;
        UInt32 info = mprReadLe32(page + 0x1C);
        UInt16 flags = mprReadLe16(page + 0x20);
        if ((info & 7) == 2 || (info & 7) == 3) expander = true;
        /* SES/vSEP enclosure processors behind an expander are SSP end
         * devices too, but not block disks. Counting them as standalone
         * targets makes the single-target guard refuse the real disk. */
        if ((info & 7) == MPI2_SAS_DEVICE_INFO_END_DEVICE &&
            (info & (MPI2_SAS_DEVICE_INFO_SATA_DEVICE | MPI2_SAS_DEVICE_INFO_STP_TARGET |
                     MPI2_SAS_DEVICE_INFO_SSP_TARGET)) != 0 &&
            (info & (MPI2_SAS_DEVICE_INFO_ATAPI_DEVICE | MPI2_SAS_DEVICE_INFO_SEP)) == 0 &&
            (flags & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) != 0) disks++;
        if ((info & 7) != MPI2_SAS_DEVICE_INFO_END_DEVICE ||
            (info & (MPI2_SAS_DEVICE_INFO_SATA_DEVICE | MPI2_SAS_DEVICE_INFO_STP_TARGET |
                     MPI2_SAS_DEVICE_INFO_SSP_TARGET)) == 0 ||
            (info & (MPI2_SAS_DEVICE_INFO_ATAPI_DEVICE | MPI2_SAS_DEVICE_INFO_SEP)) != 0 ||
            page[0x17] != 0 ||
            (flags & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) == 0 ||
            (flags & (MPI2_SAS_DEVICE0_FLAGS_UNSUPPORTED_DEVICE |
                      MPI2_SAS_DEVICE0_FLAGS_UNAUTHORIZED_DEVICE)) != 0) continue;
        bool raid = false;
        if (!readRaidMembership(handle, &raid)) return false;
        if (raid) {
            setProperty("MPRRaidExcludedHandle", (unsigned long long)handle, 16);
            continue;
        }
        const UInt64 address = mprReadLe64(page + 0x0C);
        /* Identity is the SAS address, not the firmware handle. After a
         * controller reset the IOC can hand the same device a different handle,
         * and two devices can swap handles; matching by handle first would then
         * publish one disk twice and orphan the other's medium. */
        MPRTargetSlot *slot = NULL;
        for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
            if (fTargets[i].handle != 0 && fTargets[i].sasAddress == address) {
                slot = &fTargets[i];
                break;
            }
        }
        if (slot != NULL && slot->scanToken == token) {
            /* The same device is listed twice in one snapshot, which a reset
             * can leave behind. Adopt it once instead of creating a second
             * medium for one disk. */
            setProperty("MPRTopologyDuplicate", "same-sas-address");
            continue;
        }
        if (slot != NULL) {
            if (slot->handle != handle) {
                /* Same disk, new firmware address: rebind in place. Queued I/O
                 * carries the old handle, so it must fail rather than reach
                 * whatever else that handle now names. */
                slot->generation++;
                slot->handle = handle;
                setProperty("MPRTargetReuseState", "handle-moved-same-device");
            }
        } else {
            slot = findTarget(handle);
            if (slot != NULL) {
                /* The handle now names a different device. The old binding is
                 * dropped here; its commands and medium are invalidated before
                 * the new device is probed. */
                if (slot->nub != NULL) slot->offlinePending = true;
                slot->online = false;
                slot->onlinePending = false;
                slot->generation++;
                slot->probeValid = false;
                slot->identityValid = false;
                slot->removalRequested = false;
                setProperty("MPRTargetReuseState", "handle-reassigned");
            } else {
                /* A disk that returns after REMOVE_DEVICE gets a new firmware
                 * handle; reuse the retired slot of the same SAS address when
                 * one exists, so its nub and IOMedia come back instead of
                 * leaking a slot and creating a second medium. */
                MPRTargetSlot *retired = NULL;
                for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
                    MPRTargetSlot *candidate = &fTargets[i];
                    if (candidate->handle == 0 && candidate->nub != NULL &&
                        candidate->sasAddress == address) {
                        retired = candidate;
                        break;
                    }
                }
                if (retired != NULL) {
                    slot = retired;
                    slot->handle = handle;
                    slot->probeValid = false;
                    slot->identityValid = false;
                    slot->generation++;
                    slot->removalRequested = false;
                    slot->onlinePending = false;
                    setTargetRemovalState(slot, "reinserted");
                } else {
                    slot = allocTargetSlot();
                    if (slot == NULL) {
                        setProperty("MPRTopologyFailure", "too-many-targets");
                        return false;
                    }
                    /* A recycled slot can still own the nub of its previous
                     * device; that nub is re-pointed when this target is
                     * published. */
                    MPRBlockDev *reusedNub = slot->nub;
                    memset(slot, 0, sizeof(*slot));
                    slot->nub = reusedNub;
                    slot->handle = handle;
                }
            }
        }
        slot->handle = handle;
        slot->deviceInfo = info;
        slot->flags = flags;
        slot->sasAddress = address;
        slot->fastPath =
            (flags & MPI25_SAS_DEVICE0_FLAGS_ENABLED_FAST_PATH) != 0 &&
            (flags & MPI25_SAS_DEVICE0_FLAGS_FAST_PATH_CAPABLE) != 0;
        slot->scanToken = token;
        if (!slot->present) {
            /* Listed again after being dropped: this is a new medium behind a
             * possibly identical identity, so its generation advances and its
             * probe budget starts over. */
            slot->present = true;
            slot->removalRequested = false;
            slot->probeRetries = 0;
            slot->probeState = kMPRProbeOk;
            if (!slot->probeValid) slot->generation++;
            if (slot->nub != NULL) slot->onlinePending = false;
        }
        candidates++;
        selected = handle;
    }
    if (!finished) return false;

    /* Every handle the firmware stopped listing is a removal candidate. The
     * medium stays invalid until the guarded TARGET_RESET/REMOVE_DEVICE
     * sequence finishes, so queued I/O cannot reach a replacement device. */
    for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
        MPRTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 || slot->scanToken == token || !slot->present)
            continue;
        slot->present = false;
        slot->probeValid = false;
        if (slot->online || slot->nub != NULL) {
            slot->online = false;
            slot->offlinePending = true;
        }
        slot->onlinePending = false;
        requestTargetRemoval(slot);
    }
    fTargetCount = 0;
    for (UInt32 i = 0; i < kMPRMaxTargets; i++)
        if (fTargets[i].handle != 0) fTargetCount++;

    setProperty("MPRStandaloneTargetCount", (unsigned long long)candidates, 32);
    setProperty("MPRTargetCount", (unsigned long long)fTargetCount, 32);
    publishTargetProperties();
    /* Every eligible target stays in the table; publication is per target.
     * The selected handle's own device page is a cross-check, not a gate: one
     * unreadable page must not keep the other targets unpublished. */
    if (selected != 0 && !readSasDevicePage0(selected)) {
        setProperty("MPRSasDevicePageState", "unavailable");
    } else {
        removeProperty("MPRSasDevicePageState");
    }
    fAttachedDevHandle = selected;
    fLinkUpPhys = links;
    fHasExpander = expander;
    fDiscoveredDiskCount = disks;
    if (selected == 0) {
        fFastPathEnabled = false;
        fTargetDeviceInfo = 0;
        fTargetDeviceFlags = 0;
        fTargetSasAddress = 0;
    }
    fIdentityLostCount = 0;
    setProperty("MPRLinkUpPhys", (unsigned long long)links, 8);
    setProperty("MPRAttachedDevHandle", (unsigned long long)selected, 16);
    setProperty("MPRAttachedDeviceInfo", (unsigned long long)fTargetDeviceInfo, 32);
    setProperty("MPRTopologyHasExpander", fHasExpander);
    setProperty("MPRRaidFilterState", "standalone-only");
    removeProperty("MPRTopologyFailure");
    return true;
}

bool
MPRController::scsiInquiryVpd(UInt8 page, UInt8 *buf, UInt32 len,
                              UInt32 *actualBytes)
{
    if (actualBytes != NULL) *actualBytes = 0;
    if (buf == NULL || len < 4 || len > 0xFFFF || fDataBuffer == NULL) return false;
    UInt8 cdb[6] = { 0x12, 1, page, (UInt8)(len >> 8), (UInt8)len, 0 };
    UInt32 actual = 0;
    IOReturn status = kIOReturnIOError;
    if (!issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_READ,
                          NULL, len, 8, &status, &actual, true)) return false;
    if (actual < 4 || actual > len) return false;
    const UInt8 *data = (const UInt8 *)fDataBuffer->getBytesNoCopy();
    if (!mprVpdPayload(data, actual, page, 0, NULL)) return false;
    memset(buf, 0, len);
    memcpy(buf, data, actual);
    if (actualBytes != NULL) *actualBytes = actual;
    return true;
}

static bool
mprHexIdentity(char *out, size_t capacity, const char *prefix,
                 const UInt8 *bytes, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t prefixBytes = strlen(prefix);
    if (length == 0 || prefixBytes + 1 >= capacity ||
        length > (capacity - prefixBytes - 2) / 2) return false;
    bool nonzero = false;
    for (size_t i = 0; i < length; i++) nonzero |= bytes[i] != 0;
    if (!nonzero) return false;
    memcpy(out, prefix, prefixBytes);
    out[prefixBytes++] = ':';
    for (size_t i = 0; i < length; i++) {
        out[prefixBytes++] = hex[bytes[i] >> 4];
        out[prefixBytes++] = hex[bytes[i] & 0x0F];
    }
    out[prefixBytes] = 0;
    return true;
}

bool
MPRController::scsiSataIdentify(UInt8 identify[512])
{
    if ((fTargetDeviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) == 0 ||
        fDataBuffer == NULL || fRequestBuffer == NULL || !beginManagementRequest(5))
        return false;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + 5 * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPRSataPassthroughRequest *request = (MPRSataPassthroughRequest *)frame;
    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SATA_PASSTHROUGH;
    request->PassthroughFlags = MPI2_SATA_PT_REQ_PT_FLAGS_PIO | MPI2_SATA_PT_REQ_PT_FLAGS_READ;
    request->DataLength = 512;
    request->CommandFIS[0] = 0x27;
    request->CommandFIS[1] = 0x80;
    request->CommandFIS[2] = 0xEC;
    request->DataAddress = fDataPhys;
    request->DataBytes = 512;
    request->SgeFlags = 0x40;
    memset(fDataBuffer->getBytesNoCopy(), 0, 512);
    postDefaultRequest(5);
    UInt8 replyBytes[64], kind = 0;
    if (!finishManagementRequest(5, 15000, replyBytes, &kind)) return false;
    const MPRSataPassthroughReply *reply = (const MPRSataPassthroughReply *)replyBytes;
    if (kind != 1 || reply->Function != MPI2_FUNCTION_SATA_PASSTHROUGH ||
        reply->MsgLength * 4U < sizeof(*reply) || reply->DevHandle != fAttachedDevHandle) {
        recordProtocolFault(kMPRTargetMalformedReply, 5);
        return false;
    }
    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
        reply->SASStatus != 0 || (reply->StatusFIS[2] & 1) != 0 ||
        reply->TransferCount != 512) return false;
    memcpy(identify, fDataBuffer->getBytesNoCopy(), 512);
    return mprReadLe16(identify) != 0xFFFF;
}

bool
MPRController::probeStableIdentity(void)
{
    UInt8 page[512];
    UInt32 actual = 0;
    fTargetIdentityValid = false;
    fTargetIdentity[0] = 0;
    if (!allocateResidentBuffers()) return false;
    /* SATA addresses exposed by the IOC can follow the port. Prefer the
     * device's model/serial identity, as the FreeBSD SATA mapping path does. */
    if (scsiSataIdentify(page)) {
        UInt8 key[60];
        bool serial = false;
        char model[41], serialText[21];
        for (UInt32 i = 0; i < 40; i++) {
            key[i] = page[54 + (i ^ 1)];
            model[i] = (char)key[i];
        }
        for (UInt32 i = 0; i < 20; i++) {
            key[40 + i] = page[20 + (i ^ 1)];
            serialText[i] = (char)key[40 + i];
            serial |= key[40 + i] != ' ' && key[40 + i] != 0;
        }
        model[40] = serialText[20] = 0;
        for (UInt32 i = 40; i > 0 && model[i - 1] == ' '; i--) model[i - 1] = 0;
        for (UInt32 i = 20; i > 0 && serialText[i - 1] == ' '; i--) serialText[i - 1] = 0;
        strlcpy(fTargetModel, model, sizeof(fTargetModel));
        strlcpy(fTargetSerial, serialText, sizeof(fTargetSerial));
        if (serial) fTargetIdentityValid = mprHexIdentity(fTargetIdentity,
            sizeof(fTargetIdentity), "ata-model-serial", key, sizeof(key));
        setProperty("MPRSataSolidState", mprReadLe16(page + 434) == 1);
    }
    if (!fTargetIdentityValid && !fProtocolRecoveryPending &&
        scsiInquiryVpd(0x83, page, sizeof(page), &actual)) {
        UInt32 end = 4 + mprReadBe16(page + 2), best = 0, bestRank = 0;
        bool valid = true;
        for (UInt32 offset = 4; offset < end;) {
            if (end - offset < 4 || page[offset + 3] > end - offset - 4) {
                valid = false; break;
            }
            UInt8 type = page[offset + 1] & 0x0F;
            UInt8 association = (page[offset + 1] >> 4) & 3;
            UInt8 codeSet = page[offset] & 0x0F;
            UInt8 length = page[offset + 3];
            UInt32 rank = type == 3 ? 4 : type == 2 ? 3 : type == 8 ? 2 : type == 1 ? 1 : 0;
            if (association == 0 && length != 0 &&
                ((type == 3 && codeSet == 1 && (length == 8 || length == 16)) ||
                 (type == 2 && codeSet == 1 && (length == 8 || length == 12 || length == 16)) ||
                 ((type == 1 || type == 8) && (codeSet == 2 || codeSet == 3))) &&
                rank > bestRank) { best = offset; bestRank = rank; }
            offset += 4 + length;
        }
        if (valid && bestRank != 0) {
            char prefix[16];
            snprintf(prefix, sizeof(prefix), "vpd83-%u", (unsigned)(page[best + 1] & 0x0F));
            fTargetIdentityValid = mprHexIdentity(fTargetIdentity, sizeof(fTargetIdentity),
                                                  prefix, page + best + 4, page[best + 3]);
        }
    }
    if (!fTargetIdentityValid && !fProtocolRecoveryPending &&
        scsiInquiryVpd(0x80, page, sizeof(page), &actual)) {
        UInt32 serialBytes = mprReadBe16(page + 2);
        while (serialBytes != 0 && page[4 + serialBytes - 1] == ' ') serialBytes--;
        bool serialPresent = false;
        for (UInt32 i = 0; i < serialBytes; i++)
            serialPresent |= page[4 + i] != 0 && page[4 + i] != ' ';
        if (serialPresent && serialBytes <= 128) {
            UInt8 key[164] = {};
            memcpy(key, fVendor, 16);
            memcpy(key + 16, fProduct, 20);
            memcpy(key + 36, page + 4, serialBytes);
            fTargetIdentityValid = mprHexIdentity(fTargetIdentity, sizeof(fTargetIdentity),
                                                  "vpd80", key, 36 + serialBytes);
        }
    }
    setProperty("MPRStableIdentityValid", fTargetIdentityValid);
    if (fTargetIdentityValid) setProperty("MPRStableIdentity", fTargetIdentity);
    else removeProperty("MPRStableIdentity");
    return fTargetIdentityValid && !fProtocolRecoveryPending;
}

MPRTargetSlot *
MPRController::findTarget(UInt16 handle)
{
    if (handle == 0) return NULL;
    for (UInt32 i = 0; i < kMPRMaxTargets; i++)
        if (fTargets[i].handle == handle) return &fTargets[i];
    return NULL;
}

bool
MPRController::probeScsiIdentity(void)
{
    if (fProtocolRecoveryPending || !allocatePageBuffer()) return false;
    bool ready = scsiTur();
    setProperty("MPRTurStatus", ready ? "ok" : "failed");
    bool ok = !fProtocolRecoveryPending && scsiInquiry() && probeStableIdentity();
    setProperty("MPRScsiProbeState", ok ? "ok" : "failed");
    return ok;
}

bool
MPRController::checkTargetIdentity(void)
{
    /* Identity comparison is per target: probeTarget() compares the freshly
     * probed identity, capacity and block size with the slot that owns this
     * device and advances that slot's media generation when they differ. */
    if (!fTargetIdentityValid || fCapacityBlocks == 0 || fBlockSize == 0 ||
        fCapacityBlocks > UINT64_MAX / fBlockSize) {
        setProperty("MPRIdentityState", "unverified");
        return false;
    }
    setProperty("MPRIdentityState", "verified");
    return true;
}

bool
MPRController::probeTargetCapabilities(void)
{
    UInt8 limits[256], provisioning[256];
    UInt32 limitBytes = 0, provisioningBytes = 0;
    fUnmapEnabled = false;
    fUnmapLimits = MPRUnmapLimits{};
    if (scsiInquiryVpd(0xB0, limits, sizeof(limits), &limitBytes) &&
        scsiInquiryVpd(0xB2, provisioning, sizeof(provisioning), &provisioningBytes)) {
        fUnmapEnabled = mprParseUnmapLimits(limits, limitBytes, provisioning,
                                           provisioningBytes, &fUnmapLimits);
    }
    fNativeFuaSupported = (fTargetDeviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) != 0 &&
        (fTargetDeviceFlags & MPI2_SAS_DEVICE0_FLAGS_SATA_FUA_SUPPORTED) != 0;
    if ((fTargetDeviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) == 0 &&
        !fProtocolRecoveryPending) {
        UInt8 cdb[10] = {0x5A, 0x08, 0x08, 0, 0, 0, 0, 0, 64, 0};
        IOReturn status = kIOReturnIOError;
        UInt32 actual = 0;
        if (issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_READ,
                             NULL, 64, 8, &status, &actual, true) && actual >= 8) {
            const UInt8 *mode = (const UInt8 *)fDataBuffer->getBytesNoCopy();
            fNativeFuaSupported = (mode[3] & 0x10) != 0;
        }
    }
    setProperty("MPRNativeFuaSupported", fNativeFuaSupported);
    setProperty("MPRFuaPolicy", fNativeFuaSupported ? "native" : "ordered-cache-flush");
    setProperty("MPRUnmapEnabled", fUnmapEnabled);
    setProperty("MPRUnmapMaxLbaCount", (unsigned long long)fUnmapLimits.maxBlocks, 32);
    setProperty("MPRUnmapMaxDescriptors", (unsigned long long)fUnmapLimits.maxDescriptors, 32);
    setProperty("MPRUnmapOptimalGranularity", (unsigned long long)fUnmapLimits.optimalGranularity, 32);
    setProperty("MPRUnmapAlignment", (unsigned long long)fUnmapLimits.alignment, 32);
    setProperty("MPRUnmapAlignmentValid", fUnmapLimits.alignmentValid);
    setProperty("MPRUnmapReadsZero", fUnmapLimits.readsZero);
    return !fProtocolRecoveryPending;
}

bool
MPRController::prepareTargetMedia(void)
{
    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fProtocolRecoveryPending ||
        !allocateResidentBuffers() || !checkTargetIdentity() || !probeTargetCapabilities()) {
        setProperty("MPRMediaReady", false);
        return false;
    }
    UInt32 blocks = fCapacityBlocks < 8 ? (UInt32)fCapacityBlocks : 8;
    if (blocks > fDataBytes / fBlockSize) blocks = fDataBytes / fBlockSize;
    /* One read per probe pass. A device that is still spinning up answers
     * NOT READY or BUSY, and the hotplug worker owns the single per-target
     * retry budget at its one-second cooldown. Retrying here too would create
     * a second budget for the same target and sleep under fIOLock. */
    (void)scsiTur();
    IOReturn status = kIOReturnIOError;
    bool ready = blocks != 0 &&
        scsiReadWrite(false, 0, blocks, fBlockSize, 9, false, false, &status);
    setProperty("MPRMediaReadyStatus", (unsigned long long)(UInt32)status, 32);
    ready = ready && !fProtocolRecoveryPending && fPageBuffer != NULL &&
            fDataBuffer != NULL && fDataBytes != 0;
    setProperty("MPRMediaReady", ready);
    return ready;
}

IOReturn
MPRController::scsiSynchronizeCache(UInt64 lba, UInt64 blocks, UInt32 smid, bool barrier)
{
    UInt8 cdb[16];
    UInt32 length = mprBuildSynchronizeCdb(cdb, lba, blocks);
    if (length == 0) return kIOReturnBadArgument;
    IOReturn status = kIOReturnNotReady;
    if (!issueScsiCommand(cdb, length, MPI2_SCSIIO_CONTROL_NODATATRANSFER,
                          NULL, 0, smid, &status)) return status;
    setProperty("MPRSyncBarrier", barrier);
    return kIOReturnSuccess;
}

IOReturn
MPRController::scsiUnmap(const IOBlockStorageDeviceExtent *extents,
                          UInt32 extentsCount, UInt32 smid)
{
    if (extents == NULL || extentsCount == 0 || extentsCount > kMPRUnmapBatchDescriptors ||
        extentsCount > fUnmapLimits.maxDescriptors) return kIOReturnBadArgument;
    UInt8 cdb[10] = {}, params[8 + 16 * kMPRUnmapBatchDescriptors] = {};
    UInt32 dataLen = 8 + 16 * extentsCount;
    UInt64 totalBlocks = 0;
    cdb[0] = 0x42;
    mprWriteBe16(cdb + 7, (UInt16)dataLen);
    mprWriteBe16(params, (UInt16)(dataLen - 2));
    mprWriteBe16(params + 2, (UInt16)(dataLen - 8));
    for (UInt32 i = 0; i < extentsCount; i++) {
        UInt64 count = extents[i].blockCount;
        if (count == 0 || count > UINT32_MAX ||
            !mprValidBlockExtent(extents[i].blockStart, count, fCapacityBlocks) ||
            count > fUnmapLimits.maxBlocks - totalBlocks) return kIOReturnBadArgument;
        totalBlocks += count;
        mprWriteBe64(params + 8 + 16 * i, extents[i].blockStart);
        mprWriteBe32(params + 16 + 16 * i, (UInt32)count);
    }
    IOReturn status = kIOReturnNotReady;
    if (!issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_WRITE,
                          params, dataLen, smid, &status)) {
        fUnmapEnabled = false;
        setProperty("MPRUnmapEnabled", false);
        setProperty("MPRUnmapDisabled", "command-failed");
        return status;
    }
    return kIOReturnSuccess;
}

IOReturn
MPRController::synchronizeCacheNow(UInt64 block, UInt64 nblks, bool barrier,
                                   UInt32 expectedGeneration, UInt16 handle)
{
    if (fIOLock == NULL) return kIOReturnNotReady;
    IOReturn status = kIOReturnNotReady;
    IOLockLock(fIOLock);
    /* The generation and the capacity are the target's, not the controller's:
     * a flush queued for a departed disk must not be sent to its replacement. */
    MPRTargetSlot *slot = findTarget(handle);
    if (slot == NULL || !slot->present || !slot->probeValid ||
        slot->generation != expectedGeneration || slot->offlinePending ||
        mediaRemovalPending())
        status = kIOReturnNoMedia;
    else if (!fDeviceOffline && !fStopping && !fSuspended && !fReinitInProgress &&
             !fProtocolRecoveryPending) {
        if (block >= slot->capacityBlocks ||
            (nblks != 0 && nblks > slot->capacityBlocks - block))
            status = kIOReturnBadArgument;
        else if (!selectTarget(handle)) status = kIOReturnNoMedia;
        else status = scsiSynchronizeCache(block, nblks, 29, barrier);
        if (slot->generation != expectedGeneration || !slot->present ||
            slot->offlinePending) status = kIOReturnNoMedia;
    }
    IOLockUnlock(fIOLock);
    return status;
}

IOReturn
MPRController::unmapBlocksNow(const IOBlockStorageDeviceExtent *extents, UInt32 extentsCount,
                              UInt32 expectedGeneration, UInt16 handle)
{
    if (extentsCount == 0) return kIOReturnSuccess;
    if (extents == NULL) return kIOReturnBadArgument;
    if (fIOLock == NULL) return kIOReturnNotReady;
    IOReturn status = kIOReturnSuccess;
    IOLockLock(fIOLock);
    MPRTargetSlot *slot = findTarget(handle);
    if (slot == NULL || !slot->present || !slot->probeValid ||
        slot->generation != expectedGeneration || slot->offlinePending ||
        mediaRemovalPending()) {
        status = kIOReturnNoMedia; goto finished;
    }
    if (fDeviceOffline || fStopping || fSuspended || fReinitInProgress ||
        fProtocolRecoveryPending || fDataBuffer == NULL) {
        status = kIOReturnNotReady; goto finished;
    }
    if (!selectTarget(handle)) { status = kIOReturnNoMedia; goto finished; }
    if (!fUnmapEnabled) { status = kIOReturnUnsupported; goto finished; }
    /* Validate every input before issuing the first destructive command. */
    for (UInt32 i = 0; i < extentsCount; i++) {
        if (!mprValidBlockExtent(extents[i].blockStart, extents[i].blockCount,
                                 fCapacityBlocks)) {
            status = kIOReturnBadArgument; goto finished;
        }
    }
    {
        UInt32 index = 0;
        UInt64 consumed = 0;
        while (index < extentsCount) {
            IOBlockStorageDeviceExtent batch[kMPRUnmapBatchDescriptors];
            UInt32 count = 0;
            UInt64 budget = fUnmapLimits.maxBlocks;
            while (index < extentsCount && count < kMPRUnmapBatchDescriptors &&
                   count < fUnmapLimits.maxDescriptors && budget != 0) {
                UInt64 left = extents[index].blockCount - consumed;
                if (left == 0) { index++; consumed = 0; continue; }
                UInt64 take = left < budget ? left : budget;
                if (take > UINT32_MAX) take = UINT32_MAX;
                batch[count].blockStart = extents[index].blockStart + consumed;
                batch[count].blockCount = take;
                count++;
                consumed += take;
                budget -= take;
                if (consumed == extents[index].blockCount) { index++; consumed = 0; }
            }
            if (count == 0) break;
            if (slot->generation != expectedGeneration || !slot->present ||
                slot->offlinePending || mediaRemovalPending()) {
                status = kIOReturnNoMedia; break;
            }
            status = scsiUnmap(batch, count, 30);
            if (status != kIOReturnSuccess) break;
        }
    }
    if (slot->generation != expectedGeneration || !slot->present ||
        slot->offlinePending || mediaRemovalPending())
        status = kIOReturnNoMedia;
finished:
    IOLockUnlock(fIOLock);
    return status;
}

/* ------------------------------------------------------------------------ */
/* per-target table, publication, invalidation and removal.                  */

MPRTargetSlot *
MPRController::allocTargetSlot(void)
{
    /* Prefer a slot that never had a device. A slot whose device is gone is
     * reusable only once its nub has been told the medium is offline, so the
     * storage layer never sees two devices under one medium without a
     * transition. */
    for (UInt32 i = 0; i < kMPRMaxTargets; i++)
        if (fTargets[i].handle == 0 && fTargets[i].nub == NULL) return &fTargets[i];
    for (UInt32 i = 0; i < kMPRMaxTargets; i++)
        if (fTargets[i].handle == 0 && fTargets[i].nub != NULL &&
            !fTargets[i].offlinePending) return &fTargets[i];
    return NULL;
}

bool
MPRController::selectTarget(UInt16 handle)
{
    /* The management and synchronous-command chain is single-threaded under
     * fIOLock and therefore bound to one handle at a time. Selecting a target
     * copies its policy into the controller fields those helpers read; it does
     * not change the target table. */
    MPRTargetSlot *slot = findTarget(handle);
    if (slot == NULL || !slot->present || !slot->probeValid) return false;
    fAttachedDevHandle = handle;
    fCapacityBlocks = slot->capacityBlocks;
    fBlockSize = slot->blockSize;
    fTargetDeviceInfo = slot->deviceInfo;
    fTargetDeviceFlags = slot->flags;
    fTargetSasAddress = slot->sasAddress;
    fFastPathEnabled = slot->fastPath;
    fUnmapEnabled = slot->unmapEnabled;
    fUnmapLimits = slot->unmapLimits;
    fNativeFuaSupported = slot->nativeFua;
    fTargetIdentityValid = slot->identityValid;
    strlcpy(fTargetIdentity, slot->identity, sizeof(fTargetIdentity));
    strlcpy(fTargetModel, slot->model, sizeof(fTargetModel));
    strlcpy(fTargetSerial, slot->serial, sizeof(fTargetSerial));
    memcpy(fVendor, slot->vendor, sizeof(fVendor));
    memcpy(fProduct, slot->product, sizeof(fProduct));
    memcpy(fRevision, slot->revision, sizeof(fRevision));
    return true;
}

void
MPRController::publishTargetProperties(void)
{
    for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
        MPRTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 && slot->nub == NULL) continue;
        char name[48], address[24];
        const char *removal = slot->removalState[0] != 0 ? slot->removalState :
                              slot->removalRequested ? "requested" :
                              slot->present ? "live" : "gone";
        snprintf(name, sizeof(name), "MPRTarget%uHandle", (unsigned)i);
        setProperty(name, (unsigned long long)slot->handle, 16);
        snprintf(name, sizeof(name), "MPRTarget%uDeviceInfo", (unsigned)i);
        setProperty(name, (unsigned long long)slot->deviceInfo, 32);
        snprintf(name, sizeof(name), "MPRTarget%uSasAddress", (unsigned)i);
        snprintf(address, sizeof(address), "%016llx",
                 (unsigned long long)slot->sasAddress);
        setProperty(name, address);
        snprintf(name, sizeof(name), "MPRTarget%uPresent", (unsigned)i);
        setProperty(name, slot->present);
        snprintf(name, sizeof(name), "MPRTarget%uProbed", (unsigned)i);
        setProperty(name, slot->probeValid);
        snprintf(name, sizeof(name), "MPRTarget%uProbeState", (unsigned)i);
        setProperty(name, slot->probeState == kMPRProbeOk ? "ok" :
                          slot->probeState == kMPRProbeNotReady ?
                          "not-ready" : "failed");
        snprintf(name, sizeof(name), "MPRTarget%uProbeRetries", (unsigned)i);
        setProperty(name, (unsigned long long)slot->probeRetries, 32);
        snprintf(name, sizeof(name), "MPRTarget%uOnline", (unsigned)i);
        setProperty(name, slot->online);
        snprintf(name, sizeof(name), "MPRTarget%uBlocks", (unsigned)i);
        setProperty(name, (unsigned long long)slot->capacityBlocks, 64);
        snprintf(name, sizeof(name), "MPRTarget%uBlockSize", (unsigned)i);
        setProperty(name, (unsigned long long)slot->blockSize, 32);
        snprintf(name, sizeof(name), "MPRTarget%uIdentity", (unsigned)i);
        setProperty(name, slot->identityValid ? slot->identity : "unknown");
        snprintf(name, sizeof(name), "MPRTarget%uModel", (unsigned)i);
        setProperty(name, slot->model[0] != 0 ? slot->model : "unknown");
        snprintf(name, sizeof(name), "MPRTarget%uSerial", (unsigned)i);
        setProperty(name, slot->serial[0] != 0 ? slot->serial : "unknown");
        snprintf(name, sizeof(name), "MPRTarget%uRemovalState", (unsigned)i);
        setProperty(name, removal);
        snprintf(name, sizeof(name), "MPRTarget%uGeneration", (unsigned)i);
        setProperty(name, (unsigned long long)slot->generation, 32);
        snprintf(name, sizeof(name), "MPRTarget%uFailureCount", (unsigned)i);
        setProperty(name, (unsigned long long)slot->failureCount, 32);
        snprintf(name, sizeof(name), "MPRTarget%uWriteCount", (unsigned)i);
        setProperty(name, (unsigned long long)slot->writeCount, 32);
    }
}

void
MPRController::publishTargetCounters(void)
{
    setProperty("MPRScanCount", (unsigned long long)fScanSerial, 32);
    setProperty("MPRRemovedMediaCount", (unsigned long long)fRemovedMediaCount, 32);
    setProperty("MPRRemovalFailures", (unsigned long long)fRemovalFailures, 32);
    setProperty("MPRFaultRecoveries", (unsigned long long)fFaultRecoveries, 32);
    setProperty("MPRRediscoveryResets", (unsigned long long)fRediscoveryResets, 32);
    /* Called from the watchdog: the counters move without a topology event, so
     * they are republished periodically instead of only on publication. */
    UInt32 probeRetries = 0;
    for (UInt32 i = 0; i < kMPRMaxTargets; i++)
        probeRetries += fTargets[i].probeRetries;
    setProperty("MPRProbeRetries", (unsigned long long)probeRetries, 32);
    for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
        MPRTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 && slot->nub == NULL) continue;
        char name[48];
        snprintf(name, sizeof(name), "MPRTarget%uWriteCount", (unsigned)i);
        setProperty(name, (unsigned long long)slot->writeCount, 32);
        snprintf(name, sizeof(name), "MPRTarget%uFailureCount", (unsigned)i);
        setProperty(name, (unsigned long long)slot->failureCount, 32);
        snprintf(name, sizeof(name), "MPRTarget%uProbeState", (unsigned)i);
        setProperty(name, slot->probeState == kMPRProbeOk ? "ok" :
                          slot->probeState == kMPRProbeNotReady ?
                          "not-ready" : "failed");
        snprintf(name, sizeof(name), "MPRTarget%uProbeRetries", (unsigned)i);
        setProperty(name, (unsigned long long)slot->probeRetries, 32);
        snprintf(name, sizeof(name), "MPRTarget%uOnline", (unsigned)i);
        setProperty(name, slot->online);
    }
}

void
MPRController::setTargetRemovalState(MPRTargetSlot *slot, const char *state)
{
    if (slot == NULL || state == NULL) return;
    strlcpy(slot->removalState, state, sizeof(slot->removalState));
}

void
MPRController::requestTargetRemoval(MPRTargetSlot *slot)
{
    if (slot == NULL || slot->handle == 0 || slot->removalRequested) return;
    slot->removalRequested = true;
    setTargetRemovalState(slot, "requested");
    publishTargetProperties();
}

void
MPRController::invalidateTarget(MPRTargetSlot *slot)
{
    if (slot == NULL) return;
    slot->probeValid = false;
    slot->generation++;
    slot->onlinePending = false;
    if (slot->nub != NULL) {
        slot->online = false;
        slot->offlinePending = true;
    }
}

void
MPRController::invalidateTargets(void)
{
    /* A controller-wide event (IOC reset, protocol fault) unbinds every
     * medium. Transport failure and publication state are separate: setting
     * DeviceOffline must never suppress the later OFFLINE notification. */
    for (UInt32 i = 0; i < kMPRMaxTargets; i++) invalidateTarget(&fTargets[i]);
    fMediaPresent = false;
    fDeviceOffline = true;
    setProperty("MPRDeviceOffline", true);
    setProperty("MPRMediaPresent", false);
    setProperty("MPRMediaReady", false);
    publishTargetProperties();
}

UInt32
MPRController::liveTargetCount(void)
{
    UInt32 live = 0;
    for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
        MPRTargetSlot *slot = &fTargets[i];
        if (slot->handle != 0 && slot->present && slot->probeValid &&
            slot->nub != NULL) live++;
    }
    return live;
}

MPRProbeResult
MPRController::probeTarget(MPRTargetSlot *slot)
{
    if (slot == NULL || slot->handle == 0 || !slot->present)
        return kMPRProbeFailed;
    /* The shared probe chain is read-only (TUR, INQUIRY, VPD, READ CAPACITY,
     * MODE SENSE, READ) and serialised by fIOLock. Its controller-level state
     * belongs to the slot being probed and is copied back before the next
     * target uses the chain. */
    fAttachedDevHandle = slot->handle;
    fTargetIdentityValid = false;
    fTargetIdentity[0] = 0;
    fTargetModel[0] = 0;
    fTargetSerial[0] = 0;
    fCapacityBlocks = 0;
    fBlockSize = 0;
    fUnmapEnabled = false;
    fUnmapLimits = MPRUnmapLimits{};
    fNativeFuaSupported = false;
    /* The topology enumeration already captured this target's device page
     * fields. Reuse them instead of re-reading config pages while the start
     * path still owns the shared management SMID. */
    fTargetDeviceInfo = slot->deviceInfo;
    fTargetDeviceFlags = slot->flags;
    fTargetSasAddress = slot->sasAddress;
    fFastPathEnabled = slot->fastPath;
    UInt32 index = (UInt32)(slot - fTargets);
    char name[48];
    const char *stage = NULL;
    IOReturn status = kIOReturnSuccess;
    /* Each stage starts with a clean classification: an expected fallback
     * within one stage (RC16 rejected by a SATA bridge, then RC10) must not
     * decide the outcome of a later stage. */
    fProbeInProgress = true;
    beginProbeStage();
    if (!probeScsiIdentity()) {
        stage = "scsi-identity";
    } else {
        beginProbeStage();
        if (!probeCapacity()) {
            stage = "capacity";
        } else {
            beginProbeStage();
            if (!probeBlockRead()) {
                stage = "block-read";
            } else {
                beginProbeStage();
                if (!prepareTargetMedia()) stage = "media";
            }
        }
    }
    status = fLastProbeStatus;
    fProbeInProgress = false;
    if (stage != NULL) {
        /* Only a decoded NOT READY / UNIT ATTENTION / BUSY means the device is
         * on the fabric but still becoming ready. A chain that failed without
         * any decoded status (timeout, missing buffer, malformed reply) or
         * with a real SCSI error is a definite failure. */
        MPRProbeResult result =
            (status == kIOReturnNotReady || status == kIOReturnBusy)
                ? kMPRProbeNotReady : kMPRProbeFailed;
        snprintf(name, sizeof(name), "MPRTarget%uProbe", index);
        setProperty(name, result == kMPRProbeNotReady ? "not-ready" : "failed");
        snprintf(name, sizeof(name), "MPRTarget%uProbeStage", index);
        setProperty(name, stage);
        snprintf(name, sizeof(name), "MPRTarget%uProbeStatus", index);
        setProperty(name, (unsigned long long)(UInt32)status, 32);
        return result;
    }
    bool changed = slot->probeValid &&
        (strcmp(slot->identity, fTargetIdentity) != 0 ||
         slot->capacityBlocks != fCapacityBlocks ||
         slot->blockSize != fBlockSize);
    slot->capacityBlocks = fCapacityBlocks;
    slot->blockSize = fBlockSize;
    slot->identityValid = fTargetIdentityValid;
    memcpy(slot->vendor, fVendor, sizeof(slot->vendor));
    memcpy(slot->product, fProduct, sizeof(slot->product));
    memcpy(slot->revision, fRevision, sizeof(slot->revision));
    strlcpy(slot->identity, fTargetIdentity, sizeof(slot->identity));
    /* A SATA disk reports an ATA model; an SSP disk behind an expander only
     * has its INQUIRY product string. */
    strlcpy(slot->model, fTargetModel[0] != 0 ? fTargetModel : fProduct,
            sizeof(slot->model));
    strlcpy(slot->serial, fTargetSerial, sizeof(slot->serial));
    slot->unmapEnabled = fUnmapEnabled;
    slot->unmapLimits = fUnmapLimits;
    slot->nativeFua = fNativeFuaSupported;
    if (!slot->probeValid || changed) {
        /* A republished medium gets a new generation: I/O accepted for the old
         * one must never reach this device, even when its identity matches. */
        slot->generation++;
        slot->onlinePending = false;
        if (slot->nub != NULL) {
            if (slot->online) {
                slot->online = false;
                slot->offlinePending = true;
            }
        }
    }
    slot->probeValid = true;
    snprintf(name, sizeof(name), "MPRTarget%uProbe", index);
    setProperty(name, "ok");
    snprintf(name, sizeof(name), "MPRTarget%uProbeStatus", index);
    setProperty(name, 0ULL, 32);
    return kMPRProbeOk;
}

bool
MPRController::publishTarget(MPRTargetSlot *slot)
{
    if (slot == NULL || slot->handle == 0 || !slot->present || !slot->probeValid ||
        slot->blockSize == 0 || slot->capacityBlocks == 0 ||
        fIOLock == NULL || fPageBuffer == NULL || fDataBuffer == NULL ||
        fPagePhys == 0 || fDataPhys == 0 || fDataBytes == 0) return false;
    if (slot->nub != NULL) {
        /* The nub stayed attached while the medium was offline. Re-point it at
         * this target's current handle and geometry, and let the unlocked
         * worker deliver OFFLINE -> ONLINE around the transition. */
        slot->nub->setController(this, slot->handle);
        slot->onlinePending = true;
        setTargetRemovalState(slot, "live");
        publishTargetProperties();
        return true;
    }
    MPRBlockDev *nub = new MPRBlockDev;
    if (nub == NULL) return false;
    if (!nub->init(NULL)) {
        nub->release();
        return false;
    }
    nub->setController(this, slot->handle);
    if (!nub->attach(this)) {
        nub->release();
        return false;
    }
    if (!nub->start(this)) {
        nub->detach(this);
        nub->release();
        return false;
    }
    slot->nub = nub;
    /* Matching an IOBlockStorageDriver against this device reads the media
     * state directly, so its first publication needs no notification. */
    slot->online = true;
    slot->offlinePending = false;
    slot->onlinePending = false;
    setTargetRemovalState(slot, "live");
    nub->registerService();
    publishTargetProperties();
    return true;
}

bool
MPRController::ensureTargetsPublished(void)
{
    UInt32 published = 0;
    bool allOk = true;
    for (UInt32 i = 0; i < kMPRMaxTargets; i++) {
        MPRTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 || !slot->present) continue;
        if (!slot->probeValid) {
            MPRProbeResult result = probeTarget(slot);
            recordProbeResult(slot, result);
            if (result != kMPRProbeOk) {
                /* A still-spinning disk is not a failed target; the hotplug
                 * worker owns its remaining probes. Only a definite failure
                 * counts against this device. */
                if (result == kMPRProbeFailed) slot->failureCount++;
                allOk = false;
                continue;
            }
        }
        if (!publishTarget(slot)) {
            slot->failureCount++;
            allOk = false;
            continue;
        }
        published++;
    }
    /* Point the controller's default handle at the first published target;
     * the legacy synchronous helpers use it. Per-request routing never
     * depends on this default. */
    UInt16 defaultHandle = 0;
    for (UInt32 i = 0; i < kMPRMaxTargets && defaultHandle == 0; i++)
        if (fTargets[i].nub != NULL) defaultHandle = fTargets[i].handle;
    fAttachedDevHandle = defaultHandle;
    if (defaultHandle != 0) {
        MPRTargetSlot *slot = findTarget(defaultHandle);
        if (slot != NULL) {
            fCapacityBlocks = slot->capacityBlocks;
            fBlockSize = slot->blockSize;
            setProperty("MPRMediaBlocks", slot->capacityBlocks, 64);
            setProperty("MPRMediaBlockSize", (unsigned long long)slot->blockSize, 32);
        }
    }
    fMediaPresent = published != 0;
    setProperty("MPRMediaPresent", fMediaPresent);
    setProperty("MPRPublishedTargetCount", published, 32);
    publishTargetProperties();
    return allOk && published != 0;
}

bool
MPRController::targetDevicePresent(UInt16 handle, bool *present, UInt64 *sasAddress)
{
    /* true: the firmware answered (the device is present or absent);
     * false: the page could not be read, so its state is unknown. */
    if (present != NULL) *present = false;
    if (sasAddress != NULL) *sasAddress = 0;
    UInt8 page[512];
    UInt32 actual = 0;
    UInt16 status = 0xFFFF;
    bool found = readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
        MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle, page, sizeof(page), &actual, &status);
    if (!found) return status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE;
    if (actual < 0x22 || mprReadLe16(page + 0x18) != handle) return false;
    if ((mprReadLe16(page + 0x20) & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) == 0)
        return true;
    if (present != NULL) *present = true;
    if (sasAddress != NULL) *sasAddress = mprReadLe64(page + 0x0C);
    return true;
}

bool
MPRController::removeTargetDevice(UInt16 handle)
{
    if (fRequestBuffer == NULL || handle == 0 || fProtocolRecoveryPending) return false;
    if (!beginManagementRequest(4)) return false;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + 4 * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPRSasIoUnitControlRequest *request = (MPRSasIoUnitControlRequest *)frame;
    request->Function = MPI2_FUNCTION_SAS_IO_UNIT_CONTROL;
    request->Operation = MPI2_SAS_OP_REMOVE_DEVICE;
    request->DevHandle = handle;
    postDefaultRequest(4);
    UInt8 replyBytes[64], kind = 0;
    if (!finishManagementRequest(4, 2000, replyBytes, &kind)) return false;
    const MPRSasIoUnitControlReply *reply = (const MPRSasIoUnitControlReply *)replyBytes;
    if (kind != 1 || reply->Function != MPI2_FUNCTION_SAS_IO_UNIT_CONTROL ||
        reply->MsgLength * 4U < sizeof(*reply) || reply->DevHandle != handle ||
        reply->Operation != MPI2_SAS_OP_REMOVE_DEVICE) {
        recordProtocolFault(kMPRTargetMalformedReply, 4);
        return false;
    }
    return (reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPRController::retireTarget(MPRTargetSlot *slot)
{
    /* Firmware expects TARGET_RESET (which aborts the commands still owned by
     * the device) followed by SAS IO Unit Control REMOVE_DEVICE. Both name the
     * removed handle explicitly: the controller's current default may point at
     * a different, healthy target.
     *
     * Presence decides the outcome. A handle that still names this device is the
     * disk coming back; a device that has left the fabric has nothing left to
     * reset, and the medium is unbound locally even if REMOVE_DEVICE cannot be
     * sent. The only result that keeps the medium bound is "commands may still
     * be in flight", which the caller escalates to a bounded IOC reset. */
    if (slot == NULL || slot->handle == 0 || !slot->removalRequested) return true;
    const UInt16 handle = slot->handle;
    bool present = false;
    UInt64 address = 0;
    bool known = targetDevicePresent(handle, &present, &address);
    if (known && present) {
        slot->removalRequested = false;
        if (address == slot->sasAddress) {
            slot->present = true;
            setTargetRemovalState(slot, "reinserted");
        } else {
            /* Reused by a different device: the scan already adopted it, so
             * the new target must not receive the old removal. */
            setTargetRemovalState(slot, "handle-reassigned");
        }
        publishTargetProperties();
        return true;
    }
    if (fProtocolRecoveryPending) {
        /* The controller is already being repaired; a reset cannot be issued
         * from here without a second owner of the same fault. */
        setTargetRemovalState(slot, "recovery-pending");
        publishTargetProperties();
        return false;
    }
    UInt16 iocStatus = 0xFFFF;
    (void)sendTaskMgmt(MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET, 0, 28, handle,
                       &iocStatus);
    setProperty("MPRTargetResetStatus", (unsigned long long)iocStatus, 16);
    if (iocStatus == 0xFFFF) {
        /* No completion at all: the firmware may still own the request frame. */
        setTargetRemovalState(slot, "reset-reply-timeout");
        publishTargetProperties();
        return false;
    }
    /* A reset that answered with a non-success status usually means the device
     * is already gone; the drain below is the evidence that matters. The TMF
     * reply consumes its own SMID but not the SCSI replies it aborted. */
    if (!drainTaskManagedCommands(handle, 0, true) || fProtocolRecoveryPending) {
        setTargetRemovalState(slot, "commands-still-owned");
        publishTargetProperties();
        return false;
    }
    known = targetDevicePresent(handle, &present, &address);
    if (known && present) {
        slot->removalRequested = false;
        if (address == slot->sasAddress) {
            slot->present = true;
            setTargetRemovalState(slot, "reinserted-during-reset");
        } else {
            setTargetRemovalState(slot, "handle-reassigned-during-reset");
        }
        publishTargetProperties();
        return true;
    }
    /* The device is gone from the fabric. A failed or unanswered REMOVE_DEVICE
     * is not a reason to keep the medium bound: the slot is detached locally,
     * and a disk that was only reported absent is rediscovered by the next
     * scan and published again. */
    const bool removed = removeTargetDevice(handle);
    slot->removalRequested = false;
    slot->present = false;
    slot->probeValid = false;
    slot->identityValid = false;
    slot->generation++;
    slot->onlinePending = false;
    if (slot->nub != NULL) {
        slot->online = false;
        slot->offlinePending = true;
    }
    slot->handle = 0;   /* free the firmware address; the nub stays offline */
    setTargetRemovalState(slot, removed ? "removed" : "removed-unconfirmed");
    publishTargetProperties();
    return true;
}
