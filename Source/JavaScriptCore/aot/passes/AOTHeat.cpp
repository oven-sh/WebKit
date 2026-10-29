/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeat.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCodeShapes.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTGuardFactDetails.h"
#include "AOTHeatGraph.h"
#include "AOTIRBuilder.h"
#include "AOTOutlineColdOperations.h"
#include "AOTOutliningCandidates.h"
#include "AOTPass.h"
#include "AOTPhaseTimes.h"
#include "AOTPointsToSolver.h"
#include "AOTProgramCaches.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTSummarizeForInlining.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/HashFunctions.h>
#include <wtf/Hasher.h>
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(HeatModel::coldPercentile < HeatModel::hotPercentile && HeatModel::hotPercentile < 100);

ASCIILiteral nameOf(HeatClass heatClass)
{
    switch (heatClass) {
    case HeatClass::Unknown:
        return "Unknown"_s;
    case HeatClass::RunsOnce:
        return "RunsOnce"_s;
    case HeatClass::Cold:
        return "Cold"_s;
    case HeatClass::Warm:
        return "Warm"_s;
    case HeatClass::Hot:
        return "Hot"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static double maxLayoutAllocations(const Heat& heat, const Program& program, TypeID id)
{
    if (!id || TypeTable::isProvisional(id))
        return 0;
    const Type& type = program.at(id);
    double allocations = 0;
    for (LayoutID layout : membersOf(program, type))
        allocations = std::max(allocations, heat.allocationsOf(layout).predicted());
    for (const ClassPart& part : type.classes) {
        for (TypeID contents : part.writtenContents)
            allocations = std::max(allocations, maxLayoutAllocations(heat, program, contents));
    }
    return allocations;
}

Expected<double> Heat::activationsOf(const Program& program, FunctionID id) const
{
    constexpr double activationsWhereEverythingIsHot = 1e12;

    // Which is expected of no call.
    if (program.at(id).runsOnlyBehindFailedTest())
        return 0;
    if (m_classOfEverything)
        return *m_classOfEverything == HeatClass::Hot ? activationsWhereEverythingIsHot : 0;
    const Entry& entry = m_entries.of(program, id);
    if (entry.heatClass != HeatClass::Unknown || isOfVeryLargeProgram())
        return entry.activations;
    const auto* parser = program.at(id).creator.tryAs<CreatorRecord::Parser>();
    return std::max(entry.activations, parser ? maxLayoutAllocations(*this, program, parser->type) : 0);
}

Expected<double> Heat::estimatedActivationsOf(const Program& program, FunctionID id) const
{
    if (m_classOfEverything || program.at(id).runsOnlyBehindFailedTest())
        return activationsOf(program, id);
    RELEASE_ASSERT(m_hasWeights);
    return m_entries.of(program, id).estimatedActivations;
}

static uint64_t packedKeyOf(auto upperHalf, auto lowerHalf)
{
    return static_cast<uint64_t>(upperHalf.index()) << 32 | lowerHalf.index();
}

Expected<bool> Heat::isAllocatedIn(LayoutID layout, ModuleID module) const
{
    RELEASE_ASSERT(m_hasWeights);
    return std::ranges::binary_search(m_modulesThatAllocate, packedKeyOf(layout, module));
}

Expected<bool> Heat::isStoredUnder(LayoutID layout, PropertyKeyID name) const
{
    RELEASE_ASSERT(m_hasWeights);
    return std::ranges::binary_search(m_layoutsStoredUnderNames, packedKeyOf(name, layout));
}

Expected<double> Heat::allocationsOf(LayoutID layout) const
{
    RELEASE_ASSERT(m_hasWeights);
    return layout.index() < m_allocationsOfLayouts.size() ? m_allocationsOfLayouts[layout.index()] : 0;
}

Expected<HeatClass> Heat::of(const Program& program, FunctionID id) const
{
    if (program.at(id).runsOnlyBehindFailedTest())
        return HeatClass::Cold;
    if (m_classOfEverything)
        return *m_classOfEverything;
    return m_entries.of(program, id).heatClass;
}

Expected<bool> Heat::hasWarmCode(const Program& program, FunctionID id) const
{
    if (program.at(id).runsOnlyBehindFailedTest())
        return false;
    if (m_classOfEverything)
        return !isCold(*m_classOfEverything).predicted();
    return m_entries.of(program, id).hasWarmCode;
}

Expected<uint8_t> Heat::percentileOf(const Program& program, FunctionID id) const
{
    if (program.at(id).runsOnlyBehindFailedTest())
        return 0;
    return m_entries.of(program, id).percentile;
}

Expected<double> Heat::leastHeatOfFastCode(const Program& program, FunctionID id, Expected<double> expectedFrequencyInFunction) const
{
    double frequencyInFunction = expectedFrequencyInFunction.predicted();
    if (!m_thresholds)
        return frequencyConstants.iterationsPerLoop;
    // How often a function is entered is a product along paths of calls, and a call by value spreads over all that it may reach. A loop of its own is in the text.
    if (frequencyInFunction >= HeatModel::minWeightOfCallInLoop)
        return m_thresholds->leastHeatOfFastCodeInLoops / frequencyConstants.iterationsPerLoop;
    return m_entries.of(program, id).isCalledInLoop ? m_thresholds->leastHeatOfFastCodeInLoops : frequencyConstants.iterationsPerLoop;
}

// Where nothing in the program runs more often than one loop, the loop is hot, and so is the function that it calls.
static bool leadsToBody(const Function& function)
{
    return isWrapperCreatedByEngine(function) || function.creator.kind() == CreatorRecord::Kind::Entrypoint;
}

Expected<HeatClass> Heat::classOf(const Program& program, FunctionID id, Expected<double> expectedActivations, Expected<double> expectedFrequencyInFunction) const
{
    double activations = expectedActivations.predicted();
    double frequencyInFunction = expectedFrequencyInFunction.predicted();
    // A share of the program would cut through a handler: its blocks run equally often but for the calls that may throw in between.
    if (m_thresholds && !m_classOfEverything && frequencyInFunction >= HeatModel::minWeightOfNearlyCertainCall && m_entries.of(program, id).isCalledPerEvent)
        return HeatClass::Hot;
    double heat = activations * frequencyInFunction;
    HeatClass heatClass = classOf(heat).predicted();
    if (m_hotIsByRank && !m_classOfEverything && !isCold(heatClass).predicted()) {
        heat = m_entries.of(program, id).rank * frequencyInFunction;
        heatClass = heat >= m_thresholds->hot ? HeatClass::Hot : HeatClass::Warm;
    }
    if (heatClass != HeatClass::Hot || heat >= frequencyConstants.iterationsPerLoop || frequencyInFunction >= HeatModel::minWeightOfCallInLoop)
        return heatClass;
    return m_entries.of(program, id).isCalledInLoop && !leadsToBody(program.at(id)) ? heatClass : HeatClass::Warm;
}

Expected<HeatClass> Heat::classOf(Expected<double> expectedHeat) const
{
    double heat = expectedHeat.predicted();
    if (m_classOfEverything)
        return *m_classOfEverything;
    if (m_seedOfShuffledHeat) [[unlikely]] {
        uint64_t bits = std::bit_cast<uint64_t>(heat);
        return static_cast<HeatClass>(static_cast<uint32_t>(HeatClass::Cold) + drawForShuffledHeat(*m_seedOfShuffledHeat, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32), 3));
    }
    if (!m_thresholds)
        return HeatClass::Unknown;
    if (!(heat > 0) || heat < m_thresholds->cold)
        return HeatClass::Cold;
    return heat >= m_thresholds->hot ? HeatClass::Hot : HeatClass::Warm;
}

static void printWithThreeDigits(PrintStream& out, double number)
{
    NumberToStringBuffer buffer;
    out.print(numberToFixedPrecisionString(number, 3, buffer, true));
}

void Heat::dump(PrintStream& out, const Program& program) const
{
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Entry& entry = m_entries.of(program, id);
        if (entry.heatClass == HeatClass::Unknown)
            continue;
        out.print("f", index, " ", isCompact(program, id).predicted() ? "Compact"_s : nameOf(of(program, id).predicted()), " ", entry.percentile, " ");
        printWithThreeDigits(out, entry.activations);
        out.print("\n");
    }
    out.print("hottest:");
    for (FunctionID id : m_hottestFirst)
        out.print(" f", id.index());
    out.print("\n");
    if (!m_thresholds)
        return;
    out.print("thresholds: cold=");
    printWithThreeDigits(out, m_thresholds->cold);
    out.print(" hot=");
    printWithThreeDigits(out, m_thresholds->hot);
    out.print("\n");
    if (m_valuesInCompactFunctions && !m_classOfEverything)
        out.print("compact: ", m_valuesInCompactFunctions, " of ", m_valuesInAllFunctions, " values\n");
}

namespace {

struct FunctionDensity {
    double density { 0 };
    FunctionID function;
};

// heat is the frequency per activation until computeThresholds multiplies in the function's activations.
struct BlockHeat {
    double heat { 0 };
    uint32_t size { 0 };
    uint32_t functionIndex { 0 };
};

} // anonymous namespace

static Vector<FunctionDensity> sortByDensity(const ParallelLoops& loops, const HeatGraph& graph, const Vector<uint32_t>& functionsWithBody, const Vector<double>& activations)
{
    Vector<FunctionDensity> order(functionsWithBody.size());
    forEachIndexInParallel(loops, order.size(), [&](unsigned k) {
        FunctionID function(functionsWithBody[k]);
        uint32_t size = graph.sizeOf(function);
        order[k] = { activations[function.index()] * graph.massOf(function) / std::max(1u, size), function };
    });
    parallelSort(loops, order.mutableSpan(), [](const FunctionDensity& a, const FunctionDensity& b) {
        return a.density != b.density ? a.density > b.density : a.function < b.function;
    });
    return order;
}

static Vector<uint8_t> computePercentiles(const HeatGraph& graph, const Vector<FunctionDensity>& hottestFirst, uint64_t totalSize)
{
    Vector<uint8_t> result(FillWith { }, graph.numberOfFunctions(), static_cast<uint8_t>(0));
    uint64_t visitedSize = 0;
    uint64_t colderSize = 0;
    for (size_t k = hottestFirst.size(); k--;) {
        auto [density, function] = hottestFirst[k];
        if (k + 1 < hottestFirst.size() && density != hottestFirst[k + 1].density)
            colderSize = visitedSize;
        result[function.index()] = static_cast<uint8_t>(std::min<uint64_t>(99, colderSize * 100 / totalSize));
        visitedSize += graph.sizeOf(function);
    }
    return result;
}

static Vector<BlockHeat> collectApplicationBlocks(const Program& program, const ParallelLoops& loops, const HeatGraph& graph, const Vector<uint32_t>& functionsWithBody)
{
    return collectInParallel<BlockHeat>(loops, functionsWithBody.size(), [&](unsigned k, Vector<BlockHeat>& blocks) {
        uint32_t index = functionsWithBody[k];
        if (isLibraryCode(program, program.functions[index]))
            return;
        for (const HeatBlockGroup& group : graph.blockGroupsOf(FunctionID(index)))
            blocks.append(BlockHeat { group.frequency, group.size, index });
    });
}

// A threshold is compared with the heat of a block, so it comes from blocks: the hottest fifth of the application's code is hot and the coldest quarter is cold.
static HeatThresholds computeThresholds(const ParallelLoops& loops, Vector<BlockHeat> hottestFirst, const Vector<double>& activations, uint64_t maxHotSize, uint64_t maxWarmSize)
{
    forEachIndexInParallel(loops, hottestFirst.size(), [&](unsigned k) {
        hottestFirst[k].heat *= activations[hottestFirst[k].functionIndex];
    });
    parallelSort(loops, hottestFirst.mutableSpan(), [](const BlockHeat& a, const BlockHeat& b) {
        return a.heat > b.heat;
    });
    uint64_t totalSize = 0;
    for (const BlockHeat& block : hottestFirst)
        totalSize += block.size;

    HeatThresholds thresholds;
    std::optional<double> hottestHeatBeyondWarmCode;
    uint64_t hotterSize = 0;
    for (size_t first = 0; first < hottestFirst.size();) {
        double heat = hottestFirst[first].heat;
        uint64_t size = 0;
        for (; first < hottestFirst.size() && hottestFirst[first].heat == heat; ++first)
            size += hottestFirst[first].size;
        bool fitsHotCode = !hotterSize || hotterSize + size <= maxHotSize;
        if (fitsHotCode)
            thresholds.coldestOfHottestCode = heat;
        if (hotterSize && hotterSize + size > maxWarmSize && !hottestHeatBeyondWarmCode)
            hottestHeatBeyondWarmCode = heat;
        if (hotterSize * 100 < totalSize * (100 - HeatModel::hotPercentile) && fitsHotCode && heat >= HeatModel::minWeightOfCallInLoop)
            thresholds.hot = heat;
        uint64_t colderSize = totalSize - hotterSize - size;
        if (colderSize * 100 >= totalSize * HeatModel::coldPercentile)
            thresholds.cold = heat;
        hotterSize += size;
    }
    thresholds.cold = std::clamp(thresholds.cold, 1.0, std::nextafter(1.0, 2.0));
    thresholds.coldWithoutLimit = thresholds.cold;
    if (hottestHeatBeyondWarmCode) {
        thresholds.cold = std::max(thresholds.cold, std::nextafter(*hottestHeatBeyondWarmCode, std::numeric_limits<double>::infinity()));
        thresholds.hot = std::max(thresholds.hot, thresholds.cold);
    }
    return thresholds;
}

static constexpr double minWeightOfCallThatLoopRepeats = frequencyConstants.iterationsPerLoop / 2;

static constexpr HeatThresholds thresholdsOfVeryLargeProgram {
    // The body of a loop runs one time less than its head, so what stands behind one even branch in it runs a little less than half as often as the head. It belongs to the loop.
    .cold = frequencyConstants.iterationsPerLoop * 0.4,
    .hot = HeatModel::activationsOfFunctionThatRunsOften / 2,
    .coldestOfHottestCode = 0,
    .coldWithoutLimit = 1,
    .leastHeatOfFastCodeInLoops = frequencyConstants.iterationsPerLoop,
    .callsInLoopsAreHot = false,
};

static Vector<uint8_t> findFunctionsCalledInLoops(const ParallelLoops& loops, const HeatGraph& graph, std::span<const uint32_t> functionsWithBody)
{
    Vector<uint8_t> isCalledInLoop(FillWith { }, graph.numberOfFunctions(), static_cast<uint8_t>(0));
    forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
        HeatNodeID caller = graph.nodeOf(FunctionID(functionsWithBody[k]));
        std::span<const HeatNodeID> callees = graph.successors(caller);
        for (unsigned index = 0; index < callees.size(); ++index) {
            FunctionID callee = graph.functionOf(callees[index]);
            if (callee && graph.multiplierOf(caller, index) >= minWeightOfCallThatLoopRepeats)
                AtomicRef(isCalledInLoop[callee.index()]).store(1, std::memory_order_relaxed);
        }
    });
    return isCalledInLoop;
}

