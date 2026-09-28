/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonBuiltins.h"

#include "PythonBytes.h"

#include <unicode/uchar.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringBuilder.h>

// str.

namespace JSC { namespace Python {

bool isSpace(char32_t c)
{
    if (c < 0x80)
        return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1C && c <= 0x1F);
    return c == 0x85 || u_isUWhiteSpace(c);
}

static JSValue unboxString(JSValue value)
{
    if (auto* boxed = tryBoxedValue(value))
        return boxed->value();
    return value;
}

// The string that a method was called on. Null, with TypeError raised, if it was called on something else.
static String selfString(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral method)
{
    JSValue self = unboxString(args.at(0));
    if (!self || !self.isString()) {
        raiseTypeError(globalObject, scope, makeString("descriptor '"_s, method, "' for 'str' objects doesn't apply to a '"_s, self ? typeName(globalObject, self) : "NULL"_str, "' object"_s));
        return { };
    }
    return asString(self)->value(globalObject);
}

static String stringArgument(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, ASCIILiteral method, unsigned position)
{
    value = unboxString(value);
    if (!value.isString()) {
        raiseTypeError(globalObject, scope, makeString(method, "() argument "_s, position, " must be str, not "_s, typeName(globalObject, value)));
        return { };
    }
    return asString(value)->value(globalObject);
}

#define STR_PROLOGUE(method) \
    NATIVE_PROLOGUE(); \
    String self = selfString(globalObject, scope, args, method ""_s); \
    RETURN_IF_EXCEPTION(scope, { });

static JSValue toJS(VM& vm, const String& string) { return jsString(vm, string); }
static JSValue toJS(VM& vm, StringView view) { return jsString(vm, view.toString()); }

// From code units, which is what JavaScript's strings are made of, to characters, which is what Python counts in.
static int64_t toCharacterIndex(StringView view, size_t offset)
{
    if (!stringHasSurrogatePairs(view))
        return offset;
    int64_t count = 0;
    for (size_t i = 0; i < offset; ++i, ++count) {
        if (U16_IS_LEAD(view[i]) && i + 1 < view.length() && U16_IS_TRAIL(view[i + 1]))
            ++i;
    }
    return count;
}

static unsigned toOffset(StringView view, int64_t character)
{
    return stringHasSurrogatePairs(view) ? stringOffsetOfCharacter(view, character) : character;
}

static int64_t characterCount(StringView view)
{
    return toCharacterIndex(view, view.length());
}

// The part of a string that optional start and end arguments select, in code units. False if it raised.
static bool sliceArguments(JSGlobalObject* globalObject, ThrowScope& scope, StringView view, JSValue startValue, JSValue endValue, unsigned& start, unsigned& end)
{
    int64_t length = characterCount(view);
    auto resolve = [&] (JSValue value, int64_t whenAbsent) -> int64_t {
        if (!value || isNone(value))
            return whenAbsent;
        auto index = toIndex(globalObject, value, true);
        RETURN_IF_EXCEPTION(scope, 0);
        int64_t i = *index;
        if (i < 0)
            i = std::max<int64_t>(i + length, 0);
        return std::min(i, length);
    };
    int64_t first = resolve(startValue, 0);
    RETURN_IF_EXCEPTION(scope, false);
    int64_t last = resolve(endValue, length);
    RETURN_IF_EXCEPTION(scope, false);
    start = toOffset(view, first);
    end = toOffset(view, std::max(first, last));
    // An empty range past the end is not somewhere that even an empty string is found.
    return true;
}

// ---- Making one

// str(object=''), str(bytes, encoding, errors)
PYTHON_NATIVE(strNew)
{
    NATIVE_PROLOGUE();
    auto* type = asType(args.at(0));
    // Without keywords it is called in a way of its own, which puts this differently.
    if (args.size() > 4 && !args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("str expected at most 3 arguments, got "_s, args.size() - 1)));
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    JSValue value = args.at(1);
    JSValue result = jsEmptyString(vm);
    JSValue encodingValue = args.at(2);
    JSValue errorsValue = args.at(3);
    if (encodingValue || errorsValue) {
        for (JSValue option : { encodingValue, errorsValue }) {
            if (option && !stringIn(option))
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString("str() argument '"_s, option == encodingValue ? "encoding"_s : "errors"_s, "' must be str, not "_s, typeName(globalObject, option))));
        }
        if (value) {
            if (stringIn(value))
                return JSValue::encode(raiseTypeError(globalObject, scope, "decoding str is not supported"_s));
            auto buffer = tryBufferOf(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            if (!buffer)
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString("decoding to str: need a bytes-like object, "_s, typeName(globalObject, value), " found"_s)));
            String text = decodeBytes(globalObject, value, *buffer, encodingValue ? String(stringIn(encodingValue)->value(globalObject)) : String(), errorsValue ? String(stringIn(errorsValue)->value(globalObject)) : String());
            RETURN_IF_EXCEPTION(scope, { });
            result = jsString(vm, text);
        }
    } else if (value) {
        String text = str(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        result = value.isString() ? value : jsString(vm, text);
    }
    return JSValue::encode(boxIfDerived(globalObject, type, realm->typeStr(), result));
}

PYTHON_NATIVE(strStr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(unboxString(args.at(0)));
}

PYTHON_NATIVE(strFormatMethod)
{
    NATIVE_PROLOGUE();
    JSString* specification = stringIn(args[1]);
    if (!specification)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("__format__() argument must be str, not "_s, typeName(globalObject, args[1]))));
    RELEASE_AND_RETURN(scope, JSValue::encode(builtinFormat(globalObject, args[0], specification->value(globalObject))));
}

// ---- Joining and splitting

