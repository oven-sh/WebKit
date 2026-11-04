/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentation.h"

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(maxTrackedInteger == AOT::maxSlotInteger);

static constexpr uint16_t representedMarkerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;

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
        return { Primitive::Integer | representedMarkerPrimitives, Range::full() };
    case Representation::OptionalDouble:
        return { Primitive::Number | representedMarkerPrimitives, Range::full() };
    case Representation::Cell:
        return { cellPrimitives, Range::empty(), true };
    case Representation::Cell32:
        return { cellPrimitives | representedMarkerPrimitives, Range::empty(), true };
    case Representation::JSValue:
        return { Primitive::Defined & ~Primitive::Unspecified, Range::full(), true };
    case Representation::LiteralString:
        return { Primitive::AnyString | representedMarkerPrimitives };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static uint16_t representedPrimitivesOf(const Type& type)
{
    return static_cast<uint16_t>(type.primitives & ~Primitive::Unspecified);
}

static constexpr bool isSingleMarker(uint16_t primitives)
{
    return !(primitives & ~representedMarkerPrimitives) && hasOneBitSet(primitives);
}

// For a type without objects. `primitives` excludes Unspecified.
static constexpr Representation representationOfPrimitives(uint16_t primitives, Range integers)
{
    auto hasOnly = [&](uint16_t allowed) {
        return !(primitives & ~allowed);
    };
    if (!primitives || isSingleMarker(primitives))
        return Representation::Void;
    if (hasOnly(Primitive::Boolean))
        return Representation::Boolean;
    if (hasOnly(Primitive::Integer))
        return integers.isSubsetOf(Range::int32()) ? Representation::Int32 : Representation::Int64;
    if (hasOnly(Primitive::Number))
        return Representation::Double;
    if (primitives & Primitive::Number && hasOnly(Primitive::Number | representedMarkerPrimitives))
        return primitives & Primitive::Double ? Representation::OptionalDouble : Representation::OptionalInt64;
    if (primitives & Primitive::LiteralString && hasOnly(Primitive::LiteralString | representedMarkerPrimitives))
        return Representation::LiteralString;
    if (hasOnly(cellPrimitives))
        return Representation::Cell;
    return Representation::JSValue;
}

static_assert(representationOfPrimitives(0, Range::empty()) == Representation::Void);
static_assert(representationOfPrimitives(Primitive::Absent, Range::empty()) == Representation::Void);
static_assert(representationOfPrimitives(Primitive::UndefinedOrNull, Range::empty()) == Representation::JSValue);
static_assert(representationOfPrimitives(Primitive::True, Range::empty()) == Representation::Boolean);
static_assert(representationOfPrimitives(Primitive::Integer, Range::int32()) == Representation::Int32);
static_assert(representationOfPrimitives(Primitive::Integer, Range::uint32()) == Representation::Int64);
static_assert(representationOfPrimitives(Primitive::Integer | Primitive::WholeNumber, Range::one(0)) == Representation::Double);
static_assert(representationOfPrimitives(Primitive::Integer | Primitive::Null, Range::one(0)) == Representation::OptionalInt64);
static_assert(representationOfPrimitives(Primitive::FractionalNumber | Primitive::Absent, Range::empty()) == Representation::OptionalDouble);
static_assert(representationOfPrimitives(Primitive::LiteralString | Primitive::Undefined, Range::empty()) == Representation::LiteralString);
static_assert(representationOfPrimitives(Primitive::AnyString, Range::empty()) == Representation::Cell);
static_assert(representationOfPrimitives(Primitive::String | Primitive::Undefined, Range::empty()) == Representation::JSValue);

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

Representation representationOf(const Type& type)
{
    uint16_t primitives = representedPrimitivesOf(type);
    if (auto internal = internalRepresentationOf(type)) {
        if (*internal == Representation::Cell && primitives)
            return Representation::JSValue;
        return *internal;
    }
    if (type.hasObjects())
        return primitives & ~cellPrimitives ? Representation::JSValue : Representation::Cell;
    return representationOfPrimitives(primitives, type.integers);
}

Representation representationOf(TypeReader types, TypeID id)
{
    if (holdsOnlyPrimitives(id))
        return representationOfPrimitives(static_cast<uint16_t>(primitivesInID(id) & ~Primitive::Unspecified), integersInID(id));
    return representationOf(types.at(id));
}

