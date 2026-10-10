/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// The family aot/builtins/intl-temporal.def.

std::optional<Writes> writesOfInlineIntlTemporal(InlineBuiltin, std::span<const Type>)
{
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Lowering::lowerInlineIntlTemporal(InlineBuiltin, ValueID, const Value&, Writes)
{
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
