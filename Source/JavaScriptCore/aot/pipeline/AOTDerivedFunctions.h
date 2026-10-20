/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDerivedFunctionList.h"
#include "AOTIR.h"
#include "AOTProvisionalFunctions.h"
#include <array>
#include <atomic>
#include <compare>
#include <memory>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/ConcurrentVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Hasher.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Threading.h>
#include <wtf/UniqueArray.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class DerivedFunctions;
class FlowResults;
class InterningPhases;
class StringArena;
class TypeArena;

enum class DerivedFunctionTarget : uint8_t { Function, Layout, Type, ValidationNode };
enum class DerivedFunctionCreation : uint8_t { OnRequest, ByProgramPass };

struct DerivedFunctionInputs {
    const Program& program;
    const FlowResults& flow;
    TypeArena& types;
    StringArena& strings;
    DerivedFunctions& derivedFunctions;
    BuildContext& build;
};

using DerivedFunctionBuilder = Function (*)(const DerivedFunctionKey&, DerivedFunctionInputs&);

struct DerivedFunctionRole {
    FunctionRole role;
    DerivedFunctionTarget target;
    DerivedFunctionCreation creation;
    DerivedFunctionBuilder build;
    Wire::StatisticID created;
};

const DerivedFunctionRole* findDerivedFunctionRole(FunctionRole);

#define AOT_DECLARE_DERIVED_FUNCTION_BUILDER(role, builder, target, created) Function builder(const DerivedFunctionKey&, DerivedFunctionInputs&);
FOR_EACH_AOT_FUNCTION_ROLE_DERIVED_ON_REQUEST(AOT_DECLARE_DERIVED_FUNCTION_BUILDER)
#undef AOT_DECLARE_DERIVED_FUNCTION_BUILDER

constexpr DerivedFunctionKey derivedFunctionKey(FunctionRole role, FunctionID target, uint32_t parameter = 0)
{
    return { .role = role, .target = target, .parameter = parameter };
}

constexpr DerivedFunctionKey derivedFunctionKey(FunctionRole role, LayoutID target)
{
    return { .role = role, .target = FunctionID(), .parameter = target.index() };
}

constexpr DerivedFunctionKey derivedFunctionKey(FunctionRole role, TypeID target)
{
    return { .role = role, .target = FunctionID(), .parameter = target.index() };
}

constexpr DerivedFunctionKey derivedFunctionKey(FunctionRole role, ExpectedResultID target, uint32_t node)
{
    return { .role = role, .target = FunctionID(), .parameter = target.index(), .node = node };
}

struct DerivedFunctionCommit {
    FunctionRemap remap;
    Vector<std::unique_ptr<Function>> functions;
    Vector<unsigned> tasks;
    FunctionID firstID;
};

class DerivedFunctions final : public ProvisionalFunctions {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DerivedFunctions);
    WTF_MAKE_NONCOPYABLE(DerivedFunctions);
    WTF_MAKE_NONMOVABLE(DerivedFunctions);
public:
    explicit DerivedFunctions(BuildContext&);
    ~DerivedFunctions();

    FunctionID find(const DerivedFunctionKey&) const;
    FunctionID ensure(const DerivedFunctionKey&);
    const Function& at(FunctionID) const final;
    DerivedFunctionKey keyOf(FunctionID provisional) const;
    std::weak_ordering compare(FunctionID, FunctionID) const;

    void record(const DerivedFunctionKey&, FunctionID);
    void recordFunctionsOf(const Program&);
    void forgetFunctionsWithoutBody(const Program&);

