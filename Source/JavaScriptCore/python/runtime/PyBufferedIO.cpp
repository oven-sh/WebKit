/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyIO.h"

#include "JSCInlines.h"
#include "PyModuleHelpers.h"
#include "PyType.h"
#include <errno.h>
#include <unistd.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// BufferedReader, BufferedWriter, BufferedRandom and BufferedRWPair. (Modules/_io/bufferedio.c in CPython.)
//
// The first three are one implementation, told apart by whether they read, write or both. There is a single buffer, for a
// stretch of the raw stream. All positions below but the absolute one are offsets into it:
//
//   position                    where the next read or write takes place
//   rawPosition                 where the raw stream is
//   0 .. readEnd                what has been read from the raw stream
//   writePosition .. writeEnd   what is still to be written to it
//
// When the raw stream is called, and with what, is what CPython does: programs that provide one of their own can tell.

namespace {

struct BufferedState {
    bool isInitialized { false };
    bool isDetached { false };
    bool readable { false };
    bool writable { false };
    bool finalizing { false };
    ThreadLock lock; // Held while an operation that cannot be interleaved with another is under way.
    bool rawIsFileIO { false }; // A FileIO itself, no subclass: there is no need to go through its methods.
    bool fastClosedChecks { false };
    bool isListedForPendingOutput { false }; // See ioNotePendingOutput().
    int64_t absolutePosition { -1 }; // Of the raw stream. -1: not known.
    int64_t position { 0 };
    int64_t rawPosition { 0 };
    int64_t readEnd { -1 }; // -1: nothing in the buffer has been read.
    int64_t writePosition { 0 };
    int64_t writeEnd { -1 }; // -1: nothing in the buffer is to be written.
    int64_t bufferSize { 0 };
    int64_t bufferMask { 0 }; // bufferSize - 1 if that is a power of two, or 0.
};

enum BufferedField : unsigned {
    BufferedStateField,
    BufferedRawField, // Empty: none yet, or detached.
    BufferedBufferField, // A PyByteStorage. Empty: none yet, or closed.
    BufferedFieldCount,
};

class BufferedStream {
public:
    BufferedStream(JSGlobalObject* globalObject, JSValue self)
        : globalObject(globalObject)
        , vm(getVM(globalObject))
        , record(PyRecord::from(self))
        , s(ioStateOf<BufferedState>(record))
    {
    }

    JSValue raw() const { return record->get(BufferedRawField); }
    PyRecord* rawFile() const { return PyRecord::from(raw()); }
    JSCell* storage() const { return record->get(BufferedBufferField).asCell(); }
    uint8_t* buffer() const
    {
        JSValue storage = record->get(BufferedBufferField);
        return storage ? uncheckedDowncast<PyByteStorage>(storage.asCell())->data() : nullptr;
    }

    // ---- False: an exception ----
    bool checkInitialized();
    bool checkClosed(ASCIILiteral message);
    bool initialize(JSValue raw, int64_t bufferSize, bool readable, bool writable);
    bool flushWrites();
    bool flushAndRewind();

    // 1, 0, or -1 for an exception.
    int isClosed();

    bool hasValidReadBuffer() const { return s.readable && s.readEnd != -1; }
    bool hasValidWriteBuffer() const { return s.writable && s.writeEnd != -1; }
    void resetReadBuffer() { s.readEnd = -1; }
    void resetWriteBuffer()
    {
        s.writePosition = 0;
        s.writeEnd = -1;
    }
    void adjustPosition(int64_t position)
    {
        s.position = position;
        if (hasValidReadBuffer() && s.readEnd < s.position)
            s.readEnd = s.position;
    }
    // How much can be read without asking the raw stream.
    int64_t readAhead() const { return hasValidReadBuffer() ? s.readEnd - s.position : 0; }
    // How far ahead of the position the raw stream is.
    int64_t rawOffset() const { return (hasValidReadBuffer() || hasValidWriteBuffer()) && s.rawPosition >= 0 ? s.rawPosition - s.position : 0; }
    int64_t withoutLastBlock(int64_t size) const { return s.bufferMask ? size & ~s.bufferMask : s.bufferSize * (size / s.bufferSize); }

    // ---- The raw stream. -1: an exception. -2: it would block ----
    int64_t rawTell();
    int64_t cachedRawTell() { return s.absolutePosition != -1 ? s.absolutePosition : rawTell(); }
    int64_t rawSeek(int64_t target, int whence);
    // The memory is at `start`, which is `offset` bytes into that of the cell `owner`.
    int64_t rawRead(JSCell* owner, size_t offset, uint8_t* start, int64_t length);
    int64_t rawWrite(JSCell* owner, size_t offset, const uint8_t* start, int64_t length);
    int64_t fillBuffer();

    // ---- With the lock held, but for readFast() ----
    JSValue readAll();
    // None if the buffer does not hold that much.
    JSValue readFast(int64_t size);
    JSValue readGeneric(int64_t size);
    JSValue peek();
    JSValue readline(int64_t limit);
    JSValue readInto(JSValue target, bool isReadInto1);

    void raiseBlocking(int64_t written);
    int64_t invalidPosition(int64_t position);

    JSGlobalObject* globalObject;
    VM& vm;
    PyRecord* record;
    BufferedState& s;
    int lastErrno { 0 };
};

// Other threads wait. The raw stream may call back into the stream that is calling it: that is an error.
class BufferedLock {
    WTF_MAKE_NONCOPYABLE(BufferedLock);
public:
    explicit BufferedLock(BufferedStream& stream)
        : m_stream(stream)
    {
        acquire();
    }
    ~BufferedLock() { release(); }

    // False: a RuntimeError.
    explicit operator bool() const { return m_isHeld; }

    bool acquire()
    {
        if (m_stream.s.lock.isHeldByThisThread(m_stream.globalObject)) {
            auto scope = DECLARE_THROW_SCOPE(m_stream.vm);
            raiseRuntimeError(m_stream.globalObject, scope, "reentrant call inside "_s, reprForMessage(m_stream.globalObject, m_stream.record));
            return false;
        }
        m_isHeld = m_stream.s.lock.acquire(m_stream.globalObject);
        return m_isHeld;
    }
    void release()
    {
        if (m_isHeld)
            m_stream.s.lock.release(m_stream.globalObject);
        m_isHeld = false;
    }

private:
    BufferedStream& m_stream;
    bool m_isHeld { false };
};

} // namespace

static void bufferedNotePendingOutput(BufferedStream& stream)
{
    auto& s = stream.s;
    if (s.isListedForPendingOutput || !s.rawIsFileIO)
        return;
    PyType* type = stream.record->type();
    if (type != ioType(stream.globalObject, IOType::BufferedWriter) && type != ioType(stream.globalObject, IOType::BufferedRandom))
        return;
    s.isListedForPendingOutput = true;
    ioNotePendingOutput(stream.globalObject, stream.record);
}

void bufferedWriteOutPending(JSGlobalObject* globalObject, PyRecord* record)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    BufferedStream stream(globalObject, record);
    auto& s = stream.s;
    s.isListedForPendingOutput = false;
    // Whoever is in the middle of something with it will get to that.
    if (!s.isInitialized || s.isDetached || s.lock.isHeld() || !bufferedHasPendingOutput(record) || stream.isClosed())
        return;
    BufferedLock lock(stream);
    if (failed(scope, !!lock) || failed(scope, stream.flushAndRewind()))
        reportUnraisable(globalObject, makeString("Exception ignored while flushing "_s, reprForMessage(globalObject, record)), { });
}

