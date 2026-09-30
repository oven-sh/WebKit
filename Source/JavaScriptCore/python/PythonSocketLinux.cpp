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

#if OS(LINUX)

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include <net/if.h>
#include <sys/ioctl.h>

// The kinds of address that there are only on Linux: what makesockaddr(), getsockaddrarg() and getsockaddrlen() of CPython's Modules/socketmodule.c do about them.

namespace JSC { namespace Python {

namespace {

// A tuple, as PyArg_ParseTuple() takes one apart. What is said when it is not what is wanted is what comes after the ';' of the format if there is one. If not, it begins with what comes after the ':', if there is that.
class Fields {
public:
    Fields(JSGlobalObject* globalObject, PyTuple* tuple, ASCIILiteral function, ASCIILiteral message)
        : m_globalObject(globalObject)
        , m_tuple(tuple)
        , m_function(function)
        , m_message(message)
    {
    }

    unsigned size() const { return m_tuple->length(); }

    // False if it raised.
    bool checkCount(unsigned minimum, unsigned maximum)
    {
        auto scope = DECLARE_THROW_SCOPE(m_globalObject->vm());
        unsigned given = size();
        if (given >= minimum && given <= maximum)
            return true;
        if (!m_message.isNull()) {
            raiseTypeError(m_globalObject, scope, m_message);
            return false;
        }
        unsigned wanted = given < minimum ? minimum : maximum;
        raiseTypeError(m_globalObject, scope, concatenate(m_function.isNull() ? "function"_s : m_function, m_function.isNull() ? ""_s : "()"_s, " takes "_s,
            minimum == maximum ? "exactly"_s : given < minimum ? "at least"_s : "at most"_s, ' ', wanted, " argument"_s, wanted == 1 ? ""_s : "s"_s, " ("_s, given, " given)"_s));
        return false;
    }

    // "B", "H" and "I": PyLong_AsUnsignedLongMask(), of which as much is kept as there is room for.
    std::optional<uint64_t> bits(unsigned index)
    {
        auto scope = DECLARE_THROW_SCOPE(m_globalObject->vm());
        JSValue number = toInt(m_globalObject, m_tuple->at(index));
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        return lowBitsOfInt(number);
    }

    // "k" and "K", which say for themselves that it is not an int.
    std::optional<uint64_t> bitsOfInt(unsigned index)
    {
        VM& vm = m_globalObject->vm();
        if (!typeOf(m_globalObject, m_tuple->at(index))->lookup(vm, vm.pythonNames().dunder_index)) {
            raiseMustBe(index, "int"_s);
            return std::nullopt;
        }
        return bits(index);
    }

    // "i"
    std::optional<int> integer(unsigned index) { return toCIntOfFormat(m_globalObject, m_tuple->at(index)); }

    // "s"
    std::optional<CString> text(unsigned index)
    {
        auto scope = DECLARE_THROW_SCOPE(m_globalObject->vm());
        JSValue value = m_tuple->at(index);
        if (!stringIn(value)) {
            raiseMustBe(index, "str"_s);
            return std::nullopt;
        }
        auto encoded = encodeString(m_globalObject, value, "utf-8"_s, String());
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        auto bytes = encoded->span();
        if (WTF::find(bytes, static_cast<uint8_t>(0)) != notFound) {
            raiseValueError(m_globalObject, scope, "embedded null character"_s);
            return std::nullopt;
        }
        return CString(byteCast<char>(bytes));
    }

    // "y*". What is raised in getting at the bytes stands.
    Buffer bytes(unsigned index) { return bufferOf(m_globalObject, m_tuple->at(index)); }

