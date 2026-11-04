/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyIO.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyModuleHelpers.h"
#include "PyType.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// StringIO: a text file in memory. (Modules/_io/stringio.c in CPython.)
//
// One that has only ever been written to at its end is "accumulating": the contents are a str, to which each write adds, and
// which is a rope until someone looks at it. Anything else "realizes" it: the contents become an array of code points.

namespace {

struct StringIOState {
    int64_t position { 0 }; // In code points, as the size.
    int64_t size { 0 };
    bool isInitialized { false };
    bool isClosed { false };
    bool isAccumulating { false };
    bool readUniversal { false };
    bool readTranslate { false };
};

enum StringIOField : unsigned {
    StringIOStateField,
    StringIOBufferField, // A PyByteStorage of char32_t, or empty.
    StringIOAccumulatedField, // A str, while accumulating.
    StringIODecoderField, // An IncrementalNewlineDecoder, or empty.
    StringIOReadNewlineField, // A str, or empty for None.
    StringIOWriteNewlineField, // A str, or empty if "\n" is written as it is.
    StringIOFieldCount,
};

} // namespace

static ALWAYS_INLINE StringIOState& stringIOStateOf(PyRecord* stream) { return ioStateOf<StringIOState>(stream); }

static ALWAYS_INLINE PyByteStorage* stringIOStorage(PyRecord* stream)
{
    JSValue buffer = stream->get(StringIOBufferField);
    return buffer ? uncheckedDowncast<PyByteStorage>(buffer.asCell()) : nullptr;
}
static ALWAYS_INLINE char32_t* stringIOCharacters(PyRecord* stream) { return reinterpret_cast<char32_t*>(stringIOStorage(stream)->data()); }
static ALWAYS_INLINE size_t stringIOCapacity(PyRecord* stream)
{
    PyByteStorage* storage = stringIOStorage(stream);
    return storage ? storage->capacity() / sizeof(char32_t) : 0;
}

// False: a ValueError.
static bool stringIOCheckInitialized(JSGlobalObject* globalObject, PyRecord* stream)
{
    if (stringIOStateOf(stream).isInitialized) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "I/O operation on uninitialized object"_s);
    return false;
}

static bool stringIOCheckOpen(JSGlobalObject* globalObject, PyRecord* stream)
{
    if (!stringIOStateOf(stream).isClosed) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "I/O operation on closed file"_s);
    return false;
}

static ALWAYS_INLINE bool stringIOCheckUsable(JSGlobalObject* globalObject, PyRecord* stream)
{
    return stringIOCheckInitialized(globalObject, stream) && stringIOCheckOpen(globalObject, stream);
}

// Makes room for `size` characters, or gives back what that many have no use for. `kept` of those there are stay.
static bool stringIOResizeBuffer(JSGlobalObject* globalObject, PyRecord* stream, size_t size, size_t kept)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    size_t capacity = stringIOCapacity(stream);
    if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max()) / sizeof(char32_t) - 16) {
        raiseOverflowError(globalObject, scope, "new buffer size too large"_s);
        return false;
    }
    if (size < capacity / 2)
        capacity = size + 1;
    else if (size < capacity)
        return true;
    else if (static_cast<double>(size) <= static_cast<double>(capacity) * 1.125)
        capacity = size + (size >> 3) + (size < 9 ? 3 : 6);
    else
        capacity = size + 1;

    PyByteStorage* storage = ioTryNewStorage(globalObject, capacity * sizeof(char32_t));
    RETURN_IF_EXCEPTION(scope, false);
    if (kept)
        memcpy(storage->data(), stringIOCharacters(stream), std::min(kept, capacity) * sizeof(char32_t));
    stream->set(vm, StringIOBufferField, storage);
    return true;
}

static void stringIOCopyCodePoints(const String& text, char32_t* destination)
{
    if (text.is8Bit()) {
        for (auto character : text.span8())
            *destination++ = static_cast<uint8_t>(character);
        return;
    }
    auto units = text.span16();
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t character = units[i];
        if (U16_IS_LEAD(character) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            character = U16_GET_SUPPLEMENTARY(character, units[++i]);
        *destination++ = character;
    }
}

