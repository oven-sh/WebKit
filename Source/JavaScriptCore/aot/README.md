# The engine half of `bun build --compile --aot`

This folder reads a program as IR, analyzes and optimizes it, generates machine code through B3 and Air, writes the image, and holds the runtime that compiled programs call.
The front end (resolve, parse, check types, lower to IR) is in oven-sh/bun, `src/aot`.

- How the compiler works: `docs/compiler/internals.mdx` in oven-sh/bun.
- The design rules, how to measure, and what was tried and did not work: `src/aot/CLAUDE.md` in oven-sh/bun. They hold for this folder too: the two repositories are one product.
- The tests of this folder: [`tests/README.md`](tests/README.md). Checking tools: `Tools/Scripts/aot/README.md`. How to build, and what CI does: `CLAUDE.md` at the root.

## The fast loop

| Step | Command | Time |
| --- | --- | --- |
| Build the test program | `cmake --build <build> --target testaot` | a minute after a small change |
| Run the tests of what you changed | `<build>/bin/testaot passes/sink-allocations`, `testaot <file>.ir` | seconds |
| Run everything | `testaot all` | a minute on eight cores |
| Print the IR of a program behind a pass | `testaot print <program> --until=<pass> --function=f12`; `testaot list passes` | |
| Whole JavaScript and TypeScript programs | need the front end: `test/aot/shell/quick.py` in oven-sh/bun takes this `testaot` and `jsc` | seconds a program |

A pass gets an IR text test for what it does **and** for what it must leave alone.

A pass that moves an allocation asks `creationMayBeOrderedWithGuards()` and `guardsOrderedWithCreation()` (`passes/AOTVirtualObjectShapes.h`), as SinkAllocations does. A `NewObject`
without a prototype operand takes what its constructor function holds as `prototype` when its site first allocates, and the effects of the operation do not say so.

No pass merges or removes a call of a host function or getter: `url.href` twice with nothing between stays two calls. The rows of the built-ins are relied on for that and are not checked against it: what a call changes may be
read through another object (`params.append()` and `url.href`, `controller.abort()` and a kept `signal.aborted`) or be ambient (the working folder, the environment, the time zone, the files). Whoever makes such a call
mergeable or removable runs `test/aot/fuzz/case-scripts/state-held-by-another-object.py` in oven-sh/bun first: of its 402 programs only 14 can fail today.

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
- **A `Structure*` decoded from its id must not be the only thing that keeps a structure alive across a call that may collect.** `find-structures-decoded-behind-calls.py` reads the finished binary for that.
- **An operation that compiled code calls says in its declaration whether it may collect, throw or enter the VM.** The checking builds hold it to that at run time.
- **B3 and Air are shared with the JIT.** A change there is judged by plain JavaScriptCore too (`testb3`, `testair`, JSTests), and by how it scales on very large functions.
- **Files that upstream owns are not split or moved.** It makes every later merge harder.
