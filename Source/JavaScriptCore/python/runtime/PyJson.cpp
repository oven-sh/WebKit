/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyException.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _json: what makes the package json fast. (Modules/_json.c in CPython is the reference.)
//
// Python counts the characters of a str, the engine its UTF-16 code units. The scanner works in code units; what comes in from
// Python, or goes out to it, as an index is converted on the way (which costs nothing for a str without astral characters).

// ---- Writing strings ----

static constexpr char jsonHexDigits[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

// The letter after the backslash. 0: the character has no short escape.
static ALWAYS_INLINE char jsonShortEscape(char16_t c)
{
    switch (c) {
    case '\\':
    case '"':
        return static_cast<char>(c);
    case '\b':
        return 'b';
    case '\f':
        return 'f';
    case '\n':
        return 'n';
    case '\r':
        return 'r';
    case '\t':
        return 't';
    default:
        return 0;
    }
}

static void jsonAppendEscape(StringBuilder& out, char16_t c)
{
    out.append('\\');
    if (char letter = jsonShortEscape(c)) {
        out.append(letter);
        return;
    }
    out.append('u', jsonHexDigits[c >> 12], jsonHexDigits[(c >> 8) & 15], jsonHexDigits[(c >> 4) & 15], jsonHexDigits[c & 15]);
}

template<bool asciiOnly, typename CharacterType>
static void jsonAppendQuotedCharacters(StringBuilder& out, std::span<const CharacterType> characters)
{
    auto isPlain = [](CharacterType c) {
        if (c == '"' || c == '\\' || c < ' ')
            return false;
        return !asciiOnly || c <= '~';
    };
    out.append('"');
    size_t start = 0;
    for (size_t i = 0; i < characters.size(); ++i) {
        if (isPlain(characters[i])) [[likely]]
            continue;
        if (i > start)
            out.append(characters.subspan(start, i - start));
        // An astral character is its two surrogates, which is what it is here already.
        jsonAppendEscape(out, characters[i]);
        start = i + 1;
    }
    if (characters.size() > start)
        out.append(characters.subspan(start));
    out.append('"');
}

template<bool asciiOnly>
static void jsonAppendQuoted(StringBuilder& out, StringView text)
{
    if (text.is8Bit())
        jsonAppendQuotedCharacters<asciiOnly>(out, text.span8());
    else
        jsonAppendQuotedCharacters<asciiOnly>(out, text.span16());
}

template<bool asciiOnly>
static JSValue jsonEncodeBaseString(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value))
        return raiseTypeError(globalObject, scope, asciiOnly ? "encode_basestring_ascii"_s : "encode_basestring"_s, "() argument must be str, not "_s, typeNameOfArgument(globalObject, value));
    String text = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    StrBuilder out;
    jsonAppendQuoted<asciiOnly>(out, text);
    if (out.hasOverflowed())
        return raiseOverflowError(globalObject, scope, "string is too long to escape"_s);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, out));
}

// ---- The scanner ----

enum JsonScannerField : unsigned {
    JsonScannerObjectHook,
    JsonScannerObjectPairsHook,
    JsonScannerArrayHook,
    JsonScannerParseFloat,
    JsonScannerParseInt,
    JsonScannerParseConstant,
    JsonScannerFieldCount
}; // number: strict

// json.decoder.JSONDecodeError(message, document, index)
static NEVER_INLINE void jsonRaiseDecodeError(JSGlobalObject* globalObject, ASCIILiteral message, JSString* document, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "json.decoder"_s);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue errorType = getAttr(globalObject, module, internString(vm, "JSONDecodeError"_s));
    RETURN_IF_EXCEPTION(scope, void());
    JSValue exception = call(globalObject, errorType, makeStr(vm, message), document, jsNumber(static_cast<double>(index)));
    RETURN_IF_EXCEPTION(scope, void());
    raiseException(globalObject, scope, exception);
}

template<typename CharacterType>
class JsonScanner {
public:
    // `scanner` is null for scanstring(), which needs none.
    JsonScanner(JSGlobalObject* globalObject, PyRecord* scanner, JSString* document, const StrView& view, std::span<const CharacterType> characters, bool strict)
        : m_globalObject(globalObject)
        , m_vm(getVM(globalObject))
        , m_scanner(scanner)
        , m_document(document)
        , m_view(view)
        , m_characters(characters)
        , m_length(characters.size())
        , m_strict(strict)
    {
    }

