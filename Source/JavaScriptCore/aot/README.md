# The engine half of `bun build --compile --aot`

This folder reads a program as IR, compiles it to machine code and writes the image that goes into the executable. It also holds the runtime that compiled programs call. The front end (resolve, parse, check types, lower to IR) is in oven-sh/bun, `src/aot`.

## Pipeline

`driver/` runs these in order. Each stage prints what it made as text, through a command of `testaot`.

| Stage | Folder | What it does | Command |
| --- | --- | --- | --- |
| Scan | `facts/` | By function, in parallel. Leaves a short summary of each function, with nothing about locals | `summary` |
| Link | `facts/` | Reads the summaries only: what is reached, who calls whom, what fields and results hold | `link` |
| Plan | `plan/` | The layout of objects, the calling convention of each function, the units to compile | `plan` |
| Walk | `walk/` | By function, callers before callees. Reads the body again, optimizes it and builds `Code` (`walk/AOTCode.h`). The sweep then removes what is dead | `walk` |
| Emit | `codegen/`, `machine/` | The emitters turn `Code` into operations. The back end orders blocks, allocates registers and writes bytes | `emit`, `allocate`, `bytes` |
| Image | `image/` | Links the code and writes the tables that the runtime reads | `image` |

A value's type is proven by its origin in the function, by a check that dominates the use, by Link, or by what a caller had proven. A TypeScript type is a speculation. It never removes a check.

JavaScript does not go through B3 or Air: `machine/` is the back end. Only WebAssembly modules do (`image/AOTWasmModules.cpp`).

The compiler takes no profile, runs none of the program's code during the build (macros are refused), keeps no cache between builds and adds no syntax.

## Other folders

| Folder | What it holds |
| --- | --- |
| `ir/` | the IR: its schema (`ir/AOTSchema.def`, `ir/schema/*.def`), the generator of its C++, the reader and the verifier |
| `object/`, `runtime/`, `stubs/` | the runtime: objects and classes, the operations that compiled code calls, shared stubs |
| `builtins/` | the table of built-ins (`*.builtins.ts`) and the emitters of calls to them |
| `support/` | options, diagnostics, threads |
| `tests/` | `testaot` and its tests. See [`tests/README.md`](tests/README.md) |

`Tools/Scripts/aot/README.md` has the checking tools. `CLAUDE.md` at the root says how to build and what CI does.

## Build and test

| Step | Command |
| --- | --- |
| Build the test program | `cmake --build <build> --target testaot`, or `bun run build:local --target=testaot` in oven-sh/bun |
| Run the tests of what you changed | `testaot run <file or folder>` |
| Run everything | `testaot all aot/tests` |
| Print what a stage makes of a function | `testaot walk f.ir f3`. `testaot list commands` prints the others |
| Fuzz register allocation | `testaot fuzz-allocation`, then `--show=<seed>` for a seed that failed |

`f.ir` is IR text, or what `bun build --compile --aot --aot-out=<file>` writes.

Whole programs need the front end, so they are in oven-sh/bun, under `test/aot`. Each one runs under `bun <file>` and as an executable built four ways: as written, with its types erased, with speculations that fail on purpose, and with proof checks. Every run must print the same. `test/aot/other-suites` there does the same, by hand, with test262, JSTests, Node's tests and Bun's.

## What CI catches that a local build does not

`node .github/scripts/lanes.mjs build <label> --output <dir>` reproduces any lane, in Docker. These local builds are quicker, because they skip optimization:

| Build | What it catches | How |
| --- | --- | --- |
| musl with assertions, `-O0`, `ninja -k 0` | What libstdc++ on Alpine does not accept, and code inside `ASSERT` that no longer compiles | `--target=x86_64-alpine-linux-musl --sysroot=<an Alpine root>`, `-DENABLE_ASSERTS=ON` |
| The hardened lanes' defines, syntax only | Code under `BUN_HARDENED`, `ENABLE_AOT_POSITION_INDEPENDENT_IMAGE`, `ENABLE_AOT_GUARDED_PAGES` | the flags are `HARDENING` in `.github/scripts/lanes.mjs`; `clang -fsyntax-only` over the sources of `aot/` |
| `-DENABLE_AOT=0` among the compiler's flags, `-O0` | Every lane without `-aot` in its label. What the rest of JavaScriptCore calls outside `ENABLE(AOT)` has to be in `JavaScriptCore_AOT_SOURCES_WITHOUT_AOT` (`AOT.cmake`) | |
| x64 glibc with ThinLTO, then the checks after the link | `Tools/Scripts/check-classinfo-uniqueness.py`, `Tools/Scripts/aot/find-structures-decoded-behind-calls.py` | |

`Tools/Scripts/aot/check-includes.py` needs no whole build.

## Rules

- `aot/` is compiled in unified bundles. A `static` function, a type, or anything in an unnamed namespace needs a name that no other source of `aot/` uses.
- After you add a source, run `bun scripts/build/aot-source-lists.ts --webkit=<checkout>` in oven-sh/bun. It writes `Sources.txt` and the lists in `AOT.cmake`. `SourcesCompiledForSize.txt` and `SourcesCompiledForMinimumSize.txt` are kept by hand.
- Nothing that a generator writes is checked in.
- No static initializers.
- A change to the IR's schema changes both repositories. They land together.
- A switch over an enum of the compiler names every enumerator, with no `default`.
- Nothing compares the text of a name of the program with a literal. `name == "eval"` says yes to a local that somebody called `eval`, and no to `const e = eval`. Compare ids. `test/aot/compiler/names-compared-as-text.test.ts` in oven-sh/bun checks it.
- The walk never merges or removes a call of a host function or getter. `url.href` twice with nothing between stays two calls.
- A `Structure*` decoded from its id must not be the only thing that keeps a structure alive across a call that may collect.
- An operation that compiled code calls says in its declaration whether it may collect, throw or enter the VM.
- Files that upstream owns are not split or moved. It makes every later merge harder.
