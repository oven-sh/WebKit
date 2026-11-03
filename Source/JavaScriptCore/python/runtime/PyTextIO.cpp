/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyIO.h"

#include "JSCInlines.h"
#include "PyCodecs.h"
#include "PyCommandLine.h"
#include "PyModuleHelpers.h"
#include "PyType.h"
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// IncrementalNewlineDecoder and TextIOWrapper. (Modules/_io/textio.c in CPython.)

// False: a TypeError.
static bool textIOCheckDecoded(JSGlobalObject* globalObject, JSValue decoded)
{
    if (!decoded)
        return false;
    if (isStr(decoded)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, "decoder should return a string result, not '"_s, typeName(globalObject, decoded), '\'');
    return false;
}

bool ioIsLegalNewline(const String& newline)
{
    return newline.isEmpty() || newline == "\n"_s || newline == "\r"_s || newline == "\r\n"_s;
}

// The low 64 bits of an int. `message` is for what is not one.
static bool textIOToUInt64Mask(JSGlobalObject* globalObject, JSValue value, ASCIILiteral message, uint64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasIndex(globalObject, value)) {
        raiseTypeError(globalObject, scope, message);
        return false;
    }
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (auto number = tryInt64(index)) {
        result = static_cast<uint64_t>(*number);
        return true;
    }
    JSValue masked = intAnd(globalObject, index, makeInt(globalObject, std::numeric_limits<uint64_t>::max()));
    RETURN_IF_EXCEPTION(scope, false);
    result = tryUInt64(masked).value_or(0);
    return true;
}

// The two items of what a decoder's getstate() returns, or of the argument of its setstate(). False: a TypeError.
static bool textIOUnpackState(JSGlobalObject* globalObject, JSValue state, ASCIILiteral notATuple, ASCIILiteral wrongLength, JSValue& buffer, JSValue& flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* tuple = PyTuple::tryFrom(state);
    if (!tuple) {
        raiseTypeError(globalObject, scope, notATuple);
        return false;
    }
    if (tuple->length() != 2) {
        raiseTypeError(globalObject, scope, wrongLength);
        return false;
    }
    buffer = tuple->at(0);
    flags = tuple->at(1);
    return true;
}

// ---- The decoders of the most common encodings ----
//
// A TextIOWrapper gets its decoder from the registry of codecs, which takes importing the package `encodings`. For these
// encodings it makes do with one of its own, which does what the incremental decoder in the registry does. No program ever
// sees it.

namespace {

enum class BuiltinCodec : uint8_t { None, UTF8, ASCII, Latin1 };

enum BuiltinDecoderField : unsigned {
    BuiltinDecoderPendingField, // bytes: the start of a character whose end has not been seen yet.
    BuiltinDecoderErrorsField, // A str.
    BuiltinDecoderFieldCount,
};
// PyRecord::number is the BuiltinCodec.

} // namespace

static BuiltinCodec textIOBuiltinCodec(const String& encoding)
{
    if (encoding.length() > 10)
        return BuiltinCodec::None;
    StrBuilder builder;
    for (unsigned i = 0; i < encoding.length(); ++i) {
        char16_t character = encoding[i];
        builder.append(character == '-' || character == ' ' ? static_cast<char16_t>('_') : toASCIILower(character));
    }
    String name = builder.toString();
    if (name == "utf_8"_s || name == "utf8"_s)
        return BuiltinCodec::UTF8;
    if (name == "ascii"_s || name == "us_ascii"_s)
        return BuiltinCodec::ASCII;
    if (name == "latin_1"_s || name == "latin1"_s || name == "iso_8859_1"_s || name == "iso8859_1"_s)
        return BuiltinCodec::Latin1;
    return BuiltinCodec::None;
}

static PyRecord* builtinDecoderCreate(JSGlobalObject* globalObject, BuiltinCodec codec, JSString* errors)
{
    Runtime& rt = runtime(globalObject);
    PyRecord* decoder = PyRecord::create(globalObject, ioType(globalObject, IOType::BuiltinDecoder), BuiltinDecoderFieldCount);
    decoder->set(rt.vm(), BuiltinDecoderPendingField, rt.emptyBytes());
    decoder->set(rt.vm(), BuiltinDecoderErrorsField, errors);
    decoder->number = static_cast<int64_t>(codec);
    return decoder;
}

static JSValue builtinDecoderDecode(JSGlobalObject* globalObject, PyRecord* decoder, JSValue input, bool isFinal)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    IOBuffer buffer;
    if (!buffer.acquire(globalObject, input))
        return { };
    String errors = asString(decoder->get(BuiltinDecoderErrorsField))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    switch (static_cast<BuiltinCodec>(decoder->number)) {
    case BuiltinCodec::ASCII:
        RELEASE_AND_RETURN(scope, codecDecodeASCII(globalObject, buffer.span(), errors));
    case BuiltinCodec::Latin1:
        RELEASE_AND_RETURN(scope, codecDecodeLatin1(globalObject, buffer.span()));
    default:
        break;
    }

    std::span<const uint8_t> data = buffer.span();
    PyBytes* pending = PyBytes::from(decoder->get(BuiltinDecoderPendingField));
    PyBytes* joined = nullptr;
    if (pending->length()) {
        joined = ioTryNewBytes(globalObject, pending->length() + data.size());
        RETURN_IF_EXCEPTION(scope, { });
        memcpy(joined->mutableData(), pending->data(), pending->length());
        memcpy(joined->mutableData() + pending->length(), data.data(), data.size());
        data = joined->span();
    }
    size_t consumed = data.size();
    JSValue result = codecDecodeUTF8(globalObject, data, errors, isFinal ? nullptr : &consumed);
    RETURN_IF_EXCEPTION(scope, { });
    decoder->set(vm, BuiltinDecoderPendingField, consumed < data.size() ? PyBytes::create(globalObject, data.subspan(consumed)) : rt.emptyBytes());
    ensureStillAliveHere(joined);
    return result;
}

static JSValue builtinDecoderDecodeMethod(JSGlobalObject* globalObject, JSValue self, JSValue input, JSValue finalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isFinal = finalValue && isTrue(globalObject, finalValue);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, builtinDecoderDecode(globalObject, PyRecord::from(self), input, isFinal));
}

static JSValue builtinDecoderGetState(JSGlobalObject* globalObject, JSValue self)
{
    return PyTuple::create(globalObject, { PyRecord::from(self)->get(BuiltinDecoderPendingField), jsNumber(0) });
}

static JSValue builtinDecoderSetState(JSGlobalObject* globalObject, JSValue self, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue buffer, flags;
    if (!textIOUnpackState(globalObject, state, "state argument must be a tuple"_s, "setstate(): illegal state argument"_s, buffer, flags))
        return { };
    if (!PyBytes::is(buffer))
        return raiseTypeError(globalObject, scope, "setstate(): illegal state argument"_s);
    PyRecord* decoder = PyRecord::from(self);
    if (static_cast<BuiltinCodec>(decoder->number) == BuiltinCodec::UTF8)
        decoder->set(vm, BuiltinDecoderPendingField, buffer);
    return jsUndefined();
}

static JSValue builtinDecoderReset(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord::from(self)->set(getVM(globalObject), BuiltinDecoderPendingField, runtime(globalObject).emptyBytes());
    return jsUndefined();
}

static constexpr NativeDef builtinDecoderMethods[] = {
    PY_METHOD("decode", builtinDecoderDecodeMethod, "($self, /, input, final=False)"),
    PY_METHOD("getstate", builtinDecoderGetState, "($self, /)"),
    PY_METHOD("setstate", builtinDecoderSetState, "($self, state, /)"),
    PY_METHOD("reset", builtinDecoderReset, "($self, /)"),
};

constexpr TypeDef ioBuiltinDecoderTypeDef { "_io._BuiltinDecoder"_s, { }, builtinDecoderMethods, { }, { } };

// ---- IncrementalNewlineDecoder ----

namespace {

enum NewlineDecoderField : unsigned {
    NewlineDecoderDecoderField, // The decoder it wraps, or None if what it is given is text already.
    NewlineDecoderErrorsField, // Empty: __init__ has not been called.
    NewlineDecoderFieldCount,
};

// In PyRecord::number.
enum NewlineDecoderFlag : int64_t {
    NewlineDecoderPendingCR = 1 << 0, // The text so far ended in "\r", which is held back: a "\n" may follow.
    NewlineDecoderTranslate = 1 << 1,
    NewlineDecoderWrapsBuiltin = 1 << 2,
    NewlineDecoderSeenCR = 1 << 3,
    NewlineDecoderSeenLF = 1 << 4,
    NewlineDecoderSeenCRLF = 1 << 5,
    NewlineDecoderSeenAll = NewlineDecoderSeenCR | NewlineDecoderSeenLF | NewlineDecoderSeenCRLF,
};

} // namespace

static bool newlineDecoderCheckInitialized(JSGlobalObject* globalObject, PyRecord* decoder)
{
    if (decoder->get(NewlineDecoderErrorsField)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "IncrementalNewlineDecoder.__init__() not called"_s);
    return false;
}

static JSValue newlineDecoderNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    return PyRecord::create(rt.vm(), rt.structure<PyRecord>(), PyType::from(cls), NewlineDecoderFieldCount);
}

static JSValue newlineDecoderInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "decoder"_s, "translate"_s, "errors"_s };
    JSValue values[std::size(keywords)];
    if (!unpackKeywordArguments(globalObject, "IncrementalNewlineDecoder"_s, keywords, 2, 3, false, args, values))
        return { };
    bool translate = isTrue(globalObject, values[1]);
    RETURN_IF_EXCEPTION(scope, { });

    PyRecord* decoder = PyRecord::from(self);
    decoder->set(vm, NewlineDecoderErrorsField, values[2] ? values[2] : JSValue(rt.names.strict()));
    decoder->set(vm, NewlineDecoderDecoderField, values[0]);
    decoder->number = (translate ? NewlineDecoderTranslate : 0) | (ioTryExact(globalObject, values[0], IOType::BuiltinDecoder) ? NewlineDecoderWrapsBuiltin : 0);
    return jsUndefined();
}