PYTHON_NATIVE(strJoin)
{
    STR_PROLOGUE("join");
    MarkedArgumentBuffer items;
    collect(globalObject, args[1], items);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::TypeError))
            raiseTypeError(globalObject, scope, "can only join an iterable"_s);
        return { };
    }
    StringBuilder builder;
    for (unsigned i = 0; i < items.size(); ++i) {
        JSValue item = unboxString(items.at(i));
        if (!item.isString())
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("sequence item "_s, i, ": expected str instance, "_s, typeName(globalObject, item), " found"_s)));
        if (i)
            builder.append(self);
        auto view = asString(item)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(view.data);
    }
    return JSValue::encode(toJS(vm, builder.toString()));
}

// split(sep=None, maxsplit=-1)
PYTHON_NATIVE(strSplit)
{
    auto fromRight = unpack<bool>(callFrame, 0);
    STR_PROLOGUE("split");
    JSValue separatorValue = args.at(1);
    JSValue limitValue = args.at(2);
    int64_t limit = -1;
    if (limitValue) {
        auto index = toIndex(globalObject, limitValue, true);
        RETURN_IF_EXCEPTION(scope, { });
        limit = *index;
    }
    if (limit < 0)
        limit = std::numeric_limits<int64_t>::max();

    StringView view = self;
    unsigned length = view.length();
    Vector<std::pair<unsigned, unsigned>, 16> pieces; // Where each begins and ends.

    if (!separatorValue || isNone(separatorValue)) {
        // Runs of white space, and none at the ends.
        if (!fromRight) {
            unsigned i = 0;
            while (true) {
                while (i < length && isSpace(view[i]))
                    ++i;
                if (i >= length)
                    break;
                if (static_cast<int64_t>(pieces.size()) == limit) {
                    pieces.append({ i, length });
                    break;
                }
                unsigned start = i;
                while (i < length && !isSpace(view[i]))
                    ++i;
                pieces.append({ start, i });
            }
        } else {
            unsigned i = length;
            while (true) {
                while (i && isSpace(view[i - 1]))
                    --i;
                if (!i)
                    break;
                if (static_cast<int64_t>(pieces.size()) == limit) {
                    pieces.append({ 0, i });
                    break;
                }
                unsigned end = i;
                while (i && !isSpace(view[i - 1]))
                    --i;
                pieces.append({ i, end });
            }
            pieces.reverse();
        }
    } else {
        String separator = stringArgument(globalObject, scope, separatorValue, "split"_s, 1);
        RETURN_IF_EXCEPTION(scope, { });
        if (separator.isEmpty())
            return JSValue::encode(raiseValueError(globalObject, scope, "empty separator"_s));
        if (!fromRight) {
            unsigned start = 0;
            while (static_cast<int64_t>(pieces.size()) < limit) {
                size_t found = view.find(separator, start);
                if (found == notFound)
                    break;
                pieces.append({ start, static_cast<unsigned>(found) });
                start = found + separator.length();
            }
            pieces.append({ start, length });
        } else {
            unsigned end = length;
            while (static_cast<int64_t>(pieces.size()) < limit && end >= separator.length()) {
                size_t found = view.left(end).reverseFind(separator);
                if (found == notFound)
                    break;
                pieces.append({ static_cast<unsigned>(found + separator.length()), end });
                end = found;
            }
            pieces.append({ 0, end });
            pieces.reverse();
        }
    }

    MarkedArgumentBuffer result;
    for (auto [start, end] : pieces)
        result.append(toJS(vm, view.substring(start, end - start)));
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, result)));
}

PYTHON_NATIVE(strSplitLines)
{
    STR_PROLOGUE("splitlines");
    JSValue keepValue = args.at(1);
    bool keepEnds = keepValue && isTrue(globalObject, keepValue);
    RETURN_IF_EXCEPTION(scope, { });
    auto isLineBreak = [] (char16_t c) {
        return c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x85 || c == 0x2028 || c == 0x2029;
    };
    StringView view = self;
    MarkedArgumentBuffer result;
    unsigned length = view.length();
    for (unsigned i = 0; i < length;) {
        unsigned start = i;
        while (i < length && !isLineBreak(view[i]))
            ++i;
        unsigned end = i;
        if (i < length) {
            if (view[i] == '\r' && i + 1 < length && view[i + 1] == '\n')
                ++i;
            ++i;
        }
        result.append(toJS(vm, view.substring(start, (keepEnds ? i : end) - start)));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, result)));
}

PYTHON_NATIVE(strPartition)
{
    auto fromRight = unpack<bool>(callFrame, 0);
    STR_PROLOGUE("partition");
    String separator = stringArgument(globalObject, scope, args[1], "partition"_s, 1);
    RETURN_IF_EXCEPTION(scope, { });
    if (separator.isEmpty())
        return JSValue::encode(raiseValueError(globalObject, scope, "empty separator"_s));
    StringView view = self;
    size_t found = fromRight ? view.reverseFind(separator) : view.find(separator);
    JSValue empty = jsEmptyString(vm);
    if (found == notFound)
        return JSValue::encode(fromRight ? PyTuple::create(globalObject, { empty, empty, unboxString(args[0]) }) : PyTuple::create(globalObject, { unboxString(args[0]), empty, empty }));
    return JSValue::encode(PyTuple::create(globalObject, { toJS(vm, view.left(found)), toJS(vm, separator), toJS(vm, view.substring(found + separator.length())) }));
}

// ---- Trimming and padding

