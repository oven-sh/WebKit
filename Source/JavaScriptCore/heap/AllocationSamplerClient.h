#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "CollectionScope.h"

namespace JSC {

class HeapCell;
class JSCell;

// What Heap::startAllocationSampling reports to. The heap picks the allocations with an exponential byte budget drawn
// from the sampling interval (a cell's probability of being picked is 1 - exp(-bytes / interval)); the client attributes
// them to the code that allocated and weighs them by that probability. The heap keeps no record of the picks: the
// client holds the addresses, and the heap tells it when one moves and when to check which are still alive.
//
// Every call runs on the mutator thread with the API lock held, except reconcileSampledCells, which runs where the GC
// reconciles weak references: after marking, before sweeping, with the mutator stopped. No call may allocate a GC cell.
class AllocationSamplerClient {
public:
    virtual ~AllocationSamplerClient() = default;

    // A cell the heap just handed out, or the bytes a precise cell grew by. A new cell is not initialized yet: only
    // its address and size mean anything.
    virtual void didSampleAllocation(HeapCell*, size_t bytes) = 0;

    // The characters of a string (Heap::reportStringExtraMemoryAllocated): memory outside the GC heap that lives as
    // long as its owner.
    virtual void didSampleExtraMemory(const JSCell* owner, size_t bytes) = 0;

    // A cell moved. `to` may have been reported by didSampleAllocation while the heap made the copy; that report was
    // not an allocation and the sample that records it is void.
    virtual void didRelocateCell(HeapCell* from, HeapCell* to) = 0;

    // Heap::isMarked answers for every address the client holds. A cell that is not marked is about to be swept, and
    // its address must not be used after this call.
    virtual void reconcileSampledCells(CollectionScope) = 0;
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
