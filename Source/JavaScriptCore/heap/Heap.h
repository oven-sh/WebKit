/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2026 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "ArrayBuffer.h"
#include "CellState.h"
#include "CollectionScope.h"
#include "CollectorPhase.h"
#include "CompleteSubspace.h"
#include "DeleteAllCodeEffort.h"
#include "GCConductor.h"
#include "GCIncomingRefCountedSet.h"
#include "GCMemoryOperations.h"
#include "GCRequest.h"
#include "GCSafepointEpoch.h"
#include "GCThreadLocalCache.h"
#include "HandleSet.h"
#include "HeapClientSet.h"
#include "JSCConfig.h"
#include "HeapFinalizerCallback.h"
#include "HeapObserver.h"
#include "IsoCellSet.h"
#include "IsoHeapCellType.h"
#include "IsoInlinedHeapCellType.h"
#include "IsoSubspace.h"
#include "JSDestructibleObjectHeapCellType.h"
#include "MarkedBlock.h"
#include "MarkedSpace.h"
#include "MutatorState.h"
#include "Options.h"
#include "PreciseSubspace.h"
#include "StructureID.h"
#include "Synchronousness.h"
#include "WeakHandleOwner.h"
#include <JavaScriptCore/SubspaceAccess.h>
#include <atomic>
#include <limits>
#include <wtf/Atomics.h>
#include <wtf/AutomaticThread.h>
#include <wtf/Box.h>
#include <wtf/Condition.h>
#include <wtf/ConcurrentPtrHashSet.h>
#include <wtf/Deque.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/HashCountedSet.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/Markable.h>
#include <wtf/NotFound.h>
#include <wtf/ParallelHelperPool.h>
#include <wtf/Threading.h>

#if USE(BUN_JSC_ADDITIONS)
#include "WeakSet.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace WTF {
class AtomStringTable;
}

namespace JSC {

class CodeBlock;
class CodeBlockSet;
class CollectingScope;
class ConservativeRoots;
class GCDeferralContext;
class EdenGCActivityCallback;
class FastMallocAlignedMemoryAllocator;
class FullGCActivityCallback;
class GCActivityCallback;
class GCAwareJITStubRoutine;
class GigacageAlignedMemoryAllocator;
class Heap;
class HeapProfiler;
class HeapVerifier;
class IncrementalSweeper;
class JITStubRoutine;
class JITStubRoutineSet;
class JSCell;
class JSCellButterfly;
class JSRopeString;
class JSString;
class JSValue;
class LocalAllocator;
class MachineThreads;
class MarkStackArray;
class MarkStackMergingConstraint;
class BlockDirectory;
class MarkedVectorBase;
class MarkingConstraint;
class JSLock;
class MarkingConstraintSet;
class MutatorScheduler;
class RetiredStructureChainInvalidationWatchpoints;
class RunningScope;
class SlotVisitor;
class SpaceTimeMutatorScheduler;
class StopIfNecessaryTimer;
class SweepingScope;
class VM;
class VerifierSlotVisitor;
class WeakGCHashTable;
struct CurrentThreadState;

#ifdef JSC_GLIB_API_ENABLED
class JSCGLibWrapperObject;
#endif

namespace DFG {
class SpeculativeJIT;
}

namespace Wasm {
class Callee;
}

namespace GCClient {
class Heap;
}

#define FOR_EACH_JSC_COMMON_ISO_SUBSPACE(v) \
    v(arraySpace, cellHeapCellType, JSArray) \
    v(calleeSpace, cellHeapCellType, JSCallee) \
    v(clonedArgumentsSpace, cellHeapCellType, ClonedArguments) \
    v(customGetterSetterSpace, cellHeapCellType, CustomGetterSetter) \
    v(dateInstanceSpace, dateInstanceHeapCellType, DateInstance) \
    v(domAttributeGetterSetterSpace, cellHeapCellType, DOMAttributeGetterSetter) \
    v(exceptionSpace, destructibleCellHeapCellType, Exception) \
    v(functionSpace, cellHeapCellType, JSFunction) \
    v(getterSetterSpace, cellHeapCellType, GetterSetter) \
    v(globalLexicalEnvironmentSpace, globalLexicalEnvironmentHeapCellType, JSGlobalLexicalEnvironment) \
    v(internalFunctionSpace, cellHeapCellType, InternalFunction) \
    v(jsGlobalProxySpace, cellHeapCellType, JSGlobalProxy) \
    v(nativeExecutableSpace, destructibleCellHeapCellType, NativeExecutable) \
    v(numberObjectSpace, cellHeapCellType, NumberObject) \
    v(plainObjectSpace, cellHeapCellType, JSNonFinalObject) \
    v(promiseSpace, cellHeapCellType, JSPromise) \
    v(iteratorSpace, cellHeapCellType, JSIterator) \
    v(propertyNameEnumeratorSpace, cellHeapCellType, JSPropertyNameEnumerator) \
    v(propertyTableSpace, destructibleCellHeapCellType, PropertyTable) \
    v(regExpSpace, destructibleCellHeapCellType, RegExp) \
    v(regExpObjectSpace, cellHeapCellType, RegExpObject) \
    v(ropeStringSpace, ropeStringHeapCellType, JSRopeString) \
    v(scopedArgumentsSpace, cellHeapCellType, ScopedArguments) \
    v(sparseArrayValueMapSpace, destructibleCellHeapCellType, SparseArrayValueMap) \
    v(stringSpace, stringHeapCellType, JSString) \
    v(stringObjectSpace, cellHeapCellType, StringObject) \
    v(structureChainSpace, cellHeapCellType, StructureChain) \
    v(structureRareDataSpace, destructibleCellHeapCellType, StructureRareData) \
    v(symbolTableSpace, destructibleCellHeapCellType, SymbolTable) \
    v(internalFieldTupleSpace, cellHeapCellType, InternalFieldTuple)

#if ENABLE(WEBASSEMBLY)
#define FOR_EACH_JSC_WEBASSEMBLY_STRUCTURE_ISO_SUBSPACE(v) \
    v(webAssemblyGCStructureSpace, destructibleCellHeapCellType, WebAssemblyGCStructure)
#else
#define FOR_EACH_JSC_WEBASSEMBLY_STRUCTURE_ISO_SUBSPACE(v)
#endif

#define FOR_EACH_JSC_STRUCTURE_ISO_SUBSPACE(v) \
    v(structureSpace, destructibleCellHeapCellType, Structure) \
    v(brandedStructureSpace, destructibleCellHeapCellType, BrandedStructure) \
    FOR_EACH_JSC_WEBASSEMBLY_STRUCTURE_ISO_SUBSPACE(v)

#define FOR_EACH_JSC_ISO_SUBSPACE(v) \
    FOR_EACH_JSC_COMMON_ISO_SUBSPACE(v) \
    FOR_EACH_JSC_STRUCTURE_ISO_SUBSPACE(v)

#if JSC_OBJC_API_ENABLED
#define FOR_EACH_JSC_OBJC_API_DYNAMIC_ISO_SUBSPACE(v) \
    v(apiWrapperObjectSpace, apiWrapperObjectHeapCellType, JSCallbackObject<JSAPIWrapperObject>) \
    v(objCCallbackFunctionSpace, objCCallbackFunctionHeapCellType, ObjCCallbackFunction)
#else
#define FOR_EACH_JSC_OBJC_API_DYNAMIC_ISO_SUBSPACE(v)
#endif

#ifdef JSC_GLIB_API_ENABLED
#define FOR_EACH_JSC_GLIB_API_DYNAMIC_ISO_SUBSPACE(v) \
    v(apiWrapperObjectSpace, apiWrapperObjectHeapCellType, JSCallbackObject<JSAPIWrapperObject>) \
    v(jscCallbackFunctionSpace, jscCallbackFunctionHeapCellType, JSCCallbackFunction) \
    v(callbackAPIWrapperGlobalObjectSpace, callbackAPIWrapperGlobalObjectHeapCellType, JSCallbackObject<JSAPIWrapperGlobalObject>)
#else
#define FOR_EACH_JSC_GLIB_API_DYNAMIC_ISO_SUBSPACE(v)
#endif

#if ENABLE(WEBASSEMBLY)
#define FOR_EACH_JSC_WEBASSEMBLY_DYNAMIC_ISO_SUBSPACE(v) \
    v(pinballCompletionSpace, destructibleCellHeapCellType, PinballCompletion) \
    v(webAssemblyExceptionSpace, webAssemblyExceptionHeapCellType, JSWebAssemblyException) \
    v(webAssemblyFunctionSpace, webAssemblyFunctionHeapCellType, WebAssemblyFunction) \
    v(webAssemblyGlobalSpace, webAssemblyGlobalHeapCellType, JSWebAssemblyGlobal) \
    v(webAssemblyMemorySpace, webAssemblyMemoryHeapCellType, JSWebAssemblyMemory) \
    v(webAssemblyModuleSpace, webAssemblyModuleHeapCellType, JSWebAssemblyModule) \
    v(webAssemblyModuleRecordSpace, webAssemblyModuleRecordHeapCellType, WebAssemblyModuleRecord) \
    v(webAssemblyTableSpace, webAssemblyTableHeapCellType, JSWebAssemblyTable) \
    v(webAssemblyTagSpace, webAssemblyTagHeapCellType, JSWebAssemblyTag) \
    v(webAssemblyStreamingContextSpace, destructibleCellHeapCellType, JSWebAssemblyStreamingContext) \
    v(webAssemblyWrapperFunctionSpace, cellHeapCellType, WebAssemblyWrapperFunction)

// FIXME: This is a bit confusingly named since the objects in here are exclusive to the subspace but they can vary in size thus can't be in an IsoSubspace.
#define FOR_EACH_JSC_WEBASSEMBLY_DYNAMIC_NON_ISO_SUBSPACE(v) \
    v(webAssemblyInstanceSpace, webAssemblyInstanceHeapCellType, JSWebAssemblyInstance, PreciseSubspace)

#else
#define FOR_EACH_JSC_WEBASSEMBLY_DYNAMIC_ISO_SUBSPACE(v)
#define FOR_EACH_JSC_WEBASSEMBLY_DYNAMIC_NON_ISO_SUBSPACE(v)
#endif

#define FOR_EACH_JSC_DYNAMIC_ISO_SUBSPACE(v) \
    FOR_EACH_JSC_OBJC_API_DYNAMIC_ISO_SUBSPACE(v) \
    FOR_EACH_JSC_GLIB_API_DYNAMIC_ISO_SUBSPACE(v) \
    \
    v(apiGlobalObjectSpace, apiGlobalObjectHeapCellType, JSAPIGlobalObject) \
    v(apiValueWrapperSpace, cellHeapCellType, JSAPIValueWrapper) \
    v(arrayBufferSpace, cellHeapCellType, JSArrayBuffer) \
    v(arrayIteratorSpace, cellHeapCellType, JSArrayIterator) \
    v(asyncGeneratorSpace, cellHeapCellType, JSAsyncGenerator) \
    v(asyncFunctionGeneratorSpace, cellHeapCellType, JSAsyncFunctionGenerator) \
    v(bigInt64ArraySpace, cellHeapCellType, JSBigInt64Array) \
    v(bigIntObjectSpace, cellHeapCellType, BigIntObject) \
    v(bigUint64ArraySpace, cellHeapCellType, JSBigUint64Array) \
    v(booleanObjectSpace, cellHeapCellType, BooleanObject) \
    v(boundFunctionSpace, cellHeapCellType, JSBoundFunction) \
    v(callbackConstructorSpace, callbackConstructorHeapCellType, JSCallbackConstructor) \
    v(callbackGlobalObjectSpace, callbackGlobalObjectHeapCellType, JSCallbackObject<JSGlobalObject>) \
    v(callbackFunctionSpace, cellHeapCellType, JSCallbackFunction) \
    v(callbackObjectSpace, callbackObjectHeapCellType, JSCallbackObject<JSNonFinalObject>) \
    v(customGetterFunctionSpace, customGetterFunctionHeapCellType, JSCustomGetterFunction) \
    v(customSetterFunctionSpace, customSetterFunctionHeapCellType, JSCustomSetterFunction) \
    v(dataViewSpace, cellHeapCellType, JSDataView) \
    v(debuggerScopeSpace, cellHeapCellType, DebuggerScope) \
    v(errorInstanceSpace, errorInstanceHeapCellType, ErrorInstance) \
    v(finalizationRegistrySpace, finalizationRegistryCellType, JSFinalizationRegistry) \
    v(float16ArraySpace, cellHeapCellType, JSFloat16Array) \
    v(float32ArraySpace, cellHeapCellType, JSFloat32Array) \
    v(float64ArraySpace, cellHeapCellType, JSFloat64Array) \
    v(functionRareDataSpace, destructibleCellHeapCellType, FunctionRareData) \
    v(functionWithFieldsSpace, cellHeapCellType, JSFunctionWithFields) \
    v(generatorSpace, cellHeapCellType, JSGenerator) \
    v(globalObjectSpace, globalObjectHeapCellType, JSGlobalObject) \
    v(injectedScriptHostSpace, injectedScriptHostSpaceHeapCellType, Inspector::JSInjectedScriptHost) \
    v(int8ArraySpace, cellHeapCellType, JSInt8Array) \
    v(int16ArraySpace, cellHeapCellType, JSInt16Array) \
    v(int32ArraySpace, cellHeapCellType, JSInt32Array) \
    v(intlCollatorSpace, intlCollatorHeapCellType, IntlCollator) \
    v(intlDateTimeFormatSpace, intlDateTimeFormatHeapCellType, IntlDateTimeFormat) \
    v(intlDisplayNamesSpace, intlDisplayNamesHeapCellType, IntlDisplayNames) \
    v(intlDurationFormatSpace, intlDurationFormatHeapCellType, IntlDurationFormat) \
    v(intlListFormatSpace, intlListFormatHeapCellType, IntlListFormat) \
    v(intlLocaleSpace, intlLocaleHeapCellType, IntlLocale) \
    v(intlNumberFormatSpace, intlNumberFormatHeapCellType, IntlNumberFormat) \
    v(intlPluralRulesSpace, intlPluralRulesHeapCellType, IntlPluralRules) \
    v(intlRelativeTimeFormatSpace, intlRelativeTimeFormatHeapCellType, IntlRelativeTimeFormat) \
    v(intlSegmentIteratorSpace, intlSegmentIteratorHeapCellType, IntlSegmentIterator) \
    v(intlSegmenterSpace, intlSegmenterHeapCellType, IntlSegmenter) \
    v(intlSegmentsSpace, intlSegmentsHeapCellType, IntlSegments) \
    v(iteratorHelperSpace, cellHeapCellType, JSIteratorHelper) \
    v(javaScriptCallFrameSpace, javaScriptCallFrameHeapCellType, Inspector::JSJavaScriptCallFrame) \
    v(jsModuleRecordSpace, jsModuleRecordHeapCellType, JSModuleRecord) \
    v(moduleRegistryEntrySpace, destructibleCellHeapCellType, ModuleRegistryEntry) \
    v(moduleLoadingContextSpace, destructibleCellHeapCellType, ModuleLoadingContext) \
    v(sentinelSpace, cellHeapCellType, JSSentinel) \
    v(syntheticModuleRecordSpace, syntheticModuleRecordHeapCellType, SyntheticModuleRecord) \
    v(jsMicrotaskDispatcherSpace, destructibleCellHeapCellType, JSMicrotaskDispatcher) \
    v(mapIteratorSpace, cellHeapCellType, JSMapIterator) \
    v(mapSpace, cellHeapCellType, JSMap) \
    v(moduleNamespaceObjectSpace, moduleNamespaceObjectHeapCellType, JSModuleNamespaceObject) \
    v(nativeStdFunctionSpace, nativeStdFunctionHeapCellType, JSNativeStdFunction) \
    v(proxyObjectSpace, cellHeapCellType, ProxyObject) \
    v(proxyRevokeSpace, cellHeapCellType, ProxyRevoke) \
    v(rawJSONObjectSpace, cellHeapCellType, JSRawJSONObject) \
    v(remoteFunctionSpace, cellHeapCellType, JSRemoteFunction) \
    v(scopedArgumentsTableSpace, destructibleCellHeapCellType, ScopedArgumentsTable) \
    v(setIteratorSpace, cellHeapCellType, JSSetIterator) \
    v(setSpace, cellHeapCellType, JSSet) \
    v(shadowRealmSpace, cellHeapCellType, ShadowRealmObject) \
    v(strictEvalActivationSpace, cellHeapCellType, StrictEvalActivation) \
    v(stringIteratorSpace, cellHeapCellType, JSStringIterator) \
    v(sourceCodeSpace, destructibleCellHeapCellType, JSSourceCode) \
    v(symbolSpace, destructibleCellHeapCellType, Symbol) \
    v(symbolObjectSpace, cellHeapCellType, SymbolObject) \
    v(templateObjectDescriptorSpace, destructibleCellHeapCellType, JSTemplateObjectDescriptor) \
    v(temporalDurationSpace, cellHeapCellType, TemporalDuration) \
    v(temporalInstantSpace, cellHeapCellType, TemporalInstant) \
    v(temporalPlainDateSpace, cellHeapCellType, TemporalPlainDate) \
    v(temporalPlainDateTimeSpace, cellHeapCellType, TemporalPlainDateTime) \
    v(temporalPlainTimeSpace, cellHeapCellType, TemporalPlainTime) \
    v(temporalTimeZoneSpace, temporalTimeZoneHeapCellType, TemporalTimeZone) \
    v(uint8ArraySpace, cellHeapCellType, JSUint8Array) \
    v(uint8ClampedArraySpace, cellHeapCellType, JSUint8ClampedArray) \
    v(uint16ArraySpace, cellHeapCellType, JSUint16Array) \
    v(uint32ArraySpace, cellHeapCellType, JSUint32Array) \
    v(unlinkedEvalCodeBlockSpace, destructibleCellHeapCellType, UnlinkedEvalCodeBlock) \
    v(unlinkedFunctionCodeBlockSpace, destructibleCellHeapCellType, UnlinkedFunctionCodeBlock) \
    v(unlinkedModuleProgramCodeBlockSpace, destructibleCellHeapCellType, UnlinkedModuleProgramCodeBlock) \
    v(unlinkedProgramCodeBlockSpace, destructibleCellHeapCellType, UnlinkedProgramCodeBlock) \
    v(weakObjectRefSpace, cellHeapCellType, JSWeakObjectRef) \
    v(weakMapSpace, weakMapHeapCellType, JSWeakMap) \
    v(weakSetSpace, weakSetHeapCellType, JSWeakSet) \
    v(withScopeSpace, cellHeapCellType, JSWithScope) \
    v(wrapForValidIteratorSpace, cellHeapCellType, JSWrapForValidIterator) \
    v(promiseCombinatorsContextSpace, cellHeapCellType, JSPromiseCombinatorsContext) \
    v(promiseCombinatorsGlobalContextSpace, cellHeapCellType, JSPromiseCombinatorsGlobalContext) \
    v(slimPromiseReactionSpace, cellHeapCellType, JSSlimPromiseReaction) \
    v(fullPromiseReactionSpace, cellHeapCellType, JSFullPromiseReaction) \
    v(asyncFromSyncIteratorSpace, cellHeapCellType, JSAsyncFromSyncIterator) \
    v(regExpStringIteratorSpace, cellHeapCellType, JSRegExpStringIterator) \
    v(disposableStackSpace, cellHeapCellType, JSDisposableStack) \
    v(asyncDisposableStackSpace, cellHeapCellType, JSAsyncDisposableStack) \
    v(moduleLoaderSpace, destructibleCellHeapCellType, JSModuleLoader) \
    v(moduleLoaderPayloadSpace, cellHeapCellType, ModuleLoaderPayload) \
    v(moduleGraphLoadingStateSpace, destructibleCellHeapCellType, ModuleGraphLoadingState) \
    \
    FOR_EACH_JSC_WEBASSEMBLY_DYNAMIC_ISO_SUBSPACE(v)

typedef HashCountedSet<JSCell*> ProtectCountSet;
typedef HashCountedSet<ASCIILiteral> TypeCountSet;

enum class HeapType : uint8_t { Small, Medium, Large };

class HeapUtil;

class Heap {
    WTF_MAKE_NONCOPYABLE(Heap);
public:
    friend class JIT;
    friend class DFG::SpeculativeJIT;
    static JSC::Heap* heap(const JSValue); // 0 for immediate values
    static JSC::Heap* heap(const HeapCell*);

