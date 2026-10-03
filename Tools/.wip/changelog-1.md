SECTION: EMBEDDER
- `35637c6433` `for...of` and array destructuring over an Array (`FastArray` mode) no longer allocate a `JSArrayIterator` in LLInt, Baseline and DFG. `op_iterator_open` keeps `VM::fastArraySentinel()` in `iterator` and an Int32 index in `next`, and the new opcode `op_iterator_close_check` creates the iterator only when `arrayIteratorProtocolWatchpointSet` is invalid at close. The bytecode format changes, and Bun needs no source edit because `getWebKitBytecodeCacheVersion()` in `src/jsc/bindings/ZigGlobalObject.cpp` keys the cache on the WebKit version. Bun's WebKit fork already had this optimization under `Options::useUnboxedFastArrayIteration`. https://bugs.webkit.org/show_bug.cgi?id=324224

SECTION: WASM
- `5e551b0a24` `OMGIRGenerator::addArguments` and `addReturn` pass the frame pointer offset to the `B3::MemoryValue` load or store instead of emitting a separate `B3::Add`. B3 already folded that `Add` in a later phase, so only the generated IR gets smaller. https://bugs.webkit.org/show_bug.cgi?id=324945

SECTION: NOEFFECT
- `8591160211` Safer C++ checker expectations only. https://bugs.webkit.org/show_bug.cgi?id=325119

SECTION: JIT
- `d9fb479bd0` `B3::generateToAir` runs `fixSSA` at every opt level, so no `Get` or `Set` reaches `B3LowerToAir`. `B3LowerToAir` drops its variable lowering and release-asserts that the procedure has no variables. Code at the default opt level 2 does not change. https://bugs.webkit.org/show_bug.cgi?id=325079

SECTION: NOEFFECT
- `c150a5007f` WebCore CSS `corner` shorthands and their web preference. https://bugs.webkit.org/show_bug.cgi?id=325010

SECTION: EMBEDDER
- `165f9ed4d0` `WTF::CString(std::span<const char>)` becomes protected, and `CString::buffer()` and `CString(CStringBuffer*)` go away. `CString(const char*)` becomes protected only under `PLATFORM(COCOA)`, which Bun's JSCOnly build does not define. `Profiler::Database::save()` and `registerToSaveAtExit()` take `const UTF8CString&`, `Printer::setPrinter()` takes `UTF8CString&&` and `registerLabel()` takes `ASCIICString&&`. Bun does not use any of these, and the `CString` edits in Bun's `src` come from later commits of this series. https://bugs.webkit.org/show_bug.cgi?id=324955

SECTION: JIT
- `c3148b98af` `FTL::Output::branch` and `Output::switchInstruction` emit a `Jump` when the condition is a constant. `OMGIRGenerator` folds constant conditions in `addIf`, `addBranch`, `addSwitch` and `addSelect`, and reads a local that still holds a known constant without a `Get`. `B3ReduceStrength` folds a `Switch` on a constant into a `Jump`. Its conversion of a `Check` that always exits now clears the successors of the block, which fixes a B3 validation failure. https://bugs.webkit.org/show_bug.cgi?id=325097

SECTION: NOEFFECT
- `46991b930d` WPE port build option `ENABLE_WPE_PLATFORM`. https://bugs.webkit.org/show_bug.cgi?id=325147

SECTION: EMBEDDER
- `c2fa192760` A new `JSC::Collector` class (`heap/Collector.h`, reached through `Heap::collector()`) takes the collection-cycle state out of `Heap`: the marking machinery, the phase machine, the collector thread and the request queue. `Heap::runFunctionInParallel()`, `runTaskInParallel()` and `forEachSlotVisitor()` move to `Collector`, `Heap::Ticket` becomes `GCRequest::Ticket`, `Heap::phaseVersion()` and `increaseLastFullGCLength()` go away, and the `SlotVisitor`, `AbstractSlotVisitor` and `VerifierSlotVisitor` constructors take a `Collector&`. The commit states no change in behavior. Bun does not use any of the moved or removed members. https://bugs.webkit.org/show_bug.cgi?id=325056

SECTION: WASM
- `a8c7000ca6` `OMGIRGenerator` stops calling `addPredecessor` when it wires B3 blocks, because `Procedure::resetReachability` recomputes the predecessors. Generated code does not change. https://bugs.webkit.org/show_bug.cgi?id=325101

