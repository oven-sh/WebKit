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
#include "PythonMultibyteCodec.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonIO.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"

// The module _multibytecodec: Modules/cjkcodecs/multibytecodec.c of CPython. It is what runs the codecs for Chinese, Japanese and Korean, which the modules _codecs_cn and the rest have. What is in Lib/encodings for each of
// them is a few lines over this.
//
// A handler of errors is any function, and can do anything, which includes changing what is being decoded. That is seen, as it is in CPython. There it cannot be made longer or shorter meanwhile, since somebody is looking at
// it. Nobody is counted here, so it is asked afterwards where it is and how much of it there is.

namespace JSC { namespace Python {

namespace {

using Kind = PyNativeFunction::Kind;

constexpr int MBENC_RESET = MBENC_MAX << 1; // It is the end of what is being encoded.
constexpr unsigned MAXENCPENDING = 2;
constexpr unsigned MAXDECPENDING = 8;

// module_state
struct FrameworkState final : NativeState {
    PYTHON_NATIVE_STATE(FrameworkState);
    WriteBarrier<PyType> codecType;
    WriteBarrier<PyType> encoderType;
    WriteBarrier<PyType> decoderType;
    WriteBarrier<PyType> readerType;
    WriteBarrier<PyType> writerType;
};

template<typename Visitor> void FrameworkState::visit(Visitor& visitor)
{
    visitor.append(codecType);
    visitor.append(encoderType);
    visitor.append(decoderType);
    visitor.append(readerType);
    visitor.append(writerType);
}

FrameworkState& frameworkState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<FrameworkState>(); }

// A MultibyteCodec. What it points at is the realm's, and lasts as long.
struct CodecState final : NativeState {
    PYTHON_NATIVE_STATE(CodecState);
    explicit CodecState(const MultibyteCodec* codec)
        : codec(codec)
    {
    }
    const MultibyteCodec* codec;
};

template<typename Visitor> void CodecState::visit(Visitor&) { }

// What is to be done about what cannot be encoded or decoded: ERROR_STRICT and the rest, or the name of a handler.
struct Errors {
    enum class Way : uint8_t { Strict, Ignore, Replace, Custom };
    Way way { Way::Strict };
    JSValue name; // A str, if it is Custom
};

// internal_error_callback(). A null String is "strict".
Errors errorsNamed(VM& vm, const String& name)
{
    if (name.isNull() || name == "strict"_s)
        return { Errors::Way::Strict, { } };
    if (name == "ignore"_s)
        return { Errors::Way::Ignore, { } };
    if (name == "replace"_s)
        return { Errors::Way::Replace, { } };
    return { Errors::Way::Custom, jsString(vm, name) };
}

// A MultibyteIncrementalEncoder, MultibyteIncrementalDecoder, MultibyteStreamReader or MultibyteStreamWriter. In CPython they begin alike, and what is written for one that begins so is given any of them.
struct Context final : NativeState {
    PYTHON_NATIVE_STATE(Context);
    Errors errors() const { return { way, errorsName.get() }; }
    void setErrors(VM& vm, JSCell* owner, const Errors& errors)
    {
        way = errors.way;
        setOrClear(vm, owner, errorsName, errors.name);
    }
    static void setOrClear(VM& vm, JSCell* owner, WriteBarrier<Unknown>& slot, JSValue value)
    {
        if (value)
            slot.set(vm, owner, value);
        else
            slot.clear();
    }

    const MultibyteCodec* codec { nullptr };
    MultibyteCodec_State state { };
    Errors::Way way { Errors::Way::Strict };
    WriteBarrier<Unknown> errorsName;
    // What has been given to encode and cannot be until it is seen what follows: a str, or empty
    WriteBarrier<Unknown> pendingText;
    // The same, to decode
    unsigned char pending[MAXDECPENDING];
    Py_ssize_t pendingSize { 0 };
    WriteBarrier<Unknown> stream;
};

template<typename Visitor> void Context::visit(Visitor& visitor)
{
    visitor.append(errorsName);
    visitor.append(pendingText);
    visitor.append(stream);
}

// MultibyteEncodeBuffer
struct EncodeBuffer {
    EncodeBuffer(JSValue text, const CodePoints& characters)
        : text(text)
        , characters(characters)
        , inlen(characters.size())
    {
    }
    Py_ssize_t room() const { return outbufEnd - outbuf; }

    JSValue text;
    const CodePoints& characters;
    Py_ssize_t inpos { 0 };
    Py_ssize_t inlen;
    Vector<uint8_t> out;
    unsigned char* outbuf { nullptr };
    unsigned char* outbufEnd { nullptr };
    JSValue exception;
};

// MultibyteDecodeBuffer
struct DecodeBuffer {
    // decoder_prepare_buffer()
    void prepare(std::span<const uint8_t> input)
    {
        inbuf = inbufTop = input.data();
        inbufEnd = inbufTop + input.size();
    }
    // The same, of what a program has hold of and can change.
    void prepare(const Buffer& input)
    {
        live = &input;
        prepare(input.span());
    }
    // After anything of the program's has been run
    void lookAgain()
    {
        if (!live)
            return;
        size_t position = inbuf - inbufTop;
        auto input = live->span();
        inbufTop = input.data();
        inbufEnd = inbufTop + input.size();
        inbuf = inbufTop + std::min(position, input.size());
    }
    std::span<const uint8_t> all() const { return { inbufTop, inbufEnd }; }
    bool hasMore() const { return inbuf < inbufEnd; }

