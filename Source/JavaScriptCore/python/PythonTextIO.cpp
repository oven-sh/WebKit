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
#include "PythonIO.h"

#include "JSBigInt.h"
#include "PythonCharacters.h"
#include "PythonCodecs.h"
#include "PythonLocale.h"
#include "PythonText.h"
#if OS(UNIX)
#include <langinfo.h>
#include <locale.h>
#include <unistd.h>
#if OS(DARWIN)
#include <xlocale.h>
#endif
#endif

// io.IncrementalNewlineDecoder and io.TextIOWrapper: Modules/_io/textio.c of CPython.

namespace JSC { namespace Python {

// ---- IncrementalNewlineDecoder

namespace {

struct NewlineDecoderState final : NativeState {
    PYTHON_NATIVE_STATE(NewlineDecoderState);

    enum Seen : uint8_t { CR = 1, LF = 2, CRLF = 4, All = CR | LF | CRLF };

    WriteBarrier<Unknown> decoder;
    WriteBarrier<Unknown> errors; // Empty until __init__() has been called.
    bool hasPendingCR { false };
    bool translates { false };
    uint8_t seen { 0 };
};

template<typename Visitor>
void NewlineDecoderState::visit(Visitor& visitor)
{
    visitor.append(decoder);
    visitor.append(errors);
}

bool checkIsInitialized(JSGlobalObject* globalObject, ThrowScope& scope, NewlineDecoderState& state)
{
    if (state.errors)
        return true;
    raiseValueError(globalObject, scope, "IncrementalNewlineDecoder.__init__() not called"_s);
    return false;
}

// check_decoded()
bool checkIsDecoded(JSGlobalObject* globalObject, ThrowScope& scope, JSValue decoded)
{
    if (stringIn(decoded))
        return true;
    raiseTypeError(globalObject, scope, concatenate("decoder should return a string result, not '"_s, typeName(globalObject, decoded), '\''));
    return false;
}

// What is seen of line endings in some text, and the text with each of them made "\n" if that is wanted. They are all characters that are one code unit, so it makes no difference what else is in it.
template<typename Character>
String scanNewlines(std::span<const Character> in, bool translates, uint8_t& seen)
{
    using Seen = NewlineDecoderState::Seen;
    Vector<Character> out;
    if (translates)
        out.reserveInitialCapacity(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        Character c = in[i];
        if (c == '\n')
            seen |= Seen::LF;
        else if (c == '\r') {
            if (i + 1 < in.size() && in[i + 1] == '\n') {
                seen |= Seen::CRLF;
                ++i;
                if (!translates)
                    continue;
            } else
                seen |= Seen::CR;
            c = '\n';
        }
        if (translates)
            out.append(c);
        else if (seen == Seen::All)
            break;
    }
    return translates ? String(out.span()) : String();
}

} // anonymous namespace

JSValue decodeNewlines(JSGlobalObject* globalObject, JSValue self, JSValue input, bool isFinal)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Seen = NewlineDecoderState::Seen;
    auto& state = stateOf<NewlineDecoderState>(self);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };

    // What is given is decoded, along with any "\r" that was kept back last time.
    JSValue output = input;
    if (!isNone(state.decoder.get())) {
        output = callMethodNamed(globalObject, state.decoder.get(), vm.pythonNames().attribute_decode, input, jsBoolean(isFinal));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!checkIsDecoded(globalObject, scope, output))
        return { };
    String text = stringIn(output)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    bool isChanged = false;
    if (state.hasPendingCR && (isFinal || !text.isEmpty())) {
        text = textOrMemoryError(globalObject, tryMakeString('\r', text));
        RETURN_IF_EXCEPTION(scope, { });
        state.hasPendingCR = false;
        isChanged = true;
    }
    // A "\r" at the end is kept back even if nothing is being translated, so that readline() gets "\r\n" all at once.
    if (!isFinal && !text.isEmpty() && text[text.length() - 1] == '\r') {
        text = text.left(text.length() - 1);
        state.hasPendingCR = true;
        isChanged = true;
    }
    auto finish = [&] () -> JSValue {
        // A str, and not an instance of a class derived from it, only if there was something to do.
        return isChanged ? strOrMemoryError(globalObject, text) : output;
    };
    if (text.isEmpty())
        RELEASE_AND_RETURN(scope, finish());

    uint8_t seen = state.seen;
    bool hasNoCR = (seen == Seen::LF || !seen) && !text.contains('\r');
    if (hasNoCR) {
        if (!seen && text.contains('\n'))
            seen |= Seen::LF;
    } else if (!state.translates) {
        if (seen != Seen::All) {
            if (text.is8Bit())
                scanNewlines(text.span8(), false, seen);
            else
                scanNewlines(text.span16(), false, seen);
        }
    } else {
        text = text.is8Bit() ? scanNewlines(text.span8(), true, seen) : scanNewlines(text.span16(), true, seen);
        isChanged = true;
    }
    state.seen |= seen;
    RELEASE_AND_RETURN(scope, finish());
}

// IncrementalNewlineDecoder(decoder, translate, errors='strict')
PYTHON_NATIVE(newlineDecoderInit)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<NewlineDecoderState>(self);
    bool translates = isTrue(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue errors = args.at(3);
    state.errors.set(vm, self, errors ? errors : JSValue(jsNontrivialString(vm, "strict"_s)));
    state.decoder.set(vm, self, args.at(1));
    state.translates = translates;
    state.seen = 0;
    state.hasPendingCR = false;
    RETURN_NONE();
}

PYTHON_NATIVE(newlineDecoderDecode)
{
    NATIVE_PROLOGUE();
    bool isFinal = false;
    if (JSValue value = args.at(2)) {
        isFinal = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeNewlines(globalObject, args[0], args.at(1), isFinal)));
}

// PyArg_ParseTuple(state, "OK;message", &buffer, &flag). False if it raised.
static bool parseDecoderState(JSGlobalObject* globalObject, ThrowScope& scope, JSValue state, ASCIILiteral message, JSValue& buffer, uint64_t& flag)
{
    PyTuple* tuple = asTuple(state);
    if (tuple->length() != 2 || !isInstance(globalObject, tuple->at(1), globalObject->pyRealm()->typeInt())) {
        raiseTypeError(globalObject, scope, message);
        return false;
    }
    buffer = tuple->at(0);
    flag = lowBitsOfInt(tuple->at(1));
    return true;
}

PYTHON_NATIVE(newlineDecoderGetState)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<NewlineDecoderState>(args[0]);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    JSValue buffer;
    uint64_t flag = 0;
    if (!isNone(state.decoder.get())) {
        JSValue inner = callMethodNamed(globalObject, state.decoder.get(), names.attribute_getstate);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isInstance(globalObject, inner, realm->typeTuple()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "illegal decoder state"_s));
        if (!parseDecoderState(globalObject, scope, inner, "illegal decoder state"_s, buffer, flag))
            return { };
    } else {
        buffer = newBytes(globalObject, { });
        RETURN_IF_EXCEPTION(scope, { });
    }
    flag <<= 1;
    if (state.hasPendingCR)
        flag |= 1;
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { buffer, intFromUInt64(globalObject, flag) })));
}

PYTHON_NATIVE(newlineDecoderSetState)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<NewlineDecoderState>(args[0]);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    if (!isInstance(globalObject, args[1], realm->typeTuple()))
        return JSValue::encode(raiseTypeError(globalObject, scope, "state argument must be a tuple"_s));
    JSValue buffer;
    uint64_t flag;
    if (!parseDecoderState(globalObject, scope, args[1], "setstate(): illegal state argument"_s, buffer, flag))
        return { };
    state.hasPendingCR = flag & 1;
    flag >>= 1;
    if (isNone(state.decoder.get()))
        RETURN_NONE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.decoder.get(), names.attribute_setstate, PyTuple::create(globalObject, { buffer, intFromUInt64(globalObject, flag) }))));
}

PYTHON_NATIVE(newlineDecoderReset)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<NewlineDecoderState>(args[0]);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    state.seen = 0;
    state.hasPendingCR = false;
    if (isNone(state.decoder.get()))
        RETURN_NONE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.decoder.get(), names.attribute_reset)));
}

static JSValue getNewlinesSeen(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    using Seen = NewlineDecoderState::Seen;
    auto& state = stateOf<NewlineDecoderState>(self);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    MarkedArgumentBuffer seen;
    if (state.seen & Seen::CR)
        seen.append(jsString(vm, String("\r"_s)));
    if (state.seen & Seen::LF)
        seen.append(jsString(vm, String("\n"_s)));
    if (state.seen & Seen::CRLF)
        seen.append(jsString(vm, String("\r\n"_s)));
    if (seen.isEmpty())
        return jsUndefined();
    if (seen.size() == 1)
        return seen.at(0);
    RELEASE_AND_RETURN(scope, PyTuple::createFromArguments(globalObject, seen));
}

