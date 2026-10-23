/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRepresentation.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantsComparedBitwise { Opcode::Undefined, Opcode::Null, Opcode::Boolean };

bool isDescribedToRunTime(const Program& program, const FlowResults& flow, ModuleID exporter, const ModuleExport& moduleExport)
{
    switch (moduleExport.kind) {
    case ModuleExportKind::Variable:
        switch (flow.module(exporter).namespaceUse) {
        case NamespaceUse::Unknown:
            return true;
        case NamespaceUse::NamedReadsOnly:
            // A call through the namespace object names its callee only where the object is the one receiver of the site. Beside other receivers the site reads the name at run time.
            return program.at(moduleExport.variable).flow.reads != ReadState::NeverRead;
        case NamespaceUse::NeverCreated:
            return program.at(moduleExport.variable).flow.reads == ReadState::Unknown;
        }
        RELEASE_ASSERT_NOT_REACHED();
    case ModuleExportKind::Namespace:
    case ModuleExportKind::Indirect:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector findVariablesDescribedToRunTime(const Program& program, const FlowResults& flow)
{
    BitVector described(program.variables.size());
    auto add = [&](VariableID variable) {
        if (variable)
            described.quickSet(variable.index());
    };
    for (unsigned index = 0; index < program.modules.size(); ++index) {
        auto& module = program.modules[index];
        for (auto& entry : module.exports) {
            if (isDescribedToRunTime(program, flow, ModuleID(index), entry))
                add(entry.variable);
        }
        for (auto& entry : module.syntheticExports)
            add(entry.variable);
        add(module.exportsVariable);
    }
    for (auto& scope : program.evalScopes) {
        for (auto& binding : scope.bindings)
            add(binding.variable);
    }
    for (auto& layout : program.layouts) {
        for (auto& field : layout.staticFields)
            add(field.variable);
    }
    return described;
}

// Nothing checks what arrives at an expected position, and a variable has no other place for a value that does not fit its form.
static const Type& typeOfStoredValues(const Program& program, const Variable& variable)
{
    return program.at(variable.declared.flags.contains(PositionFlags::IsExpected) ? TypeTable::anything : variable.declared.type);
}

static bool isFilledWithUndefinedValue(const Program& program, const Variable& variable)
{
    return variable.initialization == VariableInitialization::Undefined
        && variable.flow.initialization == InitializationState::Unknown
        && defaultSlotRepresentationOf(typeOfStoredValues(program, variable)) == SlotRepresentation::JSValue;
}

static bool canHoldAbsentValue(const Program& program, const Variable& variable)
{
    return !!(typeOfStoredValues(program, variable).primitives & Primitive::Absent);
}

static BitVector findZeroEncodingCandidates(const Program& program, const BitVector& described)
{
    BitVector candidates(program.variables.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (!described.quickGet(index) && isFilledWithUndefinedValue(program, variable) && !canHoldAbsentValue(program, variable))
            candidates.quickSet(index);
    }
    return candidates;
}

BitVector findZeroEncodingCandidates(const SlotPlanContext& context)
{
    return findZeroEncodingCandidates(context.program, findVariablesDescribedToRunTime(context.program, context.inputs.flow));
}

static bool isColdBlock(const Block& block)
{
    TerminatorKind kind = block.terminator.kind;
    return block.frequency != BlockFrequency::Normal
        || kind == TerminatorKind::Throw
        || kind == TerminatorKind::Rethrow
        || kind == TerminatorKind::Unreachable;
}

static bool canUseEncodedValue(const Program& program, const Function& function, const Use& use)
{
    if (isColdBlock(function.at(use.block)))
        return true;
    switch (use.kind) {
    case Use::Kind::Operand:
        break;
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    auto& user = function.at(use.user);
    if (user.opcode == Opcode::TypeTest)
        return program.at(user.testedType()).isUndefinedOrNull();
    if (user.opcode == Opcode::StrictEq) {
        uint32_t otherOperand = use.operandIndex == OperandIndex::StrictEq::left ? OperandIndex::StrictEq::right : OperandIndex::StrictEq::left;
        auto constant = constantThroughCasts(program, function, user.operand(otherOperand));
        return constant && constantsComparedBitwise.contains(function.at(*constant).opcode);
    }
    if (user.opcode == Opcode::Check)
        return use.operandIndex == OperandIndex::Check::value && !(program.at(user.type).primitives & Primitive::Undefined);
    return false;
}

void findUncheckedVariableReads(FunctionAnalyses& analyses, const BitVector& candidates, Vector<VariableID>& unchecked)
{
    auto& program = analyses.program();
    auto& function = analyses.function();
    auto& useLists = analyses.useLists();
    size_t numberBefore = unchecked.size();
    for (auto& block : function.blocks) {
        for (ValueID id : block.values) {
            auto& read = function.at(id);
            if (read.opcode != Opcode::GetModuleVariable || !candidates.quickGet(read.variable().index()))
                continue;
            bool isChecked = std::ranges::all_of(useLists.usesOf(id), [&](const Use& use) {
                return canUseEncodedValue(program, function, use);
            });
            if (!isChecked)
                unchecked.append(read.variable());
        }
    }
    auto added = unchecked.mutableSpan().subspan(numberBefore);
    AOT::sort(added);
    unchecked.shrink(unchecked.size() - std::ranges::unique(added).size());
}

static PlannedVariable planFunctionVariable(const Variable& variable)
{
    if (variable.flow.stores != StoreCount::Never)
        return { .storage = VariableStorage::Statics, .encoding = VariableEncoding::FunctionAsZero, .representation = SlotRepresentation::JSValue };
    return { .storage = VariableStorage::ClosureTable, .encoding = VariableEncoding::FunctionAsZero, .representation = SlotRepresentation::DeferredClosure };
}

static SlotRepresentation variableRepresentationOf(const SlotPlanContext& context, const Variable& variable)
{
    const Type& stored = typeOfStoredValues(context.program, variable);
    bool canObserveDeadZone = variable.initialization == VariableInitialization::Uninitialized && variable.flow.initialization == InitializationState::Unknown;
    bool isStaticField = !!variable.owner;
    if (canObserveDeadZone || isStaticField)
        return representationFor(context, stored.withPrimitives(Primitive::Absent), { });
    return representationFor(context, stored, { });
}

static PlannedVariable planVariable(const SlotPlanContext& context, const Variable& variable, const VariableUse& use, bool isDescribed, bool isZeroEncodingCandidate)
{
    if (!use.isNamedByCode && !isDescribed)
        return { .storage = VariableStorage::None };
    switch (variable.initialization) {
    case VariableInitialization::Function:
        return planFunctionVariable(variable);
    case VariableInitialization::Uninitialized:
    case VariableInitialization::Undefined:
        break;
    }
    // The page of a variable that reads zero as undefined is written only when the program stores, which is better than four bytes that are filled at program entry.
    bool readsZeroAsUndefined = isZeroEncodingCandidate && !use.hasUncheckedRead;
    return {
        .storage = VariableStorage::Statics,
        .encoding = readsZeroAsUndefined ? VariableEncoding::UndefinedAsZero : VariableEncoding::Plain,
        .representation = readsZeroAsUndefined ? SlotRepresentation::JSValue : variableRepresentationOf(context, variable),
        .isUsedBeforeProgramEntry = use.isUsedBeforeProgramEntry || context.program.at(variable.module).kind == ModuleKind::RuntimeSource,
    };
}

Vector<PlannedVariable> planVariables(const SlotPlanContext& context, const SlotUses& uses)
{
    const Program& program = context.program;
    const BitVector described = findVariablesDescribedToRunTime(program, context.inputs.flow);
    const BitVector candidates = findZeroEncodingCandidates(program, described);
    Vector<PlannedVariable> variables(program.variables.size());
    forEachIndexInParallel(context.build.parallelLoops(), program.variables.size(), [&](unsigned index) {
        variables[index] = planVariable(context, program.variables[index], uses.variables[index], described.quickGet(index), candidates.quickGet(index));
    });
    return variables;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
