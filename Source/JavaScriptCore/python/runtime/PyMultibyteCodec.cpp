/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyCJKCodecs.h"
#include "PyCodecs.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyTuple.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace CJK {

// The module _multibytecodec: the objects through which programs use the codecs of PyCJKCodecs.cpp. Lib/encodings/*.py derive the
// classes of a codec from them. (Modules/cjkcodecs/multibytecodec.c in CPython is the reference.)

// ---- How errors are treated ----

enum class ErrorMode : uint8_t {
    Strict,
    Ignore,
    Replace,
    Handler, // One that is registered under a name. It is looked up when there is an error, not before.
};

struct ErrorTreatment {
    ErrorMode mode { ErrorMode::Strict };
    String handler;
};

// A null name is "strict".
static ErrorTreatment treatmentNamed(const String& name)
{
    if (name.isNull() || name == "strict"_s)
        return { ErrorMode::Strict, { } };
    if (name == "ignore"_s)
        return { ErrorMode::Ignore, { } };
    if (name == "replace"_s)
        return { ErrorMode::Replace, { } };
    return { ErrorMode::Handler, name };
}

enum class NUL : uint8_t {
    IsAnError,
    EndsTheText,
};

// The text of a str that CPython takes as a C string in UTF-8.
static bool textOfCString(JSGlobalObject* globalObject, JSString* string, NUL nul, String& text)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (text.isNull())
        text = emptyString();
    if (!text.is8Bit()) {
        // A lone surrogate is a UnicodeEncodeError.
        codecEncodeUTF8(globalObject, string, String());
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (size_t end = text.find(static_cast<char16_t>(0)); end != notFound) {
        if (nul == NUL::IsAnError) {
            raiseValueError(globalObject, scope, "embedded null character"_s);
            return false;
        }
        text = text.left(end);
    }
    return true;
}

// The argument `errors` of MultibyteCodec.encode() and decode(): a str or None.
static bool treatmentFromArgument(JSGlobalObject* globalObject, ASCIILiteral function, JSValue errors, ErrorTreatment& treatment)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!errors || isNone(errors))
        return true;
    if (!isStr(errors)) {
        raiseTypeError(globalObject, scope, function, "() argument 'errors' must be str or None, not "_s, typeNameOfArgument(globalObject, errors));
        return false;
    }
    String name;
    if (!textOfCString(globalObject, strValue(errors), NUL::IsAnError, name))
        return false;
    treatment = treatmentNamed(name);
    return true;
}

static constexpr auto illegalSequence = "illegal multibyte sequence"_s;
static constexpr auto incompleteSequence = "incomplete multibyte sequence"_s;

// For what a codec returned that is not a number of characters or bytes, nor needsMoreInput.
static void raiseCodecFailure(JSGlobalObject* globalObject, int result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseRuntimeError(globalObject, scope, result == internalError ? "internal codec error"_s : "unknown runtime error"_s);
}

// A UnicodeEncodeError or UnicodeDecodeError. One is made for a whole run, and changed for each further error.
static bool prepareUnicodeError(JSGlobalObject* globalObject, PyException*& exception, PyType* type, const Codec& codec, JSValue object, size_t start, size_t end, ASCIILiteral reason)
{
    VM& vm = getVM(globalObject);
    if (!exception) {
        JSValue arguments[] = { makeStr(vm, codec.name), object, jsNumber(start), jsNumber(end), makeStr(vm, reason) };
        exception = newException(globalObject, type, arguments);
        return exception;
    }
    exception->setField(vm, ExceptionField::unicodeErrorStart, jsNumber(start));
    exception->setField(vm, ExceptionField::unicodeErrorEnd, jsNumber(end));
    exception->setField(vm, ExceptionField::unicodeErrorReason, makeStr(vm, reason));
    return true;
}

static void raiseUnicodeError(JSGlobalObject* globalObject, PyType* type, const Codec& codec, JSValue object, size_t start, size_t end, ASCIILiteral reason)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = nullptr;
    if (prepareUnicodeError(globalObject, exception, type, codec, object, start, end, reason))
        raiseException(globalObject, scope, exception);
}

// Where the handler says to go on, among `length` characters or bytes. False: nowhere among them.
static bool positionFromHandler(JSGlobalObject* globalObject, JSValue value, size_t length, size_t& position)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What does not fit a Py_ssize_t is -1 to CPython, and is not counted from the end.
    auto fitting = tryInt64(intValue(value));
    int64_t given = fitting.value_or(-1);
    if (fitting && given < 0)
        given += static_cast<int64_t>(length);
    if (given < 0 || given > static_cast<int64_t>(length)) {
        raiseIndexError(globalObject, scope, "position "_s, given, " from error handler out of bounds"_s);
        return false;
    }
    position = static_cast<size_t>(given);
    return true;
}

// ---- Encoding ----

constexpr unsigned flushes = 1; // There is no more text to come.
constexpr unsigned resets = 2; // The output is to end in the state it began in.

