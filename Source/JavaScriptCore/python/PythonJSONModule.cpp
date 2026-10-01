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

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonCodecs.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonText.h"
#include <wtf/dtoa.h>

// The module _json: Modules/_json.c of CPython, function for function. json does without it, being written in Python besides, and is a good deal quicker with it.

namespace JSC { namespace Python {

namespace {

struct JSONModuleState final : NativeState {
    PYTHON_NATIVE_STATE(JSONModuleState);
    WriteBarrier<PyType> scannerType;
    WriteBarrier<PyType> encoderType;
};

template<typename Visitor>
void JSONModuleState::visit(Visitor& visitor)
{
    visitor.append(scannerType);
    visitor.append(encoderType);
}

JSONModuleState& jsonModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<JSONModuleState>(); }

// ---- Strings, written

constexpr char hexDigits[] = "0123456789abcdef";

// S_CHAR()
constexpr bool isWrittenAsItIs(char16_t c) { return c >= ' ' && c <= '~' && c != '\\' && c != '"'; }

// What comes after the backslash, for those that have a letter. 0 for the rest.
constexpr char letterOfEscape(char16_t c)
{
    switch (c) {
    case '\\':
    case '"':
        return c;
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

void appendUnicodeEscape(TextBuilder& out, char16_t c)
{
    out.append('\\', 'u', hexDigits[(c >> 12) & 0xf], hexDigits[(c >> 8) & 0xf], hexDigits[(c >> 4) & 0xf], hexDigits[c & 0xf]);
}

// ascii_escape_unicode() and escape_unicode(). What is beyond the sixteen bits of an escape is written as the two halves of a pair, which is how it is kept here.
template<bool asciiOnly, typename CharacterType>
void appendEscaped(TextBuilder& out, std::span<const CharacterType> characters)
{
    out.append('"');
    size_t written = 0;
    for (size_t i = 0; i < characters.size(); ++i) {
        char16_t c = characters[i];
        if (asciiOnly ? isWrittenAsItIs(c) : c > 0x1f && c != '\\' && c != '"')
            continue;
        out.append(characters.subspan(written, i - written));
        written = i + 1;
        if (char letter = letterOfEscape(c))
            out.append('\\', letter);
        else
            appendUnicodeEscape(out, c);
    }
    out.append(characters.subspan(written));
    out.append('"');
}

template<bool asciiOnly>
void appendEscaped(TextBuilder& out, const String& text)
{
    if (text.is8Bit())
        appendEscaped<asciiOnly>(out, text.span8());
    else
        appendEscaped<asciiOnly>(out, text.span16());
}

template<bool asciiOnly>
EncodedJSValue encodeBaseString(JSGlobalObject* globalObject, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* string = stringIn(given);
    if (!string)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("first argument must be a string, not "_s, typeName(globalObject, given))));
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    TextBuilder out;
    appendEscaped<asciiOnly>(out, text);
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, out.tryFinish())));
}

// encode_basestring_ascii(string)
PYTHON_NATIVE(jsonEncodeBaseStringASCII)
{
    return encodeBaseString<true>(globalObject, callFrame->argument(0));
}

// encode_basestring(string)
PYTHON_NATIVE(jsonEncodeBaseString)
{
    return encodeBaseString<false>(globalObject, callFrame->argument(0));
}

// ---- Reading

struct Scanner final : NativeState {
    PYTHON_NATIVE_STATE(Scanner);
    bool strict { true };
    WriteBarrier<Unknown> objectHook;
    WriteBarrier<Unknown> objectPairsHook;
    WriteBarrier<Unknown> parseFloat;
    WriteBarrier<Unknown> parseInt;
    WriteBarrier<Unknown> parseConstant;
};

template<typename Visitor>
void Scanner::visit(Visitor& visitor)
{
    visitor.append(objectHook);
    visitor.append(objectPairsHook);
    visitor.append(parseFloat);
    visitor.append(parseInt);
    visitor.append(parseConstant);
}

// raise_errmsg()
void raiseDecodeError(JSGlobalObject* globalObject, ASCIILiteral message, JSValue string, int64_t end)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue type = importModuleAttribute(globalObject, "json.decoder"_s, "JSONDecodeError"_s);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue exception = call(globalObject, type, jsNontrivialString(vm, message), string, intFromInt64(globalObject, end));
    RETURN_IF_EXCEPTION(scope, void());
    raiseObject(globalObject, scope, exception);
}

// Where things are is counted in characters, as Python counts. So what is gone through is the characters: those of the string as it is kept, unless there are surrogate pairs in it, and then they have been
// written out one to a place (CodePoints). Each function below is the one of the same name, or near it, in _json.c.
template<typename CharacterType>
class Reader {
public:
    Reader(JSGlobalObject* globalObject, JSValue string, std::span<const CharacterType> characters)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_string(string)
        , m_characters(characters)
        , m_length(characters.size())
    {
    }

