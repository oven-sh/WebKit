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
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

// io.FileIO: Modules/_io/fileio.c of CPython.

namespace JSC { namespace Python {

namespace {

constexpr size_t smallChunk = std::max<size_t>(BUFSIZ, 8 * KB); // SMALLCHUNK
constexpr size_t largeBufferCutoff = 65536; // LARGE_BUFFER_CUTOFF_SIZE
constexpr int64_t mostToRead = std::numeric_limits<int64_t>::max(); // _PY_READ_MAX

struct FileIOState final : NativeState {
    PYTHON_NATIVE_STATE(FileIOState);

    int descriptor { -1 };
    bool isCreated { false };
    bool isReadable { false };
    bool isWritable { false };
    bool isAppending { false };
    int8_t isSeekable { -1 }; // -1 is that it is not known.
    bool closesDescriptor { true };
    bool isFinalizing { false };
    // How things were when it was opened, which is something to go by and no more: it may all have changed since.
    std::optional<FileStatus> statusWhenOpened;
};

template<typename Visitor> void FileIOState::visit(Visitor&) { }

FileIOState& stateOfFile(JSValue self) { return stateOf<FileIOState>(self); }

JSValue raiseClosed(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return raiseValueError(globalObject, scope, "I/O operation on closed file"_s);
}

JSValue raiseWrongMode(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral action)
{
    return raiseUnsupportedOperation(globalObject, scope, concatenate("File not open for "_s, action));
}

// If there are no files to be had, everything fails as a system does that has no such call.
struct NoFiles {
    static const FileOperations* operations()
    {
        static constexpr FileOperations none {
            [] (const CString&, int, int) { return -ENOSYS; },
            [] (int) { return -ENOSYS; },
            [] (int, std::span<uint8_t>) -> int64_t { return -ENOSYS; },
            [] (int, std::span<const uint8_t>) -> int64_t { return -ENOSYS; },
            [] (int, int64_t, int) -> int64_t { return -ENOSYS; },
            [] (int, int64_t) { return -ENOSYS; },
            [] (int, FileStatus&) { return -ENOSYS; },
            [] (int) { return false; },
            [] (int, bool) { return -ENOSYS; },
        };
        return &none;
    }
};

const FileOperations& files(JSGlobalObject* globalObject)
{
    auto* operations = fileOperations(globalObject);
    return operations ? *operations : *NoFiles::operations();
}

// _Py_read(): how many were read. If it failed, that has been raised, and this is what errno was, negated.
int64_t readFrom(JSGlobalObject* globalObject, int descriptor, std::span<uint8_t> buffer)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t count;
    do {
        count = files(globalObject).read(descriptor, buffer);
        if (count != -EINTR)
            break;
        if (!checkSignals(globalObject))
            return count;
    } while (true);
    if (count < 0)
        raiseOSError(globalObject, scope, static_cast<int>(-count));
    return count;
}

// _Py_write()
int64_t writeTo(JSGlobalObject* globalObject, int descriptor, std::span<const uint8_t> bytes)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t count;
    do {
        count = files(globalObject).write(descriptor, bytes);
        if (count != -EINTR)
            break;
        if (!checkSignals(globalObject))
            return count;
    } while (true);
    if (count < 0)
        raiseOSError(globalObject, scope, static_cast<int>(-count));
    return count;
}

// If what failed is only that there is nothing to be had for now, that is not raised after all.
bool wouldBlock(JSGlobalObject* globalObject, int64_t result)
{
    if (result != -EAGAIN)
        return false;
    takeRaisedException(globalObject->vm());
    // BufferedWriter looks at this to say why.
    errno = EAGAIN;
    return true;
}

// internal_close()
void closeDescriptor(JSGlobalObject* globalObject, FileIOState& state)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int result = 0;
    if (state.descriptor >= 0)
        result = files(globalObject).close(std::exchange(state.descriptor, -1));
    state.statusWhenOpened = std::nullopt;
    if (result < 0)
        raiseOSError(globalObject, scope, -result);
}

