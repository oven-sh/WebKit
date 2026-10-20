/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPromoteEnvironmentsToVariables.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTPromoteEnvironmentsToVariablesPlan.h"
#include "AOTRemoveUnusedParameters.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class FunctionRewriter {
public:
    FunctionRewriter(const PromotionPlan& plan, IRBuilder& builder, FunctionID id)
        : m_plan(plan)
        , m_builder(builder)
        , m_function(builder.function())
        , m_id(id)
        , m_storedValues(plan.storedValuesIn(id))
    {
    }

    void run();

private:
    bool isPromotedEnvironment(ValueID) const;

    void rewriteValues(BlockID);
    void appendStore(BlockValues&, VariableID, ValueID stored);
    void appendCapturedValueStore(BlockValues&, ValueID);
    bool replaceAllocation(ValueID, BlockValues&);
    void rewriteRead(Value&);
    void rewriteStore(Value&);
    void removeCapturedOperands(Value&);
    void removeBlockParameters(BlockID);
    void replaceCaptures();

    const PromotionPlan& m_plan;
    IRBuilder& m_builder;
    Function& m_function;
    FunctionID m_id;
    std::span<const StoredValue> m_storedValues;
};

} // anonymous namespace

static bool isCaughtValue(Opcode opcode)
{
    return opcode == Opcode::CatchValue || opcode == Opcode::CatchException;
}

bool FunctionRewriter::isPromotedEnvironment(ValueID id) const
{
    LayoutID environment = m_builder.context().program.typeOf(m_function, id).environment;
    return environment && m_plan.isPromoted(environment);
}

void FunctionRewriter::appendStore(BlockValues& values, VariableID variable, ValueID stored)
{
    Value store = IRBuilder::make(Opcode::PutModuleVariable, { stored });
    store.setVariable(variable);
    values.append(m_builder.addDetached(WTF::move(store)));
}

void FunctionRewriter::appendCapturedValueStore(BlockValues& values, ValueID id)
{
    auto stored = std::ranges::lower_bound(m_storedValues, id, { }, &StoredValue::value);
    if (stored == m_storedValues.end() || stored->value != id)
        return;
    m_builder.setOrigin(m_function.at(id).origin());
    appendStore(values, stored->variable, id);
}

bool FunctionRewriter::replaceAllocation(ValueID id, BlockValues& values)
{
    const Value& allocation = m_function.at(id);
    LayoutID layout = allocation.layout();
    if (!m_plan.isPromoted(layout))
        return false;

    BuildContext& build = m_builder.context().build;
    if (build.options().collectsAllocationRemarks() && allocation.allocationSite())
        build.addAllocationRemark({ .site = allocation.allocationSite(), .location = m_function.locationOf(id), .outcome = AllocationOutcome::Removed });

    Vector<uint32_t, 8> fields(allocation.fields());
    Vector<ValueID, 8> initialValues(allocation.valuesOperands());
    m_builder.setOrigin(allocation.origin());
    for (size_t index = 0; index < fields.size(); ++index)
        appendStore(values, m_plan.variableOf(layout, fields[index]), initialValues[index]);
    return true;
}

void FunctionRewriter::rewriteRead(Value& read)
{
    if (!m_plan.isPromoted(read.layout()))
        return;
    VariableID variable = m_plan.variableOf(read.layout(), read.field());
    read.become(Opcode::GetModuleVariable);
    read.setVariable(variable);
}

void FunctionRewriter::rewriteStore(Value& store)
{
    if (!m_plan.isPromoted(store.layout()))
        return;
    VariableID variable = m_plan.variableOf(store.layout(), store.field());
    ValueID stored = store.valueOperand();
    store.become(Opcode::PutModuleVariable);
    store.setVariable(variable);
    store.setValueOperand(stored);
}

void FunctionRewriter::removeCapturedOperands(Value& supplier)
{
    std::span<const PlannedCapture> planned = m_plan.capturesOf(supplier.function());
    for (uint32_t capture = planned.size(); capture--;) {
        if (planned[capture].change != CaptureChange::None)
            supplier.removeVariadicOperand(capture);
    }
}

