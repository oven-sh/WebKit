SECTION: EMBEDDER
- `5d43ab597d` `WTF::ReadWriteLock` becomes a 16-byte phase-fair lock: an uncontended read lock or unlock is one atomic read-modify-write instead of an exclusive `Lock` acquisition. The message measures 2.2x to 14x the read throughput of `std::shared_mutex` and up to 28x lower write-acquire latency. The nested `ReadLock` and `WriteLock` classes become `ReadLockView` and `WriteLockView` without `tryLock()`, `ParkingLot::UnparkResult::didUnparkThread` becomes `unsigned unparkedCount`, and `ParkingLot::unparkCount(address, count, callback)` is new. Bun does not use `ReadWriteLock` or `ParkingLot`, and JSC uses the lock for `s_destructionLock` in `VM.cpp`. https://bugs.webkit.org/show_bug.cgi?id=325091

SECTION: NOEFFECT
- `7c0df54a06` WebCore site-isolation fix for Web Inspector script profiling. JSC only adds `InspectorEnvironment::forEachDebugger` and a `Debugger::hasProfilingClient` overload. https://bugs.webkit.org/show_bug.cgi?id=323477

SECTION: RUNTIME
- `0506f92aff` `JSON.parse` on a 16-bit source now uses the single-transition check and `JSONTransitionCache` for object keys, which only the 8-bit path used before. `JSONTransitionCache::get` and `add` become templates over the character type, and `Lexer::tryConsumeStringEqualTo` scans 16-bit input with SIMD. `json-parse-utf16-leaf-object` is 1.54x faster, `json-parse-utf16-nested-records` 1.13x, `todomvc-javascript-es5-json-parse` 1.21x and `todomvc-javascript-es6-webpack-json-parse` 1.11x. https://bugs.webkit.org/show_bug.cgi?id=325304

SECTION: GC
- `6730df1f85` To prepare for a `SlotVisitor` that marks more than one heap, `FunctionExecutable`, `JSWeakObjectRef`, `JSFinalizationRegistry` and `JSAPIWrapperObject` take the VM from the cell instead of `visitor.vm()`. The Output marking constraint captures its `Heap`, and `JITWorklist::visitWeakReferences` takes a `VM&`. No behaviour change, and Bun does not call `visitWeakReferences`. https://bugs.webkit.org/show_bug.cgi?id=325294

SECTION: NOEFFECT
- `5bd718a24e` Cocoa CMake port only: `GENERATE_DSYM` option, default OFF. https://bugs.webkit.org/show_bug.cgi?id=323941

SECTION: WASM
- `03befd703d` `Wasm::RTT::tryCreateFunction`, `tryCreateStruct` and `tryCreateArray` allocate with `FastCompactMalloc` instead of `tryFastMalloc`, and `TypeSectionState::m_projectionStorage` does the same. `RTT` declares itself compact-allocated, and the mismatch broke `CompactPointerTuple` only with MTE enabled, which in practice means lockdown mode. https://bugs.webkit.org/show_bug.cgi?id=323513

SECTION: NOEFFECT
- `2407eeea2c` CMake style only: `return()` becomes `return ()` in `Source/cmake`. https://bugs.webkit.org/show_bug.cgi?id=325324

SECTION: RUNTIME
- `3141e1c662` Temporal truncates an invalid calendar id to 100 characters in its `RangeError` message, so the message cannot exceed `String::MaxLength`. A new `canonicalizeCalendar` helper gives every path one text: `new Temporal.PlainDate(2020, 1, 1).withCalendar("foo")` throws `'foo' is not a valid calendar identifier`, where it threw `invalid calendar identifier: foo` before. The other paths now quote the id as written instead of lower-cased. Bun enables Temporal, no existing Bun test matched the old text, and `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` pins the new one. https://bugs.webkit.org/show_bug.cgi?id=325305

SECTION: JIT
- `6dd325c7d0` B3 `specializeSelect` no longer specializes a `Select` when a `Phi` sits between it and the `Check`. The pass cloned that `Phi` into both arms, where no `Upsilon` wrote its slot, so the original `Phi` and the exit state received garbage. Cloning an `Upsilon` stays allowed. https://bugs.webkit.org/show_bug.cgi?id=325312