static void followCalls(const HeatGraph& graph, HeatNodeID caller, double minWeight, Vector<uint8_t>& isReached, Vector<uint32_t>& worklist)
{
    std::span<const HeatNodeID> callees = graph.successors(caller);
    for (unsigned index = 0; index < callees.size(); ++index) {
        FunctionID callee = graph.functionOf(callees[index]);
        if (!callee || isReached[callee.index()] || graph.multiplierOf(caller, index) < minWeight)
            continue;
        isReached[callee.index()] = 1;
        worklist.append(callee.index());
    }
}

static void followCallsFromWorklist(const HeatGraph& graph, double minWeight, Vector<uint8_t>& isReached, Vector<uint32_t>& worklist)
{
    while (!worklist.isEmpty())
        followCalls(graph, graph.nodeOf(FunctionID(worklist.takeLast())), minWeight, isReached, worklist);
}

// What such a function calls nearly every time runs as often as it.
static Vector<uint8_t> followNearlyCertainCalls(const HeatGraph& graph, const Vector<uint8_t>& isCalledInLoop)
{
    Vector<uint8_t> runsInLoop = isCalledInLoop;
    Vector<uint32_t> worklist;
    for (uint32_t index = 0; index < runsInLoop.size(); ++index) {
        if (runsInLoop[index])
            worklist.append(index);
    }
    followCallsFromWorklist(graph, HeatModel::minWeightOfNearlyCertainCall, runsInLoop, worklist);
    return runsInLoop;
}

// What the host calls once for each request, message or tick, and the body behind an entry that the engine has put in front of it.
static Vector<uint8_t> findFunctionsCalledPerEvent(const Program& program, const HeatGraph& graph)
{
    Vector<uint8_t> isCalledPerEvent(FillWith { }, graph.numberOfFunctions(), static_cast<uint8_t>(0));
    Vector<uint32_t> worklist;
    followCalls(graph, graph.hostNode(), HeatModel::eventsPerRun, isCalledPerEvent, worklist);
    while (!worklist.isEmpty()) {
        FunctionID entry(worklist.takeLast());
        if (leadsToBody(program.at(entry)))
            followCalls(graph, graph.nodeOf(entry), HeatModel::minWeightOfNearlyCertainCall, isCalledPerEvent, worklist);
    }
    return isCalledPerEvent;
}

// A caller that runs once, or that waits between its steps, says nothing about how often the callee runs.
static bool hasManyCallers(const Program& program, const FlowResults& flow, const HeatGraph& graph, FunctionID id)
{
    unsigned callers = 0;
    for (HeatNodeID node : graph.predecessors(graph.nodeOf(id))) {
        FunctionID caller = graph.functionOf(node);
        if (caller && !flow.runsAtMostOnce(program, caller) && !program.at(caller).has(FunctionFlags::IsAsync))
            ++callers;
    }
    return callers >= HeatModel::callersOfFunctionThatRunsOften;
}

// A function's callers are seen at the entry that the engine put before it, which tests or unpacks a parameter, packs a result, binds arguments or starts an async function or a generator.
static void followEntriesMadeByEngine(const Program& program, const FlowResults& flow, const HeatGraph& graph, std::span<const uint32_t> functionsWithBody, Vector<uint8_t>& runsOften)
{
    Vector<uint32_t> worklist;
    for (uint32_t index : functionsWithBody) {
        if (runsOften[index] && leadsToBody(program.at(FunctionID(index))))
            worklist.append(index);
    }
    while (!worklist.isEmpty()) {
        HeatNodeID entry = graph.nodeOf(FunctionID(worklist.takeLast()));
        std::span<const HeatNodeID> callees = graph.successors(entry);
        for (unsigned index = 0; index < callees.size(); ++index) {
            FunctionID callee = graph.functionOf(callees[index]);
            if (!callee || runsOften[callee.index()] || graph.multiplierOf(entry, index) < HeatModel::minWeightOfNearlyCertainCall)
                continue;
            if (flow.runsAtMostOnce(program, callee))
                continue;
            runsOften[callee.index()] = 1;
            if (leadsToBody(program.at(callee)))
                worklist.append(callee.index());
        }
    }
}

// The structure of a very large program does not tell which of its cold functions run, but it tells which hardly do.
static Vector<uint8_t> findCompactFunctions(const Program& program, const FlowResults& flow, const ParallelLoops& loops, const HeatGraph& graph, std::span<const uint32_t> functionsWithBody, const Vector<uint8_t>& isExpectedToRun)
{
    auto findWhatBusyCodeReaches = [&](double minWeight) {
        Vector<uint32_t> worklist;
        Vector<uint8_t> isReached = isExpectedToRun;
        for (uint32_t index : functionsWithBody) {
            if (isExpectedToRun[index])
                worklist.append(index);
        }
        followCalls(graph, graph.hostNode(), minWeightOfCallThatLoopRepeats, isReached, worklist);
        for (uint32_t index : functionsWithBody) {
            if (!flow.runsAtMostOnce(program, FunctionID(index)))
                followCalls(graph, graph.nodeOf(FunctionID(index)), HeatModel::minWeightOfCallInLoop, isReached, worklist);
        }
        followCallsFromWorklist(graph, minWeight, isReached, worklist);
        return isReached;
    };
    Vector<uint8_t> isNearBusyCode;
    Vector<uint8_t> mayRun;
    runTasksInParallel(loops, {
        [&] { isNearBusyCode = findWhatBusyCodeReaches(HeatModel::minWeightOfCallNearBusyCode); },
        [&] { mayRun = findWhatBusyCodeReaches(HeatModel::minWeightOfCallToFollow); },
    });

    Vector<uint32_t> worklist;
    for (uint32_t index : functionsWithBody) {
        FunctionID id(index);
        if (mayRun[index] || flow.runsAtMostOnce(program, id))
            continue;
        unsigned callers = 0;
        for (HeatNodeID node : graph.predecessors(graph.nodeOf(id)))
            callers += !!graph.functionOf(node);
        bool hasHeavyLoop = graph.massOf(id) >= HeatModel::minMassPerValueWithHeavyLoop * graph.sizeOf(id);
        if (callers <= HeatModel::maxCallersOfCompactFunction && !hasHeavyLoop)
            continue;
        mayRun[index] = 1;
        worklist.append(index);
    }
    followCallsFromWorklist(graph, HeatModel::minWeightOfNearlyCertainCall, mayRun, worklist);

    auto waitsFarFromBusyCode = [&](FunctionID id) {
        if (!program.at(id).has(FunctionFlags::IsAsync) || isNearBusyCode[id.index()])
            return false;
        return std::ranges::none_of(graph.blockGroupsOf(id), [](const HeatBlockGroup& group) {
            return group.frequency >= thresholdsOfVeryLargeProgram.cold;
        });
    };

    Vector<uint8_t> isCompact(FillWith { }, graph.numberOfFunctions(), static_cast<uint8_t>(0));
    for (uint32_t index : functionsWithBody) {
        FunctionID id(index);
        isCompact[index] = waitsFarFromBusyCode(id) || (!mayRun[index] && graph.sizeOf(id) > HeatModel::maxSizeOfSmallFunction);
    }
    return isCompact;
}

// A value that may be one of more layouts than this tells nothing about what lies under the name.
AOT_THRESHOLD(size_t, maxLayoutsOfStoredValue, 4);

// `Object.create(null)`, before and after the call is resolved.
static bool makesDictionary(const Program& program, const Function& function, ValueID id)
{
    const Value* value = &function.at(id);
    while (isCast(value->opcode))
        value = &function.at(value->operand(0));
    if (value->opcode == Opcode::NewDictionary)
        return true;
    const BuiltinTable& builtins = program.builtins();
    BuiltinID callee;
    if (value->opcode == Opcode::Call)
        callee = singleBuiltinCalleeOf(program, function, *value);
    else if (value->opcode == Opcode::CallBuiltin)
        callee = builtins.canonicalID(builtins.at(value->overload()).builtin);
    if (callee != Builtins::ObjectCreate)
        return false;
    std::span<const ValueID> arguments = value->argumentsOperands();
    return arguments.size() == 1 && program.typeOf(function, arguments[0]).hasOnlyPrimitives(Primitive::Null);
}

static Vector<uint64_t> collectLayoutsStoredUnderNames(const Program& program, const ParallelLoops& loops, std::span<const uint32_t> functionsWithBody)
{
    Vector<uint64_t> allStored = collectInParallel<uint64_t>(loops, functionsWithBody.size(), [&](unsigned k, Vector<uint64_t>& stored) {
        const Function& function = program.functions[functionsWithBody[k]];
        auto add = [&](PropertyKeyID name, ValueID value) {
            if (!name || !value)
                return;
            if (makesDictionary(program, function, value)) {
                stored.append(packedKeyOf(name, LayoutID()));
                return;
            }
            LayoutSetID layouts = program.typeOf(function, value).layouts;
            if (!layouts || program.at(layouts).written().size() > maxLayoutsOfStoredValue)
                return;
            for (LayoutID layout : program.at(layouts).completeMembers())
                stored.append(packedKeyOf(name, layout));
        };
        forEachValueListedOrNot(program, FunctionID(functionsWithBody[k]), { Opcode::PropSet, Opcode::PropDefine, Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::NewObject }, [&](ValueID, const Value& value) {
            switch (value.opcode) {
            case Opcode::PropSet:
            case Opcode::PropDefine:
                add(value.key(), value.operand(OperandIndex::PropSet::value));
                break;
            case Opcode::PutSlot:
            case Opcode::PutSlotOrAdd:
                add(value.key(), value.operand(OperandIndex::PutSlot::value));
                break;
            case Opcode::NewObject: {
                const Vector<Field>& fields = program.at(declaredLayoutOf(program, value.layout())).fields;
                std::span<const uint32_t> listed = value.fields();
                for (size_t position = 0; position < listed.size(); ++position)
                    add(fields[listed[position]].key, value.operand(OperandIndex::NewObject::values + position));
                break;
            }
            default:
                break;
            }
        });
    });
    sortAndRemoveDuplicates(loops, allStored);
    return allStored;
}

static_assert(OperandIndex::PropSet::value == OperandIndex::PropDefine::value && OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);