SECTION: WTF
- `9679818e58` `SparseBitVector::clear()`, `URLParser` (`m_asciiBuffer`) and Air's `GenerateAndAllocateRegisters::prepareForGeneration` empty reused vectors with `shrink(0)` instead of `clear()`, which keeps the capacity and avoids a free and a malloc on each refill. The gains in the message (+7.1%, +4.3% and +18.4% on HTML parser and DOM benchmarks) come from WebCore call sites. https://bugs.webkit.org/show_bug.cgi?id=325059

SECTION: NOEFFECT
- `ce4d34e807` Cocoa-port CMake entitlement generation. https://bugs.webkit.org/show_bug.cgi?id=325044

SECTION: NOEFFECT
- `da746c2237` WebCore preference `CSSLinkParametersEnabled` on by default. https://bugs.webkit.org/show_bug.cgi?id=325042

SECTION: NOEFFECT
- `86a2b88ebc` Cocoa-port CMake XPC service definitions. https://bugs.webkit.org/show_bug.cgi?id=323941

SECTION: RUNTIME
- `b718c65267` `JSON.parse` guesses that the next key of an object is the property name of the single transition of its `Structure`. `LiteralParser::Lexer::tryConsumeStringEqualTo` compares the 8-bit source with that name with SIMD (names under 16 characters), which skips key lexing and atomization, and `Lexer::consumeColon` peeks for `:`. The property store uses `putDirectWithoutBarrier` and `setStructureIDDirectly` with one write barrier. This targets arrays of same-shaped objects such as `[{"name":"hello","value":42},{"name":"world","value":45}]`. https://bugs.webkit.org/show_bug.cgi?id=325179

SECTION: NOEFFECT
- `e7d44093fc` Cocoa-port CMake Info.plist and code signing cleanup. https://bugs.webkit.org/show_bug.cgi?id=324579

SECTION: EMBEDDER
- `653b77d5bd` On Windows, `Options::notifyOptionsChanged()` no longer forces `useWasmFastMemory` off. `BufferMemoryManager` reserves fast and growable memories with `OSAllocator::tryReserveUncommitted`, `Wasm::Memory::tryCreate` and resizable `ArrayBuffer` allocation commit only the bytes in use, and the no-access branch of `OSAllocator::tryProtect` stops committing the 4 GiB guard region. Bun sets neither `useWasmFastMemory` nor `useWasmFaultSignalHandler`, so the new default applies to Bun on Windows, and `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` checks that an out-of-bounds load traps in every tier. 987be10500, later in the range, turns the crash at the Windows commit limit that this introduces into an out-of-memory error. https://bugs.webkit.org/show_bug.cgi?id=304154

SECTION: NOEFFECT
- `18e3f9366c` Safer C++ checker expectations only. https://bugs.webkit.org/show_bug.cgi?id=325227

SECTION: EMBEDDER
- `1f47117433` `Promise.prototype.then`, `catch` and `finally` take their fast path only for a promise of the current realm, and DFG no longer inlines `Promise.prototype.then`, `Promise.resolve` and `Promise.reject` of another realm. `Promise.prototype.then.call(otherRealmPromise, f)` used to return a promise of the calling realm and now runs the species lookup, which returns a promise of the other realm. The exported `JSPromise::then(JSGlobalObject*, ...)` has the same `realm()` check. Bun's call in `src/jsc/bindings/NodeVM.cpp` passes the promise's own realm and needs no edit, and `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the `node:vm` case. https://bugs.webkit.org/show_bug.cgi?id=325110

SECTION: EMBEDDER
- `63b7d629dd` `(a?.b)(x)` parses as `Call(OptionalChain(a.b), x)`, so the call stays outside the optional chain, and the new `OptionalChainNode::emitCallee` keeps `a` as `this`. When `a` is nullish, JSC used to skip the argument and the call and return `undefined`, and now it evaluates `x` and throws a `TypeError`. `(a?.b)() = 1` in sloppy mode becomes a runtime `ReferenceError` instead of a `SyntaxError`, and `x = (o?.f)()` in a class field initializer no longer drops the call. Bun's printer keeps these parentheses (`src/js_printer/lib.rs`), so the change reaches user code, and `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` pins it. https://bugs.webkit.org/show_bug.cgi?id=325104

SECTION: RUNTIME
- `12ea13fb7c` `JSON.parse` without a reviver peeks one character instead of calling the full `Lexer::next()`. After a value, `Lexer::nextAfterValue()` consumes `,`, `}` or `]` directly, and a property value that starts with `"`, `-` or a digit goes straight to `Lexer::nextString()` or `Lexer::nextNumber()`. Whitespace falls back to `next()`. Bun's WebKit fork also records `m_positionAfterLastToken` in `nextAfterValue()`, because `Bun.JSONL` (`LiteralParser::tryStreamingParse`) reads it. https://bugs.webkit.org/show_bug.cgi?id=325207

