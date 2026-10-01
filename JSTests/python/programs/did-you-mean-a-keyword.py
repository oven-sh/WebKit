# What a SyntaxError says of where the last statement was, which nothing looks at but traceback.py, and what that makes of it: a name that may have been meant for a keyword.
import traceback

sources = ["fur x in y: pass", "import os\nclas A:\n    pass\n", "if x:\n    pass\nfor i in y:\n    a = 1\n    b = = 2\n", "def f():\n    if x:\n        pass\n    retrun 1\n", "x = (1,\n", "x = 'abc\n", "  x = 1\n", "if x:\npass\n",
           "class A:\n    def f(self):\n        pass\n    def g(self):\n        whille x: pass\n", "@d\ndef f(): pass\nx y\n", "a = 1\nb = 2\nc d\n", "if a:\n    pass\nelif b:\n    pass\nels:\n    pass\n", "é = 1\nif é:\n  pass\n  ü ö\n", "try:\n    pass\nexcpt E:\n    pass\n", "x = 1 +\n", "f(a b)\n", "def f(:\n    pass\n", "with a:\n    pass\nx = [1, 2\n", "break x\n", "return\n", "def f():\n    x = 1\n    nonlocal x\n"]
for s in sources:
    for mode in ("exec", "single"):
        try: compile(s, "<t>", mode); print(repr(s), mode, "compiles")
        except SyntaxError as e: print(repr(s), mode, type(e).__name__, e.msg, "|", e._metadata)
e = SyntaxError("m"); print(e._metadata); e._metadata = 5; print(e._metadata); del e._metadata; print(e._metadata)
print(type(SyntaxError.__dict__["_metadata"]).__name__, "_metadata" in dir(SyntaxError))
for s in ("if a:\n    pass\ndef f(): pass\nx y\n", "def f(): pass\nif a:\n    pass\nx y\n", "class C: pass\nx y\n", "if a: pass\nclass C: pass\nx y\n", "async def f(): pass\nx y\n", "for i in a: pass\nx y\n", "while a: pass\nx y\n", "try: pass\nfinally: pass\nx y\n", "with a: pass\nx y\n", "match a:\n    case 1: pass\nx y\n", "async def f():\n    async for i in a: pass\n    async with a: pass\n    x y\n", "def f():\n    for i in a:\n        pass\n    def g():\n        while a:\n            pass\n        x y\n", "if a:\n    if b:\n        pass\n    x y\n", "\n\n\nif a: pass\n\n\nx y\n", "if a: pass\nif b: pass\nx y", "    if a: pass\n", "if a: pass; x y\n", "type X = int\nx y\n"):
    try: compile(s, "<t>", "exec"); print(repr(s), "compiles")
    except SyntaxError as e: print(repr(s), e.msg, "|", e._metadata[:2])
print("---- what is suggested")
for source in ("fur x in range(3):\n    print(x)\n", "def f():\n    retrun 1\n", "whille True:\n    pass\n", "import os\nclas A:\n    pass\n", "if x:\n    pass\nelfi y:\n    pass\n", "asynch def f(): pass\n", "form os import path\n", "from os imprt path\n", "x = 1 fi y else 2\n", "x = 1 if y esle 2\n", "with a sa b: pass\n", "for x ni y: pass\n", "x = a adn b\n", "x = nto a\n", "lambd x: x\n", "tyr:\n    pass\nfinally:\n    pass\n", "try:\n    pass\nexcpet E:\n    pass\n", "def f():\n    yeild 1\n", "asert x\n", "rasie E\n", "dle x\n", "pas\n", "brek\n", "x y\n", "foo bar baz\n", "prnt 'a'\n", "print 'a'\n", "def f():\n    if x:\n        pass\n    for i in y:\n        whlie z: pass\n", "class A:\n    dfe f(self): pass\n", "a = 1\nb = 2\nfro i in x: pass\n", "matc x:\n    case 1: pass\n", "x = [i fro i in y]\n", "x = (yeild)\n", "Fals = 1 2\n", "x = Non e\n"):
    try:
        compile(source, "<test>", "exec")
        print(repr(source), "compiles")
    except SyntaxError as e:
        print(repr(source))
        print("".join("    " + line for line in traceback.format_exception_only(e)), end="")