Heat estimateHeat(const Program& program, const CallGraph& callGraph, const FlowResults& flow, BuildContext& build, const EffectSummaries* summariesFromFlow, const BodyReferences* references, const BodyWeights* keptWeights, BodyWeights* madeWeights, HeatFor heatFor)
{
    const ParallelLoops& loops = build.parallelLoops();
    PhaseTimes times(build, PassID::EstimateHeat);
    std::optional<EffectSummaries> ownSummaries;
    if (!summariesFromFlow)
        summariesFromFlow = &ownSummaries.emplace(createSummariesFromFlow(program, flow, build));
    times.didFinish("createSummariesFromFlow"_s);
    HeatGraph graphOfClasses;
    HeatGraph graph = buildHeatGraph(program, callGraph, flow, *summariesFromFlow, build, references, keptWeights, madeWeights, &graphOfClasses);
    times.didFinish("buildHeatGraph"_s);
    const bool estimatesTwice = graphOfClasses.numberOfFunctions();
    Vector<double> ranks;
    Vector<double> placesInOrder;

    unsigned count = graph.numberOfFunctions();
    Heat heat;
    Vector<double> activations;
    Vector<BlockHeat> applicationBlocks;
    Vector<uint32_t> functionsWithBody = findIndicesInParallel(loops, count, [&](unsigned i) {
        return graph.sizeOf(FunctionID(i)) > 0;
    });
    uint64_t totalSize = 0;
    for (uint32_t i : functionsWithBody)
        totalSize += graph.sizeOf(FunctionID(i));
    // Most of a very large program never runs, and which part does is not in the estimate: its products along paths of calls pass every bound, and a call that nobody follows spreads evenly over all that it may reach.
    // The thresholds of a very large program are fixed, and no class in it comes from a threshold.
    const bool isVeryLargeProgram = totalSize > maxValuesOfOrdinaryProgram(build);
    heat.m_hasWeights = heatFor == HeatFor::Everything || !isVeryLargeProgram;
    Vector<uint8_t> isCalledInLoop;
    Vector<uint8_t> runsInLoop;
    Vector<uint8_t> isCalledPerEvent;
    Vector<uint8_t> functionsThatRunOften;
    Vector<uint8_t> isCompact;
    // estimateActivations is serial, so everything that needs only the graph runs next to it.
    runTasksInParallel(loops, {
        [&] {
            if (heat.m_hasWeights)
                activations = estimateActivations(estimatesTwice ? graphOfClasses : graph);
        },
        [&] {
            if (heat.m_hasWeights && estimatesTwice)
                ranks = estimateActivations(graph);
        },
        [&] {
            if (!isVeryLargeProgram)
                applicationBlocks = collectApplicationBlocks(program, loops, graph, functionsWithBody);
            heat.m_entries = PerFunction<Heat::Entry>(count);
            if (heat.m_hasWeights)
                heat.m_layoutsStoredUnderNames = collectLayoutsStoredUnderNames(program, loops, functionsWithBody.span());
            isCalledInLoop = findFunctionsCalledInLoops(loops, graph, functionsWithBody.span());
            // Each follows calls on one thread, and none depends on another's results.
            runTasksInParallel(loops, {
                [&] { runsInLoop = followNearlyCertainCalls(graph, isCalledInLoop); },
                [&] { isCalledPerEvent = findFunctionsCalledPerEvent(program, graph); },
                [&] {
                    if (!isVeryLargeProgram)
                        return;
                    functionsThatRunOften.fill(static_cast<uint8_t>(0), graph.numberOfFunctions());
                    forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
                        FunctionID id(functionsWithBody[k]);
                        functionsThatRunOften[id.index()] = hasManyCallers(program, flow, graph, id);
                    });
                    followEntriesMadeByEngine(program, flow, graph, functionsWithBody.span(), functionsThatRunOften);
                    // A function called by a loop and few others is entered no more often than cold code. It may still run.
                    Vector<uint8_t> functionsExpectedToRun = functionsThatRunOften;
                    for (uint32_t index : functionsWithBody)
                        functionsExpectedToRun[index] |= isCalledInLoop[index];
                    isCompact = findCompactFunctions(program, flow, loops, graph, functionsWithBody.span(), functionsExpectedToRun);
                },
            });
        },
    });
    times.didFinish("estimateActivations"_s);
    if (functionsWithBody.isEmpty())
        return Heat();

    Vector<uint8_t> runsWithEveryEvent;
    if (estimatesTwice && heat.m_hasWeights) {
        for (unsigned index = 0; index < count; ++index)
            activations[index] = std::max(activations[index], graphOfClasses.entriesPerEventOf(FunctionID(index)));
        placesInOrder = ranks;
        runsWithEveryEvent = followNearlyCertainCalls(graph, isCalledPerEvent);
        // A rank says which of two functions runs more often, not that either runs often: what comes with events has one only where it is entered as often for each event as a loop in a loop would enter it.
        // The streams, emitters and sockets of the library call each other in cycles that are no loops: of the library, only what a loop calls has one.
        // What a handler of the application calls nearly every time runs as often as the handler, and keeps its rank.
        for (unsigned index = 0; index < count; ++index) {
            if (graphOfClasses.entriesPerEventOf(FunctionID(index)) < 1)
                continue;
            bool isOfLibrary = isLibraryCode(program, program.functions[index]);
            if (runsWithEveryEvent[index] && !isOfLibrary)
                continue;
            if (activations[index] <= HeatModel::minEntriesPerEventOfHotCode * HeatModel::eventsPerRun || (isOfLibrary && !isCalledInLoop[index]))
                ranks[index] = 0;
        }
    }
    if (heat.m_hasWeights && !isVeryLargeProgram) {
        std::optional<HeatThresholds> thresholdsOfRanks;
        if (estimatesTwice)
            thresholdsOfRanks = computeThresholds(loops, applicationBlocks, ranks, build.limit(LimitID::MaxHotCodeValues), build.limit(LimitID::MaxWarmCodeValues));
        heat.m_thresholds = computeThresholds(loops, WTF::move(applicationBlocks), activations, build.limit(LimitID::MaxHotCodeValues), build.limit(LimitID::MaxWarmCodeValues));
        if (thresholdsOfRanks) {
            // Most of the library runs in no one program: its code is hot only where both estimates put it beside the application's hot code.
            if (std::isfinite(thresholdsOfRanks->hot)) {
                double ranksPerActivation = thresholdsOfRanks->hot / heat.m_thresholds->hot;
                for (uint32_t index : functionsWithBody) {
                    if (isLibraryCode(program, program.functions[index]))
                        ranks[index] = std::min(ranks[index], activations[index] * ranksPerActivation);
                }
            }
            heat.m_thresholds->hot = thresholdsOfRanks->hot;
            heat.m_hotIsByRank = true;
        }
    }
    Vector<uint8_t> percentiles;
    if (estimatesTwice && heat.m_hasWeights) {
        // A helper of the library that is entered from many places keeps its place among the functions: whoever calls it takes it in.
        for (unsigned index = 0; index < count; ++index) {
            bool isBusyHelper = graph.sizeOf(FunctionID(index)) <= HeatModel::maxSizeOfHelper && activations[index] > HeatModel::minEntriesPerEventOfHotCode * HeatModel::eventsPerRun && isLibraryCode(program, program.functions[index]);
            if (!isBusyHelper)
                placesInOrder[index] = ranks[index];
        }
    }
    const Vector<double>& ranksOrActivations = estimatesTwice ? placesInOrder : activations;
    if (heat.m_hasWeights) {
        heat.m_allocationsOfLayouts.fill(0, program.layouts.size());
        for (const AllocationSite& site : flow.allocationSites()) {
            if (site.layout && site.function && site.layout.index() < program.layouts.size() && site.function.index() < activations.size()) {
                heat.m_allocationsOfLayouts[site.layout.index()] += ranksOrActivations[site.function.index()];
                if (ModuleID module = program.at(site.function).module)
                    heat.m_modulesThatAllocate.append(packedKeyOf(site.layout, module));
            }
        }
        AOT::sort(heat.m_modulesThatAllocate);
        removeRepeatedElements(heat.m_modulesThatAllocate);

        Vector<FunctionDensity> order = sortByDensity(loops, graph, functionsWithBody, ranksOrActivations);
        percentiles = computePercentiles(graph, order, totalSize);
        heat.m_hottestFirst = Vector<FunctionID>(order.size(), [&](size_t k) {
            return order[k].function;
        });
    }
    times.didFinish("computeThresholds"_s);
    forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
        uint32_t i = functionsWithBody[k];
        FunctionID id(i);
        // Without weights: classifyByCallers gives the entry its activations and its percentile.
        heat.m_entries[id] = heat.m_hasWeights ? Heat::Entry { activations[i], HeatClass::RunsOnce, percentiles[i], activations[i] } : Heat::Entry { 1, HeatClass::RunsOnce };
        if (heat.m_hotIsByRank)
            heat.m_entries[id].rank = ranks[i];
        heat.m_entries[id].isCalledInLoop = runsInLoop[i];
        heat.m_entries[id].isCalledPerEvent = !isVeryLargeProgram && isCalledPerEvent[i] && !isLibraryCode(program, program.functions[i]);
        if (!flow.runsAtMostOnce(program, id))
            heat.m_entries[id].heatClass = heat.classOf(program, id, 1).predicted();
    });
    if (estimatesTwice) {
        for (uint32_t i : functionsWithBody) {
            Heat::Entry& entry = heat.m_entries[FunctionID(i)];
            entry.isCompact = !(activations[i] > 0) && entry.heatClass == HeatClass::Cold && graph.sizeOf(FunctionID(i)) > HeatModel::maxSizeOfSmallFunction;
        }
        // A call of a built-in enters the function that stands behind it, and no edge of the graph says so.
        for (const RuntimeFunctionEntry& behindBuiltin : program.runtimeFunctions) {
            if (behindBuiltin.function)
                heat.m_entries[behindBuiltin.function].isCompact = false;
        }
    }
    times.didFinish("classifyFunctions"_s);
    if (isVeryLargeProgram) {
        forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
            FunctionID id(functionsWithBody[k]);
            Heat::Entry& entry = heat.m_entries[id];
            bool runsOften = functionsThatRunOften[id.index()];
            entry.activations = runsOften ? HeatModel::activationsOfFunctionThatRunsOften : 1;
            entry.percentile = runsOften ? HeatModel::percentileOfFunctionThatRunsOften : HeatModel::percentileOfOtherFunctions;
            if (entry.heatClass != HeatClass::RunsOnce)
                entry.heatClass = runsOften ? HeatClass::Hot : HeatClass::Cold;
        });
        heat.m_thresholds = thresholdsOfVeryLargeProgram;
        times.didFinish("classifyByCallers"_s);

        heat.m_valuesInAllFunctions = totalSize;
        for (uint32_t index : functionsWithBody) {
            Heat::Entry& entry = heat.m_entries[FunctionID(index)];
            entry.isCompact = isCompact[index] && entry.heatClass == HeatClass::Cold;
            if (entry.isCompact)
                heat.m_valuesInCompactFunctions += graph.sizeOf(FunctionID(index));
        }
        times.didFinish("findCompactFunctions"_s);
    }

    switch (build.recordedOptions().heat()) {
    case HeatOverride::None:
        break;
    case HeatOverride::OnlyLoopsAreHot:
        for (uint32_t i : functionsWithBody)
            heat.m_entries[FunctionID(i)] = Heat::Entry { 1, HeatClass::Warm, 50 };
        heat.m_hotIsByRank = false;
        heat.m_valuesInCompactFunctions = 0;
        heat.m_valuesInAllFunctions = 0;
        heat.m_thresholds = HeatThresholds { .cold = frequencyConstants.unlikelyEdgeShare, .hot = frequencyConstants.iterationsPerLoop };
        break;
    case HeatOverride::EverythingIsHot:
        heat.m_classOfEverything = HeatClass::Hot;
        break;
    case HeatOverride::EverythingIsCold:
        heat.m_classOfEverything = HeatClass::Cold;
        break;
    case HeatOverride::NothingIsHottest:
        heat.m_hottestFirst.clear();
        break;
    case HeatOverride::Shuffled:
        heat.m_seedOfShuffledHeat = seedOfShuffledHeat(build);
        for (uint32_t i : functionsWithBody) {
            Heat::Entry& entry = heat.m_entries[FunctionID(i)];
            entry.heatClass = static_cast<HeatClass>(static_cast<uint32_t>(HeatClass::RunsOnce) + drawForShuffledHeat(*heat.m_seedOfShuffledHeat, i, 0, 4));
            entry.isCompact = drawForShuffledHeat(*heat.m_seedOfShuffledHeat, i, 1, 4) == 0;
            entry.isCalledInLoop = drawForShuffledHeat(*heat.m_seedOfShuffledHeat, i, 2, 2);
            entry.isCalledPerEvent = drawForShuffledHeat(*heat.m_seedOfShuffledHeat, i, 3, 4) == 0;
        }
        break;
    }
    forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
        FunctionID id(functionsWithBody[k]);
        Heat::Entry& entry = heat.m_entries[id];
        entry.hasWarmCode = std::ranges::any_of(graph.blockGroupsOf(id), [&](const HeatBlockGroup& group) {
            bool runsOnce = entry.heatClass == HeatClass::RunsOnce && group.frequency <= roundingOfFrequencySums;
            return !runsOnce && !isCold(heat.classOf(entry.activations * group.frequency)).predicted();
        });
    });
    build.destroyLater(WTF::move(graph));
    return heat;
}

// The tests of the parameters stand in front of the body, and each gives a share to its failure. What passes them is what the function was entered for.
static double frequencyBehindEntryTests(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    BlockID block = entryBlock;
    for (size_t tests = 0; tests < parameters.size(); ++tests) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            break;
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode != Opcode::TypeTest || !parameters.contains(test.operand(0)))
            break;
        if (function.at(terminator.taken().block).frequency.predicted() != BlockFrequency::Normal || function.at(terminator.notTaken().block).frequency.predicted() == BlockFrequency::Normal)
            break;
        block = terminator.taken().block;
    }
    return analyses.blockFrequencies().frequencyOf(block).predicted();
}

Expected<HeatClass> heatClassOf(const Heat& heat, FunctionAnalyses& analyses, BlockID block)
{
    const AnalysisContext& context = analyses.context();
    Expected<double> activations = heat.activationsOf(context.program, context.id);
    bool isColdEverywhere = heat.classOf(context.program, context.id, activations, roundingOfFrequencySums).predicted() == HeatClass::Cold && runsNoBlockMoreOftenThanEntry(analyses.cfg());
    Expected<HeatClass> heatClass = isColdEverywhere ? Expected(HeatClass::Cold) : heat.classOf(context.program, context.id, activations, analyses.blockFrequencies().frequencyOf(block));
    if (heatClass.predicted() == HeatClass::Cold && !isColdEverywhere)
        heatClass = heat.classOf(context.program, context.id, activations, analyses.blockFrequencies().frequencyOf(block) / frequencyBehindEntryTests(analyses));
    switch (context.build.recordedOptions().heat()) {
    case HeatOverride::EverythingIsHot:
    case HeatOverride::EverythingIsCold:
        return heatClass;
    case HeatOverride::Shuffled:
        return static_cast<HeatClass>(static_cast<uint32_t>(HeatClass::RunsOnce) + drawForShuffledHeat(*seedOfShuffledHeat(context.build), context.id.index(), block.index(), 4));
    case HeatOverride::None:
    case HeatOverride::OnlyLoopsAreHot:
    case HeatOverride::NothingIsHottest:
        break;
    }
    if (analyses.executionCountOf(block) == ExecutionCount::AtMostOnce)
        return HeatClass::RunsOnce;
    // The throw costs more than all that leads up to it, however often the function is entered.
    if (heatClass.predicted() > HeatClass::Cold && analyses.blockFrequencies().alwaysEndsInThrow(block))
        return HeatClass::Cold;
    return heatClass;
}

Expected<bool> isInColderPartOfProgram(const Heat& heat, const Program& program, FunctionID id)
{
    constexpr uint8_t firstPercentileOfHotterPart = 35;

    Expected<HeatClass> heatClass = heat.of(program, id);
    if (isHot(heatClass).predicted() || isCold(heatClass).predicted())
        return isCold(heatClass).predicted();
    std::span<const FunctionID> hottestFirst = heat.hottestFirst().predicted();
    if (hottestFirst.empty() || !heat.percentileOf(program, hottestFirst.front()).predicted())
        return false;
    return heat.percentileOf(program, id) < firstPercentileOfHotterPart;
}

Expected<HeatClass> heatClassOf(FunctionPassContext& context, BlockID block)
{
    return heatClassOf(context.inputs.heat, context.analyses, block);
}

Expected<HeatClass> heatClassByOwnLoops(FunctionPassContext& context, BlockID block)
{
    switch (context.build.recordedOptions().heat()) {
    case HeatOverride::EverythingIsHot:
        return HeatClass::Hot;
    case HeatOverride::EverythingIsCold:
        return HeatClass::Cold;
    case HeatOverride::Shuffled:
        return static_cast<HeatClass>(static_cast<uint32_t>(HeatClass::RunsOnce) + drawForShuffledHeat(*seedOfShuffledHeat(context.build), context.id.index(), block.index(), 4));
    case HeatOverride::None:
    case HeatOverride::OnlyLoopsAreHot:
    case HeatOverride::NothingIsHottest:
        break;
    }
    if (context.analyses.executionCountOf(block) == ExecutionCount::AtMostOnce)
        return HeatClass::RunsOnce;
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    if (frequencies.alwaysEndsInThrow(block))
        return HeatClass::Cold;
    return (frequencies.frequencyOf(block) > 1.0).predicted() ? HeatClass::Hot : HeatClass::Warm;
}

Expected<bool> isExpectedToRun(FunctionPassContext& context, const NaturalLoop& loop)
{
    return context.analyses.blockFrequencies().classOf(loop.header()).predicted() == BlockFrequency::Normal && !isCold(heatClassOf(context, loop.header())).predicted();
}

static constexpr uint64_t outlinedBodyOverheadBytes = 48;
static constexpr size_t sitesInLongHashRun = 1024;

constexpr OpcodeSet callOpcodes = builtinCallOpcodes | OpcodeSet { Opcode::Call, Opcode::CallList, Opcode::CallDirect, Opcode::CallForwardVarargs, Opcode::CallMethod, Opcode::CallResolved, Opcode::AwaitCallBySavingFrame };

struct OutliningSite {
    bool isRegion() const { return !!exitBlock; }

    FunctionID function { };
    BlockID entryBlock { };
    uint32_t firstIndex { 0 };
    uint32_t numberOfEntryValues { 0 };
    Vector<BlockID, 2> interiorBlocks { };
    BlockID exitBlock { };
    std::optional<uint32_t> exitArgument { };
    ValueID root { };
    // Whether the operations that a stack trace can show stand for a call in tail position: all of them do or none does.
    std::optional<bool> isTailCall { };
    // Whether those operations have a second place, which a stack trace shows for some of their frames: all of them have it or none has.
    std::optional<bool> isPartOfConstruction { };
    // The built-in function that those operations run steps of, if all of them run steps of the same one or of none. The call of the body names it.
    std::optional<BuiltinID> calledBuiltin { };
    bool rootIsUsed { false };
    bool canPayOff { false };
    uint32_t orderOfEntryBlock { 0 };
    Vector<ValueID, 6> liveIns { };
    TypeID resultType { };
    uint32_t codeBytes { 0 };
    uint32_t callBytes { 0 };
    unsigned hash { 0 };
};

