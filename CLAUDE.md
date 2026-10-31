# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is the Bun WebKit fork - a customized version of WebKit's JavaScriptCore engine optimized for Bun's runtime. The repository contains three core components:
- **JavaScriptCore (JSC)**: The JavaScript engine with multi-tier JIT compilation
- **WTF (Web Template Framework)**: Platform abstraction and utility library
- **bmalloc**: High-performance memory allocator

## Build Commands

### Building a CI lane
```bash
# Every lane CI builds, and the toolchain images they start from
node .github/scripts/lanes.mjs

# Build one with CI's Dockerfile and settings, toolchain included (Docker on a Linux x64 host; everything but Linux x64
# is cross-compiled). CI itself starts from the prebuilt toolchain image: add --base-image <ref>, see CI/CD below.
node .github/scripts/lanes.mjs build bun-webkit-macos-arm64-lto --output /tmp/bun-webkit
```

There are deliberately no other build scripts: `lanes.mjs` and the Dockerfiles are the only place the flags of a shipped build are written down. Which ICU is built and bundled is `icu/source.json` (version and sha256) and nowhere else: `lanes.mjs` passes it to the Dockerfiles, so bumping ICU is a change to that one file. `build-icu.ps1` is not part of CI; Bun runs it when it builds WebKit from source on Windows (`scripts/build/deps/webkit.ts` in oven-sh/bun). It builds the same ICU (same `icu/source.json`, clang on both architectures) with ICU's MSBuild projects, because ICU's configure/make, which CI uses on Linux, does not run on Windows; its header lists the two ways its output differs from what ships.

### Building `jsc` for development (CMake)
```bash
# Configure
cmake -G Ninja \
  -DPORT=JSCOnly \
  -DENABLE_STATIC_JSC=ON \
  -DUSE_BUN_JSC_ADDITIONS=ON \
  -DUSE_BUN_EVENT_LOOP=ON \
  -DENABLE_FTL_JIT=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  /path/to/webkit

# Build
cmake --build . --target jsc
```

`node` (24.2 or later, or 22.18 or later of 22: it runs TypeScript as it is) has to be on `PATH`. Nothing that a generator of `Source/JavaScriptCore/aot` writes is checked in. While CMake configures, `aot/ir/generator/index.ts` writes the C++ of the compiler's IR from `aot/ir/AOTSchema.def`, and `aot/builtins/generator/engine.ts` writes what the engine needs of the table of built-ins from `aot/builtins/*.builtins.ts`, plus the whole table of `jsc` and `testaot`: the engine's entries and a stand-in host (`aot/tests/host`). `aot/image/generator/runtime-facts.ts` is a step of the build. All three write into `<build>/JavaScriptCore/DerivedSources/aot`. `aot/ir/AOTSchemaFiles.cmake` and `aot/builtins/AOTBuiltinTable.cmake` list what the first two read and write; people keep them and the generators name what is missing. Bun's build runs the same scripts. Every lane's tarball carries `aot/ir/generator` with the schema, from which Bun generates the Rust half, and `aot/builtins/generator` with the five files the engine was compiled with (`aot/builtins/generated`), from which Bun generates and compiles its own table: the engine's entries and Bun's. An `.ir` test here passes with the stand-in host's table. One that names an entry, a class, a module or a function number of Bun's table lies in Bun's repository (`test/aot/engine-tests`), where the pull request that changes the entry can change the test. The native tests that need Bun's entries (`AOTNativeTestsOfHostEntries.inc`) are left out of the `testaot` that CMake links: it prints `LEFT OUT` and the name for each, its last line ends with how many, and the `test` job fails when that is not the number written in `ci.yml`. Bun's CI runs them. No object of `testaot` or `jsc` holds anything of one table, so CMake also archives them without a table, `lib/libtestaot-objects.a` and `lib/libjsc-objects.a` (`shell/CMakeLists.txt`), and Bun's build links those with Bun's table. A test must not include `AOTBuiltinTableArrays.h`, the table's own header: `AOTHostEntriesTestSupport.h` finds what has no constant in the table the program is linked with.

### Key Build Flags
- `USE_BUN_JSC_ADDITIONS=ON`: Enable Bun-specific features
- `USE_BUN_EVENT_LOOP=ON`: Use Bun's event loop implementation
- `ENABLE_FTL_JIT=ON`: Enable the FTL (Faster Than Light) JIT tier
- `ENABLE_STATIC_JSC=ON`: Build static libraries instead of shared
- `ENABLE_SANITIZERS=address`: Enable AddressSanitizer for debugging
- `ENABLE_FUZZILLI=ON`: Compile JavaScriptCore with `-fsanitize-coverage=trace-pc-guard` and give `jsc` the coverage callbacks and `--reprl`