    const unsigned char* inbuf { nullptr };
    const unsigned char* inbufTop { nullptr };
    const unsigned char* inbufEnd { nullptr };
    const Buffer* live { nullptr };
    JSValue exception;
    TextWriter writer;
};

void raiseInstance(JSGlobalObject* globalObject, JSValue exception)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
}

// Empty if it raised.
JSValue newEncodeError(JSGlobalObject* globalObject, const MultibyteCodec* codec, JSValue text, Py_ssize_t start, Py_ssize_t end, ASCIILiteral reason)
{
    VM& vm = globalObject->vm();
    MarkedArgumentBuffer arguments;
    arguments.append(jsString(vm, String::fromLatin1(codec->encoding)));
    arguments.append(text);
    arguments.append(intFromInt64(globalObject, start));
    arguments.append(intFromInt64(globalObject, end));
    arguments.append(jsString(vm, String(reason)));
    return call(globalObject, globalObject->pyRealm()->typeUnicodeEncodeError(), arguments);
}

// PyUnicodeDecodeError_Create(). Empty if it raised.
JSValue newDecodeError(JSGlobalObject* globalObject, const MultibyteCodec* codec, std::span<const uint8_t> input, Py_ssize_t start, Py_ssize_t end, ASCIILiteral reason)
{
    VM& vm = globalObject->vm();
    MarkedArgumentBuffer arguments;
    arguments.append(jsString(vm, String::fromLatin1(codec->encoding)));
    arguments.append(newBytes(globalObject, input));
    arguments.append(intFromInt64(globalObject, start));
    arguments.append(intFromInt64(globalObject, end));
    arguments.append(jsString(vm, String(reason)));
    return call(globalObject, globalObject->pyRealm()->typeUnicodeDecodeError(), arguments);
}

// call_error_callback(). Empty if it raised.
JSValue callErrorHandler(JSGlobalObject* globalObject, JSValue name, JSValue exception)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto text = toTextArgument(globalObject, name, { }, { });
    RETURN_IF_EXCEPTION(scope, { });
    JSValue handler = lookupErrorHandler(globalObject, *text);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, handler, exception));
}

bool isIntObject(JSValue value)
{
    if (auto* boxed = tryBoxedValue(value))
        value = boxed->value();
    return value.isBoolean() || isInt(value);
}

// Where a handler says to go on from, which is counted from the end if it is less than nothing. Nothing if it raised.
std::optional<Py_ssize_t> positionFromHandler(JSGlobalObject* globalObject, JSValue given, Py_ssize_t length)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Py_ssize_t position = -1;
    auto converted = toSsize(globalObject, given);
    if (scope.exception()) [[unlikely]]
        (void)scope.tryClearException();
    else {
        position = *converted;
        if (position < 0)
            position += length;
    }
    if (position < 0 || position > length) {
        raise(globalObject, scope, BuiltinType::IndexError, concatenate("position "_s, static_cast<int64_t>(position), " from error handler out of bounds"_s));
        return std::nullopt;
    }
    return position;
}

// expand_encodebuffer(), by at least `size` if that is more than nothing.
void expand(JSGlobalObject* globalObject, EncodeBuffer& buffer, Py_ssize_t size)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    size_t position = buffer.outbuf - buffer.out.mutableSpan().data();
    Py_ssize_t original = buffer.out.size();
    Py_ssize_t more = size < (original >> 1) ? (original >> 1) | 1 : size;
    if (original > std::numeric_limits<Py_ssize_t>::max() - more || !buffer.out.tryGrow(original + more)) {
        raiseMemoryError(globalObject, scope);
        return;
    }
    buffer.outbuf = buffer.out.mutableSpan().data() + position;
    buffer.outbufEnd = buffer.out.mutableSpan().data() + buffer.out.size();
}

// REQUIRE_ENCODEBUFFER()
void requireRoom(JSGlobalObject* globalObject, EncodeBuffer& buffer, Py_ssize_t size)
{
    if (size < 0 || size > buffer.room())
        expand(globalObject, buffer, size);
}

std::optional<Vector<uint8_t>> encode(JSGlobalObject*, const MultibyteCodec*, MultibyteCodec_State*, JSValue text, Py_ssize_t* consumed, const Errors&, int flags);

