SECTION: RUNTIME
- `240fe1fc63` A TypedArray constructor no longer takes a module namespace export named `length` as the array length. 71d329770b, earlier in this range, made `constructGenericTypedArrayViewWithArguments` call `Get` for `length` on every object that taints a `VMInquiry` lookup, and a module namespace is one of them. The real `Get` now runs only when the lookup stops at a `ProxyObject`, including a Proxy on the prototype chain. For a namespace with `export let length = 2`, `new Float64Array(ns).length` is 0 again, as before this range. https://bugs.webkit.org/show_bug.cgi?id=325491

SECTION: RUNTIME
- `1df0dac22d` `parseInt` no longer flattens a short rope. The new `parseIntString(JSGlobalObject*, JSString*, int32_t radix)` in `runtime/ParseInt.h` copies a non-substring rope of at most 64 characters into a stack buffer with `resolveToBuffer` and parses the copy, so it allocates no `StringImpl` for the number text. `globalFuncParseInt` and the DFG operations `operationParseIntString`, `operationParseIntStringNoRadix`, `operationParseIntGeneric` and `operationParseIntGenericNoRadix` call it. The result of `parseInt` does not change. https://bugs.webkit.org/show_bug.cgi?id=325765

SECTION: NOEFFECT
- `bcc6f08841` WebCore quirk for Google Maps embeds with AppKit gestures, removes one WebKit preference. https://bugs.webkit.org/show_bug.cgi?id=325829

SECTION: EMBEDDER
- `529a52d27c` The default of the JSC option `useCachedAssemblerDataCapacityLimit` changes from true to false. JSC generates this option from `CachedAssemblerDataCapacityLimitEnabled` in `UnifiedWebPreferences.yaml`, and now only the WebKit UI process turns it on, when Site Isolation is in use. The 4 MB limit (`maximumCachedAssemblerDataCapacity`) on the `AssemblerData` buffer that each thread caches, which 6e9eb5b802 adds earlier in this range (re-landed as edb44235bc), is therefore off in a JSC-only embedder. Bun's source sets neither option, and Bun's WebKit fork already had its own limit in `~AssemblerDataImpl()`, the option `maximumCachedAssemblerBufferSize` with a 1 MB default. https://bugs.webkit.org/show_bug.cgi?id=325833

SECTION: WTF
- `88167d46ef` `bmalloc_get_allocation_size()`, the libpas backend of `fastMallocSize()`, now finds the owning heap config with `pas_get_page_base()`: it tries `BMALLOC_HEAP_CONFIG`, then `TAGGED_BMALLOC_HEAP_CONFIG`, and falls back to `BMALLOC_HEAP_CONFIG` for large allocations. Since c8cb35beac, earlier in this range, a process with MTE serves some allocations from the tagged heap, and the size lookup knew only the untagged config. Bun's non-ASAN builds configure WebKit with `USE_MIMALLOC=ON` (`scripts/build/deps/webkit.ts`), which turns libpas off (`BENABLE_LIBPAS` 0). https://bugs.webkit.org/show_bug.cgi?id=325834

SECTION: BUILD
- `523f2e2ae2` Build fix for 1df0dac22d: `runtime/ParseInt.h` now includes `JavaScriptCore/JSStringInlines.h`, which the new `parseIntString` needs. https://bugs.webkit.org/show_bug.cgi?id=325860

SECTION: JIT
- `64baa91531` DFG `ConstantFoldingPhase::run` skips blocks with `cfaHasVisited` false in its SSA pass that clips a block after the abstract state becomes invalid. CFA proved such a block unreachable, so it has no abstract state at its head, and running the abstract interpreter over it could trip debug assertions such as the indexing type check for array materialization. CFG simplification removes the block later. https://bugs.webkit.org/show_bug.cgi?id=325835

SECTION: WTF
- `a506af486d` Adds `wtf/posix/POSIXExtras.h`: 18 inline wrappers that take a `UTF8CStringView` where the C function takes a `const char*` and that keep the C return types, among them `posixOpen()`, `posixAccess()`, `posixStat()`, `posixOpendir()`, `posixRealpath()` and `posixDlopen()`. `posixFopen()` takes an `ASCIILiteral` mode and exists on every platform, Windows included. `FileSystemPOSIX.cpp`, `ExecutableAllocator.cpp`, `GdbJIT.cpp` and `jsc.cpp` now call them, which removes 38 `legacyCStringPointer()` calls across the tree. No existing WTF API changes. https://bugs.webkit.org/show_bug.cgi?id=325773

