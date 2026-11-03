/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyIO.h"
#include "PyPosix.h"

#include "JSCInlines.h"
#include "PyCodecs.h"
#include "PyType.h"
#include "PyVirtualFiles.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// FileIO: a file descriptor, read and written with one system call for each call. (Modules/_io/fileio.c in CPython.)

namespace {

struct FileIOState {
    int fd { -1 };
    bool created { false };
    bool readable { false };
    bool writable { false };
    bool appending { false };
    bool truncate { false };
    bool closefd { true };
    bool finalizing { false };
    int8_t seekable { -1 }; // Negative: not known yet.
    // What fstat() said when the file was opened: a guess at best by the time it is used.
    bool hasStat { false };
    mode_t statMode { 0 };
    int64_t statSize { 0 };
    int64_t statBlockSize { 0 };
};

} // namespace

static constexpr unsigned fileIOFieldCount = 1;
static constexpr size_t fileIOSmallChunk = 8 * 1024;
static constexpr size_t fileIOLargeBufferCutoff = 65536;
#if OS(DARWIN)
static constexpr size_t fileIOMaxTransfer = std::numeric_limits<int>::max();
#else
static constexpr size_t fileIOMaxTransfer = std::numeric_limits<ssize_t>::max();
#endif

static ALWAYS_INLINE FileIOState& fileIOStateOf(JSValue self) { return ioStateOf<FileIOState>(PyRecord::from(self)); }

static JSValue fileIORaiseClosed(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, "I/O operation on closed file"_s);
}

static JSValue fileIORaiseMode(JSGlobalObject* globalObject, ASCIILiteral action)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return ioRaiseUnsupported(globalObject, scope, makeString("File not open for "_s, action));
}

static JSValue fileIORaiseErrno(JSGlobalObject* globalObject, int error, JSValue filename = { })
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseOSError(globalObject, scope, error, filename);
}

bool fileIOIsClosed(PyRecord* file) { return ioStateOf<FileIOState>(file).fd < 0; }

// ---- System calls ----
// The count. -1: an exception. -2: nothing can be read or written without waiting.

static int64_t fileIOSystemRead(JSGlobalObject* globalObject, int fd, uint8_t* buffer, size_t count)
{
    ssize_t result;
    do {
        AllowThreads allow(globalObject);
        result = read(fd, buffer, std::min(count, fileIOMaxTransfer));
    } while (result < 0 && errno == EINTR && checkSignals(globalObject));
    if (result >= 0)
        return result;
    if (getVM(globalObject).exceptionForInspection())
        return -1;
    if (errno == EAGAIN)
        return -2;
    fileIORaiseErrno(globalObject, errno);
    return -1;
}

static int64_t fileIOSystemWrite(JSGlobalObject* globalObject, int fd, const uint8_t* buffer, size_t count)
{
    ssize_t result;
    do {
        AllowThreads allow(globalObject);
        result = write(fd, buffer, std::min(count, fileIOMaxTransfer));
    } while (result < 0 && errno == EINTR && checkSignals(globalObject));
    if (result >= 0)
        return result;
    if (getVM(globalObject).exceptionForInspection())
        return -1;
    if (errno == EAGAIN)
        return -2;
    fileIORaiseErrno(globalObject, errno);
    return -1;
}

// -1: an exception.
static int64_t fileIOSystemSeek(JSGlobalObject* globalObject, FileIOState& state, int64_t position, int whence, bool pipeIsFine = false)
{
    off_t result = lseek(state.fd, position, whence);
    if (state.seekable < 0)
        state.seekable = result >= 0;
    if (result >= 0)
        return result;
    if (pipeIsFine && errno == ESPIPE)
        return 0;
    fileIORaiseErrno(globalObject, errno);
    return -1;
}

// False: an exception.
static bool fileIOSystemClose(JSGlobalObject* globalObject, FileIOState& state)
{
    int fd = std::exchange(state.fd, -1);
    state.hasStat = false;
    if (fd < 0)
        return true;
    int result;
    {
        AllowThreads allow(globalObject);
        result = close(fd);
    }
    if (!result)
        return true;
    fileIORaiseErrno(globalObject, errno);
    return false;
}

