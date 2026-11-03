/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyEventWait.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyPosix.h"
#include "PySocket.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#if OS(LINUX)
#include <linux/netlink.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _socket. (Modules/socketmodule.c in CPython is the reference.)
//
// Addresses are understood for AF_INET, AF_INET6, AF_UNIX and AF_NETLINK. A socket of another family can be made, and used
// as far as no address is involved.

void addSocketConstants(JSGlobalObject*, PyModule*); // PySocketConstants.cpp

static constexpr int64_t socketNanosecondsPerSecond = 1000000000;

static int socketFamilyOf(PyRecord* socket) { return socket->get(SocketFamily).asInt32(); }

// ---- Errors ----

static constexpr TypeDef socketHostErrorTypeDef { "socket.herror"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };
static constexpr TypeDef socketAddressInfoErrorTypeDef { "socket.gaierror"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };

static PyType* socketHostErrorType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(socketHostErrorTypeDef, rt.OSError());
}

static PyType* socketAddressInfoErrorType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(socketAddressInfoErrorTypeDef, rt.OSError());
}

static JSValue socketRaiseHostError(JSGlobalObject* globalObject, int error)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseWithArgs(globalObject, scope, socketHostErrorType(globalObject), { jsNumber(error), posixDecode(globalObject, hstrerror(error)) });
}

static JSValue socketRaiseAddressInfoError(JSGlobalObject* globalObject, int error)
{
    if (error == EAI_SYSTEM)
        return posixError(globalObject);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseWithArgs(globalObject, scope, socketAddressInfoErrorType(globalObject), { jsNumber(error), posixDecode(globalObject, gai_strerror(error)) });
}

// An OSError that no system call is behind.
static JSValue socketRaiseOSError(JSGlobalObject* globalObject, const String& message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).OSError(), message);
}

static JSValue socketRaiseMemoryError(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).MemoryError(), String());
}

// ---- Arguments ----
//
// Most of these functions have no signature in CPython: they take their arguments apart with PyArg_ParseTuple(), whose
// complaints are worded differently from those of the functions that have one. Here they take *args, and these do the same.

static bool socketCheckCount(JSGlobalObject* globalObject, const CallArgs& args, ASCIILiteral qualifier, ASCIILiteral name, unsigned min, unsigned max)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.keywordCount()) {
        raiseTypeError(globalObject, scope, qualifier, name, "() takes no keyword arguments"_s);
        return false;
    }
    unsigned count = args.size();
    if (count >= min && count <= max)
        return true;
    unsigned limit = count < min ? min : max;
    raiseTypeError(globalObject, scope, name, "() takes "_s, min == max ? "exactly "_s : count < min ? "at least "_s : "at most "_s, limit, limit == 1 ? " argument ("_s : " arguments ("_s, count, " given)"_s);
    return false;
}

// The format "i".
static bool socketToInt(JSGlobalObject* globalObject, JSValue value, int& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted)
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
    else if (*converted > INT_MAX)
        raiseOverflowError(globalObject, scope, "signed integer is greater than maximum"_s);
    else if (*converted < INT_MIN)
        raiseOverflowError(globalObject, scope, "signed integer is less than minimum"_s);
    else {
        result = static_cast<int>(*converted);
        return true;
    }
    return false;
}

// A descriptor given as an int. What does not fit is cut off, as a cast does.
static bool socketToDescriptor(JSGlobalObject* globalObject, JSValue value, int& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t number;
    if (failed(scope, posixToLong(globalObject, value, number)))
        return false;
    result = static_cast<int>(number);
    return true;
}

template<typename Integer>
static bool socketToUnsigned(JSGlobalObject* globalObject, JSValue value, ASCIILiteral typeName, Integer& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted || *converted > std::numeric_limits<Integer>::max()) {
        raiseOverflowError(globalObject, scope, "Python int too large for C "_s, typeName);
        return false;
    }
    result = static_cast<Integer>(*converted);
    return true;
}

static bool socketEncodeUTF8(JSGlobalObject* globalObject, JSString* string, PosixBytes& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (auto encoded = tryEncodeUTF8(text)) {
        result.assign(byteCast<char>(encoded->span()));
        return true;
    }
    // Which raises.
    strEncode(globalObject, string, makeStr(vm, "utf-8"_s), makeStr(vm, "strict"_s));
    return false;
}

// The format "s". `position` 0: it is the only argument.
static bool socketToCString(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, unsigned position, PosixBytes& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        if (position)
            raiseTypeError(globalObject, scope, function, "() argument "_s, position, " must be str, not "_s, posixTypeNameOfArgument(globalObject, value));
        else
            raiseTypeError(globalObject, scope, function, "() argument must be str, not "_s, posixTypeNameOfArgument(globalObject, value));
        return false;
    }
    if (!socketEncodeUTF8(globalObject, strValue(value), result))
        return false;
    if (result.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    return true;
}

// str.encode('idna'). The codec is written in Python. A name that is ASCII it only checks: no label but the last may be empty,
// and none may be longer than 63. There is no need to import it for "localhost".
static bool socketEncodeIDNA(JSGlobalObject* globalObject, JSString* string, PosixBytes& result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (text.containsOnlyASCII()) {
        for (bool looksForEmpty : { true, false }) {
            unsigned labelStart = 0;
            for (unsigned i = 0; i <= text.length(); ++i) {
                if (i < text.length() && text[i] != '.')
                    continue;
                unsigned labelLength = i - labelStart;
                if (looksForEmpty ? !labelLength && i < text.length() : labelLength >= 64) {
                    raiseWithArgs(globalObject, scope, rt.UnicodeEncodeError(), { makeStr(vm, "idna"_s), string, jsNumber(labelStart), jsNumber(labelStart + std::max(labelLength, 1u)),
                        makeStr(vm, looksForEmpty ? "label empty"_s : "label too long"_s) });
                    return false;
                }
                labelStart = i + 1;
            }
        }
        CString ascii = text.ascii();
        result.assign(ascii.span());
        return true;
    }
    JSValue encoded = strEncode(globalObject, string, makeStr(vm, "idna"_s), makeStr(vm, "strict"_s));
    RETURN_IF_EXCEPTION(scope, false);
    result.assign(byteCast<char>(PyBytes::from(encoded)->span()));
    return true;
}

// The format "et" with "idna": the name of a host, as the only argument of a function.
static bool socketToHostName(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, PosixBytes& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* bytes = PyBytes::tryFrom(value))
        result.assign(byteCast<char>(bytes->span()));
    else if (auto* array = PyByteArray::tryFrom(value))
        result.assign(byteCast<char>(array->span()));
    else if (isStr(value)) {
        if (!socketEncodeIDNA(globalObject, strValue(value), result))
            return false;
    } else {
        raiseTypeError(globalObject, scope, function, "() argument 1 must be str, bytes or bytearray, not "_s, posixTypeNameOfArgument(globalObject, value));
        return false;
    }
    if (result.contains('\0')) {
        raiseTypeError(globalObject, scope, function, "() argument 1 must be encoded string without null bytes, not "_s, posixTypeNameOfArgument(globalObject, value));
        return false;
    }
    return true;
}

// The host in an address.
static bool socketToHostInAddress(JSGlobalObject* globalObject, JSValue value, PosixBytes& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* bytes = PyBytes::tryFrom(value))
        result.assign(byteCast<char>(bytes->span()));
    else if (auto* array = PyByteArray::tryFrom(value))
        result.assign(byteCast<char>(array->span()));
    else if (isStr(value)) {
        String text = strValue(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (text.containsOnlyASCII()) {
            CString ascii = text.ascii();
            result.assign(ascii.span());
        } else {
            JSValue encoded = strEncode(globalObject, strValue(value), makeStr(vm, "idna"_s), makeStr(vm, "strict"_s));
            if (scope.exception()) [[unlikely]] {
                takePendingException(globalObject);
                raiseTypeError(globalObject, scope, "encoding of hostname failed"_s);
                return false;
            }
            result.assign(byteCast<char>(PyBytes::from(encoded)->span()));
        }
    } else {
        raiseTypeError(globalObject, scope, "str, bytes or bytearray expected, not "_s, typeName(globalObject, value));
        return false;
    }
    if (result.contains('\0')) {
        raiseTypeError(globalObject, scope, "host name must not contain null character"_s);
        return false;
    }
    return true;
}

// None: -1.
static bool socketParseTimeout(JSGlobalObject* globalObject, JSValue value, int64_t& timeout)
{
    timeout = -1;
    if (isNone(value))
        return true;
    if (!timeoutFromObject(globalObject, value, socketNanosecondsPerSecond, timeout, false))
        return false;
    if (timeout < 0) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseValueError(globalObject, scope, "Timeout value out of range"_s);
        return false;
    }
    return true;
}

static JSValue socketTimeoutObject(JSGlobalObject* globalObject, int64_t timeout)
{
    if (timeout < 0)
        return jsUndefined();
    if (!(timeout % socketNanosecondsPerSecond))
        return makeFloat(globalObject, static_cast<double>(timeout / socketNanosecondsPerSecond));
    return makeFloat(globalObject, static_cast<double>(timeout) / 1e9);
}

// The items of any iterable. Whatever goes wrong, the complaint is that it is not one.
static bool socketItems(JSGlobalObject* globalObject, JSValue iterable, ASCIILiteral complaint, MarkedArgumentBuffer& items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* tuple = tupleFromIterable(globalObject, iterable);
    if (scope.exception()) [[unlikely]] {
        takePendingException(globalObject);
        raiseTypeError(globalObject, scope, complaint);
        return false;
    }
    for (unsigned i = 0; i < tuple->length(); ++i)
        items.append(tuple->at(i));
    return true;
}

// Several buffers at once, for sendmsg() and recvmsg_into().
class SocketBufferList {
    WTF_MAKE_NONCOPYABLE(SocketBufferList);
public:
    explicit SocketBufferList(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
    {
    }

    ~SocketBufferList()
    {
        for (auto& buffer : m_buffers)
            releaseBuffer(m_globalObject, buffer);
    }

    bool add(JSValue object, unsigned flags)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        Buffer buffer;
        if (failed(scope, getBuffer(m_globalObject, object, buffer, flags)))
            return false;
        // The vector is not where the collector looks.
        m_values.append(buffer.object);
        m_values.append(buffer.view ? buffer.view : jsUndefined());
        m_values.append(buffer.owner ? JSValue(buffer.owner) : jsUndefined());
        m_buffers.append(WTF::move(buffer));
        return true;
    }

    // To be called after anything that may have run code. False: an exception.
    bool fillVectors()
    {
        m_vectors.shrink(0);
        for (auto& buffer : m_buffers) {
            if (!refreshBuffer(m_globalObject, buffer))
                return false;
            m_vectors.append({ buffer.data, static_cast<size_t>(buffer.length) });
        }
        return true;
    }

    size_t size() const { return m_buffers.size(); }
    const Buffer& at(size_t index) const { return m_buffers[index]; }
    struct iovec* vectors() { return m_vectors.mutableSpan().data(); }

private:
    JSGlobalObject* m_globalObject;
    Vector<Buffer, 4> m_buffers;
    Vector<struct iovec, 4> m_vectors;
    MarkedArgumentBuffer m_values;
};

// ---- Addresses ----