// ---- Finding where a line ends

template<typename Character>
std::optional<size_t> findLineEnding(const LineEndings& endings, std::span<const Character> text, size_t& consumed)
{
    auto findFrom = [&] (size_t from, Character wanted) -> size_t {
        for (size_t i = from; i < text.size(); ++i) {
            if (text[i] == wanted)
                return i;
        }
        return notFound;
    };
    if (endings.isTranslated) {
        size_t found = findFrom(0, '\n');
        if (found != notFound)
            return found + 1;
        consumed = text.size();
        return std::nullopt;
    }
    if (endings.isUniversal) {
        // The decoder sees to it that "\r\n" is not in two pieces.
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n')
                return i + 1;
            if (text[i] == '\r')
                return i + 1 < text.size() && text[i + 1] == '\n' ? i + 2 : i + 1;
        }
        consumed = text.size();
        return std::nullopt;
    }
    auto& newline = endings.readNewline;
    size_t length = newline.length();
    if (length == 1) {
        size_t found = findFrom(0, static_cast<Character>(newline[0]));
        if (found != notFound)
            return found + 1;
        consumed = text.size();
        return std::nullopt;
    }
    // Where the last place is that all of it could begin.
    size_t limit = text.size() >= length - 1 ? text.size() - (length - 1) : 0;
    for (size_t from = 0; from < limit;) {
        size_t found = findFrom(from, static_cast<Character>(newline[0]));
        if (found == notFound || found >= limit)
            break;
        size_t matched = 1;
        while (matched < length && text[found + matched] == newline[matched])
            ++matched;
        if (matched == length)
            return found + length;
        from = found + 1;
    }
    // The beginning of one may be at the end, and the rest of it to come.
    size_t found = findFrom(limit, static_cast<Character>(newline[0]));
    consumed = found == notFound ? text.size() : found;
    return std::nullopt;
}

template std::optional<size_t> findLineEnding(const LineEndings&, std::span<const Latin1Character>, size_t&);
template std::optional<size_t> findLineEnding(const LineEndings&, std::span<const char16_t>, size_t&);
template std::optional<size_t> findLineEnding(const LineEndings&, std::span<const char32_t>, size_t&);

// ---- TextIOWrapper

namespace {

// The encodings that are encoded without going by way of the encoder, which is written in Python: `encodefuncs`.
enum class EncodeFunction : uint8_t { None, ASCII, Latin1, UTF8, UTF16BE, UTF16LE, UTF16, UTF32BE, UTF32LE, UTF32 };

struct TextIOState final : NativeState {
    PYTHON_NATIVE_STATE(TextIOState);

    bool isInitialized { false };
    bool isDetached { false };
    int64_t chunkSize { 0 };
    WriteBarrier<Unknown> buffer;
    WriteBarrier<Unknown> encoding;
    WriteBarrier<Unknown> encoder;
    WriteBarrier<Unknown> decoder;
    WriteBarrier<Unknown> errors;
    LineEndings endings;
    String writeNewline; // What "\n" is written as, if not as it is.
    bool isLineBuffered { false };
    bool writesThrough { false };
    bool translatesOnWriting { false };
    bool isSeekable { false };
    bool hasRead1 { false };
    bool isTelling { false };
    bool isFinalizing { false };
    EncodeFunction encodeFunction { EncodeFunction::None };
    bool isAtStartOfStream { false };

    // What has been decoded and not yet given out. Null if there is none.
    String decodedChars;
    WriteBarrier<Unknown> decodedObject; // The very str that the decoder gave, which is what is given out if it is all of it that is wanted.
    int64_t decodedCharsUsed { 0 }; // In characters.
    // What has been encoded and not yet sent on. Text is encoded as it is written, so that what cannot be encoded is heard of at once.
    ByteVector pendingBytes;
    // (flags, input): the state that the decoder was in at some place where it had nothing kept back, and the bytes that come next from there. tell() works out from it where the decoder has got to.
    WriteBarrier<Unknown> snapshot;
    double bytesPerCharacter { 0 }; // In the last piece that was read, for tell() to guess by.
    WriteBarrier<Unknown> raw; // The raw stream, if it is a FileIO.
};

template<typename Visitor>
void TextIOState::visit(Visitor& visitor)
{
    visitor.append(buffer);
    visitor.append(encoding);
    visitor.append(encoder);
    visitor.append(decoder);
    visitor.append(errors);
    visitor.append(decodedObject);
    visitor.append(snapshot);
    visitor.append(raw);
}

TextIOState& stateOfText(JSValue self) { return stateOf<TextIOState>(self); }

int64_t characterCount(VM& vm, const String& text) { return Characters(vm, text).count(); }

// PyUnicode_Substring()
String substringByCharacters(VM& vm, const String& text, int64_t start, int64_t end)
{
    Characters characters(vm, text);
    unsigned from = characters.codeUnitOf(static_cast<unsigned>(start));
    return text.substring(from, characters.codeUnitOf(static_cast<unsigned>(end)) - from);
}

bool checkIsInitialized(JSGlobalObject* globalObject, ThrowScope& scope, TextIOState& state)
{
    if (state.isInitialized)
        return true;
    raiseValueError(globalObject, scope, "I/O operation on uninitialized object"_s);
    return false;
}

bool checkIsAttached(JSGlobalObject* globalObject, ThrowScope& scope, TextIOState& state)
{
    if (!checkIsInitialized(globalObject, scope, state))
        return false;
    if (!state.isDetached)
        return true;
    raiseValueError(globalObject, scope, "underlying buffer has been detached"_s);
    return false;
}

#define CHECK_ATTACHED() \
    if (!checkIsAttached(globalObject, scope, state)) \
        return { };

// CHECK_CLOSED(). False if it raised.
bool checkIsOpen(JSGlobalObject* globalObject, JSValue self, TextIOState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (typeOf(globalObject, self) != ioState(globalObject).textIOWrapper.get())
        RELEASE_AND_RETURN(scope, checkIsNotClosed(globalObject, self));
    bool isClosed;
    if (state.raw)
        isClosed = isFileIOClosed(state.raw.get());
    else {
        JSValue closed = getAttribute(globalObject, state.buffer.get(), vm.pythonNames().attribute_closed);
        RETURN_IF_EXCEPTION(scope, false);
        isClosed = isTrue(globalObject, closed);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (isClosed)
        raiseValueError(globalObject, scope, "I/O operation on closed file."_s);
    return !isClosed;
}

#define CHECK_CLOSED() \
    if (!checkIsOpen(globalObject, self, state)) \
        return { };

// _PyFile_Flush(). False if it raised.
bool flushFile(JSGlobalObject* globalObject, JSValue file)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    callMethodNamed(globalObject, file, globalObject->vm().pythonNames().attribute_flush);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// set_newline(). A null String is None.
void setNewline(TextIOState& state, const String& newline)
{
    state.endings.readNewline = newline;
    state.endings.isUniversal = newline.isNull() || newline.isEmpty();
    state.endings.isTranslated = newline.isNull();
    state.translatesOnWriting = newline.isNull() || !newline.isEmpty();
    state.writeNewline = !state.endings.isUniversal && newline != "\n"_s ? newline : String();
}

bool validateNewline(JSGlobalObject* globalObject, ThrowScope& scope, const String& newline)
{
    if (isValidNewline(newline))
        return true;
    raiseValueError(globalObject, scope, concatenate("illegal newline value: "_s, newline));
    return false;
}

// Whether what a method of the buffer returns is true. Nothing if it raised.
std::optional<bool> askBuffer(JSGlobalObject* globalObject, TextIOState& state, const Identifier& method)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = callMethodNamed(globalObject, state.buffer.get(), method);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    bool answer = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return answer;
}

// _textiowrapper_set_decoder(). False if it raised.
bool setDecoder(JSGlobalObject* globalObject, JSCell* self, TextIOState& state, JSValue codecInfo, const String& errors)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isReadable = askBuffer(globalObject, state, vm.pythonNames().attribute_readable);
    RETURN_IF_EXCEPTION(scope, false);
    if (!*isReadable)
        return true;
    state.decoder.clear();
    JSValue decoder = makeIncrementalDecoder(globalObject, codecInfo, errors);
    RETURN_IF_EXCEPTION(scope, false);
    state.decoder.set(vm, self, decoder);
    if (state.endings.isUniversal) {
        decoder = call(globalObject, ioState(globalObject).incrementalNewlineDecoder.get(), decoder, jsBoolean(state.endings.isTranslated));
        RETURN_IF_EXCEPTION(scope, false);
        state.decoder.set(vm, self, decoder);
    }
    return true;
}

