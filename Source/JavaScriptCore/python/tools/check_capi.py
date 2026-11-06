#!/usr/bin/env python3
"""check_capi.py: every function that C calls (PY_CAPI in ../capi/*.cpp) says in its first line whether it is quiet or noisy: see CAPI_QUIET in ../capi/CApi.h.
Prints the quiet ones, which are to be few. Fails if a function says nothing, or if one that asks nobody who calls (CAPI_QUIET_AND_ASKS_NOBODY()) names the interpreter,
or asks after all."""
import glob, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
quiet, asking_nobody, silent, looking = [], [], [], []
for path in sorted(glob.glob(f"{here}/../capi/*.cpp")):
    for name, first, body in re.findall(r"^PY_CAPI [^\n;(]*?\b(\w+)\([^\n;]*\)\n\{\n\s*([^\n]*)\n(.*?)^\}", open(path).read(), re.M | re.S):
        if first.startswith("CAPI_QUIET("):
            quiet.append(name)
        elif first.startswith("CAPI_QUIET_AND_ASKS_NOBODY()"):
            asking_nobody.append(name)
            if re.search(r"\b(interpreter\(\)|s_interpreter|mayEnter|hasGIL|CAPI_\w+)\b", body):
                looking.append(f"{os.path.basename(path)}: {name}")
        elif not first.startswith(("CAPI_ENTER(", "CAPI_NOISY(")) and not re.match(r"TakenOver \w+\(\w+\);", first):
            silent.append(f"{os.path.basename(path)}: {name}")
print(f"{len(quiet)} quiet:", " ".join(sorted(quiet)))
print(f"{len(asking_nobody)} quiet, and ask nobody:", " ".join(sorted(asking_nobody)))
if silent:
    print("These do not say:", *silent, sep="\n  ")
if looking:
    print("These ask nobody and name the interpreter:", *looking, sep="\n  ")
sys.exit(bool(silent or looking))
