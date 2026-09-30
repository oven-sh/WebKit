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
#include "PythonSocket.h"

#if OS(UNIX)

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonSignatures.h"
#include <sys/uio.h>

// The class _socket.socket, of CPython's Modules/socketmodule.c
//
// Where the bytes of something are is asked each time a system call is about to be made, and never kept. What sees to a signal runs between one try and the next, and can make a bytearray longer or shorter.

namespace JSC { namespace Python {

#define SOCKET_PROLOGUE() \
    NATIVE_PROLOGUE(); \
    Socket& self = stateOf<Socket>(args[0])

#define CONVERT(name, expression) \
    auto name##Converted = (expression); \
    RETURN_IF_EXCEPTION(scope, { }); \
    auto name = *name##Converted

// An "i" that need not be there
#define CONVERT_INT_OR(name, value, defaultValue) \
    int name = defaultValue; \
    if (JSValue name##Given = (value)) { \
        auto name##Converted = toCIntOfFormat(globalObject, name##Given); \
        RETURN_IF_EXCEPTION(scope, { }); \
        name = *name##Converted; \
    }

// ---- Being made, and shown, and let go of

PYTHON_NATIVE(socketNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Socket>()));
}

// socket(family=-1, type=-1, proto=-1, fileno=None)
PYTHON_NATIVE(socketInit)
{
    SOCKET_PROLOGUE();
    // The `int` of Argument Clinic, which is not the "i" of PyArg_ParseTuple() when it comes to what does not fit
    auto take = [&] (int& target, JSValue value) {
        if (!value)
            return true;
        auto converted = toCInt(globalObject, value);
        if (!converted)
            return false;
        target = *converted;
        return true;
    };
    int family = -1;
    int type = -1;
    int protocol = -1;
    if (!take(family, args.at(1)) || !take(type, args.at(2)) || !take(protocol, args.at(3)))
        return { };
    JSValue given = args.at(4);
    if (!audit(globalObject, "socket.__new__"_s, args[0], jsNumber(family), jsNumber(type), jsNumber(protocol)))
        return { };

    int descriptor;
    if (given && !isNone(given)) {
        CONVERT(number, toCLong(globalObject, given));
        descriptor = static_cast<int>(number);
        if (descriptor < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, "negative file descriptor"_s));
        // That it is open, and is a socket
        SocketAddress address;
        socklen_t length = sizeof(address);
        zeroBytes(address);
        if (!getsockname(descriptor, &address.sa, &length)) {
            if (family == -1)
                family = address.sa.sa_family;
        } else if (family == -1 || errno == EBADF || errno == ENOTSOCK)
            return JSValue::encode(raiseSocketError(globalObject, scope));
        if (type == -1) {
            int found;
            socklen_t size = sizeof(found);
            if (getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &found, &size))
                return JSValue::encode(raiseSocketError(globalObject, scope));
            type = found;
        }
#ifdef SO_PROTOCOL
        if (protocol == -1) {
            int found;
            socklen_t size = sizeof(found);
            if (getsockopt(descriptor, SOL_SOCKET, SO_PROTOCOL, &found, &size))
                return JSValue::encode(raiseSocketError(globalObject, scope));
            protocol = found;
        }
#else
        protocol = 0;
#endif
    } else {
        if (family == -1)
            family = AF_INET;
        if (type == -1)
            type = SOCK_STREAM;
        if (protocol == -1)
            protocol = 0;
#ifdef SOCK_CLOEXEC
        descriptor = ::socket(family, type | SOCK_CLOEXEC, protocol);
#else
        descriptor = ::socket(family, type, protocol);
#endif
        if (descriptor == -1)
            return JSValue::encode(raiseSocketError(globalObject, scope));
#ifndef SOCK_CLOEXEC
        if (!setNotInheritable(globalObject, descriptor)) {
            ::close(descriptor);
            return { };
        }
#endif
    }
    if (!initializeSocket(globalObject, self, descriptor, family, type, protocol)) {
        ::close(descriptor);
        return { };
    }
    RETURN_NONE();
}

PYTHON_NATIVE(socketRepr)
{
    SOCKET_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsString(vm, concatenate("<socket object, fd="_s, self.descriptor, ", family="_s, self.family, ", type="_s, self.type, ", proto="_s, self.protocol, '>')));
}

// sock_finalize(). It is there to be called, and nothing calls it: see the README.
PYTHON_NATIVE(socketDel)
{
    SOCKET_PROLOGUE();
    Exception* raised = takeRaisedException(vm);
    if (self.descriptor != -1) {
        String shown = repr(globalObject, args[0]);
        if (!scope.exception())
            warn(globalObject, BuiltinType::ResourceWarning, concatenate("unclosed "_s, shown), 1, args[0]);
        if (Exception* thrown = scope.exception()) {
            if (vm.isTerminationException(thrown))
                return { };
            // A program can have asked for warnings to be raised.
            if (isInstance(globalObject, thrown->value(), realm->type(BuiltinType::Warning)))
                reportUnraisableShowing(globalObject, "Exception ignored while finalizing socket"_s, args[0]);
            if (scope.exception() && !scope.tryClearException())
                return { };
        }
        // Only now, so that what is told of the warning can still ask it things.
        int descriptor = std::exchange(self.descriptor, -1);
        ::close(descriptor);
    }
    restoreRaisedException(globalObject, raised);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(socketClose)
{
    SOCKET_PROLOGUE();
    int descriptor = std::exchange(self.descriptor, -1);
    // It is not tried again if it is interrupted. That the other end has gone already is nothing to complain of.
    if (descriptor != -1 && ::close(descriptor) < 0 && errno != ECONNRESET)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(socketDetach)
{
    SOCKET_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsNumber(std::exchange(self.descriptor, -1)));
}

PYTHON_NATIVE(socketFileno)
{
    SOCKET_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsNumber(self.descriptor));
}

// ---- Whether it waits

