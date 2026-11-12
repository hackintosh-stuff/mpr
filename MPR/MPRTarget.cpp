#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <string.h>
#include "MPRController.h"
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
     * IOC PHY handles alone can name the expander, not its attached disk. */
    bool finished = false;
    fTargetCount = 0;
    memset(fTargets, 0, sizeof(fTargets));
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
        if (fTargetCount < kMPRMaxTargets) {
            MPRTargetSlot *slot = &fTargets[fTargetCount++];
            slot->handle = handle;
            slot->deviceInfo = info;
            slot->flags = flags;
            slot->sasAddress = mprReadLe64(page + 0x0C);
            slot->fastPath =
                (flags & MPI25_SAS_DEVICE0_FLAGS_ENABLED_FAST_PATH) != 0 &&
                (flags & MPI25_SAS_DEVICE0_FLAGS_FAST_PATH_CAPABLE) != 0;
            slot->present = true;
            slot->readOnly = true; /* fail-closed until a write rule matches */
        }
        candidates++;
        selected = handle;
    }
    if (!finished) return false;
    setProperty("MPRStandaloneTargetCount", (unsigned long long)candidates, 32);
    setProperty("MPRTargetCount", (unsigned long long)fTargetCount, 32);
    for (UInt32 i = 0; i < fTargetCount; i++) {
        char name[40], address[24];
        snprintf(name, sizeof(name), "MPRTarget%uHandle", (unsigned)i);
        setProperty(name, (unsigned long long)fTargets[i].handle, 16);
        snprintf(name, sizeof(name), "MPRTarget%uDeviceInfo", (unsigned)i);
        setProperty(name, (unsigned long long)fTargets[i].deviceInfo, 32);
        snprintf(name, sizeof(name), "MPRTarget%uSasAddress", (unsigned)i);
        snprintf(address, sizeof(address), "%016llx",
                 (unsigned long long)fTargets[i].sasAddress);
        setProperty(name, address);
    }
    /* Every eligible target stays in the table; publication is per target.
     * An unmatched identity is read-only, so a ZFS member is never writable. */
    if (fTargetCount > kMPRMaxTargets) {
        setProperty("MPRTopologyFailure", "too-many-targets");
        return false;
    }
    if (selected != 0 && !readSasDevicePage0(selected)) return false;
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

bool
MPRController::loadWritePolicy(void)
{
    fWriteRuleCount = 0;
    fTargetModel[0] = 0;
    fTargetSerial[0] = 0;
    fTargetReadOnly = true;
    for (UInt32 i = 0; i < kMPRMaxWriteRules; i++) {
        fWriteRuleModel[i][0] = 0;
        fWriteRuleSerial[i][0] = 0;
    }
    OSArray *rules = OSDynamicCast(OSArray, getProperty("MPRWritableDevices"));
    if (rules != NULL) {
        for (UInt32 i = 0;
             i < rules->getCount() && fWriteRuleCount < kMPRMaxWriteRules; i++) {
            OSDictionary *rule = OSDynamicCast(OSDictionary, rules->getObject(i));
            if (rule == NULL) continue;
            OSString *model = OSDynamicCast(OSString, rule->getObject("Model"));
            OSString *serial = OSDynamicCast(OSString, rule->getObject("Serial"));
            if (model == NULL || serial == NULL) continue;
            strlcpy(fWriteRuleModel[fWriteRuleCount], model->getCStringNoCopy(),
                    sizeof(fWriteRuleModel[0]));
            strlcpy(fWriteRuleSerial[fWriteRuleCount], serial->getCStringNoCopy(),
                    sizeof(fWriteRuleSerial[0]));
            fWriteRuleCount++;
        }
    }
    setProperty("MPRWriteRuleCount", (unsigned long long)fWriteRuleCount, 8);
    return true;
}

bool
MPRController::isWritableIdentity(const char *model, const char *serial) const
{
    /* An unidentified target is never writable. A wildcard serial would make
     * every disk writable, so only concrete serial rules can grant access. */
    if (model == NULL || serial == NULL || serial[0] == 0) return false;
    for (UInt32 i = 0; i < fWriteRuleCount; i++) {
        if (fWriteRuleSerial[i][0] == 0 ||
            strcmp(fWriteRuleSerial[i], "*") == 0) continue;
        bool modelMatch = strcmp(fWriteRuleModel[i], "*") == 0 ||
                          strcmp(fWriteRuleModel[i], model) == 0;
        if (modelMatch && strcmp(fWriteRuleSerial[i], serial) == 0) return true;
    }
    return false;
}

