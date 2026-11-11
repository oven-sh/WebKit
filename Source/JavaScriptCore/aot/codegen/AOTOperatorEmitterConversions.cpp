/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTCastEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTIRProgram.h"
#include "AOTOperatorForms.h"
#include "AOTSort.h"
#include "AOTStringEmitter.h"
#include "AOTStubsValues.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeArena.h"
#include "JSType.h"

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr OpcodeSet conversionsToString { Opcode::ToString };

class ToStringEmitter final : public OperationEmitter {
public:
    explicit ToStringEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    CellPointer convert(const OperationContext& context, Operand operand, const Type& type, OperatorForm form)
    {
        if (type.isBoolean()) {
            m_out.noteFormOf(conversionsToString, Form::Open, StateOfKnowledge::Booleans);
            return textOfBoolean(context, operand);
        }
        if (operand.is<CellPointer>())
            operand = boxCell(operand.as<CellPointer>());

        StringEmitter& strings = emitter<StringEmitter>();
        auto orByOperation = [&](Condition isConvertedInLine, const auto& convertInLine) {
            return either<CellPointer>(isConvertedInLine, convertInLine, [&] {
                return callToString(context, operand.as<Boxed>(), { .isOnRarePath = true });
            });
        };
        switch (form) {
        case OperatorForm::Generic:
            m_out.noteFormOf(conversionsToString, Form::Runtime, StateOfKnowledge::Generic);
            return callToString(context, operand.as<Boxed>());
        case OperatorForm::String:
            m_out.noteFormOf(conversionsToString, Form::Open, StateOfKnowledge::String);
            return unboxCell(operand.as<Boxed>());
        case OperatorForm::Number:
            switch (operand.representation) {
            case Representation::Int32:
                m_out.noteFormOf(conversionsToString, Form::Closed, StateOfKnowledge::Number);
                return strings.integerToString(context, operand.as<Word32>());
            case Representation::Int64:
                m_out.noteFormOf(conversionsToString, Form::Closed, StateOfKnowledge::Number);
                return strings.integerToString(context, operand.as<Integer>());
            case Representation::Double:
                m_out.noteFormOf(conversionsToString, Form::Closed, StateOfKnowledge::NumberWithFraction);
                return convertDouble(context, operand.as<Number>());
            case Representation::Unchosen:
            case Representation::Void:
            case Representation::Boolean:
            case Representation::OptionalInt64:
            case Representation::OptionalDouble:
            case Representation::Cell:
            case Representation::Cell32:
            case Representation::JSValue:
            case Representation::RawWord:
                break;
            }
            break;
        case OperatorForm::NullableString:
            m_out.noteFormOf(conversionsToString, Form::Open, StateOfKnowledge::NullableString);
            return orByOperation(isCell(operand.as<Boxed>()), [&] {
                return unboxCell(operand.as<Boxed>());
            });
        case OperatorForm::NullableNumber:
            m_out.noteFormOf(conversionsToString, Form::Closed, StateOfKnowledge::NullableNumber);
            return orByOperation(isInt32(operand.as<Boxed>()), [&] {
                return strings.integerToString(context, unboxInt32(operand.as<Boxed>()));
            });
        case OperatorForm::MayBeString:
        case OperatorForm::MayBeStringOrNumber:
            return convertPossibleString(context, operand.as<Boxed>(), type, form);
        case OperatorForm::Unselected:
        case OperatorForm::Integers:
        case OperatorForm::CheckedIntegers:
        case OperatorForm::Numbers:
        case OperatorForm::NumberAndValue:
        case OperatorForm::Bits:
        case OperatorForm::Booleans:
        case OperatorForm::BigInts:
        case OperatorForm::Strings:
        case OperatorForm::StringAndValue:
        case OperatorForm::Int32AndValue:
        case OperatorForm::StringAndPrimitive:
        case OperatorForm::NullableStrings:
        case OperatorForm::NullableStringAndPrimitive:
        case OperatorForm::NumberByOperation:
            break;
        }
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
    }

    CellPointer convertAddend(const OperationContext& context, ValueID id)
    {
        return convertByType(context, get(id), typeIDOf(id));
    }

    StringEmitter::StringPart partOfConcatenation(const OperationContext& context, ValueID id)
    {
        if (StringID constant = emitter<StringEmitter>().findNumberedStringConstant(context, id))
            return constant;
        return convertAddend(context, id);
    }

    CellPointer convertByType(const OperationContext& context, Operand operand, TypeID typeID)
    {
        Type type = types().at(typeID);
        if (type.isBoolean())
            return textOfBoolean(context, operand);
        bool isUnboxedNumber = operand.is<Word32>() || operand.is<Integer>() || operand.is<Number>();
        if (!isUnboxedNumber || !type.isNumber())
            operand = emitter<ConversionEmitter>().box(context, operand, type);
        OperatorForm form = chooseConversionForm(m_program, types(), Opcode::ToString, typeID);
        if (operand.is<Boxed>() && form == OperatorForm::Number)
            form = OperatorForm::NullableNumber;
        return convert(context, operand, type, form);
    }

private:
    CellPointer textOfBoolean(const OperationContext& context, Operand operand)
    {
        Condition isTrue = operand.is<Condition>() ? operand.as<Condition>() : unboxBoolean(operand.as<Boxed>());
        ConstantEmitter& constants = emitter<ConstantEmitter>();
        CellPointer textOfTrue = constants.stringConstant(context, m_program.strings.textOf(true));
        CellPointer textOfFalse = constants.stringConstant(context, m_program.strings.textOf(false));
        return select(isTrue, textOfTrue, textOfFalse);
    }