PYTHON_NATIVE(socketSetBlocking)
{
    SOCKET_PROLOGUE();
    bool isBlocking = isTrue(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    self.timeout = isBlocking ? -nanosecondsPerSecond : 0;
    if (!setBlocking(globalObject, self, isBlocking))
        return { };
    RETURN_NONE();
}

PYTHON_NATIVE(socketGetBlocking)
{
    SOCKET_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsBoolean(!!self.timeout));
}

PYTHON_NATIVE(socketSetTimeout)
{
    SOCKET_PROLOGUE();
    CONVERT(timeout, parseSocketTimeout(globalObject, args[1]));
    self.timeout = timeout;
    // With a time to wait no longer than, the descriptor does not wait, and what waits is here.
    if (!setBlocking(globalObject, self, timeout < 0))
        return { };
    RETURN_NONE();
}

static JSValue timeoutOf(Socket& self) { return self.timeout < 0 ? jsUndefined() : floatFromDouble(timeAsSeconds(self.timeout)); }

PYTHON_NATIVE(socketGetTimeout)
{
    SOCKET_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(timeoutOf(self));
}

// ---- Options

// setsockopt(level, option, value: int), setsockopt(level, option, value: buffer) and setsockopt(level, option, None, optlen: int)
PYTHON_NATIVE(socketSetOption)
{
    SOCKET_PROLOGUE();
    unsigned given = args.size() - 1;
#ifdef AF_VSOCK
    // "iiK:setsockopt". What is set is of 64 bits.
    if (self.family == AF_VSOCK) {
        if (given != 3)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("setsockopt() takes exactly 3 arguments ("_s, given, " given)"_s)));
        CONVERT(level, toCIntOfFormat(globalObject, args[1]));
        CONVERT(option, toCIntOfFormat(globalObject, args[2]));
        if (!typeOf(globalObject, args[3])->lookup(vm, vm.pythonNames().dunder_index))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("setsockopt() argument 3 must be int, not "_s, typeNameOfArgument(globalObject, args[3]))));
        JSValue integer = toInt(globalObject, args[3]);
        RETURN_IF_EXCEPTION(scope, { });
        uint64_t flag = lowBitsOfInt(integer);
        if (setsockopt(self.descriptor, level, option, &flag, sizeof(flag)) < 0)
            return JSValue::encode(raiseSocketError(globalObject, scope));
        RETURN_NONE();
    }
#endif
    // Each way of calling it is tried in turn, and what is wrong with the last is what is said.
    int level = 0;
    int option = 0;
    auto takeFirstTwo = [&] {
        auto first = toCIntOfFormat(globalObject, args[1]);
        if (!first)
            return false;
        auto second = toCIntOfFormat(globalObject, args[2]);
        if (!second)
            return false;
        level = *first;
        option = *second;
        return true;
    };
    std::optional<int> result;
    if (given == 3 && takeFirstTwo()) {
        if (auto flag = toCIntOfFormat(globalObject, args[3]))
            result = setsockopt(self.descriptor, level, option, &*flag, sizeof(int));
    }
    if (!result) {
        if (scope.exception() && !scope.tryClearException())
            return { };
        if (given == 4 && takeFirstTwo() && isNone(args[3])) {
            if (auto length = toCUnsignedIntOfFormat(globalObject, args[4]))
                result = setsockopt(self.descriptor, level, option, nullptr, static_cast<socklen_t>(*length));
        }
    }
    if (!result) {
        if (scope.exception() && !scope.tryClearException())
            return { };
        if (given != 3)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("setsockopt() takes exactly 3 arguments ("_s, given, " given)"_s)));
        if (!takeFirstTwo())
            return { };
        Buffer value = bufferOf(globalObject, args[3]);
        RETURN_IF_EXCEPTION(scope, { });
        auto span = value.span();
        result = setsockopt(self.descriptor, level, option, span.data(), span.size());
    }
    if (*result < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RETURN_NONE();
}

// getsockopt(level, option[, buffersize])
PYTHON_NATIVE(socketGetOption)
{
    SOCKET_PROLOGUE();
    CONVERT(level, toCIntOfFormat(globalObject, args[1]));
    CONVERT(option, toCIntOfFormat(globalObject, args[2]));
    CONVERT_INT_OR(givenLength, args.at(3), 0);
    socklen_t length = givenLength;
#ifdef AF_VSOCK
    if (self.family == AF_VSOCK) {
        if (length)
            return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "getsockopt string buffer not allowed"_s));
        uint64_t flag = 0;
        socklen_t size = sizeof(flag);
        if (getsockopt(self.descriptor, level, option, &flag, &size) < 0)
            return JSValue::encode(raiseSocketError(globalObject, scope));
        return JSValue::encode(intFromUInt64(globalObject, flag));
    }
#endif
    if (!length) {
        int flag = 0;
        socklen_t size = sizeof(flag);
        if (getsockopt(self.descriptor, level, option, &flag, &size) < 0)
            return JSValue::encode(raiseSocketError(globalObject, scope));
        return JSValue::encode(jsNumber(flag));
    }
    if (length > 1024)
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "getsockopt buflen out of range"_s));
    std::array<uint8_t, 1024> buffer;
    if (getsockopt(self.descriptor, level, option, buffer.data(), &length) < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    return JSValue::encode(newBytes(globalObject, std::span(buffer).first(length)));
}

// ---- Connections

