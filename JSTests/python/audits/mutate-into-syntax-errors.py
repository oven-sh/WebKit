# Run by CPython. Makes pieces of source that will not compile, out of source that will, by changing one token.
import io, random, sys, tokenize, warnings, ast, os
warnings.simplefilter("ignore")
random.seed(int(sys.argv[1])); want = int(sys.argv[2]); paths = sys.argv[3:]
pieces = []
for path in paths:
    text = open(path, encoding="utf-8").read()
    try: tree = ast.parse(text)
    except SyntaxError: continue
    lines = text.split("\n")
    def visit(body):
        for node in body:
            src = "\n".join(lines[node.lineno - 1:node.end_lineno])
            import textwrap
            src = textwrap.dedent(src) + "\n"
            if 5 < len(src) < 400:
                try: compile(src, "<t>", "exec"); pieces.append(src)
                except Exception: pass
            for name in ("body", "orelse", "finalbody"):
                if isinstance(getattr(node, name, None), list): visit(getattr(node, name))
            for h in getattr(node, "handlers", []): visit(h.body)
    visit(tree.body)
pieces = sorted(set(pieces))
extra = ["(", ")", "[", "]", "{", "}", ",", ":", "=", "==", ":=", "*", "**", ".", "+", "-", "not", "in", "is", "if", "else", "for", "lambda", "yield", "await", "async", "def", "class", "return", "pass", "break", "import", "from", "as", "with", "try", "except", "finally", "None", "True", "1", "x", "'s'", "f'{x}'", "print", "->", "@", ";", "/", "\n", "    ", "match", "case", "type", "del", "global", "raise", "assert", "while", "elif", "or", "and", "!", "$", "?", "1x", "0b2", "\\"]
found = {}
tries = 0
while len(found) < want and tries < want * 40:
    tries += 1
    src = random.choice(pieces)
    try: toks = [t for t in tokenize.generate_tokens(io.StringIO(src).readline) if t.string and t.type not in (tokenize.NEWLINE, tokenize.NL, tokenize.INDENT, tokenize.DEDENT, tokenize.ENDMARKER, tokenize.COMMENT)]
    except Exception: continue
    if not toks: continue
    t = random.choice(toks)
    lines = src.split("\n")
    if t.start[0] != t.end[0]: continue
    line = lines[t.start[0] - 1]; a, b = t.start[1], t.end[1]
    kind = random.randrange(5)
    if kind == 0: new = line[:a] + line[b:]
    elif kind == 1: new = line[:b] + " " + line[a:b] + line[b:]
    elif kind == 2: new = line[:a] + random.choice(extra) + line[b:]
    elif kind == 3: new = line[:a] + random.choice(extra) + " " + line[a:]
    else:
        u = random.choice(toks)
        new = line[:a] + u.string + line[b:]
    lines[t.start[0] - 1] = new
    out = "\n".join(lines)
    if out in found or "\x00" in out: continue
    try: compile(out, "<test>", "exec")
    except SyntaxError: found[out] = 1
    except Exception: pass
print("SOURCES = [")
for s in sorted(found): print("    " + ascii(s) + ",")
print("]")
