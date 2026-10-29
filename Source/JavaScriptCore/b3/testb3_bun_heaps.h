/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3.h"

#if ENABLE(B3_JIT)

#include "B3AbstractHeapRepository.h"
#include "B3CompilationScratch.h"
#include "B3HeapSet.h"
#include "B3PatchpointValue.h"
#include <algorithm>
#include <array>
#include <memory>
#include <span>

static PatchpointValue* appendVoidPatchpoint(Procedure& proc, BasicBlock* block)
{
    return block->appendNew<PatchpointValue>(proc, Void, Origin());
}

static CCallValue* appendVoidCCall(Procedure& proc, BasicBlock* block)
{
    Value* callee = block->appendIntConstant(proc, Origin(), pointerType(), 0);
    return block->appendNew<CCallValue>(proc, Void, Origin(), callee);
}

static std::array<const AbstractHeap*, 3> unrelatedHeaps(const AbstractHeapRepository& heaps)
{
    return { &heaps.JSCell_structureID, &heaps.Butterfly_publicLength, &heaps.JSPromise_slot };
}

static const AbstractHeap& rootOf(const AbstractHeap& heap)
{
    const AbstractHeap* result = &heap;
    while (const AbstractHeap* parent = result->parent())
        result = parent;
    return *result;
}

template<typename Functor>
static void forEachLeafHeap(const AbstractHeap& heap, const Functor& functor)
{
    if (heap.children().isEmpty()) {
        functor(heap);
        return;
    }
    for (const AbstractHeap* child : heap.children())
        forEachLeafHeap(*child, functor);
}

static NO_RETURN_DUE_TO_CRASH void reportWrongOverlap(const HeapSet& set, ASCIILiteral expectation, const AbstractHeap& heap)
{
    crashLock.lock();
    dataLogLn("The set ", set, " ", expectation, " ", heap);
    CRASH();
}

static void checkExactSet(const HeapSet& set, std::span<const AbstractHeap* const> heaps)
{
    CHECK(!heaps.empty());
    HeapRange cover;
    for (const AbstractHeap* heap : heaps) {
        if (!set.overlaps(heap->range()))
            reportWrongOverlap(set, "must overlap"_s, *heap);
        cover |= heap->range();
    }
    CHECK_EQ(set.bounds(), cover);

    unsigned numberOfUnlistedHeapsInBounds = 0;
    forEachLeafHeap(rootOf(*heaps[0]), [&](const AbstractHeap& leaf) {
        bool isListed = std::ranges::any_of(heaps, [&](const AbstractHeap* heap) {
            return heap->range().overlaps(leaf.range());
        });
        if (set.overlaps(leaf.range()) != isListed)
            reportWrongOverlap(set, isListed ? "must overlap"_s : "must not overlap"_s, leaf);
        numberOfUnlistedHeapsInBounds += !isListed && cover.overlaps(leaf.range());
    });
    CHECK(numberOfUnlistedHeapsInBounds);
}

static void testHeapListCoversEveryHeap()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    auto list = unrelatedHeaps(heaps);
    PatchpointValue* patchpoint = appendVoidPatchpoint(proc, proc.addBlock());

    heaps.decoratePatchpointWrite(list, patchpoint);
    heaps.computeRangesAndDecorateInstructions();

    checkExactSet(patchpoint->effects.writes, list);
    CHECK_EQ(patchpoint->effects.writes.ranges().size(), list.size());
}

static void testHeapListIgnoresOrder()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    BasicBlock* root = proc.addBlock();
    auto list = unrelatedHeaps(heaps);
    Vector<PatchpointValue*> patchpoints;

    std::sort(list.begin(), list.end());
    do {
        patchpoints.append(appendVoidPatchpoint(proc, root));
        heaps.decoratePatchpointWrite(list, patchpoints.last());
    } while (std::next_permutation(list.begin(), list.end()));
    heaps.computeRangesAndDecorateInstructions();

    CHECK_EQ(patchpoints.size(), 6u);
    for (PatchpointValue* patchpoint : patchpoints) {
        checkExactSet(patchpoint->effects.writes, list);
        CHECK_EQ(patchpoint->effects.writes, patchpoints[0]->effects.writes);
    }
    CHECK_EQ(proc.heapSets().numberOfLists(), 1u);
}