    // `end` is where the first character after the opening quote is. Empty: an exception.
    JSValue scanString(size_t end, size_t& next)
    {
        size_t begin = end - 1;
        StrBuilder builder;
        bool usesBuilder = false;
        while (true) {
            char32_t c = 0;
            size_t position = end;
            for (; position < m_length; ++position) {
                c = m_characters[position];
                if (c == '"' || c == '\\')
                    break;
                if (c <= 0x1f && m_strict)
                    return fail("Invalid control character at"_s, position);
            }
            if (position == m_length)
                return fail("Unterminated string starting at"_s, begin);
            if (c == '"' && !usesBuilder) {
                next = position + 1;
                return substring(end, position);
            }
            usesBuilder = true;
            if (position != end)
                builder.append(m_characters.subspan(end, position - end));
            ++position;
            if (c == '"') {
                end = position;
                break;
            }
            if (position == m_length)
                return fail("Unterminated string starting at"_s, begin);
            c = m_characters[position];
            if (c != 'u') {
                end = position + 1;
                switch (c) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                default:
                    return fail("Invalid \\escape"_s, end - 2);
                }
            } else {
                ++position;
                end = position + 4;
                if (end > m_length || !hexDigits(position, c))
                    return fail("Invalid \\uXXXX escape"_s, position - 1);
                // The other half of a pair? (There has to be something after it: at least the closing quote.)
                if (U16_IS_LEAD(c) && hasCharactersAfter(end, 6) && m_characters[end] == '\\' && m_characters[end + 1] == 'u') {
                    char32_t c2;
                    if (!hexDigits(end + 2, c2))
                        return fail("Invalid \\uXXXX escape"_s, end + 1);
                    if (U16_IS_TRAIL(c2)) {
                        c = U16_GET_SUPPLEMENTARY(c, c2);
                        end += 6;
                    }
                }
            }
            appendCodePoint(builder, c);
        }
        next = end;
        return makeStr(m_vm, builder.toString());
    }

    // One JSON value, which starts at `index`. Empty: an exception; StopIteration(index) if there is no value there.
    JSValue scanOnce(size_t index, size_t& next)
    {
        if (index >= m_length)
            return stop(index);
        switch (m_characters[index]) {
        case '"':
            return scanString(index + 1, next);
        case '{':
            if (!m_vm.isSafeToRecurseSoft()) [[unlikely]]
                return overflow("maximum recursion depth exceeded while decoding a JSON object from a unicode string"_s);
            return parseObject(index + 1, next);
        case '[':
            if (!m_vm.isSafeToRecurseSoft()) [[unlikely]]
                return overflow("maximum recursion depth exceeded while decoding a JSON array from a unicode string"_s);
            return parseArray(index + 1, next);
        case 'n':
            if (matches(index, "null"_s)) {
                next = index + 4;
                return jsUndefined();
            }
            break;
        case 't':
            if (matches(index, "true"_s)) {
                next = index + 4;
                return jsBoolean(true);
            }
            break;
        case 'f':
            if (matches(index, "false"_s)) {
                next = index + 5;
                return jsBoolean(false);
            }
            break;
        case 'N':
            if (matches(index, "NaN"_s))
                return parseConstant("NaN"_s, index, next);
            break;
        case 'I':
            if (matches(index, "Infinity"_s))
                return parseConstant("Infinity"_s, index, next);
            break;
        case '-':
            if (matches(index, "-Infinity"_s))
                return parseConstant("-Infinity"_s, index, next);
            break;
        default:
            break;
        }
        return matchNumber(index, next);
    }

