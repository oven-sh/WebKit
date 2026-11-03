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

// BytesIO: a file in memory. (Modules/_io/bytesio.c in CPython.)

namespace {

struct BytesIOState {
    int64_t position { 0 };
    int64_t size { 0 }; // Of the contents. The buffer may be larger.
    int64_t exports { 0 }; // How many views getbuffer() has handed out: the buffer must stay as it is meanwhile.
};

enum BytesIOField : unsigned {
    BytesIOStateField,
    // A bytes object, which others may hold too and which is copied before anything is written, or a PyByteStorage, which is
    // the stream's own. Empty: the stream is closed.
    BytesIOBufferField,
    BytesIOFieldCount,
};

enum BytesIOExporterField : unsigned {
    BytesIOExporterSourceField, // The BytesIO.
    BytesIOExporterFieldCount,
};

} // namespace

static ALWAYS_INLINE BytesIOState& bytesIOStateOf(PyRecord* stream) { return ioStateOf<BytesIOState>(stream); }
static ALWAYS_INLINE bool bytesIOIsClosed(PyRecord* stream) { return !stream->get(BytesIOBufferField); }
static ALWAYS_INLINE PyBytes* bytesIOSharedBuffer(PyRecord* stream) { return PyBytes::tryFrom(stream->get(BytesIOBufferField)); }

static ALWAYS_INLINE const uint8_t* bytesIOData(PyRecord* stream)
{
    JSCell* buffer = stream->get(BytesIOBufferField).asCell();
    return buffer->type() == PyBytesType ? uncheckedDowncast<PyBytes>(buffer)->data() : uncheckedDowncast<PyByteStorage>(buffer)->data();
}

static ALWAYS_INLINE size_t bytesIOCapacity(PyRecord* stream)
{
    JSCell* buffer = stream->get(BytesIOBufferField).asCell();
    return buffer->type() == PyBytesType ? uncheckedDowncast<PyBytes>(buffer)->length() : uncheckedDowncast<PyByteStorage>(buffer)->capacity();
}

// False: a ValueError.
static bool bytesIOCheckOpen(JSGlobalObject* globalObject, PyRecord* stream)
{
    if (!bytesIOIsClosed(stream)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ioRaiseClosed(globalObject, scope);
    return false;
}

// False: a BufferError.
static bool bytesIOCheckExports(JSGlobalObject* globalObject, PyRecord* stream)
{
    auto& state = bytesIOStateOf(stream);
    if (!isStillExported(globalObject, stream, [&] { return state.exports; })) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseBufferError(globalObject, scope, "Existing exports of data: object cannot be re-sized"_s);
    return false;
}

// Gives the stream a buffer of its own with room for `capacity` bytes, which is no less than the contents take.
static bool bytesIOReallocate(JSGlobalObject* globalObject, PyRecord* stream, size_t capacity)
{
    auto& state = bytesIOStateOf(stream);
    ASSERT(capacity >= static_cast<size_t>(state.size));
    PyByteStorage* storage = ioTryNewStorage(globalObject, capacity);
    if (!storage)
        return false;
    memcpy(storage->data(), bytesIOData(stream), state.size);
    stream->set(getVM(globalObject), BytesIOBufferField, storage);
    return true;
}

// Makes room for `size` bytes, or gives back what that many have no use for.
static bool bytesIOResizeBuffer(JSGlobalObject* globalObject, PyRecord* stream, size_t size)
{
    size_t capacity = bytesIOCapacity(stream);
    if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
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
    return bytesIOReallocate(globalObject, stream, capacity);
}

// How long the line at the position is, looking no further than `limit` bytes if that is not negative.
static int64_t bytesIOScanLine(PyRecord* stream, int64_t limit)
{
    auto& state = bytesIOStateOf(stream);
    if (state.position >= state.size)
        return 0;
    int64_t available = state.size - state.position;
    if (limit < 0 || limit > available)
        limit = available;
    if (limit) {
        const uint8_t* start = bytesIOData(stream) + state.position;
        if (auto* newline = static_cast<const uint8_t*>(memchr(start, '\n', limit)))
            limit = newline - start + 1;
    }
    return limit;
}

// The next `size` bytes, which are there.
static JSValue bytesIOTake(JSGlobalObject* globalObject, PyRecord* stream, int64_t size)
{
    auto& state = bytesIOStateOf(stream);
    if (!size)
        return runtime(globalObject).emptyBytes();
    if (PyBytes* shared = bytesIOSharedBuffer(stream); shared && !state.position && static_cast<size_t>(size) == shared->length()) {
        state.position = size;
        return shared;
    }
    PyBytes* result = ioTryNewBytes(globalObject, size);
    if (!result)
        return { };
    memcpy(result->mutableData(), bytesIOData(stream) + state.position, size);
    state.position += size;
    return result;
}

// The number of bytes written. -1: an exception.
static int64_t bytesIOWriteBytes(JSGlobalObject* globalObject, PyRecord* stream, JSValue data)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    IOBuffer buffer;
    if (!buffer.acquire(globalObject, data))
        return -1;
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)) || failed(scope, bytesIOCheckExports(globalObject, stream)))
        return -1;
    size_t length = buffer.size();
    if (!length)
        return 0;

    auto& state = bytesIOStateOf(stream);
    size_t end = static_cast<size_t>(state.position) + length;
    if (end > bytesIOCapacity(stream)) {
        if (!bytesIOResizeBuffer(globalObject, stream, end))
            return -1;
    } else if (bytesIOSharedBuffer(stream)) {
        if (!bytesIOReallocate(globalObject, stream, std::max(end, static_cast<size_t>(state.size))))
            return -1;
    }

    uint8_t* contents = uncheckedDowncast<PyByteStorage>(stream->get(BytesIOBufferField).asCell())->data();
    // What a seek beyond the end has skipped reads as zeros.
    if (state.position > state.size)
        memset(contents + state.size, 0, state.position - state.size);
    memcpy(contents + state.position, buffer.span().data(), length);
    state.position = end;
    state.size = std::max<int64_t>(state.size, end);
    return length;
}