    // This constant determines how many blocks we iterate between checks of our 
    // deadline when calling Heap::isPagedOut. Decreasing it will cause us to detect 
    // overstepping our deadline more quickly, while increasing it will cause 
    // our scan to run faster. 
    static constexpr unsigned s_timeCheckResolution = 16;

    bool isMarked(const void*);
    static bool testAndSetMarked(HeapVersion, const void*);

    static inline size_t cellSize(const void*);

    void writeBarrier(const JSCell* from);
    void writeBarrier(const JSCell* from, JSValue to);
    void writeBarrier(const JSCell* from, JSCell* to);

    void mutatorFence();
    
    // Take this if you know that from->cellState() < barrierThreshold.
    JS_EXPORT_PRIVATE void writeBarrierSlowPath(const JSCell* from);

    Heap(VM&, HeapType);
    ~Heap();
    void lastChanceToFinalize();
    void releaseDelayedReleasedObjects();

    // SharedGC (T9): returns "the main mutator VM" (SPEC-heap.md deviation
    // 3); plain pointer arithmetic, callable from any thread incl. VM-less
    // conductors. See the audit legend at the definition (HeapInlines.h).
    VM& vm() const;

    MarkedSpace& objectSpace() LIFETIME_BOUND { return m_objectSpace; }
    MachineThreads& machineThreads() { return *m_machineThreads; }

    SlotVisitor& collectorSlotVisitor() { return *m_collectorSlotVisitor; }

    JS_EXPORT_PRIVATE GCActivityCallback* fullActivityCallback();
    JS_EXPORT_PRIVATE GCActivityCallback* edenActivityCallback();

    JS_EXPORT_PRIVATE void setFullActivityCallback(RefPtr<GCActivityCallback>&&);
    JS_EXPORT_PRIVATE void setEdenActivityCallback(RefPtr<GCActivityCallback>&&);
    JS_EXPORT_PRIVATE void disableStopIfNecessaryTimer();

    JS_EXPORT_PRIVATE void setGarbageCollectionTimerEnabled(bool);
    JS_EXPORT_PRIVATE void scheduleOpportunisticFullCollection();

    IncrementalSweeper& sweeper() { return m_sweeper.get(); }

    void addObserver(HeapObserver* observer) { m_observers.append(observer); }
    void removeObserver(HeapObserver* observer) { m_observers.removeFirst(observer); }

    // SharedGC (review round 2): per-THREAD once ISS — reads the calling
    // thread's mutator-state slot (see mutatorStateSlot()); !ISS: the server
    // field, exactly today's behavior. Defined at the bottom of this header
    // (needs GCClient::Heap).
    inline MutatorState mutatorState() const;
    std::optional<CollectionScope> collectionScope() const { return m_collectionScope; }
    bool hasHeapAccess() const
    {
        // SharedGC (§10A): once ISS, per-client access state supersedes the
        // hasAccessBit; this server-level query forwards to the main client.
        if (isSharedServer()) [[unlikely]]
            return mainClientHasHeapAccess();
        return m_worldState.load() & hasAccessBit;
    }
    bool worldIsStopped() const { return WTF::atomicLoad(const_cast<bool*>(&m_worldIsStopped), std::memory_order_relaxed); }
    bool worldIsRunning() const { return !worldIsStopped(); }

    // --- Shared heap server interface (SPEC-heap.md §9; THREADS) ---
    // N GCClient::Heaps (one per mutator thread, post-GIL) may share this
    // server when Options::useSharedGCHeap() is set. T1 scaffolding; the
    // shared-mode protocols land in T2-T8.

    HeapClientSet& clientSet() LIFETIME_BOUND { return m_clientSet; }

    // T7-mspl-per-directory: the mutator-slow-path lock (MSPL, rank 7) is now
    // a writer-preferring RW facade so unrelated size-classes' refills do not
    // serialize on one server-wide WTF::Lock. The two acquisition modes:
    //
    //   EXCLUSIVE (lock()/unlock(), the legacy "server-wide MSPL"): excludes
    //   every per-directory stripe AND every other exclusive holder. Taken by
    //   the genuinely cross-directory / cross-registry sections that the old
    //   single lock covered and that the per-directory + registry stripes do
    //   NOT make safe on their own:
    //     - cross-directory empty-block steal (sweep + removeFromDirectory of
    //       a FOREIGN directory while a sibling stripe could be resizing that
    //       directory's m_bits — the I5b lock-free read in
    //       MarkedBlock::Handle::sweep is the hazard);
    //     - mutator-concurrent full sweeps (Heap::sweepSynchronously,
    //       IncrementalSweeper::sweepNextBlockShared) for the same I5b reason
    //       across every directory;
    //     - WeakSet::allocate / sweep / shrink (the weak-mutation protocol is
    //       still server-wide; review-round-4 carve-outs unchanged);
    //     - GCThreadLocalCache teardown, server teardown,
    //       enablePreciseAllocationTracking, sweepAllLogicallyEmptyWeakBlocks.
    //
    //   STRIPE (enterStripe()/exitStripe(), shared): the per-BlockDirectory
    //   refill leg (own-directory cursor search, in-lock block sweep,
    //   tryAllocateBlock + addBlock — the 75%-JSBigInt locksite). Any number
    //   of stripes run concurrently; each stripe holder ALSO holds that
    //   directory's BlockDirectory::m_refillLock (rank 7a) so two clients
    //   refilling the SAME directory still serialize, and so the I5b m_bits
    //   resize in addBlock excludes the same-directory lock-free bit reads
    //   inside MarkedBlock::Handle::sweep / assertIsMutatorOrMutatorIsStopped.
    //   Cross-directory MarkedSpace state touched on the stripe leg
    //   (m_blocks set, m_newActiveWeakSets, the precise registry via
    //   tryAllocateLowerTierPrecise) is leaf-locked under
    //   m_markedSpaceRegistryLock (rank 7r).
    //
    // isHeld() preserves the legacy assertion idiom
    // `heap.mutatorSlowPathLock().isHeld()` verbatim (it appears in
    // out-of-file-set call sites: WeakSet.cpp, WeakSetInlines.h,
    // IncrementalSweeper.cpp, GCThreadLocalCache.cpp): true iff EITHER an
    // exclusive section or at least one stripe is currently held by some
    // thread — the same "held by ANYONE" weakness WTF::Lock::isHeld already
    // carried (documented at BlockDirectory.cpp assertNoUnswept).
    //
    // Flag-off / !isSharedServer(): no MutatorSlowPathLocker ever calls into
    // this facade (the locker's isSharedServer() gate short-circuits), so the
    // only observable difference from the old `Lock m_mutatorSlowPathLock` is
    // Heap layout — no flag-off code path executes any new instruction.
    class MutatorSlowPathLockFacade {
        WTF_MAKE_NONCOPYABLE(MutatorSlowPathLockFacade);
    public:
        MutatorSlowPathLockFacade() = default;

        void lock() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            Locker locker { m_stateLock };
            // Publish WAITING first so enterStripe()'s lock-free fast path
            // starts deferring immediately (writer-preferring drain).
            m_state.fetch_add(exclusiveWaitingOne, std::memory_order_relaxed);
            while (m_state.load(std::memory_order_relaxed) & (stripeDepthMask | exclusiveHeldBit))
                m_cond.wait(m_stateLock);
            // Order matters: set HELD before dropping our WAITING count so the
            // lock-free enterStripe()/tryLock() CAS never observes a
            // fully-clear gate between the two RMWs.
            m_state.fetch_or(exclusiveHeldBit, std::memory_order_acquire);
            m_state.fetch_sub(exclusiveWaitingOne, std::memory_order_relaxed);
            didAcquireExclusive();
        }

        // M2-alloc-tax-residual (c): non-blocking exclusive acquire. Fails if
        // ANY stripe is held, exclusive is held, OR an exclusive waiter is
        // queued (so a try never starves a draining lock()). Used only by the
        // shared-server steal-before-fresh leg in LocalAllocator::
        // allocateSlowCase: at W=1 the sole mutator just dropped its own
        // stripe → succeeds; at high W a sibling refill is almost always in a
        // stripe → fails fast (one uncontended CAS, no m_stateLock) and the
        // caller goes straight to its re-striped fresh-mint. Never called
        // flag-off / !isSharedServer().
        bool tryLock()
        {
            // Single CAS against the packed word: succeeds iff depth==0 &&
            // !held && waiting==0; on success atomically sets HELD so no
            // concurrent enterStripe() fast-path CAS can slip a stripe in.
            uint32_t expected = 0;
            if (!m_state.compare_exchange_strong(expected, exclusiveHeldBit, std::memory_order_acquire, std::memory_order_relaxed))
                return false;
            didAcquireExclusive();
            return true;
        }

        void unlock() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            Locker locker { m_stateLock };
            ASSERT(m_state.load(std::memory_order_relaxed) & exclusiveHeldBit);
            ASSERT(isHeldExclusivelyByCurrentThread());
            willReleaseExclusive();
            m_state.fetch_and(~exclusiveHeldBit, std::memory_order_release);
            m_cond.notifyAll();
        }

        // Lock-free fast path. With no exclusive section in flight or queued
        // this is one acquire-CAS and m_stateLock is never touched. Exclusive
        // sections do run while mutators allocate: the shared-mode
        // IncrementalSweeper takes lock() once per block on the main VM's run
        // loop after every cycle, and each such lock() drains every stripe
        // and routes concurrent refills through enterStripeSlow() until it
        // unlocks. Writer-preferring semantics are preserved: the CAS gate
        // tests HELD | WAITING, both packed into m_state, so a queued lock()
        // drains stripes exactly as before.
        void enterStripe() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            uint32_t state = m_state.load(std::memory_order_relaxed);
            do {
                if (state & exclusiveGateMask) [[unlikely]]
                    return enterStripeSlow();
                ASSERT((state >> stripeDepthShift) < stripeDepthLimit);
            } while (!m_state.compare_exchange_weak(state, state + stripeDepthOne, std::memory_order_acquire, std::memory_order_relaxed));
        }

        void exitStripe() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            uint32_t prev = m_state.fetch_sub(stripeDepthOne, std::memory_order_release);
            ASSERT(prev & stripeDepthMask);
            // Drain wake: we were the last stripe AND an exclusive waiter is
            // queued. Both fields come from the same atomic RMW result, so
            // this is totally ordered against lock()'s fetch_add(WAITING) —
            // either we observe the waiter here and notify, or lock()'s
            // subsequent load observes depth==0 and never waits.
            if ((prev & stripeDepthMask) == stripeDepthOne && (prev & exclusiveWaitingMask)) [[unlikely]] {
                Locker locker { m_stateLock };
                m_cond.notifyAll();
            }
        }

        // Debug-assert helper only (relaxed; no synchronization implied —
        // identical contract to WTF::Lock::isHeld).
        bool isHeld() const
        {
            return m_state.load(std::memory_order_relaxed) & (exclusiveHeldBit | stripeDepthMask);
        }

#if ASSERT_ENABLED
        // Unlike isHeld(), which is true while ANY thread holds a stripe or
        // the exclusive side, this identifies the exclusive owner, so an
        // assertion can require that the current thread is the one excluding
        // every stripe.
        bool isHeldExclusivelyByCurrentThread() const
        {
            return m_exclusiveOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton();
        }
#endif

    private:
#if ASSERT_ENABLED
        void didAcquireExclusive() { m_exclusiveOwner.store(&Thread::currentSingleton(), std::memory_order_relaxed); }
        void willReleaseExclusive() { m_exclusiveOwner.store(nullptr, std::memory_order_relaxed); }
#else
        void didAcquireExclusive() { }
        void willReleaseExclusive() { }
#endif

        NEVER_INLINE void enterStripeSlow() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
        {
            // Exclusive in flight or queued: block under m_stateLock so
            // unlock()/exitStripe()'s notifyAll can reach us. CAS (not
            // fetch_add) because a lock-free tryLock() may race the gate
            // clearing — if it wins HELD, our CAS fails and we re-wait.
            Locker locker { m_stateLock };
            for (;;) {
                uint32_t state = m_state.load(std::memory_order_relaxed);
                if (state & exclusiveGateMask) {
                    m_cond.wait(m_stateLock);
                    continue;
                }
                if (m_state.compare_exchange_weak(state, state + stripeDepthOne, std::memory_order_acquire, std::memory_order_relaxed))
                    return;
            }
        }

        // T3-mspl-facade-lockfree-stripe: {stripeDepth, exclusiveWaiting,
        // exclusiveHeld} packed into one atomic word so the high-rate stripe
        // enter/exit path is a single CAS / fetch_sub with no WTF::Lock hop.
        //   bit  0      : exclusiveHeld
        //   bits 1..15  : exclusiveWaiting count (sweep callers; <=2 in practice)
        //   bits 16..31 : stripeDepth count (<= client count)
        static constexpr uint32_t exclusiveHeldBit = 1u << 0;
        static constexpr unsigned exclusiveWaitingShift = 1;
        static constexpr uint32_t exclusiveWaitingOne = 1u << exclusiveWaitingShift;
        static constexpr uint32_t exclusiveWaitingMask = ((1u << 15) - 1u) << exclusiveWaitingShift;
        static constexpr unsigned stripeDepthShift = 16;
        static constexpr uint32_t stripeDepthOne = 1u << stripeDepthShift;
        static constexpr uint32_t stripeDepthMask = ~uint32_t { 0 } << stripeDepthShift;
        static constexpr uint32_t stripeDepthLimit = (1u << (32 - stripeDepthShift)) - 1u;
        // enterStripe()'s writer-preferring gate: held OR any waiter.
        static constexpr uint32_t exclusiveGateMask = exclusiveHeldBit | exclusiveWaitingMask;
        static_assert(!(exclusiveGateMask & stripeDepthMask));
        static_assert((exclusiveGateMask | stripeDepthMask) == ~uint32_t { 0 });

        std::atomic<uint32_t> m_state { 0 };
        // Slow-path blocking/wake protocol ONLY (exclusive acquire/release,
        // and the rare stripe-drains-for-exclusive handoff). Never touched on
        // the stripe fast path. Reachable only when isSharedServer()
        // (clients >= 2); W=1 / flag-off never enters the facade at all.
        Lock m_stateLock;
        Condition m_cond;
#if ASSERT_ENABLED
        std::atomic<Thread*> m_exclusiveOwner { nullptr };
