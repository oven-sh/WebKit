/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer. 
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution. 
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "VM.h"

#include "ConcurrentButterfly.h"
#include "PropertyTable.h"
#include "VMLiteInlines.h" // UNGIL §E.1/I11 (U-T9): per-lite microtask enqueue/drain reroute.
#include "VMLiteShared.h"

#include "AbortReason.h"
#include "AccessCase.h"
#include "AggregateError.h"
#include "ArgList.h"
#include "BuiltinExecutables.h"
#include "BytecodeIntrinsicRegistry.h"
#include "CallMode.h"
#include "CheckpointOSRExitSideState.h"
#include "CodeBlock.h"
#include "CodeCache.h"
#include "CommonIdentifiers.h"
#include "ControlFlowProfiler.h"
#include "CrossTaskToken.h"
#include "CustomGetterSetterInlines.h"
#include "DOMAttributeGetterSetterInlines.h"
#include "Debugger.h"
#include "DeferredWorkTimer.h"
#include "Disassembler.h"
#include "DoublePredictionFuzzerAgent.h"
#include "ErrorInstance.h"
#include "EvalCodeBlockInlines.h"
#include "EvalExecutableInlines.h"
#include "Exception.h"
#include "FTLThunks.h"
#include "FileBasedFuzzerAgent.h"
#include "FunctionCodeBlockInlines.h"
#include "FunctionExecutableInlines.h"
#include "GetterSetterInlines.h"
#include "GigacageAlignedMemoryAllocator.h"
#include "GlobalObjectMethodTable.h"
#include "HasOwnPropertyCache.h"
#include "Heap.h"
#include "HeapInlines.h"
#include "HeapProfiler.h"
#include "IncrementalSweeper.h"
#include "Interpreter.h"
#include "IntlCache.h"
#include "IntlObject.h"
#include "JITCode.h"
#include "JITOperationList.h"
#include "JITSizeStatistics.h"
#include "JITThunks.h"
#include "JITWorklist.h"
#include "JSAPIValueWrapper.h"
#include "JSBigInt.h"
#include "JSCellButterflyInlines.h"
#include "JSGlobalObject.h"
#include "JSIterator.h"
#include "JSLock.h"
#include "JSMap.h"
#include "JSMicrotask.h"
#include "JSMicrotaskDispatcher.h"
#include "JSModuleLoaderInlines.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsContextInlines.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSPromiseConstructor.h"
#include "JSPromiseReaction.h"
#include "JSPropertyNameEnumeratorInlines.h"
#include "JSSentinelInlines.h"
#include "JSSet.h"
#include "JSSourceCodeInlines.h"
#include "JSTemplateObjectDescriptorInlines.h"
#include "JSToWasm.h"
#include "LLIntData.h"
#include "LLIntExceptions.h"
#include "MarkedBlockInlines.h"
#include "MegamorphicCache.h"
#include "MicrotaskQueueInlines.h"
#include "MinimumReservedZoneSize.h"
#include "ModuleGraphLoadingStateInlines.h"
#include "ModuleLoadingContextInlines.h"
#include "ModuleLoaderPayloadInlines.h"
#include "ModuleProgramCodeBlockInlines.h"
#include "ModuleProgramExecutableInlines.h"
#include "ModuleRegistryEntryInlines.h"
#include "NarrowingNumberPredictionFuzzerAgent.h"
#include "NativeExecutable.h"
#include "NumberObject.h"
#include "PinballCompletion.h"
#include "PredictionFileCreatingFuzzerAgent.h"
#include "ProfilerDatabase.h"
#include "ProgramCodeBlockInlines.h"
#include "RaceAmplifier.h"
#include "ProgramExecutableInlines.h"
#include "PropertyInlineCache.h"
#include "PropertyTableInlines.h"
#include "RandomizingFuzzerAgent.h"
#include "RegExpCache.h"
#include "RegExpInlines.h"
#include "ResourceExhaustion.h"
#include "SamplingProfiler.h"
#include "ScopedArguments.h"
#include "ShadowChicken.h"
#include "SharedJITStubSet.h"
#include "SideDataRepository.h"
#include "SimpleTypedArrayController.h"
#include "SourceProviderCache.h"
#include "StrongInlines.h"
#include "StructureChainInlines.h"
#include "StructureInlines.h"
#include "SubspaceInlines.h"
#include "SymbolInlines.h"
#include "SymbolTableInlines.h"
#include "TestRunnerUtils.h"
#include "ThreadManager.h"
#include "ThunkGenerators.h"
#include "TypeProfiler.h"
#include "TypeProfilerLog.h"
#include "UnlinkedEvalCodeBlockInlines.h"
#include "UnlinkedFunctionCodeBlockInlines.h"
#include "UnlinkedFunctionExecutableInlines.h"
#include "UnlinkedModuleProgramCodeBlockInlines.h"
#include "UnlinkedProgramCodeBlockInlines.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"
#include "VMManager.h"
#include "VMTrapsInlines.h"
#include "VariableEnvironment.h"
#include "WaiterListManager.h"
#include "WasmDebugServerUtilities.h"
#include "WasmExecutionHandler.h"
#include "WasmWorklist.h"
#include "Watchdog.h"
#include "WeakGCMapInlines.h"
#include "WideningNumberPredictionFuzzerAgent.h"
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/MainThread.h>
#include <wtf/MemoryPressureHandler.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>
#include <wtf/ReadWriteLock.h>
#include <wtf/SimpleStats.h>
#include <wtf/StackTrace.h>
#include <wtf/StringPrintStream.h>
#include <wtf/SystemTracing.h>
#include <wtf/Threading.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/StringToIntegerConversion.h>

#if ENABLE(DFG_JIT) || ENABLE(WEBASSEMBLY)
#include "ConservativeRoots.h"
#endif

#if ENABLE(REGEXP_TRACING)
#include "RegExp.h"
#endif

#if ENABLE(WEBASSEMBLY)
#include "JSWebAssemblyInstance.h"
#include "JSWebAssemblyStreamingContextInlines.h"
#endif

#if PLATFORM(COCOA)
#include <notify.h>
#include <wtf/darwin/DispatchExtras.h>
#endif

#if ENABLE(WEBASSEMBLY_DEBUGGER)
#include "WasmDebugServerUtilities.h"
#endif

#include <span>

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(VM);

// UNGIL §E.1b.4/SD15 (U-T8e) — forward declarations for the rejection-tracker
// carrier-handoff machinery defined ahead of VM::callPromiseRejectionCallback
// below; ~VM (purge) and the non-static seams are consumed before that point.
void enqueuePromiseRejectionTrackerHandoffRecord(VM&, JSPromise*, JSPromiseRejectionOperation);
void flushPromiseRejectionTrackerHandoffRecords(VM&);
void notifyPromiseRejectionTrackerCrossThreadAware(JSGlobalObject*, JSPromise*, JSPromiseRejectionOperation);
static void purgePromiseRejectionHandoffRecordsAtVMDestruction(VM&);
// UNGIL §E.7.3 (U-T9): jsThreadsPurgeCrossThreadDeferredWorkAtVMDestruction
// (DeferredWorkTimer.cpp) is consumed in ~VM below via its ThreadManager.h
// declaration (already included by this TU).

// ===========================================================================
// UNGIL §A.1.1 (U-T3): g_jscCurrentVMLite — the JIT/LLInt-visible mirror of
// vmstate L4's `t_currentVMLite` (runtime/VMLite.cpp). The L4 freeze keeps
// the C++ accessors (VMLite::current/currentIfExists/setCurrent) backed by
// the plain thread_local in VMLite.cpp; generated code instead reads THIS
// symbol, per SPEC-jit annex App. R5 mechanics (the g_jscButterflyTIDTag
// precedent, jit/ConcurrentButterflyOperations.cpp):
//
// - ELF (Linux glibc+musl): initial-exec model => the symbol's TPOFF is
//   link-time thread-invariant. The JIT-tier read is LANDED (the
//   loadVMLite free-function emitter in jit/AssemblyHelpers.cpp; the
//   member surface follows with the emission slice). The LLInt side —
//   the `loadVMLite` offlineasm macro reading this symbol with
//   initial-exec relocations (x86-64 `movq %fs:g_jscCurrentVMLite@TPOFF`;
//   arm64 mrs + :tprel_hi12:/:tprel_lo12_nc:) plus the per-Group-3-site
//   two-level selection and the JSCConfig `gilOffProcess` byte — is NOT
//   YET LANDED: llint/ and runtime/JSCConfig.h are outside this slice's
//   writable file set (OPEN U-T3 obligation, INTEGRATE-ungil.md 9b;
//   escalated at the U-T3 amendment — the licensed flag-off golden-disasm
//   re-baseline for the LLInt Group-3 branches happens when those branches
//   land, not before). The symbol is extern "C" (unmangled
//   asm-referenceable name) and defined OUTSIDE any ENABLE(JIT) region so
//   that LLInt slice can bind to it unchanged.
// - Darwin: Mach-O TLV has no constant offset; the JIT-visible copy lives in
//   a pthread TSD slot whose key is to be published through the M4a-style
//   JSCConfig slot (vmLiteTLSKey, beside butterflyTIDTagTLSKey). That slot,
//   its key creation, and the per-thread pthread_setspecific are NOT YET
//   LANDED (same writable-set constraint; see the Darwin arm in
//   jit/AssemblyHelpers.cpp). This thread_local still exists there
//   (harmless; generated code will read the TSD slot, not this symbol).
//
// COHERENCE CONTRACT (the App. R5 CS3 discipline, applied to the lite
// pointer): VMLite::setCurrent — the SOLE writer of t_currentVMLite — must
// mirror every TLS write here (and, on Darwin, pthread_setspecific the same
// value into the vmLiteTLSKey slot), immediately after the t_currentVMLite
// store and before its TID-tag hook fires — INCLUDING the null/uninstall
// writes (carrier teardown, thread exit): an unmirrored clear would leave a
// reused thread's generated code reading a stale/freed lite. STATUS: the
// mirror store is NOT YET IMPLEMENTED (runtime/VMLite.cpp is outside this
// slice's writable set), and NO existing INTEGRATE-ungil.md obligation row
// covers it — obligation 9b covers only the Config byte / LLInt selection /
// loadVMLite emitter / VMEntryRecord slot. A dedicated IU obligation row
// (owner: the VMLite.cpp slice) must be added before any task emits live
// reads of this symbol; escalated at the U-T3 amendment. Until the mirror
// lands, this symbol stays null on every thread — dark-safe: only
// gilOff-mode compilations (§A.1.3 COMPILED-FOR-VM-mode rule) emit reads of
// it, and no shipping configuration constructs a gilOff VM.
// ===========================================================================
#if OS(LINUX)
extern "C" __attribute__((tls_model("initial-exec"))) thread_local VMLite* g_jscCurrentVMLite = nullptr;
#else
extern "C" thread_local VMLite* g_jscCurrentVMLite = nullptr;
#endif

MicrotaskQueue& VM::defaultMicrotaskQueue() { return m_defaultMicrotaskQueue.get(); }

#if ENABLE(GC_VALIDATION)
// Per-thread (see the VM.h declaration comment): GIL-off N mutators share a
// VM, and an object initialization is one thread's stack property.
thread_local const ClassInfo* VM::s_initializingObjectClass { nullptr };
#endif

// UNGIL §F.2 (U-T8): defined in JSLock.cpp (the token machinery's home);
// same-library linkage, deliberately not declared in any header — the
// predicate split is an implementation detail of the two functions below.
bool currentThreadHoldsEntryToken(const VM&);

bool VM::currentThreadIsHoldingAPILock() const
{
    // UNGIL §F.2 (U-T8), the predicate split: GIL-off this predicate is
    // REDEFINED as "the current thread holds an entry token for this VM" —
    // the host-call assert meaning (DWT §E.7.2; U12/U13). Spawned threads
    // never touch JSLock::m_lock (§F.1) and main/embedder m_lock holders
    // ALSO hold a token (F1B), so the token question subsumes the mutex one.
    // JSLock::currentThreadIsHoldingLock() stays MUTEX-LITERAL — §F.4's DAL
    // handling + the m_lockDropDepth LIFO depend on it; consumers needing
    // the mutex meaning ask the JSLock directly (IU table rows 19/22/26/36,
    // JSLock.cpp). GIL-on (m_gilOff == 0 — every shipping configuration):
    // bit-identical to the landed mutex forward.
    if (m_gilOff) [[unlikely]]
        return currentThreadHoldsEntryToken(*this);
    return m_apiLock->currentThreadIsHoldingLock();
}

JSLock& VM::apiLock() { return m_apiLock.get(); }

// Note: Platform.h will enforce that ENABLE(ASSEMBLER) is true if either
// ENABLE(JIT) or ENABLE(YARR_JIT) or both are enabled. The code below
// just checks for ENABLE(JIT) or ENABLE(YARR_JIT) with this premise in mind.

#if ENABLE(ASSEMBLER)
static bool enableAssembler()
{
    if (!Options::useJIT())
        return false;

    auto canUseJITString = unsafeSpan(getenv("JavaScriptCoreUseJIT"));
    if (canUseJITString.data() && !parseInteger<int>(canUseJITString).value_or(0))
        return false;

    ExecutableAllocator::initializeUnderlyingAllocator();
    if (!ExecutableAllocator::singleton().isValid()) {
        if (Options::crashIfCantAllocateJITMemory())
            CRASH();
        return false;
    }

    return true;
}
#endif // ENABLE(!ASSEMBLER)

bool VM::canUseAssembler()
{
#if ENABLE(ASSEMBLER)
    static std::once_flag onceKey;
    static bool enabled = false;
    std::call_once(onceKey, [] {
        enabled = enableAssembler();
    });
    return enabled;
#else
    return false; // interpreter only
#endif
}

void VM::computeCanUseJIT()
{
#if ENABLE(JIT)
#if ASSERT_ENABLED
    RELEASE_ASSERT(!g_jscConfig.vm.canUseJITIsSet);
    g_jscConfig.vm.canUseJITIsSet = true;
#endif
    g_jscConfig.vm.canUseJIT = VM::canUseAssembler() && Options::useJIT();
#endif
}

static bool vmCreationShouldCrash = false;

VM::VM(VMType vmType, HeapType heapType, WTF::RunLoop* runLoop, bool* success)
    : topCallFrame(CallFrame::noCaller())
    , m_identifier(VMIdentifier::generate())
    , m_apiLock(adoptRef(*new JSLock(this)))
    , m_runLoop(runLoop ? *runLoop : WTF::RunLoop::currentSingleton())
    , m_random(Options::seedOfVMRandomForFuzzer() ? Options::seedOfVMRandomForFuzzer() : cryptographicallyRandomNumber<uint32_t>())
    , m_heapRandom(Options::seedOfVMRandomForFuzzer() ? Options::seedOfVMRandomForFuzzer() : cryptographicallyRandomNumber<uint32_t>())
    , m_integrityRandom(*this)
    , heap(*this, heapType)
    , clientHeap(heap)
    , vmType(vmType)
    , deferredWorkTimer(DeferredWorkTimer::create(*this))
    , m_atomStringTable(vmType == VMType::Default ? Thread::currentSingleton().atomStringTable() : new AtomStringTable)
    , m_symbolRegistry(makeUniqueRef<SymbolRegistry>())
    , m_privateSymbolRegistry(makeUniqueRef<SymbolRegistry>(SymbolRegistry::Type::PrivateSymbol))
    , emptyList(new ArgList)
    , machineCodeBytesPerBytecodeWordForBaselineJIT(makeUnique<SimpleStats>())
    // These per-VM WeakGCMap caches are mutated from every JS thread when
    // useJSThreads is on (one shared VM under VMLite), so they opt into
    // WeakGCMap's internal leaf lock (SPEC-ungil §H / §LK.7). With
    // useJSThreads off they stay lock-free, exactly as before.
    , symbolImplToSymbolMap(*this, Options::useJSThreads() ? WeakGCMapLocking::Yes : WeakGCMapLocking::No)
    , atomStringToJSStringMap(*this, Options::useJSThreads() ? WeakGCMapLocking::Yes : WeakGCMapLocking::No)
#if ENABLE(WEBASSEMBLY)
    , wasmGCStructureMap(*this, Options::useJSThreads() ? WeakGCMapLocking::Yes : WeakGCMapLocking::No)
