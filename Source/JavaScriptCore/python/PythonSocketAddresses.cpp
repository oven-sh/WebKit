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
#include "PythonCodecs.h"
#include "PythonOperations.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>

// What the rest of _socket is written over: what raises, what waits, and addresses, of CPython's Modules/socketmodule.c.

namespace JSC { namespace Python {

SocketModuleState& socketModuleState(JSGlobalObject* globalObject) { return globalObject->pyRealm()->moduleState<SocketModuleState>(); }

// ---- What raises

JSValue raiseSocketError(JSGlobalObject* globalObject, ThrowScope& scope) { return raiseOSError(globalObject, scope, errno); }

JSValue raiseOSErrorSaying(JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { return raise(globalObject, scope, BuiltinType::OSError, message); }

// PyErr_SetObject(type, (number, message))
static JSValue raiseNumbered(JSGlobalObject* globalObject, ThrowScope& scope, PyType* type, int number, const char* message)
{
    // decode_error_message()
    JSValue text = decodeFileSystemBytes(globalObject, unsafeSpan(message));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue exception = call(globalObject, type->object(), jsNumber(number), text);
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
    return { };
}

JSValue raiseHostError(JSGlobalObject* globalObject, ThrowScope& scope, int error)
{
    return raiseNumbered(globalObject, scope, socketModuleState(globalObject).hostError.get(), error, hstrerror(error));
}

JSValue raiseAddressInfoError(JSGlobalObject* globalObject, ThrowScope& scope, int error)
{
    if (error == EAI_SYSTEM)
        return raiseSocketError(globalObject, scope);
    return raiseNumbered(globalObject, scope, socketModuleState(globalObject).addressInfoError.get(), error, gai_strerror(error));
}

// ---- Arguments

std::optional<unsigned> toCUnsignedIntOfFormat(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // PyLong_AsUnsignedLongMask()
    JSValue number = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return static_cast<unsigned>(lowBitsOfInt(number));
}

std::optional<CString> toHostNameOfFormat(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ByteVector bytes;
    if (isBytes(value) || isByteArray(value)) {
        Buffer buffer = bufferOf(globalObject, value);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        bytes.append(buffer.span());
    } else if (stringIn(value)) {
        auto encoded = encodeString(globalObject, value, "idna"_s, String());
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        bytes = WTF::move(*encoded);
    } else {
        raiseTypeError(globalObject, scope, concatenate(function, "() argument 1 must be str, bytes or bytearray, not "_s, typeNameOfArgument(globalObject, value)));
        return std::nullopt;
    }
    if (WTF::find(bytes.span(), static_cast<uint8_t>(0)) != notFound) {
        raiseTypeError(globalObject, scope, concatenate(function, "() argument 1 must be encoded string without null bytes, not "_s, typeNameOfArgument(globalObject, value)));
        return std::nullopt;
    }
    return CString(byteCast<char>(bytes.span()));
}

// ---- Waiting

bool setBlocking(JSGlobalObject* globalObject, Socket& socket, bool isBlocking)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    unsigned isNotBlocking = !isBlocking;
    if (ioctl(socket.descriptor, FIONBIO, &isNotBlocking) == -1) {
        raiseSocketError(globalObject, scope);
        return false;
    }
    return true;
}

std::optional<int64_t> parseSocketTimeout(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isNone(value))
        return -nanosecondsPerSecond;
    auto timeout = timeFromSecondsObject(globalObject, value, TimeRounding::Timeout);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*timeout < 0) {
        raiseValueError(globalObject, scope, "Timeout value out of range"_s);
        return std::nullopt;
    }
    return timeout;
}

enum class Readiness : uint8_t { Ready, TimedOut, Failed };

// internal_select()
static Readiness waitForSocket(Socket& socket, WaitingTo waitingTo, int64_t interval, bool isConnecting)
{
    ASSERT(!isConnecting || waitingTo == WaitingTo::Write);
    // It has been closed.
    if (socket.descriptor == -1)
        return Readiness::Ready;
    struct pollfd entry;
    entry.fd = socket.descriptor;
    entry.events = waitingTo == WaitingTo::Write ? POLLOUT : POLLIN;
    // It can be written to once it is connected, and once it has failed to be.
    if (isConnecting)
        entry.events |= POLLERR;
    int64_t milliseconds = std::min<int64_t>(divideTime(interval, nanosecondsPerMillisecond, TimeRounding::Ceiling), std::numeric_limits<int>::max());
    // Some systems will have no other negative number.
    if (milliseconds < 0) {
#ifdef INFTIM
        milliseconds = INFTIM;
#else
        milliseconds = -1;
#endif
    }
    int count = poll(&entry, 1, static_cast<int>(milliseconds));
    if (count < 0)
        return Readiness::Failed;
    return count ? Readiness::Ready : Readiness::TimedOut;
}

