/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"
#include "AOTSchemaEnums.h"
#include "AOTType.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

using RepresentationSet = EnumeratorBitSet<Representation, numberOfEnumerators<Representation>>;

Representation defaultRepresentationOf(const Type&);
Representation representationOf(OpaqueKind);
Representation representationOf(SlotRepresentation);
Representation loadedRepresentationOf(SlotRepresentation, const Type& loaded);
bool isLegalRepresentation(const Type&, Representation);
RepresentationSet legalRepresentationsOf(const Type&);
SlotRepresentation defaultSlotRepresentationOf(const Type&);
Type widestTypeOf(Representation);

constexpr bool holdsCells(Representation representation)
{
    switch (representation) {
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
        return true;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::RawWord:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline bool isNumberOrUndefined(const Type& type)
{
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
}

inline bool areNumbers(std::span<const Type> types)
{
    for (auto& type : types) {
        if (!type.isNumber())
            return false;
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
