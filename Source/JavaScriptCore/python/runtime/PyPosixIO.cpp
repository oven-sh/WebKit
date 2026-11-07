/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include "PyEventWait.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <termios.h>

#if OS(LINUX)
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#endif

#if OS(DARWIN)
#include <copyfile.h>
#include <sys/socket.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The functions of the module posix that take a file descriptor, or make one.

static JSValue posixNoMemory(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).MemoryError(), String());
}

// The most a single read() or write() is asked for.
#if OS(DARWIN)
static constexpr size_t posixMaxTransfer = INT_MAX;
#else
static constexpr size_t posixMaxTransfer = SSIZE_MAX;
#endif

// ---- Opening and closing ----

static JSValue posixOpen(JSGlobalObject* globalObject, JSValue pathValue, JSValue flagsValue, JSValue modeValue, JSValue dirFdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixPath path("open"_s, PosixPath::AllowVirtual);
    int flags;
    int mode = 0777;
    int dirFd;
    if (failed(scope, path.convert(globalObject, pathValue)) || failed(scope, posixToInt(globalObject, flagsValue, flags)) || (modeValue && failed(scope, posixToInt(globalObject, modeValue, mode)))
        || failed(scope, posixToDirFd(globalObject, dirFdValue, dirFd)))
        return { };
    flags |= O_CLOEXEC;
    if (failed(scope, audit(globalObject, "open"_s, path.object(), jsUndefined(), jsNumber(flags))))
        return { };
    writeOutPendingOutput(globalObject);
    int fd;
    bool hasFreedDescriptors = false;
    do {
        if (path.isVirtual())
            fd = virtualOpen(path.virtualPath(), flags);
        else {
            AllowThreads allow(globalObject, AllowThreads::LosesNothing);
            fd = dirFd != AT_FDCWD ? openat(dirFd, path.narrow(), flags, mode) : open(path.narrow(), flags, mode);
        }
    } while (fd < 0 && ((errno == EINTR && !failed(scope, checkSignals(globalObject))) || (!std::exchange(hasFreedDescriptors, true) && posixFreeDescriptors(globalObject))));
    if (fd < 0)
        RELEASE_AND_RETURN(scope, path.error(globalObject));
    return jsNumber(fd);
}

static JSValue posixClose(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    // Not tried again after EINTR: the descriptor may be closed already, and be someone else's by now.
    int result;
    {
        AllowThreads allow(globalObject);
        result = close(fd);
    }
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue posixCloserange(JSGlobalObject* globalObject, JSValue lowValue, JSValue highValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int low;
    int high;
    if (failed(scope, posixToInt(globalObject, lowValue, low)) || failed(scope, posixToInt(globalObject, highValue, high)))
        return { };
    low = std::max(low, 0);
    if (high <= low)
        return jsUndefined();
#if OS(LINUX) && defined(SYS_close_range)
    if (!syscall(SYS_close_range, static_cast<unsigned>(low), static_cast<unsigned>(high - 1), 0u))
        return jsUndefined();
#endif
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit > 0 && high > limit)
        high = static_cast<int>(limit);
    for (int fd = low; fd < high; ++fd)
        close(fd);
    return jsUndefined();
}

static JSValue posixDup(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    int result = posixDupNotInheritable(fd);
    if (result < 0 && posixFreeDescriptors(globalObject))
        result = posixDupNotInheritable(fd);
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsNumber(result);
}

static JSValue posixDup2(JSGlobalObject* globalObject, JSValue fdValue, JSValue fd2Value, JSValue inheritableValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    int fd2;
    bool inheritable;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToInt(globalObject, fd2Value, fd2)) || failed(scope, posixToBool(globalObject, inheritableValue, true, inheritable)))
        return { };
#if OS(LINUX)
    int result = inheritable ? dup2(fd, fd2) : dup3(fd, fd2, O_CLOEXEC);
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
#else
    int result = dup2(fd, fd2);
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    if (!inheritable && !posixSetInheritable(fd2, false)) {
        JSValue error = posixError(globalObject);
        close(fd2);
        return error;
    }
#endif
    return jsNumber(result);
}

static JSValue posixPipe(JSGlobalObject* globalObject)
{
    int fds[2];
#if OS(LINUX)
    if (pipe2(fds, O_CLOEXEC) && (!posixFreeDescriptors(globalObject) || pipe2(fds, O_CLOEXEC)))
        return posixError(globalObject);
#else
    if (pipe(fds) && (!posixFreeDescriptors(globalObject) || pipe(fds)))
        return posixError(globalObject);
    if (!posixSetInheritable(fds[0], false) || !posixSetInheritable(fds[1], false)) {
        JSValue error = posixError(globalObject);
        close(fds[0]);
        close(fds[1]);
        return error;
    }
#endif
    return PyTuple::create(globalObject, { jsNumber(fds[0]), jsNumber(fds[1]) });
}

#if OS(LINUX)
static JSValue posixPipe2(JSGlobalObject* globalObject, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int flags;
    if (failed(scope, posixToInt(globalObject, flagsValue, flags)))
        return { };
    int fds[2];
    if (pipe2(fds, flags))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { jsNumber(fds[0]), jsNumber(fds[1]) }));
}
#endif

// ---- Reading and writing ----