int64_t fileIOReadInto(JSGlobalObject* globalObject, PyRecord* file, std::span<uint8_t> buffer)
{
    auto& state = ioStateOf<FileIOState>(file);
    if (state.fd < 0) {
        fileIORaiseClosed(globalObject);
        return -1;
    }
    if (!state.readable) {
        fileIORaiseMode(globalObject, "reading"_s);
        return -1;
    }
    return fileIOSystemRead(globalObject, state.fd, buffer.data(), buffer.size());
}

int64_t fileIOWrite(JSGlobalObject* globalObject, PyRecord* file, std::span<const uint8_t> buffer)
{
    auto& state = ioStateOf<FileIOState>(file);
    if (state.fd < 0) {
        fileIORaiseClosed(globalObject);
        return -1;
    }
    if (!state.writable) {
        fileIORaiseMode(globalObject, "writing"_s);
        return -1;
    }
    return fileIOSystemWrite(globalObject, state.fd, buffer.data(), buffer.size());
}

int64_t fileIOSeek(JSGlobalObject* globalObject, PyRecord* file, int64_t position, int whence)
{
    auto& state = ioStateOf<FileIOState>(file);
    if (state.fd < 0) {
        fileIORaiseClosed(globalObject);
        return -1;
    }
    return fileIOSystemSeek(globalObject, state, position, whence);
}

// ---- Opening and closing ----

static JSValue fileIONew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    PyRecord* file = ioAllocate<FileIOState>(globalObject, cls, fileIOFieldCount);
    ioDidAllocate(globalObject, file);
    return file;
}

// open(2), for a file that programs started from this one do not inherit.
static int fileIOSystemOpen(JSGlobalObject* globalObject, const char* name, int flags)
{
    writeOutPendingOutput(globalObject);
    // Opening a FIFO waits for its other end, which may be another thread's to open.
    VirtualPath fromRoot;
    bool isVirtual = toVirtualPath(std::span<const char> { name, strlen(name) }, fromRoot);
    auto attempt = [&] {
        int fd;
        do {
            if (isVirtual)
                fd = openFile(name, flags, 0666);
            else {
                AllowThreads allow(globalObject);
                fd = open(name, flags, 0666);
            }
        } while (fd < 0 && errno == EINTR && checkSignals(globalObject));
        return fd;
    };
    int fd = attempt();
    if (fd < 0 && posixFreeDescriptors(globalObject))
        fd = attempt();
    return fd;
}

