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

#include <errno.h>
#include <unistd.h>

// io.BufferedReader, BufferedWriter, BufferedRandom and BufferedRWPair: Modules/_io/bufferedio.c of CPython.

namespace JSC { namespace Python {

namespace {

// The first three share nearly everything, and one buffer for reading and for writing, so that the two can be mixed without sending anything on. See the "Implementation notes" of CPython's.
struct BufferedState final : NativeState {
    PYTHON_NATIVE_STATE(BufferedState);

    WriteBarrier<Unknown> raw;
    // The buffer. It is a bytearray, because the raw stream is given a memoryview of some of it to read into or to write from. No program gets hold of it.
    WriteBarrier<JSObject> buffer;
    bool isInitialized { false }; // ok
    bool isDetached { false };
    bool isReadable { false };
    bool isWritable { false };
    bool isFinalizing { false };
    // It is of one of these classes and not of one derived from them, and the raw stream is a FileIO likewise.
    bool hasFastClosedChecks { false };
    // Something is in the middle of being done with it. There is one thread, so if something else is begun it is by what that called.
    bool isBusy { false };
    int64_t absolutePosition { -1 }; // In the raw stream. -1 is that it is not known.
    int64_t position { 0 }; // In the buffer, of where the program is.
    int64_t rawPosition { 0 }; // In the buffer, of where the raw stream is.
    int64_t readEnd { -1 }; // Just after the last byte that has been read into the buffer. -1 is that it is not fit to be read from.
    int64_t writePosition { 0 }; // Just after the last byte that has been sent on.
    int64_t writeEnd { -1 }; // Just after the last that is waiting to be. -1 is that it is not fit to be written to.
    int64_t bufferSize { 0 };
    int64_t bufferMask { 0 };

    bool hasValidReadBuffer() const { return isReadable && readEnd != -1; }
    bool hasValidWriteBuffer() const { return isWritable && writeEnd != -1; }
    int64_t readAhead() const { return hasValidReadBuffer() ? readEnd - position : 0; }
    int64_t rawOffset() const { return (hasValidReadBuffer() || hasValidWriteBuffer()) && rawPosition >= 0 ? rawPosition - position : 0; }
    void adjustPosition(int64_t newPosition)
    {
        position = newPosition;
        if (hasValidReadBuffer() && readEnd < position)
            readEnd = position;
    }
    int64_t minusLastBlock(int64_t size) const { return bufferMask ? size & ~bufferMask : bufferSize * (size / bufferSize); }
    void resetReadBuffer() { readEnd = -1; }
    void resetWriteBuffer()
    {
        writePosition = 0;
        writeEnd = -1;
    }
    std::span<uint8_t> bytes() const { return uncheckedDowncast<JSUint8Array>(buffer.get())->typedSpan(); }
    std::span<uint8_t> bytes(int64_t from, int64_t count) const { return bytes().subspan(static_cast<size_t>(from), static_cast<size_t>(count)); }
};

template<typename Visitor>
void BufferedState::visit(Visitor& visitor)
{
    visitor.append(raw);
    visitor.append(buffer);
}

struct PairState final : NativeState {
    PYTHON_NATIVE_STATE(PairState);
    WriteBarrier<Unknown> reader;
    WriteBarrier<Unknown> writer;
};

template<typename Visitor>
void PairState::visit(Visitor& visitor)
{
    visitor.append(reader);
    visitor.append(writer);
}

BufferedState& stateOfBuffered(JSValue self) { return stateOf<BufferedState>(self); }

// ENTER_BUFFERED() and LEAVE_BUFFERED()
class BusyScope {
    WTF_MAKE_NONCOPYABLE(BusyScope);
public:
    BusyScope(JSGlobalObject* globalObject, JSValue self, BufferedState& state)
        : m_state(state)
    {
        enter(globalObject, self);
    }
    ~BusyScope() { leave(); }

    // False if it raised.
    explicit operator bool() const { return m_hasEntered; }
    void leave()
    {
        if (std::exchange(m_hasEntered, false))
            m_state.isBusy = false;
    }
    bool enter(JSGlobalObject* globalObject, JSValue self)
    {
        ASSERT(!m_hasEntered);
        if (m_state.isBusy) {
            auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
            String shown = repr(globalObject, self);
            RETURN_IF_EXCEPTION(scope, false);
            raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("reentrant call inside "_s, shown));
            return false;
        }
        m_state.isBusy = true;
        m_hasEntered = true;
        return true;
    }

private:
    BufferedState& m_state;
    bool m_hasEntered { false };
};

bool checkIsInitialized(JSGlobalObject* globalObject, ThrowScope& scope, BufferedState& state)
{
    if (state.isInitialized)
        return true;
    raiseValueError(globalObject, scope, state.isDetached ? "raw stream has been detached"_s : "I/O operation on uninitialized object"_s);
    return false;
}

#define CHECK_INITIALIZED() \
    if (!checkIsInitialized(globalObject, scope, state)) \
        return { };

// buffered_closed(): 1, 0, or -1 if it raised.
int isRawClosed(JSGlobalObject* globalObject, BufferedState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!checkIsInitialized(globalObject, scope, state))
        return -1;
    JSValue closed = getAttribute(globalObject, state.raw.get(), vm.pythonNames().attribute_closed);
    RETURN_IF_EXCEPTION(scope, -1);
    bool result = isTrue(globalObject, closed);
    RETURN_IF_EXCEPTION(scope, -1);
    return result;
}

// IS_CLOSED(). If finding out raised, it is taken for closed, and what was raised gives way to what is said of that.
bool isClosed(JSGlobalObject* globalObject, BufferedState& state)
{
    if (!state.buffer)
        return true;
    if (state.hasFastClosedChecks)
        return isFileIOClosed(state.raw.get());
    int result = isRawClosed(globalObject, state);
    if (result < 0)
        takeRaisedException(globalObject->vm());
    return result;
}

// CHECK_CLOSED(): what has been read already can still be had of one that is closed.
bool checkIsOpen(JSGlobalObject* globalObject, ThrowScope& scope, BufferedState& state, ASCIILiteral complaint)
{
    bool closed = isClosed(globalObject, state);
    RETURN_IF_EXCEPTION(scope, false);
    if (!closed || state.readAhead())
        return true;
    raiseValueError(globalObject, scope, complaint);
    return false;
}

#define CHECK_CLOSED(complaint) \
    if (!checkIsOpen(globalObject, scope, state, complaint)) \
        return { };

// _set_BlockingIOError()
void raiseBlockingIOError(JSGlobalObject* globalObject, int errorNumber, ASCIILiteral message, int64_t written)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue error = call(globalObject, globalObject->pyRealm()->type(BuiltinType::BlockingIOError), jsNumber(errorNumber), jsString(vm, String(message)), intFromInt64(globalObject, written));
    RETURN_IF_EXCEPTION(scope, void());
    raiseObject(globalObject, scope, error);
}