SECTION: BUILD
- `5869341e6c` `InlineWatchpointSet::add(Watchpoint*)` and both `PropertyTable::clone` overloads become `JS_EXPORT_PRIVATE`. This fixes undefined symbols from WebCore's bindings in the arm64e CMake build. A static JSC link does not change. https://bugs.webkit.org/show_bug.cgi?id=325330

SECTION: EMBEDDER
- `0f3178a96a` The commit retypes bare `const char*` parameters and removes about 160 `legacyCStringPointer()` call sites. `Inspector::RemoteInspectorServer::start`, `RemoteInspectorConnectionClient::connectInet`, `processConfigFile`, `B3::validate` and `FilePrintStream::open` now take a `CStringView`, and `makeDOMAttributeGetterTypeErrorMessage`, `makeDOMAttributeSetterTypeErrorMessage`, `WTFLogChannelByName` and `WTFInitializeLogChannelStatesFromString` take a `StringView`. Bun called two of them: this upgrade changes `src/jsc/modules/BunJSCModule.h` to pass `UTF8CStringView::unsafeFromUTF8(host)` (the name after `df3a6b749b`) to `RemoteInspectorServer::start`, and `src/jsc/bindings/ZigGlobalObject.cpp` to pass the `String` itself to `makeDOMAttributeGetterTypeErrorMessage`. https://bugs.webkit.org/show_bug.cgi?id=325248

SECTION: RUNTIME
- `ec88ec5ecb` The `JSON.parse` lexer factors its SIMD scan into `Lexer::findUnsafeStringCharacter` and uses it in `lexStringSlow` too, so a string keeps the SIMD scan after its first escape. `tryConsumeStringEqualTo` now also matches property keys of `SIMD::stride<uint8_t>` characters or more on an 8-bit source. The message gives no numbers. https://bugs.webkit.org/show_bug.cgi?id=325319

SECTION: RUNTIME
- `7fb452f1a4` `JSONAtomStringCache::makeJSString` adds a 128-slot cache (`m_longJSStrings`) for values of 17 to 256 characters, so `JSON.parse` shares one `JSString` for a repeated long value such as a URL. The slot index hashes the first 8 bytes, the last 8 bytes and the length, and `clearJSStrings()` clears the new cache too. `JSONTransitionCache::transitionIfMatches` compares the key with an inlined `WTF::equal`. Bun's WebKit fork renames `makeJSString` to `tryMakeJSString`, which Bun's `src/jsc/bindings/JSONRowsToJS.cpp` also calls. https://bugs.webkit.org/show_bug.cgi?id=325344

SECTION: NOEFFECT
- `4a435c45f0` WebCore preference `ThreadedTimeBasedAnimationsEnabled` defaults to true. https://bugs.webkit.org/show_bug.cgi?id=325354

SECTION: EMBEDDER
- `8a38069dce` `CString(const char*)` becomes `protected` on every port, where only Cocoa restricted it before. A `CStringWithEncoding` (`UTF8CString`, `Latin1CString`, `ASCIICString`) is now the only way to put bytes into a `CString`. Bun's `src/jsc/bindings/v8/V8CpuProfiler.cpp` assigned `"(root)"` and `""` to `WTF::CString` fields, and this upgrade changes them to `"(root)"_s` and `""_s` on `UTF8CString` fields. https://bugs.webkit.org/show_bug.cgi?id=325142

SECTION: NOEFFECT
- `ce1fa925b8` Safer CPP `const` data members in WebCore and WebKit, plus three in WTF. No behaviour change. https://bugs.webkit.org/show_bug.cgi?id=325268

SECTION: EMBEDDER
- `df3a6b749b` `WTF::CStringView` becomes `UTF8CStringView`, and the header `wtf/text/CStringView.h` becomes `wtf/text/UTF8CStringView.h`. No behaviour change. Bun did not use `CStringView` before, and the `RemoteInspectorServer::start` call that this upgrade rewrites in `src/jsc/modules/BunJSCModule.h` uses the new name. https://bugs.webkit.org/show_bug.cgi?id=325362

