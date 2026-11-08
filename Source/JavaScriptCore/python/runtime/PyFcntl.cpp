/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include "PyThreads.h"
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#if OS(LINUX) && !defined(FICLONE)
// As in <linux/fs.h>, which defines much else.
#define FICLONE _IOW(0x94, 9, int)
#define FICLONERANGE _IOW(0x94, 13, char[32])
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module fcntl. (Modules/fcntlmodule.c in CPython.)

// fcntl() and ioctl() take an int or the address of a structure, which one depending on the operation: the caller passes an
// int, or the bytes of the structure. How much the system writes there is more than can be known here, so what follows
// the bytes is checked afterwards.
static constexpr size_t fcntlInlineSize = 1024;
static constexpr uint8_t fcntlGuard[8] = { 0x00, 0xfa, 0x69, 0xc4, 0x67, 0xa3, 0x6c, 0x58 };

// The bytes of a str (in UTF-8) or of a bytes-like object.
static bool fcntlToBytes(JSGlobalObject* globalObject, JSValue value, Vector<uint8_t, fcntlInlineSize + sizeof(fcntlGuard)>& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(value)) {
        JSValue encoded = strEncode(globalObject, strValue(value), nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, false);
        result.append(PyBytes::from(encoded)->span());
    } else {
        BytesHolder bytes(globalObject);
        if (failed(scope, bytes.getLast(value)))
            return false;
        result.append(bytes.span());
    }
    result.append(std::span<const uint8_t> { fcntlGuard });
    return true;
}

static bool fcntlIsBuffer(JSValue value) { return isStr(value) || hasBuffer(value); }

// call(address) with the address of a copy of the bytes, which is the result.
template<typename Function>
static JSValue fcntlCallWithBytes(JSGlobalObject* globalObject, JSValue argument, ASCIILiteral function, const Function& call)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<uint8_t, fcntlInlineSize + sizeof(fcntlGuard)> buffer;
    if (failed(scope, fcntlToBytes(globalObject, argument, buffer)))
        return { };
    size_t length = buffer.size() - sizeof(fcntlGuard);
    int result;
    do {
        AllowThreads allow(globalObject);
        result = call(buffer.mutableSpan().data());
    } while (result == -1 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    if (memcmp(buffer.span().data() + length, fcntlGuard, sizeof(fcntlGuard))) {
        return raiseSystemError(globalObject, scope, length <= fcntlInlineSize ? "Possible stack corruption in "_s : "Memory corruption in "_s, function,
            "() due to buffer overflow. Provide an argument of sufficient size as determined by the operation."_s);
    }
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, buffer.span().first(length)));
}

template<typename Function>
static JSValue fcntlCallWithInt(JSGlobalObject* globalObject, const Function& call)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int result;
    do {
        AllowThreads allow(globalObject);
        result = call();
    } while (result == -1 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsNumber(result);
}

static JSValue fcntlBadArgument(JSGlobalObject* globalObject, ASCIILiteral function, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, function, "() argument 3 must be an integer, a bytes-like object, or a string, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, argument)));
}

// Any int, of which the low bits are kept.
static bool fcntlToBits(JSGlobalObject* globalObject, JSValue value, uint64_t& result, bool& fits)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    fits = true;
    if (auto converted = tryInt64(index)) {
        result = static_cast<uint64_t>(*converted);
        return true;
    }
    if (auto converted = tryUInt64(index)) {
        result = *converted;
        return true;
    }
    fits = false;
    JSValue low = intAnd(globalObject, index, makeInt(globalObject, std::numeric_limits<uint64_t>::max()));
    RETURN_IF_EXCEPTION(scope, false);
    result = tryUInt64(low).value_or(0);
    return true;
}

static JSValue fcntlFcntl(JSGlobalObject* globalObject, JSValue fdValue, JSValue commandValue, JSValue argument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int command;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || failed(scope, posixToInt(globalObject, commandValue, command)))
        return { };
    if (failed(scope, audit(globalObject, "fcntl.fcntl"_s, jsNumber(fd), jsNumber(command), argument)))
        return { };
    if (!argument || hasIndex(globalObject, argument)) {
        uint64_t bits = 0;
        bool fits;
        if (argument && failed(scope, fcntlToBits(globalObject, argument, bits, fits)))
            return { };
        RELEASE_AND_RETURN(scope, fcntlCallWithInt(globalObject, [&] { return fcntl(fd, command, static_cast<int>(static_cast<unsigned>(bits))); }));
    }
    if (!fcntlIsBuffer(argument))
        RELEASE_AND_RETURN(scope, fcntlBadArgument(globalObject, "fcntl"_s, argument));
    RELEASE_AND_RETURN(scope, fcntlCallWithBytes(globalObject, argument, "fcntl"_s, [&](uint8_t* address) { return fcntl(fd, command, address); }));
}