#endif
    };

    // MSPL, rank 7 (SPEC-heap.md §6): serializes block handout, steals,
    // accounting, lower-tier precise allocation, addBlock resizes (I5b), and
    // precise-allocation registration (§5.6) when isSharedServer().
    // T7-mspl-per-directory: now an RW facade — see MutatorSlowPathLockFacade.
    MutatorSlowPathLockFacade& mutatorSlowPathLock() { return m_mutatorSlowPathLock; }

    // T7-mspl-per-directory: leaf lock (rank 7r) for the cross-directory
    // MarkedSpace state that stripe holders touch on the refill leg:
    // MarkedSpace::m_blocks (didAddBlock), m_newActiveWeakSets
    // (didAllocateInBlock / addActiveWeakSet), and the precise registry
    // (registerPreciseAllocation via IsoSubspace::tryAllocateLowerTierPrecise).
    // Taken ONLY when isSharedServer(); never held across BVL /
    // m_localAllocatorsLock / any rank>=8 lock; never held across an
    // allocation, sweep, or stop. Exclusive MSPL holders may also take it
    // (lock order: 7/7a -> 7r). Flag-off: never touched.
    Lock& markedSpaceRegistryLock() WTF_RETURNS_LOCK(m_markedSpaceRegistryLock) { return m_markedSpaceRegistryLock; }

    // Sticky ISS (§5.1/I13): set once the client set EVER reaches size() > 1
    // with the option on; cleared only via §10D reversion.
    //
    // Why a relaxed load is sound (review rounds 1+3): correctness never
    // rests on an isolated relaxed read observing the flip "in time". The
    // §10B.4 flip (noteSharedServerSticky) excludes any in-flight legacy
    // mutator, and — review round 3 — the exclusion is installed ATOMICALLY
    // with its own precondition: under quiescence clause (b) the flip's
    // gate-CAS pins hasAccessBit in m_worldState in the same atomic step
    // that verifies no thread holds legacy access (no TOCTOU window in
    // which a stale inline acquireAccess() CAS(0 -> hasAccessBit) could
    // still succeed); under clause (a) the flipping thread holds the main
    // VM's API lock, which orders the sole access holder and every later
    // JSLock entrant after the flip. Every path by which a thread
    // subsequently BEGINS a legacy heap operation therefore passes a
    // synchronization point ordered after the flip: the JSLock mutex
    // (API-lock entrants), or the pinned-bit "poison" that forces the
    // inline acquireAccess() CAS to fail into acquireAccessSlow(), whose
    // in-loop resolution locks *m_threadLock (held by the flip from the
    // gate-CAS through the ISS store) and re-reads ISS. After such an edge,
    // relaxed reads here are coherence-bound to return true.
    bool isSharedServer() const { return m_isSharedServer.load(std::memory_order_relaxed); }

    // SPEC-congc §5.2/§5.3(3) C1R (F33; ANNEX CGD4.4): the per-client
    // barrier-state routing predicate := useConcurrentSharedGCMarking && ISS.
    // Gates the CMS reroute and fence-copy read in addToRememberedSet, the
    // per-client didRun notes, and the WND-open fold loops. FALSE whenever
    // the C1 stage flag is off — the CG-T1 flag-off identity arm: C1R false
    // routes NOTHING (every consumer keeps the landed server path
    // byte-for-byte; CGD4.4 ruling, option (b)). The option byte is checked
    // FIRST so flags-off pays exactly one predicted-false Config-page test
    // and never loads the Heap line (FIX-V5B-F1 class).
    bool sharedGCBarrierStateIsPerClient() const
    {
        return Options::useConcurrentSharedGCMarking() && isSharedServer();
    }

    // UNGIL §0 U0c (ANNEX U0C, BINDING; U-T1): the GIL-off shared-server
    // DESIGNATION primitive — the s_stickySharedServer CAS, returning
    // won/lost, NO assert (noteSharedServerSticky's inner CAS
    // RELEASE_ASSERTs — I13 — so it cannot BE the designation). Under
    // gilOffProcess every VM ctor calls this; the winner sets vm.m_gilOff=1
    // and eagerly calls noteSharedServerSticky() at clientSet()==1
    // (quiescence trivial at birth; I13 sees previous==this and never
    // fires). Idempotent for the winner.
    JS_EXPORT_PRIVATE bool tryDesignateStickySharedServer();

    // The designation check: RELEASE_ASSERT(gilOffProcess => the server
    // VM's m_gilOff == 1), run immediately before every
    // noteSharedServerSticky() trigger — the winner VM ctor's eager
    // clientSet()==1 trigger and HeapClientSet::add's second-client
    // trigger. Under gilOffProcess a LOSER VM (m_gilOff == 0) can never
    // legally reach a trigger (spawn refusal keeps its clientSet() <= 1),
    // so this fail-stops the bug precisely instead of leaving it to I13's
    // inner CAS. No-op (early return) when !gilOffProcess. Defined in
    // runtime/VM.cpp because it needs the complete VM type.
    JS_EXPORT_PRIVATE void verifyStickySharedServerDesignation();

    // WSAC (F7): written only by the conductor under m_gcBarrierLock; reads acquire.
    bool worldIsStoppedForAllClients() const { return m_worldIsStoppedForAllClients.load(std::memory_order_acquire); }

    // SPEC-ungil §B (heap Dev 8: ONE GCClient PER Thread) / I4: resolve the
    // GCClient whose TLC/iso LocalAllocators the CURRENT thread may allocate
    // through. Gate is vm.gilOff(), NOT isSharedServer(): GIL-on and
    // flag-off stay identity BY CONSTRUCTION (a GIL-on thread nested across
    // two VMs sharing one server must not re-route the outer VM's
    // allocations into the inner VM's client), and spawned-client stamps
    // cannot exist GIL-on. Unstamped threads (GC helpers, pre-attach) fall
    // back to the VM's original client under the access-owner identity
    // tripwire. Templated on VMType (always VM) ONLY so the ALWAYS_INLINE
    // body can live in this header — VM is incomplete here, and an
    // out-of-line call on the iso fast path would regress the flag-off bench
    // gate. Defined at the bottom of this header (needs GCClient::Heap),
    // next to the deferralDepthSlot()/mutatorStateSlot() dispatchers it
    // mirrors.
    template<typename VMType>
    static GCClient::Heap& allocationClientForCurrentThread(VMType& vm, GCClient::Heap& vmOriginalClient);

    // GSP (F8): read-only view of the stop-pending flag; seq_cst.
    bool gcStopPendingForAllClients() const { return m_gcStopPending.load(std::memory_order_seq_cst); }

    // §10 preconditions: caller holds its client's heap access; no rank >= 4
    // or SAL lock; not inside a stop window.
    JS_EXPORT_PRIVATE void collectSyncAllClients(CollectionScope);
    JS_EXPORT_PRIVATE void requestCollectionAllClients(GCRequest);
    JS_EXPORT_PRIVATE void stopIfNecessaryForAllClients(); // §10A poll, from collectIfNecessaryOrDefer.

    GCSafepointEpoch& safepointEpoch() LIFETIME_BOUND { return m_safepointEpoch; }

    // Allocated only under useJSThreads (see StructureRareData.h).
    RetiredStructureChainInvalidationWatchpoints& retiredStructureChainInvalidationWatchpoints() LIFETIME_BOUND
    {
        ASSERT(m_retiredStructureChainInvalidationWatchpoints);
        return *m_retiredStructureChainInvalidationWatchpoints;
    }

    // Registers a hook run world-stopped (§9 contract notes): legacy
    // (!isSharedServer(), incl. option-off) once per collection in
    // runEndPhase just before didFinishCollection(); shared mode once per
    // drained ticket batch at §10 step 7 (conductSharedCollection). Used by
    // the object-model workstream (quarantined-slot release). The registry is
    // per-Heap and has no removal API; re-adding a hook this heap already has
    // is a no-op, so a caller that cannot tell whether it registered on THIS
    // heap (a second VM in the process, or a heap reusing a destroyed heap's
    // address) simply adds again.
    JS_EXPORT_PRIVATE void addStopTheWorldSafepointHook(void (*)(JSC::Heap&));

    // STW-forbidden scope (I14): a holder of the structure-allocation lock
    // (and similar) must not initiate/join/wait for a stop-the-world. The
    // depth is a release-mode per-thread counter: collectIfNecessaryOrDefer
    // consults it to defer stop polls and GC initiation reached inside such a
    // region, and the §10 entry points assert it is zero.
    JS_EXPORT_PRIVATE void incrementSTWForbiddenScope();
    JS_EXPORT_PRIVATE void decrementSTWForbiddenScope();

    // §10A.1: !isSharedServer() => today's API-lock predicate; shared =>
    // membership of the LocalAllocator in the current thread's client TLC.
    bool currentThreadIsAllocatorOwner(const LocalAllocator*) const;

    // §10C/CS2 (jit CS2 resolution): rank-2 m_gcConductorLock RAII bracket
    // for a JSThreads/debugger stop. Pre: heap access released; no
    // bumpAndReclaim inside; !isSharedServer(): no-op.
    class JSThreadsStopScope {
        WTF_MAKE_NONCOPYABLE(JSThreadsStopScope);
    public:
        // Acquires the GCL in bounded tryLock quanta, calling
        // JSThreadsSafepoint::watchdogAssertStopProgress against
        // `watchdogRequestStart` per quantum, so a conductor wedged behind a
        // non-converging shared GC fail-stops at the 30s bound instead of
        // hanging unwatched. Pass the same requestStart that covers the
        // predicate-wait phase: reaching conductor tenure is part of reaching
        // a stopped world. A caller with no earlier request passes
        // MonotonicTime::now().
        JS_EXPORT_PRIVATE JSThreadsStopScope(JSC::Heap&, MonotonicTime watchdogRequestStart);
        JS_EXPORT_PRIVATE ~JSThreadsStopScope();
    private:
        JSC::Heap& m_heap;
        bool m_didLock { false };
        // SPEC-congc §9.1(2) (CG-3b): set iff this scope's ctor found
        // m_currentPhase != NotRunning after acquiring GCL and ran
        // pauseConcurrentMarkingForForeignStop(); the dtor then resumes
        // BEFORE releasing GCL (dtor order NORMATIVE — no WND-open with
        // paused markers; paired resume in the same scope, CG-I16).
        bool m_didPauseConcurrentMarking { false };
    };

    // --- End shared heap server interface ---

    // We're always busy on the collection threads. On the main thread, this returns true if we're
    // helping heap.
    JS_EXPORT_PRIVATE bool currentThreadIsDoingGCWork();
    
    typedef void (*CFinalizer)(JSCell*);
    JS_EXPORT_PRIVATE void addFinalizer(JSCell*, CFinalizer);
    // A LambdaFinalizer may be called with a null JSCell*: under GIL-off the
    // shared collector runs lambda finalizers after the world resumes, when
    // the dead cell's memory may already hold a new cell, so the argument must
    // never be dereferenced or used as an identity key. A finalizer that needs
    // the cell address must be a CFinalizer, which always runs inside the sweep.
    using LambdaFinalizer = WTF::Function<void(JSCell*)>;
    JS_EXPORT_PRIVATE void addFinalizer(JSCell*, LambdaFinalizer);

    void notifyIsSafeToCollect();
    bool isSafeToCollect() const { return m_isSafeToCollect; }
    
    bool isShuttingDown() const { return m_isShuttingDown; }

    JS_EXPORT_PRIVATE void sweepSynchronously();

    bool shouldCollectHeuristic();
    
    // Queue up a collection. Returns immediately. This will not queue a collection if a collection
    // of equal or greater strength exists. Full collections are stronger than std::nullopt collections
    // and std::nullopt collections are stronger than Eden collections. std::nullopt means that the GC can
    // choose Eden or Full. This implies that if you request a GC while that GC is ongoing, nothing
    // will happen.
    JS_EXPORT_PRIVATE void collectAsync(GCRequest = GCRequest());
    
    // Queue up a collection and wait for it to complete. This won't return until you get your own
    // complete collection. For example, if there was an ongoing asynchronous collection at the time
    // you called this, then this would wait for that one to complete and then trigger your
    // collection and then return. In weird cases, there could be multiple GC requests in the backlog
    // and this will wait for that backlog before running its GC and returning.
    JS_EXPORT_PRIVATE void collectSync(GCRequest = GCRequest());
    
    JS_EXPORT_PRIVATE void collect(Synchronousness, GCRequest = GCRequest());
    
    // Like collect(), but in the case of Async this will stopIfNecessary() and in the case of
    // Sync this will sweep synchronously.
    JS_EXPORT_PRIVATE void collectNow(Synchronousness, GCRequest = GCRequest());
    
    JS_EXPORT_PRIVATE void collectNowFullIfNotDoneRecently(Synchronousness);
    
    void collectIfNecessaryOrDefer(GCDeferralContext* = nullptr);

    void completeAllJITPlans();
    
    // Note that:
    // 1. Use this API to report non-GC memory referenced by GC objects. Be sure to
    // call both of these functions: Calling only one may trigger catastropic
    // memory growth.
    // 2. Use this API may trigger JSRopeString::resolveRope. If this API need
    // to be used when resolving a rope string, then make sure to call this API
    // after the rope string is completely resolved.
    void reportExtraMemoryAllocated(const JSCell* cell, size_t size)
    {
        if (size > minExtraMemory)
            reportExtraMemoryAllocatedSlowCase(nullptr, cell, size);
    }
    void reportExtraMemoryAllocated(GCDeferralContext* deferralContext, const JSCell* cell, size_t size)
    {
        if (size > minExtraMemory)
            reportExtraMemoryAllocatedSlowCase(deferralContext, cell, size);
    }
    JS_EXPORT_PRIVATE void reportExtraMemoryVisited(size_t);

#if ENABLE(RESOURCE_USAGE)
    // Use this API to report the subset of extra memory that lives outside this process.
    JS_EXPORT_PRIVATE void reportExternalMemoryVisited(size_t);
    size_t externalMemorySize() { return m_externalMemorySize; }
#endif

    // Use this API to report non-GC memory if you can't use the better API above.
    void deprecatedReportExtraMemory(size_t size)
    {
        if (size > minExtraMemory)
            deprecatedReportExtraMemorySlowCase(size);
    }

    JS_EXPORT_PRIVATE void reportAbandonedObjectGraph();

    JS_EXPORT_PRIVATE void protect(JSValue);
    JS_EXPORT_PRIVATE bool unprotect(JSValue); // True when the protect count drops to 0.
    
    JS_EXPORT_PRIVATE size_t extraMemorySize(); // Non-GC memory referenced by GC objects.
    JS_EXPORT_PRIVATE size_t size();
    JS_EXPORT_PRIVATE size_t capacity();
    JS_EXPORT_PRIVATE size_t objectCount();
    JS_EXPORT_PRIVATE size_t globalObjectCount();
    JS_EXPORT_PRIVATE size_t protectedObjectCount();
    JS_EXPORT_PRIVATE size_t protectedGlobalObjectCount();
    JS_EXPORT_PRIVATE TypeCountSet protectedObjectTypeCounts();
    JS_EXPORT_PRIVATE TypeCountSet objectTypeCounts();
    JS_EXPORT_PRIVATE size_t arrayBufferSize();

    UncheckedKeyHashSet<MarkedVectorBase*>& markListSet() { return m_markListSet; }

    // DW-2 (deepwater LEDGER row 2): under Options::useSharedGCHeap() every
    // mutator thread's MarkedVector/MarkedArgumentBuffer spill path
    // (MarkedVector.h fill/fillWith, MarkedVectorBase::addMarkSet) registers
    // with this ONE server Heap concurrently, and unregisters from arbitrary
    // threads in ~MarkedVectorBase — confirmed UAF/SEGV in
    // HashTable::removeIterator/add via MarkedVector::fill <- sortImpl <-
    // arrayProtoFuncSort on a spawned Thread. Shared mode therefore routes
    // registration through this SHARDED, per-shard-locked variant instead of
    // m_markListSet. Sharding (not one per-heap lock, and explicitly not the
    // dive's recon-only global s_limpMarkListSetLock) because this is the hot
    // spill path of every big sort/apply: shard choice hashes the vector's
    // address, so concurrent threads' registrations land on different shards
    // with high probability and each shard lock is held only for one hash-set
    // add/remove. Flag-off (!useSharedGCHeap) never touches the shards: the
    // m_markListSet registration/marking paths and the lock-free accessor
    // above are unchanged, and MarkedVectorBase's layout and constructor are
    // identical to the pre-shard engine; only the registration and
    // unregistration slow paths (spilled vectors) test useSharedGCHeap().
    static constexpr unsigned numMarkListSetShards = 32;
    static_assert(!(numMarkListSetShards & (numMarkListSetShards - 1)), "shard count must be a power of two");
    struct MarkListSetShard {
        Lock lock;
        UncheckedKeyHashSet<MarkedVectorBase*> set WTF_GUARDED_BY_LOCK(lock);

        // A shared-mode MarkedVectorBase records only &set; this recovers the
        // shard whose lock guards it.
        static MarkListSetShard& fromSet(UncheckedKeyHashSet<MarkedVectorBase*>& set)
        {
            return *std::bit_cast<MarkListSetShard*>(std::bit_cast<uintptr_t>(&set) - OBJECT_OFFSETOF(MarkListSetShard, set));
        }
    };
    MarkListSetShard& markListSetShard(MarkedVectorBase* vector)
    {
        uintptr_t word = std::bit_cast<uintptr_t>(vector);
        // MarkedVectors are stack-resident (WTF_FORBID_HEAP_ALLOCATION) and
        // at least 16-byte aligned; fold the page-granularity bits in so the
        // per-thread stack base (which differs across threads) contributes
        // even when frame-local offsets collide.
        unsigned index = static_cast<unsigned>((word >> 4) ^ (word >> 12)) & (numMarkListSetShards - 1);
        return m_markListSetShards[index];
    }

    template<typename Functor> inline void forEachProtectedCell(const Functor&);
    template<typename Functor> inline void forEachCodeBlock(NOESCAPE const Functor&);
    template<typename Functor> inline void forEachCodeBlockIgnoringJITPlans(const AbstractLocker& codeBlockSetLocker, NOESCAPE const Functor&);

    HandleSet* handleSet() LIFETIME_BOUND { return &m_handleSet; }

    JS_EXPORT_PRIVATE void willStartIterating();
    JS_EXPORT_PRIVATE void didFinishIterating();

    Seconds lastFullGCLength() const { return m_lastFullGCLength; }
    Seconds lastEdenGCLength() const { return m_lastEdenGCLength; }
    void increaseLastFullGCLength(Seconds amount) { m_lastFullGCLength += amount; }

    size_t sizeBeforeLastEdenCollection() const { return m_sizeBeforeLastEdenCollect; }
    size_t sizeAfterLastEdenCollection() const { return m_sizeAfterLastEdenCollect; }
    size_t sizeBeforeLastFullCollection() const { return m_sizeBeforeLastFullCollect; }
    size_t sizeAfterLastFullCollection() const { return m_sizeAfterLastFullCollect; }

    void deleteAllCodeBlocks(DeleteAllCodeEffort);
    void deleteAllUnlinkedCodeBlocks(DeleteAllCodeEffort);

    JS_EXPORT_PRIVATE void didAllocate(size_t);
    bool isPagedOut();
    
    const JITStubRoutineSet& jitStubRoutines() { return *m_jitStubRoutines; }
    
    void addReference(JSCell*, ArrayBuffer*);

    // GIL-off: serializes mutation AND reads of each ArrayBuffer's
    // incoming-reference storage (see GCIncomingRefCountedSet.h). Unlocked
    // readers (ArrayBuffer::notifyDetaching / refreshAfterWasmMemoryGrow)
    // snapshot the cell list under this lock; it is a strict heap-rank leaf.
    Lock& arrayBufferIncomingReferencesLock() LIFETIME_BOUND { return m_arrayBuffers.lock(); }

    // SharedGC (§5.4/I17): once ISS, DeferGC depth is per-CLIENT — this
    // consults the calling thread's client depth (defined after
    // GCClient::Heap at the bottom of this header). !ISS => the server
    // counter, exactly today's behavior (I10).
    inline bool isDeferred() const;

    CodeBlockSet& codeBlockSet() { return *m_codeBlocks; }

    // SPEC-jit §5.8/§4.4 (record-named CodeBlock identity; w16 follow-up
    // jit-null-metadatatable-counter-bump): a replaced/unlinked call-link
    // record is retired through RetiredJITArtifacts (epoch-deferred), which
    // keeps the RECORD memory and the target machine code dispatchable for a
    // straggler that loaded `r = m_record` before the replacement. But the record's
    // `codeBlockToTransfer` is a raw GC-cell pointer the straggler stores
    // into the callee frame BEFORE any conservative root can see it (the
    // only copies live inside the record itself during the few-instruction
    // dispatch window, I16). Nothing kept that CELL alive: the GC could
    // sweep the named CodeBlock and recycle its IsoSubspace slot, so the
    // straggler transferred a WRONG live CodeBlock into the callee frame —
    // observed as the unlinked-DFG/baseline prologue materializing
    // m_jitData==null from a recycled FTL CodeBlock cell and crashing on the
    // tier-up counter bump at jitDataRegister+offsetOfJITExecuteCounter
    // (SIGSEGV write at 0x28, r13==0, scalebench W=16). These pins make every
    // record's named CodeBlock a validated GC root for exactly the record's
    // lifetime: pinned at publish, unpinned when the record is actually
    // destroyed (epoch expiry). Marking validates each
    // entry against codeBlockSet() under its lock, so an entry whose cell
    // the GC already declared dead (removed by
    // clearCurrentlyExecutingAndRemoveDeadCodeBlocks before any pin-driven
    // mark could retain it) is skipped, never resurrected; a recycled slot
    // re-added as a NEW CodeBlock is over-marked (kept alive) — benign.
    // Counted: multiple retired records may name one CodeBlock. Lock order:
    // the pin lock is taken OUTSIDE codeBlockSet()'s lock (the marking
    // constraint holds both, pin lock first); pin/unpin take only the pin
    // lock and never run under the codeBlockSet lock. Callable from any
    // mutator (the §5.8 writers run under
    // CallLinkInfo::s_callLinkSerializationLock, which never parks).
    void pinRetiredCallLinkRecordCodeBlock(void* codeBlock);
    void unpinRetiredCallLinkRecordCodeBlock(void* codeBlock);

#if USE(FOUNDATION)
    template<typename T> inline void releaseSoon(RetainPtr<T>&&);
#endif
#ifdef JSC_GLIB_API_ENABLED
    inline void releaseSoon(std::unique_ptr<JSCGLibWrapperObject>&&);
#endif

    JS_EXPORT_PRIVATE void registerWeakGCHashTable(WeakGCHashTable*);
    JS_EXPORT_PRIVATE void unregisterWeakGCHashTable(WeakGCHashTable*);

    void addLogicallyEmptyWeakBlock(WeakBlock*);

#if ENABLE(RESOURCE_USAGE)
    size_t blockBytesAllocated() const { return m_blockBytesAllocated.load(std::memory_order_relaxed); }
