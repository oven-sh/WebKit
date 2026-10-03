SECTION: RUNTIME
- `9ef3e2eb29` `JSObject::setPrototypeDirect()` calls `JSGlobalObject::haveABadTime()` only when the object and its old prototype chain could not already intercept indexed accesses. Before, it called it every time an object that may be a prototype got such a chain, for example on each retargeting of a `JSGlobalProxy`, which intercepts indexed accesses itself. https://bugs.webkit.org/show_bug.cgi?id=325587

SECTION: BUILD
- `fce5f76375` `Source/cmake/WebKitCompilerFlags.cmake` passes `-Wno-unused-template` in place of the misspelled `-Wno-unused-templates` that 75930ee50a added earlier in this range. The flag turns off `-Wunused-template`, which the comment in the file calls broken with Clang 23.1 because of false positives. https://bugs.webkit.org/show_bug.cgi?id=325655

SECTION: JIT
- `5330149c55` `for...of` and destructuring over a String no longer allocate a `JSStringIterator` in LLInt, Baseline and DFG. In `IterationMode::FastString`, `op_iterator_open` keeps the sentinel `VM::fastStringSentinel()` in `iterator` and an Int32 index in `next`, and `op_iterator_close_check` creates the `JSStringIterator` only when `stringIteratorProtocolWatchpointSet` is no longer valid. The four string microbenchmarks run 1.06x to 1.20x faster with `JSC_useDFGJIT=false` and 1.21x to 1.29x faster with `JSC_useFTLJIT=false`. The commit renames `VM::fastStringValuesSentinel()` to `VM::fastStringSentinel()`, which Bun does not call, and Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers String iteration. https://bugs.webkit.org/show_bug.cgi?id=325452

SECTION: NOEFFECT
- `36f536ea84` Cocoa-only WebKit IPC cleanup, removes `HAVE_WK_SECURE_CODING_NSURLREQUEST` from `wtf/PlatformHave.h`. https://bugs.webkit.org/show_bug.cgi?id=325555

SECTION: WTF
- `c8cb35beac` libpas serves MTE-tagged allocations from a separate `tagged_bmalloc_heap`, and the `bmalloc::api` functions in `bmalloc.h` dispatch between it and `bmalloc_heap`. The libpas allocation functions lose their `pas_allocation_mode` parameter, and the commit removes `pas_allocation_mode.h`. The inline part of `fastMalloc` shrinks from 141 to 73 instructions, and the inline part of `fastFree` grows from 44 to 45. Bun's non-ASAN builds pass `USE_MIMALLOC=ON` to the WebKit build, which turns libpas off. https://bugs.webkit.org/show_bug.cgi?id=317893

SECTION: WTF
- `35ba3364d3` `ParkingLot::unparkCount(address, count, callback)`, which 5d43ab597d added earlier in this range, no longer accepts a count of 0. `ParkingLot::unparkCountImpl()` now hits `RELEASE_ASSERT(count)`, where before it ran the callback under the queue lock and unparked no thread. The two-argument `ParkingLot::unparkCount(address, 0)` still returns 0. Bun does not call `ParkingLot`. https://bugs.webkit.org/show_bug.cgi?id=325596

SECTION: JIT
- `4f03ef594d` DFG `FixupPhase` converts `CompareEq` into `CompareStrictEq` when one operand speculates String and the other speculates String or null/undefined. It adds a `Check` with `StringOrOtherUse` on the latter and fixes the former to `StringUse`, and under these types `==` and `===` give the same result. The pattern comes from code like `str[i] == " "`, where an index of -1 yields `undefined`. https://bugs.webkit.org/show_bug.cgi?id=325583

SECTION: REVERTED
- `6e9eb5b802` 06d713139e reverts this commit, and edb44235bc lands it again with a limit of 4 MB in place of 1 MB. The entry of edb44235bc describes the change to `AssemblerDataImpl::~AssemblerDataImpl()`. https://bugs.webkit.org/show_bug.cgi?id=325503

SECTION: NOEFFECT
- `495909dc15` WebCore macOS system colors, adds one `SDKAlignedBehavior` value to `wtf/cocoa/RuntimeApplicationChecksCocoa.h`. https://bugs.webkit.org/show_bug.cgi?id=325250

