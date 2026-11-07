# The engine half of `bun build --compile --aot`

This folder reads a program as IR (`ir/`), compiles it in three passes, and writes the image (`image/`). `driver/` runs them in order. **Every function is visited exactly twice.**

1. **Scan, going up.** By function, in parallel. It leaves a thin summary of each function, with nothing about locals.
2. **Link.** It reads summaries only: what is reached, who calls whom, the order, what fields and results hold, the layout of objects, each function's convention.
3. **Walk, going down.** By function, callers before callees. It reads the body once more and builds `Code` (`walk/AOTCode.h`), which the sweep, the emitters (`codegen/`) and the back end (`machine/`) turn into machine code.

**One function, one walk, one body.** No function has a second version, and its entries are prefixes of the body. How an operation is compiled follows from what is known of its value:

| What is known of the value | Outside a loop | In a loop |
| --- | --- | --- |
| Proven by flow | typed | typed |
| Declared in the source | one test at the function's entry, then typed; the failing side is a closed arm in the same body | the same |
| A weaker guess | closed | one test, the typed run, one closed arm, rejoin |
| Nothing | closed | closed |

**Closed** is the form of the operation that calls a shared stub. **Proven** is: by the value's origin in the function, by a test that dominates the use, by Link, or by what a caller had proven. **A TypeScript type is a guess. It never removes a test.**
**A suspension is a call that may return on another stack:** for `await` and `yield` the frame is saved and restored.

The folder also holds the runtime that compiled programs call (`object/`, `runtime/`, `stubs/`, `builtins/`). The front end (resolve, parse, check types, lower to IR) is in oven-sh/bun, `src/aot`.

- How the compiler works: `docs/compiler/internals.mdx` in oven-sh/bun.
- The tests of this folder: [`tests/README.md`](tests/README.md). Checking tools: `Tools/Scripts/aot/README.md`. How to build, and what CI does: `CLAUDE.md` at the root.

## The fast loop

The paths of tests are relative to `aot/tests`.

| Step | Command | Time |
| --- | --- | --- |
| Build the test program | `cmake --build <build> --target testaot` | a minute after a small change |
| Run the tests of what you changed | `<build>/bin/testaot run <folder>`, `testaot run <file>.ir` | seconds |
| Run everything | `testaot all .` | |
| Print what a stage makes of a function | `testaot walk f.ir f3` prints the Code of `f3` after the walk and the sweep. The back end has commands too (`emit`, `order`, `allocate`, `frame`, `bytes`): `testaot list commands` | |
| Fuzz register allocation | `testaot fuzz-allocation`, then `--show=<seed>` for a seed that failed | |
| Whole JavaScript and TypeScript programs | need the front end: `test/aot/shell/quick.py` in oven-sh/bun takes this `testaot` and `jsc` | seconds a program |

`f.ir` is IR text, or what `bun build --compile --aot --aot-out=<file>` writes.

Every boundary between two stages is text, so a bug gets a test at the stage that has it. A rewrite of the walk gets a test for what it does **and** for what it must leave alone.

The walk must never merge or remove a call of a host function or getter: `url.href` twice with nothing between stays two calls. Whatever effects the row of such a built-in names, the walk may not answer a second `CallBuiltin` with the first, and the sweep may drop neither (`Sweep::isRoot()`, `walk/AOTSweep.cpp`).
A call of a built-in of the language (`BuiltinOrigin::Engine`) whose result nothing reads is dropped by the effects of its row with the operands at hand: `s.padStart(8)`, `Math.hypot(a, b)` on numbers.
A row of a host function or getter is not checked for what it leaves out: what a call changes may be read through another object (`params.append()` and `url.href`, `controller.abort()` and a kept `signal.aborted`) or be ambient (the working folder, the environment, the time zone, the files).
Whoever makes such a call mergeable or removable runs `test/aot/fuzz/case-scripts/state-held-by-another-object.py` in oven-sh/bun first. While the rule holds, only those of its programs whose reads become loads can fail.

## What CI catches that a local build does not