PYTHON_NATIVE(strStrip)
{
    auto left = unpack<bool>(callFrame, 0);
    auto right = unpack<bool>(callFrame, 1);
    STR_PROLOGUE("strip");
    StringView view = self;
    String characters;
    bool hasCharacters = args.size() > 1 && !isNone(args[1]);
    if (hasCharacters) {
        characters = stringArgument(globalObject, scope, args[1], "strip"_s, 1);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto shouldStrip = [&] (char16_t c) { return hasCharacters ? characters.contains(c) : isSpace(c); };
    unsigned start = 0;
    unsigned end = view.length();
    if (left) {
        while (start < end && shouldStrip(view[start]))
            ++start;
    }
    if (right) {
        while (end > start && shouldStrip(view[end - 1]))
            --end;
    }
    if (!start && end == view.length())
        return JSValue::encode(unboxString(args[0]));
    return JSValue::encode(toJS(vm, view.substring(start, end - start)));
}

// ljust, rjust and center(width, fillchar=' ')
PYTHON_NATIVE(strJustify)
{
    auto align = unpack<char>(callFrame, 0);
    STR_PROLOGUE("center");
    auto width = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    String fill = " "_s;
    if (args.size() > 2) {
        fill = stringArgument(globalObject, scope, args[2], "center"_s, 2);
        RETURN_IF_EXCEPTION(scope, { });
        if (characterCount(fill) != 1)
            return JSValue::encode(raiseTypeError(globalObject, scope, "The fill character must be exactly one character long"_s));
    }
    int64_t length = characterCount(self);
    if (*width <= length)
        return JSValue::encode(unboxString(args[0]));
    int64_t padding = *width - length;
    // As CPython has it, so that an odd one out goes where it does there.
    int64_t before = align == '<' ? 0 : align == '>' ? padding : padding / 2 + (padding & *width & 1);
    StringBuilder builder;
    for (int64_t i = 0; i < before; ++i)
        builder.append(fill);
    builder.append(self);
    for (int64_t i = before; i < padding; ++i)
        builder.append(fill);
    return JSValue::encode(toJS(vm, builder.toString()));
}

PYTHON_NATIVE(strZfill)
{
    STR_PROLOGUE("zfill");
    auto width = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t length = characterCount(self);
    if (*width <= length)
        return JSValue::encode(unboxString(args[0]));
    StringBuilder builder;
    unsigned start = 0;
    if (length && (self[0] == '+' || self[0] == '-'))
        builder.append(self[start++]);
    for (int64_t i = length; i < *width; ++i)
        builder.append('0');
    builder.append(StringView(self).substring(start));
    return JSValue::encode(toJS(vm, builder.toString()));
}

PYTHON_NATIVE(strExpandTabs)
{
    STR_PROLOGUE("expandtabs");
    JSValue sizeValue = args.at(1);
    int64_t tabSize = 8;
    if (sizeValue) {
        auto index = toIndex(globalObject, sizeValue);
        RETURN_IF_EXCEPTION(scope, { });
        tabSize = *index;
    }
    StringBuilder builder;
    int64_t column = 0;
    for (char32_t c : StringView(self).codePoints()) {
        if (c == '\t') {
            if (tabSize > 0) {
                int64_t spaces = tabSize - column % tabSize;
                for (int64_t i = 0; i < spaces; ++i)
                    builder.append(' ');
                column += spaces;
            }
            continue;
        }
        builder.append(c);
        column = c == '\n' || c == '\r' ? 0 : column + 1;
    }
    return JSValue::encode(toJS(vm, builder.toString()));
}

PYTHON_NATIVE(strRemoveAffix)
{
    auto prefix = unpack<bool>(callFrame, 0);
    STR_PROLOGUE("removeprefix");
    String affix = stringArgument(globalObject, scope, args[1], prefix ? "removeprefix"_s : "removesuffix"_s, 1);
    RETURN_IF_EXCEPTION(scope, { });
    if (!affix.isEmpty() && (prefix ? self.startsWith(affix) : self.endsWith(affix)))
        return JSValue::encode(toJS(vm, prefix ? StringView(self).substring(affix.length()) : StringView(self).left(self.length() - affix.length())));
    return JSValue::encode(unboxString(args[0]));
}

// ---- Case

PYTHON_NATIVE(strUpper)
{
    STR_PROLOGUE("upper");
    return JSValue::encode(toJS(vm, self.convertToUppercaseWithoutLocale()));
}

PYTHON_NATIVE(strLower)
{
    STR_PROLOGUE("lower");
    return JSValue::encode(toJS(vm, self.convertToLowercaseWithoutLocale()));
}

PYTHON_NATIVE(strCasefold)
{
    STR_PROLOGUE("casefold");
    return JSValue::encode(toJS(vm, self.foldCase()));
}

static bool isCased(char32_t c) { return u_isUUppercase(c) || u_isULowercase(c) || u_istitle(c); }

PYTHON_NATIVE(strSwapCase)
{
    STR_PROLOGUE("swapcase");
    StringBuilder builder;
    for (char32_t c : StringView(self).codePoints())
        builder.append(static_cast<char32_t>(u_isUUppercase(c) ? u_tolower(c) : u_isULowercase(c) ? u_toupper(c) : c));
    return JSValue::encode(toJS(vm, builder.toString()));
}

PYTHON_NATIVE(strCapitalize)
{
    STR_PROLOGUE("capitalize");
    StringBuilder builder;
    bool isFirst = true;
    for (char32_t c : StringView(self).codePoints()) {
        builder.append(static_cast<char32_t>(isFirst ? u_totitle(c) : u_tolower(c)));
        isFirst = false;
    }
    return JSValue::encode(toJS(vm, builder.toString()));
}

PYTHON_NATIVE(strTitle)
{
    STR_PROLOGUE("title");
    StringBuilder builder;
    bool previousIsCased = false;
    for (char32_t c : StringView(self).codePoints()) {
        builder.append(static_cast<char32_t>(previousIsCased ? u_tolower(c) : u_totitle(c)));
        previousIsCased = isCased(c);
    }
    return JSValue::encode(toJS(vm, builder.toString()));
}

// ---- Questions

static bool isAlpha(char32_t c) { return u_isalpha(c); }
static bool isDecimal(char32_t c) { return u_charType(c) == U_DECIMAL_DIGIT_NUMBER; }
static bool isDigit(char32_t c) { return u_charDigitValue(c) >= 0 || u_getIntPropertyValue(c, UCHAR_NUMERIC_TYPE) == U_NT_DIGIT; }
static bool isNumeric(char32_t c) { return u_getIntPropertyValue(c, UCHAR_NUMERIC_TYPE) != U_NT_NONE; }
static bool isAlphanumeric(char32_t c) { return isAlpha(c) || isNumeric(c); }

enum class CharacterClass : uint8_t { Alpha, Decimal, Digit, Numeric, Alphanumeric, Space };

// True if there is at least one character and all of them are of the class.
PYTHON_NATIVE(strAll)
{
    static constexpr bool (*predicates[])(char32_t) = { isAlpha, isDecimal, isDigit, isNumeric, isAlphanumeric, isSpace };
    auto predicate = predicates[unpack<unsigned>(callFrame, 0)];
    STR_PROLOGUE("isalpha");
    if (self.isEmpty())
        return JSValue::encode(jsBoolean(false));
    for (char32_t c : StringView(self).codePoints()) {
        if (!predicate(c))
            return JSValue::encode(jsBoolean(false));
    }
    return JSValue::encode(jsBoolean(true));
}

// True if there is a cased character, and all of those are upper (or lower) case.
PYTHON_NATIVE(strIsCase)
{
    auto upper = unpack<bool>(callFrame, 0);
    STR_PROLOGUE("isupper");
    bool hasCased = false;
    for (char32_t c : StringView(self).codePoints()) {
        if (upper ? (u_isULowercase(c) || u_istitle(c)) : (u_isUUppercase(c) || u_istitle(c)))
            return JSValue::encode(jsBoolean(false));
        hasCased |= isCased(c);
    }
    return JSValue::encode(jsBoolean(hasCased));
}

PYTHON_NATIVE(strIsTitle)
{
    STR_PROLOGUE("istitle");
    bool hasCased = false;
    bool previousIsCased = false;
    for (char32_t c : StringView(self).codePoints()) {
        if (u_isUUppercase(c) || u_istitle(c)) {
            if (previousIsCased)
                return JSValue::encode(jsBoolean(false));
            previousIsCased = true;
            hasCased = true;
        } else if (u_isULowercase(c)) {
            if (!previousIsCased)
                return JSValue::encode(jsBoolean(false));
            hasCased = true;
        } else
            previousIsCased = false;
    }
    return JSValue::encode(jsBoolean(hasCased));
}

PYTHON_NATIVE(strIsAscii)
{
    STR_PROLOGUE("isascii");
    return JSValue::encode(jsBoolean(self.containsOnlyASCII()));
}

bool isIdentifier(StringView string)
{
    bool isFirst = true;
    for (char32_t c : string.codePoints()) {
        if (isFirst ? !(c == '_' || u_hasBinaryProperty(c, UCHAR_XID_START)) : !u_hasBinaryProperty(c, UCHAR_XID_CONTINUE))
            return false;
        isFirst = false;
    }
    return !isFirst;
}

PYTHON_NATIVE(strIsIdentifier)
{
    STR_PROLOGUE("isidentifier");
    return JSValue::encode(jsBoolean(isIdentifier(self)));
}

PYTHON_NATIVE(strIsPrintable)
{
    STR_PROLOGUE("isprintable");
    // What repr() leaves as it is.
    for (char32_t c : StringView(self).codePoints()) {
        StringBuilder one;
        one.append(c);
        String quoted = reprOfString(one.toString());
        if (c != '\\' && c != '\'' && quoted.length() != one.length() + 2)
            return JSValue::encode(jsBoolean(false));
    }
    return JSValue::encode(jsBoolean(true));
}

// startswith(prefix or a tuple of them, start, end)
PYTHON_NATIVE(strStartsOrEndsWith)
{
    auto atStart = unpack<bool>(callFrame, 0);
    STR_PROLOGUE("startswith");
    ASCIILiteral method = atStart ? "startswith"_s : "endswith"_s;
    unsigned start;
    unsigned end;
    sliceArguments(globalObject, scope, self, args.at(2), args.at(3), start, end);
    RETURN_IF_EXCEPTION(scope, { });
    StringView view = StringView(self).substring(start, end - start);
    auto matches = [&] (JSValue candidate) -> bool {
        auto affix = asString(candidate)->view(globalObject);
        return atStart ? view.startsWith(affix) : view.endsWith(affix);
    };
    JSValue candidate = unboxString(args[1]);
    if (candidate.isString())
        return JSValue::encode(jsBoolean(matches(candidate)));
    if (!isTuple(candidate))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(method, " first arg must be str or a tuple of str, not "_s, typeName(globalObject, candidate))));
    for (auto& entry : uncheckedDowncast<PyTuple>(candidate.asCell())->span()) {
        JSValue item = unboxString(entry.get());
        if (!item.isString())
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("tuple for "_s, method, " must only contain str, not "_s, typeName(globalObject, item))));
        if (matches(item))
            return JSValue::encode(jsBoolean(true));
    }
    return JSValue::encode(jsBoolean(false));
}

