/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineBuiltinForms.h"

#if ENABLE(AOT)

#include "AOTBuiltinFormRules.h"
#include "AOTRuleSupport.h"
#include "AOTStringBuilderAnalysis.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

using FormChooser = InlineForm (*)(const InlineBuiltinSite&);

static bool isString(const InlineBuiltinSite& site, ValueID id)
{
    return id && site.typeOf(id).isAnyString();
}

static bool hasStringReceiver(const InlineBuiltinSite& site)
{
    return isString(site, site.receiver());
}

static bool isStringArgument(const InlineBuiltinSite& site, unsigned index)
{
    const Type* type = argumentTypeOf(site, index);
    return type && type->isAnyString();
}

static bool isIntegerArgument(const InlineBuiltinSite& site, unsigned index)
{
    const Type* type = argumentTypeOf(site, index);
    return type && type->isInteger();
}

static bool isNumberArgument(const InlineBuiltinSite& site, unsigned index)
{
    const Type* type = argumentTypeOf(site, index);
    return type && type->isNumber();
}

static bool isNumberOrAbsent(const InlineBuiltinSite& site, unsigned index)
{
    const Type* type = argumentTypeOf(site, index);
    if (!type || type->isNumber())
        return true;
    if (type->isNever() || type->primitives & Primitive::Null)
        return false;
    // `to?: number`: the code in line asks whether the operand is undefined.
    return type->hasOnlyPrimitives(Primitive::Undefined) || type->withoutNullish().isNumber();
}

static InlineForm wholeIf(const InlineBuiltinSite& site, bool handlesOperandTypes)
{
    // Without a slow path the parameter types of the overload imply the condition.
    if (handlesOperandTypes || !site.hasSlowPath)
        return InlineForm::Whole;
    return InlineForm::Call;
}

static InlineForm twoWayIf(bool hasInlineCode)
{
    return hasInlineCode ? InlineForm::TwoWay : InlineForm::Call;
}

static InlineForm formOfStringLength(const InlineBuiltinSite& site)
{
    return wholeIf(site, hasStringReceiver(site));
}

static InlineForm formOfIndexedRead(const InlineBuiltinSite& site)
{
    return wholeIf(site, hasStringReceiver(site) && isNumberOrAbsent(site, 0) && site.arguments().size() <= 1);
}

static InlineForm formOfRange(const InlineBuiltinSite& site)
{
    return wholeIf(site, hasStringReceiver(site) && isNumberOrAbsent(site, 0) && isNumberOrAbsent(site, 1) && site.arguments().size() <= 2);
}

static InlineForm formOfSearch(const InlineBuiltinSite& site)
{
    return wholeIf(site, hasStringReceiver(site) && isStringArgument(site, 0) && isNumberOrAbsent(site, 1) && site.arguments().size() <= 2);
}

static InlineForm formOfConversion(const InlineBuiltinSite& site)
{
    return wholeIf(site, hasStringReceiver(site) && site.arguments().empty());
}

static InlineForm formOfStringPair(const InlineBuiltinSite& site)
{
    return wholeIf(site, hasStringReceiver(site) && isStringArgument(site, 0) && site.arguments().size() == 1);
}

static InlineForm formOfStringConcat(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    bool hasOnlyStrings = hasStringReceiver(site) && std::ranges::all_of(arguments, [&](ValueID argument) {
        return isString(site, argument);
    });
    return wholeIf(site, hasOnlyStrings && (arguments.size() == 1 || arguments.size() == 2));
}

static InlineForm formOfStringFromCharCode(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    return wholeIf(site, arguments.size() == 1 && site.typeOf(arguments[0]).isNumber());
}

static InlineForm formOfCallStringConstructor(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    return wholeIf(site, arguments.size() == 1 && isStringBuilderPart(site.typeOf(arguments[0])));
}

static bool isUTF8Encoding(const InlineBuiltinSite& site, ValueID encoding)
{
    const Program& program = site.context.program();
    std::optional<StringID> constant = stringConstantOf(program, site.context.function(), encoding);
    if (!constant)
        return false;
    Vector<char16_t> buffer;
    StringView name = program.strings.viewOf(*constant, buffer);
    return name == "utf8"_s || name == "utf-8"_s;
}

