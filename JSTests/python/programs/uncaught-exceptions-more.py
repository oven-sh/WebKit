import sys
# What is printed says where this file is, which is not the same everywhere.
class WithoutDirectory:
    directory = __file__[:__file__.rfind("/") + 1]
    def write(self, text): sys.stdout.write(text.replace(self.directory, "") if self.directory else text)
    def flush(self): pass
sys.stderr = WithoutDirectory()
def show(f, *a):
    print("=====", f.__name__)
    try: f(*a)
    except BaseException:
        sys.__excepthook__(*sys.exc_info())
x = None; z = 0; d = {}; l = [1]
# ---- how wide things are
def wide_before(): return "日本語" + x
def wide_name():
    変数 = None
    return 変数.属性
def emoji(): return "😀😀" + x + "😀"
def combining(): return "é" + x
def wide_call(): return len("日本", "語")
def wide_subscript(): return {"日本": 1}["語"]
def astral_name():
    𝐱 = None
    return 1 + 𝐱
# ---- more than one line
def two_lines():
    return (x
            .missing)
def many_lines():
    return len(
        1,
        2,
        3,
        4,
        5,
        6,
    )
def binop_far_apart():
    return (1
            +
            2
            +
            3
            +
            x)
def subscript_lines():
    return d[
        "a"
        "b"
    ]
def call_of_multiline_function():
    return (lambda:
            None)(
        1)
def string_lines():
    return """a
b""" + x
def comment_between():
    return (1 + # a comment
            x)
def backslash():
    return 1 + \
        x
def indented_more():
    if True:
        if True:
            return (x +
        1)
def blank_line_inside():
    return (1 +

            x)
# ---- which operator
def two_character_operator(): return 1 // z
def shift(): return 1 << x
def power(): return 2 ** x
def matmul(): return 1 @ 2
def no_spaces(): return 1+x
def parenthesized_left(): return (1) + x
def nested_parentheses(): return ((1 + 2)) * x
def unary_in_binop(): return -1 + x
def compare_chain(): return 1 < 2 < x
def boolean(): return (1 and x.a)
def call_in_call(): return len(len(1))
def subscript_of_call(): return len([])[0]
def call_of_subscript(): return l[0]()
def attribute_of_subscript(): return l[0].nothing
def method_with_keywords(): return l.append(a=1)
def slice_of_none(): return x[1:2, 3]
def await_free(): return [i for i in x]
def dict_display(): return {**x}
def list_display(): return [*x]
def conditional(): return 1 if x.a else 2
def lambda_body(): return (lambda a: a + x)(1)
def return_call(): return int("a")
def assign_call():
    v = int("a")
def assign_call_attribute():
    v = str.nothing("a")
def return_method_call(): return "a".nothing()
def expression_statement(): int("a")
def whole_line_raise(): raise ValueError("whole")
def with_as():
    with x as y: pass
def for_target():
    for a, b in [1]: pass
def decorator():
    @x
    def f(): pass
def default_argument():
    def f(a=1 / z): pass
def class_body():
    class C:
        v = 1 / z
def class_base():
    class C(x): pass
def global_del(): del undefined_global
def star_assign(): a, *b = x
def augmented_subscript(): d["k"] += 1
def augmented_attribute(): x.a += 1
def format_spec(): return f"{1:{x.a}}"
def match_statement():
    match x.a:
        case 1: pass
def yield_expression():
    def g(): yield 1 / z
    return list(g())
def generator_expression(): return list(1 / i for i in [0])
# ---- names that may have been meant
def name_close():
    something = 1
    return somethin
def name_global(): return sho
def name_builtin(): return lenn
def name_far(): return qqqqqqqqqq
def name_self():
    class C:
        def __init__(self): self.value = 1
        def m(self): return value
    C().m()
def name_case():
    Value = 1
    return value
def attribute_close(): return l.appendd
def attribute_far(): return l.qqqqqqqq
def attribute_private():
    class C:
        _hidden = 1
    return C().hidden
def attribute_private_from_method():
    class C:
        _hidden = 1
        def m(self): return self.hidden
    return C().m()
def attribute_underscore():
    class C:
        _hidden = 1
    return C()._hiden
def attribute_of_class(): return list.appendd
def attribute_of_module(): return sys.versio
def attribute_of_none(): return x.appendd
def import_from_close(): from sys import versio
def import_from_far(): from sys import qqqqqqqqq
def unbound_local():
    print(later); later = 1
def name_deleted():
    v = 1; del v; return v
# ---- notes
def note_kinds():
    e = ValueError("v"); e.__notes__ = ["a", 1, None, "two\nlines", ""]; raise e
def notes_tuple():
    e = ValueError("v"); e.__notes__ = ("t",); raise e
def notes_string():
    e = ValueError("v"); e.__notes__ = "string"; raise e
