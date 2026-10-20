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
#include "AOTStringEmitter.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsValues.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeTable.h"
#include "AOTTypeTestPlan.h"
#include "JSType.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

class ToStringEmitter final : public OperationEmitter {
public:
    explicit ToStringEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    CellPointer convert(const OperationContext&, Operand, const Type&, OperatorForm);
    CellPointer convertAddend(const OperationContext&, ValueID);

private:
    OperatorForm formOf(const OperationContext&, const Type&);
    OperatorForm formOfBoxedValue(const OperationContext&, const Type&);
    CellPointer callToString(const OperationContext&, Boxed, const CallOptions& = { });
    CellPointer convertNumber(const OperationContext&, Operand, OperatorForm);
    CellPointer convertPossibleString(const OperationContext&, Boxed, const Type&, OperatorForm);
    CellPointer failWithoutString(const OperationContext&);
};

CellPointer ToStringEmitter::failWithoutString(const OperationContext& context)
{
    fail(context, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(Representation::Cell).as<CellPointer>();
}

CellPointer ToStringEmitter::callToString(const OperationContext& context, Boxed boxed, const CallOptions& options)
{
    return callOperation<CellPointer>(context, RuntimeOperation::ToString, { boxed }, options);
}

CellPointer ToStringEmitter::convertNumber(const OperationContext& context, Operand operand, OperatorForm form)
{
    bool callsOperation = form == OperatorForm::NumberByOperation;
    switch (operand.representation) {
    case Representation::Int32:
        return emitter<StringEmitter>().integerToString(context, operand.as<Word32>());
    case Representation::Int64:
        return emitter<StringEmitter>().integerToString(context, operand.as<Integer>());
    case Representation::Double: {
        if (callsOperation)
            return callOperation<CellPointer>(context, RuntimeOperation::DoubleToString, { operand });
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Number value = operand.as<Number>();
        Word32 whole = low32(conversions.toInteger(value));
        return selectByBranch<CellPointer>(equal(conversions.bitsOf(toNumber(whole)), conversions.bitsOf(value)), [&] {
            return emitter<StringEmitter>().integerToString(context, whole);
        }, [&] {
            return emitter<StringEmitter>().numberToString(context, value);
        });
    }
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
    return failWithoutString(context);
}

CellPointer ToStringEmitter::convertPossibleString(const OperationContext& context, Boxed boxed, const Type& type, OperatorForm form)
{
    bool everyCellIsString = !type.hasObjects() && !(type.primitives & (Primitive::AnySymbol | Primitive::BigInt));
    LBasicBlock cellCase = newBlock();
    LBasicBlock stringCase = newBlock();
    LBasicBlock notCellCase = newBlock();
    LBasicBlock otherCase = newBlock();
    Join<CellPointer> join(m_out);
    branch(isCell(boxed), { normalEdge(cellCase), normalEdge(notCellCase) });

    m_out.appendTo(cellCase);
    if (everyCellIsString)
        m_out.jump(stringCase);
    else
        branch(hasCellType(unboxCell(boxed), StringType), { normalEdge(stringCase), normalEdge(otherCase) });

    m_out.appendTo(stringCase);
    join.add(unboxCell(boxed));

    m_out.appendTo(notCellCase);
    if (form == OperatorForm::MayBeStringOrNumber) {
        LBasicBlock int32Case = newBlock();
        branch(isInt32(boxed), { normalEdge(int32Case), normalEdge(otherCase) });
        m_out.appendTo(int32Case);
        join.add(emitter<StringEmitter>().integerToString(context, unboxInt32(boxed)));
    } else
        m_out.jump(otherCase);

    m_out.appendTo(otherCase);
    join.add(callToString(context, boxed));
    return join.finish();
}

CellPointer ToStringEmitter::convert(const OperationContext& context, Operand operand, const Type& type, OperatorForm form)
{
    switch (form) {
    case OperatorForm::Generic:
        return callToString(context, operand.as<Boxed>());
    case OperatorForm::String:
        return operand.as<CellPointer>();
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
        return convertNumber(context, operand, form);
    case OperatorForm::NullableString: {
        Boxed boxed = operand.as<Boxed>();
        return either<CellPointer>(isCell(boxed), [&] {
            return unboxCell(boxed);
        }, [&] {
            return callToString(context, boxed, { .isOnRarePath = true });
        });
    }
    case OperatorForm::NullableNumber: {
        Boxed boxed = operand.as<Boxed>();
        return either<CellPointer>(isInt32(boxed), [&] {
            return emitter<StringEmitter>().integerToString(context, unboxInt32(boxed));
        }, [&] {
            return callToString(context, boxed, { .isOnRarePath = true });
        });
    }
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
    return failWithoutString(context);
}

OperatorForm ToStringEmitter::formOf(const OperationContext& context, const Type& type)
{
    TypingContext typing { m_program, m_function, types() };
    OperatorSite site { typing, analyses().effectContext(), context.shape() };
    OperatorTypes operatorTypes { type, type, m_program.at(TypeTable::string) };
    return chooseConversionForm(site, Opcode::ToString, operatorTypes);
}

OperatorForm ToStringEmitter::formOfBoxedValue(const OperationContext& context, const Type& type)
{
    OperatorForm form = formOf(context, type);
    // The two forms for a number take it unboxed.
    if (form == OperatorForm::Number)
        return OperatorForm::NullableNumber;
    if (form == OperatorForm::NumberByOperation)
        return OperatorForm::Generic;
    return form;
}

CellPointer ToStringEmitter::convertAddend(const OperationContext& context, ValueID id)
{
    Operand operand = get(id);
    if (operand.is<CellPointer>())
        return operand.as<CellPointer>();
    const Type& type = typeOf(id);
    return convert(context, operand, type, operand.is<Boxed>() ? formOfBoxedValue(context, type) : formOf(context, type));
}

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
    Boxed boxed = emitter<ConversionEmitter>().box(context, operand, type);
    return ToStringEmitter(generator()).convert(context, boxed, type, OperatorForm::Generic);
}

void OperatorEmitter::generateStrcat(const ValueContext& value)
{
    std::span<const ValueID> parts = value->partsOperands();
    if (!value->isLikelyKey()) {
        ToStringEmitter toStrings(generator());
        Vector<CellPointer, 8> fibers;
        for (ValueID part : parts)
            fibers.append(toStrings.convertAddend(value, part));
        define(value, emitter<StringEmitter>().concatenate(value, fibers.span()));
        return;
    }
    if (parts.empty()) {
        define(value, emptyString());
        return;
    }
    if (parts.size() == 1) {
        define(value, toString(value, get(parts[0]), typeOf(parts[0])));
        return;
    }
    Vector<Boxed, 8> primitives;
    for (ValueID part : parts) {
        const Type& type = typeOf(part);
        bool conversionIsObservable = type.hasObjects() || type.primitives & Primitive::AnySymbol;
        primitives.append(conversionIsObservable ? boxCell(toString(value, get(part), type)) : in<Boxed>(part));
    }
    define(value, emitter<StringEmitter>().concatenateValues(value, primitives.span(), value->isLikelyKey()));
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
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

void OperatorEmitter::generateToPropertyKey(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    define(value, unboxCell(toPropertyKey(value, in<Boxed>(operand), typeOf(operand))));
}

Boxed OperatorEmitter::toPropertyKey(const OperationContext& context, Boxed value, const Type& type)
{
    auto convert = [&] {
        return Boxed(callStub(context, AOT::toPropertyKey(), { value.value() }).first);
    };
    TypeID keyType = types().intern(Type::fromPrimitives(Primitive::String | Primitive::Symbol));
    const TypeTestPlan& plan = typeTestPlanFor(types().intern(type), Representation::JSValue, keyType);
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        return value;
    case TypeTestVerdict::NeverPasses:
        return convert();
    case TypeTestVerdict::NeedsTest:
        break;
    }

    LBasicBlock isKey = newBlock();
    LBasicBlock isNotKey = newBlock();
    Join<Boxed> join(m_out);
    branchOnType(value, plan, { normalEdge(isKey), rareEdge(isNotKey) });

    m_out.appendTo(isKey);
    join.add(value);

    m_out.appendTo(isNotKey);
    join.add(convert());
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
