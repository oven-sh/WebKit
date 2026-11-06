#!/usr/bin/env python3
"""check_capi.py: every function that C calls (PY_CAPI in ../capi/*.cpp) says in its first line whether it is quiet or noisy: see CAPI_QUIET() in ../capi/CApi.h.
Prints the quiet ones, which are to be few, and fails if a function says nothing."""
import glob, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
quiet, silent = [], []
for path in sorted(glob.glob(f"{here}/../capi/*.cpp")):
    for name, first in re.findall(r"^PY_CAPI [^\n;(]*?\b(\w+)\([^\n;]*\)\n\{\n\s*([^\n]*)\n", open(path).read(), re.M):
        if first.startswith("CAPI_QUIET()"):
            quiet.append(name)
        elif not first.startswith(("CAPI_ENTER(", "CAPI_NOISY(")) and not re.match(r"TakenOver \w+\(\w+\);", first):
            silent.append(f"{os.path.basename(path)}: {name}")
print(f"{len(quiet)} quiet:", " ".join(sorted(quiet)))
if silent:
    print("These do not say:", *silent, sep="\n  ")
sys.exit(bool(silent))
