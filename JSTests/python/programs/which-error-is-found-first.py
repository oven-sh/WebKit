# Where more than one thing is wrong with a piece of source, what is said is what is come to first, going through it from the top. What is in a function is come to where the function is defined.

for source in [
    "def f():\n  x = *a\nbreak", "def f():\n  break\ncontinue", "class C:\n  return\nbreak", "x = lambda: (yield)\nbreak", "def f():\n  def g():\n    break\n  continue\nreturn", "break\ndef f():\n  continue",
    "x = [(yield) for a in b]\nbreak", "def f(a=(yield)): pass\ndef g():\n  break", "def f():\n  break\ndef g():\n  continue", "@d\ndef f[T=int, U](): break", "def f[T: (yield)](): pass\nbreak",
    "def f():\n  f(a=1, a=2)\nf(b=1, b=2)", "x = (lambda: f(a=1, a=2)), f(b=1, b=2)", "def f():\n  match x:\n    case a | b: pass\nbreak", "def f() -> (yield): pass\nbreak",
    "def f():\n  del *a\nfor x in y: pass\nelse: break", "class C:\n  def m(self):\n    return *a\n  break\ncontinue", "def f():\n  try: pass\n  except: pass\n  except E: pass\nbreak",
    # The decorators, the defaults, the annotations and then the body.
    "@f(a=1, a=2)\ndef g(b=f(c=1, c=2)): break", "def g(b=f(c=1, c=2), d: f(e=1, e=2) = 0): break", "def g(d: f(e=1, e=2)): break", "def g(d: f(e=1, e=2) = f(c=1, c=2)): pass", "def g() -> f(e=1, e=2): break",
    "@f(a=1, a=2)\nclass C(f(b=1, b=2)): break", "class C(f(b=1, b=2)): break", "class C(m=1, m=2): break", "class C(*f(b=1, b=2), m=1, m=2): pass",
    # What evaluates the annotations of a class comes after its body, and of a module after everything.
    "class C:\n  x: f(a=1, a=2)\n  break", "class C:\n  x: f(a=1, a=2)\n  def m(self): break", "x: f(a=1, a=2)\nbreak", "x: f(a=1, a=2)\ndef g(): break", "x: f(a=1, a=2)\ny: f(b=1, b=2)", "def g(): break\nx: f(a=1, a=2)",
    "x: f(a=1, a=2)\nclass C:\n  y: f(b=1, b=2)", "x: (lambda: f(a=1, a=2))\ny = lambda: f(b=1, b=2)",
    # Type parameters, in their order.
    "def f[T: g(a=1, a=2), U: g(b=1, b=2)](): pass", "def f[T = g(a=1, a=2), U](): pass", "def f[T = int, U: g(b=1, b=2)](): pass", "def f[T: g(a=1, a=2) = g(b=1, b=2)](): pass", "class C[T: g(a=1, a=2)](g(b=1, b=2)): break",
    "type A[T: g(a=1, a=2)] = g(b=1, b=2)", "type A[T] = g(b=1, b=2)\nbreak", "def f[T](x=g(a=1, a=2), y: g(b=1, b=2) = 0): break",
    # What the symbol table finds is found before any of it.
    "break\ndef f(a, a): pass", "def f():\n  break\ndef g():\n  nonlocal x", "f(a=1, a=2)\nx = 1\nglobal x" if 0 else "f(a=1, a=2)\ndef g():\n  x = 1\n  global x",
    # Comprehensions and generators.
    "x = [f(a=1, a=2) for b in f(c=1, c=2)]", "x = (f(a=1, a=2) for b in f(c=1, c=2))", "x = (f(a=1, a=2) for b in c), f(d=1, d=2)", "x = [[f(a=1, a=2) for b in c] for d in f(e=1, e=2)]",
]:
    try:
        compile(source, "<test>", "exec")
        result = "compiles"
    except SyntaxError as error:
        result = (error.msg, error.lineno, error.offset)
    print(ascii(source), "=>", result)