bool callSocket(JSGlobalObject* globalObject, Socket& socket, WaitingTo waitingTo, const ScopedLambda<bool()>& function, bool isConnecting, int* error, int64_t timeout)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    bool hasTimeout = timeout > 0;
    std::optional<int64_t> deadline;

    // Round again if waiting is interrupted, or if it turns out that there was nothing to have waited for.
    for (;;) {
        // What is connecting goes on with it by itself, so that is waited for even by one that would wait for ever.
        if (hasTimeout || isConnecting) {
            Readiness readiness;
            if (hasTimeout) {
                int64_t interval = timeout;
                if (deadline)
                    interval = timeUntil(*deadline);
                else
                    deadline = deadlineAfter(timeout);
                readiness = interval >= 0 ? waitForSocket(socket, waitingTo, interval, isConnecting) : Readiness::TimedOut;
            } else
                readiness = waitForSocket(socket, waitingTo, timeout, isConnecting);

            if (readiness == Readiness::Failed) {
                if (error)
                    *error = errno;
                if (errno == EINTR) {
                    if (!checkSignals(globalObject)) {
                        if (error)
                            *error = -1;
                        return false;
                    }
                    continue;
                }
                raiseSocketError(globalObject, scope);
                return false;
            }
            if (readiness == Readiness::TimedOut) {
                if (error)
                    *error = EWOULDBLOCK;
                else
                    raise(globalObject, scope, BuiltinType::TimeoutError, "timed out"_s);
                return false;
            }
        }

        // Round again if the call itself is interrupted.
        for (;;) {
            if (function()) {
                if (error)
                    *error = 0;
                return true;
            }
            if (error)
                *error = errno;
            if (errno != EINTR)
                break;
            if (!checkSignals(globalObject)) {
                if (error)
                    *error = -1;
                return false;
            }
        }

        // It was said to be ready and was not: what had come was thrown away for having the wrong checksum, say.
        if (socket.timeout > 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
            continue;

        if (!error)
            raiseSocketError(globalObject, scope);
        return false;
    }
}

// ---- Making one

bool setNotInheritable(JSGlobalObject* globalObject, int descriptor)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int flags = fcntl(descriptor, F_GETFD);
    if (flags < 0 || (!(flags & FD_CLOEXEC) && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)) {
        raiseSocketError(globalObject, scope);
        return false;
    }
    return true;
}

bool initializeSocket(JSGlobalObject* globalObject, Socket& socket, int descriptor, int family, int type, int protocol)
{
    socket.descriptor = descriptor;
    socket.family = family;
    socket.type = type;
    // On some systems these can be given as part of the type. They are not part of what it says its type is, so that `sock.type == SOCK_STREAM` means the same everywhere.
#ifdef SOCK_NONBLOCK
    socket.type &= ~SOCK_NONBLOCK;
#endif
#ifdef SOCK_CLOEXEC
    socket.type &= ~SOCK_CLOEXEC;
#endif
    socket.protocol = protocol;
#ifdef SOCK_NONBLOCK
    if (type & SOCK_NONBLOCK) {
        socket.timeout = 0;
        return true;
    }
#endif
    socket.timeout = socketModuleState(globalObject).defaultTimeout;
    return socket.timeout < 0 || setBlocking(globalObject, socket, false);
}

PyStateObject* newSocket(JSGlobalObject* globalObject, int descriptor, int family, int type, int protocol)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyStateObject::create(vm, socketModuleState(globalObject).socketType->instanceStructure(), makeUnique<Socket>());
    bool succeeded = initializeSocket(globalObject, object->state<Socket>(), descriptor, family, type, protocol);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ASSERT_UNUSED(succeeded, succeeded);
    return object;
}

// ---- Addresses