// Which kinds of line endings the text has, and how many of them are "\r\n".
template<typename CharacterType>
static int64_t newlineDecoderScan(std::span<const CharacterType> text, size_t& pairs)
{
    int64_t seen = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        CharacterType character = text[i];
        if (character > '\r')
            continue;
        if (character == '\n')
            seen |= NewlineDecoderSeenLF;
        else if (character == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                seen |= NewlineDecoderSeenCRLF;
                ++pairs;
                ++i;
            } else
                seen |= NewlineDecoderSeenCR;
        }
    }
    return seen;
}

template<typename CharacterType, typename OutputType>
static void newlineDecoderTranslate(std::span<const CharacterType> text, std::span<OutputType> output)
{
    size_t out = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        CharacterType character = text[i];
        if (character == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            character = '\n';
        }
        output[out++] = character;
    }
    ASSERT(out == output.size());
}

JSValue newlineDecoderDecode(JSGlobalObject* globalObject, PyRecord* self, JSValue input, bool isFinal)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!newlineDecoderCheckInitialized(globalObject, self))
        return { };

    JSValue output = input;
    if (self->number & NewlineDecoderWrapsBuiltin)
        output = builtinDecoderDecode(globalObject, PyRecord::from(self->get(NewlineDecoderDecoderField)), input, isFinal);
    else if (JSValue decoder = self->get(NewlineDecoderDecoderField); !isNone(decoder))
        output = callMethod(globalObject, decoder, ioName(globalObject, IOName::decode), input, jsBoolean(isFinal));
    if (!textIOCheckDecoded(globalObject, output))
        return { };

    String text = strValue(output)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    bool isModified = false;
    if ((self->number & NewlineDecoderPendingCR) && (isFinal || !text.isEmpty())) {
        text = makeString('\r', text);
        self->number &= ~NewlineDecoderPendingCR;
        isModified = true;
    }
    // Even when nothing is translated: that way readline() gets a "\r\n" in one piece.
    if (!isFinal && !text.isEmpty() && text[text.length() - 1] == '\r') {
        text = text.left(text.length() - 1);
        self->number |= NewlineDecoderPendingCR;
        isModified = true;
    }

    if (!text.isEmpty()) {
        if (!text.contains('\r')) {
            if (!(self->number & NewlineDecoderSeenLF) && text.contains('\n'))
                self->number |= NewlineDecoderSeenLF;
        } else if ((self->number & NewlineDecoderTranslate) || (self->number & NewlineDecoderSeenAll) != NewlineDecoderSeenAll) {
            size_t pairs = 0;
            self->number |= text.is8Bit() ? newlineDecoderScan(byteCast<uint8_t>(text.span8()), pairs) : newlineDecoderScan(text.span16(), pairs);
            if (self->number & NewlineDecoderTranslate) {
                String translated;
                if (text.is8Bit()) {
                    std::span<Latin1Character> data;
                    translated = String::createUninitialized(text.length() - pairs, data);
                    newlineDecoderTranslate(byteCast<uint8_t>(text.span8()), data);
                } else {
                    std::span<char16_t> data;
                    translated = String::createUninitialized(text.length() - pairs, data);
                    newlineDecoderTranslate(text.span16(), data);
                }
                text = WTF::move(translated);
                isModified = true;
            }
        }
    }
    return isModified ? JSValue(makeStr(vm, text)) : output;
}

static JSValue newlineDecoderDecodeMethod(JSGlobalObject* globalObject, JSValue self, JSValue input, JSValue finalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isFinal = finalValue && isTrue(globalObject, finalValue);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newlineDecoderDecode(globalObject, PyRecord::from(self), input, isFinal));
}

static JSValue newlineDecoderGetState(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* decoder = PyRecord::from(self);
    if (!newlineDecoderCheckInitialized(globalObject, decoder))
        return { };
    JSValue buffer = runtime(globalObject).emptyBytes();
    uint64_t flag = 0;
    if (JSValue wrapped = decoder->get(NewlineDecoderDecoderField); !isNone(wrapped)) {
        JSValue state = callMethod(globalObject, wrapped, ioName(globalObject, IOName::getstate));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue flagValue;
        if (!textIOUnpackState(globalObject, state, "illegal decoder state"_s, "illegal decoder state"_s, buffer, flagValue))
            return { };
        if (!textIOToUInt64Mask(globalObject, flagValue, "illegal decoder state"_s, flag))
            return { };
    }
    flag <<= 1;
    if (decoder->number & NewlineDecoderPendingCR)
        flag |= 1;
    return PyTuple::create(globalObject, { buffer, makeInt(globalObject, flag) });
}

static JSValue newlineDecoderSetState(JSGlobalObject* globalObject, JSValue self, JSValue state)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* decoder = PyRecord::from(self);
    if (!newlineDecoderCheckInitialized(globalObject, decoder))
        return { };
    JSValue buffer, flagValue;
    if (!textIOUnpackState(globalObject, state, "state argument must be a tuple"_s, "setstate(): illegal state argument"_s, buffer, flagValue))
        return { };
    uint64_t flag;
    if (!textIOToUInt64Mask(globalObject, flagValue, "setstate(): illegal state argument"_s, flag))
        return { };
    if (flag & 1)
        decoder->number |= NewlineDecoderPendingCR;
    else
        decoder->number &= ~NewlineDecoderPendingCR;
    flag >>= 1;
    JSValue wrapped = decoder->get(NewlineDecoderDecoderField);
    if (isNone(wrapped))
        return jsUndefined();
    RELEASE_AND_RETURN(scope, callMethod(globalObject, wrapped, ioName(globalObject, IOName::setstate), PyTuple::create(globalObject, { buffer, makeInt(globalObject, flag) })));
}

static JSValue newlineDecoderReset(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* decoder = PyRecord::from(self);
    if (!newlineDecoderCheckInitialized(globalObject, decoder))
        return { };
    decoder->number &= ~(NewlineDecoderSeenAll | NewlineDecoderPendingCR);
    JSValue wrapped = decoder->get(NewlineDecoderDecoderField);
    if (isNone(wrapped))
        return jsUndefined();
    return callMethod(globalObject, wrapped, ioName(globalObject, IOName::reset));
}

static JSValue newlineDecoderGetNewlines(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* decoder = PyRecord::from(self);
    if (!newlineDecoderCheckInitialized(globalObject, decoder))
        return { };
    JSValue seen[3];
    unsigned count = 0;
    if (decoder->number & NewlineDecoderSeenCR)
        seen[count++] = makeStr(vm, "\r"_s);
    if (decoder->number & NewlineDecoderSeenLF)
        seen[count++] = makeStr(vm, "\n"_s);
    if (decoder->number & NewlineDecoderSeenCRLF)
        seen[count++] = makeStr(vm, "\r\n"_s);
    if (!count)
        return jsUndefined();
    if (count == 1)
        return seen[0];
    return PyTuple::create(globalObject, std::span<const JSValue> { seen, count });
}

