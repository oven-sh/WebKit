/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <wtf/text/StringBuilder.h>

#if OS(LINUX)
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#endif

#if OS(DARWIN)
#include <sys/mount.h>
#include <sys/param.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The functions of the module posix that take the name of a file.

// ---- stat ----

static constexpr ASCIILiteral posixStatResultFields[] = {
    "st_mode"_s, "st_ino"_s, "st_dev"_s, "st_nlink"_s, "st_uid"_s, "st_gid"_s, "st_size"_s,
    // The times in whole seconds, which only an index reaches: the names go to the floats.
    ASCIILiteral(), ASCIILiteral(), ASCIILiteral(),
    "st_atime"_s, "st_mtime"_s, "st_ctime"_s,
    "st_atime_ns"_s, "st_mtime_ns"_s, "st_ctime_ns"_s,
    "st_blksize"_s, "st_blocks"_s, "st_rdev"_s,
#if OS(DARWIN)
    "st_flags"_s, "st_gen"_s, "st_birthtime"_s,
#endif
};
static constexpr unsigned posixStatResultFieldCount = std::size(posixStatResultFields);
static constexpr StructSeqDef posixStatResultDef { "os.stat_result"_s, posixStatResultFields, 10 };

// stat_result(tuple of 10): the floats are taken from the whole seconds.
static JSValue posixStatResultNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = structSeqNew(globalObject, cls, args);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* tuple = PyTuple::from(result);
    JSValue values[posixStatResultFieldCount];
    for (unsigned i = 0; i < posixStatResultFieldCount; ++i)
        values[i] = structSeqField(globalObject, tuple, i);
    for (unsigned i = 7; i <= 9; ++i) {
        if (isNone(values[i + 3]))
            values[i + 3] = values[i];
    }
    return makeStructSeq(globalObject, PyType::from(cls), std::span<const JSValue> { values, posixStatResultFieldCount });
}

static constexpr NativeDef posixStatResultConstructor[] = {
    PY_NEW(posixStatResultNew, "($type, /, sequence, dict=<unrepresentable>, *args)"),
};

PyType* posixStatResultType(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    if (JSValue type = posixState(globalObject)->get(internString(vm, posixStatResultDef.name)))
        return PyType::from(type);
    PyType* type = cachedStructSeqType(globalObject, posixStatResultDef);
    type->addMethods(globalObject, posixStatResultConstructor);
    type->didChange(vm);
    return type;
}

static JSValue posixNanoseconds(JSGlobalObject* globalObject, int64_t seconds, long nanoseconds)
{
    int64_t total;
    if (!__builtin_mul_overflow(seconds, static_cast<int64_t>(1000000000), &total) && !__builtin_add_overflow(total, static_cast<int64_t>(nanoseconds), &total))
        return makeInt(globalObject, total);
    JSValue product = intMultiply(globalObject, makeInt(globalObject, seconds), jsNumber(1000000000));
    return intAdd(globalObject, product, posixMakeInt(globalObject, nanoseconds));
}

JSValue posixStatResult(JSGlobalObject* globalObject, const struct stat& status)
{
#if OS(DARWIN)
    const struct timespec& accessed = status.st_atimespec;
    const struct timespec& modified = status.st_mtimespec;
    const struct timespec& changed = status.st_ctimespec;
#else
    const struct timespec& accessed = status.st_atim;
    const struct timespec& modified = status.st_mtim;
    const struct timespec& changed = status.st_ctim;
#endif
    auto inSeconds = [&](const struct timespec& time) { return makeFloat(globalObject, static_cast<double>(time.tv_sec) + 1e-9 * static_cast<double>(time.tv_nsec)); };
    JSValue values[posixStatResultFieldCount] = {
        posixMakeInt(globalObject, static_cast<long>(status.st_mode)),
        makeInt(globalObject, static_cast<uint64_t>(status.st_ino)),
        posixFromDev(globalObject, status.st_dev),
        posixMakeInt(globalObject, static_cast<long>(status.st_nlink)),
        posixFromUid(globalObject, status.st_uid),
        posixFromGid(globalObject, status.st_gid),
        makeInt(globalObject, static_cast<int64_t>(status.st_size)),
        makeInt(globalObject, static_cast<int64_t>(accessed.tv_sec)),
        makeInt(globalObject, static_cast<int64_t>(modified.tv_sec)),
        makeInt(globalObject, static_cast<int64_t>(changed.tv_sec)),
        inSeconds(accessed),
        inSeconds(modified),
        inSeconds(changed),
        posixNanoseconds(globalObject, accessed.tv_sec, accessed.tv_nsec),
        posixNanoseconds(globalObject, modified.tv_sec, modified.tv_nsec),
        posixNanoseconds(globalObject, changed.tv_sec, changed.tv_nsec),
        posixMakeInt(globalObject, static_cast<long>(status.st_blksize)),
        posixMakeInt(globalObject, static_cast<long>(status.st_blocks)),
        posixFromDev(globalObject, status.st_rdev),
#if OS(DARWIN)
        posixMakeInt(globalObject, static_cast<long>(status.st_flags)),
        posixMakeInt(globalObject, static_cast<long>(status.st_gen)),
        inSeconds(status.st_birthtimespec),
#endif
    };
    return makeStructSeq(globalObject, posixStatResultType(globalObject), std::span<const JSValue> { values, posixStatResultFieldCount });
}

