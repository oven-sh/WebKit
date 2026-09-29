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

// io.BytesIO: Modules/_io/bytesio.c of CPython.
//
// In CPython what is in it cannot change size while there is a memoryview of it from getbuffer(), which is until the last reference to the view goes. Here that would be until some collection or other, and it is
// as it is with a bytearray: see "Where it differs from CPython on purpose" in README.md.

namespace JSC { namespace Python {

namespace {

struct BytesIOState final : NativeState {
    PYTHON_NATIVE_STATE(BytesIOState);

    // What is in it is in one of these or in both. It is in the first, which is bytes, when it was made from that or that has been given out, so that neither takes copying anything. It is in the second, which is a
    // bytearray, once it has been written to, and then the first is let go of.
    WriteBarrier<JSObject> shared;
    WriteBarrier<JSObject> array;
    int64_t position { 0 };
    // How large CPython's buffer would be, which is all that __sizeof__() goes by.
    size_t allocated { 0 };
    bool isClosed { false };
    // There may be a memoryview through which it can be written to with nothing here being told, so nothing that has been given out can be taken for what is in it now.
    bool hasGivenBuffer { false };

    std::span<const uint8_t> content() const
    {
        JSObject* holder = array ? array.get() : shared.get();
        return holder ? uncheckedDowncast<JSUint8Array>(holder)->span() : std::span<const uint8_t>();
    }
    int64_t size() const { return static_cast<int64_t>(content().size()); }
};

template<typename Visitor>
void BytesIOState::visit(Visitor& visitor)
{
    visitor.append(shared);
    visitor.append(array);
}

BytesIOState& stateOfBytes(JSValue self) { return stateOf<BytesIOState>(self); }

bool checkIsOpen(JSGlobalObject* globalObject, ThrowScope& scope, BytesIOState& state)
{
    if (!state.isClosed)
        return true;
    raiseValueError(globalObject, scope, "I/O operation on closed file."_s);
    return false;
}

#define CHECK_CLOSED() \
    if (!checkIsOpen(globalObject, scope, state)) \
        return { };

// What is in it is in the bytearray and nowhere else, and is so many bytes. What is added is zeros. False if it raised.
bool makeWritable(JSGlobalObject* globalObject, JSCell* owner, BytesIOState& state, size_t size)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!state.array) {
        JSUint8Array* array = newByteArray(globalObject, state.content());
        RETURN_IF_EXCEPTION(scope, false);
        state.array.set(vm, owner, array);
    }
    state.shared.clear();
    RELEASE_AND_RETURN(scope, resizeByteArray(globalObject, uncheckedDowncast<JSUint8Array>(state.array.get()), size));
}

// resize_buffer(), as far as how much room CPython would have goes.
void noteResize(BytesIOState& state, size_t size)
{
    size_t allocated = state.allocated;
    if (size < allocated / 2)
        allocated = size + 1;
    else if (size < allocated)
        return;
    else if (size <= allocated * 1.125)
        allocated = size + (size >> 3) + (size < 9 ? 3 : 6);
    else
        allocated = size + 1;
    state.allocated = allocated;
}

// scan_eol(): how far it is from where it is to the end of the line, and no further than `limit` if that is not negative.
int64_t scanToEndOfLine(BytesIOState& state, int64_t limit)
{
    if (state.position >= state.size())
        return 0;
    int64_t most = state.size() - state.position;
    if (limit < 0 || limit > most)
        limit = most;
    if (limit) {
        size_t found = find(state.content().subspan(static_cast<size_t>(state.position), static_cast<size_t>(limit)), '\n');
        if (found != notFound)
            limit = static_cast<int64_t>(found) + 1;
    }
    return limit;
}

// All of it, as bytes: what getvalue() gives.
JSValue valueOf(JSGlobalObject* globalObject, JSCell* owner, BytesIOState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (state.size() <= 1 || state.hasGivenBuffer)
        RELEASE_AND_RETURN(scope, newBytes(globalObject, state.content()));
    state.allocated = static_cast<size_t>(state.size());
    if (!state.shared) {
        JSUint8Array* bytes = newBytes(globalObject, state.content());
        RETURN_IF_EXCEPTION(scope, { });
        state.shared.set(vm, owner, bytes);
    }
    return state.shared.get();
}

// read_bytes()
JSValue readBytes(JSGlobalObject* globalObject, JSCell* owner, BytesIOState& state, int64_t size)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (size > 1 && !state.position && size == state.size() && static_cast<size_t>(size) == state.allocated && !state.hasGivenBuffer) {
        state.position += size;
        RELEASE_AND_RETURN(scope, valueOf(globalObject, owner, state));
    }
    if (!size)
        RELEASE_AND_RETURN(scope, newBytes(globalObject, { }));
    auto bytes = state.content().subspan(static_cast<size_t>(state.position), static_cast<size_t>(size));
    state.position += size;
    RELEASE_AND_RETURN(scope, newBytes(globalObject, bytes));
}

