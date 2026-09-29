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

#include "PythonCodecs.h"
#include "PythonTime.h"
#include <dirent.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/time.h>
#include <unistd.h>
#include <wtf/Scope.h>
#include <sys/statvfs.h>
#if OS(DARWIN)
#include <sys/mount.h>
#else
#include <sys/sysmacros.h>
#endif

// The functions of posix that take the name of a file: Modules/posixmodule.c of CPython.

namespace JSC { namespace Python {

#define CONVERT_PATH(variable, value, function, argument, options) \
    PathArgument variable(function, argument, options); \
    if (!variable.convert(globalObject, value)) \
        return { };
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

static constexpr unsigned allowsDescriptor = PathArgument::AllowsDescriptor;
static constexpr unsigned nullable = PathArgument::Nullable;

static JSValue auditedDirectory(int directory) { return jsNumber(directory == defaultDirectoryDescriptor ? -1 : directory); }

// path_and_dir_fd_invalid(), dir_fd_and_fd_invalid() and fd_and_follow_symlinks_invalid(). Each is true if it raised.
static bool pathAndDirectoryAreInvalid(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, const PathArgument& path, int directory)
{
    if (path.hasNarrow || directory == defaultDirectoryDescriptor)
        return false;
    raiseValueError(globalObject, scope, concatenate(function, ": can't specify dir_fd without matching path"_s));
    return true;
}

static bool directoryAndDescriptorAreInvalid(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, int directory, int descriptor)
{
    if (directory == defaultDirectoryDescriptor || descriptor == -1)
        return false;
    raiseValueError(globalObject, scope, concatenate(function, ": can't specify both dir_fd and fd"_s));
    return true;
}

static bool descriptorAndFollowingAreInvalid(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, int descriptor, bool followsSymlinks)
{
    if (descriptor < 0 || followsSymlinks)
        return false;
    raiseValueError(globalObject, scope, concatenate(function, ": cannot use fd and follow_symlinks together"_s));
    return true;
}

// posix_do_stat()
static JSValue doStat(JSGlobalObject* globalObject, const PathArgument& path, int directory, bool followsSymlinks)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (pathAndDirectoryAreInvalid(globalObject, scope, "stat"_s, path, directory) || directoryAndDescriptorAreInvalid(globalObject, scope, "stat"_s, directory, path.descriptor) || descriptorAndFollowingAreInvalid(globalObject, scope, "stat"_s, path.descriptor, followsSymlinks))
        return { };
    struct stat status;
    int result;
    if (path.descriptor != -1)
        result = ::fstat(path.descriptor, &status);
    else if (!followsSymlinks && directory == defaultDirectoryDescriptor)
        result = ::lstat(path.narrow(), &status);
    else if (directory != defaultDirectoryDescriptor || !followsSymlinks)
        result = ::fstatat(directory, path.narrow(), &status, followsSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = ::stat(path.narrow(), &status);
    if (result)
        return raisePathError(globalObject, scope, path);
    RELEASE_AND_RETURN(scope, statResultFrom(globalObject, status));
}

PYTHON_NATIVE(posixStat)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "stat"_s, "path"_s, allowsDescriptor);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(1)));
    CONVERT_BOOL(followsSymlinks, args.at(2), true);
    RELEASE_AND_RETURN(scope, JSValue::encode(doStat(globalObject, path, directory, followsSymlinks)));
}

PYTHON_NATIVE(posixLstat)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "lstat"_s, "path"_s, 0);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(1)));
    RELEASE_AND_RETURN(scope, JSValue::encode(doStat(globalObject, path, directory, false)));
}

PYTHON_NATIVE(posixAccess)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "access"_s, "path"_s, 0);
    CONVERT(mode, toCInt(globalObject, args.at(1)));
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(2)));
    CONVERT_BOOL(usesEffectiveIDs, args.at(3), false);
    CONVERT_BOOL(followsSymlinks, args.at(4), true);
    int result;
    if (directory != defaultDirectoryDescriptor || usesEffectiveIDs || !followsSymlinks)
        result = ::faccessat(directory, path.narrow(), mode, (followsSymlinks ? 0 : AT_SYMLINK_NOFOLLOW) | (usesEffectiveIDs ? AT_EACCESS : 0));
    else
        result = ::access(path.narrow(), mode);
    return JSValue::encode(jsBoolean(!result));
}

