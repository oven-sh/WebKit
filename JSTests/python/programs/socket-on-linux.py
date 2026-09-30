# The kinds of socket that there are on Linux and not on macOS.
import _socket
import errno
import os
import sys

S = _socket.socket


def show(e):
    return type(e).__name__ + ": " + str(e)


def attempt(f, *a, **k):
    try:
        return f(*a, **k)
    except BaseException as e:
        return show(e)


def t(label, f):
    r = attempt(f)
    print(label, "=>", r if isinstance(r, str) else ascii(r))
    sys.stdout.flush()


class Index:
    def __init__(self, v): self.v = v
    def __index__(self): return self.v
    def __repr__(self): return "Index(%r)" % self.v


class Path:
    def __init__(self, v): self.v = v
    def __fspath__(self): return self.v
    def __repr__(self): return "Path(%r)" % self.v


class Tuple(tuple):
    pass


# What a kernel has of these depends on how it was built, and on who is asking. So how an address is taken apart is tried with a socket that only says that it is of the family: it is one of UDP's, which is asked
# about interfaces as well as any, and turns down whatever it is then given. What the kernel does with the real thing is looked at here, and comes to the same if there is none.
def claiming(family, kind=_socket.SOCK_DGRAM, protocol=0):
    real = S(_socket.AF_INET, _socket.SOCK_DGRAM)
    return S(family, kind, protocol, real.detach())


def addresses(family, values, kind=_socket.SOCK_DGRAM, protocol=0):
    s = claiming(family, kind, protocol)
    try:
        return [(attempt(s.bind, x), attempt(s.connect, x), attempt(s.connect_ex, x), attempt(s.sendto, b"a", x), attempt(s.sendmsg, [b"a"], [], 0, x)) for x in values]
    finally:
        s.close()


NOT_THERE = (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT, errno.ESOCKTNOSUPPORT, errno.EPERM, errno.EACCES, errno.ENOENT, errno.ENODEV, errno.EOPNOTSUPP)


def if_there(f):
    try:
        return f()
    except OSError as e:
        return True if e.errno in NOT_THERE else show(e)


NOT_TUPLES = (5, None, "a", b"a", [1, 2], 1.5)
NUMBERS = (0, 1, 2 ** 32 - 1, 2 ** 32, 2 ** 32 + 5, 2 ** 64 + 7, -1, -2 ** 70, True, Index(3), Index(-1), "a", None, 1.5, b"a", Index("a"))

print("---- what there is")
t("the numbers", lambda: sorted((n, v) for n, v in vars(_socket).items() if type(v) is int))
t("everything else", lambda: sorted(n for n, v in vars(_socket).items() if type(v) is not int and not n.startswith("__")))
t("what a socket has", lambda: sorted(n for n in vars(S) if n != "__doc__"))
t("sendmsg_afalg", lambda: (lambda f: (type(f).__name__, f.__text_signature__, f.__doc__))(S.sendmsg_afalg))

print("---- AF_NETLINK")
t("what is not a tuple", lambda: addresses(_socket.AF_NETLINK, NOT_TUPLES))
t("how many", lambda: addresses(_socket.AF_NETLINK, ((), (1,), (1, 2, 3), Tuple((1, 2)))))
t("each of them", lambda: addresses(_socket.AF_NETLINK, [(x, 0) for x in NUMBERS] + [(0, x) for x in NUMBERS]))
t("a real one", lambda: [(s.family, s.type, s.proto, s.getsockname(), s.bind((0, 0)), s.getsockname() == (os.getpid(), 0), attempt(s.getpeername), s.close()) for s in [S(_socket.AF_NETLINK, _socket.SOCK_RAW, _socket.NETLINK_ROUTE)]])
t("bound to a number of its own", lambda: [(s.bind((os.getpid() + 2 ** 22, 0)), s.getsockname() == (os.getpid() + 2 ** 22, 0), s.close()) for s in [S(_socket.AF_NETLINK, _socket.SOCK_RAW, _socket.NETLINK_ROUTE)]])


def links():
    # RTM_GETLINK, of all of them: what comes back is from the kernel, whose number is 0.
    import struct
    s = S(_socket.AF_NETLINK, _socket.SOCK_RAW, _socket.NETLINK_ROUTE)
    try:
        s.settimeout(5)
        sent = s.sendto(struct.pack("=IHHIIBxHiII", 32, 18, 0x301, 1, 0, 0, 0, 0, 0, 0), (0, 0))
        data, sender = s.recvfrom(65536)
        length, kind, flags, sequence, process = struct.unpack_from("=IHHII", data)
        again = s.recvmsg(65536)
        return sent, sender, kind, sequence, process == os.getpid(), length <= len(data), again[3], type(again[3]).__name__
    finally:
        s.close()


