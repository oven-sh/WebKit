/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCodecs.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyCommandLine.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyUnicode.h"
#include <bit>
#include <unicode/uchar.h>
#include <wtf/text/ASCIIFastPath.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// str.encode(), bytes.decode() and the module _codecs: the codecs every program needs, the error handlers, and the registry
// through which the package `encodings` provides the other codecs.
// (Python/codecs.c, Modules/_codecsmodule.c and the codecs in Objects/unicodeobject.c in CPython are the reference.)
//
// Positions in a str count code points, as everywhere in Python, while the text is UTF-16.

static constexpr bool codecHostIsLittleEndian = std::endian::native == std::endian::little;

// ---- Small things ----

static int64_t codecToSsize(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, -1);
    if (auto result = tryInt64(index))
        return *result;
    raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
    return -1;
}

static int codecToCInt(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isInt32()) [[likely]]
        return value.asInt32();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, -1);
    auto result = tryInt64(index);
    if (result && *result >= std::numeric_limits<int>::min() && *result <= std::numeric_limits<int>::max())
        return static_cast<int>(*result);
    raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
    return -1;
}

// The name of an encoding or of an error handler, which CPython takes as a C string. A null `name` gives a null string.
static bool codecNameText(JSGlobalObject* globalObject, JSString* name, String& text)
{
    text = String();
    if (!name)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    text = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (text.isNull())
        text = emptyString();
    if (text.find(static_cast<char16_t>(0)) != notFound) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    return true;
}

static JSValue codecRaiseMemoryError(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseWithArgs(globalObject, scope, runtime(globalObject).MemoryError(), { });
}

void addNoteToPendingException(JSGlobalObject* globalObject, const String& note)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto add = [&] {
        JSValue notes = getAttrIfPresent(globalObject, exception, rt.names.dNotes());
        RETURN_IF_EXCEPTION(scope, void());
        if (!notes) {
            notes = newList(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
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
        if (JSValue failure = pendingException(globalObject))
            PyException::from(failure)->setContext(vm, exception);
        return;
    }
    throwPythonException(globalObject, scope, exception);
}

template<typename Put>
static void codecWriteBackslashEscape(char32_t character, const Put& put)
{
    static constexpr char digits[] = "0123456789abcdef";
    put('\\');
    unsigned count;
    if (character >= 0x10000) {
        put('U');
        count = 8;
    } else if (character >= 0x100) {
        put('u');
        count = 4;
    } else {
        put('x');
        count = 2;
    }
    while (count--)
        put(digits[(character >> (count * 4)) & 0xf]);
}

template<typename Put>
static void codecWriteCharacterReference(char32_t character, const Put& put)
{
    put('&');
    put('#');
    char buffer[10];
    unsigned count = 0;
    do {
        buffer[count++] = static_cast<char>('0' + character % 10);
        character /= 10;
    } while (character);
    while (count--)
        put(buffer[count]);
    put(';');
}

static void codecAppendUTF8(ByteBuilder& out, char32_t character)
{
    if (character < 0x80)
        out.append(static_cast<uint8_t>(character));
    else if (character < 0x800) {
        out.append(static_cast<uint8_t>(0xc0 | (character >> 6)));
        out.append(static_cast<uint8_t>(0x80 | (character & 0x3f)));
    } else if (character < 0x10000) {
        out.append(static_cast<uint8_t>(0xe0 | (character >> 12)));
        out.append(static_cast<uint8_t>(0x80 | ((character >> 6) & 0x3f)));
        out.append(static_cast<uint8_t>(0x80 | (character & 0x3f)));
    } else {
        out.append(static_cast<uint8_t>(0xf0 | (character >> 18)));
        out.append(static_cast<uint8_t>(0x80 | ((character >> 12) & 0x3f)));
        out.append(static_cast<uint8_t>(0x80 | ((character >> 6) & 0x3f)));
        out.append(static_cast<uint8_t>(0x80 | (character & 0x3f)));
    }
}

static int codecHexDigitValue(uint8_t character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

namespace {

// The handlers some codecs apply themselves, without looking them up.
enum class CodecErrorKind : uint8_t {
    Unknown,
    Strict,
    SurrogateEscape,
    Replace,
    Ignore,
    BackslashReplace,
    SurrogatePass,
    XMLCharRefReplace,
    Other,
};

// The code points of a str.
class CodecText {
    WTF_MAKE_NONCOPYABLE(CodecText);
public:
    CodecText() = default;

    // False: an exception.
    bool initialize(JSGlobalObject* globalObject, JSString* string)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        set(text);
        return true;
    }

    void set(const String& string)
    {
        m_string = string;
        m_length = string.length();
        if (string.is8Bit()) {
            m_form = Form::Narrow;
            m_narrow = byteCast<uint8_t>(string.span8());
            return;
        }
        m_units = string.span16();
        bool hasPair = false;
        for (size_t i = 0; i + 1 < m_units.size(); ++i) {
            if (U16_IS_LEAD(m_units[i]) && U16_IS_TRAIL(m_units[i + 1])) {
                hasPair = true;
                break;
            }
        }
        if (!hasPair) {
            m_form = Form::Units;
            return;
        }
        m_form = Form::Wide;
        m_wide.reserveInitialCapacity(m_units.size());
        for (size_t i = 0; i < m_units.size(); ++i) {
            char32_t character = m_units[i];
            if (U16_IS_LEAD(character) && i + 1 < m_units.size() && U16_IS_TRAIL(m_units[i + 1])) {
                character = U16_GET_SUPPLEMENTARY(character, m_units[i + 1]);
                ++i;
            }
            m_wide.append(character);
        }
        m_length = static_cast<unsigned>(m_wide.size());
    }

    unsigned length() const { return m_length; }
    char32_t operator[](unsigned index) const
    {
        ASSERT(index < m_length);
        switch (m_form) {
        case Form::Narrow:
            return m_narrow[index];
        case Form::Units:
            return m_units[index];
        case Form::Wide:
            return m_wide[index];
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isNarrow() const { return m_form == Form::Narrow; }
    std::span<const uint8_t> narrow() const { return m_narrow; }
    bool isASCII() const { return m_string.containsOnlyASCII(); }
    bool isLatin1() const { return m_string.containsOnlyLatin1(); }

private:
    enum class Form : uint8_t { Narrow, Units, Wide };

    String m_string;
    std::span<const uint8_t> m_narrow;
    std::span<const char16_t> m_units;
    Vector<char32_t> m_wide;
    unsigned m_length { 0 };
    Form m_form { Form::Narrow };
};

// The bytes of an argument, which stay where they are meanwhile: an error handler may run anything.
class CodecBuffer {
    WTF_MAKE_NONCOPYABLE(CodecBuffer);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    CodecBuffer() = default;
    ~CodecBuffer()
    {
        if (m_globalObject)
            releaseBuffer(m_globalObject, m_buffer);
    }

    // With `acceptStr`, a str stands for its UTF-8. False: an exception.
    bool initialize(JSGlobalObject* globalObject, JSValue value, bool acceptStr)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (acceptStr && isStr(value)) {
            String text = strValue(value)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            auto encoded = tryEncodeUTF8(text);
            if (!encoded) {
                scope.release();
                codecEncodeUTF8(globalObject, strValue(value), String());
                return false;
            }
            m_owned = WTF::move(*encoded);
            m_span = m_owned.span();
            return true;
        }
        scope.release();
        if (!getBuffer(globalObject, value, m_buffer))
            return false;
        m_globalObject = globalObject;
        m_span = m_buffer.span();
        return true;
    }

    std::span<const uint8_t> span() const { return m_span; }

private:
    std::span<const uint8_t> m_span;
    Vector<uint8_t> m_owned;
    Buffer m_buffer;
    JSGlobalObject* m_globalObject { nullptr };
};

} // namespace

static CodecErrorKind codecErrorKindOf(const String& errors)
{
    if (errors.isNull() || errors == "strict"_s)
        return CodecErrorKind::Strict;
    if (errors == "surrogateescape"_s)
        return CodecErrorKind::SurrogateEscape;
    if (errors == "replace"_s)
        return CodecErrorKind::Replace;
    if (errors == "ignore"_s)
        return CodecErrorKind::Ignore;
    if (errors == "backslashreplace"_s)
        return CodecErrorKind::BackslashReplace;
    if (errors == "surrogatepass"_s)
        return CodecErrorKind::SurrogatePass;
    if (errors == "xmlcharrefreplace"_s)
        return CodecErrorKind::XMLCharRefReplace;
    return CodecErrorKind::Other;
}

// ---- The registry ----
//
// It lives in the dict of the module _codecs, under names `from _codecs import *` leaves alone.

namespace {

struct CodecRegistry {
    PyDict* moduleDict { nullptr };
    JSArray* searchPath { nullptr };
    PyDict* cache { nullptr };
    PyDict* errorHandlers { nullptr };
};

} // namespace

static bool codecGetRegistry(JSGlobalObject* globalObject, CodecRegistry& registry)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Not by way of sys.modules: codecs are looked up while the interpreter shuts down, when nothing can be imported.
    JSValue module = nativeModuleInstance(globalObject, internString(vm, "_codecs"_s), *findNativeModule("_codecs"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (auto* codecs = PyModule::tryFrom(module)) {
        registry.moduleDict = codecs->dict();
        if (JSValue searchPath = registry.moduleDict->get(internString(vm, "_search_path"_s)))
            registry.searchPath = tryAsList(searchPath);
        if (JSValue cache = registry.moduleDict->get(internString(vm, "_cache"_s)))
            registry.cache = PyDict::tryFrom(cache);
        if (JSValue errorHandlers = registry.moduleDict->get(internString(vm, "_errors"_s)))
            registry.errorHandlers = PyDict::tryFrom(errorHandlers);
    }
    if (!registry.searchPath || !registry.cache || !registry.errorHandlers) {
        raiseSystemError(globalObject, scope, "the codec registry is gone from the module _codecs"_s);
        return false;
    }
    return true;
}

// The package `encodings` registers the function that finds the standard codecs, which is to come before any other.
// CPython imports it as it starts; here, that waits until the registry is first used.
static bool codecImportEncodings(JSGlobalObject* globalObject, CodecRegistry& registry)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* key = internString(vm, "_encodings_imported"_s);
    if (registry.moduleDict->get(key))
        return true;
    registry.moduleDict->set(vm, key, jsBoolean(true));
    importModule(globalObject, "encodings"_s);
    if (scope.exception()) [[unlikely]] {
        registry.moduleDict->remove(vm, key);
        return false;
    }
    return true;
}

JSValue codecLookup(JSGlobalObject* globalObject, const String& encoding)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodecRegistry registry;
    if (!codecGetRegistry(globalObject, registry))
        return { };

    // Whatever the case of the ASCII letters, and with hyphens for spaces: "US ASCII" is "us-ascii".
    StrBuilder builder;
    for (unsigned i = 0; i < encoding.length(); ++i) {
        char16_t character = encoding[i];
        builder.append(character == ' ' ? static_cast<char16_t>('-') : toASCIILower(character));
    }
    JSString* name = internString(vm, builder.toString());

    if (JSValue cached = registry.cache->get(name))
        return cached;

    if (!codecImportEncodings(globalObject, registry))
        return { };

    unsigned count = listLength(registry.searchPath);
    if (!count)
        return raiseLookupError(globalObject, scope, "no codec search functions registered: can't find encoding"_s);
    JSValue result;
    for (unsigned i = 0; i < count && i < listLength(registry.searchPath); ++i) {
        result = call(globalObject, listGet(registry.searchPath, i), name);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(result)) {
            result = JSValue();
            continue;
        }
        auto* tuple = PyTuple::tryFrom(result);
        if (!tuple || tuple->length() != 4)
            return raiseTypeError(globalObject, scope, "codec search functions must return 4-tuples"_s);
        break;
    }
    if (!result)
        return raiseLookupError(globalObject, scope, "unknown encoding: "_s, encoding);
    registry.cache->set(vm, name, result);
    return result;
}

JSValue codecLookupTextEncoding(JSGlobalObject* globalObject, const String& encoding, ASCIILiteral alternateCommand)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue codec = codecLookup(globalObject, encoding);
    RETURN_IF_EXCEPTION(scope, { });
    // A plain tuple, or anything that does not say, is taken to be a text encoding.
    if (PyTuple::from(codec)->type() == rt.tupleType())
        return codec;
    JSValue flag = getAttrIfPresent(globalObject, codec, internString(vm, "_is_text_encoding"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!flag)
        return codec;
    bool isText = isTrue(globalObject, flag);
    RETURN_IF_EXCEPTION(scope, { });
    if (isText)
        return codec;
    if (!alternateCommand.isNull())
        return raiseLookupError(globalObject, scope, '\'', encoding, "' is not a text encoding; use "_s, alternateCommand, " to handle arbitrary codecs"_s);
    return raiseLookupError(globalObject, scope, '\'', encoding, "' is not a text encoding"_s);
}

// encoder(object[, errors])[0], or the same with a decoder.
static JSValue codecCallCodec(JSGlobalObject* globalObject, JSValue function, JSValue object, const String& encoding, const String& errors, bool isEncoder)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = errors.isNull() ? call(globalObject, function, object) : call(globalObject, function, object, makeStr(vm, errors));
    if (!result) {
        scope.release();
        addNoteToPendingException(globalObject, makeString(isEncoder ? "encoding"_s : "decoding"_s, " with '"_s, encoding, "' codec failed"_s));
        return { };
    }
    auto* tuple = PyTuple::tryFrom(result);
    if (!tuple || tuple->length() != 2)
        return raiseTypeError(globalObject, scope, isEncoder ? "encoder must return a tuple (object, integer)"_s : "decoder must return a tuple (object,integer)"_s);
    return tuple->at(0);
}

JSValue codecEncodeObject(JSGlobalObject* globalObject, JSValue object, const String& encoding, const String& errors)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue codec = codecLookup(globalObject, encoding);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, codecCallCodec(globalObject, PyTuple::from(codec)->at(0), object, encoding, errors, true));
}

JSValue codecDecodeObject(JSGlobalObject* globalObject, JSValue object, const String& encoding, const String& errors)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue codec = codecLookup(globalObject, encoding);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, codecCallCodec(globalObject, PyTuple::from(codec)->at(1), object, encoding, errors, false));
}

static JSValue codecMakeIncremental(JSGlobalObject* globalObject, JSValue codecInfo, const String& errors, ASCIILiteral attribute)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue factory = getAttr(globalObject, codecInfo, internString(vm, attribute));
    RETURN_IF_EXCEPTION(scope, { });
    if (errors.isNull())
        RELEASE_AND_RETURN(scope, call(globalObject, factory));
    RELEASE_AND_RETURN(scope, call(globalObject, factory, makeStr(vm, errors)));
}