// _buffered_check_blocking_error(): whether that is what has been raised. It stays raised.
bool hasRaisedBlockingIOError(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Exception* raised = scope.exception();
    return raised && isInstance(globalObject, raised->value(), globalObject->pyRealm()->type(BuiltinType::BlockingIOError));
}

// What a raw stream says of where it is. -1 if it raised.
int64_t checkRawPosition(JSGlobalObject* globalObject, BufferedState& state, JSValue result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto position = toOffset(globalObject, result, BuiltinType::ValueError);
    RETURN_IF_EXCEPTION(scope, -1);
    if (*position < 0) {
        raise(globalObject, scope, BuiltinType::OSError, concatenate("Raw stream returned invalid position "_s, *position));
        return -1;
    }
    state.absolutePosition = *position;
    return *position;
}

// _buffered_raw_tell()
int64_t rawTell(JSGlobalObject* globalObject, BufferedState& state)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = callMethodNamed(globalObject, state.raw.get(), globalObject->vm().pythonNames().attribute_tell);
    RETURN_IF_EXCEPTION(scope, -1);
    RELEASE_AND_RETURN(scope, checkRawPosition(globalObject, state, result));
}

// _buffered_raw_seek()
int64_t rawSeek(JSGlobalObject* globalObject, BufferedState& state, int64_t target, int whence)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = callMethodNamed(globalObject, state.raw.get(), globalObject->vm().pythonNames().attribute_seek, intFromInt64(globalObject, target), jsNumber(whence));
    RETURN_IF_EXCEPTION(scope, -1);
    RELEASE_AND_RETURN(scope, checkRawPosition(globalObject, state, result));
}

// _buffered_init(). False if it raised.
bool initializeBuffer(JSGlobalObject* globalObject, JSCell* owner, BufferedState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (state.bufferSize <= 0) {
        raiseValueError(globalObject, scope, "buffer size must be strictly positive"_s);
        return false;
    }
    ByteVector zeros;
    zeros.appendFill(0, static_cast<size_t>(state.bufferSize));
    JSUint8Array* buffer = newByteArray(globalObject, zeros);
    RETURN_IF_EXCEPTION(scope, false);
    state.buffer.set(vm, owner, buffer);
    state.isBusy = false;
    // Whether the size is a power of two.
    state.bufferMask = hasOneBitSet(static_cast<uint64_t>(state.bufferSize)) ? state.bufferSize - 1 : 0;
    if (rawTell(globalObject, state) == -1) {
        takeRaisedException(vm);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// A memoryview of some of the bytes of something, for the raw stream to read into or to write from.
PyMemoryView* viewOf(JSGlobalObject* globalObject, JSObject* holder, int64_t start, int64_t length, bool isReadOnly)
{
    PyMemoryView::Layout layout;
    layout.isReadOnly = isReadOnly;
    layout.offset = start;
    PyMemoryView::Dimension dimension { length, 1 };
    return PyMemoryView::create(globalObject, holder, layout, { &dimension, 1 });
}

// PyNumber_AsSsize_t(value, PyExc_ValueError)
std::optional<int64_t> toSsizeOrValueError(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto result = tryInt64(integer);
    if (!result)
        raiseValueError(globalObject, scope, concatenate("cannot fit '"_s, typeName(globalObject, value), "' into an index-sized integer"_s));
    return result;
}

constexpr int64_t failed = -1;
constexpr int64_t wouldHaveBlocked = -2;

// _bufferedreader_raw_read(): reads into so many of the bytes of `holder`, which is the buffer or what is to be given back.
int64_t rawRead(JSGlobalObject* globalObject, BufferedState& state, JSObject* holder, int64_t start, int64_t length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyMemoryView* view = viewOf(globalObject, holder, start, length, false);
    JSValue result;
    do {
        result = callMethodNamed(globalObject, state.raw.get(), vm.pythonNames().attribute_readinto, view);
    } while (scope.exception() && trapInterruptedError(globalObject));
    // Whoever has kept it has nothing to look at any longer.
    {
        Exception* raised = takeRaisedException(vm);
        view->release(globalObject);
        restoreRaisedException(globalObject, raised);
    }
    RETURN_IF_EXCEPTION(scope, failed);
    if (isNone(result))
        return wouldHaveBlocked;
    auto count = toSsizeOrValueError(globalObject, result);
    if (scope.exception()) {
        // _PyErr_FormatFromCause()
        Exception* cause = takeRaisedException(vm);
        RETURN_IF_EXCEPTION(scope, failed);
        JSObject* error = createException(globalObject, globalObject->pyRealm()->typeOSError(), "raw readinto() failed"_s);
        RETURN_IF_EXCEPTION(scope, failed);
        error->putDirect(vm, vm.pythonNames().private_cause, cause->value());
        error->putDirect(vm, vm.pythonNames().private_context, cause->value());
        error->putDirect(vm, vm.pythonNames().private_suppressContext, jsBoolean(true));
        throwException(globalObject, scope, error);
        return failed;
    }
    if (*count < 0 || *count > length) {
        raise(globalObject, scope, BuiltinType::OSError, concatenate("raw readinto() returned invalid length "_s, *count, " (should have been between 0 and "_s, length, ')'));
        return failed;
    }
    if (*count > 0 && state.absolutePosition != -1)
        state.absolutePosition += *count;
    return *count;
}

// _bufferedreader_fill_buffer()
int64_t fillBuffer(JSGlobalObject* globalObject, BufferedState& state)
{
    int64_t start = state.hasValidReadBuffer() ? state.readEnd : 0;
    int64_t count = rawRead(globalObject, state, state.buffer.get(), start, state.bufferSize - start);
    if (count <= 0)
        return count;
    state.readEnd = start + count;
    state.rawPosition = start + count;
    return count;
}

// _bufferedwriter_raw_write(). `errorNumber` is what errno was left as, which is what is said if it would have blocked.
int64_t rawWrite(JSGlobalObject* globalObject, BufferedState& state, JSObject* holder, int64_t start, int64_t length, int& errorNumber)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyMemoryView* view = viewOf(globalObject, holder, start, length, true);
    JSValue result;
    do {
        errno = 0;
        result = callMethodNamed(globalObject, state.raw.get(), vm.pythonNames().attribute_write, view);
        errorNumber = errno;
    } while (scope.exception() && trapInterruptedError(globalObject));
    {
        Exception* raised = takeRaisedException(vm);
        view->release(globalObject);
        restoreRaisedException(globalObject, raised);
    }
    RETURN_IF_EXCEPTION(scope, failed);
    if (isNone(result))
        return wouldHaveBlocked;
    auto given = toSsizeOrValueError(globalObject, result);
    // What that raised gives way to this.
    if (scope.exception() && !scope.tryClearException())
        return failed;
    int64_t count = given.value_or(-1);
    if (count < 0 || count > length) {
        raise(globalObject, scope, BuiltinType::OSError, concatenate("raw write() returned invalid length "_s, count, " (should have been between 0 and "_s, length, ')'));
        return failed;
    }
    if (count > 0 && state.absolutePosition != -1)
        state.absolutePosition += count;
    return count;
}

// _bufferedwriter_flush_unlocked(). False if it raised.
bool flushUnlocked(JSGlobalObject* globalObject, BufferedState& state)
{
    if (state.hasValidWriteBuffer() && state.writePosition != state.writeEnd) {
        // First the raw stream goes back to where what is to be written belongs.
        int64_t rewind = state.rawOffset() + (state.position - state.writePosition);
        if (rewind) {
            if (rawSeek(globalObject, state, -rewind, 1) < 0)
                return false;
            state.rawPosition -= rewind;
        }
        while (state.writePosition < state.writeEnd) {
            int errorNumber = 0;
            int64_t count = rawWrite(globalObject, state, state.buffer.get(), state.writePosition, state.writeEnd - state.writePosition, errorNumber);
            if (count == failed)
                return false;
            if (count == wouldHaveBlocked) {
                raiseBlockingIOError(globalObject, errorNumber, "write could not complete without blocking"_s, 0);
                return false;
            }
            state.writePosition += count;
            state.rawPosition = state.writePosition;
            // Some can have been written when it was interrupted, and what interrupted it is seen to before waiting again.
            if (!checkSignals(globalObject))
                return false;
        }
    }
    // So that the buffer is not fit to be written to when this returns, and tell() has nothing to allow for.
    state.resetWriteBuffer();
    return true;
}

// buffered_flush_and_rewind_unlocked()
bool flushAndRewindUnlocked(JSGlobalObject* globalObject, BufferedState& state)
{
    if (!flushUnlocked(globalObject, state))
        return false;
    if (state.isReadable) {
        // The raw stream goes back to where the program is.
        int64_t position = rawSeek(globalObject, state, -state.rawOffset(), 1);
        state.resetReadBuffer();
        if (position == -1)
            return false;
    }
    return true;
}

// _bufferedreader_read_fast(): so many bytes out of the buffer, if it has them. Otherwise empty, with nothing raised.
JSValue readFast(JSGlobalObject* globalObject, BufferedState& state, int64_t count)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (count > state.readAhead())
        return { };
    JSValue result = newBytes(globalObject, count ? state.bytes(state.position, count) : std::span<uint8_t>());
    RETURN_IF_EXCEPTION(scope, { });
    state.position += count;
    return result;
}