private:
    NEVER_INLINE JSValue fail(ASCIILiteral message, size_t offset)
    {
        // scanstring(s, 0) takes the opening quote to be before the start.
        jsonRaiseDecodeError(m_globalObject, message, m_document, offset == std::numeric_limits<size_t>::max() ? -1 : static_cast<int64_t>(m_view.indexOfUnitOffset(offset)));
        return { };
    }

    NEVER_INLINE JSValue stop(size_t offset)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        RELEASE_AND_RETURN(scope, raiseWithArgs(m_globalObject, scope, runtime(m_globalObject).StopIteration(), { jsNumber(m_view.indexOfUnitOffset(offset)) }));
    }

    NEVER_INLINE JSValue overflow(ASCIILiteral message)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        RELEASE_AND_RETURN(scope, raiseRecursionError(m_globalObject, scope, message));
    }

    JSValue substring(size_t start, size_t end)
    {
        if (start == end)
            return runtime(m_globalObject).names.empty();
        return makeStr(m_vm, String(m_characters.subspan(start, end - start)));
    }

    bool matches(size_t index, ASCIILiteral word) const
    {
        if (m_length - index < word.length())
            return false;
        for (size_t i = 1; i < word.length(); ++i) {
            if (m_characters[index + i] != word[i])
                return false;
        }
        return true;
    }

    // Four of them, which are there.
    bool hexDigits(size_t position, char32_t& value) const
    {
        value = 0;
        for (size_t i = position; i < position + 4; ++i) {
            if (!isASCIIHexDigit(m_characters[i]))
                return false;
            value = (value << 4) | toASCIIHexValue(m_characters[i]);
        }
        return true;
    }

    // More than `count` characters, not code units, from `offset` on.
    bool hasCharactersAfter(size_t offset, size_t count) const
    {
        if (m_view.isSimple())
            return m_length - offset > count;
        size_t found = 0;
        for (size_t i = offset; i < m_length && found <= count; ++i, ++found) {
            if (U16_IS_LEAD(m_characters[i]) && i + 1 < m_length && U16_IS_TRAIL(m_characters[i + 1]))
                ++i;
        }
        return found > count;
    }

    ALWAYS_INLINE void skipWhitespace(size_t& index) const
    {
        while (index < m_length) {
            auto c = m_characters[index];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++index;
        }
    }

    ALWAYS_INLINE bool isAt(size_t index, char c) const { return index < m_length && m_characters[index] == c; }
    ALWAYS_INLINE bool isDigitAt(size_t index) const { return index < m_length && isASCIIDigit(m_characters[index]); }

    JSValue hook(JsonScannerField field) const { return m_scanner->get(field); }

    // `index` is where the first character after the brace is.
    JSValue parseObject(size_t index, size_t& next)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue pairsHook = hook(JsonScannerObjectPairsHook);
        bool hasPairsHook = !isNone(pairsHook);
        JSArray* pairs = hasPairsHook ? newList(m_globalObject) : nullptr;
        PyDict* dict = hasPairsHook ? nullptr : PyDict::create(m_globalObject);

        skipWhitespace(index);
        if (!isAt(index, '}')) {
            while (true) {
                if (!isAt(index, '"'))
                    RELEASE_AND_RETURN(scope, fail("Expecting property name enclosed in double quotes"_s, index));
                JSValue key = scanString(index + 1, index);
                RETURN_IF_EXCEPTION(scope, { });
                // Equal keys in one document are one object.
                if (!m_memo)
                    m_memo = PyDict::create(m_globalObject);
                key = m_memo->setDefault(m_globalObject, key, key);
                RETURN_IF_EXCEPTION(scope, { });

                skipWhitespace(index);
                if (!isAt(index, ':'))
                    RELEASE_AND_RETURN(scope, fail("Expecting ':' delimiter"_s, index));
                ++index;
                skipWhitespace(index);

                JSValue value = scanOnce(index, index);
                RETURN_IF_EXCEPTION(scope, { });
                if (hasPairsHook)
                    listAppend(m_globalObject, pairs, PyTuple::create(m_globalObject, { key, value }));
                else
                    dict->set(m_globalObject, key, value);
                RETURN_IF_EXCEPTION(scope, { });

                skipWhitespace(index);
                if (isAt(index, '}'))
                    break;
                if (!isAt(index, ','))
                    RELEASE_AND_RETURN(scope, fail("Expecting ',' delimiter"_s, index));
                size_t comma = index++;
                skipWhitespace(index);
                if (isAt(index, '}'))
                    RELEASE_AND_RETURN(scope, fail("Illegal trailing comma before end of object"_s, comma));
            }
        }
        next = index + 1;
        if (hasPairsHook)
            RELEASE_AND_RETURN(scope, call(m_globalObject, pairsHook, pairs));
        JSValue objectHook = hook(JsonScannerObjectHook);
        if (!isNone(objectHook))
            RELEASE_AND_RETURN(scope, call(m_globalObject, objectHook, dict));
        return dict;
    }

    JSValue parseArray(size_t index, size_t& next)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSArray* list = newList(m_globalObject);
        skipWhitespace(index);
        if (!isAt(index, ']')) {
            while (true) {
                JSValue value = scanOnce(index, index);
                RETURN_IF_EXCEPTION(scope, { });
                listAppend(m_globalObject, list, value);
                RETURN_IF_EXCEPTION(scope, { });
                skipWhitespace(index);
                if (isAt(index, ']'))
                    break;
                if (!isAt(index, ','))
                    RELEASE_AND_RETURN(scope, fail("Expecting ',' delimiter"_s, index));
                size_t comma = index++;
                skipWhitespace(index);
                if (isAt(index, ']'))
                    RELEASE_AND_RETURN(scope, fail("Illegal trailing comma before end of array"_s, comma));
            }
        }
        next = index + 1;
        JSValue arrayHook = hook(JsonScannerArrayHook);
        if (!isNone(arrayHook))
            RELEASE_AND_RETURN(scope, call(m_globalObject, arrayHook, list));
        return list;
    }

    JSValue parseConstant(ASCIILiteral constant, size_t index, size_t& next)
    {
        next = index + constant.length();
        return call(m_globalObject, hook(JsonScannerParseConstant), internString(m_vm, constant));
    }

    JSValue matchNumber(size_t start, size_t& next)
    {
        Runtime& rt = runtime(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        size_t index = start;
        bool isFloat = false;

        if (m_characters[index] == '-') {
            if (++index == m_length)
                RELEASE_AND_RETURN(scope, stop(start));
        }
        if (m_characters[index] >= '1' && m_characters[index] <= '9') {
            ++index;
            while (isDigitAt(index))
                ++index;
        } else if (m_characters[index] == '0')
            ++index;
        else
            RELEASE_AND_RETURN(scope, stop(start));

        if (isAt(index, '.') && isDigitAt(index + 1)) {
            isFloat = true;
            index += 2;
            while (isDigitAt(index))
                ++index;
        }
        // An exponent, unless it turns out to have no digits.
        if (index + 1 < m_length && (m_characters[index] == 'e' || m_characters[index] == 'E')) {
            size_t exponent = index++;
            if (index + 1 < m_length && (m_characters[index] == '-' || m_characters[index] == '+'))
                ++index;
            while (isDigitAt(index))
                ++index;
            if (isASCIIDigit(m_characters[index - 1]))
                isFloat = true;
            else
                index = exponent;
        }

        next = index;
        auto text = m_characters.subspan(start, index - start);
        JSValue custom = hook(isFloat ? JsonScannerParseFloat : JsonScannerParseInt);
        if (custom != JSValue(isFloat ? rt.floatType() : rt.intType()))
            RELEASE_AND_RETURN(scope, call(m_globalObject, custom, makeStr(m_vm, String(text))));
        if (isFloat) {
            auto value = floatFromText(StringView(text));
            ASSERT(value);
            RELEASE_AND_RETURN(scope, makeFloat(m_globalObject, value.value_or(0)));
        }
        if (text.size() <= 15) {
            bool isNegative = text[0] == '-';
            int64_t value = 0;
            for (size_t i = isNegative; i < text.size(); ++i)
                value = value * 10 + (text[i] - '0');
            return jsNumber(static_cast<double>(isNegative ? -value : value) + 0.0);
        }
        RELEASE_AND_RETURN(scope, intFromText(m_globalObject, StringView(text), 10));
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    PyRecord* m_scanner;
    JSString* m_document;
    const StrView& m_view;
    std::span<const CharacterType> m_characters;
    size_t m_length;
    PyDict* m_memo { nullptr };
    bool m_strict;
};

// (value, index)
static JSValue jsonResult(JSGlobalObject* globalObject, const StrView& view, JSValue value, size_t next)
{
    return PyTuple::create(globalObject, { value, jsNumber(view.indexOfUnitOffset(next)) });
}

static JSValue jsonScanString(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "_json.scanstring"_s, args)) || failed(scope, checkPositional(globalObject, "scanstring"_s, args, 2, 3)))
        return { };
    if (!isStr(args[0]))
        return raiseTypeError(globalObject, scope, "scanstring() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[0]));
    int64_t end;
    if (failed(scope, ssizeArgument(globalObject, args[1], end)))
        return { };
    bool strict = args.size() < 3 || isTrue(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });

    JSString* document = strValue(args[0]);
    StrView view(globalObject, document);
    RETURN_IF_EXCEPTION(scope, { });
    if (end < 0 || end > static_cast<int64_t>(view.length()))
        return raiseValueError(globalObject, scope, "end is out of bounds"_s);
    size_t offset = view.unitOffset(static_cast<unsigned>(end));
    size_t next = 0;
    JSValue result;
    if (view.is8Bit())
        result = JsonScanner<Latin1Character>(globalObject, nullptr, document, view, view.view().span8(), strict).scanString(offset, next);
    else
        result = JsonScanner<char16_t>(globalObject, nullptr, document, view, view.view().span16(), strict).scanString(offset, next);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, jsonResult(globalObject, view, result, next));
}