struct SocketAddress {
    union {
        struct sockaddr generic;
        struct sockaddr_in inet;
        struct sockaddr_in6 inet6;
        struct sockaddr_un local;
#if OS(LINUX)
        struct sockaddr_nl netlink;
#endif
        struct sockaddr_storage storage;
    };
    socklen_t length { 0 };

    SocketAddress() { memset(&storage, 0, sizeof(storage)); }
};

static void socketSetLength(struct sockaddr* address, size_t length)
{
#if OS(DARWIN) || OS(FREEBSD) || OS(NETBSD) || OS(OPENBSD)
    address->sa_len = static_cast<uint8_t>(length);
#else
    UNUSED_PARAM(address);
    UNUSED_PARAM(length);
#endif
}

// Turns the name of a host, or an address written out, into an address of the family (AF_UNSPEC: of either).
// "" is any address, "<broadcast>" the one that all hosts listen to. False: an exception.
static bool socketResolve(JSGlobalObject* globalObject, const char* name, SocketAddress& result, int family)
{
    memset(&result.storage, 0, sizeof(result.storage));
    struct addrinfo hints { };
    struct addrinfo* found = nullptr;
    hints.ai_family = family;
    if (!name[0]) {
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;
        if (int error = getaddrinfo(nullptr, "0", &hints, &found)) {
            socketRaiseAddressInfoError(globalObject, error);
            return false;
        }
        ASCIILiteral complaint;
        if (found->ai_family != AF_INET && found->ai_family != AF_INET6)
            complaint = "unsupported address family"_s;
        else if (found->ai_next)
            complaint = "wildcard resolved to multiple address"_s;
        else
            memcpy(&result.storage, found->ai_addr, std::min<size_t>(found->ai_addrlen, sizeof(result.storage)));
        freeaddrinfo(found);
        if (!complaint.isNull()) {
            socketRaiseOSError(globalObject, complaint);
            return false;
        }
        return true;
    }
    if (!strcmp(name, "255.255.255.255") || !strcmp(name, "<broadcast>")) {
        if (family != AF_INET && family != AF_UNSPEC) {
            socketRaiseOSError(globalObject, "address family mismatched"_s);
            return false;
        }
        result.inet.sin_family = AF_INET;
        socketSetLength(&result.generic, sizeof(result.inet));
        result.inet.sin_addr.s_addr = INADDR_BROADCAST;
        return true;
    }
    // An address written out needs no resolver.
    if ((family == AF_UNSPEC || family == AF_INET) && inet_pton(AF_INET, name, &result.inet.sin_addr) > 0) {
        result.inet.sin_family = AF_INET;
        socketSetLength(&result.generic, sizeof(result.inet));
        return true;
    }
    memset(&result.storage, 0, sizeof(result.storage));
    // With a scope, it does: that may be the name of an interface.
    if ((family == AF_UNSPEC || family == AF_INET6) && !strchr(name, '%') && inet_pton(AF_INET6, name, &result.inet6.sin6_addr) > 0) {
        result.inet6.sin6_family = AF_INET6;
        socketSetLength(&result.generic, sizeof(result.inet6));
        return true;
    }
    memset(&result.storage, 0, sizeof(result.storage));
    int error;
    {
        AllowThreads allow(globalObject);
        error = getaddrinfo(name, nullptr, &hints, &found);
    }
    if (error) {
        socketRaiseAddressInfoError(globalObject, error);
        return false;
    }
    memcpy(&result.storage, found->ai_addr, std::min<size_t>(found->ai_addrlen, sizeof(result.storage)));
    freeaddrinfo(found);
    if (result.generic.sa_family != AF_INET && result.generic.sa_family != AF_INET6) {
        socketRaiseOSError(globalObject, "unknown address family"_s);
        return false;
    }
    return true;
}

static JSValue socketFormatIP(JSGlobalObject* globalObject, int family, const void* address)
{
    char text[INET6_ADDRSTRLEN];
    if (!inet_ntop(family, address, text, sizeof(text)))
        return posixError(globalObject);
    return makeStr(globalObject, String::fromLatin1(text));
}

// The address as Python code sees it.
static JSValue socketAddressObject(JSGlobalObject* globalObject, const SocketAddress& address, size_t length)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!length)
        return jsUndefined();
    // The system reports how long the address is, even if that is more than there was room for.
    length = std::min(length, sizeof(address.storage));
    switch (address.generic.sa_family) {
    case AF_INET: {
        JSValue host = socketFormatIP(globalObject, AF_INET, &address.inet.sin_addr);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { host, jsNumber(ntohs(address.inet.sin_port)) }));
    }
    case AF_INET6: {
        JSValue host = socketFormatIP(globalObject, AF_INET6, &address.inet6.sin6_addr);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { host, jsNumber(ntohs(address.inet6.sin6_port)), jsNumber(ntohl(address.inet6.sin6_flowinfo)), jsNumber(address.inet6.sin6_scope_id) }));
    }
    case AF_UNIX: {
        constexpr size_t pathOffset = offsetof(struct sockaddr_un, sun_path);
        const char* path = address.local.sun_path;
#if OS(LINUX)
        // The abstract namespace: the name is all the bytes, of which the first is 0.
        if (length > pathOffset && !path[0])
            RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, std::span<const char> { path, length - pathOffset }));
#endif
        RELEASE_AND_RETURN(scope, posixDecode(globalObject, std::span<const char> { path, strnlen(path, sizeof(address.storage) - pathOffset) }));
    }
#if OS(LINUX)
    case AF_NETLINK:
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { jsNumber(address.netlink.nl_pid), jsNumber(address.netlink.nl_groups) }));
#endif
    default:
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { jsNumber(address.generic.sa_family), PyBytes::create(globalObject, std::span<const char> { address.generic.sa_data, sizeof(address.generic.sa_data) }) }));
    }
}

// (host, port), and for IPv6 also (host, port, flowinfo, scope_id).
static bool socketParseInternetAddress(JSGlobalObject* globalObject, int family, JSValue value, SocketAddress& result, ASCIILiteral caller)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isIPv6 = family == AF_INET6;
    auto* tuple = PyTuple::tryFrom(value);
    if (!tuple) {
        raiseTypeError(globalObject, scope, caller, isIPv6 ? "(): AF_INET6 address must be tuple, not "_s : "(): AF_INET address must be tuple, not "_s, typeName(globalObject, value));
        return false;
    }
    if (tuple->length() < 2 || tuple->length() > (isIPv6 ? 4u : 2u)) {
        raiseTypeError(globalObject, scope, isIPv6 ? "AF_INET6 address must be a tuple (host, port[, flowinfo[, scopeid]])"_s : "AF_INET address must be a pair (host, port)"_s);
        return false;
    }
    auto portOutOfRange = [&] {
        raiseOverflowError(globalObject, scope, caller, "(): port must be 0-65535."_s);
        return false;
    };
    PosixBytes host;
    int port;
    unsigned flowInfo = 0;
    unsigned scopeId = 0;
    if (failed(scope, socketToHostInAddress(globalObject, tuple->at(0), host)))
        return false;
    if (!socketToInt(globalObject, tuple->at(1), port)
        || (tuple->length() > 2 && !posixToUnsignedMask(globalObject, tuple->at(2), flowInfo))
        || (tuple->length() > 3 && !posixToUnsignedMask(globalObject, tuple->at(3), scopeId))) {
        if (clearPendingExceptionIfMatches(globalObject, rt.OverflowError()))
            return portOutOfRange();
        return false;
    }
    if (failed(scope, socketResolve(globalObject, host.characters(), result, isIPv6 ? AF_INET6 : AF_INET)))
        return false;
    if (port < 0 || port > 0xffff)
        return portOutOfRange();
    if (!isIPv6) {
        result.inet.sin_family = AF_INET;
        result.inet.sin_port = htons(static_cast<uint16_t>(port));
        result.length = sizeof(result.inet);
        return true;
    }
    if (flowInfo > 0xfffff) {
        raiseOverflowError(globalObject, scope, caller, "(): flowinfo must be 0-1048575."_s);
        return false;
    }
    result.inet6.sin6_family = AF_INET6;
    result.inet6.sin6_port = htons(static_cast<uint16_t>(port));
    result.inet6.sin6_flowinfo = htonl(flowInfo);
    result.inet6.sin6_scope_id = scopeId;
    result.length = sizeof(result.inet6);
    return true;
}

static bool socketParseLocalAddress(JSGlobalObject* globalObject, JSValue value, SocketAddress& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes encoded;
    BufferHolder holder(globalObject);
    std::span<const char> path;
    // Not os.fsencode(): on Linux there may be NULs in it.
    if (isStr(value)) {
        if (failed(scope, posixEncodeStr(globalObject, strValue(value), encoded)))
            return false;
        path = encoded.span();
    } else {
        if (!holder.get(value))
            return false;
        path = byteCast<char>(holder.buffer.span());
    }
    constexpr size_t pathOffset = offsetof(struct sockaddr_un, sun_path);
    constexpr size_t capacity = sizeof(result.local.sun_path);
    bool isTerminated = true;
#if OS(LINUX)
    // The abstract namespace. An empty address asks for a name in it to be made up.
    if (path.empty() || !path[0])
        isTerminated = false;
#endif
    if (path.size() + isTerminated > capacity) {
        socketRaiseOSError(globalObject, "AF_UNIX path too long"_s);
        return false;
    }
    result.local.sun_family = AF_UNIX;
    if (!path.empty())
        memcpy(result.local.sun_path, path.data(), path.size());
    result.length = static_cast<socklen_t>(pathOffset + path.size() + isTerminated);
    socketSetLength(&result.generic, result.length);
    return true;
}

// An address given to a method of the socket, which is of its family. `caller` is the method.
static bool socketParseAddress(JSGlobalObject* globalObject, PyRecord* socket, JSValue value, SocketAddress& result, ASCIILiteral caller)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int family = socketFamilyOf(socket);
    switch (family) {
    case AF_UNIX:
        RELEASE_AND_RETURN(scope, socketParseLocalAddress(globalObject, value, result));
    case AF_INET:
    case AF_INET6:
        RELEASE_AND_RETURN(scope, socketParseInternetAddress(globalObject, family, value, result, caller));
#if OS(LINUX)
    case AF_NETLINK: {
        auto* tuple = PyTuple::tryFrom(value);
        if (!tuple) {
            raiseTypeError(globalObject, scope, caller, "(): AF_NETLINK address must be tuple, not "_s, typeName(globalObject, value));
            return false;
        }
        if (tuple->length() != 2) {
            raiseTypeError(globalObject, scope, "AF_NETLINK address must be a pair (pid, groups)"_s);
            return false;
        }
        unsigned pid;
        unsigned groups;
        if (!posixToUnsignedMask(globalObject, tuple->at(0), pid) || !posixToUnsignedMask(globalObject, tuple->at(1), groups))
            return false;
        result.netlink.nl_family = AF_NETLINK;
        result.netlink.nl_pid = pid;
        result.netlink.nl_groups = groups;
        result.length = sizeof(result.netlink);
        return true;
    }
#endif
    default:
        socketRaiseOSError(globalObject, makeString(caller, "(): bad family"_s));
        return false;
    }
}