// write_bytes(): how many were written. Negative if it raised.
int64_t writeBytes(JSGlobalObject* globalObject, JSCell* owner, BytesIOState& state, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Buffer buffer = bufferOf(globalObject, value);
    RETURN_IF_EXCEPTION(scope, -1);
    if (!checkIsOpen(globalObject, scope, state))
        return -1;
    size_t length = buffer.size();
    if (!length)
        return 0;
    // They may be its own, by way of getbuffer(), and about to be moved.
    ByteVector copy;
    copy.append(buffer.span());
    if (copy.hasOverflowed()) {
        raiseMemoryError(globalObject, scope);
        return -1;
    }
    uint64_t end = static_cast<uint64_t>(state.position) + length;
    if (end > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        raise(globalObject, scope, BuiltinType::OverflowError, "new buffer size too large"_s);
        return -1;
    }
    if (end > state.allocated)
        noteResize(state, static_cast<size_t>(end));
    else if (state.shared)
        state.allocated = std::max<size_t>(static_cast<size_t>(end), static_cast<size_t>(state.size()));
    // If it is past the end, what is between is zeros.
    if (!makeWritable(globalObject, owner, state, std::max<size_t>(static_cast<size_t>(end), static_cast<size_t>(state.size()))))
        return -1;
    memcpySpan(uncheckedDowncast<JSUint8Array>(state.array.get())->typedSpan().subspan(static_cast<size_t>(state.position), length), copy.span());
    state.position = static_cast<int64_t>(end);
    return static_cast<int64_t>(length);
}

} // anonymous namespace

PYTHON_NATIVE(bytesIONew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<BytesIOState>()));
}

// BytesIO(initial_bytes=b'')
PYTHON_NATIVE(bytesIOInit)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfBytes(self);
    JSValue initial = args.at(1);
    // It may not be the first time.
    state.position = 0;
    if (initial && !isNone(initial) && typeOf(globalObject, initial) == realm->typeBytes()) {
        state.array.clear();
        state.shared.set(vm, self, asObject(initial));
        state.allocated = static_cast<size_t>(state.size());
        state.isClosed = false;
        RETURN_NONE();
    }
    if (!state.isClosed) {
        if (!makeWritable(globalObject, self, state, 0))
            return { };
    }
    if (initial && !isNone(initial)) {
        writeBytes(globalObject, self, state, initial);
        RETURN_IF_EXCEPTION(scope, { });
        state.position = 0;
    }
    RETURN_NONE();
}

enum class BytesAsk : uint8_t { True, False, None, Tell };

PYTHON_NATIVE(bytesIOAsk)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    CHECK_CLOSED();
    switch (unpack<BytesAsk>(callFrame, 0)) {
    case BytesAsk::True:
        return JSValue::encode(jsBoolean(true));
    case BytesAsk::False:
        return JSValue::encode(jsBoolean(false));
    case BytesAsk::None:
        RETURN_NONE();
    case BytesAsk::Tell:
        return JSValue::encode(intFromInt64(globalObject, state.position));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PYTHON_NATIVE(bytesIOGetBuffer)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfBytes(self);
    CHECK_CLOSED();
    if (!makeWritable(globalObject, self, state, static_cast<size_t>(state.size())))
        return { };
    state.allocated = std::max<size_t>(state.allocated, static_cast<size_t>(state.size()));
    state.hasGivenBuffer = true;
    // What the view says that it is of is something in between, which has nothing to it but that it keeps this.
    // It is laid out as what stands between a memoryview and an object of a program's is: see BufferWrapperField in PythonBytes.cpp. There is nothing to be told when the last view is released.
    auto* between = PyNativeObject::create(vm, ioState(globalObject).bytesIOBuffer->instanceStructure());
    between->setField(vm, 0, jsUndefined());
    between->setField(vm, 1, jsUndefined());
    between->setField(vm, 2, jsNumber(0));
    between->setField(vm, 3, self);
    PyMemoryView::Dimension dimension { state.size(), 1 };
    return JSValue::encode(PyMemoryView::create(globalObject, state.array.get(), { }, { &dimension, 1 }, between));
}

PYTHON_NATIVE(bytesIOGetValue)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    CHECK_CLOSED();
    RELEASE_AND_RETURN(scope, JSValue::encode(valueOf(globalObject, args[0].asCell(), state)));
}

