/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocation.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTEffectsDescription.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationMaterialization.h"
#include "AOTFrameAllocationPlanner.h"
#include "AOTFunctionEditing.h"
#include "AOTPass.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(Value::opcodesWithStorage == (OpcodeSet { Opcode::NewClosure, Opcode::NewObject } | arrayAllocationsWithStorage));

namespace {

// Most functions allocate nothing and have no plan.
struct PlannedFunction {
    FunctionID id;
    FramePlan plan;
};

} // anonymous namespace

static bool hasAllocationSite(const Value& value)
{
    return Value::opcodesWithAllocationSite.contains(value.opcode) && value.allocationSite();
}

struct FoundAllocations {
    BitVector allocations;
    // Nothing but arrays that are not empty literals.
    bool areAllFilledArrays { true };
};

static FoundAllocations findAllocations(const Program& program, FunctionID function, bool collectsRemarks)
{
    FoundAllocations found;
    size_t numberOfValues = program.at(function).values.size();
    forEachListedValue(program, function, collectsRemarks ? Value::opcodesWithStorage | Value::opcodesWithAllocationSite : Value::opcodesWithStorage, [&](BlockID, uint32_t, ValueID id, const Value& value) {
        if (!hasStorageChoice(value) && !(collectsRemarks && hasAllocationSite(value)))
            return;
        found.allocations.ensureSizeAndSet(id.index(), numberOfValues);
        found.areAllFilledArrays &= arrayAllocationsWithStorage.contains(value.opcode) && !isEmptyArrayLiteral(value) && !collectsRemarks;
    });
    return found;
}

static FramePlan createHeapPlan(const Program& program, const Function& function, const BitVector& allocations)
{
    FramePlan plan;
    for (size_t index : allocations) {
        ValueID id(index);
        const Value& value = function.at(id);
        AllocationStorage storage = hasStorageChoice(value) ? value.storage() : AllocationStorage::Heap;
        if (storage == AllocationStorage::Heap)
            plan.decisions.append(FrameDecision { .allocation = id });
        if (!hasStorageChoice(value) || arrayAllocationsWithStorage.contains(value.opcode))
            continue;
        if (value.opcode == Opcode::NewClosure) {
            plan.allocation.addClosure(value.function(), storage);
            continue;
        }
        plan.allocation.addCell(value.layout(), storage);
        if (storage == AllocationStorage::Heap && program.at(value.layout()).kind == LayoutKind::Environment)
            ++plan.environmentsOnHeap;
    }
    return plan;
}

// The planner puts nothing of the function into its frame, whatever the escape analysis finds. It follows no more than maxFollowedArrays arrays, and which ones depends on that analysis.
static bool keepsEveryCellOnHeapByForm(const Program& program, const FlowResults& flow, const BuildContext& build, const Function& function, const BitVector& allocations)
{
    bool hasArrays = false;
    for (size_t index : allocations) {
        const Value& allocation = function.at(ValueID(index));
        if (allocation.storage() != AllocationStorage::Heap || isEmptyArrayLiteral(allocation))
            return false;
        if (arrayAllocationsWithStorage.contains(allocation.opcode))
            hasArrays = true;
        else if (!FramePlanner::staysOnHeapByForm(program, flow, build, allocation))
            return false;
    }
    if (!hasArrays)
        return true;
    return findArraysThatMayLieInFrame(program, build, function).size() <= maxFollowedArrays && !mayPutSomeArrayInFrame(program, flow, build, function);
}

// What FramePlanner::finishPlan() lists.
static BitVector findListedAllocations(const Program& program, const BuildContext& build, FunctionAnalyses& analyses, const BitVector& allocations)
{
    const Function& function = analyses.function();
    BitVector listed(function.values.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (allocations.get(id.index()) && (!arrayAllocationsWithStorage.contains(value.opcode) || canLieInFrame(program, build, value)))
                listed.quickSet(id.index());
        }
    }
    return listed;
}