MPRTargetSlot *
MPRController::findTarget(UInt16 handle)
{
    if (handle == 0) return NULL;
    for (UInt32 i = 0; i < fTargetCount && i < kMPRMaxTargets; i++)
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
    if (!fTargetIdentityValid || fCapacityBlocks == 0 || fBlockSize == 0 ||
        fCapacityBlocks > UINT64_MAX / fBlockSize) return false;
    if (fRequireSameIdentity &&
        (!fMediaIdentityValid || strcmp(fMediaIdentity, fTargetIdentity) != 0 ||
         fIdentityCapacity != fCapacityBlocks || fIdentityBlockSize != fBlockSize)) {
        setProperty("MPRIdentityState", "recovery-target-changed");
        return false;
    }
    strlcpy(fMediaIdentity, fTargetIdentity, sizeof(fMediaIdentity));
    fMediaIdentityValid = true;
    fIdentityCapacity = fCapacityBlocks;
    fIdentityBlockSize = fBlockSize;
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
    bool ready = false;
    for (UInt32 attempt = 0; attempt < 5 && blocks != 0; attempt++) {
        if (fStopping || fProtocolRecoveryPending) break;
        (void)scsiTur();
        IOReturn status = kIOReturnNotReady;
        if (scsiReadWrite(false, 0, blocks, fBlockSize, 9, false, false, &status)) {
            ready = true; break;
        }
        if (status != kIOReturnNotReady && status != kIOReturnBusy) break;
        IOSleep(100);
    }
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
                                   UInt32 expectedGeneration)
{
    if (fIOLock == NULL) return kIOReturnNotReady;
    UInt32 generation = expectedGeneration;
    IOReturn status = kIOReturnNotReady;
    IOLockLock(fIOLock);
    if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending())
        status = kIOReturnNoMedia;
    else if (!fDeviceOffline && !fStopping && !fSuspended && !fReinitInProgress &&
             !fProtocolRecoveryPending && fCapacityBlocks != 0) {
        if (block >= fCapacityBlocks ||
            (nblks != 0 && nblks > fCapacityBlocks - block)) status = kIOReturnBadArgument;
        else status = scsiSynchronizeCache(block, nblks, 29, barrier);
        if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending())
            status = kIOReturnNoMedia;
    }
    IOLockUnlock(fIOLock);
    return status;
}

IOReturn
MPRController::unmapBlocksNow(const IOBlockStorageDeviceExtent *extents, UInt32 extentsCount,
                              UInt32 expectedGeneration)
{
    if (extentsCount == 0) return kIOReturnSuccess;
    if (extents == NULL) return kIOReturnBadArgument;
    if (fIOLock == NULL) return kIOReturnNotReady;
    UInt32 generation = expectedGeneration;
    IOReturn status = kIOReturnSuccess;
    IOLockLock(fIOLock);
    if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending()) {
        status = kIOReturnNoMedia; goto finished;
    }
    if (fDeviceOffline || fStopping || fSuspended || fReinitInProgress ||
        fProtocolRecoveryPending || fDataBuffer == NULL || fCapacityBlocks == 0) {
        status = kIOReturnNotReady; goto finished;
    }
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
            if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending()) {
                status = kIOReturnNoMedia; break;
            }
            status = scsiUnmap(batch, count, 30);
            if (status != kIOReturnSuccess) break;
        }
    }
    if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending())
        status = kIOReturnNoMedia;
finished:
    IOLockUnlock(fIOLock);
    return status;
}

bool
MPRController::retireRemovedTarget(void)
{
    UInt16 handle;
    UInt32 generation;
    IOSimpleLockLock(fReplyLock);
    bool requested = fTargetRemovalRequested;
    handle = fRemovedTargetHandle;
    generation = fRemovalGeneration;
    IOSimpleLockUnlock(fReplyLock);
    if (!requested || handle == 0) return true;
    if (fProtocolRecoveryPending || fMediaPresent || fAttachedDevHandle != handle) return false;
    UInt8 page[512];
    UInt32 actual = 0;
    UInt16 configStatus = 0xFFFF;
    bool found = readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
        MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle, page, sizeof(page), &actual, &configStatus);
    if (found && (actual < 0x22 || mprReadLe16(page + 0x18) != handle)) return false;
    if (found &&
        (mprReadLe16(page + 0x20) & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) != 0) {
        /* The handle is live again: its removal belongs to an older insertion. */
        IOSimpleLockLock(fReplyLock);
        if (generation == fRemovalGeneration) fTargetRemovalRequested = false;
        IOSimpleLockUnlock(fReplyLock);
        setProperty("MPRTargetRemovalState", "live-handle-preserved");
        return true;
    }
    if (!found && configStatus != MPI2_IOCSTATUS_CONFIG_INVALID_PAGE) return false;
    IOSimpleLockLock(fReplyLock);
    bool current = fTargetRemovalRequested && generation == fRemovalGeneration &&
                   generation == fTopologyGeneration;
    IOSimpleLockUnlock(fReplyLock);
    if (!current) return true;
    if (!sendTaskMgmt(MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET, 0, 28)) return false;
    bool drained = false;
    for (UInt32 wait = 0; wait < 100 && !drained; wait++) {
        processReplyQueue();
        drained = true;
        for (UInt32 smid = 10; smid <= 30; smid++) {
            if (smid >= 26 && smid <= 28) continue;
            if (!commandDmaSafe(smid)) { drained = false; break; }
        }
        if (!drained) IOSleep(10);
    }
    if (!drained || fProtocolRecoveryPending) return false;
    found = readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
        MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle, page, sizeof(page), &actual, &configStatus);
    if (found && (actual < 0x22 || mprReadLe16(page + 0x18) != handle)) return false;
    if (found && (mprReadLe16(page + 0x20) & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) != 0) {
        setProperty("MPRTargetRemovalState", "reinsert-during-reset-preserved");
        return true;
    }
    if (!found && configStatus != MPI2_IOCSTATUS_CONFIG_INVALID_PAGE) return false;
    /* A reinsert observed while TARGET_RESET was pending cancels the remove. */
    IOSimpleLockLock(fReplyLock);
    current = fTargetRemovalRequested && generation == fRemovalGeneration &&
              generation == fTopologyGeneration;
    IOSimpleLockUnlock(fReplyLock);
    if (!current) return true;
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
    bool ok = (reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS;
    if (ok) {
        IOSimpleLockLock(fReplyLock);
        if (generation == fRemovalGeneration) fTargetRemovalRequested = false;
        IOSimpleLockUnlock(fReplyLock);
    }
    setProperty("MPRTargetRemovalState", ok ? "removed" : "remove-failed");
    return ok;
}
