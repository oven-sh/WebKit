/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTFunctionAnalyses.h"

namespace JSC { namespace AOT { namespace Typed {

RepresentationEmitter::RepresentationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Operand RepresentationEmitter::convert(const OperationContext& context, Opcode conversion, ValueID sourceID, Operand source, const Type& type, Representation to)
{
    using enum Representation;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    const Representation from = source.representation;

    auto numberUnboxing = [&] {
        using enum ConversionEmitter::NumberUnboxing;
        if ((to != Double && to != OptionalDouble) || !sourceID || !analyses().cfg().hasCycle())
            return Branches;
        const NaturalLoops& loops = analyses().naturalLoops();
        const NaturalLoop* loop = loops.innerMostLoopOf(context.block());
        return loop && !loops.belongsTo(analyses().useLists().blockOf(sourceID), *loop) ? ComputesBothSides : Branches;
    };

    switch (conversion) {
    case Opcode::Box:
        if (to == JSValue && RepresentationSet { Void, Boolean, Int32, Int64, Double, OptionalInt64, OptionalDouble, Cell }.contains(from))
            return conversions.box(context, source, type);
        if ((to == OptionalInt64 && RepresentationSet { Void, Int32, Int64 }.contains(from)) || (to == OptionalDouble && RepresentationSet { Void, Double, OptionalInt64 }.contains(from)))
            return conversions.toOptional(source, type, to);
        break;
    case Opcode::Unbox:
        if (from == JSValue && RepresentationSet { Boolean, Int32, Int64, Double, OptionalInt64, OptionalDouble, Cell }.contains(to))
            return conversions.unbox(source.as<Boxed>(), type, to, numberUnboxing());
        if (from == OptionalInt64 && to == Int32)
            return conversions.narrow(Integer(source.value));
        if ((from == OptionalInt64 && to == Int64) || (from == OptionalDouble && to == Double))
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
            return conversions.narrow(conversions.toInteger(source.as<Number>()));
        if (from == Double && to == Int64)
            return conversions.toInteger(source.as<Number>());
        break;
    case Opcode::WidenInteger:
        if (from == Int32 && to == Int64)
            return conversions.widen(source.as<Word32>(), type.integers);
        break;
    case Opcode::NarrowInteger:
        if (from == Int64 && to == Int32)
            return conversions.narrow(source.as<Integer>());
        break;
    case Opcode::Decode:
        if (from == Cell32 && (to == Cell || to == JSValue))
            return conversions.decode(source.as<NarrowReference>(), type, to);
        if (from == LiteralString && (to == Cell || to == JSValue))
            return Operand(to, conversions.boxLiteralString(context, source.as<LiteralStringWord>(), type).value());
        break;
    case Opcode::Compress:
        if (to == Cell32 && RepresentationSet { Void, Cell, JSValue }.contains(from))
            return conversions.compress(source, type);
        if (to == LiteralString && RepresentationSet { Void, Cell, JSValue }.contains(from))
            return conversions.toLiteralString(source, type);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return emitter<ConstantEmitter>().zeroOf(to);
}

void RepresentationEmitter::generate(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    define(value, convert(value, value.opcode(), source, get(source), typeOf(source), value.representation()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