static void testEmptyHeapListWritesNothing()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    PatchpointValue* patchpoint = appendVoidPatchpoint(proc, proc.addBlock());
    patchpoint->effects = Effects::forCall();

    heaps.decoratePatchpointWrite(std::span<const AbstractHeap* const>(), patchpoint);
    heaps.computeRangesAndDecorateInstructions();

    CHECK_EQ(patchpoint->effects.writes, HeapRange());
    CHECK_EQ(patchpoint->effects.reads, HeapRange::top());
}

static void testHeapListForReads()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    BasicBlock* root = proc.addBlock();
    auto list = unrelatedHeaps(heaps);
    PatchpointValue* patchpointWithList = appendVoidPatchpoint(proc, root);
    patchpointWithList->effects = Effects::forCall();
    PatchpointValue* patchpointWithEmptyList = appendVoidPatchpoint(proc, root);
    patchpointWithEmptyList->effects = Effects::forCall();

    heaps.decoratePatchpointRead(list, patchpointWithList);
    heaps.decoratePatchpointRead(std::span<const AbstractHeap* const>(), patchpointWithEmptyList);
    heaps.computeRangesAndDecorateInstructions();

    checkExactSet(patchpointWithList->effects.reads, list);
    CHECK_EQ(patchpointWithList->effects.writes, HeapRange::top());
    CHECK_EQ(patchpointWithEmptyList->effects.reads, HeapRange());
    CHECK_EQ(patchpointWithEmptyList->effects.writes, HeapRange::top());
}

static void testHeapListSkipsReplacedValue()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    BasicBlock* root = proc.addBlock();
    auto list = unrelatedHeaps(heaps);
    Value* replaced = appendVoidPatchpoint(proc, root);
    PatchpointValue* remaining = appendVoidPatchpoint(proc, root);

    heaps.decoratePatchpointRead(list, replaced);
    heaps.decoratePatchpointWrite(list, replaced);
    heaps.decoratePatchpointRead(list[0], replaced);
    heaps.decoratePatchpointWrite(list[0], replaced);
    heaps.decoratePatchpointRead(list, remaining);
    heaps.decoratePatchpointWrite(list, remaining);
    replaced->replaceWithNop();
    heaps.computeRangesAndDecorateInstructions();

    CHECK(replaced->opcode() == Nop);
    checkExactSet(remaining->effects.reads, list);
    checkExactSet(remaining->effects.writes, list);
}

static void testSingleHeapDecoration()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    BasicBlock* root = proc.addBlock();
    const AbstractHeap* heap = &heaps.JSPromise_slot;
    const AbstractHeap* noHeap = nullptr;
    PatchpointValue* patchpointWithHeap = appendVoidPatchpoint(proc, root);
    PatchpointValue* patchpointWithoutHeap = appendVoidPatchpoint(proc, root);

    heaps.decoratePatchpointRead(heap, patchpointWithHeap);
    heaps.decoratePatchpointWrite(heap, patchpointWithHeap);
    heaps.decoratePatchpointRead(noHeap, patchpointWithoutHeap);
    heaps.decoratePatchpointWrite(noHeap, patchpointWithoutHeap);
    heaps.computeRangesAndDecorateInstructions();

    CHECK_EQ(patchpointWithHeap->effects.reads, heap->range());
    CHECK_EQ(patchpointWithHeap->effects.writes, heap->range());
    CHECK_EQ(patchpointWithoutHeap->effects.reads, HeapRange());
    CHECK_EQ(patchpointWithoutHeap->effects.writes, HeapRange());
}