PYTHON_NATIVE(posixChdir)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "chdir"_s, "path"_s, allowsDescriptor);
    if (!audit(globalObject, "os.chdir"_s, path.object))
        return { };
    if (path.descriptor != -1 ? ::fchdir(path.descriptor) : ::chdir(path.narrow()))
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

PYTHON_NATIVE(posixChmod)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "chmod"_s, "path"_s, allowsDescriptor);
    CONVERT(mode, toCInt(globalObject, args.at(1)));
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(2)));
    CONVERT_BOOL(followsSymlinks, args.at(3), true);
    if (!audit(globalObject, "os.chmod"_s, path.object, jsNumber(mode), auditedDirectory(directory)))
        return { };
    int result;
    bool cannotLeaveSymlinks = false;
    if (path.descriptor != -1)
        result = ::fchmod(path.descriptor, mode);
#if OS(DARWIN)
    else if (!followsSymlinks && directory == defaultDirectoryDescriptor)
        result = ::lchmod(path.narrow(), mode);
#endif
    else if (directory != defaultDirectoryDescriptor || !followsSymlinks) {
        result = ::fchmodat(directory, path.narrow(), mode, followsSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
        cannotLeaveSymlinks = result && (errno == ENOTSUP || errno == EOPNOTSUPP) && !followsSymlinks;
    } else
        result = ::chmod(path.narrow(), mode);
    if (!result)
        RETURN_NONE();
    if (cannotLeaveSymlinks) {
        if (directory != defaultDirectoryDescriptor)
            return JSValue::encode(raiseValueError(globalObject, scope, "chmod: cannot use dir_fd and follow_symlinks together"_s));
        return JSValue::encode(raiseArgumentUnavailable(globalObject, scope, "chmod"_s, "follow_symlinks"_s));
    }
    return JSValue::encode(raisePathError(globalObject, scope, path));
}