// ---- Searching

// find, rfind, index and rindex(sub, start, end)
PYTHON_NATIVE(strFind)
{
    auto fromRight = unpack<bool>(callFrame, 0);
    auto raises = unpack<bool>(callFrame, 1);
    STR_PROLOGUE("find");
    ASCIILiteral method = raises ? (fromRight ? "rindex"_s : "index"_s) : (fromRight ? "rfind"_s : "find"_s);
    String needle = stringArgument(globalObject, scope, args[1], method, 1);
    RETURN_IF_EXCEPTION(scope, { });
    // What is given for the start is asked what it is once.
    JSValue startValue = args.at(2);
    bool startsBeyondEnd = false;
    if (startValue && !isNone(startValue)) {
        auto given = toIndex(globalObject, startValue, true);
        RETURN_IF_EXCEPTION(scope, { });
        startsBeyondEnd = *given > characterCount(self);
        startValue = intFromInt64(globalObject, *given);
    }
    unsigned start;
    unsigned end;
    sliceArguments(globalObject, scope, self, startValue, args.at(3), start, end);
    RETURN_IF_EXCEPTION(scope, { });
    StringView view = StringView(self).substring(start, end - start);
    size_t found = fromRight ? view.reverseFind(needle) : view.find(needle);
    // A start beyond the end finds nothing, not even nothing.
    if (startsBeyondEnd)
        found = notFound;
    if (found == notFound) {
        if (raises)
            return JSValue::encode(raiseValueError(globalObject, scope, "substring not found"_s));
        return JSValue::encode(jsNumber(-1));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, toCharacterIndex(self, start + found))));
}

