#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/storage/IOStorage.h>
#include <kern/task.h>
#include <kern/kern_types.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "MPRController.h"
#include "MPRScsi.h"
#include "MPRDmaSgl.h"
#include "MPI/mpi2_min.h"

enum {
    kMPRAsyncRequests = kMPRAsyncRequestCount,
    kMPRAsyncSlots = kMPRAsyncMaxCommands,
    kMPRAsyncBytes = kMPRAsyncTransferBytes,
    kMPRAsyncSegments = 32,
    kMPRChainStorageBytes = 4096, kMPRChainLimit = 4,
    kMPRRuntimeSmid = kMPRAsyncFirstSmid
};

struct MPRAsyncRequest {
    IOMemoryDescriptor *buffer;
    IOStorageCompletion completion;
    UInt64 sequence, turn, block, blocks, total, issued, completed, firstErrorOffset;
    UInt32 generation, targetGeneration, inFlight, blockSize;
    UInt16 devHandle;
    IOReturn status;
    bool occupied, initialized, prepared, finished, write, fua;
};

struct MPRAsyncWindow {
    IOBufferMemoryDescriptor *bounce, *chains;
    IODMACommand *dma, *bounceDma, *chainDma;
    UInt64 bounceAddress, chainAddress;
    MPRAsyncRequest *request;
    UInt64 offset, deadlineMs;
    UInt32 bytes;
    bool posted, direct, prepared, recoveryAttempted;
    bool bouncePrepared, chainsPrepared;
};

struct MPRAsyncState {
    IOLock *lock;
    thread_call_t call, completionCall;
    MPRAsyncRequest requests[kMPRAsyncRequests];
    MPRAsyncWindow windows[kMPRAsyncSlots];
    UInt64 sequence, turn, fence;
    UInt64 directBytes, copiedBytes;
    UInt32 outstanding, peak, mappedCommands, chainedCommands, bounceCommands;
    UInt32 mapFailures, peakCommands, heldCommands, nativePrpCommands;
    UInt32 requestedSlots, slotCapacity, slotLimit;
    bool directEnabled, dmaBlocked;
    bool scheduled, completionsScheduled, accepting, stopping, barrier, released;
};

static bool
mprFencePending(const MPRAsyncState *state)
{
    for (unsigned i = 0; i < kMPRAsyncRequests; ++i)
        if (state->requests[i].occupied && !state->requests[i].finished &&
            state->requests[i].sequence <= state->fence) return true;
    return false;
}

static MPRAsyncRequest *
mprNextAsync(MPRAsyncState *state)
{
    MPRAsyncRequest *chosen = NULL;
    for (unsigned i = 0; i < kMPRAsyncRequests; ++i) {
        MPRAsyncRequest &job = state->requests[i];
        if (!job.occupied || job.finished || job.status != kIOReturnSuccess ||
            (job.initialized && job.issued >= job.total) ||
            (state->barrier && job.sequence > state->fence)) continue;
        if (!chosen || job.turn < chosen->turn) chosen = &job;
    }
    return chosen;
}

/* The caller must prove this window is no longer owned by the IOC. In
 * particular, retaining a descriptor is not a substitute for its preparation
 * or for withholding the client's callback while direct DMA is possible. */
static IOReturn
mprDetachClient(MPRAsyncWindow &window, bool copyBack)
{
    if (!window.dma) return kIOReturnSuccess;
    IOReturn result = kIOReturnSuccess;
    if (window.prepared) {
        result = window.dma->complete(true, copyBack);
        window.prepared = false;
    }
    IOReturn cleared = window.dma->clearMemoryDescriptor(false);
    window.direct = false;
    return result == kIOReturnSuccess ? cleared : result;
}

static void
mprReleaseWindow(MPRAsyncWindow &window)
{
    /* Used only before posting, or after the controller proved DMA stopped. */
    if (window.dma) {
        (void)mprDetachClient(window, false);
        window.dma->release();
    }
    if (window.bounceDma) {
        window.bounceDma->clearMemoryDescriptor();
        window.bounceDma->release();
    }
    if (window.chainDma) {
        window.chainDma->clearMemoryDescriptor();
        window.chainDma->release();
    }
    if (window.bounce) {
        if (window.bouncePrepared) window.bounce->complete();
        window.bounce->release();
    }
    if (window.chains) {
        if (window.chainsPrepared) window.chains->complete();
        window.chains->release();
    }
    bzero(&window, sizeof(window));
}

static bool
mprSingleMapping(IODMACommand *dma, UInt32 bytes, UInt64 *address)
{
    if (!dma) return false;
    IODMACommand::Segment64 segment;
    UInt64 offset = 0;
    UInt32 count = 1;
    if (dma->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
        count != 1 || offset != bytes || segment.fLength != bytes ||
        segment.fIOVMAddr > UINT64_C(0x7fffffffffffffff) ||
        bytes - 1 > UINT64_C(0x7fffffffffffffff) - segment.fIOVMAddr)
        return false;
    *address = segment.fIOVMAddr;
    return true;
}

