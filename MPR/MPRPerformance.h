#ifndef MPR_PERFORMANCE_H
#define MPR_PERFORMANCE_H

#include <stdint.h>

/* Keep management/probe/synchronous I/O (1..31) isolated from the async pool.
 * Request frame, sense and reply-owner arrays all have 128 entries. */
enum {
    kMPRAsyncFirstSmid = 32,
    kMPRAsyncMaxCommands = 32,
    kMPRAsyncRequestCount = 64,
    kMPRAsyncTransferBytes = 65536,
    kMPRReplyBatchBudget = 32,
    kMPRReplyReserve = 8,
    kMPRAsyncTimeoutMs = 15000
};

static inline bool mprIsAsyncSmid(uint32_t smid)
{
    return smid >= kMPRAsyncFirstSmid &&
           smid - kMPRAsyncFirstSmid < kMPRAsyncMaxCommands;
}

static inline uint32_t mprConfiguredCommandDepth(int requested)
{
    if (requested <= 0) return kMPRAsyncMaxCommands;
    return (uint32_t)requested < kMPRAsyncMaxCommands ?
           (uint32_t)requested : (uint32_t)kMPRAsyncMaxCommands;
}

/* Conservatively reserve all low SMIDs/credits, plus reply capacity for
 * management, TMFs and asynchronous events. No unsigned subtraction wraps.
 * IOC Facts, allocated resources and the same limits after reset all apply. */
static inline uint32_t
mprAsyncCommandLimit(uint32_t allocated, uint32_t requestCount,
                     uint32_t requestCredit, uint32_t postDepth,
                     uint32_t freeDepth)
{
    if (requestCount <= kMPRAsyncFirstSmid ||
        requestCredit <= kMPRAsyncFirstSmid ||
        postDepth <= kMPRReplyReserve + 1 ||
        freeDepth <= kMPRReplyReserve + 1) return 0;
    uint32_t limit = allocated < kMPRAsyncMaxCommands ?
                     allocated : (uint32_t)kMPRAsyncMaxCommands;
    const uint32_t bounds[] = {
        requestCount - kMPRAsyncFirstSmid,
        requestCredit - kMPRAsyncFirstSmid,
        postDepth - 1 - kMPRReplyReserve,
        freeDepth - 1 - kMPRReplyReserve,
        128 - kMPRAsyncFirstSmid
    };
    for (unsigned i = 0; i < sizeof(bounds) / sizeof(bounds[0]); ++i)
        if (bounds[i] < limit) limit = bounds[i];
    return limit;
}

/* After all issued chunks retire, only the prefix before the first error is
 * reportable. Out-of-order successes beyond a failed chunk are not a prefix. */
static inline uint64_t
mprCompletedPrefix(uint64_t successfulBytes, uint64_t firstErrorOffset)
{
    return successfulBytes < firstErrorOffset ? successfulBytes : firstErrorOffset;
}

#endif
