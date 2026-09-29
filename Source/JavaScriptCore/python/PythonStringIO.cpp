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

#include "PythonCharacters.h"
#include <wtf/text/StringBuilder.h>

// io.StringIO: Modules/_io/stringio.c of CPython.

namespace JSC { namespace Python {

namespace {

struct StringIOState final : NativeState {
    PYTHON_NATIVE_STATE(StringIOState);

    // While it has only ever been added to at the end, what is in it is in `writer`, and getting it out is making a string of that. Once anything else is done to it, it is in `buffer`, a character to each, so
    // that where a character is is a matter of counting.
    bool isAccumulating { false };
    StringBuilder writer { OverflowPolicy::RecordOverflow };
    Vector<char32_t> buffer;
    int64_t position { 0 };
    int64_t size { 0 }; // In characters.
    bool isInitialized { false };
    bool isClosed { false };
    LineEndings endings;
    String writeNewline; // What "\n" is written as, if not as it is.
    WriteBarrier<Unknown> decoder;
};

template<typename Visitor>
void StringIOState::visit(Visitor& visitor)
{
    visitor.append(decoder);
}

StringIOState& stateOfString(JSValue self) { return stateOf<StringIOState>(self); }

bool checkIsInitialized(JSGlobalObject* globalObject, ThrowScope& scope, StringIOState& state)
{
    if (state.isInitialized)
        return true;
    raiseValueError(globalObject, scope, "I/O operation on uninitialized object"_s);
    return false;
}

bool checkIsOpen(JSGlobalObject* globalObject, ThrowScope& scope, StringIOState& state)
{
    if (!state.isClosed)
        return true;
    raiseValueError(globalObject, scope, "I/O operation on closed file"_s);
    return false;
}

#define CHECK_INITIALIZED() \
    if (!checkIsInitialized(globalObject, scope, state)) \
        return { };
#define CHECK_CLOSED() \
    if (!checkIsOpen(globalObject, scope, state)) \
        return { };

// The characters of a string, put one to each from somewhere on. There is room for them.
void copyCharacters(const String& text, std::span<char32_t> out)
{
    size_t at = 0;
    if (text.is8Bit()) {
        for (auto c : text.span8())
            out[at++] = c;
        return;
    }
    auto units = text.span16();
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t c = units[i];
        if (U16_IS_LEAD(c) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            c = U16_GET_SUPPLEMENTARY(c, units[++i]);
        out[at++] = c;
    }
}

// PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, ...)
JSValue strFromCharacters(JSGlobalObject* globalObject, std::span<const char32_t> characters)
{
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    builder.reserveCapacity(characters.size());
    for (char32_t c : characters) {
        if (U_IS_BMP(c))
            builder.append(static_cast<char16_t>(c));
        else
            builder.append(c);
    }
    return strOrMemoryError(globalObject, builder.hasOverflowed() ? String() : builder.isEmpty() ? emptyString() : builder.toString());
}

bool resizeBuffer(JSGlobalObject* globalObject, StringIOState& state, uint64_t size)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / sizeof(char32_t)) {
        raise(globalObject, scope, BuiltinType::OverflowError, "new buffer size too large"_s);
        return false;
    }
    if (size <= state.buffer.size()) {
        state.buffer.shrink(static_cast<size_t>(size));
        if (size < state.buffer.capacity() / 2)
            state.buffer.shrinkToFit();
        return true;
    }
    if (!state.buffer.tryGrow(static_cast<size_t>(size))) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    return true;
}

// What has been accumulated, as a str. It goes on accumulating: make_intermediate().
JSValue accumulated(JSGlobalObject* globalObject, StringIOState& state)
{
    return strOrMemoryError(globalObject, state.writer.hasOverflowed() ? String() : state.writer.isEmpty() ? emptyString() : state.writer.toString());
}

// realize(). False if it raised.
bool realize(JSGlobalObject* globalObject, StringIOState& state)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!state.isAccumulating)
        return true;
    state.isAccumulating = false;
    if (state.writer.hasOverflowed()) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    String text = state.writer.toString();
    state.writer.clear();
    if (!resizeBuffer(globalObject, state, static_cast<uint64_t>(state.size)))
        return false;
    copyCharacters(text, state.buffer.mutableSpan());
    return true;
}

#define ENSURE_REALIZED() \
    if (!realize(globalObject, state)) \
        return { };

