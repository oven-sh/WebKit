/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3MergeAdjacentConstantStores.h"
#include <array>

namespace {

struct StoreSequence {
    void storeConstant(int32_t offset, int32_t value) { storeConstant(offset, value, pointer); }
    void storeConstant(int32_t offset, int32_t value, Value* base, HeapRange range = firstHeap) { graph.store(root, graph.int32(root, value), base, offset, range); }
    void storeWideConstant(int32_t offset, int64_t value, HeapRange range = firstHeap) { graph.store(root, graph.int64(root, value), pointer, offset, range); }

    Graph& graph;
    BasicBlock* root;
    Value* pointer;
    Value* other;
    Value* word;
};

} // anonymous namespace

template<typename Functor>
static void buildStores(Procedure& proc, const Functor& fill)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int32_t*, int32_t*, int32_t>(proc, root);
    StoreSequence stores { graph, root, arguments[0], arguments[1], arguments[2] };
    fill(stores);
    graph.ret(root, graph.int32(root, 0));
    proc.resetReachability();
}

static std::array<uint8_t, 32> bytesStoredBy(Procedure& proc)
{
    alignas(8) std::array<uint8_t, 32> memory;
    memory.fill(0xaa);
    int32_t* pointer = std::bit_cast<int32_t*>(memory.data());
    compileAndRun<int32_t>(proc, pointer, pointer, 7);
    return memory;
}

template<typename Functor>
static unsigned storeCountAfterMerging(const Functor& fill)
{
    Procedure unmerged;
    buildStores(unmerged, fill);

    Procedure merged;
    buildStores(merged, fill);
    bool changed = mergeAdjacentConstantStores(merged);
    validate(merged);

    unsigned storeCount = numberOfValues(merged, Store);
    unsigned mergedStoreCount = numberOfValues(merged, [](Value* value) {
        return value->opcode() == Store && value->child(0)->opcode() == Const64;
    });
    CHECK_EQ(changed, !!mergedStoreCount);
    CHECK_EQ(storeCount + mergedStoreCount, numberOfValues(unmerged, Store));
    CHECK(bytesStoredBy(unmerged) == bytesStoredBy(merged));
    return storeCount;
}

static void testMergeAdjacentConstantStores()
{
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 4, 8, 12 })
            stores.storeConstant(offset, 1);
    }), 2u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 4, 0, 12, 8 })
            stores.storeConstant(offset, 1);
    }), 2u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset, 0);
            stores.storeConstant(offset + 4, -1);
        }
    }), 2u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        stores.storeConstant(0, 5);
        stores.storeConstant(4, 0);
    }), isX86_64() ? 1u : 2u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset, 5);
            stores.storeConstant(offset + 4, 0);
        }
    }), isARM64() ? 4u : 2u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8, 16, 24 }) {
            stores.storeConstant(offset, 5);
            stores.storeConstant(offset + 4, 0);
        }
    }), 4u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        stores.storeConstant(0, 1);
        stores.storeConstant(4, 1);
    }), 2u);
}

static void testMergeAdjacentConstantStoresJoinsHeapRanges()
{
    Procedure proc;
    buildStores(proc, [](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset, 1);
            stores.storeConstant(offset + 4, 1, stores.pointer, secondHeap);
        }
    });
    CHECK(mergeAdjacentConstantStores(proc));
    validate(proc);
    CHECK_EQ(numberOfValues(proc, Store), 2u);
    CHECK_EQ(numberOfValues(proc, [](Value* value) {
        return value->opcode() == Store && value->as<MemoryValue>()->range() == bothHeaps;
    }), 2u);
}

static void testMergeAdjacentConstantStoresKeepsOtherStores()
{
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        stores.storeConstant(4, 0);
        stores.storeConstant(8, 0);
    }), 2u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset, 1);
            stores.graph.load(stores.root, Int32, stores.pointer, 0, firstHeap);
            stores.storeConstant(offset + 4, 1);
        }
    }), 4u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset + 4, 1);
            stores.root->appendNew<MemoryValue>(stores.graph.proc, Store8, Origin(), stores.word, stores.pointer, offset + 5, firstHeap);
            stores.storeConstant(offset, 1);
        }
    }), 4u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset, 1);
            stores.storeConstant(offset + 4, 1, stores.other);
        }
    }), 4u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset, 1);
            stores.graph.store(stores.root, stores.word, stores.pointer, offset + 4, firstHeap);
        }
    }), 4u);
    CHECK_EQ(storeCountAfterMerging([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8 }) {
            stores.storeConstant(offset, 1);
            stores.root->appendNew<MemoryValue>(stores.graph.proc, Store, Origin(), stores.graph.int32(stores.root, 1), stores.pointer, offset + 4, firstHeap, HeapRange::top());
        }
    }), 4u);
}

