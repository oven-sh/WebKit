# Porting the ahead-of-time compiler to another CPU

The back end is written for ARM64 and has run on macOS and Linux. Everywhere else `ENABLE(AOT)` is off, all of this compiles away,
and the engine is as upstream has it. This says what assumes ARM64, where, and in what order to go about x86-64.

The counts are from reading the source, not from compiling it for another CPU. Expect them to be low.

## How it fits together

| Stage | Files | Depends on the CPU? |
| --- | --- | --- |
| Bytecode to a graph, and what is known about the whole program | `AOTGraph.*`, `AOTProgram.*`, `AOTEscapeAnalysis.*`, the type inference | No |
| Graph to B3 | `AOTLower*.cpp` | Only in patchpoints |
| B3 to machine code | `AOTCompiler.cpp`, then B3 and Air as they are | A little |
| Code shared by all functions: calls to unknown callees, property access, entry and exit | `AOTStubs.*`, `AOTThunks.*` | **Yes, nearly all of it** |
| Laying the code out and resolving references | `AOTImage.cpp`, with helpers at the end of `AOTStubs.cpp` | The helpers |
| At run time: instances, linking, caches, operations | `AOTRuntime.*`, `AOTOperations*.cpp`, `AOTInlineCaches.cpp` | No |
| Objects made when the program is built | `heap/StaticHeap.*`, `bmalloc/StaticRegion.*` | No; see "The platform" below |
| The interpreter's side | `llint/LowLevelInterpreter.asm` (`virtualThunkFor`) | No: offlineasm, and it already builds everywhere |

## What assumes ARM64

### 1. The gates

- `ENABLE_AOT` in `wtf/PlatformEnable.h`, and `BENABLE_STATIC_REGION` in `bmalloc/BPlatform.h`. They must agree.
- The nine `AOTLower*.cpp` are gated as whole files on `ENABLE(AOT) && CPU(ARM64)`, as are most of `AOTCompiler.cpp`, `AOTStubs.cpp` and
  `AOTThunks.cpp`. **None of that has ever been compiled for another CPU.**
- `compileForImage()` declines everything where there is no back end, so with the gates on and the back end missing, a program
  runs from its bytecode. That is the first state to reach.

### 2. Registers: the hard part

`AOTConvention.h` describes the convention. It has choices for x86-64 that nothing has ever used.

