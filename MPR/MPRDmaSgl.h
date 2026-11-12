#ifndef MPR_DMA_SGL_H
#define MPR_DMA_SGL_H

#include <stdint.h>
#include <stddef.h>

/* MPI 2.5 IEEE SGE wire format, checked against the FreeBSD MPI headers.
 * This bounded encoder has no IOKit dependencies so its output can be
 * validated without a controller or a particular physical memory layout. */
struct MPRDmaSegment {
    uint64_t address;
    uint32_t length;
};

struct MPRSglResult {
    uint32_t chainsUsed;
    uint8_t requestChainOffset;
};

static inline void
mprSglStore32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}

static inline void
mprSglElement(uint8_t *p, uint64_t address, uint32_t length,
              uint8_t nextChain, uint8_t flags)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(address >> (i * 8));
    mprSglStore32(p + 8, length);
    p[12] = p[13] = 0;
    p[14] = nextChain;
    p[15] = flags;
}

/* Returns false before touching either output if geometry/address/length
 * cannot be represented. No truncation, allocation, or wraparound fallback.
 * ChainOffset and NextChainOffset are measured in 16-byte IEEE elements. */
static inline bool
mprBuildIeeeSgl(uint8_t *request, uint32_t requestBytes, uint32_t sglOffset,
                 const MPRDmaSegment *segments, uint32_t segmentCount,
                 uint32_t expectedBytes, uint8_t *chainStorage,
                 uint64_t chainAddress, uint32_t chainFrameBytes,
                 uint32_t chainDepth, uint32_t chainStorageBytes,
                 MPRSglResult *result)
{
    const uint64_t maxAddress = UINT64_C(0x7fffffffffffffff);
    if (!request || !segments || !result || !segmentCount || !expectedBytes ||
        sglOffset > requestBytes || (sglOffset & 15) ||
        requestBytes - sglOffset < 16 || requestBytes > 4096) return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < segmentCount; ++i) {
        if (!segments[i].length || segments[i].address > maxAddress ||
            segments[i].length - 1 > maxAddress - segments[i].address)
            return false;
        total += segments[i].length;
        if (total > expectedBytes) return false;
    }
    if (total != expectedBytes) return false;

    const uint32_t inlineCount = (requestBytes - sglOffset) / 16;
    uint32_t chainCount = 0;
    uint32_t chainSlots = 0;
    uint32_t remaining = 0;
    if (segmentCount > inlineCount) {
        if (!chainStorage || !chainAddress || (chainAddress & 15) ||
            (chainFrameBytes & 15) || chainFrameBytes < 32 ||
            chainFrameBytes > 4096 || !chainDepth) return false;
        chainSlots = chainFrameBytes / 16;
        remaining = segmentCount - (inlineCount - 1);
        uint32_t n = remaining;
        while (n) {
            if (++chainCount > chainDepth ||
                chainCount > chainStorageBytes / chainFrameBytes) return false;
            n = n <= chainSlots ? 0 : n - (chainSlots - 1);
        }
        uint64_t usedBytes = (uint64_t)chainCount * chainFrameBytes;
        if (chainAddress > maxAddress || usedBytes - 1 > maxAddress - chainAddress)
            return false;
        if ((sglOffset + (inlineCount - 1) * 16) / 16 > 255)
            return false;
    }

    result->chainsUsed = chainCount;
    result->requestChainOffset = 0;
    uint32_t pos = 0;
    uint32_t inlineData = chainCount ? inlineCount - 1 : segmentCount;
    for (; pos < inlineData; ++pos) {
        mprSglElement(request + sglOffset + pos * 16, segments[pos].address,
                      segments[pos].length, 0,
                      pos + 1 == segmentCount ? 0x40 : 0x00);
    }
    if (!chainCount) return true;

    uint8_t *link = request + sglOffset + inlineData * 16;
    result->requestChainOffset = (uint8_t)((sglOffset + inlineData * 16) / 16);
    for (uint32_t chain = 0; chain < chainCount; ++chain) {
        const bool last = chain + 1 == chainCount;
        const uint32_t dataCount = last ? remaining : chainSlots - 1;
        const uint32_t frameLength = (dataCount + (last ? 0 : 1)) * 16;
        mprSglElement(link, chainAddress + (uint64_t)chain * chainFrameBytes,
                      frameLength, last ? 0 : (uint8_t)dataCount, 0x80);
        uint8_t *frame = chainStorage + chain * chainFrameBytes;
        for (uint32_t i = 0; i < dataCount; ++i, ++pos) {
            mprSglElement(frame + i * 16, segments[pos].address,
                          segments[pos].length, 0,
                          pos + 1 == segmentCount ? 0x40 : 0x00);
        }
        remaining -= dataCount;
        link = frame + dataCount * 16;
    }
    return true;
}

#endif