// multibytecodec_encerror(): what is done about what the codec returned, which is not nothing.
void handleEncodeError(JSGlobalObject* globalObject, const MultibyteCodec* codec, MultibyteCodec_State* state, EncodeBuffer& buffer, const Errors& errors, Py_ssize_t e)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ASCIILiteral reason;
    Py_ssize_t size;
    if (e > 0) {
        reason = "illegal multibyte sequence"_s;
        size = e;
    } else {
        switch (e) {
        case MBERR_TOOSMALL:
            // It is tried again.
            RELEASE_AND_RETURN(scope, requireRoom(globalObject, buffer, -1));
        case MBERR_TOOFEW:
            reason = "incomplete multibyte sequence"_s;
            size = buffer.inpos;
            break;
        case MBERR_INTERNAL:
            raise(globalObject, scope, BuiltinType::RuntimeError, "internal codec error"_s);
            return;
        default:
            raise(globalObject, scope, BuiltinType::RuntimeError, "unknown runtime error"_s);
            return;
        }
    }

    if (errors.way == Errors::Way::Replace) {
        CodePoints questionMark("?"_s);
        Py_ssize_t position = 0;
        Py_ssize_t result;
        while (true) {
            result = codec->encode(state, codec, &questionMark, &position, 1, &buffer.outbuf, buffer.room(), 0);
            if (result != MBERR_TOOSMALL)
                break;
            requireRoom(globalObject, buffer, -1);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (result) {
            requireRoom(globalObject, buffer, 1);
            RETURN_IF_EXCEPTION(scope, void());
            *buffer.outbuf++ = '?';
        }
    }
    if (errors.way == Errors::Way::Ignore || errors.way == Errors::Way::Replace) {
        buffer.inpos += size;
        return;
    }

    Py_ssize_t start = buffer.inpos;
    Py_ssize_t end = start + size;
    // There is one exception, which is told each time where the trouble is now.
    if (!buffer.exception) {
        buffer.exception = newEncodeError(globalObject, codec, buffer.text, start, end, reason);
        RETURN_IF_EXCEPTION(scope, void());
    } else
        setWhereUnicodeErrorIs(globalObject, buffer.exception, start, end, reason);

    if (errors.way == Errors::Way::Strict)
        RELEASE_AND_RETURN(scope, raiseInstance(globalObject, buffer.exception));

    JSValue result = callErrorHandler(globalObject, errors.name, buffer.exception);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue replacement = isTuple(result) && asTuple(result)->length() == 2 ? asTuple(result)->at(0) : JSValue();
    if (!replacement || (!stringIn(replacement) && !isBytes(replacement)) || !isIntObject(asTuple(result)->at(1))) {
        raiseTypeError(globalObject, scope, "encoding error handler must return (str, int) tuple"_s);
        return;
    }

    auto write = [&](std::span<const uint8_t> bytes) {
        if (bytes.empty())
            return;
        requireRoom(globalObject, buffer, bytes.size());
        RETURN_IF_EXCEPTION(scope, void());
        memcpy(buffer.outbuf, bytes.data(), bytes.size());
        buffer.outbuf += bytes.size();
    };
    if (stringIn(replacement)) {
        auto encoded = encode(globalObject, codec, state, replacement, nullptr, { }, MBENC_FLUSH);
        RETURN_IF_EXCEPTION(scope, void());
        write(encoded->span());
    } else
        write(*builtinBufferOf(replacement));
    RETURN_IF_EXCEPTION(scope, void());

    auto position = positionFromHandler(globalObject, asTuple(result)->at(1), buffer.inlen);
    RETURN_IF_EXCEPTION(scope, void());
    buffer.inpos = *position;
}

// multibytecodec_decerror()
void handleDecodeError(JSGlobalObject* globalObject, const MultibyteCodec* codec, DecodeBuffer& buffer, const Errors& errors, Py_ssize_t e)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ASCIILiteral reason;
    Py_ssize_t size;
    if (e > 0) {
        reason = "illegal multibyte sequence"_s;
        size = e;
    } else {
        switch (e) {
        case MBERR_TOOSMALL:
            return;
        case MBERR_TOOFEW:
            reason = "incomplete multibyte sequence"_s;
            size = buffer.inbufEnd - buffer.inbuf;
            break;
        case MBERR_INTERNAL:
            raise(globalObject, scope, BuiltinType::RuntimeError, "internal codec error"_s);
            return;
        default:
            raise(globalObject, scope, BuiltinType::RuntimeError, "unknown runtime error"_s);
            return;
        }
    }

    if (errors.way == Errors::Way::Replace)
        buffer.writer.append(static_cast<char32_t>(0xFFFD));
    if (errors.way == Errors::Way::Ignore || errors.way == Errors::Way::Replace) {
        buffer.inbuf += size;
        return;
    }

    Py_ssize_t start = buffer.inbuf - buffer.inbufTop;
    Py_ssize_t end = start + size;
    if (!buffer.exception) {
        buffer.exception = newDecodeError(globalObject, codec, buffer.all(), start, end, reason);
        RETURN_IF_EXCEPTION(scope, void());
    } else
        setWhereUnicodeErrorIs(globalObject, buffer.exception, start, end, reason);

    if (errors.way == Errors::Way::Strict)
        RELEASE_AND_RETURN(scope, raiseInstance(globalObject, buffer.exception));

    JSValue result = callErrorHandler(globalObject, errors.name, buffer.exception);
    RETURN_IF_EXCEPTION(scope, void());
    buffer.lookAgain();
    if (!isTuple(result) || asTuple(result)->length() != 2 || !stringIn(asTuple(result)->at(0)) || !isIntObject(asTuple(result)->at(1))) {
        raiseTypeError(globalObject, scope, "decoding error handler must return (str, int) tuple"_s);
        return;
    }
    buffer.writer.append(textOfString(globalObject, asTuple(result)->at(0)));
    RETURN_IF_EXCEPTION(scope, void());
    auto position = positionFromHandler(globalObject, asTuple(result)->at(1), buffer.inbufEnd - buffer.inbufTop);
    RETURN_IF_EXCEPTION(scope, void());
    buffer.inbuf = buffer.inbufTop + *position;
}