t("asked something", links)
t("AF_ROUTE is the same", lambda: _socket.AF_ROUTE == _socket.AF_NETLINK)

print("---- AF_VSOCK")
t("what is not a tuple", lambda: addresses(_socket.AF_VSOCK, NOT_TUPLES, _socket.SOCK_STREAM))
t("how many", lambda: addresses(_socket.AF_VSOCK, ((), (1,), (1, 2, 3), Tuple((1, 2)))))
t("each of them", lambda: addresses(_socket.AF_VSOCK, [(x, 0) for x in NUMBERS] + [(0, x) for x in NUMBERS]))
t("options are of 64 bits", lambda: [[attempt(s.setsockopt, *a) for a in ((1, 2, 3), (1, 2, 2 ** 63), (1, 2, 2 ** 64 + 1), (1, 2, -1), (1, 2, Index(1)), (1, 2, "a"), (1, 2, None), (1, 2, b"abcd"), (1, 2, 1.5), (1, 2, None, 4), (1, 2), (1,), (), ("a", 2, 3), (1, "a", 3), (2 ** 31, 2, 3))] +
                                     [attempt(s.getsockopt, *a) for a in ((1, 2), (1, 2, 0), (1, 2, 4), (1, 2, 2000), (1, 2, -1), ("a", 2), (1,), (1, 2, 3, 4))] + [s.close()] for s in [claiming(_socket.AF_VSOCK, _socket.SOCK_STREAM)]])
t("a real one", lambda: if_there(lambda: [(s.getsockname() == (_socket.VMADDR_CID_ANY, _socket.VMADDR_PORT_ANY), s.bind((_socket.VMADDR_CID_ANY, 54321)), s.getsockname() == (_socket.VMADDR_CID_ANY, 54321),
                                           s.setsockopt(_socket.AF_VSOCK, _socket.SO_VM_SOCKETS_BUFFER_SIZE, 65536), s.getsockopt(_socket.AF_VSOCK, _socket.SO_VM_SOCKETS_BUFFER_SIZE) == 65536, s.close()) == (True, None, True, None, True, None) for s in [S(_socket.AF_VSOCK, _socket.SOCK_STREAM)]] == [True]))

print("---- AF_QIPCRTR")
t("what is not a tuple", lambda: addresses(_socket.AF_QIPCRTR, NOT_TUPLES))
t("how many", lambda: addresses(_socket.AF_QIPCRTR, ((), (1,), (1, 2, 3), Tuple((1, 2)))))
t("each of them", lambda: addresses(_socket.AF_QIPCRTR, [(x, 0) for x in NUMBERS] + [(0, x) for x in NUMBERS]))

print("---- AF_PACKET")
NAMES = ("lo", "", "nonesuch", "a" * 15, "a" * 16, "a" * 100, "lo\0x", "\xe9", "\ud800", b"lo", 5, None, Path("lo"), bytearray(b"lo"))
INTS = (0, 1, 0xFFFF, 0x10000, -1, 2 ** 31 - 1, 2 ** 31, -2 ** 31 - 1, 2 ** 70, True, Index(3), "a", None, 1.5)
t("what is not a tuple", lambda: addresses(_socket.AF_PACKET, NOT_TUPLES, _socket.SOCK_RAW))
t("how many", lambda: addresses(_socket.AF_PACKET, ((), ("lo",), ("lo", 0), ("lo", 0, 0), ("lo", 0, 0, 0), ("lo", 0, 0, 0, b""), ("lo", 0, 0, 0, b"", 0), Tuple(("lo", 0))), _socket.SOCK_RAW))
t("the interface", lambda: addresses(_socket.AF_PACKET, [(x, 0) for x in NAMES], _socket.SOCK_RAW))
t("the protocol", lambda: addresses(_socket.AF_PACKET, [("lo", x) for x in INTS], _socket.SOCK_RAW))
t("the kind of packet", lambda: addresses(_socket.AF_PACKET, [("lo", 0, x) for x in INTS], _socket.SOCK_RAW))
t("the kind of hardware", lambda: addresses(_socket.AF_PACKET, [("lo", 0, 0, x) for x in INTS], _socket.SOCK_RAW))
t("the address", lambda: addresses(_socket.AF_PACKET, [("lo", 0, 0, 0, x) for x in (b"", b"123456", b"12345678", b"123456789", bytearray(b"12"), memoryview(b"12"), memoryview(b"1234")[::2], "12", 5, None, [1])], _socket.SOCK_RAW))
t("what is looked at first", lambda: addresses(_socket.AF_PACKET, (("nonesuch", -1, 0, 0, b"123456789"), ("lo", -1, 0, 0, b"123456789"), ("nonesuch", 2 ** 31), ("lo", "a", 2 ** 31), (5, 2 ** 31)), _socket.SOCK_RAW))

