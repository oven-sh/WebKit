# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is the Bun WebKit fork - a customized version of WebKit's JavaScriptCore engine optimized for Bun's runtime. The repository contains three core components:
- **JavaScriptCore (JSC)**: The JavaScript engine with multi-tier JIT compilation
- **WTF (Web Template Framework)**: Platform abstraction and utility library
- **bmalloc**: High-performance memory allocator

## Build Commands

### Quick Build (TypeScript)
```bash
# Debug build (recommended for development)
bun build.ts debug

# Release build
bun build.ts release

# Release with LTO (Link Time Optimization)
bun build.ts lto
```

### Platform-Specific Build Scripts
```bash
# macOS
bash mac-release.bash

# Linux (Docker-based)
bash release.sh

# Linux musl (Docker-based)
bash musl-release.sh

# Windows
./windows-release.ps1
```

### CMake Build (Advanced)
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

5. **Startup Snapshots**
   - `heap/Heap.cpp` (`freezeCurrentHeapAsImmortalStartupSnapshot`, `resetPacingAfterSnapshotRestore`), `runtime/VM.cpp` (`didRestoreFromStartupSnapshot`)
   - Freezes the live heap as immortal so an embedder can write the process's memory out and resume later launches from it

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
1. Use debug builds for development (`bun build.ts debug`)
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

GitHub Actions workflows (`.github/workflows/build.yml`) build for:
- **macOS**: x64/arm64, debug/release/ASAN builds
- **Linux**: x64/arm64, glibc/musl, debug/release/LTO/ASAN
- **Windows**: x64, debug/release

Artifacts are automatically published to GitHub releases as `autobuild-{sha}`.

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