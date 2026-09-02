/*
 *  Copyright (C) 2003-2026 Apple Inc. All rights reserved.
 *  Copyright (C) 2007 Eric Seidel <eric@webkit.org>
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

#include "config.h"
#include "Heap.h"

#include "JSCJSValueInlines.h"

#include "BaselineJITCode.h"
#include "BuiltinExecutables.h"
#include "CachedTypes.h"
#include "CodeBlock.h"
#include "CodeBlockSetInlines.h"
#include "CollectingScope.h"
#include "ConservativeRoots.h"
#include "EdenGCActivityCallback.h"
#include "EvalExecutable.h"
#include "Exception.h"
#include "FastMallocAlignedMemoryAllocator.h"
#include "FullGCActivityCallback.h"
#include "FunctionExecutableInlines.h"
#include "GCActivityCallback.h"
#include "GCIncomingRefCountedInlines.h"
#include "GCIncomingRefCountedSetInlines.h"
#include "GCSegmentedArrayInlines.h"
#include "GCTypeMap.h"
#include "GigacageAlignedMemoryAllocator.h"
#include "HasOwnPropertyCache.h"
#include "HeapHelperPool.h"
#include "HeapIterationScope.h"
#include "HeapProfiler.h"
#include "HeapSnapshot.h"
#include "JSCJSValueInlines.h"
#include "HeapSubspaceTypes.h"
#include "HeapVerifier.h"
#include "IncrementalSweeper.h"
#include "Interpreter.h"
#include "IsoCellSetInlines.h"
#include "IsoInlinedHeapCellTypeInlines.h"
#include "JITStubRoutineSet.h"
#include "JITWorklistInlines.h"
#include "JSFinalizationRegistry.h"
#include "JSThreadsSafepoint.h"
#include "JSFunctionWithFields.h"
#include "JSIterator.h"
#include "JSMicrotaskDispatcher.h"
#include "JSModuleLoader.h"
#include "JSPromiseCombinatorsContext.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSPromiseReaction.h"
#include "JSRawJSONObject.h"
#include "JSRemoteFunction.h"
#include "JSSentinel.h"
#include "JSVirtualMachineInternal.h"
#include "JSWeakMap.h"
#include "JSWeakObjectRef.h"
#include "JSWeakSet.h"
#include "MachineStackMarker.h"
#include "MarkStackMergingConstraint.h"
#include "MarkedSpaceInlines.h"
#include "MarkingConstraintSet.h"
#include "MegamorphicCache.h"
#include "ModuleLoadingContext.h"
#include "ModuleProgramExecutable.h"
#include "ModuleRegistryEntry.h"
#include "NumberObject.h"
#include "PinballCompletion.h"
#include "PreventCollectionScope.h"
#include "RaceAmplifier.h"
#include "ProgramExecutable.h"
#include "ProxyObject.h"
#include "SamplingProfiler.h"
#include "ShadowChicken.h"
#include "SpaceTimeMutatorScheduler.h"
#include "StochasticSpaceTimeMutatorScheduler.h"
#include "StopIfNecessaryTimer.h"
#include "StringSplitCache.h"
#include "StructureAlignedMemoryAllocator.h"
#include "SubspaceInlines.h"
#include "SuperSampler.h"
#include "SweepingScope.h"
#include "SymbolTableInlines.h"
#include "SynchronousStopTheWorldMutatorScheduler.h"
#include "TypeProfiler.h"
#include "TypeProfilerLog.h"
#include "UnlinkedEvalCodeBlock.h"
#include "StopTheWorldCallback.h" // THREADS T5: StopTheWorldEvent for the §10.2 follower park.
#include "Structure.h" // UNGIL §D.1 (U-T12): transition-TID restamp + D1R TTL fires in the rebias stop.
#include "ThreadManager.h" // UNGIL §D.1 (U-T12): the dead-TID snapshot hand-off (two-phase vs §LK).
#include "VM.h"
#include "VMLite.h"
#include "VMLiteShared.h"
#include "VMManager.h" // THREADS T5 (§10.3/§10.9 + manifest items 4-5): requestStopAll/requestResumeAll(StopReason::GC), setGCParkCallbacks.
#include "VMTraps.h" // THREADS T5 (§10.2): election followers poll the stop-the-world trap bit.
#include "VerifierSlotVisitorInlines.h"
#include "WasmCallee.h"
#include "WeakMapImplInlines.h"
#include "WeakSetInlines.h"
#include <algorithm>
#include <bmalloc/bmalloc.h>
#include <wtf/AvailableMemory.h>
#include <wtf/BitVector.h> // UNGIL §D.1 (U-T12): the in-stop dead-TID membership set.
#include <atomic>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/ListDump.h>
#include <wtf/MainThread.h>
#include <wtf/MemoryFootprint.h>
#include <wtf/RAMSize.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/SimpleStats.h>
#include <wtf/StringPrintStream.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include "InternalFieldTuple.h"

#if USE(BUN_JSC_ADDITIONS)
#include "JSFFICallback.h"
#include "JSFFIFunction.h"
#include "JSString.h"
#include <wtf/text/ExternalStringImpl.h>
#endif

#if USE(FOUNDATION)
#include <wtf/spi/cocoa/objcSPI.h>
#endif

#ifdef JSC_GLIB_API_ENABLED
#include "JSCGLibWrapperObject.h"
#endif

namespace JSC {

// ===== gc-sharedheap-zero-concurrent-overlap-now-11pct (SPEC-congc §7.1a) =====
//
// SCALEBENCH §35 round-2 measured Σp = Σcycle (zero mutator/marker overlap)
// under the pinned GIL-off env: every shared collection is a single STW
// window (§3.6 degenerate), and the i#1 root pass (Cs/Msr/Wlr/Msm) + bulk
// drainInParallel runs with all W siblings parked. At W=16 that fixed STW
// floor is ~143 ms = 11.0% of the 1291 ms wall — 45% of the residual JS->Java
// gap (Java W=16 = 976 ms). §27.S2 had previously REFUTED defaulting the full
// stage-C1 flag because at that tree (a) STW was only 5.4% of wall and (b)
// the unbounded scheduler-driven Concurrent handoffs added ~30 extra Reentry
// rendezvous per run (~9 ms each at W=16), eating the saving. Round-1's
// denominator shrink makes (a) no longer hold; (b) is the structural defect
// this lever bounds.
//
// sharedGCWindowedConductActive(): the §3 windowed conduct machinery
// (Reentry open / non-final close / §3.7 wait / F46 per-window atom-table
// pin / per-cycle reclaim placement) is live whenever a §13.2 stage flag is
// on OR the process is GIL-off. Under the gilOff arm the machinery runs in a
// BOUNDED single-handoff shape (§7.1a):
//  - runFixpointPhase schedules AT MOST ONE Concurrent phase per cycle
//    (t_sharedGCConcurrentHandoffsThisCycle, conductor-thread-local; reset in
//    runBeginPhase's gilOff block) — the §27.S2(b) bound: extra rendezvous
//    per conduct = #cycles, not scheduler-driven;
//  - C1R stays OFF (sharedGCBarrierStateIsPerClient() unchanged): barriers
//    keep the server-stack append (serialized across mutators by
//    m_serverMutatorMarkStackLock), drained at the Reentry window's Msm
//    constraint pass; the F19 server-master always-fenced pin
//    holds under gilOff (setMutatorShouldBeFenced's `|| isGILOffProcess()`
//    arm), so addToRememberedSet's unfenced ASSERT(isMarked) is unreachable
//    and the fenced re-whiten CAS path (mutator-count-independent, §5.2
//    CG-T3) covers between-window barrier execution. SPEC-congc §5.2/F44
//    states CMS is contention/accounting only, NOT a soundness gate — so the
//    single-handoff mode's correctness rests on the same §6.2 + §8.1 rules
//    as full C1: Wlr/Cs are GreyedByExecution and re-run at the post-Reentry
//    fixpoint (m_phaseVersion bumped by the Concurrent->Reloop edge), and
//    every per-client LA is re-flushed by the Reentry stopThePeriphery
//    (CG-I6 once-per-window pairing).
// Flag-off (useJSThreads=0 / useThreadGIL=1): VM::isGILOffProcess() is false,
// the predicate degenerates to the §13.2 stage-flag disjunction, and every arm
// keyed on it stays byte-for-byte the §27.S2 default (CG-I0).
//
// The option disjunction below is the ONLY copy of the §13.2 stage-flag list
// (all four default false; Options.cpp's notifyOptionsChanged enforces the §7
// prefix rule and forces each off without useSharedGCHeap, so flags-off this
// is false on a non-shared heap). Every windowed arm keys on this predicate.
static ALWAYS_INLINE bool sharedGCWindowedConductActive()
{
    return Options::useConcurrentSharedGCMarking() || Options::useSharedGCCollectorThread()
        || Options::useSharedGCIncrementalSweep() || Options::useSharedGCMutatorAssist()
        || VM::isGILOffProcess();
}

// Conductor-thread-local per-cycle handoff cap (§7.1a). Written and read only
// on the §3.7 closed-loop conductor thread (one cycle = one thread; CG-I19);
// reset at runBeginPhase's gilOff block, bumped at the runFixpointPhase
// gilOff scheduling arm. Unused outside gilOff (the C1 stage-flag arm is
// scheduler-driven, not capped).
static thread_local unsigned t_sharedGCConcurrentHandoffsThisCycle { 0 };

namespace HeapInternal {
static constexpr bool verbose = false;
static constexpr bool verboseStop = false;
}

namespace {

double maxPauseMS(double thisPauseMS)
{
    static double maxPauseMS;
    maxPauseMS = std::max(thisPauseMS, maxPauseMS);
    return maxPauseMS;
}

size_t minHeapSize(HeapType heapType, size_t ramSize)
{
    switch (heapType) {
    case HeapType::Large:
        return static_cast<size_t>(std::min(
            static_cast<double>(Options::largeHeapSize()),
            ramSize * Options::smallHeapRAMFraction()));
    case HeapType::Medium:
        return Options::mediumHeapSize();
    case HeapType::Small:
        return Options::smallHeapSize();
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
}

size_t proportionalHeapSize(size_t heapSize, size_t ramSize)
{
    if (VM::isInMiniMode())
        return Options::miniVMHeapGrowthFactor() * heapSize;

    bool useNewHeapGrowthFactor = true;

    // Use new heuristic function for machines >= 16GB RAM.
    // https://www.mathway.com/en/Algebra?asciimath=2%20*%20e%5E(-1%20*%20x)%20%2B%201%20%3Dy
    size_t heapGrowthFunctionThresholdInBytes = static_cast<size_t>(Options::heapGrowthFunctionThresholdInMB()) * MB;
    if (ramSize < heapGrowthFunctionThresholdInBytes)
        useNewHeapGrowthFactor = false;

    // Disable it for Darwin Intel machine.
#if OS(DARWIN) && CPU(X86_64)
    useNewHeapGrowthFactor = false;
#endif

    if (useNewHeapGrowthFactor) {
        double x = static_cast<double>(std::min(heapSize, ramSize)) / ramSize;
        double ratio = Options::heapGrowthMaxIncrease() * std::exp(-(Options::heapGrowthSteepnessFactor() * x)) + 1;
        return ratio * heapSize;
    }

#if USE(MEMORY_FOOTPRINT_API)
    size_t memoryFootprint = WTF::memoryFootprint();
    if (memoryFootprint < ramSize * Options::smallHeapRAMFraction())
        return Options::smallHeapGrowthFactor() * heapSize;
    if (memoryFootprint < ramSize * Options::mediumHeapRAMFraction())
        return Options::mediumHeapGrowthFactor() * heapSize;
#else
    if (heapSize < ramSize * Options::smallHeapRAMFraction())
        return Options::smallHeapGrowthFactor() * heapSize;
    if (heapSize < ramSize * Options::mediumHeapRAMFraction())
        return Options::mediumHeapGrowthFactor() * heapSize;
#endif
    return Options::largeHeapGrowthFactor() * heapSize;
}

void recordType(TypeCountSet& set, JSCell* cell)
{
    auto typeName = "[unknown]"_s;
    const ClassInfo* info = cell->classInfo();
    if (info && info->className)
        typeName = info->className;
    set.add(typeName);
}

constexpr bool NODELETE measurePhaseTiming()
{
    return false;
}

UncheckedKeyHashMap<const char*, GCTypeMap<SimpleStats>>& timingStats()
{
    static UncheckedKeyHashMap<const char*, GCTypeMap<SimpleStats>>* result;
    static std::once_flag once;
    std::call_once(
        once,
        [] {
            result = new UncheckedKeyHashMap<const char*, GCTypeMap<SimpleStats>>();
        });
    return *result;
}

SimpleStats& timingStats(const char* name, CollectionScope scope)
{
    return timingStats().add(name, GCTypeMap<SimpleStats>()).iterator->value[scope];
}

class TimingScope {
public:
    TimingScope(std::optional<CollectionScope> scope, ASCIILiteral name)
        : m_scope(scope)
        , m_name(name)
    {
        if (measurePhaseTiming())
            m_before = MonotonicTime::now();
    }
    
    TimingScope(JSC::Heap& heap, ASCIILiteral name)
        : TimingScope(heap.collectionScope(), name)
    {
    }
    
    void NODELETE setScope(std::optional<CollectionScope> scope)
    {
        m_scope = scope;
    }
    
    void NODELETE setScope(JSC::Heap& heap)
    {
        setScope(heap.collectionScope());
    }
    
    ~TimingScope()
    {
        if (measurePhaseTiming()) {
            MonotonicTime after = MonotonicTime::now();
            Seconds timing = after - m_before;
            SimpleStats& stats = timingStats(m_name, *m_scope);
            stats.add(timing.milliseconds());
            dataLog("[GC:", *m_scope, "] ", m_name, " took: ", timing.milliseconds(), "ms (average ", stats.mean(), "ms).\n");
        }
    }
private:
    std::optional<CollectionScope> m_scope;
    MonotonicTime m_before;
    ASCIILiteral m_name;
};

} // anonymous namespace

class Heap::HeapThread final : public AutomaticThread {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(HeapThread);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(HeapThread);
public:
    HeapThread(const AbstractLocker& locker, JSC::Heap& heap)
        : AutomaticThread(locker, heap.m_threadLock, heap.m_threadCondition.copyRef())
        , m_heap(heap)
    {
    }

    ASCIILiteral name() const final
    {
        return "JSC Heap Collector Thread"_s;
    }
    
private:
    PollResult poll(const AbstractLocker& locker) final
    {
        if (m_heap.m_threadShouldStop) {
            m_heap.notifyThreadStopping(locker);
            return PollResult::Stop;
        }
        if (m_heap.shouldCollectInCollectorThread(locker)) {
            m_heap.m_collectorThreadIsRunning = true;
            return PollResult::Work;
        }
        m_heap.m_collectorThreadIsRunning = false;
        return PollResult::Wait;
    }
    
    WorkResult work() final
    {
        m_heap.collectInCollectorThread();
        return WorkResult::Continue;
    }
    
    void threadDidStart() final
    {
        Thread::registerGCThread(GCThreadType::Main);
    }

    void threadIsStopping(const AbstractLocker&) final
    {
        m_heap.m_collectorThreadIsRunning = false;
    }

    JSC::Heap& m_heap;
};

#define INIT_SERVER_ISO_SUBSPACE(name, heapCellType, type) \
    , name ISO_SUBSPACE_INIT(*this, heapCellType, type)

#define INIT_SERVER_STRUCTURE_ISO_SUBSPACE(name, heapCellType, type) \
    , name(#name, *this, heapCellType, WTF::roundUpToMultipleOf<type::atomSize>(sizeof(type)), type::numberOfLowerTierPreciseCells, makeUnique<StructureAlignedMemoryAllocator>())

Heap::Heap(VM& vm, HeapType heapType)
    : m_heapType(heapType)
    , m_ramSize(Options::forceRAMSize() ? Options::forceRAMSize() : ramSize())
    , m_minBytesPerCycle(minHeapSize(m_heapType, m_ramSize))
    , m_maxEdenSize(m_minBytesPerCycle)
    , m_maxHeapSize(m_minBytesPerCycle)
    , m_objectSpace(this)
    , m_machineThreads(makeUnique<MachineThreads>())
    , m_collectorSlotVisitor(makeUnique<SlotVisitor>(*this, "C"_s))
    , m_mutatorSlotVisitor(makeUnique<SlotVisitor>(*this, "M"_s))
    , m_mutatorMarkStack(makeUnique<MarkStackArray>())
    , m_raceMarkStack(makeUnique<MarkStackArray>())
    , m_constraintSet(makeUnique<MarkingConstraintSet>(*this))
    , m_strongSet(vm)
    , m_codeBlocks(makeUnique<CodeBlockSet>())
    , m_jitStubRoutines(makeUnique<JITStubRoutineSet>())
    // We seed with 10ms so that GCActivityCallback::didAllocate doesn't continuously
    // schedule the timer if we've never done a collection.
    , m_fullActivityCallback(FullGCActivityCallback::tryCreate(*this))
    , m_edenActivityCallback(EdenGCActivityCallback::tryCreate(*this))
    , m_sweeper(adoptRef(*new IncrementalSweeper(this)))
    , m_stopIfNecessaryTimer(adoptRef(*new StopIfNecessaryTimer(vm)))
    , m_sharedCollectorMarkStack(makeUnique<MarkStackArray>())
    , m_sharedMutatorMarkStack(makeUnique<MarkStackArray>())
    , m_helperClient(&heapHelperPool())
    , m_threadLock(Box<Lock>::create())
    , m_threadCondition(AutomaticThreadCondition::create())

    // HeapCellTypes
    , auxiliaryHeapCellType(CellAttributes(DoesNotNeedDestruction, HeapCell::Auxiliary))
    , immutableButterflyHeapCellType(CellAttributes(DoesNotNeedDestruction, HeapCell::JSCellWithIndexingHeader))
    , cellHeapCellType(CellAttributes(DoesNotNeedDestruction, HeapCell::JSCell))
    , destructibleCellHeapCellType(CellAttributes(NeedsDestruction, HeapCell::JSCell))
    , apiGlobalObjectHeapCellType(IsoHeapCellType::Args<JSAPIGlobalObject>())
    , callbackConstructorHeapCellType(IsoHeapCellType::Args<JSCallbackConstructor>())
    , callbackGlobalObjectHeapCellType(IsoHeapCellType::Args<JSCallbackObject<JSGlobalObject>>())
    , callbackObjectHeapCellType(IsoHeapCellType::Args<JSCallbackObject<JSNonFinalObject>>())
    , customGetterFunctionHeapCellType(IsoHeapCellType::Args<JSCustomGetterFunction>())
    , customSetterFunctionHeapCellType(IsoHeapCellType::Args<JSCustomSetterFunction>())
    , dateInstanceHeapCellType(IsoHeapCellType::Args<DateInstance>())
    , errorInstanceHeapCellType(IsoHeapCellType::Args<ErrorInstance>())
    , finalizationRegistryCellType(IsoHeapCellType::Args<JSFinalizationRegistry>())
    , globalLexicalEnvironmentHeapCellType(IsoHeapCellType::Args<JSGlobalLexicalEnvironment>())
    , globalObjectHeapCellType(IsoHeapCellType::Args<JSGlobalObject>())
    , injectedScriptHostSpaceHeapCellType(IsoHeapCellType::Args<Inspector::JSInjectedScriptHost>())
    , javaScriptCallFrameHeapCellType(IsoHeapCellType::Args<Inspector::JSJavaScriptCallFrame>())
    , jsModuleRecordHeapCellType(IsoHeapCellType::Args<JSModuleRecord>())
    , syntheticModuleRecordHeapCellType(IsoHeapCellType::Args<SyntheticModuleRecord>())
    , moduleNamespaceObjectHeapCellType(IsoHeapCellType::Args<JSModuleNamespaceObject>())
    , nativeStdFunctionHeapCellType(IsoHeapCellType::Args<JSNativeStdFunction>())
    , weakMapHeapCellType(IsoHeapCellType::Args<JSWeakMap>())
    , weakSetHeapCellType(IsoHeapCellType::Args<JSWeakSet>())
#if JSC_OBJC_API_ENABLED
    , apiWrapperObjectHeapCellType(IsoHeapCellType::Args<JSCallbackObject<JSAPIWrapperObject>>())
    , objCCallbackFunctionHeapCellType(IsoHeapCellType::Args<ObjCCallbackFunction>())
#endif
#ifdef JSC_GLIB_API_ENABLED
    , apiWrapperObjectHeapCellType(IsoHeapCellType::Args<JSCallbackObject<JSAPIWrapperObject>>())
    , callbackAPIWrapperGlobalObjectHeapCellType(IsoHeapCellType::Args<JSCallbackObject<JSAPIWrapperGlobalObject>>())
    , jscCallbackFunctionHeapCellType(IsoHeapCellType::Args<JSCCallbackFunction>())
#endif
    , intlCollatorHeapCellType(IsoHeapCellType::Args<IntlCollator>())
    , intlDateTimeFormatHeapCellType(IsoHeapCellType::Args<IntlDateTimeFormat>())
    , intlDisplayNamesHeapCellType(IsoHeapCellType::Args<IntlDisplayNames>())
    , intlDurationFormatHeapCellType(IsoHeapCellType::Args<IntlDurationFormat>())
    , intlListFormatHeapCellType(IsoHeapCellType::Args<IntlListFormat>())
    , intlLocaleHeapCellType(IsoHeapCellType::Args<IntlLocale>())
    , intlNumberFormatHeapCellType(IsoHeapCellType::Args<IntlNumberFormat>())
    , intlPluralRulesHeapCellType(IsoHeapCellType::Args<IntlPluralRules>())
    , intlRelativeTimeFormatHeapCellType(IsoHeapCellType::Args<IntlRelativeTimeFormat>())
    , intlSegmentIteratorHeapCellType(IsoHeapCellType::Args<IntlSegmentIterator>())
    , intlSegmenterHeapCellType(IsoHeapCellType::Args<IntlSegmenter>())
    , intlSegmentsHeapCellType(IsoHeapCellType::Args<IntlSegments>())
#if USE(BUN_JSC_ADDITIONS)
    , ffiFunctionHeapCellType(IsoHeapCellType::Args<JSFFIFunction>())
    , ffiCallbackHeapCellType(IsoHeapCellType::Args<JSFFICallback>())
#endif
#if ENABLE(WEBASSEMBLY)
    , webAssemblyExceptionHeapCellType(IsoHeapCellType::Args<JSWebAssemblyException>())
    , webAssemblyFunctionHeapCellType(IsoHeapCellType::Args<WebAssemblyFunction>())
    , webAssemblyGlobalHeapCellType(IsoHeapCellType::Args<JSWebAssemblyGlobal>())
    , webAssemblyInstanceHeapCellType(IsoHeapCellType::Args<JSWebAssemblyInstance>())
    , webAssemblyMemoryHeapCellType(IsoHeapCellType::Args<JSWebAssemblyMemory>())
    , webAssemblyModuleHeapCellType(IsoHeapCellType::Args<JSWebAssemblyModule>())
    , webAssemblyModuleRecordHeapCellType(IsoHeapCellType::Args<WebAssemblyModuleRecord>())
    , webAssemblyTableHeapCellType(IsoHeapCellType::Args<JSWebAssemblyTable>())
    , webAssemblyTagHeapCellType(IsoHeapCellType::Args<JSWebAssemblyTag>())
#endif
    // AlignedMemoryAllocators
    , fastMallocAllocator(makeUnique<FastMallocAlignedMemoryAllocator>())
    , primitiveGigacageAllocator(makeUnique<GigacageAlignedMemoryAllocator>(Gigacage::Primitive))

    // Subspaces
    , primitiveGigacageAuxiliarySpace("Primitive Gigacage Auxiliary"_s, *this, auxiliaryHeapCellType, primitiveGigacageAllocator.get()) // Hash:0x3e7cd762
    , auxiliarySpace("Auxiliary"_s, *this, auxiliaryHeapCellType, fastMallocAllocator.get()) // Hash:0x96255ba1
    , immutableButterflyAuxiliarySpace("ImmutableButterfly JSCellWithIndexingHeader"_s, *this, immutableButterflyHeapCellType, fastMallocAllocator.get()) // Hash:0xaadcb3c1
    , cellSpace("JSCell"_s, *this, cellHeapCellType, fastMallocAllocator.get()) // Hash:0xadfb5a79
    , destructibleObjectSpace("JSDestructibleObject"_s, *this, destructibleObjectHeapCellType, fastMallocAllocator.get()) // Hash:0x4f5ed7a9
    FOR_EACH_JSC_COMMON_ISO_SUBSPACE(INIT_SERVER_ISO_SUBSPACE)
    FOR_EACH_JSC_STRUCTURE_ISO_SUBSPACE(INIT_SERVER_STRUCTURE_ISO_SUBSPACE)
    , codeBlockSpaceAndSet ISO_SUBSPACE_INIT(*this, destructibleCellHeapCellType, CodeBlock) // Hash:0x2b743c6a
    , functionExecutableSpaceAndSet ISO_SUBSPACE_INIT(*this, destructibleCellHeapCellType, FunctionExecutable) // Hash:0xbcb36268
    , programExecutableSpaceAndSet ISO_SUBSPACE_INIT(*this, destructibleCellHeapCellType, ProgramExecutable) // Hash:0x4c9208f7
    , unlinkedFunctionExecutableSpaceAndSet ISO_SUBSPACE_INIT(*this, destructibleCellHeapCellType, UnlinkedFunctionExecutable) // Hash:0x3ba0f4e1

{
    if (Options::forceFencedBarrier()) {
        m_mutatorShouldBeFenced = true;
        m_barrierThreshold = tautologicalThreshold;
    }

    m_worldState.store(0);

    // §11 (T7): the epoch is a by-value member; wire its server back-pointer
    // here so bumpAndReclaim() can assert I11 and walk the client registry.
    m_safepointEpoch.setServer(*this);

    if (Options::useJSThreads()) [[unlikely]]
        m_retiredStructureChainInvalidationWatchpoints = makeUnique<RetiredStructureChainInvalidationWatchpoints>();

    for (unsigned i = 0, numberOfParallelThreads = heapHelperPool().numberOfThreads(); i < numberOfParallelThreads; ++i) {
        std::unique_ptr<SlotVisitor> visitor = makeUnique<SlotVisitor>(*this, toCString("P", i + 1));
        if (Options::optimizeParallelSlotVisitorsForStoppedMutator())
            visitor->optimizeForStoppedMutator();
        m_availableParallelSlotVisitors.append(visitor.get());
        m_parallelSlotVisitors.append(WTF::move(visitor));
    }
    
    if (Options::useConcurrentGC()) {
        if (Options::useStochasticMutatorScheduler())
            m_scheduler = makeUnique<StochasticSpaceTimeMutatorScheduler>(*this);
        else
            m_scheduler = makeUnique<SpaceTimeMutatorScheduler>(*this);
    } else {
        // We simulate turning off concurrent GC by making the scheduler say that the world
        // should always be stopped when the collector is running.
        m_scheduler = makeUnique<SynchronousStopTheWorldMutatorScheduler>();
    }
    
    if (Options::verifyHeap())
        m_verifier = makeUnique<HeapVerifier>(this, Options::numberOfGCCyclesToRecordForVerification());
    
    m_collectorSlotVisitor->optimizeForStoppedMutator();

    // When memory is critical, allow allocating 25% of the amount above the critical threshold before collecting.
    size_t memoryAboveCriticalThreshold = static_cast<size_t>(static_cast<double>(m_ramSize) * (1.0 - Options::criticalGCMemoryThreshold()));
    m_maxEdenSizeWhenCritical = memoryAboveCriticalThreshold / 4;

    Locker locker { *m_threadLock };
    lazyInitialize(m_thread, adoptRef(*new HeapThread(locker, *this)));
}

#undef INIT_SERVER_ISO_SUBSPACE
#undef INIT_SERVER_STRUCTURE_ISO_SUBSPACE

Heap::~Heap()
{
    // Scribble m_worldState to make it clear that the heap has already been destroyed if we crash in checkConn
    m_worldState.store(0xbadbeeffu);

    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            visitor.clearMarkStacks();
        });
    m_mutatorMarkStack->clear();
    m_raceMarkStack->clear();

    for (WeakBlock* block : m_logicallyEmptyWeakBlocks)
        WeakBlock::destroy(*this, block);
}

bool Heap::isPagedOut()
{
    return m_objectSpace.isPagedOut();
}

void Heap::dumpHeapStatisticsAtVMDestruction()
{
    unsigned counter = 0;
    // SharedGC (T8): VM-destruction context — no other client thread can be
    // running JS against this server; MSPL covers the I5b bit reads of the
    // iteration (MarkedSpace::stopAllocating's teardown carve-out).
    MutatorSlowPathLocker mutatorSlowPathLocker(*this);
    HeapIterationScope iterationScope(*this);
    m_objectSpace.forEachBlock([&] (MarkedBlock::Handle* block) {
        unsigned live = 0;
        block->forEachLiveCell([&] (size_t, HeapCell*, HeapCell::Kind) {
            live++;
            return IterationStatus::Continue;
        });
        dataLogLn("[", counter++, "] ", block->cellSize(), ", ", live, " / ", block->cellsPerBlock(), " ", static_cast<double>(live) / block->cellsPerBlock() * 100, "% ", block->attributes(), " ", block->subspace()->name());
        block->forEachLiveCell([&] (size_t, HeapCell* heapCell, HeapCell::Kind kind) {
            if (kind == HeapCell::Kind::JSCell) {
                auto* cell = static_cast<JSCell*>(heapCell);
                if (cell->isObject())
                    dataLogLn("    ", JSValue((JSObject*)cell));
                else
                    dataLogLn("    ", *cell);
            }
            return IterationStatus::Continue;
        });
    });
}

// The VM is being destroyed and the collector will never run again.
// Run all pending finalizers now because we won't get another chance.
void Heap::stopCollectingContinuously()
{
    if (!m_collectContinuouslyThread)
        return;
    {
        Locker locker { m_collectContinuouslyLock };
        m_shouldStopCollectingContinuously = true;
        m_collectContinuouslyCondition.notifyOne();
    }
    m_collectContinuouslyThread->waitForCompletion();
    m_collectContinuouslyThread = nullptr;
}

void Heap::prepareForVMDestruction()
{
    if (!isSharedServer())
        return;
    // In the shared heap, a granted ticket is served by a mutator that conducts
    // the collection, and the destroying thread is the last mutator. Once it
    // gives up its heap access, lastChanceToFinalize would wait for a conductor
    // that never comes. So stop the thread that requests collections, then
    // serve what it requested. (The legacy heap's collector thread serves the
    // tickets by itself.)
    stopCollectingContinuously();
    Ticket pending;
    {
        Locker locker { *m_threadLock };
        pending = m_lastServedTicket < m_lastGrantedTicket ? m_lastGrantedTicket : 0;
    }
    if (pending)
        runSharedGCElection(pending);
}

void Heap::lastChanceToFinalize()
{
    MonotonicTime before;
    if (Options::logGC()) [[unlikely]] {
        before = MonotonicTime::now();
        dataLog("[GC<", RawPointer(this), ">: shutdown ");
    }
    
    m_isShuttingDown = true;

    // SharedGC (T9): main-VM-only — server shutdown runs on the main VM's
    // destruction path (secondary clients must already have detached/removed;
    // HeapClientSet teardown ordering, I13).
    RELEASE_ASSERT(!vm().entryScope);
    RELEASE_ASSERT(m_mutatorState == MutatorState::Running);
    
    stopCollectingContinuously();

    dataLogIf(Options::logGC(), "1");
    
    // Prevent new collections from being started. This is probably not even necessary, since we're not
    // going to call into anything that starts collections. Still, this makes the algorithm more
    // obviously sound.
    m_isSafeToCollect = false;
    
    dataLogIf(Options::logGC(), "2");

    bool isCollecting;
    {
        Locker locker { *m_threadLock };
        RELEASE_ASSERT(m_lastServedTicket <= m_lastGrantedTicket);
        isCollecting = m_lastServedTicket < m_lastGrantedTicket;
    }
    if (isCollecting) {
        dataLogIf(Options::logGC(), "...]\n");
        
        // Wait for the current collection to finish.
        waitForCollector(
            [&] (const AbstractLocker&) -> bool {
                RELEASE_ASSERT(m_lastServedTicket <= m_lastGrantedTicket);
                return m_lastServedTicket == m_lastGrantedTicket;
            });
        
        dataLogIf(Options::logGC(), "[GC<", RawPointer(this), ">: shutdown ");
    }
    dataLogIf(Options::logGC(), "3");

    RELEASE_ASSERT(m_requests.isEmpty());
    RELEASE_ASSERT(m_lastServedTicket == m_lastGrantedTicket);
    
    // Carefully bring the thread down.
    bool stopped = false;
    {
        Locker locker { *m_threadLock };
        stopped = m_thread->tryStop(locker);
        m_threadShouldStop = true;
        if (!stopped)
            m_threadCondition->notifyOne(locker);
    }

    dataLogIf(Options::logGC(), "4");
    
    if (!stopped)
        m_thread->join();
    
    dataLogIf(Options::logGC(), "5 ");

    if (Options::dumpHeapStatisticsAtVMDestruction()) [[unlikely]]
        dumpHeapStatisticsAtVMDestruction();
    
    m_arrayBuffers.lastChanceToFinalize();
    // No safepoint will ever reclaim the epoch's retired items, and their
    // destroy thunks may write into heap memory (a retired Weak<JSArrayBuffer>
    // from SimpleTypedArrayController::registerWrapper clears its WeakImpl
    // inside a WeakBlock), so they must be destroyed before the object space
    // is finalized and freed. Cell destructors below can retire more items
    // (flag-on IC chains from ~CodeBlock), hence the second drain.
    m_safepointEpoch.drainForTeardown();
    // Retired enumerator watchpoints unlink from Structures' transition
    // watchpoint sets, so destroy them while those cells are still intact.
    if (m_retiredStructureChainInvalidationWatchpoints) [[unlikely]]
        m_retiredStructureChainInvalidationWatchpoints->destroyAll();
    {
        // SharedGC (T8/§5.3 teardown): a stale sticky-ISS flag can outlive
        // the last secondary client until the §10D revert poll, so server
        // teardown's directory-bit flips and final sweeps run under MSPL
        // (no-op when !isSharedServer(), I10). Dropped around
        // releaseDelayedReleasedObjects(), which may re-enter JS.
        MutatorSlowPathLocker mutatorSlowPathLocker(*this);
        m_objectSpace.lastChanceToFinalize();
    }
    releaseDelayedReleasedObjects();
#if ENABLE(WEBASSEMBLY)
    Wasm::TypeInformation::cleanupIfRequested();
#endif

    m_safepointEpoch.drainForTeardown();

    sweepAllLogicallyEmptyWeakBlocks(); // Takes MSPL itself when shared (T8).

    {
        MutatorSlowPathLocker mutatorSlowPathLocker(*this);
        m_objectSpace.freeMemory();
    }
    
    dataLogIf(Options::logGC(), (MonotonicTime::now() - before).milliseconds(), "ms]\n");
}

void Heap::releaseDelayedReleasedObjects()
{
#if USE(FOUNDATION) || defined(JSC_GLIB_API_ENABLED)
    // We need to guard against the case that releasing an object can create more objects due to the
    // release calling into JS. When those JS call(s) exit and all locks are being dropped we end up
    // back here and could try to recursively release objects. We guard that with a recursive entry
    // count. Only the initial call will release objects, recursive calls simple return and let the
    // the initial call to the function take care of any objects created during release time.
    // This also means that we need to loop until there are no objects in m_delayedReleaseObjects
    // and use a temp Vector for the actual releasing.
    if (!m_delayedReleaseRecursionCount++) {
        while (!m_delayedReleaseObjects.isEmpty()) {
            // SharedGC (T9): main-VM-only — Foundation/GLib delayed releases
            // are API-lock-coupled (DropAllLocks below targets the main VM's
            // JSLock); GIL-phase sound via JSLock migration (I2).
            ASSERT(vm().currentThreadIsHoldingAPILock());

            auto objectsToRelease = WTF::move(m_delayedReleaseObjects);

            {
                // We need to drop locks before calling out to arbitrary code.
                JSLock::DropAllLocks dropAllLocks(vm());

#if USE(FOUNDATION)
                void* context = objc_autoreleasePoolPush();
#endif
                objectsToRelease.clear();
#if USE(FOUNDATION)
                objc_autoreleasePoolPop(context);
#endif
            }
        }
    }
    m_delayedReleaseRecursionCount--;
#endif
}

void Heap::reportExtraMemoryAllocatedPossiblyFromAlreadyMarkedCell(const JSCell* cell, size_t size)
{
    ASSERT(cell);

    // Increasing extraMemory of already marked objects will not be visible as a retained memory.
    // We need to report this additionally to tell GC that we get additional extra memory now,
    // and GC needs to consider scheduling GC based on this increase.

    if (mutatorShouldBeFenced()) [[unlikely]] {
        // In this case, the barrierThreshold is the tautological threshold, so cell could still be
        // not black. But we can't know for sure until we fire off a fence.
        WTF::storeLoadFence();
        if (cell->cellState() != CellState::PossiblyBlack)
            return;

        WTF::loadLoadFence();
        if (!isMarked(cell)) {
            // During a full collection a store into an unmarked object that had surivived past
            // collections will manifest as a store to an unmarked PossiblyBlack object. If the
            // object gets marked at some time after this then it will go down the normal marking
            // path. So, we don't have to remember this object. We could return here. But we go
            // further and attempt to re-white the object.
            ASSERT(m_collectionScope && m_collectionScope.value() == CollectionScope::Full);
            return;
        }
    } else
        ASSERT(isMarked(cell));

    // It could be that the object was *just* marked. This means that the collector may set the
    // state to DefinitelyGrey and then to PossiblyOldOrBlack at any time. It's OK for us to
    // race with the collector here. If we win then this is accurate because the object _will_
    // get scanned again. If we lose then someone else will barrier the object again. That would
    // be unfortunate but not the end of the world.
    reportExtraMemoryVisited(size);
}

void Heap::reportExtraMemoryAllocatedSlowCase(GCDeferralContext* deferralContext, const JSCell* cell, size_t size)
{
    didAllocate(size);
    if (cell) {
        if (isWithinThreshold(cell->cellState(), barrierThreshold())) [[unlikely]]
            reportExtraMemoryAllocatedPossiblyFromAlreadyMarkedCell(cell, size);
    }
    collectIfNecessaryOrDefer(deferralContext);
}

void Heap::deprecatedReportExtraMemorySlowCase(size_t size)
{
    // FIXME: Change this to use SaturatingArithmetic when available.
    // https://bugs.webkit.org/show_bug.cgi?id=170411
    CheckedSize checkedNewSize = m_deprecatedExtraMemorySize;
    checkedNewSize += size;
    size_t newSize = std::numeric_limits<size_t>::max();
    if (!checkedNewSize.hasOverflowed()) [[likely]]
        newSize = checkedNewSize.value();
    m_deprecatedExtraMemorySize = newSize;
    reportExtraMemoryAllocatedSlowCase(nullptr, nullptr, size);
}

ALWAYS_INLINE bool Heap::activityCallbackDispatchAllowed()
{
    // T4(c): activity-callback timer state (GCActivityCallback::m_delay and
    // friends) is plain data historically guarded by "one mutator thread".
    // Once shared, multiple clients reach the didAllocate dispatch sites
    // concurrently; the timers are bound to the MAIN VM's run loop and their
    // doWork fires on the main client's thread, so restricting mutator-side
    // dispatch to that same thread restores the single-writer regime without
    // re-disabling the callbacks wholesale (the pre-T4 state, which removed
    // every idle-time GC trigger once ISS and helped make capacity
    // monotone). World-stopped dispatch sites (updateAllocationLimits) call
    // the callbacks directly instead: every mutator is parked there, so no
    // concurrent access exists regardless of the conducting thread.
    if (!isSharedServer()) [[likely]]
        return true;
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    return client && client == m_mainClient;
}

bool Heap::overCriticalMemoryThreshold(MemoryThresholdCallType memoryThresholdCallType)
{
#if USE(MEMORY_FOOTPRINT_API)
    if (memoryThresholdCallType == MemoryThresholdCallType::Direct || ++m_percentAvailableMemoryCachedCallCount >= 100) {
        m_overCriticalMemoryThreshold = WTF::percentAvailableMemoryInUse() > Options::criticalGCMemoryThreshold();
        m_percentAvailableMemoryCachedCallCount = 0;
    }

    return m_overCriticalMemoryThreshold;
#else
    UNUSED_PARAM(memoryThresholdCallType);
    return false;
#endif
}

size_t Heap::effectiveMaxEdenSize()
{
    if (overCriticalMemoryThreshold())
        return std::min(m_maxEdenSize, m_maxEdenSizeWhenCritical);
    return m_maxEdenSize;
}

void Heap::reportAbandonedObjectGraph()
{
    // Our clients don't know exactly how much memory they
    // are abandoning so we just guess for them.
    size_t abandonedBytes = static_cast<size_t>(0.1 * capacity());

    m_bytesAbandonedSinceLastFullCollect.fetch_add(abandonedBytes, std::memory_order_relaxed); // F3.

    // We want to accelerate the next collection. Because memory has just
    // been abandoned, the next collection has the potential to
    // be more profitable. Since allocation is the trigger for collection,
    // we hasten the next collection by pretending that we've allocated more memory.
    // T4(c): re-enabled when shared (was the §5.4/I15 blanket disable). The
    // callback's plain timer state is single-writer-safe here only on the
    // main client's thread (the timer is bound to the main VM's run loop and
    // doWork runs there); under shared, restrict dispatch to that thread —
    // the I15 no-fire-and-forget invariant is preserved by collectAsync's
    // ISS reroute to ticketing, not by suppressing the timer.
    if (m_fullActivityCallback && activityCallbackDispatchAllowed()) {
        m_fullActivityCallback->didAllocate(*this,
            m_sizeAfterLastCollect - m_sizeAfterLastFullCollect + totalBytesAllocatedThisCycle() + m_bytesAbandonedSinceLastFullCollect.load(std::memory_order_relaxed));
    }
}

void Heap::protect(JSValue k)
{
    ASSERT(k);
    // The protect set is server state. The caller must be entered (GIL-off:
    // holds an entry token, hence heap access), which is what keeps the
    // collector's lock-free root walk from racing it; mutual exclusion
    // between entered GIL-off mutators comes from m_protectedValuesLock.
    ASSERT(vm().currentThreadIsHoldingAPILock());

    if (!k.isCell())
        return;

    if (vm().gilOff()) [[unlikely]] {
        Locker locker { m_protectedValuesLock };
        m_protectedValues.add(k.asCell());
        return;
    }
    m_protectedValues.add(k.asCell());
}

bool Heap::unprotect(JSValue k)
{
    ASSERT(k);
    ASSERT(vm().currentThreadIsHoldingAPILock());

    if (!k.isCell())
        return false;

    if (vm().gilOff()) [[unlikely]] {
        Locker locker { m_protectedValuesLock };
        return m_protectedValues.remove(k.asCell());
    }
    return m_protectedValues.remove(k.asCell());
}

void Heap::pinRetiredCallLinkRecordCodeBlock(void* codeBlock)
{
    // See the declaration comment (SPEC-jit §5.8/§4.4 record-named CodeBlock
    // identity). Flag-on only; callers guarantee codeBlock is the non-null
    // codeBlockToTransfer of a record being PUBLISHED on this (server) heap
    // (w16 amend: the pin is taken at publish, while the linking mutator
    // provably holds the cell live, and spans the record's whole reachable
    // lifetime — live, then retired until epoch expiry, or until the owning
    // CallLinkInfo's destructor frees the record inline; both paths unpin
    // through destroyUnreachableCallLinkRecord).
    ASSERT(Options::useJSThreads());
    ASSERT(codeBlock);
    Locker locker { m_retiredCallLinkRecordCodeBlocksLock };
    m_retiredCallLinkRecordCodeBlocks.add(codeBlock);
}

void Heap::unpinRetiredCallLinkRecordCodeBlock(void* codeBlock)
{
    ASSERT(Options::useJSThreads());
    ASSERT(codeBlock);
    Locker locker { m_retiredCallLinkRecordCodeBlocksLock };
    m_retiredCallLinkRecordCodeBlocks.remove(codeBlock);
}

void Heap::addReference(JSCell* cell, ArrayBuffer* buffer)
{
    if (m_arrayBuffers.addReference(cell, buffer)) {
        collectIfNecessaryOrDefer();
        didAllocate(buffer->gcSizeEstimateInBytes());
    }
}

template<typename CellType, typename CellSet>
void Heap::reconcileWeakReferencesInMarkedCells(CellSet& cellSet, CollectionScope collectionScope)
{
    // SharedGC (T9): conductor-context OK — end-phase work, world stopped
    // (worldIsStopped() / WSAC once shared); vm() is the main mutator VM
    // (deviation 3), the only VM whose cells live in this server phase 1.
    // No JS runs in unconditional finalizers (§10B.5: no JS finalizers in
    // the stop window).
    cellSet.forEachMarkedCell(
        [&] (HeapCell* cell, HeapCell::Kind) {
            static_cast<CellType*>(cell)->reconcileWeakReferencesAtGCEnd(vm(), collectionScope);
        });
}

// Weak reference reconciliation: settle every untraced pointer against the liveness that
// marking just established. Must run after marking, because isMarked() only means "dead"
// once the closure is complete, and before sweeping, because a dying referent may still
// need to be identified or read.
void Heap::reconcileWeakReferencesAtGCEnd()
{
    CollectionScope collectionScope = this->collectionScope().value_or(CollectionScope::Full);

    {
        // Executables go before CodeBlock, since CodeBlock::reconcileWeakReferencesAtGCEnd looks at the owner executable's installed CodeBlock.

        // FunctionExecutable requires all live instances to be processed, so iterate the whole space rather than a tracking set.
        reconcileWeakReferencesInMarkedCells<FunctionExecutable>(functionExecutableSpaceAndSet.space, collectionScope);

        reconcileWeakReferencesInMarkedCells<ProgramExecutable>(programExecutableSpaceAndSet.weakReconciliationSet, collectionScope);
        if (m_evalExecutableSpace)
            reconcileWeakReferencesInMarkedCells<EvalExecutable>(m_evalExecutableSpace->weakReconciliationSet, collectionScope);
        if (m_moduleProgramExecutableSpace)
            reconcileWeakReferencesInMarkedCells<ModuleProgramExecutable>(m_moduleProgramExecutableSpace->weakReconciliationSet, collectionScope);
    }

    reconcileWeakReferencesInMarkedCells<SymbolTable>(symbolTableSpace, collectionScope);

    forEachCodeBlockSpace(
        [&] (auto& space) {
            this->reconcileWeakReferencesInMarkedCells<CodeBlock>(space.set, collectionScope);
        });
    if (collectionScope == CollectionScope::Full) {
        reconcileWeakReferencesInMarkedCells<Structure>(structureSpace, collectionScope);
        reconcileWeakReferencesInMarkedCells<BrandedStructure>(brandedStructureSpace, collectionScope);
#if ENABLE(WEBASSEMBLY)
        reconcileWeakReferencesInMarkedCells<WebAssemblyGCStructure>(webAssemblyGCStructureSpace, collectionScope);
#endif
    }
    reconcileWeakReferencesInMarkedCells<StructureRareData>(structureRareDataSpace, collectionScope);
    // Mutators are stopped and no cell has been swept yet, so destroying the
    // retired enumerator watchpoints (which unlinks them from watched
    // Structures' transition watchpoint sets) neither races a foreign walker
    // nor touches freed memory; a watchpoint never outlives its rare data's
    // sweep.
    if (m_retiredStructureChainInvalidationWatchpoints) [[unlikely]]
        m_retiredStructureChainInvalidationWatchpoints->destroyAll();
    reconcileWeakReferencesInMarkedCells<UnlinkedFunctionExecutable>(unlinkedFunctionExecutableSpaceAndSet.set, collectionScope);
    if (m_weakSetSpace)
        reconcileWeakReferencesInMarkedCells<JSWeakSet>(*m_weakSetSpace, collectionScope);
    if (m_weakMapSpace)
        reconcileWeakReferencesInMarkedCells<JSWeakMap>(*m_weakMapSpace, collectionScope);
    if (m_weakObjectRefSpace)
        reconcileWeakReferencesInMarkedCells<JSWeakObjectRef>(*m_weakObjectRefSpace, collectionScope);
    if (m_errorInstanceSpace)
        reconcileWeakReferencesInMarkedCells<ErrorInstance>(*m_errorInstanceSpace, collectionScope);

    // FinalizationRegistries currently rely on serial finalization because they can post tasks to the deferredWorkTimer, which normally expects tasks to only be posted by the API lock holder.
    if (m_finalizationRegistrySpace)
        reconcileWeakReferencesInMarkedCells<JSFinalizationRegistry>(*m_finalizationRegistrySpace, collectionScope);

#if ENABLE(WEBASSEMBLY)
    if (m_webAssemblyInstanceSpace)
        reconcileWeakReferencesInMarkedCells<JSWebAssemblyInstance>(*m_webAssemblyInstanceSpace, collectionScope);
#endif

    vm().reconcileWeakReferencesAtGCEnd();

    if (auto* clientData = vm().clientData)
        clientData->reconcileWeakReferencesAtGCEnd(vm(), collectionScope);
}

void Heap::willStartIterating()
{
    m_objectSpace.willStartIterating();
}

void Heap::didFinishIterating()
{
    m_objectSpace.didFinishIterating();
}

void Heap::runWithOtherClientsStoppedSlow(const ScopedLambda<void()>& func)
{
    if (worldIsStoppedForAllClients() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor())) {
        func();
        return;
    }
    // A heap becomes a shared server only for a GIL-off VM.
    VM& vm = this->vm();
    ASSERT(vm.gilOff());
    // The window reads the heap and, for deleteAllCode, clears code that no
    // thread is running. It rewrites no object or Structure a parked thread's
    // optimized code depends on, so parked threads need not jettison on resume.
    JSThreadsSafepoint::PureCodeLifecycleStopWindowScope noHeapFactRewrite;
    JSThreadsSafepoint::ClassAStopWatchdogContext watchdogContext(this, "heap walk");
    JSThreadsSafepoint::stopTheWorldAndRun(vm, func);
}

void Heap::completeAllJITPlans()
{
    if (!Options::useJIT())
        return;
#if ENABLE(JIT)
    // SharedGC (T9): conductor-context OK — vm() is a worklist KEY (plans are
    // tagged by the one main VM phase 1), not a calling-thread assumption.
    // Post-GIL (deviation 8) clients are per-thread within the SAME VM, so
    // the key stays singular; no clientSet() iteration needed.
    JITWorklist::ensureGlobalWorklist().completeAllPlansForVM(vm());
#endif // ENABLE(JIT)
}

template<typename Visitor>
void Heap::iterateExecutingAndCompilingCodeBlocks(Visitor& visitor, NOESCAPE const Function<void(CodeBlock*)>& func)
{
    m_codeBlocks->iterateCurrentlyExecuting(func);
#if ENABLE(JIT)
    // SharedGC (T9): conductor-context OK — vm() = worklist key (see
    // completeAllJITPlans()); runs while marking with the world stopped.
    if (Options::useJIT())
        JITWorklist::ensureGlobalWorklist().iterateCodeBlocksForGC(visitor, vm(), func);
#else
    UNUSED_PARAM(visitor);
#endif // ENABLE(JIT)
}

template<typename Func, typename Visitor>
void Heap::iterateExecutingAndCompilingCodeBlocksWithoutHoldingLocks(Visitor& visitor, const Func& func)
{
    Vector<CodeBlock*, 256> codeBlocks;
    iterateExecutingAndCompilingCodeBlocks(visitor,
        [&] (CodeBlock* codeBlock) {
            codeBlocks.append(codeBlock);
        });
    for (CodeBlock* codeBlock : codeBlocks)
        func(codeBlock);
}

void Heap::assertMarkStacksEmpty()
{
    bool ok = true;
    
    if (!m_sharedCollectorMarkStack->isEmpty()) {
        dataLog("FATAL: Shared collector mark stack not empty! It has ", m_sharedCollectorMarkStack->size(), " elements.\n");
        ok = false;
    }
    
    if (!m_sharedMutatorMarkStack->isEmpty()) {
        dataLog("FATAL: Shared mutator mark stack not empty! It has ", m_sharedMutatorMarkStack->size(), " elements.\n");
        ok = false;
    }
    
    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            if (visitor.isEmpty())
                return;
            
            dataLog("FATAL: Visitor ", RawPointer(&visitor), " is not empty!\n");
            ok = false;
        });
    
    RELEASE_ASSERT(ok);
}

// Shared-server GC verifier state, live only under Options::verifyGC(): the
// conservative stack roots of the current cycle's gather, re-checked for a
// version-current liveness bit in Heap::endMarking(). Conductor-private:
// written and read only while the world is stopped for all clients.
static Vector<HeapCell*>& sharedGCStackRootSnapshot()
{
    static Vector<HeapCell*>* snapshot = new Vector<HeapCell*>;
    return *snapshot;
}

void Heap::gatherStackRoots(ConservativeRoots& roots)
{
    // SharedGC §10.6 (T6): one MachineThreads scan covers all N mutators.
    // Every thread that ever acquired heap access on any client of this
    // server is I4(b)-registered (enforced in GCClient::Heap::
    // acquireHeapAccess and in attachCurrentThread), so the suspend-and-copy
    // pass inside gatherConservativeRoots scans every registered thread
    // other than the conductor; the conductor's own stack and registers flow
    // through m_currentThreadState/m_currentThread (set by runCurrentPhase —
    // in shared mode the conductor runs as GCConductor::Mutator, §10B.2).
    ASSERT(worldIsStopped());
    // I5: in shared mode the scan runs only on the conductor (or its
    // parallel helpers) while the world is stopped for all clients; the
    // §10.4 access barrier completed, so no client thread can be mutating
    // the heap while we suspend it.
    ASSERT(!isSharedServer() || worldIsStoppedForAllClients());
#if ASSERT_ENABLED
    if (isSharedServer()) {
        clientSet().forEach([](GCClient::Heap& client) {
            if (!client.hasHeapAccess()) [[likely]]
                return;
            // F8 step-1 flicker (counter-lock contgc finding, root-caused
            // live: the flagged client read NoAccess/owner-null again by
            // the time the abort handler dumped it, with GSP and WSAC both
            // still set): a mid-window FRESH acquirer is LICENSED to flip
            // its access byte HasAccess transiently — F8 step 1's seq_cst
            // CAS — before its MANDATORY step-3 revert (it sampled GSP set
            // and never enters the heap; the §10.4 barrier's convergence
            // is one-shot and unaffected by post-convergence flickers).
            // A single racy sample here cannot distinguish that licensed
            // transient from a real admission violation — but TIME can: a
            // truly admitted client SUSTAINS access (it is off running JS).
            // Wait out the flicker; sustained in-window access stays a
            // hard fail-stop in the stop-watchdog budget class. The revert
            // needs no lock this walk holds (it is a CAS + GBL notify; GBL
            // is rank 4, this walk holds the rank-6 HCS lock only).
            MonotonicTime flickerDeadline = MonotonicTime::now() + Seconds(30);
            while (client.hasHeapAccess()) {
                RELEASE_ASSERT(MonotonicTime::now() < flickerDeadline); // Real §10.4 admission violation: client kept in-window heap access.
                Thread::yield();
            }
        });
    }
#endif
    // T5-rootscan-skip-coop-parked-suspend (SCALEBENCH §31, offcpu16 row #4):
    // build the Thread* -> coop-parked-snapshot lookup the suspend loop will
    // consult to skip the SIGUSR2 round-trip for siblings that already
    // published a register/stack snapshot at their park site. The Thread* ->
    // GCClient::Heap* mapping IS the existing HeapClientSet (maintained
    // under its rank-6 registry lock and FROZEN inside this stop window by
    // I13's add/remove deferral) keyed on each client's
    // m_parkedRootSnapshotThread, so a transient HashMap built here under
    // forEach()'s registry-lock hold is exactly that map specialised to the
    // clients that have a live snapshot — no separate persistent map state on
    // the server. The seq_cst snapshot load pairs against the publishing
    // thread's seq_cst store (after its seq_cst RHA) and the conductor's own
    // GSP/stop-word seq_cst stores: any sibling that released-then-published
    // before the §10.4 barrier converged is visible here, and any sibling
    // that has since cleared (about to re-acquire) reads null and falls back
    // to suspend. Gated on isSharedServer() so flag-off passes nullptr and
    // tryCopyOtherThreadStacks runs its original suspend-everything path
    // byte-for-byte; W=1 IMPACT ZERO — every publish call site requires the
    // calling thread to NOT be the §A.3 conductor / Mode-stop
    // representative, which is impossible with one thread, so the table is
    // empty and every registered thread (just the conductor, excluded
    // anyway) goes through the unchanged path.
    // LIFETIME (T5-amend, review-major): cache GCClient::Heap* (stable for
    // the stop window; registry frozen by I13), NOT the CurrentThreadState*
    // itself, and re-load parkedRootSnapshot() seq_cst in the lookup lambda
    // at USE time inside tryCopyOtherThreadStacks. Site (b)'s publish
    // (VMManager notifyVMStop sibling bounded-poll) wraps a 1ms waitFor that
    // can expire and clear() BETWEEN this forEach and the copy, and again
    // between growBuffer retry iterations that re-invoke the lambda over the
    // SAME table; a cached snapshot pointer would dangle into a dead block
    // scope (or, worse, a sibling since admitted into HelperDrain whose
    // cleared snapshot must per the gcClientPublishParkedRootSnapshot
    // contract fall back to suspend). The re-load returns null in both cases
    // and the suspend path runs. The residual seq_cst-load->dereference
    // window is closed on the publish side: site (b) declares its
    // CurrentThreadState at the gilOff sibling-loop's enclosing scope so the
    // struct memory stays live (last-spilled, valid) for every iteration.
    UncheckedKeyHashMap<Thread*, GCClient::Heap*> coopParkedClients;
    if (isSharedServer()) [[unlikely]] {
        clientSet().forEach([&](GCClient::Heap& client) {
            if (client.parkedRootSnapshot())
                coopParkedClients.add(client.parkedRootSnapshotThread(), &client);
        });
    }
    auto coopParkedSnapshotLookupFunctor = [&](Thread& thread) -> CurrentThreadState* {
        if (GCClient::Heap* client = coopParkedClients.get(&thread))
            return client->parkedRootSnapshot(); // seq_cst re-load AT USE TIME: cleared sibling falls back to suspend (incl. across growBuffer retries).
        return nullptr;
    };
    ScopedLambda<CurrentThreadState*(Thread&)> coopParkedSnapshotLookup(coopParkedSnapshotLookupFunctor);
    m_machineThreads->gatherConservativeRoots(roots, *m_jitStubRoutines, *m_codeBlocks, m_currentThreadState, m_currentThread, coopParkedClients.isEmpty() ? nullptr : &coopParkedSnapshotLookup);
#if ENABLE(C_LOOP)
    // SharedGC (I12, T6): the CLoop stack is per-VM, not per-thread. Phase 1
    // the shared server is the main VM's heap (deviation 3) and JS execution
    // migrates over that one VM under the JSLock (deviation 8, GIL phase),
    // so the main VM's CLoopStack is the only one; vm() here is the server's
    // back-pointer and is valid even when the conductor is a VM-less
    // standalone client (§12.1 — conductor-context OK, T9). Standalone
    // clients never run JS and have no CLoop stack to scan.
    // THREADS-INTEGRATE(heap): post-GIL, if Thread() ever gets per-thread
    // CLoopStacks (one VM, N stacks), iterate them here per I12.
    vm().cloopStack().gatherConservativeRoots(roots, *m_jitStubRoutines, *m_codeBlocks);
#endif

    // Verifier (Options::verifyGC()): snapshot this gather's roots for the
    // endMarking() liveness check. If the constraint runs more than once per
    // cycle the last gather wins, which is sound: mutators stay parked for the
    // whole conducted cycle, so only the conductor's own roots can differ
    // between gathers, and every gather's roots are marked after the snapshot.
    if (isSharedServer() && Options::verifyGC()) [[unlikely]] {
        auto& snapshot = sharedGCStackRootSnapshot();
        snapshot.shrink(0);
        for (size_t i = 0; i < roots.size(); ++i)
            snapshot.append(roots.roots()[i]);
    }
}

void Heap::gatherVMRoots(ConservativeRoots& roots)
{
    // SharedGC (T9): conductor-context OK — root gathering, world stopped
    // (WSAC once shared, I5); scratch buffers / side state are VM-global
    // state of the one main VM. Post-GIL per-THREAD scratch state moves to
    // VMLite (vmstate spec); if that lands as per-client state this becomes
    // a clientSet().forEach() site.
    VM& vm = this->vm();
#if ENABLE(DFG_JIT)
    if (Options::useJIT()) {
        vm.gatherScratchBufferRoots(roots);
        vm.scanSideState(roots);
    }
#endif
#if !ENABLE(DFG_JIT)
    UNUSED_PARAM(roots);
    UNUSED_VARIABLE(vm);
#endif
}

void Heap::beginMarking()
{
    TimingScope timingScope(*this, "Heap::beginMarking"_s);
    m_jitStubRoutines->clearMarks();
    m_objectSpace.beginMarking();
    // SharedGC (T9): conductor-context OK — world stopped (I5); VM-global
    // mark bookkeeping of the one main VM.
    vm().beginMarking();
    setMutatorShouldBeFenced(true);
}

void Heap::removeDeadCompilerWorklistEntries()
{
    if (!Options::useJIT())
        return;
#if ENABLE(JIT)
    // SharedGC (T9): conductor-context OK — vm() = worklist key (see
    // completeAllJITPlans()); end-phase, world stopped.
    JITWorklist::ensureGlobalWorklist().removeDeadPlans(vm());
#endif // ENABLE(JIT)
}

struct GatherExtraHeapData : MarkedBlock::CountFunctor {
    GatherExtraHeapData(HeapAnalyzer& analyzer)
        : m_analyzer(analyzer)
    {
    }

    IterationStatus operator()(HeapCell* heapCell, HeapCell::Kind kind) const
    {
        if (isJSCellKind(kind)) {
            JSCell* cell = static_cast<JSCell*>(heapCell);
            cell->methodTable()->analyzeHeap(cell, m_analyzer);
        }
        return IterationStatus::Continue;
    }

    HeapAnalyzer& m_analyzer;
};

void Heap::gatherExtraHeapData(HeapProfiler& heapProfiler)
{
    if (auto* analyzer = heapProfiler.activeHeapAnalyzer()) {
        HeapIterationScope heapIterationScope(*this);
        GatherExtraHeapData functor(*analyzer);
        m_objectSpace.forEachLiveCell(heapIterationScope, functor);
    }
}

struct RemoveDeadHeapSnapshotNodes : MarkedBlock::CountFunctor {
    RemoveDeadHeapSnapshotNodes(HeapSnapshot& snapshot)
        : m_snapshot(snapshot)
    {
    }

    IterationStatus operator()(HeapCell* cell, HeapCell::Kind kind) const
    {
        if (isJSCellKind(kind))
            m_snapshot.sweepCell(static_cast<JSCell*>(cell));
        return IterationStatus::Continue;
    }

    HeapSnapshot& m_snapshot;
};

void Heap::removeDeadHeapSnapshotNodes(HeapProfiler& heapProfiler)
{
    if (HeapSnapshot* snapshot = heapProfiler.mostRecentSnapshot()) {
        HeapIterationScope heapIterationScope(*this);
        RemoveDeadHeapSnapshotNodes functor(*snapshot);
        m_objectSpace.forEachDeadCell(heapIterationScope, functor);
        snapshot->shrinkToFit();
    }
}

void Heap::updateObjectCounts()
{
    if (m_collectionScope && m_collectionScope.value() == CollectionScope::Full) {
        m_totalBytesVisitedAfterLastFullCollect = m_totalBytesVisited;
        m_totalBytesVisited = 0;
    }

    m_totalBytesVisitedThisCycle = bytesVisited();
    
    m_totalBytesVisited += m_totalBytesVisitedThisCycle;
}

void Heap::endMarking()
{
    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            visitor.reset();
        });

    assertMarkStacksEmpty();

    RELEASE_ASSERT(m_raceMarkStack->isEmpty());

    // Verifier (Options::verifyGC()): marking is complete and every
    // conservative stack root was appended to a visitor, which either set its
    // mark bit or skipped it as version-current newlyAllocated, so each
    // snapshot cell must carry one of the two version-current liveness bits.
    // Runs before m_objectSpace.endMarking() retires the newlyAllocated
    // version. A trap here means this cycle's marking lost a
    // conservatively-reachable cell.
    if (isSharedServer() && Options::verifyGC()) [[unlikely]] {
        auto& snapshot = sharedGCStackRootSnapshot();
        for (HeapCell* cell : snapshot) {
            if (cell->isPreciseAllocation()) {
                if (!cell->preciseAllocation().isLive()) [[unlikely]] {
                    dataLogLn(
                        "SharedGC verifier: conservative stack root ",
                        RawPointer(cell), " (precise allocation) is neither marked nor newlyAllocated at end of marking (scope = ",
                        *m_collectionScope, ")");
                    RELEASE_ASSERT_NOT_REACHED();
                }
                continue;
            }
            MarkedBlock& block = cell->markedBlock();
            bool marked = !block.areMarksStale(m_objectSpace.markingVersion()) && block.isMarkedRaw(cell);
            bool newlyAllocated = !block.isNewlyAllocatedStale() && block.isNewlyAllocated(cell);
            if (!marked && !newlyAllocated) [[unlikely]] {
                dataLogLn(
                    "SharedGC verifier: conservative stack root ",
                    RawPointer(cell), " in block ", RawPointer(&block),
                    " of directory ", RawPointer(block.handle().directory()),
                    " (cellSize = ", block.handle().directory()->cellSize(),
                    ") has no version-current liveness bit at end of marking (scope = ",
                    *m_collectionScope,
                    ", marksStale = ", block.areMarksStale(m_objectSpace.markingVersion()),
                    ", newlyAllocatedStale = ", block.isNewlyAllocatedStale(), ")");
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        snapshot.shrink(0);
    }

    // SPEC-heap I4/I5 — shared-server window-liveness retention now happens
    // INSIDE the marking fixpoint, as the "Wlr" core marking constraint (see
    // addCoreConstraints()), not here. Rationale (EVIDENCE.md §10/§11): the
    // original endMarking()-time form set mark bits on window-witnessed cells
    // WITHOUT tracing them, after the fixpoint had closed — sound for sweep
    // (no IsEmpty rehand-out) but unsound for every consumer that treats a
    // mark bit as "this cell is a consistent live object": the transition
    // WeakGCHashTable prune (runs after endMarking, keys on mark bits) kept
    // marked-dead dictionary Structures FINDABLE while their untraced
    // PropertyTables were swept, and a same-shape re-add adopted the zombie
    // with a stale table (objectmodel/i03-quarantine-readd-across-gc.js,
    // deterministic). Running the same witness scan as a constraint makes
    // the retained set closed under tracing for free: appended cells are
    // marked AND traced, weak-set/output constraints re-run on the new marks
    // via the normal convergence loop, and by the time we get here every
    // retained cell is a fully consistent member of the mark set. By this
    // point marking is complete, so the witnesses retired by
    // m_objectSpace.endMarking() below are no longer load-bearing.

    m_objectSpace.endMarking();
    setMutatorShouldBeFenced(Options::forceFencedBarrier());
#if USE(BUN_JSC_ADDITIONS)
    if (vm().clientData) {
        if (auto* table = vm().clientData->decoderStringTable())
            table->didFinishCollection();
    }
#endif
}

size_t Heap::objectCount()
{
    return m_objectSpace.objectCount();
}

size_t Heap::arrayBufferSize()
{
    return m_arrayBuffers.size();
}

size_t Heap::extraMemorySize()
{
    // FIXME: Change this to use SaturatingArithmetic when available.
    // https://bugs.webkit.org/show_bug.cgi?id=170411
    CheckedSize checkedTotal = m_extraMemorySize;
    checkedTotal += m_deprecatedExtraMemorySize;
    checkedTotal += m_arrayBuffers.size();
    size_t total = std::numeric_limits<size_t>::max();
    if (!checkedTotal.hasOverflowed()) [[likely]]
        total = checkedTotal.value();

    // It would be nice to have `ASSERT(m_objectSpace.capacity() >= m_objectSpace.size());` here but `m_objectSpace.size()`
    // requires having heap access which thread might not. Specifically, we might be called from the resource usage thread.
    return std::min(total, std::numeric_limits<size_t>::max() - m_objectSpace.capacity());
}

size_t Heap::size()
{
    return m_objectSpace.size() + extraMemorySize();
}

size_t Heap::capacity()
{
    return m_objectSpace.capacity() + extraMemorySize();
}

size_t Heap::protectedGlobalObjectCount()
{
    size_t result = 0;
    forEachProtectedCell(
        [&] (JSCell* cell) {
            if (cell->isObject() && asObject(cell)->isGlobalObject())
                result++;
        });
    return result;
}

size_t Heap::globalObjectCount()
{
    size_t result = 0;
    runWithOtherClientsStopped([&] {
        HeapIterationScope iterationScope(*this);
        m_objectSpace.forEachLiveCell(
            iterationScope,
            [&] (HeapCell* heapCell, HeapCell::Kind kind) -> IterationStatus {
                if (!isJSCellKind(kind))
                    return IterationStatus::Continue;
                JSCell* cell = static_cast<JSCell*>(heapCell);
                if (cell->isObject() && asObject(cell)->isGlobalObject())
                    result++;
                return IterationStatus::Continue;
            });
    });
    return result;
}

size_t Heap::protectedObjectCount()
{
    size_t result = 0;
    forEachProtectedCell(
        [&] (JSCell*) {
            result++;
        });
    return result;
}

TypeCountSet Heap::protectedObjectTypeCounts()
{
    TypeCountSet result;
    forEachProtectedCell(
        [&] (JSCell* cell) {
            recordType(result, cell);
        });
    return result;
}

TypeCountSet Heap::objectTypeCounts()
{
    TypeCountSet result;
    runWithOtherClientsStopped([&] {
        HeapIterationScope iterationScope(*this);
        m_objectSpace.forEachLiveCell(
            iterationScope,
            [&] (HeapCell* cell, HeapCell::Kind kind) -> IterationStatus {
                if (isJSCellKind(kind))
                    recordType(result, static_cast<JSCell*>(cell));
                return IterationStatus::Continue;
            });
    });
    return result;
}

void Heap::deleteAllCodeBlocks(DeleteAllCodeEffort effort)
{
    if (m_collectionScope && effort == DeleteAllCodeIfNotCollecting)
        return;

    PreventCollectionScope preventCollectionScope(*this);
    deleteAllCodeBlocksWithCollectionPrevented();
}

void Heap::deleteAllCodeBlocksWithCollectionPrevented()
{
    // Reached only through VM::deleteAllCode and VM::deleteAllLinkedCode. For
    // a GIL-off VM they call this inside a stop window in which no thread of
    // the VM is entered (VM::whenIdleWithOtherThreadsStopped), so no other
    // thread can run, compile against, or return into the code cleared here.
    VM& vm = this->vm();

    // If JavaScript is running, it's not safe to delete all JavaScript code, since
    // we'll end up returning to deleted code. GIL-off, vm.entryScope is never
    // written; isEntered() asks every thread of the VM.
    RELEASE_ASSERT(!vm.isEntered());
    RELEASE_ASSERT(!m_collectionScope);

    completeAllJITPlans();

    runWithOtherClientsStopped([&] {
        forEachScriptExecutableSpace(
            [&] (auto& spaceAndSet) {
                HeapIterationScope heapIterationScope(*this);
                auto& set = spaceAndSet.clearableCodeSet;
                set.forEachLiveCell(
                    [&] (HeapCell* cell, HeapCell::Kind) {
                        ScriptExecutable* executable = static_cast<ScriptExecutable*>(cell);
                        executable->clearCode(set);
                    });
            });
    });

    // MicrotaskCallCache lives outside any CodeBlock and keys its cached entry points on the callee's
    // executable, so after the code is detached above its callee check would still hit and call into it.
    vm.clearMicrotaskCallCaches();

#if ENABLE(WEBASSEMBLY)
    {
        // We must ensure that we clear the JS call ICs from Wasm. Otherwise, Wasm will
        // have no idea that we cleared the code from all of the Executables in the
        // VM. This could leave Wasm in an inconsistent state where it has an IC that
        // points into a CodeBlock that could be dead. The IC will still succeed because
        // it uses a callee check, but then it will call into dead code.

        // PreciseAllocations are always eagerly swept so we don't have to worry about handling instances pending destruction thus need a HeapIterationScope
        if (m_webAssemblyInstanceSpace) {
            m_webAssemblyInstanceSpace->forEachLiveCell([&] (HeapCell* cell, HeapCell::Kind kind) {
                ASSERT_UNUSED(kind, kind == HeapCell::JSCell);
                static_cast<JSWebAssemblyInstance*>(cell)->clearJSCallICs(vm);
            });
        }
    }
#endif
}

void Heap::deleteAllUnlinkedCodeBlocks(DeleteAllCodeEffort effort)
{
    if (m_collectionScope && effort == DeleteAllCodeIfNotCollecting)
        return;

    PreventCollectionScope preventCollectionScope(*this);
    deleteAllUnlinkedCodeBlocksWithCollectionPrevented();
}

void Heap::deleteAllUnlinkedCodeBlocksWithCollectionPrevented()
{
    // Same callers and the same stop window as deleteAllCodeBlocksWithCollectionPrevented().
    VM& vm = this->vm();

    RELEASE_ASSERT(!m_collectionScope);

    runWithOtherClientsStopped([&] {
        HeapIterationScope heapIterationScope(*this);
        unlinkedFunctionExecutableSpaceAndSet.set.forEachLiveCell(
            [&] (HeapCell* cell, HeapCell::Kind) {
                UnlinkedFunctionExecutable* executable = static_cast<UnlinkedFunctionExecutable*>(cell);
                executable->clearCode(vm);
            });
        clearUnlinkedBaselineCodeCaches();
    });
}

void Heap::clearUnlinkedBaselineCodeCaches()
{
#if ENABLE(JIT)
    // Shareable Baseline JIT code is cached on UnlinkedCodeBlock::m_unlinkedBaselineCode (populated by
    // CodeBlock::setupWithUnlinkedBaselineCode). That cache is not owned by any linked CodeBlock or
    // executable, so it survives deleteAllCodeBlocks and is otherwise only released when the
    // UnlinkedCodeBlock itself is collected. A "warm" UnlinkedCodeBlock can therefore pin Baseline JIT
    // executable memory across memory warnings indefinitely. Drop the cache eagerly here: any still-linked
    // CodeBlock holds its own ref to the BaselineJITCode (so we never free code that is still in use),
    // while a cache-only entry is freed as soon as its last ref goes away, synchronously here.
    if (Options::useBaselineJITCodeSharing()) {
        auto clearUnlinkedBaselineCode = [] (HeapCell* cell, HeapCell::Kind) {
            // Readers copy the field under the lock (unlinkedBaselineCodeConcurrently).
            auto* unlinkedCodeBlock = static_cast<UnlinkedCodeBlock*>(cell);
            ConcurrentJSLocker locker(unlinkedCodeBlock->m_lock);
            unlinkedCodeBlock->m_unlinkedBaselineCode = nullptr;
        };
        for (auto* space : { m_unlinkedFunctionCodeBlockSpace.get(), m_unlinkedProgramCodeBlockSpace.get(), m_unlinkedEvalCodeBlockSpace.get(), m_unlinkedModuleProgramCodeBlockSpace.get() }) {
            if (space)
                space->forEachLiveCell(clearUnlinkedBaselineCode);
        }
    }
#endif
}

void Heap::deleteUnmarkedCompiledCode()
{
    // SharedGC (T9): conductor-context OK — end-phase, world stopped; vm()
    // is VM-global stub-routine bookkeeping of the one main VM. CodeBlock
    // RECLAMATION (freeing jettisoned code) additionally goes through the
    // §11 epoch (GCSafepointEpoch), which is per-CLIENT — that is the
    // per-client-iteration half, handled in runSafepointHooksAndReclaim().
    m_jitStubRoutines->deleteUnmarkedJettisonedStubRoutines(vm());
}

// Baseline JIT code is cached on the UnlinkedCodeBlock (CodeBlock::setupWithUnlinkedBaselineCode) so a re-created
// CodeBlock can reuse it, but nothing ever releases that cache: once every CodeBlock that used it has died, the machine
// code stays for as long as the unlinked code does. Drop cache entries that no CodeBlock has used for a baseline TTL
// lease; a later warm-up simply compiles baseline again.
void Heap::releaseUnusedSharedBaselineCode()
{
#if ENABLE(JIT) && USE(BUN_JSC_ADDITIONS)
    if (!Options::useBaselineJITCodeSharing() || !Options::useExecutionCountForCodeBlockAging())
        return;
    MonotonicTime cutoff = m_currentGCStartTime - CodeBlock::timeToLive(JITType::BaselineJIT) * Options::codeBlockAgingLeaseMultiplier();
    // End phase: the world is stopped and allocation already is, so no HeapIterationScope.
    auto visit = [&] (HeapCell* cell, HeapCell::Kind) {
        auto& code = static_cast<UnlinkedCodeBlock*>(cell)->m_unlinkedBaselineCode;
        if (code && code->hasOneRef() && code->m_ownerWentAwayAt < cutoff)
            code = nullptr;
    };
    for (auto* space : { m_unlinkedFunctionCodeBlockSpace.get(), m_unlinkedProgramCodeBlockSpace.get(), m_unlinkedEvalCodeBlockSpace.get(), m_unlinkedModuleProgramCodeBlockSpace.get() }) {
        if (space)
            space->forEachLiveCell(visit);
    }
#endif
}

void Heap::addToRememberedSet(const JSCell* constCell)
{
    JSCell* cell = const_cast<JSCell*>(constCell);
    ASSERT(cell);
    ASSERT(!Options::useConcurrentJIT() || !isCompilationThread());
    // Relaxed load + store (not an RMW): m_barriersExecuted is a stats/heuristic
    // counter (JIT SPEC §5.7.7 advisory datum); with N mutators the increments
    // race by design and lost updates are tolerated. The relaxed accesses remove
    // the plain-access UB without changing flag-off codegen (plain mov/inc).
    WTF::atomicStore(&m_barriersExecuted, WTF::atomicLoad(&m_barriersExecuted, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    // SPEC-congc §5.2 (CG-2): when C1R (F33/CGD4.4), this slow path routes
    // via the calling thread's §10A.1 client — the fence read consults the
    // client's §5.3(2) copy and the append (tail of this function) goes to
    // the client's CMS. The three arms:
    //  - C1R + client of this server: per-client routing.
    //  - C1R + NULL client (F31; ANNEX CGD4.5(a) GOVERNS): conductor-context
    //    barrier execution (runEndPhase / finalize-side writeBarrier
    //    batches, GC helpers) — SERVER stack + SERVER fence master, sound
    //    because it occurs ONLY in-window (WSAC => single writer); debug
    //    assert below. (A CLIENT-conductor's in-window appends instead land
    //    in its own CMS via the first arm and are NEXT-CYCLE grey,
    //    CGD4.5(b) — same landed semantics as end-phase server-stack
    //    appends, MarkStackMergingConstraint.cpp.)
    //  - !C1R (incl. all flags off): server fence master + the server stack.
    //    Under useSharedGCHeap every attached mutator's slow path lands
    //    here, so the append is serialized by m_serverMutatorMarkStackLock;
    //    with the option off the single mutator appends unlocked (CG-I0).
    GCClient::Heap* routedClient = nullptr;
    bool fenced;
    if (sharedGCBarrierStateIsPerClient()) [[unlikely]] {
        routedClient = GCClient::Heap::currentThreadClient();
        if (!routedClient)
            ASSERT(worldIsStoppedForAllClients()); // CGD4.5(a): null client => conductor context, in-window only.
        else if (&routedClient->server() != this)
            routedClient = nullptr; // Foreign-server client thread: server arm (locked append); no WSAC implication.
        fenced = routedClient ? routedClient->m_mutatorShouldBeFenced : m_mutatorShouldBeFenced;
    } else
        fenced = m_mutatorShouldBeFenced;
    if (fenced) {
        WTF::loadLoadFence();
        if (!isMarked(cell)) {
            // During a full collection a store into an unmarked object that had surivived past
            // collections will manifest as a store to an unmarked PossiblyBlack object. If the
            // object gets marked at some time after this then it will go down the normal marking
            // path. So, we don't have to remember this object. We could return here. But we go
            // further and attempt to re-white the object.
            
            RELEASE_ASSERT(m_collectionScope && m_collectionScope.value() == CollectionScope::Full);
            
            if (cell->atomicCompareExchangeCellStateStrong(CellState::PossiblyBlack, CellState::DefinitelyWhite) == CellState::PossiblyBlack) {
                // Now we protect against this race:
                //
                //     1) Object starts out black + unmarked.
                //     --> We do isMarked here.
                //     2) Object is marked and greyed.
                //     3) Object is scanned and blacked.
                //     --> We do atomicCompareExchangeCellStateStrong here.
                //
                // In this case we would have made the object white again, even though it should
                // be black. This check lets us correct our mistake. This relies on the fact that
                // isMarked converges monotonically to true.
                if (isMarked(cell)) {
                    // It's difficult to work out whether the object should be grey or black at
                    // this point. We say black conservatively.
                    cell->setCellState(CellState::PossiblyBlack);
                }
                
                // Either way, we can return. Most likely, the object was not marked, and so the
                // object is now labeled white. This means that future barrier executions will not
                // fire. In the unlikely event that the object had become marked, we can still
                // return anyway, since we proved that the object was not marked at the time that
                // we executed this slow path.
            }
            
            return;
        }
    } else
        ASSERT(isMarked(cell));
    // It could be that the object was *just* marked. This means that the collector may set the
    // state to DefinitelyGrey and then to PossiblyOldOrBlack at any time. It's OK for us to
    // race with the collector here. If we win then this is accurate because the object _will_
    // get scanned again. If we lose then someone else will barrier the object again. That would
    // be unfortunate but not the end of the world.
    cell->setCellState(CellState::PossiblyGrey);
    if (routedClient) [[unlikely]] {
        // §5.2 CMS append. The CMS lock is LK.9c, a TERMINAL leaf; this site
        // holds at most rank 7-9b allocation-side locks, under which a
        // terminal leaf is legal (CG-I10/F21; SINFAC I6 legalizes the CG-3
        // donation site). Not a nested LK.9d>LK.9c site: NO outer
        // m_markingMutex here — that edge exists only at the WND-open drain.
        Locker locker { routedClient->m_mutatorMarkStackLock };
        if (!routedClient->m_mutatorMarkStack) [[unlikely]]
            routedClient->m_mutatorMarkStack = makeUnique<MarkStackArray>();
        routedClient->m_mutatorMarkStack->append(cell);
        return;
    }
    if (Options::useSharedGCHeap()) [[unlikely]] {
        Locker locker { m_serverMutatorMarkStackLock };
        m_mutatorMarkStack->append(cell);
        return;
    }
    m_mutatorMarkStack->append(cell);
}

void Heap::clearConcurrentRetainedDataIfPossible()
{

    // FIXME: It's weird that we drive the runloop in the middle of a JS stack. But a few places in WebCore testing/debugger code do that so clearing would otherwise be invalid there. This is technically not strong enough to catch any bad code but it seems to work for the testing/debugger code in question.
    if (vm().entryScope) [[unlikely]]
        return;

    // It wouldn't be safe to clear the list if it's possible to have a GCOwnedDataScope on the stack since
    // m_possiblyAccessedStringsFromConcurrentThreadsOrGCOwnedDataScope
    // is what's keeping the backing bytes alive.
    ASSERT(!m_topGCOwnedDataScope);

    if (!m_possiblyAccessedStringsFromConcurrentThreadsOrGCOwnedDataScope.size())
        return;

    // The mutator needs to be fenced while marking and marker threads can access StringImpl::costDuringGC so we have to keep the Impls alive.
    if (mutatorShouldBeFenced())
        return;
#if ENABLE(JIT)
    auto* worklist = JITWorklist::existingGlobalWorklistOrNull();
    // We need to make sure no JIT thread could be looking at one of our old strings. Any thread that starts after
    // this check will load the new StringImpl rather than the one in this list so we're safe to delete these as
    // long as none were running at the time of this check.
    if (!worklist || !worklist->totalOngoingCompilations()) {
#else
    {
#endif
        m_possiblyAccessedStringsFromConcurrentThreadsOrGCOwnedDataScope.clear();
    }
}

void Heap::sweepSynchronously()
{
    if (!Options::useGC()) [[unlikely]]
        return;

    // SharedGC (T8/I5b): callers are either the conductor (finalize()'s
    // shouldSweepSynchronously() path, inside the stop window) or an
    // access-holding requester (collectNow(Sync)'s tail) racing other
    // clients' allocation slow paths. Hold MSPL across the sweep (and the
    // shrink leg, which only executes world-stopped once shared — see
    // below): serializes the lock-free directory-bit reads in
    // MarkedBlock::Handle::sweep against addBlock's m_bits resize, and the
    // block frees against the block registry/weak-set lists. No-op when
    // !isSharedServer() (I10). MSPL holders never hold it across a collection
    // request (L2) — sweeping requests none. Taking MSPL while the world is
    // stopped is safe: MSPL sections always run with heap access held, so no
    // parked mutator can own it.
    MutatorSlowPathLocker mutatorSlowPathLocker(*this);

    MonotonicTime before { };
    if (Options::logGC()) [[unlikely]] {
        dataLog("Full sweep: ", capacity() / 1024, "kb ");
        before = MonotonicTime::now();
    }
    m_objectSpace.sweepBlocks();
    // SharedGC (MC-SAFE S4 / SPEC-heap §11): physical block reclamation must
    // be world-stopped (or epoch-quarantined). When this sweep runs
    // mutator-concurrently (collectNow(Sync)'s tail, MSPL held, world
    // running), sibling mutators are still executing lock-free heap reads:
    // the concurrent string/IC protocols license transient stale reads of
    // dead-verdicted cells (e.g. JSString::fiberConcurrently), which are
    // benign only while the memory stays heap-owned. MSPL serializes
    // allocation slow paths and the registry, not those reads, so freeing
    // "empty" blocks here is a use-after-free window
    // (mc-safe-gcwait-vs-classa-stop, ASAN). Defer the shrink to the next
    // world-stopped sweep (runCollectionEpilogue()'s shouldSweepSynchronously()
    // path, critical-memory/mini-mode, or teardown) — the same
    // world-stopped-only rule precise allocations already enforce (I5/I16 at
    // MarkedSpace::sweepPreciseAllocations: mutator-concurrent sweeping is a
    // deviation-4 disabled feature), extended to the block shrink leg. Empty
    // blocks stay on the directories' empty lists and remain reusable by
    // every client's allocator — only the OS-return is deferred, not reuse,
    // so this cannot starve allocation.
    if (!isSharedServer() || worldIsStoppedForAllClients())
        m_objectSpace.shrink();
#if ENABLE(WEBASSEMBLY)
    Wasm::TypeInformation::cleanupIfRequested();
#endif
    if (Options::logGC()) [[unlikely]] {
        MonotonicTime after = MonotonicTime::now();
        dataLog("=> ", capacity() / 1024, "kb, ", (after - before).milliseconds(), "ms");
    }
}

void Heap::collect(Synchronousness synchronousness, GCRequest request)
{
    if (!Options::useGC()) [[unlikely]]
        return;

    switch (synchronousness) {
    case Async: {
        collectAsync(request);
        return;
    }
    case Sync:
        collectSync(request);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Heap::collectNow(Synchronousness synchronousness, GCRequest request)
{
    if (!Options::useGC()) [[unlikely]]
        return;

    // SharedGC (T9): conductor-context OK — verifyCanGC()/DeferGCForAWhile
    // below read main-VM/server state that is valid from any access-holding
    // requester (deviation 3: vm() is plain arithmetic to the main VM);
    // DeferGC depth itself routes per-client once ISS (§5.4/I17, T3).
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    switch (synchronousness) {
    case Async: {
        collectAsync(request);
        stopIfNecessary();
        return;
    }
        
    case Sync: {
        collectSync(request);
        
        DeferGCForAWhile deferGC(vm());
        if (Options::useImmortalObjects()) [[unlikely]]
            sweeper().stopSweeping();
        
        bool alreadySweptInCollectSync = shouldSweepSynchronously();
        if (!alreadySweptInCollectSync) {
            dataLogIf(Options::logGC(), "[GC<", RawPointer(this), ">: ");
            sweepSynchronously();
            dataLogIf(Options::logGC(), "]\n");
        }
        // SharedGC: "no unswept after my sync sweep" is not an invariant when
        // this heap serves multiple clients. collectNow(Sync)'s sweep here runs
        // mutator-concurrently under MSPL with the world running, and
        // BlockDirectory::sweep's weak-bearing carve-out deliberately skips
        // blocks whose WeakSet has WeakBlocks in that mode — they stay unswept
        // until the next world-stopped sweep (lazy in-lock sweeping plus the
        // T4(d) MSPL'd IncrementalSweeper — which skips the same weak-bearing
        // blocks — is the shared-mode steady state). So legitimately-unswept
        // blocks remain after our own sweep, with no concurrent collection
        // needed. Only assert when this heap serves a single client.
        if (!isSharedServer())
            m_objectSpace.assertNoUnswept();
        
        sweepAllLogicallyEmptyWeakBlocks();
        return;
    } }
    RELEASE_ASSERT_NOT_REACHED();
}

void Heap::collectAsync(GCRequest request)
{
    if (!Options::useGC()) [[unlikely]]
        return;

    // SharedGC (T9): conductor-context OK — see collectNow().
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    if (!m_isSafeToCollect)
        return;

    // SharedGC (I15, T5): once shared, every async trigger re-routes to the
    // §10B.1 ticketing — no fire-and-forget collections (§10.1). The ticket
    // is served by the next conductor: a sync requester's §10.2 election or
    // a mutator's stopIfNecessaryForAllClients() poll. Shared requesters are
    // not serialized by the API lock (every client's allocation slow path
    // gets here once the server-wide limit is crossed), so the subsumption
    // scan and the grant share one lock hold; otherwise each racing
    // requester sees an empty queue and enqueues its own redundant cycle.
    if (isSharedServer()) [[unlikely]] {
        Locker locker { *m_threadLock };
        for (const GCRequest& previousRequest : m_requests) {
            if (request.subsumedBy(previousRequest))
                return;
        }
        requestCollectionShared(locker, request);
        return;
    }

    bool alreadyRequested = false;
    {
        Locker locker { *m_threadLock };
        for (const GCRequest& previousRequest : m_requests) {
            if (request.subsumedBy(previousRequest)) {
                alreadyRequested = true;
                break;
            }
        }
    }
    if (alreadyRequested)
        return;

    requestCollection(request);
}

void Heap::collectSync(GCRequest request)
{
    if (!Options::useGC()) [[unlikely]]
        return;

    // SharedGC (T9): conductor-context OK — see collectNow().
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    if (!m_isSafeToCollect)
        return;

    // SharedGC (I15, T5): once shared, sync triggers run the §10.2 election —
    // ticket, then conduct (or follow) until the ticket is served. Sync
    // callers never wait on a ticket while holding access without
    // electioneering, and never need notifyVMStop (§10.2).
    if (isSharedServer()) [[unlikely]] {
        Ticket ticket;
        bool holdsPreventGate;
        {
            Locker locker { *m_threadLock };
            ticket = requestCollectionShared(locker, request);
            holdsPreventGate = m_sharedGCPreventCount && m_sharedGCPreventHolder == &Thread::currentSingleton();
            if (holdsPreventGate)
                m_sharedGCPreventHolderTicket = ticket - 1;
        }
        if (!holdsPreventGate) {
            runSharedGCElection(ticket);
            return;
        }

        // The prevent-scope holder gets exactly one cycle, its own, as in legacy
        // mode: a heap analyzer installed for this call must see only that cycle,
        // and no cycle may run after it until allowCollection(). Tickets granted
        // before this one are served first, with the analyzer uninstalled.
        HeapProfiler* heapProfiler = vm().heapProfiler();
        HeapAnalyzer* analyzer = heapProfiler ? heapProfiler->activeHeapAnalyzer() : nullptr;
        if (analyzer)
            heapProfiler->setActiveHeapAnalyzer(nullptr);
        runSharedGCElection(ticket - 1);
        if (analyzer)
            heapProfiler->setActiveHeapAnalyzer(analyzer);
        {
            Locker locker { *m_threadLock };
            m_sharedGCPreventHolderTicket = ticket;
        }
        runSharedGCElection(ticket);
        return;
    }

    waitForCollection(requestCollection(request));
}

bool Heap::shouldCollectInCollectorThread(const AbstractLocker&)
{
    RELEASE_ASSERT(m_requests.isEmpty() == (m_lastServedTicket == m_lastGrantedTicket));
    RELEASE_ASSERT(m_lastServedTicket <= m_lastGrantedTicket);

    // SharedGC (§10B.3/I15, T5b): the collector thread is quiesced once
    // shared — every shared ticket is granted with the conn bit set
    // (requestCollectionShared), so the check below stays false; make the
    // quiescence explicit so a spurious wakeup can never start a legacy
    // collector-side collection mid-shared-mode.
    if (isSharedServer()) [[unlikely]] {
        ASSERT(m_requests.isEmpty() || (m_worldState.load() & mutatorHasConnBit));
        return false;
    }
    dataLogLnIf(HeapInternal::verbose, "Mutator has the conn = ", !!(m_worldState.load() & mutatorHasConnBit));
    
    return !m_requests.isEmpty() && !(m_worldState.load() & mutatorHasConnBit);
}

void Heap::collectInCollectorThread()
{
    for (;;) {
        RunCurrentPhaseResult result = runCurrentPhase(GCConductor::Collector, nullptr);
        switch (result) {
        case RunCurrentPhaseResult::Finished:
            return;
        case RunCurrentPhaseResult::Continue:
            break;
        case RunCurrentPhaseResult::NeedCurrentThreadState:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
    }
}

ALWAYS_INLINE int asInt(CollectorPhase phase)
{
    return static_cast<int>(phase);
}

void Heap::checkConn(GCConductor conn)
{
    unsigned worldState = m_worldState.load();
    switch (conn) {
    case GCConductor::Mutator:
        // SharedGC (§10B.2/§10B.5, T5b): once shared, the conn is always
        // Mutator (the §10.2 election winner conducts as the mutator, while
        // the world is stopped for all clients); the legacy world-state bits
        // are main-client-only and superseded by WSAC.
        // SharedGC (T9): the vm() uses in both asserts are diagnostic-only
        // crash-payload arguments (main VM identifier) — conductor-context
        // OK, no calling-thread assumption.
        RELEASE_ASSERT((worldState & mutatorHasConnBit) || worldIsStoppedForAllClients(), worldState, asInt(m_lastPhase), asInt(m_currentPhase), asInt(m_nextPhase), vm().identifier().toUInt64(), vm().isEntered());
        return;
    case GCConductor::Collector:
        RELEASE_ASSERT(!isSharedServer()); // SharedGC (§10B.3, T5b): collector thread quiesced once shared (I15).
        RELEASE_ASSERT(!(worldState & mutatorHasConnBit), worldState, asInt(m_lastPhase), asInt(m_currentPhase), asInt(m_nextPhase), vm().identifier().toUInt64(), vm().isEntered());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto Heap::runCurrentPhase(GCConductor conn, CurrentThreadState* currentThreadState) -> RunCurrentPhaseResult
{
    checkConn(conn);
    m_currentThreadState = currentThreadState;
    m_currentThread = &Thread::currentSingleton();

    // SharedGC (T9): conductor-context OK (incl. VM-less) —
    // sanitizeStackForVM() self-guards: it returns immediately unless the
    // CALLING thread holds the main VM's API lock, so a §10.2 conductor that
    // is not the main VM's mutator (or is standalone) makes this a no-op.
    if (conn == GCConductor::Mutator)
        sanitizeStackForVM(vm());
    
    // If the collector transfers the conn to the mutator, it leaves us in between phases.
    if (!finishChangingPhase(conn)) {
        // A mischevious mutator could repeatedly relinquish the conn back to us. We try to avoid doing
        // this, but it's probably not the end of the world if it did happen.
        dataLogLnIf(HeapInternal::verbose, "Conn bounce-back.");
        return RunCurrentPhaseResult::Finished;
    }
    
    bool result = false;
    switch (m_currentPhase) {
    case CollectorPhase::NotRunning:
        result = runNotRunningPhase(conn);
        break;
        
    case CollectorPhase::Begin:
        result = runBeginPhase(conn);
        break;
        
    case CollectorPhase::Fixpoint:
        if (!currentThreadState && conn == GCConductor::Mutator)
            return RunCurrentPhaseResult::NeedCurrentThreadState;
        
        result = runFixpointPhase(conn);
        break;
        
    case CollectorPhase::Concurrent:
        result = runConcurrentPhase(conn);
        break;
        
    case CollectorPhase::Reloop:
        result = runReloopPhase(conn);
        break;
        
    case CollectorPhase::End:
        result = runEndPhase(conn);
        break;
    }

    return result ? RunCurrentPhaseResult::Continue : RunCurrentPhaseResult::Finished;
}

NEVER_INLINE bool Heap::runNotRunningPhase(GCConductor conn)
{
    // Check m_requests since the mutator calls this to poll what's going on.
    {
        Locker locker { *m_threadLock };
        if (m_requests.isEmpty())
            return false;
        if (sharedGCPreventGateBlocksNextTicket(locker)) [[unlikely]]
            return false;
    }
    
    return changePhase(conn, CollectorPhase::Begin);
}

NEVER_INLINE bool Heap::runBeginPhase(GCConductor conn)
{
    m_currentGCStartTime = MonotonicTime::now();
    
    {
        Locker locker { *m_threadLock };
        RELEASE_ASSERT(!m_requests.isEmpty());
        m_currentRequest = m_requests.first();
    }

    dataLogIf(Options::logGC(), "[GC<", RawPointer(this), ">: START ", gcConductorShortName(conn), " ", capacity() / 1024, "kb ");

    m_beforeGC = MonotonicTime::now();

    // SharedGC (T9): conductor-context OK — per-collection reseed of the one
    // main VM's RNG; runs at collection begin with the request lock dropped,
    // mutators stopped (WSAC) once shared.
    if (!Options::seedOfVMRandomForFuzzer())
        vm().random().setSeed(cryptographicallyRandomNumber<uint32_t>());

    if (m_collectionScope) {
        dataLogLn("Collection scope already set during GC: ", *m_collectionScope);
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    willStartCollection();
        
    if (m_verifier) [[unlikely]] {
        // Verify that live objects from the last GC cycle haven't been corrupted by
        // mutators before we begin this new GC cycle.
        m_verifier->verify(HeapVerifier::Phase::BeforeGC);
            
        m_verifier->startGC();
        m_verifier->gatherLiveCells(HeapVerifier::Phase::BeforeMarking);
    }

    ASSERT(m_collectionScope);
    bool isFullGC = m_collectionScope.value() == CollectionScope::Full;
    if (Options::useGCSignpost()) [[unlikely]] {
        StringPrintStream stream;
        stream.print("GC:(", RawPointer(this), "),mode:(", (isFullGC ? "Full" : "Eden"), "),version:(", m_gcVersion, "),conn:(", gcConductorShortName(conn), "),capacity(", capacity() / 1024, "kb)");
        m_signpostMessage = stream.toCString();
        WTFBeginSignpost(this, JSCGarbageCollector, "%" PUBLIC_LOG_STRING, m_signpostMessage.data() ? m_signpostMessage.data() : "(nullptr)");
    }

    prepareForMarking();
        
    if (isFullGC) {
        m_opaqueRoots.clear();
        m_collectorSlotVisitor->clearMarkStacks();
        if (Options::useSharedGCHeap()) [[unlikely]] {
            Locker locker { m_serverMutatorMarkStackLock };
            m_mutatorMarkStack->clear();
        } else
            m_mutatorMarkStack->clear();
    } else
        m_bytesAllocatedBeforeLastEdenCollect = totalBytesAllocatedThisCycle();

    RELEASE_ASSERT(m_raceMarkStack->isEmpty());

    beginMarking();

#if ENABLE(WEBASSEMBLY)
    prepareWasmCalleeCleanup();
#endif

    // T1-gc-siblings-mark: like the heapHelperPool visitors, every sibling
    // visitor this cycle can use exists BEFORE the didStartMarking walk and
    // before any MarkingConstraintSolver snapshots the visitor set — a visitor
    // created mid-round would mark cells that round's convergence test never
    // counts. One visitor per admissible sibling (the admission cap, bounded
    // by the clients other than the conductor; the registry is frozen inside
    // this stop window); the pool persists across cycles. No sibling is in
    // the assist here (runEndPhase drained them), so this thread is the only
    // one touching the pool, and the sticky bit is set before the pool is
    // first populated so forEachSlotVisitor never skips a non-empty pool.
    if (VM::isGILOffProcess()) [[unlikely]] {
        if (unsigned cap = Options::sharedGCMaxSiblingMarkingAssists()) {
            unsigned clients = clientSet().size();
            unsigned wanted = std::min(cap, clients ? clients - 1 : 0u);
            m_siblingSlotVisitorPoolMayGrow.store(true, std::memory_order_relaxed);
            Locker locker { m_parallelSlotVisitorLock };
            while (m_siblingSlotVisitors.size() < wanted) {
                auto visitor = makeUnique<SlotVisitor>(*this, toCString("S", m_siblingSlotVisitors.size() + 1));
                if (Options::optimizeParallelSlotVisitorsForStoppedMutator())
                    visitor->optimizeForStoppedMutator();
                m_availableSiblingSlotVisitors.append(visitor.get());
                m_siblingSlotVisitors.append(WTF::move(visitor));
            }
        }
    }

    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            visitor.didStartMarking();
        });

    m_parallelMarkersShouldExit = false;

    // T1-gc-siblings-mark: open the sibling-assist gate. gilOff-only (the
    // sole caller is inside the [[unlikely]] vm.gilOff() notifyVMStop
    // branch); flag-off this block is dead and the landed sequence is
    // byte-identical. Ordering: AFTER forEachSlotVisitor(didStartMarking)
    // and m_parallelMarkersShouldExit=false above — the m_markingMutex
    // release here is the happens-before edge that publishes both to a
    // sibling's enabled-check acquire. Siblings parked on the bounded
    // 1ms poll are NOT woken here (m_worldConditionVariable is a different
    // condvar) — they discover the open gate at their next poll re-fire;
    // worst case one quantum of lost assist, which is the same granularity
    // the fallback already had.
    if (VM::isGILOffProcess()) [[unlikely]] {
        // §7.1a single-handoff: per-CYCLE cap reset, conductor-thread-local
        // (this thread runs the whole cycle as a §3.7 closed loop; CG-I19).
        // Reset BEFORE the m_markingMutex acquire so the value is published
        // by the same release edge that publishes assistEnabled; consumed
        // only by this same thread in runFixpointPhase below.
        t_sharedGCConcurrentHandoffsThisCycle = 0;
        Locker locker { m_markingMutex };
        m_siblingMarkingAssistEnabled = true;
    }

    m_helperClient.setFunction(
        [this] () {
            SlotVisitor* visitor;
            {
                Locker locker { m_parallelSlotVisitorLock };
                RELEASE_ASSERT_WITH_MESSAGE(!m_availableParallelSlotVisitors.isEmpty(), "Parallel SlotVisitors are allocated apriori");
                visitor = m_availableParallelSlotVisitors.takeLast();
            }

            Thread::registerGCThread(GCThreadType::Helper);

            {
                ParallelModeEnabler parallelModeEnabler(*visitor);
                visitor->drainFromShared(SlotVisitor::HelperDrain);
            }

            {
                Locker locker { m_parallelSlotVisitorLock };
                m_availableParallelSlotVisitors.append(visitor);
            }
        });

    SlotVisitor& visitor = *m_collectorSlotVisitor;

    m_constraintSet->didStartMarking();
    
    m_scheduler->beginCollection();
    if (Options::logGC()) [[unlikely]]
        m_scheduler->log();
    
    // After this, we will almost certainly fall through all of the "visitor.isEmpty()"
    // checks because bootstrap would have put things into the visitor. So, we should fall
    // through to draining.
    
    if (!visitor.didReachTermination()) {
        dataLog("Fatal: SlotVisitor should think that GC should terminate before constraint solving, but it does not think this.\n");
        dataLog("visitor.isEmpty(): ", visitor.isEmpty(), "\n");
        dataLog("visitor.collectorMarkStack().isEmpty(): ", visitor.collectorMarkStack().isEmpty(), "\n");
        dataLog("visitor.mutatorMarkStack().isEmpty(): ", visitor.mutatorMarkStack().isEmpty(), "\n");
        dataLog("m_numberOfActiveParallelMarkers: ", m_numberOfActiveParallelMarkers, "\n");
        dataLog("m_sharedCollectorMarkStack->isEmpty(): ", m_sharedCollectorMarkStack->isEmpty(), "\n");
        dataLog("m_sharedMutatorMarkStack->isEmpty(): ", m_sharedMutatorMarkStack->isEmpty(), "\n");
        dataLog("visitor.didReachTermination(): ", visitor.didReachTermination(), "\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
        
    return changePhase(conn, CollectorPhase::Fixpoint);
}

// Whether a shared-heap fixpoint may hand the world back to the mutators now.
bool Heap::sharedFixpointMayResume() const
{
    ASSERT(isSharedServer());
    if (Options::useConcurrentSharedGCMarking())
        return Options::numberOfGCMarkers() >= 2;
    if (VM::isGILOffProcess())
        return Options::numberOfGCMarkers() >= 2 && !t_sharedGCConcurrentHandoffsThisCycle;
    return false;
}

NEVER_INLINE bool Heap::runFixpointPhase(GCConductor conn)
{
    RELEASE_ASSERT(conn == GCConductor::Collector || m_currentThreadState);
    
    SlotVisitor& visitor = *m_collectorSlotVisitor;
    
    if (Options::logGC()) [[unlikely]] {
        UncheckedKeyHashMap<const char*, size_t> visitMap;
        forEachSlotVisitor(
            [&] (SlotVisitor& visitor) {
                visitMap.add(visitor.codeName(), visitor.bytesVisited() / 1024);
            });
        
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        auto perVisitorDump = sortedMapDump(
            visitMap,
            [] (const char* a, const char* b) -> bool {
                return strcmp(a, b) < 0;
            },
            ":"_s, " "_s);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        
        dataLog("v=", bytesVisited() / 1024, "kb (", perVisitorDump, ") o=", m_opaqueRoots.size(), " b=", WTF::atomicLoad(&m_barriersExecuted, std::memory_order_relaxed), " ");
    }
        
    if (visitor.didReachTermination()) {
        m_opaqueRoots.deleteOldTables();
        
        m_scheduler->didReachTermination();
        
        assertMarkStacksEmpty();
            
        // FIXME: Take m_mutatorDidRun into account when scheduling constraints. Most likely,
        // we don't have to execute root constraints again unless the mutator did run. At a
        // minimum, we could use this for work estimates - but it's probably more than just an
        // estimate.
        // https://bugs.webkit.org/show_bug.cgi?id=166828
            
        // Wondering what this does? Look at Heap::addCoreConstraints(). The DOM and others can also
        // add their own using Heap::addMarkingConstraint().
        bool converged = m_constraintSet->executeConvergence(visitor);
        
        // FIXME: The visitor.isEmpty() check is most likely not needed.
        // https://bugs.webkit.org/show_bug.cgi?id=180310
        if (converged && visitor.isEmpty()) {
            assertMarkStacksEmpty();
            return changePhase(conn, CollectorPhase::End);
        }
            
        m_scheduler->didExecuteConstraints();
    }
        
    dataLogIf(Options::logGC(), visitor.collectorMarkStack().size(), "+", m_mutatorMarkStack->size() + visitor.mutatorMarkStack().size(), " ");
        
    // The scheduler's deadline is when the mutator should resume. In the shared
    // heap the fixpoint often does not resume (see below). It then loops back
    // here with the same deadline, and a deadline that has already passed (a
    // zero pause budget) drains nothing on every pass. So drain to the end.
    MonotonicTime drainDeadline = m_scheduler->timeToResume();
    if (isSharedServer() && !sharedFixpointMayResume()) [[unlikely]]
        drainDeadline = MonotonicTime::infinity();
    {
        ParallelModeEnabler enabler(visitor);
        visitor.drainInParallel(drainDeadline);
    }
        
    m_scheduler->synchronousDrainingDidStall();

    // This is kinda tricky. The termination check looks at:
    //
    // - Whether the marking threads are active. If they are not, this means that the marking threads'
    //   SlotVisitors are empty.
    // - Whether the collector's slot visitor is empty.
    // - Whether the shared mark stacks are empty.
    //
    // This doesn't have to check the mutator SlotVisitor because that one becomes empty after every GC
    // work increment, so it must be empty now.
    if (visitor.didReachTermination())
        return true; // This is like relooping to the top of runFixpointPhase().
        
    if (!m_scheduler->shouldResume())
        return true;

    // SharedGC (deviation 4, T5) — RETIRED BY STAGE C1 (SPEC-congc §7.1,
    // CG-3a) and the §7.1a gilOff single-handoff: with both off, no
    // concurrent-marking window once shared — the conducted collection is
    // fully synchronous, so never resume the world into
    // CollectorPhase::Concurrent; keep draining at the fixpoint until
    // termination (the world stays suspended Begin..End; §3.6 degenerate,
    // CG-I0). With the C1 stage flag on, the conduct tenure may schedule
    // Concurrent — but ONLY when numberOfGCMarkers() >= 2 (§3.7/ANNEX
    // CGD1.3 zero-helper rule: the conductor's between-window wait is
    // passive; with no helpers nobody would drain and waitForTermination
    // would park until the scheduler timeout every window for no progress).
    //
    // §7.1a gilOff single-handoff arm (gc-sharedheap-zero-concurrent-overlap-
    // now-11pct; SCALEBENCH §35 round-2 / §27.S2 re-evaluation): under
    // gilOff WITHOUT the C1 stage flag, schedule AT MOST ONE Concurrent
    // window per cycle — the i#1 root pass (Cs/Msr/Wlr/Msm) + the bulk
    // drainInParallel above have already executed in-window; the one
    // Concurrent handoff lets the W siblings run JS while the heapHelperPool
    // drains (between-window barriers land in the server stack under
    // m_serverMutatorMarkStackLock and are merged at the Reentry window's
    // Msm convergence pass);
    // the post-Reentry fixpoint then re-runs Cs/Wlr (GreyedByExecution,
    // m_phaseVersion-keyed) and converges in-window. This bounds extra
    // rendezvous to exactly #cycles (the §27.S2(b) defect: unbounded
    // scheduler-driven handoffs added ~30 reentries/run at ~9 ms each and
    // net-regressed). C1R stays OFF — F19 keeps the server fence master
    // always-true under gilOff, so addToRememberedSet's fenced re-whiten
    // CAS path covers N-mutator between-window barriers without per-client
    // CMS routing (§5.2/F44: CMS is contention-only, not a soundness gate).
    // Same >=2-marker rule (§3.7). Flag-off: VM::isGILOffProcess() is false,
    // this arm is dead, behavior identical to the §27.S2 default.
    if (isSharedServer()) [[unlikely]] {
        if (!sharedFixpointMayResume())
            return true;
        if (!Options::useConcurrentSharedGCMarking())
            t_sharedGCConcurrentHandoffsThisCycle++;
    }

    m_scheduler->willResume();
        
    if (Options::logGC()) [[unlikely]] {
        double thisPauseMS = (MonotonicTime::now() - m_stopTime).milliseconds();
        dataLog("p=", thisPauseMS, "ms (max ", maxPauseMS(thisPauseMS), ")...]\n");
    }

    // Forgive the mutator for its past failures to keep up.
    // FIXME: Figure out if moving this to different places results in perf changes.
    m_incrementBalance = 0;
        
    return changePhase(conn, CollectorPhase::Concurrent);
}

NEVER_INLINE bool Heap::runConcurrentPhase(GCConductor conn)
{
    // SPEC-congc §7.1 ISS arm (CG-3a; F13/ANNEX CGD1.3 GOVERNS): once shared
    // with stage C1 on, NEITHER legacy arm below may run. The Mutator arm is
    // a poll-and-return-to-JS protocol — the §3.7 conductor is a closed loop
    // and never returns to JS mid-tenure. The Collector arm's
    // drainInParallelPassively keys its passive/active branch on the MAIN
    // client's unrelated access state and its MainDrain park has no §9.1(2)
    // checkpoint — the CGD1.3 UAF/deadlock/wakeup-race walks. Instead the
    // conductor runs the §3.7 between-window wait (donateAll +
    // waitForTermination(timeToStop) — condvar-only, in NEITHER marker
    // counter, never visitChildren); helpers keep running
    // drainFromShared(HelperDrain) between windows exactly as in-window (the
    // runBeginPhase helper tasks span the whole cycle). On wake (helper
    // notifyAll or scheduler timeout) reloop — finishChangingPhase's
    // Concurrent -> Reloop suspend edge is the WND-reopen (§3.1 Reentry).
    if (isSharedServer()) [[unlikely]] {
        // Reachable only via the retired runFixpointPhase kill switch, which
        // requires the C1 flag OR the §7.1a gilOff single-handoff, + >= 2
        // markers (§3.7).
        RELEASE_ASSERT(Options::useConcurrentSharedGCMarking() || VM::isGILOffProcess());
        RELEASE_ASSERT(Options::numberOfGCMarkers() >= 2);
        // §10B.2: the conn is pinned Mutator in stages C0-C1 (the collector
        // thread stays quiesced until C2/CG-4).
        RELEASE_ASSERT(conn == GCConductor::Mutator);
        waitBetweenSharedGCWindows();
        return changePhase(conn, CollectorPhase::Reloop);
    }

    SlotVisitor& visitor = *m_collectorSlotVisitor;

    switch (conn) {
    case GCConductor::Mutator: {
        // When the mutator has the conn, we poll runConcurrentPhase() on every time someone says
        // stopIfNecessary(), so on every allocation slow path. When that happens we poll if it's time
        // to stop and do some work.
        if (visitor.didReachTermination()
            || m_scheduler->shouldStop())
            return changePhase(conn, CollectorPhase::Reloop);
        
        // We could be coming from a collector phase that stuffed our SlotVisitor, so make sure we donate
        // everything. This is super cheap if the SlotVisitor is already empty.
        visitor.donateAll();
        return false;
    }
    case GCConductor::Collector: {
        {
            ParallelModeEnabler enabler(visitor);
            visitor.drainInParallelPassively(m_scheduler->timeToStop());
        }
        return changePhase(conn, CollectorPhase::Reloop);
    } }
    
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

NEVER_INLINE bool Heap::runReloopPhase(GCConductor conn)
{
    dataLogIf(Options::logGC(), "[GC<", RawPointer(this), ">: ", gcConductorShortName(conn), " ");
    
    m_scheduler->didStop();
    
    if (Options::logGC()) [[unlikely]]
        m_scheduler->log();
    
    return changePhase(conn, CollectorPhase::Fixpoint);
}

NEVER_INLINE bool Heap::runEndPhase(GCConductor conn)
{
    m_scheduler->endCollection();
        
    {
        Locker locker { m_markingMutex };
        // T1-gc-siblings-mark: close the gate IN THE SAME critical section
        // that sets shouldExit — a sibling whose enabled-check (under this
        // mutex) sees true is guaranteed shouldExit==false at that instant,
        // and once shouldExit is set no NEW sibling can enter (it sees
        // enabled==false). The notifyAll below wakes any sibling already
        // parked inside drainFromShared(HelperDrain)'s isReady wait; each
        // returns Done, then decrements
        // m_numberOfSiblingMarkingAssists. Flag-off: dead store of false
        // into a field that was never true.
        if (VM::isGILOffProcess()) [[unlikely]]
            m_siblingMarkingAssistEnabled = false;
        m_parallelMarkersShouldExit = true;
        m_markingConditionVariable.notifyAll();
    }
    m_helperClient.finish();
    // T1-gc-siblings-mark: the pool-helper finish() above does NOT cover
    // siblings (they are not heapHelperPool tasks). Wait them out so the
    // active==paused==inDrainFromShared==0 invariant (the ASSERT block below)
    // and endMarking()'s forEachSlotVisitor reset() walk see no in-flight
    // sibling visitor. shouldExit + the notifyAll already issued above
    // guarantee every entered sibling's drainFromShared returns; the
    // decrement-side notifyAll (gilOffSiblingAssistMarking) wakes this
    // wait. Flag-off: the count is identically zero and the gate is dead.
    if (VM::isGILOffProcess()) [[unlikely]] {
        Locker locker { m_markingMutex };
        while (m_numberOfSiblingMarkingAssists)
            m_markingConditionVariable.wait(m_markingMutex);
    }

#if ASSERT_ENABLED
    // After m_helperClient.finish() every helper has returned from
    // drainFromShared and no marker pause can be mid-flight on this heap's
    // markers. m_numberOfWaitingParallelMarkers is deliberately not asserted:
    // it is only the steal-partitioning hint and stays incremented across
    // returns (see Heap.h).
    {
        Locker locker { m_markingMutex };
        ASSERT(!m_numberOfParallelMarkersInDrainFromShared);
        ASSERT(!m_numberOfActiveParallelMarkers);
        ASSERT(!m_pausedParallelMarkers);
    }
#endif

    ASSERT(m_mutatorMarkStack->isEmpty());
    ASSERT(m_raceMarkStack->isEmpty());
#if ASSERT_ENABLED
    // SPEC-congc CGA1 A4 (F37 as amended by F48; CG-2): the relocated
    // "all CMS empty" walk stays at this LANDED site — after
    // m_helperClient.finish(), strictly BEFORE the first conductor-context
    // writeBarrier batch below (whose appends are deliberately NEXT-CYCLE
    // grey: a client-conductor's land in its own CMS, a null-client C2
    // conductor's in the server stack — F31/CGD4.5). F48 ruling: the
    // emptiness claim holds for every NON-CONDUCTOR client only — the final
    // window's §3.1(e) WND-open drain emptied every CMS and WSAC bars
    // suspended-client appends since, but WSAC never barred the conductor's
    // OWN thread, and F43/CGD7.1(a) made the client-conductor a FULL CLIENT
    // whose in-window conduct-path barriers legally append to its own CMS
    // at ANY in-window point (not only the post-site end-phase batches F37
    // reasoned about). Those appends are NEXT-CYCLE grey (drained at the
    // next WND-open per §5.2 drain (i); exit-flushed via
    // flushClientMutatorMarkStackForExit otherwise), so the conductor's own
    // client is EXEMPT here — and ONLY it: any other client's non-empty CMS
    // is still corruption and must fail-stop.
    if (sharedGCBarrierStateIsPerClient()) {
        GCClient::Heap* conductorClient = GCClient::Heap::currentThreadClient();
        clientSet().forEach([&](GCClient::Heap& client) {
            if (&client == conductorClient)
                return; // F48: legal in-window conductor-client appends, next-cycle grey.
            // Bare terminal-leaf acquisition; not a nested LK.9d>LK.9c site
            // (no outer m_markingMutex — debug-only emptiness probe).
            Locker cmsLocker { client.m_mutatorMarkStackLock };
            ASSERT(!client.m_mutatorMarkStack || client.m_mutatorMarkStack->isEmpty());
        });
    }
#endif

    SlotVisitor& visitor = *m_collectorSlotVisitor;
    iterateExecutingAndCompilingCodeBlocks(visitor,
        [&] (CodeBlock* codeBlock) {
            writeBarrier(codeBlock);
        });

    updateObjectCounts();
    endMarking();

#if ENABLE(WEBASSEMBLY)
    finalizeWasmCalleeCleanup();
#endif

    if (Options::verifyGC()) [[unlikely]]
        verifyGC();

    if (m_verifier) [[unlikely]] {
        m_verifier->gatherLiveCells(HeapVerifier::Phase::AfterMarking);
        m_verifier->verify(HeapVerifier::Phase::AfterMarking);
    }
        
    {
        auto* previous = Thread::currentSingleton().setCurrentAtomStringTable(nullptr);
        auto scopeExit = makeScopeExit([&] {
            Thread::currentSingleton().setCurrentAtomStringTable(previous);
        });

        // SharedGC (T9): conductor-context OK — end phase, mutators
        // suspended (worldIsStopped() legacy / WSAC shared); all vm() uses
        // in this block (type profiler, deferred work, array buffers,
        // compiler worklists) touch VM-global state of the one main VM.
        if (vm().typeProfiler())
            vm().typeProfiler()->invalidateTypeSetCache(vm());

        cancelDeferredWorkIfNeeded();
        reapWeakHandles();
        pruneStaleEntriesFromWeakGCHashTables();
        sweepArrayBuffers();
        snapshotUnswept();
        reconcileWeakReferencesAtGCEnd(); // Must precede clearCurrentlyExecuting: CodeBlock::reconcileWeakReferencesAtGCEnd queries which CodeBlocks are currently executing.
        removeDeadCompilerWorklistEntries();
        deleteUnmarkedCompiledCode();
        if (m_collectionScope == CollectionScope::Full)
            releaseUnusedSharedBaselineCode();
    }

    notifyIncrementalSweeper();
    
    m_codeBlocks->iterateCurrentlyExecuting(
        [&] (CodeBlock* codeBlock) {
            writeBarrier(codeBlock);
        });
    // SharedGC (T9): conductor-context OK — end phase, world stopped; vm()
    // = worklist key (see completeAllJITPlans()).
    m_codeBlocks->clearCurrentlyExecutingAndRemoveDeadCodeBlocks(vm());

    m_objectSpace.prepareForAllocation();
    updateAllocationLimits();

    if (m_verifier) [[unlikely]] {
        m_verifier->trimDeadCells();
        m_verifier->verify(HeapVerifier::Phase::AfterGC);
    }

    auto endingCollectionScope = *m_collectionScope;

    // SharedGC (§9 contract notes, T5): runStopTheWorldSafepointHooks() fires
    // once per collection in the legacy protocol (!isSharedServer(),
    // including option-off — the sole option-off behavior delta, I10
    // exemption): fire the hooks + the §11 legacy epoch-reclamation sequence
    // HERE, just before didFinishCollection(), with the mutator suspended.
    // Shared mode runs the equivalent at §10 step 7
    // (conductSharedCollection), once per drained ticket batch — never here
    // (collectInMutatorThread() drains several tickets per call).
    if (!isSharedServer()) {
        ASSERT(worldIsStopped());
        runSafepointHooksAndReclaim();
    }

    didFinishCollection();
    
    if (m_currentRequest.didFinishEndPhase)
        m_currentRequest.didFinishEndPhase->run();
    
    if (HeapInternal::verbose) {
        dataLogLn(HeapInternal::verbose, "Heap state after GC:");
        m_objectSpace.dumpBits();
    }
    
    if (Options::logGC()) [[unlikely]] {
        double thisPauseMS = (m_afterGC - m_stopTime).milliseconds();
        dataLog("p=", thisPauseMS, "ms (max ", maxPauseMS(thisPauseMS), "), cycle ", (m_afterGC - m_beforeGC).milliseconds(), "ms END]\n");
    }
    
    {
        Locker locker { *m_threadLock };
        m_requests.removeFirst();
        m_lastServedTicket++;
        clearMutatorWaiting();
        // SharedGC (§10.2 normative, T5): the serve path also notifies the
        // GC election condition — shared-mode followers wait on it (never on
        // m_threadCondition) and must observe each served ticket.
        m_gcElectionCondition.notifyAll();
    }
    ParkingLot::unparkAll(&m_worldState);

    dataLogLnIf(Options::logGC(), "GC END!");
    if (Options::useGCSignpost()) [[unlikely]] {
        WTFEndSignpost(this, JSCGarbageCollector, "%" PUBLIC_LOG_STRING, m_signpostMessage.data() ? m_signpostMessage.data() : "(nullptr)");
        m_signpostMessage = { };
    }

    setNeedCollectionEpilogue();

    m_lastGCStartTime = m_currentGCStartTime;
    m_lastGCEndTime = MonotonicTime::now();
    m_totalGCTime += m_lastGCEndTime - m_lastGCStartTime;
    if (endingCollectionScope == CollectionScope::Full)
        m_lastFullGCEndTime = m_lastGCEndTime;
    return changePhase(conn, CollectorPhase::NotRunning);
}

bool Heap::changePhase(GCConductor conn, CollectorPhase nextPhase)
{
    checkConn(conn);

    m_lastPhase = m_currentPhase;
    m_nextPhase = nextPhase;

    return finishChangingPhase(conn);
}

NEVER_INLINE bool Heap::finishChangingPhase(GCConductor conn)
{
    checkConn(conn);
    
    if (m_nextPhase == m_currentPhase)
        return true;

    dataLogLnIf(HeapInternal::verbose, conn, ": Going to phase: ", m_nextPhase, " (from ", m_currentPhase, ")");

    m_phaseVersion++;
    
    bool suspendedBefore = worldShouldBeSuspended(m_currentPhase);
    bool suspendedAfter = worldShouldBeSuspended(m_nextPhase);
    
    // SPEC-congc §7.1 (CG-3a): once shared with stage C1 on, the
    // suspend-state periphery pairing below IS the window boundary — the
    // resume edge INTO Concurrent becomes a non-final WND-close (§3.2) and
    // the suspend edge OUT of Concurrent becomes a WND-reopen (§3.1
    // Reentry). The edge predicates key on the Concurrent phase itself, NOT
    // merely on the suspend-state change: the NotRunning -> Begin suspend
    // edge runs INSIDE the already-open first window (F15 — no reopen), and
    // the End -> NotRunning resume edge closes nothing: the conduct loop in
    // conductSharedCollection drains any further ticket inside that same
    // window and performs the one FINAL close after it (no non-final close
    // there). Concurrent is the only phase a conduct tenure
    // passes through with the world resumed, so this is exhaustive.
    // §7.1a: the gilOff single-handoff arm activates the same WND-close /
    // WND-reopen pairing on the Concurrent edge as the C1 stage flag (the
    // ONE handoff per cycle scheduled by runFixpointPhase reaches here via
    // changePhase(Concurrent)); flag-off VM::isGILOffProcess() is false and
    // the predicate is the §27.S2 default (CG-I0).
    bool windowedConcurrentMarking = isSharedServer() && (Options::useConcurrentSharedGCMarking() || VM::isGILOffProcess());
    bool deferredNonFinalWindowClose = false;

    if (suspendedBefore != suspendedAfter) {
        if (suspendedBefore) {
            RELEASE_ASSERT(!suspendedAfter);

            resumeThePeriphery();
            if (conn == GCConductor::Collector)
                resumeTheMutator();
            else
                handleNeedCollectionEpilogue();
            if (windowedConcurrentMarking && m_nextPhase == CollectorPhase::Concurrent) [[unlikely]] {
                // WND-close, deferred below the phase store: F22 PHASE-STORE
                // ORDER (NORMATIVE) — finishChangingPhase's stores complete
                // BEFORE the close's GCL release, so every GCL-ordered
                // reader (§3.4 guards, the §9.1(2) ctor) observes the
                // published phase. resumeThePeriphery() above is the §3.2
                // heap-resume; the close performs the cache resume pass, ISB
                // bump, WSAC clear, GSP clear, GBC broadcast and VMM resume
                // (heap-resume-before-VMM-resume stays normative).
                deferredNonFinalWindowClose = true;
            }
        } else {
            RELEASE_ASSERT(!suspendedBefore);
            RELEASE_ASSERT(suspendedAfter);

            if (windowedConcurrentMarking && m_currentPhase == CollectorPhase::Concurrent) [[unlikely]] {
                // WND-reopen (§3.1 Reentry: F45 foreign-waiter deferral, then
                // the BLOCKING GCL acquire — legal exactly because the
                // conductor is access-released all tenure (§3.7), then GSP /
                // VMM stop / GBL barrier / WSAC). Must precede
                // stopThePeriphery(), whose ISS assert requires WSAC (I5).
                RELEASE_ASSERT(m_sharedGCConductorClient);
                openSharedGCStopWindow(*m_sharedGCConductorClient, SharedGCWindowOpen::Reentry);
            }
            if (conn == GCConductor::Collector) {
                waitWhileNeedCollectionEpilogue();
                if (!stopTheMutator()) {
                    dataLogLnIf(HeapInternal::verbose, "Returning false.");
                    return false;
                }
            } else {
                // SharedGC (T9): conductor-context OK — self-guarded no-op
                // unless the caller is the main VM's API-lock holder (see
                // runCurrentPhase()).
                sanitizeStackForVM(vm());
                handleNeedCollectionEpilogue();
            }
            stopThePeriphery(conn);
        }
    }

    m_currentPhase = m_nextPhase;
    if (deferredNonFinalWindowClose) [[unlikely]]
        closeSharedGCStopWindow(false /* isFinalClose */); // F22: phase store above precedes this close's GCL release (CG-I4).
    return true;
}

void Heap::stopThePeriphery(GCConductor conn)
{
    // SharedGC (§10.5/§10B.5, T5b): once shared this runs only on the §10.2
    // conductor while the world is stopped for all clients. The
    // m_objectSpace.stopAllocating() below iterates the shared
    // BlockDirectories' m_localAllocators lists, which contain EVERY client's
    // LocalAllocators (TLC slots + registered GCClient::IsoSubspace
    // allocators) — this is the §10 step-5 flush (I2 exception). The fence
    // bookkeeping here and in resumeThePeriphery() is conductor-private;
    // mutators are barred by the §10.4 barrier (always-fenced once shared,
    // see setMutatorShouldBeFenced()).
    ASSERT(!isSharedServer() || worldIsStoppedForAllClients()); // I5 (T8).
    // Collector-thread-only self-check; relaxed is sufficient here. The
    // cross-thread edge is the release store below.
    if (WTF::atomicLoad(&m_worldIsStopped, std::memory_order_relaxed)) {
        dataLog("FATAL: world already stopped.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    if (m_mutatorDidRun)
        m_mutatorExecutionVersion++;
    
    m_mutatorDidRun = false;

    m_isCompilerThreadsSuspended = suspendCompilerThreads();
    // Release store pairs with the atomic load in worldIsStopped() (same
    // pattern as m_worldIsStoppedForAllClients).
    WTF::atomicStore(&m_worldIsStopped, true, std::memory_order_release);

    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            visitor.updateMutatorIsStopped(NoLockingNecessary);
        });

    UNUSED_PARAM(conn);
    
    // SharedGC (T9): conductor-context OK — runs world-stopped; the shadow
    // chicken log and topCallFrame are state of the one main VM, quiescent
    // while its mutator is parked (§10.4 barrier). Post-GIL topCallFrame
    // moves to VMLite (per-thread); revisit with the deviation-8 charter.
    if (auto* shadowChicken = vm().shadowChicken())
        shadowChicken->update(vm(), vm().topCallFrame);
    
    m_objectSpace.stopAllocating();

    // Verifier (Options::verifyGC()), O(blocks): the stopAllocating() above
    // flushes every client's LocalAllocators through the shared directories'
    // m_localAllocators lists, so no block may still be freelisted; a
    // freelisted block's handed-out cells would carry no version-current
    // liveness bit through marking.
    if (isSharedServer() && Options::verifyGC()) [[unlikely]] {
        m_objectSpace.forEachBlock(
            [&] (MarkedBlock::Handle* block) {
                if (block->isFreeListed()) [[unlikely]] {
                    dataLogLn(
                        "SharedGC verifier: block ",
                        RawPointer(block), " of directory ", RawPointer(block->directory()),
                        " (cellSize = ", block->directory()->cellSize(),
                        ") is still freelisted after the step-5 stopAllocating() flush.");
                    RELEASE_ASSERT_NOT_REACHED();
                }
            });
    }

    m_stopTime = MonotonicTime::now();
}

NEVER_INLINE void Heap::resumeThePeriphery()
{
    // SharedGC (I5, T8): in shared mode this is conductor-only and runs while
    // the world is STILL stopped for all clients — §10 step 8 (the WSAC clear
    // + VMM resume) strictly follows the conducted cycle, so resuming the
    // directory-linked allocators here cannot race any mutator.
    ASSERT(!isSharedServer() || worldIsStoppedForAllClients());
    // Calling resumeAllocating does the Right Thing depending on whether this is the end of a
    // collection cycle or this is just a concurrent phase within a collection cycle:
    // - At end of collection cycle: it's a no-op because prepareForAllocation already cleared the
    //   last active block.
    // - During collection cycle: it reinstates the last active block.
    m_objectSpace.resumeAllocating();
    
    WTF::atomicStore(&m_barriersExecuted, static_cast<uintptr_t>(0), std::memory_order_relaxed);
    
    if (!WTF::atomicLoad(&m_worldIsStopped, std::memory_order_relaxed)) {
        dataLog("Fatal: collector does not believe that the world is stopped.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    WTF::atomicStore(&m_worldIsStopped, false, std::memory_order_release);
    
    // FIXME: This could be vastly improved: we want to grab the locks in the order in which they
    // become available. We basically want a lockAny() method that will lock whatever lock is available
    // and tell you which one it locked. That would require teaching ParkingLot how to park on multiple
    // queues at once, which is totally achievable - it would just require memory allocation, which is
    // suboptimal but not a disaster. Alternatively, we could replace the SlotVisitor rightToRun lock
    // with a DLG-style handshake mechanism, but that seems not as general.
    Vector<SlotVisitor*, 8> visitorsToUpdate;

    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            visitorsToUpdate.append(&visitor);
        });
    
    for (unsigned countdown = 40; !visitorsToUpdate.isEmpty() && countdown--;) {
        for (unsigned index = 0; index < visitorsToUpdate.size(); ++index) {
            SlotVisitor& visitor = *visitorsToUpdate[index];
            bool remove = false;
            if (visitor.hasAcknowledgedThatTheMutatorIsResumed())
                remove = true;
            else if (visitor.rightToRun().tryLock()) {
                Locker locker { AdoptLock, visitor.rightToRun() };
                visitor.updateMutatorIsStopped(locker);
                remove = true;
            }
            if (remove) {
                visitorsToUpdate[index--] = visitorsToUpdate.last();
                visitorsToUpdate.takeLast();
            }
        }
        Thread::yield();
    }
    
    for (SlotVisitor* visitor : visitorsToUpdate)
        visitor->updateMutatorIsStopped();

    if (std::exchange(m_isCompilerThreadsSuspended, false))
        resumeCompilerThreads();
}

bool Heap::stopTheMutator()
{
    // SharedGC (§10B.3, T5b): unreachable once shared — only the collector
    // thread's conn path reaches here, and that thread is quiesced (I15).
    RELEASE_ASSERT(!isSharedServer());
    for (;;) {
        unsigned oldState = m_worldState.load();
        if (oldState & stoppedBit) {
            RELEASE_ASSERT(!(oldState & hasAccessBit));
            RELEASE_ASSERT(!(oldState & mutatorWaitingBit));
            RELEASE_ASSERT(!(oldState & mutatorHasConnBit));
            return true;
        }
        
        if (oldState & mutatorHasConnBit) {
            RELEASE_ASSERT(!(oldState & hasAccessBit));
            RELEASE_ASSERT(!(oldState & stoppedBit));
            return false;
        }

        if (!(oldState & hasAccessBit)) {
            RELEASE_ASSERT(!(oldState & mutatorHasConnBit));
            RELEASE_ASSERT(!(oldState & mutatorWaitingBit));
            // We can stop the world instantly.
            if (m_worldState.compareExchangeWeak(oldState, oldState | stoppedBit))
                return true;
            continue;
        }
        
        // Transfer the conn to the mutator and bail.
        RELEASE_ASSERT(oldState & hasAccessBit);
        RELEASE_ASSERT(!(oldState & stoppedBit));
        unsigned newState = (oldState | mutatorHasConnBit) & ~mutatorWaitingBit;
        if (m_worldState.compareExchangeWeak(oldState, newState)) {
            dataLogLnIf(HeapInternal::verbose, "Handed off the conn.");
            m_stopIfNecessaryTimer->scheduleSoon();
            ParkingLot::unparkAll(&m_worldState);
            return false;
        }
    }
}

NEVER_INLINE void Heap::resumeTheMutator()
{
    // SharedGC (§10B.3, T5b): unreachable once shared (see stopTheMutator()).
    RELEASE_ASSERT(!isSharedServer());
    dataLogLnIf(HeapInternal::verbose, "Resuming the mutator.");
    for (;;) {
        unsigned oldState = m_worldState.load();
        if (!!(oldState & hasAccessBit) != !(oldState & stoppedBit)) {
            dataLog("Fatal: hasAccess = ", !!(oldState & hasAccessBit), ", stopped = ", !!(oldState & stoppedBit), "\n");
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (oldState & mutatorHasConnBit) {
            dataLog("Fatal: mutator has the conn.\n");
            RELEASE_ASSERT_NOT_REACHED();
        }
        
        if (!(oldState & stoppedBit)) {
            dataLogLnIf(HeapInternal::verbose, "Returning because not stopped.");
            return;
        }
        
        if (m_worldState.compareExchangeWeak(oldState, oldState & ~stoppedBit)) {
            dataLogLnIf(HeapInternal::verbose, "CASing and returning.");
            ParkingLot::unparkAll(&m_worldState);
            return;
        }
    }
}

void Heap::stopIfNecessarySlow()
{
    // SharedGC (T9): main-VM-only — legacy m_worldState stop path; once ISS,
    // stopIfNecessary() re-routes to stopIfNecessaryForAllClients() before
    // reaching here (I15), so the caller is the main VM's mutator.
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    while (stopIfNecessarySlow(m_worldState.load())) { }
    
    // RELEASE_ASSERT(m_worldState.load() & hasAccessBit);
    // RELEASE_ASSERT(!(m_worldState.load() & stoppedBit));
    
    handleNeedCollectionEpilogue();
    m_mutatorDidRun = true;
}

bool Heap::stopIfNecessarySlow(unsigned oldState)
{
    // SharedGC (T9): main-VM-only — see stopIfNecessarySlow() above.
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    // RELEASE_ASSERT(oldState & hasAccessBit);
    // RELEASE_ASSERT(!(oldState & stoppedBit));
    
    // It's possible for us to wake up with the epilogue already requested but the world not yet
    // resumed. If that happens, we can't run the epilogue yet.
    if (handleNeedCollectionEpilogue(oldState))
        return true;

    // FIXME: When entering the concurrent phase, we could arrange for this branch not to fire, and then
    // have the SlotVisitor do things to the m_worldState to make this branch fire again. That would
    // prevent us from polling this so much. Ideally, stopIfNecessary would ignore the mutatorHasConnBit
    // and there would be some other bit indicating whether we were in some GC phase other than the
    // NotRunning or Concurrent ones.
    if (oldState & mutatorHasConnBit)
        collectInMutatorThread();
    
    return false;
}

NEVER_INLINE void Heap::collectInMutatorThread()
{
    CollectingScope collectingScope(*this);
    for (;;) {
        RunCurrentPhaseResult result = runCurrentPhase(GCConductor::Mutator, nullptr);
        switch (result) {
        case RunCurrentPhaseResult::Finished:
            return;
        case RunCurrentPhaseResult::Continue:
            break;
        case RunCurrentPhaseResult::NeedCurrentThreadState:
            // SharedGC (T9): conductor-context OK — self-guarded (see
            // runCurrentPhase()); the §10.2 conductor reaches here through
            // conductSharedCollection()'s phase loop.
            sanitizeStackForVM(vm());
            auto lambda = [&] (CurrentThreadState& state) {
                for (;;) {
                    RunCurrentPhaseResult result = runCurrentPhase(GCConductor::Mutator, &state);
                    switch (result) {
                    case RunCurrentPhaseResult::Finished:
                        return;
                    case RunCurrentPhaseResult::Continue:
                        break;
                    case RunCurrentPhaseResult::NeedCurrentThreadState:
                        RELEASE_ASSERT_NOT_REACHED();
                        break;
                    }
                }
            };
            callWithCurrentThreadState(lambda);
            return;
        }
    }
}

template<typename Func>
void Heap::waitForCollector(const Func& func)
{
    for (;;) {
        // SharedGC (C1 amend — corrected disposition of the line-2755 audit
        // row: this function IS reachable once shared, via preventCollection()
        // — PreventCollectionScope users incl. the snapshot builders and
        // deleteAll*CodeBlocks/addMarkingConstraint — and the shutdown drain
        // above). The legacy wait body is wrong on all three legs once
        // shared: (1) stopIfNecessarySlow(oldState) sees the pinned-Mutator
        // conn bit (§10B.2) and would conduct pending shared tickets via
        // collectInMutatorThread() outside the §10.2 election, without GCL
        // and without the world stopped for all clients (I5);
        // (2) relinquishConn() would clear the permanently-Mutator conn bit
        // (§10B rule 1) and notify the quiesced collector thread (§10B.3/I15)
        // — the C1 assert through a different door (now also guarded in
        // relinquishConn itself); (3) setMutatorWaiting()/compareAndPark is
        // the legacy collector-thread handshake (though the serve path's
        // clearMutatorWaiting()+unparkAll runs in both protocols). Instead:
        // poll SINFAC — it cooperates with a pending §10A stop and conducts
        // granted-unserved tickets when this client is eligible, giving the
        // waiter itself liveness — and do §10B.4-style timed waits on the GC
        // election condition (the serve path notifies it on every served
        // ticket). Re-checked every iteration; a mid-wait legacy->shared flip
        // is safe because §10B.4 requires legacy quiescence (served ==
        // granted) before ISS can be set, at which point both call sites'
        // func is true.
        if (isSharedServer()) [[unlikely]] {
            // W16-C1 residual (a) — bounded-diagnostics semantics for the
            // no-conductor case: a shared-mode waiter that is deferred
            // (I17 — SINFAC's conduct arm refuses while isDeferred()) or has
            // no eligible client cannot conduct pending tickets itself; with
            // no other polling mutator this loop spins on <=1ms timed waits
            // until a conductor appears. Per the STW-watchdog policy ruling
            // (deepwater LEDGER: overload-latency, not a wedge), the landed
            // semantics are KEEP-WAITING + RATE-LIMITED DUMP: never abort,
            // never weaken the wait predicate; after 5s emit a diagnostic
            // state dump and repeat it at most every 30s. Liveness is
            // preserved by the SINFAC poll each iteration (stop-cooperative,
            // and it conducts granted-unserved tickets whenever this client
            // IS eligible).
            MonotonicTime issWaitStart = MonotonicTime::now();
            MonotonicTime nextStallDump = issWaitStart + Seconds(5);
            for (;;) {
                {
                    Locker locker { *m_threadLock };
                    if (func(locker))
                        return;
                }
                stopIfNecessaryForAllClients();
                {
                    Locker locker { *m_threadLock };
                    if (func(locker))
                        return;
                    m_gcElectionCondition.waitFor(*m_threadLock, 1_ms);
                    MonotonicTime now = MonotonicTime::now();
                    if (now >= nextStallDump) [[unlikely]] {
                        dumpSharedGCWaitForCollectorStall(now - issWaitStart);
                        nextStallDump = now + Seconds(30);
                    }
                }
            }
        }

        bool done;
        {
            Locker locker { *m_threadLock };
            done = func(locker);
            if (!done) {
                setMutatorWaiting();
                
                // At this point, the collector knows that we intend to wait, and he will clear the
                // waiting bit and then unparkAll when the GC cycle finishes. Clearing the bit
                // prevents us from parking except if there is also stop-the-world. Unparking after
                // clearing means that if the clearing happens after we park, then we will unpark.
            }
        }
        
        // If we're in a stop-the-world scenario, we need to wait for that even if done is true.
        unsigned oldState = m_worldState.load();
        if (stopIfNecessarySlow(oldState))
            continue;
        
        m_mutatorDidRun = true;
        // FIXME: We wouldn't need this if stopIfNecessarySlow() had a mode where it knew to just
        // do the collection.
        relinquishConn();

        if (done) {
            clearMutatorWaiting(); // Clean up just in case.
            return;
        }
        
        // If mutatorWaitingBit is still set then we want to wait.
        ParkingLot::compareAndPark(&m_worldState, oldState | mutatorWaitingBit);
    }
}

NEVER_INLINE void Heap::dumpSharedGCWaitForCollectorStall(Seconds elapsed)
{
    // W16-C1 residual (a): rate-limited stall dump for the shared-mode
    // waitForCollector loop (keep-waiting policy — see the call site).
    // Out-of-line so the template body stays small. Caller holds
    // *m_threadLock, so the ticket/conductor/phase reads are coherent.
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    unsigned deferralDepth = client ? client->m_deferralDepth : 0;
    dataLogLn("JSC SharedGC: waitForCollector has waited ", elapsed.seconds(),
        "s under isSharedServer() without its predicate becoming true. State: granted=", m_lastGrantedTicket,
        " served=", m_lastServedTicket,
        " conductorActive=", m_gcConductorActive,
        " phase=", m_currentPhase,
        " preventCount=", m_sharedGCPreventCount,
        " thisClient=", RawPointer(client),
        " thisClientDeferralDepth=", deferralDepth,
        ". A deferred or ineligible waiter cannot conduct pending tickets itself (I17); it keeps waiting for a conductor "
        "(another mutator's SINFAC poll or a sync requester's election), per the keep-waiting + rate-limited-dump policy. "
        "If this repeats with no progress, no eligible mutator is polling: check for N threads all parked/deferred.");
}

NEVER_INLINE void Heap::dumpSharedGCAccessBarrierStall(Seconds elapsed)
{
    // Rate-limited stall dump for the §10.4 access barrier in
    // openSharedGCStopWindow (keep-waiting policy, same shape as
    // waitForCollector's). Caller holds m_gcBarrierLock; the per-client reads
    // are the barrier's own seq_cst samples.
    StringPrintStream holders;
    clientSet().forEach([&](GCClient::Heap& client) {
        if (client.m_accessState.load(std::memory_order_seq_cst) != GCClient::Heap::hasAccessState)
            return;
        WTF::Thread* owner = client.m_accessOwner.load(std::memory_order_relaxed);
        holders.print(" client=", RawPointer(&client), &client == m_mainClient ? "(main)" : "", " ownerThread=", RawPointer(owner), " ownerUID=", owner ? owner->uid() : 0);
    });
    dataLogLn("JSC SharedGC: the stop-the-world access barrier has waited ", elapsed.seconds(),
        "s for every client to release heap access. Still holding:", holders.toCString().data(),
        ". A GC stop traps only threads executing JS; a client holding access while blocked in native code or idle in an "
        "event loop is reached only by its own releaseHeapAccess()/stopIfNecessary(), so embedders must release heap access "
        "(ReleaseHeapAccessScope) across blocking sections.");
}

void Heap::acquireAccessSlow()
{
    for (;;) {
        // SharedGC (§10B.4 flip handshake; review rounds 1+3): the ISS check
        // lives INSIDE the retry loop. A legacy acquirer lands here for two
        // distinct reasons: (1) its inline CAS (which expects exactly 0)
        // read the §10B.4 poison — then this check forwards; or (2) its
        // inline CAS read some PRE-flip non-zero state (e.g. needCollectionEpilogueBit
        // is legitimately set with no access holder after a collector-thread
        // cycle) — then it carries NO synchronizes-with edge to a concurrent
        // flip, this read may be stale-false, and a poison observed on a
        // LATER iteration must re-resolve ISS via the hasAccessBit branch
        // below instead of tripping the legacy double-acquire crash (review
        // round 3; the previous shape checked ISS once before the loop and
        // RELEASE_ASSERTed unconditionally on hasAccessBit).
        if (isSharedServer()) [[unlikely]] {
            acquireAccessForwardedToMainClient();
            return;
        }

        unsigned oldState = m_worldState.load();
        if (oldState & hasAccessBit) [[unlikely]] {
            // Either the §10B.4 poison, or a genuine double-acquire. The
            // flip installs the poison and stores ISS inside ONE
            // *m_threadLock critical section with no waits in between
            // (noteSharedServerSticky), so locking it and re-reading ISS
            // decides: if a flip published this bit, its critical section
            // completes before we get the lock (ISS true -> loop back to
            // the leading check, which is now coherence-bound true on this
            // thread, and forward); if ISS is still false under the lock,
            // the bit belongs to a real legacy access holder and this is
            // the same double-acquire bug the legacy protocol has always
            // crashed on (also the post-§10D-reversion behavior: the
            // reversion-era pinned bit denotes the main mutator's real
            // access, and a second acquirer is just as much a bug).
            bool sharedNow;
            {
                Locker locker { *m_threadLock };
                sharedNow = m_isSharedServer.load(std::memory_order_seq_cst);
            }
            if (sharedNow)
                continue;
            dataLog("FATAL: Attempting to acquire access but another thread holds it (no ISS flip in flight).\n");
            RELEASE_ASSERT_NOT_REACHED();
        }

        if (oldState & stoppedBit) {
            if (HeapInternal::verboseStop) {
                dataLogLn("Stopping in acquireAccess!");
                WTFReportBacktrace();
            }
            // Wait until we're not stopped anymore.
            ParkingLot::compareAndPark(&m_worldState, oldState);
            continue;
        }
        
        RELEASE_ASSERT(!(oldState & stoppedBit));
        unsigned newState = oldState | hasAccessBit;
        if (m_worldState.compareExchangeWeak(oldState, newState)) {
            handleNeedCollectionEpilogue();
            m_mutatorDidRun = true;
            stopIfNecessary();
            return;
        }
    }
}

void Heap::releaseAccessSlow()
{
    for (;;) {
        // SharedGC (§10B.4 flip handshake; review rounds 1+3): mirror of
        // acquireAccessSlow()'s IN-LOOP re-check, as a backstop. Pre-flip
        // access holders are API-lock-ordered after the flip (quiescence
        // clause (a)) and post-flip acquirers were forwarded (their inline
        // ISS check is coherent-true, so they never reach this slow path) —
        // but if a release does land here once shared, or a flip completes
        // between iterations of this loop, forward it rather than letting
        // the CAS below clear the superseded legacy bits (which would strip
        // the permanent §10B.4 poison and reopen the stale-acquirer funnel).
        if (isSharedServer()) [[unlikely]] {
            releaseAccessForwardedToMainClient();
            return;
        }

        unsigned oldState = m_worldState.load();
        if (!(oldState & hasAccessBit)) {
            dataLog("FATAL: Attempting to release access but the mutator does not have access.\n");
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (oldState & stoppedBit) {
            dataLog("FATAL: Attempting to release access but the mutator is stopped.\n");
            RELEASE_ASSERT_NOT_REACHED();
        }
        
        if (handleNeedCollectionEpilogue(oldState))
            continue;
        
        unsigned newState = oldState & ~(hasAccessBit | mutatorHasConnBit);
        
        if ((oldState & mutatorHasConnBit)
            && m_nextPhase != m_currentPhase) {
            // This means that the collector thread had given us the conn so that we would do something
            // for it. Stop ourselves as we release access. This ensures that acquireAccess blocks. In
            // the meantime, since we're handing the conn over, the collector will be awoken and it is
            // sure to have work to do.
            newState |= stoppedBit;
        }

        if (m_worldState.compareExchangeWeak(oldState, newState)) {
            if (oldState & mutatorHasConnBit)
                finishRelinquishingConn();
            return;
        }
    }
}

bool Heap::relinquishConn(unsigned oldState)
{
    // SharedGC (§10B.2/§10B.3, C1 amend — corrected disposition of the
    // line-2755 audit row): once shared, the conn is permanently Mutator
    // (rule 1: every shared ticket is granted with mutatorHasConnBit set,
    // requestCollectionShared and the CIND timer's inline grant) and the
    // collector thread is quiesced (I15). Clearing the bit here and letting
    // finishRelinquishingConn() notify m_threadCondition on a pending shared
    // ticket would wake the quiesced collector with m_requests non-empty and
    // no conn bit — the shouldCollectInCollectorThread() quiescence assert
    // (Heap.cpp:1712, the C1 signature) through a different door. Also note
    // the legacy preconditions below are meaningless once shared: hasAccessBit
    // is the §10B.4 poison while pinned, but clause-(a) migration lets the
    // main holder's release legitimately clear it, so the RELEASE_ASSERT
    // could spuriously fire too.
    if (isSharedServer()) [[unlikely]]
        return false; // Done: under ISS the conn never leaves the mutator.
    RELEASE_ASSERT(oldState & hasAccessBit);
    RELEASE_ASSERT(!(oldState & stoppedBit));
    
    if (!(oldState & mutatorHasConnBit))
        return false; // Done.
    
    if (m_threadShouldStop)
        return false;
    
    if (!m_worldState.compareExchangeWeak(oldState, oldState & ~mutatorHasConnBit))
        return true; // Loop around.
    
    finishRelinquishingConn();
    return true;
}

void Heap::finishRelinquishingConn()
{
    dataLogLnIf(HeapInternal::verbose, "Relinquished the conn.");

    // SharedGC (T9): main-VM-only — conn relinquishing belongs to the legacy
    // collector-thread protocol, unreachable once ISS (collector thread
    // quiesced, §10B.3/I15); sanitizeStackForVM self-guards regardless.
    sanitizeStackForVM(vm());
    
    Locker locker { *m_threadLock };
    if (!m_requests.isEmpty())
        m_threadCondition->notifyOne(locker);
    ParkingLot::unparkAll(&m_worldState);
}

void Heap::relinquishConn()
{
    while (relinquishConn(m_worldState.load())) { }
}

NEVER_INLINE bool Heap::handleNeedCollectionEpilogue(unsigned oldState)
{
    // RELEASE_ASSERT(oldState & hasAccessBit);
    // RELEASE_ASSERT(!(oldState & stoppedBit));
    // SharedGC (§10B.5, T5b): the commented preconditions above would gain
    // "|| worldIsStoppedForAllClients()" — once shared, the legacy
    // hasAccessBit/stoppedBit are main-client-only and the §10.2 conductor
    // runs the epilogue here (End -> NotRunning, conn == Mutator) while the
    // world is stopped for all clients. needCollectionEpilogueBit semantics
    // are unchanged in both protocols; no JS finalizers run inside the stop
    // window (§10B.5).

    if (!(oldState & needCollectionEpilogueBit))
        return false;
    if (m_worldState.compareExchangeWeak(oldState, oldState & ~needCollectionEpilogueBit)) {
        runCollectionEpilogue();
        // Wake up anyone waiting for us to run the epilogue. Note that they may have woken up already, in
        // which case they would be waiting for us to release heap access.
        ParkingLot::unparkAll(&m_worldState);
        return true;
    }
    return true;
}

void Heap::handleNeedCollectionEpilogue()
{
    while (handleNeedCollectionEpilogue(m_worldState.load())) { }
}

void Heap::setNeedCollectionEpilogue()
{
    m_worldState.exchangeOr(needCollectionEpilogueBit);
    ParkingLot::unparkAll(&m_worldState);
    m_stopIfNecessaryTimer->scheduleSoon();
}

void Heap::waitWhileNeedCollectionEpilogue()
{
    for (;;) {
        unsigned oldState = m_worldState.load();
        if (!(oldState & needCollectionEpilogueBit)) {
            // This means that either there was no epilogue request or the main thread will run it
            // with heap access, so a subsequent call to stopTheWorld() will return only when
            // the epilogue finishes.
            return;
        }
        ParkingLot::compareAndPark(&m_worldState, oldState);
    }
}

void Heap::setMutatorWaiting()
{
    m_worldState.exchangeOr(mutatorWaitingBit);
}

void Heap::clearMutatorWaiting()
{
    m_worldState.exchangeAnd(~mutatorWaitingBit);
}

void Heap::notifyThreadStopping(const AbstractLocker&)
{
    clearMutatorWaiting();
    ParkingLot::unparkAll(&m_worldState);
}

void Heap::runCollectionEpilogue()
{
    MonotonicTime before;
    if (Options::logGC()) [[unlikely]] {
        before = MonotonicTime::now();
        dataLog("[GC<", RawPointer(this), ">: epilogue ");
    }
    
    {
        SweepingScope sweepingScope(*this);
        deleteSourceProviderCaches();
        sweepEagerlyInEpilogue();
    }
#if ENABLE(WEBASSEMBLY)
    Wasm::TypeInformation::cleanupIfRequested();
#endif
    
    // SharedGC (T9): conductor-context OK — finalize() runs with the world
    // stopped (shared: §10 step 7 region; legacy: mutator suspended). All
    // vm() uses below clear VM-global caches of the one main VM. Post-GIL
    // (deviation 8) clients are threads of the SAME VM, so these stay
    // singular — NOT clientSet() iteration sites; per-THREAD caches (if any
    // move to VMLite) become the vmstate workstream's responsibility.
    if (HasOwnPropertyCache* cache = vm().hasOwnPropertyCache())
        cache->clear();
    if (auto* cache = vm().megamorphicCache())
        cache->age(m_lastCollectionScope && m_lastCollectionScope.value() == CollectionScope::Full ? CollectionScope::Full : CollectionScope::Eden);

    if (m_lastCollectionScope && m_lastCollectionScope.value() == CollectionScope::Full) {
        vm().jsonAtomStringCache.clear();
        vm().numericStrings.clearOnGarbageCollection();
        vm().stringReplaceCache.clear();
    }
    vm().keyAtomStringCache.clear();
    if (auto* cache = vm().stringSplitCache())
        cache->clear();
    vm().jsonAtomStringCache.clearJSStrings();

    // World-stopped here; under gilOff take the leaf lock anyway so TSAN sees
    // the same guard the concurrent appenders use. Flag-off: the plain prune.
    auto pruneRetainedStrings = [&] {
        m_possiblyAccessedStringsFromConcurrentThreadsOrGCOwnedDataScope.removeAllMatching([&](const auto& iter) {
            return !m_discoveredAccessedStringsFromGCOwnedDataScope.contains(iter.first);
        });
        m_discoveredAccessedStringsFromGCOwnedDataScope.clear();
    };
    if (vm().gilOff()) [[unlikely]] {
        Locker locker { m_possiblyAccessedStringsFromConcurrentThreadsLock };
        pruneRetainedStrings();
    } else
        pruneRetainedStrings();

    immutableButterflyToStringCache.clear();
    
    // SharedGC (T9): conductor-context OK — embedder completion callbacks
    // receive the main VM (the only VM on this server, deviation 3); they
    // run world-stopped and must not allocate or re-enter JS.
    for (const GCCompletionCallback& callback : m_gcCompletionCallbacks)
        callback.run(vm());
    
    if (shouldSweepSynchronously())
        sweepSynchronously();

    if (Options::logGC()) [[unlikely]] {
        MonotonicTime after = MonotonicTime::now();
        dataLog((after - before).milliseconds(), "ms]\n");
    }
}

Heap::Ticket Heap::requestCollection(GCRequest request)
{
    // SharedGC (§10B.1, T5b): unreachable once shared — every trigger
    // re-routes at collectAsync()/collectSync() to requestCollectionShared(),
    // whose precondition is access-holder-or-conductor (§10A) instead of the
    // legacy API-lock/atom-table asserts below.
    ASSERT(!isSharedServer());
    stopIfNecessary();

    ASSERT(vm().currentThreadIsHoldingAPILock() || worldIsStoppedForAllClients()); // SharedGC (T5b): tolerate a late ISS flip mid-call.
    RELEASE_ASSERT(vm().atomStringTable() == Thread::currentSingleton().atomStringTable() || worldIsStoppedForAllClients());
    
    Locker locker { *m_threadLock };
    // We may be able to steal the conn. That only works if the collector is definitely not running
    // right now. This is an optimization that prevents the collector thread from ever starting in most
    // cases.
    ASSERT(m_lastServedTicket <= m_lastGrantedTicket);
    if ((m_lastServedTicket == m_lastGrantedTicket) && !m_collectorThreadIsRunning) {
        dataLogLnIf(HeapInternal::verbose, "Taking the conn.");
        m_worldState.exchangeOr(mutatorHasConnBit);
    }
    
    m_requests.append(request);
    m_lastGrantedTicket++;
    if (!(m_worldState.load() & mutatorHasConnBit))
        m_threadCondition->notifyOne(locker);
    return m_lastGrantedTicket;
}

void Heap::waitForCollection(Ticket ticket)
{
    waitForCollector(
        [&] (const AbstractLocker&) -> bool {
            return m_lastServedTicket >= ticket;
        });
}

void Heap::sweepEagerlyInEpilogue()
{
    m_objectSpace.sweepPreciseAllocations();
#if ENABLE(WEBASSEMBLY)
    // We hold onto a lot of memory, so it makes a lot of sense to be swept eagerly.
    if (m_webAssemblyMemorySpace)
        m_webAssemblyMemorySpace->sweep();
#endif
}

bool Heap::suspendCompilerThreads()
{
#if ENABLE(JIT)
    // We ensure the worklists so that it's not possible for the mutator to start a new worklist
    // after we have suspended the ones that he had started before. That's not very expensive since
    // the worklists use AutomaticThreads anyway.
    if (!Options::useJIT())
        return false;
    // SharedGC (T9): conductor-context OK — VM-global active-plan count of
    // the one main VM (worklist key; see completeAllJITPlans()). Also taken
    // by the §11 reclaimer's own suspend/resume pair (I11).
    if (!vm().numberOfActiveJITPlans())
        return false;
    JITWorklist::ensureGlobalWorklist().suspendAllThreads();
    return true;
#else
    return false;
#endif
}

void Heap::willStartCollection()
{
    ++m_gcVersion;
    // T4: per-cycle Wlr retention accounting — reset world-stopped at cycle
    // start, accumulated by the Wlr constraint executor, consumed by
    // updateAllocationLimits. Always 0 when !isSharedServer() (the only
    // writer is ISS-gated), so flag-off behavior is unchanged.
    m_sharedGCWindowRetainedBytesThisCycle = 0;
    if (Options::verifyGC()) [[unlikely]] {
        m_verifierSlotVisitor = makeUnique<VerifierSlotVisitor>(*this);
        ASSERT(!m_isMarkingForGCVerifier);
    }

    dataLogIf(Options::logGC(), "=> ");
    
    if (shouldDoFullCollection()) {
        m_collectionScope = CollectionScope::Full;
        m_shouldDoFullCollection = false;
        dataLogIf(Options::logGC(), "FullCollection, ");
    } else {
        m_collectionScope = CollectionScope::Eden;
        dataLogIf(Options::logGC(), "EdenCollection, ");
    }
    if (m_collectionScope.value() == CollectionScope::Full) {
        m_sizeBeforeLastFullCollect = m_sizeAfterLastCollect + totalBytesAllocatedThisCycle();
        m_extraMemorySize = 0;
        m_deprecatedExtraMemorySize = 0;
#if ENABLE(RESOURCE_USAGE)
        m_externalMemorySize = 0;
#endif
        m_shouldDoOpportunisticFullCollection = false;
        if (m_fullActivityCallback)
            m_fullActivityCallback->willCollect();
    } else {
        ASSERT(m_collectionScope && m_collectionScope.value() == CollectionScope::Eden);
        m_sizeBeforeLastEdenCollect = m_sizeAfterLastCollect + totalBytesAllocatedThisCycle();
    }

    if (m_edenActivityCallback)
        m_edenActivityCallback->willCollect();

    for (auto* observer : m_observers)
        observer->willGarbageCollect();
}

void Heap::prepareForMarking()
{
    m_objectSpace.prepareForMarking();
}

void Heap::cancelDeferredWorkIfNeeded()
{
    // SharedGC (T9): conductor-context OK — end phase, world stopped;
    // deferredWorkTimer is VM-global state of the one main VM.
    vm().deferredWorkTimer->cancelPendingWork(vm());
}

void Heap::reapWeakHandles()
{
    m_objectSpace.reapWeakSets();
}

void Heap::pruneStaleEntriesFromWeakGCHashTables()
{
    if (!m_collectionScope || m_collectionScope.value() != CollectionScope::Full)
        return;
    // SharedGC (CVE-AUDIT A3 / map-MC-GC S12b): End phase, world stopped —
    // but under gilOff take the registry leaf lock anyway so (a) TSAN sees
    // one consistent guard for the set (same discipline as
    // m_possiblyAccessedStringsFromConcurrentThreadsLock above), and (b) the
    // walk is defended against the K4.VIII.9 secondary signature
    // (.stw-variant.txt — a non-quiescent lite still inside register() while
    // a Class-A stop is wedged; that wedge is a SEPARATE filed bug, FIX-2
    // family). Snapshot under the lock, walk outside it: pruneStaleEntries()
    // overrides do real work (WeakGCMap removeIf) and the lock is leaf-rank,
    // so we don't hold it across callouts. Flag-off: byte-identical (no
    // lock, no snapshot).
    if (vm().gilOff()) [[unlikely]] {
        Vector<WeakGCHashTable*, 16> snapshot;
        {
            Locker locker { m_weakGCHashTablesLock };
            snapshot.reserveInitialCapacity(m_weakGCHashTables.size());
            for (auto* weakGCHashTable : m_weakGCHashTables)
                snapshot.append(weakGCHashTable);
        }
        for (auto* weakGCHashTable : snapshot)
            weakGCHashTable->pruneStaleEntries();
        return;
    }
    for (auto* weakGCHashTable : m_weakGCHashTables)
        weakGCHashTable->pruneStaleEntries();
}

void Heap::sweepArrayBuffers()
{
    // SharedGC (T9): conductor-context OK — end phase, world stopped; the
    // array-buffer registry is server state, vm() is passed through for
    // accounting against the one main VM.
    m_arrayBuffers.sweep(vm(), collectionScope().value_or(CollectionScope::Eden));
}

void Heap::snapshotUnswept()
{
    TimingScope timingScope(*this, "Heap::snapshotUnswept"_s);
    m_objectSpace.snapshotUnswept();
}

void Heap::deleteSourceProviderCaches()
{
    // SharedGC (T9): conductor-context OK — finalize-time, world stopped;
    // VM-global caches of the one main VM (see finalize()).
    if (m_lastCollectionScope && m_lastCollectionScope.value() == CollectionScope::Full)
        vm().clearSourceProviderCaches();
}

void Heap::notifyIncrementalSweeper()
{
    if (m_collectionScope && m_collectionScope.value() == CollectionScope::Full) {
        if (!m_logicallyEmptyWeakBlocks.isEmpty())
            m_indexOfNextLogicallyEmptyWeakBlockToSweep = 0;
    }

    // The sweeper also runs when the server is shared: it sweeps
    // mutator-concurrently on the main VM's run loop, one block per exclusive
    // MSPL hold, never frees or shrinks a block (physical reclamation stays
    // world-stopped, see reclaimSharedGCMemoryAtCycleEnd) and skips
    // weak-bearing blocks (IncrementalSweeper::sweepNextBlockShared). Arming
    // the timer from the conductor thread is safe: setTimeUntilFire locks
    // internally, and the owning run-loop thread is parked for the stop while
    // we run here, so the sweeper's plain members are published by the
    // resume edge.
    m_sweeper->startSweeping(*this);
}

void Heap::updateAllocationLimits()
{
    constexpr bool verbose = false;
    
    dataLogLnIf(verbose, "\nnonOversizedBytesAllocatedThisCycle = ", m_nonOversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed), ", oversizedBytesAllocatedThisCycle", m_oversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed));
    
    // Calculate our current heap size threshold for the purpose of figuring out when we should
    // run another collection. This isn't the same as either size() or capacity(), though it should
    // be somewhere between the two. The key is to match the size calculations involved calls to
    // didAllocate(), while never dangerously underestimating capacity(). In extreme cases of
    // fragmentation, we may have size() much smaller than capacity().
    size_t currentHeapSize = 0;

    // For marked space, we use the total number of bytes visited. This matches the logic for
    // BlockDirectory's calls to didAllocate(), which effectively accounts for the total size of
    // objects allocated rather than blocks used. This will underestimate capacity(), and in case
    // of fragmentation, this may be substantial. Fortunately, marked space rarely fragments because
    // cells usually have a narrow range of sizes. So, the underestimation is probably OK.
    currentHeapSize += m_totalBytesVisited;
    dataLogLnIf(verbose, "totalBytesVisited = ", m_totalBytesVisited, ", currentHeapSize = ", currentHeapSize);

    // It's up to the user to ensure that extraMemorySize() ends up corresponding to allocation-time
    // extra memory reporting.
    auto computedExtraMemorySize = extraMemorySize();
    currentHeapSize += computedExtraMemorySize;
    if (ASSERT_ENABLED) {
        CheckedSize checkedCurrentHeapSize = m_totalBytesVisited;
        checkedCurrentHeapSize += computedExtraMemorySize;
        ASSERT(!checkedCurrentHeapSize.hasOverflowed() && checkedCurrentHeapSize == currentHeapSize);
    }

    dataLogLnIf(verbose, "extraMemorySize() = ", computedExtraMemorySize, ", currentHeapSize = ", currentHeapSize);

    // Get critical memory threshold for next cycle.
    bool isCritical = overCriticalMemoryThreshold(MemoryThresholdCallType::Direct);

    if (m_collectionScope && m_collectionScope.value() == CollectionScope::Full) {
        // To avoid pathological GC churn in very small and very large heaps, we set
        // the new allocation limit based on the current size of the heap, with a
        // fixed minimum.
        size_t lastMaxHeapSize = m_maxHeapSize;
        size_t windowRetainedBytes = isSharedServer() ? std::min(m_sharedGCWindowRetainedBytesThisCycle, currentHeapSize) : 0;
        // B2-serial-eden-block-churn (a): per-TLC partial-block fragmentation
        // rebase. With N>=2 attached clients, every GCClient::Heap holds its
        // own LocalAllocator + partially-filled MarkedBlock for each size
        // class touched in the parallel section, so committed capacity()
        // diverges from m_totalBytesVisited by ~N x the single-client slack
        // (SCALEBENCH §28: footprint W=16 1095MB vs W=1 328MB at IDENTICAL
        // 4.16M-postings live; bytes_allowed 593M vs 120M, 4.92x). Feeding
        // that multi-client transient peak through proportionalHeapSize()
        // hands the SERIAL section (one mutator, siblings parked) ~5x its
        // single-client eden budget; the BigInt loop then mints ~5x the fresh
        // MarkedBlocks per cycle (tryAllocateBlock -> MarkedBlock::tryCreate
        // -> bmalloc mmap -> kernel page-fault), and reclaimSharedGCMemoryAtCycleEnd
        // frees them again — the _raw_spin_lock / asm_exc_page_fault /
        // sync_regs / __free_one_page kernel-symbol family that accounts for
        // ~2900M cycles of the W=16-vs-W=1 1875ms postingsChecksum delta.
        //
        // Rebase the GROWTH HEADROOM on a single-client live estimate by
        // subtracting the per-TLC capacity overhead, exactly mirroring the
        // T4(c)/T2-wlr windowRetainedBytes conservative-subtraction pattern
        // immediately below (so the CIND assert, the m_maxEdenSize
        // subtraction, and the next-eden monotone-visited asserts all hold by
        // the SAME headroomBase/clamp proof). The subtrahend is the share of
        // (capacity - currentHeapSize) attributable to the N-1 extra clients
        // — capacityOverhead * (N-1)/N — clamped to currentHeapSize so the
        // rebase can only LOWER limits (never inflates them; an
        // over-subtraction floors at minHeapSize via the max() below — more
        // GCs, never fewer; correctness > speed). capacity() is exact here
        // (world stopped, I7). clientSet().size() takes its own lock; world
        // stopped -> uncontended (same as the §27 W-adaptive arm). Flag-off /
        // !isSharedServer(): perTLCFragmentationBytes == 0 (the gate
        // short-circuits before the size() call). W=1 GIL-off: numClients<2
        // -> 0. Both take the landed else-branch unchanged — byte-identical.
        //
        // RECORDED, NOT FIXED HERE (B2 secondary): per-client conservative
        // root-scan scaling — ~0.22ms/GC/client linear in attached clients
        // (microbench W=16-parked GC pause 4.297ms vs W=1 0.675ms,
        // ~0.42 + 0.22*W), 141ms total = 7.5% of the 1875ms penalty. That is
        // gatherStackRoots / MachineThreads cost, not allocation-limit
        // policy; addressed separately.
        size_t perTLCFragmentationBytes = 0;
        if (isSharedServer()) [[unlikely]] {
            unsigned numClients = clientSet().size();
            if (numClients >= 2) {
                size_t committed = m_objectSpace.capacity();
                size_t capacityOverhead = committed > currentHeapSize ? committed - currentHeapSize : 0;
                // M3-phaseA-gc-overtrigger (SCALEBENCH §30): the (N-1)/N
                // formula above was derived for the SERIAL section's shape
                // — ONE active mutator, W-1 siblings parked at join() with
                // their TLCs holding stale slack — but it fired identically
                // during phaseA when ALL W clients are actively allocating
                // and every TLC's slack is WORKING headroom, not parked
                // fragmentation. Subtracting it there collapsed
                // headroomBase toward minHeapSize and ~4x'd the
                // FullCollection count (W=16: 12 -> 52 fulls; marking
                // self-time 2.62% -> 9.10%, +6.48pp; phaseA +329-454ms).
                // Scale the rebase by the PARKED fraction instead: count
                // clients whose pre-stop state was access-HELD — the
                // conductor (it triggered this GC from an allocation slow
                // path; m_sharedGCConductorClient is set for the whole
                // conducted tenure, line ~6364) plus every client that
                // released access via the §10A trap-park hook
                // (gcWillParkInStopTheWorld -> m_releasedByGCPark = true,
                // still set throughout the stop window; cleared on resume).
                // A SINFAC-path releaser is NOT counted — that UNDERcounts
                // numActive, so numParked and the rebase can only be too
                // LARGE: more GCs, never fewer (the same conservative
                // direction as the clamp below; correctness > speed).
                // numParked is further capped at numClients-1 so the rebase
                // can never exceed the original B2(a) (N-1)/N value — the
                // CIND assert / m_maxEdenSize subtraction / monotone-visited
                // proof in the comment block above stays sound by the
                // SAME headroomBase/clamp argument.
                //   phaseA (all W active): numParked ~ 0 -> rebase ~ 0
                //     -> headroomBase unchanged -> v34 GC schedule.
                //   serial pc1+2 (1 active): numParked = W-1
                //     -> identical to the original B2(a) intent.
                // Flag-off / W=1 GIL-off: the numClients<2 short-circuit
                // above is unchanged -> perTLCFragmentationBytes == 0
                // -> byte-identical (the forEach is never reached;
                // isSharedServer() gates the whole block anyway).
                unsigned numActive = 0;
                clientSet().forEach([&](GCClient::Heap& client) {
                    if (&client == m_sharedGCConductorClient || client.m_releasedByGCPark)
                        ++numActive;
                });
                if (!numActive)
                    numActive = 1; // The conductor was active by construction; defensive (sync-request paths).
                unsigned numParked = numClients > numActive ? numClients - numActive : 0;
                numParked = std::min(numParked, numClients - 1); // Never exceed the original (N-1)/N rebase.
                perTLCFragmentationBytes = capacityOverhead / numClients * numParked;
            }
        }
        size_t sharedRebaseBytes = std::min(windowRetainedBytes + perTLCFragmentationBytes, currentHeapSize);
        if (sharedRebaseBytes) [[unlikely]] {
            // T4(c) — threshold feedback-loop fix. m_totalBytesVisited (and
            // hence currentHeapSize) includes the Wlr window-witness cohort,
            // which is RETENTION, not live program size; feeding it into
            // proportionalHeapSize() doubled m_maxHeapSize geometrically
            // (capacity ladder 34MB -> 10.5GB, m_maxHeapSize -> 7.87GB):
            // bigger limit -> bigger window -> bigger retained cohort ->
            // bigger visited -> bigger limit. Key the GROWTH HEADROOM to the
            // non-retained estimate instead. windowRetainedBytes undercounts
            // the cohort's traced closure, so this subtraction can only be
            // too small — i.e. limits stay >= what precise accounting would
            // give (conservative; never starves eden). Reached only when the
            // Wlr pass actually retained something this cycle (ISS + >= 2
            // clients): flag-off and single-client GIL-off take the landed
            // formula below unchanged.
            //
            // T2-wlr-rss-residual FLOOR FIX: campaign-1 keyed only the
            // GROWTH headroom on the subtracted base; the FLOOR of
            // m_maxHeapSize stayed at the raw currentHeapSize (= visited,
            // including the cohort), and m_sizeAfterLastCollect /
            // m_sizeAfterLastFullCollect below were stored as the raw value
            // too. With W>=2 the cohort is ~the whole window
            // (bytesAllocatedThisCycle == v on every cycle in the rss2
            // profile), so m_maxHeapSize and m_sizeAfterLast*Collect both
            // ratcheted up by the cohort each full collection — heap.capacity
            // 3146MB at W=2 vs 317MB at W=1, accounting for ~84% of the
            // +3.4GB RSS delta. REBASE currentHeapSize itself to the
            // non-retained estimate so EVERY downstream consumer in this
            // function (m_maxHeapSize floor, m_maxEdenSize subtraction at
            // L+3, m_sizeAfterLastFullCollect, m_sizeAfterLastCollect, and
            // hence collectIfNecessaryOrDefer's bytesAllowedThisCycle =
            // m_maxHeapSize - m_sizeAfterLastCollect and its ASSERT) keys on
            // the same base. headroomBase >= minSize and we add >= minSize,
            // so m_maxHeapSize > currentHeapSize (rebased) — the CIND assert
            // and the m_maxEdenSize subtraction stay sound. The eden-branch
            // ASSERT(currentHeapSize >= m_sizeAfterLastCollect) on the next
            // cycle holds via the clamp below. Flag-off identity:
            // windowRetainedBytes == 0 outside ISS + >= 2 clients, so this
            // branch — and the rebase — are unreachable there.
            //
            // B2-serial-eden-block-churn (a): the subtrahend is the COMBINED
            // sharedRebaseBytes (Wlr cohort + per-TLC capacity overhead), so
            // both shared-only inflation terms key the headroom on the same
            // single-client base in one pass. sharedRebaseBytes is clamped to
            // currentHeapSize above, so the subtraction is non-negative; the
            // minSize floor and the +max(headroom,minSize) addend are
            // unchanged, hence every soundness clause in this comment block
            // holds verbatim with the wider subtrahend.
            size_t minSize = minHeapSize(m_heapType, m_ramSize);
            size_t headroomBase = std::max(currentHeapSize - sharedRebaseBytes, minSize);
            size_t headroom = std::max(proportionalHeapSize(headroomBase, m_ramSize), headroomBase) - headroomBase;
            // Clamp the rebased currentHeapSize to at most m_totalBytesVisited
            // so the next eden's monotone-visited asserts (line 3538
            // currentHeapSize >= m_sizeAfterLastCollect; line 3576
            // >= m_sizeAfterLastFullCollect) hold: an eden cycle's
            // currentHeapSize is m_totalBytesVisited(now) + bytesVisited(eden)
            // + extraMemorySize(eden) >= m_totalBytesVisited(now). headroomBase
            // can exceed m_totalBytesVisited when the minSize floor applies on
            // a small heap (heap-allocation-storm.js et al.) or when
            // extraMemorySize > sharedRebaseBytes; in those cases the clamp
            // sets m_sizeAfterLast*Collect a little lower than headroomBase,
            // which only enlarges bytesAllowedThisCycle (a more lenient
            // trigger — conservative). m_maxHeapSize stays keyed on
            // headroomBase so the threshold itself is unchanged;
            // m_maxHeapSize - currentHeapSize >= max(headroom, minSize) > 0
            // preserves the CIND assert and m_maxEdenSize subtraction.
            currentHeapSize = std::min(headroomBase, m_totalBytesVisited);
            m_maxHeapSize = headroomBase + std::max(headroom, minSize);
        } else
            m_maxHeapSize = std::max(minHeapSize(m_heapType, m_ramSize), proportionalHeapSize(currentHeapSize, m_ramSize));
        m_maxEdenSize = m_maxHeapSize - currentHeapSize;
        if (m_isInOpportunisticTask && !isCritical) {
            // After an Opportunistic Full GC, we allow eden to occupy all the space we recovered.
            // In this case, m_maxHeapSize may be larger than currentHeapSize + m_maxEdenSize.
            // Note that m_maxEdenSize is still used when we increase m_maxHeapSize after an
            // Eden GC to ensure that eden can grow to at least m_maxHeapSize.
            m_maxHeapSize = std::max(m_maxHeapSize, lastMaxHeapSize);
        }
        dataLogLnIf(verbose, "Full: maxHeapSize = ", m_maxHeapSize);
        dataLogLnIf(verbose, "Full: maxEdenSize = ", m_maxEdenSize);
        m_sizeAfterLastFullCollect = currentHeapSize;
        dataLogLnIf(verbose, "Full: sizeAfterLastFullCollect = ", currentHeapSize);
        m_bytesAbandonedSinceLastFullCollect.store(0, std::memory_order_relaxed);
        dataLogLnIf(verbose, "Full: bytesAbandonedSinceLastFullCollect = ", 0);
    } else {
        ASSERT(currentHeapSize >= m_sizeAfterLastCollect);
        // Theoretically, we shouldn't ever scan more memory than the heap size we planned to have.
        // But we are sloppy, so we have to defend against the overflow.
        size_t remainingHeapSize = currentHeapSize > m_maxHeapSize ? 0 : m_maxHeapSize - currentHeapSize;
        dataLogLnIf(verbose, "Eden: remainingHeapSize = ", remainingHeapSize);
        m_sizeAfterLastEdenCollect = currentHeapSize;
        dataLogLnIf(verbose, "Eden: sizeAfterLastEdenCollect = ", currentHeapSize);
        double edenToOldGenerationRatio = (double)remainingHeapSize / (double)m_maxHeapSize;
        if (edenToOldGenerationRatio < Options::minEdenToOldGenerationRatio())
            m_shouldDoFullCollection = true;
        // T4(b)/(c) — retention-pressure full-collection trigger. An eden
        // cycle's Wlr-retained dead cohort is mark-sticky: it is reclaimable
        // only at the next FULL collection. Without a coupling from
        // retention volume to full-collection cadence, eden windows float
        // their dead cohorts indefinitely ("eden GCs reclaim ~nothing":
        // 3857MB -> 3810MB).
        // When this eden cycle retained more than half an eden window,
        // upgrade the next cycle to Full so the cohort is reclaimed promptly.
        // Nonzero only when ISS with >= 2 clients (the only writer of the
        // counter): flag-off and single-client GIL-off never take this
        // branch.
        if (isSharedServer() && m_sharedGCWindowRetainedBytesThisCycle > m_maxEdenSize / 2) [[unlikely]]
            m_shouldDoFullCollection = true;
        // T5-rss-eden-floating-garbage — N-mutator eden-survival Full
        // trigger. With N>=2 mutators allocating concurrently, every
        // eden cycle conservatively roots N live stacks; mutator B's
        // in-flight short-lived temporaries are therefore promoted past the
        // eden boundary even though they die immediately after. rss3
        // measured this directly: W=2 old-gen ratchets 164MB -> 774MB across
        // 19 consecutive eden cycles after Full#61 (single-cycle eden
        // survival 268MB vs W=1's 0.6-3MB), with NO Full in between because
        // (a) the 1/3 edenToOldGenerationRatio gate above is computed
        // against m_maxHeapSize, which the line below ratchets up by exactly
        // the surviving floating garbage every cycle (so the ratio never
        // drops), and (b) the Wlr-retention trigger just above counts only
        // the window-witness cohort, not ordinary eden survivors. The
        // floating dead in old-gen (774MB peak-after-eden vs 143MB true
        // live) is 631MB at W=2 — 85% of the +1176MB W=1->W=2 peak-RSS
        // delta — and was the residual the §25 attribution mis-assigned to
        // per-thread allocator state / segmented butterflies (both refuted
        // by rss3: forceSegmentedButterflies W=1 peaks at 183MB; allocator
        // state is ~6.7MB/thread).
        //
        // Fix: when ISS with >=2 clients and old-gen has grown past
        // m_sizeAfterLastFullCollect * sharedGCEdenSurvivalFullTriggerRatio
        // (default 3.0), force the next collection Full so the floating
        // cohort is reclaimed. Gated on isSharedServer() &&
        // m_distinctAllocatingClientsThisCycle >= 2 (T1-sibint, §31; was
        // clientSet().size()) so flag-off and single-client GIL-off (W=1)
        // are byte-identical: the predicate is tested only after the cheap
        // isSharedServer() relaxed load. m_sizeAfterLastFullCollect == 0
        // (no Full yet this process) skips the whole arm — the very first
        // collection is Full anyway.
        //
        // NO ratchet cap. Review-round-prior had a second arm capping the
        // m_maxHeapSize ratchet below at oldGenGrowthBound + m_maxEdenSize;
        // that was UNSOUND: the cap is fixed for the inter-Full epoch while
        // currentHeapSize keeps growing, and the documented sloppy-overshoot
        // paths (the `currentHeapSize > m_maxHeapSize ? 0 :` defence above,
        // extraMemorySize() not budgeted by bytesAllowedThisCycle, the 1/3-
        // oversized shouldRequestGC exemption, an explicit Eden request
        // after m_shouldDoFullCollection is already set) can drive
        // currentHeapSize >= cap. Then m_maxHeapSize = cap <=
        // currentHeapSize = m_sizeAfterLastCollect: the CIND
        // ASSERT(m_maxHeapSize > m_sizeAfterLastCollect) fires in Debug and
        // bytesAllowedThisCycle underflows to ~SIZE_MAX in Release (GC
        // never requested again). The cap bought at most one eden-overshoot
        // of budget reduction between the trigger and the Full; the trigger
        // alone closes the 19-eden-no-Full gap, so the ratchet line is left
        // byte-identical to upstream.
        //
        // Ratio is W-adaptive (R1-rss-ratio-adaptive, SCALEBENCH §27
        // honest-negative (b)): the option default 3.0 admits ~2 floating
        // live-sets before forcing Full, which was tuned for the W>=4 case
        // and §27 confirms RSS is flat-to-down there (-0.8%..+5.2%). At
        // exactly N=2 clients, campaign-3's T1/T3 keep more JIT code +
        // segmented spines reachable per cycle than §25 projected, so the
        // 3.0× bound is reached later and W=2 peak RSS regressed +10.3%
        // same-host (1543->1701 MB). The effective ratio is therefore
        // computed as min(option, 2.0 + 0.5*(clients-2)): clients==2 -> 2.0
        // (one floating live-set then Full), clients==3 -> 2.5, clients>=4
        // -> 3.0 (== option default; W>=4 path byte-identical to v33). The
        // option remains a hard upper bound so an explicit override (e.g.
        // for a workload with cheaper Fulls) is never raised by the
        // adaptive floor, and 0 still disables the whole arm. Cost at W=2:
        // Full cadence rises toward ~50%, but gcwall measured total STW at
        // 5.4% of W=2 wall, so the marginal Fulls cost <1% wall against the
        // ~158 MB RSS recovery.
        //
        // T1-sibint (SCALEBENCH §31): the gate now reads
        // m_distinctAllocatingClientsThisCycle (count of clients that
        // actually called didAllocate() since the last reset below) instead
        // of clientSet().size() (registered clients). sibint proved the
        // entire 1.55-1.57x serial-section slowdown at W=16 IS this trigger:
        // during the pc-loop only thread-0 allocates while 15 siblings sit
        // parked at a JS barrier, but clientSet().size()==16, so after the
        // first post-phaseA Full (sizeAfterLastFullCollect=33MB) the lone
        // mutator's ~494MB post-eden heap blows past 33*3.0=99MB and forces
        // alternating Eden/Full (32/32 vs W=1's 99/0). Reading the
        // distinct-allocator count makes the serial pc-loop see
        // numClients==1 -> trigger never fires -> all-Eden, exactly the W=1
        // schedule. The same value feeds the W-adaptive effectiveRatio so a
        // genuinely-parallel phase with k<W active allocators uses the
        // k-appropriate floor. Relaxed load is exact here (world stopped,
        // I7). The >=2 test stays inside the body so the counter load stays
        // behind the cheap isSharedServer() relaxed-load gate — flag-off and
        // W=1 GIL-off remain byte-identical (isSharedServer() is false at
        // W=1; the counter is never written on the !isSharedServer()
        // didAllocate leg).
        //
        // F4-burst (SCALEBENCH §32(b)): the §31 window = "first didAllocate
        // of the cycle" proved too short at W=32 phaseB: all 32 siblings wake
        // from the JS barrier and each performs >=1 (tiny) allocation inside
        // a single short Eden cycle -> numClients=32 -> trigger fires ->
        // Full; reset; next cycle the same wake-burst -> another Full, i.e.
        // the alternating Eden/Full train §31 was meant to remove, restored
        // by a transient burst rather than sustained N-mutator pressure. The
        // bump is therefore deferred until a client has allocated at least
        // m_distinctAllocatorByteThreshold bytes this cycle (recomputed below
        // as m_maxEdenSize / registeredClients / 4 — i.e. one quarter of a
        // fair-share eden slice). A wake-time freelist refill (~16KB) cannot
        // cross it; a genuinely-allocating client crosses it well before the
        // eden budget is spent, so a sustained k-mutator phase still reads
        // numClients==k. The §31 pc-loop case is preserved a fortiori: the
        // 15 parked siblings allocate 0 bytes, so numClients stays 1 there
        // exactly as before.
        if (isSharedServer() && m_sizeAfterLastFullCollect && Options::sharedGCEdenSurvivalFullTriggerRatio()) [[unlikely]] {
            unsigned numClients = m_distinctAllocatingClientsThisCycle.load(std::memory_order_relaxed);
            if (numClients >= 2) {
                double optionRatio = Options::sharedGCEdenSurvivalFullTriggerRatio();
                double effectiveRatio = std::min(optionRatio, 2.0 + 0.5 * static_cast<double>(numClients - 2));
                size_t oldGenGrowthBound = static_cast<size_t>(static_cast<double>(m_sizeAfterLastFullCollect) * effectiveRatio);
                if (currentHeapSize > oldGenGrowthBound) {
                    m_shouldDoFullCollection = true;
                    dataLogLnIf(verbose, "Eden: shared N-mutator floating-garbage Full trigger (currentHeapSize ", currentHeapSize, " > sizeAfterLastFull ", m_sizeAfterLastFullCollect, " * ", effectiveRatio, " [allocatingClients=", numClients, ", option=", optionRatio, "])");
                }
            }
        }
        m_maxHeapSize = std::max(m_maxHeapSize, currentHeapSize + m_maxEdenSize);
        dataLogLnIf(verbose, "Eden: maxHeapSize = ", m_maxHeapSize);
        dataLogLnIf(verbose, "Eden: maxEdenSize = ", m_maxEdenSize);
        // T4(c): re-enabled when shared (was the §5.4/I15 blanket disable;
        // that disable removed the only idle-time full-GC trigger and helped
        // make capacity monotone). Safe here: we run world-stopped (every
        // mutator parked), so the callback's plain timer state has no
        // concurrent reader/writer; the timer's eventual doCollection() goes
        // through Heap::collect -> collectAsync, whose ISS arm reroutes to
        // the §10B.1 ticketing (no fire-and-forget collection can result —
        // I15's actual invariant is preserved by the trigger path, not by
        // suppressing the timer).
        if (m_fullActivityCallback) {
            ASSERT(currentHeapSize >= m_sizeAfterLastFullCollect);
            m_fullActivityCallback->didAllocate(*this, currentHeapSize - m_sizeAfterLastFullCollect);
        }
    }

    m_sizeAfterLastCollect = currentHeapSize;
#if USE(BUN_JSC_ADDITIONS)
    if (std::exchange(m_reenableEdenActivityCallback, false) && m_edenActivityCallback)
        m_edenActivityCallback->setEnabled(true);
    if (std::exchange(m_reenableFullActivityCallback, false) && m_fullActivityCallback)
        m_fullActivityCallback->setEnabled(true);
#endif
    dataLogLnIf(verbose, "sizeAfterLastCollect = ", m_sizeAfterLastCollect);
    // I7: we are at a safepoint (world stopped); the relaxed counters are
    // exact here.
    m_nonOversizedBytesAllocatedThisCycle.store(0, std::memory_order_relaxed);
    m_oversizedBytesAllocatedThisCycle.store(0, std::memory_order_relaxed);
    m_lastOversidedAllocationThisCycle.store(0, std::memory_order_relaxed);
    // T1-sibint (SCALEBENCH §31) + F4-burst (§32(b)): reset the distinct-
    // allocator count, the per-client latched bool, and the per-client byte
    // accumulator alongside the byte counters; recompute the byte threshold
    // for the next cycle. World is stopped (I7): the per-client fields have
    // no concurrent reader/writer (each is otherwise touched only by its own
    // access-holding thread in didAllocate's isSharedServer() leg), and
    // clientSet() iteration is safe (no attach/detach in flight). Gated on
    // isSharedServer() so flag-off and W=1 GIL-off never reach the forEach —
    // byte-identical.
    if (isSharedServer()) [[unlikely]] {
        m_distinctAllocatingClientsThisCycle.store(0, std::memory_order_relaxed);
        // F4-burst: threshold = one quarter of a fair-share eden slice. The
        // divisor uses registered clients (clientSet().size()), not last
        // cycle's distinct count, so a phase with few active allocators gets
        // a LARGER per-client threshold (harder to over-count) and a phase
        // with many registered clients gets a smaller one (a genuinely busy
        // client still crosses it quickly). size() is safe world-stopped
        // (same I7 argument as forEach). m_maxEdenSize was finalised above
        // for both Full and Eden branches. Floor: integer division may yield
        // 0 for tiny edens / huge W -> degrades to the §31 first-alloc
        // semantics (every allocation counts), never less aggressive than
        // before.
        unsigned registered = clientSet().size();
        m_distinctAllocatorByteThreshold = m_maxEdenSize / std::max<unsigned>(registered, 1) / 4;
        clientSet().forEach([](GCClient::Heap& client) {
            client.m_allocatedThisServerCycle = false;
            client.m_bytesAllocatedThisServerCycle = 0;
        });
    }

    dataLogIf(Options::logGC(), "=> ", currentHeapSize / 1024, "kb, ");
}

void Heap::didFinishCollection()
{
    m_afterGC = MonotonicTime::now();
    CollectionScope scope = *m_collectionScope;
    if (scope == CollectionScope::Full)
        m_lastFullGCLength = m_afterGC - m_beforeGC;
    else
        m_lastEdenGCLength = m_afterGC - m_beforeGC;

#if ENABLE(RESOURCE_USAGE)
    ASSERT(externalMemorySize() <= extraMemorySize());
#endif

    // SharedGC (T9): conductor-context OK — end-of-collection bookkeeping,
    // world stopped; the heap profiler is owned by the one main VM.
    if (HeapProfiler* heapProfiler = vm().heapProfiler()) {
        gatherExtraHeapData(*heapProfiler);
        removeDeadHeapSnapshotNodes(*heapProfiler);
    }

    if (m_verifier) [[unlikely]]
        m_verifier->endGC();

    RELEASE_ASSERT(m_collectionScope);
    m_lastCollectionScope = m_collectionScope;
    m_collectionScope = std::nullopt;

    for (auto* observer : m_observers)
        observer->didGarbageCollect(scope);

#if USE(MIMALLOC)
    // Process retired theap pages and queue arena purges; the consumer's
    // mimalloc scavenger thread does the actual madvise off the GC thread.
    if (scope == CollectionScope::Full)
        bmalloc::api::scavengeThisThread(/* force */ false);
#endif
}

void Heap::resumeCompilerThreads()
{
#if ENABLE(JIT)
    JITWorklist::ensureGlobalWorklist().resumeAllThreads();
#endif
}

GCActivityCallback* Heap::fullActivityCallback()
{
    return m_fullActivityCallback.get();
}

GCActivityCallback* Heap::edenActivityCallback()
{
    return m_edenActivityCallback.get();
}

void Heap::setGarbageCollectionTimerEnabled(bool enable)
{
    // An explicit choice supersedes setInitialAllocationBudget()'s "re-enable after the first collection".
    m_reenableEdenActivityCallback = false;
    m_reenableFullActivityCallback = false;
    if (m_fullActivityCallback)
        m_fullActivityCallback->setEnabled(enable);
    if (m_edenActivityCallback)
        m_edenActivityCallback->setEnabled(enable);
}

constexpr size_t oversizedAllocationThreshold = 64 * KB;
void Heap::didAllocate(size_t bytes)
{
    if (!isSharedServer()) [[likely]] {
        // Single-writer regime: plain load+store RMW (no lock-prefixed xadd on
        // the per-freelist-refill path). Why no increment can be lost:
        //   (a) Precondition — didAllocate executes only within heap-access-
        //       holding sections (every caller is a mutator-side allocation /
        //       extra-memory path; asserted below), so when !isSharedServer()
        //       there is exactly one writer, serialized by heap access; the
        //       store(0) resets in updateAllocationLimits run world-stopped.
        //   (b) isSharedServer() transitions in BOTH directions only while
        //       heap access is quiescent or held by the transitioning thread:
        //       false->true at second-client attach (§10B.4: flip under
        //       *m_threadLock with hasAccessBit pinned, so the legacy inline
        //       access CAS is unwinnable and post-flip acquisition routes
        //       through acquireAccessSlow(), which locks *m_threadLock —
        //       store happens-before unlock happens-before lock happens-before
        //       the next relaxed isSharedServer() load here); true->false in
        //       pollIssRevertIfNeeded (§10D), executed by the sole surviving
        //       client's own access-holding thread after the registry-locked
        //       size==1 re-check, ticket-quiescent, collector not running.
        //   Therefore a plain-leg execution and a fetch_add-leg execution can
        //   never be concurrent; whenever two writers can exist, every writer
        //   observes isSharedServer()==true and takes the fetch_add leg.
        // Counters stay std::atomic (store/load) — at-safepoint cross-thread
        // readers rely on atomicity for tear-freedom.
        ASSERT(hasHeapAccess() || worldIsStopped());
        if (bytes >= oversizedAllocationThreshold) {
            m_oversizedBytesAllocatedThisCycle.store(m_oversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
            m_lastOversidedAllocationThisCycle.store(bytes, std::memory_order_relaxed);
        } else
            m_nonOversizedBytesAllocatedThisCycle.store(m_nonOversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
    } else {
        // T1-sibint (SCALEBENCH §31) + F4-burst (§32(b)): bump the server's
        // distinct-allocator count once this client has allocated at least
        // m_distinctAllocatorByteThreshold bytes this cycle (one quarter of a
        // fair-share eden slice). The per-client bool + accumulator are plain
        // (this client's own access-holding thread is the only mutator-side
        // writer; the world-stopped reset in updateAllocationLimits is the
        // only other writer — same regime as the byte counters' (a)/(b)
        // argument above). currentThreadClient() is an initial-exec TLS read.
        // Steady-state hot path is unchanged from §31: once the bool latches
        // true the [[unlikely]] arm is skipped for the remainder of the
        // cycle, so the accumulator add+compare cost is bounded by
        // (threshold / mean-bytes-per-call) iterations per client per cycle.
        // m_distinctAllocatorByteThreshold is written only world-stopped (no
        // concurrent writer here; same publication regime as m_maxEdenSize).
        // Null-check is defensive for world-stopped collector-thread
        // allocations (no client bound); those contribute to byte counters
        // but not the distinct-client count, which is the conservative
        // direction for the Full trigger. W>=2-only: this whole leg is
        // reached iff isSharedServer(); the !isSharedServer() arm above
        // (W=1 / flag-off) is unchanged.
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && !client->m_allocatedThisServerCycle) [[unlikely]] {
            client->m_bytesAllocatedThisServerCycle += bytes;
            if (client->m_bytesAllocatedThisServerCycle >= m_distinctAllocatorByteThreshold) {
                client->m_allocatedThisServerCycle = true;
                m_distinctAllocatingClientsThisCycle.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (bytes >= oversizedAllocationThreshold) {
            m_oversizedBytesAllocatedThisCycle.fetch_add(bytes, std::memory_order_relaxed);
            m_lastOversidedAllocationThisCycle.store(bytes, std::memory_order_relaxed);
        } else
            m_nonOversizedBytesAllocatedThisCycle.fetch_add(bytes, std::memory_order_relaxed);
    }

    // totalBytesAllocatedThisCycle() depends on values updated above.
    // So, only do this m_edenActivityCallback after updating those values.
    // T4(c): eden-activity dispatch re-enabled when shared, restricted to
    // the main client's thread (single-writer regime for the callback's
    // plain timer state — see activityCallbackDispatchAllowed()). The
    // resulting timer fire routes through collectAsync's ISS arm into
    // §10B.1 ticketing, so I15's no-fire-and-forget invariant holds at the
    // trigger path. Counters are relaxed atomics (F3); exact at safepoints
    // (I7).
    if (m_edenActivityCallback && activityCallbackDispatchAllowed())
        m_edenActivityCallback->didAllocate(*this, totalBytesAllocatedThisCycle() + m_bytesAbandonedSinceLastFullCollect.load(std::memory_order_relaxed));

    performIncrement(bytes);
}

void Heap::addFinalizer(JSCell* cell, CFinalizer finalizer)
{
    WeakSet::allocate(cell, &m_cFinalizerOwner, std::bit_cast<void*>(finalizer)); // Balanced by CFinalizerOwner::finalize().
}

void Heap::addFinalizer(JSCell* cell, LambdaFinalizer function)
{
    WeakSet::allocate(cell, &m_lambdaFinalizerOwner, function.leak()); // Balanced by LambdaFinalizerOwner::finalize().
}

void Heap::CFinalizerOwner::finalize(Handle<Unknown> handle, void* context)
{
    HandleSlot slot = handle.slot();
    CFinalizer finalizer = std::bit_cast<CFinalizer>(context);
    finalizer(slot->asCell());
    WeakSet::deallocate(WeakImpl::asWeakImpl(slot));
}

void Heap::LambdaFinalizerOwner::finalize(Handle<Unknown> handle, void* context)
{
    auto finalizer = WTF::adopt(static_cast<LambdaFinalizer::Impl*>(context));
    HandleSlot slot = handle.slot();
    // Lambda finalizers (ThreadObject's ThreadState hook, WebCore's
    // ScriptController completion guards, embedder hooks) may clear Strongs
    // under StrongSet::m_gilOffLock and take API-rank locks such as
    // ThreadState::joinLock, which must happen entered-with-access outside a
    // stop window. WeakBlock::sweep reaches here inside the conducted GIL-off
    // stop, so the body is deferred to drainDeferredLambdaFinalizers at the
    // conductSharedCollection tail (after closeSharedGCStopWindow and
    // acquireHeapAccess, on this same conductor thread); the WeakImpl is
    // deallocated now. WSAC is set only between open/closeSharedGCStopWindow,
    // so lastChanceToFinalize and any non-stopped sweep run the body inline.
    // Conductor-thread-private append (see m_deferredLambdaFinalizers).
    if (VM::isGILOffProcess() && m_heap.worldIsStoppedForAllClients()) [[unlikely]] {
        m_heap.m_deferredLambdaFinalizers.append(WTF::move(finalizer));
        WeakSet::deallocate(WeakImpl::asWeakImpl(slot));
        return;
    }
    finalizer(slot->asCell());
    WeakSet::deallocate(WeakImpl::asWeakImpl(slot));
}

void Heap::drainDeferredLambdaFinalizers()
{
    // §F.3 carve-out (b) drain (MC-GC S5 / CVE-AUDIT B7): runs the
    // addFinalizer lambdas that LambdaFinalizerOwner::finalize deferred from
    // inside the conducted §10 stop. Called from conductSharedCollection's
    // tail, AFTER closeSharedGCStopWindow (world resumed; WSAC cleared) and
    // conductorClient.acquireHeapAccess (the conductor is entered-with-access
    // — the carve-out's stated context), BEFORE the conductor returns and
    // re-enters its §10.2 ticket loop or releases anything. The lambdas clear
    // Strongs under StrongSet::m_gilOffLock (a §LK.8 destructor-leaf lock,
    // taken with access held — its sanctioned context) and take
    // ThreadState::joinLock (api-rank, never across user JS); both are now
    // ordinary mutator-side acquisitions racing other resumed mutators —
    // exactly the carve-out (b) intent — instead of conductor-in-stop writes
    // outside the WS(ii) closed list. Heap I6 ("parked threads hold neither
    // lock") made the in-stop execution benign TODAY, but SPEC-congc moves
    // finalization off the global stop, at which point that argument
    // collapses; the deferral is the forward-safe form.
    // The JSCell* argument is passed as nullptr: the cell was dead at sweep
    // time and its memory may already hold a new cell (the world resumed and
    // reclaimSharedGCMemoryAtCycleEnd ran inside the stop). This is the
    // LambdaFinalizer contract declared in Heap.h; a finalizer that needs the
    // cell address must be a CFinalizer.
    // Conductor-thread-private (see m_deferredLambdaFinalizers in Heap.h).
    if (m_deferredLambdaFinalizers.isEmpty())
        return;
    ASSERT(VM::isGILOffProcess());
    ASSERT(!worldIsStoppedForAllClients());
    auto deferred = std::exchange(m_deferredLambdaFinalizers, { });
    for (auto& finalizer : deferred)
        finalizer(nullptr);
}

void Heap::collectNowFullIfNotDoneRecently(Synchronousness synchronousness)
{
    if (!m_fullActivityCallback) {
        collectNow(synchronousness, CollectionScope::Full);
        return;
    }

    if (m_fullActivityCallback->didGCRecently()) {
        // A synchronous GC was already requested recently so we merely accelerate next collection.
        reportAbandonedObjectGraph();
        return;
    }

    m_fullActivityCallback->setDidGCRecently(true);
    collectNow(synchronousness, CollectionScope::Full);
}

void Heap::setFullActivityCallback(RefPtr<GCActivityCallback>&& callback)
{
    m_fullActivityCallback = WTF::move(callback);
}

void Heap::setEdenActivityCallback(RefPtr<GCActivityCallback>&& callback)
{
    m_edenActivityCallback = WTF::move(callback);
}

void Heap::disableStopIfNecessaryTimer()
{
    m_stopIfNecessaryTimer->disable();
}

bool Heap::useGenerationalGC()
{
    return Options::useGenerationalGC() && !VM::isInMiniMode();
}

bool Heap::shouldSweepSynchronously()
{
    // updateAllocationLimits() updates info that overCriticalMemoryThreshold() needs.
    return overCriticalMemoryThreshold() || Options::sweepSynchronously() || VM::isInMiniMode();
}

#if USE(BUN_JSC_ADDITIONS)
void Heap::setInitialAllocationBudget(size_t bytes)
{
    if (m_sizeAfterLastCollect || m_lastCollectionScope)
        return; // a collection already ran; the heap is sizing itself from what it found
    if (bytes <= m_maxEdenSize)
        return;
    m_maxEdenSize = bytes;
    m_maxHeapSize = bytes;
    // The allocation-paced timers would otherwise bring the first collection in well under the budget; they come back
    // on as soon as any collection (budget, explicit request, memory pressure) has run.
    if (m_edenActivityCallback && m_edenActivityCallback->isEnabled()) {
        m_edenActivityCallback->setEnabled(false);
        m_reenableEdenActivityCallback = true;
    }
    if (m_fullActivityCallback && m_fullActivityCallback->isEnabled()) {
        m_fullActivityCallback->setEnabled(false);
        m_reenableFullActivityCallback = true;
    }
}
#endif

bool Heap::shouldDoFullCollection()
{
    // UNGIL §D.1 / ANNEX D1 (U-T12): a SEALED dead-TID rebias snapshot "arms
    // the next full collection" — upgrade the shared server's next conducted
    // cycle to Full so the in-stop restamp + D1R fire can run and the SD9
    // RangeError exhaustion window can close (liveness: without the upgrade
    // an Eden-only workload would never rebias and exhausted spawns would
    // RangeError forever). gilOffProcess-only — GIL-on keeps Dev 10 and the
    // probe is never armed there (U19: GIL-on behavior unchanged); flag-off
    // the branch is dead (isGILOffProcess() false). isSharedServer() under
    // gilOffProcess identifies the U0c WINNER heap uniquely (I13: shared-
    // server-ness is process-unique, s_stickySharedServer CAS), so this
    // probe cannot upgrade a foreign heap's cycle; the snapshot-CONSUMING
    // edge is additionally RELEASE_ASSERTed vm().gilOff() in
    // conductSharedCollection.
    if (VM::isGILOffProcess() && isSharedServer() && ThreadManager::singleton().rebiasSnapshotIsSealed()) [[unlikely]]
        return true;

    if (!useGenerationalGC())
        return true;

    if (!m_currentRequest.scope)
        return m_shouldDoFullCollection || overCriticalMemoryThreshold();
    return *m_currentRequest.scope == CollectionScope::Full;
}

void Heap::addLogicallyEmptyWeakBlock(WeakBlock* block)
{
    // SharedGC (T8 audit): reached only from WeakSet::sweep, which runs under
    // MSPL (in-lock block sweeps, §5.2) or on the conductor while stopped.
    ASSERT(!isSharedServer() || worldIsStoppedForAllClients() || mutatorSlowPathLock().isHeld());
    RELEASE_ASSERT(!block->next() && !block->prev());
    m_logicallyEmptyWeakBlocks.append(block);
}

void Heap::sweepAllLogicallyEmptyWeakBlocks()
{
    // SharedGC (T8): collectNow(Sync)'s tail and server teardown call this on
    // an access-holding thread; serialize against WeakSet::sweep's
    // m_logicallyEmptyWeakBlocks mutations (no-op when !isSharedServer()).
    MutatorSlowPathLocker mutatorSlowPathLocker(*this);

    if (m_logicallyEmptyWeakBlocks.isEmpty())
        return;

    m_indexOfNextLogicallyEmptyWeakBlockToSweep = 0;
    while (sweepNextLogicallyEmptyWeakBlock()) { }
}

bool Heap::sweepNextLogicallyEmptyWeakBlock()
{
    // SharedGC (T8 audit): callers — WeakSet::sweep (MSPL or conductor,
    // including the §A.3 class-4 conductor, AB-10 — see WeakSet.cpp),
    // IncrementalSweeper (T4(d): holds MSPL once shared —
    // sweepNextBlockShared), and sweepAllLogicallyEmptyWeakBlocks (takes
    // MSPL).
    ASSERT(!isSharedServer() || worldIsStoppedForAllClients() || mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));
    if (m_indexOfNextLogicallyEmptyWeakBlockToSweep == WTF::notFound)
        return false;

    WeakBlock* block = m_logicallyEmptyWeakBlocks[m_indexOfNextLogicallyEmptyWeakBlockToSweep];
    RELEASE_ASSERT(!block->next() && !block->prev());

    block->sweep();
    if (block->isEmpty()) {
        std::swap(m_logicallyEmptyWeakBlocks[m_indexOfNextLogicallyEmptyWeakBlockToSweep], m_logicallyEmptyWeakBlocks.last());
        m_logicallyEmptyWeakBlocks.removeLast();
        WeakBlock::destroy(*this, block);
    } else
        m_indexOfNextLogicallyEmptyWeakBlockToSweep++;

    if (m_indexOfNextLogicallyEmptyWeakBlockToSweep >= m_logicallyEmptyWeakBlocks.size()) {
        m_indexOfNextLogicallyEmptyWeakBlockToSweep = WTF::notFound;
        return false;
    }

    return true;
}

size_t Heap::visitCount()
{
    size_t result = 0;
    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            result += visitor.visitCount();
        });
    return result;
}

size_t Heap::bytesVisited()
{
    size_t result = 0;
    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            result += visitor.bytesVisited();
        });
    return result;
}

void Heap::forEachCodeBlockImpl(const ScopedLambda<void(CodeBlock*)>& func)
{
    // We don't know the full set of CodeBlocks until compilation has terminated.
    completeAllJITPlans();

    return m_codeBlocks->iterate(func);
}

void Heap::forEachCodeBlockIgnoringJITPlansImpl(const AbstractLocker& locker, const ScopedLambda<void(CodeBlock*)>& func)
{
    return m_codeBlocks->iterate(locker, func);
}

void Heap::writeBarrierSlowPath(const JSCell* from)
{
    if (mutatorShouldBeFenced()) [[unlikely]] {
        // In this case, the barrierThreshold is the tautological threshold, so from could still be
        // not black. But we can't know for sure until we fire off a fence.
        WTF::storeLoadFence();
        if (from->cellState() != CellState::PossiblyBlack)
            return;
    }
    
    addToRememberedSet(from);
}

bool Heap::currentThreadIsDoingGCWork()
{
    return Thread::mayBeGCThread() || mutatorState() != MutatorState::Running;
}

void Heap::reportExtraMemoryVisited(size_t size)
{
    size_t* counter = &m_extraMemorySize;
    
    for (;;) {
        size_t oldSize = *counter;
        // FIXME: Change this to use SaturatingArithmetic when available.
        // https://bugs.webkit.org/show_bug.cgi?id=170411
        CheckedSize checkedNewSize = oldSize;
        checkedNewSize += size;
        size_t newSize = std::numeric_limits<size_t>::max();
        if (!checkedNewSize.hasOverflowed()) [[likely]]
            newSize = checkedNewSize.value();
        if (WTF::atomicCompareExchangeWeakRelaxed(counter, oldSize, newSize))
            return;
    }
}

#if ENABLE(RESOURCE_USAGE)
void Heap::reportExternalMemoryVisited(size_t size)
{
    size_t* counter = &m_externalMemorySize;

    for (;;) {
        size_t oldSize = *counter;
        if (WTF::atomicCompareExchangeWeakRelaxed(counter, oldSize, oldSize + size))
            return;
    }
}
#endif

void Heap::collectIfNecessaryOrDefer(GCDeferralContext* deferralContext)
{
    ASSERT(deferralContext || isDeferred() || !AssertNoGC::isInEffectOnCurrentThread());
    // SharedGC (T9): conductor-context OK — CIND is called by EVERY client
    // (incl. standalone, via the §12.1 allocateForClient seam); vm() is plain
    // arithmetic to the main VM (deviation 3) and verifyCanGC reads only
    // compile-state validation flags. Deferral itself is per-client (I17).
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    if (!m_isSafeToCollect)
        return;

    switch (mutatorState()) {
    case MutatorState::Running:
    case MutatorState::Allocating:
        break;
    case MutatorState::Sweeping:
    case MutatorState::Collecting:
        return;
    }
    if (!Options::useGC()) [[unlikely]]
        return;
    
    // IT-4 review: the hint write below must stay owner-thread-only. On a
    // shared server, an unstamped thread's didDeferGCWorkSlot() aliases the
    // server's plain bool — a cross-thread plain-bool write GIL-off, the
    // exact race the per-client split exists to avoid. Progress never
    // depends on the hint (see the branch comments), so unstamped threads
    // simply skip it.
    auto ownsDidDeferGCWorkSlot = [&]() -> bool {
        if (!isSharedServer()) [[likely]]
            return true; // Single-client heap: the slot is the owning thread's own flag.
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        return client && &client->server() == this;
    };

    if (mayNeedToStop()) {
        if (deferralContext)
            deferralContext->m_shouldGC = true;
        else if (isDeferred())
            didDeferGCWorkSlot() = true; // Review round 4: per-client hint once ISS (pairs with the per-client depth isDeferred() just consulted).
        else if (currentThreadHasSTWForbiddenScope()) [[unlikely]] {
            // IT-4 (I14/S1): an allocation inside an STW-forbidden region
            // (e.g. the SAL held for a structure transition) reached this
            // poll without a threaded GCDeferralContext and without an
            // enclosing DeferGC. Parking for the stop here would hold the
            // process-global SAL across the whole stop window (S1-S3
            // violation; debug: the I14 assert in
            // stopIfNecessaryForAllClients). Defer instead. Progress edge:
            // there is NO enclosing DeferGC here (isDeferred() was false),
            // so the hint cannot be consumed by a DeferGC unwind on this
            // path — liveness rests on GSP staying set, so the stop is
            // served at this thread's first post-region poll (per-lite trap
            // word or the next allocation slow-path CIND, by which point
            // the scope has exited and this branch no longer takes). The
            // hint write is a best-effort accelerant for a future DeferGC
            // unwind and is skipped when this thread does not own the slot.
            dataLogLnIf(Options::logGC(), "[GC<", RawPointer(this), ">] IT-4: deferring stop poll inside STW-forbidden scope");
            if (ownsDidDeferGCWorkSlot())
                didDeferGCWorkSlot() = true;
        } else
            stopIfNecessary();
    }
    
    auto shouldRequestGC = [&] () -> bool {
        bool logRequestGC = false;
        // Don't log if we already have a request pending or if we have to come back later so we don't flood dataFile.
        if (Options::logGC()) [[unlikely]]
            logRequestGC = m_requests.isEmpty() && !deferralContext && !isDeferred();
#if !USE(BUN_JSC_ADDITIONS)
        if (Options::gcMaxHeapSize()) [[unlikely]] {
            size_t bytesAllocatedThisCycle = totalBytesAllocatedThisCycle();
            if (bytesAllocatedThisCycle <= Options::gcMaxHeapSize())
                return false;
            dataLogLnIf(logRequestGC, "Requesting GC because bytes allocated this cycle: ", bytesAllocatedThisCycle, " exceed Options::gcMaxHeapSize(): ", Options::gcMaxHeapSize());
            return true;
        }
#endif

        ASSERT(m_maxHeapSize > m_sizeAfterLastCollect);
        size_t bytesAllowedThisCycle = m_maxHeapSize - m_sizeAfterLastCollect;

        bool isCritical = overCriticalMemoryThreshold();
        if (isCritical)
            bytesAllowedThisCycle = std::min(m_maxEdenSizeWhenCritical, bytesAllowedThisCycle);

        size_t bytesAllocatedThisCycle = totalBytesAllocatedThisCycle();

#if USE(BUN_JSC_ADDITIONS)
        if (Options::gcMaxHeapSize()) {
            if (bytesAllocatedThisCycle > Options::gcMaxHeapSize()) {
                dataLogLnIf(logRequestGC, "Requesting GC because bytes allocated this cycle: ", bytesAllocatedThisCycle, " exceed Options::gcMaxHeapSize(): ", Options::gcMaxHeapSize());
                return true;
            }
        }
#endif

        if (bytesAllocatedThisCycle <= bytesAllowedThisCycle)
            return false;

        // We don't want to GC if the last oversized allocation makes up too much of the memory allocated this cycle since it's likely
        //  that object is still live and doesn't give us much indication about how much memory we could actually reclaim. That said,
        // if the system is cricital or we have a small heap we want to be very agressive about reclaiming memory to reduce overall
        // pressure on the system.
        if (!isCritical && m_heapType == HeapType::Large) {
            if (static_cast<double>(m_lastOversidedAllocationThisCycle.load(std::memory_order_relaxed)) / bytesAllocatedThisCycle > 1.0 / 3.0)
                return false;
        }

        dataLogLnIf(logRequestGC, "Requesting GC because bytes allocated this cycle: ", bytesAllocatedThisCycle, " exceed bytes allowed: ", bytesAllowedThisCycle, ConditionalDump(isCritical, " (critical)"), " normal bytes: ", m_nonOversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed), " oversized bytes: ", m_oversizedBytesAllocatedThisCycle.load(std::memory_order_relaxed), " last oversized: ", m_lastOversidedAllocationThisCycle.load(std::memory_order_relaxed));
        return true;
    };
    if (!shouldRequestGC())
        return;

    if (deferralContext)
        deferralContext->m_shouldGC = true;
    else if (isDeferred())
        didDeferGCWorkSlot() = true; // Review round 4: per-client hint once ISS.
    else if (currentThreadHasSTWForbiddenScope()) [[unlikely]] {
        // IT-4: see the mayNeedToStop() branch above — never initiate/park
        // inside an STW-forbidden region. Liveness does not depend on the
        // hint: shouldRequestGC() recomputes from allocation counters at the
        // next poll after the scope exits.
        dataLogLnIf(Options::logGC(), "[GC<", RawPointer(this), ">] IT-4: deferring GC request inside STW-forbidden scope");
        if (ownsDidDeferGCWorkSlot())
            didDeferGCWorkSlot() = true;
    } else {
        collectAsync();
        stopIfNecessary(); // This will immediately start the collection if we have the conn.
    }
}

void Heap::decrementDeferralDepthAndGCIfNeededSlow()
{
    // Can't do anything if we're still deferred. SharedGC (§5.4/I17): this
    // consults the CALLING client's depth once ISS.
    if (currentDeferralDepth())
        return;

    ASSERT(!isDeferred());

    // Review round 4: clears only the CALLING thread's slot once ISS — a
    // concurrent set by another client lands in that client's own flag, so
    // this clear cannot lose it.
    didDeferGCWorkSlot() = false;
    // FIXME: Bring back something like the DeferGCProbability mode.
    // https://bugs.webkit.org/show_bug.cgi?id=166627
    collectIfNecessaryOrDefer();
}

void Heap::registerWeakGCHashTable(WeakGCHashTable* weakGCHashTable)
{
    // SharedGC (CVE-AUDIT A3 / map-MC-GC S12b / K4.VIII.9): the registry is a
    // bare HashSet on the SHARED server heap; under GIL-off two lites running
    // JSGlobalObject::JSGlobalObject concurrently (each registers several
    // WeakGCMaps — StructureCache, transition tables, …) tear the rehash and
    // either SEGV (HashTable::removeIterator at 0x8, .crash.txt) or quietly
    // LOSE a registration so the conductor's prune never visits that table
    // (identity loss for liveness-keyed caches; map-MC-GC S12b). Leaf lock,
    // construction-/destruction-rate, gilOff-gated so flag-off stays
    // byte-identical.
    if (vm().gilOff()) [[unlikely]] {
        Locker locker { m_weakGCHashTablesLock };
        m_weakGCHashTables.add(weakGCHashTable);
        return;
    }
    m_weakGCHashTables.add(weakGCHashTable);
}

void Heap::unregisterWeakGCHashTable(WeakGCHashTable* weakGCHashTable)
{
    // SharedGC (CVE-AUDIT A3 / map-MC-GC S12b): see registerWeakGCHashTable.
    if (vm().gilOff()) [[unlikely]] {
        Locker locker { m_weakGCHashTablesLock };
        m_weakGCHashTables.remove(weakGCHashTable);
        return;
    }
    m_weakGCHashTables.remove(weakGCHashTable);
}

void Heap::didAllocateBlock(size_t capacity)
{
#if ENABLE(RESOURCE_USAGE)
    m_blockBytesAllocated.fetch_add(capacity, std::memory_order_relaxed); // F3.
#else
    UNUSED_PARAM(capacity);
#endif
}

void Heap::didFreeBlock(size_t capacity)
{
#if ENABLE(RESOURCE_USAGE)
    m_blockBytesAllocated.fetch_sub(capacity, std::memory_order_relaxed); // F3.
#else
    UNUSED_PARAM(capacity);
#endif
}

#if ENABLE(SAMPLING_PROFILER)
constexpr bool samplingProfilerSupported = true;
template<typename Visitor>
static ALWAYS_INLINE void visitSamplingProfiler(VM& vm, Visitor& visitor)
{
    SamplingProfiler* samplingProfiler = vm.samplingProfiler();
    if (samplingProfiler) [[unlikely]] {
        Locker locker { samplingProfiler->getLock() };
        samplingProfiler->processUnverifiedStackTraces();
        samplingProfiler->visit(visitor);
        if (Options::logGC() == GCLogging::Verbose)
            dataLog("Sampling Profiler data:\n", visitor);
    }
};
#else
constexpr bool samplingProfilerSupported = false;
static UNUSED_FUNCTION void visitSamplingProfiler(VM&, AbstractSlotVisitor&) { };
#endif

void Heap::addCoreConstraints()
{
    m_constraintSet->add(
        "Cs", "Conservative Scan",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this, lastVersion = static_cast<uint64_t>(0)] (auto& visitor) mutable {
            bool shouldNotProduceWork = lastVersion == m_phaseVersion;
            SuperSamplerScope superSamplerScope(false);

            // For the GC Verfier, we would like to use the identical set of conservative roots
            // as the real GC. Otherwise, the GC verifier may report false negatives due to
            // variations in stack values. For this same reason, we will skip this constraint
            // when we're running the GC verification in the End phase.
            if (shouldNotProduceWork || m_isMarkingForGCVerifier)
                return;
            
            TimingScope preConvergenceTimingScope(*this, "Constraint: conservative scan"_s);
            m_objectSpace.prepareForConservativeScan();
            m_jitStubRoutines->prepareForConservativeScan();

            {

                // We only want to do this when the mutator has the conn because that means we're under a safepoint.
                // If we tried to scan while not under a safepoint we could stop a thread that's in the process of calling
                // one of the callees we are looking for.
                // FIXME: Should we have two constraints for this? One for concurrent and one under safepoint at the bitter end.
                ASSERT(worldIsStopped());
                ConservativeRoots conservativeRoots(*this);

                gatherStackRoots(conservativeRoots);
                gatherVMRoots(conservativeRoots);

                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ConservativeScan);
                visitor.append(conservativeRoots);
                if (m_verifierSlotVisitor) [[unlikely]] {
                    SetRootMarkReasonScope rootScope(*m_verifierSlotVisitor, RootMarkReason::ConservativeScan);
                    m_verifierSlotVisitor->append(conservativeRoots);
                }
            }

            // JITStubRoutines must be visited after scanning ConservativeRoots since JITStubRoutines depend on the hook executed during gathering ConservativeRoots.
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::JITStubRoutines);
            m_jitStubRoutines->traceMarkedStubRoutines(visitor);
            if (m_verifierSlotVisitor) [[unlikely]] {
                // It's important to cast m_verifierSlotVisitor to an AbstractSlotVisitor here
                // so that we'll call the AbstractSlotVisitor version of traceMarkedStubRoutines().
                AbstractSlotVisitor& visitor = *m_verifierSlotVisitor;
                m_jitStubRoutines->traceMarkedStubRoutines(visitor);
            }
            lastVersion = m_phaseVersion;
        })),
        ConstraintVolatility::GreyedByExecution,
        // The scan excludes the executing thread from the suspend-and-copy
        // pass. heapHelperPool threads are never registered mutators, so
        // they may run it; a sibling-assist visitor (gilOff, admitted under
        // sharedGCMaxSiblingMarkingAssists) runs on a registered JS thread
        // whose own frames and registers would then never be scanned, so
        // with siblings admissible the scan runs on the conductor only.
        Options::sharedGCMaxSiblingMarkingAssists() ? ConstraintConcurrency::Sequential : ConstraintConcurrency::Concurrent);

    m_constraintSet->add(
        "Wlr", "Window Liveness Retention",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this, lastVersion = static_cast<uint64_t>(0)] (auto& visitor) mutable {
            // SPEC-heap I4/I5 — shared-server window-liveness retention
            // (EVIDENCE.md §10/§11). With N parked mutators, a cell allocated
            // in the pre-stop mutator window can be live with its ONLY
            // liveness witnesses being the step-5 flush's newlyAllocated
            // stamp (MarkedBlock::Handle::stopAllocating) or its block's
            // directory allocated bit: §10 Experiment B proved the conducted
            // cycle traced no heap edge to the cohort AND no parked thread's
            // copied stack/register image references it, so the legacy
            // invariant "every live cell is marked by end of marking" does
            // NOT hold under the N-mutator §10 protocol — and
            // Heap::endMarking() retires BOTH witnesses, after which an
            // IsEmpty quick sweep would rehand the still-held whole payload
            // to another thread (the §9 corruption chain). Append every
            // version-current-unmarked window-witnessed cell to the visitor
            // through the REAL conservative-root path
            // (appendJSCellOrAuxiliary), inside the marking fixpoint. JSCell
            // kinds are marked AND traced, so the retained JSCell set is
            // closed under tracing: weak sets and output constraints
            // re-converge over the retained cells, and every mark-keyed
            // registry (e.g. the transition WeakGCHashTables pruned after
            // endMarking) only ever observes CONSISTENT retained objects.
            // Auxiliary kinds (butterflies, Map/Set buffers) are marked only
            // — they carry no type information to trace, and their contents
            // are reached exclusively through an owning JSCell, which, if
            // live, is rooted or witnessed itself. Mark-without-trace was proven
            // unsound there: a retained zombie dictionary Structure with a
            // swept PropertyTable was re-adopted via the transition table
            // (objectmodel/i03-quarantine-readd-across-gc.js). Kind-agnostic
            // on purpose: the corruption-carrying cells are Auxiliary /
            // JSCellWithIndexingHeader (a JSCell-only walk was shown
            // insufficient, §7 Result 4 vs §10 Experiment A).
            //
            // Retention cost: the snapshot appends every version-current
            // newlyAllocated cell that marking has not reached (narrowing
            // this per cell was shown to reintroduce the under-marking hole).
            // In an eden cycle a retained dead window cell's mark is sticky,
            // so it stays marked through every subsequent eden cycle and is
            // reclaimable only at the next full collection; that floating
            // cohort is bounded by the retention-pressure full-collection
            // trigger in updateAllocationLimits. In a full collection the
            // block's marks are stale, so its prior-cycle survivors (which
            // stopAllocating newlyAllocated-stamped alongside the window
            // cells) are retained as well; tracing them is safe because their
            // referents were marked last cycle and this cycle's sweep has not
            // run.
            //
            // SOUNDNESS LEMMAS (round-6): unlike the stock conservative
            // scan, this pass dereferences cells (structureID read +
            // visitChildren) for which NO stopped thread published a pointer
            // value. That is sound because:
            //   L1 (publication): every lite's pre-park heap stores are
            //   sequenced-before its GCClient::Heap::releaseHeapAccess()
            //   seq_cst exchange to NoAccess ("RHA ... publishes all prior
            //   heap writes to the conductor (F6)", Heap.cpp), and
            //   conductSharedCollection()'s §10.4 access barrier seq_cst-
            //   samples every client's m_accessState under GBL and proceeds
            //   only when ALL are NoAccess — so every pre-park store
            //   happens-before constraint execution on the conductor; the
            //   chain extends to parallel marker helpers through the
            //   m_markingMutex / m_markingConditionVariable wakeup
            //   (lock release/acquire).
            //   L2 (no park mid-initialization): object allocation +
            //   initialization runs entirely under heap access; a lite's
            //   access is released only at park brackets / poll sites
            //   (GILDroppedSection, RHA/SINFAC polls,
            //   parkSitePollAndParkForStopTheWorld), never inside an
            //   allocation or initialization path, and the §10.4 barrier
            //   waits for NoAccess on every client — so
            //   worldIsStoppedForAllClients() implies no lite is
            //   mid-initialization. This is the same structural property
            //   stock conservative scanning already relies on when it
            //   dereferences arbitrary cell-aligned pointers into allocated
            //   blocks. Any new code that released heap access mid-
            //   initialization would break BOTH paths, not just this one.
            //
            // One execution per phase version suffices: the witness set is
            // fixed at stop time (no mutator runs, conductor allocation is
            // forbidden inside the stop window), marking never clears mark
            // bits, and the scan appends EVERY unmarked witness cell — later
            // convergence iterations cannot discover new witnesses.
            //
            // GATE RULING (round-7 F5): keyed on sticky ISS, NOT on a
            // gilOff-process check — deliberately. ISS is reachable on two
            // paths: the gilOff designation (VM.cpp) AND the GIL-ON
            // second-client trigger (HeapClientSet::add ->
            // noteSharedServerSticky when useSharedGCHeap && size > 1).
            // Once ISS, EVERY collection runs the same §10 conducted stop
            // protocol — stopAllocating NA stamping pre-stop, witness
            // retirement in endMarking — so the §10 window-witness hole is
            // a property of shared conduction itself, not of gilOff; a
            // GIL-on shared-heap run needs this retention for exactly the
            // same reason. The broader gate is also the conservative
            // direction (retains more, never less). Flag-off (default
            // options) identity is unaffected: ISS never flips without
            // useSharedGCHeap. Recorded in EVIDENCE.md §14. T4 UPDATE: ISS
            // remains the outer key, but inside it the pass now also
            // requires a SECOND attached client at execution time — see the
            // SINGLE-MUTATOR GATE comment in the executor below for the
            // soundness argument (no parked mutators with one client; the
            // count is frozen inside the stop window).
            if (!isSharedServer()) [[likely]]
                return;
            if constexpr (std::is_same_v<std::decay_t<decltype(visitor)>, SlotVisitor>) {
                // Round-7 F3: this pass dereferences cells (structureID +
                // visitChildren) for which NO stopped thread published a
                // pointer — its entire memory-safety argument (L1/L2) is
                // worldIsStoppedForAllClients(). A debug-only check is too
                // weak for the Release configuration all pinned verifies
                // run, and checkConn's Mutator arm deliberately tolerates a
                // legacy mutator-conn collection without WSAC; enforce at
                // the point of use. One load per phase version — free.
                RELEASE_ASSERT(worldIsStoppedForAllClients());
                // T4 SINGLE-MUTATOR GATE (narrows the round-7 F5 ruling; the
                // ISS key alone made a single-client GIL-off heap pay full
                // window retention for its entire process lifetime — the top
                // RSS retention mechanism AND the largest flag-tax mechanism
                // per the rss/flagtax profiles). The §10 window-witness hole
                // is a property of cells allocated by mutators that are
                // PARKED at stop time with their liveness reachable only
                // through per-thread state the trace cannot see (Experiment
                // B). With exactly ONE attached client, that client is the
                // conductor of this very cycle (conduction requires a
                // registered client; remove() cannot complete inside a stop
                // window, so the count is frozen while we run): there are no
                // parked mutators, the lone mutator's stack/registers flow
                // through m_currentThreadState into the stock conservative
                // scan, and every live window cell is rooted exactly as in
                // the legacy single-mutator protocol (the GIL-on W=1 escape
                // — same stopAllocating NA stamping, same endMarking witness
                // retirement, sound for decades). A client that attached and
                // detached entirely within the window needs no witness
                // either: its published cells are heap-traced; its handed-
                // out-but-unpublished cells died with its thread state (it
                // has no parked image); its un-consumed free-list remainder
                // was returned by its detach-side stopAllocating and is NA-
                // stamped-dead. Per-execution sampling (not a per-cycle
                // latch) is what keeps the windowed-marking flag sound: a
                // client that attaches BETWEEN stop windows and parks at a
                // later window is counted at that window's execution — and
                // the skip below deliberately does NOT update lastVersion,
                // so a later execution of the same phase version re-judges
                // with the then-current count (skipping can never mask a
                // pass that must run). ISS itself stays the outer key (F5's
                // conservative direction): flag-off identity is unaffected.
                // AMEND (audit): on the pinned bench
                // (Tools/threads/scalebench/js/bench.js) W=1 means ONE
                // attached client — the main shell thread is worker 0 and
                // the spawn loop runs for ids 1..W-1, i.e. zero spawned
                // Threads at W=1 — so this gate engages for the whole run
                // (an audit claim of main+1 worker = 2 clients was checked
                // against the spawn loop and is wrong). Whether the W=1
                // flag-tax/RSS win comes from THIS mechanism vs T4(b)/(c)/
                // (d) is still a bench question: confirm on the pinned
                // beat-or-explain run before attributing the headline.
                if (clientSet().size() <= 1)
                    return;
                if (lastVersion == m_phaseVersion)
                    return;
                lastVersion = m_phaseVersion;
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ConservativeScan);
                HeapVersion markingVersion = m_objectSpace.markingVersion();
                Vector<HeapCell*> candidates;
                size_t retainedBytes = 0; // T4: directly-appended witness bytes (closure excluded — an UNDERcount, which is the conservative direction for the updateAllocationLimits subtraction).
                auto visitWindowBlock = [&] (MarkedBlock::Handle* handle) {
                    // The witness judgment (NA version, marks staleness,
                    // per-cell bits) is one consistent snapshot under the
                    // block's header lock — the same lock aboutToMarkSlow
                    // holds while it clears/folds the bitmaps and bumps
                    // both versions concurrently on parallel marker
                    // helpers. Appends happen HERE, after the header lock
                    // is dropped (appendJSCellOrAuxiliary can retake it via
                    // aboutToMark).
                    candidates.shrink(0);
                    handle->block().sharedGCWindowWitnessSnapshot(markingVersion, candidates);
                    // Under the bitvector lock so the empty judge in
                    // BlockDirectory::endMarking() and any subsequent
                    // sweep classification observe it; an IsEmpty
                    // whole-payload rehand-out of a window-live block
                    // is thereby impossible. (appendJSCellOrAuxiliary
                    // only sets it via aboutToMarkSlow when the
                    // block's marks were stale — eden cycles need
                    // this explicit form.) Set unconditionally, not only
                    // when candidates is non-empty: a parked client's
                    // m_lastActiveBlock with ZERO surviving NA cells must
                    // STILL not be IsEmpty-recycled (resumeAllocating
                    // reinstates it as m_currentBlock regardless; a
                    // concurrent rehand would alias the handle across two
                    // clients — the §9 corruption chain).
                    {
                        BlockDirectory* directory = handle->directory();
                        Locker locker { directory->bitvectorLock() };
                        directory->setIsMarkingNotEmpty(handle, true);
                    }
                    if (!candidates.isEmpty()) {
                        retainedBytes += candidates.size() * handle->cellSize();
                        for (HeapCell* cell : candidates)
                            visitor.appendJSCellOrAuxiliary(cell);
                    }
                };
                // Visit only the blocks ACTUALLY HELD by parked mutators at
                // this stop — each registered client's per-allocator
                // m_lastActiveBlock (the block the §10 step-5
                // stopAllocating() flush NA-stamped) — instead of
                // m_objectSpace.forEachBlock(), which paid O(heap blocks)
                // per cycle and retained every cell of every block consumed
                // this window.
                //
                // SOUNDNESS RE-DERIVATION (against L1/L2 above; required by
                // the T2 ruling): the §10 window-witness hole — and the §9
                // corruption chain it feeds — is a property of a PARKED
                // mutator's m_lastActiveBlock ONLY. After step 5 every
                // LocalAllocator has m_currentBlock == nullptr and
                // m_lastActiveBlock == (the block it was bump-allocating
                // from at park time, or nullptr if its free list was already
                // exhausted). resumeAllocating() reinstates exactly that
                // block as m_currentBlock and re-derives its free list by
                // SWEEPING (MarkedBlock::Handle::resumeAllocating ->
                // sweep(&freeList)): an unmarked NA-stamped cell would be
                // free-listed and rehanded out (double-allocation), and a
                // block with no marks would be IsEmpty-judged in
                // BlockDirectory::endMarking() and rehanded whole to another
                // client while the parked owner still holds it. So this pass
                // MUST mark every NA-stamped cell of every client's
                // m_lastActiveBlock and set its markingNotEmpty bit — and
                // that suffices: a block NOT any client's m_lastActiveBlock
                // at stop time was either fully consumed before the park
                // (LocalAllocator::didConsumeFreeList: m_currentBlock <-
                // null, inUse cleared via didFinishUsingBlock) or returned
                // by a detached client's stopAllocatingForGood; no client
                // resumes into it, so it carries no §9 obligation. Its
                // window-allocated cells are fully constructed (L2: no
                // client parks mid-initialization) with all outgoing edges
                // published to the conductor (L1), and gatherStackRoots()
                // scans EVERY registered thread's saved stack/register image
                // (m_machineThreads, §10.6/I4(b)) — so each such cell is
                // either (i) reachable from a heap root or a parked stack
                // image and marked by the Cs/Msr fixpoint, or (ii)
                // reachable only from an NA-stamped cell of some client's
                // m_lastActiveBlock and marked by THIS pass's traced
                // closure (appendJSCellOrAuxiliary -> visitChildren), or
                // (iii) unreachable, i.e. dead and correctly reclaimed.
                // There is no fourth case; in particular Experiment B's
                // "no heap edge AND no stack-image reference" cohort lives
                // in (iii) for consumed blocks (it was the m_lastActiveBlock
                // resume-sweep that made retaining it load-bearing, and
                // consumed blocks have no resume-sweep). A visited block is
                // never in its directory's allocated set
                // (MarkedBlock::Handle::stopAllocating ASSERTs
                // !isAllocated(this)), so sharedGCWindowWitnessSnapshot
                // judges the newlyAllocated witness alone.
                //
                // Locking: clientSet().forEach()'s precondition is met (we
                // are the conductor with worldIsStoppedForAllClients() —
                // RELEASE_ASSERTed above); the registry is frozen inside the
                // stop window (I13 add/remove sides), so the walk sees
                // exactly the parked-client set. Each client's
                // GCThreadLocalCache::m_perDirectory and each allocator's
                // m_lastActiveBlock are owner-thread-mutated outside the
                // stop window and frozen inside it (I2 exception). The
                // per-block body takes only the block header lock and the
                // directory bitvector lock — same lock surface as the
                // campaign-1 forEachBlock body, both taken inside
                // clientSet().forEach() at step-8 already (resumeAllocating
                // -> sweep), so no new lock-order edge.
                clientSet().forEach([&](GCClient::Heap& client) {
                    client.threadLocalCache().forEachLocalAllocator([&](LocalAllocator* allocator) {
                        if (MarkedBlock::Handle* handle = allocator->lastActiveBlock())
                            visitWindowBlock(handle);
                    });
                });
                // Precise-allocation leg (closes EVIDENCE.md §11 residual 1):
                // MarkedSpace::endMarking() also retires the per-allocation
                // newlyAllocated witness, so a window-allocated PRECISE cell
                // (> largeCutoff Map storage / butterfly) with the same
                // no-root profile would be freed by the sweep. Same witness,
                // same retention, same trace closure.
                for (PreciseAllocation* allocation : m_objectSpace.preciseAllocations()) {
                    if (allocation->isNewlyAllocated() && !allocation->isMarked()) {
                        retainedBytes += allocation->cellSize();
                        visitor.appendJSCellOrAuxiliary(allocation->cell());
                    }
                }
                // T4(c): feed the threshold logic. Conductor-written inside
                // the stop window (one constraint executor at a time), reset
                // world-stopped in willStartCollection, consumed world-
                // stopped in updateAllocationLimits — never touched with the
                // world running. Accumulates across phase versions of one
                // cycle (full collections can legitimately run this twice).
                m_sharedGCWindowRetainedBytesThisCycle += retainedBytes;
            } else {
                // GC-verifier mirror (AbstractSlotVisitor): skipped, like the
                // conservative scan — the verifier has no window witnesses to
                // re-derive (the real fixpoint already marked the cohort).
                UNUSED_PARAM(visitor);
            }
        })),
        ConstraintVolatility::GreyedByExecution,
        // TSAN-DEEP-01: Wlr reads m_objectSpace.preciseAllocations() while the
        // Concurrent "Cs" constraint std::sorts that same storage in place
        // (MarkedSpace::prepareForConservativeScan), so a parallel pass can
        // skip entries mid-sort and leave a window-allocated precise cell
        // un-retained. The visitWindowBlock leg likewise touches per-block /
        // per-directory state that other Concurrent constraints touch. Wlr's
        // own L1/L2 soundness comment above already assumes "one constraint
        // executor at a time"; Sequential makes that premise true (matches the
        // Pbc precedent below).
        ConstraintConcurrency::Sequential);

    m_constraintSet->add(
        "Msr", "Misc Small Roots",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
            // SharedGC (T9): conductor-context OK — marking constraints run
            // on the conductor/parallel markers inside the stop window (I5);
            // vm() here (and in the Sh/D/Jw constraints below) names the one
            // main VM's global roots (smallStrings, exceptions, aggregates,
            // profilers, worklist key). Post-GIL per-THREAD roots (exception
            // state, top call frame) move to VMLite; if they become
            // per-client they must be added as clientSet().forEach() visits
            // here (deviation-8 charter) — phase 1 there are none.
            VM& vm = this->vm();
#if JSC_OBJC_API_ENABLED
            {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ExternalRememberedSet);
                scanExternalRememberedSet(vm, visitor);
            }
#endif

            {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::StrongReferences);
                if (vm.smallStrings.needsToBeVisited(*m_collectionScope))
                    vm.smallStrings.visitStrongReferences(visitor);
#if USE(BUN_JSC_ADDITIONS)
                if (vm.clientData) {
                    if (auto* table = vm.clientData->decoderStringTable())
                        table->visitStrongReferences(visitor, m_collectionScope.value_or(CollectionScope::Full));
                }
#endif
            }
            
            {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ProtectedValues);
                for (auto& pair : m_protectedValues)
                    visitor.appendUnbarriered(pair.key);
            }

            if (Options::useJSThreads()) [[unlikely]] {
                // SPEC-jit §5.8/§4.4: retired call-link records' named
                // CodeBlocks (see pinRetiredCallLinkRecordCodeBlock). Each
                // entry is marked only while codeBlockSet() still vouches for
                // the address being a not-yet-dead CodeBlock cell: a pinned
                // cell the GC already declared dead (removed at the end of
                // the cycle that unmarked it) is skipped — pins retain, they
                // never resurrect. A recycled slot re-added as a NEW
                // CodeBlock over-marks a live cell, which is benign. Lock
                // order: pin lock, then the codeBlockSet lock inside it;
                // mutator pin/unpin take only the pin lock, so no inversion. This
                // constraint re-runs to fixpoint (GreyedByExecution), so a
                // record retired during this cycle's concurrent marking has
                // its pin appended before the final fixpoint closes.
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::StrongReferences);
                Locker pinLocker { m_retiredCallLinkRecordCodeBlocksLock };
                Locker setLocker { m_codeBlocks->getLock() };
                for (auto& pair : m_retiredCallLinkRecordCodeBlocks) {
                    if (m_codeBlocks->contains(setLocker, pair.key))
                        visitor.appendUnbarriered(static_cast<JSCell*>(pair.key));
                }
            }

            if (Options::useSharedGCHeap()) [[unlikely]] {
                // DW-2: shared mode keeps MarkedVector registrations in the
                // per-shard locked sets (markListSetShard()); m_markListSet
                // stays empty. Mutators with heap access are quiesced inside
                // the stop window (I5), so these locks are uncontended from
                // them — but a thread running native code WITHOUT heap
                // access may still destroy a MarkedVector concurrently, and
                // ~MarkedVectorBase removes from the shard and frees the
                // spill buffer under this same lock, so holding it across
                // the whole shard walk is what makes markLists' unlocked
                // m_size/m_buffer reads safe against that teardown.
                for (auto& shard : m_markListSetShards) {
                    Locker locker { shard.lock };
                    if (shard.set.isEmpty())
                        continue;
                    SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ConservativeScan);
                    MarkedVectorBase::markLists(visitor, shard.set);
                }
            } else if (!m_markListSet.isEmpty()) {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ConservativeScan);
                MarkedVectorBase::markLists(visitor, m_markListSet);
            }

            {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::VMExceptions);
                if (vm.gilOff()) [[unlikely]] {
                    // UNGIL §A.1.3 GC roots (r6 F5, NORMATIVE; U-T1): post
                    // Group-3 rerouting, vm.exception()/vm.lastException()
                    // resolve through the CURRENT lite — wrong on a GC visit
                    // thread (conductor/marker). The shared collection's
                    // root visit instead iterates the VMLiteRegistry under
                    // its lock and appends EVERY registered same-VM lite's
                    // exception cells (per-VM filter). The registry is
                    // stable here: mutators are quiesced by the heap §10
                    // stop. Amplifier arms (IU): a thrower parked pre-catch
                    // must survive a forced full collection; two-VM arm.
                    auto& registry = VMLiteRegistry::singleton();
                    Locker locker { registry.lock };
                    for (VMLite* lite : registry.lites) {
                        if (lite->vm != &vm)
                            continue;
                        visitor.appendUnbarriered(lite->primitives.m_exception);
                        visitor.appendUnbarriered(lite->primitives.m_lastException);
                    }
                } else {
                    visitor.appendUnbarriered(vm.exception());
                    visitor.appendUnbarriered(vm.lastException());
                }

                // We're going to m_terminationException directly instead of going through
                // the exception() getter because we want to assert in the getter that the
                // TerminationException has been reified. Here, we don't care if it is
                // reified or not.
                // UNGIL r6 F5: m_terminationException stays VM-global (a
                // per-VM singleton, not per-thread Group-3 state) — rooted
                // here in BOTH modes.
                visitor.appendUnbarriered(WTF::atomicLoad(&vm.m_terminationException, std::memory_order_relaxed)); // THREADS: concurrent marker vs lazy creation.
            }
        })),
        ConstraintVolatility::GreyedByExecution);
    
    m_constraintSet->add(
        "Sh", "Strong Handles",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::StrongHandles);
            m_strongSet.visitAggregate(visitor);
            // SharedGC (T9): conductor-context OK — see the Msr constraint.
            vm().visitAggregate(visitor);
        })),
        ConstraintVolatility::GreyedByExecution);
    
    m_constraintSet->add(
        "D", "Debugger",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::Debugger);

            // SharedGC (T9): conductor-context OK — see the Msr constraint.
            VM& vm = this->vm();
            if constexpr (samplingProfilerSupported)
                visitSamplingProfiler(vm, visitor);

            if (vm.typeProfiler())
                vm.typeProfilerLog()->visit(visitor);
            
            if (auto* shadowChicken = vm.shadowChicken())
                shadowChicken->visitChildren(visitor);
        })),
        ConstraintVolatility::GreyedByExecution);
    
    m_constraintSet->add(
        "Ws", "Weak Sets",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::WeakSets);
            RefPtr<SharedTask<void(decltype(visitor)&)>> task = m_objectSpace.forEachWeakInParallel<decltype(visitor)>(visitor);
            visitor.addParallelConstraintTask(WTF::move(task));
        })),
        ConstraintVolatility::GreyedByMarking,
        ConstraintParallelism::Parallel);
    
    m_constraintSet->add(
        "O", "Output",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([] (auto& visitor) {
            JSC::Heap* heap = visitor.heap();

            auto callOutputConstraint = [] (auto& visitor, HeapCell* heapCell, HeapCell::Kind) {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::Output);
                JSCell* cell = static_cast<JSCell*>(heapCell);
                cell->methodTable()->visitOutputConstraints(cell, visitor);
            };
            
            auto add = [&] (auto& set) {
                RefPtr<SharedTask<void(decltype(visitor)&)>> task = set.template forEachMarkedCellInParallel<decltype(visitor)>(callOutputConstraint);
                visitor.addParallelConstraintTask(WTF::move(task));
            };

            {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ExecutableToCodeBlockEdges);
                add(heap->functionExecutableSpaceAndSet.outputConstraintsSet);
                add(heap->programExecutableSpaceAndSet.outputConstraintsSet);
                if (heap->m_evalExecutableSpace)
                    add(heap->m_evalExecutableSpace->outputConstraintsSet);
                if (heap->m_moduleProgramExecutableSpace)
                    add(heap->m_moduleProgramExecutableSpace->outputConstraintsSet);
            }
            if (heap->m_weakMapSpace) {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::WeakMapSpace);
                add(*heap->m_weakMapSpace);
            }
        })),
        ConstraintVolatility::GreyedByMarking,
        ConstraintParallelism::Parallel);

#if ENABLE(WEBASSEMBLY)
    m_constraintSet->add(
        "Pbc", "Pinball Completions",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
            // FIXME: Unlike the "Cs" constraint which is skipped during verification
            // because conservative roots are not stable, this skip is only here because
            // ConservativeRoots::genericAddPointer asserts isMarking(), which doesn't
            // hold during verification. This constraint could run always otherwise, but
            // that would require rethinking the assumptions in ConservativeRoots.
            if (m_isMarkingForGCVerifier)
                return;
            IsoSubspace* subspace = m_pinballCompletionSpace.get();
            if (!subspace)
                return;
            ASSERT(worldIsStopped());
            // ConservativeRoots gathering requires an up-to-date precise allocations snapshot.
            m_objectSpace.prepareForConservativeScan();
            // FIXME: Add a second CellState for PinballCompletion so we can skip
            // pinballs whose conservative roots have already been gathered this cycle.
            ConservativeRoots conservativeRoots(*this);
            subspace->forEachMarkedCell([&](HeapCell* cell, HeapCell::Kind) {
                auto* pinball = uncheckedDowncast<PinballCompletion>(static_cast<JSCell*>(cell));
                pinball->gatherConservativeRoots(conservativeRoots);
            });
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::PinballCompletionConservativeRoots);
            visitor.append(conservativeRoots);
        })),
        ConstraintVolatility::GreyedByMarking,
        ConstraintConcurrency::Sequential);
#endif

#if ENABLE(JIT)
    if (Options::useJIT()) {
        m_constraintSet->add(
            "Jw", "JIT Worklist",
            MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::JITWorkList);

                JITWorklist::ensureGlobalWorklist().visitWeakReferences(visitor);
                
                // FIXME: This is almost certainly unnecessary.
                // https://bugs.webkit.org/show_bug.cgi?id=166829
                // SharedGC (T9): conductor-context OK — see the Msr
                // constraint; vm() = worklist key.
                JITWorklist::ensureGlobalWorklist().iterateCodeBlocksForGC(visitor,
                    vm(),
                    [&] (CodeBlock* codeBlock) {
                        visitor.appendUnbarriered(codeBlock);
                    });
                
                if (Options::logGC() == GCLogging::Verbose)
                    dataLog("JIT Worklists:\n", visitor);
            })),
            ConstraintVolatility::GreyedByMarking);
    }
#endif
    
    m_constraintSet->add(
        "Cb", "CodeBlocks",
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::CodeBlocks);
            iterateExecutingAndCompilingCodeBlocksWithoutHoldingLocks(visitor,
                [&] (CodeBlock* codeBlock) {
                    // Visit the CodeBlock as a constraint only if it's black.
                    if (visitor.isMarked(codeBlock)
                        && codeBlock->cellState() == CellState::PossiblyBlack)
                        visitor.visitAsConstraint(codeBlock);
                });
        })),
        ConstraintVolatility::SeldomGreyed);
    
    m_constraintSet->add(makeUnique<MarkStackMergingConstraint>(*this));
}

void Heap::addMarkingConstraint(std::unique_ptr<MarkingConstraint> constraint)
{
    PreventCollectionScope preventCollectionScope(*this);
    m_constraintSet->add(WTF::move(constraint));
}

void Heap::notifyIsSafeToCollect()
{
    if (!Options::useGC()) [[unlikely]]
        return;

    MonotonicTime before;
    if (Options::logGC()) [[unlikely]] {
        before = MonotonicTime::now();
        dataLog("[GC<", RawPointer(this), ">: starting ");
    }
    
    addCoreConstraints();
    
    m_isSafeToCollect = true;
    
    if (Options::collectContinuously()) {
        m_collectContinuouslyThread = Thread::create(
            "JSC DEBUG Continuous GC"_s,
            [this] () {
                MonotonicTime initialTime = MonotonicTime::now();
                Seconds period = Seconds::fromMilliseconds(Options::collectContinuouslyPeriodMS());
                while (true) {
                    Locker locker { m_collectContinuouslyLock };
                    {
                        Locker locker { *m_threadLock };
                        if (m_requests.isEmpty()) {
                            // SharedGC (§10B.1/§10B.3, I15): once shared, the
                            // continuous-GC timer is just another async
                            // trigger and must follow the shared ticketing
                            // protocol (requestCollectionShared): grant the
                            // ticket with the conn bit set idempotently
                            // (asserting only !m_collectorThreadIsRunning)
                            // and never notify m_threadCondition — the
                            // collector thread is quiesced once shared and
                            // would trip the §10B.3 quiescence assert in
                            // shouldCollectInCollectorThread() on a ticket
                            // granted without the conn bit. The ticket is
                            // served by the next conductor: a sync
                            // requester's §10.2 election or a mutator's
                            // stopIfNecessaryForAllClients() poll.
                            if (isSharedServer()) [[unlikely]] {
                                ASSERT(!m_collectorThreadIsRunning);
                                m_worldState.exchangeOr(mutatorHasConnBit);
                                m_requests.append(std::nullopt);
                                m_lastGrantedTicket++;
                            } else {
                                m_requests.append(std::nullopt);
                                m_lastGrantedTicket++;
                                m_threadCondition->notifyOne(locker);
                            }
                        }
                    }
                    
                    Seconds elapsed = MonotonicTime::now() - initialTime;
                    Seconds elapsedInPeriod = elapsed % period;
                    MonotonicTime timeToWakeUp =
                        initialTime + elapsed - elapsedInPeriod + period;
                    while (!hasElapsed(timeToWakeUp) && !m_shouldStopCollectingContinuously) {
                        m_collectContinuouslyCondition.waitUntil(
                            m_collectContinuouslyLock, timeToWakeUp);
                    }
                    if (m_shouldStopCollectingContinuously)
                        break;
                }
            }, ThreadType::GarbageCollection);
    }
    
    dataLogIf(Options::logGC(), (MonotonicTime::now() - before).milliseconds(), "ms]\n");
}

// Use WTF_IGNORES_THREAD_SAFETY_ANALYSIS because this function conditionally locks m_collectContinuouslyLock,
// which is not supported by analysis.
void Heap::preventCollection() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    if (!m_isSafeToCollect)
        return;
    
    // This prevents the collectContinuously thread from starting a collection.
    m_collectContinuouslyLock.lock();

    // Wait for all collections to finish.
    //
    // W16-C1 residual (b) — N-mutator semantics once shared: the legacy
    // "wait until served == granted, then nothing can start" postcondition
    // silently relied on the single-mutator protocol (only this thread and
    // the CIND timer could request, and the timer is excluded by
    // m_collectContinuouslyLock above). Once ISS, OTHER mutators can
    // ticket (requestCollectionShared) at any time, so served == granted
    // is neither achievable under churn nor sufficient after return.
    // Instead: raise m_sharedGCPreventCount (the conduct-tenure gate read
    // by the §10.2 election winner arm and
    // tryConductSharedCollectionForPoll(), both under *m_threadLock — the
    // only two sites that START a shared collection), then wait for any
    // in-flight cycle to fully finish (!GCA && phase == NotRunning).
    // Tickets granted meanwhile sit unserved until allowCollection(), except
    // that the holder itself (m_sharedGCPreventHolder) is exempt from the
    // gate: its own collectNow(Sync) must conduct, as it did in legacy mode.
    // The gate is raised INSIDE the func (under *m_threadLock, first
    // iteration) and re-checked per iteration, so a mid-wait legacy->shared
    // flip still raises it before any shared-mode predicate is trusted.
    // The waitForCollector ISS branch keeps us stop-cooperative (SINFAC)
    // while the in-flight cycle drains, so this cannot deadlock against a
    // conductor that needs this client stopped. Raise tracking for
    // allowCollection() uses m_sharedGCPreventGateRaised (guarded by
    // m_collectContinuouslyLock, which we hold until allowCollection()) so
    // a §10D reversion between prevent and allow cannot leak a raised gate.
    waitForCollector(
        [&] (const AbstractLocker&) -> bool {
            ASSERT(m_lastServedTicket <= m_lastGrantedTicket);
            if (isSharedServer()) [[unlikely]] {
                if (!m_sharedGCPreventGateRaised) {
                    RELEASE_ASSERT(!m_sharedGCPreventCount); // Holders serialize on m_collectContinuouslyLock.
                    m_sharedGCPreventCount = 1;
                    m_sharedGCPreventHolder = &Thread::currentSingleton();
                    m_sharedGCPreventHolderTicket = 0;
                    m_sharedGCPreventGateRaised = true;
                }
                return !m_gcConductorActive && m_currentPhase == CollectorPhase::NotRunning;
            }
            return m_lastServedTicket == m_lastGrantedTicket;
        });

    // Now a collection can only start if this thread starts it.
    RELEASE_ASSERT(!m_collectionScope);
}

// Use WTF_IGNORES_THREAD_SAFETY_ANALYSIS because this function conditionally unlocks m_collectContinuouslyLock,
// which is not supported by analysis.
void Heap::allowCollection() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    if (!m_isSafeToCollect)
        return;

    // W16-C1 residual (b): drop the shared conduct-tenure gate if this
    // prevent scope raised it. Keyed on m_sharedGCPreventGateRaised (guarded
    // by m_collectContinuouslyLock, still held here) rather than
    // isSharedServer(): a §10D reversion between prevent and allow must not
    // leak a raised gate into a later re-flip era, and the legacy flag-off
    // path must not grow a *m_threadLock acquisition. GEC notifyAll wakes
    // prevented election followers / ISS waitForCollector waiters promptly
    // (their waits are timed, so this is latency, not correctness).
    if (m_sharedGCPreventGateRaised) [[unlikely]] {
        {
            Locker locker { *m_threadLock };
            RELEASE_ASSERT(m_sharedGCPreventCount == 1);
            RELEASE_ASSERT(m_sharedGCPreventHolder == &Thread::currentSingleton());
            m_sharedGCPreventCount = 0;
            m_sharedGCPreventHolder = nullptr;
            m_sharedGCPreventHolderTicket = 0;
            m_gcElectionCondition.notifyAll();
        }
        m_sharedGCPreventGateRaised = false;
    }

    m_collectContinuouslyLock.unlock();
}

void Heap::setMutatorShouldBeFenced(bool value)
{
    // SharedGC (§10B.5, T5b): always-fenced once shared. beginMarking() sets
    // the fence and endMarking() would normally drop it back to
    // Options::forceFencedBarrier(); with N mutators the store-ordering the
    // fence provides must hold at all times, so the drop is suppressed.
    // noteSharedServerSticky() raises the fence at the ISS flip; after a §10D
    // reversion the next legacy cycle's endMarking() restores today's value.
    //
    // SPEC-congc §5.3(1)/(3) (CG-2): once the C1 stage flag
    // (useConcurrentSharedGCMarking) is on AND the process is NOT GIL-off,
    // the forcing drops — the master pair becomes a real in-window
    // raise/lower, republished to every client's §5.3(2) copy before the
    // window closes. GIL-off C1+ KEEPS the forcing (F19; the §5.3(3) JIT
    // address pin): emitted code and the inline C++ barrier read ONLY the
    // SERVER pair — ANNEX CGD2.2 is the complete reader table (baked
    // addressOfBarrierThreshold / addressOfMutatorShouldBeFenced
    // AbsoluteAddresses in AssemblyHelpers, the branchIfBarriered VM-offset
    // load, the FTL VM_heap_barrierThreshold / VM_heap_mutatorShouldBeFenced
    // AbstractHeap offsets) — so until the §13.3(a) per-client JIT address
    // reroute lands (chartered to the jit/ungil owners), dropping the server
    // forcing GIL-off would under-fence every JIT store from the first
    // endMarking lower onward. Master pinned => copies tautological; FEP
    // stays at the raise (CG-I3). Flag-off (!useConcurrentSharedGCMarking):
    // the landed always-fenced forcing, byte-for-byte (CG-I0/CGD4.4).
    if (isSharedServer()) [[unlikely]] {
        if (!Options::useConcurrentSharedGCMarking() || VM::isGILOffProcess())
            value = true;
        // §5.3(1) FEP bump (release) per master mutation; the WND-close
        // republish loop stamps every client's m_fenceEpochSeen with it.
        m_barrierFenceEpoch.exchangeAdd(1, std::memory_order_release);
    }
    m_mutatorShouldBeFenced = value;
    m_barrierThreshold = value ? tautologicalThreshold : blackThreshold;
}

void Heap::performIncrement(size_t bytes)
{
    // SharedGC (§5.4/deviation 4): the incremental-marking mutator assist is
    // disabled in shared mode — marking only happens inside the conducted
    // stop (I5), so there is never an active marking phase to assist here and
    // m_incrementBalance stays a plain double. The mutator SlotVisitor is
    // used only while the world is stopped for all clients once shared
    // (T8 audits assert this).
    if (isSharedServer()) [[unlikely]]
        return;

    if (!m_objectSpace.isMarking())
        return;

    if (isDeferred())
        return;

    m_incrementBalance += bytes * Options::gcIncrementScale();

    // Save ourselves from crazy. Since this is an optimization, it's OK to go back to any consistent
    // state when the double goes wild.
    if (std::isnan(m_incrementBalance) || std::isinf(m_incrementBalance))
        m_incrementBalance = 0;
    
    if (m_incrementBalance < static_cast<double>(Options::gcIncrementBytes()))
        return;

    double targetBytes = m_incrementBalance;
    if (targetBytes <= 0)
        return;
    targetBytes = std::min(targetBytes, Options::gcIncrementMaxBytes());

    SlotVisitor& visitor = *m_mutatorSlotVisitor;
    ParallelModeEnabler parallelModeEnabler(visitor);
    size_t bytesVisited = visitor.performIncrementOfDraining(static_cast<size_t>(targetBytes));
    // incrementBalance may go negative here because it'll remember how many bytes we overshot.
    m_incrementBalance -= bytesVisited;
}

void Heap::addGCCompletionCallback(const GCCompletionCallback& callback)
{
    m_gcCompletionCallbacks.append(callback);
}

void Heap::removeGCCompletionCallback(const GCCompletionCallback& callback)
{
    m_gcCompletionCallbacks.removeFirst(callback);
}

void Heap::runTaskInParallel(RefPtr<SharedTask<void(SlotVisitor&)>> task)
{
    unsigned initialRefCount = task->refCount();
    {
        Locker locker { m_markingMutex };
        m_bonusVisitorTask = task;
        m_markingConditionVariable.notifyAll();
    }

    task->run(*m_collectorSlotVisitor);

    {
        Locker locker { m_markingMutex };
        m_bonusVisitorTask = nullptr;

        // The constraint solver expects return of this function to imply termination of the task in all
        // threads. This ensures that property.
        while (task->refCount() > initialRefCount)
            m_bonusVisitorTaskConditionVariable.wait(m_markingMutex);
    }
}

void Heap::verifierMark()
{
    RELEASE_ASSERT(!m_isMarkingForGCVerifier);

    SetForScope isMarkingForGCVerifierScope(m_isMarkingForGCVerifier, true);
    VerifierSlotVisitor& visitor = *m_verifierSlotVisitor;
    do {
        while (!visitor.isEmpty())
            visitor.drain();
        m_constraintSet->executeAllSynchronously(visitor);
        visitor.executeConstraintTasks();
    } while (!visitor.isEmpty());

    visitor.setDoneMarking();
}

void Heap::dumpVerifierMarkerData(HeapCell* cell)
{
    if (!Options::verifyGC())
        return;

    if (!Heap::isMarked(cell)) {
        dataLogLn("\n" "GC Verifier: cell ", RawPointer(cell), " was not marked by SlotVisitor");
        return;
    }

    // Use VerifierSlotVisitorScope to keep it live.
    RELEASE_ASSERT(m_verifierSlotVisitor && !m_isMarkingForGCVerifier);
    VerifierSlotVisitor& visitor = *m_verifierSlotVisitor;
    RELEASE_ASSERT(visitor.doneMarking());

    if (!visitor.isMarked(cell)) {
        dataLogLn("\n" "GC Verifier: ERROR cell ", RawPointer(cell), " was not marked by VerifierSlotVisitor");
        return;
    }

    dataLogLn("\n" "GC Verifier: Found marked cell ", RawPointer(cell), " with MarkerData:");
    visitor.dumpMarkerData(cell);
}

void Heap::verifyGC()
{
    RELEASE_ASSERT(m_verifierSlotVisitor);
    verifierMark();
    VerifierSlotVisitor& visitor = *m_verifierSlotVisitor;
    RELEASE_ASSERT(visitor.doneMarking() && !m_isMarkingForGCVerifier);

    visitor.forEachLiveCell([&] (HeapCell* cell) {
        if (Heap::isMarked(cell))
            return;

        dataLogLn("\n" "GC Verifier: ERROR cell ", RawPointer(cell), " was not marked");
        if (Options::verboseVerifyGC()) [[unlikely]]
            visitor.dumpMarkerData(cell);
        RELEASE_ASSERT(this->isMarked(cell));
    });

    if (!m_keepVerifierSlotVisitor)
        clearVerifierSlotVisitor();
}

void Heap::setKeepVerifierSlotVisitor() { m_keepVerifierSlotVisitor = true; }

void Heap::clearVerifierSlotVisitor()
{
    m_verifierSlotVisitor = nullptr;
    m_keepVerifierSlotVisitor = false;
}

void Heap::scheduleOpportunisticFullCollection()
{
    m_shouldDoOpportunisticFullCollection = true;
}

#define DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW(name, heapCellType, type) \
    IsoSubspace* Heap::name##Slow() \
    { \
        ASSERT(!m_##name); \
        auto space = makeUnique<IsoSubspace> ISO_SUBSPACE_INIT(*this, heapCellType, type); \
        WTF::storeStoreFence(); \
        m_##name = WTF::move(space); \
        return m_##name.get(); \
    }

FOR_EACH_JSC_DYNAMIC_ISO_SUBSPACE(DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW)

#undef DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW

#define DEFINE_DYNAMIC_SPACE_AND_SET_MEMBER_SLOW(name, heapCellType, type, spaceType) \
    IsoSubspace* Heap::name##Slow() \
    { \
        ASSERT(!m_##name); \
        auto space = makeUnique<spaceType> ISO_SUBSPACE_INIT(*this, heapCellType, type); \
        WTF::storeStoreFence(); \
        m_##name = WTF::move(space); \
        return &m_##name->space; \
    }

DEFINE_DYNAMIC_SPACE_AND_SET_MEMBER_SLOW(evalExecutableSpace, destructibleCellHeapCellType, EvalExecutable, Heap::ScriptExecutableSpaceAndSets) // Hash:0x958e3e9d
DEFINE_DYNAMIC_SPACE_AND_SET_MEMBER_SLOW(moduleProgramExecutableSpace, destructibleCellHeapCellType, ModuleProgramExecutable, Heap::ScriptExecutableSpaceAndSets) // Hash:0x6506fa3c

#undef DEFINE_DYNAMIC_SPACE_AND_SET_MEMBER_SLOW

#define DEFINE_DYNAMIC_NON_ISO_SUBSPACE_MEMBER_SLOW(name, heapCellType, type, SubspaceType) \
    SubspaceType* Heap::name##Slow() \
    { \
        ASSERT(!m_##name); \
        auto space = makeUnique<SubspaceType>(ASCIILiteral(#SubspaceType " " #name), *this, heapCellType, fastMallocAllocator.get()); \
        WTF::storeStoreFence(); \
        m_##name = WTF::move(space); \
        return m_##name.get(); \
    }

FOR_EACH_JSC_WEBASSEMBLY_DYNAMIC_NON_ISO_SUBSPACE(DEFINE_DYNAMIC_NON_ISO_SUBSPACE_MEMBER_SLOW)
#undef DEFINE_DYNAMIC_NON_ISO_SUBSPACE_MEMBER_SLOW

#if ENABLE(WEBASSEMBLY)

void Heap::reportWasmCalleePendingDestruction(Ref<Wasm::Callee>&& callee)
{
    void* boxedCallee = CalleeBits::boxNativeCallee(callee.ptr());
    // This better be true or we won't find the callee in ConservativeRoots.
    ASSERT_UNUSED(boxedCallee, boxedCallee == removeArrayPtrTag(boxedCallee));

    Locker locker(m_wasmCalleesPendingDestructionLock);
    m_wasmCalleesPendingDestruction.add(WTF::move(callee));
}

bool Heap::isWasmCalleePendingDestruction(Wasm::Callee& callee)
{
    Locker locker(m_wasmCalleesPendingDestructionLock);
    return m_wasmCalleesPendingDestruction.contains(callee);
}

bool Heap::didDiscoverPendingWasmCallee(Wasm::Callee* callee)
{
    if (!m_wasmCalleesPendingDestructionSnapshot.contains(callee))
        return false;
    m_wasmCalleesDiscoveredDuringGC.add(callee);
    return true;
}

void Heap::prepareWasmCalleeCleanup()
{
    ASSERT(worldIsStopped());
    ASSERT(m_wasmCalleesPendingDestructionSnapshot.isEmpty());
    ASSERT(m_wasmCalleesDiscoveredDuringGC.isEmpty());
    m_wasmCalleesPendingDestructionSnapshot.clear();
    m_wasmCalleesDiscoveredDuringGC.clear();
    m_boxedWasmCalleeFilter = TinyBloomFilter<uintptr_t>();

    Locker locker(m_wasmCalleesPendingDestructionLock);
    for (auto& callee : m_wasmCalleesPendingDestruction) {
        m_wasmCalleesPendingDestructionSnapshot.add(callee.ptr());
        m_boxedWasmCalleeFilter.add(std::bit_cast<uintptr_t>(CalleeBits::boxNativeCallee(callee.ptr())));
    }
}

void Heap::finalizeWasmCalleeCleanup()
{
    ASSERT(worldIsStopped());
    if (m_wasmCalleesPendingDestructionSnapshot.isEmpty())
        return;

    // Release refs outside the lock since Callee destructors may call reportWasmCalleePendingDestruction.
    Vector<RefPtr<Wasm::Callee>, 8> wasmCalleesToRelease;
    {
        Locker locker(m_wasmCalleesPendingDestructionLock);
        wasmCalleesToRelease = m_wasmCalleesPendingDestruction.takeIf<8>([&](const auto& callee) {
            return m_wasmCalleesPendingDestructionSnapshot.contains(callee.ptr())
                && !m_wasmCalleesDiscoveredDuringGC.contains(callee.ptr());
        });
    }

    m_wasmCalleesPendingDestructionSnapshot.clear();
    m_wasmCalleesDiscoveredDuringGC.clear();
}

#endif

// --- Shared heap server (SPEC-heap.md; THREADS T1 scaffolding) ---

// I13: at most one sticky-shared server per process (phase 1: the main VM's heap).
static Atomic<Heap*> s_stickySharedServer;

// I14: per-thread depth of STW-forbidden scopes (e.g. vmstate's
// StructureAllocationLocker). Release-real since IT-4: collectIfNecessaryOrDefer
// consults it to defer stop polls / GC initiation reached inside such a region
// (S1-S3 — a scope holder must never park for a stop while holding the SAL).
static thread_local unsigned t_stwForbiddenScopeDepth { 0 };

bool Heap::tryDesignateStickySharedServer()
{
    // UNGIL §0 U0c (ANNEX U0C; U-T1): designation primitive — the
    // s_stickySharedServer CAS, returning won/lost, NO assert. Called by
    // every VM ctor under gilOffProcess, BEFORE m_mainVMLite registration,
    // any entry, any codegen. The winner follows up with
    // noteSharedServerSticky() at clientSet()==1 (its inner CAS then sees
    // previous==this, so I13 stands textually unchanged below and never
    // fires on this path).
    Heap* previous = s_stickySharedServer.compareExchangeStrong(nullptr, this);
    return !previous || previous == this;
}

void Heap::noteSharedServerSticky() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // Sticky ISS (§5.1): option && clientSet().size() EVER > 1; set under
    // legacy-GC quiescence (§10B.4); cleared only via §10D reversion.
    // Called by HeapClientSet::add BEFORE inserting the client that makes
    // size() > 1, with no lock held (rank 5 is taken here, outer to HCS's
    // rank 6).
    // THREADS-INTEGRATE(heap): Options::useSharedGCHeap() is added by
    // INTEGRATE-heap.md manifest item 2 (runtime/OptionsList.h).
    if (!Options::useSharedGCHeap())
        return;
    if (m_isSharedServer.load(std::memory_order_relaxed))
        return;

    // I13: only one sticky-shared server may ever exist in this process.
    // After a §10D reversion the same server may go shared again
    // (s_stickySharedServer still points at it), but a different server may not.
    Heap* previous = s_stickySharedServer.compareExchangeStrong(nullptr, this);
    RELEASE_ASSERT(!previous || previous == this);

    // §5.5 never-populate audit (T4), RELEASE_ASSERTed at second-client
    // attach: with the option on, allocatorForSlow never materializes
    // server-side non-iso Allocators, so the JS-tier inline-allocation
    // emitters can only ever have baked/loaded null (slow path => the
    // caller's TLC).
    verifyServerNonIsoAllocatorsNeverMaterialized();

    // T5 (manifest items 4-5): install the heap-owned GC park hooks before
    // any conducted stop can occur. Inert until a GC stop is requested
    // (callbacks no-op unless ISS && GSP). Idempotent.
    // THREADS-INTEGRATE(heap): VMManager::setGCParkCallbacks is added by
    // INTEGRATE-heap.md manifest item 4 (runtime/VMManager.h); it stores
    // into file-local Atomic statics in runtime/VMManager.cpp (item 5d,
    // review round 4) — NOT g_jscConfig slots: JSC::Config lives in the
    // WTF::Config region that Config::finalize() (run from every VM
    // constructor) mprotects read-only, and this installer necessarily runs
    // at SECOND-client attach, after that freeze — a config store here would
    // SIGSEGV. The notifyVMStop call sites and the StopReason::GC
    // keep-parked/latch-exclusion/resume-notify behavior are item 5
    // (SPEC-heap-annex.md §A5; the manifest's round-4 hunks are the source
    // of truth).
    VMManager::setGCParkCallbacks(&Heap::gcWillParkInStopTheWorld, &Heap::gcDidResumeFromStopTheWorld);

    // §10B.4 attach quiescence (I13/I15): under *m_threadLock, timed re-check
    // loop (<= 1ms waits on the GC election condition — never on
    // m_threadCondition, which the mutator must not wait on) until the legacy
    // protocol is fully quiescent; then, in the same critical section, set
    // sticky ISS so new triggers re-route (I15).
    //
    // Mutator-exclusion clause (review round 1): collector-protocol
    // quiescence alone is NOT enough — a legacy mutator on another thread
    // could be mid-allocateSlowCase (entered with isSharedServer() == false,
    // so its MutatorSlowPathLocker is a no-op) at the flip, and would then
    // race the second client's MSPL-licensed slow path on the shared
    // BlockDirectories (I1/I8/I5b). The loop therefore additionally requires
    // one of:
    //  (a) this thread holds the main VM's API lock — legacy heap mutation
    //      requires that lock (currentThreadIsAllocatorOwner's !ISS
    //      predicate, and the GIL serializes all JS on it), so no other
    //      thread can be mid-slow-path; every later legacy entrant orders
    //      after the flip through the JSLock mutex release/acquire pair, so
    //      even its relaxed isSharedServer() reads observe ISS; or
    //  (b) no thread holds legacy heap access (hasAccessBit clear) — nobody
    //      is inside the heap at all. Review round 3: this observation and
    //      the §10B.4 poison are ONE atomic CAS inside the loop (no TOCTOU
    //      window in which a stale acquirer could slip in); post-flip
    //      entrants are funneled by the pinned bit into acquireAccessSlow's
    //      ISS resolution. A foreign access holder is simply waited out:
    //      callers that keep access across blocking sections must release it
    //      (ReleaseHeapAccessScope; the SharedHeapTestHarness contract does
    //      exactly this) before clients can attach on other threads.
    // NORMATIVE cross-part contract (review round 2; recorded in
    // INTEGRATE-heap.md "Cross-part contract: ISS-flip liveness"): clause (b)
    // below waits out a foreign legacy access holder — unboundedly. The
    // api/runtime workstream's Thread() spawn path MUST guarantee the main
    // mutator reaches an access-release point (JS poll points release across
    // blocking sections; embedders that acquireAccess() at instantiation and
    // then block — e.g. join() without a ReleaseHeapAccessScope — would
    // deadlock the attach). The diagnostic below makes a violation
    // release-visible instead of a silent hang.
    {
        Locker locker { *m_threadLock };

        // Concurrent-flip re-entrancy (review round 3): two clients attaching
        // back-to-back can BOTH pass the relaxed entry check above (taken
        // with no lock held) and reach here. The loser must NOT run the
        // quiescence loop: the winner's flip pins hasAccessBit permanently
        // (the §10B.4 poison), so the loop's no-access clause would never
        // again be satisfiable on a thread that does not hold the main VM's
        // API lock — a permanent attach hang. The winner completed the
        // ENTIRE flip (hook install, migration, poison, fence) before
        // storing ISS, so returning here is complete; HeapClientSet::add()
        // then re-checks isSharedServer() and takes the already-shared
        // insert path. Re-checked both here and inside the wait loop (the
        // timed waits release *m_threadLock, so a concurrent winner can
        // finish while we wait).
        if (m_isSharedServer.load(std::memory_order_seq_cst))
            return;

        MonotonicTime quiescenceWaitStart = MonotonicTime::now();
        bool loggedQuiescenceStall = false;
        // True => quiescence clause (a): the main mutator holds legacy heap
        // access and this thread holds the main VM's API lock. Decides the
        // migration branch below — m_worldState must NOT be re-sampled for
        // that decision once the clause-(b) gate has installed the poison
        // (the re-sample would read the poison and take the wrong branch).
        bool apiLockedAccessHolder = false;
        for (;;) {
            if (m_isSharedServer.load(std::memory_order_seq_cst))
                return; // A concurrent flipper won while we waited (see above).
            if (m_lastServedTicket == m_lastGrantedTicket
                && !m_collectorThreadIsRunning
                && m_currentPhase == CollectorPhase::NotRunning) {
                unsigned state = m_worldState.load();
                if (state & hasAccessBit) {
                    if (vm().currentThreadIsHoldingAPILock()) {
                        // Clause (a): the access holder is the main mutator,
                        // and legacy heap mutation requires the API lock we
                        // hold, so it is not mid-operation; every later
                        // legacy entrant orders after the flip through the
                        // JSLock mutex release/acquire pair, so even its
                        // relaxed isSharedServer() reads observe ISS.
                        apiLockedAccessHolder = true;
                        break;
                    }
                    // A foreign legacy access holder: wait it out (NORMATIVE
                    // ISS-flip liveness contract, INTEGRATE-heap.md — callers
                    // that keep access across blocking sections must release
                    // it via ReleaseHeapAccessScope before clients can attach
                    // on other threads). Fall through to the timed wait.
                } else {
                    // Clause (b): gate + poison in ONE atomic step (review
                    // round 3). The previous shape sampled "no access holder"
                    // here but installed the poison only AFTER the migration
                    // code below — a TOCTOU window in which a stale legacy
                    // inline acquireAccess() CAS (0 -> hasAccessBit) could
                    // still succeed and enter the heap un-forwarded (no
                    // per-client access state, no MSPL — the I1/I8 hazard).
                    // The CAS below closes it: it succeeds only against the
                    // very no-access state it verifies, and once it succeeds
                    // the legacy inline acquire CAS (which expects exactly 0)
                    // can never succeed again. The poison precedes the ISS
                    // store; acquireAccessSlow() resolves that sub-window by
                    // locking *m_threadLock — which we hold continuously
                    // from this CAS through the ISS store — and re-reading
                    // ISS (see its hasAccessBit branch).
                    if (m_worldState.compareExchangeStrong(state, state | hasAccessBit) == state)
                        break;
                    continue; // m_worldState changed under us; re-evaluate.
                }
            }
            m_gcElectionCondition.waitFor(*m_threadLock, 1_ms);
            if (!loggedQuiescenceStall && MonotonicTime::now() - quiescenceWaitStart > Seconds(5)) [[unlikely]] {
                loggedQuiescenceStall = true;
                dataLogLn("JSC SharedGC: second-client attach (ISS flip) has waited >5s for legacy heap-access quiescence. "
                    "Another thread holds the main VM's heap access and is not releasing it; spawn/join paths must release "
                    "heap access across blocking sections (ReleaseHeapAccessScope) or this attach cannot complete "
                    "(INTEGRATE-heap.md: cross-part contract, ISS-flip liveness).");
            }
        }

        // A collector-thread cycle that ended with no access holder leaves
        // needCollectionEpilogueBit for the main mutator's next acquireAccess() or
        // stopIfNecessary(). Once ISS both forward to the per-client protocol
        // and never consume it, so the stale finalize() would run inside the
        // first conducted cycle after clients have registered precise
        // allocations (sweepPreciseAllocations asserts the nursery offset
        // equals the registry size). Drain it now, while the legacy protocol
        // is quiescent and no mutator can enter the heap (clause (a): this
        // thread holds the API lock; clause (b): hasAccessBit is pinned and
        // *m_threadLock is held). finalize() derefs the main VM's atoms, so
        // that VM's AtomStringTable must be current for the drain.
        if (m_worldState.load() & needCollectionEpilogueBit) [[unlikely]] {
            auto* previousAtomStringTable = Thread::currentSingleton().setCurrentAtomStringTable(vm().atomStringTable());
            handleNeedCollectionEpilogue();
            Thread::currentSingleton().setCurrentAtomStringTable(previousAtomStringTable);
        }

        // Migrate the legacy access state to the per-client protocol (§10A):
        // under clause (a) the main mutator holds legacy heap access and this
        // thread is — or, by the I2 JSLock hand-off rule, just became — the
        // access owner: stamp it and the §10A.1 TLS slot. The branch keys on
        // apiLockedAccessHolder, NOT on a fresh m_worldState sample: under
        // clause (b) the gate above already pinned hasAccessBit while no
        // mutator holds access (§10B.5 — T5b audits the residual readers),
        // so a re-sample here would misclassify the poison as a holder.
        GCClient::Heap* mainClient = m_mainClient;
        RELEASE_ASSERT(mainClient);
        if (apiLockedAccessHolder) {
            mainClient->m_accessState.store(GCClient::Heap::hasAccessState, std::memory_order_seq_cst);
            mainClient->m_accessOwner.store(&Thread::currentSingleton(), std::memory_order_relaxed);
            GCClient::Heap::setCurrentThreadClient(mainClient);
        } else if (mainClient->m_accessState.load(std::memory_order_seq_cst) == GCClient::Heap::hasAccessState) {
            // SharedGC (§10D re-flip; T10, found by issRevertChurn): after a
            // §10D reversion the legacy protocol owns access tracking again,
            // so the main mutator's releases go through the legacy
            // hasAccessBit and the per-client state goes STALE at HasAccess.
            // A later re-flip with the legacy bit CLEAR must clear that stale
            // per-client state too — otherwise the very first §10.4 barrier
            // waits forever on a client whose "access" no thread holds. Safe
            // here: the legacy protocol is quiescent (loop above) and the bit
            // says no mutator holds access.
            mainClient->m_accessState.store(GCClient::Heap::noAccessState, std::memory_order_seq_cst);
            mainClient->m_accessOwner.store(nullptr, std::memory_order_relaxed);
        }

        // §5.4/I17 (T3): migrate any open DeferGC depth to the main client.
        // Pre-ISS, all depth belongs to the legacy single mutator (= the main
        // client); post-ISS, deferralDepthSlot() routes that thread's
        // decrements to the main client's counter, so the pairing stays
        // balanced across the flip.
        if (m_deferralDepth) {
            mainClient->m_deferralDepth += m_deferralDepth;
            m_deferralDepth = 0;
        }
        // Review round 4: the deferred-GC hint migrates with the depth it
        // annotates (didDeferGCWorkSlot() routes to the main client after the
        // flip, so a server-set hint would otherwise be orphaned).
        if (m_didDeferGCWork) {
            mainClient->m_didDeferGCWork = true;
            m_didDeferGCWork = false;
        }

        m_isSharedServer.store(true, std::memory_order_seq_cst);

        // §10B.4 flip handshake / poison (review rounds 1+3): hasAccessBit
        // is pinned in m_worldState, permanently. Once ISS the legacy access
        // bits are superseded (§10B.5), but a thread whose relaxed
        // isSharedServer() read is still stale could otherwise win the
        // legacy inline acquireAccess() CAS (0 -> hasAccessBit) and enter
        // the heap un-forwarded — no per-client access state, no MSPL. With
        // the bit pinned that CAS can never succeed: the stale acquirer
        // falls into acquireAccessSlow(), whose hasAccessBit branch locks
        // *m_threadLock (ordering it after this critical section) and
        // re-reads ISS — true — then forwards (releaseAccessSlow()'s
        // in-loop re-check is the release-side backstop). Under clause (b)
        // the gate-CAS in the loop above ALREADY installed the pin,
        // atomically with the no-access observation, so no legacy acquirer
        // can have slipped in between sample and pin (review round 3).
        // Under clause (a) we install it here: while the bit is set no NEW
        // legacy acquirer can succeed (the inline CAS expects exactly 0, and
        // a concurrent acquire while another thread holds access is the
        // double-acquire bug the legacy protocol always crashed on), and the
        // existing holder is API-lock-ordered after the flip, so its later
        // releaseAccess() observes ISS and forwards — it can never clear the
        // pin. The exchangeOr is idempotent under clause (b). The pin
        // survives until a §10D reversion era: after a reversion the main
        // mutator's per-client access and the pinned bit denote the SAME
        // holder, so a legacy releaseAccess() CAS (hasAccessBit -> 0) by
        // that holder is then a correct release, not a lost poison.
        m_worldState.exchangeOr(hasAccessBit);

        // §10B.5 (T5b): always-fenced once shared — raise the fence at the
        // flip so the very first multi-mutator window is fenced even before
        // the first conducted cycle's beginMarking().
        setMutatorShouldBeFenced(true);
    }
}

void Heap::collectSyncAllClients(CollectionScope scope)
{
    ASSERT(!currentThreadHasSTWForbiddenScope()); // I14/L5.
    // §10.1 CSAC: ticket + §10.2 election once shared; legacy collectSync
    // otherwise. collectSync() itself performs the ISS re-route (I15), so
    // both protocols funnel through one entry.
    collectSync(GCRequest(scope));
}

void Heap::requestCollectionAllClients(GCRequest request)
{
    ASSERT(!currentThreadHasSTWForbiddenScope()); // I14/L5.
    // §10.1 RCAC: ticketing only — no fire-and-forget collections when
    // shared; the ticket is served by the next conductor (a sync requester's
    // election or a stopIfNecessaryForAllClients() poll). collectAsync()
    // performs the ISS re-route (I15) and the subsumption check.
    collectAsync(request);
}

Heap::Ticket Heap::requestCollectionShared(GCRequest request)
{
    Locker locker { *m_threadLock };
    return requestCollectionShared(locker, request);
}

Heap::Ticket Heap::requestCollectionShared(const AbstractLocker&, GCRequest request)
{
    // §10B.1 ticketing: like requestCollection() minus the legacy
    // stopIfNecessary() prelude. Precondition (SharedGC, T5b — the shared
    // replacement for requestCollection()'s API-lock/atom-table asserts):
    // the requester holds its client's heap access, or is the conductor
    // while the world is stopped for all clients.
    ASSERT(isSharedServer());
#if ASSERT_ENABLED
    GCClient::Heap* requester = GCClient::Heap::currentThreadClient();
    ASSERT((requester && &requester->server() == this && requester->hasHeapAccess())
        || worldIsStoppedForAllClients());
#endif

    ASSERT(m_lastServedTicket <= m_lastGrantedTicket);
    // §10B.2/§10B.3: the conductor always runs as the mutator and the
    // collector thread is quiesced once shared (I15) — set the conn bit
    // idempotently via exchangeOr; never assert served == granted mid-drain.
    ASSERT(!m_collectorThreadIsRunning);
    m_worldState.exchangeOr(mutatorHasConnBit);
    m_requests.append(request);
    m_lastGrantedTicket++;
    // No m_threadCondition notify: the collector thread never serves shared
    // tickets (§10B.3); followers wait on m_gcElectionCondition (§10.2).
    return m_lastGrantedTicket;
}

void Heap::runSharedGCElection(Ticket ticket) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // §10.2 election. Preconditions: caller holds its client's heap access;
    // no rank >= 4 lock, no SAL (L2/I14); not inside a stop window.
    ASSERT(!currentThreadHasSTWForbiddenScope()); // I14/L5.
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    RELEASE_ASSERT(client && &client->server() == this);
    RELEASE_ASSERT(client->hasHeapAccess());

    for (;;) { // while (T not served)
        {
            Locker locker { *m_threadLock };
            if (m_lastServedTicket >= ticket)
                return;
        }

        if (m_gcConductorLock.tryLock()) { // GCL, rank 2; tryLock — no 5 -> 2 inversion. §3.4 disposition (ANNEX CGD6.2 row: election).
            bool betweenWindowsBackOff = false;
            {
                Locker locker { *m_threadLock };
                if (m_lastServedTicket >= ticket) {
                    m_gcConductorLock.unlock();
                    return;
                }
                // SPEC-congc §3.4 between-windows guard (ANNEX CGD6.2,
                // election row): "GCL free && GCA set && phase != NotRunning"
                // is a STEADY STATE between windows of a live cycle; a
                // tryLock winner here would NEST a conductor against it
                // (ANNEX CGD1.1: nested requestStopAll, double
                // endMarking/finalize). Fall to the follower wait instead.
                // Flag-off this never fires: any GCL-free point has
                // m_currentPhase == NotRunning (the one window spans the
                // whole cycle — CGD1.1 flag-off half; CG-I0).
                if (m_gcConductorActive && m_currentPhase != CollectorPhase::NotRunning)
                    betweenWindowsBackOff = true;
                else if (m_sharedGCPreventCount && m_sharedGCPreventHolder != &Thread::currentSingleton()) [[unlikely]] {
                    // W16-C1 residual (b): a PreventCollectionScope holder
                    // raised the conduct-tenure gate — no NEW shared
                    // collection may start until allowCollection(), unless
                    // this thread IS the holder (its own collectNow(Sync)
                    // must conduct, or it would wait on itself). Fall to
                    // the follower wait (timed <=1ms GEC waits when no
                    // conductor is active — the same shape as the landed
                    // §10.2 GCL-busy rule; allowCollection() notifies GEC).
                    // Our ticket stays granted-unserved and wins a later
                    // election; keep-waiting is the correct semantics —
                    // prevention means no collection can start.
                    betweenWindowsBackOff = true;
                } else {
                    m_gcConductorActive = true; // GCA.
                    // §3.5: stamp the tenure owner. Restamp over a stale
                    // owner (a descheduled predecessor's deferred clear) is
                    // legal only in NotRunning (ANNEX CGD3.1; CG-I21).
                    ASSERT(!m_gcConductorThread || m_currentPhase == CollectorPhase::NotRunning);
                    m_gcConductorThread = &Thread::currentSingleton();
                }
            }
            if (betweenWindowsBackOff)
                m_gcConductorLock.unlock(); // Back off; fall through to the follower wait below.
            else {
                conductSharedCollection(*client); // §10 steps 3-9.
                m_gcConductorLock.unlock();
                {
                    Locker locker { *m_threadLock };
                    // F20 OWNERSHIP-CHECKED CLEAR (ANNEX CGD3.1 GOVERNS;
                    // BENIGN-DELTA flag-off, cited in INTEGRATE-congc.md): a
                    // successor that won GCL between our unlock above and
                    // this deferred clear has RESTAMPED the owner — clearing
                    // GCA under its live tenure would re-open the CGD1.1
                    // nesting for a third requester and break the §3.4
                    // FOREIGN discrimination. Clear only if still ours;
                    // notifyAll unconditional (same condvar, no lost wakeup).
                    if (m_gcConductorThread == &Thread::currentSingleton()) {
                        m_gcConductorActive = false;
                        m_gcConductorThread = nullptr;
                    }
                    m_gcElectionCondition.notifyAll();
                }
                continue; // Re-check our ticket (late-granted tickets re-loop and win tryLock).
            }
        }

        // Follower (a conductor is active), or GCL-busy (a JSThreadsStopScope
        // holds GCL, §10C(e)). Either way: release access — REQUIRED for the
        // §10.4 barrier — and wait on the election condition; never on
        // m_threadCondition, never spinning, and in the GCL-busy case never
        // untimed (§10.2 GCL-busy rule).
        client->releaseHeapAccess();
        {
            Locker locker { *m_threadLock };
            if (m_gcConductorActive) {
                while (m_lastServedTicket < ticket && m_gcConductorActive)
                    m_gcElectionCondition.wait(*m_threadLock);
            } else
                m_gcElectionCondition.waitFor(*m_threadLock, 1_ms); // GCL-busy: timed (<= 1ms).
        }
        // §10.2: VM-backed requesters poll VMTraps each iteration and park in
        // notifyVMStop if a VMM stop pends (a JSThreads/debugger conductor
        // needs us parked, not merely access-released). We hold no lock and
        // no access here (I6). Standalone (§12.1) clients have no VM/traps.
        // THREADS-INTEGRATE(heap): the trap bit is set for entered VMs by the
        // manifest-5g(ii) hunk and by the requester's requestStopAll.
        if (!client->m_isStandalone) [[likely]] {
            // UNGIL §B.2 (U-T6): route via the server — GCClient::Heap::vm()
            // is VM-embedding pointer arithmetic (HeapInlines.h:286) that is
            // GARBAGE for the GIL-off per-thread heap-allocated clients
            // (spawned + embedder carriers). A non-standalone client's
            // server is a VM's own heap, and every client of one server
            // belongs to that one VM (U0b), so the server-side vm() IS the
            // client's VM in every case, per-thread clients included.
            VM& vm = client->server().vm();
            if (vm.traps().needHandling(VMTraps::NeedStopTheWorld)) [[unlikely]]
                VMManager::singleton().notifyVMStop(vm, StopTheWorldEvent::VMStopped);
        }
        client->acquireHeapAccess(); // F8: blocks while a stop is pending.
    }
}

bool Heap::tryConductSharedCollectionForPoll(GCClient::Heap& client) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // Non-blocking election attempt: serves granted-unserved tickets from a
    // mutator poll (mutator-driven triggering, §5.4/I15 — RCAC/CIND tickets
    // have no waiting requester). Returns true if it conducted.
    ASSERT(isSharedServer());
    ASSERT(client.hasHeapAccess());
    if (!m_gcConductorLock.tryLock()) // §3.4 disposition (ANNEX CGD6.2 row: poll).
        return false; // Conductor active or JSThreadsStopScope held (§10C(e)); retry next poll.
    bool shouldConduct = false;
    {
        Locker locker { *m_threadLock };
        // SPEC-congc §3.4 between-windows guard (ANNEX CGD6.2, poll row):
        // mid-cycle between windows (GCA set, phase != NotRunning) this
        // tryLock can now succeed; conducting would nest (ANNEX CGD1.1).
        // Return false; retry next poll. Flag-off never fires (CGD1.1
        // flag-off half; CG-I0).
        if (m_gcConductorActive && m_currentPhase != CollectorPhase::NotRunning) {
            // shouldConduct stays false.
        } else if (m_sharedGCPreventCount && m_sharedGCPreventHolder != &Thread::currentSingleton()) [[unlikely]] {
            // W16-C1 residual (b): conduct-tenure gate raised by another
            // thread's PreventCollectionScope — refuse to conduct; tickets
            // sit granted-unserved until allowCollection(). shouldConduct
            // stays false; the poll retries naturally. The holder itself
            // may conduct (legacy semantics).
        } else if (m_lastServedTicket < m_lastGrantedTicket && !sharedGCPreventGateBlocksNextTicket(locker)) {
            m_gcConductorActive = true;
            // §3.5 owner stamp (restamp NotRunning-only — ANNEX CGD3.1; CG-I21).
            ASSERT(!m_gcConductorThread || m_currentPhase == CollectorPhase::NotRunning);
            m_gcConductorThread = &Thread::currentSingleton();
            shouldConduct = true;
        }
    }
    if (!shouldConduct) {
        m_gcConductorLock.unlock();
        return false;
    }
    conductSharedCollection(client);
    m_gcConductorLock.unlock();
    {
        Locker locker { *m_threadLock };
        // F20 ownership-checked deferred clear (ANNEX CGD3.1 GOVERNS;
        // BENIGN-DELTA flag-off, cited in INTEGRATE-congc.md) — the election
        // path's twin: skip if a successor restamped; notifyAll
        // unconditional.
        if (m_gcConductorThread == &Thread::currentSingleton()) {
            m_gcConductorActive = false;
            m_gcConductorThread = nullptr;
        }
        m_gcElectionCondition.notifyAll();
    }
    return true;
}

// ============================================================================
// UNGIL §D.1 TID rebias — conductor phase (ANNEXES D1 + D1R, BINDING; U-T12).
//
// Runs INSIDE the full shared-GC stop (heap §10 barrier, WSAC set; NOT a §A.3
// stop — jit R1.h; re-entry blocked per §A.3.8), strictly BEFORE the step-8/9
// resumes. Restamps every live cell's dead-TID state to 0 FROM THE SEALED
// SNAPSHOT ONLY (two-phase vs §LK, r9 F2: the snapshot was sealed pre-stop by
// a mutator under TM::m_lock; the conductor takes NO api lock — both
// ThreadManager calls used here are lock-free state flips):
//
//   - instance butterfly tags (JSObject tagged words, Flat/FlatShared): dead
//     tid -> 0, payload + SW bit preserved. Restamp-to-0 soundness (D1): the
//     object becomes equivalent to main-allocated (the payload-0/TID-0
//     regime; OM decode tests payload first). No fire needed for instance
//     tags (D1R item 4): the jit read/write predicates load the instance tag
//     at runtime against the R5 TLS tag — neither side is a baked immediate.
//     Segmented words (TID == notTTLTID, reserved) are never dead and are
//     skipped; aux/fragment allocations carry no TID tags, so the JSCell
//     walk is the complete restamp surface (the D1 "precise + aux"
//     enumeration is the forEachLiveCell coverage of MarkedBlocks +
//     PreciseAllocations).
//
//   - Structure::m_transitionThreadLocalTID (the N1 butterfly-less
//     transition key): dead tid -> 0, AND (D1R item 1) fireTransitionThread-
//     Local on EVERY such structure before the stop resumes — jettisoning
//     every DFG/FTL/IC body specialized on it (E4 emission bakes
//     "R5 tag vs tid<<48 immediate" when specialized on a concrete S; the
//     TTL fire kills it), so no baked dead-TID immediate survives to the
//     post-resume m_freeTIDs release that makes reissue possible (OM
//     I11/I15 hold by construction). Structures are JSCells in
//     m_objectSpace, so the single walk IS the D1 "StructureID-table walk".
//     I13 SUPERSESSION (D1R item 2, rebias-stop fires only): the §10 stop
//     barrier provides equivalent quiescence; butterflyWorldIsStopped()'s
//     worldIsStoppedForAllClients() disjunct routes the fires to the
//     run-inline branch; the resume-side sync is the ISB1.1 generation bump
//     in conductSharedCollection (which executes AFTER this, before the GSP
//     clear); conservative scan R2 + I7 gate the jettisoned-code frees.
//
// The fires run AFTER the iteration scope closes (still in-stop): fire
// bodies take ConcurrentJSLockers / rank-6b CodeBlock locks and run the F4
// chain-fire — none of which belongs inside a forEachLiveCell functor. The
// fired set = the restamped-structure set (D1R item 3 cost bound; chain-fire
// per OM F4, covered by the jit Task-13 stop-budget gate — rebias is a rare,
// exhaustion-driven event under SD9's spawn gate).
//
// Multi-VM gilOffProcess: other VMs' threads legitimately run un-stopped
// during the rebias stop (TM is process-global). The fire loop below reaches
// stopTheWorldAndRun on this stop's WSAC evidence and runs inline under
// AlreadyStoppedWorldWitnessScope, whose tripwire counts only entered VMs
// attached to THIS stopped server, so an entered loser VM on its own heap is
// not counted. That is sound: loser-VM mutators can never execute the winner
// VM's compiled code, so firing/jettisoning winner code under the
// winner-heap-only stop patches nothing a running thread can be inside.
//
// Both restamps (Structure::restampTransitionThreadLocalTID and the butterfly
// word store) are relaxed atomic stores: mutators are stopped, but DFG/FTL
// compiler threads still run and may concurrently read these words (their
// stale reads are killed by the very fires this walk performs — any
// compilation specialized on a restamped structure watches a set that is now
// fired and dies at link time).
// ============================================================================
static NEVER_INLINE void conductTIDRebiasUnderSharedStop(JSC::Heap& heap, const Vector<uint16_t>& deadTIDs)
{
    RELEASE_ASSERT(heap.worldIsStoppedForAllClients());
    RELEASE_ASSERT(!deadTIDs.isEmpty());
    VM& vm = heap.vm();

    static_assert(static_cast<uint16_t>(JSC::notTTLTID) == ThreadManager::notTTLTID, "the OM butterfly TID space and the TM TID space are the same 2^15 space");

    BitVector dead;
    dead.ensureSize(ThreadManager::notTTLTID);
    for (uint16_t tid : deadTIDs) {
        RELEASE_ASSERT(tid && tid < ThreadManager::notTTLTID); // never 0 (main / restamp target), never the segmented sentinel
        dead.quickSet(tid);
    }

    RaceAmplifier::perturb(); // U-T12 two-VM TM-churn stall point: pre-walk, in-stop (other VMs' threads are NOT stopped and may churn TM::m_lock).

    Vector<Structure*> structuresToFire;
    {
        HeapIterationScope iterationScope(heap);
        heap.objectSpace().forEachLiveCell(
            iterationScope,
            [&](HeapCell* heapCell, HeapCell::Kind kind) -> IterationStatus {
                if (!isJSCellKind(kind))
                    return IterationStatus::Continue; // aux storage carries no TID tags (see banner)
                JSCell* cell = static_cast<JSCell*>(heapCell);

                if (cell->type() == StructureType) {
                    Structure* structure = static_cast<Structure*>(cell);
                    uint16_t transitionTID = structure->transitionThreadLocalTID();
                    if (transitionTID && transitionTID < ThreadManager::notTTLTID && dead.quickGet(transitionTID)) {
                        structure->restampTransitionThreadLocalTID(0);
                        structuresToFire.append(structure);
                    }
                    return IterationStatus::Continue;
                }

                if (!cell->isObject())
                    return IterationStatus::Continue;
                if (cell->type() == WebAssemblyGCObjectType)
                    return IterationStatus::Continue; // JSObject WITHOUT the butterfly word (the sole such family; offset 8 is not a tag word there)
                JSObject* object = asObject(cell);
                uint64_t word = object->taggedButterflyWord();
                if (!(word & butterflyPointerMask))
                    return IterationStatus::Continue; // None regime: all-zero word
                ButterflyTID instanceTID = butterflyTID(word);
                if (!instanceTID || instanceTID == JSC::notTTLTID || !dead.quickGet(instanceTID))
                    return IterationStatus::Continue; // TID-0, segmented, or live-owner word
                // Dead flat/flat-shared tag: tid -> 0, payload + SW preserved.
                uint64_t restamped = word & ~butterflyTIDMask;
                reinterpret_cast<Atomic<uint64_t>*>(reinterpret_cast<char*>(object) + JSObject::butterflyOffset())->store(restamped, std::memory_order_relaxed);
                return IterationStatus::Continue;
            });
    }

    RaceAmplifier::perturb(); // U-T12 D1R.5 stall point: post-restamp, pre-fire.

    // D1R item 1: fire (and thereby jettison) BEFORE the stop resumes —
    // hence strictly before the post-resume m_freeTIDs release. The fires
    // reach stopTheWorldAndRun on this GC stop's evidence, and under gilOff
    // it licenses inline execution only for the thread conducting that stop,
    // which it reads from mutatorState(): mark this thread Collecting for the
    // fires exactly as collectInMutatorThread does for the cycle's own fires
    // (this runs after that scope has closed, still inside the stop window).
    {
        CollectingScope collectingScope(heap);
        for (Structure* structure : structuresToFire)
            structure->fireTransitionThreadLocal(vm, "UNGIL D1R: TID rebias restamped this structure's transition TID inside the shared-GC stop");
    }

    RaceAmplifier::perturb(); // U-T12 D1R.5 stall point: post-fire, pre-Restamped flip.
}

void Heap::openSharedGCStopWindow(GCClient::Heap& conductorClient, SharedGCWindowOpen openKind) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // SPEC-congc §3.1 WND-open (CG-1). Two arms:
    //  - FirstWindow (F15 carve-out): the landed §10 steps 3-4 — GCL was
    //    tryLock'd access-HELD by the election/poll caller; GSP first, THEN
    //    the step-3 access release. Flag-off this is the ONLY arm reachable
    //    (one window per conduct, §3.6 degenerate; CG-I0 byte-for-byte).
    //  - Reentry (§3.1(a)-(b), flag-dead at C0): access stays released all
    //    tenure (§3.7), F45 foreign-waiter deferral, then a BLOCKING GCL
    //    acquire — legal exactly because access-released (ungil §A.3 rule 2;
    //    HBT4 extended to re-entry, ANNEX CGS2.4(b); rev 1's
    //    GCL-before-release order REJECTED, F9).
    // CG-I19/F40 (ANNEX CGD6.1): m_nativeLockDepth == 0 at conducting entry.
    // THREADS-INTEGRATE(congc/nativeaffinity): the NL depth slot does not
    // exist in this tree yet; the debug assert lands with the BL1.8 drop
    // scope (nativeaffinity owner).
    RELEASE_ASSERT(isSharedServer());
    RELEASE_ASSERT(!m_worldIsStoppedForAllClients.load(std::memory_order_acquire));
    RELEASE_ASSERT(!m_gcStopPending.load(std::memory_order_seq_cst));

    if (openKind == SharedGCWindowOpen::Reentry) {
        RELEASE_ASSERT(sharedGCWindowedConductActive()); // Flag-dead at C0 (CG-I0); §7.1a gilOff single-handoff also reaches Reentry.
        ASSERT(!conductorClient.hasHeapAccess()); // §3.7: released all tenure.
        // §9.1(2a) GCL FAIRNESS (F45; ANNEX CGD7.2): the landed §A.3
        // acquisition is an unqueued 1ms tryLock poll with no queue position;
        // a blocking re-acquire issued the moment timeToStop() elapses can
        // starve it into the 30s watchdog fail-stop. DEFER while any foreign
        // waiter is registered: GCL is FREE between windows (CG-I12) and we
        // abstain, so a registered waiter's next tryLock succeeds within one
        // poll quantum; waiters drain in poll order; our wait is bounded by
        // the sum of their bounded scopes (all CGS2.3 terms). No counter
        // check inside the lock itself — fairness purely by abstention.
        // CG-I26; CG-T8's F45 arm.
        while (m_foreignGCLWaiters.load(std::memory_order_relaxed))
            WTF::sleep(Seconds::fromMilliseconds(1));
        m_gcConductorLock.lock(); // Blocking is legal: access-released (rule 2).
    }
    // FirstWindow: GCL already held by this thread.

    // Step (c)/3 — stop request: seq_cst GSP = true (F8 Dekker store),
    // release our own access (FirstWindow only — F15 order: GSP, THEN the
    // step-3 release), then the async VMM stop. Our own trap bit is harmless
    // (we do not run JS until resume).
    m_gcStopPending.store(true, std::memory_order_seq_cst);
    if (openKind == SharedGCWindowOpen::FirstWindow) {
        if (conductorClient.hasHeapAccess())
            conductorClient.releaseHeapAccess();
    }
    // THREADS-INTEGRATE(heap): StopReason::GC never enters VMM's
    // latch/dispatch; the keep-parked bit, park hooks, resume notify and
    // re-latch behavior are INTEGRATE-heap.md manifest item 5 (annex §A5).
    VMManager::requestStopAll(VMManager::StopReason::GC);

    // Step (e)/4 — access barrier: under GBL, wait until every client is
    // NoAccess (F8: seq_cst samples). Entered mutators park via traps ->
    // notifyVMStop -> manifest-5a willPark; others release at their next
    // RHA/SINFAC poll; acquirers revert-and-block (F8 step 3). Nothing here
    // pushes onto a run loop: the VMM stop traps only entered VMs, so a
    // client that holds access while its thread is outside JS (blocked in
    // native code, idle in an event loop) is reached only by its own
    // releaseHeapAccess() or stopIfNecessary() poll — the embedder
    // obligation behind ReleaseHeapAccessScope around blocking sections.
    // Such a holder stalls this barrier; the rate-limited dump below names it.
    {
        Locker locker { m_gcBarrierLock }; // GBL, rank 4.
        MonotonicTime barrierWaitStart = MonotonicTime::now();
        MonotonicTime nextStallDump = barrierWaitStart + Seconds(5);
        for (;;) {
            bool anyAccess = false;
            clientSet().forEach([&](GCClient::Heap& client) { // Rank 6 inside rank 4 (§6).
                if (client.m_accessState.load(std::memory_order_seq_cst) == GCClient::Heap::hasAccessState)
                    anyAccess = true;
            });
            if (!anyAccess)
                break;
            MonotonicTime now = MonotonicTime::now();
            if (now >= nextStallDump) [[unlikely]] {
                dumpSharedGCAccessBarrierStall(now - barrierWaitStart);
                nextStallDump = now + Seconds(30);
            }
            // §10.4 fan re-assertion (mc-safe-gcwait-vs-classa-stop family;
            // mirrors the §A.3 conductor's re-fire, VMManager.cpp "Re-fire on
            // every non-quiescent sample"): under the §A.2.1 interim seam the
            // per-lite stop bits ALIAS the single VM-wide trap word, and
            // VMTraps' take rule clears NeedStopTheWorld when the FIRST
            // trapping thread latches it. A sibling mutator still running JS
            // (or spinning in a compile-lock tryLock loop that services traps
            // only when the bit is visible) would poll a clear word forever,
            // never release access, and wedge this barrier — observed as the
            // ic-publish-reset-loops / gcwait 30s-watchdog and 100s-hang
            // signatures. Re-fire per sample (requestStopAllInternal's
            // mode>=Stopping GC arm re-traps entered VMs; idempotent).
            // Lock order: m_worldLock ranks ABOVE GBL (notifyVMStop holds it
            // while park hooks take GBL via RHA), so drop GBL for the call.
            // The timed wait below bounds the re-sample so a wakeup lost
            // between the drop and the wait cannot wedge us. RETIRED when the
            // per-lite trap words land (VMTraps.h activation checklist).
            {
                DropLockForScope dropper(locker);
                VMManager::requestStopAll(VMManager::StopReason::GC);
            }
            m_gcBarrierCondition.waitFor(m_gcBarrierLock, Seconds::fromMilliseconds(1));
        }
        m_worldIsStoppedForAllClients.store(true, std::memory_order_seq_cst); // WSAC set under GBL (F7).
    }

    // SPEC-congc §4.1 + §5.2(i) (CG-2) — WND-open per-client fold loops,
    // WSAC held (clientSet add/remove is frozen in-window, heap I13; the
    // client bytes are quiesced — every owner thread is parked or
    // access-released, so the relaxed reads below are window-barrier-ordered,
    // CG-I9). C1R-only (F33): flag-off the clients never set didRun and
    // every CMS is null, and the landed shared-mode behavior stands
    // byte-for-byte (CGD4.4 — this block is skipped entirely).
    //  (1) didRun fold: OR every client's m_didRunSinceLastWindow into the
    //      legacy consumer (stopThePeriphery's m_mutatorDidRun ->
    //      m_mutatorExecutionVersion bump) and clear it in-window.
    //  (2) CMS drain: transfer every client CMS, under m_markingMutex
    //      (donateAll shape, §5.2(i) — the correctness drain; §5.2(ii)
    //      threshold donation is CG-3). The drain TARGET is OPEN-KIND SPLIT
    //      (CG-3b amend; the CG-T8 Arm-1 RED root cause):
    //      - Reentry (mid-cycle: marking in flight, shared-stack accounting
    //        live): m_sharedMutatorMarkStack. NORMATIVE (F32/CGA1 A21): the
    //        drain PRECEDES the window's first constraint-solver pass —
    //        MarkStackMergingConstraint covers the SERVER + race stacks
    //        only; mid-cycle CMS work is accounted exclusively via the
    //        shared stacks (hasWork/didReachTermination count them,
    //        SlotVisitor.cpp).
    //      - FirstWindow (a PRE-CYCLE open — it precedes the conducted
    //        cycle's runBeginPhase): the SERVER legacy
    //        m_mutatorMarkStack. Pre-loading m_sharedMutatorMarkStack here
    //        violated runBeginPhase's didReachTermination() precondition
    //        ("SlotVisitor should think that GC should terminate before
    //        constraint solving" fail-stop — hasWork counts the shared
    //        stacks; with >1 marker the freshly-armed helpers stole the
    //        pre-cycle cells concurrently, which is why the crash dump
    //        recomputed didReachTermination()=true). Pre-cycle CMS cells are
    //        exactly pre-cycle BARRIER appends, so they take the landed
    //        single-VM pre-cycle route: cleared at full-GC begin (the mark
    //        version reset makes them redundant — roots re-mark), retained
    //        and constraint-merged on Eden (MarkStackMergingConstraint) —
    //        which still PRECEDES the window's first constraint-solver pass,
    //        so §5.2(i)'s normative order holds on this arm too. The
    //        server-stack target splice holds m_serverMutatorMarkStackLock,
    //        the same lock every server-arm barrier append takes.
    // Lock order per client: HCS m_lock (rank 6, inside forEach) ->
    // m_markingMutex (LK.9d) -> m_serverMutatorMarkStackLock (server-stack
    // target only) -> CMS (LK.9c) — the CGS2.2 chain, forward edges only.
    if (sharedGCBarrierStateIsPerClient()) [[unlikely]] {
        bool anyClientRan = false;
        MarkStackArray& cmsDrainTarget = openKind == SharedGCWindowOpen::Reentry ? *m_sharedMutatorMarkStack : *m_mutatorMarkStack;
        clientSet().forEach([&](GCClient::Heap& client) {
            if (WTF::atomicLoad(&client.m_didRunSinceLastWindow, std::memory_order_relaxed)) {
                anyClientRan = true;
                WTF::atomicStore(&client.m_didRunSinceLastWindow, false, std::memory_order_relaxed);
            }
            Locker markingLocker { m_markingMutex };
            std::optional<Locker<Lock>> serverStackLocker;
            if (openKind != SharedGCWindowOpen::Reentry)
                serverStackLocker.emplace(m_serverMutatorMarkStackLock);
            // LK.9d>LK.9c (lint R4 marker): the §5.2(i) WND-open drain edge —
            // the ONE place m_markingMutex nests over a CMS lock at CG-2.
            Locker cmsLocker { client.m_mutatorMarkStackLock };
            if (client.m_mutatorMarkStack && !client.m_mutatorMarkStack->isEmpty()) {
                client.m_mutatorMarkStack->transferTo(cmsDrainTarget);
                // Wake parked helpers (ANNEX CGD1.3 condvar) — same producer
                // contract as the §5.2(ii) donation edge below: helpers park
                // in drainFromShared's waitUntil(isReady) with an INFINITE
                // timeout, so a Reentry drain that loads
                // m_sharedMutatorMarkStack without a notify strands the
                // cells — hasWork flips true for the between-window
                // conductor's waitForTermination (also parked, also never
                // re-notified) while every consumer sleeps: a deterministic
                // termination wedge once the mutators exit. The legacy
                // (non-Reentry) target needs no wakeup, but notifying under
                // the held m_markingMutex is harmless there.
                m_markingConditionVariable.notifyAll();
            }
        });
        if (anyClientRan)
            m_mutatorDidRun = true;
    }

    // §3.7 ATOM-TABLE PIN (F46; ANNEX CGD7.3) — windowed stages only: the
    // install moves INTO the window (after WSAC; restored by the close before
    // its GCL release). Flag-off the landed TENURE-WIDE install in
    // conductSharedCollection stands byte-for-byte (CG-I0); consumers
    // (finalize(), deleteUnmarkedCompiledCode, the step-7 phase loop) are all
    // in-window (§8.1), so no consumer loses coverage.
    if (sharedGCWindowedConductActive()) {
        WTF::AtomStringTable* previous = Thread::currentSingleton().setCurrentAtomStringTable(vm().atomStringTable());
        if (openKind == SharedGCWindowOpen::FirstWindow) {
            // Tenure-original table. Reentry's `previous` is the CG-I27 debug
            // null / non-final-close restore — discard it.
            m_sharedGCWindowSavedAtomStringTable = previous;
        }
    }

    // Step 5 onward (flush / stacks / collection) runs in the caller
    // (conductSharedCollection): WND-open ends here with the world stopped
    // for all clients. Per-client flush (§3.1(e)) rides the cycle's first
    // stopThePeriphery() -> stopAllocating() exactly as landed (see the
    // step-5 note in conductSharedCollection).
}

void Heap::closeSharedGCStopWindow(bool isFinalClose) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // SPEC-congc §3.2 WND-close (CG-1) = §10 steps 8-9: client cache resume
    // pass; ISB bump when gilOffProcess (EVERY close — each window may
    // jettison/patch, ISB1.1); clear WSAC; seq_cst GSP = false; GBC
    // broadcast; requestResumeAll(GC); THEN — NON-FINAL closes only — release
    // GCL (CG-I12). Heap-resume-before-VMM-resume stays normative.
    //
    // FINAL-CLOSE CARVE-OUT (F23): the FINAL close (-> NotRunning; the drain
    // loop's m_requests exit precedes it) leaves GCL HELD — released by the
    // landed CALLER (runSharedGCElection / tryConductSharedCollectionForPoll
    // tails). Flag-off = today's caller-bracketed hold (CG-I0).
    //
    // PHASE-STORE ORDER (F22, NORMATIVE under any §13.2 flag):
    // finishChangingPhase's phase stores complete BEFORE this close's GCL
    // release, so every reader (§3.4 guards, the §9.1(2) ctor; F34 leaves no
    // other) is GCL-ordered. Flag-off unaffected (callers hold GCL across
    // it). CG-I4. At C0 the non-final arm below is flag-dead; CG-3 wires it
    // into the finishChangingPhase periphery pairing.
    RELEASE_ASSERT(isSharedServer());
    RELEASE_ASSERT(m_worldIsStoppedForAllClients.load(std::memory_order_acquire));

    // SPEC-congc §5.3(2) (CG-2) — fence/threshold republish, WSAC still held
    // (pre-close): copy the server master pair into EVERY client's §5.3(2)
    // copy and stamp m_fenceEpochSeen = FEP. Clients never write these
    // fields; running at every close means a RAISE completes for all clients
    // before the window that published it closes and a LOWER (final window
    // only, post-termination) likewise — over-fenced is always sound
    // (CG-I3). Runs for every ISS close, not just C1R: flag-off the copies
    // are unrouted, unread state (ANNEX CGD4.4 vacuous-by-construction arm)
    // and the walk is conductor-only, in-window — invisible to the flags-off
    // gates (no client thread executes it). The §5.3(4) WND-close debug
    // assert follows the loop.
    {
        uint64_t fenceEpoch = m_barrierFenceEpoch.load(std::memory_order_acquire);
        bool masterFenced = m_mutatorShouldBeFenced;
        unsigned masterThreshold = m_barrierThreshold;
        clientSet().forEach([&](GCClient::Heap& client) {
            client.m_mutatorShouldBeFenced = masterFenced;
            client.m_barrierThreshold = masterThreshold;
            client.m_fenceEpochSeen = fenceEpoch;
        });
#if ASSERT_ENABLED
        // §5.3(4): every client saw the latest FEP before this close. The
        // master pair is conductor-mutated, in-window only once ISS, so a
        // mismatch here means a mutation raced this close — a protocol bug.
        clientSet().forEach([&](GCClient::Heap& client) {
            ASSERT(client.m_fenceEpochSeen == m_barrierFenceEpoch.load(std::memory_order_acquire));
        });
#endif
    }

    // Step 8 — resume (heap), strictly before the VMM resume (normative).
    // resumeAllocating() on all client caches: idempotent — the cycle's
    // final resumeThePeriphery() already resumed every directory-linked
    // allocator; this pass re-checks each client cache slot while still
    // owning the stop (I2 exception).
    clientSet().forEach([&](GCClient::Heap& client) {
        client.threadLocalCache().resumeAllocating();
    });

    // UNGIL ANNEX ISB1.1 (U-T5, review round): the cheap conservative form
    // bumps the stop-generation counter for EVERY conductor — including this
    // §10 shared-GC conductor, whose cycle jettisons and patches code. A
    // gilOff mutator that parked in the F8 barrier (NOT an NVS exit — no
    // unconditional ISB) resumes through the ISB1.2 compare in
    // acquireHeapAccess, which is sound only if this window bumped; without
    // it an arm64 mutator re-enters patched/jettisoned JIT code with no
    // context-synchronizing instruction. Patcher-side ifetch publication
    // first; the bump is INSIDE the stop window, sequenced before the
    // seq_cst GSP clear below, and a re-acquirer reaches JIT code only after
    // its seq_cst F8 GSP load observes that clear — the same
    // synchronizes-with edge the §A.3 conductor gets from its stop-word
    // clear (ISB1.5). Under the window model this runs at EVERY close
    // (CG-F4). gilOff-process only: flag-off/GIL-on zero cost.
    if (VM::isGILOffProcess()) [[unlikely]] {
        WTF::crossModifyingCodeFence();
        jsThreadsBumpStopGeneration();
    }

    // §3.7 ATOM-TABLE PIN (F46; ANNEX CGD7.3): restore before this close's
    // GCL release. Between windows the closed loop performs NO AtomString
    // create/deref (CG-I27) — debug builds null the conductor's table so any
    // violation crashes deterministically. Flag-off: dead (the tenure-wide
    // install in conductSharedCollection restores at function return).
    if (sharedGCWindowedConductActive()) {
        if (isFinalClose) {
            Thread::currentSingleton().setCurrentAtomStringTable(m_sharedGCWindowSavedAtomStringTable);
            m_sharedGCWindowSavedAtomStringTable = nullptr;
        } else {
#if ASSERT_ENABLED
            Thread::currentSingleton().setCurrentAtomStringTable(nullptr); // CG-I27.
#else
            Thread::currentSingleton().setCurrentAtomStringTable(m_sharedGCWindowSavedAtomStringTable);
#endif
        }
    }

    {
        Locker locker { m_gcBarrierLock };
        m_worldIsStoppedForAllClients.store(false, std::memory_order_seq_cst); // Clear WSAC pre-resume (F7).
        m_gcStopPending.store(false, std::memory_order_seq_cst); // GSP = false (F8).
        m_gcBarrierCondition.notifyAll(); // Broadcast GBC: revert-blocked acquirers retry.
    }

    // Step 9 — resume (VMM): wakes manifest-5b-parked mutators (5e notify);
    // GC is never latched (5c), so they exit or re-latch another reason (5f);
    // didResume hooks re-acquire access (5a).
    VMManager::requestResumeAll(VMManager::StopReason::GC);

    if (!isFinalClose) {
        RELEASE_ASSERT(sharedGCWindowedConductActive()); // Flag-dead at C0 (CG-I0: one window per conduct); §7.1a gilOff single-handoff also reaches the non-final close.
        // CG-I12: GCL released between windows — what lets a JSThreads stop
        // interleave (§9.1(1)). F22: the phase stores above (wired by CG-3)
        // completed before this release.
        m_gcConductorLock.unlock();
    }
    // Final close: GCL stays HELD (F23); the conductor re-acquires its own
    // access only at the landed conduct tail, after this close (§3.2).
}

void Heap::waitBetweenSharedGCWindows()
{
    // SPEC-congc §3.7 closed-loop between-window wait (F13; ANNEX CGD1.3
    // GOVERNS — the legacy drain arms are UNSOUND here): donateAll() +
    // waitForTermination(m_scheduler->timeToStop()) — condvar-only under
    // m_markingMutex, in NEITHER marker counter, never visitChildren, never
    // drainInParallelPassively/drainFromShared (whose access-state branch
    // keys on the MAIN client's unrelated state and whose MainDrain park has
    // no §9.1(2) checkpoint — UAF/deadlock/wakeup-race walks in CGD1.3).
    // Wake-ups: helper notifyAll + the scheduler timeout.
    // numberOfGCMarkers()==1: Concurrent is NEVER scheduled (a passive
    // conductor would wait forever — nobody drains), per §3.7.
    // Flag-dead at C0; CG-3 calls this from runConcurrentPhase's ISS arm
    // (also reached under the §7.1a gilOff single-handoff).
    RELEASE_ASSERT(sharedGCWindowedConductActive());
    m_collectorSlotVisitor->donateAll();
    m_collectorSlotVisitor->waitForTermination(m_scheduler->timeToStop());
}

void Heap::conductSharedCollection(GCClient::Heap& conductorClient) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // §10 steps 3-9, restructured as ONE GCA TENURE of stop windows
    // (SPEC-congc §3; CG-1). Pre: GCL held (rank 2), GCA set +
    // m_gcConductorThread stamped; the conductor runs as the mutator
    // (GCConductor::Mutator, §10B.2) and may be VM-less. Flag-off: exactly
    // ONE window (open(FirstWindow) ... close(final)) — the landed §10
    // sequence byte-for-byte (CG-I0; the runFixpointPhase stays-stopped
    // fixpoint keeps the world suspended for the entire cycle, Deviation 4).
    RELEASE_ASSERT(isSharedServer());

    // §3.5 conductor identity (CG-3a): publish the conducting client for the
    // tenure — finishChangingPhase's §7.1 WND-reopen arm passes it to
    // openSharedGCStopWindow(Reentry). Conductor-private (read only on this
    // thread inside the closed loop); cleared before the tail access
    // re-acquire ends the tenure.
    RELEASE_ASSERT(!m_sharedGCConductorClient);
    m_sharedGCConductorClient = &conductorClient;
    auto conductorClientScopeExit = makeScopeExit([&] {
        m_sharedGCConductorClient = nullptr;
    });

    // WND-open #1 (F15 first-window carve-out): §10 steps 3-4.
    openSharedGCStopWindow(conductorClient, SharedGCWindowOpen::FirstWindow);

    // The conducted cycle below (step 7: runEndPhase, finalize(),
    // conductor-side synchronous sweeps, §11 destroy thunks) runs on THIS
    // thread but tears down state of the one main VM (deviation 3):
    // finalize() clears vm().keyAtomStringCache / jsonAtomStringCache /
    // stringSplitCache, and deleteUnmarkedCompiledCode() derefs Identifiers —
    // any of which can take an AtomStringImpl's refcount to zero. A dying
    // atom is removed from the CURRENT THREAD's AtomStringTable
    // (AtomStringImpl::remove), so a conductor that is not the main VM's
    // entered thread would target its own per-thread table and trip the
    // cross-thread removal RELEASE_ASSERT ("string table of an other
    // thread"). Install the main VM's table for the stopped region: every
    // mutator is parked behind the §10.4 barrier, so this cannot race the
    // owner — the same license as runEndPhase's "main thread is suspended"
    // note (and the worldIsStoppedForAllClients() tolerance on the
    // requestCollection atom-table assert). No-op when the conductor is the
    // main VM's entered thread (JSLock already installed this table).
    // Restored by the scope-exit at function return (see the step-8 note).
    //
    // F46 (ANNEX CGD7.3): this TENURE-WIDE install is the FLAG-OFF form only
    // — its "every mutator is parked" license is true with one window and
    // FALSE between windows (the owner thread runs JS and mutates the
    // table). Windowed stages install PER WINDOW instead (WND-open after
    // WSAC / WND-close before GCL release — see the open/close helpers);
    // between windows the §3.7 closed loop takes no atom operations
    // (CG-I27).
    auto* previousAtomStringTable = sharedGCWindowedConductActive()
        ? nullptr
        : Thread::currentSingleton().setCurrentAtomStringTable(vm().atomStringTable());
    auto atomStringTableScopeExit = makeScopeExit([&] {
        if (!sharedGCWindowedConductActive())
            Thread::currentSingleton().setCurrentAtomStringTable(previousAtomStringTable);
    });

    // Step 5 — flush: every client's LocalAllocators (TLC non-iso slots and
    // the registered GCClient::IsoSubspace allocators) are linked into the
    // shared BlockDirectories' m_localAllocators lists, so the conducted
    // cycle's first stopThePeriphery() -> m_objectSpace.stopAllocating()
    // flushes every client's caches (I2 exception: conductor while WSAC).
    // LocalAllocator::stopAllocating() is not idempotent (it asserts
    // !m_lastActiveBlock), so the flush happens exactly once — inside the
    // cycle, not eagerly here. T8 audit of the N-client stop/resume/sweeper
    // interplay: stop/prepare/resume directory iterations assert
    // WSAC v MSPL v !ISS (MarkedSpace/LocalAllocator); BlockDirectory::
    // stopAllocating()'s inUse-empty check verifies every client's handles
    // were returned (no thread can park inside an MSPL section — MSPL holders
    // always hold access, so the §10.4 barrier excludes them; this includes
    // the teardown path: GCClient::Heap::~Heap acquires access BEFORE
    // lastChanceToFinalize()'s MSPL section, review round 1); the
    // The IncrementalSweeper runs in restricted shared mode once ISS (T4(d):
    // MSPL-held per-block steps, no physical frees) alongside in-lock
    // allocation-path sweeps and conductor-side synchronous sweeps.

    // Step 6 — stacks (T6): gatherStackRoots()'s MachineThreads scan
    // suspends-and-copies every I4(b)-registered thread; the conductor's own
    // state flows through m_currentThreadState. Registration is enforced in
    // GCClient::Heap::acquireHeapAccess (and the forwarding transfer branch),
    // so by the time the §10.4 barrier completed, every thread that ever
    // held access is scannable; the CLoop stack case is handled inside
    // gatherStackRoots (per-VM, main VM only in phase 1).

    // Step 7 — collection: full synchronous collection per §10B, conductor
    // as the mutator; drains ALL granted tickets (§10B.1). Deviation 4 keeps
    // the world suspended for the entire cycle (no Concurrent phase; see
    // runFixpointPhase). Parallel marking inside the stop stays (I5 helpers).
    // U-T12: §D.1 rebias may only run inside a full shared collection's stop
    // (ANNEX D1). Only the scope of each drained batch's LAST cycle is
    // observable after collectInMutatorThread(), so this is true when the
    // last cycle of some batch of this conduct was Full.
    //
    // Ticket drain: a ticket granted while this conduct runs is served by
    // collectInMutatorThread()'s own phase loop, or by the loop's
    // re-iteration, INSIDE the predecessor cycle's still-open final window
    // (WSAC set, GCL held) — finishChangingPhase closes a window only on the
    // edge into Concurrent, so End -> NotRunning closes nothing and the one
    // final close is below, after the loop's m_requests check.
    bool sawFullCollectionThisStop = false;

    // UNGIL §D.1 TID rebias executor (ANNEXES D1 + D1R; U-T12 — see the
    // banner on conductTIDRebiasUnderSharedStop above), shared by the two
    // placement arms below (§8.3/ANNEX CGD5.1 windowed pin, CG-3b; vs the
    // landed flag-off post-loop position). Both arms run: still inside a
    // §10 stop (WSAC set), after the cycle's quarantine/reclaim bar,
    // strictly BEFORE the covering window's ISB1.1 generation bump (whose
    // crossModifyingCodeFence + the F8 GSP-clear synchronizes-with edge are
    // the D1R item-2 resume-side sync for the fires' jettisons) and that
    // window's step-8/9 resumes. The Sealed -> Restamped flip is what
    // licenses the POST-RESUME mutator-side m_freeTIDs release
    // (ThreadManager phase 3, the SD9 gate-lift site) — so restamp + fire
    // are complete before any dead TID can be reissued; a snapshot sealed
    // after the running cycle's rebias point stays Sealed across
    // between-window/between-cycle mutator execution (CGD5.1(3) — the
    // gate-lift is licensed ONLY by the flip). gilOffProcess-only;
    // flag-off/GIL-on dead (U19/golden-disasm: zero behavior delta).
    auto runTIDRebiasIfSnapshotSealed = [&](bool fullCollectionSeen) {
        if (!VM::isGILOffProcess()) [[likely]]
            return;
        auto& threadManager = ThreadManager::singleton();
        if (const Vector<uint16_t>* deadTIDs = threadManager.rebiasSnapshotForConductor()) {
            // Single-consumer proof for the Sealed snapshot (the
            // Sealed -> Restamped edge must have exactly ONE potential
            // writer): under gilOffProcess the only heap that can be a
            // shared server is the U0c winner's — shared-server-ness is
            // PROCESS-unique via the I13 s_stickySharedServer CAS (the sole
            // m_isSharedServer=true site, noteSharedServerSticky, RELEASE_
            // ASSERTs it), and the winner heap took that CAS in its VM
            // ctor. So isSharedServer() (asserted at function entry) + the
            // process flag already imply this is the winner heap; this
            // assert makes the implication enforced rather than argued, so
            // no future second-server shape could restamp the WRONG heap
            // and release dead TIDs that still alias winner-heap tags.
            RELEASE_ASSERT(vm().gilOff());
            if (fullCollectionSeen) {
                conductTIDRebiasUnderSharedStop(*this, *deadTIDs);
                threadManager.noteRebiasRestampComplete();
            }
            // else: the snapshot sealed mid-stop (un-gated carrier-exit
            // retire under TM::m_lock) or no Full collection ran in this
            // arm's scope — it stays Sealed, and shouldDoFullCollection()'s
            // probe arms the NEXT conducted cycle as Full (the D1 trigger),
            // which performs the rebias in ITS final window.
        }
    };

    for (;;) {
        {
            Locker locker { *m_threadLock };
            if (m_requests.isEmpty()) {
                ASSERT(m_lastServedTicket == m_lastGrantedTicket);
                break;
            }
            if (sharedGCPreventGateBlocksNextTicket(locker)) [[unlikely]]
                break;
        }
        // collectInMutatorThread() runs runCurrentPhase until runNotRunningPhase
        // finds m_requests empty (or the prevent gate blocks the next ticket), so
        // it drains every servable ticket and only the
        // batch's last cycle is observable here (m_lastCollectionScope). The
        // hooks, reclaim and rebias below therefore run once per drained
        // BATCH, in the final window of that last cycle (WSAC set, GCL held:
        // the End -> NotRunning edge closes no window).
        collectInMutatorThread();
        bool cycleWasFull = m_lastCollectionScope && m_lastCollectionScope.value() == CollectionScope::Full;
        if (cycleWasFull)
            sawFullCollectionThisStop = true;

        // WINDOWED arm. A batch of several tickets resumes mutators between a
        // later cycle's windows before the earlier cycle's retired items and
        // quarantines are reclaimed here; that only delays frees and the TID
        // release (the quarantine and epoch bars are still crossed inside a
        // stop with WSAC set), it never frees early. A Full cycle followed by
        // an Eden cycle in one batch leaves the Sealed snapshot untouched;
        // shouldDoFullCollection() then arms the next conducted cycle as Full
        // and that conduct performs the rebias. Everything here is still
        // strictly before the covering window's ISB bump and WSAC/GSP clears,
        // which the next close (a later batch's non-final close, or the
        // final close below) performs.
        if (sharedGCWindowedConductActive()) [[unlikely]] {
            runSafepointHooksAndReclaim();
            runTIDRebiasIfSnapshotSealed(cycleWasFull);
            reclaimSharedGCMemoryAtCycleEnd();
        }
    }

    // Step 7 tail — FLAG-OFF arm (landed position, byte-for-byte: CG-I0;
    // one window per conduct makes the post-loop position and the per-batch
    // final-window position identical): still stopped, shared mode fires the
    // safepoint hooks HERE (§9 contract notes; = OM §6's quarantine bar — the
    // legacy runEndPhase site is skipped when isSharedServer()), followed by
    // the §11 reclaim sequence (I11) under the reclaimer's own
    // compiler-thread suspension, then the §D.1 rebias.
    if (!sharedGCWindowedConductActive()) {
        runSafepointHooksAndReclaim();
        runTIDRebiasIfSnapshotSealed(sawFullCollectionThisStop);
        // T4(d): world-stopped physical reclamation — the only point in the
        // shared steady state where MC-SAFE S4's world-stopped-only rule is
        // satisfied every cycle. See the helper for the policy.
        reclaimSharedGCMemoryAtCycleEnd();
    }

    // End of the conductor's main-VM teardown work. Flag-off, the
    // conductor's own AtomStringTable is restored by atomStringTableScopeExit
    // at function return — i.e. after the final WND-close below. That
    // ordering is acceptable because nothing past this point touches atoms:
    // steps 8-9 only flip allocator/barrier/VMM state, and acquireHeapAccess
    // does not create or destroy strings.

    // FINAL WND-close (F23 carve-out: GCL stays HELD — released by the
    // landed caller): §10 steps 8-9.
    closeSharedGCStopWindow(true /* isFinalClose */);

    // Re-acquire our own access (§3.2: only at this landed tail, after the
    // FINAL WND-close); the §10.2 loop then re-checks the ticket.
    conductorClient.acquireHeapAccess();

    // §F.3 carve-out (b) (MC-GC S5 / CVE-AUDIT B7): post-resume,
    // entered-with-access — drain the addFinalizer lambdas deferred from
    // inside the stop window(s) above. See drainDeferredLambdaFinalizers.
    drainDeferredLambdaFinalizers();
}

void Heap::reclaimSharedGCMemoryAtCycleEnd()
{
    // T4(d) — decommit restoration for the shared server. Before this
    // landed, capacity was monotone once ISS: MarkedSpace::shrink() (the
    // only OS-return path for marked blocks) is world-stopped-only when
    // shared (MC-SAFE S4), and the only world-stopped sweep/shrink sites
    // were shouldSweepSynchronously() (critical-memory/mini-mode only) and
    // teardown — so the steady state NEVER returned a block (rss profile:
    // 12,481MB of 13,430MB RSS was marked-live committed capacity vs ~378MB
    // true live; an explicit full GC reclaimed 1MB). This runs once per
    // drained ticket batch, inside the final stop window of the batch's last
    // cycle (whose scope m_lastCollectionScope reports):
    //  - every cycle: m_objectSpace.shrink() — frees the blocks already
    //    judged empty by this cycle's marking (BlockDirectory::endMarking's
    //    empty = live & ~markingNotEmpty), skipping destructible and inUse
    //    blocks (allocator-held blocks carry inUse from resumeAllocating's
    //    re-take, so a TLC-held free list can never be freed under it).
    //    Cheap: a per-directory bitvector scan plus the frees themselves.
    //  - full cycles whose committed capacity carries >= 50% slack over the
    //    marked size: a full synchronous sweep first (sweepSynchronously —
    //    MSPL is safely takeable while stopped, and its shrink leg is
    //    world-stopped here), which runs destructors so DESTRUCTIBLE empty
    //    blocks (excluded from plain shrink) and the weak-bearing blocks
    //    skipped by mutator-concurrent sweeps also become reclaimable. The
    //    slack gate keeps the full-sweep cost off tight steady-state loops;
    //    once capacity tracks live size again the gate stays closed.
    // Conductor context: same license as finalize()'s conductor-side
    // synchronous sweeps (the main VM's atom table is installed for the
    // stopped region; destructors may deref Identifiers).
    RELEASE_ASSERT(isSharedServer());
    RELEASE_ASSERT(worldIsStoppedForAllClients());
    if (m_lastCollectionScope && m_lastCollectionScope.value() == CollectionScope::Full) {
        size_t slackBase = std::max(m_sizeAfterLastCollect, minHeapSize(m_heapType, m_ramSize));
        if (capacity() > slackBase + slackBase / 2) {
            sweepSynchronously(); // Includes the world-stopped shrink leg.
            return;
        }
    }
    // B2-serial-eden-block-churn (b): RETAIN the steady-state empty-block set
    // across cycles instead of mmap/munmap-churning it. The unconditional
    // every-cycle shrink() below was the T4(d) decommit-restoration fix for
    // the monotone-capacity bug; with the (a) arm above now keying
    // m_maxHeapSize on a single-client base, a serial section's per-cycle
    // budget is bounded again, so committed capacity tracks m_maxHeapSize
    // and shrinking it every cycle just frees blocks that the very next
    // cycle re-mints via tryAllocateBlock -> MarkedBlock::tryCreate ->
    // bmalloc mmap (kernel page-fault: _raw_spin_lock + asm_exc_page_fault +
    // sync_regs + __list_del_entry + __free_one_page = ~2900M cycles of the
    // §28 W=16 1875ms penalty). When committed capacity already fits inside
    // the next cycle's m_maxHeapSize budget, KEEP the empties: endMarking
    // has set canAllocate = live & ~markingRetired on them, so the stripe
    // leg's tryAllocateFromOwnDirectory picks them up next cycle with NO
    // fresh page (the "recycles a steady-state block set" goal). Only shrink
    // when capacity overshoots the budget (genuine over-commit — e.g. the
    // first cycle after a wide parallel section collapses to serial), so RSS
    // stays bounded by ~m_maxHeapSize and the original T4(d) decommit
    // guarantee is preserved at the bound. m_maxHeapSize was just assigned by
    // updateAllocationLimits (this runs strictly after it in the conducted
    // cycle's final stop window). isSharedServer()-only path (RELEASE_ASSERT
    // above): flag-off never reaches here.
    if (m_objectSpace.capacity() <= m_maxHeapSize)
        return;
    m_objectSpace.shrink();
}

void Heap::runSafepointHooksAndReclaim()
{
    // §9 contract notes + §11 reclaim sequence. Sole call sites (I11): §10
    // step 7 (shared, conductSharedCollection) and the legacy runEndPhase
    // site (!isSharedServer(), including option-off — the I10 exemption).
    // Never from a JSThreads stop (jit R4/CS4 refused; such stops enqueue a
    // GC request instead, §13.10a).
    ASSERT(worldIsStopped() || worldIsStoppedForAllClients());

    runStopTheWorldSafepointHooks();

    // V5b fast path (I10): when nothing is retired, bumpAndReclaim() is a
    // documented no-op (§11 empty-check: no bump, no client iteration), so
    // the reclaimer's compiler-thread suspension (I11(c)) would license
    // nothing and the localEpoch stamping loop (I11(a)) would feed nothing —
    // a later cycle that DOES find retired items re-stamps every client to
    // the then-current epoch before its own bump, so skipping the stamp here
    // can never shrink that later min(localEpoch). Flag-off the only retire()
    // feeder is the displaced ArrayBuffer wrapper Weak in
    // SimpleTypedArrayController::registerWrapper, so this is the common
    // every-eden-GC path: skip the suspend/resume pair and the bracket
    // instead of paying them to license a no-op. A racing in-stop retire()
    // landing just after this check simply waits for the NEXT reclaim
    // sequence — the same sound, later-destruction outcome bumpAndReclaim's
    // own under-bracket empty-check already permits for items retired during
    // the current stop window (epoch == oldEpoch survives the bump).
    if (!m_safepointEpoch.hasRetiredItems())
        return;

    // I11: compiler threads must be suspended across the bump by the
    // reclaimer's OWN suspend/resume pair — a conducted cycle's periphery
    // suspension does not by itself license a bump (bumpAndReclaim
    // release-asserts the bracket below, T7). JITWorklist's suspension lock
    // is not recursive, so when this thread already holds the cycle's
    // suspension (stopThePeriphery set m_isCompilerThreadsSuspended; it is
    // released only in resumeThePeriphery, after this call site in both
    // protocols), the reclaimer's bracket explicitly ADOPTS that suspension —
    // this thread holds it across the whole bracket — instead of re-entering
    // suspendAllThreads(); otherwise it takes a fresh pair.
    // suspendCompilerThreads() returning false with no prior suspension means
    // there are no compiler threads to suspend (JIT off / no active plans):
    // I11(c) is vacuous.
    bool reclaimerSuspended = false;
    if (!m_isCompilerThreadsSuspended)
        reclaimerSuspended = suspendCompilerThreads();

    // The bracket is the I11(c) license: it opens only here, only with the
    // suspension (fresh, adopted, or vacuous) established, and closes before
    // that suspension can be released.
    m_safepointEpoch.beginReclaimerBracket();

    // §11: publish each registered client's local epoch exactly — the world
    // is stopped (legacy mutator suspended, or stopped for all clients), so
    // no client is between heap operations.
    uint64_t epoch = m_safepointEpoch.current();
    clientSet().forEach([&](GCClient::Heap& client) {
        client.m_localEpoch.store(epoch, std::memory_order_seq_cst);
    });

    // B14 / MC-DOS S7 (SPEC-jit §4.4): the per-client stamp above IS the
    // per-thread epoch publication. bumpAndReclaim()'s min scan
    // (GCSafepointEpoch.cpp:148) reads GCClient::Heap::m_localEpoch only;
    // under U-T6 (per-thread clients — JSLock perThreadClientForCarrierEntry
    // + the spawned-lite client in ThreadManager.cpp) every JS-executing lite
    // owns a distinct registered client, so min-over-clients == min-over-JS-
    // threads, and bumpAndReclaim()'s RELEASE_ASSERT(minLocalEpoch >=
    // oldEpoch) backstops a missed stamp. An earlier B14 revision additionally
    // walked VMLiteRegistry to stamp a per-lite slot — dropped at adversarial
    // review as write-only dead code (no reader) that added a release-build
    // lock-site inside the conductor stop for zero functional gain.
    // STANDING OBLIGATION: if U-T6 ever weakens (a lite executing JS without
    // its own registered GCClient::Heap, or two lites sharing one client
    // under a future carrier path), epoch-expiry reclamation
    // (RetiredJITArtifacts) collapses into a handler-chain/record UAF — that
    // change MUST restore the per-lite witness AND wire it into
    // bumpAndReclaim()'s min scan.

    m_safepointEpoch.bumpAndReclaim();

    m_safepointEpoch.endReclaimerBracket();

    if (reclaimerSuspended)
        resumeCompilerThreads();
}

void Heap::pollIssRevertIfNeeded() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // UNGIL §0 U0c (ANNEX U0C; U-T1): the §10D m_isSharedServer=false arm is
    // conditioned on !gilOffProcess — under gilOffProcess the designated
    // server stays ISS for process lifetime (codegen and lite gilOff bytes
    // were stamped against gilOff=1; un-sharing would not un-stamp them; a
    // GIL-off process that joins all threads keeps shared-server overheads —
    // accepted). Disarm the hint so the poll stays cheap.
    if (VM::isGILOffProcess()) [[unlikely]] {
        m_issRevertPending.store(false, std::memory_order_relaxed);
        return;
    }

    // §10D ISS reversion: performed by the MAIN client's thread at a
    // CIND/SINFAC poll — never inside HeapClientSet::remove() or a stop
    // window. One bounded attempt per poll: if the server is not yet
    // ticket-quiescent the caller's own poll will service the tickets first
    // and we retry at a later poll (this also keeps §10B.4's liveness rule —
    // never block indefinitely while granted-unserved tickets exist).
    ASSERT(isSharedServer());
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    if (!client || client != m_mainClient)
        return;
    ASSERT(client->hasHeapAccess());

    // (1) GCL tryLock must succeed (no conductor, no JSThreadsStopScope).
    if (!m_gcConductorLock.tryLock()) // §3.4 disposition (ANNEX CGD6.2 row: §10D revert poll).
        return;
    {
        Locker locker { *m_threadLock };
        // While a conductor is winding down, GCA may still be set: wait
        // briefly (§10B.4-style timed waits), then re-check once.
        //
        // SPEC-congc §3.4 RESTRUCTURE (F11; ANNEX CGD1.2/CGD6.2 §10D
        // revert-poll row): the landed unconditional wait-while-GCA is sound
        // only while GCA-true/GCL-free lasts the few wind-down instructions.
        // Once a cycle has between-window gaps, this poller — holding GCL
        // AND heap access — would wait for a GCA that cannot clear until the
        // cycle ends, while the cycle's next WND-open blocks on the GCL we
        // hold and the §10.4 barrier on the access we hold: permanent
        // (CGD1.2 walk). Mid-cycle (phase != NotRunning): back off, hint
        // stays armed, retry at a later poll. Bounded wait ONLY when
        // NotRunning. Flag-off the back-off never fires (any GCL-free point
        // is NotRunning — CGD1.1 flag-off half; CG-I0).
        while (m_gcConductorActive) {
            if (m_currentPhase != CollectorPhase::NotRunning) {
                m_gcConductorLock.unlock();
                return;
            }
            m_gcElectionCondition.waitFor(*m_threadLock, 1_ms);
        }
        bool quiescent = m_issRevertPending.load(std::memory_order_relaxed)
            && m_lastServedTicket == m_lastGrantedTicket
            && !m_collectorThreadIsRunning
            && m_currentPhase == CollectorPhase::NotRunning
            // Review round 2: SINFAC runs this poll BEFORE its isDeferred()
            // gate, so we can legally arrive here inside an open DeferGC
            // scope (m_deferralDepth > 0; stopIfNecessary() has many runtime
            // call sites under deferral). That defers the REVERT — not a
            // protocol violation: clearing ISS now would re-route the open
            // scope's pending decrements to the server counter (I17
            // imbalance), so we leave the hint armed and retry at a later
            // poll. This condition is what licenses the RELEASE_ASSERT
            // below. Safe to read here: m_deferralDepth is touched only by
            // this client's access-holding thread, which is us (asserted at
            // entry).
            && !client->m_deferralDepth;
        if (quiescent) {
            // (2) The size() == 1 re-check and the ISS clear are ONE atomic
            // step under the registry lock (rank 6 inside ranks 2/5, §6;
            // review round 1): HeapClientSet::add()'s already-shared insert
            // re-checks isSharedServer() under this same lock, so a
            // concurrent add() can never interleave between the size sample
            // and the clear — the TOCTOU that would otherwise yield two
            // registered clients with isSharedServer() == false.
            m_clientSet.withSizeUnderRegistryLock([&](unsigned size) {
                if (size != 1) {
                    // A new client raced in: stay shared. Disarm the hint —
                    // we hold *m_threadLock (the flag's writer lock), so this
                    // cannot lose a concurrent remove()'s re-arm: a remove()
                    // that takes the survivor back down to 1 arms the flag
                    // under this same lock, after us.
                    m_issRevertPending.store(false, std::memory_order_relaxed);
                    return;
                }
                // Clear ISS + the flag. Deviation-4 features and the server
                // deferral counter re-enable; the survivor's per-client depth
                // MUST be 0 (I17) — its open scopes would otherwise decrement
                // the wrong counter after the flip. Guaranteed by the
                // quiescent condition above (review round 2: a deferred
                // caller retries at a later poll instead of asserting), and
                // the depth cannot have grown since: only this thread
                // increments it.
                RELEASE_ASSERT(!client->m_deferralDepth);
                // Review round 4: migrate any pending per-client deferred-GC
                // hint back to the server flag — didDeferGCWorkSlot() routes
                // to the server again once ISS clears, so a hint left on the
                // client would be orphaned. Only this thread touches either
                // slot here (we are the main client's access-holding thread).
                if (client->m_didDeferGCWork) {
                    m_didDeferGCWork = true;
                    client->m_didDeferGCWork = false;
                }
                m_issRevertPending.store(false, std::memory_order_relaxed);
                m_isSharedServer.store(false, std::memory_order_seq_cst);
                // Residual m_retired items drain via §11's legacy runEndPhase
                // site; the §10A.1 TLS slot stays stamped; a later add()
                // re-runs §10B.4 (the I13 assert keys on current ISS).
            });
        }
    }
    m_gcConductorLock.unlock();
}

void Heap::stopIfNecessaryForAllClients()
{
    ASSERT(!currentThreadHasSTWForbiddenScope()); // I14/L5.
#if ASSERT_ENABLED
    // SPEC-congc §8.2 CG-I18 (CG-3c): cell-lock-no-park — depth == 0 at
    // SINFAC entry. A JSCellLock (10a) holder must not pass a stop poll:
    // parking here would hold 10a across the stop window, breaking the
    // ANNEX CGN1 N3 termination argument (IN-WINDOW every 10a lock must be
    // free so the visitor's tryLock retries succeed). Stage-gated so
    // flag-off debug behavior is unchanged; the bookkeeping itself
    // (GCCellLockDepth, GCThreadLocalCache.h) is inert.
    ASSERT(!Options::useConcurrentSharedGCMarking() || !GCCellLockDepth::current());
#endif

    if (!isSharedServer()) {
        // Legacy single-client protocol (I10/I15). Call the slow path
        // directly: the inline Heap::stopIfNecessary() re-dispatches here
        // when ISS, so going through it again would recurse on an ISS flip.
        if (mayNeedToStop())
            stopIfNecessarySlow();
        return;
    }

    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    ASSERT(!client || &client->server() == this);
    if (!client || &client->server() != this)
        return;

    // §10A SINFAC: release -> wait -> re-acquire when shared and a stop is
    // pending. Precondition (I6): the caller holds no rank >= 4 lock and no
    // SAL — it may block here for the whole stop window.
    if (m_gcStopPending.load(std::memory_order_seq_cst)) [[unlikely]] { // GSP, F8 — the hot poll.
        if (client->hasHeapAccess()) {
            ASSERT(client->m_accessOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton()
                || !client->m_accessOwner.load(std::memory_order_relaxed)); // Null tolerated defensively (the §10B.4 migration always stamps the owner since review round 1).
            client->releaseHeapAccess(); // Signals the §10.4 barrier (GSP is set).
            client->acquireHeapAccess(); // F8: blocks until the conductor clears GSP.
        }
        // No access (e.g. the conductor's own polls mid-stop): nothing to do.
    }

    if (!client->hasHeapAccess())
        return;

    // SPEC-congc §4.1 (CG-2): SINFAC hot-poll exit didRun note, C1R-only
    // (F33) — this client kept access through the poll and is about to keep
    // running JS, so note it for the next window's fold (mirrors the AHA
    // success tail; the legacy stopIfNecessarySlow writes it consumed at
    // stopThePeriphery). Owner-thread relaxed store (heap I17/CG-I9).
    if (sharedGCBarrierStateIsPerClient()) [[unlikely]]
        WTF::atomicStore(&client->m_didRunSinceLastWindow, true, std::memory_order_relaxed);

    // SPEC-congc §5.2(ii) (CG-3c): out-of-window CMS threshold donation —
    // the SINFAC hot poll tail is the NORMATIVE (and only) trigger site
    // (after the GSP leg, access held; SINFAC I6 legalizes it: no rank >= 4
    // lock, no SAL held here). Donation is latency-only — the §5.2(i)
    // WND-open drains give correctness; this bounds CMS memory and feeds
    // between-window helper draining without waiting for the next window.
    // Cheap probe first: one terminal-leaf CMS lock acquisition
    // (LK.9c, bare — not a nested LK.9d>LK.9c site), no donation work
    // unless over Options::sharedGCMutatorMarkStackDonationThreshold().
    if (sharedGCBarrierStateIsPerClient()) [[unlikely]] {
        bool overThreshold = false;
        {
            // Bare terminal-leaf probe — not a nested LK.9d>LK.9c site (no
            // outer m_markingMutex; the donation below re-takes both).
            Locker cmsLocker { client->m_mutatorMarkStackLock };
            overThreshold = client->m_mutatorMarkStack
                && client->m_mutatorMarkStack->size() > Options::sharedGCMutatorMarkStackDonationThreshold();
        }
        if (overThreshold) [[unlikely]] {
            // Phase-read license (NOT an F34 site — F34 covers ACT/DCT
            // threads holding neither access nor a lock): this thread HOLDS
            // access, so no stop window can be open and no in-window phase
            // store can be concurrent — the phase is frozen for the whole
            // access tenure. Visibility of the LAST in-window store: it
            // happened-before its window's seq_cst GSP clear/WSAC clear,
            // which happened-before this thread's F8 seq_cst acquisition
            // (and this very poll's seq_cst GSP load above) — race-free,
            // no TSAN edge. Donate ONLY mid-cycle (the only out-of-window
            // mid-cycle phase is Concurrent): a between-cycles donation
            // into m_sharedMutatorMarkStack would pre-load the shared-stack
            // accounting before runBeginPhase's didReachTermination()
            // precondition (the CG-T8 Arm-1 RED class); between cycles
            // there is no marking latency to win, so we simply skip — the
            // next WND-open drain handles it (§5.2(i)).
            if (m_currentPhase != CollectorPhase::NotRunning) {
                ASSERT(m_currentPhase == CollectorPhase::Concurrent); // §8.1: Concurrent is the only non-suspended mid-cycle phase.
                Locker markingLocker { m_markingMutex };
                // LK.9d>LK.9c (lint R4 marker): the §5.2(ii) donation edge —
                // same forward chain as the WND-open drain.
                Locker cmsLocker { client->m_mutatorMarkStackLock };
                if (client->m_mutatorMarkStack && !client->m_mutatorMarkStack->isEmpty()) {
                    // Target m_sharedMutatorMarkStack (guarded by the held
                    // m_markingMutex): mid-cycle CMS work is accounted via
                    // the shared stacks (hasWork/didReachTermination count
                    // them), so between-window HelperDrain visitors steal
                    // it immediately; cells land before the cycle's
                    // termination pass by the same argument as the Reentry
                    // WND-open drain (any later fixpoint window's
                    // convergence counts the shared stacks first).
                    client->m_mutatorMarkStack->transferTo(*m_sharedMutatorMarkStack);
                    m_markingConditionVariable.notifyAll(); // Wake parked helpers (ANNEX CGD1.3 condvar).
                }
            }
        }
    }

    // §10D revert poll (main client only; relaxed read is a hint, re-checked
    // under *m_threadLock inside).
    if (m_issRevertPending.load(std::memory_order_relaxed)) [[unlikely]]
        pollIssRevertIfNeeded();
    if (!isSharedServer()) [[unlikely]]
        return; // Reverted just now: back to the legacy protocol (I15).

    // Serve granted-unserved tickets (mutator-driven triggering, §5.4/I15):
    // RCAC/CIND enqueue tickets with no waiting requester; this poll is what
    // conducts them. Mirrors the legacy "stopIfNecessary() will immediately
    // start the collection if we have the conn" behavior. Skip when this
    // client is deferred (I17) or when re-entered from GC/sweep internals.
    if (isDeferred())
        return;
    switch (mutatorState()) {
    case MutatorState::Running:
    case MutatorState::Allocating:
        break;
    case MutatorState::Sweeping:
    case MutatorState::Collecting:
        return;
    }
    bool ticketsPending = false;
    if (m_threadLock->tryLock()) { // Opportunistic: never contend on the hot poll.
        // W16-C1 residual (b): don't bother attempting a conduct the gate
        // would refuse anyway (the gate check inside
        // tryConductSharedCollectionForPoll() remains authoritative).
        ticketsPending = m_lastServedTicket < m_lastGrantedTicket && !m_gcConductorActive
            && (!m_sharedGCPreventCount || m_sharedGCPreventHolder == &Thread::currentSingleton())
            && !sharedGCPreventGateBlocksNextTicket(AbstractLocker(NoLockingNecessary));
        m_threadLock->unlock();
    }
    if (ticketsPending) [[unlikely]]
        tryConductSharedCollectionForPoll(*client);
}

void Heap::addStopTheWorldSafepointHook(void (*hook)(JSC::Heap&))
{
    RELEASE_ASSERT(hook);
    Locker locker { m_stopTheWorldSafepointHookLock };
    if (m_stopTheWorldSafepointHooks.contains(hook))
        return;
    m_stopTheWorldSafepointHooks.append(hook);
}

void Heap::runStopTheWorldSafepointHooks()
{
    // §9 contract notes: fires world-stopped, once per collection in the
    // legacy protocol and once per drained ticket batch in shared mode. Call
    // sites (T5, via runSafepointHooksAndReclaim): legacy runEndPhase just
    // before didFinishCollection() (worldIsStopped() asserted) and
    // shared-mode §10 step 7 (conductSharedCollection).
    Vector<void (*)(JSC::Heap&)> hooks;
    {
        Locker locker { m_stopTheWorldSafepointHookLock };
        hooks = m_stopTheWorldSafepointHooks;
    }
    for (auto hook : hooks)
        hook(*this);
}

void Heap::incrementSTWForbiddenScope()
{
    ++t_stwForbiddenScopeDepth;
}

void Heap::decrementSTWForbiddenScope()
{
    ASSERT(t_stwForbiddenScopeDepth);
    --t_stwForbiddenScopeDepth;
}

bool Heap::currentThreadHasSTWForbiddenScope()
{
    return !!t_stwForbiddenScopeDepth;
}

void Heap::verifyServerNonIsoAllocatorsNeverMaterialized()
{
    // §5.5 (T4): called from noteSharedServerSticky() at the second-client
    // attach, before sticky ISS is set.
    ASSERT(Options::useSharedGCHeap());
    objectSpace().forEachSubspace([&](Subspace& subspace) -> IterationStatus {
        if (subspace.kind() == SubspaceKind::CompleteSubspace)
            static_cast<CompleteSubspace&>(subspace).verifyNoAllocatorsMaterialized();
        return IterationStatus::Continue;
    });
}

bool Heap::currentThreadIsAllocatorOwner(const LocalAllocator* allocator) const
{
    if (isSharedServer()) {
        // §10A.1: the current thread owns `allocator` iff its TLS-stamped
        // client (a) belongs to this server and (b) has the allocator in its
        // TLC's per-directory map (T4: covers TLC-materialized non-iso
        // allocators and the registered GCClient::IsoSubspace allocators).
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (!client || &client->server() != this)
            return false;
        bool owns = client->threadLocalCache().ownsLocalAllocator(allocator);
        // Debug cross-check (I2): an owning thread must hold its client's
        // heap access.
        ASSERT(!owns || client->m_accessOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton());
        return owns;
    }
    // !ISS (option off, or on pre-sticky): today's predicate (I10; the
    // §10A.1 TLS slot may be unset here).
    return vm().currentThreadIsHoldingAPILock();
}

// --- §10A access forwarding (T2) ---

GCClient::Heap* Heap::gilOffClientForServerLevelAccess() const
{
    // SPEC-ungil §B.3: GIL-off every thread holds access on its OWN client,
    // so the server-level acquireAccess()/releaseAccess()/hasAccess() act on
    // the calling thread's client — the one its JSLock carrier or spawn
    // attach stamped into the §10A.1 slot, or the VM's original client for
    // the process main thread outside any entry (the main carrier reuses it).
    // The slot is never re-stamped here: JSLock is the GIL-off stamping
    // authority. A non-main thread without a client holds no access and is
    // never handed the main thread's; callers fail-stop on null.
    ASSERT(vm().gilOff());
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    if (client && &client->server() == this)
        return client;
    if (WTF::isMainThread())
        return m_mainClient;
    return nullptr;
}

NEVER_INLINE void Heap::acquireAccessForwardedToMainClient()
{
    GCClient::Heap* mainClient = m_mainClient;
    RELEASE_ASSERT(mainClient);
    if (vm().gilOff()) [[unlikely]] {
        GCClient::Heap* client = gilOffClientForServerLevelAccess();
        RELEASE_ASSERT(client); // A thread with no client of this server cannot acquire access through the server GIL-off.
        client->acquireHeapAccess(); // F8 step 0 makes a re-acquire by the holder idempotent.
        return;
    }
    // §10A.1: re-stamp the TLS slot before AHA — JSLock migration moves the
    // main VM between threads, and the slot is keyed per-thread.
    GCClient::Heap::setCurrentThreadClient(mainClient);
    // I2 (access-based, not thread-pinned): if the main client already holds
    // access — either this thread re-entering (JSLock recursion) or a JSLock
    // hand-off from a thread that kept access across the unlock (the
    // "permanent access" pattern) — transfer/confirm ownership. The caller
    // holds the main VM's API lock, so the previous owner cannot be running
    // inside the VM.
    if (mainClient->m_accessState.load(std::memory_order_seq_cst) == GCClient::Heap::hasAccessState) {
        // I4(b) (T6): the ownership-transfer branch bypasses AHA, but the
        // incoming thread becomes a heap-accessing mutator right here — its
        // stack must be in the §10.6 scan's root set before it touches the
        // heap (JSLock::didAcquireLock's own addCurrentThread() runs later).
        mainClient->ensureCurrentThreadIsRegisteredForConservativeScan(Thread::currentSingleton());
        mainClient->m_accessOwner.store(&Thread::currentSingleton(), std::memory_order_relaxed);
        return;
    }
    mainClient->acquireHeapAccess();
}

NEVER_INLINE void Heap::releaseAccessForwardedToMainClient()
{
    GCClient::Heap* mainClient = m_mainClient;
    RELEASE_ASSERT(mainClient);
    if (vm().gilOff()) [[unlikely]] {
        GCClient::Heap* client = gilOffClientForServerLevelAccess();
        RELEASE_ASSERT(client); // See acquireAccessForwardedToMainClient: never strip another thread's (the main client's) access.
        client->releaseHeapAccess();
        return;
    }
    mainClient->releaseHeapAccess();
}

bool Heap::mainClientHasHeapAccess() const
{
    GCClient::Heap* mainClient = m_mainClient;
    if (vm().gilOff()) [[unlikely]] {
        GCClient::Heap* client = gilOffClientForServerLevelAccess();
        return client && client->hasHeapAccess() && client->m_accessOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton();
    }
    if (!mainClient || !mainClient->hasHeapAccess())
        return false;
    // Owner-sensitive on purpose: after a JSLock migration the new thread
    // must see "no access" so JSLock::didAcquireLock re-enters
    // acquireAccess(), which transfers ownership and re-stamps the §10A.1
    // TLS slot (migration-safe re-stamping).
    return mainClient->m_accessOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton();
}

// --- End §10A access forwarding ---

// --- Manifest 5a GC park hooks (T5; SPEC-heap-annex.md §A5(a)) ---

void Heap::gcWillParkInStopTheWorld(VM& vm)
{
    // Heap-owned impl; idempotent; called from VMManager::notifyVMStop with
    // no VMM lock held (L6) — once after the park counter-increment, and
    // again before each wait while the GC bit pends (annex 5g(i)).
    // Rule: iff ISS && GSP && this VM's client holds access -> RHA + set
    // m_releasedByGCPark; else no-op.
    //
    // UNGIL §A.3.8 (U-T5; heap §13.5 re-rule): with N entered threads in ONE
    // gilOff VM, each thread parks on its OWN ticket and §13.5a/g run on
    // CURRENT THREAD's client — currentThreadClient() — with the per-client
    // m_releasedByGCPark pairing. vm.clientHeap is the MAIN client only and
    // would release/re-acquire the wrong client on every sibling thread.
    // No current client (e.g. a VM-construction park on a thread that never
    // attached) => nothing to release => no-op. GIL-on/flag-off: unchanged
    // (the landed vm.clientHeap resolution is exact under the GIL).
    GCClient::Heap* parkClient = &vm.clientHeap;
    if (vm.gilOff()) [[unlikely]] {
        parkClient = GCClient::Heap::currentThreadClient();
        if (!parkClient)
            return;
    }
    GCClient::Heap& client = *parkClient;
    JSC::Heap& server = client.server();
    if (!server.isSharedServer())
        return;
    if (!server.m_gcStopPending.load(std::memory_order_seq_cst)) // GSP, F8.
        return;
    if (client.m_releasedByGCPark) // Idempotent: 5g(i) re-fires.
        return;
    if (!client.hasHeapAccess())
        return;
    // The hook runs on the parking VM's own thread, which is the access
    // owner (null tolerated defensively; the §10B.4 migration always stamps
    // the owner since review round 1).
    ASSERT(client.m_accessOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton()
        || !client.m_accessOwner.load(std::memory_order_relaxed));
    client.releaseHeapAccess(); // Signals the §10.4 barrier (GSP is set).
    client.m_releasedByGCPark = true; // Written only inside notifyVMStop (§10A).
}

void Heap::gcDidResumeFromStopTheWorld(VM& vm)
{
    // Heap-owned impl; idempotent; called after notifyVMStop's final
    // decrement block. Rule: iff m_releasedByGCPark -> AHA (F8-blocking if a
    // NEW stop pends), then clear; else no-op (F8 step 0 backstops).
    //
    // UNGIL §A.3.8 (U-T5): per-thread client resolution, mirroring
    // gcWillParkInStopTheWorld above — the resume hook MUST re-acquire on
    // the same client the park hook released (per-client m_releasedByGCPark
    // pairing). GIL-on/flag-off unchanged.
    GCClient::Heap* parkClient = &vm.clientHeap;
    if (vm.gilOff()) [[unlikely]] {
        parkClient = GCClient::Heap::currentThreadClient();
        if (!parkClient)
            return;
    }
    GCClient::Heap& client = *parkClient;
    if (!client.m_releasedByGCPark)
        return;
    client.acquireHeapAccess(); // F8: blocks while a (new) stop is pending.
    client.m_releasedByGCPark = false;
}

// --- End manifest 5a GC park hooks ---

Heap::JSThreadsStopScope::JSThreadsStopScope(JSC::Heap& heap, MonotonicTime watchdogRequestStart) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    : m_heap(heap)
{
    // §10C/CS2: the GCL (rank 2) bracket for a JSThreads/debugger stop.
    // Pre: caller released heap access (I6); no bumpAndReclaim() inside the
    // scope (jit R4/CS4 refused — JSThreads stops enqueue a GC request).
    // !isSharedServer() => no-op. The GCL wait is covered by the 30s stop
    // watchdog — a conductor queued behind a shared GC that never converges
    // (or a GCL wedge) fail-stops with the standard timeout diagnostics
    // instead of hanging unwatched forever. Quantum: 1ms tryLock; cost is nil
    // on the uncontended path (first tryLock succeeds).
    if (!m_heap.isSharedServer())
        return;
    ASSERT(!currentThreadHasSTWForbiddenScope()); // I14/L5.
#if ASSERT_ENABLED
    // Pre (§9/I6): the caller's client released heap access before
    // bracketing — a JSThreads conductor must never stop the world while
    // still counted as a heap-accessing mutator (a concurrent GC requester
    // parked under the GCL-busy rule waits for OUR access state too).
    if (GCClient::Heap* client = GCClient::Heap::currentThreadClient())
        ASSERT(!client->hasHeapAccess());
#endif
    // SPEC-congc §9.1(2a) F45 (ANNEX CGD7.2): register as a foreign GCL
    // waiter BEFORE the first lock attempt; deregister once the lock is
    // HELD. The windowed conductor's WND-open re-entry abstains from its
    // blocking acquire while this counter is nonzero, so this scope cannot
    // be starved by back-to-back fixpoint windows. The !isSharedServer()
    // early return above never increments; the dtor never touches the
    // counter. Flag-off: maintained, never consulted (CG-I0; same cost
    // class as the watchdog bookkeeping).
    m_heap.m_foreignGCLWaiters.exchangeAdd(1, std::memory_order_relaxed);
    // §3.4 disposition (ANNEX CGD6.2 row: watchdog-ctor tryLock loop, F47):
    // PROCEED — NO between-windows back-off guard. A foreign mid-cycle GCL
    // hold is exactly what SPEC-congc §9.1(1) legalizes (today this tryLock
    // succeeds only when no conduct is in flight; post-spec it succeeds
    // between windows BY DESIGN). Obligations on success: the §9.1(2) marker
    // pause (CG-3b: pauseConcurrentMarkingForForeignStop, called below after
    // a SUCCESSFUL tryLock only, never per failed iteration) and the F45
    // deregister below.
    while (!m_heap.m_gcConductorLock.tryLock()) {
        // [r34] F-A item (3): thread the TARGET VM instead of nullptr so a
        // timeout attributes to the requesting VM (kills the
        // nil-Class-A-context misattribution signature). Under U0b every
        // client of one server belongs to that one VM, so the server-side
        // vm() IS the requester's VM (same routing as runSharedGCElection's
        // traps poll).
        JSThreadsSafepoint::watchdogAssertStopProgress(watchdogRequestStart, &m_heap.vm());
        WTF::sleep(Seconds::fromMilliseconds(1));
    }
    m_heap.m_foreignGCLWaiters.exchangeSub(1, std::memory_order_relaxed);
    m_didLock = true;
    // SPEC-congc §9.1(2) (CG-3b; ANNEX CGP1 GOVERNS): a foreign GCL holder
    // mid-cycle must not race marking helpers — when the phase is not
    // NotRunning (read GCL-ordered: F22 pins every phase store before the
    // publishing close's GCL release; mid-cycle tryLock success is between
    // windows BY DESIGN under the §13.2 stage flags), pause the HelperDrain
    // markers before this scope's §A.3 window does any work (jettison/patch;
    // the AB-10 weak-sweep license is sound only with markers paused,
    // CGD7.1(d)). The pause is a TIMED wait sampling the SAME requestStart
    // the tryLock loop sampled (one end-to-end 30s budget for this leg), so
    // a wedged marker batch fail-stops on this conductor with the same VM
    // attribution.
    if (m_heap.m_currentPhase != CollectorPhase::NotRunning) {
        m_heap.pauseConcurrentMarkingForForeignStop(watchdogRequestStart);
        m_didPauseConcurrentMarking = true;
    }
}

Heap::JSThreadsStopScope::~JSThreadsStopScope() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // §9.1(2) DTOR ORDER (NORMATIVE; SPEC-congc rule 2): resume the paused
    // markers strictly BEFORE releasing GCL — the GC conductor's next
    // WND-open blocks on GCL (its blocking re-acquire defers to F45 waiters,
    // then locks), so this order makes "no WND-open with paused markers"
    // structural. The marker resume postdates every free this scope's
    // window performed (ISB1.1 bump + rule 4 / the AB-10 license cover
    // them, CGD7.1(d)).
    if (m_didPauseConcurrentMarking)
        m_heap.resumeConcurrentMarkingAfterForeignStop();
    if (m_didLock)
        m_heap.m_gcConductorLock.unlock();
}

void Heap::pauseConcurrentMarkingForForeignStop(MonotonicTime requestStart)
{
    // SPEC-congc §9.1(2) marker-pause mechanism (CG-3b; ANNEX CGP1 BINDING
    // GOVERNS) + the [r34] F-A item (1) amendment (SPEC-ungil-history; the
    // CGS2A.4(a) pause is a TIMED wait sampling watchdogAssertStopProgress
    // per 1ms quantum — same quantum family as the watchdog ctor's tryLock
    // loop). Caller holds GCL with m_currentPhase != NotRunning.
    //
    // Participant set (F14): EXACTLY the helpers inside
    // drainFromShared(HelperDrain) — the counters' only maintainers. The
    // conductor is in no counter and needs no checkpoint (§3.7 — its window
    // re-open blocks at the GCL acquire, held by this foreign scope); C4
    // assist visitors take NO checkpoint (§9.1(6)). Checkpoints (CG-3a):
    // the helper-wait isReady lambda (a woken waiting helper moves
    // waiting-- -> paused++) and the per-batch drain safepoint
    // (donateAll(): a paused helper holds NO local work; active-- ->
    // paused++; granularity = one drained batch, the CG-I12 bound).
    // ShouldPause gates counter (re-)entry including a fresh helper's entry
    // increment of m_numberOfParallelMarkersInDrainFromShared (transient:
    // checkpoint (a) moves it to paused under this mutex), so the predicate
    // below — every visitor inside drainFromShared is paused — is stable
    // once reached.
    // didReachTermination requires m_pausedParallelMarkers == 0 (CG-I22),
    // so the conductor's waitForTermination stays parked across this stop.
    //
    // Termination (CG-I16): only m_markingMutex is acquired (no api-rank,
    // no heap rank >= 7 lock); helpers hold no lock a §A.3 window needs.
    // No lost wakeup: every flag/count write and wait shares
    // m_markingMutex. Flag-off (CGD2.1): unreachable — every GCL-free point
    // has m_currentPhase == NotRunning (ANNEX CGD1.1 flag-off half), so the
    // callers' phase gate never passes and the pair is never set.
    Locker locker { m_markingMutex };
    ASSERT(!m_parallelMarkersShouldPause); // Single foreign scope: it holds GCL.
    // WRITER CONTRACT (Heap.h): writes are WTF::atomicStore under the mutex
    // (the mutex carries the protocol; the atomic makes the lock-free
    // per-batch hint read well-defined).
    WTF::atomicStore(&m_parallelMarkersShouldPause, true, std::memory_order_relaxed);
    m_markingConditionVariable.notifyAll();
    while (m_numberOfParallelMarkersInDrainFromShared != m_pausedParallelMarkers) {
        // [r34] F-A item (1): per-quantum watchdog sample — the fail-stop
        // fires ON THIS CONDUCTOR (with the item-(3) VM attribution; under
        // U0b the server-side vm() IS the requester's VM) if a wedged
        // marker batch never reaches its checkpoint. CG-T8 wedged-marker
        // arm ([r34] item (4)) witnesses this leg.
        JSThreadsSafepoint::watchdogAssertStopProgress(requestStart, &vm());
        m_markingConditionVariable.waitFor(m_markingMutex, Seconds::fromMilliseconds(1));
    }
    ASSERT(!m_numberOfActiveParallelMarkers);
}

void Heap::resumeConcurrentMarkingAfterForeignStop()
{
    // §9.1(2) resume half (ANNEX CGP1): clear flag + notifyAll. Paused
    // helpers re-enter their counters (paused-- -> waiting++/active++) and
    // re-evaluate; the conductor's waitForTermination re-checks CG-I22.
    Locker locker { m_markingMutex };
    ASSERT(m_parallelMarkersShouldPause);
    WTF::atomicStore(&m_parallelMarkersShouldPause, false, std::memory_order_relaxed);
    m_markingConditionVariable.notifyAll();
}

// --- End shared heap server ---

// SPEC-congc §9.3(1) (CG-3c): mid-cycle ATTACH fence-init handshake. Defined
// here (not HeapClientSet.cpp) because it needs both heaps' privates via
// HeapClientSet's friendship; the add()-side call is the chartered-out
// HeapClientSet.cpp hunk (INTEGRATE-congc.md manifest row CG-3c-M1).
// Contract (asserted): GBL held, !WSAC, client NOT yet published in the
// registry. Happens-before: the server master pair + FEP mutate only
// in-window (WSAC under GBL); this section holds GBL with !WSAC, so the
// snapshot is untorn and never stale; a live-marking attachee starts RAISED
// (the master is raised for the whole marking span, §5.3(4)); CG-I3's
// close assert holds because the WND-close republish stamps every
// registered client and this client is published with the CURRENT FEP
// already stamped. §9.3(2) m_isMarking visibility and §9.3(4)
// didRun=false/CMS=empty need no code here: the client is zero-init and its
// first AHA performs the seq_cst GSP load.
void HeapClientSet::snapshotBarrierFenceStateForAttach(GCClient::Heap& client)
{
    JSC::Heap& server = client.server();
    ASSERT(server.m_gcBarrierLock.isHeld());
    ASSERT(!server.worldIsStoppedForAllClients());
    ASSERT(!client.isOnList()); // BEFORE the insert publishes the client.
    if (!server.sharedGCBarrierStateIsPerClient()) [[likely]]
        return; // !C1R no-op: the copies are unrouted, unread state (F33/CGD4.4; CG-I0 byte-for-byte).
    client.m_mutatorShouldBeFenced = server.m_mutatorShouldBeFenced;
    client.m_barrierThreshold = server.m_barrierThreshold;
    client.m_fenceEpochSeen = server.m_barrierFenceEpoch.load(std::memory_order_acquire); // FEP stamp (§5.3(2)).
}

// SPEC-congc §9.2(1) (CG-3c): EXIT1/teardown CMS final flush. Runs strictly
// after the client's PERMANENT access drop (asserted: the CMS is frozen —
// barriers require access, so no append can race or postdate this flush) and
// strictly before the epoch=MAX park and the HCS remove (call-site order in
// detachCurrentThread / ~GCClient::Heap). Target: the SERVER legacy
// m_mutatorMarkStack under m_serverMutatorMarkStackLock, NOT
// m_sharedMutatorMarkStack — F34 forbids the phase read that could
// discriminate live-marking from between-cycles here, and a between-cycles
// append to the shared accounting pre-loads runBeginPhase's
// didReachTermination() precondition (the CG-T8 Arm-1 RED root cause; the
// CG-3b open-kind narrowing, which this flush extends — AMEND record in
// INTEGRATE-congc.md is the normative content). Soundness of the server
// target in BOTH cases:
//  - between cycles: pre-cycle barrier cells take the landed single-VM
//    route — cleared at full-GC begin (mark-version reset makes them
//    redundant), retained and constraint-merged on Eden.
//  - mid-cycle: MarkStackMergingConstraint (volatile; covers the SERVER +
//    race stacks when C1R, F32) converts them to work at the next fixpoint
//    window's constraint pass, which precedes termination. The flush can
//    never land cells after the cycle's last constraint pass: cells exist in
//    the CMS only if no WND-open drained it since their append, and every
//    in-flight window's open (which postdates the access drop) drains every
//    registered CMS first — so a mid-final-window flush is structurally an
//    empty no-op, and runEndPhase's ASSERT(m_mutatorMarkStack->isEmpty())
//    cannot trip on flushed cells.
// Locking: m_markingMutex (LK.9d) -> m_serverMutatorMarkStackLock -> CMS
// lock (LK.9c) — the same forward chain as the WND-open drain's server-stack
// arm. m_markingMutex is held per §9.2(1) so the flush serializes against
// any concurrent WND-open drain of this same CMS. The drain is a transferTo:
// GCSegmentedArray::removeLast() does not refill across segment boundaries,
// so a bare isEmpty()/removeLast() loop underflows m_top once the head
// segment empties with further segments chained. F36: no dead-state
// publication — the flush leaves no marker behind; the CMS is simply empty
// when the GCH dies.
void HeapClientSet::flushClientMutatorMarkStackForExit(GCClient::Heap& client)
{
    JSC::Heap& server = client.server();
    if (!server.sharedGCBarrierStateIsPerClient()) [[likely]]
        return; // !C1R: the CMS was never created (F33/CGD4.4; CG-I0 byte-for-byte).
    ASSERT(!client.hasHeapAccess()); // §9.2(1): strictly after the permanent access drop.
    Locker markingLocker { server.m_markingMutex };
    Locker serverStackLocker { server.m_serverMutatorMarkStackLock };
    Locker cmsLocker { client.m_mutatorMarkStackLock }; // LK.9d>LK.9c (lint R4 marker): the §9.2(1) exit-flush edge.
    if (!client.m_mutatorMarkStack || client.m_mutatorMarkStack->isEmpty())
        return;
    client.m_mutatorMarkStack->transferTo(*server.m_mutatorMarkStack);
}

namespace GCClient {

#define INIT_CLIENT_ISO_SUBSPACE_FROM_SPACE_AND_SET(subspace) subspace(heap.subspace##AndSet.space)

#define INIT_CLIENT_ISO_SUBSPACE(name, heapCellType, type) \
    , name(heap.name)

Heap::Heap(JSC::Heap& heap)
    : m_server(heap)
    FOR_EACH_JSC_ISO_SUBSPACE(INIT_CLIENT_ISO_SUBSPACE)
    , INIT_CLIENT_ISO_SUBSPACE_FROM_SPACE_AND_SET(codeBlockSpace)
    , INIT_CLIENT_ISO_SUBSPACE_FROM_SPACE_AND_SET(functionExecutableSpace)
    , INIT_CLIENT_ISO_SUBSPACE_FROM_SPACE_AND_SET(programExecutableSpace)
    , INIT_CLIENT_ISO_SUBSPACE_FROM_SPACE_AND_SET(unlinkedFunctionExecutableSpace)
    , m_threadLocalCache(heap)
{
    // §5.3 (T4): the GCClient::IsoSubspace LocalAllocators enter the TLC's
    // per-directory map (lookup-only) so the §10A.1 ownership predicate and
    // §5.3 teardown cover iso. Before clientSet().add() — the TLC map itself
    // is owner-thread-private until the client is published. NOTE (review
    // round 2): the LocalAllocators constructed by this ctor's member-init
    // list are NOT private — each LocalAllocator ctor links into its shared
    // server BlockDirectory's m_localAllocators under m_localAllocatorsLock,
    // globally visible immediately. The GC-side traversals therefore hold
    // that lock too (BlockDirectory::stopAllocating et al.), which is what
    // makes pre-publication construction safe against a concurrent legacy
    // collection or conducted stop. Option off: skipped (I10; server
    // teardown handles iso exactly as today).
    if (Options::useSharedGCHeap()) [[unlikely]]
        registerIsoSubspaceLocalAllocators();
    // SPEC-heap.md §5.1 (T2): every client registers with its server's
    // HeapClientSet. An add() that makes size() > 1 with the option on runs
    // the §10B.4 sticky switch first (see HeapClientSet::add).
    m_server.clientSet().add(*this);
}

void Heap::registerIsoSubspaceLocalAllocators()
{
    ASSERT(Options::useSharedGCHeap());
#define THREADS_REGISTER_CLIENT_ISO_LA(name, heapCellType, type) \
    m_threadLocalCache.registerExternalAllocator(&name.localAllocator());
    FOR_EACH_JSC_ISO_SUBSPACE(THREADS_REGISTER_CLIENT_ISO_LA)
#undef THREADS_REGISTER_CLIENT_ISO_LA
    m_threadLocalCache.registerExternalAllocator(&codeBlockSpace.localAllocator());
    m_threadLocalCache.registerExternalAllocator(&functionExecutableSpace.localAllocator());
    m_threadLocalCache.registerExternalAllocator(&programExecutableSpace.localAllocator());
    m_threadLocalCache.registerExternalAllocator(&unlinkedFunctionExecutableSpace.localAllocator());
}

Heap::~Heap()
{
    // Teardown order (I2/I9/I13; review round 1): allocator relinquishment
    // MUST run while this client still holds heap access and is still
    // registered. lastChanceToFinalize() mutates shared directories under
    // MSPL, but MSPL by itself does not exclude a conducted stop — the
    // conductor's flush/sweep is licensed by WSAC, not MSPL, so the two
    // disjunctive licenses (assertSharedAllocatorMutationIsSafe) only
    // exclude each other through the access protocol: while we hold access,
    // the §10.4 barrier cannot complete, so no stop window can open around
    // our MSPL section; and acquireHeapAccess() (F8) parks us across any
    // stop already pending. This is what makes "MSPL holders always hold
    // access" (the step-5 note in conductSharedCollection and the
    // sweepSynchronously contract) true on the teardown path too. Option
    // off: the TLC is empty, lastChanceToFinalize() is a no-op, and the
    // access bracket is skipped (I10).
    bool sharedTeardown = Options::useSharedGCHeap();

    // SPEC-congc §9.2(4)/§3.7 (CG-3c): the detaching thread is never the
    // live conductor — EXIT1 (this dtor) on m_gcConductorThread mid-cycle is
    // FORBIDDEN (release assert; CG-T9's conductor-exit arm attempts it). A
    // conductor between its final close and the F20 deferred clear is legal
    // here exactly because its phase is back to NotRunning; the F20 stale
    // case (owner restamped by a successor) makes the owner the successor,
    // never us. Phase read under *m_threadLock (the landed §3.4 guard reader
    // shape — F22's enumerated set). Flag-off (!ISS or option off): skipped
    // — byte-for-byte (CG-I0; mid-cycle GCL-free exits do not exist
    // flag-off, ANNEX CGD1.1, so the assert is vacuous there anyway).
    if (sharedTeardown && m_server.isSharedServer()) {
        Locker locker { *m_server.m_threadLock };
        RELEASE_ASSERT(m_server.m_gcConductorThread != &Thread::currentSingleton()
            || m_server.m_currentPhase == CollectorPhase::NotRunning);
    }

    // A spawned thread relinquished already (tearDownSpawnedThreadForExit,
    // while its lite was Live): re-acquiring access here would run the MSPL
    // section on a Teardown lite the §A.3 quiescence predicate no longer
    // counts, racing a thread-granular stop's stopAllocating over the same
    // allocators.
    if (!m_relinquishedAllocators) {
        if (sharedTeardown && !hasHeapAccess())
            acquireHeapAccess(); // F8: blocks while a stop is pending; threads other than the attached one re-assert I2 via the step-0/owner checks.
        lastChanceToFinalize();
    }
    // SPEC-congc §9.2(1) EXIT1 order (CG-3c): teardown (the
    // lastChanceToFinalize MSPL section above, run while access is held) ->
    // PERMANENT access drop -> CMS final flush under m_markingMutex
    // (strictly after the last possible barrier — the drop freezes the CMS)
    // -> epoch=MAX -> HCS remove. F36: NO dead-state publication. Both
    // branches below implement that order: detachCurrentThread() performs
    // drop -> flush -> epoch=MAX for the attached thread; the non-attached
    // branch drops the bracket access then flushes (its epoch was parked at
    // MAX by that thread's own earlier detach, §9 lifecycle).
    if (currentThreadClient() == this)
        detachCurrentThread();
    else {
        if (sharedTeardown && hasHeapAccess())
            releaseHeapAccess();
        HeapClientSet::flushClientMutatorMarkStackForExit(*this); // No-op when !C1R (F33).
    }
    // Unregister last: a stop that begins the moment remove() unblocks can
    // no longer touch our state — every allocator of ours is stopped and
    // unlinked from the shared directories, and we hold no access (remove()
    // asserts that; it also defers across an in-flight stop window, I13).
    // §9.2(2): when windowed stages are live, remove() still blocks inside
    // windows (heap I13, the GBL/!WSAC bracket in HeapClientSet::remove);
    // removal BETWEEN windows is legal exactly because the flush above
    // already ran — never a registered client with an unreachable CMS
    // (CG-I17; the flush completes while still registered, F2 rejected
    // rev 1 order).
    m_server.clientSet().remove(*this);
}

void Heap::lastChanceToFinalize()
{
    // Implements the GlobalGC FIXME (Heap.h): relinquish memory from this
    // client's allocators back to the server (§5.3/I9). The TLC owns every
    // per-client non-iso LocalAllocator, and (option on) holds the
    // GCClient::IsoSubspace LocalAllocators as lookup-only entries (T4), so
    // one teardown pass — per-slot stopAllocatingForGood() under MSPL, then
    // unlink — covers both. Option off: the TLC holds nothing and this is a
    // no-op; server teardown via BlockDirectory::stopAllocatingForGood()
    // proceeds exactly as today (I10).
    m_threadLocalCache.stopAllocatingForGood();
    m_relinquishedAllocators = true;
}

void Heap::attachCurrentThread()
{
    // I4(a): the ctor registered us with the server's client set.
    ASSERT(isOnList());
    // §10A.1: stamp the current-client TLS slot.
    setCurrentThreadClient(this);
    // I4(b): this thread's stack must be visible to conservative scanning
    // (§10.6/I12) before any allocation. Stamps the uid cache so the
    // acquireHeapAccess() enforcement below short-circuits.
    ensureCurrentThreadIsRegisteredForConservativeScan(Thread::currentSingleton());
    // §11 (review round 2): deliberately NO m_localEpoch store here. The
    // ctor / a prior detach parked it at MAX, which is safe until the next
    // stop window: the sole consumer is bumpAndReclaim()'s min scan, and the
    // reclaim sequence's stamping loop (runSafepointHooksAndReclaim)
    // overwrites EVERY registered client's value inside the same stop window
    // just before that scan, so the pre-stop value is never load-bearing —
    // and MAX can only make the min scan MORE conservative, never lower. A
    // store of current() here, taken before access is held, could be delayed
    // (this thread preempted between the current() read and the store)
    // across two complete stop windows and then land its stale value between
    // a later stop's stamping loop and min scan, tripping
    // RELEASE_ASSERT(minLocalEpoch >= oldEpoch) (GCSafepointEpoch.cpp).
    // Resulting invariant: m_localEpoch is written ONLY by the conductor's
    // stamping loop (world stopped) and by detachCurrentThread (MAX). While
    // this client holds access (acquired below), no stop window — hence no
    // stamping or min scan — can run at all, so post-attach heap use is
    // covered without an attach-side stamp.
    // I4(c): acquire access (F8: blocks while a shared-mode stop is pending).
    acquireHeapAccess();
}

void Heap::detachCurrentThread()
{
    RELEASE_ASSERT(currentThreadClient() == this);
    if (hasHeapAccess())
        releaseHeapAccess();
    // T10 amplifier hook (AMPLIFIER.md): widen the detach window — access is
    // released but the local epoch is not yet parked at MAX; a reclaimer
    // computing min(localEpoch) right now must still count us (we are still
    // registered) and must not free items we could have been touching.
    RaceAmplifier::perturb();
    // SPEC-congc §9.2(1) (CG-3c): CMS final flush — strictly after the
    // access drop above (the CMS is frozen: barriers require access) and
    // strictly BEFORE the epoch=MAX park below, under m_markingMutex (see
    // the helper's banner for the target rationale and the lock chain).
    // No-op when !C1R (F33/CGD4.4; CG-I0 byte-for-byte) and harmless on a
    // harness detach/re-attach cycle (an early total donation).
    HeapClientSet::flushClientMutatorMarkStackForExit(*this);
    // §11: a detached client never holds up reclamation. §9.2(1): epoch=MAX
    // strictly AFTER the CMS flush, strictly BEFORE the HCS remove (the
    // dtor's clientSet().remove() call).
    m_localEpoch.store(std::numeric_limits<uint64_t>::max(), std::memory_order_seq_cst);
    setCurrentThreadClient(nullptr);
}

void Heap::markStandalone()
{
    // §12.1: this client is not embedded in a VM (SharedHeapTestHarness).
    // Arms the RELEASE_ASSERT in vm() (HeapInlines.h; T9).
    m_isStandalone = true;
}

void Heap::ensureCurrentThreadIsRegisteredForConservativeScan(WTF::Thread& currentThread)
{
    // I4(b) (§10.6/I12, T6). MachineThreads::addCurrentThread() is idempotent
    // but takes the thread-group WordLock; cache the last registered uid so
    // the hot JSLock hand-back path skips it. The cache is per-client and a
    // client's access is held by one thread at a time (I2) — hand-offs
    // synchronize through the JSLock — but the load/store are relaxed
    // atomics anyway: a stale read merely re-runs the idempotent
    // registration. uid 0 is never a valid Thread uid (main thread is 1,
    // others increment from there), so the zero-initialized cache always
    // misses first time. Registration is permanent for the thread's lifetime
    // (ThreadGroup drops a thread only when it dies), so a cache hit implies
    // the thread is still scannable.
    uint32_t uid = currentThread.uid();
    if (m_lastConservativeScanRegisteredUid.load(std::memory_order_relaxed) == uid)
        return;
    m_server.machineThreads().addCurrentThread();
    m_lastConservativeScanRegisteredUid.store(uid, std::memory_order_relaxed);
}

void Heap::acquireHeapAccess()
{
    auto& currentThread = Thread::currentSingleton();

    // F8 step 0: already HasAccess on this thread => return. Idempotent, no
    // CAS-spin: JSLock recursion, attachCurrentThread(), and the manifest-5a
    // didResume hook may re-enter.
    if (m_accessState.load(std::memory_order_seq_cst) == hasAccessState) {
        RELEASE_ASSERT(m_accessOwner.load(std::memory_order_relaxed) == &currentThread);
        return;
    }

    // I4(b) enforcement (§10A/§10.6, T6): "AHA = F8 + re-stamp m_accessOwner
    // + ensure addCurrentThread()". A thread may not enter the heap unless
    // its stack and registers are visible to the conductor's conservative
    // scan (I12). Ensure — not assert — because JSLock migration can route a
    // brand-new thread here through the server-side forwarding before
    // JSLock::didAcquireLock reaches its own addCurrentThread() call.
    // Placed before the CAS loop (and after the step-0 early return, whose
    // owner already registered when it first acquired): once the CAS below
    // succeeds this thread is a heap-accessing mutator and must already be
    // scannable.
    ensureCurrentThreadIsRegisteredForConservativeScan(currentThread);
    ASSERT(m_server.machineThreads().includesCurrentThread());

    // UNGIL §A.3.2b (U-T5): thread-granular §A.3 stops gate FRESH access
    // acquisition for clients of the gilOff VM. Resolved once: m_isStandalone
    // and the VM's gilOff bit are immutable, and the stop word itself is
    // re-polled seq_cst inside the loop (SB1.3). Standalone (harness) clients
    // have no VM and no §A.3 windows.
    //
    // UNGIL §B.2 (U-T6): the VM is resolved through the SERVER, never
    // through GCClient::Heap::vm() — that accessor is VM-embedding pointer
    // arithmetic and is GARBAGE for the per-thread heap-allocated clients
    // this function now serves (spawned threads + embedder carriers). A
    // non-standalone client's server is a VM's own heap (U0b: one VM per
    // shared server), so m_server.vm() is correct for every client shape.
    VM* serverVM = m_isStandalone ? nullptr : &m_server.vm();
    bool threadGranularGated = serverVM && serverVM->gilOff();
#if ASSERT_ENABLED
    // ANNEX EXIT1.4(a): a TEARDOWN lite's access re-acquisition is FORBIDDEN
    // — re-entry to JS would need it, and a TEARDOWN lite can never run JS
    // again. State byte read under the registry lock only (r31).
    if (threadGranularGated) {
        if (VMLite* lite = VMLite::currentIfExists()) {
            Locker registryLocker { VMLiteRegistry::singleton().lock };
            ASSERT(lite->state == VMLite::State::Live);
        }
    }
#endif

    for (;;) {
        // F8 step 1: seq_cst CAS NoAccess -> HasAccess. Only this client's
        // owning thread may attempt the transition (I2), so failure is a
        // protocol violation, not contention.
        uint8_t previous = m_accessState.compareExchangeStrong(noAccessState, hasAccessState, std::memory_order_seq_cst);
        RELEASE_ASSERT(previous == noAccessState);

        // T10 amplifier hook (AMPLIFIER.md): widen the F8 Dekker window —
        // we are HasAccess but have not yet sampled GSP; a conductor
        // publishing GSP right now must observe our state and we must revert.
        RaceAmplifier::perturb();

        // F8 step 2: seq_cst load of GSP. The seq_cst CAS/load pair is the
        // client half of the Dekker pair with the conductor's seq_cst
        // GSP-store / access-state sample (acq/rel is insufficient; see
        // SPEC-heap.md §7 F8).
        if (m_server.m_gcStopPending.load(std::memory_order_seq_cst)) [[unlikely]] {
            // F8 step 3: mandatory revert — never enter the heap while a
            // stop is pending.
#if ASSERT_ENABLED
            // SPEC-congc §8.2 CG-I18 (CG-3c): cell-lock-no-park — a
            // JSCellLock (10a) holder must never reach an AHA park leg (it
            // would hold the lock across a whole stop window, breaking the
            // CGN1 N3 in-window tryLock-termination argument). Stage-gated
            // so flag-off debug behavior is unchanged (CG-T5's CG-I18 storm
            // arm runs with the C1 flag on).
            ASSERT(!Options::useConcurrentSharedGCMarking() || !GCCellLockDepth::current());
#endif
            uint8_t reverted = m_accessState.exchange(noAccessState, std::memory_order_seq_cst);
            ASSERT_UNUSED(reverted, reverted == hasAccessState);
            {
                Locker locker { m_server.m_gcBarrierLock }; // GBL, rank 4; released while waiting (I6).
                m_server.m_gcBarrierCondition.notifyAll(); // Wake the conductor's §10.4 barrier.
                while (m_server.m_gcStopPending.load(std::memory_order_seq_cst))
                    m_server.m_gcBarrierCondition.wait(m_server.m_gcBarrierLock);
            }
            continue; // Retry from step 1.
        }

        // UNGIL §A.3.2b(i) / ANNEX SB1 item 3 (U-T5): the §A.3 stop-word
        // poll, positioned AFTER the F8 step-1 seq_cst CAS and BESIDE the
        // step-2 GSP load, as a seq_cst load (inside jsThreadsStopPendingFor;
        // the SB1.4 Dekker proof needs the CAS/poll pair in the single
        // seq_cst total order — acq/rel is insufficient, both interleavings
        // are SB litmus shapes). On set: F8 mandatory-revert
        // (seq_cst exchange -> NoAccess), wake the conductor's predicate
        // sampler, then park on this thread's own NVS ticket until resume.
        // This leg CARRIES soundness for every unenumerable AHA/RHA bracket
        // (heap §9) and is what makes the §A.3.2 access-released exemption
        // and §A.3.4 entry gating sound: fresh acquisition never admits a
        // mutator into an open window. The conductor itself is exempt
        // (HBT3.2: a class-4 conductor re-acquires inside its own window
        // before fanning; the default conductor re-acquires only after the
        // word is cleared, so the exemption is a no-op for it).
        if (threadGranularGated && jsThreadsStopPendingFor(*serverVM) && !jsThreadsCurrentThreadIsStopConductor()) [[unlikely]] {
            uint8_t reverted = m_accessState.exchange(noAccessState, std::memory_order_seq_cst);
            ASSERT_UNUSED(reverted, reverted == hasAccessState);
            // F8/§10.4 composition (mc-safe-gcwait-vs-classa-stop): every
            // HasAccess->NoAccess transition while GSP is pending must wake
            // the GC conductor's untimed §10.4 barrier wait, exactly like
            // RHA does — the conductor may have sampled this client as
            // HasAccess right after our step-2 GSP load missed its store.
            // (For THIS leg the GCL ordering shield makes the overlap
            // unreachable today — an open §A.3 window holds GCL, so no
            // shared GC can be mid-barrier — but the notify is one seq_cst
            // load on an already-unlikely path; keep the rule uniform.)
            if (m_server.m_gcStopPending.load(std::memory_order_seq_cst)) [[unlikely]] {
                Locker locker { m_server.m_gcBarrierLock };
                m_server.m_gcBarrierCondition.notifyAll();
            }
            jsThreadsNotifyMutatorQuiesced();
#if ASSERT_ENABLED
            ASSERT(!Options::useConcurrentSharedGCMarking() || !GCCellLockDepth::current()); // CG-I18 (CG-3c): no 10a hold across an §A.3 park.
#endif
            jsThreadsParkForStopWindow(*serverVM);
            continue; // Retry from step 1 (a GC stop may have arrived meanwhile; GSP re-polls).
        }

        // UNGIL §A.3.2b(i), MODE-MACHINE leg (review round): SPEC-ungil item
        // 2b(i) is "acquireHeapAccess()/attachCurrentThread() polls the
        // LITE'S STOP BIT" — under the §A.2.1 alias, the VM trap word's
        // NeedStopTheWorld bit — which EVERY stop request sets, including
        // Mode-machine (debugger) stops via requestStop/requestStopAll. The
        // §A.3-word leg above covers only thread-granular windows; without
        // this leg a gilOff mutator could re-acquire and run JS while a
        // debugger STW service is in flight (the §A.3.8 service-gating
        // conjunct samples access states on the assumption re-acquisition is
        // gated). The elected representative and the free-running RunOne
        // target are exempt inside the helper; GC keep-parked stops are
        // carried by the GSP leg above. Mandatory F8 revert BEFORE the NVS
        // park (r9 F3), exactly like the §A.3 leg.
        if (threadGranularGated && jsThreadsModeStopGatesCurrentThread(*serverVM)) [[unlikely]] {
            uint8_t reverted = m_accessState.exchange(noAccessState, std::memory_order_seq_cst);
            ASSERT_UNUSED(reverted, reverted == hasAccessState);
            // F8/§10.4 composition (mc-safe-gcwait-vs-classa-stop, REAL
            // deadlock): the Mode-machine trap bit is fanned WITHOUT GCL
            // (VMManager::requestStopAll(StopReason::GC) fans it in §10 step
            // 3, after GSP), so this leg CAN fire between the conductor's
            // GSP store and its §10.4 barrier sample: our step-2 GSP load
            // read false, the conductor then sampled us HasAccess and
            // entered the untimed m_gcBarrierCondition.wait, and we revert
            // here for the mode bit. Without this notify the conductor
            // sleeps forever holding GCL, a queued Class-A requester blocks
            // in the JSThreadsStopScope tryLock-poll, and the 30s stop
            // watchdog fail-stops the process (heap §10A RHA rule: every
            // HasAccess->NoAccess transition signals the barrier iff GSP).
            if (m_server.m_gcStopPending.load(std::memory_order_seq_cst)) [[unlikely]] {
                Locker locker { m_server.m_gcBarrierLock };
                m_server.m_gcBarrierCondition.notifyAll();
            }
            jsThreadsNotifyMutatorQuiesced();
#if ASSERT_ENABLED
            ASSERT(!Options::useConcurrentSharedGCMarking() || !GCCellLockDepth::current()); // CG-I18 (CG-3c): no 10a hold across a Mode-machine park.
#endif
            jsThreadsParkForModeStop(*serverVM);
            continue; // Retry from step 1 (GSP and the §A.3 word re-poll).
        }

        // Re-stamp the owner (§10A; I2: JSLock migration transfers via the
        // server-side forwarding, which re-stamps before/instead of AHA).
        m_accessOwner.store(&currentThread, std::memory_order_relaxed);

        // SPEC-congc §4.1 (CG-2): AHA success-tail didRun note, C1R-only
        // (F33). Scheduling-only (feeds the m_mutatorExecutionVersion
        // constraint-staleness heuristic): the conductor ORs this into the
        // legacy m_mutatorDidRun consumer and clears it at each WND-open.
        // Owner-thread relaxed store on our own client byte (heap I17); the
        // window barrier orders it (CG-I9). Flag-off: never set — the
        // landed shared-mode didRun behavior stands byte-for-byte (CGD4.4).
        if (m_server.sharedGCBarrierStateIsPerClient()) [[unlikely]]
            WTF::atomicStore(&m_didRunSinceLastWindow, true, std::memory_order_relaxed);

        // UNGIL ANNEX ISB1.2 (U-T5): AHA is a "may execute JIT code"
        // transition that need not pass through an NVS exit (incl. the
        // bit-already-clear path, §F token acquisition and ACT, the DAL2
        // dtor and the §F.5 LIFO restore — all funnel through here).
        // Compare the per-thread stop-generation copy; mismatch => ISB
        // before any JIT entry. GIL-on/flag-off cost: zero (gated off).
        if (threadGranularGated) [[unlikely]]
            jsThreadsSyncToStopGenerationBeforeJITEntry();
        return;
    }
}

void Heap::releaseHeapAccess()
{
    // Tolerate a null owner defensively (historically the §10B.4 migration
    // could leave the owner unstamped; since review round 1 it always stamps
    // — the flip thread holds the API lock whenever the legacy bit is set).
    ASSERT(m_accessOwner.load(std::memory_order_relaxed) == &Thread::currentSingleton()
        || !m_accessOwner.load(std::memory_order_relaxed));
    m_accessOwner.store(nullptr, std::memory_order_relaxed);

    // §10A RHA: seq_cst exchange -> NoAccess publishes all prior heap writes
    // to the conductor (F6).
    uint8_t previous = m_accessState.exchange(noAccessState, std::memory_order_seq_cst);
    RELEASE_ASSERT(previous == hasAccessState);

    // T10 amplifier hook (AMPLIFIER.md): widen the RHA window between the
    // access publication and the barrier signal — a conductor entering its
    // §10.4 wait right here must not miss the wakeup.
    RaceAmplifier::perturb();

    // Signal the §10.4 barrier only if a stop is pending (F8).
    if (m_server.m_gcStopPending.load(std::memory_order_seq_cst)) [[unlikely]] {
        Locker locker { m_server.m_gcBarrierLock };
        m_server.m_gcBarrierCondition.notifyAll();
    }

    // UNGIL §A.3/SB1 (U-T5): a gilOff client's RHA is a conductor-predicate
    // edge — the §A.3.2 predicate samples access states per EXIT1.2 walk, and
    // a thread going access-released into native code is exactly what lets a
    // window close without it. The seq_cst exchange above is the SB1 store;
    // wake the conductor's sampler if a window is open (cheap: one seq_cst
    // load when gilOff, nothing GIL-on/flag-off/standalone).
    if (!m_isStandalone) [[likely]] {
        // UNGIL §B.2 (U-T6): server-routed VM resolution — see the
        // acquireHeapAccess banner (GCClient::Heap::vm() is unusable for
        // per-thread heap-allocated clients). m_server.vm() is plain pointer
        // arithmetic; gilOff() is an immutable byte — flag-off this is the
        // same one-branch cost as the landed form.
        VM& serverVM = m_server.vm();
        if (serverVM.gilOff() && jsThreadsStopPendingFor(serverVM)) [[unlikely]]
            jsThreadsNotifyMutatorQuiesced();
    }
}

#undef INIT_CLIENT_ISO_SUBSPACE
#undef CLIENT_ISO_SUBSPACE_INIT_FROM_SPACE_AND_SET


#define DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW_IMPL(name, heapCellType, type) \
    IsoSubspace* Heap::name##Slow() \
    { \
        ASSERT(!m_##name); \
        Locker locker { server().m_lock }; \
        JSC::IsoSubspace& serverSpace = *server().name<SubspaceAccess::OnMainThread>(); \
        auto space = makeUnique<IsoSubspace>(serverSpace); \
        if (Options::useSharedGCHeap()) [[unlikely]] \
            m_threadLocalCache.registerExternalAllocator(&space->localAllocator()); \
        IsoSubspace* result = space.release(); \
        WTF::atomicStore(std::bit_cast<IsoSubspace**>(&m_##name), result, std::memory_order_release); \
        return result; \
    }

#define DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW(name) \
    DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW_IMPL(name, unused, unused2) \

FOR_EACH_JSC_DYNAMIC_ISO_SUBSPACE(DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW_IMPL)

DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW(evalExecutableSpace)
DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW(moduleProgramExecutableSpace)

#undef DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW_IMPL
#undef DEFINE_DYNAMIC_ISO_SUBSPACE_MEMBER_SLOW

} // namespace GCClient

// ===== UNGIL §A.3/§A.3.8 (U-T5): thread-granular park access pairing =====
//
// A gilOff thread parking at an NVS ticket for ANY stop reason releases its
// own client's heap access and re-acquires it on resume. This is what makes
// the §A.3.2 conductor predicate purely access-based for parked threads
// ("parked" implies "access-released"), and it is the per-thread form of the
// heap §13.5a/g rule under §A.3.8: each entered thread releases ITS OWN
// client, so the shared-GC §10.4 barrier and a §A.3 conductor's predicate
// both complete with N threads in one VM. Pairing is a thread_local (the
// per-client pairing in the spec letter: gilOff clients are per-thread —
// U-T6 — so per-thread == per-client; the GC-specific m_releasedByGCPark
// member keeps its own, independent pairing for the §13.5 hooks above).
// Callers: VMManager::notifyVMStop's gilOff ticket/side parks (U-T5).
//
// Re-entrancy note: gcClientDidResumeFromThreadGranularStop's AHA can itself
// park (a NEW window opened) — that park happens INSIDE acquireHeapAccess's
// §A.3.2b gate with access already reverted, and does not call back into
// these helpers, so the pairing flag cannot be torn by recursion.

static thread_local bool t_releasedByThreadGranularPark { false };

// ===== T1-gc-siblings-mark: gilOff Mode-machine sibling -> parallel marker =====
//
// Converts the W-1 sibling park at VMManager::notifyVMStop (the bounded 1ms
// m_worldConditionVariable.waitFor while the elected representative runs
// conductSharedCollection) into a parallel-marking assist: the sibling joins
// the existing m_markingMutex / m_markingConditionVariable HelperDrain
// protocol on a per-sibling SlotVisitor, indistinguishable from a
// heapHelperPool helper to the F14/F17 counter machine and the §9.1(2)
// pause checkpoints. The representative-side fixpoint already waits on this
// pool (runTaskInParallel, drainInParallel), so siblings directly relieve
// the helper-starved 4890 wait. Gate is m_siblingMarkingAssistEnabled under
// m_markingMutex (raised by runBeginPhase AFTER didStartMarking +
// shouldExit=false; lowered by runEndPhase WITH shouldExit=true) — a false
// return means no marking phase is open (debugger Mode-stop, between
// cycles, or before runBeginPhase), the admission cap is reached, or no pool
// visitor is free, and the caller falls back to its bounded poll. Pre:
// caller is heap-access-released and holds no VMManager / api-rank lock; the
// only locks taken here are m_markingMutex (the marker protocol's own rank)
// and, nested inside it, the leaf m_parallelSlotVisitorLock. Flag-off:
// unreachable (sole call site is inside the [[unlikely]] vm.gilOff()
// branch); the runBeginPhase / runEndPhase wiring is gilOff-gated and the
// member fields are never set.
bool Heap::gilOffSiblingAssistMarking() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    SlotVisitor* visitor = nullptr;
    {
        Locker locker { m_markingMutex };
        if (!m_siblingMarkingAssistEnabled)
            return false;
        // T4-sibling-assist-admission-cap (SCALEBENCH sec 31 / offcpu16 row #3):
        // 16 siblings + ~7 heapHelperPool threads >> the parallelizable marking
        // — over-admitted siblings immediately park at drainFromShared's isReady
        // wait (m_markingMutex / m_markingConditionVariable churn) for the WHOLE
        // marking phase, and additionally become on-CPU-then-parked suspend
        // victims for tryCopyOtherThreadStacks (offcpu16 row #4: 17/52 victims
        // were inside drainFromShared). Cap admitted siblings at the slack
        // between numberOfGCMarkers (the configured drain parallelism) and the
        // pool helpers already serving it; over-cap siblings return false and
        // the caller (gcSiblingAssistMarkingIfEnabled) falls back to its
        // bounded poll — i.e. they stay cheaply parked at the
        // jsThreadsParkForStopWindow stripe condvar instead of entering the
        // marker wait. Re-evaluated under m_markingMutex on every bounded-poll
        // tick, so a sibling that was capped re-tries each tick and can be
        // admitted later in the same marking phase if an earlier assist
        // finishes (--m_numberOfSiblingMarkingAssists below). runEndPhase's
        // drain-wait is unaffected: capped siblings never increment the count.
        // W=1 IMPACT ZERO: this function is reached only via the [[unlikely]]
        // vm.gilOff() sibling-park branch when isSharedServer() — W=1 has no
        // siblings to park. Flag-off byte-identical (call site unreachable).
        // The default cap of 0 disables the assist: the heapHelperPool
        // drainers plus the conductor saturate marking (extra drainers
        // measured inside noise), and a parked sibling keeps its published
        // cooperative root snapshot whereas an admitted one becomes a
        // suspend victim for the root scan. A nonzero cap is an experiment
        // knob.
        unsigned cap = Options::sharedGCMaxSiblingMarkingAssists();
        if (m_numberOfSiblingMarkingAssists >= cap)
            return false;
        // The pool was sized by runBeginPhase for the clients registered at
        // the start of this cycle; a client that attached between windows
        // finds it empty and declines, because the visitor set must not
        // change while a MarkingConstraintSolver round may be in flight.
        {
            Locker poolLocker { m_parallelSlotVisitorLock };
            if (m_availableSiblingSlotVisitors.isEmpty())
                return false;
            visitor = m_availableSiblingSlotVisitors.takeLast();
        }
        // Count this sibling IN before dropping the mutex: runEndPhase's
        // close (under this mutex) lowers enabled + raises shouldExit, then
        // waits this count to zero — so an entered sibling is always
        // accounted for, and a not-yet-entered sibling sees enabled==false
        // and never enters. The shouldExit==false observation here is the
        // happens-before that publishes runBeginPhase's didStartMarking
        // (m_markingVersion etc.) to the drain below.
        m_numberOfSiblingMarkingAssists++;
    }

    {
        // Exactly the heapHelperPool body: ParallelModeEnabler +
        // drainFromShared(HelperDrain). Returns Done when runEndPhase sets
        // m_parallelMarkersShouldExit (one whole marking phase per call).
        // Siblings are JS mutator threads — do NOT registerGCThread here
        // (sticky Helper would trip every post-resume
        // ASSERT(!mayBeGCThread()) on the JS path); nothing in the
        // HelperDrain marking path requires it. The §9.1(2) per-batch
        // pause checkpoint and the m_rightToRun safepoint in drain() make
        // this visitor a full participant of any foreign-stop /
        // resumeThePeriphery handshake.
        ParallelModeEnabler parallelModeEnabler(*visitor);
        visitor->drainFromShared(SlotVisitor::HelperDrain);
    }

    {
        Locker locker { m_parallelSlotVisitorLock };
        m_availableSiblingSlotVisitors.append(visitor);
    }

    {
        Locker locker { m_markingMutex };
        RELEASE_ASSERT(m_numberOfSiblingMarkingAssists);
        if (!--m_numberOfSiblingMarkingAssists)
            m_markingConditionVariable.notifyAll(); // Wakes runEndPhase's sibling-drain wait.
    }
    return true;
}

// VMManager.cpp seam (same forward-declared free-function shape as the
// gcClientWillPark / DidResume pair below): resolves the calling thread's
// server heap and forwards. Returns false (caller falls back to its bounded
// poll) when the thread has no client / no shared server / no open marking
// phase. Called ONLY from inside the [[unlikely]] vm.gilOff() sibling-park
// branch — flag-off unreachable.
bool gcSiblingAssistMarkingIfEnabled()
{
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    if (!client)
        return false;
    JSC::Heap& server = client->server();
    if (!server.isSharedServer())
        return false;
    return server.gilOffSiblingAssistMarking();
}

void gcClientWillParkForThreadGranularStop()
{
    if (t_releasedByThreadGranularPark)
        return; // Idempotent across re-fires within one park episode.
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    if (!client || !client->hasHeapAccess())
        return; // Nothing to release (not attached, or already released).
    client->releaseHeapAccess(); // seq_cst RHA; signals §10.4 + §A.3 samplers.
    t_releasedByThreadGranularPark = true;
}

// T5-rootscan-skip-coop-parked-suspend: per-park-episode publish/clear of
// the cooperative root snapshot (see GCClient::Heap::publishParkedRootSnapshot
// in Heap.h for the full protocol). Kept SEPARATE from the willPark/didResume
// access-pairing above because the snapshot's validity window is STRICTLY
// the pure-park span (libc futex/condvar machinery, no JSCell* below the
// captured stackTop), which is a SUBSET of the access-released span: a
// sibling that released access via willPark but then enters the
// gcSiblingAssistMarkingIfEnabled() HelperDrain assist is RUNNING marking
// code with live JSCell* on its stack — its snapshot MUST be cleared (or
// never published) for that span so the conductor's root scan falls back to
// suspend() for it. Call sites therefore bracket each individual
// jsThreadsParkForStopWindow / m_worldConditionVariable.waitFor /
// F8-blocked acquireHeapAccess with publish-then-clear, with the
// DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE spill captured in the SAME stack
// frame so the pointed-to RegisterState and stackTop stay live across the
// park. didResume below clears too (idempotent) so an early-exit path can
// never leave a stale snapshot live across the seq_cst access re-acquire.
// Flag-off / W=1: every call site is inside an [[unlikely]] vm.gilOff()
// branch that additionally requires the caller to NOT be the §A.3 conductor
// or the Mode-stop representative (or to have a pending GSP set by another
// conductor) — unreachable with a single thread; the helpers themselves are
// no-ops without an attached client.
void GCClient::Heap::publishParkedRootSnapshot(WTF::Thread& thread, CurrentThreadState* snapshot)
{
    // Every publisher builds the snapshot with DECLARE_AND_COMPUTE_CURRENT_
    // THREAD_STATE in the frame that stays live across the park, so
    // [stackTop, stackOrigin] is a sub-range of the thread's real stack.
    // Under ASAN with detect_stack_use_after_return that frame can be a
    // heap-backed fake frame outside thread.stack(); the consumer
    // (MachineThreads::tryCopyCooperativelyParkedThreadStack) would then
    // compute a span that runs off the mapped stack. Validate at this single
    // publish chokepoint and decline such a snapshot by publishing null: the
    // conductor's root scan falls back to the SIGUSR2 suspend path, which
    // captures the real SP from the signal context. m_parkedRootSnapshotThread
    // is still recorded so it is always written before the seq_cst snapshot
    // store that gates it. On a non-fake-stack build the check is a tautology.
    if (snapshot
        && (snapshot->stackOrigin != thread.stack().origin()
            || snapshot->stackTop < thread.stack().end()
            || snapshot->stackTop > thread.stack().origin())) [[unlikely]] {
#if ASSERT_ENABLED && !ASAN_ENABLED
        dataLogLn("[SharedGC T5-rootscan] declining coop root snapshot publish: stackTop ", RawPointer(snapshot->stackTop), " / stackOrigin ", RawPointer(snapshot->stackOrigin), " outside thread.stack() [", RawPointer(thread.stack().end()), ", ", RawPointer(thread.stack().origin()), "] — falling back to suspend()");
#endif
        WTF::atomicStore(&m_parkedRootSnapshotThread, &thread, std::memory_order_relaxed);
        m_parkedRootSnapshot.store(nullptr, std::memory_order_seq_cst);
        return;
    }
    WTF::atomicStore(&m_parkedRootSnapshotThread, &thread, std::memory_order_relaxed);
    m_parkedRootSnapshot.store(snapshot, std::memory_order_seq_cst);
}

void gcClientPublishParkedRootSnapshot(CurrentThreadState* snapshot)
{
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    if (!client)
        return;
    client->publishParkedRootSnapshot(Thread::currentSingleton(), snapshot);
}

void gcClientClearParkedRootSnapshot()
{
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    if (!client)
        return;
    client->clearParkedRootSnapshot();
}

void gcClientDidResumeFromThreadGranularStop()
{
    if (!t_releasedByThreadGranularPark)
        return; // Idempotent: nothing was released by the matching willPark.
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    RELEASE_ASSERT(client); // The releasing thread cannot lose its client while parked (teardown re-acquires first, EXIT1.3).
    // T5-rootscan-skip: the snapshot's validity ends BEFORE re-acquisition
    // (the F8/§A.3.2b gates inside acquireHeapAccess can themselves run
    // non-park code paths). seq_cst clear; idempotent vs the call-site
    // gcClientClearParkedRootSnapshot() that already ran in the common case.
    client->clearParkedRootSnapshot();
    client->acquireHeapAccess(); // F8 + the §A.3.2b stop-word gate: blocks across any pending stop.
    t_releasedByThreadGranularPark = false;
}

// Shared-GC-stop leg of the class-(2) park-site poll (consumed by
// JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld; same U-T5 seam
// discipline as the two hooks above). The §10A SINFAC shape, lifted to the
// FIX-2 wait sites: iff GSP is pending (F8) and THIS thread's client holds
// heap access, release -> F8-blocked re-acquire. releaseHeapAccess signals
// the conductor's §10.4 barrier (RHA rule); acquireHeapAccess blocks until
// the conductor clears GSP, and its §A.3.2b/Mode gates cover any
// thread-granular stop that arrives while we are parked here. Returns true
// iff it released (a fresh acquisition episode for the caller). Callers
// without a current client (compiler/worklist threads) are exact no-ops:
// the §10.4 barrier never waits on a thread that holds no client access.
// Benign TOCTOU: if GSP clears between the poll and the release, this is a
// harmless release/re-acquire pair on a quantum-bounded wait loop.
bool gcClientReleaseAccessAndBlockForPendingSharedGCStop()
{
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    if (!client)
        return false;
    JSC::Heap& server = client->server();
    if (!server.isSharedServer())
        return false;
    if (!server.gcStopPendingForAllClients()) [[likely]] // GSP, F8 — the hot poll (mirrors Heap::stopIfNecessary).
        return false;
    if (!client->hasHeapAccess())
        return false;
    client->releaseHeapAccess(); // Signals the §10.4 barrier (GSP is set).
    // T5-rootscan-skip: the F8 wait below is a pure-park span (condvar under
    // the GBL); publish a coop root snapshot captured in THIS frame so the
    // conductor's root scan can skip the SIGUSR2 suspend for this thread
    // (offcpu16 row #4: 3/52 victims were inside acquireHeapAccess's F8
    // wait). Spilled AFTER the GSP slow-path checks above so the hot poll
    // pays nothing; the frame stays live across acquireHeapAccess() so the
    // RegisterState and stackTop remain valid. Cleared seq_cst before
    // returning (the caller resumes JS immediately).
    DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE(parkedRootSnapshot);
    client->publishParkedRootSnapshot(Thread::currentSingleton(), &parkedRootSnapshot);
    client->acquireHeapAccess(); // F8: blocks until the conductor clears GSP.
    client->clearParkedRootSnapshot();
    return true;
}

} // namespace JSC
