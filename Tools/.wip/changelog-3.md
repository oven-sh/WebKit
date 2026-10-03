SECTION: JIT
- `bb73af4a4b` Baseline JIT emits the `FastArray` path of `op_iterator_next` inline in `JIT::emit_op_iterator_next()` when the array has Int32 or Contiguous shape. It compares the index with the length, loads the element and increments the index without a call to `operationIteratorNextFastArray`. Other shapes, holes and the end of the iteration still call the operation. With `JSC_useDFGJIT=false`, 14 for-of and array destructuring microbenchmarks are 1.09x to 1.86x faster, for example `for-of-array` 1.11x, `for-of-iterate-array-values` 1.60x and `default-value-destructuring-array` 1.86x. https://bugs.webkit.org/show_bug.cgi?id=325235

SECTION: NOEFFECT
- `780fb0070b` GLib port only, `JSCException.cpp` in `API/glib`. https://bugs.webkit.org/show_bug.cgi?id=325402

SECTION: NOEFFECT
- `2caf6d6e11` WPE port only, removes the `ENABLE_COG` option from `OptionsWPE.cmake`. https://bugs.webkit.org/show_bug.cgi?id=325424

SECTION: NOEFFECT
- `a5815bc979` GLib port only, `JSCClass.cpp` in `API/glib`. https://bugs.webkit.org/show_bug.cgi?id=325403

SECTION: EMBEDDER
- `22e1cc5592` WTF renames the class template `CStringWithEncoding<CharacterType>` to `CString<CharacterType>` and `CStringWithEncodingHash` to `CStringHash`. The aliases `UTF8CString`, `Latin1CString` and `ASCIICString` keep their names. The bare name `CString` is now a template, so a declaration of the old untyped class such as `Vector<CString>` or a `const CString&` parameter does not compile. Bun never names `CStringWithEncoding`, and this upgrade changes its untyped `WTF::CString` declarations to `UTF8CString` or `Latin1CString` in 22 files under `src/jsc/bindings` and `src/runtime/webview`. https://bugs.webkit.org/show_bug.cgi?id=325460

SECTION: JIT
- `73af864d11` DFG and FTL compile a HeapBigInt comparison against the constant `0n` inline. `==`, `!=`, `===` and `!==` become a pointer comparison because one `0n` instance exists per VM, `<` and `>=` test the sign bit, and `<=` and `>` combine both checks. `Node::isHeapBigIntZeroConstant()`, `SpeculativeJIT::tryCompileHeapBigIntCompareWithZero()` and `compareHeapBigIntWithZero()` in `FTLLowerDFGToB3.cpp` implement it. https://bugs.webkit.org/show_bug.cgi?id=325432

SECTION: WTF
- `1bb8f8abf2` `CStringBase` uses `WTF_MAKE_TZONE_ALLOCATED_EXPORT` instead of `WTF_DEPRECATED_MAKE_FAST_ALLOCATED`, and `CString<CharacterType>` uses `WTF_MAKE_INHERITED_TZONE_ALLOCATED`. Without `USE(TZONE_MALLOC)`, as in a mimalloc build, the new macro expands to the old one and allocation does not change. https://bugs.webkit.org/show_bug.cgi?id=325461

SECTION: EMBEDDER
- `1ffb8e9a91` B3 `ReduceStrength` drops its fixpoint mode (`runFixpoint()`) and always runs a single pass. The debugging option `useB3ReduceStrengthFixpoint` (default false) is removed from `OptionsList.h`. Bun does not set this option. https://bugs.webkit.org/show_bug.cgi?id=325459

SECTION: WTF
- `c06ff4e993` `WTF::Deque` gains `constructAndAppend(Args&&...)`, matching `Vector::constructAndAppend()`, which constructs the new element in place at the end of the deque. `Deque::append()` delegates to it. https://bugs.webkit.org/show_bug.cgi?id=325441

SECTION: EMBEDDER
- `f62059d547` The `VM` members `jsonAtomStringCache` and `jsonTransitionCache` merge into one heap-allocated `JSONCache`, reached through `VM::jsonCache()`. `JSONCache.h` and `JSONCacheInlines.h` replace `JSONAtomStringCache.h`, `JSONTransitionCache.h` and their `Inlines` headers. `makeIdentifier()`, `existingIdentifier()` and `makeJSString()` take a `VM&` first argument, `clear()` becomes `clearStrings()`, and the transition `get()` and `add()` become `getTransition()` and `addTransition()`. Bun's `src/jsc/bindings/JSONRowsToJS.cpp` used `vm.jsonAtomStringCache`, and this upgrade changes it to `vm.jsonCache()` with the new include and the `VM&` argument. https://bugs.webkit.org/show_bug.cgi?id=325483

