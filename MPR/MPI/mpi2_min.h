/*-
 * Copyright 2000-2020 Broadcom Inc. All rights reserved.
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

/*
 * mpi2_min.h - minimal MPI2.5 system-interface subset for the M1-a stage.
 *
 * Values and structure layouts are transcribed from FreeBSD
 * sys/dev/mpr/mpi/{mpi2.h,mpi2_ioc.h} at the fixed reference commit in
 * PLAN.md section 2. Only the registers, bits and messages needed to reach
 * IOC state READY and issue IOC Facts are included. All structures are
 * packed and their sizes are asserted below; do not rely on host ABI types
 * (the original U32 typedef becomes "unsigned long" outside FreeBSD).
 */

#ifndef MPR_MPI2_MIN_H
#define MPR_MPI2_MIN_H

#include <IOKit/IOTypes.h>
#include <stddef.h>

/* System interface register offsets (mpi2.h:226-255, :260-388) */
#define MPI2_DOORBELL_OFFSET                  0x00
#define MPI2_WRITE_SEQUENCE_OFFSET            0x04
#define MPI2_HOST_DIAGNOSTIC_OFFSET           0x08
#define MPI2_DIAG_RW_DATA_OFFSET              0x10
#define MPI2_DIAG_RW_ADDRESS_LOW_OFFSET       0x14
#define MPI2_DIAG_RW_ADDRESS_HIGH_OFFSET      0x18
#define MPI2_HOST_INTERRUPT_STATUS_OFFSET     0x30
#define MPI2_HOST_INTERRUPT_MASK_OFFSET       0x34
#define MPI2_DCR_DATA_OFFSET                  0x38
#define MPI2_DCR_ADDRESS_OFFSET               0x3C
#define MPI2_REPLY_FREE_HOST_INDEX_OFFSET     0x48
#define MPI2_REPLY_POST_HOST_INDEX_OFFSET     0x6C
#define MPI2_HCB_SIZE_OFFSET                  0x74
#define MPI2_HCB_ADDRESS_LOW_OFFSET           0x78
#define MPI2_HCB_ADDRESS_HIGH_OFFSET          0x7C
#define MPI2_SCRATCHPAD0_OFFSET               0xB0
#define MPI2_SCRATCHPAD1_OFFSET               0xB4
#define MPI2_SCRATCHPAD2_OFFSET               0xB8
#define MPI2_SCRATCHPAD3_OFFSET               0xBC
#define MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET  0xC0
#define MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET 0xC4
#define MPI2_ATOMIC_REQUEST_DESCRIPTOR_OFFSET    0xC8

/* Doorbell bits (mpi2.h:263-273) */
#define MPI2_DOORBELL_USED                    0x08000000U
#define MPI2_DOORBELL_WHO_INIT_MASK           0x07000000U
#define MPI2_DOORBELL_WHO_INIT_SHIFT          24
#define MPI2_DOORBELL_FAULT_CODE_MASK         0x0000FFFFU
#define MPI2_DOORBELL_DATA_MASK               0x0000FFFFU
#define MPI2_DOORBELL_FUNCTION_MASK           0xFF000000U
#define MPI2_DOORBELL_FUNCTION_SHIFT          24
#define MPI2_DOORBELL_ADD_DWORDS_MASK         0x00FF0000U
#define MPI2_DOORBELL_ADD_DWORDS_SHIFT        16

/* WriteSequence unlock keys, 7 writes (mpi2.h:279-286, mpr.c:122) */
#define MPI2_WRSEQ_FLUSH_KEY_VALUE            0x00
#define MPI2_WRSEQ_1ST_KEY_VALUE              0x0F
#define MPI2_WRSEQ_2ND_KEY_VALUE              0x04
#define MPI2_WRSEQ_3RD_KEY_VALUE              0x0B
#define MPI2_WRSEQ_4TH_KEY_VALUE              0x02
#define MPI2_WRSEQ_5TH_KEY_VALUE              0x07
#define MPI2_WRSEQ_6TH_KEY_VALUE              0x0D

