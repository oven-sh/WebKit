# Where something is said to be wrong, when it is CPython's tokenizer that says so and there are characters before it that take more than a byte. For most things it counts in characters. For two it gives what it has,
# which is in bytes.
for source in ("\xe9 = 0123", "\xe9\xe9 = ub''", "\xe9 = 1_", "\xe9 = 'abc", "\xe9 = 0b12", "\xe9 = (]", "\xe9 = 1a", "\xe9 = \U0001F600", "\xe9 = $", "\xe9 = f'{'", "\xe9 = f'}'", "\xe9\xe9 = 00012 + x", "'\xe9'; bf''", "\U0001F600 = 1; x = 007", "'中'; tf''", "x = 0123", "ub''"):
    try:
        compile(source, "<s>", "exec")
    except SyntaxError as e:
        print(ascii(source), e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, ascii(e.text))

# How many strings with expressions in them can be one inside another
for prefix in ("f", "t", "rf"):
    for quote in ("'", '"""'):
        for n in (147, 148, 149, 150):
            try:
                compile(prefix + quote + ("{" + prefix + quote) * n, "<s>", "exec")
            except SyntaxError as e:
                print(prefix, quote, n, e.msg, e.lineno, e.offset, e.end_offset)
