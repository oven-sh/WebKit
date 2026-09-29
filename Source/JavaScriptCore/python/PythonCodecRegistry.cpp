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
#include "PythonCodecs.h"

#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonUnicodeData.h"
#include <wtf/text/StringBuilder.h>

// The registry of codecs and of error handlers, and the handlers that are built in: Python/codecs.c of CPython.

namespace JSC { namespace Python {

static void initializeErrorHandlers(JSGlobalObject*, PyDict*);

// _PyCodec_InitRegistry()
static CodecRegistryState& registry(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    CodecRegistryState& state = realm->codecRegistry();
    if (state.searchPath)
        return state;
    state.searchPath.set(vm, realm, newList(globalObject));
    state.searchCache.set(vm, realm, PyDict::create(globalObject));
    PyDict* handlers = PyDict::create(globalObject);
    state.errorRegistry.set(vm, realm, handlers);
    initializeErrorHandlers(globalObject, handlers);
    return state;
}

// ---- Codecs

void registerCodecSearchFunction(JSGlobalObject* globalObject, JSValue function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isCallable(globalObject, function)) {
        raiseTypeError(globalObject, scope, "argument must be callable"_s);
        return;
    }
    scope.release();
    listAppend(globalObject, registry(globalObject).searchPath.get(), function);
}

void unregisterCodecSearchFunction(JSGlobalObject* globalObject, JSValue function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& state = registry(globalObject);
    JSArray* path = state.searchPath.get();
    for (unsigned i = 0; i < path->length(); ++i) {
        if (path->getIndexQuickly(i) != function)
            continue;
        listRemoveRange(globalObject, path, i, 1);
        RETURN_IF_EXCEPTION(scope, void());
        state.searchCache->clear(globalObject);
        return;
    }
}

String normalizeEncodingName(const String& encoding)
{
    StringBuilder result;
    bool afterPunctuation = false;
    for (unsigned i = 0; i < encoding.length(); ++i) {
        char16_t c = encoding[i];
        if (!isASCIIAlphanumeric(c) && c != '.') {
            afterPunctuation = true;
            continue;
        }
        if (afterPunctuation && !result.isEmpty())
            result.append('_');
        afterPunctuation = false;
        result.append(toASCIILower(c));
    }
    return result.isEmpty() ? emptyString() : result.toString();
}

JSValue lookupCodec(JSGlobalObject* globalObject, const String& encoding)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = registry(globalObject);
    JSValue name = jsString(vm, normalizeEncodingName(encoding));

    JSValue result = state.searchCache->get(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;

    JSArray* path = state.searchPath.get();
    unsigned count = path->length();
    if (!count)
        return raise(globalObject, scope, BuiltinType::LookupError, "no codec search functions registered: can't find encoding"_s);
    for (unsigned i = 0; i < count; ++i) {
        // One of them may have unregistered another.
        if (i >= path->length())
            return raise(globalObject, scope, BuiltinType::IndexError, "list index out of range"_s);
        result = call(globalObject, path->getIndexQuickly(i), name);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(result)) {
            result = JSValue();
            continue;
        }
        if (!isInstance(globalObject, result, globalObject->pyRealm()->typeTuple()) || asTuple(result)->length() != 4)
            return raiseTypeError(globalObject, scope, "codec search functions must return 4-tuples"_s);
        break;
    }
    if (!result)
        return raise(globalObject, scope, BuiltinType::LookupError, concatenate("unknown encoding: "_s, encoding));
    state.searchCache->set(globalObject, name, result);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

JSValue lookupTextEncoding(JSGlobalObject* globalObject, const String& encoding, ASCIILiteral alternateCommand)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue codec = lookupCodec(globalObject, encoding);
    RETURN_IF_EXCEPTION(scope, { });
    // A plain tuple is taken to be of text, and so is anything that does not say.
    if (typeOf(globalObject, codec) == globalObject->pyRealm()->typeTuple())
        return codec;
    JSValue attribute = getAttributeIfPresent(globalObject, codec, Identifier::fromString(vm, "_is_text_encoding"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!attribute)
        return codec;
    bool isText = isTrue(globalObject, attribute);
    RETURN_IF_EXCEPTION(scope, { });
    if (isText)
        return codec;
    if (!alternateCommand.isNull())
        return raise(globalObject, scope, BuiltinType::LookupError, concatenate('\'', encoding, "' is not a text encoding; use "_s, alternateCommand, " to handle arbitrary codecs"_s));
    return raise(globalObject, scope, BuiltinType::LookupError, concatenate('\'', encoding, "' is not a text encoding"_s));
}

// codec_makeincrementalcodec()
static JSValue makeIncrementalCodec(JSGlobalObject* globalObject, JSValue codecInfo, const String& errors, ASCIILiteral attribute)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue factory = getAttribute(globalObject, codecInfo, Identifier::fromString(vm, attribute));
    RETURN_IF_EXCEPTION(scope, { });
    if (errors.isNull())
        RELEASE_AND_RETURN(scope, call(globalObject, factory));
    RELEASE_AND_RETURN(scope, call(globalObject, factory, jsString(vm, errors)));
}