/* HostDiagnostic bits (mpi2.h:293-315); note the MPI2_DIAG_ prefix */
#define MPI2_DIAG_BOOT_DEVICE_SELECT_MASK     0x00001800U
#define MPI2_DIAG_HCB_MODE                    0x00000100U
#define MPI2_DIAG_DIAG_WRITE_ENABLE           0x00000080U
#define MPI2_DIAG_RESET_HISTORY               0x00000020U
#define MPI2_DIAG_DIAG_RW_ENABLE              0x00000010U
#define MPI2_DIAG_RESET_ADAPTER               0x00000004U
#define MPI2_DIAG_HOLD_IOC_RESET              0x00000002U
#define MPI2_DIAG_CLEAR_FLASH_BAD_SIG         0x00000400U
#define MPI2_HCB_SIZE_HCB_ENABLE              0x00000001U

/* HostInterruptStatus / Mask bits (mpi2.h:327-343) */
#define MPI2_HIS_SYS2IOC_DB_STATUS            0x80000000U
#define MPI2_HIS_RESET_IRQ_STATUS             0x40000000U
#define MPI2_HIS_REPLY_DESCRIPTOR_INTERRUPT   0x00000008U
#define MPI2_HIS_IOC2SYS_DB_STATUS            0x00000001U
#define MPI2_HIM_REPLY_INT_MASK               0x00000008U
#define MPI2_HIM_IOC2SYS_DB_MASK              0x00000001U

/* IOC states are in Doorbell bits [31:28] (mpi2.h:208-214) */
#define MPI2_IOC_STATE_RESET                  0x00000000U
#define MPI2_IOC_STATE_READY                  0x10000000U
#define MPI2_IOC_STATE_OPERATIONAL            0x20000000U
#define MPI2_IOC_STATE_FAULT                  0x40000000U
#define MPI2_IOC_STATE_MASK                   0xF0000000U
#define MPI2_IOC_STATE_SHIFT                  28

/* MPI function codes (mpi2.h:663-699) */
#define MPI2_FUNCTION_IOC_INIT                0x02
#define MPI2_FUNCTION_IOC_FACTS               0x03
#define MPI2_FUNCTION_CONFIG                  0x04
#define MPI2_FUNCTION_PORT_ENABLE             0x06
#define MPI2_FUNCTION_EVENT_NOTIFICATION      0x07
#define MPI2_FUNCTION_IOC_MESSAGE_UNIT_RESET  0x40
#define MPI2_FUNCTION_HANDSHAKE               0x42

/* WhoInit (mpi2_ioc.h:258-263) */
#define MPI2_WHOINIT_NOT_READY                0x00
#define MPI2_WHOINIT_ROM_BIOS                 0x02
#define MPI2_WHOINIT_PCI_PEER                 0x03
#define MPI2_WHOINIT_HOST_DRIVER              0x04
#define MPI2_WHOINIT_MANUFACTURER             0x05

/* IOCStatus (mpi2.h:708-724) */
#define MPI2_IOCSTATUS_MASK                   0x7FFF
#define MPI2_IOCSTATUS_SUCCESS                0x0000

/* MPI and header versions (mpi2.h:177, :194-200) */
#define MPI2_VERSION                          0x0200
#define MPI2_VERSION_02_05                    0x0205
#define MPI2_HEADER_VERSION                   0x3400

/* IOC Init HostPageSize is a power-of-two exponent (mprvar.h:66) */
#define MPR_HOST_PAGE_SIZE_4K                 12

/* Default reply layout (mpi2.h:843-858) */
#define MPI2_DEFAULT_REPLY_MSG_LENGTH_OFF     0x02
#define MPI2_DEFAULT_REPLY_FUNCTION_OFF       0x03
#define MPI2_DEFAULT_REPLY_IOCSTATUS_OFF      0x0E

#pragma pack(push, 1)

/* MPI2_IOC_FACTS_REQUEST: 0x0C bytes (mpi2_ioc.h:320-332) */
typedef struct {
    UInt16  Reserved1;      /* 0x00 */
    UInt8   ChainOffset;    /* 0x02 */
    UInt8   Function;       /* 0x03 */
    UInt16  Reserved2;      /* 0x04 */
    UInt8   Reserved3;      /* 0x06 */
    UInt8   MsgFlags;       /* 0x07 */
    UInt8   VP_ID;          /* 0x08 */
    UInt8   VF_ID;          /* 0x09 */
    UInt16  Reserved4;      /* 0x0A */
} MPRIOCFactsRequest;

