/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIR.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTSchemaWalkers.h"
#include <algorithm>
#include <concepts>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;
class ParallelLoops;

namespace Typed {

class BodyReferences;
class FlowResults;
struct EffectDescription;

enum class CallerSet : uint8_t { Unknown, Complete };
enum class CalleeSetState : uint8_t { Unknown, Complete };
enum class EntrySet : uint8_t { Unknown, DirectCallers };
enum class CallSiteSet : uint8_t { Unknown, Complete };
enum class Recursion : uint8_t { Unknown, NotRecursive };

class CallGraph {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CallGraph);
public:
    CallGraph() = default;

    unsigned numberOfFunctions() const { return m_directCallSites.size(); }
    std::span<const FunctionID> calleesOf(FunctionID function) const LIFETIME_BOUND { return m_callees.of(function); }
    std::span<const FunctionID> callersOf(FunctionID function) const LIFETIME_BOUND { return m_callers.of(function); }
    unsigned numberOfDirectCallSites(FunctionID function) const { return function.index() < m_directCallSites.size() ? m_directCallSites[function.index()] : 0; }
    // Every operation that may enter the function: a direct call, a call by value whose callee set lists it, an access or a built-in that flow linked to it.
    unsigned numberOfCallSites(FunctionID function) const { return function.index() < m_callSites.size() ? m_callSites[function.index()] : 0; }
    // The host enters a function through an entrypoint.
    unsigned numberOfCallSitesInCode(FunctionID function) const { return function.index() < m_callSites.size() ? m_callSites[function.index()] - m_callSitesOfHost[function.index()] : 0; }
    std::span<const FunctionID> roots() const LIFETIME_BOUND { return m_roots.span(); }
    std::span<const FunctionID> functionsCalledByUnknownCode() const LIFETIME_BOUND { return m_calledByUnknownCode.span(); }

    CallerSet callerSet(FunctionID function) const { return m_hasCompleteCallerSet.get(function.index()) ? CallerSet::Complete : CallerSet::Unknown; }
    CalleeSetState calleeSet(FunctionID function) const { return m_hasCompleteCalleeSet.get(function.index()) ? CalleeSetState::Complete : CalleeSetState::Unknown; }
    EntrySet entrySet(FunctionID function) const { return m_hasOnlyDirectCallers.get(function.index()) ? EntrySet::DirectCallers : EntrySet::Unknown; }
    CallSiteSet callSiteSet(FunctionID function) const { return m_hasCompleteCallSiteSet.get(function.index()) ? CallSiteSet::Complete : CallSiteSet::Unknown; }
    // Some value is expected to be the function. A call of such a value is in no count: it tests for the function and calls it by name once its caller is optimized.
    bool isExpectedByCode(FunctionID function) const { return m_isExpectedByCode.get(function.index()); }

    // Few graphs are asked for recursion() or bottomUpRank(). Whoever asks calls this first, on the thread that starts the loops, before any other thread reads the graph.
    void computeRanks(BuildContext&) const;

    // Unknown code counts as a node: a function that reaches it and is reachable from it is in a cycle.
    Recursion recursion(FunctionID function) const
    {
        if (function.index() >= numberOfFunctions())
            return Recursion::Unknown;
        RELEASE_ASSERT(m_hasRanks);
        return m_isInCycle.get(function.index()) ? Recursion::Unknown : Recursion::NotRecursive;
    }

    // Functions in the same cycle share a rank. Any other callee ranks below its caller.
    unsigned bottomUpRank(FunctionID function) const
    {
        if (function.index() >= numberOfFunctions())
            return 0;
        RELEASE_ASSERT(m_hasRanks);
        return m_bottomUpRanks[function.index()];
    }
    // Of a graph with ranks.
    void dump(PrintStream&, const Program&) const;

private:
    friend CallGraph buildCallGraph(const Program&, const FlowResults&, BuildContext&, BodyReferences*);