static constexpr NativeDef newlineDecoderMethods[] = {
    PY_NEW(newlineDecoderNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", newlineDecoderInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("decode", newlineDecoderDecodeMethod, "($self, /, input, final=False)"),
    PY_METHOD("getstate", newlineDecoderGetState, "($self, /)"),
    PY_METHOD("setstate", newlineDecoderSetState, "($self, state, /)"),
    PY_METHOD("reset", newlineDecoderReset, "($self, /)"),
};

static constexpr NativeGetSet newlineDecoderGetSets[] = {
    { "newlines"_s, newlineDecoderGetNewlines },
};

constexpr TypeDef ioIncrementalNewlineDecoderTypeDef { "_io.IncrementalNewlineDecoder"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, newlineDecoderMethods, newlineDecoderGetSets,
    "Codec used when reading a file in universal newlines mode.\n\n"
    "It wraps another incremental decoder, translating \\r\\n and \\r into \\n. It also records the types of newlines\n"
    "encountered. When used with translate=False, it ensures that the newline sequence is returned in one piece. When\n"
    "used with decoder=None, it expects unicode strings as decode input and translates newlines without first invoking\n"
    "an external decoder."_s };

// ---- TextIOWrapper ----

namespace {

// The encodings that are written without going through the encoder found in the registry.
enum class TextIOEncoding : uint8_t { Other, ASCII, Latin1, UTF8, UTF16BE, UTF16LE, UTF16, UTF32BE, UTF32LE, UTF32 };

struct TextIOState {
    bool isInitialized { false };
    bool isDetached { false };
    bool isExactType { false };
    bool lineBuffering { false };
    bool writeThrough { false };
    bool readUniversal { false };
    bool readTranslate { false };
    bool writeTranslate { false };
    bool seekable { false };
    bool hasRead1 { false };
    bool telling { false };
    bool finalizing { false };
    bool encodingStartOfStream { false };
    bool decoderIsNewlineDecoder { false }; // An IncrementalNewlineDecoder itself, no subclass.
    bool decoderIsBuiltin { false };
    bool isListedForPendingOutput { false }; // See ioNotePendingOutput().
    uint8_t writeNewlineLength { 0 }; // What "\n" is written as. 0: itself. 1: "\r". 2: "\r\n".
    TextIOEncoding encoding { TextIOEncoding::Other };
    int64_t chunkSize { 8192 };
    int64_t decodedUsed { 0 }; // How much of the decoded text has been read, in code units.
    int64_t pendingCount { 0 }; // How many bytes are waiting to be written.
    double bytesPerCharacter { 0 }; // In the last chunk: where tell() starts looking.
};

enum TextIOField : unsigned {
    TextIOStateField,
    TextIOBufferField, // Empty: none yet, or detached.
    TextIOEncodingField, // A str.
    TextIOEncoderField, // Empty: the stream cannot be written to. True: none is needed.
    TextIODecoderField, // Empty: the stream cannot be read from.
    TextIOReadNewlineField, // A str, or empty for None.
    TextIOErrorsField, // A str.
    TextIODecodedField, // A str that is no rope: text that has been decoded and not all read yet. Or empty.
    TextIOPendingField, // A PyByteStorage, or empty: text that has been encoded and not written yet.
    // A point in the file where the decoder had nothing buffered, for tell(): the flags of the decoder's state there, and
    // the bytes read since, as bytes. Both empty: none.
    TextIOSnapshotFlagsField,
    TextIOSnapshotInputField,
    TextIORawField, // The FileIO under the buffer, if that is what there is, to find out fast whether it is closed.
    TextIOFieldCount,
};

// What tell() returns, as one int, and seek() takes: where to start reading, and how to get from there to the position.
struct TextIOCookie {
    int64_t startPosition { 0 };
    int decoderFlags { 0 };
    int bytesToFeed { 0 };
    int charactersToSkip { 0 };
    bool needEOF { false };
};

class TextStream {
public:
    TextStream(JSGlobalObject* globalObject, JSValue self)
        : globalObject(globalObject)
        , vm(getVM(globalObject))
        , record(PyRecord::from(self))
        , s(ioStateOf<TextIOState>(record))
    {
    }

    JSValue get(TextIOField field) const { return record->get(field); }
    void set(TextIOField field, JSValue value) { record->set(vm, field, value); }
    void clear(TextIOField field) { record->set(vm, field, JSValue()); }
    JSString* name(IOName id) const { return ioName(globalObject, id); }

    // ---- False: an exception ----
    bool checkInitialized();
    bool checkAttached();
    bool checkClosed();
    bool setNewline(const String&);
    bool setDecoder(BuiltinCodec, JSValue codecInfo, JSString* errors);
    bool setEncoder(BuiltinCodec, JSValue codecInfo, JSString* errors);
    bool fixEncoderState();
    bool changeEncoding(JSValue encoding, JSValue errors, bool newlineChanged);
    bool resetEncoder(bool startOfStream);
    bool resetDecoder();
    bool setDecoderState(const TextIOCookie&);
    // The length of the bytes the decoder holds, and its flags.
    bool getDecoderState(int64_t& bufferLength, int& flags);
    // How many characters the decoder makes of the bytes.
    bool countDecoded(std::span<const uint8_t>, bool isFinal, int64_t& count);
    bool writeToBuffer(PyBytes*);
    bool writeFlush();

    // The buffer. Empty: there is none, a ValueError.
    JSValue safeBuffer();
    JSValue callBuffer(JSString* method);
    JSValue callBuffer(JSString* method, JSValue argument);
    JSValue getBufferAttribute(JSString* attribute);
    JSValue closedValue();

    JSValue encode(JSString*);
    JSValue decode(JSValue bytes, bool isFinal);

    bool hasSnapshot() const { return !!get(TextIOSnapshotInputField); }
    void clearSnapshot()
    {
        clear(TextIOSnapshotFlagsField);
        clear(TextIOSnapshotInputField);
    }
    void setDecoded(JSValue text)
    {
        set(TextIODecodedField, text);
        s.decodedUsed = 0;
    }
    // Up to `count` characters of the decoded text, all of it if that is negative. `taken` is how many there were.
    String takeDecoded(int64_t count, int64_t& taken);
    // 1, 0 at the end of the file, or -1 for an exception.
    int readChunk(int64_t sizeHint);
    JSValue readline(int64_t limit);

    JSValue raiseUnsupported(ASCIILiteral message)
    {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return ioRaiseUnsupported(globalObject, scope, message);
    }

    JSGlobalObject* globalObject;
    VM& vm;
    PyRecord* record;
    TextIOState& s;
};

} // namespace

JSValue textIOWrappedStream(PyRecord* stream) { return stream->get(TextIOBufferField); }

bool TextStream::checkInitialized()
{
    if (s.isInitialized) [[likely]]
        return true;
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "I/O operation on uninitialized object"_s);
    return false;
}

bool TextStream::checkAttached()
{
    if (!checkInitialized())
        return false;
    if (!s.isDetached) [[likely]]
        return true;
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "underlying buffer has been detached"_s);
    return false;
}

JSValue TextStream::safeBuffer()
{
    if (JSValue buffer = get(TextIOBufferField)) [[likely]]
        return buffer;
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, s.isInitialized ? "underlying buffer has been detached"_s : "I/O operation on uninitialized object"_s);
}

JSValue TextStream::callBuffer(JSString* method)
{
    JSValue buffer = safeBuffer();
    return buffer ? callMethod(globalObject, buffer, method) : JSValue();
}

JSValue TextStream::callBuffer(JSString* method, JSValue argument)
{
    JSValue buffer = safeBuffer();
    return buffer ? callMethod(globalObject, buffer, method, argument) : JSValue();
}

JSValue TextStream::getBufferAttribute(JSString* attribute)
{
    JSValue buffer = safeBuffer();
    return buffer ? getAttr(globalObject, buffer, attribute) : JSValue();
}

// One that is not, or no longer, put together counts as closed: whoever is taking it apart sees to the buffer.
JSValue TextStream::closedValue()
{
    if (!s.isInitialized)
        return jsBoolean(true);
    return getBufferAttribute(name(IOName::closed));
}

bool TextStream::checkClosed()
{
    if (!s.isExactType)
        return ioCheckClosed(globalObject, record);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isClosed;
    if (JSValue raw = get(TextIORawField))
        isClosed = fileIOIsClosed(PyRecord::from(raw));
    else {
        JSValue closed = closedValue();
        RETURN_IF_EXCEPTION(scope, false);
        isClosed = isTrue(globalObject, closed);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (isClosed) {
        ioRaiseClosed(globalObject, scope);
        return false;
    }
    return true;
}

// ---- Encoding and decoding ----

JSValue TextStream::encode(JSString* text)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String errors = asString(get(TextIOErrorsField))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    // After the start of the stream there is no byte order mark, and the byte order is that of the machine.
    int unmarkedOrder = s.encodingStartOfStream ? 0 : -1;
    scope.release();
    switch (s.encoding) {
    case TextIOEncoding::ASCII:
        return codecEncodeASCII(globalObject, text, errors);
    case TextIOEncoding::Latin1:
        return codecEncodeLatin1(globalObject, text, errors);
    case TextIOEncoding::UTF8:
        return codecEncodeUTF8(globalObject, text, errors);
    case TextIOEncoding::UTF16BE:
        return codecEncodeUTF16(globalObject, text, errors, 1);
    case TextIOEncoding::UTF16LE:
        return codecEncodeUTF16(globalObject, text, errors, -1);
    case TextIOEncoding::UTF16:
        return codecEncodeUTF16(globalObject, text, errors, unmarkedOrder);
    case TextIOEncoding::UTF32BE:
        return codecEncodeUTF32(globalObject, text, errors, 1);
    case TextIOEncoding::UTF32LE:
        return codecEncodeUTF32(globalObject, text, errors, -1);
    case TextIOEncoding::UTF32:
        return codecEncodeUTF32(globalObject, text, errors, unmarkedOrder);
    case TextIOEncoding::Other:
        break;
    }
    return callMethod(globalObject, get(TextIOEncoderField), name(IOName::encode), text);
}

JSValue TextStream::decode(JSValue bytes, bool isFinal)
{
    JSValue decoder = get(TextIODecoderField);
    JSValue text;
    if (s.decoderIsNewlineDecoder)
        text = newlineDecoderDecode(globalObject, PyRecord::from(decoder), bytes, isFinal);
    else if (s.decoderIsBuiltin)
        text = builtinDecoderDecode(globalObject, PyRecord::from(decoder), bytes, isFinal);
    else
        text = callMethod(globalObject, decoder, name(IOName::decode), bytes, jsBoolean(isFinal));
    return textIOCheckDecoded(globalObject, text) ? text : JSValue();
}

bool TextStream::setNewline(const String& newline)
{
    set(TextIOReadNewlineField, newline.isNull() ? JSValue() : JSValue(makeStr(vm, newline)));
    s.readUniversal = newline.isEmpty();
    s.readTranslate = newline.isNull();
    s.writeTranslate = newline.isNull() || !newline.isEmpty();
    // With None, "\n" is written as os.linesep, which is "\n".
    s.writeNewlineLength = !s.readUniversal && newline != "\n"_s ? newline.length() : 0;
    return true;
}

bool TextStream::setDecoder(BuiltinCodec codec, JSValue codecInfo, JSString* errors)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue readable = callBuffer(name(IOName::readable));
    RETURN_IF_EXCEPTION(scope, false);
    bool isReadable = isTrue(globalObject, readable);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isReadable)
        return true;

    clear(TextIODecoderField);
    s.decoderIsBuiltin = false;
    s.decoderIsNewlineDecoder = false;
    JSValue decoder;
    if (codec != BuiltinCodec::None)
        decoder = builtinDecoderCreate(globalObject, codec, errors);
    else {
        String errorsText = errors->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        decoder = codecIncrementalDecoder(globalObject, codecInfo, errorsText);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (s.readUniversal) {
        decoder = call(globalObject, ioType(globalObject, IOType::IncrementalNewlineDecoder), decoder, jsBoolean(s.readTranslate));
        RETURN_IF_EXCEPTION(scope, false);
        s.decoderIsNewlineDecoder = true;
    } else
        s.decoderIsBuiltin = codec != BuiltinCodec::None;
    set(TextIODecoderField, decoder);
    return true;
}

