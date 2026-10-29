/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTGrouping.h"
#include "AOTIR.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <concepts>
#include <iterator>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class BuildContext;
class ParallelLoops;

namespace Typed {

class BodyReferences;
class FlowResults;
struct EffectDescription;

enum class CallerSet : uint8_t { Unknown, Complete };
enum class EntrySet : uint8_t { Unknown, DirectCallers };
enum class CallSiteSet : uint8_t { Unknown, Complete };
enum class Recursion : uint8_t { Unknown, NotRecursive };

// Sorted lists of functions, read as one sorted list that names no function twice.
class MergedFunctionLists {
public:
    MergedFunctionLists() = default;
    explicit MergedFunctionLists(std::span<const FunctionID> own)
        : m_own(own)
    {
    }
    MergedFunctionLists(std::span<const FunctionID> own, std::span<const uint32_t> groups, const Vector<uint32_t>& firstEntry LIFETIME_BOUND, const Vector<FunctionID>& entries LIFETIME_BOUND)
        : m_own(own)
        , m_groups(groups)
        , m_firstEntry(&firstEntry)
        , m_entries(&entries)
    {
    }

    class iterator {
    public:
        using value_type = FunctionID;
        using difference_type = std::ptrdiff_t;

        iterator() = default;
        explicit iterator(const MergedFunctionLists& lists)
            : m_own(lists.m_own)
        {
            for (uint32_t group : lists.m_groups) {
                if (std::span<const FunctionID> list = lists.group(group); !list.empty())
                    m_others.append(Rest { list });
            }
            if (!m_others.isEmpty())
                findLeast();
        }

        FunctionID operator*() const { return m_others.isEmpty() ? m_own.front() : m_least; }
        iterator& operator++()
        {
            if (m_others.isEmpty()) [[likely]] {
                m_own = m_own.subspan(1);
                return *this;
            }
            auto pass = [&](std::span<const FunctionID>& list) {
                if (!list.empty() && list.front() == m_least)
                    list = list.subspan(1);
            };
            pass(m_own);
            for (Rest& rest : m_others)
                pass(rest.list);
            findLeast();
            return *this;
        }
        void operator++(int) { ++*this; }
        bool operator==(std::default_sentinel_t) const { return m_others.isEmpty() ? m_own.empty() : !m_least; }

    private:
        struct Rest {
            std::span<const FunctionID> list;
        };

        void findLeast()
        {
            m_least = m_own.empty() ? FunctionID() : m_own.front();
            for (const Rest& rest : m_others) {
                if (!rest.list.empty() && (!m_least || rest.list.front() < m_least))
                    m_least = rest.list.front();
            }
        }

        std::span<const FunctionID> m_own;
        Vector<Rest, 2> m_others;
        FunctionID m_least;
    };

    iterator begin() const { return iterator(*this); }
    std::default_sentinel_t end() const { return { }; }
    bool contains(FunctionID function) const
    {
        return std::ranges::binary_search(m_own, function) || std::ranges::any_of(m_groups, [&](uint32_t group) {
            return std::ranges::binary_search(this->group(group), function);
        });
    }

private:
    std::span<const FunctionID> group(uint32_t group) const { return m_entries->subspan((*m_firstEntry)[group], (*m_firstEntry)[group + 1] - (*m_firstEntry)[group]); }

    std::span<const FunctionID> m_own;
    std::span<const uint32_t> m_groups;
    const Vector<uint32_t>* m_firstEntry { nullptr };
    const Vector<FunctionID>* m_entries { nullptr };
};

class CallGraph {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CallGraph);
public:
    unsigned numberOfFunctions() const { return m_directCallSites.size(); }
    // What the operations of the body name or list. Beside them the body may run code that nobody knows: allCalleesOf() says where it does not.
    MergedFunctionLists calleesOf(FunctionID function) const LIFETIME_BOUND { return { ownCalleesOf(function), longSetsOf(function), m_longSets.firstMember, m_longSets.members }; }
    std::optional<ProvenValue<MergedFunctionLists>> allCalleesOf(FunctionID function) const LIFETIME_BOUND
    {
        return hasCompleteCalleeSet(function) ? std::optional(Proven::byClosedSet(calleesOf(function))) : std::nullopt;
    }
    bool hasCompleteCalleeSet(FunctionID function) const { return m_hasCompleteCalleeSet.get(function.index()); }
    MergedFunctionLists callersOf(FunctionID function) const LIFETIME_BOUND { return { ownCallersOf(function), longSetsWith(function), m_longSets.firstUser, m_longSets.users }; }