/* MPI2_IOC_FACTS_REPLY: 0x44 bytes (mpi2_ioc.h:335-378) */
typedef struct {
    UInt16  MsgVersion;                     /* 0x00 */
    UInt8   MsgLength;                      /* 0x02 */
    UInt8   Function;                       /* 0x03 */
    UInt16  HeaderVersion;                  /* 0x04 */
    UInt8   IOCNumber;                      /* 0x06 */
    UInt8   MsgFlags;                       /* 0x07 */
    UInt8   VP_ID;                          /* 0x08 */
    UInt8   VF_ID;                          /* 0x09 */
    UInt16  Reserved1;                      /* 0x0A */
    UInt16  IOCExceptions;                  /* 0x0C */
    UInt16  IOCStatus;                      /* 0x0E */
    UInt32  IOCLogInfo;                     /* 0x10 */
    UInt8   MaxChainDepth;                  /* 0x14 */
    UInt8   WhoInit;                        /* 0x15 */
    UInt8   NumberOfPorts;                  /* 0x16 */
    UInt8   MaxMSIxVectors;                 /* 0x17 */
    UInt16  RequestCredit;                  /* 0x18 */
    UInt16  ProductID;                      /* 0x1A */
    UInt32  IOCCapabilities;                /* 0x1C */
    UInt32  FWVersion;                      /* 0x20 (MPI2_VERSION_UNION.Word) */
    UInt16  IOCRequestFrameSize;            /* 0x24 */
    UInt16  IOCMaxChainSegmentSize;         /* 0x26 */
    UInt16  MaxInitiators;                  /* 0x28 */
    UInt16  MaxTargets;                     /* 0x2A */
    UInt16  MaxSasExpanders;                /* 0x2C */
    UInt16  MaxEnclosures;                  /* 0x2E */
    UInt16  ProtocolFlags;                  /* 0x30 */
    UInt16  HighPriorityCredit;             /* 0x32 */
    UInt16  MaxReplyDescriptorPostQueueDepth; /* 0x34 */
    UInt8   ReplyFrameSize;                 /* 0x36 */
    UInt8   MaxVolumes;                     /* 0x37 */
    UInt16  MaxDevHandle;                   /* 0x38 */
    UInt16  MaxPersistentEntries;           /* 0x3A */
    UInt16  MinDevHandle;                   /* 0x3C */
    UInt8   CurrentHostPageSize;            /* 0x3E */
    UInt8   Reserved4;                      /* 0x3F */
    UInt8   SGEModifierMask;                /* 0x40 */
    UInt8   SGEModifierValue;               /* 0x41 */
    UInt8   SGEModifierShift;               /* 0x42 */
    UInt8   Reserved5;                      /* 0x43 */
} MPRIOCFactsReply;

/* MPI2_IOC_INIT_REQUEST: 0x48 bytes (mpi2_ioc.h:226-255) */
typedef struct {
    UInt8   WhoInit;                        /* 0x00 */
    UInt8   Reserved1;                      /* 0x01 */
    UInt8   ChainOffset;                    /* 0x02 */
    UInt8   Function;                       /* 0x03 */
    UInt16  Reserved2;                      /* 0x04 */
    UInt8   Reserved3;                      /* 0x06 */
    UInt8   MsgFlags;                       /* 0x07 */
    UInt8   VP_ID;                          /* 0x08 */
    UInt8   VF_ID;                          /* 0x09 */
    UInt16  Reserved4;                      /* 0x0A */
    UInt16  MsgVersion;                     /* 0x0C */
    UInt16  HeaderVersion;                  /* 0x0E */
    UInt32  Reserved5;                      /* 0x10 */
    UInt16  ConfigurationFlags;             /* 0x14 */
    UInt8   HostPageSize;                   /* 0x16 */
    UInt8   HostMSIxVectors;                /* 0x17 */
    UInt16  Reserved8;                      /* 0x18 */
    UInt16  SystemRequestFrameSize;         /* 0x1A */
    UInt16  ReplyDescriptorPostQueueDepth;  /* 0x1C */
    UInt16  ReplyFreeQueueDepth;            /* 0x1E */
    UInt32  SenseBufferAddressHigh;         /* 0x20 */
    UInt32  SystemReplyAddressHigh;         /* 0x24 */
    UInt64  SystemRequestFrameBaseAddress;  /* 0x28 */
    UInt64  ReplyDescriptorPostQueueAddress;/* 0x30 */
    UInt64  ReplyFreeQueueAddress;          /* 0x38 */
    UInt64  TimeStamp;                      /* 0x40 */
} MPRIOCInitRequest;

