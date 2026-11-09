/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallPlan.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "YarrFlags.h"
#include <optional>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

#define AOT_CASE_IN_FORMS_OF_PRIMITIVES(name) case InlineBuiltin::name:

#define FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_PRIMITIVES(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_STORAGE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUFFER_VIEW_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OWN_PROPERTY_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OBJECT_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MAP_SET_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_PROMISE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_BUFFER_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_DATE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_FUNCTION_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ERROR_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_INTL_BUILTIN(v)

namespace {

class PrimitiveFormChooser {
public:
    explicit PrimitiveFormChooser(const InlineBuiltinQuery& query)
        : m_query(query)
        , m_types(query.context.types)
    {
    }

    InlineForm mathForm() const;
    InlineForm stringForm() const;
    InlineForm regExpForm() const;

private:
    static constexpr unsigned mostOperandsOfHypot = 3;
    static constexpr unsigned mostStringsConcatenatedToReceiver = 2;

    static InlineForm wholeIf(bool takesOperands) { return takesOperands ? InlineForm::Whole : InlineForm::Call; }
    InlineForm twoWayIf(bool takesOperands) const { return takesOperands && m_query.hasSlowPath ? InlineForm::TwoWay : InlineForm::Call; }

    TypeID receiver() const { return m_query.receiver(); }
    TypeID argument(unsigned index) const { return m_query.argument(index); }
    unsigned numberOfArguments() const { return m_query.numberOfArguments(); }

    bool isValue(TypeID type) const { return type && !isNeverType(type); }
    bool isNumber(TypeID type) const { return type && m_types.at(type).isNumber(); }
    bool isInt32(TypeID type) const { return type && m_types.at(type).isInt32(); }
    bool isString(TypeID type) const { return type && m_types.at(type).isAnyString(); }
    bool isInstanceOf(TypeID type, BuiltinClassID builtinClass) const { return isValue(type) && m_types.isSubset(type, m_types.builtinClass(builtinClass)); }
    bool isRegExp(TypeID type) const { return isInstanceOf(type, BuiltinClasses::RegExp); }
    bool isNumberOrUndefinedIfPassed(TypeID) const;
    bool convertsAsConcatenationDoes(TypeID) const;
    bool startsWithNumbers(unsigned count) const;
    bool hasOnlyNumberArguments() const { return startsWithNumbers(numberOfArguments()); }
    bool hasOnlyStringArguments() const;

    bool readsStringAtPosition() const { return isString(receiver()) && isNumberOrUndefinedIfPassed(argument(0)) && numberOfArguments() <= 1; }
    bool takesRangeOfString() const { return isString(receiver()) && isNumberOrUndefinedIfPassed(argument(0)) && isNumberOrUndefinedIfPassed(argument(1)) && numberOfArguments() <= 2; }
    bool searchesStringForString() const { return isString(receiver()) && isString(argument(0)) && isNumberOrUndefinedIfPassed(argument(1)) && numberOfArguments() <= 2; }

    InlineForm numberConstructorForm() const;
    InlineForm numberToStringForm() const;
    bool namesUTF8(TypeID encoding) const;
    InlineForm utf8ByteLengthForm() const;

    std::optional<SearchValueKind> searchValueKind() const;
    bool isSearchValue(TypeID) const;
    bool replacesInLine() const;
    InlineForm splitForm() const;
    bool namesValidRegExpFlags(TypeID) const;

    const InlineBuiltinQuery& m_query;
    TypeArena& m_types;
};

} // anonymous namespace

bool PrimitiveFormChooser::isNumberOrUndefinedIfPassed(TypeID type) const
{
    if (!type)
        return true;
    Type passed = m_types.at(type);
    return !passed.isNever() && passed.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
}

bool PrimitiveFormChooser::convertsAsConcatenationDoes(TypeID type) const
{
    return isValue(type) && m_types.at(type).hasOnlyPrimitives(Primitive::AnyString | Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::BigInt);
}

bool PrimitiveFormChooser::startsWithNumbers(unsigned count) const
{
    for (unsigned index = 0; index < count; ++index) {
        if (!isNumber(argument(index)))
            return false;
    }
    return true;
}

InlineForm PrimitiveFormChooser::numberConstructorForm() const
{
    if (numberOfArguments() != 1 || !isValue(argument(0)) || !m_types.isSubset(argument(0), TypeTable::anything))
        return InlineForm::Call;
    if (isString(argument(0)) || isNumber(argument(0)))
        return InlineForm::Whole;
    return twoWayIf(true);
}