// portable_lseek()
JSValue seekTo(JSGlobalObject* globalObject, FileIOState& state, JSValue position, int whence, bool passesOverPipes)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
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
    int64_t offset = 0;
    if (position) {
        // PyLong_AsLongLong()
        auto given = toCLong(globalObject, position);
        RETURN_IF_EXCEPTION(scope, { });
        offset = *given;
    }
    int64_t result = files(globalObject).seek(state.descriptor, offset, whence);
    if (state.isSeekable < 0)
        state.isSeekable = result >= 0;
    if (result < 0) {
        if (!passesOverPipes || result != -ESPIPE)
            return raiseOSError(globalObject, scope, static_cast<int>(-result));
        result = 0;
    }
    return intFromInt64(globalObject, result);
}

ASCIILiteral modeOf(const FileIOState& state)
{
    if (state.isCreated)
        return state.isReadable ? "xb+"_s : "xb"_s;
    if (state.isAppending)
        return state.isReadable ? "ab+"_s : "ab"_s;
    if (state.isReadable)
        return state.isWritable ? "rb+"_s : "rb"_s;
    return "wb"_s;
}

// fileio_dealloc_warn()
void warnOfNotBeingClosed(JSGlobalObject* globalObject, JSValue self, JSValue source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = stateOfFile(self);
    if (state.descriptor < 0 || !state.closesDescriptor)
        return;
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, void());
    String shown = repr(globalObject, source);
    if (!scope.exception())
        warn(globalObject, BuiltinType::ResourceWarning, concatenate("unclosed file "_s, shown), 1, source);
    if (scope.exception()) {
        // What has nothing to do with the warning can come up when everything is being taken down.
        if (isInstance(globalObject, scope.exception()->value(), globalObject->pyRealm()->type(BuiltinType::Warning))) {
            Exception* warning = takeRaisedException(vm);
            String file = repr(globalObject, self);
            if (scope.exception() && !scope.tryClearException())
                return;
            restoreRaisedException(globalObject, warning);
            reportUnraisable(globalObject, concatenate("Exception ignored while finalizing file "_s, file));
        }
    }
    // PyErr_SetRaisedException(), which does away with whatever else there is.
    if (scope.exception() && !scope.tryClearException())
        return;
    restoreRaisedException(globalObject, raised);
}

} // anonymous namespace

bool isFileIOClosed(JSValue self)
{
    return stateOfFile(self).descriptor < 0;
}

PYTHON_NATIVE(fileIONew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<FileIOState>()));
}

