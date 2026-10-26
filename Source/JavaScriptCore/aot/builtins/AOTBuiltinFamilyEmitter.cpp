/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinFamilyEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTVector.h"
#include "JSObject.h"

namespace JSC { namespace AOT { namespace Typed {

BuiltinFamilyEmitter::BuiltinFamilyEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Number BuiltinFamilyEmitter::numberOperand(ValueID id)
{
    const Type& type = typeOf(id);
    RELEASE_ASSERT(isLegalRepresentation(type, Representation::Double));
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    switch (representationOf(id)) {
    case Representation::Int32:
        return toNumber(in<Word32>(id));
    case Representation::Int64:
        return conversions.toNumber(in<Integer>(id), type.integers);
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return in<Number>(id);
    case Representation::OptionalInt64:
        return conversions.toNumber(conversions.withoutMarkers(get(id)).as<Integer>(), type.integers);
    case Representation::OptionalDouble:
        return conversions.withoutMarkers(get(id)).as<Number>();
    case Representation::JSValue:
        return numberToDouble(in<Boxed>(id));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Integer BuiltinFamilyEmitter::integerOperand(ValueID id)
{
    const Type& type = typeOf(id);
    RELEASE_ASSERT(isLegalRepresentation(type, Representation::Int64));
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    switch (representationOf(id)) {
    case Representation::Int32:
        return signExtended(in<Word32>(id));
    case Representation::Int64:
        return in<Integer>(id);
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return conversions.toInteger(in<Number>(id));
    case Representation::OptionalInt64:
        return conversions.withoutMarkers(get(id)).as<Integer>();
    case Representation::OptionalDouble:
        return conversions.toInteger(conversions.withoutMarkers(get(id)).as<Number>());
    case Representation::JSValue:
        return conversions.unbox(in<Boxed>(id), type, Representation::Int64).as<Integer>();
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Word32 BuiltinFamilyEmitter::int32Operand(ValueID id)
{
    const Type& type = typeOf(id);
    RELEASE_ASSERT(isLegalRepresentation(type, Representation::Int32));
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    switch (representationOf(id)) {
    case Representation::Int32:
        return in<Word32>(id);
    case Representation::Int64:
        return conversions.narrow(in<Integer>(id));
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return conversions.toInt32(in<Number>(id));
    case Representation::OptionalInt64:
        return conversions.narrow(conversions.withoutMarkers(get(id)).as<Integer>());
    case Representation::OptionalDouble:
        return conversions.toInt32(conversions.withoutMarkers(get(id)).as<Number>());
    case Representation::JSValue:
        return conversions.unbox(in<Boxed>(id), type, Representation::Int32).as<Word32>();
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition BuiltinFamilyEmitter::booleanOperand(ValueID id)
{
    RELEASE_ASSERT(isLegalRepresentation(typeOf(id), Representation::Boolean));
    switch (representationOf(id)) {
    case Representation::Boolean:
        return in<Condition>(id);
    case Representation::JSValue:
        return unboxBoolean(in<Boxed>(id));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer BuiltinFamilyEmitter::getCell(ValueID id)
{
    switch (representationOf(id)) {
    case Representation::Cell:
        return in<CellPointer>(id);
    case Representation::Cell32:
        return decode(in<NarrowReference>(id));
    case Representation::JSValue:
        return unboxCell(in<Boxed>(id));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer BuiltinFamilyEmitter::cellOperand(ValueID id)
{
    RELEASE_ASSERT(isLegalRepresentation(typeOf(id), Representation::Cell));
    return getCell(id);
}

Boxed BuiltinFamilyEmitter::boxedOperand(const OperationContext& context, ValueID id)
{
    if (representationOf(id) == Representation::JSValue)
        return in<Boxed>(id);
    return emitter<ConversionEmitter>().box(context, get(id), typeOf(id));
}

CellPointer BuiltinFamilyEmitter::objectOrElse(ValueID id, LBasicBlock otherwise)
{
    const Type& type = typeOf(id);
    if (type.isObject())
        return cellOperand(id);
    if (!type.hasObjects()) {
        m_out.jump(otherwise);
        m_out.appendTo(newBlock());
        return CellPointer(m_out.intPtrZero);
    }

    const TypeTestPlan& plan = typeTestPlanFor(m_function.at(id).type, representationOf(id), TypeTable::object);
    LBasicBlock isObject = newBlock();
    branchOnType(get(id), plan, { normalEdge(isObject), rareEdge(otherwise) });
    m_out.appendTo(isObject);
    return getCell(id);
}

CellPointer BuiltinFamilyEmitter::objectOrMiss(const InlineCall& call, ValueID id)
{
    if (call.canMiss())
        return objectOrElse(id, missBlockOf(call));
    RELEASE_ASSERT(typeOf(id).isObject());
    return cellOperand(id);
}

void BuiltinFamilyEmitter::missUnless(const InlineCall& call, Condition mustHold)
{
    RELEASE_ASSERT(call.canMiss());
    branchUnless(mustHold, missBlockOf(call));
}

Operand BuiltinFamilyEmitter::inRepresentation(const OperationContext& context, Representation wanted, Operand produced, const Type& producedType)
{
    return emitter<ConversionEmitter>().inRepresentation(context, produced, producedType, wanted);
}

void BuiltinFamilyEmitter::defineAs(const ValueContext& value, Operand produced, const Type& producedType)
{
    define(value, inRepresentation(value, value.representation(), produced, producedType));
}

void BuiltinFamilyEmitter::finish(const InlineCall& call, Operand produced, const Type& producedType)
{
    finishInlineCall(call, inRepresentation(call.context(), resultRepresentationOf(call), produced, producedType));
}

void BuiltinFamilyEmitter::finish(const InlineCall& call, Number number)
{
    finish(call, number, m_program.at(TypeTable::number));
}

void BuiltinFamilyEmitter::finish(const InlineCall& call, Integer integer, Range range)
{
    finish(call, integer, Type::integer(range));
}

void BuiltinFamilyEmitter::finish(const InlineCall& call, Condition condition)
{
    finish(call, condition, m_program.at(TypeTable::boolean));
}

void BuiltinFamilyEmitter::finish(const InlineCall& call, CellPointer cell)
{
    finish(call, cell, resultTypeOf(call));
}

void BuiltinFamilyEmitter::finish(const InlineCall& call, Boxed value)
{
    finish(call, value, resultTypeOf(call));
}

void BuiltinFamilyEmitter::finishWithUndefined(const InlineCall& call)
{
    if (resultRepresentationOf(call) == Representation::Void) {
        finishInlineCall(call, Operand());
        return;
    }
    finish(call, undefined(), m_program.at(TypeTable::undefined));
}

void BuiltinFamilyEmitter::finishWithResults(const InlineCall& call, std::span<const Operand> results)
{
    RELEASE_ASSERT(call.value() && !call.canMiss());
    TypingContext typing { m_program, m_function, types() };
    Vector<Operand, 8> converted;
    converted.reserveInitialCapacity(results.size());
    for (uint32_t index = 0; index < results.size(); ++index) {
        Type type = computeResultType(typing, call.value()->value(), index);
        converted.append(inRepresentation(call.context(), defaultRepresentationOf(type), results[index], type));
    }
    defineResults(*call.value(), converted.span());
}

TypedPointer BuiltinFamilyEmitter::inlineProperty(CellPointer object, PropertyOffset offset)
{
    return m_out.address(m_heaps.properties.atAnyNumber(), object.value(), JSObject::offsetOfInlineStorage() + offset * sizeof(EncodedJSValue));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