#if OS(DARWIN)
PYTHON_NATIVE(posixLchmod)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "lchmod"_s, "path"_s, 0);
    CONVERT(mode, toCInt(globalObject, args.at(1)));
    if (!audit(globalObject, "os.chmod"_s, path.object, jsNumber(mode), jsNumber(-1)))
        return { };
    if (::lchmod(path.narrow(), mode) < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// `unsigned_long(bitwise=True)`
static std::optional<unsigned long> toUnsignedLongMask(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, ASCIILiteral function, ASCIILiteral argument)
{
    if (!isInstance(globalObject, value, globalObject->pyRealm()->typeInt())) {
        raiseTypeError(globalObject, scope, concatenate(function, "() argument '"_s, argument, "' must be int, not "_s, typeNameOfArgument(globalObject, value)));
        return std::nullopt;
    }
    return static_cast<unsigned long>(lowBitsOfInt(value));
}

// chflags(path, flags, follow_symlinks=True) and lchflags(path, flags)
PYTHON_NATIVE(posixChflags)
{
    NATIVE_PROLOGUE();
    bool isForLink = unpack<bool>(callFrame, 0);
    CONVERT_PATH(path, args.at(0), isForLink ? "lchflags"_s : "chflags"_s, "path"_s, 0);
    CONVERT(flags, toUnsignedLongMask(globalObject, scope, args.at(1), isForLink ? "lchflags"_s : "chflags"_s, "flags"_s));
    CONVERT_BOOL(followsSymlinks, isForLink ? JSValue() : args.at(2), !isForLink);
    if (!audit(globalObject, "os.chflags"_s, path.object, intFromUInt64(globalObject, flags)))
        return { };
    if (followsSymlinks ? ::chflags(path.narrow(), static_cast<unsigned>(flags)) : ::lchflags(path.narrow(), static_cast<unsigned>(flags)))
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}
#endif

PYTHON_NATIVE(posixChroot)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "chroot"_s, "path"_s, 0);
    if (::chroot(path.narrow()) < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

PYTHON_NATIVE(posixChown)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "chown"_s, "path"_s, allowsDescriptor);
    CONVERT(user, toUserID(globalObject, args.at(1)));
    CONVERT(group, toGroupID(globalObject, args.at(2)));
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(3)));
    CONVERT_BOOL(followsSymlinks, args.at(4), true);
    if (directoryAndDescriptorAreInvalid(globalObject, scope, "chown"_s, directory, path.descriptor) || descriptorAndFollowingAreInvalid(globalObject, scope, "chown"_s, path.descriptor, followsSymlinks))
        return { };
    if (!audit(globalObject, "os.chown"_s, path.object, intFromInt64(globalObject, user), intFromInt64(globalObject, group), auditedDirectory(directory)))
        return { };
    int result;
    if (path.descriptor != -1)
        result = ::fchown(path.descriptor, user, group);
    else if (!followsSymlinks && directory == defaultDirectoryDescriptor)
        result = ::lchown(path.narrow(), user, group);
    else if (directory != defaultDirectoryDescriptor || !followsSymlinks)
        result = ::fchownat(directory, path.narrow(), user, group, followsSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = ::chown(path.narrow(), user, group);
    if (result)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

PYTHON_NATIVE(posixLchown)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "lchown"_s, "path"_s, 0);
    CONVERT(user, toUserID(globalObject, args.at(1)));
    CONVERT(group, toGroupID(globalObject, args.at(2)));
    if (!audit(globalObject, "os.chown"_s, path.object, intFromInt64(globalObject, user), intFromInt64(globalObject, group), jsNumber(-1)))
        return { };
    if (::lchown(path.narrow(), user, group) < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// getcwd() and getcwdb()
PYTHON_NATIVE(posixGetcwd)
{
    NATIVE_PROLOGUE();
    Vector<char> buffer;
    char* result;
    do {
        if (!buffer.tryGrow(buffer.size() + 1024))
            return JSValue::encode(raiseMemoryError(globalObject, scope));
        result = ::getcwd(buffer.mutableSpan().data(), buffer.size());
    } while (!result && errno == ERANGE);
    if (!result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    auto characters = unsafeSpan(result);
    if (unpack<bool>(callFrame, 0))
        RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, byteCast<uint8_t>(characters))));
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, characters)));
}

PYTHON_NATIVE(posixLink)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(source, args.at(0), "link"_s, "src"_s, 0);
    CONVERT_PATH(destination, args.at(1), "link"_s, "dst"_s, 0);
    CONVERT(sourceDirectory, toDirectoryDescriptor(globalObject, args.at(2)));
    CONVERT(destinationDirectory, toDirectoryDescriptor(globalObject, args.at(3)));
    CONVERT_BOOL(followsSymlinks, args.at(4), true);
    if (!audit(globalObject, "os.link"_s, source.object, destination.object, auditedDirectory(sourceDirectory), auditedDirectory(destinationDirectory)))
        return { };
    if (::linkat(sourceDirectory, source.narrow(), destinationDirectory, destination.narrow(), followsSymlinks ? AT_SYMLINK_FOLLOW : 0))
        return JSValue::encode(raisePathError(globalObject, scope, source, destination));
    RETURN_NONE();
}

// _Py_dup(): another descriptor for the same, which what is started from here does not get. -1 if it raised.
int duplicateDescriptor(JSGlobalObject* globalObject, int descriptor)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int result = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if (result < 0)
        raisePosixError(globalObject, scope);
    return result;
}

static bool isDotOrDotDot(const struct dirent* entry)
{
    const char* name = entry->d_name;
    return name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]));
}