static JSValue fcntlIoctl(JSGlobalObject* globalObject, JSValue fdValue, JSValue requestValue, JSValue argument, JSValue mutateValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)))
        return { };
    if (!hasIndex(globalObject, requestValue))
        return raiseTypeError(globalObject, scope, "ioctl() argument 2 must be int, not "_s, posixTypeNameOfArgument(globalObject, requestValue));
    uint64_t request;
    bool fits;
    if (failed(scope, fcntlToBits(globalObject, requestValue, request, fits)))
        return { };
    if (failed(scope, audit(globalObject, "fcntl.ioctl"_s, jsNumber(fd), requestValue, argument)))
        return { };
    if (!fits && failed(scope, warn(globalObject, runtime(globalObject).DeprecationWarning(), "integer value out of range"_s)))
        return { };
    bool mutate;
    if (failed(scope, posixToBool(globalObject, mutateValue, true, mutate)))
        return { };

    if (!argument || hasIndex(globalObject, argument)) {
        int64_t number = 0;
        if (argument && failed(scope, posixToLong(globalObject, argument, number)))
            return { };
        if (number > std::numeric_limits<int>::max())
            return raiseOverflowError(globalObject, scope, "signed integer is greater than maximum"_s);
        if (number < std::numeric_limits<int>::min())
            return raiseOverflowError(globalObject, scope, "signed integer is less than minimum"_s);
        RELEASE_AND_RETURN(scope, fcntlCallWithInt(globalObject, [&] { return ioctl(fd, request, static_cast<int>(number)); }));
    }
    if (!fcntlIsBuffer(argument))
        RELEASE_AND_RETURN(scope, fcntlBadArgument(globalObject, "ioctl"_s, argument));

    // What can be written to gets what the system wrote, and the result is the system's.
    if (mutate && !PyBytes::is(argument) && !isStr(argument)) {
        BufferHolder holder(globalObject);
        if (!failed(scope, holder.get(argument, BufferWritable))) {
            auto span = holder.buffer.mutableSpan();
            uint8_t copy[fcntlInlineSize + sizeof(fcntlGuard)];
            bool usesCopy = span.size() <= fcntlInlineSize;
            if (usesCopy) {
                memcpy(copy, span.data(), span.size());
                memcpy(copy + span.size(), fcntlGuard, sizeof(fcntlGuard));
            }
            int result;
            do {
                if (failed(scope, refreshBuffer(globalObject, holder.buffer)))
                    return { };
                AllowThreads allow(globalObject);
                result = ioctl(fd, request, usesCopy ? copy : holder.buffer.data);
            } while (result == -1 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
            if (result < 0)
                RELEASE_AND_RETURN(scope, posixError(globalObject));
            if (usesCopy) {
                memcpy(holder.buffer.data, copy, span.size());
                if (memcmp(copy + span.size(), fcntlGuard, sizeof(fcntlGuard))) {
                    return raiseSystemError(globalObject, scope, "Possible stack corruption in ioctl() due to buffer overflow. "
                        "Provide a writable buffer argument of sufficient size as determined by the operation."_s);
                }
            }
            return jsNumber(result);
        }
        if (!clearPendingExceptionIfMatches(globalObject, runtime(globalObject).BufferError()))
            return { };
    }
    RELEASE_AND_RETURN(scope, fcntlCallWithBytes(globalObject, argument, "ioctl"_s, [&](uint8_t* address) { return ioctl(fd, request, address); }));
}

static JSValue fcntlNoneUnlessFailed(JSGlobalObject* globalObject, int result)
{
    if (result < 0)
        return posixError(globalObject);
    return jsUndefined();
}

