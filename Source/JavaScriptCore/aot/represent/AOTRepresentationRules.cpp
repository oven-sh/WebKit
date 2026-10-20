/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCalls.h"
#include "AOTFusedAccessEmitter.h"
#include "AOTOperatorForms.h"
#include "AOTSchemaResultRules.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet operationsIndexedByNumber {
    Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::HasArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement,
    Opcode::StringRangeCodeUnitOrNaN, Opcode::StringRangeCodeUnitOrMinusOne, Opcode::StringRangeCodePointOrUndefined, Opcode::StringRangeCharacterOrEmpty, Opcode::StringRangeCharacterOrUndefined
};
static constexpr OpcodeSet numberTruncations { Opcode::ToInt32, Opcode::ToUint32 };
static constexpr OpcodeSet storesIntoKnownArrayStorage { Opcode::PutArrayElement, Opcode::TryPutByValDirect };
static_assert((storesIntoKnownArrayStorage | Value::opcodesWithArrayStorage) == Value::opcodesWithArrayStorage);

static constexpr RepresentationSet unboxedElementRepresentations { Representation::Int32, Representation::Double, Representation::OptionalDouble };
static constexpr RepresentationSet stringSubjectRepresentations { Representation::Cell, Representation::LiteralString };

static constexpr bool forwardsOperand(Opcode opcode)
{
    return isCast(opcode) || opcode == Opcode::FinishClass;
}

static constexpr bool isRepresentationConversion(Opcode opcode)
{
    return familyOf(opcode) == OperationFamily::Representation;
}

Representation integerRepresentationOf(const Type& type)
{
    RELEASE_ASSERT(type.hasOnlyPrimitives(Primitive::Integer));
    return type.integers.isSubsetOf(Range::int32()) ? Representation::Int32 : Representation::Int64;
}

static bool isEntryParameter(const Function& function, const Value& value)
{
    return function.at(entryBlock).parameters.containsIf([&](ValueID parameter) {
        return &function.at(parameter) == &value;
    });
}

static bool picksOwnRepresentation(const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Parameter)
        return !isEntryParameter(function, value);
    return forwardsOperand(value.opcode) || value.opcode == Opcode::Unspecified;
}

bool picksOwnRepresentation(const Function& function, ValueID id)
{
    return picksOwnRepresentation(function, function.at(id));
}

static Representation unboxedNumberOrJSValue(const Type& type)
{
    if (type.isInteger())
        return integerRepresentationOf(type);
    return type.isNumber() ? Representation::Double : Representation::JSValue;
}

static Representation naturalOrDefaultRepresentation(const Type& type, Representation natural)
{
    return isLegalRepresentation(type, natural) ? natural : defaultRepresentationOf(type);
}

