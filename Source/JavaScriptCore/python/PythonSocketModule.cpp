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


// Without this the system's headers leave out what RFC 3542 added for IPv6. It has to be said before any of them is read, so this file is compiled by itself.
#if defined(__APPLE__)
#define __APPLE_USE_RFC_3542 1
#endif

#include "config.h"
#include "PythonSocket.h"

#if OS(UNIX)

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonOperations.h"
#include "PythonPosixModule.h"
#include "PythonSequences.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <wtf/Scope.h>
#if __has_include(<net/ethernet.h>)
#include <net/ethernet.h>
#endif

// The module _socket: the functions of CPython's Modules/socketmodule.c, and what is in the module. socket is written over it.

namespace JSC { namespace Python {

#define CONVERT(name, expression) \
    auto name##Converted = (expression); \
    RETURN_IF_EXCEPTION(scope, { }); \
    auto name = *name##Converted

#define CONVERT_INT_OR(name, value, defaultValue) \
    int name = defaultValue; \
    if (JSValue name##Given = (value)) { \
        auto name##Converted = toCIntOfFormat(globalObject, name##Given); \
        RETURN_IF_EXCEPTION(scope, { }); \
        name = *name##Converted; \
    }

// The "s" of PyArg_ParseTuple(). A null CString is what was not given. Nothing if it raised.
static std::optional<CString> toCStringOfFormat(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, ASCIILiteral argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value)
        return CString();
    auto text = toTextArgument(globalObject, value, function, argument);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto encoded = encodeString(globalObject, jsString(globalObject->vm(), *text), "utf-8"_s, String());
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return CString(byteCast<char>(encoded->span()));
}

// PyUnicode_FromString(). Empty if it raised.
static JSValue stringFromUTF8(JSGlobalObject* globalObject, const char* text)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String decoded = decodeUTF8(globalObject, byteCast<uint8_t>(unsafeSpan(text)), "strict"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return jsString(globalObject->vm(), decoded);
}

// ---- The name of this host, and of others

PYTHON_NATIVE(socketGetHostName)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    if (!audit(globalObject, "socket.gethostname"_s))
        return { };
    char buffer[1024];
    if (gethostname(buffer, sizeof(buffer) - 1) < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    buffer[sizeof(buffer) - 1] = '\0';
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, unsafeSpan(buffer))));
}

PYTHON_NATIVE(socketSetHostName)
{
    NATIVE_PROLOGUE();
    // bytes as they are, or whatever will do for the name of a file
    JSValue name = args[0];
    if (!isBytes(name)) {
        auto encoded = toFileSystemEncoded(globalObject, name);
        RETURN_IF_EXCEPTION(scope, { });
        name = newBytes(globalObject, byteCast<uint8_t>(encoded->span()));
    }
    if (!audit(globalObject, "socket.sethostname"_s, name))
        return { };
    Buffer buffer = bufferOf(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    auto span = buffer.span();
    if (sethostname(byteCast<char>(span.data()), span.size()))
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(socketGetHostByName)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toHostNameOfFormat(globalObject, args[0], "gethostbyname"_s));
    if (!audit(globalObject, "socket.gethostbyname"_s, args[0]))
        return { };
    struct sockaddr_in address;
    setIPAddress(globalObject, name.data(), std::bit_cast<struct sockaddr*>(&address), sizeof(address), AF_INET);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(makeIPv4Address(globalObject, address)));
}