PYTHON_NATIVE(strCount)
{
    STR_PROLOGUE("count");
    String needle = stringArgument(globalObject, scope, args[1], "count"_s, 1);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned start;
    unsigned end;
    sliceArguments(globalObject, scope, self, args.at(2), args.at(3), start, end);
    RETURN_IF_EXCEPTION(scope, { });
    StringView view = StringView(self).substring(start, end - start);
    if (needle.isEmpty())
        RELEASE_AND_RETURN(scope, JSValue::encode(intFromInt64(globalObject, characterCount(view) + 1)));
    int32_t count = 0;
    for (size_t at = view.find(needle); at != notFound; at = view.find(needle, at + needle.length()))
        ++count;
    return JSValue::encode(jsNumber(count));
}

// replace(old, new, count=-1)
PYTHON_NATIVE(strReplace)
{
    STR_PROLOGUE("replace");
    if (args.size() < 3 || args.size() > 4)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("replace expected at least 2 arguments, got "_s, args.size() - 1)));
    String from = stringArgument(globalObject, scope, args[1], "replace"_s, 1);
    RETURN_IF_EXCEPTION(scope, { });
    String to = stringArgument(globalObject, scope, args[2], "replace"_s, 2);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue countValue = args.at(3);
    int64_t limit = -1;
    if (countValue) {
        auto index = toIndex(globalObject, countValue, true);
        RETURN_IF_EXCEPTION(scope, { });
        limit = *index;
    }
    if (limit < 0)
        limit = std::numeric_limits<int64_t>::max();

    StringView view = self;
    StringBuilder builder;
    if (from.isEmpty()) {
        // Between every two characters, and at both ends.
        int64_t done = 0;
        for (char32_t c : view.codePoints()) {
            if (done++ < limit)
                builder.append(to);
            builder.append(c);
        }
        if (done < limit)
            builder.append(to);
        return JSValue::encode(toJS(vm, builder.toString()));
    }
    unsigned start = 0;
    for (int64_t done = 0; done < limit; ++done) {
        size_t found = view.find(from, start);
        if (found == notFound)
            break;
        builder.append(view.substring(start, found - start), to);
        start = found + from.length();
    }
    if (!start)
        return JSValue::encode(unboxString(args[0]));
    builder.append(view.substring(start));
    return JSValue::encode(toJS(vm, builder.toString()));
}

// ---- format()

// A port of Objects/stringlib/unicode_format.h of CPython. What is an error, and which, depends on just how the string is gone through.
class Formatter {
public:
    // With `mapping`, it is format_map(): names are looked up in it, and there are no positional arguments.
    Formatter(JSGlobalObject* globalObject, const NativeArguments& args, JSValue mapping)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_args(args)
        , m_mapping(mapping)
    {
    }

    // Null if it raised.
    String format(const String& text)
    {
        Characters characters;
        for (char32_t c : StringView(text).codePoints())
            characters.append(c);
        return buildString(characters, 0, characters.size(), 2);
    }

private:
    using Characters = Vector<char32_t, 64>;

    struct Range {
        size_t start { 0 };
        size_t end { 0 };
        bool isEmpty() const { return start >= end; }
    };

    static String toString(const Characters& characters, Range range)
    {
        StringBuilder builder;
        for (size_t i = range.start; i < range.end; ++i)
            builder.append(characters[i]);
        String result = builder.toString();
        return result.isNull() ? emptyString() : result;
    }