static JSValue posixLseek(JSGlobalObject* globalObject, JSValue fdValue, JSValue positionValue, JSValue whenceValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    off_t position;
    int whence;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToOffset(globalObject, positionValue, position)) || failed(scope, posixToInt(globalObject, whenceValue, whence)))
        return { };
    switch (whence) {
    case 0:
        whence = SEEK_SET;
        break;
    case 1:
        whence = SEEK_CUR;
        break;
    case 2:
        whence = SEEK_END;
        break;
    }
    off_t result = lseek(fd, position, whence);
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<int64_t>(result)));
}

// function(buffer, size) reads; what it read becomes bytes. In a frame of its own: os.read() waits before it comes here, and is not to wait with
// the room for the buffer in its frame, where what earlier frames left there is under the eyes of the collector.
template<typename Function>
static NEVER_INLINE JSValue posixReadBytes(JSGlobalObject* globalObject, int64_t length, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    if (length < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject, EINVAL));
    Vector<uint8_t, 8192> buffer;
    if (!buffer.tryGrow(static_cast<size_t>(length)))
        RELEASE_AND_RETURN(scope, posixNoMemory(globalObject));
    ssize_t count;
    do {
        AllowThreads allow(globalObject, AllowThreads::LosesNothing);
        count = function(buffer.mutableSpan().data(), buffer.size());
    } while (count < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (count < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, buffer.span().first(static_cast<size_t>(count))));
}

static JSValue posixRead(JSGlobalObject* globalObject, JSValue fdValue, JSValue lengthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int64_t length;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToSsize(globalObject, lengthValue, length)))
        return { };
    if (length > 0 && static_cast<size_t>(length) > posixMaxTransfer)
        length = static_cast<int64_t>(posixMaxTransfer);
    if (length > 0) {
        writeOutPendingOutput(globalObject);
        if (failed(scope, waitUntilReadable(globalObject, fd)))
            return { };
    }
    RELEASE_AND_RETURN(scope, posixReadBytes(globalObject, length, [&](uint8_t* buffer, size_t size) { return read(fd, buffer, size); }));
}

static JSValue posixPread(JSGlobalObject* globalObject, JSValue fdValue, JSValue lengthValue, JSValue offsetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int64_t length;
    off_t offset;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToSsize(globalObject, lengthValue, length)) || failed(scope, posixToOffset(globalObject, offsetValue, offset)))
        return { };
    RELEASE_AND_RETURN(scope, posixReadBytes(globalObject, length, [&](uint8_t* buffer, size_t size) { return pread(fd, buffer, size, offset); }));
}