A plain local build is one compiler, one C++ library, assertions on or off. CI has some eighty lanes. Four cheap local builds catch nearly everything the others have ever caught, and each takes
minutes where a round of CI takes 35. `node .github/scripts/lanes.mjs build <label> --output <dir>` reproduces any lane exactly, in Docker; the forms below are quicker because they skip optimization.

| Build | What it catches | How |
| --- | --- | --- |
| musl with assertions, `-O0`, `ninja -k 0` | What libstdc++ on Alpine does not accept (a `std::atomic` member without an initializer, a missing `#include` that glibc's headers pull in), and code inside `ASSERT` that no longer compiles | `--target=x86_64-alpine-linux-musl --sysroot=<an Alpine root>`, `-DENABLE_ASSERTS=ON` |
| The hardened lanes' defines, syntax only | Code under `BUN_HARDENED`, `ENABLE_AOT_POSITION_INDEPENDENT_IMAGE`, `ENABLE_AOT_GUARDED_PAGES` | the flags are `HARDENING` in `.github/scripts/lanes.mjs`; `clang -fsyntax-only` over the sources of `aot/` |
| `-DENABLE_AOT=0` among the compiler's flags, `-O0` | Every lane without `-aot` in its label. What the rest of JavaScriptCore calls outside `ENABLE(AOT)` has to be in `JavaScriptCore_AOT_SOURCES_WITHOUT_AOT` (`AOT.cmake`), or twenty lanes fail at the link | three minutes from nothing |
| x64 glibc with ThinLTO, then the checks behind the link | `Tools/Scripts/check-classinfo-uniqueness.py`, `Tools/Scripts/aot/find-structures-decoded-behind-calls.py`, symbols newer than `GLIBC_2.31` | four minutes with a warm ThinLTO cache |

Without a whole build, in about a minute: `Tools/Scripts/aot/check-includes.py`.

## Things that have bitten

- **`aot/` is compiled in unified bundles.** A `static` function, a type, or anything in an unnamed namespace needs a name that no other source of `aot/` uses.
- **A new source goes into `Sources.txt`** (or, if builds spend no time in it, `SourcesCompiledForSize.txt` or `SourcesCompiledForMinimumSize.txt`: `AOT.cmake` says which). Bun reads the same lists when it builds this folder itself.
- **Nothing that a generator writes is checked in.** A new generated header that Bun's build reads must also be copied into the release by every Dockerfile's pack step, or Bun cannot be built against the prebuilt engine.
- **No static initializers.**
- **A change to the IR's schema (`ir/schema/*.def`) changes both repositories.** They land together.
- **A switch over an enum of the compiler names every enumerator**, with no `default`. A test in oven-sh/bun reads the sources and checks it.
- **Nothing compares the text of a name of the program with a literal**: not an identifier, not a property, not a module specifier. `name == "eval"` says yes to a local that somebody called `eval`, and no to `const e = eval`. Ask about a thing: this binding, this object, this cell. Comparing ids is fine, and text becomes an id in the table of built-ins. `test/aot/compiler/names-compared-as-text.test.ts` in oven-sh/bun reads the sources of both halves and checks it. `allowed`, in `names-compared-as-text.ts` beside it, lists the few files that may. `x["name"]` is `x.name`: `test/aot/tools/same-code-in-every-spelling.test.ts`.
- **When an object is built shows at run time.** A `NewObject` without a prototype operand gets whatever its constructor function holds as `prototype` when the record of its allocation site is first filled (`StructureTable::fillAllocationSite()`), and the effects of the operation do not say so. After `const o = new F(); F.prototype = p`, `o` still has the old prototype. So an object is never built behind a store to that `prototype` which the program makes after the allocation. A `NewObject` that is handed its prototype may end a guard of its class when it is built (`object/AOTClassGuards.h`: no instance got another prototype), so it is also built before every test of those guards that follows it in the program.
- **A `Structure*` decoded from its id must not be the only thing that keeps a structure alive across a call that may collect.** `find-structures-decoded-behind-calls.py` reads the finished binary for that.
- **An operation that compiled code calls says in its declaration whether it may collect, throw or enter the VM.** The checking builds hold it to that at run time.
- **Files that upstream owns are not split or moved.** It makes every later merge harder. `b3/` is upstream's as it stands: this compiler does not use B3 or Air.