static JSValue jsonScannerNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "context"_s };
    JSValue context;
    if (failed(scope, unpackKeywordArguments(globalObject, "make_scanner"_s, keywords, 1, 1, false, args, &context)))
        return { };

    PyRecord* scanner = PyRecord::create(globalObject, PyType::from(cls), JsonScannerFieldCount);
    JSValue strict = getAttr(globalObject, context, runtime(globalObject).names.strict());
    RETURN_IF_EXCEPTION(scope, { });
    scanner->number = isTrue(globalObject, strict);
    RETURN_IF_EXCEPTION(scope, { });
    static constexpr ASCIILiteral attributes[] = { "object_hook"_s, "object_pairs_hook"_s, "array_hook"_s, "parse_float"_s, "parse_int"_s, "parse_constant"_s };
    static_assert(std::size(attributes) == JsonScannerFieldCount);
    for (unsigned i = 0; i < JsonScannerFieldCount; ++i) {
        JSValue value = getAttr(globalObject, context, internString(vm, attributes[i]));
        RETURN_IF_EXCEPTION(scope, { });
        scanner->set(vm, i, value);
    }
    return scanner;
}

static JSValue jsonScannerCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "string"_s, "idx"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "scan_once"_s, keywords, 2, 2, false, args, values)))
        return { };
    int64_t index;
    if (failed(scope, ssizeArgument(globalObject, values[1], index)))
        return { };
    if (!isStr(values[0]))
        return raiseTypeError(globalObject, scope, "first argument must be a string, not "_s, typeName(globalObject, values[0]));

    auto* scanner = PyRecord::from(self);
    JSString* document = strValue(values[0]);
    StrView view(globalObject, document);
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return raiseValueError(globalObject, scope, "idx cannot be negative"_s);
    if (index >= static_cast<int64_t>(view.length()))
        return raiseWithArgs(globalObject, scope, runtime(globalObject).StopIteration(), { makeInt(globalObject, index) });
    size_t offset = view.unitOffset(static_cast<unsigned>(index));
    size_t next = 0;
    JSValue result;
    if (view.is8Bit())
        result = JsonScanner<Latin1Character>(globalObject, scanner, document, view, view.view().span8(), scanner->number).scanOnce(offset, next);
    else
        result = JsonScanner<char16_t>(globalObject, scanner, document, view, view.view().span16(), scanner->number).scanOnce(offset, next);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, jsonResult(globalObject, view, result, next));
}

template<unsigned field>
static JSValue jsonGetField(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(field); }

template<int64_t bit>
static JSValue jsonGetBit(JSGlobalObject*, JSValue self) { return jsBoolean(PyRecord::from(self)->number & bit); }

