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

### Key Build Flags
- `USE_BUN_JSC_ADDITIONS=ON`: Enable Bun-specific features
- `USE_BUN_EVENT_LOOP=ON`: Use Bun's event loop implementation
- `ENABLE_FTL_JIT=ON`: Enable the FTL (Faster Than Light) JIT tier
- `ENABLE_STATIC_JSC=ON`: Build static libraries instead of shared
- `ENABLE_SANITIZERS=address`: Enable AddressSanitizer for debugging

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

`.github/workflows/ci.yml` is the one build workflow, and a push to `main` and a pull request from a branch of this repository run exactly the same jobs. (A pull request from a fork runs none of them: its token cannot publish, so `plan` is skipped and everything else with it. Push the branch to this repository to build it.) They differ only in what the result is called: `main` publishes the release `autobuild-{sha}`, a pull request publishes the prerelease `autobuild-preview-pr-{n}-{sha8}` (built from the pull request's head) and links it from a comment on the pull request. Running the workflow by hand on a branch builds that branch's head as a prerelease `autobuild-{sha}`. Either way it builds every lane and publishes them as one GitHub release. A lane is one `<label>.tar.gz`, e.g. `bun-webkit-linux-amd64-lto`: Linux glibc/musl, macOS, Windows, FreeBSD and Android, x64/arm64, release/LTO/debug/ASAN. Every lane builds on a Linux x64 runner, in a linux/amd64 Docker container, and everything that is not Linux x64 is cross-compiled there with `clang --target` and a sysroot: macOS, Windows, FreeBSD, Android, and Linux arm64 (glibc against an ubuntu 20.04 arm64 sysroot, so glibc 2.31 like the x64 container itself; musl against an alpine aarch64 sysroot). The glibc and musl versions a lane is built against are checked when the toolchain image is built, and every glibc `jsc` that is linked is checked not to need symbols newer than `GLIBC_2.31`.

The lanes are defined in `.github/scripts/lanes.mjs` and nowhere else: label, runner, Dockerfile, every build argument passed to it, and whether the lane is tested. `node .github/scripts/lanes.mjs` lists them (`--json` for the full settings), and `lanes.mjs build <label> --output <dir>` builds one; that command is also what CI runs, so a lane can be reproduced exactly. To add, remove or change a lane, edit `platforms` there. The workflow's `plan` job names the release and turns that table into the matrices of `image`, `build` (lanes that are only built), `build-tested` (lanes that are also tested; same steps) and `test`, and into the list of assets the `release` job requires.

Each lane is built by a Dockerfile (`Dockerfile`, `Dockerfile.musl`, `Dockerfile.macos`, `Dockerfile.windows`, `Dockerfile.freebsd`, `Dockerfile.android`). In every one of them the `base` stage is the toolchain and nothing else (compilers, SDKs, sysroots): it must not take any lane setting, so that it is identical for every lane of a platform and architecture; lane settings go in the stages on top (`lane`, `build_icu`, `build_webkit`). Everything a lane needs from the network is in `base` too (packages, SDKs, sysroots, ICU's sources and host tools, `mig`, zstd, node): the stages on top must not download or install anything, so a lane's only network use is pulling its image. CI keeps those `base` stages as images in `ghcr.io/oven-sh/bun-webkit-build-env`, tagged `<toolchain>-<hash>`, the hash being of the Dockerfile up to the end of `base` (less comments and lane-only ARGs), the files `base` copies in and the build arguments it takes. The `image` job builds and pushes the ones whose tag is missing (so only after a change to a `base` stage), failing if it cannot, and each lane starts from its image (`lanes.mjs build --base-image`, which is `--build-context base=docker-image://...`). Without `--base-image` the toolchain is built as part of the build. Old images are not kept around: a build of `main` runs `prune`, which deletes every image that `main`'s lanes do not use, that is more than 14 days old and that is not one of the two newest of its toolchain: images a pull request pushed recently survive while a toolchain change is being worked on, and so does the image `main` used until the last change.

The release starts as a draft, each lane uploads its tarball onto it, and the `release` job publishes it once every lane succeeded (or deletes the draft when one failed).

The `test` job runs `Tools/Scripts/run-javascriptcore-tests` (JSTests, LayoutTests/js, the PerformanceTests collections) and `testFFI` against the `bin/jsc` built by the lanes marked `tested` in `lanes.mjs`, as soon as those lanes are built, each on a runner of its own platform and architecture: Linux x64 and arm64, macOS arm64 and Windows x64 (the asan lane and the lto lane, which is the build Bun ships), and Windows arm64 (release; it has no asan or lto lane, and release is what ships there). The Linux asan lanes pass `--no-slow`, so the tests marked `//@ slow!` do not run there (they pass the 300 second hard timeout under ASan); the lto lanes run them. The macOS and Windows lanes are tested on GitHub's standard runners (3 or 4 cores), so with `--quick`, which also skips the `//@ slow!` tests; on Windows the runner is driven as upstream's Windows bots drive it (`--test-writer=ruby --ruby-runner`, Windows' own perl). It runs every test before failing (`--no-fail-fast`) and turns the run red when any fail, but does not gate the release, which is still published; the step's own log shows each `FAIL:` as it happens, a progress line and the runner's results (the full output is over 100,000 lines, more than the log viewer shows); the failing tests are listed in the job summary, and the workflow artifact has `jsc-failures.log` (what each failing run printed, with its repro command, grouped under its name), the full `jsc-tests.log` and the results JSON. To reproduce locally: extract `bun-webkit/bin` from the asan tarball and run `ASAN_OPTIONS=detect_leaks=0:allocator_may_return_null=1 JSCTEST_memoryLimit=4294967296 Tools/Scripts/run-javascriptcore-tests --no-build --root=<path>/bun-webkit --release --jsc-only --no-testmasm --no-testair --no-testb3 --no-testdfg --no-testapi --no-testwasmdebugger --no-fail-fast --memory-limited --no-slow` (`--quick` instead of `--no-slow` for what the macOS and Windows legs run; add `--filter <regex>` for a subset).

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