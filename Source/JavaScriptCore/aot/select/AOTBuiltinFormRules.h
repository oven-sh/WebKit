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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