JSValue codecIncrementalEncoder(JSGlobalObject* globalObject, JSValue codecInfo, const String& errors)
{
    return codecMakeIncremental(globalObject, codecInfo, errors, "incrementalencoder"_s);
}

JSValue codecIncrementalDecoder(JSGlobalObject* globalObject, JSValue codecInfo, const String& errors)
{
    return codecMakeIncremental(globalObject, codecInfo, errors, "incrementaldecoder"_s);
}

JSValue codecLookupError(JSGlobalObject* globalObject, const String& name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodecRegistry registry;
    if (!codecGetRegistry(globalObject, registry))
        return { };
    if (JSValue handler = registry.errorHandlers->get(internString(vm, name.isNull() ? String("strict"_s) : name)))
        return handler;
    return raiseLookupError(globalObject, scope, "unknown error handler name '"_s, name, '\'');
}

// ---- What an error handler is told, and what it answers ----

static JSValue codecErrorAttribute(JSGlobalObject* globalObject, PyException* exception, unsigned field, ASCIILiteral name, bool asBytes)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = exception->field(field);
    if (!value)
        return raiseTypeError(globalObject, scope, "UnicodeError '"_s, name, "' attribute is not set"_s);
    if (asBytes ? !PyBytes::is(value) : !isStr(value))
        return raiseTypeError(globalObject, scope, "UnicodeError '"_s, name, "' attribute must be a "_s, asBytes ? "bytes"_s : "string"_s);
    return value;
}

namespace {

// The object of a UnicodeError and the range in it, clipped to what is there.
struct CodecErrorInfo {
    CodecText text; // Of a UnicodeEncodeError or a UnicodeTranslateError.
    std::span<const uint8_t> bytes; // Of a UnicodeDecodeError.
    int64_t length { 0 };
    int64_t start { 0 };
    int64_t end { 0 };
    int64_t sliceLength { 0 };
};

} // namespace

static bool codecGetErrorInfo(JSGlobalObject* globalObject, PyException* exception, bool asBytes, CodecErrorInfo& info)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    codecErrorAttribute(globalObject, exception, ExceptionField::unicodeErrorObject, "object"_s, asBytes);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue startValue = exception->field(ExceptionField::unicodeErrorStart);
    JSValue endValue = exception->field(ExceptionField::unicodeErrorEnd);
    int64_t start = startValue ? toSsize(globalObject, startValue, nullptr) : 0;
    RETURN_IF_EXCEPTION(scope, false);
    int64_t end = endValue ? toSsize(globalObject, endValue, nullptr) : 0;
    RETURN_IF_EXCEPTION(scope, false);
    // Nothing of the program's runs from here on, so the exception goes on holding the object.
    JSValue object = codecErrorAttribute(globalObject, exception, ExceptionField::unicodeErrorObject, "object"_s, asBytes);
    RETURN_IF_EXCEPTION(scope, false);
    if (asBytes) {
        info.bytes = PyBytes::from(object)->span();
        info.length = static_cast<int64_t>(info.bytes.size());
    } else {
        if (!info.text.initialize(globalObject, strValue(object)))
            return false;
        info.length = info.text.length();
    }
    start = std::max<int64_t>(start, 0);
    if (start >= info.length)
        start = info.length ? info.length - 1 : 0;
    end = std::min(std::max<int64_t>(end, 1), info.length);
    info.start = start;
    info.end = end;
    info.sliceLength = std::max<int64_t>(end - start, 0);
    return true;
}

namespace {

// What an encoder has in hand: the text, the bytes so far, and the way to the error handler.
class CodecEncoder {
    WTF_MAKE_NONCOPYABLE(CodecEncoder);
public:
    CodecEncoder(JSGlobalObject* globalObject, JSString* string, const String& errors, ASCIILiteral encoding)
        : m_globalObject(globalObject)
        , m_string(string)
        , m_errors(errors)
        , m_encoding(encoding)
    {
    }

    bool initialize() { return text.initialize(m_globalObject, m_string); }

    CodecErrorKind kind()
    {
        if (m_kind == CodecErrorKind::Unknown)
            m_kind = codecErrorKindOf(m_errors);
        return m_kind;
    }

    // What the handler gives in place of the characters from `start` to `end`, a str or bytes, and where to go on from.
    JSValue callHandler(ASCIILiteral reason, unsigned start, unsigned end, unsigned& newPosition)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!m_handler) {
            m_handler = codecLookupError(m_globalObject, m_errors);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (!prepareException(reason, start, end))
            return { };
        JSValue result = call(m_globalObject, m_handler, m_exception);
        RETURN_IF_EXCEPTION(scope, { });

        static constexpr auto message = "encoding error handler must return (str/bytes, int) tuple"_s;
        auto* tuple = PyTuple::tryFrom(result);
        if (!tuple || tuple->length() != 2)
            return raiseTypeError(m_globalObject, scope, message);
        JSValue replacement = tuple->at(0);
        int64_t position = codecToSsize(m_globalObject, tuple->at(1));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isStr(replacement) && !PyBytes::is(replacement))
            return raiseTypeError(m_globalObject, scope, message);
        int64_t length = text.length();
        if (position < 0)
            position += length;
        if (position < 0 || position > length)
            return raiseIndexError(m_globalObject, scope, "position "_s, position, " from error handler out of bounds"_s);
        newPosition = static_cast<unsigned>(position);
        return replacement;
    }

    // Raises the UnicodeEncodeError, whatever the handler.
    void raise(ASCIILiteral reason, unsigned start, unsigned end)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (prepareException(reason, start, end))
            raiseException(m_globalObject, scope, m_exception);
    }

    // Appends the replacement as it is: bytes, or a str whose characters are all below the limit. Otherwise, raises.
    bool appendReplacement(JSValue replacement, char32_t limit, ASCIILiteral reason, unsigned start, unsigned end)
    {
        if (auto* bytes = PyBytes::tryFrom(replacement)) {
            out.append(bytes->span());
            return true;
        }
        CodecText characters;
        if (!characters.initialize(m_globalObject, strValue(replacement)))
            return false;
        if (limit == 256 ? !characters.isLatin1() : !characters.isASCII()) {
            raise(reason, start, end);
            return false;
        }
        for (unsigned i = 0; i < characters.length(); ++i)
            out.append(static_cast<uint8_t>(characters[i]));
        return true;
    }

    JSValue finish() { return makeBytes(m_globalObject, out); }

    CodecText text;
    ByteBuilder out;

private:
    bool prepareException(ASCIILiteral reason, unsigned start, unsigned end)
    {
        Runtime& rt = runtime(m_globalObject);
        VM& vm = rt.vm();
        if (!m_exception) {
            JSValue arguments[] = { makeStr(vm, m_encoding), m_string, jsNumber(start), jsNumber(end), makeStr(vm, reason) };
            m_exception = newException(m_globalObject, rt.UnicodeEncodeError(), arguments);
            return m_exception;
        }
        m_exception->setField(vm, ExceptionField::unicodeErrorStart, jsNumber(start));
        m_exception->setField(vm, ExceptionField::unicodeErrorEnd, jsNumber(end));
        m_exception->setField(vm, ExceptionField::unicodeErrorReason, makeStr(vm, reason));
        return true;
    }

    JSGlobalObject* m_globalObject;
    JSString* m_string;
    const String& m_errors;
    ASCIILiteral m_encoding;
    CodecErrorKind m_kind { CodecErrorKind::Unknown };
    JSValue m_handler;
    PyException* m_exception { nullptr };
};

// The same for a decoder.
class CodecDecoder {
    WTF_MAKE_NONCOPYABLE(CodecDecoder);
public:
    CodecDecoder(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors)
        : m_globalObject(globalObject)
        , m_input(input)
        , m_errors(errors)
    {
    }

    // A handler may replace the bytes being decoded, so they are to be asked for again after each call.
    std::span<const uint8_t> input() const { return m_input; }

    CodecErrorKind kind()
    {
        if (m_kind == CodecErrorKind::Unknown)
            m_kind = codecErrorKindOf(m_errors);
        return m_kind;
    }

    // Appends what the handler gives in place of the bytes from `start` to `end`, and sets where to go on from.
    bool callHandler(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end, size_t& position)
    {
        Runtime& rt = runtime(m_globalObject);
        VM& vm = rt.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!m_handler) {
            m_handler = codecLookupError(m_globalObject, m_errors);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (!m_exception) {
            JSValue arguments[] = { makeStr(vm, encoding), PyBytes::create(m_globalObject, m_input), jsNumber(start), jsNumber(end), makeStr(vm, reason) };
            m_exception = newException(m_globalObject, rt.UnicodeDecodeError(), arguments);
            RETURN_IF_EXCEPTION(scope, false);
        } else {
            m_exception->setField(vm, ExceptionField::unicodeErrorStart, jsNumber(start));
            m_exception->setField(vm, ExceptionField::unicodeErrorEnd, jsNumber(end));
            m_exception->setField(vm, ExceptionField::unicodeErrorReason, makeStr(vm, reason));
        }
        JSValue result = call(m_globalObject, m_handler, m_exception);
        RETURN_IF_EXCEPTION(scope, false);

        auto* tuple = PyTuple::tryFrom(result);
        if (!tuple || tuple->length() != 2 || !isStr(tuple->at(0))) {
            raiseTypeError(m_globalObject, scope, "decoding error handler must return (str, int) tuple"_s);
            return false;
        }
        int64_t newPosition = codecToSsize(m_globalObject, tuple->at(1));
        RETURN_IF_EXCEPTION(scope, false);

        // From here on the bytes are those of the exception.
        m_object = codecErrorAttribute(m_globalObject, m_exception, ExceptionField::unicodeErrorObject, "object"_s, true);
        RETURN_IF_EXCEPTION(scope, false);
        m_input = PyBytes::from(m_object)->span();
        int64_t size = static_cast<int64_t>(m_input.size());
        if (newPosition < 0)
            newPosition += size;
        if (newPosition < 0 || newPosition > size) {
            raiseIndexError(m_globalObject, scope, "position "_s, newPosition, " from error handler out of bounds"_s);
            return false;
        }
        String replacement = strValue(tuple->at(0))->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        out.append(replacement);
        position = static_cast<size_t>(newPosition);
        return true;
    }

    JSValue finish()
    {
        if (out.hasOverflowed())
            return codecRaiseMemoryError(m_globalObject);
        return makeStr(getVM(m_globalObject), out.toString());
    }

    StrBuilder out;

private:
    JSGlobalObject* m_globalObject;
    std::span<const uint8_t> m_input;
    JSValue m_object; // What holds the input, once that is no longer the caller's.
    const String& m_errors;
    CodecErrorKind m_kind { CodecErrorKind::Unknown };
    JSValue m_handler;
    PyException* m_exception { nullptr };
};

} // namespace

// ---- The built-in error handlers ----

static PyException* codecAsError(JSValue value, PyType* type)
{
    auto* exception = PyException::tryFrom(value);
    return exception && exception->type()->isSubtypeOf(type) ? exception : nullptr;
}

static JSValue codecRaiseWrongExceptionType(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = qualifiedTypeName(globalObject, typeOf(globalObject, value));
    if (name.startsWith("__main__."_s))
        name = name.substring(9);
    return raiseTypeError(globalObject, scope, "don't know how to handle "_s, name, " in error callback"_s);
}

static JSValue codecHandlerResult(JSGlobalObject* globalObject, JSValue replacement, int64_t position)
{
    return PyTuple::create(globalObject, { replacement, jsNumber(static_cast<double>(position)) });
}

static JSValue codecHandlerResult(JSGlobalObject* globalObject, StringBuilder& replacement, int64_t position)
{
    if (replacement.hasOverflowed())
        return codecRaiseMemoryError(globalObject);
    return codecHandlerResult(globalObject, makeStr(globalObject, replacement.toString()), position);
}

// The exception again, as it is.
static JSValue codecRaiseAgain(JSGlobalObject* globalObject, PyException* exception)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseException(globalObject, scope, exception);
}

static JSValue codecStrictErrors(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyException::is(value))
        return raiseException(globalObject, scope, value);
    return raiseTypeError(globalObject, scope, "codec must pass exception instance"_s);
}

static JSValue codecIgnoreErrors(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    PyException* exception = codecAsError(value, rt.UnicodeEncodeError());
    if (!exception)
        exception = codecAsError(value, rt.UnicodeTranslateError());
    bool asBytes = false;
    if (!exception) {
        exception = codecAsError(value, rt.UnicodeDecodeError());
        asBytes = true;
    }
    if (!exception)
        return codecRaiseWrongExceptionType(globalObject, value);
    CodecErrorInfo info;
    if (!codecGetErrorInfo(globalObject, exception, asBytes, info))
        return { };
    return codecHandlerResult(globalObject, jsEmptyString(rt.vm()), info.end);
}

static JSValue codecReplaceErrors(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    CodecErrorInfo info;
    StrBuilder replacement;
    if (auto* exception = codecAsError(value, rt.UnicodeEncodeError())) {
        if (!codecGetErrorInfo(globalObject, exception, false, info))
            return { };
        for (int64_t i = 0; i < info.sliceLength; ++i)
            replacement.append('?');
    } else if (auto* exception = codecAsError(value, rt.UnicodeDecodeError())) {
        if (!codecGetErrorInfo(globalObject, exception, true, info))
            return { };
        replacement.append(static_cast<char16_t>(0xfffd));
    } else if (auto* exception = codecAsError(value, rt.UnicodeTranslateError())) {
        if (!codecGetErrorInfo(globalObject, exception, false, info))
            return { };
        for (int64_t i = 0; i < info.sliceLength; ++i)
            replacement.append(static_cast<char16_t>(0xfffd));
    } else
        return codecRaiseWrongExceptionType(globalObject, value);
    return codecHandlerResult(globalObject, replacement, info.end);
}

static JSValue codecXMLCharRefReplaceErrors(JSGlobalObject* globalObject, JSValue value)
{
    auto* exception = codecAsError(value, runtime(globalObject).UnicodeEncodeError());
    if (!exception)
        return codecRaiseWrongExceptionType(globalObject, value);
    CodecErrorInfo info;
    if (!codecGetErrorInfo(globalObject, exception, false, info))
        return { };
    StrBuilder replacement;
    for (int64_t i = info.start; i < info.end; ++i)
        codecWriteCharacterReference(info.text[static_cast<unsigned>(i)], [&](char c) { replacement.append(c); });
    return codecHandlerResult(globalObject, replacement, info.end);
}