static bool encodeText(JSGlobalObject*, const Codec&, State&, JSValue text, const ErrorTreatment&, unsigned flags, Output&, size_t& position, size_t& length);

struct EncodeRun {
    const Codec& codec;
    State& state;
    const ErrorTreatment& errors;
    JSValue object; // The str, which may be of a class derived from str.
    const Text& text;
    Output& out;
    size_t& position;
    PyException* exception { nullptr };
};

static bool handleEncodeError(JSGlobalObject* globalObject, EncodeRun& run, int result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASCIILiteral reason = illegalSequence;
    size_t size = result;
    if (result == needsMoreInput) {
        reason = incompleteSequence;
        size = run.position; // As CPython has it. No codec asks for more when there is no more.
    } else if (result < 0) {
        raiseCodecFailure(globalObject, result);
        return false;
    }

    if (run.errors.mode == ErrorMode::Replace) {
        Text questionMark { "?"_s };
        size_t position = 0;
        if (run.codec.encode(run.codec, run.state, questionMark, position, run.out, false))
            *run.out.room(1) = '?';
    }
    if (run.errors.mode == ErrorMode::Ignore || run.errors.mode == ErrorMode::Replace) {
        run.position += size;
        return true;
    }

    if (!prepareUnicodeError(globalObject, run.exception, rt.UnicodeEncodeError(), run.codec, run.object, run.position, run.position + size, reason))
        return false;
    if (run.errors.mode == ErrorMode::Strict) {
        raiseException(globalObject, scope, run.exception);
        return false;
    }

    JSValue handler = codecLookupError(globalObject, run.errors.handler);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue answer = call(globalObject, handler, run.exception);
    RETURN_IF_EXCEPTION(scope, false);
    auto* tuple = PyTuple::tryFrom(answer);
    if (!tuple || tuple->length() != 2 || !(isStr(tuple->at(0)) || PyBytes::is(tuple->at(0))) || !isInt(tuple->at(1))) {
        raiseTypeError(globalObject, scope, "encoding error handler must return (str, int) tuple"_s);
        return false;
    }
    if (auto* bytes = PyBytes::tryFrom(tuple->at(0)))
        run.out.append(bytes->span());
    else {
        size_t position, length;
        if (!encodeText(globalObject, run.codec, run.state, tuple->at(0), { }, flushes, run.out, position, length))
            return false;
    }
    return positionFromHandler(globalObject, tuple->at(1), run.text.length(), run.position);
}

// Appends the bytes for the text. `position`: how many of its `length` characters they are for; without `flushes`, the last may
// have to wait for what follows.
static bool encodeText(JSGlobalObject* globalObject, const Codec& codec, State& state, JSValue object, const ErrorTreatment& errors, unsigned flags, Output& out, size_t& position, size_t& length)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String value = strValue(object)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    Text text { value };
    if (text.hasFailed()) {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    position = 0;
    length = text.length();
    out.expect(length * 2);

    EncodeRun run { codec, state, errors, object, text, out, position };
    while (position < length) {
        int result = codec.encode(codec, state, text, position, out, flags & flushes);
        if (!result || (result == needsMoreInput && !(flags & flushes)))
            break;
        if (!handleEncodeError(globalObject, run, result))
            return false;
        if (result == needsMoreInput)
            break;
    }
    if (codec.finishEncoding && (flags & resets))
        codec.finishEncoding(state, out);
    return true;
}

// What is not a str is encoded as str() shows it.
static JSValue textToEncode(JSGlobalObject* globalObject, JSValue input)
{
    if (isStr(input)) [[likely]]
        return input;
    return strObject(globalObject, input);
}

// ---- Decoding ----

struct DecodeRun {
    std::span<const uint8_t> in;
    size_t position { 0 };
    StrBuilder out;
    PyException* exception { nullptr };
};

static bool handleDecodeError(JSGlobalObject* globalObject, const Codec& codec, DecodeRun& run, const ErrorTreatment& errors, int result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASCIILiteral reason = illegalSequence;
    size_t size = result;
    if (result == needsMoreInput) {
        reason = incompleteSequence;
        size = run.in.size() - run.position;
    } else if (result < 0) {
        raiseCodecFailure(globalObject, result);
        return false;
    }

    if (errors.mode == ErrorMode::Replace)
        run.out.append(static_cast<char16_t>(0xFFFD));
    if (errors.mode == ErrorMode::Ignore || errors.mode == ErrorMode::Replace) {
        run.position += size;
        return true;
    }

    JSValue object;
    if (!run.exception) {
        object = PyBytes::create(globalObject, run.in);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!prepareUnicodeError(globalObject, run.exception, rt.UnicodeDecodeError(), codec, object, run.position, run.position + size, reason))
        return false;
    if (errors.mode == ErrorMode::Strict) {
        raiseException(globalObject, scope, run.exception);
        return false;
    }

    JSValue handler = codecLookupError(globalObject, errors.handler);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue answer = call(globalObject, handler, run.exception);
    RETURN_IF_EXCEPTION(scope, false);
    auto* tuple = PyTuple::tryFrom(answer);
    if (!tuple || tuple->length() != 2 || !isStr(tuple->at(0)) || !isInt(tuple->at(1))) {
        raiseTypeError(globalObject, scope, "decoding error handler must return (str, int) tuple"_s);
        return false;
    }
    String replacement = strValue(tuple->at(0))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    run.out.append(replacement);
    return positionFromHandler(globalObject, tuple->at(1), run.in.size(), run.position);
}