// gethost_common(): (name, aliases, addresses). Empty if it raised.
static JSValue hostEntry(JSGlobalObject* globalObject, struct hostent* entry, int family)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!entry)
        return raiseHostError(globalObject, scope, h_errno);
    if (entry->h_addrtype != family)
        return raiseOSError(globalObject, scope, EAFNOSUPPORT);

    // Where the pointers are need not be where a pointer can be read from.
    auto pointerAt = [] (char** where) {
        char* pointer;
        memcpy(&pointer, where, sizeof(pointer));
        return pointer;
    };
    JSArray* names = newList(globalObject);
    JSArray* addresses = newList(globalObject);
    if (entry->h_aliases) {
        for (char** where = entry->h_aliases; char* alias = pointerAt(where); ++where) {
            JSValue name = stringFromUTF8(globalObject, alias);
            RETURN_IF_EXCEPTION(scope, { });
            listAppend(globalObject, names, name);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    for (char** where = entry->h_addr_list; char* bytes = pointerAt(where); ++where) {
        JSValue address;
        switch (family) {
        case AF_INET: {
            struct sockaddr_in in;
            zeroBytes(in);
            memcpy(&in.sin_addr, bytes, sizeof(in.sin_addr));
            address = makeIPv4Address(globalObject, in);
            break;
        }
        case AF_INET6: {
            struct sockaddr_in6 in6;
            zeroBytes(in6);
            memcpy(&in6.sin6_addr, bytes, sizeof(in6.sin6_addr));
            address = makeIPv6Address(globalObject, in6);
            break;
        }
        default:
            return raiseOSErrorSaying(globalObject, scope, "unsupported address family"_s);
        }
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, addresses, address);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue name = stringFromUTF8(globalObject, entry->h_name);
    RETURN_IF_EXCEPTION(scope, { });
    return PyTuple::create(globalObject, { name, names, addresses });
}

PYTHON_NATIVE(socketGetHostByNameEx)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toHostNameOfFormat(globalObject, args[0], "gethostbyname_ex"_s));
    if (!audit(globalObject, "socket.gethostbyname"_s, args[0]))
        return { };
    SocketAddress address;
    setIPAddress(globalObject, name.data(), &address.sa, sizeof(address), AF_INET);
    RETURN_IF_EXCEPTION(scope, { });
    ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    struct hostent* entry = gethostbyname(name.data());
    ALLOW_DEPRECATED_DECLARATIONS_END
    RELEASE_AND_RETURN(scope, JSValue::encode(hostEntry(globalObject, entry, address.sa.sa_family)));
}

PYTHON_NATIVE(socketGetHostByAddress)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toHostNameOfFormat(globalObject, args[0], "gethostbyaddr"_s));
    if (!audit(globalObject, "socket.gethostbyaddr"_s, args[0]))
        return { };
    SocketAddress address;
    setIPAddress(globalObject, name.data(), &address.sa, sizeof(address), AF_UNSPEC);
    RETURN_IF_EXCEPTION(scope, { });
    int family = address.sa.sa_family;
    const void* bytes;
    socklen_t length;
    switch (family) {
    case AF_INET:
        bytes = &address.in.sin_addr;
        length = sizeof(address.in.sin_addr);
        break;
    case AF_INET6:
        bytes = &address.in6.sin6_addr;
        length = sizeof(address.in6.sin6_addr);
        break;
    default:
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "unsupported address family"_s));
    }
    ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    struct hostent* entry = gethostbyaddr(bytes, length, family);
    ALLOW_DEPRECATED_DECLARATIONS_END
    RELEASE_AND_RETURN(scope, JSValue::encode(hostEntry(globalObject, entry, family)));
}

// ---- Services and protocols, by name and by number

static JSValue stringOrNone(JSGlobalObject* globalObject, const CString& text) { return text.isNull() ? jsUndefined() : stringFromUTF8(globalObject, text.data()); }

// getservbyname(servicename[, protocolname])
PYTHON_NATIVE(socketGetServiceByName)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toCStringOfFormat(globalObject, args[0], "getservbyname"_s, "argument 1"_s));
    CONVERT(protocol, toCStringOfFormat(globalObject, args.at(1), "getservbyname"_s, "argument 2"_s));
    if (!audit(globalObject, "socket.getservbyname"_s, stringOrNone(globalObject, name), stringOrNone(globalObject, protocol)))
        return { };
    struct servent* entry = getservbyname(name.data(), protocol.data());
    if (!entry)
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "service/proto not found"_s));
    return JSValue::encode(jsNumber(ntohs(entry->s_port)));
}

// getservbyport(port[, protocolname])
PYTHON_NATIVE(socketGetServiceByPort)
{
    NATIVE_PROLOGUE();
    CONVERT(port, toCIntOfFormat(globalObject, args[0]));
    CONVERT(protocol, toCStringOfFormat(globalObject, args.at(1), "getservbyport"_s, "argument 2"_s));
    if (port < 0 || port > 0xffff)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "getservbyport: port must be 0-65535."_s));
    if (!audit(globalObject, "socket.getservbyport"_s, jsNumber(port), stringOrNone(globalObject, protocol)))
        return { };
    struct servent* entry = getservbyport(htons(static_cast<short>(port)), protocol.data());
    if (!entry)
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "port/proto not found"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(stringFromUTF8(globalObject, entry->s_name)));
}