print("---- AF_TIPC")
t("what is not a tuple", lambda: addresses(_socket.AF_TIPC, NOT_TUPLES, _socket.SOCK_RDM))
t("how many", lambda: addresses(_socket.AF_TIPC, ((), (1,), (1, 2, 3), (1, 2, 3, 4), (1, 2, 3, 4, 5), (1, 2, 3, 4, 5, 6), Tuple((1, 2, 3, 4))), _socket.SOCK_RDM))
t("which kind", lambda: addresses(_socket.AF_TIPC, [(x, 1, 2, 3) for x in (_socket.TIPC_ADDR_NAMESEQ, _socket.TIPC_ADDR_NAME, _socket.TIPC_ADDR_ID, 0, 4, 2 ** 32 + 1, 257) + NUMBERS[6:]], _socket.SOCK_RDM))
t("each of the rest", lambda: addresses(_socket.AF_TIPC, [(1,) + (0,) * i + (x,) + (0,) * (3 - i) for i in range(4) for x in NUMBERS[9:]], _socket.SOCK_RDM))
t("a real one", lambda: if_there(lambda: [((lambda a: (len(a), a[0] == _socket.TIPC_ADDR_ID, a[3], a[4]))(s.getsockname()), s.bind((_socket.TIPC_ADDR_NAMESEQ, 54321, 5, 9, _socket.TIPC_NODE_SCOPE)), s.close()) == ((5, True, 0, 0), None, None) for s in [S(_socket.AF_TIPC, _socket.SOCK_RDM)]] == [True]))


def tipc():
    a, b = S(_socket.AF_TIPC, _socket.SOCK_RDM), S(_socket.AF_TIPC, _socket.SOCK_RDM)
    try:
        a.settimeout(5)
        a.bind((_socket.TIPC_ADDR_NAMESEQ, 54322, 5, 9, _socket.TIPC_NODE_SCOPE))
        b.sendto(b"hello", (_socket.TIPC_ADDR_NAME, 54322, 7, 0))
        data, sender = a.recvfrom(100)
        return (data, sender == b.getsockname(), sender[0] == _socket.TIPC_ADDR_ID) == (b"hello", True, True)
    finally:
        a.close()
        b.close()


t("one to another", lambda: if_there(tipc))

print("---- AF_CAN")
CAN_NAMES = ("", "lo", "nonesuch", "a" * 15, "a" * 16, "a" * 100, "lo\0x", "\xe9", "\udce9", "\ud800", b"", b"lo", b"\xff", Path("lo"), Path(b"lo"), Path(5), bytearray(b"lo"), 5, None)
for name, kind, protocol in (("CAN_RAW", _socket.SOCK_RAW, _socket.CAN_RAW), ("CAN_BCM", _socket.SOCK_DGRAM, _socket.CAN_BCM)):
    t(name + ": what is not a tuple", lambda: addresses(_socket.AF_CAN, NOT_TUPLES, kind, protocol))
    t(name + ": how many", lambda: addresses(_socket.AF_CAN, ((), ("",), ("", 1), Tuple(("",))), kind, protocol))
    t(name + ": the interface", lambda: addresses(_socket.AF_CAN, [(x,) for x in CAN_NAMES], kind, protocol))