static bool keepsSameCellsOnHeap(const FramePlan& heapPlan, const FramePlan& plan)
{
    return plan.emptyArgumentLists.isEmpty() && plan.demotedReturnCalls.isEmpty()
        && plan.allocation.fieldsHoldingFramePointers().empty() && plan.allocation.capturesHoldingFramePointers().empty()
        && plan.environmentsOnHeap == heapPlan.environmentsOnHeap
        && std::ranges::equal(plan.decisions, heapPlan.decisions, [](const FrameDecision& planned, const FrameDecision& kept) {
            return planned.storage == AllocationStorage::Heap && planned.allocation == kept.allocation && planned.isHeldByClosureTable == kept.isHeldByClosureTable;
        });
}

static std::optional<FramePlan> planFunction(const Program& program, const OptimizationInputs& inputs, BuildContext& build, FunctionID id)
{
    const Function& function = program.at(id);
    auto [allocations, areAllFilledArrays] = findAllocations(program, id, build.options().collectsAllocationRemarks());
    if (allocations.isEmpty())
        return std::nullopt;
    if (!build.isEnabled(PassID::AllocateInFrames, id))
        return createHeapPlan(program, function, allocations);
    if (areAllFilledArrays && !mayPutSomeArrayInFrame(program, inputs.flow, build, function))
        return std::nullopt;

    FunctionAnalyses analyses(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function);
    // A remark and a dump say what keeps a cell on the heap, which the planner finds out.
    bool namesCauses = build.options().collectsAllocationRemarks() || build.options().logging.dumpedPass == PassID::AllocateInFrames;
    if (!namesCauses && !areAllFilledArrays && keepsEveryCellOnHeapByForm(program, inputs.flow, build, function, allocations)) {
        FramePlan plan = createHeapPlan(program, function, findListedAllocations(program, build, analyses, allocations));
        for (FrameDecision& decision : plan.decisions)
            decision.isHeldByClosureTable = FramePlanner::isLikeEveryClosureOfItsFunction(program, &inputs.flow, function, function.at(decision.allocation));
        RELEASE_ASSERT(!build.options().verifiesAfterEveryPass || keepsSameCellsOnHeap(plan, FramePlanner(analyses, inputs.heat).run()));
        return plan;
    }
    return FramePlanner(analyses, inputs.heat).run();
}

bool FramePlan::changesFunction() const
{
    return !emptyArgumentLists.isEmpty() || decisions.containsIf([](const FrameDecision& decision) {
        return decision.storage != AllocationStorage::Heap || decision.isHeldByClosureTable;
    });
}

static Value closureOfHeapLike(const Value& closure)
{
    Value copy = closure;
    copy.setStorage(AllocationStorage::Heap);
    copy.setIsLent(false);
    return copy;
}