    // "O&", with PyUnicode_FSConverter()
    std::optional<CString> fileSystemEncoded(unsigned index) { return toFileSystemEncoded(m_globalObject, m_tuple->at(index)); }

private:
    // converterr(), and then seterror()
    void raiseMustBe(unsigned index, ASCIILiteral expected)
    {
        auto scope = DECLARE_THROW_SCOPE(m_globalObject->vm());
        if (!m_message.isNull()) {
            raiseTypeError(m_globalObject, scope, m_message);
            return;
        }
        raiseTypeError(m_globalObject, scope, concatenate(m_function.isNull() ? ""_s : m_function, m_function.isNull() ? ""_s : "() "_s, "argument "_s, index + 1, " must be "_s, expected, ", not "_s, typeNameOfArgument(m_globalObject, m_tuple->at(index))));
    }

    JSGlobalObject* m_globalObject;
    PyTuple* m_tuple;
    ASCIILiteral m_function;
    ASCIILiteral m_message;
};

// False if it raised.
bool checkIsTuple(JSGlobalObject* globalObject, JSValue value, ASCIILiteral before, ASCIILiteral family)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isTuple(value))
        return true;
    raiseTypeError(globalObject, scope, concatenate(before, family, " address must be tuple, not "_s, typeName(globalObject, value)));
    return false;
}

// The name of the interface that has that number, which is asked of the socket. Nothing if it has none.
CString interfaceName(int descriptor, int index)
{
    struct ifreq request;
    if (index) {
        request.ifr_ifindex = index;
        if (!ioctl(descriptor, SIOCGIFNAME, &request))
            return CString(unsafeSpan(request.ifr_name));
    }
    return CString(""_span);
}

JSValue stringFromUTF8(JSGlobalObject* globalObject, std::span<const char> text)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String decoded = decodeUTF8(globalObject, byteCast<uint8_t>(text), "strict"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return jsString(globalObject->vm(), decoded);
}

template<size_t size>
std::span<const char> upToZero(const unsigned char (&characters)[size])
{
    auto* start = std::bit_cast<const char*>(&characters[0]);
    return unsafeMakeSpan(start, strnlen(start, size));
}

#ifdef AF_CAN
// The interface that an address of AF_CAN begins with. None is any of them. False if it raised.
bool takeInterfaceOfCAN(JSGlobalObject* globalObject, Socket& socket, const CString& name, struct sockaddr_can& address)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    struct ifreq request;
    if (!name.length())
        request.ifr_ifindex = 0;
    else if (name.length() < sizeof(request.ifr_name)) {
        strncpy(request.ifr_name, name.data(), sizeof(request.ifr_name));
        request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';
        if (ioctl(socket.descriptor, SIOCGIFINDEX, &request) < 0) {
            raiseSocketError(globalObject, scope);
            return false;
        }
    } else {
        raiseOSErrorSaying(globalObject, scope, "AF_CAN interface name too long"_s);
        return false;
    }
    address.can_family = AF_CAN;
    address.can_ifindex = request.ifr_ifindex;
    return true;
}
#endif

} // anonymous namespace

#define TAKE(name, expression) \
    auto name##Taken = (expression); \
    RETURN_IF_EXCEPTION(scope, false); \
    auto name = *name##Taken