SECTION: REVERTED
- `06d713139e` Reverts 6e9eb5b802 because the limit should be 4 MB, not 1 MB. edb44235bc reverts this revert. https://bugs.webkit.org/show_bug.cgi?id=325699

SECTION: JIT
- `edb44235bc` Reverts the revert 06d713139e, so the change of 6e9eb5b802 lands again, with a limit of 4 MB in place of 1 MB. When the new option `useCachedAssemblerDataCapacityLimit` is on, `AssemblerDataImpl::~AssemblerDataImpl()` frees a buffer whose capacity exceeds `Options::maximumCachedAssemblerDataCapacity()` instead of keeping it in the per-thread cache, where one 33 MB compile on youtube.com stayed for the process lifetime. The message reports more than 2% on Membuster on an M5 MacBook Air and about 1% on PLT. `UnifiedWebPreferences.yaml` declares the option with default true in this commit, and 529a52d27c later in the range sets the default to false. https://bugs.webkit.org/show_bug.cgi?id=325503

SECTION: EMBEDDER
- `3fb615cd24` WTF removes `StringView::fromLatin1(std::span<const Latin1Character>)`, which only forwarded to the `StringView(std::span<const Latin1Character>)` constructor, and adds a `StringView(const ASCIICString&)` constructor. `StringView::fromLatin1(const char*)` stays. Bun's `src/runtime/webview/ChromeBackend.cpp` called the removed overload in `errorFromExceptionDetails()` and `Transport::handleResponse()`, and this upgrade changes both calls to the constructor. https://bugs.webkit.org/show_bug.cgi?id=325624

SECTION: NOEFFECT
- `6448e98fed` GLib ports only, adds typed wrappers for GLib functions in `wtf/glib/GLibExtras.h`. https://bugs.webkit.org/show_bug.cgi?id=325576

SECTION: EMBEDDER
- `b0aeb54667` WTF marks callable parameters that it only invokes synchronously with `NOESCAPE`, and takes them by `const F&` where it took `F&&`, for example in `WTF::map()`, `WTF::compactMap()`, `WTF::flatMap()`, `StringView::find()`, `FixedVector::createWithSizeFromGenerator()` and `WeakHashMap::removeIf()`. A `mutable` lambda no longer compiles at those call sites. Bun passes only non-mutable lambdas to the ones it calls, such as `WTF::map()` in `src/jsc/bindings/webcore/MessagePort.cpp` and `StringView::find()` in `src/jsc/bindings/DOMURL.cpp`, so this upgrade changes no Bun file for it. https://bugs.webkit.org/show_bug.cgi?id=325623

SECTION: RUNTIME
- `3e27303e85` `IntlSegments::containing()` now calls `ubrk_following(index)` and then `ubrk_previous()` to find the segment, where it called `ubrk_preceding(index + 1)` for the start. When the index is the lead surrogate that starts a segment, the old code stepped back to the previous boundary. `new Intl.Segmenter("en", { granularity: "grapheme" }).segment(" \u{1F600}").containing(1)` returned `{ segment: " \u{1F600}", index: 0 }` and now returns `{ segment: "\u{1F600}", index: 1 }`. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the new result. https://bugs.webkit.org/show_bug.cgi?id=324036

SECTION: WTF
- `e71d94bd49` `ReadWriteLock::writeLockSlow()` now waits for the last reader to clear `s_writerDrainParkedBit` instead of for the out count to reach the drain target, so that clear is the reader's last access to the lock. Before, the `unparkOne()` callback in `readUnlockSlow()` could run an atomic AND on the lock after the writer had acquired it, finished and freed the object that holds it. A writer may now destroy the lock as soon as it has acquired it, and a reader has no such guarantee. Bun does not use `ReadWriteLock` directly, and JSC uses it for `s_destructionLock` in `VM.cpp`. https://bugs.webkit.org/show_bug.cgi?id=325611

SECTION: EMBEDDER
- `01e2533964` JSC marks callable parameters that it only invokes synchronously with `NOESCAPE` across 212 files, among them `StackVisitor::visit()`, `JSC::initialize()`, `Heap::forEachProtectedCell()`, `Structure::forEachProperty()` and `VMManager::forEachVM()`. A few parameters also change type from `F&&` to `const F&`: `Debugger::forEachBreakpointLocation()` now takes `const Function<void(int, int)>&`, and `WeakGCMap::ensureValue()` and `MarkedVector::fillWith()` take the functor by `const&`. Bun calls `StackVisitor::visit()` and `JSC::initialize()` with lambdas that compile unchanged, and calls none of the functions whose parameter type changed. https://bugs.webkit.org/show_bug.cgi?id=325622

