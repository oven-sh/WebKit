/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

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
    if (functions.isEmpty() || functions.last() < id) {
        functions.append(id);
        return;
    }
    size_t position = std::ranges::lower_bound(functions, id) - functions.begin();
    if (position < functions.size() && functions[position] == id)
        return;
    functions.insert(position, id);
}

static size_t countMissing(std::span<const FunctionID> functions, std::span<const FunctionID> added)
{
    size_t numberOfMissing = 0;
    size_t index = 0;
    for (FunctionID id : added) {
        while (index < functions.size() && functions[index] < id)
            ++index;
        if (index == functions.size() || functions[index] != id)
            ++numberOfMissing;
    }
    return numberOfMissing;
}

static void uniteAscending(Vector<FunctionID, 2>& functions, std::span<const FunctionID> added)
{
    if (added.empty())
        return;
    RELEASE_ASSERT(added.front() && added.back());
    if (functions.isEmpty() || functions.last() < added.front()) {
        functions.append(added);
        return;
    }

    size_t numberOfMissing = countMissing(functions.span(), added);
    if (!numberOfMissing)
        return;

    size_t remaining = functions.size();
    size_t remainingAdded = added.size();
    size_t end = remaining + numberOfMissing;
    functions.grow(end);
    while (remainingAdded) {
        if (remaining && added[remainingAdded - 1] < functions[remaining - 1]) {
            functions[--end] = functions[--remaining];
            continue;
        }
        if (remaining && functions[remaining - 1] == added[remainingAdded - 1])
            --remaining;
        functions[--end] = added[--remainingAdded];
    }
    RELEASE_ASSERT(end == remaining);
}

void EffectDescription::addFunction(FunctionID id)
{
    insertAscending(functions, id);
}

void EffectDescription::addFunctions(std::span<const FunctionID> added)
{
    bool isAscending = std::ranges::adjacent_find(added, std::ranges::greater_equal { }) == added.end();
    if (isAscending) {
        uniteAscending(functions, added);
        return;
    }
    for (FunctionID id : added)
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

static ValueID returnedOperand(const Value& value)
{
    ValueID returned;
    value.forEachOperand([&](ValueID operand, uint32_t index) {
        if (operandUsesOf(value.opcode, index).contains(OperandUse::Returns))
            returned = operand;
    });
    return returned;
}

static bool castCannotFail(const Program& program, const Function& function, const Value& value)
{
    if (!value.type)
        return false;
    ValueID source = returnedOperand(value);
    return source && program.typeOf(function, source).isSubsetOf(program.at(value.type), program);
}

static bool reportsNothing(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::ReportMismatch && program.typeOf(function, value.valueOperand()).isSubsetOf(program.at(value.expectedType()), program);
}

static constexpr OpcodeSet checkedCasts { Opcode::Check, Opcode::AssumeType };

static bool typeDependsOnPosition(const Program& program, const Function& function, ValueID id, const Type& validated)
{
    while (id) {
        const Value& definition = function.at(id);
        switch (resultRuleOf(definition.opcode)) {
        case ResultRule::Constant:
        case ResultRule::Allocation:
            return false;
        case ResultRule::Declared:
            return definition.opcode != Opcode::Parameter || function.at(entryBlock).parameters.contains(id);
        case ResultRule::None:
        case ResultRule::Unknown:
        case ResultRule::Results:
            return true;
        case ResultRule::Cast:
            if (checkedCasts.contains(definition.opcode) && program.at(definition.castType()).isSubsetOf(validated, program))
                return false;
            break;
        case ResultRule::Natural:
            break;
        }
        id = returnedOperand(definition);
    }
    return false;
}

static bool isRedundantValidation(const Program& program, const Function& function, ValueID operand, TypeID validatedType)
{
    const Type& validated = program.at(validatedType);
    return program.typeOf(function, operand).isSubsetOf(validated, program) && !typeDependsOnPosition(program, function, operand, validated);
}

static bool validatesNothing(const Program& program, const Function& function, const Value& value)
{
    switch (value.opcode) {
    case Opcode::ValidateType:
        return isRedundantValidation(program, function, value.valueOperand(), value.validatedType());
    case Opcode::ValidateCallee:
        return isRedundantValidation(program, function, value.calleeOperand(), value.validatedType());
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
    return isBoundOf(NamedEffectSet::ResolvesRopes, declared) && readsOnlyFlatStrings(program, function, value);
}

static bool createsNamespaceFromTables(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::ModuleNamespace || value.phase() != ImportPhase::Evaluation)
        return false;
    const Module& module = program.at(value.module());
    return module.kind == ModuleKind::SourceText && module.exportRule == SyntheticExportRule::None
        && std::ranges::none_of(module.requests, &ModuleRequest::isDynamicStarExport);
}

static bool readsPrototypePropertyWithoutRunningCode(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::GetConstructPrototype)
        return false;
    const Type& newTarget = program.typeOf(function, value.newTargetOperand());
    OperandBehavior behavior = declaredBehaviorOf(value.opcode, OperandIndex::GetConstructPrototype::newTarget);
    return newTarget.isObject() && membersRunBy(program, newTarget, behavior.protocols, behavior.namedMembers, behavior.calledMembers, behavior.namedMemberUse).runsNothing();
}

