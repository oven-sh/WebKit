# Porting the AOT compiler to another CPU

There are two back ends, ARM64 and x86-64. ARM64 runs on macOS and Linux. x86-64 has only been run on macOS under Rosetta. On every
other target `ENABLE(AOT)` is off and `aot/` compiles to nothing.

This document says what depends on the CPU, how the two back ends differ, and how to add a third.

## Architecture

| Stage | Files | CPU-dependent? |
| --- | --- | --- |
| Bytecode to graph, whole-program analysis | `AOTGraph.*`, `AOTProgram.*`, `AOTEscapeAnalysis.*`, `AOTTypeInference.cpp` | No |
| Graph to B3 | `AOTLower*.cpp` | Which registers patchpoints name |
| B3 to machine code | `AOTCompiler.cpp`, then stock B3 and Air | The prologue |
| Structural stubs: calls, entries, exceptions, constants, adapters around operations | `AOTStubs.*` | Shared generators with a few conditionals |
| Data stubs: property access, arithmetic, comparison, iteration, allocation | `AOTStubs.*`, `AOTThunks.*` | **ARM64 only, and optional** |
| Image layout and relocation | `AOTImage.cpp`, helpers at the end of `AOTStubs.cpp` | The helpers |
| Runtime: instances, linking, inline caches, operations | `AOTRuntime.*`, `AOTOperations*.cpp`, `AOTInlineCaches.cpp` | No |
| Program data and the engine objects created from it | `AOTProgramData.*`, `runtime/CachedTypes.*` | No |
| Interpreter support | `llint/LowLevelInterpreter.asm` (`virtualThunkFor`) | No (offlineasm) |

## What depends on the CPU

### 1. Data stubs are optional

`usesStubs` says whether an image contains shared stubs at all. `usesDataStubs()` says whether property access, arithmetic and the like
go through them. Without data stubs `Lowering::isCompact()` is false everywhere: those operations are lowered to inline fast paths in B3
with plain calls of the operations as slow paths, which is portable. Allocation helpers, stub intrinsics, per-register and per-immediate
entry points (`thunkFor()`) and the front ends in `AOTThunks.cpp` are all off.

About 100 of the 150 stubs are data stubs. **A port does not need them.** x86-64 has none: `FOR_EACH_AOT_STUB_WITHOUT_DATA_STUBS` in
`AOTStubs.cpp` lists the 42 stubs it generates, and every other stub is a breakpoint there.

`--useAOTDataStubs=0` selects the same lowering on ARM64, and `run-tests.py` runs every test in that mode too. So most of what another
CPU compiles is tested on ARM64, and a failure that also happens there is not in the port. The mode is part of `imageStamp()`, because
the formats of the inline caches depend on it.

What this costs is code size, and speed outside loops where a data stub has a fast path that the inline lowering lacks (comparison with
short string literals, allocation helpers, calls of common builtin methods).

### 2. Registers

`AOTConvention.h` defines the calling convention and the registers that stubs may use.

| Role | ARM64 | x86-64 |
| --- | --- | --- |
| Arguments | `x0`-`x7` | `rdi`, `rsi`, `rdx`, `rcx` |
| `this` | `x8` | `rax` |
| Argument count, and the immediate of a stub (`T9`) | `x9` | `r10` |
| Callee (`T10`) | `x10` | `r8` |
| `T11` | `x11` | `r9` |
| `T12`, `T13` | `x12`, `x13` | `rbx`, `r12` |
| `T14`, `T15` | `x14`, `x15` | none (only data stubs use them) |
| Instance, number tag, not-cell mask | as the JIT tiers | `r13`, `r14`, `r15` |

`numberOfArgumentGPRs` need not equal `GPRInfo::numberOfArgumentRegisters`. Functions with more parameters use `Signature::List`.
Callers and callees both derive the convention from bytecode via `conventionOf()`.

Where a temporary is callee-saved in the C ABI, as `rbx` and `r12` are:

- Calls clobber it (`Lowering::registersClobberedByCalls()`).
- **A function that ends in a tail call has restored it before the call stub uses it.** So compiled code as a whole does not preserve
  it, and `adapt()`, through which the engine enters compiled code, saves and restores it. `adapterSavedRegisters()` tells the unwinder.
- **A stub that the engine enters must not use it before `adapt()` has saved it.** Those stubs use `entryT12` and `entryT13`, which on
  x86-64 are argument registers: at such an entry the arguments are on the stack.

The adapters around operations must not use a C argument register as a temporary (`operationGPR`). Operations take up to eight integer
arguments; where the C ABI has fewer registers, `operationArgumentGPR()` names registers for the rest and `callAndCheckException()` pushes
them.