static JSValue codecBackslashReplaceErrors(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    CodecErrorInfo info;
    StrBuilder replacement;
    auto put = [&](char c) { replacement.append(c); };
    if (auto* exception = codecAsError(value, rt.UnicodeDecodeError())) {
        if (!codecGetErrorInfo(globalObject, exception, true, info))
            return { };
        for (int64_t i = info.start; i < info.end; ++i)
            codecWriteBackslashEscape(info.bytes[static_cast<size_t>(i)], put);
        return codecHandlerResult(globalObject, replacement, info.end);
    }
    PyException* exception = codecAsError(value, rt.UnicodeEncodeError());
    if (!exception)
        exception = codecAsError(value, rt.UnicodeTranslateError());
    if (!exception)
        return codecRaiseWrongExceptionType(globalObject, value);
    if (!codecGetErrorInfo(globalObject, exception, false, info))
        return { };
    for (int64_t i = info.start; i < info.end; ++i)
        codecWriteBackslashEscape(info.text[static_cast<unsigned>(i)], put);
    return codecHandlerResult(globalObject, replacement, info.end);
}

static JSValue codecNameReplaceErrors(JSGlobalObject* globalObject, JSValue value)
{
    auto* exception = codecAsError(value, runtime(globalObject).UnicodeEncodeError());
    if (!exception)
        return codecRaiseWrongExceptionType(globalObject, value);
    CodecErrorInfo info;
    if (!codecGetErrorInfo(globalObject, exception, false, info))
        return { };
    StrBuilder replacement;
    for (int64_t i = info.start; i < info.end; ++i) {
        char32_t character = info.text[static_cast<unsigned>(i)];
        char name[256];
        UErrorCode status = U_ZERO_ERROR;
        int32_t length = u_charName(static_cast<UChar32>(character), U_UNICODE_CHAR_NAME, name, sizeof(name), &status);
        if (U_SUCCESS(status) && length > 0 && static_cast<size_t>(length) < sizeof(name))
            replacement.append("\\N{"_s, std::span<const Latin1Character> { byteCast<Latin1Character>(&name[0]), static_cast<size_t>(length) }, '}');
        else if (String newName = nameOfNewCodePoint(character); !newName.isNull())
            replacement.append("\\N{"_s, newName, '}');
        else
            codecWriteBackslashEscape(character, [&](char c) { replacement.append(c); });
    }
    return codecHandlerResult(globalObject, replacement, std::max(info.start, info.end));
}

namespace {

enum class CodecStandardEncoding : uint8_t { Unknown, UTF8, UTF16BE, UTF16LE, UTF32BE, UTF32LE };

} // namespace

// Which of the encodings that "surrogatepass" knows the name stands for, and how many bytes a surrogate takes in it.
static CodecStandardEncoding codecStandardEncodingOf(StringView name, unsigned& byteLength)
{
    auto at = [&](unsigned index) -> char16_t { return index < name.length() ? name[index] : 0; };
    auto lower = [&](unsigned index) -> char16_t { return toASCIILower(at(index)); };
    // "-le", "_be", "le"... at the index. 0: neither.
    auto order = [&](unsigned index) -> char16_t {
        if (at(index) == '-' || at(index) == '_')
            ++index;
        if (at(index) && lower(index + 1) == 'e' && !at(index + 2) && (lower(index) == 'b' || lower(index) == 'l'))
            return lower(index);
        return 0;
    };
    if (lower(0) == 'u' && lower(1) == 't' && lower(2) == 'f') {
        unsigned index = 3;
        if (at(index) == '-' || at(index) == '_')
            ++index;
        if (at(index) == '8' && !at(index + 1)) {
            byteLength = 3;
            return CodecStandardEncoding::UTF8;
        }
        if (at(index) == '1' && at(index + 1) == '6') {
            byteLength = 2;
            if (!at(index + 2))
                return codecHostIsLittleEndian ? CodecStandardEncoding::UTF16LE : CodecStandardEncoding::UTF16BE;
            if (char16_t which = order(index + 2))
                return which == 'b' ? CodecStandardEncoding::UTF16BE : CodecStandardEncoding::UTF16LE;
        } else if (at(index) == '3' && at(index + 1) == '2') {
            byteLength = 4;
            if (!at(index + 2))
                return codecHostIsLittleEndian ? CodecStandardEncoding::UTF32LE : CodecStandardEncoding::UTF32BE;
            if (char16_t which = order(index + 2))
                return which == 'b' ? CodecStandardEncoding::UTF32BE : CodecStandardEncoding::UTF32LE;
        }
        return CodecStandardEncoding::Unknown;
    }
    if (name == "cp65001"_s) {
        byteLength = 3;
        return CodecStandardEncoding::UTF8;
    }
    return CodecStandardEncoding::Unknown;
}