// Decodes until the end, or until what is left is the beginning of something.
static bool decodeWhatIsComplete(JSGlobalObject* globalObject, const Codec& codec, State& state, DecodeRun& run, const ErrorTreatment& errors)
{
    while (run.position < run.in.size()) {
        int result = codec.decode(codec, state, run.in, run.position, run.out);
        if (!result || result == needsMoreInput)
            break;
        if (!handleDecodeError(globalObject, codec, run, errors, result))
            return false;
    }
    return true;
}

// The bytes of an argument, for as long as this lives.
class InputBytes {
    WTF_MAKE_NONCOPYABLE(InputBytes);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit InputBytes(JSGlobalObject* globalObject)
        : m_holder(globalObject)
    {
    }

    // `before` comes first.
    bool get(JSGlobalObject* globalObject, JSValue input, std::span<const uint8_t> before, const ErrorTreatment& errors)
    {
        if (!m_holder.get(input))
            return false;
        span = m_holder.buffer.span();
        // A handler may run anything, and memory that belongs to JS may then be gone.
        if (before.empty() && (errors.mode != ErrorMode::Handler || PyBytes::is(input)))
            return true;
        if (!m_copy.tryReserveInitialCapacity(before.size() + span.size())) {
            VM& vm = getVM(globalObject);
            auto scope = DECLARE_THROW_SCOPE(vm);
            raiseMemoryError(globalObject, scope, String());
            return false;
        }
        m_copy.append(before);
        m_copy.append(span);
        span = m_copy.span();
        return true;
    }

    std::span<const uint8_t> span;

private:
    BufferHolder m_holder;
    Vector<uint8_t> m_copy;
};

// ---- MultibyteCodec ---- (number: the address of the Codec)

static JSValue codecEncode(JSGlobalObject* globalObject, JSValue self, JSValue input, JSValue errorsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Codec& codec = *std::bit_cast<const Codec*>(static_cast<intptr_t>(PyRecord::from(self)->number));
    ErrorTreatment errors;
    if (!treatmentFromArgument(globalObject, "encode"_s, errorsValue, errors))
        return { };
    JSValue text = textToEncode(globalObject, input);
    RETURN_IF_EXCEPTION(scope, { });

    State state;
    if (codec.startEncoding)
        codec.startEncoding(state);
    Output out;
    size_t position, length;
    if (!encodeText(globalObject, codec, state, text, errors, flushes | resets, out, position, length))
        return { };
    PyBytes* bytes = out.makeBytes(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { bytes, jsNumber(length) }));
}

static JSValue codecDecode(JSGlobalObject* globalObject, JSValue self, JSValue input, JSValue errorsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Codec& codec = *std::bit_cast<const Codec*>(static_cast<intptr_t>(PyRecord::from(self)->number));
    // CPython gets the buffer first, and complains of `errors` after.
    BufferHolder probe(globalObject);
    if (!probe.get(input))
        return { };
    ErrorTreatment errors;
    if (!treatmentFromArgument(globalObject, "decode"_s, errorsValue, errors))
        return { };
    InputBytes bytes(globalObject);
    if (!bytes.get(globalObject, input, { }, errors))
        return { };

    State state;
    if (codec.startDecoding)
        codec.startDecoding(state);
    DecodeRun run;
    run.in = bytes.span;
    while (run.position < run.in.size()) {
        int result = codec.decode(codec, state, run.in, run.position, run.out);
        if (!result)
            break;
        if (!handleDecodeError(globalObject, codec, run, errors, result))
            return { };
    }
    JSString* text = makeStr(globalObject, run.out);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { text, jsNumber(run.in.size()) }));
}

static constexpr NativeDef codecMethods[] = {
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_METHOD("encode", codecEncode, "($self, /, input, errors=None)"),
    PY_METHOD("decode", codecDecode, "($self, /, input, errors=None)"),
};

static constexpr TypeDef codecTypeDef { "_multibytecodec.MultibyteCodec"_s, { TypeFlag::DisallowInstantiation }, codecMethods, { }, { } };

// ---- What the four kinds of objects with a state have in common ----
//
// number: the state of the codec.

enum Field : unsigned {
    FieldCodec, // The MultibyteCodec.
    FieldErrors, // A str.
    FieldPending, // What could not be converted yet: of an encoder a str, of a decoder bytes. Empty: nothing.
    FieldStream,
    numberOfFields,
};

constexpr size_t maxPendingCharacters = 2;
constexpr size_t maxPendingBytes = 8;