bool TextStream::setEncoder(BuiltinCodec codec, JSValue codecInfo, JSString* errors)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue writable = callBuffer(name(IOName::writable));
    RETURN_IF_EXCEPTION(scope, false);
    bool isWritable = isTrue(globalObject, writable);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isWritable)
        return true;

    clear(TextIOEncoderField);
    s.encoding = TextIOEncoding::Other;
    switch (codec) {
    case BuiltinCodec::UTF8:
        s.encoding = TextIOEncoding::UTF8;
        break;
    case BuiltinCodec::ASCII:
        s.encoding = TextIOEncoding::ASCII;
        break;
    case BuiltinCodec::Latin1:
        s.encoding = TextIOEncoding::Latin1;
        break;
    case BuiltinCodec::None:
        break;
    }
    if (codec != BuiltinCodec::None) {
        set(TextIOEncoderField, jsBoolean(true));
        return true;
    }

    String errorsText = errors->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue encoder = codecIncrementalEncoder(globalObject, codecInfo, errorsText);
    RETURN_IF_EXCEPTION(scope, false);
    set(TextIOEncoderField, encoder);

    JSValue codecName = getAttrIfPresent(globalObject, codecInfo, runtime(globalObject).names.name());
    RETURN_IF_EXCEPTION(scope, false);
    if (codecName && isStr(codecName)) {
        String text = strValue(codecName)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        static constexpr std::pair<ASCIILiteral, TextIOEncoding> table[] = {
            { "ascii"_s, TextIOEncoding::ASCII },
            { "iso8859-1"_s, TextIOEncoding::Latin1 },
            { "utf-8"_s, TextIOEncoding::UTF8 },
            { "utf-16-be"_s, TextIOEncoding::UTF16BE },
            { "utf-16-le"_s, TextIOEncoding::UTF16LE },
            { "utf-16"_s, TextIOEncoding::UTF16 },
            { "utf-32-be"_s, TextIOEncoding::UTF32BE },
            { "utf-32-le"_s, TextIOEncoding::UTF32LE },
            { "utf-32"_s, TextIOEncoding::UTF32 },
        };
        for (auto& [encodingName, encoding] : table) {
            if (text == encodingName) {
                s.encoding = encoding;
                break;
            }
        }
    }
    return true;
}

bool TextStream::resetEncoder(bool startOfStream)
{
    s.encodingStartOfStream = startOfStream;
    JSValue encoder = get(TextIOEncoderField);
    if (encoder.isBoolean())
        return true;
    if (startOfStream)
        return !!callMethod(globalObject, encoder, name(IOName::reset));
    return !!callMethod(globalObject, encoder, name(IOName::setstate), jsNumber(0));
}

// An encoder that starts with a byte order mark is not to write one in the middle of a file.
bool TextStream::fixEncoderState()
{
    if (!s.seekable || !get(TextIOEncoderField))
        return true;
    auto scope = DECLARE_THROW_SCOPE(vm);
    s.encodingStartOfStream = true;
    JSValue position = callBuffer(name(IOName::tell));
    RETURN_IF_EXCEPTION(scope, false);
    bool isAtStart = isEqual(globalObject, position, jsNumber(0));
    RETURN_IF_EXCEPTION(scope, false);
    if (isAtStart)
        return true;
    RELEASE_AND_RETURN(scope, resetEncoder(false));
}

bool TextStream::resetDecoder()
{
    return !!callMethod(globalObject, get(TextIODecoderField), name(IOName::reset));
}

bool TextStream::setDecoderState(const TextIOCookie& cookie)
{
    // At the start of the stream the state of some decoders is not (b"", 0): that of UTF-16 expects a byte order mark.
    if (!cookie.startPosition && !cookie.decoderFlags)
        return resetDecoder();
    return !!callMethod(globalObject, get(TextIODecoderField), name(IOName::setstate), PyTuple::create(globalObject, { runtime(globalObject).emptyBytes(), jsNumber(cookie.decoderFlags) }));
}

bool TextStream::getDecoderState(int64_t& bufferLength, int& flags)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue state = callMethod(globalObject, get(TextIODecoderField), name(IOName::getstate));
    RETURN_IF_EXCEPTION(scope, false);
    JSValue buffer, flagsValue;
    if (!textIOUnpackState(globalObject, state, "illegal decoder state"_s, "illegal decoder state"_s, buffer, flagsValue))
        return false;
    flags = toCInt(globalObject, flagsValue);
    RETURN_IF_EXCEPTION(scope, false);
    auto* bytes = PyBytes::tryFrom(buffer);
    if (!bytes) {
        raiseTypeError(globalObject, scope, "illegal decoder state: the first item should be a bytes object, not '"_s, typeName(globalObject, buffer), '\'');
        return false;
    }
    bufferLength = bytes->length();
    return true;
}

bool TextStream::countDecoded(std::span<const uint8_t> bytes, bool isFinal, int64_t& count)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue text = decode(PyBytes::create(globalObject, bytes), isFinal);
    RETURN_IF_EXCEPTION(scope, false);
    count = strLength(globalObject, strValue(text));
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// ---- Construction ----

static JSValue textIONew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    PyRecord* stream = ioAllocate<TextIOState>(globalObject, cls, TextIOFieldCount);
    ioStateOf<TextIOState>(stream).isExactType = !stream->type()->isHeapType();
    ioDidAllocate(globalObject, stream);
    return stream;
}

// What the locale says text is encoded in.
// FIXME: Ask the locale, once the interpreter sets one up. CPython makes the "C" locale one that uses UTF-8 as it starts.
static ASCIILiteral textIOLocaleEncoding() { return "UTF-8"_s; }

static JSValue textIOInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "buffer"_s, "encoding"_s, "errors"_s, "newline"_s, "line_buffering"_s, "write_through"_s };
    JSValue values[std::size(keywords)];
    if (!unpackKeywordArguments(globalObject, "TextIOWrapper"_s, keywords, 1, 6, false, args, values))
        return { };
    JSValue buffer = values[0];
    String encoding, newline;
    if (!ioToOptionalString(globalObject, values[1], "TextIOWrapper"_s, "encoding"_s, encoding))
        return { };
    JSValue errorsValue = values[2] ? values[2] : jsUndefined();
    if (!ioToOptionalString(globalObject, values[3], "TextIOWrapper"_s, "newline"_s, newline))
        return { };
    bool lineBuffering = values[4] && isTrue(globalObject, values[4]);
    RETURN_IF_EXCEPTION(scope, { });
    bool writeThrough = values[5] && isTrue(globalObject, values[5]);
    RETURN_IF_EXCEPTION(scope, { });

    TextStream stream(globalObject, self);
    auto& s = stream.s;
    s.isInitialized = false;
    s.isDetached = false;

    if (encoding.isNull() && configuration().warnDefaultEncoding && !warn(globalObject, rt.EncodingWarning(), "'encoding' argument not specified"_s))
        return { };
    JSString* errors = rt.names.strict();
    if (!isNone(errorsValue)) {
        String ignored;
        if (!ioToOptionalString(globalObject, errorsValue, "TextIOWrapper"_s, "errors"_s, ignored))
            return { };
        errors = strValue(errorsValue);
    }
    if (!ioIsLegalNewline(newline))
        return raiseValueError(globalObject, scope, "illegal newline value: "_s, newline);

    if (JSValue previous = stream.get(TextIOBufferField); previous && previous != buffer)
        ioDidUnwrap(globalObject, previous);
    for (unsigned field = TextIOBufferField; field < TextIOFieldCount; ++field)
        stream.clear(static_cast<TextIOField>(field));
    s.decodedUsed = 0;
    s.pendingCount = 0;
    s.encoding = TextIOEncoding::Other;
    s.decoderIsBuiltin = false;
    s.decoderIsNewlineDecoder = false;
    s.bytesPerCharacter = 0;

    if (encoding.isNull())
        encoding = "utf-8"_s;
    else if (encoding == "locale"_s)
        encoding = textIOLocaleEncoding();

    BuiltinCodec codec = textIOBuiltinCodec(encoding);
    JSValue codecInfo;
    if (codec == BuiltinCodec::None) {
        codecInfo = codecLookupTextEncoding(globalObject, encoding, ASCIILiteral());
        RETURN_IF_EXCEPTION(scope, { });
    }
    stream.set(TextIOEncodingField, makeStr(vm, encoding));
    stream.set(TextIOErrorsField, errors);
    s.chunkSize = 8192;
    s.lineBuffering = lineBuffering;
    s.writeThrough = writeThrough;
    stream.setNewline(newline);
    stream.set(TextIOBufferField, buffer);

    if (!stream.setDecoder(codec, codecInfo, errors) || !stream.setEncoder(codec, codecInfo, errors))
        return { };

    if (auto* record = PyRecord::tryFrom(buffer); record && !record->type()->isHeapType()) {
        PyType* type = record->type();
        if (type == ioType(globalObject, IOType::BufferedReader) || type == ioType(globalObject, IOType::BufferedWriter) || type == ioType(globalObject, IOType::BufferedRandom)) {
            if (JSValue raw = bufferedWrappedStream(record); ioTryExact(globalObject, raw, IOType::FileIO))
                stream.set(TextIORawField, raw);
        }
    }

    JSValue seekable = callMethod(globalObject, buffer, stream.name(IOName::seekable));
    RETURN_IF_EXCEPTION(scope, { });
    s.seekable = s.telling = isTrue(globalObject, seekable);
    RETURN_IF_EXCEPTION(scope, { });
    s.hasRead1 = hasAttr(globalObject, buffer, stream.name(IOName::read1));
    RETURN_IF_EXCEPTION(scope, { });
    s.encodingStartOfStream = false;
    if (!stream.fixEncoderState())
        return { };

    s.isInitialized = true;
    ioDidOpen(globalObject, stream.record);
    ioDidWrap(globalObject, buffer);
    return jsUndefined();
}