// _textiowrapper_decode(). Empty if it raised.
JSValue decodeWith(JSGlobalObject* globalObject, JSValue decoder, JSValue bytes, bool isEnd)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue characters;
    if (typeOf(globalObject, decoder) == ioState(globalObject).incrementalNewlineDecoder.get())
        characters = decodeNewlines(globalObject, decoder, bytes, isEnd);
    else
        characters = callMethodNamed(globalObject, decoder, vm.pythonNames().attribute_decode, bytes, jsBoolean(isEnd));
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkIsDecoded(globalObject, scope, characters))
        return { };
    return characters;
}

// _textiowrapper_set_encoder(). False if it raised.
bool setEncoder(JSGlobalObject* globalObject, JSCell* self, TextIOState& state, JSValue codecInfo, const String& errors)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isWritable = askBuffer(globalObject, state, vm.pythonNames().attribute_writable);
    RETURN_IF_EXCEPTION(scope, false);
    if (!*isWritable)
        return true;
    state.encoder.clear();
    state.encodeFunction = EncodeFunction::None;
    JSValue encoder = makeIncrementalEncoder(globalObject, codecInfo, errors);
    RETURN_IF_EXCEPTION(scope, false);
    state.encoder.set(vm, self, encoder);

    // What the codec calls itself.
    JSValue name = getAttributeIfPresent(globalObject, codecInfo, vm.pythonNames().attribute_name);
    RETURN_IF_EXCEPTION(scope, false);
    if (!name || !stringIn(name))
        return true;
    String text = stringIn(name)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    static constexpr std::pair<ASCIILiteral, EncodeFunction> table[] = {
        { "ascii"_s, EncodeFunction::ASCII }, { "iso8859-1"_s, EncodeFunction::Latin1 }, { "utf-8"_s, EncodeFunction::UTF8 }, { "utf-16-be"_s, EncodeFunction::UTF16BE }, { "utf-16-le"_s, EncodeFunction::UTF16LE },
        { "utf-16"_s, EncodeFunction::UTF16 }, { "utf-32-be"_s, EncodeFunction::UTF32BE }, { "utf-32-le"_s, EncodeFunction::UTF32LE }, { "utf-32"_s, EncodeFunction::UTF32 },
    };
    for (auto& [known, function] : table) {
        if (text == known) {
            state.encodeFunction = function;
            break;
        }
    }
    return true;
}

// _textiowrapper_fix_encoder_state(). False if it raised.
bool fixEncoderState(JSGlobalObject* globalObject, TextIOState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!state.isSeekable || !state.encoder)
        return true;
    state.isAtStartOfStream = true;
    JSValue position = callMethodNamed(globalObject, state.buffer.get(), names.attribute_tell);
    RETURN_IF_EXCEPTION(scope, false);
    bool isZero = isEqual(globalObject, position, jsNumber(0));
    RETURN_IF_EXCEPTION(scope, false);
    if (!isZero) {
        state.isAtStartOfStream = false;
        callMethodNamed(globalObject, state.encoder.get(), names.attribute_setstate, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// _Py_GetLocaleEncodingObject(): what the environment says text is in. The locale of the process is left as it is, since it is not Python's alone.
String localeEncoding(JSGlobalObject* globalObject)
{
#if OS(UNIX)
    if (locale_t locale = characterLocale(globalObject)) {
        String name = String::fromLatin1(nl_langinfo_l(CODESET, locale));
        if (!name.isEmpty())
            return name;
    }
#else
    UNUSED_PARAM(globalObject);
#endif
    return "utf-8"_s;
}

void setDecodedChars(TextIOState& state, const String& characters)
{
    state.decodedChars = characters;
    state.decodedObject.clear();
    state.decodedCharsUsed = 0;
}

// textiowrapper_get_decoded_chars()
String getDecodedChars(VM& vm, TextIOState& state, int64_t count)
{
    if (state.decodedChars.isNull())
        return emptyString();
    int64_t available = characterCount(vm, state.decodedChars) - state.decodedCharsUsed;
    if (count < 0 || count > available)
        count = available;
    String characters = state.decodedCharsUsed > 0 || count < available ? substringByCharacters(vm, state.decodedChars, state.decodedCharsUsed, state.decodedCharsUsed + count) : state.decodedChars;
    state.decodedCharsUsed += count;
    return characters;
}

// _textiowrapper_writeflush(): sends on what has been encoded. It does not flush what it sends it to. False if it raised.
bool writeFlush(JSGlobalObject* globalObject, TextIOState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (state.pendingBytes.isEmpty())
        return true;
    ByteVector pending = std::exchange(state.pendingBytes, ByteVector());
    JSValue bytes = newBytes(globalObject, pending);
    RETURN_IF_EXCEPTION(scope, false);
    // How much of it has gone, if it fails, there is no knowing.
    do {
        callMethodNamed(globalObject, state.buffer.get(), vm.pythonNames().attribute_write, bytes);
    } while (scope.exception() && trapInterruptedError(globalObject));
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// The state of a decoder, taken apart as by PyArg_ParseTuple() with "OO;illegal decoder state". False if it raised.
bool getDecoderState(JSGlobalObject* globalObject, JSValue decoder, JSValue& buffer, JSValue& flags)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue state = callMethodNamed(globalObject, decoder, vm.pythonNames().attribute_getstate);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isInstance(globalObject, state, globalObject->pyRealm()->typeTuple()) || asTuple(state)->length() != 2) {
        raiseTypeError(globalObject, scope, "illegal decoder state"_s);
        return false;
    }
    buffer = asTuple(state)->at(0);
    flags = asTuple(state)->at(1);
    if (!typeOf(globalObject, buffer)->hasFlag(PyType::IsBytes)) {
        raiseTypeError(globalObject, scope, concatenate("illegal decoder state: the first item should be a bytes object, not '"_s, typeName(globalObject, buffer), '\''));
        return false;
    }
    return true;
}

// textiowrapper_read_chunk(): reads some more and decodes it, in place of what had been decoded. False at the end of the file. Nothing if it raised.
std::optional<bool> readChunk(JSGlobalObject* globalObject, JSCell* self, TextIOState& state, int64_t sizeHint)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!state.decoder) {
        raiseUnsupportedOperation(globalObject, scope, "not readable"_s);
        return std::nullopt;
    }
    JSValue decoderBuffer;
    JSValue decoderFlags;
    // For tell(), it is noted what state the decoder is in. What it has kept back is that many bytes ago, and it had nothing kept back there.
    if (state.isTelling) {
        if (!getDecoderState(globalObject, state.decoder.get(), decoderBuffer, decoderFlags))
            return std::nullopt;
    }
    if (sizeHint > 0)
        sizeHint = static_cast<int64_t>(std::max(state.bytesPerCharacter, 1.0) * static_cast<double>(sizeHint));
    JSValue inputChunk = callMethodNamed(globalObject, state.buffer.get(), state.hasRead1 ? names.attribute_read1 : names.attribute_read, intFromInt64(globalObject, std::max(state.chunkSize, sizeHint)));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    int64_t byteCount;
    {
        Buffer input = bufferOrNothing(globalObject, inputChunk);
        if (!input) {
            raiseTypeError(globalObject, scope, concatenate("underlying "_s, state.hasRead1 ? "read1"_s : "read"_s, "() should have returned a bytes-like object, not '"_s, typeName(globalObject, inputChunk), '\''));
            return std::nullopt;
        }
        byteCount = static_cast<int64_t>(input.size());
    }
    bool isEnd = !byteCount;
    JSValue decoded = decodeWith(globalObject, state.decoder.get(), inputChunk, isEnd);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    String text = textOfString(globalObject, decoded);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    setDecodedChars(state, text);
    state.decodedObject.set(vm, self, decoded);
    int64_t count = characterCount(vm, text);
    state.bytesPerCharacter = count > 0 ? static_cast<double>(byteCount) / static_cast<double>(count) : 0.0;
    if (count > 0)
        isEnd = false;
    if (state.isTelling) {
        // From where the decoder had nothing kept back, what comes next is what it had kept back and then this.
        JSValue nextInput = binaryOperation(globalObject, BinaryOperator::Add, false, decoderBuffer, inputChunk);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        state.snapshot.set(vm, self, PyTuple::create(globalObject, { decoderFlags, nextInput }));
    }
    return !isEnd;
}