static JSValue stringIOMakeStr(JSGlobalObject* globalObject, std::span<const char32_t> characters)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (characters.empty())
        return rt.names.empty();
    char32_t all = 0;
    size_t supplementary = 0;
    for (char32_t character : characters) {
        all |= character;
        supplementary += character > 0xffff;
    }
    String result;
    if (all < 0x100) {
        std::span<Latin1Character> data;
        result = String::tryCreateUninitialized(characters.size(), data);
        if (!result.isNull()) {
            for (size_t i = 0; i < characters.size(); ++i)
                data[i] = static_cast<uint8_t>(characters[i]);
        }
    } else {
        std::span<char16_t> data;
        result = String::tryCreateUninitialized(characters.size() + supplementary, data);
        if (!result.isNull()) {
            size_t i = 0;
            for (char32_t character : characters) {
                if (character > 0xffff) {
                    data[i++] = U16_LEAD(character);
                    data[i++] = U16_TRAIL(character);
                } else
                    data[i++] = static_cast<char16_t>(character);
            }
        }
    }
    if (result.isNull())
        return raiseWithArgs(globalObject, scope, rt.MemoryError(), { });
    return makeStr(vm, result);
}

// From a str to an array of code points.
static bool stringIORealize(JSGlobalObject* globalObject, PyRecord* stream)
{
    auto& state = stringIOStateOf(stream);
    if (!state.isAccumulating)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = asString(stream->get(StringIOAccumulatedField))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    size_t length = ioCountCodePoints(text);
    if (failed(scope, stringIOResizeBuffer(globalObject, stream, length, 0)))
        return false;
    state.isAccumulating = false;
    stream->set(vm, StringIOAccumulatedField, JSValue());
    stringIOCopyCodePoints(text, stringIOCharacters(stream));
    // Two halves of a surrogate pair written one after the other have become one character.
    state.size = length;
    return true;
}

static bool stringIOWriteStr(JSGlobalObject* globalObject, PyRecord* stream, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = stringIOStateOf(stream);

    JSValue decoded = strValue(object);
    if (JSValue decoder = stream->get(StringIODecoderField)) {
        decoded = newlineDecoderDecode(globalObject, PyRecord::from(decoder), decoded, true);
        RETURN_IF_EXCEPTION(scope, false);
    }
    JSString* string = strValue(decoded);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (JSValue newline = stream->get(StringIOWriteNewlineField); newline && text.contains('\n')) {
        text = asString(newline)->length() == 1 ? makeStringByReplacingAll(text, '\n', '\r') : makeStringByReplacingAll(text, '\n', "\r\n"_s);
        string = makeStr(vm, text);
    }

    int64_t length = ioCountCodePoints(text);
    if (state.position > std::numeric_limits<int64_t>::max() - length) {
        raiseOverflowError(globalObject, scope, "new position too large"_s);
        return false;
    }

    bool isAppended = false;
    if (state.isAccumulating) {
        if (state.size == state.position) {
            JSString* joined = jsString(globalObject, asString(stream->get(StringIOAccumulatedField)), string);
            RETURN_IF_EXCEPTION(scope, false);
            stream->set(vm, StringIOAccumulatedField, joined);
            isAppended = true;
        } else if (failed(scope, stringIORealize(globalObject, stream)))
            return false;
    }
    if (!isAppended) {
        if (state.position + length > state.size && failed(scope, stringIOResizeBuffer(globalObject, stream, state.position + length, state.size)))
            return false;
        char32_t* characters = stringIOCharacters(stream);
        // What a seek beyond the end has skipped reads as NULs.
        if (state.position > state.size)
            memset(characters + state.size, 0, (state.position - state.size) * sizeof(char32_t));
        stringIOCopyCodePoints(text, characters + state.position);
    }
    state.position += length;
    state.size = std::max(state.size, state.position);
    return true;
}

// ---- Methods ----

static JSValue stringIONew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    PyRecord* stream = ioAllocate<StringIOState>(globalObject, cls, StringIOFieldCount);
    // As for BytesIO.
    if (stream->type()->isHeapType())
        ioDidOpen(globalObject, stream);
    return stream;
}