    // n calls that each name one set of n functions are n * n pairs of a caller and a callee. The graph keeps the functions of a long callee set once, and each caller names the set.
    // A walk that visits each long set once costs nothing per pair. A callee may be both among a caller's own and in its long sets.
    std::span<const FunctionID> ownCalleesOf(FunctionID function) const LIFETIME_BOUND { return m_callees.of(function); }
    std::span<const FunctionID> ownCallersOf(FunctionID function) const LIFETIME_BOUND { return m_callers.of(function); }
    unsigned numberOfLongSets() const { return m_longSets.firstMember.isEmpty() ? 0 : m_longSets.firstMember.size() - 1; }
    std::span<const uint32_t> longSetsOf(FunctionID caller) const LIFETIME_BOUND { return m_longSets.of(m_longSets.firstSetOfUser, m_longSets.setsOfUsers, caller); }
    std::span<const uint32_t> longSetsWith(FunctionID callee) const LIFETIME_BOUND { return m_longSets.of(m_longSets.firstSetOfMember, m_longSets.setsOfMembers, callee); }
    std::span<const FunctionID> membersOfLongSet(uint32_t set) const LIFETIME_BOUND { return entriesOfGroup(m_longSets.firstMember, m_longSets.members, set); }
    std::span<const FunctionID> usersOfLongSet(uint32_t set) const LIFETIME_BOUND { return entriesOfGroup(m_longSets.firstUser, m_longSets.users, set); }
    std::optional<uint32_t> findLongSet(CalleeSetID id) const
    {
        auto found = std::ranges::lower_bound(m_longSets.ids, id);
        return found != m_longSets.ids.end() && *found == id ? std::optional<uint32_t>(found - m_longSets.ids.begin()) : std::nullopt;
    }
    unsigned numberOfDirectCallSites(FunctionID function) const { return function.index() < m_directCallSites.size() ? m_directCallSites[function.index()] : 0; }
    unsigned numberOfCallSites(FunctionID function) const { return function.index() < m_callSites.size() ? m_callSites[function.index()] : 0; }
    unsigned numberOfCallSitesInCode(FunctionID function) const { return function.index() < m_callSites.size() ? m_callSites[function.index()] - m_callSitesOfHost[function.index()] : 0; }
    std::span<const FunctionID> roots() const LIFETIME_BOUND { return m_roots.span(); }
    std::span<const FunctionID> functionsCalledByUnknownCode() const LIFETIME_BOUND { return m_calledByUnknownCode.span(); }

    CallerSet callerSet(FunctionID function) const { return m_hasCompleteCallerSet.get(function.index()) ? CallerSet::Complete : CallerSet::Unknown; }
    EntrySet entrySet(FunctionID function) const { return m_hasOnlyDirectCallers.get(function.index()) ? EntrySet::DirectCallers : EntrySet::Unknown; }
    CallSiteSet callSiteSet(FunctionID function) const { return m_hasCompleteCallSiteSet.get(function.index()) ? CallSiteSet::Complete : CallSiteSet::Unknown; }
    bool isExpectedByCode(FunctionID function) const { return m_isExpectedByCode.get(function.index()); }

    void computeRanks(BuildContext&) const;

    Recursion recursion(FunctionID function) const
    {
        if (function.index() >= numberOfFunctions())
            return Recursion::Unknown;
        RELEASE_ASSERT(m_hasRanks);
        return m_isInCycle.get(function.index()) ? Recursion::Unknown : Recursion::NotRecursive;
    }

    unsigned bottomUpRank(FunctionID function) const
    {
        if (function.index() >= numberOfFunctions())
            return 0;
        RELEASE_ASSERT(m_hasRanks);
        return m_bottomUpRanks[function.index()];
    }
    void dump(PrintStream&, const Program&) const;

private:
    friend CallGraph buildCallGraph(const Program&, const FlowResults&, BuildContext&, BodyReferences*);

    struct Adjacency {
        std::span<const FunctionID> of(FunctionID function) const LIFETIME_BOUND
        {
            size_t index = function.index();
            if (index + 1 >= offsets.size())
                return { };
            if (uint32_t list = index < longListOf.size() ? longListOf[index] : 0)
                return longLists.subspan(longListOffsets[list - 1], longListOffsets[list] - longListOffsets[list - 1]);
            return targets.subspan(offsets[index], offsets[index + 1] - offsets[index]);
        }