// The uses name blocks, values and targets as they were planned. Insertions move no block and no target, and an edge is split once for all that it passes.
static void makeClosuresAgainWhereTheyLeave(IRBuilder& builder, FunctionAnalyses& analyses, Function& function, const FramePlan& plan)
{
    struct Replacement {
        Use use;
        ValueID original;
        ValueID closure;
    };
    Vector<Replacement, 4> replacements;
    for (const FrameDecision& decision : plan.decisions) {
        for (const Use& use : decision.madeAgainAt) {
            if (use.kind == Use::Kind::EdgeArgument)
                continue;
            builder.setOrigin(function.at(decision.allocation).origin());
            replacements.append({ use, decision.allocation, builder.insertBefore(analyses.useLists().pointOf(use), closureOfHeapLike(function.at(decision.allocation))) });
        }
    }
    builder.applyInsertions();
    for (const Replacement& replacement : replacements) {
        if (replacement.use.kind != Use::Kind::Operand) {
            function.edit(replacement.use.block, BlockEdit::Terminator).terminator.setOperand(replacement.use.operandIndex, replacement.closure);
            continue;
        }
        function.edit(replacement.use.user, ValueEdit::Definition).setOperand(replacement.use.operandIndex, replacement.closure);
        // The user may itself be a closure that is made again. What was made of it above holds what it held then.
        for (const Replacement& madeOfUser : replacements) {
            if (madeOfUser.original == replacement.use.user)
                function.edit(madeOfUser.closure, ValueEdit::Definition).setOperand(replacement.use.operandIndex, replacement.closure);
        }
    }

    struct SplitEdge {
        Edge edge;
        BlockID added;
    };
    Vector<SplitEdge, 2> splitEdges;
    for (const FrameDecision& decision : plan.decisions) {
        for (const Use& use : decision.madeAgainAt) {
            if (use.kind != Use::Kind::EdgeArgument)
                continue;
            size_t index = splitEdges.findIf([&](const SplitEdge& split) {
                return split.edge == use.edge();
            });
            if (index == notFound) {
                index = splitEdges.size();
                splitEdges.append({ use.edge(), builder.splitEdge(use.edge()) });
            }
            BlockID added = splitEdges[index].added;
            builder.setOrigin(function.at(decision.allocation).origin());
            function.edit(added, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.targets[0].arguments[use.operandIndex] = builder.insertBefore(ProgramPoint::atHeadOf(added), closureOfHeapLike(function.at(decision.allocation)));
            builder.applyInsertions();
        }
    }
}

static void setStorage(Function& function, const FramePlan& plan)
{
    for (const FrameDecision& decision : plan.decisions) {
        if (decision.storage == AllocationStorage::Heap)
            continue;
        Value& allocation = function.edit(decision.allocation);
        allocation.setStorage(decision.storage);
        if (decision.isLent)
            allocation.setIsLent(true);
    }
}

// applyFramePlan() does no more than setStorage().
static bool setsOnlyStorage(const FramePlan& plan)
{
    return plan.emptyArgumentLists.isEmpty() && plan.demotedReturnCalls.isEmpty() && !plan.decisions.containsIf([](const FrameDecision& decision) {
        return decision.isHeldByClosureTable || decision.storage == AllocationStorage::FrameUntilMaterialized || !decision.madeAgainAt.isEmpty();
    });
}

void applyFramePlan(const AnalysisContext& context, Function& function, TypeArena& types, const FramePlan& plan)
{
    for (ValueID id : plan.emptyArgumentLists) {
        Value& list = function.edit(id);
        list.become(Opcode::EmptyArgumentList);
        list.setValueType(list.type);
    }

    setStorage(function, plan);
    BitVector frameClosures(function.values.size());
    Vector<ValueID, 4> environmentsMaterializedLate;
    for (const FrameDecision& decision : plan.decisions) {
        Value& allocation = function.edit(decision.allocation);
        switch (decision.storage) {
        case AllocationStorage::Heap:
            if (decision.isHeldByClosureTable)
                allocation.setIsHeldByClosureTable(true);
            break;
        case AllocationStorage::Frame:
            if (allocation.opcode == Opcode::NewClosure && !plan.frameClosuresOverHeapCells.contains(decision.allocation))
                frameClosures.quickSet(decision.allocation.index());
            break;
        case AllocationStorage::FrameUntilMaterialized:
            environmentsMaterializedLate.append(decision.allocation);
            break;
        }
    }
    bool makesClosuresAgain = plan.decisions.containsIf([](const FrameDecision& decision) {
        return !decision.madeAgainAt.isEmpty();
    });
    if (environmentsMaterializedLate.isEmpty() && plan.demotedReturnCalls.isEmpty() && !makesClosuresAgain)
        return;

    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    if (makesClosuresAgain)
        makeClosuresAgainWhereTheyLeave(builder, analyses, function, plan);
    for (BlockID block : plan.demotedReturnCalls)
        builder.demoteReturnCall(block);
    for (ValueID environment : environmentsMaterializedLate)
        EnvironmentMaterialization(analyses, environment, frameClosures).run(builder);
}

static AllocationOutcome outcomeOf(AllocationStorage storage)
{
    switch (storage) {
    case AllocationStorage::Heap:
        return AllocationOutcome::Heap;
    case AllocationStorage::Frame:
        return AllocationOutcome::Frame;
    case AllocationStorage::FrameUntilMaterialized:
        return AllocationOutcome::FrameUntilMaterialized;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

AllocationRemark remarkOf(const Function& function, const FrameDecision& decision)
{
    AllocationRemark remark {
        .site = function.at(decision.allocation).allocationSite(),
        .location = function.locationOf(decision.allocation),
        .outcome = outcomeOf(decision.storage),
        .cause = decision.cause,
        .reasons = static_cast<AllocationEscapeReason>(decision.reasons.toRaw()),
        .limit = decision.limit,
    };
    if (!decision.forcedBy)
        return remark;

    const Use& use = *decision.forcedBy;
    switch (use.kind) {
    case Use::Kind::Operand:
        remark.forcedBy = function.locationOf(use.user);
        remark.forcingOpcode = function.at(use.user).opcode;
        return remark;
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        remark.forcedBy = function.locationOf(function.at(use.block).terminator.origin());
        return remark;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addAllocationRemarks(const Function& function, BuildContext& build, const FramePlan& plan)
{
    for (const FrameDecision& decision : plan.decisions) {
        if (hasAllocationSite(function.at(decision.allocation)))
            build.addAllocationRemark(remarkOf(function, decision));
    }
}

static void countPlans(BuildContext& build, std::span<const PlannedFunction> plannedFunctions)
{
    uint64_t closuresInFrames = 0;
    uint64_t closuresLent = 0;
    uint64_t environmentsInFrames = 0;
    uint64_t environmentsOnHeap = 0;
    uint64_t environmentsMaterializedLate = 0;
    uint64_t objectsInFrames = 0;
    uint64_t emptyArgumentListsShared = 0;
    uint64_t arraysInFrames = 0;
    uint64_t restArraysInFrames = 0;
    for (const PlannedFunction& planned : plannedFunctions) {
        const FramePlan& plan = planned.plan;
        emptyArgumentListsShared += plan.emptyArgumentLists.size();
        closuresInFrames += plan.closuresInFrames;
        closuresLent += plan.closuresLent;
        environmentsInFrames += plan.environmentsInFrames;
        environmentsOnHeap += plan.environmentsOnHeap;
        environmentsMaterializedLate += plan.environmentsMaterializedLate;
        objectsInFrames += plan.objectsInFrames;
        arraysInFrames += plan.arraysInFrames;
        restArraysInFrames += plan.restArraysInFrames;
    }
    build.statistics().count(StatisticID::ClosuresInFrames, closuresInFrames);
    build.statistics().count(StatisticID::ClosuresLent, closuresLent);
    build.statistics().count(StatisticID::EnvironmentsInFrames, environmentsInFrames);
    build.statistics().count(StatisticID::EnvironmentsOnHeap, environmentsOnHeap);
    build.statistics().count(StatisticID::EnvironmentsMaterializedLate, environmentsMaterializedLate);
    build.statistics().count(StatisticID::ObjectsInFrames, objectsInFrames);
    build.statistics().count(StatisticID::EmptyArgumentListsShared, emptyArgumentListsShared);
    build.statistics().count(StatisticID::ArraysInFrames, arraysInFrames);
    build.statistics().count(StatisticID::RestArraysInFrames, restArraysInFrames);
}

static void addCellsCreatedByRuntime(const Program& program, FrameAllocation& result)
{
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind != LayoutKind::Environment)
            result.addCell(LayoutID(index), AllocationStorage::Heap);
    }
}

static Vector<PlannedFunction> planFunctions(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<PlannedFunction> plannedFunctions;
    TypeInterningPhase scratch(program.types, 0u, TypeInterningPhase::Scratch);
    {
        ProgramCaches caches(program, build);
        plannedFunctions = collectInParallel<PlannedFunction>(loops, program.functions.size(), largeFunctions(program, loops).span(), [&](unsigned index, Vector<PlannedFunction>& planned) {
            FunctionID id(index);
            std::optional<FramePlan> plan = planFunction(program, inputs, build, id);
            if (!plan)
                return;
            planned.append(PlannedFunction { id, WTF::move(*plan) });
        });
    }
    scratch.discard();
    return plannedFunctions;
}

// The stub of a call may copy a closure that is lent to the heap, with all that it holds.
static bool staysInFrame(const Function& function, const FrameDecision& decision)
{
    return decision.storage == AllocationStorage::Frame && !decision.isLent && function.at(decision.allocation).opcode == Opcode::NewClosure;
}

static BitVector closuresInFrameBy(const Function& function, const FramePlan& plan)
{
    BitVector closures(function.values.size());
    for (const FrameDecision& decision : plan.decisions) {
        if (staysInFrame(function, decision))
            closures.quickSet(decision.allocation.index());
    }
    return closures;
}

static bool keepsClosuresInFrame(const Function& function, const FramePlan& earlier, const FramePlan& later)
{
    BitVector closures = closuresInFrameBy(function, earlier);
    closures.exclude(closuresInFrameBy(function, later));
    return closures.isEmpty();
}

// A closure in the frame captures an entry parameter that the table has lost.
static bool mayBorrowMoreBy(const FlowResults& flow, FunctionID id, const Function& function, const FramePlan& plan)
{
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    return plan.decisions.containsIf([&](const FrameDecision& decision) {
        if (!staysInFrame(function, decision))
            return false;
        return std::ranges::any_of(function.at(decision.allocation).capturesOperands(), [&](ValueID capture) {
            size_t entryParameter = parameters.find(sameObjectThroughCasts(function, capture));
            return entryParameter != notFound && !flow.borrowedParameters().isBorrowed(id, entryParameter);
        });
    });
}

// Something went to the heap, or is made again, for a callee that borrows more now.
static bool mayGainFrom(const Program& program, const Function& function, const FramePlan& plan, const BitVector& functionsThatBorrowMore)
{
    auto handsToSuchFunction = [&](const Use& use) {
        if (use.kind != Use::Kind::Operand || effectRuleOf(function.at(use.user).opcode) != EffectRule::Call)
            return false;
        return std::ranges::any_of(calleesOfCall(program, function.at(use.user)).functions, [&](FunctionID callee) {
            return functionsThatBorrowMore.get(callee.index());
        });
    };
    return plan.decisions.containsIf([&](const FrameDecision& decision) {
        if (decision.storage == AllocationStorage::Heap)
            return decision.forcedBy && handsToSuchFunction(*decision.forcedBy);
        return std::ranges::any_of(decision.madeAgainAt, handsToSuchFunction);
    });
}

AOT_THRESHOLD(unsigned, maxRoundsOfPlans, 3);

// A parameter that a closure of the function's own frame captures stays borrowed (AOTBorrowedParameters.h), and which closures lie in the frame is known by the plans.
static void planAgainWithWhatFramesHold(const Program& program, const OptimizationInputs& inputs, FlowResults& flow, BuildContext& build, Vector<PlannedFunction>& plannedFunctions)
{
    struct GrownWord {
        uint32_t position { 0 };
        uint64_t word { 0 };
    };
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> positionsToScan = findIndicesInParallel(loops, plannedFunctions.size(), [&](unsigned position) {
        return mayBorrowMoreBy(flow, plannedFunctions[position].id, program.at(plannedFunctions[position].id), plannedFunctions[position].plan);
    });
    // The table counts on the closures that these plans put into frames.
    BitVector isCountedOn(plannedFunctions.size());

    for (unsigned round = 1; round < maxRoundsOfPlans && !positionsToScan.isEmpty(); ++round) {
        Vector<GrownWord> grownWords = collectInParallel<GrownWord>(loops, positionsToScan.size(), [&](unsigned index, Vector<GrownWord>& grown) {
            const PlannedFunction& planned = plannedFunctions[positionsToScan[index]];
            uint64_t word = flow.borrowedParameters().wordOfBody(program, build, planned.id, closuresInFrameBy(program.at(planned.id), planned.plan));
            if (word & ~flow.borrowedParameters().wordOf(planned.id))
                grown.append({ positionsToScan[index], word });
        });
        if (grownWords.isEmpty())
            return;

        BitVector functionsThatBorrowMore(program.functions.size());
        for (const GrownWord& grown : grownWords) {
            FunctionID id = plannedFunctions[grown.position].id;
            if (build.options().logging.dumpedPass == PassID::AllocateInFrames) [[unlikely]]
                dataLogLn("borrows more in round ", round, ": f", id.index(), " ", RawHex(flow.borrowedParameters().wordOf(id)), " -> ", RawHex(grown.word));
            flow.addBorrowedParameters(id, grown.word);
            functionsThatBorrowMore.quickSet(id.index());
            isCountedOn.quickSet(grown.position);
        }

        Vector<uint32_t> positionsToPlan = findIndicesInParallel(loops, plannedFunctions.size(), [&](unsigned position) {
            return mayGainFrom(program, program.at(plannedFunctions[position].id), plannedFunctions[position].plan, functionsThatBorrowMore);
        });
        if (build.options().logging.dumpedPass == PassID::AllocateInFrames) [[unlikely]] {
            for (uint32_t position : positionsToPlan)
                dataLogLn("planned again in round ", round, ": f", plannedFunctions[position].id.index());
        }
        TypeInterningPhase scratch(program.types, 0u, TypeInterningPhase::Scratch);
        {
            ProgramCaches caches(program, build);
            forEachIndexInParallel(loops, positionsToPlan.size(), [&](unsigned index) {
                PlannedFunction& planned = plannedFunctions[positionsToPlan[index]];
                std::optional<FramePlan> plan = planFunction(program, inputs, build, planned.id);
                if (!plan)
                    return;
                if (isCountedOn.get(positionsToPlan[index]) && !keepsClosuresInFrame(program.at(planned.id), planned.plan, *plan))
                    return;
                planned.plan = WTF::move(*plan);
            });
        }
        scratch.discard();
        positionsToScan = WTF::move(positionsToPlan);
        positionsToScan.removeAllMatching([&](uint32_t position) {
            return !mayBorrowMoreBy(flow, plannedFunctions[position].id, program.at(plannedFunctions[position].id), plannedFunctions[position].plan);
        });
    }
}

static FrameAllocation mergePlans(const Program& program, BuildContext& build, std::span<const PlannedFunction> plannedFunctions)
{
    FrameAllocation result;
    for (const PlannedFunction& planned : plannedFunctions)
        result.merge(planned.plan.allocation);
    addCellsCreatedByRuntime(program, result);
    result.finish();
    countPlans(build, plannedFunctions);
    return result;
}

static void applyPlans(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, std::span<const PlannedFunction> plannedFunctions)
{
    Vector<FunctionID> changedFunctions;
    Vector<const FramePlan*> plansOfChangedFunctions;
    Vector<const PlannedFunction*> functionsWithNewStorage;
    for (const PlannedFunction& planned : plannedFunctions) {
        if (!planned.plan.changesFunction())
            continue;
        if (setsOnlyStorage(planned.plan)) {
            functionsWithNewStorage.append(&planned);
            continue;
        }
        changedFunctions.append(planned.id);
        plansOfChangedFunctions.append(&planned.plan);
    }
    forEachIndexInParallel(build.parallelLoops(), functionsWithNewStorage.size(), [&](unsigned index) {
        setStorage(editor.atForStorage(functionsWithNewStorage[index]->id), functionsWithNewStorage[index]->plan);
    });
    editFunctionsInParallel(editor, build, changedFunctions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        applyFramePlan(AnalysisContext { build, editor.program(), &inputs.flow, &inputs.effects, id }, function, arenas.types, *plansOfChangedFunctions[index]);
    });
}

// One line for each closure and each environment: where it lies, and what keeps it out of the frame. The census of a run counts closures by the number of the entrypoint.
static void explainClosuresAndEnvironments(const Program& program, const FlowResults& flow, std::span<const PlannedFunction> plannedFunctions)
{
    Vector<FunctionID> entrypoints(FillWith { }, program.functions.size(), FunctionID());
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (function.role == FunctionRole::GenericEntrypoint)
            entrypoints[function.creator.as<CreatorRecord::Entrypoint>().target.index()] = FunctionID(index);
    }

    PrintStream& out = WTF::dataFile();
    for (const PlannedFunction& planned : plannedFunctions) {
        const Function& function = program.at(planned.id);
        for (const FrameDecision& decision : planned.plan.decisions) {
            const Value& cell = function.at(decision.allocation);
            if (cell.opcode == Opcode::NewClosure)
                out.print("closure f", cell.function().index(), " entered by f", entrypoints[cell.function().index()].index());
            else if (cell.opcode == Opcode::NewObject && program.at(cell.layout()).isEnvironment())
                out.print("environment L", cell.layout().index());
            else
                continue;
            out.print(" made in f", planned.id.index(), ": ", nameOf(decision.storage), decision.isLent ? " lent" : "", decision.madeAgainAt.isEmpty() ? "" : " made again", decision.isHeldByClosureTable ? " of the table" : "");
            if (decision.storage == AllocationStorage::Heap)
                out.print(" ", nameOf(decision.cause));
            if (decision.forcedBy && decision.forcedBy->kind == Use::Kind::Operand) {
                const Value& user = function.at(decision.forcedBy->user);
                out.print(" by operand ", decision.forcedBy->operandIndex, " of ", nameOf(user.opcode));
                if (user.opcode == Opcode::CallDirect)
                    out.print(" of f", user.function().index());
            } else if (decision.forcedBy)
                out.print(" by an edge or a terminator");
            if (cell.opcode == Opcode::NewClosure && cell.capturesOperands().empty())
                out.print(", captures nothing");
            if (cell.opcode == Opcode::NewClosure && flow.function(cell.function()).closureUse == ClosureUse::CalledOnly)
                out.print(", only called");
            out.print(" reasons ", decision.reasons.toRaw());
            if (SourceLocation location = function.locationOf(decision.allocation); hasModule(location)) {
                const Module& module = program.modules[location.module];
                out.print(" at ", byteCast<char8_t>(program.string(module.displayPath ? module.displayPath : module.path)), ":", program.lineColumnFor(ModuleID(location.module), location.position).line);
            }
            out.print("\n");
        }
    }
}

FrameAllocation allocateInFrames(ProgramEditor& editor, const OptimizationInputs& inputs, FlowResults& flow, BuildContext& build)
{
    RELEASE_ASSERT(&flow == &inputs.flow);
    PhaseTimes times(build, PassID::AllocateInFrames);
    Vector<PlannedFunction> plannedFunctions = planFunctions(editor.program(), inputs, build);
    times.didFinish("planFunctions"_s);
    planAgainWithWhatFramesHold(editor.program(), inputs, flow, build, plannedFunctions);
    times.didFinish("planAgainWithWhatFramesHold"_s);
    if (build.options().collectsAllocationRemarks()) {
        for (const PlannedFunction& planned : plannedFunctions)
            addAllocationRemarks(editor.program().at(planned.id), build, planned.plan);
    }
    if (build.options().logging.dumpedPass == PassID::AllocateInFrames) [[unlikely]]
        explainClosuresAndEnvironments(editor.program(), inputs.flow, plannedFunctions.span());

    // The edits change functions and types. The merge reads the plans and the layouts.
    FrameAllocation result;
    runTasksInParallel(build.parallelLoops(), {
        [&] { applyPlans(editor, inputs, build, plannedFunctions.span()); },
        [&] { result = mergePlans(editor.program(), build, plannedFunctions.span()); },
    });
    times.didFinish("applyAndMergePlans"_s);
    build.destroyLater(WTF::move(plannedFunctions));
    return result;
}

bool FrameAllocation::mayHoldFramePointer(FieldReference field) const
{
    RELEASE_ASSERT(m_isFinished);
    return std::ranges::binary_search(m_fieldsHoldingFramePointers, field);
}

bool FrameAllocation::mayHoldFramePointer(CaptureReference capture) const
{
    RELEASE_ASSERT(m_isFinished);
    return std::ranges::binary_search(m_capturesHoldingFramePointers, capture);
}

void FrameAllocation::addCell(LayoutID layout, AllocationStorage storage)
{
    RELEASE_ASSERT(layout);
    m_pendingCells.append(PendingStorage<LayoutID> { layout, storage });
    m_isFinished = false;
}

void FrameAllocation::addClosure(FunctionID closureFunction, AllocationStorage storage)
{
    RELEASE_ASSERT(closureFunction);
    m_pendingClosures.append(PendingStorage<FunctionID> { closureFunction, storage });
    m_isFinished = false;
}

void FrameAllocation::addHolder(FieldReference field)
{
    m_fieldsHoldingFramePointers.append(field);
    m_isFinished = false;
}

void FrameAllocation::addHolder(CaptureReference capture)
{
    m_capturesHoldingFramePointers.append(capture);
    m_isFinished = false;
}

void FrameAllocation::merge(const FrameAllocation& other)
{
    m_pendingCells.appendVector(other.m_pendingCells);
    m_pendingClosures.appendVector(other.m_pendingClosures);
    m_layoutsInFrames.merge(other.m_layoutsInFrames);
    m_layoutsOnHeap.merge(other.m_layoutsOnHeap);
    m_closuresInFrames.merge(other.m_closuresInFrames);
    m_closuresOnHeap.merge(other.m_closuresOnHeap);
    m_fieldsHoldingFramePointers.appendVector(other.m_fieldsHoldingFramePointers);
    m_capturesHoldingFramePointers.appendVector(other.m_capturesHoldingFramePointers);
    m_isFinished = false;
}

template<typename Entry>
static void applyPendingStorage(Vector<Entry>& pending, BitVector& inFrames, BitVector& onHeap)
{
    size_t size = 0;
    for (const Entry& entry : pending)
        size = std::max(size, static_cast<size_t>(entry.id.index()) + 1);
    inFrames.ensureSize(size);
    onHeap.ensureSize(size);

    for (const Entry& entry : pending) {
        switch (entry.storage) {
        case AllocationStorage::Heap:
            onHeap.quickSet(entry.id.index());
            break;
        case AllocationStorage::Frame:
            inFrames.quickSet(entry.id.index());
            break;
        case AllocationStorage::FrameUntilMaterialized:
            inFrames.quickSet(entry.id.index());
            onHeap.quickSet(entry.id.index());
            break;
        }
    }
    pending.clear();
}

void FrameAllocation::finish()
{
    applyPendingStorage(m_pendingCells, m_layoutsInFrames, m_layoutsOnHeap);
    applyPendingStorage(m_pendingClosures, m_closuresInFrames, m_closuresOnHeap);
    AOT::sort(m_fieldsHoldingFramePointers);
    removeRepeatedElements(m_fieldsHoldingFramePointers);
    AOT::sort(m_capturesHoldingFramePointers);
    removeRepeatedElements(m_capturesHoldingFramePointers);
    m_isFinished = true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
