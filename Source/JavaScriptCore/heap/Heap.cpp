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

#include "BuiltinExecutables.h"
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
#include <wtf/AvailableMemory.h>
#include <wtf/BitVector.h> // UNGIL §D.1 (U-T12): the in-stop dead-TID membership set.
#include <atomic>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/ListDump.h>
#include <wtf/MemoryFootprint.h>
#include <wtf/RAMSize.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/SimpleStats.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include "InternalFieldTuple.h"

#if USE(BUN_JSC_ADDITIONS)
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

// ===== UNGIL §A.3 (U-T5) cross-TU seams =====
// Defined in runtime/VMManager.cpp (window state + ticket park) and
// runtime/VMLite.cpp (ANNEX ISB1 stop-generation sync). Their headers
// (VMManager.h / VMLite.h) are OUTSIDE U-T5's writable file set; lifting
// these declarations into those headers is an orchestrator-tracked cleanup.
// Signatures must stay byte-identical to the definitions.
bool jsThreadsStopPendingFor(VM&); // seq_cst stop-word load (SB1; sole accessor pair lives in VMManager.cpp — U20).
bool jsThreadsCurrentThreadIsStopConductor(); // §A.3.3 tenure check (HBT3.2 self-exemption).
bool jsThreadsThreadGranularWorldIsStopped(); // §A.3.2 post-quiescence depth (AB-10 class-4 conductor disjuncts).
void jsThreadsParkForStopWindow(VM&); // NVS ticket park; pre: caller holds NO heap access.
void jsThreadsNotifyMutatorQuiesced(); // wakes the conductor's §A.3.2 predicate wait.
void jsThreadsSyncToStopGenerationBeforeJITEntry(); // ANNEX ISB1.2 (VMLite.cpp).
void jsThreadsBumpStopGeneration(); // ANNEX ISB1.1 (VMLite.cpp); bumped by EVERY conductor — §A.3 AND the §10 shared-GC conductor below.
bool jsThreadsModeStopGatesCurrentThread(VM&); // SPEC-ungil §A.3.2b(i): Mode-machine stop bit gates fresh access (VMManager.cpp).
void jsThreadsParkForModeStop(VM&); // §A.3.2b(i) NVS park until the Mode machine resumes; pre: caller holds NO heap access.

// NEVER_INLINE to prevent LTO from inlining this function, which can break
// compiler barriers in MarkedBlock::isMarked on x86_64.
NEVER_INLINE bool Heap::isMarked(const void* rawCell)
{
    ASSERT(!m_isMarkingForGCVerifier);
    HeapCell* cell = std::bit_cast<HeapCell*>(rawCell);
    if (cell->isPreciseAllocation())
        return cell->preciseAllocation().isMarked();
    MarkedBlock& block = cell->markedBlock();
    return block.isMarked(m_objectSpace.markingVersion(), cell);
}

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
    , m_handleSet(vm)
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
    , temporalTimeZoneHeapCellType(IsoHeapCellType::Args<TemporalTimeZone>())
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

    // GIL-off shared-GC-heap: Heap::addToRememberedSet appends to
    // m_mutatorMarkStack from the write-barrier slow path of EVERY attached
    // mutator thread, so its append()s must serialize (see MarkStack.h —
    // postIncTop is a non-atomic RMW of both the cached top and the head
    // segment's top, and append can expand the segment list; a lost
    // increment is a lost remembered-set entry => live old-gen object's
    // young reference never re-scanned => use-after-free). The other shared
    // stacks are already serialized elsewhere: m_raceMarkStack by
    // m_raceMarkStackLock (SlotVisitor.cpp appendToMarkStack race arm),
    // m_sharedCollectorMarkStack / m_sharedMutatorMarkStack by
    // m_markingMutex; per-SlotVisitor stacks are single-producer.
    if (Options::useSharedGCHeap())
        m_mutatorMarkStack->setMultiProducerAccess();

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
    
    if (m_collectContinuouslyThread) {
        {
            Locker locker { m_collectContinuouslyLock };
            m_shouldStopCollectingContinuously = true;
            m_collectContinuouslyCondition.notifyOne();
        }
        m_collectContinuouslyThread->waitForCompletion();
    }

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
    {
        // SharedGC (T8/§5.3 teardown): a stale sticky-ISS flag can outlive
        // the last secondary client until the §10D revert poll, so server
        // teardown's directory-bit flips and final sweeps run under MSPL
        // (no-op when !isSharedServer(), I10). Dropped around
        // releaseDelayedReleasedObjects(), which may re-enter JS.
        MutatorSlowPathLocker mutatorSlowPathLocker(*this);
        m_objectSpace.stopAllocatingForGood();
        m_objectSpace.lastChanceToFinalize();
    }
    releaseDelayedReleasedObjects();

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
    // FIXME: Change this to use SaturatedArithmetic when available.
    // https://bugs.webkit.org/show_bug.cgi?id=170411
    CheckedSize checkedNewSize = m_deprecatedExtraMemorySize;
    checkedNewSize += size;
    size_t newSize = std::numeric_limits<size_t>::max();
    if (!checkedNewSize.hasOverflowed()) [[likely]]
        newSize = checkedNewSize.value();
    m_deprecatedExtraMemorySize = newSize;
    reportExtraMemoryAllocatedSlowCase(nullptr, nullptr, size);
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

void Heap::reportAbandonedObjectGraph()
{
    // Our clients don't know exactly how much memory they
    // are abandoning so we just guess for them.
    size_t abandonedBytes = static_cast<size_t>(0.1 * capacity());

    // We want to accelerate the next collection. Because memory has just 
    // been abandoned, the next collection has the potential to 
    // be more profitable. Since allocation is the trigger for collection, 
    // we hasten the next collection by pretending that we've allocated more memory. 
    // SharedGC (§5.4): activity callbacks never fire collections when shared
    // — triggering is mutator-driven (CIND/CSAC; I15).
    if (m_fullActivityCallback && !isSharedServer()) {
        m_fullActivityCallback->didAllocate(*this,
            m_sizeAfterLastCollect - m_sizeAfterLastFullCollect + totalBytesAllocatedThisCycle() + m_bytesAbandonedSinceLastFullCollect.load(std::memory_order_relaxed));
    }
    m_bytesAbandonedSinceLastFullCollect.fetch_add(abandonedBytes, std::memory_order_relaxed); // F3.
}

void Heap::protect(JSValue k)
{
    ASSERT(k);
    // SharedGC (T9): main-VM-only assert (protect/unprotect below) — the
    // protect set is server state but the API-lock predicate names the main
    // VM; GIL-phase sound (JSLock migration). Post-GIL this becomes an
    // access-held predicate (currentThreadClient()), not per-client
    // iteration — the set itself stays one-per-server.
    ASSERT(vm().currentThreadIsHoldingAPILock());

    if (!k.isCell())
        return;

    m_protectedValues.add(k.asCell());
}

bool Heap::unprotect(JSValue k)
{
    ASSERT(k);
    // SharedGC (T9): main-VM-only assert — see protect().
    ASSERT(vm().currentThreadIsHoldingAPILock());

    if (!k.isCell())
        return false;

    return m_protectedValues.remove(k.asCell());
}

void Heap::addReference(JSCell* cell, ArrayBuffer* buffer)
{
    if (m_arrayBuffers.addReference(cell, buffer)) {
        collectIfNecessaryOrDefer();
        didAllocate(buffer->gcSizeEstimateInBytes());
    }
}

template<typename CellType, typename CellSet>
void Heap::finalizeMarkedUnconditionalFinalizers(CellSet& cellSet, CollectionScope collectionScope)
{
    // SharedGC (T9): conductor-context OK — end-phase work, world stopped
    // (worldIsStopped() / WSAC once shared); vm() is the main mutator VM
    // (deviation 3), the only VM whose cells live in this server phase 1.
    // No JS runs in unconditional finalizers (§10B.5: no JS finalizers in
    // the stop window).
    cellSet.forEachMarkedCell(
        [&] (HeapCell* cell, HeapCell::Kind) {
            static_cast<CellType*>(cell)->finalizeUnconditionally(vm(), collectionScope);
        });
}