// How much room an address of the socket's family takes.
static bool socketAddressLength(JSGlobalObject* globalObject, PyRecord* socket, socklen_t& length)
{
    switch (socketFamilyOf(socket)) {
    case AF_UNIX:
        length = sizeof(struct sockaddr_un);
        return true;
    case AF_INET:
        length = sizeof(struct sockaddr_in);
        return true;
    case AF_INET6:
        length = sizeof(struct sockaddr_in6);
        return true;
#if OS(LINUX)
    case AF_NETLINK:
        length = sizeof(struct sockaddr_nl);
        return true;
#endif
    default:
        socketRaiseOSError(globalObject, "getsockaddrlen: bad family"_s);
        return false;
    }
}

// ---- Calls that may have to wait ----

static bool socketSetBlocking(JSGlobalObject* globalObject, int fd, bool blocking)
{
    int nonblocking = !blocking;
    if (ioctl(fd, FIONBIO, &nonblocking) < 0) {
        posixError(globalObject);
        return false;
    }
    return true;
}

struct SocketCallOptions {
    bool isConnect { false };
    // The call returns at once whatever the socket is like: MSG_DONTWAIT.
    bool neverBlocks { false };
    // If not null, a failure of the system is stored here and not raised. -1: there is an exception all the same.
    int* error { nullptr };
};

// Calls `function`, which makes a system call on the socket and returns whether that succeeded, leaving the reason in errno.
// (It may also raise, and return false.)
// - If the socket has a timeout, this first waits until the socket can be read from, or written to, for no longer than `timeout`.
// - A call that a signal interrupts is made again, once the handler has run, with the time that is left (PEP 475).
// False: an exception, unless the options say otherwise.
template<typename Function>
static bool socketCall(JSGlobalObject* globalObject, PyRecord* socket, bool isWriting, int64_t timeout, const Function& function, SocketCallOptions options = { })
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasTimeout = timeout > 0;
    bool hasDeadline = false;
    int64_t deadline = 0;
    // A host with an event loop keeps it turning in waitForEvents(), not in recv().
    bool waitsForHost = timeout < 0 && !options.neverBlocks && waitsInHost(globalObject) && !(fcntl(socketDescriptor(socket), F_GETFL) & O_NONBLOCK);

    while (true) {
        // A connection is made in the background even if the socket blocks.
        if ((hasTimeout || options.isConnect || waitsForHost) && socketDescriptor(socket) >= 0) {
            int64_t interval = hasTimeout ? timeout : -1;
            if (hasDeadline)
                interval = deadline - monotonicNanoseconds();
            else if (hasTimeout) {
                hasDeadline = true;
                if (__builtin_add_overflow(monotonicNanoseconds(), timeout, &deadline))
                    deadline = std::numeric_limits<int64_t>::max();
            }
            int ready = 0;
            if (!hasTimeout || interval >= 0) {
                struct pollfd descriptor { socketDescriptor(socket), static_cast<short>(isWriting ? POLLOUT : POLLIN), 0 };
                if (options.isConnect)
                    descriptor.events |= POLLERR;
                WaitRequest request { WaitRequest::Kind::Poll };
                request.timeout = interval;
                request.descriptors = &descriptor;
                request.descriptorCount = 1;
                ready = waitForEvents(globalObject, request);
            }
            if (ready == waitRaised) {
                if (options.error)
                    *options.error = -1;
                return false;
            }
            if (ready < 0) {
                if (options.error)
                    *options.error = errno;
                else
                    posixError(globalObject);
                return false;
            }
            if (!ready) {
                if (options.error)
                    *options.error = EWOULDBLOCK;
                else
                    raise(globalObject, scope, rt.TimeoutError(), "timed out"_s);
                return false;
            }
        }

        int error;
        while (true) {
            if (function()) {
                if (options.error)
                    *options.error = 0;
                return true;
            }
            error = errno;
            if (scope.exception()) [[unlikely]] {
                if (options.error)
                    *options.error = -1;
                return false;
            }
            if (options.error)
                *options.error = error;
            if (error != EINTR)
                break;
            if (!checkSignals(globalObject)) {
                if (options.error)
                    *options.error = -1;
                return false;
            }
        }
        // It was said to be ready, and is not: a datagram with a wrong checksum has been thrown away, for one.
        if (socketTimeoutOf(socket) > 0 && (error == EWOULDBLOCK || error == EAGAIN))
            continue;
        if (!options.error)
            posixError(globalObject, error);
        return false;
    }
}

// ---- The type ----

static JSValue socketFinalize(JSGlobalObject*, JSValue self);

static JSString* socketDefaultTimeoutKey(VM& vm) { return internString(vm, "socket.defaulttimeout"_s); }

static int64_t socketDefaultTimeout(JSGlobalObject* globalObject)
{
    JSValue value = posixState(globalObject)->get(socketDefaultTimeoutKey(getVM(globalObject)));
    return value ? tryInt64(value).value_or(-1) : -1;
}

static bool socketHasOwnFinalizer(PyType* type)
{
    auto* native = PyNativeFunction::tryFrom(type->lookup(Special::Del));
    return !native || native->def().pointer.f1 != socketFinalize;
}

// The socket has no descriptor any more.
static void socketDidClose(JSGlobalObject* globalObject, PyRecord* socket)
{
    if (!socketHasOwnFinalizer(socket->type()))
        runtime(globalObject).unregisterFinalizable(socket);
}

static PyRecord* socketAllocate(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    PyRecord* socket = PyRecord::create(globalObject, type, SocketFieldCount);
    socket->number = -1;
    socket->set(vm, SocketFamily, jsNumber(0));
    socket->set(vm, SocketKind, jsNumber(0));
    socket->set(vm, SocketProtocol, jsNumber(0));
    socket->set(vm, SocketTimeout, jsNumber(-1));
    return socket;
}

// Gives the socket the descriptor. False: an exception, and the descriptor is still the caller's.
static bool socketAdopt(JSGlobalObject* globalObject, PyRecord* socket, int fd, int family, int type, int protocol)
{
    VM& vm = getVM(globalObject);
    int64_t timeout = socketDefaultTimeout(globalObject);
    int flags = 0;
#ifdef SOCK_NONBLOCK
    flags |= SOCK_NONBLOCK;
    if (type & SOCK_NONBLOCK)
        timeout = 0;
    else
#endif
    if (timeout >= 0 && !socketSetBlocking(globalObject, fd, false))
        return false;
#ifdef SOCK_CLOEXEC
    flags |= SOCK_CLOEXEC;
#endif
    socket->number = fd;
    socket->set(vm, SocketFamily, jsNumber(family));
    socket->set(vm, SocketKind, jsNumber(type & ~flags));
    socket->set(vm, SocketProtocol, jsNumber(protocol));
    socket->set(vm, SocketTimeout, makeInt(globalObject, timeout));
    runtime(globalObject).registerFinalizable(socket);
    return true;
}

static JSValue socketNew(JSGlobalObject* globalObject, JSValue type, const CallArgs&)
{
    auto* socket = socketAllocate(globalObject, PyType::from(type));
    // It registers its finalizer when it has a descriptor: in __init__, which socket.py calls from its own.
    if (socket && runtime(globalObject).allocationTracebackLimit) [[unlikely]]
        noteWhereObjectWasMade(globalObject, socket);
    return socket;
}

static int socketCreateDescriptor(int family, int type, int protocol)
{
#ifdef SOCK_CLOEXEC
    return socket(family, type | SOCK_CLOEXEC, protocol);
#else
    int fd = socket(family, type, protocol);
    if (fd >= 0 && !posixSetInheritable(fd, false)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
#endif
}

static JSValue socketInit(JSGlobalObject* globalObject, JSValue self, JSValue familyValue, JSValue typeValue, JSValue protocolValue, JSValue fileno)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int family = -1;
    int type = -1;
    int protocol = -1;
    if ((familyValue && failed(scope, posixToInt(globalObject, familyValue, family))) || (typeValue && failed(scope, posixToInt(globalObject, typeValue, type))) || (protocolValue && failed(scope, posixToInt(globalObject, protocolValue, protocol))))
        return { };
    if (!audit(globalObject, "socket.__new__"_s, self, jsNumber(family), jsNumber(type), jsNumber(protocol)))
        return { };
    int fd;
    if (fileno && !isNone(fileno)) {
        if (failed(scope, socketToDescriptor(globalObject, fileno, fd)))
            return { };
        if (fd < 0)
            return raiseValueError(globalObject, scope, "negative file descriptor"_s);
        // What was not said is asked of the descriptor.
        SocketAddress address;
        address.length = sizeof(address.storage);
        if (!getsockname(fd, &address.generic, &address.length)) {
            if (family == -1)
                family = address.generic.sa_family;
        } else if (family == -1 || errno == EBADF || errno == ENOTSOCK)
            RELEASE_AND_RETURN(scope, posixError(globalObject));
        auto ask = [&](int option, int& result) {
            socklen_t length = sizeof(result);
            return result != -1 || !getsockopt(fd, SOL_SOCKET, option, &result, &length);
        };
        if (!ask(SO_TYPE, type))
            RELEASE_AND_RETURN(scope, posixError(globalObject));
#ifdef SO_PROTOCOL
        if (!ask(SO_PROTOCOL, protocol))
            RELEASE_AND_RETURN(scope, posixError(globalObject));
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
        fd = socketCreateDescriptor(family, type, protocol);
        if (fd < 0 && posixFreeDescriptors(globalObject))
            fd = socketCreateDescriptor(family, type, protocol);
        if (fd < 0)
            RELEASE_AND_RETURN(scope, posixError(globalObject));
    }
    if (!socketAdopt(globalObject, PyRecord::from(self), fd, family, type, protocol)) {
        // Also if it is the caller's.
        close(fd);
        return { };
    }
    return jsUndefined();
}

static JSValue socketRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* socket = PyRecord::from(self);
    return makeStr(globalObject, makeString("<socket object, fd="_s, socketDescriptor(socket), ", family="_s, socketFamilyOf(socket), ", type="_s, socket->get(SocketKind).asInt32(), ", proto="_s, socket->get(SocketProtocol).asInt32(), '>'));
}

static JSValue socketFinalize(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    if (socket->number < 0)
        return jsUndefined();
    JSValue saved = takePendingException(globalObject);
    {
        if (JSString* text = repr(globalObject, self)) {
            String string = text->value(globalObject);
            if (!scope.exception())
                warnAbout(globalObject, rt.ResourceWarning(), makeString("unclosed "_s, string), 1, self);
        }
        if (pendingExceptionMatches(globalObject, rt.Warning())) {
            JSValue exception = takePendingException(globalObject);
            String text = reprForMessage(globalObject, self);
            throwPythonException(globalObject, scope, exception);
            reportUnraisable(globalObject, makeString("Exception ignored while finalizing socket "_s, text), { });
        } else
            takePendingException(globalObject);
    }
    // Only now, so that what logs the warning can still ask the socket for its name.
    int fd = static_cast<int>(std::exchange(socket->number, -1));
    if (fd >= 0)
        close(fd);
    if (saved)
        throwPythonException(globalObject, scope, saved);
    return jsUndefined();
}

static JSValue socketGetFamily(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(SocketFamily); }
static JSValue socketGetKind(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(SocketKind); }
static JSValue socketGetProtocol(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(SocketProtocol); }
static JSValue socketGetTimeout(JSGlobalObject* globalObject, JSValue self) { return socketTimeoutObject(globalObject, socketTimeoutOf(PyRecord::from(self))); }

static JSValue socketFileno(JSGlobalObject*, JSValue self) { return jsNumber(socketDescriptor(PyRecord::from(self))); }