## High-Level Architecture

### JavaScriptCore (Source/JavaScriptCore)

#### Execution Tiers
JSC uses a 4-tier JIT compilation strategy:

1. **LLInt** (`llint/`): Low-level interpreter written in assembly
   - First execution tier for all code
   - Collects profiling data for optimization decisions

2. **Baseline JIT** (`jit/`): Template-based JIT compiler
   - Quick compilation with inline caching
   - Moderate optimizations

3. **DFG** (`dfg/`): Data Flow Graph optimizer
   - SSA-based intermediate representation
   - Type speculation and profiling-guided optimizations

4. **FTL** (`ftl/`): Highest optimization tier
   - Uses B3 backend (`b3/`) for advanced optimizations
   - LLVM-level optimization capabilities

#### Key Components

- **Runtime** (`runtime/`): Core VM, object model, and built-in types
  - `VM.h/cpp`: Central virtual machine orchestrator
  - `JSGlobalObject.h/cpp`: Global JavaScript environment
  - `JSValue.h`: Value representation system

- **Parser** (`parser/`): JavaScript parsing and AST generation
  - Recursive descent parser with semantic analysis
  - Module dependency resolution

- **Bytecode** (`bytecode/`, `bytecompiler/`): Bytecode generation and management
  - `BytecodeGenerator`: AST to bytecode compiler
  - Profiling metadata and optimization hints

- **Heap** (`heap/`): Garbage collection and memory management
  - Generational GC with incremental marking
  - IsoSubspace for type isolation

- **API** (`API/`): External interfaces
  - C API: Traditional JSContextRef/JSValueRef interface
  - Objective-C API: Higher-level wrappers

### WTF (Source/WTF)

Platform abstraction layer providing:
- **Threading**: Cross-platform thread management and synchronization
- **Memory**: Smart pointers (RefPtr, UniquePtr) and containers
- **Text**: String handling with AtomString optimizations
- **Utilities**: Assertions, logging, time handling

Key files:
- `wtf/Platform.h`: Platform detection and configuration
- `wtf/FastMalloc.h/cpp`: Performance-optimized memory allocation
- `wtf/text/AtomString.h`: Interned string implementation
- `wtf/RunLoop.h`: Event loop abstraction

### bmalloc (Source/bmalloc)

High-performance memory allocator with:
- **IsoHeap**: Type-segregated heaps for security and performance
- **Gigacage**: Security boundaries for typed arrays
- **Scavenger**: Periodic memory decommit
- **libpas**: Physical Address Space management

## Bun-Specific Modifications

### USE_BUN_JSC_ADDITIONS Features

1. **V8 Heap Snapshot Support**
   - `heap/BunV8HeapSnapshotBuilder.h/cpp`
   - Generates V8-compatible heap snapshots for debugging tools

2. **AsyncLocalStorage**
   - `runtime/InternalFieldTuple.h`
   - Node.js-compatible async context tracking

3. **Inspector Extensions**
   - `inspector/protocol/BunFrontendDevServer.json`
   - Custom dev server domain for HMR and bundling

4. **Enhanced Error Handling**
   - Stack trace improvements
   - Better error reporting for development

### USE_BUN_EVENT_LOOP
Custom event loop implementation for Bun's runtime requirements

## Testing

### Run JSC Shell
```bash
# After building
./WebKitBuild/Debug/bin/jsc [script.js]
./WebKitBuild/Release/bin/jsc [script.js]
```

### Run Tests
```bash
# C++ tests
./WebKitBuild/Debug/bin/testmasm
./WebKitBuild/Debug/bin/testb3

# JavaScript tests (from JSTests directory)
./Tools/Scripts/run-javascriptcore-tests
```

### When a test fails, and after an upstream sync
`JSTests/`, `LayoutTests/js` and the PerformanceTests collections are upstream's tests, and this fork has changed 129 of them. `JSTests/BUN-TEST-DIFFERENCES.md` lists every one by reason: the behaviors the fork has on purpose (with the fork code behind each and the files whose expectations follow it), the tests narrowed to the modes or builds they can pass in, and the ones skipped with `TODO(bun)` for a known gap. Read it before changing a test's expectation or skipping one: a failure that matches an entry there means the test is what to change; one that matches none is a regression in the engine. Its last section is the procedure for sorting a new failure. When a test is changed, add it to that file, and put a `// Bun:` or `// TODO(bun):` comment on the changed line saying what upstream expects. `JSTests/CLAUDE.md` and `JSTests/README.md` are upstream's files: leave them as they are.