static constexpr NativeDef jsonScannerMethods[] = {
    PY_NEW(jsonScannerNew, "($type, *args, **kwargs)"),
    PY_SLOT("__call__", jsonScannerCall, "($self, *args, **kwargs)"),
};

static constexpr NativeGetSet jsonScannerGetSets[] = {
    PY_MEMBER("strict", jsonGetBit<1>),
    PY_MEMBER("object_hook", jsonGetField<JsonScannerObjectHook>),
    PY_MEMBER("object_pairs_hook", jsonGetField<JsonScannerObjectPairsHook>),
    PY_MEMBER("array_hook", jsonGetField<JsonScannerArrayHook>),
    PY_MEMBER("parse_float", jsonGetField<JsonScannerParseFloat>),
    PY_MEMBER("parse_int", jsonGetField<JsonScannerParseInt>),
    PY_MEMBER("parse_constant", jsonGetField<JsonScannerParseConstant>),
};

static constexpr TypeDef jsonScannerTypeDef { "_json.Scanner"_s, { }, jsonScannerMethods, jsonScannerGetSets, "JSON scanner object"_s };

// ---- The encoder ----

enum JsonEncoderField : unsigned {
    JsonEncoderMarkers,
    JsonEncoderDefault,
    JsonEncoderEncoder,
    JsonEncoderIndent,
    JsonEncoderKeySeparator,
    JsonEncoderItemSeparator,
    JsonEncoderFieldCount
};
enum JsonEncoderBit : int64_t {
    JsonEncoderSortKeys = 1,
    JsonEncoderSkipKeys = 2,
    JsonEncoderAllowNaN = 4,
    // `encoder` is one of the two functions of this module, which need not be called.
    JsonEncoderIsBaseStringASCII = 8,
    JsonEncoderIsBaseString = 16,
};

static JSValue jsonEncoderNew(JSGlobalObject*, JSValue cls, const CallArgs&);

// Adds a note to the exception that is propagating. `makeNote` may raise, and then returns a null String.
template<typename Function>
static NEVER_INLINE void jsonAddNote(JSGlobalObject* globalObject, const Function& makeNote)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto add = [&] {
        String note = makeNote();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue notes = getAttrIfPresent(globalObject, exception, rt.names.dNotes());
        RETURN_IF_EXCEPTION(scope, void());
        if (!notes) {
            notes = newList(globalObject);
            setAttr(globalObject, exception, rt.names.dNotes(), notes);
            RETURN_IF_EXCEPTION(scope, void());
        } else if (!isList(notes)) {
            raiseTypeError(globalObject, scope, "Cannot add note: __notes__ is not a list"_s);
            return;
        }
        listAppend(globalObject, asList(notes), makeStr(vm, note));
    };
    add();
    if (scope.exception()) {
        if (JSValue failure = pendingException(globalObject); failure && PyException::is(failure))
            PyException::from(failure)->setContext(vm, exception);
        return;
    }
    throwPythonException(globalObject, scope, exception);
}

class JsonEncoder {
public:
    JsonEncoder(JSGlobalObject* globalObject, PyRecord* encoder)
        : m_globalObject(globalObject)
        , m_vm(getVM(globalObject))
        , m_encoder(encoder)
        , m_flags(encoder->number)
        , m_markers(PyDict::tryFrom(encoder->get(JsonEncoderMarkers)))
        , m_hasIndent(!isNone(encoder->get(JsonEncoderIndent)))
    {
    }

    // '\n' + indent * level. False: an exception.
    bool startIndent(int64_t level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_hasIndent)
            return true;
        String first = "\n"_s;
        if (level) {
            JSValue indent = m_encoder->get(JsonEncoderIndent);
            if (!typeOf(m_globalObject, indent)->lookup(Special::GetItem)) {
                raiseTypeError(m_globalObject, scope, '\'', typeName(m_globalObject, indent), "' object can't be repeated"_s);
                return false;
            }
            JSValue repeated = multiply(indent, level);
            RETURN_IF_EXCEPTION(scope, false);
            if (!isStr(repeated)) {
                raiseTypeError(m_globalObject, scope, "can only concatenate str (not \""_s, typeName(m_globalObject, repeated), "\") to str"_s);
                return false;
            }
            first = makeString(first, StringView(strValue(repeated)->value(m_globalObject)));
            RETURN_IF_EXCEPTION(scope, false);
        }
        m_newlineIndents.append(WTF::move(first));
        m_separatorIndents.append(String());
        return true;
    }

    // False: an exception.
    bool encode(JSValue object, unsigned level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isNone(object)) {
            m_out.append("null"_s);
            return true;
        }
        if (object.isBoolean()) {
            m_out.append(object.asBoolean() ? "true"_s : "false"_s);
            return true;
        }
        if (isStr(object))
            RELEASE_AND_RETURN(scope, writeString(object));
        if (isInt(object)) {
            m_out.append(intToString(m_globalObject, intValue(object)));
            return !scope.exception();
        }
        if (isFloat(object)) {
            String text = encodeFloat(object);
            RETURN_IF_EXCEPTION(scope, false);
            m_out.append(text);
            return true;
        }
        if (!m_vm.isSafeToRecurseSoft()) [[unlikely]] {
            raiseRecursionError(m_globalObject, scope, "maximum recursion depth exceeded while encoding a JSON object"_s);
            return false;
        }
        if (isList(object) || PyTuple::is(object))
            RELEASE_AND_RETURN(scope, encodeSequence(object, level));
        if (auto* dict = PyDict::tryFrom(object))
            RELEASE_AND_RETURN(scope, encodeDict(dict, level));

        JSValue identity;
        if (!enter(object, identity))
            return false;
        JSValue replacement = call(m_globalObject, m_encoder->get(JsonEncoderDefault), object);
        RETURN_IF_EXCEPTION(scope, false);
        if (!encode(replacement, level)) {
            scope.release();
            jsonAddNote(m_globalObject, [&] {
                return makeString("when serializing "_s, typeNameForMessage(m_globalObject, typeOf(m_globalObject, object)), " object"_s);
            });
            return false;
        }
        RELEASE_AND_RETURN(scope, leave(identity));
    }

    JSValue finish()
    {
        JSString* text = makeStr(m_globalObject, m_out);
        if (!text)
            return { };
        return PyTuple::create(m_globalObject, { text });
    }

