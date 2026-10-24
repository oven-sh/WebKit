/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCalls.h"
#include "AOTCFG.h"
#include "AOTChooseCallingConventions.h"
#include "AOTOperatorForms.h"
#include "AOTPlacement.h"
#include "AOTResultRules.h"
#include "AOTSchemaResultRules.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet operationsIndexedByNumber {
    Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::HasArrayElement, Opcode::IsElementPresent,
    Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement, Opcode::IsInsideView,
    Opcode::StringRangeCodeUnitOrNaN, Opcode::StringRangeCodeUnitOrMinusOne, Opcode::StringRangeCodePointOrUndefined, Opcode::StringRangeCharacterOrEmpty, Opcode::StringRangeCharacterOrUndefined
};
static constexpr OpcodeSet numberTruncations { Opcode::ToInt32, Opcode::ToUint32 };
static constexpr OpcodeSet storesIntoKnownArrayStorage { Opcode::PutArrayElement, Opcode::TryPutByValDirect };
static_assert((storesIntoKnownArrayStorage | Value::opcodesWithArrayStorage) == Value::opcodesWithArrayStorage);
static constexpr OpcodeSet testsOfCellOrBoxedValue { Opcode::ShapeTest, Opcode::IsPristine, Opcode::IsClosureOf, Opcode::IsEnumeratorCurrent };
static constexpr OpcodeSet operationsReturningBoxedValue { Opcode::RegExpMatchLiteral, Opcode::GetBuiltinValue };

static constexpr OpcodeSet representedBitwiseOperators {
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift, Opcode::BitNot
};
static constexpr OpcodeSet testOperators {
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::Eq, Opcode::StrictEq, Opcode::ToBoolean
};
static_assert((representedBitwiseOperators | testOperators | Value::opcodesWithForm) == Value::opcodesWithForm);
static_assert(!representedBitwiseOperators.intersects(testOperators));
static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
static constexpr OpcodeSet binaryOperatorsOnInt32 { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift };

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

static constexpr bool isBoxedOrUnchosen(Representation representation)
{
    return representation == Representation::JSValue || representation == Representation::Unchosen;
}

static constexpr bool isEquality(Opcode opcode)
{
    return opcode == Opcode::StrictEq || opcode == Opcode::Eq;
}

static Representation integerRepresentationOf(const Type& type)
{
    RELEASE_ASSERT(type.hasOnlyPrimitives(Primitive::Integer));
    return type.integers.isSubsetOf(Range::int32()) ? Representation::Int32 : Representation::Int64;
}

static Representation unboxedNumberRepresentationOf(const Type& type)
{
    return type.isInteger() ? integerRepresentationOf(type) : Representation::Double;
}

static Representation unboxedNumberOrJSValue(const Type& type)
{
    return type.isNumber() ? unboxedNumberRepresentationOf(type) : Representation::JSValue;
}

static Representation naturalOrDefaultRepresentation(const Type& type, Representation natural)
{
    return isLegalRepresentation(type, natural) ? natural : defaultRepresentationOf(type);
}

static Representation chosenOrDefaultRepresentationOf(const RepresentationInputs& inputs, ValueID id)
{
    Representation chosen = inputs.function.at(id).representation;
    return chosen == Representation::Unchosen ? defaultRepresentationOf(inputs.typeOf(id)) : chosen;
}

static std::optional<unsigned> entryParameterIndexOf(const Function& function, const Value& value)
{
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
    for (unsigned index = 0; index < parameters.size(); ++index) {
        if (&function.at(parameters[index]) == &value)
            return index;
    }
    return std::nullopt;
}

static bool picksOwnRepresentation(const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Parameter)
        return !entryParameterIndexOf(function, value);
    return forwardsOperand(value.opcode) || value.opcode == Opcode::Unspecified || constantsInFormOfUses.contains(value.opcode);
}

bool picksOwnRepresentation(const Function& function, ValueID id)
{
    return picksOwnRepresentation(function, function.at(id));
}

bool isForwardedOperand(const Value& user, uint32_t operandIndex)
{
    return forwardsOperand(user.opcode) && user.operandUses(operandIndex).contains(OperandUse::Returns);
}