#endif
    , m_regExpCache(makeUnique<RegExpCache>())
    , m_compactVariableMap(adoptRef(*new CompactTDZEnvironmentMap))
    , m_codeCache(makeUnique<CodeCache>())
    , m_intlCache(makeUnique<IntlCache>())
    , m_builtinExecutables(makeUnique<BuiltinExecutables>(*this))
    , m_defaultMicrotaskQueue(MicrotaskQueue::create(*this))
    , m_syncWaiter(adoptRef(*new Waiter(this)))
{
    if (vmCreationShouldCrash || g_jscConfig.vmCreationDisallowed) [[unlikely]]
        CRASH_WITH_EXTRA_SECURITY_IMPLICATION_AND_INFO(VMCreationDisallowed, "VM creation disallowed"_s, 0x4242424220202020, 0xbadbeef0badbeef, 0x1234123412341234, 0x1337133713371337);

    // ANNEX A36 (UNGIL U-T1): process-monotonic epoch for the per-thread
    // VM->carrier TLS maps (stale-epoch detection; never 0).
    {
        static std::atomic<uint64_t> s_nextVMEpoch { 1 };
        m_vmEpoch = s_nextVMEpoch.fetch_add(1, std::memory_order_relaxed);
    }

    // UNGIL §0 U0c (ANNEX U0C, BINDING): m_gilOff is computed ONCE, here —
    // BEFORE m_mainVMLite registration (end of this ctor), any entry
    // (including this ctor's own JSLockHolder below), and any codegen
    // (including the JIT thunk initialization below) — and is IMMUTABLE for
    // the VM's lifetime. Under gilOffProcess every VM ctor races the
    // designation CAS (Heap::tryDesignateStickySharedServer — won/lost, NO
    // assert): the WINNER is the one m_gilOff VM per process (U0b) and
    // eagerly flips sticky-ISS at clientSet()==1 (quiescence trivial at
    // birth; noteSharedServerSticky's inner CAS sees previous==this, so I13
    // stands textually unchanged and never fires on this path). A LOSER
    // keeps m_gilOff=0 and the GIL-on single-migrating-client protocol; U0b
    // spawn-refusal keeps its clientSet()<=1, so the HeapClientSet::add
    // trigger never runs for it.
    // AB17g item 2 (F1): latch the Config gilOffProcess byte BEFORE the
    // m_gilOff designation below, so gilOffWithProcessGate()'s process-byte
    // test alone is sufficient in every reachable state (the VM.h fallback
    // term is dropped). Options are finalized strictly before any VM ctor
    // (InitializeThreading), so the latch's isFinalized assert cannot fire
    // here. The Config::finalize() call later in this ctor is unchanged:
    // its latch call is a spent-call_once no-op, and the forceFencedBarrier
    // options store just before it keeps its required position BEFORE the
    // WTF::Config::finalize() freeze (M8/GT#7).
    Config::latchGILOffProcess();
    if (VM::isGILOffProcess()) [[unlikely]] {
        // AB17g amendment (d): designation must never precede the latch.
        ASSERT(g_jscConfig.gilOffProcess);
        if (heap.tryDesignateStickySharedServer()) {
            m_gilOff = true;
            // AB17c F4: re-stamp the HandleSet's cached §F.3 mode byte. The
            // HandleSet was constructed in this ctor's INIT LIST (Heap
            // member), i.e. before m_gilOff above was computed, so its ctor
            // stamp is always false; without this re-stamp every Strong
            // allocate/free/barrier takes the UNLOCKED inline arm GIL-off
            // and two threads race m_freeList (observed: double-allocated
            // Strong slot under counter-lock.js — a spawned thread's
            // property-wait Strong clobbered the carrier's in-flight
            // UnlinkedCodeBlockGenerator codeBlock handle). Still
            // single-threaded and unpublished here, so the write is
            // pre-publication (see noteOwnerVMDesignatedGILOff()).
            heap.handleSet()->noteOwnerVMDesignatedGILOff();
            // U0c invariant check immediately before EVERY in-scope
            // noteSharedServerSticky() trigger (annex U0C). The second
            // trigger family — HeapClientSet::add's second-client site
            // (HeapClientSet.cpp:69) — is OUTSIDE this slice's writable set
            // and remains UNWIRED; see the declaration comment in
            // heap/Heap.h and INTEGRATE-ungil.md ledger row 6 (still open).
            heap.verifyStickySharedServerDesignation();
            heap.noteSharedServerSticky();
        }
    }

    // Arm the race amplifier (no-op unless --randomYieldPeriod is set).
    // Idempotent across VM constructions; see runtime/RaceAmplifier.h.
    RaceAmplifier::initialize();

    // Set up lazy initializers.
    {
        m_hasOwnPropertyCache.initLater([](VM&, auto& ref) {
            ref.set(HasOwnPropertyCache::create());
        });

        m_megamorphicCache.initLater([](VM&, auto& ref) {
            ref.set(makeUniqueRef<MegamorphicCache>());
        });

        m_shadowChicken.initLater([](VM&, auto& ref) {
            ref.set(makeUniqueRef<ShadowChicken>());
        });

        m_heapProfiler.initLater([](VM& vm, auto& ref) {
            ref.set(makeUniqueRef<HeapProfiler>(vm));
        });

        m_stringSearcherTables.initLater([](VM&, auto& ref) {
            ref.set(makeUniqueRef<AdaptiveStringSearcherTables>());
        });

        m_watchdog.initLater([](VM& vm, auto& ref) {
            ref.set(adoptRef(*new Watchdog(&vm)));
            vm.ensureTerminationException();
            vm.requestEntryScopeService(EntryScopeService::Watchdog);
        });
    }

    updateSoftReservedZoneSize(Options::softReservedZoneSize());
    setLastStackTop(Thread::currentSingleton());
    stringSplitIndice.reserveInitialCapacity(256);

    JSRunLoopTimer::Manager::singleton().registerVM(*this);

    // Need to be careful to keep everything consistent here
    JSLockHolder lock(this);
    AtomStringTable* existingEntryAtomStringTable = Thread::currentSingleton().setCurrentAtomStringTable(m_atomStringTable);
    structureStructure.setWithoutWriteBarrier(Structure::createStructure(*this));
    structureRareDataStructure.setWithoutWriteBarrier(StructureRareData::createStructure(*this, nullptr, jsNull()));
    stringStructure.setWithoutWriteBarrier(JSString::createStructure(*this, nullptr, jsNull()));

    smallStrings.initializeCommonStrings(*this);
    numericStrings.initializeSmallIntCache(*this);

    propertyNames = new CommonIdentifiers(*this);
    propertyNameEnumeratorStructure.setWithoutWriteBarrier(JSPropertyNameEnumerator::createStructure(*this, nullptr, jsNull()));
    getterSetterStructure.setWithoutWriteBarrier(GetterSetter::createStructure(*this, nullptr, jsNull()));
    customGetterSetterStructure.setWithoutWriteBarrier(CustomGetterSetter::createStructure(*this, nullptr, jsNull()));
    domAttributeGetterSetterStructure.setWithoutWriteBarrier(DOMAttributeGetterSetter::createStructure(*this, nullptr, jsNull()));
    scopedArgumentsTableStructure.setWithoutWriteBarrier(ScopedArgumentsTable::createStructure(*this, nullptr, jsNull()));
    apiWrapperStructure.setWithoutWriteBarrier(JSAPIValueWrapper::createStructure(*this, nullptr, jsNull()));
    nativeExecutableStructure.setWithoutWriteBarrier(NativeExecutable::createStructure(*this, nullptr, jsNull()));
    evalExecutableStructure.setWithoutWriteBarrier(EvalExecutable::createStructure(*this, nullptr, jsNull()));
    programExecutableStructure.setWithoutWriteBarrier(ProgramExecutable::createStructure(*this, nullptr, jsNull()));
    functionExecutableStructure.setWithoutWriteBarrier(FunctionExecutable::createStructure(*this, nullptr, jsNull()));
#if ENABLE(WEBASSEMBLY)
    pinballCompletionStructure.setWithoutWriteBarrier(PinballCompletion::createStructure(*this, nullptr, jsNull()));
    webAssemblyStreamingContextStructure.setWithoutWriteBarrier(JSWebAssemblyStreamingContext::createStructure(*this, nullptr, jsNull()));
#endif
    moduleProgramExecutableStructure.setWithoutWriteBarrier(ModuleProgramExecutable::createStructure(*this, nullptr, jsNull()));
    slimPromiseReactionStructure.setWithoutWriteBarrier(JSSlimPromiseReaction::createStructure(*this, nullptr, jsNull()));
    fullPromiseReactionStructure.setWithoutWriteBarrier(JSFullPromiseReaction::createStructure(*this, nullptr, jsNull()));
    jsMicrotaskDispatcherStructure.setWithoutWriteBarrier(JSMicrotaskDispatcher::createStructure(*this, nullptr, jsNull()));
    moduleLoaderStructure.setWithoutWriteBarrier(JSModuleLoader::createStructure(*this, nullptr, jsNull()));
    moduleRegistryEntryStructure.setWithoutWriteBarrier(ModuleRegistryEntry::createStructure(*this, nullptr, jsNull()));
    moduleLoadingContextStructure.setWithoutWriteBarrier(ModuleLoadingContext::createStructure(*this, nullptr, jsNull()));
    moduleLoaderPayloadStructure.setWithoutWriteBarrier(ModuleLoaderPayload::createStructure(*this, nullptr, jsNull()));
    moduleGraphLoadingStateStructure.setWithoutWriteBarrier(ModuleGraphLoadingState::createStructure(*this, nullptr, jsNull()));
    promiseCombinatorsContextStructure.setWithoutWriteBarrier(JSPromiseCombinatorsContext::createStructure(*this, nullptr, jsNull()));
    promiseCombinatorsGlobalContextStructure.setWithoutWriteBarrier(JSPromiseCombinatorsGlobalContext::createStructure(*this, nullptr, jsNull()));
    regExpStructure.setWithoutWriteBarrier(RegExp::createStructure(*this, nullptr, jsNull()));
    symbolStructure.setWithoutWriteBarrier(Symbol::createStructure(*this, nullptr, jsNull()));
    symbolTableStructure.setWithoutWriteBarrier(SymbolTable::createStructure(*this, nullptr, jsNull()));

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    rawImmutableButterflyStructure(CopyOnWriteArrayWithInt32).setWithoutWriteBarrier(JSCellButterfly::createStructure(*this, nullptr, jsNull(), CopyOnWriteArrayWithInt32));
    Structure* copyOnWriteArrayWithContiguousStructure = JSCellButterfly::createStructure(*this, nullptr, jsNull(), CopyOnWriteArrayWithContiguous);
    rawImmutableButterflyStructure(CopyOnWriteArrayWithDouble).setWithoutWriteBarrier(Options::allowDoubleShape() ? JSCellButterfly::createStructure(*this, nullptr, jsNull(), CopyOnWriteArrayWithDouble) : copyOnWriteArrayWithContiguousStructure);
    rawImmutableButterflyStructure(CopyOnWriteArrayWithContiguous).setWithoutWriteBarrier(copyOnWriteArrayWithContiguousStructure);
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    // This is only for JSCellButterfly filled with atom strings.
    cellButterflyOnlyAtomStringsStructure.setWithoutWriteBarrier(JSCellButterfly::createStructure(*this, nullptr, jsNull(), CopyOnWriteArrayWithContiguous));

    sourceCodeStructure.setWithoutWriteBarrier(JSSourceCode::createStructure(*this, nullptr, jsNull()));
    structureChainStructure.setWithoutWriteBarrier(StructureChain::createStructure(*this, nullptr, jsNull()));
    sparseArrayValueMapStructure.setWithoutWriteBarrier(SparseArrayValueMap::createStructure(*this, nullptr, jsNull()));
    templateObjectDescriptorStructure.setWithoutWriteBarrier(JSTemplateObjectDescriptor::createStructure(*this, nullptr, jsNull()));
    unlinkedFunctionExecutableStructure.setWithoutWriteBarrier(UnlinkedFunctionExecutable::createStructure(*this, nullptr, jsNull()));
    unlinkedProgramCodeBlockStructure.setWithoutWriteBarrier(UnlinkedProgramCodeBlock::createStructure(*this, nullptr, jsNull()));
    unlinkedEvalCodeBlockStructure.setWithoutWriteBarrier(UnlinkedEvalCodeBlock::createStructure(*this, nullptr, jsNull()));
    unlinkedFunctionCodeBlockStructure.setWithoutWriteBarrier(UnlinkedFunctionCodeBlock::createStructure(*this, nullptr, jsNull()));
    unlinkedModuleProgramCodeBlockStructure.setWithoutWriteBarrier(UnlinkedModuleProgramCodeBlock::createStructure(*this, nullptr, jsNull()));
    propertyTableStructure.setWithoutWriteBarrier(PropertyTable::createStructure(*this, nullptr, jsNull()));
    functionRareDataStructure.setWithoutWriteBarrier(FunctionRareData::createStructure(*this, nullptr, jsNull()));
    exceptionStructure.setWithoutWriteBarrier(Exception::createStructure(*this, nullptr, jsNull()));
    programCodeBlockStructure.setWithoutWriteBarrier(ProgramCodeBlock::createStructure(*this, nullptr, jsNull()));
    moduleProgramCodeBlockStructure.setWithoutWriteBarrier(ModuleProgramCodeBlock::createStructure(*this, nullptr, jsNull()));
    evalCodeBlockStructure.setWithoutWriteBarrier(EvalCodeBlock::createStructure(*this, nullptr, jsNull()));
    functionCodeBlockStructure.setWithoutWriteBarrier(FunctionCodeBlock::createStructure(*this, nullptr, jsNull()));
    bigIntStructure.setWithoutWriteBarrier(JSBigInt::createStructure(*this, nullptr, jsNull()));
    m_orderedHashTableDeletedValue.setWithoutWriteBarrier(JSOrderedHashMap::createDeletedValue(*this));
    m_orderedHashTableSentinel.setWithoutWriteBarrier(JSOrderedHashMap::createSentinel(*this));
    m_sortScratchSentinel.setWithoutWriteBarrier(JSCellButterfly::create(*this, CopyOnWriteArrayWithContiguous, 0));

    {
        Structure* sentinelStructure = JSSentinel::createStructure(*this, nullptr, jsNull());
        m_sentinelStructure.setWithoutWriteBarrier(sentinelStructure);
        m_fastArrayValuesSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastArrayKeysSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastArrayEntriesSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastMapKeysSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastMapValuesSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastMapEntriesSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastSetValuesSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastSetEntriesSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
        m_fastStringValuesSentinel.setWithoutWriteBarrier(JSSentinel::create(*this, sentinelStructure));
    }

    // Eagerly initialize constant cells since the concurrent compiler can access them.
    if (Options::useJIT()) {
        emptyPropertyNameEnumerator();
        ensureMegamorphicCache();
    }
    {
        auto* bigInt = JSBigInt::tryCreateFrom(*this, 1);
        if (bigInt)
            heapBigIntConstantOne.setWithoutWriteBarrier(bigInt);
        else {
            if (success)
                *success = false;
            else
                RELEASE_ASSERT_RESOURCE_AVAILABLE(bigInt, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        }
    }
    {
        auto* bigInt = JSBigInt::tryCreateWithLength(*this, 0);
        if (bigInt)
            heapBigIntConstantZero.setWithoutWriteBarrier(bigInt);
        else {
            if (success)
                *success = false;
            else
                RELEASE_ASSERT_RESOURCE_AVAILABLE(bigInt, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        }
    }

    Thread::currentSingleton().setCurrentAtomStringTable(existingEntryAtomStringTable);
    
    Gigacage::addPrimitiveDisableCallback(primitiveGigacageDisabledCallback, this);

    heap.notifyIsSafeToCollect();
    
    if (Options::useProfiler()) [[unlikely]] {
        m_perBytecodeProfiler = makeUnique<Profiler::Database>(*this);

        StringPrintStream pathOut;
        const char* profilerPath = getenv("JSC_PROFILER_PATH");
        if (profilerPath)
            pathOut.print(profilerPath, "/");
        else
            pathOut.print("/tmp/");
        pathOut.print("JSCProfile-", getCurrentProcessID(), "-", m_perBytecodeProfiler->databaseID(), ".json");
        static NeverDestroyed<CString> pathOutString = pathOut.toCString();

#if PLATFORM(COCOA)
        static std::once_flag registerFlag;
        std::call_once(registerFlag, [this]() {
            int pid = getpid();
            const char* key = "com.apple.WebKit.bytecode.profiler";
            dataLogLn("<BYTECODE.STAT><", pid, "> Registering callback for dumping profiles, dumping to ", pathOutString.get(), ".");
            dataLogLn("<BYTECODE.STAT><", pid, "> Use `notifyutil -v -p ", key, "` to dump statistics.");

            int token;
            notify_register_dispatch(key, &token, mainDispatchQueueSingleton(), ^(int) {
                dataLogLn("<BYTECODE.STAT><", pid, "> Dumping");
                if (!m_perBytecodeProfiler->save(pathOutString->data()))
                    dataLogLn("<BYTECODE.STAT><", pid, "> Failed to dump to ", pathOutString.get(), ". Do you need to add a sandbox extension? ((allow file-write* (subpath \"/private/tmp/\")) in WebProcess.sb.in");
                else
                    dataLogLn("<BYTECODE.STAT><", pid, "> Dumped to ", pathOutString.get());
                dataLogLn("<BYTECODE.STAT><", pid, "> Dumping finished");
            });
        });
#endif

        if (Options::dumpProfilerDataAtExit()) [[unlikely]]
            m_perBytecodeProfiler->registerToSaveAtExit(pathOutString->data());
    }

    // Initialize this last, as a free way of asserting that VM initialization itself
    // won't use this.
    m_typedArrayController = adoptRef(new SimpleTypedArrayController());

    m_bytecodeIntrinsicRegistry = makeUnique<BytecodeIntrinsicRegistry>(*this);

    if (Options::useTypeProfiler())
        enableTypeProfiler();
    if (Options::useControlFlowProfiler())
        enableControlFlowProfiler();
#if ENABLE(SAMPLING_PROFILER)
    if (Options::useSamplingProfiler()) {
        setShouldBuildPCToCodeOriginMapping();
        Ref<Stopwatch> stopwatch = Stopwatch::create();
        stopwatch->start();
        ensureSamplingProfiler(WTF::move(stopwatch));
        if (Options::samplingProfilerPath())
            m_samplingProfiler->registerForReportAtExit();
        m_samplingProfiler->start();
    }
#endif // ENABLE(SAMPLING_PROFILER)

    if (Options::useRandomizingFuzzerAgent())
        setFuzzerAgent(makeUnique<RandomizingFuzzerAgent>(*this));
    if (Options::useDoublePredictionFuzzerAgent())
        setFuzzerAgent(makeUnique<DoublePredictionFuzzerAgent>(*this));
    if (Options::useFileBasedFuzzerAgent())
        setFuzzerAgent(makeUnique<FileBasedFuzzerAgent>(*this));
    if (Options::usePredictionFileCreatingFuzzerAgent())
        setFuzzerAgent(makeUnique<PredictionFileCreatingFuzzerAgent>(*this));
    if (Options::useNarrowingNumberPredictionFuzzerAgent())
        setFuzzerAgent(makeUnique<NarrowingNumberPredictionFuzzerAgent>(*this));
    if (Options::useWideningNumberPredictionFuzzerAgent())
        setFuzzerAgent(makeUnique<WideningNumberPredictionFuzzerAgent>(*this));

    if (Options::alwaysGeneratePCToCodeOriginMap())
        setShouldBuildPCToCodeOriginMapping();

    if (Options::watchdog()) {
        Ref watchdog = ensureWatchdog();
        watchdog->setTimeLimit(Seconds::fromMilliseconds(Options::watchdog()));
    }

    if (Options::useTracePoints())
        requestEntryScopeService(EntryScopeService::TracePoints);

#if ENABLE(WEBASSEMBLY_DEBUGGER)
    if (Options::enableWasmDebugger()) [[unlikely]]
        m_debugState = makeUnique<Wasm::DebugState>();
#endif

#if ENABLE(JIT)
    // Make sure that any stubs that the JIT is going to use are initialized in non-compilation threads.
    if (Options::useJIT()) {
        jitStubs = makeUnique<JITThunks>();
        jitStubs->initialize(*this);
#if ENABLE(FTL_JIT)
        ftlThunks = makeUnique<FTL::Thunks>();
#endif // ENABLE(FTL_JIT)
        m_sharedJITStubs = makeUnique<SharedJITStubSet>();
        getBoundFunction(/* isJSFunction */ true, SourceTaintedOrigin::Untainted);
    }
#endif // ENABLE(JIT)

    if (Options::forceDebuggerBytecodeGeneration() || Options::alwaysUseShadowChicken())
        ensureShadowChicken();

#if ENABLE(JIT)
    if (Options::dumpBaselineJITSizeStatistics() || Options::dumpDFGJITSizeStatistics())
        jitSizeStatistics = makeUnique<JITSizeStatistics>();
#endif

    // SPEC-objectmodel §10 manifest entry 4b / M8 (GT#7): flag-on, the fenced
    // nuke/publication order must be the ONLY branch (see the flag-on block
    // below). The Options write must happen BEFORE Config::finalize()
    // write-protects the options storage. Skip the store when the option is
    // already set: Config::finalize() freezes the options page once per
    // process, so a second VM constructed afterwards must not write to it
    // (even a same-value store to a read-only page faults). The first flag-on
    // VM forces the option before the freeze, so later VMs always observe
    // true here and skip. THREADS-INTEGRATE(objectmodel)
    if (Options::useJSThreads() && !Options::forceFencedBarrier()) [[unlikely]]
        Options::forceFencedBarrier() = true;

    Config::finalize();

    // Intentionally do NOT eagerly resolve the host timezone / IANA timezone data
    // here. ucal_open() + the IANA timezone enumeration + ICU likely-subtags load
    // is one of the single largest contributors to interpreter startup CPU, and a
    // large fraction of short-lived processes never touch Date/Intl/toLocaleString.
    // DateCache::timeZoneCache() (via timeZoneCacheSlow()) and intlAvailableTimeZones()
    // are both guarded by their own one-time initialization, so the work is performed
    // lazily on first use instead. process.env.TZ is still honored eagerly because
    // WTF::setTimeZoneOverride() only records the timezone id string (cheap) and the
    // ICU calendar/likely-subtags resolution is deferred to first use anyway.

    if (Options::useVMLite()) [[unlikely]] {
        // SPEC-vmstate §6.4.4: main carrier (tid 0), created at the END of
        // the ctor. registerLite is the sole writer of VMLite::vm. The ctor
        // NEVER calls setCurrent — JSLock::didAcquireLock installs the
        // carrier at the outermost acquisition (M4).
        m_mainVMLite = makeUnique<VMLite>();
        // UNGIL §A.1.3 level-2 byte: copied from vm.m_gilOff AT lite
        // registration. Set BEFORE registerLite publishes the lite to
        // registry walkers. (A36: GIL-off entry never INSTALLS m_mainVMLite
        // — every thread, the main one included, uses a per-(thread,VM)
        // carrier from JSLock's TLS map — but the byte is stamped uniformly
        // at every registration site.)
        m_mainVMLite->gilOff = m_gilOff ? 1 : 0;
        VMLiteRegistry::singleton().registerLite(*m_mainVMLite, *this);
    }

    // SPEC-objectmodel §10 manifest entry 4b / M8 (GT#7): in-place butterfly
    // reallocs must stay disabled for the HEAP LIFETIME. Heap::endMarking
    // restores the fence from Options::forceFencedBarrier() (Heap.cpp), which
    // was forced above, before Config::finalize(). THREADS-INTEGRATE(objectmodel)
    if (Options::useJSThreads()) [[unlikely]]
        heap.setMutatorShouldBeFenced(true);

    // SPEC-objectmodel §9.2/I32 + Task 1 self-test (manifest entry 4a).
    if (Options::useJSThreads()) [[unlikely]] {
        alignas(16) static uint64_t sampleCell[2];
        RELEASE_ASSERT(concurrentButterflyAtomicsAreLockFree(&sampleCell));
        concurrentButterflySelfTestIfNeeded(); // runs iff Options::verifyConcurrentButterfly()
    }

    // SPEC-objectmodel §6 / §10 manifest entry 4c (Task 9): register the
    // per-server-heap butterfly-quarantine epoch bump. The adapter runs
    // world-stopped once per collection of THIS server heap (legacy AND
    // shared protocols, heap CR §13.10d) and bumps ONLY that heap's slot in
    // the owned ButterflyQuarantineEpochs registry — NEVER a process-global
    // counter (r13). Idempotent per heap; registration must precede client #2.
    if (Options::useJSThreads()) [[unlikely]]
        registerButterflyQuarantineEpochHook(heap);

    // We must set this at the end only after the VM is fully initialized.
    WTF::storeStoreFence();
    m_isInService = true;

    // Register after all VM state is initialized so that a stop-the-world triggered
    // immediately on registration sees a fully constructed VM.
    VMManager::singleton().notifyVMConstruction(*this);
}

static ReadWriteLock s_destructionLock;

void waitForVMDestruction()
{
    Locker locker { s_destructionLock.write() };
}

void VM::setCrossTaskToken(RefPtr<CrossTaskToken>&& token)
{
    m_crossTaskToken = WTF::move(token);
}

#if USE(BUN_JSC_ADDITIONS)
void VM::queueMicrotask(QueuedTask&& task)
{
    // UNGIL §E.1/I11 (U-T9): GIL-off, enqueue re-routes to the CURRENT
    // lite's queue on spawned/non-main-carrier threads — the VM default
    // queue is the MAIN carrier's (vmstate §6.6). MAIN-CARRIER KEY
    // (GIL-removal review round): GIL-off, m_mainVMLite is NEVER installed
    // (A36 — every thread gets a per-(thread,VM) carrier, the main thread
    // included), so `lite != m_mainVMLite.get()` alone was constantly true
    // and the default queue became an undrained sink. The gilOff main
    // carrier is the MAIN THREAD's carrier — exactly the
    // ownerHasNoTlsDtor==true lite (A36 r32, fixed at registration; it also
    // borrows &vm.clientHeap, F1B) — and it keeps the default queue, paired
    // with the same key in drainMicrotasks. Flag-off/GIL-on: the landed
    // single-queue enqueue, byte-identical.
    if (m_gilOff) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == this && lite != m_mainVMLite.get() && !lite->ownerHasNoTlsDtor) {
            lite->enqueueMicrotaskToDefaultQueue(WTF::move(task));
            return;
        }
        // UNGIL §E.1/§E.4 (TSAN family 30, microtask-queue): the AB-25
        // interim fail-stop (RELEASE_ASSERT(isMainThread())) is RETIRED for
        // the enqueue arm — this IS the AB-20/AB-23/AB-25 service-request
        // word for cross-thread enqueues. GIL-off the VM default queue is
        // OWNED by the main thread's carrier (the AB-23 re-key above); a
        // no-lite-window/foreign-VM-lite enqueue from any OTHER thread must
        // not touch the owner's plain Deque (that was the corruption-grade
        // unsynchronized write racing the carrier's drain). Instead it is
        // handed off through the queue's lock-guarded foreign inbox and
        // serviced at the owner's next drain: the inbox lock
        // release(enqueuer)/acquire(carrier drain splice) establishes the
        // happens-before that publishes the task's words before the carrier
        // dequeues/runs/frees them. The main thread (its own carrier, or
        // the pre-carrier no-lite window — ownerHasNoTlsDtor is fixed from
        // WTF::isMainThread() at registration, JSLock.cpp) is the owner and
        // keeps the landed plain enqueue below. Flag-off/GIL-on: branch not
        // taken, landed single-queue enqueue byte-identical.
        if (!WTF::isMainThread()) [[unlikely]] {
            m_defaultMicrotaskQueue->enqueueFromForeignThread(WTF::move(task));
            return;
        }
    }
    m_defaultMicrotaskQueue->enqueue(WTF::move(task));
}
#endif

