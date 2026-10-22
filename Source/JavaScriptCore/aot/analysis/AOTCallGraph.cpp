/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallGraph.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTExactBuffer.h"
#include "AOTFlowResults.h"
#include "AOTLargeVectors.h"
#include "AOTPhaseTimes.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include <atomic>
#include <functional>
#include <numeric>
#include <optional>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/Atomics.h>
#include <wtf/EnumTraits.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CollectedReferences {
    CollectedReferences(unsigned numberOfFunctions, unsigned numberOfCalleeSets)
        : calleeOffsets(FillWith { }, numberOfFunctions + 1, 0u)
        , directCallSites(FillWith { }, numberOfFunctions, 0u)
        , linkedCallSites(FillWith { }, numberOfFunctions, 0u)
        , calleeSetCallSites(FillWith { }, numberOfCalleeSets, 0u)
        , hasOtherReference(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0))
        , callsUnknownCode(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0))
    {
    }

    Vector<uint32_t> calleeOffsets;
    Vector<FunctionID> callees;
    Vector<uint32_t> directCallSites;
    Vector<uint32_t> linkedCallSites;
    Vector<uint32_t> calleeSetCallSites;
    Vector<uint8_t> hasOtherReference;
    Vector<uint8_t> callsUnknownCode;
};

struct CallEdge {
    uint32_t target;
    uint32_t source;
};

struct FunctionRanks {
    Vector<uint32_t> ranks;
    Vector<uint8_t> isInCycle;
    Vector<uint8_t> reachesUnknownCode;
    uint32_t numberOfRanks { 0 };
};

class SuccessorList {
public:
    SuccessorList(std::span<const FunctionID> functions, std::optional<unsigned> unknownCodeNode)
        : m_functions(functions)
        , m_unknownCodeNode(unknownCodeNode)
    {
    }

    size_t size() const { return m_functions.size() + (m_unknownCodeNode ? 1 : 0); }
    unsigned operator[](size_t position) const { return position < m_functions.size() ? m_functions[position].index() : *m_unknownCodeNode; }

private:
    std::span<const FunctionID> m_functions;
    std::optional<unsigned> m_unknownCodeNode;
};

} // anonymous namespace

BodyReferenceCollector::BodyReferenceCollector(const Program& program)
    : m_program(program)
{
}

void BodyReferenceCollector::add(FunctionID callee)
{
    Vector<FunctionID>& callees = m_references.callees;
    if (!m_seen.isEmpty()) {
        if (markSeen(callee))
            callees.append(callee);
        return;
    }
    callees.append(callee);
    if (callees.size() >= deduplicationThreshold)
        startDeduplicating();
}

void BodyReferenceCollector::startDeduplicating()
{
    Vector<FunctionID>& callees = m_references.callees;
    m_seen.fill(0, m_program.functions.size());
    size_t end = 0;
    for (size_t index = 0; index < callees.size(); ++index) {
        FunctionID callee = callees[index];
        if (markSeen(callee))
            callees[end++] = callee;
    }
    callees.shrink(end);
}