void Heap::finalizeUnconditionalFinalizers()
{
    CollectionScope collectionScope = this->collectionScope().value_or(CollectionScope::Full);

    {
        // We run this before CodeBlock's unconditional finalizer since CodeBlock looks at the owner executable's installed CodeBlock in its finalizeUnconditionally.

        // FunctionExecutable requires all live instances to run finalizers. Thus, we do not use finalizer set.
        finalizeMarkedUnconditionalFinalizers<FunctionExecutable>(functionExecutableSpaceAndSet.space, collectionScope);

        finalizeMarkedUnconditionalFinalizers<ProgramExecutable>(programExecutableSpaceAndSet.finalizerSet, collectionScope);
        if (m_evalExecutableSpace)
            finalizeMarkedUnconditionalFinalizers<EvalExecutable>(m_evalExecutableSpace->finalizerSet, collectionScope);
        if (m_moduleProgramExecutableSpace)
            finalizeMarkedUnconditionalFinalizers<ModuleProgramExecutable>(m_moduleProgramExecutableSpace->finalizerSet, collectionScope);
    }

    finalizeMarkedUnconditionalFinalizers<SymbolTable>(symbolTableSpace, collectionScope);

    forEachCodeBlockSpace(
        [&] (auto& space) {
            this->finalizeMarkedUnconditionalFinalizers<CodeBlock>(space.set, collectionScope);
        });
    if (collectionScope == CollectionScope::Full) {
        finalizeMarkedUnconditionalFinalizers<Structure>(structureSpace, collectionScope);
        finalizeMarkedUnconditionalFinalizers<BrandedStructure>(brandedStructureSpace, collectionScope);
#if ENABLE(WEBASSEMBLY)
        finalizeMarkedUnconditionalFinalizers<WebAssemblyGCStructure>(webAssemblyGCStructureSpace, collectionScope);
#endif
    }
    finalizeMarkedUnconditionalFinalizers<StructureRareData>(structureRareDataSpace, collectionScope);
    finalizeMarkedUnconditionalFinalizers<UnlinkedFunctionExecutable>(unlinkedFunctionExecutableSpaceAndSet.set, collectionScope);
    if (m_weakSetSpace)
        finalizeMarkedUnconditionalFinalizers<JSWeakSet>(*m_weakSetSpace, collectionScope);
    if (m_weakMapSpace)
        finalizeMarkedUnconditionalFinalizers<JSWeakMap>(*m_weakMapSpace, collectionScope);
    if (m_weakObjectRefSpace)
        finalizeMarkedUnconditionalFinalizers<JSWeakObjectRef>(*m_weakObjectRefSpace, collectionScope);
    if (m_errorInstanceSpace)
        finalizeMarkedUnconditionalFinalizers<ErrorInstance>(*m_errorInstanceSpace, collectionScope);

    // FinalizationRegistries currently rely on serial finalization because they can post tasks to the deferredWorkTimer, which normally expects tasks to only be posted by the API lock holder.
    if (m_finalizationRegistrySpace)
        finalizeMarkedUnconditionalFinalizers<JSFinalizationRegistry>(*m_finalizationRegistrySpace, collectionScope);

#if ENABLE(WEBASSEMBLY)
    if (m_webAssemblyInstanceSpace)
        finalizeMarkedUnconditionalFinalizers<JSWebAssemblyInstance>(*m_webAssemblyInstanceSpace, collectionScope);
#endif
}

void Heap::willStartIterating()
{
    m_objectSpace.willStartIterating();
}

void Heap::didFinishIterating()
{
    m_objectSpace.didFinishIterating();
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

// FIXME(fix-shared-heap-corruption): TEMPORARY diagnostic instrumentation —
// strip before declaring the gates green (the snapshot copy below must not
// be in place when the >1% bench gate is measured). Round 1 refuted both
// original hypotheses: the registered-thread count matched the client count
// (no I4(b) registration shortfall), and neither LocalAllocator freelist
// probe ever fired (no sweep handed out a version-current marked or
// newlyAllocated cell). The surviving mechanism is under-MARKING: a
// conservatively-reachable cell the conducted cycle's marking never visited
// ends the cycle with no version-current liveness bit, so a later sweep
// frees it "legitimately" — invisible to the round-1 probes by construction.
// This snapshot records the §10.6 conservative stack roots at gather time;
// Heap::endMarking() re-checks that every snapshot cell carries a
// version-current liveness bit before m_objectSpace.endMarking() retires the
// newlyAllocated version — trapping at the guilty CYCLE on a mark failure,
// while a sweep/steal failure stays silent here and still trips the harness
// pattern asserts later, splitting the two hypotheses in one verify run.
// Conductor-private: written and read only while the world is stopped for
// all clients (I5), so an unsynchronized file-static is safe.
static Vector<HeapCell*>& sharedGCStackRootSnapshot()
{
    static Vector<HeapCell*>* snapshot = new Vector<HeapCell*>;
    return *snapshot;
}

// FIXME(fix-shared-heap-ring-liveness-5): TEMPORARY diagnostic seam — strip
// together with the rest of the fix-shared-heap-corruption instrumentation
// before declaring the gates green. SharedHeapTestHarness.cpp installs an
// audit function here (extern declaration on its side; deliberately no
// header change so the seam stays diagnostic-only and invisible to the rest
// of the tree). Review round 5 rejected landing an I12 m_currentBlock root
// walk before the PRODUCER-side question is answered: the round-3 safepoint
// hook proves WHICH ring cell lost liveness but not WHY the §10.6
// suspend-and-copy scan never produced it. This seam hands the audit every
// shared-mode gather's full conservative root set so it can discriminate,
// per missing ring cell, between (a) a TinyBloomFilter / MarkedBlockSet
// publication miss (filter side), (b) a MachineThreads stack-coverage miss
// (scan side — owning thread unregistered or bad stack bounds), and (c) a
// cell already killed by an EARLIER stop's sweep. The audit only LOGS; the
// gathered root set is never modified, so no invariant is masked and the
// round-3 trap still fires in the same run, now with provenance above it.
// Threading: written once by the harness at scenario entry; read by whichever
// thread executes the conservative-scan constraint of a shared conduction —
// which can be a parallel marker helper created at VM startup, BEFORE the
// store, so this must be an atomic (release/acquire), not a plain pointer
// published by Thread::create alone (TSAN gate).
std::atomic<void (*)(Heap&, HeapCell**, size_t)> g_sharedGCConservativeRootAuditHook { nullptr };

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
            ASSERT(!client.hasHeapAccess());
        });
    }
