/*-
 *  Copyright 2000-2020 Broadcom Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Broadcom Inc. (LSI) MPT-Fusion Host Adapter FreeBSD
 */

#ifndef MPR_MPI26_DEVICES_H
#define MPR_MPI26_DEVICES_H
/* Wire constants from reference/freebsd/sys/dev/mpr/mpi (Broadcom BSD).
 * Keep these exact values; do not infer support from a numerical ID range. */
#ifndef MPI2_IOCFACTS_PROTOCOL_NVME_DEVICES
#define MPI2_IOCFACTS_PROTOCOL_NVME_DEVICES (0x00000008U)
#endif
#ifndef MPI26_IOCFACTS_CAPABILITY_ATOMIC_REQ
#define MPI26_IOCFACTS_CAPABILITY_ATOMIC_REQ (0x00080000U)
#endif
#ifndef MPI2_CONFIG_PAGETYPE_RAID_VOLUME
#define MPI2_CONFIG_PAGETYPE_RAID_VOLUME (0x00000008U)
#endif
#ifndef MPI2_CONFIG_EXTPAGETYPE_PCIE_DEVICE
#define MPI2_CONFIG_EXTPAGETYPE_PCIE_DEVICE (0x0000001dU)
#endif
#ifndef MPI2_RAID_VOLUME_PGAD_FORM_GET_NEXT_HANDLE
#define MPI2_RAID_VOLUME_PGAD_FORM_GET_NEXT_HANDLE (0x00000000U)
#endif
#ifndef MPI2_RAID_VOLUME_PGAD_FORM_HANDLE
#define MPI2_RAID_VOLUME_PGAD_FORM_HANDLE (0x10000000U)
#endif
#ifndef MPI26_PCIE_DEVICE_PGAD_FORM_GET_NEXT_HANDLE
#define MPI26_PCIE_DEVICE_PGAD_FORM_GET_NEXT_HANDLE (0x00000000U)
#endif
#ifndef MPI26_PCIE_DEVICE_PGAD_FORM_HANDLE
#define MPI26_PCIE_DEVICE_PGAD_FORM_HANDLE (0x20000000U)
#endif
#ifndef MPI26_PCIE_DEVINFO_MASK_DEVICE_TYPE
#define MPI26_PCIE_DEVINFO_MASK_DEVICE_TYPE (0x0000000fU)
#endif
#ifndef MPI26_PCIE_DEVINFO_NVME
#define MPI26_PCIE_DEVINFO_NVME (0x00000003U)
#endif
#ifndef MPI26_PCIEDEV0_FLAGS_DEVICE_PRESENT
#define MPI26_PCIEDEV0_FLAGS_DEVICE_PRESENT (0x00000001U)
#endif
#ifndef MPI26_PCIEDEV0_FLAGS_UNSUPPORTED_DEVICE
#define MPI26_PCIEDEV0_FLAGS_UNSUPPORTED_DEVICE (0x00000100U)
#endif
#ifndef MPI26_PCIEDEV0_FLAGS_UNAUTHORIZED_DEVICE
#define MPI26_PCIEDEV0_FLAGS_UNAUTHORIZED_DEVICE (0x00008000U)
#endif
#ifndef MPI26_PCIEDEV0_FLAGS_ENABLED_FAST_PATH
#define MPI26_PCIEDEV0_FLAGS_ENABLED_FAST_PATH (0x00004000U)
#endif
#ifndef MPI26_PCIEDEV0_FLAGS_FAST_PATH_CAPABLE
#define MPI26_PCIEDEV0_FLAGS_FAST_PATH_CAPABLE (0x00002000U)
#endif
#ifndef MPI2_EVENT_PCIE_DEVICE_STATUS_CHANGE
#define MPI2_EVENT_PCIE_DEVICE_STATUS_CHANGE (0x00000030U)
#endif
#ifndef MPI2_EVENT_PCIE_ENUMERATION
#define MPI2_EVENT_PCIE_ENUMERATION (0x00000031U)
#endif
#ifndef MPI2_EVENT_PCIE_TOPOLOGY_CHANGE_LIST
#define MPI2_EVENT_PCIE_TOPOLOGY_CHANGE_LIST (0x00000032U)
#endif
#ifndef MPI26_SCSITASKMGMT_MSGFLAGS_PROTOCOL_LVL_RST_PCIE
#define MPI26_SCSITASKMGMT_MSGFLAGS_PROTOCOL_LVL_RST_PCIE (0x00000018U)
#endif
#ifndef MPI2_FUNCTION_NVME_ENCAPSULATED
#define MPI2_FUNCTION_NVME_ENCAPSULATED (0x00000033U)
#endif
#ifndef MPI26_REQ_DESCRIPT_FLAGS_PCIE_ENCAPSULATED
#define MPI26_REQ_DESCRIPT_FLAGS_PCIE_ENCAPSULATED (0x00000010U)
#endif
#ifndef MPI26_RPY_DESCRIPT_FLAGS_PCIE_ENCAPSULATED_SUCCESS
#define MPI26_RPY_DESCRIPT_FLAGS_PCIE_ENCAPSULATED_SUCCESS (0x00000008U)
#endif
#ifndef MPI26_IEEE_SGE_FLAGS_NSF_NVME_PRP
#define MPI26_IEEE_SGE_FLAGS_NSF_NVME_PRP (0x00000008U)
#endif
#ifndef MPI2_RAID_VOL_STATE_ONLINE
#define MPI2_RAID_VOL_STATE_ONLINE (0x00000003U)
#endif
#ifndef MPI2_RAID_VOL_STATE_DEGRADED
#define MPI2_RAID_VOL_STATE_DEGRADED (0x00000004U)
#endif
#ifndef MPI2_RAID_VOL_STATE_OPTIMAL
#define MPI2_RAID_VOL_STATE_OPTIMAL (0x00000005U)
#endif
#ifndef MPI2_RAIDVOL0_STATUS_FLAG_VOLUME_INACTIVE
#define MPI2_RAIDVOL0_STATUS_FLAG_VOLUME_INACTIVE (0x00000004U)
#endif


