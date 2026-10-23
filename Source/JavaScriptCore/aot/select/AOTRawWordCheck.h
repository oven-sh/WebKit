/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

constexpr bool isRawWord(OpaqueKind kind)
{
    return isInteriorPointer(kind) || kind == OpaqueKind::CodeAddress;
}

constexpr bool endsAtSuspension(OpaqueKind kind)
{
    switch (kind) {
    case OpaqueKind::BuilderCursor:
    case OpaqueKind::Cursor:
    case OpaqueKind::FlatString:
        return true;
    case OpaqueKind::IteratorState:
    case OpaqueKind::Enumerator:
    case OpaqueKind::Exception:
    case OpaqueKind::ClassToken:
    case OpaqueKind::StringBuilder:
    case OpaqueKind::KeyOrder:
    case OpaqueKind::Guard:
    case OpaqueKind::CodeAddress:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static_assert(isInteriorPointer(OpaqueKind::BuilderCursor));
static_assert(!isInteriorPointer(OpaqueKind::CodeAddress) && !isInteriorPointer(OpaqueKind::FlatString));
static_assert(std::ranges::all_of(allMembersOf<OpaqueKind>, [](OpaqueKind kind) {
    return endsAtSuspension(kind) || !isInteriorPointer(kind);
}));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