void BodyReferenceCollector::addReference(FunctionID target, FunctionReferenceKind kind)
{
    switch (kind) {
    case FunctionReferenceKind::Call:
        m_references.directCallees.append(target);
        add(target);
        return;
    case FunctionReferenceKind::Closure:
    case FunctionReferenceKind::Constructor:
        m_references.otherReferences.append(target);
        return;
    case FunctionReferenceKind::Dispatch:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BodyReferenceCollector::readInstruction(const auto& instruction, const EffectDescription& description)
{
    m_hasReadBody = true;
    forEachFunctionReference(instruction, [&](FunctionID target, FunctionReferenceKind kind) {
        addReference(target, kind);
    });
    forEachCalleeSet(instruction, [&](CalleeSetID set) {
        m_references.calleeSets.append(set);
    });
    for (FunctionID callee : description.functions)
        add(callee);
    for (FunctionID callee : description.slowPathFunctions)
        add(callee);
    if (description.callsUnknownCode)
        m_references.callsUnknownCode = true;
    if (description.followsFieldsOfOperands)
        m_references.followsFieldsOfOperands = true;
    if (description.callsUnknownCode || !description.functions.isEmpty())
        noteSite(description);
}

void BodyReferenceCollector::noteSite(const EffectDescription& description)
{
    RELEASE_ASSERT(m_indexInBlock < 1u << 30);
    bool listsManyFunctions = description.functions.size() > mostFunctionsKeptOfSite;
    if (!listsManyFunctions)
        m_references.functionsOfSites.append(description.functions.span());
    m_references.sitesThatRunCode.append(SiteThatRunsCode { BlockID(m_block), m_indexInBlock, description.callsUnknownCode, listsManyFunctions, static_cast<uint32_t>(m_references.functionsOfSites.size()) });
}

void BodyReferenceCollector::read(const Value& value, const EffectDescription& description)
{
    readInstruction(value, description);
    ++m_indexInBlock;
}

void BodyReferenceCollector::read(const Terminator& terminator, const EffectDescription& description)
{
    readInstruction(terminator, description);
    ++m_block;
    m_indexInBlock = 0;
}

void BodyReferenceCollector::addMembersOfCalleeSets()
{
    Vector<CalleeSetID>& sets = m_references.calleeSets;
    std::ranges::sort(sets);
    for (size_t index = 0; index < sets.size(); ++index) {
        if (index && sets[index] == sets[index - 1])
            continue;
        for (FunctionID callee : m_program.at(sets[index]).functions)
            add(callee);
    }
}

void BodyReferenceCollector::addIndirectCallees(const FlowResults& flow, FunctionID id)
{
    Vector<FunctionID, 8> sources;
    auto addSource = [&](FunctionID source) {
        if (FunctionID asRead = findFunctionAsRead(&m_program, source))
            sources.append(asRead);
    };
    addSource(id);
    for (const InlineFrame& frame : m_program.at(id).inlineFrames)
        addSource(frame.callee);
    std::ranges::sort(sources);
    removeRepeatedElements(sources);
    for (FunctionID source : sources) {
        for (const FlowIndirectCall& call : flow.indirectCallsFrom(source))
            add(call.callee);
    }
}

void BodyReferenceCollector::addExpectedFunctionTypes(FunctionID id)
{
    if (!mayHoldExpectedTypes(m_program, id))
        return;
    Vector<TypeID>& types = m_references.expectedFunctionTypes;
    for (const Value& value : m_program.at(id).values) {
        TypeID expected = value.expected();
        if (expected && (types.isEmpty() || types.last() != expected) && !m_program.at(expected).functions.isEmpty())
            types.append(expected);
    }
    std::ranges::sort(types);
    removeRepeatedElements(types);
    moveToExactBuffer(types);
}

ReferencesOfBody BodyReferenceCollector::finish(const FlowResults& flow, FunctionID id)
{
    addMembersOfCalleeSets();
    addIndirectCallees(flow, id);
    addExpectedFunctionTypes(id);
    std::ranges::sort(m_references.callees);
    removeRepeatedElements(m_references.callees);
    std::ranges::sort(m_references.otherReferences);
    removeRepeatedElements(m_references.otherReferences);
    moveToExactBuffer(m_references.callees);
    moveToExactBuffer(m_references.directCallees);
    moveToExactBuffer(m_references.calleeSets);
    moveToExactBuffer(m_references.otherReferences);
    moveToExactBuffer(m_references.sitesThatRunCode);
    moveToExactBuffer(m_references.functionsOfSites);
    return WTF::move(m_references);
}

EntriesThatRunNoCode findEntriesThatRunNoCode()
{
    EntriesThatRunNoCode entries;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (effectRuleOf(opcode) == EffectRule::Declared && !declaredBoundOf(opcode).mayCallUnknownCode && !mayRunOperandCode(opcode))
            entries.opcodes.add(opcode);
    }
    for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
        if (effectRuleOf(kind) != EffectRule::Inline && !declaredBoundOf(kind).mayCallUnknownCode && !mayRunOperandCode(kind))
            entries.terminatorKinds.add(kind);
    }
    entries.completeOthers();
    return entries;
}

ReferencesOfBody collectReferencesOf(const Program& program, const FlowResults& flow, FunctionID id, const EntriesThatRunNoCode& entriesThatRunNoCode)
{
    const Function& function = program.at(id);
    if (!function.hasBody())
        return { };
    BodyReferenceCollector collector(program);
    const EffectDescription descriptionWithoutCode;
    if (!mayEndBlockWith(program, id, entriesThatRunNoCode.otherTerminatorKinds)) {
        forEachListedValue(program, id, entriesThatRunNoCode.opcodesWithReferences, [&](BlockID block, uint32_t indexInBlock, ValueID, const Value& value) {
            collector.skipTo(block, indexInBlock);
            if (entriesThatRunNoCode.opcodes.contains(value.opcode))
                collector.read(value, descriptionWithoutCode);
            else
                collector.read(value, describeCodeRunBy(program, function, value));
        });
        return collector.finish(flow, id);
    }
    for (const Block& block : function.blocks) {
        for (ValueID listed : block.values) {
            const Value& value = function.at(listed);
            if (entriesThatRunNoCode.opcodes.contains(value.opcode))
                collector.read(value, descriptionWithoutCode);
            else
                collector.read(value, describeCodeRunBy(program, function, value));
        }
        if (entriesThatRunNoCode.terminatorKinds.contains(block.terminator.kind))
            collector.read(block.terminator, descriptionWithoutCode);
        else
            collector.read(block.terminator, describeCodeRunBy(program, function, block.terminator));
    }
    return collector.finish(flow, id);
}