// multibytecodec_encode(). `text` is a str. Nothing if it raised.
std::optional<Vector<uint8_t>> encode(JSGlobalObject* globalObject, const MultibyteCodec* codec, MultibyteCodec_State* state, JSValue text, Py_ssize_t* consumed, const Errors& errors, int flags)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    CodePoints characters(textOfString(globalObject, text));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!characters.size() && !(flags & MBENC_RESET))
        return Vector<uint8_t> { };

    EncodeBuffer buffer(text, characters);
    if (buffer.inlen > (std::numeric_limits<Py_ssize_t>::max() - 16) / 2 || !buffer.out.tryGrow(buffer.inlen * 2 + 16)) {
        raiseMemoryError(globalObject, scope);
        return std::nullopt;
    }
    buffer.outbuf = buffer.out.mutableSpan().data();
    buffer.outbufEnd = buffer.outbuf + buffer.out.size();

    while (buffer.inpos < buffer.inlen) {
        // A handler can go on from anywhere, so how much is left is worked out each time.
        Py_ssize_t result = codec->encode(state, codec, &characters, &buffer.inpos, buffer.inlen, &buffer.outbuf, buffer.room(), flags);
        if (!result || (result == MBERR_TOOFEW && !(flags & MBENC_FLUSH)))
            break;
        handleEncodeError(globalObject, codec, state, buffer, errors, result);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (result == MBERR_TOOFEW)
            break;
    }

    if (codec->encreset && (flags & MBENC_RESET)) {
        while (true) {
            Py_ssize_t result = codec->encreset(state, codec, &buffer.outbuf, buffer.room());
            if (!result)
                break;
            handleEncodeError(globalObject, codec, state, buffer, errors, result);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
        }
    }

    buffer.out.shrink(buffer.outbuf - buffer.out.mutableSpan().data());
    if (consumed)
        *consumed = buffer.inpos;
    return WTF::move(buffer.out);
}

// What encode() is given is made a str of, if it is not one. Empty if it raised.
JSValue textToEncode(JSGlobalObject* globalObject, JSValue input)
{
    if (stringIn(input))
        return input;
    return strObject(globalObject, input);
}

// The `errors` of a method that Argument Clinic wrote, which may be None. Nothing if it raised.
std::optional<Errors> errorsArgument(JSGlobalObject* globalObject, JSValue given, ASCIILiteral function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!given || isNone(given))
        return Errors { };
    auto name = toTextArgument(globalObject, given, function, "argument 'errors'"_s, true);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return errorsNamed(vm, *name);
}

// ---- MultibyteCodec

PYTHON_NATIVE(codecEncode)
{
    NATIVE_PROLOGUE();
    const MultibyteCodec* codec = stateOf<CodecState>(args[0]).codec;
    auto errors = errorsArgument(globalObject, args.at(2), "encode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue text = textToEncode(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t length = stringLength(globalObject, stringIn(text));
    MultibyteCodec_State state;
    if (codec->encinit)
        codec->encinit(&state, codec);
    auto encoded = encode(globalObject, codec, &state, text, nullptr, *errors, MBENC_FLUSH | MBENC_RESET);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { newBytes(globalObject, encoded->span()), intFromInt64(globalObject, length) }));
}

// decoder_feed_buffer(), which for one that keeps what is left over stops where there are too few.
void feed(JSGlobalObject* globalObject, const MultibyteCodec* codec, MultibyteCodec_State* state, DecodeBuffer& buffer, const Errors& errors, bool keepsWhatIsLeft)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    while (buffer.hasMore()) {
        Py_ssize_t result = codec->decode(state, codec, &buffer.inbuf, buffer.inbufEnd - buffer.inbuf, &buffer.writer);
        if (!result || (keepsWhatIsLeft && result == MBERR_TOOFEW))
            return;
        handleDecodeError(globalObject, codec, buffer, errors, result);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

PYTHON_NATIVE(codecDecode)
{
    NATIVE_PROLOGUE();
    const MultibyteCodec* codec = stateOf<CodecState>(args[0]).codec;
    Buffer input = bufferOf(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    auto errors = errorsArgument(globalObject, args.at(2), "decode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (input.empty())
        return JSValue::encode(PyTuple::create(globalObject, { jsEmptyString(vm), jsNumber(0) }));
    size_t length = input.size();
    DecodeBuffer buffer;
    buffer.prepare(input);
    MultibyteCodec_State state;
    if (codec->decinit)
        codec->decinit(&state, codec);
    feed(globalObject, codec, &state, buffer, *errors, false);
    RETURN_IF_EXCEPTION(scope, { });
    String text = buffer.writer.finish(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { jsString(vm, text), intFromUInt64(globalObject, length) }));
}

// ---- What the four that keep something from one call to the next have in common

// The beginning of mbiencoder_new() and the rest: one of the class, for the codec that the class says is its. Null if it raised.
PyStateObject* newContext(JSGlobalObject* globalObject, PyType* type, JSValue errorsGiven, ASCIILiteral function, unsigned position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String errorsName;
    if (errorsGiven) {
        auto name = toTextArgument(globalObject, errorsGiven, function, position == 1 ? "argument 1"_s : "argument 2"_s);
        RETURN_IF_EXCEPTION(scope, nullptr);
        errorsName = *name;
    }
    JSValue codec = getAttribute(globalObject, type->object(), Identifier::fromString(vm, "codec"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (typeOf(globalObject, codec) != frameworkState(globalObject).codecType.get()) {
        raiseTypeError(globalObject, scope, "codec is unexpected type"_s);
        return nullptr;
    }
    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<Context>());
    auto& self = object->state<Context>();
    self.codec = stateOf<CodecState>(codec).codec;
    self.setErrors(vm, object, errorsNamed(vm, errorsName));
    return object;
}

// mbiencoder_init() and the rest, which take whatever they are given and do nothing with it
PYTHON_NATIVE(contextInit)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    RETURN_NONE();
}

// codecctx_errors_get()
JSValue contextErrors(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = globalObject->vm();
    Context& self = stateOf<Context>(object);
    switch (self.way) {
    case Errors::Way::Strict:
        return jsNontrivialString(vm, "strict"_s);
    case Errors::Way::Ignore:
        return jsNontrivialString(vm, "ignore"_s);
    case Errors::Way::Replace:
        return jsNontrivialString(vm, "replace"_s);
    case Errors::Way::Custom:
        break;
    }
    return self.errorsName.get();
}

// codecctx_errors_set()
void setContextErrors(JSGlobalObject* globalObject, JSValue object, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raise(globalObject, scope, BuiltinType::AttributeError, "cannot delete attribute"_s);
        return;
    }
    if (!stringIn(value)) {
        raiseTypeError(globalObject, scope, "errors must be a string"_s);
        return;
    }
    // PyUnicode_AsUTF8(), which stops at nothing
    auto encoded = encodeUTF8(globalObject, value, { });
    RETURN_IF_EXCEPTION(scope, void());
    auto bytes = encoded->span();
    if (size_t end = find(bytes, static_cast<uint8_t>(0)); end != notFound)
        bytes = bytes.first(end);
    stateOf<Context>(object).setErrors(vm, object.asCell(), errorsNamed(vm, String::fromUTF8(bytes)));
}

// _PyLong_FromByteArray(), least first, of what is not less than nothing
JSValue intFromLittleEndian(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    Vector<uint64_t, 4> digits;
    digits.grow((bytes.size() + 7) / 8);
    digits.fill(0);
    for (size_t i = 0; i < bytes.size(); ++i)
        digits[i / 8] |= static_cast<uint64_t>(bytes[i]) << (i % 8 * 8);
    return intFromDigits(globalObject, digits.span(), false);
}

// _PyLong_AsByteArray(), the same way about
void littleEndianFromInt(JSGlobalObject* globalObject, JSValue value, std::span<uint8_t> bytes)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    zeroSpan(bytes);
    Number number = classify(value);
    size_t written = 0;
    auto put = [&](uint64_t digit) {
        for (unsigned k = 0; k < 8; ++k, ++written) {
            auto byte = static_cast<uint8_t>(digit >> (8 * k));
            if (written < bytes.size())
                bytes[written] = byte;
            else if (byte)
                return false;
        }
        return true;
    };
    bool isNegative;
    bool fits = true;
    if (number.kind == Number::Kind::Small) {
        isNegative = number.small < 0;
        fits = isNegative || put(number.small);
    } else {
        isNegative = number.big->sign();
        for (unsigned i = 0; fits && i < number.big->length(); ++i)
            fits = put(number.big->digit(i));
    }
    if (isNegative)
        raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative int to unsigned"_s);
    else if (!fits)
        raise(globalObject, scope, BuiltinType::OverflowError, "int too big to convert"_s);
}