// _bufferedreader_read_all()
JSValue readAll(JSGlobalObject* globalObject, BufferedState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto& names = vm.pythonNames();
    // What there is in the buffer first.
    ByteVector data;
    int64_t currentSize = state.readAhead();
    if (currentSize) {
        data.append(state.bytes(state.position, currentSize));
        state.position += currentSize;
    }
    if (state.isWritable && !flushAndRewindUnlocked(globalObject, state))
        return { };
    state.resetReadBuffer();

    JSValue readall = getAttributeIfPresent(globalObject, state.raw.get(), names.attribute_readall);
    RETURN_IF_EXCEPTION(scope, { });
    if (readall) {
        JSValue rest = call(globalObject, readall);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(rest) && !isInstance(globalObject, rest, realm->typeBytes()))
            return raiseTypeError(globalObject, scope, "readall() should return bytes"_s);
        if (!currentSize)
            return rest;
        if (!isNone(rest))
            data.append(*builtinBufferOf(rest));
        RELEASE_AND_RETURN(scope, newBytes(globalObject, data));
    }
    while (true) {
        // Until there is no more, or there would be none without waiting.
        JSValue more = callMethodNamed(globalObject, state.raw.get(), names.attribute_read);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNone(more) && !isInstance(globalObject, more, realm->typeBytes()))
            return raiseTypeError(globalObject, scope, "read() should return bytes"_s);
        if (isNone(more) || builtinBufferOf(more)->empty()) {
            if (!currentSize)
                return more;
            RELEASE_AND_RETURN(scope, newBytes(globalObject, data));
        }
        auto bytes = *builtinBufferOf(more);
        data.append(bytes);
        currentSize += static_cast<int64_t>(bytes.size());
        if (state.absolutePosition != -1)
            state.absolutePosition += static_cast<int64_t>(bytes.size());
    }
}

// _bufferedreader_read_generic()
JSValue readGeneric(JSGlobalObject* globalObject, BufferedState& state, int64_t wanted)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t currentSize = state.readAhead();
    if (wanted <= currentSize)
        RELEASE_AND_RETURN(scope, readFast(globalObject, state, wanted));

    // What is to be given back is put together here. The raw stream reads straight into it where it can.
    ByteVector zeros;
    zeros.appendFill(0, static_cast<size_t>(wanted));
    JSUint8Array* out = newByteArray(globalObject, zeros);
    RETURN_IF_EXCEPTION(scope, { });
    auto outBytes = [&] (int64_t from, int64_t count) { return out->typedSpan().subspan(static_cast<size_t>(from), static_cast<size_t>(count)); };
    auto finish = [&] (int64_t count) { return newBytes(globalObject, out->span().first(static_cast<size_t>(count))); };
    int64_t remaining = wanted;
    int64_t written = 0;
    if (currentSize > 0) {
        memcpySpan(outBytes(0, currentSize), state.bytes(state.position, currentSize));
        remaining -= currentSize;
        written += currentSize;
        state.position += currentSize;
    }
    if (state.isWritable && !flushAndRewindUnlocked(globalObject, state))
        return { };
    state.resetReadBuffer();
    while (remaining > 0) {
        // The last of it is to be a whole block read into the buffer.
        int64_t count = state.minusLastBlock(remaining);
        if (!count)
            break;
        count = rawRead(globalObject, state, out, written, count);
        if (count == failed)
            return { };
        if (!count || count == wouldHaveBlocked) {
            if (!count || written > 0)
                RELEASE_AND_RETURN(scope, finish(written));
            return jsUndefined();
        }
        remaining -= count;
        written += count;
    }
    state.position = 0;
    state.rawPosition = 0;
    state.readEnd = 0;
    // Once there is as much as was asked for nothing more is read, which might mean waiting for ever.
    while (remaining > 0 && state.readEnd < state.bufferSize) {
        int64_t count = fillBuffer(globalObject, state);
        if (count == failed)
            return { };
        if (!count || count == wouldHaveBlocked) {
            if (!count || written > 0)
                RELEASE_AND_RETURN(scope, finish(written));
            return jsUndefined();
        }
        int64_t taken = std::min(remaining, count);
        memcpySpan(outBytes(written, taken), state.bytes(state.position, taken));
        written += taken;
        state.position += taken;
        remaining -= taken;
    }
    RELEASE_AND_RETURN(scope, finish(wanted));
}