void BodyReferences::prepareToTake(const Program& program, const ParallelLoops& loops, std::span<const uint64_t> stampsOfRecords)
{
    size_t numberOfKeptBodies = m_bodies.size();
    size_t numberOfBodies = stampsOfRecords.size();
    RELEASE_ASSERT(numberOfKeptBodies <= numberOfBodies && numberOfBodies == program.functions.size() && m_stampsOfRecordsForCallers.size() == numberOfKeptBodies);
    growInParallel(loops, m_bodies, numberOfBodies);
    reserveCapacityInParallel(loops, m_isChanged, numberOfBodies);
    m_isChanged.insertFill(numberOfKeptBodies, 1, numberOfBodies - numberOfKeptBodies);
    reserveCapacityInParallel(loops, m_stampsOfRecords, numberOfBodies);
    m_stampsOfRecords.append(stampsOfRecords.subspan(numberOfKeptBodies));
    // The record of a function lies in lines of its own, which nobody has read of late.
    growInParallel(loops, m_stampsOfRecordsForCallers, numberOfBodies);
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfBodies - numberOfKeptBodies), [&](unsigned added) {
        size_t index = numberOfKeptBodies + added;
        m_stampsOfRecordsForCallers[index] = stampOfRecordForCallers(program.functions[index]);
    });
}

void BodyReferences::take(const Program& program, FunctionID id, ReferencesOfBody&& references, uint64_t stampOfRecord)
{
    m_bodies[id.index()] = WTF::move(references);
    m_isChanged[id.index()] = 0;
    if (m_stampsOfRecords[id.index()] == stampOfRecord)
        return;
    m_stampsOfRecords[id.index()] = stampOfRecord;
    m_stampsOfRecordsForCallers[id.index()] = stampOfRecordForCallers(program.at(id));
}

void BodyReferences::didChange(std::span<const FunctionID> functions)
{
    for (FunctionID id : functions) {
        if (id.index() < m_isChanged.size())
            m_isChanged[id.index()] = 1;
    }
}

static CollectedReferences addUpReferences(const Program& program, const Vector<ReferencesOfBody>& bodies, const ParallelLoops& loops)
{
    unsigned count = program.functions.size();
    CollectedReferences references(count, program.types.numberOfCalleeSets());
    Vector<uint8_t> hasBodyByFunction(count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        hasBodyByFunction[index] = Typed::hasBody(program, FunctionID(index));
    });
    auto hasBody = [&](FunctionID id) {
        return !!hasBodyByFunction[id.index()];
    };
    // The lists of a body lie anywhere in memory. Those of a body that the loop comes to soon are asked for ahead of time.
    constexpr unsigned prefetchDistance = 4;
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (index + prefetchDistance < count) {
            const ReferencesOfBody& next = bodies[index + prefetchDistance];
            __builtin_prefetch(next.callees.span().data());
            __builtin_prefetch(next.directCallees.span().data());
            __builtin_prefetch(next.otherReferences.span().data());
            __builtin_prefetch(next.calleeSets.span().data());
        }
        const ReferencesOfBody& body = bodies[index];
        references.calleeOffsets[index + 1] = static_cast<uint32_t>(std::ranges::count_if(body.callees, hasBody));
        references.callsUnknownCode[index] = body.callsUnknownCode;
        for (FunctionID target : body.directCallees)
            WTF::atomicExchangeAdd(&references.directCallSites[target.index()], 1u, std::memory_order_relaxed);
        for (FunctionID target : body.otherReferences) {
            if (!WTF::atomicLoad(&references.hasOtherReference[target.index()], std::memory_order_relaxed))
                WTF::atomicStore(&references.hasOtherReference[target.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        }
        for (CalleeSetID set : body.calleeSets)
            WTF::atomicExchangeAdd(&references.calleeSetCallSites[set.index()], 1u, std::memory_order_relaxed);
    });
    std::partial_sum(references.calleeOffsets.begin(), references.calleeOffsets.end(), references.calleeOffsets.begin());
    references.callees.grow(references.calleeOffsets[count]);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (index + prefetchDistance < count)
            __builtin_prefetch(bodies[index + prefetchDistance].callees.span().data());
        uint32_t next = references.calleeOffsets[index];
        for (FunctionID callee : bodies[index].callees) {
            if (hasBody(callee))
                references.callees[next++] = callee;
        }
    });
    forEachIndexInParallel(loops, references.calleeSetCallSites.size(), [&](unsigned index) {
        uint32_t numberOfSites = references.calleeSetCallSites[index];
        if (!numberOfSites)
            return;
        for (FunctionID callee : program.at(CalleeSetID(index)).functions)
            WTF::atomicExchangeAdd(&references.linkedCallSites[callee.index()], numberOfSites, std::memory_order_relaxed);
    });
    return references;
}

static BitVector findFunctionsExpectedByCode(const Program& program, const Vector<ReferencesOfBody>& bodies)
{
    BitVector isListed(program.types.size());
    for (const ReferencesOfBody& body : bodies) {
        for (TypeID type : body.expectedFunctionTypes)
            isListed.set(type.index());
    }
    BitVector functions(program.functions.size());
    for (size_t type : isListed) {
        for (FunctionID function : program.at(TypeID(static_cast<uint32_t>(type))).functions)
            functions.set(function.index());
    }
    return functions;
}