PYTHON_NATIVE(socketGetProtocolByName)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toCStringOfFormat(globalObject, args[0], "getprotobyname"_s, "argument 1"_s));
    struct protoent* entry = getprotobyname(name.data());
    if (!entry)
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "protocol not found"_s));
    return JSValue::encode(jsNumber(entry->p_proto));
}

// ---- Descriptors

PYTHON_NATIVE(socketCloseDescriptor)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCLong(globalObject, args[0]));
    if (::close(static_cast<int>(descriptor)) < 0 && errno != ECONNRESET)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RETURN_NONE();
}

PYTHON_NATIVE(socketDuplicate)
{
    NATIVE_PROLOGUE();
    CONVERT(descriptor, toCLong(globalObject, args[0]));
    // _Py_dup()
    int result = fcntl(static_cast<int>(descriptor), F_DUPFD_CLOEXEC, 0);
    if (result < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    return JSValue::encode(jsNumber(result));
}

// socketpair([family[, type[, proto]]])
PYTHON_NATIVE(socketPair)
{
    NATIVE_PROLOGUE();
    CONVERT_INT_OR(family, args.at(0), AF_UNIX);
    CONVERT_INT_OR(type, args.at(1), SOCK_STREAM);
    CONVERT_INT_OR(protocol, args.at(2), 0);
    int descriptors[2];
#ifdef SOCK_CLOEXEC
    int result = socketpair(family, type | SOCK_CLOEXEC, protocol, descriptors);
#else
    int result = socketpair(family, type, protocol, descriptors);
#endif
    if (result < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    auto closeBoth = [&] {
        ::close(descriptors[0]);
        ::close(descriptors[1]);
    };
#ifndef SOCK_CLOEXEC
    if (!setNotInheritable(globalObject, descriptors[0]) || !setNotInheritable(globalObject, descriptors[1])) {
        closeBoth();
        return { };
    }
#endif
    PyStateObject* first = newSocket(globalObject, descriptors[0], family, type, protocol);
    if (!first) {
        closeBoth();
        return { };
    }
    PyStateObject* second = newSocket(globalObject, descriptors[1], family, type, protocol);
    if (!second) {
        // The first has its own now, and there is nothing to close it but this: see the README on what is let go of.
        first->state<Socket>().descriptor = -1;
        closeBoth();
        return { };
    }
    return JSValue::encode(PyTuple::create(globalObject, { first, second }));
}

// ---- The order of bytes

enum class ByteOrder : uint8_t { NetworkToHostShort, NetworkToHostLong, HostToNetworkShort, HostToNetworkLong };

// ntohs(), ntohl(), htons() and htonl()
PYTHON_NATIVE(socketByteOrder)
{
    NATIVE_PROLOGUE();
    auto which = unpack<ByteOrder>(callFrame, 0);
    if (which == ByteOrder::NetworkToHostShort || which == ByteOrder::HostToNetworkShort) {
        CONVERT(value, toUnsigned<uint16_t>(globalObject, args[0], "uint16_t"_s));
        return JSValue::encode(jsNumber(which == ByteOrder::NetworkToHostShort ? ntohs(value) : htons(value)));
    }
    CONVERT(value, toUnsigned<uint32_t>(globalObject, args[0], "uint32_t"_s));
    // Not jsNumber(), which makes a float of what does not fit in 31 bits.
    return JSValue::encode(intFromUInt64(globalObject, which == ByteOrder::NetworkToHostLong ? ntohl(value) : htonl(value)));
}

// ---- Addresses, written out and packed

PYTHON_NATIVE(socketInetAToN)
{
    NATIVE_PROLOGUE();
    CONVERT(text, toCStringOfFormat(globalObject, args[0], "inet_aton"_s, "argument"_s));
    struct in_addr packed;
    if (!inet_aton(text.data(), &packed))
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "illegal IP address string passed to inet_aton"_s));
    return JSValue::encode(newBytes(globalObject, asByteSpan(packed)));
}

PYTHON_NATIVE(socketInetNToA)
{
    NATIVE_PROLOGUE();
    Buffer buffer = bufferOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct in_addr packed;
    auto span = buffer.span();
    if (span.size() != sizeof(packed))
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "packed IP wrong length for inet_ntoa"_s));
    memcpy(&packed, span.data(), sizeof(packed));
    ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    return JSValue::encode(jsString(vm, String::fromLatin1(inet_ntoa(packed))));
    ALLOW_DEPRECATED_DECLARATIONS_END
}

