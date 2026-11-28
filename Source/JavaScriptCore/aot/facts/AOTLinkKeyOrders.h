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
    PropertyKeyID appendedKey;

    friend constexpr auto operator<=>(const AccessorLaidInField&, const AccessorLaidInField&) = default;
};

void writeAccessorsExpectedInFields(const LinkFlow&, LinkTables&);
void writeMembersAboveCreatedLayouts(const LinkFlow&, LinkTables&);
LayoutID findHolderAboveCreatedLayout(const LinkFlow&, LayoutID created);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