static bool
mprAllocateWindow(MPRAsyncWindow &window, IOMapper *mapper)
{
    /* Every window has a device-mapped fallback. It never aliases fDataBuffer
     * or the synchronous SMIDs, so a completed window may be refilled while
     * management and other asynchronous commands are still outstanding. */
    window.bounce = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        kMPRAsyncBytes, UINT64_C(0x00000000ffffffff));
    if (!window.bounce || !window.bounce->getBytesNoCopy()) return false;
    window.bouncePrepared = window.bounce->prepare() == kIOReturnSuccess;
    if (!window.bouncePrepared) return false;
    window.bounceDma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 63, kMPRAsyncBytes,
        IODMACommand::kMapped, kMPRAsyncBytes, 1, mapper);
    if (!window.bounceDma ||
        window.bounceDma->setMemoryDescriptor(window.bounce) != kIOReturnSuccess ||
        !mprSingleMapping(window.bounceDma, kMPRAsyncBytes, &window.bounceAddress))
        return false;

    /* No whole-descriptor maximum here: prepare(offset,length) below always
     * bounds the mapping to one chunk, even for multi-megabyte client I/O. */
    window.dma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 63, kMPRAsyncBytes,
        IODMACommand::kMapped, 0, 1, mapper);
    window.chains = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionOut | kIOMemoryPhysicallyContiguous,
        kMPRChainStorageBytes, 4096);
    if (window.chains)
        window.chainsPrepared = window.chains->prepare() == kIOReturnSuccess;
    window.chainDma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 63, kMPRChainStorageBytes,
        IODMACommand::kMapped, kMPRChainStorageBytes, 4096, mapper);
    bool chainsReady = window.chains && window.chainsPrepared &&
        window.chains->getBytesNoCopy() && window.chainDma &&
        window.chainDma->setMemoryDescriptor(window.chains) == kIOReturnSuccess &&
        mprSingleMapping(window.chainDma, kMPRChainStorageBytes, &window.chainAddress) &&
        (window.chainAddress & 4095) == 0;
    if (!chainsReady) {
        if (window.chainDma) {
            window.chainDma->clearMemoryDescriptor();
            window.chainDma->release();
            window.chainDma = NULL;
        }
        if (window.chains) {
            if (window.chainsPrepared) window.chains->complete();
            window.chains->release();
            window.chains = NULL;
        }
        window.chainsPrepared = false;
        window.chainAddress = 0;
        /* Inline direct SGLs and the one-element fallback remain usable. */
    }
    return true;
}

static bool
mprMapClient(MPRAsyncWindow &window, MPRAsyncRequest &job,
             UInt8 *frame, UInt32 frameBytes, UInt32 chainFrameBytes,
             UInt32 maxChainDepth, bool nvme, MPRSglResult *result)
{
    if (!window.dma ||
        window.dma->setMemoryDescriptor(job.buffer, false) != kIOReturnSuccess)
        return false;
    IOReturn prepared = window.dma->prepare(window.offset, window.bytes, true,
                                            job.write);
    if (prepared != kIOReturnSuccess) {
        /* Some prepare failures leave a partial active preparation. Suppress
         * copy-back: no read has been submitted and bounce data is not valid. */
        (void)window.dma->complete(true, false);
        (void)window.dma->clearMemoryDescriptor(false);
        return false;
    }
    window.prepared = true;
    IODMACommand::Segment64 raw[kMPRAsyncSegments];
    MPRDmaSegment segments[kMPRAsyncSegments];
    /* SDK: this cursor is relative to the offset passed to prepare(). */
    UInt64 offset = 0;
    UInt32 count = kMPRAsyncSegments;
    bool mapped = window.dma->gen64IOVMSegments(&offset, raw, &count) ==
                  kIOReturnSuccess && offset == window.bytes && count != 0 &&
                  count <= kMPRAsyncSegments;
    if (mapped) {
        for (UInt32 i = 0; i < count; ++i) {
            if (!raw[i].fLength || raw[i].fLength > UINT32_MAX) {
                mapped = false;
                break;
            }
            segments[i].address = raw[i].fIOVMAddr;
            segments[i].length = (UInt32)raw[i].fLength;
        }
    }
    UInt32 depth = maxChainDepth < kMPRChainLimit ? maxChainDepth : kMPRChainLimit;
    if (mapped && nvme) mapped = mprBuildNvmeSgl(frame, frameBytes,
        MPR_SCSIIO_SGE_DWORDS * 4, segments, count, window.bytes,
        window.chains ? (UInt8 *)window.chains->getBytesNoCopy() : NULL,
        window.chainAddress, kMPRChainStorageBytes, result);
    else if (mapped) mapped = mprBuildIeeeSgl(frame, frameBytes,
        MPR_SCSIIO_SGE_DWORDS * 4, segments, count, window.bytes,
        window.chains ? (UInt8 *)window.chains->getBytesNoCopy() : NULL,
        window.chainAddress, chainFrameBytes, depth,
        kMPRChainStorageBytes, result);
    if (mapped && result->chainsUsed)
        mapped = window.chainDma &&
                 window.chainDma->synchronize(kIODirectionOut) == kIOReturnSuccess;
    if (!mapped) {
        (void)mprDetachClient(window, false);
        return false;
    }
    window.direct = true;
    return true;
}

