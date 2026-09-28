INC, DEDENT = 0x4000, 0x200
for s in ["if x:", "if x:\n", "if x:\n  y", "if x:\n  y\n", "if x:\n  y\n\n", "x = (", "x = (1,\n", "x = '''a", "x = 'a", "def f(", "x +", "x + \\", "x + \\\n", "1 2", "if x", "class C:\n  def f(self):\n", "try:\n  x\n", "try:\n  x\nexcept:\n", "@d\n", "x = [\n1,\n2", "f'{", "f'{x", "for x in", "while", "lambda", "", "\n", "x", ")", "if x:\n  y\nelse:", "match x:\n", "match x:\n  case 1:\n", "a b", "with x as", "import", "from a import (", "x = {1:", "async def f():\n"]:
    for flags in (INC, DEDENT, INC | DEDENT):
        try: compile(s, "<t>", "single", flags); r = "compiled"
        except SyntaxError as e: r = (type(e).__name__, e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, e.text)
        except BaseException as e: r = ("!!", type(e).__name__, str(e))
        print(hex(flags), ascii(s).ljust(34), r)