static std::span<const ValueID> entryValuesOf(const Function& function, const OutliningSite& site)
{
    return function.at(site.entryBlock).values.subspan(site.firstIndex, site.numberOfEntryValues);
}

static void forEachValueListAndTerminator(const Function& function, const OutliningSite& site, const Invocable<void(std::span<const ValueID>)> auto& onValues, const Invocable<void(const Terminator&)> auto& onTerminator)
{
    onValues(entryValuesOf(function, site));
    if (!site.isRegion())
        return;
    onTerminator(function.at(site.entryBlock).terminator);
    for (BlockID block : site.interiorBlocks) {
        onValues(function.at(block).values.span());
        onTerminator(function.at(block).terminator);
    }
}

static void forEachPayloadWordThatBodiesShare(const Value& value, const OutliningSite& site, const Invocable<void(uint32_t)> auto& functor)
{
    uint64_t omittedWords = 0;
    for (const PayloadFieldInfo& field : payloadFieldsOf(value.opcode)) {
        if (field.kind != PayloadKind::AccessSite)
            continue;
        RELEASE_ASSERT(field.firstWord < 64);
        omittedWords |= uint64_t { 1 } << field.firstWord;
    }
    if (std::optional<unsigned> word = PayloadWord::calledBuiltin(value.opcode); word && site.calledBuiltin)
        omittedWords |= uint64_t { 1 } << *word;
    unsigned position = 0;
    auto visit = [&](uint32_t word) {
        bool isOmitted = position < 64 && (omittedWords >> position & 1);
        ++position;
        functor(isOmitted ? absentPayloadWord : word);
    };
    functor(static_cast<uint16_t>(value.flags));
    for (uint32_t word : value.inlinePayload())
        visit(word);
    functor(static_cast<uint32_t>(value.outOfLinePayload().size()));
    for (uint32_t word : value.outOfLinePayload())
        visit(word);
}

static bool readsConditionOperands(const Terminator& terminator)
{
    if (!terminator.isConditionalBranch())
        return false;
    return terminator.kind == TerminatorKind::TestAndBranch || std::ranges::any_of(terminator.targets, [](const Target& target) {
        return !target.proofs.isEmpty();
    });
}

namespace {

struct OutliningNormalForm {
    enum class Mark : uint32_t { Value, Parameter, Terminator, InteriorTarget, ExitTarget, Proof, Result, Absent, InSite, LiveIn, Constant };

    OutliningNormalForm(const Program& program, const Function& function, const OutliningSite& site, uint64_t maxLiveIns)
        : program(program)
        , function(function)
        , site(site)
        , maxLiveIns(maxLiveIns)
    {
        for (ValueID id : entryValuesOf(function, site))
            appendValue(id);
        if (site.isRegion()) {
            appendTerminator(function.at(site.entryBlock).terminator);
            for (BlockID blockID : site.interiorBlocks) {
                const Block& block = function.at(blockID);
                for (ValueID parameter : block.parameters) {
                    if (isFact(parameter))
                        continue;
                    append(Mark::Parameter, function.at(parameter).type.index());
                    definitions.append(parameter);
                }
                for (ValueID id : block.values)
                    appendValue(id);
                appendTerminator(block.terminator);
            }
        }
        append(Mark::Result, site.rootIsUsed);
        // A constant leaves nothing in the form where it stands, and it may be what the run answers.
        if (site.rootIsUsed && isRematerializable(function.at(site.root)))
            appendReference(site.root);
        append(site.resultType.index());
    }

    void append(auto... added) { (words.append(static_cast<uint32_t>(added)), ...); }

    // A fact leaves nothing in the form: it makes no code, and the shared function is entered behind every fact that its operations name.
    bool isFact(ValueID id) const { return program.typeOf(function, id).isOpaque(OpaqueKind::Fact); }

    void appendValue(ValueID id)
    {
        const Value& value = function.at(id);
        if (isRematerializable(value) || isFact(id))
            return;
        append(Mark::Value, value.opcode, value.type.index());
        bool dependsOnModule = std::ranges::any_of(payloadFieldsOf(value.opcode), [](const PayloadFieldInfo& field) {
            return field.kind == PayloadKind::Origin;
        });
        if (dependsOnModule)
            append(function.module.index());
        payloads.append(&value);
        appendOperands(value, false);
        definitions.append(id);
    }

    void appendOperands(const auto& operation, bool needsAllInSite)
    {
        uint32_t numberOfOperands = operation.numberOfOperands();
        append(numberOfOperands);
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            if (appendReference(operation.operand(operandIndex)) && (needsAllInSite || mustBeDefinition(operation.operandKind(operandIndex))))
                hasRequiredDefinitionOutside = true;
        }
    }

    void appendTerminator(const Terminator& terminator)
    {
        append(Mark::Terminator, terminator.kind);
        appendOperands(terminator, readsConditionOperands(terminator));

        append(terminator.flags);
        std::array<uint32_t, numberOfInlinePayloadWords> inlinePayload = terminator.inlinePayload();
        append(inlinePayload.size());
        words.append(std::span<const uint32_t> { inlinePayload });
        append(terminator.outOfLinePayload().size());
        words.append(terminator.outOfLinePayload());

        append(terminator.targets.size());
        for (const Target& target : terminator.targets) {
            if (target.block == site.exitBlock) {
                append(Mark::ExitTarget);
                if (site.exitArgument)
                    appendReference(target.arguments[*site.exitArgument]);
                continue;
            }
            size_t interiorIndex = site.interiorBlocks.find(target.block);
            RELEASE_ASSERT(interiorIndex != notFound);
            append(Mark::InteriorTarget, interiorIndex, target.arguments.size() - std::ranges::count_if(target.arguments, [&](ValueID argument) { return isFact(argument); }));
            for (ValueID argument : target.arguments) {
                if (!isFact(argument))
                    appendReference(argument);
            }
            append(Mark::Proof, target.proofs.size());
            words.append(target.proofs.span());
        }
    }

    // Says whether the value comes from outside the site.
    bool appendReference(ValueID id)
    {
        if (!id || isFact(id)) {
            append(Mark::Absent);
            return false;
        }
        const Value& value = function.at(id);
        if (isRematerializable(value)) {
            append(Mark::Constant, value.opcode, value.type.index());
            payloads.append(&value);
            return true;
        }
        size_t ordinal = definitions.find(id);
        if (ordinal != notFound) {
            append(Mark::InSite, ordinal);
            return false;
        }
        size_t liveInIndex = liveIns.find(id);
        if (liveInIndex == notFound) {
            liveInIndex = liveIns.size();
            if (liveInIndex <= maxLiveIns)
                liveIns.append(id);
        }
        append(Mark::LiveIn, liveInIndex, value.type.index());
        return true;
    }

    unsigned hash() const
    {
        Hasher hasher;
        for (uint32_t word : words)
            add(hasher, word);
        for (const Value* payload : payloads) {
            forEachPayloadWordThatBodiesShare(*payload, site, [&](uint32_t word) {
                add(hasher, word);
            });
        }
        return hasher.hash();
    }

    Vector<uint32_t, 128> payloadWords() const
    {
        Vector<uint32_t, 128> result;
        for (const Value* payload : payloads) {
            forEachPayloadWordThatBodiesShare(*payload, site, [&](uint32_t word) {
                result.append(word);
            });
        }
        return result;
    }

    const Program& program;
    const Function& function;
    const OutliningSite& site;
    uint64_t maxLiveIns;
    bool hasRequiredDefinitionOutside { false };
    Vector<ValueID, 24> definitions;
    Vector<ValueID, 6> liveIns;
    Vector<uint32_t, 128> words;
    Vector<const Value*, 24> payloads;
};

enum class BackwardExtension : uint8_t { ConstantsOnly, UsedValues, AllValues };
enum class SitesThatCannotPayOff : uint8_t { AreKeptToBeSplit, AreDropped };
enum class NamedGuards : uint8_t { None, AllDefinedInSite, SomeDefinedOutside };

struct SplittableSite {
    uint32_t site { 0 };
    uint32_t firstPart { 0 };
    uint32_t numberOfParts { 0 };
    bool isRepeated { false };
};

// The parts are what takes the place of a site that no other site in the program is equal to.
struct OutliningSites {
    Vector<OutliningSite> sites;
    Vector<OutliningSite> parts;
    Vector<SplittableSite> splittableSites;
};

} // anonymous namespace

static bool areEqualSites(const Program& program, const OutliningSite& first, const OutliningSite& second)
{
    if (first.hash != second.hash)
        return false;
    OutliningNormalForm firstForm(program, program.at(first.function), first, first.liveIns.size());
    OutliningNormalForm secondForm(program, program.at(second.function), second, second.liveIns.size());
    ASSERT(firstForm.liveIns == first.liveIns && secondForm.liveIns == second.liveIns);
    return firstForm.words == secondForm.words && firstForm.payloadWords() == secondForm.payloadWords();
}

// Every operation whose result the escape analysis may take for an object of its own.
static OpcodeSet findOpcodesThatMayDefineObjects()
{
    OpcodeSet opcodes = opcodesThatMayDefineObjects();
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool mayDefineObject = resultRuleOf(opcode) == ResultRule::Allocation || resultRuleOf(opcode) == ResultRule::Declared || OperandPosition::parent(opcode);
        switch (effectRuleOf(opcode)) {
        case EffectRule::Builtin:
        case EffectRule::Inline:
            mayDefineObject = true;
            break;
        case EffectRule::Declared:
        case EffectRule::Operator:
        case EffectRule::Access:
        case EffectRule::Call:
            break;
        }
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex)
            mayDefineObject |= operandUsesOf(opcode, operandIndex).contains(OperandUse::Retains) && retainedInOf(opcode, operandIndex) == RetainedIn::Result;
        if (mayDefineObject)
            opcodes.add(opcode);
    }
    return opcodes;
}

static OpcodeSet findOpcodesThatProve()
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (provesOf(opcode) != Proof::None)
            opcodes.add(opcode);
    }
    return opcodes;
}

struct OutliningSiteFinder::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    // True where the hash was added before. False says nothing: there may have been no room for it.
    bool addHashOfSplittableSite(unsigned hash)
    {
        if (!hash)
            return false;
        size_t mask = hashesOfSplittableSites.size() - 1;
        size_t index = WTF::intHash(hash) & mask;
        for (unsigned probe = 0; probe < longestProbe; ++probe, index = (index + 1) & mask) {
            AtomicRef<unsigned> slot(hashesOfSplittableSites[index]);
            unsigned found = slot.load(std::memory_order_relaxed);
            if (!found && slot.compare_exchange_strong(found, hash, std::memory_order_relaxed))
                return false;
            if (found == hash)
                return true;
        }
        return false;
    }

    static constexpr size_t slotsForFunction = 8;
    AOT_CLASS_THRESHOLD(unsigned, longestProbe, 16);

    const Program& program;
    const OptimizationInputs inputs;
    BuildContext& build;
    OpcodeSet opcodesThatMayDefineObjects = findOpcodesThatMayDefineObjects();
    OpcodeSet opcodesThatDefineObjectsThatNeedNotBeFresh = Typed::opcodesThatDefineObjectsThatNeedNotBeFresh();
    OpcodeSet opcodesThatProve = findOpcodesThatProve();
    ParallelArray<unsigned> hashesOfSplittableSites = ParallelArray<unsigned>(build.parallelLoops(), roundUpToPowerOfTwo(std::max<size_t>(slotsForFunction * program.functions.size(), 1 << 10)), [](unsigned) { return 0u; });
    ParallelArray<OutliningSites> functions = ParallelArray<OutliningSites>(build.parallelLoops(), program.functions.size(), [](unsigned) { return OutliningSites(); });
    Vector<uint8_t> hasSites = Vector<uint8_t>(FillWith { }, program.functions.size(), static_cast<uint8_t>(0));
};

namespace {

struct OutliningScan {
    bool hasGuards()
    {
        if (!guards) {
            guards = guardValuesOf(function, cfg);
            hasAnyGuard = !guards->isEmpty();
        }
        return hasAnyGuard;
    }

    bool isGuard(ValueID id) { return id && hasGuards() && guards->quickGet(id.index()); }

    bool isAnyGuardLiveAtHead(BlockID block)
    {
        if (!hasGuards())
            return false;
        if (!livenessOfGuards)
            livenessOfGuards.emplace(function, cfg, useLists, LivenessOptions { .trackedValues = &*guards });
        return !livenessOfGuards->liveAtHead(block).empty();
    }

    bool isAnyGuardLiveOnEdge(const Target& target)
    {
        return isAnyGuardLiveAtHead(target.block) || std::ranges::any_of(target.arguments, [&](ValueID argument) {
            return isGuard(argument);
        });
    }

    uint32_t findEndOfGuardUses(BlockID id)
    {
        const Block& block = function.at(id);
        bool isUsedAtEnd = std::ranges::any_of(cfg.successors(id), [&](BlockID successor) {
            return isAnyGuardLiveAtHead(successor);
        });
        block.forEachChildOutsideValues([&](ValueID child) {
            isUsedAtEnd |= isGuard(child);
        });

        uint32_t end = block.values.size();
        if (isUsedAtEnd)
            return end + 1;
        for (; end; --end) {
            bool usesGuard = false;
            function.at(block.values[end - 1]).forEachChild([&](ValueID operand) {
                usesGuard |= isGuard(operand);
            });
            if (usesGuard)
                break;
        }
        return end;
    }

    bool isAnyGuardLiveAt(ProgramPoint point)
    {
        if (!hasGuards())
            return false;
        std::optional<uint32_t>& end = endOfGuardUses[point.block.index()];
        if (!end)
            end = findEndOfGuardUses(point.block);
        return point.index < *end;
    }

    bool isFramePointer(ValueID id)
    {
        if (!mayHaveFrameStorage)
            return false;
        if (!framePointers)
            framePointers.emplace(function);
        return framePointers->contains(id);
    }

    bool exceedsRegionValues(uint64_t numberOfValues) { return numberOfValues > maxRegionValues && build.exceeds(LimitID::MaxOutlinedRegionValues, numberOfValues, subject); }