// None is the default; anything else must be an int.
static bool textIOToOptionalBool(JSGlobalObject* globalObject, JSValue value, bool defaultValue, bool& result)
{
    if (!value || isNone(value)) {
        result = defaultValue;
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t number = toInt64(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    result = !!number;
    return true;
}

bool TextStream::changeEncoding(JSValue encodingValue, JSValue errorsValue, bool newlineChanged)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(encodingValue) && isNone(errorsValue) && !newlineChanged)
        return true;

    // What is not given stays as it is, except that a new encoding comes with "strict".
    JSString* encoding;
    JSString* errors = isNone(errorsValue) ? nullptr : strValue(errorsValue);
    if (isNone(encodingValue)) {
        encoding = asString(get(TextIOEncodingField));
        if (!errors)
            errors = asString(get(TextIOErrorsField));
    } else {
        encoding = strValue(encodingValue);
        if (!errors)
            errors = runtime(globalObject).names.strict();
    }
    String encodingText = encoding->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (encodingText == "locale"_s) {
        encodingText = textIOLocaleEncoding();
        encoding = makeStr(vm, encodingText);
    }
    if (!ioCheckUTF8(globalObject, encoding) || !ioCheckUTF8(globalObject, errors))
        return false;

    BuiltinCodec codec = textIOBuiltinCodec(encodingText);
    JSValue codecInfo;
    if (codec == BuiltinCodec::None) {
        codecInfo = codecLookupTextEncoding(globalObject, encodingText, ASCIILiteral());
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!setDecoder(codec, codecInfo, errors) || !setEncoder(codec, codecInfo, errors))
        return false;
    set(TextIOEncodingField, encoding);
    set(TextIOErrorsField, errors);
    RELEASE_AND_RETURN(scope, fixEncoderState());
}

static JSValue textIOReconfigure(JSGlobalObject* globalObject, JSValue self, JSValue encoding, JSValue errors, JSValue newlineValue, JSValue lineBufferingValue, JSValue writeThroughValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    TextStream stream(globalObject, self);
    auto& s = stream.s;
    if (!encoding)
        encoding = jsUndefined();
    if (!errors)
        errors = jsUndefined();

    auto checkStr = [&](JSValue value, ASCIILiteral argument) {
        if (!value || isNone(value) || isStr(value))
            return true;
        raiseTypeError(globalObject, scope, "reconfigure() argument '"_s, argument, "' must be str or None, not "_s, typeName(globalObject, value));
        return false;
    };
    if (!checkStr(encoding, "encoding"_s) || !checkStr(errors, "errors"_s) || !checkStr(newlineValue, "newline"_s))
        return { };

    if (stream.get(TextIODecodedField) && (!isNone(encoding) || !isNone(errors) || newlineValue))
        return stream.raiseUnsupported("It is not possible to set the encoding or newline of stream after the first read"_s);

    String newline;
    if (newlineValue && !isNone(newlineValue)) {
        newline = strValue(newlineValue)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!ioCheckUTF8(globalObject, strValue(newlineValue)))
            return { };
        if (newline.isNull())
            newline = emptyString();
        if (!ioIsLegalNewline(newline))
            return raiseValueError(globalObject, scope, "illegal newline value: "_s, newline);
    }
    bool lineBuffering, writeThrough;
    if (!textIOToOptionalBool(globalObject, lineBufferingValue, s.lineBuffering, lineBuffering) || !textIOToOptionalBool(globalObject, writeThroughValue, s.writeThrough, writeThrough))
        return { };

    if (!ioFlush(globalObject, self))
        return { };
    s.bytesPerCharacter = 0;
    if (newlineValue)
        stream.setNewline(newline);
    if (!stream.changeEncoding(encoding, errors, !!newlineValue))
        return { };
    s.lineBuffering = lineBuffering;
    s.writeThrough = writeThrough;
    return jsUndefined();
}

static JSValue textIODetach(JSGlobalObject* globalObject, JSValue self)
{
    TextStream stream(globalObject, self);
    if (!ioFlush(globalObject, self))
        return { };
    // Flushing may have run anything.
    JSValue buffer = stream.safeBuffer();
    if (!buffer)
        return { };
    stream.clear(TextIOBufferField);
    stream.s.isDetached = true;
    ioDidClose(globalObject, stream.record);
    ioDidUnwrap(globalObject, buffer);
    return buffer;
}

// ---- Writing ----

bool TextStream::writeToBuffer(PyBytes* bytes)
{
    JSString* write = runtime(globalObject).names.write();
    JSValue result;
    do {
        result = callBuffer(write, bytes);
    } while (!result && ioTrapEINTR(globalObject));
    return !!result;
}

// Hands what has been encoded to the buffer, which is not flushed in turn.
bool TextStream::writeFlush()
{
    if (!s.pendingCount)
        return true;
    auto* storage = uncheckedDowncast<PyByteStorage>(get(TextIOPendingField).asCell());
    PyBytes* bytes = PyBytes::create(globalObject, std::span<const uint8_t> { storage->data(), static_cast<size_t>(s.pendingCount) });
    // If this fails, there is no telling how much has been written: what was pending is gone either way.
    s.pendingCount = 0;
    return writeToBuffer(bytes);
}

void textIOWriteOutPending(JSGlobalObject* globalObject, PyRecord* record)
{
    TextStream stream(globalObject, record);
    stream.s.isListedForPendingOutput = false;
    if (!stream.s.isInitialized || stream.s.isDetached || !stream.s.pendingCount)
        return;
    if (!stream.writeFlush())
        reportUnraisable(globalObject, makeString("Exception ignored while flushing "_s, reprForMessage(globalObject, record)), { });
}

bool textIOHasPendingOutput(PyRecord* stream)
{
    return ioStateOf<TextIOState>(stream).pendingCount;
}

static JSValue textIOWrite(JSGlobalObject* globalObject, JSValue self, JSValue textValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(textValue))
        return raiseTypeError(globalObject, scope, "write() argument must be str, not "_s, isNone(textValue) ? "None"_str : typeName(globalObject, textValue));
    TextStream stream(globalObject, self);
    auto& s = stream.s;
    if (!stream.checkAttached() || !stream.checkClosed())
        return { };
    if (!stream.get(TextIOEncoderField))
        return stream.raiseUnsupported("not writable"_s);

    JSString* string = strValue(textValue);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    size_t textLength = ioCountCodePoints(text);

    bool translates = s.writeTranslate && s.writeNewlineLength;
    bool hasLF = (translates || s.lineBuffering) && text.contains('\n');
    if (hasLF && translates) {
        text = s.writeNewlineLength == 1 ? makeStringByReplacingAll(text, '\n', '\r') : makeStringByReplacingAll(text, '\n', "\r\n"_s);
        string = makeStr(vm, text);
    }
    bool needsFlush = s.lineBuffering && (hasLF || text.contains('\r'));

    // ASCII text is its own encoding in most encodings.
    PyBytes* encoded = nullptr;
    std::span<const uint8_t> bytes;
    bool isASCIICompatible = s.encoding == TextIOEncoding::ASCII || s.encoding == TextIOEncoding::Latin1 || s.encoding == TextIOEncoding::UTF8;
    if (isASCIICompatible && text.is8Bit() && text.containsOnlyASCII())
        bytes = byteCast<uint8_t>(text.span8());
    else {
        JSValue result = stream.encode(string);
        RETURN_IF_EXCEPTION(scope, { });
        encoded = PyBytes::tryFrom(result);
        if (!encoded)
            return raiseTypeError(globalObject, scope, "encoder should return a bytes object, not '"_s, typeName(globalObject, result), '\'');
        bytes = encoded->span();
    }
    if (s.encoding != TextIOEncoding::Other)
        s.encodingStartOfStream = false;

    int64_t byteCount = bytes.size();
    if (byteCount >= s.chunkSize) {
        // Too much to be worth copying. What the buffer's write() runs may write to this stream in turn.
        while (s.pendingCount) {
            if (!stream.writeFlush())
                return { };
        }
        if (!encoded)
            encoded = PyBytes::create(globalObject, bytes);
        if (!stream.writeToBuffer(encoded))
            return { };
    } else if (byteCount) {
        JSValue pending = stream.get(TextIOPendingField);
        auto* storage = pending ? uncheckedDowncast<PyByteStorage>(pending.asCell()) : nullptr;
        size_t needed = s.pendingCount + byteCount;
        if (!storage || storage->capacity() < needed) {
            PyByteStorage* larger = ioTryNewStorage(globalObject, std::max<size_t>(needed, 2 * s.chunkSize));
            RETURN_IF_EXCEPTION(scope, { });
            if (s.pendingCount)
                memcpy(larger->data(), storage->data(), s.pendingCount);
            stream.set(TextIOPendingField, larger);
            storage = larger;
        }
        memcpy(storage->data() + s.pendingCount, bytes.data(), byteCount);
        s.pendingCount += byteCount;
        if (!s.isListedForPendingOutput && s.isExactType && stream.get(TextIORawField)) {
            s.isListedForPendingOutput = true;
            ioNotePendingOutput(globalObject, stream.record);
        }
    }
    ensureStillAliveHere(encoded);

    if (s.pendingCount >= s.chunkSize || needsFlush || s.writeThrough) {
        if (!stream.writeFlush())
            return { };
    }
    if (needsFlush) {
        JSValue buffer = stream.safeBuffer();
        if (!buffer || !ioFlush(globalObject, buffer))
            return { };
    }

    if (stream.hasSnapshot()) {
        stream.setDecoded({ });
        stream.clearSnapshot();
    }
    if (stream.get(TextIODecoderField) && !stream.resetDecoder())
        return { };
    return makeInt(globalObject, static_cast<uint64_t>(textLength));
}

// ---- Reading ----

String TextStream::takeDecoded(int64_t count, int64_t& taken)
{
    taken = 0;
    JSValue decoded = get(TextIODecodedField);
    if (!decoded)
        return emptyString();
    String text = stringOf(asString(decoded));
    size_t start = s.decodedUsed;
    size_t end = text.length();
    if (text.is8Bit()) {
        if (count >= 0 && static_cast<size_t>(count) < end - start)
            end = start + count;
        taken = end - start;
    } else if (count < 0)
        taken = ioCountCodePoints(StringView(text).substring(start));
    else {
        auto units = text.span16();
        size_t i = start;
        for (; taken < count && i < units.size(); ++taken)
            i += i + 1 < units.size() && U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1]) ? 2 : 1;
        end = i;
    }
    s.decodedUsed = end;
    if (!start && end == text.length())
        return text;
    return text.substring(start, end - start);
}