SECTION: JIT
- `33bc312ad4` B3 `ReduceStrength` calls `simplifyCFG()` once where it looped in `simplifyCFGToFixpoint()`, and the commit removes that function. `simplifyCFG()` now forwards a successor over a whole chain of jump-only blocks in one step, as `Air::simplifyCFG` does, with a tortoise and hare walk that bails on a cycle of orphan blocks. The message says a single iteration covers almost all CFG simplification. https://bugs.webkit.org/show_bug.cgi?id=325484

SECTION: EMBEDDER
- `4356bc5514` `Yarr::FlagsString` changes from `std::array<char, numberOfFlags + 1>` to a class whose `span()` returns `std::span<const Latin1Character>`, so `Yarr::flagsString(flags).data()` no longer compiles. JSC call sites that print strings through raw pointers move to `SAFE_PRINTF()`, `SAFE_FPRINTF()` and `SAFE_DATALOGF()`, and the RISCV64 instruction `name` constants become `ASCIILiteral`. Bun's `src/jsc/bindings/webcore/SerializedScriptValue.cpp` wrote RegExp flags with `String::fromLatin1(flagsString(...).data())`, and this upgrade changes it to `String { flagsString(...).span() }`. https://bugs.webkit.org/show_bug.cgi?id=325592

SECTION: RUNTIME
- `24e354459d` `TaggedTemplateNode::emitBytecode()` passes the current `this` to a tag that is a super property, as `FunctionCallDotNode` and `FunctionCallBracketNode` already do. Before, the tag function received `HomeObject.[[Prototype]]` as `this`. With `class A { tag() { return this } }` and ``class B extends A { test() { return super.tag`x` } }``, `new B().test()` returned `A.prototype` and now returns the instance. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the new result. https://bugs.webkit.org/show_bug.cgi?id=325756

SECTION: JIT
- `db86e0961a` DFG `FixupPhase::attemptToMakeFastStringAdd()` accepts an operand that speculates String or null/undefined and converts it with `StringOrOtherUse`, so a string addition still becomes `MakeRope` when `str[i]` goes out of bounds and yields `undefined`. The conversion skips `StrCat` nodes from the `String.prototype.concat` intrinsic, because `concat` must throw for a null or undefined `this`. https://bugs.webkit.org/show_bug.cgi?id=325751

