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
#include "PythonPosixModule.h"

#if OS(UNIX)

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if OS(LINUX) && __has_include(<linux/fs.h>)
#include <linux/fs.h> // FICLONE
#endif

// The module fcntl: Modules/fcntlmodule.c of CPython. Like posix, it is for whoever embeds the engine to say whether a program is to have it.

namespace JSC { namespace Python {

namespace {

// What comes after what the system is given to write in, so that it shows if it has written more than it was given room for: a zero, and then bytes that mean nothing.
constexpr std::array<uint8_t, 8> guard { 0x00, 0xfa, 0x69, 0xc4, 0x67, 0xa3, 0x6c, 0x58 };
// As much as is copied to be given to the system: FCNTL_BUFSZ and IOCTL_BUFSZ
constexpr size_t mostCopied = 1024;

struct GuardedCopy {
    explicit GuardedCopy(std::span<const uint8_t> bytes)
        : length(bytes.size())
    {
        ASSERT(length <= mostCopied);
        memcpySpan(std::span(storage), bytes);
        memcpySpan(std::span(storage).subspan(length), std::span(guard));
    }

    std::span<const uint8_t> bytes() const { return std::span(storage).first(length); }
    bool isIntact() const { return equalSpans(std::span(storage).subspan(length, guard.size()), std::span(guard)); }

    std::array<uint8_t, mostCopied + guard.size()> storage;
    size_t length;
};

// PyIndex_Check()
bool isIndex(JSGlobalObject* globalObject, JSValue value)
{
    return classify(value).isInt() || typeOf(globalObject, value)->lookup(globalObject->vm(), globalObject->vm().pythonNames().dunder_index);
}

// Calls the system until it is not that a signal came in the middle of it. False if it raised, which it does if the system says no.
template<typename Function>
bool callUntilNotInterrupted(JSGlobalObject* globalObject, int& result, const Function& function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    do {
        result = function();
        if (result != -1 || errno != EINTR)
            break;
        if (!checkSignals(globalObject))
            return false;
    } while (true);
    if (result < 0) {
        raiseOSError(globalObject, scope, errno);
        return false;
    }
    return true;
}

// The "s*" of PyArg_Parse(): the bytes of a str, in UTF-8, or of what has bytes to show. Nothing if it raised.
std::optional<ByteVector> bytesOfStringOrBuffer(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (stringIn(value))
        RELEASE_AND_RETURN(scope, encodeString(globalObject, value, "utf-8"_s, String()));
    Buffer buffer = tryBufferOf(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    ByteVector bytes;
    bytes.append(buffer.span());
    return bytes;
}

} // anonymous namespace

// fcntl(fd, cmd, arg=0, /)
PYTHON_NATIVE(fcntlFcntl)
{
    NATIVE_PROLOGUE();
    auto descriptor = toFileDescriptorOrFile(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto code = toCInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue argument = args.at(2);
    if (!audit(globalObject, "fcntl.fcntl"_s, jsNumber(*descriptor), jsNumber(*code), argument ? argument : jsUndefined()))
        return { };

    int result;
    if (!argument || isIndex(globalObject, argument)) {
        // The "I" of PyArg_Parse(): as much of it as fits
        unsigned number = 0;
        if (argument) {
            JSValue integer = toInt(globalObject, argument);
            RETURN_IF_EXCEPTION(scope, { });
            number = static_cast<unsigned>(lowBitsOfInt(integer));
        }
        if (!callUntilNotInterrupted(globalObject, result, [&] { return ::fcntl(*descriptor, *code, static_cast<int>(number)); }))
            return { };
        return JSValue::encode(jsNumber(result));
    }
    if (stringIn(argument) || hasBuffer(globalObject, argument)) {
        auto bytes = bytesOfStringOrBuffer(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, { });
        if (bytes->size() > mostCopied)
            return JSValue::encode(raiseValueError(globalObject, scope, "fcntl argument 3 is too long"_s));
        GuardedCopy copy(bytes->span());
        if (!callUntilNotInterrupted(globalObject, result, [&] { return ::fcntl(*descriptor, *code, copy.storage.data()); }))
            return { };
        if (!copy.isIntact())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "buffer overflow"_s));
        RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, copy.bytes())));
    }
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("fcntl() argument 3 must be an integer, a bytes-like object, or a string, not "_s, fullyQualifiedTypeName(globalObject, argument))));
}