PYTHON_NATIVE(socketBind)
{
    SOCKET_PROLOGUE();
    SocketAddress address;
    int length;
    if (!toSocketAddress(globalObject, self, args[1], address, length, "bind"_s))
        return { };
    if (!audit(globalObject, "socket.bind"_s, args[0], args[1]))
        return { };
    if (::bind(self.descriptor, &address.sa, length) < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RETURN_NONE();
}

// listen([backlog])
PYTHON_NATIVE(socketListen)
{
    SOCKET_PROLOGUE();
    // Enough that connections are not dropped in the ordinary way of things, and no more than that
    CONVERT_INT_OR(backlog, args.at(1), std::min(SOMAXCONN, 128));
    if (::listen(self.descriptor, std::max(backlog, 0)) < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RETURN_NONE();
}

// _accept() -> (fd, address)
PYTHON_NATIVE(socketAccept)
{
    SOCKET_PROLOGUE();
    SocketAddress address;
    socklen_t length;
    if (!socketAddressLength(globalObject, self, length))
        return { };
    zeroBytes(address);
    int accepted = -1;
    auto accept = [&] {
#if defined(SOCK_CLOEXEC) && OS(LINUX)
#ifdef HAVE_SOCKADDR_ALG
        // One of these cannot say who it is from, and if it is asked the kernel says that the connection was given up.
        if (self.family == AF_ALG) {
            length = 0;
            accepted = ::accept4(self.descriptor, nullptr, nullptr, SOCK_CLOEXEC);
            return accepted >= 0;
        }
#endif
        accepted = ::accept4(self.descriptor, &address.sa, &length, SOCK_CLOEXEC);
#else
        accepted = ::accept(self.descriptor, &address.sa, &length);
#endif
        return accepted >= 0;
    };
    if (!callSocket(globalObject, self, WaitingTo::Read, accept))
        return { };
#if !(defined(SOCK_CLOEXEC) && OS(LINUX))
    if (!setNotInheritable(globalObject, accepted)) {
        ::close(accepted);
        return { };
    }
#endif
    JSValue from = makeSocketAddress(globalObject, self.descriptor, &address.sa, length, self.protocol);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { jsNumber(accepted), from }));
}

// internal_connect(). If it is to raise, it is nothing that it gives when it has, and zero when all is well. If it is not, what goes wrong with connecting is what it gives, and it is nothing only if what saw to a signal raised.
static std::optional<int> connectSocket(JSGlobalObject* globalObject, Socket& self, const SocketAddress& address, int length, bool raises)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!::connect(self.descriptor, &address.sa, length))
        return 0;

    // What sees to a signal can change errno.
    int error = errno;
    bool waits;
    if (error == EINTR) {
        if (!checkSignals(globalObject))
            return std::nullopt;
        // It goes on connecting by itself. One that waits, waits for that. For one that does not, it is for whoever called this to.
        waits = !!self.timeout;
    } else
        waits = self.timeout > 0 && error == EINPROGRESS;

    if (!waits) {
        if (!raises)
            return error;
        errno = error;
        raiseSocketError(globalObject, scope);
        return std::nullopt;
    }

    // sock_connect_impl()
    auto hasConnected = [&] {
        int pending;
        socklen_t size = sizeof(pending);
        if (getsockopt(self.descriptor, SOL_SOCKET, SO_ERROR, &pending, &size))
            return false;
        if (pending == EISCONN || !pending)
            return true;
        errno = pending;
        return false;
    };
    if (raises) {
        if (!callSocket(globalObject, self, WaitingTo::Write, hasConnected, true, nullptr, self.timeout))
            return std::nullopt;
        return 0;
    }
    if (!callSocket(globalObject, self, WaitingTo::Write, hasConnected, true, &error, self.timeout)) {
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        return error;
    }
    return 0;
}

// connect(address) and connect_ex(address)
PYTHON_NATIVE(socketConnect)
{
    SOCKET_PROLOGUE();
    bool raises = unpack<bool>(callFrame, 0);
    SocketAddress address;
    int length;
    if (!toSocketAddress(globalObject, self, args[1], address, length, raises ? "connect"_s : "connect_ex"_s))
        return { };
    if (!audit(globalObject, "socket.connect"_s, args[0], args[1]))
        return { };
    auto result = connectSocket(globalObject, self, address, length, raises);
    RETURN_IF_EXCEPTION(scope, { });
    if (raises)
        RETURN_NONE();
    return JSValue::encode(jsNumber(*result));
}

// getsockname() and getpeername()
PYTHON_NATIVE(socketGetName)
{
    SOCKET_PROLOGUE();
    bool isPeer = unpack<bool>(callFrame, 0);
    SocketAddress address;
    socklen_t length;
    if (!socketAddressLength(globalObject, self, length))
        return { };
    zeroBytes(address);
    if ((isPeer ? getpeername(self.descriptor, &address.sa, &length) : getsockname(self.descriptor, &address.sa, &length)) < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(makeSocketAddress(globalObject, self.descriptor, &address.sa, length, self.protocol)));
}

PYTHON_NATIVE(socketShutdown)
{
    SOCKET_PROLOGUE();
    CONVERT(how, toCInt(globalObject, args[1]));
    if (::shutdown(self.descriptor, how) < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RETURN_NONE();
}

// ---- Receiving

// sock_recv_guts() and sock_recvfrom_guts(). `where` gives where to put what comes, each time it is asked. How much came, or nothing if it raised. If `from` is not null it is told who sent it.
static std::optional<ssize_t> receive(JSGlobalObject* globalObject, Socket& self, const ScopedLambda<std::span<uint8_t>()>& where, int flags, JSValue* from)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ssize_t result = 0;
    if (!from) {
        // If none were asked for there is nothing to do.
        if (where().empty())
            return 0;
        auto call = [&] {
            auto span = where();
            result = ::recv(self.descriptor, span.data(), span.size(), flags);
            return result >= 0;
        };
        if (!callSocket(globalObject, self, WaitingTo::Read, call))
            return std::nullopt;
        return result;
    }
    SocketAddress address;
    socklen_t length;
    if (!socketAddressLength(globalObject, self, length))
        return std::nullopt;
    socklen_t fullLength = length;
    auto call = [&] {
        auto span = where();
        zeroBytes(address);
        length = fullLength;
        result = ::recvfrom(self.descriptor, span.data(), span.size(), flags, &address.sa, &length);
        return result >= 0;
    };
    if (!callSocket(globalObject, self, WaitingTo::Read, call))
        return std::nullopt;
    *from = makeSocketAddress(globalObject, self.descriptor, &address.sa, length, self.protocol);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return result;
}