// FileIO(file, mode='r', closefd=True, opener=None)
PYTHON_NATIVE(fileIOInit)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfFile(self);
    JSValue nameObject = args.at(1);
    String mode = "r"_s;
    if (JSValue value = args.at(2)) {
        auto given = toTextArgument(globalObject, value, "FileIO"_s, "argument 'mode'"_s);
        RETURN_IF_EXCEPTION(scope, { });
        mode = *given;
    }
    bool closesDescriptor = true;
    if (JSValue value = args.at(3)) {
        closesDescriptor = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue opener = args.at(4);
    if (!opener)
        opener = jsUndefined();

    if (state.descriptor >= 0) {
        if (state.closesDescriptor) {
            // What it had open is closed first.
            closeDescriptor(globalObject, state);
            RETURN_IF_EXCEPTION(scope, { });
        } else
            state.descriptor = -1;
    }

    if (nameObject.isBoolean()) {
        if (!warn(globalObject, BuiltinType::RuntimeWarning, "bool is used as a file descriptor"_s))
            return { };
    }
    // PyLong_AsInt(). What is no number is a name.
    int descriptor = -1;
    {
        auto given = toCInt(globalObject, nameObject);
        if (scope.exception()) {
            if (!scope.tryClearException())
                return { };
        } else {
            descriptor = *given;
            if (descriptor < 0)
                return JSValue::encode(raiseValueError(globalObject, scope, "negative file descriptor"_s));
        }
    }
    std::optional<CString> name;
    if (descriptor < 0) {
        name = toFileSystemPath(globalObject, nameObject, "embedded null byte"_s);
        RETURN_IF_EXCEPTION(scope, { });
    }

    bool ownsDescriptor = false;
    // What is at `error:`.
    auto fail = [&] () -> EncodedJSValue {
        if (!ownsDescriptor)
            state.descriptor = -1;
        if (state.descriptor >= 0) {
            Exception* raised = takeRaisedException(vm);
            closeDescriptor(globalObject, state);
            chainRaisedExceptions(globalObject, raised);
        }
        state.statusWhenOpened = std::nullopt;
        return { };
    };
    auto failBadMode = [&] {
        raiseValueError(globalObject, scope, "Must have exactly one of create/read/write/append mode and at most one plus"_s);
        return fail();
    };

    int flags = 0;
    bool hasKind = false;
    bool hasPlus = false;
    for (unsigned i = 0; i < mode.length(); ++i) {
        switch (mode[i]) {
        case 'x':
            if (hasKind)
                return failBadMode();
            hasKind = true;
            state.isCreated = true;
            state.isWritable = true;
            flags |= O_EXCL | O_CREAT;
            break;
        case 'r':
            if (hasKind)
                return failBadMode();
            hasKind = true;
            state.isReadable = true;
            break;
        case 'w':
            if (hasKind)
                return failBadMode();
            hasKind = true;
            state.isWritable = true;
            flags |= O_CREAT | O_TRUNC;
            break;
        case 'a':
            if (hasKind)
                return failBadMode();
            hasKind = true;
            state.isWritable = true;
            state.isAppending = true;
            flags |= O_APPEND | O_CREAT;
            break;
        case 'b':
            break;
        case '+':
            if (hasPlus)
                return failBadMode();
            state.isReadable = state.isWritable = true;
            hasPlus = true;
            break;
        default:
            raiseValueError(globalObject, scope, concatenate("invalid mode: "_s, mode));
            return fail();
        }
    }
    if (!hasKind)
        return failBadMode();

    if (state.isReadable && state.isWritable)
        flags |= O_RDWR;
    else if (state.isReadable)
        flags |= O_RDONLY;
    else
        flags |= O_WRONLY;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    if (!audit(globalObject, "open"_s, nameObject, jsString(vm, mode), jsNumber(flags)))
        return fail();

    if (descriptor >= 0) {
        state.descriptor = descriptor;
        state.closesDescriptor = closesDescriptor;
    } else {
        state.closesDescriptor = true;
        if (!closesDescriptor) {
            raiseValueError(globalObject, scope, "Cannot use closefd=False with file name"_s);
            return fail();
        }
        bool isKnownNotToBeInherited = false;
        if (isNone(opener)) {
            do {
                state.descriptor = files(globalObject).open(*name, flags, 0666);
                if (state.descriptor != -EINTR)
                    break;
                if (!checkSignals(globalObject))
                    return fail();
            } while (true);
            if (state.descriptor < 0) {
                raiseOSError(globalObject, scope, -std::exchange(state.descriptor, -1), nameObject);
                return fail();
            }
#ifdef O_CLOEXEC
            isKnownNotToBeInherited = true;
#endif
        } else {
            JSValue result = call(globalObject, opener, nameObject, jsNumber(flags));
            if (scope.exception())
                return fail();
            if (!isInstance(globalObject, result, realm->typeInt())) {
                raiseTypeError(globalObject, scope, "expected integer from opener"_s);
                return fail();
            }
            auto given = toCInt(globalObject, result);
            if (scope.exception())
                return fail();
            state.descriptor = *given;
            if (state.descriptor < 0) {
                raiseValueError(globalObject, scope, concatenate("opener returned "_s, std::exchange(state.descriptor, -1)));
                return fail();
            }
        }
        ownsDescriptor = true;
        if (!isKnownNotToBeInherited) {
            if (int result = files(globalObject).setInheritable(state.descriptor, false); result < 0) {
                raiseOSError(globalObject, scope, -result);
                return fail();
            }
        }
    }

    FileStatus status;
    if (int result = files(globalObject).status(state.descriptor, status); result < 0) {
        // That it cannot be asked about is put up with, unless it is because there is no such thing.
        if (result == -EBADF) {
            raiseOSError(globalObject, scope, EBADF);
            return fail();
        }
        state.statusWhenOpened = std::nullopt;
    } else {
        state.statusWhenOpened = status;
        // The system will open a directory. There is to be no file that is one.
        if (S_ISDIR(status.mode)) {
            raiseOSError(globalObject, scope, EISDIR, nameObject);
            return fail();
        }
    }

    setAttribute(globalObject, self, names.attribute_name, nameObject);
    if (scope.exception())
        return fail();

    if (state.isAppending) {
        // So that it is at the end from the first, and not only once something has been written.
        seekTo(globalObject, state, JSValue(), 2, true);
        if (scope.exception())
            return fail();
    }
    RETURN_NONE();
}