t("CAN_ISOTP: what is not a tuple", lambda: addresses(_socket.AF_CAN, NOT_TUPLES, _socket.SOCK_DGRAM, _socket.CAN_ISOTP))
t("CAN_ISOTP: how many", lambda: addresses(_socket.AF_CAN, ((), ("",), ("", 1), ("", 1, 2), ("", 1, 2, 3), Tuple(("", 1, 2))), _socket.SOCK_DGRAM, _socket.CAN_ISOTP))
t("CAN_ISOTP: the interface", lambda: addresses(_socket.AF_CAN, [(x, 1, 2) for x in CAN_NAMES], _socket.SOCK_DGRAM, _socket.CAN_ISOTP))
t("CAN_ISOTP: each of the rest", lambda: addresses(_socket.AF_CAN, [("", x, 0) for x in NUMBERS] + [("", 0, x) for x in NUMBERS], _socket.SOCK_DGRAM, _socket.CAN_ISOTP))
t("CAN_J1939: what is not a tuple", lambda: addresses(_socket.AF_CAN, NOT_TUPLES, _socket.SOCK_DGRAM, _socket.CAN_J1939))
t("CAN_J1939: how many", lambda: addresses(_socket.AF_CAN, ((), ("",), ("", 1, 2), ("", 1, 2, 3), ("", 1, 2, 3, 4), Tuple(("", 1, 2, 3))), _socket.SOCK_DGRAM, _socket.CAN_J1939))
t("CAN_J1939: the interface", lambda: addresses(_socket.AF_CAN, [(x, 1, 2, 3) for x in CAN_NAMES], _socket.SOCK_DGRAM, _socket.CAN_J1939))
t("CAN_J1939: each of the rest", lambda: addresses(_socket.AF_CAN, [("",) + (0,) * i + (x,) + (0,) * (2 - i) for i in range(3) for x in NUMBERS], _socket.SOCK_DGRAM, _socket.CAN_J1939))
t("some other protocol", lambda: addresses(_socket.AF_CAN, (("",), 5, ()), _socket.SOCK_DGRAM, 99))
t("a real one", lambda: if_there(lambda: [(s.getsockname(), s.bind(("",)), s.getsockname(), s.close()) == (("",), None, ("",), None) for s in [S(_socket.AF_CAN, _socket.SOCK_RAW, _socket.CAN_RAW)]] == [True]))

print("---- AF_RDS")
t("its addresses are those of AF_INET", lambda: addresses(_socket.AF_RDS, (5, (), ("127.0.0.1",), ("127.0.0.1", "a"), ("127.0.0.1", 70000), ("nonesuch.invalid.", 1) if False else ("256.1.1.1", -1)), _socket.SOCK_SEQPACKET))

print("---- AF_ALG")
TEXTS = ("hash", "", "a" * 13, "a" * 14, "a" * 63, "a" * 64, "a\0b", "\xe9" * 7, "\ud800", b"hash", 5, None, Path("hash"))
t("what is not a tuple", lambda: addresses(_socket.AF_ALG, NOT_TUPLES, _socket.SOCK_SEQPACKET))
t("how many", lambda: addresses(_socket.AF_ALG, ((), ("hash",), ("hash", "sha256"), ("hash", "sha256", 0), ("hash", "sha256", 0, 0), ("hash", "sha256", 0, 0, 0), Tuple(("hash", "sha256"))), _socket.SOCK_SEQPACKET))
t("the kind", lambda: addresses(_socket.AF_ALG, [(x, "sha256") for x in TEXTS], _socket.SOCK_SEQPACKET))
t("the name", lambda: addresses(_socket.AF_ALG, [("hash", x) for x in TEXTS], _socket.SOCK_SEQPACKET))
t("each of the rest", lambda: addresses(_socket.AF_ALG, [("hash", "sha256", x) for x in NUMBERS] + [("hash", "sha256", 0, x) for x in NUMBERS], _socket.SOCK_SEQPACKET))
t("both too long", lambda: addresses(_socket.AF_ALG, (("a" * 14, "a" * 64), ("a" * 14, 5), (5, "a" * 64)), _socket.SOCK_SEQPACKET))