// _bufferedreader_peek_unlocked()
JSValue peekUnlocked(JSGlobalObject* globalObject, BufferedState& state)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // Where it is is not to change, and neither are blocks to get out of step by anything in the buffer being moved. So it is what there is, or if there is nothing, a buffer full.
    if (int64_t have = state.readAhead(); have > 0)
        RELEASE_AND_RETURN(scope, newBytes(globalObject, state.bytes(state.position, have)));
    state.resetReadBuffer();
    int64_t count = fillBuffer(globalObject, state);
    if (count == failed)
        return { };
    if (count == wouldHaveBlocked)
        count = 0;
    state.position = 0;
    RELEASE_AND_RETURN(scope, newBytes(globalObject, state.bytes(0, count)));
}

// _buffered_readline()
JSValue readLine(JSGlobalObject* globalObject, JSValue self, BufferedState& state, int64_t limit)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CHECK_CLOSED("readline of closed file"_s);

    // First in what the buffer has.
    int64_t count = state.readAhead();
    if (limit >= 0 && count > limit)
        count = limit;
    if (count) {
        size_t found = find(state.bytes(state.position, count), '\n');
        if (found != notFound || count == limit) {
            int64_t length = found != notFound ? static_cast<int64_t>(found) + 1 : count;
            JSValue result = newBytes(globalObject, state.bytes(state.position, length));
            RETURN_IF_EXCEPTION(scope, { });
            state.position += length;
            return result;
        }
    } else if (!limit)
        RELEASE_AND_RETURN(scope, newBytes(globalObject, { }));

    BusyScope busy(globalObject, self, state);
    if (!busy)
        return { };
    // Then in what more is to be had of the raw stream.
    ByteVector line;
    if (count > 0) {
        line.append(state.bytes(state.position, count));
        state.position += count;
        if (limit >= 0)
            limit -= count;
    }
    if (state.isWritable && !flushAndRewindUnlocked(globalObject, state))
        return { };
    while (true) {
        state.resetReadBuffer();
        count = fillBuffer(globalObject, state);
        if (count == failed)
            return { };
        if (count <= 0)
            break;
        if (limit >= 0 && count > limit)
            count = limit;
        size_t found = find(state.bytes(0, count), '\n');
        if (found != notFound) {
            line.append(state.bytes(0, static_cast<int64_t>(found) + 1));
            state.position = static_cast<int64_t>(found) + 1;
            break;
        }
        line.append(state.bytes(0, count));
        if (count == limit) {
            state.position = count;
            break;
        }
        if (limit >= 0)
            limit -= count;
    }
    RELEASE_AND_RETURN(scope, newBytes(globalObject, line));
}

} // anonymous namespace

// ---- What all three have

PYTHON_NATIVE(bufferedSizeOf)
{
    NativeArguments args(callFrame);
    auto& state = stateOfBuffered(args[0]);
    return JSValue::encode(intFromInt64(globalObject, typeOf(globalObject, args[0])->basicSize() + (state.buffer ? state.bufferSize : 0)));
}

static void deallocWarn(JSGlobalObject* globalObject, BufferedState& state, JSValue source)
{
    VM& vm = globalObject->vm();
    if (!state.isInitialized || !state.raw)
        return;
    callMethodNamed(globalObject, state.raw.get(), vm.pythonNames().attribute__dealloc_warn, source);
    takeRaisedException(vm);
}

PYTHON_NATIVE(bufferedDeallocWarn)
{
    NativeArguments args(callFrame);
    deallocWarn(globalObject, stateOfBuffered(args[0]), args[1]);
    RETURN_NONE();
}

enum class BufferedForward : uint8_t { Flush, Seekable, Readable, Writable, Fileno, IsATTY };

// What is only asked of the raw stream.
PYTHON_NATIVE(bufferedForward)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    CHECK_INITIALIZED();
    const Identifier* forwarded[] = { &names.attribute_flush, &names.attribute_seekable, &names.attribute_readable, &names.attribute_writable, &names.attribute_fileno, &names.attribute_isatty };
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, state.raw.get(), *forwarded[static_cast<unsigned>(unpack<BufferedForward>(callFrame, 0))])));
}

template<const Identifier CommonNames::* name>
static JSValue getFromRaw(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = stateOfBuffered(self);
    CHECK_INITIALIZED();
    RELEASE_AND_RETURN(scope, getAttribute(globalObject, state.raw.get(), vm.pythonNames().*name));
}

PYTHON_NATIVE(bufferedClose)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfBuffered(self);
    CHECK_INITIALIZED();
    BusyScope busy(globalObject, self, state);
    if (!busy)
        return { };
    int closed = isRawClosed(globalObject, state);
    if (closed < 0)
        return { };
    if (closed)
        RETURN_NONE();
    if (state.isFinalizing) {
        deallocWarn(globalObject, state, self);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // flush() will want it for itself.
    busy.leave();
    callMethodNamed(globalObject, self, names.attribute_flush);
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, { });
    if (!busy.enter(globalObject, self))
        return { };
    JSValue result = callMethodNamed(globalObject, state.raw.get(), names.attribute_close);
    state.buffer.clear();
    chainRaisedExceptions(globalObject, raised);
    state.readEnd = 0;
    state.position = 0;
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(bufferedDetach)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    CHECK_INITIALIZED();
    callMethodNamed(globalObject, args[0], names.attribute_flush);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue raw = state.raw.get();
    state.raw.clear();
    state.isDetached = true;
    state.isInitialized = false;
    return JSValue::encode(raw);
}