static_assert(sizeof(State) == sizeof(PyRecord::number));
// The state itself, not a copy: what an error handler or a stream does to the object in the middle of a call counts.
static State& stateOf(PyRecord* object) { return *reinterpret_cast<State*>(&object->number); }
static const Codec& codecOf(PyRecord* object) { return *std::bit_cast<const Codec*>(static_cast<intptr_t>(PyRecord::from(object->get(FieldCodec))->number)); }
static ErrorTreatment treatmentOf(PyRecord* object) { return treatmentNamed(stringOf(asString(object->get(FieldErrors)))); }

static std::span<const uint8_t> pendingBytesOf(PyRecord* object)
{
    JSValue pending = object->get(FieldPending);
    return pending ? PyBytes::from(pending)->span() : std::span<const uint8_t> { };
}

static void setErrors(VM& vm, PyRecord* object, const String& name)
{
    object->set(vm, FieldErrors, makeStr(vm, name.isNull() ? String("strict"_s) : name));
}

// The arguments of a constructor, taken as PyArg_ParseTupleAndKeywords() takes them: [stream,] errors='strict'.
static PyRecord* newStatefulObject(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args, ASCIILiteral function, bool hasStream)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral allNames[] = { "stream"_s, "errors"_s };
    auto names = std::span<const ASCIILiteral> { allNames }.subspan(hasStream ? 0 : 1);
    unsigned total = names.size();
    unsigned count = args.size();
    unsigned keywordCount = args.keywordCount();
    if (count + keywordCount > total) {
        raiseTypeError(globalObject, scope, function, "() takes at most "_s, total, count ? " argument"_s : " keyword argument"_s, total == 1 ? ""_s : "s"_s, " ("_s, count + keywordCount, " given)"_s);
        return nullptr;
    }

    JSValue values[2];
    String errors;
    for (unsigned i = 0; i < total; ++i) {
        values[i] = i < count ? args[i] : args.keyword(names[i]);
        if (!values[i]) {
            if (hasStream && !i) {
                raiseTypeError(globalObject, scope, function, "() missing required argument 'stream' (pos 1)"_s);
                return nullptr;
            }
            continue;
        }
        if (i + 1 < total)
            continue;
        if (!isStr(values[i])) {
            raiseTypeError(globalObject, scope, function, "() argument "_s, i + 1, " must be str, not "_s, typeNameOfArgument(globalObject, values[i]));
            return nullptr;
        }
        if (!textOfCString(globalObject, strValue(values[i]), NUL::IsAnError, errors))
            return nullptr;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (args.keyword(names[i])) {
            raiseTypeError(globalObject, scope, "argument for "_s, function, "() given by name ('"_s, names[i], "') and position ("_s, i + 1, ')');
            return nullptr;
        }
    }
    for (unsigned k = 0; k < keywordCount; ++k) {
        String keyword = stringOf(args.keywordName(k));
        if (std::ranges::find(names, keyword, [](ASCIILiteral name) { return String(name); }) != names.end())
            continue;
        Vector<String> candidates;
        for (ASCIILiteral name : names)
            candidates.append(String(name));
        String suggestion = calculateSuggestion(candidates, keyword);
        if (suggestion.isNull())
            raiseTypeError(globalObject, scope, function, "() got an unexpected keyword argument '"_s, keyword, '\'');
        else
            raiseTypeError(globalObject, scope, function, "() got an unexpected keyword argument '"_s, keyword, "'. Did you mean '"_s, suggestion, "'?"_s);
        return nullptr;
    }

    // The class that Lib/encodings derives for a codec says which.
    JSValue codec = getAttr(globalObject, cls, internString(vm, "codec"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!PyRecord::tryFrom(codec, runtime(globalObject).typeFor(codecTypeDef))) {
        raiseTypeError(globalObject, scope, "codec is unexpected type"_s);
        return nullptr;
    }
    PyRecord* object = PyRecord::create(globalObject, PyType::from(cls), numberOfFields);
    object->set(vm, FieldCodec, codec);
    setErrors(vm, object, errors);
    if (hasStream)
        object->set(vm, FieldStream, values[0]);
    return object;
}

static JSValue statefulInit(JSGlobalObject*, JSValue, const CallArgs&) { return jsUndefined(); }

static JSValue errorsGetter(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(FieldErrors); }

static void errorsSetter(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raise(globalObject, scope, rt.AttributeError(), "cannot delete attribute"_s);
        return;
    }
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, "errors must be a string"_s);
        return;
    }
    String name;
    if (textOfCString(globalObject, strValue(value), NUL::EndsTheText, name))
        setErrors(vm, PyRecord::from(self), name);
}

static JSValue streamGetter(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(FieldStream); }

static constexpr NativeGetSet statefulGetSets[] = {
    { "errors"_s, errorsGetter, errorsSetter, false, "how to treat errors"_s },
};

static constexpr NativeGetSet streamGetSets[] = {
    PY_MEMBER("stream", streamGetter),
    { "errors"_s, errorsGetter, errorsSetter, false, "how to treat errors"_s },
};