// recv(buffersize[, flags]) and recvfrom(buffersize[, flags])
PYTHON_NATIVE(socketReceive)
{
    SOCKET_PROLOGUE();
    bool tellsWhoFrom = unpack<bool>(callFrame, 0);
    CONVERT(size, toSsize(globalObject, args[1]));
    CONVERT_INT_OR(flags, args.at(2), 0);
    if (size < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, tellsWhoFrom ? "negative buffersize in recvfrom"_s : "negative buffersize in recv"_s));
    ByteVector bytes;
    bytes.appendFill(0, size);
    if (bytes.hasOverflowed())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    JSValue from;
    auto received = receive(globalObject, self, [&] { return bytes.mutableSpan(); }, flags, tellsWhoFrom ? &from : nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue data = newBytes(globalObject, bytes.span().first(*received));
    if (!tellsWhoFrom)
        return JSValue::encode(data);
    return JSValue::encode(PyTuple::create(globalObject, { data, from }));
}

// recv_into(buffer[, nbytes[, flags]]) and recvfrom_into(buffer[, nbytes[, flags]])
PYTHON_NATIVE(socketReceiveInto)
{
    SOCKET_PROLOGUE();
    bool tellsWhoFrom = unpack<bool>(callFrame, 0);
    Buffer buffer = writableBufferArgument(globalObject, args.at(1), tellsWhoFrom ? "recvfrom_into"_s : "recv_into"_s, "argument 1"_s);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t size = 0;
    if (JSValue given = args.at(2)) {
        CONVERT(converted, toSsize(globalObject, given));
        size = converted;
    }
    CONVERT_INT_OR(flags, args.at(3), 0);
    int64_t available = buffer.size();
    if (size < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, tellsWhoFrom ? "negative buffersize in recvfrom_into"_s : "negative buffersize in recv_into"_s));
    // If it was not said how many, as many as there is room for
    if (!size)
        size = available;
    else if (size > available)
        return JSValue::encode(raiseValueError(globalObject, scope, tellsWhoFrom ? "nbytes is greater than the length of the buffer"_s : "buffer too small for requested bytes"_s));
    JSValue from;
    auto where = [&] {
        auto span = mutableSpanOf(buffer);
        return span.first(std::min<size_t>(span.size(), size));
    };
    auto received = receive(globalObject, self, where, flags, tellsWhoFrom ? &from : nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    if (!tellsWhoFrom)
        return JSValue::encode(intFromInt64(globalObject, *received));
    return JSValue::encode(PyTuple::create(globalObject, { intFromInt64(globalObject, *received), from }));
}

// cmsg_min_space(): whether the length of the control buffer makes sense, `header` is in it with `space` bytes after where it begins, and its cmsg_len is inside too
static bool hasControlSpace(const struct msghdr& message, const struct cmsghdr* header, size_t space)
{
    constexpr size_t endOfLength = offsetof(struct cmsghdr, cmsg_len) + sizeof(header->cmsg_len);
    if (!header || !message.msg_control)
        return false;
    space = std::max(space, endOfLength);
    size_t offset = std::bit_cast<const char*>(header) - static_cast<const char*>(message.msg_control);
    return offset <= std::numeric_limits<size_t>::max() - space && offset + space <= static_cast<size_t>(message.msg_controllen);
}

// get_cmsg_data_space(): how many bytes of the control buffer there are from where the data of `header` begins. Nothing if that is not in it.
static std::optional<size_t> controlDataSpace(const struct msghdr& message, struct cmsghdr* header)
{
    // An array, in glibc
    auto* data = std::bit_cast<const char*>(static_cast<const unsigned char*>(CMSG_DATA(header)));
    if (!data)
        return std::nullopt;
    size_t offset = data - static_cast<const char*>(message.msg_control);
    if (offset > static_cast<size_t>(message.msg_controllen))
        return std::nullopt;
    return message.msg_controllen - offset;
}

enum class ControlData : int8_t { Invalid = -1, Whole = 0, Truncated = 1 };

// get_cmsg_data_len()
static ControlData controlDataLength(const struct msghdr& message, struct cmsghdr* header, size_t& length)
{
    if (!hasControlSpace(message, header, CMSG_LEN(0)) || header->cmsg_len < CMSG_LEN(0))
        return ControlData::Invalid;
    size_t said = header->cmsg_len - CMSG_LEN(0);
    auto space = controlDataSpace(message, header);
    if (!space)
        return ControlData::Invalid;
    if (*space >= said) {
        length = said;
        return ControlData::Whole;
    }
    length = *space;
    return ControlData::Truncated;
}

// sock_recvmsg_guts(). `where` fills in where to put what comes, each time it is asked, and `makeValue` makes the first of the four things that are given back, of how much came. Empty if it raised.
static JSValue receiveMessage(JSGlobalObject* globalObject, Socket& self, const ScopedLambda<void(Vector<struct iovec>&)>& where, int flags, int64_t controlLength, const ScopedLambda<JSValue(ssize_t)>& makeValue)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    SocketAddress address;
    socklen_t addressLength;
    if (!socketAddressLength(globalObject, self, addressLength))
        return { };
    // So that it is not taken for an address if nothing is put there
    zeroBytes(address);
    address.sa.sa_family = AF_UNSPEC;
    if (controlLength < 0 || static_cast<size_t>(controlLength) > socketLengthLimit)
        return raiseValueError(globalObject, scope, "invalid ancillary data buffer length"_s);
    ByteVector control;
    control.appendFill(0, controlLength);
    if (control.hasOverflowed())
        return raiseMemoryError(globalObject, scope);

    struct msghdr message;
    Vector<struct iovec> vectors;
    ssize_t received = 0;
    auto call = [&] {
        vectors.shrink(0);
        where(vectors);
        zeroBytes(message);
        message.msg_name = &address.sa;
        message.msg_namelen = addressLength;
        message.msg_iov = vectors.mutableSpan().data();
        message.msg_iovlen = vectors.size();
        message.msg_control = controlLength ? control.mutableSpan().data() : nullptr;
        message.msg_controllen = controlLength;
        received = ::recvmsg(self.descriptor, &message, flags);
        return received >= 0;
    };
    if (!callSocket(globalObject, self, WaitingTo::Read, call))
        return { };

    auto first = [&] { return message.msg_controllen > 0 ? CMSG_FIRSTHDR(&message) : nullptr; };
    // What was sent along that is open files is closed, so that they are not left open with nobody knowing of them.
    auto closeDescriptors = [&] {
        for (auto* header = first(); header; header = CMSG_NXTHDR(&message, header)) {
            size_t length = 0;
            auto status = controlDataLength(message, header, length);
            if (status == ControlData::Invalid)
                break;
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
                auto* data = CMSG_DATA(header);
                for (size_t i = 0; i + sizeof(int) <= length; i += sizeof(int)) {
                    int descriptor;
                    memcpy(&descriptor, data + i, sizeof(int));
                    ::close(descriptor);
                }
            }
            if (status != ControlData::Whole)
                break;
        }
    };