JSValue makeIncrementalDecoder(JSGlobalObject* globalObject, JSValue codecInfo, const String& errors)
{
    return makeIncrementalCodec(globalObject, codecInfo, errors, "incrementaldecoder"_s);
}

JSValue makeIncrementalEncoder(JSGlobalObject* globalObject, JSValue codecInfo, const String& errors)
{
    return makeIncrementalCodec(globalObject, codecInfo, errors, "incrementalencoder"_s);
}

// _PyCodec_EncodeInternal() and _PyCodec_DecodeInternal()
static JSValue runCodec(JSGlobalObject* globalObject, JSValue object, JSValue function, const String& encoding, const String& errors, bool isEncoding)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = errors.isNull() ? call(globalObject, function, object) : call(globalObject, function, object, jsString(vm, errors));
    if (scope.exception()) {
        scope.release();
        addNoteToRaised(globalObject, concatenate(isEncoding ? "encoding"_s : "decoding"_s, " with '"_s, encoding, "' codec failed"_s));
        return { };
    }
    if (!isInstance(globalObject, result, globalObject->pyRealm()->typeTuple()) || asTuple(result)->length() != 2)
        return raiseTypeError(globalObject, scope, isEncoding ? "encoder must return a tuple (object, integer)"_s : "decoder must return a tuple (object,integer)"_s);
    // How much of it was used is neither looked at nor used.
    return asTuple(result)->at(0);
}

static JSValue runCodecNamed(JSGlobalObject* globalObject, JSValue object, const String& encoding, const String& errors, bool isEncoding, bool isForText)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue codec = isForText ? lookupTextEncoding(globalObject, encoding, isEncoding ? "codecs.encode()"_s : "codecs.decode()"_s) : lookupCodec(globalObject, encoding);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, runCodec(globalObject, object, asTuple(codec)->at(isEncoding ? 0 : 1), encoding, errors, isEncoding));
}

JSValue encodeWithCodec(JSGlobalObject* globalObject, JSValue object, const String& encoding, const String& errors) { return runCodecNamed(globalObject, object, encoding, errors, true, false); }
JSValue decodeWithCodec(JSGlobalObject* globalObject, JSValue object, const String& encoding, const String& errors) { return runCodecNamed(globalObject, object, encoding, errors, false, false); }
JSValue encodeTextWithCodec(JSGlobalObject* globalObject, JSValue object, const String& encoding, const String& errors) { return runCodecNamed(globalObject, object, encoding, errors, true, true); }
JSValue decodeTextWithCodec(JSGlobalObject* globalObject, JSValue object, const String& encoding, const String& errors) { return runCodecNamed(globalObject, object, encoding, errors, false, true); }

// ---- Error handlers

static constexpr ASCIILiteral builtinErrorHandlers[] = { "strict"_s, "ignore"_s, "replace"_s, "xmlcharrefreplace"_s, "backslashreplace"_s, "namereplace"_s, "surrogatepass"_s, "surrogateescape"_s };

void registerErrorHandler(JSGlobalObject* globalObject, const String& name, JSValue handler)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isCallable(globalObject, handler)) {
        raiseTypeError(globalObject, scope, "handler must be callable"_s);
        return;
    }
    scope.release();
    registry(globalObject).errorRegistry->setString(globalObject, name, handler);
}

std::optional<bool> unregisterErrorHandler(JSGlobalObject* globalObject, const String& name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (ASCIILiteral builtin : builtinErrorHandlers) {
        if (name == builtin) {
            raiseValueError(globalObject, scope, concatenate("cannot un-register built-in error handler '"_s, name, '\''));
            return std::nullopt;
        }
    }
    JSValue removed = registry(globalObject).errorRegistry->remove(globalObject, jsString(vm, name));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return !!removed;
}