// _textiowrapper_readline(). Empty if it raised.
JSValue readLineOfText(JSGlobalObject* globalObject, JSValue self, TextIOState& state, int64_t limit)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CHECK_CLOSED();
    if (!writeFlush(globalObject, state))
        return { };

    TextBuilder chunks;
    bool hasChunks = false;
    JSValue lineObject;
    String line;
    String remaining;
    int64_t start = 0;
    int64_t end = 0;
    int64_t chunked = 0;
    int64_t offsetToBuffer = 0;
    while (true) {
        // First, something to look through.
        bool hasMore = true;
        while (state.decodedChars.isEmpty()) {
            auto more = readChunk(globalObject, self.asCell(), state, 0);
            if (scope.exception()) {
                if (trapInterruptedError(globalObject))
                    continue;
                return { };
            }
            hasMore = *more;
            if (!hasMore)
                break;
        }
        if (!hasMore) {
            setDecodedChars(state, String());
            state.snapshot.clear();
            start = end = offsetToBuffer = 0;
            line = String();
            break;
        }
        lineObject = JSValue();
        if (remaining.isNull()) {
            line = state.decodedChars;
            lineObject = state.decodedObject.get();
            start = state.decodedCharsUsed;
            offsetToBuffer = 0;
        } else {
            line = textOrMemoryError(globalObject, tryMakeString(remaining, state.decodedChars));
            RETURN_IF_EXCEPTION(scope, { });
            start = 0;
            offsetToBuffer = characterCount(vm, remaining);
            remaining = String();
        }

        // Line endings are looked for among the code units, and where they are is counted in characters.
        Characters characters(vm, line);
        int64_t lineLength = characters.count();
        unsigned startUnit = characters.codeUnitOf(static_cast<unsigned>(start));
        size_t consumedUnits = 0;
        auto found = line.is8Bit() ? findLineEnding(state.endings, line.span8().subspan(startUnit), consumedUnits) : findLineEnding(state.endings, line.span16().subspan(startUnit), consumedUnits);
        if (found) {
            end = characters.characterAt(startUnit + static_cast<unsigned>(*found));
            if (limit >= 0 && (end - start) + chunked >= limit)
                end = start + limit - chunked;
            break;
        }
        // So much can be put aside.
        end = characters.characterAt(startUnit + static_cast<unsigned>(consumedUnits));
        if (limit >= 0 && (end - start) + chunked >= limit) {
            end = start + limit - chunked;
            break;
        }
        if (end > start) {
            chunks.append(substringByCharacters(vm, line, start, end));
            hasChunks = true;
            chunked += end - start;
        }
        // What is left may be the beginning of a line ending, and goes before what is read next.
        if (end < lineLength)
            remaining = substringByCharacters(vm, line, end, lineLength);
        line = String();
        setDecodedChars(state, String());
    }
    if (!line.isNull()) {
        // The line ends in what has been decoded.
        state.decodedCharsUsed = end - offsetToBuffer;
        if (start > 0 || end < characterCount(vm, line)) {
            line = substringByCharacters(vm, line, start, end);
            lineObject = JSValue();
        }
        if (lineObject && !hasChunks && remaining.isNull())
            return lineObject;
    }
    if (!remaining.isNull())
        chunks.append(remaining);
    if (!line.isNull())
        chunks.append(line);
    String result = chunks.finish(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, result));
}

// ---- Where it is

// What tell() returns is more than a place in the file. It is a place where the decoder had nothing kept back, the state that it was in there, how many bytes to give it from there, and how many of the characters
// that come of that have been given out already.
struct Cookie {
    int64_t startPosition { 0 };
    int32_t decoderFlags { 0 };
    int32_t bytesToFeed { 0 };
    int32_t charactersToSkip { 0 };
    bool needsEnd { false };
};

static constexpr size_t cookieSize = sizeof(int64_t) + 3 * sizeof(int32_t) + 1;

// textiowrapper_build_cookie(): all of it as one int, with the place in the file lowest, so that where there is nothing else to say it is the place in the file.
JSValue buildCookie(JSGlobalObject* globalObject, const Cookie& cookie)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::array<uint64_t, 3> digits {
        static_cast<uint64_t>(cookie.startPosition),
        static_cast<uint64_t>(static_cast<uint32_t>(cookie.decoderFlags)) | (static_cast<uint64_t>(static_cast<uint32_t>(cookie.bytesToFeed)) << 32),
        static_cast<uint64_t>(static_cast<uint32_t>(cookie.charactersToSkip)) | (static_cast<uint64_t>(cookie.needsEnd) << 32),
    };
    unsigned length = 3;
    while (length && !digits[length - 1])
        --length;
    if (length <= 1)
        return intFromUInt64(globalObject, digits[0]);
    JSBigInt* result = JSBigInt::tryCreateWithLength(vm, length);
    if (!result)
        return raiseMemoryError(globalObject, scope);
    for (unsigned i = 0; i < length; ++i)
        result->setDigit(i, digits[i]);
    return result;
}

// textiowrapper_parse_cookie(). False if it raised.
bool parseCookie(JSGlobalObject* globalObject, JSValue value, Cookie& cookie)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // PyNumber_Long()
    JSValue integer = call(globalObject, globalObject->pyRealm()->typeInt(), value);
    RETURN_IF_EXCEPTION(scope, false);
    Number number = classify(integer);
    std::array<uint64_t, 3> digits { };
    if (number.kind == Number::Kind::Small) {
        if (number.small < 0) {
            raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative int to unsigned"_s);
            return false;
        }
        digits[0] = static_cast<uint64_t>(number.small);
    } else {
        if (number.big->sign()) {
            raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative int to unsigned"_s);
            return false;
        }
        unsigned length = number.big->length();
        static_assert(cookieSize == 21);
        if (length > 3 || (length == 3 && number.big->digit(2) >> 40)) {
            raise(globalObject, scope, BuiltinType::OverflowError, "int too big to convert"_s);
            return false;
        }
        for (unsigned i = 0; i < length; ++i)
            digits[i] = number.big->digit(i);
    }
    cookie.startPosition = static_cast<int64_t>(digits[0]);
    cookie.decoderFlags = static_cast<int32_t>(static_cast<uint32_t>(digits[1]));
    cookie.bytesToFeed = static_cast<int32_t>(static_cast<uint32_t>(digits[1] >> 32));
    cookie.charactersToSkip = static_cast<int32_t>(static_cast<uint32_t>(digits[2]));
    cookie.needsEnd = static_cast<uint8_t>(digits[2] >> 32);
    return true;
}

// _textiowrapper_decoder_setstate(). False if it raised.
bool setDecoderState(JSGlobalObject* globalObject, TextIOState& state, const Cookie& cookie)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    // At the beginning it is reset, since for a few decoders the state there is not (b"", 0): UTF-16 is waiting to be told which way round it is.
    if (!cookie.startPosition && !cookie.decoderFlags)
        callMethodNamed(globalObject, state.decoder.get(), names.attribute_reset);
    else {
        JSValue empty = newBytes(globalObject, { });
        RETURN_IF_EXCEPTION(scope, false);
        callMethodNamed(globalObject, state.decoder.get(), names.attribute_setstate, PyTuple::create(globalObject, { empty, jsNumber(cookie.decoderFlags) }));
    }
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// _textiowrapper_encoder_reset(). False if it raised.
bool resetEncoder(JSGlobalObject* globalObject, TextIOState& state, bool isAtStartOfStream)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (isAtStartOfStream)
        callMethodNamed(globalObject, state.encoder.get(), names.attribute_reset);
    else
        callMethodNamed(globalObject, state.encoder.get(), names.attribute_setstate, jsNumber(0));
    state.isAtStartOfStream = isAtStartOfStream;
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

} // anonymous namespace

