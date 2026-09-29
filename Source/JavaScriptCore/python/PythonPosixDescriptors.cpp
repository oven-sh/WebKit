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
#include "PythonPosix.h"

#if OS(UNIX)

#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>
#include <wtf/text/StringToIntegerConversion.h>
#if OS(DARWIN)
#include <copyfile.h>
#include <sys/socket.h>
#else
#include <pty.h>
#include <sys/sendfile.h>
#include <sys/sysmacros.h>
#endif

#if OS(DARWIN)
// These are declared in <util.h>, which finds wtf/dragonbox/detail/util.h.
extern "C" int login_tty(int);
extern "C" int openpty(int*, int*, char*, struct termios*, struct winsize*);
#endif

// The functions of posix that take an open file: Modules/posixmodule.c of CPython.

namespace JSC { namespace Python {

int duplicateDescriptor(JSGlobalObject*, int);
bool truncateDescriptor(JSGlobalObject*, int, int64_t);
std::optional<dev_t> toDevice(JSGlobalObject*, JSValue);

#define CONVERT(variable, expression) \
    auto variable##Converted = expression; \
    RETURN_IF_EXCEPTION(scope, { }); \
    auto variable = *variable##Converted;
#define CONVERT_BOOL(variable, value, defaultValue) \
    bool variable = defaultValue; \
    if (JSValue given = value) { \
        variable = isTrue(globalObject, given); \
        RETURN_IF_EXCEPTION(scope, { }); \
    }
#define CONVERT_INT_OR(variable, value, defaultValue) \
    int variable = defaultValue; \
    if (JSValue given = value) { \
        auto converted = toCInt(globalObject, given); \
        RETURN_IF_EXCEPTION(scope, { }); \
        variable = *converted; \
    }

// What the host's file operations return is the result, or what errno would be, negated.
template<typename Result>
static Result withErrno(Result result)
{
    if (result >= 0)
        return result;
    errno = static_cast<int>(-result);
    return -1;
}

// _Py_set_inheritable(). False if it raised.
static bool setInheritable(JSGlobalObject* globalObject, int descriptor, bool isInheritable)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (withErrno(fileOperations(globalObject)->setInheritable(descriptor, isInheritable)) < 0) {
        raisePosixError(globalObject, scope);
        return false;
    }
    return true;
}

PYTHON_NATIVE(posixOpen)
{
    NATIVE_PROLOGUE();
    PathArgument path("open"_s, "path"_s);
    if (!path.convert(globalObject, args.at(0)))
        return { };
    CONVERT(flags, toCInt(globalObject, args.at(1)));
    CONVERT_INT_OR(mode, args.at(2), 0777);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(3)));
    flags |= O_CLOEXEC;
    if (!audit(globalObject, "open"_s, path.object, jsUndefined(), jsNumber(flags)))
        return { };
    int descriptor;
    if (!retryIfInterrupted(globalObject, descriptor, [&] { return directory != defaultDirectoryDescriptor ? ::openat(directory, path.narrow(), flags, mode) : withErrno(fileOperations(globalObject)->open(path.bytes, flags, mode)); }))
        return { };
    if (descriptor < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    return JSValue::encode(jsNumber(descriptor));
}

PYTHON_NATIVE(posixClose)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    // It is not tried again if it is interrupted: what the descriptor is by then is not known.
    if (withErrno(fileOperations(globalObject)->close(descriptor)) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixCloserange)
{
    NATIVE_PROLOGUE();
    CONVERT(low, toCInt(globalObject, args.at(0)));
    CONVERT(high, toCInt(globalObject, args.at(1)));
    auto* files = fileOperations(globalObject);
    low = std::max(low, 0);
    // Those that are open are listed, which saves trying each number there is. What is failed at is passed over.
    if (DIR* directory = ::opendir("/dev/fd")) {
        Vector<int> open;
        while (struct dirent* entry = ::readdir(directory)) {
            if (auto descriptor = parseInteger<int>(StringView::fromLatin1(entry->d_name)); descriptor && *descriptor >= low && *descriptor < high)
                open.append(*descriptor);
        }
        ::closedir(directory);
        for (int descriptor : open)
            files->close(descriptor);
        RETURN_NONE();
    }
    for (int descriptor = low; descriptor < high; ++descriptor)
        files->close(descriptor);
    RETURN_NONE();
}

