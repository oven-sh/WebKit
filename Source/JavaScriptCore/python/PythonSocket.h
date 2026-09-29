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


#pragma once

#include "PyStateObject.h"
#include "PythonPosix.h"
#include "PythonTime.h"

#if OS(UNIX)

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#if OS(DARWIN)
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#endif

// What the parts of _socket share: Modules/socketmodule.h of CPython, and what is at the top of Modules/socketmodule.c.

namespace JSC { namespace Python {

// socket_state
struct SocketModuleState final : NativeState {
    PYTHON_NATIVE_STATE(SocketModuleState);
    WriteBarrier<PyType> socketType;
    WriteBarrier<PyType> hostError; // socket.herror
    WriteBarrier<PyType> addressInfoError; // socket.gaierror
    int64_t defaultTimeout { -nanosecondsPerSecond }; // For new sockets. Negative is none.
};

template<typename Visitor>
void SocketModuleState::visit(Visitor& visitor)
{
    visitor.append(socketType);
    visitor.append(hostError);
    visitor.append(addressInfoError);
}

SocketModuleState& socketModuleState(JSGlobalObject*);

// sock_addr_t
union SocketAddress {
    struct sockaddr_in in;
    struct sockaddr sa;
    struct sockaddr_un un;
    struct sockaddr_in6 in6;
    struct sockaddr_storage storage;
#if OS(DARWIN)
    struct sockaddr_ctl ctl;
#endif
};

// PySocketSockObject
struct Socket final : NativeState {
    PYTHON_NATIVE_STATE(Socket);
    int descriptor { -1 };
    int family { 0 };
    int type { 0 };
    int protocol { 0 };
    int64_t timeout { -nanosecondsPerSecond }; // Negative is to wait for as long as it takes, and zero not to wait at all.
};

template<typename Visitor> void Socket::visit(Visitor&) { }

// SOCKLEN_T_LIMIT: the most that is put in a socklen_t
static constexpr size_t socketLengthLimit = 0x7fffffff;

// set_error(), set_herror() and set_gaierror()
JSValue raiseSocketError(JSGlobalObject*, ThrowScope&);
JSValue raiseHostError(JSGlobalObject*, ThrowScope&, int error);
JSValue raiseAddressInfoError(JSGlobalObject*, ThrowScope&, int error);
// PyErr_SetString(PyExc_OSError, ...)
JSValue raiseOSErrorSaying(JSGlobalObject*, ThrowScope&, const String& message);

// The "I" of PyArg_ParseTuple(): the low bits of an int, whatever else it has. Nothing if it raised.
std::optional<unsigned> toCUnsignedIntOfFormat(JSGlobalObject*, JSValue);
// The "et" of PyArg_ParseTuple(), with "idna": the name of a host. Nothing if it raised.
std::optional<CString> toHostNameOfFormat(JSGlobalObject*, JSValue, ASCIILiteral function);

// internal_setblocking(). False if it raised.
bool setBlocking(JSGlobalObject*, Socket&, bool);
// socket_parse_timeout(). Nothing if it raised.
std::optional<int64_t> parseSocketTimeout(JSGlobalObject*, JSValue);
// init_sockobject(). False if it raised.
bool initializeSocket(JSGlobalObject*, Socket&, int descriptor, int family, int type, int protocol);
// new_sockobject(). Null if it raised.
PyStateObject* newSocket(JSGlobalObject*, int descriptor, int family, int type, int protocol);
// _Py_set_inheritable(descriptor, 0, ...). False if it raised.
bool setNotInheritable(JSGlobalObject*, int descriptor);

// sock_call_ex(): calls what makes a system call, having waited for the socket to be ready if it has a time to wait no longer than, and again if it is interrupted or turns out not to have been ready.
// `function` says whether it succeeded, and leaves errno as it was left. If `error` is null, what goes wrong is raised. If it is not, what goes wrong is put there and nothing is raised, but that what sees to a signal may
// raise, and then it is -1. False if anything went wrong.
enum class WaitingTo : uint8_t { Read, Write };
bool callSocket(JSGlobalObject*, Socket&, WaitingTo, const ScopedLambda<bool()>& function, bool isConnecting, int* error, int64_t timeout);
// sock_call()
inline bool callSocket(JSGlobalObject* globalObject, Socket& socket, WaitingTo waitingTo, const ScopedLambda<bool()>& function) { return callSocket(globalObject, socket, waitingTo, function, false, nullptr, socket.timeout); }

// setipaddr(): the address that a name is for, which may be a number, or one of a few names that mean something here. How many bytes long the address is, or nothing if it raised.
std::optional<int> setIPAddress(JSGlobalObject*, const char* name, struct sockaddr* result, size_t resultSize, int family);
// make_ipv4_addr() and make_ipv6_addr(). Empty if it raised.
JSValue makeIPv4Address(JSGlobalObject*, const struct sockaddr_in&);
JSValue makeIPv6Address(JSGlobalObject*, const struct sockaddr_in6&);
// makesockaddr(): an address as a program has it. Empty if it raised.
JSValue makeSocketAddress(JSGlobalObject*, const struct sockaddr*, size_t length, int protocol);
// getsockaddrarg(): an address as a program gave it, for the kind of socket. False if it raised.
bool toSocketAddress(JSGlobalObject*, Socket&, JSValue, SocketAddress&, int& length, ASCIILiteral caller);
// getsockaddrlen(). False if it raised.
bool socketAddressLength(JSGlobalObject*, Socket&, socklen_t&);

// get_CMSG_LEN() and get_CMSG_SPACE(). Nothing if it is out of range.
std::optional<size_t> controlMessageLength(size_t);
std::optional<size_t> controlMessageSpace(size_t);

void initializeSocketType(JSGlobalObject*, SocketModuleState&);

} } // namespace JSC::Python

#endif // OS(UNIX)