// The descriptors through which this process has taken a lock with flock() and has not given it back. One that has been closed since
// goes when it is next looked at; one whose number is in use again is at worst a collection for nothing. One list for the process,
// whatever runs Python in it, as descriptors are the process's.
struct OwnLock {
    int fd;
    // A collection that was made for the sake of such a lock has left it open: somebody had it then.
    bool wasSeenInUse;
};
static Lock s_ownLocksLock;
static Vector<OwnLock> s_ownLocks WTF_GUARDED_BY_LOCK(s_ownLocksLock);

static void fcntlDidFlock(int fd, bool holdsLock)
{
    Locker locker { s_ownLocksLock };
    s_ownLocks.removeFirstMatching([&](auto& lock) {
        return lock.fd == fd;
    });
    if (holdsLock)
        s_ownLocks.append({ fd, false });
}

enum class OwnLocks : uint8_t { None, SeenInUse, NeverLookedAt };

// On the file of this descriptor, through others.
static OwnLocks fcntlOwnLocksOnFileOf(int fd)
{
    struct stat wanted;
    if (fstat(fd, &wanted))
        return OwnLocks::None;
    OwnLocks result = OwnLocks::None;
    Locker locker { s_ownLocksLock };
    s_ownLocks.removeAllMatching([&](auto& lock) {
        struct stat locked;
        if (fstat(lock.fd, &locked))
            return true;
        if (lock.fd != fd && locked.st_dev == wanted.st_dev && locked.st_ino == wanted.st_ino)
            result = std::max(result, lock.wasSeenInUse ? OwnLocks::SeenInUse : OwnLocks::NeverLookedAt);
        return false;
    });
    return result;
}

static void fcntlDidLookForDroppedFiles()
{
    Locker locker { s_ownLocksLock };
    for (auto& lock : s_ownLocks)
        lock.wasSeenInUse = true;
}

static JSValue fcntlFlock(JSGlobalObject* globalObject, JSValue fdValue, JSValue operationValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int operation;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || failed(scope, posixToInt(globalObject, operationValue, operation)))
        return { };
    if (failed(scope, audit(globalObject, "fcntl.flock"_s, jsNumber(fd), jsNumber(operation))))
        return { };
    // With reference counts a file that the program has dropped is closed, and has given back its lock. Here it holds it until the
    // collector comes. A call that waits for the lock would keep waiting for that: nothing is allocated meanwhile, and it cannot be
    // made piece by piece. One that does not wait (LOCK_NB: "is another instance running?") would be told that somebody has the lock.
    // So if the lock is not free, and this very process has one on the same file, the collector looks first.
    int result = -1;
    bool isAnswered = false;
    if (!(operation & LOCK_UN)) {
        {
            // It does not wait for the lock, but on a file system of the network it waits for the answer.
            AllowThreads allow(globalObject, AllowThreads::LosesNothing);
            result = flock(fd, operation | LOCK_NB);
        }
        // Noted at once: what follows makes system calls of its own, which may fail. (So a call that is refused asks once more.)
        bool isTaken = result == -1 && errno == EWOULDBLOCK;
        bool wasInterrupted = result == -1 && errno == EINTR;
        isAnswered = !isTaken && !wasInterrupted;
        if (wasInterrupted && failed(scope, checkSignals(globalObject)))
            return { };
        if (OwnLocks own = isTaken ? fcntlOwnLocksOnFileOf(fd) : OwnLocks::None; own != OwnLocks::None) {
            // A lock that no collection has looked at is worth one, whatever the share of the time that such collections have. If all
            // were in use the last time, this may be a program whose threads contend for the lock, or that polls: within the share.
            // But if the call would wait, and there is no other thread, nothing else can end the wait, as far as this process goes.
            IdleLook look = own == OwnLocks::NeverLookedAt ? IdleLook::WhateverTheShare : IdleLook::WithinShare;
            if (!(operation & LOCK_NB) && !runtime(globalObject).threads().runningCount())
                look = IdleLook::LastResort;
            int64_t deadline = -1;
            IdleWait idle(globalObject, deadline, true);
            // A hook hears of a call once.
            if (isAudited(globalObject))
                idle.mustNotBeginCallAgain();
            bool hasLooked;
            if (failed(scope, idle.collectBeforeBlocking(look, hasLooked)))
                return { };
            if (hasLooked)
                fcntlDidLookForDroppedFiles();
        }
    }
    if (!isAnswered) {
        do {
            AllowThreads allow(globalObject, AllowThreads::LosesNothing);
            result = flock(fd, operation);
        } while (result == -1 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    }
    if (!result)
        fcntlDidFlock(fd, !(operation & LOCK_UN));
    RELEASE_AND_RETURN(scope, fcntlNoneUnlessFailed(globalObject, result));
}

