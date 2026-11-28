/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr std::optional<Opcode> conversionStepFromVoid(Representation to)
{
    using enum Representation;
    switch (to) {
    case OptionalInt64: case OptionalDouble: case JSValue:
        return Opcode::Box;
    case Cell32:
        return Opcode::Compress;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case Cell: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromBoolean(Representation to)
{
    using enum Representation;
    switch (to) {
    case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell: case Cell32: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromInt32(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int64:
        return Opcode::WidenInteger;
    case Double:
        return Opcode::IntegerToNumber;
    case OptionalInt64: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int32: case OptionalDouble: case Cell: case Cell32: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromInt64(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int32:
        return Opcode::NarrowInteger;
    case Double:
        return Opcode::IntegerToNumber;
    case OptionalInt64: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int64: case OptionalDouble: case Cell: case Cell32: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromDouble(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int32: case Int64:
        return Opcode::NumberToInteger;
    case OptionalDouble: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Double: case OptionalInt64: case Cell: case Cell32: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromOptionalInt64(Representation to)
{
    using enum Representation;
    switch (to) {
    case Int32: case Int64:
        return Opcode::Unbox;
    case OptionalDouble: case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Double: case OptionalInt64: case Cell: case Cell32: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromOptionalDouble(Representation to)
{
    using enum Representation;
    switch (to) {
    case Double: case OptionalInt64:
        return Opcode::Unbox;
    case JSValue:
        return Opcode::Box;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case OptionalDouble: case Cell: case Cell32: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromCell(Representation to)
{
    using enum Representation;
    switch (to) {
    case JSValue:
        return Opcode::Box;
    case Cell32:
        return Opcode::Compress;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromEncodedCell(Representation to)
{
    using enum Representation;
    switch (to) {
    case Cell: case JSValue:
        return Opcode::Decode;
    case Unchosen: case Void: case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell32: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr std::optional<Opcode> conversionStepFromJSValue(Representation to)
{
    using enum Representation;
    switch (to) {
    case Boolean: case Int32: case Int64: case Double: case OptionalInt64: case OptionalDouble: case Cell:
        return Opcode::Unbox;
    case Cell32:
        return Opcode::Compress;
    case Unchosen: case Void: case JSValue: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static ALWAYS_INLINE constexpr std::optional<Opcode> singleConversionStep(Representation from, Representation to)
{
    using enum Representation;
    switch (from) {
    case Void:
        return conversionStepFromVoid(to);
    case Boolean:
        return conversionStepFromBoolean(to);
    case Int32:
        return conversionStepFromInt32(to);
    case Int64:
        return conversionStepFromInt64(to);
    case Double:
        return conversionStepFromDouble(to);
    case OptionalInt64:
        return conversionStepFromOptionalInt64(to);
    case OptionalDouble:
        return conversionStepFromOptionalDouble(to);
    case Cell:
        return conversionStepFromCell(to);
    case Cell32:
        return conversionStepFromEncodedCell(to);
    case JSValue:
        return conversionStepFromJSValue(to);
    case Unchosen: case RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr RepresentationSet representationsNeverConvertedFrom { Representation::Unchosen, Representation::RawWord };
static constexpr RepresentationSet representationsNeverConvertedTo { Representation::Unchosen, Representation::Void, Representation::RawWord };

static constexpr bool conversionTableIsWellFormed()
{
    for (Representation from : allMembersOf<Representation>) {
        for (Representation to : allMembersOf<Representation>) {
            if (!singleConversionStep(from, to))
                continue;
            if (from == to || representationsNeverConvertedFrom.contains(from) || representationsNeverConvertedTo.contains(to))
                return false;
        }
    }
    return true;
}

static_assert(conversionTableIsWellFormed());

ConversionInSteps findConversionInSteps(TypeReader types, TypeID proven, Representation from, Representation to)
{
    if (from == to)
        return { .isFound = true };

    if (!isLegalRepresentation(types, proven, to))
        return { };
    if (from == Representation::Void && !isLegalRepresentation(types, proven, Representation::Void))
        return { };

    if (std::optional<Opcode> opcode = singleConversionStep(from, to))
        return { .steps = { { ConversionStep { *opcode, to } } }, .numberOfSteps = 1, .isFound = true };

    for (Representation intermediate : { Representation::Int64, Representation::Double, Representation::Cell, Representation::JSValue }) {
        if (!isLegalRepresentation(types, proven, intermediate))
            continue;
        std::optional<Opcode> first = singleConversionStep(from, intermediate);
        std::optional<Opcode> second = singleConversionStep(intermediate, to);
        if (first && second)
            return { .steps = { { ConversionStep { *first, intermediate }, ConversionStep { *second, to } } }, .numberOfSteps = 2, .isFound = true };
    }
    return { };
}

std::optional<ConversionPath> findConversion(TypeReader types, TypeID proven, Representation from, Representation to)
{
    ConversionInSteps found = findConversionInSteps(types, proven, from, to);
    if (!found.isFound)
        return std::nullopt;
    ConversionPath path;
    for (uint8_t index = 0; index < found.numberOfSteps; ++index)
        path.append(found.steps[index]);
    return path;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