// ---- Methods ----

static JSValue bytesIONew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    PyRecord* stream = ioAllocate<BytesIOState>(globalObject, cls, BytesIOFieldCount);
    stream->set(getVM(globalObject), BytesIOBufferField, runtime(globalObject).emptyBytes());
    // There is nothing to be done for a BytesIO nobody refers to. A subclass may have given close() something to do.
    if (stream->type()->isHeapType())
        ioDidOpen(globalObject, stream);
    return stream;
}

static JSValue bytesIOInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Runtime& rt = runtime(globalObject);
    static constexpr ASCIILiteral keywords[] = { "initial_bytes"_s };
    JSValue initial;
    if (failed(scope, unpackKeywordArguments(globalObject, "BytesIO"_s, keywords, 0, 1, false, args, &initial)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckExports(globalObject, stream)))
        return { };

    auto& state = bytesIOStateOf(stream);
    state.size = 0;
    state.position = 0;
    if (initial && !isNone(initial)) {
        if (auto* bytes = PyBytes::tryFrom(initial); bytes && bytes->type() == rt.bytesType()) {
            stream->set(rt.vm(), BytesIOBufferField, bytes);
            state.size = bytes->length();
        } else {
            if (bytesIOWriteBytes(globalObject, stream, initial) < 0)
                return { };
            state.position = 0;
        }
    }
    return jsUndefined();
}

static JSValue bytesIOTrueIfOpen(JSGlobalObject* globalObject, JSValue self) { return bytesIOCheckOpen(globalObject, PyRecord::from(self)) ? jsBoolean(true) : JSValue(); }
static JSValue bytesIOFalseIfOpen(JSGlobalObject* globalObject, JSValue self) { return bytesIOCheckOpen(globalObject, PyRecord::from(self)) ? jsBoolean(false) : JSValue(); }
static JSValue bytesIONoneIfOpen(JSGlobalObject* globalObject, JSValue self) { return bytesIOCheckOpen(globalObject, PyRecord::from(self)) ? jsUndefined() : JSValue(); }
static JSValue bytesIOGetClosed(JSGlobalObject*, JSValue self) { return jsBoolean(bytesIOIsClosed(PyRecord::from(self))); }