def notes_number():
    e = ValueError("v"); e.__notes__ = 5; raise e
def notes_none():
    e = ValueError("v"); e.__notes__ = None; raise e
def notes_empty():
    e = ValueError("v"); e.__notes__ = []; raise e
def note_bad_str():
    class B:
        def __str__(s): raise RuntimeError
    e = ValueError("v"); e.__notes__ = [B()]; raise e
def notes_raise():
    class E(ValueError):
        @property
        def __notes__(s): raise RuntimeError("no notes")
    raise E("v")
# ---- what it is called, and what it says
def module_other():
    class E(Exception): pass
    E.__module__ = "some.where"; raise E("m")
def module_none():
    class E(Exception): pass
    E.__module__ = None; raise E("m")
def module_builtins():
    class E(Exception): pass
    E.__module__ = "builtins"; raise E("m")
def qualname_set():
    class E(Exception): pass
    E.__qualname__ = "Other.Name"; raise E("m")
def message_lines(): raise ValueError("first\nsecond")
def message_empty(): raise ValueError("")
def message_none(): raise ValueError(None)
def key_error(): raise KeyError("k")
def os_error(): raise OSError(2, "No such file", "name")
def stop_iteration(): raise StopIteration(5)
# ---- what led to it
def cycle_context():
    a = ValueError("a"); b = ValueError("b"); a.__context__ = b; b.__context__ = a; raise a
def cycle_cause():
    a = ValueError("a"); b = ValueError("b"); a.__cause__ = b; b.__cause__ = a; raise a
def self_cause():
    a = ValueError("a"); a.__cause__ = a; raise a
def cause_and_context():
    a = ValueError("a"); a.__cause__ = KeyError("cause"); a.__context__ = TypeError("context"); raise a
def cause_is_context():
    c = KeyError("both"); a = ValueError("a"); a.__cause__ = c; a.__context__ = c; raise a
def suppressed():
    a = ValueError("a"); a.__context__ = TypeError("context"); a.__suppress_context__ = True; raise a
def long_chain():
    e = None
    for i in range(4):
        n = ValueError(i); n.__context__ = e; e = n
    raise e
def chain_with_tracebacks():
    try:
        try: 1 / z
        except ZeroDivisionError: d["k"]
    except KeyError: x.a
# ---- groups
EG = ExceptionGroup
def raised(e):
    try: raise e
    except BaseException as r: return r
def group_of_raised(): raise EG("g", [raised(ValueError(1)), raised(TypeError(2))])
def group_wide(): raise EG("wide", [ValueError(i) for i in range(17)])
def group_just_wide_enough(): raise EG("wide", [ValueError(i) for i in range(15)])
def group_one_too_wide(): raise EG("wide", [ValueError(i) for i in range(16)])
def group_deep():
    e = ValueError("bottom")
    for i in range(12): e = EG("level %d" % i, [e])
    raise e
def group_notes():
    e = EG("g", [ValueError(1)]); e.add_note("of the group"); e.exceptions[0].add_note("of the member\nin two lines"); raise e
def group_member_has_cause():
    m = ValueError("member"); m.__cause__ = raised(KeyError("cause")); raise EG("g", [m, TypeError(2)])
def group_member_is_chained_group():
    inner = EG("inner", [ValueError(1)]); inner.__context__ = EG("before", [KeyError(2)]); raise EG("outer", [inner, TypeError(3)])
def group_is_context():
    try: raise EG("first", [ValueError(1)])
    except EG: raise TypeError("second")
def group_after_plain():
    try: 1 / z
    except ZeroDivisionError: raise EG("second", [ValueError(1)])
def group_last_is_group(): raise EG("outer", [ValueError(1), EG("inner", [TypeError(2), EG("innermost", [KeyError(3)])])])
def group_first_is_group(): raise EG("outer", [EG("inner", [TypeError(2)]), ValueError(1)])
def group_same_member_twice():
    v = ValueError("twice"); raise EG("g", [v, v])
def group_syntax_error():
    try: compile("a b", "<s>", "exec")
    except SyntaxError as s: raise EG("g", [s])
def group_message_lines(): raise EG("two\nlines", [ValueError("also\ntwo")])
def base_group(): raise BaseExceptionGroup("b", [KeyboardInterrupt()])
def group_subclass():
    class MyGroup(EG): pass
    raise MyGroup("mine", [ValueError(1)])
def star_leftover():
    try: raise EG("g", [ValueError(1), TypeError(2)])
    except* ValueError: pass
def star_raises():
    try: raise EG("g", [ValueError(1), TypeError(2)])
    except* ValueError: raise KeyError("new")
for n, f in list(globals().items()):
    if type(f) is type(show) and f.__code__.co_argcount == 0: show(f)