static InlineForm formOfStringUTF8ByteLength(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    if (!isStringArgument(site, 0))
        return wholeIf(site, false);
    // Only a string names an encoding. `Buffer.byteLength(chunk, encoding)`: the code in line asks whether it is one.
    if (arguments.size() == 1 || isUTF8Encoding(site, arguments[1]) || !(site.typeOf(arguments[1]).primitives & Primitive::AnyString))
        return InlineForm::Whole;
    return twoWayIf(site.hasSlowPath);
}

static InlineForm formOfStringFromCodePoint(const InlineBuiltinSite& site)
{
    return twoWayIf(site.arguments().size() == 1 && isIntegerArgument(site, 0));
}

static InlineForm formOfStringRepeat(const InlineBuiltinSite& site)
{
    return twoWayIf(hasStringReceiver(site) && isNumberArgument(site, 0));
}

static InlineForm formOfPad(const InlineBuiltinSite& site)
{
    bool hasUsableFiller = site.arguments().size() < 2 || isStringArgument(site, 1);
    return twoWayIf(hasStringReceiver(site) && isNumberArgument(site, 0) && hasUsableFiller);
}

static InlineForm formOfWellFormedness(const InlineBuiltinSite& site)
{
    return twoWayIf(hasStringReceiver(site));
}

static InlineForm formOfStringNormalize(const InlineBuiltinSite& site)
{
    return twoWayIf(hasStringReceiver(site) && site.arguments().empty());
}

static InlineForm formOfAppendForm(const InlineBuiltinSite&)
{
    return InlineForm::Call;
}

static constexpr FormChooser formOfStringAt = formOfIndexedRead;
static constexpr FormChooser formOfStringCharAt = formOfIndexedRead;
static constexpr FormChooser formOfStringCharCodeAt = formOfIndexedRead;
static constexpr FormChooser formOfStringCodePointAt = formOfIndexedRead;
static constexpr FormChooser formOfStringSlice = formOfRange;
static constexpr FormChooser formOfStringSubstring = formOfRange;
static constexpr FormChooser formOfStringSubstr = formOfRange;
static constexpr FormChooser formOfStringStartsWith = formOfSearch;
static constexpr FormChooser formOfStringEndsWith = formOfSearch;
static constexpr FormChooser formOfStringIncludes = formOfSearch;
static constexpr FormChooser formOfStringIndexOf = formOfSearch;
static constexpr FormChooser formOfStringLastIndexOf = formOfSearch;
static constexpr FormChooser formOfStringToLowerCase = formOfConversion;
static constexpr FormChooser formOfStringToUpperCase = formOfConversion;
static constexpr FormChooser formOfStringTrim = formOfConversion;
static constexpr FormChooser formOfStringTrimStart = formOfConversion;
static constexpr FormChooser formOfStringTrimEnd = formOfConversion;
static constexpr FormChooser formOfStringLocaleCompare = formOfStringPair;
static constexpr FormChooser formOfStringSplitPrefix = formOfStringPair;
static constexpr FormChooser formOfStringPadStart = formOfPad;
static constexpr FormChooser formOfStringPadEnd = formOfPad;
static constexpr FormChooser formOfStringIsWellFormed = formOfWellFormedness;
static constexpr FormChooser formOfStringToWellFormed = formOfWellFormedness;
static constexpr FormChooser formOfStringSliceAppend = formOfAppendForm;
static constexpr FormChooser formOfStringSubstringAppend = formOfAppendForm;
static constexpr FormChooser formOfCallStringConstructorAppend = formOfAppendForm;

InlineForm chooseFormOfStringBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
#define AOT_CHOOSE_FORM(name) \
    case InlineBuiltin::name: \
        return formOf##name(site);
    FOR_EACH_AOT_INLINE_BUILTIN_IN_STRING_BUILTIN(AOT_CHOOSE_FORM)
#undef AOT_CHOOSE_FORM
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