// encoder_encode_stateful(). Nothing if it raised.
std::optional<Vector<uint8_t>> encodeKeepingWhatIsLeft(JSGlobalObject* globalObject, JSCell* owner, Context& self, JSValue input, bool isFinal)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue text = textToEncode(globalObject, input);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    JSValue original = self.pendingText.get();
    if (original) {
        text = jsString(globalObject, asString(original), stringIn(text));
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        self.pendingText.clear();
    }
    Py_ssize_t length = stringLength(globalObject, stringIn(text));
    Py_ssize_t position = 0;
    auto encoded = encode(globalObject, self.codec, &self.state, text, &position, self.errors(), isFinal ? MBENC_FLUSH | MBENC_RESET : 0);
    if (scope.exception()) [[unlikely]] {
        // What there was before is put back.
        Context::setOrClear(vm, owner, self.pendingText, original);
        return std::nullopt;
    }
    if (position < length) {
        if (length - position > static_cast<Py_ssize_t>(MAXENCPENDING)) {
            // None of the codecs that there are comes to this.
            JSValue exception = newEncodeError(globalObject, self.codec, text, position, length, "pending buffer overflow"_s);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            scope.release();
            raiseInstance(globalObject, exception);
            return std::nullopt;
        }
        CodePoints characters(textOfString(globalObject, text));
        TextWriter rest;
        for (Py_ssize_t i = position; i < length; ++i)
            rest.append(characters[i]);
        String kept = rest.finish(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        self.pendingText.set(vm, owner, jsString(vm, kept));
    }
    return encoded;
}

// decoder_append_pending()
void keepWhatIsLeft(JSGlobalObject* globalObject, Context& self, DecodeBuffer& buffer)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Py_ssize_t left = buffer.inbufEnd - buffer.inbuf;
    if (left + self.pendingSize > static_cast<Py_ssize_t>(MAXDECPENDING)) {
        JSValue exception = newDecodeError(globalObject, self.codec, buffer.all(), 0, buffer.all().size(), "pending buffer overflow"_s);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, raiseInstance(globalObject, exception));
    }
    memcpy(self.pending + self.pendingSize, buffer.inbuf, left);
    self.pendingSize += left;
}

// ---- MultibyteIncrementalEncoder

PYTHON_NATIVE(encoderNew)
{
    NATIVE_PROLOGUE();
    auto* object = newContext(globalObject, asType(args[0]), args.at(1), "IncrementalEncoder"_s, 1);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = object->state<Context>();
    if (self.codec->encinit)
        self.codec->encinit(&self.state, self.codec);
    return JSValue::encode(object);
}