// TextIOWrapper(buffer, encoding=None, errors=None, newline=None, line_buffering=False, write_through=False)
PYTHON_NATIVE(textIOInit)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfText(self);
    JSValue buffer = args.at(1);
    String encoding;
    if (JSValue value = args.at(2)) {
        auto given = toTextArgument(globalObject, value, "TextIOWrapper"_s, "argument 'encoding'"_s, true);
        RETURN_IF_EXCEPTION(scope, { });
        encoding = *given;
    }
    JSValue errors = args.at(3);
    String newline;
    if (JSValue value = args.at(4)) {
        auto given = toTextArgument(globalObject, value, "TextIOWrapper"_s, "argument 'newline'"_s, true);
        RETURN_IF_EXCEPTION(scope, { });
        newline = *given;
    }
    bool isLineBuffered = false;
    if (JSValue value = args.at(5)) {
        isLineBuffered = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool writesThrough = false;
    if (JSValue value = args.at(6)) {
        writesThrough = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }

    state.isInitialized = false;
    state.isDetached = false;
    if (!errors || isNone(errors))
        errors = jsNontrivialString(vm, "strict"_s);
    else if (!stringIn(errors))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("TextIOWrapper() argument 'errors' must be str or None, not "_s, typeName(globalObject, errors))));
    String errorsText = textOfString(globalObject, errors);
    RETURN_IF_EXCEPTION(scope, { });
    if (errorsText.contains(static_cast<char16_t>(0)))
        return JSValue::encode(raiseValueError(globalObject, scope, "embedded null character"_s));
    if (!validateNewline(globalObject, scope, newline))
        return { };

    state.buffer.clear();
    state.encoding.clear();
    state.encoder.clear();
    state.decoder.clear();
    state.endings = { };
    state.decodedChars = String();
    state.pendingBytes = ByteVector();
    state.snapshot.clear();
    state.errors.clear();
    state.raw.clear();
    state.decodedCharsUsed = 0;
    state.encodeFunction = EncodeFunction::None;
    state.bytesPerCharacter = 0;

    // Text is in UTF-8 unless it is said otherwise: the "UTF-8 mode" of CPython, which is how it is to be from 3.15 on.
    if (encoding.isNull())
        encoding = "utf-8"_s;
    else if (encoding == "locale"_s)
        encoding = localeEncoding(globalObject);
    state.encoding.set(vm, self, jsString(vm, encoding));

    JSValue codecInfo = lookupTextEncoding(globalObject, encoding);
    if (scope.exception()) {
        state.encoding.clear();
        return { };
    }
    state.errors.set(vm, self, errors);
    state.chunkSize = 8192;
    state.isLineBuffered = isLineBuffered;
    state.writesThrough = writesThrough;
    setNewline(state, newline);
    state.buffer.set(vm, self, buffer);
    if (!setDecoder(globalObject, self, state, codecInfo, errorsText) || !setEncoder(globalObject, self, state, codecInfo, errorsText))
        return { };

    auto& io = ioState(globalObject);
    PyType* bufferType = typeOf(globalObject, buffer);
    if (bufferType == io.bufferedReader.get() || bufferType == io.bufferedWriter.get() || bufferType == io.bufferedRandom.get()) {
        JSValue raw = getAttributeIfPresent(globalObject, buffer, names.attribute_raw);
        RETURN_IF_EXCEPTION(scope, { });
        // So that whether it is closed can be told without asking.
        if (raw && typeOf(globalObject, raw) == io.fileIO.get())
            state.raw.set(vm, self, raw);
    }
    auto isSeekable = askBuffer(globalObject, state, names.attribute_seekable);
    RETURN_IF_EXCEPTION(scope, { });
    state.isSeekable = state.isTelling = *isSeekable;
    JSValue read1 = getAttributeIfPresent(globalObject, buffer, names.attribute_read1);
    RETURN_IF_EXCEPTION(scope, { });
    state.hasRead1 = !!read1;
    state.isAtStartOfStream = false;
    if (!fixEncoderState(globalObject, state))
        return { };
    state.isInitialized = true;
    RETURN_NONE();
}

// convert_optional_bool(). Nothing if it raised.
static std::optional<bool> toOptionalBool(JSGlobalObject* globalObject, JSValue value, bool defaultValue)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value || isNone(value))
        return defaultValue;
    auto number = toCLong(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return !!*number;
}

// reconfigure(*, encoding=None, errors=None, newline=None, line_buffering=None, write_through=None)
PYTHON_NATIVE(textIOReconfigure)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfText(self);
    auto orNone = [] (JSValue value) { return value ? value : jsUndefined(); };
    JSValue encoding = orNone(args.at(1));
    JSValue errors = orNone(args.at(2));
    JSValue newlineObject = args.at(3);
    for (auto [value, name] : { std::pair { encoding, "encoding"_s }, std::pair { errors, "errors"_s }, std::pair { orNone(newlineObject), "newline"_s } }) {
        if (!isNone(value) && !stringIn(value))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("reconfigure() argument '"_s, name, "' must be str or None, not "_s, typeName(globalObject, value))));
    }
    if (!state.decodedChars.isNull() && (!isNone(encoding) || !isNone(errors) || newlineObject))
        return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "It is not possible to set the encoding or newline of stream after the first read"_s));
    String newline;
    if (newlineObject && !isNone(newlineObject)) {
        newline = textOfString(globalObject, newlineObject);
        RETURN_IF_EXCEPTION(scope, { });
        // It is as much of it as comes before a zero, as it is to C.
        if (size_t zero = newline.find(static_cast<char16_t>(0)); zero != notFound)
            newline = newline.left(zero);
        if (!validateNewline(globalObject, scope, newline))
            return { };
    }
    auto isLineBuffered = toOptionalBool(globalObject, args.at(4), state.isLineBuffered);
    RETURN_IF_EXCEPTION(scope, { });
    auto writesThrough = toOptionalBool(globalObject, args.at(5), state.writesThrough);
    RETURN_IF_EXCEPTION(scope, { });
    if (!flushFile(globalObject, self))
        return { };
    state.bytesPerCharacter = 0;
    if (newlineObject)
        setNewline(state, newline);

    // textiowrapper_change_encoding()
    if (!isNone(encoding) || !isNone(errors) || newlineObject) {
        if (isNone(encoding)) {
            encoding = state.encoding.get();
            if (isNone(errors))
                errors = state.errors.get();
        } else {
            String name = textOfString(globalObject, encoding);
            RETURN_IF_EXCEPTION(scope, { });
            if (name == "locale"_s)
                encoding = jsString(vm, localeEncoding(globalObject));
            if (isNone(errors))
                errors = jsNontrivialString(vm, "strict"_s);
        }
        // Each is as much of it as comes before a zero, as it is to C.
        auto upToZero = [] (const String& text) {
            size_t zero = text.find(static_cast<char16_t>(0));
            return zero == notFound ? text : text.left(zero);
        };
        String encodingText = upToZero(textOfString(globalObject, encoding));
        RETURN_IF_EXCEPTION(scope, { });
        String errorsText = upToZero(textOfString(globalObject, errors));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue codecInfo = lookupTextEncoding(globalObject, encodingText);
        RETURN_IF_EXCEPTION(scope, { });
        if (!setDecoder(globalObject, self, state, codecInfo, errorsText) || !setEncoder(globalObject, self, state, codecInfo, errorsText))
            return { };
        state.encoding.set(vm, self, encoding);
        state.errors.set(vm, self, errors);
        if (!fixEncoderState(globalObject, state))
            return { };
    }
    state.isLineBuffered = *isLineBuffered;
    state.writesThrough = *writesThrough;
    RETURN_NONE();
}

PYTHON_NATIVE(textIODetach)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    CHECK_ATTACHED();
    if (!flushFile(globalObject, self))
        return { };
    JSValue buffer = state.buffer.get();
    state.buffer.clear();
    state.isDetached = true;
    return JSValue::encode(buffer);
}