#include <stdint.h>
#include <stddef.h>
/* mpi2_pci.h: fixed MPI prefix plus the 64-byte NVMe command (not the
 * reference header's four-byte variable-length placeholder). */
#pragma pack(push, 1)
struct MPRNvmeEncapRequest {
    uint16_t DevHandle;
    uint8_t ChainOffset, Function;
    uint16_t EncapsulatedCommandLength;
    uint8_t Reserved1, MsgFlags, VP_ID, VF_ID;
    uint16_t Reserved2;
    uint32_t Reserved3;
    uint64_t ErrorResponseBaseAddress;
    uint16_t ErrorResponseAllocationLength, Flags;
    uint32_t DataLength;
    uint8_t Command[64];
};
struct MPRNvmeEncapReply {
    uint16_t DevHandle;
    uint8_t MsgLength, Function;
    uint16_t EncapsulatedCommandLength;
    uint8_t Reserved1, MsgFlags, VP_ID, VF_ID;
    uint16_t Reserved2, Reserved3, IOCStatus;
    uint32_t IOCLogInfo;
    uint16_t ErrorResponseCount, Reserved4;
};
#pragma pack(pop)
static_assert(sizeof(MPRNvmeEncapRequest) == 0x60, "NVMe request wire size");
static_assert(offsetof(MPRNvmeEncapRequest, Command) == 0x20, "NVMe command offset");
static_assert(offsetof(MPRNvmeEncapRequest, Flags) == 0x1A, "NVMe flags offset");
static_assert(sizeof(MPRNvmeEncapReply) == 0x18, "NVMe reply wire size");
static_assert(offsetof(MPRNvmeEncapReply, IOCStatus) == 0x0E, "NVMe IOC status offset");

#endif