static JSValue fcntlLockf(JSGlobalObject* globalObject, JSValue fdValue, JSValue commandValue, JSValue lengthValue, JSValue startValue, JSValue whenceValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int command;
    int whence = 0;
    if (failed(scope, posixToFileDescriptor(globalObject, fdValue, fd)) || failed(scope, posixToInt(globalObject, commandValue, command)) || (whenceValue && failed(scope, posixToInt(globalObject, whenceValue, whence))))
        return { };
    if (failed(scope, audit(globalObject, "fcntl.lockf"_s, jsNumber(fd), jsNumber(command), lengthValue, startValue, jsNumber(whence))))
        return { };
    struct flock lock { };
    if (command == LOCK_UN)
        lock.l_type = F_UNLCK;
    else if (command & LOCK_SH)
        lock.l_type = F_RDLCK;
    else if (command & LOCK_EX)
        lock.l_type = F_WRLCK;
    else {
        return raiseValueError(globalObject, scope, "unrecognized lockf argument"_s);
    }
    if ((startValue && failed(scope, posixToOffset(globalObject, startValue, lock.l_start))) || (lengthValue && failed(scope, posixToOffset(globalObject, lengthValue, lock.l_len))))
        return { };
    lock.l_whence = static_cast<short>(whence);
    int result;
    do {
        AllowThreads allow(globalObject, AllowThreads::LosesNothing);
        result = fcntl(fd, command & LOCK_NB ? F_SETLK : F_SETLKW, &lock);
    } while (result == -1 && errno == EINTR && !failed(scope, checkSignals(globalObject)));
    RELEASE_AND_RETURN(scope, fcntlNoneUnlessFailed(globalObject, result));
}

static constexpr NativeDef fcntlFunctions[] = {
    PY_FUNCTION("fcntl", fcntlFcntl, "(fd, cmd, arg=0, /)"),
    PY_FUNCTION("ioctl", fcntlIoctl, "(fd, request, arg=0, mutate_flag=True, /)"),
    NativeDef(NativeKind::Function, "flock"_s, fcntlFlock, "(fd, operation, /)"_s, MayWait),
    PY_FUNCTION("lockf", fcntlLockf, "(fd, cmd, len=0, start=0, whence=0, /)"),
};