static JSValue bytesIOGetBuffer(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, bytesIOCheckOpen(globalObject, PyRecord::from(self))))
        return { };
    PyRecord* exporter = PyRecord::create(globalObject, ioType(globalObject, IOType::BytesIOBuffer), BytesIOExporterFieldCount);
    exporter->set(getVM(globalObject), BytesIOExporterSourceField, self);
    RELEASE_AND_RETURN(scope, makeMemoryView(globalObject, exporter));
}

static JSValue bytesIOGetValue(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    auto& state = bytesIOStateOf(stream);
    if (PyBytes* shared = bytesIOSharedBuffer(stream); shared && shared->length() == static_cast<size_t>(state.size))
        return shared;
    PyBytes* result = ioTryNewBytes(globalObject, state.size);
    if (!result)
        return { };
    memcpy(result->mutableData(), bytesIOData(stream), state.size);
    // Asking again costs nothing, unless something is written in between.
    if (state.size > 1 && !state.exports)
        stream->set(getVM(globalObject), BytesIOBufferField, result);
    return result;
}

static JSValue bytesIOTell(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    RELEASE_AND_RETURN(scope, makeInt(globalObject, bytesIOStateOf(stream).position));
}

static JSValue bytesIORead(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (failed(scope, ioToOptionalSize(globalObject, sizeValue, size)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    auto& state = bytesIOStateOf(stream);
    int64_t available = state.size - state.position;
    if (size < 0 || size > available)
        size = std::max<int64_t>(available, 0);
    RELEASE_AND_RETURN(scope, bytesIOTake(globalObject, stream, size));
}

static JSValue bytesIOReadline(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (failed(scope, ioToOptionalSize(globalObject, sizeValue, size)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    RELEASE_AND_RETURN(scope, bytesIOTake(globalObject, stream, bytesIOScanLine(stream, size)));
}

static JSValue bytesIOReadlines(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    int64_t maxSize = -1;
    if (sizeValue && !isNone(sizeValue)) {
        if (!isInt(sizeValue))
            return raiseTypeError(globalObject, scope, "integer argument expected, got '"_s, typeName(globalObject, sizeValue), '\'');
        if (failed(scope, ssizeArgument(globalObject, sizeValue, maxSize)))
            return { };
    }

    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t total = 0;
    while (int64_t length = bytesIOScanLine(stream, -1)) {
        JSValue line = bytesIOTake(globalObject, stream, length);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, result, line);
        RETURN_IF_EXCEPTION(scope, { });
        total += length;
        if (maxSize > 0 && total >= maxSize)
            break;
    }
    return result;
}

static JSValue bytesIOReadInto(JSGlobalObject* globalObject, JSValue self, JSValue target)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    IOBuffer buffer;
    if (failed(scope, buffer.acquireWritable(globalObject, "readinto"_s, target)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    auto& state = bytesIOStateOf(stream);
    int64_t length = std::min<int64_t>(buffer.size(), state.size - state.position);
    if (length <= 0)
        return jsNumber(0);
    memmove(buffer.mutableSpan().data(), bytesIOData(stream) + state.position, length);
    state.position += length;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, length));
}

static JSValue bytesIOTruncate(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)) || failed(scope, bytesIOCheckExports(globalObject, stream)))
        return { };
    auto& state = bytesIOStateOf(stream);
    int64_t size = state.position;
    if (sizeValue && !isNone(sizeValue)) {
        size = toInt64(globalObject, sizeValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (size < 0)
            return raiseValueError(globalObject, scope, "negative size value "_s, size);
        // Converting the size may have run anything.
        if (failed(scope, bytesIOCheckOpen(globalObject, stream)) || failed(scope, bytesIOCheckExports(globalObject, stream)))
            return { };
    }
    if (size < state.size) {
        int64_t oldSize = std::exchange(state.size, size);
        if (!bytesIOResizeBuffer(globalObject, stream, size)) {
            state.size = oldSize;
            return { };
        }
    }
    RELEASE_AND_RETURN(scope, makeInt(globalObject, size));
}

static JSValue bytesIOSelf(JSGlobalObject*, JSValue self) { return self; }