static JSValue socketCloseMethod(JSGlobalObject* globalObject, JSValue self)
{
    auto* socket = PyRecord::from(self);
    int fd = static_cast<int>(std::exchange(socket->number, -1));
    if (fd < 0)
        return jsUndefined();
    socketDidClose(globalObject, socket);
    int result;
    {
        AllowThreads allow(globalObject);
        result = close(fd);
    }
    // The peer having reset the connection is nothing to tell whoever is done with it.
    if (result < 0 && errno != ECONNRESET)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue socketDetach(JSGlobalObject* globalObject, JSValue self)
{
    auto* socket = PyRecord::from(self);
    int fd = static_cast<int>(std::exchange(socket->number, -1));
    socketDidClose(globalObject, socket);
    return jsNumber(fd);
}

static JSValue socketSetBlockingMethod(JSGlobalObject* globalObject, JSValue self, JSValue flag)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool blocking = isTrue(globalObject, flag);
    RETURN_IF_EXCEPTION(scope, { });
    auto* socket = PyRecord::from(self);
    socket->set(vm, SocketTimeout, jsNumber(blocking ? -1 : 0));
    if (!socketSetBlocking(globalObject, socketDescriptor(socket), blocking))
        return { };
    return jsUndefined();
}

static JSValue socketGetBlocking(JSGlobalObject*, JSValue self) { return jsBoolean(!!socketTimeoutOf(PyRecord::from(self))); }

static JSValue socketSetTimeout(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    int64_t timeout;
    if (!socketParseTimeout(globalObject, value, timeout))
        return { };
    auto* socket = PyRecord::from(self);
    socket->set(getVM(globalObject), SocketTimeout, makeInt(globalObject, timeout));
    if (!socketSetBlocking(globalObject, socketDescriptor(socket), timeout < 0))
        return { };
    return jsUndefined();
}

static JSValue socketSetOption(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int level;
    int option;
    unsigned length = 0;
    if (failed(scope, socketCheckCount(globalObject, args, "socket."_s, "setsockopt"_s, 3, 4)) || !socketToInt(globalObject, args[0], level) || !socketToInt(globalObject, args[1], option)
        || (args.size() == 4 && !posixToUnsignedMask(globalObject, args[3], length)))
        return { };
    JSValue value = args[2];
    if (args.size() == 3 && isNone(value))
        return raiseTypeError(globalObject, scope, "setsockopt() requires 4 arguments when the third argument is None"_s);
    if (args.size() == 4 && !isNone(value))
        return raiseTypeError(globalObject, scope, "setsockopt() only takes 4 arguments when the third argument is None (got "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)), ')');
    int fd = socketDescriptor(PyRecord::from(self));
    int result;
    if (hasIndex(globalObject, value)) {
        int flag;
        if (!socketToInt(globalObject, value, flag))
            return { };
        result = setsockopt(fd, level, option, &flag, sizeof(flag));
    } else if (isNone(value))
        result = setsockopt(fd, level, option, nullptr, length);
    else if (hasBuffer(value)) {
        BufferHolder holder(globalObject);
        if (!holder.get(value))
            return { };
        result = setsockopt(fd, level, option, holder.buffer.data, static_cast<socklen_t>(holder.buffer.length));
    } else
        return raiseTypeError(globalObject, scope, "socket option should be int, bytes-like object or None (got "_s, typeNameForMessage(globalObject, typeOf(globalObject, value)), ')');
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue socketGetOption(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int level;
    int option;
    int size = 0;
    if (failed(scope, socketCheckCount(globalObject, args, "socket."_s, "getsockopt"_s, 2, 3)) || !socketToInt(globalObject, args[0], level) || !socketToInt(globalObject, args[1], option)
        || (args.size() == 3 && !socketToInt(globalObject, args[2], size)))
        return { };
    int fd = socketDescriptor(PyRecord::from(self));
    if (!size) {
        int flag = 0;
        socklen_t length = sizeof(flag);
        if (getsockopt(fd, level, option, &flag, &length) < 0)
            RELEASE_AND_RETURN(scope, posixError(globalObject));
        return jsNumber(flag);
    }
    if (size < 0 || size > 1024)
        RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "getsockopt buflen out of range"_s));
    char buffer[1024];
    socklen_t length = size;
    if (getsockopt(fd, level, option, buffer, &length) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, std::span<const char> { buffer, std::min<size_t>(length, sizeof(buffer)) }));
}

static JSValue socketBind(JSGlobalObject* globalObject, JSValue self, JSValue addressValue)
{
    auto* socket = PyRecord::from(self);
    SocketAddress address;
    if (!socketParseAddress(globalObject, socket, addressValue, address, "bind"_s))
        return { };
    if (!audit(globalObject, "socket.bind"_s, self, addressValue))
        return { };
    if (bind(socketDescriptor(socket), &address.generic, address.length) < 0)
        return posixError(globalObject);
    return jsUndefined();
}

static JSValue socketListen(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int backlog = std::min(SOMAXCONN, 128);
    if (failed(scope, socketCheckCount(globalObject, args, "socket."_s, "listen"_s, 0, 1)) || (args.size() && !socketToInt(globalObject, args[0], backlog)))
        return { };
    if (listen(socketDescriptor(PyRecord::from(self)), std::max(backlog, 0)) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

// (descriptor, address)
static JSValue socketAccept(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    SocketAddress address;
    socklen_t capacity;
    if (failed(scope, socketAddressLength(globalObject, socket, capacity)))
        return { };
    int fd = -1;
    bool succeeded = socketCall(globalObject, socket, false, socketTimeoutOf(socket), [&] {
        for (bool isSecondTry : { false, true }) {
            address.length = capacity;
            {
                AllowThreads allow(globalObject);
#if defined(SOCK_CLOEXEC) && !OS(DARWIN)
                fd = accept4(socketDescriptor(socket), &address.generic, &address.length, SOCK_CLOEXEC);
#else
                fd = accept(socketDescriptor(socket), &address.generic, &address.length);
#endif
            }
            if (fd >= 0 || isSecondTry || !posixFreeDescriptors(globalObject))
                break;
        }
        return fd >= 0;
    });
    if (!succeeded)
        return { };
#if !defined(SOCK_CLOEXEC) || OS(DARWIN)
    if (!posixSetInheritable(fd, false)) {
        int saved = errno;
        close(fd);
        RELEASE_AND_RETURN(scope, posixError(globalObject, saved));
    }
#endif
    JSValue peer = socketAddressObject(globalObject, address, address.length);
    if (scope.exception()) [[unlikely]] {
        close(fd);
        return { };
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { jsNumber(fd), peer }));
}

// 0, or the reason why not; -1: an exception. With `raises`, any failure is one.
static int socketConnectTo(JSGlobalObject* globalObject, PyRecord* socket, const SocketAddress& address, bool raises)
{
    int result;
    {
        AllowThreads allow(globalObject);
        result = connect(socketDescriptor(socket), &address.generic, address.length);
    }
    if (!result)
        return 0;
    int error = errno;
    int64_t timeout = socketTimeoutOf(socket);
    bool waits;
    if (error == EINTR) {
        if (!checkSignals(globalObject))
            return -1;
        // The attempt goes on regardless. Unless the socket does not block, this waits for how it ends.
        waits = !!timeout;
    } else
        waits = timeout > 0 && error == EINPROGRESS;
    if (!waits) {
        if (!raises)
            return error;
        posixError(globalObject, error);
        return -1;
    }
    SocketCallOptions options;
    options.isConnect = true;
    options.error = raises ? nullptr : &error;
    bool succeeded = socketCall(globalObject, socket, true, timeout, [&] {
        int outcome;
        socklen_t length = sizeof(outcome);
        if (getsockopt(socketDescriptor(socket), SOL_SOCKET, SO_ERROR, &outcome, &length))
            return false;
        if (!outcome || outcome == EISCONN)
            return true;
        errno = outcome;
        return false;
    }, options);
    if (succeeded)
        return 0;
    return raises ? -1 : error;
}

static JSValue socketConnect(JSGlobalObject* globalObject, JSValue self, JSValue addressValue)
{
    auto* socket = PyRecord::from(self);
    SocketAddress address;
    if (!socketParseAddress(globalObject, socket, addressValue, address, "connect"_s))
        return { };
    if (!audit(globalObject, "socket.connect"_s, self, addressValue))
        return { };
    if (socketConnectTo(globalObject, socket, address, true) < 0)
        return { };
    return jsUndefined();
}

static JSValue socketConnectEx(JSGlobalObject* globalObject, JSValue self, JSValue addressValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    SocketAddress address;
    if (!socketParseAddress(globalObject, socket, addressValue, address, "connect_ex"_s))
        return { };
    if (!audit(globalObject, "socket.connect"_s, self, addressValue))
        return { };
    int result = socketConnectTo(globalObject, socket, address, false);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(result);
}

template<int (*function)(int, struct sockaddr*, socklen_t*)>
static JSValue socketName(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    SocketAddress address;
    if (failed(scope, socketAddressLength(globalObject, socket, address.length)))
        return { };
    if (function(socketDescriptor(socket), &address.generic, &address.length) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, socketAddressObject(globalObject, address, address.length));
}

static JSValue socketShutdown(JSGlobalObject* globalObject, JSValue self, JSValue howValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int how;
    if (failed(scope, posixToInt(globalObject, howValue, how)))
        return { };
    if (shutdown(socketDescriptor(PyRecord::from(self)), how) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

// ---- Receiving ----

static SocketCallOptions socketOptionsForFlags(int flags)
{
    SocketCallOptions options;
    options.neverBlocks = flags & MSG_DONTWAIT;
    return options;
}

using SocketScratch = Vector<uint8_t, 2048>;

// With MSG_TRUNC, the system says how long the datagram was, not how much of it there was room for.
static JSValue socketReceivedBytes(JSGlobalObject* globalObject, const SocketScratch& scratch, ssize_t count)
{
    if (static_cast<size_t>(count) > scratch.size()) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raiseValueError(globalObject, scope, "size larger than allocated size"_s);
    }
    return PyBytes::create(globalObject, scratch.span().first(count));
}

// (size[, flags])
static bool socketParseReceive(JSGlobalObject* globalObject, const CallArgs& args, ASCIILiteral name, int64_t& size, int& flags, SocketScratch& scratch)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    flags = 0;
    if (failed(scope, socketCheckCount(globalObject, args, "socket."_s, name, 1, 2)) || failed(scope, posixToSsize(globalObject, args[0], size)) || (args.size() > 1 && !socketToInt(globalObject, args[1], flags)))
        return false;
    if (size < 0) {
        raiseValueError(globalObject, scope, "negative buffersize in "_s, name);
        return false;
    }
    if (!scratch.tryGrow(size)) {
        socketRaiseMemoryError(globalObject);
        return false;
    }
    return true;
}

static JSValue socketRecv(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    auto* socket = PyRecord::from(self);
    int64_t size;
    int flags;
    SocketScratch scratch;
    if (!socketParseReceive(globalObject, args, "recv"_s, size, flags, scratch))
        return { };
    ssize_t count = 0;
    if (size) {
        bool succeeded = socketCall(globalObject, socket, false, socketTimeoutOf(socket), [&] {
            AllowThreads allow(globalObject);
            count = recv(socketDescriptor(socket), scratch.mutableSpan().data(), size, flags);
            return count >= 0;
        }, socketOptionsForFlags(flags));
        if (!succeeded)
            return { };
    }
    return socketReceivedBytes(globalObject, scratch, count);
}

static JSValue socketRecvFrom(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    int64_t size;
    int flags;
    SocketScratch scratch;
    SocketAddress address;
    socklen_t capacity;
    if (!socketParseReceive(globalObject, args, "recvfrom"_s, size, flags, scratch) || failed(scope, socketAddressLength(globalObject, socket, capacity)))
        return { };
    ssize_t count = 0;
    bool succeeded = socketCall(globalObject, socket, false, socketTimeoutOf(socket), [&] {
        memset(&address.storage, 0, sizeof(address.storage));
        address.length = capacity;
        AllowThreads allow(globalObject);
        count = recvfrom(socketDescriptor(socket), scratch.mutableSpan().data(), size, flags, &address.generic, &address.length);
        return count >= 0;
    }, socketOptionsForFlags(flags));
    if (!succeeded)
        return { };
    JSValue sender = socketAddressObject(globalObject, address, address.length);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue data = socketReceivedBytes(globalObject, scratch, count);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { data, sender }));
}