    // scanstring_unicode(). Empty if it raised.
    JSValue scanString(int64_t end, bool strict, int64_t& nextEnd)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t begin = end - 1;
        nextEnd = -1;
        if (end < 0 || m_length < end)
            return raiseValueError(m_globalObject, scope, "end is out of bounds"_s);
        TextWriter writer;
        bool hasWritten = false;
        while (true) {
            // The end of the string, or the next escape
            char32_t c = 0;
            int64_t next = end;
            for (; next < m_length; ++next) {
                c = m_characters[next];
                if (c == '"' || c == '\\')
                    break;
                if (c <= 0x1f && strict)
                    return fail("Invalid control character at"_s, next);
            }
            if (next == m_length)
                c = next > end ? m_characters[next - 1] : 0;
            bool isAtQuote = next < m_length && c == '"';
            if (isAtQuote) {
                if (!hasWritten) {
                    nextEnd = next + 1;
                    RELEASE_AND_RETURN(scope, substring(end, next));
                }
            } else if (next == m_length || c != '\\')
                return fail("Unterminated string starting at"_s, begin);

            for (int64_t i = end; i < next; ++i)
                writer.append(static_cast<char32_t>(m_characters[i]));
            hasWritten = true;
            ++next;
            if (isAtQuote) {
                end = next;
                break;
            }
            if (next == m_length)
                return fail("Unterminated string starting at"_s, begin);
            c = m_characters[next];
            if (c != 'u') {
                end = next + 1;
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
                ++next;
                end = next + 4;
                if (end > m_length)
                    return fail("Invalid \\uXXXX escape"_s, next - 1);
                if (!readFourHexDigits(next, c))
                    return fail("Invalid \\uXXXX escape"_s, end - 5);
                next = end;
                // A surrogate pair
                if (U16_IS_LEAD(c) && end + 6 < m_length && m_characters[next] == '\\' && m_characters[next + 1] == 'u') {
                    char32_t second;
                    end += 6;
                    if (!readFourHexDigits(next + 2, second))
                        return fail("Invalid \\uXXXX escape"_s, end - 5);
                    if (U16_IS_TRAIL(second))
                        c = U16_GET_SUPPLEMENTARY(c, second);
                    else
                        end -= 6;
                }
            }
            writer.append(c);
        }
        nextEnd = end;
        String text = writer.finish(m_globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return jsString(m_vm, text);
    }

    // scan_once_unicode(). Empty if it raised.
    JSValue scanOnce(Scanner& scanner, PyDict* memo, int64_t index, int64_t& nextIndex)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (index < 0)
            return raiseValueError(m_globalObject, scope, "idx cannot be negative"_s);
        if (index >= m_length)
            return stop(index);

        auto follows = [&] (ASCIILiteral rest) {
            auto expected = rest.span8();
            if (index + static_cast<int64_t>(expected.size()) >= m_length)
                return false;
            for (size_t i = 0; i < expected.size(); ++i) {
                if (m_characters[index + 1 + i] != expected[i])
                    return false;
            }
            return true;
        };
        switch (m_characters[index]) {
        case '"':
            RELEASE_AND_RETURN(scope, scanString(index + 1, scanner.strict, nextIndex));
        case '{':
            if (!m_vm.isSafeToRecurse()) [[unlikely]]
                return raise(m_globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded while decoding a JSON object from a unicode string"_s);
            RELEASE_AND_RETURN(scope, parseObject(scanner, memo, index + 1, nextIndex));
        case '[':
            if (!m_vm.isSafeToRecurse()) [[unlikely]]
                return raise(m_globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded while decoding a JSON array from a unicode string"_s);
            RELEASE_AND_RETURN(scope, parseArray(scanner, memo, index + 1, nextIndex));
        case 'n':
            if (follows("ull"_s)) {
                nextIndex = index + 4;
                return jsUndefined();
            }
            break;
        case 't':
            if (follows("rue"_s)) {
                nextIndex = index + 4;
                return jsBoolean(true);
            }
            break;
        case 'f':
            if (follows("alse"_s)) {
                nextIndex = index + 5;
                return jsBoolean(false);
            }
            break;
        case 'N':
            if (follows("aN"_s))
                RELEASE_AND_RETURN(scope, parseConstant(scanner, "NaN"_s, index, nextIndex));
            break;
        case 'I':
            if (follows("nfinity"_s))
                RELEASE_AND_RETURN(scope, parseConstant(scanner, "Infinity"_s, index, nextIndex));
            break;
        case '-':
            if (follows("Infinity"_s))
                RELEASE_AND_RETURN(scope, parseConstant(scanner, "-Infinity"_s, index, nextIndex));
            break;
        }
        RELEASE_AND_RETURN(scope, matchNumber(scanner, index, nextIndex));
    }

private:
    static bool isWhitespace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    JSValue fail(ASCIILiteral message, int64_t end)
    {
        raiseDecodeError(m_globalObject, message, m_string, end);
        return { };
    }

    // raise_stop_iteration()
    JSValue stop(int64_t index)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        return raise(m_globalObject, scope, BuiltinType::StopIteration, intFromInt64(m_globalObject, index));
    }