PYTHON_NATIVE(bufferedFlush)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    CHECK_INITIALIZED();
    CHECK_CLOSED("flush of closed file"_s);
    BusyScope busy(globalObject, args[0], state);
    if (!busy)
        return { };
    if (!flushAndRewindUnlocked(globalObject, state))
        return { };
    RETURN_NONE();
}

PYTHON_NATIVE(bufferedPeek)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    if (JSValue size = args.at(1)) {
        toSsize(globalObject, size);
        RETURN_IF_EXCEPTION(scope, { });
    }
    CHECK_INITIALIZED();
    CHECK_CLOSED("peek of closed file"_s);
    BusyScope busy(globalObject, args[0], state);
    if (!busy)
        return { };
    if (state.isWritable && !flushAndRewindUnlocked(globalObject, state))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(peekUnlocked(globalObject, state)));
}

PYTHON_NATIVE(bufferedRead)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    auto count = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_INITIALIZED();
    if (*count < -1)
        return JSValue::encode(raiseValueError(globalObject, scope, "read length must be non-negative or -1"_s));
    CHECK_CLOSED("read of closed file"_s);
    if (*count == -1) {
        BusyScope busy(globalObject, args[0], state);
        if (!busy)
            return { };
        RELEASE_AND_RETURN(scope, JSValue::encode(readAll(globalObject, state)));
    }
    JSValue result = readFast(globalObject, state, *count);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return JSValue::encode(result);
    BusyScope busy(globalObject, args[0], state);
    if (!busy)
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(readGeneric(globalObject, state, *count)));
}

PYTHON_NATIVE(bufferedRead1)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    int64_t count = -1;
    if (JSValue size = args.at(1)) {
        auto given = toSsize(globalObject, size);
        RETURN_IF_EXCEPTION(scope, { });
        count = *given;
    }
    CHECK_INITIALIZED();
    if (count < 0)
        count = state.bufferSize;
    CHECK_CLOSED("read of closed file"_s);
    if (!count)
        RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, { })));
    // If there is anything in the buffer, some of that and no more. Otherwise the raw stream is read from once.
    if (int64_t have = state.readAhead(); have > 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(readFast(globalObject, state, std::min(have, count))));
    ByteVector zeros;
    zeros.appendFill(0, static_cast<size_t>(count));
    JSUint8Array* out = newByteArray(globalObject, zeros);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t read;
    {
        BusyScope busy(globalObject, args[0], state);
        if (!busy)
            return { };
        if (state.isWritable && !flushAndRewindUnlocked(globalObject, state))
            return { };
        state.resetReadBuffer();
        read = rawRead(globalObject, state, out, 0, count);
    }
    if (read == failed)
        return { };
    if (read == wouldHaveBlocked)
        read = 0;
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, out->span().first(static_cast<size_t>(read)))));
}

// _buffered_readinto_generic()
PYTHON_NATIVE(bufferedReadInto)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    bool isReadInto1 = unpack<bool>(callFrame, 0);
    Buffer buffer = writableBufferArgument(globalObject, args[1], isReadInto1 ? "readinto1"_s : "readinto"_s);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_INITIALIZED();
    CHECK_CLOSED("readinto of closed file"_s);
    int64_t length = static_cast<int64_t>(buffer.size());
    // What runs meanwhile can make what is being read into shorter.
    auto copyOut = [&] (int64_t at, std::span<const uint8_t> bytes) {
        auto destination = mutableSpanOf(buffer);
        if (static_cast<size_t>(at) >= destination.size())
            return;
        destination = destination.subspan(static_cast<size_t>(at));
        size_t count = std::min(destination.size(), bytes.size());
        memmoveSpan(destination.first(count), bytes.first(count));
    };
    int64_t written = 0;
    int64_t count = state.readAhead();
    if (count > 0) {
        if (count >= length) {
            copyOut(0, state.bytes(state.position, length));
            state.position += length;
            return JSValue::encode(intFromInt64(globalObject, length));
        }
        copyOut(0, state.bytes(state.position, count));
        state.position += count;
        written = count;
    }
    BusyScope busy(globalObject, args[0], state);
    if (!busy)
        return { };
    if (state.isWritable && !flushAndRewindUnlocked(globalObject, state))
        return { };
    state.resetReadBuffer();
    state.position = 0;
    for (int64_t remaining = length - written; remaining > 0; written += count, remaining -= count) {
        if (remaining > state.bufferSize) {
            // There is more to come than the buffer holds, so it goes straight to where it is wanted, by way of something that a memoryview can be of.
            ByteVector zeros;
            zeros.appendFill(0, static_cast<size_t>(remaining));
            JSUint8Array* direct = newByteArray(globalObject, zeros);
            RETURN_IF_EXCEPTION(scope, { });
            count = rawRead(globalObject, state, direct, 0, remaining);
            if (count > 0)
                copyOut(written, direct->span().first(static_cast<size_t>(count)));
        } else if (!(isReadInto1 && written)) {
            count = fillBuffer(globalObject, state);
            if (count > 0) {
                count = std::min(count, remaining);
                copyOut(written, state.bytes(state.position, count));
                state.position += count;
                continue;
            }
        } else
            count = 0;
        if (!count || (count == wouldHaveBlocked && written > 0))
            break;
        if (count < 0) {
            if (count == wouldHaveBlocked)
                RETURN_NONE();
            return { };
        }
        // No more than the one read.
        if (isReadInto1) {
            written += count;
            break;
        }
    }
    return JSValue::encode(intFromInt64(globalObject, written));
}

PYTHON_NATIVE(bufferedReadLine)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    auto size = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_INITIALIZED();
    RELEASE_AND_RETURN(scope, JSValue::encode(readLine(globalObject, args[0], state, *size)));
}

PYTHON_NATIVE(bufferedTell)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    CHECK_INITIALIZED();
    int64_t position = rawTell(globalObject, state);
    if (position == -1)
        return { };
    return JSValue::encode(intFromInt64(globalObject, std::max<int64_t>(position - state.rawOffset(), 0)));
}

PYTHON_NATIVE(bufferedSeek)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    int whence = 0;
    if (JSValue value = args.at(2)) {
        auto given = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        whence = *given;
    }
    CHECK_INITIALIZED();
    bool isKnownWhence = whence >= 0 && whence <= 2;
#ifdef SEEK_HOLE
    isKnownWhence |= whence == SEEK_HOLE;