    String buildString(const Characters& s, size_t start, size_t end, int recursionDepth)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (recursionDepth <= 0) {
            raiseValueError(m_globalObject, scope, "Max string recursion exceeded"_s);
            return { };
        }
        StringBuilder result;
        size_t position = start;
        while (position < end) {
            // Literal text, up to a brace.
            size_t literalStart = position;
            char32_t c = 0;
            bool markupFollows = false;
            while (position < end) {
                c = s[position++];
                if (c == '{' || c == '}') {
                    markupFollows = true;
                    break;
                }
            }
            bool atEnd = position >= end;
            size_t length = position - literalStart;
            if (markupFollows && c == '}' && (atEnd || s[position] != c)) {
                raiseValueError(m_globalObject, scope, "Single '}' encountered in format string"_s);
                return { };
            }
            if (markupFollows && atEnd && c == '{') {
                raiseValueError(m_globalObject, scope, "Single '{' encountered in format string"_s);
                return { };
            }
            if (markupFollows && !atEnd) {
                if (s[position] == c) {
                    // A doubled brace stands for one.
                    ++position;
                    markupFollows = false;
                } else
                    --length;
            }
            for (size_t i = literalStart; i < literalStart + length; ++i)
                result.append(s[i]);
            if (!markupFollows)
                continue;

            Range fieldName;
            Range specification;
            bool specificationNeedsExpanding = false;
            char32_t conversion = 0;
            if (!parseField(s, position, end, fieldName, specification, specificationNeedsExpanding, conversion))
                return { };
            String piece = outputMarkup(s, fieldName, specification, specificationNeedsExpanding, conversion, recursionDepth);
            RETURN_IF_EXCEPTION(scope, { });
            result.append(piece);
        }
        String text = result.toString();
        return text.isNull() ? emptyString() : text;
    }

    bool parseField(const Characters& s, size_t& position, size_t end, Range& fieldName, Range& specification, bool& specificationNeedsExpanding, char32_t& conversion)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto fail = [&] (ASCIILiteral message) {
            raiseValueError(m_globalObject, scope, message);
            return false;
        };
        char32_t c = 0;
        fieldName.start = position;
        while (position < end) {
            c = s[position++];
            if (c == '{')
                return fail("unexpected '{' in field name"_s);
            if (c == '[') {
                for (; position < end; ++position) {
                    if (s[position] == ']')
                        break;
                }
                continue;
            }
            if (c == '}' || c == ':' || c == '!')
                break;
        }
        fieldName.end = position - 1;
        if (c == '!' || c == ':') {
            if (c == '!') {
                if (position >= end)
                    return fail("end of string while looking for conversion specifier"_s);
                conversion = s[position++];
                if (position < end) {
                    c = s[position++];
                    if (c == '}')
                        return true;
                    if (c != ':')
                        return fail("expected ':' after conversion specifier"_s);
                }
            }
            specification.start = position;
            unsigned count = 1;
            while (position < end) {
                c = s[position++];
                if (c == '{') {
                    specificationNeedsExpanding = true;
                    ++count;
                } else if (c == '}' && !--count) {
                    specification.end = position - 1;
                    return true;
                }
            }
            return fail("unmatched '{' in format spec"_s);
        }
        if (c != '}')
            return fail("expected '}' before end of string"_s);
        return true;
    }

    // The number that the characters spell, or -1 if they spell none. -2 if it raised.
    int64_t getInteger(const Characters& s, Range range)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (range.isEmpty())
            return -1;
        int64_t accumulator = 0;
        for (size_t i = range.start; i < range.end; ++i) {
            int digit = u_charType(s[i]) == U_DECIMAL_DIGIT_NUMBER ? u_charDigitValue(s[i]) : -1;
            if (digit < 0)
                return -1;
            if (accumulator > (std::numeric_limits<int64_t>::max() - digit) / 10) {
                raiseValueError(m_globalObject, scope, "Too many decimal digits in format string"_s);
                return -2;
            }
            accumulator = accumulator * 10 + digit;
        }
        return accumulator;
    }

    JSValue getFieldObject(const Characters& s, Range input)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        // Up to the first '.' or '['.
        size_t i = input.start;
        while (i < input.end && s[i] != '[' && s[i] != '.')
            ++i;
        Range first { input.start, i };
        int64_t index = getInteger(s, first);
        RETURN_IF_EXCEPTION(scope, { });
        bool isEmpty = first.isEmpty();
        if (isEmpty || index != -1) {
            if (m_numbering == Numbering::Undecided)
                m_numbering = isEmpty ? Numbering::Automatic : Numbering::Manual;
            if (m_numbering == Numbering::Manual && isEmpty)
                return raiseValueError(m_globalObject, scope, "cannot switch from manual field specification to automatic field numbering"_s);
            if (m_numbering == Numbering::Automatic && !isEmpty)
                return raiseValueError(m_globalObject, scope, "cannot switch from automatic field numbering to manual field specification"_s);
            if (isEmpty)
                index = m_nextIndex++;
        }

        JSValue object;
        if (index == -1) {
            JSValue key = jsString(m_vm, toString(s, first));
            if (m_mapping)
                object = getItem(m_globalObject, m_mapping, key);
            else {
                String name = asString(key)->value(m_globalObject);
                for (unsigned k = 0; k < m_args.keywordCount() && !object; ++k) {
                    if (m_args.keywordName(k)->value(m_globalObject).data == name)
                        object = m_args.keywordValue(k);
                }
                if (!object)
                    return raise(m_globalObject, scope, BuiltinType::KeyError, key);
            }
            RETURN_IF_EXCEPTION(scope, { });
        } else {
            if (m_mapping)
                return raiseValueError(m_globalObject, scope, "Format string contains positional fields"_s);
            // The first argument is the format string itself.
            if (index >= static_cast<int64_t>(m_args.size()) - 1)
                return raise(m_globalObject, scope, BuiltinType::IndexError, makeString("Replacement index "_s, index, " out of range for positional args tuple"_s));
            object = m_args[index + 1];
        }

        // .attribute and [item], as many as there are.
        while (i < input.end) {
            char32_t c = s[i++];
            Range name;
            name.start = i;
            if (c == '.') {
                while (i < input.end && s[i] != '.' && s[i] != '[')
                    ++i;
                name.end = i;
                if (name.isEmpty())
                    return raiseValueError(m_globalObject, scope, "Empty attribute in format string"_s);
                object = getAttribute(m_globalObject, object, Identifier::fromString(m_vm, toString(s, name)));
            } else if (c == '[') {
                bool sawBracket = false;
                while (i < input.end) {
                    if (s[i++] == ']') {
                        sawBracket = true;
                        break;
                    }
                }
                if (!sawBracket)
                    return raiseValueError(m_globalObject, scope, "Missing ']' in format string"_s);
                name.end = i - 1;
                int64_t item = getInteger(s, name);
                RETURN_IF_EXCEPTION(scope, { });
                if (name.isEmpty())
                    return raiseValueError(m_globalObject, scope, "Empty attribute in format string"_s);
                object = getItem(m_globalObject, object, item == -1 ? JSValue(jsString(m_vm, toString(s, name))) : intFromInt64(m_globalObject, item));
            } else
                return raiseValueError(m_globalObject, scope, "Only '.' or '[' may follow ']' in format field specifier"_s);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return object;
    }

    String outputMarkup(const Characters& s, Range fieldName, Range specification, bool specificationNeedsExpanding, char32_t conversion, int recursionDepth)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue object = getFieldObject(s, fieldName);
        RETURN_IF_EXCEPTION(scope, { });
        if (conversion) {
            String text;
            if (conversion == 'r')
                text = repr(m_globalObject, object);
            else if (conversion == 's')
                text = str(m_globalObject, object);
            else if (conversion == 'a') {
                text = repr(m_globalObject, object);
                if (!text.isNull())
                    text = escapeNonASCII(text);
            } else {
                if (conversion > 32 && conversion < 127)
                    raiseValueError(m_globalObject, scope, makeString("Unknown conversion specifier "_s, static_cast<char>(conversion)));
                else
                    raiseValueError(m_globalObject, scope, makeString("Unknown conversion specifier \\x"_s, hex(static_cast<unsigned>(conversion), Lowercase)));
                return { };
            }
            RETURN_IF_EXCEPTION(scope, { });
            object = jsString(m_vm, text);
        }
        String specificationText;
        if (specificationNeedsExpanding) {
            specificationText = buildString(s, specification.start, specification.end, recursionDepth - 1);
            RETURN_IF_EXCEPTION(scope, { });
        } else
            specificationText = toString(s, specification);
        JSValue result = Python::format(m_globalObject, object, specificationText);
        RETURN_IF_EXCEPTION(scope, { });
        return asString(result)->value(m_globalObject);
    }

    enum class Numbering : uint8_t { Undecided, Automatic, Manual };

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    const NativeArguments& m_args;
    JSValue m_mapping;
    int64_t m_nextIndex { 0 };
    Numbering m_numbering { Numbering::Undecided };
};