static constexpr PosixConstant fcntlConstants[] = {
#ifdef LOCK_SH
    { "LOCK_SH"_s, LOCK_SH },
#endif
#ifdef LOCK_EX
    { "LOCK_EX"_s, LOCK_EX },
#endif
#ifdef LOCK_NB
    { "LOCK_NB"_s, LOCK_NB },
#endif
#ifdef LOCK_UN
    { "LOCK_UN"_s, LOCK_UN },
#endif
#ifdef LOCK_MAND
    { "LOCK_MAND"_s, LOCK_MAND },
#endif
#ifdef LOCK_READ
    { "LOCK_READ"_s, LOCK_READ },
#endif
#ifdef LOCK_WRITE
    { "LOCK_WRITE"_s, LOCK_WRITE },
#endif
#ifdef LOCK_RW
    { "LOCK_RW"_s, LOCK_RW },
#endif
#ifdef F_DUPFD
    { "F_DUPFD"_s, F_DUPFD },
#endif
#ifdef F_DUPFD_CLOEXEC
    { "F_DUPFD_CLOEXEC"_s, F_DUPFD_CLOEXEC },
#endif
#ifdef F_GETFD
    { "F_GETFD"_s, F_GETFD },
#endif
#ifdef F_SETFD
    { "F_SETFD"_s, F_SETFD },
#endif
#ifdef F_GETFL
    { "F_GETFL"_s, F_GETFL },
#endif
#ifdef F_SETFL
    { "F_SETFL"_s, F_SETFL },
#endif
#ifdef F_GETLK
    { "F_GETLK"_s, F_GETLK },
#endif
#ifdef F_SETLK
    { "F_SETLK"_s, F_SETLK },
#endif
#ifdef F_SETLKW
    { "F_SETLKW"_s, F_SETLKW },
#endif
#ifdef F_OFD_GETLK
    { "F_OFD_GETLK"_s, F_OFD_GETLK },
#endif
#ifdef F_OFD_SETLK
    { "F_OFD_SETLK"_s, F_OFD_SETLK },
#endif
#ifdef F_OFD_SETLKW
    { "F_OFD_SETLKW"_s, F_OFD_SETLKW },
#endif
#ifdef F_GETOWN
    { "F_GETOWN"_s, F_GETOWN },
#endif
#ifdef F_SETOWN
    { "F_SETOWN"_s, F_SETOWN },
#endif
#ifdef F_GETPATH
    { "F_GETPATH"_s, F_GETPATH },
#endif
#ifdef F_GETSIG
    { "F_GETSIG"_s, F_GETSIG },
#endif
#ifdef F_SETSIG
    { "F_SETSIG"_s, F_SETSIG },
#endif
#ifdef F_RDLCK
    { "F_RDLCK"_s, F_RDLCK },
#endif
#ifdef F_WRLCK
    { "F_WRLCK"_s, F_WRLCK },
#endif
#ifdef F_UNLCK
    { "F_UNLCK"_s, F_UNLCK },
#endif
#ifdef F_GETLK64
    { "F_GETLK64"_s, F_GETLK64 },
#endif
#ifdef F_SETLK64
    { "F_SETLK64"_s, F_SETLK64 },
#endif
#ifdef F_SETLKW64
    { "F_SETLKW64"_s, F_SETLKW64 },
#endif
#ifdef FASYNC
    { "FASYNC"_s, FASYNC },
#endif
#ifdef F_SETLEASE
    { "F_SETLEASE"_s, F_SETLEASE },
#endif
#ifdef F_GETLEASE
    { "F_GETLEASE"_s, F_GETLEASE },
#endif
#ifdef F_NOTIFY
    { "F_NOTIFY"_s, F_NOTIFY },
#endif
#ifdef F_DUPFD_QUERY
    { "F_DUPFD_QUERY"_s, F_DUPFD_QUERY },
#endif
#ifdef F_EXLCK
    { "F_EXLCK"_s, F_EXLCK },
#endif
#ifdef F_SHLCK
    { "F_SHLCK"_s, F_SHLCK },
#endif
#ifdef F_SETPIPE_SZ
    { "F_SETPIPE_SZ"_s, F_SETPIPE_SZ },
#endif
#ifdef F_GETPIPE_SZ
    { "F_GETPIPE_SZ"_s, F_GETPIPE_SZ },
#endif
#ifdef FICLONE
    { "FICLONE"_s, FICLONE },
#endif
#ifdef FICLONERANGE
    { "FICLONERANGE"_s, FICLONERANGE },
#endif
#ifdef F_GETOWN_EX
    { "F_GETOWN_EX"_s, F_GETOWN_EX },
#endif
#ifdef F_SETOWN_EX
    { "F_SETOWN_EX"_s, F_SETOWN_EX },
#endif
#ifdef F_GETOWN_EX
    { "F_OWNER_TID"_s, F_OWNER_TID },
#endif
#ifdef F_GETOWN_EX
    { "F_OWNER_PID"_s, F_OWNER_PID },
#endif
#ifdef F_GETOWN_EX
    { "F_OWNER_PGRP"_s, F_OWNER_PGRP },
#endif
#ifdef F_GET_RW_HINT
    { "F_GET_RW_HINT"_s, F_GET_RW_HINT },
#endif
#ifdef F_SET_RW_HINT
    { "F_SET_RW_HINT"_s, F_SET_RW_HINT },
#endif
#ifdef F_GET_FILE_RW_HINT
    { "F_GET_FILE_RW_HINT"_s, F_GET_FILE_RW_HINT },
#endif
#ifdef F_SET_FILE_RW_HINT
    { "F_SET_FILE_RW_HINT"_s, F_SET_FILE_RW_HINT },
#endif
#ifdef RWH_WRITE_LIFE_NOT_SET
    { "RWH_WRITE_LIFE_NOT_SET"_s, RWH_WRITE_LIFE_NOT_SET },
#endif
#ifdef RWH_WRITE_LIFE_NONE
    { "RWH_WRITE_LIFE_NONE"_s, RWH_WRITE_LIFE_NONE },
#endif
#ifdef RWH_WRITE_LIFE_SHORT
    { "RWH_WRITE_LIFE_SHORT"_s, RWH_WRITE_LIFE_SHORT },
#endif
#ifdef RWH_WRITE_LIFE_MEDIUM
    { "RWH_WRITE_LIFE_MEDIUM"_s, RWH_WRITE_LIFE_MEDIUM },
#endif
#ifdef RWH_WRITE_LIFE_LONG
    { "RWH_WRITE_LIFE_LONG"_s, RWH_WRITE_LIFE_LONG },
#endif
#ifdef RWH_WRITE_LIFE_EXTREME
    { "RWH_WRITE_LIFE_EXTREME"_s, RWH_WRITE_LIFE_EXTREME },
#endif
#ifdef F_FULLFSYNC
    { "F_FULLFSYNC"_s, F_FULLFSYNC },
#endif
#ifdef F_NOCACHE
    { "F_NOCACHE"_s, F_NOCACHE },
#endif
#ifdef F_DUP2FD
    { "F_DUP2FD"_s, F_DUP2FD },
#endif
#ifdef F_DUP2FD_CLOEXEC
    { "F_DUP2FD_CLOEXEC"_s, F_DUP2FD_CLOEXEC },
#endif
#ifdef F_READAHEAD
    { "F_READAHEAD"_s, F_READAHEAD },
#endif
#ifdef F_RDAHEAD
    { "F_RDAHEAD"_s, F_RDAHEAD },
#endif
#ifdef F_ISUNIONSTACK
    { "F_ISUNIONSTACK"_s, F_ISUNIONSTACK },
#endif
#ifdef F_KINFO
    { "F_KINFO"_s, F_KINFO },
#endif
#ifdef F_CLOSEM
    { "F_CLOSEM"_s, F_CLOSEM },
#endif
#ifdef F_MAXFD
    { "F_MAXFD"_s, F_MAXFD },
#endif
#ifdef F_GETNOSIGPIPE
    { "F_GETNOSIGPIPE"_s, F_GETNOSIGPIPE },
#endif
#ifdef F_SETNOSIGPIPE
    { "F_SETNOSIGPIPE"_s, F_SETNOSIGPIPE },
#endif
#ifdef FD_CLOEXEC
    { "FD_CLOEXEC"_s, FD_CLOEXEC },
#endif
#ifdef DN_ACCESS
    { "DN_ACCESS"_s, DN_ACCESS },
#endif
#ifdef DN_MODIFY
    { "DN_MODIFY"_s, DN_MODIFY },
#endif
#ifdef DN_CREATE
    { "DN_CREATE"_s, DN_CREATE },
#endif
#ifdef DN_DELETE
    { "DN_DELETE"_s, DN_DELETE },
#endif
#ifdef DN_RENAME
    { "DN_RENAME"_s, DN_RENAME },
#endif
#ifdef DN_ATTRIB
    { "DN_ATTRIB"_s, DN_ATTRIB },
#endif
#ifdef DN_MULTISHOT
    { "DN_MULTISHOT"_s, DN_MULTISHOT },
#endif
#ifdef F_ADD_SEALS
    { "F_ADD_SEALS"_s, F_ADD_SEALS },
#endif
#ifdef F_GET_SEALS
    { "F_GET_SEALS"_s, F_GET_SEALS },
#endif
#ifdef F_SEAL_SEAL
    { "F_SEAL_SEAL"_s, F_SEAL_SEAL },
#endif
#ifdef F_SEAL_SHRINK
    { "F_SEAL_SHRINK"_s, F_SEAL_SHRINK },
#endif
#ifdef F_SEAL_GROW
    { "F_SEAL_GROW"_s, F_SEAL_GROW },
#endif
#ifdef F_SEAL_WRITE
    { "F_SEAL_WRITE"_s, F_SEAL_WRITE },
#endif
#ifdef F_SEAL_FUTURE_WRITE
    { "F_SEAL_FUTURE_WRITE"_s, F_SEAL_FUTURE_WRITE },
#endif
};

void initializeFcntlModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, fcntlFunctions);
    addPosixConstants(globalObject, module, fcntlConstants);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