    struct Adjacency {
        std::span<const FunctionID> of(FunctionID function) const LIFETIME_BOUND
        {
            size_t index = function.index();
            if (index + 1 >= offsets.size())
                return { };
            return targets.subspan(offsets[index], offsets[index + 1] - offsets[index]);
        }

        Adjacency inverse(const ParallelLoops&) const;

        Vector<uint32_t> offsets;
        Vector<FunctionID> targets;
    };

    Adjacency m_callees;
    Adjacency m_callers;
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

// An operation or a terminator whose description lists a function or unknown code.
struct SiteThatRunsCode {
    BlockID block;
    // Of a terminator: the number of operations in its block.
    uint32_t indexInBlock : 30 { 0 };
    uint32_t callsUnknownCode : 1 { 0 };
    // The description lists more functions than are kept. Whoever wants them describes the entry again.
    uint32_t listsManyFunctions : 1 { 0 };
    // In ReferencesOfBody::functionsOfSites. The functions of a site begin where those of the site in front of it end.
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

    // Ascending, each once, whether or not it has a body.
    Vector<FunctionID> callees;
    // An entry for each site.
    Vector<FunctionID> directCallees;
    Vector<CalleeSetID> calleeSets;
    Vector<FunctionID> otherReferences;
    // Ascending, each once: the expected types of values that list functions.
    Vector<TypeID> expectedFunctionTypes;
    // Block by block as the function lists them, the operations of a block in front of its terminator.
    Vector<SiteThatRunsCode> sitesThatRunCode;
    // EffectDescription::functions of the sites, site by site.
    Vector<FunctionID> functionsOfSites;
    bool callsUnknownCode { false };
    bool followsFieldsOfOperands { false };

    friend bool operator==(const ReferencesOfBody&, const ReferencesOfBody&) = default;
};

// Collects what one body names. It is told every operation and every terminator of the body, block by block, each with what describeEffects() says of it.
// Past deduplicationThreshold callees it switches to a seen-set, so large callee sets repeated at many call sites stay cheap.
class BodyReferenceCollector {
    WTF_MAKE_NONCOPYABLE(BodyReferenceCollector);
public:
    explicit BodyReferenceCollector(const Program&);

    void read(const Value&, const EffectDescription&);
    void read(const Terminator&, const EffectDescription&);
    // The next entry that is read stands there. What is left out in between has an empty description and names neither a function nor a set of callees.
    void skipTo(BlockID block, uint32_t indexInBlock)
    {
        m_block = block.index();
        m_indexInBlock = indexInBlock;
    }
    bool hasReadBody() const { return m_hasReadBody; }
    ReferencesOfBody finish(const FlowResults&, FunctionID);

private:
    static constexpr size_t deduplicationThreshold = 4096;
    static constexpr size_t mostFunctionsKeptOfSite = 16;

    void readInstruction(const auto&, const EffectDescription&);
    void noteSite(const EffectDescription&);
    void add(FunctionID callee);
    void addReference(FunctionID target, FunctionReferenceKind);
    void addMembersOfCalleeSets();
    void addIndirectCallees(const FlowResults&, FunctionID);
    void addExpectedFunctionTypes(FunctionID);
    bool markSeen(FunctionID callee) { return !std::exchange(m_seen[callee.index()], static_cast<uint8_t>(1)); }
    void startDeduplicating();

    const Program& m_program;
    ReferencesOfBody m_references;
    Vector<uint8_t> m_seen;
    uint32_t m_block { 0 };
    uint32_t m_indexInBlock { 0 };
    bool m_hasReadBody { false };
};

// What each body names, kept from one call graph to the next. Whoever keeps this says which functions changed.
class BodyReferences {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodyReferences);
    WTF_MAKE_NONCOPYABLE(BodyReferences);
public:
    BodyReferences() = default;