PYTHON_NATIVE(encoderEncode)
{
    NATIVE_PROLOGUE();
    bool isFinal = false;
    if (JSValue given = args.at(2)) {
        isFinal = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto encoded = encodeKeepingWhatIsLeft(globalObject, args[0].asCell(), stateOf<Context>(args[0]), args.at(1), isFinal);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(newBytes(globalObject, encoded->span()));
}

// A byte for how much is kept, that much in UTF-8, and then what the codec keeps
constexpr size_t encoderStateSize = 1 + MAXENCPENDING * 4 + sizeof(MultibyteCodec_State::c);

PYTHON_NATIVE(encoderGetState)
{
    NATIVE_PROLOGUE();
    Context& self = stateOf<Context>(args[0]);
    uint8_t bytes[encoderStateSize];
    size_t size = 1;
    bytes[0] = 0;
    if (JSValue pending = self.pendingText.get()) {
        auto encoded = encodeUTF8(globalObject, pending, { });
        RETURN_IF_EXCEPTION(scope, { });
        if (encoded->size() > MAXENCPENDING * 4) {
            JSValue exception = newEncodeError(globalObject, self.codec, pending, 0, stringLength(globalObject, asString(pending)), "pending buffer too large"_s);
            RETURN_IF_EXCEPTION(scope, { });
            scope.release();
            raiseInstance(globalObject, exception);
            return { };
        }
        bytes[0] = static_cast<uint8_t>(encoded->size());
        memcpy(bytes + 1, encoded->span().data(), encoded->size());
        size += encoded->size();
    }
    memcpy(bytes + size, self.state.c, sizeof(self.state.c));
    size += sizeof(self.state.c);
    RELEASE_AND_RETURN(scope, JSValue::encode(intFromLittleEndian(globalObject, std::span(bytes).first(size))));
}

PYTHON_NATIVE(encoderSetState)
{
    NATIVE_PROLOGUE();
    Context& self = stateOf<Context>(args[0]);
    if (!isIntObject(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("setstate() argument must be int, not "_s, typeNameOfArgument(globalObject, args[1]))));
    uint8_t bytes[encoderStateSize];
    littleEndianFromInt(globalObject, args[1], bytes);
    RETURN_IF_EXCEPTION(scope, { });
    if (bytes[0] > MAXENCPENDING * 4)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::UnicodeError, "pending buffer too large"_s));
    String pending = decodeUTF8(globalObject, std::span(bytes).subspan(1, bytes[0]), { });
    RETURN_IF_EXCEPTION(scope, { });
    self.pendingText.set(vm, args[0].asCell(), jsString(vm, pending));
    memcpy(self.state.c, bytes + 1 + bytes[0], sizeof(self.state.c));
    RETURN_NONE();
}

PYTHON_NATIVE(encoderReset)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    Context& self = stateOf<Context>(args[0]);
    if (self.codec->encreset) {
        // The most that comes out is four bytes, with ISO 2022. It is not wanted.
        unsigned char buffer[4];
        unsigned char* outbuf = buffer;
        self.codec->encreset(&self.state, self.codec, &outbuf, sizeof(buffer));
    }
    self.pendingText.clear();
    RETURN_NONE();
}

// ---- MultibyteIncrementalDecoder

PYTHON_NATIVE(decoderNew)
{
    NATIVE_PROLOGUE();
    auto* object = newContext(globalObject, asType(args[0]), args.at(1), "IncrementalDecoder"_s, 1);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = object->state<Context>();
    if (self.codec->decinit)
        self.codec->decinit(&self.state, self.codec);
    return JSValue::encode(object);
}

