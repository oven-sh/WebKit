/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkTables.h"

namespace JSC { namespace AOT { namespace Typed {

class LinkFlow;

struct AccessorLaidInField {
    FieldReference field;
    FunctionID function;
    bool isGetter { true };

    friend constexpr auto operator<=>(const AccessorLaidInField&, const AccessorLaidInField&) = default;
};

// Behind writeAppendedFields() too, and by itself: LinkTables::accessorsExpectedInFields and keysOfAccessorsExpectedInFields, out of LinkFlow::accessorsLaidInFields alone.
void writeAccessorsExpectedInFields(const LinkFlow&, LinkTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