static JSValue codecSurrogatePassErrors(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = codecAsError(value, rt.UnicodeEncodeError());
    bool isEncoding = exception;
    if (!exception)
        exception = codecAsError(value, rt.UnicodeDecodeError());
    if (!exception)
        RELEASE_AND_RETURN(scope, codecRaiseWrongExceptionType(globalObject, value));

    JSValue encodingValue = codecErrorAttribute(globalObject, exception, ExceptionField::unicodeErrorEncoding, "encoding"_s, false);
    RETURN_IF_EXCEPTION(scope, { });
    String encodingName = strValue(encodingValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned byteLength = 0;
    CodecStandardEncoding encoding = codecStandardEncodingOf(encodingName, byteLength);
    if (encoding == CodecStandardEncoding::Unknown)
        RELEASE_AND_RETURN(scope, codecRaiseAgain(globalObject, exception));

    CodecErrorInfo info;
    if (!codecGetErrorInfo(globalObject, exception, !isEncoding, info))
        return { };

    if (isEncoding) {
        ByteBuilder out;
        for (int64_t i = info.start; i < info.end; ++i) {
            char32_t character = info.text[static_cast<unsigned>(i)];
            if (!U_IS_SURROGATE(character))
                RELEASE_AND_RETURN(scope, codecRaiseAgain(globalObject, exception));
            switch (encoding) {
            case CodecStandardEncoding::UTF8:
                codecAppendUTF8(out, character);
                break;
            case CodecStandardEncoding::UTF16LE:
                out.append(static_cast<uint8_t>(character));
                out.append(static_cast<uint8_t>(character >> 8));
                break;
            case CodecStandardEncoding::UTF16BE:
                out.append(static_cast<uint8_t>(character >> 8));
                out.append(static_cast<uint8_t>(character));
                break;
            case CodecStandardEncoding::UTF32LE:
                out.append(static_cast<uint8_t>(character));
                out.append(static_cast<uint8_t>(character >> 8));
                out.append(0);
                out.append(0);
                break;
            case CodecStandardEncoding::UTF32BE:
                out.append(0);
                out.append(0);
                out.append(static_cast<uint8_t>(character >> 8));
                out.append(static_cast<uint8_t>(character));
                break;
            case CodecStandardEncoding::Unknown:
                break;
            }
        }
        PyBytes* replacement = makeBytes(globalObject, out);
        if (!replacement)
            return { };
        return codecHandlerResult(globalObject, replacement, info.end);
    }

    // One surrogate. If more follow, the codec will be back.
    uint32_t character = 0;
    if (info.length - info.start >= byteLength) {
        const uint8_t* p = info.bytes.data() + info.start;
        switch (encoding) {
        case CodecStandardEncoding::UTF8:
            if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80)
                character = ((p[0] & 0x0f) << 12) + ((p[1] & 0x3f) << 6) + (p[2] & 0x3f);
            break;
        case CodecStandardEncoding::UTF16LE:
            character = p[1] << 8 | p[0];
            break;
        case CodecStandardEncoding::UTF16BE:
            character = p[0] << 8 | p[1];
            break;
        case CodecStandardEncoding::UTF32LE:
            character = (static_cast<uint32_t>(p[3]) << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
            break;
        case CodecStandardEncoding::UTF32BE:
            character = (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            break;
        case CodecStandardEncoding::Unknown:
            break;
        }
    }
    if (!U_IS_SURROGATE(character))
        RELEASE_AND_RETURN(scope, codecRaiseAgain(globalObject, exception));
    return codecHandlerResult(globalObject, makeStr(vm, stringFromCodePoint(character)), info.start + byteLength);
}

static JSValue codecSurrogateEscapeErrors(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    CodecErrorInfo info;
    if (auto* exception = codecAsError(value, rt.UnicodeEncodeError())) {
        if (!codecGetErrorInfo(globalObject, exception, false, info))
            return { };
        ByteBuilder out;
        for (int64_t i = info.start; i < info.end; ++i) {
            char32_t character = info.text[static_cast<unsigned>(i)];
            if (character < 0xdc80 || character > 0xdcff)
                return codecRaiseAgain(globalObject, exception);
            out.append(static_cast<uint8_t>(character - 0xdc00));
        }
        PyBytes* replacement = makeBytes(globalObject, out);
        if (!replacement)
            return { };
        return codecHandlerResult(globalObject, replacement, info.end);
    }
    if (auto* exception = codecAsError(value, rt.UnicodeDecodeError())) {
        if (!codecGetErrorInfo(globalObject, exception, true, info))
            return { };
        // Up to four bytes at a time, and never an ASCII one.
        StrBuilder replacement;
        int64_t consumed = 0;
        while (consumed < 4 && consumed < info.sliceLength) {
            uint8_t byte = info.bytes[static_cast<size_t>(info.start + consumed)];
            if (byte < 128)
                break;
            replacement.append(static_cast<char16_t>(0xdc00 + byte));
            ++consumed;
        }
        if (!consumed)
            return codecRaiseAgain(globalObject, exception);
        return codecHandlerResult(globalObject, replacement, info.start + consumed);
    }
    return codecRaiseWrongExceptionType(globalObject, value);
}

// ---- UTF-8 ----

JSValue codecEncodeUTF8(JSGlobalObject* globalObject, JSString* string, const String& errors)
{
    static constexpr auto reason = "surrogates not allowed"_s;
    CodecEncoder encoder(globalObject, string, errors, "utf-8"_s);
    if (!encoder.initialize())
        return { };
    const CodecText& text = encoder.text;
    if (text.isNarrow() && text.isASCII())
        return PyBytes::create(globalObject, text.narrow());

    ByteBuilder& out = encoder.out;
    unsigned size = text.length();
    out.reserveInitialCapacity(static_cast<size_t>(size) * (text.isNarrow() ? 2 : 3));
    auto put = [&](char c) { out.append(static_cast<uint8_t>(c)); };
    for (unsigned i = 0; i < size;) {
        char32_t character = text[i];
        if (!U_IS_SURROGATE(character)) [[likely]] {
            codecAppendUTF8(out, character);
            ++i;
            continue;
        }
        unsigned start = i;
        unsigned end = start + 1;
        while (end < size && U_IS_SURROGATE(text[end]))
            ++end;
        bool callsHandler = false;
        switch (encoder.kind()) {
        case CodecErrorKind::Replace:
            for (unsigned k = start; k < end; ++k)
                out.append('?');
            break;
        case CodecErrorKind::Ignore:
            break;
        case CodecErrorKind::SurrogatePass:
            for (unsigned k = start; k < end; ++k)
                codecAppendUTF8(out, text[k]);
            break;
        case CodecErrorKind::BackslashReplace:
            for (unsigned k = start; k < end; ++k)
                codecWriteBackslashEscape(text[k], put);
            break;
        case CodecErrorKind::XMLCharRefReplace:
            for (unsigned k = start; k < end; ++k)
                codecWriteCharacterReference(text[k], put);
            break;
        case CodecErrorKind::SurrogateEscape:
            while (start < end && text[start] >= 0xdc80 && text[start] <= 0xdcff)
                out.append(static_cast<uint8_t>(text[start++] & 0xff));
            callsHandler = start < end;
            break;
        default:
            callsHandler = true;
            break;
        }
        if (!callsHandler) {
            i = end;
            continue;
        }
        JSValue replacement = encoder.callHandler(reason, start, end, i);
        if (!replacement || !encoder.appendReplacement(replacement, 128, reason, start, end))
            return { };
    }
    return encoder.finish();
}

namespace {

enum class CodecUTF8Status : uint8_t {
    Valid,
    Incomplete, // The data ends before the sequence does.
    InvalidStart,
    InvalidContinuation, // `length` is how many bytes are good.
};

} // namespace

// Looks at the sequence the bytes, of which there is at least one, start with.
static CodecUTF8Status codecScanUTF8(std::span<const uint8_t> s, char32_t& character, unsigned& length)
{
    auto isContinuation = [](uint8_t byte) { return (byte & 0xc0) == 0x80; };
    auto invalidAfter = [&](unsigned good) {
        length = good;
        return CodecUTF8Status::InvalidContinuation;
    };
    size_t available = s.size();
    uint32_t first = s[0];
    if (first < 0x80) {
        character = first;
        length = 1;
        return CodecUTF8Status::Valid;
    }
    if (first < 0xe0) {
        // A continuation byte, or what would be a longer form of an ASCII character.
        if (first < 0xc2)
            return CodecUTF8Status::InvalidStart;
        if (available < 2)
            return CodecUTF8Status::Incomplete;
        if (!isContinuation(s[1]))
            return invalidAfter(1);
        character = ((first & 0x1f) << 6) | (s[1] & 0x3f);
        length = 2;
        return CodecUTF8Status::Valid;
    }
    if (first < 0xf0) {
        if (available < 2)
            return CodecUTF8Status::Incomplete;
        uint32_t second = s[1];
        // Neither a longer form of a shorter sequence nor a surrogate.
        if (!isContinuation(s[1]) || (second < 0xa0 ? first == 0xe0 : first == 0xed))
            return invalidAfter(1);
        if (available < 3)
            return CodecUTF8Status::Incomplete;
        if (!isContinuation(s[2]))
            return invalidAfter(2);
        character = ((first & 0x0f) << 12) | ((second & 0x3f) << 6) | (s[2] & 0x3f);
        length = 3;
        return CodecUTF8Status::Valid;
    }
    if (first < 0xf5) {
        if (available < 2)
            return CodecUTF8Status::Incomplete;
        uint32_t second = s[1];
        // Neither a longer form of a shorter sequence nor beyond U+10FFFF.
        if (!isContinuation(s[1]) || (second < 0x90 ? first == 0xf0 : first == 0xf4))
            return invalidAfter(1);
        if (available < 3)
            return CodecUTF8Status::Incomplete;
        if (!isContinuation(s[2]))
            return invalidAfter(2);
        if (available < 4)
            return CodecUTF8Status::Incomplete;
        if (!isContinuation(s[3]))
            return invalidAfter(3);
        character = ((first & 0x07) << 18) | ((second & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        length = 4;
        return CodecUTF8Status::Valid;
    }
    return CodecUTF8Status::InvalidStart;
}

JSValue codecDecodeUTF8(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors, size_t* consumed)
{
    VM& vm = getVM(globalObject);
    if (input.empty()) {
        if (consumed)
            *consumed = 0;
        return jsEmptyString(vm);
    }
    if (input.size() > String::MaxLength)
        return codecRaiseMemoryError(globalObject);
    // Most text is well formed.
    if (String text { byteCast<char8_t>(input) }; !text.isNull()) {
        if (consumed)
            *consumed = input.size();
        return makeStr(vm, text);
    }

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    size_t position = 0;
    while (position < decoder.input().size()) {
        auto rest = decoder.input().subspan(position);
        char32_t character = 0;
        unsigned length = 0;
        size_t end = position;
        ASCIILiteral reason;
        bool stops = false;
        switch (codecScanUTF8(rest, character, length)) {
        case CodecUTF8Status::Valid:
            appendCodePoint(out, character);
            position += length;
            continue;
        case CodecUTF8Status::Incomplete:
            stops = consumed;
            reason = "unexpected end of data"_s;
            end = position + rest.size();
            break;
        case CodecUTF8Status::InvalidStart:
            reason = "invalid start byte"_s;
            end = position + 1;
            break;
        case CodecUTF8Status::InvalidContinuation:
            // The first two bytes of a surrogate, which "surrogatepass" may accept once the third is there.
            stops = consumed && length == 1 && rest.size() == 2 && rest[0] == 0xed && rest[1] >= 0xa0 && rest[1] <= 0xbf;
            reason = "invalid continuation byte"_s;
            end = position + length;
            break;
        }
        if (stops)
            break;

        switch (decoder.kind()) {
        case CodecErrorKind::Ignore:
            position = end;
            break;
        case CodecErrorKind::Replace:
            out.append(static_cast<char16_t>(0xfffd));
            position = end;
            break;
        case CodecErrorKind::SurrogateEscape:
            for (; position < end; ++position)
                out.append(static_cast<char16_t>(0xdc00 + decoder.input()[position]));
            break;
        default:
            if (!decoder.callHandler("utf-8"_s, reason, position, end, position))
                return { };
            break;
        }
    }
    if (consumed)
        *consumed = position;
    return decoder.finish();
}

// ---- ASCII, Latin-1 ----

static JSValue codecEncodeNarrow(JSGlobalObject* globalObject, JSString* string, const String& errors, char32_t limit)
{
    ASCIILiteral reason = limit == 256 ? "ordinal not in range(256)"_s : "ordinal not in range(128)"_s;
    CodecEncoder encoder(globalObject, string, errors, limit == 256 ? "latin-1"_s : "ascii"_s);
    if (!encoder.initialize())
        return { };
    const CodecText& text = encoder.text;
    if (text.isNarrow() && (limit == 256 || text.isASCII()))
        return PyBytes::create(globalObject, text.narrow());

    ByteBuilder& out = encoder.out;
    unsigned size = text.length();
    out.reserveInitialCapacity(size);
    auto put = [&](char c) { out.append(static_cast<uint8_t>(c)); };
    for (unsigned position = 0; position < size;) {
        char32_t character = text[position];
        if (character < limit) {
            out.append(static_cast<uint8_t>(character));
            ++position;
            continue;
        }
        // All that cannot be encoded goes to the handler at once.
        unsigned start = position;
        unsigned end = start + 1;
        while (end < size && text[end] >= limit)
            ++end;
        bool callsHandler = false;
        switch (encoder.kind()) {
        case CodecErrorKind::Strict:
            encoder.raise(reason, start, end);
            return { };
        case CodecErrorKind::Replace:
            for (unsigned k = start; k < end; ++k)
                out.append('?');
            break;
        case CodecErrorKind::Ignore:
            break;
        case CodecErrorKind::BackslashReplace:
            for (unsigned k = start; k < end; ++k)
                codecWriteBackslashEscape(text[k], put);
            break;
        case CodecErrorKind::XMLCharRefReplace:
            for (unsigned k = start; k < end; ++k)
                codecWriteCharacterReference(text[k], put);
            break;
        case CodecErrorKind::SurrogateEscape:
            while (start < end && text[start] >= 0xdc80 && text[start] <= 0xdcff)
                out.append(static_cast<uint8_t>(text[start++] - 0xdc00));
            callsHandler = start < end;
            break;
        default:
            callsHandler = true;
            break;
        }
        if (!callsHandler) {
            position = end;
            continue;
        }
        JSValue replacement = encoder.callHandler(reason, start, end, position);
        if (!replacement || !encoder.appendReplacement(replacement, limit, reason, start, end))
            return { };
    }
    return encoder.finish();
}

JSValue codecEncodeASCII(JSGlobalObject* globalObject, JSString* string, const String& errors)
{
    return codecEncodeNarrow(globalObject, string, errors, 128);
}

JSValue codecEncodeLatin1(JSGlobalObject* globalObject, JSString* string, const String& errors)
{
    return codecEncodeNarrow(globalObject, string, errors, 256);
}

JSValue codecDecodeLatin1(JSGlobalObject* globalObject, std::span<const uint8_t> input)
{
    if (input.size() > String::MaxLength)
        return codecRaiseMemoryError(globalObject);
    return makeStr(getVM(globalObject), String(byteCast<Latin1Character>(input)));
}

JSValue codecDecodeASCII(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors)
{
    if (charactersAreAllASCII(byteCast<Latin1Character>(input)))
        return codecDecodeLatin1(globalObject, input);

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    size_t position = 0;
    while (position < decoder.input().size()) {
        uint8_t byte = decoder.input()[position];
        if (byte < 128) {
            out.append(static_cast<char>(byte));
            ++position;
            continue;
        }
        switch (decoder.kind()) {
        case CodecErrorKind::Replace:
            out.append(static_cast<char16_t>(0xfffd));
            ++position;
            break;
        case CodecErrorKind::SurrogateEscape:
            out.append(static_cast<char16_t>(0xdc00 + byte));
            ++position;
            break;
        case CodecErrorKind::Ignore:
            ++position;
            break;
        default:
            if (!decoder.callHandler("ascii"_s, "ordinal not in range(128)"_s, position, position + 1, position))
                return { };
            break;
        }
    }
    return decoder.finish();
}

// ---- UTF-16, UTF-32 ----

template<unsigned unitSize>
static JSValue codecEncodeUnits(JSGlobalObject* globalObject, JSString* string, const String& errors, int byteOrder, ASCIILiteral encoding)
{
    static constexpr auto reason = "surrogates not allowed"_s;
    CodecEncoder encoder(globalObject, string, errors, encoding);
    if (!encoder.initialize())
        return { };
    const CodecText& text = encoder.text;
    ByteBuilder& out = encoder.out;
    unsigned size = text.length();
    out.reserveInitialCapacity((static_cast<size_t>(size) + 1) * unitSize);
    bool isLittleEndian = byteOrder < 0 || (!byteOrder && codecHostIsLittleEndian);
    auto put = [&](uint32_t unit) {
        for (unsigned i = 0; i < unitSize; ++i)
            out.append(static_cast<uint8_t>(unit >> (8 * (isLittleEndian ? i : unitSize - 1 - i))));
    };

    if (!byteOrder)
        put(0xfeff);
    for (unsigned position = 0; position < size;) {
        char32_t character = text[position];
        if (!U_IS_SURROGATE(character)) [[likely]] {
            if (unitSize == 2 && character >= 0x10000) {
                put(U16_LEAD(character));
                put(U16_TRAIL(character));
            } else
                put(character);
            ++position;
            continue;
        }
        unsigned start = position;
        JSValue replacement = encoder.callHandler(reason, start, start + 1, position);
        if (!replacement)
            return { };
        if (auto* bytes = PyBytes::tryFrom(replacement)) {
            if (bytes->length() & (unitSize - 1)) {
                encoder.raise(reason, start, start + 1);
                return { };
            }
            out.append(bytes->span());
            continue;
        }
        CodecText characters;
        if (!characters.initialize(globalObject, strValue(replacement)))
            return { };
        if (!characters.isASCII()) {
            encoder.raise(reason, start, start + 1);
            return { };
        }
        for (unsigned i = 0; i < characters.length(); ++i)
            put(characters[i]);
    }
    return encoder.finish();
}

JSValue codecEncodeUTF16(JSGlobalObject* globalObject, JSString* string, const String& errors, int byteOrder)
{
    return codecEncodeUnits<2>(globalObject, string, errors, byteOrder, byteOrder < 0 ? "utf-16-le"_s : byteOrder > 0 ? "utf-16-be"_s : "utf-16"_s);
}

JSValue codecEncodeUTF32(JSGlobalObject* globalObject, JSString* string, const String& errors, int byteOrder)
{
    return codecEncodeUnits<4>(globalObject, string, errors, byteOrder, byteOrder == -1 ? "utf-32-le"_s : byteOrder == 1 ? "utf-32-be"_s : "utf-32"_s);
}

JSValue codecDecodeUTF16(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors, int* byteOrder, size_t* consumed)
{
    size_t position = 0;
    int order = byteOrder ? *byteOrder : 0;
    if (!order && input.size() >= 2) {
        unsigned mark = (input[1] << 8) | input[0];
        if (mark == 0xfeff) {
            position = 2;
            order = -1;
        } else if (mark == 0xfffe) {
            position = 2;
            order = 1;
        }
        if (byteOrder)
            *byteOrder = order;
    }
    if (position == input.size()) {
        if (consumed)
            *consumed = input.size();
        return jsEmptyString(getVM(globalObject));
    }
    bool isLittleEndian = order < 0 || (!order && codecHostIsLittleEndian);
    ASCIILiteral encoding = isLittleEndian ? "utf-16-le"_s : "utf-16-be"_s;

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    while (true) {
        auto in = decoder.input();
        size_t size = in.size();
        auto unitAt = [&](size_t offset) -> char16_t {
            return static_cast<char16_t>(isLittleEndian ? in[offset] | (in[offset + 1] << 8) : (in[offset] << 8) | in[offset + 1]);
        };
        size_t start = 0;
        size_t end = 0;
        ASCIILiteral reason;
        while (size - position >= 2) {
            char16_t unit = unitAt(position);
            position += 2;
            if (!U16_IS_SURROGATE(unit)) {
                out.append(unit);
                continue;
            }
            if (!U16_IS_LEAD(unit)) {
                reason = "illegal encoding"_s;
                start = position - 2;
                end = position;
                break;
            }
            if (size - position < 2) {
                position -= 2;
                reason = "unexpected end of data"_s;
                start = position;
                end = size;
                break;
            }
            char16_t second = unitAt(position);
            position += 2;
            if (!U16_IS_TRAIL(second)) {
                reason = "illegal UTF-16 surrogate"_s;
                start = position - 4;
                end = start + 2;
                break;
            }
            out.append(unit);
            out.append(second);
        }
        if (reason.isNull()) {
            if (position == size || consumed)
                break;
            reason = "truncated data"_s;
            start = position;
            end = size;
        } else if (end == size && start == position && consumed)
            break; // Half of a pair: the rest is to come.
        if (!decoder.callHandler(encoding, reason, start, end, position))
            return { };
    }
    if (consumed)
        *consumed = position;
    return decoder.finish();
}

JSValue codecDecodeUTF32(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors, int* byteOrder, size_t* consumed)
{
    size_t position = 0;
    int order = byteOrder ? *byteOrder : 0;
    if (!order && input.size() >= 4) {
        uint32_t mark = (static_cast<uint32_t>(input[3]) << 24) | (input[2] << 16) | (input[1] << 8) | input[0];
        if (mark == 0x0000feff) {
            position = 4;
            order = -1;
        } else if (mark == 0xfffe0000) {
            position = 4;
            order = 1;
        }
        if (byteOrder)
            *byteOrder = order;
    }
    if (position == input.size()) {
        if (consumed)
            *consumed = input.size();
        return jsEmptyString(getVM(globalObject));
    }
    bool isLittleEndian = order < 0 || (!order && codecHostIsLittleEndian);
    ASCIILiteral encoding = isLittleEndian ? "utf-32-le"_s : "utf-32-be"_s;

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    while (true) {
        auto in = decoder.input();
        size_t size = in.size();
        ASCIILiteral reason;
        while (size - position >= 4) {
            const uint8_t* p = in.data() + position;
            uint32_t character = isLittleEndian
                ? (static_cast<uint32_t>(p[3]) << 24) | (p[2] << 16) | (p[1] << 8) | p[0]
                : (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            if (U_IS_SURROGATE(character)) {
                reason = "code point in surrogate code point range(0xd800, 0xe000)"_s;
                break;
            }
            if (character > 0x10ffff) {
                reason = "code point not in range(0x110000)"_s;
                break;
            }
            appendCodePoint(out, character);
            position += 4;
        }
        size_t end = position + 4;
        if (reason.isNull()) {
            if (position == size || consumed)
                break;
            reason = "truncated data"_s;
            end = size;
        }
        if (!decoder.callHandler(encoding, reason, position, end, position))
            return { };
    }
    if (consumed)
        *consumed = position;
    return decoder.finish();
}

// ---- UTF-7 ----

static bool codecIsBase64(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
}

static unsigned codecFromBase64(uint32_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : 63;
}

static uint8_t codecToBase64(uint64_t bits)
{
    return static_cast<uint8_t>("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[bits & 0x3f]);
}

// Whether the encoder leaves the character as it is: all of ASCII but + \ ~ and the control characters that are not whitespace.
static bool codecUTF7EncodesDirectly(char32_t c)
{
    if (c >= 127 || c == '+' || c == '\\' || c == '~')
        return false;
    return c >= ' ' || c == '\t' || c == '\n' || c == '\r';
}

JSValue codecDecodeUTF7(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors, size_t* consumed)
{
    if (input.empty()) {
        if (consumed)
            *consumed = 0;
        return jsEmptyString(getVM(globalObject));
    }

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    size_t s = 0;
    size_t start = 0;
    bool inShift = false;
    unsigned shiftOutStart = 0;
    unsigned base64Bits = 0;
    uint64_t base64Buffer = 0;
    char32_t surrogate = 0;

    while (true) {
        while (s < decoder.input().size()) {
            auto in = decoder.input();
            uint32_t c = in[s];
            ASCIILiteral reason;
            if (inShift) {
                if (codecIsBase64(c)) {
                    base64Buffer = (base64Buffer << 6) | codecFromBase64(c);
                    base64Bits += 6;
                    ++s;
                    if (base64Bits >= 16) {
                        // Enough for a UTF-16 code unit.
                        char32_t unit = static_cast<char32_t>(base64Buffer >> (base64Bits - 16));
                        base64Bits -= 16;
                        base64Buffer &= (static_cast<uint64_t>(1) << base64Bits) - 1;
                        if (surrogate) {
                            char32_t lead = surrogate;
                            surrogate = 0;
                            if (U16_IS_TRAIL(unit)) {
                                appendCodePoint(out, U16_GET_SUPPLEMENTARY(lead, unit));
                                continue;
                            }
                            appendCodePoint(out, lead);
                        }
                        if (U16_IS_LEAD(unit))
                            surrogate = unit;
                        else
                            appendCodePoint(out, unit);
                    }
                    continue;
                }
                // The end of the base-64 section.
                inShift = false;
                if (base64Bits >= 6) {
                    ++s;
                    reason = "partial character in shift sequence"_s;
                } else if (base64Bits > 0 && base64Buffer) {
                    ++s;
                    reason = "non-zero padding bits in shift sequence"_s;
                } else {
                    if (surrogate && c <= 127 && c != '+')
                        appendCodePoint(out, surrogate);
                    surrogate = 0;
                    // A '-' is absorbed; anything else that ends the section stands for itself.
                    if (c == '-')
                        ++s;
                    continue;
                }
            } else if (c == '+') {
                start = s;
                ++s;
                if (s < in.size() && in[s] == '-') {
                    ++s;
                    out.append('+');
                    continue;
                }
                if (s < in.size() && !codecIsBase64(in[s])) {
                    ++s;
                    reason = "ill-formed sequence"_s;
                } else {
                    inShift = true;
                    surrogate = 0;
                    shiftOutStart = out.length();
                    base64Bits = 0;
                    base64Buffer = 0;
                    continue;
                }
            } else if (c <= 127) {
                ++s;
                out.append(static_cast<char>(c));
                continue;
            } else {
                start = s;
                ++s;
                reason = "unexpected special character"_s;
            }
            if (!decoder.callHandler("utf7"_s, reason, start, s, s))
                return { };
        }

        if (!inShift || consumed)
            break;
        // In a base-64 section with nothing more to come.
        inShift = false;
        if (!surrogate && base64Bits < 6 && !(base64Bits > 0 && base64Buffer))
            break;
        if (!decoder.callHandler("utf7"_s, "unterminated shift sequence"_s, start, decoder.input().size(), s))
            return { };
        if (s >= decoder.input().size())
            break;
    }

    if (consumed) {
        if (inShift) {
            // The state of a section is not kept: next time, it is read again from its start.
            *consumed = start;
            out.shrink(shiftOutStart);
        } else
            *consumed = s;
    }
    return decoder.finish();
}

JSValue codecEncodeUTF7(JSGlobalObject* globalObject, JSString* string, const String&)
{
    CodecText text;
    if (!text.initialize(globalObject, string))
        return { };
    ByteBuilder out;
    out.reserveInitialCapacity(text.length());
    bool inShift = false;
    unsigned base64Bits = 0;
    uint64_t base64Buffer = 0;
    auto putUnit = [&](char32_t unit) {
        base64Bits += 16;
        base64Buffer = (base64Buffer << 16) | unit;
        while (base64Bits >= 6) {
            out.append(codecToBase64(base64Buffer >> (base64Bits - 6)));
            base64Bits -= 6;
        }
        base64Buffer &= 0x3f;
    };
    for (unsigned i = 0; i < text.length(); ++i) {
        char32_t c = text[i];
        if (inShift) {
            if (codecUTF7EncodesDirectly(c)) {
                if (base64Bits) {
                    out.append(codecToBase64(base64Buffer << (6 - base64Bits)));
                    base64Buffer = 0;
                    base64Bits = 0;
                }
                inShift = false;
                // Only what could be taken for more of the section needs a '-' before it.
                if (codecIsBase64(c) || c == '-')
                    out.append('-');
                out.append(static_cast<uint8_t>(c));
                continue;
            }
        } else if (c == '+') {
            out.append('+');
            out.append('-');
            continue;
        } else if (codecUTF7EncodesDirectly(c)) {
            out.append(static_cast<uint8_t>(c));
            continue;
        } else {
            out.append('+');
            inShift = true;
        }
        if (c >= 0x10000) {
            putUnit(U16_LEAD(c));
            c = U16_TRAIL(c);
        }
        putUnit(c);
    }
    if (base64Bits)
        out.append(codecToBase64(base64Buffer << (6 - base64Bits)));
    if (inShift)
        out.append('-');
    return makeBytes(globalObject, out);
}

// ---- unicode_escape, raw_unicode_escape ----

// "\8" is an invalid escape sequence, "\400" an invalid octal one. False: the warning became an exception.
static bool codecWarnInvalidEscape(JSGlobalObject* globalObject, int character, ASCIILiteral prefix)
{
    Runtime& rt = runtime(globalObject);
    if (character > 0xff) {
        char digits[] = { static_cast<char>('0' + ((character >> 6) & 7)), static_cast<char>('0' + ((character >> 3) & 7)), static_cast<char>('0' + (character & 7)) };
        return warn(globalObject, rt.DeprecationWarning(), makeString(prefix, "\"\\"_s, digits[0], digits[1], digits[2], "\" is an invalid octal escape sequence. Such sequences will not work in the future. "_s));
    }
    return warn(globalObject, rt.DeprecationWarning(), makeString(prefix, "\"\\"_s, stringFromCodePoint(static_cast<char32_t>(character)), "\" is an invalid escape sequence. Such sequences will not work in the future. "_s));
}

namespace {

enum class CodecEscapeOutcome : uint8_t {
    Done,
    Incomplete, // The data ends in the middle of the escape.
    Error,
};

} // namespace

// Reads that many hexadecimal digits at `s`, which is left after the last one read.
static CodecEscapeOutcome codecReadHexDigits(std::span<const uint8_t> in, size_t& s, unsigned count, uint32_t& character)
{
    character = 0;
    for (; count; ++s, --count) {
        if (s >= in.size())
            return CodecEscapeOutcome::Incomplete;
        int digit = codecHexDigitValue(in[s]);
        if (digit < 0)
            return CodecEscapeOutcome::Error;
        character = (character << 4) + static_cast<uint32_t>(digit);
    }
    return CodecEscapeOutcome::Done;
}

JSValue codecDecodeUnicodeEscape(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors, size_t* consumed)
{
    if (input.empty()) {
        if (consumed)
            *consumed = 0;
        return jsEmptyString(getVM(globalObject));
    }

    if (consumed)
        *consumed = input.size();

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    int firstInvalidEscape = -1;
    size_t s = 0;
    while (s < decoder.input().size()) {
        auto in = decoder.input();
        size_t size = in.size();
        uint8_t c = in[s++];
        if (c != '\\') {
            appendCodePoint(out, c);
            continue;
        }

        size_t start = s - 1;
        ASCIILiteral message = "\\ at end of string"_s;
        CodecEscapeOutcome outcome = CodecEscapeOutcome::Done;
        unsigned hexDigits = 0;
        if (s >= size)
            outcome = CodecEscapeOutcome::Incomplete;
        else {
            c = in[s++];
            switch (c) {
            case '\n':
                break;
            case '\\':
            case '\'':
            case '"':
                out.append(static_cast<char>(c));
                break;
            case 'b':
                out.append('\b');
                break;
            case 'f':
                out.append('\f');
                break;
            case 't':
                out.append('\t');
                break;
            case 'n':
                out.append('\n');
                break;
            case 'r':
                out.append('\r');
                break;
            case 'v':
                out.append('\v');
                break;
            case 'a':
                out.append('\a');
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7': {
                char32_t character = c - '0';
                for (unsigned more = 0; more < 2 && s < size && in[s] >= '0' && in[s] <= '7'; ++more)
                    character = (character << 3) + in[s++] - '0';
                if (character > 0377 && firstInvalidEscape == -1)
                    firstInvalidEscape = static_cast<int>(character);
                appendCodePoint(out, character);
                break;
            }
            case 'x':
                hexDigits = 2;
                message = "truncated \\xXX escape"_s;
                break;
            case 'u':
                hexDigits = 4;
                message = "truncated \\uXXXX escape"_s;
                break;
            case 'U':
                hexDigits = 8;
                message = "truncated \\UXXXXXXXX escape"_s;
                break;
            case 'N': {
                message = "malformed \\N character escape"_s;
                if (s >= size) {
                    outcome = CodecEscapeOutcome::Incomplete;
                    break;
                }
                outcome = CodecEscapeOutcome::Error;
                if (in[s] != '{')
                    break;
                size_t nameStart = ++s;
                while (s < size && in[s] != '}')
                    ++s;
                if (s >= size) {
                    outcome = CodecEscapeOutcome::Incomplete;
                    break;
                }
                size_t nameLength = s - nameStart;
                if (!nameLength)
                    break;
                ++s;
                if (auto character = codePointFromName(in.subspan(nameStart, nameLength))) {
                    appendCodePoint(out, *character);
                    outcome = CodecEscapeOutcome::Done;
                } else
                    message = "unknown Unicode character name"_s;
                break;
            }
            default:
                if (firstInvalidEscape == -1)
                    firstInvalidEscape = c;
                out.append('\\');
                appendCodePoint(out, c);
                break;
            }
            if (hexDigits) {
                uint32_t character;
                outcome = codecReadHexDigits(in, s, hexDigits, character);
                if (outcome == CodecEscapeOutcome::Done) {
                    if (character > 0x10ffff) {
                        message = "illegal Unicode character"_s;
                        outcome = CodecEscapeOutcome::Error;
                    } else
                        appendCodePoint(out, character);
                }
            }
        }

        if (outcome == CodecEscapeOutcome::Done)
            continue;
        if (outcome == CodecEscapeOutcome::Incomplete && consumed) {
            *consumed = start;
            break;
        }
        if (!decoder.callHandler("unicodeescape"_s, message, start, s, s))
            return { };
    }
    if (firstInvalidEscape != -1 && !codecWarnInvalidEscape(globalObject, firstInvalidEscape, ""_s))
        return { };
    return decoder.finish();
}

JSValue codecEncodeUnicodeEscape(JSGlobalObject* globalObject, JSString* string)
{
    CodecText text;
    if (!text.initialize(globalObject, string))
        return { };
    ByteBuilder out;
    out.reserveInitialCapacity(text.length());
    auto put = [&](char c) { out.append(static_cast<uint8_t>(c)); };
    for (unsigned i = 0; i < text.length(); ++i) {
        char32_t character = text[i];
        if (character >= ' ' && character < 127) {
            if (character == '\\')
                put('\\');
            put(static_cast<char>(character));
        } else if (character == '\t') {
            put('\\');
            put('t');
        } else if (character == '\n') {
            put('\\');
            put('n');
        } else if (character == '\r') {
            put('\\');
            put('r');
        } else
            codecWriteBackslashEscape(character, put);
    }
    return makeBytes(globalObject, out);
}

JSValue codecDecodeRawUnicodeEscape(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors, size_t* consumed)
{
    if (input.empty()) {
        if (consumed)
            *consumed = 0;
        return jsEmptyString(getVM(globalObject));
    }

    if (consumed)
        *consumed = input.size();

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    size_t s = 0;
    while (s < decoder.input().size()) {
        auto in = decoder.input();
        size_t size = in.size();
        uint8_t c = in[s++];
        if (c != '\\' || (s >= size && !consumed)) {
            appendCodePoint(out, c);
            continue;
        }

        size_t start = s - 1;
        ASCIILiteral message = "\\ at end of string"_s;
        CodecEscapeOutcome outcome = CodecEscapeOutcome::Incomplete;
        if (s < size) {
            c = in[s++];
            if (c != 'u' && c != 'U') {
                out.append('\\');
                appendCodePoint(out, c);
                continue;
            }
            message = c == 'u' ? "truncated \\uXXXX escape"_s : "truncated \\UXXXXXXXX escape"_s;
            uint32_t character;
            outcome = codecReadHexDigits(in, s, c == 'u' ? 4 : 8, character);
            if (outcome == CodecEscapeOutcome::Done) {
                if (character <= 0x10ffff) {
                    appendCodePoint(out, character);
                    continue;
                }
                message = "\\Uxxxxxxxx out of range"_s;
                outcome = CodecEscapeOutcome::Error;
            }
        }
        if (outcome == CodecEscapeOutcome::Incomplete && consumed) {
            *consumed = start;
            break;
        }
        if (!decoder.callHandler("rawunicodeescape"_s, message, start, s, s))
            return { };
    }
    return decoder.finish();
}

JSValue codecEncodeRawUnicodeEscape(JSGlobalObject* globalObject, JSString* string)
{
    CodecText text;
    if (!text.initialize(globalObject, string))
        return { };
    if (text.isNarrow())
        return PyBytes::create(globalObject, text.narrow());
    ByteBuilder out;
    out.reserveInitialCapacity(text.length());
    auto put = [&](char c) { out.append(static_cast<uint8_t>(c)); };
    for (unsigned i = 0; i < text.length(); ++i) {
        char32_t character = text[i];
        if (character < 0x100)
            out.append(static_cast<uint8_t>(character));
        else
            codecWriteBackslashEscape(character, put);
    }
    return makeBytes(globalObject, out);
}

// ---- charmap ----

// mapping[key]. Empty, with no exception: it has no such key.
static JSValue codecMappingGet(JSGlobalObject* globalObject, JSValue mapping, uint32_t key)
{
    Runtime& rt = runtime(globalObject);
    if (auto* dict = PyDict::tryFrom(mapping); dict && dict->type() == rt.dictType())
        return dict->get(globalObject, jsNumber(key));
    JSValue item = getItem(globalObject, mapping, jsNumber(key));
    if (!item)
        clearPendingExceptionIfMatches(globalObject, rt.LookupError());
    return item;
}

JSValue codecDecodeCharmap(JSGlobalObject* globalObject, std::span<const uint8_t> input, JSValue mapping, const String& errors)
{
    static constexpr auto reason = "character maps to <undefined>"_s;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!mapping)
        RELEASE_AND_RETURN(scope, codecDecodeLatin1(globalObject, input));
    if (input.empty())
        return jsEmptyString(vm);

    CodecDecoder decoder(globalObject, input, errors);
    StringBuilder& out = decoder.out;
    size_t position = 0;

    if (mapping.isString()) {
        CodecText table;
        if (!table.initialize(globalObject, asString(mapping)))
            return { };
        while (position < decoder.input().size()) {
            uint8_t byte = decoder.input()[position];
            char32_t character = byte < table.length() ? table[byte] : 0xfffe;
            if (character != 0xfffe) {
                appendCodePoint(out, character);
                ++position;
                continue;
            }
            if (!decoder.callHandler("charmap"_s, reason, position, position + 1, position))
                return { };
        }
        RELEASE_AND_RETURN(scope, decoder.finish());
    }

    while (position < decoder.input().size()) {
        JSValue item = codecMappingGet(globalObject, mapping, decoder.input()[position]);
        RETURN_IF_EXCEPTION(scope, { });
        bool isUndefined = !item || isNone(item);
        if (isUndefined) {
        } else if (isInt(item)) {
            auto value = tryInt64(intValue(item));
            if (value && *value == 0xfffe)
                isUndefined = true;
            else if (!value || *value < 0 || *value > 0x10ffff)
                return raiseTypeError(globalObject, scope, "character mapping must be in range(0x110000)"_s);
            else
                appendCodePoint(out, static_cast<char32_t>(*value));
        } else if (isStr(item)) {
            String text = strValue(item)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (text.length() == 1 && text[0] == 0xfffe)
                isUndefined = true;
            else
                out.append(text);
        } else
            return raiseTypeError(globalObject, scope, "character mapping must return integer, None or str"_s);
        if (!isUndefined) {
            ++position;
            continue;
        }
        if (!decoder.callHandler("charmap"_s, reason, position, position + 1, position))
            return { };
    }
    RELEASE_AND_RETURN(scope, decoder.finish());
}

namespace {

enum class CodecCharmapResult : uint8_t {
    Success,
    Failed, // The mapping has nothing for the character.
    Exception,
};

} // namespace

// Appends what the character maps to. With no `out`, only looks.
static CodecCharmapResult codecCharmapOutput(JSGlobalObject* globalObject, char32_t character, JSValue mapping, ByteBuilder* out)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue item = codecMappingGet(globalObject, mapping, character);
    RETURN_IF_EXCEPTION(scope, CodecCharmapResult::Exception);
    if (!item || isNone(item))
        return CodecCharmapResult::Failed;
    if (isInt(item)) {
        auto value = tryInt64(intValue(item));
        if (!value || *value < 0 || *value > 255) {
            raiseTypeError(globalObject, scope, "character mapping must be in range(256)"_s);
            return CodecCharmapResult::Exception;
        }
        if (out)
            out->append(static_cast<uint8_t>(*value));
        return CodecCharmapResult::Success;
    }
    if (auto* bytes = PyBytes::tryFrom(item)) {
        if (out)
            out->append(bytes->span());
        return CodecCharmapResult::Success;
    }
    raiseTypeError(globalObject, scope, "character mapping must return integer, bytes or None, not "_s, typeName(globalObject, item));
    return CodecCharmapResult::Exception;
}

JSValue codecEncodeCharmap(JSGlobalObject* globalObject, JSString* string, JSValue mapping, const String& errors)
{
    static constexpr auto reason = "character maps to <undefined>"_s;
    if (!mapping)
        return codecEncodeLatin1(globalObject, string, errors);
    CodecEncoder encoder(globalObject, string, errors, "charmap"_s);
    if (!encoder.initialize())
        return { };
    const CodecText& text = encoder.text;
    ByteBuilder& out = encoder.out;
    unsigned size = text.length();
    out.reserveInitialCapacity(size);

    for (unsigned position = 0; position < size;) {
        auto result = codecCharmapOutput(globalObject, text[position], mapping, &out);
        if (result == CodecCharmapResult::Exception)
            return { };
        if (result == CodecCharmapResult::Success) {
            ++position;
            continue;
        }

        // All that cannot be encoded goes to the handler at once.
        unsigned start = position;
        unsigned end = start + 1;
        while (end < size) {
            result = codecCharmapOutput(globalObject, text[end], mapping, nullptr);
            if (result == CodecCharmapResult::Exception)
                return { };
            if (result == CodecCharmapResult::Success)
                break;
            ++end;
        }
        // What stands in for it is itself put through the mapping.
        bool failed = false;
        auto put = [&](char32_t character) {
            if (failed)
                return;
            result = codecCharmapOutput(globalObject, character, mapping, &out);
            if (result == CodecCharmapResult::Failed)
                encoder.raise(reason, start, end);
            failed = result != CodecCharmapResult::Success;
        };
        switch (encoder.kind()) {
        case CodecErrorKind::Strict:
            encoder.raise(reason, start, end);
            return { };
        case CodecErrorKind::Replace:
            for (unsigned k = start; k < end; ++k)
                put('?');
            position = end;
            break;
        case CodecErrorKind::Ignore:
            position = end;
            break;
        case CodecErrorKind::XMLCharRefReplace:
            for (unsigned k = start; k < end; ++k)
                codecWriteCharacterReference(text[k], put);
            position = end;
            break;
        default: {
            JSValue replacement = encoder.callHandler(reason, start, end, position);
            if (!replacement)
                return { };
            if (auto* bytes = PyBytes::tryFrom(replacement)) {
                out.append(bytes->span());
                break;
            }
            CodecText characters;
            if (!characters.initialize(globalObject, strValue(replacement)))
                return { };
            for (unsigned i = 0; i < characters.length(); ++i)
                put(characters[i]);
            break;
        }
        }
        if (failed)
            return { };
    }
    return encoder.finish();
}

// ---- str.encode(), bytes.decode() ----

// As encodings.normalize_encoding(): runs of anything but ASCII letters, digits and dots become one underscore, or nothing at the ends.
static String codecNormalizeEncoding(StringView encoding, bool toLower)
{
    StrBuilder result;
    bool pendingPunctuation = false;
    for (unsigned i = 0; i < encoding.length(); ++i) {
        char16_t character = encoding[i];
        if (!isASCIIAlphanumeric(character) && character != '.') {
            pendingPunctuation = true;
            continue;
        }
        if (pendingPunctuation && !result.isEmpty())
            result.append('_');
        pendingPunctuation = false;
        result.append(toLower ? toASCIILower(character) : character);
    }
    return result.toString();
}

namespace {

enum class CodecShortcut : uint8_t { None, UTF8, UTF16, UTF16LE, UTF16BE, UTF32, UTF32LE, UTF32BE, ASCII, Latin1 };

} // namespace

// The encodings that need no look in the registry.
static CodecShortcut codecShortcutFor(const String& encoding)
{
    String name = codecNormalizeEncoding(encoding, true);
    if (name.startsWith("utf"_s)) {
        StringView rest = StringView(name).substring(name.length() > 3 && name[3] == '_' ? 4 : 3);
        if (rest == "8"_s)
            return CodecShortcut::UTF8;
        if (rest == "16"_s)
            return CodecShortcut::UTF16;
        if (rest == "32"_s)
            return CodecShortcut::UTF32;
        // CPython leaves these to the package `encodings`, under just these names.
        if (name == "utf_16_le"_s || name == "utf_16le"_s)
            return CodecShortcut::UTF16LE;
        if (name == "utf_16_be"_s || name == "utf_16be"_s)
            return CodecShortcut::UTF16BE;
        if (name == "utf_32_le"_s || name == "utf_32le"_s)
            return CodecShortcut::UTF32LE;
        if (name == "utf_32_be"_s || name == "utf_32be"_s)
            return CodecShortcut::UTF32BE;
        return CodecShortcut::None;
    }
    if (name == "ascii"_s || name == "us_ascii"_s)
        return CodecShortcut::ASCII;
    if (name == "latin1"_s || name == "latin_1"_s || name == "iso_8859_1"_s || name == "iso8859_1"_s)
        return CodecShortcut::Latin1;
    return CodecShortcut::None;
}

// What a codec found in the registry adds to an exception.
static JSValue codecNoteFailure(JSGlobalObject* globalObject, JSValue result, ASCIILiteral operation, const String& encoding)
{
    if (!result)
        addNoteToPendingException(globalObject, makeString(operation, " with '"_s, encoding, "' codec failed"_s));
    return result;
}

// python -X dev: the names are looked up even where nothing has a use for them, as for an empty text. False: a LookupError.
static bool codecCheckNamesInDevelopmentMode(JSGlobalObject* globalObject, const String& encoding, const String& errors)
{
    if (!configuration().devMode) [[likely]]
        return true;
    if (!encoding.isNull() && encoding != "utf-8"_s && encoding != "utf8"_s && encoding != "ascii"_s && !codecLookup(globalObject, encoding))
        return false;
    if (errors.isNull() || errors == "strict"_s || errors == "ignore"_s || errors == "replace"_s || errors == "surrogateescape"_s || errors == "surrogatepass"_s)
        return true;
    return !!codecLookupError(globalObject, errors);
}

JSValue strEncode(JSGlobalObject* globalObject, JSString* string, JSString* encodingName, JSString* errorsName)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String encoding;
    String errors;
    if (!codecNameText(globalObject, encodingName, encoding) || !codecNameText(globalObject, errorsName, errors))
        return { };
    if (!codecCheckNamesInDevelopmentMode(globalObject, encoding, errors))
        return { };
    if (encoding.isNull())
        RELEASE_AND_RETURN(scope, codecEncodeUTF8(globalObject, string, errors));

    switch (codecShortcutFor(encoding)) {
    case CodecShortcut::UTF8:
        RELEASE_AND_RETURN(scope, codecEncodeUTF8(globalObject, string, errors));
    case CodecShortcut::UTF16:
        RELEASE_AND_RETURN(scope, codecEncodeUTF16(globalObject, string, errors, 0));
    case CodecShortcut::UTF32:
        RELEASE_AND_RETURN(scope, codecEncodeUTF32(globalObject, string, errors, 0));
    case CodecShortcut::ASCII:
        RELEASE_AND_RETURN(scope, codecEncodeASCII(globalObject, string, errors));
    case CodecShortcut::Latin1:
        RELEASE_AND_RETURN(scope, codecEncodeLatin1(globalObject, string, errors));
    case CodecShortcut::UTF16LE:
        RELEASE_AND_RETURN(scope, codecNoteFailure(globalObject, codecEncodeUTF16(globalObject, string, errors, -1), "encoding"_s, encoding));
    case CodecShortcut::UTF16BE:
        RELEASE_AND_RETURN(scope, codecNoteFailure(globalObject, codecEncodeUTF16(globalObject, string, errors, 1), "encoding"_s, encoding));
    case CodecShortcut::UTF32LE:
        RELEASE_AND_RETURN(scope, codecNoteFailure(globalObject, codecEncodeUTF32(globalObject, string, errors, -1), "encoding"_s, encoding));
    case CodecShortcut::UTF32BE:
        RELEASE_AND_RETURN(scope, codecNoteFailure(globalObject, codecEncodeUTF32(globalObject, string, errors, 1), "encoding"_s, encoding));
    case CodecShortcut::None:
        break;
    }

    JSValue codec = codecLookupTextEncoding(globalObject, encoding, "codecs.encode()"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = codecCallCodec(globalObject, PyTuple::from(codec)->at(0), string, encoding, errors, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (PyBytes::is(result))
        return result;
    if (auto* byteArray = PyByteArray::tryFrom(result)) {
        bool goesOn = warn(globalObject, rt.RuntimeWarning(), makeString("encoder "_s, encoding, " returned bytearray instead of bytes; use codecs.encode() to encode to arbitrary types"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!goesOn)
            return { };
        return PyBytes::create(globalObject, byteArray->span());
    }
    return raiseTypeError(globalObject, scope, '\'', encoding, "' encoder returned '"_s, typeName(globalObject, result), "' instead of 'bytes'; use codecs.encode() to encode to arbitrary types"_s);
}

JSValue bytesDecode(JSGlobalObject* globalObject, JSValue bytesLike, JSString* encodingName, JSString* errorsName)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String encoding;
    String errors;
    if (!codecNameText(globalObject, encodingName, encoding) || !codecNameText(globalObject, errorsName, errors))
        return { };
    CodecBuffer buffer;
    if (!buffer.initialize(globalObject, bytesLike, false))
        return { };
    auto bytes = buffer.span();
    if (!codecCheckNamesInDevelopmentMode(globalObject, encoding, errors))
        return { };
    // Whatever the encoding, even one there is no codec for.
    if (bytes.empty())
        return jsEmptyString(vm);
    if (encoding.isNull())
        RELEASE_AND_RETURN(scope, codecDecodeUTF8(globalObject, bytes, errors));

    CodecShortcut shortcut = codecShortcutFor(encoding);
    int byteOrder = shortcut == CodecShortcut::UTF16LE || shortcut == CodecShortcut::UTF32LE ? -1 : 1;
    switch (shortcut) {
    case CodecShortcut::UTF8:
        RELEASE_AND_RETURN(scope, codecDecodeUTF8(globalObject, bytes, errors));
    case CodecShortcut::UTF16:
        RELEASE_AND_RETURN(scope, codecDecodeUTF16(globalObject, bytes, errors));
    case CodecShortcut::UTF32:
        RELEASE_AND_RETURN(scope, codecDecodeUTF32(globalObject, bytes, errors));
    case CodecShortcut::ASCII:
        RELEASE_AND_RETURN(scope, codecDecodeASCII(globalObject, bytes, errors));
    case CodecShortcut::Latin1:
        RELEASE_AND_RETURN(scope, codecDecodeLatin1(globalObject, bytes));
    case CodecShortcut::UTF16LE:
    case CodecShortcut::UTF16BE:
        RELEASE_AND_RETURN(scope, codecNoteFailure(globalObject, codecDecodeUTF16(globalObject, bytes, errors, &byteOrder), "decoding"_s, encoding));
    case CodecShortcut::UTF32LE:
    case CodecShortcut::UTF32BE:
        RELEASE_AND_RETURN(scope, codecNoteFailure(globalObject, codecDecodeUTF32(globalObject, bytes, errors, &byteOrder), "decoding"_s, encoding));
    case CodecShortcut::None:
        break;
    }

    JSValue codec = codecLookupTextEncoding(globalObject, encoding, "codecs.decode()"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // FIXME: CPython passes a memoryview of the bytes.
    JSValue object = PyBytes::is(bytesLike) ? bytesLike : JSValue(PyBytes::create(globalObject, bytes));
    JSValue result = codecCallCodec(globalObject, PyTuple::from(codec)->at(1), object, encoding, errors, false);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(result))
        return raiseTypeError(globalObject, scope, '\'', encoding, "' decoder returned '"_s, typeName(globalObject, result), "' instead of 'str'; use codecs.decode() to decode to arbitrary types"_s);
    return result;
}

// ---- The module _codecs ----

// What a message about an argument of the wrong type calls what it was given.
static String codecArgumentTypeName(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) ? String("None"_s) : typeName(globalObject, value);
}

// An argument that must be a str: "f() argument 1 must be str, not int". `what`: "argument 1", "argument 'encoding'" or "argument".
static bool codecStrArgument(JSGlobalObject* globalObject, ASCIILiteral function, ASCIILiteral what, JSValue value, String& text)
{
    if (!isStr(value)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, function, "() "_s, what, " must be str, not "_s, codecArgumentTypeName(globalObject, value));
        return false;
    }
    return codecNameText(globalObject, strValue(value), text);
}

// The second argument of the codec functions: the name of an error handler, or None. A null string: none was given.
static bool codecErrorsArgument(JSGlobalObject* globalObject, ASCIILiteral function, JSValue value, String& errors)
{
    errors = String();
    if (!value || isNone(value))
        return true;
    if (!isStr(value)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, function, "() argument 2 must be str or None, not "_s, typeName(globalObject, value));
        return false;
    }
    return codecNameText(globalObject, strValue(value), errors);
}

static JSValue codecModuleRegister(JSGlobalObject* globalObject, JSValue searchFunction)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isCallable(globalObject, searchFunction))
        return raiseTypeError(globalObject, scope, "argument must be callable"_s);
    CodecRegistry registry;
    if (!codecGetRegistry(globalObject, registry) || !codecImportEncodings(globalObject, registry))
        return { };
    listAppend(globalObject, registry.searchPath, searchFunction);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue codecModuleUnregister(JSGlobalObject* globalObject, JSValue searchFunction)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodecRegistry registry;
    if (!codecGetRegistry(globalObject, registry))
        return { };
    for (unsigned i = 0; i < listLength(registry.searchPath); ++i) {
        if (!isIdentical(listGet(registry.searchPath, i), searchFunction))
            continue;
        listRemove(globalObject, registry.searchPath, i);
        RETURN_IF_EXCEPTION(scope, { });
        registry.cache->clear(vm);
        break;
    }
    return jsUndefined();
}

