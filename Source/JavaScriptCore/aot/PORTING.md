# Porting the AOT compiler to another CPU

There are two back ends, ARM64 and x86-64. `ENABLE(AOT)` is on for both wherever B3 is built and `useAOTFile()` is implemented: macOS, Linux
(Android included), FreeBSD and Windows. ARM64 has been run on macOS and Linux, and x86-64 on macOS under Rosetta. The rest has not been
compiled yet. On every other target `aot/` compiles to nothing.

This document says what depends on the CPU, how the two back ends differ, and how to add a third.

## Architecture

| Stage | Files | CPU-dependent? |
| --- | --- | --- |
| Bytecode to graph, whole-program analysis | `AOTGraph.*`, `AOTProgram.*`, `AOTEscapeAnalysis.*`, `AOTTypeInference.cpp` | No |
| Graph to B3 | `AOTLower*.cpp` | Which registers patchpoints name |
| B3 to machine code | `AOTCompiler.cpp`, then stock B3 and Air | The prologue |
| Structural stubs: calls, entries, exceptions, constants, adapters around operations | `AOTStubs.*` | Shared generators with a few conditionals |
| Data stubs: property access, arithmetic, comparison, iteration, allocation | `AOTStubs.*`, `AOTThunks.*` | Shared generators with a few conditionals. **Optional** |
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

About 100 of the 150 stubs are data stubs. **A port does not need them to run**, and x86-64 ran without them at first. It needs them to
be worth using: on a large application, compiling without them costs about 50% more instructions and a 56% larger executable.

`--useAOTDataStubs=0` selects the lowering without them on either CPU, and `run-tests.py` runs every test in that mode too. So what a
new CPU compiles at first is tested on the existing ones, and a failure that also happens there is not in the port. The mode is part of
`imageStamp()`, because the formats of the inline caches depend on it.

The generators are shared. Of the 98 functions that were written for ARM64, 87 needed no change of registers for x86-64. Sections 2 and
3 say what the others needed.

### 2. Registers

`AOTConvention.h` defines the calling convention and the registers that stubs may use.

| Role | ARM64 | x86-64 |
| --- | --- | --- |
| Arguments of a function, and of a stub after the first (`A1`-`A3`) | `x0`-`x7` | `rdi`, `rsi`, `rdx`, `rcx` |
| First operand and result of a data stub (`R0`) | `x0` | `rax` |
| `this` | `x8` | `rax` |
| Argument count, and the immediate of a stub (`T9`) | `x9` | `r10` |
| Callee (`T10`) | `x10` | `r8` |
| `T11` | `x11` | `r9` |
| `T12` (data stubs only on x86-64) | `x12` | `rdi`, which is also `A0` |
| `T13` | `x13` | `r12` |
| `T14` (data stubs only) | `x14` | `rcx`, which is also `A3` |
| `T15`, the index of the calling function on a miss (data stubs only) | `x15` | `r10`, which is also `T9` |
| Survive calls and stubs | `x19`-`x25` | `rbx` |
| Instance, number tag, not-cell mask | as the JIT tiers | `r13`, `r14`, `r15` |

`numberOfArgumentGPRs` need not equal `GPRInfo::numberOfArgumentRegisters`. Functions with more parameters use `Signature::List`.
Callers and callees both derive the convention from bytecode via `conventionOf()`.

**A data stub takes its first operand where it returns its result** (`firstStubOperandGPR`). The result of one stub is then where the
next one wants it, so `a.b.c` needs no move between its stubs, and a stub returns what it computed in place. It has to return in the
return register of calls, because `GetById` leaves through a call to a getter. On ARM64 that is also the first C argument register. On
x86-64 it is not, so in the generators `R0` names the operand and `A0` only the first argument of a C++ call. The lowering places the
first operand last on x86-64, so that a result still in `rax` can be moved out of it first.

Where there are few registers, names overlap. On x86-64 `A4` and `A5` are `T10` and `T11`, `T14` is `A3`, `T12` is `A0` and `T15` is `T9`. A function may use both
names of a pair only one after the other: `prepareMissAtSite()` orders its last moves for this. The functions that need more registers
than there are give some of them another meaning per CPU at their top (`generateGetByIdWith()`), or keep what does not change in a loop
on the stack and compare with memory (`MapOrSetLookup`).

Front ends (`AOTThunks.cpp`) run in place of a C++ operation, under the C convention. Only `rax` and `r10` are free there, so on x86-64
they save four registers on entry (`enter()`, `leave()`).

**One register has to survive stubs and calls**, for the pointer to the function's inline caches. A third of all calls pass the address of
a cache, and reloading the pointer from the frame for each was 4% of the instructions of x86-64. So no stub uses `rbx`. During a call the
receiver, the arguments, the callee and the count leave two temporaries, which is why x86-64 has its own `findCalleeCode()`: it compares with
memory and loads the entry last (`callTargetGPR`, `callTemporaryGPR`). With one such register the allocator has to be told what to keep in
it, or it gives it to a value for which it only turns a load into a move: `keepDataInRegister()` makes the pointer a fast temporary, and
asks that the group it is coalesced into stays one, because the pointer is a phi wherever a function may have to be linked first.