PYTHON_NATIVE(posixListdir)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "listdir"_s, "path"_s, nullable | allowsDescriptor);
    if (!audit(globalObject, "os.listdir"_s, path.object ? path.object : jsUndefined()))
        return { };
    errno = 0;
    DIR* directory;
    int descriptor = -1;
    if (path.descriptor != -1) {
        // closedir() closes it, so it is another that is given up.
        descriptor = duplicateDescriptor(globalObject, path.descriptor);
        if (descriptor == -1)
            return { };
        directory = ::fdopendir(descriptor);
    } else
        directory = ::opendir(path.hasNarrow ? path.narrow() : ".");
    if (!directory) {
        raisePathError(globalObject, scope, path);
        if (descriptor != -1)
            ::close(descriptor);
        return { };
    }
    auto close = makeScopeExit([&] {
        if (descriptor > -1)
            ::rewinddir(directory);
        ::closedir(directory);
    });
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        errno = 0;
        struct dirent* entry = ::readdir(directory);
        if (!entry) {
            if (!errno)
                break;
            return JSValue::encode(raisePathError(globalObject, scope, path));
        }
        if (isDotOrDotDot(entry))
            continue;
        JSValue name = nameLike(globalObject, path, unsafeSpan(entry->d_name));
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(list);
}

PYTHON_NATIVE(posixMkdir)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "mkdir"_s, "path"_s, 0);
    CONVERT_INT_OR(mode, args.at(1), 0777);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(2)));
    if (!audit(globalObject, "os.mkdir"_s, path.object, jsNumber(mode), auditedDirectory(directory)))
        return { };
    if ((directory != defaultDirectoryDescriptor ? ::mkdirat(directory, path.narrow(), mode) : ::mkdir(path.narrow(), mode)) < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// rename() and replace(): internal_rename()
PYTHON_NATIVE(posixRename)
{
    NATIVE_PROLOGUE();
    ASCIILiteral function = unpack<bool>(callFrame, 0) ? "replace"_s : "rename"_s;
    CONVERT_PATH(source, args.at(0), function, "src"_s, 0);
    CONVERT_PATH(destination, args.at(1), function, "dst"_s, 0);
    CONVERT(sourceDirectory, toDirectoryDescriptor(globalObject, args.at(2)));
    CONVERT(destinationDirectory, toDirectoryDescriptor(globalObject, args.at(3)));
    if (!audit(globalObject, "os.rename"_s, source.object, destination.object, auditedDirectory(sourceDirectory), auditedDirectory(destinationDirectory)))
        return { };
    bool hasDirectory = sourceDirectory != defaultDirectoryDescriptor || destinationDirectory != defaultDirectoryDescriptor;
    if (hasDirectory ? ::renameat(sourceDirectory, source.narrow(), destinationDirectory, destination.narrow()) : ::rename(source.narrow(), destination.narrow()))
        return JSValue::encode(raisePathError(globalObject, scope, source, destination));
    RETURN_NONE();
}

PYTHON_NATIVE(posixRmdir)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "rmdir"_s, "path"_s, 0);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(1)));
    if (!audit(globalObject, "os.rmdir"_s, path.object, auditedDirectory(directory)))
        return { };
    if (directory != defaultDirectoryDescriptor ? ::unlinkat(directory, path.narrow(), AT_REMOVEDIR) : ::rmdir(path.narrow()))
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// unlink() and remove()
PYTHON_NATIVE(posixUnlink)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), unpack<bool>(callFrame, 0) ? "remove"_s : "unlink"_s, "path"_s, 0);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(1)));
    if (!audit(globalObject, "os.remove"_s, path.object, auditedDirectory(directory)))
        return { };
    if (directory != defaultDirectoryDescriptor ? ::unlinkat(directory, path.narrow(), 0) : ::unlink(path.narrow()))
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

PYTHON_NATIVE(posixSymlink)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(source, args.at(0), "symlink"_s, "src"_s, 0);
    CONVERT_PATH(destination, args.at(1), "symlink"_s, "dst"_s, 0);
    CONVERT_BOOL(targetIsDirectory, args.at(2), false);
    UNUSED_VARIABLE(targetIsDirectory);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(3)));
    if (!audit(globalObject, "os.symlink"_s, source.object, destination.object, auditedDirectory(directory)))
        return { };
    if (directory != defaultDirectoryDescriptor ? ::symlinkat(source.narrow(), directory, destination.narrow()) : ::symlink(source.narrow(), destination.narrow()))
        return JSValue::encode(raisePathError(globalObject, scope, source, destination));
    RETURN_NONE();
}

