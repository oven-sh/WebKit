# U+FEFF at the beginning of source is passed over if the source is bytes, which is how a file says that it is UTF-8. In a str it is a character like any other, and not one that can be there.
def t(label, f):
    try: r = f()
    except SyntaxError as e: r = ("SyntaxError", e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, e.text)
    except BaseException as e: r = type(e).__name__ + ": " + str(e)
    print(label, "=>", r)
for mode in ("exec", "eval", "single"):
    for label, source in (("bom", "﻿"), ("bom x", "﻿x"), ("bom nl", "﻿\n"), ("bom bom", "﻿﻿x"), ("x bom", "x﻿"), (" bom", " ﻿x"), ("nl bom", "\n﻿x"), ("bom #", "﻿# c"), ("bytes bom", b"\xef\xbb\xbf"), ("bytes bom x", b"\xef\xbb\xbfx"),
                          ("bytes bom bom", b"\xef\xbb\xbf\xef\xbb\xbfx"), ("bytes x bom", b"x\xef\xbb\xbf"), ("in a string", "'﻿'"), ("in a comment", "x # ﻿")):
        t(mode + " " + label, lambda: type(compile(source, "<s>", mode)).__name__)