    // Whether the escape analysis takes the result for an object of its own. Nothing else is bound, but for what belongs to a JSONObjectFinish or to a CopyObject.
    bool mayDefineObject(ValueID id)
    {
        const Value& value = function.at(id);
        if (!finder.opcodesThatMayDefineObjects.contains(value.opcode))
            return false;
        if (finder.opcodesThatDefineObjectsThatNeedNotBeFresh.contains(value.opcode))
            return true;
        auto keptEffects = [&] {
            return analyses.effectsOf(id);
        };
        return freshnessOf(analyses.effectContext(), function, value, ScopedLambda<EffectsOnEdges()>(keptEffects)) != ResultFreshness::Unknown;
    }

    bool isBoundToFunction(ValueID value)
    {
        if (!mayBindAnyValue && !mayDefineObject(value))
            return false;
        if (!valuesBoundToFunction)
            findBoundValues();
        return valuesBoundToFunction->quickGet(value.index());
    }

    // An allocation that is made again on several ways arrives as the parameter of the block where they meet.
    void bindObjectAndWhatArrivesAsIt(ValueID object)
    {
        BitVector isVisited(function.values.size());
        Vector<ValueID, 4> worklist { object };
        while (!worklist.isEmpty()) {
            ValueID origin;
            for (ValueID link : CastChain(function, worklist.takeLast())) {
                valuesBoundToFunction->quickSet(link.index());
                origin = link;
            }
            if (isVisited.quickSet(origin.index()))
                continue;
            ValueLocation location = useLists.locationOf(origin);
            if (location.placement != ValuePlacement::Parameter || location.block == entryBlock)
                continue;
            for (Edge edge : cfg.incomingEdges(location.block)) {
                IncomingValue incoming = incomingValueOf(function, edge, location.index);
                if (incoming.kind == IncomingValue::Kind::Argument)
                    worklist.append(incoming.argument);
            }
        }
    }

    void findBoundValues()
    {
        BitVector& bound = valuesBoundToFunction.emplace(function.values.size());
        bool didBindEveryObject = false;
        auto bindObjectsOf = [&](const EscapeAnalysis& escape, ValueID pointer) {
            if (!escape.gaveUp()) {
                for (ObjectID object : escape.pointsTo(pointer).objects)
                    bound.quickSet(escape.at(object).definition.index());
                return;
            }
            // An analysis that gave up does not say which objects a value names, so it may name any of them.
            if (std::exchange(didBindEveryObject, true))
                return;
            for (BlockID block : cfg.reversePostOrder()) {
                for (ValueID id : function.at(block).values) {
                    if (mayDefineObject(id))
                        bound.quickSet(id.index());
                }
            }
        };
        // Other code cannot reach an object that was stored into another one while that one is seen to be allocated in this function and to stay in it.
        BitVector objectsWhoseHoldersAreBound;
        auto bindHoldersOf = [&](const EscapeAnalysis& escape, ValueID pointer) {
            if (escape.gaveUp())
                return;
            Vector<ObjectID, 8> worklist;
            auto visit = [&](ObjectID object) {
                if (!objectsWhoseHoldersAreBound.set(object.index()))
                    worklist.append(object);
            };
            for (ObjectID object : escape.pointsTo(pointer).objects)
                visit(object);
            while (!worklist.isEmpty()) {
                for (ObjectID holder : escape.containersOf(worklist.takeLast())) {
                    if (isOpenContainer(escape.at(holder)))
                        continue;
                    bound.quickSet(escape.at(holder).definition.index());
                    visit(holder);
                }
            }
        };

        // What another rule wants in the function where it stands.
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                if (value.opcode == Opcode::CopyObject) {
                    if (ValueID second = findCopyIntoCopy(function, function.at(block), id)) {
                        bound.quickSet(id.index());
                        bound.quickSet(second.index());
                    }
                    continue;
                }
                if (value.opcode == Opcode::JSONObjectFinish) {
                    bound.quickSet(id.index());
                    bindObjectAndWhatArrivesAsIt(value.objectOperand());
                    continue;
                }
                if (!provesForStretch(provesOf(value.opcode)) || !value.numberOfOperands() || !value.operand(subjectOperandIndex))
                    continue;
                ValueID tested = analyses.identityOf(value.operand(subjectOperandIndex));
                if (freshnessOf(analyses.effectContext(), function, function.at(tested)) != ResultFreshness::Unknown)
                    bound.quickSet(tested.index());
                // The subject may be one of several allocations that meet in a block parameter, or that were read out of another one.
                bindObjectsOf(analyses.escape(), tested);
                bindHoldersOf(analyses.escape(), tested);
            }
        }