static JSValue posixReadinto(JSGlobalObject* globalObject, JSValue fdValue, JSValue bufferValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    BufferHolder holder(globalObject);
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixGetWritableBuffer(globalObject, bufferValue, "readinto"_s, 2, holder)))
        return { };
    ssize_t count;
    do {
        if (failed(scope, refreshBuffer(globalObject, holder.buffer)))
            return { };
        auto buffer = holder.buffer.mutableSpan();
        AllowThreads allow(globalObject, AllowThreads::LosesNothing);
        count = read(fd, buffer.data(), std::min(buffer.size(), posixMaxTransfer));
    } while (count < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (count < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

static JSValue posixWrite(JSGlobalObject* globalObject, JSValue fdValue, JSValue data)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    BufferHolder holder(globalObject);
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, holder.get(data)))
        return { };
    ssize_t count;
    do {
        if (failed(scope, refreshBuffer(globalObject, holder.buffer)))
            return { };
        auto bytes = holder.buffer.span();
        AllowThreads allow(globalObject);
        count = write(fd, bytes.data(), std::min(bytes.size(), posixMaxTransfer));
    } while (count < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (count < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

static JSValue posixPwrite(JSGlobalObject* globalObject, JSValue fdValue, JSValue data, JSValue offsetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    BufferHolder holder(globalObject);
    off_t offset;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, holder.get(data)) || failed(scope, posixToOffset(globalObject, offsetValue, offset)))
        return { };
    ssize_t count;
    do {
        if (failed(scope, refreshBuffer(globalObject, holder.buffer)))
            return { };
        auto bytes = holder.buffer.span();
        AllowThreads allow(globalObject);
        count = pwrite(fd, bytes.data(), bytes.size(), offset);
    } while (count < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (count < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

// A sequence of buffers, for the calls that take several at once.
class PosixBufferList {
    WTF_MAKE_NONCOPYABLE(PosixBufferList);
public:
    explicit PosixBufferList(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
    {
    }

    ~PosixBufferList()
    {
        for (auto& buffer : m_buffers)
            releaseBuffer(m_globalObject, buffer);
    }

    // False: an exception. `complaint` is what to say of what is not a sequence.
    bool gather(JSValue sequence, ASCIILiteral complaint, bool writable)
    {
        JSGlobalObject* globalObject = m_globalObject;
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!posixIsSequence(globalObject, sequence)) {
            raiseTypeError(globalObject, scope, complaint);
            return false;
        }
        int64_t count = length(globalObject, sequence);
        RETURN_IF_EXCEPTION(scope, false);
        for (int64_t i = 0; i < count; ++i) {
            JSValue item = getItem(globalObject, sequence, makeInt(globalObject, i));
            RETURN_IF_EXCEPTION(scope, false);
            Buffer buffer;
            if (failed(scope, getBuffer(globalObject, item, buffer, writable ? BufferWritable : BufferSimple)))
                return false;
            // The vector is not where the collector looks.
            m_values.append(buffer.object);
            m_values.append(buffer.view ? buffer.view : jsUndefined());
            m_values.append(buffer.owner ? JSValue(buffer.owner) : jsUndefined());
            m_buffers.append(WTF::move(buffer));
        }
        // (Where they are is asked right before the call: refresh().)
        for (auto& buffer : m_buffers)
            m_vectors.append({ nullptr, static_cast<size_t>(buffer.length) });
        return true;
    }

    // See refreshBuffer(). Getting the later ones may have run code too.
    bool refresh()
    {
        auto scope = DECLARE_THROW_SCOPE(getVM(m_globalObject));
        for (size_t i = 0; i < m_buffers.size(); ++i) {
            if (failed(scope, refreshBuffer(m_globalObject, m_buffers[i])))
                return false;
            m_vectors[i].iov_base = m_buffers[i].data;
        }
        return true;
    }

    struct iovec* vectors() { return m_vectors.mutableSpan().data(); }
    int count() const { return static_cast<int>(m_vectors.size()); }
    bool isEmpty() const { return m_vectors.isEmpty(); }
    size_t totalSize() const
    {
        size_t total = 0;
        for (auto& vector : m_vectors)
            total += vector.iov_len;
        return total;
    }

private:
    JSGlobalObject* m_globalObject;
    Vector<Buffer, 4> m_buffers;
    Vector<struct iovec, 8> m_vectors;
    MarkedArgumentBuffer m_values;
};

template<typename Function>
static JSValue posixTransfer(JSGlobalObject* globalObject, PosixBufferList* buffers, const Function& function, AllowThreads::WhenInterrupted whenInterrupted = AllowThreads::MayLoseSomething)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    ssize_t count;
    do {
        if (buffers && failed(scope, buffers->refresh()))
            return { };
        AllowThreads allow(globalObject, whenInterrupted);
        count = function();
    } while (count < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (count < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

static JSValue posixReadv(JSGlobalObject* globalObject, JSValue fdValue, JSValue buffersValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    PosixBufferList buffers(globalObject);
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, buffers.gather(buffersValue, "readv() arg 2 must be a sequence"_s, true)))
        return { };
    RELEASE_AND_RETURN(scope, posixTransfer(globalObject, &buffers, [&] { return readv(fd, buffers.vectors(), buffers.count()); }, AllowThreads::LosesNothing));
}

static JSValue posixWritev(JSGlobalObject* globalObject, JSValue fdValue, JSValue buffersValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    PosixBufferList buffers(globalObject);
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, buffers.gather(buffersValue, "writev() arg 2 must be a sequence"_s, false)))
        return { };
    RELEASE_AND_RETURN(scope, posixTransfer(globalObject, &buffers, [&] { return writev(fd, buffers.vectors(), buffers.count()); }));
}

[[maybe_unused]] static JSValue posixFlagsUnavailable(JSGlobalObject* globalObject, ASCIILiteral function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseNotImplementedError(globalObject, scope, function, ": flags unavailable on this platform"_s);
}

static JSValue posixPreadv(JSGlobalObject* globalObject, JSValue fdValue, JSValue buffersValue, JSValue offsetValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    off_t offset;
    int flags = 0;
    PosixBufferList buffers(globalObject);
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToOffset(globalObject, offsetValue, offset)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    if (failed(scope, buffers.gather(buffersValue, "preadv2() arg 2 must be a sequence"_s, true)))
        return { };
#if OS(LINUX) && defined(SYS_preadv2)
    // For this call, unlike preadv(), an offset of -1 is where the descriptor is.
    scope.release();
    return posixTransfer(globalObject, &buffers, [&] {
        ssize_t count = static_cast<ssize_t>(syscall(SYS_preadv2, fd, buffers.vectors(), buffers.count(), static_cast<long>(offset), 0L, flags));
        if (count < 0 && errno == ENOSYS && !flags)
            count = preadv(fd, buffers.vectors(), buffers.count(), offset);
        return count;
    }, AllowThreads::LosesNothing);
#else
    if (flags)
        RELEASE_AND_RETURN(scope, posixFlagsUnavailable(globalObject, "preadv2"_s));
    RELEASE_AND_RETURN(scope, posixTransfer(globalObject, &buffers, [&] { return preadv(fd, buffers.vectors(), buffers.count(), offset); }, AllowThreads::LosesNothing));
#endif
}

static JSValue posixPwritev(JSGlobalObject* globalObject, JSValue fdValue, JSValue buffersValue, JSValue offsetValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    off_t offset;
    int flags = 0;
    PosixBufferList buffers(globalObject);
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToOffset(globalObject, offsetValue, offset)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    if (failed(scope, buffers.gather(buffersValue, "pwritev() arg 2 must be a sequence"_s, false)))
        return { };
#if OS(LINUX) && defined(SYS_pwritev2)
    scope.release();
    return posixTransfer(globalObject, &buffers, [&] {
        ssize_t count = static_cast<ssize_t>(syscall(SYS_pwritev2, fd, buffers.vectors(), buffers.count(), static_cast<long>(offset), 0L, flags));
        if (count < 0 && errno == ENOSYS && !flags)
            count = pwritev(fd, buffers.vectors(), buffers.count(), offset);
        return count;
    });
#else
    if (flags)
        RELEASE_AND_RETURN(scope, posixFlagsUnavailable(globalObject, "pwritev2"_s));
    RELEASE_AND_RETURN(scope, posixTransfer(globalObject, &buffers, [&] { return pwritev(fd, buffers.vectors(), buffers.count(), offset); }));
#endif
}

// ---- From one descriptor to another ----

// An offset, or None for "where the descriptor is".
static bool posixToOptionalOffset(JSGlobalObject* globalObject, JSValue value, off_t& storage, off_t*& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = nullptr;
    if (!value || isNone(value))
        return true;
    if (failed(scope, posixToOffset(globalObject, value, storage)))
        return false;
    result = &storage;
    return true;
}

#if OS(LINUX)
static JSValue posixSendfile(JSGlobalObject* globalObject, JSValue outValue, JSValue inValue, JSValue offsetValue, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int out;
    int in;
    int64_t count;
    off_t storage;
    off_t* offset;
    if (failed(scope, posixToInt(globalObject, outValue, out)) || failed(scope, posixToInt(globalObject, inValue, in)) || failed(scope, posixToCount(globalObject, countValue, "count"_s, count))
        || failed(scope, posixToOptionalOffset(globalObject, offsetValue, storage, offset)))
        return { };
    RELEASE_AND_RETURN(scope, posixTransfer(globalObject, nullptr, [&] { return sendfile(out, in, offset, static_cast<size_t>(count)); }));
}

static JSValue posixCopyFileRange(JSGlobalObject* globalObject, JSValue sourceValue, JSValue destinationValue, JSValue countValue, JSValue sourceOffsetValue, JSValue destinationOffsetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int source;
    int destination;
    int64_t count;
    off_t sourceStorage;
    off_t destinationStorage;
    off_t* sourceOffset;
    off_t* destinationOffset;
    if (failed(scope, posixToInt(globalObject, sourceValue, source)) || failed(scope, posixToInt(globalObject, destinationValue, destination)) || failed(scope, posixToCount(globalObject, countValue, "count"_s, count))
        || failed(scope, posixToOptionalOffset(globalObject, sourceOffsetValue, sourceStorage, sourceOffset)) || failed(scope, posixToOptionalOffset(globalObject, destinationOffsetValue, destinationStorage, destinationOffset)))
        return { };
    RELEASE_AND_RETURN(scope, posixTransfer(globalObject, nullptr, [&] { return static_cast<ssize_t>(syscall(SYS_copy_file_range, source, sourceOffset, destination, destinationOffset, static_cast<size_t>(count), 0u)); }));
}

static JSValue posixSplice(JSGlobalObject* globalObject, JSValue sourceValue, JSValue destinationValue, JSValue countValue, JSValue sourceOffsetValue, JSValue destinationOffsetValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int source;
    int destination;
    int64_t count;
    off_t sourceStorage;
    off_t destinationStorage;
    off_t* sourceOffset;
    off_t* destinationOffset;
    unsigned flags = 0;
    if (failed(scope, posixToInt(globalObject, sourceValue, source)) || failed(scope, posixToInt(globalObject, destinationValue, destination)) || failed(scope, posixToCount(globalObject, countValue, "count"_s, count))
        || (flagsValue && failed(scope, posixToUnsigned(globalObject, flagsValue, flags)))
        || failed(scope, posixToOptionalOffset(globalObject, sourceOffsetValue, sourceStorage, sourceOffset)) || failed(scope, posixToOptionalOffset(globalObject, destinationOffsetValue, destinationStorage, destinationOffset)))
        return { };
    RELEASE_AND_RETURN(scope, posixTransfer(globalObject, nullptr, [&] { return splice(source, sourceOffset, destination, destinationOffset, static_cast<size_t>(count), flags); }));
}
#endif // OS(LINUX)

#if OS(DARWIN)
static JSValue posixSendfile(JSGlobalObject* globalObject, JSValue outValue, JSValue inValue, JSValue offsetValue, JSValue countValue, JSValue headersValue, JSValue trailersValue, JSValue flagsValue)
{
    writeOutPendingOutput(globalObject);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int out;
    int in;
    off_t offset;
    off_t byteCount;
    int flags = 0;
    if (failed(scope, posixToInt(globalObject, outValue, out)) || failed(scope, posixToInt(globalObject, inValue, in)) || failed(scope, posixToOffset(globalObject, offsetValue, offset))
        || failed(scope, posixToOffset(globalObject, countValue, byteCount)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    if (byteCount < 0)
        return raiseValueError(globalObject, scope, "count cannot be negative"_s);
    PosixBufferList headers(globalObject);
    PosixBufferList trailers(globalObject);
    if (headersValue && failed(scope, headers.gather(headersValue, "sendfile() headers must be a sequence"_s, false)))
        return { };
    if (trailersValue && failed(scope, trailers.gather(trailersValue, "sendfile() trailers must be a sequence"_s, false)))
        return { };
    struct sf_hdtr extra { };
    if (!headers.isEmpty()) {
        extra.headers = headers.vectors();
        extra.hdr_cnt = headers.count();
        // There, the count includes the headers.
        byteCount += static_cast<off_t>(headers.totalSize());
    }
    if (!trailers.isEmpty()) {
        extra.trailers = trailers.vectors();
        extra.trl_cnt = trailers.count();
    }
    int result;
    do {
        if (failed(scope, headers.refresh()) || failed(scope, trailers.refresh()))
            return { };
        AllowThreads allow(globalObject);
        result = sendfile(in, out, offset, &byteCount, &extra, flags);
    } while (result < 0 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (result < 0) {
        // Some of it may have been sent all the same.
        if ((errno == EAGAIN || errno == EBUSY) && byteCount)
            RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<int64_t>(byteCount)));
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    }
    RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<int64_t>(byteCount)));
}