// read() and read1()
PYTHON_NATIVE(bytesIORead)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    auto size = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_CLOSED();
    int64_t left = state.size() - state.position;
    if (*size < 0 || *size > left)
        size = std::max<int64_t>(left, 0);
    RELEASE_AND_RETURN(scope, JSValue::encode(readBytes(globalObject, args[0].asCell(), state, *size)));
}

PYTHON_NATIVE(bytesIOReadLine)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    auto size = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_CLOSED();
    RELEASE_AND_RETURN(scope, JSValue::encode(readBytes(globalObject, args[0].asCell(), state, scanToEndOfLine(state, *size))));
}

PYTHON_NATIVE(bytesIOReadLines)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    JSValue argument = args.at(1);
    CHECK_CLOSED();
    int64_t most = -1;
    if (argument && isInstance(globalObject, argument, realm->typeInt())) {
        auto given = toSsize(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, { });
        most = *given;
    } else if (argument && !isNone(argument))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("integer argument expected, got '"_s, typeName(globalObject, argument), '\'')));
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t total = 0;
    while (int64_t count = scanToEndOfLine(state, -1)) {
        JSValue line = newBytes(globalObject, state.content().subspan(static_cast<size_t>(state.position), static_cast<size_t>(count)));
        RETURN_IF_EXCEPTION(scope, { });
        state.position += count;
        listAppend(globalObject, result, line);
        RETURN_IF_EXCEPTION(scope, { });
        total += count;
        if (most > 0 && total >= most)
            break;
    }
    return JSValue::encode(result);
}

PYTHON_NATIVE(bytesIOReadInto)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    Buffer buffer = writableBufferArgument(globalObject, args[1], "readinto"_s);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_CLOSED();
    int64_t count = static_cast<int64_t>(buffer.size());
    int64_t left = state.size() - state.position;
    if (count > left) {
        count = left;
        if (count < 0)
            return JSValue::encode(jsNumber(0));
    }
    memmoveSpan(mutableSpanOf(buffer).first(static_cast<size_t>(count)), state.content().subspan(static_cast<size_t>(state.position), static_cast<size_t>(count)));
    state.position += count;
    return JSValue::encode(intFromInt64(globalObject, count));
}

PYTHON_NATIVE(bytesIOTruncate)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfBytes(self);
    JSValue size = args.at(1);
    CHECK_CLOSED();
    int64_t newSize = state.position;
    if (size && !isNone(size)) {
        auto given = toCLong(globalObject, size);
        RETURN_IF_EXCEPTION(scope, { });
        newSize = *given;
        if (newSize < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, concatenate("negative size value "_s, newSize)));
    }
    if (newSize < state.size()) {
        noteResize(state, static_cast<size_t>(newSize));
        if (!makeWritable(globalObject, self, state, static_cast<size_t>(newSize)))
            return { };
    }
    return JSValue::encode(intFromInt64(globalObject, newSize));
}

PYTHON_NATIVE(bytesIONext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    CHECK_CLOSED();
    int64_t count = scanToEndOfLine(state, -1);
    if (!count)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    RELEASE_AND_RETURN(scope, JSValue::encode(readBytes(globalObject, args[0].asCell(), state, count)));
}

PYTHON_NATIVE(bytesIOSeek)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    auto given = toSsize(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t position = *given;
    int whence = 0;
    if (JSValue value = args.at(2)) {
        auto givenWhence = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        whence = *givenWhence;
    }
    CHECK_CLOSED();
    if (position < 0 && !whence)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("negative seek value "_s, position)));
    if (whence == 1 || whence == 2) {
        int64_t from = whence == 1 ? state.position : state.size();
        if (position > std::numeric_limits<int64_t>::max() - from)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "new position too large"_s));
        position += from;
    } else if (whence)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("invalid whence ("_s, whence, ", should be 0, 1 or 2)"_s)));
    state.position = std::max<int64_t>(position, 0);
    return JSValue::encode(intFromInt64(globalObject, state.position));
}

PYTHON_NATIVE(bytesIOWrite)
{
    NATIVE_PROLOGUE();
    int64_t count = writeBytes(globalObject, args[0].asCell(), stateOfBytes(args[0]), args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, count));
}

PYTHON_NATIVE(bytesIOWriteLines)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBytes(args[0]);
    CHECK_CLOSED();
    JSValue iterator = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        JSValue item = iteratorNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        writeBytes(globalObject, args[0].asCell(), state, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(bytesIOClose)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    auto& state = stateOfBytes(args[0]);
    state.isClosed = true;
    state.shared.clear();
    state.array.clear();
    state.allocated = 0;
    RETURN_NONE();
}