        // A guard about one object lasts over a write to another that is seen to be allocated in this function. What a call returns may be any object, the guarded one among them.
        for (BlockID block : cfg.reversePostOrder()) {
            const auto& values = function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (!isAnyGuardLiveAt({ block, index + 1 }))
                    break;
                const EffectsOnEdges& effects = analyses.effectsOf(values[index]);
                if (effects.normal.writes.isEmpty() && effects.handler.writes.isEmpty())
                    continue;
                const EscapeAnalysis& escape = analyses.escape();
                function.at(values[index]).forEachOperand([&](ValueID operand, uint32_t) {
                    bindObjectsOf(escape, operand);
                });
            }
        }

        if (mayBindAnyValue)
            return;
        for (size_t index : bound)
            RELEASE_ASSERT(function.values[index].opcode == Opcode::Parameter || mayDefineObject(ValueID(index)));
    }

    // The objects that a test has made a guard about, each under the value that all its casts lead back to.
    bool isSubjectOfGuard(ValueID identity)
    {
        if (!subjectsOfGuards) {
            subjectsOfGuards.emplace(function.values.size());
            for (BlockID block : cfg.reversePostOrder()) {
                for (ValueID id : function.at(block).values) {
                    const Value& value = function.at(id);
                    if (provesOf(value.opcode) != Proof::None && value.numberOfOperands() && value.operand(subjectOperandIndex))
                        subjectsOfGuards->quickSet(analyses.identityOf(value.operand(subjectOperandIndex)).index());
                }
            }
        }
        return subjectsOfGuards->quickGet(identity.index());
    }

    bool computeIsMovable(ValueID id)
    {
        const Value& value = function.at(id);
        if (boundToOf(value.opcode) != BoundTo::Nothing || value.type == TypeTable::never)
            return false;
        // Only the body of a coroutine may hold it.
        if (value.opcode == Opcode::TakeGeneratorPrototype)
            return false;
        // The function object that it calls is in this frame.
        if (value.opcode == Opcode::CallDirect && value.calleeIsKeptInFrame())
            return false;
        const Terminator& terminator = function.at(useLists.blockOf(id)).terminator;
        if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id)
            return false;
        // A call in tail position that returns here has one frame more than the program wrote. A body would add another.
        if (value.isTailCall && callOpcodes.contains(value.opcode))
            return false;
        // Reflect.construct(..) is shown at one place where it constructs and at another where it calls. The call of a body does neither.
        // The read of an update is shown at one place where it throws and at another where it calls. The call of a body always calls.
        if (value.isPartOfConstruction) {
            const ConstructionStart* construction = program.findConstructionReportedAt(program.sourceFunctionOf(function, value).module, value.position);
            if (construction && (construction->isWrittenAsCall || construction->isForCallers))
                return false;
        }
        bool dependsOnInlineFrame = std::ranges::any_of(payloadFieldsOf(value.opcode), [&](const PayloadFieldInfo& field) {
            return field.kind == PayloadKind::Origin && value.payloadWordAt(field.firstWord + 1);
        });
        if (dependsOnInlineFrame)
            return false;
        if (builtinCallOpcodes.contains(value.opcode)) {
            const auto& builtin = program.builtins().at(value.overload());
            if (builtin.code.implementation.inlineBuiltin == InlineBuiltin::NewError) {
                const Function& source = program.sourceFunctionOf(function, value);
                if (source.isHiddenInStackTraces() && source.enclosingRuntimeFunction)
                    return false;
            }
            // Flow's candidates for such an operand are found by the access site of the call, and equal operations of several sites share one outlined body.
            bool hasOperandKeyedByAccessSite = std::ranges::any_of(builtin.parameters, [](const BuiltinParameter& parameter) {
                return parameter.roles.contains(BuiltinOperandRole::ContentKey);
            });
            if (hasOperandKeyedByAccessSite)
                return false;
        }
        if (hasFrameStorage(value) || isBoundToFunction(id))
            return false;
        return !maySuspend(value.opcode) || !analyses.maySuspend(id);
    }

    bool isMovable(ValueID id)
    {
        std::optional<bool>& cached = cachedIsMovable[id.index()];
        if (!cached)
            cached = computeIsMovable(id);
        return *cached;
    }

    bool isUseInSite(const OutliningSite& site, const Use& use)
    {
        bool isInEntryBlock = use.block == site.entryBlock;
        switch (use.kind) {
        case Use::Kind::Operand: {
            if (!isInEntryBlock)
                return site.interiorBlocks.contains(use.block);
            uint32_t index = useLists.locationOf(use.user).index;
            return index >= site.firstIndex && index - site.firstIndex < site.numberOfEntryValues;
        }
        case Use::Kind::TerminatorOperand:
        case Use::Kind::EdgeArgument:
            return isInEntryBlock ? site.isRegion() : site.interiorBlocks.contains(use.block);
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool hasOnlyUsesInSite(const OutliningSite& site, ValueID id)
    {
        return std::ranges::all_of(useLists.usesOf(id), [&](const Use& use) {
            return isUseInSite(site, use);
        });
    }

    bool isDefinedInSite(const OutliningSite& site, ValueID id)
    {
        ValueLocation location = useLists.locationOf(id);
        if (location.block != site.entryBlock)
            return site.interiorBlocks.contains(location.block);
        return location.placement == ValuePlacement::InBlock && location.index >= site.firstIndex && location.index - site.firstIndex < site.numberOfEntryValues;
    }

    NamedGuards findNamedGuards(const OutliningSite& site)
    {
        NamedGuards result = NamedGuards::None;
        auto namesGuardDefinedOutside = [&](ValueID id) {
            const Value& value = function.at(id);
            for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
                ValueID operand = value.operand(operandIndex);
                if (!operand || value.operandKind(operandIndex) != OperandKind::Guard)
                    continue;
                if (!isDefinedInSite(site, operand))
                    return true;
                result = NamedGuards::AllDefinedInSite;
            }
            return false;
        };
        bool namesOneDefinedOutside = std::ranges::any_of(entryValuesOf(function, site), namesGuardDefinedOutside) || std::ranges::any_of(site.interiorBlocks, [&](BlockID block) {
            return std::ranges::any_of(function.at(block).values, namesGuardDefinedOutside);
        });
        return namesOneDefinedOutside ? NamedGuards::SomeDefinedOutside : result;
    }

    bool alwaysRunsUnknownCode(const OutliningSite& site)
    {
        auto anyRunsUnknownCode = [&](std::span<const ValueID> values) {
            return std::ranges::any_of(values, [&](ValueID id) {
                const EffectsOnEdges& effects = analyses.effectsOf(id);
                return effects.normal.mayCallUnknownCode && effects.handler.mayCallUnknownCode;
            });
        };
        if (anyRunsUnknownCode(entryValuesOf(function, site)))
            return true;
        if (!site.isRegion())
            return false;

        BitVector isReachedWithoutUnknownCode(site.interiorBlocks.size());
        auto reachesExit = [&](const Terminator& terminator) {
            for (const Target& target : terminator.targets) {
                if (target.block == site.exitBlock)
                    return true;
                size_t interiorIndex = site.interiorBlocks.find(target.block);
                RELEASE_ASSERT(interiorIndex != notFound);
                isReachedWithoutUnknownCode.quickSet(interiorIndex);
            }
            return false;
        };

        if (reachesExit(function.at(site.entryBlock).terminator))
            return false;
        for (size_t index = 0; index < site.interiorBlocks.size(); ++index) {
            const Block& block = function.at(site.interiorBlocks[index]);
            if (!isReachedWithoutUnknownCode.quickGet(index) || anyRunsUnknownCode(block.values.span()))
                continue;
            if (reachesExit(block.terminator))
                return false;
        }
        return true;
    }

    bool isAnyGuardLiveAfter(const OutliningSite& site)
    {
        if (!site.isRegion())
            return isAnyGuardLiveAt({ site.entryBlock, site.firstIndex + site.numberOfEntryValues });

        const std::optional<Target>& handler = function.at(site.entryBlock).handler;
        if (handler && isAnyGuardLiveOnEdge(*handler))
            return true;

        auto exitsWithLiveGuard = [&](BlockID block) {
            return std::ranges::any_of(function.at(block).terminator.targets, [&](const Target& target) {
                return target.block == site.exitBlock && isAnyGuardLiveOnEdge(target);
            });
        };
        return exitsWithLiveGuard(site.entryBlock) || std::ranges::any_of(site.interiorBlocks, exitsWithLiveGuard);
    }

    // Analyses may treat the call as unknown code, and a local allocation passed to it is a parameter in the body, so it no longer counts as unescaped.
    bool mayEndGuardInUse(const OutliningSite& site, std::span<const ValueID> liveIns, NamedGuards namedGuards)
    {
        bool crossesLiveGuard = isAnyGuardLiveAfter(site);
        if (crossesLiveGuard && !alwaysRunsUnknownCode(site))
            return true;
        if (!crossesLiveGuard && namedGuards == NamedGuards::None)
            return false;
        return std::ranges::any_of(liveIns, [&](ValueID liveIn) {
            const EscapeAnalysis& escape = analyses.escape();
            if (escape.gaveUp())
                return mayBeReference(program.typeOf(function, liveIn));
            for (ObjectID object : escape.pointsTo(liveIn).objects) {
                if (escape.at(object).kind == ObjectKind::Allocation)
                    return true;
            }
            return false;
        });
    }

    bool canBeInterior(const OutliningSite& region, BlockID blockID)
    {
        if (!coldBlocks.quickGet(blockID.index()) || blocksInSites.quickGet(blockID.index()))
            return false;
        for (Edge edge : cfg.incomingEdges(blockID)) {
            if (edge.from != region.entryBlock && !region.interiorBlocks.contains(edge.from))
                return false;
        }
        const Block& block = function.at(blockID);
        if (block.isHandlerBlock() || block.handler != function.at(region.entryBlock).handler)
            return false;
        return canEndInteriorBlockOf(block.terminator.kind);
    }

    bool addPendingTargets(const OutliningSite& region, const Terminator& terminator, Vector<uint32_t, 8>& pending)
    {
        for (const Target& target : terminator.targets) {
            uint32_t order = cfg.reversePostOrderIndex(target.block);
            std::span<const uint32_t> ordered = pending.span();
            size_t position = std::ranges::lower_bound(ordered, order) - ordered.begin();
            if (position < pending.size() && pending[position] == order)
                continue;
            pending.insert(position, order);
            if (exceedsRegionValues(region.interiorBlocks.size() + pending.size() - 1))
                return false;
        }
        return true;
    }

    // In a stack trace the call of the body stands for every operation of the site. It lies in one inline frame and has one position.
    Origin originOfBodyCall(const OutliningSite& site)
    {
        return site.root ? function.at(site.root).origin() : function.at(site.entryBlock).terminator.origin();
    }

    bool isShownAt(OutliningSite& site, ValueID id, Origin origin)
    {
        const Value& value = function.at(id);
        if (value.inlineFrame != origin.inlineFrame)
            return false;
        if (!analyses.mayThrow(id) && !analyses.mayCallUnknownCode(id))
            return true;
        if (!site.isTailCall) {
            site.isTailCall = site.root ? origin.isTailCall : !!value.isTailCall;
            site.isPartOfConstruction = site.root ? origin.isPartOfConstruction : !!value.isPartOfConstruction;
        }
        return value.position == origin.position && value.isTailCall == *site.isTailCall && value.isPartOfConstruction == *site.isPartOfConstruction;
    }

    std::optional<BuiltinID> findCalledBuiltin(const OutliningSite& site)
    {
        std::optional<BuiltinID> found;
        bool allAgree = true;
        forEachValueListAndTerminator(function, site, [&](std::span<const ValueID> values) {
            for (ValueID id : values) {
                if (!analyses.mayThrow(id) && !analyses.mayCallUnknownCode(id))
                    continue;
                const Value& value = function.at(id);
                BuiltinID builtin = Value::opcodesWithCalledBuiltin.contains(value.opcode) ? value.calledBuiltin() : BuiltinID();
                allAgree &= !found || *found == builtin;
                found = builtin;
            }
        }, [](const Terminator&) { });
        if (!allAgree)
            return std::nullopt;
        return found.value_or(BuiltinID());
    }

    bool findRegionBlocks(OutliningSite& region, uint32_t& numberOfValues)
    {
        Origin origin = originOfBodyCall(region);
        Vector<uint32_t, 8> pending;
        if (!addPendingTargets(region, function.at(region.entryBlock).terminator, pending))
            return false;

        while (pending.size() > 1) {
            BlockID blockID = cfg.reversePostOrder()[pending.first()];
            pending.removeAt(0);
            if (!canBeInterior(region, blockID))
                return false;

            const Block& block = function.at(blockID);
            numberOfValues += block.values.size();
            if (exceedsRegionValues(numberOfValues))
                return false;
            if (block.terminator.inlineFrame != origin.inlineFrame)
                return false;
            for (ValueID id : block.values) {
                if (!isRematerializable(function.at(id)) && (!isMovable(id) || !isShownAt(region, id, origin)))
                    return false;
            }

            region.interiorBlocks.append(blockID);
            if (!addPendingTargets(region, block.terminator, pending))
                return false;
        }

        if (pending.isEmpty() || region.interiorBlocks.isEmpty())
            return false;
        region.exitBlock = cfg.reversePostOrder()[pending.first()];
        return true;
    }

    uint32_t extendBackward(OutliningSite& site, uint32_t lowestIndex, BackwardExtension extension, uint32_t numberOfValues)
    {
        const Block& block = function.at(site.entryBlock);
        Origin origin = originOfBodyCall(site);
        uint32_t numberOfOperations = 0;
        while (site.firstIndex > lowestIndex) {
            ValueID id = block.values[site.firstIndex - 1];
            bool isConstant = isRematerializable(function.at(id));
            if (valuesInSites.quickGet(id.index()))
                break;
            if (!isConstant && (extension == BackwardExtension::ConstantsOnly || !isMovable(id) || !isShownAt(site, id, origin)))
                break;
            if (extension == BackwardExtension::UsedValues && !useLists.useCount(id))
                break;
            if (!hasOnlyUsesInSite(site, id))
                break;
            if (exceedsRegionValues(++numberOfValues))
                break;
            --site.firstIndex;
            ++site.numberOfEntryValues;
            numberOfOperations += !isConstant;
        }
        return numberOfOperations;
    }

    bool isComputedByRegion(const OutliningSite& region, std::span<const Arguments* const> exitEdges, uint32_t position)
    {
        ValueID first = (*exitEdges.front())[position];
        bool allEdgesPassFirst = std::ranges::all_of(exitEdges, [&](const Arguments* arguments) {
            return (*arguments)[position] == first;
        });
        if (allEdgesPassFirst && !isDefinedInSite(region, first))
            return false;

        const Value& firstValue = function.at(first);
        if (!isRematerializable(firstValue))
            return true;
        return !std::ranges::all_of(exitEdges, [&](const Arguments* arguments) {
            const Value& value = function.at((*arguments)[position]);
            return value.opcode == firstValue.opcode && value.hasSamePayloadAs(firstValue);
        });
    }

    bool findExitArgument(OutliningSite& region)
    {
        Vector<const Arguments*, 4> exitEdges;
        auto collectExitEdges = [&](const Terminator& terminator) {
            for (const Target& target : terminator.targets) {
                if (target.block != region.exitBlock)
                    continue;
                if (!target.proofs.isEmpty())
                    return false;
                exitEdges.append(&target.arguments);
            }
            return true;
        };
        auto hasOnlyUsesInRegion = [&](ValueID id) {
            return hasOnlyUsesInSite(region, id);
        };

        if (!collectExitEdges(function.at(region.entryBlock).terminator))
            return false;
        for (BlockID blockID : region.interiorBlocks) {
            const Block& block = function.at(blockID);
            if (!std::ranges::all_of(block.parameters, hasOnlyUsesInRegion) || !std::ranges::all_of(block.values, hasOnlyUsesInRegion) || !collectExitEdges(block.terminator))
                return false;
        }

        uint32_t numberOfPositions = function.at(region.exitBlock).parameters.size();
        for (uint32_t position = 0; position < numberOfPositions; ++position) {
            if (!isComputedByRegion(region, exitEdges.span(), position))
                continue;
            if (region.exitArgument)
                return false;
            region.exitArgument = position;
        }
        return true;
    }

    uint32_t codeBytesOf(ValueID id)
    {
        std::optional<uint32_t>& cached = cachedCodeBytes[id.index()];
        if (!cached)
            cached = estimatedCodeBytes(program, function, function.at(id));
        return *cached;
    }

    uint32_t computeCodeBytes(const OutliningSite& site)
    {
        uint32_t bytes = 0;
        forEachValueListAndTerminator(function, site, [&](std::span<const ValueID> values) {
            for (ValueID id : values) {
                if (!isRematerializable(function.at(id)))
                    bytes += codeBytesOf(id);
            }
        }, [&](const Terminator& terminator) {
            bytes += estimatedCodeBytes(terminator);
        });
        return bytes;
    }

    // In cold code an access by key or by name, a comparison, a test for truth and a call of a value are the moves of their operands and one call of a stub: what a call of a function is. As a function of its own it gets a frame and two more jumps each time it runs.
    bool isOneCallOfStub(const OutliningSite& site)
    {
        static constexpr OpcodeSet callsOfOneStub { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemDelete, Opcode::PropGet, Opcode::PropSet, Opcode::StrictEq, Opcode::ToBoolean };
        if (site.isRegion())
            return false;
        auto passesConstantThatTakesCode = [&](const Value& call) {
            bool result = false;
            call.forEachOperand([&](ValueID operand, uint32_t) {
                result |= isRematerializable(function.at(operand)) && codeBytesOf(operand);
            });
            return result;
        };
        unsigned numberOfOperations = 0;
        for (ValueID id : entryValuesOf(function, site)) {
            const Value& value = function.at(id);
            if (isRematerializable(value) || !codeBytesOf(id))
                continue;
            bool isCallOfValue = accessClassOf(value.opcode) == AccessClass::IndirectCall && !passesConstantThatTakesCode(value);
            if (!(isCallOfValue || callsOfOneStub.contains(value.opcode)) || ++numberOfOperations > 1)
                return false;
        }
        return numberOfOperations;
    }

    // A parameter is a value of its own too. Two names of one object that both go in are two objects inside, and a guard that a test there makes about the one does not speak of the other.
    bool passesOneObjectUnderTwoNames(std::span<const ValueID> liveIns)
    {
        for (size_t index = 0; index < liveIns.size(); ++index) {
            ValueID identity = analyses.identityOf(liveIns[index]);
            bool isPassedBefore = std::ranges::any_of(liveIns.first(index), [&](ValueID earlier) {
                return analyses.identityOf(earlier) == identity;
            });
            if (isPassedBefore)
                return true;
        }
        return false;
    }

    bool canBeSplit(const OutliningSite& site)
    {
        if (site.isRegion())
            return true;
        if (site.numberOfEntryValues < 2)
            return false;
        return std::ranges::count_if(entryValuesOf(function, site), [&](ValueID id) {
            return !isRematerializable(function.at(id));
        }) > 1;
    }

    bool completeSite(OutliningSite& site, SitesThatCannotPayOff unprofitableSites)
    {
        site.resultType = TypeTable::undefined;
        ValueID result;
        if (site.rootIsUsed)
            result = site.root;
        else if (site.exitArgument)
            result = function.at(site.exitBlock).parameters[*site.exitArgument];
        if (result)
            site.resultType = function.at(result).type;
        if (!program.at(site.resultType).isJavaScriptValue() || program.at(site.resultType).mayBeUnspecified())
            return false;
        NamedGuards namedGuards = findNamedGuards(site);
        if (namedGuards == NamedGuards::SomeDefinedOutside)
            return false;
        // What a call returns is a value of its own.
        if (result && hasOperationThatProves) {
            ValueID identity = analyses.identityOf(result);
            if (identity != result && isSubjectOfGuard(identity) && !isDefinedInSite(site, identity))
                return false;
        }

        site.calledBuiltin = findCalledBuiltin(site);
        OutliningNormalForm form(program, function, site, build.limit(LimitID::MaxOutlinedOperands));
        if (form.hasRequiredDefinitionOutside)
            return false;
        // No use of an Unspecified value is reached, and an operand or the result of the call of the shared function would be one.
        for (ValueID liveIn : form.liveIns) {
            const Value& value = function.at(liveIn);
            if (!program.at(value.type).isJavaScriptValue() || isFramePointer(liveIn) || branchesOf(value.opcode) == Branches::Only || isDeferredClosure(function, liveIn) || program.at(value.type).mayBeUnspecified())
                return false;
        }
        if (mayEndGuardInUse(site, form.liveIns.span(), namedGuards))
            return false;
        if (namedGuards != NamedGuards::None && passesOneObjectUnderTwoNames(form.liveIns.span()))
            return false;
        if (build.exceeds(LimitID::MaxOutlinedOperands, form.liveIns.size(), subject))
            return false;

        site.liveIns = form.liveIns;
        site.codeBytes = computeCodeBytes(site);
        site.callBytes = estimatedDirectCallBytes(site.liveIns.size());
        // However many equal sites there are, each of them gets no shorter.
        site.canPayOff = site.codeBytes > site.callBytes && !isOneCallOfStub(site);
        if (site.canPayOff || (unprofitableSites == SitesThatCannotPayOff::AreKeptToBeSplit && canBeSplit(site)))
            site.hash = form.hash();
        return true;
    }

    void markSite(const OutliningSite& site)
    {
        for (ValueID id : entryValuesOf(function, site))
            valuesInSites.quickSet(id.index());
        if (!site.isRegion())
            return;
        blocksInSites.quickSet(site.entryBlock.index());
        for (BlockID blockID : site.interiorBlocks) {
            blocksInSites.quickSet(blockID.index());
            for (ValueID id : function.at(blockID).values)
                valuesInSites.quickSet(id.index());
        }
    }

    std::optional<OutliningSite> tryCreateRegion(BlockID blockID)
    {
        const Block& block = function.at(blockID);
        if (blocksInSites.quickGet(blockID.index()) || !canStartRegionOf(block.terminator.kind))
            return std::nullopt;

        OutliningSite region { .function = id, .entryBlock = blockID, .firstIndex = static_cast<uint32_t>(block.values.size()), .orderOfEntryBlock = cfg.reversePostOrderIndex(blockID) };
        uint32_t numberOfValues = 0;
        if (!findRegionBlocks(region, numberOfValues))
            return std::nullopt;
        extendBackward(region, 0, BackwardExtension::UsedValues, numberOfValues);
        if (!findExitArgument(region) || !completeSite(region, SitesThatCannotPayOff::AreKeptToBeSplit))
            return std::nullopt;
        return region;
    }

    bool canUseCallResult(const Use& use)
    {
        auto acceptsAnyDefinition = [](OperandKind kind) {
            return !mustBeDefinition(kind) && kind != OperandKind::Guard;
        };
        switch (use.kind) {
        case Use::Kind::Operand:
            return acceptsAnyDefinition(function.at(use.user).operandKind(use.operandIndex));
        case Use::Kind::TerminatorOperand: {
            const Terminator& terminator = function.at(use.block).terminator;
            return terminator.kind != TerminatorKind::TestAndBranch && acceptsAnyDefinition(terminator.operandKind(use.operandIndex));
        }
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
            return true;
        case Use::Kind::BlockSubject:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool canBeRoot(ValueID id)
    {
        const Value& value = function.at(id);
        if (!isMovable(id) || branchesOf(value.opcode) == Branches::Only || familyOf(value.opcode) == OperationFamily::GuardValue)
            return false;
        if (isConditionOfBranchWithProofs(function, useLists, id))
            return false;
        // A deferred closure has no code and no value: it moves only together with the store that names it.
        if (isDeferredClosure(function, id))
            return false;

        std::span<const Use> uses = useLists.usesOf(id);
        if (!uses.empty() && numberOfResultsOf(program, function, value))
            return false;
        return std::ranges::all_of(uses, [&](const Use& use) {
            return canUseCallResult(use);
        });
    }

    OutliningSite createSingleValueRun(BlockID block, uint32_t rootIndex)
    {
        ValueID root = function.at(block).values[rootIndex];
        return { .function = id, .entryBlock = block, .firstIndex = rootIndex, .numberOfEntryValues = 1, .root = root, .rootIsUsed = !!useLists.useCount(root), .orderOfEntryBlock = cfg.reversePostOrderIndex(block) };
    }

    bool isOneOperationThatCannotPayOff(const OutliningSite& run)
    {
        Vector<ValueID, 6> liveIns;
        if (!isRematerializable(function.at(run.root))) {
            function.at(run.root).forEachOperand([&](ValueID operand, uint32_t) {
                if (!isRematerializable(function.at(operand)) && !liveIns.contains(operand))
                    liveIns.append(operand);
            });
        }
        if (liveIns.size() > build.limit(LimitID::MaxOutlinedOperands))
            return false;
        return computeCodeBytes(run) <= estimatedDirectCallBytes(liveIns.size()) || isOneCallOfStub(run);
    }

    std::optional<OutliningSite> tryCreateRun(BlockID block, uint32_t lowestIndex, uint32_t rootIndex, BackwardExtension extension, SitesThatCannotPayOff unprofitableSites)
    {
        OutliningSite run = createSingleValueRun(block, rootIndex);
        uint32_t numberOfOperationsAdded = extendBackward(run, lowestIndex, extension, 1);
        if (!numberOfOperationsAdded && isOneOperationThatCannotPayOff(run))
            return run;
        if (completeSite(run, unprofitableSites))
            return run;
        if (!numberOfOperationsAdded)
            return std::nullopt;

        run = createSingleValueRun(block, rootIndex);
        extendBackward(run, lowestIndex, BackwardExtension::ConstantsOnly, 1);
        if (completeSite(run, unprofitableSites))
            return run;
        return std::nullopt;
    }

    void findRuns(BlockID block, uint32_t begin, uint32_t end, BackwardExtension extension, SitesThatCannotPayOff unprofitableSites, Vector<OutliningSite>& sites)
    {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = end; index-- > begin;) {
            ValueID root = values[index];
            if (valuesInSites.quickGet(root.index()) || !canBeRoot(root))
                continue;
            std::optional<OutliningSite> run = tryCreateRun(block, begin, index, extension, unprofitableSites);
            if (!run)
                continue;
            markSite(*run);
            index = run->firstIndex;
            if (run->canPayOff || (unprofitableSites == SitesThatCannotPayOff::AreKeptToBeSplit && canBeSplit(*run)))
                sites.append(WTF::move(*run));
        }
    }

    OutliningSites findSites()
    {
        OutliningSites found;
        for (BlockID block : cfg.reversePostOrder()) {
            if (!coldBlocks.quickGet(block.index()))
                continue;
            size_t firstSiteOfBlock = found.sites.size();
            if (std::optional<OutliningSite> region = tryCreateRegion(block)) {
                markSite(*region);
                found.sites.append(WTF::move(*region));
            }
            findRuns(block, 0, function.at(block).values.size(), BackwardExtension::AllValues, SitesThatCannotPayOff::AreKeptToBeSplit, found.sites);
            std::ranges::reverse(found.sites.mutableSubspan(firstSiteOfBlock));
        }
        if (found.sites.isEmpty())
            return found;

        // Whether a site is repeated is known when all functions have been looked at.
        valuesInSites.clearAll();
        for (uint32_t index = 0; index < found.sites.size(); ++index) {
            const OutliningSite& site = found.sites[index];
            if (!canBeSplit(site))
                continue;
            uint32_t firstPart = found.parts.size();
            if (!finder.addHashOfSplittableSite(site.hash)) {
                findRuns(site.entryBlock, site.firstIndex, site.firstIndex + site.numberOfEntryValues, BackwardExtension::ConstantsOnly, SitesThatCannotPayOff::AreDropped, found.parts);
                for (BlockID block : site.interiorBlocks)
                    findRuns(block, 0, function.at(block).values.size(), BackwardExtension::ConstantsOnly, SitesThatCannotPayOff::AreDropped, found.parts);
            }
            found.splittableSites.append(SplittableSite { index, firstPart, static_cast<uint32_t>(found.parts.size() - firstPart), false });
        }
        return found;
    }

    OutliningSiteFinder::State& finder;
    FunctionID id;
    FunctionAnalyses& analyses;
    const BitVector& coldBlocks;
    const Program& program { finder.program };
    BuildContext& build { finder.build };
    const Function& function { analyses.function() };
    const CFG& cfg { analyses.cfg() };
    const UseLists& useLists { analyses.useLists() };
    Subject subject = analyses.subject();
    // Found when somebody asks.
    std::optional<FramePointers> framePointers { };
    std::optional<BitVector> guards { };
    bool hasAnyGuard { false };
    std::optional<Liveness> livenessOfGuards { };
    Vector<std::optional<uint32_t>> endOfGuardUses = Vector<std::optional<uint32_t>>(function.blocks.size());
    std::optional<BitVector> valuesBoundToFunction { };
    std::optional<BitVector> subjectsOfGuards { };
    Vector<std::optional<bool>> cachedIsMovable = Vector<std::optional<bool>>(function.values.size());
    Vector<std::optional<uint32_t>> cachedCodeBytes = Vector<std::optional<uint32_t>>(function.values.size());
    BitVector valuesInSites = BitVector(function.values.size());
    BitVector blocksInSites = BitVector(function.blocks.size());
    uint64_t maxRegionValues = build.limit(LimitID::MaxOutlinedRegionValues);
    bool mayHaveFrameStorage = analyses.opcodesOfValues().intersects(Value::opcodesWithStorage);
    bool mayBindAnyValue = analyses.opcodesOfValues().intersects(OpcodeSet { Opcode::JSONObjectFinish, Opcode::CopyObject });
    bool hasOperationThatProves = analyses.opcodesOfValues().intersects(finder.opcodesThatProve);
};

} // anonymous namespace

