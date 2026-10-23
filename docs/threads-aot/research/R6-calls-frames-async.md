# R6. Control flow at run time in an AOT executable: calls, frames, exceptions, faults, coroutines, jobs, VM entry

Sources are read at engine head 5c2923e566cb and Bun head 8bb8d41bffe. Path prefixes used below:

- `JSC/` = `Source/JavaScriptCore/`
- `WTF/` = `Source/WTF/wtf/`
- `BUN/` = `<bun>/`

Terms: an **Instance** is the per-realm block of memory that compiled code addresses through one pinned register
(`AOT::Instance`, `JSC/aot/runtime/AOTInstance.h:249`). A **stub** is a piece of shared machine code in the image, written with the
macro assembler (`JSC/aot/stubs/`). An **operation** is a C++ function that compiled code or a stub calls (`operationAOT...`).
An **adapter** is the stub at the border from JSC's calling convention to the compiled one. A **SavedFrame** is the heap cell that
holds the stack frame of a suspended coroutine.

## 0. The one-paragraph answer

Compiled code keeps no per-thread state of its own. Everything that a thread would own (stack limit, pending thrown value, stack
pointer of the handler being unwound to, "who is resumed next", scratch words, the page that loop polls read) lies either in the
`Instance` (one per realm) or in the `VM`, and is written with plain stores. The `Instance` also holds what threads of one realm must
share (module variables, string cells, site caches, method table), all at fixed offsets from the same pinned register. The code,
the handler tables, the fault table and the call-site tables are read-only and keyed by PC, so they need no change. Generators,
SavedFrames, promises and the microtask queue are all changed by "load, test, plain store" sequences of several words, many of them
in stubs, and most of these races end in type confusion, not only in a wrong JavaScript value.

---

## 1. The compiled calling convention

### 1.1 Registers

`JSC/aot/stubs/AOTConvention.h:18-46`, `JSC/aot/stubs/AOTStubRegisters.h:20-63`, `JSC/jit/GPRInfo.h:118-124, 248-254`.

| Role | x86-64 | ARM64 | Where |
| --- | --- | --- | --- |
| Instance (pinned) | `r13` (`GPRInfo::jitDataRegister`) | `x26` | `AOTStubRegisters.h:20`, `GPRInfo.h:121, 251` |
| Base of the cell cage (pinned) | `r14` | `x25` | `AOTStubRegisters.h:22, 26` |
| Number tag, not-cell mask | not kept in registers: constants, or words of the Instance | `x27`, `x28` pinned | `JSC/aot/codegen/AOTPrimitiveEmitter.cpp:180-190` |
| `x18` | | pinned if B3 would otherwise allocate it; no stub names it | `AOTPrimitiveEmitter.cpp:187-188`, `JSC/aot/stubs/AOTStubAssemblerOperationCalls.cpp:326-327` |
| Generic arguments 0.. | `rdi rsi rdx rcx` (4) | `x0..x7` (8) | `AOTConvention.h:20, 26` |
| `this` | `rax` | `x8` | `AOTConvention.h:21, 27` |
| Argument count | `r10` | `x9` | `AOTConvention.h:22, 28` |
| Callee (the closure cell) | `r8` | `x10` | `AOTConvention.h:23, 29` |
| "value entry" (resolved code address of a call by value) | `r12` | `x11` | `AOTConvention.h:24, 30` |
| Direct arguments, GPR | `rdi rsi rdx rcx r8 r9 rax r10` (8) | `x0..x7` | `AOTConvention.h:33-44` |
| Direct arguments, FPR | `FPRInfo::toArgumentRegister` (`xmm0..`) | `q0..` | `AOTConvention.h:45-46` |
| Result | `rax` or `xmm0`; a second result in the allocator's next register | `x0` or `q0` | `JSC/aot/passes/AOTArgumentLocations.cpp:195-210` |
| Macro scratch | `r11` | `x16`, `x17` | `MacroAssembler::s_scratchRegister` |
| Word of a throw site | `r11` | `x16` | `AOTStubRegisters.h:23, 27` |

There are three entry conventions (`JSC/aot/passes/AOTArgumentLocations.h:20`, `AOTArgumentLocations.cpp:55-89`):

- **GenericRegisters**: what a call by value uses. Every argument is a boxed `JSValue`. `this`, count and callee in the registers
  above. Arguments beyond the registers are "stack words" at `fp + 16 + 8*i` (`AOTConvention.h:48-56`). Only functions with the role
  `GenericEntrypoint` have it.
- **GenericBuffer**: all arguments in memory, from stack word 0 (`JSC/aot/codegen/AOTEntryEmitter.cpp:22-33`).
- **Direct**: every other function role. Each parameter travels in its own representation: `Int32`, `Int64`, `Boolean`, `Cell`,
  `Cell32` (32-bit reference), `JSValue`, `LiteralString`, `RawWord` in a GPR, `Double` and `OptionalDouble` in an FPR
  (`AOTArgumentLocations.h:50-71`). Captures, `this`, callee, new.target and the count are ordinary parameters that exist only if the
  function uses them (`AOTArgumentLocations.cpp:110-139`). `ChooseCallingConventions` (`JSC/aot/passes/AOTChooseCallingConventions*.cpp`)
  picks how many registers a function takes and whether its result is boxed. A proven `undefined` or `never` result has no location
  (`AOTArgumentLocations.cpp:203-204`).

### 1.2 Callee-saved registers

- The set is the C ABI's (SysV also on Windows: operations are `SYSV_ABI`, `WTF/PlatformCallingConventions.h:37, 84`), with one
  exception on x86-64: `r12` is **not** preserved by a compiled call (`unsavedCalleeSaveGPRs`, `AOTStubRegisters.h:50`;
  `registersClobberedByCompiledCalls()`, `:63`; handed to Air at `JSC/aot/codegen/AOTB3Policy.cpp:177`).
- So the callee-saved registers that the allocator can use are `rbx`, `r15` on x86-64, and `x19..x24` plus `q8..q15` on ARM64.
- A function that saves its frame on suspension loses `r15` (x86-64) or `x19..x24` (ARM64) at each suspension
  (`suspensionClobberedGPRs`, `AOTStubRegisters.h:51, 55`) and may not use `q8..q15` (`pinnedFPRsInFrameSavingFunctions`, `:56`;
  `AOTB3Policy.cpp:188-191`).

### 1.3 Frame layout

```
higher addresses
  fp + 16 + 8*i   stack word i: argument beyond the registers, or a stack result     AOTConvention.h:49-56
  fp + 8          return address
  fp + 0          caller's frame pointer         <- "the place of the frame record" names the frame
  fp - 8*k        callee saves, from ImageFrame::startOfCalleeSaves() words below the record
  ...             spills, frame-allocated objects, outgoing stack words
  sp
lower addresses
```

- **There is no `CodeBlock` slot, no callee slot, no argument-count slot and no `this` slot.** They exist in registers at entry and
  die unless the function uses them.
- `ImageFrame` (8 bytes per function, read-only, `JSC/aot/image/AOTImageFormat.h:190-226`) gives the frame size in 16-byte units, the
  mask of saved registers and where they start.
- **On x86-64 a function may have no frame record at all** (`frame.mayOmitFrameRecord = isX86_64() && ...`, `AOTB3Policy.cpp:222`).
  Frames are also shrink-wrapped (`:211`), so a function can be "without its frame" at some return addresses. The image says per
  return address whether the frame is set up (`SideTableParts::frameRanges`, `AOTImageFormat.h:244-267`).
- "A frame of compiled code is named by the place of its frame record, the word below its return address, whether it has a record or
  not." (`JSC/aot/runtime/AOTCompiledFrame.h:33`). `frameAtReturnSite()` (`:34-41`) computes it: `fp` with a record, `sp + frameSize`
  without, `sp - 8` for an empty frame.

### 1.4 How a compiled frame is recognised and linked

- **By return address only.** `Image::locate(pc)` classifies an address as `Function`, `Stub`, `RegExp`, `WebAssembly`, `FFI` or
  `NotInImage`. A walker looks at the return address stored in a frame it already knows.
- `CompiledFrameIterator` (`AOTCompiledFrame.h:185-305`, `JSC/aot/runtime/AOTCompiledFrameIterator.cpp`) walks a **run** of compiled
  frames and stub frames. Its cursor carries frame pointer, stack pointer, raw PC and the `EntryFrame`
  (`AOTCompiledFrame.h:254-272`). `passOverFrame()` (`AOTCompiledFrameIterator.cpp:254-267`) steps: with a record the caller's `fp`
  is `record->callerFrame`, without one `fp` is unchanged; the caller's `sp` is `frame + 16 + bytesAboveReturnAddress`.
- Stub frames are told apart by **which return label** the address is: `StubFrameKind` (14 kinds,
  `JSC/aot/stubs/AOTStubFrames.h:16-36`). A run ends at `Adapter`, `ConstructAdapter`, `SavedFrameAdapter`, `HandlerOfJob`
  (`AOTCompiledFrameIterator.cpp:302-316`) or at an address outside the image.
- What the image says about a return address is cached per VM in `CallSiteCache` (`JSC/aot/runtime/AOTCallSiteCache.h:23-82`): a
  two-choice hash table of 128 to 2048 entries of 64 bytes, filled, evicted and rehashed by `resolve()`
  (`AOTCallSiteCache.cpp:85-133`). "The next resolve() may move or replace the entry that either of the two returns."
  (`AOTCallSiteCache.h:50`). "Only for the thread that runs the VM: the sampling profiler asks the image."
  (`AOTCompiledFrameIterator.cpp:53`).
- **Which Instance a frame belongs to** is not in the frame. With one Instance in the VM it is `state.instances.first()`; otherwise
  the walker goes up to the enclosing adapter frame and reads `AdapterFrame::offsetOfInstance`
  (`AOTCompiledFrameIterator.cpp:100-124`).

### 1.5 A call by value

`generateCallValue` (`JSC/aot/stubs/AOTStubsCalls.cpp:333-358`). The callee cell holds a 64-bit **closure word**
(`JSC/aot/object/AOTClosureWord.h:15-98`): bits 0-31 code offset in the image, 32-53 function number, 54-55 borrowed arguments, 56
"ends its tail calls", 57 takes new.target, 58-61 `15 - numberOfParameters`, 62 "is a closure word", 63 "is not callable". One signed
compare answers "callable and declares no more parameters than are passed" (`minWordOfCalleeTaking`, `:89-94`). Then:
stack check (`checkStackBeforeEntering`, section 5), `count` into `countGPR`, `code base (Instance word image.code) + offset`, jump
(`AOTStubsCalls.cpp:148-187`). The closure word is written once when the closure is made; nothing patches it.

Not a closure of this realm: host function, bound function, `InternalFunction`, proxy, or "engine callee"
(`emitCallNotPlainClosure`, `AOTStubsCalls.cpp:294-331`).

---

## 2. Borders, and the per-VM state each one touches

### 2.1 C++ -> compiled

`AOT::callCompiledCode` / `constructCompiledCode` (`JSC/aot/runtime/AOTCompiledCall.cpp:45-105`), reached from
`Interpreter::executeCall` (`JSC/interpreter/Interpreter.cpp:1780`) and `executeConstruct` (`:1889`). Order:

1. `vm.didEnterVM = true` at scope exit (`AOTCompiledCall.cpp:49-51`).
2. `VMEntryScope entryScope(vm, realm)` (`:59`). If `vm.entryScope` is null: `vm.entryScope = this`, `++m_entryCountFromOutside`,
   `Thread::registerJSThread`, Wasm thread tracking, Mach exception ports, entry-scope services
   (`JSC/runtime/VMEntryScope.cpp:39-60`). Always `vm.clearLastException()` (`JSC/runtime/VMEntryScopeInlines.h:37-39`).
3. `vm.isSafeToRecurseSoft()` reads `vm.softStackLimit()` (`:60`); `vm.disallowVMEntryCount` (`:63`).
4. `vmEntryToJavaScriptWithNArguments(entry, &vm, nullptr, callee, this, context, args...)` for up to 6 arguments, else
   `vmEntryToJavaScript(entry, &vm, &protoCallFrame)` (`JSC/aot/runtime/AOTCompiledCall.h:50-70`, `AOTCompiledCall.cpp:20-43`). **There
   is no AOT-specific `vmEntry`**: it is LLInt's. `entry` is the adapter (`CallData::compiled.entrypoint`).

