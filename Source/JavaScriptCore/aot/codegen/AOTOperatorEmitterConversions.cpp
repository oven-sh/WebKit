/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTOperatorForms.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTStringEmitter.h"
#include "AOTStubsValues.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeTable.h"
#include "AOTVector.h"
#include "JSType.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

class ToStringEmitter final : public OperationEmitter {
public:
    explicit ToStringEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    CellPointer convert(const OperationContext& context, Operand operand, const Type& type, OperatorForm form)
    {
        StringEmitter& strings = emitter<StringEmitter>();
        auto orByOperation = [&](Condition isConvertedInLine, const auto& convertInLine) {
            return either<CellPointer>(isConvertedInLine, convertInLine, [&] {
                return callToString(context, operand.as<Boxed>(), { .isOnRarePath = true });
            });
        };
        switch (form) {
        case OperatorForm::Generic:
            return callToString(context, operand.as<Boxed>());
        case OperatorForm::String:
            return operand.is<Boxed>() ? unboxCell(operand.as<Boxed>()) : operand.as<CellPointer>();
        case OperatorForm::Number:
        case OperatorForm::NumberByOperation:
            switch (operand.representation) {
            case Representation::Int32:
                return strings.integerToString(context, operand.as<Word32>());
            case Representation::Int64:
                return strings.integerToString(context, operand.as<Integer>());
            case Representation::Double:
            case Representation::DoubleWithPayload:
            case Representation::ImpureDouble:
                return convertDouble(context, operand.as<Number>(), form);
            case Representation::Unchosen:
            case Representation::Void:
            case Representation::Boolean:
            case Representation::OptionalInt64:
            case Representation::OptionalDouble:
            case Representation::Cell:
            case Representation::Cell32:
            case Representation::JSValue:
            case Representation::LiteralString:
            case Representation::RawWord:
                break;
            }
            break;
        case OperatorForm::NullableString:
            return orByOperation(isCell(operand.as<Boxed>()), [&] {
                return unboxCell(operand.as<Boxed>());
            });
        case OperatorForm::NullableNumber:
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
            break;
        }
        fail(context, InternalErrorKind::OpcodeWithoutCode);
        return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
    }

    CellPointer convertAddend(const OperationContext& context, ValueID id)
    {
        Operand operand = get(id);
        if (operand.is<CellPointer>())
            return operand.as<CellPointer>();
        return convertByType(context, operand, typeOf(id));
    }

    CellPointer convertByType(const OperationContext& context, Operand operand, const Type& type)
    {
        TypingContext typing { m_program, m_function, types() };
        OperatorSite site { typing, analyses().effectContext(), context.shape() };
        OperatorTypes operatorTypes { type, type, m_program.at(TypeTable::string) };
        OperatorForm form = chooseConversionForm(site, Opcode::ToString, operatorTypes);
        // The two forms for a number take it unboxed.
        if (operand.is<Boxed>() && form == OperatorForm::Number)
            form = OperatorForm::NullableNumber;
        else if (operand.is<Boxed>() && form == OperatorForm::NumberByOperation)
            form = OperatorForm::Generic;
        return convert(context, operand, type, form);
    }

private:
    CellPointer callToString(const OperationContext& context, Boxed boxed, const CallOptions& options = { })
    {
        return callOperation<CellPointer>(context, RuntimeOperation::ToString, { boxed }, options);
    }

    CellPointer convertDouble(const OperationContext& context, Number value, OperatorForm form)
    {
        if (form == OperatorForm::NumberByOperation)
            return callOperation<CellPointer>(context, RuntimeOperation::DoubleToString, { value });
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Word32 whole = low32(conversions.toInteger(value));
        Integer bitsOfWhole = conversions.bitsOf(toNumber(whole));
        return selectByBranch<CellPointer>(equal(bitsOfWhole, conversions.bitsOf(value)), [&] {
            return emitter<StringEmitter>().integerToString(context, whole);
        }, [&] {
            return emitter<StringEmitter>().numberToString(context, value);
        });
    }