std::optional<JSValue> makeLinuxSocketAddress(JSGlobalObject* globalObject, int descriptor, const struct sockaddr* address, int protocol)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto number = [&] (uint64_t value) { return intFromUInt64(globalObject, value); };
    UNUSED_PARAM(descriptor);
    UNUSED_PARAM(protocol);
    switch (address->sa_family) {
#ifdef AF_NETLINK
    case AF_NETLINK: {
        auto& netlink = *std::bit_cast<const struct sockaddr_nl*>(address);
        return PyTuple::create(globalObject, { number(netlink.nl_pid), number(netlink.nl_groups) });
    }
#endif
#ifdef AF_QIPCRTR
    case AF_QIPCRTR: {
        auto& router = *std::bit_cast<const struct sockaddr_qrtr*>(address);
        return PyTuple::create(globalObject, { number(router.sq_node), number(router.sq_port) });
    }
#endif
#ifdef AF_VSOCK
    case AF_VSOCK: {
        auto& machine = *std::bit_cast<const struct sockaddr_vm*>(address);
        return PyTuple::create(globalObject, { number(machine.svm_cid), number(machine.svm_port) });
    }
#endif
#ifdef HAVE_NETPACKET_PACKET_H
    case AF_PACKET: {
        auto& link = *std::bit_cast<const struct sockaddr_ll*>(address);
        CString name = interfaceName(descriptor, link.sll_ifindex);
        JSValue interface = stringFromUTF8(globalObject, name.span());
        RETURN_IF_EXCEPTION(scope, JSValue());
        return PyTuple::create(globalObject, { interface, jsNumber(ntohs(link.sll_protocol)), jsNumber(link.sll_pkttype), jsNumber(link.sll_hatype), newBytes(globalObject, std::span<const uint8_t>(link.sll_addr).first(std::min<size_t>(link.sll_halen, sizeof(link.sll_addr)))) });
    }
#endif
#ifdef HAVE_LINUX_TIPC_H
    case AF_TIPC: {
        auto& tipc = *std::bit_cast<const struct sockaddr_tipc*>(address);
        switch (tipc.addrtype) {
        case TIPC_ADDR_NAMESEQ:
            return PyTuple::create(globalObject, { number(tipc.addrtype), number(tipc.addr.nameseq.type), number(tipc.addr.nameseq.lower), number(tipc.addr.nameseq.upper), number(static_cast<unsigned>(tipc.scope)) });
        case TIPC_ADDR_NAME:
            return PyTuple::create(globalObject, { number(tipc.addrtype), number(tipc.addr.name.name.type), number(tipc.addr.name.name.instance), number(tipc.addr.name.name.instance), number(static_cast<unsigned>(tipc.scope)) });
        case TIPC_ADDR_ID:
            return PyTuple::create(globalObject, { number(tipc.addrtype), number(tipc.addr.id.node), number(tipc.addr.id.ref), jsNumber(0), number(static_cast<unsigned>(tipc.scope)) });
        }
        return raiseValueError(globalObject, scope, "Invalid address type"_s);
    }
#endif
#ifdef AF_CAN
    case AF_CAN: {
        auto& can = *std::bit_cast<const struct sockaddr_can*>(address);
        CString name = interfaceName(descriptor, can.can_ifindex);
        JSValue interface = decodeFileSystemBytes(globalObject, name.span());
        RETURN_IF_EXCEPTION(scope, JSValue());
        switch (protocol) {
#ifdef CAN_ISOTP
        case CAN_ISOTP:
            return PyTuple::create(globalObject, { interface, number(can.can_addr.tp.rx_id), number(can.can_addr.tp.tx_id) });
#endif
#ifdef CAN_J1939
        case CAN_J1939:
            return PyTuple::create(globalObject, { interface, number(can.can_addr.j1939.name), number(can.can_addr.j1939.pgn), jsNumber(can.can_addr.j1939.addr) });
#endif
        }
        return PyTuple::create(globalObject, { interface });
    }
#endif
#ifdef HAVE_SOCKADDR_ALG
    case AF_ALG: {
        auto& algorithm = *std::bit_cast<const struct sockaddr_alg*>(address);
        JSValue type = stringFromUTF8(globalObject, upToZero(algorithm.salg_type));
        RETURN_IF_EXCEPTION(scope, JSValue());
        JSValue name = stringFromUTF8(globalObject, upToZero(algorithm.salg_name));
        RETURN_IF_EXCEPTION(scope, JSValue());
        // The "H" of Py_BuildValue() is given an int and makes what it can of it.
        return PyTuple::create(globalObject, { type, name, jsNumber(static_cast<int>(algorithm.salg_feat)), jsNumber(static_cast<int>(algorithm.salg_mask)) });
    }
#endif
    }
    return std::nullopt;
}