// The bytes of an int that is not negative, the least significant first.
static bool bytesOfInt(JSGlobalObject* globalObject, JSValue value, std::span<uint8_t> bytes)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue number = intValue(value);
    if (intSign(number) < 0) {
        raiseOverflowError(globalObject, scope, "can't convert negative int to unsigned"_s);
        return false;
    }
    size_t size = (intBitLength(number) + 7) / 8;
    if (size > bytes.size()) {
        raiseOverflowError(globalObject, scope, "int too big to convert"_s);
        return false;
    }
    JSBigInt* digits = toBigInt(globalObject, number);
    RETURN_IF_EXCEPTION(scope, false);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = i < size ? static_cast<uint8_t>(digits->digit(i / 8) >> (i % 8 * 8)) : 0;
    return true;
}

// ---- Encoders with a state ----

// The bytes for what was pending and the text. Null: an exception.
static PyBytes* encodeMore(JSGlobalObject* globalObject, PyRecord* object, JSValue input, bool isFinal)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Codec& codec = codecOf(object);
    JSValue text = textToEncode(globalObject, input);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSValue pendingBefore = object->get(FieldPending);
    if (pendingBefore) {
        text = jsString(globalObject, asString(pendingBefore), strValue(text));
        RETURN_IF_EXCEPTION(scope, nullptr);
        object->set(vm, FieldPending, JSValue());
    }

    ErrorTreatment errors = treatmentOf(object);
    Output out;
    size_t position, length;
    if (!encodeText(globalObject, codec, stateOf(object), text, errors, isFinal ? flushes | resets : 0, out, position, length)) {
        object->set(vm, FieldPending, pendingBefore);
        return nullptr;
    }
    if (position < length) {
        if (length - position > maxPendingCharacters) {
            raiseUnicodeError(globalObject, rt.UnicodeEncodeError(), codec, text, position, length, "pending buffer overflow"_s);
            return nullptr;
        }
        StrView view(globalObject, strValue(text));
        RETURN_IF_EXCEPTION(scope, nullptr);
        object->set(vm, FieldPending, makeStr(vm, view.substring(position, length)));
    }
    RELEASE_AND_RETURN(scope, out.makeBytes(globalObject));
}

// ---- MultibyteIncrementalEncoder ----

static JSValue incrementalEncoderNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    PyRecord* object = newStatefulObject(globalObject, cls, args, "IncrementalEncoder"_s, false);
    if (!object)
        return { };
    if (auto* start = codecOf(object).startEncoding)
        start(stateOf(object));
    return object;
}

static JSValue incrementalEncoderEncode(JSGlobalObject* globalObject, JSValue self, JSValue input, JSValue finalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isFinal = finalValue && isTrue(globalObject, finalValue);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, encodeMore(globalObject, PyRecord::from(self), input, isFinal));
}

// An int, of these bytes: how many bytes what is pending takes in UTF-8, those, and the state of the codec.
constexpr size_t maxPendingUTF8 = maxPendingCharacters * 4;

static JSValue incrementalEncoderGetState(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyRecord::from(self);
    Vector<uint8_t, 1 + maxPendingUTF8 + sizeof(State)> bytes;
    bytes.append(0);
    if (JSValue pending = object->get(FieldPending)) {
        auto* encoded = PyBytes::tryFrom(codecEncodeUTF8(globalObject, asString(pending), String()));
        RETURN_IF_EXCEPTION(scope, { });
        if (encoded->span().size() > maxPendingUTF8) {
            StrView view(globalObject, asString(pending));
            RETURN_IF_EXCEPTION(scope, { });
            raiseUnicodeError(globalObject, rt.UnicodeEncodeError(), codecOf(object), pending, 0, view.length(), "pending buffer too large"_s);
            return { };
        }
        bytes[0] = static_cast<uint8_t>(encoded->span().size());
        bytes.append(encoded->span());
    }
    bytes.append(std::span<const uint8_t> { stateOf(object).c });
    RELEASE_AND_RETURN(scope, intFromByteSpan(globalObject, bytes.span(), true, false));
}

static JSValue incrementalEncoderSetState(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyRecord::from(self);
    if (!isInt(value))
        return raiseTypeError(globalObject, scope, "setstate() argument must be int, not "_s, typeNameOfArgument(globalObject, value));
    uint8_t bytes[1 + maxPendingUTF8 + sizeof(State)];
    if (!bytesOfInt(globalObject, value, bytes))
        return { };
    size_t size = bytes[0];
    if (size > maxPendingUTF8)
        return raise(globalObject, scope, rt.UnicodeError(), "pending buffer too large"_s);
    JSValue pending = codecDecodeUTF8(globalObject, std::span<const uint8_t> { bytes }.subspan(1, size), String());
    RETURN_IF_EXCEPTION(scope, { });
    object->set(vm, FieldPending, pending);
    memcpy(stateOf(object).c, bytes + 1 + size, sizeof(State));
    return jsUndefined();
}