/* MPI2_IOC_INIT_REPLY: 0x14 bytes (mpi2_ioc.h:296-313) */
typedef struct {
    UInt8   WhoInit;        /* 0x00 */
    UInt8   Reserved1;      /* 0x01 */
    UInt8   MsgLength;      /* 0x02 */
    UInt8   Function;       /* 0x03 */
    UInt16  Reserved2;      /* 0x04 */
    UInt8   Reserved3;      /* 0x06 */
    UInt8   MsgFlags;       /* 0x07 */
    UInt8   VP_ID;          /* 0x08 */
    UInt8   VF_ID;          /* 0x09 */
    UInt16  Reserved4;      /* 0x0A */
    UInt16  Reserved5;      /* 0x0C */
    UInt16  IOCStatus;      /* 0x0E */
    UInt32  IOCLogInfo;     /* 0x10 */
} MPRIOCInitReply;

#pragma pack(pop)

static_assert(sizeof(MPRIOCFactsRequest) == 0x0C, "IOC Facts request layout");
static_assert(sizeof(MPRIOCFactsReply) == 0x44, "IOC Facts reply layout");
static_assert(offsetof(MPRIOCFactsReply, IOCStatus) == 0x0E, "IOCStatus offset");
static_assert(offsetof(MPRIOCFactsReply, IOCCapabilities) == 0x1C, "IOCCapabilities offset");
static_assert(sizeof(MPRIOCInitRequest) == 0x48, "IOC Init request layout");
static_assert(sizeof(MPRIOCInitReply) == 0x14, "IOC Init reply layout");
static_assert(offsetof(MPRIOCInitRequest, SystemRequestFrameBaseAddress) == 0x28,
              "request frame base offset");
static_assert(offsetof(MPRIOCInitRequest, ReplyDescriptorPostQueueAddress) == 0x30,
              "reply post queue offset");
static_assert(offsetof(MPRIOCInitRequest, ReplyFreeQueueAddress) == 0x38,
              "reply free queue offset");

/* ---------------------------------------------------------------------- */
/* M1-c: config page request/response over the request/reply queues.       */

/* Reply descriptor flags (mpi2.h:546-557) */
#define MPI2_RPY_DESCRIPT_FLAGS_TYPE_MASK      0x0F
#define MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY  0x01
#define MPI2_RPY_DESCRIPT_FLAGS_UNUSED         0x0F

/* Request descriptor default type (mpi2.h:421) */
#define MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE   0x08

/* Config action / page type (mpi2_cnfg.h:336-343, :506-513) */
#define MPI2_CONFIG_ACTION_PAGE_HEADER         0x00
#define MPI2_CONFIG_ACTION_PAGE_READ_CURRENT   0x01
#define MPI2_CONFIG_PAGETYPE_MANUFACTURING     0x09
#define MPI2_MANUFACTURING0_PAGEVERSION        0x00

/* Simple SGE flags (mpi2.h:1068-1078, :1084, :1103) */
#define MPI2_SGE_FLAGS_LAST_ELEMENT            0x80
#define MPI2_SGE_FLAGS_END_OF_BUFFER           0x40
#define MPI2_SGE_FLAGS_SIMPLE_ELEMENT          0x10
#define MPI2_SGE_FLAGS_64_BIT_ADDRESSING       0x02
#define MPI2_SGE_FLAGS_SHIFT                   24
#define MPI2_SGE_LENGTH_MASK                   0x00FFFFFF

#pragma pack(push, 1)