        Adjacency inverse(const ParallelLoops&) const;
        uint32_t addLongList(std::span<const FunctionID>);
        void addInverseOfLongLists(Adjacency& result) const;

        Vector<uint32_t> offsets;
        Vector<FunctionID> targets;
        // A long list is kept apart, once for all the functions that have it: n functions that each may call any of the n have n equal lists, and so have their callers.
        // longListOf is empty, or holds one more than the number of the function's long list, or 0.
        Vector<uint32_t> longListOf;
        Vector<uint32_t> longListOffsets;
        Vector<FunctionID> longLists;
    };

    // The long callee sets that some body names, in the order of their IDs. The members are the functions with a body.
    struct LongSets {
        static std::span<const uint32_t> of(const Vector<uint32_t>& firstSet, const Vector<uint32_t>& sets LIFETIME_BOUND, FunctionID function)
        {
            return function.index() + 1 < firstSet.size() ? entriesOfGroup(firstSet, sets, function.index()) : std::span<const uint32_t>();
        }

        Vector<CalleeSetID> ids;
        Vector<uint32_t> firstMember;
        Vector<FunctionID> members;
        Vector<uint32_t> firstUser;
        Vector<FunctionID> users;
        Vector<uint32_t> firstSetOfUser;
        Vector<uint32_t> setsOfUsers;
        Vector<uint32_t> firstSetOfMember;
        Vector<uint32_t> setsOfMembers;
    };

    Adjacency m_callees;
    Adjacency m_callers;
    LongSets m_longSets;
    Vector<uint32_t> m_directCallSites;
    Vector<uint32_t> m_callSites;
    Vector<uint32_t> m_callSitesOfHost;
    Vector<FunctionID> m_roots;
    Vector<FunctionID> m_calledByUnknownCode;
    BitVector m_hasCompleteCallerSet;
    BitVector m_hasCompleteCalleeSet;
    BitVector m_hasOnlyDirectCallers;
    BitVector m_hasCompleteCallSiteSet;
    BitVector m_isExpectedByCode;
    mutable bool m_hasRanks { false };
    mutable BitVector m_isInCycle;
    mutable Vector<uint32_t> m_bottomUpRanks;
};

struct SiteThatRunsCode {
    BlockID block;
    uint32_t indexInBlock : 30 { 0 };
    uint32_t callsUnknownCode : 1 { 0 };
    uint32_t listsManyFunctions : 1 { 0 };
    uint32_t endOfFunctions { 0 };

    friend bool operator==(const SiteThatRunsCode&, const SiteThatRunsCode&) = default;
};
static_assert(sizeof(SiteThatRunsCode) == 12);

struct ReferencesOfBody {
    std::span<const FunctionID> functionsOfSite(size_t position) const LIFETIME_BOUND
    {
        uint32_t first = position ? sitesThatRunCode[position - 1].endOfFunctions : 0;
        return functionsOfSites.subspan(first, sitesThatRunCode[position].endOfFunctions - first);
    }

    // Without the functions of the long sets among calleeSets, unless something else names them.
    Vector<FunctionID> callees;
    Vector<FunctionID> directCallees;
    Vector<CalleeSetID> calleeSets;
    Vector<FunctionID> otherReferences;
    Vector<TypeID> expectedFunctionTypes;
    Vector<SiteThatRunsCode> sitesThatRunCode;
    Vector<FunctionID> functionsOfSites;
    bool callsUnknownCode { false };
    bool followsFieldsOfOperands { false };

    friend bool operator==(const ReferencesOfBody&, const ReferencesOfBody&) = default;
};

class BodyReferenceCollector {
    WTF_MAKE_NONCOPYABLE(BodyReferenceCollector);
public:
    explicit BodyReferenceCollector(const Program& program)
        : m_program(program)
    {
    }

    void read(const Value&, const EffectDescription&);
    void read(const Terminator&, const EffectDescription&);
    void skipTo(BlockID block, uint32_t indexInBlock)
    {
        m_block = block.index();
        m_indexInBlock = indexInBlock;
    }
    bool hasReadBody() const { return m_hasReadBody; }
    ReferencesOfBody finish(const FlowResults&, FunctionID);

private:
    AOT_CLASS_THRESHOLD(size_t, deduplicationThreshold, 4096);
    AOT_CLASS_THRESHOLD(size_t, mostFunctionsKeptOfSite, 16);

