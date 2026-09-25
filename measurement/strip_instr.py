#!/usr/bin/env python3
# Removes the temporary LLIntICStats hooks from the working tree files given on the command line.
import re, sys
for path in sys.argv[1:]:
    s = open(path).read()
    orig = s
    # #if defined(JSC_LLINT_IC_STATS) ... #endif blocks
    s = re.sub(r'#if defined\(JSC_LLINT_IC_STATS\)\n.*?#endif\n', '', s, flags=re.S)
    # single-line LLINT_IC_STATS(...) hooks
    s = re.sub(r'^[ \t]*LLINT_IC_STATS\(.*\)\n', '', s, flags=re.M)
    # include lines
    s = re.sub(r'^#include "LLIntICStats.h"\n', '', s, flags=re.M)
    if s != orig:
        open(path, 'w').write(s)
        print("stripped", path)