// write_str(). False if it raised.
bool writeString(JSGlobalObject* globalObject, StringIOState& state, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue decoded = object;
    if (state.decoder) {
        decoded = decodeNewlines(globalObject, state.decoder.get(), object, true);
        RETURN_IF_EXCEPTION(scope, false);
    }
    String text = stringIn(decoded)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!state.writeNewline.isNull())
        text = makeStringByReplacingAll(text, "\n"_s, state.writeNewline);
    int64_t length = Characters(vm, text).count();
    if (state.position > std::numeric_limits<int64_t>::max() - length) {
        raise(globalObject, scope, BuiltinType::OverflowError, "new position too large"_s);
        return false;
    }
    bool isWritten = false;
    if (state.isAccumulating) {
        if (state.size == state.position) {
            state.writer.append(text);
            isWritten = true;
        } else if (!realize(globalObject, state))
            return false;
    }
    if (!isWritten) {
        int64_t oldSize = state.size;
        if (state.position + length > oldSize) {
            if (!resizeBuffer(globalObject, state, static_cast<uint64_t>(state.position + length)))
                return false;
        }
        // If it is past the end, what is between is zeros.
        for (int64_t i = oldSize; i < state.position; ++i)
            state.buffer[static_cast<size_t>(i)] = 0;
        copyCharacters(text, state.buffer.mutableSpan().subspan(static_cast<size_t>(state.position)));
    }
    state.position += length;
    state.size = std::max(state.size, state.position);
    return true;
}

// _stringio_readline()
JSValue readLine(JSGlobalObject* globalObject, StringIOState& state, int64_t limit)
{
    if (state.position >= state.size)
        return jsEmptyString(globalObject->vm());
    if (limit < 0 || limit > state.size - state.position)
        limit = state.size - state.position;
    auto text = state.buffer.span().subspan(static_cast<size_t>(state.position), static_cast<size_t>(limit));
    size_t consumed;
    size_t length = findLineEnding(state.endings, text, consumed).value_or(text.size());
    state.position += static_cast<int64_t>(length);
    return strFromCharacters(globalObject, text.first(length));
}

JSValue valueOf(JSGlobalObject* globalObject, StringIOState& state)
{
    if (state.isAccumulating)
        return accumulated(globalObject, state);
    return strFromCharacters(globalObject, state.buffer.span().first(static_cast<size_t>(state.size)));
}

// StringIO.__init__(). False if it raised.
bool initialize(JSGlobalObject* globalObject, JSCell* self, JSValue value, JSValue newlineObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = stateOfString(self);
    String newline = "\n"_s;
    if (newlineObject && isNone(newlineObject))
        newline = String();
    else if (newlineObject) {
        JSString* string = stringIn(newlineObject);
        if (!string) {
            raiseTypeError(globalObject, scope, concatenate("newline must be str or None, not "_s, typeName(globalObject, newlineObject)));
            return false;
        }
        newline = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (newline.isNull())
            newline = emptyString();
        // It is as much of it as comes before a zero, as it is to C.
        if (size_t zero = newline.find(static_cast<char16_t>(0)); zero != notFound)
            newline = newline.left(zero);
    }
    if (!isValidNewline(newline)) {
        String shown = repr(globalObject, newlineObject);
        RETURN_IF_EXCEPTION(scope, false);
        raiseValueError(globalObject, scope, concatenate("illegal newline value: "_s, shown));
        return false;
    }
    if (value && !isNone(value) && !stringIn(value)) {
        raiseTypeError(globalObject, scope, concatenate("initial_value must be str or None, not "_s, typeName(globalObject, value)));
        return false;
    }

    state.isInitialized = false;
    state.writer.clear();
    state.decoder.clear();
    state.endings.readNewline = newline;
    state.endings.isUniversal = newline.isNull() || newline.isEmpty();
    state.endings.isTranslated = newline.isNull();
    // With "" nothing is translated, and with "\n" or None it is to "\n", which is to do nothing.
    state.writeNewline = !newline.isNull() && newline.startsWith('\r') ? newline : String();
    if (state.endings.isUniversal) {
        JSValue decoder = call(globalObject, ioState(globalObject).incrementalNewlineDecoder.get(), jsUndefined(), jsBoolean(state.endings.isTranslated));
        RETURN_IF_EXCEPTION(scope, false);
        state.decoder.set(vm, self, decoder);
    }

    state.size = 0;
    state.buffer.clear();
    state.position = 0;
    bool hasValue = false;
    if (value && !isNone(value)) {
        hasValue = stringIn(value)->length();
    }
    if (hasValue) {
        state.isAccumulating = false;
        if (!writeString(globalObject, state, value))
            return false;
    } else
        state.isAccumulating = true;
    state.position = 0;
    state.isClosed = false;
    state.isInitialized = true;
    return true;
}

} // anonymous namespace