`doVMEntry` (`JSC/llint/LowLevelInterpreter64.asm:177-312`) in order: makes the `VMEntryRecord` on the stack; stores `m_vm`,
`m_context`; **reads `VM::topCallFrame` and `VM::topEntryFrame`** into `m_prevTopCallFrame`, `m_prevTopEntryFrame` (`:188-201`);
compares the new `sp` with `VMSoftStackLimitOffset[vm]` (`:228`); copies the header and arguments into a JSC call frame;
**writes `VM::topCallFrame = sp`, `VM::topEntryFrame = cfr`** (`:273-284`); calls; afterwards **writes both back** from the record
(`:297-306`). On ARM64 the two are one `stp`/`ldp` pair (`JSC/runtime/VM.h:426-432, 1582`).

`VMEntryRecord` (`JSC/interpreter/VMEntryRecord.h:38-62`) lies on the stack of the thread that entered. It also holds
`calleeSaveRegistersBuffer`, which unwinding uses (section 3).

Some callers skip `VMEntryScope` and the checks and call `enterCompiledCode` directly because a job loop already holds a scope:
`JSC/runtime/JSMicrotask.cpp:119-127`, `enterSavedFrame` (`JSC/aot/runtime/AOTAsyncResumption.cpp:27-34`), `tryEnterFunctionOfJob`
(`:104-122`).

### 2.2 JSC convention -> compiled: the adapters

`JSC/aot/stubs/AOTStubsCallAdapters.cpp`. Entered by `vmEntry`, by LLInt calls and by the engine's trampolines with a normal JSC
call frame being built (`callee`, `argumentCountIncludingThis`, `this`, arguments in memory).

`emitEnterAdapter` (`:154-159`) =

1. `emitFindInstance` (`:97-104`): callee from the incoming frame; **VM from the callee's MarkedBlock header or PreciseAllocation**
   (`loadCellVM`, `JSC/aot/stubs/AOTStubHelpersValues.cpp:215-224`); `structureID + VM.aotState.structureIDBase` -> Structure;
   `Structure::realm` -> `JSGlobalObject::m_aotInstance`. **This is the only machine code that produces the value of `instanceGPR`.**
2. `emitAdapterPrologue` (`:119-135`): push frame record; reserve `AdapterFrame::saveAreaSize` (64 bytes on x86-64, 48 on ARM64);
   save the caller's `r15, r14, r13, r12, rbx` (x86-64) or `x28, x27, x26, x25` (ARM64) and **the Instance itself at
   `fp-48` / `fp-40`** (`AOTStubFrames.h:86-116`); `instanceGPR = instance`; `cellCageBaseGPR = instance->cellCageBase`; on ARM64
   materialise the tag registers; **store null into the frame's `codeBlock` slot**; load the callee.

Then `generateEnterClosure` (`:279-310`): closure word; `this` and count from the frame (a scope as `this` becomes `undefined`,
`:161-178`); arguments into registers, the rest copied below as stack words after a check against `softStackLimit()` (`:205-232`);
`sp < softStackLimit` -> stack overflow (`:250`); call; `emitAdapterEpilogue` restores the saved registers (`:137-152`).

Entries: `enterClosure`, `enterSlimClosure`, `enterModuleBody` (`:279-298`), `enterClosureForConstruct` (`:442-466`),
`enterSavedFrame`, `enterHandlerOfJob` (`JSC/aot/stubs/AOTStubsCoroutineResumption.cpp:159, 361`).

Per-VM or per-Instance state: reads `VMState::structureIDBase`, `m_aotInstance`, `Instance::cellCageBase`, `stackMirror`;
the construct adapter **writes** `Instance::constructWords` (section 11, H-17). It does not touch `topCallFrame`.

### 2.3 Interpreter (LLInt) -> compiled

- First call at a site: `llint_slow_path_call` -> `handleHostCall`: `getCallData(callee).type == CallData::Type::Compiled` returns the
  adapter as the machine-code target (`JSC/llint/LLIntSlowPaths.cpp:2339-2342`, construct `:2371-2374`).
- Megamorphic: `llint_aot_virtual_call_trampoline` (`JSC/llint/LowLevelInterpreterAOT.asm:1701-1731`) tests the cell type, loads
  callee -> Structure -> realm -> `m_aotInstance` -> `stubs.closureEntrypoint` or `stubs.slimClosureEntrypoint`, and jumps.
- `op_call` has an AOT handler that writes no profile (`:1682-1698`).

### 2.4 Compiled -> operation (C++ run time)

`StubAssembler::callOperation` (`JSC/aot/stubs/AOTStubAssemblerOperationCalls.cpp:38-44`): function pointer from
`instance->runtimeTable[entry]` (a per-VM table of pointers, read-only once filled under `BUN_HARDENED`,
`JSC/aot/runtime/AOTRuntimeTable.cpp:222-233`), then a C call. First argument is the Instance, the global object or the VM
(`:101-117`).

**The stub does not store `topCallFrame` in a release build** (`prepareCallOperation`, `:46-53`, is under `ASSERT_ENABLED`). The
operation stores it itself:

```cpp
#define AOT_OPERATION_FRAME_RECORD() std::bit_cast<JSC::CallerFrameAndPC*>(__builtin_frame_address(0))   // AOTOperationScope.h:69
AOTOperationPrologueCallFrameTracer tracer(vm, frameRecordOfOperation);                                   // AOTOperationScope.h:76
vm.topCallFrame = std::bit_cast<CallFrame*>(operationFrameRecord);                                        // interpreter/FrameTracers.h:178
```

So `vm.topCallFrame` points at the **C++ operation's own frame record**, and walkers recognise that by its return address into the
image (`isOperationFrameRecord`, `AOTCompiledFrameIterator.cpp:418-424`; `StackVisitor::WithCompiledFrames::tryEnterCompiledFrames`,
`JSC/interpreter/StackVisitor.cpp:144-173`). Nothing resets it on return (only a poison value under assertions).

"Leaf" operations get no frame tracer and may not throw, allocate or look at the stack (`callLeaf`,
`JSC/aot/stubs/AOTStubAssemblerCalls.cpp:20-48`).

### 2.5 Compiled -> native host function

`generateCallHostFunction` (`JSC/aot/stubs/AOTStubsCallHost.cpp:88-108`), `generateCallInternalFunction` (`:121-129`):