PYTHON_NATIVE(posixReadlink)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "readlink"_s, "path"_s, 0);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(1)));
    std::array<char, MAXPATHLEN + 1> buffer;
    ssize_t length = directory != defaultDirectoryDescriptor ? ::readlinkat(directory, path.narrow(), buffer.data(), MAXPATHLEN) : ::readlink(path.narrow(), buffer.data(), MAXPATHLEN);
    if (length < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    auto characters = std::span<const char>(buffer).first(static_cast<size_t>(length));
    if (stringIn(path.object))
        RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, characters)));
    RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, byteCast<uint8_t>(characters))));
}

// ftruncate(), which truncate() is too if it is given an open file. False if it raised.
bool truncateDescriptor(JSGlobalObject* globalObject, int descriptor, int64_t length)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!audit(globalObject, "os.truncate"_s, jsNumber(descriptor), intFromInt64(globalObject, length)))
        return false;
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(::ftruncate(descriptor, length)); }))
        return false;
    if (result)
        raisePosixError(globalObject, scope);
    return !result;
}

PYTHON_NATIVE(posixTruncate)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "truncate"_s, "path"_s, allowsDescriptor);
    CONVERT(length, toFileOffset(globalObject, args.at(1)));
    if (path.descriptor != -1) {
        scope.release();
        if (!truncateDescriptor(globalObject, path.descriptor, length))
            return { };
        RETURN_NONE();
    }
    if (!audit(globalObject, "os.truncate"_s, path.object, intFromInt64(globalObject, length)))
        return { };
    if (::truncate(path.narrow(), length) < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// ---- utime()

// _PyTime_ObjectToTimespec(), rounding down
static bool toTimespec(JSGlobalObject* globalObject, JSValue value, struct timespec& result)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isInstance(globalObject, value, globalObject->pyRealm()->typeFloat())) {
        double seconds = classify(value).real;
        if (std::isnan(seconds)) {
            raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
            return false;
        }
        // pytime_double_to_denominator()
        double whole;
        double fraction = std::floor(std::modf(seconds, &whole) * 1e9);
        if (fraction >= 1e9) {
            fraction -= 1e9;
            whole += 1.0;
        } else if (fraction < 0) {
            fraction += 1e9;
            whole -= 1.0;
        }
        constexpr double least = static_cast<double>(std::numeric_limits<time_t>::min());
        if (!(least <= whole && whole < -least)) {
            raise(globalObject, scope, BuiltinType::OverflowError, "timestamp out of range for platform time_t"_s);
            return false;
        }
        result.tv_sec = static_cast<time_t>(whole);
        result.tv_nsec = static_cast<long>(fraction);
        return true;
    }
    // _PyLong_AsTime_t()
    JSValue integer = toInt(globalObject, value);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::TypeError)) {
            String type = fullyQualifiedTypeName(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            raiseTypeError(globalObject, scope, concatenate("argument must be int or float, not "_s, type));
        }
        return false;
    }
    auto seconds = tryInt64(integer);
    if (!seconds) {
        raise(globalObject, scope, BuiltinType::OverflowError, "timestamp out of range for platform time_t"_s);
        return false;
    }
    result.tv_sec = *seconds;
    result.tv_nsec = 0;
    return true;
}

// split_py_long_to_s_and_ns()
static bool splitNanoseconds(JSGlobalObject* globalObject, JSValue value, struct timespec& result)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue pair = divmod(globalObject, value, jsNumber(1000000000));
    RETURN_IF_EXCEPTION(scope, false);
    if (!isInstance(globalObject, pair, globalObject->pyRealm()->typeTuple()) || asTuple(pair)->length() != 2) {
        raiseTypeError(globalObject, scope, concatenate(typeOf(globalObject, value)->nameWithoutModule(globalObject), ".__divmod__() must return a 2-tuple, not "_s, typeOf(globalObject, pair)->nameWithoutModule(globalObject)));
        return false;
    }
    JSValue integer = toInt(globalObject, asTuple(pair)->at(0));
    RETURN_IF_EXCEPTION(scope, false);
    auto seconds = tryInt64(integer);
    if (!seconds) {
        raise(globalObject, scope, BuiltinType::OverflowError, "timestamp out of range for platform time_t"_s);
        return false;
    }
    auto nanoseconds = toCLong(globalObject, asTuple(pair)->at(1));
    RETURN_IF_EXCEPTION(scope, false);
    result.tv_sec = *seconds;
    result.tv_nsec = static_cast<long>(*nanoseconds);
    return true;
}