// Reads a chunk from the buffer and decodes it. The text replaces what had been decoded before. The decoder is given the whole
// chunk, some of which it may keep for later.
int TextStream::readChunk(int64_t sizeHint)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!get(TextIODecoderField)) {
        raiseUnsupported("not readable"_s);
        return -1;
    }

    // For tell(): what the decoder holds was read right after a point where it held nothing.
    PyBytes* decoderBuffer = nullptr;
    JSValue decoderFlags;
    if (s.telling) {
        JSValue state = callMethod(globalObject, get(TextIODecoderField), name(IOName::getstate));
        RETURN_IF_EXCEPTION(scope, -1);
        JSValue buffer;
        if (!textIOUnpackState(globalObject, state, "illegal decoder state"_s, "illegal decoder state"_s, buffer, decoderFlags))
            return -1;
        decoderBuffer = PyBytes::tryFrom(buffer);
        if (!decoderBuffer) {
            raiseTypeError(globalObject, scope, "illegal decoder state: the first item should be a bytes object, not '"_s, typeName(globalObject, buffer), '\'');
            return -1;
        }
    }

    if (sizeHint > 0)
        sizeHint = static_cast<int64_t>(std::max(s.bytesPerCharacter, 1.0) * static_cast<double>(sizeHint));
    JSValue chunk = callBuffer(s.hasRead1 ? name(IOName::read1) : rt.names.read(), makeInt(globalObject, std::max(s.chunkSize, sizeHint)));
    RETURN_IF_EXCEPTION(scope, -1);
    if (!isBytesLike(chunk)) {
        raiseTypeError(globalObject, scope, "underlying "_s, s.hasRead1 ? "read1"_s : "read"_s, "() should have returned a bytes-like object, not '"_s, typeName(globalObject, chunk), '\'');
        return -1;
    }
    IOBuffer chunkBytes;
    if (!chunkBytes.acquire(globalObject, chunk))
        return -1;
    size_t byteCount = chunkBytes.size();
    bool isAtEnd = !byteCount;

    JSValue decoded = decode(chunk, isAtEnd);
    RETURN_IF_EXCEPTION(scope, -1);
    JSString* string = strValue(decoded);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, -1);
    setDecoded(string);
    size_t characterCount = ioCountCodePoints(text);
    s.bytesPerCharacter = characterCount ? static_cast<double>(byteCount) / static_cast<double>(characterCount) : 0;
    if (characterCount)
        isAtEnd = false;

    if (s.telling) {
        PyBytes* nextInput = ioTryNewBytes(globalObject, decoderBuffer->length() + byteCount);
        RETURN_IF_EXCEPTION(scope, -1);
        memcpy(nextInput->mutableData(), decoderBuffer->data(), decoderBuffer->length());
        memcpy(nextInput->mutableData() + decoderBuffer->length(), chunkBytes.span().data(), byteCount);
        set(TextIOSnapshotFlagsField, decoderFlags);
        set(TextIOSnapshotInputField, nextInput);
    }
    return !isAtEnd;
}

static JSValue textIORead(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (!ioToOptionalSize(globalObject, sizeValue, size))
        return { };
    TextStream stream(globalObject, self);
    if (!stream.checkAttached() || !stream.checkClosed())
        return { };
    if (!stream.get(TextIODecoderField))
        return stream.raiseUnsupported("not readable"_s);
    if (!stream.writeFlush())
        return { };

    int64_t taken;
    if (size < 0) {
        JSValue bytes = stream.callBuffer(rt.names.read());
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(bytes))
            return raise(globalObject, scope, rt.BlockingIOError(), "Read returned None."_s);
        JSValue decoded = stream.decode(bytes, true);
        RETURN_IF_EXCEPTION(scope, { });
        String before = stream.takeDecoded(-1, taken);
        if (stream.hasSnapshot()) {
            stream.setDecoded({ });
            stream.clearSnapshot();
        }
        if (before.isEmpty())
            return strValue(decoded);
        RELEASE_AND_RETURN(scope, jsString(globalObject, makeStr(vm, before), strValue(decoded)));
    }

    String first = stream.takeDecoded(size, taken);
    int64_t remaining = size - taken;
    if (remaining <= 0)
        return makeStr(vm, first);
    StrBuilder result;
    result.append(first);
    while (remaining > 0) {
        int status = stream.readChunk(remaining);
        if (status < 0) {
            if (ioTrapEINTR(globalObject))
                continue;
            return { };
        }
        if (!status)
            break;
        result.append(stream.takeDecoded(remaining, taken));
        remaining -= taken;
    }
    if (result.hasOverflowed())
        return raiseWithArgs(globalObject, scope, rt.MemoryError(), { });
    return makeStr(globalObject, result);
}

JSValue TextStream::readline(int64_t limit)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!checkClosed() || !writeFlush())
        return { };

    uint8_t newline[2] = { };
    size_t newlineLength = 0;
    if (JSValue readNewline = get(TextIOReadNewlineField)) {
        String text = stringOf(asString(readNewline));
        newlineLength = text.length();
        for (size_t i = 0; i < newlineLength; ++i)
            newline[i] = static_cast<uint8_t>(text[i]);
    }

    // Offsets are in code units; the limit, and `chunked`, which is only kept up if there is a limit, are in characters.
    String line; // Null: none.
    String remaining; // The end of a chunk that may be the start of a line ending.
    StrBuilder chunks;
    bool hasChunks = false;
    int64_t chunked = 0;
    size_t start = 0;
    size_t end = 0;
    size_t offsetToBuffer = 0;
    auto lengthOf = [&](size_t from, size_t to) -> int64_t { return ioCountCodePoints(StringView(line).substring(from, to - from)); };

    while (true) {
        int status = 1;
        while (!get(TextIODecodedField) || !asString(get(TextIODecodedField))->length()) {
            status = readChunk(0);
            if (status < 0) {
                if (ioTrapEINTR(globalObject))
                    continue;
                return { };
            }
            if (!status)
                break;
        }
        if (!status) {
            setDecoded({ });
            clearSnapshot();
            start = end = offsetToBuffer = 0;
            break;
        }

        String decoded = stringOf(asString(get(TextIODecodedField)));
        if (remaining.isNull()) {
            line = decoded;
            start = s.decodedUsed;
            offsetToBuffer = 0;
        } else {
            line = makeString(remaining, decoded);
            start = 0;
            offsetToBuffer = remaining.length();
            remaining = String();
        }

        int64_t consumed = 0;
        int64_t found = line.is8Bit()
            ? ioFindLineEnding<uint8_t>(s.readTranslate, s.readUniversal, { newline, newlineLength }, byteCast<uint8_t>(line.span8()).subspan(start), consumed)
            : ioFindLineEnding<char16_t>(s.readTranslate, s.readUniversal, { newline, newlineLength }, line.span16().subspan(start), consumed);
        end = start + (found >= 0 ? found : consumed);
        if (limit >= 0 && lengthOf(start, end) + chunked >= limit) {
            end = ioSkipCodePoints(line, start, limit - chunked);
            break;
        }
        if (found >= 0)
            break;

        // No line ending yet: what cannot be part of one is put aside.
        if (end > start) {
            hasChunks = true;
            chunks.append(StringView(line).substring(start, end - start));
            if (limit >= 0)
                chunked += lengthOf(start, end);
        }
        if (end < line.length())
            remaining = line.substring(end);
        line = String();
        setDecoded({ });
    }

    if (!line.isNull()) {
        // The line ends in the text that was decoded last.
        s.decodedUsed = end - offsetToBuffer;
        if (start > 0 || end < line.length())
            line = line.substring(start, end - start);
    }
    if (!remaining.isNull()) {
        hasChunks = true;
        chunks.append(remaining);
    }
    if (hasChunks) {
        if (!line.isNull())
            chunks.append(line);
        if (chunks.hasOverflowed())
            return raiseWithArgs(globalObject, scope, rt.MemoryError(), { });
        line = chunks.toString();
    }
    return makeStr(vm, line);
}

static JSValue textIOReadline(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    int64_t size = -1;
    if (sizeValue && !ssizeArgument(globalObject, sizeValue, size))
        return { };
    TextStream stream(globalObject, self);
    if (!stream.checkAttached())
        return { };
    return stream.readline(size);
}

static JSValue textIONext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    TextStream stream(globalObject, self);
    if (!stream.checkAttached())
        return { };
    stream.s.telling = false;

    JSValue line;
    if (stream.s.isExactType)
        line = stream.readline(-1);
    else {
        line = callMethod(globalObject, self, rt.names.readline());
        if (line && !isStr(line))
            return raise(globalObject, scope, rt.OSError(), makeString("readline() should have returned a str object, not '"_s, typeName(globalObject, line), '\''));
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!strValue(line)->length()) {
        stream.clearSnapshot();
        stream.s.telling = stream.s.seekable;
        return raiseStopIteration(globalObject, scope);
    }
    return line;
}

// ---- The position ----

static constexpr size_t textIOCookieSize = sizeof(int64_t) + 3 * sizeof(int) + 1;

static JSValue textIOBuildCookie(JSGlobalObject* globalObject, const TextIOCookie& cookie)
{
    uint8_t bytes[textIOCookieSize];
    memcpy(bytes, &cookie.startPosition, 8);
    memcpy(bytes + 8, &cookie.decoderFlags, 4);
    memcpy(bytes + 12, &cookie.bytesToFeed, 4);
    memcpy(bytes + 16, &cookie.charactersToSkip, 4);
    bytes[20] = cookie.needEOF;
    return intFromByteSpan(globalObject, bytes, true, false);
}