JSValue lookupErrorHandler(JSGlobalObject* globalObject, const String& name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue handler = registry(globalObject).errorRegistry->getString(globalObject, name.isNull() ? String("strict"_s) : name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!handler)
        return raise(globalObject, scope, BuiltinType::LookupError, concatenate("unknown error handler name '"_s, name, '\''));
    return handler;
}

// ---- The handlers that are built in

enum class UnicodeErrorKind : uint8_t { Encode, Decode, Translate, Other };

static UnicodeErrorKind kindOf(JSGlobalObject* globalObject, JSValue exception)
{
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, exception);
    if (type->isSubtypeOf(realm->typeUnicodeEncodeError()))
        return UnicodeErrorKind::Encode;
    if (type->isSubtypeOf(realm->typeUnicodeDecodeError()))
        return UnicodeErrorKind::Decode;
    if (type->isSubtypeOf(realm->typeUnicodeTranslateError()))
        return UnicodeErrorKind::Translate;
    return UnicodeErrorKind::Other;
}

static JSValue raiseWrongExceptionType(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception)
{
    String type = fullyQualifiedTypeName(globalObject, exception);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseTypeError(globalObject, scope, concatenate("don't know how to handle "_s, type, " in error callback"_s));
}

// PyErr_SetObject(PyExceptionInstance_Class(exc), exc)
static JSValue raiseAgain(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception)
{
    setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
    return { };
}

// What an exception says of where the trouble is, brought within what it is about.
struct UnicodeErrorParameters {
    JSValue object;
    int64_t objectLength { 0 };
    int64_t start { 0 };
    int64_t end { 0 };
    int64_t length { 0 };
};

// as_unicode_error_attribute()
static JSValue unicodeErrorAttribute(JSGlobalObject* globalObject, ThrowScope& scope, JSValue exception, const Identifier& field, ASCIILiteral name, bool asBytes)
{
    JSValue value = asObject(exception)->getDirect(globalObject->vm(), field);
    if (!value)
        return raiseTypeError(globalObject, scope, concatenate("UnicodeError '"_s, name, "' attribute is not set"_s));
    if (asBytes ? !typeOf(globalObject, value)->hasFlag(PyType::IsBytes) : !stringIn(value))
        return raiseTypeError(globalObject, scope, concatenate("UnicodeError '"_s, name, "' attribute must be a "_s, asBytes ? "bytes"_s : "string"_s));
    return value;
}

// _PyUnicodeError_GetParams(). Nothing if it raised.
static std::optional<UnicodeErrorParameters> parametersOf(JSGlobalObject* globalObject, JSValue exception, bool asBytes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    UnicodeErrorParameters result;
    result.object = unicodeErrorAttribute(globalObject, scope, exception, names.field_subject, "object"_s, asBytes);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    int64_t size = length(globalObject, result.object);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto bound = [&] (const Identifier& field) -> int64_t {
        JSValue value = asObject(exception)->getDirect(vm, field);
        return value ? *tryInt64(value) : 0;
    };
    result.objectLength = size;
    result.start = std::max<int64_t>(bound(names.field_start), 0);
    if (result.start >= size)
        result.start = size ? size - 1 : 0;
    result.end = std::min(std::max<int64_t>(bound(names.field_end), 1), size);
    result.length = std::clamp<int64_t>(result.end - result.start, 0, size);
    return result;
}

static JSValue replacementAndPosition(JSGlobalObject* globalObject, JSValue replacement, int64_t position)
{
    return PyTuple::create(globalObject, { replacement, intFromInt64(globalObject, position) });
}

static String textOf(JSGlobalObject* globalObject, JSValue string)
{
    return stringIn(string)->value(globalObject);
}

// codec_handler_write_unicode_hex()
static void appendUnicodeHex(StringBuilder& out, char32_t c)
{
    if (c >= 0x10000)
        out.append("\\U"_s, hex(static_cast<unsigned>(c), 8, Lowercase));
    else if (c >= 0x100)
        out.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
    else
        out.append("\\x"_s, hex(static_cast<unsigned>(c), 2, Lowercase));
}

static JSValue finishReplacement(JSGlobalObject* globalObject, StringBuilder& out, int64_t position)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue text = strOrMemoryError(globalObject, out.hasOverflowed() ? String() : out.isEmpty() ? emptyString() : out.toString());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, replacementAndPosition(globalObject, text, position));
}