1. Push a frame record, then `emitHostCallFrame` (`JSC/aot/stubs/AOTStubHelpersFrames.cpp:40-110`) builds a **real JSC `CallFrame`** below
   it: caller frame = `fp`; return PC = a label bound as `StubFrameKind::HostCall` ("This address follows no call, so it is the
   return address of nothing else", `:43`); `codeBlock = 0`; callee; count; `this`; arguments.
2. `callNativeFunctionAndReturn` (`:137-157`): `vm = instance->vm`; **`vm->topCallFrame = sp`** (`:145-146`); realm argument from the
   callee; call `NativeFunction(globalObject, callFrame)`.
3. After the call: **`vm = instance->vm; if (vm->m_exception) unwind`** (`:149-150`). This is a load from memory at
   `VM::exceptionOffset()`.

Variants:
- `CallDirectFunction` (`AOTStubsCallHost.cpp:55-86`): a native entry with unboxed arguments. "The native function is no operation:
  it stores no frame, and it reports an exception by the empty value." The stub stores `topCallFrame` (`:77-78`) and tests the result
  register for zero (`:81`). The `Leaf` form is a plain tail jump.
- "Boxed calls": a 64-entry table in the Instance maps a native function to a stub that does its work inline
  (`JSC/aot/stubs/AOTStubWords.h:18-52`, filled at start by `JSC/aot/stubs/AOTStubEntrypoints.cpp:65-84`). The index is memoised with one byte
  store into the `NativeExecutable` (`JSC/aot/stubs/AOTStubsBoxedCalls.cpp:97-146`); idempotent.
- Proxy: `generateCallProxy` (`AOTStubsCallHost.cpp:132-200`).

### 2.6 Compiled -> interpreter or any other engine callee

`generateCallEngineCallee` (`JSC/aot/stubs/AOTStubsCallEngine.cpp:106-140`): spill the argument registers above the return address;
frame record; on x86-64 save `r14`, `r15` (`EngineCalleeFrame`, `JSC/aot/stubs/AOTStubsCallFrames.h:19-30`) and materialise JSC's tag
registers in them (`:123-125`); build the outgoing JSC frame after checks against `softStackLimit()` (`:42-62`); write the realm into
its own `codeBlock` slot and 0 into its callee slot, because "CallLinkInfo::ownerForSlowPath() reads these two slots of the caller's
frame" (`:64-72`); call `llint_virtual_call_trampoline` with **one per-VM `CallLinkInfo`** set to virtual mode
(`Entry::CallLinkInfoForCall`, `AOTRuntimeTable.cpp:222-229`). The return label is `StubFrameKind::EngineCallee`. No exception test
follows: an exception in the callee unwinds (section 3).

### 2.7 Table of state per border

| State | Where it lives | Written by | Read by |
| --- | --- | --- | --- |
| `topCallFrame` | `VM` (`VM.h:431`) | `doVMEntry` in and out; every non-leaf operation; host-call and direct-call stubs | all stack walks, GC-independent; sampling profiler from another thread |
| `topEntryFrame` | `VM` (`VM.h:432`) | `doVMEntry` in and out | unwinder (end of walk, callee-save buffer), walkers |
| `VMEntryRecord` | stack of the entering thread | `doVMEntry`; the SavedFrame adapter rewrites `m_context` (`AOTStubsCoroutineResumption.cpp:276-277, 324-325, 347-348, 378-379`) | async stack traces, unwinder |
| `entryScope`, `didEnterVM`, `m_entryCountFromOutside`, `disallowVMEntryCount` | `VM` (`VM.h:995, 459, 419`) | `VMEntryScope`, callers | traps, watchdog, profiler, `isEntered()` |
| `m_exception`, `m_lastException` | `VM` (`VM.h:421-423`) | `setException`, `clearException` | stubs after host calls, `makeOperationResult`, `RETURN_IF_EXCEPTION` through the trap bits |
| `m_stackPointerAtVMEntry`, `m_stackLimit`, `m_lastStackTop`, `m_currentSoftReservedZoneSize` | `VM` (`VM.h:1431-1434`) | `JSLock`, `updateStackLimits` | C++ recursion checks, `sanitizeStackForVM` |
| soft stack limit, trap-aware soft limit | `VM.traps().m_stack` and a **mirror in each Instance** | `StackManager` | prologues, stubs, `doVMEntry` |
| `callFrameForCatch`, `targetMachinePCForThrow`, `targetMachinePCAfterCatch`, `targetInterpreterPCForThrow`, ... | `VM` (`VM.h:952-957`) | `genericUnwind`, `unwindBeyondCompiledFrames` | the `Catch` stub, LLInt's catch |
| `encodedHostCallReturnValue`, `newCallFrameReturnValue` | `VM` (`VM.h:950-951`) | LLInt host calls | LLInt |

---

## 3. Exceptions

### 3.1 Two places for "what is thrown"

1. `VM::m_exception` (an `Exception*` cell), the engine's. Setting it also sets the trap bit `NeedExceptionHandling`
   (`JSC/runtime/VM.cpp:1342-1349`); C++ tests that bit, not the pointer (`RETURN_IF_EXCEPTION`, `JSC/runtime/ExceptionScope.h:131-135`).
2. `Instance::runtime.exceptions.thrownValue` (`JSC/aot/runtime/AOTRuntimeWords.h:33-41`): the bare thrown `JSValue`, **with no `Exception`
   cell and no stack trace**. It is a GC root (`JSC/aot/runtime/AOTProgramStatics.cpp:172`).

```cpp
struct ExceptionWords {
    EncodedJSValue thrownValue;
    // Of the frame of the handler that unwinding has found, from there to the jump to the handler.
    void* stackPointerOfHandler;
    // One more than the identifier number of the method that FindMethodById read, from there to the call of what it read ...
    uint32_t nameOfUncallableMethod;
    uint32_t predictionFailureState;
};
```

### 3.2 A `throw` in compiled code, in order

1. The stub in front of `operationAOTThrow` stores the value and jumps: `store64(value, instance->runtime.exceptions.thrownValue);
   jumpToExceptionHandler()` (`emitThrowPrelude`, `JSC/aot/stubs/AOTStubsOperations.cpp:291-299`). The return address on the stack (or in
   `lr`) is the site that threw. The C++ form does the same unless an exception is already pending
   (`JSC/aot/runtime/AOTExceptionOperations.cpp:82-92`).
2. `HandleException` = `emitUnwind` (`JSC/aot/stubs/AOTStubsExceptions.cpp:96-109`): `vm = instance->vm`; push a frame record;
   `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(vm)` (**reads `vm->topEntryFrame`**, writes the registers into that
   `VMEntryRecord`, `:42-53`); call `operationAOTLookupExceptionHandler(vm, instance)`.
3. `operationAOTLookupExceptionHandler` (`JSC/aot/runtime/AOTCallSlowPaths.cpp:66-115`):
   - takes **its own frame record** as the start (`:69-70`), not `topCallFrame`;
   - `exception = vm.exceptionForInspection()`; if set, `thrownValue = 0`, else reads `thrownValue` (`:72-82`);
   - a termination exception finds no compiled handler (`:83-84`);
   - `findCompiledCallers` stops if the caller is `vm.topEntryFrame` (`JSC/aot/runtime/AOTUnwinding.cpp:127-132`);
   - `unwindCompiledFrames(vm, frames, *vmEntryRecord(vm.topEntryFrame), false)` (`:87`).
4. `unwindCompiledFrames` (`AOTUnwinding.cpp:134-185`), for each machine frame from the youngest:
   - `site = frames.callSite()`: the `CallSiteCache::Entry` of the return address;
   - `site.exceptionHandler()`: the **`HandlerRecord { codeOffset, type }`** that the image's handler table has for that call site
     (`JSC/aot/image/AOTHandlerTable.h:24-36`; `findHandler(handlers, CallSiteIndex)`). Found -> `finishAtHandler` (`:107-113`):
     **`instance.runtime.exceptions.stackPointerOfHandler = callFrame - frameSize`**, also `instance.json.didUnwindTo(sp)`;
   - else `copyFrameSavedRegisters`: the frame's saved registers go into the `VMEntryRecord` buffer (`:81-89, 162`);
   - a function that "rejects on throw" (async) ends in the stub `RejectInFrame` (`:116-120, 164-165`);
   - leaving a generator's body by throw sets its state to `Completed` (`completeExitedGenerator`, `:75-79`);
   - the end of the run: a resumed SavedFrame rejects; an adapter's saved registers are copied to the buffer and
     `instance->coroutines.frameThatMayContinue = nullptr` (`:173-182`).
5. Back in the operation: with a handler and no need for an `Exception` cell it returns `(handler code, handler frame pointer)`
   (`AOTCallSlowPaths.cpp:90-98`). A `finally` handler, a debugger or `breakOnThrow` needs the cell: it is created (with or without a
   captured stack, `:96, 104`) and parked again in `thrownValue` (`:105-109`).
6. The stub: `fp = result2`; restore callee saves from the buffer and **zero the buffer** (`restoreCalleeSavesAndClearBuffer`,
   `AOTStubsExceptions.cpp:55-65`); on x86-64 **`rsp = instance->runtime.exceptions.stackPointerOfHandler`** (`:86-94`; on ARM64 the
   landing pad computes `sp` from `fp`, `JSC/aot/codegen/AOTExceptionEmitter.cpp:117-118`); jump to the landing pad.
   "The callee-saved registers are those of the frame of the handler, so the instance is the one that unwinding has left the word in."
7. The handler takes the value: `emitCatchPrelude` loads `thrownValue`, stores 0, returns it; if it was 0 the value is in
   `vm.m_exception` and `operationAOTCatch` takes and clears that (`AOTStubsOperations.cpp:277-289`,
   `AOTExceptionOperations.cpp:94-113`, with `RELEASE_ASSERT(exception && !isTerminationException)`).

No handler in the compiled run: `unwindBeyondCompiledFrames` (`AOTUnwinding.cpp:187-206`) either calls `genericUnwind(vm, frameRecord)`
or, at the entry frame, writes `vm.callFrameForCatch`, `vm.targetMachinePCForThrow = handleUncaughtException`, etc. The stub then fakes a
small native-callee frame and jumps to `vm->targetMachinePCForThrow` (`jumpToGenericUnwindTarget`, `AOTStubsExceptions.cpp:67-83`).

### 3.3 Checks after calls

| Call | How the caller learns of an exception |
| --- | --- |
| compiled -> compiled | **No check.** A throw never returns normally: it unwinds by tables |
| compiled or stub -> operation returning a value | second result register (`rdx` / `x1`) holds the `Exception*` (`AOTStubAssemblerOperationCalls.cpp:184-186`; `ExceptionOperationResult`, `JSC/jit/OperationResult.h:57-69`, filled from `scope.exception()` at `:143-151`) |
| ... returning void | first result register (`:187-189`) |
| ... returning a double or a pair | load `instance->vm`, load `VM::m_exception` (`:190-200`) |
| stub that preserves registers around a call | load `VM::m_exception` (`AOTStubAssemblerCalls.cpp:92-93`) |
| host function | load `VM::m_exception` (`AOTStubHelpersFrames.cpp:149-150`) |
| direct native function | result register is the empty value (`AOTStubsCallHost.cpp:81`) |
| engine callee (LLInt) | no check; the engine unwinds into compiled frames |
| polls | load `VM::m_exception` (`JSC/aot/stubs/AOTStubsPolls.cpp:106-107, 131-132, 150-156`) |
| the SavedFrame adapter's job loop | load `VM::m_exception` (`AOTStubsCoroutineResumption.cpp:197-198, 331-332, 353-354`) |

`operationAOTThrow` returns the fake pointer `1` in the exception register so that the caller unwinds
(`AOTExceptionOperations.cpp:90-91`).

### 3.4 Mixed frames

An exception thrown in C++ or LLInt: `genericUnwind` -> `Interpreter::unwind` (`Interpreter.cpp:1396-1460`). With compiled code it uses
`StackVisitor::visitWithCompiledFrames` (`:1439-1444`), whose constructor starts from **`vm.topCallFrame` and `vm.topEntryFrame`**
(`StackVisitor.cpp:73-106`). `UnwindFunctor::unwindCompiledFrames` (`Interpreter.cpp:1256-1272`) calls the same
`AOT::unwindCompiledFrames`; on `LeftCompiledCode` the walk goes on from the exit (`visitFrom`). A compiled handler becomes a
`CatchInfo` whose `m_nativeCode` is the stub `Catch` and whose `m_nativeCodeForDispatchAndCatch` is the landing pad
(`AOTUnwinding.cpp:304-311`). `generateCatch` (`AOTStubsExceptions.cpp:186-209`) finds the VM from the callee slot of the frame it is
entered with, restores callee saves from the buffer, loads and clears `vm->callFrameForCatch`, loads
`vm->targetMachinePCAfterCatch`, loads the handler's stack pointer from the Instance, jumps.

### 3.5 Errors raised at a site (null reads, TDZ, not a function)

`ThrowAtSite` (`dumpRegistersAndThrow`, `AOTStubsOperations.cpp:215-250`) dumps **all** registers to the stack and calls
`operationAOTThrowAtSite(instance, registers, site, source)` (`AOTExceptionOperations.cpp:356-395`). The site's words say which register
held the offending value and in which representation (`throwSiteValue`, `:296-347`). If the nearest handler ignores the value
(`catch {}`), no error object is made: `thrownValue = undefined` (`:365-369`; `nearestHandlerIgnoresValue`, `AOTUnwinding.cpp:235-260`).

### 3.6 Validation and termination

- `EXCEPTION_SCOPE_VERIFICATION` state is per VM: `m_topExceptionScope`, `m_needExceptionCheck`, `m_simulatedThrowPointLocation`,
  `m_simulatedThrowPointRecursionDepth`, `m_throwingThread` (`VM.h:1303, 1437-1440`, `VM.cpp:1416-1419`). `ExceptionScope` links itself into
  `m_topExceptionScope` (a stack discipline of one thread).
- Termination is one shared `Exception` cell per VM (`m_terminationException`, `VM.h:422`), a flag `m_hasTerminationRequest`
  (`:1543`) and the trap bit `NeedTermination`. Compiled handlers never catch it (`isTermination` in `unwindCompiledFrames`).
  `Interpreter::unwind` defers termination while it unwinds an ordinary exception (`Interpreter.cpp:1398-1420`);
  `m_deferTerminationCount` is per VM.
- "Compiled code polls the stop request alone, and looks at no trap bit. Native code that clears whatever was thrown may clear a
  TerminationException, whose bit is spent by then. So the polls stay armed for as long as the termination is asked for, and throw it
  again." (`JSC/runtime/VMTraps.cpp:431-435`).

### 3.7 Two more users of the exception words

- **"Misfit" returns.** A callee whose result is not of the promised type jumps to `ReturnMisfit`
  (`AOTStubsCalls.cpp:780-798`); `operationAOTFindLandingOfMisfit` looks the caller's return address up, stores the value in
  **`instance->scratch.misfit`** and returns the landing address (`AOTCallSlowPaths.cpp:41-51`). "the landing takes it before anything
  else runs" (`JSC/aot/codegen/AOTCodegenScratchWords.h:33-34`).
- **Owed out-of-memory errors.** `throwOutOfMemoryErrorAtReturnOf` (`AOTUnwinding.cpp:286-298`) overwrites a **return address on the
  stack** with `instance + offsetOfLoopPollPage() + 8` (not executable) and parks the real return address in
  `stackPointerOfHandler`. The return faults, and the fault handler turns it into `HandleException` (section 4.4).

---

## 4. The fault handler

### 4.1 Installation

- `AOT::installFaultHandler()` is called once from `JSC::initialize` when `Options::runsCompiledImage()`
  (`JSC/runtime/InitializeThreading.cpp:158-161`). It adds one lambda for `Signal::AccessFault`
  (`JSC/aot/image/AOTFaultHandler.cpp:193-228`). `activateSignalHandlersFor(Signal::AccessFault)` follows when the image is published
  (`JSC/aot/image/AOTImageLoading.cpp:496`).
- **It is process wide.** The list of handlers is in `g_wtfConfig.signalHandlers`, frozen after initialisation.
- POSIX: `SIGSEGV` and `SIGBUS` (`WTF/threads/Signals.cpp:420`), `sigaction` with `SA_SIGINFO` only, **no `SA_ONSTACK`**, all other
  signals blocked but the thread-suspend signal (`:589-603`). The handler runs on the faulting thread, on its own stack.
  An unhandled fault goes to the previous action (`:543-551`), which is Bun's crash handler. That one registers one static 512 KB
  `sigaltstack` from the thread that installs it (`BUN/src/crash_handler/lib.rs:1667-1695`).
- Windows: a vectored exception handler, `EXCEPTION_ACCESS_VIOLATION` (`WTF/win/SignalsWin.cpp:87, 94-127`), on the faulting thread.
- macOS: **Mach exceptions** by default (`useMachForExceptions`, `JSC/runtime/OptionsList.h:636`; Bun allows them for compiled programs,
  `BUN/src/jsc/bindings/ZigGlobalObject.cpp:342-345`). One port, one dispatch source, `mach_msg_server_once`
  (`Signals.cpp:122-187`): **the handler runs on another thread** and edits a copy of the faulting thread's register state
  (`catch_mach_exception_raise_state`, `:311-368`). Exception ports are set **per thread**, when the thread first enters a VM
  (`registerThreadForMachExceptionHandling`, `:403-413`, called from `VMEntryScope::setUpSlow`, `VMEntryScope.cpp:52-54`).

### 4.2 How a fault is recognised

`AOTFaultHandler.cpp:195-227`, in order:

1. PC from the context. `Image::current()` (an atomic pointer, `JSC/aot/image/AOTImage.h:85`). No image: not handled.
2. `pc == instanceGPR + offsetOfLoopPollPage() + 8`: a diverted return (4.4).
3. `image->faultSiteAt(pc)` (`JSC/aot/image/AOTFaultTable.cpp:368-406`): binary search over checkpoints in the read-only section
   `FaultSites`, then at most `faultSitesBetweenCheckpoints` varint steps. Exact PC match only. The site says `NullCheck` or
   `LoopPoll`, whether the function has its frame there, and up to 3 words of throw-site description
   (`JSC/aot/image/AOTFaultTable.h:21-33`).
4. `NullCheck` (`handleNullCheck`, `:125-146`): the faulting **address** must be `< 16 KB` (`FaultSite::reach`) or within 16 KB above
   the value of `cellCageBaseGPR` in the context. Otherwise not handled (a real crash).

### 4.3 What it rewrites

For a null check (`:135-145`): "The call-site table and ThrowAtSite treat the faulting instruction as a near call, whatever its
length." `returnAddress = pc + sizeOfNearCall` (5 on x86-64, 4 on ARM64, `AOTImageFormat.h:44-48`).
- ARM64: `lr = returnAddress` (in a frameless function the old `lr` goes to `x16` first); `pc = stub ThrowAtSite(FaultSite, ...)`.
- x86-64: `r11` is an allocatable register, so its value is parked in
  **`static thread_local uint64_t s_scratchRegisterBeforeFault`** (`:74, 141`); `r11 = returnAddress`; `pc = stub`. The stub pushes `r11`
  as the return address (`AOTStubsOperations.cpp:232-233`), and the operation puts the parked value back into the register dump
  (`AOTExceptionOperations.cpp:359-363`). **It writes nothing to the stack and nothing to shared memory.**

For a loop poll (x86-64 Linux only, `handleLoopPoll`, `:149-172`): the address must be exactly the Instance's poll page (+4 for a
frameless function); it **pushes** onto the thread's stack the frame pointer (frameless case) and `pc + 7`, and sets `pc` to the stub
`PollAtLoop`.