template<typename Address>
static void setLength(Address& address)
{
    // HAVE_SOCKADDR_SA_LEN
#if OS(DARWIN) || OS(FREEBSD) || OS(NETBSD) || OS(OPENBSD)
    std::bit_cast<struct sockaddr*>(&address)->sa_len = sizeof(Address);
#else
    UNUSED_PARAM(address);
#endif
}

std::optional<int> setIPAddress(JSGlobalObject* globalObject, const char* name, struct sockaddr* result, size_t resultSize, int family)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    struct addrinfo hints;
    struct addrinfo* found;
    zeroBytes(*result);

    if (!name[0]) {
        zeroBytes(hints);
        hints.ai_family = family;
        hints.ai_socktype = SOCK_DGRAM; // Which makes no difference
        hints.ai_flags = AI_PASSIVE;
        if (int error = getaddrinfo(nullptr, "0", &hints, &found)) {
            raiseAddressInfoError(globalObject, scope, error);
            return std::nullopt;
        }
        int size;
        switch (found->ai_family) {
        case AF_INET:
            size = 4;
            break;
        case AF_INET6:
            size = 16;
            break;
        default:
            freeaddrinfo(found);
            raiseOSErrorSaying(globalObject, scope, "unsupported address family"_s);
            return std::nullopt;
        }
        if (found->ai_next) {
            freeaddrinfo(found);
            raiseOSErrorSaying(globalObject, scope, "wildcard resolved to multiple address"_s);
            return std::nullopt;
        }
        memcpy(result, found->ai_addr, std::min<size_t>(found->ai_addrlen, resultSize));
        freeaddrinfo(found);
        return size;
    }

    if (!strcmp(name, "255.255.255.255") || !strcmp(name, "<broadcast>")) {
        if (family != AF_INET && family != AF_UNSPEC) {
            raiseOSErrorSaying(globalObject, scope, "address family mismatched"_s);
            return std::nullopt;
        }
        auto* address = std::bit_cast<struct sockaddr_in*>(result);
        zeroBytes(*address);
        address->sin_family = AF_INET;
        setLength(*address);
        address->sin_addr.s_addr = INADDR_BROADCAST;
        return sizeof(address->sin_addr);
    }

    // What is a number already is not looked up.
    if (family == AF_UNSPEC || family == AF_INET) {
        auto* address = std::bit_cast<struct sockaddr_in*>(result);
        zeroBytes(*address);
        if (inet_pton(AF_INET, name, &address->sin_addr) > 0) {
            address->sin_family = AF_INET;
            setLength(*address);
            return 4;
        }
    }
    // One that says which interface it is on is left to getaddrinfo(), which knows the interfaces by name.
    if ((family == AF_UNSPEC || family == AF_INET6) && !strchr(name, '%')) {
        auto* address = std::bit_cast<struct sockaddr_in6*>(result);
        zeroBytes(*address);
        if (inet_pton(AF_INET6, name, &address->sin6_addr) > 0) {
            address->sin6_family = AF_INET6;
            setLength(*address);
            return 16;
        }
    }

    zeroBytes(hints);
    hints.ai_family = family;
    if (int error = getaddrinfo(name, nullptr, &hints, &found)) {
        raiseAddressInfoError(globalObject, scope, error);
        return std::nullopt;
    }
    memcpy(result, found->ai_addr, std::min<size_t>(found->ai_addrlen, resultSize));
    freeaddrinfo(found);
    switch (result->sa_family) {
    case AF_INET:
        return 4;
    case AF_INET6:
        return 16;
    default:
        raiseOSErrorSaying(globalObject, scope, "unknown address family"_s);
        return std::nullopt;
    }
}

JSValue makeIPv4Address(JSGlobalObject* globalObject, const struct sockaddr_in& address)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    char buffer[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)))
        return raiseSocketError(globalObject, scope);
    return jsString(globalObject->vm(), String::fromLatin1(buffer));
}

JSValue makeIPv6Address(JSGlobalObject* globalObject, const struct sockaddr_in6& address)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    char buffer[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, &address.sin6_addr, buffer, sizeof(buffer)))
        return raiseSocketError(globalObject, scope);
    return jsString(globalObject->vm(), String::fromLatin1(buffer));
}

JSValue makeSocketAddress(JSGlobalObject* globalObject, int descriptor, const struct sockaddr* address, size_t length, int protocol)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // There is none, as when what is received is from what the socket is connected to.
    if (!length)
        return jsUndefined();
