import _warnings
_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))
for s in ["", "\n", "\n\n", "  \n", "# c", "# c\n", "x", "x\n", "x\n\n", "x\n# c\n", "x; y", "x; y\n", "x;\n", "x\ny", "x\ny\n", "x\n  y\n", "x\n)", "x\n'", "x # c\ny", "if x: y", "if x: y\n", "if x:\n  y", "if x:\n  y\n", "if x:\n  y\n\n", "if x:\n  y\nz", "if x:\n  y\nelse:\n  z\n", "if x:\n  y\n# c\n", "if x:\n  y\n  # c\n", "def f(): pass", "def f():\n  pass\n\n\nx", "class C: pass\nx", "for x in y: z\n", "while x: y\nelse: z\n", "with x: y\n", "try: x\nfinally: y\n", "match x:\n  case y: z\n", "@d\ndef f(): pass\n", "x = (\n1)\n", "x = (\n1)\ny", "x \\\n+ 1", "\nx", "\n\nx\n", "  x", "x\n\x0c", "x\t\n \n", "x = 1 2", "if x:\n", "if x", "1 +", "(", "pass\npass", "async def f(): pass\n", "import a, b\n", "x: int = 1\n", "lambda: 1\n", "'''a\nb'''\n", "'''a\nb'''\nx", 'def f():\n  x\n  ', 'def f():\n  x\n ', 'def f():\n  x\n    ', 'if x:\n  y\n  ', 'if x:\n  y\n  # c', 'x\n  ', 'if x:\n  y\n\t', 'if x:\n  y\n  \n', 'if x:\n  y\n  \n  ', 'if x:\n  if y:\n    z\n  ', 'if x:\n  if y:\n    z\n    ', 'import ', 'x \\', '(\\', 'f(a=1']:
    try:
        compile(s, "<t>", "single"); r = "compiled"
    except SyntaxError as e: r = (type(e).__name__, e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, e.text)
    print(ascii(s).ljust(34), r)
