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
#include "AOTFrameAllocationPlanner.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(Value::opcodesWithStorage == (OpcodeSet { Opcode::NewClosure, Opcode::NewObject } | arrayAllocationsWithStorage));

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

static FramePlan createHeapPlan(const Program& program, FunctionID id, const BitVector& allocations)
{
    FramePlan plan { .function = id };
    for (size_t index : allocations) {
        const Value& value = program.at(id).at(ValueID(index));
        if (!hasStorageChoice(value) || value.storage() == AllocationStorage::Heap)
            plan.decisions.append(FrameDecision { .allocation = ValueID(index) });
        if (hasStorageChoice(value))
            plan.add(program, value, value.storage(), value.opcode == Opcode::NewClosure && value.isLent());
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
        else if (!staysOnHeapByForm(program, flow, build, allocation))
            return false;
    }
    return !hasArrays || (findArraysThatMayLieInFrame(program, build, function).size() <= maxFollowedArrays && !mayPutSomeArrayInFrame(program, flow, build, function));
}

// What the plan of the planner lists.
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
        && plan.fieldsHoldingFramePointers.isEmpty() && plan.capturesHoldingFramePointers.isEmpty()
        && plan.counts == heapPlan.counts
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
        return createHeapPlan(program, id, allocations);
    if (areAllFilledArrays && !mayPutSomeArrayInFrame(program, inputs.flow, build, function))
        return std::nullopt;

    FunctionAnalyses analyses(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function);
    // A remark says what keeps a cell on the heap, which the planner finds out.
    bool namesCauses = build.options().collectsAllocationRemarks();
    if (namesCauses || areAllFilledArrays || !keepsEveryCellOnHeapByForm(program, inputs.flow, build, function, allocations))
        return planFrame(analyses, inputs.heat);

    FramePlan plan = createHeapPlan(program, id, findListedAllocations(program, build, analyses, allocations));
    for (FrameDecision& decision : plan.decisions)
        decision.isHeldByClosureTable = isLikeEveryClosureOfItsFunction(program, &inputs.flow, function, function.at(decision.allocation));
    RELEASE_ASSERT(!build.options().verifiesAfterEveryPass || keepsSameCellsOnHeap(plan, planFrame(analyses, inputs.heat)));
    return plan;
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

