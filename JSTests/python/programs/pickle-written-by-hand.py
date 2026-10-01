# _pickle: Unpickler, and pickles written by hand.
import _pickle, pickle, io, copyreg, collections, sys, types, struct
import re; address = re.compile('0x[0-9a-f]+')
def attempt(label, f):
    try:
        r = f()
    except BaseException as e:
        r = "%s: %s" % (type(e).__name__, e)
        c = e.__context__ or e.__cause__
        if c is not None: r += " <- %s: %s" % (type(c).__name__, c)
    print(label, "->", address.sub("0x", ascii(r)))
U = _pickle.Unpickler
loads = _pickle.loads
class C:
    def __repr__(self): return "C(%r)" % (sorted(vars(self).items()),)
class WithArgs:
    def __init__(self, *a): self.a = a
    def __repr__(self): return "WithArgs%r" % (getattr(self, "a", "no __init__"),)
class InitArgs:
    def __init__(self, *a): self.a = a
    def __getinitargs__(self): return ()
    def __repr__(self): return "InitArgs%r" % (getattr(self, "a", "no __init__"),)
class Slotted:
    __slots__ = ("x",)
    def __repr__(self): return "Slotted(%r)" % (getattr(self, "x", None),)
class SetState:
    def __setstate__(self, s): self.s = s
    def __repr__(self): return "SetState(%r)" % (getattr(self, "s", "no state"),)
class Appender:
    def __init__(self): self.l = []
    def append(self, v): self.l.append(v)
    def __repr__(self): return "Appender(%r)" % (self.l,)
class Extender(Appender):
    def extend(self, v): self.l.append(("extend", v))
class Adder:
    def __init__(self): self.l = []
    def add(self, v): self.l.append(v)
    def __repr__(self): return "Adder(%r)" % (self.l,)
class Setter:
    def __init__(self): self.l = []
    def __setitem__(self, k, v): self.l.append((k, v))
    def __repr__(self): return "Setter(%r)" % (self.l,)