## Development Tips

### Important Directories
- **For JavaScript execution**: Start with `runtime/`, `interpreter/`, `jit/`
- **For memory/GC work**: Focus on `heap/`, `bmalloc/`
- **For optimizations**: Look at `dfg/`, `ftl/`, `b3/`
- **For API changes**: Check `API/` and bindings
- **For platform code**: See `wtf/` and platform-specific subdirectories

### Debugging
1. Use a debug build for development (`-DCMAKE_BUILD_TYPE=Debug`, see the CMake commands above)
2. Enable sanitizers for memory debugging: `ENABLE_SANITIZERS=address`
3. Use `dataLog()` for printf-style debugging in JSC code
4. Set breakpoints in tier transitions: `DFG::Plan::compileInThread`, `FTL::compile`

### Common Modifications
- **Adding opcodes**: Edit `bytecode/BytecodeList.rb`, regenerate with build
- **Runtime functions**: Add to appropriate `runtime/*` files
- **JIT optimizations**: Modify relevant tier in `jit/`, `dfg/`, or `ftl/`
- **Heap/GC changes**: Update `heap/` components

### Build Optimization
- Use `ninja` for faster incremental builds
- `ccache` can significantly speed up rebuilds
- For quick iterations, build only `jsc` target instead of full WebKit

## CI/CD