static void markRecordReferences(const Program& program, Vector<uint8_t>& hasOtherReference, const ParallelLoops& loops)
{
    auto mark = [&](FunctionID function) {
        std::atomic_ref(hasOtherReference[function.index()]).store(1, std::memory_order_relaxed);
    };
    auto markReference = [&](FunctionID function, FunctionReferenceKind kind) {
        switch (kind) {
        case FunctionReferenceKind::Closure:
        case FunctionReferenceKind::Constructor:
            mark(function);
            return;
        case FunctionReferenceKind::Call:
        case FunctionReferenceKind::Dispatch:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        forEachFunctionReference(layout, markReference);
        for (const Vector<ClassMember>* members : { &layout.classFlow.members, &layout.classFlow.namedMembers }) {
            for (const ClassMember& member : *members) {
                if (member.function && member.closureUse != ClosureUse::CalledOnly)
                    mark(member.function);
            }
        }
    });
    // Nobody takes the closure out of a variable that is never read: a function declaration that is only called has its direct callers and no other.
    forEachIndexInParallel(loops, program.variables.size(), [&](unsigned index) {
        const Variable& variable = program.variables[index];
        if (variable.flow.reads != ReadState::NeverRead)
            forEachFunctionReference(variable, markReference);
    });
    forEachIndexInParallel(loops, program.modules.size(), [&](unsigned index) {
        forEachFunctionReference(program.modules[index], markReference);
    });
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        if (recordFactsOf(program, FunctionID(index)).creatorKind == CreatorRecord::Kind::None)
            return;
        const CreatorRecord& creator = program.functions[index].creator;
        forEachFunctionReference(creator, markReference);
        if (creator.is<CreatorRecord::Entrypoint>())
            mark(FunctionID(index));
    });
}

static BitVector findRoots(const Program& program, const FlowResults& flow, const BitVector& isEnteredFromOutside)
{
    BitVector isRoot = isEnteredFromOutside;
    for (const Variable& variable : program.variables) {
        if (variable.function)
            isRoot.set(variable.function.index());
    }
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        if (call.kind == FlowIndirectCallKind::Host)
            isRoot.set(call.callee.index());
    }
    return isRoot;
}

static BitVector createBitVector(const Vector<uint8_t>& flags)
{
    BitVector bits(flags.size());
    for (size_t index = 0; index < flags.size(); ++index) {
        if (flags[index])
            bits.quickSet(index);
    }
    return bits;
}