#endif

    void didAllocateBlock(size_t capacity);
    void didFreeBlock(size_t capacity);
    
    // The inline C++ barrier and emitted code both read the SERVER master
    // pair. Once shared, the master is mutated only inside a stop window and
    // closeSharedGCStopWindow republishes it to every client's copy before
    // that window closes, so a running mutator never observes a copy that
    // differs from the master; only addToRememberedSet's per-client routing
    // (sharedGCBarrierStateIsPerClient()) reads a client copy.
    bool mutatorShouldBeFenced() const { return m_mutatorShouldBeFenced; }
    const bool* addressOfMutatorShouldBeFenced() const LIFETIME_BOUND { return &m_mutatorShouldBeFenced; }

    unsigned barrierThreshold() const { return m_barrierThreshold; }
    const unsigned* addressOfBarrierThreshold() const LIFETIME_BOUND { return &m_barrierThreshold; }

    // If true, the GC believes that the mutator is currently messing with the heap. We call this
    // "having heap access". The GC may block if the mutator is in this state. If false, the GC may
    // currently be doing things to the heap that make the heap unsafe to access for the mutator.
    bool hasAccess() const
    {
        // SharedGC (§10A): forwarded to the main client once ISS (see
        // mainClientHasHeapAccess() for the owner-sensitive semantics that
        // keep JSLock migration re-stamping the owner + TLS).
        if (isSharedServer()) [[unlikely]]
            return mainClientHasHeapAccess();
        return m_worldState.loadRelaxed() & hasAccessBit;
    }
    
    // If the mutator does not currently have heap access, this function will acquire it. If the GC
    // is currently using the lack of heap access to do dangerous things to the heap then this
    // function will block, waiting for the GC to finish. It's not valid to call this if the mutator
    // already has heap access. The mutator is required to precisely track whether or not it has
    // heap access.
    //
    // It's totally fine to acquireAccess() upon VM instantiation and keep it that way. This is how
    // WebCore uses us. For most other clients, JSLock does acquireAccess()/releaseAccess() for you.
    void acquireAccess();
    
    // Releases heap access. If the GC is blocking waiting to do bad things to the heap, it will be
    // allowed to run now.
    //
    // Ordinarily, you should use the ReleaseHeapAccessScope to release and then reacquire heap
    // access. You should do this anytime you're about do perform a blocking operation, like waiting
    // on the ParkingLot.
    void releaseAccess()
    {
        // SharedGC (§10A): once ISS, JSLock::willReleaseLock's call here
        // forwards to the main client's releaseHeapAccess() (RHA).
        if (isSharedServer()) [[unlikely]] {
            releaseAccessForwardedToMainClient();
            return;
        }
        // Why a stale-ISS release can never clear the §10B.4 poison (review
        // round 3): a releaser must hold legacy access. Post-flip,
        // un-forwarded legacy holders cannot exist — the flip's clause-(b)
        // gate-CAS pins hasAccessBit atomically with verifying that no
        // holder exists, and clause (a) orders the sole (API-lock-coupled)
        // holder after the flip via the JSLock mutex, so its ISS read here
        // is true and it forwards above. Post-§10D-reversion, the pinned
        // bit denotes the main mutator's REAL access, so the CAS below is
        // then a correct release. releaseAccessSlow() re-checks ISS inside
        // its retry loop as the backstop.
        if (m_worldState.compareExchangeWeak(hasAccessBit, 0))
            return;
        releaseAccessSlow();
    }
    
    // This is like a super optimized way of saying:
    //
    //     releaseAccess()
    //     acquireAccess()
    //
    // The fast path is an inlined relaxed load and branch. The slow path will block the mutator if
    // the GC wants to do bad things to the heap.
    //
    // All allocations logically call this. As an optimization to improve GC progress, you can call
    // this anywhere that you can afford a load-branch and where an object allocation would have been
    // safe.
    //
    // In the legacy (single-client) protocol the GC will also push a stopIfNecessary() event onto
    // the runloop of the thread that instantiated the VM whenever it wants the mutator to stop. This
    // means that if you never block but instead use the runloop to wait for events, then you could
    // safely run in a mode where the mutator has permanent heap access (like the DOM does). If you
    // have good event handling discipline (i.e. you don't block the runloop) then you can be sure
    // that stopIfNecessary() will already be called for you at the right times.
    //
    // Once the heap is a shared server (isSharedServer(), per-thread clients) there is no such push:
    // a stop-the-world traps only threads executing JS, and a client holding access while its thread
    // is outside JS is reached only by its own stopIfNecessary() poll or releaseAccess(). Permanent
    // heap access is therefore not an option there; release access (ReleaseHeapAccessScope) across
    // every blocking section, or the stop barrier waits on you (it logs the holder after 5s).
    inline void stopIfNecessary();
    
    // This gives the conn to the collector.
    void relinquishConn();
    
    bool mayNeedToStop() { return m_worldState.loadRelaxed() != hasAccessBit; }

    void performIncrement(size_t bytes);
    
    // This is a much stronger kind of stopping of the collector, and it may require waiting for a
    // while. This is meant to be a legacy API for clients of collectAllGarbage that expect that there
    // is no GC before or after that function call. After calling this, you are free to start GCs
    // yourself but you can be sure that none are running.
    //
    // This both prevents new collections from being started asynchronously and waits for any
    // outstanding collections to complete.
    void preventCollection();
    void allowCollection();
    
    uint64_t mutatorExecutionVersion() const { return m_mutatorExecutionVersion; }
    uint64_t phaseVersion() const { return m_phaseVersion; }
    
    JS_EXPORT_PRIVATE void addMarkingConstraint(std::unique_ptr<MarkingConstraint>);
    
    HeapVerifier* verifier() const LIFETIME_BOUND { return m_verifier.get(); }
    
    void addHeapFinalizerCallback(const HeapFinalizerCallback&);
    void removeHeapFinalizerCallback(const HeapFinalizerCallback&);
    
    void runTaskInParallel(RefPtr<SharedTask<void(SlotVisitor&)>>);
    
    template<typename Func>
    void runFunctionInParallel(const Func& func)
    {
        runTaskInParallel(createSharedTask<void(SlotVisitor&)>(func));
    }

    template<typename Func>
    inline void forEachSlotVisitor(const Func&);
    
    Seconds totalGCTime() const { return m_totalGCTime; }

    UncheckedKeyHashMap<JSCellButterfly*, JSString*> immutableButterflyToStringCache;

    bool isMarkingForGCVerifier() const { return m_isMarkingForGCVerifier; }

    void setKeepVerifierSlotVisitor();
    void clearVerifierSlotVisitor();

    void appendPossiblyAccessedStringFromConcurrentThreads(String&& string, bool gilOff)
    {
        // GIL-off, N mutators reach this from JSString::swapToAtomString
        // concurrently — an unlocked Vector::append races reserveCapacity
        // (one thread memcpys out of a buffer a sibling just freed). GIL-on
        // and flag-off have one mutator (the JSLock holder) and the GC-end
        // clear runs world-stopped, so they keep the plain append.
        if (gilOff) [[unlikely]] {
            Locker locker { m_possiblyAccessedStringsFromConcurrentThreadsLock };
            m_possiblyAccessedStringsFromConcurrentThreads.append(WTF::move(string));
            return;
        }
        m_possiblyAccessedStringsFromConcurrentThreads.append(WTF::move(string));
    }

    bool isInPhase(CollectorPhase phase) const { return m_currentPhase == phase; }

#if ENABLE(WEBASSEMBLY)
    // FIXME: We should have a way to clear Wasm::Callees pending destruction when the Module dies.
    void reportWasmCalleePendingDestruction(Ref<Wasm::Callee>&&);
    bool isWasmCalleePendingDestruction(Wasm::Callee&);

    const TinyBloomFilter<uintptr_t>& boxedWasmCalleeFilter() const { return m_boxedWasmCalleeFilter; }
    bool didDiscoverPendingWasmCallee(Wasm::Callee*);
#endif

    // This is a debug function for checking who marked the target cell.
    void dumpVerifierMarkerData(HeapCell*);

private:
    friend class AllocatingScope;
    friend class CodeBlock;
    friend class CollectingScope;
    friend class ConservativeRoots;
    friend class DeferGC;
    friend class DeferGCForAWhile;
    friend class GCAwareJITStubRoutine;
    friend class GCLogging;
    friend class GCThread;
    friend class HandleSet;
    friend class HeapUtil;
    friend class HeapVerifier;
    friend class JITStubRoutine;
    friend class LLIntOffsetsExtractor;
    friend class MarkStackMergingConstraint;
    friend class MarkedSpace;
    friend class BlockDirectory;
    friend class MarkedBlock;
    friend class RunningScope;
    friend class SlotVisitor;
    friend class SpaceTimeMutatorScheduler;
    friend class StochasticSpaceTimeMutatorScheduler;
    friend class SweepingScope;
    friend class IncrementalSweeper;
    friend class VM;
    friend class VerifierSlotVisitor;
    friend class WeakSet;

    class HeapThread;
    friend class HeapThread;

    friend class GCClient::Heap;
    friend class JSC::HeapClientSet;
    friend class SharedHeapTestHarness; // T10 (§12.1): standalone scenarios drive the private per-client deferral-depth routing (I17) directly.
    friend class VM; // UNGIL §0 U0c (U-T1): the winner's VM ctor calls the private noteSharedServerSticky() eagerly at clientSet()==1.

    // THREADS (SPEC-heap.md): shared-server internals (T1 scaffolding).
    void noteSharedServerSticky(); // Sticky ISS switch (§10B.4 quiescence); I13 one-shared-server assert.
    void verifyServerNonIsoAllocatorsNeverMaterialized(); // §5.5 never-populate audit (T4); RELEASE_ASSERTs at second-client attach.
    void runStopTheWorldSafepointHooks(); // World-stopped; once per legacy collection, once per shared-mode drained ticket batch (§9).
    static bool currentThreadHasSTWForbiddenScope(); // I14; release-real per-thread depth test, read by collectIfNecessaryOrDefer.

    // §10A access forwarding (T2): once ISS, the legacy server-level
    // acquireAccess()/releaseAccess()/hasAccess() (called by JSLock and
    // ReleaseHeapAccessScope on the main VM's behalf) forward to the main
    // client's AHA/RHA GIL-on; GIL-off they act on the calling thread's own
    // client (gilOffClientForServerLevelAccess). JS_EXPORT_PRIVATE because
    // the inline callers above are instantiated outside JSC.
    JS_EXPORT_PRIVATE void acquireAccessForwardedToMainClient();
    JS_EXPORT_PRIVATE void releaseAccessForwardedToMainClient();
    JS_EXPORT_PRIVATE bool mainClientHasHeapAccess() const;
    GCClient::Heap* gilOffClientForServerLevelAccess() const;

    static constexpr size_t minExtraMemory = 256;
    
    class CFinalizerOwner final : public WeakHandleOwner {
        void finalize(Handle<Unknown>, void* context) final;
    };

    class LambdaFinalizerOwner final : public WeakHandleOwner {
    public:
        explicit LambdaFinalizerOwner(JSC::Heap& heap) : m_heap(heap) { }
    private:
        void finalize(Handle<Unknown>, void* context) final;
        JSC::Heap& m_heap;
    };

    Lock& lock() LIFETIME_BOUND { return m_lock; }

    void reportExtraMemoryAllocatedPossiblyFromAlreadyMarkedCell(const JSCell*, size_t);
    JS_EXPORT_PRIVATE void reportExtraMemoryAllocatedSlowCase(GCDeferralContext*, const JSCell*, size_t);
    JS_EXPORT_PRIVATE void deprecatedReportExtraMemorySlowCase(size_t);
    
    size_t totalBytesAllocatedThisCycle() { return m_nonOversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed) + m_oversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed); } // F3: relaxed; exact at safepoints (I7).

    bool shouldCollectInCollectorThread(const AbstractLocker&);
    void collectInCollectorThread();
    
    void checkConn(GCConductor);

    enum class RunCurrentPhaseResult {
        Finished,
        Continue,
        NeedCurrentThreadState
    };
    RunCurrentPhaseResult runCurrentPhase(GCConductor, CurrentThreadState*);
    
    // Returns true if we should keep doing things.
    bool runNotRunningPhase(GCConductor);
    bool runBeginPhase(GCConductor);
    bool runFixpointPhase(GCConductor);
    bool runConcurrentPhase(GCConductor);
    bool runReloopPhase(GCConductor);
    bool runEndPhase(GCConductor);
    bool changePhase(GCConductor, CollectorPhase);
    bool finishChangingPhase(GCConductor);
    
    void collectInMutatorThread();
    
    void stopThePeriphery(GCConductor);
    void resumeThePeriphery();
    
    // Returns true if the mutator is stopped, false if the mutator has the conn now.
    bool stopTheMutator();
    void resumeTheMutator();
    
    JS_EXPORT_PRIVATE void stopIfNecessarySlow();
    bool stopIfNecessarySlow(unsigned extraStateBits);
    
    template<typename Func>
    void waitForCollector(const Func&);
    // W16-C1 residual (a): out-of-line rate-limited stall dump for the
    // shared-mode waitForCollector wait loop (keep-waiting + rate-limited
    // dump, the STW-watchdog policy shape — deepwater LEDGER §2/§3 ruling).
    // Caller holds *m_threadLock.
    NEVER_INLINE void dumpSharedGCWaitForCollectorStall(Seconds elapsed);
    // Same policy for the §10.4 access barrier in openSharedGCStopWindow:
    // lists the clients still holding access. Caller holds m_gcBarrierLock.
    NEVER_INLINE void dumpSharedGCAccessBarrierStall(Seconds elapsed);

    JS_EXPORT_PRIVATE void acquireAccessSlow();
    JS_EXPORT_PRIVATE void releaseAccessSlow();
    
    bool handleNeedFinalize(unsigned);
    void handleNeedFinalize();
    
    bool relinquishConn(unsigned);
    void finishRelinquishingConn();
    
    void setNeedFinalize();
    void waitWhileNeedFinalize();
    
    void setMutatorWaiting();
    void clearMutatorWaiting();
    void notifyThreadStopping(const AbstractLocker&);
    
    typedef uint64_t Ticket;
    Ticket requestCollection(GCRequest);
    void waitForCollection(Ticket);

    // THREADS T5 (SPEC-heap.md §10): shared-mode requester-conducted stop.
    Ticket requestCollectionShared(GCRequest); // §10B.1 ticketing (RCAC core); pre: access holder or conductor.
    Ticket requestCollectionShared(const AbstractLocker& threadLockLocker, GCRequest); // Same, with *m_threadLock already held.
    void runSharedGCElection(Ticket); // §10.2 election loop; returns once the ticket is served.
    bool tryConductSharedCollectionForPoll(GCClient::Heap&); // Non-blocking election attempt (SINFAC/CIND poll service).
    void conductSharedCollection(GCClient::Heap&); // §10 steps 3-9; pre: GCL held, GCA set.

public:
    // T1-gc-siblings-mark: called by a gilOff Mode-machine SIBLING parked
    // access-released at VMManager::notifyVMStop. If a conducted cycle's
    // marking phase is open (m_siblingMarkingAssistEnabled under
    // m_markingMutex), acquires/creates a sibling SlotVisitor and runs
    // drainFromShared(HelperDrain) to termination of THIS marking phase
    // (returns when runEndPhase sets m_parallelMarkersShouldExit). Returns
    // true iff the sibling joined a marking phase (caller re-evaluates the
    // Mode/stop predicate); false means no marking is open — caller falls
    // back to the bounded condvar poll. Pre: caller is heap-access-released
    // (gcClientWillParkForThreadGranularStop already ran) and holds no
    // VMManager / api-rank lock. The HelperDrain protocol (m_markingMutex /
    // m_markingConditionVariable, F14/F17, the §9.1(2) pause checkpoints)
    // is already N-helper-safe; siblings are indistinguishable from pool
    // helpers to the marker-counter machine. Flag-off: unreachable (the
    // only call site is inside the [[unlikely]] vm.gilOff() branch).
    bool gilOffSiblingAssistMarking();
private:

    // SPEC-congc §3 window model (CG-1, C0 infra). A shared collection is ONE
    // GCA tenure containing a SEQUENCE of stop windows. Flag-off (every §13.2
    // stage flag false) the conduct is exactly ONE window (§3.6 degenerate):
    // FirstWindow open + final close — byte-for-byte today's steps 3-4/8-9
    // (CG-I0). The Reentry arm and the non-final close run only on the
    // Concurrent-phase edges of a windowed conduct (finishChangingPhase).
    // Every conduct performs exactly one FirstWindow open and one final close;
    // tickets drained by the same conduct run inside the predecessor cycle's
    // still-open final window.
    enum class SharedGCWindowOpen : uint8_t {
        // F15 FIRST-WINDOW carve-out: the first WND-open IS the landed entry —
        // GCL was tryLock'd access-HELD by the election/poll caller; this arm
        // runs GSP, THEN the step-3 access release, then steps (d)-(e).
        FirstWindow,
        // §3.1(a)-(e) re-entry order: access stays released all tenure (§3.7);
        // F45 foreign-waiter deferral; BLOCKING GCL acquire (legal exactly
        // because access-released — ungil §A.3 rule 2 / HBT4 extended,
        // ANNEX CGS2.4(b)); then (c)-(e).
        Reentry,
    };
    void openSharedGCStopWindow(GCClient::Heap& conductorClient, SharedGCWindowOpen); // WND-open (§3.1).
    void closeSharedGCStopWindow(bool isFinalClose); // WND-close (§3.2; F23: final close leaves GCL HELD; access re-acquire stays at the landed conduct tail, after the FINAL close only).
    void waitBetweenSharedGCWindows(); // §3.7 closed-loop between-window wait (F13/ANNEX CGD1.3); flag-dead until CG-3.

    // SPEC-congc §9.1(2) marker pause for a foreign GCL holder (CG-3b; ANNEX
    // CGP1 BINDING GOVERNS), amended by SPEC-ungil-history [r34] F-A item (1):
    // the wait is TIMED, sampling JSThreadsSafepoint::watchdogAssertStopProgress
    // (requestStart, &vm()) per 1ms quantum — a wedged marker batch fail-stops
    // ON THE CONDUCTOR ITSELF at the 30s bound instead of hanging unwatched
    // (the conductor's next sample otherwise sits in the VMManager predicate
    // loop, never reached). Called ONLY from the JSThreadsStopScope ctor (the
    // heap-OWNED ctor/dtor are the ONLY pause/resume sites, F18/F47;
    // §13.3(b): no foreign row), after the GCL is HELD (after a SUCCESSFUL
    // tryLock only, never per failed iteration), when
    // m_currentPhase != NotRunning (the read is GCL-ordered, F22). Acquires
    // only m_markingMutex — no api-rank or heap rank >= 7 lock, and helpers
    // hold no lock a §A.3 window needs, so the pause terminates (CG-I16).
    void pauseConcurrentMarkingForForeignStop(MonotonicTime requestStart);
    // Resume half: clear flag + notifyAll under m_markingMutex; called by
    // ~JSThreadsStopScope strictly BEFORE its GCL release (§9.1(2) dtor order
    // NORMATIVE — no WND-open with paused markers).
    void resumeConcurrentMarkingAfterForeignStop();

    void runSafepointHooksAndReclaim(); // §9 hooks + §11 reclaim sequence; both protocols' sole call sites.
    void reclaimSharedGCMemoryAtCycleEnd(); // T4(d): world-stopped per-cycle shrink (+ slack-gated full sweep) — the shared server's only steady-state decommit point.
    bool activityCallbackDispatchAllowed(); // T4(c): mutator-side activity-callback dispatch — true when !ISS, else main client's thread only (single-writer timer state).
    void pollIssRevertIfNeeded(); // §10D ISS reversion, main client's poll.
    // Manifest 5a park hooks (heap-owned impls; installed via
    // VMManager::setGCParkCallbacks, manifest items 3-5). Run inside
    // VMManager::notifyVMStop with no VMM lock held (L6).
    static void gcWillParkInStopTheWorld(VM&);
    static void gcDidResumeFromStopTheWorld(VM&);
    
    bool suspendCompilerThreads();
    void willStartCollection();
    void prepareForMarking();
    
    void gatherStackRoots(ConservativeRoots&);
    void gatherVMRoots(ConservativeRoots&);
    void beginMarking();