    CellPointer callToString(const OperationContext& context, Boxed boxed, const CallOptions& options = { })
    {
        return callOperation<CellPointer>(context, RuntimeOperation::ToString, { boxed }, options);
    }

    CellPointer convertDouble(const OperationContext& context, Number value)
    {
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Word32 whole = conversions.truncateToWord(value);
        Integer bitsOfWhole = conversions.bitsOf(toNumber(whole));
        return selectByBranch<CellPointer>(equal(bitsOfWhole, conversions.bitsOf(value)), [&] {
            return emitter<StringEmitter>().integerToString(context, whole);
        }, [&] {
            return emitter<StringEmitter>().numberToString(context, value);
        });
    }

    CellPointer convertPossibleString(const OperationContext& context, Boxed boxed, const Type& type, OperatorForm form)
    {
        static_assert(formDependsOnLoopOf(Opcode::ToString));
        FormAtPlace named = formByLoopAlone(Opcode::ToString, stateOf(form), context.block() && generator().isInLoop(context.block()));
        m_out.noteFormOf(conversionsToString, named.form, stateOf(form), named.place);
        if (named.form != Form::Open)
            return CellPointer(callStub(context, unaryOperation(UnaryOperationForm::ToString), { boxed.value() }).first);
        bool everyCellIsString = !type.hasObjects() && !(type.primitives & (Primitive::AnySymbol | Primitive::BigInt));
        bool convertsIntegers = form == OperatorForm::MayBeStringOrNumber;
        LBasicBlock cellCase = newBlock();
        LBasicBlock stringCase = newBlock();
        LBasicBlock notCellCase = newBlock();
        LBasicBlock otherCase = newBlock();
        Join<CellPointer> join(m_out);
        branch(isCell(boxed), { normalEdge(cellCase), convertsIntegers ? normalEdge(notCellCase) : unlikelyEdge(notCellCase) });

        m_out.appendTo(cellCase);
        if (everyCellIsString)
            m_out.jump(stringCase);
        else
            branch(hasCellType(unboxCell(boxed), StringType), { normalEdge(stringCase), unlikelyEdge(otherCase) });

        m_out.appendTo(stringCase);
        join.add(unboxCell(boxed));

        m_out.appendTo(notCellCase);
        if (convertsIntegers) {
            LBasicBlock int32Case = newBlock();
            branch(isInt32(boxed), { normalEdge(int32Case), unlikelyEdge(otherCase) });
            m_out.appendTo(int32Case);
            join.add(emitter<StringEmitter>().integerToString(context, unboxInt32(boxed)));
        } else
            m_out.jump(otherCase);

        m_out.appendTo(otherCase);
        join.add(CellPointer(callStub(context, unaryOperation(UnaryOperationForm::ToString), { boxed.value() }).first));
        return join.finish();
    }
};

} // anonymous namespace