// Kahn-style peeling from the leaves, one parallel round per rank. Each round scans everything that remains, so it stops once a round peels too little to pay for the next scan.
static Vector<uint32_t> peelAcyclicFunctions(const CallGraph& graph, const ParallelLoops& loops, Vector<uint8_t>& isRanked, FunctionRanks& result)
{
    constexpr size_t maxRemainingPerPeeledFunction = 16;

    Vector<uint32_t> remaining(graph.numberOfFunctions(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    for (;;) {
        uint32_t rank = result.numberOfRanks++;
        Vector<uint32_t> peeled = collectInParallel<uint32_t>(loops, remaining.size(), [&](unsigned position, Vector<uint32_t>& chunk) {
            uint32_t index = remaining[position];
            FunctionID function(index);
            uint8_t reachesUnknownCode = graph.calleeSet(function) != CalleeSetState::Complete;
            for (FunctionID callee : graph.calleesOf(function)) {
                if (!isRanked[callee.index()])
                    return;
                reachesUnknownCode |= result.reachesUnknownCode[callee.index()];
            }
            result.ranks[index] = rank;
            result.reachesUnknownCode[index] = reachesUnknownCode;
            chunk.append(index);
        });
        forEachIndexInParallel(loops, peeled.size(), [&](unsigned position) {
            isRanked[peeled[position]] = 1;
        });
        remaining = collectInParallel<uint32_t>(loops, remaining.size(), [&](unsigned position, Vector<uint32_t>& chunk) {
            if (!isRanked[remaining[position]])
                chunk.append(remaining[position]);
        });
        if (remaining.isEmpty() || peeled.size() * maxRemainingPerPeeledFunction < remaining.size())
            return remaining;
    }
}

static void rankRemainingFunctions(const CallGraph& graph, const ParallelLoops& loops, std::span<const uint32_t> remaining, const Vector<uint8_t>& isRanked, FunctionRanks& result)
{
    unsigned count = static_cast<unsigned>(remaining.size());
    if (!count)
        return;

    Vector<uint32_t> nodeIndex(graph.numberOfFunctions());
    Vector<uint32_t> successorOffsets(count + 1);
    forEachIndexInParallel(loops, count, [&](unsigned node) {
        uint32_t index = remaining[node];
        FunctionID function(index);
        nodeIndex[index] = node;
        uint8_t reachesUnknownCode = graph.calleeSet(function) != CalleeSetState::Complete;
        uint32_t numberOfSuccessors = 0;
        for (FunctionID callee : graph.calleesOf(function)) {
            if (isRanked[callee.index()])
                reachesUnknownCode |= result.reachesUnknownCode[callee.index()];
            else
                ++numberOfSuccessors;
        }
        result.reachesUnknownCode[index] = reachesUnknownCode;
        successorOffsets[node] = numberOfSuccessors;
    });
    uint32_t numberOfEdges = 0;
    for (unsigned node = 0; node < count; ++node)
        numberOfEdges += std::exchange(successorOffsets[node], numberOfEdges);
    successorOffsets[count] = numberOfEdges;
    Vector<uint32_t> successors(numberOfEdges);
    forEachIndexInParallel(loops, count, [&](unsigned node) {
        uint32_t next = successorOffsets[node];
        for (FunctionID callee : graph.calleesOf(FunctionID(remaining[node]))) {
            if (!isRanked[callee.index()])
                successors[next++] = nodeIndex[callee.index()];
        }
    });
    auto successorsOf = [&](unsigned node) {
        return successors.subspan(successorOffsets[node], successorOffsets[node + 1] - successorOffsets[node]);
    };

    // Components come out in reverse topological order: callees first.
    StronglyConnectedComponents components = computeStronglyConnectedComponents(count, successorsOf);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        std::span<const uint32_t> members = components.membersOf(component);
        uint8_t reachesUnknownCode = 0;
        for (uint32_t member : members) {
            reachesUnknownCode |= result.reachesUnknownCode[remaining[member]];
            for (uint32_t successor : successorsOf(member))
                reachesUnknownCode |= result.reachesUnknownCode[remaining[successor]];
        }
        uint8_t isInCycle = components.isInCycle.quickGet(members[0]);
        for (uint32_t member : members) {
            uint32_t index = remaining[member];
            result.ranks[index] = result.numberOfRanks + component;
            result.reachesUnknownCode[index] = reachesUnknownCode;
            result.isInCycle[index] = isInCycle;
        }
    }
    result.numberOfRanks += components.numberOfComponents;
}

static FunctionRanks computeRanksIgnoringUnknownCode(const CallGraph& graph, const ParallelLoops& loops)
{
    unsigned count = graph.numberOfFunctions();
    FunctionRanks result;
    result.ranks.fill(0u, count);
    result.isInCycle.fill(0, count);
    result.reachesUnknownCode.fill(0, count);
    Vector<uint8_t> isRanked(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint32_t> remaining = peelAcyclicFunctions(graph, loops, isRanked, result);
    rankRemainingFunctions(graph, loops, remaining.span(), isRanked, result);
    return result;
}

// Parallel BFS. The order within a frontier depends on thread timing; the set of reachable functions does not.
static Vector<uint8_t> findFunctionsReachableFromUnknownCode(const CallGraph& graph, const ParallelLoops& loops)
{
    Vector<uint8_t> isReachable(FillWith { }, graph.numberOfFunctions(), static_cast<uint8_t>(0));
    Vector<FunctionID> frontier(graph.functionsCalledByUnknownCode());
    for (FunctionID function : frontier)
        isReachable[function.index()] = 1;
    while (!frontier.isEmpty()) {
        frontier = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& chunk) {
            for (FunctionID callee : graph.calleesOf(frontier[position])) {
                std::atomic_ref isCalleeReachable(isReachable[callee.index()]);
                if (!isCalleeReachable.load(std::memory_order_relaxed) && !isCalleeReachable.exchange(1, std::memory_order_relaxed))
                    chunk.append(callee);
            }
        });
    }
    return isReachable;
}

// With unknown code as a node, almost every function is in one SCC, and Tarjan's algorithm is serial.
static void addUnknownCodeComponent(const Vector<uint8_t>& isReachableFromUnknownCode, const ParallelLoops& loops, FunctionRanks& result)
{
    uint32_t unknownCodeRank = result.numberOfRanks;
    forEachIndexInParallel(loops, result.ranks.size(), [&](unsigned index) {
        if (!result.reachesUnknownCode[index])
            return;
        if (isReachableFromUnknownCode[index]) {
            result.ranks[index] = unknownCodeRank;
            result.isInCycle[index] = 1;
        } else
            result.ranks[index] += unknownCodeRank + 1;
    });
}

void CallGraph::computeRanks(BuildContext& build) const
{
    if (m_hasRanks)
        return;
    const ParallelLoops& loops = build.parallelLoops();
    PhaseTimes times(build, PassID::BuildCallGraph);
    FunctionRanks ranks;
    Vector<uint8_t> isReachableFromUnknownCode;
    runTasksInParallel(loops, {
        [&] {
            ranks = computeRanksIgnoringUnknownCode(*this, loops);
        },
        [&] {
            isReachableFromUnknownCode = findFunctionsReachableFromUnknownCode(*this, loops);
        },
    });
    addUnknownCodeComponent(isReachableFromUnknownCode, loops, ranks);
    m_isInCycle = createBitVector(ranks.isInCycle);
    m_bottomUpRanks = WTF::move(ranks.ranks);
    m_hasRanks = true;
    times.didFinish("computeRanks"_s);
}