static bool stringIOInitialize(JSGlobalObject* globalObject, PyRecord* stream, JSValue value, JSValue newlineValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // A null string stands for None.
    String newline = "\n"_s;
    if (newlineValue) {
        if (isNone(newlineValue))
            newline = String();
        else if (!isStr(newlineValue)) {
            raiseTypeError(globalObject, scope, "newline must be str or None, not "_s, typeName(globalObject, newlineValue));
            return false;
        } else {
            newline = strValue(newlineValue)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (failed(scope, ioCheckUTF8(globalObject, strValue(newlineValue))))
                return false;
            if (newline.isNull())
                newline = emptyString();
        }
    }
    if (!ioIsLegalNewline(newline)) {
        raiseValueError(globalObject, scope, "illegal newline value: "_s, reprForMessage(globalObject, newlineValue));
        return false;
    }
    if (value && !isNone(value) && !isStr(value)) {
        raiseTypeError(globalObject, scope, "initial_value must be str or None, not "_s, typeName(globalObject, value));
        return false;
    }

    auto& state = stringIOStateOf(stream);
    state.isInitialized = false;
    stream->set(vm, StringIOAccumulatedField, JSValue());
    stream->set(vm, StringIODecoderField, JSValue());
    stream->set(vm, StringIOReadNewlineField, newline.isNull() ? JSValue() : JSValue(makeStr(vm, newline)));
    state.readUniversal = newline.isEmpty();
    state.readTranslate = newline.isNull();
    // With None, "\n" would be written as os.linesep to a file. Here that has no point.
    stream->set(vm, StringIOWriteNewlineField, !newline.isEmpty() && newline[0] == '\r' ? stream->get(StringIOReadNewlineField) : JSValue());

    if (state.readUniversal) {
        JSValue decoder = call(globalObject, ioType(globalObject, IOType::IncrementalNewlineDecoder), jsUndefined(), jsBoolean(state.readTranslate));
        RETURN_IF_EXCEPTION(scope, false);
        stream->set(vm, StringIODecoderField, decoder);
    }

    state.size = 0;
    state.position = 0;
    stream->set(vm, StringIOBufferField, JSValue());
    bool hasValue = value && !isNone(value) && strValue(value)->length();
    state.isAccumulating = !hasValue;
    if (hasValue) {
        if (failed(scope, stringIOWriteStr(globalObject, stream, value)))
            return false;
    } else
        stream->set(vm, StringIOAccumulatedField, rt.names.empty());
    state.position = 0;
    state.isClosed = false;
    state.isInitialized = true;
    return true;
}

static JSValue stringIOInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "initial_value"_s, "newline"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, "StringIO"_s, keywords, 0, 2, false, args, values)))
        return { };
    RELEASE_AND_RETURN(scope, stringIOInitialize(globalObject, PyRecord::from(self), values[0], values[1]) ? jsUndefined() : JSValue());
}

static JSValue stringIOGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)))
        return { };
    auto& state = stringIOStateOf(stream);
    if (state.isAccumulating)
        return stream->get(StringIOAccumulatedField);
    RELEASE_AND_RETURN(scope, stringIOMakeStr(globalObject, { stringIOStorage(stream) ? stringIOCharacters(stream) : nullptr, static_cast<size_t>(state.size) }));
}

static JSValue stringIOTell(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)))
        return { };
    RELEASE_AND_RETURN(scope, makeInt(globalObject, stringIOStateOf(stream).position));
}

static JSValue stringIORead(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (failed(scope, ioToOptionalSize(globalObject, sizeValue, size)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)))
        return { };
    auto& state = stringIOStateOf(stream);
    int64_t available = state.size - state.position;
    if (size < 0 || size > available)
        size = std::max<int64_t>(available, 0);

    // seek(0); read()
    if (state.isAccumulating && !state.position && size == available) {
        state.position = state.size;
        return stream->get(StringIOAccumulatedField);
    }
    if (failed(scope, stringIORealize(globalObject, stream)))
        return { };
    if (!size)
        RELEASE_AND_RETURN(scope, runtime(globalObject).names.empty());
    size = std::min(size, std::max<int64_t>(state.size - state.position, 0));
    const char32_t* start = stringIOCharacters(stream) + state.position;
    state.position += size;
    RELEASE_AND_RETURN(scope, stringIOMakeStr(globalObject, { start, static_cast<size_t>(size) }));
}

// The stream is realized.
static JSValue stringIOReadlineImpl(JSGlobalObject* globalObject, PyRecord* stream, int64_t limit)
{
    auto& state = stringIOStateOf(stream);
    if (state.position >= state.size)
        return runtime(globalObject).names.empty();
    if (limit < 0 || limit > state.size - state.position)
        limit = state.size - state.position;

    uint8_t newline[2] = { };
    size_t newlineLength = 0;
    if (JSValue readNewline = stream->get(StringIOReadNewlineField)) {
        String text = stringOf(asString(readNewline));
        newlineLength = text.length();
        for (size_t i = 0; i < newlineLength; ++i)
            newline[i] = static_cast<uint8_t>(text[i]);
    }
    std::span<const char32_t> text { stringIOCharacters(stream) + state.position, static_cast<size_t>(limit) };
    int64_t consumed;
    int64_t length = ioFindLineEnding<char32_t>(state.readTranslate, state.readUniversal, { newline, newlineLength }, text, consumed);
    if (length < 0)
        length = limit;
    state.position += length;
    return stringIOMakeStr(globalObject, text.first(length));
}