static JSValue posixDoStat(JSGlobalObject* globalObject, const PosixPath& path, int dirFd, bool followSymlinks)
{
    if (posixPathAndDirFdInvalid(globalObject, "stat"_s, path, dirFd)
        || posixDirFdAndFdInvalid(globalObject, "stat"_s, dirFd, path.fd())
        || posixFdAndFollowSymlinksInvalid(globalObject, "stat"_s, path.isFd(), followSymlinks))
        return { };
    struct stat status;
    int result;
    if (path.isVirtual())
        result = virtualStat(path.virtualPath(), status);
    else if (path.isFd())
        result = fstat(path.fd(), &status);
    else if (!followSymlinks && dirFd == AT_FDCWD)
        result = lstat(path.narrow(), &status);
    else if (dirFd != AT_FDCWD || !followSymlinks)
        result = fstatat(dirFd, path.narrow(), &status, followSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = stat(path.narrow(), &status);
    if (result)
        return path.error(globalObject);
    return posixStatResult(globalObject, status);
}

static JSValue posixStat(JSGlobalObject* globalObject, JSValue pathValue, JSValue dirFdValue, JSValue followSymlinksValue)
{
    PosixPath path("stat"_s, PosixPath::AllowFd | PosixPath::AllowVirtual);
    int dirFd;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToDirFd(globalObject, dirFdValue, dirFd) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    return posixDoStat(globalObject, path, dirFd, followSymlinks);
}

static JSValue posixLstat(JSGlobalObject* globalObject, JSValue pathValue, JSValue dirFdValue)
{
    PosixPath path("lstat"_s, PosixPath::AllowVirtual);
    int dirFd;
    if (!path.convert(globalObject, pathValue) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    return posixDoStat(globalObject, path, dirFd, false);
}

static JSValue posixAccess(JSGlobalObject* globalObject, JSValue pathValue, JSValue modeValue, JSValue dirFdValue, JSValue effectiveIdsValue, JSValue followSymlinksValue)
{
    PosixPath path("access"_s, PosixPath::AllowVirtual);
    int mode;
    int dirFd;
    bool effectiveIds;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToInt(globalObject, modeValue, mode) || !posixToDirFd(globalObject, dirFdValue, dirFd)
        || !posixToBool(globalObject, effectiveIdsValue, false, effectiveIds) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    int result;
    if (path.isVirtual())
        result = virtualAccess(path.virtualPath(), mode);
    else if (dirFd != AT_FDCWD || effectiveIds || !followSymlinks) {
        int flags = (followSymlinks ? 0 : AT_SYMLINK_NOFOLLOW) | (effectiveIds ? AT_EACCESS : 0);
        result = faccessat(dirFd, path.narrow(), mode, flags);
    } else
        result = access(path.narrow(), mode);
    return jsBoolean(!result);
}

// ---- The current directory ----

static JSValue posixChdir(JSGlobalObject* globalObject, JSValue pathValue)
{
    PosixPath path("chdir"_s, PosixPath::AllowFd | PosixPath::VirtualUnsupported);
    if (!path.convert(globalObject, pathValue))
        return { };
    if (!audit(globalObject, "os.chdir"_s, path.object()))
        return { };
    if (path.isFd() ? fchdir(path.fd()) : chdir(path.narrow()))
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixGetcwdAs(JSGlobalObject* globalObject, bool asBytes)
{
    Vector<char, 1024> buffer;
    char* directory;
    do {
        buffer.grow(buffer.size() + 1024);
        directory = getcwd(buffer.mutableSpan().data(), buffer.size());
    } while (!directory && errno == ERANGE);
    if (!directory)
        return posixError(globalObject);
    JSValue result = posixFileName(globalObject, std::span<const char> { directory, strlen(directory) }, asBytes);
#if OS(LINUX)
    // "(unreachable)/...", of a directory that is no longer under the root of the process.
    if (directory[0] != '/') {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseOSError(globalObject, scope, ENOENT, result);
    }
#endif
    return result;
}

static JSValue posixGetcwd(JSGlobalObject* globalObject) { return posixGetcwdAs(globalObject, false); }
static JSValue posixGetcwdb(JSGlobalObject* globalObject) { return posixGetcwdAs(globalObject, true); }

static JSValue posixChroot(JSGlobalObject* globalObject, JSValue pathValue)
{
    PosixPath path("chroot"_s, PosixPath::VirtualUnsupported);
    if (!path.convert(globalObject, pathValue))
        return { };
    if (chroot(path.narrow()) < 0)
        return path.error(globalObject);
    return jsUndefined();
}

// ---- Permissions and owners ----

static JSValue posixArgumentUnavailable(JSGlobalObject* globalObject, ASCIILiteral function, ASCIILiteral argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseNotImplementedError(globalObject, scope, function, ": "_s, argument, " unavailable on this platform"_s);
}

static JSValue posixChmod(JSGlobalObject* globalObject, JSValue pathValue, JSValue modeValue, JSValue dirFdValue, JSValue followSymlinksValue)
{
    PosixPath path("chmod"_s, PosixPath::AllowFd);
    int mode;
    int dirFd;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToInt(globalObject, modeValue, mode) || !posixToDirFd(globalObject, dirFdValue, dirFd)
        || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (!audit(globalObject, "os.chmod"_s, path.object(), jsNumber(mode), jsNumber(dirFd == AT_FDCWD ? -1 : dirFd)))
        return { };
    int result;
    bool noFollowIsUnsupported = false;
    if (path.isFd())
        result = fchmod(path.fd(), static_cast<mode_t>(mode));
#if OS(DARWIN)
    else if (!followSymlinks && dirFd == AT_FDCWD)
        result = lchmod(path.narrow(), static_cast<mode_t>(mode));
#endif
    else if (dirFd != AT_FDCWD || !followSymlinks) {
        result = fchmodat(dirFd, path.narrow(), static_cast<mode_t>(mode), followSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
        noFollowIsUnsupported = result && (errno == ENOTSUP || errno == EOPNOTSUPP) && !followSymlinks;
    } else
        result = chmod(path.narrow(), static_cast<mode_t>(mode));
    if (!result)
        return jsUndefined();
    if (noFollowIsUnsupported) {
        if (dirFd != AT_FDCWD) {
            posixDirFdAndFollowSymlinksInvalid(globalObject, "chmod"_s, dirFd, followSymlinks);
            return { };
        }
        return posixArgumentUnavailable(globalObject, "chmod"_s, "follow_symlinks"_s);
    }
    return path.error(globalObject);
}

#if OS(DARWIN)
static JSValue posixLchmod(JSGlobalObject* globalObject, JSValue pathValue, JSValue modeValue)
{
    PosixPath path("lchmod"_s);
    int mode;
    if (!path.convert(globalObject, pathValue) || !posixToInt(globalObject, modeValue, mode))
        return { };
    if (!audit(globalObject, "os.chmod"_s, path.object(), jsNumber(mode), jsNumber(-1)))
        return { };
    if (lchmod(path.narrow(), static_cast<mode_t>(mode)) < 0)
        return path.error(globalObject);
    return jsUndefined();
}

static bool posixToFileFlags(JSGlobalObject* globalObject, JSValue value, unsigned& flags)
{
    return posixToUnsignedMask(globalObject, value, flags);
}

static JSValue posixChflags(JSGlobalObject* globalObject, JSValue pathValue, JSValue flagsValue, JSValue followSymlinksValue)
{
    PosixPath path("chflags"_s);
    unsigned flags;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToFileFlags(globalObject, flagsValue, flags) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (followSymlinks ? chflags(path.narrow(), flags) : lchflags(path.narrow(), flags))
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixLchflags(JSGlobalObject* globalObject, JSValue pathValue, JSValue flagsValue)
{
    PosixPath path("lchflags"_s);
    unsigned flags;
    if (!path.convert(globalObject, pathValue) || !posixToFileFlags(globalObject, flagsValue, flags))
        return { };
    if (lchflags(path.narrow(), flags) < 0)
        return path.error(globalObject);
    return jsUndefined();
}
#endif

static JSValue posixChown(JSGlobalObject* globalObject, JSValue pathValue, JSValue uidValue, JSValue gidValue, JSValue dirFdValue, JSValue followSymlinksValue)
{
    PosixPath path("chown"_s, PosixPath::AllowFd);
    uid_t uid;
    gid_t gid;
    int dirFd;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToUid(globalObject, uidValue, uid) || !posixToGid(globalObject, gidValue, gid)
        || !posixToDirFd(globalObject, dirFdValue, dirFd) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (!audit(globalObject, "os.chown"_s, path.object(), jsNumber(static_cast<uint32_t>(uid)), jsNumber(static_cast<uint32_t>(gid)), jsNumber(dirFd == AT_FDCWD ? -1 : dirFd)))
        return { };
    if (posixDirFdAndFdInvalid(globalObject, "chown"_s, dirFd, path.fd()) || posixFdAndFollowSymlinksInvalid(globalObject, "chown"_s, path.isFd(), followSymlinks))
        return { };
    int result;
    if (path.isFd())
        result = fchown(path.fd(), uid, gid);
    else if (!followSymlinks && dirFd == AT_FDCWD)
        result = lchown(path.narrow(), uid, gid);
    else if (dirFd != AT_FDCWD || !followSymlinks)
        result = fchownat(dirFd, path.narrow(), uid, gid, followSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = chown(path.narrow(), uid, gid);
    if (result)
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixLchown(JSGlobalObject* globalObject, JSValue pathValue, JSValue uidValue, JSValue gidValue)
{
    PosixPath path("lchown"_s);
    uid_t uid;
    gid_t gid;
    if (!path.convert(globalObject, pathValue) || !posixToUid(globalObject, uidValue, uid) || !posixToGid(globalObject, gidValue, gid))
        return { };
    if (!audit(globalObject, "os.chown"_s, path.object(), jsNumber(static_cast<uint32_t>(uid)), jsNumber(static_cast<uint32_t>(gid)), jsNumber(-1)))
        return { };
    if (lchown(path.narrow(), uid, gid) < 0)
        return path.error(globalObject);
    return jsUndefined();
}

// ---- Names: making, removing, moving ----

static JSValue posixLink(JSGlobalObject* globalObject, JSValue sourceValue, JSValue destinationValue, JSValue sourceDirFdValue, JSValue destinationDirFdValue, JSValue followSymlinksValue)
{
    PosixPath source("link"_s, "src"_s);
    PosixPath destination("link"_s, "dst"_s, PosixPath::MakesNew);
    int sourceDirFd;
    int destinationDirFd;
    bool followSymlinks;
    if (!source.convert(globalObject, sourceValue) || !destination.convert(globalObject, destinationValue)
        || !posixToDirFd(globalObject, sourceDirFdValue, sourceDirFd) || !posixToDirFd(globalObject, destinationDirFdValue, destinationDirFd)
        || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (!audit(globalObject, "os.link"_s, source.object(), destination.object(), jsNumber(sourceDirFd == AT_FDCWD ? -1 : sourceDirFd), jsNumber(destinationDirFd == AT_FDCWD ? -1 : destinationDirFd)))
        return { };
    if (linkat(sourceDirFd, source.narrow(), destinationDirFd, destination.narrow(), followSymlinks ? AT_SYMLINK_FOLLOW : 0))
        return source.error(globalObject, destination);
    return jsUndefined();
}

static JSValue posixSymlink(JSGlobalObject* globalObject, JSValue sourceValue, JSValue destinationValue, JSValue targetIsDirectoryValue, JSValue dirFdValue)
{
    PosixPath source("symlink"_s, "src"_s, PosixPath::AllowVirtual);
    PosixPath destination("symlink"_s, "dst"_s, PosixPath::MakesNew);
    bool targetIsDirectory;
    int dirFd;
    if (!source.convert(globalObject, sourceValue) || !destination.convert(globalObject, destinationValue)
        || !posixToBool(globalObject, targetIsDirectoryValue, false, targetIsDirectory) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    if (!audit(globalObject, "os.symlink"_s, source.object(), destination.object(), jsNumber(dirFd == AT_FDCWD ? -1 : dirFd)))
        return { };
    if (dirFd != AT_FDCWD ? symlinkat(source.narrow(), dirFd, destination.narrow()) : symlink(source.narrow(), destination.narrow()))
        return source.error(globalObject, destination);
    return jsUndefined();
}

static JSValue posixReadlink(JSGlobalObject* globalObject, JSValue pathValue, JSValue dirFdValue)
{
    PosixPath path("readlink"_s, PosixPath::AllowVirtual);
    int dirFd;
    if (!path.convert(globalObject, pathValue) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    if (path.isVirtual()) {
        // Nothing there is a link.
        struct stat status;
        if (!virtualStat(path.virtualPath(), status))
            errno = EINVAL;
        return path.error(globalObject);
    }
    char buffer[PATH_MAX + 1];
    ssize_t length = dirFd != AT_FDCWD ? readlinkat(dirFd, path.narrow(), buffer, PATH_MAX) : readlink(path.narrow(), buffer, PATH_MAX);
    if (length < 0)
        return path.error(globalObject);
    return posixFileName(globalObject, std::span<const char> { buffer, static_cast<size_t>(length) }, path.isBytes());
}

static JSValue posixMkdir(JSGlobalObject* globalObject, JSValue pathValue, JSValue modeValue, JSValue dirFdValue)
{
    PosixPath path("mkdir"_s, PosixPath::MakesNew);
    int mode = 0777;
    int dirFd;
    if (!path.convert(globalObject, pathValue) || (modeValue && !posixToInt(globalObject, modeValue, mode)) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    if (!audit(globalObject, "os.mkdir"_s, path.object(), jsNumber(mode), jsNumber(dirFd == AT_FDCWD ? -1 : dirFd)))
        return { };
    if ((dirFd != AT_FDCWD ? mkdirat(dirFd, path.narrow(), static_cast<mode_t>(mode)) : mkdir(path.narrow(), static_cast<mode_t>(mode))) < 0)
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixRmdir(JSGlobalObject* globalObject, JSValue pathValue, JSValue dirFdValue)
{
    PosixPath path("rmdir"_s);
    int dirFd;
    if (!path.convert(globalObject, pathValue) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    if (!audit(globalObject, "os.rmdir"_s, path.object(), jsNumber(dirFd == AT_FDCWD ? -1 : dirFd)))
        return { };
    if (dirFd != AT_FDCWD ? unlinkat(dirFd, path.narrow(), AT_REMOVEDIR) : rmdir(path.narrow()))
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixUnlinkNamed(JSGlobalObject* globalObject, ASCIILiteral function, JSValue pathValue, JSValue dirFdValue)
{
    PosixPath path(function);
    int dirFd;
    if (!path.convert(globalObject, pathValue) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    if (!audit(globalObject, "os.remove"_s, path.object(), jsNumber(dirFd == AT_FDCWD ? -1 : dirFd)))
        return { };
    if (dirFd != AT_FDCWD ? unlinkat(dirFd, path.narrow(), 0) : unlink(path.narrow()))
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixUnlink(JSGlobalObject* globalObject, JSValue path, JSValue dirFd) { return posixUnlinkNamed(globalObject, "unlink"_s, path, dirFd); }
static JSValue posixRemove(JSGlobalObject* globalObject, JSValue path, JSValue dirFd) { return posixUnlinkNamed(globalObject, "remove"_s, path, dirFd); }

static JSValue posixRenameNamed(JSGlobalObject* globalObject, ASCIILiteral function, JSValue sourceValue, JSValue destinationValue, JSValue sourceDirFdValue, JSValue destinationDirFdValue)
{
    PosixPath source(function, "src"_s);
    PosixPath destination(function, "dst"_s, PosixPath::MakesOrReplaces);
    int sourceDirFd;
    int destinationDirFd;
    if (!source.convert(globalObject, sourceValue) || !destination.convert(globalObject, destinationValue)
        || !posixToDirFd(globalObject, sourceDirFdValue, sourceDirFd) || !posixToDirFd(globalObject, destinationDirFdValue, destinationDirFd))
        return { };
    if (!audit(globalObject, "os.rename"_s, source.object(), destination.object(), jsNumber(sourceDirFd == AT_FDCWD ? -1 : sourceDirFd), jsNumber(destinationDirFd == AT_FDCWD ? -1 : destinationDirFd)))
        return { };
    int result;
    if (sourceDirFd != AT_FDCWD || destinationDirFd != AT_FDCWD)
        result = renameat(sourceDirFd, source.narrow(), destinationDirFd, destination.narrow());
    else
        result = rename(source.narrow(), destination.narrow());
    if (result)
        return source.error(globalObject, destination);
    return jsUndefined();
}

static JSValue posixRename(JSGlobalObject* globalObject, JSValue source, JSValue destination, JSValue sourceDirFd, JSValue destinationDirFd)
{
    return posixRenameNamed(globalObject, "rename"_s, source, destination, sourceDirFd, destinationDirFd);
}

static JSValue posixReplace(JSGlobalObject* globalObject, JSValue source, JSValue destination, JSValue sourceDirFd, JSValue destinationDirFd)
{
    return posixRenameNamed(globalObject, "replace"_s, source, destination, sourceDirFd, destinationDirFd);
}

static JSValue posixMkfifo(JSGlobalObject* globalObject, JSValue pathValue, JSValue modeValue, JSValue dirFdValue)
{
    PosixPath path("mkfifo"_s, PosixPath::MakesNew);
    int mode = 0666;
    int dirFd;
    if (!path.convert(globalObject, pathValue) || (modeValue && !posixToInt(globalObject, modeValue, mode)) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    int result;
    do {
        result = dirFd != AT_FDCWD ? mkfifoat(dirFd, path.narrow(), static_cast<mode_t>(mode)) : mkfifo(path.narrow(), static_cast<mode_t>(mode));
    } while (result && errno == EINTR && checkSignals(globalObject));
    if (result)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue posixMknod(JSGlobalObject* globalObject, JSValue pathValue, JSValue modeValue, JSValue deviceValue, JSValue dirFdValue)
{
    PosixPath path("mknod"_s, PosixPath::MakesNew);
    int mode = 0600;
    dev_t device = 0;
    int dirFd;
    if (!path.convert(globalObject, pathValue) || (modeValue && !posixToInt(globalObject, modeValue, mode))
        || (deviceValue && !posixToDev(globalObject, deviceValue, device)) || !posixToDirFd(globalObject, dirFdValue, dirFd))
        return { };
    int result;
    do {
        result = dirFd != AT_FDCWD ? mknodat(dirFd, path.narrow(), static_cast<mode_t>(mode), device) : mknod(path.narrow(), static_cast<mode_t>(mode), device);
    } while (result && errno == EINTR && checkSignals(globalObject));
    if (result)
        return posixError(globalObject);
    return jsUndefined();
}

// ---- Device numbers ----

static unsigned posixMajorOf(dev_t device) { return major(device); }
static unsigned posixMinorOf(dev_t device) { return minor(device); }
static dev_t posixMakeDevice(unsigned majorNumber, unsigned minorNumber) { return makedev(majorNumber, minorNumber); }
#if OS(LINUX)
// Not names to leave defined for the files compiled together with this one.
#undef major
#undef minor
#undef makedev
#endif

static JSValue posixDevicePart(unsigned value)
{
    if (value == static_cast<unsigned>(posixNoDevice))
        return jsNumber(-1);
    return jsNumber(value);
}

static JSValue posixMajor(JSGlobalObject* globalObject, JSValue deviceValue)
{
    dev_t device;
    if (!posixToDev(globalObject, deviceValue, device))
        return { };
    return posixDevicePart(posixMajorOf(device));
}

static JSValue posixMinor(JSGlobalObject* globalObject, JSValue deviceValue)
{
    dev_t device;
    if (!posixToDev(globalObject, deviceValue, device))
        return { };
    return posixDevicePart(posixMinorOf(device));
}

static JSValue posixMakedev(JSGlobalObject* globalObject, JSValue majorValue, JSValue minorValue)
{
    dev_t majorNumber;
    dev_t minorNumber;
    if (!posixToDev(globalObject, majorValue, majorNumber) || !posixToDev(globalObject, minorValue, minorNumber))
        return { };
    auto fits = [](dev_t value) { return value == posixNoDevice || static_cast<dev_t>(static_cast<unsigned>(value)) == value; };
    if (!fits(majorNumber) || !fits(minorNumber)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C unsigned int"_s);
    }
    return posixFromDev(globalObject, posixMakeDevice(static_cast<unsigned>(majorNumber), static_cast<unsigned>(minorNumber)));
}

// ---- statx ----

#if OS(LINUX)
// As the kernel lays it out. The C library's declaration is as old as the headers it is compiled with, and its function
// is newer than the oldest library this may run with.
struct PosixStatxTimestamp {
    int64_t seconds;
    uint32_t nanoseconds;
    int32_t reserved;
};

struct PosixStatx {
    uint32_t mask;
    uint32_t blksize;
    uint64_t attributes;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t spare0;
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributesMask;
    PosixStatxTimestamp atime;
    PosixStatxTimestamp btime;
    PosixStatxTimestamp ctime;
    PosixStatxTimestamp mtime;
    uint32_t rdevMajor;
    uint32_t rdevMinor;
    uint32_t devMajor;
    uint32_t devMinor;
    uint64_t mntId;
    uint32_t dioMemAlign;
    uint32_t dioOffsetAlign;
    uint64_t subvol;
    uint32_t atomicWriteUnitMin;
    uint32_t atomicWriteUnitMax;
    uint32_t atomicWriteSegmentsMax;
    uint32_t dioReadOffsetAlign;
    uint32_t atomicWriteUnitMaxOpt;
    uint32_t spare2;
    uint64_t spare3[8];
};
static_assert(sizeof(PosixStatx) == 256);

enum PosixStatxMask : uint32_t {
    PosixStatxType = 0x1,
    PosixStatxMode = 0x2,
    PosixStatxNlink = 0x4,
    PosixStatxUid = 0x8,
    PosixStatxGid = 0x10,
    PosixStatxAtime = 0x20,
    PosixStatxMtime = 0x40,
    PosixStatxCtime = 0x80,
    PosixStatxIno = 0x100,
    PosixStatxSize = 0x200,
    PosixStatxBlocks = 0x400,
    PosixStatxBtime = 0x800,
    PosixStatxMntId = 0x1000,
    PosixStatxDioAlign = 0x2000,
    PosixStatxMntIdUnique = 0x4000,
    PosixStatxSubvol = 0x8000,
    PosixStatxWriteAtomic = 0x10000,
    PosixStatxDioReadAlign = 0x20000,
    // A bit beyond these may stand for a member beyond the end of the structure.
    PosixStatxKnown = 0x3ffff,
};

// The attributes of os.statx_result, in the order its repr() shows them: those that are always there, then those that are
// None unless the mask says the system filled them in. An instance is a record with a field for each.
#define POSIX_FOR_EACH_STATX_MEMBER(v) \
    v(stx_mask) v(stx_blksize) v(stx_attributes) v(stx_attributes_mask) v(stx_rdev_major) v(stx_rdev_minor) v(stx_rdev) \
    v(stx_dev_major) v(stx_dev_minor) v(stx_dev)
#define POSIX_FOR_EACH_STATX_OPTIONAL(v) \
    v(stx_mode) v(stx_nlink) v(stx_uid) v(stx_gid) v(stx_ino) v(stx_size) v(stx_blocks) v(stx_atime) v(stx_atime_ns) \
    v(stx_btime) v(stx_btime_ns) v(stx_ctime) v(stx_ctime_ns) v(stx_mtime) v(stx_mtime_ns) v(stx_mnt_id) \
    v(stx_dio_mem_align) v(stx_dio_offset_align) v(stx_subvol) v(stx_atomic_write_unit_min) v(stx_atomic_write_unit_max) \
    v(stx_atomic_write_segments_max) v(stx_dio_read_offset_align) v(stx_atomic_write_unit_max_opt)

enum PosixStatxField : unsigned {
#define POSIX_DECLARE_FIELD(name) PosixStatxField_##name,
    POSIX_FOR_EACH_STATX_MEMBER(POSIX_DECLARE_FIELD)
    POSIX_FOR_EACH_STATX_OPTIONAL(POSIX_DECLARE_FIELD)
#undef POSIX_DECLARE_FIELD
    PosixStatxFieldCount,
};

template<unsigned index>
static JSValue posixStatxGet(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(index); }

static constexpr NativeGetSet posixStatxGetSets[] = {
#define POSIX_DEFINE_MEMBER(name) PY_MEMBER(#name, posixStatxGet<PosixStatxField_##name>),
    POSIX_FOR_EACH_STATX_MEMBER(POSIX_DEFINE_MEMBER)
#undef POSIX_DEFINE_MEMBER
#define POSIX_DEFINE_OPTIONAL(name) { #name ""_s, posixStatxGet<PosixStatxField_##name> },
    POSIX_FOR_EACH_STATX_OPTIONAL(POSIX_DEFINE_OPTIONAL)
#undef POSIX_DEFINE_OPTIONAL
};

static JSValue posixStatxRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = PyRecord::from(self);
    StrBuilder builder;
    builder.append("os.statx_result("_s);
    for (unsigned i = 0; i < PosixStatxFieldCount; ++i) {
        JSValue value = record->get(i);
        if (isNone(value))
            continue;
        if (i)
            builder.append(", "_s);
        JSString* text = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(posixStatxGetSets[i].name, '=', stringOf(text));
    }
    builder.append(')');
    return makeStr(globalObject, builder);
}

static constexpr NativeDef posixStatxMethods[] = {
    PY_SLOT("__repr__", posixStatxRepr, "($self, /)"),
};

static constexpr TypeDef posixStatxResultTypeDef { "os.statx_result"_s, { }, posixStatxMethods, posixStatxGetSets, { } };

static JSValue posixStatxFunction(JSGlobalObject* globalObject, JSValue pathValue, JSValue maskValue, JSValue flagsValue, JSValue dirFdValue, JSValue followSymlinksValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixPath path("statx"_s, PosixPath::AllowFd | PosixPath::AllowVirtual);
    unsigned mask;
    int flags = 0;
    int dirFd;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToUnsignedMask(globalObject, maskValue, mask) || (flagsValue && !posixToInt(globalObject, flagsValue, flags))
        || !posixToDirFd(globalObject, dirFdValue, dirFd) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (posixPathAndDirFdInvalid(globalObject, "statx"_s, path, dirFd)
        || posixDirFdAndFdInvalid(globalObject, "statx"_s, dirFd, path.fd())
        || posixFdAndFollowSymlinksInvalid(globalObject, "statx"_s, path.isFd(), followSymlinks))
        return { };
    if (flags & (AT_SYMLINK_NOFOLLOW | AT_SYMLINK_FOLLOW))
        return raiseValueError(globalObject, scope, "use follow_symlinks kwarg instead of AT_SYMLINK_* flag"_s);
    if (flags & AT_EMPTY_PATH)
        return raiseValueError(globalObject, scope, "use dir_fd kwarg instead of AT_EMPTY_PATH flag"_s);
    mask &= PosixStatxKnown;
    if (!followSymlinks)
        flags |= AT_SYMLINK_NOFOLLOW;

    PosixStatx status { };
    long result;
    if (path.isVirtual()) {
        struct stat basic;
        result = virtualStat(path.virtualPath(), basic);
        if (!result) {
            status.mask = PosixStatxBtime - 1; // STATX_BASIC_STATS
            status.blksize = basic.st_blksize;
            status.nlink = basic.st_nlink;
            status.mode = basic.st_mode;
            status.ino = basic.st_ino;
            status.size = basic.st_size;
            status.blocks = basic.st_blocks;
            status.atime.seconds = status.ctime.seconds = status.mtime.seconds = basic.st_mtime;
            status.devMajor = posixMajorOf(basic.st_dev);
            status.devMinor = posixMinorOf(basic.st_dev);
        }
    } else if (path.isFd())
        result = syscall(SYS_statx, path.fd(), "", flags | AT_EMPTY_PATH, mask, &status);
    else
        result = syscall(SYS_statx, dirFd, path.narrow(), flags, mask, &status);
    if (result)
        return path.error(globalObject);

    PyRecord* record = PyRecord::create(globalObject, rt.typeFor(posixStatxResultTypeDef), PosixStatxFieldCount);
    auto set = [&](PosixStatxField field, JSValue value) { record->set(vm, field, value); };
    auto setIf = [&](uint32_t bits, PosixStatxField field, uint64_t value) { record->set(vm, field, status.mask & bits ? makeInt(globalObject, value) : jsUndefined()); };
    auto setTime = [&](uint32_t bit, PosixStatxField inSeconds, PosixStatxField inNanoseconds, const PosixStatxTimestamp& time) {
        bool isThere = status.mask & bit;
        record->set(vm, inSeconds, isThere ? makeFloat(globalObject, static_cast<double>(time.seconds) + time.nanoseconds * 1e-9) : jsUndefined());
        record->set(vm, inNanoseconds, isThere ? posixNanoseconds(globalObject, time.seconds, time.nanoseconds) : jsUndefined());
    };
    set(PosixStatxField_stx_mask, jsNumber(status.mask));
    set(PosixStatxField_stx_blksize, jsNumber(status.blksize));
    set(PosixStatxField_stx_attributes, makeInt(globalObject, status.attributes));
    set(PosixStatxField_stx_attributes_mask, makeInt(globalObject, status.attributesMask));
    set(PosixStatxField_stx_rdev_major, jsNumber(status.rdevMajor));
    set(PosixStatxField_stx_rdev_minor, jsNumber(status.rdevMinor));
    set(PosixStatxField_stx_rdev, makeInt(globalObject, static_cast<uint64_t>(posixMakeDevice(status.rdevMajor, status.rdevMinor))));
    set(PosixStatxField_stx_dev_major, jsNumber(status.devMajor));
    set(PosixStatxField_stx_dev_minor, jsNumber(status.devMinor));
    set(PosixStatxField_stx_dev, makeInt(globalObject, static_cast<uint64_t>(posixMakeDevice(status.devMajor, status.devMinor))));
    setIf(PosixStatxType | PosixStatxMode, PosixStatxField_stx_mode, status.mode);
    setIf(PosixStatxNlink, PosixStatxField_stx_nlink, status.nlink);
    setIf(PosixStatxUid, PosixStatxField_stx_uid, status.uid);
    setIf(PosixStatxGid, PosixStatxField_stx_gid, status.gid);
    setIf(PosixStatxIno, PosixStatxField_stx_ino, status.ino);
    setIf(PosixStatxSize, PosixStatxField_stx_size, status.size);
    setIf(PosixStatxBlocks, PosixStatxField_stx_blocks, status.blocks);
    setTime(PosixStatxAtime, PosixStatxField_stx_atime, PosixStatxField_stx_atime_ns, status.atime);
    setTime(PosixStatxBtime, PosixStatxField_stx_btime, PosixStatxField_stx_btime_ns, status.btime);
    setTime(PosixStatxCtime, PosixStatxField_stx_ctime, PosixStatxField_stx_ctime_ns, status.ctime);
    setTime(PosixStatxMtime, PosixStatxField_stx_mtime, PosixStatxField_stx_mtime_ns, status.mtime);
    setIf(PosixStatxMntId, PosixStatxField_stx_mnt_id, status.mntId);
    setIf(PosixStatxDioAlign, PosixStatxField_stx_dio_mem_align, status.dioMemAlign);
    setIf(PosixStatxDioAlign, PosixStatxField_stx_dio_offset_align, status.dioOffsetAlign);
    setIf(PosixStatxSubvol, PosixStatxField_stx_subvol, status.subvol);
    setIf(PosixStatxWriteAtomic, PosixStatxField_stx_atomic_write_unit_min, status.atomicWriteUnitMin);
    setIf(PosixStatxWriteAtomic, PosixStatxField_stx_atomic_write_unit_max, status.atomicWriteUnitMax);
    setIf(PosixStatxWriteAtomic, PosixStatxField_stx_atomic_write_segments_max, status.atomicWriteSegmentsMax);
    setIf(PosixStatxDioReadAlign, PosixStatxField_stx_dio_read_offset_align, status.dioReadOffsetAlign);
    setIf(PosixStatxWriteAtomic, PosixStatxField_stx_atomic_write_unit_max_opt, status.atomicWriteUnitMaxOpt);
    return record;
}
#endif // OS(LINUX)

// ---- Size and times ----

static JSValue posixTruncate(JSGlobalObject* globalObject, JSValue pathValue, JSValue lengthValue)
{
    PosixPath path("truncate"_s, PosixPath::AllowFd);
    off_t length;
    if (!path.convert(globalObject, pathValue) || !posixToOffset(globalObject, lengthValue, length))
        return { };
    if (!audit(globalObject, "os.truncate"_s, path.object(), lengthValue))
        return { };
    if (path.isFd()) {
        int result;
        do {
            result = ftruncate(path.fd(), length);
        } while (result && errno == EINTR && checkSignals(globalObject));
        if (result)
            return posixError(globalObject);
        return jsUndefined();
    }
    if (truncate(path.narrow(), length) < 0)
        return path.error(globalObject);
    return jsUndefined();
}

// A time in nanoseconds, as an int.
static bool posixSplitNanoseconds(JSGlobalObject* globalObject, JSValue value, struct timespec& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue pair = divmod(globalObject, value, jsNumber(1000000000));
    RETURN_IF_EXCEPTION(scope, false);
    auto* tuple = PyTuple::tryFrom(pair);
    if (!tuple || tuple->length() != 2) {
        raiseTypeError(globalObject, scope, typeName(globalObject, value), ".__divmod__() must return a 2-tuple, not "_s, typeName(globalObject, pair));
        return false;
    }
    JSValue seconds = toIndex(globalObject, tuple->at(0));
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(seconds);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "timestamp out of range for platform time_t"_s);
        return false;
    }
    int64_t nanoseconds;
    if (!posixToLong(globalObject, tuple->at(1), nanoseconds))
        return false;
    result.tv_sec = static_cast<time_t>(*converted);
    result.tv_nsec = static_cast<long>(nanoseconds);
    return true;
}

static JSValue posixUtime(JSGlobalObject* globalObject, JSValue pathValue, JSValue times, JSValue ns, JSValue dirFdValue, JSValue followSymlinksValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixPath path("utime"_s, PosixPath::AllowFd);
    int dirFd;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToDirFd(globalObject, dirFdValue, dirFd) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (!audit(globalObject, "os.utime"_s, path.object(), times, ns, jsNumber(dirFd == AT_FDCWD ? -1 : dirFd)))
        return { };

    bool hasTimes = times && !isNone(times);
    if (hasTimes && ns)
        return raiseValueError(globalObject, scope, "utime: you may specify either 'times' or 'ns' but not both"_s);
    Runtime& rt = runtime(globalObject);
    auto exactPair = [&](JSValue value) -> PyTuple* {
        auto* tuple = PyTuple::tryFrom(value);
        return tuple && tuple->type() == rt.tupleType() && tuple->length() == 2 ? tuple : nullptr;
    };
    struct timespec values[2];
    struct timespec* when = values;
    if (hasTimes) {
        PyTuple* pair = exactPair(times);
        if (!pair)
            return raiseTypeError(globalObject, scope, "utime: 'times' must be either a tuple of two numbers or None"_s);
        for (unsigned i = 0; i < 2; ++i) {
            if (!posixToTimespec(globalObject, pair->at(i), values[i].tv_sec, values[i].tv_nsec))
                return { };
        }
    } else if (ns) {
        PyTuple* pair = exactPair(ns);
        if (!pair)
            return raiseTypeError(globalObject, scope, "utime: 'ns' must be a tuple of two ints"_s);
        for (unsigned i = 0; i < 2; ++i) {
            if (!posixSplitNanoseconds(globalObject, pair->at(i), values[i]))
                return { };
        }
    } else
        when = nullptr; // Now.

    if (posixPathAndDirFdInvalid(globalObject, "utime"_s, path, dirFd)
        || posixDirFdAndFdInvalid(globalObject, "utime"_s, dirFd, path.fd())
        || posixFdAndFollowSymlinksInvalid(globalObject, "utime"_s, path.isFd(), followSymlinks))
        return { };

    int result;
    if (path.isFd())
        result = futimens(path.fd(), when);
    else
        result = utimensat(dirFd, path.narrow(), when, followSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    if (result < 0)
        return path.error(globalObject);
    return jsUndefined();
}

// ---- File systems ----

static constexpr ASCIILiteral posixStatvfsResultFields[] = {
    "f_bsize"_s, "f_frsize"_s, "f_blocks"_s, "f_bfree"_s, "f_bavail"_s, "f_files"_s, "f_ffree"_s, "f_favail"_s, "f_flag"_s, "f_namemax"_s, "f_fsid"_s,
};
static constexpr StructSeqDef posixStatvfsResultDef { "os.statvfs_result"_s, posixStatvfsResultFields, 10 };

#if OS(DARWIN)
// statvfs() counts blocks in 32 bits there.
using PosixFileSystemStatus = struct statfs;
static int posixFileSystemStatusOf(const char* path, PosixFileSystemStatus& status) { return statfs(path, &status); }
static int posixFileSystemStatusOf(int fd, PosixFileSystemStatus& status) { return fstatfs(fd, &status); }

static JSValue posixStatvfsResult(JSGlobalObject* globalObject, const PosixFileSystemStatus& status)
{
    long flags = (status.f_flags & MNT_RDONLY ? ST_RDONLY : 0) | (status.f_flags & MNT_NOSUID ? ST_NOSUID : 0);
    return makeStructSeq(globalObject, cachedStructSeqType(globalObject, posixStatvfsResultDef), {
        posixMakeInt(globalObject, static_cast<long>(status.f_iosize)), posixMakeInt(globalObject, static_cast<long>(status.f_bsize)),
        makeInt(globalObject, static_cast<int64_t>(status.f_blocks)), makeInt(globalObject, static_cast<int64_t>(status.f_bfree)),
        makeInt(globalObject, static_cast<int64_t>(status.f_bavail)), makeInt(globalObject, static_cast<int64_t>(status.f_files)),
        makeInt(globalObject, static_cast<int64_t>(status.f_ffree)), makeInt(globalObject, static_cast<int64_t>(status.f_ffree)),
        posixMakeInt(globalObject, flags), posixMakeInt(globalObject, static_cast<long>(NAME_MAX)),
        makeInt(globalObject, static_cast<uint64_t>(static_cast<unsigned long>(status.f_fsid.val[0]))) });
}
#else
using PosixFileSystemStatus = struct statvfs;
static int posixFileSystemStatusOf(const char* path, PosixFileSystemStatus& status) { return statvfs(path, &status); }
static int posixFileSystemStatusOf(int fd, PosixFileSystemStatus& status) { return fstatvfs(fd, &status); }

static JSValue posixStatvfsResult(JSGlobalObject* globalObject, const PosixFileSystemStatus& status)
{
    auto number = [&](auto value) { return makeInt(globalObject, static_cast<int64_t>(value)); };
    return makeStructSeq(globalObject, cachedStructSeqType(globalObject, posixStatvfsResultDef), {
        number(status.f_bsize), number(status.f_frsize), number(status.f_blocks), number(status.f_bfree), number(status.f_bavail),
        number(status.f_files), number(status.f_ffree), number(status.f_favail), number(status.f_flag), number(status.f_namemax),
        makeInt(globalObject, static_cast<uint64_t>(status.f_fsid)) });
}
#endif

static JSValue posixStatvfs(JSGlobalObject* globalObject, JSValue pathValue)
{
    PosixPath path("statvfs"_s, PosixPath::AllowFd | PosixPath::VirtualUnsupported);
    if (!path.convert(globalObject, pathValue))
        return { };
    PosixFileSystemStatus status;
    if (path.isFd() ? posixFileSystemStatusOf(path.fd(), status) : posixFileSystemStatusOf(path.narrow(), status))
        return path.error(globalObject);
    return posixStatvfsResult(globalObject, status);
}

static JSValue posixFstatvfs(JSGlobalObject* globalObject, JSValue fdValue)
{
    int fd;
    if (!posixToInt(globalObject, fdValue, fd))
        return { };
    PosixFileSystemStatus status;
    int result;
    do {
        result = posixFileSystemStatusOf(fd, status);
    } while (result && errno == EINTR && checkSignals(globalObject));
    if (result)
        return posixError(globalObject);
    return posixStatvfsResult(globalObject, status);
}

static JSValue posixSync(JSGlobalObject*)
{
    sync();
    return jsUndefined();
}

// ---- Directories ----

// function(name, inode, type of entry) for each entry but "." and ".."; it returns false having raised an exception.
template<typename Function>
static bool posixForEachDirectoryEntry(JSGlobalObject* globalObject, const PosixPath& path, const Function& function)
{
    if (path.isVirtual()) {
        Vector<VirtualDirectoryEntry> entries;
        if (!virtualList(path.virtualPath(), entries)) {
            path.error(globalObject);
            return false;
        }
        for (auto& entry : entries) {
            if (!function(byteCast<char>(entry.name.span()), entry.inode, static_cast<unsigned char>(entry.kind == VirtualFileKind::Directory ? DT_DIR : DT_REG)))
                return false;
        }
        return true;
    }

    DIR* directory;
    errno = 0;
    if (path.isFd()) {
        // closedir() closes the descriptor it was given.
        int fd = posixDupNotInheritable(path.fd());
        if (fd < 0) {
            posixError(globalObject);
            return false;
        }
        directory = fdopendir(fd);
        if (!directory) {
            path.error(globalObject);
            close(fd);
            return false;
        }
    } else {
        directory = opendir(path.narrow() ? path.narrow() : ".");
        if (!directory) {
            path.error(globalObject);
            return false;
        }
    }
    bool succeeded = true;
    while (true) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (!entry) {
            if (errno) {
                path.error(globalObject);
                succeeded = false;
            }
            break;
        }
        const char* name = entry->d_name;
        size_t length = strlen(name);
        if (name[0] == '.' && (length == 1 || (name[1] == '.' && length == 2)))
            continue;
        if (!function(std::span<const char> { name, length }, static_cast<uint64_t>(entry->d_ino), static_cast<unsigned char>(entry->d_type))) {
            succeeded = false;
            break;
        }
    }
    // The copy shares its position with the caller's descriptor.
    if (path.isFd())
        rewinddir(directory);
    closedir(directory);
    return succeeded;
}

static JSValue posixListdir(JSGlobalObject* globalObject, JSValue pathValue)
{
    VM& vm = getVM(globalObject);
    PosixPath path("listdir"_s, PosixPath::AllowFd | PosixPath::Nullable | PosixPath::AllowVirtual);
    if (!path.convert(globalObject, pathValue))
        return { };
    if (!audit(globalObject, "os.listdir"_s, path.object()))
        return { };
    JSArray* list = newList(globalObject);
    bool asBytes = path.isBytes();
    bool succeeded = posixForEachDirectoryEntry(globalObject, path, [&](std::span<const char> name, uint64_t, unsigned char) {
        listAppend(globalObject, list, posixFileName(globalObject, name, asBytes));
        return !vm.exceptionForInspection();
    });
    if (!succeeded)
        return { };
    return list;
}

// os.DirEntry
enum PosixDirEntryField : unsigned {
    PosixDirEntryName,
    PosixDirEntryPath,
    PosixDirEntryInode,
    PosixDirEntryStat, // Empty until asked for, as the next.
    PosixDirEntryLstat,
    PosixDirEntryFieldCount,
};
// `number` is the descriptor of the directory (or AT_FDCWD), above 8 bits for the type the directory gives for the entry.

static int posixDirEntryDirFd(PyRecord* entry) { return static_cast<int>(entry->number >> 8); }
static unsigned char posixDirEntryType(PyRecord* entry) { return static_cast<unsigned char>(entry->number & 0xff); }

static JSValue posixDirEntryFetchStat(JSGlobalObject* globalObject, PyRecord* entry, bool followSymlinks)
{
    JSValue pathObject = entry->get(PosixDirEntryPath);
    PosixBytes path;
    if (!posixToFSBytes(globalObject, pathObject, path))
        return { };
    struct stat status;
    int result;
    int dirFd = posixDirEntryDirFd(entry);
    VirtualPath virtualPath;
    if (toVirtualPath(path.span(), virtualPath))
        result = virtualStat(virtualPath.span(), status);
    else if (dirFd != AT_FDCWD)
        result = fstatat(dirFd, path.characters(), &status, followSymlinks ? 0 : AT_SYMLINK_NOFOLLOW);
    else
        result = followSymlinks ? stat(path.characters(), &status) : lstat(path.characters(), &status);
    if (result) {
        int errorNumber = errno;
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseOSError(globalObject, scope, errorNumber, pathObject);
    }
    return posixStatResult(globalObject, status);
}

static JSValue posixDirEntryGetLstat(JSGlobalObject* globalObject, PyRecord* entry)
{
    if (JSValue cached = entry->get(PosixDirEntryLstat))
        return cached;
    JSValue result = posixDirEntryFetchStat(globalObject, entry, false);
    if (result)
        entry->set(getVM(globalObject), PosixDirEntryLstat, result);
    return result;
}

enum class PosixTriState : uint8_t { False, True, Exception };

static PosixTriState posixDirEntryTestMode(JSGlobalObject*, PyRecord*, bool followSymlinks, mode_t modeBits);

static PosixTriState posixDirEntryIsSymlinkState(JSGlobalObject* globalObject, PyRecord* entry)
{
    unsigned char type = posixDirEntryType(entry);
    if (type != DT_UNKNOWN)
        return type == DT_LNK ? PosixTriState::True : PosixTriState::False;
    return posixDirEntryTestMode(globalObject, entry, false, S_IFLNK);
}

static JSValue posixDirEntryGetStat(JSGlobalObject* globalObject, PyRecord* entry, bool followSymlinks)
{
    if (!followSymlinks)
        return posixDirEntryGetLstat(globalObject, entry);
    if (JSValue cached = entry->get(PosixDirEntryStat))
        return cached;
    PosixTriState isSymlink = posixDirEntryIsSymlinkState(globalObject, entry);
    if (isSymlink == PosixTriState::Exception)
        return { };
    JSValue result = isSymlink == PosixTriState::True ? posixDirEntryFetchStat(globalObject, entry, true) : posixDirEntryGetLstat(globalObject, entry);
    if (result)
        entry->set(getVM(globalObject), PosixDirEntryStat, result);
    return result;
}

static PosixTriState posixDirEntryTestMode(JSGlobalObject* globalObject, PyRecord* entry, bool followSymlinks, mode_t modeBits)
{
    unsigned char type = posixDirEntryType(entry);
    bool isSymlink = type == DT_LNK;
    if (type == DT_UNKNOWN || (followSymlinks && isSymlink)) {
        JSValue status = posixDirEntryGetStat(globalObject, entry, followSymlinks);
        if (!status) {
            // A file that is there no longer is neither a file nor a directory.
            if (clearPendingExceptionIfMatches(globalObject, runtime(globalObject).FileNotFoundError()))
                return PosixTriState::False;
            return PosixTriState::Exception;
        }
        int64_t mode;
        if (!posixToLong(globalObject, PyTuple::from(status)->at(0), mode))
            return PosixTriState::Exception;
        return (static_cast<mode_t>(mode) & S_IFMT) == modeBits ? PosixTriState::True : PosixTriState::False;
    }
    if (isSymlink)
        return PosixTriState::False;
    return type == (modeBits == S_IFDIR ? DT_DIR : DT_REG) ? PosixTriState::True : PosixTriState::False;
}

static JSValue posixFromTriState(PosixTriState state)
{
    if (state == PosixTriState::Exception)
        return { };
    return jsBoolean(state == PosixTriState::True);
}

static JSValue posixDirEntryIsDir(JSGlobalObject* globalObject, JSValue self, JSValue followSymlinksValue)
{
    bool followSymlinks;
    if (!posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    return posixFromTriState(posixDirEntryTestMode(globalObject, PyRecord::from(self), followSymlinks, S_IFDIR));
}

static JSValue posixDirEntryIsFile(JSGlobalObject* globalObject, JSValue self, JSValue followSymlinksValue)
{
    bool followSymlinks;
    if (!posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    return posixFromTriState(posixDirEntryTestMode(globalObject, PyRecord::from(self), followSymlinks, S_IFREG));
}

static JSValue posixDirEntryIsSymlink(JSGlobalObject* globalObject, JSValue self)
{
    return posixFromTriState(posixDirEntryIsSymlinkState(globalObject, PyRecord::from(self)));
}

static JSValue posixDirEntryIsJunction(JSGlobalObject*, JSValue) { return jsBoolean(false); }

static JSValue posixDirEntryStatMethod(JSGlobalObject* globalObject, JSValue self, JSValue followSymlinksValue)
{
    bool followSymlinks;
    if (!posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    return posixDirEntryGetStat(globalObject, PyRecord::from(self), followSymlinks);
}

static JSValue posixDirEntryInodeMethod(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(PosixDirEntryInode); }
static JSValue posixDirEntryGetName(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(PosixDirEntryName); }
static JSValue posixDirEntryGetPath(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(PosixDirEntryPath); }

static JSValue posixDirEntryRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = repr(globalObject, PyRecord::from(self)->get(PosixDirEntryName));
    RETURN_IF_EXCEPTION(scope, { });
    String text = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("<DirEntry "_s, text, '>'));
}

static constexpr NativeDef posixDirEntryMethods[] = {
    PY_SLOT("__repr__", posixDirEntryRepr, "($self, /)"),
    PY_METHOD("is_dir", posixDirEntryIsDir, "($self, /, *, follow_symlinks=True)"),
    PY_METHOD("is_file", posixDirEntryIsFile, "($self, /, *, follow_symlinks=True)"),
    PY_METHOD("is_symlink", posixDirEntryIsSymlink, "($self, /)"),
    PY_METHOD("is_junction", posixDirEntryIsJunction, "($self, /)"),
    PY_METHOD("stat", posixDirEntryStatMethod, "($self, /, *, follow_symlinks=True)"),
    PY_METHOD("inode", posixDirEntryInodeMethod, "($self, /)"),
    PY_METHOD("__fspath__", posixDirEntryGetPath, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet posixDirEntryGetSets[] = {
    PY_MEMBER("name", posixDirEntryGetName),
    PY_MEMBER("path", posixDirEntryGetPath),
};

static constexpr TypeDef posixDirEntryTypeDef { "posix.DirEntry"_s, { }, posixDirEntryMethods, posixDirEntryGetSets, { } };

// What os.scandir() returns. The directory is read at once, since nothing would close it if the iterator were dropped;
// `number` is the index of the next entry in the list, which is gone once the iterator is closed or at its end.
enum PosixScandirField : unsigned {
    PosixScandirEntries,
    PosixScandirFieldCount,
};

static void posixScandirIteratorShut(JSGlobalObject* globalObject, PyRecord* iterator)
{
    if (iterator->get(PosixScandirEntries))
        runtime(globalObject).unregisterFinalizable(iterator);
    iterator->set(getVM(globalObject), PosixScandirEntries, JSValue());
}

static JSValue posixScandirIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* iterator = PyRecord::from(self);
    if (JSValue entries = iterator->get(PosixScandirEntries)) {
        JSArray* list = asList(entries);
        if (iterator->number < listLength(list))
            return listGet(list, static_cast<unsigned>(iterator->number++));
        posixScandirIteratorShut(globalObject, iterator);
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseStopIteration(globalObject, scope);
}

static JSValue posixScandirIteratorIter(JSGlobalObject*, JSValue self) { return self; }

static JSValue posixScandirIteratorClose(JSGlobalObject* globalObject, JSValue self)
{
    posixScandirIteratorShut(globalObject, PyRecord::from(self));
    return jsUndefined();
}

static JSValue posixScandirIteratorExit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    posixScandirIteratorShut(globalObject, PyRecord::from(self));
    return jsUndefined();
}

// Nothing is open here: the directory was read at once. The program is told all the same, as elsewhere it leaks a descriptor.
static JSValue posixScandirIteratorFinalize(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    auto* iterator = PyRecord::from(self);
    if (!iterator->get(PosixScandirEntries))
        return jsUndefined();
    JSValue saved = takePendingException(globalObject);
    if (!warnAbout(globalObject, rt.ResourceWarning(), makeString("unclosed scandir iterator "_s, reprForMessage(globalObject, self)), 1, self)) {
        if (pendingExceptionMatches(globalObject, rt.Warning()))
            reportUnraisable(globalObject, makeString("Exception ignored while finalizing scandir iterator "_s, reprForMessage(globalObject, self)), { });
        else
            takePendingException(globalObject);
    }
    posixScandirIteratorShut(globalObject, iterator);
    if (saved) {
        auto scope = DECLARE_THROW_SCOPE(rt.vm());
        throwPythonException(globalObject, scope, saved);
        return { };
    }
    return jsUndefined();
}

static constexpr NativeDef posixScandirIteratorMethods[] = {
    PY_SLOT("__del__", posixScandirIteratorFinalize, "($self, /)"),
    PY_SLOT("__iter__", posixScandirIteratorIter, "($self, /)"),
    PY_SLOT("__next__", posixScandirIteratorNext, "($self, /)"),
    PY_METHOD("__enter__", posixScandirIteratorIter, "($self, /)"),
    PY_METHOD("__exit__", posixScandirIteratorExit, "($self, *args)"),
    PY_METHOD("close", posixScandirIteratorClose, "($self, /)"),
};

static constexpr TypeDef posixScandirIteratorTypeDef { "posix.ScandirIterator"_s, { }, posixScandirIteratorMethods, { }, { } };

static JSValue posixScandir(JSGlobalObject* globalObject, JSValue pathValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PosixPath path("scandir"_s, PosixPath::AllowFd | PosixPath::Nullable | PosixPath::AllowVirtual);
    if (!path.convert(globalObject, pathValue))
        return { };
    if (!audit(globalObject, "os.scandir"_s, path.object()))
        return { };
    PyType* entryType = rt.typeFor(posixDirEntryTypeDef);
    JSArray* entries = newList(globalObject);
    bool asBytes = path.isBytes();
    int dirFd = path.isFd() ? path.fd() : AT_FDCWD;

    // The directory, up to where the name of an entry goes.
    Vector<char, 256> joined;
    if (!path.isFd()) {
        if (path.narrow())
            joined.append(path.span());
        else
            joined.append('.');
        if (!joined.isEmpty() && joined.last() != '/')
            joined.append('/');
    }
    size_t prefixLength = joined.size();

    bool succeeded = posixForEachDirectoryEntry(globalObject, path, [&](std::span<const char> name, uint64_t inode, unsigned char type) {
        PyRecord* entry = PyRecord::create(globalObject, entryType, PosixDirEntryFieldCount);
        JSValue nameObject = posixFileName(globalObject, name, asBytes);
        entry->set(vm, PosixDirEntryName, nameObject);
        if (path.isFd())
            entry->set(vm, PosixDirEntryPath, nameObject);
        else {
            joined.shrink(prefixLength);
            joined.append(name);
            entry->set(vm, PosixDirEntryPath, posixFileName(globalObject, joined.span(), asBytes));
        }
        entry->set(vm, PosixDirEntryInode, makeInt(globalObject, inode));
        entry->number = (static_cast<int64_t>(dirFd) << 8) | type;
        listAppend(globalObject, entries, entry);
        return !vm.exceptionForInspection();
    });
    if (!succeeded)
        return { };
    PyRecord* iterator = PyRecord::create(globalObject, rt.typeFor(posixScandirIteratorTypeDef), PosixScandirFieldCount);
    iterator->set(vm, PosixScandirEntries, entries);
    rt.registerFinalizable(iterator);
    return iterator;
}

// ---- Extended attributes ----

#if OS(LINUX)
// The kernel's limits, from <linux/limits.h>, which does not agree with every C library about the rest of what it defines.
static constexpr size_t posixMaxAttributeSize = 65536;
static constexpr size_t posixMaxAttributeListSize = 65536;

static JSValue posixGetxattr(JSGlobalObject* globalObject, JSValue pathValue, JSValue attributeValue, JSValue followSymlinksValue)
{
    PosixPath path("getxattr"_s, PosixPath::AllowFd | PosixPath::VirtualUnsupported);
    PosixPath attribute("getxattr"_s, "attribute"_s);
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !attribute.convert(globalObject, attributeValue) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (!audit(globalObject, "os.getxattr"_s, path.object(), attribute.object()))
        return { };
    if (posixFdAndFollowSymlinksInvalid(globalObject, "getxattr"_s, path.isFd(), followSymlinks))
        return { };
    Vector<char, 128> buffer;
    for (size_t size : { static_cast<size_t>(128), posixMaxAttributeSize }) {
        buffer.grow(size);
        ssize_t result;
        if (path.isFd())
            result = fgetxattr(path.fd(), attribute.narrow(), buffer.mutableSpan().data(), size);
        else if (followSymlinks)
            result = getxattr(path.narrow(), attribute.narrow(), buffer.mutableSpan().data(), size);
        else
            result = lgetxattr(path.narrow(), attribute.narrow(), buffer.mutableSpan().data(), size);
        if (result >= 0)
            return PyBytes::create(globalObject, buffer.span().first(static_cast<size_t>(result)));
        if (errno != ERANGE)
            break;
    }
    return path.error(globalObject);
}

static JSValue posixSetxattr(JSGlobalObject* globalObject, JSValue pathValue, JSValue attributeValue, JSValue value, JSValue flagsValue, JSValue followSymlinksValue)
{
    PosixPath path("setxattr"_s, PosixPath::AllowFd);
    PosixPath attribute("setxattr"_s, "attribute"_s);
    BufferHolder holder(globalObject);
    int flags = 0;
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !attribute.convert(globalObject, attributeValue) || !holder.get(value)
        || (flagsValue && !posixToInt(globalObject, flagsValue, flags)) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks)
        || !refreshBuffer(globalObject, holder.buffer))
        return { };
    if (!audit(globalObject, "os.setxattr"_s, path.object(), attribute.object(), value, flagsValue ? flagsValue : jsNumber(0)))
        return { };
    if (posixFdAndFollowSymlinksInvalid(globalObject, "setxattr"_s, path.isFd(), followSymlinks))
        return { };
    auto bytes = holder.buffer.span();
    int result;
    if (path.isFd())
        result = fsetxattr(path.fd(), attribute.narrow(), bytes.data(), bytes.size(), flags);
    else if (followSymlinks)
        result = setxattr(path.narrow(), attribute.narrow(), bytes.data(), bytes.size(), flags);
    else
        result = lsetxattr(path.narrow(), attribute.narrow(), bytes.data(), bytes.size(), flags);
    if (result)
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixRemovexattr(JSGlobalObject* globalObject, JSValue pathValue, JSValue attributeValue, JSValue followSymlinksValue)
{
    PosixPath path("removexattr"_s, PosixPath::AllowFd);
    PosixPath attribute("removexattr"_s, "attribute"_s);
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !attribute.convert(globalObject, attributeValue) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (!audit(globalObject, "os.removexattr"_s, path.object(), attribute.object()))
        return { };
    if (posixFdAndFollowSymlinksInvalid(globalObject, "removexattr"_s, path.isFd(), followSymlinks))
        return { };
    int result;
    if (path.isFd())
        result = fremovexattr(path.fd(), attribute.narrow());
    else if (followSymlinks)
        result = removexattr(path.narrow(), attribute.narrow());
    else
        result = lremovexattr(path.narrow(), attribute.narrow());
    if (result)
        return path.error(globalObject);
    return jsUndefined();
}

static JSValue posixListxattr(JSGlobalObject* globalObject, JSValue pathValue, JSValue followSymlinksValue)
{
    PosixPath path("listxattr"_s, PosixPath::AllowFd | PosixPath::Nullable | PosixPath::VirtualUnsupported);
    bool followSymlinks;
    if (!path.convert(globalObject, pathValue) || !posixToBool(globalObject, followSymlinksValue, true, followSymlinks))
        return { };
    if (!audit(globalObject, "os.listxattr"_s, path.object()))
        return { };
    if (posixFdAndFollowSymlinksInvalid(globalObject, "listxattr"_s, path.isFd(), followSymlinks))
        return { };
    const char* name = path.narrow() ? path.narrow() : ".";
    Vector<char, 256> buffer;
    for (size_t size : { static_cast<size_t>(256), posixMaxAttributeListSize }) {
        buffer.grow(size);
        ssize_t length;
        if (path.isFd())
            length = flistxattr(path.fd(), buffer.mutableSpan().data(), size);
        else if (followSymlinks)
            length = listxattr(name, buffer.mutableSpan().data(), size);
        else
            length = llistxattr(name, buffer.mutableSpan().data(), size);
        if (length < 0) {
            if (errno == ERANGE)
                continue;
            break;
        }
        // The names, each with a NUL after it.
        JSArray* result = newList(globalObject);
        auto names = buffer.span().first(static_cast<size_t>(length));
        size_t start = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i])
                continue;
            listAppend(globalObject, result, posixDecode(globalObject, names.subspan(start, i - start)));
            start = i + 1;
        }
        return result;
    }
    return path.error(globalObject);
}
#endif // OS(LINUX)

// ---- The table ----

static constexpr NativeDef posixFileFunctions[] = {
    PY_FUNCTION("stat", posixStat, "(path, *, dir_fd=None, follow_symlinks=True)"),
    PY_FUNCTION("lstat", posixLstat, "(path, *, dir_fd=None)"),
#if OS(LINUX)
    PY_FUNCTION("statx", posixStatxFunction, "(path, mask, *, flags=0, dir_fd=None, follow_symlinks=True)"),
#endif
    PY_FUNCTION("access", posixAccess, "(path, mode, *, dir_fd=None, effective_ids=False, follow_symlinks=True)"),
    PY_FUNCTION("chdir", posixChdir, "(path)"),
    PY_FUNCTION("getcwd", posixGetcwd, "()"),
    PY_FUNCTION("getcwdb", posixGetcwdb, "()"),
    PY_FUNCTION("chroot", posixChroot, "(path)"),
    PY_FUNCTION("chmod", posixChmod, "(path, mode, *, dir_fd=None, follow_symlinks=True)"),
#if OS(DARWIN)
    PY_FUNCTION("lchmod", posixLchmod, "(path, mode)"),
    PY_FUNCTION("chflags", posixChflags, "(path, flags, follow_symlinks=True)"),
    PY_FUNCTION("lchflags", posixLchflags, "(path, flags)"),
#endif
    PY_FUNCTION("chown", posixChown, "(path, uid, gid, *, dir_fd=None, follow_symlinks=True)"),
    PY_FUNCTION("lchown", posixLchown, "(path, uid, gid)"),
    PY_FUNCTION("link", posixLink, "(src, dst, *, src_dir_fd=None, dst_dir_fd=None, follow_symlinks=True)"),
    PY_FUNCTION("symlink", posixSymlink, "(src, dst, target_is_directory=False, *, dir_fd=None)"),
    PY_FUNCTION("readlink", posixReadlink, "(path, *, dir_fd=None)"),
    PY_FUNCTION("mkdir", posixMkdir, "(path, mode=511, *, dir_fd=None)"),
    PY_FUNCTION("rmdir", posixRmdir, "(path, *, dir_fd=None)"),
    PY_FUNCTION("unlink", posixUnlink, "(path, *, dir_fd=None)"),
    PY_FUNCTION("remove", posixRemove, "(path, *, dir_fd=None)"),
    PY_FUNCTION("rename", posixRename, "(src, dst, *, src_dir_fd=None, dst_dir_fd=None)"),
    PY_FUNCTION("replace", posixReplace, "(src, dst, *, src_dir_fd=None, dst_dir_fd=None)"),
    PY_FUNCTION("mkfifo", posixMkfifo, "(path, mode=438, *, dir_fd=None)"),
    PY_FUNCTION("mknod", posixMknod, "(path, mode=384, device=0, *, dir_fd=None)"),
    PY_FUNCTION("major", posixMajor, "(device, /)"),
    PY_FUNCTION("minor", posixMinor, "(device, /)"),
    PY_FUNCTION("makedev", posixMakedev, "(major, minor, /)"),
    PY_FUNCTION("truncate", posixTruncate, "(path, length)"),
    PY_FUNCTION("utime", posixUtime, "(path, times=None, *, ns=<unrepresentable>, dir_fd=None, follow_symlinks=True)"),
    PY_FUNCTION("statvfs", posixStatvfs, "(path)"),
    PY_FUNCTION("fstatvfs", posixFstatvfs, "(fd, /)"),
    PY_FUNCTION("sync", posixSync, "()"),
    PY_FUNCTION("listdir", posixListdir, "(path=None)"),
    PY_FUNCTION("scandir", posixScandir, "(path=None)"),
#if OS(LINUX)
    PY_FUNCTION("getxattr", posixGetxattr, "(path, attribute, *, follow_symlinks=True)"),
    PY_FUNCTION("setxattr", posixSetxattr, "(path, attribute, value, flags=0, *, follow_symlinks=True)"),
    PY_FUNCTION("removexattr", posixRemovexattr, "(path, attribute, *, follow_symlinks=True)"),
    PY_FUNCTION("listxattr", posixListxattr, "(path=None, *, follow_symlinks=True)"),
#endif
};

void addPosixFileFunctions(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, posixFileFunctions);
    module->add(globalObject, "DirEntry"_s, runtime(globalObject).typeFor(posixDirEntryTypeDef));
    module->add(globalObject, "statvfs_result"_s, cachedStructSeqType(globalObject, posixStatvfsResultDef));
#if OS(LINUX)
    module->add(globalObject, "statx_result"_s, runtime(globalObject).typeFor(posixStatxResultTypeDef));
#endif
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