/* MPI2_DEFAULT_REQUEST_DESCRIPTOR: 8 bytes (mpi2.h:404-413) */
typedef struct {
    UInt8   RequestFlags;               /* 0x00 */
    UInt8   MSIxIndex;                  /* 0x01 */
    UInt16  SMID;                       /* 0x02 */
    UInt16  LMID;                       /* 0x04 */
    UInt16  DescriptorTypeDependent;    /* 0x06 */
} MPRDefaultRequestDescriptor;

/* MPI2_CONFIG_PAGE_HEADER: 4 bytes (mpi2_cnfg.h:288-295) */
typedef struct {
    UInt8   PageVersion;    /* 0x00 */
    UInt8   PageLength;     /* 0x01 */
    UInt8   PageNumber;     /* 0x02 */
    UInt8   PageType;       /* 0x03 */
} MPRConfigPageHeader;

/* MPI2_CONFIG_REQUEST: 0x28 bytes (mpi2_cnfg.h:483-503) */
typedef struct {
    UInt8   Action;                 /* 0x00 */
    UInt8   SGLFlags;               /* 0x01 */
    UInt8   ChainOffset;            /* 0x02 */
    UInt8   Function;               /* 0x03 */
    UInt16  ExtPageLength;          /* 0x04 */
    UInt8   ExtPageType;            /* 0x06 */
    UInt8   MsgFlags;               /* 0x07 */
    UInt8   VP_ID;                  /* 0x08 */
    UInt8   VF_ID;                  /* 0x09 */
    UInt16  Reserved1;              /* 0x0A */
    UInt8   Reserved2;              /* 0x0C */
    UInt8   ProxyVF_ID;             /* 0x0D */
    UInt16  Reserved4;              /* 0x0E */
    UInt32  Reserved3;              /* 0x10 */
    MPRConfigPageHeader Header;     /* 0x14 */
    UInt32  PageAddress;            /* 0x18 */
    UInt32  SgeFlagsLength;         /* 0x1C */
    UInt64  SgeAddress;             /* 0x20 */
} MPRConfigRequest;

/* MPI2_CONFIG_REPLY: 0x18 bytes (mpi2_cnfg.h:518-535) */
typedef struct {
    UInt8   Action;                 /* 0x00 */
    UInt8   SGLFlags;               /* 0x01 */
    UInt8   MsgLength;              /* 0x02 */
    UInt8   Function;               /* 0x03 */
    UInt16  ExtPageLength;          /* 0x04 */
    UInt8   ExtPageType;            /* 0x06 */
    UInt8   MsgFlags;               /* 0x07 */
    UInt8   VP_ID;                  /* 0x08 */
    UInt8   VF_ID;                  /* 0x09 */
    UInt16  Reserved1;              /* 0x0A */
    UInt16  Reserved2;              /* 0x0C */
    UInt16  IOCStatus;              /* 0x0E */
    UInt32  IOCLogInfo;             /* 0x10 */
    MPRConfigPageHeader Header;     /* 0x14 */
} MPRConfigReply;

/* MPI2_CONFIG_PAGE_MAN_0: 0x4C bytes (mpi2_cnfg.h:615-625) */
typedef struct {
    MPRConfigPageHeader Header;             /* 0x00 */
    UInt8   ChipName[16];                   /* 0x04 */
    UInt8   ChipRevision[8];                /* 0x14 */
    UInt8   BoardName[16];                  /* 0x1C */
    UInt8   BoardAssembly[16];              /* 0x2C */
    UInt8   BoardTracerNumber[16];          /* 0x3C */
} MPRConfigPageMan0;

#pragma pack(pop)

static_assert(sizeof(MPRDefaultRequestDescriptor) == 0x08, "request descriptor layout");
static_assert(sizeof(MPRConfigPageHeader) == 0x04, "config page header layout");
static_assert(sizeof(MPRConfigRequest) == 0x28, "config request layout");
static_assert(sizeof(MPRConfigReply) == 0x18, "config reply layout");
static_assert(sizeof(MPRConfigPageMan0) == 0x4C, "manufacturing page 0 layout");
static_assert(offsetof(MPRConfigRequest, SgeFlagsLength) == 0x1C, "config SGE offset");
static_assert(offsetof(MPRConfigPageMan0, ChipName) == 0x04, "ChipName offset");

#endif /* MPR_MPI2_MIN_H */