PYTHON_NATIVE(posixDup)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    int result = duplicateDescriptor(globalObject, descriptor);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(result));
}

PYTHON_NATIVE(posixDup2)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(descriptor2, toCInt(globalObject, args.at(1)));
    CONVERT_BOOL(isInheritable, args.at(2), true);
    int result = ::dup2(descriptor, descriptor2);
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    if (!isInheritable && !setInheritable(globalObject, descriptor2, false)) {
        ::close(descriptor2);
        return { };
    }
    return JSValue::encode(jsNumber(result));
}

PYTHON_NATIVE(posixLockf)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(command, toCInt(globalObject, args.at(1)));
    CONVERT(length, toFileOffset(globalObject, args.at(2)));
    if (!audit(globalObject, "os.lockf"_s, jsNumber(descriptor), jsNumber(command), intFromInt64(globalObject, length)))
        return { };
    if (::lockf(descriptor, command, length) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixLseek)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(position, toFileOffset(globalObject, args.at(1)));
    CONVERT(whence, toCInt(globalObject, args.at(2)));
    int64_t result = withErrno(fileOperations(globalObject)->seek(descriptor, position, whence));
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, result));
}

// As much as is asked of the system at a time: _PY_READ_MAX and _PY_WRITE_MAX.
static constexpr size_t mostAtATime = std::numeric_limits<int>::max();

// _Py_read(). Less than nothing if it raised.
static int64_t readDescriptor(JSGlobalObject* globalObject, int descriptor, std::span<uint8_t> into)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t count;
    if (!retryIfInterrupted(globalObject, count, [&] { return withErrno(fileOperations(globalObject)->read(descriptor, into.first(std::min(into.size(), mostAtATime)))); }))
        return -1;
    if (count < 0)
        raisePosixError(globalObject, scope);
    return count;
}

PYTHON_NATIVE(posixRead)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(length, toSsize(globalObject, args.at(1)));
    if (length < 0) {
        errno = EINVAL;
        return JSValue::encode(raisePosixError(globalObject, scope));
    }
    ByteVector buffer;
    buffer.appendFill(0, std::min(static_cast<size_t>(length), mostAtATime));
    if (buffer.hasOverflowed())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    int64_t count = readDescriptor(globalObject, descriptor, buffer.mutableSpan());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, buffer.span().first(static_cast<size_t>(count)))));
}

PYTHON_NATIVE(posixReadinto)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    Buffer buffer = writableBufferArgument(globalObject, args.at(1), "readinto"_s, "argument 2"_s);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count = readDescriptor(globalObject, descriptor, mutableSpanOf(buffer));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, count));
}

PYTHON_NATIVE(posixPread)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(length, toSsize(globalObject, args.at(1)));
    CONVERT(offset, toFileOffset(globalObject, args.at(2)));
    if (length < 0) {
        errno = EINVAL;
        return JSValue::encode(raisePosixError(globalObject, scope));
    }
    ByteVector buffer;
    buffer.appendFill(0, static_cast<size_t>(length));
    if (buffer.hasOverflowed())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    ssize_t count;
    if (!retryIfInterrupted(globalObject, count, [&] { return ::pread(descriptor, buffer.mutableSpan().data(), buffer.size(), offset); }))
        return { };
    if (count < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, buffer.span().first(static_cast<size_t>(count)))));
}

PYTHON_NATIVE(posixWrite)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    Buffer data = bufferOf(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count;
    if (!retryIfInterrupted(globalObject, count, [&] { return withErrno(fileOperations(globalObject)->write(descriptor, data.first(std::min(data.size(), mostAtATime)))); }))
        return { };
    if (count < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, count));
}

PYTHON_NATIVE(posixPwrite)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    Buffer data = bufferOf(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    CONVERT(offset, toFileOffset(globalObject, args.at(2)));
    ssize_t count;
    if (!retryIfInterrupted(globalObject, count, [&] { return ::pwrite(descriptor, data.data(), data.size(), offset); }))
        return { };
    if (count < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, count));
}

// ---- Several buffers at once

