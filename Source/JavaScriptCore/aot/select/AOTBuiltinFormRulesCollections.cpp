/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinFormRules.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

InlineForm chooseFormOfHashTableBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::NewMap:
    case InlineBuiltin::MapGet:
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas:
    case InlineBuiltin::MapSet:
    case InlineBuiltin::SetAdd:
    case InlineBuiltin::MapSize:
    case InlineBuiltin::SetSize:
        return InlineForm::Whole;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// The operation reads the elements of an array, and for a Set also those of an arguments object and the code points of a string, without an iterator. Any other iterable is left to the constructor.
static bool mayBeListedByOperation(const InlineBuiltinSite& site, const Type& iterable)
{
    if (iterable.anyObject || iterable.findClassPart(BuiltinClasses::Array))
        return true;
    if (site.builtin != InlineBuiltin::SetFromIterable)
        return false;
    return iterable.primitives & Primitive::AnyString || iterable.findClassPart(BuiltinClasses::Arguments);
}

InlineForm chooseFormOfMapSetBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::SetFromIterable:
    case InlineBuiltin::MapFromEntries: {
        std::span<const ValueID> arguments = site.arguments();
        return arguments.size() == 1 && mayBeListedByOperation(site, site.typeOf(arguments[0])) ? InlineForm::TwoWay : InlineForm::Call;
    }
    case InlineBuiltin::WeakMapGet:
    case InlineBuiltin::WeakMapHas:
    case InlineBuiltin::WeakSetHas:
    case InlineBuiltin::MapDelete:
    case InlineBuiltin::SetDelete:
    case InlineBuiltin::MapClear:
    case InlineBuiltin::SetClear:
    case InlineBuiltin::MapIterator:
    case InlineBuiltin::SetIterator:
        return InlineForm::Whole;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