static void testPatchpointDecorationsAccumulate()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    BasicBlock* root = proc.addBlock();
    auto all = unrelatedHeaps(heaps);
    std::array<const AbstractHeap*, 2> list { all[0], all[1] };
    const AbstractHeap* heap = all[2];
    const AbstractHeap* noHeap = nullptr;
    PatchpointValue* listThenHeap = appendVoidPatchpoint(proc, root);
    PatchpointValue* heapThenList = appendVoidPatchpoint(proc, root);
    PatchpointValue* heapThenNothing = appendVoidPatchpoint(proc, root);

    heaps.decoratePatchpointRead(list, listThenHeap);
    heaps.decoratePatchpointRead(heap, listThenHeap);
    heaps.decoratePatchpointWrite(list, listThenHeap);
    heaps.decoratePatchpointWrite(heap, listThenHeap);

    heaps.decoratePatchpointRead(heap, heapThenList);
    heaps.decoratePatchpointRead(list, heapThenList);
    heaps.decoratePatchpointWrite(heap, heapThenList);
    heaps.decoratePatchpointWrite(list, heapThenList);

    heaps.decoratePatchpointWrite(heap, heapThenNothing);
    heaps.decoratePatchpointWrite(noHeap, heapThenNothing);
    heaps.decoratePatchpointWrite(std::span<const AbstractHeap* const>(), heapThenNothing);

    heaps.computeRangesAndDecorateInstructions();

    for (PatchpointValue* patchpoint : { listThenHeap, heapThenList }) {
        checkExactSet(patchpoint->effects.reads, all);
        checkExactSet(patchpoint->effects.writes, all);
    }
    CHECK_EQ(listThenHeap->effects.writes, heapThenList->effects.writes);
    CHECK_EQ(heapThenNothing->effects.writes, heap->range());

    heaps.computeRangesAndDecorateInstructions();
    checkExactSet(listThenHeap->effects.writes, all);
    CHECK_EQ(heapThenNothing->effects.writes, heap->range());
}

static void testDetachProcedureDropsHeapLists()
{
    Procedure proc;
    AbstractHeapRepository& heaps = proc.heaps();
    auto list = unrelatedHeaps(heaps);
    PatchpointValue* patchpoint = appendVoidPatchpoint(proc, proc.addBlock());

    heaps.decoratePatchpointRead(list, patchpoint);
    heaps.decoratePatchpointWrite(list, patchpoint);
    heaps.detachProcedure();
    patchpoint->effects.reads = HeapRange();
    patchpoint->effects.writes = HeapRange();
    heaps.computeRangesAndDecorateInstructions();

    CHECK_EQ(patchpoint->effects.reads, HeapRange());
    CHECK_EQ(patchpoint->effects.writes, HeapRange());
}

struct TestRepository final : AbstractHeapRepository {
    static constexpr ptrdiff_t retainedSlotIndex = 0;

    TestRepository()
        : clientState(&root, "clientState")
        , clientSlots(&root, "clientSlots", 0, sizeof(uint64_t))
    {
        clientSlots.atAnyIndex().changeParent(&clientState);
        JSMap_storage.changeParent(&clientState);
        clientSlots.at(retainedSlotIndex);
        clientSlots.retainCurrentHeaps();
    }

    AbstractHeap clientState;
    IndexedAbstractHeap clientSlots;

private:
    void removeTemporaryClientHeaps() final { clientSlots.removeTemporaryHeaps(); }
    bool hasTemporaryClientHeaps() const final { return clientSlots.hasTemporaryHeaps(); }
};

static std::unique_ptr<AbstractHeapRepository> createTestRepository()
{
    return makeUnique<TestRepository>();
}

struct TestClient {
    CompilationScratch scratch { createTestRepository };
    Procedure proc { Policy { }, &scratch };
    TestRepository& heaps { static_cast<TestRepository&>(proc.heaps()) };
    BasicBlock* root { proc.addBlock() };
};

static void testClientHeapsAreNested()
{
    TestClient client;
    TestRepository& heaps = client.heaps;
    const AbstractHeap& anySlot = heaps.clientSlots.atAnyIndex();
    const AbstractHeap& retainedSlot = heaps.clientSlots[TestRepository::retainedSlotIndex];
    const AbstractHeap& slot = heaps.clientSlots[3];
    std::array<const AbstractHeap*, 2> list { &heaps.JSCell_structureID, &heaps.clientState };
    PatchpointValue* patchpoint = appendVoidPatchpoint(client.proc, client.root);

    heaps.decoratePatchpointWrite(list, patchpoint);
    heaps.computeRangesAndDecorateInstructions();

    CHECK(heaps.clientState.parent() == &heaps.root);
    CHECK(anySlot.parent() == &heaps.clientState);
    CHECK(heaps.JSMap_storage.parent() == &heaps.clientState);
    CHECK(retainedSlot.parent() == &anySlot);
    CHECK(slot.parent() == &anySlot);

    CHECK(HeapSet(heaps.clientState.range()).isSubsetOf(heaps.root.range()));
    CHECK(HeapSet(anySlot.range()).isSubsetOf(heaps.clientState.range()));
    CHECK(HeapSet(heaps.JSMap_storage.range()).isSubsetOf(heaps.clientState.range()));
    CHECK(HeapSet(retainedSlot.range()).isSubsetOf(anySlot.range()));
    CHECK(HeapSet(slot.range()).isSubsetOf(anySlot.range()));
    CHECK(!slot.range().overlaps(retainedSlot.range()));
    CHECK(!anySlot.range().overlaps(heaps.JSMap_storage.range()));
    CHECK(!heaps.clientState.range().overlaps(heaps.JSSet_storage.range()));

    checkExactSet(patchpoint->effects.writes, list);
    CHECK(patchpoint->effects.writes.overlaps(slot.range()));
    CHECK(patchpoint->effects.writes.overlaps(heaps.JSMap_storage.range()));
    CHECK(!patchpoint->effects.writes.overlaps(heaps.JSSet_storage.range()));

    Procedure procWithoutClient;
    CHECK(procWithoutClient.heaps().JSMap_storage.parent() == &procWithoutClient.heaps().root);
}