cases = {
    "nothing": b"", "stop": b".", "none": b"N.", "none, and more": b"N.N.", "no stop": b"N", "two left": b"NN.", "unknown": b"\x00", "unknown letter": b"Z", "unknown quote": b"'", "unknown backslash": b"\\", "unknown high": b"\xff", "unknown space": b" ", "unknown tilde": b"~", "unknown del": b"\x7f",
    "INT": b"I5\n.", "INT negative": b"I-5\n.", "INT 00": b"I00\n.", "INT 01": b"I01\n.", "INT 02": b"I02\n.", "INT 0": b"I0\n.", "INT 1": b"I1\n.", "INT +1": b"I+1\n.", "INT -0": b"I-0\n.", "INT space": b"I 1\n.", "INT large": b"I123456789012345678901234567890\n.", "INT empty": b"I\n.", "INT junk": b"Ix\n.", "INT trailing": b"I5x\n.",
    "INT underscore": b"I1_0\n.", "INT float": b"I1.5\n.", "INT no newline": b"I5", "INT hex": b"I0x10\n.", "INT trailing space": b"I5 \n.", "INT L": b"I5L\n.", "INT zero byte": b"I5\x00x\n.", "INT 2**31": b"I2147483648\n.", "INT -2**31": b"I-2147483648\n.", "INT 2**63": b"I9223372036854775808\n.",
    "LONG": b"L5L\n.", "LONG no L": b"L5\n.", "LONG negative": b"L-5L\n.", "LONG large": b"L123456789012345678901234567890L\n.", "LONG empty": b"L\n.", "LONG only L": b"LL\n.", "LONG junk": b"LxL\n.", "LONG junk no L": b"L5x\n.", "LONG two Ls": b"L5LL\n.", "LONG space": b"L 5 L\n.", "LONG underscore": b"L1_0L\n.",
    "BININT": b"J\x01\x02\x03\x04.", "BININT negative": b"J\xff\xff\xff\xff.", "BININT short": b"J\x01\x02.", "BININT1": b"K\xff.", "BININT1 short": b"K", "BININT2": b"M\xff\xff.", "LONG1": b"\x8a\x01\x05.", "LONG1 none": b"\x8a\x00.", "LONG1 negative": b"\x8a\x01\xff.", "LONG1 two": b"\x8a\x02\xff\x00.", "LONG1 -256": b"\x8a\x02\x00\xff.",
    "LONG1 nine": b"\x8a\x09" + b"\xff" * 8 + b"\x00.", "LONG1 nine negative": b"\x8a\x09" + b"\x00" * 8 + b"\xff.", "LONG1 redundant": b"\x8a\x03\x05\x00\x00.", "LONG1 redundant negative": b"\x8a\x03\xff\xff\xff.", "LONG1 short": b"\x8a\x05\x01.", "LONG4": b"\x8b\x02\x00\x00\x00\x01\x02.", "LONG4 none": b"\x8b\x00\x00\x00\x00.",
    "LONG4 negative count": b"\x8b\xff\xff\xff\xff.", "LONG4 short": b"\x8b\x05\x00\x00\x00\x01.", "LONG1 -2**63": b"\x8a\x08" + b"\x00" * 7 + b"\x80.", "LONG1 2**31": b"\x8a\x05\x00\x00\x00\x80\x00.", "LONG1 -2**31": b"\x8a\x04\x00\x00\x00\x80.",
    "FLOAT": b"F1.5\n.", "FLOAT int": b"F5\n.", "FLOAT exp": b"F1e5\n.", "FLOAT inf": b"Finf\n.", "FLOAT -inf": b"F-inf\n.", "FLOAT nan": b"Fnan\n.", "FLOAT Infinity": b"FInfinity\n.", "FLOAT infx": b"Finfx\n.", "FLOAT infinit": b"Finfinit\n.", "FLOAT empty": b"F\n.", "FLOAT junk": b"Fx\n.", "FLOAT trailing": b"F1.5x\n.", "FLOAT space": b"F 1.5\n.",
    "FLOAT trailing space": b"F1.5 \n.", "FLOAT underscore": b"F1_0.5\n.", "FLOAT too large": b"F1e999\n.", "FLOAT too small": b"F1e-999\n.", "FLOAT dot": b"F.\n.", "FLOAT .5": b"F.5\n.", "FLOAT 5.": b"F5.\n.", "FLOAT e": b"F1e\n.", "FLOAT +": b"F+1.5\n.", "FLOAT hex": b"F0x10\n.", "FLOAT zero byte": b"F1.5\x00x\n.", "FLOAT -0": b"F-0.0\n.", "FLOAT sign only": b"F-\n.",
    "BINFLOAT": b"G?\xf8\x00\x00\x00\x00\x00\x00.", "BINFLOAT nan": b"G\x7f\xf8\x00\x00\x00\x00\x00\x01.", "BINFLOAT short": b"G?\xf8.", "BINFLOAT whole": b"G@\x00\x00\x00\x00\x00\x00\x00.",
    "STRING": b"S'abc'\n.", "STRING double": b'S"abc"\n.', "STRING escapes": b"S'a\\n\\x41\\''\n.", "STRING unquoted": b"Sabc\n.", "STRING one quote": b"S'\n.", "STRING mismatched": b"S'abc\"\n.", "STRING empty": b"S''\n.", "STRING nothing": b"S\n.", "STRING high": b"S'\\xe9'\n.", "STRING raw high": b"S'\xe9'\n.", "STRING bad escape": b"S'\\x4'\n.", "STRING trailing backslash": b"S'abc\\'\n.",
    "STRING space after": b"S'abc' \n.", "STRING no newline": b"S'abc'", "BINSTRING": b"T\x03\x00\x00\x00abc.", "BINSTRING negative": b"T\xff\xff\xff\xffabc.", "BINSTRING short": b"T\x05\x00\x00\x00abc.", "BINSTRING high": b"T\x01\x00\x00\x00\xe9.", "SHORT_BINSTRING": b"U\x03abc.", "SHORT_BINSTRING none": b"U\x00.", "SHORT_BINSTRING short": b"U\x05abc.",
    "UNICODE": b"Vabc\n.", "UNICODE escapes": b"Va\\u20ac\\U0001f600\\n\n.", "UNICODE latin": b"V\xe9\n.", "UNICODE empty": b"V\n.", "UNICODE bad": b"V\\u20\n.", "UNICODE bad U": b"V\\U00110000\n.", "UNICODE no newline": b"Vabc", "UNICODE surrogate": b"V\\ud800\n.",
    "BINUNICODE": b"X\x03\x00\x00\x00abc.", "BINUNICODE utf8": b"X\x03\x00\x00\x00\xe2\x82\xac.", "BINUNICODE bad": b"X\x01\x00\x00\x00\xe9.", "BINUNICODE surrogate": b"X\x03\x00\x00\x00\xed\xa0\x80.", "BINUNICODE astral": b"X\x04\x00\x00\x00\xf0\x9f\x98\x80.", "BINUNICODE short": b"X\x05\x00\x00\x00abc.", "BINUNICODE none": b"X\x00\x00\x00\x00.", "BINUNICODE huge": b"X\xff\xff\xff\xffabc.",
    "SHORT_BINUNICODE": b"\x8c\x03abc.", "SHORT_BINUNICODE short": b"\x8c\x05abc.", "BINUNICODE8": b"\x8d\x03\x00\x00\x00\x00\x00\x00\x00abc.", "BINUNICODE8 huge": b"\x8d\xff\xff\xff\xff\xff\xff\xff\x7fabc.", "BINUNICODE8 negative": b"\x8d\xff\xff\xff\xff\xff\xff\xff\xffabc.",
    "BINBYTES": b"B\x03\x00\x00\x00abc.", "BINBYTES short": b"B\x05\x00\x00\x00abc.", "BINBYTES none": b"B\x00\x00\x00\x00.", "SHORT_BINBYTES": b"C\x03abc.", "SHORT_BINBYTES short": b"C\x05abc.", "BINBYTES8": b"\x8e\x03\x00\x00\x00\x00\x00\x00\x00abc.", "BINBYTES8 negative": b"\x8e\xff\xff\xff\xff\xff\xff\xff\xffabc.", "BINBYTES8 short": b"\x8e\x05\x00\x00\x00\x00\x00\x00\x00abc.",
    "BYTEARRAY8": b"\x96\x03\x00\x00\x00\x00\x00\x00\x00abc.", "BYTEARRAY8 none": b"\x96\x00\x00\x00\x00\x00\x00\x00\x00.", "BYTEARRAY8 negative": b"\x96\xff\xff\xff\xff\xff\xff\xff\xffabc.", "BYTEARRAY8 short": b"\x96\x05\x00\x00\x00\x00\x00\x00\x00abc.",
    "NEWTRUE": b"\x88.", "NEWFALSE": b"\x89.", "EMPTY_TUPLE": b").", "TUPLE1": b"N\x85.", "TUPLE1 nothing": b"\x85.", "TUPLE2": b"NN\x86.", "TUPLE2 one": b"N\x86.", "TUPLE3": b"NNN\x87.", "TUPLE3 two": b"NN\x87.", "TUPLE": b"(NNNNt.", "TUPLE none": b"(t.", "TUPLE no mark": b"NNt.", "TUPLE1 at a mark": b"(\x85.", "TUPLE2 across a mark": b"N(N\x86.",
    "EMPTY_LIST": b"].", "LIST": b"(NNl.", "LIST no mark": b"Nl.", "EMPTY_DICT": b"}.", "DICT": b"(K\x01K\x02K\x03K\x04d.", "DICT odd": b"(K\x01K\x02K\x03d.", "DICT none": b"(d.", "DICT no mark": b"d.", "DICT unhashable": b"(]K\x01d.", "EMPTY_SET": b"\x8f.", "FROZENSET": b"(K\x01K\x02K\x01\x91.", "FROZENSET none": b"(\x91.", "FROZENSET no mark": b"\x91.", "FROZENSET unhashable": b"(]\x91.",
    "APPEND": b"]K\x01a.", "APPEND nothing": b"]a.", "APPEND to nothing": b"K\x01a.", "APPEND to an int": b"K\x01K\x02a.", "APPEND at a mark": b"](a.", "APPEND across a mark": b"](K\x01a.", "APPENDS": b"](K\x01K\x02e.", "APPENDS none": b"](e.", "APPENDS no mark": b"]K\x01e.", "APPENDS to nothing": b"(K\x01e.", "APPENDS to an int": b"K\x00(K\x01e.", "APPENDS to a tuple": b")(K\x01e.",
    "SETITEM": b"}K\x01K\x02s.", "SETITEM one": b"}K\x01s.", "SETITEM nothing": b"}s.", "SETITEM to nothing": b"K\x01K\x02s.", "SETITEM to an int": b"K\x00K\x01K\x02s.", "SETITEM unhashable": b"}]K\x02s.", "SETITEM on a list": b"]K\x00K\x02s.", "SETITEMS": b"}(K\x01K\x02K\x03K\x04u.", "SETITEMS odd": b"}(K\x01u.", "SETITEMS none": b"}(u.", "SETITEMS no mark": b"}K\x01K\x02u.", "SETITEMS to nothing": b"(K\x01K\x02u.",
    "ADDITEMS": b"\x8f(K\x01K\x02\x90.", "ADDITEMS none": b"\x8f(\x90.", "ADDITEMS no mark": b"\x8fK\x01\x90.", "ADDITEMS to nothing": b"(K\x01\x90.", "ADDITEMS to a list": b"](K\x01\x90.", "ADDITEMS unhashable": b"\x8f(]\x90.", "ADDITEMS to a frozenset": b"(\x91(K\x01\x90.",
    "POP": b"NK\x010.", "POP nothing": b"0.", "POP a mark": b"N(0.", "POP below a mark": b"N(00.", "POP_MARK": b"N(K\x01K\x021.", "POP_MARK no mark": b"N1.", "DUP": b"N2\x86.", "DUP nothing": b"2.", "DUP at a mark": b"N(2.", "MARK left": b"N(.", "MARK and stop": b"(.", "marks": b"((((K\x01tttt.", "stop at a mark": b"(N.",
    "PUT GET": b"]p0\ng0\n\x86.", "GET nothing": b"g0\n.", "GET junk": b"gx\n.", "GET empty": b"g\n.", "GET negative": b"g-1\n.", "GET huge": b"g99999999999999999999\n.", "GET large": b"g1000000\n.", "PUT nothing": b"p0\n.", "PUT negative": b"Np-1\n.", "PUT junk": b"Npx\n.", "PUT huge": b"Np99999999999999999999\n.", "PUT at a mark": b"N(p0\n.", "PUT 100": b"]p100\ng100\n\x86.", "PUT space": b"]p 1 \ng1\n\x86.",
    "BINPUT BINGET": b"]q\x05h\x05\x86.", "BINGET nothing": b"h\x05.", "BINPUT nothing": b"q\x05.", "BINPUT 255": b"]q\xffh\xff\x86.", "LONG_BINPUT": b"]r\x00\x01\x00\x00j\x00\x01\x00\x00\x86.", "LONG_BINGET nothing": b"j\x00\x01\x00\x00.", "LONG_BINGET huge": b"j\xff\xff\xff\xff.", "MEMOIZE": b"]\x94h\x00\x86.", "MEMOIZE nothing": b"\x94.", "MEMOIZE twice": b"]\x94N\x94h\x00h\x01\x87.",
    "MEMOIZE after BINPUT": b"]q\x05N\x94h\x01.", "put twice": b"]q\x00Nq\x00h\x00.", "MEMOIZE at a mark": b"N(\x94.", "memoized None": b"N\x94h\x00\x86.",
    "GLOBAL": b"c__main__\nC\n.", "GLOBAL builtin": b"cbuiltins\nlen\n.", "GLOBAL no module": b"cnowhere_at_all\nC\n.", "GLOBAL no name": b"c__main__\nnowhere\n.", "GLOBAL empty module": b"c\nC\n.", "GLOBAL empty name": b"c__main__\n\n.", "GLOBAL one line": b"c__main__\n", "GLOBAL short": b"c__main__", "GLOBAL dotted, old": b"c__main__\nC.__repr__\n.", "GLOBAL dotted, 4": b"\x80\x04c__main__\nC.__repr__\n.",
    "GLOBAL not utf-8": b"c\xe9\nC\n.", "GLOBAL python 2": b"c__builtin__\nxrange\n.", "GLOBAL python 2 module": b"ccPickle\ndumps\n.", "GLOBAL python 2, protocol 3": b"\x80\x03c__builtin__\nxrange\n.", "GLOBAL exceptions": b"cexceptions\nStandardError\n.",
    "STACK_GLOBAL": b"\x8c\x08__main__\x8c\x01C\x93.", "STACK_GLOBAL dotted": b"\x80\x04\x8c\x08__main__\x8c\x0aC.__repr__\x93.", "STACK_GLOBAL dotted, no proto": b"\x8c\x08__main__\x8c\x0aC.__repr__\x93.", "STACK_GLOBAL one": b"\x8c\x01C\x93.", "STACK_GLOBAL none": b"\x93.", "STACK_GLOBAL ints": b"K\x01K\x02\x93.", "STACK_GLOBAL bytes": b"C\x08__main__C\x01C\x93.",
    "STACK_GLOBAL bad path": b"\x80\x04\x8c\x08__main__\x8c\x0bC.nowhere.x\x93.", "STACK_GLOBAL locals": b"\x80\x04\x8c\x08__main__\x8c\x0aC.<locals>\x93.", "STACK_GLOBAL empty": b"\x80\x04\x8c\x08__main__\x8c\x00\x93.", "STACK_GLOBAL dots": b"\x80\x04\x8c\x08__main__\x8c\x02..\x93.",
    "REDUCE": b"cbuiltins\nlen\n(]tR.", "REDUCE no tuple": b"cbuiltins\nlen\n]R.", "REDUCE not callable": b"K\x01)R.", "REDUCE one": b")R.", "REDUCE none": b"R.", "REDUCE raises": b"cbuiltins\nlen\n)R.", "REDUCE None args": b"cbuiltins\nlen\nNR.",
    "NEWOBJ": b"c__main__\nC\n)\x81.", "NEWOBJ args": b"cbuiltins\nint\n(K\x05t\x81.", "NEWOBJ no class": b"K\x01)\x81.", "NEWOBJ no tuple": b"c__main__\nC\n]\x81.", "NEWOBJ one": b")\x81.", "NEWOBJ none": b"\x81.", "NEWOBJ a function": b"cbuiltins\nlen\n)\x81.", "NEWOBJ wrong args": b"c__main__\nC\n(K\x01t\x81.",
    "NEWOBJ_EX": b"c__main__\nC\n)}\x92.", "NEWOBJ_EX keywords": b"cbuiltins\nint\n(V10\nt}Vbase\nK\x02s\x92.", "NEWOBJ_EX no dict": b"c__main__\nC\n)]\x92.", "NEWOBJ_EX no tuple": b"c__main__\nC\n]}\x92.", "NEWOBJ_EX no class": b"K\x01)}\x92.", "NEWOBJ_EX two": b")}\x92.",
    "OBJ": b"(c__main__\nC\no.", "OBJ args": b"(c__main__\nWithArgs\nK\x01K\x02o.", "OBJ no args, __init__ not called": b"(c__main__\nWithArgs\no.", "OBJ initargs": b"(c__main__\nInitArgs\no.", "OBJ nothing": b"(o.", "OBJ no mark": b"c__main__\nC\no.", "OBJ not a class": b"(K\x01o.", "OBJ a function": b"(cbuiltins\nlen\n]o.", "OBJ a function, no args": b"(cbuiltins\nlen\no.",
    "INST": b"(i__main__\nC\n.", "INST args": b"(K\x01K\x02i__main__\nWithArgs\n.", "INST no mark": b"i__main__\nC\n.", "INST short": b"(i__main__\n", "INST not ascii": b"(i\xc3\xa9\nC\n.", "INST empty": b"(i\n\n.", "INST no such": b"(i__main__\nnowhere\n.",
    "BUILD dict": b"c__main__\nC\n)\x81}Va\nK\x01sb.", "BUILD None": b"c__main__\nC\n)\x81Nb.", "BUILD int": b"c__main__\nC\n)\x81K\x01b.", "BUILD setstate": b"c__main__\nSetState\n)\x81K\x05b.", "BUILD slots": b"c__main__\nSlotted\n)\x81N}Vx\nK\x01s\x86b.", "BUILD both": b"c__main__\nC\n)\x81}Va\nK\x01s}Vb\nK\x02s\x86b.", "BUILD slots no dict": b"c__main__\nC\n)\x81NK\x01\x86b.",
    "BUILD state in a pair no dict": b"c__main__\nC\n)\x81K\x01N\x86b.", "BUILD one": b"Nb.", "BUILD none": b"b.", "BUILD no __dict__": b"K\x01}Va\nK\x01sb.", "BUILD int keys": b"c__main__\nC\n)\x81}K\x01K\x02sb.", "BUILD slot int keys": b"c__main__\nC\n)\x81N}K\x01K\x02s\x86b.", "BUILD no such slot": b"c__main__\nSlotted\n)\x81N}Vy\nK\x01s\x86b.", "BUILD a triple": b"c__main__\nC\n)\x81NNN\x87b.", "BUILD at a mark": b"N(Nb.",
    "APPENDS append": b"c__main__\nAppender\n)R(K\x01K\x02e.", "APPENDS extend": b"c__main__\nExtender\n)R(K\x01K\x02e.", "APPEND append": b"c__main__\nAppender\n)RK\x01a.", "ADDITEMS add": b"c__main__\nAdder\n)R(K\x01K\x02\x90.", "SETITEMS setitem": b"c__main__\nSetter\n)R(K\x01K\x02K\x03K\x04u.",
    "PROTO": b"\x80\x02N.", "PROTO 0": b"\x80\x00N.", "PROTO 5": b"\x80\x05N.", "PROTO 6": b"\x80\x06N.", "PROTO 255": b"\x80\xffN.", "PROTO short": b"\x80", "PROTO twice": b"\x80\x02\x80\x04N.", "PROTO later": b"N\x80\x02.",
    "FRAME": b"\x95\x02\x00\x00\x00\x00\x00\x00\x00N.", "FRAME short": b"\x95\x05\x00\x00\x00\x00\x00\x00\x00N.", "FRAME too short": b"\x95\x01\x00\x00\x00\x00\x00\x00\x00N.", "FRAME none": b"\x95\x00\x00\x00\x00\x00\x00\x00\x00N.", "FRAME negative": b"\x95\xff\xff\xff\xff\xff\xff\xff\xffN.", "FRAME huge": b"\x95\xff\xff\xff\xff\xff\xff\xff\x7fN.", "FRAME header short": b"\x95\x02\x00",
    "FRAME straddled": b"\x95\x02\x00\x00\x00\x00\x00\x00\x00K\x01.", "FRAME straddled by an argument": b"\x95\x03\x00\x00\x00\x00\x00\x00\x00J\x01\x02\x03\x04.", "FRAME straddled by a line": b"\x95\x02\x00\x00\x00\x00\x00\x00\x00I5\n.", "FRAME in a frame": b"\x95\x0c\x00\x00\x00\x00\x00\x00\x00\x95\x02\x00\x00\x00\x00\x00\x00\x00N.", "FRAME after a frame": b"\x95\x01\x00\x00\x00\x00\x00\x00\x00N\x95\x01\x00\x00\x00\x00\x00\x00\x00.",
    "FRAME header at the end of a frame": b"\x95\x0a\x00\x00\x00\x00\x00\x00\x00N\x95\x01\x00\x00\x00\x00\x00\x00\x00.", "FRAME bytes straddle": b"\x95\x04\x00\x00\x00\x00\x00\x00\x00C\x05abcde.", "FRAME bytes after": b"\x95\x02\x00\x00\x00\x00\x00\x00\x00C\x03abc.", "FRAME str straddles": b"\x95\x04\x00\x00\x00\x00\x00\x00\x00\x8c\x05abcde.",
    "PERSID": b"Pabc\n.", "BINPERSID": b"K\x01Q.", "BINPERSID nothing": b"Q.", "PERSID empty": b"P\n.", "PERSID short": b"Pabc", "EXT1": b"\x82\x01.", "EXT1 0": b"\x82\x00.", "EXT2": b"\x83\x01\x00.", "EXT4": b"\x84\x01\x00\x00\x00.", "EXT4 negative": b"\x84\xff\xff\xff\xff.", "EXT1 short": b"\x82",
    "NEXT_BUFFER": b"\x97.", "READONLY_BUFFER nothing": b"\x98.", "READONLY_BUFFER of an int": b"K\x01\x98.", "READONLY_BUFFER of bytes": b"C\x01a\x98.", "READONLY_BUFFER of a bytearray": b"\x96\x01\x00\x00\x00\x00\x00\x00\x00a\x98.",
}
for label, data in cases.items():
    attempt(label, lambda: loads(data))
    attempt("   from a file", lambda: _pickle.load(io.BytesIO(data)))
