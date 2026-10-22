/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeat.h"

#if ENABLE(AOT)

#include "AOTSummarizeForInlining.h"
#include "AOTBlockFrequencies.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCodeShapes.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeatGraph.h"
#include "AOTPass.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <cmath>
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

// Which is expected of no call.
static bool runsOnlyBehindFailedTest(const Program& program, FunctionID id)
{
    return program.at(id).runsOnlyBehindFailedTest();
}

static double maxLayoutAllocations(const Heat& heat, const Program& program, TypeID id)
{
    if (!id || TypeTable::isProvisional(id))
        return 0;
    const Type& type = program.at(id);
    double allocations = 0;
    for (LayoutID layout : membersOf(program, type))
        allocations = std::max(allocations, heat.allocationsOf(layout));
    for (const ClassPart& part : type.classes) {
        for (TypeID contents : part.contents)
            allocations = std::max(allocations, maxLayoutAllocations(heat, program, contents));
    }
    return allocations;
}

static double estimateActivationsFromAllocations(const Heat& heat, const Program& program, const Function& function)
{
    const auto* parser = function.creator.tryAs<CreatorRecord::Parser>();
    return parser ? maxLayoutAllocations(heat, program, parser->type) : 0;
}

double Heat::activationsOf(const Program& program, FunctionID id) const
{
    constexpr double activationsWhereEverythingIsHot = 1e12;

    if (m_classOfEverything)
        return isHot(*m_classOfEverything) ? activationsWhereEverythingIsHot : 0;
    if (runsOnlyBehindFailedTest(program, id))
        return 0;
    const Entry& entry = m_entries.of(program, id);
    if (entry.heatClass != HeatClass::Unknown || isOfVeryLargeProgram())
        return entry.activations;
    return std::max(entry.activations, estimateActivationsFromAllocations(*this, program, program.at(id)));
}

double Heat::estimatedActivationsOf(const Program& program, FunctionID id) const
{
    if (m_classOfEverything || runsOnlyBehindFailedTest(program, id))
        return activationsOf(program, id);
    RELEASE_ASSERT(m_hasWeights);
    return m_entries.of(program, id).estimatedActivations;
}

static uint64_t allocationKeyOf(LayoutID layout, ModuleID module)
{
    return static_cast<uint64_t>(layout.index()) << 32 | module.index();
}

bool Heat::isAllocatedIn(LayoutID layout, ModuleID module) const
{
    RELEASE_ASSERT(m_hasWeights);
    return std::ranges::binary_search(m_modulesThatAllocate, allocationKeyOf(layout, module));
}

static uint64_t storedLayoutKeyOf(PropertyKeyID name, LayoutID layout)
{
    return static_cast<uint64_t>(name.index()) << 32 | layout.index();
}

bool Heat::isStoredUnder(LayoutID layout, PropertyKeyID name) const
{
    RELEASE_ASSERT(m_hasWeights);
    return std::ranges::binary_search(m_layoutsStoredUnderNames, storedLayoutKeyOf(name, layout));
}

double Heat::allocationsOf(LayoutID layout) const
{
    RELEASE_ASSERT(m_hasWeights);
    return layout.index() < m_allocationsOfLayouts.size() ? m_allocationsOfLayouts[layout.index()] : 0;
}

HeatClass Heat::of(const Program& program, FunctionID id) const
{
    if (m_classOfEverything)
        return *m_classOfEverything;
    if (runsOnlyBehindFailedTest(program, id))
        return HeatClass::Cold;
    return m_entries.of(program, id).heatClass;
}

bool Heat::isCompact(const Program& program, FunctionID id) const
{
    return !m_classOfEverything && m_entries.of(program, id).isCompact;
}

ASCIILiteral Heat::classNameOf(const Program& program, FunctionID id) const
{
    return isCompact(program, id) ? "Compact"_s : nameOf(of(program, id));
}