    bool readFourHexDigits(int64_t from, char32_t& result)
    {
        result = 0;
        for (int64_t i = from; i < from + 4; ++i) {
            char32_t digit = m_characters[i];
            if (digit >= 0x80 || !isASCIIHexDigit(static_cast<char>(digit)))
                return false;
            result = result << 4 | toASCIIHexValue(static_cast<char>(digit));
        }
        return true;
    }

    // A str of its own, that does not keep all of what it was read from.
    JSValue substring(int64_t from, int64_t to)
    {
        auto piece = m_characters.subspan(from, to - from);
        if (piece.empty())
            return jsEmptyString(m_vm);
        if constexpr (std::is_same_v<CharacterType, char32_t>) {
            TextWriter writer;
            for (char32_t c : piece)
                writer.append(c);
            String text = writer.finish(m_globalObject);
            return text.isNull() ? JSValue() : JSValue(jsString(m_vm, text));
        } else if constexpr (std::is_same_v<CharacterType, char16_t>) {
            // As narrow as it can be, as any str is.
            char16_t all = 0;
            for (char16_t c : piece)
                all |= c;
            if (all < 0x100) {
                std::span<Latin1Character> buffer;
                String narrow = String::tryCreateUninitialized(piece.size(), buffer);
                if (!narrow.isNull()) {
                    for (size_t i = 0; i < piece.size(); ++i)
                        buffer[i] = static_cast<Latin1Character>(piece[i]);
                }
                return strOrMemoryError(m_globalObject, narrow);
            }
            return strOrMemoryError(m_globalObject, String(piece));
        } else
            return strOrMemoryError(m_globalObject, String(piece));
    }

    void skipWhitespace(int64_t& index)
    {
        while (index < m_length && isWhitespace(m_characters[index]))
            ++index;
    }

    bool isAt(int64_t index, char c) { return index < m_length && m_characters[index] == static_cast<CharacterType>(c); }

    // _parse_object_unicode()
    JSValue parseObject(Scanner& scanner, PyDict* memo, int64_t index, int64_t& nextIndex)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        bool hasPairsHook = !isNone(scanner.objectPairsHook.get());
        JSArray* pairs = hasPairsHook ? newList(m_globalObject) : nullptr;
        PyDict* dict = hasPairsHook ? nullptr : PyDict::create(m_globalObject);

