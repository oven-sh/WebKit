/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "TypedArrayType.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

constexpr bool hasInlineElementAccess(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
    case TypeUint8:
    case TypeUint8Clamped:
    case TypeInt16:
    case TypeUint16:
    case TypeInt32:
    case TypeUint32:
    case TypeFloat32:
    case TypeFloat64:
        return true;
    case NotTypedArray:
    case TypeFloat16:
    case TypeBigInt64:
    case TypeBigUint64:
    case TypeDataView:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool hasIntegerElements(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
    case TypeUint8:
    case TypeInt16:
    case TypeUint16:
    case TypeInt32:
    case TypeUint32:
        return true;
    case NotTypedArray:
    case TypeUint8Clamped:
    case TypeFloat16:
    case TypeFloat32:
    case TypeFloat64:
    case TypeBigInt64:
    case TypeBigUint64:
    case TypeDataView:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr SlotRepresentation elementStore(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return SlotRepresentation::Int8;
    case TypeUint8:
    case TypeUint8Clamped:
        return SlotRepresentation::Uint8;
    case TypeInt16:
        return SlotRepresentation::Int16;
    case TypeUint16:
        return SlotRepresentation::Uint16;
    case TypeInt32:
        return SlotRepresentation::Int32;
    case TypeUint32:
        return SlotRepresentation::Uint32;
    case TypeFloat32:
        return SlotRepresentation::Float;
    case TypeFloat64:
        return SlotRepresentation::Double;
    case TypeBigInt64:
    case TypeBigUint64:
        return SlotRepresentation::Int64;
    case NotTypedArray:
    case TypeFloat16:
    case TypeDataView:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr std::optional<TypedArrayType> elementsMovedBy(unsigned byteCount, NumberKind kind)
{
    switch (kind) {
    case NumberKind::Signed:
        switch (byteCount) {
        case 1:
            return TypeInt8;
        case 2:
            return TypeInt16;
        case 4:
            return TypeInt32;
        case 8:
            return TypeBigInt64;
        }
        return std::nullopt;
    case NumberKind::Unsigned:
        switch (byteCount) {
        case 1:
            return TypeUint8;
        case 2:
            return TypeUint16;
        case 4:
            return TypeUint32;
        case 8:
            return TypeBigUint64;
        }
        return std::nullopt;
    case NumberKind::Float:
        switch (byteCount) {
        case 4:
            return TypeFloat32;
        case 8:
            return TypeFloat64;
        }
        return std::nullopt;
    case NumberKind::BigInt:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static_assert(hasIntegerElements(TypeUint32) && !hasIntegerElements(TypeUint8Clamped));
static_assert(hasInlineElementAccess(TypeUint8Clamped) && !hasInlineElementAccess(TypeBigInt64) && !hasInlineElementAccess(TypeFloat16));
static_assert(elementStore(TypeUint8Clamped) == elementStore(TypeUint8) && elementStore(TypeBigUint64) == SlotRepresentation::Int64);
static_assert(elementsMovedBy(8, NumberKind::Unsigned) == TypeBigUint64 && !elementsMovedBy(3, NumberKind::Signed) && !elementsMovedBy(2, NumberKind::Float));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