    void didChange(std::span<const FunctionID>);
    // What is kept of the body also came from the records of layouts that the body does not name: those that the fields of an operand hold, and so on.
    bool followsFieldsOfOperands(FunctionID id) const { return id.index() < m_bodies.size() && m_bodies[id.index()].followsFieldsOfOperands; }
    // False where the function counts as changed already.
    bool keepsSomethingOf(FunctionID id) const { return id.index() < m_isChanged.size() && !m_isChanged[id.index()]; }
    // They describe the bodies as they were when the latest call graph was made from this.
    size_t size() const { return m_bodies.size(); }
    const ReferencesOfBody& of(FunctionID id) const LIFETIME_BOUND { return m_bodies[id.index()]; }

    // For somebody who reads bodies anyway and has collected what they name. First the stamps of the records of all functions as they are now, then the bodies, each from any one thread.
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

// The row of such an entry declares all that it does, and neither the row nor an operand runs code. So its description lists no function and no unknown code, whatever its operands are.
struct EntriesThatRunNoCode {
    OpcodeSet opcodes;
    TerminatorKindSet terminatorKinds;
    // All others, and the operations among these that name a function or a set of callees. No terminator names either.
    OpcodeSet opcodesWithReferences;
    TerminatorKindSet otherTerminatorKinds;

    EntriesThatRunNoCode() { completeOthers(); }
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

// What buildCallGraph() keeps of a body: for somebody who has the body in the cache. See BodyReferences::take().
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

// What the run time makes from the record of a class: the class object, the closures of the members and, before them, the class above.
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

void forEachFunctionReferenceAmong(const Program& program, const Function& function, const auto& forEachEntry, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor)
{
    auto visitWritten = [&](FunctionID target, FunctionReferenceKind kind) {
        if (target && keepsFunctionAlive(kind))
            functor(target, kind);
    };
    Vector<LayoutID, 16> layouts;
    Vector<CalleeSetID, 16> calleeSets;
    // No instruction names both a function and a variable, so the functor is told of them in the order of the instructions.
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
            // The value of a function declaration's variable is the closure, which the first read creates.
            if (FunctionID declaration = program.at(reference).function)
                functor(declaration, FunctionReferenceKind::Closure);
        }
    };
    forEachEntry([&](const auto& entry) {
        walkReferences(entry, visitNamed);
    });

    std::ranges::sort(calleeSets);
    removeRepeatedElements(calleeSets);
    for (CalleeSetID set : calleeSets) {
        for (FunctionID callee : program.at(set).functions)
            functor(callee, FunctionReferenceKind::Dispatch);
    }

    // The run time knows a class object by the entrypoint of its constructor, and makes it from the record of the class.
    const auto* entrypoint = function.creator.tryAs<CreatorRecord::Entrypoint>();
    if (LayoutID constructed = entrypoint ? program.at(entrypoint->target).constructedClass : LayoutID())
        layouts.append(declaredLayoutOf(program, constructed));
    std::ranges::sort(layouts);
    removeRepeatedElements(layouts);
    for (LayoutID id : layouts)
        forEachFunctionInClassRecord(program, program.at(id), functor);

    // An entrypoint enters its target by a call in its body, or holds a copy of it.
    forEachFunctionReference(function.creator, [&](FunctionID target, FunctionReferenceKind kind) {
        if (!entrypoint || target != entrypoint->target)
            visitWritten(target, kind);
    });
}

void forEachFunctionReference(const Program& program, const Function& function, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor)
{
    forEachFunctionReferenceAmong(program, function, [&](const auto& visit) {
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                visit(function.at(id));
            visit(block.terminator);
        }
    }, functor);
}

void forEachFunctionReference(const Program& program, FunctionID id, const Invocable<void(FunctionID, FunctionReferenceKind)> auto& functor)
{
    static const OpcodeSet wanted = [] {
        RELEASE_ASSERT((findTerminatorKindsNaming<FunctionID, CalleeSetID, LayoutID, VariableID>().isEmpty()));
        return findOpcodesNaming<FunctionID, CalleeSetID, LayoutID, VariableID>();
    }();
    forEachFunctionReferenceAmong(program, program.at(id), [&](const auto& visit) {
        forEachListedValue(program, id, wanted, [&](BlockID, uint32_t, ValueID, const Value& value) {
            visit(value);
        });
    }, functor);
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