bool bufferedHasPendingOutput(PyRecord* buffer)
{
    auto& state = ioStateOf<BufferedState>(buffer);
    return state.writable && state.writeEnd != -1 && state.writePosition < state.writeEnd;
}

bool bufferedIsBusy(PyRecord* buffer) { return ioStateOf<BufferedState>(buffer).lock.isHeld(); }

JSValue bufferedWrappedStream(PyRecord* stream) { return stream->get(BufferedRawField); }

bool BufferedStream::checkInitialized()
{
    if (s.isInitialized) [[likely]]
        return true;
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, s.isDetached ? "raw stream has been detached"_s : "I/O operation on uninitialized object"_s);
    return false;
}

int BufferedStream::isClosed()
{
    if (!record->get(BufferedBufferField))
        return 1;
    if (s.fastClosedChecks)
        return fileIOIsClosed(rawFile());
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkInitialized()))
        return -1;
    JSValue closed = getAttr(globalObject, raw(), ioName(globalObject, IOName::closed));
    RETURN_IF_EXCEPTION(scope, -1);
    bool result = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, -1);
    return result;
}

// What is in the buffer of a stream whose raw stream was closed behind its back can still be read.
bool BufferedStream::checkClosed(ASCIILiteral message)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    int closed = isClosed();
    if (failed(scope, closed >= 0))
        return false;
    if (closed && !readAhead()) {
        raiseValueError(globalObject, scope, message);
        return false;
    }
    return true;
}

void BufferedStream::raiseBlocking(int64_t written)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    takePendingException(globalObject);
    raiseWithArgs(globalObject, scope, runtime(globalObject).BlockingIOError(), { jsNumber(lastErrno), makeStr(vm, "write could not complete without blocking"_s), makeInt(globalObject, written) });
}

int64_t BufferedStream::invalidPosition(int64_t position)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    RETURN_IF_EXCEPTION(scope, -1);
    raise(globalObject, scope, runtime(globalObject).OSError(), makeString("Raw stream returned invalid position "_s, position));
    return -1;
}

int64_t BufferedStream::rawTell()
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t position;
    if (s.rawIsFileIO) {
        position = fileIOSeek(globalObject, rawFile(), 0, SEEK_CUR);
        if (failed(scope, position >= 0))
            return -1;
    } else {
        JSValue result = callMethod(globalObject, raw(), ioName(globalObject, IOName::tell));
        RETURN_IF_EXCEPTION(scope, -1);
        position = ioToOffset(globalObject, result, runtime(globalObject).ValueError());
        RETURN_IF_EXCEPTION(scope, -1);
        if (position < 0)
            RELEASE_AND_RETURN(scope, invalidPosition(position));
    }
    s.absolutePosition = position;
    return position;
}

int64_t BufferedStream::rawSeek(int64_t target, int whence)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t position;
    if (s.rawIsFileIO) {
        position = fileIOSeek(globalObject, rawFile(), target, whence);
        if (failed(scope, position >= 0))
            return -1;
    } else {
        JSValue result = callMethod(globalObject, raw(), ioName(globalObject, IOName::seek), makeInt(globalObject, target), jsNumber(whence));
        RETURN_IF_EXCEPTION(scope, -1);
        position = ioToOffset(globalObject, result, runtime(globalObject).ValueError());
        RETURN_IF_EXCEPTION(scope, -1);
        if (position < 0)
            RELEASE_AND_RETURN(scope, invalidPosition(position));
    }
    s.absolutePosition = position;
    return position;
}

bool BufferedStream::initialize(JSValue raw, int64_t bufferSize, bool readable, bool writable)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (s.lock.isHeld()) {
        raiseRuntimeError(globalObject, scope, "reentrant call inside "_s, reprForMessage(globalObject, record));
        return false;
    }
    s.isInitialized = false;
    s.isDetached = false;

    if (readable && writable && failed(scope, ioCheckSeekable(globalObject, raw)))
        return false;
    if (readable && failed(scope, ioCheckReadable(globalObject, raw)))
        return false;
    if (writable && failed(scope, ioCheckWritable(globalObject, raw)))
        return false;

    if (JSValue previous = this->raw(); previous && previous != raw)
        ioDidUnwrap(globalObject, previous);
    record->set(vm, BufferedRawField, raw);
    s.rawIsFileIO = !!ioTryExact(globalObject, raw, IOType::FileIO);
    s.fastClosedChecks = false;
    s.bufferSize = bufferSize;
    s.readable = readable;
    s.writable = writable;

    if (bufferSize <= 0) {
        raiseValueError(globalObject, scope, "buffer size must be strictly positive"_s);
        return false;
    }
    PyByteStorage* storage = ioTryNewStorage(globalObject, bufferSize);
    RETURN_IF_EXCEPTION(scope, false);
    record->set(vm, BufferedBufferField, storage);
    s.bufferMask = !(bufferSize & (bufferSize - 1)) ? bufferSize - 1 : 0;
    if (failed(scope, rawTell() != -1)) {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }

    if (readable)
        resetReadBuffer();
    if (writable) {
        resetWriteBuffer();
        s.position = 0;
    }
    s.fastClosedChecks = s.rawIsFileIO && !record->type()->isHeapType();
    s.isInitialized = true;
    ioDidOpen(globalObject, record);
    ioDidWrap(globalObject, raw);
    return true;
}

// ---- Reading ----

int64_t BufferedStream::rawRead(JSCell* owner, size_t offset, uint8_t* start, int64_t length)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count;
    if (s.rawIsFileIO) {
        count = fileIOReadInto(globalObject, rawFile(), { start, static_cast<size_t>(length) });
        EXCEPTION_ASSERT(!!scope.exception() == (count == -1));
        if (count < 0)
            return count;
    } else {
        PyMemoryView* target = PyMemoryView::createForMemory(globalObject, owner, offset, length, false);
        RETURN_IF_EXCEPTION(scope, -1);
        JSValue result;
        do {
            result = callMethod(globalObject, raw(), ioName(globalObject, IOName::readinto), target);
        } while (failed(scope, result) && ioTrapEINTR(globalObject));
        if (!result)
            return -1;
        if (isNone(result))
            return -2;
        count = toSsize(globalObject, result, rt.ValueError());
        if (scope.exception()) [[unlikely]] {
            JSValue cause = takePendingException(globalObject);
            RETURN_IF_EXCEPTION(scope, -1);
            PyException* error = newException(globalObject, rt.OSError(), "raw readinto() failed"_s);
            RETURN_IF_EXCEPTION(scope, -1);
            error->setCause(vm, cause);
            error->setContext(vm, cause);
            error->setSuppressContext(true);
            scope.release();
            ioRestoreException(globalObject, error);
            return -1;
        }
        if (count < 0 || count > length) {
            raise(globalObject, scope, rt.OSError(), makeString("raw readinto() returned invalid length "_s, count, " (should have been between 0 and "_s, length, ')'));
            return -1;
        }
    }
    if (count > 0 && s.absolutePosition != -1)
        s.absolutePosition += count;
    return count;
}

int64_t BufferedStream::fillBuffer()
{
    int64_t start = hasValidReadBuffer() ? s.readEnd : 0;
    int64_t count = rawRead(storage(), start, buffer() + start, s.bufferSize - start);
    if (count <= 0)
        return count;
    s.readEnd = start + count;
    s.rawPosition = start + count;
    return count;
}