std::optional<bool> toLinuxSocketAddress(JSGlobalObject* globalObject, Socket& socket, JSValue value, SocketAddress& result, int& length, ASCIILiteral caller)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto isTupleFor = [&] (ASCIILiteral family) {
        if (isTuple(value))
            return true;
        raiseTypeError(globalObject, scope, concatenate(caller, "(): "_s, family, " address must be tuple, not "_s, typeName(globalObject, value)));
        return false;
    };
    switch (socket.family) {
#ifdef AF_NETLINK
    case AF_NETLINK: {
        auto& address = result.netlink;
        zeroBytes(address);
        if (!isTupleFor("AF_NETLINK"_s))
            return false;
        Fields fields(globalObject, asTuple(value), { }, "AF_NETLINK address must be a pair (pid, groups)"_s);
        if (!fields.checkCount(2, 2))
            return false;
        TAKE(process, fields.bits(0));
        TAKE(groups, fields.bits(1));
        address.nl_family = AF_NETLINK;
        address.nl_pid = static_cast<unsigned>(process);
        address.nl_groups = static_cast<unsigned>(groups);
        length = sizeof(address);
        return true;
    }
#endif
#ifdef AF_QIPCRTR
    case AF_QIPCRTR: {
        auto& address = result.router;
        zeroBytes(address);
        if (!checkIsTuple(globalObject, value, "getsockaddrarg: "_s, "AF_QIPCRTR"_s))
            return false;
        Fields fields(globalObject, asTuple(value), "getsockaddrarg"_s, { });
        if (!fields.checkCount(2, 2))
            return false;
        TAKE(node, fields.bits(0));
        TAKE(port, fields.bits(1));
        address.sq_family = AF_QIPCRTR;
        address.sq_node = static_cast<unsigned>(node);
        address.sq_port = static_cast<unsigned>(port);
        length = sizeof(address);
        return true;
    }
#endif
#ifdef AF_VSOCK
    case AF_VSOCK: {
        auto& address = result.machine;
        zeroBytes(address);
        if (!checkIsTuple(globalObject, value, "getsockaddrarg: "_s, "AF_VSOCK"_s))
            return false;
        Fields fields(globalObject, asTuple(value), "getsockaddrarg"_s, { });
        if (!fields.checkCount(2, 2))
            return false;
        TAKE(context, fields.bits(0));
        TAKE(port, fields.bits(1));
        address.svm_family = socket.family;
        address.svm_port = static_cast<unsigned>(port);
        address.svm_cid = static_cast<unsigned>(context);
        length = sizeof(address);
        return true;
    }
#endif
#ifdef HAVE_NETPACKET_PACKET_H
    case AF_PACKET: {
        if (!isTupleFor("AF_PACKET"_s))
            return false;
        Fields fields(globalObject, asTuple(value), { }, "AF_PACKET address must be a tuple of two to five elements"_s);
        CString name;
        int protocol = 0;
        int packetType = PACKET_HOST;
        int hardwareType = 0;
        Buffer hardwareAddress;
        auto parse = [&] {
            if (!fields.checkCount(2, 5))
                return false;
            TAKE(givenName, fields.text(0));
            name = WTF::move(givenName);
            TAKE(givenProtocol, fields.integer(1));
            protocol = givenProtocol;
            if (fields.size() > 2) {
                TAKE(given, fields.integer(2));
                packetType = given;
            }
            if (fields.size() > 3) {
                TAKE(given, fields.integer(3));
                hardwareType = given;
            }
            if (fields.size() > 4) {
                hardwareAddress = fields.bytes(4);
                RETURN_IF_EXCEPTION(scope, false);
            }
            return true;
        };
        if (!parse()) {
            if (catchException(globalObject, BuiltinType::OverflowError))
                raise(globalObject, scope, BuiltinType::OverflowError, concatenate(caller, "(): address argument out of range"_s));
            return false;
        }
        struct ifreq request;
        strncpy(request.ifr_name, name.data(), sizeof(request.ifr_name));
        request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';
        if (ioctl(socket.descriptor, SIOCGIFINDEX, &request) < 0) {
            raiseSocketError(globalObject, scope);
            return false;
        }
        if (hardwareAddress && hardwareAddress.size() > 8) {
            raiseValueError(globalObject, scope, "Hardware address must be 8 bytes or less"_s);
            return false;
        }
        if (protocol < 0 || protocol > 0xffff) {
            raise(globalObject, scope, BuiltinType::OverflowError, concatenate(caller, "(): proto must be 0-65535."_s));
            return false;
        }
        auto& address = result.link;
        zeroBytes(address);
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(static_cast<uint16_t>(protocol));
        address.sll_ifindex = request.ifr_ifindex;
        address.sll_pkttype = static_cast<unsigned char>(packetType);
        address.sll_hatype = static_cast<unsigned short>(hardwareType);
        if (hardwareAddress) {
            memcpy(address.sll_addr, hardwareAddress.data(), hardwareAddress.size());
            address.sll_halen = static_cast<unsigned char>(hardwareAddress.size());
        }
        length = sizeof(address);
        return true;
    }
#endif
#ifdef HAVE_LINUX_TIPC_H
    case AF_TIPC: {
        if (!isTupleFor("AF_TIPC"_s))
            return false;
        Fields fields(globalObject, asTuple(value), { }, "AF_TIPC address must be a tuple (addr_type, v1, v2, v3[, scope])"_s);
        if (!fields.checkCount(4, 5))
            return false;
        TAKE(kind, fields.bits(0));
        TAKE(first, fields.bits(1));
        TAKE(second, fields.bits(2));
        TAKE(third, fields.bits(3));
        uint64_t reach = TIPC_CLUSTER_SCOPE;
        if (fields.size() > 4) {
            TAKE(given, fields.bits(4));
            reach = given;
        }
        auto& address = result.tipc;
        zeroBytes(address);
        address.family = AF_TIPC;
        address.scope = static_cast<signed char>(reach);
        address.addrtype = static_cast<unsigned char>(kind);
        switch (static_cast<unsigned>(kind)) {
        case TIPC_ADDR_NAMESEQ:
            address.addr.nameseq.type = static_cast<unsigned>(first);
            address.addr.nameseq.lower = static_cast<unsigned>(second);
            address.addr.nameseq.upper = static_cast<unsigned>(third);
            break;
        case TIPC_ADDR_NAME:
            address.addr.name.name.type = static_cast<unsigned>(first);
            address.addr.name.name.instance = static_cast<unsigned>(second);
            break;
        case TIPC_ADDR_ID:
            address.addr.id.node = static_cast<unsigned>(first);
            address.addr.id.ref = static_cast<unsigned>(second);
            break;
        default:
            raiseTypeError(globalObject, scope, "Invalid address type"_s);
            return false;
        }
        length = sizeof(address);
        return true;
    }
#endif
#ifdef AF_CAN
    case AF_CAN: {
        auto& address = result.can;
        // These two do not look what it is, and PyArg_ParseTuple() has something to say of that.
        auto isTupleForFormat = [&] {
            if (isTuple(value))
                return true;
            raise(globalObject, scope, BuiltinType::SystemError, "new style getargs format but argument is not a tuple"_s);
            return false;
        };
        switch (socket.protocol) {
        case CAN_RAW:
        case CAN_BCM: {
            zeroBytes(address);
            if (!isTupleFor("AF_CAN"_s))
                return false;
            Fields fields(globalObject, asTuple(value), { }, "AF_CAN address must be a tuple (interface, )"_s);
            if (!fields.checkCount(1, 1))
                return false;
            TAKE(name, fields.fileSystemEncoded(0));
            if (!takeInterfaceOfCAN(globalObject, socket, name, address))
                return false;
            length = sizeof(address);
            return true;
        }
#ifdef CAN_ISOTP
        case CAN_ISOTP: {
            zeroBytes(address);
            if (!isTupleForFormat())
                return false;
            Fields fields(globalObject, asTuple(value), { }, { });
            if (!fields.checkCount(3, 3))
                return false;
            TAKE(name, fields.fileSystemEncoded(0));
            TAKE(receives, fields.bitsOfInt(1));
            TAKE(sends, fields.bitsOfInt(2));
            if (!takeInterfaceOfCAN(globalObject, socket, name, address))
                return false;
            address.can_addr.tp.rx_id = static_cast<canid_t>(receives);
            address.can_addr.tp.tx_id = static_cast<canid_t>(sends);
            length = sizeof(address);
            return true;
        }
#endif
#ifdef CAN_J1939
        case CAN_J1939: {
            zeroBytes(address);
            if (!isTupleForFormat())
                return false;
            Fields fields(globalObject, asTuple(value), { }, { });
            if (!fields.checkCount(4, 4))
                return false;
            TAKE(name, fields.fileSystemEncoded(0));
            TAKE(deviceName, fields.bitsOfInt(1));
            TAKE(group, fields.bits(2));
            TAKE(deviceAddress, fields.bits(3));
            if (!takeInterfaceOfCAN(globalObject, socket, name, address))
                return false;
            address.can_addr.j1939.name = deviceName;
            address.can_addr.j1939.pgn = static_cast<uint32_t>(group);
            address.can_addr.j1939.addr = static_cast<uint8_t>(deviceAddress);
            length = sizeof(address);
            return true;
        }
#endif
        }
        raiseOSErrorSaying(globalObject, scope, concatenate(caller, "(): unsupported CAN protocol"_s));
        return false;
    }
#endif
#ifdef HAVE_SOCKADDR_ALG
    case AF_ALG: {
        auto& address = result.algorithm;
        zeroBytes(address);
        address.salg_family = AF_ALG;
        if (!isTupleFor("AF_ALG"_s))
            return false;
        Fields fields(globalObject, asTuple(value), { }, "AF_ALG address must be a tuple (type, name[, feat[, mask]])"_s);
        if (!fields.checkCount(2, 4))
            return false;
        TAKE(type, fields.text(0));
        TAKE(name, fields.text(1));
        // "H", into fields that are wider than that, of which it fills the first half
        if (fields.size() > 2) {
            TAKE(features, fields.bits(2));
            uint16_t narrowed = static_cast<uint16_t>(features);
            memcpy(&address.salg_feat, &narrowed, sizeof(narrowed));
        }
        if (fields.size() > 3) {
            TAKE(mask, fields.bits(3));
            uint16_t narrowed = static_cast<uint16_t>(mask);
            memcpy(&address.salg_mask, &narrowed, sizeof(narrowed));
        }
        if (type.length() >= sizeof(address.salg_type)) {
            raiseValueError(globalObject, scope, "AF_ALG type too long."_s);
            return false;
        }
        memcpy(address.salg_type, type.data(), type.length());
        if (name.length() >= sizeof(address.salg_name)) {
            raiseValueError(globalObject, scope, "AF_ALG name too long."_s);
            return false;
        }
        memcpy(address.salg_name, name.data(), name.length());
        length = sizeof(address);
        return true;
    }
#endif
    }
    return std::nullopt;
}

std::optional<socklen_t> linuxSocketAddressLength(int family)
{
    switch (family) {
#ifdef AF_NETLINK
    case AF_NETLINK:
        return sizeof(struct sockaddr_nl);
#endif
#ifdef AF_QIPCRTR
    case AF_QIPCRTR:
        return sizeof(struct sockaddr_qrtr);
#endif
#ifdef AF_VSOCK
    case AF_VSOCK:
        return sizeof(struct sockaddr_vm);
#endif
#ifdef HAVE_NETPACKET_PACKET_H
    case AF_PACKET:
        return sizeof(struct sockaddr_ll);
#endif
#ifdef HAVE_LINUX_TIPC_H
    case AF_TIPC:
        return sizeof(struct sockaddr_tipc);
#endif
#ifdef AF_CAN
    case AF_CAN:
        return sizeof(struct sockaddr_can);
#endif
#ifdef HAVE_SOCKADDR_ALG
    case AF_ALG:
        return sizeof(struct sockaddr_alg);
#endif
    }
    return std::nullopt;
}

} } // namespace JSC::Python

#endif // OS(LINUX)