PYTHON_NATIVE(textIOWrite)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    JSValue text = args[1];
    if (!stringIn(text))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("write() argument must be str, not "_s, typeNameOfArgument(globalObject, text))));
    CHECK_ATTACHED();
    CHECK_CLOSED();
    if (!state.encoder)
        return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "not writable"_s));
    String content = textOfString(globalObject, text);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t textLength = characterCount(vm, content);
    bool translates = state.translatesOnWriting && !state.writeNewline.isNull();
    bool hasLineFeed = (translates || state.isLineBuffered) && content.contains('\n');
    if (hasLineFeed && translates) {
        text = callMethodNamed(globalObject, text, Identifier::fromString(vm, "replace"_s), jsString(vm, String("\n"_s)), jsString(vm, state.writeNewline));
        RETURN_IF_EXCEPTION(scope, { });
        content = textOfString(globalObject, text);
        RETURN_IF_EXCEPTION(scope, { });
    }
    bool needsFlush = state.isLineBuffered && (hasLineFeed || content.contains('\r'));

    std::optional<ByteVector> encoded;
    if (state.encodeFunction != EncodeFunction::None) {
        String errors = textOfString(globalObject, state.errors.get());
        RETURN_IF_EXCEPTION(scope, { });
        // After the beginning there is no mark to say which way round it is, and it is the way that this machine has it.
        int orderIfUnmarked = state.isAtStartOfStream ? 0 : -1;
        switch (state.encodeFunction) {
        case EncodeFunction::ASCII:
            encoded = encodeASCII(globalObject, text, errors);
            break;
        case EncodeFunction::Latin1:
            encoded = encodeLatin1(globalObject, text, errors);
            break;
        case EncodeFunction::UTF8:
            encoded = encodeUTF8(globalObject, text, errors);
            break;
        case EncodeFunction::UTF16BE:
            encoded = encodeUTF16(globalObject, text, errors, 1);
            break;
        case EncodeFunction::UTF16LE:
            encoded = encodeUTF16(globalObject, text, errors, -1);
            break;
        case EncodeFunction::UTF16:
            encoded = encodeUTF16(globalObject, text, errors, orderIfUnmarked);
            break;
        case EncodeFunction::UTF32BE:
            encoded = encodeUTF32(globalObject, text, errors, 1);
            break;
        case EncodeFunction::UTF32LE:
            encoded = encodeUTF32(globalObject, text, errors, -1);
            break;
        case EncodeFunction::UTF32:
            encoded = encodeUTF32(globalObject, text, errors, orderIfUnmarked);
            break;
        case EncodeFunction::None:
            RELEASE_ASSERT_NOT_REACHED();
        }
        RETURN_IF_EXCEPTION(scope, { });
        state.isAtStartOfStream = false;
    } else {
        JSValue bytes = callMethodNamed(globalObject, state.encoder.get(), names.attribute_encode, text);
        RETURN_IF_EXCEPTION(scope, { });
        if (!typeOf(globalObject, bytes)->hasFlag(PyType::IsBytes))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("encoder should return a bytes object, not '"_s, typeName(globalObject, bytes), '\'')));
        encoded = ByteVector();
        encoded->append(*builtinBufferOf(bytes));
    }

    // What is large is not put together with what is waiting. That is sent on first, and sending it on can put more there.
    if (static_cast<int64_t>(encoded->size()) >= state.chunkSize) {
        while (!state.pendingBytes.isEmpty()) {
            if (!writeFlush(globalObject, state))
                return { };
        }
    }
    state.pendingBytes.append(encoded->span());
    if (state.pendingBytes.hasOverflowed()) {
        state.pendingBytes = ByteVector();
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    }
    if (static_cast<int64_t>(state.pendingBytes.size()) >= state.chunkSize || needsFlush || state.writesThrough) {
        if (!writeFlush(globalObject, state))
            return { };
    }
    if (needsFlush && !flushFile(globalObject, state.buffer.get()))
        return { };
    if (state.snapshot) {
        setDecodedChars(state, String());
        state.snapshot.clear();
    }
    if (state.decoder) {
        callMethodNamed(globalObject, state.decoder.get(), names.attribute_reset);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(intFromInt64(globalObject, textLength));
}

PYTHON_NATIVE(textIORead)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    auto given = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count = *given;
    CHECK_ATTACHED();
    CHECK_CLOSED();
    if (!state.decoder)
        return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "not readable"_s));
    if (!writeFlush(globalObject, state))
        return { };

    if (count < 0) {
        // Everything
        JSValue bytes = callMethodNamed(globalObject, state.buffer.get(), names.attribute_read);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNone(bytes))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::BlockingIOError, "Read returned None."_s));
        JSValue decoded = decodeWith(globalObject, state.decoder.get(), bytes, true);
        RETURN_IF_EXCEPTION(scope, { });
        String rest = textOfString(globalObject, decoded);
        RETURN_IF_EXCEPTION(scope, { });
        String before = getDecodedChars(vm, state, -1);
        if (state.snapshot) {
            setDecodedChars(state, String());
            state.snapshot.clear();
        }
        // Added to nothing, it is what it is.
        if (before.isEmpty())
            return JSValue::encode(decoded);
        String result = textOrMemoryError(globalObject, tryMakeString(before, rest));
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, result)));
    }

    TextBuilder result;
    String piece = getDecodedChars(vm, state, count);
    result.append(piece);
    int64_t remaining = count - characterCount(vm, piece);
    while (remaining > 0) {
        auto hasMore = readChunk(globalObject, self.asCell(), state, remaining);
        if (scope.exception()) {
            if (trapInterruptedError(globalObject))
                continue;
            return { };
        }
        if (!*hasMore)
            break;
        piece = getDecodedChars(vm, state, remaining);
        result.append(piece);
        remaining -= characterCount(vm, piece);
    }
    String text = result.finish(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, text)));
}

PYTHON_NATIVE(textIOReadLine)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfText(args[0]);
    int64_t size = -1;
    if (JSValue value = args.at(1)) {
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        size = *given;
    }
    CHECK_ATTACHED();
    RELEASE_AND_RETURN(scope, JSValue::encode(readLineOfText(globalObject, args[0], state, size)));
}

PYTHON_NATIVE(textIOSeek)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    JSValue cookieObject = args[1];
    int whence = 0;
    if (JSValue value = args.at(2)) {
        auto given = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        whence = *given;
    }
    CHECK_ATTACHED();
    CHECK_CLOSED();
    if (!state.isSeekable)
        return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "underlying stream is not seekable"_s));

    switch (whence) {
    case SEEK_CUR: {
        bool isZero = isEqual(globalObject, cookieObject, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isZero)
            return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "can't do nonzero cur-relative seeks"_s));
        // Going to where it is brings what is underneath to the same place.
        cookieObject = callMethodNamed(globalObject, self, names.attribute_tell);
        RETURN_IF_EXCEPTION(scope, { });
        break;
    }
    case SEEK_END: {
        bool isZero = isEqual(globalObject, cookieObject, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        if (!isZero)
            return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "can't do nonzero end-relative seeks"_s));
        if (!flushFile(globalObject, self))
            return { };
        setDecodedChars(state, String());
        state.snapshot.clear();
        if (state.decoder) {
            callMethodNamed(globalObject, state.decoder.get(), names.attribute_reset);
            RETURN_IF_EXCEPTION(scope, { });
        }
        JSValue result = callMethodNamed(globalObject, state.buffer.get(), names.attribute_seek, jsNumber(0), jsNumber(2));
        RETURN_IF_EXCEPTION(scope, { });
        if (state.encoder) {
            bool isAtStart = isEqual(globalObject, result, jsNumber(0));
            RETURN_IF_EXCEPTION(scope, { });
            if (!resetEncoder(globalObject, state, isAtStart))
                return { };
        }
        return JSValue::encode(result);
    }
    case SEEK_SET:
        break;
    default:
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("invalid whence ("_s, whence, ", should be "_s, SEEK_SET, ", "_s, SEEK_CUR, " or "_s, SEEK_END, ')')));
    }

    JSValue compared = compare(globalObject, ComparisonOperator::Lt, cookieObject, jsNumber(0));
    RETURN_IF_EXCEPTION(scope, { });
    bool isNegative = isTrue(globalObject, compared);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNegative) {
        String shown = repr(globalObject, cookieObject);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("negative seek position "_s, shown)));
    }
    if (!flushFile(globalObject, self))
        return { };

    // It goes back to a place that is safe to start from, and does again what read() did from there.
    Cookie cookie;
    if (!parseCookie(globalObject, cookieObject, cookie))
        return { };
    callMethodNamed(globalObject, state.buffer.get(), names.attribute_seek, intFromInt64(globalObject, cookie.startPosition));
    RETURN_IF_EXCEPTION(scope, { });
    setDecodedChars(state, String());
    state.snapshot.clear();
    if (state.decoder && !setDecoderState(globalObject, state, cookie))
        return { };

    if (cookie.charactersToSkip) {
        JSValue inputChunk = callMethodNamed(globalObject, state.buffer.get(), names.attribute_read, jsNumber(cookie.bytesToFeed));
        RETURN_IF_EXCEPTION(scope, { });
        if (!typeOf(globalObject, inputChunk)->hasFlag(PyType::IsBytes))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("underlying read() should have returned a bytes object, not '"_s, typeName(globalObject, inputChunk), '\'')));
        state.snapshot.set(vm, self.asCell(), PyTuple::create(globalObject, { jsNumber(cookie.decoderFlags), inputChunk }));
        JSValue decoded = callMethodNamed(globalObject, state.decoder.get(), names.attribute_decode, inputChunk, jsBoolean(cookie.needsEnd));
        RETURN_IF_EXCEPTION(scope, { });
        if (!checkIsDecoded(globalObject, scope, decoded))
            return { };
        String text = textOfString(globalObject, decoded);
        RETURN_IF_EXCEPTION(scope, { });
        setDecodedChars(state, text);
        if (characterCount(vm, text) < cookie.charactersToSkip)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, "can't restore logical file position"_s));
        state.decodedCharsUsed = cookie.charactersToSkip;
    } else {
        JSValue empty = newBytes(globalObject, { });
        RETURN_IF_EXCEPTION(scope, { });
        state.snapshot.set(vm, self.asCell(), PyTuple::create(globalObject, { jsNumber(cookie.decoderFlags), empty }));
    }
    // For the sake of the mark at the beginning.
    if (state.encoder && !resetEncoder(globalObject, state, !cookie.startPosition && !cookie.decoderFlags))
        return { };
    return JSValue::encode(cookieObject);
}