JSValue BufferedStream::readAll()
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    // First what the buffer holds.
    int64_t currentSize = readAhead();
    PyBytes* buffered = nullptr;
    if (currentSize) {
        buffered = PyBytes::create(globalObject, std::span<const uint8_t> { buffer() + s.position, static_cast<size_t>(currentSize) });
        s.position += currentSize;
    }
    if (s.writable && failed(scope, flushAndRewind()))
        return { };
    resetReadBuffer();

    auto concatenate = [&](PyBytes* first, PyBytes* second) -> JSValue {
        PyBytes* result = ioTryNewBytes(globalObject, first->length() + second->length());
        if (!result)
            return { };
        memcpy(result->mutableData(), first->data(), first->length());
        memcpy(result->mutableData() + first->length(), second->data(), second->length());
        return result;
    };

    JSValue readall = getAttrIfPresent(globalObject, raw(), ioName(globalObject, IOName::readall));
    RETURN_IF_EXCEPTION(scope, { });
    if (readall) {
        JSValue rest = call(globalObject, readall);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(rest) && !PyBytes::is(rest))
            return raiseTypeError(globalObject, scope, "readall() should return bytes"_s);
        if (!buffered)
            return rest;
        if (isNone(rest))
            return buffered;
        RELEASE_AND_RETURN(scope, concatenate(buffered, PyBytes::from(rest)));
    }

    Vector<uint8_t> all;
    if (buffered)
        all.append(buffered->span());
    while (true) {
        JSValue data = callMethod(globalObject, raw(), rt.names.read());
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(data) && !PyBytes::is(data))
            return raiseTypeError(globalObject, scope, "read() should return bytes"_s);
        if (isNone(data) || !PyBytes::from(data)->length()) {
            if (!currentSize)
                return data;
            RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, all.span()));
        }
        auto* bytes = PyBytes::from(data);
        all.append(bytes->span());
        currentSize += bytes->length();
        if (s.absolutePosition != -1)
            s.absolutePosition += bytes->length();
    }
}

JSValue BufferedStream::readFast(int64_t size)
{
    if (size > readAhead())
        return jsUndefined();
    JSValue result = PyBytes::create(globalObject, std::span<const uint8_t> { buffer() + s.position, static_cast<size_t>(size) });
    s.position += size;
    return result;
}

JSValue BufferedStream::readGeneric(int64_t size)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t currentSize = readAhead();
    if (size <= currentSize)
        return readFast(size);

    PyBytes* result = ioTryNewBytes(globalObject, size);
    RETURN_IF_EXCEPTION(scope, { });
    uint8_t* out = result->mutableData();
    int64_t remaining = size;
    int64_t written = 0;
    auto finish = [&]() -> JSValue {
        if (written == size)
            return result;
        return PyBytes::create(globalObject, result->span().first(written));
    };
    // At the end of the stream, or where it would block.
    auto stopShort = [&](int64_t count) -> JSValue {
        if (!count || written > 0)
            return finish();
        return jsUndefined();
    };

    if (currentSize > 0) {
        memcpy(out, buffer() + s.position, currentSize);
        remaining -= currentSize;
        written += currentSize;
        s.position += currentSize;
    }
    if (s.writable && failed(scope, flushAndRewind()))
        return { };
    resetReadBuffer();

    // Whole blocks go straight to the result. The last, which may be read beyond what was asked for, goes through the buffer.
    while (remaining > 0) {
        int64_t count = withoutLastBlock(remaining);
        if (!count)
            break;
        count = rawRead(result, written, out + written, count);
        if (failed(scope, count != -1))
            return { };
        if (!count || count == -2)
            return stopShort(count);
        remaining -= count;
        written += count;
    }
    s.position = 0;
    s.rawPosition = 0;
    s.readEnd = 0;
    // Once there is enough, nothing more is read: that might wait forever.
    while (remaining > 0 && s.readEnd < s.bufferSize) {
        int64_t count = fillBuffer();
        if (failed(scope, count != -1))
            return { };
        if (!count || count == -2)
            return stopShort(count);
        int64_t taken = std::min(remaining, count);
        memcpy(out + written, buffer() + s.position, taken);
        written += taken;
        s.position += taken;
        remaining -= taken;
    }
    return finish();
}

// What the buffer holds if it holds anything, without moving it so as to stay aligned on blocks; otherwise a buffer full.
JSValue BufferedStream::peek()
{
    int64_t have = readAhead();
    if (have > 0)
        return PyBytes::create(globalObject, std::span<const uint8_t> { buffer() + s.position, static_cast<size_t>(have) });
    resetReadBuffer();
    int64_t count = fillBuffer();
    if (count == -1)
        return { };
    if (count == -2)
        count = 0;
    s.position = 0;
    return PyBytes::create(globalObject, std::span<const uint8_t> { buffer(), static_cast<size_t>(count) });
}

JSValue BufferedStream::readInto(JSValue target, bool isReadInto1)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    IOBuffer destination;
    if (failed(scope, destination.acquireWritable(globalObject, isReadInto1 ? "readinto1"_s : "readinto"_s, target)))
        return { };
    if (failed(scope, checkInitialized()) || failed(scope, checkClosed("readinto of closed file"_s)))
        return { };

    int64_t length = destination.size();
    uint8_t* out = destination.mutableData();
    int64_t written = 0;
    int64_t count = readAhead();
    if (count > 0) {
        if (count >= length) {
            if (length)
                memcpy(out, buffer() + s.position, length);
            s.position += length;
            RELEASE_AND_RETURN(scope, makeInt(globalObject, length));
        }
        memcpy(out, buffer() + s.position, count);
        s.position += count;
        written = count;
    }

    BufferedLock lock(*this);
    if (failed(scope, !!lock))
        return { };
    if (s.writable && failed(scope, flushAndRewind()))
        return { };
    resetReadBuffer();
    s.position = 0;

    for (int64_t remaining = length - written; remaining > 0; written += count, remaining -= count) {
        if (failed(scope, destination.refresh()))
            return { };
        out = destination.mutableData();
        if (remaining > s.bufferSize) {
            // More than the buffer could take anyway.
            count = rawRead(destination.owner(), destination.offset() + written, out + written, remaining);
            EXCEPTION_ASSERT(!!scope.exception() == (count == -1));
        } else if (!(isReadInto1 && written)) {
            count = fillBuffer();
            EXCEPTION_ASSERT(!!scope.exception() == (count == -1));
            if (count > 0) {
                if (failed(scope, destination.refresh()))
                    return { };
                out = destination.mutableData();
                count = std::min(count, remaining);
                memcpy(out + written, buffer() + s.position, count);
                s.position += count;
                continue;
            }
        } else
            count = 0;
        if (!count || (count == -2 && written > 0))
            break;
        if (count < 0)
            return count == -2 ? jsUndefined() : JSValue();
        // readinto1() asks the raw stream once at most.
        if (isReadInto1) {
            written += count;
            break;
        }
    }
    RELEASE_AND_RETURN(scope, makeInt(globalObject, written));
}