private:
    JSValue multiply(JSValue sequence, int64_t count)
    {
        return callMethod(m_globalObject, sequence, runtime(m_globalObject).name(Special::Mul), makeInt(m_globalObject, count));
    }

    // Notes that the object is being written, which it must not be already.
    bool enter(JSValue object, JSValue& identity)
    {
        if (!m_markers)
            return true;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        identity = identityOf(m_globalObject, object);
        bool isThere = m_markers->contains(m_globalObject, identity);
        RETURN_IF_EXCEPTION(scope, false);
        if (isThere) {
            raiseValueError(m_globalObject, scope, "Circular reference detected"_s);
            return false;
        }
        m_markers->set(m_globalObject, identity, object);
        return !scope.exception();
    }

    bool leave(JSValue identity)
    {
        if (!m_markers)
            return true;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        bool removed = m_markers->remove(m_globalObject, identity);
        RETURN_IF_EXCEPTION(scope, false);
        if (!removed) {
            raiseKeyError(m_globalObject, scope, identity);
            return false;
        }
        return true;
    }

    // As PyUnicodeWriter_WriteStr() does it: what is not a str itself is asked for its str().
    bool write(JSValue string)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSString* text = isExactStr(string) ? asString(string) : str(m_globalObject, string);
        RETURN_IF_EXCEPTION(scope, false);
        m_out.append(StringView(text->value(m_globalObject)));
        return !scope.exception();
    }

    bool writeString(JSValue string)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_flags & (JsonEncoderIsBaseStringASCII | JsonEncoderIsBaseString)) {
            String text = strValue(string)->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (m_flags & JsonEncoderIsBaseStringASCII)
                jsonAppendQuoted<true>(m_out, text);
            else
                jsonAppendQuoted<false>(m_out, text);
            return true;
        }
        JSValue encoded = call(m_globalObject, m_encoder->get(JsonEncoderEncoder), string);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isStr(encoded)) {
            raiseTypeError(m_globalObject, scope, "encoder() must return a string, not "_s, typeName(m_globalObject, encoded));
            return false;
        }
        RELEASE_AND_RETURN(scope, write(encoded));
    }

    // Null: an exception.
    String encodeFloat(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        double value = floatValue(object);
        if (std::isfinite(value))
            return floatRepr(value);
        if (!(m_flags & JsonEncoderAllowNaN)) {
            JSString* text = repr(m_globalObject, object);
            RETURN_IF_EXCEPTION(scope, String());
            raiseValueError(m_globalObject, scope, "Out of range float values are not JSON compliant: "_s, StringView(text->value(m_globalObject)));
            return String();
        }
        return value > 0 ? "Infinity"_s : value < 0 ? "-Infinity"_s : "NaN"_s;
    }

    // What goes between the items at the level. False: an exception.
    bool prepareLevel(unsigned level)
    {
        if (level < m_newlineIndents.size())
            return true;
        ASSERT(level == m_newlineIndents.size());
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue indent = m_encoder->get(JsonEncoderIndent);
        if (!isStr(indent)) {
            raiseTypeError(m_globalObject, scope, "can only concatenate str (not \""_s, typeName(m_globalObject, indent), "\") to str"_s);
            return false;
        }
        String indentText = strValue(indent)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        String separator = strValue(m_encoder->get(JsonEncoderItemSeparator))->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        String newlineIndent = makeString(m_newlineIndents.last(), indentText);
        m_separatorIndents.append(makeString(separator, newlineIndent));
        m_newlineIndents.append(WTF::move(newlineIndent));
        return true;
    }

    bool writeSeparator(unsigned level)
    {
        if (m_hasIndent) {
            m_out.append(m_separatorIndents[level]);
            return true;
        }
        return write(m_encoder->get(JsonEncoderItemSeparator));
    }

    bool encodeKeyAndValue(PyDict* dict, JSValue key, JSValue value, unsigned level, bool& isFirst)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue keyString;
        if (isStr(key))
            keyString = key;
        else if (isFloat(key)) {
            String text = encodeFloat(key);
            RETURN_IF_EXCEPTION(scope, false);
            keyString = makeStr(m_vm, text);
        } else if (key.isBoolean())
            keyString = makeStr(m_vm, key.asBoolean() ? "true"_s : "false"_s);
        else if (isNone(key))
            keyString = makeStr(m_vm, "null"_s);
        else if (isInt(key)) {
            String text = intToString(m_globalObject, intValue(key));
            RETURN_IF_EXCEPTION(scope, false);
            keyString = makeStr(m_vm, text);
        } else if (m_flags & JsonEncoderSkipKeys)
            return true;
        else {
            raiseTypeError(m_globalObject, scope, "keys must be str, int, float, bool or None, not "_s, typeName(m_globalObject, key));
            return false;
        }

        if (isFirst) {
            isFirst = false;
            if (m_hasIndent)
                m_out.append(m_newlineIndents[level]);
        } else {
            writeSeparator(level);
            RETURN_IF_EXCEPTION(scope, false);
        }
        writeString(keyString);
        RETURN_IF_EXCEPTION(scope, false);
        write(m_encoder->get(JsonEncoderKeySeparator));
        RETURN_IF_EXCEPTION(scope, false);
        if (!encode(value, level)) {
            scope.release();
            jsonAddNote(m_globalObject, [&]() -> String {
                auto noteScope = DECLARE_THROW_SCOPE(m_vm);
                JSString* keyRepr = repr(m_globalObject, key);
                RETURN_IF_EXCEPTION(noteScope, String());
                return makeString("when serializing "_s, typeNameForMessage(m_globalObject, dict->type()), " item "_s, StringView(keyRepr->value(m_globalObject)));
            });
            return false;
        }
        return true;
    }

    bool encodeDict(PyDict* dict, unsigned level)
    {
        Runtime& rt = runtime(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (dict->isEmpty()) {
            m_out.append("{}"_s);
            return true;
        }
        JSValue identity;
        if (!enter(dict, identity))
            return false;
        m_out.append('{');
        if (m_hasIndent) {
            ++level;
            if (failed(scope, prepareLevel(level)))
                return false;
        }

        bool isFirst = true;
        bool isExact = dict->type() == rt.dictType() || dict->type() == rt.frozendictType();
        if ((m_flags & JsonEncoderSortKeys) || !isExact) {
            JSArray* items = itemsOf(dict, isExact);
            RETURN_IF_EXCEPTION(scope, false);
            if (m_flags & JsonEncoderSortKeys) {
                listSort(m_globalObject, items, { }, false);
                RETURN_IF_EXCEPTION(scope, false);
            }
            for (unsigned i = 0; i < listLength(items); ++i) {
                auto* item = PyTuple::tryFrom(listGet(items, i));
                if (!item || item->length() != 2) {
                    raiseValueError(m_globalObject, scope, "items must return 2-tuples"_s);
                    return false;
                }
                encodeKeyAndValue(dict, item->at(0), item->at(1), level, isFirst);
                RETURN_IF_EXCEPTION(scope, false);
            }
        } else {
            JSValue key, value;
            for (unsigned position = 0; dict->next(position, key, value);) {
                encodeKeyAndValue(dict, key, value, level, isFirst);
                RETURN_IF_EXCEPTION(scope, false);
            }
        }

        if (failed(scope, leave(identity)))
            return false;
        if (m_hasIndent && !isFirst)
            m_out.append(m_newlineIndents[level - 1]);
        m_out.append('}');
        return true;
    }

    // list(dict.items())
    JSArray* itemsOf(PyDict* dict, bool isExact)
    {
        Runtime& rt = runtime(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isExact) {
            JSArray* items = newList(m_globalObject);
            JSValue key, value;
            for (unsigned position = 0; dict->next(position, key, value);) {
                listAppend(m_globalObject, items, PyTuple::create(m_globalObject, { key, value }));
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
            return items;
        }
        JSValue view = callMethod(m_globalObject, dict, rt.names.items());
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSValue iterator = getIter(m_globalObject, view);
        if (scope.exception()) {
            if (clearPendingExceptionIfMatches(m_globalObject, rt.TypeError()))
                raiseTypeError(m_globalObject, scope, typeName(m_globalObject, dict), ".items() must return an iterable, not "_s, typeName(m_globalObject, view));
            return nullptr;
        }
        RELEASE_AND_RETURN(scope, listFromIterable(m_globalObject, iterator));
    }

    bool encodeSequence(JSValue sequence, unsigned level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSArray* list = tryAsList(sequence);
        PyTuple* tuple = list ? nullptr : PyTuple::from(sequence);
        auto length = [&] { return list ? listLength(list) : tuple->length(); };
        if (!length()) {
            m_out.append("[]"_s);
            return true;
        }
        JSValue identity;
        if (!enter(sequence, identity))
            return false;
        m_out.append('[');
        if (m_hasIndent) {
            ++level;
            if (failed(scope, prepareLevel(level)))
                return false;
            m_out.append(m_newlineIndents[level]);
        }
        for (unsigned i = 0; i < length(); ++i) {
            if (i) {
                writeSeparator(level);
                RETURN_IF_EXCEPTION(scope, false);
            }
            if (!encode(list ? listGet(list, i) : tuple->at(i), level)) {
                scope.release();
                jsonAddNote(m_globalObject, [&] {
                    return makeString("when serializing "_s, typeNameForMessage(m_globalObject, typeOf(m_globalObject, sequence)), " item "_s, i);
                });
                return false;
            }
        }
        if (failed(scope, leave(identity)))
            return false;
        if (m_hasIndent)
            m_out.append(m_newlineIndents[level - 1]);
        m_out.append(']');
        return true;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    PyRecord* m_encoder;
    int64_t m_flags;
    PyDict* m_markers;
    bool m_hasIndent;
    StrBuilder m_out;
    // By level: what is written after an opening and before a closing bracket, and what between two items.
    Vector<String, 8> m_newlineIndents;
    Vector<String, 8> m_separatorIndents;
};

static JSValue jsonEncoderCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "obj"_s, "_current_indent_level"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "_iterencode"_s, keywords, 2, 2, false, args, values)))
        return { };
    int64_t level;
    if (failed(scope, ssizeArgument(globalObject, values[1], level)))
        return { };
    JsonEncoder encoder(globalObject, PyRecord::from(self));
    if (failed(scope, encoder.startIndent(level)))
        return { };
    encoder.encode(values[0], 0);
    RETURN_IF_EXCEPTION(scope, { });
    return encoder.finish();
}

