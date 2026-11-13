/* Host-only wire-format regressions; no IOKit, controller, or disk required.
 *
 * Build: c++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
 *   -fsanitize=address,undefined tools/test_protocol.cpp -o /tmp/mpr-protocol
 * Run: /tmp/mpr-protocol
 *
 * Golden layouts come from the FreeBSD MPI 2.5 headers and SCSI CDB/VPD
 * field definitions. The SGL reader below follows wire links independently
 * of the encoder's allocation/count algorithm.
 */
#include "../MPR/MPRScsi.h"
#include "../MPR/MPRCompletion.h"
#include "../MPR/MPRDmaSgl.h"
#include "../MPR/MPRPerformance.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <set>
#include <vector>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::cerr << "line " << __LINE__ << ": " #condition "\n"; \
        std::exit(1); \
    } \
} while (0)

static void cdbTests()
{
    uint8_t cdb[16];
    CHECK(mprBuildReadWriteCdb(cdb, false, 0x12345678, 0x1234, false) == 10);
    const uint8_t read10[16] = {
        0x28, 0, 0x12, 0x34, 0x56, 0x78, 0, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0
    };
    CHECK(std::memcmp(cdb, read10, sizeof(cdb)) == 0);
    CHECK(mprBuildReadWriteCdb(cdb, true, 0x12345678, 0x1234, true) == 10);
    CHECK(cdb[0] == 0x2a && cdb[1] == 8);

    // Last 512-byte sector below 2 TiB, followed by a transfer crossing it.
    CHECK(mprBuildReadWriteCdb(cdb, false, UINT64_C(0xffffffff), 1, false) == 10);
    CHECK(mprBuildReadWriteCdb(cdb, true, UINT64_C(0xffffffff), 2, true) == 16);
    const uint8_t write16[16] = {
        0x8a, 8, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 2, 0, 0
    };
    CHECK(std::memcmp(cdb, write16, sizeof(cdb)) == 0);
    CHECK(mprBuildReadWriteCdb(cdb, false, UINT64_C(0x1122334455667788),
                              0x12345678, false) == 16);
    const uint8_t read16[16] = {
        0x88, 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x12, 0x34, 0x56, 0x78, 0, 0
    };
    CHECK(std::memcmp(cdb, read16, sizeof(cdb)) == 0);
    CHECK(mprBuildReadWriteCdb(cdb, false, 0, 65535, false) == 10);
    CHECK(mprBuildReadWriteCdb(cdb, false, 0, 65536, false) == 16);
    CHECK(mprBuildReadWriteCdb(cdb, false, UINT64_MAX, 1, false) == 16);
    CHECK(mprBuildReadWriteCdb(cdb, false, UINT64_MAX, 2, false) == 0);
    CHECK(mprBuildReadWriteCdb(cdb, false, 0, 0, false) == 0);

    CHECK(mprBuildSynchronizeCdb(cdb, UINT64_C(0x100000000), 0x10203) == 16);
    const uint8_t sync16[16] = {
        0x91, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 2, 3, 0, 0
    };
    CHECK(std::memcmp(cdb, sync16, sizeof(cdb)) == 0);
    CHECK(mprBuildSynchronizeCdb(cdb, 0x12345678, 0x1234) == 10);
    CHECK(cdb[0] == 0x35 && cdb[2] == 0x12 && cdb[7] == 0x12);
    CHECK(mprBuildSynchronizeCdb(cdb, 0, UINT64_C(0x100000000)) == 10);
    CHECK(cdb[0] == 0x35);
    for (unsigned i = 1; i < sizeof(cdb); ++i) CHECK(cdb[i] == 0);
    CHECK(mprBuildSynchronizeCdb(cdb, UINT64_MAX, 2) == 0);
}