JSValue BufferedStream::readline(int64_t limit)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkClosed("readline of closed file"_s)))
        return { };

    // The line may be in the buffer already.
    int64_t count = readAhead();
    if (limit >= 0 && count > limit)
        count = limit;
    const uint8_t* start = buffer() + s.position;
    if (auto* newline = count ? static_cast<const uint8_t*>(memchr(start, '\n', count)) : nullptr)
        count = newline - start + 1;
    else if (count != limit)
        start = nullptr;
    if (start) {
        JSValue result = PyBytes::create(globalObject, std::span<const uint8_t> { start, static_cast<size_t>(count) });
        s.position += count;
        return result;
    }

    BufferedLock lock(*this);
    if (failed(scope, !!lock))
        return { };
    Vector<uint8_t, 256> line;
    if (count > 0) {
        line.append(std::span<const uint8_t> { buffer() + s.position, static_cast<size_t>(count) });
        s.position += count;
        if (limit >= 0)
            limit -= count;
    }
    if (s.writable && failed(scope, flushAndRewind()))
        return { };

    while (true) {
        resetReadBuffer();
        count = fillBuffer();
        if (failed(scope, count != -1))
            return { };
        if (count <= 0)
            break;
        if (limit >= 0 && count > limit)
            count = limit;
        start = buffer();
        if (auto* newline = static_cast<const uint8_t*>(memchr(start, '\n', count))) {
            count = newline - start + 1;
            line.append(std::span<const uint8_t> { start, static_cast<size_t>(count) });
            s.position = count;
            break;
        }
        line.append(std::span<const uint8_t> { start, static_cast<size_t>(count) });
        if (count == limit) {
            s.position = count;
            break;
        }
        if (limit >= 0)
            limit -= count;
    }
    return PyBytes::create(globalObject, line.span());
}

// ---- Writing ----

int64_t BufferedStream::rawWrite(JSCell* owner, size_t offset, const uint8_t* start, int64_t length)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count;
    if (s.rawIsFileIO) {
        count = fileIOWrite(globalObject, rawFile(), { start, static_cast<size_t>(length) });
        EXCEPTION_ASSERT(!!scope.exception() == (count == -1));
        if (count == -2)
            lastErrno = EAGAIN;
        if (count < 0)
            return count;
    } else {
        PyMemoryView* data = PyMemoryView::createForMemory(globalObject, owner, offset, length, true);
        RETURN_IF_EXCEPTION(scope, -1);
        JSValue result;
        do {
            errno = 0;
            result = callMethod(globalObject, raw(), rt.names.write(), data);
            lastErrno = errno;
        } while (failed(scope, result) && ioTrapEINTR(globalObject));
        if (!result)
            return -1;
        if (isNone(result))
            return -2;
        count = toSsize(globalObject, result, rt.ValueError());
        if (scope.exception()) [[unlikely]] {
            takePendingException(globalObject);
            RETURN_IF_EXCEPTION(scope, -1);
            count = -1;
        }
        if (count < 0 || count > length) {
            raise(globalObject, scope, rt.OSError(), makeString("raw write() returned invalid length "_s, count, " (should have been between 0 and "_s, length, ')'));
            return -1;
        }
    }
    if (count > 0 && s.absolutePosition != -1)
        s.absolutePosition += count;
    return count;
}

bool BufferedStream::flushWrites()
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasValidWriteBuffer() && s.writePosition != s.writeEnd) {
        // The raw stream goes back to where what is to be written belongs.
        int64_t rewind = rawOffset() + (s.position - s.writePosition);
        if (rewind) {
            if (failed(scope, rawSeek(-rewind, SEEK_CUR) >= 0))
                return false;
            s.rawPosition -= rewind;
        }
        while (s.writePosition < s.writeEnd) {
            int64_t count = rawWrite(storage(), s.writePosition, buffer() + s.writePosition, s.writeEnd - s.writePosition);
            if (failed(scope, count != -1))
                return false;
            if (count == -2) {
                scope.release();
                raiseBlocking(0);
                return false;
            }
            s.writePosition += count;
            s.rawPosition = s.writePosition;
            // A write that a signal cuts short succeeds. The handler has to run before the next one blocks, for who knows how long.
            if (failed(scope, checkOnlySignals(globalObject)))
                return false;
        }
    }
    // So that rawOffset() is 0 if there is no valid read buffer either.
    resetWriteBuffer();
    return true;
}

bool BufferedStream::flushAndRewind()
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, flushWrites()))
        return false;
    if (s.readable) {
        // The raw stream goes back to where the program thinks it is.
        int64_t position = rawSeek(-rawOffset(), SEEK_CUR);
        resetReadBuffer();
        if (failed(scope, position != -1))
            return false;
    }
    return true;
}

// ---- Methods ----

static JSValue bufferedNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    PyRecord* stream = ioAllocate<BufferedState>(globalObject, cls, BufferedFieldCount);
    ioDidAllocate(globalObject, stream);
    return stream;
}

static JSValue bufferedInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args, ASCIILiteral name, bool readable, bool writable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "raw"_s, "buffer_size"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, name, keywords, 1, 2, false, args, values)))
        return { };
    int64_t bufferSize = ioDefaultBufferSize;
    if (values[1] && failed(scope, ssizeArgument(globalObject, values[1], bufferSize)))
        return { };
    RELEASE_AND_RETURN(scope, BufferedStream(globalObject, self).initialize(values[0], bufferSize, readable, writable) ? jsUndefined() : JSValue());
}

static JSValue bufferedReaderInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) { return bufferedInit(globalObject, self, args, "BufferedReader"_s, true, false); }
static JSValue bufferedWriterInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) { return bufferedInit(globalObject, self, args, "BufferedWriter"_s, false, true); }
static JSValue bufferedRandomInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) { return bufferedInit(globalObject, self, args, "BufferedRandom"_s, true, true); }

static JSValue bufferedSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    BufferedStream stream(globalObject, self);
    size_t size = PyRecord::allocationSize(BufferedFieldCount) + 2 * sizeof(PyByteStorage) + sizeof(BufferedState);
    if (stream.buffer())
        size += stream.s.bufferSize;
    return makeInt(globalObject, static_cast<uint64_t>(size));
}

static JSValue bufferedDeallocWarn(JSGlobalObject* globalObject, JSValue self, JSValue source)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(getVM(globalObject));
    BufferedStream stream(globalObject, self);
    if (stream.s.isInitialized && stream.raw()) {
        if (failed(scope, callMethod(globalObject, stream.raw(), ioName(globalObject, IOName::deallocWarn), source)))
            takePendingException(globalObject);
    }
    return jsUndefined();
}

// raw.method()
static JSValue bufferedForward(JSGlobalObject* globalObject, JSValue self, JSString* method)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    RELEASE_AND_RETURN(scope, callMethod(globalObject, stream.raw(), method));
}

// raw.attribute
static JSValue bufferedForwardGet(JSGlobalObject* globalObject, JSValue self, JSString* attribute)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    RELEASE_AND_RETURN(scope, getAttr(globalObject, stream.raw(), attribute));
}