static JSValue posixFcopyfile(JSGlobalObject* globalObject, JSValue inValue, JSValue outValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int in;
    int out;
    int flags;
    if (failed(scope, posixToInt(globalObject, inValue, in)) || failed(scope, posixToInt(globalObject, outValue, out)) || failed(scope, posixToInt(globalObject, flagsValue, flags)))
        return { };
    if (fcopyfile(in, out, nullptr, static_cast<copyfile_flags_t>(flags)) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}
#endif // OS(DARWIN)

// ---- What a descriptor refers to ----

static JSValue posixFstat(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    struct stat status;
    int result;
    do {
        result = fstat(fd, &status);
    } while (result && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (result)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixStatResult(globalObject, status));
}

// function(fd), again if a signal interrupts it.
template<typename Function>
static JSValue posixCallWithDescriptor(JSGlobalObject* globalObject, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int result;
    do {
        AllowThreads allow(globalObject);
        result = function();
    } while (result && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (result)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue posixFsync(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    RELEASE_AND_RETURN(scope, posixCallWithDescriptor(globalObject, [&] { return fsync(fd); }));
}

#if OS(LINUX)
static JSValue posixFdatasync(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    RELEASE_AND_RETURN(scope, posixCallWithDescriptor(globalObject, [&] { return fdatasync(fd); }));
}
#endif

static JSValue posixFchdir(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    if (failed(scope, audit(globalObject, "os.chdir"_s, jsNumber(fd))))
        return { };
    JSValue result = posixCallWithDescriptor(globalObject, [&] { return fchdir(fd); });
    RETURN_IF_EXCEPTION(scope, { });
    posixDidChangeDirectory(globalObject);
    return result;
}

static JSValue posixFchmod(JSGlobalObject* globalObject, JSValue fdValue, JSValue modeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int mode;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToInt(globalObject, modeValue, mode)))
        return { };
    if (failed(scope, audit(globalObject, "os.chmod"_s, jsNumber(fd), jsNumber(mode), jsNumber(-1))))
        return { };
    RELEASE_AND_RETURN(scope, posixCallWithDescriptor(globalObject, [&] { return fchmod(fd, static_cast<mode_t>(mode)); }));
}

static JSValue posixFchown(JSGlobalObject* globalObject, JSValue fdValue, JSValue uidValue, JSValue gidValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    uid_t uid;
    gid_t gid;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToUid(globalObject, uidValue, uid)) || failed(scope, posixToGid(globalObject, gidValue, gid)))
        return { };
    if (failed(scope, audit(globalObject, "os.chown"_s, jsNumber(fd), jsNumber(static_cast<uint32_t>(uid)), jsNumber(static_cast<uint32_t>(gid)), jsNumber(-1))))
        return { };
    RELEASE_AND_RETURN(scope, posixCallWithDescriptor(globalObject, [&] { return fchown(fd, uid, gid); }));
}