        skipWhitespace(index);
        if (!isAt(index, '}')) {
            while (true) {
                if (!isAt(index, '"'))
                    return fail("Expecting property name enclosed in double quotes"_s, index);
                int64_t next;
                JSValue key = scanString(index + 1, scanner.strict, next);
                RETURN_IF_EXCEPTION(scope, { });
                // The same key is the one str each time.
                key = memo->getOrAdd(m_globalObject, key, key);
                RETURN_IF_EXCEPTION(scope, { });
                index = next;

                skipWhitespace(index);
                if (!isAt(index, ':'))
                    return fail("Expecting ':' delimiter"_s, index);
                ++index;
                skipWhitespace(index);

                JSValue value = scanOnce(scanner, memo, index, next);
                RETURN_IF_EXCEPTION(scope, { });
                if (hasPairsHook)
                    listAppend(m_globalObject, pairs, PyTuple::create(m_globalObject, { key, value }));
                else
                    dict->set(m_globalObject, key, value);
                RETURN_IF_EXCEPTION(scope, { });
                index = next;

                skipWhitespace(index);
                if (isAt(index, '}'))
                    break;
                if (!isAt(index, ','))
                    return fail("Expecting ',' delimiter"_s, index);
                int64_t comma = index;
                ++index;
                skipWhitespace(index);
                if (isAt(index, '}'))
                    return fail("Illegal trailing comma before end of object"_s, comma);
            }
        }
        nextIndex = index + 1;
        if (hasPairsHook)
            RELEASE_AND_RETURN(scope, call(m_globalObject, scanner.objectPairsHook.get(), pairs));
        if (!isNone(scanner.objectHook.get()))
            RELEASE_AND_RETURN(scope, call(m_globalObject, scanner.objectHook.get(), dict));
        return dict;
    }

    // _parse_array_unicode()
    JSValue parseArray(Scanner& scanner, PyDict* memo, int64_t index, int64_t& nextIndex)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSArray* list = newList(m_globalObject);
        skipWhitespace(index);
        if (!isAt(index, ']')) {
            while (true) {
                int64_t next;
                JSValue value = scanOnce(scanner, memo, index, next);
                RETURN_IF_EXCEPTION(scope, { });
                listAppend(m_globalObject, list, value);
                RETURN_IF_EXCEPTION(scope, { });
                index = next;

                skipWhitespace(index);
                if (isAt(index, ']'))
                    break;
                if (!isAt(index, ','))
                    return fail("Expecting ',' delimiter"_s, index);
                int64_t comma = index;
                ++index;
                skipWhitespace(index);
                if (isAt(index, ']'))
                    return fail("Illegal trailing comma before end of array"_s, comma);
            }
        }
        nextIndex = index + 1;
        return list;
    }

    // _parse_constant()
    JSValue parseConstant(Scanner& scanner, ASCIILiteral constant, int64_t index, int64_t& nextIndex)
    {
        nextIndex = index + constant.length();
        return call(m_globalObject, scanner.parseConstant.get(), jsNontrivialString(m_vm, constant));
    }

    // _match_number_unicode()
    JSValue matchNumber(Scanner& scanner, int64_t start, int64_t& nextIndex)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyRealm* realm = m_globalObject->pyRealm();
        int64_t index = start;
        auto isDigitAt = [&] (int64_t at) { return at < m_length && m_characters[at] >= '0' && m_characters[at] <= '9'; };

        if (m_characters[index] == '-') {
            ++index;
            if (index >= m_length)
                return stop(start);
        }
        if (m_characters[index] >= '1' && m_characters[index] <= '9') {
            ++index;
            while (isDigitAt(index))
                ++index;
        } else if (m_characters[index] == '0')
            ++index;
        else
            return stop(start);

        bool isFloat = false;
        if (isAt(index, '.') && isDigitAt(index + 1)) {
            isFloat = true;
            index += 2;
            while (isDigitAt(index))
                ++index;
        }
        if (index + 1 < m_length && (m_characters[index] == 'e' || m_characters[index] == 'E')) {
            int64_t exponentStart = index;
            ++index;
            if (index + 1 < m_length && (m_characters[index] == '-' || m_characters[index] == '+'))
                ++index;
            while (isDigitAt(index))
                ++index;
            // With a digit it is a float. Without, it is gone back on.
            if (isDigitAt(index - 1))
                isFloat = true;
            else
                index = exponentStart;
        }
        nextIndex = index;

        JSValue custom;
        if (isFloat && scanner.parseFloat.get() != JSValue(realm->typeFloat()->object()))
            custom = scanner.parseFloat.get();
        else if (!isFloat && scanner.parseInt.get() != JSValue(realm->typeInt()->object()))
            custom = scanner.parseInt.get();
        if (custom) {
            JSValue text = substring(start, index);
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, call(m_globalObject, custom, text));
        }

        size_t size = index - start;
        if (!isFloat && size <= 18) {
            bool isNegative = m_characters[start] == '-';
            int64_t value = 0;
            for (int64_t i = start + isNegative; i < index; ++i)
                value = value * 10 + (m_characters[i] - '0');
            return intFromInt64(m_globalObject, isNegative ? -value : value);
        }
        Vector<Latin1Character, 64> digits;
        if (!digits.tryReserveCapacity(size))
            return raiseMemoryError(m_globalObject, scope);
        for (int64_t i = start; i < index; ++i)
            digits.append(static_cast<Latin1Character>(m_characters[i]));
        if (!isFloat)
            RELEASE_AND_RETURN(scope, Python::parseInt(m_globalObject, StringView(digits.span()), 10));
        size_t parsed;
        return floatFromDouble(WTF::parseDouble(digits.span(), parsed));
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    JSValue m_string;
    std::span<const CharacterType> m_characters;
    int64_t m_length;
};

// Calls the function with what reads the string.
template<typename Function>
JSValue withReader(JSGlobalObject* globalObject, JSValue given, const Function& function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = stringIn(given)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints characters(text);
    RELEASE_AND_RETURN(scope, characters.withSpan([&] (auto span) -> JSValue {
        Reader reader(globalObject, given, span);
        return function(reader);
    }));
}

// _build_rval_index_tuple()
JSValue withIndex(JSGlobalObject* globalObject, JSValue value, int64_t index)
{
    return PyTuple::create(globalObject, { value, intFromInt64(globalObject, index) });
}

// scanstring(string, end, strict=True)
PYTHON_NATIVE(jsonScanString)
{
    NATIVE_PROLOGUE();
    auto end = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    bool strict = true;
    if (JSValue given = args.at(2)) {
        strict = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!stringIn(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("first argument must be a string, not "_s, typeName(globalObject, args[0]))));
    int64_t nextEnd = -1;
    JSValue result = withReader(globalObject, args[0], [&] (auto& reader) { return reader.scanString(*end, strict, nextEnd); });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(withIndex(globalObject, result, nextEnd));
}

// scanner_new(): make_scanner(context)
PYTHON_NATIVE(scannerNew)
{
    NATIVE_PROLOGUE();
    JSValue context = args[1];
    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Scanner>());
    auto& self = object->state<Scanner>();
    JSValue strict = getAttribute(globalObject, context, Identifier::fromString(vm, "strict"_s));
    RETURN_IF_EXCEPTION(scope, { });
    self.strict = isTrue(globalObject, strict);
    RETURN_IF_EXCEPTION(scope, { });
    for (auto [slot, name] : { std::pair { &self.objectHook, "object_hook"_s }, std::pair { &self.objectPairsHook, "object_pairs_hook"_s }, std::pair { &self.parseFloat, "parse_float"_s },
        std::pair { &self.parseInt, "parse_int"_s }, std::pair { &self.parseConstant, "parse_constant"_s } }) {
        JSValue value = getAttribute(globalObject, context, Identifier::fromString(vm, name));
        RETURN_IF_EXCEPTION(scope, { });
        slot->set(vm, object, value);
    }
    return JSValue::encode(object);
}