SECTION: NOEFFECT
- `4d00570868` Cocoa CMake port only: Swift API tests and framework search flags in `OptionsCocoa.cmake`. https://bugs.webkit.org/show_bug.cgi?id=325716

SECTION: BUILD
- `12dd44cb41` CMake builds no longer start the generated `cmake_pch.hxx` with `#pragma clang system_header`. The top-level `CMakeLists.txt` sets `CMAKE_PCH_PROLOGUE` to empty after `project()`, and the new `WEBKIT_ENABLE_LANGUAGE` macro in `Source/cmake/WebKitMacros.cmake` does the same after each `enable_language()`. The compiler now reports warnings in the headers that a prefix header such as `JavaScriptCorePrefix.h` pulls in, as the Xcode build does, and this includes the JSCOnly build of Bun's WebKit fork wherever it precompiles that header (the fork turns PCH off on Windows). `JavaScriptCorePrefix.h` also drops its `#define NULL` block for Apple platforms. https://bugs.webkit.org/show_bug.cgi?id=325757

SECTION: NOEFFECT
- `b46c2cfc57` GLib ports only: `SAFE_G_*` printf-style macros in `wtf/glib/GLibExtras.h`. https://bugs.webkit.org/show_bug.cgi?id=325742

SECTION: NOEFFECT
- `a216b77387` visionOS feature in WebCore and WebKit, adds the `ENABLE_CONNECTED_VOLUMETRIC_SCENE` flag, off by default. https://bugs.webkit.org/show_bug.cgi?id=324527

SECTION: WTF
- `28001e7f66` `MonotonicObjectIdentifier<T>` moves from `Source/WebKit/Shared` into WTF as `wtf/MonotonicObjectIdentifier.h`, in namespace `WTF`, with a forward declaration and a global `using` in `wtf/Forward.h`. The rest of the commit is OffscreenCanvas work in WebCore and WebKit. JSC does not use the type. https://bugs.webkit.org/show_bug.cgi?id=325214

SECTION: WTF
- `5f4afbfb12` `RefCounted`, `ThreadSafeRefCounted` and `ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr` carry the new `BASE_CLASS_SWIFT_SHARED_REFERENCE` annotation, so that Swift treats every derived type as a reference type. The new `ENABLE(SWIFT_BASE_CLASS_ANNOTATIONS)` gates it: it needs Swift 6.4, Cocoa ports turn it on for SDKs 27 and later, and other ports get a CMake option that defaults to on when the detected `swiftc` is 6.4 or newer. `wtf/RefCounted.h` now includes `wtf/SwiftBridging.h`, and the GLib `ActivityObserver` class moves into `wtf/RunLoop.h`. A C++-only build sees attribute changes only, because `swiftRef()` exists only under `defined(__swift__)`. https://bugs.webkit.org/show_bug.cgi?id=303818

SECTION: WTF
- `bdea79f67f` Adds `StringView::codePointCount()`, which returns `length()` for an 8-bit view and counts code points with `U16_FWD_1` for a 16-bit view. The rest adds GLib and GStreamer wrappers (`gDBusConnectionEmitSignal()`, `gObjectNew()`, `gstStructureSet()`, `gstStructureNew()`) that only GLib ports compile, and removes 60 `legacyCStringPointer()` calls there. https://bugs.webkit.org/show_bug.cgi?id=325782

SECTION: JIT
- `39f7f217cd` Like bb73af4a4b for the Baseline JIT earlier in this range, the 64-bit LLInt now runs the FastArray path of `op_iterator_next` inline when the iterable has `ArrayWithInt32` or `ArrayWithContiguous` indexing type. `LowLevelInterpreter64.asm` compares the index with `publicLength`, loads the element, stores `value` and `done` and increments the index, where it used to call the `iterator_next_fast_array_*` slow path for every element. Holes, other shapes and the end of the iteration still call the slow path. Bun's WebKit fork already ran an equivalent inline path in its LLInt (label `.iteratorNextIsNotCell`, slow path `iterator_next_index_in_frame_*`), so this is not new for Bun. https://bugs.webkit.org/show_bug.cgi?id=325774

SECTION: NOEFFECT
- `35090d6160` WebCore select element fixes, one preference status and layout tests. https://bugs.webkit.org/show_bug.cgi?id=325465

