/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConversions.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static std::optional<Opcode> stepFromVoid(Representation to)
{
    switch (to) {
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Cell32:
    case Representation::LiteralString:
        return Opcode::Compress;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromBoolean(Representation to)
{
    switch (to) {
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromInt32(Representation to)
{
    switch (to) {
    case Representation::Int64:
        return Opcode::WidenInteger;
    case Representation::Double:
        return Opcode::IntegerToNumber;
    case Representation::OptionalInt64:
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromInt64(Representation to)
{
    switch (to) {
    case Representation::Int32:
        return Opcode::NarrowInteger;
    case Representation::Double:
        return Opcode::IntegerToNumber;
    case Representation::OptionalInt64:
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromDouble(Representation to)
{
    switch (to) {
    case Representation::Int32:
    case Representation::Int64:
        return Opcode::NumberToInteger;
    case Representation::OptionalDouble:
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromOptionalInt64(Representation to)
{
    switch (to) {
    case Representation::Int32:
    case Representation::Int64:
        return Opcode::Unbox;
    case Representation::OptionalDouble:
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromOptionalDouble(Representation to)
{
    switch (to) {
    case Representation::Double:
    case Representation::OptionalInt64:
        return Opcode::Unbox;
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::LiteralString:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromCell(Representation to)
{
    switch (to) {
    case Representation::JSValue:
        return Opcode::Box;
    case Representation::Cell32:
    case Representation::LiteralString:
        return Opcode::Compress;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> stepFromEncodedCell(Representation to)
{
    switch (to) {
    case Representation::Cell:
    case Representation::JSValue:
        return Opcode::Decode;
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

static std::optional<Opcode> stepFromJSValue(Representation to)
{
    switch (to) {
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell:
        return Opcode::Unbox;
    case Representation::Cell32:
    case Representation::LiteralString:
        return Opcode::Compress;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::JSValue:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> singleStep(Representation from, Representation to)
{
    switch (from) {
    case Representation::Void:
        return stepFromVoid(to);
    case Representation::Boolean:
        return stepFromBoolean(to);
    case Representation::Int32:
        return stepFromInt32(to);
    case Representation::Int64:
        return stepFromInt64(to);
    case Representation::Double:
        return stepFromDouble(to);
    case Representation::OptionalInt64:
        return stepFromOptionalInt64(to);
    case Representation::OptionalDouble:
        return stepFromOptionalDouble(to);
    case Representation::Cell:
        return stepFromCell(to);
    case Representation::Cell32:
    case Representation::LiteralString:
        return stepFromEncodedCell(to);
    case Representation::JSValue:
        return stepFromJSValue(to);
    case Representation::Unchosen:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr Representation intermediates[] = {
    Representation::Int64,
    Representation::Double,
    Representation::Cell,
    Representation::JSValue,
};

std::optional<ConversionPath> findConversion(const Type& type, Representation from, Representation to)
{
    if (from == to)
        return ConversionPath { };
    RELEASE_ASSERT(from != Representation::Unchosen && to != Representation::Unchosen && to != Representation::Void);
    if (!isLegalRepresentation(type, to))
        return std::nullopt;

    if (std::optional<Opcode> opcode = singleStep(from, to))
        return ConversionPath { ConversionStep { *opcode, to } };

    for (Representation intermediate : intermediates) {
        if (!isLegalRepresentation(type, intermediate))
            continue;
        std::optional<Opcode> first = singleStep(from, intermediate);
        std::optional<Opcode> second = singleStep(intermediate, to);
        if (first && second)
            return ConversionPath { ConversionStep { *first, intermediate }, ConversionStep { *second, to } };
    }
    return std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