static void vpdAndRaidTests()
{
    // Block Limits page: 4096 blocks, three descriptors, 8-block hint,
    // alignment two. Logical Block Provisioning page: LBPU and LBPRZ.
    std::array<uint8_t, 36> limits{};
    limits[1] = 0xb0; limits[3] = 32;
    limits[22] = 0x10; limits[27] = 3; limits[31] = 8;
    limits[32] = 0x80; limits[35] = 2;
    std::array<uint8_t, 8> provisioning{{0, 0xb2, 0, 4, 0, 0x84, 0, 0}};
    MPRUnmapLimits out{};
    CHECK(mprParseUnmapLimits(limits.data(), limits.size(),
                              provisioning.data(), provisioning.size(), &out));
    CHECK(out.maxBlocks == 4096 && out.maxDescriptors == 3);
    CHECK(out.optimalGranularity == 8 && out.alignmentValid && out.alignment == 2);
    CHECK(out.readsZero);
    CHECK(!mprParseUnmapLimits(limits.data(), 35, provisioning.data(), 8, &out));
    CHECK(!mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 7, &out));
    limits[1] = 0xb1;
    CHECK(!mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[1] = 0xb0;
    limits[3] = 31;
    CHECK(!mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[3] = 33;
    CHECK(!mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[3] = 32;
    provisioning[5] = 4;
    CHECK(!mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    provisioning[5] = 0x80;
    CHECK(mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    CHECK(!out.readsZero);
    limits[22] = 0;
    CHECK(!mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[22] = 0x10; limits[27] = 0;
    CHECK(!mprParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));

    CHECK(mprValidBlockExtent(99, 1, 100));
    CHECK(!mprValidBlockExtent(99, 2, 100));
    CHECK(!mprValidBlockExtent(100, 1, 100));
    CHECK(!mprValidBlockExtent(UINT64_MAX - 2, 4, UINT64_MAX));
    CHECK(mprValidBlockExtent(UINT64_MAX - 2, 2, UINT64_MAX));
    CHECK(mprValidBlockExtent(UINT64_MAX, 0, 0));

    // Genuine extended-page header: number@2, type@3, ext type@6.
    // This deliberately differs from the prototype's former wrong offsets.
    std::array<uint8_t, 64> raid{};
    raid[3] = 0x0f; raid[4] = 16; raid[6] = 0x16; raid[0x2c] = 2;
    raid[0x32] = 0xcd; raid[0x33] = 0xab; // volume ABCD
    raid[0x38] = 1;                     // physical member
    raid[0x3a] = 0xcd; raid[0x3b] = 0xab;
    raid[0x3e] = 0x34; raid[0x3f] = 0x12;
    bool contains = false;
    CHECK(mprRaidConfigContains(raid.data(), raid.size(), 0x1234, &contains) && contains);
    CHECK(mprRaidConfigContains(raid.data(), raid.size(), 0xabcd, &contains) && contains);
    CHECK(mprRaidConfigContains(raid.data(), raid.size(), 0x4321, &contains) && !contains);
    CHECK(mprRaidConfigContains(raid.data(), raid.size(), 0, &contains) && !contains);
    CHECK(!mprRaidConfigContains(raid.data(), 63, 0x1234, &contains));
    raid[0x2c] = 3;
    CHECK(!mprRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[0x2c] = 2; raid[2] = 1;
    CHECK(!mprRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[2] = 0; raid[6] = 0x12;
    CHECK(!mprRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[6] = 0x16; raid[0x38] = 4;
    CHECK(!mprRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[0x38] = 2;
    CHECK(mprRaidConfigContains(raid.data(), 64, 0x1234, &contains) && contains);
    raid[0x38] = 3;
    CHECK(mprRaidConfigContains(raid.data(), 64, 0x1234, &contains) && contains);
    CHECK(!mprRaidConfigContains(nullptr, 64, 0x1234, &contains));
}

static void senseTests()
{
    std::array<uint8_t, 14> fixed{{0x70, 0, 5, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0x20, 1}};
    std::array<uint8_t, 8> descriptor{{0x72, 6, 0x29, 2, 0, 0, 0, 0}};
    MPRSenseFields first{}, second{};
    CHECK(mprDecodeSenseFields(fixed.data(), fixed.size(), &first));
    CHECK(first.key == 5 && first.asc == 0x20 && first.ascq == 1);
    CHECK(mprDecodeSenseFields(descriptor.data(), descriptor.size(), &second));
    CHECK(second.key == 6 && second.asc == 0x29 && second.ascq == 2);
    CHECK(first.key == 5 && first.asc == 0x20 && first.ascq == 1);
    fixed[0] = 0xf1; descriptor[0] = 0x73;
    CHECK(mprDecodeSenseFields(fixed.data(), fixed.size(), &first));
    CHECK(mprDecodeSenseFields(descriptor.data(), descriptor.size(), &second));

    // Actual short allocations let ASan detect any attempted ASC/ASCQ read.
    const std::array<uint8_t, 8> shortFixed{{0x70, 0, 3, 0, 0, 0, 0, 255}};
    CHECK(mprDecodeSenseFields(shortFixed.data(), shortFixed.size(), &first));
    CHECK(first.key == 3 && first.asc == 0 && first.ascq == 0);
    CHECK(mprDecodeSenseFields(fixed.data(), 13, &first));
    CHECK(first.asc == 0 && first.ascq == 0);
    fixed[7] = 0;
    CHECK(mprDecodeSenseFields(fixed.data(), fixed.size(), &first));
    CHECK(first.asc == 0 && first.ascq == 0);
    CHECK(!mprDecodeSenseFields(descriptor.data(), 7, &second));
    descriptor[0] = 0x55;
    CHECK(!mprDecodeSenseFields(descriptor.data(), 8, &second));
    CHECK(!mprDecodeSenseFields(nullptr, 8, &second));
}

static uint64_t readLittle(const uint8_t *p, unsigned bytes)
{
    uint64_t result = 0;
    for (unsigned i = bytes; i != 0; --i) result = (result << 8) | p[i - 1];
    return result;
}

static std::vector<MPRDmaSegment>
decodeSgl(const std::vector<uint8_t>& request, unsigned sglOffset,
          unsigned chainOffset, const std::vector<uint8_t>& storage,
          uint64_t base, unsigned& chains)
{
    std::vector<MPRDmaSegment> decoded;
    auto data = [&](const uint8_t *entry, bool last) {
        CHECK(entry[15] == (last ? 0x40 : 0));
        CHECK(entry[12] == 0 && entry[13] == 0 && entry[14] == 0);
        uint64_t length = readLittle(entry + 8, 4);
        CHECK(length != 0);
        decoded.push_back({readLittle(entry, 8), static_cast<uint32_t>(length)});
    };
    if (chainOffset == 0) {
        bool ended = false;
        for (unsigned offset = sglOffset; offset + 16 <= request.size(); offset += 16) {
            bool last = (request[offset + 15] & 0x40) != 0;
            data(request.data() + offset, last);
            if (last) { ended = true; break; }
        }
        CHECK(ended);
        return decoded;
    }
    unsigned linkOffset = chainOffset * 16;
    CHECK(linkOffset >= sglOffset && linkOffset + 16 <= request.size());
    for (unsigned offset = sglOffset; offset < linkOffset; offset += 16)
        data(request.data() + offset, false);
    const uint8_t *link = request.data() + linkOffset;
    std::set<uint64_t> visited;
    for (;;) {
        CHECK(link[15] == 0x80 && link[12] == 0 && link[13] == 0);
        uint64_t address = readLittle(link, 8);
        uint64_t length = readLittle(link + 8, 4);
        unsigned next = link[14];
        CHECK(address >= base && visited.insert(address).second);
        CHECK(length != 0 && length % 16 == 0);
        CHECK(address - base <= storage.size() && length <= storage.size() - (address - base));
        const uint8_t *frame = storage.data() + (address - base);
        ++chains;
        if (next == 0) {
            for (unsigned offset = 0; offset < length; offset += 16)
                data(frame + offset, offset + 16 == length);
            return decoded;
        }
        CHECK((next + 1) * 16 == length);
        for (unsigned index = 0; index < next; ++index) data(frame + index * 16, false);
        link = frame + next * 16;
    }
}

static void sglGoldenTests()
{
    const MPRDmaSegment segments[] = {{0x1000, 0x200}, {0x2000, 0x300}, {0x3000, 0x400}};
    std::array<uint8_t, 112> request{};
    std::array<uint8_t, 64> storage{};
    MPRSglResult result{};
    CHECK(mprBuildIeeeSgl(request.data(), 112, 96, segments, 3, 0x900,
                           storage.data(), 0x200000, 32, 2, 64, &result));
    CHECK(result.chainsUsed == 2 && result.requestChainOffset == 6);
    const uint8_t expectedLink[16] = {
        0, 0, 0x20, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 1, 0x80
    };
    const uint8_t expectedChains[64] = {
        0, 0x10, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,
        32, 0, 0x20, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0x80,
        0, 0x20, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0,
        0, 0x30, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0x40
    };
    CHECK(std::memcmp(request.data() + 96, expectedLink, 16) == 0);
    CHECK(std::memcmp(storage.data(), expectedChains, 64) == 0);

    request.fill(0xa5); storage.fill(0xa5);
    result = {0xfedcba98, 0x76};
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, segments, 3, 0x900,
                            storage.data(), 0x200000, 32, 1, 64, &result));
    CHECK(std::all_of(request.begin(), request.end(), [](uint8_t n) { return n == 0xa5; }));
    CHECK(std::all_of(storage.begin(), storage.end(), [](uint8_t n) { return n == 0xa5; }));
    CHECK(result.chainsUsed == 0xfedcba98 && result.requestChainOffset == 0x76);
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, segments, 3, 0x900,
                            storage.data(), 0x200000, 32, 2, 63, &result));
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, segments, 3, 0x900,
                            storage.data(), 0x200001, 32, 2, 64, &result));
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, segments, 3, 0x900,
                            storage.data(), 0x200000, 16, 4, 64, &result));
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, segments, 3, 0x900,
                            storage.data(), UINT64_C(0x7ffffffffffffff0), 32, 2, 64, &result));
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, segments, 3, 0x901,
                            storage.data(), 0x200000, 32, 2, 64, &result));
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 95, segments, 3, 0x900,
                            storage.data(), 0x200000, 32, 2, 64, &result));
    const MPRDmaSegment zero = {0x1000, 0};
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, &zero, 1, 1,
                            nullptr, 0, 0, 0, 0, &result));
    const MPRDmaSegment high = {UINT64_C(0x7fffffffffffffff), 2};
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, &high, 1, 2,
                            nullptr, 0, 0, 0, 0, &result));
    const MPRDmaSegment edge = {UINT64_C(0x7fffffffffffffff), 1};
    CHECK(mprBuildIeeeSgl(request.data(), 112, 96, &edge, 1, 1,
                           nullptr, 0, 0, 0, 0, &result));
    CHECK(result.chainsUsed == 0 && result.requestChainOffset == 0);

    const MPRDmaSegment oversized[] = {{0x1000, UINT32_MAX}, {UINT64_C(0x100001000), 1}};
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, oversized, 2, UINT32_MAX,
                            storage.data(), 0x200000, 32, 2, 64, &result));
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, &edge, 1, 0,
                            nullptr, 0, 0, 0, 0, &result));
    CHECK(!mprBuildIeeeSgl(request.data(), 112, 96, &edge, 0, 1,
                            nullptr, 0, 0, 0, 0, &result));

    // Maximum byte-valued NextChainOffset and request ChainOffset must be
    // represented as 255, never wrap to zero (which means end-of-chain).
    std::vector<MPRDmaSegment> many(257);
    for (unsigned i = 0; i < many.size(); ++i) many[i] = {0x1000 + i * 4096U, 1};
    std::vector<uint8_t> wideStorage(8192);
    CHECK(mprBuildIeeeSgl(request.data(), 112, 96, many.data(), 257, 257,
                           wideStorage.data(), 0x200000, 4096, 2, 8192, &result));
    CHECK(result.chainsUsed == 2 && request[96 + 14] == 255);
    unsigned decodedChains = 0;
    std::vector<uint8_t> smallRequest(request.begin(), request.end());
    CHECK(decodeSgl(smallRequest, 96, result.requestChainOffset,
                    wideStorage, 0x200000, decodedChains).size() == 257);
    std::vector<uint8_t> wideRequest(4096);
    CHECK(mprBuildIeeeSgl(wideRequest.data(), 4096, 96, many.data(), 251, 251,
                           storage.data(), 0x200000, 32, 1, 64, &result));
    CHECK(result.requestChainOffset == 255 && result.chainsUsed == 1);
    CHECK(!mprBuildIeeeSgl(wideRequest.data(), 4112, 96, many.data(), 251, 251,
                            storage.data(), 0x200000, 32, 1, 64, &result));
}

