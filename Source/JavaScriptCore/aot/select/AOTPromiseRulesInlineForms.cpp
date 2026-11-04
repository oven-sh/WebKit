/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineBuiltinForms.h"

#if ENABLE(AOT)

#include "AOTEffectsOfProtocols.h"

namespace JSC { namespace AOT { namespace Typed {

#define AOT_COUNT_INLINE_BUILTIN(name) + 1
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_PROMISE_BUILTIN(AOT_COUNT_INLINE_BUILTIN) == 3, "chooseFormOfPromiseBuiltin needs a case for every member of the family");
#undef AOT_COUNT_INLINE_BUILTIN

static InlineForm chooseFormOfPromiseResolve(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    if (arguments.empty() || !protocolMayFindMember(site.context.program(), site.typeOf(arguments.front()), Protocol::Thenable))
        return InlineForm::Whole;

    RELEASE_ASSERT(site.hasSlowPath);
    if (site.shape == CodeShape::Compact)
        return InlineForm::Call;
    return InlineForm::TwoWay;
}

InlineForm chooseFormOfPromiseBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::PromiseResolve:
        return chooseFormOfPromiseResolve(site);
    case InlineBuiltin::PromiseThen:
    case InlineBuiltin::PromiseCatch:
        return InlineForm::Whole;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