### 4.4 Diverted return

`handleDivertedReturn` (`:175-191`) reads `instance->runtime.exceptions.stackPointerOfHandler` **through the Instance register of the
context**, makes it the return address (`lr`, or pushed), and continues at `HandleException`.

### 4.5 Memory kept without access

| Range | Size | Purpose | Where |
| --- | --- | --- | --- |
| Address 0 .. 16 KB | `FaultSite::reach` | a load through a boxed `null` (2) / `undefined` (10) / empty (0) used as a pointer. `static_assert(ValueUndefined + typeInfoTypeOffset < reach)` | `JSC/aot/codegen/AOTThrowEmitter.cpp:46` |
| Cage base .. +16 MB (+ a random prefix) | `unusedPrefixSize = minIndex << shift = (1<<21) << 3` | a `Cell32` of `absent` 0, `null` 2, `undefined` 10 decodes to `base + (n << 3)`: at most `base + 80`. The whole reservation starts with `InitialAccess::None` | `JSC/heap/MarkedBlockCage.cpp:34, 92, 104`; `JSC/aot/object/AOTSlotRepresentation.h:69-73`; `AOTThrowEmitter.cpp:47` |
| Cage base + 8 MB, one page | page | **readable**, read-only, zero: the "empty extension". `static_assert(FaultSite::reach <= offsetOfEmptyExtensionInCage)` | `MarkedBlockCage.cpp:63-73`; `JSC/aot/object/AOTCellFormat.h:91`; `JSC/aot/codegen/AOTSlotEmitter.cpp:239` |
| Instance - 1 MB, one page | 16 KB reserved | the loop poll page: `PROT_READ|WRITE` normally, `PROT_NONE` while a stop is requested (x86-64 Linux). Never executable, which the diverted return relies on | `JSC/aot/runtime/AOTInstanceLayout.h:27-28, 45`; `JSC/aot/runtime/AOTRuntimeHooks.cpp:141-153`; `JSC/aot/runtime/AOTInstance.cpp:289-300` |

Which accesses may trap is decided in Air (`b3/air/AirFoldTrapsIntoAccesses.cpp`, not read) under
`trapReach = reach - (undefinedCell32 << 3)` (`AOTB3Policy.cpp:39, 252`).

`JSC/aot/runtime/AOTEmptyPages.h` is **not** about this: it decommits pages of tables that hold only zeros after a collection
(`decommitEmptyPages`, `:48-91`).

### 4.6 Several threads faulting at once

- POSIX and Windows: safe as written. The lambda reads the read-only image, the context's registers and one `thread_local`.
  No lock, no allocation, no shared write.
- The cage base and the Instance are taken from the **faulting context's registers**, so the handler needs no notion of "the VM".
- Exception: `handleDivertedReturn` reads a shared Instance word (H-3).
- Mach: faults of all threads are served one after the other by one dispatch source. On macOS x86-64 the `thread_local` is written on
  the handler's thread and read on the faulting thread; see "Not sure" N-4.

---

## 5. Stack overflow

### 5.1 The limits

`VM::updateStackLimits()` (`VM.cpp:1504-1547`): bounds of **`Thread::currentSingleton().stack()`**; with `m_stackPointerAtVMEntry` set,

```
soft limit = stack.recursionLimit(spAtEntry, Options::maxPerThreadStackUsage() /*5 MB*/, m_currentSoftReservedZoneSize)
m_stackLimit = stack.recursionLimit(spAtEntry, 5 MB, Options::reservedZoneSize() /*64 KB*/)
```

(`WTF/StackBounds.h:89-109`, `OptionsList.h:134-136`.) With an image the soft reserved zone is
`softReservedZoneSize (128 KB) + BuiltinCells::stackKeptFromProgram (256 KB, 768 KB under ASAN)`
(`JSC/aot/runtime/AOTVMState.cpp:49`, `JSC/aot/runtime/AOTBuiltinCells.h:46-47`). On Windows the stack is touched page by page down to the new soft
limit (`preCommitStackMemory`, `VM.cpp:1494-1501, 1544-1545`).

- The **hard** limit `m_stackLimit` is for C++ only (`isSafeToRecurse()`).
- The **soft** limit goes through `traps().setStackSoftLimit()` into `StackManager` (`JSC/runtime/StackManager.cpp:86-102`): it stores
  `m_softStackLimit`, then under `m_mirrorLock` the trap-aware limit (unless a stop is requested), then **both words of every
  registered `Mirror`**.

`StackManager::Mirror` = `{ Atomic<void*> m_trapAwareSoftStackLimit; void* m_softStackLimit; }` plus list links
(`JSC/runtime/StackManager.h:42-64`). Each Instance has one in its header (`AOTInstance.h:104`), registered in
`registerStackMirror` (`AOTInstance.cpp:289-300`).

### 5.2 The checks in code

All compare `sp` (or the future `sp`) with a word **at a fixed offset from `instanceGPR`**:

- Small frame (`emitSmallFrameCheck`, `JSC/aot/codegen/AOTStackCheckEmitter.cpp:29-33`): after the frame is allocated,
  `cmp sp, [instance + stackMirror.trapAwareSoftStackLimit]; jb`. One compare serves both the overflow test and the trap poll. On failure
  it calls the stub `PollAtPrologue`, which compares again with the plain soft limit to tell the two apart
  (`AOTStubsPolls.cpp:55-97`).
- Large frame (`emitLargeFrameCheck`, `:60-69`): before allocation, `newSP = base - size`; wrap-around test; compare with the plain soft
  limit.
- A function may have **no** check (`setStackCheckGenerator`, `:71-93`): `StackCheck::CoveredByCallers` with a frame of at most 240 bytes,
  or `DeferredToCallee`. Constants: `maxFrameSizeWithoutStackCheck = 240`, `maxUncheckedFrameSize = 256`,
  `maxNestedFunctionsWithoutStackCheck = 8`, `maxStackUsedWithoutCheck = 4096` (`AOTConvention.h:62-68`).
- `checkUncheckedStackUsage` (`JSC/aot/image/AOTStackUsageCheck.cpp:147-170`) is a **build-time proof** over the linked code: the graph of
  calls into bodies that are entered without a check has no cycle that uses stack, and the deepest chain from a body "proven to fit"
  needs at most `LimitID::UncheckedStackBytes` (= 4096). A violation is an internal build error.
- Stubs: calls by value (`checkStackBeforeEntering`, trap-aware, `JSC/aot/stubs/AOTStubAssembler.cpp:617-635`), adapters, argument lists, engine
  calls, constructs, RegExp frames, restoring a SavedFrame (`frame size + 4096`, `AOTStubsCoroutineResumption.cpp:461-465, 504-508`).
  The full list is the `grep softStackLimit|stackMirror` in `aot/` without the tests: 33 lines in 17 files.
- C++: `vm.isSafeToRecurseSoft()` before entries.

Failure: `ThrowStackOverflow` -> `operationAOTThrowStackOverflowError` (`AOTCallSlowPaths.cpp:29-38`) -> `HandleException`. "a frame that does
not fit is an error at the call in the caller" (`AOTB3Policy.cpp:209`).

### 5.3 Bounds of a thread's stack

`StackBounds::currentThreadStackBoundsInternal()` (`WTF/StackBounds.cpp:167-207`), cached in `Thread::m_stack` (`WTF/Threading.h:421`):

- Linux main thread (`getpid() == gettid()`): top from `getauxval(AT_EXECFN)`, rounded up to a page, believed only if
  `environ` lies above the current `sp` and the path lies above `environ` within 8 MB (`:151-165`). The reason: "glibc's
  pthread_getattr_np() reads /proc/self/maps for the main thread, with sscanf() on each line." The size is `RLIMIT_STACK` (8 MB if
  unlimited) less a page, and the origin is lowered to the oldest `environ` (`:182-207`).
- Any other thread: `pthread_getattr_np` + `pthread_attr_getstack` (`newThreadStackBounds`, `:123-146`).

---

## 6. Coroutines

### 6.1 Shape

A generator or async function is compiled as ordinary machine code with an ordinary stack frame. **To suspend, the frame's words are
copied into a heap cell; to resume, they are copied back onto whatever stack is current**, and control jumps to the saved resume
address. There is no state machine in the code and no separate coroutine stack.

`SavedFrame` (`JSC/aot/runtime/AOTSavedFrame.h:52-230`), 64 bytes of header plus the words:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | JSCell header, butterfly | an ordinary `JSNonFinalObject`, callable (`OverridesGetCallData`): its call entry is the adapter `EnterSavedFrame` |
| 16 | `uint16 m_numberOfWords`, `uint8 m_numberOfWordsSkippedAtTop`, `uint8 ...AtBottom` | fixed when the cell is made |
| 20 | `uint32 m_version` | seqlock for the concurrent marker: odd while being written |
| 24 | `uint32 m_liveSlotMapOffsetPlusOne` | **0 = the frame is on a stack** (`isOnStack()`, `:138`); else which map of live references applies |
| 28 | `CoroutineKind m_kind`, `Continuation m_continuation`, `uint8 m_numberOfSavedRegisterPairs`, `uint8 m_flags` | flags: bit 0 "entered by job loop", bit 1 "suspended at yield", bits 2-3 step status |
| 32 | `void* m_resumeAddress` | address in the image |
| 40 | `JSCell* m_promiseOrWaiter` | the function's promise, or the SavedFrame that awaits it, or a combinator context, or itself |
| 48 | `uint64 m_preservedRegister` | `rbx` on x86-64 |
| 56 | `EncodedJSValue m_asyncContext` | |
| 64 | words | the frame from `bottom + skippedAtBottom` up to `fp - skippedAtTop` |

- The callee-save area at the top of the frame is skipped: "The callee-save area holds registers of whoever resumed the function, and
  every restore writes it anew." (`:108`). The outgoing argument area at the bottom is skipped too (`:97`).
- The 64-bit **site word** that compiled code passes at each suspension encodes the live-slot-map offset (18 bits), which registers
  the function saves (2), where the function's promise lies (32), and the words skipped at the bottom (8) (`:87-118`).
- GC: `visitFields` (`:271-315`) reads version, map and words, and re-reads the version: "A reader that races with save() can see a new resume
  address with an old offset. The version says afterwards whether it did." On a race `visitor.didRace(...)`. The map lists boxed values
  and 32-bit references; all other words are opaque (`JSC/aot/runtime/AOTSavedFrameMap.h`).

`CoroutineWords` (`JSC/aot/runtime/AOTCoroutineWords.h:22-61`) is **not** the frame layout. It is the part of the Instance with the transient
words of resumption (section 7.4) and constants.

### 6.2 Suspending (`yield`)

`generateSuspendGenerator` (`JSC/aot/stubs/AOTStubsCoroutineSuspension.cpp:824-889`), called from the body with generator, value, site word:

1. Load the generator's SavedFrame (`:860`). None yet: first save (allocate in the stub, `emitAllocateSavedFrame`, `:375-467`, or
   `operationAOTSaveFrameAndYield`, `JSC/aot/runtime/AOTCoroutineOperations.cpp:54-66`).
2. **`emitStoreSuspendedState`: `generator->m_state = SuspendedAtYield`** (`:861`, `:776-798`). *This comes before the frame is saved.*
3. `emitSaveFrame` (`:338-354`): `++version`; store fence; copy the words (`rep movsq` from 32 words on, else pairs); `m_preservedRegister`;
   `m_resumeAddress` = the stub's return address; `m_flags`; `m_liveSlotMapOffsetPlusOne`; store fence; `++version`. The increments are
   plain `add32` to memory (`:272-282`).
4. Write barrier on the SavedFrame.
5. `emitLeaveFunction` (`:179-206`): reload the resumer's callee saves from the frame, then return **from the suspended function's frame**
   to whoever resumed it, with the yielded value in the result register.

### 6.3 Resuming

`generateRestoreSavedFrameToStack` (`AOTStubsCoroutineResumption.cpp:603-616`):