static JSValue codecModuleLookup(JSGlobalObject* globalObject, JSValue encodingValue)
{
    String encoding;
    if (!codecStrArgument(globalObject, "lookup"_s, "argument"_s, encodingValue, encoding))
        return { };
    return codecLookup(globalObject, encoding);
}

template<bool isEncoder>
static JSValue codecModuleTranscode(JSGlobalObject* globalObject, JSValue object, JSValue encodingValue, JSValue errorsValue)
{
    ASCIILiteral function = isEncoder ? "encode"_s : "decode"_s;
    String encoding = "utf-8"_s;
    String errors;
    if (encodingValue && !codecStrArgument(globalObject, function, "argument 'encoding'"_s, encodingValue, encoding))
        return { };
    if (errorsValue && !codecStrArgument(globalObject, function, "argument 'errors'"_s, errorsValue, errors))
        return { };
    return isEncoder ? codecEncodeObject(globalObject, object, encoding, errors) : codecDecodeObject(globalObject, object, encoding, errors);
}

static JSValue codecModuleRegisterError(JSGlobalObject* globalObject, JSValue nameValue, JSValue handler)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name;
    if (!codecStrArgument(globalObject, "register_error"_s, "argument 1"_s, nameValue, name))
        return { };
    if (!isCallable(globalObject, handler))
        return raiseTypeError(globalObject, scope, "handler must be callable"_s);
    CodecRegistry registry;
    if (!codecGetRegistry(globalObject, registry))
        return { };
    registry.errorHandlers->set(vm, internString(vm, name), handler);
    return jsUndefined();
}

