/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInlineBuiltinForms.h"

namespace JSC { namespace AOT { namespace Typed {

InlineForm wholeOrTwoWay(const InlineBuiltinSite&, bool handlesEveryValue);

const Type& receiverTypeOf(const InlineBuiltinSite&);
const Type* argumentTypeOf(const InlineBuiltinSite&, unsigned index);
bool allArgumentsAreNumbers(const InlineBuiltinSite&);
bool isObjectArgument(const InlineBuiltinSite&, unsigned index);

inline bool isNumberArgument(const InlineBuiltinSite& site, unsigned index)
{
    const Type* type = argumentTypeOf(site, index);
    return type && type->isNumber();
}

inline InlineForm twoWayIf(const InlineBuiltinSite& site, bool hasInlineCode)
{
    RELEASE_ASSERT(site.hasSlowPath);
    return hasInlineCode ? InlineForm::TwoWay : InlineForm::Call;
}

inline InlineForm wholeIf(const InlineBuiltinSite& site, bool handlesOperandTypes)
{
    // Without a slow path the parameter types of the overload imply the condition.
    if (handlesOperandTypes || !site.hasSlowPath)
        return InlineForm::Whole;
    return InlineForm::Call;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
