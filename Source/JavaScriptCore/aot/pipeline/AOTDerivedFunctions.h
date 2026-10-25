/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTProvisionalFunctions.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <compare>
#include <memory>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/ConcurrentVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Hasher.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Threading.h>
#include <wtf/UniqueArray.h>

#define FOR_EACH_AOT_FUNCTION_ROLE_DERIVED_ON_REQUEST(v) \
    v(GenericEntrypoint, buildGenericEntrypoint, Function, GenericEntrypoints) \
    v(AllocatedThisVersion, buildAllocatedThisVersion, Function, None) \
    v(ConstructBody, buildConstructBody, Function, None) \
    v(CalleeVersion, buildCalleeVersion, FunctionAndCallee, CalleeVersions) \
    v(TableEntrypoint, buildTableEntrypoint, FunctionAndSelector, TableEntrypoints) \
    v(TableLookup, buildTableLookup, Selector, TableLookups) \
    v(BoundFunction, buildBoundFunction, Function, BoundFunctions) \
    v(ResultWorker, buildResultWorker, Function, FreshResultsSplit) \
    v(ChainStep, buildChainStep, Function, None) \
    v(OutlinedBody, buildOutlinedIteration, Selector, None) \
    v(Serializer, buildSerializer, Layout, SerializersCreated) \
    v(Parser, buildParser, Type, ParsersCreated)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class DerivedFunctions;
class FlowResults;
class InterningPhases;
class StringArena;
class TypeArena;
struct InliningSummary;

enum class DerivedFunctionTarget : uint8_t { Function, FunctionAndCallee, Layout, Type, Selector, FunctionAndSelector };
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
    explicit DerivedFunctions(BuildContext& build)
        : m_build(build)
    {
    }

    ~DerivedFunctions() { RELEASE_ASSERT(!m_phase); }

    FunctionID find(const DerivedFunctionKey&) const;
    FunctionID ensure(const DerivedFunctionKey&);
    const Function& at(FunctionID) const final;
    // Of the function as at() answers it. The first who asks makes it, once.
    const InliningSummary& inliningSummaryOf(FunctionID provisional, NOESCAPE const ScopedLambda<InliningSummary()>& summarize) const;
    DerivedFunctionKey keyOf(FunctionID provisional) const { return entryOf(provisional).key; }
    std::weak_ordering compare(FunctionID, FunctionID) const;

    void record(const DerivedFunctionKey&, FunctionID);
    void recordFunctionsOf(const Program&);
    void forgetFunctionsWithoutBody(const Program&);

private:
    friend class DerivedFunctionPhase;
    friend class DerivedFunctionRequester;

    enum class State : uint8_t { Requested, Building, Published };
    enum class RequestHandling : uint8_t { Confirm, Record };

    // Only a confirmed entry gets a final id. Building an entry records the requests that its builder makes. They are confirmed when the entry is.
    struct Entry {
        enum Flag : uint8_t {
            Confirmed = 1 << 0,
            Built = 1 << 1,
        };

        ~Entry();

        unsigned optimizeTask() const { return buildTask + 1; }
        bool isConfirmed() const { return flags.load(std::memory_order_relaxed) & Confirmed; }

        DerivedFunctionKey key { };
        unsigned buildTask { 0 };
        Atomic<State> state { State::Requested };
        std::atomic<uint8_t> flags { 0 };
        std::atomic<const Thread*> builder { nullptr };
        std::atomic<const Function*> published { nullptr };
        std::unique_ptr<Function> asBuilt;
        std::unique_ptr<Function> optimized;
        Vector<uint32_t> nestedRequests;
        Lock inliningSummaryLock;
        std::atomic<const InliningSummary*> publishedInliningSummary { nullptr };
        std::unique_ptr<const InliningSummary> inliningSummary WTF_GUARDED_BY_LOCK(inliningSummaryLock);
    };

    // Handles the requests that its thread makes while it is the innermost one on the stack. Without one, a request is confirmed immediately.
    class RequestRecorder {
        WTF_MAKE_NONCOPYABLE(RequestRecorder);
        WTF_MAKE_NONMOVABLE(RequestRecorder);
    public:
        explicit RequestRecorder(RequestHandling handling)
            : handling(handling)
            , m_outer(std::exchange(s_current, this))
        {
        }

        ~RequestRecorder()
        {
            RELEASE_ASSERT(s_current == this);
            s_current = m_outer;
        }

        void record(uint32_t ordinal)
        {
            if (m_recordedOrdinals.add(ordinal).isNewEntry)
                m_requests.append(ordinal);
        }

        Vector<uint32_t> takeRequests()
        {
            m_recordedOrdinals.clear();
            return std::exchange(m_requests, { });
        }

        static thread_local RequestRecorder* s_current;
        const RequestHandling handling;

    private:
        RequestRecorder* const m_outer;
        Vector<uint32_t> m_requests;
        UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_recordedOrdinals;
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
        void confirm(uint32_t ordinal);

        Program& program;
        const FlowResults& flow;
        InterningPhases& interning;
        ConcurrentVector<Entry, 64> entries;
        std::atomic<uint32_t> numberOfEntries { 0 };
        std::atomic<uint32_t> numberOfCreatedEntries { 0 };
        Lock creationLock;
        Lock recordedRequestsLock;
        UncheckedKeyHashMap<FunctionID, Vector<uint32_t>> recordedRequests WTF_GUARDED_BY_LOCK(recordedRequestsLock);
    };

    // Every reference to a closure asks for an id, from every thread.
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
    // The high bits: a shard's table places its keys by the low ones.
    Shard& shardOf(uint32_t hash) const { return m_shards[hash >> (32 - numberOfShardsLog2)]; }
    FunctionID findOrAdd(const DerivedFunctionKey&);
    void removeIDsIf(NOESCAPE const ScopedLambda<bool(FunctionID)>&);
    Entry& entryOf(FunctionID) const;
    const Function& buildOrWait(Entry&) const;
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

    unsigned numberOfRequests() const { return m_table.m_phase->size(); }
    unsigned taskOf(FunctionID provisional) const { return m_table.entryOf(provisional).optimizeTask(); }
    void setOptimizedBody(FunctionID provisional, std::unique_ptr<Function> function) { m_table.entryOf(provisional).optimized = WTF::move(function); }
    void confirmRequestsOf(FunctionID requester);
    DerivedFunctionCommit commit(const BitVector& reachedOrdinals);

private:
    friend class DerivedFunctionRequester;

    DerivedFunctions& m_table;
    bool m_didCommit { false };
};

// Lives on the stack of the thread that optimizes the requester. The requests of an unconfirmed requester are dropped unless confirmRequestsOf() is called for it.
class DerivedFunctionRequester {
    WTF_MAKE_NONCOPYABLE(DerivedFunctionRequester);
    WTF_MAKE_NONMOVABLE(DerivedFunctionRequester);
public:
    DerivedFunctionRequester(DerivedFunctionPhase&, FunctionID requester, bool isConfirmed);
    ~DerivedFunctionRequester();

private:
    DerivedFunctionPhase& m_phase;
    const FunctionID m_requester;
    DerivedFunctions::RequestRecorder m_recorder;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