PYTHON_NATIVE(stringIONew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<StringIOState>()));
}

// StringIO(initial_value='', newline='\n')
PYTHON_NATIVE(stringIOInit)
{
    NativeArguments args(callFrame);
    if (!initialize(globalObject, args[0].asCell(), args.at(1), args.at(2)))
        return { };
    RETURN_NONE();
}

PYTHON_NATIVE(stringIOGetValue)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfString(args[0]);
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    RELEASE_AND_RETURN(scope, JSValue::encode(valueOf(globalObject, state)));
}

enum class StringAsk : uint8_t { True, Tell };

PYTHON_NATIVE(stringIOAsk)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfString(args[0]);
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    if (unpack<StringAsk>(callFrame, 0) == StringAsk::True)
        return JSValue::encode(jsBoolean(true));
    return JSValue::encode(intFromInt64(globalObject, state.position));
}

PYTHON_NATIVE(stringIORead)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfString(args[0]);
    auto given = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    int64_t size = *given;
    int64_t left = state.size - state.position;
    if (size < 0 || size > left)
        size = std::max<int64_t>(left, 0);
    // seek(0) and then read()
    if (state.isAccumulating && !state.position && size == left) {
        state.position = state.size;
        RELEASE_AND_RETURN(scope, JSValue::encode(accumulated(globalObject, state)));
    }
    ENSURE_REALIZED();
    if (!size)
        return JSValue::encode(jsEmptyString(vm));
    auto characters = state.buffer.span().subspan(static_cast<size_t>(state.position), static_cast<size_t>(size));
    state.position += size;
    RELEASE_AND_RETURN(scope, JSValue::encode(strFromCharacters(globalObject, characters)));
}

PYTHON_NATIVE(stringIOReadLine)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfString(args[0]);
    auto size = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    ENSURE_REALIZED();
    RELEASE_AND_RETURN(scope, JSValue::encode(readLine(globalObject, state, *size)));
}

PYTHON_NATIVE(stringIONext)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfString(self);
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    ENSURE_REALIZED();
    JSValue line;
    if (typeOf(globalObject, self) == ioState(globalObject).stringIO.get())
        line = readLine(globalObject, state, -1);
    else {
        line = callMethodNamed(globalObject, self, names.attribute_readline);
        if (line && !stringIn(line))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, concatenate("readline() should have returned a str object, not '"_s, typeName(globalObject, line), '\'')));
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(line)->length())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    return JSValue::encode(line);
}

PYTHON_NATIVE(stringIOTruncate)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfString(args[0]);
    auto given = toOptionalSsize(globalObject, args.at(1), state.position);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    int64_t size = *given;
    if (size < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("Negative size value "_s, size)));
    if (size < state.size) {
        ENSURE_REALIZED();
        if (!resizeBuffer(globalObject, state, static_cast<uint64_t>(size)))
            return { };
        state.size = size;
    }
    return JSValue::encode(intFromInt64(globalObject, size));
}

PYTHON_NATIVE(stringIOSeek)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfString(args[0]);
    auto given = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t position = *given;
    int whence = 0;
    if (JSValue value = args.at(2)) {
        auto givenWhence = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        whence = *givenWhence;
    }
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    if (whence < 0 || whence > 2)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("Invalid whence ("_s, whence, ", should be 0, 1 or 2)"_s)));
    if (position < 0 && !whence)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("Negative seek position "_s, position)));
    if (whence && position)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, "Can't do nonzero cur-relative seeks"_s));
    if (whence == 1)
        position = state.position;
    else if (whence == 2)
        position = state.size;
    state.position = position;
    return JSValue::encode(intFromInt64(globalObject, position));
}

PYTHON_NATIVE(stringIOWrite)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfString(args[0]);
    CHECK_INITIALIZED();
    JSString* string = stringIn(args[1]);
    if (!string)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("string argument expected, got '"_s, typeName(globalObject, args[1]), '\'')));
    CHECK_CLOSED();
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned size = Characters(vm, text).count();
    if (size && !writeString(globalObject, state, args[1]))
        return { };
    return JSValue::encode(jsNumber(size));
}

PYTHON_NATIVE(stringIOClose)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    auto& state = stateOfString(args[0]);
    state.isClosed = true;
    state.buffer.clear();
    state.writer.clear();
    state.endings.readNewline = String();
    state.writeNewline = String();
    state.decoder.clear();
    RETURN_NONE();
}

