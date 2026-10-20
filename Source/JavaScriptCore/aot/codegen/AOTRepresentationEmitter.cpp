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
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class Conversion final : public OperationEmitter {
public:
    using Converter = std::optional<Operand> (Conversion::*)();

    Conversion(CodeGenerator&, const ValueContext&);

    void generate(Converter);

    std::optional<Operand> box();
    std::optional<Operand> unbox();
    std::optional<Operand> integerToNumber();
    std::optional<Operand> numberToInteger();
    std::optional<Operand> widenInteger();
    std::optional<Operand> narrowInteger();
    std::optional<Operand> decode();
    std::optional<Operand> compress();

private:
    std::optional<Operand> boxToValue();
    std::optional<Operand> boxToOptionalInteger();
    std::optional<Operand> boxToOptionalNumber();
    std::optional<Operand> unboxValue();
    std::optional<Operand> unboxOptionalInteger();
    std::optional<Operand> unboxOptionalNumber();
    std::optional<Operand> decodeNarrowReference();
    std::optional<Operand> decodeLiteralString();
    std::optional<Operand> compressToNarrowReference();
    std::optional<Operand> compressToLiteralString();
    ConversionEmitter::NumberUnboxing numberUnboxing();

    const ValueContext& m_value;
    const Operand m_source;
    const Type& m_type;
    const Representation m_to;
    ConversionEmitter& m_conversions;
};

}

Conversion::Conversion(CodeGenerator& generator, const ValueContext& value)
    : OperationEmitter(generator)
    , m_value(value)
    , m_source(get(value->valueOperand()))
    , m_type(typeOf(value->valueOperand()))
    , m_to(value.representation())
    , m_conversions(emitter<ConversionEmitter>())
{
}

void Conversion::generate(Converter convert)
{
    std::optional<Operand> converted = (this->*convert)();
    if (!converted) {
        fail(m_value, InternalErrorKind::UnsupportedConversion);
        converted = emitter<ConstantEmitter>().zeroOf(m_to);
    }
    define(m_value, *converted);
}

std::optional<Operand> Conversion::box()
{
    switch (m_to) {
    case Representation::JSValue:
        return boxToValue();
    case Representation::OptionalInt64:
        return boxToOptionalInteger();
    case Representation::OptionalDouble:
        return boxToOptionalNumber();
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::boxToValue()
{
    switch (m_source.representation) {
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
        return m_conversions.box(m_value, m_source, m_type);
    case Representation::Unchosen:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::boxToOptionalInteger()
{
    switch (m_source.representation) {
    case Representation::Void:
    case Representation::Int32:
    case Representation::Int64:
        return m_conversions.toOptional(m_source, m_type, Representation::OptionalInt64);
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::boxToOptionalNumber()
{
    switch (m_source.representation) {
    case Representation::Void:
    case Representation::Double:
    case Representation::OptionalInt64:
        return m_conversions.toOptional(m_source, m_type, Representation::OptionalDouble);
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::unbox()
{
    switch (m_source.representation) {
    case Representation::JSValue:
        return unboxValue();
    case Representation::OptionalInt64:
        return unboxOptionalInteger();
    case Representation::OptionalDouble:
        return unboxOptionalNumber();
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ConversionEmitter::NumberUnboxing Conversion::numberUnboxing()
{
    if (m_to != Representation::Double && m_to != Representation::OptionalDouble)
        return ConversionEmitter::NumberUnboxing::Branches;

    const NaturalLoops& loops = analyses().naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(m_value.block());
    bool isInvariantInLoop = loop && !loops.belongsTo(analyses().useLists().blockOf(m_value->valueOperand()), *loop);
    return isInvariantInLoop ? ConversionEmitter::NumberUnboxing::ComputesBothSides : ConversionEmitter::NumberUnboxing::Branches;
}

std::optional<Operand> Conversion::unboxValue()
{
    switch (m_to) {
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
        return m_conversions.unbox(m_source.as<Boxed>(), m_type, m_to, numberUnboxing());
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::unboxOptionalInteger()
{
    switch (m_to) {
    case Representation::Int32:
        return m_conversions.narrow(m_conversions.withoutMarkers(m_source).as<Integer>());
    case Representation::Int64:
        return m_conversions.withoutMarkers(m_source);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::unboxOptionalNumber()
{
    switch (m_to) {
    case Representation::Double:
        return m_conversions.withoutMarkers(m_source);
    case Representation::OptionalInt64:
        return m_conversions.toOptional(m_source, m_type, Representation::OptionalInt64);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::integerToNumber()
{
    if (m_to != Representation::Double)
        return std::nullopt;
    switch (m_source.representation) {
    case Representation::Int32:
        return toNumber(m_source.as<Word32>());
    case Representation::Int64:
        return m_conversions.toNumber(m_source.as<Integer>(), m_type.integers);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::numberToInteger()
{
    if (!m_source.is<Number>())
        return std::nullopt;
    switch (m_to) {
    case Representation::Int32:
        return m_conversions.narrow(m_conversions.toInteger(m_source.as<Number>()));
    case Representation::Int64:
        return m_conversions.toInteger(m_source.as<Number>());
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::widenInteger()
{
    if (!m_source.is<Word32>() || m_to != Representation::Int64)
        return std::nullopt;
    return m_conversions.widen(m_source.as<Word32>(), m_type.integers);
}

std::optional<Operand> Conversion::narrowInteger()
{
    if (!m_source.is<Integer>() || m_to != Representation::Int32)
        return std::nullopt;
    return m_conversions.narrow(m_source.as<Integer>());
}

std::optional<Operand> Conversion::decode()
{
    switch (m_source.representation) {
    case Representation::Cell32:
        return decodeNarrowReference();
    case Representation::LiteralString:
        return decodeLiteralString();
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::decodeNarrowReference()
{
    switch (m_to) {
    case Representation::Cell:
    case Representation::JSValue:
        return m_conversions.decode(m_source.as<NarrowReference>(), m_type, m_to);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::decodeLiteralString()
{
    switch (m_to) {
    case Representation::Cell:
        return unboxCell(m_conversions.boxLiteralString(m_value, m_source.as<LiteralStringWord>(), m_type));
    case Representation::JSValue:
        return m_conversions.boxLiteralString(m_value, m_source.as<LiteralStringWord>(), m_type);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::compress()
{
    switch (m_to) {
    case Representation::Cell32:
        return compressToNarrowReference();
    case Representation::LiteralString:
        return compressToLiteralString();
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::compressToNarrowReference()
{
    switch (m_source.representation) {
    case Representation::Void:
    case Representation::Cell:
    case Representation::JSValue:
        return m_conversions.compress(m_source, m_type);
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Conversion::compressToLiteralString()
{
    switch (m_source.representation) {
    case Representation::Void:
    case Representation::Cell:
    case Representation::JSValue:
        return m_conversions.toLiteralString(m_source, m_type);
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RepresentationEmitter::RepresentationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void RepresentationEmitter::generateBox(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::box);
}

void RepresentationEmitter::generateUnbox(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::unbox);
}

void RepresentationEmitter::generateIntegerToNumber(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::integerToNumber);
}

void RepresentationEmitter::generateNumberToInteger(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::numberToInteger);
}

void RepresentationEmitter::generateWidenInteger(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::widenInteger);
}

void RepresentationEmitter::generateNarrowInteger(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::narrowInteger);
}

void RepresentationEmitter::generateDecode(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::decode);
}

void RepresentationEmitter::generateCompress(const ValueContext& value)
{
    Conversion(generator(), value).generate(&Conversion::compress);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