static Vector<uint32_t, 8> firstOccurrencesOfStrcatParts(std::span<const ValueID> parts)
{
    Vector<uint32_t, 8> indexesByValue(parts.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::stableSort(indexesByValue, { }, [&](uint32_t index) {
        return parts[index];
    });

    Vector<uint32_t, 8> firstOccurrences(parts.size());
    uint32_t first = indexesByValue.first();
    for (uint32_t index : indexesByValue) {
        if (parts[index] != parts[first])
            first = index;
        firstOccurrences[index] = first;
    }
    return firstOccurrences;
}

void OperatorEmitter::generateToBoolean(const ValueContext& value)
{
    ValueID tested = value->valueOperand();
    TruthinessEmitter& truthiness = emitter<TruthinessEmitter>();
    switch (value->form()) {
    case OperatorForm::Generic:
        if (value.isBranchCondition()) {
            Operand operand = get(tested);
            if (!operand.is<Boxed>())
                m_out.noteForm(Form::Open, StateOfKnowledge::Bits);
            truthiness.branchOnTruthiness(value, operand, typeOf(tested), value.branchTargets(), value->expectsObjectOrNothing(), value->expectsInt32OrNothing(), (callsByValue | OpcodeSet { Opcode::CallResolved }).contains(generator().code().at(tested).opcode), value->expectsBoolean());
            return;
        }
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        define(value, Condition(m_out.castToInt32(callStub(value, AOT::toBoolean(), { in<Boxed>(tested).value() }).first)));
        return;
    case OperatorForm::Integers:
    case OperatorForm::Numbers:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
        m_out.noteForm(Form::Open, stateOf(value->form()));
        if (ValueID check = generator().checkLeftToTruthTest(value.id())) {
            emitter<CastEmitter>().checkBooleanOrBranch(value, check);
            if (value.isBranchCondition())
                return;
        }
        defineTest(value, truthiness.isTruthy(value, get(tested), typeOf(tested)));
        return;
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::NumberAndValue:
    case OperatorForm::BigInts:
    case OperatorForm::Strings:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::NullableNumber:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    defineTest(value, boolean(false));
}

void OperatorEmitter::generateToString(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    define(value, ToStringEmitter(generator()).convert(value, get(operand), typeOf(operand), value->form()));
}

CellPointer OperatorEmitter::toString(const OperationContext& context, Operand operand, const Type& type)
{
    return ToStringEmitter(generator()).convertByType(context, operand, types().intern(type));
}

CellPointer OperatorEmitter::concatenationOfParts(const ValueContext& value)
{
    std::span<const ValueID> parts = value->partsOperands();
    if (parts.empty())
        return emptyString();
    ToStringEmitter toStrings(generator());
    if (!value->isLikelyKey()) {
        Vector<uint32_t, 8> firstOccurrences = firstOccurrencesOfStrcatParts(parts);
        Vector<StringEmitter::StringPart, 8> fibers;
        for (uint32_t index = 0; index < parts.size(); ++index) {
            uint32_t first = firstOccurrences[index];
            bool isConvertedAlready = first < index && !typeOf(parts[index]).hasObjects();
            fibers.append(isConvertedAlready ? fibers[first] : toStrings.partOfConcatenation(value, parts[index]));
        }
        return emitter<StringEmitter>().concatenate(value, fibers.span());
    }
    if (parts.size() == 1)
        return toStrings.convertAddend(value, parts[0]);
    Vector<Boxed, 8> primitives;
    for (ValueID part : parts) {
        const Type& type = typeOf(part);
        bool conversionIsObservable = type.hasObjects() || type.primitives & Primitive::AnySymbol;
        primitives.append(conversionIsObservable ? boxCell(toStrings.convertAddend(value, part)) : in<Boxed>(part));
    }
    return emitter<StringEmitter>().concatenateKey(value, primitives.span(), value->isLikelyName() ? StringEmitter::KeyUse::NameOfProperty : StringEmitter::KeyUse::AnyKey);
}

Operand OperatorEmitter::addStrings(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    ToStringEmitter toStrings(generator());
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    bool resultIsBoxed = value.representation() == Representation::JSValue;
    switch (value->form()) {
    case OperatorForm::Strings: {
        auto partOf = [&](ValueID operand) -> StringEmitter::StringPart {
            if (StringID constant = strings.findNumberedStringConstant(value, operand))
                return constant;
            return in<CellPointer>(operand);
        };
        return strings.concatenate(value, { partOf(left), partOf(right) });
    }
    case OperatorForm::StringAndPrimitive: {
        StringEmitter::StringPart first = toStrings.partOfConcatenation(value, left);
        StringEmitter::StringPart second = toStrings.partOfConcatenation(value, right);
        return strings.concatenate(value, { first, second });
    }
    case OperatorForm::NullableStrings: {
        if (!resultIsBoxed)
            break;
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        return either<Boxed>(isCell(Boxed(m_out.bitOr(x.value(), y.value()))), [&] {
            return boxCell(strings.concatenate(value, { unboxCell(x), unboxCell(y) }));
        }, [&] {
            return arithmeticOnBoxedOperands(value, ArithmeticOperator::Add, x, y);
        });
    }
    case OperatorForm::MayBeString: {
        auto isEmptyString = [&](ValueID addend) {
            StringID literal = singleStringLiteralOf(typeOf(addend));
            return literal && literal == m_program.strings.emptyString();
        };
        bool emptyStringIsOnLeft = isEmptyString(left);
        if (!emptyStringIsOnLeft && !isEmptyString(right))
            break;
        Boxed other = in<Boxed>(emptyStringIsOnLeft ? right : left);
        return unboxCell(Boxed(callStub(value, unaryOperation(UnaryOperationForm::AddToEmptyString), { other.value() }).first));
    }
    case OperatorForm::NullableStringAndPrimitive: {
        if (!resultIsBoxed)
            break;
        bool stringIsOnLeft = typeOf(left).isAnyStringOrNullish();
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        return either<Boxed>(isCell(stringIsOnLeft ? x : y), [&] {
            CellPointer string = unboxCell(stringIsOnLeft ? x : y);
            CellPointer other = toStrings.convertAddend(value, stringIsOnLeft ? right : left);
            return boxCell(stringIsOnLeft ? strings.concatenate(value, { string, other }) : strings.concatenate(value, { other, string }));
        }, [&] {
            return arithmeticOnBoxedOperands(value, ArithmeticOperator::Add, x, y);
        });
    }
    case OperatorForm::Unselected:
    case OperatorForm::Generic:
    case OperatorForm::Integers:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::Numbers:
    case OperatorForm::NumberAndValue:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::BigInts:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::NullableNumber:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    return failWithZero(value);
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