#define CLOSE_AND_RETURN_IF_EXCEPTION() \
    if (scope.exception()) [[unlikely]] { \
        closeDescriptors(); \
        return { }; \
    }

    JSArray* items = newList(globalObject);
    for (auto* header = first(); header; header = CMSG_NXTHDR(&message, header)) {
        size_t length = 0;
        auto status = controlDataLength(message, header, length);
        if (status != ControlData::Whole) {
            warn(globalObject, BuiltinType::RuntimeWarning, "received malformed or improperly-truncated ancillary data"_s, 1);
            CLOSE_AND_RETURN_IF_EXCEPTION();
        }
        if (status == ControlData::Invalid)
            break;
        JSValue data = newBytes(globalObject, std::span(static_cast<const uint8_t*>(CMSG_DATA(header)), length));
        listAppend(globalObject, items, PyTuple::create(globalObject, { jsNumber(header->cmsg_level), jsNumber(header->cmsg_type), data }));
        CLOSE_AND_RETURN_IF_EXCEPTION();
        if (status != ControlData::Whole)
            break;
    }
    JSValue value = makeValue(received);
    CLOSE_AND_RETURN_IF_EXCEPTION();
    JSValue from = makeSocketAddress(globalObject, self.descriptor, &address.sa, std::min(message.msg_namelen, addressLength), self.protocol);
    CLOSE_AND_RETURN_IF_EXCEPTION();
#undef CLOSE_AND_RETURN_IF_EXCEPTION
    return PyTuple::create(globalObject, { value, items, jsNumber(message.msg_flags), from });
}

// recvmsg(bufsize[, ancbufsize[, flags]])
PYTHON_NATIVE(socketReceiveMessage)
{
    SOCKET_PROLOGUE();
    CONVERT(size, toSsize(globalObject, args[1]));
    int64_t controlLength = 0;
    if (JSValue given = args.at(2)) {
        CONVERT(converted, toSsize(globalObject, given));
        controlLength = converted;
    }
    CONVERT_INT_OR(flags, args.at(3), 0);
    if (size < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "negative buffer size in recvmsg()"_s));
    ByteVector bytes;
    bytes.appendFill(0, size);
    if (bytes.hasOverflowed())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    auto where = [&] (Vector<struct iovec>& vectors) { vectors.append({ bytes.mutableSpan().data(), bytes.size() }); };
    auto makeValue = [&] (ssize_t received) -> JSValue { return newBytes(globalObject, bytes.span().first(std::min<size_t>(received, bytes.size()))); };
    RELEASE_AND_RETURN(scope, JSValue::encode(receiveMessage(globalObject, self, where, flags, controlLength, makeValue)));
}

// recvmsg_into(buffers[, ancbufsize[, flags]])
PYTHON_NATIVE(socketReceiveMessageInto)
{
    SOCKET_PROLOGUE();
    int64_t controlLength = 0;
    if (JSValue given = args.at(2)) {
        CONVERT(converted, toSsize(globalObject, given));
        controlLength = converted;
    }
    CONVERT_INT_OR(flags, args.at(3), 0);
    PyTuple* given = tupleFromIterable(globalObject, args[1]);
    if (scope.exception()) [[unlikely]] {
        if (!scope.tryClearException())
            return { };
        return JSValue::encode(raiseTypeError(globalObject, scope, "recvmsg_into() argument 1 must be an iterable"_s));
    }
    Buffers buffers(globalObject);
    for (unsigned i = 0; i < given->length(); ++i) {
        Buffer buffer = tryBufferOf(globalObject, given->at(i), WritableBuffer);
        if (scope.exception() && !scope.tryClearException())
            return { };
        if (!buffer)
            return JSValue::encode(raiseTypeError(globalObject, scope, "recvmsg_into() argument 1 must be an iterable of single-segment read-write buffers"_s));
        buffers.append(WTF::move(buffer));
    }
    auto where = [&] (Vector<struct iovec>& vectors) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            auto span = buffers.at(i);
            vectors.append({ const_cast<uint8_t*>(span.data()), span.size() });
        }
    };
    auto makeValue = [&] (ssize_t received) { return intFromInt64(globalObject, received); };
    RELEASE_AND_RETURN(scope, JSValue::encode(receiveMessage(globalObject, self, where, flags, controlLength, makeValue)));
}

// ---- Sending

// send(data[, flags])
PYTHON_NATIVE(socketSend)
{
    SOCKET_PROLOGUE();
    Buffer buffer = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    CONVERT_INT_OR(flags, args.at(2), 0);
    ssize_t sent = 0;
    auto call = [&] {
        auto span = buffer.span();
        sent = ::send(self.descriptor, span.data(), span.size(), flags);
        return sent >= 0;
    };
    if (!callSocket(globalObject, self, WaitingTo::Write, call))
        return { };
    return JSValue::encode(intFromInt64(globalObject, sent));
}