// scanner_call(): scan_once(string, idx)
PYTHON_NATIVE(scannerCall)
{
    NATIVE_PROLOGUE();
    auto index = toSsize(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("first argument must be a string, not "_s, typeName(globalObject, args[1]))));
    auto& self = stateOf<Scanner>(args[0]);
    PyDict* memo = PyDict::create(globalObject);
    int64_t nextIndex = -1;
    JSValue result = withReader(globalObject, args[1], [&] (auto& reader) { return reader.scanOnce(self, memo, *index, nextIndex); });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(withIndex(globalObject, result, nextIndex));
}

// ---- Writing

struct Encoder final : NativeState {
    PYTHON_NATIVE_STATE(Encoder);
    enum class FastEncode : uint8_t { None, ASCII, AsItIs };
    WriteBarrier<Unknown> markers;
    WriteBarrier<Unknown> defaultFunction;
    WriteBarrier<Unknown> encoder;
    WriteBarrier<Unknown> indent;
    WriteBarrier<Unknown> keySeparator;
    WriteBarrier<Unknown> itemSeparator;
    bool sortKeys { false };
    bool skipKeys { false };
    bool allowNaN { false };
    FastEncode fastEncode { FastEncode::None };
};

template<typename Visitor>
void Encoder::visit(Visitor& visitor)
{
    visitor.append(markers);
    visitor.append(defaultFunction);
    visitor.append(encoder);
    visitor.append(indent);
    visitor.append(keySeparator);
    visitor.append(itemSeparator);
}

// encoder_new(): make_encoder(markers, default, encoder, indent, key_separator, item_separator, sort_keys, skipkeys, allow_nan)
PYTHON_NATIVE(encoderNew)
{
    NATIVE_PROLOGUE();
    for (unsigned i : { 5u, 6u }) {
        if (!stringIn(args[i]))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("make_encoder() argument "_s, i, " must be str, not "_s, typeNameOfArgument(globalObject, args[i]))));
    }
    bool flags[3];
    for (unsigned i = 0; i < 3; ++i) {
        flags[i] = isTrue(globalObject, args[7 + i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!isNone(args[1]) && !isDict(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("make_encoder() argument 1 must be dict or None, not "_s, typeName(globalObject, args[1]))));

    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Encoder>());
    auto& self = object->state<Encoder>();
    self.markers.set(vm, object, args[1]);
    self.defaultFunction.set(vm, object, args[2]);
    self.encoder.set(vm, object, args[3]);
    self.indent.set(vm, object, args[4]);
    self.keySeparator.set(vm, object, args[5]);
    self.itemSeparator.set(vm, object, args[6]);
    self.sortKeys = flags[0];
    self.skipKeys = flags[1];
    self.allowNaN = flags[2];
    if (auto* function = dynamicDowncast<PyNativeFunction>(args[3])) {
        if (function->nativeFunction() == TaggedNativeFunction(jsonEncodeBaseStringASCII))
            self.fastEncode = Encoder::FastEncode::ASCII;
        else if (function->nativeFunction() == TaggedNativeFunction(jsonEncodeBaseString))
            self.fastEncode = Encoder::FastEncode::AsItIs;
    }
    return JSValue::encode(object);
}

// One call of an Encoder.
class Writer {
public:
    Writer(JSGlobalObject* globalObject, Encoder& encoder)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_encoder(encoder)
        , m_hasIndent(!isNone(encoder.indent.get()))
    {
    }

    // create_indent_cache(). At 2 * k, what comes after an opening bracket and before a closing one, k deep: a new line and so much indentation. At 2 * k - 1, what comes between items: the separator and then that.
    void createIndentCache(int64_t level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_hasIndent)
            return;
        TextBuilder newlineIndent;
        newlineIndent.append('\n');
        if (level) {
            // PySequence_Repeat(). json gives a str.
            JSString* indent = stringIn(m_encoder.indent.get());
            if (!indent) {
                raiseTypeError(m_globalObject, scope, concatenate('\'', typeName(m_globalObject, m_encoder.indent.get()), "' object can't be repeated"_s));
                return;
            }
            String text = indent->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            for (int64_t i = 0; i < level && !newlineIndent.hasOverflowed() && !text.isEmpty(); ++i)
                newlineIndent.append(text);
        }
        String text = newlineIndent.finish(m_globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        m_indentCache.append(text);
    }

