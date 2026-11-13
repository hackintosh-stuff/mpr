#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/storage/IOStorage.h>
#include <kern/task.h>
#include <kern/kern_types.h>
#include "MPRController.h"
#include "MPRScsi.h"
#include "MPRDmaSgl.h"
#include "MPI/mpi2_min.h"

enum {
    kMPRAsyncRequests = 32, kMPRAsyncSlots = 4,
    kMPRAsyncBytes = 65536, kMPRAsyncSegments = 32,
    kMPRChainStorageBytes = 4096, kMPRChainLimit = 4,
    kMPRRuntimeSmid = 10
};

struct MPRAsyncRequest {
    IOMemoryDescriptor *buffer;
    IOStorageCompletion completion;
    UInt64 sequence, turn, block, blocks, total, issued, completed;
    UInt32 generation, targetGeneration, inFlight, blockSize;
    UInt16 devHandle;
    IOReturn status;
    bool occupied, initialized, prepared, finished, write, fua;
};

struct MPRAsyncWindow {
    IOBufferMemoryDescriptor *buffer, *chains;
    IODMACommand *dma, *chainDma;
    UInt64 chainAddress;
    MPRAsyncRequest *request;
    UInt64 offset;
    UInt32 bytes;
    bool posted, mapped;
};

struct MPRAsyncState {
    IOLock *lock;
    thread_call_t call, completionCall;
    MPRAsyncRequest requests[kMPRAsyncRequests];
    MPRAsyncWindow windows[kMPRAsyncSlots];
    UInt64 sequence, turn, fence;
    UInt32 outstanding, peak, mappedCommands, chainedCommands, bounceCommands;
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
    state->accepting = true;
    /* The controller never DMA-writes a client's buffer. A failed IOC that
     * cannot be proven quiescent can retain these bounded resident windows
     * while every accepted client request still completes exactly once. */
    IOMapper *mapper = IOMapper::copyMapperForDevice(fPCIDevice);
    for (unsigned i = 0; i < kMPRAsyncSlots; ++i) {
        MPRAsyncWindow &window = state->windows[i];
        window.buffer = IOBufferMemoryDescriptor::inTaskWithOptions(
            kernel_task, kIODirectionInOut, kMPRAsyncBytes, 4096);
        window.chains = IOBufferMemoryDescriptor::inTaskWithOptions(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
            kMPRChainStorageBytes, 4096);
        window.dma = IODMACommand::withSpecification(
            kIODMACommandOutputHost64, 63, kMPRAsyncBytes,
            IODMACommand::kMapped, kMPRAsyncBytes, 1, mapper);
        window.chainDma = IODMACommand::withSpecification(
            kIODMACommandOutputHost64, 63, kMPRChainStorageBytes,
            IODMACommand::kMapped, kMPRChainStorageBytes, 16, mapper);
        bool mapped = window.buffer && window.chains && window.dma &&
                      window.chainDma &&
            window.dma->setMemoryDescriptor(window.buffer) == kIOReturnSuccess &&
            window.chainDma->setMemoryDescriptor(window.chains) == kIOReturnSuccess;
        if (mapped) {
            IODMACommand::Segment64 segment;
            UInt64 offset = 0;
            UInt32 count = 1;
            mapped = window.chainDma->gen64IOVMSegments(&offset, &segment,
                                                       &count) == kIOReturnSuccess &&
                     count == 1 && offset == kMPRChainStorageBytes &&
                     segment.fLength == kMPRChainStorageBytes &&
                     (segment.fIOVMAddr & 15) == 0;
            if (mapped) window.chainAddress = segment.fIOVMAddr;
        }
        /* Mapped SG is an optimization of the established staging path.
         * Resource/geometry limits retain a safe, explicit bounce fallback. */
        if (!mapped) {
            if (window.dma) {
                window.dma->clearMemoryDescriptor();
                window.dma->release();
                window.dma = NULL;
            }
            if (window.chainDma) {
                window.chainDma->clearMemoryDescriptor();
                window.chainDma->release();
                window.chainDma = NULL;
            }
            if (window.buffer) { window.buffer->release(); window.buffer = NULL; }
            if (window.chains) { window.chains->release(); window.chains = NULL; }
            window.chainAddress = 0;
        }
    }
    if (mapper) mapper->release();
    MPR_DEBUG_PROP("MPRAsyncRequestLimit", (unsigned long long)kMPRAsyncRequests, 32);
    MPR_DEBUG_PROP("MPRAsyncCommandLimit", (unsigned long long)kMPRAsyncSlots, 32);
    return true;
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
    for (unsigned i = 0; i < kMPRAsyncSlots; ++i) {
        MPRAsyncWindow &window = state->windows[i];
        if (!dmaStopped) continue; /* IOC may still own mapped data/chain memory. */
        if (window.dma) {
            window.dma->clearMemoryDescriptor();
            window.dma->release();
        }
        if (window.chainDma) {
            window.chainDma->clearMemoryDescriptor();
            window.chainDma->release();
        }
        if (window.buffer) window.buffer->release();
        if (window.chains) window.chains->release();
    }
    /* Keep the state lock until final IOService::free(), as synchronous
     * storage callers retained before stop may still unwind a fence. */
    state->released = true;
}