Where a temporary is callee-saved in the C ABI, as `r12` is:

- Calls and data stubs clobber it (`Lowering::registersClobberedByCalls()`).
- **B3 counts a callee save that a patchpoint clobbers as used**, and would save it in every prologue for no one.
  `Air::Code::setUnsavedCalleeSaves()` says not to. The helpers that B3 compiles do save it, because cold calls save only what the C ABI
  lets an operation clobber.
- **A function that ends in a tail call has restored it before the call stub uses it.** So compiled code as a whole does not preserve
  it, and `adapt()`, through which the engine enters compiled code, saves and restores it. `adapterSavedRegisters()` tells the unwinder.
- **A stub that the engine enters must not use it before `adapt()` has saved it.** Those stubs use `entryT12` and `entryT13`, which on
  x86-64 are argument registers: at such an entry the arguments are on the stack.
- The seventh argument of an operation is passed in it, because a miss writes that argument. The eighth is passed in `rbx`, which is only
  read.

The adapters around operations must not use a C argument register as a temporary (`operationGPR`). Operations take up to eight integer
arguments; where the C ABI has fewer registers, `operationArgumentGPR()` names registers for the rest and `callAndCheckException()` pushes
them.

### 3. Where the return address is

On ARM64 it is in `lr`; on x86-64 `call` pushes it.

- Wherever a stub begins with `emitFunctionPrologue()` the two are the same.
- `outgoingFrameSlot()` addresses a slot of the frame being made before the call, `incomingFrameSlot()` at the entry of the callee. They
  differ on x86-64.
- `callPreservingRegistersAndReturn()` gives the return address to its callback in `T11`.
- A miss finds the calling function from the return address (`loadCallerIndex()`). On x86-64 it has to be told how much the stub has
  pushed since it was entered.
- At the entry of a stub the stack pointer of x86-64 is 8 bytes past a 16-byte boundary. `pushWithReturnAddress()` saves one register and
  realigns; `preservingRegisters()` saves several around a call to C++.
- The stack check of a prologue is a stub on ARM64. On x86-64 it is inline (`AOTCompiler.cpp`): a stub that moves the stack pointer
  has to return with a jump, which unbalances the CPU's prediction of returns.
- A leaf function with no spills has no frame (`hasNoFrame()`). A call is detected by a patchpoint that clobbers `callMarkerGPR`, a
  register that is never allocated: `lr`, or `rbp`. On ARM64 such a function may still make cold calls and fetch constants, through stubs
  that make its frame for it. x86-64 has no such stubs (`hasStubsForFunctionsWithoutFrame`), and without a frame its stack would be
  misaligned at a call, so there those count as calls. Few functions are affected: 0.4% of those of a large typed program.
- Frames made for calls with an argument list are recognized by their return address, `returnFromCallWithList()`, so that tail calls can
  reuse them. ARM64 sets `lr` to it and jumps. x86-64 makes all such calls with one call instruction (`callTargetWithList()`), or
  pushes the address. `loadLabelAddress()` is `adr`, or `lea` relative to `rip`.

### 4. What x86-64 leaves out

- **Entry points for an operand in any register** (`acceptsOperandInAnyRegister()` and the like). Most are a move and a jump: they trade
  an instruction at each call site for two executed, to make ARM64's code smaller. On x86-64 a stub clobbers every register but one, so an
  operand usually comes from the frame, and a load can target any register.
- **Entry points that return in a callee save** (`returnsResultInAnyRegister()`). The one register that survives a call is taken.
- **The prologue stub**, see section 3.

How many registers survive a call matters less than it seems. With ARM64's allocator restricted to 4, 2 and 1 callee saves instead of 7,
a large application executed the same number of instructions to within 1%, in code 3%, 7% and 12% larger: a value that is kept in a
callee save costs a save, a restore and a move, where a spilled one costs a store and a load.

### 5. Instruction size

`sizeOfNearCall` and `codeOffsetUnit` in `AOTStubs.h`. `StubCall::offset` is where the call instruction starts. Trailing padding is only
trimmed on ARM64, where it cannot be mistaken for the end of an instruction.

### 6. Relocations applied at image layout

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

### 7. Instances

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

### 8. Ownership levels

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

### 9. CPU features