#endif
#ifdef SEEK_DATA
    isKnownWhence |= whence == SEEK_DATA;
#endif
    if (!isKnownWhence)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("whence value "_s, whence, " unsupported"_s)));
    CHECK_CLOSED("seek of closed file"_s);
    if (!checkIsSeekable(globalObject, state.raw.get()))
        return { };
    auto given = toOffset(globalObject, args[1], BuiltinType::ValueError);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t target = *given;

    // From the beginning or from where it is, it may be to somewhere that is in the buffer, and then there is nothing to it.
    if ((!whence || whence == 1) && state.isReadable) {
        int64_t current = state.absolutePosition != -1 ? state.absolutePosition : rawTell(globalObject, state);
        // As in CPython, where what that raises is left lying.
        RETURN_IF_EXCEPTION(scope, { });
        int64_t available = state.readAhead();
        if (available > 0) {
            int64_t offset = !whence ? target - (current - state.rawOffset()) : target;
            if (offset >= -state.position && offset <= available) {
                state.position += offset;
                return JSValue::encode(intFromInt64(globalObject, std::max<int64_t>(current - available + offset, 0)));
            }
        }
    }

    BusyScope busy(globalObject, args[0], state);
    if (!busy)
        return { };
    // Otherwise the raw stream is asked, and what is in the buffer is done with.
    if (state.isWritable && !flushUnlocked(globalObject, state))
        return { };
    if (whence == 1)
        target -= state.rawOffset();
    int64_t position = rawSeek(globalObject, state, target, whence);
    if (position == -1)
        return { };
    state.rawPosition = -1;
    if (state.isReadable)
        state.resetReadBuffer();
    return JSValue::encode(intFromInt64(globalObject, position));
}

PYTHON_NATIVE(bufferedTruncate)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfBuffered(args[0]);
    JSValue position = args.at(1);
    CHECK_INITIALIZED();
    CHECK_CLOSED("truncate of closed file"_s);
    if (!state.isWritable)
        return JSValue::encode(raiseUnsupportedOperation(globalObject, scope, "truncate"_s));
    BusyScope busy(globalObject, args[0], state);
    if (!busy)
        return { };
    if (!flushAndRewindUnlocked(globalObject, state))
        return { };
    JSValue result = callMethodNamed(globalObject, state.raw.get(), names.attribute_truncate, position ? position : jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    // Where the raw stream is may not be what it was taken to be.
    if (rawTell(globalObject, state) == -1) {
        takeRaisedException(vm);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

PYTHON_NATIVE(bufferedNext)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfBuffered(self);
    CHECK_INITIALIZED();
    auto& io = ioState(globalObject);
    PyType* type = typeOf(globalObject, self);
    JSValue line;
    if (type == io.bufferedReader.get() || type == io.bufferedRandom.get())
        line = readLine(globalObject, self, state, -1);
    else {
        line = callMethodNamed(globalObject, self, names.attribute_readline);
        if (line && !isInstance(globalObject, line, realm->typeBytes()))
            return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, concatenate("readline() should have returned a bytes object, not '"_s, typeName(globalObject, line), '\'')));
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (builtinBufferOf(line)->empty())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    return JSValue::encode(line);
}

PYTHON_NATIVE(bufferedRepr)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    String type = typeName(globalObject, self);
    JSValue name = getAttributeIfPresent(globalObject, self, names.attribute_name);
    if (scope.exception()) {
        // If the raw stream has been detached, that is nothing to make anything of.
        if (!catchException(globalObject, BuiltinType::ValueError))
            return { };
        name = { };
    }
    if (!name)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', type, '>'))));
    ReprGuard guard(globalObject, self.asCell());
    if (guard.isRecursive())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("reentrant call inside "_s, type, ".__repr__"_s)));
    String shown = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', type, " name="_s, shown, '>'))));
}

// ---- What makes each what it is

enum class BufferedKind : uint8_t { Reader, Writer, Random };

// BufferedReader(raw, buffer_size=DEFAULT_BUFFER_SIZE), and the other two
PYTHON_NATIVE(bufferedInit)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfBuffered(self);
    auto kind = unpack<BufferedKind>(callFrame, 0);
    JSValue raw = args.at(1);
    int64_t bufferSize = defaultBufferSize;
    if (JSValue value = args.at(2)) {
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        bufferSize = *given;
    }
    state.isInitialized = false;
    state.isDetached = false;
    if (kind == BufferedKind::Random && !checkIsSeekable(globalObject, raw))
        return { };
    if (kind != BufferedKind::Writer && !checkIsReadable(globalObject, raw))
        return { };
    if (kind != BufferedKind::Reader && !checkIsWritable(globalObject, raw))
        return { };
    state.raw.set(vm, self.asCell(), raw);
    state.bufferSize = bufferSize;
    state.isReadable = kind != BufferedKind::Writer;
    state.isWritable = kind != BufferedKind::Reader;
    if (!initializeBuffer(globalObject, self.asCell(), state))
        return { };
    if (state.isReadable)
        state.resetReadBuffer();
    if (state.isWritable) {
        state.resetWriteBuffer();
        state.position = 0;
    }
    auto& io = ioState(globalObject);
    PyType* plain = kind == BufferedKind::Reader ? io.bufferedReader.get() : kind == BufferedKind::Writer ? io.bufferedWriter.get() : io.bufferedRandom.get();
    state.hasFastClosedChecks = typeOf(globalObject, self) == plain && typeOf(globalObject, raw) == io.fileIO.get();
    state.isInitialized = true;
    RETURN_NONE();
}