1. `emitCopyFrameToStack` (`:453-527`): the three counts from one 32-bit load; stack check; push a frame record (**so the frame's return
   address is the label after the call of this stub, "which tells who resumed the frame"**, `:602`); `sp -= size`; copy the words.
2. `emitStoreResumerCalleeSaves` (`:531-544`): the resumer's registers into the frame's save area; reload `rbx`.
3. `emitMarkAsOnStack` (`:547-555`): `++version; fence; m_liveSlotMapOffsetPlusOne = 0; fence; ++version`. "From here on the words of the
   SavedFrame are stale, so it lists none of them."
4. `emitContinueAtSite` (`:557-595`): flags, resume address; if "entered by job loop" and `!awaitMustSuspend`,
   **`instance->coroutines.frameThatMayContinue = fp`**; jump to the resume address with value and mode. Mode `Throw`:
   `instance->runtime.exceptions.thrownValue = value`, then `HandleException` with the resume address as the throwing site.

Who calls it: `StepGenerator` (return label `GeneratorBody`), `RunAsyncGeneratorStep` (`AsyncGeneratorBody`), the adapter
`EnterSavedFrame` (`SavedFrameAdapter`). `SavedFrame::forFrame(instance, frame)` finds the running SavedFrame of a frame by comparing the
frame's return address with `instance.stubs.returnAddress(SavedFrameAdapter)` and reading the adapter frame's callee slot
(`JSC/aot/runtime/AOTSavedFrame.cpp:40-45`).

### 6.4 The state word of a sync generator

`AOT::Generator` (`JSC/aot/runtime/AOTGenerator.h:26-78`), 32 bytes: `SavedFrame* m_savedFrame` at 16, `int32 m_state` at 24,
`bool m_hasOtherPrototype` at 28.

```cpp
enum class State : int32_t { Running = -2, Completed = -1, SuspendedAtStart = 0, SuspendedAtYield = 1 };
```

Transitions:

| From | To | By | Where |
| --- | --- | --- | --- |
| SuspendedAtStart / AtYield | Running | plain `store32` after `load32; js` | stub: `AOTStubsCoroutineResumption.cpp:112-118`; C++: `JSC/aot/runtime/AOTGenerator.cpp:87-110` |
| Running | SuspendedAtYield | the suspension stub, **before** saving | `AOTStubsCoroutineSuspension.cpp:785, 861` |
| Running | Completed | after the body returned: `load32; cmp Running; store32` | `AOTStubsCoroutineResumption.cpp:129-133`; `AOTGenerator.cpp:117-119` |
| Running | Completed | the unwinder passes the step stub's frame | `AOTUnwinding.cpp:75-79`, `AOTGenerator.cpp:52-55, 171-174` |

**"Generator is already running"** is only the sign test of `m_state`:

```cpp
jit.load32(Address(generator, Generator::offsetOfState()), status);
isNotStepped.append(jit.branch32(CCallHelpers::LessThan, status, TrustedImm32(0)));       // Running or Completed: not stepped here
...
jit.store32(TrustedImm32(static_cast<int32_t>(Generator::State::Running)), Address(generator, Generator::offsetOfState()));
```

The stub answers `NotStepped` and the caller takes the generic path, which ends in `Generator::resume` and throws
`createGeneratorIsExecutingError` (`AOTGenerator.cpp:88-90`; native `next`/`return`/`throw` at
`JSC/aot/builtins/AOTEngineIntrinsicOperations.cpp:111, 126`).

### 6.5 Two threads calling `next()` on one generator: what breaks

