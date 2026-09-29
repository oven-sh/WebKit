# `from __future__ import feature` is for the compiler, and is an import besides: it binds the names.
from __future__ import annotations, division as d
from __future__ import generator_stop
import __future__


def t(label, f):
    try:
        r = f()
    except BaseException as e:
        r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)


t("bound", lambda: (annotations, d, generator_stop))
t("they are what the module has", lambda: (annotations is __future__.annotations, d is __future__.division))
t("what one has", lambda: (annotations.optional, annotations.mandatory, annotations.compiler_flag, annotations.getOptionalRelease(), annotations.getMandatoryRelease(), type(annotations).__name__, type(annotations).__module__))
t("all of them", lambda: [(name, getattr(__future__, name)) for name in __future__.all_feature_names])
t("what the module has", lambda: sorted(name for name in dir(__future__) if not name.startswith("__")))
t("__all__", lambda: __future__.__all__)
t("the flags are what compile() takes", lambda: [bool(compile("x: y", "<s>", "exec", flag).co_flags & flag) for flag in (__future__.annotations.compiler_flag, __future__.barry_as_FLUFL.compiler_flag)])
t("and it does what it asks", lambda: (lambda n: (exec("from __future__ import annotations\ndef f(a: nothing): pass", n), n["f"].__annotations__, n["annotations"]))({})[1:])
t("in a function it is too late", lambda: compile("def f():\n    from __future__ import annotations", "<s>", "exec"))
t("what there is none of", lambda: compile("from __future__ import nothing", "<s>", "exec"))
t("all of them at once", lambda: compile("from __future__ import *", "<s>", "exec"))
t("by another name", lambda: (lambda n: (exec("from __future__ import annotations as a, division", n), sorted(k for k in n if not k.startswith("__"))))({})[1])
t("the module is a module", lambda: (type(__future__).__name__, __future__.__name__))