static constexpr NativeDef jsonEncoderMethods[] = {
    PY_NEW(jsonEncoderNew, "($type, *args, **kwargs)"),
    PY_SLOT("__call__", jsonEncoderCall, "($self, *args, **kwargs)"),
};

static constexpr NativeGetSet jsonEncoderGetSets[] = {
    PY_MEMBER("markers", jsonGetField<JsonEncoderMarkers>),
    PY_MEMBER("default", jsonGetField<JsonEncoderDefault>),
    PY_MEMBER("encoder", jsonGetField<JsonEncoderEncoder>),
    PY_MEMBER("indent", jsonGetField<JsonEncoderIndent>),
    PY_MEMBER("key_separator", jsonGetField<JsonEncoderKeySeparator>),
    PY_MEMBER("item_separator", jsonGetField<JsonEncoderItemSeparator>),
    PY_MEMBER("sort_keys", jsonGetBit<JsonEncoderSortKeys>),
    PY_MEMBER("skipkeys", jsonGetBit<JsonEncoderSkipKeys>),
};

static constexpr TypeDef jsonEncoderTypeDef { "_json.Encoder"_s, { }, jsonEncoderMethods, jsonEncoderGetSets, "Encoder(markers, default, encoder, indent, key_separator, item_separator, sort_keys, skipkeys, allow_nan)"_s };