PYTHON_NATIVE(strFormat)
{
    STR_PROLOGUE("format");
    String result = Formatter(globalObject, args, JSValue()).format(self);
    RETURN_IF_EXCEPTION(scope, { });
    // As with `format % values`: where there is nothing to fill in, it is itself.
    if (!self.contains('{') && !self.contains('}'))
        return JSValue::encode(args[0]);
    return JSValue::encode(toJS(vm, result));
}

PYTHON_NATIVE(strFormatMap)
{
    STR_PROLOGUE("format_map");
    String result = Formatter(globalObject, args, args[1]).format(self);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(toJS(vm, result));
}

// ---- translate() and maketrans(): _PyUnicode_TranslateCharmap() and unicode_maketrans_impl() of CPython's Objects/unicodeobject.c

// What the table has for a character.
struct Translation {
    enum class Kind : uint8_t { Same, Deleted, Character, Text } kind { Kind::Same };
    char32_t character { 0 };
    JSString* text { nullptr };
};

static Translation lookUpTranslation(JSGlobalObject* globalObject, JSValue table, char32_t character)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue found = getItem(globalObject, table, jsNumber(static_cast<int32_t>(character)));
    if (scope.exception()) {
        // What is not in the table stays as it is.
        catchException(globalObject, BuiltinType::LookupError);
        return { };
    }
    if (isNone(found))
        return { Translation::Kind::Deleted };
    if (isInstance(globalObject, found, globalObject->pyRealm()->typeInt())) {
        auto value = toIndex(globalObject, found, true);
        if (*value < 0 || *value > 0x10FFFF) {
            raiseValueError(globalObject, scope, "character mapping must be in range(0x110000)"_s);
            return { };
        }
        return { Translation::Kind::Character, static_cast<char32_t>(*value) };
    }
    if (JSString* text = stringIn(found))
        return { Translation::Kind::Text, 0, text };
    raiseTypeError(globalObject, scope, "character mapping must return integer, None or str"_s);
    return { };
}

PYTHON_NATIVE(strTranslate)
{
    STR_PROLOGUE("translate");
    JSValue table = args[1];
    StringBuilder result;
    StringView view { self };
    unsigned start = 0;

    // While it is ASCII to ASCII the table is asked about each character once, as in CPython, where it can be told how often it is asked.
    if (view.containsOnlyASCII()) {
        constexpr uint8_t unknown = 0xFF;
        constexpr uint8_t deleted = 0xFE;
        std::array<uint8_t, 128> known;
        known.fill(unknown);
        for (; start < view.length(); ++start) {
            uint8_t character = view[start];
            if (known[character] == unknown) {
                Translation translation = lookUpTranslation(globalObject, table, character);
                RETURN_IF_EXCEPTION(scope, { });
                char32_t replacement = character;
                if (translation.kind == Translation::Kind::Character)
                    replacement = translation.character;
                else if (translation.kind == Translation::Kind::Text) {
                    if (translation.text->length() != 1)
                        break;
                    replacement = translation.text->view(globalObject)[0];
                }
                if (replacement > 127)
                    break;
                known[character] = translation.kind == Translation::Kind::Deleted ? deleted : static_cast<uint8_t>(replacement);
            }
            if (known[character] != deleted)
                result.append(static_cast<Latin1Character>(known[character]));
        }
    }

    for (char32_t character : view.substring(start).codePoints()) {
        Translation translation = lookUpTranslation(globalObject, table, character);
        RETURN_IF_EXCEPTION(scope, { });
        switch (translation.kind) {
        case Translation::Kind::Same:
            result.append(character);
            break;
        case Translation::Kind::Deleted:
            break;
        case Translation::Kind::Character:
            result.append(translation.character);
            break;
        case Translation::Kind::Text:
            result.append(translation.text->view(globalObject).data);
            break;
        }
    }
    return JSValue::encode(toJS(vm, result.toString()));
}