#if ENABLE(WEBASSEMBLY)
    void prepareWasmCalleeCleanup();
    void finalizeWasmCalleeCleanup();
#endif
    void visitCompilerWorklistWeakReferences();
    void removeDeadCompilerWorklistEntries();
    void updateObjectCounts();
    void endMarking();

    void cancelDeferredWorkIfNeeded();
    void reapWeakHandles();
    void pruneStaleEntriesFromWeakGCHashTables();
    void sweepArrayBuffers();
    void snapshotUnswept();
    void deleteSourceProviderCaches();
    void notifyIncrementalSweeper();
    void harvestWeakReferences();

    template<typename CellType, typename CellSet>
    void finalizeMarkedUnconditionalFinalizers(CellSet&, CollectionScope);

    void finalizeUnconditionalFinalizers();

    void deleteUnmarkedCompiledCode();
    JS_EXPORT_PRIVATE void addToRememberedSet(const JSCell*);
    void updateAllocationLimits();
    void didFinishCollection();
    void resumeCompilerThreads();
    void gatherExtraHeapData(HeapProfiler&);
    void removeDeadHeapSnapshotNodes(HeapProfiler&);
    void finalize();
    void sweepInFinalize();
    void drainDeferredLambdaFinalizers(); // §F.3 carve-out (b) (MC-GC S5 / B7).
    
    void sweepAllLogicallyEmptyWeakBlocks();
    bool sweepNextLogicallyEmptyWeakBlock();

    bool shouldDoFullCollection();

    inline void incrementDeferralDepth();
    inline void decrementDeferralDepth();
    inline void decrementDeferralDepthAndGCIfNeeded();
    JS_EXPORT_PRIVATE void decrementDeferralDepthAndGCIfNeededSlow();

    // SharedGC (§5.4/I17): the calling thread's deferral-depth slot. Once
    // ISS, this is the current client's per-client counter (touched only by
    // its access-holding thread, debug-asserted); !ISS, or on threads with no
    // client TLS stamp (GC helpers world-stopped), the server counter.
    // Defined at the bottom of this header (needs GCClient::Heap).
    inline unsigned& deferralDepthSlot();
    inline unsigned currentDeferralDepth() const;

    // SharedGC (review round 4): the calling thread's deferred-GC-hint slot.
    // m_didDeferGCWork used to be a single plain server bool; with N mutator
    // clients, collectIfNecessaryOrDefer sets it on any client thread (under
    // that client's per-client deferral, I17) while another client's
    // ~DeferGC concurrently reads and clears it — a plain-bool data race
    // (TSAN gate) AND a lost-hint hazard (client B's empty recheck swallows
    // client A's just-set hint, breaking the I17 contract that closing a
    // DeferGC scope performs the deferred work). Same routing as
    // deferralDepthSlot(): once ISS, the current client's per-client flag
    // (touched only by its access-holding thread); !ISS, or on threads with
    // no client TLS stamp, the server field. The flag always pairs with the
    // deferral depth it annotates because both route identically.
    // Defined at the bottom of this header (needs GCClient::Heap).
    inline bool& didDeferGCWorkSlot();

    // SharedGC (review round 2): the calling thread's mutator-state slot.
    // m_mutatorState used to be a single plain server field; with N mutator
    // clients, two concurrent allocation slow paths would trip
    // AllocatingScope's RELEASE_ASSERTs (T1 sets Allocating; T2's ctor
    // asserts Running) and the SweepingScope/CollectingScope save/restore
    // pattern would lose updates, mis-driving collectIfNecessaryOrDefer and
    // the SINFAC ticket-serving gate. Same routing as deferralDepthSlot():
    // once ISS, the current client's per-client slot (touched only by its
    // access-holding thread, or the conductor's own thread world-stopped);
    // !ISS, or on threads with no client TLS stamp (collector thread, GC
    // helpers), the server field. The Allocating/Sweeping/Collecting/Running
    // scopes cache the returned reference, so an ISS flip (§10B.4) or
    // reversion (§10D) mid-scope cannot split a scope's ctor/dtor across two
    // slots — the flip protocols already exclude in-scope mutators, but the
    // cached reference makes the scopes correct without relying on that.
    // Defined at the bottom of this header (needs GCClient::Heap).
    inline MutatorState& mutatorStateSlot();

    size_t visitCount();
    size_t bytesVisited();
    
    void forEachCodeBlockImpl(const ScopedLambda<void(CodeBlock*)>&);
    void forEachCodeBlockIgnoringJITPlansImpl(const AbstractLocker& codeBlockSetLocker, const ScopedLambda<void(CodeBlock*)>&);
    
    void setMutatorShouldBeFenced(bool value);
    
    void addCoreConstraints();

    enum class MemoryThresholdCallType {
        Cached,
        Direct
    };

    bool overCriticalMemoryThreshold(MemoryThresholdCallType memoryThresholdCallType = MemoryThresholdCallType::Cached);
    
    template<typename Visitor>
    void iterateExecutingAndCompilingCodeBlocks(Visitor&, NOESCAPE const Function<void(CodeBlock*)>&);
    
    template<typename Func, typename Visitor>
    void iterateExecutingAndCompilingCodeBlocksWithoutHoldingLocks(Visitor&, const Func&);
    
    void assertMarkStacksEmpty();

    void setBonusVisitorTask(RefPtr<SharedTask<void(SlotVisitor&)>>);

    void dumpHeapStatisticsAtVMDestruction();

    static bool useGenerationalGC();
    bool shouldSweepSynchronously();

    void verifyGC();
    void verifierMark();

    Lock m_lock;
    const HeapType m_heapType;
    // SharedGC (review round 2): once ISS this is only the slot for threads
    // with no client TLS stamp — client threads route through
    // mutatorStateSlot() to their GCClient::Heap::m_mutatorState. Never read
    // or written cross-thread by mutators (each thread touches its own slot).
    MutatorState m_mutatorState { MutatorState::Running };
    const size_t m_ramSize;
    const size_t m_minBytesPerCycle;
    size_t m_bytesAllocatedBeforeLastEdenCollect { 0 };
    size_t m_sizeAfterLastCollect { 0 };
    size_t m_sizeAfterLastFullCollect { 0 };
    size_t m_sizeBeforeLastFullCollect { 0 };
    size_t m_sizeAfterLastEdenCollect { 0 };
    size_t m_sizeBeforeLastEdenCollect { 0 };

    // SharedGC (§5.4): allocation accounting is updated by N mutator slow
    // paths once the server is shared; std::atomic with relaxed ordering on
    // both sides (F3) — exact sums re-establish at safepoints (I7).
    std::atomic<size_t> m_oversizedBytesAllocatedThisCycle { 0 };
    std::atomic<size_t> m_lastOversidedAllocationThisCycle { 0 };

    std::atomic<size_t> m_nonOversizedBytesAllocatedThisCycle { 0 };
    // T1-sibint (SCALEBENCH §31): count of DISTINCT clients that have called
    // didAllocate() since the last updateAllocationLimits reset. Feeds the
    // §25/T5-rss N-mutator floating-garbage Full trigger in place of
    // clientSet().size() so that a serial section with W-1 siblings parked at
    // a JS barrier (registered but not allocating) does not force the
    // alternating Eden/Full train. Relaxed fetch_add from the isSharedServer()
    // leg of didAllocate (W>=2-only); store(0) world-stopped at the byte-
    // counter reset site below. Flag-off / W=1: never written, never read.
    std::atomic<unsigned> m_distinctAllocatingClientsThisCycle { 0 };
    // F4-burst (SCALEBENCH §32(b)): per-client byte threshold a client must
    // cross in a single GC cycle before it is counted toward
    // m_distinctAllocatingClientsThisCycle. Recomputed world-stopped at the
    // reset site (= m_maxEdenSize / registeredClients / 4); read by mutators
    // in didAllocate's isSharedServer() leg with no concurrent writer (same
    // regime as m_maxEdenSize / m_maxHeapSize). 0 until the first collection
    // -> first cycle degrades to the §31 first-alloc semantics, which is the
    // conservative direction. Flag-off / W=1: never written, never read.
    size_t m_distinctAllocatorByteThreshold { 0 };
    std::atomic<size_t> m_bytesAbandonedSinceLastFullCollect { 0 };
    size_t m_maxEdenSize;
    size_t m_maxEdenSizeWhenCritical;
    size_t m_maxHeapSize;
    size_t m_totalBytesVisitedAfterLastFullCollect { 0 };
    size_t m_totalBytesVisited { 0 };
    size_t m_totalBytesVisitedThisCycle { 0 };
    double m_incrementBalance { 0 };
    
    bool m_shouldDoOpportunisticFullCollection { false };
    bool m_isInOpportunisticTask { false };
    bool m_shouldDoFullCollection { false };
    Markable<CollectionScope> m_collectionScope;
    Markable<CollectionScope> m_lastCollectionScope;
    Lock m_raceMarkStackLock;
    // Serializes m_mutatorMarkStack between the barrier slow paths of the
    // attached mutators (Heap::addToRememberedSet) and the server-side
    // drains. Ordered after m_markingMutex and before a client's CMS lock
    // where a CMS drains into the server stack; nothing else nests under it.
    // Taken only under Options::useSharedGCHeap(): with one mutator the
    // stack keeps its unlocked single-producer append.
    Lock m_serverMutatorMarkStackLock;

    MarkedSpace m_objectSpace;
    GCIncomingRefCountedSet<ArrayBuffer> m_arrayBuffers;
    size_t m_extraMemorySize { 0 };
    size_t m_deprecatedExtraMemorySize { 0 };

    // Leaf; guards m_protectedValues against concurrent protect()/unprotect()
    // and statistics walks from N gilOff mutators. Taken gilOff-only so the
    // flag-off and GIL-on paths stay byte-identical; the collector's root walk
    // runs inside the stop (every entered mutator is parked) and takes no lock.
    Lock m_protectedValuesLock;
    ProtectCountSet m_protectedValues;
    UncheckedKeyHashSet<MarkedVectorBase*> m_markListSet;
    // DW-2: shared-GC-heap-mode replacement for m_markListSet (see
    // markListSetShard()). Empty and never locked when !useSharedGCHeap.
    MarkListSetShard m_markListSetShards[numMarkListSetShards];

    std::unique_ptr<MachineThreads> m_machineThreads;
    
    std::unique_ptr<SlotVisitor> m_collectorSlotVisitor;
    std::unique_ptr<SlotVisitor> m_mutatorSlotVisitor;
    std::unique_ptr<MarkStackArray> m_mutatorMarkStack;
    std::unique_ptr<MarkStackArray> m_raceMarkStack;
    std::unique_ptr<MarkingConstraintSet> m_constraintSet;
    std::unique_ptr<VerifierSlotVisitor> m_verifierSlotVisitor;

    // We pool the slot visitors used by parallel marking threads. It's useful to be able to
    // enumerate over them, and it's useful to have them cache some small amount of memory from
    // one GC to the next. GC marking threads claim these at the start of marking, and return
    // them at the end.
    Vector<std::unique_ptr<SlotVisitor>> m_parallelSlotVisitors;
    Vector<SlotVisitor*> m_availableParallelSlotVisitors WTF_GUARDED_BY_LOCK(m_parallelSlotVisitorLock);
    // T4-heap-layout-restore: the T1-gc-siblings-mark sibling SlotVisitor
    // pool (m_siblingSlotVisitors / m_availableSiblingSlotVisitors /
    // m_siblingSlotVisitorPoolMayGrow) was MOVED to the campaign-2 trailer
    // block at the end of `class Heap` so m_handleSet and every member after
    // it keeps its pre-campaign-2 (729430dbc80c) offset. See the trailer
    // comment for the full protocol.
    HandleSet m_handleSet;
    std::unique_ptr<CodeBlockSet> m_codeBlocks;
    // §5.8 record-named CodeBlock pins — see pinRetiredCallLinkRecordCodeBlock.
    Lock m_retiredCallLinkRecordCodeBlocksLock;
    HashCountedSet<void*> m_retiredCallLinkRecordCodeBlocks WTF_GUARDED_BY_LOCK(m_retiredCallLinkRecordCodeBlocksLock);
    std::unique_ptr<JITStubRoutineSet> m_jitStubRoutines;
    CFinalizerOwner m_cFinalizerOwner;
    LambdaFinalizerOwner m_lambdaFinalizerOwner { *this };
    // UNGIL-HANDOUT §F.3 carve-out (b) (MC-GC S5 / CVE-AUDIT B7): addFinalizer
    // lambdas that LambdaFinalizerOwner::finalize deferred from inside the
    // conducted §10 stop window. Conductor-thread-private: appended only by
    // the conductor inside its own WSAC window (no other thread reaches
    // weak-bearing sweeps while WSAC is set), drained only by the same thread
    // at the conductSharedCollection tail (post-resume, with access) — no lock.
    Vector<LambdaFinalizer> m_deferredLambdaFinalizers;

    Lock m_parallelSlotVisitorLock;
    bool m_isSafeToCollect { false };
    bool m_isShuttingDown { false };
    bool m_mutatorShouldBeFenced { false };
    bool m_isMarkingForGCVerifier { false };
    bool m_keepVerifierSlotVisitor { false };
    Lock m_wasmCalleesPendingDestructionLock;

    unsigned m_barrierThreshold { blackThreshold };

    // SPEC-congc §5.3(1) (CG-2): the barrier-fence epoch (FEP). Bumped
    // (release) by setMutatorShouldBeFenced per mutation of the server
    // master pair above; once ISS those mutations are conductor-only and
    // in-window (raise in beginMarking, lower in endMarking) apart from the
    // noteSharedServerSticky ISS-flip raise, which the next window's
    // republish covers. The WND-close republish loop
    // (closeSharedGCStopWindow) copies master->every client and stamps each
    // client's m_fenceEpochSeen with this value; the §5.3(4) WND-close
    // debug assert checks the stamps. Flag-off: bumped but never consulted
    // (the copies are unrouted, unread state — ANNEX CGD4.4).
    Atomic<uint64_t> m_barrierFenceEpoch { 0 };

#if PLATFORM(MAC)
    Seconds m_lastFullGCLength { 2_ms };
    Seconds m_lastEdenGCLength { 2_ms };
#else
    Seconds m_lastFullGCLength { 10_ms };
    Seconds m_lastEdenGCLength { 10_ms };
#endif

    Vector<WeakBlock*> m_logicallyEmptyWeakBlocks;
    size_t m_indexOfNextLogicallyEmptyWeakBlockToSweep { WTF::notFound };

    // Leaf; taken gilOff-only around the vector below (N gilOff mutators
    // append; the GC-end clear runs world-stopped). NOT WTF_GUARDED_BY_LOCK:
    // the single-mutator GIL-on/flag-off append deliberately stays unlocked.
    Lock m_possiblyAccessedStringsFromConcurrentThreadsLock;
    Vector<String> m_possiblyAccessedStringsFromConcurrentThreads;

    RefPtr<GCActivityCallback> m_fullActivityCallback;
    RefPtr<GCActivityCallback> m_edenActivityCallback;
    const Ref<IncrementalSweeper> m_sweeper;
    const Ref<StopIfNecessaryTimer> m_stopIfNecessaryTimer;

    Vector<HeapObserver*> m_observers;
    
    Vector<HeapFinalizerCallback> m_heapFinalizerCallbacks;
    
    std::unique_ptr<HeapVerifier> m_verifier;

#if USE(FOUNDATION)
    Vector<RetainPtr<CFTypeRef>> m_delayedReleaseObjects;
    unsigned m_delayedReleaseRecursionCount { 0 };
#endif
#ifdef JSC_GLIB_API_ENABLED
    Vector<std::unique_ptr<JSCGLibWrapperObject>> m_delayedReleaseObjects;
    unsigned m_delayedReleaseRecursionCount { 0 };
#endif
    unsigned m_deferralDepth { 0 };

    // SharedGC (CVE-AUDIT A3 / map-MC-GC S12b / K4.VIII.9): leaf; guards
    // m_weakGCHashTables — N gilOff mutators register/unregister concurrently
    // (every JSGlobalObject ctor registers several WeakGCMaps via
    // WeakGCMapInlines.h:40 / WeakGCSetInlines.h:38; reproduced as ASAN SEGV
    // in HashTable removeIterator, JSTests/threads/cve/
    // mc-gc-weakgcmap-registry-vs-prune.crash.txt) vs the conductor's
    // world-stopped prune walk. Taken gilOff-only so flag-off
    // register/unregister/prune stay byte-identical. NOT WTF_GUARDED_BY_LOCK:
    // the flag-off path deliberately accesses the set unlocked and the
    // single-mutator invariant makes that sound.
    Lock m_weakGCHashTablesLock;
    UncheckedKeyHashSet<WeakGCHashTable*> m_weakGCHashTables;
    
#if ENABLE(WEBASSEMBLY)
    UncheckedKeyHashSet<Ref<Wasm::Callee>> m_wasmCalleesPendingDestruction WTF_GUARDED_BY_LOCK(m_wasmCalleesPendingDestructionLock);
    // We snapshot m_wasmCalleesPendingDestruction at the start of GC rather than consulting it
    // directly during scanning because new callees can be registered while we scan. Without the
    // snapshot, a callee could be added after we already passed its frame, never get recorded
    // as discovered, and be incorrectly destroyed.
    UncheckedKeyHashSet<const Wasm::Callee*> m_wasmCalleesPendingDestructionSnapshot;
    UncheckedKeyHashSet<const Wasm::Callee*> m_wasmCalleesDiscoveredDuringGC;
    TinyBloomFilter<uintptr_t> m_boxedWasmCalleeFilter;