bool
MPRController::initializeAsync(void)
{
    if (fAsync != NULL) return !fAsync->released;
    MPRAsyncState *state = (MPRAsyncState *)IOMalloc(sizeof(MPRAsyncState));
    if (!state) return false;
    bzero(state, sizeof(*state));
    fAsync = state;
    state->lock = IOLockAlloc();
    state->call = thread_call_allocate_with_options(asyncCall, this,
        THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    state->completionCall = thread_call_allocate_with_options(asyncCompletionCall,
        this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!state->lock || !state->call || !state->completionCall) {
        releaseAsync(true);
        destroyAsyncState();
        return false;
    }
    int requested = 0, direct = 1;
    (void)PE_parse_boot_argn("mpr_io_depth", &requested, sizeof(requested));
    (void)PE_parse_boot_argn("mpr_direct_dma", &direct, sizeof(direct));
    state->requestedSlots = mprConfiguredCommandDepth(requested);
    state->directEnabled = direct != 0;
    IOMapper *mapper = IOMapper::copyMapperForDevice(fPCIDevice);
    for (unsigned i = 0; i < state->requestedSlots; ++i) {
        if (!mprAllocateWindow(state->windows[i], mapper)) {
            mprReleaseWindow(state->windows[i]);
            break; /* Resource pressure reduces depth, never removes bounds. */
        }
        ++state->slotCapacity;
    }
    if (mapper) mapper->release();
    if (state->slotCapacity == 0) {
        releaseAsync(true);
        destroyAsyncState();
        return false;
    }
    state->accepting = true;
    MPR_DEBUG_PROP("MPRAsyncRequestLimit", (unsigned long long)kMPRAsyncRequests, 32);
    MPR_DEBUG_PROP("MPRAsyncRequestedDepth", (unsigned long long)state->requestedSlots, 32);
    MPR_DEBUG_PROP("MPRAsyncAllocatedDepth", (unsigned long long)state->slotCapacity, 32);
    MPR_DEBUG_PROP("MPRDirectDMAEnabled", state->directEnabled);
    /* IOC Facts do not exist yet. initializeQueues() sets the effective limit. */
    return true;
}

bool
MPRController::configureAsyncLimits(void)
{
    MPRAsyncState *state = fAsync;
    if (!state || state->released) return false;
    state->slotLimit = mprAsyncCommandLimit(state->slotCapacity, fRequestCount,
                                           fRequestCredit, fPqDepth, fFqDepth);
    MPR_DEBUG_PROP("MPRAsyncCommandLimit", (unsigned long long)state->slotLimit, 32);
    MPR_DEBUG_PROP("MPRAsyncFirstSmid", (unsigned long long)kMPRRuntimeSmid, 32);
    return state->slotLimit != 0;
}

void
MPRController::releaseAsync(bool dmaStopped)
{
    MPRAsyncState *state = fAsync;
    if (!state || state->released) return;
    if (state->call) {
        thread_call_cancel_wait(state->call);
        thread_call_free(state->call);
        state->call = NULL;
    }
    if (state->completionCall) {
        thread_call_cancel_wait(state->completionCall);
        thread_call_free(state->completionCall);
        state->completionCall = NULL;
    }
    if (dmaStopped) {
        /* stopAsync() could not return a direct-DMA buffer before stop()
         * quiesced PCI. The proof now permits unmapping and exactly one error
         * completion. This runs outside fIOLock and after IRQ teardown. */
        for (unsigned i = 0; i < kMPRAsyncSlots; ++i) {
            MPRAsyncWindow &window = state->windows[i];
            if (window.posted && window.request) {
                MPRAsyncRequest *job = window.request;
                (void)mprDetachClient(window, false);
                job->status = kIOReturnNoMedia;
                if (window.offset < job->firstErrorOffset)
                    job->firstErrorOffset = window.offset;
                --job->inFlight;
            }
            mprReleaseWindow(window);
        }
        if (state->lock) {
            IOLockLock(state->lock);
            for (unsigned i = 0; i < kMPRAsyncRequests; ++i) {
                MPRAsyncRequest &job = state->requests[i];
                if (job.occupied && !job.finished && !job.inFlight) {
                    if (job.status == kIOReturnSuccess) job.status = kIOReturnNoMedia;
                    job.completed = mprCompletedPrefix(job.completed, job.firstErrorOffset);
                    job.finished = true;
                }
            }
            IOLockWakeup(state->lock, &state->outstanding, false);
            IOLockUnlock(state->lock);
            asyncCompletions();
        }
    }
    /* On unproved stop, retain resident mappings AND any client preparation,
     * job, buffer and controller reference. Never "finish" a direct request
     * while a malfunctioning IOC could still DMA into recycled client pages. */
    state->released = true;
}

void
MPRController::destroyAsyncState(void)
{
    MPRAsyncState *state = fAsync;
    if (!state) return;
    releaseAsync(false);
    /* Accepted held requests retain the controller, so normally free() cannot
     * reach this condition. Keep the ownership records even on defensive exit. */
    if (state->outstanding != 0) return;
    if (state->lock) IOLockFree(state->lock);
    IOFree(state, sizeof(*state));
    fAsync = NULL;
}

void
MPRController::kickAsync(void)
{
    MPRAsyncState *state = fAsync;
    if (!state || !state->lock) return;
    IOLockLock(state->lock);
    if (state->outstanding && !state->scheduled && !state->stopping &&
        !state->released && state->call) {
        state->scheduled = true;
        thread_call_enter(state->call);
    }
    IOLockUnlock(state->lock);
}

void
MPRController::publishAsyncStats(void)
{
    MPRAsyncState *state = fAsync;
    if (!state || !state->lock || !fIOLock || !IOLockTryLock(fIOLock)) return;
    UInt64 directBytes = state->directBytes, copiedBytes = state->copiedBytes;
    UInt32 peakCommands = state->peakCommands, direct = state->mappedCommands;
    UInt32 chained = state->chainedCommands, bounce = state->bounceCommands;
    UInt32 failures = state->mapFailures, held = state->heldCommands;
    UInt32 nativePrp = state->nativePrpCommands;
    UInt32 nvmeDsm = fNvmeDsmCommands;
    IOLockLock(state->lock);
    UInt32 peak = state->peak, outstanding = state->outstanding;
    IOLockUnlock(state->lock);
    IOLockUnlock(fIOLock);
    /* Once per watchdog tick, not once per chunk or scheduler sweep. */
    MPR_DEBUG_PROP("MPRAsyncPeakRequests", (unsigned long long)peak, 32);
    MPR_DEBUG_PROP("MPRAsyncOutstanding", (unsigned long long)outstanding, 32);
    MPR_DEBUG_PROP("MPRAsyncPeakCommands", (unsigned long long)peakCommands, 32);
    MPR_DEBUG_PROP("MPRMappedCommands", (unsigned long long)direct, 32);
    MPR_DEBUG_PROP("MPRDirectCommands", (unsigned long long)direct, 32);
    MPR_DEBUG_PROP("MPRDirectBytes", (unsigned long long)directBytes, 64);
    MPR_DEBUG_PROP("MPRCopiedBytes", (unsigned long long)copiedBytes, 64);
    MPR_DEBUG_PROP("MPRChainedCommands", (unsigned long long)chained, 32);
    MPR_DEBUG_PROP("MPRBounceCommands", (unsigned long long)bounce, 32);
    MPR_DEBUG_PROP("MPRDirectMapFailures", (unsigned long long)failures, 32);
    MPR_DEBUG_PROP("MPRAsyncHeldDMA", (unsigned long long)held, 32);
    MPR_DEBUG_PROP("MPRNativePrpCommands", (unsigned long long)nativePrp, 32);
    MPR_DEBUG_PROP("MPRNvmeDsmCommands", (unsigned long long)nvmeDsm, 32);
}

void
MPRController::setAsyncAcceptance(bool enabled)
{
    if (!fAsync) return;
    IOLockLock(fAsync->lock);
    fAsync->accepting = enabled && !fAsync->stopping && !fAsync->released;
    IOLockUnlock(fAsync->lock);
}

IOReturn
MPRController::submitAsyncBlocks(bool write, IOMemoryDescriptor *buffer,
                                 UInt64 block, UInt64 nblks, bool fua,
                                 const IOStorageCompletion *completion,
                                 UInt16 devHandle)
{
    if (!buffer || !completion || !completion->action) return kIOReturnBadArgument;
    MPRAsyncState *state = fAsync;
    if (!state) return kIOReturnNotReady;
    const UInt32 generation = fMediaGeneration;
    IOLockLock(state->lock);
    if (!state->accepting || state->stopping || state->released) {
        IOLockUnlock(state->lock);
        return kIOReturnNotReady;
    }
    MPRAsyncRequest *job = NULL;
    for (unsigned i = 0; i < kMPRAsyncRequests; ++i)
        if (!state->requests[i].occupied) { job = &state->requests[i]; break; }
    if (!job || state->sequence == UINT64_MAX || state->turn == UINT64_MAX) {
        IOLockUnlock(state->lock);
        return kIOReturnNoResources;
    }
    bzero(job, sizeof(*job));
    job->firstErrorOffset = UINT64_MAX;
    job->buffer = buffer;
    job->completion = *completion;
    job->block = block;
    job->blocks = nblks;
    job->generation = generation;
    /* The target's own generation is what rejects a request queued for a disk
     * that has since been removed or replaced behind the same handle. */
    MPRTargetSlot *slot = findTarget(devHandle);
    job->targetGeneration = slot != NULL ? slot->generation : 0;
    job->write = write;
    job->fua = fua;
    job->devHandle = devHandle;
    job->occupied = true;
    job->status = kIOReturnSuccess;
    job->sequence = ++state->sequence;
    job->turn = ++state->turn;
    buffer->retain();
    retain(); /* Each accepted request owns its controller through callback. */
    ++state->outstanding;
    if (state->outstanding > state->peak) state->peak = state->outstanding;
    if (!state->scheduled) {
        state->scheduled = true;
        thread_call_enter(state->call);
    }
    IOLockUnlock(state->lock);
    notifyReplyWaiters();
    return kIOReturnSuccess;
}

IOReturn
MPRController::beginAsyncBarrier(void)
{
    MPRAsyncState *state = fAsync;
    if (!state) return kIOReturnSuccess;
    IOLockLock(state->lock);
    while (state->barrier && !state->stopping)
        IOLockSleep(state->lock, &state->barrier, THREAD_UNINT);
    if (state->stopping || state->released || state->dmaBlocked) {
        IOLockUnlock(state->lock);
        return kIOReturnNotReady;
    }
    state->barrier = true;
    state->fence = state->sequence;
    /* Newer requests remain queued. Only commands accepted before this
     * fence must retire before the ordered cache/UNMAP operation starts. */
    while (mprFencePending(state) && !state->stopping && !state->dmaBlocked) {
        if (!state->scheduled) {
            state->scheduled = true;
            thread_call_enter(state->call);
        }
        IOLockSleep(state->lock, &state->outstanding, THREAD_UNINT);
    }
    if (state->stopping || state->released || state->dmaBlocked) {
        state->barrier = false;
        IOLockWakeup(state->lock, &state->barrier, false);
        IOLockUnlock(state->lock);
        return kIOReturnNotReady;
    }
    IOLockUnlock(state->lock);
    notifyReplyWaiters();
    return kIOReturnSuccess;
}

void
MPRController::endAsyncBarrier(void)
{
    MPRAsyncState *state = fAsync;
    if (!state) return;
    IOLockLock(state->lock);
    state->barrier = false;
    IOLockWakeup(state->lock, &state->barrier, false);
    if (state->outstanding && !state->scheduled && !state->stopping) {
        state->scheduled = true;
        thread_call_enter(state->call);
    }
    IOLockUnlock(state->lock);
    notifyReplyWaiters();
}

IOReturn
MPRController::synchronizeCache(UInt64 block, UInt64 nblks, bool barrier,
                                UInt16 handle)
{
    /* The fence orders this client's earlier writes; the generation proves the
     * command still names the medium those writes were accepted for. */
    MPRTargetSlot *slot = findTarget(handle);
    const UInt32 generation = slot != NULL ? slot->generation : 0;
    IOReturn result = beginAsyncBarrier();
    if (result != kIOReturnSuccess) return result;
    result = synchronizeCacheNow(block, nblks, barrier, generation, handle);
    endAsyncBarrier();
    return result;
}

IOReturn
MPRController::unmapBlocks(const IOBlockStorageDeviceExtent *extents,
                           UInt32 extentsCount, UInt16 handle)
{
    MPRTargetSlot *slot = findTarget(handle);
    const UInt32 generation = slot != NULL ? slot->generation : 0;
    IOReturn result = beginAsyncBarrier();
    if (result != kIOReturnSuccess) return result;
    result = unmapBlocksNow(extents, extentsCount, generation, handle);
    endAsyncBarrier();
    return result;
}

void
MPRController::asyncCall(thread_call_param_t param0, thread_call_param_t)
{
    MPRController *controller = (MPRController *)param0;
    if (controller) controller->asyncWorker();
}

void
MPRController::asyncWorker(void)
{
    MPRAsyncState *state = fAsync;
    if (!state || state->released) return;
    retain();
    for (;;) {
        /* Capture before checking completion slots: an interrupt between the
         * check and the sleep must change this ticket and prevent that sleep. */
        UInt32 wakeSequence = replyWakeSequence();
        bool progress = false;
        bool waiting = false;
        UInt64 earliest = UINT64_MAX;
        UInt32 held = 0, active = 0;
        IOLockLock(fIOLock);
        if (!fStopping && !fSuspended && fRegs && !fPowerQuiesced)
            processReplyQueue();

        /* Reap every ready command, not the first array entry. A slow disk no
         * longer holds other disks' completed windows until its own timeout. */
        for (unsigned index = 0; index < state->slotCapacity; ++index) {
            MPRAsyncWindow &window = state->windows[index];
            if (!window.posted) continue;
            MPRAsyncRequest *job = window.request;
            const UInt32 smid = kMPRRuntimeSmid + index;
            IOSimpleLockLock(fReplyLock);
            bool ready = fReplySlots[smid].valid != 0;
            bool safe = fCommandOwners[smid].state != kMPRCommandPosted &&
                        fCommandOwners[smid].state != kMPRCommandQuarantined;
            IOSimpleLockUnlock(fReplyLock);
            const bool expired = uptimeMilliseconds() >= window.deadlineMs;
            if (!ready && !safe && !fStopping && !fProtocolRecoveryPending &&
                !expired) continue;
            if (window.recoveryAttempted && !safe) continue;

            IOReturn result = kIOReturnNoMedia;
            bool complete = false;
            if (!fStopping && !fSuspended && !window.recoveryAttempted) {
                /* Zero additional wait: readiness or the original post-time
                 * deadline caused this call. Recovery retains its old policy. */
                complete = scsiWaitChunk(smid, window.bytes, &result, NULL, NULL, 0);
            } else if (safe) {
                UInt8 orphan[64], kind = 0;
                (void)takeReplySlot(smid, orphan, sizeof(orphan), &kind);
            }
            if (!commandDmaSafe(smid)) {
                /* A failed reset must not complete a direct request. Keep it
                 * attached; the watchdog retries reaping after late replies or
                 * a subsequent proved controller stop, without reissuing I/O. */
                window.recoveryAttempted = true;
                if (job->status == kIOReturnSuccess)
                    job->status = result == kIOReturnSuccess ? kIOReturnIOError : result;
                if (window.offset < job->firstErrorOffset)
                    job->firstErrorOffset = window.offset;
                if (window.direct) continue;
                /* Resident fallback DMA cannot touch the client's pages. */
                complete = false;
            }
            MPRTargetSlot *slot = findTarget(job->devHandle);
            if (complete && (job->generation != fMediaGeneration || !slot ||
                !slot->present || slot->offlinePending ||
                job->targetGeneration != slot->generation || mediaRemovalPending())) {
                complete = false;
                result = kIOReturnNoMedia;
            }
            IOByteCount copied = 0;
            if (window.direct) {
                IOReturn unmapped = mprDetachClient(window, complete && !job->write);
                if (unmapped != kIOReturnSuccess) {
                    complete = false;
                    result = unmapped;
                }
            } else if (complete && !job->write) {
                if (window.bounceDma->synchronize(kIODirectionIn) != kIOReturnSuccess) {
                    complete = false;
                    result = kIOReturnIOError;
                } else {
                    copied = job->buffer->writeBytes((IOByteCount)window.offset,
                        window.bounce->getBytesNoCopy(), window.bytes);
                    state->copiedBytes += copied;
                    if (copied != window.bytes) {
                        complete = false;
                        result = copied < window.bytes ? kIOReturnUnderrun : kIOReturnOverrun;
                    }
                }
            }
            if (!complete) {
                if (job->status == kIOReturnSuccess) {
                    job->status = result == kIOReturnSuccess ? kIOReturnIOError : result;
                    if (slot) ++slot->failureCount;
                }
                UInt64 prefix = window.offset;
                if (!job->write && copied < window.bytes) {
                    prefix += copied;
                    job->completed += copied;
                }
                if (prefix < job->firstErrorOffset) job->firstErrorOffset = prefix;
            } else {
                job->completed += window.bytes;
                if (job->write) {
                    ++fWriteCount;
                    fWriteBytes += window.bytes;
                    fLastWriteLba = job->block + window.offset / job->blockSize;
                    fLastWriteSmid = smid;
                    if (slot) ++slot->writeCount;
                }
            }
            --job->inFlight;
            window.posted = false;
            window.request = NULL;
            window.recoveryAttempted = false;
            progress = true;
        }

        /* Refill only truly free windows. Each has a separate DMA command,
         * chain area, fallback buffer and runtime SMID. */
        for (unsigned index = 0; index < state->slotLimit; ++index) {
            MPRAsyncWindow &window = state->windows[index];
            const UInt32 smid = kMPRRuntimeSmid + index;
            if (window.posted || !commandDmaSafe(smid)) continue;
            UInt8 orphan[64], orphanKind = 0;
            (void)takeReplySlot(smid, orphan, sizeof(orphan), &orphanKind);
            IOLockLock(state->lock);
            MPRAsyncRequest *job = mprNextAsync(state);
            if (job) job->turn = ++state->turn;
            IOLockUnlock(state->lock);
            if (!job) continue;
            MPRTargetSlot *slot = findTarget(job->devHandle);
            const UInt32 blockSize = slot ? slot->blockSize : 0;
            const UInt64 capacityBlocks = slot ? slot->capacityBlocks : 0;
            if (fStopping || fSuspended || fDeviceOffline || fProtocolRecoveryPending ||
                !slot || !slot->present || !slot->probeValid || slot->offlinePending ||
                job->targetGeneration != slot->generation ||
                job->generation != fMediaGeneration || mediaRemovalPending()) {
                job->status = kIOReturnNoMedia;
                progress = true;
                continue;
            }
            if (!job->initialized) {
                job->initialized = true;
                if (!blockSize || blockSize > kMPRAsyncBytes || !capacityBlocks ||
                    job->blocks > UINT64_MAX / blockSize ||
                    (job->blocks && (job->block >= capacityBlocks ||
                     job->blocks > capacityBlocks - job->block))) {
                    job->status = kIOReturnBadArgument;
                    progress = true;
                    continue;
                }
                job->total = job->blocks * blockSize;
                if (job->buffer->getLength() < job->total) {
                    job->status = kIOReturnBadArgument;
                    progress = true;
                    continue;
                }
                if (!job->total) { progress = true; continue; }
                job->status = job->buffer->prepare();
                if (job->status != kIOReturnSuccess) { progress = true; continue; }
                job->prepared = true;
                job->blockSize = blockSize;
            }
            /* An in-place re-probe must never change an already accepted
             * request's geometry, even if a future caller forgets the generation. */
            if (blockSize != job->blockSize || !capacityBlocks ||
                job->block >= capacityBlocks ||
                job->blocks > capacityBlocks - job->block) {
                job->status = kIOReturnNoMedia;
                progress = true;
                continue;
            }
            UInt64 remaining = job->total - job->issued;
            UInt32 limit = kMPRAsyncBytes;
            const bool nvme = slot->kind == kMPRTargetNvme;
            if (nvme) {
                if (slot->maxTransferBytes && slot->maxTransferBytes < limit)
                    limit = slot->maxTransferBytes;
                if (!window.chains && limit > 16384) limit = 16384;
            }
            UInt32 blocks = (UInt32)((remaining < limit ? remaining : limit) / blockSize);
            if (!blocks || !fRequestBuffer || !fRequestBuffer->getBytesNoCopy() ||
                fRequestFrameSize < MPR_SCSIIO_SGE_DWORDS * 4 + 16 ||
                !prepareScsiSense(smid)) {
                job->status = kIOReturnNotReady;
                progress = true;
                continue;
            }
            window.offset = job->issued;
            window.bytes = blocks * blockSize;
            window.direct = false;
            window.recoveryAttempted = false;
            UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() +
                           smid * fRequestFrameSize;
            bzero(frame, fRequestFrameSize);
            MPRScsiIoRequest *request = (MPRScsiIoRequest *)frame;
            UInt8 cdb[16];
            UInt32 cdbLength = mprBuildReadWriteCdb(cdb, job->write,
                job->block + window.offset / blockSize, blocks,
                job->fua && slot->nativeFua);
            if (!cdbLength) {
                cancelUnpostedCommand(smid);
                job->status = kIOReturnBadArgument;
                progress = true;
                continue;
            }
            request->DevHandle = job->devHandle;
            request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
            request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
            request->SenseBufferLength = 96;
            request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
            request->DataLength = window.bytes;
            request->IoFlags = (UInt16)(cdbLength | (slot->fastPath ?
                                           MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
            request->Control = job->write ? MPI2_SCSIIO_CONTROL_WRITE :
                                            MPI2_SCSIIO_CONTROL_READ;
            memcpy(request->CDB, cdb, cdbLength);
            MPRSglResult sgl = {};
            if (state->directEnabled && mprMapClient(window, *job, frame,
                    fRequestFrameSize, fChainFrameBytes, fMaxChainDepth, nvme, &sgl)) {
                request->ChainOffset = sgl.requestChainOffset;
            } else {
                if (state->directEnabled) ++state->mapFailures;
                request->ChainOffset = 0;
                if (nvme) {
                    MPRDmaSegment segment = {window.bounceAddress, window.bytes};
                    if (!mprBuildNvmeSgl(frame, fRequestFrameSize,
                        MPR_SCSIIO_SGE_DWORDS * 4, &segment, 1, window.bytes,
                        window.chains ? (UInt8 *)window.chains->getBytesNoCopy() : NULL,
                        window.chainAddress, kMPRChainStorageBytes, &sgl) ||
                        (sgl.chainsUsed && (!window.chainDma ||
                         window.chainDma->synchronize(kIODirectionOut) != kIOReturnSuccess))) {
                        cancelUnpostedCommand(smid);
                        job->status = kIOReturnNoResources;
                        MPR_DEBUG_LOG("MPR: NVMe mapped fallback/PRP unavailable handle=0x%04x bytes=%u\n",
                                      job->devHandle, (unsigned)window.bytes);
                        progress = true;
                        continue;
                    }
                } else {
                    mprSglElement(frame + MPR_SCSIIO_SGE_DWORDS * 4,
                        window.bounceAddress, window.bytes, 0, 0x40);
                }
                if (job->write) {
                    IOByteCount copied = job->buffer->readBytes(
                        (IOByteCount)window.offset, window.bounce->getBytesNoCopy(),
                        window.bytes);
                    state->copiedBytes += copied;
                    if (copied != window.bytes ||
                        window.bounceDma->synchronize(kIODirectionOut) != kIOReturnSuccess) {
                        cancelUnpostedCommand(smid);
                        job->status = copied != window.bytes ?
                                      kIOReturnUnderrun : kIOReturnIOError;
                        progress = true;
                        continue;
                    }
                }
            }
            window.deadlineMs = uptimeMilliseconds() + kMPRAsyncTimeoutMs;
            if (!postScsiIo(smid)) {
                (void)mprDetachClient(window, false); // never submitted
                cancelUnpostedCommand(smid);
                job->status = kIOReturnNoMedia;
                progress = true;
                continue;
            }
            window.request = job;
            window.posted = true;
            if (nvme && sgl.chainsUsed && !sgl.requestChainOffset) ++state->nativePrpCommands;
            if (window.direct) {
                ++state->mappedCommands;
                state->directBytes += window.bytes;
                if (sgl.chainsUsed) ++state->chainedCommands;
            } else ++state->bounceCommands;
            ++fIoCount;
            job->issued += window.bytes;
            ++job->inFlight;
            progress = true;
        }

        for (unsigned index = 0; index < state->slotCapacity; ++index) {
            const MPRAsyncWindow &window = state->windows[index];
            if (!window.posted) continue;
            ++active;
            if (window.recoveryAttempted) { ++held; continue; }
            waiting = true;
            if (window.deadlineMs < earliest) earliest = window.deadlineMs;
        }
        if (active > state->peakCommands) state->peakCommands = active;
        state->heldCommands = held;
        for (unsigned i = 0; i < kMPRAsyncRequests; ++i) {
            IOLockLock(state->lock);
            MPRAsyncRequest &job = state->requests[i];
            bool eligible = job.occupied && !job.finished && !job.inFlight;
            bool beforeFence = !state->barrier || job.sequence <= state->fence;
            IOLockUnlock(state->lock);
            if (!eligible) continue;
            if (job.status == kIOReturnSuccess && beforeFence &&
                (fStopping || fDeviceOffline || fProtocolRecoveryPending ||
                 fSuspended || !state->slotLimit))
                job.status = kIOReturnNoMedia;
            if (job.status != kIOReturnSuccess ||
                (job.initialized && job.issued == job.total)) {
                if (job.status == kIOReturnSuccess && job.write && job.fua && job.total) {
                    MPRTargetSlot *flushSlot = findTarget(job.devHandle);
                    if (!flushSlot || job.generation != fMediaGeneration ||
                        job.targetGeneration != flushSlot->generation ||
                        !flushSlot->present || !flushSlot->probeValid ||
                        flushSlot->offlinePending || mediaRemovalPending()) {
                        job.status = kIOReturnNoMedia;
                    } else if (!flushSlot->nativeFua) {
                        job.status = selectTarget(flushSlot->handle) ?
                            scsiSynchronizeCache(0, 0, 29, true) : kIOReturnNoMedia;
                    } // Native FUA was already set on each WRITE CDB.
                }
                job.completed = mprCompletedPrefix(job.completed, job.firstErrorOffset);
                IOLockLock(state->lock);
                job.finished = true;
                IOLockWakeup(state->lock, &state->outstanding, false);
                IOLockUnlock(state->lock);
                progress = true;
            }
        }
        IOLockUnlock(fIOLock);

        IOLockLock(state->lock);
        state->dmaBlocked = held != 0;
        bool completions = false;
        for (unsigned i = 0; i < kMPRAsyncRequests; ++i)
            if (state->requests[i].occupied && state->requests[i].finished)
                completions = true;
        if (completions && !state->completionsScheduled && state->completionCall) {
            state->completionsScheduled = true;
            thread_call_enter(state->completionCall);
        }
        bool queued = mprNextAsync(state) != NULL;
        /* Held DMA needs an external late reply/reset, not a busy retry loop.
         * The watchdog kicks us once per second while ownership is retained. */
        bool more = waiting || queued;
        if (!more) state->scheduled = false;
        IOLockWakeup(state->lock, &state->outstanding, false);
        IOLockUnlock(state->lock);
        if (!more) break;
        if (!progress) {
            UInt64 now = uptimeMilliseconds();
            UInt32 sleepMs = earliest <= now ? 0 :
                (UInt32)(earliest - now < 20 ? earliest - now : 20);
            waitForReplyActivity(wakeSequence, sleepMs);
        }
    }
    release();
}

void
MPRController::asyncCompletionCall(thread_call_param_t param0, thread_call_param_t)
{
    MPRController *controller = (MPRController *)param0;
    if (controller) controller->asyncCompletions();
}

void
MPRController::asyncCompletions(void)
{
    MPRAsyncState *state = fAsync;
    if (!state) return;
    retain();
    for (;;) {
        IOLockLock(state->lock);
        MPRAsyncRequest *job = NULL;
        for (unsigned i = 0; i < kMPRAsyncRequests; ++i)
            if (state->requests[i].occupied && state->requests[i].finished) {
                job = &state->requests[i];
                break;
            }
        if (!job) {
            state->completionsScheduled = false;
            IOLockUnlock(state->lock);
            break;
        }
        MPRAsyncRequest finished = *job;
        bzero(job, sizeof(*job));
        --state->outstanding;
        IOLockWakeup(state->lock, &state->outstanding, false);
        IOLockUnlock(state->lock);
        /* A separate callback context is essential: a client's callback may
         * issue a synchronous flush and wait for the I/O worker's fence. */
        if (finished.prepared) finished.buffer->complete();
        finished.completion.action(finished.completion.target,
            finished.completion.parameter, finished.status, finished.completed);
        finished.buffer->release();
        release();
    }
    release();
}

void
MPRController::stopAsync(void)
{
    MPRAsyncState *state = fAsync;
    if (!state || state->released) return;
    IOLockLock(state->lock);
    state->accepting = false;
    state->stopping = true;
    state->barrier = false;
    IOLockWakeup(state->lock, &state->barrier, false);
    IOLockWakeup(state->lock, &state->outstanding, false);
    IOLockUnlock(state->lock);
    /* Caller has already closed fStopping. Waiting takes no transport lock.
     * A cancelled queued call still leaves accepted requests to finish. */
    if (state->call) thread_call_cancel_wait(state->call);
    asyncWorker();
    if (state->completionCall) thread_call_cancel_wait(state->completionCall);
    asyncCompletions();
}