static JSValue posixFtruncate(JSGlobalObject* globalObject, JSValue fdValue, JSValue lengthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int fd;
    off_t length;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToOffset(globalObject, lengthValue, length)))
        return { };
    if (failed(scope, audit(globalObject, "os.truncate"_s, jsNumber(fd), lengthValue)))
        return { };
    RELEASE_AND_RETURN(scope, posixCallWithDescriptor(globalObject, [&] { return ftruncate(fd, length); }));
}

static JSValue posixLockf(JSGlobalObject* globalObject, JSValue fdValue, JSValue commandValue, JSValue lengthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int command;
    off_t length;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToInt(globalObject, commandValue, command)) || failed(scope, posixToOffset(globalObject, lengthValue, length)))
        return { };
    if (failed(scope, audit(globalObject, "os.lockf"_s, jsNumber(fd), jsNumber(command), lengthValue)))
        return { };
    int result;
    {
        AllowThreads allow(globalObject);
        result = lockf(fd, command, length);
    }
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

#if OS(LINUX)
// These two return the error rather than set errno.
template<typename Function>
static JSValue posixCallReturningError(JSGlobalObject* globalObject, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    writeOutPendingOutput(globalObject);
    int result;
    do {
        AllowThreads allow(globalObject);
        result = function();
        if (result == -1)
            result = errno;
    } while (result == EINTR && !failed(scope, checkSignals(globalObject)));
    if (result)
        RELEASE_AND_RETURN(scope, posixError(globalObject, result));
    return jsUndefined();
}

static JSValue posixFallocate(JSGlobalObject* globalObject, JSValue fdValue, JSValue offsetValue, JSValue lengthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    off_t offset;
    off_t length;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToOffset(globalObject, offsetValue, offset)) || failed(scope, posixToOffset(globalObject, lengthValue, length)))
        return { };
    RELEASE_AND_RETURN(scope, posixCallReturningError(globalObject, [&] { return posix_fallocate(fd, offset, length); }));
}

