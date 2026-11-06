#!/usr/bin/env python3
"""check_capi.py: every function that C calls (PY_CAPI in ../capi/*.cpp) says in its first line whether it is quiet or noisy: see CAPI_QUIET in ../capi/CApi.h.
Prints the quiet ones, which are to be few. Fails if a function says nothing, or if one that asks nobody who calls (CAPI_QUIET_AND_ASKS_NOBODY()) names the interpreter,
or asks after all. --self-test: whether it would see that."""
import glob, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
# What a function that asks nobody does not say. (Py_IsInitialized() asks whether one of the pointers is null.)
names_interpreter = re.compile(r"\b(interpreter\w*\(|current\b|s_interpreter\w*|s_runtime\w*|hasGIL\w*|CAPI_\w+|Entry\b)")
may_name = {"Py_IsInitialized": "return !!s_runtimeItIsFor.load(std::memory_order_relaxed);"}
if sys.argv[1:] == ["--self-test"]:
    seen = ["return interpreter().counts.get(cell);", "interpreter();", "f(interpreter())", "interpreterToEnter()", "current->vm", "s_interpreter.load()",
        "s_interpreterWhoseCountsFall.load()", "s_runtimeItIsFor.load()", "hasGIL(x)", "hasGILSlow(x)", "CAPI_ENTER(0);", "Entry entry(a, b);"]
    unseen = ["retain(object);", "return toC(value);", "return plain(object).isTrue();", "setErrorIndicator(nullptr);"]
    wrong = [line for line in seen if not names_interpreter.search(line)] + [line for line in unseen if names_interpreter.search(line)]
    print("wrong:", wrong)
    sys.exit(bool(wrong))
quiet, asking_nobody, silent, looking = [], [], [], []
for path in sorted(glob.glob(f"{here}/../capi/*.cpp")):
    for name, first, body in re.findall(r"^PY_CAPI [^\n;(]*?\b(\w+)\([^\n;]*\)\n\{\n\s*([^\n]*)\n(.*?)^\}", open(path).read(), re.M | re.S):
        if first.startswith("CAPI_QUIET("):
            quiet.append(name)
        elif first.startswith("CAPI_QUIET_AND_ASKS_NOBODY()"):
            asking_nobody.append(name)
            if names_interpreter.search(body) and body.strip() != may_name.get(name):
                looking.append(f"{os.path.basename(path)}: {name}")
        elif not first.startswith(("CAPI_ENTER(", "CAPI_NOISY(")):
            silent.append(f"{os.path.basename(path)}: {name}")
print(f"{len(quiet)} quiet:", " ".join(sorted(quiet)))
print(f"{len(asking_nobody)} quiet, and ask nobody:", " ".join(sorted(asking_nobody)))
if silent:
    print("These do not say:", *silent, sep="\n  ")
if looking:
    print("These ask nobody and name the interpreter:", *looking, sep="\n  ")
sys.exit(bool(silent or looking))