    void readInstruction(const auto&, const EffectDescription&);
    void add(FunctionID callee);
    void addReference(FunctionID target, FunctionReferenceKind);
    bool markSeen(FunctionID callee) { return !std::exchange(m_seen[callee.index()], static_cast<uint8_t>(1)); }

    const Program& m_program;
    ReferencesOfBody m_references;
    Vector<uint8_t> m_seen;
    uint32_t m_block { 0 };
    uint32_t m_indexInBlock { 0 };
    bool m_hasReadBody { false };
};

class BodyReferences {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodyReferences);
    WTF_MAKE_NONCOPYABLE(BodyReferences);
public:
    BodyReferences() = default;

    void didChange(std::span<const FunctionID>);
    bool followsFieldsOfOperands(FunctionID id) const { return id.index() < m_bodies.size() && m_bodies[id.index()].followsFieldsOfOperands; }
    bool keepsSomethingOf(FunctionID id) const { return id.index() < m_isChanged.size() && !m_isChanged[id.index()]; }
    size_t size() const { return m_bodies.size(); }
    const ReferencesOfBody& of(FunctionID id) const LIFETIME_BOUND { return m_bodies[id.index()]; }

    void prepareToTake(const Program&, const ParallelLoops&, std::span<const uint64_t> stampsOfRecords);
    void take(const Program&, FunctionID, ReferencesOfBody&&, uint64_t stampOfRecord);

private:
    friend CallGraph buildCallGraph(const Program&, const FlowResults&, BuildContext&, BodyReferences*);

    Vector<ReferencesOfBody> m_bodies;
    Vector<uint8_t> m_isChanged;
    Vector<uint64_t> m_stampsOfRecords;
    Vector<uint64_t> m_stampsOfRecordsForCallers;
};

CallGraph buildCallGraph(const Program&, const FlowResults&, BuildContext&, BodyReferences* kept = nullptr);

struct EntriesThatRunNoCode {
    OpcodeSet opcodes;
    TerminatorKindSet terminatorKinds;
    OpcodeSet opcodesWithReferences;
    TerminatorKindSet otherTerminatorKinds;

    EntriesThatRunNoCode() { completeOthers(); }
    bool contains(const Value& value) const { return opcodes.contains(value.opcode); }
    bool contains(const Terminator& terminator) const { return terminatorKinds.contains(terminator.kind); }
    void completeOthers()
    {
        opcodesWithReferences = Value::opcodesWithFunction | Value::opcodesWithCallees;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (!opcodes.contains(opcode))
                opcodesWithReferences.add(opcode);
        }
        otherTerminatorKinds = { };
        for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
            if (!terminatorKinds.contains(kind))
                otherTerminatorKinds.add(kind);
        }
    }
};

EntriesThatRunNoCode findEntriesThatRunNoCode();

ReferencesOfBody collectReferencesOf(const Program&, const FlowResults&, FunctionID, const EntriesThatRunNoCode&);

