/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3HeapSet.h"
#include "B3MovedLoadCheck.h"
#include "B3Limits.h"
#include "B3MemoryValue.h"
#include "B3ValueKey.h"
#include <span>
#include <wtf/IndexSet.h>
#include <wtf/LayeredHashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class BasicBlock;
class Dominators;
class NaturalLoops;
class PatchpointValue;
class Procedure;

class MemoryNumbering {
    WTF_MAKE_NONCOPYABLE(MemoryNumbering);
public:
    enum class Result : uint8_t {
        Unchanged,
        Replaced,
    };

    explicit MemoryNumbering(Procedure&);

    void beginWalk(Dominators&);
    void enterBlock(BasicBlock*);

    static HeapSet writesOf(Value*);

    Result numberValue(Value*, Opcode opcodeBeforeReduction, const HeapSet& writesBeforeReduction);
    void didFinishBlock(BasicBlock*);
    void leaveDominatorSubtree();
    void endWalk();

    Value* representativeOf(Value*);
    Value* ensureRepresentative(Value*);

private:
    static constexpr unsigned noID = 0;

    struct RecordedWrite {
        unsigned begin { 0 };
        unsigned end { 0 };
        unsigned id { noID };

        bool hasSingleHeap() const { return end - begin == 1; }
        bool hasSameHeapsAs(const RecordedWrite& other) const { return begin == other.begin && end == other.end; }
    };

    using RecordedWrites = Vector<RecordedWrite, 8>;

    struct MemoryState {
        unsigned epoch { noID };
        unsigned id { noID };
        BasicBlock* owner { nullptr };
        BasicBlock* epochOwner { nullptr };
        RecordedWrites recordedWrites;

        void beginEpoch(BasicBlock* block)
        {
            epoch = id;
            epochOwner = block;
            recordedWrites.shrink(0);
        }
    };

    struct LoopWrites {
        bool writesEverything { false };
        RecordedWrites writes;
    };

    struct ReplacedLoad {
        Value* load;
        BasicBlock* block;
        Value* replacement;
        Value* address;
        MemoryValue::OffsetType offset;
        HeapRange range;
        bool isImmutable;
        bool isForwardedStore;
    };

    struct MixingValueKeyHash {
        static unsigned hash(const ValueKey& key)
        {
            uint64_t low = key.childIndex(0) | static_cast<uint64_t>(key.childIndex(1)) << 32;
            uint64_t high = key.childIndex(2) | static_cast<uint64_t>(key.childIndex(3)) << 32;
            uint64_t mixed = (low + (static_cast<uint64_t>(key.kind().hash()) << 32 | key.type().hash())) * 0x9e3779b97f4a7c15ull;
            mixed = ((mixed ^ mixed >> 32) + high) * 0xbf58476d1ce4e5b9ull;
            return static_cast<unsigned>(mixed >> 32);
        }
        static bool equal(const ValueKey& a, const ValueKey& b) { return a == b; }
    };

    struct PureCallKey {
        uint64_t id { 0 };
        Type type { Void };
        Vector<unsigned, 4> children;

        friend bool operator==(const PureCallKey&, const PureCallKey&) = default;
    };

    struct PureCallKeyHash {
        static unsigned hash(const PureCallKey&);
        static bool equal(const PureCallKey& a, const PureCallKey& b) { return a == b; }
    };

    static bool canBeRecorded(const HeapRange&);
    bool record(RecordedWrites&, RecordedWrite, Origin);
    bool hasRecordedOverlay(const RecordedWrite&);
    void recordLoopWrites();
    bool inheritStateFromBeforeLoop(BasicBlock* header, MemoryState&);
    bool mergeSameEpochStates(BasicBlock*, MemoryState& merged);
    void didWriteMemory(const HeapSet&, Origin);
    unsigned heapEpoch(unsigned heap);
    ValueKey keyForLoad(Kind, Type, MemoryValue*);
    static PureCallKey keyForPureCall(const PatchpointValue*);
    static bool isEqualLoad(const MemoryValue*, const Value*);
    Result replaceIfRedundant(Value*);
    Result replacePureCallIfRedundant(Value* current, PatchpointValue*);
    void validateReplacedLoads();
    void checkReplacedLoadsAtRunTime();
    bool isUnchangedSince(const ReplacedLoad&, const IndexSet<BasicBlock*>& reachable, Value*& clobber);

    Procedure& m_proc;
    Dominators* m_dominators { nullptr };
    const NaturalLoops* m_loops { nullptr };
    BasicBlock* m_block { nullptr };
    MemoryState m_memoryState;
    unsigned m_lastMemoryStateID { noID };
    Vector<MemoryState> m_memoryStateAtTail;
    Vector<LoopWrites> m_loopWrites;
    LayeredHashMap<ValueKey, Value*, MixingValueKeyHash> m_values;
    LayeredHashMap<PureCallKey, Value*, PureCallKeyHash> m_pureCalls;
    LayeredHashMap<unsigned, unsigned, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_heapEpochs;
    const bool m_validates;
    Vector<ReplacedLoad> m_replacedLoads;

    struct LoadToCheck {
        MovedLoad moved;
        Value* load;
        BasicBlock* block;
        Value* address;
        Value* replacement;
    };
    Vector<LoadToCheck> m_loadsToCheck;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