static JSValue fileIOInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    static constexpr ASCIILiteral keywords[] = { "file"_s, "mode"_s, "closefd"_s, "opener"_s };
    JSValue values[std::size(keywords)];
    if (!unpackKeywordArguments(globalObject, "FileIO"_s, keywords, 1, 4, false, args, values))
        return { };
    JSValue nameObject = values[0];
    String mode = "r"_s;
    if (values[1]) {
        if (!isStr(values[1]))
            return raiseTypeError(globalObject, scope, "FileIO() argument 'mode' must be str, not "_s, isNone(values[1]) ? "None"_str : typeName(globalObject, values[1]));
        if (!ioToOptionalString(globalObject, values[1], "FileIO"_s, "mode"_s, mode))
            return { };
    }
    bool closefd = !values[2] || isTrue(globalObject, values[2]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue opener = values[3] ? values[3] : jsUndefined();

    PyRecord* file = PyRecord::from(self);
    auto& state = ioStateOf<FileIOState>(file);

    if (state.fd >= 0) {
        if (!state.closefd)
            state.fd = -1;
        else if (!fileIOSystemClose(globalObject, state))
            return { };
    }

    if (nameObject.isBoolean()) {
        if (!warn(globalObject, rt.RuntimeWarning(), "bool is used as a file descriptor"_s, 1))
            return { };
    }

    int fd = -1;
    if (ioToInt(globalObject, nameObject, fd)) {
        if (fd < 0)
            return raiseValueError(globalObject, scope, "negative file descriptor"_s);
    } else {
        takePendingException(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        fd = -1;
    }

    PyBytes* encodedName = nullptr;
    if (fd < 0) {
        JSValue path = ioFSPath(globalObject, nameObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (isStr(path)) {
            path = codecEncodeUTF8(globalObject, strValue(path), "surrogateescape"_s);
            RETURN_IF_EXCEPTION(scope, { });
        }
        encodedName = PyBytes::from(path);
        if (memchr(encodedName->data(), 0, encodedName->length()))
            return raiseValueError(globalObject, scope, "embedded null byte"_s);
    }

    bool fdIsOwn = false;
    auto open = [&]() -> bool {
        static constexpr auto badMode = "Must have exactly one of create/read/write/append mode and at most one plus"_s;
        bool hasKind = false;
        bool hasPlus = false;
        int flags = 0;
        for (unsigned i = 0; i < mode.length(); ++i) {
            char16_t character = mode[i];
            if (character == 'x' || character == 'r' || character == 'w' || character == 'a') {
                if (hasKind) {
                    raiseValueError(globalObject, scope, badMode);
                    return false;
                }
                hasKind = true;
            }
            switch (character) {
            case 'x':
                state.created = true;
                state.writable = true;
                flags |= O_EXCL | O_CREAT;
                break;
            case 'r':
                state.readable = true;
                break;
            case 'w':
                state.writable = true;
                state.truncate = true;
                flags |= O_CREAT | O_TRUNC;
                break;
            case 'a':
                state.writable = true;
                state.appending = true;
                flags |= O_APPEND | O_CREAT;
                break;
            case 'b':
                break;
            case '+':
                if (hasPlus) {
                    raiseValueError(globalObject, scope, badMode);
                    return false;
                }
                state.readable = true;
                state.writable = true;
                hasPlus = true;
                break;
            default:
                raiseValueError(globalObject, scope, "invalid mode: "_s, mode);
                return false;
            }
        }
        if (!hasKind) {
            raiseValueError(globalObject, scope, badMode);
            return false;
        }
        flags |= state.readable && state.writable ? O_RDWR : state.readable ? O_RDONLY : O_WRONLY;
        flags |= O_CLOEXEC;

        if (isAudited(globalObject) && !audit(globalObject, "open"_s, nameObject, makeStr(vm, mode), jsNumber(flags)))
            return false;

        if (fd >= 0) {
            state.fd = fd;
            state.closefd = closefd;
        } else {
            state.closefd = true;
            if (!closefd) {
                raiseValueError(globalObject, scope, "Cannot use closefd=False with file name"_s);
                return false;
            }
            if (isNone(opener)) {
                state.fd = fileIOSystemOpen(globalObject, reinterpret_cast<const char*>(encodedName->data()), flags);
                RETURN_IF_EXCEPTION(scope, false);
                if (state.fd < 0) {
                    fileIORaiseErrno(globalObject, errno, nameObject);
                    return false;
                }
                fdIsOwn = true;
            } else {
                JSValue result = call(globalObject, opener, nameObject, jsNumber(flags));
                RETURN_IF_EXCEPTION(scope, false);
                if (!isInt(result)) {
                    raiseTypeError(globalObject, scope, "expected integer from opener"_s);
                    return false;
                }
                int opened;
                if (!ioToInt(globalObject, result, opened))
                    return false;
                state.fd = opened;
                if (opened < 0) {
                    raiseValueError(globalObject, scope, "opener returned "_s, opened);
                    return false;
                }
                fdIsOwn = true;
                int descriptorFlags = fcntl(opened, F_GETFD);
                if (descriptorFlags < 0 || (!(descriptorFlags & FD_CLOEXEC) && fcntl(opened, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0)) {
                    fileIORaiseErrno(globalObject, errno);
                    return false;
                }
            }
        }

        struct stat status;
        if (fstat(state.fd, &status) < 0) {
            // Some file systems fail here for a file that is perfectly usable.
            if (errno == EBADF) {
                fileIORaiseErrno(globalObject, errno);
                return false;
            }
            state.hasStat = false;
        } else {
            if (S_ISDIR(status.st_mode)) {
                fileIORaiseErrno(globalObject, EISDIR, nameObject);
                return false;
            }
            state.hasStat = true;
            state.statMode = status.st_mode;
            state.statSize = status.st_size;
            state.statBlockSize = status.st_blksize;
        }

        setAttr(globalObject, self, rt.names.name(), nameObject);
        RETURN_IF_EXCEPTION(scope, false);

        // Otherwise the position would move to the end only at the first write.
        if (state.appending && fileIOSystemSeek(globalObject, state, 0, SEEK_END, true) < 0)
            return false;
        return true;
    };

    if (!open()) {
        if (!fdIsOwn)
            state.fd = -1;
        if (state.fd >= 0) {
            JSValue error = takePendingException(globalObject);
            fileIOSystemClose(globalObject, state);
            ioChainExceptions(globalObject, error);
        }
        state.hasStat = false;
        scope.release();
        return { };
    }
    ensureStillAliveHere(encodedName);

    if (state.closefd || file->type()->isHeapType())
        ioDidOpen(globalObject, file);
    return jsUndefined();
}

static JSValue fileIODeallocWarn(JSGlobalObject* globalObject, JSValue self, JSValue source)
{
    auto& state = fileIOStateOf(self);
    if (state.fd >= 0 && state.closefd)
        ioWarnUnclosed(globalObject, self, source);
    return jsUndefined();
}

static JSValue fileIOClose(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* file = PyRecord::from(self);
    auto& state = ioStateOf<FileIOState>(file);

    JSValue result = ioBaseClose(globalObject, self);
    if (!state.closefd) {
        state.fd = -1;
        ioDidClose(globalObject, file);
        scope.release();
        return result;
    }
    JSValue error = result ? JSValue() : takePendingException(globalObject);
    if (state.finalizing)
        fileIODeallocWarn(globalObject, self, self);
    bool closed = fileIOSystemClose(globalObject, state);
    ioDidClose(globalObject, file);
    ioChainExceptions(globalObject, error);
    scope.release();
    return closed ? result : JSValue();
}

// ---- What it can do ----

static JSValue fileIOFileno(JSGlobalObject* globalObject, JSValue self)
{
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    return jsNumber(state.fd);
}

static JSValue fileIOReadable(JSGlobalObject* globalObject, JSValue self)
{
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    return jsBoolean(state.readable);
}

static JSValue fileIOWritable(JSGlobalObject* globalObject, JSValue self)
{
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    return jsBoolean(state.writable);
}

static JSValue fileIOSeekable(JSGlobalObject* globalObject, JSValue self)
{
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    if (state.seekable < 0)
        state.seekable = lseek(state.fd, 0, SEEK_CUR) >= 0;
    return jsBoolean(state.seekable);
}

static JSValue fileIOIsATTY(JSGlobalObject* globalObject, JSValue self)
{
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    return jsBoolean(isatty(state.fd));
}

// For open(), right after the file was opened: only a character device can be a terminal.
static JSValue fileIOIsATTYOpenOnly(JSGlobalObject* globalObject, JSValue self)
{
    auto& state = fileIOStateOf(self);
    if (state.hasStat && !S_ISCHR(state.statMode))
        return jsBoolean(false);
    return fileIOIsATTY(globalObject, self);
}

// ---- Reading and writing ----

static JSValue fileIOResult(JSGlobalObject* globalObject, int64_t count)
{
    if (count == -1)
        return { };
    if (count == -2)
        return jsUndefined();
    return makeInt(globalObject, count);
}

static JSValue fileIOReadIntoMethod(JSGlobalObject* globalObject, JSValue self, JSValue target)
{
    IOBuffer buffer;
    if (!buffer.acquireWritable(globalObject, "readinto"_s, target, "argument 1"_s))
        return { };
    return fileIOResult(globalObject, fileIOReadInto(globalObject, PyRecord::from(self), buffer.mutableSpan()));
}

static JSValue fileIOReadall(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    if (!state.readable)
        return fileIORaiseMode(globalObject, "reading"_s);

    // How much there probably is.
    size_t expected = 0;
    if (state.hasStat && state.statSize > 0) {
        expected = state.statSize;
        // Some of a large file may have been read already.
        if (expected >= fileIOLargeBufferCutoff) {
            off_t position = lseek(state.fd, 0, SEEK_CUR);
            if (position >= 0 && state.statSize >= position)
                expected = state.statSize - position;
        }
    }

    // If that is right, one call reads it into its final place, and another finds nothing more.
    PyBytes* result = nullptr;
    size_t filled = 0;
    bool wouldBlock = false;
    if (expected) {
        result = ioTryNewBytes(globalObject, expected);
        RETURN_IF_EXCEPTION(scope, { });
        while (filled < expected) {
            int64_t count = fileIOSystemRead(globalObject, state.fd, result->mutableData() + filled, expected - filled);
            if (count == -1)
                return { };
            if (count <= 0) {
                wouldBlock = count == -2;
                if (wouldBlock && !filled)
                    return jsUndefined();
                return PyBytes::create(globalObject, result->span().first(filled));
            }
            filled += count;
        }
    }

    Vector<uint8_t> rest;
    size_t restFilled = 0;
    while (true) {
        if (restFilled >= rest.size()) {
            size_t total = filled + restFilled;
            size_t addend = std::max(total > fileIOLargeBufferCutoff ? total >> 3 : 256 + total, fileIOSmallChunk);
            // The first time round, this is most likely the call that finds the end of the file.
            if (!rest.tryGrow(rest.size() + (result && rest.isEmpty() ? fileIOSmallChunk : addend)))
                return raiseWithArgs(globalObject, scope, runtime(globalObject).MemoryError(), { });
        }
        int64_t count = fileIOSystemRead(globalObject, state.fd, rest.mutableSpan().data() + restFilled, rest.size() - restFilled);
        if (count == -1)
            return { };
        if (count == -2) {
            if (!filled && !restFilled)
                return jsUndefined();
            break;
        }
        if (!count)
            break;
        restFilled += count;
    }
    if (!restFilled)
        return result ? result : runtime(globalObject).emptyBytes();
    if (!result)
        return PyBytes::create(globalObject, rest.span().first(restFilled));
    PyBytes* all = ioTryNewBytes(globalObject, filled + restFilled);
    RETURN_IF_EXCEPTION(scope, { });
    memcpy(all->mutableData(), result->data(), filled);
    memcpy(all->mutableData() + filled, rest.span().data(), restFilled);
    return all;
}

static JSValue fileIORead(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = -1;
    if (!ioToOptionalSize(globalObject, sizeValue, size))
        return { };
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    if (!state.readable)
        return fileIORaiseMode(globalObject, "reading"_s);
    if (size < 0)
        RELEASE_AND_RETURN(scope, fileIOReadall(globalObject, self));

    PyBytes* result = ioTryNewBytes(globalObject, size);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count = fileIOSystemRead(globalObject, state.fd, result->mutableData(), size);
    if (count < 0)
        RELEASE_AND_RETURN(scope, fileIOResult(globalObject, count));
    if (count == size)
        return result;
    return PyBytes::create(globalObject, result->span().first(count));
}

static JSValue fileIOWriteMethod(JSGlobalObject* globalObject, JSValue self, JSValue data)
{
    IOBuffer buffer;
    if (!buffer.acquire(globalObject, data))
        return { };
    return fileIOResult(globalObject, fileIOWrite(globalObject, PyRecord::from(self), buffer.span()));
}

// ---- The position ----

static JSValue fileIOSeekMethod(JSGlobalObject* globalObject, JSValue self, JSValue positionValue, JSValue whenceValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int whence = 0;
    if (whenceValue && !ioToInt(globalObject, whenceValue, whence))
        return { };
    auto& state = fileIOStateOf(self);
    if (state.fd < 0)
        return fileIORaiseClosed(globalObject);
    int64_t position = toInt64(globalObject, positionValue);
    RETURN_IF_EXCEPTION(scope, { });
    // Converting the position may have run anything.
    int64_t result = fileIOSystemSeek(globalObject, state, position, whence);
    RETURN_IF_EXCEPTION(scope, { });
    return makeInt(globalObject, result);
}

static JSValue fileIOTell(JSGlobalObject* globalObject, JSValue self)
{
    return fileIOResult(globalObject, fileIOSeek(globalObject, PyRecord::from(self), 0, SEEK_CUR));
}

static JSValue fileIOTruncate(JSGlobalObject* globalObject, JSValue self, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = fileIOStateOf(self);
    int fd = state.fd;
    if (fd < 0)
        return fileIORaiseClosed(globalObject);
    if (!state.writable)
        return fileIORaiseMode(globalObject, "writing"_s);

    if (!sizeValue || isNone(sizeValue)) {
        int64_t position = fileIOSystemSeek(globalObject, state, 0, SEEK_CUR);
        RETURN_IF_EXCEPTION(scope, { });
        sizeValue = makeInt(globalObject, position);
    }
    int64_t size = toInt64(globalObject, sizeValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (ftruncate(fd, size))
        return fileIORaiseErrno(globalObject, errno);
    // The size it had when it was opened says nothing any more.
    state.hasStat = false;
    return sizeValue;
}

// ---- Attributes ----

static ASCIILiteral fileIOModeString(const FileIOState& state)
{
    if (state.created)
        return state.readable ? "xb+"_s : "xb"_s;
    if (state.appending)
        return state.readable ? "ab+"_s : "ab"_s;
    if (state.readable)
        return !state.writable ? "rb"_s : state.truncate ? "wb+"_s : "rb+"_s;
    return "wb"_s;
}

static JSValue fileIORepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = fileIOStateOf(self);
    String ownName = typeName(globalObject, self);
    if (state.fd < 0)
        return makeStr(vm, makeString('<', ownName, " [closed]>"_s));

    JSValue name = getAttrIfPresent(globalObject, self, rt.names.name());
    RETURN_IF_EXCEPTION(scope, { });
    if (!name)
        return makeStr(vm, makeString('<', ownName, " fd="_s, state.fd, " mode='"_s, fileIOModeString(state), "' closefd="_s, state.closefd ? "True"_s : "False"_s, '>'));

    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return raiseRuntimeError(globalObject, scope, "reentrant call inside "_s, ownName, ".__repr__"_s);
    JSString* nameRepr = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    String nameText = nameRepr->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString('<', ownName, " name="_s, nameText, " mode='"_s, fileIOModeString(state), "' closefd="_s, state.closefd ? "True"_s : "False"_s, '>'));
}

static JSValue fileIOGetClosed(JSGlobalObject*, JSValue self) { return jsBoolean(fileIOStateOf(self).fd < 0); }
static JSValue fileIOGetClosefd(JSGlobalObject*, JSValue self) { return jsBoolean(fileIOStateOf(self).closefd); }
static JSValue fileIOGetMode(JSGlobalObject* globalObject, JSValue self) { return makeStr(getVM(globalObject), fileIOModeString(fileIOStateOf(self))); }

static JSValue fileIOGetBlockSize(JSGlobalObject* globalObject, JSValue self)
{
    auto& state = fileIOStateOf(self);
    return makeInt(globalObject, state.hasStat && state.statBlockSize > 1 ? state.statBlockSize : ioDefaultBufferSize);
}

static JSValue fileIOGetFinalizing(JSGlobalObject*, JSValue self) { return jsBoolean(fileIOStateOf(self).finalizing); }
static void fileIOSetFinalizing(JSGlobalObject* globalObject, JSValue self, JSValue value) { ioToFinalizing(globalObject, value, fileIOStateOf(self).finalizing); }

static constexpr NativeDef fileIOMethods[] = {
    PY_NEW(fileIONew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", fileIOInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", fileIORepr, "($self, /)"),
    PY_METHOD("read", fileIORead, "($self, size=-1, /)"),
    PY_METHOD("readall", fileIOReadall, "($self, /)"),
    PY_METHOD("readinto", fileIOReadIntoMethod, "($self, buffer, /)"),
    PY_METHOD("write", fileIOWriteMethod, "($self, b, /)"),
    PY_METHOD("seek", fileIOSeekMethod, "($self, pos, whence=0, /)"),
    PY_METHOD("tell", fileIOTell, "($self, /)"),
    PY_METHOD("truncate", fileIOTruncate, "($self, size=None, /)"),
    PY_METHOD("close", fileIOClose, "($self, /)"),
    PY_METHOD("seekable", fileIOSeekable, "($self, /)"),
    PY_METHOD("readable", fileIOReadable, "($self, /)"),
    PY_METHOD("writable", fileIOWritable, "($self, /)"),
    PY_METHOD("fileno", fileIOFileno, "($self, /)"),
    PY_METHOD("isatty", fileIOIsATTY, "($self, /)"),
    PY_METHOD("_isatty_open_only", fileIOIsATTYOpenOnly, "($self, /)"),
    PY_METHOD("_dealloc_warn", fileIODeallocWarn, "($self, source, /)"),
    PY_METHOD("__getstate__", ioCannotPickle, "($self, /)"),
};

static constexpr NativeGetSet fileIOGetSets[] = {
    { "closed"_s, fileIOGetClosed },
    { "closefd"_s, fileIOGetClosefd },
    { "mode"_s, fileIOGetMode },
    { "_blksize"_s, fileIOGetBlockSize },
    PY_WRITABLE_MEMBER("_finalizing", fileIOGetFinalizing, fileIOSetFinalizing),
};

constexpr TypeDef ioFileIOTypeDef { "_io.FileIO"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, fileIOMethods, fileIOGetSets,
    "Open a file.\n\n"
    "The mode can be 'r' (default), 'w', 'x' or 'a' for reading, writing, exclusive creation or appending, with a '+' to\n"
    "allow both reading and writing. `file` is a path, or a file descriptor, which close() closes unless closefd is false.\n"
    "With an opener, the file descriptor is what opener(file, flags) returns."_s };

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