static constexpr NativeDef jsonFunctions[] = {
    PY_FUNCTION("encode_basestring_ascii", jsonEncodeBaseString<true>, "(pystr, /)"),
    PY_FUNCTION("encode_basestring", jsonEncodeBaseString<false>, "(pystr, /)"),
    PY_FUNCTION("scanstring", jsonScanString, "!(pystr, end, strict=True, /)"),
};

static JSValue jsonEncoderNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = {
        "markers"_s, "default"_s, "encoder"_s, "indent"_s, "key_separator"_s, "item_separator"_s, "sort_keys"_s, "skipkeys"_s, "allow_nan"_s,
    };
    JSValue values[9];
    if (failed(scope, unpackKeywordArguments(globalObject, "make_encoder"_s, keywords, 9, 9, false, args, values)))
        return { };
    for (unsigned i = 4; i < 6; ++i) {
        if (!isStr(values[i]))
            return raiseTypeError(globalObject, scope, "make_encoder() argument "_s, i + 1, " must be str, not "_s, typeNameOfArgument(globalObject, values[i]));
    }
    int64_t flags = 0;
    for (unsigned i = 6; i < 9; ++i) {
        bool flag = isTrue(globalObject, values[i]);
        RETURN_IF_EXCEPTION(scope, { });
        if (flag)
            flags |= 1 << (i - 6);
    }
    if (!isNone(values[0]) && !isDict(values[0]))
        return raiseTypeError(globalObject, scope, "make_encoder() argument 1 must be dict or None, not "_s, typeName(globalObject, values[0]));
    if (auto* function = PyNativeFunction::tryFrom(values[2])) {
        if (&function->def() == &jsonFunctions[0])
            flags |= JsonEncoderIsBaseStringASCII;
        else if (&function->def() == &jsonFunctions[1])
            flags |= JsonEncoderIsBaseString;
    }
    PyRecord* encoder = PyRecord::create(globalObject, PyType::from(cls), JsonEncoderFieldCount);
    for (unsigned i = 0; i < JsonEncoderFieldCount; ++i)
        encoder->set(vm, i, values[i]);
    encoder->number = flags;
    return encoder;
}

void initializeJsonModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "__doc__"_s, makeStr(rt.vm(), "json speedups\n"_s));
    module->addFunctions(globalObject, jsonFunctions);
    module->add(globalObject, "make_scanner"_s, rt.typeFor(jsonScannerTypeDef));
    module->add(globalObject, "make_encoder"_s, rt.typeFor(jsonEncoderTypeDef));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