static constexpr ASCIILiteral codecBuiltinHandlerNames[] = {
    "strict"_s, "ignore"_s, "replace"_s, "xmlcharrefreplace"_s, "backslashreplace"_s, "namereplace"_s, "surrogatepass"_s, "surrogateescape"_s,
};

static constexpr NativeDef codecBuiltinHandlers[] = {
    PY_FUNCTION("strict_errors", codecStrictErrors, "(exc, /)"),
    PY_FUNCTION("ignore_errors", codecIgnoreErrors, "(exc, /)"),
    PY_FUNCTION("replace_errors", codecReplaceErrors, "(exc, /)"),
    PY_FUNCTION("xmlcharrefreplace_errors", codecXMLCharRefReplaceErrors, "(exc, /)"),
    PY_FUNCTION("backslashreplace_errors", codecBackslashReplaceErrors, "(exc, /)"),
    PY_FUNCTION("namereplace_errors", codecNameReplaceErrors, "(exc, /)"),
    PY_FUNCTION("surrogatepass", codecSurrogatePassErrors, "(exc, /)"),
    PY_FUNCTION("surrogateescape", codecSurrogateEscapeErrors, "(exc, /)"),
};
static_assert(std::size(codecBuiltinHandlerNames) == std::size(codecBuiltinHandlers));