SECTION: GC
- `36d446cb4f` `Collector::Collector()` builds the parallel `SlotVisitor`s, picks the `MutatorScheduler`, prepares the collector slot visitor and creates the `CollectorThread`, and `Collector::~Collector()` clears the race mark stack. `Heap::Heap()` and `Heap::~Heap()` no longer do this work. `Collector::startThread()` is removed, and `~Heap()` stops clearing each slot visitor's mark stacks because `~SlotVisitor` does it. No change in behaviour. https://bugs.webkit.org/show_bug.cgi?id=325434

SECTION: EMBEDDER
- `155da27992` `RunLoop::dispatchAfter(Seconds, Function<void()>&&)`, which returns a `Ref<RunLoop::DispatchTimer>`, is renamed to `RunLoop::scheduleTimer()`. `dispatchAfter()` becomes a pure virtual of `GuaranteedSerialFunctionDispatcher` that returns `void`, with overrides in `RunLoop`, `WorkQueue` and `MainThreadDispatcher`. JSC's `WaiterListManager::waitAsyncImpl()` now calls `scheduleTimer()`, and the rest of the commit is an iOS media fix in WebCore. Bun calls neither function and has no `GuaranteedSerialFunctionDispatcher` subclass. https://bugs.webkit.org/show_bug.cgi?id=324711

SECTION: WASM
- `3087a95539` With `verboseWasmDebugger` on, `collectCallStack()` in `wasm/debugger/WasmDebugServerUtilities.cpp` no longer reads a name off the null `CodeBlock` of a host function frame, a read that crashes the debug server thread. It logs the `NativeExecutable` name for such a frame instead. `WebAssembly.promising` and `JSON.stringify` with a wasm `toJSON` reach this path. https://bugs.webkit.org/show_bug.cgi?id=325518

SECTION: RUNTIME
- `e5517023f3` `LiteralParser::materializeArray()` copies the elements of a `JSON.parse` array in bulk by indexing shape instead of one `initializeIndex()` call per element. Double arrays store raw doubles, Int32 arrays use `memcpy`, and Contiguous arrays use `gcSafeMemcpy` plus one write barrier. Other shapes keep the per-element loop. https://bugs.webkit.org/show_bug.cgi?id=325436

SECTION: EMBEDDER
- `2c5cd6d774` `CStringBase::legacyCStringPointer()` is removed. `UTF8CString::legacyCStringPointer()` is the only remaining form, and `ASCIICString::data()` already returns `const char*`. Bun calls `legacyCStringPointer()` only on `UTF8CString` values, so it needs no change for this commit. https://bugs.webkit.org/show_bug.cgi?id=325471

SECTION: WASM
- `840c8efcbd` The WASM debugger answers `qMemoryRegionInfo` for an Invalid-type address with an unmapped region instead of an `InvalidAddress` error, because LLDB turns region info off for the rest of the session after one error reply. A mapped memory or module region reports its window base as the start instead of the queried address, so `qMemoryRegionInfo:10` no longer describes `[0x10, 0x1010010)`. The last module gap gains the one byte that `INVALID_END - address` leaves out. https://bugs.webkit.org/show_bug.cgi?id=325534

SECTION: NOEFFECT
- `cae8ae4bce` WebKit and WebCore drag handling, plus Swift-only flags in `WebKitSwiftFlags.cmake`. https://bugs.webkit.org/show_bug.cgi?id=325315

SECTION: GC
- `5e53548c66` The `MarkingConstraintSet` moves from `Heap` to `Collector`, which exposes `Collector::addMarkingConstraint()` and asserts that no collection is running. `MarkingConstraintSet` drops its unused `Heap&` and the default arguments of `add()`, and `MarkingConstraintSolver` reaches the `Collector` through its main `SlotVisitor`. `Heap::addMarkingConstraint()`, which Bun's `src/jsc/bindings/BunClientData.cpp` calls, keeps its signature. No change in behaviour. https://bugs.webkit.org/show_bug.cgi?id=325513

SECTION: NOEFFECT
- `4b3653d2ed` Logging only. JSC disassembler comments and Wasm plan dumps print UTF-8 instead of lossy ASCII. https://bugs.webkit.org/show_bug.cgi?id=325477

SECTION: EMBEDDER
- `1d03118dfe` `Heap::lastChanceToFinalize()` splits in two. The new public `Heap::shutDown()`, which `VM::~VM()` now calls, stops collection, stops the collector thread through `Collector::stopThread()` and then calls `lastChanceToFinalize()`, which only finalizes and is now private. Bun does not call `lastChanceToFinalize()`, it only names it in comments, so it needs no change. https://bugs.webkit.org/show_bug.cgi?id=325527

SECTION: NOEFFECT
- `f4478d48f8` bmalloc `BCRASH()` static analyzer annotation name, no code change. https://bugs.webkit.org/show_bug.cgi?id=325557