// (buffer, nbytes=0, flags=0). `size` is how much to receive.
static bool socketParseReceiveInto(JSGlobalObject* globalObject, ASCIILiteral name, ASCIILiteral tooSmall, JSValue bufferValue, JSValue sizeValue, JSValue flagsValue, BufferHolder& holder, int64_t& size, int& flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    size = 0;
    flags = 0;
    if (!posixGetWritableBuffer(globalObject, bufferValue, name, 1, holder) || (sizeValue && failed(scope, posixToSsize(globalObject, sizeValue, size))) || (flagsValue && !socketToInt(globalObject, flagsValue, flags)))
        return false;
    if (size < 0) {
        raiseValueError(globalObject, scope, "negative buffersize in "_s, name);
        return false;
    }
    if (!size)
        size = holder.buffer.length;
    else if (size > holder.buffer.length) {
        raiseValueError(globalObject, scope, tooSmall);
        return false;
    }
    return true;
}

static JSValue socketRecvInto(JSGlobalObject* globalObject, JSValue self, JSValue bufferValue, JSValue sizeValue, JSValue flagsValue)
{
    auto* socket = PyRecord::from(self);
    BufferHolder holder(globalObject);
    int64_t size;
    int flags;
    if (!socketParseReceiveInto(globalObject, "recv_into"_s, "buffer too small for requested bytes"_s, bufferValue, sizeValue, flagsValue, holder, size, flags))
        return { };
    ssize_t count = 0;
    if (size) {
        bool succeeded = socketCall(globalObject, socket, false, socketTimeoutOf(socket), [&] {
            if (!refreshBuffer(globalObject, holder.buffer))
                return false;
            AllowThreads allow(globalObject);
            count = recv(socketDescriptor(socket), holder.buffer.data, size, flags);
            return count >= 0;
        }, socketOptionsForFlags(flags));
        if (!succeeded)
            return { };
    }
    return posixMakeInt(globalObject, count);
}

static JSValue socketRecvFromInto(JSGlobalObject* globalObject, JSValue self, JSValue bufferValue, JSValue sizeValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    BufferHolder holder(globalObject);
    int64_t size;
    int flags;
    SocketAddress address;
    socklen_t capacity;
    if (!socketParseReceiveInto(globalObject, "recvfrom_into"_s, "nbytes is greater than the length of the buffer"_s, bufferValue, sizeValue, flagsValue, holder, size, flags)
        || !socketAddressLength(globalObject, socket, capacity))
        return { };
    ssize_t count = 0;
    bool succeeded = socketCall(globalObject, socket, false, socketTimeoutOf(socket), [&] {
        if (!refreshBuffer(globalObject, holder.buffer))
            return false;
        memset(&address.storage, 0, sizeof(address.storage));
        address.length = capacity;
        AllowThreads allow(globalObject);
        count = recvfrom(socketDescriptor(socket), holder.buffer.data, size, flags, &address.generic, &address.length);
        return count >= 0;
    }, socketOptionsForFlags(flags));
    if (!succeeded)
        return { };
    JSValue sender = socketAddressObject(globalObject, address, address.length);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { posixMakeInt(globalObject, count), sender }));
}

// ---- Ancillary data ----

static constexpr size_t socketLengthLimit = 0x7fffffff; // socklen_t may be signed.

static bool socketControlLength(size_t dataLength, size_t& result)
{
    if (dataLength > socketLengthLimit - CMSG_LEN(0))
        return false;
    result = CMSG_LEN(dataLength);
    return result <= socketLengthLimit && result >= dataLength;
}

static bool socketControlSpace(size_t dataLength, size_t& result)
{
    // Of 1, to allow for the padding both before the data and after it.
    if (dataLength > socketLengthLimit - CMSG_SPACE(1))
        return false;
    result = CMSG_SPACE(dataLength);
    return result <= socketLengthLimit && result >= dataLength;
}

enum class ControlData : uint8_t { Whole, CutOff, Invalid };

// How much of the data of a control message that was received is there.
static ControlData socketControlData(const struct msghdr& message, struct cmsghdr* header, size_t& length)
{
    size_t total = message.msg_controllen;
    size_t headerOffset = reinterpret_cast<char*>(header) - static_cast<char*>(message.msg_control);
    if (headerOffset > total || total - headerOffset < CMSG_LEN(0) || header->cmsg_len < CMSG_LEN(0))
        return ControlData::Invalid;
    size_t dataOffset = reinterpret_cast<char*>(CMSG_DATA(header)) - static_cast<char*>(message.msg_control);
    if (dataOffset > total)
        return ControlData::Invalid;
    length = header->cmsg_len - CMSG_LEN(0);
    if (length <= total - dataOffset)
        return ControlData::Whole;
    length = total - dataOffset;
    return ControlData::CutOff;
}

template<typename Function>
static void socketForEachControlMessage(struct msghdr& message, const Function& function)
{
    for (struct cmsghdr* header = message.msg_controllen > 0 ? CMSG_FIRSTHDR(&message) : nullptr; header; header = CMSG_NXTHDR(&message, header)) {
        size_t length = 0;
        ControlData state = socketControlData(message, header, length);
        if (!function(header, length, state) || state != ControlData::Whole)
            break;
    }
}

// What recvmsg() and recvmsg_into() share. `makeData` makes the first item of the result of the number of bytes received.
template<typename Refresh, typename MakeData>
static JSValue socketReceiveMessage(JSGlobalObject* globalObject, PyRecord* socket, int64_t controlSize, int flags, const Refresh& refresh, const MakeData& makeData)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    SocketAddress address;
    socklen_t capacity;
    if (failed(scope, socketAddressLength(globalObject, socket, capacity)))
        return { };
    if (controlSize < 0 || static_cast<uint64_t>(controlSize) > socketLengthLimit)
        return raiseValueError(globalObject, scope, "invalid ancillary data buffer length"_s);
    Vector<uint8_t, 256> control;
    if (!control.tryGrow(controlSize))
        RELEASE_AND_RETURN(scope, socketRaiseMemoryError(globalObject));

    struct msghdr message { };
    ssize_t count = 0;
    bool succeeded = socketCall(globalObject, socket, false, socketTimeoutOf(socket), [&] {
        if (!refresh(message))
            return false;
        memset(&address.storage, 0, sizeof(address.storage));
        address.generic.sa_family = AF_UNSPEC;
        message.msg_name = &address.generic;
        message.msg_namelen = capacity;
        message.msg_control = controlSize ? control.mutableSpan().data() : nullptr;
        message.msg_controllen = controlSize;
        message.msg_flags = 0;
        AllowThreads allow(globalObject);
        count = recvmsg(socketDescriptor(socket), &message, flags);
        return count >= 0;
    }, socketOptionsForFlags(flags));
    if (!succeeded)
        return { };

    JSArray* ancillary = newList(globalObject);
    socketForEachControlMessage(message, [&](struct cmsghdr* header, size_t length, ControlData state) {
        if (state != ControlData::Whole && !warn(globalObject, rt.RuntimeWarning(), "received malformed or improperly-truncated ancillary data"_s))
            return false;
        if (state == ControlData::Invalid)
            return false;
        JSValue data = PyBytes::create(globalObject, std::span<const uint8_t> { CMSG_DATA(header), length });
        listAppend(globalObject, ancillary, PyTuple::create(globalObject, { jsNumber(header->cmsg_level), jsNumber(header->cmsg_type), data }));
        return !scope.exception();
    });
    JSValue result;
    if (!scope.exception()) {
        JSValue data = makeData(count);
        JSValue sender = scope.exception() ? JSValue() : socketAddressObject(globalObject, address, std::min<size_t>(message.msg_namelen, capacity));
        if (!scope.exception())
            result = PyTuple::create(globalObject, { data, ancillary, jsNumber(message.msg_flags), sender });
    }
    if (!result) {
        // Nobody will ever know of the descriptors that have come with the message.
        socketForEachControlMessage(message, [&](struct cmsghdr* header, size_t length, ControlData state) {
            if (state != ControlData::Invalid && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
                for (size_t i = 0; i + sizeof(int) <= length; i += sizeof(int)) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(header) + i, sizeof(fd));
                    close(fd);
                }
            }
            return true;
        });
    }
    return result;
}

static JSValue socketRecvMsg(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size;
    int64_t controlSize = 0;
    int flags = 0;
    if (failed(scope, socketCheckCount(globalObject, args, "socket."_s, "recvmsg"_s, 1, 3)) || failed(scope, posixToSsize(globalObject, args[0], size)) || (args.size() > 1 && failed(scope, posixToSsize(globalObject, args[1], controlSize)))
        || (args.size() > 2 && !socketToInt(globalObject, args[2], flags)))
        return { };
    if (size < 0)
        return raiseValueError(globalObject, scope, "negative buffer size in recvmsg()"_s);
    SocketScratch scratch;
    if (!scratch.tryGrow(size))
        RELEASE_AND_RETURN(scope, socketRaiseMemoryError(globalObject));
    struct iovec vector { scratch.mutableSpan().data(), static_cast<size_t>(size) };
    RELEASE_AND_RETURN(scope, socketReceiveMessage(globalObject, PyRecord::from(self), controlSize, flags, [&](struct msghdr& message) {
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        return true;
    }, [&](ssize_t count) -> JSValue {
        return socketReceivedBytes(globalObject, scratch, count);
    }));
}

static JSValue socketRecvMsgInto(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t controlSize = 0;
    int flags = 0;
    if (failed(scope, socketCheckCount(globalObject, args, "socket."_s, "recvmsg_into"_s, 1, 3)) || (args.size() > 1 && failed(scope, posixToSsize(globalObject, args[1], controlSize))) || (args.size() > 2 && !socketToInt(globalObject, args[2], flags)))
        return { };
    MarkedArgumentBuffer items;
    if (failed(scope, socketItems(globalObject, args[0], "recvmsg_into() argument 1 must be an iterable"_s, items)))
        return { };
    SocketBufferList buffers(globalObject);
    for (unsigned i = 0; i < items.size(); ++i) {
        if (buffers.add(items.at(i), BufferWritable))
            continue;
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()) || clearPendingExceptionIfMatches(globalObject, rt.BufferError()))
            raiseTypeError(globalObject, scope, "recvmsg_into() argument 1 must be an iterable of single-segment read-write buffers"_s);
        return { };
    }
    RELEASE_AND_RETURN(scope, socketReceiveMessage(globalObject, PyRecord::from(self), controlSize, flags, [&](struct msghdr& message) {
        if (!buffers.fillVectors())
            return false;
        message.msg_iov = buffers.vectors();
        message.msg_iovlen = buffers.size();
        return true;
    }, [&](ssize_t count) -> JSValue {
        return posixMakeInt(globalObject, count);
    }));
}