static Representation wantedByKind(const RepresentationInputs& inputs, OperandKind kind, ValueID operand)
{
    const Type& type = inputs.typeOf(operand);
    if (type.opaque) {
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
    case OperandKind::SourcePlan:
    case OperandKind::BuilderCursor:
        return representationOf(*opaqueKindOf(kind));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Representation integerWidthOf(const RepresentationInputs& inputs, const Value& operation)
{
    const Type& result = inputs.program.at(operation.type);
    bool fitsInt32 = (!result.isInteger() || result.isInt32()) && std::ranges::all_of(operation.operands(), [&](ValueID operand) {
        return !operand || inputs.typeOf(operand).isInt32();
    });
    return fitsInt32 ? Representation::Int32 : Representation::Int64;
}

static Representation wantedByMixedOperands(const RepresentationInputs& inputs, const Value& operation, uint32_t operandIndex, bool (Type::*hasUnboxedKind)() const, Representation unboxed)
{
    uint32_t left = declaredPosition(OperandPosition::left(operation.opcode));
    uint32_t right = declaredPosition(OperandPosition::right(operation.opcode));
    uint32_t unboxedIndex = (inputs.typeOf(operation.operand(left)).*hasUnboxedKind)() ? left : right;
    return operandIndex == unboxedIndex ? unboxed : Representation::JSValue;
}

static bool everyOperandHasDefault(const RepresentationInputs& inputs, const Value& operation, Representation representation)
{
    return std::ranges::all_of(operation.operands(), [&](ValueID operand) {
        return !operand || defaultRepresentationOf(inputs.typeOf(operand)) == representation;
    });
}

static Representation wantedByBits(const RepresentationInputs& inputs, const Value& operation, const Type& type)
{
    if (operation.opcode == Opcode::ToBoolean)
        return isLegalRepresentation(type, Representation::Cell32) ? Representation::Unchosen : defaultRepresentationOf(type);
    return everyOperandHasDefault(inputs, operation, Representation::Cell) ? Representation::Cell : Representation::JSValue;
}

static bool comparesLiteralStringWords(const RepresentationInputs& inputs, const Value& operation)
{
    return isEquality(operation.opcode) && everyOperandHasDefault(inputs, operation, Representation::LiteralString) && std::ranges::none_of(operation.operands(), [&](ValueID operand) {
        return operand && isSameWord(inputs.function.at(operand).representation, Representation::JSValue);
    });
}

static Representation wantedByConcatenation(const Type& part)
{
    return part.isAnyString() ? Representation::Cell : unboxedNumberOrJSValue(part);
}

bool isEqualityOfNumbers(const Program& program, const Function& function, const Value& value)
{
    if (!isEquality(value.opcode) || value.form() != OperatorForm::Numbers)
        return false;
    return program.typeOf(function, value.leftOperand()).isNumber() && program.typeOf(function, value.rightOperand()).isNumber();
}

static bool isSumOfBoxedNumbers(const Value& value)
{
    return sumsOfNumbers.contains(value.opcode) && value.form() == OperatorForm::Numbers && value.representation == Representation::JSValue;
}

static Representation wantedByNumbers(const RepresentationInputs& inputs, const Value& user, ValueID operand)
{
    const Type& type = inputs.typeOf(operand);
    Representation chosen = inputs.function.at(operand).representation;
    if (representedBitwiseOperators.contains(user.opcode) && user.opcode != Opcode::BitNot) {
        if (type.isInteger())
            return integerRepresentationOf(type);
        if (defaultRepresentationOf(type) == Representation::OptionalInt64)
            return Representation::OptionalInt64;
        if (type.isNumber() && isBoxedOrUnchosen(chosen))
            return Representation::Unchosen;
    }
    if (isSumOfBoxedNumbers(user))
        return type.isInt32() ? Representation::Int32 : Representation::JSValue;
    bool takesNumberAsItIs = relations.contains(user.opcode) || isEqualityOfNumbers(inputs.program, inputs.function, user) || (user.opcode == Opcode::ToBoolean && type.isNumber());
    if (takesNumberAsItIs && (isBoxedOrUnchosen(chosen) || (chosen == Representation::Int32 && type.isInt32())))
        return Representation::Unchosen;
    if (type.isNumber())
        return Representation::Double;
    RELEASE_ASSERT(isLegalRepresentation(type, Representation::OptionalDouble));
    return Representation::OptionalDouble;
}

static Representation wantedByOperator(const RepresentationInputs& inputs, const Value& user, uint32_t operandIndex)
{
    OperatorForm form = user.form();
    RELEASE_ASSERT(isLegalOperatorForm(user.opcode, form));

    const Type& type = inputs.typeOf(user.operand(operandIndex));
    switch (form) {
    case OperatorForm::Generic:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        return Representation::JSValue;
    case OperatorForm::NullableStrings:
        return comparesLiteralStringWords(inputs, user) ? Representation::LiteralString : Representation::JSValue;
    case OperatorForm::Integers:
        return representedBitwiseOperators.contains(user.opcode) ? Representation::Int32 : integerWidthOf(inputs, user);
    case OperatorForm::CheckedIntegers:
        return integerWidthOf(inputs, user);
    case OperatorForm::Numbers:
        return wantedByNumbers(inputs, user, user.operand(operandIndex));
    case OperatorForm::NumberAndValue:
        return wantedByMixedOperands(inputs, user, operandIndex, &Type::isNumber, Representation::Double);
    case OperatorForm::Bits:
        return wantedByBits(inputs, user, type);
    case OperatorForm::Booleans:
        return Representation::Boolean;
    case OperatorForm::Strings:
        return comparesLiteralStringWords(inputs, user) ? Representation::LiteralString : Representation::Cell;
    case OperatorForm::BigInts:
    case OperatorForm::String:
        return Representation::Cell;
    case OperatorForm::StringAndValue:
        return wantedByMixedOperands(inputs, user, operandIndex, &Type::isAnyString, Representation::Cell);
    case OperatorForm::Int32AndValue:
        return wantedByMixedOperands(inputs, user, operandIndex, &Type::isInt32, Representation::Int32);
    case OperatorForm::StringAndPrimitive:
        return wantedByConcatenation(type);
    case OperatorForm::NullableStringAndPrimitive:
        return type.isAnyString() ? Representation::Cell : Representation::JSValue;
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
        return unboxedNumberRepresentationOf(type);
    case OperatorForm::NullableNumber:
        return user.opcode != Opcode::ToString && type.isBoolean() ? Representation::Boolean : Representation::JSValue;
    case OperatorForm::Unselected:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Representation resultOfOperator(const RepresentationInputs& inputs, const Value& value)
{
    OperatorForm form = value.form();
    RELEASE_ASSERT(isLegalOperatorForm(value.opcode, form));

    if (testOperators.contains(value.opcode))
        return Representation::Boolean;
    if (value.opcode == Opcode::ToString)
        return Representation::Cell;
    if (value.opcode == Opcode::URShift)
        return Representation::Int64;

    bool isBitwise = representedBitwiseOperators.contains(value.opcode);
    switch (form) {
    case OperatorForm::Generic:
        if (binaryOperatorsOnInt32.contains(value.opcode) && inputs.program.at(value.type).isInt32())
            return Representation::Int32;
        return value.opcode == Opcode::ToNumber ? Representation::Double : Representation::JSValue;
    case OperatorForm::Integers:
        return isBitwise ? Representation::Int32 : integerWidthOf(inputs, value);
    case OperatorForm::Numbers:
        if (isSumOfBoxedNumbers(value))
            return Representation::JSValue;
        return isBitwise ? Representation::Int32 : Representation::Double;
    case OperatorForm::CheckedIntegers:
    case OperatorForm::NumberAndValue:
    case OperatorForm::NullableNumber:
        return Representation::Double;
    case OperatorForm::BigInts:
    case OperatorForm::Strings:
    case OperatorForm::StringAndPrimitive:
        return Representation::Cell;
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
        return Representation::JSValue;
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
    case OperatorForm::Unselected:
        break;
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

static Representation wantedByNewArrayElement(const RepresentationInputs& inputs, ValueID element)
{
    return unboxedElementRepresentations.contains(chosenOrDefaultRepresentationOf(inputs, element)) ? Representation::Unchosen : Representation::JSValue;
}

static bool isCompiledInline(BuiltinCallForm form)
{
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

static bool isTestOfNumber(std::optional<InlineBuiltin> builtin)
{
    return builtin == InlineBuiltin::NumberIsNaN || builtin == InlineBuiltin::NumberIsFinite || builtin == InlineBuiltin::NumberIsInteger || builtin == InlineBuiltin::NumberIsSafeInteger;
}

template<typename Call>
static Representation wantedByBuiltinCall(const RepresentationInputs& inputs, const Call& call, uint32_t operandIndex)
{
    const Program& program = inputs.program;
    bool namesInlineBuiltin = true;
    if constexpr (std::is_same_v<Call, Value>)
        namesInlineBuiltin = call.opcode == Opcode::CallInline;
    BuiltinCallForm form = builtinCallOf(program, call).form;
    if (!namesInlineBuiltin && !isCompiledInline(form))
        return wantedByKind(inputs, call.operandKind(operandIndex), call.operand(operandIndex));
    if (form == BuiltinCallForm::List)
        return Representation::Cell;

    const Type& type = inputs.typeOf(call.operand(operandIndex));
    std::optional<InlineBuiltin> builtin = namesInlineBuiltin ? std::optional(call.inlineBuiltin()) : std::nullopt;
    std::optional<uint32_t> argumentIndex = call.variadicOperandIndex(operandIndex);
    if (argumentIndex && builtin == InlineBuiltin::ArrayPush) {
        Representation wanted = wantedByNewArrayElement(inputs, call.operand(operandIndex));
        bool isPushedToDoubles = arrayStorageDeclaredBy(program, inputs.typeOf(call.operand(0))) == ArrayStorage::Double;
        return wanted == Representation::JSValue && isPushedToDoubles && type.isNumber() ? Representation::Double : wanted;
    }

    BuiltinOperand operand = argumentIndex ? BuiltinOperand::argument(*argumentIndex) : BuiltinOperand::receiver();
    Representation wanted = defaultRepresentationOf(parameterTypeOfOverload(program, call.overload(), operand));
    if (argumentIndex && type.isNumber() && isTestOfNumber(builtin))
        wanted = Representation::Double;
    return wanted == Representation::Double && type.isInteger() ? integerRepresentationOf(type) : wanted;
}

static const PlacedSlot& slotOf(const RepresentationInputs& inputs, FieldAccessID id)
{
    RELEASE_ASSERT(id);
    return inputs.placement.fieldAccess(id).slot;
}

// A closure that its slot leaves unmade has no value. Whatever else such a slot takes is a reference.
static Representation storeRepresentationOf(const RepresentationInputs& inputs, const PlacedSlot& slot, ValueID stored)
{
    if (!slot.hasStorage())
        return Representation::Unchosen;
    if (slot.representation == SlotRepresentation::DeferredClosure)
        return isDeferredClosure(inputs.function, stored) ? Representation::Unchosen : Representation::Cell32;
    return representationOf(slot.representation);
}

static Representation loadRepresentationOf(const PlacedVariable& variable, const Type& type)
{
    switch (variable.storage) {
    case VariableStorage::Statics:
    case VariableStorage::None:
        return representationOf(variable.representation);
    case VariableStorage::ClosureTable:
        return defaultRepresentationOf(type);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Representation captureRepresentationOf(const RepresentationInputs& inputs, FunctionID function, uint32_t captureIndex)
{
    return declaredRepresentationOf(inputs.program, inputs.program.at(function).captures[captureIndex]);
}

static bool isReturnedByFunction(const Function& function, const Value& results)
{
    RELEASE_ASSERT(&function.values.first() <= &results && &results <= &function.values.last());
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        if (block.terminator.kind != TerminatorKind::Return)
            return false;
        ValueID returned = block.terminator.valueOperand();
        return returned && &function.at(returned) == &results;
    });
}

static Representation wantedByResults(const RepresentationInputs& inputs, const Value& results, uint32_t resultIndex)
{
    const Function& function = inputs.function;
    if (function.results.size() != 1 && resultIndex < function.results.size() && isReturnedByFunction(function, results))
        return inputs.conventions.resultRepresentation(inputs.program, inputs.id, resultIndex);
    return declaredRepresentationOf(inputs.program, DeclaredType { .type = function.at(results.valuesOperands()[resultIndex]).type });
}

static Representation wantedByDeclaredPosition(const RepresentationInputs& inputs, const Value& user, uint32_t operandIndex)
{
    ValueID operand = user.operand(operandIndex);
    if (!operand)
        return Representation::Unchosen;

    const Placement& placement = inputs.placement;
    auto variadicIndex = [&] {
        return *user.variadicOperandIndex(operandIndex);
    };

    switch (user.opcode) {
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return storeRepresentationOf(inputs, slotOf(inputs, user.fieldAccess()), operand);
    case Opcode::PutSlots:
        return storeRepresentationOf(inputs, slotOf(inputs, user.placedKeys()[variadicIndex()].fieldAccess), operand);
    case Opcode::NewObject:
        if (user.takesCellOfInnerObject() && placement.layout(user.layout()).innerObjectIn(user.fields()[variadicIndex()]) && inputs.typeOf(operand).isObject())
            return Representation::Cell;
        return storeRepresentationOf(inputs, placement.slot(user.layout(), user.fields()[variadicIndex()]), operand);
    case Opcode::CloneAs:
    case Opcode::PrivateDefine:
        return Representation::JSValue;
    case Opcode::PutEnvironmentField:
        return storeRepresentationOf(inputs, placement.slot(user.layout(), user.field()), operand);
    case Opcode::PutModuleVariable: {
        const PlacedVariable& variable = placement.variable(user.variable());
        return variable.hasWordInStatics() ? representationOf(variable.representation) : Representation::Unchosen;
    }
    case Opcode::NewClosure:
        return captureRepresentationOf(inputs, user.function(), variadicIndex());
    case Opcode::CallDirect:
    case Opcode::CallTable:
    case Opcode::AwaitCallBySavingFrame:
        return inputs.conventions.parameterRepresentation(inputs.program, user.function(), variadicIndex());
    case Opcode::Results:
        return wantedByResults(inputs, user, variadicIndex());
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Representation wantedByDeclaredPosition(const RepresentationInputs& inputs, const Terminator& user, uint32_t operandIndex)
{
    RELEASE_ASSERT(user.kind == TerminatorKind::Return);
    ValueID returned = user.operand(operandIndex);
    if (!returned)
        return Representation::Unchosen;
    if (numberOfResultsOf(inputs.program, inputs.function, inputs.function.at(returned)))
        return Representation::Void;
    return inputs.conventions.resultRepresentation(inputs.program, inputs.id, 0);
}

static Representation wantedByOperation(const RepresentationInputs& inputs, const Value& user, uint32_t operandIndex)
{
    ValueID operand = user.operand(operandIndex);
    Opcode opcode = user.opcode;
    OperandKind kind = user.operandKind(operandIndex);
    if (kind == OperandKind::Guard || kind == OperandKind::Results)
        return Representation::Void;
    if (forwardsOperand(opcode))
        return isForwardedOperand(user, operandIndex) ? user.representation : Representation::Unchosen;
    if (opcode == Opcode::TypeTest || opcode == Opcode::ReportMismatch || familyOf(opcode) == OperationFamily::RuntimeValidation || isRepresentationConversion(opcode))
        return Representation::Unchosen;
    if (hasOperatorForm(opcode))
        return wantedByOperator(inputs, user, operandIndex);

    const Type& type = inputs.typeOf(operand);
    if (acceptsIntegerOrDouble(opcode, operandIndex))
        return unboxedNumberRepresentationOf(type);
    if (takesUntaggedIndex(user, operandIndex) && type.isInteger())
        return integerRepresentationOf(type);
    if (opcode == Opcode::GetMyArgumentByVal)
        return unboxedNumberOrJSValue(type);
    if (isNewArrayElement(user, operandIndex))
        return wantedByNewArrayElement(inputs, operand);
    if (opcode == Opcode::Strcat && !user.isLikelyKey())
        return wantedByConcatenation(type);
    if (opcode == Opcode::StringBuilderAppendNumber && kind == OperandKind::Number && isBoxedOrUnchosen(inputs.function.at(operand).representation))
        return Representation::Unchosen;

    bool isStoredValue = OperandPosition::value(opcode) == operandIndex;
    if (isStoredValue && storesIntoKnownArrayStorage.contains(opcode)) {
        Representation element = elementRepresentationOf(user.arrayStorage());
        return isLegalRepresentation(type, element) ? element : Representation::JSValue;
    }
    if (isStoredValue && opcode == Opcode::PutTypedArrayElement)
        return unboxedNumberOrJSValue(type);

    if (kind == OperandKind::Any && testsOfCellOrBoxedValue.contains(opcode))
        return chosenOrDefaultRepresentationOf(inputs, operand) == Representation::Cell ? Representation::Unchosen : Representation::JSValue;
    if (builtinCallOpcodes.contains(opcode))
        return wantedByBuiltinCall(inputs, user, operandIndex);
    return wantedByKind(inputs, kind, operand);
}

static Representation wantedByOperation(const RepresentationInputs& inputs, const Terminator& user, uint32_t operandIndex)
{
    ValueID operand = user.operand(operandIndex);
    const Type& type = inputs.typeOf(operand);
    Representation chosen = inputs.function.at(operand).representation;
    switch (user.kind) {
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Return:
        return wantedByKind(inputs, user.operandKind(operandIndex), operand);
    case TerminatorKind::Switch:
        if (type.isNumber() && !type.isInteger() && isBoxedOrUnchosen(chosen))
            return Representation::Unchosen;
        return unboxedNumberOrJSValue(type);
    case TerminatorKind::SwitchString: {
        Representation natural = defaultRepresentationOf(type);
        if (natural == Representation::LiteralString && isSameWord(chosen, Representation::JSValue))
            return chosen;
        return stringSubjectRepresentations.contains(natural) ? natural : Representation::JSValue;
    }
    case TerminatorKind::ReturnCall:
        return Representation::Unchosen;
    case TerminatorKind::TryCallInline:
        return wantedByBuiltinCall(inputs, user, operandIndex);
    case TerminatorKind::Jump:
    case TerminatorKind::Unreachable:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename User>
static Representation wantedByUser(const RepresentationInputs& inputs, const User& user, uint32_t operandIndex)
{
    Representation wanted = user.operand(operandIndex) ? wantedByOperation(inputs, user, operandIndex) : Representation::Unchosen;
    if (wanted != Representation::Unchosen || user.operandKind(operandIndex) != OperandKind::Declared)
        return wanted;
    return wantedByDeclaredPosition(inputs, user, operandIndex);
}

Representation wantedRepresentationOf(const RepresentationInputs& inputs, const Value& user, uint32_t operandIndex)
{
    return wantedByUser(inputs, user, operandIndex);
}

Representation wantedRepresentationOf(const RepresentationInputs& inputs, const Terminator& user, uint32_t operandIndex)
{
    return wantedByUser(inputs, user, operandIndex);
}

static Representation resultOfDeclaredPosition(const RepresentationInputs& inputs, const Value& value, uint32_t resultIndex)
{
    switch (value.opcode) {
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
    case Opcode::GetDeferredClosureSlot:
    case Opcode::GetDeferredClosureSlotOrUndefined: {
        const Type& type = inputs.program.at(value.type);
        const PlacedSlot& slot = slotOf(inputs, value.fieldAccess());
        return naturalOrDefaultRepresentation(type, slot.isOutOfLine() ? representationOf(slot.representation) : loadedRepresentationOf(slot.representation, type));
    }
    case Opcode::GetSlotGetter:
    case Opcode::GetSlotSetter:
        return Representation::Cell;
    case Opcode::GetEnvironmentField:
        return representationOf(inputs.placement.slot(value.layout(), value.field()).representation);
    case Opcode::GetModuleVariable:
    case Opcode::GetEncodedModuleVariable:
        return loadRepresentationOf(inputs.placement.variable(value.variable()), inputs.program.at(value.type));
    case Opcode::GetCapture:
        return captureRepresentationOf(inputs, value.function(), value.index());
    case Opcode::CallDirect:
    case Opcode::CallTable:
        return inputs.conventions.resultRepresentation(inputs.program, value.function(), resultIndex);
    case Opcode::Parameter: {
        std::optional<unsigned> index = entryParameterIndexOf(inputs.function, value);
        RELEASE_ASSERT(index);
        return inputs.conventions.parameterRepresentation(inputs.program, inputs.id, *index);
    }
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    Representation declared(const Value& value, TypeID) { return byType(value); }
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
        RELEASE_ASSERT(!resultIndex || value.opcode == Opcode::CallDirect || value.opcode == Opcode::CallTable);
        return resultOfDeclaredPosition(inputs, value, resultIndex.value_or(0));
    }
};

} // anonymous namespace

Representation resultRepresentationOf(const RepresentationInputs& inputs, const Value& value)
{
    if (picksOwnRepresentation(inputs.function, value))
        return Representation::Unchosen;
    if (isRepresentationConversion(value.opcode))
        return value.representation;
    if (numberOfResultsOf(inputs.program, inputs.function, value))
        return Representation::Void;
    if (hasOperatorForm(value.opcode))
        return resultOfOperator(inputs, value);

    const Type& type = inputs.program.at(value.type);
    if (value.opcode == Opcode::GetArrayElement)
        return naturalOrDefaultRepresentation(type, elementRepresentationOf(value.arrayStorage()));
    if (value.opcode == Opcode::GetTypedArrayElement)
        return naturalOrDefaultRepresentation(type, defaultRepresentationOf(elementTypeOf(value.typedArrayType())));
    if (operationsReturningBoxedValue.contains(value.opcode) || (value.opcode == Opcode::CallBuiltin && !isCompiledInline(value.builtinCallForm())))
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

static std::optional<Opcode> stepFromVoid(Representation to)
{
    using enum Representation;
    switch (to) {
    case OptionalInt64: case OptionalDouble: case JSValue:
        return Opcode::Box;
    case Cell32: case LiteralString:
        return Opcode::Compress;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case Cell: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromBoolean(Representation to)
{
    using enum Representation;
    switch (to) {
    case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell: case Cell32: case LiteralString: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromInt32(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int64:
        return Opcode::WidenInteger;
    case Double:
        return Opcode::IntegerToNumber;
    case OptionalInt64: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int32: case OptionalDouble: case Cell: case Cell32: case LiteralString: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromInt64(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int32:
        return Opcode::NarrowInteger;
    case Double:
        return Opcode::IntegerToNumber;
    case OptionalInt64: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int64: case OptionalDouble: case Cell: case Cell32: case LiteralString: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromDouble(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int32: case Int64:
        return Opcode::NumberToInteger;
    case OptionalDouble: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Double: case OptionalInt64: case Cell: case Cell32: case LiteralString: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromOptionalInt64(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int32: case Int64:
        return Opcode::Unbox;
    case OptionalDouble: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Double: case OptionalInt64: case Cell: case Cell32: case LiteralString: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromOptionalDouble(Representation to)
{
    using enum Representation;
    switch (to) {
    case Double: case OptionalInt64:
        return Opcode::Unbox;
    case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case OptionalDouble: case Cell: case Cell32: case LiteralString: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromCell(Representation to)
{
    using enum Representation;
    switch (to) {
    case JSValue:
        return Opcode::Box;
    case Cell32: case LiteralString:
        return Opcode::Compress;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromEncodedCell(Representation to)
{
    using enum Representation;
    switch (to) {
    case Cell: case JSValue:
        return Opcode::Decode;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell32: case LiteralString: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromJSValue(Representation to)
{
    using enum Representation;
    switch (to) {
    case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell:
        return Opcode::Unbox;
    case Cell32: case LiteralString:
        return Opcode::Compress;
    case Unchosen: case Void: case JSValue: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> singleStep(Representation from, Representation to)
{
    using enum Representation;
    switch (from) {
    case Void:
        return stepFromVoid(to);
    case Boolean:
        return stepFromBoolean(to);
    case Int32:
        return stepFromInt32(to);
    case Int64:
        return stepFromInt64(to);
    case Double:
        return stepFromDouble(to);
    case OptionalInt64:
        return stepFromOptionalInt64(to);
    case OptionalDouble:
        return stepFromOptionalDouble(to);
    case Cell:
        return stepFromCell(to);
    case Cell32: case LiteralString:
        return stepFromEncodedCell(to);
    case JSValue:
        return stepFromJSValue(to);
    case Unchosen: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<ConversionPath> findConversion(const Type& type, Representation from, Representation to)
{
    if (from == to)
        return ConversionPath { };
    RELEASE_ASSERT(from != Representation::Unchosen && to != Representation::Unchosen && to != Representation::Void);
    if (!isLegalRepresentation(type, to))
        return std::nullopt;

    if (std::optional<Opcode> opcode = singleStep(from, to))
        return ConversionPath { ConversionStep { *opcode, to } };

    for (Representation intermediate : { Representation::Int64, Representation::Double, Representation::Cell, Representation::JSValue }) {
        if (!isLegalRepresentation(type, intermediate))
            continue;
        std::optional<Opcode> first = singleStep(from, intermediate);
        std::optional<Opcode> second = singleStep(intermediate, to);
        if (first && second)
            return ConversionPath { ConversionStep { *first, intermediate }, ConversionStep { *second, to } };
    }
    return std::nullopt;
}

void checkRepresentations(const RepresentationInputs& inputs, Vector<InternalError>& errors)
{
    const Function& function = inputs.function;
    auto storedRepresentationOf = [&](ValueID id) {
        return function.at(id).representation;
    };
    auto report = [&](InternalErrorKind kind, BlockID block, ValueID value) {
        StringPrintStream detail;
        detail.print("the value"_s);
        detail.print(" is kept as "_s, nameOf(storedRepresentationOf(value)));
        errors.append(InternalError { .kind = kind, .function = inputs.id, .block = block, .value = value, .detail = detail.toString() });
    };
    auto hasRepresentationOfResult = [&](ValueID id) {
        const Value& value = function.at(id);
        if (!isLegalRepresentation(inputs.typeOf(id), value.representation))
            return false;
        Representation expected = resultRepresentationOf(inputs, value);
        return expected == Representation::Unchosen || expected == value.representation;
    };
    auto canConvert = [&](ValueID id, Representation wanted) {
        return storedRepresentationOf(id) != Representation::Unchosen && !!findConversion(inputs.typeOf(id), storedRepresentationOf(id), wanted);
    };
    auto conversionIsNeeded = [&](ValueID id, Representation wanted) {
        return hasRegister(wanted) && storedRepresentationOf(id) != wanted;
    };
    auto findOperandWithoutConversion = [&](const auto& user) {
        ValueID found;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            Representation wanted = wantedRepresentationOf(inputs, user, operandIndex);
            if (!found && conversionIsNeeded(operand, wanted) && !canConvert(operand, wanted))
                found = operand;
        });
        return found;
    };
    auto findOperandTakenInTwoForms = [&](const auto& user) {
        auto formTakenAt = [&](ValueID operand, uint32_t operandIndex) {
            Representation wanted = wantedRepresentationOf(inputs, user, operandIndex);
            return conversionIsNeeded(operand, wanted) ? wanted : storedRepresentationOf(operand);
        };
        ValueID found;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            user.forEachOperand([&](ValueID other, uint32_t otherIndex) {
                if (!found && other == operand && otherIndex < operandIndex && formTakenAt(other, otherIndex) != formTakenAt(operand, operandIndex))
                    found = operand;
            });
        });
        return found;
    };
    auto isSingleConversionStep = [&](ValueID id) {
        const Value& conversion = function.at(id);
        Representation from = storedRepresentationOf(conversion.operand(0));
        Representation to = conversion.representation;
        if (from == Representation::Unchosen || !hasRegister(to))
            return false;
        std::optional<ConversionPath> path = findConversion(inputs.typeOf(id), from, to);
        return path && path->size() == 1 && path->first() == ConversionStep { conversion.opcode, to };
    };
    auto findError = [&](ValueID id) -> std::optional<InternalErrorKind> {
        const Value& value = function.at(id);
        if (!hasRepresentationOfResult(id))
            return InternalErrorKind::WrongRepresentation;
        if (findOperandWithoutConversion(value))
            return InternalErrorKind::UnsupportedConversion;
        if (findOperandTakenInTwoForms(value))
            return InternalErrorKind::WrongRepresentation;
        if (isRepresentationConversion(value.opcode) && !isSingleConversionStep(id))
            return InternalErrorKind::UnsupportedConversion;
        return std::nullopt;
    };

    CFG cfg(function);
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters) {
            if (!hasRepresentationOfResult(parameter))
                report(InternalErrorKind::WrongRepresentation, id, parameter);
        }
        for (ValueID value : block.values) {
            if (std::optional<InternalErrorKind> error = findError(value))
                report(*error, id, value);
        }
        if (ValueID operand = findOperandWithoutConversion(block.terminator))
            report(InternalErrorKind::UnsupportedConversion, id, operand);
        if (ValueID operand = findOperandTakenInTwoForms(block.terminator))
            report(InternalErrorKind::WrongRepresentation, id, operand);
        forEachOutgoingEdge(function, id, [&](Edge edge, const Target& target) {
            const CompactList<ValueID>& parameters = function.at(target.block).parameters;
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                Representation wanted = storedRepresentationOf(parameters[index]);
                IncomingValue incoming = incomingValueOf(function, edge, index);
                switch (incoming.kind) {
                case IncomingValue::Kind::Argument:
                    if (incoming.argument && hasRegister(wanted) && !canConvert(incoming.argument, wanted))
                        report(InternalErrorKind::UnsupportedConversion, edge.from, incoming.argument);
                    break;
                case IncomingValue::Kind::EdgeResult:
                    break;
                }
            }
        });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
