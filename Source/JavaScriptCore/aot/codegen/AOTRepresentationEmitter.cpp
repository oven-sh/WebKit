/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayIntrinsicEmitter.h"
#include "AOTCastEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTThrowEmitter.h"
#include "IndexingHeader.h"

namespace JSC { namespace AOT { namespace Typed {

RepresentationEmitter::RepresentationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Integer RepresentationEmitter::heldToWhatConversionGives(const OperationContext& context, Number number, Integer integer)
{
    if (emittersCheckProofs())
        emitter<ThrowEmitter>().requireOrCrash(context, equal(integer, emitter<ConversionEmitter>().toInteger(number)), std::nullopt, ProofSource::OtherOperation);
    return integer;
}

Operand RepresentationEmitter::convert(const OperationContext& context, Opcode conversion, Operand source, const Type& type, Representation to)
{
    using enum Representation;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    const Representation from = source.representation;

    switch (conversion) {
    case Opcode::Box:
        if (to == JSValue && RepresentationSet { Void, Boolean, Int32, Int64, Double, OptionalInt64, OptionalDouble, Cell }.contains(from))
            return conversions.box(context, source, type);
        if ((to == OptionalInt64 && RepresentationSet { Void, Int32, Int64 }.contains(from)) || (to == OptionalDouble && RepresentationSet { Void, Double, OptionalInt64 }.contains(from)))
            return conversions.toOptional(source, type, to);
        break;
    case Opcode::Unbox:
        if (from == JSValue && RepresentationSet { Boolean, Int32, Int64, Double, OptionalInt64, OptionalDouble, Cell }.contains(to))
            return conversions.unbox(source.as<Boxed>(), type, to);
        if (from == OptionalInt64 && to == Int32) {
            if (std::optional<Word32> narrow = narrowWordBehind(source.as<OptionalInteger>()))
                return *narrow;
            return conversions.lowHalfOfIntegerToldInsideInt32(Integer(source.value));
        }
        if (from == OptionalInt64 && to == Int64)
            return conversions.integerKnownNotMarker(source.as<OptionalInteger>());
        if (from == OptionalDouble && to == Double)
            return conversions.withoutMarkers(source);
        if (from == OptionalDouble && to == OptionalInt64)
            return conversions.toOptional(source, type, to);
        break;
    case Opcode::IntegerToNumber:
        if (from == Int32 && to == Double)
            return toNumber(source.as<Word32>());
        if (from == Int64 && to == Double)
            return conversions.toNumber(source.as<Integer>(), type.integers);
        break;
    case Opcode::NumberToInteger:
        if (from == Double && to == Int32)
            return conversions.narrow(heldToWhatConversionGives(context, source.as<Number>(), conversions.toInteger(source.as<Number>(), type.integers, context.block() && generator().usesLoopForms(context.block()))));
        if (from == Double && to == Int64)
            return heldToWhatConversionGives(context, source.as<Number>(), conversions.toInteger(source.as<Number>(), type.integers, context.block() && generator().usesLoopForms(context.block())));
        break;
    case Opcode::WidenInteger:
        if (from == Int32 && to == Int64)
            return conversions.widen(source.as<Word32>(), type.integers);
        break;
    case Opcode::NarrowInteger:
        if (from == Int64 && to == Int32)
            return conversions.lowHalfOfIntegerToldInsideInt32(source.as<Integer>());
        break;
    case Opcode::Decode:
        if (from == Cell32 && (to == Cell || to == JSValue))
            return conversions.decode(source.as<NarrowReference>(), type, to);
        break;
    case Opcode::Compress:
        if (to == Cell32 && RepresentationSet { Void, Cell, JSValue }.contains(from))
            return conversions.compress(source, type);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return emitter<ConstantEmitter>().zeroOf(to);
}

static bool conversionAsksForNoType(Opcode conversion, Representation from, Representation to)
{
    using enum Representation;
    switch (conversion) {
    case Opcode::Box:
        return to == JSValue && RepresentationSet { Boolean, Int32, Double, Cell }.contains(from);
    case Opcode::Unbox:
        return from == JSValue && RepresentationSet { Boolean, Int32, Int64, Cell }.contains(to);
    case Opcode::IntegerToNumber:
        return from == Int32 && to == Double;
    case Opcode::NarrowInteger:
        return from == Int64 && to == Int32;
    case Opcode::Decode:
        return from == Cell32 && to == Cell;
    case Opcode::Compress:
        return from == Cell && to == Cell32;
    default:
        return false;
    }
}

std::optional<Integer> RepresentationEmitter::integerOfStep(const ValueContext& value)
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement };
    ValueID number = value->valueOperand();
    if (value.representation() != Representation::Int64 || m_code.isConstant(number))
        return std::nullopt;
    const Operation& step = operationAt(number);
    if (!steps.contains(step.opcode) || step.form() != OperatorForm::CheckedIntegers || step.representation != Representation::Double || m_code.blockOf(number) != value.block())
        return std::nullopt;
    std::span<const ValueID> values = m_code.valuesOf(m_code.blockOf(number));
    auto found = std::ranges::find(values, number);
    if (found == values.end() || found + 1 == values.end() || *(found + 1) != value.id())
        return std::nullopt;
    ValueID stepped = step.valueOperand();
    Range range = typeOf(stepped).integers;
    Representation form = representationOf(stepped);
    if (!typeOf(stepped).isInteger() || !range.isSubsetOf(Range::exact()) || (form != Representation::Int32 && form != Representation::Int64))
        return std::nullopt;
    const int64_t one = step.opcode == Opcode::Increment ? 1 : -1;
    Integer operand = form == Representation::Int32 ? signExtended(in<Word32>(stepped)) : in<Integer>(stepped);
    Integer sum = add(operand, integer(one));
    if (one > 0 && range.maximum == maxExactInteger)
        sum = select(lessThan(operand, integer(maxExactInteger)), sum, operand);
    if (one < 0 && range.minimum == -maxExactInteger)
        sum = select(greaterThan(operand, integer(-maxExactInteger)), sum, operand);
    return heldToWhatConversionGives(value, in<Number>(number), sum);
}

void RepresentationEmitter::generate(const ValueContext& value)
{
    ValueID converted = value->valueOperand();
    if (generator().isCarriedBoxed(converted)) [[unlikely]] {
        RELEASE_ASSERT(value.opcode() == Opcode::Box && value.representation() == Representation::JSValue);
        define(value, get(converted));
        return;
    }
    if (generator().isCarriedUnboxed(converted)) [[unlikely]] {
        RELEASE_ASSERT(value.opcode() == Opcode::Unbox && value.representation() == generator().carriedRepresentationOf(converted));
        define(value, Operand(value.representation(), generator().wordOf(converted)));
        return;
    }
    if (std::optional<Boxed> boxed = generator().findBoxed(converted)) [[unlikely]] {
        RELEASE_ASSERT(value.opcode() == Opcode::Box && value.representation() == Representation::JSValue);
        define(value, *boxed);
        return;
    }
    if (ValueID sum = generator().sumBoxedBy(value.value())) [[unlikely]] {
        define(value, emitter<OperatorEmitter>().boxedSum(sum));
        return;
    }
    if (value.opcode() == Opcode::Box && value.representation() == Representation::JSValue && representationOf(converted) == Representation::Int64 && emitter<ArrayIntrinsicEmitter>().standsBehindTestThatFoundElement(value, { }, converted)) {
        define(value, emitter<ConversionEmitter>().boxInteger(in<Integer>(converted), Range { 0, IndexingHeader::maximumLength - 1 }));
        return;
    }
    if (value.opcode() == Opcode::Unbox && generator().isOnlyUsedAsIndex(value.id())) {
        generator().defineBoxed(value, in<Boxed>(converted));
        return;
    }
    if (value.opcode() == Opcode::Unbox || value.opcode() == Opcode::NumberToInteger) {
        if (std::optional<Operand> handedOn = emitter<CastEmitter>().takeIntegerHandedOnTo(value.id())) {
            bool isWidened = value.representation() == Representation::Int64 && handedOn->is<Word32>();
            define(value, isWidened ? Operand(signExtended(handedOn->as<Word32>())) : *handedOn);
            return;
        }
    }
    if (value.opcode() == Opcode::NumberToInteger) {
        if (std::optional<Integer> stepped = integerOfStep(value)) {
            define(value, *stepped);
            return;
        }
    }
    if (value.opcode() == Opcode::IntegerToNumber) {
        if (LValue carried = generator().loopCarriedNumberOf(value.id())) [[unlikely]] {
            define(value, Number(carried));
            return;
        }
        if (isARM64() && generator().rotatesUnrolledIterationNumbers() && value.representation() == Representation::Double && generator().representationOf(converted) == Representation::Int32) [[unlikely]] {
            define(value, Number(generator().numberOfInteger(get(converted).as<Word32>().value())));
            return;
        }
    }
    Operand source = get(converted);
    bool asksForNoType = conversionAsksForNoType(value.opcode(), source.representation, value.representation());
    define(value, convert(value, value.opcode(), source, asksForNoType ? Type() : typeOf(value.id()), value.representation()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