bool Heat::hasWarmCode(const Program& program, FunctionID id) const
{
    if (m_classOfEverything)
        return !isCold(*m_classOfEverything);
    return !runsOnlyBehindFailedTest(program, id) && m_entries.of(program, id).hasWarmCode;
}

uint8_t Heat::percentileOf(const Program& program, FunctionID id) const
{
    if (runsOnlyBehindFailedTest(program, id))
        return 0;
    return m_entries.of(program, id).percentile;
}

double Heat::leastHeatOfFastCode(const Program& program, FunctionID id, double frequencyInFunction) const
{
    if (!m_thresholds)
        return frequencyConstants.iterationsPerLoop;
    bool runsInLoop = frequencyInFunction >= HeatModel::minWeightOfCallInLoop || m_entries.of(program, id).isCalledInLoop;
    return runsInLoop ? m_thresholds->leastHeatOfFastCodeInLoops : frequencyConstants.iterationsPerLoop;
}

// Where nothing in the program runs more often than one loop, the loop is hot, and so is the function that it calls. A wrapper that the engine has made for that function is not:
// nobody may enter it, and a hot wrapper takes in a copy of the body.
static bool leadsToBody(const Function& function)
{
    return isWrapperCreatedByEngine(function) || function.creator.kind() == CreatorRecord::Kind::Entrypoint;
}

HeatClass Heat::classOf(const Program& program, FunctionID id, double frequencyInFunction) const
{
    return classOf(program, id, activationsOf(program, id), frequencyInFunction);
}

HeatClass Heat::classOf(const Program& program, FunctionID id, double activations, double frequencyInFunction) const
{
    double heat = activations * frequencyInFunction;
    HeatClass heatClass = classOf(heat);
    if (!isHot(heatClass) || heat >= frequencyConstants.iterationsPerLoop || frequencyInFunction >= HeatModel::minWeightOfCallInLoop)
        return heatClass;
    return m_entries.of(program, id).isCalledInLoop && !leadsToBody(program.at(id)) ? heatClass : HeatClass::Warm;
}

HeatClass Heat::classOf(double heat) const
{
    if (m_classOfEverything)
        return *m_classOfEverything;
    if (!m_thresholds)
        return HeatClass::Unknown;
    if (!(heat > 0))
        return HeatClass::Cold;
    if (heat >= m_thresholds->hot)
        return HeatClass::Hot;
    if (heat < m_thresholds->cold)
        return HeatClass::Cold;
    return HeatClass::Warm;
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
        out.print("f", index, " ", classNameOf(program, id), " ", entry.percentile, " ");
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

namespace {

// heat is the frequency per activation until computeThresholds multiplies in the function's activations.
struct BlockHeat {
    double heat { 0 };
    uint32_t size { 0 };
    uint32_t functionIndex { 0 };
};

} // anonymous namespace

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
// Blocks that are equally hot have one class, so the hottest ones are hot however large they are. The library is in every executable and its share of a small one
// is large, so it has no say in which code of the application is hot. Code outside every loop, recursion and handler is never hot. Code that is expected to run less than
// once is cold however much of it there is, and code that is expected to run more than once is not cold however little there is below it.
// A fifth of a very large program is more than a processor keeps at hand, and most of it never runs: what is hot also fits MaxHotCodeValues,
// and what is not cold fits MaxWarmCodeValues: below the hottest code of that size everything is cold, however often it is expected to run.
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

// Most of a very large program never runs, and which part does is not in the estimate: its products along paths of calls pass every bound, and a call that nobody follows
// spreads evenly over all that it may reach. A share of the code, or a budget, would also fill up again with other functions whenever some code goes away.
// So the classes there are read off the structure and depend on nothing else in the program: a function that many functions call runs often,
// a loop runs as often as a loop, and the rest is cold.
static bool isVeryLarge(uint64_t totalSize, BuildContext& build)
{
    return totalSize > HeatModel::warmCodeLimitsInVeryLargeProgram * build.limit(LimitID::MaxWarmCodeValues);
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
                std::atomic_ref(isCalledInLoop[callee.index()]).store(1, std::memory_order_relaxed);
        }
    });
    return isCalledInLoop;
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
    while (!worklist.isEmpty()) {
        HeatNodeID caller = graph.nodeOf(FunctionID(worklist.takeLast()));
        std::span<const HeatNodeID> callees = graph.successors(caller);
        for (unsigned index = 0; index < callees.size(); ++index) {
            FunctionID callee = graph.functionOf(callees[index]);
            if (!callee || runsInLoop[callee.index()] || graph.multiplierOf(caller, index) < HeatModel::minWeightOfNearlyCertainCall)
                continue;
            runsInLoop[callee.index()] = 1;
            worklist.append(callee.index());
        }
    }
    return runsInLoop;
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

