/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTIR.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr Effects ropeResolution = declaredBoundOf(NamedEffectSet::ResolvesRopes);
static constexpr Effects creationFromTables = [] {
    Effects effects = declaredBoundOf(NamedEffectSet::Allocates);
    effects.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
    return effects;
}();

void EffectDescription::addThrow()
{
    own.normal.mayThrow = true;
    own.handler.mayThrow = true;
}

static void insertAscending(Vector<FunctionID, 2>& functions, FunctionID id)
{
    RELEASE_ASSERT(id);
    size_t position = std::ranges::lower_bound(functions, id) - functions.begin();
    if (position < functions.size() && functions[position] == id)
        return;
    functions.insert(position, id);
}

void EffectDescription::addFunction(FunctionID id)
{
    insertAscending(functions, id);
}

void EffectDescription::addSlowPathFunction(FunctionID id)
{
    insertAscending(slowPathFunctions, id);
}

void describeBound(const Effects& declared, EffectDescription& description)
{
    if (!declared.mayCallUnknownCode) {
        description.addOwn(declared);
        return;
    }
    description.addUnknownCode();
    if (!declared.maySuspend)
        return;
    description.addThrow();
    description.own.normal.maySuspend = true;
    description.own.handler.maySuspend = true;
}

static void describeOperands(const Program& program, const Function& function, const Instruction& instruction, NOESCAPE const Invocable<OperandBehavior(uint32_t)> auto& behaviorAt, EffectDescription& description)
{
    instruction.forEachOperand([&](ValueID operand, uint32_t index) {
        describeOperandCode(program, program.typeOf(function, operand), behaviorAt(index), description);
    });
}

static bool castCannotFail(const Program& program, const Function& function, const Value& value)
{
    if (!value.type)
        return false;
    const Type& result = program.at(value.type);
    bool cannotFail = false;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        if (operandUsesOf(value.opcode, index).contains(OperandUse::Returns))
            cannotFail = program.typeOf(function, operand).isSubsetOf(result, program);
    });
    return cannotFail;
}

static bool reportsNothing(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::ReportMismatch && program.typeOf(function, value.valueOperand()).isSubsetOf(program.at(value.expectedType()), program);
}

static bool validatesNothing(const Program& program, const Function& function, const Value& value)
{
    switch (value.opcode) {
    case Opcode::ValidateType:
        return program.typeOf(function, value.valueOperand()).isSubsetOf(program.at(value.validatedType()), program);
    case Opcode::ValidateCallee:
        return program.typeOf(function, value.calleeOperand()).isSubsetOf(program.at(value.validatedType()), program);
    case Opcode::ValidateSameValue:
        return value.keptOperand() == value.replacementOperand();
    default:
        return false;
    }
}

static bool readsOnlyFlatStrings(const Program& program, const Function& function, const Value& value)
{
    bool readsString = false;
    bool mayReadRope = false;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        if (operandKindOf(value.opcode, index) != OperandKind::String)
            return;
        readsString = true;
        mayReadRope |= !program.typeOf(function, operand).isOpaque(OpaqueKind::FlatString);
    });
    return readsString && !mayReadRope;
}

static bool resolvesNoRope(const Program& program, const Function& function, const Value& value, const Effects& declared)
{
    return declared == ropeResolution && readsOnlyFlatStrings(program, function, value);
}

// The importer is linked, so the module is. Its namespace object is made from the tables of the image where they name every export. A module that takes names
// from a module of another kind asks that module, a deferred import is evaluated by the first read, and neither a CommonJS module nor uncompiled code has a table.
static bool createsNamespaceFromTables(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::ModuleNamespace || value.phase() != ImportPhase::Evaluation)
        return false;
    const Module& module = program.at(value.module());
    return module.kind == ModuleKind::SourceText && module.exportRule == SyntheticExportRule::None
        && std::ranges::none_of(module.requests, &ModuleRequest::isDynamicStarExport);
}

static Effects refinedEffectsOf(const Program& program, const Function& function, const Value& value, const Effects& declared)
{
    if (createsNamespaceFromTables(program, value))
        return creationFromTables;
    Effects effects = declared;
    if (resolvesNoRope(program, function, value, declared)) {
        effects = Effects::none();
        effects.reads = declared.reads;
    }
    if (std::optional<unsigned> position = OperandPosition::position(value.opcode)) {
        bool isInsideLength = Value::opcodesWithIsInBounds.contains(value.opcode) && value.isInBounds();
        if (ValueID operand = value.operand(*position))
            refineElementEffects(value.opcode, program.typeOf(function, operand), isInsideLength ? ElementPosition::InsideLength : ElementPosition::Anywhere, effects);
    }
    return effects;
}

void describeDeclaredEntry(const Program& program, const Function& function, const Value& value, const Effects& declared, EffectDescription& description)
{
    if (resultRuleOf(value.opcode) == ResultRule::Cast && castCannotFail(program, function, value))
        return;
    if (reportsNothing(program, function, value) || validatesNothing(program, function, value))
        return;
    describeBound(refinedEffectsOf(program, function, value, declared), description);
    if (declared.mayCallUnknownCode)
        return;
    describeOperands(program, function, value, [&](uint32_t index) {
        return declaredBehaviorOf(value.opcode, index);
    }, description);
}

EffectDescription describeEffects(const Program& program, const Function& function, const Value& value)
{
    Effects declared = declaredEffectsOf(program, value);
    EffectDescription description;
    switch (effectRuleOf(value.opcode)) {
    case EffectRule::Declared:
        describeDeclaredEntry(program, function, value, declared, description);
        break;
    case EffectRule::Operator:
        describeOperator(program, function, value, declared, description);
        break;
    case EffectRule::Access:
        describeAccess(program, function, value, declared, description);
        break;
    case EffectRule::Call:
        describeCall(program, function, value, declared, description);
        break;
    case EffectRule::Builtin:
        describeBuiltinCall(program, function, value, declared, description);
        break;
    case EffectRule::Inline:
        describeInlineBuiltin(program, function, value, declared, description);
        break;
    }
    return description;
}

EffectDescription describeEffects(const Program& program, const Function& function, const Terminator& terminator)
{
    Effects declared = declaredEffectsOf(program, terminator);
    EffectDescription description;
    switch (effectRuleOf(terminator.kind)) {
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
    case EffectRule::Builtin:
        describeBound(declared, description);
        if (declared.mayCallUnknownCode)
            break;
        describeOperands(program, function, terminator, [&](uint32_t index) {
            return declaredBehaviorOf(terminator.kind, index);
        }, description);
        break;
    case EffectRule::Inline:
        describeInlineBuiltin(program, function, terminator, declared, description);
        break;
    }
    return description;
}

EffectsOnEdges resolveEffects(const EffectContext& context, const EffectDescription& description)
{
    EffectsOnEdges result = description.own;
    for (FunctionID function : description.functions)
        result.merge(CalleeSummary(context, function)->onCall());
    // While a frame is suspended its callers run on, and what they store into their own allocations is in no summary.
    if (description.own.normal.maySuspend)
        result.merge(EffectsOnEdges::unknownCode());
    else if (description.callsUnknownCode)
        result.merge(EffectsOnEdges::onBothEdges(context.summaries.unknownCode()));
    if (result.mayThrow())
        result.handler.merge(result.normal);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