static void testTemporaryClientHeapsAreRemoved()
{
    TestClient client;
    TestRepository& heaps = client.heaps;
    const AbstractHeap& anySlot = heaps.clientSlots.atAnyIndex();
    const AbstractHeap& retainedSlot = heaps.clientSlots[TestRepository::retainedSlotIndex];

    heaps.computeRangesAndDecorateInstructions();
    HeapRange rootRangeWithoutTemporaryHeaps = heaps.root.range();
    CHECK(!heaps.clientSlots.hasTemporaryHeaps());
    CHECK_EQ(anySlot.numberOfChildren(), 1u);

    const AbstractHeap& temporarySlot = heaps.clientSlots[3];
    heaps.clientSlots.at(100);
    CHECK(heaps.clientSlots.hasTemporaryHeaps());
    CHECK_EQ(anySlot.numberOfChildren(), 3u);
    CHECK(!temporarySlot.hasRange());

    heaps.computeRangesAndDecorateInstructions();
    CHECK(temporarySlot.hasRange());
    CHECK(HeapSet(temporarySlot.range()).isSubsetOf(anySlot.range()));
    CHECK(heaps.root.range() != rootRangeWithoutTemporaryHeaps);

    heaps.detachProcedure();
    CHECK(!heaps.clientSlots.hasTemporaryHeaps());
    CHECK_EQ(anySlot.numberOfChildren(), 1u);
    CHECK(anySlot.children()[0] == &retainedSlot);
    CHECK(retainedSlot.parent() == &anySlot);
    CHECK(!temporarySlot.isInitialized());

    heaps.computeRangesAndDecorateInstructions();
    CHECK_EQ(heaps.root.range(), rootRangeWithoutTemporaryHeaps);
}

static void testCCallDecorationLists()
{
    TestClient client;
    TestRepository& heaps = client.heaps;
    std::array<const AbstractHeap*, 2> reads { &heaps.JSCell_structureID, &heaps.clientState };
    std::array<const AbstractHeap*, 2> writes { &heaps.Butterfly_publicLength, &heaps.clientSlots[3] };
    CCallValue* callWithLists = appendVoidCCall(client.proc, client.root);
    CCallValue* callWithSeveralDecorations = appendVoidCCall(client.proc, client.root);
    CCallValue* callWithEmptyLists = appendVoidCCall(client.proc, client.root);
    CCallValue* callWithoutDecoration = appendVoidCCall(client.proc, client.root);

    heaps.decorateCCallRead(reads, callWithLists);
    heaps.decorateCCallWrite(writes, callWithLists);
    for (const AbstractHeap* heap : reads)
        heaps.decorateCCallRead(heap, callWithSeveralDecorations);
    for (const AbstractHeap* heap : writes)
        heaps.decorateCCallWrite(heap, callWithSeveralDecorations);
    heaps.decorateCCallRead(std::span<const AbstractHeap* const>(), callWithEmptyLists);
    heaps.decorateCCallWrite(std::span<const AbstractHeap* const>(), callWithEmptyLists);
    heaps.computeRangesAndDecorateInstructions();

    checkExactSet(callWithLists->effects.reads, reads);
    checkExactSet(callWithLists->effects.writes, writes);
    CHECK_EQ(callWithSeveralDecorations->effects.reads, callWithLists->effects.reads);
    CHECK_EQ(callWithSeveralDecorations->effects.writes, callWithLists->effects.writes);
    CHECK_EQ(callWithEmptyLists->effects.reads, HeapRange());
    CHECK_EQ(callWithEmptyLists->effects.writes, HeapRange());
    CHECK_EQ(callWithoutDecoration->effects.reads, HeapRange::top());
    CHECK_EQ(callWithoutDecoration->effects.writes, HeapRange::top());
}