static JSValue codecModuleUnregisterError(JSGlobalObject* globalObject, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name;
    if (!codecStrArgument(globalObject, "_unregister_error"_s, "argument"_s, nameValue, name))
        return { };
    for (ASCIILiteral builtin : codecBuiltinHandlerNames) {
        if (name == builtin)
            return raiseValueError(globalObject, scope, "cannot un-register built-in error handler '"_s, name, '\'');
    }
    CodecRegistry registry;
    if (!codecGetRegistry(globalObject, registry))
        return { };
    return jsBoolean(registry.errorHandlers->remove(vm, internString(vm, name)));
}

static JSValue codecModuleLookupError(JSGlobalObject* globalObject, JSValue nameValue)
{
    String name;
    if (!codecStrArgument(globalObject, "lookup_error"_s, "argument"_s, nameValue, name))
        return { };
    return codecLookupError(globalObject, name);
}

static JSValue codecModuleNormalizeEncoding(JSGlobalObject* globalObject, JSValue encodingValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(encodingValue))
        return raiseTypeError(globalObject, scope, "_normalize_encoding() argument 'encoding' must be str, not "_s, codecArgumentTypeName(globalObject, encodingValue));
    String encoding = strValue(encodingValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    // CPython reads a C string.
    size_t end = encoding.find(static_cast<char16_t>(0));
    return makeStr(vm, codecNormalizeEncoding(StringView(encoding).left(end == notFound ? encoding.length() : static_cast<unsigned>(end)), false));
}

// (decode(data, errors, final), how much of the data it took)
template<typename Decode>
static JSValue codecRunDecoder(JSGlobalObject* globalObject, ASCIILiteral function, JSValue data, JSValue errorsValue, JSValue finalValue, bool acceptsStr, bool isFinal, const Decode& decode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodecBuffer buffer;
    if (!buffer.initialize(globalObject, data, acceptsStr))
        return { };
    String errors;
    if (!codecErrorsArgument(globalObject, function, errorsValue, errors))
        return { };
    if (finalValue) {
        isFinal = isTrue(globalObject, finalValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    size_t consumed = buffer.span().size();
    JSValue decoded = decode(buffer.span(), errors, isFinal ? nullptr : &consumed);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { decoded, jsNumber(consumed) });
}

// (decode(data, errors, byteorder, final), how much of the data it took, the byte order found)
template<typename Decode>
static JSValue codecRunDecoderWithByteOrder(JSGlobalObject* globalObject, ASCIILiteral function, JSValue data, JSValue errorsValue, JSValue byteOrderValue, JSValue finalValue, const Decode& decode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    CodecBuffer buffer;
    if (!buffer.initialize(globalObject, data, false))
        return { };
    String errors;
    if (!codecErrorsArgument(globalObject, function, errorsValue, errors))
        return { };
    int byteOrder = 0;
    if (byteOrderValue) {
        byteOrder = codecToCInt(globalObject, byteOrderValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool isFinal = false;
    if (finalValue) {
        isFinal = isTrue(globalObject, finalValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    size_t consumed = buffer.span().size();
    JSValue decoded = decode(globalObject, buffer.span(), errors, &byteOrder, isFinal ? nullptr : &consumed);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { decoded, jsNumber(consumed), jsNumber(byteOrder) });
}

// (encode(str, errors), len(str))
template<typename Encode>
static JSValue codecRunEncoder(JSGlobalObject* globalObject, ASCIILiteral function, JSValue value, JSValue errorsValue, const Encode& encode)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value))
        return raiseTypeError(globalObject, scope, function, "() argument 1 must be str, not "_s, codecArgumentTypeName(globalObject, value));
    String errors;
    if (!codecErrorsArgument(globalObject, function, errorsValue, errors))
        return { };
    JSString* string = strValue(value);
    JSValue encoded = encode(string, errors);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned length = strLength(globalObject, string);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { encoded, jsNumber(length) });
}

#define CODEC_DEFINE_STATEFUL_DECODER(function, name, acceptsStr, isFinal, expression) \
    static JSValue function(JSGlobalObject* globalObject, JSValue data, JSValue errorsValue, JSValue finalValue) \
    { \
        return codecRunDecoder(globalObject, name ""_s, data, errorsValue, finalValue, acceptsStr, isFinal, [&](std::span<const uint8_t> bytes, const String& errors, size_t* consumed) -> JSValue { \
            return expression; \
        }); \
    }

#define CODEC_DEFINE_BYTE_ORDER_DECODER(function, name, decoder, order) \
    static JSValue function(JSGlobalObject* globalObject, JSValue data, JSValue errorsValue, JSValue finalValue) \
    { \
        return codecRunDecoder(globalObject, name ""_s, data, errorsValue, finalValue, false, false, [&](std::span<const uint8_t> bytes, const String& errors, size_t* consumed) -> JSValue { \
            int byteOrder = order; \
            return decoder(globalObject, bytes, errors, &byteOrder, consumed); \
        }); \
    }

#define CODEC_DEFINE_SIMPLE_DECODER(function, name, expression) \
    static JSValue function(JSGlobalObject* globalObject, JSValue data, JSValue errorsValue) \
    { \
        return codecRunDecoder(globalObject, name ""_s, data, errorsValue, { }, false, true, [&](std::span<const uint8_t> bytes, const String& errors, size_t*) -> JSValue { \
            UNUSED_PARAM(errors); \
            return expression; \
        }); \
    }

#define CODEC_DEFINE_ENCODER(function, name, expression) \
    static JSValue function(JSGlobalObject* globalObject, JSValue value, JSValue errorsValue) \
    { \
        return codecRunEncoder(globalObject, name ""_s, value, errorsValue, [&](JSString* string, const String& errors) -> JSValue { \
            UNUSED_PARAM(errors); \
            return expression; \
        }); \
    }

#define CODEC_DEFINE_BYTE_ORDER_ENCODER(function, name, encoder) \
    static JSValue function(JSGlobalObject* globalObject, JSValue value, JSValue errorsValue, JSValue byteOrderValue) \
    { \
        VM& vm = getVM(globalObject); \
        auto scope = DECLARE_THROW_SCOPE(vm); \
        /* The arguments are checked in order. */ \
        String ignored; \
        if (isStr(value) && !codecErrorsArgument(globalObject, name ""_s, errorsValue, ignored)) \
            return { }; \
        int byteOrder = 0; \
        if (isStr(value) && byteOrderValue) { \
            byteOrder = codecToCInt(globalObject, byteOrderValue); \
            RETURN_IF_EXCEPTION(scope, { }); \
        } \
        RELEASE_AND_RETURN(scope, codecRunEncoder(globalObject, name ""_s, value, errorsValue, [&](JSString* string, const String& errors) -> JSValue { \
            return encoder(globalObject, string, errors, byteOrder); \
        })); \
    }

CODEC_DEFINE_STATEFUL_DECODER(codecModuleUTF7Decode, "utf_7_decode", false, false, codecDecodeUTF7(globalObject, bytes, errors, consumed))
CODEC_DEFINE_STATEFUL_DECODER(codecModuleUTF8Decode, "utf_8_decode", false, false, codecDecodeUTF8(globalObject, bytes, errors, consumed))
CODEC_DEFINE_STATEFUL_DECODER(codecModuleUnicodeEscapeDecode, "unicode_escape_decode", true, true, codecDecodeUnicodeEscape(globalObject, bytes, errors, consumed))
CODEC_DEFINE_STATEFUL_DECODER(codecModuleRawUnicodeEscapeDecode, "raw_unicode_escape_decode", true, true, codecDecodeRawUnicodeEscape(globalObject, bytes, errors, consumed))
CODEC_DEFINE_BYTE_ORDER_DECODER(codecModuleUTF16Decode, "utf_16_decode", codecDecodeUTF16, 0)
CODEC_DEFINE_BYTE_ORDER_DECODER(codecModuleUTF16LEDecode, "utf_16_le_decode", codecDecodeUTF16, -1)
CODEC_DEFINE_BYTE_ORDER_DECODER(codecModuleUTF16BEDecode, "utf_16_be_decode", codecDecodeUTF16, 1)
CODEC_DEFINE_BYTE_ORDER_DECODER(codecModuleUTF32Decode, "utf_32_decode", codecDecodeUTF32, 0)
CODEC_DEFINE_BYTE_ORDER_DECODER(codecModuleUTF32LEDecode, "utf_32_le_decode", codecDecodeUTF32, -1)
CODEC_DEFINE_BYTE_ORDER_DECODER(codecModuleUTF32BEDecode, "utf_32_be_decode", codecDecodeUTF32, 1)
CODEC_DEFINE_SIMPLE_DECODER(codecModuleLatin1Decode, "latin_1_decode", codecDecodeLatin1(globalObject, bytes))
CODEC_DEFINE_SIMPLE_DECODER(codecModuleASCIIDecode, "ascii_decode", codecDecodeASCII(globalObject, bytes, errors))