// UNGIL U-T6 seam (defined in JSLock.cpp, same-library linkage — see the
// banner there): the calling thread's carrier lite for `vm`, or null.
VMLite* carrierLiteOfCurrentThreadIfExists(VM&);

// ============================================================================
// UNGIL ANNEX A36 (as AMENDED r32) + EXIT1.9 (U-T6): the ~VM foreign-carrier
// collection walk — step (2) of the §6.4.4 ~VM order. Runs only for the
// m_gilOff VM (flag-off/GIL-on ~VM is bit-identical to the landed shape).
//
// Under registry-lock holds, each of this VM's carrier lites (the
// ThreadManager carrier TID range — spawned lites are covered by the
// EXIT1.9 step-(3) wait instead) that is not marked TEARDOWN is
// token-free-asserted (proxy: its client holds no heap access; the
// destroying thread's OWN carrier is exempt — its token survives until the
// final m_lock drop, §F.2 IU row 21), marked COLLECTED — the lock-published
// discriminator the owner's TLS destructor keys on — and physically
// unregistered via the notifying wrapper. TEARDOWN lites are SKIPPED (owner
// mid-live-detach, still registered; the step-(3) wait covers them).
//
// The lock is then RELEASED and the walk performs the FULL server-side
// detach of each COLLECTED client lock-free (it acquires MSPL and can park
// in the access bracket — holding the registry lock across it is ILLEGAL,
// LK.6/I7). RECORDED REFINEMENT (this implementation vs the A36 letter):
// the detach is `delete client` — the live GCClient::Heap dtor body IS
// "everything in ~GCClient::Heap that names m_server" (access bracket,
// lastChanceToFinalize's MSPL relinquishment, clientSet().remove), and its
// member destruction (TLC tables, LocalAllocator unlinks — all structural
// no-ops after the relinquishment) also runs against the STILL-ALIVE
// server. A36 defers "client + lite destruction" to the owner's deferred
// dtor; deferring the CLIENT is impossible within U-T6's file ownership —
// ~GCThreadLocalCache (GCThreadLocalCache.cpp, unowned) unconditionally
// constructs a MutatorSlowPathLocker against m_server, so a post-VM-death
// client destruction would UAF the freed server no matter what the dtor
// body skips. Destroying the client INSIDE the walk (server alive; the
// owner is excluded — it is either pre-COLLECTED-wait or token-free and
// never touches the client again, and the degenerate dtor path never
// dereferences lite->clientHeap) preserves every invariant the deferral
// argued for: no double clientSet().remove, no concurrent MSPL section on
// the same client, no owner-side UAF; and the LITE (with its state byte) is
// still freed only by the party that observed DETACHED (the owner's dtor
// for bit-CLEAR; the walk itself, post-flip, for bit-SET) — the byte is
// never read after free. The main-thread carrier's client is BORROWED
// (&vm.clientHeap, F1B) and is never destroyed here — it dies with the VM
// as today.
//
// After each client's detach the walk re-acquires the registry lock, flips
// COLLECTED->DETACHED, notifyAll()s vmTeardownCondition, drops the lock
// (short hold, acquires nothing) and never touches that lite/client again —
// EXCEPT (r32) a bit-SET (ownerHasNoTlsDtor) lite, which the walk itself
// degenerately frees right after its flip: no destructor is ever installed
// over the main-thread slot on any platform, so no competing dtor exists BY
// CONSTRUCTION. A bit-CLEAR lite is NEVER walk-freed.
//
// RECORDED DEVIATION (EXIT1.9 step (2) "one registry-lock hold"): the
// COLLECTED mark and the physical unregistration run in SEPARATE holds
// because VMLiteRegistry::unregisterLite (VMLiteShared.cpp, outside U-T6's
// owned set) takes the non-recursive registry lock itself. Sound: a
// COLLECTED-but-still-registered lite counts EXITED to every conductor
// (EXIT1.4(a)) and the owner's dtor keys ONLY on the state byte, never on
// registration; the whole walk still strictly precedes the step-(3) wait,
// so the wait never counts a carrier.
// ============================================================================
// Returns the DESTROYING THREAD's own collected carrier when its client is
// OWNED (a non-main destroyer) — that client's detach is DEFERRED to
// detachDeferredOwnCarrierClientForVMDestruction, called later in ~VM right
// before heap.lastChanceToFinalize(): the destroying thread's client access
// must survive the access-requiring mid-~VM steps (Strong-clearing teardown
// such as the SD15 purge and DWT shutdown mutate the HandleSet, which
// requires an entered thread WITH access — GIL-on the destroyer holds
// access through all of ~VM for the same reason). Null for a main-thread
// destroyer (borrowed client, flipped in the walk) and when this thread has
// no carrier.
static VMLite* collectForeignCarriersForVMDestruction(VM& vm)
{
    ASSERT(vm.gilOff());
    auto& registry = VMLiteRegistry::singleton();
    VMLite* ownCarrier = carrierLiteOfCurrentThreadIfExists(vm);
    VMLite* deferredOwnCarrier = nullptr;
    Vector<VMLite*, 4> collected;
    {
        Locker locker { registry.lock };
        for (VMLite* lite : registry.lites) {
            if (lite->vm != &vm || lite == vm.mainVMLite())
                continue;
            if (!ThreadManager::isCarrierTID(lite->tid))
                continue; // spawned lite mid-T5: the EXIT1.9 step-(3) wait covers it
            if (lite->state == VMLite::State::Teardown)
                continue; // owner mid-live-detach: SKIPPED; step (3) covers it
            RELEASE_ASSERT(lite->state == VMLite::State::Live); // Collected/Detached lites were unregistered in the hold that marked them
            // Token-free assert (A36): an entered carrier holds client heap
            // access (F1B acquires on every lock()); an embedder thread
            // still entered at ~VM is a §F.6 contract violation. The
            // destroying thread's own carrier is exempt (IU row 21).
            if (lite != ownCarrier && lite->clientHeap)
                RELEASE_ASSERT(!lite->clientHeap->hasHeapAccess());
            lite->state = VMLite::State::Collected;
            collected.append(lite);
        }
    }
    for (VMLite* lite : collected)
        unregisterVMLiteAndNotifyTeardown(*lite); // U20 r31: EVERY physical removal is the notifying call
    for (VMLite* lite : collected) {
        // The destroying thread's own carrier with an OWNED client: detach
        // deferred (see above) — it stays COLLECTED until the deferred
        // detach flips it; its owner is THIS thread, so no TLS destructor
        // can race the deferral, and the EXIT1.9 wait does not count it
        // (already unregistered).
        if (lite == ownCarrier && lite->clientHeap && lite->clientHeap != &vm.clientHeap) {
            deferredOwnCarrier = lite;
            continue;
        }
        RaceAmplifier::perturb(); // EXIT1.8 CARRIER-TLS-DEATH-DURING-DETACH stall point: post-unregister, pre-detach.
        // Lock-free full server-side detach (see the banner). Borrowed
        // (main-thread) clients are the VM's own — skipped.
        GCClient::Heap* client = lite->clientHeap;
        if (client && client != &vm.clientHeap)
            delete client; // the live dtor against the still-alive server; may park in the access bracket
        RaceAmplifier::perturb(); // EXIT1.8 stall point: post-detach, pre-flip (incl. mid-lastChanceToFinalize via the dtor's own hooks).
        bool walkFrees = false;
        uint16_t tid = lite->tid;
        {
            Locker locker { registry.lock };
            RELEASE_ASSERT(lite->state == VMLite::State::Collected); // terminal-state machine: no other transition is legal
            lite->state = VMLite::State::Detached;
            walkFrees = lite->ownerHasNoTlsDtor; // r32: registration-time-fixed; read under the lock like the state byte
            vmLiteTeardownCondition().notifyAll(); // wake the owner's COLLECTED wait (short re-hold; acquires nothing)
        }
        if (walkFrees) {
            RaceAmplifier::perturb(); // EXIT1.8 r32 WALK-FREE stall point: between the flip and the degenerate free.
            // r32: the walk runs the degenerate free for the bit-SET lite —
            // exactly once; no destructor ever visits it (destructor-free
            // main-thread map). The owner's TLS-map unique_ptr dangles —
            // never consulted (lock() compares the VM epoch BEFORE the
            // cached carrier; the stale-epoch eviction release()s it).
            delete lite;
            releaseCarrierTIDIfHooked(tid);
        }
        // bit-CLEAR: NEVER walk-freed — the owner's TLS destructor (or the
        // stale-epoch eviction on re-entry) runs the degenerate free after
        // observing DETACHED and retires the TID there.
    }
    return deferredOwnCarrier;
}

// The deferred half of the own-carrier disposition (see
// collectForeignCarriersForVMDestruction): runs on the destroying thread
// right before heap.lastChanceToFinalize() — the last point at which the
// server is fully alive and the latest the destroyer may keep client
// access. Deletes the owned client (releasing this thread's access inside
// the dtor) and flips the lite COLLECTED->DETACHED so this thread's own
// eventual TLS destructor (or a stale-epoch eviction) takes the degenerate
// path. Never called for a main-thread destroyer (borrowed client).
static void detachDeferredOwnCarrierClientForVMDestruction(VM& vm, VMLite* lite)
{
    if (!lite)
        return;
    GCClient::Heap* client = lite->clientHeap;
    ASSERT(client);
    ASSERT_UNUSED(vm, client != &vm.clientHeap);
    delete client; // live dtor against the still-alive server; releases this thread's access
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    RELEASE_ASSERT(lite->state == VMLite::State::Collected);
    lite->state = VMLite::State::Detached;
    ASSERT(!lite->ownerHasNoTlsDtor); // own-deferred arm exists only for non-main destroyers
    vmLiteTeardownCondition().notifyAll(); // protocol symmetry; no waiter can exist for this lite (its owner is this thread)
}

// EXIT1.9 step (3): THE NORMATIVE COMPLETION FENCE for the spawned T5
// server-touching tail (and for TEARDOWN carriers the walk skipped). ~VM
// blocks here, under the registry lock, until no registered lite other than
// m_mainVMLite has lite->vm == this; unregisterVMLiteAndNotifyTeardown and
// the walk's DETACHED flips signal the condition (both waiters are
// predicate loops — cross-wakeups benign). Progress: every counted lite's
// owner is in straight-line teardown that runs access-released holding NO
// api or heap lock and acquires only the leaf registry lock — which this
// waiter does NOT own while parked (Condition::wait drops it into the
// parking lot) — so it always reaches its unregisterLite and signals
// (EXIT1.6 acyclicity; join-then-destroy-VM is safe in every build
// configuration without any new embedder contract).
static void waitForForeignLiteTeardownAtVMDestruction(VM& vm) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    ASSERT(vm.gilOff());
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    auto anyForeignLiteRemains = [&]() -> bool {
        for (VMLite* lite : registry.lites) {
            if (lite->vm == &vm && lite != vm.mainVMLite())
                return true;
        }
        return false;
    };
    while (anyForeignLiteRemains())
        vmLiteTeardownCondition().wait(registry.lock);
}

VM::~VM()
{
    // SPEC-vmstate §6.4.4/I20, at the TOP of ~VM — as AMENDED by UNGIL annex
    // EXIT1.9 (U-T6) for the m_gilOff VM: (1) uninstall the main carrier
    // from this thread's TLS via JSLock — must run while JSLock::m_vm is
    // still valid, i.e. BEFORE m_apiLock->willDestroyVM(this) below; (2) the
    // A36 foreign-carrier collection walk (gilOff only; ALL of it precedes
    // the wait, so the wait never counts a carrier whose deferred TLS
    // destructor runs at an unbounded future time); (3) the EXIT1.9 BLOCKING
    // wait until no registered lite other than m_mainVMLite points at this
    // VM — the NORMATIVE completion fence, release AND debug builds (the
    // pre-existing assert walk is DEMOTED to a post-wait debug sanity
    // check); (4) only then unregister m_mainVMLite and run the rest of ~VM
    // (notifyVMDestruction, lastChanceToFinalize, the M11 force-removal,
    // member teardown). GIL-on/flag-off (m_gilOff == 0): the landed
    // assert-only shape, bit-identical. Result: no thread's TLS dangles
    // across lastChanceToFinalize, and the T5 server-touching tail of any
    // just-joined spawned thread completes before the server Heap dies.
    VMLite* deferredOwnCarrierLite = nullptr;
    if (m_mainVMLite) {
        // UNGIL §F.2 (U-T8): GIL-off this assert is the TOKEN meaning — the
        // destroying thread's entry token deliberately SURVIVES the carrier
        // uninstall below and every teardown step that asserts the predicate
        // (DWT stopRunningTasks, traps().willDestroyVM, the :801 assert); it
        // is retired only when m_lock actually drops, by willReleaseLock's
        // lock-keyed retirement (JSLock.cpp, retireEntryTokenForLock).
        ASSERT(currentThreadIsHoldingAPILock());
        m_apiLock->uninstallVMLiteForVMDestruction(); // step (1)
        ASSERT(VMLite::currentIfExists() != m_mainVMLite.get());
        if (m_gilOff) [[unlikely]] {
            deferredOwnCarrierLite = collectForeignCarriersForVMDestruction(*this); // step (2)
            waitForForeignLiteTeardownAtVMDestruction(*this); // step (3): the fence
#if ASSERT_ENABLED
            {
                // Post-wait debug sanity walk (the demoted §6.5.1 assert):
                // only m_mainVMLite remains for this VM.
                Locker locker { VMLiteRegistry::singleton().lock };
                for (VMLite* lite : VMLiteRegistry::singleton().lites)
                    ASSERT(lite->vm != this || lite == m_mainVMLite.get());
            }
#endif
            // step (4): m_mainVMLite leaves through the notifying call too
            // (U20 r31: EVERY physical removal). No TEARDOWN mark is needed
            // — a VM inside ~VM has no live conductors (§F.6).
            unregisterVMLiteAndNotifyTeardown(*m_mainVMLite);
        } else {
#if ASSERT_ENABLED
            {
                Locker locker { VMLiteRegistry::singleton().lock };
                for (VMLite* lite : VMLiteRegistry::singleton().lites)
                    ASSERT(lite->vm != this || lite == m_mainVMLite.get());
            }
#endif
            VMLiteRegistry::singleton().unregisterLite(*m_mainVMLite);
        }
        m_mainVMLite = nullptr;
    }

    // Remove from VMManager before marking as no longer in service or cancelling traps,
    // so requestStopAllInternal() never iterates a VM with m_isShuttingDown set.
    VMManager::singleton().notifyVMDestruction(*this);

    Locker destructionLocker { s_destructionLock.read() };

    if (vmType == VMType::Default)
        WaiterListManager::singleton().unregister(this);

    Gigacage::removePrimitiveDisableCallback(primitiveGigacageDisabledCallback, this);
    deferredWorkTimer->stopRunningTasks();
#if ENABLE(WEBASSEMBLY)
    if (Wasm::Worklist* worklist = Wasm::existingWorklistOrNull())
        worklist->stopAllPlansForContext(*this);
#endif
    if (RefPtr watchdog = this->watchdog(); watchdog) [[unlikely]]
        watchdog->willDestroyVM(this);
    traps().willDestroyVM();
    m_isInService = false;
    WTF::storeStoreFence();

    if (m_hasSideData)
        sideDataRepository().deleteAll(this);

    // Never GC, ever again.
    heap.incrementDeferralDepth();

#if ENABLE(SAMPLING_PROFILER)
    if (m_samplingProfiler) {
        m_samplingProfiler->reportDataToOptionFile();
        m_samplingProfiler->shutdown();
    }
#endif // ENABLE(SAMPLING_PROFILER)
    
#if ENABLE(JIT)
    if (JITWorklist* worklist = JITWorklist::existingGlobalWorklistOrNull())
        worklist->cancelAllPlansForVM(*this);
#endif // ENABLE(JIT)
    
    // Clear this first to ensure that nobody tries to remove themselves from it.
    m_perBytecodeProfiler = nullptr;

    ASSERT(currentThreadIsHoldingAPILock());
    // UNGIL §E.1b.4/SD15 (U-T8e): free any never-drained spawned tracker
    // records (and their Strongs) BEFORE lastChanceToFinalize, on the
    // destroying thread, which still holds the token here (the §F.2
    // lock-keyed retirement above keeps it live through teardown). Entries
    // exist only for gilOff VMs; flag-off this is a no-op gate.
    if (m_gilOff) [[unlikely]] {
        purgePromiseRejectionHandoffRecordsAtVMDestruction(*this);
        // UNGIL §E.7.3 (U-T9): same lifetime point for the cross-thread DWT
        // handoff queue + internal-arm marks (queued-but-never-drained work
        // is dropped — the declared SD15-class exit-before-drain leak,
        // bounded by VM lifetime).
        jsThreadsPurgeCrossThreadDeferredWorkAtVMDestruction(*this);
    }
    m_apiLock->willDestroyVM(this);
    smallStrings.setIsInitialized(false);
    // UNGIL A36/U-T6: the deferred own-carrier client detach (non-main
    // destroyer only) — the last point with a fully-alive server; its
    // allocator relinquishment must precede the server-side
    // stopAllocatingForGood inside heap.lastChanceToFinalize() below.
    if (deferredOwnCarrierLite) [[unlikely]]
        detachDeferredOwnCarrierClientForVMDestruction(*this, deferredOwnCarrierLite);
    heap.lastChanceToFinalize();

    if (Options::useVMLite()) [[unlikely]] {
        // SPEC-vmstate §6.5(c): the same leaf lock that guards GC-marker
        // iteration (M11) and queue ctor/dtor list mutation (M12).
        Locker locker { VMLiteRegistry::singleton().lock };
        while (!m_microtaskQueues.isEmpty())
            m_microtaskQueues.begin()->remove();
    } else {
        while (!m_microtaskQueues.isEmpty())
            m_microtaskQueues.begin()->remove();
    }

    JSRunLoopTimer::Manager::singleton().unregisterVM(*this);

    delete emptyList;

    delete propertyNames;
    if (vmType != VMType::Default)
        delete m_atomStringTable;

    delete clientData;
    m_regExpCache.reset();

#if ENABLE(DFG_JIT)
    for (unsigned i = 0; i < m_scratchBuffers.size(); ++i)
        VMMalloc::free(m_scratchBuffers[i]);
#endif

#if ENABLE(JIT)
    m_sharedJITStubs = nullptr;
#endif

#if ENABLE(WEBASSEMBLY_DEBUGGER)
    if (Options::enableWasmDebugger()) [[unlikely]] {
        auto& debugServer = Wasm::DebugServer::singleton();
        if (debugServer.hasDebugger())
            debugServer.execution().notifyVMDestruction(this);
    }
#endif
}

