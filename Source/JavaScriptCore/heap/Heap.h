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

// vmstate N7 shim (SPEC-heap.md §9): signals that this tree's Heap provides
// increment/decrementSTWForbiddenScope().
#define JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE 1

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

    // MSPL, rank 7 (SPEC-heap.md §6): serializes block handout, steals,
    // accounting, lower-tier precise allocation, addBlock resizes (I5b), and
    // precise-allocation registration (§5.6) when isSharedServer().
    Lock& mutatorSlowPathLock() WTF_RETURNS_LOCK(m_mutatorSlowPathLock) { return m_mutatorSlowPathLock; }

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

    // UNGIL §0 U0c (ANNEX U0C, BINDING; U-T1): the GIL-off shared-server
    // DESIGNATION primitive — the s_stickySharedServer CAS, returning
    // won/lost, NO assert (noteSharedServerSticky's inner CAS
    // RELEASE_ASSERTs — I13 — so it cannot BE the designation). Under
    // gilOffProcess every VM ctor calls this; the winner sets vm.m_gilOff=1
    // and eagerly calls noteSharedServerSticky() at clientSet()==1
    // (quiescence trivial at birth; I13 sees previous==this and never
    // fires). Idempotent for the winner.
    JS_EXPORT_PRIVATE bool tryDesignateStickySharedServer();

    // UNGIL §0 U0c (ANNEX U0C, BINDING; U-T3): the designation check —
    // RELEASE_ASSERT(gilOffProcess => the server VM's m_gilOff == 1), run
    // immediately before a noteSharedServerSticky() trigger. Under
    // gilOffProcess a LOSER VM (m_gilOff == 0) can never legally reach a
    // trigger (U0b spawn refusal keeps its clientSet() <= 1), so this
    // fail-stops the bug precisely instead of leaving it to I13's inner
    // CAS. No-op (early return) when !gilOffProcess. Defined in
    // runtime/VM.cpp (needs the complete VM type; Heap.cpp is outside this
    // slice's owned-file set).
    //
    // CALL-SITE STATUS (INTEGRATE-ungil.md supersession ledger row 6):
    // - WIRED: the winner-ctor eager trigger (runtime/VM.cpp VM ctor,
    //   immediately before the clientSet()==1 noteSharedServerSticky()).
    // - OPEN — NOT YET WIRED: HeapClientSet::add's second-client trigger
    //   (HeapClientSet.cpp:69 — which STAYS, idempotent; the U0c
    //   SUPERSESSION of heap §5.1's "option && clientSet().size() EVER > 1"
    //   trigger, both sides). The mandated one-line call
    //   `server.verifyStickySharedServerDesignation();` immediately before
    //   noteSharedServerSticky() there could NOT land from this slice:
    //   HeapClientSet.cpp is outside its writable file set, and no later
    //   task currently owns the wiring — ESCALATED to the orchestrator at
    //   the U-T3 amendment; ledger row 6 stays open until it lands. Until
    //   then a loser reaching that trigger is still fail-stopped — by
    //   noteSharedServerSticky's inner I13 one-shared-server RELEASE_ASSERT
    //   (its CAS sees previous != this) — just with the less precise
    //   failure signature this assert exists to improve on.
    JS_EXPORT_PRIVATE void verifyStickySharedServerDesignation();

    // WSAC (F7): written only by the conductor under m_gcBarrierLock; reads acquire.
    bool worldIsStoppedForAllClients() const { return m_worldIsStoppedForAllClients.load(std::memory_order_acquire); }

    // SPEC-ungil §B (heap Dev 8: ONE GCClient PER Thread) / I4 — promotion of
    // the chartered Heap.cpp:5266 namespace-scope helper (apply-scope note
    // items (1)-(3)): resolve the GCClient whose TLC/iso LocalAllocators the
    // CURRENT thread may allocate through. Gate is vm.gilOff(), NOT
    // isSharedServer() (review amendment recorded at the Heap.cpp note):
    // GIL-on and flag-off stay identity BY CONSTRUCTION, and spawned-client
    // stamps cannot exist GIL-on. Unstamped threads (GC helpers, pre-attach)
    // fall back to the VM's original client under the access-owner identity
    // tripwire. Templated on VMType (always VM) ONLY so the ALWAYS_INLINE
    // body can live in this header — VM is incomplete here, and an
    // out-of-line call on the iso fast path would regress the flag-off bench
    // gate (apply-scope item (1)). Defined at the bottom of this header
    // (needs GCClient::Heap), next to the deferralDepthSlot()/
    // mutatorStateSlot() dispatchers it mirrors.
    template<typename VMType>
    static GCClient::Heap& allocationClientForCurrentThread(VMType& vm, GCClient::Heap& vmOriginalClient);

    // IT-9 (SPEC-ungil §B / I4, JIT-codegen leg): resolve the client whose
    // LocalAllocators a JIT compilation may BAKE into generated code
    // (JITAllocator::constant / emitAllocateVariableSized allocator-table
    // base). GIL-off there is NO such client: the compiled artifact is
    // executed by EVERY lite of the VM, and DFG/FTL worklist threads are
    // unstamped, so return null (NULLABLE — every consumer must null-check) —
    // emission then bakes an empty Allocator and every inline allocation
    // takes the slow path, which re-dispatches per-thread through
    // allocationClientForCurrentThread at run time. Never consults the
    // §10A.1 TLS slot, so it is safe on unstamped compiler threads (no
    // access-owner tripwire to trip). GIL-on/flag-off returns
    // &vmOriginalClient — today's behavior, byte-identical codegen
    // (golden-disasm gates unaffected). Interim until U-T7 §B.4 item (1)
    // (lite-relative TLC/iso emission) lands; see VMLite.cpp OPEN list.
    // NOTE: intentionally UNREFERENCED this round — the consuming edits
    // (static iso accessors in VM.h need a mode-aware, pointer-returning
    // Concurrently variant; iso subspaceFor overloads drop the mode) are a
    // separate change set; do NOT delete as dead code.
    // AB17c F4 STATUS: the consumer landed in FUNNEL form —
    // allocatorForConcurrently (runtime/JSCellInlines.h) now returns an
    // empty Allocator when vm.gilOff(), which is semantically this
    // resolver's null arm applied at the single choke point every
    // JITAllocator::constant bake flows through (observed crash it closes:
    // baked per-client iso LocalAllocator => N threads popping ONE FreeList
    // unlocked in DFG NewFunction inline allocation). The per-accessor
    // rewiring (and U-T7 lite-relative emission, which would re-enable
    // inline allocation GIL-off) remains open; keep this resolver for that
    // landing.
    template<typename VMType>
    static GCClient::Heap* allocationClientForJITCodegen(VMType& vm, GCClient::Heap& vmOriginalClient);

    // GSP (F8): read-only view of the stop-pending flag; seq_cst.
    bool gcStopPendingForAllClients() const { return m_gcStopPending.load(std::memory_order_seq_cst); }

    // §10 preconditions: caller holds its client's heap access; no rank >= 4
    // or SAL lock; not inside a stop window.
    JS_EXPORT_PRIVATE void collectSyncAllClients(CollectionScope);
    JS_EXPORT_PRIVATE void requestCollectionAllClients(GCRequest);
    JS_EXPORT_PRIVATE void stopIfNecessaryForAllClients(); // §10A poll, from collectIfNecessaryOrDefer.

    GCSafepointEpoch& safepointEpoch() LIFETIME_BOUND { return m_safepointEpoch; }

    // Registers a hook run once per collection, in BOTH protocols (§9
    // contract notes): legacy (!isSharedServer(), incl. option-off) in
    // runEndPhase just before didFinishCollection(); shared mode at §10
    // step 7. Used by the object-model workstream (quarantined-slot release).
    JS_EXPORT_PRIVATE void addStopTheWorldSafepointHook(void (*)(JSC::Heap&));

    // STW-forbidden scope (I14; debug-only counting): a holder of the
    // structure-allocation lock (and similar) must not initiate/join/wait for
    // a stop-the-world. Checked at the §10 entry points.
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
        JS_EXPORT_PRIVATE explicit JSThreadsStopScope(JSC::Heap&);
        // Watchdog-covered variant (review round): acquires the GCL in
        // bounded tryLock quanta, calling JSThreadsSafepoint::
        // watchdogAssertStopProgress against `watchdogRequestStart` per
        // quantum, so a conductor wedged behind a non-converging shared GC
        // fail-stops at the 30s bound instead of hanging unwatched. Pass the
        // same requestStart that covers the predicate-wait phase: reaching
        // conductor tenure is part of reaching a stopped world.
        JS_EXPORT_PRIVATE JSThreadsStopScope(JSC::Heap&, MonotonicTime watchdogRequestStart);
        JS_EXPORT_PRIVATE ~JSThreadsStopScope();
    private:
        JSC::Heap& m_heap;
        bool m_didLock { false };
    };

    // --- End shared heap server interface ---

    // We're always busy on the collection threads. On the main thread, this returns true if we're
    // helping heap.
    JS_EXPORT_PRIVATE bool currentThreadIsDoingGCWork();
    
    typedef void (*CFinalizer)(JSCell*);
    JS_EXPORT_PRIVATE void addFinalizer(JSCell*, CFinalizer);
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
    // The GC will also push a stopIfNecessary() event onto the runloop of the thread that
    // instantiated the VM whenever it wants the mutator to stop. This means that if you never block
    // but instead use the runloop to wait for events, then you could safely run in a mode where the
    // mutator has permanent heap access (like the DOM does). If you have good event handling
    // discipline (i.e. you don't block the runloop) then you can be sure that stopIfNecessary() will
    // already be called for you at the right times.
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

    void appendPossiblyAccessedStringFromConcurrentThreads(String&& string)
    {
        // GIL-off, N mutators reach this from JSString::swapToAtomString
        // concurrently — an unlocked Vector::append races reserveCapacity
        // (one thread memcpys out of a buffer a sibling just freed). The
        // lock is leaf-rank and uncontended in the single-mutator
        // configurations (one cheap CAS per first-atomization; this is
        // atomization-rate, not transition-rate).
        Locker locker { m_possiblyAccessedStringsFromConcurrentThreadsLock };
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
    void runStopTheWorldSafepointHooks(); // Fired once per collection in both protocols (§9); call sites land in T5.
    static bool currentThreadHasSTWForbiddenScope(); // I14; always false in release.

    // §10A access forwarding (T2): once ISS, the legacy server-level
    // acquireAccess()/releaseAccess()/hasAccess() (called by JSLock and
    // ReleaseHeapAccessScope on the main VM's behalf) forward to the main
    // client's AHA/RHA. JS_EXPORT_PRIVATE because the inline callers above
    // are instantiated outside JSC.
    JS_EXPORT_PRIVATE void acquireAccessForwardedToMainClient();
    JS_EXPORT_PRIVATE void releaseAccessForwardedToMainClient();
    JS_EXPORT_PRIVATE bool mainClientHasHeapAccess() const;

    static constexpr size_t minExtraMemory = 256;
    
    class CFinalizerOwner final : public WeakHandleOwner {
        void finalize(Handle<Unknown>, void* context) final;
    };

    class LambdaFinalizerOwner final : public WeakHandleOwner {
        void finalize(Handle<Unknown>, void* context) final;
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
    void runSharedGCElection(Ticket); // §10.2 election loop; returns once the ticket is served.
    bool tryConductSharedCollectionForPoll(GCClient::Heap&); // Non-blocking election attempt (SINFAC/CIND poll service).
    void conductSharedCollection(GCClient::Heap&); // §10 steps 3-9; pre: GCL held, GCA set.
    void runSafepointHooksAndReclaim(); // §9 hooks + §11 reclaim sequence; both protocols' sole call sites.
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

    MarkedSpace m_objectSpace;
    GCIncomingRefCountedSet<ArrayBuffer> m_arrayBuffers;
    size_t m_extraMemorySize { 0 };
    size_t m_deprecatedExtraMemorySize { 0 };

    ProtectCountSet m_protectedValues;
    UncheckedKeyHashSet<MarkedVectorBase*> m_markListSet;

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
    
    HandleSet m_handleSet;
    std::unique_ptr<CodeBlockSet> m_codeBlocks;
    std::unique_ptr<JITStubRoutineSet> m_jitStubRoutines;
    CFinalizerOwner m_cFinalizerOwner;
    LambdaFinalizerOwner m_lambdaFinalizerOwner;
    
    Lock m_parallelSlotVisitorLock;
    bool m_isSafeToCollect { false };
    bool m_isShuttingDown { false };
    bool m_mutatorShouldBeFenced { false };
    bool m_isMarkingForGCVerifier { false };
    bool m_keepVerifierSlotVisitor { false };
    Lock m_wasmCalleesPendingDestructionLock;

    unsigned m_barrierThreshold { blackThreshold };

#if PLATFORM(MAC)
    Seconds m_lastFullGCLength { 2_ms };
    Seconds m_lastEdenGCLength { 2_ms };
#else
    Seconds m_lastFullGCLength { 10_ms };
    Seconds m_lastEdenGCLength { 10_ms };
#endif

    Vector<WeakBlock*> m_logicallyEmptyWeakBlocks;
    size_t m_indexOfNextLogicallyEmptyWeakBlockToSweep { WTF::notFound };

    Lock m_possiblyAccessedStringsFromConcurrentThreadsLock; // Leaf; guards the vector below (N gilOff mutators append; GC-end clear).
    Vector<String> m_possiblyAccessedStringsFromConcurrentThreads WTF_GUARDED_BY_LOCK(m_possiblyAccessedStringsFromConcurrentThreadsLock);

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
    Lock m_mutatorSlowPathLock; // MSPL, rank 7 (§5.2/§5.6).
    Lock m_gcConductorLock; // GCL, rank 2 (§10/§10C).
    Lock m_gcBarrierLock; // GBL, rank 4 (§10.4/F7).
    Condition m_gcBarrierCondition; // GBC; signaled by clients releasing access while GSP (F8).
    bool m_gcConductorActive { false }; // GCA; guarded by *m_threadLock (rank 5; §10.2).
    Condition m_gcElectionCondition; // GEC; waited on under *m_threadLock (§10.2/§10B.4).
    Atomic<bool> m_gcStopPending { false }; // GSP; sole writer = conductor, seq_cst (F8).
    Atomic<bool> m_isSharedServer { false }; // Sticky ISS (§5.1/I13/§10D).
    Atomic<bool> m_worldIsStoppedForAllClients { false }; // WSAC; conductor-written under GBL (F7).
    Atomic<bool> m_issRevertPending { false }; // §10D; written under *m_threadLock (HeapClientSet::remove / pollIssRevertIfNeeded); relaxed reads are a poll hint only.
    GCClient::Heap* m_mainClient { nullptr }; // First registered client (the owning VM's); written under HeapClientSet::m_lock (§3.3/§10A).
    GCSafepointEpoch m_safepointEpoch; // §11.
    Lock m_stopTheWorldSafepointHookLock;
    Vector<void (*)(JSC::Heap&)> m_stopTheWorldSafepointHooks WTF_GUARDED_BY_LOCK(m_stopTheWorldSafepointHookLock);
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
class MutatorSlowPathLocker : public AbstractLocker {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit MutatorSlowPathLocker(JSC::Heap& heap) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        if (heap.isSharedServer()) [[unlikely]] {
            m_lock = &heap.mutatorSlowPathLock();
            m_lock->lock();
        }
    }

    ~MutatorSlowPathLocker() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        if (m_lock) [[unlikely]]
            m_lock->unlock();
    }

private:
    Lock* m_lock { nullptr };
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
    static constexpr ptrdiff_t offsetOfThreadLocalCache() { return OBJECT_OFFSETOF(Heap, m_threadLocalCache); }

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

    // §10A.1 current-client TLS slot (set by attachCurrentThread() and the
    // server's ISS access forwarding; cleared by detachCurrentThread();
    // releaseHeapAccess() does NOT clear it). Null on non-client threads.
    JS_EXPORT_PRIVATE static Heap* currentThreadClient();

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

    static void setCurrentThreadClient(Heap*); // §10A.1; defined in GCThreadLocalCache.cpp.

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
    bool m_releasedByGCPark { false }; // §10A; written only inside VMManager::notifyVMStop (manifest 5a hooks, JSC::Heap::gcWillParkInStopTheWorld / gcDidResumeFromStopTheWorld; T5).
    Atomic<uint64_t> m_localEpoch { std::numeric_limits<uint64_t>::max() }; // §11; written ONLY by the conductor's stamping loop (world stopped) and detachCurrentThread (MAX) — attach deliberately does NOT stamp it (review round 2: a pre-access stamp can land stale across stop windows and regress bumpAndReclaim's min scan).
    bool m_isStandalone { false }; // §12.1; arms the vm() RELEASE_ASSERT (T9).
    Atomic<uint32_t> m_lastConservativeScanRegisteredUid { 0 }; // I4(b) cache (T6): uid of the last thread this client registered with machineThreads(); relaxed — a stale read merely re-runs the idempotent addCurrentThread().
    unsigned m_deferralDepth { 0 }; // §5.4/I17 (T3): per-client DeferGC depth once ISS; touched only by this client's access-holding thread.
    bool m_didDeferGCWork { false }; // Review round 4: per-client deferred-GC hint once ISS (companion to m_deferralDepth, same touch rules), via JSC::Heap::didDeferGCWorkSlot(). Migrated server<->client at the §10B.4 flip / §10D reversion alongside the depth.
    MutatorState m_mutatorState { MutatorState::Running }; // Review round 2: per-client mutator state once ISS; touched only by this client's access-holding thread (or the conductor's own thread while it conducts), via JSC::Heap::mutatorStateSlot().
    // --- End shared heap client state ---

    friend class JSC::VM;
};

} // namespace GCClient