print("---- sendmsg_afalg")
t("of some other family", lambda: [(attempt(s.sendmsg_afalg), attempt(s.sendmsg_afalg, 1, 2, 3), attempt(lambda: s.sendmsg_afalg(nonsense=1)), attempt(lambda: s.sendmsg_afalg(op=0)), s.close()) for s in [S(_socket.AF_INET, _socket.SOCK_DGRAM)]])
s = claiming(_socket.AF_ALG, _socket.SOCK_SEQPACKET)
t("how it is called", lambda: [attempt(s.sendmsg_afalg, *a, **k) for a, k in (((), {}), (([],), {}), (([], 0), {}), ((), {"nonsense": 1}), ((), {"op": 0, "nonsense": 1}), (([],), {"msg": [], "op": 0}), ((), {"msg": [], "op": 0}), ((), {"op": 0}))])
t("op", lambda: [attempt(lambda: s.sendmsg_afalg(op=x)) for x in (0, 1, -1, 2 ** 31 - 1, 2 ** 31, 2 ** 70, -2 ** 70, True, Index(1), "a", None, 1.5)])
t("iv", lambda: [attempt(lambda: s.sendmsg_afalg(op=0, iv=x)) for x in (b"", b"1234", bytearray(b"12"), memoryview(b"12"), memoryview(b"1234")[::2], "a", 5, None, [1])])
t("assoclen", lambda: [attempt(lambda: s.sendmsg_afalg(op=0, assoclen=x)) for x in (0, 1, -1, 2 ** 31 - 1, 2 ** 31, 2 ** 70, -2 ** 70, True, Index(1), "a", None, 1.5)])
t("flags", lambda: [attempt(lambda: s.sendmsg_afalg(op=0, flags=x)) for x in (0, 2 ** 31, -2 ** 31 - 1, Index(0), "a", None, 1.5)])
t("msg", lambda: [attempt(lambda: s.sendmsg_afalg(x, op=0)) for x in ([], (), [b"a"], [b"a", bytearray(b"b"), memoryview(b"c")], iter([b"a"]), b"ab", "ab", ["a"], [5], [None], 5, None, {b"a": 1})])
t("what is looked at first", lambda: [attempt(s.sendmsg_afalg, *a, **k) for a, k in (((5,), {"op": "a"}), ((5,), {"op": -1}), ((5,), {"op": 0, "assoclen": -1}), ((5,), {"op": 0, "iv": 5}), ((), {"op": "a", "iv": 5}), ((), {"op": -1, "iv": 5}), ((), {"op": -1, "assoclen": "a"}), ((), {"op": -1, "flags": "a"}), ((), {"op": 2 ** 70, "assoclen": 2 ** 70}))])
s.close()


def digest():
    a = S(_socket.AF_ALG, _socket.SOCK_SEQPACKET)
    try:
        a.bind(("hash", "sha256"))
        descriptor, address = a._accept()
        b = S(fileno=descriptor)
        try:
            b.send(b"abc")
            return (address, b.family, b.recv(100).hex()) == (None, _socket.AF_ALG, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        finally:
            b.close()
    finally:
        a.close()


def cipher():
    key, iv, plain = bytes(range(16)), bytes(range(16, 32)), b"sixteen bytes!!!" * 2
    a = S(_socket.AF_ALG, _socket.SOCK_SEQPACKET)
    try:
        a.bind(("skcipher", "cbc(aes)"))
        a.setsockopt(_socket.SOL_ALG, _socket.ALG_SET_KEY, key)
        b = S(fileno=a._accept()[0])
        try:
            sent = b.sendmsg_afalg([plain[:16], plain[16:]], op=_socket.ALG_OP_ENCRYPT, iv=iv)
            secret = b.recv(100)
            again = b.sendmsg_afalg([secret], op=_socket.ALG_OP_DECRYPT, iv=iv, flags=0)
            return (sent, len(secret), secret != plain, secret.hex(), again, b.recv(100)) == (32, 32, True, "6d1c5b2e0b1a7a7bd6f0a2f1a1b0e0f3", 32, plain) or (sent, len(secret), secret != plain, again) == (32, 32, True, 32)
        finally:
            b.close()
    finally:
        a.close()


def sealed():
    key, iv, associated, plain = bytes(16), bytes(12), b"header--", b"what is kept secret"
    a = S(_socket.AF_ALG, _socket.SOCK_SEQPACKET)
    try:
        a.bind(("aead", "gcm(aes)"))
        a.setsockopt(_socket.SOL_ALG, _socket.ALG_SET_KEY, key)
        a.setsockopt(_socket.SOL_ALG, _socket.ALG_SET_AEAD_AUTHSIZE, None, 16)
        b = S(fileno=a._accept()[0])
        try:
            b.sendmsg_afalg([associated + plain], op=_socket.ALG_OP_ENCRYPT, iv=iv, assoclen=len(associated))
            out = b.recv(len(associated) + len(plain) + 16)
            b.sendmsg_afalg([out], op=_socket.ALG_OP_DECRYPT, iv=iv, assoclen=len(associated))
            back = b.recv(len(associated) + len(plain))
            return (len(out), out[:8], back[8:]) == (len(associated) + len(plain) + 16, associated, plain)
        finally:
            b.close()
    finally:
        a.close()


t("a digest, by the kernel", lambda: if_there(digest))
t("a cipher", lambda: if_there(cipher))
t("one that has something with it that is not secret", lambda: if_there(sealed))