static void sglRoundTrips()
{
    std::mt19937 random(0x6d7072);
    for (unsigned trial = 0; trial < 2000; ++trial) {
        const unsigned inlineSlots = 1 + random() % 12;
        const unsigned chainSlots = 2 + random() % 12;
        const unsigned depth = 1 + random() % 8;
        const unsigned count = 1 + random() % 80;
        const unsigned requestBytes = 96 + inlineSlots * 16;
        const unsigned storageBytes = chainSlots * 16 * depth;
        const uint64_t base = UINT64_C(0x123400000);
        std::vector<uint8_t> guardedRequest(requestBytes + 32, 0xa5);
        std::vector<uint8_t> guardedStorage(storageBytes + 32, 0xa5);
        std::vector<MPRDmaSegment> original;
        uint32_t total = 0;
        for (unsigned i = 0; i < count; ++i) {
            uint32_t length = 1 + random() % 65536;
            original.push_back({UINT64_C(0x100000000) + i * UINT64_C(0x20000), length});
            total += length;
        }
        MPRSglResult result{0x12345678, 0x9a};
        bool ok = mprBuildIeeeSgl(guardedRequest.data() + 16, requestBytes, 96,
                                  original.data(), count, total,
                                  guardedStorage.data() + 16, base,
                                  chainSlots * 16, depth, storageBytes, &result);
        // Independent capacity equation: each nonfinal frame spends one
        // element on a link, while the final frame uses all data elements.
        CHECK(ok == (count <= inlineSlots + depth * (chainSlots - 1)));
        if (!ok) {
            CHECK(std::all_of(guardedRequest.begin(), guardedRequest.end(), [](uint8_t n) { return n == 0xa5; }));
            CHECK(std::all_of(guardedStorage.begin(), guardedStorage.end(), [](uint8_t n) { return n == 0xa5; }));
            CHECK(result.chainsUsed == 0x12345678 && result.requestChainOffset == 0x9a);
            continue;
        }
        CHECK(std::all_of(guardedRequest.begin(), guardedRequest.begin() + 112, [](uint8_t n) { return n == 0xa5; }));
        CHECK(std::all_of(guardedRequest.end() - 16, guardedRequest.end(), [](uint8_t n) { return n == 0xa5; }));
        CHECK(std::all_of(guardedStorage.begin(), guardedStorage.begin() + 16, [](uint8_t n) { return n == 0xa5; }));
        CHECK(std::all_of(guardedStorage.end() - 16, guardedStorage.end(), [](uint8_t n) { return n == 0xa5; }));
        std::vector<uint8_t> request(guardedRequest.begin() + 16, guardedRequest.end() - 16);
        std::vector<uint8_t> storage(guardedStorage.begin() + 16, guardedStorage.end() - 16);
        unsigned chains = 0;
        auto decoded = decodeSgl(request, 96, result.requestChainOffset, storage, base, chains);
        CHECK(decoded.size() == original.size() && chains == result.chainsUsed && chains <= depth);
        uint64_t recoveredBytes = 0;
        for (unsigned i = 0; i < decoded.size(); ++i) {
            CHECK(decoded[i].address == original[i].address);
            CHECK(decoded[i].length == original[i].length);
            recoveredBytes += decoded[i].length;
        }
        CHECK(recoveredBytes == total);
    }
}