The JIT tiers ask the CPU they run on what it supports. An image runs elsewhere, so `compileImage()` first calls
`MacroAssembler::useOnlyFeaturesOfBuildTarget()`, which forgets every feature that the engine itself was not compiled to require. An image
belongs to one build of the engine, so the two cannot disagree. Built for Nehalem, x86-64 uses up to SSE4.2 and `popcnt`, and no AVX, BMI or
`lzcnt`. On Linux, ARM64 may lose `fjcvtzs`; `Lowering::doubleToInt32()` then truncates inline and calls only when that saturates, as on
x86-64.

### 10. Regular expressions

`Yarr::jitCompileForImage()` makes position-independent code. It differs from the JIT's in three places: the address of a table, the address
to backtrack to that is stored in the frame, and the call of a slow path that the JIT shares. The first two are a placeholder that is filled
in after linking, `adr` (or `adrp` and `add` for a table that regular expressions share) or `lea` relative to `rip`.

### 11. Miscellaneous

- A StructureID is converted to an address by adding `Instance::structureIDBase` (`structureWithID()` in `AOTStubs.cpp`). The stub that enters
  compiled code from outside has no instance yet and reads the same value from the VM, found through the callee's `MarkedBlock`.
- NaNs differ. Arithmetic on x86-64 makes NaNs with the sign bit set, so nothing may compare NaNs by their bits.
- `imageStamp()` includes the CPU.
- Compiled code requires the JIT to be off (`Options::notifyOptionsChanged()`); an image is rejected otherwise.

## Platform (as opposed to CPU)

- **Mapping.** Both the shell and embedders use `useAOTFile()`. The embedder maps the file read-only at any address and passes the file and
  offset. Only the code is mapped a second time, as executable. There are no other platform requirements: no reserved address ranges, no
  allocator or WTF changes, and no per-thread or per-VM setup.
- **FreeBSD** has no `/proc/self/exe`, and `/dev/fd` only goes up to 2 unless `fdescfs` is mounted. The shell asks `sysctl()` for its path and
  runs the image from a temporary file.
- **Windows.** JIT operations, host functions and the entry points from C++ use the System V convention there (`SYSV_ABI`), so compiled
  code and stubs are the same as on other systems. `useAOTFile()` takes a `HANDLE` that was opened for execution. A view of a file starts
  on the allocation granularity, 64 KB, so the view starts before the code. For an image in a section of the executable, which the loader
  has mapped, `useAOTFileInLoadedSection()` makes the code executable in place.
- **An image belongs to one build of the engine** (`imageStamp()`), and layouts differ between systems. So an image for a system is built
  on that system, even where the CPU is the same.
- **Page size.** An image is aligned to 16 KB within its file (`imagePageSize`, `pageSizeOfImage`), so kernels with larger pages cannot map
  the code.

## Suggested order

The x86-64 port took these steps: about 520 lines up to step 8, and about 700 for step 9.

1. Enable the gate for the new CPU with no back end. Everything should compile and link. `compileForImage()` rejects every function, so
   `run-tests.py` runs interpreted and only the tests that require compiled code fail.
2. Decide the register assignment (section 2).
3. Open the gates of `AOTLower*.cpp`, `AOTCompiler.cpp`, the structural part of `AOTStubs.cpp` and `installImageCompiler()`, and fix the build.
4. Relocations (section 6).
5. `run-tests.py`.
6. `JSTests/stress` compiled ahead of time. Run what fails on ARM64 as well, with and without data stubs. What fails there too is not in
   the port: much of it is a difference that compiled code has on purpose.
7. The `aot` and `aot-validate` modes of `run-javascriptcore-tests`, then `fuzz.py`.
8. The embedder's tests. An image is built by the CPU it is for: B3 targets its host.
9. Data stubs. Remove the conditionals around the generators and let the compiler find what the CPU lacks, then read every generator for
   what it cannot find (see the pitfalls). The verbose log lists the offset of every stub, so comparing it before and after a change shows
   that the existing back ends generate what they did.

Steps 1-7 only need `jsc`, which builds an image and runs from it the same way an embedder does. The tools are in
`Tools/Scripts/aot/`; see the README there.

## Pitfalls

- Registers that are distinct on one CPU may not be on another. On x86-64 the second return register is the third argument register, so a
  stub that reloads `A2` before it tests for an exception has lost the exception. The receiver of a call is the first operand of a stub.
- The macro assembler uses its scratch register silently for a 64-bit immediate on x86-64, including in comparisons with a pointer. Where a
  stub keeps a value there, it compares with 32-bit immediates.
- Division and multiplication to 128 bits have fixed registers on x86-64.

- In the shell, a function the compiler rejects is interpreted and the test still passes. Check that it was actually compiled.
- A path that only one configuration takes rots. The lowering without data stubs had an inline cache that did not know a format added
  later for the stubs. Every configuration needs a mode of the tests.
- Be wary of anything that only usually holds, such as a mapping that tends to land nearby or a page size that happens to match. Make
  the rare case the common one in tests.
- A syntax-only check cannot catch link errors, and a build with a precompiled header cannot catch missing includes.