void FunctionRewriter::rewriteValues(BlockID block)
{
    BlockValues values = std::exchange(m_function.at(block).values, { });
    uint32_t numberOfCaughtValues = 0;
    while (numberOfCaughtValues < values.size() && isCaughtValue(m_function.at(values[numberOfCaughtValues]).opcode))
        ++numberOfCaughtValues;

    BlockValues rewritten;
    rewritten.reserveInitialCapacity(values.size());
    for (uint32_t index = 0; index <= values.size(); ++index) {
        if (index == numberOfCaughtValues) {
            Vector<ValueID, 8> parameters(m_function.at(block).parameters.span());
            for (ValueID parameter : parameters)
                appendCapturedValueStore(rewritten, parameter);
        }
        if (index == values.size())
            break;

        ValueID id = values[index];
        Opcode opcode = m_function.at(id).opcode;
        if (opcode == Opcode::NewObject && replaceAllocation(id, rewritten))
            continue;
        if (opcode == Opcode::GetEnvironmentField)
            rewriteRead(m_function.at(id));
        else if (opcode == Opcode::PutEnvironmentField)
            rewriteStore(m_function.at(id));
        else if (opcode == Opcode::NewClosure || opcode == Opcode::CallDirect)
            removeCapturedOperands(m_function.at(id));
        rewritten.append(id);
        appendCapturedValueStore(rewritten, id);
    }
    m_function.at(block).values = WTF::move(rewritten);
}

void FunctionRewriter::removeBlockParameters(BlockID block)
{
    const auto& parameters = m_function.at(block).parameters;
    BitVector removed(parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (isPromotedEnvironment(parameters[index]))
            removed.quickSet(index);
    }
    m_builder.removeParameters(block, removed);
}

void FunctionRewriter::replaceCaptures()
{
    std::span<const PlannedCapture> planned = m_plan.capturesOf(m_id);
    if (planned.empty())
        return;

    BlockValues values;
    Vector<Value, 4> reads;
    const auto& parameters = m_function.at(entryBlock).parameters;
    for (size_t capture = 0; capture < planned.size(); ++capture) {
        if (!planned[capture].variable)
            continue;
        Value read = m_function.at(parameters[capture]);
        read.become(Opcode::GetModuleVariable);
        read.setVariable(planned[capture].variable);
        values.append(parameters[capture]);
        reads.append(WTF::move(read));
    }

    removeEntryParameters(m_function, m_plan.removedCapturesOf(m_id));
    for (size_t index = 0; index < reads.size(); ++index)
        m_function.at(values[index]) = WTF::move(reads[index]);
    values.appendVector(m_function.at(entryBlock).values);
    m_function.at(entryBlock).values = WTF::move(values);
}

void FunctionRewriter::run()
{
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index)
        rewriteValues(BlockID(index));
    for (uint32_t index = 1; index < m_function.blocks.size(); ++index)
        removeBlockParameters(BlockID(index));
    replaceCaptures();
}

static void renumberParameterOrigins(Function& function, const BitVector& removedFromSource)
{
    for (ParameterOrigin& origin : function.derivation.parameterMap) {
        if (!origin.original)
            continue;
        uint32_t numberOfRemovedBefore = 0;
        for (size_t removed : removedFromSource)
            numberOfRemovedBefore += removed < *origin.original;
        origin.original = *origin.original - numberOfRemovedBefore;
    }
}

static void rewriteFunction(const PromotionPlan& plan, const AnalysisContext& context, Function& function, TypeArena& types)
{
    if (function.hasBody()) {
        IRBuilder builder(context, function, types);
        FunctionRewriter(plan, builder, context.id).run();
    } else
        removeEntryParameters(function, plan.removedCapturesOf(context.id));
    renumberParameterOrigins(function, plan.removedCapturesOf(context.id));
}

void promoteEnvironmentsToVariables(ProgramEditor& editor, FlowResults& flow, BuildContext& build)
{
    if (!flow.hasResults())
        return;

    const Program& program = editor.program();
    PromotionPlan plan = planEnvironmentPromotion(program, flow, build);
    if (plan.isEmpty())
        return;

    for (Variable& variable : plan.variables)
        editor.addVariable(WTF::move(variable));
    editFunctionsInParallel(editor, build, plan.editedFunctions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteFunction(plan, AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
    });

    for (FunctionID function : plan.editedFunctions)
        flow.removeEntryParameters(function, plan.removedCapturesOf(function));
    for (size_t layout : plan.promotedLayouts)
        editor.at(LayoutID(layout)).flow.allocation = AllocationState::NeverAllocated;

    build.statistics().count(StatisticID::EnvironmentsPromotedToVariables, plan.promotedLayouts.bitCount());
    build.statistics().count(StatisticID::CapturesPromotedToVariables, plan.numberOfPromotedCaptures);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