SECTION: RUNTIME
- `501d1f661f` `Intl.DurationFormat` keeps the minus sign of a sub-second value between -1 and 0. `new Intl.DurationFormat("en", { seconds: "numeric" }).format({ milliseconds: -500 })` returns `"-0.5"` instead of `"0.5"`, and `formatToParts()` includes a `minusSign` part. `buildDecimalFormat` passes the nanoseconds to ICU with a decimal exponent (`-500000000E-9`) instead of a split integer and fraction. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the new result. https://bugs.webkit.org/show_bug.cgi?id=325247

SECTION: EMBEDDER
- `bffe36ec16` `String.prototype.match`, `matchAll`, `replace`, `replaceAll`, `search`, `split` and `RegExp.prototype.test`, `@@match`, `@@matchAll`, `@@search`, `@@split` no longer take the fast path for a RegExp of another realm, and DFG no longer inlines these functions from another realm. `"abc".match(otherRealmRegExp)` now returns an array of the RegExp's realm and updates that realm's `RegExp.lastMatch`, where it used the caller's realm before. `RegExpObject::isSymbolMatchFastAndNonObservable` and its four siblings, and `regExpExecWatchpointIsValid`, now take the `JSGlobalObject*` to check against. Bun does not call them, and `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` pins the new result for a `node:vm` RegExp. https://bugs.webkit.org/show_bug.cgi?id=325134

SECTION: JIT
- `ee4792cb07` FTL stores its OSR exits in a compact `FTL::OSRExitStream` instead of one 56-byte `FTL::OSRExit` per exit, the counterpart of `DFG::OSRExitStream`. One JetStream3 run with 166k exits, of which 0.5% ever fire, drops from 9.3 MB to 2.0 MB, which is 12.1 B per exit. An exit that fires for the first time decodes the stream from the start, and the code origin encoding moves to `DFGOSRExitBase` for both tiers. https://bugs.webkit.org/show_bug.cgi?id=324517

SECTION: NOEFFECT
- `23e2202805` WebCore preference `MutationEventsEnabled` and layout tests. https://bugs.webkit.org/show_bug.cgi?id=325358

SECTION: NOEFFECT
- `9cf419e5c4` Apple-only `JavaScriptCoreTools` corpse tooling: new `Corpse::Memory`. https://bugs.webkit.org/show_bug.cgi?id=325293

SECTION: EMBEDDER
- `101392f63c` `AbstractSlotVisitor` loses `vm()`, `heap()` and its `Heap&`, so a visitor only knows its `Collector`. It gains `collectionScope()` and `heapAnalyzer()`, whose values the `Collector` supplies through `didStartMarking(CollectionScope, HeapVersion, HeapAnalyzer*)`, and the constructors become `SlotVisitor(Collector&, ASCIICString)` and `VerifierSlotVisitor(Collector&)`. `sizeof(SlotVisitor)` stays at 256, and behaviour does not change. Bun's `GlobalObject::visitChildrenImpl` in `src/jsc/bindings/ZigGlobalObject.cpp` called `visitor.vm()`, and this upgrade changes it to `cell->vm()`. https://bugs.webkit.org/show_bug.cgi?id=325317

SECTION: GC
- `dfdac830e3` `MachineThreads::gatherFromCurrentThread` becomes `gatherFromConductor`, and `gatherConservativeRoots` names its `CurrentThreadState*` and `Thread*` parameters `conductingMutatorState` and `conductorThread`. The conservative scan runs in the `Cs` constraint on any marking thread, so "current thread" was misleading. No behaviour change. https://bugs.webkit.org/show_bug.cgi?id=325325

SECTION: GC
- `868ef79407` `Heap::didDiscoverPendingWasmCallee` becomes `markWasmCalleeIfPending`, `prepareWasmCalleeCleanup` becomes `beginMarkingWasmCallees`, `finalizeWasmCalleeCleanup` becomes `releaseUnmarkedWasmCallees`, and `m_wasmCalleesDiscoveredDuringGC` becomes `m_wasmCalleesFoundOnStacks`. No behaviour change, and Bun calls none of them. https://bugs.webkit.org/show_bug.cgi?id=325327