PYTHON_NATIVE(bufferedWrite)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfBuffered(self);
    Buffer buffer = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    CHECK_INITIALIZED();
    BusyScope busy(globalObject, self, state);
    if (!busy)
        return { };
    bool closed = isClosed(globalObject, state);
    RETURN_IF_EXCEPTION(scope, { });
    if (closed)
        return JSValue::encode(raiseValueError(globalObject, scope, "write to closed file"_s));
    int64_t length = static_cast<int64_t>(buffer.size());

    // If it all fits in the buffer, that is all there is to it.
    if (!state.hasValidReadBuffer() && !state.hasValidWriteBuffer()) {
        state.position = 0;
        state.rawPosition = 0;
    }
    int64_t available = state.bufferSize - state.position;
    if (length <= available && length < state.bufferSize) {
        memcpySpan(state.bytes(state.position, length), buffer.span());
        if (!state.hasValidWriteBuffer() || state.writePosition > state.position)
            state.writePosition = state.position;
        state.adjustPosition(state.position + length);
        if (state.position > state.writeEnd)
            state.writeEnd = state.position;
        return JSValue::encode(intFromInt64(globalObject, length));
    }

    // What runs from here on can be a program's, and can change what is being written. It is what it was when this was called.
    JSUint8Array* data = newBytes(globalObject, buffer.span());
    RETURN_IF_EXCEPTION(scope, { });
    auto dataBytes = [&] (int64_t from, int64_t count) { return data->span().subspan(static_cast<size_t>(from), static_cast<size_t>(count)); };

    // First what is in the buffer is sent on.
    if (!flushUnlocked(globalObject, state)) {
        if (!hasRaisedBlockingIOError(globalObject))
            return { };
        if (state.isReadable)
            state.resetReadBuffer();
        // Room is made by moving what is left to the front.
        memmoveSpan(state.bytes(0, state.writeEnd - state.writePosition), state.bytes(state.writePosition, state.writeEnd - state.writePosition));
        state.writeEnd -= state.writePosition;
        state.rawPosition -= state.writePosition;
        state.position -= state.writePosition;
        state.writePosition = 0;
        available = state.bufferSize - state.writeEnd;
        if (length <= available) {
            takeRaisedException(vm);
            RETURN_IF_EXCEPTION(scope, { });
            memcpySpan(state.bytes(state.writeEnd, length), dataBytes(0, length));
            state.writeEnd += length;
            state.position += length;
            return JSValue::encode(intFromInt64(globalObject, length));
        }
        // As much as there is room for.
        memcpySpan(state.bytes(state.writeEnd, available), dataBytes(0, available));
        state.writeEnd += available;
        state.position += available;
        raiseBlockingIOError(globalObject, errno, "write could not complete without blocking"_s, available);
        return { };
    }

    // The raw stream may be somewhere other than where the program is, if the buffer was read into and never written to.
    if (int64_t offset = state.rawOffset()) {
        if (rawSeek(globalObject, state, -offset, 1) < 0)
            return { };
        state.rawPosition -= offset;
    }

    // Then what was given, the buffer being empty now.
    int64_t remaining = length;
    int64_t written = 0;
    while (remaining >= state.bufferSize) {
        int errorNumber = 0;
        int64_t count = rawWrite(globalObject, state, data, written, length - written, errorNumber);
        if (count == failed)
            return { };
        if (count == wouldHaveBlocked) {
            if (remaining > state.bufferSize) {
                // It does not all fit in the buffer. As much as does is kept.
                memcpySpan(state.bytes(0, state.bufferSize), dataBytes(written, state.bufferSize));
                state.rawPosition = 0;
                state.adjustPosition(state.bufferSize);
                state.writeEnd = state.bufferSize;
                written += state.bufferSize;
                raiseBlockingIOError(globalObject, errorNumber, "write could not complete without blocking"_s, written);
                return { };
            }
            break;
        }
        written += count;
        remaining -= count;
        if (!checkSignals(globalObject))
            return { };
    }
    if (state.isReadable)
        state.resetReadBuffer();
    if (remaining > 0) {
        memcpySpan(state.bytes(0, remaining), dataBytes(written, remaining));
        written += remaining;
    }
    state.writePosition = 0;
    state.writeEnd = remaining;
    state.adjustPosition(remaining);
    state.rawPosition = 0;
    return JSValue::encode(intFromInt64(globalObject, written));
}

// ---- BufferedRWPair

// BufferedRWPair(reader, writer, buffer_size=DEFAULT_BUFFER_SIZE, /)
PYTHON_NATIVE(pairInit)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<PairState>(self);
    JSValue bufferSize = jsNumber(static_cast<int32_t>(defaultBufferSize));
    if (JSValue value = args.at(3)) {
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        bufferSize = intFromInt64(globalObject, *given);
    }
    if (!checkIsReadable(globalObject, args.at(1)) || !checkIsWritable(globalObject, args.at(2)))
        return { };
    auto& io = ioState(globalObject);
    JSValue reader = call(globalObject, io.bufferedReader.get(), args.at(1), bufferSize);
    RETURN_IF_EXCEPTION(scope, { });
    state.reader.set(vm, self, reader);
    JSValue writer = call(globalObject, io.bufferedWriter.get(), args.at(2), bufferSize);
    if (scope.exception()) {
        state.reader.clear();
        return { };
    }
    state.writer.set(vm, self, writer);
    RETURN_NONE();
}

// _forward_call()
static JSValue forwardCall(JSGlobalObject* globalObject, JSValue target, const Identifier& name, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!target)
        return raiseValueError(globalObject, scope, "I/O operation on uninitialized object"_s);
    JSValue function = getAttribute(globalObject, target, name);
    if (scope.exception()) {
        if (!scope.tryClearException())
            return { };
        return raise(globalObject, scope, BuiltinType::AttributeError, jsString(vm, name.string()));
    }
    RELEASE_AND_RETURN(scope, call(globalObject, function, arguments));
}

enum class PairForward : uint8_t { Read, Peek, Read1, ReadInto, ReadInto1, Write, Flush, Readable, Writable };

PYTHON_NATIVE(pairForward)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<PairState>(args[0]);
    auto which = unpack<PairForward>(callFrame, 0);
    struct Forwarded {
        const Identifier& name;
        bool isToWriter;
        ASCIILiteral qualified;
    };
    const Forwarded all[] = {
        { names.attribute_read, false, "BufferedRWPair.read"_s }, { names.attribute_peek, false, "BufferedRWPair.peek"_s }, { names.attribute_read1, false, "BufferedRWPair.read1"_s },
        { names.attribute_readinto, false, "BufferedRWPair.readinto"_s }, { names.attribute_readinto1, false, "BufferedRWPair.readinto1"_s }, { names.attribute_write, true, "BufferedRWPair.write"_s },
        { names.attribute_flush, true, { } }, { names.attribute_readable, false, { } }, { names.attribute_writable, true, { } },
    };
    auto& forwarded = all[static_cast<unsigned>(which)];
    if (!forwarded.qualified.isNull() && !args.checkNoKeywords(globalObject, scope, forwarded.qualified))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(forwardCall(globalObject, forwarded.isToWriter ? state.writer.get() : state.reader.get(), forwarded.name, args.allFrom(1))));
}

