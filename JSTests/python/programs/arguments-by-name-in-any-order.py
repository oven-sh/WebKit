# What can be given by name can be given in any order. These are written in C++, and each took what came first for its first argument.
import binascii
import math
import re
import select
import struct
import types
import _opcode
import _sre


def t(label, f):
    try:
        print(label, "=>", f())
    except Exception as e:
        print(label, "=>", type(e).__name__, e)


t("binascii.b2a_hex", lambda: (binascii.b2a_hex(data=b"abcd", sep=":"), binascii.b2a_hex(sep=":", data=b"abcd"), binascii.b2a_hex(bytes_per_sep=2, sep="-", data=b"abcdef")))
t("binascii.hexlify", lambda: (binascii.hexlify(sep=":", data=b"abcd"), binascii.hexlify(bytes_per_sep=-2, data=b"abcdef", sep=b".")))
t("binascii.a2b_qp", lambda: (binascii.a2b_qp(header=True, data=b"a_b=41"), binascii.a2b_qp(header=False, data="a_b=41"), binascii.a2b_qp(data=b"=3D")))
t("binascii.b2a_qp", lambda: (binascii.b2a_qp(header=True, data=b"a b\t="), binascii.b2a_qp(quotetabs=True, data=b"a b\t="), binascii.b2a_qp(istext=False, header=False, quotetabs=False, data=b"a\r\nb")))
t("math.isclose", lambda: (math.isclose(b=1.0, a=1.0 + 1e-10), math.isclose(b=0.0, a=100.0), math.isclose(abs_tol=200, b=0.0, a=100.0), math.isclose(rel_tol=0.5, b=10, a=6), math.isclose(b=float("inf"), a=1)))
t("math.isclose with one missing", lambda: math.isclose(b=1.0))
t("math.isclose of the wrong kind", lambda: math.isclose(b=1.0, a="s"))
t("memoryview._from_flags", lambda: (bytes(memoryview._from_flags(flags=284, object=b"abc")), memoryview._from_flags(flags=284, object=bytearray(b"ab")).readonly))
t("memoryview._from_flags of the wrong kind", lambda: memoryview._from_flags(flags=284, object=5))
t("_opcode.has_arg", lambda: [f(opcode=n) for f in (_opcode.has_arg, _opcode.has_const, _opcode.has_name, _opcode.has_jump, _opcode.has_free, _opcode.has_local, _opcode.has_exc) for n in (1, 83, 260)])
t("_opcode.stack_effect", lambda: (_opcode.stack_effect(1, jump=True), _opcode.stack_effect(1, None, jump=None)))
t("_opcode.get_executor", lambda: _opcode.get_executor(offset=0, code=5))
t("_opcode.get_executor of the wrong kind", lambda: _opcode.get_executor(offset="s", code=t.__code__))
if hasattr(select, "kevent"):
    t("select.kevent", lambda: [(k.ident, k.filter, k.flags, k.fflags, k.data, k.udata) for k in (select.kevent(filter=-2, flags=5, ident=7), select.kevent(udata=9, data=8, fflags=3, flags=5, filter=-1, ident=1), select.kevent(ident=7))])


def compiled_again(pattern):
    "What re makes of it, handed to _sre by name, the last first"
    from re import _compiler, _parser
    p = _parser.parse(pattern, 0)
    code = _compiler._code(p, 0)
    groupindex = p.state.groupdict
    indexgroup = [None] * p.state.groups
    for k, i in groupindex.items():
        indexgroup[i] = k
    made = _sre.compile(indexgroup=tuple(indexgroup), groupindex=groupindex, groups=p.state.groups - 1, code=code, flags=p.state.flags, pattern=pattern)
    m = made.match("ab12")
    return made.pattern, made.groups, dict(made.groupindex), m.groups(), m.groupdict()


t("_sre.compile", lambda: compiled_again(r"(?P<letters>[a-z]+)(\d+)"))
t("_sre.compile of the wrong kind", lambda: _sre.compile(indexgroup=(), groupindex={}, groups=0, code=5, flags=0, pattern="a"))

pattern = re.compile(r"(?P<letters>[a-z]+)(\d+)")
t("Pattern.match and the like", lambda: [f(endpos=5, pos=1, string="xab12z").span() for f in (pattern.match, pattern.search)] + [pattern.fullmatch(endpos=5, string="xab12z", pos=1).groups()])
t("Pattern.findall", lambda: pattern.findall(endpos=7, pos=1, string="xab12cd3"))
t("Pattern.finditer", lambda: [m.span() for m in pattern.finditer(endpos=8, pos=1, string="xab12cd3")])
t("Pattern.scanner", lambda: pattern.scanner(pos=1, string="xab12").match().groups())
t("Pattern.split", lambda: pattern.split(maxsplit=1, string="-ab1-cd2-"))
t("Pattern.sub", lambda: (pattern.sub(count=1, string="ab1 cd2", repl="<\\2>"), pattern.sub(string="ab1 cd2", repl=lambda m: m[1].upper()), pattern.subn(string="ab1 cd2", count=0, repl="")))
t("Pattern.sub with one missing", lambda: pattern.sub(string="ab1"))
t("Match.expand", lambda: pattern.match("ab12").expand(template="\\2-\\g<letters>"))
t("Struct.unpack_from", lambda: (struct.Struct("<H").unpack_from(offset=1, buffer=b"\x00\x01\x02"), struct.Struct("<H").unpack_from(buffer=b"\x01\x02")))
t("struct.unpack_from", lambda: (struct.unpack_from("<H", offset=1, buffer=b"\x00\x01\x02"), struct.unpack_from("<H", buffer=b"\x01\x02")))
t("mappingproxy", lambda: dict(types.MappingProxyType(mapping={"a": 1})))
t("mappingproxy of the wrong kind", lambda: types.MappingProxyType(mapping=5))