// ---- Sending ----

static JSValue socketSend(JSGlobalObject* globalObject, JSValue self, JSValue data, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    BufferHolder holder(globalObject);
    int flags = 0;
    if (!holder.get(data) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    ssize_t count = 0;
    bool succeeded = socketCall(globalObject, socket, true, socketTimeoutOf(socket), [&] {
        if (!refreshBuffer(globalObject, holder.buffer))
            return false;
        AllowThreads allow(globalObject);
        count = send(socketDescriptor(socket), holder.buffer.data, holder.buffer.length, flags);
        return count >= 0;
    }, socketOptionsForFlags(flags));
    if (!succeeded)
        return { };
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

static JSValue socketSendAll(JSGlobalObject* globalObject, JSValue self, JSValue data, JSValue flagsValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    BufferHolder holder(globalObject);
    int flags = 0;
    if (!holder.get(data) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    int64_t timeout = socketTimeoutOf(socket);
    bool hasTimeout = timeout > 0;
    int64_t deadline = 0;
    if (hasTimeout && __builtin_add_overflow(monotonicNanoseconds(), timeout, &deadline))
        deadline = std::numeric_limits<int64_t>::max();
    int64_t sent = 0;
    bool isFirst = true;
    do {
        // The timeout is for all of it.
        if (hasTimeout && !std::exchange(isFirst, false)) {
            timeout = deadline - monotonicNanoseconds();
            if (timeout <= 0)
                return raise(globalObject, scope, rt.TimeoutError(), "timed out"_s);
        }
        ssize_t count = 0;
        bool succeeded = socketCall(globalObject, socket, true, timeout, [&] {
            if (!refreshBuffer(globalObject, holder.buffer))
                return false;
            AllowThreads allow(globalObject);
            count = send(socketDescriptor(socket), holder.buffer.data + sent, holder.buffer.length - sent, flags);
            return count >= 0;
        }, socketOptionsForFlags(flags));
        if (!succeeded)
            return { };
        sent += count;
        // A signal may have cut the call short without making it fail.
        if (!checkSignals(globalObject))
            return { };
    } while (sent < holder.buffer.length);
    return jsUndefined();
}

// (data[, flags], address)
static JSValue socketSendTo(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    if (args.keywordCount())
        return raiseTypeError(globalObject, scope, "socket.sendto() takes no keyword arguments"_s);
    if (args.size() != 2 && args.size() != 3)
        return raiseTypeError(globalObject, scope, "sendto() takes 2 or 3 arguments ("_s, args.size(), " given)"_s);
    BufferHolder holder(globalObject);
    int flags = 0;
    SocketAddress address;
    if (!holder.get(args[0]) || (args.size() == 3 && !socketToInt(globalObject, args[1], flags)) || !socketParseAddress(globalObject, socket, args[args.size() - 1], address, "sendto"_s))
        return { };
    if (!audit(globalObject, "socket.sendto"_s, self, args[args.size() - 1]))
        return { };
    ssize_t count = 0;
    bool succeeded = socketCall(globalObject, socket, true, socketTimeoutOf(socket), [&] {
        if (!refreshBuffer(globalObject, holder.buffer))
            return false;
        AllowThreads allow(globalObject);
        count = sendto(socketDescriptor(socket), holder.buffer.data, holder.buffer.length, flags, &address.generic, address.length);
        return count >= 0;
    }, socketOptionsForFlags(flags));
    if (!succeeded)
        return { };
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

static JSValue socketSendMsg(JSGlobalObject* globalObject, JSValue self, JSValue buffersValue, JSValue ancillaryValue, JSValue flagsValue, JSValue addressValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* socket = PyRecord::from(self);
    int flags = 0;
    if (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags)))
        return { };
    struct msghdr message { };
    SocketAddress address;
    if (addressValue && !isNone(addressValue)) {
        if (!socketParseAddress(globalObject, socket, addressValue, address, "sendmsg"_s))
            return { };
        message.msg_name = &address.generic;
        message.msg_namelen = address.length;
    }
    if (!audit(globalObject, "socket.sendmsg"_s, self, addressValue))
        return { };

    MarkedArgumentBuffer items;
    if (failed(scope, socketItems(globalObject, buffersValue, "sendmsg() argument 1 must be an iterable"_s, items)))
        return { };
    SocketBufferList buffers(globalObject);
    for (unsigned i = 0; i < items.size(); ++i) {
        if (!buffers.add(items.at(i), BufferSimple))
            return { };
    }

    // (level, type, data), ...
    struct Header {
        int level;
        int type;
    };
    MarkedArgumentBuffer ancillaryItems;
    if (ancillaryValue && failed(scope, socketItems(globalObject, ancillaryValue, "sendmsg() argument 2 must be an iterable"_s, ancillaryItems)))
        return { };
    SocketBufferList ancillary(globalObject);
    Vector<Header, 4> headers;
    size_t controlSize = 0;
    for (unsigned i = 0; i < ancillaryItems.size(); ++i) {
        JSValue item = ancillaryItems.at(i);
        auto* tuple = PyTuple::tryFrom(item);
        if (!tuple) {
            // Another sequence will do, unless it is one that is more likely a mistake.
            if (!posixIsSequence(globalObject, item) || isStr(item) || PyBytes::is(item) || PyByteArray::is(item))
                return raiseTypeError(globalObject, scope, "[sendmsg() ancillary data items]() argument must be 3-item tuple, not "_s, posixTypeNameOfArgument(globalObject, item));
            int64_t count = length(globalObject, item);
            RETURN_IF_EXCEPTION(scope, { });
            if (count != 3)
                return raiseTypeError(globalObject, scope, "[sendmsg() ancillary data items]() argument must be sequence of length 3, not "_s, count);
            tuple = tupleFromIterable(globalObject, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (tuple->length() != 3)
            return raiseTypeError(globalObject, scope, "[sendmsg() ancillary data items]() argument must be tuple of length 3, not "_s, tuple->length());
        Header header;
        if (!socketToInt(globalObject, tuple->at(0), header.level) || !socketToInt(globalObject, tuple->at(1), header.type) || !ancillary.add(tuple->at(2), BufferSimple))
            return { };
        headers.append(header);
        size_t space;
        if (!socketControlSpace(ancillary.at(i).length, space))
            RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "ancillary data item too large"_s));
        controlSize += space;
        if (controlSize > socketLengthLimit)
            RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "too much ancillary data"_s));
    }
    Vector<uint8_t, 256> control;
    if (!control.tryGrow(controlSize))
        RELEASE_AND_RETURN(scope, socketRaiseMemoryError(globalObject));

    ssize_t count = 0;
    bool succeeded = socketCall(globalObject, socket, true, socketTimeoutOf(socket), [&] {
        if (!buffers.fillVectors() || !ancillary.fillVectors())
            return false;
        message.msg_iov = buffers.vectors();
        message.msg_iovlen = buffers.size();
        if (controlSize) {
            // CMSG_NXTHDR() looks at the length in the header it steps to.
            memset(control.mutableSpan().data(), 0, controlSize);
            message.msg_control = control.mutableSpan().data();
            message.msg_controllen = controlSize;
            struct cmsghdr* header = nullptr;
            for (size_t i = 0; i < headers.size(); ++i) {
                header = header ? CMSG_NXTHDR(&message, header) : CMSG_FIRSTHDR(&message);
                size_t length;
                if (!header || !socketControlLength(ancillary.at(i).length, length)) {
                    raiseRuntimeError(globalObject, scope, "ancillary data does not fit in calculated space"_s);
                    return false;
                }
                header->cmsg_len = length;
                header->cmsg_level = headers[i].level;
                header->cmsg_type = headers[i].type;
                if (ancillary.at(i).length)
                    memcpy(CMSG_DATA(header), ancillary.at(i).data, ancillary.at(i).length);
            }
        }
        AllowThreads allow(globalObject);
        count = sendmsg(socketDescriptor(socket), &message, flags);
        return count >= 0;
    }, socketOptionsForFlags(flags));
    if (!succeeded)
        return { };
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, count));
}

static constexpr NativeDef socketMethods[] = {
    PY_NEW(socketNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", socketInit, "($self, /, family=-1, type=-1, proto=-1, fileno=None)"),
    PY_SLOT("__repr__", socketRepr, "($self, /)"),
    PY_SLOT("__del__", socketFinalize, "($self, /)"),
    PY_METHOD("_accept", socketAccept, "($self, /)"),
    PY_METHOD("bind", socketBind, "($self, object, /)"),
    PY_METHOD("close", socketCloseMethod, "($self, /)"),
    PY_METHOD("connect", socketConnect, "($self, object, /)"),
    PY_METHOD("connect_ex", socketConnectEx, "($self, object, /)"),
    PY_METHOD("detach", socketDetach, "($self, /)"),
    PY_METHOD("fileno", socketFileno, "($self, /)"),
    PY_METHOD("getpeername", socketName<getpeername>, "($self, /)"),
    PY_METHOD("getsockname", socketName<getsockname>, "($self, /)"),
    PY_METHOD("getsockopt", socketGetOption, "($self, *args)"),
    PY_METHOD("listen", socketListen, "($self, *args)"),
    PY_METHOD("recv", socketRecv, "($self, *args)"),
    PY_METHOD("recv_into", socketRecvInto, "($self, /, buffer, nbytes=0, flags=0)"),
    PY_METHOD("recvfrom", socketRecvFrom, "($self, *args)"),
    PY_METHOD("recvfrom_into", socketRecvFromInto, "($self, /, buffer, nbytes=0, flags=0)"),
    PY_METHOD("recvmsg", socketRecvMsg, "($self, *args)"),
    PY_METHOD("recvmsg_into", socketRecvMsgInto, "($self, *args)"),
    PY_METHOD("send", socketSend, "($self, data, flags=0, /)"),
    PY_METHOD("sendall", socketSendAll, "($self, data, flags=0, /)"),
    PY_METHOD("sendmsg", socketSendMsg, "($self, buffers, ancdata=(), flags=0, address=None, /)"),
    PY_METHOD("sendto", socketSendTo, "($self, *args)"),
    PY_METHOD("setblocking", socketSetBlockingMethod, "($self, object, /)"),
    PY_METHOD("getblocking", socketGetBlocking, "($self, /)"),
    PY_METHOD("settimeout", socketSetTimeout, "($self, object, /)"),
    PY_METHOD("gettimeout", socketGetTimeout, "($self, /)"),
    PY_METHOD("setsockopt", socketSetOption, "($self, *args)"),
    PY_METHOD("shutdown", socketShutdown, "($self, object, /)"),
};

static constexpr NativeGetSet socketGetSets[] = {
    PY_MEMBER("family", socketGetFamily),
    PY_MEMBER("type", socketGetKind),
    PY_MEMBER("proto", socketGetProtocol),
    { "timeout"_s, socketGetTimeout },
};

static constexpr TypeDef socketTypeDef { "_socket.socket"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, socketMethods, socketGetSets, { } };

PyType* socketType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(socketTypeDef); }