static void verifyCallers(const CallGraph& graph)
{
    size_t numberOfCallees = 0;
    size_t numberOfCallers = 0;
    for (unsigned index = 0; index < graph.numberOfFunctions(); ++index) {
        FunctionID function(index);
        std::span<const FunctionID> callers = graph.callersOf(function);
        numberOfCallees += graph.calleesOf(function).size();
        numberOfCallers += callers.size();
        RELEASE_ASSERT(std::ranges::adjacent_find(callers, std::ranges::greater_equal { }) == callers.end());
        for (FunctionID caller : callers)
            RELEASE_ASSERT(std::ranges::binary_search(graph.calleesOf(caller), function));
    }
    RELEASE_ASSERT(numberOfCallees == numberOfCallers);
}

static void verifyRanks(const CallGraph& graph)
{
    unsigned unknownCodeNode = graph.numberOfFunctions();
    StronglyConnectedComponents components = computeStronglyConnectedComponents(unknownCodeNode + 1, [&](unsigned node) -> SuccessorList {
        if (node == unknownCodeNode)
            return { graph.functionsCalledByUnknownCode(), std::nullopt };
        FunctionID function(node);
        if (graph.calleeSet(function) == CalleeSetState::Complete)
            return { graph.calleesOf(function), std::nullopt };
        return { graph.calleesOf(function), unknownCodeNode };
    });
    for (unsigned index = 0; index < unknownCodeNode; ++index) {
        FunctionID function(index);
        RELEASE_ASSERT((graph.recursion(function) == Recursion::Unknown) == components.isInCycle.get(index));
        for (FunctionID callee : graph.calleesOf(function)) {
            if (components.componentOf[callee.index()] == components.componentOf[index])
                RELEASE_ASSERT(graph.bottomUpRank(callee) == graph.bottomUpRank(function));
            else
                RELEASE_ASSERT(graph.bottomUpRank(callee) < graph.bottomUpRank(function));
        }
    }
}

// Two-pass parallel counting sort of the edges by target: first into buckets of consecutive targets, then within each bucket.
CallGraph::Adjacency CallGraph::Adjacency::inverse(const ParallelLoops& loops) const
{
    constexpr unsigned maxBuckets = 256;
    constexpr size_t maxChunks = 128;
    constexpr size_t edgesPerChunk = 1 << 15;

    Adjacency result;
    result.offsets.fill(0, offsets.size());
    if (targets.isEmpty())
        return result;

    unsigned count = offsets.size() - 1;
    size_t numberOfEdges = targets.size();
    unsigned bucketShift = 0;
    while ((count >> bucketShift) >= maxBuckets)
        ++bucketShift;
    unsigned numberOfBuckets = (count >> bucketShift) + 1;
    unsigned numberOfChunks = static_cast<unsigned>(std::min(maxChunks, (numberOfEdges + edgesPerChunk - 1) / edgesPerChunk));
    Vector<uint32_t> chunkStarts(numberOfChunks + 1, [&](size_t chunk) {
        uint32_t firstEdge = static_cast<uint32_t>(numberOfEdges * chunk / numberOfChunks);
        return static_cast<uint32_t>(std::ranges::lower_bound(offsets, firstEdge) - offsets.begin());
    });

    Vector<uint32_t> bucketCursors(FillWith { }, static_cast<size_t>(numberOfChunks) * numberOfBuckets, 0u);
    auto cursorsForChunk = [&](unsigned chunk) {
        return bucketCursors.mutableSubspan(static_cast<size_t>(chunk) * numberOfBuckets, numberOfBuckets);
    };
    forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
        std::span<uint32_t> counts = cursorsForChunk(chunk);
        uint32_t firstEdge = offsets[chunkStarts[chunk]];
        for (FunctionID target : targets.subspan(firstEdge, offsets[chunkStarts[chunk + 1]] - firstEdge))
            ++counts[target.index() >> bucketShift];
    });
    Vector<uint32_t> bucketStarts(numberOfBuckets + 1);
    uint32_t position = 0;
    for (unsigned bucket = 0; bucket < numberOfBuckets; ++bucket) {
        bucketStarts[bucket] = position;
        for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk)
            position += std::exchange(cursorsForChunk(chunk)[bucket], position);
    }
    bucketStarts[numberOfBuckets] = position;

    Vector<CallEdge> edges(numberOfEdges);
    forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
        std::span<uint32_t> cursors = cursorsForChunk(chunk);
        for (uint32_t source = chunkStarts[chunk]; source < chunkStarts[chunk + 1]; ++source) {
            for (FunctionID target : of(FunctionID(source)))
                edges[cursors[target.index() >> bucketShift]++] = CallEdge { target.index(), source };
        }
    });

    result.targets.grow(numberOfEdges);
    forEachIndexInParallel(loops, numberOfBuckets, [&](unsigned bucket) {
        std::span<const CallEdge> bucketEdges = edges.subspan(bucketStarts[bucket], bucketStarts[bucket + 1] - bucketStarts[bucket]);
        uint32_t firstTarget = bucket << bucketShift;
        uint32_t endTarget = std::min(count, (bucket + 1) << bucketShift);
        for (const CallEdge& edge : bucketEdges)
            ++result.offsets[edge.target];
        uint32_t offset = bucketStarts[bucket];
        for (uint32_t target = firstTarget; target < endTarget; ++target)
            offset += std::exchange(result.offsets[target], offset);
        Vector<uint32_t> cursors(result.offsets.subspan(firstTarget, endTarget - firstTarget));
        for (const CallEdge& edge : bucketEdges)
            result.targets[cursors[edge.target - firstTarget]++] = FunctionID(edge.source);
    });
    result.offsets[count] = static_cast<uint32_t>(numberOfEdges);
    return result;
}