PYTHON_NATIVE(decoderDecode)
{
    NATIVE_PROLOGUE();
    Context& self = stateOf<Context>(args[0]);
    Buffer input = bufferOf(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    bool isFinal = false;
    if (JSValue given = args.at(2)) {
        isFinal = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    Py_ssize_t originalPending = self.pendingSize;
    unsigned char original[MAXDECPENDING];
    memcpy(original, self.pending, originalPending);
    // What was kept goes in front of it, if anything was.
    Vector<uint8_t> joined;
    DecodeBuffer buffer;
    if (!originalPending)
        buffer.prepare(input);
    else {
        if (!joined.tryReserveInitialCapacity(originalPending + input.size()))
            return JSValue::encode(raiseMemoryError(globalObject, scope));
        joined.append(std::span<const uint8_t>(self.pending, originalPending));
        joined.append(input.span());
        self.pendingSize = 0;
        buffer.prepare(joined.span());
    }
    feed(globalObject, self.codec, &self.state, buffer, self.errors(), true);
    RETURN_IF_EXCEPTION(scope, { });
    if (isFinal && buffer.hasMore()) {
        handleDecodeError(globalObject, self.codec, buffer, self.errors(), MBERR_TOOFEW);
        if (scope.exception()) [[unlikely]] {
            // What there was before is put back.
            memcpy(self.pending, original, originalPending);
            self.pendingSize = originalPending;
            return { };
        }
    }
    if (buffer.hasMore()) {
        keepWhatIsLeft(globalObject, self, buffer);
        RETURN_IF_EXCEPTION(scope, { });
    }
    String text = buffer.writer.finish(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(decoderGetState)
{
    NATIVE_PROLOGUE();
    Context& self = stateOf<Context>(args[0]);
    JSValue number = intFromLittleEndian(globalObject, self.state.c);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { newBytes(globalObject, std::span<const uint8_t>(self.pending, self.pendingSize)), number }));
}

PYTHON_NATIVE(decoderSetState)
{
    NATIVE_PROLOGUE();
    Context& self = stateOf<Context>(args[0]);
    if (!isTuple(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("setstate() argument must be tuple, not "_s, typeNameOfArgument(globalObject, args[1]))));
    // PyArg_ParseTuple() with "SO!;setstate(): illegal state argument"
    PyTuple* given = asTuple(args[1]);
    if (given->length() != 2 || !isBytes(given->at(0)) || !isIntObject(given->at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "setstate(): illegal state argument"_s));
    uint8_t bytes[sizeof(MultibyteCodec_State::c)];
    littleEndianFromInt(globalObject, given->at(1), bytes);
    RETURN_IF_EXCEPTION(scope, { });
    auto pending = *builtinBufferOf(given->at(0));
    if (pending.size() > MAXDECPENDING) {
        JSValue exception = newDecodeError(globalObject, self.codec, pending, 0, pending.size(), "pending buffer too large"_s);
        RETURN_IF_EXCEPTION(scope, { });
        scope.release();
        raiseInstance(globalObject, exception);
        return { };
    }
    self.pendingSize = pending.size();
    memcpy(self.pending, pending.data(), pending.size());
    memcpy(self.state.c, bytes, sizeof(bytes));
    RETURN_NONE();
}

// Of a MultibyteStreamReader too
PYTHON_NATIVE(decoderReset)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    Context& self = stateOf<Context>(args[0]);
    if (self.codec->decreset)
        self.codec->decreset(&self.state, self.codec);
    self.pendingSize = 0;
    RETURN_NONE();
}

// ---- MultibyteStreamReader

PYTHON_NATIVE(readerNew)
{
    NATIVE_PROLOGUE();
    auto* object = newContext(globalObject, asType(args[0]), args.at(2), "StreamReader"_s, 2);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = object->state<Context>();
    self.stream.set(vm, object, args.at(1));
    if (self.codec->decinit)
        self.codec->decinit(&self.state, self.codec);
    return JSValue::encode(object);
}

// mbstreamreader_iread(). A null String if it raised.
String readFromStream(JSGlobalObject* globalObject, Context& self, ASCIILiteral method, Py_ssize_t sizeHint)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!sizeHint)
        return emptyString();
    Identifier name = Identifier::fromString(vm, method);
    DecodeBuffer buffer;
    while (true) {
        JSValue chunk = sizeHint < 0 ? callMethodNamed(globalObject, self.stream.get(), name) : callMethodNamed(globalObject, self.stream.get(), name, intFromInt64(globalObject, static_cast<int>(sizeHint)));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isBytes(chunk)) {
            raiseTypeError(globalObject, scope, concatenate("stream function returned a non-bytes object ("_s, typeOf(globalObject, chunk)->nameString(globalObject).left(100), ')'));
            return { };
        }
        auto read = *builtinBufferOf(chunk);
        bool isEndOfFile = read.empty();
        Vector<uint8_t> data;
        if (!data.tryReserveInitialCapacity(self.pendingSize + read.size())) {
            raiseMemoryError(globalObject, scope);
            return { };
        }
        data.append(std::span<const uint8_t>(self.pending, self.pendingSize));
        data.append(read);
        self.pendingSize = 0;

        buffer.prepare(data.span());
        // The exception that there may be has what was being decoded the last time round.
        feed(globalObject, self.codec, &self.state, buffer, self.errors(), true);
        RETURN_IF_EXCEPTION(scope, { });
        if ((isEndOfFile || sizeHint < 0) && buffer.hasMore()) {
            handleDecodeError(globalObject, self.codec, buffer, self.errors(), MBERR_TOOFEW);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (buffer.hasMore()) {
            keepWhatIsLeft(globalObject, self, buffer);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (sizeHint < 0 || buffer.writer.position() || data.isEmpty())
            break;
        // One byte more, and it is tried again.
        sizeHint = 1;
    }
    RELEASE_AND_RETURN(scope, buffer.writer.finish(globalObject));
}

// How much read(), readline() and readlines() are asked for. Nothing if it raised.
std::optional<Py_ssize_t> sizeArgument(JSGlobalObject* globalObject, JSValue given)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!given || isNone(given))
        return -1;
    if (!isIntObject(given)) {
        raiseTypeError(globalObject, scope, "arg 1 must be an integer"_s);
        return std::nullopt;
    }
    auto size = toSsize(globalObject, given);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return *size;
}

// read() and readline(), which ask the stream for the same
PYTHON_NATIVE(readerRead)
{
    bool isLine = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto size = sizeArgument(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    String text = readFromStream(globalObject, stateOf<Context>(args[0]), isLine ? "readline"_s : "read"_s, *size);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, text));
}

PYTHON_NATIVE(readerReadLines)
{
    NATIVE_PROLOGUE();
    auto size = sizeArgument(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    String text = readFromStream(globalObject, stateOf<Context>(args[0]), "read"_s, *size);
    RETURN_IF_EXCEPTION(scope, { });
    // PyUnicode_Splitlines(), keeping the ends
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, jsString(vm, text), Identifier::fromString(vm, "splitlines"_s), jsBoolean(true))));
}

// ---- MultibyteStreamWriter

PYTHON_NATIVE(writerNew)
{
    NATIVE_PROLOGUE();
    auto* object = newContext(globalObject, asType(args[0]), args.at(2), "StreamWriter"_s, 2);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = object->state<Context>();
    self.stream.set(vm, object, args.at(1));
    if (self.codec->encinit)
        self.codec->encinit(&self.state, self.codec);
    return JSValue::encode(object);
}

// mbstreamwriter_iwrite()
void writeToStream(JSGlobalObject* globalObject, JSCell* owner, Context& self, JSValue text)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto encoded = encodeKeepingWhatIsLeft(globalObject, owner, self, text, false);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    callMethodNamed(globalObject, self.stream.get(), Identifier::fromString(vm, "write"_s), newBytes(globalObject, encoded->span()));
}