static JSValue stringIOReadline(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (failed(scope, ioToOptionalSize(globalObject, sizeValue, size)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)) || failed(scope, stringIORealize(globalObject, stream)))
        return { };
    RELEASE_AND_RETURN(scope, stringIOReadlineImpl(globalObject, stream, size));
}

static JSValue stringIONext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)) || failed(scope, stringIORealize(globalObject, stream)))
        return { };

    JSValue line;
    if (!stream->type()->isHeapType())
        line = stringIOReadlineImpl(globalObject, stream, -1);
    else {
        line = callMethod(globalObject, self, rt.names.readline());
        if (!failed(scope, line) && !isStr(line))
            return raise(globalObject, scope, rt.OSError(), makeString("readline() should have returned a str object, not '"_s, typeName(globalObject, line), '\''));
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!strValue(line)->length())
        return raiseStopIteration(globalObject, scope);
    return line;
}

static JSValue stringIOTruncate(JSGlobalObject* globalObject, JSValue self, JSValue positionValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)))
        return { };
    auto& state = stringIOStateOf(stream);
    int64_t size = state.position;
    if (positionValue && !isNone(positionValue)) {
        size = toInt64(globalObject, positionValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (size < 0)
            return raiseValueError(globalObject, scope, "negative pos value "_s, size);
    }
    if (size < state.size) {
        if (failed(scope, stringIORealize(globalObject, stream)))
            return { };
        if (size < state.size) {
            if (failed(scope, stringIOResizeBuffer(globalObject, stream, size, size)))
                return { };
            state.size = size;
        }
    }
    RELEASE_AND_RETURN(scope, makeInt(globalObject, size));
}

static JSValue stringIOSeek(JSGlobalObject* globalObject, JSValue self, JSValue positionValue, JSValue whenceValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t position;
    if (failed(scope, ssizeArgument(globalObject, positionValue, position)))
        return { };
    int whence = 0;
    if (whenceValue && failed(scope, ioToInt(globalObject, whenceValue, whence)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)))
        return { };
    auto& state = stringIOStateOf(stream);

    if (whence < 0 || whence > 2)
        return raiseValueError(globalObject, scope, "Invalid whence ("_s, whence, ", should be 0, 1 or 2)"_s);
    if (position < 0 && !whence)
        return raiseValueError(globalObject, scope, "Negative seek position "_s, position);
    if (whence && position)
        return raise(globalObject, scope, rt.OSError(), "Can't do nonzero cur-relative seeks"_s);
    if (whence == 1)
        position = state.position;
    else if (whence == 2)
        position = state.size;
    state.position = position;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, position));
}

static JSValue stringIOWrite(JSGlobalObject* globalObject, JSValue self, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckInitialized(globalObject, stream)))
        return { };
    if (!isStr(object))
        return raiseTypeError(globalObject, scope, "string argument expected, got '"_s, typeName(globalObject, object), '\'');
    if (failed(scope, stringIOCheckOpen(globalObject, stream)))
        return { };
    unsigned size = strLength(globalObject, strValue(object));
    RETURN_IF_EXCEPTION(scope, { });
    if (size && failed(scope, stringIOWriteStr(globalObject, stream, object)))
        return { };
    return jsNumber(size);
}

static JSValue stringIOClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* stream = PyRecord::from(self);
    stringIOStateOf(stream).isClosed = true;
    for (unsigned field = StringIOBufferField; field < StringIOFieldCount; ++field)
        stream->set(vm, field, JSValue());
    ioDidClose(globalObject, stream);
    return jsUndefined();
}

static JSValue stringIOTrueIfUsable(JSGlobalObject* globalObject, JSValue self) { return stringIOCheckUsable(globalObject, PyRecord::from(self)) ? jsBoolean(true) : JSValue(); }

// ---- Pickling ----

static JSValue stringIOGetState(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    JSValue value = stringIOGetValue(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    PyDict* dict = instanceDict(globalObject, self, false);
    JSValue newline = stream->get(StringIOReadNewlineField);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { value, newline ? newline : jsUndefined(), makeInt(globalObject, stringIOStateOf(stream).position), dict ? JSValue(dict->copy(globalObject)) : jsUndefined() }));
}

