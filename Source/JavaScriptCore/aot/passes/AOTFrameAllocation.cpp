/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameAllocation.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFrameAllocationMaterialization.h"
#include "AOTFrameAllocationPlanner.h"
#include "AOTFunctionEditing.h"
#include "AOTPass.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(Value::opcodesWithStorage == OpcodeSet { Opcode::NewClosure, Opcode::NewObject });

static bool hasAllocationSite(const Value& value)
{
    return Value::opcodesWithAllocationSite.contains(value.opcode) && value.allocationSite();
}

static BitVector findAllocations(const Function& function, bool collectsRemarks)
{
    BitVector allocations;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (hasStorageChoice(value) || (collectsRemarks && hasAllocationSite(value)))
                allocations.ensureSizeAndSet(id.index(), function.values.size());
        }
    }
    return allocations;
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
        if (!hasStorageChoice(value))
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

static FramePlan planFunction(const Program& program, const OptimizationInputs& inputs, BuildContext& build, FunctionID id)
{
    const Function& function = program.at(id);
    BitVector allocations = findAllocations(function, build.options().collectsAllocationRemarks());
    if (allocations.isEmpty())
        return { };
    if (!build.isEnabled(PassID::AllocateInFrames, id))
        return createHeapPlan(program, function, allocations);

    FunctionAnalyses analyses(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function);
    return FramePlanner(analyses).run();
}

bool FramePlan::changesFunction() const
{
    return decisions.containsIf([](const FrameDecision& decision) {
        return decision.storage != AllocationStorage::Heap;
    });
}

void applyFramePlan(const AnalysisContext& context, Function& function, TypeArena& types, const FramePlan& plan)
{
    BitVector frameClosures(function.values.size());
    Vector<ValueID, 4> environmentsMaterializedLate;
    for (const FrameDecision& decision : plan.decisions) {
        Value& allocation = function.at(decision.allocation);
        switch (decision.storage) {
        case AllocationStorage::Heap:
            continue;
        case AllocationStorage::Frame:
            if (allocation.opcode == Opcode::NewClosure)
                frameClosures.quickSet(decision.allocation.index());
            break;
        case AllocationStorage::FrameUntilMaterialized:
            environmentsMaterializedLate.append(decision.allocation);
            break;
        }
        allocation.setStorage(decision.storage);
    }
    if (environmentsMaterializedLate.isEmpty())
        return;

    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
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

static void addAllocationRemarks(const Program& program, BuildContext& build, std::span<const FramePlan> plans)
{
    if (!build.options().collectsAllocationRemarks())
        return;
    for (unsigned index = 0; index < plans.size(); ++index) {
        const Function& function = program.functions[index];
        for (const FrameDecision& decision : plans[index].decisions) {
            if (hasAllocationSite(function.at(decision.allocation)))
                build.addAllocationRemark(remarkOf(function, decision));
        }
    }
}

static void countPlans(BuildContext& build, std::span<const FramePlan> plans)
{
    uint64_t closuresInFrames = 0;
    uint64_t environmentsInFrames = 0;
    uint64_t environmentsOnHeap = 0;
    uint64_t environmentsMaterializedLate = 0;
    uint64_t objectsInFrames = 0;
    for (const FramePlan& plan : plans) {
        closuresInFrames += plan.closuresInFrames;
        environmentsInFrames += plan.environmentsInFrames;
        environmentsOnHeap += plan.environmentsOnHeap;
        environmentsMaterializedLate += plan.environmentsMaterializedLate;
        objectsInFrames += plan.objectsInFrames;
    }
    build.statistics().count(StatisticID::ClosuresInFrames, closuresInFrames);
    build.statistics().count(StatisticID::EnvironmentsInFrames, environmentsInFrames);
    build.statistics().count(StatisticID::EnvironmentsOnHeap, environmentsOnHeap);
    build.statistics().count(StatisticID::EnvironmentsMaterializedLate, environmentsMaterializedLate);
    build.statistics().count(StatisticID::ObjectsInFrames, objectsInFrames);
}

static void addCellsCreatedByRuntime(const Program& program, FrameAllocation& result)
{
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind != LayoutKind::Environment)
            result.addCell(LayoutID(index), AllocationStorage::Heap);
    }
}

FrameAllocation allocateInFrames(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build)
{
    const Program& program = editor.program();
    Vector<FramePlan> plans(program.functions.size());
    forEachIndexInParallel(build.parallelLoops(), plans.size(), [&](unsigned index) {
        plans[index] = planFunction(program, inputs, build, FunctionID(index));
    });

    FrameAllocation result;
    Vector<FunctionID> changedFunctions;
    for (unsigned index = 0; index < plans.size(); ++index) {
        result.merge(plans[index].allocation);
        if (plans[index].changesFunction())
            changedFunctions.append(FunctionID(index));
    }
    addCellsCreatedByRuntime(program, result);
    result.finish();
    countPlans(build, plans.span());
    addAllocationRemarks(program, build, plans.span());

    editFunctionsInParallel(editor, build, changedFunctions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        applyFramePlan(AnalysisContext { build, program, &inputs.flow, &inputs.effects, id }, function, arenas.types, plans[id.index()]);
    });
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
    std::ranges::sort(m_fieldsHoldingFramePointers);
    removeRepeatedElements(m_fieldsHoldingFramePointers);
    std::ranges::sort(m_capturesHoldingFramePointers);
    removeRepeatedElements(m_capturesHoldingFramePointers);
    m_isFinished = true;
}

static void dumpStorage(PrintStream& out, ASCIILiteral prefix, const BitVector& inFrames, const BitVector& onHeap)
{
    for (size_t index : inFrames) {
        if (onHeap.get(index))
            out.println(prefix, index, " may be in a frame");
        else
            out.println(prefix, index, " only in frames");
    }
}

void FrameAllocation::dump(PrintStream& out, const Program&) const
{
    RELEASE_ASSERT(m_isFinished);
    if (m_layoutsInFrames.isEmpty() && m_closuresInFrames.isEmpty()) {
        out.println("nothing in frames");
        return;
    }
    dumpStorage(out, "L"_s, m_layoutsInFrames, m_layoutsOnHeap);
    dumpStorage(out, "f"_s, m_closuresInFrames, m_closuresOnHeap);
    for (FieldReference field : m_fieldsHoldingFramePointers)
        out.println("L", field.layout.index(), ".", field.field, " holds frame pointers");
    for (CaptureReference capture : m_capturesHoldingFramePointers)
        out.println("f", capture.function.index(), " capture ", capture.capture, " holds frame pointers");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