#if OS(LINUX)
    if (auto made = makeLinuxSocketAddress(globalObject, descriptor, address, protocol))
        RELEASE_AND_RETURN(scope, *made);
#else
    UNUSED_PARAM(descriptor);
#endif

    switch (address->sa_family) {
    case AF_INET: {
        auto& in = *std::bit_cast<const struct sockaddr_in*>(address);
        JSValue host = makeIPv4Address(globalObject, in);
        RETURN_IF_EXCEPTION(scope, { });
        return PyTuple::create(globalObject, { host, jsNumber(ntohs(in.sin_port)) });
    }
    case AF_UNIX: {
        auto& un = *std::bit_cast<const struct sockaddr_un*>(address);
#if OS(LINUX)
        // A name that begins with a zero is not the name of a file, and is as long as it is said to be.
        size_t nameLength = length - offsetof(struct sockaddr_un, sun_path);
        if (nameLength > 0 && !un.sun_path[0])
            return newBytes(globalObject, byteCast<uint8_t>(std::span<const char>(un.sun_path).first(nameLength)));
#endif
        RELEASE_AND_RETURN(scope, decodeFileSystemBytes(globalObject, unsafeSpan(un.sun_path)));
    }
    case AF_INET6: {
        auto& in6 = *std::bit_cast<const struct sockaddr_in6*>(address);
        JSValue host = makeIPv6Address(globalObject, in6);
        RETURN_IF_EXCEPTION(scope, { });
        return PyTuple::create(globalObject, { host, jsNumber(ntohs(in6.sin6_port)), intFromUInt64(globalObject, ntohl(in6.sin6_flowinfo)), intFromUInt64(globalObject, in6.sin6_scope_id) });
    }
#if OS(DARWIN)
    case PF_SYSTEM:
        if (protocol == SYSPROTO_CONTROL) {
            auto& control = *std::bit_cast<const struct sockaddr_ctl*>(address);
            return PyTuple::create(globalObject, { intFromUInt64(globalObject, control.sc_id), intFromUInt64(globalObject, control.sc_unit) });
        }
        return raiseValueError(globalObject, scope, "Invalid address type"_s);
#endif
    default:
        UNUSED_PARAM(protocol);
        // One of a kind that is not known is given as it is.
        return PyTuple::create(globalObject, { jsNumber(address->sa_family), newBytes(globalObject, byteCast<uint8_t>(std::span<const char>(address->sa_data))) });
    }
}

// idna_converter(): the name of a host, in an address. What is all ASCII, as a number is, is not put through the codec. Nothing if it raised.
static std::optional<CString> toHostName(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ByteVector bytes;
    if (isBytes(value) || isByteArray(value)) {
        Buffer buffer = bufferOf(globalObject, value);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        bytes.append(buffer.span());
    } else if (JSString* string = stringIn(value)) {
        String text = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (text.containsOnlyASCII())
            bytes.append(byteCast<uint8_t>(text.ascii().span()));
        else {
            auto encoded = encodeString(globalObject, value, "idna"_s, String());
            if (scope.exception()) [[unlikely]] {
                if (scope.tryClearException())
                    raiseTypeError(globalObject, scope, "encoding of hostname failed"_s);
                return std::nullopt;
            }
            bytes = WTF::move(*encoded);
        }
    } else {
        raiseTypeError(globalObject, scope, concatenate("str, bytes or bytearray expected, not "_s, typeName(globalObject, value)));
        return std::nullopt;
    }
    if (WTF::find(bytes.span(), static_cast<uint8_t>(0)) != notFound) {
        raiseTypeError(globalObject, scope, "host name must not contain null character"_s);
        return std::nullopt;
    }
    return CString(byteCast<char>(bytes.span()));
}