1. **Both pass the test.** Load and store are separate plain accesses. Both see 1, both store -2.
2. Both run `RestoreSavedFrameToStack` on the same SavedFrame. The copies only read, so each thread gets a full copy of the frame.
   **The same activation now runs twice.** Its words include raw pointers, unboxed numbers, 32-bit references, half-built state ("an
   integer, half a fraction, the header for the next closure, whatever compiled code keeps over the suspension", `AOTSavedFrame.cpp:136`).
   Any invariant of the kind "this object is known only to this frame" (unescaped allocations, proofs that hold "for a stretch of code")
   is void.
3. `emitMarkAsOnStack` on both threads: four non-atomic `add32` on `m_version`. A lost update leaves the version **odd for good**: the
   marker then reports a race at every visit (`visitFields`, `version & 1`), and never marks the frame's references. Or it is even while
   a later writer is in the middle.
4. At the next `yield` both threads run `emitSaveFrame` into **the same cell**, perhaps from two different yield sites:
   `m_resumeAddress` of one, `m_liveSlotMapOffsetPlusOne` of the other, words interleaved. The marker then reads raw words as cells, and
   the next resume jumps to site A with the frame of site B. **Type confusion and wild pointers.**
5. Even with a correct lock on entry, **step 2 of 6.2 publishes `SuspendedAtYield` before the words are saved.** A second thread that calls
   `next()` in that window passes the test and copies stale words with the old resume address while the first thread overwrites them.
   The order of the stores has to change, or the release of the state has to move behind `emitDidWrite`.
6. `Running -> Completed` after return is a check-then-act too, and the unwinder's `didThrow()` is a blind store.

Async generators (`JSAsyncGenerator`, 10 internal fields, `JSC/runtime/JSAsyncGenerator.h:78-89`) are worse: `StepAsyncGenerator` tests `State`,
`ResumeMode`, `Frame`, then writes `ResumeValue`, `ResumeMode`, `ResumePromise`, `State = Executing` as four plain 64-bit stores
(`JSC/aot/stubs/AOTStubsCoroutineSteps.cpp:159-176`), and `emitFinishOnlyRequestAtYield` writes four more (`:199-206`).

Async functions have **no state word at all**. What keeps a SavedFrame from being resumed twice is that exactly one job or one "waiter"
hand-over refers to it. `SavedFrame::save` asserts `!m_liveSlotMapOffsetPlusOne` in C++ (`AOTSavedFrame.cpp:93`); the stubs assert nothing.

---

## 7. Promises, jobs, the microtask queue

### 7.1 JSPromise: two words

`JSC/runtime/JSPromise.h:41-45, 66-99, 300-301`:

```
m_packed : CompactPointerTuple<JSCell*, uint16_t>   low 48 bits: payload cell, high 16 bits: flags
m_slot   : WriteBarrier<Unknown>
flags: 0-1 Status (Pending 0, Fulfilled 1, Rejected 2) | 2 isHandled | 3 isFirstResolvingFunctionCalled
       4-5 InlineReactionKind (None, InternalMicrotask, FulfillHandler, RejectHandler) | 6-13 InternalMicrotask
       14 payload is the captured async context | 15 reserved
```

The meaning of both words depends on the flags:

| State | payload cell | `m_slot` |
| --- | --- | --- |
| Pending, no reaction | null | empty |
| Pending, inline `InternalMicrotask` | the task's cell argument, or the async context (bit 14) | **the context: for `await` the SavedFrame** |
| Pending, inline handler | the derived `JSPromise` | the handler function |
| Pending, list | head `JSPromiseReaction` | empty |
| Settled | null | the value or reason |

Every transition is **"store `m_slot`, then store `m_packed`"**, both plain:

- `setInlineMicrotaskReaction`, `setInlineHandlerReaction` (`JSC/runtime/JSPromise.cpp:254-282`).
- `spillInlineReaction` (`:284-318`): allocate a reaction from the two words, `clearSlot()`, `setPackedCell(flags & ~inline, reaction)`.
- `performPromiseThen` (`:384-465`): `switch (status())`; pending: inline if `kind == None && !payload`, else
  `existing = reactionHead(vm)`, new reaction with `next = existing`, `setPackedCell(flags() | isHandled, reaction)`.
- `fulfillPromise` / `rejectPromise` (`:773-830`): `currentFlags = flags()`; by kind: read the reaction out of the two words;
  `setSlot(argument)`; `setPackedCell(settledFlags, nullptr)`; queue jobs.
- `resolve` / `reject` / `fulfill` (`:184-209`): `if (!isFirstResolvingFunctionCalled()) { setFlags(flags() | flag); ... }`.
- The resolving functions: `callee.ResolvingOther` and `other.ResolvingOther` are nulled as the "already resolved" record (`:882-899`).

**The stubs do the same in machine code**, with the flags tested as one 64-bit word:

| Stub code | What | Where |
| --- | --- | --- |
| `emitResumeWhenPromiseSettles` | `await` on a pending promise without a reaction: test `packed & ~(isHandled|firstCalled) == 0`, then `slot = savedFrame; packed = word | reaction bits` | `AOTStubsCoroutineSuspension.cpp:488-507` |
| `emitLoadSettledValue` | `await` on a fulfilled promise that may continue at once: `packed |= isHandled` (load, or, store), read `slot` | `:149-168` |
| `generatePromiseThen` | `then` with one handler | `JSC/aot/stubs/AOTStubsCoroutinePromises.cpp:67-99` |
| `emitFulfillPromise` | fulfil with no reaction, an inline handler or an inline resume task; reserves and fills the job in between | `:121-213` |
| `generateReturnAsyncResult` | an async function returns: `packed == 0` -> `slot = value; packed = Fulfilled|firstCalled`; or hand over to the waiter | `:292-383` |
| `generateNewPromise` | inline allocation | `:30-57` |
| LLInt `op_new_promise` | | `LowLevelInterpreterAOT.asm:1080` |

### 7.2 Two threads on one promise

- **resolve vs resolve.** Both read the flag clear, both proceed: two settlements. The second `fulfillPromise` finds `status != Pending`
  (only an `ASSERT`), reads `payloadCell()` (null) and overwrites the value. Reactions that ran with the first value have seen another
  value than later `then`s: a wrong JavaScript value. Not unsafe by itself.
- **then vs then.** Both see "no reaction" and both write the inline form: one reaction is lost (its handler never runs). With mixed
  stores, `slot` = handler of A and `packed` = derived promise of B: B's promise is settled with the result of A's handler. Wrong
  values, still well typed, because both are handler reactions. **But** `then` (handler kind) against `await` (microtask kind) mixes
  `slot = handler function` with `packed = InternalMicrotask(CompiledAsyncFunctionResume)`: the job then does
  `uncheckedDowncast<SavedFrame>(context)` on a function (`AOTAsyncResumption.cpp:152`; the stub tests the StructureID first,
  `JSC/aot/stubs/AOTStubsCoroutineJobs.cpp:190-192`, the C++ path does not). **Type confusion.**
- **then/await vs settle.** T1 (`await`): `slot = SF`. T2 (fulfil): `slot = value; packed = Fulfilled`. T1: `packed = old flags | resume
  reaction`. The promise is now *pending again*, with an inline resume reaction whose context is an arbitrary value, and the "first
  resolving function called" bit is gone, so it can be settled again: `CompiledAsyncFunctionResume` with a non-SavedFrame context.
  **Type confusion.** The benign interleavings lose the reaction (the async function never resumes) or deliver the handler as the value.
- **spill vs anything.** `spillInlineReaction` reads two words, allocates (a GC point), clears and rewrites: a long window.
- `markAsHandled()` and the stub's `packed |= isHandled` are read-modify-writes of the whole word: they can undo a concurrent settlement.

### 7.3 The queue

- **Where.** `VM::m_defaultMicrotaskQueue` (`VM.h:1555`, created at `VM.cpp:296`). Each `JSGlobalObject` holds a `Ref<MicrotaskQueue>` that is
  the VM's default unless replaced (`JSC/runtime/JSGlobalObject.h:245`, `JSGlobalObject.cpp:969, 4076-4083`). All queues of a VM are in
  `VM::m_microtaskQueues` for marking (`VM.h:1447`, `VM.cpp:2256-2280`). The Instance caches the pointer:
  `realm.microtaskQueue` (`AOTInstance.h:216`).
- **What.** `MarkedMicrotaskDeque { Deque<QueuedTask> m_queue; size_t m_markedBefore; }` twice (`m_queue`, `m_toKeep`) plus
  `m_isScheduledToRun`, `m_isPerformingMicrotaskCheckpoint` (`JSC/runtime/MicrotaskQueue.h:162-226, 228-313`). `Deque` is a ring buffer:
  `start`, `end`, `capacityMask`, `buffer`. `QueuedTask` is 40 bytes: word 0 = realm pointer (or dispatcher | 1) with
  `InternalMicrotask` in bits 48-55 and a payload (status) in bits 56-63; then 4 `JSValue` arguments (`:99-154`).
- **Enqueue in C++.** `JSGlobalObject::queueMicrotask` -> fast path `m_microtaskQueue->enqueue` if `m_canFastQueueMicrotask` and no
  `crossTaskToken` (`JSC/runtime/MicrotaskQueueInlines.h:66-75, 106-133`). `Deque::append` may reallocate the buffer.
- **Enqueue in stubs** (`AOTStubsCoroutineJobs.cpp:42-96`): `emitReserveJob`: `awaitMustSuspend == 0`; queue pointer; `isScheduledToRun != 0`;
  `slot = end; next = (end + 1) & mask; next != start` (else the operation grows it); address `= buffer + slot*40`. `emitFillJob` /
  `emitPublishJob`: five stores, then `end = next`. "Stores nothing. Between the two, scratch is free and nothing may be queued."
  (`AOTStubsCoroutines.h:182`). "The collector reads the queue while the mutator is stopped, and what it has not visited yet lies behind
  m_markedBefore, so a new job takes no barrier." (`AOTStubsCoroutineJobs.cpp:62`).
- **Dequeue in stubs.** `emitLoadHeadJob` (`:99-109`), `emitDequeueHeadJob` (`:199-210`): `if (markedBefore) --markedBefore; start = (start + 1) & mask`.

### 7.4 Who drains

1. Bun's event loop: `EventLoop::drain_microtasks_with_global` (`BUN/src/jsc/event_loop.rs:412-457`) ->
   `JSC__JSGlobalObject__drainMicrotasks` -> `GlobalObject::drainMicrotasks` (`BUN/src/jsc/bindings/ZigGlobalObject.cpp:3478-3551`): next-tick
   queue, `vm.drainMicrotasks()`, next-tick queue again. It also resets the async context slot when no script is on the stack (`:3513-3514`).
2. `VM::drainMicrotasks` (`VM.cpp:1809-1838`): `VMEntryScope`, then `performMicrotaskCheckpoint<true>` in a loop with
   `didExhaustMicrotaskQueue()`.
3. `JSLock::willReleaseLock` drains when the last lock is released (`JSC/runtime/JSLock.cpp:314-315`).
4. `drainImpl` (`JSC/runtime/MicrotaskQueue.cpp:182-253`): `front()`, runnability of the realm, `dequeue()`, `runMicrotask(...)`, and
   `vm.callOnEachMicrotaskTick()` after each job.
5. **The adapter `EnterSavedFrame` drains too, in machine code.** After a resumed coroutine or a handler has returned to it, if this
   entry "ends the job" it looks at the head of the queue and, for 12 known first words (`CoroutineWords::resumeJobWords`,
   `bodyCallJobWords`, `yieldAwaitedJobWord`, `driverResumeJobWord`, `reactionJobWords[4]`, `performMicrotaskJobWord`,
   `promiseAllResolveJobWord`), takes the job and runs it without leaving compiled code (`AOTStubsCoroutineResumption.cpp:186-386`).
   "MicrotaskQueue::drainImpl takes a job out of the queue before it runs it, and asks afterwards whether the queue is empty, so it does
   not notice a job that is gone." (`AOTStubsCoroutineJobs.cpp:98`). It leaves the loop on a pending exception, on a trap (trap-aware limit),
   on any job it does not know, and when `awaitMustSuspend` is set.

### 7.5 `await`, in order

`generateAwaitBySavingFrame` (`AOTStubsCoroutineSuspension.cpp:585-708`):

1. **May the frame just go on?** `emitBranchUnlessFrameMayContinue` (`:142-146`) tests `awaitConditionsOnState`
   (`JSC/aot/runtime/AOTAsyncAwaitConditions.h:61-64`): `instance->coroutines.frameThatMayContinue == fp` ("a job entered the frame as the last
   thing it does, the frame is still that one, and awaitMustSuspend is clear") and `queue.start == queue.end`. If both hold and the value
   is a primitive, a non-thenable object, or a **fulfilled** promise of the realm, the stub returns the value at once: no suspension, no job.
   This is unobservable only because nothing else can run in between.
2. Else find or allocate the SavedFrame (the frame's return address tells whether it was restored by the adapter, `:645-648`), `emitSaveFrame`.
3. Register the continuation: pending promise without reaction -> the inline reaction (7.1); settled-as-is value -> queue
   `CompiledAsyncFunctionResume(undefined, value, savedFrame, asyncContext)`; a call "without a promise" whose callee returned its own
   SavedFrame -> `callee.m_promiseOrWaiter = waiter; callee.m_continuation = Waiter` (`emitResumeWhenCallSettles`, `:510-538`); anything else ->
   an operation (`resumeWhenSettled`, `resumeWhenCallSettles`, `JSC/aot/runtime/AOTJobs.cpp:41-79`).
4. Barriers; result = `m_promiseOrWaiter`; leave the function's frame.

The job: `runInternalMicrotask` case `CompiledAsyncFunctionResume` (`JSMicrotask.cpp:2152-2158`) -> `AOT::resumeAsyncFunction`
(`AOTAsyncResumption.cpp:150-161`): sets the SavedFrame's flag, **`instance.coroutines.nextEntryEndsJob`**, then `resumeSavedAsyncFunction`
(`:136-148`) with an `AdapterEntry` that saves and replaces `enteredFrame`, `enteredAsyncGenerator`, `promiseOfEnteredHandler`,
`asyncContextIsPutBack` (`:48-100`), enters the adapter, and afterwards uses those words to learn **what the adapter entered last** so as to
reject or settle the right thing.

**Hand-over without a job**: an async function that finishes while its one waiter is known stores three Instance words
`waiterToResume`, `waiterResolution`, `waiterAsyncContext` (`emitHandOverToWaiter`, `AOTStubsCoroutinePromises.cpp:263-285`;
`tryScheduleImmediateResume`, `AOTJobs.cpp:81-108`) and returns; the adapter reads and clears them and resumes the waiter
(`AOTStubsCoroutineResumption.cpp:204-214`); `resumeRemainingWaiters` is the C++ fallback (`AOTJobs.cpp:110-126`). They are GC roots
(`JSC/aot/runtime/AOTCoroutineTable.cpp:95-101`).

### 7.6 Host hooks

- `RuntimeHooks::didChangeMicrotaskQueueing(vm)` (`AOTRuntimeHooks.cpp:116-125`) recomputes for every Instance of the VM:
  `realm.microtaskQueue`, `awaitMustSuspend = !canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries()`, and clears
  `frameThatMayContinue`. Called when the debugger, the runnability, the "fully active" flag or the queue of a realm changes
  (`JSGlobalObject.h:247-254, 1401-1408`, `JSGlobalObject.cpp:4076-4090`) and from `VM::updateHostObservesMicrotaskBoundaries`
  (`VM.cpp:644-651`: `m_onEachMicrotaskTick || m_crossTaskToken`).
- Bun sets `setOnEachMicrotaskTick` when a next-tick queue is created and clears it when it has drained
  (`ZigGlobalObject.cpp:512-519, 556-564`). So `process.nextTick` switches all the fast paths above off and on at run time.
- `AOTHost.h` is only an umbrella header plus five exported functions (`JSC/aot/runtime/AOTHost.h:33-37`). The host's other hooks are
  `hostDidCreateRealm`, `hostDidCreateStructure` (`ZigGlobalObject.cpp:3035, 2649, 2705, 3043`).
- `AOTModuleMicrotasks.h` declares `runCompiledModuleImport` and `runCompiledModuleImportSettled`, the bodies of the jobs
  `CompiledModuleImport[Settled]` (`JSMicrotask.cpp:2360-2364`).
- The async context (AsyncLocalStorage) is **one slot per realm**: field 0 of `JSGlobalObject::m_asyncContextData`, swapped around each job by
  `AsyncContextSwapScope` (`JSC/runtime/AsyncContextSwapScope.h:39-124`) and by stubs (`emitPutAsyncContextOfJobInPlace`,
  `AOTStubsCoroutineJobs.cpp:111-141`; `emitLoadCurrentAsyncContext`, `AOTStubsCoroutineSuspension.cpp:135-140`).
  `VM::m_asyncContextTrackingEnabled` (`VM.h:568`) gates it.

---

## 8. Entering the VM from an OS thread; JSLock; Workers

### 8.1 What `JSLockHolder` does today

`JSLock::lock` (`JSLock.cpp:91-116`): `m_lock` (a `WTF::Lock`; recursion by `m_ownerThread == current`), `m_ownerThread`,
`m_hasOwnerThread`, `m_lockCount`, then `didAcquireLock()` (`:118-160`), in order:

1. `thread = Thread::currentSingleton()` (TLS).
2. **`m_entryAtomStringTable = thread.setCurrentAtomStringTable(m_vm->atomStringTable())`** (`:126`). The atom table is found through the
   *thread* (`stringTable()`, `WTF/text/AtomStringImpl.cpp:76-79`), has **no lock** (`AtomStringTableLocker` is empty unless `USE(WEB_THREAD)`,
   `:48-70`), and belongs to the VM (`VM.cpp:279`: the creating thread's default table for `VMType::Default`, else a new one).
3. `m_vm->setLastStackTop(thread)` (`:129`; `VM.cpp:799-804`, with a `RELEASE_ASSERT` that it lies in this thread's stack).
4. `heap.acquireAccess()` unless it already has access (`:131-136`). **One bit** (`hasAccessBit`) of `Heap::m_worldState`
   (`JSC/heap/Heap.h:1070-1072`, `JSC/heap/HeapInlines.h:211-217`): "the mutator" has access or has not.
5. `RELEASE_ASSERT(!m_vm->stackPointerAtVMEntry())`; `setStackPointerAtVMEntry(currentStackPointer())` -> `updateStackLimits()` ->
   all mirrors (`:138-140`).
6. If another thread owned the lock last: `heap.machineThreads().addCurrentThread()` (`:142-148`): the thread joins the `ThreadGroup` whose
   stacks the collector scans.
7. `traps().notifyGrabAllLocks()`; `samplingProfiler->noticeJSLockAcquisition()` (**`m_jscExecutionThread = current`**,
   `JSC/runtime/SamplingProfiler.cpp:1057-1073`).

`willReleaseLock` (`:301-332`): drain microtasks; clear the last exception if `!topCallFrame`; release delayed objects;
`setStackPointerAtVMEntry(nullptr)`; release heap access; put the thread's atom table back.

`DropAllLocks` parks `stackPointerAtVMEntry` and `lastStackTop` **in the `Thread` object** (`:345-385`; `Threading.h:283-301, 447-448`).

Then the first `VMEntryScope` (2.1) and `doVMEntry`.

So "a VM runs on one thread at a time" is built into: the lock itself, `m_ownerThread` (used by `vm.ownerThread()`,
`currentThreadIsHoldingAPILock()` assertions everywhere, `sanitizeStackForVMInCallSlowPath`, `VM.cpp:1867-1874`), one heap-access bit, one
stack limit, one `lastStackTop`, one `entryScope`, one atom table reached through whichever thread holds the lock.

### 8.2 Bun Workers

One OS thread = one `bun_jsc::VirtualMachine` = one `JSC::VM` = one heap = (with an image) one cell cage range, one `VMState`, one Instance per
realm. **Nothing of the JavaScript heap is shared.** The image is.

- `WebWorker::thread_main` (`BUN/src/jsc/web_worker.rs:640-683`): `start_vm` -> `VirtualMachine::init_worker` (`:718`) ->
  `Zig__GlobalObject__create` (`ZigGlobalObject.cpp:597-759`): `VM::tryCreate`, `vm.heap.acquireAccess()` (`:607`), `JSLockHolder` (`:608`),
  client data, `Bun::requireCellCage()` (`:671`), the global object, `vm.ensureTerminationException()` for workers (`:754`).
- Then "Take the API lock for the thread's whole life and abandon it with the VM": `JSC__VM__getAPILock(global.vm())` (`web_worker.rs:678-681`;
  `BUN/src/jsc/bindings/bindings.cpp:5230-5234`).
- `VM::VM` attaches the image: `if (AOT::Image::isRegistered()) m_aotState.attachImage(*this)` (`VM.cpp:369-372`; `AOTVMState.cpp:39-72`).
- Termination from the parent: `request_termination` (`web_worker.rs:568-589`) -> trap `NeedTermination`.
- Bun's own per-thread state: `VMHolder` (`#[thread_local]`, `BUN/src/jsc/VirtualMachine.rs:681-700`), `Bun__getVM()`,
  `Bun__setDefaultGlobalObject` (`:711`), `defaultGlobalObject()`; `GlobalObject::m_bunVM` is captured from TLS at construction
  (`ZigGlobalObject.cpp:1280`). The event loop, timers, uSockets loop are per `VirtualMachine`, hence per thread.

---

## 9. Traps, termination, watchdog, profiler, debugger

| Mechanism | How it reaches the mutator | "One mutator" assumption |
| --- | --- | --- |
| `VMTraps::fireTrap(event)` (`JSC/runtime/VMTraps.h:245-255`) | sets a bit in `m_trapBits` (atomic or), then `updateThreadStopRequestIfNeeded` -> `StackManager::requestStop` (`StackManager.cpp:36-49`): `m_stopsAtEveryPoll = true`; trap-aware limit of the VM and of **every mirror** = `UINTPTR_MAX`; `setLoopPollsEnabled(vm, true)` = `mprotect(PROT_NONE)` of every Instance's poll page (x86-64 Linux) | The bits are per VM. `handleTraps` **takes** each bit (`takeTopPriorityTrap`, `VMTraps.cpp:468-481`): the first thread that polls consumes the event. `cancelStop` follows as soon as no bit is left |
| Polls in compiled code | prologue compare (5.2); calls by value; loop polls: a faulting 7-byte load on x86-64 Linux, elsewhere `test [instance+trapAwareLimit], sign; js` (`JSC/aot/codegen/AOTHintEmitter.cpp:34-59, 165-189`). **A loop that counts up to the length of an array, string or typed array and whose likely paths always complete has no poll** (`:102-150`) | all read Instance words |
| Poll slow paths | `operationAOTPollSlow`, `operationAOTPollAtPrologue` (`AOTExceptionOperations.cpp:52-72`): `handleTraps(AsyncEvents)`, `throwTerminationExceptionIfNeeded()` | |
| Signal-based traps (`SignalSender`, `VMTraps.cpp:200-345`) | suspends `vm.ownerThread()` and patches JIT code | **Off**: no JIT means `usePollingTraps = true` (`JSC/runtime/Options.cpp:738-750`, `applyCompiledCodeDefaults`, `:805-819`) |
| Termination | `NeedTermination` -> `vm.setHasTerminationRequest(); vm.throwTerminationException()` (`VMTraps.cpp:508-513`): sets the VM's one `m_exception` | one exception slot, one request flag, `m_deferTerminationCount`, `m_executionForbidden` per VM. `notifySyncWaiterOfTermination` wakes `vm.syncWaiter()`: **one `Waiter` per VM** for `Atomics.wait` (`VM.h:1556`) |
| Watchdog (`JSC/runtime/Watchdog.cpp`) | a timer on `VMTraps::queue()` fires `NeedWatchdogCheck`; the mutator compares **`CPUTime::forCurrentThread()`** with `m_cpuDeadline` (`:75, 136`) and asks `vm.entryScope->globalObject()` (`VMTraps.cpp:504`) | CPU time of one thread; `enteredVM`/`exitedVM` pair; all under the API lock |
| `NeedStopTheWorld` / `VMManager` (`JSC/runtime/VMManager.h:44-120`, `VMTraps.cpp:515-518`) | stops **VMs**: "VM (and its VMThreadContext) represents a thread" | one thread per VM by definition. It is an existing rendezvous: request, each VM parks in `notifyVMStop`, resume |
| Sampling profiler (`SamplingProfiler.cpp:579-707`) | its own thread; `if (m_vm.entryScope)`; locks; **`m_jscExecutionThread->suspend()`**; reads registers; reads `m_vm.topCallFrame`, `m_vm.m_executingRegExp`; walks with `MachineFrameIterator<SuspendedThreadStack>`, which asks the **image**, not the `CallSiteCache` (`AOTCompiledFrame.h:83-129`, `findSampledTop`, `AOTCompiledFrameIterator.cpp:436-523`) | one execution thread, set at lock acquisition and VM entry |
| Collector's stack scan (`JSC/heap/MachineStackMarker.cpp:159-222`) | suspends **every** thread of the VM's `ThreadGroup` but the current and the conductor, copies registers and stack | already plural |
| Debugger | "Breakpoints, stepping and `debugger` don't work in compiled code" (PR text). Left in compiled paths: `needsExceptionForEveryThrow` = `globalObject->debugger() || breakOnThrow` (`AOTUnwinding.cpp:122-125`); a debugger makes `canFastQueueMicrotask` false, hence `awaitMustSuspend` | |
| Out of memory in the cage | `setNeedsOutOfMemoryError` -> `StackManager::setStopsAtFunctionEntries` (`StackManager.cpp:73-83`): the trap-aware limit is the marker until the error is thrown; prologue polls answer `pollAtPrologueMustThrow` | one flag per VM: whichever thread polls first throws the `RangeError` |

---

## 10. Thread-local storage

### 10.1 What exists on these paths

| TLS | Mechanism | Used for |
| --- | --- | --- |
| `Thread::currentSingleton()` (`Threading.h:454-482`) | Darwin with the private header: `_pthread_getspecific_direct(WTF_THREAD_DATA_KEY)`; other POSIX: `pthread_getspecific(s_key)`; Windows: a C++ `thread_local` holder (`WTF/win/ThreadingWin.cpp:338-343`) | stack bounds, atom table, saved stack pointers, `isJSThread`, suspend/resume |
| `Thread::m_currentAtomStringTable`, `m_defaultAtomStringTable` (`Threading.h:441-442`) | members of `Thread` | every `AtomStringImpl::add/lookUp`. Image atoms are looked up first through a process-wide function pointer (`AtomStringTable::readOnlyAtomLookup`, `AtomStringImpl.cpp:81-118`) |
| `Thread::m_stack` | member | `updateStackLimits`, `sanitizeStackForVM`, frame-cell tests (`JSC/aot/runtime/AOTHeapHooks.cpp:114`) |
| `g_jscConfig`, `g_wtfConfig` | **process globals**, not TLS, frozen after start | signal handlers, structure heap base, cage size |
| `s_scratchRegisterBeforeFault` | C++ `thread_local` (`AOTFaultHandler.cpp:74`) | 4.3 |
| other `thread_local`s in the run time | `AOTPrivateNameOperations.cpp:202` (a 64-entry memo), `AOTObjectInternal.cpp:42`, `AOTErrorMessages.cpp:472`, `AOTAssignmentPlans.cpp:111`, validation only: `AOTNativeCallValidation.cpp:114`, `AOTRuntimeValidation.cpp:343` | |
| Bun | `#[thread_local]` statics in `VirtualMachine.rs:695-765, 4202-4234` | the thread's VM and global object |

### 10.2 Does compiled code or a stub read TLS today?

**No.** A search of `aot/`, `b3/`, `llint/`, `offlineasm/`, `wasm/`, `ffi/` for `TPIDR`, `.gs()`, `.fs()`, `loadFromTLS`, `storeToTLS` finds
nothing outside `testb3`. The macro assembler has `loadFromTLS32/64` and `storeToTLS32/64` only under `ENABLE(FAST_TLS_JIT)` = `ENABLE(JIT) &&
HAVE(FAST_TLS)`, i.e. Darwin with `<pthread/tsd_private.h>`: `mrs TPIDRRO_EL0` (+ `and ~7` on some systems) + `ldr` on ARM64
(`JSC/assembler/MacroAssemblerARM64.h:6750-6802`), a `gs:`-prefixed `mov` on x86-64 (`JSC/assembler/MacroAssemblerX86_64.h:4592-4613, 7725-7735`).
There is no Linux (`fs:` / `TPIDR_EL0`) and no Windows form in the assembler.

The VM is found without TLS: from the Instance (`instance->vm`), or from a cell's block header (`loadCellVM`).

### 10.3 Cost of a TLS read from compiled code

This is background knowledge, **not read in the tree**; see N-5.

---

## 11. Hazards with several mutators

"Unsafe" = type confusion, wild pointer, wrong stack. "Value" = only a wrong JavaScript value, a lost callback or a spurious error.
"Crash" = a `RELEASE_ASSERT` or a clean fault.

### A. State that is logically per thread but lies in the VM or the Instance

| # | State | Written / read | A race gives |
| --- | --- | --- | --- |
| H-1 | **Soft stack limit and trap-aware limit**: `VM.traps().m_stack`, `Instance::stackMirror` | written by `updateStackLimits` for "the" thread at lock acquisition; read at every checked prologue, call by value, adapter, restore | With the limit of another thread's stack: either every check fails (spurious `RangeError` at once) or **no check ever fails**: the stack runs into its guard page, and a large frame can jump over it. Unsafe |
| H-2 | `Instance::runtime.exceptions.thrownValue` | store at `throw`; read and cleared by the lookup operation, the catch prelude, `RejectWithException` | T1 catches T2's value (Value); T2's catch finds 0 and no `m_exception`: `RELEASE_ASSERT` (Crash) |
| H-3 | **`Instance::runtime.exceptions.stackPointerOfHandler`** | written by the unwinder, loaded into `rsp` by the stub (x86-64); also the parked return address of a diverted return | T1 continues on **T2's stack pointer**, or returns to T2's address. Unsafe |
| H-4 | `VM::m_exception`, `m_lastException`, trap bit `NeedExceptionHandling` | any throw in C++; tested after host calls, in polls, in the job loop, by every `RETURN_IF_EXCEPTION` | T2 unwinds for T1's exception; T1's exception is cleared by T2's catch. Value, then Crash (`assertNoException`, `RELEASE_ASSERT(exception)`) |
| H-5 | **`VM::topCallFrame`, `VM::topEntryFrame`** | 2.7 | A stack walk starts in another thread's live stack. `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer` writes T1's registers into **T2's `VMEntryRecord` on T2's stack**, and restores from it. `doVMEntry`'s save and restore of the pair un-nests. Unsafe |
| H-6 | `VM::callFrameForCatch`, `targetMachinePCForThrow`, `targetMachinePCAfterCatch`, `targetInterpreterPCForThrow`, ... | `genericUnwind` -> stub `Catch` / LLInt | jump to another thread's handler with another frame pointer. Unsafe |
| H-7 | `Instance::scratch.misfit`, `scratch.pendingBarrierOwner`, `exceptions.nameOfUncallableMethod`, `coroutines.stepStatus`, `VM::encodedHostCallReturnValue`, `VMState::regExpMatchOutput` | "from here to the next instruction that reads it" words | `misfit`: a value of the wrong thread arrives in a generic landing: still a boxed value (Value). `stepStatus`: a consumer takes "done" for "yielded" (Value). `regExpMatchOutput` (one `int` array per VM, `AOTVMState.h:75`): wrong capture offsets used as string ranges; whether they are re-checked was not read |
| H-8 | `coroutines.frameThatMayContinue` | set at restore, cleared by the adapter, unwinder, hooks; compared with `fp` | A foreign value never equals one's own `fp`, so the test fails safe (suspends). But it can be **cleared or overwritten** by others: only slower. The pair "word == fp, queue empty" is a check-then-act on job order (Value: order of jobs) |
| H-9 | `coroutines.waiterToResume` + `waiterResolution` + `waiterAsyncContext` | three stores, read and cleared by whichever adapter returns next | T2's adapter resumes T1's waiter, or both do: **double resume of a SavedFrame** (6.5). Torn triple: wrong value. Unsafe |
| H-10 | `coroutines.nextEntryEndsJob`, `asyncContextIsPutBack`, `enteredFrame`, `enteredAsyncGenerator`, `promiseOfEnteredHandler`, `stepInProgress` | set by C++ right before entering the adapter, read and cleared in its first instructions; the others saved and restored around an entry like a stack | An adapter starts draining the queue in the middle of another job; `AdapterEntry::finish` rejects or settles the wrong promise or frame. `stepInProgress` points into **another thread's stack** (`AsyncGeneratorStepInProgress` is `WTF_FORBID_HEAP_ALLOCATION`, `AOTJobs.h:39-60`). Unsafe |
| H-11 | `VM::entryScope`, `didEnterVM`, `m_entryCountFromOutside`, `disallowVMEntryCount`, `m_stackPointerAtVMEntry`, `m_lastStackTop`, `m_deferTerminationCount`, exception-scope verification state | VM entry and exit | `entryScope` dangles when the thread that set it leaves while another is inside; `tearDownSlow` runs exit services under running code; `RELEASE_ASSERT(stack.contains(lastStackTop))` (Crash) |
| H-12 | The realm's async context slot (`m_asyncContextData[0]`) | swapped around each job | AsyncLocalStorage of one thread leaks into another (Value) |
| H-13 | The loop poll page, `m_stopsAtEveryPoll`, trap bits | 9 | Not a race, a semantic gap: an event is consumed by one thread. A "stop all threads" needs a count of arrivals |
| H-14 | `instance.json.didUnwindTo(sp)` and other JSON words (`AOTUnwinding.cpp:110`) | per-Instance state keyed by a stack address | not read in detail |

### B. Caches written at run time on these paths

| # | State | Race | Result |
| --- | --- | --- | --- |
| H-15 | **`CallSiteCache`** (per VM) | `resolve()` overwrites 64-byte entries in place, evicts, and `setNumberOfEntries` frees the old table, while another thread holds `const Entry*` or copies an entry | wrong frame size, handler or saved-register mask during unwinding; use after free. Unsafe. `m_positions`, `m_inlineFrames` likewise |
| H-16 | `NativeExecutable::indexOfBoxedCall` | one idempotent byte store | none |
| H-17 | **`Instance::constructWords`** (`ConstructWordCache`, 16-byte entries) | writer: `wordOfCall`, then `wordOfConstruct` (`AOTStubsCallAdapters.cpp:383-387`); reader: compare `wordOfCall`, load `wordOfConstruct` (`AOTStubsCalls.cpp:730-731`) | the reader pairs the new key with the **old function's construct entry**: calls another function's code with this callee. Unsafe |
| H-18 | The per-VM `CallLinkInfo` for engine callees | set to virtual mode once; whether the virtual path writes to it was not read | N-7 |
| H-19 | `prepareInlineAllocation` (`AOTOperationScope.h:89-97`): `header`, then `allocator` | the stub tests `allocator` for null and then reads `header` | store order is right for x86; on ARM64 it needs a release. The allocators themselves are per-VM `LocalAllocator`s: another report |

### C. Heap objects changed by several plain stores

| # | Object | Sequence | Result |
| --- | --- | --- | --- |
| H-20 | `AOT::Generator::m_state` | load, test, store (6.4) | double resume. Unsafe |
| H-21 | `Generator` at `yield` | state published **before** the frame is saved | resume of a stale or half-written frame. Unsafe |
| H-22 | `SavedFrame` | seqlock with non-atomic increments; one writer assumed; words, resume address and map offset are separate stores | torn frame; marker reads raw words as cells, or never marks. Unsafe |
| H-23 | `SavedFrame::m_promiseOrWaiter` + `m_continuation` | pointer, then a byte that says how to read the pointer (`setWaiter`, `ensurePromise`, `setCombinatorElement`, stub `emitResumeWhenCallSettles`); readers `uncheckedDowncast` by the byte | promise read as SavedFrame or the reverse. Unsafe |
| H-24 | `JSAsyncGenerator` internal fields | 4 tests, 4 stores | two requests overwrite each other; body entered twice. Unsafe |
| H-25 | `JSPromise` (`m_slot`, `m_packed`) | 7.1, 7.2 | lost reactions, double settlement (Value); slot/flags mismatch (Unsafe) |
| H-26 | `JSPromiseCombinatorsGlobalContext::remainingElementsCount` | `cmp 1` ... `add -1` (`AOTStubsCoroutinePromises.cpp:242-254`); the values array is written by index without a lock ("Nobody but the jobs has seen it") | `Promise.all` never settles or settles twice (Value) |
| H-27 | **`MicrotaskQueue`** | producers: read `end`, fill, store `end`; consumers: read `start`, read the job, store `start`; C++ `append` reallocates | two producers share a slot: first word of A with arguments of B, e.g. `CompiledAsyncFunctionResume` with a handler as context; a stub writes into a freed buffer; one job runs twice. Unsafe |
| H-28 | `m_markedBefore` | decremented at dequeue; read by the collector "while the mutator is stopped" | jobs not marked. Unsafe |
| H-29 | Return addresses on the stack | `throwOutOfMemoryErrorAtReturnOf` edits the current thread's own stack | safe if the walk starts from the right thread's frame (depends on H-5) |

### D. "This holds until the next call" assumptions

- **An `await` that goes on without suspending** (7.5 step 1) and the adapter's in-line job loop (7.4 item 5) rest on: nobody else adds
  to or takes from the queue between the test and the act, and a fulfilled promise stays as it was read.
- "the job is taken out of the queue, so no code of the program may run before the frame has returned to the adapter" (`AOTJobs.h:34`).
- `emitReserveJob` .. `emitFillJob`: "nothing may be queued" in between.
- `DeferGCForAWhile` in prologue polls: "What a caller knows about a callee that collects nothing holds for the callee's prologue too."
  (`AOTExceptionOperations.cpp:63-64`). With several mutators another thread can start a collection at any time; facts of the form "this call does
  not collect" become "this thread reaches no safepoint".
- Loops without polls (9) bound the time to a safepoint by the length of the data, which another thread could now grow.
- **A SavedFrame may be restored on another thread** if the job that resumes it runs there. The words are already independent of the
  stack address. But B3 may keep values derived from the pinned registers (for example `instance + constant`: `pinned.statics`,
  `pinned.instancePrefix`, `AOTPrimitiveEmitter.cpp:204-205`) in the frame over a suspension. As long as all threads of a realm have the same
  Instance and cage base this is harmless. If the design gives each thread its own Instance-like block, such values must not survive a
  suspension.
- Objects and closures that lie **in a frame** (`AllocateInFrames`, "lent" closures, `callValueLending`, `AOTStubsCalls.h:21-22`; frame arrays)
  are cells on one thread's stack. Their safety proof is "does not escape"; whether "escape" includes "stored where another thread can read it"
  is a question for the flow analysis report.

### E. Host

- Bun finds its `VirtualMachine`, event loop and default global object through TLS (8.2). A second OS thread in the same realm has none,
  or the wrong one. `GlobalObject::m_bunVM` points to the creating thread's.
- `checkIfNextTickWasCalledDuringMicrotask` uses `defaultGlobalObject()` (TLS) from inside a drain (`ZigGlobalObject.cpp:512-519`).
- Bun's crash handler has one alternate signal stack, registered on one thread.

---

## 12. What helps

1. **No code is ever patched, invalidated or freed.** No jettison, no OSR exit, no inline-cache repatching, no watchpoint that rewrites
   code. The handler table, fault table, call-site table, frame table and frame maps are read-only and keyed by PC. In the JIT engine most of the
   stop-the-world reasons are code changes; here there are none.
2. **A frame needs nothing but `fp`, `sp`, `pc` and the image to be walked.** `MachineFrameIterator<SuspendedThreadStack>` already walks a
   suspended thread's stack from another thread with no VM state (the profiler's path).
3. **The unwinder for a throw in compiled code starts from the operation's own frame record**, not from `topCallFrame`
   (`AOTCallSlowPaths.cpp:69-70`). It needs `topEntryFrame` only as the end marker and for the callee-save buffer, and that buffer already
   lies on the entering thread's stack.
4. **Compiled-to-compiled calls have no exception check**, and most operations return the exception in a register. The loads of
   `VM::m_exception` are in about ten places in stubs, all of the form `instance->vm->m_exception`.
5. **Every per-thread word is reached through one pinned register**, and that register is set in exactly one place in machine code
   (`emitFindInstance` + `emitAdapterPrologue`) and restored by unwinding from saved registers. Retargeting the per-thread words means changing
   offsets or the base in stubs and emitters, not finding hidden globals.
6. **Safepoint polls exist** at prologues, calls by value and loops, and they are one compare with a word (or one load from a page) that
   another thread can flip. The slow paths preserve all registers. `StackManager::requestStop` is already `CONCURRENT_SAFE` and loops over mirrors.
7. **The fault handler is stateless**: it decides from the PC, the faulting address and the registers in the context.
8. **The collector's stack scan is already plural** (`MachineThreads`: a `ThreadGroup`, suspend all, copy). Frames are scanned conservatively,
   so no per-frame stack maps are needed at a safepoint.
9. **SavedFrames are position independent** and have a version protocol for a concurrent reader.
10. **The state that must become atomic is small and regular.** `Generator::m_state` is its own aligned `int32`: one compare-and-swap on entry and a
    release store at the end. `JSPromise::m_packed` holds the whole state in one 64-bit word, and **flag bit 15 is reserved** (`JSPromise.h:77`).
11. **Signal-based VMTraps are off** without the JIT, and the debugger does not work in compiled code: two users of "suspend the one mutator" are gone.
12. **Workers already run N VMs over one image in one process**: `Image::s_current` is atomic, image atoms are read-only and shared, function-local
    statics such as `static void* const code = addressOfStubInCurrentImage(...)` (`AOTUnwinding.cpp:118`) are thread-safe.
13. `awaitMustSuspend` is an existing run-time switch that turns off **all** the queue and promise fast paths in stubs that reserve or take jobs
    (`emitReserveJob`, `emitLoadHeadJob`, `frameThatMayContinue`). A first implementation can set it for good in a process that has started a second
    thread, and fall back to C++ for queueing. It does **not** cover `emitResumeWhenPromiseSettles`, `generatePromiseThen`, the no-reaction case of
    `emitFulfillPromise` and the `packed == 0` case of `ReturnAsyncResult`, which write promise words without asking it.
14. On ARM64 `x18` is pinned and unused by compiled code and stubs.
15. `VMManager` is an existing request / park / resume protocol with priorities, though its unit is the VM.

---

## 13. Not found / not sure

- **N-1. The direct code generator** (`aot/codegen/AOTDirectCodeGenerator.cpp`) and `AOTLinearScan*` were not read. I assume that they emit the same
  prologue checks and polls through the same generators; not verified.
- **N-2. What keeps addresses 0 .. 16 KB unmapped.** I found no code that reserves them. It seems to rest on the OS (`mmap_min_addr`, page zero).
- **N-3. `AOTEmptyPages`** was named in the task as a no-access range. It is not (4.5).
- **N-4. macOS x86-64 and `s_scratchRegisterBeforeFault`.** With Mach exceptions the lambda runs on the dispatch thread, so the `thread_local` it writes
  is not the one that `operationAOTThrowAtSite` reads on the faulting thread. Either Mach is not used there for a reason I did not find, or the dumped
  `r11` is wrong in that configuration (it matters only if the offending value was in `r11`). Not tested.
- **N-5. Cost of TLS from compiled code.** Nothing in the tree measures or uses it. From general knowledge, to be checked: Linux x86-64 local-exec: one
  `mov reg, fs:[offset]`; Linux ARM64: `mrs reg, TPIDR_EL0` + `ldr`; macOS: a pthread direct key is `gs:[8*key]` or `mrs TPIDRRO_EL0` + `ldr`, while a C++
  `thread_local` goes through a call of the TLV thunk; Windows x64: `gs:[0x58]` -> TLS array -> `_tls_index` -> offset, three dependent loads; Windows ARM64:
  `x18` is the TEB. The image is position independent and "holds addresses in two kinds of places only" (PR text), so a TLS offset would have to come from
  a word of the Instance or the runtime table, or be a link-time constant of the final executable.
- **N-6. How the collector stops a mutator that is in compiled code.** `AOTHintEmitter.cpp:171` says "Handling a trap stops the thread for the collector or
  ends it." I did not find a trap that the collector fires. `Heap::stopIfNecessary` at allocation slow paths and the "mutator has the conn" bit seem to be the
  way. Left to the heap report.
- **N-7. LLInt call sites to compiled callees.** I read the slow path and the virtual trampoline, not how `callHelper` caches the adapter in the site's
  `CallLinkInfo`. Interpreted code (`eval`, `new Function`, `node:vm`) has mutable metadata, call link infos and LLInt inline caches: all the JIT-engine
  hazards apply there. Whether threads may run interpreted code at all is a design decision.
- **N-8.** `AOTStackTrace.cpp` (602 lines), `AOTAsyncStackTrace.cpp`, the second half of `AOTJobs.cpp` (async generator steps) and of
  `AOTStubsCoroutineSteps.cpp`, `AOTStubsConstruct.cpp`, `AOTStubsCallLists.cpp`, `AOTStubsBoxedCalls.cpp` beyond the dispatch were skimmed or not read.
  They use the same words.
- **N-9.** `b3/air/AirFoldTrapsIntoAccesses.cpp` was not read. Which accesses may become faulting null tests, and whether a faulting *store* is possible, I do
  not know. `ThrowKind::NullishWrite` exists.
- **N-10.** Windows: whether a new thread's stack needs more than `preCommitStackMemory` (guard pages, `SetThreadStackGuarantee`) was not looked at.
- **N-11.** FFI callbacks and WebAssembly in the image (`CodeKind::FFI`, `WebAssembly`) enter and leave through the engine's conventions; not read.
- **N-12.** `Instance::awaitMustSuspend` and `mutatorShouldBeFenced`, `barrierThreshold` are bytes and words in the Instance header that other threads (hooks, the
  collector) write while compiled code reads them with plain loads. On one mutator that is fine because the writer is the mutator or the world is stopped. I did
  not check each writer.