// inet_pton(af, ip)
PYTHON_NATIVE(socketInetPToN)
{
    NATIVE_PROLOGUE();
    CONVERT(family, toCIntOfFormat(globalObject, args[0]));
    CONVERT(text, toCStringOfFormat(globalObject, args[1], "inet_pton"_s, "argument 2"_s));
    std::array<uint8_t, std::max(sizeof(struct in_addr), sizeof(struct in6_addr))> packed;
    int result = inet_pton(family, text.data(), packed.data());
    if (result < 0)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    if (!result)
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "illegal IP address string passed to inet_pton"_s));
    if (family == AF_INET)
        return JSValue::encode(newBytes(globalObject, std::span(packed).first(sizeof(struct in_addr))));
    if (family == AF_INET6)
        return JSValue::encode(newBytes(globalObject, std::span(packed).first(sizeof(struct in6_addr))));
    return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "unknown address family"_s));
}

// inet_ntop(af, packed_ip)
PYTHON_NATIVE(socketInetNToP)
{
    NATIVE_PROLOGUE();
    CONVERT(family, toCIntOfFormat(globalObject, args[0]));
    Buffer buffer = bufferOf(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto span = buffer.span();
    if (family != AF_INET && family != AF_INET6)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("unknown address family "_s, family)));
    if (span.size() != (family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr)))
        return JSValue::encode(raiseValueError(globalObject, scope, "invalid length of packed IP address string"_s));
    char text[std::max(INET_ADDRSTRLEN, INET6_ADDRSTRLEN)];
    if (!inet_ntop(family, span.data(), text, sizeof(text)))
        return JSValue::encode(raiseSocketError(globalObject, scope));
    return JSValue::encode(jsString(vm, String::fromLatin1(text)));
}

// ---- Looking things up

// getaddrinfo(host, port, family=AF_UNSPEC, type=0, proto=0, flags=0)
PYTHON_NATIVE(socketGetAddressInfo)
{
    NATIVE_PROLOGUE();
    JSValue givenHost = args.at(0);
    JSValue givenPort = args.at(1);
    CONVERT_INT_OR(family, args.at(2), AF_UNSPEC);
    CONVERT_INT_OR(type, args.at(3), 0);
    CONVERT_INT_OR(protocol, args.at(4), 0);
    CONVERT_INT_OR(flags, args.at(5), 0);

    CString host;
    if (isNone(givenHost)) {
        // Null
    } else if (stringIn(givenHost)) {
        auto encoded = encodeString(globalObject, givenHost, "idna"_s, String());
        RETURN_IF_EXCEPTION(scope, { });
        host = CString(byteCast<char>(encoded->span()));
    } else if (isBytes(givenHost)) {
        Buffer buffer = bufferOf(globalObject, givenHost);
        RETURN_IF_EXCEPTION(scope, { });
        host = CString(byteCast<char>(buffer.span()));
    } else
        return JSValue::encode(raiseTypeError(globalObject, scope, "getaddrinfo() argument 1 must be string or None"_s));

    CString port;
    if (typeOf(globalObject, givenPort)->lookup(vm, names.dunder_index)) {
        JSValue number = toInt(globalObject, givenPort);
        RETURN_IF_EXCEPTION(scope, { });
        String text = str(globalObject, number);
        RETURN_IF_EXCEPTION(scope, { });
        port = text.utf8();
    } else if (stringIn(givenPort)) {
        auto encoded = encodeString(globalObject, givenPort, "utf-8"_s, String());
        RETURN_IF_EXCEPTION(scope, { });
        port = CString(byteCast<char>(encoded->span()));
    } else if (isBytes(givenPort)) {
        Buffer buffer = bufferOf(globalObject, givenPort);
        RETURN_IF_EXCEPTION(scope, { });
        port = CString(byteCast<char>(buffer.span()));
    } else if (!isNone(givenPort))
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "Int or String expected"_s));
#if OS(DARWIN)
    // The system's has crashed when it was told that the service is a number and was given none, or "0".
    if ((flags & AI_NUMERICSERV) && (port.isNull() || !strcmp(port.data(), "0")))
        port = CString("00"_span);