void VM::primitiveGigacageDisabledCallback(void* argument)
{
    static_cast<VM*>(argument)->primitiveGigacageDisabled();
}

void VM::primitiveGigacageDisabled()
{
    // UNGIL §F.2 ANNEX F2 fixed ruling (U-T8): this site KEEPS the MUTEX
    // predicate (not the token redefinition) + the §A.1.5 deferred arm — the
    // gigacage-disable service is VM-WIDE, so a GIL-off token holder that is
    // not the m_lock owner routes through requestEntryScopeService, whose
    // VM-wide classification fans the bit to every registered lite of this
    // VM under the registry lock (CONCURRENT_SAFE: the disabling thread may
    // hold NO lite at all).
    if (m_apiLock->currentThreadIsHoldingLock()) {
        m_primitiveGigacageEnabled.fireAll(*this, "Primitive gigacage disabled");
        return;
    }
 
    // This is totally racy, and that's OK. The point is, it's up to the user to ensure that they pass the
    // uncaged buffer in a nicely synchronized manner.
    requestEntryScopeService(EntryScopeService::FirePrimitiveGigacageEnabled);
}

void VM::setLastStackTop(const Thread& thread)
{
    // UNGIL §A.1.3/§A.1.4 mode split: per-lite when gilOff.
    void*& lastStackTop = group3Primitives().m_lastStackTop;
    lastStackTop = thread.savedLastStackTop();
    auto& stack = thread.stack();
    RELEASE_ASSERT(stack.contains(lastStackTop), 0x5510, lastStackTop, stack.origin(), stack.end());
}

Ref<VM> VM::createContextGroup(HeapType heapType)
{
    return adoptRef(*new VM(VMType::APIContextGroup, heapType));
}

Ref<VM> VM::create(HeapType heapType, WTF::RunLoop* runLoop)
{
    return adoptRef(*new VM(VMType::Default, heapType, runLoop));
}

RefPtr<VM> VM::tryCreate(HeapType heapType, WTF::RunLoop* runLoop)
{
    bool success = true;
    RefPtr<VM> vm = adoptRef(new VM(VMType::Default, heapType, runLoop, &success));
    if (!success) {
        // Here, we're destructing a partially constructed VM and we know that
        // no one else can be using it at the same time. So, acquiring the lock
        // is superflous. However, we don't want to change how VMs are destructed.
        // Just going through the motion of acquiring the lock here allows us to
        // use the standard destruction process.

        // VM expects us to be holding the VM lock when destructing it. Acquiring
        // the lock also puts the VM in a state (e.g. acquiring heap access) that
        // is needed for destruction. The lock will hold the last reference to
        // the VM after we nullify the refPtr below. The VM will actually be
        // destructed in JSLockHolder's destructor.
        JSLockHolder lock(vm.get());
        vm = nullptr;
    }
    return vm;
}

#if ENABLE(SAMPLING_PROFILER)
SamplingProfiler& VM::ensureSamplingProfiler(Ref<Stopwatch>&& stopwatch)
{
    if (!m_samplingProfiler) {
        lazyInitialize(m_samplingProfiler, adoptRef(*new SamplingProfiler(*this, WTF::move(stopwatch))));
        requestEntryScopeService(EntryScopeService::SamplingProfiler);
    }
    return *m_samplingProfiler;
}

void VM::enableSamplingProfiler()
{
    RefPtr profiler = samplingProfiler();
    if (!profiler)
        profiler = &ensureSamplingProfiler(Stopwatch::create());
    profiler->start();
}

void VM::disableSamplingProfiler()
{
    RefPtr profiler = samplingProfiler();
    if (!profiler)
        profiler = &ensureSamplingProfiler(Stopwatch::create());
    {
        Locker locker { profiler->getLock() };
        profiler->pause();
    }
}

RefPtr<JSON::Value> VM::takeSamplingProfilerSamplesAsJSON()
{
    RefPtr profiler = samplingProfiler();
    return profiler ? RefPtr { profiler->stackTracesAsJSON() } : nullptr;
}

#endif // ENABLE(SAMPLING_PROFILER)

static StringImpl::StaticStringImpl terminationErrorString { "JavaScript execution terminated." };
Exception* VM::ensureTerminationException()
{
    // THREADS: serialize lazy creation — isTerminationException compares pointer
    // identity, so exactly one Exception may ever be published VM-wide.
    Exception* exception = WTF::atomicLoad(&m_terminationException, std::memory_order_relaxed);
    if (!exception) {
        Locker locker { m_terminationExceptionLock };
        exception = WTF::atomicLoad(&m_terminationException, std::memory_order_relaxed);
        if (!exception) {
            JSString* terminationError = jsNontrivialString(*this, terminationErrorString);
            exception = Exception::create(*this, terminationError, Exception::StackCaptureAction::DoNotCaptureStack);
            WTF::atomicStore(&m_terminationException, exception, std::memory_order_release);
        }
    }
    return exception;
}

#if ENABLE(JIT)
static ThunkGenerator NODELETE thunkGeneratorForIntrinsic(Intrinsic intrinsic)
{
    switch (intrinsic) {
    case CharCodeAtIntrinsic:
        return charCodeAtThunkGenerator;
    case CharAtIntrinsic:
        return charAtThunkGenerator;
    case StringPrototypeAtIntrinsic:
        return stringAtThunkGenerator;
    case StringPrototypeCodePointAtIntrinsic:
        return stringPrototypeCodePointAtThunkGenerator;
    case Clz32Intrinsic:
        return clz32ThunkGenerator;
    case FromCharCodeIntrinsic:
        return fromCharCodeThunkGenerator;
    case FromCodePointIntrinsic:
        return fromCodePointThunkGenerator;
    case GlobalIsNaNIntrinsic:
        return globalIsNaNThunkGenerator;
    case NumberIsNaNIntrinsic:
        return numberIsNaNThunkGenerator;
    case GlobalIsFiniteIntrinsic:
        return globalIsFiniteThunkGenerator;
    case NumberIsFiniteIntrinsic:
        return numberIsFiniteThunkGenerator;
    case NumberIsSafeIntegerIntrinsic:
        return numberIsSafeIntegerThunkGenerator;
    case SqrtIntrinsic:
        return sqrtThunkGenerator;
    case AbsIntrinsic:
        return absThunkGenerator;
    case FloorIntrinsic:
        return floorThunkGenerator;
    case CeilIntrinsic:
        return ceilThunkGenerator;
    case TruncIntrinsic:
        return truncThunkGenerator;
    case RoundIntrinsic:
        return roundThunkGenerator;
    case ExpIntrinsic:
        return expThunkGenerator;
    case LogIntrinsic:
        return logThunkGenerator;
    case IMulIntrinsic:
        return imulThunkGenerator;
    case RandomIntrinsic:
        return randomThunkGenerator;
#if USE(JSVALUE64)
    case ObjectIsIntrinsic:
        return objectIsThunkGenerator;
#endif
    case BoundFunctionCallIntrinsic:
        return boundFunctionCallGenerator;
    case RemoteFunctionCallIntrinsic:
        return remoteFunctionCallGenerator;
    case NumberConstructorIntrinsic:
        return numberConstructorCallThunkGenerator;
    case StringConstructorIntrinsic:
        return stringConstructorCallThunkGenerator;
    case ToIntegerOrInfinityIntrinsic:
        return toIntegerOrInfinityThunkGenerator;
    case ToLengthIntrinsic:
        return toLengthThunkGenerator;
    case WasmFunctionIntrinsic:
#if ENABLE(WEBASSEMBLY) && ENABLE(JIT)
        // UNGIL SD7/§I item (2) interim (AB-15): no specialized wasm call
        // thunk under useJSThreads — calls fall back to the generic native
        // call path, which lands in callWebAssemblyFunction where the SD7
        // spawned-thread refusal trips (wasm stack-limit reads are
        // carrier-published only post-AB-17). Flag-off cost: zero.
        if (Options::useJSThreads()) [[unlikely]]
            return nullptr;
        return Wasm::wasmFunctionThunkGenerator;
#else
        return nullptr;
#endif
    default:
        return nullptr;
    }
}

MacroAssemblerCodeRef<JITThunkPtrTag> VM::getCTIStub(ThunkGenerator generator)
{
    return jitStubs->ctiStub(*this, generator);
}

MacroAssemblerCodeRef<JITThunkPtrTag> VM::getCTIStub(CommonJITThunkID thunkID)
{
    return jitStubs->ctiStub(thunkID);
}

#endif // ENABLE(JIT)

NativeExecutable* VM::getHostFunction(NativeFunction function, ImplementationVisibility implementationVisibility, NativeFunction constructor, const String& name)
{
    return getHostFunction(function, implementationVisibility, NoIntrinsic, constructor, nullptr, name);
}

static Ref<NativeJITCode> jitCodeForCallTrampoline(Intrinsic intrinsic)
{
    switch (intrinsic) {
#if ENABLE(WEBASSEMBLY)
    case WasmFunctionIntrinsic: {
        // UNGIL SD7/§I item (2) interim (AB-15): under useJSThreads, do NOT
        // install the direct LLInt js_to_wasm_wrapper_entry trampoline —
        // it enters wasm without ever reaching C++, so a spawned Thread
        // calling a carrier-created export would run wasm against the
        // carrier-published (VM-level) stack limits. Fall through to the
        // generic native call trampoline instead, which invokes the
        // executable's NativeFunction (callWebAssemblyFunction), where the
        // SD7 spawned-thread refusal trips deterministically. Carrier calls
        // still work (one extra host-call hop). Flag-off cost: zero.
        if (Options::useJSThreads()) [[unlikely]]
            goto genericTrampoline;
        {
            static LazyNeverDestroyed<Ref<NativeJITCode>> result;
            static std::once_flag onceKey;
            std::call_once(onceKey, [&] {
                result.construct(adoptRef(*new NativeJITCode(LLInt::getCodeRef<JSEntryPtrTag>(js_to_wasm_wrapper_entry), JITType::HostCallThunk, intrinsic)));
            });
            return result.get();
        }
    }
    genericTrampoline:
#endif
    default: {
        static LazyNeverDestroyed<Ref<NativeJITCode>> result;
        static std::once_flag onceKey;
        std::call_once(onceKey, [&] {
            result.construct(adoptRef(*new NativeJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_native_call_trampoline), JITType::HostCallThunk, NoIntrinsic)));
        });
        return result.get();
    }
    }
}

static Ref<NativeJITCode> jitCodeForConstructTrampoline()
{
    static LazyNeverDestroyed<Ref<NativeJITCode>> result;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        result.construct(adoptRef(*new NativeJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_native_construct_trampoline), JITType::HostCallThunk, NoIntrinsic)));
    });
    return result.get();
}

NativeExecutable* VM::getHostFunction(NativeFunction function, ImplementationVisibility implementationVisibility, Intrinsic intrinsic, NativeFunction constructor, const DOMJIT::Signature* signature, const String& name)
{
#if ENABLE(JIT)
    if (Options::useJIT()) {
        return jitStubs->hostFunctionStub(
            *this, toTagged(function), toTagged(constructor),
            intrinsic != NoIntrinsic ? thunkGeneratorForIntrinsic(intrinsic) : nullptr,
            implementationVisibility, intrinsic, signature, name);
    }
#endif // ENABLE(JIT)
    UNUSED_PARAM(intrinsic);
    UNUSED_PARAM(signature);
    return NativeExecutable::create(*this, jitCodeForCallTrampoline(intrinsic), toTagged(function), jitCodeForConstructTrampoline(), toTagged(constructor), implementationVisibility, name);
}

NativeExecutable* VM::getBoundFunction(bool isJSFunction, SourceTaintedOrigin taintedness)
{
    bool slowCase = !isJSFunction;

    auto getOrCreate = [&](WriteBarrier<NativeExecutable>& slot) -> NativeExecutable* {
        if (taintedness < SourceTaintedOrigin::IndirectlyTainted) {
            if (auto* cached = slot.get())
                return cached;
        }
        NativeExecutable* result = getHostFunction(
            slowCase ? boundFunctionCall : boundThisNoArgsFunctionCall,
            ImplementationVisibility::Private, // Bound function's visibility is private on the stack.
            slowCase ? NoIntrinsic : BoundFunctionCallIntrinsic,
            boundFunctionConstruct, nullptr, String());
        slot.setWithoutWriteBarrier(result);
        return result;
    };

    if (slowCase)
        return getOrCreate(m_slowCanConstructBoundExecutable);
    return getOrCreate(m_fastCanConstructBoundExecutable);
}

NativeExecutable* VM::getRemoteFunction(bool isJSFunction)
{
    bool slowCase = !isJSFunction;
    auto getOrCreate = [&] (Weak<NativeExecutable>& slot) -> NativeExecutable* {
        if (auto* cached = slot.get())
            return cached;

        Intrinsic intrinsic = NoIntrinsic;
        if (!slowCase)
            intrinsic = RemoteFunctionCallIntrinsic;

        NativeExecutable* result = getHostFunction(
            slowCase ? remoteFunctionCallGeneric : remoteFunctionCallForJSFunction,
            ImplementationVisibility::Public, intrinsic,
            callHostFunctionAsConstructor, nullptr, String());
        slot = Weak<NativeExecutable>(result);
        return result;
    };

    if (slowCase)
        return getOrCreate(m_slowRemoteFunctionExecutable);
    return getOrCreate(m_fastRemoteFunctionExecutable);
}

CodePtr<JSEntryPtrTag> VM::getCTIInternalFunctionTrampolineFor(CodeSpecializationKind kind)
{
#if ENABLE(JIT)
    if (Options::useJIT()) {
        if (kind == CodeSpecializationKind::CodeForCall)
            return jitStubs->ctiInternalFunctionCall(*this).retagged<JSEntryPtrTag>();
        return jitStubs->ctiInternalFunctionConstruct(*this).retagged<JSEntryPtrTag>();
    }
#endif
    if (kind == CodeSpecializationKind::CodeForCall)
        return LLInt::getCodePtr<JSEntryPtrTag>(llint_internal_function_call_trampoline);
    return LLInt::getCodePtr<JSEntryPtrTag>(llint_internal_function_construct_trampoline);
}

MacroAssemblerCodeRef<JSEntryPtrTag> VM::getCTIThrowExceptionFromCallSlowPath()
{
#if ENABLE(JIT)
    if (Options::useJIT())
        return getCTIStub(CommonJITThunkID::ThrowExceptionFromCallSlowPath).template retagged<JSEntryPtrTag>();
#endif
    return LLInt::callToThrow(*this).template retagged<JSEntryPtrTag>();
}