static bool textIOParseCookie(JSGlobalObject* globalObject, JSValue value, TextIOCookie& cookie)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue number = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    number = intValue(number);
    if (intSign(number) < 0) {
        raiseOverflowError(globalObject, scope, "can't convert negative int to unsigned"_s);
        return false;
    }
    if (intBitLength(number) > textIOCookieSize * 8) {
        raiseOverflowError(globalObject, scope, "int too big to convert"_s);
        return false;
    }
    // The next `bits` bits.
    auto take = [&](unsigned bits) -> uint64_t {
        if (number.isInt32() && !number.asInt32())
            return 0;
        if (auto small = tryUInt64(number); small && bits == 64) {
            number = jsNumber(0);
            return *small;
        }
        JSValue low = intAnd(globalObject, number, makeInt(globalObject, bits == 64 ? std::numeric_limits<uint64_t>::max() : (static_cast<uint64_t>(1) << bits) - 1));
        RETURN_IF_EXCEPTION(scope, 0);
        number = intRightShift(globalObject, number, jsNumber(bits));
        RETURN_IF_EXCEPTION(scope, 0);
        return tryUInt64(low).value_or(0);
    };
    cookie.startPosition = static_cast<int64_t>(take(64));
    cookie.decoderFlags = static_cast<int>(static_cast<uint32_t>(take(32)));
    cookie.bytesToFeed = static_cast<int>(static_cast<uint32_t>(take(32)));
    cookie.charactersToSkip = static_cast<int>(static_cast<uint32_t>(take(32)));
    cookie.needEOF = !!take(8);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static JSValue textIOSeek(JSGlobalObject* globalObject, JSValue self, JSValue cookieValue, JSValue whenceValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int whence = 0;
    if (whenceValue && !ioToInt(globalObject, whenceValue, whence))
        return { };
    TextStream stream(globalObject, self);
    auto& s = stream.s;
    if (!stream.checkAttached() || !stream.checkClosed())
        return { };
    if (!s.seekable)
        return stream.raiseUnsupported("underlying stream is not seekable"_s);

    if (whence == SEEK_CUR) {
        bool isZero = isEqual(globalObject, cookieValue, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isZero)
            return stream.raiseUnsupported("can't do nonzero cur-relative seeks"_s);
        // Going to where the stream is brings the buffer there too.
        cookieValue = callMethod(globalObject, self, stream.name(IOName::tell));
        RETURN_IF_EXCEPTION(scope, { });
    } else if (whence == SEEK_END) {
        bool isZero = isEqual(globalObject, cookieValue, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isZero)
            return stream.raiseUnsupported("can't do nonzero end-relative seeks"_s);
        if (!ioFlush(globalObject, self))
            return { };
        stream.setDecoded({ });
        stream.clearSnapshot();
        if (stream.get(TextIODecoderField) && !stream.resetDecoder())
            return { };
        JSValue buffer = stream.safeBuffer();
        RETURN_IF_EXCEPTION(scope, { });
        JSValue position = callMethod(globalObject, buffer, stream.name(IOName::seek), jsNumber(0), jsNumber(2));
        RETURN_IF_EXCEPTION(scope, { });
        if (stream.get(TextIOEncoderField)) {
            bool isAtStart = isEqual(globalObject, position, jsNumber(0));
            RETURN_IF_EXCEPTION(scope, { });
            if (!stream.resetEncoder(isAtStart))
                return { };
        }
        return position;
    } else if (whence != SEEK_SET)
        return raiseValueError(globalObject, scope, "invalid whence ("_s, whence, ", should be "_s, SEEK_SET, ", "_s, SEEK_CUR, " or "_s, SEEK_END, ')');

    bool isNegative = richCompareBool(globalObject, cookieValue, jsNumber(0), CompareOperator::Lt);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNegative) {
        JSString* text = repr(globalObject, cookieValue);
        RETURN_IF_EXCEPTION(scope, { });
        String string = text->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return raiseValueError(globalObject, scope, "negative seek position "_s, string);
    }
    if (!ioFlush(globalObject, self))
        return { };

    // Back to the safe point the cookie names; from there, what read(charactersToSkip) would do.
    TextIOCookie cookie;
    if (!textIOParseCookie(globalObject, cookieValue, cookie))
        return { };
    stream.callBuffer(stream.name(IOName::seek), makeInt(globalObject, cookie.startPosition));
    RETURN_IF_EXCEPTION(scope, { });
    stream.setDecoded({ });
    stream.clearSnapshot();
    if (stream.get(TextIODecoderField) && !stream.setDecoderState(cookie))
        return { };

    if (cookie.charactersToSkip) {
        JSValue chunk = stream.callBuffer(rt.names.read(), jsNumber(cookie.bytesToFeed));
        RETURN_IF_EXCEPTION(scope, { });
        if (!PyBytes::is(chunk))
            return raiseTypeError(globalObject, scope, "underlying read() should have returned a bytes object, not '"_s, typeName(globalObject, chunk), '\'');
        stream.set(TextIOSnapshotFlagsField, jsNumber(cookie.decoderFlags));
        stream.set(TextIOSnapshotInputField, chunk);
        if (!stream.get(TextIODecoderField))
            return stream.raiseUnsupported("not readable"_s);
        JSValue decoded = stream.decode(chunk, cookie.needEOF);
        RETURN_IF_EXCEPTION(scope, { });
        JSString* string = strValue(decoded);
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        stream.setDecoded(string);
        if (ioCountCodePoints(text) < static_cast<size_t>(cookie.charactersToSkip))
            return raise(globalObject, scope, rt.OSError(), "can't restore logical file position"_s);
        s.decodedUsed = ioSkipCodePoints(text, 0, cookie.charactersToSkip);
    } else {
        stream.set(TextIOSnapshotFlagsField, jsNumber(cookie.decoderFlags));
        stream.set(TextIOSnapshotInputField, rt.emptyBytes());
    }

    // For the byte order mark.
    if (stream.get(TextIOEncoderField) && !stream.resetEncoder(!cookie.startPosition && !cookie.decoderFlags))
        return { };
    return cookieValue;
}

static JSValue textIOTell(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    TextStream stream(globalObject, self);
    auto& s = stream.s;
    if (!stream.checkAttached() || !stream.checkClosed())
        return { };
    if (!s.seekable)
        return stream.raiseUnsupported("underlying stream is not seekable"_s);
    if (!s.telling)
        return raise(globalObject, scope, rt.OSError(), "telling position disabled by next() call"_s);
    if (!stream.writeFlush() || !ioFlush(globalObject, self))
        return { };

    JSValue position = stream.callBuffer(stream.name(IOName::tell));
    RETURN_IF_EXCEPTION(scope, { });
    if (!stream.get(TextIODecoderField) || !stream.hasSnapshot())
        return position;

    TextIOCookie cookie;
    cookie.startPosition = toInt64(globalObject, position);
    RETURN_IF_EXCEPTION(scope, { });

    // Back to the snapshot.
    cookie.decoderFlags = toCInt(globalObject, stream.get(TextIOSnapshotFlagsField));
    RETURN_IF_EXCEPTION(scope, { });
    PyBytes* nextInput = PyBytes::from(stream.get(TextIOSnapshotInputField));
    cookie.startPosition -= nextInput->length();
    if (!s.decodedUsed)
        RELEASE_AND_RETURN(scope, textIOBuildCookie(globalObject, cookie));

    // How many characters have been read since.
    int64_t charactersToSkip = ioCountCodePoints(StringView(stringOf(asString(stream.get(TextIODecodedField)))).left(s.decodedUsed));

    // The decoder is put through the bytes again, and is given its state back at the end.
    JSValue savedState = callMethod(globalObject, stream.get(TextIODecoderField), stream.name(IOName::getstate));
    RETURN_IF_EXCEPTION(scope, { });

    auto search = [&]() -> bool {
        std::span<const uint8_t> input = nextInput->span();
        int64_t decoderBufferLength;
        int decoderFlags;
        int64_t decodedCount;

        // A guess at a point close to the position, from where the decoder can start with nothing buffered.
        int64_t skipBytes = static_cast<int64_t>(s.bytesPerCharacter * static_cast<double>(charactersToSkip));
        skipBytes = std::min<int64_t>(skipBytes, input.size());
        int64_t skipBack = 1;
        while (skipBytes > 0) {
            if (!stream.setDecoderState(cookie) || !stream.countDecoded(input.first(skipBytes), false, decodedCount))
                return false;
            if (decodedCount <= charactersToSkip) {
                if (!stream.getDecoderState(decoderBufferLength, decoderFlags))
                    return false;
                if (!decoderBufferLength) {
                    cookie.decoderFlags = decoderFlags;
                    charactersToSkip -= decodedCount;
                    break;
                }
                skipBytes -= decoderBufferLength;
                skipBack = 1;
            } else {
                // Too far.
                skipBytes -= skipBack;
                skipBack *= 2;
            }
        }
        if (skipBytes <= 0) {
            skipBytes = 0;
            if (!stream.setDecoderState(cookie))
                return false;
        }
        cookie.startPosition += skipBytes;
        cookie.charactersToSkip = static_cast<int>(charactersToSkip);
        if (!charactersToSkip)
            return true;

        // From there, a byte at a time up to the position, noting the last point where the decoder had nothing buffered.
        decodedCount = 0;
        size_t index = skipBytes;
        for (; index < input.size(); ++index) {
            int64_t count;
            if (!stream.countDecoded(input.subspan(index, 1), false, count))
                return false;
            decodedCount += count;
            ++cookie.bytesToFeed;
            if (!stream.getDecoderState(decoderBufferLength, decoderFlags))
                return false;
            if (!decoderBufferLength && decodedCount <= charactersToSkip) {
                cookie.startPosition += cookie.bytesToFeed;
                charactersToSkip -= decodedCount;
                cookie.decoderFlags = decoderFlags;
                cookie.bytesToFeed = 0;
                decodedCount = 0;
            }
            if (decodedCount >= charactersToSkip)
                break;
        }
        if (index == input.size()) {
            // Not enough: the rest came out of the decoder when it was told there was no more.
            int64_t count;
            if (!stream.countDecoded({ }, true, count))
                return false;
            decodedCount += count;
            cookie.needEOF = true;
            if (decodedCount < charactersToSkip) {
                raise(globalObject, scope, rt.OSError(), "can't reconstruct logical file position"_s);
                return false;
            }
        }
        cookie.charactersToSkip = static_cast<int>(charactersToSkip);
        return true;
    };

    bool found = search();
    ensureStillAliveHere(nextInput);
    JSValue error = found ? JSValue() : takePendingException(globalObject);
    callMethod(globalObject, stream.get(TextIODecoderField), stream.name(IOName::setstate), savedState);
    ioChainExceptions(globalObject, error);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, textIOBuildCookie(globalObject, cookie));
}