PYTHON_NATIVE(strictErrors)
{
    NATIVE_PROLOGUE();
    JSValue exception = args[0];
    if (!isInstance(globalObject, exception, realm->typeBaseException()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "codec must pass exception instance"_s));
    return JSValue::encode(raiseAgain(globalObject, scope, exception));
}

PYTHON_NATIVE(ignoreErrors)
{
    NATIVE_PROLOGUE();
    auto kind = kindOf(globalObject, args[0]);
    if (kind == UnicodeErrorKind::Other)
        return JSValue::encode(raiseWrongExceptionType(globalObject, scope, args[0]));
    auto parameters = parametersOf(globalObject, args[0], kind == UnicodeErrorKind::Decode);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(replacementAndPosition(globalObject, jsEmptyString(vm), parameters->end)));
}

PYTHON_NATIVE(replaceErrors)
{
    NATIVE_PROLOGUE();
    auto kind = kindOf(globalObject, args[0]);
    if (kind == UnicodeErrorKind::Other)
        return JSValue::encode(raiseWrongExceptionType(globalObject, scope, args[0]));
    auto parameters = parametersOf(globalObject, args[0], kind == UnicodeErrorKind::Decode);
    RETURN_IF_EXCEPTION(scope, { });
    StringBuilder out(OverflowPolicy::RecordOverflow);
    if (kind == UnicodeErrorKind::Decode)
        out.append(static_cast<char16_t>(0xFFFD));
    else {
        for (int64_t i = 0; i < parameters->length && !out.hasOverflowed(); ++i)
            out.append(kind == UnicodeErrorKind::Encode ? static_cast<char16_t>('?') : static_cast<char16_t>(0xFFFD));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(finishReplacement(globalObject, out, parameters->end)));
}

PYTHON_NATIVE(xmlCharRefReplaceErrors)
{
    NATIVE_PROLOGUE();
    if (kindOf(globalObject, args[0]) != UnicodeErrorKind::Encode)
        return JSValue::encode(raiseWrongExceptionType(globalObject, scope, args[0]));
    auto parameters = parametersOf(globalObject, args[0], false);
    RETURN_IF_EXCEPTION(scope, { });
    String text = textOf(globalObject, parameters->object);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints characters(text);
    StringBuilder out(OverflowPolicy::RecordOverflow);
    for (int64_t i = parameters->start; i < parameters->end; ++i)
        out.append("&#"_s, static_cast<unsigned>(characters[i]), ';');
    RELEASE_AND_RETURN(scope, JSValue::encode(finishReplacement(globalObject, out, parameters->end)));
}

PYTHON_NATIVE(backslashReplaceErrors)
{
    NATIVE_PROLOGUE();
    auto kind = kindOf(globalObject, args[0]);
    if (kind == UnicodeErrorKind::Other)
        return JSValue::encode(raiseWrongExceptionType(globalObject, scope, args[0]));
    auto parameters = parametersOf(globalObject, args[0], kind == UnicodeErrorKind::Decode);
    RETURN_IF_EXCEPTION(scope, { });
    StringBuilder out(OverflowPolicy::RecordOverflow);
    if (kind == UnicodeErrorKind::Decode) {
        auto bytes = *builtinBufferOf(parameters->object);
        for (int64_t i = parameters->start; i < parameters->end; ++i)
            out.append("\\x"_s, hex(bytes[i], 2, Lowercase));
    } else {
        String text = textOf(globalObject, parameters->object);
        RETURN_IF_EXCEPTION(scope, { });
        CodePoints characters(text);
        for (int64_t i = parameters->start; i < parameters->end; ++i)
            appendUnicodeHex(out, characters[i]);
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(finishReplacement(globalObject, out, parameters->end)));
}

PYTHON_NATIVE(nameReplaceErrors)
{
    NATIVE_PROLOGUE();
    if (kindOf(globalObject, args[0]) != UnicodeErrorKind::Encode)
        return JSValue::encode(raiseWrongExceptionType(globalObject, scope, args[0]));
    auto parameters = parametersOf(globalObject, args[0], false);
    RETURN_IF_EXCEPTION(scope, { });
    String text = textOf(globalObject, parameters->object);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints characters(text);
    StringBuilder out(OverflowPolicy::RecordOverflow);
    for (int64_t i = parameters->start; i < parameters->end; ++i) {
        String name = Unicode::nameOfCharacter(characters[i], true);
        if (name.isNull())
            appendUnicodeHex(out, characters[i]);
        else
            out.append("\\N{"_s, name, '}');
    }
    // Where to go on from is how far it got, which is where it began if that is past the end.
    RELEASE_AND_RETURN(scope, JSValue::encode(finishReplacement(globalObject, out, std::max(parameters->start, parameters->end))));
}