    // encoder_listencode_obj()
    void writeObject(JSValue object, unsigned level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isNone(object)) {
            m_out.append("null"_s);
            return;
        }
        if (object.isBoolean()) {
            m_out.append(object.asBoolean() ? "true"_s : "false"_s);
            return;
        }
        if (stringIn(object))
            RELEASE_AND_RETURN(scope, writeString(object));
        Number number = classify(object);
        if (number.isInt())
            RELEASE_AND_RETURN(scope, writeInt(number));
        if (number.kind == Number::Kind::Float)
            RELEASE_AND_RETURN(scope, writeFloat(object, number));
        bool isSequence = isList(object) || isTuple(object);
        if (isSequence || isDict(object)) {
            checkDepth();
            RETURN_IF_EXCEPTION(scope, void());
            RELEASE_AND_RETURN(scope, isSequence ? writeList(object, level) : writeDict(asDict(object), level));
        }

        JSValue identity;
        enter(object, identity);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue replacement = call(m_globalObject, m_encoder.defaultFunction.get(), object);
        RETURN_IF_EXCEPTION(scope, void());
        checkDepth();
        RETURN_IF_EXCEPTION(scope, void());
        writeObject(replacement, level);
        if (scope.exception()) [[unlikely]] {
            addNoteToRaised(m_globalObject, concatenate("when serializing "_s, fullyQualifiedTypeName(m_globalObject, object), " object"_s));
            return;
        }
        RELEASE_AND_RETURN(scope, leave(identity));
    }

    JSValue finish() { return strOrMemoryError(m_globalObject, m_out.tryFinish()); }