// SharedGC (§5.4/I17, T3): per-client DeferGC depth dispatch. Defined here
// (below GCClient::Heap) because it needs the client's members; JSC::Heap is
// a friend of GCClient::Heap.
ALWAYS_INLINE unsigned& Heap::deferralDepthSlot()
{
    if (isSharedServer()) [[unlikely]] {
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
    if (isSharedServer()) [[unlikely]] {
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
    if (isSharedServer()) [[unlikely]] {
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
    if (isSharedServer()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == this)
            return client->m_mutatorState;
        // No client TLS stamp (e.g. a GC helper thread world-stopped, or a
        // pre-attach thread): the server field — reads on such threads
        // consult the same field, so save/restore pairing holds.
    }
    return m_mutatorState;
}

// SPEC-ungil §B / I4 (apply-scope items (1)+(2); see the declaration comment
// above): per-thread allocation-client dispatch. Mirrors deferralDepthSlot().
// FIX-V5B-F1 (supersedes the AB17f gilOffWithProcessGate() gate HERE ONLY):
// this predicate runs on EVERY C++ allocation (transition-heavy-constructor
// reaches it once per constructed object via
// operationReallocateButterflyToHavePropertyStorage*), so flag-off it pays
// exactly ONE predicted-false byte test on the Config page
// (g_jscConfig.options.useJSThreads) and never loads a VM/Heap line — the
// per-VM m_gilOff load short-circuits behind it. The gate byte is
// options.useJSThreads, NOT the gilOffProcess latch: gilOffProcess is
// latched mid-first-VM-ctor (Config::finalize(), VM.cpp:711) AFTER the
// m_gilOff designation (VM.cpp:409), so a gilOffProcess-only gate diverges
// from gilOffWithProcessGate() throughout the ctor-body window (VM.cpp
// 473-690): for a first VM constructed OFF the process main thread, the ctor
// JSLockHolder (VM.cpp:473) stamps a fresh OWNED carrier client
// (JSLock.cpp perThreadClientForCarrierEntry gives non-main threads their
// own GCClient::Heap), and routing those allocations to vmOriginalClient —
// whose access no thread holds — is a release-semantics I2 violation, not a
// debug-only skip. The useJSThreads byte has no such window: it is finalized
// in InitializeThreading strictly before any VM ctor (asserted by the
// JSCConfig latch) and lives on the same Config page. By the VM.h
// equivalence invariant (m_gilOff==1 requires Options::useJSThreads()==1 via
// VM::isGILOffProcess(), VM.cpp; both-bytes-0 forces m_gilOff==0), this gate
// returns exactly m_gilOff in EVERY reachable state — value-identical to
// gilOffWithProcessGate() including the pre-latch window, with one byte test
// instead of two flag-off. Designation-loser VMs (m_gilOff==0) keep identity
// through the short-circuit; GIL-on useJSThreads processes read m_gilOff==0
// exactly as the AB17f gate's fallback term did. NOTE: the staged
// out-of-line free-function resolver in Heap.cpp (raw vm.gilOff() gate) is
// intentionally NOT converted — it is not on the flag-off path. If a later
// round latches gilOffProcess BEFORE the m_gilOff designation (the root-cause
// reorder recorded in INTEGRATE-ungil.md), this gate may switch to the
// gilOffProcess byte so flag-on GIL'd processes also skip the m_gilOff load;
// do not switch before that reorder lands.
template<typename VMType>
ALWAYS_INLINE GCClient::Heap& Heap::allocationClientForCurrentThread(VMType& vm, GCClient::Heap& vmOriginalClient)
{
    static_assert(std::is_same_v<VMType, VM>, "templated solely to defer instantiation until VM is complete");
    ASSERT(&vmOriginalClient.server() == &vm.heap);
    if (g_jscConfig.options.useJSThreads && vm.gilOff()) [[unlikely]] {
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
        // FIX-3 carve-out (IT-9 interim; DELETE together with the IT-9 OPEN
        // note above when allocationClientForJITCodegen's consumers land):
        // JIT worklist threads are unstamped BY DESIGN and CANNOT be stamped
        // (no GCClient exists for them — the "stamp that caller" remedy is
        // unsatisfiable). They reach this resolver only through the
        // mode-blind VM.h static iso accessors (e.g. vm.ropeStringSpace()
        // via allocatorForConcurrently<JSRopeString> in compileMakeRope,
        // DFGSpeculativeJIT.cpp): a pure pointer read used to bake an
        // Allocator into the artifact, never a FreeList pop on this thread.
        // The baked-main-client Allocator is the PRE-EXISTING I11 hole that
        // IT-9 tracks; aborting every concurrent compile here neither
        // prevents it (release builds do not assert) nor localizes it, and
        // makes the whole GIL-off full-JIT ladder unrunnable. Mutator-side
        // strength is unchanged: a compilation thread can never legally
        // construct a cell — both JSCell constructors
        // ASSERT(!isCompilationThread()) (runtime/JSCellInlines.h:60/66),
        // and on the barrier path Heap::addToRememberedSet asserts
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

// IT-9: codegen-mode client resolver; see the declaration comment above.
template<typename VMType>
ALWAYS_INLINE GCClient::Heap* Heap::allocationClientForJITCodegen(VMType& vm, GCClient::Heap& vmOriginalClient)
{
    static_assert(std::is_same_v<VMType, VM>, "templated solely to defer instantiation until VM is complete");
    ASSERT(&vmOriginalClient.server() == &vm.heap);
    if (vm.gilOff()) [[unlikely]] {
        // Even a STAMPED caller (synchronous baseline compile on a lite)
        // must not hand its own client to codegen: I11 covers EXECUTION of
        // generated code on the owning thread, not constants baked into an
        // artifact that other lites execute. No client is correct GIL-off.
        return nullptr;
    }
    return &vmOriginalClient;
}

ALWAYS_INLINE MutatorState Heap::mutatorState() const
{
    if (isSharedServer()) [[unlikely]] {
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