#endif

    std::unique_ptr<MarkStackArray> m_sharedCollectorMarkStack;
    std::unique_ptr<MarkStackArray> m_sharedMutatorMarkStack;
    unsigned m_numberOfActiveParallelMarkers { 0 };
    unsigned m_numberOfWaitingParallelMarkers { 0 };
    // SPEC-congc §9.1(2) marker-pause pair (CG-3a; ANNEX CGP1 BINDING).
    // BOTH guarded by m_markingMutex; participant set is EXACTLY the helpers
    // inside drainFromShared(HelperDrain) (F14 — the counters' only
    // maintainers). Pause protocol (set by CG-3b's
    // pauseConcurrentMarkingForForeignStop): set ShouldPause + notifyAll
    // m_markingConditionVariable, then wait (same mutex/condvar) until
    // m_numberOfParallelMarkersInDrainFromShared == m_pausedParallelMarkers
    // (every visitor inside drainFromShared is parked; see the trailer).
    // Checkpoints: the helper-wait isReady lambda and the per-batch drain
    // safepoint (SlotVisitor::helperDrainPauseCheckpointIfRequested —
    // granularity one drained batch, the CG-I12 bound). ShouldPause gates
    // counter (re-)entry including a fresh helper's entry increment
    // (transient under the mutex).
    // didReachTermination additionally requires m_pausedParallelMarkers == 0
    // (CG-I22), so waitForTermination stays parked across a foreign §A.3
    // stop. Resume: clear flag + notifyAll. WRITER CONTRACT: the bool is
    // hint-read lock-free at the per-batch checkpoint via
    // WTF::atomicLoad(relaxed) — every write MUST be a WTF::atomicStore
    // under m_markingMutex (the mutex carries the protocol; the atomic makes
    // the hint read well-defined). Flag-off the pair is never set/nonzero
    // and every checkpoint is dead — AND never even loaded:
    // m_isDrainingFromSharedHelper is option-byte-gated at the
    // drainFromShared drain entry (FIX-V5B-F1 pattern), so flag-off helpers
    // never touch this Heap line (it sits adjacent to the
    // m_markingMutex-protected marker counters) from the marking hot loop.
    bool m_parallelMarkersShouldPause { false };
    unsigned m_pausedParallelMarkers { 0 };
    // T4-heap-layout-restore: the T1-gc-siblings-mark assist gate
    // (m_siblingMarkingAssistEnabled / m_numberOfSiblingMarkingAssists) was
    // MOVED to the campaign-2 trailer block at the end of `class Heap` so
    // m_opaqueRoots / m_helperClient / m_worldState keep their
    // pre-campaign-2 (729430dbc80c) offsets. See the trailer comment for
    // the full protocol.

    ConcurrentPtrHashSet m_opaqueRoots;
    static constexpr size_t s_blockFragmentLength = 32;

    ParallelHelperClient m_helperClient;
    RefPtr<SharedTask<void(SlotVisitor&)>> m_bonusVisitorTask;

#if ENABLE(RESOURCE_USAGE)
    std::atomic<size_t> m_blockBytesAllocated { 0 }; // SharedGC (§5.4): relaxed both sides (F3).
    size_t m_externalMemorySize { 0 };
#endif
    
    std::unique_ptr<MutatorScheduler> m_scheduler;
    
    static constexpr unsigned mutatorHasConnBit = 1u << 0u; // Must also be protected by threadLock.
    static constexpr unsigned stoppedBit = 1u << 1u; // Only set when !hasAccessBit
    static constexpr unsigned hasAccessBit = 1u << 2u;
    static constexpr unsigned needFinalizeBit = 1u << 3u;
    static constexpr unsigned mutatorWaitingBit = 1u << 4u; // Allows the mutator to use this as a condition variable.
    Atomic<unsigned> m_worldState;
    bool m_worldIsStopped { false };

    // --- Shared heap server state (SPEC-heap.md §5.1; THREADS T1) ---
    HeapClientSet m_clientSet;
    // The MSPL facade (m_mutatorSlowPathLock) and m_markedSpaceRegistryLock
    // live in the trailer block at the end of the class; see the comment
    // there.
    Lock m_gcConductorLock; // GCL, rank 2 (§10/§10C).
    Lock m_gcBarrierLock; // GBL, rank 4 (§10.4/F7).
    Condition m_gcBarrierCondition; // GBC; signaled by clients releasing access while GSP (F8).
    bool m_gcConductorActive { false }; // GCA; guarded by *m_threadLock (rank 5; §10.2).
    // SPEC-congc §3.5 (CG-1): GCA's owner. Stamped (under *m_threadLock) where
    // GCA is set; restamp only in NotRunning (debug-asserted); cleared
    // ownership-checked at the deferred wind-down clears (F20, ANNEX CGD3.1 —
    // a descheduled predecessor must not clear a successor's tenure).
    // Consumers: §3.4 guards (FOREIGN discrimination), the §9.2(4) EXIT1
    // release-assert (CG-3), CG-I21.
    Thread* m_gcConductorThread { nullptr };
    // SPEC-congc §9.1(2a) F45 (ANNEX CGD7.2): foreign GCL waiter count.
    // Relaxed; the JSThreadsStopScope ctor incs BEFORE its first lock
    // attempt and decs once the lock is HELD (dtor never touches it; a
    // !isSharedServer() early return never increments). Consulted ONLY by the
    // WND-open RE-ENTRY deferral — flag-off it is maintained but never read
    // on the GC path (one-window conducts never re-enter; CG-I0).
    Atomic<unsigned> m_foreignGCLWaiters { 0 };
    // SPEC-congc §3.7 ATOM-TABLE PIN scaffolding (F46, ANNEX CGD7.3): the
    // per-window install's saved previous table, valid only while a windowed
    // (sharedGCWindowedConductActive(), Heap.cpp) window is open; conductor-only.
    WTF::AtomStringTable* m_sharedGCWindowSavedAtomStringTable { nullptr };
    // SPEC-congc §3.5 conductor-identity scaffolding (CG-3a): the conducting
    // client, stamped/cleared by conductSharedCollection (conductor-private,
    // valid only during a conduct tenure). Consumed by finishChangingPhase's
    // §7.1 WND-reopen arm (openSharedGCStopWindow(Reentry) needs the
    // conductor's GCClient for its §3.7 access-released debug assert).
    // Distinct from m_gcConductorThread (the *m_threadLock-guarded GCA
    // owner): this one is only ever read on the conductor's own thread
    // inside the tenure's closed loop, so it needs no lock.
    GCClient::Heap* m_sharedGCConductorClient { nullptr };
    Condition m_gcElectionCondition; // GEC; waited on under *m_threadLock (§10.2/§10B.4).
    // W16-C1 residual (b): shared-mode PreventCollectionScope gate; guarded
    // by *m_threadLock. Once ISS, preventCollection() raises this
    // (idempotently, from inside its waitForCollector func — flip-safe
    // because the func re-checks ISS every iteration); while nonzero the
    // §10.2 election winner arm and tryConductSharedCollectionForPoll()
    // refuse to claim a NEW conduct tenure, so no shared collection can
    // START. Tickets still GRANT per §10B.1 (requestCollectionShared / the
    // CIND timer's inline grant) and sit granted-unserved; a live cycle's
    // window re-entry (SPEC-congc §3 WND-open, part of an already-started
    // cycle) is deliberately NOT gated — preventCollection instead waits for
    // the whole cycle (!GCA && phase == NotRunning) before returning,
    // cooperating with its stop windows via the waitForCollector ISS
    // branch's SINFAC poll. Cleared by allowCollection() (GEC notifyAll).
    // The CIND timer thread is separately excluded by
    // m_collectContinuouslyLock, held across the whole prevent scope. This
    // closes the N-mutator gap where the legacy "now a collection can only
    // start if this thread starts it" postcondition silently relied on the
    // single-mutator protocol. Composition with SPEC-congc §9: the gate
    // touches neither GCL nor the §A.3 stop protocol — it only narrows the
    // two landed conduct-tenure decision points (§9.3's election/poll stay
    // tryLock-only); a prevented requester behaves exactly like the landed
    // GCL-busy follower (timed <=1ms GEC waits, §10.2 GCL-busy rule).
    unsigned m_sharedGCPreventCount { 0 };
    // The thread holding the prevent scope while m_sharedGCPreventCount is
    // nonzero; guarded by *m_threadLock. Both conduct-tenure decision points
    // exempt it, preserving the legacy "a collection can only start if this
    // thread starts it" semantics: the holder's own collectNow(Sync) (heap
    // snapshots run one inside PreventCollectionScope) must still conduct.
    Thread* m_sharedGCPreventHolder { nullptr };
    // Companion raise-tracking flag for allowCollection(); guarded by
    // m_collectContinuouslyLock (prevent/allow holders are serialized on
    // it). Lets allowCollection() clear the gate without consulting
    // isSharedServer() — a §10D reversion between prevent and allow must
    // not leak a raised gate into a later re-flip era — while keeping the
    // legacy flag-off path free of any *m_threadLock acquisition.
    bool m_sharedGCPreventGateRaised { false };
    Atomic<bool> m_gcStopPending { false }; // GSP; sole writer = conductor, seq_cst (F8).
    Atomic<bool> m_isSharedServer { false }; // Sticky ISS (§5.1/I13/§10D).
    Atomic<bool> m_worldIsStoppedForAllClients { false }; // WSAC; conductor-written under GBL (F7).
    Atomic<bool> m_issRevertPending { false }; // §10D; written under *m_threadLock (HeapClientSet::remove / pollIssRevertIfNeeded); relaxed reads are a poll hint only.
    GCClient::Heap* m_mainClient { nullptr }; // First registered client (the owning VM's); written under HeapClientSet::m_lock (§3.3/§10A).
    // T4: bytes directly appended by the Wlr window-witness pass this cycle
    // (closure excluded — a deliberate undercount). Reset in
    // willStartCollection, accumulated by the Wlr constraint executor,
    // consumed by updateAllocationLimits — all world-stopped; always 0 when
    // !isSharedServer() or with a single attached client.
    size_t m_sharedGCWindowRetainedBytesThisCycle { 0 };
    GCSafepointEpoch m_safepointEpoch; // §11.
    Lock m_stopTheWorldSafepointHookLock;
    Vector<void (*)(JSC::Heap&)> m_stopTheWorldSafepointHooks WTF_GUARDED_BY_LOCK(m_stopTheWorldSafepointHookLock);
    std::unique_ptr<RetiredStructureChainInvalidationWatchpoints> m_retiredStructureChainInvalidationWatchpoints; // Null flag-off.
    // --- End shared heap server state ---

    Lock m_markingMutex;
    Condition m_markingConditionVariable;

    MonotonicTime m_beforeGC;
    MonotonicTime m_afterGC;
    MonotonicTime m_stopTime;
    
    Deque<GCRequest> m_requests;
    GCRequest m_currentRequest;
    Ticket m_lastServedTicket { 0 };
    Ticket m_lastGrantedTicket { 0 };

    CollectorPhase m_lastPhase { CollectorPhase::NotRunning };
    CollectorPhase m_currentPhase { CollectorPhase::NotRunning };
    CollectorPhase m_nextPhase { CollectorPhase::NotRunning };
    bool m_collectorThreadIsRunning { false };
    bool m_threadShouldStop { false };
    bool m_mutatorDidRun { true };
    // SharedGC (review round 4): once ISS this is only the slot for threads
    // with no client TLS stamp — client threads route through
    // didDeferGCWorkSlot() to their GCClient::Heap::m_didDeferGCWork (same
    // dispatch as m_deferralDepth, I17). Never read or written cross-thread
    // by mutators once shared.
    bool m_didDeferGCWork { false };
    bool m_shouldStopCollectingContinuously { false };
    bool m_isCompilerThreadsSuspended { false };

    uint64_t m_mutatorExecutionVersion { 0 };
    uint64_t m_phaseVersion { 0 };
    uint64_t m_gcVersion { 0 };
    Box<Lock> m_threadLock;
    const Ref<AutomaticThreadCondition> m_threadCondition; // The mutator must not wait on this. It would cause a deadlock.
    const RefPtr<AutomaticThread> m_thread;

    RefPtr<Thread> m_collectContinuouslyThread { nullptr };
    
    MonotonicTime m_lastGCStartTime;
    MonotonicTime m_lastGCEndTime;
    MonotonicTime m_currentGCStartTime;
    MonotonicTime m_lastFullGCEndTime;
    Seconds m_totalGCTime;
    
    uintptr_t m_barriersExecuted { 0 };
    
    CurrentThreadState* m_currentThreadState { nullptr };
    Thread* m_currentThread { nullptr }; // It's OK if this becomes a dangling pointer.

#if USE(MEMORY_FOOTPRINT_API)
    unsigned m_percentAvailableMemoryCachedCallCount { 0 };
    bool m_overCriticalMemoryThreshold { false };
#endif

    bool m_parallelMarkersShouldExit { false };
    Lock m_collectContinuouslyLock;
    Condition m_collectContinuouslyCondition;

public:
    // HeapCellTypes
    HeapCellType auxiliaryHeapCellType;
    HeapCellType immutableButterflyHeapCellType;
    HeapCellType cellHeapCellType;
    HeapCellType destructibleCellHeapCellType;
    IsoHeapCellType apiGlobalObjectHeapCellType;
    IsoHeapCellType callbackConstructorHeapCellType;
    IsoHeapCellType callbackGlobalObjectHeapCellType;
    IsoHeapCellType callbackObjectHeapCellType;
    IsoHeapCellType customGetterFunctionHeapCellType;
    IsoHeapCellType customSetterFunctionHeapCellType;
    IsoHeapCellType dateInstanceHeapCellType;
    IsoHeapCellType errorInstanceHeapCellType;
    IsoHeapCellType finalizationRegistryCellType;
    IsoHeapCellType globalLexicalEnvironmentHeapCellType;
    IsoHeapCellType globalObjectHeapCellType;
    IsoHeapCellType injectedScriptHostSpaceHeapCellType;
    IsoHeapCellType javaScriptCallFrameHeapCellType;
    IsoHeapCellType jsModuleRecordHeapCellType;
    IsoHeapCellType syntheticModuleRecordHeapCellType;
    IsoHeapCellType moduleNamespaceObjectHeapCellType;
    IsoHeapCellType nativeStdFunctionHeapCellType;
    IsoInlinedHeapCellType<JSString> stringHeapCellType;
    IsoInlinedHeapCellType<JSRopeString> ropeStringHeapCellType;
    IsoHeapCellType weakMapHeapCellType;
    IsoHeapCellType weakSetHeapCellType;
    JSDestructibleObjectHeapCellType destructibleObjectHeapCellType;
#if JSC_OBJC_API_ENABLED
    IsoHeapCellType apiWrapperObjectHeapCellType;
    IsoHeapCellType objCCallbackFunctionHeapCellType;
#endif
#ifdef JSC_GLIB_API_ENABLED
    IsoHeapCellType apiWrapperObjectHeapCellType;
    IsoHeapCellType callbackAPIWrapperGlobalObjectHeapCellType;
    IsoHeapCellType jscCallbackFunctionHeapCellType;
#endif
    IsoHeapCellType intlCollatorHeapCellType;
    IsoHeapCellType intlDateTimeFormatHeapCellType;
    IsoHeapCellType intlDisplayNamesHeapCellType;
    IsoHeapCellType intlDurationFormatHeapCellType;
    IsoHeapCellType intlListFormatHeapCellType;
    IsoHeapCellType intlLocaleHeapCellType;
    IsoHeapCellType intlNumberFormatHeapCellType;
    IsoHeapCellType intlPluralRulesHeapCellType;
    IsoHeapCellType intlRelativeTimeFormatHeapCellType;
    IsoHeapCellType intlSegmentIteratorHeapCellType;
    IsoHeapCellType intlSegmenterHeapCellType;
    IsoHeapCellType intlSegmentsHeapCellType;
    IsoHeapCellType temporalTimeZoneHeapCellType;
#if ENABLE(WEBASSEMBLY)
    IsoHeapCellType webAssemblyExceptionHeapCellType;
    IsoHeapCellType webAssemblyFunctionHeapCellType;
    IsoHeapCellType webAssemblyGlobalHeapCellType;
    // We can use IsoHeapCellType for instances because it's allocated out of a PreciseSubspace reserved for just instances.
    IsoHeapCellType webAssemblyInstanceHeapCellType;
    IsoHeapCellType webAssemblyMemoryHeapCellType;
    IsoHeapCellType webAssemblyModuleHeapCellType;
    IsoHeapCellType webAssemblyModuleRecordHeapCellType;
    IsoHeapCellType webAssemblyTableHeapCellType;
    IsoHeapCellType webAssemblyTagHeapCellType;
