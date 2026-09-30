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
// What CPython finds out when it is configured. A kind of address is there if there is a header that says what one is.
#if OS(LINUX)
#include <sys/ioctl.h>
#if __has_include(<linux/netlink.h>)
#define HAVE_LINUX_NETLINK_H 1
#include <linux/netlink.h>
#else
#undef AF_NETLINK
#endif
#if __has_include(<linux/qrtr.h>)
#define HAVE_LINUX_QRTR_H 1
#include <linux/qrtr.h>
#else
#undef AF_QIPCRTR
#endif
#if __has_include(<netpacket/packet.h>)
#define HAVE_NETPACKET_PACKET_H 1
#include <netpacket/packet.h>
#endif
#if __has_include(<linux/tipc.h>)
#define HAVE_LINUX_TIPC_H 1
#include <linux/tipc.h>
#endif
#if __has_include(<linux/can.h>)
#define HAVE_LINUX_CAN_H 1
#include <linux/can.h>
#else
#undef AF_CAN
#undef PF_CAN
#endif
#if __has_include(<linux/can/raw.h>)
#define HAVE_LINUX_CAN_RAW_H 1
#include <linux/can/raw.h>
// They are not macros, so there is no asking. Linux has had them since 3.6 and 4.1.
#define HAVE_LINUX_CAN_RAW_FD_FRAMES 1
#define HAVE_LINUX_CAN_RAW_JOIN_FILTERS 1
#endif
#if __has_include(<linux/can/bcm.h>)
#define HAVE_LINUX_CAN_BCM_H 1
#include <linux/can/bcm.h>
#endif
#if __has_include(<linux/can/j1939.h>)
#define HAVE_LINUX_CAN_J1939_H 1
#include <linux/can/j1939.h>
#endif
#if __has_include(<linux/vm_sockets.h>)
#define HAVE_LINUX_VM_SOCKETS_H 1
#include <linux/vm_sockets.h>
#else
#undef AF_VSOCK
#endif
#if __has_include(<linux/netfilter_ipv4.h>)
#define HAVE_LINUX_NETFILTER_IPV4_H 1
#include <linux/netfilter_ipv4.h>
#endif
#if __has_include(<linux/if_alg.h>)
#define HAVE_SOCKADDR_ALG 1
#include <linux/if_alg.h>
#ifndef AF_ALG
#define AF_ALG 38
#endif
#ifndef SOL_ALG
#define SOL_ALG 279
#endif
// Linux 3.19
#ifndef ALG_SET_AEAD_ASSOCLEN
#define ALG_SET_AEAD_ASSOCLEN 4
#endif
#ifndef ALG_SET_AEAD_AUTHSIZE
#define ALG_SET_AEAD_AUTHSIZE 5
#endif
// Linux 4.8
#ifndef ALG_SET_PUBKEY
#define ALG_SET_PUBKEY 6
#endif
#ifndef ALG_OP_SIGN
#define ALG_OP_SIGN 2
#endif
#ifndef ALG_OP_VERIFY
#define ALG_OP_VERIFY 3
#endif
#endif
#else
// A system can have the number and nothing else.
#undef AF_NETLINK
#undef AF_QIPCRTR
#undef AF_CAN
#undef PF_CAN
#undef AF_VSOCK
#endif // OS(LINUX)

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
#if OS(LINUX)
#ifdef AF_NETLINK
    struct sockaddr_nl netlink;
#endif
#ifdef AF_QIPCRTR
    struct sockaddr_qrtr router;
#endif
#ifdef AF_VSOCK
    struct sockaddr_vm machine;
#endif
#ifdef HAVE_NETPACKET_PACKET_H
    struct sockaddr_ll link;
#endif
#ifdef HAVE_LINUX_TIPC_H
    struct sockaddr_tipc tipc;
#endif
#ifdef AF_CAN
    struct sockaddr_can can;
#endif
#ifdef HAVE_SOCKADDR_ALG
    struct sockaddr_alg algorithm;
#endif
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
JSValue makeSocketAddress(JSGlobalObject*, int descriptor, const struct sockaddr*, size_t length, int protocol);
// getsockaddrarg(): an address as a program gave it, for the kind of socket. False if it raised.
bool toSocketAddress(JSGlobalObject*, Socket&, JSValue, SocketAddress&, int& length, ASCIILiteral caller);
// getsockaddrlen(). False if it raised.
bool socketAddressLength(JSGlobalObject*, Socket&, socklen_t&);
#if OS(LINUX)
// The same three, for the kinds of address that there are only on Linux. Nothing if it is not one of those.
std::optional<JSValue> makeLinuxSocketAddress(JSGlobalObject*, int descriptor, const struct sockaddr*, int protocol);
std::optional<bool> toLinuxSocketAddress(JSGlobalObject*, Socket&, JSValue, SocketAddress&, int& length, ASCIILiteral caller);
std::optional<socklen_t> linuxSocketAddressLength(int family);
#endif

// get_CMSG_LEN() and get_CMSG_SPACE(). Nothing if it is out of range.
std::optional<size_t> controlMessageLength(size_t);
std::optional<size_t> controlMessageSpace(size_t);

void initializeSocketType(JSGlobalObject*, SocketModuleState&);

} } // namespace JSC::Python

#endif // OS(UNIX)