static void applyFramePlan(const AnalysisContext& context, Function& function, TypeArena& types, const FramePlan& plan)
{
    for (ValueID id : plan.emptyArgumentLists) {
        Value& list = function.edit(id);
        list.become(Opcode::EmptyArgumentList);
        list.setValueType(list.type);
    }

    setStorage(function, plan);
    BitVector frameClosures(function.values.size());
    Vector<ValueID, 4> environmentsMaterializedLate;
    bool makesClosuresAgain = false;
    for (const FrameDecision& decision : plan.decisions) {
        Value& allocation = function.edit(decision.allocation);
        makesClosuresAgain |= !decision.madeAgainAt.isEmpty();
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
    if (environmentsMaterializedLate.isEmpty() && plan.demotedReturnCalls.isEmpty() && !makesClosuresAgain)
        return;

    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    if (makesClosuresAgain)
        makeClosuresAgainWhereTheyLeave(builder, analyses, function, plan);
    for (BlockID block : plan.demotedReturnCalls)
        builder.demoteReturnCall(block);
    for (ValueID environment : environmentsMaterializedLate)
        materializeEnvironmentLate(builder, analyses, environment, frameClosures);
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

static Vector<FramePlan> planFunctions(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<FramePlan> plans;
    TypeInterningPhase scratch(program.types, 0u, TypeInterningPhase::Scratch);
    {
        ProgramCaches caches(program, build);
        plans = collectInParallel<FramePlan>(loops, program.functions.size(), largeFunctions(program, loops).span(), [&](unsigned index, Vector<FramePlan>& planned) {
            if (std::optional<FramePlan> plan = planFunction(program, inputs, build, FunctionID(index)))
                planned.append(WTF::move(*plan));
        });
    }
    scratch.discard();
    return plans;
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
static bool mayBorrowMoreBy(const Program& program, const FlowResults& flow, const FramePlan& plan)
{
    const Function& function = program.at(plan.function);
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    return plan.decisions.containsIf([&](const FrameDecision& decision) {
        if (!staysInFrame(function, decision))
            return false;
        return std::ranges::any_of(function.at(decision.allocation).capturesOperands(), [&](ValueID capture) {
            size_t entryParameter = parameters.find(sameObjectThroughCasts(function, capture));
            return entryParameter != notFound && !flow.borrowedParameters().isBorrowed(plan.function, entryParameter);
        });
    });
}

// Something went to the heap, or is made again, for a callee that borrows more now.
static bool mayGainFrom(const Program& program, const FramePlan& plan, const BitVector& functionsThatBorrowMore)
{
    const Function& function = program.at(plan.function);
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
static void planAgainWithWhatFramesHold(const Program& program, const OptimizationInputs& inputs, FlowResults& flow, BuildContext& build, Vector<FramePlan>& plans)
{
    struct GrownWord {
        uint32_t position { 0 };
        uint64_t word { 0 };
    };
    const ParallelLoops& loops = build.parallelLoops();
    Vector<uint32_t> positionsToScan = findIndicesInParallel(loops, plans.size(), [&](unsigned position) {
        return mayBorrowMoreBy(program, flow, plans[position]);
    });
    // The table counts on the closures that these plans put into frames.
    BitVector isCountedOn(plans.size());

    for (unsigned round = 1; round < maxRoundsOfPlans && !positionsToScan.isEmpty(); ++round) {
        Vector<GrownWord> grownWords = collectInParallel<GrownWord>(loops, positionsToScan.size(), [&](unsigned index, Vector<GrownWord>& grown) {
            const FramePlan& plan = plans[positionsToScan[index]];
            uint64_t word = flow.borrowedParameters().wordOfBody(program, build, plan.function, closuresInFrameBy(program.at(plan.function), plan));
            if (word & ~flow.borrowedParameters().wordOf(plan.function))
                grown.append({ positionsToScan[index], word });
        });
        if (grownWords.isEmpty())
            return;

        BitVector functionsThatBorrowMore(program.functions.size());
        for (const GrownWord& grown : grownWords) {
            FunctionID id = plans[grown.position].function;
            flow.addBorrowedParameters(id, grown.word);
            functionsThatBorrowMore.quickSet(id.index());
            isCountedOn.quickSet(grown.position);
        }

        Vector<uint32_t> positionsToPlan = findIndicesInParallel(loops, plans.size(), [&](unsigned position) {
            return mayGainFrom(program, plans[position], functionsThatBorrowMore);
        });
        TypeInterningPhase scratch(program.types, 0u, TypeInterningPhase::Scratch);
        {
            ProgramCaches caches(program, build);
            forEachIndexInParallel(loops, positionsToPlan.size(), [&](unsigned index) {
                FramePlan& earlier = plans[positionsToPlan[index]];
                std::optional<FramePlan> plan = planFunction(program, inputs, build, earlier.function);
                if (plan && (!isCountedOn.get(positionsToPlan[index]) || keepsClosuresInFrame(program.at(earlier.function), earlier, *plan)))
                    earlier = WTF::move(*plan);
            });
        }
        scratch.discard();
        positionsToScan = WTF::move(positionsToPlan);
        positionsToScan.removeAllMatching([&](uint32_t position) {
            return !mayBorrowMoreBy(program, flow, plans[position]);
        });
    }
}

static void countPlans(BuildContext& build, std::span<const FramePlan> plans)
{
    std::array<uint64_t, statisticsOfFramePlans.size()> sums { };
    for (const FramePlan& plan : plans) {
        for (size_t index = 0; index < sums.size(); ++index)
            sums[index] += plan.counts[index];
    }
    for (size_t index = 0; index < sums.size(); ++index)
        build.statistics().count(statisticsOfFramePlans[index], sums[index]);
}

static void applyPlans(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, std::span<const FramePlan> plans)
{
    Vector<FunctionID> changedFunctions;
    Vector<const FramePlan*> plansOfChangedFunctions;
    Vector<const FramePlan*> plansOfNewStorage;
    for (const FramePlan& plan : plans) {
        bool setsStorage = false;
        bool doesMore = !plan.emptyArgumentLists.isEmpty();
        for (const FrameDecision& decision : plan.decisions) {
            setsStorage |= decision.storage != AllocationStorage::Heap;
            doesMore |= decision.isHeldByClosureTable || decision.storage == AllocationStorage::FrameUntilMaterialized || !decision.madeAgainAt.isEmpty();
        }
        if (doesMore || (setsStorage && !plan.demotedReturnCalls.isEmpty())) {
            changedFunctions.append(plan.function);
            plansOfChangedFunctions.append(&plan);
        } else if (setsStorage)
            plansOfNewStorage.append(&plan);
    }
    forEachIndexInParallel(build.parallelLoops(), plansOfNewStorage.size(), [&](unsigned index) {
        setStorage(editor.atForStorage(plansOfNewStorage[index]->function), *plansOfNewStorage[index]);
    });
    editFunctionsInParallel(editor, build, changedFunctions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        applyFramePlan(AnalysisContext { build, editor.program(), &inputs.flow, &inputs.effects, id }, function, arenas.types, *plansOfChangedFunctions[index]);
    });
}

FrameAllocation allocateInFrames(ProgramEditor& editor, const OptimizationInputs& inputs, FlowResults& flow, BuildContext& build)
{
    RELEASE_ASSERT(&flow == &inputs.flow);
    const Program& program = editor.program();
    PhaseTimes times(build, PassID::AllocateInFrames);
    Vector<FramePlan> plans = planFunctions(program, inputs, build);
    times.didFinish("planFunctions"_s);
    planAgainWithWhatFramesHold(program, inputs, flow, build, plans);
    times.didFinish("planAgainWithWhatFramesHold"_s);
    if (build.options().collectsAllocationRemarks()) {
        for (const FramePlan& plan : plans) {
            for (const FrameDecision& decision : plan.decisions) {
                if (hasAllocationSite(program.at(plan.function).at(decision.allocation)))
                    build.addAllocationRemark(remarkOf(program.at(plan.function), decision));
            }
        }
    }

    // The edits change functions and types. The merge reads the plans and the layouts.
    FrameAllocation result;
    runTasksInParallel(build.parallelLoops(), {
        [&] { applyPlans(editor, inputs, build, plans.span()); },
        [&] {
            result = FrameAllocation(program, plans.span());
            countPlans(build, plans.span());
        },
    });
    times.didFinish("applyAndMergePlans"_s);
    build.destroyLater(WTF::move(plans));
    return result;
}

FrameAllocation::FrameAllocation(const Program& program, std::span<const FramePlan> plans)
    : m_layoutsInFrames(program.layouts.size())
    , m_layoutsOnHeap(program.layouts.size())
    , m_closuresInFrames(program.functions.size())
    , m_closuresOnHeap(program.functions.size())
{
    auto add = [](BitVector& inFrames, BitVector& onHeap, auto id, AllocationStorage storage) {
        RELEASE_ASSERT(id);
        switch (storage) {
        case AllocationStorage::Heap:
            onHeap.quickSet(id.index());
            break;
        case AllocationStorage::Frame:
            inFrames.quickSet(id.index());
            break;
        case AllocationStorage::FrameUntilMaterialized:
            inFrames.quickSet(id.index());
            onHeap.quickSet(id.index());
            break;
        }
    };
    for (const FramePlan& plan : plans) {
        for (auto [layout, storage] : plan.cells)
            add(m_layoutsInFrames, m_layoutsOnHeap, layout, storage);
        for (auto [closureFunction, storage] : plan.closures)
            add(m_closuresInFrames, m_closuresOnHeap, closureFunction, storage);
        m_fieldsHoldingFramePointers.appendVector(plan.fieldsHoldingFramePointers);
        m_capturesHoldingFramePointers.appendVector(plan.capturesHoldingFramePointers);
    }
    // The run time creates cells of every other kind.
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind != LayoutKind::Environment)
            m_layoutsOnHeap.quickSet(index);
    }
    AOT::sort(m_fieldsHoldingFramePointers);
    removeRepeatedElements(m_fieldsHoldingFramePointers);
    AOT::sort(m_capturesHoldingFramePointers);
    removeRepeatedElements(m_capturesHoldingFramePointers);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