MacroAssemblerCodeRef<JITStubRoutinePtrTag> VM::getCTIVirtualCall(CallMode callMode)
{
#if ENABLE(JIT)
    if (Options::useJIT()) {
        switch (callMode) {
        case CallMode::Regular:
            return getCTIStub(CommonJITThunkID::VirtualThunkForRegularCall).template retagged<JITStubRoutinePtrTag>();
        case CallMode::Tail:
            return getCTIStub(CommonJITThunkID::VirtualThunkForTailCall).template retagged<JITStubRoutinePtrTag>();
        case CallMode::Construct:
            return getCTIStub(CommonJITThunkID::VirtualThunkForConstruct).template retagged<JITStubRoutinePtrTag>();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
#endif
    switch (callMode) {
    case CallMode::Regular:
        return LLInt::getCodeRef<JITStubRoutinePtrTag>(llint_virtual_call_trampoline);
    case CallMode::Tail:
        return LLInt::getCodeRef<JITStubRoutinePtrTag>(llint_virtual_tail_call_trampoline);
    case CallMode::Construct:
        return LLInt::getCodeRef<JITStubRoutinePtrTag>(llint_virtual_construct_trampoline);
    }
    return LLInt::getCodeRef<JITStubRoutinePtrTag>(llint_virtual_call_trampoline);
}

void VM::whenIdle(Function<void()>&& callback)
{
    if (!entryScope) {
        callback();
        return;
    }
    m_didPopListeners.append(WTF::move(callback));
    requestEntryScopeService(EntryScopeService::PopListeners);
}

void VM::deleteAllLinkedCode(DeleteAllCodeEffort effort)
{
    whenIdle([=, this] () {
        heap.deleteAllCodeBlocks(effort);
    });
}

void VM::deleteAllCode(DeleteAllCodeEffort effort)
{
    whenIdle([=, this] () {
        m_codeCache->clear();
        m_builtinExecutables->clear();
        m_regExpCache->deleteAllCode();
        heap.deleteAllCodeBlocks(effort);
        heap.deleteAllUnlinkedCodeBlocks(effort);
        heap.reportAbandonedObjectGraph();

        if (MemoryPressureHandler::singleton().memoryPressureStatus() == SystemMemoryPressureStatus::Normal)
            return;
        // If we're deleting all code as a response to memory pressure, allow
        // worklist threads to temporarily stop, which frees any thread-local
        // data (specifically, any bulky heap-allocated data for the assembler
        // buffer).
#if ENABLE(JIT)
        if (auto worklist = JITWorklist::existingGlobalWorklistOrNull())
            worklist->requestTemporaryStop();
#if ENABLE(WEBASSEMBLY)
        if (auto worklist = Wasm::existingWorklistOrNull())
            worklist->requestTemporaryStop();
#endif // ENABLE(WEBASSEMBLY)
#endif // ENABLE(JIT)
    });
}

void VM::shrinkFootprintWhenIdle()
{
    whenIdle([=, this] () {
        sanitizeStackForVM(*this);
        deleteAllCode(DeleteAllCodeIfNotCollecting);
        heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
        // FIXME: Consider stopping various automatic threads here.
        // https://bugs.webkit.org/show_bug.cgi?id=185447
        WTF::releaseFastMallocFreeMemory();
    });
}

SourceProviderCache* VM::addSourceProviderCache(SourceProvider* sourceProvider)
{
    auto addResult = sourceProviderCacheMap.add(sourceProvider, nullptr);
    if (addResult.isNewEntry)
        addResult.iterator->value = adoptRef(new SourceProviderCache);
    return addResult.iterator->value.get();
}

void VM::clearSourceProviderCaches()
{
    sourceProviderCacheMap.clear();
}

bool VM::hasExceptionsAfterHandlingTraps()
{
    // UNGIL §A.2.2 item 3b (AB-17): GIL-off dispatch services the current
    // lite's instance too. GIL-on: byte-identical to the landed form.
    if (m_gilOff) [[unlikely]]
        handleTrapsForCurrentThreadIfNeeded(*this, VMTraps::NonDebuggerAsyncEvents);
    else if (traps().needHandling(VMTraps::NonDebuggerAsyncEvents)) [[unlikely]]
        traps().handleTraps(VMTraps::NonDebuggerAsyncEvents);
    return exception();
}

void VM::setHasTerminationRequest()
{
    m_hasTerminationRequest.store(true, std::memory_order_relaxed);
    requestEntryScopeService(ConcurrentEntryScopeService::ResetTerminationRequest);
}

void VM::setException(Exception* exception)
{
    ASSERT(!isTerminationException(exception) || hasTerminationRequest());
#if ASSERT_ENABLED
    // SPEC-vmstate I15: m_exception/m_lastException are written only by the
    // JSLock holder.
    if (Options::useVMLite())
        ASSERT(currentThreadIsHoldingAPILock());
#endif
    // UNGIL §A.1.3 mode split: the throwing thread's lite when gilOff (the
    // GC root walk picks these up per registered lite, r6 F5).
    // tsan-vm-setexception-cross-thread-r3: GIL'd useJSThreads shares this
    // word across threads, and hasPendingTerminationException() is a
    // sanctioned lock-free pointer-compare reader (jsc's runJSC result check
    // after its JSLockHolder scope closes while spawned threads still run).
    // Publish with a relaxed atomic store — codegen-identical to a plain
    // store on all supported targets, so GIL-on/flag-off perf is unchanged.
    // No ordering is required: the lock-free reader never dereferences, and
    // all dereferencing readers hold the JSLock (SPEC-vmstate I15).
    // m_lastException stays a plain store — it has no lock-free reader.
    auto& primitives = group3Primitives();
    WTF::atomicStore(&primitives.m_exception, exception, std::memory_order_relaxed);
    primitives.m_lastException = exception;
    if (exception)
        // Same storage domain as the per-lite m_exception store above
        // (trapsForCurrentThread(), VM.h): the throwing thread's own poll
        // sites see the bit; no cross-thread observe/clear GIL-off.
        trapsForCurrentThread().fireTrap(VMTraps::NeedExceptionHandling);
}

void VM::throwTerminationException()
{
    ASSERT(hasTerminationRequest());
    ASSERT(!trapsForCurrentThread().isDeferringTermination()); // Per-thread deferral keying (DeferTermination.h).
    // Termination can occur while executing DFG/FTL code that has set
    // doesGC expectations. Reset the expectation so that subsequent
    // heap access (e.g. JSLock re-acquisition) doesn't hit a stale
    // doesGC assertion.
    setDoesGCExpectation(true, DoesGCCheck::Special::Termination);
    setException(terminationException());
    if (m_executionForbiddenOnTermination)
        setExecutionForbidden();
}

Exception* VM::throwException(JSGlobalObject* globalObject, Exception* exceptionToThrow)
{
    // The TerminationException should never be overridden.
    if (hasPendingTerminationException())
        return group3Primitives().m_exception; // UNGIL §A.1.3 mode split.

    // The TerminationException is not like ordinary exceptions that should be
    // reported to the debugger. The fact that the TerminationException uses the
    // exception handling mechanism is just a VM internal implementation detail.
    // It is not meaningful to report it to the debugger as an exception.
    if (isTerminationException(exceptionToThrow)) {
        // Note: we can only get here is we're just re-throwing the TerminationException
        // from C++ functions to propagate it. If we're throwing it for the first
        // time, we would have gone through VM::throwTerminationException().
        setException(exceptionToThrow);
        return exceptionToThrow;
    }

    CallFrame* throwOriginFrame = topJSCallFrame();
    if (Options::breakOnThrow()) [[unlikely]] {
        CodeBlock* codeBlock = throwOriginFrame && !throwOriginFrame->isNativeCalleeFrame() ? throwOriginFrame->codeBlock() : nullptr;
        dataLog("Throwing exception in call frame ", RawPointer(throwOriginFrame), " for code block ", codeBlock, "\n");
        WTFBreakpointTrap();
    }

    interpreter.notifyDebuggerOfExceptionToBeThrown(*this, globalObject, throwOriginFrame, exceptionToThrow);

    setException(exceptionToThrow);

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10 mode split: capture into the THROWING thread's own
    // bookkeeping (per-lite GIL-off; VM copy GIL-on — bit-identical).
    auto& verificationState = exceptionScopeVerificationState();
    verificationState.m_nativeStackTraceOfLastThrow = StackTrace::captureStackTrace(Options::unexpectedExceptionStackTraceLimit());
    verificationState.m_throwingThread = &Thread::currentSingleton();
#endif
    return exceptionToThrow;
}

Exception* VM::throwException(JSGlobalObject* globalObject, JSValue thrownValue)
{
    Exception* exception = dynamicDowncast<Exception>(thrownValue);
    if (!exception)
        exception = Exception::create(*this, thrownValue);

    return throwException(globalObject, exception);
}

Exception* VM::throwException(JSGlobalObject* globalObject, JSObject* error)
{
    return throwException(globalObject, JSValue(error));
}

void VM::setStackPointerAtVMEntry(void* sp)
{
    group3Primitives().m_stackPointerAtVMEntry = sp; // UNGIL §A.1.4: per-entry-token lite field when gilOff.
    updateStackLimits();
}

size_t VM::updateSoftReservedZoneSize(size_t softReservedZoneSize)
{
    // THREADS: the read-modify-write is serialized under m_softReservedZoneSizeLock
    // (ErrorHandlingScope save/restore can run on multiple Threads); the field's
    // unlocked readers (updateStackLimits, softReservedZoneSize()) use relaxed loads.
    size_t oldSoftReservedZoneSize;
    {
        Locker locker { m_softReservedZoneSizeLock };
        oldSoftReservedZoneSize = WTF::atomicLoad(&m_currentSoftReservedZoneSize, std::memory_order_relaxed);
        WTF::atomicStore(&m_currentSoftReservedZoneSize, softReservedZoneSize, std::memory_order_relaxed);
    }
#if ENABLE(C_LOOP)
    cloopStack().setSoftReservedZoneSize(softReservedZoneSize);
#endif

    updateStackLimits();

    return oldSoftReservedZoneSize;
}

#if OS(WINDOWS)
// On Windows the reserved stack space consists of committed memory, a guard page, and uncommitted memory,
// where the guard page is a barrier between committed and uncommitted memory.
// When data from the guard page is read or written, the guard page is moved, and memory is committed.
// This is how the system grows the stack.
// When using the C stack on Windows we need to precommit the needed stack space.
// Otherwise we might crash later if we access uncommitted stack memory.
// This can happen if we allocate stack space larger than the page guard size (4K).
// The system does not get the chance to move the guard page, and commit more memory,
// and we crash if uncommitted memory is accessed.
// The MSVC compiler fixes this by inserting a call to the _chkstk() function,
// when needed, see http://support.microsoft.com/kb/100775.
// By touching every page up to the stack limit with a dummy operation,
// we force the system to move the guard page, and commit memory.

static void preCommitStackMemory(void* stackLimit)
{
    const int pageSize = 4096;
    for (volatile char* p = reinterpret_cast<char*>(&stackLimit); p > stackLimit; p -= pageSize) {
        char ch = *p;
        *p = ch;
    }
}
#endif

void VM::updateStackLimits()
{
    // UNGIL §A.2.2 (AB-17 item 3, LANDED): GIL-off, the soft limit generated
    // code checks is PER-THREAD — the read and the publish below target the
    // ENTERING thread's lite StackManager (its own StackBounds; no
    // cross-thread clobber). The entering lite must be THIS VM's (I14
    // mirror): an off-entry updateSoftReservedZoneSize-style call on a
    // thread carrying another VM's lite must not write VM-B bounds into
    // VM-A's per-thread word. GIL-on / no-lite keeps the VM-level word.
    VMTraps* liteTraps = nullptr;
    if (m_gilOff) [[unlikely]] {
        if (VMLite* lite = VMLite::currentIfExists(); lite && lite->gilOff && lite->vm == this)
            liteTraps = &lite->threadContext.traps();
    }
    void* lastSoftStackLimit = (liteTraps ? liteTraps : &traps())->softStackLimit();

    const StackBounds& stack = Thread::currentSingleton().stack();
    size_t reservedZoneSize = Options::reservedZoneSize();
    // We should have already ensured that Options::reservedZoneSize() >= minimumReserveZoneSize at
    // options initialization time, and the option value should not have been changed thereafter.
    // We don't have the ability to assert here that it hasn't changed, but we can at least assert
    // that the value is sane.
    RELEASE_ASSERT(reservedZoneSize >= minimumReservedZoneSize);

    // UNGIL §A.1.3/§A.2 mode split — §A.2.2 reroute LANDED (AB-17 item 3):
    // every generated-code soft-limit read (LLInt shared prologue +
    // doVMEntry/arity sites, Baseline/DFG/FTL/thunk/varargs/Yarr emission
    // sites) and every C++ VM::softStackLimit() reader now resolves through
    // the per-thread lite chain GIL-off (softStackLimitForCurrentThread /
    // the VMLite* chained offsets), and the VMTraps per-lite stop fan
    // (checklist item 3c) drives every lite's own trap-aware word. The
    // former N-entered tripwire walk is deleted with this change (the
    // VMEntryScope::setUpSlow gate retired per its own keying). The VM-level
    // word remains published by CARRIER entries only — carriers are
    // serialized under JSLock::m_lock, so there is no cross-thread clobber —
    // because it still serves (a) no-lite threads' C++ fallback reads,
    // (b) wasm instance StackManager mirrors (wasm execution is
    // carrier-only GIL-off, §I), and (c) the GIL-on/flag-off protocol
    // byte-identically. SPAWNED lites publish ONLY their own per-lite word:
    // a spawned entry must never clobber the VM word another carrier's
    // mirrors read.
    auto& primitives = group3Primitives();
    void* newSoftStackLimit = 0;
    if (primitives.m_stackPointerAtVMEntry) {
        char* startOfStack = reinterpret_cast<char*>(primitives.m_stackPointerAtVMEntry);
        newSoftStackLimit = stack.recursionLimit(startOfStack, Options::maxPerThreadStackUsage(), WTF::atomicLoad(&m_currentSoftReservedZoneSize, std::memory_order_relaxed));
        primitives.m_stackLimit = stack.recursionLimit(startOfStack, Options::maxPerThreadStackUsage(), reservedZoneSize);
    } else {
        newSoftStackLimit = stack.recursionLimit(WTF::atomicLoad(&m_currentSoftReservedZoneSize, std::memory_order_relaxed));
        primitives.m_stackLimit = stack.recursionLimit(reservedZoneSize);
    }

    // §A.2.2 per-thread publish: the entering thread's own lite word (what
    // the per-lite generated-code checks will read once the chained-offset
    // emission lands). Sequenced-before any JS on this thread; never written
    // cross-thread.
    if (liteTraps && lastSoftStackLimit != newSoftStackLimit) [[unlikely]] {
        liteTraps->setStackSoftLimit(newSoftStackLimit);
#if OS(WINDOWS)
        // The per-lite limit bounds LLInt-asm/JIT-generated stack usage on
        // this thread exactly like the VM-word limit does below (see that
        // comment for why precommit is required); spawned lites never reach
        // the VM-word block, so the precommit must happen on this publish
        // path too.
        preCommitStackMemory(newSoftStackLimit);
#endif
    }

    // Carrier-only VM-word publish (see the AB-17 comment above). GIL-on:
    // liteTraps is null and this is the landed single publish,
    // byte-identical.
    if (m_gilOff && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        // Spawned lite: per-lite word only (published above) — a spawned
        // thread must never write the VM word (carrier mirrors read it).
        // If this thread carries THIS VM's lite, the per-lite publish above
        // must have been available (liteTraps non-null); a silent
        // no-publish (neither word written) would leave a STALE per-lite
        // limit that generated code keeps checking — e.g. an off-entry
        // updateSoftReservedZoneSize republish computed under a new
        // reserved-zone size would be dropped with no assert. Fail-stop
        // deterministically instead. Tolerated no-publish windows: no lite
        // installed, or another VM's lite current (I14 mirror — this thread
        // is not running this VM's JS; its next entry republishes via
        // setStackPointerAtVMEntry).
        if (!liteTraps) [[unlikely]] {
            VMLite* lite = VMLite::currentIfExists();
            RELEASE_ASSERT(!lite || lite->vm != this);
        }
        return;
    }
    if (traps().softStackLimit() != newSoftStackLimit) {
        traps().setStackSoftLimit(newSoftStackLimit);
#if OS(WINDOWS)
        // We only need to precommit stack memory dictated by the VM::softStackLimit() limit.
        // This is because VM::softStackLimit() applies to stack usage by LLINT asm or JIT
        // generated code which can allocate stack space that the C++ compiler does not know
        // about. As such, we have to precommit that stack memory manually.
        //
        // In contrast, we do not need to worry about VM::m_stackLimit because that limit is
        // used exclusively by C++ code, and the C++ compiler will automatically commit the
        // needed stack pages.
        preCommitStackMemory(newSoftStackLimit);
#endif
    }
}

// UNGIL §A.2.2 (AB-17): gilOff arm of softStackLimitForCurrentThreadSlow
// (inline wrapper in VM.h) for readers that cannot include VMInlines.h
// (JSStringInlines, Yarr, JSON/LiteralParser). Same contract as the
// VMInlines.h helper: GIL-off with a matching current lite, the PLAIN
// per-lite soft limit (null falls back to the VM word — a never-published
// per-lite limit must not disable overflow detection); otherwise the VM
// word. The flag-off arm lives in the VM.h wrapper so those hot paths
// compile back to the single load (bench-gate finding).
void* VM::softStackLimitForCurrentThreadGilOffSlow() const
{
    ASSERT(m_gilOff);
    VMLite* lite = VMLite::currentIfExists();
    if (lite && lite->gilOff && lite->vm == this) {
        if (void* liteLimit = lite->threadContext.traps().softStackLimit())
            return liteLimit;
    }
    return softStackLimit();
}

#if ENABLE(DFG_JIT)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

void VM::gatherScratchBufferRoots(ConservativeRoots& conservativeRoots)
{
    {
        Locker locker { m_scratchBufferLock };
        for (auto* scratchBuffer : m_scratchBuffers) {
            if (scratchBuffer->activeLength()) {
                void* bufferStart = scratchBuffer->dataBuffer();
                conservativeRoots.add(bufferStart, static_cast<void*>(static_cast<char*>(bufferStart) + scratchBuffer->activeLength()));
            }
        }
    }

    // UNGIL §A.1.6 (annex A16 / jit R2): per-lite buffers — the non-baked
    // per-thread tables AND the baked registry-index installs (both live on
    // each lite's `scratchBuffers` ownership list) — are GC-scanned via the
    // registry walk, per-VM filtered (r6 F5's filter). Mutators are quiesced
    // by the heap §10 stop, so the registry is stable; taking each lite's
    // scratchBufferLock under the registry lock is the §LK.6 re-rank
    // (ScratchBufferRegistry -> VMLiteRegistry::lock -> scratchBufferLock).
    // This also closes VMLite's former Phase-A "not visited" caveat.
    if (Options::useVMLite()) [[unlikely]] {
        auto& registry = VMLiteRegistry::singleton();
        Locker registryLocker { registry.lock };
        for (VMLite* lite : registry.lites) {
            if (lite->vm != this)
                continue;
            Locker bufferLocker { lite->scratchBufferLock };
            for (auto* scratchBuffer : lite->scratchBuffers) {
                if (scratchBuffer->activeLength()) {
                    void* bufferStart = scratchBuffer->dataBuffer();
                    conservativeRoots.add(bufferStart, static_cast<void*>(static_cast<char*>(bufferStart) + scratchBuffer->activeLength()));
                }
            }
        }
    }
}

void VM::scanSideState(ConservativeRoots& roots) const
{
    ASSERT(heap.worldIsStopped());
    for (const auto& sideState : m_checkpointSideState) {
        static_assert(sizeof(sideState->tmps) / sizeof(JSValue) == maxNumCheckpointTmps);
        roots.add(sideState->tmps, sideState->tmps + maxNumCheckpointTmps);
    }
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif // ENABLE(DFG_JIT)

void VM::pushCheckpointOSRSideState(std::unique_ptr<CheckpointOSRExitSideState>&& payload)
{
    ASSERT(currentThreadIsHoldingAPILock());
    ASSERT(payload->associatedCallFrame);
#if ASSERT_ENABLED
    for (const auto& sideState : m_checkpointSideState)
        ASSERT(sideState->associatedCallFrame != payload->associatedCallFrame);
#endif
    m_checkpointSideState.append(WTF::move(payload));

#if ASSERT_ENABLED
    auto bounds = StackBounds::currentThreadStackBounds();
    void* previousCallFrame = bounds.end();
    for (size_t i = m_checkpointSideState.size(); i--;) {
        auto* callFrame = m_checkpointSideState[i]->associatedCallFrame;
        if (!bounds.contains(callFrame))
            break;
        ASSERT(previousCallFrame < callFrame);
        previousCallFrame = callFrame;
    }
#endif
}

std::unique_ptr<CheckpointOSRExitSideState> VM::popCheckpointOSRSideState(CallFrame* expectedCallFrame)
{
    ASSERT(currentThreadIsHoldingAPILock());
    auto sideState = m_checkpointSideState.takeLast();
    RELEASE_ASSERT(sideState->associatedCallFrame == expectedCallFrame);
    return sideState;
}

void VM::popAllCheckpointOSRSideStateUntil(CallFrame* target)
{
    ASSERT(currentThreadIsHoldingAPILock());
    auto bounds = StackBounds::currentThreadStackBounds().withSoftOrigin(target);
    ASSERT(bounds.contains(target));

    // We have to worry about migrating from another thread since there may be no checkpoints in our thread but one in the other threads.
    while (m_checkpointSideState.size() && bounds.contains(m_checkpointSideState.last()->associatedCallFrame))
        m_checkpointSideState.takeLast();
    m_checkpointSideState.shrinkToFit();
}

static void logSanitizeStack(VM& vm)
{
    if (Options::verboseSanitizeStack()) [[unlikely]] {
        auto& stackBounds = Thread::currentSingleton().stack();
        dataLogLn("Sanitizing stack for VM = ", RawPointer(&vm), ", current stack pointer at ", RawPointer(currentStackPointer()), ", last stack top = ", RawPointer(vm.lastStackTop()), ", in stack range (", RawPointer(stackBounds.end()), ", ", RawPointer(stackBounds.origin()), "]");
    }
}

#if ENABLE(REGEXP_TRACING)

void VM::addRegExpToTrace(RegExp* regExp)
{
    gcProtect(regExp);
    m_rtTraceList.add(regExp);
}

void VM::dumpRegExpTrace()
{
    if (m_rtTraceList.size() <= 1)
        return;

    // The first RegExp object is ignored. It is created by the RegExpPrototype ctor and not used.
    RTTraceList::iterator iter = ++m_rtTraceList.begin();
    
    if (iter != m_rtTraceList.end()) {
        RegExp::printTraceHeader();

        unsigned reCount = 0;
    
        for (; iter != m_rtTraceList.end(); ++iter, ++reCount) {
            (*iter)->printTraceData();
            gcUnprotect(*iter);
        }

        dataLogF("%d Regular Expressions\n", reCount);
    }
    
    m_rtTraceList.clear();
}

#endif

WatchpointSet* VM::ensureWatchpointSetForImpureProperty(UniquedStringImpl* propertyName)
{
    auto result = m_impurePropertyWatchpointSets.add(propertyName, nullptr);
    if (result.isNewEntry)
        result.iterator->value = WatchpointSet::create(IsWatched);
    return result.iterator->value.get();
}

void VM::addImpureProperty(UniquedStringImpl* propertyName)
{
    if (RefPtr<WatchpointSet> watchpointSet = m_impurePropertyWatchpointSets.take(propertyName))
        watchpointSet->fireAll(*this, "Impure property added");
}

template<typename Func>
static bool enableProfilerWithRespectToCount(unsigned& counter, const Func& doEnableWork)
{
    bool needsToRecompile = false;
    if (!counter) {
        doEnableWork();
        needsToRecompile = true;
    }
    counter++;

    return needsToRecompile;
}

template<typename Func>
static bool disableProfilerWithRespectToCount(unsigned& counter, const Func& doDisableWork)
{
    RELEASE_ASSERT(counter > 0);
    bool needsToRecompile = false;
    counter--;
    if (!counter) {
        doDisableWork();
        needsToRecompile = true;
    }

    return needsToRecompile;
}

bool VM::enableTypeProfiler()
{
    auto enableTypeProfiler = [this] () {
        this->m_typeProfiler = makeUnique<TypeProfiler>();
        this->m_typeProfilerLog = makeUnique<TypeProfilerLog>(*this);
    };

    return enableProfilerWithRespectToCount(m_typeProfilerEnabledCount, enableTypeProfiler);
}

bool VM::disableTypeProfiler()
{
    auto disableTypeProfiler = [this] () {
        this->m_typeProfiler.reset(nullptr);
        this->m_typeProfilerLog.reset(nullptr);
    };

    return disableProfilerWithRespectToCount(m_typeProfilerEnabledCount, disableTypeProfiler);
}

bool VM::enableControlFlowProfiler()
{
    auto enableControlFlowProfiler = [this] () {
        this->m_controlFlowProfiler = makeUnique<ControlFlowProfiler>();
    };

    return enableProfilerWithRespectToCount(m_controlFlowProfilerEnabledCount, enableControlFlowProfiler);
}

bool VM::disableControlFlowProfiler()
{
    auto disableControlFlowProfiler = [this] () {
        this->m_controlFlowProfiler.reset(nullptr);
    };

    return disableProfilerWithRespectToCount(m_controlFlowProfilerEnabledCount, disableControlFlowProfiler);
}

void VM::dumpTypeProfilerData()
{
    if (!typeProfiler())
        return;

    typeProfilerLog()->processLogEntries(*this, "VM Dump Types"_s);
    typeProfiler()->dumpTypeProfilerData(*this);
}

// =============================================================================
// UNGIL §E.1b.4 / SD15 (U-T8e): promise-rejection-tracker carrier handoff.
//
// GIL-off, the promiseRejectionTracker host hook is invoked INLINE only when
// the acting thread is a main/embedder carrier. Spawned-thread Reject/Handle
// events are appended (no JS, no allocation beyond the record) to a
// leaf-lock-guarded handoff queue as tracker records {promise Strong,
// operation}, flushed and EXECUTED at the §F.1 carrier drain points
// (VM::didExhaustMicrotaskQueue below — the same checkpoint that reports
// unhandled rejections) like off-carrier DWT work. Ordering vs carrier-side
// tracker events is unspecified (SD15): a report may arrive a drain late but
// is never lost while the carrier still drains; process-exit-before-drain
// drops are the same class as landed exit-before-microtask drains; a VM with
// no carrier ever draining leaks reports — declared. No hooks installed =>
// SAME routing (the queue is drain-owned, not hook-owned).
//
// Consequence for VM::m_aboutToBeNotifiedRejectedPromises (annex K4 table I
// row "per-lite / SD15"): the vector becomes CARRIER-CONFINED GIL-off — only
// carrier threads ever append to it (VM::promiseRejected gates below) or walk
// it (didExhaustMicrotaskQueue returns early on spawned threads), so it needs
// no lock and no per-lite copy; spawned events reach it via this queue.
//
// Strong discipline (§F.3): the ENQUEUER creates the record's Strong while
// holding its entry token (asserted); the CARRIER clears it under its token
// at flush; ~VM purges the queue on the destroying thread, which still holds
// the token at that point (see ~VM). Strong create/destroy happens OUTSIDE
// the queue lock so HandleSet::m_strongLock (also a leaf) is never nested
// under it.
//
// Lock placement note (recorded spec delta, U-T8e summary): §E.1b.4 names
// "the annex-E7 m_pendingLock-guarded handoff queue" — i.e. DWT::m_taskLock.
// DeferredWorkTimer.{h,cpp} are owned by U-T9 (§E.4/§E.7) and have no handoff
// queue yet, so this task lands the records under an equivalently-ranked §LK
// leaf lock here instead (same contract: append/removal/emptiness reads under
// the lock, never held across user JS, nothing taken under it). U-T9 may
// re-home the records onto the DWT queue verbatim; the enqueue/flush API
// below is the seam.
//
// No wake edge is required for SD15 (unlike annex-E7 DWT work): the spec only
// promises delivery at the NEXT carrier drain, which the §E.1b settling-
// thread microtask protocol already forces whenever a carrier drains.
//
// Call-site status (gates U-T9): the four installed-hook invocation sites
// (JSPromise.cpp:405/:464/:502/:637) still call the methodTable hook
// directly; U-T9 re-points them at notifyPromiseRejectionTrackerCrossThread-
// Aware() below (same-library linkage — redeclare in JSPromise.cpp, no
// header changes; the currentThreadHoldsEntryToken pattern, JSLock.cpp).
// Until then the DEFAULT tracker is already safe (its only effect,
// VM::promiseRejected, gates internally below); embedder-installed hooks at
// those sites run inline-on-spawned only until U-T9 lands the re-point.
// Flag-off (useJSThreads=false => m_gilOff false): every path below is
// bit-identical to the landed code.
// =============================================================================

namespace {

struct PromiseRejectionTrackerHandoffRecord {
    Strong<JSPromise> promise;
    JSPromiseRejectionOperation operation;
};

struct PromiseRejectionTrackerHandoffQueue {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PromiseRejectionTrackerHandoffQueue);
    Lock lock; // §LK leaf (spec home: DWT m_pendingLock; see block comment).
    Vector<PromiseRejectionTrackerHandoffRecord> records WTF_GUARDED_BY_LOCK(lock);
    bool flushing WTF_GUARDED_BY_LOCK(lock) { false }; // re-entrant carrier drain guard.
};

} // anonymous namespace

// Per-VM side table (VM.h is not in U-T8e's owned file set, so no VM member;
// U-T9 may fold this into DWT state). Entries are created only for gilOff VMs
// and removed in ~VM; the registry lock is a leaf taken only around the tiny
// vector scan — the per-queue lock is NOT nested under it.
static Lock s_promiseRejectionHandoffRegistryLock;

static Vector<std::pair<VM*, std::unique_ptr<PromiseRejectionTrackerHandoffQueue>>>& promiseRejectionHandoffRegistry() WTF_REQUIRES_LOCK(s_promiseRejectionHandoffRegistryLock)
{
    static NeverDestroyed<Vector<std::pair<VM*, std::unique_ptr<PromiseRejectionTrackerHandoffQueue>>>> registry;
    return registry.get();
}

static PromiseRejectionTrackerHandoffQueue* promiseRejectionHandoffQueueFor(VM& vm, bool createIfMissing)
{
    Locker locker { s_promiseRejectionHandoffRegistryLock };
    auto& registry = promiseRejectionHandoffRegistry();
    for (auto& entry : registry) {
        if (entry.first == &vm)
            return entry.second.get();
    }
    if (!createIfMissing)
        return nullptr;
    registry.append({ &vm, makeUnique<PromiseRejectionTrackerHandoffQueue>() });
    return registry.last().second.get();
}

// SD15 spawned-side append. No JS, no GC allocation beyond the Strong slot.
// Exported within the library for U-T9's call-site re-point (redeclare, no
// header).
void enqueuePromiseRejectionTrackerHandoffRecord(VM& vm, JSPromise* promise, JSPromiseRejectionOperation operation)
{
    ASSERT(vm.gilOff());
    ASSERT(ThreadManager::isJSThreadCurrent());
    ASSERT(vm.currentThreadIsHoldingAPILock()); // §F.3: enqueuer holds its entry token across the Strong create.
    // Strong created OUTSIDE the queue lock (leaf-vs-leaf, see block comment);
    // moving it into the vector touches no HandleSet state.
    PromiseRejectionTrackerHandoffRecord record { Strong<JSPromise>(vm, promise), operation };
    auto* queue = promiseRejectionHandoffQueueFor(vm, /* createIfMissing */ true);
    Locker locker { queue->lock };
    queue->records.append(WTF::move(record));
}

// SD15 carrier-side flush + EXECUTE, run at the §F.1 carrier drain point
// (didExhaustMicrotaskQueue). Invokes the CURRENT methodTable hook of each
// promise's realm global under the carrier's token — the default hook lands
// in VM::promiseRejected's carrier arm; installed (Bun-style) hooks run their
// JS here, on the carrier, per SD15. Records are executed with the promise's
// REALM global (the r33-class cross-realm settle rule: the spawned settle
// site's lexical global identity is not carried across the hop; recorded in
// the U-T8e summary).
void flushPromiseRejectionTrackerHandoffRecords(VM& vm)
{
    if (!vm.gilOff())
        return;
    ASSERT(!ThreadManager::isJSThreadCurrent()); // carrier drain points only.
    ASSERT(vm.currentThreadIsHoldingAPILock());
    auto* queue = promiseRejectionHandoffQueueFor(vm, /* createIfMissing */ false);
    if (!queue)
        return;

    Vector<PromiseRejectionTrackerHandoffRecord> records;
    {
        Locker locker { queue->lock };
        if (queue->flushing || queue->records.isEmpty())
            return;
        queue->flushing = true;
        records = std::exchange(queue->records, { });
    }

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    size_t executed = 0;
    bool terminated = false;
    for (auto& record : records) {
        JSPromise* promise = record.promise.get();
        // The promise's realm global is kept alive by the Strong (cell ->
        // structure -> global); the hook contract takes the global whose
        // tracker fires (same realm choice as callPromiseRejectionCallback).
        JSGlobalObject* globalObject = promise->realm();
        globalObject->globalObjectMethodTable()->promiseRejectionTracker(globalObject, promise, record.operation);
        ++executed;
        record.promise.clear(); // §F.3: carrier clears under its token.
        if (!scope.clearExceptionExceptTermination()) [[unlikely]] {
            terminated = true;
            break;
        }
    }

    {
        Locker locker { queue->lock };
        queue->flushing = false;
        if (terminated && executed < records.size()) {
            // "Never lost while the carrier still drains": re-prepend the
            // unexecuted tail ahead of anything enqueued during the flush.
            Vector<PromiseRejectionTrackerHandoffRecord> requeued;
            requeued.reserveInitialCapacity(records.size() - executed + queue->records.size());
            for (size_t i = executed; i < records.size(); ++i)
                requeued.append(WTF::move(records[i]));
            for (auto& pending : queue->records)
                requeued.append(WTF::move(pending));
            queue->records = WTF::move(requeued);
        }
    }
    // Executed records' (already-cleared) Strongs are destroyed here, outside
    // the queue lock, still under the carrier's token.
}

// SD15 invocation gate: the shape U-T9 re-points the JSPromise.cpp call sites
// at (redeclare in JSPromise.cpp; same-library linkage, no header). Carrier
// (or GIL-on / flag-off): invoke the methodTable hook inline, bit-identical
// to the landed call sites. Spawned GIL-off: append the tracker record.
void notifyPromiseRejectionTrackerCrossThreadAware(JSGlobalObject* globalObject, JSPromise* promise, JSPromiseRejectionOperation operation)
{
    VM& vm = globalObject->vm();
    if (vm.gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        enqueuePromiseRejectionTrackerHandoffRecord(vm, promise, operation);
        return;
    }
    globalObject->globalObjectMethodTable()->promiseRejectionTracker(globalObject, promise, operation);
}

// ~VM purge: drop this VM's registry entry and free the record Strongs on the
// destroying thread (which still holds the token there — asserted at the call
// site). Reports queued but never drained by a carrier are dropped — the
// SD15 declared leak class, now bounded by VM lifetime.
static void purgePromiseRejectionHandoffRecordsAtVMDestruction(VM& vm)
{
    std::unique_ptr<PromiseRejectionTrackerHandoffQueue> queue;
    {
        Locker locker { s_promiseRejectionHandoffRegistryLock };
        auto& registry = promiseRejectionHandoffRegistry();
        for (size_t i = 0; i < registry.size(); ++i) {
            if (registry[i].first == &vm) {
                queue = WTF::move(registry[i].second);
                registry.removeAt(i);
                break;
            }
        }
    }
    if (!queue)
        return;
    Vector<PromiseRejectionTrackerHandoffRecord> records;
    {
        Locker locker { queue->lock };
        ASSERT(!queue->flushing);
        records = std::exchange(queue->records, { });
    }
    records.clear(); // Strong dtors outside the queue lock, under the destroying thread's token.
}

void VM::callPromiseRejectionCallback(Strong<JSPromise>& promise)
{
    JSObject* callback = promise->realm()->unhandledRejectionCallback();
    if (!callback)
        return;

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*this);

    auto callData = JSC::getCallDataInline(callback);
    ASSERT(callData.type != CallData::Type::None);

    MarkedArgumentBuffer args;
    args.append(promise.get());
    args.append(promise->result());
    ASSERT(!args.hasOverflowed());
    call(promise->realm(), callback, callData, jsNull(), args);
    scope.clearException();
}

void VM::didExhaustMicrotaskQueue()
{
    // UNGIL §E.1b.4/SD15 (U-T8e): GIL-off this is the §F.1 carrier drain
    // point for spawned tracker records. Spawned threads NEVER execute
    // tracker events or unhandled-rejection reports and never touch the
    // carrier-confined vector below — their events were carrier-queued at
    // raise time (VM::promiseRejected / the SD15 invocation gate) and run at
    // the next carrier drain (a report may arrive a drain late; never lost
    // while the carrier drains). Flag-off/GIL-on: m_gilOff is false and this
    // block vanishes.
    if (m_gilOff) [[unlikely]] {
        if (ThreadManager::isJSThreadCurrent())
            return;
        flushPromiseRejectionTrackerHandoffRecords(*this);
        if (hasPendingTerminationException()) [[unlikely]]
            return;
    }
    while (!m_aboutToBeNotifiedRejectedPromises.isEmpty()) {
        auto unhandledRejections = WTF::move(m_aboutToBeNotifiedRejectedPromises);
        for (auto& promise : unhandledRejections) {
            if (promise->isHandled())
                continue;

            callPromiseRejectionCallback(promise);
            if (hasPendingTerminationException()) [[unlikely]]
                return;
        }
    }
}

void VM::promiseRejected(JSPromise* promise)
{
    // UNGIL §E.1b.4/SD15 (U-T8e): m_aboutToBeNotifiedRejectedPromises is
    // CARRIER-CONFINED GIL-off (annex K4 table I). The default tracker
    // (JSGlobalObject::promiseRejectionTracker) reaches here on whatever
    // thread settles; a spawned settler routes through the handoff queue and
    // the record lands in this vector when the carrier flush re-invokes the
    // default hook on the carrier. GIL-on/flag-off: unchanged single append.
    if (m_gilOff && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        enqueuePromiseRejectionTrackerHandoffRecord(*this, promise, JSPromiseRejectionOperation::Reject);
        return;
    }
    m_aboutToBeNotifiedRejectedPromises.constructAndAppend(*this, promise);
}

void VM::drainMicrotasks()
{
    if (m_drainMicrotaskDelayScopeCount.load(std::memory_order_relaxed)) [[unlikely]]
        return;

    // UNGIL §E.1/I11 (U-T9): GIL-off, a spawned/non-main-carrier thread
    // drains ONLY its own per-lite queue (enqueued/drained by its owner —
    // I11; reaction jobs run on the SETTLING thread, SD10/§E.1b.1). The VM
    // default queue stays the main carrier's. didExhaustMicrotaskQueue
    // already gates its carrier-confined work internally (U-T8e).
    // MAIN-CARRIER KEY (GIL-removal review round): same re-key as
    // queueMicrotask above — m_mainVMLite is never installed GIL-off, so
    // without the ownerHasNoTlsDtor arm the "main carrier: landed body"
    // branch below was DEAD and m_defaultMicrotaskQueue (which every
    // JSGlobalObject's m_microtaskQueue aliases at construction) was never
    // drained gilOff. The main thread's carrier now falls through to the
    // landed default-queue body. RESIDUAL (AB-20-adjacent, recorded in
    // INTEGRATE-ungil.md): a gilOff VM whose threads are ALL non-main still
    // has no default-queue drainer for no-lite-window enqueues.
    // Flag-off/GIL-on/main carrier: the landed body, byte-identical.
    if (m_gilOff) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == this && lite != m_mainVMLite.get() && !lite->ownerHasNoTlsDtor) {
            MicrotaskQueue* queue = lite->defaultMicrotaskQueue.get();
            if (!queue) {
                finalizeSynchronousJSExecution();
                return;
            }
            if (executionForbidden()) [[unlikely]]
                queue->clear();
            else {
                std::optional<VMEntryScope> entryScope;
                if (!queue->isEmpty())
                    entryScope.emplace(*this, nullptr);
                while (true) {
                    queue->performMicrotaskCheckpoint</* useCallOnEachMicrotask */ true>(*this,
                        [&](JSGlobalObject*, JSGlobalObject* nextGlobalObject) {
                            if (entryScope && nextGlobalObject)
                                entryScope->setGlobalObject(nextGlobalObject);
                        });
                    if (hasPendingTerminationException()) [[unlikely]]
                        return;
                    didExhaustMicrotaskQueue();
                    if (hasPendingTerminationException()) [[unlikely]]
                        return;
                    if (queue->isEmpty())
                        break;
                    if (!entryScope)
                        entryScope.emplace(*this, nullptr);
                }
            }
            finalizeSynchronousJSExecution();
            return;
        }
        // UNGIL §E.1 (TSAN family 30, microtask-queue) — AB-25 sibling
        // fail-stop, DRAIN arm: the landed body below dequeues from and
        // swaps the VM default queue's plain Deques, which GIL-off are
        // owned by the MAIN thread's carrier (AB-23 re-key). A foreign
        // ENQUEUE is routable through the queue's locked inbox
        // (queueMicrotask above), but a foreign DRAIN is not — it would be
        // two threads running checkpoints over one set of plain Deques.
        // Spawned same-VM threads took the per-lite arm above; anything
        // else reaching here off-main (foreign-VM lite, no-lite window on
        // a non-main thread) must fail loudly rather than race. Flag-off/
        // GIL-on: branch not taken, landed body byte-identical.
        RELEASE_ASSERT(WTF::isMainThread());
    }

    if (executionForbidden()) [[unlikely]]
        m_defaultMicrotaskQueue->clear();
    else {
        std::optional<VMEntryScope> entryScope;
        if (!m_defaultMicrotaskQueue->isEmpty())
            entryScope.emplace(*this, nullptr);
        while (true) {
            m_defaultMicrotaskQueue->performMicrotaskCheckpoint</* useCallOnEachMicrotask */ true>(*this,
                [&](JSGlobalObject*, JSGlobalObject* nextGlobalObject) {
                    if (entryScope && nextGlobalObject)
                        entryScope->setGlobalObject(nextGlobalObject);
                });
            if (hasPendingTerminationException()) [[unlikely]]
                return;
            didExhaustMicrotaskQueue();
            if (hasPendingTerminationException()) [[unlikely]]
                return;
            if (m_defaultMicrotaskQueue->isEmpty())
                break;
            if (!entryScope)
                entryScope.emplace(*this, nullptr);
        }
    }
    finalizeSynchronousJSExecution();
}