InlineForm PrimitiveFormChooser::numberToStringForm() const
{
    if (!isNumber(receiver()))
        return InlineForm::Call;
    TypeID radix = argument(0);
    if (!radix)
        return InlineForm::Whole;
    Type radixType = m_types.at(radix);
    if (!radixType.isInt32())
        return InlineForm::Call;
    return radixType.integers.isSubsetOf({ 2, 36 }) ? InlineForm::Whole : twoWayIf(true);
}

InlineForm PrimitiveFormChooser::mathForm() const
{
    switch (m_query.builtin) {
    case InlineBuiltin::IsNaN:
    case InlineBuiltin::IsFinite:
    case InlineBuiltin::MathAbs:
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
    case InlineBuiltin::MathFround:
    case InlineBuiltin::MathF16Round:
    case InlineBuiltin::MathSqrt:
    case InlineBuiltin::MathSign:
    case InlineBuiltin::MathFunction:
        return wholeIf(startsWithNumbers(1));
    case InlineBuiltin::MathPow:
    case InlineBuiltin::MathATan2:
        return wholeIf(startsWithNumbers(2));
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax:
        return wholeIf(hasOnlyNumberArguments());
    case InlineBuiltin::MathHypot:
        return wholeIf(numberOfArguments() <= mostOperandsOfHypot && hasOnlyNumberArguments());
    case InlineBuiltin::MathImul:
        return wholeIf(isInt32(argument(0)) && isInt32(argument(1)));
    case InlineBuiltin::MathClz32:
        return wholeIf(isInt32(argument(0)));
    case InlineBuiltin::NumberIsNaN:
    case InlineBuiltin::NumberIsFinite:
    case InlineBuiltin::NumberIsInteger:
    case InlineBuiltin::NumberIsSafeInteger:
        return wholeIf(isValue(argument(0)));
    case InlineBuiltin::MathRandom:
        return InlineForm::Whole;
    case InlineBuiltin::CallNumberConstructor:
        return numberConstructorForm();
    case InlineBuiltin::ParseInt:
        return twoWayIf(isString(argument(0)) && (!argument(1) || isInt32(argument(1))));
    case InlineBuiltin::NumberToString:
        return numberToStringForm();
    FOR_EACH_AOT_INLINE_BUILTIN_IN_STRING_BUILTIN(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_REG_EXP_BUILTIN(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_PRIMITIVES(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool PrimitiveFormChooser::hasOnlyStringArguments() const
{
    for (unsigned index = 0; index < numberOfArguments(); ++index) {
        if (!isString(argument(index)))
            return false;
    }
    return true;
}

bool PrimitiveFormChooser::namesUTF8(TypeID encoding) const
{
    StringID text = singleStringLiteralOf(m_types.at(encoding));
    if (!text)
        return false;
    Vector<char16_t> buffer;
    StringView name = m_query.context.program.strings.viewOf(text, buffer);
    return name == "utf8"_s || name == "utf-8"_s;
}

InlineForm PrimitiveFormChooser::utf8ByteLengthForm() const
{
    if (!isString(argument(0)))
        return InlineForm::Call;
    TypeID encoding = argument(1);
    if (!encoding)
        return InlineForm::Whole;
    if (!isValue(encoding))
        return InlineForm::Call;
    if (namesUTF8(encoding) || !(m_types.at(encoding).primitives & Primitive::AnyString))
        return InlineForm::Whole;
    return twoWayIf(true);
}

InlineForm PrimitiveFormChooser::stringForm() const
{
    switch (m_query.builtin) {
    case InlineBuiltin::StringLength:
        return wholeIf(isString(receiver()));
    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt:
        return wholeIf(readsStringAtPosition());
    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr:
        return wholeIf(takesRangeOfString());
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf:
    case InlineBuiltin::StringLastIndexOf:
        return wholeIf(searchesStringForString());
    case InlineBuiltin::StringFromCharCode:
        return wholeIf(numberOfArguments() == 1 && isNumber(argument(0)));
    case InlineBuiltin::CallStringConstructor:
        return wholeIf(numberOfArguments() == 1 && convertsAsConcatenationDoes(argument(0)));
    case InlineBuiltin::StringToLowerCase:
    case InlineBuiltin::StringToUpperCase:
    case InlineBuiltin::StringTrim:
    case InlineBuiltin::StringTrimStart:
    case InlineBuiltin::StringTrimEnd:
        return wholeIf(isString(receiver()) && !numberOfArguments());
    case InlineBuiltin::StringLocaleCompare:
        return wholeIf(isString(receiver()) && isString(argument(0)) && numberOfArguments() == 1);
    case InlineBuiltin::StringConcat:
        return wholeIf(isString(receiver()) && numberOfArguments() && numberOfArguments() <= mostStringsConcatenatedToReceiver && hasOnlyStringArguments());
    case InlineBuiltin::StringUTF8ByteLength:
        return utf8ByteLengthForm();
    case InlineBuiltin::StringFromCodePoint:
        return twoWayIf(numberOfArguments() == 1 && isValue(argument(0)) && m_types.at(argument(0)).primitives & Primitive::Number);
    case InlineBuiltin::StringRepeat:
        return twoWayIf(isString(receiver()) && isNumber(argument(0)));
    case InlineBuiltin::StringPadStart:
    case InlineBuiltin::StringPadEnd:
        return twoWayIf(isString(receiver()) && isNumber(argument(0)) && (numberOfArguments() < 2 || isString(argument(1))));
    case InlineBuiltin::StringIsWellFormed:
    case InlineBuiltin::StringToWellFormed:
        return twoWayIf(isString(receiver()));
    case InlineBuiltin::StringNormalize:
        return twoWayIf(isString(receiver()) && !numberOfArguments());
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MATH_BUILTIN(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_REG_EXP_BUILTIN(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_PRIMITIVES(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<SearchValueKind> PrimitiveFormChooser::searchValueKind() const
{
    const BuiltinImplementation& implementation = m_query.overload.code.implementation;
    if (implementation.inlineBuiltin != m_query.builtin)
        return std::nullopt;
    return static_cast<SearchValueKind>(implementation.inlineParameters[0]);
}

bool PrimitiveFormChooser::isSearchValue(TypeID type) const
{
    std::optional<SearchValueKind> kind = searchValueKind();
    if (!kind)
        return false;
    switch (*kind) {
    case SearchValueKind::String:
        return isString(type);
    case SearchValueKind::RegExp:
        return isRegExp(type);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool PrimitiveFormChooser::replacesInLine() const
{
    if (!isString(receiver()) || !isSearchValue(argument(0)))
        return false;
    return searchValueKind() == SearchValueKind::String ? isString(argument(1)) : isValue(argument(1));
}

InlineForm PrimitiveFormChooser::splitForm() const
{
    if (!isString(receiver()) || !isSearchValue(argument(0)))
        return InlineForm::Call;
    TypeID limit = argument(1);
    if (!limit)
        return InlineForm::Whole;
    Type limitType = m_types.at(limit);
    if (limitType.isNever())
        return InlineForm::Call;
    if (limitType.hasOnlyPrimitives(Primitive::Undefined))
        return InlineForm::Whole;
    if (!limitType.isInt32())
        return InlineForm::Call;
    return limitType.integers.minimum < 0 ? twoWayIf(true) : InlineForm::Whole;
}

bool PrimitiveFormChooser::namesValidRegExpFlags(TypeID type) const
{
    StringID text = singleStringLiteralOf(m_types.at(type));
    if (!text)
        return false;
    Vector<char16_t> buffer;
    return Yarr::parseFlags(m_query.context.program.strings.viewOf(text, buffer)).has_value();
}

InlineForm PrimitiveFormChooser::regExpForm() const
{
    switch (m_query.builtin) {
    case InlineBuiltin::RegExpFlags:
    case InlineBuiltin::RegExpSource:
    case InlineBuiltin::RegExpFlag:
    case InlineBuiltin::RegExpGetLastIndex:
        return wholeIf(isRegExp(receiver()));
    case InlineBuiltin::RegExpExec:
    case InlineBuiltin::RegExpTest:
        return wholeIf(isRegExp(receiver()) && isString(argument(0)));
    case InlineBuiltin::StringMatch:
    case InlineBuiltin::StringSearch:
        return wholeIf(isString(receiver()) && isRegExp(argument(0)));
    case InlineBuiltin::StringReplace:
    case InlineBuiltin::StringReplaceAll:
        return wholeIf(replacesInLine());
    case InlineBuiltin::StringSplit:
        return splitForm();
    case InlineBuiltin::NewRegExp:
        return wholeIf(isString(argument(0)) && (!argument(1) || namesValidRegExpFlags(argument(1))));
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MATH_BUILTIN(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_STRING_BUILTIN(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_PRIMITIVES(AOT_CASE_IN_FORMS_OF_PRIMITIVES)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

InlineForm chooseFormOfMathBuiltin(const InlineBuiltinQuery& query)
{
    return PrimitiveFormChooser(query).mathForm();
}

InlineForm chooseFormOfStringBuiltin(const InlineBuiltinQuery& query)
{
    return PrimitiveFormChooser(query).stringForm();
}

InlineForm chooseFormOfRegExpBuiltin(const InlineBuiltinQuery& query)
{
    return PrimitiveFormChooser(query).regExpForm();
}

#undef FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_PRIMITIVES
#undef AOT_CASE_IN_FORMS_OF_PRIMITIVES

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