#endif
    if (!audit(globalObject, "socket.getaddrinfo"_s, givenHost, givenPort, jsNumber(family), jsNumber(type), jsNumber(protocol)))
        return { };

    struct addrinfo hints;
    zeroBytes(hints);
    hints.ai_family = family;
    hints.ai_socktype = type;
    hints.ai_protocol = protocol;
    hints.ai_flags = flags;
    struct addrinfo* found = nullptr;
    if (int error = getaddrinfo(host.data(), port.data(), &hints, &found))
        return JSValue::encode(raiseAddressInfoError(globalObject, scope, error));
    auto freeFound = makeScopeExit([&] { freeaddrinfo(found); });

    JSArray* all = newList(globalObject);
    for (auto* entry = found; entry; entry = entry->ai_next) {
        JSValue address = makeSocketAddress(globalObject, -1, entry->ai_addr, entry->ai_addrlen, protocol);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue canonicalName = stringFromUTF8(globalObject, entry->ai_canonname ? entry->ai_canonname : "");
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, all, PyTuple::create(globalObject, { jsNumber(entry->ai_family), jsNumber(entry->ai_socktype), jsNumber(entry->ai_protocol), canonicalName, address }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(all);
}

// getnameinfo(sockaddr, flags)
PYTHON_NATIVE(socketGetNameInfo)
{
    NATIVE_PROLOGUE();
    JSValue given = args[0];
    CONVERT(flags, toCIntOfFormat(globalObject, args[1]));
    if (!isTuple(given))
        return JSValue::encode(raiseTypeError(globalObject, scope, "getnameinfo() argument 1 must be a tuple"_s));
    // "si|II;getnameinfo(): illegal sockaddr argument"
    PyTuple* tuple = asTuple(given);
    constexpr auto illegal = "getnameinfo(): illegal sockaddr argument"_s;
    if (tuple->length() < 2 || tuple->length() > 4 || !stringIn(tuple->at(0)))
        return JSValue::encode(raiseTypeError(globalObject, scope, illegal));
    CONVERT(host, toCStringOfFormat(globalObject, tuple->at(0), "getnameinfo"_s, "argument 1"_s));
    CONVERT(port, toCIntOfFormat(globalObject, tuple->at(1)));
    unsigned flowInfo = 0;
    unsigned scopeID = 0;
    if (tuple->length() > 2) {
        CONVERT(converted, toCUnsignedIntOfFormat(globalObject, tuple->at(2)));
        flowInfo = converted;
    }
    if (tuple->length() > 3) {
        CONVERT(converted, toCUnsignedIntOfFormat(globalObject, tuple->at(3)));
        scopeID = converted;
    }
    if (flowInfo > 0xfffff)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "getnameinfo(): flowinfo must be 0-1048575."_s));
    if (!audit(globalObject, "socket.getnameinfo"_s, given))
        return { };

    char service[NI_MAXSERV];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints;
    zeroBytes(hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM; // So that a port that is a number will do
    hints.ai_flags = AI_NUMERICHOST; // Nothing is looked up.
    struct addrinfo* found = nullptr;
    if (int error = getaddrinfo(host.data(), service, &hints, &found))
        return JSValue::encode(raiseAddressInfoError(globalObject, scope, error));
    auto freeFound = makeScopeExit([&] { freeaddrinfo(found); });
    if (found->ai_next)
        return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "sockaddr resolved to multiple addresses"_s));
    switch (found->ai_family) {
    case AF_INET:
        if (tuple->length() != 2)
            return JSValue::encode(raiseOSErrorSaying(globalObject, scope, "IPv4 sockaddr must be 2 tuple"_s));
        break;
    case AF_INET6: {
        auto* address = std::bit_cast<struct sockaddr_in6*>(found->ai_addr);
        address->sin6_flowinfo = htonl(flowInfo);
        address->sin6_scope_id = scopeID;
        break;
    }
    }
    char name[NI_MAXHOST];
    if (int error = getnameinfo(found->ai_addr, found->ai_addrlen, name, sizeof(name), service, sizeof(service), flags))
        return JSValue::encode(raiseAddressInfoError(globalObject, scope, error));
    JSValue hostName = stringFromUTF8(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue serviceName = stringFromUTF8(globalObject, service);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { hostName, serviceName }));
}

// ---- How long a new socket waits

PYTHON_NATIVE(socketGetDefaultTimeout)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    UNUSED_PARAM(scope);
    int64_t timeout = socketModuleState(globalObject).defaultTimeout;
    return JSValue::encode(timeout < 0 ? jsUndefined() : floatFromDouble(timeAsSeconds(timeout)));
}