enum class StandardEncoding : uint8_t { Unknown, UTF8, UTF16BE, UTF16LE, UTF32BE, UTF32LE };

// get_standard_encoding_impl()
static StandardEncoding standardEncodingNamed(const String& name, unsigned& bytesForEach)
{
    auto at = [&] (unsigned i) -> char16_t { return i < name.length() ? name[i] : 0; };
    if (name == "cp65001"_s) {
        bytesForEach = 3;
        return StandardEncoding::UTF8;
    }
    if (toASCIILower(at(0)) != 'u' || toASCIILower(at(1)) != 't' || toASCIILower(at(2)) != 'f')
        return StandardEncoding::Unknown;
    unsigned i = 3;
    if (at(i) == '-' || at(i) == '_')
        ++i;
    if (at(i) == '8' && !at(i + 1) && i + 1 == name.length()) {
        bytesForEach = 3;
        return StandardEncoding::UTF8;
    }
    bool is16 = at(i) == '1' && at(i + 1) == '6';
    bool is32 = at(i) == '3' && at(i + 1) == '2';
    if (!is16 && !is32)
        return StandardEncoding::Unknown;
    i += 2;
    bytesForEach = is16 ? 2 : 4;
    if (i == name.length())
        return is16 ? StandardEncoding::UTF16LE : StandardEncoding::UTF32LE;
    if (at(i) == '-' || at(i) == '_')
        ++i;
    if (toASCIILower(at(i + 1)) == 'e' && i + 2 == name.length()) {
        if (toASCIILower(at(i)) == 'b')
            return is16 ? StandardEncoding::UTF16BE : StandardEncoding::UTF32BE;
        if (toASCIILower(at(i)) == 'l')
            return is16 ? StandardEncoding::UTF16LE : StandardEncoding::UTF32LE;
    }
    return StandardEncoding::Unknown;
}