#endif
    m_machineThreads->gatherConservativeRoots(roots, *m_jitStubRoutines, *m_codeBlocks, m_currentThreadState, m_currentThread);
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

    // FIXME(fix-shared-heap-corruption): TEMPORARY diagnostic — see
    // sharedGCStackRootSnapshot() above; checked in Heap::endMarking(). If
    // this constraint executes more than once per cycle, the LAST gather
    // wins, which is sound: mutators stay parked for the whole conducted
    // cycle (deviation 4), so only the conductor's own roots can differ
    // between gathers, and each gather's roots are appended to the visitor
    // (and thereby marked) after the snapshot is taken.
    if (isSharedServer()) [[unlikely]] {
        auto& snapshot = sharedGCStackRootSnapshot();
        snapshot.shrink(0);
        for (size_t i = 0; i < roots.size(); ++i)
            snapshot.append(roots.roots()[i]);

        // FIXME(fix-shared-heap-ring-liveness-5): TEMPORARY diagnostic — see
        // g_sharedGCConservativeRootAuditHook above. If this constraint
        // executes more than once per cycle, an audit line from a non-final
        // gather can be benign noise (a later gather may still produce the
        // cell); correlate using the LAST gather's lines immediately
        // preceding a round-3 trap in the same run.
        if (auto* audit = g_sharedGCConservativeRootAuditHook.load(std::memory_order_acquire)) [[unlikely]]
            audit(*this, roots.roots(), roots.size());
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

    // FIXME(fix-shared-heap-corruption): TEMPORARY diagnostic instrumentation
    // — strip before declaring the gates green (the snapshot walk must not be
    // in place when the >1% bench gate is measured). See the rationale at
    // sharedGCStackRootSnapshot(). Invariant checked: marking is complete and
    // every conservative stack root was appended to the visitor, which either
    // set its mark bit (testAndSetMarked, adopting the block's version) or
    // skipped it because it is version-current newlyAllocated — so each
    // snapshot cell must now carry one of the two version-current liveness
    // bits (run BEFORE m_objectSpace.endMarking() retires the newlyAllocated
    // version). A trap here = the conducted cycle's marking lost a
    // conservatively-reachable cell (mark failure, with the guilty
    // block/directory in hand); silence here + a later harness pattern assert
    // = the corruption happens in the sweep/steal handout instead.
    if (isSharedServer()) [[unlikely]] {
        auto& snapshot = sharedGCStackRootSnapshot();
        for (HeapCell* cell : snapshot) {
            if (cell->isPreciseAllocation()) {
                if (!cell->preciseAllocation().isLive()) [[unlikely]] {
                    dataLogLn(
                        "SharedGC diagnostic (fix-shared-heap-corruption): conservative stack root ",
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
                    "SharedGC diagnostic (fix-shared-heap-corruption): conservative stack root ",
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
    // FIXME: Change this to use SaturatedArithmetic when available.
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
    HeapIterationScope iterationScope(*this);
    size_t result = 0;
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
    HeapIterationScope iterationScope(*this);
    m_objectSpace.forEachLiveCell(
        iterationScope,
        [&] (HeapCell* cell, HeapCell::Kind kind) -> IterationStatus {
            if (isJSCellKind(kind))
                recordType(result, static_cast<JSCell*>(cell));
            return IterationStatus::Continue;
        });
    return result;
}

void Heap::deleteAllCodeBlocks(DeleteAllCodeEffort effort)
{
    if (m_collectionScope && effort == DeleteAllCodeIfNotCollecting)
        return;

    // SharedGC (T9): main-VM-only — embedder API on the main VM's mutator
    // (!vm.entryScope asserted below names the one main VM); not run by the
    // conductor. Post-GIL it must also exclude OTHER threads being entered —
    // that exclusion is the JSThreadsStopScope / deviation-8 charter (jit
    // R1), not clientSet() iteration.
    VM& vm = this->vm();
    PreventCollectionScope preventCollectionScope(*this);
    
    // If JavaScript is running, it's not safe to delete all JavaScript code, since
    // we'll end up returning to deleted code.
    RELEASE_ASSERT(!vm.entryScope);
    RELEASE_ASSERT(!m_collectionScope);

    completeAllJITPlans();

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

    // SharedGC (T9): main-VM-only — see deleteAllCodeBlocks().
    VM& vm = this->vm();
    PreventCollectionScope preventCollectionScope(*this);

    RELEASE_ASSERT(!m_collectionScope);

    HeapIterationScope heapIterationScope(*this);
    unlinkedFunctionExecutableSpaceAndSet.set.forEachLiveCell(
        [&] (HeapCell* cell, HeapCell::Kind) {
            UnlinkedFunctionExecutable* executable = static_cast<UnlinkedFunctionExecutable*>(cell);
            executable->clearCode(vm);
        });
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
    if (m_mutatorShouldBeFenced) {
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
    m_mutatorMarkStack->append(cell);
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
    // world-stopped sweep (finalize()'s shouldSweepSynchronously() path,
    // critical-memory/mini-mode, or teardown) — the same world-stopped-only
    // rule precise allocations already enforce (I5/I16 at
    // MarkedSpace::sweepPreciseAllocations: mutator-concurrent sweeping is a
    // deviation-4 disabled feature), extended to the block shrink leg. Empty
    // blocks stay on the directories' empty lists and remain reusable by
    // every client's allocator — only the OS-return is deferred, not reuse,
    // so this cannot starve allocation.
    if (!isSharedServer() || worldIsStoppedForAllClients())
        m_objectSpace.shrink();
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
        // until the next world-stopped sweep (lazy in-lock sweeping is the
        // designed shared-mode steady state; the IncrementalSweeper is disabled
        // under isSharedServer()). So legitimately-unswept blocks remain after
        // our own sweep, with no concurrent collection needed. Only assert when
        // this heap serves a single client.
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

    // SharedGC (I15, T5): once shared, every async trigger re-routes to the
    // §10B.1 ticketing — no fire-and-forget collections (§10.1). The ticket
    // is served by the next conductor: a sync requester's §10.2 election or
    // a mutator's stopIfNecessaryForAllClients() poll.
    if (isSharedServer()) [[unlikely]] {
        requestCollectionShared(request);
        return;
    }

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
        runSharedGCElection(requestCollectionShared(request));
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
        m_mutatorMarkStack->clear();
    } else
        m_bytesAllocatedBeforeLastEdenCollect = totalBytesAllocatedThisCycle();

    RELEASE_ASSERT(m_raceMarkStack->isEmpty());

    beginMarking();

#if ENABLE(WEBASSEMBLY)
    prepareWasmCalleeCleanup();
#endif

    forEachSlotVisitor(
        [&] (SlotVisitor& visitor) {
            visitor.didStartMarking();
        });

    m_parallelMarkersShouldExit = false;

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
        
    {
        ParallelModeEnabler enabler(visitor);
        visitor.drainInParallel(m_scheduler->timeToResume());
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

    // SharedGC (deviation 4, T5): no concurrent-marking window once shared —
    // the conducted collection is fully synchronous, so never resume the
    // world into CollectorPhase::Concurrent; keep draining at the fixpoint
    // until termination (the world stays suspended Begin..End).
    if (isSharedServer()) [[unlikely]]
        return true;

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
    // SharedGC (deviation 4, T5): unreachable once shared — runFixpointPhase
    // never schedules the Concurrent phase when isSharedServer(), and the ISS
    // flip only happens at collection quiescence (§10B.4/§10D).
    ASSERT(!isSharedServer());

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
        m_parallelMarkersShouldExit = true;
        m_markingConditionVariable.notifyAll();
    }
    m_helperClient.finish();
    
    ASSERT(m_mutatorMarkStack->isEmpty());
    ASSERT(m_raceMarkStack->isEmpty());

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
        finalizeUnconditionalFinalizers(); // We rely on these unconditional finalizers running before clearCurrentlyExecuting since CodeBlock's finalizer relies on querying currently executing.
        removeDeadCompilerWorklistEntries();
    }

    // Keep in mind that we may use AtomStringTable, and this is totally OK since the main thread is suspended.
    // End phase itself can run on main thread or concurrent collector thread. But whenever running this,
    // mutator is suspended so there is no race condition.
    deleteUnmarkedCompiledCode();

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
    // once per collection in BOTH protocols. Legacy (!isSharedServer(),
    // including option-off — the sole option-off behavior delta, I10
    // exemption): fire the hooks + the §11 legacy epoch-reclamation sequence
    // HERE, just before didFinishCollection(), with the mutator suspended.
    // Shared mode runs the equivalent at §10 step 7
    // (conductSharedCollection), after the conducted cycle completes — never
    // here (the conductor may drain several tickets per stop).
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

    setNeedFinalize();

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
    
    if (suspendedBefore != suspendedAfter) {
        if (suspendedBefore) {
            RELEASE_ASSERT(!suspendedAfter);
            
            resumeThePeriphery();
            if (conn == GCConductor::Collector)
                resumeTheMutator();
            else
                handleNeedFinalize();
        } else {
            RELEASE_ASSERT(!suspendedBefore);
            RELEASE_ASSERT(suspendedAfter);
            
            if (conn == GCConductor::Collector) {
                waitWhileNeedFinalize();
                if (!stopTheMutator()) {
                    dataLogLnIf(HeapInternal::verbose, "Returning false.");
                    return false;
                }
            } else {
                // SharedGC (T9): conductor-context OK — self-guarded no-op
                // unless the caller is the main VM's API-lock holder (see
                // runCurrentPhase()).
                sanitizeStackForVM(vm());
                handleNeedFinalize();
            }
            stopThePeriphery(conn);
        }
    }
    
    m_currentPhase = m_nextPhase;
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
    // Release store pairs with worldIsStopped() readers (matches the
    // m_worldIsStoppedForAllClients pattern). NOTE (V7 adjudication): the
    // header-side accessor Heap.h:414 still does a plain read — converting it
    // requires a Heap.h edit, deferred to the header-side pass.
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

    // FIXME(fix-shared-heap-corruption): TEMPORARY diagnostic instrumentation
    // — strip before declaring the gates green (an O(blocks) walk per stop;
    // must not be in place when the >1% bench gate is measured). §10 step-5
    // invariant, directly checkable: the stopAllocating() above must flush
    // EVERY client's LocalAllocators via the shared directories'
    // m_localAllocators lists. An allocator that is somehow not on its
    // directory's list (or a stop/resume imbalance) leaves its current block
    // FREELISTED through marking — and a freelisted block's
    // handed-out-but-unrecorded cells have no version-current liveness bits,
    // which is exactly the under-marking shape the round-1 probes could not
    // see. After a complete flush no block in the space may be freelisted
    // (stopAllocating() converts each in-use block to the stopped state).
    if (isSharedServer()) [[unlikely]] {
        m_objectSpace.forEachBlock(
            [&] (MarkedBlock::Handle* block) {
                if (block->isFreeListed()) [[unlikely]] {
                    dataLogLn(
                        "SharedGC diagnostic (fix-shared-heap-corruption): block ",
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
    
    handleNeedFinalize();
    m_mutatorDidRun = true;
}

bool Heap::stopIfNecessarySlow(unsigned oldState)
{
    // SharedGC (T9): main-VM-only — see stopIfNecessarySlow() above.
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    // RELEASE_ASSERT(oldState & hasAccessBit);
    // RELEASE_ASSERT(!(oldState & stoppedBit));
    
    // It's possible for us to wake up with finalization already requested but the world not yet
    // resumed. If that happens, we can't run finalization yet.
    if (handleNeedFinalize(oldState))
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
            callWithCurrentThreadState(scopedLambda<void(CurrentThreadState&)>(WTF::move(lambda)));
            return;
        }
    }
}

template<typename Func>
void Heap::waitForCollector(const Func& func)
{
    for (;;) {
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

void Heap::acquireAccessSlow()
{
    for (;;) {
        // SharedGC (§10B.4 flip handshake; review rounds 1+3): the ISS check
        // lives INSIDE the retry loop. A legacy acquirer lands here for two
        // distinct reasons: (1) its inline CAS (which expects exactly 0)
        // read the §10B.4 poison — then this check forwards; or (2) its
        // inline CAS read some PRE-flip non-zero state (e.g. needFinalizeBit
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
            handleNeedFinalize();
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
        
        if (handleNeedFinalize(oldState))
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

NEVER_INLINE bool Heap::handleNeedFinalize(unsigned oldState)
{
    // RELEASE_ASSERT(oldState & hasAccessBit);
    // RELEASE_ASSERT(!(oldState & stoppedBit));
    // SharedGC (§10B.5, T5b): the commented preconditions above would gain
    // "|| worldIsStoppedForAllClients()" — once shared, the legacy
    // hasAccessBit/stoppedBit are main-client-only and the §10.2 conductor
    // finalizes here (End -> NotRunning, conn == Mutator) while the world is
    // stopped for all clients. needFinalizeBit semantics are unchanged in
    // both protocols; no JS finalizers run inside the stop window (§10B.5).

    if (!(oldState & needFinalizeBit))
        return false;
    if (m_worldState.compareExchangeWeak(oldState, oldState & ~needFinalizeBit)) {
        finalize();
        // Wake up anyone waiting for us to finalize. Note that they may have woken up already, in
        // which case they would be waiting for us to release heap access.
        ParkingLot::unparkAll(&m_worldState);
        return true;
    }
    return true;
}

void Heap::handleNeedFinalize()
{
    while (handleNeedFinalize(m_worldState.load())) { }
}

void Heap::setNeedFinalize()
{
    m_worldState.exchangeOr(needFinalizeBit);
    ParkingLot::unparkAll(&m_worldState);
    m_stopIfNecessaryTimer->scheduleSoon();
}

void Heap::waitWhileNeedFinalize()
{
    for (;;) {
        unsigned oldState = m_worldState.load();
        if (!(oldState & needFinalizeBit)) {
            // This means that either there was no finalize request or the main thread will finalize
            // with heap access, so a subsequent call to stopTheWorld() will return only when
            // finalize finishes.
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

void Heap::finalize()
{
    MonotonicTime before;
    if (Options::logGC()) [[unlikely]] {
        before = MonotonicTime::now();
        dataLog("[GC<", RawPointer(this), ">: finalize ");
    }
    
    {
        SweepingScope sweepingScope(*this);
        deleteSourceProviderCaches();
        sweepInFinalize();
    }
    
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
    vm().stringSplitCache.clear();
    vm().jsonAtomStringCache.clearJSStrings();

    {
        // World-stopped here, but take the leaf lock anyway so the lock
        // discipline (and TSAN) sees one consistent guard for the vector.
        Locker locker { m_possiblyAccessedStringsFromConcurrentThreadsLock };
        m_possiblyAccessedStringsFromConcurrentThreads.clear();
    }

    immutableButterflyToStringCache.clear();
    
    // SharedGC (T9): conductor-context OK — embedder finalizer callbacks
    // receive the main VM (the only VM on this server, deviation 3); they
    // run world-stopped and must not allocate or re-enter JS.
    for (const HeapFinalizerCallback& callback : m_heapFinalizerCallbacks)
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

void Heap::sweepInFinalize()
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

    // SharedGC (T8/I5b, deviation 4): mutator-concurrent sweeping is disabled
    // once the server is shared — don't arm the sweeper timer. Unswept blocks
    // are swept in-lock by allocation slow paths (§5.2 MSPL carve-out,
    // §3.7), by shouldSweepSynchronously() paths on the conductor, or
    // re-snapshotted by the next conducted cycle. Logically-empty weak blocks
    // drain via WeakSet::sweep and sweepAllLogicallyEmptyWeakBlocks.
    // doWork/doWorkUntil are also gated, so a pre-sticky armed timer stands
    // down on its next fire. Option off / pre-sticky: today's behavior (I10).
    if (isSharedServer()) [[unlikely]]
        return;

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
        double minEdenToOldGenerationRatio = 1.0 / 3.0;
        if (edenToOldGenerationRatio < minEdenToOldGenerationRatio)
            m_shouldDoFullCollection = true;
        m_maxHeapSize = std::max(m_maxHeapSize, currentHeapSize + m_maxEdenSize);
        dataLogLnIf(verbose, "Eden: maxHeapSize = ", m_maxHeapSize);
        dataLogLnIf(verbose, "Eden: maxEdenSize = ", m_maxEdenSize);
        // SharedGC (§5.4): no activity-callback-driven collections when shared (I15).
        if (m_fullActivityCallback && !isSharedServer()) {
            ASSERT(currentHeapSize >= m_sizeAfterLastFullCollect);
            m_fullActivityCallback->didAllocate(*this, currentHeapSize - m_sizeAfterLastFullCollect);
        }
    }

    m_sizeAfterLastCollect = currentHeapSize;
    dataLogLnIf(verbose, "sizeAfterLastCollect = ", m_sizeAfterLastCollect);
    // I7: we are at a safepoint (world stopped); the relaxed counters are
    // exact here.
    m_nonOversizedBytesAllocatedThisCycle.store(0, std::memory_order_relaxed);
    m_oversizedBytesAllocatedThisCycle.store(0, std::memory_order_relaxed);
    m_lastOversidedAllocationThisCycle.store(0, std::memory_order_relaxed);

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
    if (m_fullActivityCallback)
        m_fullActivityCallback->setEnabled(enable);
    if (m_edenActivityCallback)
        m_edenActivityCallback->setEnabled(enable);
}

constexpr size_t oversizedAllocationThreshold = 64 * KB;
void Heap::didAllocate(size_t bytes)
{
    // SharedGC (§5.4): eden-activity dispatch is skipped when shared —
    // activity callbacks never fire collections; collection triggering is
    // mutator-driven via collectIfNecessaryOrDefer/collectSyncAllClients
    // (I15, T5). Counters are relaxed atomics (F3); exact at safepoints (I7).
    if (m_edenActivityCallback && !isSharedServer())
        m_edenActivityCallback->didAllocate(*this, totalBytesAllocatedThisCycle() + m_bytesAbandonedSinceLastFullCollect.load(std::memory_order_relaxed));
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
    } else if (bytes >= oversizedAllocationThreshold) {
        m_oversizedBytesAllocatedThisCycle.fetch_add(bytes, std::memory_order_relaxed);
        m_lastOversidedAllocationThisCycle.store(bytes, std::memory_order_relaxed);
    } else
        m_nonOversizedBytesAllocatedThisCycle.fetch_add(bytes, std::memory_order_relaxed);
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
    finalizer(slot->asCell());
    WeakSet::deallocate(WeakImpl::asWeakImpl(slot));
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
    // IncrementalSweeper (gated off once shared), and
    // sweepAllLogicallyEmptyWeakBlocks (takes MSPL).
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
        // FIXME: Change this to use SaturatedArithmetic when available.
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
    m_weakGCHashTables.add(weakGCHashTable);
}

void Heap::unregisterWeakGCHashTable(WeakGCHashTable* weakGCHashTable)
{
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
                // TODO: Verify this part only runs on one thread.
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
        ConstraintVolatility::GreyedByExecution);

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
            // (appendJSCellOrAuxiliary: mark + trace), inside the marking
            // fixpoint, so the retained set is closed under tracing: weak
            // sets and output constraints re-converge over the retained
            // cells, and every mark-keyed registry (e.g. the transition
            // WeakGCHashTables pruned after endMarking) only ever observes
            // CONSISTENT retained objects. Mark-without-trace was proven
            // unsound there: a retained zombie dictionary Structure with a
            // swept PropertyTable was re-adopted via the transition table
            // (objectmodel/i03-quarantine-readd-across-gc.js). Kind-agnostic
            // on purpose: the corruption-carrying cells are Auxiliary /
            // JSCellWithIndexingHeader (a JSCell-only walk was shown
            // insufficient, §7 Result 4 vs §10 Experiment A).
            //
            // NOTE (deliberate over-retention, do NOT "optimize"): on the
            // allocBit leg this appends every unmarked cell in the block —
            // an allocated block's free list was fully consumed
            // (didConsumeFreeList), so every cell slot was handed out this
            // mutator window and holds a constructed object (the same
            // property stock conservative scanning relies on when it
            // accepts any cell-aligned pointer into an allocated block);
            // there is no narrower per-cell judge, and narrowing the NA leg
            // was proven to reintroduce the under-marking hole. Cost
            // (round-6 adjudication — the earlier "exactly one extra cycle"
            // claim was WRONG): (a) in an EDEN cycle a retained dead window
            // cell's mark is STICKY — it stays marked through every
            // subsequent eden cycle and is reclaimable only at the next FULL
            // collection, so each eden window's dead cohort (plus its traced
            // closure) floats as old-generation garbage until then; (b) in a
            // FULL collection areMarksStale() is true for every block, the
            // isMarkedRaw skip (inside sharedGCWindowWitnessSnapshot) is disabled, and the allocBit leg
            // therefore retains EVERY cell of every consumed block —
            // including prior-cycle survivors that are now dead, which carry
            // no window witness conceptually. Tracing those survivors is
            // safe (their referents were marked last cycle and this cycle's
            // sweep has not run), but a dead set can ride for up to two full
            // collections, not one cycle, and full-GC reclamation of
            // window-consumed blocks degrades accordingly. Quantify on the
            // bench gate AFTER the temporary diagnostics are stripped. A
            // narrowing that skips stale-marked survivors in full
            // collections was sketched (EVIDENCE.md §13) but needs its own
            // adversarial round — do NOT land it blind.
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
            // useSharedGCHeap. Recorded in EVIDENCE.md §14.
            if (!isSharedServer()) [[likely]]
                return;
            if constexpr (std::is_same_v<std::decay_t<decltype(visitor)>, SlotVisitor>) {
                if (lastVersion == m_phaseVersion)
                    return;
                lastVersion = m_phaseVersion;
                // Round-7 F3: this pass dereferences cells (structureID +
                // visitChildren) for which NO stopped thread published a
                // pointer — its entire memory-safety argument (L1/L2) is
                // worldIsStoppedForAllClients(). A debug-only check is too
                // weak for the Release configuration all pinned verifies
                // run, and checkConn's Mutator arm deliberately tolerates a
                // legacy mutator-conn collection without WSAC; enforce at
                // the point of use. One load per phase version — free.
                RELEASE_ASSERT(worldIsStoppedForAllClients());
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ConservativeScan);
                HeapVersion markingVersion = m_objectSpace.markingVersion();
                Vector<HeapCell*> candidates;
                m_objectSpace.forEachBlock(
                    [&] (MarkedBlock::Handle* handle) {
                        // Round-7 F1: the witness judgment (NA version,
                        // allocBit, marks staleness, per-cell bits) is taken
                        // as ONE consistent snapshot under the block's
                        // header lock — the same lock aboutToMarkSlow holds
                        // while it clears/folds the bitmaps and bumps both
                        // versions concurrently on parallel marker helpers.
                        // The old hoisted lock-free reads violated the
                        // documented isMarkedRaw()/Dependency protocol. See
                        // the protocol + stale-bit-zero lemma comment at
                        // MarkedBlock::sharedGCWindowWitnessSnapshot().
                        // Appends happen HERE, after the header lock is
                        // dropped (appendJSCellOrAuxiliary can retake it via
                        // aboutToMark).
                        candidates.shrink(0);
                        handle->block().sharedGCWindowWitnessSnapshot(markingVersion, candidates);
                        if (!candidates.isEmpty()) {
                            for (HeapCell* cell : candidates)
                                visitor.appendJSCellOrAuxiliary(cell);
                            // Under the bitvector lock so the empty judge in
                            // BlockDirectory::endMarking() and any subsequent
                            // sweep classification observe it; an IsEmpty
                            // whole-payload rehand-out of a window-live block
                            // is thereby impossible. (appendJSCellOrAuxiliary
                            // only sets it via aboutToMarkSlow when the
                            // block's marks were stale — eden cycles need
                            // this explicit form.)
                            BlockDirectory* directory = handle->directory();
                            Locker locker { directory->bitvectorLock() };
                            directory->setIsMarkingNotEmpty(handle, true);
                        }
                    });
                // Precise-allocation leg (closes EVIDENCE.md §11 residual 1):
                // MarkedSpace::endMarking() also retires the per-allocation
                // newlyAllocated witness, so a window-allocated PRECISE cell
                // (> largeCutoff Map storage / butterfly) with the same
                // no-root profile would be freed by the sweep. Same witness,
                // same retention, same trace closure.
                for (PreciseAllocation* allocation : m_objectSpace.preciseAllocations()) {
                    if (allocation->isNewlyAllocated() && !allocation->isMarked())
                        visitor.appendJSCellOrAuxiliary(allocation->cell());
                }
            } else {
                // GC-verifier mirror (AbstractSlotVisitor): skipped, like the
                // conservative scan — the verifier has no window witnesses to
                // re-derive (the real fixpoint already marked the cohort).
                UNUSED_PARAM(visitor);
            }
        })),
        ConstraintVolatility::GreyedByExecution);

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
            }
            
            {
                SetRootMarkReasonScope rootScope(visitor, RootMarkReason::ProtectedValues);
                for (auto& pair : m_protectedValues)
                    visitor.appendUnbarriered(pair.key);
            }
            
            if (!m_markListSet.isEmpty()) {
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
            m_handleSet.visitStrongHandles(visitor);
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
                            m_requests.append(std::nullopt);
                            m_lastGrantedTicket++;
                            m_threadCondition->notifyOne(locker);
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
    waitForCollector(
        [&] (const AbstractLocker&) -> bool {
            ASSERT(m_lastServedTicket <= m_lastGrantedTicket);
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
    if (isSharedServer()) [[unlikely]]
        value = true;
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

void Heap::addHeapFinalizerCallback(const HeapFinalizerCallback& callback)
{
    m_heapFinalizerCallbacks.append(callback);
}

void Heap::removeHeapFinalizerCallback(const HeapFinalizerCallback& callback)
{
    m_heapFinalizerCallbacks.removeFirst(callback);
}

void Heap::setBonusVisitorTask(RefPtr<SharedTask<void(SlotVisitor&)>> task)
{
    Locker locker { m_markingMutex };
    m_bonusVisitorTask = task;
    m_markingConditionVariable.notifyAll();
}


void Heap::runTaskInParallel(RefPtr<SharedTask<void(SlotVisitor&)>> task)
{
    unsigned initialRefCount = task->refCount();
    setBonusVisitorTask(task);
    task->run(*m_collectorSlotVisitor);
    setBonusVisitorTask(nullptr);
    // The constraint solver expects return of this function to imply termination of the task in all
    // threads. This ensures that property.
    {
        Locker locker { m_markingMutex };
        while (task->refCount() > initialRefCount)
            m_markingConditionVariable.wait(m_markingMutex);
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

    Locker locker { *m_threadLock };
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

        if (m_gcConductorLock.tryLock()) { // GCL, rank 2; tryLock — no 5 -> 2 inversion.
            {
                Locker locker { *m_threadLock };
                if (m_lastServedTicket >= ticket) {
                    m_gcConductorLock.unlock();
                    return;
                }
                m_gcConductorActive = true; // GCA.
            }
            conductSharedCollection(*client); // §10 steps 3-9.
            m_gcConductorLock.unlock();
            {
                Locker locker { *m_threadLock };
                m_gcConductorActive = false;
                m_gcElectionCondition.notifyAll();
            }
            continue; // Re-check our ticket (late-granted tickets re-loop and win tryLock).
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
    if (!m_gcConductorLock.tryLock())
        return false; // Conductor active or JSThreadsStopScope held (§10C(e)); retry next poll.
    bool shouldConduct = false;
    {
        Locker locker { *m_threadLock };
        if (m_lastServedTicket < m_lastGrantedTicket) {
            m_gcConductorActive = true;
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
        m_gcConductorActive = false;
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
// OPEN INTEGRATION OBLIGATION (BLOCKING for the U-T12 two-VM TM-churn
// amplifier arm; recorded HERE because bytecode/JSThreadsSafepoint.cpp is
// outside U-T12's owned file set): the D1R fire loop below routes through
// WatchpointSet::fireAllSlow -> fireAllUnderClassAStop, whose run-inline
// branch (taken: WSAC satisfies worldIsStopped(vm)) constructs
// AlreadyStoppedWorldWitnessScope. With no process-global witness raised
// (s_stubWorldStoppedDepth == 0 — the WSAC evidence is per-heap), that
// constructor runs the R2-4/R3-11 entered-VM tripwire
// (assertAlreadyStoppedEvidenceCoversEveryMutator), which counts every
// entered VM whose clientHeap.server() is not THIS stopped server and
// RELEASE_ASSERTs the count is 0. ANNEX D1 explicitly sanctions other VMs'
// threads running un-stopped during the rebias stop (TM is process-global),
// so in a MULTI-VM gilOffProcess process a legitimately entered loser VM
// makes the first rebias fire a deterministic process abort. The patch
// itself is SAFE in that shape — loser-VM mutators can never execute the
// winner VM's compiled code, so firing/jettisoning winner code under the
// winner-heap-only stop is sound; it is the tripwire's premise (phase-1
// single-entered-VM) that the GIL-removal milestone retires. REQUIRED FIX
// (in JSThreadsSafepoint.cpp, when it becomes writable): when the stopped
// shared server is the gilOffProcess U0c winner's heap, entered VMs
// belonging to OTHER heaps are legitimate concurrent losers and must not be
// counted. Until that lands: single-VM gilOffProcess runs (incl. the
// spawn-storm and D1R.5 arms) are unaffected (count is 0), and the two-VM
// TM-churn amplifier arm MUST NOT be enabled (see the matching deferral
// record in ThreadManager.h's §D.1 banner).
//
// RECORDED DEVIATION (file ownership, not semantics): Structure exposes no
// transition-TID setter and Structure.h is outside U-T12's owned set, so the
// restamp writes through the JIT-exported transitionThreadLocalTIDOffset().
// A mechanical Structure::restampTransitionThreadLocalTIDForRebias() setter
// is a follow-up once Structure.h is editable. Both pokes use relaxed Atomic
// stores: mutators are stopped, but DFG/FTL compiler threads still run and
// may concurrently read these words (their stale reads are killed by the
// very fires this walk performs — any compilation specialized on a restamped
// structure watches a set that is now fired and dies at link time).
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
                        // Restamp the N1 key to 0 (the offset poke — see the
                        // banner's recorded deviation).
                        reinterpret_cast<Atomic<uint16_t>*>(reinterpret_cast<char*>(structure) + Structure::transitionThreadLocalTIDOffset())->store(0, std::memory_order_relaxed);
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
    // hence strictly before the post-resume m_freeTIDs release.
    for (Structure* structure : structuresToFire)
        structure->fireTransitionThreadLocal(vm, "UNGIL D1R: TID rebias restamped this structure's transition TID inside the shared-GC stop");

    RaceAmplifier::perturb(); // U-T12 D1R.5 stall point: post-fire, pre-Restamped flip.
}

void Heap::conductSharedCollection(GCClient::Heap& conductorClient) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    // §10 steps 3-9. Pre: GCL held (rank 2), GCA set; the conductor runs as
    // the mutator (GCConductor::Mutator, §10B.2) and may be VM-less.
    RELEASE_ASSERT(isSharedServer());
    RELEASE_ASSERT(!m_worldIsStoppedForAllClients.load(std::memory_order_acquire));
    RELEASE_ASSERT(!m_gcStopPending.load(std::memory_order_seq_cst));

    // Step 3 — stop request: seq_cst GSP = true (F8 Dekker store), release
    // our own access, then the async VMM stop. Our own trap bit is harmless
    // (we do not run JS until resume).
    m_gcStopPending.store(true, std::memory_order_seq_cst);
    if (conductorClient.hasHeapAccess())
        conductorClient.releaseHeapAccess();
    // THREADS-INTEGRATE(heap): StopReason::GC never enters VMM's
    // latch/dispatch; the keep-parked bit, park hooks, resume notify and
    // re-latch behavior are INTEGRATE-heap.md manifest item 5 (annex §A5).
    VMManager::requestStopAll(VMManager::StopReason::GC);

    // Step 4 — access barrier: under GBL, wait until every client is
    // NoAccess (F8: seq_cst samples). Entered mutators park via traps ->
    // notifyVMStop -> manifest-5a willPark; others release at their next
    // RHA/SINFAC poll; acquirers revert-and-block (F8 step 3).
    {
        Locker locker { m_gcBarrierLock }; // GBL, rank 4.
        for (;;) {
            bool anyAccess = false;
            clientSet().forEach([&](GCClient::Heap& client) { // Rank 6 inside rank 4 (§6).
                if (client.m_accessState.load(std::memory_order_seq_cst) == GCClient::Heap::hasAccessState)
                    anyAccess = true;
            });
            if (!anyAccess)
                break;
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
    auto* previousAtomStringTable = Thread::currentSingleton().setCurrentAtomStringTable(vm().atomStringTable());
    auto atomStringTableScopeExit = makeScopeExit([&] {
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
    // IncrementalSweeper is fully disabled once shared (deviation 4), leaving
    // in-lock allocation-path sweeps and conductor-side synchronous sweeps as
    // the only sweepers.

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
    // U-T12: track whether any conducted cycle in THIS stop window was a
    // FULL collection — §D.1 rebias may only run inside a full shared
    // collection's stop (ANNEX D1).
    bool sawFullCollectionThisStop = false;
    for (;;) {
        {
            Locker locker { *m_threadLock };
            if (m_requests.isEmpty()) {
                ASSERT(m_lastServedTicket == m_lastGrantedTicket);
                break;
            }
        }
        collectInMutatorThread();
        if (m_lastCollectionScope && m_lastCollectionScope.value() == CollectionScope::Full)
            sawFullCollectionThisStop = true;
    }

    // Step 7 tail — still stopped: shared mode fires the safepoint hooks
    // HERE (§9 contract notes; = OM §6's quarantine bar — the legacy
    // runEndPhase site is skipped when isSharedServer()), followed by the
    // §11 reclaim sequence (I11) under the reclaimer's own compiler-thread
    // suspension.
    runSafepointHooksAndReclaim();

    // UNGIL §D.1 TID rebias (ANNEXES D1 + D1R; U-T12 — see the banner on
    // conductTIDRebiasUnderSharedStop above). Runs HERE: still inside the
    // §10 stop (WSAC set), after every conducted cycle and the quarantine/
    // reclaim bar, strictly BEFORE the ISB1.1 generation bump (whose
    // crossModifyingCodeFence + the F8 GSP-clear synchronizes-with edge are
    // the D1R item-2 resume-side sync for the fires' jettisons) and the
    // step-8/9 resumes. The Sealed -> Restamped flip below is what licenses
    // the POST-RESUME mutator-side m_freeTIDs release (ThreadManager phase
    // 3, the SD9 gate-lift site) — so restamp + fire are complete before
    // any dead TID can be reissued. gilOffProcess-only; flag-off/GIL-on
    // this block is dead (U19/golden-disasm: zero behavior delta).
    if (VM::isGILOffProcess()) [[unlikely]] {
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
            if (sawFullCollectionThisStop) {
                conductTIDRebiasUnderSharedStop(*this, *deadTIDs);
                threadManager.noteRebiasRestampComplete();
            }
            // else: the snapshot sealed mid-stop (un-gated carrier-exit
            // retire under TM::m_lock) or only Eden tickets were granted —
            // it stays Sealed, and shouldDoFullCollection()'s probe arms
            // the NEXT conducted cycle as Full (the D1 trigger), which
            // performs the rebias in ITS stop.
        }
    }

    // End of the conductor's main-VM teardown work. The conductor's own
    // AtomStringTable is restored by atomStringTableScopeExit at function
    // return — i.e. after the step-8/9 resumes below. That ordering is
    // acceptable because nothing past this point touches atoms: steps 8-9
    // only flip allocator/barrier/VMM state, and acquireHeapAccess does not
    // create or destroy strings.

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
    // clear (ISB1.5). gilOff-process only: flag-off/GIL-on zero cost.
    if (VM::isGILOffProcess()) [[unlikely]] {
        WTF::crossModifyingCodeFence();
        jsThreadsBumpStopGeneration();
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

    // Re-acquire our own access; the §10.2 loop then re-checks the ticket.
    conductorClient.acquireHeapAccess();
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
    // can never shrink that later min(localEpoch). Flag-off every retire()
    // feeder is useJSThreads-gated (the I10 exemption), making this the
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
    if (!m_gcConductorLock.tryLock())
        return;
    {
        Locker locker { *m_threadLock };
        // While a conductor is winding down, GCA may still be set: wait
        // briefly (§10B.4-style timed waits), then re-check once.
        while (m_gcConductorActive)
            m_gcElectionCondition.waitFor(*m_threadLock, 1_ms);
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
        ticketsPending = m_lastServedTicket < m_lastGrantedTicket && !m_gcConductorActive;
        m_threadLock->unlock();
    }
    if (ticketsPending) [[unlikely]]
        tryConductSharedCollectionForPoll(*client);
}

void Heap::addStopTheWorldSafepointHook(void (*hook)(JSC::Heap&))
{
    RELEASE_ASSERT(hook);
    Locker locker { m_stopTheWorldSafepointHookLock };
    m_stopTheWorldSafepointHooks.append(hook);
}

void Heap::runStopTheWorldSafepointHooks()
{
    // §9 contract notes: fires once per collection in BOTH protocols. Call
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

// SPEC-ungil §B (heap Dev 8: ONE GCClient PER Thread, NEVER the main
// client): resolve the client whose TLC/iso LocalAllocators the CURRENT
// thread may allocate through. A spawned thread (§B.1,
// attachSpawnedThreadGCClient) and a GIL-off carrier thread (ANNEX A36C)
// stamp their OWN client into the §10A.1 TLS slot; routing allocation
// through vm.clientHeap on such a thread hands them the MAIN thread's
// LocalAllocators — an unsynchronized FreeList shared across mutators (I3).
// Gate is vm.gilOff() (review amendment; NOT isSharedServer()), mirroring
// gcWillParkInStopTheWorld()/gcDidResumeFromStopTheWorld() below: GIL-on and
// flag-off stay identity BY CONSTRUCTION (no TLS consultation at all — under
// isSharedServer()-only gating a GIL-on thread nested across two VMs sharing
// one server would re-route the outer VM's allocations into the inner VM's
// client), and spawned-client stamps cannot exist GIL-on
// (RELEASE_ASSERT(vm.gilOff()) in attachSpawnedThreadGCClient), so the
// narrower gate covers every failing case. Unstamped threads (GC helpers,
// pre-attach): the VM's original client, today's behavior (I10).
//
// APPLY-SCOPE NOTE (I4, reviewed 3/3 approve-with-amendments; this round's
// write scope was Heap.cpp/LocalAllocator.cpp only, so the Heap.h half could
// not land atomically): the helper intentionally stays namespace-scope and
// uncalled — a Heap::-qualified out-of-line definition without its in-class
// declaration is a hard build break in every configuration. Outstanding for
// the next round that may write Heap.h, ALL-OR-NOTHING:
//   (1) Promote to a `static` Heap member declared AND defined ALWAYS_INLINE
//       in Heap.h next to the deferralDepthSlot()/mutatorStateSlot()
//       dispatchers — the consuming sites are ALWAYS_INLINE VM.h iso
//       accessors on the hottest allocation path, so an out-of-line call
//       would regress the flag-off bench gate. Do NOT leave the body
//       out-of-line in Heap.cpp with only a declaration in Heap.h (review
//       amendment: that bakes a per-allocation call+branch into the
//       serialized fast path the moment the routing edits consume it).
//   (2) With the GCClient::Heap friendship a Heap member has (Heap.h
//       `friend class JSC::Heap`), strengthen the fallback tripwire below to
//       the access-OWNER identity form:
//       `vm.heap.worldIsStoppedForAllClients() ||
//        vmOriginalClient.m_accessOwner.load(std::memory_order_relaxed) ==
//        &Thread::currentSingleton()` — hasHeapAccess() alone would pass
//       while ANOTHER thread holds the main client's access, the exact racy
//       fallback (same pattern as currentThreadIsAllocatorOwner above).
//       Before the helper goes live, re-validate the tightened ASSERT
//       against non-mutator allocation entry points (GC helpers, finalizer/
//       sweeper, pre-attach): if a legitimate unstamped caller trips it,
//       stamp that caller — never weaken the assert back.
//   (3) Consume it at the three vm.clientHeap routing sites: the VM.h iso
//       subspace accessors (including the dynamic clientHeap.name<mode>()
//       template form and codeBlockSpace()), CompleteSubspaceInlines.h
//       allocate(), and CompleteSubspace.cpp tryAllocateSlow(). Any
//       unconverted allocation entry path keeps racing exactly as before;
//       until all three land, the spawned thread still reaches the main
//       client's LocalAllocator and the (correct, load-bearing) ownership
//       ASSERT in LocalAllocator::allocateSlowCase still fires
//       (ta-wait-thread-gate.js stays red — re-verify only after the
//       companion round).
GCClient::Heap& allocationClientForCurrentThread(VM&, GCClient::Heap&);
GCClient::Heap& allocationClientForCurrentThread(VM& vm, GCClient::Heap& vmOriginalClient)
{
    ASSERT(&vmOriginalClient.server() == &vm.heap);
    if (vm.gilOff()) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        if (client && &client->server() == &vm.heap) {
            // I2: an allocating thread must hold ITS client's access; a
            // stamped thread must not silently fall back to the main client
            // (that re-creates the shared-FreeList race).
            ASSERT(client->hasHeapAccess() || vm.heap.worldIsStoppedForAllClients());
            return *client;
        }
        // Review tripwire: under sticky GIL-off every legitimate mutator is
        // stamped before its first allocation (JSLock forwarding, §B.1
        // attach, A36C carrier swap), so falling back to the main client is
        // only legal when its access is genuinely held or the world is
        // stopped — otherwise this fallback would silently reintroduce the
        // exact cross-thread FreeList race in release builds. Converts the
        // remaining window into a deterministic debug abort.
        // FIX-3: mirrors the Heap.h member's IT-9 compilation-thread
        // carve-out (see the comment there); JIT worklist threads are
        // unstampable pointer-read-only callers.
        ASSERT(Thread::currentSingleton().isCompilationThread()
            || vm.heap.worldIsStoppedForAllClients() || vmOriginalClient.hasHeapAccess());
    }
    return vmOriginalClient;
}

// --- §10A access forwarding (T2) ---

NEVER_INLINE void Heap::acquireAccessForwardedToMainClient()
{
    GCClient::Heap* mainClient = m_mainClient;
    RELEASE_ASSERT(mainClient);
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
        // UNGIL ANNEX ISB1.2 (U-T5, review round): this transfer branch is a
        // may-execute-JIT transition that bypasses AHA entirely, so it must
        // carry the ISB1.2 compare itself: the process-wide generation can
        // have been bumped by ANOTHER gilOff VM's §A.3 window or by a shared-
        // GC window that completed before this client (re)acquired — the
        // incoming thread's PE may never have executed an ISB for it
        // (t_jsThreadsStopGenerationSeen is per-thread because instruction-
        // stream sync is per-PE). No §A.3.2b stop-word/Mode gate is needed
        // here: a window targeting THIS VM cannot be open or close while the
        // main client holds access continuously (its holder lite is counted
        // non-quiescent by the §A.3.2 predicate and the §10.4 barrier), and
        // other VMs' windows never gate this VM's acquisition. GIL-on/
        // flag-off: branch dead (gilOff() false).
        if (vm().gilOff()) [[unlikely]]
            jsThreadsSyncToStopGenerationBeforeJITEntry();
        return;
    }
    mainClient->acquireHeapAccess();
}

NEVER_INLINE void Heap::releaseAccessForwardedToMainClient()
{
    GCClient::Heap* mainClient = m_mainClient;
    RELEASE_ASSERT(mainClient);
    mainClient->releaseHeapAccess();
}

bool Heap::mainClientHasHeapAccess() const
{
    GCClient::Heap* mainClient = m_mainClient;
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

Heap::JSThreadsStopScope::JSThreadsStopScope(JSC::Heap& heap) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    : m_heap(heap)
{
    // §10C/CS2: the GCL (rank 2) bracket for a JSThreads/debugger stop.
    // Pre: caller released heap access (I6); no bumpAndReclaim() inside the
    // scope (jit R4/CS4 refused — JSThreads stops enqueue a GC request).
    // !isSharedServer() => no-op.
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
    m_heap.m_gcConductorLock.lock();
    m_didLock = true;
}

Heap::JSThreadsStopScope::JSThreadsStopScope(JSC::Heap& heap, MonotonicTime watchdogRequestStart) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    : m_heap(heap)
{
    // Watchdog-covered GCL acquisition (review round): same bracket as the
    // blocking ctor above, but the wait is covered by the 30s stop watchdog —
    // a conductor queued behind a shared GC that never converges (or a GCL
    // wedge) fail-stops with the standard timeout diagnostics instead of
    // hanging unwatched forever. Quantum: 1ms tryLock; cost is nil on the
    // uncontended path (first tryLock succeeds).
    if (!m_heap.isSharedServer())
        return;
    ASSERT(!currentThreadHasSTWForbiddenScope()); // I14/L5.
#if ASSERT_ENABLED
    if (GCClient::Heap* client = GCClient::Heap::currentThreadClient())
        ASSERT(!client->hasHeapAccess());
#endif
    while (!m_heap.m_gcConductorLock.tryLock()) {
        JSThreadsSafepoint::watchdogAssertStopProgress(watchdogRequestStart, nullptr);
        WTF::sleep(Seconds::fromMilliseconds(1));
    }
    m_didLock = true;
}

Heap::JSThreadsStopScope::~JSThreadsStopScope() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    if (m_didLock)
        m_heap.m_gcConductorLock.unlock();
}

// --- End shared heap server ---

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
    if (sharedTeardown && !hasHeapAccess())
        acquireHeapAccess(); // F8: blocks while a stop is pending; threads other than the attached one re-assert I2 via the step-0/owner checks.
    lastChanceToFinalize();
    // Now drop access: detach if this thread is still attached (also parks
    // the epoch at MAX and clears the §10A.1 TLS slot; threads other than
    // the current one must have detached already, §9 lifecycle), else
    // release the bracket access directly.
    if (currentThreadClient() == this)
        detachCurrentThread();
    else if (sharedTeardown && hasHeapAccess())
        releaseHeapAccess();
    // Unregister last: a stop that begins the moment remove() unblocks can
    // no longer touch our state — every allocator of ours is stopped and
    // unlinked from the shared directories, and we hold no access (remove()
    // asserts that; it also defers across an in-flight stop window, I13).
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
    // §11: a detached client never holds up reclamation.
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
            jsThreadsParkForModeStop(*serverVM);
            continue; // Retry from step 1 (GSP and the §A.3 word re-poll).
        }

        // Re-stamp the owner (§10A; I2: JSLock migration transfers via the
        // server-side forwarding, which re-stamps before/instead of AHA).
        m_accessOwner.store(&currentThread, std::memory_order_relaxed);

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
        WTF::storeStoreFence(); \
        m_##name = WTF::move(space); \
        return m_##name.get(); \
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

void gcClientDidResumeFromThreadGranularStop()
{
    if (!t_releasedByThreadGranularPark)
        return; // Idempotent: nothing was released by the matching willPark.
    GCClient::Heap* client = GCClient::Heap::currentThreadClient();
    RELEASE_ASSERT(client); // The releasing thread cannot lose its client while parked (teardown re-acquires first, EXIT1.3).
    client->acquireHeapAccess(); // F8 + the §A.3.2b stop-word gate: blocks across any pending stop.
    t_releasedByThreadGranularPark = false;
}

} // namespace JSC
