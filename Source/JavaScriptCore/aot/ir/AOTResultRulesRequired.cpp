/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- What a body takes for granted of its entry parameters

// The type of an established position bounds what every caller brings.
// `this` of a constructor or of a field initializer is no such position: a parent may return any object and new.target may select another class, so the front end writes it as a guarded or an expected one.
static TypeID typeByConstructionOfEntryParameter(const TypingContext& context, const Function& callee, unsigned entryParameterIndex)
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

// The walk types the entry parameters of a body by the same steps, so an argument that lies inside the answer lies inside the type of the parameter.
TypeID declaredEntryTypeOf(const TypingContext& context, FunctionID callee, unsigned entryParameterIndex)
{
    TypeID byConstruction = typeByConstructionOfEntryParameter(context, context.program.at(callee), entryParameterIndex);
    TypeID known = context.positions.entryParameter(context.level, callee, entryParameterIndex);
    if (!byConstruction || !known)
        return byConstruction ? byConstruction : known;

    // An environment or an opaque value is what the compiler laid down, whatever else is said of the position.
    if (!context.at(byConstruction).isJavaScriptValue())
        return byConstruction;

    if (isNeverType(known))
        return known;

    // Both hold. Where nothing is left of the two the wider one is taken, which claims less.
    TypeID both = context.types.intersectionOf(byConstruction, known);
    return isNeverType(both) ? byConstruction : both;
}

// ---- Positions that the compiler lays down

// A guarded or an expected position takes what arrives. The type is handed on as it is written: a word that may be anything may be stored where the type has Unspecified.
static TypeID requiredTypeOfEstablishedPosition(const TypingContext& context, const DeclaredType& declared)
{
    return declared.isEstablished() ? context.typeIDOf(declared) : TypeID();
}

// A variable that Plan added is numbered behind the program's own and has no record, so nothing is established of it.
// Plan may also change what a variable of the program holds. A read goes by what the positions answer before it goes by what is established, so a store of that fits as well.
static TypeID requiredTypeOfModuleVariable(const TypingContext& context, VariableID variable)
{
    if (variable.index() >= context.program.variables.size())
        return { };
    TypeID established = requiredTypeOfEstablishedPosition(context, context.program.at(variable).declared);
    // An environment or an opaque value is what the compiler laid down, whatever else is said of the position.
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
    // No closure has a word for a capture that no caller hands over.
    if (captureIndex >= callee.captures.size() || context.positions.entryParameterIsLeftOutOfCalls(function, captureIndex))
        return { };
    return requiredTypeOfEstablishedPosition(context, callee.captures[captureIndex]);
}

// The run time defines the property by name and takes any value of JavaScript. What stands for a brand is the compiler's own, and so is its type.
static TypeID requiredTypeOfPrivateDefinition(const TypingContext& context, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = context.program.at(layout).fields;
    if (field >= fields.size())
        return { };
    return fields[field].flags.contains(FieldFlags::IsBrand) ? context.typeIDOf(fields[field].declared) : TypeTable::anything;
}

// ---- Slots

// By the index that the allocation lists, which may name a field that Facts appended behind the declared ones. Where nothing is said of that slot, the field answers as it does for a store by its key.
// The slot of a layout that no object is held to have is never. The walk may still come to its allocation, in an arm that it cannot fold away.
static TypeID requiredTypeOfInitialValue(const TypingContext& context, LayoutID layout, uint32_t field)
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

// ---- Elements

// A literal that names storage for numbers alone is born with it, and nobody who reads the name tests the storage. A NaN among the numbers is found when the literal runs.
// Storage for every value takes an element whatever is known of it.
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

// ---- The rule

// Every opcode with an operand of the kind Declared, and CreateIteratorResult. operandKindOf() is no constant expression, so they are written out, and requiredTypeOfOperand() asserts that none is left out.
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

template<typename Operation>
TypeID requiredTypeOfOperand(const TypingContext& context, const Operation& user, uint32_t operandIndex)
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
    if (operandKindOf(opcode, operandIndex) != OperandKind::Declared)
        return { };

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

    // Neither says in its own words what it enters or leaves. How many results a unit hands back and what each of them holds is Plan's to say, and the Results of an inlined callee are returned by nobody.
    if (opcode == Opcode::CallTable || opcode == Opcode::Results)
        return { };

    uint32_t variadicIndex = operandIndex - numberOfNamedOperandsOf(opcode);
    if (opcode == Opcode::NewClosure)
        return requiredTypeOfCapture(proven, user.function(), variadicIndex);
    if (opcode == Opcode::NewObject) {
        std::span<const uint32_t> fields = user.fields();
        if (variadicIndex >= fields.size())
            return { };
        return requiredTypeOfInitialValue(proven, user.layout(), fields[variadicIndex]);
    }

    RELEASE_ASSERT(opcode == Opcode::CallDirect || opcode == Opcode::AwaitCallBySavingFrame);
    // Whoever starts a coroutine hands over the entry parameters of the function too. Every other unit lays out its own parameters.
    if (!returnsResultOfFunction(user.role()) && user.role() != CodeRole::CoroutineBody)
        return { };
    // The last operands of a suspension are what the suspended function holds. Nothing reads them.
    if (opcode == Opcode::AwaitCallBySavingFrame && operandIndex + user.numberOfHeld().value_or(0) >= user.numberOfOperands())
        return { };
    // The driver.
    if (variadicIndex >= context.program.at(user.function()).numberOfEntryParameters())
        return { };
    if (context.positions.entryParameterIsLeftOutOfCalls(user.function(), variadicIndex))
        return { };
    // PositionTypes::entryParameter() speaks of the entry behind the guard. Whoever enters in front of it, or where it has failed, has proven none of that.
    if (user.role() == CodeRole::ParameterGuard || user.role() == CodeRole::SlowBody)
        return typeByConstructionOfEntryParameter(proven, context.program.at(user.function()), variadicIndex);
    return declaredEntryTypeOf(proven, user.function(), variadicIndex);
}

template TypeID requiredTypeOfOperand<Draft>(const TypingContext&, const Draft&, uint32_t);
template TypeID requiredTypeOfOperand<Operation>(const TypingContext&, const Operation&, uint32_t);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