SECTION: JIT
- `6edd3cc729` Removes the four deprecated `B3::BasicBlock::appendNewControlValue()` overloads, which `B3BasicBlock.h` exported with `JS_EXPORT_PRIVATE` and which existed so that callers could drop the old `ControlValue` class. `B3LowerMacros.cpp`, `FTLOutput.cpp`, `WasmOMGIRGenerator.cpp` and the testb3 files now call `setSuccessors()` or `clearSuccessors()` and then `appendNew<Value>()`, and `clearSuccessors()` becomes inline. Bun's source does not build B3 IR, so nothing there calls the removed functions. https://bugs.webkit.org/show_bug.cgi?id=159440

SECTION: RUNTIME
- `8ae122a848` A `Reflect.construct(F, [a, b])` call site whose arguments list is an array literal without holes or spread elements now emits `op_construct`, with the elements evaluated into the argument registers, as `f.apply(thisArg, [a, b])` already does for `op_call`. Since 7b485a76e9, the base of this range, such a site emitted `op_construct_varargs` and allocated the array only to copy its elements into the callee frame. The new `BytecodeGenerator::emitNewArrayByReversingArguments()` creates the array only when the checks of the call site fail and the ordinary call runs. The message reports `reflect-construct-array-literal` as 2.4734x faster (40.5969 against 16.4136). https://bugs.webkit.org/show_bug.cgi?id=325232

SECTION: NOEFFECT
- `49497bdc82` WebCore `MathMLAnchorElement`, behind a new preference that is off. https://bugs.webkit.org/show_bug.cgi?id=321990

SECTION: NOEFFECT
- `bb06bdc992` WebCore Device Posture API, off by default, plus `HAVE_UI_HINGE_INTERACTION` for iOS. https://bugs.webkit.org/show_bug.cgi?id=325585

SECTION: WTF
- `5b0cfbc518` libpas allocates its compact heap reservation from two fronts: allocations aligned to less than 16 bytes bump up from the bottom and can use at most 128MiB, and 16-byte-aligned ones bump down from the top. The new `PAS_DEFINE_OVERALIGNED_COMPACT_PTR` scales its 3-byte index by 16 where `PAS_DEFINE_COMPACT_PTR` scales by 8, so it reaches 256MiB, and it now backs `pas_compact_heap_ptr`, `pas_compact_segregated_size_directory_ptr`, `pas_compact_bitfit_directory_ptr` and `pas_compact_cartesian_tree_node_ptr`. The reservation, now sized by the macro `PAS_COMPACT_HEAP_RESERVATION_SIZE`, doubles from 128MiB to 256MiB on `PAS_PLATFORM(MAC)` and stays 128MiB elsewhere, because processes that use more than 50GiB of memory began to exhaust it. Bun's non-ASAN builds configure WebKit with `USE_MIMALLOC=ON`, which turns libpas off. https://bugs.webkit.org/show_bug.cgi?id=325689

SECTION: GC
- `5297538c7d` The request queue checks and the mutator's phase loop move from `Heap` into `Collector`. The new `Collector::isSubsumedByQueuedRequest()`, a self-locking `Collector::hasOutstandingRequest()` and `Collector::collectInMutatorThread(Heap&)` replace code in `Heap`, and `Heap::collect()` now holds the sequence that `collectAsync()` and `collectSync()` each spelled out. The private `Heap::requestCollection()` goes away, and `WTF_GUARDED_BY_LOCK` now covers the queue, the ticket counters, `m_threadIsWorking` (the renamed `m_collectorThreadIsRunning`) and `m_threadShouldStop`. Behaviour does not change, except that `--logGC` now takes the Collector's thread lock to decide whether to log. https://bugs.webkit.org/show_bug.cgi?id=325825

SECTION: EMBEDDER
- `d3c9261949` A `HeapObserver` now gets `didGarbageCollect()` before the heap stores the collection length, so `Heap::lastFullGCLength()` and `lastEdenGCLength()` return the previous collection's value inside the callback. The cause is a refactoring that the message describes as no change in behaviour: `Collector::runEndPhase()` hands the heap's end-of-collection work, for which it reached into `Heap` 45 times, to new private `Heap` methods (`rememberExecutingAndCompilingCodeBlocks()`, `endMarking(size_t)`, `verifyMarking()`, `pruneDeadReferences()`, `prepareForAllocation()`, `didFinishCollection()`, `recordCollectionTime(Seconds)`), and the last of them runs after the observers. Bun's observers, `GCProfilerObserver` in `src/jsc/bindings/NodeV8.h` and `HeapSizeAfterLastCollection` in `src/jsc/bindings/BunClientData.h`, read only heap sizes and capacity, which the heap still stores before the callback, so they need no change. https://bugs.webkit.org/show_bug.cgi?id=325862