static JSValue stringIOSetState(JSGlobalObject* globalObject, JSValue self, JSValue stateValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckOpen(globalObject, stream)))
        return { };
    // A longer tuple is fine: a later version may have more to say.
    auto* tuple = PyTuple::tryFrom(stateValue);
    if (!tuple || tuple->length() < 4)
        return raiseTypeError(globalObject, scope, typeName(globalObject, self), ".__setstate__ argument should be 4-tuple, got "_s, typeName(globalObject, stateValue));

    if (failed(scope, stringIOInitialize(globalObject, stream, tuple->at(0), tuple->at(1))))
        return { };
    // That has translated the newlines of a value in which they had been translated already.
    auto& state = stringIOStateOf(stream);
    if (JSValue value = tuple->at(0); isStr(value)) {
        String text = strValue(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (failed(scope, stringIORealize(globalObject, stream)))
            return { };
        size_t length = ioCountCodePoints(text);
        if (failed(scope, stringIOResizeBuffer(globalObject, stream, length, 0)))
            return { };
        if (length)
            stringIOCopyCodePoints(text, stringIOCharacters(stream));
        state.size = length;
    } else
        state.size = 0;

    JSValue positionValue = tuple->at(2);
    if (!isInt(positionValue))
        return raiseTypeError(globalObject, scope, "third item of state must be an integer, got "_s, typeName(globalObject, positionValue));
    int64_t position;
    if (failed(scope, ssizeArgument(globalObject, positionValue, position)))
        return { };
    if (position < 0)
        return raiseValueError(globalObject, scope, "position value cannot be negative"_s);
    state.position = position;

    JSValue dict = tuple->at(3);
    if (!isNone(dict)) {
        if (!isDict(dict))
            return raiseTypeError(globalObject, scope, "fourth item of state should be a dict, got a "_s, typeName(globalObject, dict));
        dictUpdate(globalObject, instanceDict(globalObject, self, true), dict);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

// ---- Attributes ----

static JSValue stringIOGetClosed(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckInitialized(globalObject, stream)))
        return { };
    return jsBoolean(stringIOStateOf(stream).isClosed);
}

static JSValue stringIOGetLineBuffering(JSGlobalObject* globalObject, JSValue self) { return stringIOCheckUsable(globalObject, PyRecord::from(self)) ? jsBoolean(false) : JSValue(); }

static JSValue stringIOGetNewlines(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, stringIOCheckUsable(globalObject, stream)))
        return { };
    JSValue decoder = stream->get(StringIODecoderField);
    if (!decoder)
        return jsUndefined();
    RELEASE_AND_RETURN(scope, getAttr(globalObject, decoder, ioName(globalObject, IOName::newlines)));
}

static constexpr NativeDef stringIOMethods[] = {
    PY_NEW(stringIONew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", stringIOInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__next__", stringIONext, "($self, /)"),
    PY_METHOD("close", stringIOClose, "($self, /)"),
    PY_METHOD("getvalue", stringIOGetValue, "($self, /)"),
    PY_METHOD("read", stringIORead, "($self, size=-1, /)"),
    PY_METHOD("readline", stringIOReadline, "($self, size=-1, /)"),
    PY_METHOD("tell", stringIOTell, "($self, /)"),
    PY_METHOD("truncate", stringIOTruncate, "($self, pos=None, /)"),
    PY_METHOD("seek", stringIOSeek, "($self, pos, whence=0, /)"),
    PY_METHOD("write", stringIOWrite, "($self, s, /)"),
    PY_METHOD("seekable", stringIOTrueIfUsable, "($self, /)"),
    PY_METHOD("readable", stringIOTrueIfUsable, "($self, /)"),
    PY_METHOD("writable", stringIOTrueIfUsable, "($self, /)"),
    PY_METHOD("__getstate__", stringIOGetState, "($self, /)"),
    PY_METHOD("__setstate__", stringIOSetState, "($self, state, /)"),
};

static constexpr NativeGetSet stringIOGetSets[] = {
    { "closed"_s, stringIOGetClosed },
    { "newlines"_s, stringIOGetNewlines },
    { "line_buffering"_s, stringIOGetLineBuffering },
};

constexpr TypeDef ioStringIOTypeDef { "_io.StringIO"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, stringIOMethods, stringIOGetSets,
    "Text I/O implementation using an in-memory buffer.\n\n"
    "The initial_value argument sets the value of object.  The newline\n"
    "argument is like the one of TextIOWrapper's constructor."_s };

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