PYTHON_NATIVE(stringIOGetState)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfString(self);
    CHECK_INITIALIZED();
    CHECK_CLOSED();
    JSValue value = valueOf(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue attributes = jsUndefined();
    PyDict* own = PyDict::backedBy(globalObject, asObject(self));
    if (own->size()) {
        PyDict* copy = PyDict::create(globalObject);
        copy->copyFrom(globalObject, *own);
        RETURN_IF_EXCEPTION(scope, { });
        attributes = copy;
    }
    JSValue newline = state.endings.readNewline.isNull() ? jsUndefined() : JSValue(jsString(vm, state.endings.readNewline));
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { value, newline, intFromInt64(globalObject, state.position), attributes })));
}

PYTHON_NATIVE(stringIOSetState)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfString(self);
    JSValue given = args[1];
    CHECK_CLOSED();
    // It may come to have more in it some day.
    if (!isInstance(globalObject, given, realm->typeTuple()) || asTuple(given)->length() < 4)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(typeName(globalObject, self), ".__setstate__ argument should be 4-tuple, got "_s, typeName(globalObject, given))));
    PyTuple* tuple = asTuple(given);
    if (!initialize(globalObject, self, tuple->at(0), tuple->at(1)))
        return { };
    // What that put in it may have had its line endings translated, which what is given here has had done to it once already. So it is put in again as it is.
    state.isAccumulating = false;
    state.writer.clear();
    state.size = 0;
    if (JSString* string = stringIn(tuple->at(0))) {
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        unsigned length = Characters(vm, text).count();
        if (!resizeBuffer(globalObject, state, length))
            return { };
        copyCharacters(text, state.buffer.mutableSpan());
        state.size = length;
    } else
        state.buffer.clear();
    JSValue position = tuple->at(2);
    if (!isInstance(globalObject, position, realm->typeInt()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("third item of state must be an integer, got "_s, typeName(globalObject, position))));
    auto newPosition = toSsize(globalObject, position);
    RETURN_IF_EXCEPTION(scope, { });
    if (*newPosition < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "position value cannot be negative"_s));
    state.position = *newPosition;
    JSValue attributes = tuple->at(3);
    if (!isNone(attributes)) {
        if (!isInstance(globalObject, attributes, realm->typeDict()))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("fourth item of state should be a dict, got a "_s, typeName(globalObject, attributes))));
        updateDictFrom(globalObject, PyDict::backedBy(globalObject, asObject(self)), attributes);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

void initializeStringIO(JSGlobalObject* globalObject, IOModuleState& io)
{
    VM& vm = globalObject->vm();
    using Kind = PyNativeFunction::Kind;
    PyType* type = createBuiltinType(globalObject, "_io.StringIO"_s, io.textIOBase.get(), PyType::Layout::Native, PyType::IsBaseType);
    io.stringIO.set(vm, globalObject->pyRealm(), type);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    addMethods(globalObject, type, {
        { "__new__"_s, stringIONew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__init__"_s, stringIOInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__next__"_s, stringIONext },
        { "close"_s, stringIOClose },
        { "getvalue"_s, stringIOGetValue },
        { "read"_s, stringIORead },
        { "readline"_s, stringIOReadLine },
        { "tell"_s, stringIOAsk, Kind::Method, pack(StringAsk::Tell) },
        { "truncate"_s, stringIOTruncate },
        { "seek"_s, stringIOSeek },
        { "write"_s, stringIOWrite },
        { "seekable"_s, stringIOAsk, Kind::Method, pack(StringAsk::True) },
        { "readable"_s, stringIOAsk, Kind::Method, pack(StringAsk::True) },
        { "writable"_s, stringIOAsk, Kind::Method, pack(StringAsk::True) },
        { "__getstate__"_s, stringIOGetState },
        { "__setstate__"_s, stringIOSetState },
    });
    addGetSet(globalObject, type, "closed"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        auto& state = stateOfString(self);
        CHECK_INITIALIZED();
        return jsBoolean(state.isClosed);
    });
    addGetSet(globalObject, type, "line_buffering"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        auto& state = stateOfString(self);
        CHECK_INITIALIZED();
        CHECK_CLOSED();
        return jsBoolean(false);
    });
    addGetSet(globalObject, type, "newlines"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto& state = stateOfString(self);
        CHECK_INITIALIZED();
        CHECK_CLOSED();
        if (!state.decoder)
            return jsUndefined();
        RELEASE_AND_RETURN(scope, getAttribute(globalObject, state.decoder.get(), vm.pythonNames().attribute_newlines));
    });
}

} } // namespace JSC::Python