### 3. Where the return address is

On ARM64 it is in `lr`; on x86-64 `call` pushes it.

- Wherever a stub begins with `emitFunctionPrologue()` the two are the same.
- `outgoingFrameSlot()` addresses a slot of the frame being made before the call, `incomingFrameSlot()` at the entry of the callee. They
  differ on x86-64.
- `callPreservingRegistersAndReturn()` gives the return address to its callback in `T11`.
- The stack check of a prologue is a stub on ARM64. On x86-64 it is inline (`AOTCompiler.cpp`): a stub that moves the stack pointer
  has to return with a jump, which unbalances the CPU's prediction of returns.
- A leaf function with no spills has no frame on ARM64 (`hasNoFrame()`, which detects calls by patchpoints that clobber `lr`). Every
  function has a frame on x86-64.
- Frames made for calls with an argument list are recognized by their return address, `returnFromCallWithList()`, so that tail calls can
  reuse them. ARM64 sets `lr` to it and jumps. x86-64 makes all such calls with one call instruction (`callTargetWithList()`), or
  pushes the address. `loadLabelAddress()` is `adr`, or `lea` relative to `rip`.

### 4. Instruction size

`sizeOfNearCall` and `codeOffsetUnit` in `AOTStubs.h`. `StubCall::offset` is where the call instruction starts. Trailing padding is only
trimmed on ARM64, where it cannot be mistaken for the end of an instruction.

### 5. Relocations applied at image layout

Each is a small function at the end of `AOTStubs.cpp` that hits `RELEASE_ASSERT_NOT_REACHED()` on other CPUs:

| Function | ARM64 | x86-64 |
| --- | --- | --- |
| `retargetStubCall()` | `bl` or `b` with a 26-bit offset | `call` or `jmp` with a 32-bit offset |
| `writeVeneer()` | `adrp`, `add`, `br` | `jmp` |
| `IndexReferences::load()` and `fill()` | `add` and `ldr` with 12-bit immediates | a load with a 32-bit displacement |

An ARM64 branch reaches 128 MB, so an image holds up to 8 copies of the stubs (`maxStubCopiesPerImage`) and uses veneers between
functions (`stubCallReach`). x86-64 needs one copy and no veneers, but the options that force them in tests work there too. The copies
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
- NaNs differ. Arithmetic on x86-64 makes NaNs with the sign bit set, so nothing may compare NaNs by their bits.
- `imageStamp()` includes the CPU.
- Compiled code requires the JIT to be off (`Options::notifyOptionsChanged()`); an image is rejected otherwise.

## Platform (as opposed to CPU)

- **Mapping.** Both the shell and embedders use `useAOTFile()`. The embedder maps the file read-only at any address and passes the file and
  offset. Only the code is mapped a second time, as executable. There are no other platform requirements: no reserved address ranges, no
  allocator or WTF changes, and no per-thread or per-VM setup.
- **Page size.** An image is aligned to 16 KB within its file (`imagePageSize`, `pageSizeOfImage`), so kernels with larger pages cannot map
  the code.

## Suggested order

The x86-64 port took these steps, and about 520 lines.

1. Enable the gate for the new CPU with no back end. Everything should compile and link. `compileForImage()` rejects every function, so
   `run-tests.py` runs interpreted and only the tests that require compiled code fail.
2. Decide the register assignment (section 2).
3. Open the gates of `AOTLower*.cpp`, `AOTCompiler.cpp`, the structural part of `AOTStubs.cpp` and `installImageCompiler()`, and fix the build.
4. Relocations (section 5).
5. `run-tests.py`.
6. `JSTests/stress` compiled ahead of time. Run what fails on ARM64 as well, with and without data stubs. What fails there too is not in
   the port: much of it is a difference that compiled code has on purpose.
7. The `aot` and `aot-validate` modes of `run-javascriptcore-tests`, then `fuzz.py`.
8. The embedder's tests. An image is built by the CPU it is for: B3 targets its host.
9. Data stubs, by what a profile says.

Steps 1-7 only need `jsc`, which builds an image and runs from it the same way an embedder does. The tools are in
`Tools/Scripts/aot/`; see the README there.

## Pitfalls

- In the shell, a function the compiler rejects is interpreted and the test still passes. Check that it was actually compiled.
- A path that only one configuration takes rots. The lowering without data stubs had an inline cache that did not know a format added
  later for the stubs. Every configuration needs a mode of the tests.
- Be wary of anything that only usually holds, such as a mapping that tends to land nearby or a page size that happens to match. Make
  the rare case the common one in tests.
- A syntax-only check cannot catch link errors, and a build with a precompiled header cannot catch missing includes.