// ---- Functions: descriptors ----

static JSValue socketClose(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, socketToDescriptor(globalObject, fdValue, fd)))
        return { };
    int result;
    {
        AllowThreads allow(globalObject);
        result = close(fd);
    }
    if (result < 0 && errno != ECONNRESET)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue socketDup(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, socketToDescriptor(globalObject, fdValue, fd)))
        return { };
    int result = posixDupNotInheritable(fd);
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsNumber(result);
}

static JSValue socketPair(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int family = AF_UNIX;
    int type = SOCK_STREAM;
    int protocol = 0;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "socketpair"_s, 0, 3)) || (args.size() > 0 && !socketToInt(globalObject, args[0], family)) || (args.size() > 1 && !socketToInt(globalObject, args[1], type))
        || (args.size() > 2 && !socketToInt(globalObject, args[2], protocol)))
        return { };
    int descriptors[2];
#ifdef SOCK_CLOEXEC
    if (socketpair(family, type | SOCK_CLOEXEC, protocol, descriptors) < 0 && (!posixFreeDescriptors(globalObject) || socketpair(family, type | SOCK_CLOEXEC, protocol, descriptors) < 0))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
#else
    if (socketpair(family, type, protocol, descriptors) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    if (!posixSetInheritable(descriptors[0], false) || !posixSetInheritable(descriptors[1], false)) {
        int saved = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        RELEASE_AND_RETURN(scope, posixError(globalObject, saved));
    }
#endif
    PyRecord* sockets[2];
    for (unsigned i = 0; i < 2; ++i) {
        sockets[i] = socketAllocate(globalObject, socketType(globalObject));
        if (!socketAdopt(globalObject, sockets[i], descriptors[i], family, type, protocol)) {
            // The first, if it has been adopted, is closed when it is collected.
            for (unsigned j = i; j < 2; ++j)
                close(descriptors[j]);
            return { };
        }
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { sockets[0], sockets[1] }));
}

// ---- Functions: names and numbers ----

static JSValue socketGetHostName(JSGlobalObject* globalObject)
{
    if (!audit(globalObject, "socket.gethostname"_s))
        return { };
    char name[1024];
    if (gethostname(name, sizeof(name) - 1) < 0)
        return posixError(globalObject);
    name[sizeof(name) - 1] = '\0';
    return posixDecode(globalObject, name);
}

static JSValue socketSetHostName(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "sethostname"_s, 1, 1)))
        return { };
    PosixBytes name;
    if (auto* bytes = PyBytes::tryFrom(args[0]))
        name.assign(byteCast<char>(bytes->span()));
    else if (!posixToFSBytes(globalObject, args[0], name))
        return { };
    if (!audit(globalObject, "socket.sethostname"_s, args[0]))
        return { };
    if (sethostname(name.characters(), name.length()) < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    return jsUndefined();
}

static JSValue socketGetHostByName(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    SocketAddress address;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "gethostbyname"_s, 1, 1)) || !socketToHostName(globalObject, args[0], "gethostbyname"_s, name) || !audit(globalObject, "socket.gethostbyname"_s, args[0]) || failed(scope, socketResolve(globalObject, name.characters(), address, AF_INET)))
        return { };
    RELEASE_AND_RETURN(scope, socketFormatIP(globalObject, AF_INET, &address.inet.sin_addr));
}

// (name, aliases, addresses)
static JSValue socketHostEntry(JSGlobalObject* globalObject, struct hostent* entry, int family, int error)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!entry)
        RELEASE_AND_RETURN(scope, socketRaiseHostError(globalObject, error));
    if (entry->h_addrtype != family)
        RELEASE_AND_RETURN(scope, posixError(globalObject, EAFNOSUPPORT));
    JSArray* aliases = newList(globalObject);
    JSArray* addresses = newList(globalObject);
    for (char** alias = entry->h_aliases; alias && *alias; ++alias)
        listAppend(globalObject, aliases, posixDecode(globalObject, *alias));
    for (char** address = entry->h_addr_list; *address; ++address) {
        JSValue text = socketFormatIP(globalObject, family, *address);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, addresses, text);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { posixDecode(globalObject, entry->h_name), aliases, addresses }));
}

static JSValue socketGetHostByNameEx(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    SocketAddress address;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "gethostbyname_ex"_s, 1, 1)) || !socketToHostName(globalObject, args[0], "gethostbyname_ex"_s, name) || !audit(globalObject, "socket.gethostbyname"_s, args[0]) || failed(scope, socketResolve(globalObject, name.characters(), address, AF_INET)))
        return { };
    struct hostent* entry = nullptr;
    int error = 0;
#if OS(LINUX)
    struct hostent storage;
    alignas(8) char buffer[16384];
    {
        AllowThreads allow(globalObject);
        gethostbyname_r(name.characters(), &storage, buffer, sizeof(buffer) - 1, &entry, &error);
    }
#else
    entry = gethostbyname(name.characters());
    error = h_errno;
#endif
    RELEASE_AND_RETURN(scope, socketHostEntry(globalObject, entry, address.generic.sa_family, error));
}

static JSValue socketGetHostByAddr(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    SocketAddress address;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "gethostbyaddr"_s, 1, 1)) || !socketToHostName(globalObject, args[0], "gethostbyaddr"_s, name) || !audit(globalObject, "socket.gethostbyaddr"_s, args[0]) || failed(scope, socketResolve(globalObject, name.characters(), address, AF_UNSPEC)))
        return { };
    int family = address.generic.sa_family;
    const void* bytes = family == AF_INET ? static_cast<const void*>(&address.inet.sin_addr) : &address.inet6.sin6_addr;
    socklen_t length = family == AF_INET ? sizeof(address.inet.sin_addr) : sizeof(address.inet6.sin6_addr);
    struct hostent* entry = nullptr;
    int error = 0;
#if OS(LINUX)
    struct hostent storage;
    alignas(8) char buffer[16384];
    {
        AllowThreads allow(globalObject);
        gethostbyaddr_r(bytes, length, family, &storage, buffer, sizeof(buffer) - 1, &entry, &error);
    }
#else
    entry = gethostbyaddr(bytes, length, family);
    error = h_errno;
#endif
    RELEASE_AND_RETURN(scope, socketHostEntry(globalObject, entry, family, error));
}

static JSValue socketGetServByName(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    PosixBytes protocol;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "getservbyname"_s, 1, 2)) || !socketToCString(globalObject, args[0], "getservbyname"_s, 1, name)
        || (args.size() > 1 && !socketToCString(globalObject, args[1], "getservbyname"_s, 2, protocol)))
        return { };
    if (!audit(globalObject, "socket.getservbyname"_s, args[0], args.get(1)))
        return { };
    struct servent* entry = getservbyname(name.characters(), args.size() > 1 ? protocol.characters() : nullptr);
    if (!entry)
        RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "service/proto not found"_s));
    return jsNumber(ntohs(static_cast<uint16_t>(entry->s_port)));
}

static JSValue socketGetServByPort(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int port;
    PosixBytes protocol;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "getservbyport"_s, 1, 2)) || !socketToInt(globalObject, args[0], port) || (args.size() > 1 && !socketToCString(globalObject, args[1], "getservbyport"_s, 2, protocol)))
        return { };
    if (!audit(globalObject, "socket.getservbyport"_s, jsNumber(port), args.get(1)))
        return { };
    if (port < 0 || port > 0xffff)
        return raiseOverflowError(globalObject, scope, "getservbyport: port must be 0-65535."_s);
    struct servent* entry = getservbyport(htons(static_cast<uint16_t>(port)), args.size() > 1 ? protocol.characters() : nullptr);
    if (!entry)
        RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "port/proto not found"_s));
    RELEASE_AND_RETURN(scope, posixDecode(globalObject, entry->s_name));
}

static JSValue socketGetProtoByName(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "getprotobyname"_s, 1, 1)) || !socketToCString(globalObject, args[0], "getprotobyname"_s, 1, name))
        return { };
    struct protoent* entry = getprotobyname(name.characters());
    if (!entry)
        RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "protocol not found"_s));
    return jsNumber(entry->p_proto);
}

static JSValue socketNtohs(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint16_t number;
    if (failed(scope, socketToUnsigned(globalObject, value, "uint16_t"_s, number)))
        return { };
    return jsNumber(ntohs(number));
}

static JSValue socketNtohl(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t number;
    if (failed(scope, socketToUnsigned(globalObject, value, "uint32_t"_s, number)))
        return { };
    return jsNumber(ntohl(number));
}

static JSValue socketInetAton(JSGlobalObject* globalObject, JSValue value)
{
    PosixBytes text;
    if (!socketToCString(globalObject, value, "inet_aton"_s, 0, text))
        return { };
    struct in_addr address;
    if (!inet_aton(text.characters(), &address))
        return socketRaiseOSError(globalObject, "illegal IP address string passed to inet_aton"_s);
    return PyBytes::create(globalObject, std::span<const char> { reinterpret_cast<const char*>(&address), sizeof(address) });
}

static JSValue socketInetNtoa(JSGlobalObject* globalObject, JSValue value)
{
    BufferHolder holder(globalObject);
    if (!holder.get(value))
        return { };
    if (holder.buffer.length != sizeof(struct in_addr))
        return socketRaiseOSError(globalObject, "packed IP wrong length for inet_ntoa"_s);
    return socketFormatIP(globalObject, AF_INET, holder.buffer.data);
}

static JSValue socketInetPton(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int family;
    PosixBytes text;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "inet_pton"_s, 2, 2)) || !socketToInt(globalObject, args[0], family) || !socketToCString(globalObject, args[1], "inet_pton"_s, 2, text))
        return { };
    char packed[sizeof(struct in6_addr)];
    int result = inet_pton(family, text.characters(), packed);
    if (result < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    if (!result)
        RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "illegal IP address string passed to inet_pton"_s));
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, std::span<const char> { packed, family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr) }));
}

static JSValue socketInetNtop(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int family;
    BufferHolder holder(globalObject);
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "inet_ntop"_s, 2, 2)) || !socketToInt(globalObject, args[0], family) || !holder.get(args[1]))
        return { };
    if (family != AF_INET && family != AF_INET6)
        return raiseValueError(globalObject, scope, "unknown address family "_s, family);
    if (static_cast<size_t>(holder.buffer.length) != (family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr)))
        return raiseValueError(globalObject, scope, "invalid length of packed IP address string"_s);
    RELEASE_AND_RETURN(scope, socketFormatIP(globalObject, family, holder.buffer.data));
}

