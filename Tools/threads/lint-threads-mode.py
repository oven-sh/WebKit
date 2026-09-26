#!/usr/bin/env python3
"""lint-threads-mode.py [<Source/JavaScriptCore>]: the rules of functions compiled once per threads mode that a compiler
does not check (runtime/ThreadsModePage.h; docs/threads/SPEC-ungil-history.md, thirteenth round).

  1. JSC_PER_THREADS_MODE_BEGIN and _END pair up, in this order, in every file.
  2. A function that runs in the cleared stack of a call slow path (sanitizeStackForVMInCallSlowPath,
     ASSERT_CALL_SLOW_PATH_RUNS_IN_CLEARED_STACK) is not compiled per mode: two copies of its body in one frame exceed the
     frame's budget in a build without optimization.
  3. The files whose code runs before the mode is latched do not read the mode byte and compile nothing per mode: they
     would run the copy without threads in a process that is about to have them.
  4. A body that calls a generic lambda by its template arguments is not compiled per mode in place (inside the templated
     lambda the call needs the `template` keyword, which the body as written does not have).
  5. Nothing but runtime/ThreadsModePage.h and .cpp names the mode page: the rest of JavaScriptCore reads it through the
     accessors, which go through the hidden name (one compare with memory instead of a load through the offset table).
  6. The test of the threads counters begins with the test of the mode (JSThreadsCounters::enabled()), so that it
     folds in the copies compiled for a process without threads.

Exit status 1 when a rule is broken."""
import glob, os, re, sys
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Source", "JavaScriptCore")
PRE_LATCH = ["runtime/Options.cpp", "runtime/Options.h", "runtime/OptionsList.h", "runtime/JSCConfig.cpp", "runtime/InitializeThreading.cpp",
             "jit/ExecutableAllocator.cpp", "jit/JITOperationList.cpp", "heap/StructureAlignedMemoryAllocator.cpp"]
bad = 0; converted = 0
for p in sorted(glob.glob(os.path.join(root, "**", "*.cpp"), recursive=True) + glob.glob(os.path.join(root, "**", "*.h"), recursive=True)):
    rel = os.path.relpath(p, root)
    if rel in ("runtime/ThreadsModePage.h", "runtime/ThreadsModePage.cpp"): continue
    text = open(p, errors="replace").read()
    for i, l in enumerate(text.split("\n"), 1):
        if re.search(r"\bg_jscThreadsModePage(Local)?\b", l) and not l.strip().startswith(("//", "*", "#")):
            print("%s:%d: names the mode page; use the accessors of ThreadsModePage.h" % (rel, i)); bad += 1
    if rel == "runtime/JSThreadsCounters.h":
        m = re.search(r"static bool enabled\(\) \{ return ([^;]*);", text)
        if not m or not m.group(1).lstrip().startswith("threadsMode()"):
            print("%s: JSThreadsCounters::enabled() does not begin with the test of the mode" % rel); bad += 1
    if rel in PRE_LATCH:
        m = re.search(r"\b(processUses\w+|processIsGILOff|threadsMode)\s*\(|JSC_PER_THREADS_MODE|JSC_CALL_PER_THREADS_MODE", text)
        if m:
            print("%s: runs before the latch and uses %s" % (rel, m.group(0))); bad += 1
        continue
    if "JSC_PER_THREADS_MODE_BEGIN" not in text: continue
    depth = 0; start = None
    for i, l in enumerate(text.split("\n"), 1):
        s = l.strip()
        if s.startswith("JSC_PER_THREADS_MODE_BEGIN("):
            if depth: print("%s:%d: BEGIN inside a per-mode body" % (rel, i)); bad += 1
            depth = 1; start = i; converted += 1
        elif s in ("JSC_PER_THREADS_MODE_END", "JSC_PER_THREADS_MODE_END_WITHOUT_RETURN"):
            if not depth: print("%s:%d: END without BEGIN" % (rel, i)); bad += 1
            depth = 0
        elif depth:
            if re.search(r"sanitizeStackForVMInCallSlowPath|ASSERT_CALL_SLOW_PATH_RUNS_IN_CLEARED_STACK", l):
                print("%s:%d: a call slow path that runs in the cleared stack is compiled per mode (body from line %d)" % (rel, i, start)); bad += 1
            if re.search(r"\.operator\(\)<", l):
                print("%s:%d: a generic lambda called by its template arguments inside a per-mode body (from line %d)" % (rel, i, start)); bad += 1
    if depth: print("%s: BEGIN at line %d without END" % (rel, start)); bad += 1
print("threads-mode lint: %d functions compiled per mode in place, %d findings" % (converted, bad))
sys.exit(1 if bad else 0)