PYTHON_NATIVE(posixUtime)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "utime"_s, "path"_s, allowsDescriptor);
    JSValue times = args.at(1);
    if (!times)
        times = jsUndefined();
    JSValue nanoseconds = args.at(2);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(3)));
    CONVERT_BOOL(followsSymlinks, args.at(4), true);
    auto isPair = [&] (JSValue value) { return typeOf(globalObject, value) == realm->typeTuple() && asTuple(value)->length() == 2; };
    struct timespec values[2];
    bool isNow = false;
    if (!isNone(times) && nanoseconds)
        return JSValue::encode(raiseValueError(globalObject, scope, "utime: you may specify either 'times' or 'ns' but not both"_s));
    if (!isNone(times)) {
        if (!isPair(times))
            return JSValue::encode(raiseTypeError(globalObject, scope, "utime: 'times' must be either a tuple of two ints or None"_s));
        if (!toTimespec(globalObject, asTuple(times)->at(0), values[0]) || !toTimespec(globalObject, asTuple(times)->at(1), values[1]))
            return { };
    } else if (nanoseconds) {
        if (!isPair(nanoseconds))
            return JSValue::encode(raiseTypeError(globalObject, scope, "utime: 'ns' must be a tuple of two ints"_s));
        if (!splitNanoseconds(globalObject, asTuple(nanoseconds)->at(0), values[0]) || !splitNanoseconds(globalObject, asTuple(nanoseconds)->at(1), values[1]))
            return { };
    } else
        isNow = true;
    if (pathAndDirectoryAreInvalid(globalObject, scope, "utime"_s, path, directory) || directoryAndDescriptorAreInvalid(globalObject, scope, "utime"_s, directory, path.descriptor) || descriptorAndFollowingAreInvalid(globalObject, scope, "utime"_s, path.descriptor, followsSymlinks))
        return { };
    if (!audit(globalObject, "os.utime"_s, path.object, times, nanoseconds ? nanoseconds : jsUndefined(), auditedDirectory(directory)))
        return { };
    const struct timespec* given = isNow ? nullptr : values;
    int result;
    if (path.descriptor != -1 && followsSymlinks && directory == defaultDirectoryDescriptor)
        result = ::futimens(path.descriptor, given);
    else
        result = ::utimensat(directory, path.narrow(), given, followsSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    if (result < 0)
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RETURN_NONE();
}

// ---- Special files

// _Py_Dev_Converter()
std::optional<dev_t> toDevice(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (compareInts(integer, jsNumber(0)) < 0) {
#ifdef NODEV
        if (auto number = tryInt64(integer); number && *number == static_cast<int64_t>(NODEV))
            return static_cast<dev_t>(NODEV);
#endif
        raise(globalObject, scope, BuiltinType::OverflowError, "can't convert negative int to unsigned"_s);
        return std::nullopt;
    }
    uint64_t low = lowBitsOfInt(integer);
    if (compareInts(integer, intFromUInt64(globalObject, low))) {
        raise(globalObject, scope, BuiltinType::OverflowError, "int too big to convert"_s);
        return std::nullopt;
    }
    if (static_cast<uint64_t>(static_cast<dev_t>(low)) != low) {
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C dev_t"_s);
        return std::nullopt;
    }
    return static_cast<dev_t>(low);
}