// What is between saving the state of the decoder and putting it back, in tell(). Empty if it raised.
static JSValue findCookie(JSGlobalObject* globalObject, TextIOState& state, Cookie& cookie, JSValue nextInput)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    int64_t charactersToSkip = state.decodedCharsUsed;
    auto input = [&] { return *builtinBufferOf(nextInput); };

    // DECODER_GETSTATE()
    int64_t decoderBufferLength = 0;
    int decoderFlags = 0;
    auto getState = [&] () -> bool {
        JSValue buffer;
        JSValue flags;
        JSValue tuple = callMethodNamed(globalObject, state.decoder.get(), names.attribute_getstate);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isInstance(globalObject, tuple, globalObject->pyRealm()->typeTuple()) || asTuple(tuple)->length() != 2) {
            raiseTypeError(globalObject, scope, "illegal decoder state"_s);
            return false;
        }
        buffer = asTuple(tuple)->at(0);
        flags = asTuple(tuple)->at(1);
        auto number = toCIntOfFormat(globalObject, flags);
        RETURN_IF_EXCEPTION(scope, false);
        decoderFlags = *number;
        if (!typeOf(globalObject, buffer)->hasFlag(PyType::IsBytes)) {
            raiseTypeError(globalObject, scope, concatenate("illegal decoder state: the first item should be a bytes object, not '"_s, typeName(globalObject, buffer), '\''));
            return false;
        }
        decoderBufferLength = static_cast<int64_t>(builtinBufferOf(buffer)->size());
        return true;
    };
    // DECODER_DECODE(): how many characters come of some of the input. Less than nothing if it raised.
    auto decode = [&] (size_t start, size_t length) -> int64_t {
        JSValue piece = newBytes(globalObject, input().subspan(start, length));
        RETURN_IF_EXCEPTION(scope, -1);
        JSValue decoded = callMethodNamed(globalObject, state.decoder.get(), names.attribute_decode, piece);
        RETURN_IF_EXCEPTION(scope, -1);
        if (!checkIsDecoded(globalObject, scope, decoded))
            return -1;
        String text = textOfString(globalObject, decoded);
        RETURN_IF_EXCEPTION(scope, -1);
        return characterCount(vm, text);
    };

    // A quick look for a place to start from that is near where it is.
    // The guess goes by the last piece that was read, which after seek() is not what this is. CPython takes it that it cannot come to more than there is, and looks past the end if it does.
    int64_t skipBytes = std::min(static_cast<int64_t>(state.bytesPerCharacter * static_cast<double>(charactersToSkip)), static_cast<int64_t>(input().size()));
    int64_t skipBack = 1;
    while (skipBytes > 0) {
        if (!setDecoderState(globalObject, state, cookie))
            return { };
        int64_t decodedCount = decode(0, static_cast<size_t>(skipBytes));
        RETURN_IF_EXCEPTION(scope, { });
        if (decodedCount <= charactersToSkip) {
            if (!getState())
                return { };
            if (!decoderBufferLength) {
                // It is before where it is, and the decoder has nothing kept back.
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
        if (!setDecoderState(globalObject, state, cookie))
            return { };
    }
    cookie.startPosition += skipBytes;
    cookie.charactersToSkip = static_cast<int32_t>(charactersToSkip);
    if (!charactersToSkip)
        return jsUndefined();

    // It is near. The decoder is given a byte at a time until as many characters have come of it, and each place is noted where it has nothing kept back, since seek() can start from there.
    int64_t decodedCount = 0;
    size_t position = static_cast<size_t>(skipBytes);
    size_t end = input().size();
    while (position < end) {
        int64_t count = decode(position, 1);
        RETURN_IF_EXCEPTION(scope, { });
        decodedCount += count;
        cookie.bytesToFeed += 1;
        if (!getState())
            return { };
        if (!decoderBufferLength && decodedCount <= charactersToSkip) {
            cookie.startPosition += cookie.bytesToFeed;
            charactersToSkip -= decodedCount;
            cookie.decoderFlags = decoderFlags;
            cookie.bytesToFeed = 0;
            decodedCount = 0;
        }
        if (decodedCount >= charactersToSkip)
            break;
        ++position;
    }
    if (position == end) {
        // Not enough came of it. It is told that that is the end, to have the rest.
        JSValue empty = newBytes(globalObject, { });
        RETURN_IF_EXCEPTION(scope, { });
        JSValue decoded = callMethodNamed(globalObject, state.decoder.get(), names.attribute_decode, empty, jsBoolean(true));
        RETURN_IF_EXCEPTION(scope, { });
        if (!checkIsDecoded(globalObject, scope, decoded))
            return { };
        String text = textOfString(globalObject, decoded);
        RETURN_IF_EXCEPTION(scope, { });
        decodedCount += characterCount(vm, text);
        cookie.needsEnd = true;
        if (decodedCount < charactersToSkip)
            return raise(globalObject, scope, BuiltinType::OSError, "can't reconstruct logical file position"_s);
    }
    cookie.charactersToSkip = static_cast<int32_t>(charactersToSkip);
    return jsUndefined();
}

PYTHON_NATIVE(textIOTell)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    CHECK_ATTACHED();
    CHECK_CLOSED();
    if (!state.isSeekable)
        return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "underlying stream is not seekable"_s));
    if (!state.isTelling)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, "telling position disabled by next() call"_s));
    if (!writeFlush(globalObject, state) || !flushFile(globalObject, self))
        return { };
    JSValue position = callMethodNamed(globalObject, state.buffer.get(), names.attribute_tell);
    RETURN_IF_EXCEPTION(scope, { });
    if (!state.decoder || !state.snapshot)
        return JSValue::encode(position);

    Cookie cookie;
    auto start = toCLong(globalObject, position);
    RETURN_IF_EXCEPTION(scope, { });
    cookie.startPosition = *start;
    // Back to where the snapshot was taken.
    PyTuple* snapshot = asTuple(state.snapshot.get());
    auto flags = toCIntOfFormat(globalObject, snapshot->at(0));
    RETURN_IF_EXCEPTION(scope, { });
    cookie.decoderFlags = *flags;
    JSValue nextInput = snapshot->at(1);
    cookie.startPosition -= static_cast<int64_t>(builtinBufferOf(nextInput)->size());
    if (!state.decodedCharsUsed)
        RELEASE_AND_RETURN(scope, JSValue::encode(buildCookie(globalObject, cookie)));

    // The decoder is used to find out, and is put back as it was.
    JSValue savedState = callMethodNamed(globalObject, state.decoder.get(), names.attribute_getstate);
    RETURN_IF_EXCEPTION(scope, { });
    findCookie(globalObject, state, cookie, nextInput);
    if (scope.exception()) {
        Exception* raised = takeRaisedException(vm);
        RETURN_IF_EXCEPTION(scope, { });
        callMethodNamed(globalObject, state.decoder.get(), names.attribute_setstate, savedState);
        scope.release();
        chainRaisedExceptions(globalObject, raised);
        return { };
    }
    callMethodNamed(globalObject, state.decoder.get(), names.attribute_setstate, savedState);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(buildCookie(globalObject, cookie)));
}

PYTHON_NATIVE(textIOTruncate)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    CHECK_ATTACHED();
    if (!flushFile(globalObject, self))
        return { };
    JSValue position = args.at(1);
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.buffer.get(), names.attribute_truncate, position ? position : jsUndefined())));
}

