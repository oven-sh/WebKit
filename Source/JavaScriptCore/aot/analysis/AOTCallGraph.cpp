/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallGraph.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTExactBuffer.h"
#include "AOTFlowResults.h"
#include "AOTGrouping.h"
#include "AOTLargeVectors.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <functional>
#include <numeric>
#include <optional>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/Atomics.h>
#include <wtf/EnumTraits.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

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

struct SuccessorList {
    size_t size() const { return functions.size() + (unknownCodeNode ? 1 : 0); }
    unsigned operator[](size_t position) const { return position < functions.size() ? functions[position].index() : *unknownCodeNode; }

    std::span<const FunctionID> functions;
    std::optional<unsigned> unknownCodeNode;
};

// What the members of each long set that have a rank say to the users of the set.
struct RankedMembersOfLongSets {
    explicit RankedMembersOfLongSets(const CallGraph& graph)
        : numberWithoutRank(graph.numberOfLongSets(), [&](size_t set) { return static_cast<uint32_t>(graph.membersOfLongSet(set).size()); })
        , reachUnknownCode(FillWith { }, graph.numberOfLongSets(), static_cast<uint8_t>(0))
    {
    }

    Vector<uint32_t> numberWithoutRank;
    Vector<uint8_t> reachUnknownCode;
};

} // anonymous namespace