private:
    void checkDepth()
    {
        if (m_vm.isSafeToRecurse()) [[likely]]
            return;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raise(m_globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded while encoding a JSON object"_s);
    }

    // What is being written is kept note of, by its id(), so that what has itself in it is found out.
    void enter(JSValue object, JSValue& identity)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isNone(m_encoder.markers.get()))
            return;
        PyDict* markers = asDict(m_encoder.markers.get());
        identity = intFromInt64(m_globalObject, static_cast<int64_t>(std::bit_cast<uintptr_t>(object.asCell())));
        bool isThere = markers->contains(m_globalObject, identity);
        RETURN_IF_EXCEPTION(scope, void());
        if (isThere) {
            raiseValueError(m_globalObject, scope, "Circular reference detected"_s);
            return;
        }
        markers->set(m_globalObject, identity, object);
        RETURN_IF_EXCEPTION(scope, void());
    }

    void leave(JSValue identity)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!identity)
            return;
        JSValue removed = asDict(m_encoder.markers.get())->remove(m_globalObject, identity);
        RETURN_IF_EXCEPTION(scope, void());
        if (!removed)
            raise(m_globalObject, scope, BuiltinType::KeyError, identity);
    }

    void append(JSValue string)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        String text = stringIn(string)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        m_out.append(text);
    }

    // encoder_encode_string()
    void writeString(JSValue string)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_encoder.fastEncode != Encoder::FastEncode::None) {
            String text = stringIn(string)->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            if (m_encoder.fastEncode == Encoder::FastEncode::ASCII)
                appendEscaped<true>(m_out, text);
            else
                appendEscaped<false>(m_out, text);
            return;
        }
        JSValue encoded = call(m_globalObject, m_encoder.encoder.get(), string);
        RETURN_IF_EXCEPTION(scope, void());
        if (!stringIn(encoded)) {
            raiseTypeError(m_globalObject, scope, concatenate("encoder() must return a string, not "_s, typeName(m_globalObject, encoded)));
            return;
        }
        RELEASE_AND_RETURN(scope, append(encoded));
    }

    // int.__repr__(), whatever the class of it has
    void writeInt(const Number& number)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        String text = reprOfInt(m_globalObject, number);
        RETURN_IF_EXCEPTION(scope, void());
        m_out.append(text);
    }

    // encoder_encode_float(). Null if it raised.
    String textOfFloat(JSValue object, const Number& number)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        double value = number.real;
        if (std::isfinite(value))
            return reprOfDouble(value);
        if (!m_encoder.allowNaN) {
            String shown = repr(m_globalObject, object);
            RETURN_IF_EXCEPTION(scope, { });
            raiseValueError(m_globalObject, scope, concatenate("Out of range float values are not JSON compliant: "_s, shown));
            return { };
        }
        return value > 0 ? "Infinity"_s : value < 0 ? "-Infinity"_s : "NaN"_s;
    }

    void writeFloat(JSValue object, const Number& number)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        String text = textOfFloat(object, number);
        RETURN_IF_EXCEPTION(scope, void());
        m_out.append(text);
    }

    // update_indent_cache() and get_item_separator(). Null if it raised.
    const String* itemSeparator(unsigned level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        ASSERT(level);
        if (level * 2 > m_indentCache.size()) {
            JSString* indent = stringIn(m_encoder.indent.get());
            if (!indent) {
                raiseTypeError(m_globalObject, scope, concatenate("can only concatenate str (not \""_s, typeName(m_globalObject, m_encoder.indent.get()), "\") to str"_s));
                return nullptr;
            }
            String indentText = indent->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            String separator = stringIn(m_encoder.itemSeparator.get())->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
            String newlineIndent = concatenate(m_indentCache[(level - 1) * 2], indentText);
            m_indentCache.append(concatenate(separator, newlineIndent));
            m_indentCache.append(newlineIndent);
        }
        return &m_indentCache[level * 2 - 1];
    }

    // write_newline_indent()
    void writeNewlineIndent(unsigned level) { m_out.append(m_indentCache[level * 2]); }

    // encoder_encode_key_value()
    void writeKeyAndValue(bool& isFirst, PyDict* dict, JSValue key, JSValue value, unsigned level, const String& separator)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue keyString;
        Number number = classify(key);
        if (stringIn(key))
            keyString = key;
        else if (number.kind == Number::Kind::Float) {
            String text = textOfFloat(key, number);
            RETURN_IF_EXCEPTION(scope, void());
            keyString = jsString(m_vm, text);
        } else if (key.isBoolean())
            keyString = jsNontrivialString(m_vm, key.asBoolean() ? "true"_s : "false"_s);
        else if (isNone(key))
            keyString = jsNontrivialString(m_vm, "null"_s);
        else if (number.isInt()) {
            String text = reprOfInt(m_globalObject, number);
            RETURN_IF_EXCEPTION(scope, void());
            keyString = jsString(m_vm, text);
        } else if (m_encoder.skipKeys)
            return;
        else {
            raiseTypeError(m_globalObject, scope, concatenate("keys must be str, int, float, bool or None, not "_s, typeName(m_globalObject, key)));
            return;
        }

        if (isFirst) {
            isFirst = false;
            if (m_hasIndent)
                writeNewlineIndent(level);
        } else
            m_out.append(separator);

        writeString(keyString);
        RETURN_IF_EXCEPTION(scope, void());
        append(m_encoder.keySeparator.get());
        RETURN_IF_EXCEPTION(scope, void());
        writeObject(value, level);
        if (scope.exception()) [[unlikely]] {
            addNoteToRaised(m_globalObject, [&] { return concatenate("when serializing "_s, fullyQualifiedTypeName(m_globalObject, dict), " item "_s, repr(m_globalObject, key)); });
            return;
        }
    }

    // encoder_listencode_dict()
    void writeDict(PyDict* dict, unsigned level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!dict->size()) {
            m_out.append("{}"_s);
            return;
        }
        JSValue identity;
        enter(dict, identity);
        RETURN_IF_EXCEPTION(scope, void());
        m_out.append('{');

        String separator = stringIn(m_encoder.itemSeparator.get())->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (m_hasIndent) {
            ++level;
            const String* indented = itemSeparator(level);
            RETURN_IF_EXCEPTION(scope, void());
            separator = *indented;
        }

        bool isFirst = true;
        if (m_encoder.sortKeys || !isExactly(m_globalObject, dict, m_globalObject->pyRealm()->typeDict())) {
            JSValue items = itemsOfMapping(m_globalObject, dict);
            RETURN_IF_EXCEPTION(scope, void());
            if (m_encoder.sortKeys) {
                MarkedArgumentBuffer values;
                collect(m_globalObject, items, values);
                RETURN_IF_EXCEPTION(scope, void());
                MarkedArgumentBuffer sorted;
                sortValues(m_globalObject, values, sorted);
                RETURN_IF_EXCEPTION(scope, void());
                items = newList(m_globalObject, sorted);
                RETURN_IF_EXCEPTION(scope, void());
            }
            JSArray* list = asList(items);
            for (unsigned i = 0; i < list->length(); ++i) {
                JSValue item = listGet(m_globalObject, list, i);
                RETURN_IF_EXCEPTION(scope, void());
                if (!isTuple(item) || asTuple(item)->length() != 2) {
                    raiseValueError(m_globalObject, scope, "items must return 2-tuples"_s);
                    return;
                }
                writeKeyAndValue(isFirst, dict, asTuple(item)->at(0), asTuple(item)->at(1), level, separator);
                RETURN_IF_EXCEPTION(scope, void());
            }
        } else {
            dict->forEach(m_globalObject, [&] (JSValue key, JSValue value) {
                writeKeyAndValue(isFirst, dict, key, value, level, separator);
                return !scope.exception();
            });
            RETURN_IF_EXCEPTION(scope, void());
        }

        leave(identity);
        RETURN_IF_EXCEPTION(scope, void());
        if (m_hasIndent && !isFirst)
            writeNewlineIndent(level - 1);
        m_out.append('}');
    }

    // encoder_listencode_list()
    void writeList(JSValue given, unsigned level)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyRealm* realm = m_globalObject->pyRealm();
        // PySequence_Fast(): one of a class derived from list or tuple is gone through as it says.
        JSValue sequence = given;
        if (!isExactly(m_globalObject, given, realm->typeList()) && !isExactly(m_globalObject, given, realm->typeTuple())) {
            sequence = listFromIterable(m_globalObject, given);
            RETURN_IF_EXCEPTION(scope, void());
        }
        auto length = [&] () -> unsigned { return isList(sequence) ? asList(sequence)->length() : asTuple(sequence)->length(); };
        if (!length()) {
            m_out.append("[]"_s);
            return;
        }
        JSValue identity;
        enter(given, identity);
        RETURN_IF_EXCEPTION(scope, void());
        m_out.append('[');

        String separator = stringIn(m_encoder.itemSeparator.get())->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (m_hasIndent) {
            ++level;
            const String* indented = itemSeparator(level);
            RETURN_IF_EXCEPTION(scope, void());
            separator = *indented;
            writeNewlineIndent(level);
        }
        // Writing one can run anything, and there may be more of them or fewer afterwards.
        for (unsigned i = 0; i < length(); ++i) {
            JSValue item = isList(sequence) ? listGet(m_globalObject, asList(sequence), i) : asTuple(sequence)->at(i);
            RETURN_IF_EXCEPTION(scope, void());
            if (i)
                m_out.append(separator);
            writeObject(item, level);
            if (scope.exception()) [[unlikely]] {
                addNoteToRaised(m_globalObject, concatenate("when serializing "_s, fullyQualifiedTypeName(m_globalObject, given), " item "_s, i));
                return;
            }
        }
        leave(identity);
        RETURN_IF_EXCEPTION(scope, void());
        if (m_hasIndent)
            writeNewlineIndent(level - 1);
        m_out.append(']');
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    Encoder& m_encoder;
    bool m_hasIndent;
    TextBuilder m_out;
    Vector<String, 9> m_indentCache;
};