struct DecoratedWrite {
    friend bool operator==(const DecoratedWrite&, const DecoratedWrite&) = default;

    const AbstractHeap* heap;
    Value* value;
};

static Vector<DecoratedWrite> writesDecoratedSince(const AbstractHeapRepository& heaps, const AbstractHeapRepository::DecorationMark& mark)
{
    Vector<DecoratedWrite> result;
    heaps.forEachWriteDecoratedSince(mark, [&](const AbstractHeap& heap, Value* value) {
        result.append(DecoratedWrite { &heap, value });
    });
    return result;
}

static void testWriteDecorationsSinceMark()
{
    TestClient client;
    TestRepository& heaps = client.heaps;
    Procedure& proc = client.proc;
    BasicBlock* root = client.root;
    const AbstractHeap& slot = heaps.clientSlots[3];
    Value* pointer = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    auto appendStore = [&](const AbstractHeap& heap) {
        MemoryValue* store = root->appendNew<MemoryValue>(proc, Store, Origin(), pointer, pointer);
        heaps.decorateMemory(&heap, store);
        return store;
    };

    appendStore(heaps.JSObject_butterfly);
    heaps.decorateCCallWrite(&heaps.JSObject_butterfly, appendVoidCCall(proc, root));
    heaps.decoratePatchpointWrite(&heaps.JSObject_butterfly, appendVoidPatchpoint(proc, root));
    AbstractHeapRepository::DecorationMark mark = heaps.markDecorations();
    CHECK(writesDecoratedSince(heaps, mark).isEmpty());

    MemoryValue* firstStore = appendStore(heaps.clientState);
    heaps.decorateMemory(&heaps.clientState, root->appendNew<MemoryValue>(proc, Load, Int64, Origin(), pointer));
    MemoryValue* secondStore = appendStore(slot);
    Vector<DecoratedWrite> stores { { &heaps.clientState, firstStore }, { &slot, secondStore } };
    CHECK(writesDecoratedSince(heaps, mark) == stores);

    AbstractHeapRepository::DecorationMark markAfterStores = heaps.markDecorations();
    std::array<const AbstractHeap*, 2> list { &heaps.clientState, &heaps.JSCell_structureID };
    CCallValue* call = appendVoidCCall(proc, root);
    PatchpointValue* patchpoint = appendVoidPatchpoint(proc, root);
    heaps.decorateCCallRead(list, call);
    heaps.decorateCCallWrite(&slot, call);
    heaps.decoratePatchpointRead(&slot, patchpoint);
    heaps.decoratePatchpointWrite(list, patchpoint);
    Vector<DecoratedWrite> listedWrites { { &slot, call }, { list[0], patchpoint }, { list[1], patchpoint } };
    CHECK(writesDecoratedSince(heaps, markAfterStores) == listedWrites);

    Vector<DecoratedWrite> allWrites = stores;
    allWrites.appendVector(listedWrites);
    CHECK(writesDecoratedSince(heaps, mark) == allWrites);
    CHECK(writesDecoratedSince(heaps, heaps.markDecorations()).isEmpty());
}

inline void addBunHeapTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    RUN(testHeapListCoversEveryHeap());
    RUN(testHeapListIgnoresOrder());
    RUN(testEmptyHeapListWritesNothing());
    RUN(testHeapListForReads());
    RUN(testHeapListSkipsReplacedValue());
    RUN(testSingleHeapDecoration());
    RUN(testPatchpointDecorationsAccumulate());
    RUN(testDetachProcedureDropsHeapLists());

    RUN(testClientHeapsAreNested());
    RUN(testTemporaryClientHeapsAreRemoved());
    RUN(testCCallDecorationLists());
    RUN(testWriteDecorationsSinceMark());
}

#endif // ENABLE(B3_JIT)
