# The module _opcode, which says what there is to know about CPython's instructions, and opcode, which is written in Python over it.
import _opcode
import opcode


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


t("the module", lambda: (_opcode.__name__, _opcode.__package__, _opcode.__loader__.__name__, _opcode.__doc__, sorted(n for n in vars(_opcode) if not n.startswith("__")), _opcode.ENABLE_SPECIALIZATION, _opcode.ENABLE_SPECIALIZATION_FT))
QUESTIONS = ("is_valid", "has_arg", "has_const", "has_name", "has_jump", "has_free", "has_local", "has_exc")
for name in sorted(n for n in vars(_opcode) if not n.startswith("__") and callable(getattr(_opcode, n))):
    t(name, lambda: (type(getattr(_opcode, name)).__name__, getattr(_opcode, name).__text_signature__, getattr(_opcode, name).__doc__, getattr(_opcode, name).__module__))
for name in QUESTIONS:
    f = getattr(_opcode, name)
    t(name, lambda: [op for op in range(-10, 400) if f(op)])
    t(name + " of the wrong thing", lambda: [attempt(f, *a, **k) for a, k in (((), {}), (("a",), {}), ((1.5,), {}), ((None,), {}), ((2 ** 31,), {}), ((-2 ** 31 - 1,), {}), ((2 ** 31 - 1,), {}), ((True,), {}), ((1, 2), {}), ((), {"opcode": 1}), ((), {"other": 1}))])
for jump in (None, True, False):
    for oparg in (None, 0, 1, 2, 3, 5, 255, 256, 257, 1000, -1, -2):
        t("stack_effect(op, %r, jump=%r)" % (oparg, jump), lambda: [(lambda r: r if isinstance(r, int) else "x")(attempt(_opcode.stack_effect, op, oparg, jump=jump)) for op in range(-2, 300)])
for a, k in (((), {}), (("a",), {}), ((1, "a"), {}), ((1, 1.5), {}), ((1, 2 ** 31), {}), ((1, 2 ** 63), {}), ((1, 2 ** 32 + 1), {}), ((1, 1, True), {}), ((1,), {"jump": 1}), ((1,), {"jump": 0}), ((1,), {"jump": "x"}), ((1,), {"oparg": 1}), ((), {"opcode": 1}), ((1,), {"other": 1}), ((-1,), {}), ((10 ** 6,), {}), ((2 ** 31,), {}), ((True, True), {}), ((opcode.opmap["BUILD_LIST"], 2 ** 32 + 3), {})):
    t("stack_effect(*%r, **%r)" % (a, k), lambda: _opcode.stack_effect(*a, **k))
t("names", lambda: (_opcode.get_nb_ops(), _opcode.get_intrinsic1_descs(), _opcode.get_intrinsic2_descs(), _opcode.get_special_method_names(), _opcode.get_specialization_stats()))
t("made anew each time", lambda: (_opcode.get_nb_ops() is _opcode.get_nb_ops(), attempt(_opcode.get_nb_ops, 1), attempt(_opcode.get_intrinsic1_descs, 1), attempt(_opcode.get_specialization_stats, 1)))
t("get_executor", lambda: [attempt(_opcode.get_executor, *a, **k) for a, k in (((), {}), ((t.__code__,), {}), ((t.__code__, 0), {}), ((5, 0), {}), ((None, 0), {}), ((t, 0), {}), ((t.__code__, "a"), {}), ((5, "a"), {}), ((t.__code__, 2 ** 31), {}), ((), {"code": t.__code__, "offset": 0}), ((t.__code__, 0, 1), {}))])
t("opcode", lambda: (len(opcode.opmap), len(opcode.opname), opcode.hasarg, opcode.hasconst, opcode.hasname, opcode.hasjump, opcode.hasjrel, opcode.hasjabs, opcode.hasfree, opcode.haslocal, opcode.hasexc, opcode.hascompare, opcode.cmp_op, opcode.HAVE_ARGUMENT, opcode.EXTENDED_ARG, opcode.stack_effect is _opcode.stack_effect, sorted(opcode.__all__)))
t("what it keeps to itself", lambda: (opcode._nb_ops, opcode._intrinsic_1_descs, opcode._intrinsic_2_descs, opcode._special_method_names, opcode._common_constants, sorted(opcode._cache_format), opcode._inline_cache_entries))