static JSValue incrementalEncoderReset(JSGlobalObject* globalObject, JSValue self)
{
    auto* object = PyRecord::from(self);
    if (auto* finish = codecOf(object).finishEncoding) {
        Output discarded;
        finish(stateOf(object), discarded);
    }
    object->set(getVM(globalObject), FieldPending, JSValue());
    return jsUndefined();
}

static constexpr NativeDef incrementalEncoderMethods[] = {
    PY_NEW(incrementalEncoderNew, "($type, *args, **kwargs)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__init__", statefulInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("encode", incrementalEncoderEncode, "($self, /, input, final=False)"),
    PY_METHOD("getstate", incrementalEncoderGetState, "($self, /)"),
    PY_METHOD("setstate", incrementalEncoderSetState, "($self, state, /)"),
    PY_METHOD("reset", incrementalEncoderReset, "($self, /)"),
};

static constexpr TypeDef incrementalEncoderTypeDef { "_multibytecodec.MultibyteIncrementalEncoder"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, incrementalEncoderMethods, statefulGetSets, { } };

// ---- Decoders with a state ----

// What is left of the input is kept for the next time.
static bool keepRestPending(JSGlobalObject* globalObject, PyRecord* object, const DecodeRun& run)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto rest = run.in.subspan(run.position);
    auto pending = pendingBytesOf(object);
    if (rest.size() + pending.size() > maxPendingBytes) {
        PyBytes* all = PyBytes::create(globalObject, run.in);
        RETURN_IF_EXCEPTION(scope, false);
        raiseUnicodeError(globalObject, rt.UnicodeDecodeError(), codecOf(object), all, 0, run.in.size(), "pending buffer overflow"_s);
        return false;
    }
    Vector<uint8_t, maxPendingBytes> bytes;
    bytes.append(pending);
    bytes.append(rest);
    PyBytes* kept = PyBytes::create(globalObject, bytes.span());
    RETURN_IF_EXCEPTION(scope, false);
    object->set(vm, FieldPending, kept);
    return true;
}

static JSValue decoderReset(JSGlobalObject* globalObject, JSValue self)
{
    auto* object = PyRecord::from(self);
    if (auto* reset = codecOf(object).resetDecoder)
        reset(stateOf(object));
    object->set(getVM(globalObject), FieldPending, JSValue());
    return jsUndefined();
}

// ---- MultibyteIncrementalDecoder ----

static JSValue incrementalDecoderNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    PyRecord* object = newStatefulObject(globalObject, cls, args, "IncrementalDecoder"_s, false);
    if (!object)
        return { };
    if (auto* start = codecOf(object).startDecoding)
        start(stateOf(object));
    return object;
}

static JSValue incrementalDecoderDecode(JSGlobalObject* globalObject, JSValue self, JSValue input, JSValue finalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyRecord::from(self);
    const Codec& codec = codecOf(object);
    // The buffer is asked for before `final` is looked at, which may run code.
    BufferHolder probe(globalObject);
    if (!probe.get(input))
        return { };
    bool isFinal = finalValue && isTrue(globalObject, finalValue);
    RETURN_IF_EXCEPTION(scope, { });

    ErrorTreatment errors = treatmentOf(object);
    JSValue pendingBefore = object->get(FieldPending);
    InputBytes bytes(globalObject);
    if (!bytes.get(globalObject, input, pendingBytesOf(object), errors))
        return { };
    object->set(vm, FieldPending, JSValue());

    DecodeRun run;
    run.in = bytes.span;
    if (!decodeWhatIsComplete(globalObject, codec, stateOf(object), run, errors))
        return { };
    if (isFinal && run.position < run.in.size()) {
        if (!handleDecodeError(globalObject, codec, run, errors, needsMoreInput)) {
            object->set(vm, FieldPending, pendingBefore);
            return { };
        }
    }
    if (run.position < run.in.size()) {
        if (!keepRestPending(globalObject, object, run))
            return { };
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, run.out));
}

static JSValue incrementalDecoderGetState(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyRecord::from(self);
    PyBytes* pending = PyBytes::create(globalObject, pendingBytesOf(object));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue state = intFromByteSpan(globalObject, stateOf(object).c, true, false);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { pending, state }));
}

static JSValue incrementalDecoderSetState(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyRecord::from(self);
    auto* tuple = PyTuple::tryFrom(value);
    if (!tuple)
        return raiseTypeError(globalObject, scope, "setstate() argument must be tuple, not "_s, typeNameOfArgument(globalObject, value));
    if (tuple->length() != 2 || !PyBytes::is(tuple->at(0)) || !isInt(tuple->at(1)))
        return raiseTypeError(globalObject, scope, "setstate(): illegal state argument"_s);
    State state;
    if (!bytesOfInt(globalObject, tuple->at(1), state.c))
        return { };
    auto* pending = PyBytes::from(tuple->at(0));
    if (pending->span().size() > maxPendingBytes) {
        raiseUnicodeError(globalObject, rt.UnicodeDecodeError(), codecOf(object), pending, 0, pending->span().size(), "pending buffer too large"_s);
        return { };
    }
    PyBytes* kept = PyBytes::create(globalObject, pending->span());
    RETURN_IF_EXCEPTION(scope, { });
    object->set(vm, FieldPending, kept);
    stateOf(object) = state;
    return jsUndefined();
}