SECTION: JIT
- `be0094718d` `tryConvertToInt52()` in `JSCJSValue.h` uses one conversion instruction on ARM64 (`vcvtd_s64_f64`) and x86-64 (`_mm_cvttsd_si64`) and validates the result with a bit-pattern round trip. `ArithProfile` no longer records `Int52Overflow` for every double result, only for a value outside the Int52 range (`ObservedResults::isInt52Overflow()`, `ArithProfile::emitSetInt52OverflowIfNeeded()`). B3 `ReduceStrength` turns `SShr(Mul(value, constant << amount), amount)` into `Mul(value, constant)` when range analysis proves that the `Mul` cannot overflow. `AssemblyHelpers::isStrictInt52()` uses `extractSignedBitfield64` on ARM64. https://bugs.webkit.org/show_bug.cgi?id=325457

SECTION: EMBEDDER
- `4cd0b19673` `CStringBase::data()` becomes protected, so a `const CStringBase&` no longer hands out a `const char*`. `CString<CharacterType>::data()` stays public and typed, and `safePrintfType(const CStringBase&)` reads `spanIncludingNullTerminator()` instead. Bun never takes a `const CStringBase&`. Where it passed `data()` of an untyped `CString` to a C API (`dlopen()` in `src/jsc/bindings/BunProcess.cpp`, libsecret in `SecretsLinux.cpp`, `sqlite3_file_control()` in `sqlite/JSSQLStatement.cpp`), this upgrade changes the call to `UTF8CString::legacyCStringPointer()`. https://bugs.webkit.org/show_bug.cgi?id=325572

SECTION: NOEFFECT
- `3941d977dc` WebCore static analysis fixes, plus a `CGDataProvider` trait in `wtf/cf/CFTypeTraits.h` under `USE(CG)`. https://bugs.webkit.org/show_bug.cgi?id=325468

SECTION: NOEFFECT
- `1869d0c19a` Apple ports only, `ENABLE_BACK_FORWARD_LIST_SWIFT` on iOS 26.5 SDKs. https://bugs.webkit.org/show_bug.cgi?id=325157

SECTION: NOEFFECT
- `f23dc1e557` WebCore preference `WebRTCWebCoreVideoCodecsEnabled` only. https://bugs.webkit.org/show_bug.cgi?id=325476

SECTION: JIT
- `fb512fb75f` `FTL::OSRExitStream` stores the `B3::ValueRep`s of each OSR exit right after the exit in its byte stream. The separate `JITCode::osrExitValueReps` stream, the `OSRExitValueReps` class and the per-exit offset are removed. One JetStream3 run with 167k exits shrinks from 4.14 MB to 3.96 MB, or from 24.8 to 23.8 bytes per exit. https://bugs.webkit.org/show_bug.cgi?id=325463

SECTION: RUNTIME
- `bef9d7e632` `toDouble()` in `JSGlobalObjectFunctions.cpp`, the string path of ToNumber, returns NaN as soon as the first character after white space is not a digit, `.`, `+`, `-` or `I`. A string such as `"rlineto"` no longer reaches the decimal literal parser. The results of `+value`, `Number(value)` and `==` do not change. https://bugs.webkit.org/show_bug.cgi?id=325591

SECTION: RUNTIME
- `7fbe85cfbf` A tagged template whose tag is a parenthesized optional chain now receives the base of the chain as `this`. With `const o = { b() { return this } }`, ``(o?.b)`x` `` returns `o` instead of `undefined`. `TaggedTemplateNode::emitBytecode()` loads the callee and `this` through `OptionalChainNode::emitCallee()`, as calls do since 63b7d629dd. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the new result. https://bugs.webkit.org/show_bug.cgi?id=325401

SECTION: EMBEDDER
- `96fed6d5b4` `JSObject::fillGetterPropertySlot()` and `fillCustomGetterPropertySlot()` now fill a cacheable `PropertySlot` for an accessor on an uncacheable dictionary, so `tryCacheGetBy` flattens the object and caches the getter as it already does for data properties. A getter read from a CommonJS exports object with 64 or more re-exported names (TypeScript `export { X } from "./x"`, as in rxjs, mongodb and graphql) no longer takes the generic slow path in every tier, and `getter-on-uncacheable-dictionary` is 24.3x faster. The `PropertySlot::setCustom()` overload that takes a `DOMAttributeAnnotation` is removed. Bun never calls `setCustom()`, and both branches of the `slot.isCacheableGetter()` checks in `JSC__JSValue__forEachPropertyImpl` (`src/jsc/bindings/bindings.cpp`) yield the `GetterSetter`, so its result does not change. https://bugs.webkit.org/show_bug.cgi?id=325245

SECTION: BUILD
- `75930ee50a` `Source/cmake/WebKitCompilerFlags.cmake` adds `-Wno-unused-templates` to the global compiler flags for GCC and Clang. Clang 23.1.0 reports many used templates as unused, so WebKit turns the warning off instead of adapting to `-Werror=unused-templates`. https://bugs.webkit.org/show_bug.cgi?id=325160