PYTHON_NATIVE(posixMkfifo)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "mkfifo"_s, "path"_s, 0);
    CONVERT_INT_OR(mode, args.at(1), 0666);
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(2)));
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(directory != defaultDirectoryDescriptor ? ::mkfifoat(directory, path.narrow(), mode) : ::mkfifo(path.narrow(), mode)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(posixMknod)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "mknod"_s, "path"_s, 0);
    CONVERT_INT_OR(mode, args.at(1), 0600);
    dev_t device = 0;
    if (JSValue value = args.at(2)) {
        CONVERT(given, toDevice(globalObject, value));
        device = given;
    }
    CONVERT(directory, toDirectoryDescriptor(globalObject, args.at(3)));
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(directory != defaultDirectoryDescriptor ? ::mknodat(directory, path.narrow(), mode, device) : ::mknod(path.narrow(), mode, device)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RETURN_NONE();
}

// ---- statvfs()

#if OS(DARWIN)
// The system's own statvfs() counts blocks in 32 bits.
using FileSystemStatus = struct statfs;
static int fileSystemStatus(const char* path, FileSystemStatus& status) { return ::statfs(path, &status); }
static int fileSystemStatus(int descriptor, FileSystemStatus& status) { return ::fstatfs(descriptor, &status); }
#else
using FileSystemStatus = struct statvfs;
static int fileSystemStatus(const char* path, FileSystemStatus& status) { return ::statvfs(path, &status); }
static int fileSystemStatus(int descriptor, FileSystemStatus& status) { return ::fstatvfs(descriptor, &status); }
#endif

static JSValue statVFSResultFrom(JSGlobalObject* globalObject, const FileSystemStatus& status)
{
    MarkedArgumentBuffer values;
    auto append = [&] (int64_t value) { values.append(intFromInt64(globalObject, value)); };
#if OS(DARWIN)
    append(status.f_iosize);
    append(status.f_bsize);
    append(status.f_blocks);
    append(status.f_bfree);
    append(status.f_bavail);
    append(status.f_files);
    append(status.f_ffree);
    append(status.f_ffree);
    append(((status.f_flags & MNT_RDONLY) ? ST_RDONLY : 0) | ((status.f_flags & MNT_NOSUID) ? ST_NOSUID : 0));
    append(NAME_MAX);
    values.append(intFromUInt64(globalObject, static_cast<unsigned long>(status.f_fsid.val[0])));
#else
    append(status.f_bsize);
    append(status.f_frsize);
    append(status.f_blocks);
    append(status.f_bfree);
    append(status.f_bavail);
    append(status.f_files);
    append(status.f_ffree);
    append(status.f_favail);
    append(status.f_flag);
    append(status.f_namemax);
    values.append(intFromUInt64(globalObject, status.f_fsid));
#endif
    return newStructSequence(globalObject, posixState(globalObject).statVFSResult.get(), values);
}

PYTHON_NATIVE(posixStatvfs)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "statvfs"_s, "path"_s, allowsDescriptor);
    FileSystemStatus status;
    if (path.descriptor != -1 ? fileSystemStatus(path.descriptor, status) : fileSystemStatus(path.narrow(), status))
        return JSValue::encode(raisePathError(globalObject, scope, path));
    RELEASE_AND_RETURN(scope, JSValue::encode(statVFSResultFrom(globalObject, status)));
}

PYTHON_NATIVE(posixFstatvfs)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCInt(globalObject, args.at(0)));
    FileSystemStatus status;
    int result;
    if (!retryIfInterrupted(globalObject, result, [&] { return -std::abs(fileSystemStatus(descriptor, status)); }))
        return { };
    if (result)
        return JSValue::encode(raisePosixError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(statVFSResultFrom(globalObject, status)));
}

// ---- What is only a matter of names

PYTHON_NATIVE(posixFspath)
{
    NativeArguments args(callFrame);
    return JSValue::encode(fileSystemPathOf(globalObject, args.at(0)));
}

// _Py_skiproot(): how much of a name is the root.
static size_t rootSizeOf(std::span<const char> path)
{
    auto isSeparator = [&] (size_t i) { return i < path.size() && path[i] == '/'; };
    if (!isSeparator(0))
        return 0;
    // Just two slashes at the beginning are for the system to make what it likes of.
    return isSeparator(1) && !isSeparator(2) ? 2 : 1;
}

