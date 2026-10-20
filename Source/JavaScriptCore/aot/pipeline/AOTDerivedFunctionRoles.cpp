/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDerivedFunctions.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static constexpr DerivedFunctionRole derivedFunctionRoles[] = {
#define AOT_ROLE_DERIVED_ON_REQUEST(role, builder, target, created) \
    { FunctionRole::role, DerivedFunctionTarget::target, DerivedFunctionCreation::OnRequest, builder, Wire::StatisticID::created },
    FOR_EACH_AOT_FUNCTION_ROLE_DERIVED_ON_REQUEST(AOT_ROLE_DERIVED_ON_REQUEST)
#undef AOT_ROLE_DERIVED_ON_REQUEST
#define AOT_ROLE_DERIVED_BY_PROGRAM_PASS(role, target) \
    { FunctionRole::role, DerivedFunctionTarget::target, DerivedFunctionCreation::ByProgramPass, nullptr, Wire::StatisticID::None },
    FOR_EACH_AOT_FUNCTION_ROLE_DERIVED_BY_PROGRAM_PASS(AOT_ROLE_DERIVED_BY_PROGRAM_PASS)
#undef AOT_ROLE_DERIVED_BY_PROGRAM_PASS
};

static constexpr unsigned numberOfRowsFor(FunctionRole role)
{
    unsigned count = 0;
    for (const DerivedFunctionRole& row : derivedFunctionRoles)
        count += row.role == role;
    return count;
}

static constexpr bool hasUniqueRoles()
{
    for (const DerivedFunctionRole& row : derivedFunctionRoles) {
        if (numberOfRowsFor(row.role) != 1)
            return false;
    }
    return true;
}

static_assert(hasUniqueRoles());
static_assert(!numberOfRowsFor(FunctionRole::Source) && !numberOfRowsFor(FunctionRole::Library) && !numberOfRowsFor(FunctionRole::Model));

const DerivedFunctionRole* findDerivedFunctionRole(FunctionRole role)
{
    for (const DerivedFunctionRole& row : derivedFunctionRoles) {
        if (row.role == role)
            return &row;
    }
    return nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