PYTHON_NATIVE(fileIOClose)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfFile(self);
    JSValue closeOfBase = getAttribute(globalObject, ioState(globalObject).rawIOBase.get(), names.attribute_close);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = call(globalObject, closeOfBase, self);
    if (!state.closesDescriptor) {
        state.descriptor = -1;
        return JSValue::encode(result);
    }
    Exception* raised = takeRaisedException(vm);
    RETURN_IF_EXCEPTION(scope, { });
    if (state.isFinalizing) {
        warnOfNotBeingClosed(globalObject, self, self);
        if (scope.exception() && !scope.tryClearException())
            return { };
    }
    closeDescriptor(globalObject, state);
    chainRaisedExceptions(globalObject, raised);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(fileIODeallocWarn)
{
    NATIVE_PROLOGUE();
    scope.release();
    warnOfNotBeingClosed(globalObject, args[0], args[1]);
    RETURN_NONE();
}

enum class FileAsk : uint8_t { Fileno, Readable, Writable, Seekable, IsATTY, IsATTYOpenOnly, Tell };

PYTHON_NATIVE(fileIOAsk)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfFile(args[0]);
    auto which = unpack<FileAsk>(callFrame, 0);
    if (which == FileAsk::IsATTYOpenOnly && state.statusWhenOpened && !S_ISCHR(state.statusWhenOpened->mode))
        return JSValue::encode(jsBoolean(false));
    if (state.descriptor < 0)
        return JSValue::encode(raiseClosed(globalObject, scope));
    switch (which) {
    case FileAsk::Fileno:
        return JSValue::encode(jsNumber(state.descriptor));
    case FileAsk::Readable:
        return JSValue::encode(jsBoolean(state.isReadable));
    case FileAsk::Writable:
        return JSValue::encode(jsBoolean(state.isWritable));
    case FileAsk::Seekable:
        if (state.isSeekable < 0) {
            // Which finds out.
            seekTo(globalObject, state, JSValue(), SEEK_CUR, false);
            if (scope.exception() && !scope.tryClearException())
                return { };
        }
        return JSValue::encode(jsBoolean(state.isSeekable > 0));
    case FileAsk::IsATTY:
    case FileAsk::IsATTYOpenOnly:
        return JSValue::encode(jsBoolean(files(globalObject).isTerminal(state.descriptor)));
    case FileAsk::Tell:
        RELEASE_AND_RETURN(scope, JSValue::encode(seekTo(globalObject, state, JSValue(), 1, false)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PYTHON_NATIVE(fileIOReadInto)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfFile(args[0]);
    Buffer buffer = writableBufferArgument(globalObject, args[1], "readinto"_s, "argument 1"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (state.descriptor < 0)
        return JSValue::encode(raiseClosed(globalObject, scope));
    if (!state.isReadable)
        return JSValue::encode(raiseWrongMode(globalObject, scope, "reading"_s));
    int64_t count = readFrom(globalObject, state.descriptor, mutableSpanOf(buffer));
    if (count < 0) {
        if (wouldBlock(globalObject, count))
            RETURN_NONE();
        return { };
    }
    return JSValue::encode(intFromInt64(globalObject, count));
}

// new_buffersize()
static size_t largerBufferSize(size_t current)
{
    // By an amount that goes with how large it is, so that it comes to no more work than there are bytes. When it is large, by less than as much again.
    size_t more = current > largeBufferCutoff ? current >> 3 : 256 + current;
    return std::max(more, smallChunk) + current;
}

static JSValue readAll(JSGlobalObject* globalObject, FileIOState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (state.descriptor < 0)
        return raiseClosed(globalObject, scope);
    int64_t end = state.statusWhenOpened && state.statusWhenOpened->size < mostToRead ? state.statusWhenOpened->size : -1;
    size_t bufferSize;
    if (end <= 0)
        bufferSize = smallChunk;
    else {
        // It is likely a file. That there is no more is found by reading and getting nothing, so there is room for one byte more than there should be, and it takes two reads and no more room.
        bufferSize = static_cast<size_t>(end) + 1;
        // Some of it may have been read already, which is worth asking about if there is a great deal of it.
        if (bufferSize > largeBufferCutoff) {
            int64_t position = files(globalObject).seek(state.descriptor, 0, SEEK_CUR);
            if (end >= position && position >= 0)
                bufferSize = static_cast<size_t>(end - position) + 1;
        }
    }
    Vector<uint8_t> result;
    if (!result.tryGrow(bufferSize))
        return raiseMemoryError(globalObject, scope);
    size_t bytesRead = 0;
    while (true) {
        if (bytesRead >= bufferSize) {
            bufferSize = largerBufferSize(bytesRead);
            if (bufferSize > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
                return raise(globalObject, scope, BuiltinType::OverflowError, "unbounded read returned more bytes than a Python bytes object can hold"_s);
            if (result.size() < bufferSize && !result.tryGrow(bufferSize))
                return raiseMemoryError(globalObject, scope);
        }
        int64_t count = readFrom(globalObject, state.descriptor, result.mutableSpan().subspan(bytesRead, bufferSize - bytesRead));
        if (!count)
            break;
        if (count < 0) {
            if (wouldBlock(globalObject, count)) {
                if (bytesRead)
                    break;
                return jsUndefined();
            }
            return { };
        }
        bytesRead += static_cast<size_t>(count);
    }
    RELEASE_AND_RETURN(scope, newBytes(globalObject, result.span().first(bytesRead)));
}

PYTHON_NATIVE(fileIOReadAll)
{
    NativeArguments args(callFrame);
    return JSValue::encode(readAll(globalObject, stateOfFile(args[0])));
}

PYTHON_NATIVE(fileIORead)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfFile(args[0]);
    auto size = toOptionalSsize(globalObject, args.at(1), -1);
    RETURN_IF_EXCEPTION(scope, { });
    if (state.descriptor < 0)
        return JSValue::encode(raiseClosed(globalObject, scope));
    if (!state.isReadable)
        return JSValue::encode(raiseWrongMode(globalObject, scope, "reading"_s));
    if (*size < 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(readAll(globalObject, state)));
    Vector<uint8_t> bytes;
    if (!bytes.tryGrow(static_cast<size_t>(*size)))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    int64_t count = readFrom(globalObject, state.descriptor, bytes.mutableSpan());
    if (count < 0) {
        if (wouldBlock(globalObject, count))
            RETURN_NONE();
        return { };
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, bytes.span().first(static_cast<size_t>(count)))));
}

PYTHON_NATIVE(fileIOWrite)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfFile(args[0]);
    Buffer buffer = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (state.descriptor < 0)
        return JSValue::encode(raiseClosed(globalObject, scope));
    if (!state.isWritable)
        return JSValue::encode(raiseWrongMode(globalObject, scope, "writing"_s));
    int64_t count = writeTo(globalObject, state.descriptor, buffer.span());
    if (count < 0) {
        if (wouldBlock(globalObject, count))
            RETURN_NONE();
        return { };
    }
    return JSValue::encode(intFromInt64(globalObject, count));
}