// _Py_normpath_and_size(). All that matters in a name is a byte each, so it is done to the bytes.
Vector<char> normalizePath(std::span<const char> path)
{
    Vector<char> out;
    if (path.empty())
        return out;
    size_t root = rootSizeOf(path);
    out.append(path.first(root));
    Vector<std::span<const char>> segments;
    size_t i = root;
    while (i < path.size()) {
        size_t end = i;
        while (end < path.size() && path[end] != '/')
            ++end;
        auto segment = path.subspan(i, end - i);
        i = end + 1;
        if (segment.empty() || (segment.size() == 1 && segment[0] == '.'))
            continue;
        bool isParent = segment.size() == 2 && segment[0] == '.' && segment[1] == '.';
        auto lastIsParent = [&] { return segments.last().size() == 2 && segments.last()[0] == '.' && segments.last()[1] == '.'; };
        if (isParent && !segments.isEmpty() && !lastIsParent()) {
            segments.removeLast();
            continue;
        }
        // There is nothing above the root.
        if (isParent && segments.isEmpty() && root)
            continue;
        segments.constructAndAppend(segment);
    }
    for (size_t n = 0; n < segments.size(); ++n) {
        if (n)
            out.append('/');
        out.append(segments[n]);
    }
    return out;
}

PYTHON_NATIVE(posixPathNormpath)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "_path_normpath"_s, "path"_s, PathArgument::NonStrict);
    Vector<char> normalized = normalizePath(path.bytes.span());
    if (normalized.isEmpty())
        normalized.append('.');
    RELEASE_AND_RETURN(scope, JSValue::encode(nameLike(globalObject, path, normalized.span())));
}

PYTHON_NATIVE(posixPathSplitrootEx)
{
    NATIVE_PROLOGUE();
    CONVERT_PATH(path, args.at(0), "_path_splitroot_ex"_s, "path"_s, PathArgument::NonStrict);
    auto characters = path.bytes.span();
    size_t root = rootSizeOf(characters);
    JSValue drive = nameLike(globalObject, path, { });
    RETURN_IF_EXCEPTION(scope, { });
    JSValue rootPart = nameLike(globalObject, path, characters.first(root));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue tail = nameLike(globalObject, path, characters.subspan(root));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { drive, rootPart, tail })));
}

void addPosixPathFunctions(JSGlobalObject* globalObject, JSObject* module)
{
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    add("stat"_s, posixStat);
    add("lstat"_s, posixLstat);
    add("access"_s, posixAccess);
    add("chdir"_s, posixChdir);
    add("chmod"_s, posixChmod);
    add("chroot"_s, posixChroot);
    add("chown"_s, posixChown);
    add("lchown"_s, posixLchown);
    add("getcwd"_s, posixGetcwd, pack(false));
    add("getcwdb"_s, posixGetcwd, pack(true));
    add("link"_s, posixLink);
    add("listdir"_s, posixListdir);
    add("mkdir"_s, posixMkdir);
    add("rename"_s, posixRename, pack(false));
    add("replace"_s, posixRename, pack(true));
    add("rmdir"_s, posixRmdir);
    add("unlink"_s, posixUnlink, pack(false));
    add("remove"_s, posixUnlink, pack(true));
    add("symlink"_s, posixSymlink);
    add("readlink"_s, posixReadlink);
    add("truncate"_s, posixTruncate);
    add("utime"_s, posixUtime);
    add("mkfifo"_s, posixMkfifo);
    add("mknod"_s, posixMknod);
    add("statvfs"_s, posixStatvfs);
    add("fstatvfs"_s, posixFstatvfs);
    add("fspath"_s, posixFspath);
    add("_path_normpath"_s, posixPathNormpath);
    add("_path_splitroot_ex"_s, posixPathSplitrootEx);
#if OS(DARWIN)
    add("lchmod"_s, posixLchmod);
    add("chflags"_s, posixChflags, pack(false));
    add("lchflags"_s, posixChflags, pack(true));
#endif
}

} } // namespace JSC::Python

#endif // OS(UNIX)