// What AF_INET and AF_INET6 have in common: (host, port), and for the second of them two more that need not be there. False if it raised.
static bool parseInternetAddress(JSGlobalObject* globalObject, JSValue value, ASCIILiteral caller, ASCIILiteral familyName, ASCIILiteral shape, unsigned maximum, CString& host, int& port, unsigned& flowInfo, unsigned& scopeID)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isTuple(value)) {
        raiseTypeError(globalObject, scope, concatenate(caller, "(): "_s, familyName, " address must be tuple, not "_s, typeName(globalObject, value)));
        return false;
    }
    PyTuple* tuple = asTuple(value);
    if (tuple->length() < 2 || tuple->length() > maximum) {
        raiseTypeError(globalObject, scope, shape);
        return false;
    }
    auto name = toHostName(globalObject, tuple->at(0));
    RETURN_IF_EXCEPTION(scope, false);
    host = WTF::move(*name);
    auto convert = [&] (auto& target, auto converted) {
        if (scope.exception()) [[unlikely]] {
            if (catchException(globalObject, BuiltinType::OverflowError))
                raise(globalObject, scope, BuiltinType::OverflowError, concatenate(caller, "(): port must be 0-65535."_s));
            return false;
        }
        target = *converted;
        return true;
    };
    if (!convert(port, toCIntOfFormat(globalObject, tuple->at(1))))
        return false;
    if (tuple->length() > 2 && !convert(flowInfo, toCUnsignedIntOfFormat(globalObject, tuple->at(2))))
        return false;
    if (tuple->length() > 3 && !convert(scopeID, toCUnsignedIntOfFormat(globalObject, tuple->at(3))))
        return false;
    return true;
}

bool toSocketAddress(JSGlobalObject* globalObject, Socket& socket, JSValue value, SocketAddress& result, int& length, ASCIILiteral caller)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto checkPort = [&] (int port) {
        if (port >= 0 && port <= 0xffff)
            return true;
        raise(globalObject, scope, BuiltinType::OverflowError, concatenate(caller, "(): port must be 0-65535."_s));
        return false;
    };

    switch (socket.family) {
    case AF_UNIX: {
        // Not as the name of a file is taken elsewhere, since on Linux it can have zeros in it.
        ByteVector path;
        if (stringIn(value)) {
            auto encoded = encodeString(globalObject, value, "utf-8"_s, "surrogateescape"_s);
            RETURN_IF_EXCEPTION(scope, false);
            path = WTF::move(*encoded);
        } else {
            Buffer buffer = bufferOf(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            path.append(buffer.span());
        }
        auto& address = result.un;
        zeroBytes(address);
#if OS(LINUX)
        if (path.isEmpty() || !path[0]) {
            if (path.size() > sizeof(address.sun_path)) {
                raiseOSErrorSaying(globalObject, scope, "AF_UNIX path too long"_s);
                return false;
            }
            length = path.size() + offsetof(struct sockaddr_un, sun_path);
        } else
#endif
        {
            if (path.size() >= sizeof(address.sun_path)) {
                raiseOSErrorSaying(globalObject, scope, "AF_UNIX path too long"_s);
                return false;
            }
            // With the zero that ends it
            length = path.size() + offsetof(struct sockaddr_un, sun_path) + 1;
        }
        address.sun_family = socket.family;
        memcpy(address.sun_path, path.span().data(), path.size());
        return true;
    }
#ifdef AF_RDS
    // Its addresses are those of AF_INET.
    case AF_RDS:
#endif
    case AF_INET: {
        CString host;
        int port = 0;
        unsigned unused = 0;
        if (!parseInternetAddress(globalObject, value, caller, "AF_INET"_s, "AF_INET address must be a pair (host, port)"_s, 2, host, port, unused, unused))
            return false;
        auto& address = result.in;
        zeroBytes(address);
        setIPAddress(globalObject, host.data(), &result.sa, sizeof(address), AF_INET);
        RETURN_IF_EXCEPTION(scope, false);
        if (!checkPort(port))
            return false;
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<short>(port));
        length = sizeof(address);
        return true;
    }
    case AF_INET6: {
        CString host;
        int port = 0;
        unsigned flowInfo = 0;
        unsigned scopeID = 0;
        if (!parseInternetAddress(globalObject, value, caller, "AF_INET6"_s, "AF_INET6 address must be a tuple (host, port[, flowinfo[, scopeid]])"_s, 4, host, port, flowInfo, scopeID))
            return false;
        auto& address = result.in6;
        zeroBytes(address);
        setIPAddress(globalObject, host.data(), &result.sa, sizeof(address), AF_INET6);
        RETURN_IF_EXCEPTION(scope, false);
        if (!checkPort(port))
            return false;
        if (flowInfo > 0xfffff) {
            raise(globalObject, scope, BuiltinType::OverflowError, concatenate(caller, "(): flowinfo must be 0-1048575."_s));
            return false;
        }
        address.sin6_family = socket.family;
        address.sin6_port = htons(static_cast<short>(port));
        address.sin6_flowinfo = htonl(flowInfo);
        address.sin6_scope_id = scopeID;
        length = sizeof(address);
        return true;
    }
#if OS(DARWIN)
    case PF_SYSTEM: {
        if (socket.protocol != SYSPROTO_CONTROL) {
            raiseOSErrorSaying(globalObject, scope, concatenate(caller, "(): unsupported PF_SYSTEM protocol"_s));
            return false;
        }
        auto& address = result.ctl;
        zeroBytes(address);
        address.sc_family = AF_SYSTEM;
        address.ss_sysaddr = AF_SYS_CONTROL;
        if (stringIn(value)) {
            auto name = toFileSystemEncoded(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            struct ctl_info info;
            if (name->length() > sizeof(info.ctl_name)) {
                raiseValueError(globalObject, scope, "provided string is too long"_s);
                return false;
            }
            strncpy(info.ctl_name, name->data(), sizeof(info.ctl_name));
            if (ioctl(socket.descriptor, CTLIOCGINFO, &info)) {
                raiseOSErrorSaying(globalObject, scope, "cannot find kernel control with provided name"_s);
                return false;
            }
            address.sc_id = info.ctl_id;
            address.sc_unit = 0;
        } else {
            // Whatever is wrong with it, the same is said.
            auto fail = [&] {
                if (!scope.exception() || scope.tryClearException())
                    raiseTypeError(globalObject, scope, concatenate(caller, "(): PF_SYSTEM address must be a str or a pair (id, unit)"_s));
                return false;
            };
            if (!isTuple(value) || asTuple(value)->length() != 2)
                return fail();
            auto id = toCUnsignedIntOfFormat(globalObject, asTuple(value)->at(0));
            if (!id)
                return fail();
            auto unit = toCUnsignedIntOfFormat(globalObject, asTuple(value)->at(1));
            if (!unit)
                return fail();
            address.sc_id = *id;
            address.sc_unit = *unit;
        }
        length = sizeof(address);
        return true;
    }
#endif
    default:
#if OS(LINUX)
        if (auto isDone = toLinuxSocketAddress(globalObject, socket, value, result, length, caller))
            RELEASE_AND_RETURN(scope, *isDone);
#endif
        raiseOSErrorSaying(globalObject, scope, concatenate(caller, "(): bad family"_s));
        return false;
    }
}

