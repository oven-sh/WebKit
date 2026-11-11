/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"

namespace JSC { namespace AOT { namespace Typed {

static ALWAYS_INLINE TypeID typeByConstructionOfEntryParameter(const TypingContext& context, const Function& callee, unsigned entryParameterIndex)
{
    switch (callee.entryParameterKind(entryParameterIndex)) {
    case EntryParameter::Capture:
    case EntryParameter::This:
    case EntryParameter::Parameter:
        return context.typeIDOf(*callee.declaredEntryParameter(entryParameterIndex));
    case EntryParameter::Callee:
        return TypeTable::function;
    case EntryParameter::NewTarget:
        return TypeTable::anything;
    case EntryParameter::ArgumentCount:
        return TypeTable::uint32;
    case EntryParameter::HiddenRest:
        return context.types.builtinClass(BuiltinClasses::Array);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE TypeID findDeclaredEntryType(const TypingContext& context, FunctionID callee, unsigned entryParameterIndex, TypeID known)
{
    TypeID byConstruction = typeByConstructionOfEntryParameter(context, context.program.at(callee), entryParameterIndex);
    if (!byConstruction || !known)
        return byConstruction ? byConstruction : known;

    if (!context.at(byConstruction).isJavaScriptValue())
        return byConstruction;

    if (isNeverType(known))
        return known;

    TypeID both = context.types.intersectionOf(byConstruction, known);
    return isNeverType(both) ? byConstruction : both;
}

TypeID declaredEntryTypeOf(const TypingContext& context, FunctionID callee, unsigned entryParameterIndex)
{
    TypeID known = context.positions.entryParameter(context.level, callee, entryParameterIndex);
    if (context.level != TypeLevel::Proven || !context.believesWhatFrontEndHasEstablished)
        return findDeclaredEntryType(context, callee, entryParameterIndex, known);
    TypeArena::TypeTakenAtEntry& last = context.types.typeTakenLastAtEntry(callee.index(), entryParameterIndex);
    if (last.function == callee.index() && last.entryParameter == entryParameterIndex && last.known == known.bits() && last.epoch == context.types.epoch())
        return last.taken;
    TypeID taken = findDeclaredEntryType(context, callee, entryParameterIndex, known);
    last = { callee.index(), entryParameterIndex, known.bits(), context.types.epoch(), taken };
    return taken;
}

static TypeID requiredTypeOfEstablishedPosition(const TypingContext& context, const DeclaredType& declared)
{
    return declared.isEstablished() ? context.typeIDOf(declared) : TypeID();
}

static TypeID requiredTypeOfModuleVariable(const TypingContext& context, VariableID variable)
{
    if (variable.index() >= context.program.variables.size())
        return { };
    TypeID established = requiredTypeOfEstablishedPosition(context, context.program.at(variable).declared);
    if (!established || !context.at(established).isJavaScriptValue())
        return established;
    TypeID known = context.positions.variable(context.level, variable);
    return known ? context.types.unionOf(established, known) : established;
}

static TypeID requiredTypeOfEnvironmentField(const TypingContext& context, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = context.program.at(layout).fields;
    if (field >= fields.size())
        return { };
    return requiredTypeOfEstablishedPosition(context, fields[field].declared);
}

static TypeID requiredTypeOfCapture(const TypingContext& context, FunctionID function, uint32_t captureIndex)
{
    const Function& callee = context.program.at(function);
    if (captureIndex >= callee.captures.size() || context.positions.entryParameterIsLeftOutOfCalls(function, captureIndex))
        return { };
    return requiredTypeOfEstablishedPosition(context, callee.captures[captureIndex]);
}

static TypeID requiredTypeOfPrivateDefinition(const TypingContext& context, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = context.program.at(layout).fields;
    if (field >= fields.size())
        return { };
    return fields[field].flags.contains(FieldFlags::IsBrand) ? context.typeIDOf(fields[field].declared) : TypeTable::anything;
}

static ALWAYS_INLINE TypeID requiredTypeOfInitialValue(const TypingContext& context, LayoutID layout, uint32_t field)
{
    const Layout& allocated = context.program.at(layout);
    if (allocated.isEnvironment())
        return requiredTypeOfEnvironmentField(context, layout, field);
    if (TypeID held = context.positions.slot(context.level, layout, field); held && !isNeverType(held))
        return held;
    if (field >= allocated.fields.size())
        return { };
    return tryTypeAcceptedBySlots(context, setOfOne<LayoutSetID>(layout), allocated.fields[field].key);
}

static TypeID requiredTypeOfIteratorResultField(const TypingContext& context, uint32_t operandIndex)
{
    const Program& program = context.program;
    BuiltinName name = operandIndex == OperandIndex::CreateIteratorResult::value ? BuiltinNames::value : BuiltinNames::done;
    return tryTypeAcceptedBySlots(context, setOfOne<LayoutSetID>(program.layoutOf(BuiltinLayouts::IteratorResult)), program.keyOf(name));
}

static TypeID requiredTypeOfElementInStorage(ArrayStorage named)
{
    switch (named) {
    case ArrayStorage::Int32:
        return TypeTable::int32;
    case ArrayStorage::Double:
        return TypeTable::number;
    case ArrayStorage::Contiguous:
    case ArrayStorage::Other:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const OpcodeSet& opcodesWithRequiredOperandTypes()
{
    static constexpr OpcodeSet opcodes {
        Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::PrivateDefine,
        Opcode::PutSlot, Opcode::PutSlotOrAdd,
        Opcode::NewClosure, Opcode::NewObject, Opcode::NewArray, Opcode::CreateIteratorResult,
        Opcode::CallDirect, Opcode::AwaitCallBySavingFrame, Opcode::CallTable, Opcode::Results
    };
    return opcodes;
}

template<bool isKnownToBeDeclared, typename Operation>
static ALWAYS_INLINE TypeID requiredTypeOfOperandInResultRules(const TypingContext& context, const Operation& user, uint32_t operandIndex)
{
    Opcode opcode = opcodeOf(user);
    if (!opcodesWithRequiredOperandTypes().contains(opcode)) {
        ASSERT(operandKindOf(opcode, operandIndex) != OperandKind::Declared);
        return { };
    }
    if (testsItsOwnGuess(user))
        return { };

    TypingContext proven = context.atLevel(TypeLevel::Proven);
    if (opcode == Opcode::CreateIteratorResult)
        return requiredTypeOfIteratorResultField(proven, operandIndex);
    if (!isKnownToBeDeclared && operandKindOf(opcode, operandIndex) != OperandKind::Declared)
        return { };

    uint32_t variadicIndex = operandIndex - numberOfNamedOperandsOf(opcode);
    if (opcode != Opcode::CallDirect) {
        if (opcode == Opcode::PutModuleVariable)
            return requiredTypeOfModuleVariable(proven, user.variable());
        if (opcode == Opcode::PutEnvironmentField)
            return requiredTypeOfEnvironmentField(proven, user.layout(), user.field());
        if (opcode == Opcode::PrivateDefine)
            return requiredTypeOfPrivateDefinition(proven, user.layout(), user.field());
        if (opcode == Opcode::PutSlot || opcode == Opcode::PutSlotOrAdd)
            return tryTypeAcceptedBySlots(proven, user.receivers(), user.key());
        if (opcode == Opcode::NewArray)
            return requiredTypeOfElementInStorage(user.arrayStorage());

        if (opcode == Opcode::CallTable || opcode == Opcode::Results)
            return { };

        if (opcode == Opcode::NewClosure)
            return requiredTypeOfCapture(proven, user.function(), variadicIndex);
        if (opcode == Opcode::NewObject) {
            std::span<const uint32_t> fields = user.fields();
            if (variadicIndex >= fields.size())
                return { };
            return requiredTypeOfInitialValue(proven, user.layout(), fields[variadicIndex]);
        }

        RELEASE_ASSERT(opcode == Opcode::AwaitCallBySavingFrame);
    }
    CodeRole role = user.role();
    if (!returnsResultOfFunction(role) && role != CodeRole::CoroutineBody)
        return { };
    if (opcode == Opcode::AwaitCallBySavingFrame && operandIndex + user.numberOfHeld().value_or(0) >= user.numberOfOperands())
        return { };
    FunctionID function = user.function();
    const Function& callee = context.program.at(function);
    if (variadicIndex >= callee.numberOfEntryParameters())
        return { };
    if (context.positions.entryParameterIsLeftOutOfCalls(function, variadicIndex))
        return { };
    if (role == CodeRole::ParameterGuard || role == CodeRole::SlowBody)
        return typeByConstructionOfEntryParameter(proven, callee, variadicIndex);
    return declaredEntryTypeOf(proven, function, variadicIndex);
}

template<typename Operation>
TypeID requiredTypeOfOperand(const TypingContext& context, const Operation& user, uint32_t operandIndex)
{
    return requiredTypeOfOperandInResultRules<false>(context, user, operandIndex);
}

template<typename Operation>
ALWAYS_INLINE_INTO_OTHER_FILES TypeID requiredTypeOfDeclaredOperand(const TypingContext& context, const Operation& user, uint32_t operandIndex)
{
    ASSERT(operandKindOf(opcodeOf(user), operandIndex) == OperandKind::Declared);
    return requiredTypeOfOperandInResultRules<true>(context, user, operandIndex);
}

template TypeID requiredTypeOfOperand<Draft>(const TypingContext&, const Draft&, uint32_t);
template TypeID requiredTypeOfOperand<Operation>(const TypingContext&, const Operation&, uint32_t);
template TypeID requiredTypeOfDeclaredOperand<Draft>(const TypingContext&, const Draft&, uint32_t);
template TypeID requiredTypeOfDeclaredOperand<Operation>(const TypingContext&, const Operation&, uint32_t);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