// iov_setup(): where the bytes are of each of a sequence of things that have bytes. Nothing runs between this and the system call that could move them. False if it raised.
static bool gatherBuffers(JSGlobalObject* globalObject, JSValue sequence, ASCIILiteral ifNotASequence, bool areWrittenTo, Buffers& buffers, Vector<struct iovec>& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isSequence(globalObject, sequence)) {
        raiseTypeError(globalObject, scope, ifNotASequence);
        return false;
    }
    auto count = length(globalObject, sequence);
    RETURN_IF_EXCEPTION(scope, false);
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = getItem(globalObject, sequence, intFromInt64(globalObject, i));
        RETURN_IF_EXCEPTION(scope, false);
        Buffer buffer = tryBufferOf(globalObject, item, areWrittenTo ? WritableBuffer : SimpleBuffer);
        RETURN_IF_EXCEPTION(scope, false);
        if (!buffer) {
            raiseTypeError(globalObject, scope, concatenate("a bytes-like object is required, not '"_s, typeName(globalObject, item), '\''));
            return false;
        }
        buffers.append(WTF::move(buffer));
    }
    // Only now that nothing more of a program's is to run is it asked where they are.
    for (size_t i = 0; i < buffers.size(); ++i) {
        auto span = buffers.at(i);
        result.append({ const_cast<uint8_t*>(span.data()), span.size() });
    }
    return true;
}

enum class VectorIO : uint8_t { Readv, Preadv, Writev, Pwritev };

// readv(fd, buffers), preadv(fd, buffers, offset, flags=0), writev(fd, buffers) and pwritev(fd, buffers, offset, flags=0)
PYTHON_NATIVE(posixVectorIO)
{
    NATIVE_PROLOGUE();
    auto which = unpack<VectorIO>(callFrame, 0);
    bool isPositioned = which == VectorIO::Preadv || which == VectorIO::Pwritev;
    bool isRead = which == VectorIO::Readv || which == VectorIO::Preadv;
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    int64_t offset = 0;
    int flags = 0;
    if (isPositioned) {
        CONVERT(givenOffset, toFileOffset(globalObject, args.at(2)));
        offset = givenOffset;
        CONVERT_INT_OR(givenFlags, args.at(3), 0);
        flags = givenFlags;
    }
    ASCIILiteral ifNotASequence = which == VectorIO::Readv ? "readv() arg 2 must be a sequence"_s : which == VectorIO::Preadv ? "preadv2() arg 2 must be a sequence"_s
        : which == VectorIO::Writev ? "writev() arg 2 must be a sequence"_s : "pwritev() arg 2 must be a sequence"_s;
    if (!isSequence(globalObject, args.at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, ifNotASequence));
    length(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (flags)
        return JSValue::encode(raiseArgumentUnavailable(globalObject, scope, isRead ? "preadv2"_s : "pwritev2"_s, "flags"_s));
    Buffers kept(globalObject);
    Vector<struct iovec> buffers;
    if (!gatherBuffers(globalObject, args.at(1), ifNotASequence, isRead, kept, buffers))
        return { };
    int count = static_cast<int>(buffers.size());
    ssize_t result;
    bool isDone = retryIfInterrupted(globalObject, result, [&] () -> ssize_t {
        switch (which) {
        case VectorIO::Readv:
            return ::readv(descriptor, buffers.span().data(), count);
        case VectorIO::Preadv:
            return ::preadv(descriptor, buffers.span().data(), count, offset);
        case VectorIO::Writev:
            return ::writev(descriptor, buffers.span().data(), count);
        case VectorIO::Pwritev:
            return ::pwritev(descriptor, buffers.span().data(), count, offset);
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    if (!isDone)
        return { };
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, result));
}

PYTHON_NATIVE(posixSendfile)
{
    NATIVE_PROLOGUE();
    CONVERT(out, toCInt(globalObject, args.at(0)));
    CONVERT(in, toCInt(globalObject, args.at(1)));
#if OS(DARWIN)
    CONVERT(offset, toFileOffset(globalObject, args.at(2)));
    CONVERT(given, toFileOffset(globalObject, args.at(3)));
    off_t count = given;
    CONVERT_INT_OR(flags, args.at(6), 0);
    struct sf_hdtr extra { };
    Buffers keptHeaders(globalObject);
    Buffers keptTrailers(globalObject);
    Vector<struct iovec> headers;
    Vector<struct iovec> trailers;
    if (JSValue value = args.at(4)) {
        if (!gatherBuffers(globalObject, value, "sendfile() headers must be a sequence"_s, false, keptHeaders, headers))
            return { };
        if (!headers.isEmpty()) {
            extra.headers = headers.mutableSpan().data();
            extra.hdr_cnt = static_cast<int>(headers.size());
            for (auto& header : headers) {
                if (count >= std::numeric_limits<off_t>::max() - static_cast<off_t>(header.iov_len))
                    return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "sendfile() header is too large"_s));
                count += static_cast<off_t>(header.iov_len);
            }
        }
    }
    if (JSValue value = args.at(5)) {
        if (!gatherBuffers(globalObject, value, "sendfile() trailers must be a sequence"_s, false, keptTrailers, trailers))
            return { };
        if (!trailers.isEmpty()) {
            extra.trailers = trailers.mutableSpan().data();
            extra.trl_cnt = static_cast<int>(trailers.size());
        }
    }
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return ::sendfile(in, out, offset, &count, &extra, flags); }))
        return { };
    // If some of it has gone, that is what there is to say. If none has, it is for whoever asked to try again.
    if (result < 0 && !((errno == EAGAIN || errno == EBUSY) && count))
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, count));
#else
    CONVERT(count, toSsize(globalObject, args.at(3)));
    ssize_t result;
    if (isNone(args.at(2))) {
        if (!retryIfInterrupted(globalObject, result, [&] { return ::sendfile(out, in, nullptr, count); }))
            return { };
    } else {
        CONVERT(given, toFileOffset(globalObject, args.at(2)));
        off_t offset = given;
        if (!retryIfInterrupted(globalObject, result, [&] { return ::sendfile(out, in, &offset, count); }))
            return { };
    }
    if (result < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, result));