#endif

    // AlignedMemoryAllocators
    std::unique_ptr<FastMallocAlignedMemoryAllocator> fastMallocAllocator;
    std::unique_ptr<GigacageAlignedMemoryAllocator> primitiveGigacageAllocator;

    // Subspaces
    CompleteSubspace primitiveGigacageAuxiliarySpace; // Typed arrays, strings, bitvectors, etc go here.
    CompleteSubspace auxiliarySpace; // Butterflies, arrays of JSValues, etc go here.
    CompleteSubspace immutableButterflyAuxiliarySpace; // JSCellButterfly goes here.

    // We make cross-cutting assumptions about typed arrays being in the primitive Gigacage and butterflies
    // being in the JSValue gigacage. For some types, it's super obvious where they should go, and so we
    // can hardcode that fact. But sometimes it's not clear, so we abstract it by having a Gigacage::Kind
    // constant somewhere.
    // FIXME: Maybe it would be better if everyone abstracted this?
    // https://bugs.webkit.org/show_bug.cgi?id=175248
    ALWAYS_INLINE CompleteSubspace& gigacageAuxiliarySpace(Gigacage::Kind kind)
    {
        switch (kind) {
        case Gigacage::Primitive:
            return primitiveGigacageAuxiliarySpace;
        case Gigacage::NumberOfKinds:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return primitiveGigacageAuxiliarySpace;
    }
    
    // Whenever possible, use subspaceFor<CellType>(vm) to get one of these subspaces.
    CompleteSubspace cellSpace;
    CompleteSubspace destructibleObjectSpace;

    // Exactly 5 CompleteSubspaces exist (the three auxiliary spaces above +
    // cellSpace + destructibleObjectSpace); each reserves one contiguous
    // MarkedSpace::numSizeClasses-slot range in every client's
    // GCThreadLocalCache table, so every non-iso tlcIndex is below
    // numCompleteSubspaces * numSizeClasses and the static IsoSubspace slots
    // start exactly there. GCThreadLocalCache's ctor allocates the table at
    // that fixed capacity under Options::useSharedGCHeap(), so m_table never
    // reallocs and the per-thread table snapshots stay valid for the
    // client's lifetime.
    // MarkedSpace::reserveThreadLocalCacheIndices RELEASE_ASSERTs on a sixth
    // reservation; adding a CompleteSubspace requires bumping this constant.
    static constexpr unsigned numCompleteSubspaces = 5;

#define DECLARE_ISO_SUBSPACE(name, heapCellType, type) \
    IsoSubspace name;

    FOR_EACH_JSC_ISO_SUBSPACE(DECLARE_ISO_SUBSPACE)
#undef DECLARE_ISO_SUBSPACE

#define DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER(name, heapCellType, type) \
    template<SubspaceAccess mode> \
    IsoSubspace* name() \
    { \
        if (m_##name || mode == SubspaceAccess::Concurrently) \
            return m_##name.get(); \
        return name##Slow(); \
    } \
    JS_EXPORT_PRIVATE IsoSubspace* name##Slow(); \
    std::unique_ptr<IsoSubspace> m_##name;

    FOR_EACH_JSC_DYNAMIC_ISO_SUBSPACE(DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER)
#undef DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER
    
#define DYNAMIC_SPACE_AND_SET_DEFINE_MEMBER(name, type) \
    template<SubspaceAccess mode> \
    IsoSubspace* name() \
    { \
        if (auto* spaceAndSet = m_##name.get()) \
            return &spaceAndSet->space; \
        if (mode == SubspaceAccess::Concurrently) \
            return nullptr; \
        return name##Slow(); \
    } \
    IsoSubspace* name##Slow(); \
    std::unique_ptr<type> m_##name;
    
    struct SpaceAndSet {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(SpaceAndSet);

        IsoSubspace space;
        IsoCellSet set;
        
        template<typename... Arguments>
        SpaceAndSet(Arguments&&... arguments)
            : space(std::forward<Arguments>(arguments)...)
            , set(space)
        {
        }
        
        static IsoCellSet& setFor(Subspace& space)
        {
            return *std::bit_cast<IsoCellSet*>(
                std::bit_cast<char*>(&space) -
                OBJECT_OFFSETOF(SpaceAndSet, space) +
                OBJECT_OFFSETOF(SpaceAndSet, set));
        }
    };

    using CodeBlockSpaceAndSet = SpaceAndSet;
    CodeBlockSpaceAndSet codeBlockSpaceAndSet;

    template<typename Func>
    void forEachCodeBlockSpace(const Func& func)
    {
        func(codeBlockSpaceAndSet);
    }

    struct ScriptExecutableSpaceAndSets {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ScriptExecutableSpaceAndSets);

        IsoSubspace space;
        IsoCellSet clearableCodeSet;
        IsoCellSet outputConstraintsSet;
        IsoCellSet finalizerSet;

        template<typename... Arguments>
        ScriptExecutableSpaceAndSets(Arguments&&... arguments)
            : space(std::forward<Arguments>(arguments)...)
            , clearableCodeSet(space)
            , outputConstraintsSet(space)
            , finalizerSet(space)
        {
        }

        static ScriptExecutableSpaceAndSets& setAndSpaceFor(Subspace& space)
        {
            return *std::bit_cast<ScriptExecutableSpaceAndSets*>(
                std::bit_cast<char*>(&space) -
                OBJECT_OFFSETOF(ScriptExecutableSpaceAndSets, space));
        }

        static IsoCellSet& clearableCodeSetFor(Subspace& space) { return setAndSpaceFor(space).clearableCodeSet; }
        static IsoCellSet& outputConstraintsSetFor(Subspace& space) { return setAndSpaceFor(space).outputConstraintsSet; }
        static IsoCellSet& finalizerSetFor(Subspace& space) { return setAndSpaceFor(space).finalizerSet; }
    };

    DYNAMIC_SPACE_AND_SET_DEFINE_MEMBER(evalExecutableSpace, ScriptExecutableSpaceAndSets)
    DYNAMIC_SPACE_AND_SET_DEFINE_MEMBER(moduleProgramExecutableSpace, ScriptExecutableSpaceAndSets)
    ScriptExecutableSpaceAndSets functionExecutableSpaceAndSet;
    ScriptExecutableSpaceAndSets programExecutableSpaceAndSet;

    template<typename Func>
    void forEachScriptExecutableSpace(const Func& func)
    {
        if (m_evalExecutableSpace)
            func(*m_evalExecutableSpace);
        func(functionExecutableSpaceAndSet);
        if (m_moduleProgramExecutableSpace)
            func(*m_moduleProgramExecutableSpace);
        func(programExecutableSpaceAndSet);
    }

    using UnlinkedFunctionExecutableSpaceAndSet = SpaceAndSet;
    UnlinkedFunctionExecutableSpaceAndSet unlinkedFunctionExecutableSpaceAndSet;

#undef DYNAMIC_SPACE_AND_SET_DEFINE_MEMBER

#define DEFINE_NON_ISO_SUBSPACE_MEMBER(name, heapCellType, type, SubspaceType) \
    template<SubspaceAccess mode> \
    SubspaceType* name() \
    { \
        if (m_##name || mode == SubspaceAccess::Concurrently) \
            return m_##name.get(); \
        return name##Slow(); \
    } \
    JS_EXPORT_PRIVATE SubspaceType* name##Slow(); \
    std::unique_ptr<SubspaceType> m_##name;

    FOR_EACH_JSC_WEBASSEMBLY_DYNAMIC_NON_ISO_SUBSPACE(DEFINE_NON_ISO_SUBSPACE_MEMBER)
#undef DEFINE_NON_ISO_SUBSPACE_MEMBER

    CString m_signpostMessage;

private:
    // ---------------------------------------------------------------------
    // T4-heap-layout-restore: CAMPAIGN-2 TRAILER BLOCK.
    //
    // Every member that campaign-2 (729430dbc80c..d8ed7b6f5254) inserted
    // into the MIDDLE of `class Heap` is collected here, in one contiguous
    // block AFTER the last pre-campaign-2 member (m_signpostMessage), so
    // every hot pre-existing field — m_objectSpace, m_handleSet,
    // m_opaqueRoots, m_helperClient, m_worldState, m_barrierThreshold,
    // m_mutatorState, m_threadLock, the allocation counters, every
    // IsoSubspace — sits at its EXACT pre-campaign-2 byte offset again.
    // SCALEBENCH §25 measured the mid-class growth at +2.7% GIL-on W=1
    // wall (5-rep interleaved A/B, reproducible) with NO new code path
    // running in that configuration: the regression was purely structural
    // (cache-line shift + inlining-decision drift on the allocation slow
    // path).
    //
    // Pure declaration reordering — zero behavior change in any
    // configuration. None of these members appear in the Heap::Heap()
    // member-init list (all in-class default-initialized), so no -Wreorder.
    // Flag-off: every member here is dead state (never read, never
    // written); the only flag-off observable is `sizeof(Heap)`, which grows
    // by the trailer — harmless (Heap is a singleton-per-VM, heap-
    // allocated). Future campaign additions: append HERE, never mid-class.
    // ---------------------------------------------------------------------

    // T7-mspl-per-directory: MSPL striped RW facade (rank 7; §5.2/§5.6) —
    // see MutatorSlowPathLockFacade above for the full protocol. Moved from
    // the §5.1 server-state cluster; `mutatorSlowPathLock()` returns this.
    MutatorSlowPathLockFacade m_mutatorSlowPathLock;
    // T7-mspl-per-directory, rank 7r (leaf); see markedSpaceRegistryLock().
    Lock m_markedSpaceRegistryLock;

    // T1-gc-siblings-mark: per-sibling parallel SlotVisitor pool. The W-1
    // gilOff Mode-machine SIBLINGS (parked access-released at
    // VMManager::notifyVMStop while the elected representative runs
    // conductSharedCollection) join marking exactly as the heapHelperPool
    // helpers do — drainFromShared(HelperDrain) on a visitor from THIS pool.
    // Like the pool helpers' visitors, these exist before marking starts:
    // the conductor grows the pool in runBeginPhase (gilOff gate, before the
    // didStartMarking walk) to the number of admissible siblings, and a
    // sibling that finds the pool empty declines the assist, so the visitor
    // set never changes under an in-flight MarkingConstraintSolver. Kept
    // SEPARATE from m_availableParallelSlotVisitors so the pool helpers'
    // apriori-count RELEASE_ASSERT stays sound. Growth and
    // forEachSlotVisitor's iteration of this vector both serialize on
    // m_parallelSlotVisitorLock; the mayGrow sticky bit gates that lock
    // acquisition so flag-off forEachSlotVisitor is byte-identical (the bit
    // is set by the conductor before the pool is first populated, so a false
    // read never skips a non-empty pool).
    Vector<std::unique_ptr<SlotVisitor>> m_siblingSlotVisitors WTF_GUARDED_BY_LOCK(m_parallelSlotVisitorLock);
    Vector<SlotVisitor*> m_availableSiblingSlotVisitors WTF_GUARDED_BY_LOCK(m_parallelSlotVisitorLock);
    Atomic<bool> m_siblingSlotVisitorPoolMayGrow { false };

    // T1-gc-siblings-mark: BOTH guarded by m_markingMutex. assistEnabled is
    // raised by runBeginPhase (after forEachSlotVisitor(didStartMarking) +
    // m_parallelMarkersShouldExit=false; gilOff-only) and lowered by
    // runEndPhase in the SAME critical section that sets shouldExit — so a
    // sibling that observes enabled==true under the mutex is guaranteed
    // shouldExit==false at that instant and the cycle's didStartMarking has
    // happened-before via the mutex. The count tracks siblings BETWEEN that
    // observe and their post-drain decrement; runEndPhase waits it to zero
    // after m_helperClient.finish() (the shouldExit notifyAll wakes them),
    // restoring the active==paused==inDrainFromShared==0 invariant before the
    // ASSERT block and endMarking()'s reset() walk. Flag-off: never set/nonzero.
    bool m_siblingMarkingAssistEnabled { false };
    unsigned m_numberOfSiblingMarkingAssists { 0 };

    // Guarded by m_markingMutex: visitors between entry to and return from
    // SlotVisitor::drainFromShared (each is active, waiting for work, or
    // paused). Balanced on every return, unlike m_numberOfWaitingParallelMarkers,
    // which is only the stealSomeCellsFrom partitioning hint and keeps the
    // pre-threads behavior of staying incremented across returns. The
    // marker-pause predicate is m_numberOfParallelMarkersInDrainFromShared ==
    // m_pausedParallelMarkers, and runEndPhase asserts it is zero.
    unsigned m_numberOfParallelMarkersInDrainFromShared { 0 };

    // Config-independent ordering guards: each trailer member is declared
    // after the hot pre-existing field it once displaced. They check
    // declaration order only, not byte offsets, so they catch a trailer
    // member moved back mid-class but not a new mid-class insertion.
    // Wrapped in a (never-called) static member function body so the asserts
    // see Heap as a COMPLETE type — OBJECT_OFFSETOF (== __builtin_offsetof)
    // rejects an incomplete type at class-member-declaration scope, and at
    // namespace scope Clang enforces access control on the private members
    // it names. A member-function body is a complete-class context AND has
    // private access, satisfying both constraints.
    static constexpr void checkCampaign2TrailerLayout()
    {
        static_assert(OBJECT_OFFSETOF(Heap, m_handleSet) < OBJECT_OFFSETOF(Heap, m_siblingSlotVisitors),
            "T4-heap-layout-restore: sibling-visitor pool must be in the trailer (after m_handleSet)");
        static_assert(OBJECT_OFFSETOF(Heap, m_opaqueRoots) < OBJECT_OFFSETOF(Heap, m_siblingMarkingAssistEnabled),
            "T4-heap-layout-restore: sibling-assist gate must be in the trailer (after m_opaqueRoots)");
        static_assert(OBJECT_OFFSETOF(Heap, m_worldState) < OBJECT_OFFSETOF(Heap, m_mutatorSlowPathLock),
            "T4-heap-layout-restore: MSPL facade must be in the trailer (after m_worldState)");
        static_assert(OBJECT_OFFSETOF(Heap, m_signpostMessage) < OBJECT_OFFSETOF(Heap, m_mutatorSlowPathLock),
            "T4-heap-layout-restore: campaign-2 trailer must follow the last pre-campaign-2 member");
    }
};

// SharedGC (§5.2/§5.6): RAII that takes the server's mutator-slow-path lock
// (MSPL, rank 7) iff the heap is a shared server. Option off / single client:
// a no-op, so the gated fast/slow paths execute today's code (I10). Derives
// from AbstractLocker so it can be passed as the lock token required by
// BlockDirectory::tryAllocateBlock (§5.2(3)).
//
// L2: construct only AFTER collectIfNecessaryOrDefer() has returned — never
// hold MSPL across a collection request or a stop.
// L4: rank 7 sections must never acquire cell/Structure locks (10a/10b).
//
// T7-mspl-per-directory: two acquisition shapes (see
// Heap::MutatorSlowPathLockFacade for the full protocol):
//   MutatorSlowPathLocker(heap)            -> EXCLUSIVE (server-wide) MSPL.
//   MutatorSlowPathLocker(heap, directory) -> STRIPE: shared MSPL +
//                                             directory.refillLock() (rank 7a).
// Flag-off / !isSharedServer(): both forms are a no-op (the directory form is
// only ever constructed inside an isSharedServer() branch).
class MutatorSlowPathLocker : public AbstractLocker {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit MutatorSlowPathLocker(JSC::Heap& heap) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        if (heap.isSharedServer()) [[unlikely]] {
            m_facade = &heap.mutatorSlowPathLock();
            m_facade->lock();
        }
    }

    MutatorSlowPathLocker(JSC::Heap& heap, BlockDirectory& directory) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        // Stripe form. Callers gate on isSharedServer() themselves (the only
        // construction site is LocalAllocator::allocateSlowCase's striped
        // branch); assert rather than re-check so flag-off never reaches the
        // facade.
        ASSERT(heap.isSharedServer());
        m_facade = &heap.mutatorSlowPathLock();
        m_facade->enterStripe();
        m_directoryRefillLock = &directory.refillLock();
        m_directoryRefillLock->lock();
#if ASSERT_ENABLED
        m_directory = &directory;
        directory.didEnterRefillStripe();
#endif
    }

    ~MutatorSlowPathLocker() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        if (m_directoryRefillLock) [[unlikely]] {
#if ASSERT_ENABLED
            m_directory->willExitRefillStripe();
#endif
            m_directoryRefillLock->unlock();
            m_facade->exitStripe();
            return;
        }
        if (m_facade) [[unlikely]]
            m_facade->unlock();
    }

private:
    JSC::Heap::MutatorSlowPathLockFacade* m_facade { nullptr };
    Lock* m_directoryRefillLock { nullptr };
#if ASSERT_ENABLED
    BlockDirectory* m_directory { nullptr };
#endif
};

namespace GCClient {

// THREADS: BasicRawSentinelNode base links this client into its server's
// HeapClientSet (SPEC-heap.md §5.1). The ctor registers with
// server().clientSet(); the dtor unregisters (T2).
class Heap : public BasicRawSentinelNode<Heap> {
    WTF_MAKE_NONCOPYABLE(Heap);
public:
    Heap(JSC::Heap&);
    ~Heap();

    inline VM& vm() const; // RELEASE_ASSERTs !m_isStandalone (SPEC-heap.md §12.1/T9).
    JSC::Heap& server() { return m_server; }

    GCThreadLocalCache& threadLocalCache() LIFETIME_BOUND { return m_threadLocalCache; }
    const GCThreadLocalCache& threadLocalCache() const LIFETIME_BOUND { return m_threadLocalCache; }

    // Implements the FIXME below (GlobalGC): relinquish memory from this
    // client's allocators back to the server (SPEC-heap.md §5.3/I9).
    void lastChanceToFinalize();

    // I4 lifecycle (SPEC-heap.md §9): call on the using thread.
    JS_EXPORT_PRIVATE void attachCurrentThread(); // I4(a)-(c) + acquires access.
    JS_EXPORT_PRIVATE void detachCurrentThread(); // Releases access; localEpoch = MAX; clears the TLS slot.
    JS_EXPORT_PRIVATE void markStandalone(); // Non-VM client (§12.1); arms the vm() assert.

    // §10A/F8 heap-access protocol; REQUIRED around indefinitely-blocking
    // native calls. acquireHeapAccess() blocks while a shared-mode GC stop is
    // pending (mandatory revert, F8 steps 1-3); step 0 makes it idempotent
    // for the owning thread (JSLock recursion / attach / hook re-entry).
    JS_EXPORT_PRIVATE void acquireHeapAccess();
    JS_EXPORT_PRIVATE void releaseHeapAccess();
    bool hasHeapAccess() const { return m_accessState.load(std::memory_order_relaxed) == hasAccessState; }

    // T5-rootscan-skip-coop-parked-suspend (SCALEBENCH §31, offcpu16 row #4):
    // a cooperatively-parked sibling — access-released and about to descend
    // into pure libc futex/condvar machinery for its NVS ticket / GSP block /
    // bounded poll — captures a CurrentThreadState (callee-saves spilled via
    // ALLOCATE_AND_GET_REGISTER_STATE + stackTop in the parking caller's
    // frame, which stays live across the park) and seq_cst-publishes a
    // pointer to it here, paired with the publishing Thread*. The conductor's
    // root scan (Heap::gatherStackRoots -> MachineThreads::
    // tryCopyOtherThreadStacks) reads this seq_cst: a non-null snapshot lets
    // it copy the saved registers + [stackTop, stackOrigin] directly and
    // SKIP the SIGUSR2 suspend()/getRegisters()/resume() round-trip for that
    // thread. Soundness: the snapshot is taken with access RELEASED and the
    // only post-snapshot frames are park machinery with no JSCell*; the
    // captured span is at-least-as-conservative as a suspend-captured one
    // (spurious retention only). Dekker leg: publish/clear are seq_cst and
    // pair against the conductor's GSP/stop-word seq_cst stores + the
    // seq_cst snapshot load at scan time. Cleared seq_cst BEFORE the parking
    // thread re-acquires heap access (gcClientDidResumeFromThreadGranularStop
    // and the explicit per-park-episode clears at the call sites). gilOff
    // clients are per-thread (U-T6), so the publishing Thread* is this
    // client's stable owning thread; the server-side lookup is built per scan
    // by walking clientSet() (HeapClientSet, frozen by I13 inside the stop
    // window) and keying on m_parkedRootSnapshotThread — i.e. the Thread* ->
    // GCClient::Heap* map is the existing registry plus this field.
    // Flag-off / W=1: every publish/clear call site is inside a vm.gilOff()
    // [[unlikely]] branch that additionally requires NOT being the §A.3
    // conductor / Mode-stop representative — unreachable with a single
    // thread; the fields stay zero-initialised and the scan-side path is
    // gated on isSharedServer() (false flag-off).
    // Out-of-line in Heap.cpp (CurrentThreadState is forward-declared here;
    // the body validates snapshot->stackTop / stackOrigin against
    // thread.stack() — see the CVE-AUDIT A3 residual / SCAN-RESULTS.md
    // residual #2 comment there).
    JS_EXPORT_PRIVATE void publishParkedRootSnapshot(WTF::Thread&, CurrentThreadState*);
    void clearParkedRootSnapshot() { m_parkedRootSnapshot.store(nullptr, std::memory_order_seq_cst); }
    CurrentThreadState* parkedRootSnapshot() const { return m_parkedRootSnapshot.load(std::memory_order_seq_cst); }
    WTF::Thread* parkedRootSnapshotThread() const { return WTF::atomicLoad(const_cast<WTF::Thread**>(&m_parkedRootSnapshotThread), std::memory_order_relaxed); }

    // §10A.1 current-client TLS slot (set by attachCurrentThread() and the
    // server's ISS access forwarding; cleared by detachCurrentThread();
    // releaseHeapAccess() does NOT clear it). Null on non-client threads.
    //
    // B1-alloc-client-tls-fastpath: ALWAYS_INLINE over a plain C++
    // `thread_local Heap*` (storage defined in GCThreadLocalCache.cpp; same
    // model as g_jscCurrentVMLite, runtime/VMLite.h) — every allocateCell under
    // (useSharedGCHeap && gilOff) routes through
    // allocationClientForCurrentThread -> this accessor, and the prior
    // out-of-line LazyNeverDestroyed<ThreadSpecific> resolver paid
    // std::call_once + pthread_getspecific per call (bigintcost: ~45% of the
    // GIL-off/GIL-on heap-BigInt regression). The slot is only READ under
    // gates that are false flag-off (gilOffWithProcessGate() /
    // isSharedServer() / sharedGCBarrierStateIsPerClient()), so flag-off
    // codegen on every hot path is byte-identical; the zero-init of the
    // unread slot matches "ThreadSpecific never constructed". Writers stay
    // out-of-line (setCurrentThreadClient below) so every existing stamp
    // site at attach/detach/JSLock A36C re-stamp is semantically unchanged.
    ALWAYS_INLINE static Heap* currentThreadClient() { return s_currentThreadClient; }