static constexpr NativeDef incrementalDecoderMethods[] = {
    PY_NEW(incrementalDecoderNew, "($type, *args, **kwargs)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__init__", statefulInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("decode", incrementalDecoderDecode, "($self, /, input, final=False)"),
    PY_METHOD("getstate", incrementalDecoderGetState, "($self, /)"),
    PY_METHOD("setstate", incrementalDecoderSetState, "($self, state, /)"),
    PY_METHOD("reset", decoderReset, "($self, /)"),
};

static constexpr TypeDef incrementalDecoderTypeDef { "_multibytecodec.MultibyteIncrementalDecoder"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, incrementalDecoderMethods, statefulGetSets, { } };

// ---- MultibyteStreamReader ----

static JSValue streamReaderNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    PyRecord* object = newStatefulObject(globalObject, cls, args, "StreamReader"_s, true);
    if (!object)
        return { };
    if (auto* start = codecOf(object).startDecoding)
        start(stateOf(object));
    return object;
}

// Decodes what stream.read() or stream.readline() returns, called with the size unless that is negative.
static JSString* readFromStream(JSGlobalObject* globalObject, PyRecord* object, ASCIILiteral method, JSValue sizeValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (sizeValue && !isNone(sizeValue)) {
        if (!isInt(sizeValue)) {
            raiseTypeError(globalObject, scope, "arg 1 must be an integer"_s);
            return nullptr;
        }
        auto given = tryInt64(intValue(sizeValue));
        if (!given) {
            raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
            return nullptr;
        }
        size = *given;
    }
    if (!size)
        return jsEmptyString(vm);

    const Codec& codec = codecOf(object);
    JSString* methodName = internString(vm, method);
    DecodeRun run;
    while (true) {
        JSValue stream = object->get(FieldStream);
        // (CPython passes the size on as a C int.)
        JSValue result = size < 0 ? callMethod(globalObject, stream, methodName) : callMethod(globalObject, stream, methodName, jsNumber(static_cast<int32_t>(size)));
        RETURN_IF_EXCEPTION(scope, nullptr);
        auto* data = PyBytes::tryFrom(result);
        if (!data) {
            raiseTypeError(globalObject, scope, "stream function returned a non-bytes object ("_s, typeName(globalObject, result), ')');
            return nullptr;
        }
        bool isAtEnd = data->span().empty();

        if (auto pending = pendingBytesOf(object); !pending.empty()) {
            ByteBuilder joined;
            joined.append(pending);
            joined.append(data->span());
            data = makeBytes(globalObject, joined);
            RETURN_IF_EXCEPTION(scope, nullptr);
            object->set(vm, FieldPending, JSValue());
        }
        EnsureStillAliveScope keepsData(data);

        ErrorTreatment errors = treatmentOf(object);
        run.in = data->span();
        run.position = 0;
        if (!decodeWhatIsComplete(globalObject, codec, stateOf(object), run, errors))
            return nullptr;
        if ((isAtEnd || size < 0) && run.position < run.in.size()) {
            if (!handleDecodeError(globalObject, codec, run, errors, needsMoreInput))
                return nullptr;
        }
        if (run.position < run.in.size()) {
            if (!keepRestPending(globalObject, object, run))
                return nullptr;
        }
        if (size < 0 || !run.out.isEmpty() || run.in.empty())
            break;
        // Not one character yet: a byte more.
        size = 1;
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, run.out));
}

static JSValue streamReaderRead(JSGlobalObject* globalObject, JSValue self, JSValue size) { return readFromStream(globalObject, PyRecord::from(self), "read"_s, size); }
static JSValue streamReaderReadLine(JSGlobalObject* globalObject, JSValue self, JSValue size) { return readFromStream(globalObject, PyRecord::from(self), "readline"_s, size); }

static JSValue streamReaderReadLines(JSGlobalObject* globalObject, JSValue self, JSValue sizeHint)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* text = readFromStream(globalObject, PyRecord::from(self), "read"_s, sizeHint);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callMethod(globalObject, text, internString(vm, "splitlines"_s), jsBoolean(true)));
}

static constexpr NativeDef streamReaderMethods[] = {
    PY_NEW(streamReaderNew, "($type, *args, **kwargs)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__init__", statefulInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("read", streamReaderRead, "($self, sizeobj=None, /)"),
    PY_METHOD("readline", streamReaderReadLine, "($self, sizeobj=None, /)"),
    PY_METHOD("readlines", streamReaderReadLines, "($self, sizehintobj=None, /)"),
    PY_METHOD("reset", decoderReset, "($self, /)"),
};

static constexpr TypeDef streamReaderTypeDef { "_multibytecodec.MultibyteStreamReader"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, streamReaderMethods, streamGetSets, { } };

// ---- MultibyteStreamWriter ----

static JSValue streamWriterNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    PyRecord* object = newStatefulObject(globalObject, cls, args, "StreamWriter"_s, true);
    if (!object)
        return { };
    if (auto* start = codecOf(object).startEncoding)
        start(stateOf(object));
    return object;
}

