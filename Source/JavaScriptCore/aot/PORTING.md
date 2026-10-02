# Porting the AOT compiler to another CPU

The back end targets ARM64 and runs on macOS and Linux. On every other target `ENABLE(AOT)` is off and `aot/` compiles to nothing.
This document lists the ARM64 assumptions, where they live, and a suggested order for an x86-64 port.

The counts below come from reading the source, not from building for another CPU. Treat them as lower bounds.

## Architecture

| Stage | Files | CPU-dependent? |
| --- | --- | --- |
| Bytecode to graph, whole-program analysis | `AOTGraph.*`, `AOTProgram.*`, `AOTEscapeAnalysis.*`, `AOTTypeInference.cpp` | No |
| Graph to B3 | `AOTLower*.cpp` | Patchpoints only |
| B3 to machine code | `AOTCompiler.cpp`, then stock B3 and Air | Slightly |
| Shared code: virtual calls, property access, entry and exit | `AOTStubs.*`, `AOTThunks.*` | **Almost entirely** |
| Image layout and relocation | `AOTImage.cpp`, helpers at the end of `AOTStubs.cpp` | The helpers |
| Runtime: instances, linking, inline caches, operations | `AOTRuntime.*`, `AOTOperations*.cpp`, `AOTInlineCaches.cpp` | No |
| Program data and the engine objects created from it | `AOTProgramData.*`, `runtime/CachedTypes.*` | No |
| Interpreter support | `llint/LowLevelInterpreter.asm` (`virtualThunkFor`) | No (offlineasm, already builds everywhere) |

## ARM64 assumptions

### 1. Build gates

- `ENABLE_AOT` in `wtf/PlatformEnable.h`.
- All nine `AOTLower*.cpp` files, and most of `AOTCompiler.cpp`, `AOTStubs.cpp` and `AOTThunks.cpp`, are wrapped in
  `ENABLE(AOT) && CPU(ARM64)`. **None of that code has been compiled for another CPU.**
- Without a back end, `compileForImage()` rejects every function. The shell then falls back to the interpreter, but an embedder's
  build fails because the executable contains no bytecode. This is the first milestone for a port.

### 2. Registers (the hard part)

`AOTConvention.h` defines the calling convention. Its x86-64 branch has never been used.

The stubs (top of `AOTStubs.cpp`) use 8 argument registers, `this`, the argument count, the callee, 5 scratch registers (`T11`-`T15`)
and 3 pinned registers (the instance and the two tag registers). That is 19 registers, not counting the frame pointer, stack pointer
and link register.

x86-64 has 16 registers. Excluding the stack pointer, frame pointer and the 3 pinned registers leaves 11. Using `GPRInfo`'s 6
argument registers plus `this`, count and callee leaves **2 scratch registers where the stubs need 5.**

Settle this first. `numberOfArgumentGPRs` does not have to equal `GPRInfo::numberOfArgumentRegisters`: with 4 argument registers
there are 4 scratch registers, and functions with more parameters use the existing `Signature::List` convention. (`EntryWord` has 4
bits for the count.) Callers and callees both derive the convention from bytecode via `conventionOf()`, so there is one place to change.

C++ operations take the instance as their first argument (a move from the pinned register). Operations shared with the JIT tiers
take the global object, loaded from the instance. `takesInstance(Entry)` derives which from the signature, and the compiler asserts
it at every call.

Also register-specific: `functionIndexGPR` in `AOTStubs.h`; the save/restore sequences around C++ calls (`AOTStubs.cpp`, around line
290: `x0`-`x15` in pairs, `d0`-`d7`, `d16`-`d31`); the stubs generated once per result register (`x19` and up); stores of the zero
register.

### 3. The return address is in a register

- A leaf function with no spills has no frame. `hasNoFrame()` in `AOTCompiler.cpp` detects calls by looking for patchpoints that
  clobber `lr`, which every call declares (`AOTLowerCalls.cpp`, `AOTLowerCore.cpp`).
- **Stubs do not push a frame.** A stub identifies its caller from `lr`: the function and the position within it are derived from the
  return address (`FunctionRef::at()`, `classifyAddress()`). On x86-64 `call` pushes the return address instead, so it is at the top of
  the stack and the stack is misaligned by 8 bytes inside a stub.
- Two sites materialize a return address that is not the next instruction (`adr lr, label` followed by a jump), and two compare return
  addresses against that label to decide whether to pop a frame. Search for `returnFromCallWithList()` and `s_labelAddresses`.

### 4. Fixed four-byte instructions

- Code is linked into a `Vector<uint32_t>` (`AOTCompiler.cpp`, `AOTStubs.cpp`).
- Removing a redundant leading jump shifts every offset by `sizeof(uint32_t)`.
- A near call is located as "end of instruction minus four bytes" (`StubCalls::link()`).
- Padding loops and veneer sizes are counted in instructions.

### 5. Relocations applied at image layout

Each is a small function at the end of `AOTStubs.cpp` that hits `RELEASE_ASSERT_NOT_REACHED()` on other CPUs, so together they form the
list of what to implement:

| Function | ARM64 encoding |
| --- | --- |
| `retargetStubCall()` | `bl` or `b` with a 26-bit offset |
| `writeVeneer()` | `adrp`, `add`, `br`, for direct calls whose target is out of range |
| `IndexReferences::load()` and `fill()` | `add` and `ldr` with 12-bit immediates, addressing a table entry by function index |
| `s_labelAddresses` fix-ups | `adr` |

An ARM64 branch reaches 128 MB, so an image holds up to 8 copies of the stubs (`maxStubCopiesPerImage`) and uses veneers between
functions. A 32-bit displacement reaches 2 GB, so x86-64 should need one copy and no veneers (see `reach` in `AOTImage.cpp`). The copies
are byte-identical, so a stub cannot tell which copy it is.