static JSValue posixFadvise(JSGlobalObject* globalObject, JSValue fdValue, JSValue offsetValue, JSValue lengthValue, JSValue adviceValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    off_t offset;
    off_t length;
    int advice;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToOffset(globalObject, offsetValue, offset)) || failed(scope, posixToOffset(globalObject, lengthValue, length))
        || failed(scope, posixToInt(globalObject, adviceValue, advice)))
        return { };
    RELEASE_AND_RETURN(scope, posixCallReturningError(globalObject, [&] { return posix_fadvise(fd, offset, length, advice); }));
}
#endif

// ---- Flags of a descriptor ----

static JSValue posixGetInheritable(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsBoolean(!(flags & FD_CLOEXEC));
}

static JSValue posixSetInheritableFunction(JSGlobalObject* globalObject, JSValue fdValue, JSValue inheritableValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int inheritable;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToInt(globalObject, inheritableValue, inheritable)))
        return { };
    if (!posixSetInheritable(fd, inheritable))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue posixGetBlocking(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsBoolean(!(flags & O_NONBLOCK));
}

static JSValue posixSetBlocking(JSGlobalObject* globalObject, JSValue fdValue, JSValue blockingValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    bool blocking;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToBool(globalObject, blockingValue, true, blocking)))
        return { };
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    int newFlags = blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK;
    if (newFlags != flags && fcntl(fd, F_SETFL, newFlags) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

// ---- Terminals ----

static JSValue posixIsatty(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    return jsBoolean(isatty(fd));
}

static JSValue posixTtyname(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    long size = sysconf(_SC_TTY_NAME_MAX);
    if (size == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    Vector<char, 256> buffer(static_cast<size_t>(size));
    if (int result = ttyname_r(fd, buffer.mutableSpan().data(), buffer.size()))
        RELEASE_AND_RETURN(scope, posixError(globalObject, result));
    RELEASE_AND_RETURN(scope, posixDecode(globalObject, buffer.span().data()));
}

static JSValue posixCtermid(JSGlobalObject* globalObject)
{
    char buffer[L_ctermid];
    if (!ctermid(buffer))
        return posixError(globalObject);
    return posixDecode(globalObject, buffer);
}

static JSValue posixTcgetpgrp(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    pid_t group = tcgetpgrp(fd);
    if (group < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsNumber(group);
}

static JSValue posixTcsetpgrp(JSGlobalObject* globalObject, JSValue fdValue, JSValue groupValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    pid_t group;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)) || failed(scope, posixToPid(globalObject, groupValue, group)))
        return { };
    if (tcsetpgrp(fd, group) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue posixOpenpt(JSGlobalObject* globalObject, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int flags;
    if (failed(scope, posixToInt(globalObject, flagsValue, flags)))
        return { };
    int fd = posix_openpt(flags | O_CLOEXEC);
    if (fd == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    // Not every system heeds the flag here.
    if (!posixSetInheritable(fd, false)) {
        JSValue error = posixError(globalObject);
        close(fd);
        return error;
    }
    return jsNumber(fd);
}

static JSValue posixGrantpt(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    if (grantpt(fd) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue posixUnlockpt(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    if (unlockpt(fd) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

// The name of the other end of a pseudo-terminal. False: errno says why.
static bool posixNameOfSlave(int fd, Vector<char, PATH_MAX + 1>& name)
{
    name.grow(PATH_MAX + 1);
#if OS(LINUX)
    if (int result = ptsname_r(fd, name.mutableSpan().data(), name.size())) {
        errno = result;
        return false;
    }
#else
    errno = 0;
    const char* result = ptsname(fd);
    if (!result)
        return false;
    strlcpy(name.mutableSpan().data(), result, name.size());
#endif
    return true;
}

static JSValue posixPtsname(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    Vector<char, PATH_MAX + 1> name;
    if (!posixNameOfSlave(fd, name))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixDecode(globalObject, name.span().data()));
}

static JSValue posixOpenpty(JSGlobalObject* globalObject)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0)
        return posixError(globalObject);
    Vector<char, PATH_MAX + 1> name;
    int slave = -1;
    if (posixSetInheritable(master, false) && !grantpt(master) && !unlockpt(master) && posixNameOfSlave(master, name))
        slave = open(name.span().data(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        JSValue error = posixError(globalObject);
        close(master);
        return error;
    }
    return PyTuple::create(globalObject, { jsNumber(master), jsNumber(slave) });
}

static JSValue posixLoginTty(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    // A new session, of which the terminal becomes the controlling one, and the three standard streams.
    if (setsid() == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    if (ioctl(fd, TIOCSCTTY, nullptr) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    if (dup2(fd, 0) == -1 || dup2(fd, 1) == -1 || dup2(fd, 2) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    if (fd > 2)
        close(fd);
    return jsUndefined();
}

// ---- Descriptors that are not files ----

#if OS(LINUX)
static JSValue posixMemfdCreate(JSGlobalObject* globalObject, JSValue nameValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    unsigned flags = 1; // MFD_CLOEXEC
    if (failed(scope, posixToFSBytes(globalObject, nameValue, name)) || (flagsValue && failed(scope, posixToUnsignedMask(globalObject, flagsValue, flags))))
        return { };
    int fd = static_cast<int>(syscall(SYS_memfd_create, name.characters(), flags));
    if (fd == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsNumber(fd);
}

static JSValue posixEventfd(JSGlobalObject* globalObject, JSValue initialValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned initial;
    int flags = EFD_CLOEXEC;
    if (failed(scope, posixToUnsigned(globalObject, initialValue, initial)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    int fd = eventfd(initial, flags);
    if (fd == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsNumber(fd);
}

static JSValue posixEventfdRead(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    eventfd_t value;
    if (eventfd_read(fd, &value) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<uint64_t>(value)));
}

static JSValue posixEventfdWrite(JSGlobalObject* globalObject, JSValue fdValue, JSValue valueValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    uint64_t value;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || failed(scope, posixToUnsignedLongLong(globalObject, valueValue, value)))
        return { };
    if (eventfd_write(fd, value) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue posixTimerfdCreate(JSGlobalObject* globalObject, JSValue clockValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int clock;
    int flags = 0;
    if (failed(scope, posixToInt(globalObject, clockValue, clock)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    int fd = timerfd_create(clock, flags | TFD_CLOEXEC);
    if (fd == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsNumber(fd);
}

static struct timespec posixTimespecFromNanoseconds(int64_t nanoseconds)
{
    int64_t seconds = nanoseconds / 1000000000;
    int64_t rest = nanoseconds % 1000000000;
    if (rest < 0) {
        rest += 1000000000;
        --seconds;
    }
    return { static_cast<time_t>(seconds), static_cast<long>(rest) };
}

// Seconds, as a float or anything that becomes one; absent is 0.
static bool posixSecondsToTimespec(JSGlobalObject* globalObject, JSValue value, struct timespec& result)
{
    result = { 0, 0 };
    if (!value)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    double seconds = toDouble(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    double nanoseconds = std::floor(seconds * 1e9);
    constexpr double limit = 9223372036854775808.0;
    if (!(nanoseconds >= -limit && nanoseconds < limit)) {
        raiseOverflowError(globalObject, scope, "timestamp out of range for C PyTime_t"_s);
        return false;
    }
    result = posixTimespecFromNanoseconds(static_cast<int64_t>(nanoseconds));
    return true;
}

static JSValue posixTimerSpecInSeconds(JSGlobalObject* globalObject, const struct itimerspec& spec)
{
    auto inSeconds = [&](const struct timespec& time) { return makeFloat(globalObject, static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9); };
    return PyTuple::create(globalObject, { inSeconds(spec.it_value), inSeconds(spec.it_interval) });
}

static JSValue posixTimerSpecInNanoseconds(JSGlobalObject* globalObject, const struct itimerspec& spec)
{
    auto inNanoseconds = [&](const struct timespec& time) { return makeInt(globalObject, static_cast<int64_t>(time.tv_sec) * 1000000000 + time.tv_nsec); };
    return PyTuple::create(globalObject, { inNanoseconds(spec.it_value), inNanoseconds(spec.it_interval) });
}

static JSValue posixTimerfdSettime(JSGlobalObject* globalObject, JSValue fdValue, JSValue flagsValue, JSValue initialValue, JSValue intervalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int flags = 0;
    struct itimerspec newSpec;
    struct itimerspec oldSpec;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags)))
        || failed(scope, posixSecondsToTimespec(globalObject, initialValue, newSpec.it_value)) || failed(scope, posixSecondsToTimespec(globalObject, intervalValue, newSpec.it_interval)))
        return { };
    if (timerfd_settime(fd, flags, &newSpec, &oldSpec) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixTimerSpecInSeconds(globalObject, oldSpec));
}

static JSValue posixTimerfdSettimeNs(JSGlobalObject* globalObject, JSValue fdValue, JSValue flagsValue, JSValue initialValue, JSValue intervalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int flags = 0;
    int64_t initial = 0;
    int64_t interval = 0;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags)))
        || (initialValue && failed(scope, posixToLongLong(globalObject, initialValue, initial))) || (intervalValue && failed(scope, posixToLongLong(globalObject, intervalValue, interval))))
        return { };
    struct itimerspec newSpec { posixTimespecFromNanoseconds(interval), posixTimespecFromNanoseconds(initial) };
    struct itimerspec oldSpec;
    if (timerfd_settime(fd, flags, &newSpec, &oldSpec) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixTimerSpecInNanoseconds(globalObject, oldSpec));
}

static JSValue posixTimerfdGettime(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    struct itimerspec spec;
    if (timerfd_gettime(fd, &spec) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixTimerSpecInSeconds(globalObject, spec));
}

static JSValue posixTimerfdGettimeNs(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    struct itimerspec spec;
    if (timerfd_gettime(fd, &spec) == -1)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixTimerSpecInNanoseconds(globalObject, spec));
}
#endif // OS(LINUX)

// ---- The table ----

static constexpr NativeDef posixDescriptorFunctions[] = {
    PY_FUNCTION("open", posixOpen, "(path, flags, mode=511, *, dir_fd=None)"),
    PY_FUNCTION("close", posixClose, "(fd)"),
    PY_FUNCTION("closerange", posixCloserange, "(fd_low, fd_high, /)"),
    PY_FUNCTION("dup", posixDup, "(fd, /)"),
    PY_FUNCTION("dup2", posixDup2, "(fd, fd2, inheritable=True)"),
    PY_FUNCTION("pipe", posixPipe, "()"),
#if OS(LINUX)
    PY_FUNCTION("pipe2", posixPipe2, "(flags, /)"),
#endif
    PY_FUNCTION("lseek", posixLseek, "(fd, position, whence, /)"),
    NativeDef(NativeKind::Function, "read"_s, posixRead, "(fd, length, /)"_s, MayWait),
    PY_FUNCTION("pread", posixPread, "(fd, length, offset, /)"),
    PY_FUNCTION("readinto", posixReadinto, "(fd, buffer, /)"),
    PY_FUNCTION("write", posixWrite, "(fd, data, /)"),
    PY_FUNCTION("pwrite", posixPwrite, "(fd, buffer, offset, /)"),
    PY_FUNCTION("readv", posixReadv, "(fd, buffers, /)"),
    PY_FUNCTION("writev", posixWritev, "(fd, buffers, /)"),
    PY_FUNCTION("preadv", posixPreadv, "(fd, buffers, offset, flags=0, /)"),
    PY_FUNCTION("pwritev", posixPwritev, "(fd, buffers, offset, flags=0, /)"),
#if OS(LINUX)
    PY_FUNCTION("sendfile", posixSendfile, "(out_fd, in_fd, offset, count)"),
    PY_FUNCTION("copy_file_range", posixCopyFileRange, "(src, dst, count, offset_src=None, offset_dst=None)"),
    PY_FUNCTION("splice", posixSplice, "(src, dst, count, offset_src=None, offset_dst=None, flags=0)"),
#endif
#if OS(DARWIN)
    PY_FUNCTION("sendfile", posixSendfile, "(out_fd, in_fd, offset, count, headers=(), trailers=(), flags=0)"),
    PY_FUNCTION("_fcopyfile", posixFcopyfile, "(in_fd, out_fd, flags, /)"),
#endif
    PY_FUNCTION("fstat", posixFstat, "(fd)"),
    PY_FUNCTION("fsync", posixFsync, "(fd)"),
#if OS(LINUX)
    PY_FUNCTION("fdatasync", posixFdatasync, "(fd)"),
#endif
    PY_FUNCTION("fchdir", posixFchdir, "(fd)"),
    PY_FUNCTION("fchmod", posixFchmod, "(fd, mode)"),
    PY_FUNCTION("fchown", posixFchown, "(fd, uid, gid)"),
    PY_FUNCTION("ftruncate", posixFtruncate, "(fd, length, /)"),
    PY_FUNCTION("lockf", posixLockf, "(fd, command, length, /)"),
#if OS(LINUX)
    PY_FUNCTION("posix_fallocate", posixFallocate, "(fd, offset, length, /)"),
    PY_FUNCTION("posix_fadvise", posixFadvise, "(fd, offset, length, advice, /)"),
#endif
    PY_FUNCTION("get_inheritable", posixGetInheritable, "(fd, /)"),
    PY_FUNCTION("set_inheritable", posixSetInheritableFunction, "(fd, inheritable, /)"),
    PY_FUNCTION("get_blocking", posixGetBlocking, "(fd, /)"),
    PY_FUNCTION("set_blocking", posixSetBlocking, "(fd, blocking, /)"),
    PY_FUNCTION("isatty", posixIsatty, "(fd, /)"),
    PY_FUNCTION("ttyname", posixTtyname, "(fd, /)"),
    PY_FUNCTION("ctermid", posixCtermid, "()"),
    PY_FUNCTION("tcgetpgrp", posixTcgetpgrp, "(fd, /)"),
    PY_FUNCTION("tcsetpgrp", posixTcsetpgrp, "(fd, pgid, /)"),
    PY_FUNCTION("posix_openpt", posixOpenpt, "(oflag, /)"),
    PY_FUNCTION("grantpt", posixGrantpt, "(fd, /)"),
    PY_FUNCTION("unlockpt", posixUnlockpt, "(fd, /)"),
    PY_FUNCTION("ptsname", posixPtsname, "(fd, /)"),
    PY_FUNCTION("openpty", posixOpenpty, "()"),
    PY_FUNCTION("login_tty", posixLoginTty, "(fd, /)"),
#if OS(LINUX)
    PY_FUNCTION("memfd_create", posixMemfdCreate, "(name, flags=MFD_CLOEXEC)"),
    PY_FUNCTION("eventfd", posixEventfd, "(initval, flags=EFD_CLOEXEC)"),
    PY_FUNCTION("eventfd_read", posixEventfdRead, "(fd)"),
    PY_FUNCTION("eventfd_write", posixEventfdWrite, "(fd, value)"),
    PY_FUNCTION("timerfd_create", posixTimerfdCreate, "(clockid, /, *, flags=0)"),
    PY_FUNCTION("timerfd_settime", posixTimerfdSettime, "(fd, /, *, flags=0, initial=0.0, interval=0.0)"),
    PY_FUNCTION("timerfd_settime_ns", posixTimerfdSettimeNs, "(fd, /, *, flags=0, initial=0, interval=0)"),
    PY_FUNCTION("timerfd_gettime", posixTimerfdGettime, "(fd, /)"),
    PY_FUNCTION("timerfd_gettime_ns", posixTimerfdGettimeNs, "(fd, /)"),
#endif
};

void addPosixDescriptorFunctions(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, posixDescriptorFunctions);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