`.github/workflows/ci.yml` is the one build workflow, and a push to `main` and a pull request from a branch of this repository run exactly the same jobs. (A pull request from a fork runs none of them: its token cannot publish, so `plan` is skipped and everything else with it. Push the branch to this repository to build it.) They differ only in what the result is called: `main` publishes the release `autobuild-{sha}`, a pull request publishes the prerelease `autobuild-preview-pr-{n}-{sha8}` (built from the pull request's head) and links it from a comment on the pull request. Running the workflow by hand on a branch builds that branch's head as a prerelease `autobuild-{sha}`. Either way it builds every lane and publishes them as one GitHub release. A lane is one `<label>.tar.gz`, e.g. `bun-webkit-linux-amd64-lto`: Linux glibc/musl, macOS, Windows, FreeBSD and Android, x64/arm64, release/LTO/debug/ASAN, and for Linux glibc x64 three more for fuzzing (`-assertions-aot`, `-coverage-aot`, `-assertions-coverage-aot`: the `-aot` lane with `ENABLE_ASSERTS=ON`, `ENABLE_FUZZILLI=ON` or both, see `variants` in `lanes.mjs`) and `-hardened-aot` and `-lto-hardened` (the `-aot` and LTO lanes with ICU and WebKit compiled with `HARDENING` in `lanes.mjs`: stack protector, `_FORTIFY_SOURCE`, stack clash protection, zeroed locals; on arm64 also without `-mtune`, with which LLVM 23 does not inline glibc's `_FORTIFY_SOURCE` wrappers, and in arm64's `-lto-hardened` with `SIGNED_RETURN_ADDRESSES`, `-mbranch-protection=pac-ret`: not yet where the lane has the compiler, whose run time reads the return address in the frame of an operation as it is. That flag is for what the compiler generates. `SIGNED_RETURN_ADDRESSES` is also `ENABLE(RETURN_ADDRESS_SIGNING)`, with which LLInt and what the JITs generate sign too, at the places where they do for `CPU(ARM64E)`.). The release, `-lto` and `-lto-hardened` lanes are compiled with `ENABLE_AOT=0`: their libraries hold neither the ahead-of-time compiler nor the run time of its programs, and a `bun` built with `--aot=off` links them. Each has a twin with both, `-aot`, `-lto-aot` and `-lto-hardened-aot`, which is what `bun` links. Every other lane has both and no twin without (`aot` in `variants`): `-debug-aot`, `-asan-aot`, `-debug-asan-aot`, the three for fuzzing and `-hardened-aot`. A label says which a lane is: it has `-aot` in it where the lane has the compiler, and only there, and `lanes.mjs` throws where the two disagree. The lanes without have the labels they had before there was a compiler. `ENABLE_AOT` changes the layout of classes in the headers, so the pack step ends every tarball's `include/cmakeconfig.h` with `BUN_WEBKIT_ENABLE_AOT` (0 or 1) and with the same value for `ENABLE_AOT` where the user of the libraries has not defined it. Every lane builds on a Linux x64 runner, in a linux/amd64 Docker container, and everything that is not Linux x64 is cross-compiled there with `clang --target` and a sysroot: macOS, Windows, FreeBSD, Android, and Linux arm64 (glibc against an ubuntu 20.04 arm64 sysroot, so glibc 2.31 like the x64 container itself; musl against an alpine aarch64 sysroot). The glibc and musl versions a lane is built against are checked when the toolchain image is built, and every glibc `jsc` that is linked is checked not to need symbols newer than `GLIBC_2.31`.

The lanes are defined in `.github/scripts/lanes.mjs` and nowhere else: label, runner, Dockerfile, every build argument passed to it, and whether the lane is tested. `node .github/scripts/lanes.mjs` lists them (`--json` for the full settings), and `lanes.mjs build <label> --output <dir>` builds one; that command is also what CI runs, so a lane can be reproduced exactly. To add, remove or change a lane, edit `platforms` there. The workflow's `plan` job names the release and turns that table into the matrices of `image`, `build` (lanes that are only built), `build-tested` (lanes that are also tested; same steps) and `test`, and into the list of assets the `release` job requires.

Each lane is built by a Dockerfile (`Dockerfile`, `Dockerfile.musl`, `Dockerfile.macos`, `Dockerfile.windows`, `Dockerfile.freebsd`, `Dockerfile.android`). In every one of them the `base` stage is the toolchain and nothing else (compilers, SDKs, sysroots): it must not take any lane setting, so that it is identical for every lane of a platform and architecture; lane settings go in the stages on top (`lane`, `build_icu`, `build_webkit`). Everything a lane needs from the network is in `base` too (packages, SDKs, sysroots, ICU's sources and host tools, `mig`, zstd, node): the stages on top must not download or install anything, so a lane's only network use is pulling its image. CI keeps those `base` stages as images in `ghcr.io/oven-sh/bun-webkit-build-env`, tagged `<toolchain>-<hash>`, the hash being of the Dockerfile up to the end of `base` (less comments and lane-only ARGs), the files `base` copies in and the build arguments it takes. The `image` job builds and pushes the ones whose tag is missing (so only after a change to a `base` stage), failing if it cannot, and each lane starts from its image (`lanes.mjs build --base-image`, which is `--build-context base=docker-image://...`). Without `--base-image` the toolchain is built as part of the build. Old images are not kept around: a build of `main` runs `prune`, which deletes every image that `main`'s lanes do not use, that is more than 14 days old and that is not one of the two newest of its toolchain: images a pull request pushed recently survive while a toolchain change is being worked on, and so does the image `main` used until the last change.

The release starts as a draft, each lane uploads its tarball onto it, and the `release` job publishes it once every lane succeeded. When lanes failed, a pull request's is published with the lanes that were built, and a branch's stays a draft: "Re-run failed jobs" builds those lanes onto it and publishes it. When the run was cancelled the draft is deleted: "Re-run all jobs". It has one more asset, `bun-webkit-aot-sources.tar.gz` (`SOURCES` in `lanes.mjs`): what a tarball has under `bun-webkit/Source/JavaScriptCore/aot` but the tests, at the same paths, 0.5 MB. The `linux-amd64-lto-aot` lane packs it out of its own output. Bun's jobs that link no engine (lint, type checks) read it in place of a lane's tarball.

Behind the link, a lane checks the `jsc` it has made: `Tools/Scripts/check-classinfo-uniqueness.py`, in the lanes with `SIGNED_RETURN_ADDRESSES` `Tools/Scripts/check-signed-return-addresses.py` (every function that saves the link register signs it, but for the few that the script lists by name: what is reached with the link register signed, one piece of inline assembly and what comes with the toolchain; and every pointer authentication instruction is one that ARMv8.0 ignores), and in the optimized lanes (not on Windows, and not with assertions or coverage) `Tools/Scripts/aot/find-structures-decoded-behind-calls.py`, which looks for a `Structure*` that the compiler made from its `StructureID` behind a call that may collect. It fails the Linux glibc x64 lanes. In the other lanes it prints its rows and "not yet fatal here": the list of expected rows was made on Linux x64, and other processors and compilers give other rows. A row that is harmless goes on `structures-decoded-behind-calls-expected.txt` with its reason. Every tarball carries that script and its two lists under `bun-webkit/Tools/Scripts/aot`, for Bun's CI to run on the `bun` it links.

Every Bun developer and every Bun CI job downloads a tarball, so its size matters; the pack step prints the size of `lib/`, of each program and of the tarball. Most of a tarball is `libJavaScriptCore.a`, and outside the `lto` lanes most of that is debug info, which every object repeats for the headers it includes. Three things keep it down. `aot/` is compiled as unified sources like the rest of JavaScriptCore, by WebKit's own `WEBKIT_COMPUTE_SOURCES` (`AOT_COMPUTE_SOURCES` in `aot/AOT.cmake`): 16 sources in a bundle here, 8 in Bun's own build. No source is left out: whatever a source declares for itself at namespace scope has a name that nothing else in `aot/` has, and no source has a `using namespace` at namespace scope. `Tools/Scripts/aot/check-unified-sources.py` checks that in five seconds, without a compiler. The sources in `aot/SourcesCompiledForSize.txt` have bundles of their own, because a compile option belongs to the bundle. The macOS and FreeBSD lanes pass `-fno-standalone-debug`, clang's default on Linux. Every lane links `jsc`, `testFFI` and `testaot`, each a full copy of JavaScriptCore: they ship without debug info (no `.pdb` on Windows), and a lane only ships the ones something downloads it for, and the two archives of objects only where Bun links them (`programs` in `lanes.mjs`).

The `test` job runs `Tools/Scripts/run-javascriptcore-tests` (JSTests, LayoutTests/js, the PerformanceTests collections, `testaot`, `testb3`, `testair`), `testFFI` and `TestWTF` against the `bin/jsc` built by the lanes marked `tested` in `lanes.mjs`, as soon as those lanes are built, each on a runner of its own platform and architecture: the Linux x64 and arm64 `-lto-aot` lanes, which are what `bun` ships there (about 23 minutes each), their `-lto` twins, which are the same without the compiler, the `-lto-hardened-aot` lanes, and arm64's `-lto-hardened`, the lane that signs return addresses and the run of `HARDENING` without the compiler (the job says whether its runner has pointer authentication: without it nothing is signed, and the tests of what reads a signed return address pass whatever the code does). A lane built with `ENABLE_AOT=0` ships no `testaot`: the job runs it with `--without-aot`, which leaves `testaot` out and skips the tests marked `//@ skip if !$aot`. Nothing else is tested. `testb3`, `testair` and `TestWTF` are built by the tested lanes that have the compiler only (`testPrograms` in `lanes.mjs`, `TEST_PROGRAMS` in `Dockerfile`), go to the job with the shell and are not in the tarball. The asan lanes are built and published but not tested: the tests take 65 to 100 minutes against them on Linux and macOS and do not finish in 275 on Windows; no tested lane has assertions on. The macOS and Windows lanes are not tested either. The job can run them, on GitHub's standard runners (3 or 4 cores, so with `--quick`, which also skips the `//@ slow!` tests; on Windows with Windows' own perl and `--test-writer=ruby --ruby-runner`, as upstream's Windows bots did): the comment at the top of `lanes.mjs` has the `tested` entries. macOS took 16 minutes; Windows x64 took 145, because `--ruby-runner` runs the tests one at a time. It runs every test before failing (`--no-fail-fast`) and turns the run red when any fail, but does not gate the release, which is still published; the step's own log shows each `FAIL:` as it happens, a progress line and the runner's results (the full output is over 100,000 lines, more than the log viewer shows); the failing tests are listed in the job summary (whether a failing test is one to change is what `JSTests/BUN-TEST-DIFFERENCES.md` is for), and the workflow artifact has `jsc-failures.log` (what each failing run printed, with its repro command, grouped under its name), the full `jsc-tests.log` and the results JSON. To run them against an asan build locally (`--no-slow`: the tests marked `//@ slow!` pass the 300 second hard timeout under ASan; `--asan` sets `$asan` in `run-jsc-stress-tests`, for `//@ skip if $asan`): extract `bun-webkit/bin` from the asan tarball and run `ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1:allow_user_segv_handler=1 JSCTEST_memoryLimit=4294967296 Tools/Scripts/run-javascriptcore-tests --no-build --root=<path>/bun-webkit --release --jsc-only --no-testmasm --no-testair --no-testb3 --no-testdfg --no-testapi --no-testwasmdebugger --no-testlibjsctools --no-fail-fast --memory-limited --no-slow --asan` (`--quick` instead of `--no-slow` for what the macOS and Windows legs run; add `--filter <regex>` for a subset).

## Architecture Notes

### Memory Safety
- IsoHeaps provide type segregation for security
- Gigacage prevents out-of-bounds access in typed arrays
- Conservative stack scanning ensures C++ integration safety

### Performance Considerations
- LLInt provides fast startup
- Baseline JIT balances compilation time vs execution speed  
- DFG/FTL optimize hot code paths
- Inline caches accelerate property access
- Polymorphic inline caches handle multiple types efficiently

### Threading Model
- Main thread runs JavaScript execution
- Compiler threads handle JIT compilation
- Marking threads assist with garbage collection
- DFG/FTL compilation happens off the main thread