PYTHON_NATIVE(socketSetDefaultTimeout)
{
    NATIVE_PROLOGUE();
    CONVERT(timeout, parseSocketTimeout(globalObject, args[0]));
    socketModuleState(globalObject).defaultTimeout = timeout;
    RETURN_NONE();
}

// ---- Interfaces

PYTHON_NATIVE(socketInterfaceNameIndex)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    struct if_nameindex* interfaces = if_nameindex();
    if (!interfaces)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    auto freeInterfaces = makeScopeExit([&] { if_freenameindex(interfaces); });
    JSArray* list = newList(globalObject);
    for (auto* entry = interfaces; entry->if_index; ++entry) {
        JSValue name = decodeFileSystemBytes(globalObject, unsafeSpan(entry->if_name));
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, PyTuple::create(globalObject, { intFromUInt64(globalObject, entry->if_index), name }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(list);
}

PYTHON_NATIVE(socketInterfaceNameToIndex)
{
    NATIVE_PROLOGUE();
    CONVERT(name, toFileSystemEncoded(globalObject, args[0]));
    // In case it does not say
    errno = ENODEV;
    unsigned index = if_nametoindex(name.data());
    if (!index)
        return JSValue::encode(raiseSocketError(globalObject, scope));
    return JSValue::encode(intFromUInt64(globalObject, index));
}

PYTHON_NATIVE(socketInterfaceIndexToName)
{
    NATIVE_PROLOGUE();
    CONVERT(index, toUnsigned<unsigned>(globalObject, args[0], "unsigned int"_s));
    errno = ENXIO;
    char name[IF_NAMESIZE + 1];
    if (!if_indextoname(index, name))
        return JSValue::encode(raiseSocketError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, unsafeSpan(name))));
}

// ---- How much room what goes along with what is sent takes

// CMSG_LEN(length) and CMSG_SPACE(length)
PYTHON_NATIVE(socketControlMessageSize)
{
    NATIVE_PROLOGUE();
    bool isSpace = unpack<bool>(callFrame, 0);
    CONVERT(length, toSsize(globalObject, args[0]));
    std::optional<size_t> result;
    if (length >= 0)
        result = isSpace ? controlMessageSpace(length) : controlMessageLength(length);
    if (!result)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, isSpace ? "CMSG_SPACE() argument out of range"_s : "CMSG_LEN() argument out of range"_s));
    return JSValue::encode(intFromUInt64(globalObject, *result));
}

// ---- The module

// Kinds of address that there is nothing here to read or write yet. In CPython each is there only if its header was found when it was built, so a program has to be ready for it not to be.
#undef AF_BLUETOOTH
#undef AF_DIVERT
#undef PF_DIVERT