// str.maketrans(x[, y[, z]])
PYTHON_NATIVE(strMakeTrans)
{
    NATIVE_PROLOGUE();
    JSValue x = args[0];
    JSValue y = args.at(1);
    JSValue z = args.at(2);
    for (unsigned i = 1; i < args.size(); ++i) {
        if (!stringIn(args[i]))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("maketrans() argument "_s, i + 1, " must be str, not "_s, isNone(args[i]) ? "None"_s : typeName(globalObject, args[i]))));
    }
    PyDict* table = PyDict::create(globalObject);
    auto number = [] (char32_t character) { return jsNumber(static_cast<int32_t>(character)); };
    if (y) {
        if (!stringIn(x))
            return JSValue::encode(raiseTypeError(globalObject, scope, "first maketrans argument must be a string if there is a second argument"_s));
        Vector<char32_t> from;
        for (char32_t character : stringIn(x)->view(globalObject)->codePoints())
            from.append(character);
        Vector<char32_t> to;
        for (char32_t character : stringIn(y)->view(globalObject)->codePoints())
            to.append(character);
        if (from.size() != to.size())
            return JSValue::encode(raiseValueError(globalObject, scope, "the first two maketrans arguments must have equal length"_s));
        for (size_t i = 0; i < from.size(); ++i)
            table->set(globalObject, number(from[i]), number(to[i]));
        if (z) {
            for (char32_t character : stringIn(z)->view(globalObject)->codePoints())
                table->set(globalObject, number(character), jsUndefined());
        }
        return JSValue::encode(table);
    }

    if (!isExactly(globalObject, x, realm->typeDict()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "if you give only one argument to maketrans it must be a dict"_s));
    MarkedArgumentBuffer pairs;
    asDict(x)->forEach(globalObject, [&] (JSValue key, JSValue value) {
        pairs.append(key);
        pairs.append(value);
        return true;
    });
    for (size_t i = 0; i < pairs.size(); i += 2) {
        JSValue key = pairs.at(i);
        if (JSString* text = stringIn(key)) {
            auto codePoints = text->view(globalObject)->codePoints();
            auto first = codePoints.begin();
            if (first == codePoints.end() || ++codePoints.begin() != codePoints.end())
                return JSValue::encode(raiseValueError(globalObject, scope, "string keys in translatetable must be of length 1"_s)); // As CPython has it.
            key = number(*first);
        } else if (!isInstance(globalObject, key, realm->typeInt()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "keys in translate table mustbe strings or integers"_s)); // The same.
        table->set(globalObject, key, pairs.at(i + 1));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(table);
}

void initializeStrType(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = realm->typeStr();
    using Kind = PyNativeFunction::Kind;
    addMethods(globalObject, type, {
        { "__new__"_s, strNew, Kind::New, 0, "(object='', encoding='utf-8', errors='strict')"_s, PyNativeFunction::Arguments::AreThoseOfTheClassButNotChecked },
        { "__str__"_s, strStr },
        { "__repr__"_s, nativeRepr },
        { "__hash__"_s, nativeHash },
        { "__len__"_s, nativeLen },
        { "__getitem__"_s, nativeGetItem },
        { "__contains__"_s, nativeContains },
        { "__iter__"_s, nativeIter },
        { "__format__"_s, strFormatMethod },
        { "join"_s, strJoin },
        { "split"_s, strSplit, PyNativeFunction::Kind::Method, pack(false) },
        { "rsplit"_s, strSplit, PyNativeFunction::Kind::Method, pack(true) },
        { "splitlines"_s, strSplitLines },
        { "partition"_s, strPartition, PyNativeFunction::Kind::Method, pack(false) },
        { "rpartition"_s, strPartition, PyNativeFunction::Kind::Method, pack(true) },
        { "strip"_s, strStrip, PyNativeFunction::Kind::Method, pack(true, true) },
        { "lstrip"_s, strStrip, PyNativeFunction::Kind::Method, pack(true, false) },
        { "rstrip"_s, strStrip, PyNativeFunction::Kind::Method, pack(false, true) },
        { "ljust"_s, strJustify, PyNativeFunction::Kind::Method, pack('<') },
        { "rjust"_s, strJustify, PyNativeFunction::Kind::Method, pack('>') },
        { "center"_s, strJustify, PyNativeFunction::Kind::Method, pack('^') },
        { "zfill"_s, strZfill },
        { "expandtabs"_s, strExpandTabs },
        { "removeprefix"_s, strRemoveAffix, PyNativeFunction::Kind::Method, pack(true) },
        { "removesuffix"_s, strRemoveAffix, PyNativeFunction::Kind::Method, pack(false) },
        { "upper"_s, strUpper },
        { "lower"_s, strLower },
        { "casefold"_s, strCasefold },
        { "swapcase"_s, strSwapCase },
        { "capitalize"_s, strCapitalize },
        { "title"_s, strTitle },
        { "isalpha"_s, strAll, PyNativeFunction::Kind::Method, pack(CharacterClass::Alpha) },
        { "isdecimal"_s, strAll, PyNativeFunction::Kind::Method, pack(CharacterClass::Decimal) },
        { "isdigit"_s, strAll, PyNativeFunction::Kind::Method, pack(CharacterClass::Digit) },
        { "isnumeric"_s, strAll, PyNativeFunction::Kind::Method, pack(CharacterClass::Numeric) },
        { "isalnum"_s, strAll, PyNativeFunction::Kind::Method, pack(CharacterClass::Alphanumeric) },
        { "isspace"_s, strAll, PyNativeFunction::Kind::Method, pack(CharacterClass::Space) },
        { "isupper"_s, strIsCase, PyNativeFunction::Kind::Method, pack(true) },
        { "islower"_s, strIsCase, PyNativeFunction::Kind::Method, pack(false) },
        { "istitle"_s, strIsTitle },
        { "isascii"_s, strIsAscii },
        { "isidentifier"_s, strIsIdentifier },
        { "isprintable"_s, strIsPrintable },
        { "startswith"_s, strStartsOrEndsWith, PyNativeFunction::Kind::Method, pack(true) },
        { "endswith"_s, strStartsOrEndsWith, PyNativeFunction::Kind::Method, pack(false) },
        { "find"_s, strFind, PyNativeFunction::Kind::Method, pack(false, false) },
        { "rfind"_s, strFind, PyNativeFunction::Kind::Method, pack(true, false) },
        { "index"_s, strFind, PyNativeFunction::Kind::Method, pack(false, true) },
        { "rindex"_s, strFind, PyNativeFunction::Kind::Method, pack(true, true) },
        { "count"_s, strCount },
        { "replace"_s, strReplace },
        { "format"_s, strFormat },
        { "format_map"_s, strFormatMap },
        { "translate"_s, strTranslate },
        { "maketrans"_s, strMakeTrans, Kind::Function },
    });
    addComparisons(globalObject, type);
    addBinaryOperators(globalObject, type, { BinaryOperator::Add }, false, false);
    addBinaryOperators(globalObject, type, { BinaryOperator::Mult, BinaryOperator::Mod }, true, false);
}

} } // namespace JSC::Python