static JSValue textIOTruncate(JSGlobalObject* globalObject, JSValue self, JSValue position)
{
    TextStream stream(globalObject, self);
    if (!stream.checkAttached() || !ioFlush(globalObject, self))
        return { };
    return stream.callBuffer(stream.name(IOName::truncate), position ? position : jsUndefined());
}

// ---- The rest ----

static JSValue textIORepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    TextStream stream(globalObject, self);
    if (!stream.checkInitialized())
        return { };
    String ownName = typeName(globalObject, self);
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return raiseRuntimeError(globalObject, scope, "reentrant call inside "_s, ownName, ".__repr__"_s);

    StrBuilder result;
    result.append('<', ownName);
    auto append = [&](ASCIILiteral label, JSValue value) {
        JSString* text = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        String string = text->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        result.append(label, string);
        return true;
    };

    JSValue name = getAttrIfPresent(globalObject, self, rt.names.name());
    if (scope.exception()) [[unlikely]] {
        // The buffer has been detached.
        if (!clearPendingExceptionIfMatches(globalObject, rt.ValueError()))
            return { };
        name = JSValue();
    }
    if (name && !append(" name="_s, name))
        return { };
    JSValue mode = getAttrIfPresent(globalObject, self, stream.name(IOName::mode));
    RETURN_IF_EXCEPTION(scope, { });
    if (mode && !append(" mode="_s, mode))
        return { };
    if (!append(" encoding="_s, stream.get(TextIOEncodingField)))
        return { };
    result.append('>');
    return makeStr(globalObject, result);
}

static JSValue textIOFileno(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).callBuffer(ioName(globalObject, IOName::fileno)); }
static JSValue textIOSeekable(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).callBuffer(ioName(globalObject, IOName::seekable)); }
static JSValue textIOReadable(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).callBuffer(ioName(globalObject, IOName::readable)); }
static JSValue textIOWritable(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).callBuffer(ioName(globalObject, IOName::writable)); }
static JSValue textIOIsATTY(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).callBuffer(ioName(globalObject, IOName::isatty)); }

static JSValue textIOFlush(JSGlobalObject* globalObject, JSValue self)
{
    TextStream stream(globalObject, self);
    if (!stream.checkAttached() || !stream.checkClosed())
        return { };
    stream.s.telling = stream.s.seekable;
    if (!stream.writeFlush())
        return { };
    return stream.callBuffer(runtime(globalObject).names.flush());
}

static JSValue textIOClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    TextStream stream(globalObject, self);
    if (!stream.checkAttached())
        return { };
    JSValue closed = stream.closedValue();
    RETURN_IF_EXCEPTION(scope, { });
    bool isClosed = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, { });
    if (isClosed || stream.s.isDetached)
        return jsUndefined();

    if (stream.s.finalizing) {
        if (!stream.callBuffer(stream.name(IOName::deallocWarn), self))
            takePendingException(globalObject);
    }
    ioFlush(globalObject, self);
    JSValue flushError = takePendingException(globalObject);
    JSValue result = stream.callBuffer(runtime(globalObject).names.close());
    ioDidClose(globalObject, stream.record);
    scope.release();
    if (flushError) {
        ioChainExceptions(globalObject, flushError);
        return { };
    }
    return result;
}

static JSValue textIOGetName(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).getBufferAttribute(runtime(globalObject).names.name()); }
static JSValue textIOGetClosed(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).closedValue(); }
static JSValue textIOGetBuffer(JSGlobalObject* globalObject, JSValue self) { return TextStream(globalObject, self).safeBuffer(); }

static JSValue textIOGetNewlines(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    TextStream stream(globalObject, self);
    if (!stream.checkAttached())
        return { };
    JSValue decoder = stream.get(TextIODecoderField);
    if (!decoder)
        return jsUndefined();
    JSValue newlines = getAttrIfPresent(globalObject, decoder, stream.name(IOName::newlines));
    RETURN_IF_EXCEPTION(scope, { });
    return newlines ? newlines : jsUndefined();
}

static JSValue textIOGetErrors(JSGlobalObject* globalObject, JSValue self)
{
    TextStream stream(globalObject, self);
    if (!stream.checkInitialized())
        return { };
    return stream.get(TextIOErrorsField);
}

static JSValue textIOGetChunkSize(JSGlobalObject* globalObject, JSValue self)
{
    TextStream stream(globalObject, self);
    if (!stream.checkAttached())
        return { };
    return makeInt(globalObject, stream.s.chunkSize);
}

static void textIOSetChunkSize(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raise(globalObject, scope, rt.AttributeError(), makeString("attribute '_CHUNK_SIZE' of '"_s, typeName(globalObject, self), "' objects cannot be deleted"_s));
        return;
    }
    TextStream stream(globalObject, self);
    if (!stream.checkAttached())
        return;
    int64_t size = toSsize(globalObject, value, rt.ValueError());
    RETURN_IF_EXCEPTION(scope, void());
    if (size <= 0) {
        raiseValueError(globalObject, scope, "a strictly positive integer is required"_s);
        return;
    }
    stream.s.chunkSize = size;
}

static JSValue textIOGetEncoding(JSGlobalObject*, JSValue self)
{
    JSValue encoding = PyRecord::from(self)->get(TextIOEncodingField);
    return encoding ? encoding : jsUndefined();
}

static JSValue textIOGetLineBuffering(JSGlobalObject*, JSValue self) { return jsBoolean(ioStateOf<TextIOState>(PyRecord::from(self)).lineBuffering); }
static JSValue textIOGetWriteThrough(JSGlobalObject*, JSValue self) { return jsBoolean(ioStateOf<TextIOState>(PyRecord::from(self)).writeThrough); }
static JSValue textIOGetFinalizing(JSGlobalObject*, JSValue self) { return jsBoolean(ioStateOf<TextIOState>(PyRecord::from(self)).finalizing); }
static void textIOSetFinalizing(JSGlobalObject* globalObject, JSValue self, JSValue value) { ioToFinalizing(globalObject, value, ioStateOf<TextIOState>(PyRecord::from(self)).finalizing); }

static constexpr NativeDef textIOMethods[] = {
    PY_NEW(textIONew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", textIOInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", textIORepr, "($self, /)"),
    PY_SLOT("__next__", textIONext, "($self, /)"),
    PY_METHOD("detach", textIODetach, "($self, /)"),
    PY_METHOD("reconfigure", textIOReconfigure, "($self, /, *, encoding=None, errors=None, newline=None, line_buffering=None, write_through=None)"),
    PY_METHOD("write", textIOWrite, "($self, text, /)"),
    PY_METHOD("read", textIORead, "($self, size=-1, /)"),
    PY_METHOD("readline", textIOReadline, "($self, size=-1, /)"),
    PY_METHOD("flush", textIOFlush, "($self, /)"),
    PY_METHOD("close", textIOClose, "($self, /)"),
    PY_METHOD("fileno", textIOFileno, "($self, /)"),
    PY_METHOD("seekable", textIOSeekable, "($self, /)"),
    PY_METHOD("readable", textIOReadable, "($self, /)"),
    PY_METHOD("writable", textIOWritable, "($self, /)"),
    PY_METHOD("isatty", textIOIsATTY, "($self, /)"),
    PY_METHOD("seek", textIOSeek, "($self, cookie, whence=0, /)"),
    PY_METHOD("tell", textIOTell, "($self, /)"),
    PY_METHOD("truncate", textIOTruncate, "($self, pos=None, /)"),
    PY_METHOD("__getstate__", ioCannotPickle, "($self, /)"),
};

static constexpr NativeGetSet textIOGetSets[] = {
    { "name"_s, textIOGetName },
    { "closed"_s, textIOGetClosed },
    { "newlines"_s, textIOGetNewlines },
    { "errors"_s, textIOGetErrors },
    { "_CHUNK_SIZE"_s, textIOGetChunkSize, textIOSetChunkSize },
    { "buffer"_s, textIOGetBuffer },
    PY_MEMBER("encoding", textIOGetEncoding),
    PY_MEMBER("line_buffering", textIOGetLineBuffering),
    PY_MEMBER("write_through", textIOGetWriteThrough),
    PY_WRITABLE_MEMBER("_finalizing", textIOGetFinalizing, textIOSetFinalizing),
};

constexpr TypeDef ioTextIOWrapperTypeDef { "_io.TextIOWrapper"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, textIOMethods, textIOGetSets,
    "Character and line based layer over a BufferedIOBase object, buffer.\n\n"
    "encoding gives the name of the encoding that the stream will be decoded or encoded with. errors determines the\n"
    "strictness of encoding and decoding and defaults to \"strict\".\n\n"
    "newline controls how line endings are handled. It can be None, '', '\\n', '\\r', and '\\r\\n'. On input, None and ''\n"
    "make '\\n', '\\r' and '\\r\\n' all end a line, and None translates them into '\\n'; any other value is the only line\n"
    "ending. On output, '\\n' is written as the given string, unless that is '' or '\\n'; None stands for os.linesep.\n\n"
    "If line_buffering is True, a call to flush is implied when a call to write contains a newline character."_s };

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