SECTION: RUNTIME
- `d29218b6de` The new `JSArray::fastSplice()` runs `Array.prototype.splice` in place for Int32, Double and Contiguous arrays when `holesMustForwardToPrototype()` is false. `arrayProtoFuncSplice` and the DFG operation `arraySpliceImpl` call it, and the DFG path reads the inserted items from the scratch buffer without a copy into a `MarkedArgumentBuffer`. `JSArray::shiftCount()` takes `startIndex` by value, and `shiftCountWithAnyIndexingType()` and `unshiftCountWithAnyIndexingType()` now return false and leave the array unchanged when a hole must forward to the prototype, where they converted the array to ArrayStorage. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` compares the in-place splice with the generic algorithm. https://commits.webkit.org/322256@main (the commit message has no bugs.webkit.org link)

SECTION: RUNTIME
- `71d329770b` `constructGenericTypedArrayViewWithArguments()` does a real `Get` of `length` and applies `ToLength` when an opaque object such as a Proxy taints the `VMInquiry` lookup. Before, the array-like path did not run the `get` trap and read `undefined`, so `new Float64Array(new Proxy({ length: 2, 0: 1, 1: 2 }, {}))` had length 0 and now has length 2. A Proxy of an array still goes through its iterator. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the new result. https://bugs.webkit.org/show_bug.cgi?id=325491

SECTION: WTF
- `ce9608ae41` `UTF8CString` gains two factory functions in `wtf/text/CString.h` for bytes that a C function returns as `char`: `unsafeFromUTF8(const char*)`, which computes the length with `strlen()`, and `fromUTF8(std::span<const char>)`. They replace `UTF8CString { byteCast<char8_t>(...) }` at the ported call sites, for example in `SafeStrerror.cpp`, `StringPrintStream.cpp` and `API/JSStringRefCPP.h`. The change only adds API, and Bun does not call the new functions. https://bugs.webkit.org/show_bug.cgi?id=325748

SECTION: NOEFFECT
- `a09645c2ee` WebCore media autoplay policy, adds the web preference `RequiresUserGestureToStartAudiblePlaybackWhenHidden`. https://bugs.webkit.org/show_bug.cgi?id=310600

SECTION: GC
- `648d26d0bf` `Collector::decideCollectionScope()` replaces `Heap::shouldDoFullCollection()`, and `Heap::willStartCollection()` takes the `CollectionScope` and also does the Eden allocation snapshot and the heap verifier's before-GC pass. The new `Collector::beginMarking()` holds the marking setup at the end of `runBeginPhase`, `MarkedSpace::prepareForMarking()` folds into `MarkedSpace::beginMarking()`, and the commit inlines `Heap::useGenerationalGC()` into its only caller. The commit states no change in behavior. https://bugs.webkit.org/show_bug.cgi?id=325691

SECTION: GC
- `152ae41b47` `Heap::relinquishConn()` no longer keeps the conn when the collector thread is stopping, a check that never fired and that read `m_threadShouldStop` without the thread lock. `Collector::requestCollection()` now release-asserts under the thread lock that the thread is not stopping, and `Heap::finishRelinquishingConn()` asserts the same before it wakes the thread for a queued request. The commit states no change in behavior. https://bugs.webkit.org/show_bug.cgi?id=325741

SECTION: RUNTIME
- `1b1238d3e3` `JSGenericArrayBufferConstructor::constructImpl()` applies `ToIndex` to the length and then to the `maxByteLength` option before it reads `newTarget.prototype`. Before, it converted the length with `ToNumber`, read the prototype and only then applied `ToIndex`, so `Reflect.construct(ArrayBuffer, [-1], newTarget)` surfaced the exception of a throwing `prototype` getter and now throws `RangeError` without reading it. Both values stay in `uint64_t` for the comparison, so a length above 2^32 cannot wrap on a 32-bit `size_t`. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the new order. https://bugs.webkit.org/show_bug.cgi?id=325496

SECTION: RUNTIME
- `e4e335cf8d` `constructArrayWithSizeQuirk()` reads `newTarget.prototype`, and calls `getFunctionRealm()` when the result is not an object, before it throws the `RangeError` for an invalid length. Before, `Reflect.construct(Array, [-1], newTarget)` threw the `RangeError` without running a `prototype` getter, and now the exception of a throwing getter comes first. A call without a custom `newTarget`, such as `new Array(-1)`, does not change. Bun's `test/js/bun/jsc/webkit-upgrade-4611b64906.test.ts` covers the new order. https://bugs.webkit.org/show_bug.cgi?id=325494

SECTION: JIT
- `f325c4f3ad` DFG `FixupPhase` clears `NodeMustGenerate` on `ArithFloor`, `ArithCeil`, `ArithRound` and `ArithTrunc` only in the `Double` rounding mode. The `Int32` and `Int32WithNegativeZeroCheck` modes exit on overflow or negative zero, and with the flag cleared DCE removed the node once its result was folded away. A function that computes `y = Math.floor(k)` and returns `(y | 0) === y` returned `true` for `k = 2147483648.5` after DFG compilation and now returns `false`. https://bugs.webkit.org/show_bug.cgi?id=325499

SECTION: EMBEDDER
- `a516249268` Upstream removes `GCRequest::didFinishEndPhase`, its call in `Collector::runEndPhase()` and the `GCRequest::subsumedBy()` rule that a request with a callback is never subsumed, because nothing upstream ever set the callback. Bun sets it in `JSC__VM__collectAsyncIdle()` in `src/jsc/bindings/bindings.cpp`, to count down `JSVMClientData::idleCollectionsPending` and wake the JS thread after an idle full collection. This upgrade leaves that Bun code unchanged, and Bun's WebKit fork keeps the field, the call and the rule in this merge (see the comment in `heap/GCRequest.h`). https://bugs.webkit.org/show_bug.cgi?id=325776