// encoder_call(): _iterencode(obj, _current_indent_level)
PYTHON_NATIVE(encoderCall)
{
    NATIVE_PROLOGUE();
    auto level = toSsize(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    Writer writer(globalObject, stateOf<Encoder>(args[0]));
    writer.createIndentCache(*level);
    RETURN_IF_EXCEPTION(scope, { });
    writer.writeObject(args[1], 0);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue text = writer.finish();
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { text }));
}

} // namespace

// ---- The module

JSObject* createJSONModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = jsonModuleState(globalObject);
    if (!state.scannerType) {
        auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name) {
            PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, 0);
            type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
            slot.set(vm, realm, type);
            return type;
        };
        PyType* scanner = make(state.scannerType, "_json.Scanner"_s);
        addMethods(globalObject, scanner, {
            { "__new__"_s, scannerNew, Kind::New, 0, "make_scanner($type, /, context)"_s, Arguments::AreThoseOfTheClass },
            { "__call__"_s, scannerCall, Kind::Wrapper, 0, "scan_once($self, /, string, idx)"_s, Arguments::AreThoseOfTheClass },
        });
        addMember(globalObject, scanner, "strict"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOf<Scanner>(self).strict); });
        addMember(globalObject, scanner, "object_hook"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Scanner>(self).objectHook.get(); });
        addMember(globalObject, scanner, "object_pairs_hook"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Scanner>(self).objectPairsHook.get(); });
        addMember(globalObject, scanner, "parse_float"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Scanner>(self).parseFloat.get(); });
        addMember(globalObject, scanner, "parse_int"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Scanner>(self).parseInt.get(); });
        addMember(globalObject, scanner, "parse_constant"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Scanner>(self).parseConstant.get(); });

        PyType* encoder = make(state.encoderType, "_json.Encoder"_s);
        addMethods(globalObject, encoder, {
            { "__new__"_s, encoderNew, Kind::New, 0, "make_encoder($type, /, markers, default, encoder, indent, key_separator, item_separator, sort_keys, skipkeys, allow_nan)"_s, Arguments::AreThoseOfTheClass },
            { "__call__"_s, encoderCall, Kind::Wrapper, 0, "_iterencode($self, /, obj, _current_indent_level)"_s, Arguments::AreThoseOfTheClass },
        });
        addMember(globalObject, encoder, "markers"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Encoder>(self).markers.get(); });
        addMember(globalObject, encoder, "default"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Encoder>(self).defaultFunction.get(); });
        addMember(globalObject, encoder, "encoder"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Encoder>(self).encoder.get(); });
        addMember(globalObject, encoder, "indent"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Encoder>(self).indent.get(); });
        addMember(globalObject, encoder, "key_separator"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Encoder>(self).keySeparator.get(); });
        addMember(globalObject, encoder, "item_separator"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Encoder>(self).itemSeparator.get(); });
        addMember(globalObject, encoder, "sort_keys"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOf<Encoder>(self).sortKeys); });
        addMember(globalObject, encoder, "skipkeys"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOf<Encoder>(self).skipKeys); });
    }

    JSObject* module = newBuiltinModule(globalObject, "_json"_s);
    addFunction(globalObject, module, "encode_basestring_ascii"_s, jsonEncodeBaseStringASCII);
    addFunction(globalObject, module, "encode_basestring"_s, jsonEncodeBaseString);
    addFunction(globalObject, module, "scanstring"_s, jsonScanString, 0, "($module, string, end, strict=True, /)"_s, Arguments::AreCheckedAsByParseTuple);
    module->putDirect(vm, Identifier::fromString(vm, "make_scanner"_s), state.scannerType->object());
    module->putDirect(vm, Identifier::fromString(vm, "make_encoder"_s), state.encoderType->object());
    return module;
}

} } // namespace JSC::Python
