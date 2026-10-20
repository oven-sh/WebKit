/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentation.h"

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(maxTrackedInteger == AOT::maxSlotInteger);

static constexpr uint16_t markerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;
static constexpr uint16_t literalPrimitives = Primitive::LiteralString | Primitive::LiteralSymbol;

namespace {

struct RepresentableSet {
    uint16_t primitives { 0 };
    Range integers { Range::empty() };
    bool hasObjects { false };
};

} // anonymous namespace

static constexpr RepresentableSet representableSetOf(Representation representation)
{
    switch (representation) {
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::RawWord:
        return { };
    case Representation::Boolean:
        return { Primitive::Boolean };
    case Representation::Int32:
        return { Primitive::Integer, Range::int32() };
    case Representation::Int64:
        return { Primitive::Integer, Range::full() };
    case Representation::Double:
        return { Primitive::Number, Range::full() };
    case Representation::OptionalInt64:
        return { Primitive::Integer | markerPrimitives, Range::full() };
    case Representation::OptionalDouble:
        return { Primitive::Number | markerPrimitives, Range::full() };
    case Representation::Cell:
        return { cellPrimitives, Range::empty(), true };
    case Representation::Cell32:
        return { cellPrimitives | markerPrimitives, Range::empty(), true };
    case Representation::JSValue:
        return { Primitive::Defined, Range::full(), true };
    case Representation::LiteralString:
        return { Primitive::AnyString | markerPrimitives };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static bool isSingleMarker(const Type& type)
{
    return type.hasOnlyPrimitives(markerPrimitives) && hasOneBitSet(type.primitives);
}

static std::optional<Representation> internalRepresentationOf(const Type& type)
{
    if (type.opaque)
        return representationOf(*type.opaque);
    if (type.environment)
        return Representation::Cell;
    return std::nullopt;
}

static bool canHoldInternalValue(Representation representation, Representation internal, bool mayBeAbsent)
{
    if (internal != Representation::Cell)
        return representation == internal;
    if (representation == Representation::Cell)
        return !mayBeAbsent;
    return representation == Representation::Cell32 || representation == Representation::JSValue;
}

Representation defaultRepresentationOf(const Type& type)
{
    if (auto internal = internalRepresentationOf(type)) {
        if (*internal == Representation::Cell && type.primitives)
            return Representation::JSValue;
        return *internal;
    }
    if (type.isNever() || isSingleMarker(type))
        return Representation::Void;
    if (type.isBoolean())
        return Representation::Boolean;
    if (type.isInteger())
        return type.integers.isSubsetOf(Range::int32()) ? Representation::Int32 : Representation::Int64;
    if (type.isNumber())
        return Representation::Double;
    if (type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | markerPrimitives))
        return type.primitives & Primitive::Double ? Representation::OptionalDouble : Representation::OptionalInt64;
    if (type.hasLiteralStrings() && type.hasOnlyPrimitives(Primitive::LiteralString | markerPrimitives))
        return Representation::LiteralString;
    if (!(type.primitives & ~cellPrimitives))
        return Representation::Cell;
    return Representation::JSValue;
}

Representation representationOf(OpaqueKind kind)
{
    switch (kind) {
    case OpaqueKind::Guard:
        return Representation::Void;
    case OpaqueKind::IteratorState:
    case OpaqueKind::Enumerator:
    case OpaqueKind::Exception:
    case OpaqueKind::ClassToken:
    case OpaqueKind::StringBuilder:
    case OpaqueKind::FlatString:
        return Representation::Cell;
    case OpaqueKind::Cursor:
    case OpaqueKind::KeyOrder:
    case OpaqueKind::CodeAddress:
    case OpaqueKind::BuilderCursor:
        return Representation::RawWord;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation representationOf(SlotRepresentation slot)
{
    switch (slot) {
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Int32:
        return Representation::Int32;
    case SlotRepresentation::Boolean:
        return Representation::Boolean;
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
        return Representation::Int64;
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
        return Representation::Double;
    case SlotRepresentation::Cell:
        return Representation::Cell;
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        return Representation::Cell32;
    case SlotRepresentation::JSValue:
    case SlotRepresentation::OptionalBoolean:
        return Representation::JSValue;
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalInt32:
        return Representation::OptionalInt64;
    case SlotRepresentation::OptionalDouble:
    case SlotRepresentation::Int32OrDouble:
        return Representation::OptionalDouble;
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
        return Representation::LiteralString;
    case SlotRepresentation::DeferredClosure:
        return Representation::RawWord;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isLegalRepresentation(const Type& type, Representation representation)
{
    if (representation == Representation::Unchosen)
        return false;
    if (type.isNever())
        return true;
    if (auto internal = internalRepresentationOf(type))
        return canHoldInternalValue(representation, *internal, type.primitives & Primitive::Absent);
    if (representation == Representation::Void)
        return isSingleMarker(type);
    RepresentableSet set = representableSetOf(representation);
    if (type.primitives & ~set.primitives)
        return false;
    if (!type.integers.isSubsetOf(set.integers))
        return false;
    return set.hasObjects || !type.hasObjects();
}

static SlotRepresentation narrowestIntegerSlotFor(Range integers)
{
    auto fits = [&](int64_t minimum, int64_t maximum) {
        return integers.isSubsetOf({ minimum, maximum });
    };
    if (fits(0, UINT8_MAX))
        return SlotRepresentation::Uint8;
    if (fits(INT8_MIN, INT8_MAX))
        return SlotRepresentation::Int8;
    if (fits(0, UINT16_MAX))
        return SlotRepresentation::Uint16;
    if (fits(INT16_MIN, INT16_MAX))
        return SlotRepresentation::Int16;
    if (fits(INT32_MIN, INT32_MAX))
        return SlotRepresentation::Int32;
    if (fits(0, UINT32_MAX))
        return SlotRepresentation::Uint32;
    return SlotRepresentation::Int64;
}

SlotRepresentation defaultSlotRepresentationOf(const Type& type)
{
    switch (defaultRepresentationOf(type)) {
    case Representation::Boolean:
        return SlotRepresentation::Boolean;
    case Representation::Int32:
    case Representation::Int64:
        return narrowestIntegerSlotFor(type.integers);
    case Representation::RawWord:
        return SlotRepresentation::Int64;
    case Representation::Double:
        return SlotRepresentation::Double;
    case Representation::OptionalInt64:
        return SlotRepresentation::OptionalInt64;
    case Representation::OptionalDouble:
        return SlotRepresentation::OptionalDouble;
    case Representation::Cell:
        return SlotRepresentation::Cell;
    case Representation::Void:
    case Representation::JSValue:
        return SlotRepresentation::JSValue;
    case Representation::LiteralString:
        return SlotRepresentation::LiteralString;
    case Representation::Unchosen:
    case Representation::Cell32:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type widestTypeOf(Representation representation)
{
    RepresentableSet set = representableSetOf(representation);
    Type type = Type::fromPrimitives(set.primitives & ~literalPrimitives);
    type.integers = set.integers;
    type.anyObject = set.hasObjects;
    type.anyFunction = set.hasObjects;
    return type;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