PYTHON_NATIVE(pairClose)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<PairState>(args[0]);
    forwardCall(globalObject, state.writer.get(), names.attribute_close, ArgList());
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = forwardCall(globalObject, state.reader.get(), names.attribute_close, ArgList());
    chainRaisedExceptions(globalObject, raised);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(pairIsATTY)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<PairState>(args[0]);
    JSValue result = forwardCall(globalObject, state.writer.get(), names.attribute_isatty, ArgList());
    RETURN_IF_EXCEPTION(scope, { });
    if (!result.isFalse())
        return JSValue::encode(result);
    RELEASE_AND_RETURN(scope, JSValue::encode(forwardCall(globalObject, state.reader.get(), names.attribute_isatty, ArgList())));
}

static JSValue getPairClosed(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = stateOf<PairState>(self);
    if (!state.writer)
        return raise(globalObject, scope, BuiltinType::RuntimeError, "the BufferedRWPair object is being garbage-collected"_s);
    RELEASE_AND_RETURN(scope, getAttribute(globalObject, state.writer.get(), vm.pythonNames().attribute_closed));
}

void initializeBufferedIO(JSGlobalObject* globalObject, IOModuleState& io)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    constexpr auto ofTheClass = PyNativeFunction::Arguments::AreThoseOfTheClass;
    constexpr auto notChecked = PyNativeFunction::Arguments::AreNotChecked;
    constexpr auto withDefiningClass = PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass;
    auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name, PyType::Allocator allocator) {
        PyType* type = createBuiltinType(globalObject, name, io.bufferedIOBase.get(), PyType::Layout::Native, PyType::IsBaseType);
        slot.set(vm, realm, type);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        type->setAllocator(allocator);
        return type;
    };
    PyType::Allocator allocateBuffered = [] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<BufferedState>()); };

    struct Class {
        WriteBarrier<PyType>& slot;
        ASCIILiteral name;
        BufferedKind kind;
    };
    for (auto [slot, name, kind] : { Class { io.bufferedWriter, "_io.BufferedWriter"_s, BufferedKind::Writer }, Class { io.bufferedReader, "_io.BufferedReader"_s, BufferedKind::Reader }, Class { io.bufferedRandom, "_io.BufferedRandom"_s, BufferedKind::Random } }) {
        PyType* type = make(slot, name, allocateBuffered);
        bool reads = kind != BufferedKind::Writer;
        bool writes = kind != BufferedKind::Reader;
        addMethods(globalObject, type, {
            { "__init__"_s, bufferedInit, Kind::Wrapper, pack(kind), { }, ofTheClass },
            { "__repr__"_s, bufferedRepr },
            { "close"_s, bufferedClose },
            { "detach"_s, bufferedDetach },
            { "seekable"_s, bufferedForward, Kind::Method, pack(BufferedForward::Seekable) },
            { "fileno"_s, bufferedForward, Kind::Method, pack(BufferedForward::Fileno) },
            { "isatty"_s, bufferedForward, Kind::Method, pack(BufferedForward::IsATTY) },
            { "_dealloc_warn"_s, bufferedDeallocWarn },
            { "seek"_s, bufferedSeek },
            { "tell"_s, bufferedTell },
            { "truncate"_s, bufferedTruncate, Kind::Method, 0, { }, withDefiningClass },
            { "__sizeof__"_s, bufferedSizeOf },
            { "__getstate__"_s, ioCannotPickle },
        });
        if (reads) {
            addMethods(globalObject, type, {
                { "__next__"_s, bufferedNext },
                { "readable"_s, bufferedForward, Kind::Method, pack(BufferedForward::Readable) },
                { "read"_s, bufferedRead },
                { "peek"_s, bufferedPeek },
                { "read1"_s, bufferedRead1 },
                { "readinto"_s, bufferedReadInto, Kind::Method, pack(false) },
                { "readinto1"_s, bufferedReadInto, Kind::Method, pack(true) },
                { "readline"_s, bufferedReadLine },
            });
        }
        if (writes) {
            addMethods(globalObject, type, {
                { "writable"_s, bufferedForward, Kind::Method, pack(BufferedForward::Writable) },
                { "write"_s, bufferedWrite },
                { "flush"_s, bufferedFlush },
            });
        } else
            addMethods(globalObject, type, { { "flush"_s, bufferedForward, Kind::Method, pack(BufferedForward::Flush) } });
        addGetSet(globalObject, type, "closed"_s, getFromRaw<&CommonNames::attribute_closed>);
        addGetSet(globalObject, type, "name"_s, getFromRaw<&CommonNames::attribute_name>);
        addGetSet(globalObject, type, "mode"_s, getFromRaw<&CommonNames::attribute_mode>);
        addMember(globalObject, type, "raw"_s, [] (JSGlobalObject*, JSValue self) -> JSValue {
            JSValue raw = stateOfBuffered(self).raw.get();
            return raw ? raw : jsUndefined();
        });
        addMember(globalObject, type, "_finalizing"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfBuffered(self).isFinalizing); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
            toBoolMember(globalObject, value, stateOfBuffered(self).isFinalizing);
        });
    }

    PyType* pair = make(io.bufferedRWPair, "_io.BufferedRWPair"_s, [] (VM& vm, Structure* structure) -> JSObject* { return PyStateObject::create(vm, structure, makeUnique<PairState>()); });
    constexpr auto anyPositional = "($self, /, *args)"_s;
    addMethods(globalObject, pair, {
        { "__init__"_s, pairInit, Kind::Wrapper, 0, { }, ofTheClass },
        { "read"_s, pairForward, Kind::Method, pack(PairForward::Read), anyPositional, notChecked },
        { "peek"_s, pairForward, Kind::Method, pack(PairForward::Peek), anyPositional, notChecked },
        { "read1"_s, pairForward, Kind::Method, pack(PairForward::Read1), anyPositional, notChecked },
        { "readinto"_s, pairForward, Kind::Method, pack(PairForward::ReadInto), anyPositional, notChecked },
        { "readinto1"_s, pairForward, Kind::Method, pack(PairForward::ReadInto1), anyPositional, notChecked },
        { "write"_s, pairForward, Kind::Method, pack(PairForward::Write), anyPositional, notChecked },
        { "flush"_s, pairForward, Kind::Method, pack(PairForward::Flush) },
        { "readable"_s, pairForward, Kind::Method, pack(PairForward::Readable) },
        { "writable"_s, pairForward, Kind::Method, pack(PairForward::Writable) },
        { "close"_s, pairClose },
        { "isatty"_s, pairIsATTY },
    });
    addGetSet(globalObject, pair, "closed"_s, getPairClosed);
}

} } // namespace JSC::Python