#endif
}

#if OS(DARWIN)
PYTHON_NATIVE(posixFcopyfile)
{
    NATIVE_PROLOGUE();
    CONVERT(in, toCInt(globalObject, args.at(0)));
    CONVERT(out, toCInt(globalObject, args.at(1)));
    CONVERT(flags, toCInt(globalObject, args.at(2)));
    if (::fcopyfile(in, out, nullptr, flags) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}
#endif

// ---- What is asked of one, or done to one

PYTHON_NATIVE(posixFstat)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    struct stat status;
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(::fstat(descriptor, &status)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(statResultFrom(globalObject, status)));
}

PYTHON_NATIVE(posixIsatty)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    return JSValue::encode(jsBoolean(fileOperations(globalObject)->isTerminal(descriptor)));
}

PYTHON_NATIVE(posixPipe)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    int descriptors[2];
    if (::pipe(descriptors))
        return JSValue::encode(raisePosixError(globalObject, scope));
    if (!setInheritable(globalObject, descriptors[0], false) || !setInheritable(globalObject, descriptors[1], false)) {
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        return { };
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { jsNumber(descriptors[0]), jsNumber(descriptors[1]) })));
}

PYTHON_NATIVE(posixFtruncate)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(length, toFileOffset(globalObject, args.at(1)));
    scope.release();
    if (!truncateDescriptor(globalObject, descriptor, length))
        return { };
    RETURN_NONE();
}

enum class OfDescriptor : uint8_t { Fchdir, Fsync };

// posix_fildes_fd(): fchdir(fd) and fsync(fd), each of which will do with a file in place of its descriptor.
PYTHON_NATIVE(posixOfDescriptor)
{
    NATIVE_PROLOGUE();
    auto which = unpack<OfDescriptor>(callFrame, 0);
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    if (which == OfDescriptor::Fchdir && !audit(globalObject, "os.chdir"_s, jsNumber(descriptor)))
        return { };
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(which == OfDescriptor::Fchdir ? ::fchdir(descriptor) : ::fsync(descriptor)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixSync)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    ::sync();
    RETURN_NONE();
}

PYTHON_NATIVE(posixFchmod)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(mode, toCInt(globalObject, args.at(1)));
    if (!audit(globalObject, "os.chmod"_s, jsNumber(descriptor), jsNumber(mode), jsNumber(-1)))
        return { };
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(::fchmod(descriptor, mode)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixFchown)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(user, toUserID(globalObject, args.at(1)));
    CONVERT(group, toGroupID(globalObject, args.at(2)));
    if (!audit(globalObject, "os.chown"_s, jsNumber(descriptor), intFromInt64(globalObject, user), intFromInt64(globalObject, group), jsNumber(-1)))
        return { };
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(::fchown(descriptor, user, group)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixGetInheritable)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    int flags = ::fcntl(descriptor, F_GETFD, 0);
    if (flags == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsBoolean(!(flags & FD_CLOEXEC)));
}