SECTION: NOEFFECT
- `1623de9877` WebCore accessibility: `AXFormActivityMonitor`. https://bugs.webkit.org/show_bug.cgi?id=324769

SECTION: JIT
- `45d9af505e` `ByteCodeParser::handleIteratorNext` adds an `ObjectUse` check on the iterator before the generic `next` call. Since `35637c6433` in this range, `op_iterator_open` stores a `JSSentinel` in `iterator` for an Array, and a for-of loop that also saw a generic iterator could inline a `next` with `ToThis` and hit the assertion in `isToThisAnIdentity`. https://bugs.webkit.org/show_bug.cgi?id=325416

SECTION: WASM
- `987be10500` On Windows, creating or growing a Wasm memory, a resizable `ArrayBuffer` or a growable `SharedArrayBuffer` at the commit limit now throws a `RangeError` or returns -1 from `memory.grow()` instead of crashing. These paths commit through the new `BufferMemoryManager::tryMakeReadableAndWritable`, which calls `OSAllocator::tryProtect` on Windows and `OSAllocator::protect` elsewhere. `OSAllocator::tryProtect` retries a commit that hits the limit, as libpas does. The crash came from `653b77d5bd` in this range, which made Windows reserve the range and commit on demand. https://bugs.webkit.org/show_bug.cgi?id=325406

SECTION: JIT
- `33ca66ce3f` B3 `ReduceStrength` replaces `PureCSE` with dominator-order value numbering on the new `WTF::LayeredHashMap`, and its first run also forwards WasmGC struct and array loads with per-field epochs. The WasmGC handling leaves `eliminateCommonSubexpressions`, whose walk over all reachable blocks made WasmGC compiles slow, and the message reports neutral performance with lower compile time. `ReduceStrength` forwards a read only from an access that dominates it, so Phi synthesis for reads and forward store elimination, which fired 3, 9 and 55 times in the three WasmGC subtests, are gone. New option `maxB3WasmGCEpochSnapshotEntries` (default 1024 * 1024) sets the size above which all struct fields share one counter. https://bugs.webkit.org/show_bug.cgi?id=324906

SECTION: EMBEDDER
- `93ebfacfd7` `SourceProvider::codeBlockHashConcurrently` is no longer virtual and becomes `const`. The new virtual `SourceProvider::withSourceConcurrently(const ScopedLambda<void(StringView)>&) const` is the override point for a provider whose `source()` is main-thread only, and `documentLineColumnForOffset()` builds its line table through it so it can run on any thread. `positionInfoForOffset()` and `offsetForPosition()` now assert that they do not run on a GC thread. Bun's providers (`Zig::SourceProvider` in `src/jsc/bindings/ZigSourceProvider.h`, `GraphCommonJSSourceProvider`, the Bake providers) never overrode `codeBlockHashConcurrently` and keep the default, which calls `source()`. https://bugs.webkit.org/show_bug.cgi?id=324838

SECTION: EMBEDDER
- `f2ca04538e` `WTF::CString` becomes `CStringBase` with protected constructors, assignment operators and destructor, so it only exists as the base of `CStringWithEncoding` (`UTF8CString`, `Latin1CString`, `ASCIICString`). `CString::newUninitialized()` becomes `CStringBase::allocateUninitialized()`, the commit removes `CStringHash`, `DefaultHash<CString>` and `HashTraits<CString>`, and comparing strings of different encodings no longer compiles. Bun declared untyped `WTF::CString` values in 22 files under `src/jsc/bindings` and `src/runtime/webview` (for example `BunProcess.cpp`, `Secrets.h`, `BunString.h`), and this upgrade changes them to `UTF8CString` or `Latin1CString` and reads a `const char*` through `legacyCStringPointer()`. A follow-up in this range, `22e1cc5592`, renames `CStringWithEncoding` to `CString`. https://bugs.webkit.org/show_bug.cgi?id=325361