PYTHON_NATIVE(fileIOSeek)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfFile(args[0]);
    int whence = 0;
    if (JSValue value = args.at(2)) {
        auto given = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        whence = *given;
    }
    if (state.descriptor < 0)
        return JSValue::encode(raiseClosed(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(seekTo(globalObject, state, args[1], whence, false)));
}

PYTHON_NATIVE(fileIOTruncate)
{
    NATIVE_PROLOGUE();
    auto& state = stateOfFile(args[0]);
    JSValue position = args.at(1);
    if (state.descriptor < 0)
        return JSValue::encode(raiseClosed(globalObject, scope));
    if (!state.isWritable)
        return JSValue::encode(raiseWrongMode(globalObject, scope, "writing"_s));
    if (!position || isNone(position)) {
        position = seekTo(globalObject, state, JSValue(), 1, false);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto size = toCLong(globalObject, position);
    RETURN_IF_EXCEPTION(scope, { });
    if (int result = files(globalObject).truncate(state.descriptor, *size); result < 0)
        return JSValue::encode(raiseOSError(globalObject, scope, -result));
    // How large it was when it was opened is nothing to go by any longer.
    state.statusWhenOpened = std::nullopt;
    return JSValue::encode(position);
}

PYTHON_NATIVE(fileIORepr)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    auto& state = stateOfFile(self);
    String type = typeName(globalObject, self);
    if (state.descriptor < 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', type, " [closed]>"_s))));
    JSValue name = getAttributeIfPresent(globalObject, self, names.attribute_name);
    RETURN_IF_EXCEPTION(scope, { });
    ASCIILiteral closes = state.closesDescriptor ? "True"_s : "False"_s;
    if (!name)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', type, " fd="_s, state.descriptor, " mode='"_s, modeOf(state), "' closefd="_s, closes, '>'))));
    ReprGuard guard(globalObject, self.asCell());
    if (guard.isRecursive())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("reentrant call inside "_s, type, ".__repr__"_s)));
    String shown = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate('<', type, " name="_s, shown, " mode='"_s, modeOf(state), "' closefd="_s, closes, '>'))));
}

