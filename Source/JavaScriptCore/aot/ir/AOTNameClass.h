/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "JSExportMacros.h"
#include <span>
#include <wtf/Assertions.h>

namespace JSC { namespace AOT { namespace Typed {

JS_EXPORT_PRIVATE NameClass classifyName(std::span<const uint8_t> canonicalProgramString);

constexpr bool isOnObjectPrototype(NameClass nameClass)
{
    return nameClass == NameClass::Proto || nameClass == NameClass::Constructor || nameClass == NameClass::ObjectPrototypeMember;
}

constexpr bool mayBeElementKey(NameClass nameClass)
{
    return nameClass == NameClass::Index || nameClass == NameClass::CanonicalNumericString;
}

constexpr bool isSymbolKind(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