SECTION: EMBEDDER
- `71d5b30126` The `createJSString()` overloads in `API/JSStringRefCPP.h` return `RefPtr<OpaqueJSString>` instead of `JSRetainPtr<JSStringRef>`, and the header no longer includes `JSRetainPtr.h`. A new header, `API/JSStringRefPtr.h`, specializes `DefaultRefDerefTraits<OpaqueJSString>` with `JSStringRetain()` and `JSStringRelease()`. Bun does not use `JSRetainPtr` or `createJSString()`. https://bugs.webkit.org/show_bug.cgi?id=325138

SECTION: RUNTIME
- `ee8e5dbbcc` `Intl.DurationFormat` no longer drops a unit whose own field is 0 when smaller units fold into it as a fraction. `collectElements` in `IntlDurationFormat.cpp` now tests the value including the fraction. `new Intl.DurationFormat("en", { milliseconds: "numeric" }).format({ milliseconds: 500 })` returns `"0.5 sec"` instead of `""`, and `{ hours: 1, milliseconds: 500 }` formats as `"1 hr, 0.5 sec"` instead of `"1 hr"`. https://bugs.webkit.org/show_bug.cgi?id=325238

SECTION: NOEFFECT
- `ceba48c3cd` Safer C++ checker expectations only. https://bugs.webkit.org/show_bug.cgi?id=325253

SECTION: NOEFFECT
- `669358d70d` WebKitLegacy Safer C++ cleanup. https://bugs.webkit.org/show_bug.cgi?id=325132

SECTION: NOEFFECT
- `17d7e3affd` WebCore CSS `scroll-axis-lock` parsing. https://bugs.webkit.org/show_bug.cgi?id=325239

SECTION: WASM
- `caa6708421` For a Wasm module loaded as an ES module, `JSWebAssemblyInstance::tryCreate` applies the `wasm-js:` module and reserved import name `LinkError` checks only to public imports. It skips string-constant imports and recognized builtins first, so a string constant named `wasm:hello` links instead of failing. Bun's module loader does not hand Wasm to JSC as an ES module, so Bun does not reach this path. https://bugs.webkit.org/show_bug.cgi?id=325162

SECTION: GC
- `59be6c1244` The `Collector` methods move from `Heap.cpp` to a new `heap/Collector.cpp`, and `Collector::forEachSlotVisitor()` moves from `HeapInlines.h` to a new `heap/CollectorInlines.h`. The moved logging uses `CollectorInternal::verbose`. No functional change. https://bugs.webkit.org/show_bug.cgi?id=325167

SECTION: NOEFFECT
- `e512535443` Cocoa Security SPI rename. https://bugs.webkit.org/show_bug.cgi?id=325168

SECTION: RUNTIME
- `1e80c1e3ef` `VM` gains `jsonTransitionCache`, a `JSONTransitionCache` with 256 primary and 64 secondary entries that maps a `Structure` and an 8-bit property name to the existing property addition transition. `LiteralParser::parseRecursively` consults it in strict JSON mode when the `Structure` has no single transition, which is the case b718c65267 misses, for example the empty object. The cache stores its entries without write barriers: the GC marks them while a parse runs (`ParsingScope`) and clears the table at the end of any other collection. f62059d547, later in the range, folds it into `JSONCache`. https://bugs.webkit.org/show_bug.cgi?id=325249

SECTION: EMBEDDER
- `ba4ffde846` `Options::useWasmWideArithmetic` now defaults to true: its source, `WasmWideArithmeticEnabled` in `UnifiedWebPreferences.yaml`, goes from `testable` and `false` to `stable` and `true`. Modules that use `i64.add128`, `i64.sub128`, `i64.mul_wide_s` or `i64.mul_wide_u` validate and run instead of failing with "wasm wide arithmetic is not enabled". Bun does not set this option in `JSCInitialize` (`src/jsc/bindings/ZigGlobalObject.cpp`), so Bun gets the new default, and `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers it. https://bugs.webkit.org/show_bug.cgi?id=325219
