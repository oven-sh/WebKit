/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"
#include "AOTSchemaEnums.h"
#include "AOTType.h"
#include "AOTTypeArena.h"
#include <array>
#include <span>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

using RepresentationSet = EnumeratorBitSet<Representation, numberOfEnumerators<Representation>>;

inline constexpr uint16_t representedMarkerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;

struct RepresentableSet {
    uint16_t primitives { 0 };
    Range integers { Range::empty() };
    bool hasObjects { false };
};

constexpr RepresentableSet representableSetOf(Representation representation)
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
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline constexpr std::array<RepresentableSet, numberOfEnumerators<Representation>> representableSetsByRepresentation = []() consteval {
    std::array<RepresentableSet, numberOfEnumerators<Representation>> sets { };
    for (Representation representation : allMembersOf<Representation>)
        sets[static_cast<unsigned>(representation)] = representableSetOf(representation);
    return sets;
}();

constexpr bool isSingleMarker(uint16_t primitives)
{
    return !(primitives & ~representedMarkerPrimitives) && hasOneBitSet(primitives);
}

Representation representationOf(const Type&);
Representation representationOf(TypeReader, TypeID);
Representation representationOf(OpaqueKind);
Representation representationOf(SlotRepresentation);
Representation loadedRepresentationOf(SlotRepresentation, const Type& loaded);
bool isLegalRepresentationOfInternalValue(const Type&, Representation);
ALWAYS_INLINE bool isLegalRepresentation(const Type& type, Representation representation)
{
    if (representation == Representation::Unchosen)
        return false;
    if (type.opaque || type.environment) [[unlikely]]
        return isLegalRepresentationOfInternalValue(type, representation);
    uint16_t primitives = static_cast<uint16_t>(type.primitives & ~Primitive::Unspecified);
    if (representation == Representation::JSValue)
        return !(primitives & ~representableSetOf(Representation::JSValue).primitives);
    bool hasObjects = type.hasObjects();
    if (!primitives && !hasObjects)
        return true;
    if (representation == Representation::Void)
        return !hasObjects && isSingleMarker(primitives);
    const RepresentableSet& set = representableSetsByRepresentation[static_cast<unsigned>(representation)];
    if (primitives & ~set.primitives)
        return false;
    if (!type.integers.isSubsetOf(set.integers))
        return false;
    return set.hasObjects || !hasObjects;
}
ALWAYS_INLINE bool isLegalRepresentation(TypeReader types, TypeID id, Representation representation)
{
    if (!holdsOnlyPrimitives(id))
        return isLegalRepresentation(types.recordOf(id), representation);
    if (representation == Representation::Unchosen)
        return false;
    uint16_t primitives = static_cast<uint16_t>(primitivesInID(id) & ~Primitive::Unspecified);
    if (representation == Representation::JSValue)
        return !(primitives & ~representableSetOf(Representation::JSValue).primitives);
    if (!primitives)
        return true;
    if (representation == Representation::Void)
        return isSingleMarker(primitives);
    const RepresentableSet& set = representableSetsByRepresentation[static_cast<unsigned>(representation)];
    return !(primitives & ~set.primitives) && integersInID(id).isSubsetOf(set.integers);
}
Representation representationOfJoin(const Type& united, std::span<const Representation> arguments);
TypeID widestTypeOf(Representation);
Representation representationWantedBy(OperandKind, const Type& operand);

constexpr bool holdsCells(Representation representation) { return holdsCellsOf(representation); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