#if USE(BUN_JSC_ADDITIONS)
void VM::drainMicrotasksForGlobalObject(JSGlobalObject* globalObject)
{
    // UNGIL review fix (Bun-additions entry point missed by the §E.1b.4
    // host-hook disposition table): GIL-off, JSGlobalObject::queueMicrotask
    // reroutes every entered thread's enqueues to the CURRENT lite's
    // defaultMicrotaskQueue (perLiteRealmRoutingLite — even the process
    // main thread runs on a carrier lite), so clearing only the VM default
    // queue would leave microtasks referencing the cleared global alive in
    // the per-lite queues — exactly the stale-context execution this API
    // exists to prevent. Clear the CURRENT lite's queue too (I11: a lite's
    // queue is touched only by its owner thread, and this API is called on
    // the thread that owns the global's tasks). Sibling lites' queues
    // cannot be cleared from here without breaking the I11 owner-only
    // discipline — recorded as activation blocker AB-20 in
    // INTEGRATE-ungil.md (per-lite clear request fan-out, serviced at each
    // owner's next drain). GIL-on/flag-off: byte-identical.
    if (m_gilOff) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == this) {
            if (MicrotaskQueue* queue = lite->defaultMicrotaskQueue.get())
                queue->clearForGlobalObject(globalObject);
        }
        // UNGIL AB-25 interim fail-stop (GIL-removal round 5): the comment
        // above asserts "this API is called on the thread that owns the
        // global's tasks", but nothing enforced it for the VM DEFAULT queue
        // arm below — gilOff that queue is owned by the MAIN thread's
        // carrier (AB-23 re-key), and clearForGlobalObject from a spawned/
        // non-main thread is an unsynchronized Deque mutation racing the
        // owner's performMicrotaskCheckpoint. Fail loudly until the
        // AB-20/AB-23/AB-25 per-owner service-request word routes this as a
        // "clear for global G" request serviced at the main carrier's next
        // drain. Flag-off/GIL-on: branch not taken, byte-identical below.
        RELEASE_ASSERT(WTF::isMainThread());
    }
    m_defaultMicrotaskQueue->clearForGlobalObject(globalObject);
}
#endif