CODEC_DEFINE_ENCODER(codecModuleUTF7Encode, "utf_7_encode", codecEncodeUTF7(globalObject, string, errors))
CODEC_DEFINE_ENCODER(codecModuleUTF8Encode, "utf_8_encode", codecEncodeUTF8(globalObject, string, errors))
CODEC_DEFINE_ENCODER(codecModuleUTF16LEEncode, "utf_16_le_encode", codecEncodeUTF16(globalObject, string, errors, -1))
CODEC_DEFINE_ENCODER(codecModuleUTF16BEEncode, "utf_16_be_encode", codecEncodeUTF16(globalObject, string, errors, 1))
CODEC_DEFINE_ENCODER(codecModuleUTF32LEEncode, "utf_32_le_encode", codecEncodeUTF32(globalObject, string, errors, -1))
CODEC_DEFINE_ENCODER(codecModuleUTF32BEEncode, "utf_32_be_encode", codecEncodeUTF32(globalObject, string, errors, 1))
CODEC_DEFINE_ENCODER(codecModuleUnicodeEscapeEncode, "unicode_escape_encode", codecEncodeUnicodeEscape(globalObject, string))
CODEC_DEFINE_ENCODER(codecModuleRawUnicodeEscapeEncode, "raw_unicode_escape_encode", codecEncodeRawUnicodeEscape(globalObject, string))
CODEC_DEFINE_ENCODER(codecModuleLatin1Encode, "latin_1_encode", codecEncodeLatin1(globalObject, string, errors))
CODEC_DEFINE_ENCODER(codecModuleASCIIEncode, "ascii_encode", codecEncodeASCII(globalObject, string, errors))
CODEC_DEFINE_BYTE_ORDER_ENCODER(codecModuleUTF16Encode, "utf_16_encode", codecEncodeUTF16)
CODEC_DEFINE_BYTE_ORDER_ENCODER(codecModuleUTF32Encode, "utf_32_encode", codecEncodeUTF32)

#undef CODEC_DEFINE_STATEFUL_DECODER
#undef CODEC_DEFINE_BYTE_ORDER_DECODER
#undef CODEC_DEFINE_SIMPLE_DECODER
#undef CODEC_DEFINE_ENCODER
#undef CODEC_DEFINE_BYTE_ORDER_ENCODER

static JSValue codecModuleUTF16ExDecode(JSGlobalObject* globalObject, JSValue data, JSValue errors, JSValue byteOrder, JSValue final)
{
    return codecRunDecoderWithByteOrder(globalObject, "utf_16_ex_decode"_s, data, errors, byteOrder, final, codecDecodeUTF16);
}

static JSValue codecModuleUTF32ExDecode(JSGlobalObject* globalObject, JSValue data, JSValue errors, JSValue byteOrder, JSValue final)
{
    return codecRunDecoderWithByteOrder(globalObject, "utf_32_ex_decode"_s, data, errors, byteOrder, final, codecDecodeUTF32);
}

static JSValue codecModuleCharmapDecode(JSGlobalObject* globalObject, JSValue data, JSValue errorsValue, JSValue mapping)
{
    if (mapping && isNone(mapping))
        mapping = JSValue();
    return codecRunDecoder(globalObject, "charmap_decode"_s, data, errorsValue, { }, false, true, [&](std::span<const uint8_t> bytes, const String& errors, size_t*) -> JSValue {
        return codecDecodeCharmap(globalObject, bytes, mapping, errors);
    });
}

static JSValue codecModuleCharmapEncode(JSGlobalObject* globalObject, JSValue value, JSValue errorsValue, JSValue mapping)
{
    if (mapping && isNone(mapping))
        mapping = JSValue();
    return codecRunEncoder(globalObject, "charmap_encode"_s, value, errorsValue, [&](JSString* string, const String& errors) -> JSValue {
        return codecEncodeCharmap(globalObject, string, mapping, errors);
    });
}

// The mapping charmap_encode() takes, from the str charmap_decode() takes.
// FIXME: CPython makes an EncodingMap, a trie, where it can. A dict does the same, slower.
static JSValue codecModuleCharmapBuild(JSGlobalObject* globalObject, JSValue map)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(map))
        return raiseTypeError(globalObject, scope, "charmap_build() argument must be str, not "_s, codecArgumentTypeName(globalObject, map));
    CodecText text;
    if (!text.initialize(globalObject, strValue(map)))
        return { };
    if (!text.length())
        return raiseBadInternalCall(globalObject, scope);
    unsigned length = std::min(text.length(), 256u);

    // Where CPython too makes a dict, U+FFFE is a key like any other; in an EncodingMap it stands for "undefined".
    bool isPlainDict = text[0];
    std::array<bool, 512> seenBlocks { };
    unsigned blockCount = 0;
    for (unsigned i = 1; i < length && !isPlainDict; ++i) {
        char32_t character = text[i];
        if (!character || character > 0xffff)
            isPlainDict = true;
        else if (character != 0xfffe && !seenBlocks[character >> 7]) {
            seenBlocks[character >> 7] = true;
            ++blockCount;
        }
    }
    if (blockCount >= 0xff)
        isPlainDict = true;

    PyDict* result = PyDict::create(globalObject);
    for (unsigned i = 0; i < length; ++i) {
        char32_t character = text[i];
        if (!isPlainDict && character == 0xfffe)
            continue;
        result->set(globalObject, jsNumber(static_cast<uint32_t>(character)), jsNumber(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue codecModuleReadbufferEncode(JSGlobalObject* globalObject, JSValue data, JSValue errorsValue)
{
    return codecRunDecoder(globalObject, "readbuffer_encode"_s, data, errorsValue, { }, true, true, [&](std::span<const uint8_t> bytes, const String&, size_t*) -> JSValue {
        return PyBytes::create(globalObject, bytes);
    });
}

// The escapes of a bytes literal.
static JSValue codecModuleEscapeDecode(JSGlobalObject* globalObject, JSValue data, JSValue errorsValue)
{
    return codecRunDecoder(globalObject, "escape_decode"_s, data, errorsValue, { }, true, true, [&](std::span<const uint8_t> in, const String& errors, size_t*) -> JSValue {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        ByteBuilder out;
        out.reserveInitialCapacity(in.size());
        int firstInvalidEscape = -1;
        size_t size = in.size();
        for (size_t s = 0; s < size;) {
            if (in[s] != '\\') {
                out.append(in[s++]);
                continue;
            }
            if (++s == size)
                return raiseValueError(globalObject, scope, "Trailing \\ in string"_s);
            uint8_t c = in[s++];
            switch (c) {
            case '\n':
                break;
            case '\\':
            case '\'':
            case '"':
                out.append(c);
                break;
            case 'b':
                out.append('\b');
                break;
            case 'f':
                out.append('\f');
                break;
            case 't':
                out.append('\t');
                break;
            case 'n':
                out.append('\n');
                break;
            case 'r':
                out.append('\r');
                break;
            case 'v':
                out.append('\v');
                break;
            case 'a':
                out.append('\a');
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7': {
                int value = c - '0';
                for (unsigned more = 0; more < 2 && s < size && in[s] >= '0' && in[s] <= '7'; ++more)
                    value = (value << 3) + in[s++] - '0';
                if (value > 0377 && firstInvalidEscape == -1)
                    firstInvalidEscape = value;
                out.append(static_cast<uint8_t>(value));
                break;
            }
            case 'x': {
                if (s + 1 < size) {
                    int high = codecHexDigitValue(in[s]);
                    int low = codecHexDigitValue(in[s + 1]);
                    if (high >= 0 && low >= 0) {
                        out.append(static_cast<uint8_t>((high << 4) + low));
                        s += 2;
                        break;
                    }
                }
                if (errors.isNull() || errors == "strict"_s)
                    return raiseValueError(globalObject, scope, "invalid \\x escape at position "_s, s - 2);
                if (errors == "replace"_s)
                    out.append('?');
                else if (errors != "ignore"_s)
                    return raiseValueError(globalObject, scope, "decoding error; unknown error handling code: "_s, errors);
                if (s < size && codecHexDigitValue(in[s]) >= 0)
                    ++s;
                break;
            }
            default:
                if (firstInvalidEscape == -1)
                    firstInvalidEscape = c;
                out.append('\\');
                --s;
                break;
            }
        }
        if (firstInvalidEscape != -1) {
            bool goesOn = codecWarnInvalidEscape(globalObject, firstInvalidEscape, "b"_s);
            RETURN_IF_EXCEPTION(scope, { });
            if (!goesOn)
                return { };
        }
        return makeBytes(globalObject, out);
    });
}

static JSValue codecModuleEscapeEncode(JSGlobalObject* globalObject, JSValue data, JSValue errorsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* bytes = PyBytes::tryFrom(data);
    if (!bytes)
        return raiseTypeError(globalObject, scope, "escape_encode() argument 1 must be bytes, not "_s, codecArgumentTypeName(globalObject, data));
    String errors;
    if (!codecErrorsArgument(globalObject, "escape_encode"_s, errorsValue, errors))
        return { };
    ByteBuilder out;
    out.reserveInitialCapacity(bytes->length());
    auto put = [&](char c) { out.append(static_cast<uint8_t>(c)); };
    for (uint8_t c : bytes->span()) {
        if (c == '\'' || c == '\\') {
            put('\\');
            out.append(c);
        } else if (c == '\t') {
            put('\\');
            put('t');
        } else if (c == '\n') {
            put('\\');
            put('n');
        } else if (c == '\r') {
            put('\\');
            put('r');
        } else if (c < ' ' || c >= 0x7f)
            codecWriteBackslashEscape(c, put);
        else
            out.append(c);
    }
    PyBytes* result = makeBytes(globalObject, out);
    if (!result)
        return { };
    return PyTuple::create(globalObject, { result, jsNumber(bytes->length()) });
}

static constexpr NativeDef codecModuleFunctions[] = {
    PY_FUNCTION("register", codecModuleRegister, "(search_function, /)"),
    PY_FUNCTION("unregister", codecModuleUnregister, "(search_function, /)"),
    PY_FUNCTION("lookup", codecModuleLookup, "(encoding, /)"),
    PY_FUNCTION("encode", codecModuleTranscode<true>, "(obj, encoding='utf-8', errors='strict')"),
    PY_FUNCTION("decode", codecModuleTranscode<false>, "(obj, encoding='utf-8', errors='strict')"),
    PY_FUNCTION("escape_encode", codecModuleEscapeEncode, "(data, errors=None, /)"),
    PY_FUNCTION("escape_decode", codecModuleEscapeDecode, "(data, errors=None, /)"),
    PY_FUNCTION("utf_8_encode", codecModuleUTF8Encode, "(str, errors=None, /)"),
    PY_FUNCTION("utf_8_decode", codecModuleUTF8Decode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_7_encode", codecModuleUTF7Encode, "(str, errors=None, /)"),
    PY_FUNCTION("utf_7_decode", codecModuleUTF7Decode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_16_encode", codecModuleUTF16Encode, "(str, errors=None, byteorder=0, /)"),
    PY_FUNCTION("utf_16_le_encode", codecModuleUTF16LEEncode, "(str, errors=None, /)"),
    PY_FUNCTION("utf_16_be_encode", codecModuleUTF16BEEncode, "(str, errors=None, /)"),
    PY_FUNCTION("utf_16_decode", codecModuleUTF16Decode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_16_le_decode", codecModuleUTF16LEDecode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_16_be_decode", codecModuleUTF16BEDecode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_16_ex_decode", codecModuleUTF16ExDecode, "(data, errors=None, byteorder=0, final=False, /)"),
    PY_FUNCTION("utf_32_encode", codecModuleUTF32Encode, "(str, errors=None, byteorder=0, /)"),
    PY_FUNCTION("utf_32_le_encode", codecModuleUTF32LEEncode, "(str, errors=None, /)"),
    PY_FUNCTION("utf_32_be_encode", codecModuleUTF32BEEncode, "(str, errors=None, /)"),
    PY_FUNCTION("utf_32_decode", codecModuleUTF32Decode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_32_le_decode", codecModuleUTF32LEDecode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_32_be_decode", codecModuleUTF32BEDecode, "(data, errors=None, final=False, /)"),
    PY_FUNCTION("utf_32_ex_decode", codecModuleUTF32ExDecode, "(data, errors=None, byteorder=0, final=False, /)"),
    PY_FUNCTION("unicode_escape_encode", codecModuleUnicodeEscapeEncode, "(str, errors=None, /)"),
    PY_FUNCTION("unicode_escape_decode", codecModuleUnicodeEscapeDecode, "(data, errors=None, final=True, /)"),
    PY_FUNCTION("raw_unicode_escape_encode", codecModuleRawUnicodeEscapeEncode, "(str, errors=None, /)"),
    PY_FUNCTION("raw_unicode_escape_decode", codecModuleRawUnicodeEscapeDecode, "(data, errors=None, final=True, /)"),
    PY_FUNCTION("latin_1_encode", codecModuleLatin1Encode, "(str, errors=None, /)"),
    PY_FUNCTION("latin_1_decode", codecModuleLatin1Decode, "(data, errors=None, /)"),
    PY_FUNCTION("ascii_encode", codecModuleASCIIEncode, "(str, errors=None, /)"),
    PY_FUNCTION("ascii_decode", codecModuleASCIIDecode, "(data, errors=None, /)"),
    PY_FUNCTION("charmap_encode", codecModuleCharmapEncode, "(str, errors=None, mapping=None, /)"),
    PY_FUNCTION("charmap_decode", codecModuleCharmapDecode, "(data, errors=None, mapping=None, /)"),
    PY_FUNCTION("charmap_build", codecModuleCharmapBuild, "(map, /)"),
    PY_FUNCTION("readbuffer_encode", codecModuleReadbufferEncode, "(data, errors=None, /)"),
    PY_FUNCTION("register_error", codecModuleRegisterError, "(errors, handler, /)"),
    PY_FUNCTION("_unregister_error", codecModuleUnregisterError, "(errors, /)"),
    PY_FUNCTION("lookup_error", codecModuleLookupError, "(name, /)"),
    PY_FUNCTION("_normalize_encoding", codecModuleNormalizeEncoding, "(encoding)"),
};

void initializeCodecsModule(JSGlobalObject* globalObject, PyModule* module)
{
    VM& vm = getVM(globalObject);
    module->addFunctions(globalObject, codecModuleFunctions);

    module->add(globalObject, "_search_path"_s, newList(globalObject));
    module->add(globalObject, "_cache"_s, PyDict::create(globalObject));
    PyDict* handlers = PyDict::create(globalObject);
    for (size_t i = 0; i < std::size(codecBuiltinHandlers); ++i)
        handlers->set(vm, internString(vm, codecBuiltinHandlerNames[i]), PyNativeFunction::create(globalObject, codecBuiltinHandlers[i], NativeKind::Function, { }, { }));
    module->add(globalObject, "_errors"_s, handlers);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