private:
    friend class DerivedFunctionPhase;

    enum class State : uint8_t { Requested, Building, Published };

    struct Entry {
        unsigned optimizeTask() const { return buildTask + 1; }

        DerivedFunctionKey key { };
        unsigned buildTask { 0 };
        Atomic<State> state { State::Requested };
        std::atomic<const Thread*> builder { nullptr };
        std::atomic<const Function*> published { nullptr };
        std::unique_ptr<Function> asBuilt;
        std::unique_ptr<Function> optimized;
    };

    struct PhaseState {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PhaseState);

        PhaseState(Program& program, const FlowResults& flow, InterningPhases& interning)
            : program(program)
            , flow(flow)
            , interning(interning)
        {
        }

        uint32_t size() const { return numberOfEntries.load(std::memory_order_acquire); }
        uint32_t add(const DerivedFunctionKey&);
        void createEntriesAheadOf(uint32_t ordinal);
        void createEntries() WTF_REQUIRES_LOCK(creationLock);

        Program& program;
        const FlowResults& flow;
        InterningPhases& interning;
        ConcurrentVector<Entry, 64> entries;
        std::atomic<uint32_t> numberOfEntries { 0 };
        std::atomic<uint32_t> numberOfCreatedEntries { 0 };
        Lock creationLock;
    };

    // Every reference to a closure asks for an id, from every thread. Looking takes no lock: a thread that loses its processor while it holds
    // one stops every thread that comes for it. One thread at a time may add, while others look.
    class IDTable {
        WTF_MAKE_NONCOPYABLE(IDTable);
        WTF_MAKE_NONMOVABLE(IDTable);
    public:
        IDTable() = default;

        FunctionID get(const DerivedFunctionKey&, uint32_t hash) const;
        void add(const DerivedFunctionKey&, uint32_t hash, FunctionID);
        void set(const DerivedFunctionKey&, uint32_t hash, FunctionID);
        void removeIfWhileNobodyLooks(NOESCAPE const ScopedLambda<bool(FunctionID)>&);

    private:
        struct Slot {
            std::atomic<uint32_t> id { FunctionID().index() };
            DerivedFunctionKey key { };
        };

        struct Slots {
            WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Slots);

            explicit Slots(size_t capacity);
            Slot& findWhileNobodyAdds(const DerivedFunctionKey&, uint32_t hash) const LIFETIME_BOUND;

            UniqueArray<Slot> slots;
            size_t mask;
        };

        Slot& findForAdding(const DerivedFunctionKey&, uint32_t hash) LIFETIME_BOUND;
        void replaceSlots(size_t capacity, NOESCAPE const ScopedLambda<bool(FunctionID)>& shouldRemove);

        std::atomic<const Slots*> m_current { nullptr };
        // A thread may still be looking in one that has been replaced.
        Vector<std::unique_ptr<Slots>> m_allSlots;
        unsigned m_size { 0 };
    };

    struct Shard {
        Lock lock;
        IDTable ids;
        uint8_t restOfCacheLine[24];
    };
    static_assert(!(sizeof(Shard) % 64));
    static constexpr unsigned numberOfShardsLog2 = 10;

    static uint32_t hashOf(const DerivedFunctionKey& key) { return computeHash(static_cast<uint32_t>(key.role), key.target.index(), key.parameter, key.node); }
    Shard& shardOf(uint32_t hash) const;
    void removeIDsIf(NOESCAPE const ScopedLambda<bool(FunctionID)>&);
    Entry& entryOf(FunctionID) const;
    const Function& buildOrWait(Entry&) const;
    std::unique_ptr<Function> build(const Entry&) const;
    std::weak_ordering compareKeys(const DerivedFunctionKey&, const DerivedFunctionKey&) const;

    BuildContext& m_build;
    mutable std::array<Shard, 1 << numberOfShardsLog2> m_shards;
    std::unique_ptr<PhaseState> m_phase;
};

class DerivedFunctionPhase {
    WTF_MAKE_NONCOPYABLE(DerivedFunctionPhase);
    WTF_MAKE_NONMOVABLE(DerivedFunctionPhase);
public:
    DerivedFunctionPhase(DerivedFunctions&, Program&, const FlowResults&, InterningPhases&);
    ~DerivedFunctionPhase();

    unsigned numberOfRequests() const;
    unsigned taskOf(FunctionID provisional) const;
    void setOptimizedBody(FunctionID provisional, std::unique_ptr<Function>);
    DerivedFunctionCommit commit(const BitVector& reachedOrdinals);

private:
    void buildRequestedFunctions();
    Vector<uint32_t> ordinalsInKeyOrder() const;
    void replaceProvisionalIDs(const BitVector& reachedOrdinals, const FunctionRemap&);

    DerivedFunctions& m_table;
    bool m_didCommit { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