void sanitizeStackForVM(VM& vm)
{
    auto& thread = Thread::currentSingleton();
    auto& stack = thread.stack();
    // UNGIL §F.2 ANNEX F2 fixed ruling (U-T8): GIL-off this BRANCH consumes
    // the TOKEN meaning and uses the CURRENT lite's lastStackTop —
    // vm.lastStackTop()/setLastStackTop are the §A.1.3 mode-split selectors
    // routing to the CURRENT carrier/spawned lite, so token-true implies the
    // slot read below belongs to THIS thread's entry (never another
    // mutator's). GIL-on: unchanged (predicate == mutex, VM-block slot).
    if (!vm.currentThreadIsHoldingAPILock())
        return; // vm.lastStackTop() may not be set up correctly if JSLock is not held.

    logSanitizeStack(vm);

    RELEASE_ASSERT(stack.contains(vm.lastStackTop()), 0xaa10, vm.lastStackTop(), stack.origin(), stack.end());
#if ENABLE(C_LOOP)
    vm.cloopStack().sanitizeStack();
#else
    sanitizeStackForVMImpl(&vm);
#endif
    RELEASE_ASSERT(stack.contains(vm.lastStackTop()), 0xaa20, vm.lastStackTop(), stack.origin(), stack.end());
}

size_t VM::committedStackByteCount()
{
#if !ENABLE(C_LOOP)
    // When using the C stack, we don't know how many stack pages are actually
    // committed. So, we use the current stack usage as an estimate.
    uint8_t* current = std::bit_cast<uint8_t*>(currentStackPointer());
    uint8_t* high = std::bit_cast<uint8_t*>(Thread::currentSingleton().stack().origin());
    return high - current;
#else
    return CLoopStack::committedByteCount();
#endif
}

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
void VM::verifyExceptionCheckNeedIsSatisfied(unsigned recursionDepth, ExceptionEventLocation& location)
{
    if (!Options::validateExceptionChecks())
        return;

    // Obligation 10 mode split: verification runs on the scope's own thread,
    // so this resolves the same storage the scope's simulateThrow wrote.
    auto& verificationState = exceptionScopeVerificationState();
    if (verificationState.m_needExceptionCheck) [[unlikely]] {
        auto throwDepth = verificationState.m_simulatedThrowPointRecursionDepth;
        auto& throwLocation = verificationState.m_simulatedThrowPointLocation;

        dataLog(
            "ERROR: Unchecked JS exception:\n"
            "    This scope can throw a JS exception: ", throwLocation, "\n"
            "        (ExceptionScope::m_recursionDepth was ", throwDepth, ")\n"
            "    But the exception was unchecked as of this scope: ", location, "\n"
            "        (ExceptionScope::m_recursionDepth was ", recursionDepth, ")\n"
            "\n");

        StringPrintStream out;
        std::unique_ptr<StackTrace> currentTrace = StackTrace::captureStackTrace(Options::unexpectedExceptionStackTraceLimit());

        if (Options::dumpSimulatedThrows()) {
            out.println("The simulated exception was thrown at:");
            out.println(StackTracePrinter { *verificationState.m_nativeStackTraceOfLastSimulatedThrow, "    " });
        }
        out.println("Unchecked exception detected at:");
        out.println(StackTracePrinter { *currentTrace, "    " });

        dataLog(out.toCString());
        RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("exception check validation failed");
    }
}

void VM::clearNativeStackTraceOfLastThrow()
{
    exceptionScopeVerificationState().m_nativeStackTraceOfLastThrow = nullptr; // Obligation 10 mode split.
}

NEVER_INLINE void VM::assertExceptionScopeVerificationFallbackArmIsSafe()
{
    // Obligation 10 landing requirement: the GIL-off fallback arm of
    // exceptionScopeVerificationState() (no current lite, or lite->vm !=
    // this) is reachable only from the carrier / m_lock-holding windows
    // (ctor tail before first entry, ~VM tail after uninstall). A
    // non-mutator scope user (GC/compiler thread) landing here would
    // silently reopen the shared-word race this split closed — fail loudly
    // instead. RELEASE_ASSERT, not ASSERT: ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // is ASSERT || ASAN, so in a release+ASAN build — the configuration where
    // the original deterministic stack-use-after-return was caught — a plain
    // ASSERT compiles to nothing and two no-lite/foreign-lite threads could
    // silently interleave the non-idempotent scope-chain write-backs on the
    // shared VM copy. NEVER_INLINE cold fallback arm: the hard assert is free.
    RELEASE_ASSERT(apiLock().currentThreadIsHoldingLock());
}
#endif

ScratchBuffer* VM::scratchBufferForSize(size_t size)
{
    if (!size)
        return nullptr;

    // UNGIL §A.1.6 (annex A16): gilOff, the non-baked path is the CURRENT
    // lite's table by size-class — this dispatch IMPLEMENTS the reserved
    // VMLite::scratchBufferForSize contract (re-freeze recorded vs
    // vmstate:534-539, both sides; INTEGRATE-ungil.md supersession ledger).
    if (m_gilOff) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == this)
            return lite->scratchBufferForSize(size);
        // No installed same-VM lite (e.g. compiler-thread C++ paths):
        // fall through to the VM-owned buffers below — those callers do not
        // run JS and the buffers they get are still GC-scanned.
    }

    Locker locker { m_scratchBufferLock };

    if (size > m_sizeOfLastScratchBuffer) {
        // Protect against a N^2 memory usage pathology by ensuring
        // that at worst, we get a geometric series, meaning that the
        // total memory usage is somewhere around
        // max(scratch buffer size) * 4.
        m_sizeOfLastScratchBuffer = size * 2;

        ScratchBuffer* newBuffer = ScratchBuffer::create(m_sizeOfLastScratchBuffer);
        RELEASE_ASSERT(newBuffer);
        m_scratchBuffers.append(newBuffer);
    }

    ScratchBuffer* result = m_scratchBuffers.last();
    return result;
}

void VM::clearScratchBuffers()
{
    // UNGIL §A.1.6: gilOff, ClearScratchBuffers is a THREAD-LOCAL service
    // (§A.1.5 table) — clear the CURRENT lite's buffers; the VM-owned list
    // is also cleared (harmless; compiler-thread fallback users).
    if (m_gilOff) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == this)
            lite->clearScratchBuffers();
    }
    {
        Locker locker { m_scratchBufferLock };
        for (auto* scratchBuffer : m_scratchBuffers)
            scratchBuffer->setActiveLength(0);
    }
    clearEntryScopeService(EntryScopeService::ClearScratchBuffers);
}

bool VM::isScratchBuffer(void* ptr)
{
    {
        Locker locker { m_scratchBufferLock };
        for (auto* scratchBuffer : m_scratchBuffers) {
            if (scratchBuffer->dataBuffer() == ptr)
                return true;
        }
    }
    // UNGIL §A.1.6: gilOff callers may hold a per-lite buffer.
    if (m_gilOff) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == this) {
            Locker locker { lite->scratchBufferLock };
            for (auto* scratchBuffer : lite->scratchBuffers) {
                if (scratchBuffer->dataBuffer() == ptr)
                    return true;
            }
        }
    }
    return false;
}

unsigned VM::allocateBakedScratchBufferIndex(size_t size)
{
    // ANNEX A16 (UNGIL U-T1; dark until U-T4 emission): baked DFG/FTL
    // scratch addresses become process-wide indices; install fans to this
    // VM's registered lites under the registry lock (§LK.6 re-rank legality
    // for the nested scratchBufferLock), registration backfills the rest.
    RELEASE_ASSERT(m_gilOff);
    unsigned index = ScratchBufferRegistry::singleton().allocateIndex(size);
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    for (VMLite* lite : registry.lites) {
        if (lite->vm == this)
            lite->ensureScratchBufferAtIndex(index, size);
    }
    return index;
}

Ref<Waiter> VM::syncWaiter()
{
    return m_syncWaiter;
}

JSValue VM::checkVMEntryPermission()
{
    if (Options::crashOnDisallowedVMEntry() || g_jscConfig.vmEntryDisallowed)
        CRASH_WITH_EXTRA_SECURITY_IMPLICATION_AND_INFO(VMEntryDisallowed, "VM entry disallowed"_s);
    return jsUndefined();
}

