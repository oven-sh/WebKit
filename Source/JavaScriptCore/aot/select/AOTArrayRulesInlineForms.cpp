/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinFormRules.h"
#include "AOTInlineBuiltinForms.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

#define AOT_COUNT_INLINE_BUILTIN(name) + 1
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUILTIN(AOT_COUNT_INLINE_BUILTIN) == 12, "chooseFormOfArrayBuiltin needs a case for every member of the family");
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_STORAGE_BUILTIN(AOT_COUNT_INLINE_BUILTIN) == 6, "chooseFormOfArrayStorageBuiltin needs a case for every member of the family");
#undef AOT_COUNT_INLINE_BUILTIN

static bool exceeds(const InlineBuiltinSite& site, LimitID limit, size_t amount)
{
    return site.context.pass().exceeds(limit, amount, site.call);
}

static bool areNumbers(const InlineBuiltinSite& site, std::span<const ValueID> values)
{
    return std::ranges::all_of(values, [&](ValueID value) {
        return site.typeOf(value).isNumber();
    });
}

static bool isArrayType(const Type& type)
{
    const ClassPart* part = type.isObject() ? type.singleBuiltinClass() : nullptr;
    return part && part->builtinClass == BuiltinClasses::Array;
}

static bool searchesForString(const InlineBuiltinSite& site)
{
    const Type* searched = argumentTypeOf(site, 0);
    return searched && site.arguments().size() == 1 && searched->isAnyString();
}

static bool searchesForStringFromStart(const InlineBuiltinSite& site)
{
    const Type* searched = argumentTypeOf(site, 0);
    return searched && site.arguments().size() == 2 && searched->isAnyString() && isNumberArgument(site, 1);
}

// Code in line serves a number, and what is compared by identity: an object, a function, a symbol, a boolean, undefined, null.
static bool canSearchInline(const InlineBuiltinSite& site)
{
    constexpr uint16_t comparedByValue = Primitive::Number | Primitive::AnyString | Primitive::BigInt;
    const Type* searched = argumentTypeOf(site, 0);
    if (!searched || site.arguments().size() != 1 || searched->isNever() || !searched->isJavaScriptValue())
        return false;
    if (searched->isNumber() || !(searched->primitives & comparedByValue))
        return true;
    return site.shape == CodeShape::Fast && (searched->hasObjects() || searched->primitives & ~comparedByValue);
}

static bool canJoinInline(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    return arguments.empty() || (arguments.size() == 1 && site.typeOf(arguments[0]).isAnyString());
}

static bool canConcatenateInline(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    bool areElementsOrArrays = std::ranges::all_of(arguments, [&](ValueID argument) {
        const Type& type = site.typeOf(argument);
        return !type.hasObjects() || isArrayType(type);
    });
    return areElementsOrArrays && !exceeds(site, LimitID::MaxInlineConcatOperands, arguments.size());
}

static bool canSliceInline(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    return arguments.size() <= 2 && areNumbers(site, arguments);
}

static bool canSpliceInline(const InlineBuiltinSite& site)
{
    constexpr size_t numberOfRangeArguments = 2;
    std::span<const ValueID> arguments = site.arguments();
    if (arguments.size() < numberOfRangeArguments || !isNumberArgument(site, 0) || !isNumberArgument(site, 1))
        return false;
    return !exceeds(site, LimitID::MaxInlineSpliceInsertions, arguments.size() - numberOfRangeArguments);
}

InlineForm chooseFormOfArrayBuiltin(const InlineBuiltinSite& site)
{
    bool isCompact = site.shape != CodeShape::Fast;
    switch (site.builtin) {
    case InlineBuiltin::ArrayAt:
        return twoWayIf(site, isNumberArgument(site, 0));
    case InlineBuiltin::ArrayIndexOf:
    case InlineBuiltin::ArrayIncludes:
        if (searchesForString(site))
            return InlineForm::Whole;
        return twoWayIf(site, searchesForStringFromStart(site) || canSearchInline(site));
    case InlineBuiltin::ArrayJoin:
        return twoWayIf(site, canJoinInline(site));
    case InlineBuiltin::ArrayConcat:
        return twoWayIf(site, !isCompact && canConcatenateInline(site));
    case InlineBuiltin::ArrayShift:
        return twoWayIf(site, !isCompact);
    case InlineBuiltin::ArrayPop:
        return InlineForm::TwoWay;
    case InlineBuiltin::ArraySlice:
        return twoWayIf(site, canSliceInline(site));
    case InlineBuiltin::ArraySplice:
        return twoWayIf(site, canSpliceInline(site));
    case InlineBuiltin::ArraySplicePrefix:
        return InlineForm::Whole;
    case InlineBuiltin::ArrayUnshift:
        return twoWayIf(site, site.arguments().size() == 1);
    case InlineBuiltin::ArrayIterator:
        // The methods of %TypedArray%.prototype throw on a detached or out-of-bounds view, which the inline code does not test.
        return wholeIf(site, site.receiver() && isArrayType(site.typeOf(site.receiver())));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static InlineForm chooseFormOfArrayPush(const InlineBuiltinSite& site)
{
    size_t numberOfPushedValues = site.arguments().size();
    if (numberOfPushedValues == 1)
        return InlineForm::Whole;
    return twoWayIf(site, numberOfPushedValues && !exceeds(site, LimitID::MaxInlinePushOperands, numberOfPushedValues));
}

static InlineForm chooseFormOfArrayIsArray(const InlineBuiltinSite& site)
{
    const Type* tested = argumentTypeOf(site, 0);
    if (tested && !tested->hasObjects())
        return InlineForm::Whole;
    return twoWayIf(site, !!tested);
}

static InlineForm chooseFormOfArraySetLength(const InlineBuiltinSite& site)
{
    bool storesNumber = isNumberArgument(site, 0);
    return site.hasSlowPath ? twoWayIf(site, storesNumber) : wholeIf(site, storesNumber);
}

InlineForm chooseFormOfArrayStorageBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::ArrayPush:
        return chooseFormOfArrayPush(site);
    case InlineBuiltin::ArrayIsArray:
        return chooseFormOfArrayIsArray(site);
    case InlineBuiltin::ArraySetLength:
        return chooseFormOfArraySetLength(site);
    case InlineBuiltin::ArrayWithLength:
        return wholeIf(site, isNumberArgument(site, 0));
    case InlineBuiltin::ArrayLength:
    case InlineBuiltin::ArrayOf:
        return InlineForm::Whole;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