// ioctl(fd, request, arg=0, mutate_flag=True, /)
PYTHON_NATIVE(fcntlIoctl)
{
    NATIVE_PROLOGUE();
    auto descriptor = toFileDescriptorOrFile(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    // unsigned_long(bitwise=True): as much of it as fits
    if (!isIndex(globalObject, args.at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("ioctl() argument 2 must be int, not "_s, fullyQualifiedTypeName(globalObject, args.at(1)))));
    JSValue codeInteger = toInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    unsigned long code = static_cast<unsigned long>(lowBitsOfInt(codeInteger));
    JSValue argument = args.at(2);
    bool mutatesArgument = true;
    if (JSValue flag = args.at(3)) {
        mutatesArgument = isTrue(globalObject, flag);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!audit(globalObject, "fcntl.ioctl"_s, jsNumber(*descriptor), intFromUInt64(globalObject, code), argument ? argument : jsUndefined()))
        return { };

    int result;
    if (!argument || isIndex(globalObject, argument)) {
        int number = 0;
        if (argument) {
            auto given = toCIntOfFormat(globalObject, argument);
            RETURN_IF_EXCEPTION(scope, { });
            number = *given;
        }
        if (!callUntilNotInterrupted(globalObject, result, [&] { return ::ioctl(*descriptor, code, number); }))
            return { };
        return JSValue::encode(jsNumber(result));
    }
    if (stringIn(argument) || hasBuffer(globalObject, argument)) {
        if (mutatesArgument && !typeOf(globalObject, argument)->hasFlag(PyType::IsBytes) && !stringIn(argument)) {
            Buffer buffer = tryBufferOf(globalObject, argument, WritableBuffer);
            if (scope.exception()) {
                // That it cannot be written to is no reason not to go on: it is copied, and what the system writes is given back.
                if (!catchException(globalObject, BuiltinType::BufferError))
                    return { };
            } else if (buffer) {
                size_t length = buffer.size();
                if (length <= mostCopied) {
                    GuardedCopy copy(buffer.span());
                    if (!callUntilNotInterrupted(globalObject, result, [&] { return ::ioctl(*descriptor, code, copy.storage.data()); }))
                        return { };
                    // What sees to a signal may have made it shorter.
                    auto target = mutableSpanOf(buffer);
                    memcpySpan(target.first(std::min(target.size(), length)), copy.bytes().first(std::min(target.size(), length)));
                    if (!copy.isIntact())
                        return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "buffer overflow"_s));
                    return JSValue::encode(jsNumber(result));
                }
                // Where it is is looked up each time, since what sees to a signal may have moved it.
                if (!callUntilNotInterrupted(globalObject, result, [&] { return ::ioctl(*descriptor, code, mutableSpanOf(buffer).data()); }))
                    return { };
                return JSValue::encode(jsNumber(result));
            }
        }
        auto bytes = bytesOfStringOrBuffer(globalObject, argument);
        RETURN_IF_EXCEPTION(scope, { });
        if (bytes->size() > mostCopied)
            return JSValue::encode(raiseValueError(globalObject, scope, "ioctl argument 3 is too long"_s));
        GuardedCopy copy(bytes->span());
        if (!callUntilNotInterrupted(globalObject, result, [&] { return ::ioctl(*descriptor, code, copy.storage.data()); }))
            return { };
        if (!copy.isIntact())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::SystemError, "buffer overflow"_s));
        RELEASE_AND_RETURN(scope, JSValue::encode(newBytes(globalObject, copy.bytes())));
    }
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("ioctl() argument 3 must be an integer, a bytes-like object, or a string, not "_s, fullyQualifiedTypeName(globalObject, argument))));
}

// flock(fd, operation, /)
PYTHON_NATIVE(fcntlFlock)
{
    NATIVE_PROLOGUE();
    auto descriptor = toFileDescriptorOrFile(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto code = toCInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (!audit(globalObject, "fcntl.flock"_s, jsNumber(*descriptor), jsNumber(*code)))
        return { };
    int result;
    if (!callUntilNotInterrupted(globalObject, result, [&] { return ::flock(*descriptor, *code); }))
        return { };
    RETURN_NONE();
}

// lockf(fd, cmd, len=0, start=0, whence=0, /)
PYTHON_NATIVE(fcntlLockf)
{
    NATIVE_PROLOGUE();
    auto descriptor = toFileDescriptorOrFile(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto code = toCInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue length = args.at(2);
    JSValue start = args.at(3);
    int whence = 0;
    if (JSValue value = args.at(4)) {
        auto given = toCInt(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        whence = *given;
    }
    if (!audit(globalObject, "fcntl.lockf"_s, jsNumber(*descriptor), jsNumber(*code), length ? length : jsUndefined(), start ? start : jsUndefined(), jsNumber(whence)))
        return { };

    struct flock lock { };
    if (*code == LOCK_UN)
        lock.l_type = F_UNLCK;
    else if (*code & LOCK_SH)
        lock.l_type = F_RDLCK;
    else if (*code & LOCK_EX)
        lock.l_type = F_WRLCK;
    else
        return JSValue::encode(raiseValueError(globalObject, scope, "unrecognized lockf argument"_s));
    if (start) {
        auto given = toCLong(globalObject, start);
        RETURN_IF_EXCEPTION(scope, { });
        lock.l_start = *given;
    }
    if (length) {
        auto given = toCLong(globalObject, length);
        RETURN_IF_EXCEPTION(scope, { });
        lock.l_len = *given;
    }
    lock.l_whence = whence;
    int result;
    if (!callUntilNotInterrupted(globalObject, result, [&] { return ::fcntl(*descriptor, *code & LOCK_NB ? F_SETLK : F_SETLKW, &lock); }))
        return { };
    RETURN_NONE();
}

JSObject* createFcntlModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "fcntl"_s);
    addFunction(globalObject, module, "fcntl"_s, fcntlFcntl);
    addFunction(globalObject, module, "ioctl"_s, fcntlIoctl);
    addFunction(globalObject, module, "flock"_s, fcntlFlock);
    addFunction(globalObject, module, "lockf"_s, fcntlLockf);
#define ADD_INT_MACRO(name) module->putDirect(vm, Identifier::fromString(vm, #name ""_s), intFromInt64(globalObject, name))
#include "PythonFcntlConstants.h"
#undef ADD_INT_MACRO
    return module;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