JSPropertyNameEnumerator* VM::emptyPropertyNameEnumeratorSlow()
{
    ASSERT(!m_emptyPropertyNameEnumerator);
    PropertyNameArrayBuilder propertyNames(*this, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    auto* enumerator = JSPropertyNameEnumerator::create(*this, nullptr, 0, 0, WTF::move(propertyNames));
    m_emptyPropertyNameEnumerator.setWithoutWriteBarrier(enumerator);
    return enumerator;
}

NativeExecutable* VM::promiseResolvingFunctionResolveExecutableSlow()
{
    ASSERT(!m_promiseResolvingFunctionResolveExecutable);
    auto* executable = getHostFunction(promiseResolvingFunctionResolve, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseResolvingFunctionResolveExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseResolvingFunctionRejectExecutableSlow()
{
    ASSERT(!m_promiseResolvingFunctionRejectExecutable);
    auto* executable = getHostFunction(promiseResolvingFunctionReject, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseResolvingFunctionRejectExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseFirstResolvingFunctionResolveExecutableSlow()
{
    ASSERT(!m_promiseFirstResolvingFunctionResolveExecutable);
    auto* executable = getHostFunction(promiseFirstResolvingFunctionResolve, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseFirstResolvingFunctionResolveExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseFirstResolvingFunctionRejectExecutableSlow()
{
    ASSERT(!m_promiseFirstResolvingFunctionRejectExecutable);
    auto* executable = getHostFunction(promiseFirstResolvingFunctionReject, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseFirstResolvingFunctionRejectExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseResolvingFunctionResolveWithInternalMicrotaskExecutableSlow()
{
    ASSERT(!m_promiseResolvingFunctionResolveWithInternalMicrotaskExecutable);
    auto* executable = getHostFunction(promiseResolvingFunctionResolveWithInternalMicrotask, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseResolvingFunctionResolveWithInternalMicrotaskExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseResolvingFunctionRejectWithInternalMicrotaskExecutableSlow()
{
    ASSERT(!m_promiseResolvingFunctionRejectWithInternalMicrotaskExecutable);
    auto* executable = getHostFunction(promiseResolvingFunctionRejectWithInternalMicrotask, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseResolvingFunctionRejectWithInternalMicrotaskExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseCapabilityExecutorExecutableSlow()
{
    ASSERT(!m_promiseCapabilityExecutorExecutable);
    auto* executable = getHostFunction(promiseCapabilityExecutor, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseCapabilityExecutorExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAllFulfillFunctionExecutableSlow()
{
    ASSERT(!m_promiseAllFulfillFunctionExecutable);
    auto* executable = getHostFunction(promiseAllFulfillFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAllFulfillFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAllSlowFulfillFunctionExecutableSlow()
{
    ASSERT(!m_promiseAllSlowFulfillFunctionExecutable);
    auto* executable = getHostFunction(promiseAllSlowFulfillFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAllSlowFulfillFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAllSettledFulfillFunctionExecutableSlow()
{
    ASSERT(!m_promiseAllSettledFulfillFunctionExecutable);
    auto* executable = getHostFunction(promiseAllSettledFulfillFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAllSettledFulfillFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAllSettledRejectFunctionExecutableSlow()
{
    ASSERT(!m_promiseAllSettledRejectFunctionExecutable);
    auto* executable = getHostFunction(promiseAllSettledRejectFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAllSettledRejectFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAllSettledSlowFulfillFunctionExecutableSlow()
{
    ASSERT(!m_promiseAllSettledSlowFulfillFunctionExecutable);
    auto* executable = getHostFunction(promiseAllSettledSlowFulfillFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAllSettledSlowFulfillFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAllSettledSlowRejectFunctionExecutableSlow()
{
    ASSERT(!m_promiseAllSettledSlowRejectFunctionExecutable);
    auto* executable = getHostFunction(promiseAllSettledSlowRejectFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAllSettledSlowRejectFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAnyRejectFunctionExecutableSlow()
{
    ASSERT(!m_promiseAnyRejectFunctionExecutable);
    auto* executable = getHostFunction(promiseAnyRejectFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAnyRejectFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

NativeExecutable* VM::promiseAnySlowRejectFunctionExecutableSlow()
{
    ASSERT(!m_promiseAnySlowRejectFunctionExecutable);
    auto* executable = getHostFunction(promiseAnySlowRejectFunction, ImplementationVisibility::Public, callHostFunctionAsConstructor, emptyString());
    m_promiseAnySlowRejectFunctionExecutable.setWithoutWriteBarrier(executable);
    return executable;
}

bool VM::isGILOffProcess()
{
    // UNGIL §A.1.3 level (i) / §0 U0c: OPTION-derived; the JSCConfig
    // gilOffProcess byte (LLInt's copy, beside the M4a slot — LANDED,
    // closing U-T3 obligation 9b; as of AB17g item 2 it is latched in the
    // VM ctor strictly BEFORE the m_gilOff designation, via
    // Config::latchGILOffProcess()) MUST stay derivation-identical to this
    // conjunction and to the notifyOptionsChanged() RELEASE_ASSERT latch
    // (runtime/Options.cpp). U0 option validation refuses GIL-off without
    // the trio (forced useThreadGIL=1), so the extra terms are
    // belt-and-suspenders.
    return Options::useJSThreads() && !Options::useThreadGIL()
        && Options::useVMLite() && Options::useSharedAtomStringTable() && Options::useSharedGCHeap();
}

void Heap::verifyStickySharedServerDesignation()
{
    // UNGIL §0 U0c (ANNEX U0C, BINDING; U-T3):
    // RELEASE_ASSERT(gilOffProcess => the server VM's m_gilOff == 1) before
    // a noteSharedServerSticky() trigger. WIRED at the winner-ctor eager
    // trigger (VM ctor, above); the annex-mandated HeapClientSet::add
    // second-client site (HeapClientSet.cpp:69) is NOT YET WIRED — see the
    // declaration comment in heap/Heap.h for the open-obligation record and
    // the I13 interim backstop. Under
    // gilOffProcess only the designation winner may ever see clientSet() > 1
    // — a LOSER VM's U0b spawn refusal keeps its clientSet() <= 1, so a
    // loser reaching the trigger IS a bug (and noteSharedServerSticky's
    // inner I13 CAS firing right after is the correct behavior; this assert
    // just makes the failure mode precise). GIL-on processes
    // (!gilOffProcess) are exempt: the legacy "option && size() EVER > 1"
    // sticky trigger is their sanctioned path.
    //
    // Defined HERE (not Heap.cpp) because it needs the complete VM type
    // (vm()/gilOff()) and Heap.cpp is outside U-T3's owned-file set — see
    // INTEGRATE-ungil.md supersession ledger row 6. Declared in heap/Heap.h.
    if (!VM::isGILOffProcess())
        return;
    RELEASE_ASSERT(vm().gilOff());
}

bool VM::isAnyThreadEntered() const
{
    // UNGIL §A.1.5: VM-wide "entered" consumers iterate the registry under
    // its lock (leaf — nothing acquired while held). Replaced by the §A.3.1
    // entered-thread set's non-emptiness when U-T5 lands it.
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    for (VMLite* lite : registry.lites) {
        if (lite->vm == this && lite->entryScope.load(std::memory_order_relaxed))
            return true;
    }
    return false;
}

void VM::requestVMWideEntryScopeService(EntryScopeService service)
{
    // UNGIL §A.1.5 service routing (mirrors §A.2.3): VM-wide requests set
    // the VM-level word AND fan into this VM's registered lites under the
    // registry lock; carrier registration backfills the word into lites
    // registered later. The VM-level word's writers are serialized by the
    // registry lock GIL-off (owner threads read/clear only their lite bits).
    ASSERT(m_gilOff);
    uint16_t bit = packedServiceBits(service);
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    entryScopeServices().add(service);
    for (VMLite* lite : registry.lites) {
        if (lite->vm == this)
            lite->entryScopeServicesRawBits.fetch_or(bit, std::memory_order_relaxed);
    }
}

void VM::requestVMWideEntryScopeService(ConcurrentEntryScopeService service)
{
    // CONCURRENT_SAFE: the requester may hold NO lite (§A.1.5); the registry
    // lock is a leaf, so this is callable from any thread.
    ASSERT(m_gilOff);
    uint16_t bit = packedServiceBits(service);
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    concurrentEntryScopeServices().add(service);
    for (VMLite* lite : registry.lites) {
        if (lite->vm == this)
            lite->entryScopeServicesRawBits.fetch_or(bit, std::memory_order_relaxed);
    }
}

void VM::backfillEntryScopeServiceBitsForLiteRegistration(VMLite& lite)
{
    // §A.1.5: under the registry lock so it cannot interleave with a
    // concurrent VM-wide fan-out (which holds the same lock).
    ASSERT(lite.vm == this);
    Locker locker { VMLiteRegistry::singleton().lock };
    if (uint16_t word = m_entryScopeServicesRawBits)
        lite.entryScopeServicesRawBits.fetch_or(word, std::memory_order_relaxed);
}

void VM::executeEntryScopeServicesOnEntry()
{
    if (hasEntryScopeServiceRequest(EntryScopeService::FirePrimitiveGigacageEnabled)) [[unlikely]] {
        m_primitiveGigacageEnabled.fireAll(*this, "Primitive gigacage disabled asynchronously");
        clearEntryScopeService(EntryScopeService::FirePrimitiveGigacageEnabled);
    }

    // Reset the date cache between JS invocations to force the VM to
    // observe time zone changes.
    dateCache.resetIfNecessary();

    RefPtr watchdog = this->watchdog();
    if (watchdog) [[unlikely]]
        watchdog->enteredVM();

#if ENABLE(SAMPLING_PROFILER)
    RefPtr samplingProfiler = this->samplingProfiler();
    if (samplingProfiler) [[unlikely]]
        samplingProfiler->noticeVMEntry();
#endif

    if (Options::useTracePoints()) [[unlikely]]
        tracePoint(VMEntryScopeStart);

    if (hasEntryScopeServiceRequest(ConcurrentEntryScopeService::NeedStopTheWorld)) [[unlikely]]
        VMManager::singleton().notifyVMActivation(*this);
}

void VM::executeEntryScopeServicesOnExit()
{
    if (hasEntryScopeServiceRequest(ConcurrentEntryScopeService::NeedStopTheWorld)) [[unlikely]]
        VMManager::singleton().notifyVMDeactivation(*this);

    if (Options::useTracePoints()) [[unlikely]]
        tracePoint(VMEntryScopeEnd);

    RefPtr watchdog = this->watchdog();
    if (watchdog) [[unlikely]]
        watchdog->exitedVM();

    if (hasEntryScopeServiceRequest(EntryScopeService::PopListeners)) {
        auto listeners = WTF::move(m_didPopListeners);
        for (auto& listener : listeners)
            listener();
        clearEntryScopeService(EntryScopeService::PopListeners);
    }

    // Normally, we want to clear the hasTerminationRequest flag here. However, if the
    // VMTraps::NeedTermination bit is still set at this point, then it means that
    // VMTraps::handleTraps() has not yet been called for this termination request. As a
    // result, the TerminationException has not been thrown yet. Some client code relies
    // on detecting the presence of the TerminationException in order to signal that a
    // termination was requested. Hence, don't clear the hasTerminationRequest flag until
    // VMTraps::handleTraps() has been called, and the TerminationException is thrown.
    //
    // Note: perhaps there's a better way for the client to know that a termination was
    // requested (after all, the request came from the client). However, this is how the
    // client code currently works. Changing that will take some significant effort to hunt
    // down all the places in client code that currently rely on this behavior.
    if (!traps().needHandling(VMTraps::NeedTermination))
        clearHasTerminationRequest();

    clearScratchBuffers();
}

JSGlobalObject* VM::deprecatedVMEntryGlobalObject(JSGlobalObject* globalObject) const
{
    // UNGIL §A.1.5: per-lite entry record when gilOff (this is asked on the
    // running thread about ITS entry).
    if (VMEntryScope* scope = const_cast<VM*>(this)->currentThreadEntryScope())
        return scope->globalObject();
    return globalObject;
}

void VM::setCrashOnVMCreation(bool shouldCrash)
{
    vmCreationShouldCrash = shouldCrash;
}

void VM::addLoopHintExecutionCounter(const JSInstruction* instruction)
{
    Locker locker { m_loopHintExecutionCountLock };
    auto addResult = m_loopHintExecutionCounts.add(instruction, std::pair<unsigned, std::unique_ptr<uintptr_t>>(0, nullptr));
    if (addResult.isNewEntry) {
        auto ptr = WTF::makeUniqueWithoutFastMallocCheck<uintptr_t>();
        *ptr = 0;
        addResult.iterator->value.second = WTF::move(ptr);
    }
    ++addResult.iterator->value.first;
}

uintptr_t* VM::getLoopHintExecutionCounter(const JSInstruction* instruction)
{
    Locker locker { m_loopHintExecutionCountLock };
    auto iter = m_loopHintExecutionCounts.find(instruction);
    return iter->value.second.get();
}

void VM::removeLoopHintExecutionCounter(const JSInstruction* instruction)
{
    Locker locker { m_loopHintExecutionCountLock };
    auto iter = m_loopHintExecutionCounts.find(instruction);
    RELEASE_ASSERT(!!iter->value.first);
    --iter->value.first;
    if (!iter->value.first)
        m_loopHintExecutionCounts.remove(iter);
}

void VM::beginMarking()
{
    if (Options::useVMLite()) [[unlikely]] {
        // SPEC-vmstate §6.5: markers traverse the registration list while
        // mutators run; markers hold no other lock here, and holders may
        // acquire NO lock while holding it (leaf, §7).
        Locker locker { VMLiteRegistry::singleton().lock };
        m_microtaskQueues.forEach([&](MicrotaskQueue* microtaskQueue) {
            microtaskQueue->beginMarking();
        });
        return;
    }
    m_microtaskQueues.forEach([&](MicrotaskQueue* microtaskQueue) {
        microtaskQueue->beginMarking();
    });
}

template<typename Visitor>
void VM::visitAggregateImpl(Visitor& visitor)
{
    if (Options::useVMLite()) [[unlikely]] {
        // SPEC-vmstate §6.5 (M11): registry leaf lock around ONLY the
        // forEach; protects LIST MEMBERSHIP against concurrent queue
        // ctor/dtor (M12). Queue CONTENTS are visited with all mutators
        // suspended (see the M11 scope note / cross-WS item 12).
        Locker locker { VMLiteRegistry::singleton().lock };
        m_microtaskQueues.forEach([&](MicrotaskQueue* microtaskQueue) {
            microtaskQueue->visitAggregate(visitor);
        });
    } else {
        m_microtaskQueues.forEach([&](MicrotaskQueue* microtaskQueue) {
            microtaskQueue->visitAggregate(visitor);
        });
    }
#if USE(BUN_JSC_ADDITIONS)
    // The synchronous-module queue's Vector buffer lives on the heap, so
    // conservative stack scanning does not see the JSValues it holds. Walk the
    // full prev-linked chain (loadModuleSync can re-enter while a queued
    // module evaluates) and mark every pending task's arguments.
    for (auto* q = m_synchronousModuleQueue; q; q = q->prev) {
        for (auto& t : q->tasks) {
            visitor.appendUnbarriered(t.arg0);
            visitor.appendUnbarriered(t.arg1);
            visitor.appendUnbarriered(t.arg2);
        }
    }
#endif
    numericStrings.visitAggregate(visitor);
    m_builtinExecutables->visitAggregate(visitor);
    m_regExpCache->visitAggregate(visitor);

    if (heap.collectionScope() != CollectionScope::Full)
        stringReplaceCache.visitAggregate(visitor);

    visitor.append(structureStructure);
    visitor.append(structureRareDataStructure);
    visitor.append(stringStructure);
    visitor.append(propertyNameEnumeratorStructure);
    visitor.append(getterSetterStructure);
    visitor.append(customGetterSetterStructure);
    visitor.append(domAttributeGetterSetterStructure);
    visitor.append(scopedArgumentsTableStructure);
    visitor.append(apiWrapperStructure);
    visitor.append(nativeExecutableStructure);
    visitor.append(evalExecutableStructure);
    visitor.append(programExecutableStructure);
    visitor.append(functionExecutableStructure);
#if ENABLE(WEBASSEMBLY)
    visitor.append(pinballCompletionStructure);
    visitor.append(webAssemblyCalleeGroupStructure);
    visitor.append(webAssemblyStreamingContextStructure);
#endif
    visitor.append(moduleProgramExecutableStructure);
    visitor.append(slimPromiseReactionStructure);
    visitor.append(fullPromiseReactionStructure);
    visitor.append(jsMicrotaskDispatcherStructure);
    visitor.append(moduleLoaderStructure);
    visitor.append(moduleRegistryEntryStructure);
    visitor.append(moduleLoadingContextStructure);
    visitor.append(moduleLoaderPayloadStructure);
    visitor.append(moduleGraphLoadingStateStructure);
    visitor.append(promiseCombinatorsContextStructure);
    visitor.append(promiseCombinatorsGlobalContextStructure);
    visitor.append(regExpStructure);
    visitor.append(symbolStructure);
    visitor.append(symbolTableStructure);
    for (auto& structure : cellButterflyStructures)
        visitor.append(structure);
    visitor.append(cellButterflyOnlyAtomStringsStructure);
    visitor.append(sourceCodeStructure);
    visitor.append(structureChainStructure);
    visitor.append(sparseArrayValueMapStructure);
    visitor.append(templateObjectDescriptorStructure);
    visitor.append(unlinkedFunctionExecutableStructure);
    visitor.append(unlinkedProgramCodeBlockStructure);
    visitor.append(unlinkedEvalCodeBlockStructure);
    visitor.append(unlinkedFunctionCodeBlockStructure);
    visitor.append(unlinkedModuleProgramCodeBlockStructure);
    visitor.append(propertyTableStructure);
    visitor.append(functionRareDataStructure);
    visitor.append(exceptionStructure);
    visitor.append(programCodeBlockStructure);
    visitor.append(moduleProgramCodeBlockStructure);
    visitor.append(evalCodeBlockStructure);
    visitor.append(functionCodeBlockStructure);
    visitor.append(hashMapBucketSetStructure);
    visitor.append(hashMapBucketMapStructure);
    visitor.append(bigIntStructure);

    visitor.append(m_emptyPropertyNameEnumerator);
    visitor.append(m_orderedHashTableDeletedValue);
    visitor.append(m_orderedHashTableSentinel);
    visitor.append(m_sentinelStructure);
    visitor.append(m_fastArrayValuesSentinel);
    visitor.append(m_fastArrayKeysSentinel);
    visitor.append(m_fastArrayEntriesSentinel);
    visitor.append(m_fastMapKeysSentinel);
    visitor.append(m_fastMapValuesSentinel);
    visitor.append(m_fastMapEntriesSentinel);
    visitor.append(m_fastSetValuesSentinel);
    visitor.append(m_fastSetEntriesSentinel);
    visitor.append(m_fastStringValuesSentinel);
    visitor.append(m_cachedSortScratch);
    visitor.append(m_sortScratchSentinel);
    visitor.append(m_fastCanConstructBoundExecutable);
    visitor.append(m_slowCanConstructBoundExecutable);
    visitor.append(lastCachedString);
    visitor.append(heapBigIntConstantOne);
    visitor.append(heapBigIntConstantZero);
    visitor.append(m_cachedBigIntDivisor);
    visitor.append(m_nextCachedBigIntDivisor);

    visitor.append(m_promiseResolvingFunctionResolveExecutable);
    visitor.append(m_promiseResolvingFunctionRejectExecutable);
    visitor.append(m_promiseFirstResolvingFunctionResolveExecutable);
    visitor.append(m_promiseFirstResolvingFunctionRejectExecutable);
    visitor.append(m_promiseResolvingFunctionResolveWithInternalMicrotaskExecutable);
    visitor.append(m_promiseResolvingFunctionRejectWithInternalMicrotaskExecutable);
    visitor.append(m_promiseCapabilityExecutorExecutable);
    visitor.append(m_promiseAllFulfillFunctionExecutable);
    visitor.append(m_promiseAllSlowFulfillFunctionExecutable);
    visitor.append(m_promiseAllSettledFulfillFunctionExecutable);
    visitor.append(m_promiseAllSettledRejectFunctionExecutable);
    visitor.append(m_promiseAllSettledSlowFulfillFunctionExecutable);
    visitor.append(m_promiseAllSettledSlowRejectFunctionExecutable);
    visitor.append(m_promiseAnyRejectFunctionExecutable);
    visitor.append(m_promiseAnySlowRejectFunctionExecutable);
}
DEFINE_VISIT_AGGREGATE(VM);

void VM::addDebugger(Debugger& debugger)
{
    m_debuggers.append(&debugger);
}

void VM::removeDebugger(Debugger& debugger)
{
    m_debuggers.remove(&debugger);
}

void VM::performOpportunisticallyScheduledTasks(MonotonicTime deadline, OptionSet<SchedulerOptions> options)
{
    constexpr bool verbose = false;

    dataLogLnIf(verbose, "[OPPORTUNISTIC TASK] QUERY", " signpost:(", JSC::activeJSGlobalObjectSignpostIntervalCount.load(), ")");
    JSLockHolder locker { *this };
    if (deferredWorkTimer->hasImminentlyScheduledWork()) {
        dataLogLnIf(verbose, "[OPPORTUNISTIC TASK] GaveUp: DeferredWorkTimer hasImminentlyScheduledWork signpost:(", JSC::activeJSGlobalObjectSignpostIntervalCount.load(), ")");
        return;
    }

    SetForScope insideOpportunisticTaskScope { heap.m_isInOpportunisticTask, true };
    [&] {
        auto secondsSinceEpoch = ApproximateTime::now().secondsSinceEpoch();
        auto remainingTime = deadline.secondsSinceEpoch() - secondsSinceEpoch;

        if (options.contains(SchedulerOptions::HasImminentlyScheduledWork)) {
            dataLogLnIf(verbose, "[OPPORTUNISTIC TASK] GaveUp: HasImminentlyScheduledWork ", remainingTime, " signpost:(", JSC::activeJSGlobalObjectSignpostIntervalCount.load(), ")");
            return;
        }

        static constexpr auto minimumDelayBeforeOpportunisticFullGC = 30_ms;
        static constexpr auto minimumDelayBeforeOpportunisticEdenGC = 10_ms;
        static constexpr auto extraDurationToAvoidExceedingDeadlineDuringFullGC = 2_ms;
        static constexpr auto extraDurationToAvoidExceedingDeadlineDuringEdenGC = 1_ms;

        auto timeSinceFinishingLastFullGC = secondsSinceEpoch - heap.m_lastFullGCEndTime.secondsSinceEpoch();
        if (timeSinceFinishingLastFullGC > minimumDelayBeforeOpportunisticFullGC && heap.m_shouldDoOpportunisticFullCollection && heap.m_totalBytesVisitedAfterLastFullCollect) {
            auto estimatedGCDuration = (heap.lastFullGCLength() * heap.m_totalBytesVisited) / heap.m_totalBytesVisitedAfterLastFullCollect;
            if (estimatedGCDuration + extraDurationToAvoidExceedingDeadlineDuringFullGC < remainingTime) {
                dataLogLnIf(verbose, "[OPPORTUNISTIC TASK] FULL", " signpost:(", JSC::activeJSGlobalObjectSignpostIntervalCount.load(), ")");
                heap.collectSync(CollectionScope::Full);
                return;
            }
        }

        auto timeSinceLastGC = secondsSinceEpoch - std::max(heap.m_lastGCEndTime, heap.m_currentGCStartTime).secondsSinceEpoch();
        if (timeSinceLastGC > minimumDelayBeforeOpportunisticEdenGC && heap.totalBytesAllocatedThisCycle() && heap.m_bytesAllocatedBeforeLastEdenCollect) {
            auto estimatedGCDuration = (heap.lastEdenGCLength() * heap.totalBytesAllocatedThisCycle()) / heap.m_bytesAllocatedBeforeLastEdenCollect;
            if (estimatedGCDuration + extraDurationToAvoidExceedingDeadlineDuringEdenGC < remainingTime) {
                dataLogLnIf(verbose, "[OPPORTUNISTIC TASK] EDEN: ", timeSinceFinishingLastFullGC, " ", timeSinceLastGC, " ", heap.m_shouldDoOpportunisticFullCollection, " ", heap.m_totalBytesVisitedAfterLastFullCollect, " ", heap.totalBytesAllocatedThisCycle(), " ", heap.m_bytesAllocatedBeforeLastEdenCollect, " ", heap.m_lastGCEndTime, " ", heap.m_currentGCStartTime, " ", (heap.lastFullGCLength() * heap.m_totalBytesVisited) / heap.m_totalBytesVisitedAfterLastFullCollect, " ", remainingTime, " ", (heap.lastEdenGCLength() * heap.totalBytesAllocatedThisCycle()) / heap.m_bytesAllocatedBeforeLastEdenCollect, " signpost:(", JSC::activeJSGlobalObjectSignpostIntervalCount.load(), ")");
                heap.collectSync(CollectionScope::Eden);
                return;
            } else if (estimatedGCDuration < 2 * remainingTime) {
                if (heap.totalBytesAllocatedThisCycle() * 2 > heap.m_minBytesPerCycle) {
                    heap.collectAsync(CollectionScope::Eden);
                    return;
                }
            }
        }

        dataLogLnIf(verbose, "[OPPORTUNISTIC TASK] GaveUp: nothing met. ", timeSinceFinishingLastFullGC, " ", timeSinceLastGC, " ", heap.m_shouldDoOpportunisticFullCollection, " ", heap.m_totalBytesVisitedAfterLastFullCollect, " ", heap.totalBytesAllocatedThisCycle(), " ", heap.m_bytesAllocatedBeforeLastEdenCollect, " ", heap.m_lastGCEndTime, " ", heap.m_currentGCStartTime, " ", (heap.lastFullGCLength() * heap.m_totalBytesVisited) / heap.m_totalBytesVisitedAfterLastFullCollect, " ", remainingTime, " ", (heap.lastEdenGCLength() * heap.totalBytesAllocatedThisCycle()) / heap.m_bytesAllocatedBeforeLastEdenCollect, " signpost:(", JSC::activeJSGlobalObjectSignpostIntervalCount.load(), ")");
    }();

    heap.sweeper().doWorkUntil(*this, deadline);
}

void VM::invalidateStructureChainIntegrity(StructureChainIntegrityEvent)
{
    if (auto* megamorphicCache = this->megamorphicCache())
        megamorphicCache->bumpEpoch();
}

VM::DrainMicrotaskDelayScope::DrainMicrotaskDelayScope(VM& vm)
    : m_vm(&vm)
{
    increment();
}

VM::DrainMicrotaskDelayScope::~DrainMicrotaskDelayScope()
{
    decrement();
}

VM::DrainMicrotaskDelayScope::DrainMicrotaskDelayScope(const VM::DrainMicrotaskDelayScope& other)
    : m_vm(other.m_vm)
{
    increment();
}

VM::DrainMicrotaskDelayScope& VM::DrainMicrotaskDelayScope::operator=(const VM::DrainMicrotaskDelayScope& other)
{
    if (this == &other)
        return *this;
    decrement();
    m_vm = other.m_vm;
    increment();
    return *this;
}

VM::DrainMicrotaskDelayScope& VM::DrainMicrotaskDelayScope::operator=(VM::DrainMicrotaskDelayScope&& other)
{
    decrement();
    m_vm = std::exchange(other.m_vm, nullptr);
    increment();
    return *this;
}

void VM::DrainMicrotaskDelayScope::increment()
{
    if (m_vm)
        m_vm->m_drainMicrotaskDelayScopeCount.fetch_add(1, std::memory_order_relaxed);
}

void VM::DrainMicrotaskDelayScope::decrement()
{
    if (!m_vm)
        return;
    ASSERT(m_vm->m_drainMicrotaskDelayScopeCount.load(std::memory_order_relaxed));
    if (m_vm->m_drainMicrotaskDelayScopeCount.fetch_sub(1, std::memory_order_relaxed) == 1) {
        JSLockHolder locker(*m_vm);
        m_vm->drainMicrotasks();
    }
}

#if ENABLE(WEBASSEMBLY_DEBUGGER)
Wasm::DebugState* VM::debugState()
{
    RELEASE_ASSERT(!!m_debugState);
    return m_debugState.get();
}
#endif

// AUD1.N2 / K4.II.12: GIL-off per-thread adaptive-search scratch tables —
// see the accessor comment in VM.h. Same lifetime story as the per-thread
// regexp match ovector (RegExp.cpp): plain scratch, no cells, no VM state;
// dies with the thread, so no ~VM walk entry is needed. The tables are an
// adaptive perf cache (a cold table is only a perf event), so per-thread
// duplication is semantics-free.
WTF::AdaptiveStringSearcherTables& VM::gilOffPerThreadStringSearcherTables()
{
    static thread_local std::unique_ptr<WTF::AdaptiveStringSearcherTables> tables;
    if (!tables) [[unlikely]]
        tables = makeUnique<WTF::AdaptiveStringSearcherTables>();
    return *tables;
}

// AUD1 / K4.II.1: GIL-off per-thread number->string caches — see the
// liveNumericStrings() comment in VM.h. The per-thread instance holds
// Strings only (JSString caching disabled, so no GC-visited cells live
// outside a registry walk); the Strings die with the thread, so no ~VM
// walk entry is needed. A cold cache is only a perf event, so per-thread
// duplication is semantics-free.
NumericStrings& VM::gilOffPerThreadNumericStrings()
{
    static thread_local std::unique_ptr<NumericStrings> strings;
    if (!strings) [[unlikely]] {
        strings = makeUniqueWithoutFastMallocCheck<NumericStrings>();
        strings->disableJSStringCaching();
    }
    return *strings;
}

// GIL-off per-thread VM-entry disallowance count — see the slot accessor
// comment in VM.h. Strictly stack-scoped RAII increments/decrements on the
// owning thread, so a plain thread_local is exact (a thread serves one
// installed lite at a time; a scope outliving its VM's installation window
// would be a bug under EITHER storage shape).
unsigned& VM::gilOffPerThreadDisallowVMEntryCount()
{
    static thread_local unsigned count { 0 };
    return count;
}

} // namespace JSC