// The one argument of write() and writelines(), which complain in their own way. Empty: they have.
static JSValue onlyArgument(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount() > 1)
        return raiseTypeError(globalObject, scope, function, "() takes at most 1 "_s, args.size() ? ""_s : "keyword "_s, "argument ("_s, args.totalCount(), " given)"_s);
    if (!args.size())
        return raiseTypeError(globalObject, scope, function, "() takes exactly 1 positional argument (0 given)"_s);
    return args[0];
}

static bool writeToStream(JSGlobalObject* globalObject, PyRecord* object, JSValue text)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyBytes* bytes = encodeMore(globalObject, object, text, false);
    RETURN_IF_EXCEPTION(scope, false);
    callMethod(globalObject, object->get(FieldStream), internString(vm, "write"_s), bytes);
    return !scope.exception();
}

static JSValue streamWriterWrite(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    JSValue text = onlyArgument(globalObject, "write"_s, args);
    if (!text || !writeToStream(globalObject, PyRecord::from(self), text))
        return { };
    return jsUndefined();
}

static JSValue streamWriterWriteLines(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue lines = onlyArgument(globalObject, "writelines"_s, args);
    if (!lines)
        return { };
    if (isDict(lines) || !typeOf(globalObject, lines)->lookup(Special::GetItem))
        return raiseTypeError(globalObject, scope, "arg must be a sequence object"_s);
    // What is written to may change the length.
    for (int64_t i = 0;; ++i) {
        int64_t count = length(globalObject, lines);
        RETURN_IF_EXCEPTION(scope, { });
        if (i >= count)
            break;
        JSValue line = getItem(globalObject, lines, jsNumber(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (!writeToStream(globalObject, PyRecord::from(self), line))
            return { };
    }
    return jsUndefined();
}

// Only what is pending is written, with what ends the output after it: nothing at all if nothing is pending.
static JSValue streamWriterReset(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount())
        return raiseTypeError(globalObject, scope, "reset() takes no arguments"_s);
    auto* object = PyRecord::from(self);
    JSValue pending = object->get(FieldPending);
    if (!pending)
        return jsUndefined();
    ErrorTreatment errors = treatmentOf(object);
    Output out;
    size_t position, length;
    bool succeeded = encodeText(globalObject, codecOf(object), stateOf(object), pending, errors, flushes | resets, out, position, length);
    object->set(vm, FieldPending, JSValue());
    if (!succeeded)
        return { };
    if (out.size()) {
        PyBytes* bytes = out.makeBytes(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        callMethod(globalObject, object->get(FieldStream), internString(vm, "write"_s), bytes);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static constexpr NativeDef streamWriterMethods[] = {
    PY_NEW(streamWriterNew, "($type, *args, **kwargs)"),
    PY_SLOT("__getattribute__", objectGetAttribute, "($self, name, /)"),
    PY_SLOT("__init__", statefulInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("write", streamWriterWrite, "!($self, strobj, /)"),
    PY_METHOD("writelines", streamWriterWriteLines, "!($self, lines, /)"),
    PY_METHOD("reset", streamWriterReset, "!($self, /)"),
};

static constexpr TypeDef streamWriterTypeDef { "_multibytecodec.MultibyteStreamWriter"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, streamWriterMethods, streamGetSets, { } };

// ---- The module ----

// What getcodec() of the modules of the codecs calls, with the codec in a capsule.
static JSValue createCodec(JSGlobalObject* globalObject, JSValue capsule)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    const void* codec = pointerOfCapsule(globalObject, capsule, nameOfCodecCapsule);
    if (!codec)
        return raiseValueError(globalObject, scope, "argument type invalid"_s);
    PyRecord* object = PyRecord::create(globalObject, runtime(globalObject).typeFor(codecTypeDef), 0);
    object->number = static_cast<int64_t>(std::bit_cast<intptr_t>(codec));
    return object;
}

static constexpr NativeDef moduleFunctions[] = {
    PY_FUNCTION("__create_codec", createCodec, "(arg, /)"),
};

} // namespace CJK

void initializeMultibyteCodecModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "MultibyteIncrementalEncoder"_s, rt.typeFor(CJK::incrementalEncoderTypeDef));
    module->add(globalObject, "MultibyteIncrementalDecoder"_s, rt.typeFor(CJK::incrementalDecoderTypeDef));
    module->add(globalObject, "MultibyteStreamReader"_s, rt.typeFor(CJK::streamReaderTypeDef));
    module->add(globalObject, "MultibyteStreamWriter"_s, rt.typeFor(CJK::streamWriterTypeDef));
    module->addFunctions(globalObject, CJK::moduleFunctions);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