namespace {

struct MergedZeroStores {
    unsigned storeCount;
    unsigned patchpointCount;
};

} // anonymous namespace

template<typename Functor>
static MergedZeroStores mergeZeroStores(const Functor& fill)
{
    Procedure unmerged;
    buildStores(unmerged, fill);

    Procedure merged;
    buildStores(merged, fill);
    mergeAdjacentConstantStores(merged);
    validate(merged);

    MergedZeroStores result { numberOfValues(merged, Store), numberOfValues(merged, Patchpoint) };
    CHECK(bytesStoredBy(unmerged) == bytesStoredBy(merged));
    return result;
}

static void testMergeZeroStoresIntoVectors()
{
    MergedZeroStores four = mergeZeroStores([](StoreSequence& stores) {
        for (int32_t offset : { 0, 8, 16, 24 })
            stores.storeWideConstant(offset, 0);
    });
    CHECK_EQ(four.storeCount, isX86_64() ? 0u : 4u);
    CHECK_EQ(four.patchpointCount, isX86_64() ? 1u : 0u);

    MergedZeroStores three = mergeZeroStores([](StoreSequence& stores) {
        for (int32_t offset : { 16, 0, 8 })
            stores.storeWideConstant(offset, 0);
    });
    CHECK_EQ(three.storeCount, isX86_64() ? 1u : 3u);
    CHECK_EQ(three.patchpointCount, isX86_64() ? 1u : 0u);

    MergedZeroStores words = mergeZeroStores([](StoreSequence& stores) {
        for (int32_t offset = 0; offset < 32; offset += 4)
            stores.storeConstant(offset, 0);
    });
    CHECK_EQ(words.storeCount, isX86_64() ? 0u : 4u);
    CHECK_EQ(words.patchpointCount, isX86_64() ? 1u : 0u);
}

static void testMergeZeroStoresKeepsOtherStores()
{
    auto staysAsItIs = [](unsigned numberOfStores, const auto& fill) {
        MergedZeroStores result = mergeZeroStores(fill);
        CHECK_EQ(result.storeCount, numberOfStores);
        CHECK_EQ(result.patchpointCount, 0u);
    };
    staysAsItIs(2, [](StoreSequence& stores) {
        stores.storeWideConstant(0, 0);
        stores.storeWideConstant(16, 0);
    });
    staysAsItIs(2, [](StoreSequence& stores) {
        stores.storeWideConstant(0, 0);
        stores.storeWideConstant(8, 1);
    });
    staysAsItIs(2, [](StoreSequence& stores) {
        stores.storeWideConstant(0, 0);
        stores.graph.load(stores.root, Int32, stores.pointer, 0, firstHeap);
        stores.storeWideConstant(8, 0);
    });
    staysAsItIs(2, [](StoreSequence& stores) {
        stores.storeWideConstant(0, 0);
        stores.graph.store(stores.root, stores.graph.int64(stores.root, 0), stores.other, 8, firstHeap);
    });
    staysAsItIs(1, [](StoreSequence& stores) {
        stores.storeWideConstant(0, 0);
    });
}

static void testMergeZeroStoresWritesExactHeaps()
{
    if (!isX86_64())
        return;
    Procedure proc;
    buildStores(proc, [](StoreSequence& stores) {
        stores.storeWideConstant(0, 0, firstHeap);
        stores.storeWideConstant(8, 0, thirdHeap);
    });
    CHECK(mergeAdjacentConstantStores(proc));
    validate(proc);
    PatchpointValue* patchpoint = nullptr;
    for (Value* value : proc.values()) {
        if (value->opcode() == Patchpoint)
            patchpoint = value->as<PatchpointValue>();
    }
    CHECK(patchpoint);
    CHECK(patchpoint->effects.writes.overlaps(firstHeap));
    CHECK(patchpoint->effects.writes.overlaps(thirdHeap));
    CHECK(!patchpoint->effects.writes.overlaps(secondHeap));
    CHECK(!patchpoint->effects.reads);
    CHECK(patchpoint->runsWithoutFrame());
}

void addBunStoreMergingTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testMergeAdjacentConstantStores());
    RUN(testMergeAdjacentConstantStoresJoinsHeapRanges());
    RUN(testMergeAdjacentConstantStoresKeepsOtherStores());
    RUN(testMergeZeroStoresIntoVectors());
    RUN(testMergeZeroStoresKeepsOtherStores());
    RUN(testMergeZeroStoresWritesExactHeaps());
}

#endif // ENABLE(B3_JIT)