PYTHON_NATIVE(posixSetInheritable)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(isInheritable, toCInt(globalObject, args.at(1)));
    scope.release();
    if (!setInheritable(globalObject, descriptor, isInheritable))
        return { };
    RETURN_NONE();
}

PYTHON_NATIVE(posixGetBlocking)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsBoolean(!(flags & O_NONBLOCK)));
}

PYTHON_NATIVE(posixSetBlocking)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT_BOOL(isBlocking, args.at(1), true);
    int argument = !isBlocking;
    if (::ioctl(descriptor, FIONBIO, &argument) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// ---- Terminals

PYTHON_NATIVE(posixTtyname)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    long size = ::sysconf(_SC_TTY_NAME_MAX);
    if (size == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    Vector<char> buffer(static_cast<size_t>(size));
    if (int error = ::ttyname_r(descriptor, buffer.mutableSpan().data(), buffer.size())) {
        errno = error;
        return JSValue::encode(raisePosixError(globalObject, scope));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, unsafeSpan(buffer.span().data()))));
}

PYTHON_NATIVE(posixCtermid)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    char buffer[L_ctermid];
    if (!::ctermid(buffer))
        return JSValue::encode(raisePosixError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, unsafeSpan(buffer))));
}

// _Py_device_encoding()
PYTHON_NATIVE(posixDeviceEncoding)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    if (!fileOperations(globalObject)->isTerminal(descriptor))
        RETURN_NONE();
    return JSValue::encode(jsNontrivialString(vm, "utf-8"_s));
}

PYTHON_NATIVE(posixGetTerminalSize)
{
    NATIVE_PROLOGUE();
    CONVERT_INT_OR(descriptor, args.at(0), STDOUT_FILENO);
    struct winsize size;
    if (::ioctl(descriptor, TIOCGWINSZ, &size))
        return JSValue::encode(raisePosixError(globalObject, scope));
    MarkedArgumentBuffer values;
    values.append(jsNumber(size.ws_col));
    values.append(jsNumber(size.ws_row));
    RELEASE_AND_RETURN(scope, JSValue::encode(newStructSequence(globalObject, posixState(globalObject).terminalSize.get(), values)));
}

PYTHON_NATIVE(posixOpenpt)
{
    NATIVE_PROLOGUE();
    CONVERT(flags, toCInt(globalObject, args.at(0)));
    int descriptor = ::posix_openpt(flags);
    if (descriptor == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    if (!setInheritable(globalObject, descriptor, false)) {
        ::close(descriptor);
        return { };
    }
    return JSValue::encode(jsNumber(descriptor));
}

enum class OfTerminal : uint8_t { Grantpt, Unlockpt, LoginTty };

PYTHON_NATIVE(posixOfTerminal)
{
    NATIVE_PROLOGUE();
    auto which = unpack<OfTerminal>(callFrame, 0);
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    int result = which == OfTerminal::Grantpt ? ::grantpt(descriptor) : which == OfTerminal::Unlockpt ? ::unlockpt(descriptor) : ::login_tty(descriptor);
    if (result == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixPtsname)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toFileDescriptorOrFile(globalObject, args.at(0)));
    char* name = ::ptsname(descriptor);
    if (!name)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, unsafeSpan(name))));
}

PYTHON_NATIVE(posixOpenpty)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    int controller = -1;
    int terminal = -1;
    if (::openpty(&controller, &terminal, nullptr, nullptr, nullptr))
        return JSValue::encode(raisePosixError(globalObject, scope));
    if (!setInheritable(globalObject, controller, false) || !setInheritable(globalObject, terminal, false)) {
        ::close(controller);
        ::close(terminal);
        return { };
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { jsNumber(controller), jsNumber(terminal) })));
}

PYTHON_NATIVE(posixTcgetpgrp)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    pid_t group = ::tcgetpgrp(descriptor);
    if (group < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(jsNumber(group));
}

PYTHON_NATIVE(posixTcsetpgrp)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    CONVERT(group, toCInt(globalObject, args.at(1)));
    if (::tcsetpgrp(descriptor, group) < 0)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// ---- Device numbers