// sendall(data[, flags])
PYTHON_NATIVE(socketSendAll)
{
    SOCKET_PROLOGUE();
    bool hasTimeout = self.timeout > 0;
    int64_t timeout = self.timeout;
    std::optional<int64_t> deadline;
    Buffer buffer = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    CONVERT_INT_OR(flags, args.at(2), 0);
    size_t length = buffer.size();
    size_t offset = 0;
    do {
        if (hasTimeout) {
            if (deadline)
                timeout = timeUntil(*deadline);
            else
                deadline = deadlineAfter(timeout);
            if (timeout <= 0)
                return JSValue::encode(raise(globalObject, scope, BuiltinType::TimeoutError, "timed out"_s));
        }
        ssize_t sent = 0;
        auto call = [&] {
            // No more of it than there is now
            auto span = buffer.span();
            size_t end = std::min(length, span.size());
            size_t start = std::min(offset, end);
            sent = ::send(self.descriptor, span.data() + start, end - start, flags);
            return sent >= 0;
        };
        if (!callSocket(globalObject, self, WaitingTo::Write, call, false, nullptr, timeout))
            return { };
        offset += sent;
        // Being interrupted can make it send some and say that all is well, so this is not only for when it says that it was.
        if (!checkSignals(globalObject))
            return { };
        length = std::min(length, buffer.size());
    } while (offset < length);
    RETURN_NONE();
}

// sendto(data, [flags,] address)
PYTHON_NATIVE(socketSendTo)
{
    SOCKET_PROLOGUE();
    unsigned given = args.size() - 1;
    if (given != 2 && given != 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("sendto() takes 2 or 3 arguments ("_s, given, " given)"_s)));
    Buffer buffer = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    CONVERT_INT_OR(flags, given == 3 ? args[2] : JSValue(), 0);
    JSValue to = args[given];
    SocketAddress address;
    int length;
    if (!toSocketAddress(globalObject, self, to, address, length, "sendto"_s))
        return { };
    if (!audit(globalObject, "socket.sendto"_s, args[0], to))
        return { };
    ssize_t sent = 0;
    auto call = [&] {
        auto span = buffer.span();
        sent = ::sendto(self.descriptor, span.data(), span.size(), flags, &address.sa, length);
        return sent >= 0;
    };
    if (!callSocket(globalObject, self, WaitingTo::Write, call))
        return { };
    return JSValue::encode(intFromInt64(globalObject, sent));
}

// sendmsg(buffers[, ancdata[, flags[, address]]])
PYTHON_NATIVE(socketSendMessage)
{
    SOCKET_PROLOGUE();
    JSValue givenControl = args.at(2);
    CONVERT_INT_OR(flags, args.at(3), 0);
    JSValue to = args.at(4);

    SocketAddress address;
    int addressLength = 0;
    bool hasAddress = to && !isNone(to);
    if (hasAddress && !toSocketAddress(globalObject, self, to, address, addressLength, "sendmsg"_s))
        return { };
    if (!audit(globalObject, "socket.sendmsg"_s, args[0], hasAddress ? to : jsUndefined()))
        return { };

    // sock_sendmsg_iovec()
    PyTuple* parts = tupleFromIterable(globalObject, args[1]);
    if (scope.exception()) [[unlikely]] {
        if (!scope.tryClearException())
            return { };
        return JSValue::encode(raiseTypeError(globalObject, scope, "sendmsg() argument 1 must be an iterable"_s));
    }
    Buffers buffers(globalObject);
    for (unsigned i = 0; i < parts->length(); ++i) {
        // What is raised in getting at the bytes stands. It is only if nothing is that something else is said.
        Buffer buffer = bufferOf(globalObject, parts->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        buffers.append(WTF::move(buffer));
    }

    struct ControlItem {
        int level;
        int type;
    };
    Vector<ControlItem> controlItems;
    Buffers controlBuffers(globalObject);
    size_t controlLength = 0;
    if (givenControl) {
        PyTuple* items = tupleFromIterable(globalObject, givenControl);
        if (scope.exception()) [[unlikely]] {
            if (!scope.tryClearException())
                return { };
            return JSValue::encode(raiseTypeError(globalObject, scope, "sendmsg() argument 2 must be an iterable"_s));
        }
        for (unsigned i = 0; i < items->length(); ++i) {
            // "(iiy*):[sendmsg() ancillary data items]", by converttuple() of CPython's Python/getargs.c. Nothing in it has to be kept alive by what it is in, so any sequence will do that is not text or bytes.
            JSValue item = items->at(i);
            constexpr auto prefix = "[sendmsg() ancillary data items]() argument must be "_s;
            if (!isTuple(item)) {
                if (!isSequence(globalObject, item) || stringIn(item) || isBytes(item) || isByteArray(item))
                    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(prefix, "3-item tuple, not "_s, typeNameOfArgument(globalObject, item))));
                auto count = length(globalObject, item);
                RETURN_IF_EXCEPTION(scope, { });
                if (count != 3)
                    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(prefix, "sequence of length 3, not "_s, count)));
                item = tupleFromIterable(globalObject, item);
                RETURN_IF_EXCEPTION(scope, { });
            }
            PyTuple* fields = asTuple(item);
            if (fields->length() != 3)
                return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(prefix, "tuple of length 3, not "_s, fields->length())));
            CONVERT(level, toCIntOfFormat(globalObject, fields->at(0)));
            CONVERT(type, toCIntOfFormat(globalObject, fields->at(1)));
            Buffer data = bufferOf(globalObject, fields->at(2));
            RETURN_IF_EXCEPTION(scope, { });
            auto space = controlMessageSpace(data.size());
            controlItems.append({ level, type });
            controlBuffers.append(WTF::move(data));
            if (!space)
                return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "ancillary data item too large"_s));
            controlLength += *space;
            if (controlLength > socketLengthLimit)
                return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "too much ancillary data"_s));
        }
    }

    struct msghdr message;
    zeroBytes(message);
    ByteVector control;
    if (!controlItems.isEmpty()) {
        // Zeros, since one C library looks at the length of the next header before there is one.
        control.appendFill(0, controlLength);
        if (control.hasOverflowed())
            return JSValue::encode(raiseMemoryError(globalObject, scope));
        message.msg_control = control.mutableSpan().data();
        message.msg_controllen = controlLength;
        struct cmsghdr* header = nullptr;
        for (size_t i = 0; i < controlItems.size(); ++i) {
            auto data = controlBuffers.at(i);
            header = !i ? CMSG_FIRSTHDR(&message) : CMSG_NXTHDR(&message, header);
            if (!header)
                return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("unexpected NULL result from "_s, !i ? "CMSG_FIRSTHDR"_s : "CMSG_NXTHDR"_s, "()"_s)));
            auto itemLength = controlMessageLength(data.size());
            if (!itemLength)
                return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "item size out of range for CMSG_LEN()"_s));
            bool hasRoom = false;
            if (hasControlSpace(message, header, *itemLength)) {
                header->cmsg_len = *itemLength;
                if (auto space = controlDataSpace(message, header))
                    hasRoom = *space >= data.size();
            }
            if (!hasRoom)
                return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "ancillary data does not fit in calculated space"_s));
            header->cmsg_level = controlItems[i].level;
            header->cmsg_type = controlItems[i].type;
            memcpy(CMSG_DATA(header), data.data(), data.size());
        }
    }
    if (hasAddress) {
        message.msg_name = &address;
        message.msg_namelen = addressLength;
    }

    Vector<struct iovec> vectors;
    ssize_t sent = 0;
    auto call = [&] {
        vectors.shrink(0);
        for (size_t i = 0; i < buffers.size(); ++i) {
            auto span = buffers.at(i);
            vectors.append({ const_cast<uint8_t*>(span.data()), span.size() });
        }
        message.msg_iov = vectors.mutableSpan().data();
        message.msg_iovlen = vectors.size();
        sent = ::sendmsg(self.descriptor, &message, flags);
        return sent >= 0;
    };
    if (!callSocket(globalObject, self, WaitingTo::Write, call))
        return { };
    return JSValue::encode(intFromInt64(globalObject, sent));
}