void
MPRController::destroyAsyncState(void)
{
    MPRAsyncState *state = fAsync;
    if (!state) return;
    releaseAsync(false);
    if (state->lock) IOLockFree(state->lock);
    IOFree(state, sizeof(*state));
    fAsync = NULL;
}

void
MPRController::setAsyncAcceptance(bool enabled)
{
    if (!fAsync) return;
    IOLockLock(fAsync->lock);
    fAsync->accepting = enabled && !fAsync->stopping;
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
    if (!state->accepting || state->stopping) {
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
    if (state->stopping) {
        IOLockUnlock(state->lock);
        return kIOReturnNotReady;
    }
    state->barrier = true;
    state->fence = state->sequence;
    /* Newer requests remain queued. Only commands accepted before this
     * fence must retire before the ordered cache/UNMAP operation starts. */
    while (mprFencePending(state) && !state->stopping) {
        if (!state->scheduled) {
            state->scheduled = true;
            thread_call_enter(state->call);
        }
        IOLockSleep(state->lock, &state->outstanding, THREAD_UNINT);
    }
    if (state->stopping) {
        state->barrier = false;
        IOLockWakeup(state->lock, &state->barrier, false);
        IOLockUnlock(state->lock);
        return kIOReturnNotReady;
    }
    IOLockUnlock(state->lock);
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
    if (!state) return;
    retain(); /* Last completion may release the last request reference. */
    for (;;) {
        IOLockLock(fIOLock);
        UInt32 posted = 0;
        for (unsigned index = 0; index < kMPRAsyncSlots; ++index) {
            MPRAsyncWindow &window = state->windows[index];
            window.posted = false;
            window.request = NULL;
            const UInt32 smid = kMPRRuntimeSmid + index;
            /* Never repurpose a window still owned by an unconfirmed IOC. */
            if (!commandDmaSafe(smid)) continue;
            /* A late legitimate reply can arrive after its failed client was
             * completed. This window has no waiter now; consume that orphan
             * before reserving its SMID, never clear a still-owned request. */
            UInt8 orphan[64], orphanKind = 0;
            (void)takeReplySlot(smid, orphan, sizeof(orphan), &orphanKind);
            IOLockLock(state->lock);
            MPRAsyncRequest *job = mprNextAsync(state);
            if (job) job->turn = ++state->turn;
            IOLockUnlock(state->lock);
            if (!job) continue;
            MPRTargetSlot *slot = findTarget(job->devHandle);
            const UInt32 blockSize = slot != NULL ? slot->blockSize : 0;
            const UInt64 capacityBlocks = slot != NULL ? slot->capacityBlocks : 0;
            /* Admission is per target: one target that is being probed, is
             * offline or has been replaced must not fail the others. */
            if (fStopping || fSuspended || fDeviceOffline || fProtocolRecoveryPending ||
                slot == NULL || !slot->present || !slot->probeValid ||
                slot->offlinePending ||
                job->targetGeneration != slot->generation ||
                job->generation != fMediaGeneration || mediaRemovalPending()) {
                job->status = kIOReturnNoMedia;
                continue;
            }
            if (!job->initialized) {
                job->initialized = true;
                if (!blockSize || !capacityBlocks ||
                    job->blocks > UINT64_MAX / blockSize ||
                    (job->blocks && (job->block >= capacityBlocks ||
                     job->blocks > capacityBlocks - job->block))) {
                    job->status = kIOReturnBadArgument;
                    continue;
                }
                job->total = job->blocks * blockSize;
                if (job->buffer->getLength() < job->total) {
                    job->status = kIOReturnBadArgument;
                    continue;
                }
                if (!job->total) continue;
                job->status = job->buffer->prepare();
                if (job->status != kIOReturnSuccess) continue;
                job->prepared = true;
                job->blockSize = blockSize;
            }
            UInt64 remaining = job->total - job->issued;
            UInt32 blocks = (UInt32)((remaining < kMPRAsyncBytes ?
                                     remaining : kMPRAsyncBytes) / blockSize);
            if (!blocks || !fDataBuffer || !fRequestBuffer) {
                job->status = kIOReturnNotReady;
                continue;
            }
            const UInt32 bytes = blocks * blockSize;
            window.offset = job->issued;
            window.bytes = bytes;
            window.mapped = false;
            UInt8 *bounce = (UInt8 *)fDataBuffer->getBytesNoCopy();
            if (!bounce) {
                job->status = kIOReturnNotReady;
                continue;
            }
            bounce += index * kMPRAsyncBytes;
            UInt8 *staging = window.buffer ?
                            (UInt8 *)window.buffer->getBytesNoCopy() : bounce;
            if (!staging || (job->write &&
                job->buffer->readBytes((IOByteCount)window.offset,
                                       staging, bytes) != bytes)) {
                job->status = kIOReturnUnderrun;
                continue;
            }
            if (!prepareScsiSense(smid)) {
                job->status = kIOReturnBusy;
                continue;
            }
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
                continue;
            }
            request->DevHandle = job->devHandle;
            request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
            request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
            request->SenseBufferLength = 96;
            request->SGLOffset0 = MPR_SCSIIO_SGE_DWORDS;
            request->DataLength = bytes;
            request->IoFlags = (UInt16)(cdbLength | (slot->fastPath ?
                                           MPI25_SCSIIO_IOFLAGS_FAST_PATH : 0));
            request->Control = job->write ? MPI2_SCSIIO_CONTROL_WRITE :
                                            MPI2_SCSIIO_CONTROL_READ;
            memcpy(request->CDB, cdb, cdbLength);
            if (window.dma && window.chainDma) {
                IODMACommand::Segment64 raw[kMPRAsyncSegments];
                MPRDmaSegment segments[kMPRAsyncSegments];
                UInt64 offset = 0, left = bytes;
                UInt32 count = kMPRAsyncSegments, used = 0;
                IOReturn mapped = window.dma->gen64IOVMSegments(&offset, raw, &count);
                if (mapped == kIOReturnSuccess) {
                    for (UInt32 i = 0; i < count && left; ++i) {
                        UInt64 take = raw[i].fLength < left ? raw[i].fLength : left;
                        if (!take || take > UINT32_MAX) break;
                        segments[used].address = raw[i].fIOVMAddr;
                        segments[used++].length = (UInt32)take;
                        left -= take;
                    }
                }
                MPRSglResult result = {};
                UInt32 depth = fMaxChainDepth < kMPRChainLimit ?
                               fMaxChainDepth : kMPRChainLimit;
                window.mapped = mapped == kIOReturnSuccess && !left &&
                    mprBuildIeeeSgl(frame, fRequestFrameSize,
                        MPR_SCSIIO_SGE_DWORDS * 4, segments, used, bytes,
                        (UInt8 *)window.chains->getBytesNoCopy(),
                        window.chainAddress, fChainFrameBytes, depth,
                        kMPRChainStorageBytes, &result);
                if (window.mapped) {
                    request->ChainOffset = result.requestChainOffset;
                    window.mapped =
                        window.dma->synchronize(kIODirectionOut) == kIOReturnSuccess &&
                        window.chainDma->synchronize(kIODirectionOut) == kIOReturnSuccess;
                    if (window.mapped) {
                        ++state->mappedCommands;
                        if (result.chainsUsed) ++state->chainedCommands;
                    }
                }
            }
            if (!window.mapped) {
                if (job->write && staging != bounce) memcpy(bounce, staging, bytes);
                request->ChainOffset = 0;
                mprSglElement(frame + MPR_SCSIIO_SGE_DWORDS * 4,
                    fDataPhys + (UInt64)index * kMPRAsyncBytes, bytes, 0, 0x40);
                ++state->bounceCommands;
            }
            if (!postScsiIo(smid)) {
                cancelUnpostedCommand(smid);
                job->status = kIOReturnNoMedia;
                continue;
            }
            window.request = job;
            window.posted = true;
            ++fIoCount;
            job->issued += bytes;
            ++job->inFlight;
            ++posted;
        }

        /* Harvest every posted command, even after a copy/command failure.
         * Reset synthesizes cancelled completions for siblings, so they do
         * not start a new timeout for a command the reset has already ended. */
        for (unsigned index = 0; index < kMPRAsyncSlots; ++index) {
            MPRAsyncWindow &window = state->windows[index];
            if (!window.posted) continue;
            MPRAsyncRequest *job = window.request;
            IOReturn result = kIOReturnIOError;
            bool complete = scsiWaitChunk(kMPRRuntimeSmid + index,
                                          window.bytes, &result);
            MPRTargetSlot *jobSlot = findTarget(job->devHandle);
            if (complete && (job->generation != fMediaGeneration ||
                jobSlot == NULL || !jobSlot->present || jobSlot->offlinePending ||
                job->targetGeneration != jobSlot->generation ||
                mediaRemovalPending())) {
                complete = false;
                result = kIOReturnNoMedia;
            }
            if (complete && !job->write) {
                if (window.mapped &&
                    window.dma->synchronize(kIODirectionIn) != kIOReturnSuccess) {
                    complete = false;
                    result = kIOReturnIOError;
                } else {
                    UInt8 *bytes = window.mapped ?
                        (UInt8 *)window.buffer->getBytesNoCopy() :
                        (UInt8 *)fDataBuffer->getBytesNoCopy() + index * kMPRAsyncBytes;
                    IOByteCount copied = job->buffer->writeBytes(
                        (IOByteCount)window.offset, bytes, window.bytes);
                    if (copied != window.bytes) {
                        if (copied < window.bytes && window.offset == job->completed)
                            job->completed += copied;
                        complete = false;
                        result = copied < window.bytes ? kIOReturnUnderrun :
                                                        kIOReturnOverrun;
                    }
                }
            }
            if (!complete && job->status == kIOReturnSuccess) {
                job->status = result == kIOReturnSuccess ? kIOReturnIOError : result;
                if (jobSlot != NULL) jobSlot->failureCount++;
            }
            if (complete && window.offset == job->completed)
                job->completed += window.bytes;
            if (complete && job->write) {
                ++fWriteCount;
                fWriteBytes += window.bytes;
                fLastWriteLba = job->block + window.offset /
                    (job->blockSize != 0 ? job->blockSize : fBlockSize);
                fLastWriteSmid = kMPRRuntimeSmid + index;
                /* Write accounting stays attributable to one target. */
                if (jobSlot != NULL) jobSlot->writeCount++;
            }
            --job->inFlight;
            window.posted = false;
            window.request = NULL;
        }
        for (unsigned i = 0; i < kMPRAsyncRequests; ++i) {
            IOLockLock(state->lock);
            MPRAsyncRequest &job = state->requests[i];
            bool eligible = job.occupied && !job.finished && !job.inFlight;
            bool beforeFence = !state->barrier || job.sequence <= state->fence;
            IOLockUnlock(state->lock);
            if (!eligible) continue;
            if (!posted && job.status == kIOReturnSuccess &&
                beforeFence &&
                (fStopping || fDeviceOffline || fProtocolRecoveryPending)) {
                job.status = kIOReturnNoMedia;
            }
            if (job.status != kIOReturnSuccess ||
                (job.initialized && job.issued == job.total)) {
                /* FUA fallback is ordered under the transport lock and names
                 * this request's target; never flush a replacement medium or
                 * another disk on behalf of this request. */
                if (job.status == kIOReturnSuccess && job.write && job.fua &&
                    job.total) {
                    MPRTargetSlot *flushSlot = findTarget(job.devHandle);
                    if (flushSlot != NULL && !flushSlot->nativeFua &&
                        job.generation == fMediaGeneration &&
                        job.targetGeneration == flushSlot->generation &&
                        flushSlot->present && flushSlot->probeValid &&
                        !flushSlot->offlinePending && !mediaRemovalPending() &&
                        selectTarget(flushSlot->handle)) {
                        job.status = scsiSynchronizeCache(0, 0, 29, true);
                    } else {
                        job.status = kIOReturnNoMedia;
                    }
                }
                IOLockLock(state->lock);
                job.finished = true;
                IOLockWakeup(state->lock, &state->outstanding, false);
                IOLockUnlock(state->lock);
            }
        }
        IOLockUnlock(fIOLock);

        IOLockLock(state->lock);
        bool completions = false;
        for (unsigned i = 0; i < kMPRAsyncRequests; ++i)
            if (state->requests[i].occupied && state->requests[i].finished)
                completions = true;
        if (completions && !state->completionsScheduled) {
            state->completionsScheduled = true;
            thread_call_enter(state->completionCall);
        }
        bool more = mprNextAsync(state) != NULL;
        if (!more) state->scheduled = false;
        IOLockWakeup(state->lock, &state->outstanding, false);
        IOLockUnlock(state->lock);
        MPR_DEBUG_PROP("MPRAsyncPeakRequests", (unsigned long long)state->peak, 32);
        MPR_DEBUG_PROP("MPRMappedCommands", (unsigned long long)state->mappedCommands, 32);
        MPR_DEBUG_PROP("MPRChainedCommands", (unsigned long long)state->chainedCommands, 32);
        MPR_DEBUG_PROP("MPRBounceCommands", (unsigned long long)state->bounceCommands, 32);
        if (!more) break;
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
    if (!state) return;
    IOLockLock(state->lock);
    state->accepting = false;
    state->stopping = true;
    state->barrier = false;
    IOLockWakeup(state->lock, &state->barrier, false);
    IOLockWakeup(state->lock, &state->outstanding, false);
    IOLockUnlock(state->lock);
    /* Caller has already closed fStopping. Waiting takes no transport lock.
     * A cancelled queued call still leaves accepted requests to finish. */
    thread_call_cancel_wait(state->call);
    asyncWorker();
    thread_call_cancel_wait(state->completionCall);
    asyncCompletions();
}