SECTION: EMBEDDER
- `7cc8fd9a7f` `wtf/AvailableMemory.h` drops `MemoryStatus`, `memoryStatus()` and `isUnderMemoryPressure()`, and `percentAvailableMemoryInUse()` becomes an exported function on every platform that returns `memoryFootprint() / availableMemory()`, capped at 1. On iOS `availableMemory()` now derives the jetsam limit from `task_info(TASK_VM_INFO)`, because the sandbox blocks `memorystatus_control`: it reported 1024 MB where the real limit is 1536 MB or 2048 MB, which made JSC collect synchronously more often than intended. Bun's source calls none of the removed functions, and the commit leaves the result of `WTF::ramSize()`, which `src/jsc/bindings/c-bindings.cpp` and `BunProcess.cpp` call, unchanged on macOS, Linux and Windows. Bun's WebKit fork carries patches in `AvailableMemory.cpp` that this rewrite overlaps (`uv_get_constrained_memory()` on Linux, a macOS `memoryStatus()`), and it turns on `USE(MEMORY_FOOTPRINT_API)`, which makes `Heap::overCriticalMemoryThreshold()` a caller of `percentAvailableMemoryInUse()` in Bun. https://bugs.webkit.org/show_bug.cgi?id=325827

SECTION: EMBEDDER
- `b31697dfe2` `URL::string()` becomes ref-qualified. `const String& string() const &` keeps the old behaviour for lvalues, and the new `String string() &&` moves `m_string` out of an expiring `URL` and marks it invalid. A call on a temporary or on `WTF::move(url)` now returns a `String` by value, so `auto&` no longer binds to the result, and `const auto&` keeps the string alive. Bun calls `URL::string()` in many files under `src/jsc/bindings` and none needs a change: calls on temporaries, such as `WTF::URL::fileURLWithFileSystemPath(u).string()` in `BunCPUProfiler.cpp`, now select the `&&` overload. https://bugs.webkit.org/show_bug.cgi?id=325669

SECTION: NOEFFECT
- `492d8d6e0d` WebCore scrolling signpost for rubber-banding, adds one trace point name to `wtf/SystemTracing.h`. https://bugs.webkit.org/show_bug.cgi?id=324835

SECTION: RUNTIME
- `c59d70e267` Two changes speed up the mapping from bytecode offsets to line and column numbers for stack traces. `ExpressionInfo` caches decoded entries by bytecode offset in `m_cachedEntries`: `entryForInstPC()` now takes a `ConcurrentJSLocker`, and `UnlinkedCodeBlock::expressionInfoForBytecodeIndex()` holds the block's lock around it, because compiler threads also look up entries. `LineStartTable::build` finds every line start in one SIMD pass with the new `SIMD::laneMask()` and `lineTerminatorLanes()`, where it restarted `findLineTerminator()` after every line break, and `lineStartAfterTerminator()` goes away. Bun reads positions through `CodeBlock::expressionInfoForBytecodeIndex()` in `src/jsc/bindings/ErrorStackFrame.cpp`, whose signature does not change, and Bun's WebKit fork already cached the line and column per bytecode offset for JSC's own lookups (`ExpressionInfo::lineColumnInTextForInstPC()`). https://bugs.webkit.org/show_bug.cgi?id=325559

SECTION: EMBEDDER
- `696c406fe5` JSC's `Error.captureStackTrace(error)` no longer formats the `stack` string during the call when the argument is an extensible `ErrorInstance` without an own `stack` property: `ErrorInstance::trySaveCapturedStackTraceForLazyMaterialization()` keeps the frames in the new `m_capturedStackTrace` and installs `stack` as a placeholder. JSC builds the string on the first read of the error info, or at the end of a collection that finds a frame's cell dead. The commit renames `setStackPropertyAlreadyMaterialized()`, which Bun does not call, to `setStackPropertyProvidedByCapturedStackTrace()`, and the new `m_hasErrorInfo` bit replaces the null check of `m_stackString`. In this upgrade Bun's own `Error.captureStackTrace` (`errorConstructorFuncCaptureStackTrace` in `src/jsc/bindings/FormatStackTraceForJS.cpp`) gains a `materializeErrorInfoIfNeeded(vm)` call before it sets `stack` on an already materialized error, so that frames saved by JSC's function in a `node:vm` realm do not replace that value on the next read. https://bugs.webkit.org/show_bug.cgi?id=325578