#ifdef HAVE_SOCKADDR_ALG
// sendmsg_afalg([msg], *, op[, iv[, assoclen[, flags]]])
PYTHON_NATIVE(socketSendMessageToAlgorithm)
{
    SOCKET_PROLOGUE();
    if (self.family != AF_ALG)
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "algset is only supported for AF_ALG"_s));
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    // "|O$O!y*O!i:sendmsg_afalg"
    auto mustBeInt = [&] (JSValue value, unsigned position) {
        if (!value || isInstance(globalObject, value, realm->typeInt()))
            return true;
        raiseTypeError(globalObject, scope, concatenate("sendmsg_afalg() argument "_s, position, " must be int, not "_s, typeNameOfArgument(globalObject, value)));
        return false;
    };
    JSValue givenParts = args.at(1);
    JSValue givenOperation = args.at(2);
    if (!mustBeInt(givenOperation, 2))
        return { };
    Buffer vector;
    if (JSValue given = args.at(3)) {
        vector = bufferOf(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue givenAssociatedLength = args.at(4);
    if (!mustBeInt(givenAssociatedLength, 4))
        return { };
    CONVERT_INT_OR(flags, args.at(5), 0);

    int operation = -1;
    if (givenOperation) {
        if (auto converted = toCInt(globalObject, givenOperation))
            operation = *converted;
        else if (!scope.tryClearException())
            return { };
    }
    if (operation < 0)
        return JSValue::encode(raiseTypeError(globalObject, scope, "Invalid or missing argument 'op'"_s));
    int associatedLength = -1;
    if (givenAssociatedLength) {
        CONVERT(converted, toCInt(globalObject, givenAssociatedLength));
        if (converted < 0)
            return JSValue::encode(raiseTypeError(globalObject, scope, "assoclen must be positive"_s));
        associatedLength = converted;
    }

    size_t controlLength = CMSG_SPACE(4);
    if (vector)
        controlLength += CMSG_SPACE(sizeof(struct af_alg_iv) + vector.size());
    if (associatedLength >= 0)
        controlLength += CMSG_SPACE(4);
    Vector<uint8_t> control;
    if (!control.tryGrow(controlLength))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    zeroSpan(control.mutableSpan());
    struct msghdr message;
    zeroBytes(message);
    message.msg_control = control.mutableSpan().data();
    message.msg_controllen = controlLength;

    // sock_sendmsg_iovec()
    Buffers buffers(globalObject);
    if (givenParts) {
        PyTuple* parts = tupleFromIterable(globalObject, givenParts);
        if (scope.exception()) [[unlikely]] {
            if (!scope.tryClearException())
                return { };
            return JSValue::encode(raiseTypeError(globalObject, scope, "sendmsg() argument 1 must be an iterable"_s));
        }
        for (unsigned i = 0; i < parts->length(); ++i) {
            Buffer buffer = bufferOf(globalObject, parts->at(i));
            RETURN_IF_EXCEPTION(scope, { });
            buffers.append(WTF::move(buffer));
        }
    }

    auto put = [] (struct cmsghdr* header, int type, unsigned value) {
        header->cmsg_level = SOL_ALG;
        header->cmsg_type = type;
        header->cmsg_len = CMSG_LEN(4);
        memcpy(CMSG_DATA(header), &value, sizeof(value));
    };
    struct cmsghdr* header = CMSG_FIRSTHDR(&message);
    if (!header)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "unexpected NULL result from CMSG_FIRSTHDR"_s));
    put(header, ALG_SET_OP, static_cast<unsigned>(operation));
    if (vector) {
        header = CMSG_NXTHDR(&message, header);
        if (!header)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "unexpected NULL result from CMSG_NXTHDR(iv)"_s));
        header->cmsg_level = SOL_ALG;
        header->cmsg_type = ALG_SET_IV;
        header->cmsg_len = CMSG_SPACE(sizeof(struct af_alg_iv) + vector.size());
        uint32_t vectorLength = static_cast<uint32_t>(vector.size());
        static_assert(!offsetof(struct af_alg_iv, ivlen) && sizeof(vectorLength) == offsetof(struct af_alg_iv, iv));
        memcpy(CMSG_DATA(header), &vectorLength, sizeof(vectorLength));
        memcpy(CMSG_DATA(header) + sizeof(vectorLength), vector.data(), vector.size());
    }
    if (associatedLength >= 0) {
        header = CMSG_NXTHDR(&message, header);
        if (!header)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "unexpected NULL result from CMSG_NXTHDR(assoc)"_s));
        put(header, ALG_SET_AEAD_ASSOCLEN, static_cast<unsigned>(associatedLength));
    }

    Vector<struct iovec> vectors;
    ssize_t sent = 0;
    auto call = [&] {
        vectors.shrink(0);
        for (size_t i = 0; i < buffers.size(); ++i) {
            auto span = buffers.at(i);
            vectors.append({ const_cast<uint8_t*>(span.data()), span.size() });
        }
        message.msg_iov = vectors.mutableSpan().data();
        message.msg_iovlen = vectors.size();
        sent = ::sendmsg(self.descriptor, &message, flags);
        return sent >= 0;
    };
    if (!callSocket(globalObject, self, WaitingTo::Write, call))
        return { };
    return JSValue::encode(intFromInt64(globalObject, sent));
}
#endif