static JSValue bufferedSimpleFlush(JSGlobalObject* globalObject, JSValue self) { return bufferedForward(globalObject, self, runtime(globalObject).names.flush()); }
static JSValue bufferedSeekable(JSGlobalObject* globalObject, JSValue self) { return bufferedForward(globalObject, self, ioName(globalObject, IOName::seekable)); }
static JSValue bufferedReadable(JSGlobalObject* globalObject, JSValue self) { return bufferedForward(globalObject, self, ioName(globalObject, IOName::readable)); }
static JSValue bufferedWritable(JSGlobalObject* globalObject, JSValue self) { return bufferedForward(globalObject, self, ioName(globalObject, IOName::writable)); }
static JSValue bufferedFileno(JSGlobalObject* globalObject, JSValue self) { return bufferedForward(globalObject, self, ioName(globalObject, IOName::fileno)); }
static JSValue bufferedIsATTY(JSGlobalObject* globalObject, JSValue self) { return bufferedForward(globalObject, self, ioName(globalObject, IOName::isatty)); }
static JSValue bufferedGetClosed(JSGlobalObject* globalObject, JSValue self) { return bufferedForwardGet(globalObject, self, ioName(globalObject, IOName::closed)); }
static JSValue bufferedGetName(JSGlobalObject* globalObject, JSValue self) { return bufferedForwardGet(globalObject, self, runtime(globalObject).names.name()); }
static JSValue bufferedGetMode(JSGlobalObject* globalObject, JSValue self) { return bufferedForwardGet(globalObject, self, ioName(globalObject, IOName::mode)); }

static JSValue bufferedGetRaw(JSGlobalObject*, JSValue self)
{
    JSValue raw = PyRecord::from(self)->get(BufferedRawField);
    return raw ? raw : jsUndefined();
}

static JSValue bufferedGetFinalizing(JSGlobalObject*, JSValue self) { return jsBoolean(ioStateOf<BufferedState>(PyRecord::from(self)).finalizing); }
static void bufferedSetFinalizing(JSGlobalObject* globalObject, JSValue self, JSValue value) { ioToFinalizing(globalObject, value, ioStateOf<BufferedState>(PyRecord::from(self)).finalizing); }

static JSValue bufferedClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    int closed = stream.isClosed();
    if (failed(scope, closed >= 0))
        return { };
    if (closed)
        return jsUndefined();

    if (stream.s.finalizing)
        bufferedDeallocWarn(globalObject, self, self);

    // flush() takes the lock itself.
    lock.release();
    JSValue flushError;
    if (failed(scope, ioFlush(globalObject, self))) {
        flushError = takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (failed(scope, lock.acquire()))
        return { };

    JSValue result;
    if (stream.raw())
        result = callMethod(globalObject, stream.raw(), runtime(globalObject).names.close());
    else
        stream.checkInitialized();
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    stream.record->set(stream.vm, BufferedBufferField, JSValue());
    ioDidClose(globalObject, stream.record);
    scope.release();
    if (flushError) {
        ioChainExceptions(globalObject, flushError);
        result = JSValue();
    }
    stream.s.readEnd = 0;
    stream.s.position = 0;
    return result;
}

static JSValue bufferedDetach(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    if (failed(scope, ioFlush(globalObject, self)))
        return { };
    // Flushing may have run anything.
    if (failed(scope, stream.checkInitialized()))
        return { };
    JSValue raw = stream.raw();
    stream.record->set(stream.vm, BufferedRawField, JSValue());
    stream.s.isDetached = true;
    stream.s.isInitialized = false;
    ioDidClose(globalObject, stream.record);
    ioDidUnwrap(globalObject, raw);
    return raw;
}

static JSValue bufferedFlush(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()) || failed(scope, stream.checkClosed("flush of closed file"_s)))
        return { };
    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    return failed(scope, stream.flushAndRewind()) ? JSValue() : jsUndefined();
}

static JSValue bufferedPeek(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = 0;
    if (sizeValue && failed(scope, ssizeArgument(globalObject, sizeValue, size)))
        return { };
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()) || failed(scope, stream.checkClosed("peek of closed file"_s)))
        return { };
    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    if (stream.s.writable && failed(scope, stream.flushAndRewind()))
        return { };
    RELEASE_AND_RETURN(scope, stream.peek());
}

static JSValue bufferedRead(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (failed(scope, ioToOptionalSize(globalObject, sizeValue, size)))
        return { };
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    if (size < -1)
        return raiseValueError(globalObject, scope, "read length must be non-negative or -1"_s);
    if (failed(scope, stream.checkClosed("read of closed file"_s)))
        return { };

    if (size != -1) {
        JSValue result = stream.readFast(size);
        if (!isNone(result))
            return result;
    }
    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    RELEASE_AND_RETURN(scope, size == -1 ? stream.readAll() : stream.readGeneric(size));
}

static JSValue bufferedRead1(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (sizeValue && failed(scope, ssizeArgument(globalObject, sizeValue, size)))
        return { };
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    if (size < 0)
        size = stream.s.bufferSize;
    if (failed(scope, stream.checkClosed("read of closed file"_s)))
        return { };
    if (!size)
        RELEASE_AND_RETURN(scope, runtime(globalObject).emptyBytes());

    // What the buffer holds, if it holds anything. Otherwise the raw stream is asked once.
    if (int64_t have = stream.readAhead(); have > 0)
        return stream.readFast(std::min(have, size));

    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    if (stream.s.writable && failed(scope, stream.flushAndRewind()))
        return { };
    stream.resetReadBuffer();

    PyBytes* result = ioTryNewBytes(globalObject, size);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count = stream.rawRead(result, 0, result->mutableData(), size);
    if (failed(scope, count != -1))
        return { };
    if (count == -2)
        count = 0;
    if (count == size)
        return result;
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, result->span().first(count)));
}

static JSValue bufferedReadInto(JSGlobalObject* globalObject, JSValue self, JSValue target) { return BufferedStream(globalObject, self).readInto(target, false); }
static JSValue bufferedReadInto1(JSGlobalObject* globalObject, JSValue self, JSValue target) { return BufferedStream(globalObject, self).readInto(target, true); }

static JSValue bufferedReadline(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (failed(scope, ioToOptionalSize(globalObject, sizeValue, size)))
        return { };
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    RELEASE_AND_RETURN(scope, stream.readline(size));
}

static JSValue bufferedTell(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };
    int64_t position = stream.rawTell();
    if (failed(scope, position != -1))
        return { };
    RELEASE_AND_RETURN(scope, makeInt(globalObject, std::max<int64_t>(position - stream.rawOffset(), 0)));
}