void BodyReferenceCollector::add(FunctionID callee)
{
    Vector<FunctionID>& callees = m_references.callees;
    if (!m_seen.isEmpty()) {
        if (markSeen(callee))
            callees.append(callee);
        return;
    }
    callees.append(callee);
    if (callees.size() < deduplicationThreshold)
        return;
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

static EffectDescription describeCodeRunBesideLongCalleeSet(const Program&, const Terminator&, CalleeSetID) { RELEASE_ASSERT_NOT_REACHED(); }

void BodyReferenceCollector::readInstruction(const auto& instruction, const EffectDescription& description)
{
    m_hasReadBody = true;
    forEachFunctionReference(instruction, [&](FunctionID target, FunctionReferenceKind kind) {
        addReference(target, kind);
    });
    CalleeSetID entered;
    if constexpr (std::same_as<std::remove_cvref_t<decltype(instruction)>, Value>) {
        if (instruction.opcode == Opcode::CallTable)
            entered = instruction.entrypoints();
    }
    forEachCalleeSet(instruction, [&](CalleeSetID set) {
        if (set != entered)
            m_references.calleeSets.append(set);
    });
    if (entered && m_program.at(entered).isLong())
        m_references.enteredSets.append(entered);
    else {
        for (FunctionID entrypoint : entrypointsOfTableCall(m_program, instruction))
            addReference(entrypoint, FunctionReferenceKind::Call);
    }
    // The description may list the functions of the set or leave them out.
    bool runsLongSet = !!longCalleeSetRunBy(m_program, instruction);
    if (!runsLongSet) {
        for (FunctionID callee : description.functions)
            add(callee);
    }
    for (FunctionID callee : description.slowPathFunctions)
        add(callee);
    if (description.callsUnknownCode)
        m_references.callsUnknownCode = true;
    if (description.followsFieldsOfOperands)
        m_references.followsFieldsOfOperands = true;
    if (!description.callsUnknownCode && description.functions.isEmpty() && !runsLongSet)
        return;
    RELEASE_ASSERT(m_indexInBlock < 1u << 30);
    bool listsManyFunctions = runsLongSet || description.functions.size() > mostFunctionsKeptOfSite;
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

ReferencesOfBody BodyReferenceCollector::finish(const FlowResults& flow, FunctionID id)
{
    Vector<CalleeSetID>& sets = m_references.calleeSets;
    AOT::sort(sets);
    for (size_t index = 0; index < sets.size(); ++index) {
        if ((index && sets[index] == sets[index - 1]) || m_program.at(sets[index]).isLong())
            continue;
        for (FunctionID callee : m_program.at(sets[index]).followed().value())
            add(callee);
    }
    AOT::sort(m_references.enteredSets);

    Vector<FunctionID, 8> sources;
    auto addSource = [&](FunctionID source) {
        if (FunctionID asRead = findFunctionAsRead(&m_program, source))
            sources.append(asRead);
    };
    addSource(id);
    for (const InlineFrame& frame : m_program.at(id).inlineFrames)
        addSource(frame.callee);
    AOT::sort(sources);
    removeRepeatedElements(sources);
    for (FunctionID source : sources) {
        for (const FlowIndirectCall& call : flow.indirectCallsFrom(source))
            add(call.callee);
    }

    if (mayHoldExpectedTypes(m_program, id)) {
        Vector<TypeID>& types = m_references.expectedFunctionTypes;
        for (const Value& value : m_program.at(id).values) {
            TypeID expected = value.expected().predicted();
            if (expected && (types.isEmpty() || types.last() != expected) && !m_program.at(expected).functions.isEmpty())
                types.append(expected);
        }
        AOT::sort(types);
        removeRepeatedElements(types);
        moveToExactBuffer(types);
    }
    AOT::sort(m_references.callees);
    removeRepeatedElements(m_references.callees);
    AOT::sort(m_references.otherReferences);
    removeRepeatedElements(m_references.otherReferences);
    moveToExactBuffer(m_references.callees);
    moveToExactBuffer(m_references.directCallees);
    moveToExactBuffer(m_references.calleeSets);
    moveToExactBuffer(m_references.enteredSets);
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
    auto read = [&](const auto& entry) {
        if (entriesThatRunNoCode.contains(entry))
            collector.read(entry, descriptionWithoutCode);
        else if (CalleeSetID set = longCalleeSetRunBy(program, entry))
            collector.read(entry, describeCodeRunBesideLongCalleeSet(program, entry, set));
        else
            collector.read(entry, describeCodeRunBy(program, function, entry));
    };
    if (!mayEndBlockWith(program, id, entriesThatRunNoCode.otherTerminatorKinds)) {
        forEachListedValue(program, id, entriesThatRunNoCode.opcodesWithReferences, [&](BlockID block, uint32_t indexInBlock, ValueID, const Value& value) {
            collector.skipTo(block, indexInBlock);
            read(value);
        });
        return collector.finish(flow, id);
    }
    for (const Block& block : function.blocks) {
        for (ValueID listed : block.values)
            read(function.at(listed));
        read(block.terminator);
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
        AtomicRef(hasOtherReference[function.index()]).store(1, std::memory_order_relaxed);
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
    forEachIndexInParallel(loops, program.variables.size(), [&](unsigned index) {
        const Variable& variable = program.variables[index];
        if (variable.flow.reads != ReadState::NeverRead && variable.flow.reads != ReadState::OnlyWhereDefined)
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

static BitVector createBitVector(const Vector<uint8_t>& flags)
{
    BitVector bits(flags.size());
    for (size_t index = 0; index < flags.size(); ++index) {
        if (flags[index])
            bits.quickSet(index);
    }
    return bits;
}

static Vector<uint32_t> peelAcyclicFunctions(const CallGraph& graph, const ParallelLoops& loops, Vector<uint8_t>& isRanked, RankedMembersOfLongSets& longSets, FunctionRanks& result)
{
    AOT_THRESHOLD(size_t, maxRemainingPerPeeledFunction, 16);

    Vector<uint32_t> remaining(graph.numberOfFunctions(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    for (;;) {
        uint32_t rank = result.numberOfRanks++;
        Vector<uint32_t> peeled = collectInParallel<uint32_t>(loops, remaining.size(), [&](unsigned position, Vector<uint32_t>& chunk) {
            uint32_t index = remaining[position];
            FunctionID function(index);
            uint8_t reachesUnknownCode = !graph.hasCompleteCalleeSet(function);
            for (FunctionID callee : graph.ownCalleesOf(function)) {
                if (!isRanked[callee.index()])
                    return;
                reachesUnknownCode |= result.reachesUnknownCode[callee.index()];
            }
            for (uint32_t set : graph.longSetsOf(function)) {
                if (longSets.numberWithoutRank[set])
                    return;
                reachesUnknownCode |= longSets.reachUnknownCode[set];
            }
            result.ranks[index] = rank;
            result.reachesUnknownCode[index] = reachesUnknownCode;
            chunk.append(index);
        });
        forEachIndexInParallel(loops, peeled.size(), [&](unsigned position) {
            isRanked[peeled[position]] = 1;
        });
        if (graph.numberOfLongSets()) {
            for (uint32_t index : peeled) {
                for (uint32_t set : graph.longSetsWith(FunctionID(index))) {
                    --longSets.numberWithoutRank[set];
                    longSets.reachUnknownCode[set] |= result.reachesUnknownCode[index];
                }
            }
        }
        remaining = collectInParallel<uint32_t>(loops, remaining.size(), [&](unsigned position, Vector<uint32_t>& chunk) {
            if (!isRanked[remaining[position]])
                chunk.append(remaining[position]);
        });
        if (remaining.isEmpty() || peeled.size() * maxRemainingPerPeeledFunction < remaining.size())
            return remaining;
    }
}

// computeStronglyConnectedComponents() for nodes that have, beside successors of their own, all the nodes of some lists. It walks as if each node listed all its successors in ascending order,
// so it numbers the components alike, and it reads a list once, however many nodes have it.
static StronglyConnectedComponents computeComponentsWithSharedLists(unsigned numberOfNodes, unsigned numberOfLists, NOESCAPE const auto& ownSuccessorsOf, NOESCAPE const auto& listsOf, NOESCAPE const auto& nodesOf, NOESCAPE const auto& listsWith)
{
    constexpr unsigned none = UINT_MAX;
    StronglyConnectedComponents result;
    result.componentOf.fill(0, numberOfNodes);
    result.isInCycle.ensureSize(numberOfNodes);
    result.members.reserveInitialCapacity(numberOfNodes);
    BitVector hasSelfEdge(numberOfNodes);
    BitVector isInComponentStack(numberOfNodes);
    BitVector isVisited(numberOfNodes);
    Vector<unsigned> order(FillWith { }, numberOfNodes, 0u);
    Vector<unsigned> lowest = order;
    Vector<unsigned> componentStack;
    Vector<std::pair<unsigned, unsigned>> walk;
    // Nodes are visited once, so the nodes of a list before this one stay visited.
    Vector<unsigned> firstNotVisited(FillWith { }, numberOfLists, 0u);
    // Components leave the stack from its top, so of the nodes of a list the one that came first leaves last.
    Vector<unsigned> lowestInStack(FillWith { }, numberOfLists, none);
    unsigned numberVisited = 0;
    auto enter = [&](unsigned node) {
        order[node] = lowest[node] = numberVisited++;
        isVisited.set(node);
        componentStack.append(node);
        isInComponentStack.set(node);
        for (uint32_t list : listsWith(node)) {
            if (lowestInStack[list] == none)
                lowestInStack[list] = order[node];
        }
        walk.append({ node, 0 });
    };
    for (unsigned root = 0; root < numberOfNodes; ++root) {
        if (isVisited.get(root))
            continue;
        enter(root);
        while (!walk.isEmpty()) {
            unsigned node = walk.last().first;
            unsigned& nextOwn = walk.last().second;
            std::span<const uint32_t> own = ownSuccessorsOf(node);
            for (; nextOwn < own.size() && isVisited.get(own[nextOwn]); ++nextOwn) {
                if (own[nextOwn] == node)
                    hasSelfEdge.set(node);
                if (isInComponentStack.get(own[nextOwn]))
                    lowest[node] = std::min(lowest[node], order[own[nextOwn]]);
            }
            unsigned next = nextOwn < own.size() ? own[nextOwn] : none;
            for (uint32_t list : listsOf(node)) {
                std::span<const uint32_t> nodes = nodesOf(list);
                unsigned& first = firstNotVisited[list];
                while (first < nodes.size() && isVisited.get(nodes[first]))
                    ++first;
                if (first < nodes.size())
                    next = std::min(next, nodes[first]);
            }
            if (next != none) {
                enter(next);
                continue;
            }
            for (uint32_t list : listsOf(node)) {
                lowest[node] = std::min(lowest[node], lowestInStack[list]);
                if (std::ranges::binary_search(nodesOf(list), node))
                    hasSelfEdge.set(node);
            }
            walk.removeLast();
            if (!walk.isEmpty())
                lowest[walk.last().first] = std::min(lowest[walk.last().first], lowest[node]);
            if (lowest[node] != order[node])
                continue;
            bool isCycle = componentStack.last() != node || hasSelfEdge.get(node);
            result.firstMemberOf.append(result.members.size());
            for (;;) {
                unsigned member = componentStack.takeLast();
                isInComponentStack.clear(member);
                for (uint32_t list : listsWith(member)) {
                    if (lowestInStack[list] == order[member])
                        lowestInStack[list] = none;
                }
                if (isCycle)
                    result.isInCycle.quickSet(member);
                result.componentOf[member] = result.numberOfComponents;
                result.members.append(member);
                if (member == node)
                    break;
            }
            ++result.numberOfComponents;
        }
    }
    result.firstMemberOf.append(result.members.size());
    return result;
}

static void rankRemainingFunctions(const CallGraph& graph, const ParallelLoops& loops, std::span<const uint32_t> remaining, const Vector<uint8_t>& isRanked, const RankedMembersOfLongSets& longSets, FunctionRanks& result)
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
        uint8_t reachesUnknownCode = !graph.hasCompleteCalleeSet(function);
        uint32_t numberOfSuccessors = 0;
        for (FunctionID callee : graph.ownCalleesOf(function)) {
            if (isRanked[callee.index()])
                reachesUnknownCode |= result.reachesUnknownCode[callee.index()];
            else
                ++numberOfSuccessors;
        }
        for (uint32_t set : graph.longSetsOf(function))
            reachesUnknownCode |= longSets.reachUnknownCode[set];
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
        for (FunctionID callee : graph.ownCalleesOf(FunctionID(remaining[node]))) {
            if (!isRanked[callee.index()])
                successors[next++] = nodeIndex[callee.index()];
        }
    });
    auto successorsOf = [&](unsigned node) {
        return successors.subspan(successorOffsets[node], successorOffsets[node + 1] - successorOffsets[node]);
    };
    auto longSetsOf = [&](unsigned node) {
        return graph.longSetsOf(FunctionID(remaining[node]));
    };

    unsigned numberOfLongSets = graph.numberOfLongSets();
    Vector<uint32_t> firstNodeOfLongSet;
    Vector<uint32_t> nodesOfLongSets;
    StronglyConnectedComponents components;
    if (!numberOfLongSets)
        components = computeStronglyConnectedComponents(count, successorsOf);
    else {
        groupByIndex(numberOfLongSets, firstNodeOfLongSet, nodesOfLongSets, [&](const auto& add) {
            for (uint32_t set = 0; set < numberOfLongSets; ++set) {
                if (!longSets.numberWithoutRank[set])
                    continue;
                for (FunctionID member : graph.membersOfLongSet(set)) {
                    if (!isRanked[member.index()])
                        add(set, nodeIndex[member.index()]);
                }
            }
        });
        components = computeComponentsWithSharedLists(count, numberOfLongSets, successorsOf, longSetsOf, [&](uint32_t set) {
            return entriesOfGroup(firstNodeOfLongSet, nodesOfLongSets, set);
        }, [&](unsigned node) {
            return graph.longSetsWith(FunctionID(remaining[node]));
        });
    }
    // The members of a set are in the component of its first user or in earlier ones. What they say is final behind that component.
    enum class AnswerOfLongSet : uint8_t { NotAsked, HoldsInComponent, IsFinal };
    Vector<AnswerOfLongSet> answerOfLongSet(FillWith { }, numberOfLongSets, AnswerOfLongSet::NotAsked);
    Vector<uint32_t> componentOfAnswer(FillWith { }, numberOfLongSets, 0u);
    Vector<uint8_t> longSetReachesUnknownCode(FillWith { }, numberOfLongSets, static_cast<uint8_t>(0));
    auto reachesUnknownCodeThrough = [&](uint32_t set, uint32_t component) {
        bool holds = answerOfLongSet[set] == AnswerOfLongSet::IsFinal || (answerOfLongSet[set] == AnswerOfLongSet::HoldsInComponent && componentOfAnswer[set] == component);
        if (!holds) {
            answerOfLongSet[set] = answerOfLongSet[set] == AnswerOfLongSet::NotAsked ? AnswerOfLongSet::HoldsInComponent : AnswerOfLongSet::IsFinal;
            componentOfAnswer[set] = component;
            longSetReachesUnknownCode[set] = std::ranges::any_of(entriesOfGroup(firstNodeOfLongSet, nodesOfLongSets, set), [&](uint32_t node) {
                return !!result.reachesUnknownCode[remaining[node]];
            });
        }
        return longSetReachesUnknownCode[set];
    };
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        std::span<const uint32_t> members = components.membersOf(component);
        uint8_t reachesUnknownCode = 0;
        for (uint32_t member : members) {
            reachesUnknownCode |= result.reachesUnknownCode[remaining[member]];
            for (uint32_t successor : successorsOf(member))
                reachesUnknownCode |= result.reachesUnknownCode[remaining[successor]];
            for (uint32_t set : longSetsOf(member))
                reachesUnknownCode |= reachesUnknownCodeThrough(set, component);
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

static Vector<uint8_t> findFunctionsReachableFromUnknownCode(const CallGraph& graph, const ParallelLoops& loops)
{
    Vector<uint8_t> isReachable(FillWith { }, graph.numberOfFunctions(), static_cast<uint8_t>(0));
    Vector<uint8_t> isLongSetReachable(FillWith { }, graph.numberOfLongSets(), static_cast<uint8_t>(0));
    Vector<FunctionID> frontier(graph.functionsCalledByUnknownCode());
    for (FunctionID function : frontier)
        isReachable[function.index()] = 1;
    auto reach = [](uint8_t& flag) {
        AtomicRef isReached(flag);
        return !isReached.load(std::memory_order_relaxed) && !isReached.exchange(1, std::memory_order_relaxed);
    };
    while (!frontier.isEmpty()) {
        frontier = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& chunk) {
            auto reachAll = [&](std::span<const FunctionID> callees) {
                for (FunctionID callee : callees) {
                    if (reach(isReachable[callee.index()]))
                        chunk.append(callee);
                }
            };
            reachAll(graph.ownCalleesOf(frontier[position]));
            for (uint32_t set : graph.longSetsOf(frontier[position])) {
                if (reach(isLongSetReachable[set]))
                    reachAll(graph.membersOfLongSet(set));
            }
        });
    }
    return isReachable;
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
            unsigned count = numberOfFunctions();
            ranks.ranks.fill(0u, count);
            ranks.isInCycle.fill(0, count);
            ranks.reachesUnknownCode.fill(0, count);
            Vector<uint8_t> isRanked(FillWith { }, count, static_cast<uint8_t>(0));
            RankedMembersOfLongSets longSets(*this);
            Vector<uint32_t> remaining = peelAcyclicFunctions(*this, loops, isRanked, longSets, ranks);
            rankRemainingFunctions(*this, loops, remaining.span(), isRanked, longSets, ranks);
        },
        [&] {
            isReachableFromUnknownCode = findFunctionsReachableFromUnknownCode(*this, loops);
        },
    });
    uint32_t unknownCodeRank = ranks.numberOfRanks;
    forEachIndexInParallel(loops, ranks.ranks.size(), [&](unsigned index) {
        if (!ranks.reachesUnknownCode[index])
            return;
        if (isReachableFromUnknownCode[index]) {
            ranks.ranks[index] = unknownCodeRank;
            ranks.isInCycle[index] = 1;
        } else
            ranks.ranks[index] += unknownCodeRank + 1;
    });
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
        numberOfCallees += std::ranges::distance(graph.calleesOf(function));
        FunctionID previous;
        for (FunctionID caller : graph.callersOf(function)) {
            ++numberOfCallers;
            RELEASE_ASSERT(!previous || previous < caller);
            previous = caller;
            RELEASE_ASSERT(graph.calleesOf(caller).contains(function));
        }
    }
    RELEASE_ASSERT(numberOfCallees == numberOfCallers);
}

static void verifyRanks(const CallGraph& graph)
{
    unsigned unknownCodeNode = graph.numberOfFunctions();
    Vector<Vector<FunctionID>> callees(unknownCodeNode, [&](size_t index) {
        Vector<FunctionID> list;
        for (FunctionID callee : graph.calleesOf(FunctionID(index)))
            list.append(callee);
        return list;
    });
    StronglyConnectedComponents components = computeStronglyConnectedComponents(unknownCodeNode + 1, [&](unsigned node) -> SuccessorList {
        if (node == unknownCodeNode)
            return { graph.functionsCalledByUnknownCode(), std::nullopt };
        if (graph.hasCompleteCalleeSet(FunctionID(node)))
            return { callees[node].span(), std::nullopt };
        return { callees[node].span(), unknownCodeNode };
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

CallGraph::Adjacency CallGraph::Adjacency::inverse(const ParallelLoops& loops) const
{
    AOT_THRESHOLD(unsigned, maxBuckets, 256);
    AOT_THRESHOLD(size_t, maxChunks, 128);
    constexpr size_t edgesPerChunk = 1 << 15;

    Adjacency result;
    result.offsets.fill(0, offsets.size());
    if (targets.isEmpty()) {
        addInverseOfLongLists(result);
        return result;
    }

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
            for (FunctionID target : targets.subspan(offsets[source], offsets[source + 1] - offsets[source]))
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
    addInverseOfLongLists(result);
    return result;
}

uint32_t CallGraph::Adjacency::addLongList(std::span<const FunctionID> list)
{
    if (longListOffsets.isEmpty())
        longListOffsets.append(0);
    longLists.append(list);
    longListOffsets.append(longLists.size());
    return longListOffsets.size() - 1;
}

// The sources of a target are those that list it themselves and the users of each long list that holds it. Targets held by the same long lists, and listed by nobody else, share one list of sources.
void CallGraph::Adjacency::addInverseOfLongLists(Adjacency& result) const
{
    if (longListOf.isEmpty())
        return;
    unsigned count = offsets.size() - 1;
    unsigned numberOfLists = longListOffsets.size() - 1;
    Vector<uint32_t> firstUser;
    Vector<FunctionID> users;
    groupByIndex(numberOfLists, firstUser, users, [&](const auto& add) {
        for (unsigned source = 0; source < longListOf.size(); ++source) {
            if (longListOf[source])
                add(longListOf[source] - 1, FunctionID(source));
        }
    });
    Vector<uint32_t> firstListOfTarget;
    Vector<uint32_t> listsOfTargets;
    groupByIndex(count, firstListOfTarget, listsOfTargets, [&](const auto& add) {
        for (uint32_t list = 0; list < numberOfLists; ++list) {
            for (FunctionID target : longLists.subspan(longListOffsets[list], longListOffsets[list + 1] - longListOffsets[list]))
                add(target.index(), list);
        }
    });

    result.longListOf.fill(0, count);
    UncheckedKeyHashMap<unsigned, Vector<std::pair<uint32_t, uint32_t>, 1>, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> sourcesOfSameLists;
    Vector<FunctionID> sources;
    for (unsigned target = 0; target < count; ++target) {
        std::span<const uint32_t> lists = entriesOfGroup(firstListOfTarget, listsOfTargets, target);
        if (lists.empty())
            continue;
        std::span<const FunctionID> ownSources = result.targets.subspan(result.offsets[target], result.offsets[target + 1] - result.offsets[target]);
        auto collectSources = [&] {
            sources.shrink(0);
            sources.append(ownSources);
            for (uint32_t list : lists)
                sources.append(entriesOfGroup(firstUser, users, list));
            AOT::sort(sources);
            return result.addLongList(sources.span());
        };
        if (!ownSources.empty()) {
            result.longListOf[target] = collectSources();
            continue;
        }
        Hasher hasher;
        for (uint32_t list : lists)
            add(hasher, list);
        auto& known = sourcesOfSameLists.add(hasher.hash(), Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
        for (auto [earlierTarget, longList] : known) {
            if (std::ranges::equal(entriesOfGroup(firstListOfTarget, listsOfTargets, earlierTarget), lists))
                result.longListOf[target] = longList;
        }
        if (!result.longListOf[target]) {
            result.longListOf[target] = collectSources();
            known.append({ target, result.longListOf[target] });
        }
    }
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
    growWithRoomToSpareInParallel(loops, bodies.m_stampsOfRecords, count);
    growWithRoomToSpareInParallel(loops, bodies.m_stampsOfRecordsForCallers, count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        const Function& function = program.functions[index];
        uint64_t stamp = stampOfRecord(program, FunctionID(index));
        bool isKept = index < numberOfKeptBodies;
        bool looksDifferent = isKept && stamp != bodies.m_stampsOfRecords[index];
        bodies.m_stampsOfRecords[index] = stamp;
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
    // What an operation runs is also read from the records of the functions that the type of an operand lists.
    Vector<uint8_t> typeListsRecordThatLooksDifferent;
    if (someRecordLooksDifferentToCallers) {
        typeListsRecordThatLooksDifferent.grow(program.types.size());
        forEachIndexInParallel(loops, typeListsRecordThatLooksDifferent.size(), [&](unsigned index) {
            typeListsRecordThatLooksDifferent[index] = std::ranges::any_of(program.at(TypeID(index)).functions, [&](FunctionID listed) {
                return listed.index() < numberOfKeptBodies && recordLooksDifferentToCallers[listed.index()];
            });
        });
    }
    Vector<uint8_t> longSetListsRecordThatLooksDifferent;
    if (someRecordLooksDifferentToCallers) {
        longSetListsRecordThatLooksDifferent.grow(program.types.numberOfCalleeSets());
        forEachIndexInParallel(loops, longSetListsRecordThatLooksDifferent.size(), [&](unsigned index) {
            const CalleeSet& set = program.at(CalleeSetID(index));
            longSetListsRecordThatLooksDifferent[index] = set.isLong() && std::ranges::any_of(set.followed().value(), [&](FunctionID listed) {
                return listed.index() < numberOfKeptBodies && recordLooksDifferentToCallers[listed.index()];
            });
        });
    }
    auto namesOnlyRecordsThatLookTheSame = [&](FunctionID id, const ReferencesOfBody& body) {
        bool callsOne = std::ranges::any_of(body.callees, [&](FunctionID callee) {
            return recordLooksDifferentToCallers[callee.index()];
        }) || std::ranges::any_of(body.calleeSets, [&](CalleeSetID set) {
            return !!longSetListsRecordThatLooksDifferent[set.index()];
        }) || std::ranges::any_of(body.enteredSets, [&](CalleeSetID set) {
            return !!longSetListsRecordThatLooksDifferent[set.index()];
        });
        if (callsOne)
            return false;
        BodyIndex* index = program.bodyIndex();
        std::optional<NamesInBody> names = index && program.at(id).hasBody() ? index->namesIn(program, id) : std::nullopt;
        if (!names)
            return !program.at(id).hasBody();
        return std::ranges::none_of(names->types, [&](TypeID type) {
            return type.index() >= typeListsRecordThatLooksDifferent.size() || typeListsRecordThatLooksDifferent[type.index()];
        });
    };
    const bool checksKeptBodies = build.options().verifiesAfterEveryPass;
    growWithRoomToSpareInParallel(loops, bodies.m_bodies, count);
    std::atomic<uint64_t> numberOfBodiesCollected { 0 };
    std::atomic<uint64_t> numberOfBodiesCollectedAsBefore { 0 };
    const bool logsCounts = build.options().logging.logsPhaseTimes;
    const EntriesThatRunNoCode entriesThatRunNoCode = findEntriesThatRunNoCode();
    forEachIndexInParallel(loops, count, largeFunctions(program, loops).span(), [&](unsigned index) {
        ReferencesOfBody& body = bodies.m_bodies[index];
        bool stillHolds = index < numberOfKeptBodies && !bodies.m_isChanged[index] && !recordLooksDifferent[index];
        if (stillHolds && someRecordLooksDifferentToCallers)
            stillHolds = namesOnlyRecordsThatLookTheSame(FunctionID(index), body);
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

    Vector<uint32_t> calleeOffsets(FillWith { }, count + 1, 0u);
    Vector<FunctionID> callees;
    Vector<uint32_t> directCallSites(FillWith { }, count, 0u);
    Vector<uint32_t> linkedCallSites(FillWith { }, count, 0u);
    Vector<uint32_t> calleeSetCallSites(FillWith { }, program.types.numberOfCalleeSets(), 0u);
    Vector<uint32_t> enteredSetCallSites(FillWith { }, program.types.numberOfCalleeSets(), 0u);
    Vector<uint8_t> hasOtherReference(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> callsUnknownCode(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasBodyByFunction(count);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        hasBodyByFunction[index] = hasBody(program, FunctionID(index));
    });
    auto isCalleeWithBody = [&](FunctionID id) {
        return !!hasBodyByFunction[id.index()];
    };
    AOT_THRESHOLD(uint32_t, minCalleesOfLongList, 16);
    Vector<unsigned> hashOfLongList(FillWith { }, count, 0u);
    Vector<uint8_t> hasLongList(FillWith { }, count, static_cast<uint8_t>(0));
    std::atomic<bool> someFunctionHasLongList { false };
    constexpr unsigned prefetchDistance = 4;
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (index + prefetchDistance < count) {
            const ReferencesOfBody& next = bodies.m_bodies[index + prefetchDistance];
            __builtin_prefetch(next.callees.span().data());
            __builtin_prefetch(next.directCallees.span().data());
            __builtin_prefetch(next.otherReferences.span().data());
            __builtin_prefetch(next.calleeSets.span().data());
        }
        const ReferencesOfBody& body = bodies.m_bodies[index];
        calleeOffsets[index + 1] = static_cast<uint32_t>(std::ranges::count_if(body.callees, isCalleeWithBody));
        if (calleeOffsets[index + 1] >= minCalleesOfLongList) {
            Hasher hasher;
            for (FunctionID callee : body.callees)
                add(hasher, callee.index());
            hashOfLongList[index] = hasher.hash();
            calleeOffsets[index + 1] = 0;
            hasLongList[index] = 1;
            someFunctionHasLongList.store(true, std::memory_order_relaxed);
        }
        callsUnknownCode[index] = body.callsUnknownCode;
        for (FunctionID target : body.directCallees)
            WTF::atomicExchangeAdd(&directCallSites[target.index()], 1u, std::memory_order_relaxed);
        for (FunctionID target : body.otherReferences) {
            if (!WTF::atomicLoad(&hasOtherReference[target.index()], std::memory_order_relaxed))
                WTF::atomicStore(&hasOtherReference[target.index()], static_cast<uint8_t>(1), std::memory_order_relaxed);
        }
        for (CalleeSetID set : body.calleeSets)
            WTF::atomicExchangeAdd(&calleeSetCallSites[set.index()], 1u, std::memory_order_relaxed);
        for (CalleeSetID set : body.enteredSets)
            WTF::atomicExchangeAdd(&enteredSetCallSites[set.index()], 1u, std::memory_order_relaxed);
    });
    std::partial_sum(calleeOffsets.begin(), calleeOffsets.end(), calleeOffsets.begin());
    callees.grow(calleeOffsets[count]);
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        if (index + prefetchDistance < count)
            __builtin_prefetch(bodies.m_bodies[index + prefetchDistance].callees.span().data());
        if (hasLongList[index])
            return;
        uint32_t next = calleeOffsets[index];
        for (FunctionID callee : bodies.m_bodies[index].callees) {
            if (isCalleeWithBody(callee))
                callees[next++] = callee;
        }
    });
    CallGraph graph;
    if (someFunctionHasLongList.load(std::memory_order_relaxed)) {
        graph.m_callees.longListOf.fill(0, count);
        UncheckedKeyHashMap<unsigned, Vector<uint32_t, 1>, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> functionsWithList;
        Vector<FunctionID> list;
        for (unsigned index = 0; index < count; ++index) {
            if (!hasLongList[index])
                continue;
            auto& known = functionsWithList.add(hashOfLongList[index], Vector<uint32_t, 1> { }).iterator->value;
            for (uint32_t earlier : known) {
                if (bodies.m_bodies[earlier].callees == bodies.m_bodies[index].callees)
                    graph.m_callees.longListOf[index] = graph.m_callees.longListOf[earlier];
            }
            if (graph.m_callees.longListOf[index])
                continue;
            list.shrink(0);
            for (FunctionID callee : bodies.m_bodies[index].callees) {
                if (isCalleeWithBody(callee))
                    list.append(callee);
            }
            graph.m_callees.longListOf[index] = graph.m_callees.addLongList(list.span());
            known.append(index);
        }
    }
    Vector<uint32_t> longSets;
    for (uint32_t index = 0; index < calleeSetCallSites.size(); ++index) {
        if (enteredSetCallSites[index] || (calleeSetCallSites[index] && program.at(CalleeSetID(index)).isLong()))
            longSets.append(index);
    }
    if (!longSets.isEmpty()) {
        CallGraph::LongSets& lists = graph.m_longSets;
        lists.ids = longSets.map([](uint32_t index) {
            return CalleeSetID(index);
        });
        groupByIndex(longSets.size(), lists.firstMember, lists.members, [&](const auto& add) {
            for (uint32_t set = 0; set < longSets.size(); ++set) {
                std::span<const FunctionID> functions = program.at(CalleeSetID(longSets[set])).followed().value();
                for (FunctionID callee : functions) {
                    if (isCalleeWithBody(callee))
                        add(set, callee);
                }
            }
        });
        groupByIndex(count, lists.firstSetOfMember, lists.setsOfMembers, [&](const auto& add) {
            for (uint32_t set = 0; set < longSets.size(); ++set) {
                for (FunctionID callee : entriesOfGroup(lists.firstMember, lists.members, set))
                    add(callee.index(), set);
            }
        });
        groupByIndex(count, lists.firstSetOfUser, lists.setsOfUsers, [&](const auto& add) {
            for (unsigned index = 0; index < count; ++index) {
                Vector<uint32_t, 8> setsOfUser;
                for (const Vector<CalleeSetID>* sets : { &bodies.m_bodies[index].calleeSets, &bodies.m_bodies[index].enteredSets }) {
                    CalleeSetID previous;
                    for (CalleeSetID set : *sets) {
                        if (set != std::exchange(previous, set) && program.at(set).isLong())
                            setsOfUser.append(static_cast<uint32_t>(std::ranges::lower_bound(longSets, set.index()) - longSets.begin()));
                    }
                }
                AOT::sort(setsOfUser);
                removeRepeatedElements(setsOfUser);
                for (uint32_t set : setsOfUser)
                    add(index, set);
            }
        });
        groupByIndex(longSets.size(), lists.firstUser, lists.users, [&](const auto& add) {
            for (unsigned index = 0; index < count; ++index) {
                for (uint32_t set : entriesOfGroup(lists.firstSetOfUser, lists.setsOfUsers, index))
                    add(set, FunctionID(index));
            }
        });
    }
    forEachIndexInParallel(loops, calleeSetCallSites.size(), [&](unsigned index) {
        uint32_t numberOfSites = calleeSetCallSites[index];
        if (!numberOfSites)
            return;
        for (FunctionID callee : program.at(CalleeSetID(index)).followed().value())
            WTF::atomicExchangeAdd(&linkedCallSites[callee.index()], numberOfSites, std::memory_order_relaxed);
    });
    forEachIndexInParallel(loops, enteredSetCallSites.size(), [&](unsigned index) {
        uint32_t numberOfSites = enteredSetCallSites[index];
        if (!numberOfSites)
            return;
        for (FunctionID entrypoint : program.at(CalleeSetID(index)).written().functions)
            WTF::atomicExchangeAdd(&directCallSites[entrypoint.index()], numberOfSites, std::memory_order_relaxed);
    });
    times.didFinish("addUpReferences"_s);
    markRecordReferences(program, hasOtherReference, loops);
    times.didFinish("markRecordReferences"_s);
    BitVector isEnteredFromOutside = functionsCalledByHost(program, loops);
    BitVector isRoot = isEnteredFromOutside;
    for (const Variable& variable : program.variables) {
        if (variable.function)
            isRoot.set(variable.function.index());
    }
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        if (call.kind == FlowIndirectCallKind::Host)
            isRoot.set(call.callee.index());
    }
    times.didFinish("findRoots"_s);

    Vector<uint8_t> hasCompleteCallerSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasCompleteCalleeSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasOnlyDirectCallers(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint8_t> hasCompleteCallSiteSet(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint32_t> callSitesOfHost(FillWith { }, count, 0u);
    for (const FlowIndirectCall& call : flow.indirectCalls()) {
        ++linkedCallSites[call.callee.index()];
        if (call.kind == FlowIndirectCallKind::Host)
            ++callSitesOfHost[call.callee.index()];
    }
    forEachIndexInParallel(loops, count, [&](unsigned index) {
        RecordFacts function = recordFactsOf(program, FunctionID(index));
        if (!function.hasBody)
            return;
        hasCompleteCallerSet[index] = !isRoot.get(index)
            && (function.hasOnlyDirectCallers() || callerStateOf(flow, FunctionID(index)) == CallerState::AllKnown);
        hasCompleteCalleeSet[index] = !callsUnknownCode[index];
        hasOnlyDirectCallers[index] = !isEnteredFromOutside.get(index)
            && (function.hasOnlyDirectCallers() || !hasOtherReference[index]);
        const FunctionFlow& functionFlow = flow.function(FunctionID(index));
        hasCompleteCallSiteSet[index] = hasOnlyDirectCallers[index]
            || (!isEnteredFromOutside.get(index) && functionFlow.callers == CallerState::AllKnown && functionFlow.closureUse == ClosureUse::CalledOnly);
    });

    graph.m_callees.offsets = WTF::move(calleeOffsets);
    graph.m_callees.targets = WTF::move(callees);
    graph.m_calledByUnknownCode = collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& called) {
        if (!hasCompleteCallerSet[index] && hasBody(program, FunctionID(index)))
            called.append(FunctionID(index));
    });
    graph.m_hasCompleteCalleeSet = createBitVector(hasCompleteCalleeSet);
    graph.m_callSites = WTF::move(linkedCallSites);
    graph.m_callSitesOfHost = WTF::move(callSitesOfHost);
    for (unsigned index = 0; index < count; ++index)
        graph.m_callSites[index] += directCallSites[index];
    graph.m_directCallSites = WTF::move(directCallSites);
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
    auto printFunctions = [&](const auto& functions) {
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
        out.print(" | sites ", numberOfDirectCallSites(function), " | callers ", enumName(callerSet(function)), " | callees ", allCalleesOf(function) ? "Complete"_s : "Unknown"_s, " | entries ", enumName(entrySet(function)), " | ", enumName(recursion(function)), " | rank ", bottomUpRank(function), " | call sites ", numberOfCallSites(function), " ", enumName(callSiteSet(function)));
        out.println(isExpectedByCode(function) ? " | expected by code"_s : ""_s);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