// major_minor_conv()
static JSValue intFromDevicePart(JSGlobalObject* globalObject, unsigned value)
{
#ifdef NODEV
    if (value == static_cast<unsigned>(NODEV))
        return jsNumber(static_cast<int>(NODEV));
#endif
    return intFromInt64(globalObject, value);
}

// major(device) and minor(device)
PYTHON_NATIVE(posixDevicePart)
{
    NATIVE_PROLOGUE();
    CONVERT(device, toDevice(globalObject, args.at(0)));
    return JSValue::encode(intFromDevicePart(globalObject, unpack<bool>(callFrame, 0) ? major(device) : minor(device)));
}

PYTHON_NATIVE(posixMakedev)
{
    NATIVE_PROLOGUE();
    CONVERT(majorPart, toDevice(globalObject, args.at(0)));
    CONVERT(minorPart, toDevice(globalObject, args.at(1)));
    auto fits = [] (dev_t value) {
#ifdef NODEV
        if (value == NODEV)
            return true;
#endif
        return static_cast<dev_t>(static_cast<unsigned>(value)) == value;
    };
    if (!fits(majorPart) || !fits(minorPart))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C unsigned int"_s));
    dev_t device = makedev(majorPart, minorPart);
#ifdef NODEV
    if (device == NODEV)
        return JSValue::encode(intFromInt64(globalObject, static_cast<int64_t>(device)));
#endif
    return JSValue::encode(intFromUInt64(globalObject, static_cast<uint64_t>(device)));
}

void addPosixDescriptorFunctions(JSGlobalObject* globalObject, JSObject* module)
{
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    add("open"_s, posixOpen);
    add("close"_s, posixClose);
    add("closerange"_s, posixCloserange);
    add("dup"_s, posixDup);
    add("dup2"_s, posixDup2);
    add("lockf"_s, posixLockf);
    add("lseek"_s, posixLseek);
    add("read"_s, posixRead);
    add("readinto"_s, posixReadinto);
    add("pread"_s, posixPread);
    add("write"_s, posixWrite);
    add("pwrite"_s, posixPwrite);
    add("readv"_s, posixVectorIO, pack(VectorIO::Readv));
    add("preadv"_s, posixVectorIO, pack(VectorIO::Preadv));
    add("writev"_s, posixVectorIO, pack(VectorIO::Writev));
    add("pwritev"_s, posixVectorIO, pack(VectorIO::Pwritev));
    add("sendfile"_s, posixSendfile);
    add("fstat"_s, posixFstat);
    add("isatty"_s, posixIsatty);
    add("pipe"_s, posixPipe);
    add("ftruncate"_s, posixFtruncate);
    add("fchdir"_s, posixOfDescriptor, pack(OfDescriptor::Fchdir));
    add("fsync"_s, posixOfDescriptor, pack(OfDescriptor::Fsync));
    add("sync"_s, posixSync);
    add("fchmod"_s, posixFchmod);
    add("fchown"_s, posixFchown);
    add("get_inheritable"_s, posixGetInheritable);
    add("set_inheritable"_s, posixSetInheritable);
    add("get_blocking"_s, posixGetBlocking);
    add("set_blocking"_s, posixSetBlocking);
    add("ttyname"_s, posixTtyname);
    add("ctermid"_s, posixCtermid);
    add("device_encoding"_s, posixDeviceEncoding);
    add("get_terminal_size"_s, posixGetTerminalSize);
    add("posix_openpt"_s, posixOpenpt);
    add("grantpt"_s, posixOfTerminal, pack(OfTerminal::Grantpt));
    add("unlockpt"_s, posixOfTerminal, pack(OfTerminal::Unlockpt));
    add("login_tty"_s, posixOfTerminal, pack(OfTerminal::LoginTty));
    add("ptsname"_s, posixPtsname);
    add("openpty"_s, posixOpenpty);
    add("tcgetpgrp"_s, posixTcgetpgrp);
    add("tcsetpgrp"_s, posixTcsetpgrp);
    add("major"_s, posixDevicePart, pack(true));
    add("minor"_s, posixDevicePart, pack(false));
    add("makedev"_s, posixMakedev);
#if OS(DARWIN)
    add("_fcopyfile"_s, posixFcopyfile);
#endif
}

} } // namespace JSC::Python

#endif // OS(UNIX)