PYTHON_NATIVE(textIORepr)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    if (!checkIsInitialized(globalObject, scope, state))
        return { };
    String type = typeName(globalObject, self);
    ReprGuard guard(globalObject, self.asCell());
    if (guard.isRecursive())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("reentrant call inside "_s, type, ".__repr__"_s)));
    TextBuilder out;
    out.append('<', type);
    JSValue name = getAttributeIfPresent(globalObject, self, names.attribute_name);
    if (scope.exception()) {
        // If the buffer has been detached, that is nothing to make anything of.
        if (!catchException(globalObject, BuiltinType::ValueError))
            return { };
        name = { };
    }
    if (name) {
        String shown = repr(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        out.append(" name="_s, shown);
    }
    JSValue mode = getAttributeIfPresent(globalObject, self, names.attribute_mode);
    RETURN_IF_EXCEPTION(scope, { });
    if (mode) {
        String shown = repr(globalObject, mode);
        RETURN_IF_EXCEPTION(scope, { });
        out.append(" mode="_s, shown);
    }
    String encoding = repr(globalObject, state.encoding.get());
    RETURN_IF_EXCEPTION(scope, { });
    out.append(" encoding="_s, encoding, '>');
    String result = out.finish(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, result)));
}

enum class TextForward : uint8_t { Fileno, Seekable, Readable, Writable, Isatty };

// What is only asked of the buffer
PYTHON_NATIVE(textIOForward)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfText(args[0]);
    CHECK_ATTACHED();
    const Identifier* method = nullptr;
    switch (unpack<TextForward>(callFrame, 0)) {
    case TextForward::Fileno:
        method = &names.attribute_fileno;
        break;
    case TextForward::Seekable:
        method = &names.attribute_seekable;
        break;
    case TextForward::Readable:
        method = &names.attribute_readable;
        break;
    case TextForward::Writable:
        method = &names.attribute_writable;
        break;
    case TextForward::Isatty:
        method = &names.attribute_isatty;
        break;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.buffer.get(), *method)));
}

PYTHON_NATIVE(textIOFlush)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    CHECK_ATTACHED();
    CHECK_CLOSED();
    state.isTelling = state.isSeekable;
    if (!writeFlush(globalObject, state))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.buffer.get(), names.attribute_flush)));
}

PYTHON_NATIVE(textIOClose)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    CHECK_ATTACHED();
    JSValue closed = getAttribute(globalObject, state.buffer.get(), names.attribute_closed);
    RETURN_IF_EXCEPTION(scope, { });
    bool isClosed = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, { });
    if (isClosed || state.isDetached)
        RETURN_NONE();
    if (state.isFinalizing) {
        callMethodNamed(globalObject, state.buffer.get(), names.attribute__dealloc_warn, self);
        if (scope.exception() && !scope.tryClearException())
            return { };
    }
    Exception* raised = nullptr;
    if (!flushFile(globalObject, self)) {
        raised = takeRaisedException(vm);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue result = callMethodNamed(globalObject, state.buffer.get(), names.attribute_close);
    if (raised) {
        scope.release();
        chainRaisedExceptions(globalObject, raised);
        return { };
    }
    return JSValue::encode(result);
}

PYTHON_NATIVE(textIONext)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfText(self);
    CHECK_ATTACHED();
    state.isTelling = false;
    JSValue line;
    if (typeOf(globalObject, self) == ioState(globalObject).textIOWrapper.get())
        line = readLineOfText(globalObject, self, state, -1);
    else {
        line = callMethodNamed(globalObject, self, names.attribute_readline);
        if (line && !stringIn(line))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, concatenate("readline() should have returned a str object, not '"_s, typeName(globalObject, line), '\'')));
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(line)->length()) {
        // The end, or it would have had to wait.
        state.snapshot.clear();
        state.isTelling = state.isSeekable;
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    }
    return JSValue::encode(line);
}

static void initializeTextIOWrapper(JSGlobalObject* globalObject, IOModuleState& io)
{
    VM& vm = globalObject->vm();
    using Kind = PyNativeFunction::Kind;
    PyType* type = createBuiltinType(globalObject, "_io.TextIOWrapper"_s, io.textIOBase.get(), PyType::Layout::Native, PyType::IsBaseType);
    io.textIOWrapper.set(vm, globalObject->pyRealm(), type);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    type->setAllocator([] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<TextIOState>()); });
    addMethods(globalObject, type, {
        { "__init__"_s, textIOInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, textIORepr },
        { "__next__"_s, textIONext },
        { "detach"_s, textIODetach },
        { "reconfigure"_s, textIOReconfigure },
        { "write"_s, textIOWrite },
        { "read"_s, textIORead },
        { "readline"_s, textIOReadLine },
        { "flush"_s, textIOFlush },
        { "close"_s, textIOClose },
        { "fileno"_s, textIOForward, Kind::Method, pack(TextForward::Fileno) },
        { "seekable"_s, textIOForward, Kind::Method, pack(TextForward::Seekable) },
        { "readable"_s, textIOForward, Kind::Method, pack(TextForward::Readable) },
        { "writable"_s, textIOForward, Kind::Method, pack(TextForward::Writable) },
        { "isatty"_s, textIOForward, Kind::Method, pack(TextForward::Isatty) },
        { "seek"_s, textIOSeek },
        { "tell"_s, textIOTell },
        { "truncate"_s, textIOTruncate },
        { "__getstate__"_s, ioCannotPickle, Kind::Method, 0, "($self, /)"_s },
    });
    auto orNone = [] (const WriteBarrier<Unknown>& value) -> JSValue { return value ? value.get() : jsUndefined(); };
    UNUSED_VARIABLE(orNone);
    addMember(globalObject, type, "encoding"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { auto& value = stateOfText(self).encoding; return value ? value.get() : jsUndefined(); });
    addMember(globalObject, type, "buffer"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { auto& value = stateOfText(self).buffer; return value ? value.get() : jsUndefined(); });
    addMember(globalObject, type, "line_buffering"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfText(self).isLineBuffered); });
    addMember(globalObject, type, "write_through"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfText(self).writesThrough); });
    addMember(globalObject, type, "_finalizing"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfText(self).isFinalizing); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        toBoolMember(globalObject, value, stateOfText(self).isFinalizing);
    });
    addGetSet(globalObject, type, "name"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto& state = stateOfText(self);
        CHECK_ATTACHED();
        RELEASE_AND_RETURN(scope, getAttribute(globalObject, state.buffer.get(), vm.pythonNames().attribute_name));
    });
    addGetSet(globalObject, type, "closed"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto& state = stateOfText(self);
        CHECK_ATTACHED();
        RELEASE_AND_RETURN(scope, getAttribute(globalObject, state.buffer.get(), vm.pythonNames().attribute_closed));
    });
    addGetSet(globalObject, type, "newlines"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto& state = stateOfText(self);
        CHECK_ATTACHED();
        if (!state.decoder)
            return jsUndefined();
        JSValue result = getAttributeIfPresent(globalObject, state.decoder.get(), vm.pythonNames().attribute_newlines);
        RETURN_IF_EXCEPTION(scope, { });
        return result ? result : jsUndefined();
    });
    addGetSet(globalObject, type, "errors"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        auto& state = stateOfText(self);
        if (!checkIsInitialized(globalObject, scope, state))
            return { };
        return state.errors.get();
    });
    addGetSet(globalObject, type, "_CHUNK_SIZE"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        auto& state = stateOfText(self);
        CHECK_ATTACHED();
        return intFromInt64(globalObject, state.chunkSize);
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        auto& state = stateOfText(self);
        if (!checkIsAttached(globalObject, scope, state))
            return;
        if (!value) {
            raise(globalObject, scope, BuiltinType::AttributeError, "cannot delete attribute"_s);
            return;
        }
        // PyNumber_AsSsize_t(value, PyExc_ValueError)
        JSValue index = toInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
        auto size = tryInt64(index);
        if (!size) {
            raiseValueError(globalObject, scope, concatenate("cannot fit '"_s, typeName(globalObject, value), "' into an index-sized integer"_s));
            return;
        }
        if (*size <= 0) {
            raiseValueError(globalObject, scope, "a strictly positive integer is required"_s);
            return;
        }
        state.chunkSize = *size;
    });
}

void initializeTextIO(JSGlobalObject* globalObject, IOModuleState& io)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;

    PyType* decoder = createBuiltinType(globalObject, "_io.IncrementalNewlineDecoder"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    io.incrementalNewlineDecoder.set(vm, realm, decoder);
    decoder->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, decoder));
    decoder->setAllocator([] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<NewlineDecoderState>()); });
    addMethods(globalObject, decoder, {
        { "__init__"_s, newlineDecoderInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "decode"_s, newlineDecoderDecode },
        { "getstate"_s, newlineDecoderGetState },
        { "setstate"_s, newlineDecoderSetState },
        { "reset"_s, newlineDecoderReset },
    });
    addGetSet(globalObject, decoder, "newlines"_s, getNewlinesSeen);

    initializeTextIOWrapper(globalObject, io);
}

} } // namespace JSC::Python