static bool joinsTextsOfPrimitives(const Program& program, const Function& function, const Value& value)
{
    return value.opcode == Opcode::Strcat && std::ranges::all_of(value.partsOperands(), [&](ValueID part) {
        return hasTextByItself(program.typeOf(function, part));
    });
}

static Effects refinedEffectsOf(const Program& program, const Function& function, const Value& value, const Effects& declared)
{
    if (createsNamespaceFromTables(program, value))
        return creationFromTables;
    if (std::optional<Effects> lookup = effectsOfLookupFromBase(program, function, value))
        return *lookup;
    Effects effects = declared;
    if (readsPrototypePropertyWithoutRunningCode(program, function, value) || joinsTextsOfPrimitives(program, function, value))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    if (resolvesNoRope(program, function, value, declared)) {
        effects = Effects::none();
        effects.reads = declared.reads;
    }
    if (std::optional<unsigned> position = OperandPosition::position(value.opcode)) {
        bool isInsideLength = (Value::opcodesWithIsInBounds.contains(value.opcode) && value.isInBounds()) || (Value::opcodesWithIsPresent.contains(value.opcode) && value.isPresent());
        if (ValueID operand = value.operand(*position))
            refineElementEffects(value.opcode, program.typeOf(function, operand), isInsideLength ? ElementPosition::InsideLength : ElementPosition::Anywhere, effects);
    }
    return effects;
}

static void describePartOfStringify(const Program& program, const Function& function, const Value& value, EffectDescription& description)
{
    describeOverload(program, program.builtins().overloadIDsOf(Builtins::JSONStringify).at(0), program.typeOf(function, value.valueOperand()), description);
    description.addOwn(declaredBoundOf(Opcode::JSONAppendValue));
}

void describeDeclaredEntry(const Program& program, const Function& function, const Value& value, const Effects& declared, EffectDescription& description)
{
    if (value.opcode == Opcode::StringBuilderAppendJSON) {
        describePartOfStringify(program, function, value, description);
        return;
    }
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

bool isRefinedByValue(Opcode opcode, const Effects& bound)
{
    return resultRuleOf(opcode) == ResultRule::Cast
        || familyOf(opcode) == OperationFamily::RuntimeValidation
        || opcode == Opcode::ReportMismatch
        || opcode == Opcode::ModuleNamespace
        || opcode == Opcode::GetConstructPrototype
        || opcode == Opcode::StringBuilderAppendJSON
        || opcode == Opcode::Strcat
        || opcode == Opcode::SuperGet
        || isBoundOf(NamedEffectSet::ResolvesRopes, bound)
        || OperandPosition::position(opcode);
}

static bool isDescribedByRowAlone(Opcode opcode)
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (Opcode member : allMembersOf<Opcode>) {
            if (effectRuleOf(member) == EffectRule::Declared && !isRefinedByValue(member, declaredBoundOf(member)) && !mayRunOperandCode(member))
                result.add(member);
        }
        return result;
    }();
    return opcodes.contains(opcode);
}

static void describeEffectsInto(const Program& program, const Function& function, const Value& value, EffectDescription& description)
{
    Effects declared = declaredEffectsOf(program, value);
    if (isDescribedByRowAlone(value.opcode)) {
        describeBound(declared, description);
        return;
    }
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
}

static void describeEffectsInto(const Program& program, const Function& function, const Terminator& terminator, EffectDescription& description)
{
    Effects declared = declaredEffectsOf(program, terminator);
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
}

EffectDescription describeEffects(const Program& program, const Function& function, const Value& value)
{
    EffectDescription description;
    describeEffectsInto(program, function, value, description);
    return description;
}

EffectDescription describeEffects(const Program& program, const Function& function, const Terminator& terminator)
{
    EffectDescription description;
    describeEffectsInto(program, function, terminator, description);
    return description;
}

EffectDescription describeCodeRunBy(const Program& program, const Function& function, const Value& value)
{
    EffectDescription description;
    description.leavesOutOwnEffects = true;
    describeEffectsInto(program, function, value, description);
    return description;
}

EffectDescription describeCodeRunBy(const Program& program, const Function& function, const Terminator& terminator)
{
    EffectDescription description;
    description.leavesOutOwnEffects = true;
    describeEffectsInto(program, function, terminator, description);
    return description;
}

EffectsOnEdges resolveEffects(const EffectContext& context, const EffectDescription& description)
{
    EffectsOnEdges result = description.own;
    bool namesFunctionWithoutBody = false;
    for (FunctionID function : description.functions) {
        if (context.program.at(function).hasBody())
            result.merge(CalleeSummary(context, function)->onCall());
        else
            namesFunctionWithoutBody = true;
    }
    static_assert(Effects::unknownCode().mayThrow);
    if (description.own.normal.maySuspend)
        result.normal.merge(Effects::unknownCode());
    else if (description.callsUnknownCode || namesFunctionWithoutBody)
        result.normal.merge(context.summaries.unknownCode());
    if (result.mayThrow())
        result.handler.merge(result.normal);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