static JSValue bufferedSeek(JSGlobalObject* globalObject, JSValue self, JSValue targetValue, JSValue whenceValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int whence = 0;
    if (whenceValue && failed(scope, ioToInt(globalObject, whenceValue, whence)))
        return { };
    BufferedStream stream(globalObject, self);
    auto& s = stream.s;
    if (failed(scope, stream.checkInitialized()))
        return { };

    bool isKnownWhence = whence >= 0 && whence <= 2;
#ifdef SEEK_HOLE
    isKnownWhence |= whence == SEEK_HOLE;
#endif
#ifdef SEEK_DATA
    isKnownWhence |= whence == SEEK_DATA;
#endif
    if (!isKnownWhence)
        return raiseValueError(globalObject, scope, "whence value "_s, whence, " unsupported"_s);

    if (failed(scope, stream.checkClosed("seek of closed file"_s)))
        return { };
    if (failed(scope, ioCheckSeekable(globalObject, stream.raw())))
        return { };
    int64_t target = ioToOffset(globalObject, targetValue, runtime(globalObject).ValueError());
    RETURN_IF_EXCEPTION(scope, { });

    // The target may be within the buffer.
    if (whence <= 1 && s.readable) {
        int64_t current = stream.cachedRawTell();
        int64_t available = stream.readAhead();
        if (failed(scope, current != -1)) {
            // Whatever is wrong with the raw stream comes up again below.
            takePendingException(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
        } else if (available > 0) {
            int64_t offset = !whence ? target - (current - stream.rawOffset()) : target;
            if (offset >= -s.position && offset <= available) {
                s.position += offset;
                RELEASE_AND_RETURN(scope, makeInt(globalObject, std::max<int64_t>(current - available + offset, 0)));
            }
        }
    }

    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    if (s.writable && failed(scope, stream.flushWrites()))
        return { };
    if (whence == 1)
        target -= stream.rawOffset();
    int64_t position = stream.rawSeek(target, whence);
    if (failed(scope, position != -1))
        return { };
    s.rawPosition = -1;
    if (s.readable)
        stream.resetReadBuffer();
    RELEASE_AND_RETURN(scope, makeInt(globalObject, position));
}

static JSValue bufferedTruncate(JSGlobalObject* globalObject, JSValue self, JSValue position)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()) || failed(scope, stream.checkClosed("truncate of closed file"_s)))
        return { };
    if (!stream.s.writable)
        return ioRaiseUnsupported(globalObject, scope, "truncate"_s);
    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    if (failed(scope, stream.flushAndRewind()))
        return { };
    JSValue result = callMethod(globalObject, stream.raw(), ioName(globalObject, IOName::truncate), position ? position : jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    // The raw stream may have moved.
    if (failed(scope, stream.rawTell() != -1)) {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue bufferedNext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    BufferedStream stream(globalObject, self);
    if (failed(scope, stream.checkInitialized()))
        return { };

    JSValue line;
    if (!stream.record->type()->isHeapType())
        line = stream.readline(-1);
    else {
        line = callMethod(globalObject, self, rt.names.readline());
        if (!failed(scope, line) && !PyBytes::is(line))
            return raise(globalObject, scope, rt.OSError(), makeString("readline() should have returned a bytes object, not '"_s, typeName(globalObject, line), '\''));
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!PyBytes::from(line)->length())
        return raiseStopIteration(globalObject, scope);
    return line;
}

static JSValue bufferedRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String ownName = typeName(globalObject, self);
    JSValue name = getAttrIfPresent(globalObject, self, rt.names.name());
    if (scope.exception()) [[unlikely]] {
        // The raw stream has been detached.
        if (!clearPendingExceptionIfMatches(globalObject, rt.ValueError()))
            return { };
        name = JSValue();
    }
    if (!name)
        return makeStr(vm, makeString('<', ownName, '>'));

    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return raiseRuntimeError(globalObject, scope, "reentrant call inside "_s, ownName, ".__repr__"_s);
    JSString* nameRepr = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    String nameText = nameRepr->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString('<', ownName, " name="_s, nameText, '>'));
}

static JSValue bufferedWrite(JSGlobalObject* globalObject, JSValue self, JSValue dataValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    IOBuffer data;
    if (failed(scope, data.acquire(globalObject, dataValue)))
        return { };
    BufferedStream stream(globalObject, self);
    auto& s = stream.s;
    if (failed(scope, stream.checkInitialized()))
        return { };
    BufferedLock lock(stream);
    if (failed(scope, !!lock))
        return { };
    int closed = stream.isClosed();
    if (failed(scope, closed >= 0))
        return { };
    if (closed)
        return raiseValueError(globalObject, scope, "write to closed file"_s);

    int64_t length = data.size();
    const uint8_t* source = data.data();

    // The buffer may have room for all of it.
    if (!stream.hasValidReadBuffer() && !stream.hasValidWriteBuffer()) {
        s.position = 0;
        s.rawPosition = 0;
    }
    int64_t available = s.bufferSize - s.position;
    if (length <= available && length < s.bufferSize) {
        if (length)
            memcpy(stream.buffer() + s.position, source, length);
        if (!stream.hasValidWriteBuffer() || s.writePosition > s.position)
            s.writePosition = s.position;
        stream.adjustPosition(s.position + length);
        s.writeEnd = std::max(s.writeEnd, s.position);
        bufferedNotePendingOutput(stream);
        RELEASE_AND_RETURN(scope, makeInt(globalObject, length));
    }

    bool flushed = !failed(scope, stream.flushWrites());
    {
        // That may have run anything.
        JSValue error = takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (failed(scope, data.refresh()))
            return { };
        source = data.data();
        if (error)
            ioRestoreException(globalObject, error);
        EXCEPTION_ASSERT(!!scope.exception() == !flushed);
    }
    if (!flushed) {
        if (!pendingExceptionMatches(globalObject, runtime(globalObject).BlockingIOError()))
            return { };
        if (s.readable)
            stream.resetReadBuffer();
        // What could not be written moves to the start of the buffer, to make room.
        uint8_t* buffer = stream.buffer();
        memmove(buffer, buffer + s.writePosition, s.writeEnd - s.writePosition);
        s.writeEnd -= s.writePosition;
        s.rawPosition -= s.writePosition;
        s.position -= s.writePosition;
        s.writePosition = 0;
        available = s.bufferSize - s.writeEnd;
        if (length <= available) {
            takePendingException(globalObject);
            memcpy(buffer + s.writeEnd, source, length);
            s.writeEnd += length;
            s.position += length;
            RELEASE_AND_RETURN(scope, makeInt(globalObject, length));
        }
        memcpy(buffer + s.writeEnd, source, available);
        s.writeEnd += available;
        s.position += available;
        scope.release();
        stream.raiseBlocking(available);
        return { };
    }

    // If the buffer had been read into and not written to, flushing has not taken the raw stream back to the position.
    if (int64_t offset = stream.rawOffset()) {
        if (failed(scope, stream.rawSeek(-offset, SEEK_CUR) >= 0))
            return { };
        s.rawPosition -= offset;
        if (failed(scope, data.refresh()))
            return { };
        source = data.data();
    }

    // The buffer is empty. Only what is left at the end, less than a buffer full, goes into it.
    int64_t remaining = length;
    int64_t written = 0;
    while (remaining >= s.bufferSize) {
        int64_t count = stream.rawWrite(data.owner(), data.offset() + written, source + written, length - written);
        if (failed(scope, count != -1))
            return { };
        if (failed(scope, data.refresh()))
            return { };
        source = data.data();
        if (count == -2) {
            if (remaining > s.bufferSize) {
                memcpy(stream.buffer(), source + written, s.bufferSize);
                s.rawPosition = 0;
                stream.adjustPosition(s.bufferSize);
                s.writeEnd = s.bufferSize;
                written += s.bufferSize;
                scope.release();
                stream.raiseBlocking(written);
                return { };
            }
            break;
        }
        written += count;
        remaining -= count;
        // As in flushWrites().
        if (failed(scope, checkOnlySignals(globalObject)) || failed(scope, data.refresh()))
            return { };
        source = data.data();
    }
    if (s.readable)
        stream.resetReadBuffer();
    if (remaining > 0) {
        memcpy(stream.buffer(), source + written, remaining);
        written += remaining;
    }
    s.writePosition = 0;
    s.writeEnd = remaining;
    stream.adjustPosition(remaining);
    s.rawPosition = 0;
    if (remaining > 0)
        bufferedNotePendingOutput(stream);
    RELEASE_AND_RETURN(scope, makeInt(globalObject, written));
}