PYTHON_NATIVE(surrogatePassErrors)
{
    NATIVE_PROLOGUE();
    JSValue exception = args[0];
    auto kind = kindOf(globalObject, exception);
    if (kind != UnicodeErrorKind::Encode && kind != UnicodeErrorKind::Decode)
        return JSValue::encode(raiseWrongExceptionType(globalObject, scope, exception));
    JSValue encodingValue = unicodeErrorAttribute(globalObject, scope, exception, names.field_encoding, "encoding"_s, false);
    RETURN_IF_EXCEPTION(scope, { });
    String encodingName = textOf(globalObject, encodingValue);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned bytesForEach = 0;
    auto encoding = standardEncodingNamed(encodingName, bytesForEach);
    if (encoding == StandardEncoding::Unknown)
        return JSValue::encode(raiseAgain(globalObject, scope, exception));
    auto parameters = parametersOf(globalObject, exception, kind == UnicodeErrorKind::Decode);
    RETURN_IF_EXCEPTION(scope, { });

    if (kind == UnicodeErrorKind::Encode) {
        String text = textOf(globalObject, parameters->object);
        RETURN_IF_EXCEPTION(scope, { });
        CodePoints characters(text);
        ByteVector out;
        for (int64_t i = parameters->start; i < parameters->end; ++i) {
            char32_t c = characters[i];
            if (!U_IS_SURROGATE(c))
                return JSValue::encode(raiseAgain(globalObject, scope, exception));
            auto byte = [&] (unsigned shift) { return static_cast<uint8_t>(c >> shift); };
            switch (encoding) {
            case StandardEncoding::UTF8:
                out.appendList({ static_cast<uint8_t>(0xE0 | (c >> 12)), static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F)), static_cast<uint8_t>(0x80 | (c & 0x3F)) });
                break;
            case StandardEncoding::UTF16LE:
                out.appendList({ byte(0), byte(8) });
                break;
            case StandardEncoding::UTF16BE:
                out.appendList({ byte(8), byte(0) });
                break;
            case StandardEncoding::UTF32LE:
                out.appendList({ byte(0), byte(8), byte(16), byte(24) });
                break;
            case StandardEncoding::UTF32BE:
                out.appendList({ byte(24), byte(16), byte(8), byte(0) });
                break;
            case StandardEncoding::Unknown:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        JSValue bytes = newBytes(globalObject, out);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(replacementAndPosition(globalObject, bytes, parameters->end)));
    }

    // One surrogate is decoded. If there are more, the codec will be back.
    char32_t c = 0;
    auto bytes = *builtinBufferOf(parameters->object);
    if (parameters->objectLength - parameters->start >= bytesForEach) {
        auto p = bytes.subspan(static_cast<size_t>(parameters->start));
        switch (encoding) {
        case StandardEncoding::UTF8:
            if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
                c = ((p[0] & 0x0F) << 12) + ((p[1] & 0x3F) << 6) + (p[2] & 0x3F);
            break;
        case StandardEncoding::UTF16LE:
            c = p[1] << 8 | p[0];
            break;
        case StandardEncoding::UTF16BE:
            c = p[0] << 8 | p[1];
            break;
        case StandardEncoding::UTF32LE:
            c = (static_cast<uint32_t>(p[3]) << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
            break;
        case StandardEncoding::UTF32BE:
            c = (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            break;
        case StandardEncoding::Unknown:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    if (!U_IS_SURROGATE(c))
        return JSValue::encode(raiseAgain(globalObject, scope, exception));
    char16_t unit = static_cast<char16_t>(c);
    RELEASE_AND_RETURN(scope, JSValue::encode(replacementAndPosition(globalObject, jsString(vm, String(std::span<const char16_t>(&unit, 1))), parameters->start + bytesForEach)));
}

PYTHON_NATIVE(surrogateEscapeErrors)
{
    NATIVE_PROLOGUE();
    JSValue exception = args[0];
    auto kind = kindOf(globalObject, exception);
    if (kind != UnicodeErrorKind::Encode && kind != UnicodeErrorKind::Decode)
        return JSValue::encode(raiseWrongExceptionType(globalObject, scope, exception));
    auto parameters = parametersOf(globalObject, exception, kind == UnicodeErrorKind::Decode);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind == UnicodeErrorKind::Encode) {
        String text = textOf(globalObject, parameters->object);
        RETURN_IF_EXCEPTION(scope, { });
        CodePoints characters(text);
        ByteVector out;
        for (int64_t i = parameters->start; i < parameters->end; ++i) {
            char32_t c = characters[i];
            // It is not a byte that was escaped.
            if (c < 0xDC80 || c > 0xDCFF)
                return JSValue::encode(raiseAgain(globalObject, scope, exception));
            out.append(static_cast<uint8_t>(c - 0xDC00));
        }
        JSValue bytes = newBytes(globalObject, out);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(replacementAndPosition(globalObject, bytes, parameters->end)));
    }
    auto bytes = *builtinBufferOf(parameters->object);
    std::array<char16_t, 4> units;
    int64_t consumed = 0;
    while (consumed < 4 && consumed < parameters->length) {
        uint8_t byte = bytes[parameters->start + consumed];
        // ASCII is not escaped.
        if (byte < 128)
            break;
        units[consumed++] = 0xDC00 + byte;
    }
    if (!consumed)
        return JSValue::encode(raiseAgain(globalObject, scope, exception));
    RELEASE_AND_RETURN(scope, JSValue::encode(replacementAndPosition(globalObject, jsString(vm, String(std::span<const char16_t>(units).first(consumed))), parameters->start + consumed)));
}

static void initializeErrorHandlers(JSGlobalObject* globalObject, PyDict* handlers)
{
    VM& vm = globalObject->vm();
    struct Handler {
        ASCIILiteral name;
        ASCIILiteral functionName;
        NativeFunction function;
    };
    static constexpr Handler table[] = {
        { "strict"_s, "strict_errors"_s, strictErrors },
        { "ignore"_s, "ignore_errors"_s, ignoreErrors },
        { "replace"_s, "replace_errors"_s, replaceErrors },
        { "xmlcharrefreplace"_s, "xmlcharrefreplace_errors"_s, xmlCharRefReplaceErrors },
        { "backslashreplace"_s, "backslashreplace_errors"_s, backslashReplaceErrors },
        { "namereplace"_s, "namereplace_errors"_s, nameReplaceErrors },
        { "surrogatepass"_s, "surrogatepass"_s, surrogatePassErrors },
        { "surrogateescape"_s, "surrogateescape"_s, surrogateEscapeErrors },
    };
    // They belong to no module.
    for (auto& handler : table)
        handlers->setString(globalObject, handler.name, PyNativeFunction::create(vm, globalObject, 1, handler.functionName, handler.function, PyNativeFunction::Kind::Function, nullptr, 0, ImplementationVisibility::Public, "($module, exc, /)"_s));
}

} } // namespace JSC::Python