CallGraph buildCallGraph(const Program& program, const FlowResults& flow, BuildContext& build, BodyReferences* kept)
{
    const ParallelLoops& loops = build.parallelLoops();
    PhaseTimes times(build, PassID::BuildCallGraph);
    unsigned count = program.functions.size();

    BodyReferences referencesOfThisGraph;
    BodyReferences& bodies = kept ? *kept : referencesOfThisGraph;
    unsigned numberOfKeptBodies = bodies.m_bodies.size();
    RELEASE_ASSERT(numberOfKeptBodies <= count && bodies.m_isChanged.size() == numberOfKeptBodies && bodies.m_stampsOfRecords.size() == numberOfKeptBodies && bodies.m_stampsOfRecordsForCallers.size() == numberOfKeptBodies);
    Vector<uint8_t> recordLooksDifferent(numberOfKeptBodies);
    Vector<uint8_t> recordLooksDifferentToCallers(numberOfKeptBodies);
    growInParallel(loops, bodies.m_stampsOfRecords, count);
    growInParallel(loops, bodies.m_stampsOfRecordsForCallers, count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        const Function& function = program.functions[index];
        uint64_t stamp = stampOfRecord(program, FunctionID(index));
        bool isKept = index < numberOfKeptBodies;
        bool looksDifferent = isKept && stamp != bodies.m_stampsOfRecords[index];
        bodies.m_stampsOfRecords[index] = stamp;
        // A record with the stamp that it had looks to callers as it did.
        bool looksDifferentToCallers = false;
        if (!isKept || looksDifferent) {
            uint64_t stampForCallers = stampOfRecordForCallers(function);
            looksDifferentToCallers = isKept && stampForCallers != bodies.m_stampsOfRecordsForCallers[index];
            bodies.m_stampsOfRecordsForCallers[index] = stampForCallers;
        }
        if (isKept) {
            recordLooksDifferent[index] = looksDifferent;
            recordLooksDifferentToCallers[index] = looksDifferentToCallers;
        }
    });
    const bool someRecordLooksDifferentToCallers = recordLooksDifferentToCallers.contains(1);
    const bool checksKeptBodies = build.options().verifiesAfterEveryPass;
    growInParallel(loops, bodies.m_bodies, count);
    std::atomic<uint64_t> numberOfBodiesCollected { 0 };
    std::atomic<uint64_t> numberOfBodiesCollectedAsBefore { 0 };
    const bool logsCounts = build.options().logging.logsPhaseTimes;
    const EntriesThatRunNoCode entriesThatRunNoCode = findEntriesThatRunNoCode();
    forEachIndexInParallel(loops, count, largeFunctions(program, loops).span(), [&](unsigned index) {
        ReferencesOfBody& body = bodies.m_bodies[index];
        bool stillHolds = index < numberOfKeptBodies && !bodies.m_isChanged[index] && !recordLooksDifferent[index];
        if (stillHolds && someRecordLooksDifferentToCallers) {
            stillHolds = std::ranges::none_of(body.callees, [&](FunctionID callee) {
                return recordLooksDifferentToCallers[callee.index()];
            });
        }
        if (stillHolds) {
            RELEASE_ASSERT(!checksKeptBodies || body == collectReferencesOf(program, flow, FunctionID(index), entriesThatRunNoCode));
            return;
        }
        ReferencesOfBody collected = collectReferencesOf(program, flow, FunctionID(index), entriesThatRunNoCode);
        RELEASE_ASSERT(!checksKeptBodies || collected == collectReferencesOf(program, flow, FunctionID(index), EntriesThatRunNoCode { }));
        if (logsCounts && program.functions[index].hasBody()) [[unlikely]] {
            numberOfBodiesCollected.fetch_add(1, std::memory_order_relaxed);
            if (index < numberOfKeptBodies && collected == body)
                numberOfBodiesCollectedAsBefore.fetch_add(1, std::memory_order_relaxed);
        }
        body = WTF::move(collected);
    });
    bodies.m_isChanged.fill(0, count);
    if (logsCounts) [[unlikely]]
        dataLogLn("AOT body references: of ", count, " functions | bodies read ", numberOfBodiesCollected.load(), " | with the same references as before ", numberOfBodiesCollectedAsBefore.load());
    times.didFinish("collectBodyReferences"_s);
    CollectedReferences references = addUpReferences(program, bodies.m_bodies, loops);
    times.didFinish("addUpReferences"_s);
    markRecordReferences(program, references.hasOtherReference, loops);
    times.didFinish("markRecordReferences"_s);
    BitVector isEnteredFromOutside = functionsCalledByHost(program, loops);
    BitVector isRoot = findRoots(program, flow, isEnteredFromOutside);
    times.didFinish("findRoots"_s);

    Vector<uint8_t> hasCompleteCallerSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasCompleteCalleeSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasOnlyDirectCallers(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasCompleteCallSiteSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint32_t> callSitesOfHost(FillWith { }, count, 0u);
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        ++references.linkedCallSites[call.callee.index()];
        if (call.kind == FlowIndirectCallKind::Host)
            ++callSitesOfHost[call.callee.index()];
    }
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        RecordFacts function = recordFactsOf(program, FunctionID(index));
        if (!function.hasBody)
            return;
        hasCompleteCallerSet[index] = !isRoot.get(index)
            && (function.hasOnlyDirectCallers() || callerStateOf(flow, FunctionID(index)) == CallerState::AllKnown);
        hasCompleteCalleeSet[index] = !references.callsUnknownCode[index];
        hasOnlyDirectCallers[index] = !isEnteredFromOutside.get(index)
            && (function.hasOnlyDirectCallers() || !references.hasOtherReference[index]);
        // Flow has followed every closure of the function to its calls, and none is kept as a value: a reference to a closure adds no way in that is not counted.
        const FunctionFlow& functionFlow = flow.function(FunctionID(index));
        hasCompleteCallSiteSet[index] = hasOnlyDirectCallers[index]
            || (!isEnteredFromOutside.get(index) && functionFlow.callers == CallerState::AllKnown && functionFlow.closureUse == ClosureUse::CalledOnly);
    });

    CallGraph graph;
    graph.m_callees.offsets = WTF::move(references.calleeOffsets);
    graph.m_callees.targets = WTF::move(references.callees);
    graph.m_calledByUnknownCode = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& called) {
        if (!hasCompleteCallerSet[index] && hasBody(program, FunctionID(index)))
            called.append(FunctionID(index));
    });
    graph.m_hasCompleteCalleeSet = createBitVector(hasCompleteCalleeSet);
    graph.m_callSites = WTF::move(references.linkedCallSites);
    graph.m_callSitesOfHost = WTF::move(callSitesOfHost);
    for (unsigned index = 0; index < count; ++index)
        graph.m_callSites[index] += references.directCallSites[index];
    graph.m_directCallSites = WTF::move(references.directCallSites);
    times.didFinish("describeFunctions"_s);

    runTasksInParallel(loops, {
        [&] {
            graph.m_callers = graph.m_callees.inverse(loops);
        },
        [&] {
            graph.m_roots.reserveInitialCapacity(isRoot.bitCount());
            for (size_t index : isRoot)
                graph.m_roots.append(FunctionID(index));
            graph.m_hasCompleteCallerSet = createBitVector(hasCompleteCallerSet);
            graph.m_hasOnlyDirectCallers = createBitVector(hasOnlyDirectCallers);
            graph.m_hasCompleteCallSiteSet = createBitVector(hasCompleteCallSiteSet);
        },
        [&] {
            graph.m_isExpectedByCode = findFunctionsExpectedByCode(program, bodies.m_bodies);
        },
    });
    times.didFinish("listCallersAndRoots"_s);
    if (build.options().verifiesAfterEveryPass) {
        graph.computeRanks(build);
        verifyCallers(graph);
        verifyRanks(graph);
    }
    return graph;
}

void CallGraph::dump(PrintStream& out, const Program& program) const
{
    auto printFunctions = [&](std::span<const FunctionID> functions) {
        for (FunctionID function : functions)
            out.print(" f", function.index());
    };
    out.print("called by unknown code");
    printFunctions(functionsCalledByUnknownCode());
    out.println();
    out.print("roots");
    printFunctions(roots());
    out.println();
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;
        FunctionID function(index);
        out.print("f", index, ": callees");
        printFunctions(calleesOf(function));
        out.print(" | callers");
        printFunctions(callersOf(function));
        out.print(" | sites ", numberOfDirectCallSites(function), " | callers ", enumName(callerSet(function)), " | callees ", enumName(calleeSet(function)), " | entries ", enumName(entrySet(function)), " | ", enumName(recursion(function)), " | rank ", bottomUpRank(function), " | call sites ", numberOfCallSites(function), " ", enumName(callSiteSet(function)));
        out.println(isExpectedByCode(function) ? " | expected by code"_s : ""_s);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