PYTHON_NATIVE(writerWrite)
{
    NATIVE_PROLOGUE();
    writeToStream(globalObject, args[0].asCell(), stateOf<Context>(args[0]), args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(writerWriteLines)
{
    NATIVE_PROLOGUE();
    JSValue lines = args[1];
    if (!isSequence(globalObject, lines))
        return JSValue::encode(raiseTypeError(globalObject, scope, "arg must be a sequence object"_s));
    // How many there are can change meanwhile.
    for (int i = 0;; ++i) {
        int64_t length = sequenceSize(globalObject, lines);
        RETURN_IF_EXCEPTION(scope, { });
        if (i >= length)
            break;
        JSValue line = sequenceItem(globalObject, lines, i);
        RETURN_IF_EXCEPTION(scope, { });
        writeToStream(globalObject, args[0].asCell(), stateOf<Context>(args[0]), line);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(writerReset)
{
    NATIVE_PROLOGUE();
    Context& self = stateOf<Context>(args[0]);
    JSValue pending = self.pendingText.get();
    if (!pending)
        RETURN_NONE();
    auto encoded = encode(globalObject, self.codec, &self.state, pending, nullptr, self.errors(), MBENC_FLUSH | MBENC_RESET);
    // It is to be as it was at the beginning, so what could not be encoded is lost.
    self.pendingText.clear();
    RETURN_IF_EXCEPTION(scope, { });
    if (!encoded->isEmpty()) {
        callMethodNamed(globalObject, self.stream.get(), Identifier::fromString(vm, "write"_s), newBytes(globalObject, encoded->span()));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// ---- The module

PYTHON_NATIVE(moduleCreateCodec)
{
    NATIVE_PROLOGUE();
    auto* codec = static_cast<const MultibyteCodec*>(capsulePointer(args[0], codecCapsuleName));
    if (!codec)
        return JSValue::encode(raiseValueError(globalObject, scope, "argument type invalid"_s));
    if (codec->codecinit) {
        codec->codecinit(globalObject, codec);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(PyStateObject::create(vm, frameworkState(globalObject).codecType->instanceStructure(), makeUnique<CodecState>(codec)));
}

PyType* createType(JSGlobalObject* globalObject, ASCIILiteral name, unsigned flags)
{
    VM& vm = globalObject->vm();
    PyType* type = createBuiltinType(globalObject, name, globalObject->pyRealm()->typeObject(), PyType::Layout::Native, flags);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    addGenericGetAttribute(globalObject, type);
    return type;
}

} // anonymous namespace

JSObject* createMultibyteCodecModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = newBuiltinModule(globalObject, "_multibytecodec"_s);
    FrameworkState& state = frameworkState(globalObject);
    constexpr auto checked = PyNativeFunction::Arguments::AreThoseOfTheClass;

    PyType* codec = createType(globalObject, "_multibytecodec.MultibyteCodec"_s, 0);
    state.codecType.set(vm, realm, codec);
    addMethods(globalObject, codec, {
        { "encode"_s, codecEncode },
        { "decode"_s, codecDecode },
    });

    auto stateful = [&](ASCIILiteral qualifiedName, ASCIILiteral name, WriteBarrier<PyType>& slot) {
        PyType* type = createType(globalObject, qualifiedName, PyType::IsBaseType);
        slot.set(vm, realm, type);
        addMethods(globalObject, type, { { "__init__"_s, contextInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreNotChecked } });
        addGetSet(globalObject, type, "errors"_s, contextErrors, setContextErrors);
        module->putDirect(vm, Identifier::fromString(vm, name), type->object());
        return type;
    };
    auto stream = [](JSGlobalObject*, JSValue self) -> JSValue { return stateOf<Context>(self).stream.get(); };

    addMethods(globalObject, stateful("_multibytecodec.MultibyteIncrementalEncoder"_s, "MultibyteIncrementalEncoder"_s, state.encoderType), {
        { "__new__"_s, encoderNew, Kind::New, 0, "IncrementalEncoder($type, /, errors=None)"_s, checked },
        { "encode"_s, encoderEncode },
        { "getstate"_s, encoderGetState },
        { "setstate"_s, encoderSetState },
        { "reset"_s, encoderReset },
    });
    addMethods(globalObject, stateful("_multibytecodec.MultibyteIncrementalDecoder"_s, "MultibyteIncrementalDecoder"_s, state.decoderType), {
        { "__new__"_s, decoderNew, Kind::New, 0, "IncrementalDecoder($type, /, errors=None)"_s, checked },
        { "decode"_s, decoderDecode },
        { "getstate"_s, decoderGetState },
        { "setstate"_s, decoderSetState },
        { "reset"_s, decoderReset },
    });
    PyType* reader = stateful("_multibytecodec.MultibyteStreamReader"_s, "MultibyteStreamReader"_s, state.readerType);
    addMethods(globalObject, reader, {
        { "__new__"_s, readerNew, Kind::New, 0, "StreamReader($type, /, stream, errors=None)"_s, checked },
        { "read"_s, readerRead, Kind::Method, pack(false) },
        { "readline"_s, readerRead, Kind::Method, pack(true) },
        { "readlines"_s, readerReadLines },
        { "reset"_s, decoderReset },
    });
    addMember(globalObject, reader, "stream"_s, stream);
    PyType* writer = stateful("_multibytecodec.MultibyteStreamWriter"_s, "MultibyteStreamWriter"_s, state.writerType);
    addMethods(globalObject, writer, {
        { "__new__"_s, writerNew, Kind::New, 0, "StreamWriter($type, /, stream, errors=None)"_s, checked },
        { "write"_s, writerWrite, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass },
        { "writelines"_s, writerWriteLines, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass },
        { "reset"_s, writerReset, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass },
    });
    addMember(globalObject, writer, "stream"_s, stream);

    addFunction(globalObject, module, "__create_codec"_s, moduleCreateCodec);
    return module;
}

} } // namespace JSC::Python