Representation representationOf(OpaqueKind kind)
{
    switch (kind) {
    case OpaqueKind::Guard:
    case OpaqueKind::Fact:
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
    case OpaqueKind::ViewVector:
    case OpaqueKind::ArrayStoragePointer:
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
        return Representation::OptionalDouble;
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
        return Representation::LiteralString;
    case SlotRepresentation::DeferredClosure:
        return Representation::RawWord;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation loadedRepresentationOf(SlotRepresentation slot, const Type& loaded)
{
    switch (slot) {
    case SlotRepresentation::OptionalInt32:
        if (loaded.isInteger())
            return Representation::Int32;
        break;
    case SlotRepresentation::OptionalInt64:
        if (loaded.isInteger())
            return Representation::Int64;
        break;
    case SlotRepresentation::OptionalDouble:
        if (loaded.isNumber())
            return Representation::Double;
        break;
    case SlotRepresentation::OptionalBoolean:
        if (loaded.isBoolean())
            return Representation::Boolean;
        break;
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::JSValue:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        break;
    }
    return representationOf(slot);
}

bool isLegalRepresentation(const Type& type, Representation representation)
{
    if (representation == Representation::Unchosen)
        return false;
    uint16_t primitives = representedPrimitivesOf(type);
    if (auto internal = internalRepresentationOf(type))
        return canHoldInternalValue(representation, *internal, primitives & Primitive::Absent);
    bool hasObjects = type.hasObjects();
    if (!primitives && !hasObjects)
        return true;
    if (representation == Representation::Void)
        return !hasObjects && isSingleMarker(primitives);
    RepresentableSet set = representableSetOf(representation);
    if (primitives & ~set.primitives)
        return false;
    if (!type.integers.isSubsetOf(set.integers))
        return false;
    return set.hasObjects || !hasObjects;
}

bool isLegalRepresentation(TypeReader types, TypeID id, Representation representation)
{
    if (!holdsOnlyPrimitives(id))
        return isLegalRepresentation(types.recordOf(id), representation);
    if (representation == Representation::Unchosen)
        return false;
    uint16_t primitives = static_cast<uint16_t>(primitivesInID(id) & ~Primitive::Unspecified);
    if (!primitives)
        return true;
    if (representation == Representation::Void)
        return isSingleMarker(primitives);
    RepresentableSet set = representableSetOf(representation);
    return !(primitives & ~set.primitives) && integersInID(id).isSubsetOf(set.integers);
}

Representation representationOfJoin(const Type& united, std::span<const Representation> arguments)
{
    Representation own = representationOf(united);
    if (arguments.empty())
        return own;
    Representation shared = arguments.front();
    bool allArgumentsAgree = std::ranges::all_of(arguments, [&](Representation argument) {
        return argument == shared;
    });
    return allArgumentsAgree && isLegalRepresentation(united, shared) ? shared : own;
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
    switch (representationOf(type)) {
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

static constexpr TypeID widestTypeOfPrimitivesHeldAs(Representation representation)
{
    RepresentableSet set = representableSetOf(representation);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(!set.hasObjects);
    return *tryMakeTypeOfPrimitives(set.primitives & ~primitivesWithLists, set.integers);
}

static_assert(widestTypeOfPrimitivesHeldAs(Representation::Unchosen) == TypeTable::never);
static_assert(widestTypeOfPrimitivesHeldAs(Representation::Void) == TypeTable::never);
static_assert(widestTypeOfPrimitivesHeldAs(Representation::RawWord) == TypeTable::never);
static_assert(widestTypeOfPrimitivesHeldAs(Representation::Boolean) == TypeTable::boolean);
static_assert(widestTypeOfPrimitivesHeldAs(Representation::Int32) == TypeTable::int32);
static_assert(widestTypeOfPrimitivesHeldAs(Representation::Int64) == TypeTable::integer);
static_assert(widestTypeOfPrimitivesHeldAs(Representation::Double) == TypeTable::number);
static_assert(widestTypeOfPrimitivesHeldAs(Representation::OptionalInt64) == typeOfPrimitives(Primitive::Integer | representedMarkerPrimitives));
static_assert(widestTypeOfPrimitivesHeldAs(Representation::OptionalDouble) == typeOfPrimitives(Primitive::Number | representedMarkerPrimitives));
static_assert(widestTypeOfPrimitivesHeldAs(Representation::LiteralString) == typeOfPrimitives(Primitive::String | representedMarkerPrimitives));

TypeID widestTypeOf(Representation representation)
{
    switch (representation) {
    case Representation::Cell:
        return TypeTable::cell;
    case Representation::Cell32:
        return TypeTable::cellOrMarker;
    case Representation::JSValue:
        return TypeTable::anythingOrAbsent;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        return widestTypeOfPrimitivesHeldAs(representation);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static_assert(wantsOf(OperandKind::Declared) == Representation::Unchosen && wantsOf(OperandKind::Convertible) == Representation::Unchosen);
static_assert(wantsOf(OperandKind::Integer) == Representation::Int64);

Representation representationWantedBy(OperandKind kind, const Type& operand)
{
    // An opaque value keeps the representation of its kind. Only an opaque cell may sit in a position that also holds other values.
    if (operand.opaque) {
        Representation internal = representationOf(*operand.opaque);
        bool takesRepresentationOfPosition = holdsCells(internal) && (kind == OperandKind::Declared || kind == OperandKind::Convertible || kind == OperandKind::EvalOperand);
        if (!takesRepresentationOfPosition)
            return internal;
    }

    switch (kind) {
    case OperandKind::Declared:
    case OperandKind::Convertible:
        return representationOf(operand);
    case OperandKind::Integer:
        // Emitters take an integer at the width of its proven type.
        return isLegalRepresentation(operand, Representation::Int32) ? Representation::Int32 : wantsOf(kind);
    case OperandKind::Any:
    case OperandKind::Boolean:
    case OperandKind::Number:
    case OperandKind::String:
    case OperandKind::PropertyKey:
    case OperandKind::Object:
    case OperandKind::Callable:
    case OperandKind::Array:
    case OperandKind::Layout:
    case OperandKind::Environment:
    case OperandKind::EvalOperand:
    case OperandKind::Results:
        return wantsOf(kind);
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::Guard:
    case OperandKind::StringBuilder:
    case OperandKind::Fact:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::BuilderCursor:
    case OperandKind::ViewVector:
    case OperandKind::ArrayStoragePointer:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
        return representationOf(*opaqueKindOf(kind));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