static bool usesExecutionCounts(HeatOverride heat)
{
    switch (heat) {
    case HeatOverride::EverythingIsHot:
    case HeatOverride::EverythingIsCold:
    case HeatOverride::Shuffled:
        return false;
    case HeatOverride::None:
    case HeatOverride::OnlyLoopsAreHot:
    case HeatOverride::NothingIsHottest:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BitVector findColdBlocks(const OutliningSiteFinder::State& finder, FunctionAnalyses& analyses)
{
    const AnalysisContext& context = analyses.context();
    const Heat& heat = finder.inputs.heat;
    bool isColdOutsideCycles = (usesExecutionCounts(finder.build.recordedOptions().heat()) && finder.inputs.flow.runsAtMostOnce(context.program, context.id))
        || (isCold(heat.classOf(heat.activationsOf(context.program, context.id) * roundingOfFrequencySums)).predicted() && runsNoBlockMoreOftenThanEntry(analyses.cfg()));
    BitVector coldBlocks(analyses.function().blocks.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (!analyses.isInCycle(block) && (isColdOutsideCycles || isCold(heatClassOf(heat, analyses, block)).predicted()))
            coldBlocks.quickSet(block.index());
    }
    return coldBlocks;
}

OutliningSiteFinder::OutliningSiteFinder(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
    : m_state(makeUnique<State>(program, inputs, build))
{
}

OutliningSiteFinder::~OutliningSiteFinder() = default;

AnalysisContext OutliningSiteFinder::contextOf(FunctionID id) const
{
    return { m_state->build, m_state->program, &m_state->inputs.flow, &m_state->inputs.effects, id };
}

bool OutliningSiteFinder::findIn(FunctionID id)
{
    FunctionAnalyses analyses(contextOf(id), m_state->program.at(id));
    return findIn(id, analyses);
}

bool OutliningSiteFinder::findIn(FunctionID id, FunctionAnalyses& analyses)
{
    if (!analyses.function().hasBody() || !m_state->build.isEnabled(PassID::OutlineColdOperations, id))
        return false;
    BitVector coldBlocks = findColdBlocks(*m_state, analyses);
    if (coldBlocks.isEmpty())
        return false;

    OutliningSites& found = m_state->functions[id.index()];
    found = OutliningScan { *m_state, id, analyses, coldBlocks }.findSites();
    m_state->hasSites[id.index()] = !found.sites.isEmpty();
    return !found.sites.isEmpty();
}

std::span<const uint8_t> OutliningSiteFinder::hasSites() const
{
    return m_state->hasSites.span();
}

ParallelArray<OutliningSite> OutliningSiteFinder::takeSites()
{
    struct SplittableSiteReference {
        unsigned hash { 0 };
        uint32_t function { 0 };
        uint32_t splittableSite { 0 };
    };
    ParallelArray<OutliningSites>& functions = m_state->functions;
    const ParallelLoops& loops = m_state->build.parallelLoops();

    ParallelArray<SplittableSiteReference> candidates = collectIntoArrayInParallel<SplittableSiteReference>(loops, functions.size(), { }, [&](unsigned index, Vector<SplittableSiteReference>& found) {
        const OutliningSites& function = functions[index];
        for (uint32_t position = 0; position < function.splittableSites.size(); ++position)
            found.append(SplittableSiteReference { function.sites[function.splittableSites[position].site].hash, index, position });
    });
    parallelSort(loops, candidates.mutableSpan(), [](const SplittableSiteReference& a, const SplittableSiteReference& b) {
        return a.hash < b.hash;
    });
    forEachIndexInParallel(loops, candidates.size(), [&](unsigned index) {
        const SplittableSiteReference& candidate = candidates[index];
        bool equalsPrevious = index && candidates[index - 1].hash == candidate.hash;
        bool equalsNext = index + 1 < candidates.size() && candidates[index + 1].hash == candidate.hash;
        functions[candidate.function].splittableSites[candidate.splittableSite].isRepeated = equalsPrevious || equalsNext;
    });

    ParallelArray<OutliningSite*> chosenSites = collectIntoArrayInParallel<OutliningSite*>(loops, functions.size(), { }, [&](unsigned functionIndex, Vector<OutliningSite*>& result) {
        OutliningSites& function = functions[functionIndex];
        size_t firstSiteOfFunction = result.size();
        bool didSplit = false;
        std::span<const SplittableSite> splittableSites = function.splittableSites.span();
        for (uint32_t index = 0; index < function.sites.size(); ++index) {
            bool isSplit = false;
            if (!splittableSites.empty() && splittableSites.front().site == index) {
                const SplittableSite& splittable = splittableSites.front();
                isSplit = !splittable.isRepeated;
                if (isSplit) {
                    for (OutliningSite& part : function.parts.mutableSubspan(splittable.firstPart, splittable.numberOfParts))
                        result.append(&part);
                }
                splittableSites = splittableSites.subspan(1);
            }
            didSplit |= isSplit;
            if (!isSplit && function.sites[index].canPayOff)
                result.append(&function.sites[index]);
        }
        if (!didSplit)
            return;
        AOT::sort(result.mutableSubspan(firstSiteOfFunction), { }, [](const OutliningSite* site) {
            return std::pair { site->orderOfEntryBlock, site->firstIndex };
        });
    });
    ParallelArray<OutliningSite> sites(loops, chosenSites.size(), [&](unsigned index) {
        return WTF::move(*chosenSites[index]);
    });
    m_state->build.destroyLater(WTF::move(functions));
    return sites;
}

static ParallelArray<OutliningSite> findOutliningSites(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
{
    OutliningSiteFinder finder(program, inputs, build);
    const ParallelLoops& loops = build.parallelLoops();
    std::optional<TypeInterningPhase> scratch;
    if (!program.types.hasOpenPhase())
        scratch.emplace(program.types, 0u, TypeInterningPhase::Scratch);
    {
        ProgramCaches caches(program, build);
        Vector<uint32_t> largestFirst = largeFunctions(program, loops);
        forEachIndexInParallel(loops, program.functions.size(), largestFirst.span(), [&](unsigned index) {
            finder.findIn(FunctionID(index));
        });
    }
    if (scratch)
        scratch->discard();
    return finder.takeSites();
}

using SiteGroup = Vector<uint32_t, 2>;

static bool savesBytes(const OutliningSite& firstSite, uint64_t numberOfSites)
{
    uint64_t inlineBytes = firstSite.codeBytes;
    uint64_t callBytes = firstSite.callBytes;
    return numberOfSites * inlineBytes > numberOfSites * callBytes + inlineBytes + outlinedBodyOverheadBytes;
}

static Vector<SiteGroup> findProfitableGroups(const Program& program, const ParallelLoops& loops, const ParallelArray<OutliningSite>& sites)
{
    // The hash in the upper half, the site in the lower.
    Vector<uint64_t> sortKeys(sites.size());
    forEachIndexInParallel(loops, sites.size(), [&](unsigned site) {
        sortKeys[site] = static_cast<uint64_t>(sites[site].hash) << 32 | site;
    });
    parallelSort(loops, sortKeys.mutableSpan(), std::less<uint64_t> { });
    Vector<uint32_t> hashRunStarts = findIndicesInParallel(loops, sortKeys.size(), [&](unsigned position) {
        return !position || sortKeys[position - 1] >> 32 != sortKeys[position] >> 32;
    });

    auto keysOfRun = [&](unsigned run) {
        size_t begin = hashRunStarts[run];
        size_t end = run + 1 < hashRunStarts.size() ? hashRunStarts[run + 1] : sortKeys.size();
        return sortKeys.subspan(begin, end - begin);
    };
    Vector<uint32_t> longestRunsFirst = findIndicesInParallel(loops, hashRunStarts.size(), [&](unsigned run) {
        return keysOfRun(run).size() >= sitesInLongHashRun;
    });
    AOT::sort(longestRunsFirst, [&](uint32_t a, uint32_t b) {
        return std::pair { keysOfRun(b).size(), a } < std::pair { keysOfRun(a).size(), b };
    });

    Vector<SiteGroup> groups = collectInParallel<SiteGroup>(loops, hashRunStarts.size(), longestRunsFirst.span(), [&](unsigned run, Vector<SiteGroup>& found) {
        std::span<const uint64_t> keys = keysOfRun(run);
        // The savings of a group grow with its size, and no group is larger than its hash run.
        bool canAnyGroupBeProfitable = keys.size() >= 2 && std::ranges::any_of(keys, [&](uint64_t key) {
            return savesBytes(sites[static_cast<uint32_t>(key)], keys.size());
        });
        if (!canAnyGroupBeProfitable)
            return;
        size_t firstGroupWithHash = found.size();
        for (uint64_t key : keys) {
            uint32_t site = static_cast<uint32_t>(key);
            std::span<SiteGroup> candidates = found.mutableSubspan(firstGroupWithHash);
            auto equalGroup = std::ranges::find_if(candidates, [&](const SiteGroup& group) {
                return areEqualSites(program, sites[group.first()], sites[site]);
            });
            if (equalGroup != candidates.end())
                equalGroup->append(site);
            else
                found.append(SiteGroup { site });
        }
        found.removeAllMatching([&](const SiteGroup& group) {
            return !savesBytes(sites[group.first()], group.size());
        }, firstGroupWithHash);
    });

    AOT::sort(groups.mutableSpan(), { }, [](const SiteGroup& group) {
        return group.first();
    });
    return groups;
}

static Function createBodyHead(const Program& program, const OutliningSite& site)
{
    const Function& function = program.at(site.function);
    Function created;
    created.name = program.strings.emptyString();
    created.module = function.module;
    created.position = function.position;
    created.kind = FunctionKind::Plain;
    created.role = FunctionRole::OutlinedBody;
    created.length = static_cast<uint32_t>(site.liveIns.size());
    created.thisType = DeclaredType(Proven::byPosition(TypeTable::anything));
    for (ValueID liveIn : site.liveIns)
        created.parameters.append(DeclaredType(Proven::byRulesWithoutChecks(function.at(liveIn).type)));
    created.results.append(DeclaredType(Proven::byRulesWithoutChecks(site.resultType)));
    created.capabilities = initialCapabilitiesOf(created);
    return created;
}

static bool isDefinedInSite(const Function& function, const OutliningSite& site, ValueID id)
{
    if (std::ranges::contains(entryValuesOf(function, site), id))
        return true;
    return std::ranges::any_of(site.interiorBlocks, [&](BlockID block) {
        return function.at(block).values.contains(id);
    });
}

static bool canLeaveByItsLastCall(const AnalysisContext& context, const CallingConventions* conventions, const Function& source, const OutliningSite& site)
{
    const Program& program = context.program;
    std::span<const ValueID> values = entryValuesOf(source, site);
    const Value& root = source.at(site.root);
    if (!conventions || site.isRegion() || !site.rootIsUsed || values.back() != site.root || root.opcode != Opcode::CallDirect || !program.at(root.function()).hasBody())
        return false;
    bool onlyPreparesArguments = std::ranges::all_of(values.first(values.size() - 1), [&](ValueID id) {
        const Value& value = source.at(id);
        return isRematerializable(value) || value.opcode == Opcode::GetCapture || isCastThatProvesNothing(value.opcode);
    });
    if (!onlyPreparesArguments)
        return false;
    ArgumentLocations own = conventions->locationsOf(program, context.id);
    ArgumentLocations callee = conventions->locationsOf(program, root.function());
    if (callee.convention != EntryConvention::Direct || conventions->stackWordsReservedByCallersOf(program, root.function(), callee) || own.numberOfStackWords())
        return false;
    return std::ranges::equal(callee.results, own.results, [](const ArgumentLocation& returned, const ArgumentLocation& expected) {
        return returned.kind != ArgumentLocation::Kind::None && returned.kind == expected.kind && returned.reg == expected.reg && haveSameRegisterRepresentation(returned.representation, expected.representation);
    });
}

static void buildOutlinedBody(const AnalysisContext& context, const CallingConventions* conventions, const Function& source, const OutliningSite& site, Function& body, TypeArena& types)
{
    IRBuilder builder(context, body, types);
    RELEASE_ASSERT(!body.hasBody());
    builder.setOrigin({ source.position, 0 });
    Vector<std::pair<ValueID, ValueID>, 32> valueCopies;
    Vector<std::pair<BlockID, BlockID>, 4> blockCopies;
    BlockID returnBlock;
    ValueID factOfEntry;

    auto copyOf = [](const auto& copies, auto original) {
        size_t index = copies.findIf([&](const auto& entry) {
            return entry.first == original;
        });
        RELEASE_ASSERT(index != notFound);
        return copies[index].second;
    };
    auto addBlockLike = [&](BlockID original, std::span<const ValueID> parameters) {
        const Block& block = source.at(original);
        BlockID added = builder.addBlock(block.frequency);
        body.edit(added, BlockEdit::Rest).shape = block.shape;
        for (ValueID parameter : parameters)
            valueCopies.append({ parameter, builder.addParameter(added, source.at(parameter).type) });
        return added;
    };
    auto copyValue = [&](ValueID id) {
        Value copy(source.at(id));
        copy.forEachOperand([&](ValueID& operand, uint32_t) {
            operand = copyOf(valueCopies, operand);
        });
        if (site.calledBuiltin && Value::opcodesWithCalledBuiltin.contains(copy.opcode))
            copy.setCalledBuiltin({ });
        ValueID copyID = builder.append(WTF::move(copy));
        ASSERT(body.at(copyID).type == source.at(id).type);
        valueCopies.append({ id, copyID });
    };
    auto copyConstantFromOutside = [&](ValueID id) {
        bool hasCopy = valueCopies.containsIf([&](const auto& entry) {
            return entry.first == id;
        });
        if (hasCopy || isDefinedInSite(source, site, id))
            return;
        if (isRematerializable(source.at(id)))
            copyValue(id);
        if (!context.program.typeOf(source, id).isOpaque(OpaqueKind::Fact))
            return;
        // The body is entered behind every fact that its operations name.
        if (!factOfEntry) {
            Value entered = IRBuilder::make(Opcode::Entered);
            entered.setFunction(site.function);
            factOfEntry = builder.append(WTF::move(entered));
        }
        valueCopies.append({ id, factOfEntry });
    };
    auto copyValuesAndTerminator = [&](std::span<const ValueID> values, const Terminator& terminator) {
        for (ValueID id : values)
            copyValue(id);
        Terminator copy(terminator);
        copy.forEachOperand([&](ValueID& operand, uint32_t) {
            operand = copyOf(valueCopies, operand);
        });
        for (Target& target : copy.targets) {
            if (target.block == site.exitBlock) {
                Arguments result;
                if (site.exitArgument)
                    result.append(copyOf(valueCopies, target.arguments[*site.exitArgument]));
                target = Target { returnBlock, WTF::move(result), { } };
                continue;
            }
            target.block = copyOf(blockCopies, target.block);
            for (ValueID& argument : target.arguments)
                argument = copyOf(valueCopies, argument);
        }
        builder.emitTerminator(WTF::move(copy));
    };

    BlockID entry = addBlockLike(site.entryBlock, site.liveIns.span());
    if (site.isRegion()) {
        for (BlockID block : site.interiorBlocks)
            blockCopies.append({ block, addBlockLike(block, source.at(block).parameters.span()) });
        returnBlock = addBlockLike(site.entryBlock, { });
        std::optional<ValueID> result;
        if (site.exitArgument)
            result = builder.addParameter(returnBlock, site.resultType);
        builder.moveToEndOf(returnBlock);
        builder.emitReturn(result);
    }

    builder.moveToEndOf(entry);
    forEachValueListAndTerminator(source, site, [&](std::span<const ValueID> values) {
        for (ValueID id : values) {
            source.at(id).forEachOperand([&](ValueID operand, uint32_t) {
                copyConstantFromOutside(operand);
            });
        }
    }, [&](const Terminator& terminator) {
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            copyConstantFromOutside(operand);
        });
        for (const Target& target : terminator.targets) {
            if (target.block == site.exitBlock) {
                if (site.exitArgument)
                    copyConstantFromOutside(target.arguments[*site.exitArgument]);
                continue;
            }
            for (ValueID argument : target.arguments)
                copyConstantFromOutside(argument);
        }
    });
    if (!site.isRegion()) {
        for (ValueID id : entryValuesOf(source, site))
            copyValue(id);
        if (canLeaveByItsLastCall(context, conventions, source, site)) {
            Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { copyOf(valueCopies, site.root) });
            terminator.setRequirement(TailCallRequirement::Allowed);
            builder.emitTerminator(WTF::move(terminator));
            return;
        }
        builder.emitReturn(site.rootIsUsed ? std::optional(copyOf(valueCopies, site.root)) : std::nullopt);
        return;
    }
    copyValuesAndTerminator(entryValuesOf(source, site), source.at(site.entryBlock).terminator);
    for (BlockID block : site.interiorBlocks) {
        builder.moveToEndOf(copyOf(blockCopies, block));
        copyValuesAndTerminator(source.at(block).values.span(), source.at(block).terminator);
    }
}

static void convertToCall(Value& value, const OutliningSite& site, FunctionID callee)
{
    value.become(Opcode::CallDirect);
    value.type = site.resultType;
    value.setExpected({ });
    value.setFunction(callee);
    value.setCalledBuiltin(site.calledBuiltin.value_or(BuiltinID()));
    value.setArgumentsOperands(site.liveIns.span());
}

static void replaceRegion(IRBuilder& builder, const OutliningSite& site, FunctionID body)
{
    Function& function = builder.function();
    RELEASE_ASSERT(std::ranges::all_of(site.interiorBlocks, [&](BlockID block) {
        return function.at(block).handler == function.at(site.entryBlock).handler;
    }));

    const Target* firstExit = nullptr;
    forEachValueListAndTerminator(function, site, [](std::span<const ValueID>) { }, [&](const Terminator& terminator) {
        for (const Target& target : terminator.targets) {
            if (!firstExit && target.block == site.exitBlock)
                firstExit = &target;
        }
    });
    RELEASE_ASSERT(firstExit);
    Arguments arguments = firstExit->arguments;
    Vector<uint32_t, 4> constantPositions;
    for (uint32_t position = 0; position < arguments.size(); ++position) {
        if (position != site.exitArgument && isDefinedInSite(function, site, arguments[position]))
            constantPositions.append(position);
    }

    Origin origin = function.at(site.entryBlock).terminator.origin();
    origin.isTailCall = site.isTailCall.value_or(origin.isTailCall);
    origin.isPartOfConstruction = site.isPartOfConstruction.value_or(origin.isPartOfConstruction);
    builder.setOrigin(origin);
    builder.splitBlock({ site.entryBlock, site.firstIndex });
    ProgramPoint point { site.entryBlock, site.firstIndex };
    for (uint32_t position : constantPositions) {
        Value constant(function.at(arguments[position]));
        RELEASE_ASSERT(isRematerializable(constant));
        arguments[position] = builder.insertBefore(point, WTF::move(constant));
    }
    Value call = IRBuilder::make(Opcode::CallDirect);
    convertToCall(call, site, body);
    ValueID callID = builder.insertBefore(point, WTF::move(call));
    if (site.exitArgument)
        arguments[*site.exitArgument] = callID;
    builder.applyInsertions();
    builder.redirectEdge(Edge { site.entryBlock, 0 }, site.exitBlock, WTF::move(arguments));
}

void outlineColdOperations(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, OutliningSiteFinder* finder, const ReadersOfOutlinedFunctions* readers)
{
    RELEASE_ASSERT(finder || !readers);
    PhaseTimes times(build, PassID::OutlineColdOperations);
    const Program& program = editor.program();
    ParallelArray<OutliningSite> sites = finder ? finder->takeSites() : findOutliningSites(program, inputs, build);
    times.didFinish("find or take sites"_s);
    const ParallelLoops& loops = build.parallelLoops();
    Vector<SiteGroup> groups;
    if (sites.size())
        groups = findProfitableGroups(program, loops, sites);
    times.didFinish("findProfitableGroups"_s);

    // By function.
    Vector<uint8_t> wasRead;
    // One that holds an id which the loop replaces at its end is not read.
    auto readIfFinal = [&](FunctionID id, const TaskArenas& arenas) {
        if (!readers || arenas.types.hasUsedProvisionalRecords() || arenas.strings.hasUsedProvisionalStrings())
            return;
        readers->read(id);
        wasRead[id.index()] = 1;
    };
    // What outlining has left as it was, and what held an id that was replaced when its loop ended.
    auto readRemainingFunctions = [&] {
        if (!readers)
            return;
        std::span<const uint8_t> hasSites = finder->hasSites();
        Vector<FunctionID> remaining = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
            bool isRead = index < wasRead.size() && wasRead[index];
            if (!isRead && (index >= hasSites.size() || hasSites[index]))
                found.append(FunctionID(index));
        });
        readFunctionsInParallel(program, build, remaining.span(), readers->read);
    };
    auto contextOf = [&](FunctionID id) {
        return AnalysisContext { build, program, &inputs.flow, &inputs.effects, id };
    };
    if (groups.isEmpty()) {
        readRemainingFunctions();
        return;
    }

    Vector<FunctionID> bodyOfSite(FillWith { }, sites.size(), FunctionID());
    FunctionID firstBody = editor.reserveFunctions(groups.size());
    Vector<FunctionID> bodies(groups.size(), [&](size_t index) {
        return FunctionID(firstBody.index() + index);
    });
    forEachIndexInParallel(loops, groups.size(), [&](unsigned index) {
        editor.replaceFunction(bodies[index], createBodyHead(program, sites[groups[index].first()]));
        for (uint32_t site : groups[index])
            bodyOfSite[site] = bodies[index];
    });
    if (readers)
        readers->didAddFunctions();
    times.didFinish("addBodyHeads"_s);

    // The sites come in the order of their functions.
    Vector<uint32_t> replacedSites = findIndicesInParallel(loops, sites.size(), [&](unsigned site) {
        return !!bodyOfSite[site];
    });
    Vector<uint32_t> firstSiteOf = findIndicesInParallel(loops, replacedSites.size(), [&](unsigned position) {
        return !position || sites[replacedSites[position - 1]].function != sites[replacedSites[position]].function;
    });
    Vector<FunctionID> functions = firstSiteOf.map([&](uint32_t position) {
        return sites[replacedSites[position]].function;
    });
    wasRead.fill(static_cast<uint8_t>(0), program.functions.size());
    times.didFinish("createWorkLists"_s);

    // A function that comes early holds the first site of most groups, so the bodies are a loop of their own, over the functions as they are before a site is replaced.
    editFunctionsInParallel(editor, build, bodies.span(), [&](unsigned index, FunctionID id, Function& body, TaskArenas& arenas) {
        const OutliningSite& site = sites[groups[index].first()];
        buildOutlinedBody(contextOf(id), inputs.callingConventions, program.at(site.function), site, body, arenas.types);
        readIfFinal(id, arenas);
    });
    times.didFinish("buildBodiesInParallel"_s);

    lowerFunctionsInParallel(editor, build, functions.span(), { }, [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        size_t begin = firstSiteOf[index];
        size_t end = index + 1 < firstSiteOf.size() ? firstSiteOf[index + 1] : replacedSites.size();
        std::span<const uint32_t> sitesOfFunction = replacedSites.subspan(begin, end - begin);
        IRBuilder builder(contextOf(id), function, arenas.types);
        for (uint32_t site : sitesOfFunction) {
            if (sites[site].isRegion())
                replaceRegion(builder, sites[site], bodyOfSite[site]);
        }

        Vector<std::pair<BlockID, BitVector>, 4> erasedIndexes;
        for (uint32_t siteIndex : sitesOfFunction) {
            const OutliningSite& site = sites[siteIndex];
            if (site.isRegion())
                continue;
            convertToCall(function.edit(site.root), site, bodyOfSite[siteIndex]);
            if (site.numberOfEntryValues == 1)
                continue;
            if (erasedIndexes.isEmpty() || erasedIndexes.last().first != site.entryBlock)
                erasedIndexes.append({ site.entryBlock, BitVector() });
            uint32_t rootIndex = site.firstIndex + site.numberOfEntryValues - 1;
            for (uint32_t erased = site.firstIndex; erased < rootIndex; ++erased)
                erasedIndexes.last().second.set(erased);
        }
        for (const auto& [block, indexes] : erasedIndexes)
            builder.eraseValues(block, indexes);

        compactFunction(function);
        readIfFinal(id, arenas);
    });
    times.didFinish("rewriteFunctionsInParallel"_s);
    readRemainingFunctions();
    times.didFinish("readRemainingFunctions"_s);

    build.statistics().count(StatisticID::OutlinedBodiesCreated, groups.size());
    build.statistics().count(StatisticID::ColdSitesOutlined, replacedSites.size());
    build.destroyLater(WTF::move(sites));
    build.destroyLater(WTF::move(groups));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