static constexpr NativeGetSet bufferedGetSets[] = {
    { "closed"_s, bufferedGetClosed },
    { "name"_s, bufferedGetName },
    { "mode"_s, bufferedGetMode },
    PY_MEMBER("raw", bufferedGetRaw),
    PY_WRITABLE_MEMBER("_finalizing", bufferedGetFinalizing, bufferedSetFinalizing),
};

static constexpr NativeDef bufferedReaderMethods[] = {
    PY_NEW(bufferedNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", bufferedReaderInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", bufferedRepr, "($self, /)"),
    PY_SLOT("__next__", bufferedNext, "($self, /)"),
    PY_METHOD("detach", bufferedDetach, "($self, /)"),
    PY_STREAM_METHOD("flush", bufferedSimpleFlush, "($self, /)"),
    PY_METHOD("close", bufferedClose, "($self, /)"),
    PY_STREAM_METHOD("seekable", bufferedSeekable, "($self, /)"),
    PY_STREAM_METHOD("readable", bufferedReadable, "($self, /)"),
    PY_STREAM_METHOD("fileno", bufferedFileno, "($self, /)"),
    PY_STREAM_METHOD("isatty", bufferedIsATTY, "($self, /)"),
    PY_METHOD("_dealloc_warn", bufferedDeallocWarn, "($self, source, /)"),
    PY_STREAM_METHOD("read", bufferedRead, "($self, size=-1, /)"),
    PY_STREAM_METHOD("peek", bufferedPeek, "($self, size=0, /)"),
    PY_STREAM_METHOD("read1", bufferedRead1, "($self, size=-1, /)"),
    PY_STREAM_METHOD("readinto", bufferedReadInto, "($self, buffer, /)"),
    PY_STREAM_METHOD("readinto1", bufferedReadInto1, "($self, buffer, /)"),
    PY_STREAM_METHOD("readline", bufferedReadline, "($self, size=-1, /)"),
    PY_STREAM_METHOD("seek", bufferedSeek, "($self, target, whence=0, /)"),
    PY_STREAM_METHOD("tell", bufferedTell, "($self, /)"),
    PY_STREAM_METHOD("truncate", bufferedTruncate, "($self, pos=None, /)"),
    PY_METHOD("__sizeof__", bufferedSizeOf, "($self, /)"),
    PY_METHOD("__getstate__", ioCannotPickle, "($self, /)"),
};

static constexpr NativeDef bufferedWriterMethods[] = {
    PY_NEW(bufferedNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", bufferedWriterInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", bufferedRepr, "($self, /)"),
    PY_METHOD("close", bufferedClose, "($self, /)"),
    PY_METHOD("detach", bufferedDetach, "($self, /)"),
    PY_STREAM_METHOD("seekable", bufferedSeekable, "($self, /)"),
    PY_STREAM_METHOD("writable", bufferedWritable, "($self, /)"),
    PY_STREAM_METHOD("fileno", bufferedFileno, "($self, /)"),
    PY_STREAM_METHOD("isatty", bufferedIsATTY, "($self, /)"),
    PY_METHOD("_dealloc_warn", bufferedDeallocWarn, "($self, source, /)"),
    PY_STREAM_METHOD("write", bufferedWrite, "($self, buffer, /)"),
    PY_STREAM_METHOD("truncate", bufferedTruncate, "($self, pos=None, /)"),
    PY_STREAM_METHOD("flush", bufferedFlush, "($self, /)"),
    PY_STREAM_METHOD("seek", bufferedSeek, "($self, target, whence=0, /)"),
    PY_STREAM_METHOD("tell", bufferedTell, "($self, /)"),
    PY_METHOD("__sizeof__", bufferedSizeOf, "($self, /)"),
    PY_METHOD("__getstate__", ioCannotPickle, "($self, /)"),
};

static constexpr NativeDef bufferedRandomMethods[] = {
    PY_NEW(bufferedNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", bufferedRandomInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", bufferedRepr, "($self, /)"),
    PY_SLOT("__next__", bufferedNext, "($self, /)"),
    PY_METHOD("close", bufferedClose, "($self, /)"),
    PY_METHOD("detach", bufferedDetach, "($self, /)"),
    PY_STREAM_METHOD("seekable", bufferedSeekable, "($self, /)"),
    PY_STREAM_METHOD("readable", bufferedReadable, "($self, /)"),
    PY_STREAM_METHOD("writable", bufferedWritable, "($self, /)"),
    PY_STREAM_METHOD("fileno", bufferedFileno, "($self, /)"),
    PY_STREAM_METHOD("isatty", bufferedIsATTY, "($self, /)"),
    PY_METHOD("_dealloc_warn", bufferedDeallocWarn, "($self, source, /)"),
    PY_STREAM_METHOD("flush", bufferedFlush, "($self, /)"),
    PY_STREAM_METHOD("seek", bufferedSeek, "($self, target, whence=0, /)"),
    PY_STREAM_METHOD("tell", bufferedTell, "($self, /)"),
    PY_STREAM_METHOD("truncate", bufferedTruncate, "($self, pos=None, /)"),
    PY_STREAM_METHOD("read", bufferedRead, "($self, size=-1, /)"),
    PY_STREAM_METHOD("read1", bufferedRead1, "($self, size=-1, /)"),
    PY_STREAM_METHOD("readinto", bufferedReadInto, "($self, buffer, /)"),
    PY_STREAM_METHOD("readinto1", bufferedReadInto1, "($self, buffer, /)"),
    PY_STREAM_METHOD("readline", bufferedReadline, "($self, size=-1, /)"),
    PY_STREAM_METHOD("peek", bufferedPeek, "($self, size=0, /)"),
    PY_STREAM_METHOD("write", bufferedWrite, "($self, buffer, /)"),
    PY_METHOD("__sizeof__", bufferedSizeOf, "($self, /)"),
    PY_METHOD("__getstate__", ioCannotPickle, "($self, /)"),
};

constexpr TypeDef ioBufferedReaderTypeDef { "_io.BufferedReader"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, bufferedReaderMethods, bufferedGetSets, "Create a new buffered reader using the given readable raw IO object."_s };
constexpr TypeDef ioBufferedWriterTypeDef { "_io.BufferedWriter"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, bufferedWriterMethods, bufferedGetSets,
    "A buffer for a writeable sequential RawIO object.\n\n"
    "The constructor creates a BufferedWriter for the given writeable raw stream. If the buffer_size is not given, it\n"
    "defaults to DEFAULT_BUFFER_SIZE."_s };
constexpr TypeDef ioBufferedRandomTypeDef { "_io.BufferedRandom"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, bufferedRandomMethods, bufferedGetSets,
    "A buffered interface to random access streams.\n\n"
    "The constructor creates a reader and writer for a seekable stream, raw, given in the first argument. If the\n"
    "buffer_size is omitted it defaults to DEFAULT_BUFFER_SIZE."_s };

// ---- BufferedRWPair ----

enum BufferedRWPairField : unsigned {
    BufferedRWPairReaderField, // A BufferedReader. Empty, as the writer: not initialized.
    BufferedRWPairWriterField, // A BufferedWriter.
    BufferedRWPairFieldCount,
};

std::pair<JSValue, JSValue> bufferedRWPairWrappedStreams(PyRecord* pair)
{
    return { pair->get(BufferedRWPairReaderField), pair->get(BufferedRWPairWriterField) };
}

static JSValue bufferedRWPairNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    Runtime& rt = runtime(globalObject);
    PyRecord* pair = PyRecord::create(rt.vm(), rt.structure<PyRecord>(), PyType::from(cls), BufferedRWPairFieldCount);
    ioDidAllocate(globalObject, pair);
    return pair;
}