    // Direct IE-TLS snapshot of the stamped client's GCThreadLocalCache
    // {m_table, m_tableBound} pair so CompleteSubspace::allocate's
    // useSharedGCHeap leg resolves `table[tlcIndexBase + sizeClassIndex]`
    // with two %fs:@TPOFF loads + one indexed load, skipping hop-1
    // (allocationClientForCurrentThread: gilOff gate + s_currentThreadClient
    // + server-identity compare) and hop-2 (client.threadLocalCache() member
    // chase) entirely on the C++ non-iso hot path. The TLC table is allocated
    // once at its lifetime capacity, write-once-per-slot and
    // owner-thread-mutated (I2), so a same-thread snapshot never observes a
    // torn or freed table; setCurrentThreadClient() is the only writer of the
    // pair and runs on this thread strictly before the next allocate() that
    // could read it. Zero-init: an unstamped thread (compilation thread,
    // pre-attach bootstrap) reads bound==0 and falls through to the
    // allocationClientForCurrentThread slow path, preserving the FIX-3
    // carve-out semantics. Only READ inside `if (Options::useSharedGCHeap())`
    // — flag-off codegen on every hot path is byte-identical.
    ALWAYS_INLINE static Allocator* currentThreadTLCTable() { return s_currentThreadTLCTable; }
    ALWAYS_INLINE static unsigned currentThreadTLCBound() { return s_currentThreadTLCBound; }

    // GlobalGC FIXME resolved (T4): lastChanceToFinalize() relinquishes
    // memory from this client's allocators — owned non-iso TLC allocators AND
    // the IsoSubspace LocalAllocators (registered lookup-only in the TLC's
    // per-directory map) — back to the server, via
    // GCThreadLocalCache::stopAllocatingForGood() (I9). Option off: iso
    // registration is skipped and server teardown still goes through
    // BlockDirectory::stopAllocatingForGood(), exactly as before (I10).

private:
    friend class JSC::Heap;
    friend class JSC::HeapClientSet;
    friend class JSC::GCSafepointEpoch; // §11: reads/stamps m_localEpoch (T7).
    friend class JSC::JSLock; // UNGIL §A.3.6/A36C (U-T1): the carrier tuple swap re-stamps the §10A.1 client slot at install/LIFO-restore.

    static constexpr uint8_t noAccessState = 0; // §10A m_accessState values.
    static constexpr uint8_t hasAccessState = 1;

    // §10A.1 backing storage for currentThreadClient(); plain C++
    // thread_local (constant zero-init, no guard), defined in
    // GCThreadLocalCache.cpp. JS_EXPORT_PRIVATE so the inlined reader links
    // from every TU including out-of-tree consumers of the header.
    //
    // M2-alloc-tax-residual (a): on ELF, default-visibility thread_local
    // forces general-dynamic TLS (interposable → __tls_get_addr), which the
    // optimizer treats as a non-inlinable call — phaseAreg nm showed v35
    // emitting allocationClientForCurrentThread<VM> out-of-line at 0xf79a0
    // despite ALWAYS_INLINE (v34 inlined it; +1.067G cyc on the W=1 GIL-off
    // pc-loop). Pinning initial-exec restores the single `movq %fs:@TPOFF`
    // load and lets the template re-inline into CompleteSubspaceInlines.h.
    // The export is KEPT (out-of-tree TUs that inline CompleteSubspace::
    // allocate still link); IE-TLS is sound because libJavaScriptCore is
    // never dlopen()'d in any supported configuration (the same precondition
    // g_jscCurrentVMLite already relies on, runtime/VM.cpp:217). The
    // attribute on this declaration carries to the definition (same entity;
    // GCThreadLocalCache.cpp includes this header).
#if OS(LINUX)
    JS_EXPORT_PRIVATE __attribute__((tls_model("initial-exec"))) static thread_local Heap* s_currentThreadClient;
    // H-TLS-TABLE: same IE-TLS rationale as s_currentThreadClient above
    // (single `movq %fs:@TPOFF`; libJavaScriptCore is never dlopen()'d). The
    // attribute on these declarations carries to the definitions in
    // GCThreadLocalCache.cpp (same entity).
    JS_EXPORT_PRIVATE __attribute__((tls_model("initial-exec"))) static thread_local Allocator* s_currentThreadTLCTable;
    JS_EXPORT_PRIVATE __attribute__((tls_model("initial-exec"))) static thread_local unsigned s_currentThreadTLCBound;
#else
    JS_EXPORT_PRIVATE static thread_local Heap* s_currentThreadClient;
    JS_EXPORT_PRIVATE static thread_local Allocator* s_currentThreadTLCTable;
    JS_EXPORT_PRIVATE static thread_local unsigned s_currentThreadTLCBound;
#endif
    static void setCurrentThreadClient(Heap*); // §10A.1; defined in GCThreadLocalCache.cpp.
    static void setCurrentThreadTLCSnapshot(Allocator*, unsigned); // H-TLS-TABLE restamp; defined in GCThreadLocalCache.cpp.

    // I4(b) enforcement (§10.6/I12, T6): every thread that acquires heap
    // access must first be registered with the server's MachineThreads so
    // the conductor's conservative scan sees its stack and registers.
    // Idempotent; cached by Thread::uid() (0 is never a valid uid) so the
    // common JSLock re-entry/hand-back path skips the thread-group lock.
    void ensureCurrentThreadIsRegisteredForConservativeScan(WTF::Thread&);

    // §5.3 (T4): enters every GCClient::IsoSubspace LocalAllocator into the
    // TLC's per-directory map (lookup-only; covers iso for §10A.1 and §5.3
    // teardown). Ctor-time for the eager members; the dynamic Slow paths
    // register at creation. Gated on Options::useSharedGCHeap().
    void registerIsoSubspaceLocalAllocators();

    JSC::Heap& m_server;

#define DECLARE_ISO_SUBSPACE(name, heapCellType, type) \
    IsoSubspace name;

    FOR_EACH_JSC_ISO_SUBSPACE(DECLARE_ISO_SUBSPACE)
#undef DECLARE_ISO_SUBSPACE

#define DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_IMPL(name, heapCellType, type) \
    template<SubspaceAccess mode> \
    IsoSubspace* name() \
    { \
        if (m_##name || mode == SubspaceAccess::Concurrently) \
            return m_##name.get(); \
        return name##Slow(); \
    } \
    JS_EXPORT_PRIVATE IsoSubspace* name##Slow(); \
    std::unique_ptr<IsoSubspace> m_##name;

#define DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER(name) \
    DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_IMPL(name, unused, unused2)

    FOR_EACH_JSC_DYNAMIC_ISO_SUBSPACE(DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_IMPL)

    DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER(evalExecutableSpace)
    DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER(moduleProgramExecutableSpace)

#undef DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_IMPL
#undef DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER

    IsoSubspace codeBlockSpace;
    IsoSubspace functionExecutableSpace;
    IsoSubspace programExecutableSpace;
    IsoSubspace unlinkedFunctionExecutableSpace;

    // --- Shared heap client state (SPEC-heap.md; THREADS T2) ---
    GCThreadLocalCache m_threadLocalCache; // §5.3; initialized after the iso subspaces (declaration order).
    Atomic<uint8_t> m_accessState { noAccessState }; // §10A; seq_cst RMWs (F6/F8).
    Atomic<WTF::Thread*> m_accessOwner { nullptr }; // §10A; step-0 idempotency, I2 hand-off re-stamping, debug cross-checks.
    WTF::Thread* m_parkedRootSnapshotThread { nullptr }; // T5-rootscan-skip: written BEFORE the seq_cst m_parkedRootSnapshot publish; read by the conductor only when that load returns non-null (the seq_cst pair orders it). Never cleared independently (stale value is harmless — gated by the snapshot pointer). Relaxed-atomic accessors: the seq_cst m_parkedRootSnapshot store/load is the real HB edge (TSAN-TRIAGE §20.3.4 parked-root-snapshot).
    Atomic<CurrentThreadState*> m_parkedRootSnapshot { nullptr }; // T5-rootscan-skip: see the public accessor block above. seq_cst publish/clear by the owning thread; seq_cst load by the conductor at root-scan time.
    bool m_releasedByGCPark { false }; // §10A; written only inside VMManager::notifyVMStop (manifest 5a hooks, JSC::Heap::gcWillParkInStopTheWorld / gcDidResumeFromStopTheWorld; T5).
    bool m_allocatedThisServerCycle { false }; // T1-sibint (SCALEBENCH §31) + F4-burst (§32(b)): set once this client's per-cycle byte accumulator crosses server.m_distinctAllocatorByteThreshold in the isSharedServer() didAllocate leg (own access-holding thread only); cleared world-stopped by server Heap::updateAllocationLimits via clientSet().forEach. Gates the once-per-cycle m_distinctAllocatingClientsThisCycle bump.
    size_t m_bytesAllocatedThisServerCycle { 0 }; // F4-burst (§32(b)): per-client byte accumulator toward m_distinctAllocatorByteThreshold. Same touch rules as the bool above (own access-holding thread writes; world-stopped reset). Once the bool latches true the accumulator is dead for the rest of the cycle (steady-state hot path = the single bool read, identical to §31).
    Atomic<uint64_t> m_localEpoch { std::numeric_limits<uint64_t>::max() }; // §11; written ONLY by the conductor's stamping loop (world stopped) and detachCurrentThread (MAX) — attach deliberately does NOT stamp it (review round 2: a pre-access stamp can land stale across stop windows and regress bumpAndReclaim's min scan).
    bool m_isStandalone { false }; // §12.1; arms the vm() RELEASE_ASSERT (T9).
    bool m_relinquishedAllocators { false }; // Set by lastChanceToFinalize(); a spawned thread relinquishes while its lite is still Live (counted by the §A.3 predicate), so the dtor must not re-acquire access on the Teardown lite to do it again.
    Atomic<uint32_t> m_lastConservativeScanRegisteredUid { 0 }; // I4(b) cache (T6): uid of the last thread this client registered with machineThreads(); relaxed — a stale read merely re-runs the idempotent addCurrentThread().
    unsigned m_deferralDepth { 0 }; // §5.4/I17 (T3): per-client DeferGC depth once ISS; touched only by this client's access-holding thread.
    bool m_didDeferGCWork { false }; // Review round 4: per-client deferred-GC hint once ISS (companion to m_deferralDepth, same touch rules), via JSC::Heap::didDeferGCWorkSlot(). Migrated server<->client at the §10B.4 flip / §10D reversion alongside the depth.
    MutatorState m_mutatorState { MutatorState::Running }; // Review round 2: per-client mutator state once ISS; touched only by this client's access-holding thread (or the conductor's own thread while it conducts), via JSC::Heap::mutatorStateSlot().

    // --- CG-2 per-client GC state (SPEC-congc §4.1-4.2, §5.2-5.3; ANNEXES
    // CGD2.2, CGD4.4/4.5). ALL of it is C1R-routed (F33: C1R := ISS &&
    // useConcurrentSharedGCMarking) — with the C1 stage flag off this block
    // is unrouted, unread state and every landed code path is byte-for-byte
    // (CGD4.4; the CG-T1 flag-off identity arm). ---
    //
    // §5.2 CMS (per-client mutator mark stack): the write-barrier slow path
    // (JSC::Heap::addToRememberedSet) appends HERE under m_mutatorMarkStackLock
    // when C1R and the calling thread's §10A.1 client is this one. Created
    // lazily under the lock by the first routed append; null whenever !C1R.
    // Drained: every WND-open transfers it into the server's
    // m_sharedMutatorMarkStack under m_markingMutex (§5.2(i) — the
    // correctness drain; the §5.2(ii) threshold donation from the SINFAC
    // hot-poll tail is CG-3, gated on sharedGCMutatorMarkStackDonationThreshold).
    // CMS is contention/scaling + window-drain accounting, NOT a soundness
    // fix: the server stack's remaining C1R producers (the F31/CGD4.5
    // conductor-context appends) stay serialized by
    // m_serverMutatorMarkStackLock.
    std::unique_ptr<MarkStackArray> m_mutatorMarkStack;
    // LK.9c (SPEC-congc CG-I10/F21; lint-lockorder-u20.sh R3/R4): TERMINAL
    // leaf — nothing may be acquired under it; legal under the rank 7-9b
    // allocation-side locks barrier slow paths hold; ordered AFTER
    // m_markingMutex (LK.9d) and the server's m_serverMutatorMarkStackLock
    // at the WND-open drain/donation and exit-flush sites only.
    Lock m_mutatorMarkStackLock;
    // §5.3(2) fence/threshold copies + FEP stamp: republished
    // master->client by the conductor inside the mutating window (WSAC,
    // pre-close, in closeSharedGCStopWindow). Clients NEVER write these; the
    // only reader is the C1R arm of JSC::Heap::addToRememberedSet (the
    // inline barrier reads the server master, see Heap::barrierThreshold).
    // CG-I3: a RAISE completes for all clients before its window closes; a
    // LOWER only in the final window; over-fenced is always sound.
    bool m_mutatorShouldBeFenced { false };
    unsigned m_barrierThreshold { blackThreshold };
    uint64_t m_fenceEpochSeen { 0 };
    // §4.1 didRun: plain byte, owner-thread relaxed writes only (the AHA
    // success tail + the SINFAC hot-poll exit; heap I17). Scheduling-only.
    // The conductor ORs it into the legacy m_mutatorDidRun consumer and
    // clears it at each WND-open (the window barrier orders it, CG-I9).
    bool m_didRunSinceLastWindow { false };
    // --- End shared heap client state ---

    friend class JSC::VM;
};

} // namespace GCClient

// SharedGC (§5.4/I17, T3): per-client DeferGC depth dispatch. Defined here
// (below GCClient::Heap) because it needs the client's members; JSC::Heap is
// a friend of GCClient::Heap. ISS is only ever set with useSharedGCHeap on
// (noteSharedServerSticky), so the option byte is tested first: every
// DeferGC scope reaches these dispatchers, and flag-off they then pay one
// predicted-false Config-page test and never load the Heap line holding
// m_isSharedServer (the same shape as sharedGCBarrierStateIsPerClient()).
ALWAYS_INLINE unsigned& Heap::deferralDepthSlot()
{
    if (Options::useSharedGCHeap() && isSharedServer()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == this) {
            // I17: a client's depth is touched only by its access-holding
            // thread (or the conductor while the world is stopped).
            ASSERT(client->hasHeapAccess() || worldIsStoppedForAllClients());
            return client->m_deferralDepth;
        }
        // No client TLS stamp (e.g. a GC helper thread world-stopped, or the
        // last pre-attach increments): fall back to the server counter; reads
        // on such threads consult the same counter, so the pairing holds.
    }
    return m_deferralDepth;
}

// SharedGC (review round 4): per-client deferred-GC-hint dispatch; see the
// declaration comment at didDeferGCWorkSlot() above. Mirrors
// deferralDepthSlot() exactly so the hint always pairs with the depth.
ALWAYS_INLINE bool& Heap::didDeferGCWorkSlot()
{
    if (Options::useSharedGCHeap() && isSharedServer()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == this) {
            ASSERT(client->hasHeapAccess() || worldIsStoppedForAllClients());
            return client->m_didDeferGCWork;
        }
        // No client TLS stamp: the server flag — reads on such threads
        // consult the same flag, so set/recheck pairing holds.
    }
    return m_didDeferGCWork;
}

ALWAYS_INLINE unsigned Heap::currentDeferralDepth() const
{
    if (Options::useSharedGCHeap() && isSharedServer()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == this)
            return client->m_deferralDepth;
    }
    return m_deferralDepth;
}

// SharedGC (review round 2): per-thread mutator-state dispatch; see the
// declaration comment at mutatorStateSlot() above.
ALWAYS_INLINE MutatorState& Heap::mutatorStateSlot()
{
    if (Options::useSharedGCHeap() && isSharedServer()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == this)
            return client->m_mutatorState;
        // No client TLS stamp (e.g. a GC helper thread world-stopped, or a
        // pre-attach thread): the server field — reads on such threads
        // consult the same field, so save/restore pairing holds.
    }
    return m_mutatorState;
}

// SPEC-ungil §B / I4 (see the declaration comment above): per-thread
// allocation-client dispatch, the only resolver every allocation-routing
// site calls. Mirrors deferralDepthSlot(). It runs on every C++ allocation,
// so the gate is VM::gilOffWithProcessGate(): one predicted-false test of a
// byte on the frozen Config page whose clear state implies m_gilOff == 0
// (the byte is latched before the m_gilOff designation in the VM ctor), so
// the predicate equals m_gilOff in every reachable state and flag-off and
// GIL-on processes never load a VM or Heap line here.
template<typename VMType>
ALWAYS_INLINE GCClient::Heap& Heap::allocationClientForCurrentThread(VMType& vm, GCClient::Heap& vmOriginalClient)
{
    static_assert(std::is_same_v<VMType, VM>, "templated solely to defer instantiation until VM is complete");
    ASSERT(&vmOriginalClient.server() == &vm.heap);
    if (vm.gilOffWithProcessGate()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == &vm.heap) {
            // I2: an allocating thread must hold ITS client's access; a
            // stamped thread must not silently fall back to the main client
            // (that re-creates the shared-FreeList race).
            ASSERT(client->hasHeapAccess() || vm.heap.worldIsStoppedForAllClients());
            return *client;
        }
        // Tripwire, access-OWNER identity form (apply-scope item (2);
        // GCClient::Heap friendship): under sticky GIL-off every legitimate
        // mutator is stamped before its first allocation (JSLock forwarding,
        // §B.1 attach, A36C carrier swap), so the fallback is only legal when
        // THIS thread owns the main client's access or the world is stopped.
        // hasHeapAccess() alone would pass while ANOTHER thread holds the
        // main client's access — the exact racy fallback. If a legitimate
        // unstamped MUTATOR trips this, stamp that caller; never weaken the
        // mutator leg.
        //
        // Compilation-thread carve-out: JIT worklist threads are unstamped
        // BY DESIGN and CANNOT be stamped (no GCClient exists for them — the
        // "stamp that caller" remedy is unsatisfiable). They reach this
        // resolver only through the mode-blind VM.h static iso accessors
        // (e.g. vm.ropeStringSpace() via allocatorForConcurrently<JSRopeString>
        // in compileMakeRope, DFGSpeculativeJIT.cpp): a pure pointer read
        // used to bake an Allocator into the artifact, never a FreeList pop
        // on this thread — and allocatorForConcurrently bakes an empty
        // Allocator GIL-off, so no per-client LocalAllocator reaches an
        // artifact. Mutator-side strength is unchanged: a compilation thread
        // can never legally construct a cell — both JSCell constructors
        // ASSERT(!isCompilationThread()) (runtime/JSCellInlines.h), and on
        // the barrier path Heap::addToRememberedSet asserts
        // ASSERT(!Options::useConcurrentJIT() || !isCompilationThread())
        // (Heap.cpp). Under --useConcurrentJIT=0 the latter leg is vacuous,
        // but a synchronously-compiling thread is the stamped mutator itself
        // and returns at the stamped early branch above, never reaching this
        // tripwire — so the exempted population is exactly the
        // pointer-read-only callers.
        ASSERT(Thread::currentSingleton().isCompilationThread()
            || vm.heap.worldIsStoppedForAllClients()
            || vmOriginalClient.m_accessOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton());
    }
    return vmOriginalClient;
}

ALWAYS_INLINE MutatorState Heap::mutatorState() const
{
    if (Options::useSharedGCHeap() && isSharedServer()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == this)
            return client->m_mutatorState;
    }
    return m_mutatorState;
}

ALWAYS_INLINE bool Heap::isDeferred() const
{
    // I17: CollectIfNecessaryOrDefer defers iff the CALLING client's depth is
    // nonzero once ISS; one client's DeferGC never masks another's triggers.
    return !!currentDeferralDepth();
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