bool socketAddressLength(JSGlobalObject* globalObject, Socket& socket, socklen_t& length)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    switch (socket.family) {
    case AF_UNIX:
        length = sizeof(struct sockaddr_un);
        return true;
#ifdef AF_RDS
    case AF_RDS:
#endif
    case AF_INET:
        length = sizeof(struct sockaddr_in);
        return true;
    case AF_INET6:
        length = sizeof(struct sockaddr_in6);
        return true;
#if OS(DARWIN)
    case PF_SYSTEM:
        if (socket.protocol == SYSPROTO_CONTROL) {
            length = sizeof(struct sockaddr_ctl);
            return true;
        }
        raiseOSErrorSaying(globalObject, scope, "getsockaddrlen: unknown PF_SYSTEM protocol"_s);
        return false;
#endif
    default:
#if OS(LINUX)
        if (auto known = linuxSocketAddressLength(socket.family)) {
            length = *known;
            return true;
        }
#endif
        raiseOSErrorSaying(globalObject, scope, "getsockaddrlen: bad family"_s);
        return false;
    }
}

// ---- What goes along with what is sent

std::optional<size_t> controlMessageLength(size_t length)
{
    if (length > socketLengthLimit - CMSG_LEN(0))
        return std::nullopt;
    size_t result = CMSG_LEN(length);
    if (result > socketLengthLimit || result < length)
        return std::nullopt;
    return result;
}

std::optional<size_t> controlMessageSpace(size_t length)
{
    // Of one, so as to allow for the padding both before and after
    if (length > socketLengthLimit - CMSG_SPACE(1))
        return std::nullopt;
    size_t result = CMSG_SPACE(length);
    if (result > socketLengthLimit || result < length)
        return std::nullopt;
    return result;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