PYTHON_NATIVE(bytesIOGetState)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfBytes(self);
    CHECK_CLOSED();
    JSValue value = valueOf(globalObject, self, state);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue attributes = jsUndefined();
    PyDict* own = PyDict::backedBy(globalObject, asObject(self));
    if (own->size()) {
        PyDict* copy = PyDict::create(globalObject);
        copy->copyFrom(globalObject, *own);
        RETURN_IF_EXCEPTION(scope, { });
        attributes = copy;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { value, intFromInt64(globalObject, state.position), attributes })));
}

PYTHON_NATIVE(bytesIOSetState)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOfBytes(self);
    JSValue given = args[1];
    // It may come to have more in it some day.
    if (!isInstance(globalObject, given, realm->typeTuple()) || asTuple(given)->length() < 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(typeName(globalObject, self), ".__setstate__ argument should be 3-tuple, got "_s, typeName(globalObject, given))));
    PyTuple* tuple = asTuple(given);
    // As it was when it was made, in case this is not the first time.
    state.position = 0;
    if (!state.isClosed && !makeWritable(globalObject, self, state, 0))
        return { };
    writeBytes(globalObject, self, state, tuple->at(0));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue position = tuple->at(1);
    if (!isInstance(globalObject, position, realm->typeInt()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("second item of state must be an integer, not "_s, typeName(globalObject, position))));
    auto newPosition = toSsize(globalObject, position);
    RETURN_IF_EXCEPTION(scope, { });
    if (*newPosition < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "position value cannot be negative"_s));
    state.position = *newPosition;
    JSValue attributes = tuple->at(2);
    if (!isNone(attributes)) {
        if (!isInstance(globalObject, attributes, realm->typeDict()))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("third item of state should be a dict, got a "_s, typeName(globalObject, attributes))));
        updateDictFrom(globalObject, PyDict::backedBy(globalObject, asObject(self)), attributes);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(bytesIOSizeOf)
{
    NativeArguments args(callFrame);
    auto& state = stateOfBytes(args[0]);
    // What a bytes takes besides what is in it: PyBytesObject_SIZE.
    constexpr int64_t sizeOfEmptyBytes = 33;
    int64_t size = typeOf(globalObject, args[0])->basicSize();
    // What it has to itself counts. What it was made from does not, nor the one empty bytes that there is.
    if (state.array && state.allocated)
        size += sizeOfEmptyBytes + static_cast<int64_t>(state.allocated);
    return JSValue::encode(intFromInt64(globalObject, size));
}

void initializeBytesIO(JSGlobalObject* globalObject, IOModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;

    PyType* between = createBuiltinType(globalObject, "_io._BytesIOBuffer"_s, realm->typeObject(), PyType::Layout::Native, 0);
    state.bytesIOBuffer.set(vm, realm, between);
    between->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, between));

    PyType* type = createBuiltinType(globalObject, "_io.BytesIO"_s, state.bufferedIOBase.get(), PyType::Layout::Native, PyType::IsBaseType);
    state.bytesIO.set(vm, realm, type);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    addMethods(globalObject, type, {
        { "__new__"_s, bytesIONew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__init__"_s, bytesIOInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__iter__"_s, nativeSelf },
        { "__next__"_s, bytesIONext },
        { "readable"_s, bytesIOAsk, Kind::Method, pack(BytesAsk::True) },
        { "seekable"_s, bytesIOAsk, Kind::Method, pack(BytesAsk::True) },
        { "writable"_s, bytesIOAsk, Kind::Method, pack(BytesAsk::True) },
        { "close"_s, bytesIOClose },
        { "flush"_s, bytesIOAsk, Kind::Method, pack(BytesAsk::None) },
        { "isatty"_s, bytesIOAsk, Kind::Method, pack(BytesAsk::False) },
        { "tell"_s, bytesIOAsk, Kind::Method, pack(BytesAsk::Tell) },
        { "write"_s, bytesIOWrite },
        { "writelines"_s, bytesIOWriteLines },
        { "read1"_s, bytesIORead },
        { "readinto"_s, bytesIOReadInto },
        { "readline"_s, bytesIOReadLine },
        { "readlines"_s, bytesIOReadLines },
        { "read"_s, bytesIORead },
        { "getbuffer"_s, bytesIOGetBuffer, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass },
        { "getvalue"_s, bytesIOGetValue },
        { "seek"_s, bytesIOSeek },
        { "truncate"_s, bytesIOTruncate },
        { "__getstate__"_s, bytesIOGetState },
        { "__setstate__"_s, bytesIOSetState },
        { "__sizeof__"_s, bytesIOSizeOf },
    });
    addGetSet(globalObject, type, "closed"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfBytes(self).isClosed); });
}

} } // namespace JSC::Python