static void performanceLimitTests()
{
    CHECK(mprConfiguredCommandDepth(0) == 32);
    CHECK(mprConfiguredCommandDepth(-1) == 32);
    CHECK(mprConfiguredCommandDepth(1) == 1);
    CHECK(mprConfiguredCommandDepth(4) == 4);
    CHECK(mprConfiguredCommandDepth(32) == 32);
    CHECK(mprConfiguredCommandDepth(33) == 32);
    CHECK(mprConfiguredCommandDepth(2147483647) == 32);
    CHECK(!mprIsAsyncSmid(0));
    CHECK(!mprIsAsyncSmid(31));
    CHECK(mprIsAsyncSmid(32));
    CHECK(mprIsAsyncSmid(63));
    CHECK(!mprIsAsyncSmid(64));
    CHECK(!mprIsAsyncSmid(UINT32_MAX));
    CHECK(mprAsyncCommandLimit(32, 128, 128, 144, 144) == 32);
    CHECK(mprAsyncCommandLimit(32, 33, 128, 144, 144) == 1);
    CHECK(mprAsyncCommandLimit(32, 128, 128, 10, 144) == 1);
    CHECK(mprAsyncCommandLimit(32, 128, 128, 144, 9) == 0);

    // Signed wide arithmetic is an independent oracle: narrow unsigned
    // subtraction in the production helper must not wrap below a reservation.
    const uint32_t allocations[] = {0, 1, 4, 32, UINT32_MAX};
    const uint32_t requests[] = {0, 31, 32, 33, 64, 128, UINT32_MAX};
    const uint32_t replies[] = {0, 9, 10, 40, 144, UINT32_MAX};
    for (uint32_t allocated : allocations)
      for (uint32_t frames : requests)
        for (uint32_t credit : requests)
          for (uint32_t post : replies)
            for (uint32_t free : replies) {
                int64_t expected = 32;
                const int64_t bounds[] = {
                    allocated, (int64_t)frames - 32, (int64_t)credit - 32,
                    (int64_t)post - 9, (int64_t)free - 9, 96
                };
                for (int64_t bound : bounds) expected = std::min(expected, bound);
                expected = std::max(INT64_C(0), expected);
                const uint32_t actual = mprAsyncCommandLimit(
                    allocated, frames, credit, post, free);
                CHECK(actual == (uint32_t)expected);
                CHECK(actual <= allocated && actual <= 32);
                if (actual) {
                    CHECK(32 + actual <= frames && 32 + actual <= credit);
                    CHECK(actual + 9 <= post && actual + 9 <= free);
                    CHECK(mprIsAsyncSmid(32 + actual - 1));
                    CHECK(32 + actual - 1 < 128);
                }
            }

    // Completion order must not turn successful chunks beyond a hole into
    // reportable bytes. Include a partially copied first failing read chunk.
    for (unsigned failed = 0; failed < 4; ++failed) {
        const uint64_t partials[] = {0, 1, 65535};
        for (uint64_t partial : partials) {
            std::array<unsigned, 4> order{{0, 1, 2, 3}};
            do {
                uint64_t sum = 0, firstError = UINT64_MAX;
                for (unsigned index : order) {
                    if (index == failed) {
                        sum += partial;
                        firstError = (uint64_t)index * 65536 + partial;
                    } else sum += 65536;
                }
                CHECK(mprCompletedPrefix(sum, firstError) ==
                      (uint64_t)failed * 65536 + partial);
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
    CHECK(mprCompletedPrefix(0, UINT64_MAX) == 0);
    CHECK(mprCompletedPrefix(UINT64_C(4) * 65536, UINT64_MAX) == 262144);
    CHECK(mprCompletedPrefix(65536, 131072) == 65536);
    CHECK(mprCompletedPrefix(UINT64_MAX, UINT64_MAX) == UINT64_MAX);
}

int main()
{
    performanceLimitTests();
    cdbTests();
    vpdAndRaidTests();
    senseTests();
    sglGoldenTests();
    sglRoundTrips();
    std::cout << "Protocol checks passed: " << checks << " assertions, 2000 deterministic SGL cases.\n";
}