JSObject* createSocketModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    using Arguments = PyNativeFunction::Arguments;
    constexpr auto byParseTuple = Arguments::AreCheckedAsByParseTuple;
    auto& state = socketModuleState(globalObject);
    if (!state.socketType) {
        // PyErr_NewException("socket.herror", PyExc_OSError, NULL), and the other
        auto newError = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name) {
            PyDict* contents = PyDict::create(globalObject);
            contents->setString(globalObject, "__module__"_s, jsNontrivialString(vm, "socket"_s));
            JSValue error = newType(globalObject, realm->typeType(), jsNontrivialString(vm, name), PyTuple::create(globalObject, { realm->type(BuiltinType::OSError) }), contents, nullptr);
            RETURN_IF_EXCEPTION(scope, void());
            slot.set(vm, realm, asType(error));
        };
        newError(state.hostError, "herror"_s);
        RETURN_IF_EXCEPTION(scope, nullptr);
        newError(state.addressInfoError, "gaierror"_s);
        RETURN_IF_EXCEPTION(scope, nullptr);
        initializeSocketType(globalObject, state);
    }

    JSObject* module = newBuiltinModule(globalObject, "_socket"_s);
    // Few of them say what they take in a way that anything but a person can read, so that is said here.
    addFunction(globalObject, module, "gethostbyname"_s, socketGetHostByName, 0, "($module, host, /)"_s, byParseTuple);
    addFunction(globalObject, module, "gethostbyname_ex"_s, socketGetHostByNameEx, 0, "($module, host, /)"_s, byParseTuple);
    addFunction(globalObject, module, "gethostbyaddr"_s, socketGetHostByAddress, 0, "($module, host, /)"_s, byParseTuple);
    addFunction(globalObject, module, "gethostname"_s, socketGetHostName, 0, "($module, /)"_s);
    addFunction(globalObject, module, "sethostname"_s, socketSetHostName, 0, "($module, name, /)"_s, byParseTuple);
    addFunction(globalObject, module, "getservbyname"_s, socketGetServiceByName, 0, "($module, servicename, protocolname=None, /)"_s, byParseTuple);
    addFunction(globalObject, module, "getservbyport"_s, socketGetServiceByPort, 0, "($module, port, protocolname=None, /)"_s, byParseTuple);
    addFunction(globalObject, module, "getprotobyname"_s, socketGetProtocolByName, 0, "($module, name, /)"_s, byParseTuple);
    addFunction(globalObject, module, "close"_s, socketCloseDescriptor, 0, "($module, integer, /)"_s);
    addFunction(globalObject, module, "dup"_s, socketDuplicate, 0, "($module, integer, /)"_s);
    addFunction(globalObject, module, "socketpair"_s, socketPair, 0, "($module, family=1, type=1, proto=0, /)"_s, byParseTuple);
    addFunction(globalObject, module, "ntohs"_s, socketByteOrder, pack(ByteOrder::NetworkToHostShort));
    addFunction(globalObject, module, "ntohl"_s, socketByteOrder, pack(ByteOrder::NetworkToHostLong));
    addFunction(globalObject, module, "htons"_s, socketByteOrder, pack(ByteOrder::HostToNetworkShort));
    addFunction(globalObject, module, "htonl"_s, socketByteOrder, pack(ByteOrder::HostToNetworkLong));
    addFunction(globalObject, module, "inet_aton"_s, socketInetAToN);
    addFunction(globalObject, module, "inet_ntoa"_s, socketInetNToA);
    addFunction(globalObject, module, "inet_pton"_s, socketInetPToN, 0, "($module, af, ip, /)"_s, byParseTuple);
    addFunction(globalObject, module, "inet_ntop"_s, socketInetNToP, 0, "($module, af, packed_ip, /)"_s, byParseTuple);
    addFunction(globalObject, module, "getaddrinfo"_s, socketGetAddressInfo, 0, "($module, /, host, port, family=0, type=0, proto=0, flags=0)"_s);
    addFunction(globalObject, module, "getnameinfo"_s, socketGetNameInfo, 0, "($module, sockaddr, flags, /)"_s, byParseTuple);
    addFunction(globalObject, module, "getdefaulttimeout"_s, socketGetDefaultTimeout, 0, "($module, /)"_s);
    addFunction(globalObject, module, "setdefaulttimeout"_s, socketSetDefaultTimeout, 0, "($module, timeout, /)"_s);
    addFunction(globalObject, module, "if_nameindex"_s, socketInterfaceNameIndex, 0, "($module, /)"_s);
    addFunction(globalObject, module, "if_nametoindex"_s, socketInterfaceNameToIndex);
    addFunction(globalObject, module, "if_indextoname"_s, socketInterfaceIndexToName);
    addFunction(globalObject, module, "CMSG_LEN"_s, socketControlMessageSize, pack(false), "($module, length, /)"_s, byParseTuple);
    addFunction(globalObject, module, "CMSG_SPACE"_s, socketControlMessageSize, pack(true), "($module, length, /)"_s, byParseTuple);

    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    add("herror"_s, state.hostError->object());
    add("gaierror"_s, state.addressInfoError->object());
    add("error"_s, realm->type(BuiltinType::OSError)->object());
    add("timeout"_s, realm->type(BuiltinType::TimeoutError)->object());
    add("SocketType"_s, state.socketType->object());
    add("socket"_s, state.socketType->object());
    add("has_ipv6"_s, jsBoolean(true));
    // What modules written in C get at this one by. There are none, and it is here to be seen.
    add("CAPI"_s, newCapsule(globalObject, "_socket.CAPI"_s));

    // PyModule_AddIntConstant() takes a long.
#define ADD_INT_MACRO(m, name) add(#name ""_s, intFromInt64(globalObject, static_cast<long>(name)))
#define ADD_INT_CONST(m, name, value) add(name ""_s, intFromInt64(globalObject, static_cast<long>(value)))
#define ADD_STR_CONST(m, name, value) add(name ""_s, jsNontrivialString(vm, value ""_s))
#include "PythonSocketConstants.h"
#undef ADD_INT_MACRO
#undef ADD_INT_CONST
#undef ADD_STR_CONST
    return module;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