static JSValue bytesIONext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    int64_t length = bytesIOScanLine(stream, -1);
    if (!length)
        return raiseStopIteration(globalObject, scope);
    RELEASE_AND_RETURN(scope, bytesIOTake(globalObject, stream, length));
}

static JSValue bytesIOSeek(JSGlobalObject* globalObject, JSValue self, JSValue positionValue, JSValue whenceValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t position;
    if (failed(scope, ssizeArgument(globalObject, positionValue, position)))
        return { };
    int whence = 0;
    if (whenceValue && failed(scope, ioToInt(globalObject, whenceValue, whence)))
        return { };
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    auto& state = bytesIOStateOf(stream);

    if (position < 0 && !whence)
        return raiseValueError(globalObject, scope, "negative seek value "_s, position);
    if (whence == 1 || whence == 2) {
        int64_t base = whence == 1 ? state.position : state.size;
        if (position > std::numeric_limits<int64_t>::max() - base)
            return raiseOverflowError(globalObject, scope, "new position too large"_s);
        position += base;
    } else if (whence)
        return raiseValueError(globalObject, scope, "invalid whence ("_s, whence, ", should be 0, 1 or 2)"_s);
    state.position = std::max<int64_t>(position, 0);
    RELEASE_AND_RETURN(scope, makeInt(globalObject, state.position));
}

static JSValue bytesIOWrite(JSGlobalObject* globalObject, JSValue self, JSValue data)
{
    int64_t written = bytesIOWriteBytes(globalObject, PyRecord::from(self), data);
    return written < 0 ? JSValue() : makeInt(globalObject, written);
}

static JSValue bytesIOWritelines(JSGlobalObject* globalObject, JSValue self, JSValue lines)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    forEach(globalObject, lines, [&](JSValue line) {
        return bytesIOWriteBytes(globalObject, stream, line) >= 0;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue bytesIOClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    if (failed(scope, bytesIOCheckExports(globalObject, stream)))
        return { };
    stream->set(getVM(globalObject), BytesIOBufferField, JSValue());
    ioDidClose(globalObject, stream);
    return jsUndefined();
}

// ---- Pickling ----

static JSValue bytesIOGetState(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = bytesIOGetValue(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    PyDict* dict = instanceDict(globalObject, self, false);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { value, makeInt(globalObject, bytesIOStateOf(PyRecord::from(self)).position), dict ? JSValue(dict->copy(globalObject)) : jsUndefined() }));
}