// The callers of a function are seen at the entry that the engine has put in front of it, which tests or unpacks a parameter, packs a result or binds arguments.
// The body behind the entry has that one caller, and runs as often. Not the body of an async function, which waits between its steps.
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
            if (flow.runsAtMostOnce(program, callee) || program.at(callee).has(FunctionFlags::IsAsync))
                continue;
            runsOften[callee.index()] = 1;
            if (leadsToBody(program.at(callee)))
                worklist.append(callee.index());
        }
    }
}

// The structure of a very large program does not tell which of its cold functions run, but it tells which hardly do. A function may run if a chain of known calls leads to it from a function
// that runs often, that a loop calls, also behind a branch or two, or that the host calls for every event; a call that its caller rarely reaches is not followed. It may also run if two functions call it, or if it has a heavy loop:
// its values run three or more times per entry. Such a function says little about most of its callees, but a callee that it calls nearly every time may run too:
// an entry that only checks its arguments has all the callers, and the body behind it has one. Code that runs once gives no reason: what it calls, it calls once.
// A small function is left out: without its tests it saves few bytes, and accessors, selectors and callbacks, which nobody is seen to call, are what is entered most.
// Everything else is compact: a third of such a program, and little of what runs. So is an async function, which waits between its steps, of whatever size and with however many callers,
// unless it has a loop of its own or a chain of calls that each weigh a quarter or more leads to it.
static Vector<uint8_t> findCompactFunctions(const Program& program, const FlowResults& flow, const HeatGraph& graph, std::span<const uint32_t> functionsWithBody, const Vector<uint8_t>& isExpectedToRun)
{
    Vector<uint32_t> worklist;
    auto followCalls = [&](Vector<uint8_t>& isReached, HeatNodeID caller, double minWeight) {
        std::span<const HeatNodeID> callees = graph.successors(caller);
        for (unsigned index = 0; index < callees.size(); ++index) {
            FunctionID callee = graph.functionOf(callees[index]);
            if (!callee || isReached[callee.index()] || graph.multiplierOf(caller, index) < minWeight)
                continue;
            isReached[callee.index()] = 1;
            worklist.append(callee.index());
        }
    };
    auto drainWorklist = [&](Vector<uint8_t>& isReached, double minWeight) {
        while (!worklist.isEmpty())
            followCalls(isReached, graph.nodeOf(FunctionID(worklist.takeLast())), minWeight);
    };
    auto findWhatBusyCodeReaches = [&](double minWeight) {
        Vector<uint8_t> isReached = isExpectedToRun;
        for (uint32_t index : functionsWithBody) {
            if (isExpectedToRun[index])
                worklist.append(index);
        }
        followCalls(isReached, graph.hostNode(), minWeightOfCallThatLoopRepeats);
        for (uint32_t index : functionsWithBody) {
            if (!flow.runsAtMostOnce(program, FunctionID(index)))
                followCalls(isReached, graph.nodeOf(FunctionID(index)), HeatModel::minWeightOfCallInLoop);
        }
        drainWorklist(isReached, minWeight);
        return isReached;
    };
    Vector<uint8_t> isNearBusyCode = findWhatBusyCodeReaches(HeatModel::minWeightOfCallNearBusyCode);
    Vector<uint8_t> mayRun = findWhatBusyCodeReaches(HeatModel::minWeightOfCallToFollow);

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
    drainWorklist(mayRun, HeatModel::minWeightOfNearlyCertainCall);

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
static constexpr size_t maxLayoutsOfStoredValue = 4;

static Vector<uint64_t> collectLayoutsStoredUnderNames(const Program& program, const ParallelLoops& loops, std::span<const uint32_t> functionsWithBody)
{
    Vector<uint64_t> allStored = collectInParallel<uint64_t>(loops, functionsWithBody.size(), [&](unsigned k, Vector<uint64_t>& stored) {
        const Function& function = program.functions[functionsWithBody[k]];
        auto add = [&](PropertyKeyID name, ValueID value) {
            if (!name || !value)
                return;
            LayoutSetID layouts = program.typeOf(function, value).layouts;
            if (!layouts || program.at(layouts).members.size() > maxLayoutsOfStoredValue)
                return;
            for (LayoutID layout : program.at(layouts).members)
                stored.append(storedLayoutKeyOf(name, layout));
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
    HeatGraph graph = buildHeatGraph(program, callGraph, flow, *summariesFromFlow, build, references, keptWeights, madeWeights);
    times.didFinish("buildHeatGraph"_s);

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
    // The thresholds of a very large program are fixed, and no class in it comes from a threshold.
    const bool isVeryLargeProgram = isVeryLarge(totalSize, build);
    heat.m_hasWeights = heatFor == HeatFor::Everything || !isVeryLargeProgram;
    Vector<uint8_t> isCalledInLoop;
    Vector<uint8_t> runsInLoop;
    Vector<uint8_t> functionsThatRunOften;
    Vector<uint8_t> isCompact;
    // estimateActivations is serial, so everything that needs only the graph runs next to it.
    runTasksInParallel(loops, {
        [&] {
            if (heat.m_hasWeights)
                activations = estimateActivations(graph);
        },
        [&] {
            if (!isVeryLargeProgram)
                applicationBlocks = collectApplicationBlocks(program, loops, graph, functionsWithBody);
            heat.m_entries = PerFunction<Heat::Entry>(count);
            if (heat.m_hasWeights)
                heat.m_layoutsStoredUnderNames = collectLayoutsStoredUnderNames(program, loops, functionsWithBody.span());
            isCalledInLoop = findFunctionsCalledInLoops(loops, graph, functionsWithBody.span());
            runsInLoop = followNearlyCertainCalls(graph, isCalledInLoop);
            if (!isVeryLargeProgram)
                return;
            functionsThatRunOften.fill(static_cast<uint8_t>(0), graph.numberOfFunctions());
            forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
                FunctionID id(functionsWithBody[k]);
                functionsThatRunOften[id.index()] = hasManyCallers(program, flow, graph, id);
            });
            followEntriesMadeByEngine(program, flow, graph, functionsWithBody.span(), functionsThatRunOften);
            // What a loop calls and few others is entered no more often than cold code is. It may run all the same.
            Vector<uint8_t> functionsExpectedToRun = functionsThatRunOften;
            for (uint32_t index : functionsWithBody)
                functionsExpectedToRun[index] |= isCalledInLoop[index];
            isCompact = findCompactFunctions(program, flow, graph, functionsWithBody.span(), functionsExpectedToRun);
        },
    });
    times.didFinish("estimateActivations"_s);
    if (functionsWithBody.isEmpty())
        return Heat();

    Vector<uint8_t> percentiles;
    if (heat.m_hasWeights) {
        heat.m_allocationsOfLayouts.fill(0, program.layouts.size());
        for (const AllocationSite& site : flow.allocationSites()) {
            if (site.layout && site.function && site.layout.index() < program.layouts.size() && site.function.index() < activations.size()) {
                heat.m_allocationsOfLayouts[site.layout.index()] += activations[site.function.index()];
                if (ModuleID module = program.at(site.function).module)
                    heat.m_modulesThatAllocate.append(allocationKeyOf(site.layout, module));
            }
        }
        std::ranges::sort(heat.m_modulesThatAllocate);
        removeRepeatedElements(heat.m_modulesThatAllocate);

        runTasksInParallel(loops, {
            [&] {
                if (!isVeryLargeProgram)
                    heat.m_thresholds = computeThresholds(loops, WTF::move(applicationBlocks), activations, build.limit(LimitID::MaxHotCodeValues), build.limit(LimitID::MaxWarmCodeValues));
            },
            [&] {
                Vector<FunctionDensity> order = sortByDensity(loops, graph, functionsWithBody, activations);
                percentiles = computePercentiles(graph, order, totalSize);
                heat.m_hottestFirst = Vector<FunctionID>(order.size(), [&](size_t k) {
                    return order[k].function;
                });
            },
        });
    }
    times.didFinish("computeThresholds"_s);
    forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
        uint32_t i = functionsWithBody[k];
        FunctionID id(i);
        // Without weights: classifyByCallers gives the entry its activations and its percentile.
        heat.m_entries[id] = heat.m_hasWeights ? Heat::Entry { activations[i], HeatClass::RunsOnce, percentiles[i], activations[i] } : Heat::Entry { 1, HeatClass::RunsOnce };
        heat.m_entries[id].isCalledInLoop = runsInLoop[i];
        if (!flow.runsAtMostOnce(program, id))
            heat.m_entries[id].heatClass = heat.classOf(program, id, 1);
    });
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
    }
    forEachIndexInParallel(loops, functionsWithBody.size(), [&](unsigned k) {
        FunctionID id(functionsWithBody[k]);
        Heat::Entry& entry = heat.m_entries[id];
        entry.hasWarmCode = std::ranges::any_of(graph.blockGroupsOf(id), [&](const HeatBlockGroup& group) {
            bool runsOnce = entry.heatClass == HeatClass::RunsOnce && group.frequency <= roundingOfFrequencySums;
            return !runsOnce && !isCold(heat.classOf(entry.activations * group.frequency));
        });
    });
    build.destroyLater(WTF::move(graph));
    return heat;
}