void initializeFileIO(JSGlobalObject* globalObject, IOModuleState& state)
{
    VM& vm = globalObject->vm();
    using Kind = PyNativeFunction::Kind;
    constexpr auto withDefiningClass = PyNativeFunction::Arguments::AreCheckedAsWithDefiningClass;
    PyType* type = createBuiltinType(globalObject, "_io.FileIO"_s, state.rawIOBase.get(), PyType::Layout::Native, PyType::IsBaseType);
    state.fileIO.set(vm, globalObject->pyRealm(), type);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    addMethods(globalObject, type, {
        { "__new__"_s, fileIONew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__init__"_s, fileIOInit, Kind::Wrapper, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, fileIORepr },
        { "read"_s, fileIORead, Kind::Method, 0, { }, withDefiningClass },
        { "readall"_s, fileIOReadAll },
        { "readinto"_s, fileIOReadInto, Kind::Method, 0, { }, withDefiningClass },
        { "write"_s, fileIOWrite, Kind::Method, 0, { }, withDefiningClass },
        { "seek"_s, fileIOSeek },
        { "tell"_s, fileIOAsk, Kind::Method, pack(FileAsk::Tell) },
        { "truncate"_s, fileIOTruncate, Kind::Method, 0, { }, withDefiningClass },
        { "close"_s, fileIOClose, Kind::Method, 0, { }, withDefiningClass },
        { "seekable"_s, fileIOAsk, Kind::Method, pack(FileAsk::Seekable) },
        { "readable"_s, fileIOAsk, Kind::Method, pack(FileAsk::Readable) },
        { "writable"_s, fileIOAsk, Kind::Method, pack(FileAsk::Writable) },
        { "fileno"_s, fileIOAsk, Kind::Method, pack(FileAsk::Fileno) },
        { "isatty"_s, fileIOAsk, Kind::Method, pack(FileAsk::IsATTY) },
        { "_isatty_open_only"_s, fileIOAsk, Kind::Method, pack(FileAsk::IsATTYOpenOnly) },
        { "_dealloc_warn"_s, fileIODeallocWarn },
        { "__getstate__"_s, ioCannotPickle },
    });
    addGetSet(globalObject, type, "closed"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfFile(self).descriptor < 0); });
    addGetSet(globalObject, type, "closefd"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfFile(self).closesDescriptor); });
    addGetSet(globalObject, type, "mode"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), String(modeOf(stateOfFile(self)))); });
    addGetSet(globalObject, type, "_blksize"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto& status = stateOfFile(self).statusWhenOpened;
        return intFromInt64(globalObject, status && status->blockSize > 1 ? status->blockSize : defaultBufferSize);
    });
    addMember(globalObject, type, "_finalizing"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsBoolean(stateOfFile(self).isFinalizing); }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        toBoolMember(globalObject, value, stateOfFile(self).isFinalizing);
    });
}

} } // namespace JSC::Python