static JSValue bytesIOSetState(JSGlobalObject* globalObject, JSValue self, JSValue stateValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(self);
    // A longer tuple is fine: a later version may have more to say.
    auto* tuple = PyTuple::tryFrom(stateValue);
    if (!tuple || tuple->length() < 3)
        return raiseTypeError(globalObject, scope, typeName(globalObject, self), ".__setstate__ argument should be 3-tuple, got "_s, typeName(globalObject, stateValue));
    if (failed(scope, bytesIOCheckExports(globalObject, stream)))
        return { };

    auto& state = bytesIOStateOf(stream);
    state.size = 0;
    state.position = 0;
    if (bytesIOWriteBytes(globalObject, stream, tuple->at(0)) < 0)
        return { };

    JSValue positionValue = tuple->at(1);
    if (!isInt(positionValue))
        return raiseTypeError(globalObject, scope, "second item of state must be an integer, not "_s, typeName(globalObject, positionValue));
    int64_t position;
    if (failed(scope, ssizeArgument(globalObject, positionValue, position)))
        return { };
    if (position < 0)
        return raiseValueError(globalObject, scope, "position value cannot be negative"_s);
    state.position = position;

    JSValue dict = tuple->at(2);
    if (!isNone(dict)) {
        if (!isDict(dict))
            return raiseTypeError(globalObject, scope, "third item of state should be a dict, got a "_s, typeName(globalObject, dict));
        dictUpdate(globalObject, instanceDict(globalObject, self, true), dict);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue bytesIOSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* stream = PyRecord::from(self);
    size_t size = PyRecord::allocationSize(BytesIOFieldCount) + sizeof(PyByteStorage) + sizeof(BytesIOState);
    if (!bytesIOIsClosed(stream) && !bytesIOSharedBuffer(stream))
        size += sizeof(PyByteStorage) + bytesIOCapacity(stream);
    return makeInt(globalObject, static_cast<uint64_t>(size));
}

static constexpr NativeDef bytesIOMethods[] = {
    PY_NEW(bytesIONew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", bytesIOInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__iter__", bytesIOSelf, "($self, /)"),
    PY_SLOT("__next__", bytesIONext, "($self, /)"),
    PY_METHOD("readable", bytesIOTrueIfOpen, "($self, /)"),
    PY_METHOD("seekable", bytesIOTrueIfOpen, "($self, /)"),
    PY_METHOD("writable", bytesIOTrueIfOpen, "($self, /)"),
    PY_METHOD("close", bytesIOClose, "($self, /)"),
    PY_METHOD("flush", bytesIONoneIfOpen, "($self, /)"),
    PY_METHOD("isatty", bytesIOFalseIfOpen, "($self, /)"),
    PY_METHOD("tell", bytesIOTell, "($self, /)"),
    PY_METHOD("write", bytesIOWrite, "($self, b, /)"),
    PY_METHOD("writelines", bytesIOWritelines, "($self, lines, /)"),
    PY_METHOD("read1", bytesIORead, "($self, size=-1, /)"),
    PY_METHOD("readinto", bytesIOReadInto, "($self, buffer, /)"),
    PY_METHOD("readline", bytesIOReadline, "($self, size=-1, /)"),
    PY_METHOD("readlines", bytesIOReadlines, "($self, size=None, /)"),
    PY_METHOD("read", bytesIORead, "($self, size=-1, /)"),
    PY_METHOD("getbuffer", bytesIOGetBuffer, "($self, /)"),
    PY_METHOD("getvalue", bytesIOGetValue, "($self, /)"),
    PY_METHOD("seek", bytesIOSeek, "($self, pos, whence=0, /)"),
    PY_METHOD("truncate", bytesIOTruncate, "($self, size=None, /)"),
    PY_METHOD("__getstate__", bytesIOGetState, "($self, /)"),
    PY_METHOD("__setstate__", bytesIOSetState, "($self, object, /)"),
    PY_METHOD("__sizeof__", bytesIOSizeOf, "($self, /)"),
};

static constexpr NativeGetSet bytesIOGetSets[] = {
    { "closed"_s, bytesIOGetClosed },
};

constexpr TypeDef ioBytesIOTypeDef { "_io.BytesIO"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, bytesIOMethods, bytesIOGetSets, "Buffered I/O implementation using an in-memory bytes buffer."_s };

// ---- What the memoryview that getbuffer() returns is a view of ----

static void bytesIOExporterRelease(JSGlobalObject*, JSValue exporter)
{
    --bytesIOStateOf(PyRecord::from(PyRecord::from(exporter)->get(BytesIOExporterSourceField))).exports;
}

static JSValue bytesIOExporterBuffer(JSGlobalObject* globalObject, JSValue self, JSValue flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* stream = PyRecord::from(PyRecord::from(self)->get(BytesIOExporterSourceField));
    if (failed(scope, bytesIOCheckOpen(globalObject, stream)))
        return { };
    auto& state = bytesIOStateOf(stream);
    if (bytesIOSharedBuffer(stream) && !bytesIOReallocate(globalObject, stream, state.size))
        return { };
    JSValue view = exportBuffer(globalObject, self, flags, { stream->get(BytesIOBufferField).asCell(), 0, static_cast<size_t>(state.size), false, 1, { }, bytesIOExporterRelease });
    if (view)
        ++state.exports;
    return view;
}

static constexpr NativeDef bytesIOExporterMethods[] = {
    PY_SLOT("__buffer__", bytesIOExporterBuffer, "($self, flags, /)"),
    PY_SLOT("__release_buffer__", releaseBufferMethod, "($self, buffer, /)"),
};

constexpr TypeDef ioBytesIOBufferTypeDef { "_io._BytesIOBuffer"_s, { }, bytesIOExporterMethods, { }, { } };

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