The stubs (`AOTStubs.cpp`, near the top) use 8 argument registers, `this`, the argument count, the callee, 5 scratch registers (`T11` to
`T15`) and the 3 pinned registers (the instance, and the engine's two tag registers): 19, besides the frame pointer, the stack
pointer and the link register.

x86-64 has 16 in all. Without the stack and frame pointers and the 3 pinned ones, 11 are left. With the 6 argument registers of
`GPRInfo` and the other three, **2 are left as scratch where the stubs use 5.**

So decide this first. `numberOfArgumentGPRs` need not be `GPRInfo::numberOfArgumentRegisters`: with 4, there are 4 scratch registers,
and functions with more parameters use `Signature::List`, which exists. (`EntryWord` has 4 bits for the count.) A function and its
callers derive the convention from the bytecode alone (`conventionOf()`), so there is one place to change.

Also: `indexOfFunctionGPR` in `AOTStubs.h`; the sequences that save and restore every register around a call into C++
(`AOTStubs.cpp`, about line 290: `x0` to `x15` in pairs, `d0` to `d7` and `d16` to `d31`); the stubs that are generated once per result
register (`x19` and up); stores of the zero register.

### 3. The return address is in a register

- A function that calls nothing and keeps nothing on the stack has no frame. `hasNoFrame()` in `AOTCompiler.cpp` recognizes a remaining
  call by its patchpoint clobbering `lr`, which every call declares (`AOTLowerCalls.cpp`, `AOTLowerCore.cpp`).
- **A stub pushes no frame.** It finds out who called it from `lr`: the function and the position in it are derived from the address
  at which execution resumes (`FunctionRef::at()`, `classifyAddress()`). With `call` pushing the address, it is at
  the top of the stack instead, and the stack is 8 bytes off its alignment inside a stub.
- Two places materialize a return address other than the next instruction (`adr lr, label`, then a jump), and two take the address
  of the same label to compare return addresses with, to tell whether a frame is to be popped on the way out: look for
  `returnFromCallWithList()` and `s_addressesOfLabels`.

### 4. Instructions are four bytes

- Code is linked into a `Vector<uint32_t>` (`AOTCompiler.cpp`, `AOTStubs.cpp`).
- A leading jump that turns out to be unnecessary is removed by moving every offset by `sizeof(uint32_t)`.
- A near call is located as "the end of the instruction, less four bytes" (`StubCalls::link()`).
- Padding loops and the sizes of veneers count in instructions.

### 5. References that are resolved when the image is laid out

Each is a small function at the end of `AOTStubs.cpp` with `RELEASE_ASSERT_NOT_REACHED()` for other CPUs, which makes them a list of what to
write:

| Function | On ARM64 |
| --- | --- |
| `retargetStubCall()` | `bl` or `b` with a 26-bit offset |
| `writeVeneer()` | `adrp`, `add`, `br`: for a direct call whose target is out of reach |
| `IndexReferences::load()` and `fill()` | `add` and `ldr` with 12-bit immediates: a table entry at a distance that depends on the function's index |
| the fix-ups of `s_addressesOfLabels` | `adr` |

A branch reaches 128 MB, which is why an image has up to 8 copies of the stubs (`mostCopiesOfStubsInImage`) and veneers between
functions. A 32-bit displacement reaches 2 GB, so on x86-64 there should be one copy and no veneers: see `reach` in `AOTImage.cpp`. The
copies are identical byte for byte, so a stub cannot know which copy it is.

**The code must stay position-independent.** No PC-relative reference leaves it, the image has no absolute pointer to it, and the
absolute addresses in it are of data in the static region. An executable of the static heap refers to its code by offset
(`EntryWord`).

### 6. Odds and ends

- A StructureID becomes an address with one `movk` of the upper half of `structureIDBaseOfImages` (three places in `AOTStubs.cpp`).
- Of the 133 methods of the macro assembler that `aot/` uses (1,941 uses), four have no definition for x86-64 or in the shared
  helpers: `extractUnsignedBitfield64` (6 uses), `div32`, `multiplySub32`, all in `AOTStubs.cpp`, and
  `convertDoubleToInt32UsingJavaScriptSemantics` in `AOTLowerCore.cpp`, which is behind a check of the CPU's features already.
- `imageStamp()` mixes in the CPU, and has a case for x86-64.
- Compiled code runs with the JIT off (`Options::notifyOptionsChanged()`), and an image is refused with it on.

## The platform, as opposed to the CPU

- **The static region** is 32 GB at a fixed address, chosen per OS in `StaticRegion.h`: beyond ASAN's shadow memory on macOS, within a
  39-bit address space on Linux. Every process maps its first 256 KB, whether or not it was compiled ahead of time, and does not start
  if the address is taken. Structures follow the region, by a hint.
- **Page and block sizes.** `MarkedBlock::blockSize` is the larger of 16 KB and `CeilingOnPageSize`, which is 64 KB on Linux arm64 and 4 KB on
  x86-64. A constant of 16 KB where that was meant crashed every process on Linux arm64 and nowhere else; there is a `static_assert`
  now (`StaticHeap::offsetOfFirstStructureBlock`). An image is aligned to 16 KB in its file (`imagePageSize`, `pageSizeOfImage`), so a kernel with
  larger pages cannot map it.
- **Mapping the code** is the embedder's business. Bun maps it from the executable's own file, wherever the system puts it.

## An order to do it in

1. Both gates on for the new CPU, the back end still missing. Everything compiles and links, in the engine and in the embedder, and
   `run-tests.py` passes, because `compileForImage()` declines everything and the tests run interpreted.
2. Decide the registers (2 above).
3. Lift the whole-file gates of `AOTLower*.cpp` and `AOTCompiler.cpp`, and make them compile.
4. The entry adapter, the prologue and the epilogue. A function that returns a constant, called from the interpreter.
5. The references (5 above), then direct calls between functions.
6. The stubs, in the order in which `JSTests/stress/aot-*.js` needs them. There are about 150 (`AOTStubs.h`), most of them small, and many are
   one generator with different arguments.
7. Exceptions and stack walking: `aot-*` tests with `catch`, stack overflow, `Error.stack`.
8. `compare-with-interpreter.py` over all of `JSTests/stress`. Then the modes `aot` and `aot-validate` of `run-javascriptcore-tests`. Then `fuzz.py`.
9. The embedder's tests.

The tools are in `Tools/Scripts/aot/`, with a README.

## What to be suspicious of

- An executable that does not use its image still runs, from its bytecode, and passes most tests. Check that the image was used.
- Anything that holds "usually": a mapping that tends to be nearby, a page size that happens to match. Make the rare case the normal
  one in tests.
- A syntax check cannot see a link error, and a build with a precompiled header cannot see a missing include.