static JSValue bufferedRWPairInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "BufferedRWPair"_s, args)) || failed(scope, checkPositional(globalObject, "BufferedRWPair"_s, args, 2, 3)))
        return { };
    int64_t bufferSize = ioDefaultBufferSize;
    if (args.size() > 2 && failed(scope, ssizeArgument(globalObject, args[2], bufferSize)))
        return { };
    if (failed(scope, ioCheckReadable(globalObject, args[0])) || failed(scope, ioCheckWritable(globalObject, args[1])))
        return { };

    PyRecord* pair = PyRecord::from(self);
    for (unsigned field = 0; field < BufferedRWPairFieldCount; ++field) {
        if (JSValue previous = pair->get(field))
            ioDidUnwrap(globalObject, previous);
    }
    JSValue reader = call(globalObject, ioType(globalObject, IOType::BufferedReader), args[0], makeInt(globalObject, bufferSize));
    RETURN_IF_EXCEPTION(scope, { });
    pair->set(vm, BufferedRWPairReaderField, reader);
    JSValue writer = call(globalObject, ioType(globalObject, IOType::BufferedWriter), args[1], makeInt(globalObject, bufferSize));
    if (failed(scope, writer)) {
        pair->set(vm, BufferedRWPairReaderField, JSValue());
        return { };
    }
    pair->set(vm, BufferedRWPairWriterField, writer);
    ioDidOpen(globalObject, pair);
    ioDidWrap(globalObject, reader);
    ioDidWrap(globalObject, writer);
    return jsUndefined();
}

// reader.method(*args), or the same of the writer.
static JSValue bufferedRWPairForward(JSGlobalObject* globalObject, JSValue self, unsigned field, JSString* method, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue target = PyRecord::from(self)->get(field);
    if (!target)
        return raiseValueError(globalObject, scope, "I/O operation on uninitialized object"_s);
    JSValue function = getAttr(globalObject, target, method);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, function, args));
}

#define PY_DEFINE_RWPAIR_METHOD(function, field, text, name) \
    static JSValue function(JSGlobalObject* globalObject, JSValue self, const CallArgs& args) \
    { \
        auto scope = DECLARE_THROW_SCOPE(getVM(globalObject)); \
        if (failed(scope, rejectKeywords(globalObject, text ""_s, args))) \
            return { }; \
        RELEASE_AND_RETURN(scope, bufferedRWPairForward(globalObject, self, field, name, args)); \
    }
PY_DEFINE_RWPAIR_METHOD(bufferedRWPairRead, BufferedRWPairReaderField, "read", runtime(globalObject).names.read())
PY_DEFINE_RWPAIR_METHOD(bufferedRWPairPeek, BufferedRWPairReaderField, "peek", ioName(globalObject, IOName::peek))
PY_DEFINE_RWPAIR_METHOD(bufferedRWPairRead1, BufferedRWPairReaderField, "read1", ioName(globalObject, IOName::read1))
PY_DEFINE_RWPAIR_METHOD(bufferedRWPairReadInto, BufferedRWPairReaderField, "readinto", ioName(globalObject, IOName::readinto))
PY_DEFINE_RWPAIR_METHOD(bufferedRWPairReadInto1, BufferedRWPairReaderField, "readinto1", ioName(globalObject, IOName::readinto1))
PY_DEFINE_RWPAIR_METHOD(bufferedRWPairWrite, BufferedRWPairWriterField, "write", runtime(globalObject).names.write())
#undef PY_DEFINE_RWPAIR_METHOD

static JSValue bufferedRWPairFlush(JSGlobalObject* globalObject, JSValue self) { return bufferedRWPairForward(globalObject, self, BufferedRWPairWriterField, runtime(globalObject).names.flush(), { }); }
static JSValue bufferedRWPairReadable(JSGlobalObject* globalObject, JSValue self) { return bufferedRWPairForward(globalObject, self, BufferedRWPairReaderField, ioName(globalObject, IOName::readable), { }); }
static JSValue bufferedRWPairWritable(JSGlobalObject* globalObject, JSValue self) { return bufferedRWPairForward(globalObject, self, BufferedRWPairWriterField, ioName(globalObject, IOName::writable), { }); }

static JSValue bufferedRWPairClose(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSString* close = runtime(globalObject).names.close();
    JSValue writerError;
    if (failed(scope, bufferedRWPairForward(globalObject, self, BufferedRWPairWriterField, close, { }))) {
        writerError = takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue result = bufferedRWPairForward(globalObject, self, BufferedRWPairReaderField, close, { });
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    ioDidClose(globalObject, PyRecord::from(self));
    scope.release();
    if (writerError) {
        ioChainExceptions(globalObject, writerError);
        return { };
    }
    return result;
}

static JSValue bufferedRWPairIsATTY(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* isatty = ioName(globalObject, IOName::isatty);
    JSValue result = bufferedRWPairForward(globalObject, self, BufferedRWPairWriterField, isatty, { });
    if (failed(scope, result) || !result.isFalse())
        return result;
    RELEASE_AND_RETURN(scope, bufferedRWPairForward(globalObject, self, BufferedRWPairReaderField, isatty, { }));
}

static JSValue bufferedRWPairGetClosed(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue writer = PyRecord::from(self)->get(BufferedRWPairWriterField);
    if (!writer)
        return raiseRuntimeError(globalObject, scope, "the BufferedRWPair object is being garbage-collected"_s);
    RELEASE_AND_RETURN(scope, getAttr(globalObject, writer, ioName(globalObject, IOName::closed)));
}

static constexpr NativeDef bufferedRWPairMethods[] = {
    PY_NEW(bufferedRWPairNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", bufferedRWPairInit, "($self, /, *args, **kwargs)"),
    PY_METHOD("read", bufferedRWPairRead, "($self, /, *args, **kwargs)"),
    PY_METHOD("peek", bufferedRWPairPeek, "($self, /, *args, **kwargs)"),
    PY_METHOD("read1", bufferedRWPairRead1, "($self, /, *args, **kwargs)"),
    PY_METHOD("readinto", bufferedRWPairReadInto, "($self, /, *args, **kwargs)"),
    PY_METHOD("readinto1", bufferedRWPairReadInto1, "($self, /, *args, **kwargs)"),
    PY_METHOD("write", bufferedRWPairWrite, "($self, /, *args, **kwargs)"),
    PY_METHOD("flush", bufferedRWPairFlush, "($self, /)"),
    PY_METHOD("readable", bufferedRWPairReadable, "($self, /)"),
    PY_METHOD("writable", bufferedRWPairWritable, "($self, /)"),
    PY_METHOD("close", bufferedRWPairClose, "($self, /)"),
    PY_METHOD("isatty", bufferedRWPairIsATTY, "($self, /)"),
};

static constexpr NativeGetSet bufferedRWPairGetSets[] = {
    { "closed"_s, bufferedRWPairGetClosed },
};

constexpr TypeDef ioBufferedRWPairTypeDef { "_io.BufferedRWPair"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, bufferedRWPairMethods, bufferedRWPairGetSets,
    "A buffered reader and writer object together.\n\n"
    "A buffered reader object and buffered writer object put together to form a sequential IO object that can read and\n"
    "write. This is typically used with a socket or two-way pipe."_s };

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