HeatClass heatClassOf(const Heat& heat, FunctionAnalyses& analyses, BlockID block)
{
    const AnalysisContext& context = analyses.context();
    double activations = heat.activationsOf(context.program, context.id);
    bool isColdEverywhere = heat.classOf(context.program, context.id, activations, roundingOfFrequencySums) == HeatClass::Cold && runsNoBlockMoreOftenThanEntry(analyses.cfg());
    HeatClass heatClass = isColdEverywhere ? HeatClass::Cold : heat.classOf(context.program, context.id, activations, analyses.blockFrequencies().frequencyOf(block));
    switch (context.build.recordedOptions().heat()) {
    case HeatOverride::EverythingIsHot:
    case HeatOverride::EverythingIsCold:
        return heatClass;
    case HeatOverride::None:
    case HeatOverride::OnlyLoopsAreHot:
    case HeatOverride::NothingIsHottest:
        break;
    }
    return analyses.executionCountOf(block) == ExecutionCount::AtMostOnce ? HeatClass::RunsOnce : heatClass;
}

bool isInColderPartOfProgram(const Heat& heat, const Program& program, FunctionID id)
{
    constexpr uint8_t firstPercentileOfHotterPart = 35;

    HeatClass heatClass = heat.of(program, id);
    if (isHot(heatClass) || isCold(heatClass))
        return isCold(heatClass);
    std::span<const FunctionID> hottestFirst = heat.hottestFirst();
    if (hottestFirst.empty() || !heat.percentileOf(program, hottestFirst.front()))
        return false;
    return heat.percentileOf(program, id) < firstPercentileOfHotterPart;
}

HeatClass heatClassOf(FunctionPassContext& context, BlockID block)
{
    return heatClassOf(context.inputs.heat, context.analyses, block);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