static Representation wantedByKind(const Program& program, const Function& function, OperandKind kind, ValueID operand)
{
    const Type& type = program.typeOf(function, operand);
    if (type.opaque) {
        // A slot, a variable and the operands of a direct eval hold an internal cell in a form of their own.
        Representation internal = representationOf(*type.opaque);
        bool isHeldInFormOfPosition = holdsCells(internal) && (kind == OperandKind::Declared || kind == OperandKind::EvalOperand);
        if (!isHeldInFormOfPosition)
            return internal;
    }

    switch (kind) {
    case OperandKind::Integer:
        return integerRepresentationOf(type);
    case OperandKind::Any:
    case OperandKind::Boolean:
    case OperandKind::Number:
    case OperandKind::String:
    case OperandKind::PropertyKey:
    case OperandKind::Object:
    case OperandKind::Callable:
    case OperandKind::Array:
    case OperandKind::Layout:
    case OperandKind::Declared:
    case OperandKind::Environment:
    case OperandKind::EvalOperand:
    case OperandKind::Results:
    case OperandKind::Convertible:
        return wantsOf(kind);
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::Guard:
    case OperandKind::StringBuilder:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::BuilderCursor:
        return representationOf(*opaqueKindOf(kind));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool acceptsIntegerOrDouble(Opcode opcode, uint32_t operandIndex)
{
    if (operationsIndexedByNumber.contains(opcode))
        return OperandPosition::position(opcode) == operandIndex;
    if (opcode == Opcode::StringRange)
        return operandIndex == OperandIndex::StringRange::first || operandIndex == OperandIndex::StringRange::second;
    return numberTruncations.contains(opcode);
}

static bool takesUntaggedIndex(const Value& user, uint32_t operandIndex)
{
    if (user.opcode != Opcode::ElemGet && user.opcode != Opcode::ElemSet)
        return false;
    return OperandPosition::property(user.opcode) == operandIndex && user.keyIsIntegral();
}

static bool isNewArrayElement(const Value& user, uint32_t operandIndex)
{
    if (user.opcode == Opcode::AppendElement)
        return operandIndex == OperandIndex::AppendElement::value;
    return user.opcode == Opcode::NewArray && user.variadicOperandIndex(operandIndex);
}

static Representation wantedByNewArrayElement(const Program& program, const Function& function, ValueID element)
{
    Representation current = function.at(element).representation;
    if (current == Representation::Unchosen)
        current = defaultRepresentationOf(program.typeOf(function, element));
    return unboxedElementRepresentations.contains(current) ? Representation::Unchosen : Representation::JSValue;
}

static Representation wantedByArrayStorage(ArrayStorage storage, const Type& stored)
{
    Representation element = elementRepresentationOf(storage);
    return isLegalRepresentation(stored, element) ? element : Representation::JSValue;
}

static bool isGroupCallClosure(const Value& user, uint32_t operandIndex)
{
    return user.opcode == Opcode::CallGroup && operandIndex == OperandIndex::CallGroup::closure;
}

static bool isCompiledInline(const Value& call, BuiltinCallForm form)
{
    if (call.opcode == Opcode::CallInline)
        return true;

    switch (form) {
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
        return true;
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::AnyReceiver:
    case BuiltinCallForm::List:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isCompiledInline(const Terminator&, BuiltinCallForm)
{
    return true;
}

static std::optional<InlineBuiltin> inlineBuiltinOf(const Value& call)
{
    if (call.opcode != Opcode::CallInline)
        return std::nullopt;
    return call.inlineBuiltin();
}

static std::optional<InlineBuiltin> inlineBuiltinOf(const Terminator& call)
{
    return call.inlineBuiltin();
}

template<typename Call>
static Representation wantedByBuiltinCall(const Program& program, const Function& function, const Call& call, uint32_t operandIndex)
{
    BuiltinCallForm form = builtinCallOf(program, call).form;
    if (!isCompiledInline(call, form))
        return wantedByKind(program, function, call.operandKind(operandIndex), call.operand(operandIndex));
    if (form == BuiltinCallForm::List)
        return Representation::Cell;

    std::optional<uint32_t> argumentIndex = call.variadicOperandIndex(operandIndex);
    if (argumentIndex && inlineBuiltinOf(call) == InlineBuiltin::ArrayPush)
        return wantedByNewArrayElement(program, function, call.operand(operandIndex));

    BuiltinOperand operand = argumentIndex ? BuiltinOperand::argument(*argumentIndex) : BuiltinOperand::receiver();
    Representation wanted = defaultRepresentationOf(parameterTypeOfOverload(program, call.overload(), operand));
    const Type& type = program.typeOf(function, call.operand(operandIndex));
    if (wanted == Representation::Double && type.isInteger())
        return integerRepresentationOf(type);
    return wanted;
}

Representation wantedRepresentationOf(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    ValueID operand = user.operand(operandIndex);
    if (!operand)
        return Representation::Unchosen;

    Opcode opcode = user.opcode;
    OperandKind kind = user.operandKind(operandIndex);
    if (kind == OperandKind::Guard || kind == OperandKind::Results)
        return Representation::Void;
    if (forwardsOperand(opcode))
        return user.representation;
    if (opcode == Opcode::TypeTest || opcode == Opcode::ReportMismatch || familyOf(opcode) == OperationFamily::RuntimeValidation || isRepresentationConversion(opcode))
        return Representation::Unchosen;
    if (hasOperatorForm(opcode))
        return wantedByOperator(program, function, user, operandIndex);

    const Type& type = program.typeOf(function, operand);
    if (acceptsIntegerOrDouble(opcode, operandIndex))
        return type.isInteger() ? integerRepresentationOf(type) : Representation::Double;
    if (takesUntaggedIndex(user, operandIndex) && type.isInteger())
        return integerRepresentationOf(type);
    if (opcode == Opcode::GetMyArgumentByVal)
        return unboxedNumberOrJSValue(type);
    if (isNewArrayElement(user, operandIndex))
        return wantedByNewArrayElement(program, function, operand);
    if (opcode == Opcode::Strcat && !user.isLikelyKey())
        return wantedByConcatenation(type);

    bool isStoredValue = OperandPosition::value(opcode) == operandIndex;
    if (isStoredValue && storesIntoKnownArrayStorage.contains(opcode))
        return wantedByArrayStorage(user.arrayStorage(), type);
    if (isStoredValue && opcode == Opcode::PutTypedArrayElement)
        return unboxedNumberOrJSValue(type);

    if (isGroupCallClosure(user, operandIndex))
        return Representation::Unchosen;
    if (builtinCallOpcodes.contains(opcode))
        return wantedByBuiltinCall(program, function, user, operandIndex);
    return wantedByKind(program, function, kind, operand);
}

Representation wantedRepresentationOf(const Program& program, const Function& function, const Terminator& user, uint32_t operandIndex)
{
    ValueID operand = user.operand(operandIndex);
    if (!operand)
        return Representation::Unchosen;

    switch (user.kind) {
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Return:
        return wantedByKind(program, function, user.operandKind(operandIndex), operand);
    case TerminatorKind::Switch:
        return unboxedNumberOrJSValue(program.typeOf(function, operand));
    case TerminatorKind::SwitchString: {
        Representation natural = defaultRepresentationOf(program.typeOf(function, operand));
        return stringSubjectRepresentations.contains(natural) ? natural : Representation::JSValue;
    }
    case TerminatorKind::ReturnCall:
        return Representation::Unchosen;
    case TerminatorKind::TryCallInline:
        return wantedByBuiltinCall(program, function, user, operandIndex);
    case TerminatorKind::Jump:
    case TerminatorKind::Unreachable:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation wantedRepresentationOf(const RepresentationInputs& inputs, const Value& user, uint32_t operandIndex)
{
    Representation wanted = wantedRepresentationOf(inputs.program, inputs.function, user, operandIndex);
    if (wanted != Representation::Unchosen)
        return wanted;
    if (user.operandKind(operandIndex) == OperandKind::Declared || isGroupCallClosure(user, operandIndex))
        return wantedByDeclaredPosition(inputs, user, operandIndex);
    return Representation::Unchosen;
}

Representation wantedRepresentationOf(const RepresentationInputs& inputs, const Terminator& user, uint32_t operandIndex)
{
    Representation wanted = wantedRepresentationOf(inputs.program, inputs.function, user, operandIndex);
    if (wanted != Representation::Unchosen)
        return wanted;
    if (user.operandKind(operandIndex) == OperandKind::Declared)
        return wantedByDeclaredPosition(inputs, user, operandIndex);
    return Representation::Unchosen;
}

namespace {

struct ResultRepresentationRules {
    using Result = Representation;

    const RepresentationInputs& inputs;
    std::optional<uint32_t> resultIndex;

    Representation none(const Value&) { return Representation::Void; }
    Representation unknown(const Value&) { return Representation::JSValue; }
    Representation constant(const Value& value) { return byType(value); }
    template<typename... Arguments> Representation natural(const Value& value, Arguments&&...) { return byType(value); }
    template<typename... Arguments> Representation allocation(const Value& value, Arguments&&...) { return byType(value); }
    Representation sameAs(const Value& value, uint32_t) { return byType(value); }
    Representation declaredContents(const Value& value, uint32_t, std::optional<ContentsSlot>) { return byType(value); }

    Representation declared(const Value& value, ModuleID) { return byType(value); }
    Representation declared(const Value& value, BuiltinOverloadID, uint32_t) { return byType(value); }
    Representation declared(const Value& value, VariableID) { return byPosition(value); }
    Representation declared(const Value& value, LayoutID, uint32_t) { return byPosition(value); }
    Representation declared(const Value& value, PropertyKeyID) { return byPosition(value); }
    Representation declared(const Value& value, FunctionID, uint32_t) { return byPosition(value); }
    Representation declaredParameter(const Value& value) { return byPosition(value); }

    Representation declared(const Value& value, LayoutID)
    {
        return effectRuleOf(value.opcode) == EffectRule::Call ? Representation::JSValue : byType(value);
    }

    Representation declared(const Value& value, TypeID type)
    {
        if (value.opcode != Opcode::PropGetChecked)
            return byType(value);
        bool returnsNumber = checkedReadKindOf(inputs.program, inputs.program.at(type)) == CheckedReadKind::Number;
        return returnsNumber ? Representation::OptionalDouble : Representation::JSValue;
    }

    Representation declaredResult(const Value& value, uint32_t operandIndex, uint32_t index)
    {
        return resultRepresentationOf(inputs, inputs.function.at(value.operand(operandIndex)), index);
    }

    Representation resultOperand(const Value& results, uint32_t index)
    {
        return wantedRepresentationOf(inputs, results, OperandIndex::Results::values + index);
    }

    template<typename... Arguments> Representation cast(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }

private:
    Representation byType(const Value& value)
    {
        if (!resultIndex)
            return defaultRepresentationOf(inputs.program.at(value.type));
        ScratchTypeArena types(inputs.program.types);
        return defaultRepresentationOf(computeResultType({ inputs.program, inputs.function, types.arena() }, value, *resultIndex));
    }

    Representation byPosition(const Value& value)
    {
        return resultIndex ? resultOfDeclaredPosition(inputs, value, *resultIndex) : resultOfDeclaredPosition(inputs, value);
    }
};

} // anonymous namespace

static bool returnsBoxedValue(const Value& value)
{
    if (value.opcode == Opcode::RegExpMatchLiteral)
        return true;
    return value.opcode == Opcode::CallBuiltin && !isCompiledInline(value, value.builtinCallForm());
}

Representation resultRepresentationOf(const RepresentationInputs& inputs, const Value& value)
{
    if (picksOwnRepresentation(inputs.function, value))
        return Representation::Unchosen;
    if (isRepresentationConversion(value.opcode))
        return value.representation;
    if (numberOfResultsOf(inputs.program, inputs.function, value))
        return Representation::Void;
    if (hasOperatorForm(value.opcode))
        return resultOfOperator(inputs.program, inputs.function, value);

    const Type& type = inputs.program.at(value.type);
    if (value.opcode == Opcode::GetArrayElement)
        return naturalOrDefaultRepresentation(type, elementRepresentationOf(value.arrayStorage()));
    if (value.opcode == Opcode::GetTypedArrayElement)
        return naturalOrDefaultRepresentation(type, defaultRepresentationOf(elementTypeOf(value.typedArrayType())));
    if (returnsBoxedValue(value))
        return Representation::JSValue;

    ResultRepresentationRules rules { inputs, std::nullopt };
    return applyResultRule(rules, value, 0, ResultSide::Unproven);
}

Representation resultRepresentationOf(const RepresentationInputs& inputs, const Value& producer, uint32_t resultIndex)
{
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(inputs.program, inputs.function, producer);
    RELEASE_ASSERT(numberOfResults && resultIndex < *numberOfResults);

    ResultRepresentationRules rules { inputs, resultIndex };
    return applyResultRule(rules, producer, resultIndex, ResultSide::Unproven);
}

Representation edgeResultRepresentationOf(const RepresentationInputs& inputs, EdgeResultPosition position)
{
    EdgeResult result = edgeResultAt(inputs.function, position);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return inputs.function.at(result.subject).representation;
    case EdgeResultKind::Guard:
        return Representation::Void;
    case EdgeResultKind::DeclaredResult: {
        ScratchTypeArena types(inputs.program.types);
        return defaultRepresentationOf(computeTypeOnEdge({ inputs.program, inputs.function, types.arena() }, position));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
