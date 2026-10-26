/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectedOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTInductionVariables.h"
#include "AOTPromiseIntrinsicEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

SelectedOperatorEmitter::SelectedOperatorEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void SelectedOperatorEmitter::generateIntegerBound(const ValueContext& value)
{
    Opcode relation = value->comparison();
    if (!relationalOperators.contains(relation)) {
        fail(value, InternalErrorKind::PayloadOutOfRange);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    switch (value.representation()) {
    case Representation::Int64:
        break;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    if (value->numberIsOnLeft())
        relation = mirroredComparison(relation);

    bool roundsUp = relation == Opcode::Less || relation == Opcode::GreaterEq;
    bool isFalseAtGreatest = relation == Opcode::Greater || relation == Opcode::GreaterEq;
    constexpr int64_t least = -maxTrackedInteger;
    constexpr int64_t greatest = maxTrackedInteger;
    static_assert(static_cast<double>(greatest + 1) == 0x1p62);
    int64_t boundForNaN = isFalseAtGreatest ? greatest : least;

    Number number = in<Number>(value->numberOperand());
    Integer rounded = emitter<ConversionEmitter>().toInteger(roundsUp ? ceil(number) : floor(number));
    Integer bound = select(equal(number, number), rounded, integer(boundForNaN));
    bound = select(lessThanOrEqual(number, this->number(-0x1p62)), integer(least), bound);
    define(value, select(greaterThanOrEqual(number, this->number(0x1p62)), integer(greatest), bound));
}

void SelectedOperatorEmitter::generateFulfillFreshPromise(const ValueContext& value)
{
    ValueID fulfillment = value->valueOperand();
    emitter<PromiseIntrinsicEmitter>().fulfillAtStore(value, in<CellPointer>(value->promiseOperand()), in<Boxed>(fulfillment), typeOf(fulfillment));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