**Nothing is at a fixed address.** No PC-relative reference leaves the code, and neither the code nor the file contains an absolute
address; `compileForImage()` and `generateHelper()` check every constant. Compiled code reaches everything through the instance
register. Executables refer to their code by offset (`EntryWord`).

### 6. Instances

All mutable program state is reached through the instance register. There is one `AOT::Instance` per module loader, so a realm can run
the same program several times. No instance, realm, VM or thread is special, and they can be destroyed in any order.

- A table of pointers to module environments (ordinary cells) sits below the `Instance`. `ImageEnvironment::distance` is the offset of
  the pointer.
- A function's Structure records its instance (`Structure::m_aotInstance`). Null means it can run under any instance of its realm, which
  is the case for builtins.
- Three stubs depend on this: `findCalleeCode()` (fast path when the callee's instance is the caller's, or null in the same realm),
  `generateEnterStaticFunction()` (callee to Structure to instance, falling back to the realm's instance), and `adapt()`, which saves,
  switches and restores the register.
- Every realm has an instance from creation, because its builtins are compiled code.
- A function belongs to the instance of the module whose environment it closes over (`instanceOf()`), or to the realm's instance if it
  closes over none.
- An instance lives as long as its loader. Running code was entered through a frame that holds its callee, which keeps the loader alive,
  so **the entry adapter's frame must keep the callee where conservative stack scanning can find it.**

### 7. Ownership levels

| Level | Contents | Where |
| --- | --- | --- |
| Process | The file: code, tables and `ProgramData`. Derived only from the program, read-only, offsets and indices only | `Image`, `ProgramData` |
| VM | Engine objects for the file's contents, created lazily in the ordinary heap: atoms, string and BigInt constants, executables, symbol tables, array literal storage, regular expressions, source providers | `VMProgram` |
| Realm | Anything involving objects: Structures for literals and known shapes, link-time constants, assumptions about built-in prototypes | The realm's `Instance` |
| Instance | Module environments and records, functions, classes, template objects, inline caches | `Instance`, `Data` |

**A VM-level object may only reference primitives and other VM-level objects, never a `JSObject`** (asserted in
`VMProgram::constant()`), because it is shared by every realm and instance in the VM.

Compiled code fetches a constant by index through `Stub::Constant`, which probes a per-VM hash table and creates the constant on a miss.
Stubs read the per-VM identifier table directly and take their slow path when an entry is null. Neither can be resolved at link time,
because functions that are called directly or start cold are never linked. A module's top-level code runs once, so its constants are
created per use and are not cached (`Stub::TransientConstant`).

A module has no `CodeBlock` or `UnlinkedCodeBlock`. `ProgramModule` holds what the module loader needs, module code has the same
`FunctionMetadata` as a function, and `Interpreter::executeModuleProgram()` enters it through `Stub::EnterModule`. A `CodeBlock` is created
on demand for direct `eval`.

### 8. Miscellaneous

- A StructureID is converted to an address by adding `Instance::structureIDBase` (`structureWithID()` in `AOTStubs.cpp`). The stub that enters
  compiled code from outside has no instance yet and reads the same value from the VM, found through the callee's `MarkedBlock`.
- `aot/` uses 133 macro assembler methods (1,941 call sites). Four have no x86-64 or shared implementation:
  `extractUnsignedBitfield64` (6 uses), `div32` and `multiplySub32`, all in `AOTStubs.cpp`, and
  `convertDoubleToInt32UsingJavaScriptSemantics` in `AOTLowerCore.cpp`, which is already behind a CPU feature check.
- `imageStamp()` includes the CPU and already has an x86-64 case.
- Compiled code requires the JIT to be off (`Options::notifyOptionsChanged()`); an image is rejected otherwise.

## Platform (as opposed to CPU)

- **Mapping.** Both the shell and embedders use `useAOTFile()`. The embedder maps the file read-only at any address and passes the file and
  offset. Only the code is mapped a second time, as executable. There are no other platform requirements: no reserved address ranges, no
  allocator or WTF changes, and no per-thread or per-VM setup.
- **Page size.** An image is aligned to 16 KB within its file (`imagePageSize`, `pageSizeOfImage`), so kernels with larger pages cannot map
  the code.

## Suggested order

1. Enable the gate for the new CPU with no back end. Everything should compile and link, in the engine and the embedder.
   `compileForImage()` rejects every function, so `run-tests.py` runs interpreted and only the tests that call `isAOTCompiled()` fail.
2. Decide the register assignment (section 2).
3. Remove the file-level gates from `AOTLower*.cpp` and `AOTCompiler.cpp` and fix the build.
4. Entry adapter, prologue and epilogue. Target: a function that returns a constant, called from the interpreter.
5. Relocations (section 5), then direct calls between functions.
6. Stubs, in the order `JSTests/stress/aot-*.js` needs them. There are about 150 (`AOTStubs.h`); most are small and many share a generator.
7. Exceptions and stack walking: the `aot-*` tests that use `catch`, stack overflow and `Error.stack`.
8. `compare-with-interpreter.py` over `JSTests/stress`, then the `aot` and `aot-validate` modes of `run-javascriptcore-tests`, then `fuzz.py`.
9. The embedder's tests.

Steps 1-8 only need `jsc`, which builds an image and runs from it the same way an embedder does. The tools are in
`Tools/Scripts/aot/`; see the README there.

## Pitfalls

- In the shell, a function the compiler rejects is interpreted and the test still passes. Check that it was actually compiled.
- Be wary of anything that only usually holds, such as a mapping that tends to land nearby or a page size that happens to match. Make
  the rare case the common one in tests.
- A syntax-only check cannot catch link errors, and a build with a precompiled header cannot catch missing includes.