static JSValue socketGetAddrInfo(JSGlobalObject* globalObject, JSValue hostValue, JSValue portValue, JSValue familyValue, JSValue typeValue, JSValue protocolValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct addrinfo hints { };
    hints.ai_family = AF_UNSPEC;
    if ((familyValue && !socketToInt(globalObject, familyValue, hints.ai_family)) || (typeValue && !socketToInt(globalObject, typeValue, hints.ai_socktype))
        || (protocolValue && !socketToInt(globalObject, protocolValue, hints.ai_protocol)) || (flagsValue && !socketToInt(globalObject, flagsValue, hints.ai_flags)))
        return { };
    if (!audit(globalObject, "socket.getaddrinfo"_s, hostValue, portValue, jsNumber(hints.ai_family), jsNumber(hints.ai_socktype), jsNumber(hints.ai_protocol)))
        return { };

    PosixBytes host;
    PosixBytes port;
    bool hasHost = !isNone(hostValue);
    bool hasPort = !isNone(portValue);
    if (isStr(hostValue)) {
        if (!socketEncodeIDNA(globalObject, strValue(hostValue), host))
            return { };
    } else if (auto* bytes = PyBytes::tryFrom(hostValue))
        host.assign(byteCast<char>(bytes->span()));
    else if (hasHost)
        return raiseTypeError(globalObject, scope, "getaddrinfo() argument 1 must be string or None"_s);
    if (hasIndex(globalObject, portValue)) {
        JSValue index = toIndex(globalObject, portValue);
        RETURN_IF_EXCEPTION(scope, { });
        JSString* text = str(globalObject, index);
        RETURN_IF_EXCEPTION(scope, { });
        if (!socketEncodeUTF8(globalObject, text, port))
            return { };
    } else if (isStr(portValue)) {
        if (!socketEncodeUTF8(globalObject, strValue(portValue), port))
            return { };
    } else if (auto* bytes = PyBytes::tryFrom(portValue))
        port.assign(byteCast<char>(bytes->span()));
    else if (hasPort)
        RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, "Int or String expected"_s));
#if OS(DARWIN)
    // There, with AI_NUMERICSERV, no service or "0" is refused.
    if ((hints.ai_flags & AI_NUMERICSERV) && (!hasPort || !strcmp(port.characters(), "0"))) {
        port.assign(std::span<const char> { "00", 2 });
        hasPort = true;
    }
#endif

    struct addrinfo* found = nullptr;
    int error;
    {
        AllowThreads allow(globalObject);
        error = getaddrinfo(hasHost ? host.characters() : nullptr, hasPort ? port.characters() : nullptr, &hints, &found);
    }
    if (error)
        RELEASE_AND_RETURN(scope, socketRaiseAddressInfoError(globalObject, error));
    JSArray* result = newList(globalObject);
    for (struct addrinfo* info = found; info && !scope.exception(); info = info->ai_next) {
        SocketAddress address;
        memcpy(&address.storage, info->ai_addr, std::min<size_t>(info->ai_addrlen, sizeof(address.storage)));
        JSValue addressObject = socketAddressObject(globalObject, address, info->ai_addrlen);
        if (scope.exception()) [[unlikely]]
            break;
        listAppend(globalObject, result, PyTuple::create(globalObject, { jsNumber(info->ai_family), jsNumber(info->ai_socktype), jsNumber(info->ai_protocol),
            posixDecode(globalObject, info->ai_canonname ? info->ai_canonname : ""), addressObject }));
    }
    freeaddrinfo(found);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// ((host, port[, flowinfo[, scope_id]]), flags)
static JSValue socketGetNameInfo(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int flags;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, "getnameinfo"_s, 2, 2)) || !socketToInt(globalObject, args[1], flags))
        return { };
    auto* tuple = PyTuple::tryFrom(args[0]);
    if (!tuple)
        return raiseTypeError(globalObject, scope, "getnameinfo() argument 1 must be a tuple"_s);
    if (tuple->length() < 2 || tuple->length() > 4 || !isStr(tuple->at(0)))
        return raiseTypeError(globalObject, scope, "getnameinfo(): illegal sockaddr argument"_s);
    if (!audit(globalObject, "socket.getnameinfo"_s, tuple))
        return { };
    PosixBytes host;
    int port;
    unsigned flowInfo = 0;
    unsigned scopeId = 0;
    if (!socketToCString(globalObject, tuple->at(0), "getnameinfo"_s, 1, host) || !socketToInt(globalObject, tuple->at(1), port)
        || (tuple->length() > 2 && !posixToUnsignedMask(globalObject, tuple->at(2), flowInfo)) || (tuple->length() > 3 && !posixToUnsignedMask(globalObject, tuple->at(3), scopeId)))
        return { };
    if (flowInfo > 0xfffff)
        return raiseOverflowError(globalObject, scope, "getnameinfo(): flowinfo must be 0-1048575."_s);

    char service[NI_MAXSERV];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints { };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM; // So that a number will do for the port.
    hints.ai_flags = AI_NUMERICHOST; // Nothing is to be looked up yet.
    struct addrinfo* found = nullptr;
    if (int error = getaddrinfo(host.characters(), service, &hints, &found))
        RELEASE_AND_RETURN(scope, socketRaiseAddressInfoError(globalObject, error));
    ASCIILiteral complaint;
    int error = 0;
    char name[NI_MAXHOST];
    if (found->ai_next)
        complaint = "sockaddr resolved to multiple addresses"_s;
    else if (found->ai_family == AF_INET && tuple->length() != 2)
        complaint = "IPv4 sockaddr must be 2 tuple"_s;
    else {
        if (found->ai_family == AF_INET6) {
            auto* address = reinterpret_cast<struct sockaddr_in6*>(found->ai_addr);
            address->sin6_flowinfo = htonl(flowInfo);
            address->sin6_scope_id = scopeId;
        }
        AllowThreads allow(globalObject);
        error = getnameinfo(found->ai_addr, found->ai_addrlen, name, sizeof(name), service, sizeof(service), flags);
    }
    freeaddrinfo(found);
    if (!complaint.isNull())
        RELEASE_AND_RETURN(scope, socketRaiseOSError(globalObject, complaint));
    if (error)
        RELEASE_AND_RETURN(scope, socketRaiseAddressInfoError(globalObject, error));
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { posixDecode(globalObject, name), posixDecode(globalObject, service) }));
}

static JSValue socketGetDefaultTimeout(JSGlobalObject* globalObject) { return socketTimeoutObject(globalObject, socketDefaultTimeout(globalObject)); }

static JSValue socketSetDefaultTimeout(JSGlobalObject* globalObject, JSValue value)
{
    int64_t timeout;
    if (!socketParseTimeout(globalObject, value, timeout))
        return { };
    VM& vm = getVM(globalObject);
    posixState(globalObject)->set(vm, socketDefaultTimeoutKey(vm), makeInt(globalObject, timeout));
    return jsUndefined();
}

static JSValue socketIfNameIndex(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    struct if_nameindex* interfaces = if_nameindex();
    if (!interfaces)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    JSArray* result = newList(globalObject);
    for (struct if_nameindex* interface = interfaces; interface->if_index && !scope.exception(); ++interface)
        listAppend(globalObject, result, PyTuple::create(globalObject, { jsNumber(interface->if_index), posixDecode(globalObject, interface->if_name) }));
    if_freenameindex(interfaces);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue socketIfNameToIndex(JSGlobalObject* globalObject, JSValue value)
{
    PosixBytes name;
    if (!posixToFSBytes(globalObject, value, name))
        return { };
    errno = ENODEV;
    unsigned index = if_nametoindex(name.characters());
    if (!index)
        return posixError(globalObject);
    return jsNumber(index);
}

static JSValue socketIfIndexToName(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned index;
    if (failed(scope, socketToUnsigned(globalObject, value, "unsigned int"_s, index)))
        return { };
    errno = ENXIO;
    char name[IF_NAMESIZE + 1];
    if (!if_indextoname(index, name))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixDecode(globalObject, name));
}

template<bool (*function)(size_t, size_t&)>
static JSValue socketControlSize(JSGlobalObject* globalObject, const CallArgs& args, ASCIILiteral name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t length;
    size_t result;
    if (failed(scope, socketCheckCount(globalObject, args, ""_s, name, 1, 1)) || failed(scope, posixToSsize(globalObject, args[0], length)))
        return { };
    if (length < 0 || !function(length, result))
        return raiseOverflowError(globalObject, scope, name, "() argument out of range"_s);
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, result));
}

static JSValue socketCMSGLen(JSGlobalObject* globalObject, const CallArgs& args) { return socketControlSize<socketControlLength>(globalObject, args, "CMSG_LEN"_s); }
static JSValue socketCMSGSpace(JSGlobalObject* globalObject, const CallArgs& args) { return socketControlSize<socketControlSpace>(globalObject, args, "CMSG_SPACE"_s); }

// ---- The module ----

static constexpr NativeDef socketFunctions[] = {
    PY_FUNCTION("CMSG_LEN", socketCMSGLen, "(*args)"),
    PY_FUNCTION("CMSG_SPACE", socketCMSGSpace, "(*args)"),
    PY_FUNCTION("close", socketClose, "(object, /)"),
    PY_FUNCTION("dup", socketDup, "(object, /)"),
    PY_FUNCTION("getaddrinfo", socketGetAddrInfo, "(host, port, family=0, type=0, proto=0, flags=0)"),
    PY_FUNCTION("getdefaulttimeout", socketGetDefaultTimeout, "()"),
    PY_FUNCTION("gethostbyaddr", socketGetHostByAddr, "(*args)"),
    PY_FUNCTION("gethostbyname", socketGetHostByName, "(*args)"),
    PY_FUNCTION("gethostbyname_ex", socketGetHostByNameEx, "(*args)"),
    PY_FUNCTION("gethostname", socketGetHostName, "()"),
    PY_FUNCTION("getnameinfo", socketGetNameInfo, "(*args)"),
    PY_FUNCTION("getprotobyname", socketGetProtoByName, "(*args)"),
    PY_FUNCTION("getservbyname", socketGetServByName, "(*args)"),
    PY_FUNCTION("getservbyport", socketGetServByPort, "(*args)"),
    PY_FUNCTION("htonl", socketNtohl, "(integer, /)"),
    PY_FUNCTION("htons", socketNtohs, "(integer, /)"),
    PY_FUNCTION("if_indextoname", socketIfIndexToName, "(if_index, /)"),
    PY_FUNCTION("if_nameindex", socketIfNameIndex, "()"),
    PY_FUNCTION("if_nametoindex", socketIfNameToIndex, "(oname, /)"),
    PY_FUNCTION("inet_aton", socketInetAton, "(ip_addr, /)"),
    PY_FUNCTION("inet_ntoa", socketInetNtoa, "(packed_ip, /)"),
    PY_FUNCTION("inet_ntop", socketInetNtop, "(*args)"),
    PY_FUNCTION("inet_pton", socketInetPton, "(*args)"),
    PY_FUNCTION("ntohl", socketNtohl, "(integer, /)"),
    PY_FUNCTION("ntohs", socketNtohs, "(integer, /)"),
    PY_FUNCTION("setdefaulttimeout", socketSetDefaultTimeout, "(object, /)"),
    PY_FUNCTION("sethostname", socketSetHostName, "(*args)"),
    PY_FUNCTION("socketpair", socketPair, "(*args)"),
};

void initializeSocketModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->addFunctions(globalObject, socketFunctions);
    module->add(globalObject, "error"_s, rt.OSError());
    module->add(globalObject, "herror"_s, socketHostErrorType(globalObject));
    module->add(globalObject, "gaierror"_s, socketAddressInfoErrorType(globalObject));
    module->add(globalObject, "timeout"_s, rt.TimeoutError());
    module->add(globalObject, "SocketType"_s, socketType(globalObject));
    module->add(globalObject, "socket"_s, socketType(globalObject));
    module->add(globalObject, "has_ipv6"_s, jsBoolean(true));
    addSocketConstants(globalObject, module);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