constexpr bool keepsFunctionAlive(FunctionReferenceKind kind)
{
    switch (kind) {
    case FunctionReferenceKind::Call:
    case FunctionReferenceKind::Closure:
    case FunctionReferenceKind::Constructor:
        return true;
    case FunctionReferenceKind::Dispatch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

void forEachFunctionInClassRecord(const Program& program, const Layout& layout, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor)
{
    if (layout.constructor)
        functor(layout.constructor, FunctionReferenceKind::Constructor);
    for (const ClassMember& member : layout.classFlow.members)
        functor(member.function, FunctionReferenceKind::Closure);
    for (const ClassMember& member : layout.classFlow.namedMembers) {
        if (member.function)
            functor(member.function, FunctionReferenceKind::Closure);
    }
    if (layout.flow.baseState != BaseState::Layout)
        return;
    if (FunctionID parent = program.at(declaredLayoutOf(program, layout.flow.base)).constructor)
        functor(parent, FunctionReferenceKind::Constructor);
}

// Whoever does the same for a function however often it is named may skip a callee set that it has read for another body.
void forEachFunctionReferenceAmong(const Program& program, const Function& function, const auto& forEachEntry, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor, const Invocable<bool(CalleeSetID)> auto& shouldRead)
{
    auto visitWritten = [&](FunctionID target, FunctionReferenceKind kind) {
        if (target && keepsFunctionAlive(kind))
            functor(target, kind);
    };
    Vector<LayoutID, 16> layouts;
    Vector<CalleeSetID, 16> calleeSets;
    auto visitNamed = [&]<typename Reference, typename... Role>(const Reference& reference, Role... role) {
        if constexpr (std::same_as<Reference, FunctionID>)
            visitWritten(reference, role...);
        else if constexpr (std::same_as<Reference, CalleeSetID>) {
            if (calleeSets.isEmpty() || calleeSets.last() != reference)
                calleeSets.append(reference);
        } else if constexpr (std::same_as<Reference, LayoutID>) {
            LayoutID layout = declaredLayoutOf(program, reference);
            if (layout && (layouts.isEmpty() || layouts.last() != layout))
                layouts.append(layout);
        } else if constexpr (std::same_as<Reference, VariableID>) {
            if (FunctionID declaration = program.at(reference).function)
                functor(declaration, FunctionReferenceKind::Closure);
        }
    };
    forEachEntry([&](const auto& entry) {
        walkReferences(entry, visitNamed);
    });

    AOT::sort(calleeSets);
    removeRepeatedElements(calleeSets);
    for (CalleeSetID set : calleeSets) {
        if (!shouldRead(set))
            continue;
        for (FunctionID callee : program.at(set).written().functions)
            functor(callee, FunctionReferenceKind::Dispatch);
    }

    const auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
    if (LayoutID constructed = entrypoint ? program.at(entrypoint->target).constructedClass : LayoutID())
        layouts.append(declaredLayoutOf(program, constructed));
    AOT::sort(layouts);
    removeRepeatedElements(layouts);
    for (LayoutID id : layouts)
        forEachFunctionInClassRecord(program, program.at(id), functor);

    forEachFunctionReference(function.creator, [&](FunctionID target, FunctionReferenceKind kind) {
        if (!entrypoint || target != entrypoint->target)
            visitWritten(target, kind);
    });
}

void forEachFunctionReference(const Program& program, const Function& function, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor, const Invocable<bool(CalleeSetID)> auto& shouldRead)
{
    forEachFunctionReferenceAmong(program, function, [&](const auto& visit) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                visit(function.at(id));
            visit(block.terminator);
        }
    }, functor, shouldRead);
}

void forEachFunctionReference(const Program& program, const Function& function, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor)
{
    forEachFunctionReference(program, function, functor, [](CalleeSetID) { return true; });
}

void forEachFunctionReference(const Program& program, FunctionID id, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor, const Invocable<bool(CalleeSetID)> auto& shouldRead)
{
    static const OpcodeSet wanted = [] {
        RELEASE_ASSERT((findTerminatorKindsNaming<FunctionID, CalleeSetID, LayoutID, VariableID>().isEmpty()));
        return findOpcodesNaming<FunctionID, CalleeSetID, LayoutID, VariableID>();
    }();
    forEachFunctionReferenceAmong(program, program.at(id), [&](const auto& visit) {
        forEachListedValue(program, id, wanted, [&](BlockID, uint32_t, ValueID, const Value& value) {
            visit(value);
        });
    }, functor, shouldRead);
}

void forEachFunctionReference(const Program& program, FunctionID id, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor)
{
    forEachFunctionReference(program, id, functor, [](CalleeSetID) { return true; });
}

void forEachCalledFunctionAmong(const Function& function, const auto& forEachEntry, const Invocable<void(FunctionID)> auto& functor)
{
    auto visitCalled = [&](FunctionID target, FunctionReferenceKind kind) {
        if (kind == FunctionReferenceKind::Call)
            functor(target);
    };
    forEachEntry([&](const auto& entry) {
        forEachFunctionReference(entry, visitCalled);
    });
    const auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
    forEachFunctionReference(function.creator, [&](FunctionID target, FunctionReferenceKind kind) {
        if (!entrypoint || target != entrypoint->target)
            visitCalled(target, kind);
    });
}

void forEachCalledFunction(const Function& function, const Invocable<void(FunctionID)> auto& functor)
{
    forEachCalledFunctionAmong(function, [&](const auto& visit) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                visit(function.at(id));
            visit(block.terminator);
        }
    }, functor);
}

void forEachCalledFunction(const Program& program, FunctionID id, const Invocable<void(FunctionID)> auto& functor)
{
    static const OpcodeSet wanted = [] {
        RELEASE_ASSERT(findTerminatorKindsNaming<FunctionID>().isEmpty());
        return findOpcodesNaming<FunctionID>();
    }();
    forEachCalledFunctionAmong(program.at(id), [&](const auto& visit) {
        forEachListedValue(program, id, wanted, [&](BlockID, uint32_t, ValueID, const Value& value) {
            visit(value);
        });
    }, functor);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