    CellPointer convertPossibleString(const OperationContext& context, Boxed boxed, const Type& type, OperatorForm form)
    {
        bool everyCellIsString = !type.hasObjects() && !(type.primitives & (Primitive::AnySymbol | Primitive::BigInt));
        bool convertsIntegers = form == OperatorForm::MayBeStringOrNumber;
        auto edgeToOther = [&](LBasicBlock block) {
            return context.shape() == CodeShape::Compact ? normalEdge(block) : unlikelyEdge(block);
        };
        LBasicBlock cellCase = newBlock();
        LBasicBlock stringCase = newBlock();
        LBasicBlock notCellCase = newBlock();
        LBasicBlock otherCase = newBlock();
        Join<CellPointer> join(m_out);
        branch(isCell(boxed), { normalEdge(cellCase), convertsIntegers ? normalEdge(notCellCase) : edgeToOther(notCellCase) });

        m_out.appendTo(cellCase);
        if (everyCellIsString)
            m_out.jump(stringCase);
        else
            branch(hasCellType(unboxCell(boxed), StringType), { normalEdge(stringCase), edgeToOther(otherCase) });

        m_out.appendTo(stringCase);
        join.add(unboxCell(boxed));

        m_out.appendTo(notCellCase);
        if (convertsIntegers) {
            LBasicBlock int32Case = newBlock();
            branch(isInt32(boxed), { normalEdge(int32Case), edgeToOther(otherCase) });
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

void OperatorEmitter::generateToBoolean(const ValueContext& value)
{
    ValueID tested = value->valueOperand();
    TruthinessEmitter& truthiness = emitter<TruthinessEmitter>();
    switch (value->form()) {
    case OperatorForm::Generic:
        if (value.isBranchCondition()) {
            truthiness.branchOnTruthiness(value, get(tested), typeOf(tested), value.branchTargets());
            return;
        }
        define(value, Condition(m_out.castToInt32(callStub(value, AOT::toBoolean(), { in<Boxed>(tested).value() }).first)));
        return;
    case OperatorForm::Integers:
    case OperatorForm::Numbers:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
        if (tryGenerateTestOfRemainder(value, tested))
            return;
        if (const WholeRemainder* remainder = findWholeRemainder(tested)) {
            Operand zeroWhereFalsy = remainder->zeroWhereWholeIsZero;
            defineTest(value, zeroWhereFalsy.is<Word32>() ? isNotZero(zeroWhereFalsy.as<Word32>()) : isNotZero(zeroWhereFalsy.as<Integer>()));
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
    bool isPlainNumber = operand.is<Word32>() || operand.is<Integer>() || operand.is<Number>();
    if (!isPlainNumber || !type.isNumber())
        operand = emitter<ConversionEmitter>().box(context, operand, type);
    return ToStringEmitter(generator()).convertByType(context, operand, type);
}

CellPointer OperatorEmitter::concatenationOfParts(const ValueContext& value)
{
    std::span<const ValueID> parts = value->partsOperands();
    if (parts.empty())
        return emptyString();
    if (!value->isLikelyKey()) {
        ToStringEmitter toStrings(generator());
        Vector<CellPointer, 8> fibers;
        for (ValueID part : parts)
            fibers.append(toStrings.convertAddend(value, part));
        return emitter<StringEmitter>().concatenate(value, fibers.span());
    }
    if (parts.size() == 1)
        return toString(value, get(parts[0]), typeOf(parts[0]));
    Vector<Boxed, 8> primitives;
    for (ValueID part : parts) {
        const Type& type = typeOf(part);
        bool conversionIsObservable = type.hasObjects() || type.primitives & Primitive::AnySymbol;
        primitives.append(conversionIsObservable ? boxCell(toString(value, get(part), type)) : in<Boxed>(part));
    }
    auto keyUse = isOnlyKeyOfProperties(m_function, analyses().useLists(), value.id()) ? StringEmitter::KeyUse::NameOfProperty : StringEmitter::KeyUse::AnyKey;
    if (keyUse == StringEmitter::KeyUse::NameOfProperty)
        build().statistics().count(StatisticID::KeysMadeAsNames);
    auto concatenateAll = [&] {
        return emitter<StringEmitter>().concatenateKey(value, primitives.span(), keyUse);
    };
    for (unsigned index : { 1u, 0u }) {
        if (parts.size() != 2 || !typeOf(parts[index]).isString())
            continue;
        Condition textIsEmpty = equal(primitives[index], boxCell(emptyString()));
        Boxed other = primitives[1 - index];
        const Type& otherType = typeOf(parts[1 - index]);
        if (otherType.isAnyString()) {
            return selectByBranch<CellPointer>(textIsEmpty, [&] {
                return unboxCell(other);
            }, concatenateAll);
        }
        if (otherType.isNumber()) {
            return selectByBranch<CellPointer>(logicalAnd(textIsEmpty, isInt32(other)), [&] {
                return emitter<StringEmitter>().integerToString(value, unboxInt32(other));
            }, concatenateAll);
        }
    }
    return concatenateAll();
}

Operand OperatorEmitter::addStrings(const ValueContext& value)
{
    StringEmitter& strings = emitter<StringEmitter>();
    ToStringEmitter toStrings(generator());
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    // undefined + null is a number, so only a form with an operand that is always a string has a cell as its result.
    bool resultIsBoxed = value.representation() == Representation::JSValue;
    switch (value->form()) {
    case OperatorForm::Strings:
        return strings.concatenate(value, { in<CellPointer>(left), in<CellPointer>(right) });
    case OperatorForm::StringAndPrimitive: {
        CellPointer first = toStrings.convertAddend(value, left);
        CellPointer second = toStrings.convertAddend(value, right);
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
        bool emptyStringIsOnLeft = typeOf(left).singleStringLiteral() == m_program.strings.emptyString();
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

#endif // ENABLE(AOT)
