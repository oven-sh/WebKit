# What is said to be wrong with source that will not compile, and where: the class of the exception, msg, lineno, offset, end_lineno, end_offset and text.

import _warnings
from syntax_errors_from_tests import SOURCES as from_tests
from syntax_errors_by_mutation import SOURCES as by_mutation

_warnings.filters.insert(0, ("ignore", None, Warning, None, 0))


def attempt(group, index, source, mode):
    try:
        compile(source, "<test>", mode)
        result = "compiles"
    except SyntaxError as e:
        result = (type(e).__name__, e.msg, e.lineno, e.offset, e.end_lineno, e.end_offset, e.text)
    except BaseException as e:
        result = (type(e).__name__, str(e))
    print(group, index, "|", ascii(result), "|", ascii(source[:60]))


for group, sources in (("tests", from_tests), ("mutation", by_mutation)):
    for index, source in enumerate(sources):
        attempt(group, index, source, "exec")
# The other two ways of compiling begin elsewhere in the grammar, and do not take a last line that nothing ends to be ended.
for mode in ("eval", "single"):
    for index, source in enumerate(by_mutation[::3]):
        attempt(mode, index, source.rstrip("\n") if index % 2 else source, mode)