// ---- The class

void initializeSocketType(JSGlobalObject* globalObject, SocketModuleState& state)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    constexpr auto byParseTuple = Arguments::AreCheckedAsByParseTuple;

    PyType* type = createBuiltinType(globalObject, "_socket.socket"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    state.socketType.set(vm, realm, type);
    // Few of them say what they take in a way that anything but a person can read, so that is said here.
    addMethods(globalObject, type, {
        { "__new__"_s, socketNew, Kind::New, 0, { }, Arguments::AreNotChecked },
        { "__init__"_s, socketInit, Kind::Wrapper, 0, "socket(family=-1, type=-1, proto=-1, fileno=None)"_s, Arguments::AreThoseOfTheClass },
        { "__repr__"_s, socketRepr, Kind::Wrapper },
        { "__del__"_s, socketDel, Kind::Wrapper },
        { "_accept"_s, socketAccept, Kind::Method, 0, "($self, /)"_s },
        { "bind"_s, socketBind, Kind::Method, 0, "($self, address, /)"_s },
        { "close"_s, socketClose },
        { "connect"_s, socketConnect, Kind::Method, pack(true), "($self, address, /)"_s },
        { "connect_ex"_s, socketConnect, Kind::Method, pack(false), "($self, address, /)"_s },
        { "detach"_s, socketDetach, Kind::Method, 0, "($self, /)"_s },
        { "fileno"_s, socketFileno, Kind::Method, 0, "($self, /)"_s },
        { "getpeername"_s, socketGetName, Kind::Method, pack(true), "($self, /)"_s },
        { "getsockname"_s, socketGetName, Kind::Method, pack(false), "($self, /)"_s },
        { "getsockopt"_s, socketGetOption, Kind::Method, 0, "($self, level, option, buffersize=0, /)"_s, byParseTuple },
        { "listen"_s, socketListen, Kind::Method, 0, "($self, backlog=128, /)"_s, byParseTuple },
        { "recv"_s, socketReceive, Kind::Method, pack(false), "($self, buffersize, flags=0, /)"_s, byParseTuple },
        { "recv_into"_s, socketReceiveInto, Kind::Method, pack(false), "($self, /, buffer, nbytes=0, flags=0)"_s },
        { "recvfrom"_s, socketReceive, Kind::Method, pack(true), "($self, buffersize, flags=0, /)"_s, byParseTuple },
        { "recvfrom_into"_s, socketReceiveInto, Kind::Method, pack(true), "($self, /, buffer, nbytes=0, flags=0)"_s },
        { "send"_s, socketSend, Kind::Method, 0, "($self, data, flags=0, /)"_s, byParseTuple },
        { "sendall"_s, socketSendAll, Kind::Method, 0, "($self, data, flags=0, /)"_s, byParseTuple },
        { "sendto"_s, socketSendTo, Kind::Method, 0, "($self, /, *args)"_s, byParseTuple },
        { "setblocking"_s, socketSetBlocking, Kind::Method, 0, "($self, flag, /)"_s },
        { "getblocking"_s, socketGetBlocking, Kind::Method, 0, "($self, /)"_s },
        { "settimeout"_s, socketSetTimeout, Kind::Method, 0, "($self, timeout, /)"_s },
        { "gettimeout"_s, socketGetTimeout, Kind::Method, 0, "($self, /)"_s },
        { "setsockopt"_s, socketSetOption, Kind::Method, 0, "($self, /, *args)"_s, byParseTuple },
        { "shutdown"_s, socketShutdown, Kind::Method, 0, "($self, flag, /)"_s },
        { "recvmsg"_s, socketReceiveMessage, Kind::Method, 0, "($self, bufsize, ancbufsize=0, flags=0, /)"_s, byParseTuple },
        { "recvmsg_into"_s, socketReceiveMessageInto, Kind::Method, 0, "($self, buffers, ancbufsize=0, flags=0, /)"_s, byParseTuple },
        { "sendmsg"_s, socketSendMessage, Kind::Method, 0, "($self, buffers, ancdata=(), flags=0, address=None, /)"_s, byParseTuple },
#ifdef HAVE_SOCKADDR_ALG
        // It has something to say before it looks at them.
        { "sendmsg_afalg"_s, socketSendMessageToAlgorithm, Kind::Method, 0, "($self, /, msg=None, *, op=None, iv=None, assoclen=None, flags=0)"_s, Arguments::AreNotChecked },
#endif
    });
    addMember(globalObject, type, "family"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<Socket>(self).family); });
    addMember(globalObject, type, "type"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<Socket>(self).type); });
    addMember(globalObject, type, "proto"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<Socket>(self).protocol); });
    addGetSet(globalObject, type, "timeout"_s, [] (JSGlobalObject*, JSValue self) { return timeoutOf(stateOf<Socket>(self)); });
}

} } // namespace JSC::Python

#endif // OS(UNIX)
