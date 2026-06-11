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

#pragma once

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "ConcurrentJSLock.h"
#include "DFGDoesGCCheck.h"
#include "ExceptionEventLocation.h"
#include "FunctionHasExecutedCache.h"
#include "Heap.h"
#include "ImplementationVisibility.h"
#include "IndexingType.h"
#include "Integrity.h"
#include "Interpreter.h"
#include "JSCConfig.h"
#include "JSDateMath.h"
#include "JSONAtomStringCache.h"
#include "KeyAtomStringCache.h"
#include "NativeFunction.h"
#include "NumericStrings.h"
#include "SmallStrings.h"
#include "StringReplaceCache.h"
#include "StringSplitCache.h"
#include "StrongForward.h"
#include "VMLite.h"
#include "VMThreadContext.h"
#include "VMTraps.h"
#include "WeakGCMap.h"
#include "WriteBarrier.h"
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/Compiler.h>
#include <wtf/LazyRef.h>
#include <wtf/LazyUniqueRef.h>
#include <wtf/Lock.h>
#include <wtf/MallocPtr.h>
#include <wtf/ObjectIdentifier.h>
#include <wtf/ThreadSafeRefCountedWithSuppressingSaferCPPChecking.h>
#include <wtf/text/AdaptiveStringSearcher.h>

#if ENABLE(WEBASSEMBLY)
#include <JavaScriptCore/WasmContext.h>
#endif

#if ENABLE(JIT)
#include <JavaScriptCore/ThunkGenerator.h>
#endif

#include "LineColumn.h"

#if ENABLE(REGEXP_TRACING)
#include <wtf/ListHashSet.h>
#endif

// Enable the Objective-C API for platforms with a modern runtime. This has to match exactly what we
// have in JSBase.h.
#if !defined(JSC_OBJC_API_ENABLED)
#if (defined(__clang__) && defined(__APPLE__) && (defined(__MAC_OS_X_VERSION_MIN_REQUIRED) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)))
#define JSC_OBJC_API_ENABLED 1
#else
#define JSC_OBJC_API_ENABLED 0
#endif
#endif

namespace WTF {
class RunLoop;
class SimpleStats;
class StackTrace;
class Stopwatch;
class SymbolImpl;
class UniquedStringImpl;
} // namespace WTF
using WTF::SimpleStats;
using WTF::StackTrace;

namespace JSC {

class ArgList;
class BuiltinExecutables;
class BytecodeIntrinsicRegistry;
class CallFrame;
enum class CallMode;
enum class CommonJITThunkID : uint8_t;
struct CheckpointOSRExitSideState;
class CodeBlock;
class CodeCache;
enum class CodeSpecializationKind : uint8_t;
class CommonIdentifiers;
class CompactTDZEnvironmentMap;
class ConservativeRoots;
class ControlFlowProfiler;
class CrossTaskToken;
class Exception;
class ExceptionScope;
class FuzzerAgent;
class HasOwnPropertyCache;
class HeapAnalyzer;
class HeapProfiler;
class IntlCache;
enum Intrinsic : uint8_t;
class JSDestructibleObjectHeapCellType;
class JSGlobalObject;
class JSSentinel;
class JSLock;
class JSObject;
struct JSPIContext;
class JSPromise;
class JSPropertyNameEnumerator;
class JITSizeStatistics;
class JITThunks;
class MegamorphicCache;
class MicrotaskQueue;
class NativeExecutable;
#if USE(BUN_JSC_ADDITIONS)
class QueuedTask;
enum class InternalMicrotask : uint8_t;
#endif
class Debugger;
class DeferredWorkTimer;
class PinballCompletion;
class RegExp;
class RegExpCache;
class Register;
#if ENABLE(SAMPLING_PROFILER)
class SamplingProfiler;
#endif
class ShadowChicken;
class SharedJITStubSet;
class SourceProvider;
class SourceProviderCache;
enum class SourceTaintedOrigin : uint8_t;
class StackFrame;
class Structure;
class Symbol;
class TypedArrayController;
class VMEntryScope;
class TypeProfiler;
class TypeProfilerLog;
class Watchdog;
class WatchpointSet;
class Waiter;

constexpr bool validateDFGDoesGC = ENABLE_DFG_DOES_GC_VALIDATION;

#if USE(BUN_JSC_ADDITIONS)
using StackTraceAppenderFunction = WTF::Function<void(VM&, JSCell* owner, Vector<StackFrame>& stackTrace, size_t maxToAppend)>;
using ErrorInfoFunction = WTF::Function<String(VM&, Vector<StackFrame>& stackTrace, unsigned& line, unsigned& column, String& sourceURL, void* bunErrorData)>;
using ErrorInfoFunctionJSValue = WTF::Function<JSValue(VM&, Vector<StackFrame>& stackTrace, unsigned& line, unsigned& column, String& sourceURL, JSC::JSObject*, void* bunErrorData)>;
#endif

#if ENABLE(FTL_JIT)
namespace FTL {
class Thunks;
}
#endif // ENABLE(FTL_JIT)
namespace Profiler {
class Database;
}
namespace DOMJIT {
class Signature;
}

#if ENABLE(WEBASSEMBLY)
class JSWebAssemblyInstance;
class WebAssemblyGCStructure;
namespace Wasm {
class IPIntCallee;
class RTT;
#if ENABLE(WEBASSEMBLY_DEBUGGER)
struct DebugState;
#endif
}
#endif

struct EntryFrame;

typedef uint8_t IndexingType;

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(VM);

struct ScratchBuffer {
    ScratchBuffer()
    {
        u.m_activeLength = 0;
    }

    static ScratchBuffer* create(size_t size)
    {
        ScratchBuffer* result = new (VMMalloc::malloc(ScratchBuffer::allocationSize(size))) ScratchBuffer;
        return result;
    }

    static ScratchBuffer* fromData(void* buffer)
    {
        return std::bit_cast<ScratchBuffer*>(static_cast<char*>(buffer) - OBJECT_OFFSETOF(ScratchBuffer, m_buffer));
    }

    static size_t allocationSize(Checked<size_t> bufferSize) { return sizeof(ScratchBuffer) + bufferSize; }
    void setActiveLength(size_t activeLength) { u.m_activeLength = activeLength; }
    size_t activeLength() const { return u.m_activeLength; };
    size_t* addressOfActiveLength() { return &u.m_activeLength; };
    void* dataBuffer() { return m_buffer; }

    union {
        size_t m_activeLength;
        double pad; // Make sure m_buffer is double aligned.
    } u;
    void* m_buffer[0];
};

class ActiveScratchBufferScope {
public:
    ActiveScratchBufferScope(ScratchBuffer*, size_t activeScratchBufferSizeInJSValues);
    ~ActiveScratchBufferScope();

private:
    ScratchBuffer* m_scratchBuffer;
};

enum VMIdentifierType { };
using VMIdentifier = AtomicObjectIdentifier<VMIdentifierType>;

class VM : public ThreadSafeRefCountedWithSuppressingSaferCPPChecking<VM> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(VM, VM);
public:
    // WebCore has a one-to-one mapping of threads to VMs;
    // create() should only be called once
    // on a thread, this is the 'default' VM (it uses the
    // thread's default string uniquing table from Thread::currentSingleton()).
    enum class VMType { Default, APIContextGroup };

    struct ClientData {
        JS_EXPORT_PRIVATE virtual ~ClientData() { };

        JS_EXPORT_PRIVATE virtual String overrideSourceURL(const StackFrame&, const String& originalSourceURL) const = 0;

        virtual bool isWebCoreJSClientData() const { return false; }
    };

    bool usingAPI() { return vmType != VMType::Default; }

    JS_EXPORT_PRIVATE static Ref<VM> create(HeapType = HeapType::Small, WTF::RunLoop* = nullptr);
    JS_EXPORT_PRIVATE static RefPtr<VM> tryCreate(HeapType = HeapType::Small, WTF::RunLoop* = nullptr);
    static Ref<VM> createContextGroup(HeapType = HeapType::Small);
    JS_EXPORT_PRIVATE ~VM();

    Watchdog* watchdog() { return m_watchdog.getIfExists(); }
    Watchdog& ensureWatchdog() { return m_watchdog.get(*this); }

    HeapProfiler* heapProfiler() { return m_heapProfiler.getIfExists(); }
    HeapProfiler& ensureHeapProfiler() { return m_heapProfiler.get(*this); }

    // AUD1.N2 / K4.II.12 (both BINDING): adaptive-search scratch tables are
    // per-VM match scratch — mutated (bad-char shift tables) by every
    // AdaptiveStringSearcher use, including the RegExp::matchInline atom fast
    // path. RULED contents per-lite; GIL-off this routes to a per-thread
    // (== per-lite for scratch, ISB1 precedent) instance — same mode-split
    // shape as RegExp::ovectorSpan(VM&). The LazyUniqueRef::get lazy-init
    // also has no concurrent-arbitration story (its initializingTag ASSERT
    // fires under racing first touches), so GIL-off must not reach it at
    // all. Flag-off/GIL-on byte-identical: one predicted-false byte test.
    // No tier bakes the tables' address (all consumers are C++ operations).
    WTF::AdaptiveStringSearcherTables& adaptiveStringSearcherTables()
    {
        // AB17d (bench I3 follow-up): Config-page gate, not the raw member
        // load — this is on every adaptive string search flag-off.
        if (gilOffWithProcessGate()) [[unlikely]]
            return gilOffPerThreadStringSearcherTables();
        return m_stringSearcherTables.get(*this);
    }
    JS_EXPORT_PRIVATE static WTF::AdaptiveStringSearcherTables& gilOffPerThreadStringSearcherTables();

    // AUD1 / K4.II.1 (BINDING): numericStrings is a per-VM number->string
    // value cache mutated on ordinary JS paths (jsAdd string concat,
    // toString, JSON, joins). Its entries reassign String members on every
    // colliding miss, so a foreign thread reading entry.value while the
    // owner reassigns it is a StringImpl use-after-free; the lazy
    // lookupSmallString fill has the same double-init shape. RULED per-lite
    // copy; GIL-off this routes to a per-thread instance (same mode-split
    // shape as adaptiveStringSearcherTables() above / RegExp::ovectorSpan).
    // The per-thread instance never caches JSString* (no visitAggregate
    // walk reaches it — see disableJSStringCaching), so the DFG/FTL
    // NumberToString fast path, which bakes the MAIN instance's
    // smallIntCache address into shared code, keeps hitting only the
    // immortal 0-9 entries written once in the VM ctor (initializeSmallIntCache,
    // pre-spawn) and misses to the routed slow call for everything else:
    // under GIL-off the main instance is never written on a JS path again.
    // Flag-off/GIL-on byte-identical: one predicted-false Config-page test.
    ALWAYS_INLINE NumericStrings& liveNumericStrings()
    {
        if (gilOffWithProcessGate()) [[unlikely]]
            return gilOffPerThreadNumericStrings();
        return numericStrings;
    }
    JS_EXPORT_PRIVATE static NumericStrings& gilOffPerThreadNumericStrings();

    bool isAnalyzingHeap() const { return m_activeHeapAnalyzer; }
    HeapAnalyzer* activeHeapAnalyzer() const { return m_activeHeapAnalyzer; }
    void setActiveHeapAnalyzer(HeapAnalyzer* analyzer) { m_activeHeapAnalyzer = analyzer; }

#if ENABLE(SAMPLING_PROFILER)
    SamplingProfiler* samplingProfiler() { return m_samplingProfiler.get(); }
    JS_EXPORT_PRIVATE SamplingProfiler& ensureSamplingProfiler(Ref<WTF::Stopwatch>&&);

    JS_EXPORT_PRIVATE void enableSamplingProfiler();
    JS_EXPORT_PRIVATE void disableSamplingProfiler();
    JS_EXPORT_PRIVATE RefPtr<JSON::Value> takeSamplingProfilerSamplesAsJSON();
#endif

    FuzzerAgent* fuzzerAgent() const LIFETIME_BOUND { return m_fuzzerAgent.get(); }
    void setFuzzerAgent(std::unique_ptr<FuzzerAgent>&&);

    VMIdentifier identifier() const { return m_identifier; }

    // UNGIL §0 U0c (ANNEX U0C): computed ONCE at the top of the VM ctor —
    // before m_mainVMLite registration, any entry, any codegen — and
    // IMMUTABLE for the VM's lifetime. True iff this VM won the
    // sticky-shared-server designation under gilOffProcess: the ONE VM per
    // process (U0b) whose threads run without the GIL. Every unqualified
    // "gilOff" predicate in SPEC-ungil means THIS member (level (ii) of the
    // §A.1.3 two-level discriminator; r27/TERM1.4), not the process byte.
    bool gilOff() const { return m_gilOff; }

    // C++-side equivalent of the derived JSCConfig gilOffProcess byte
    // (§A.1.3 level (i); the Config byte itself + the LLInt consumer land
    // with U-T3 and MUST stay derivation-identical to this). U0 option
    // validation forces useThreadGIL=1 unless the trio is on, so the
    // conjunction is the canonical derivation.
    JS_EXPORT_PRIVATE static bool isGILOffProcess();

    // ANNEX A36: process-monotonic VM epoch; the per-thread VM->carrier TLS
    // map stores {VM*, epoch, carrier} and compares epochs BEFORE trusting a
    // cached carrier (stale epoch => the VM at this address died; never a
    // dangling lite/client).
    uint64_t vmEpoch() const { return m_vmEpoch; }

    // UNGIL §A.1.5: GIL-on this is the landed "!!entryScope". GIL-off the
    // per-entry record is per-lite and "entered" means the §A.3.1 entered
    // set is non-empty; until U-T5 builds that set, the equivalent form is
    // "any registered same-VM lite has a live entry scope" (registry walk
    // under its lock — VM-wide consumers iterate the registry, §A.1.5).
    bool isEntered() const
    {
        if (gilOffWithProcessGate()) [[unlikely]]
            return isAnyThreadEntered();
        return !!entryScope;
    }
    JS_EXPORT_PRIVATE bool isAnyThreadEntered() const;

    // §A.1.5: the CURRENT thread's entry scope — per-lite when GIL-off, the
    // VM member otherwise. Null off-thread / when not entered.
    VMEntryScope* currentThreadEntryScope()
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            VMLite* lite = VMLite::currentIfExists();
            return (lite && lite->vm == this) ? lite->entryScope.load(std::memory_order_relaxed) : nullptr;
        }
        return entryScope;
    }

    inline CallFrame* topJSCallFrame() const;

    // Global object in which execution began.
    JS_EXPORT_PRIVATE JSGlobalObject* NODELETE deprecatedVMEntryGlobalObject(JSGlobalObject*) const;

    WeakRandom& random() LIFETIME_BOUND { return m_random; }
    WeakRandom& heapRandom() LIFETIME_BOUND { return m_heapRandom; }
    Integrity::Random& integrityRandom() LIFETIME_BOUND { return m_integrityRandom; }

    template<typename Type, typename Functor>
    Type& ensureSideData(void* key, const Functor&);

    bool hasTerminationRequest() const { return m_hasTerminationRequest.load(std::memory_order_relaxed); }
    void clearHasTerminationRequest()
    {
        m_hasTerminationRequest.store(false, std::memory_order_relaxed);
        clearEntryScopeService(ConcurrentEntryScopeService::ResetTerminationRequest);
    }
    void NODELETE setHasTerminationRequest();

    // UNGIL review fix: GIL-off, executionForbidden() is read from spawned
    // threads (VM::drainMicrotasks' per-lite arm, completion drains) while
    // the latch is written by whichever thread services termination — a
    // cross-thread shared flag. Relaxed atomics: it is a sticky monotonic
    // latch (false -> true, never reset), so no ordering is load-bearing;
    // GIL-on/flag-off codegen is unchanged on every supported target.
    bool executionForbidden() const { return m_executionForbidden.load(std::memory_order_relaxed); }
    void setExecutionForbidden() { m_executionForbidden.store(true, std::memory_order_relaxed); }

    static JS_EXPORT_PRIVATE JSValue checkVMEntryPermission();

    // Setting this means that the VM can never recover from a TerminationException.
    // Currently, we'll only set this for worker threads. Ideally, we want this
    // to always be true. However, we're only limiting it to workers for now until
    // we can be sure that clients using the JSC watchdog (which uses termination)
    // isn't broken by this change.
    void forbidExecutionOnTermination() { m_executionForbiddenOnTermination = true; }

    JS_EXPORT_PRIVATE Exception* ensureTerminationException();
    Exception* terminationException() const
    {
        // THREADS: relaxed atomic — published once under m_terminationExceptionLock
        // in ensureTerminationException(); pointer identity is all readers need.
        Exception* exception = WTF::atomicLoad(const_cast<Exception**>(&m_terminationException), std::memory_order_relaxed);
        ASSERT(exception);
        return exception;
    }
    bool isTerminationException(Exception* exception) const
    {
        ASSERT(exception);
        return exception == WTF::atomicLoad(const_cast<Exception**>(&m_terminationException), std::memory_order_relaxed);
    }
    bool hasPendingTerminationException() const
    {
        // UNGIL §A.1.3: Group-3 exception state is per-lite when gilOff.
        // tsan-vm-setexception-cross-thread-r3: under GIL'd useJSThreads
        // (m_gilOff == 0 — U0 validation forces useThreadGIL=1 without the
        // unsafe trio) this word is the shared VM block, and this predicate
        // is the one sanctioned lock-free reader: jsc's runJSC result check
        // runs after its JSLockHolder scope closes while a spawned thread
        // may still be throwing under its own lock acquisition. Relaxed
        // atomic load — codegen-identical to a plain load on all supported
        // targets — makes the racing access tear-free. Sound at relaxed
        // ordering because we only pointer-compare against the immutable,
        // pre-created termination exception and never dereference; every
        // dereferencing reader still holds the JSLock (SPEC-vmstate I15;
        // carve-out for this lock-free, non-dereferencing predicate is
        // pending the spec's next revision).
        Exception* exception = WTF::atomicLoad(&const_cast<VM*>(this)->group3Primitives().m_exception, std::memory_order_relaxed);
        return exception && isTerminationException(exception);
    }

    void throwTerminationException();

    enum class EntryScopeService : uint8_t {
        // Sticky services i.e. if set, these will never be cleared.
        SamplingProfiler = 1 << 0,
        TracePoints = 1 << 1,
        Watchdog = 1 << 2,

        // Transient services i.e. these will be cleared after they are serviced once, and can be set again later.
        ClearScratchBuffers = 1 << 3,
        FirePrimitiveGigacageEnabled = 1 << 4,
        PopListeners = 1 << 5,
    };

    // FIXME rdar://161576886
    // It is evident that code can be made simpler and more efficient by combining the bits of
    // ConcurrentEntryScopeServices and VMTraps. Some of them (e.g. NeedStopTheWorld) overlap.
    // However, combining them will require some filtering so that only the right bits are
    // checked at the right place. We'll fix this in a later patch.
    enum class ConcurrentEntryScopeService : uint8_t {
        // Transient services i.e. these will be cleared after they are serviced once, and can be set again later.
        ResetTerminationRequest = 1 << 0,
        NeedStopTheWorld = 1 << 1, // FIXME rdar://161576886
    };

    // UNGIL §A.1.5 service routing (U-T1; mirrors §A.2.3). GIL-on/flag-off:
    // unchanged — the VM-level word is the only storage. GIL-off, services
    // classify VM-wide vs thread-local (the U-T1 table; mirrored in
    // INTEGRATE-ungil.md):
    //   EntryScopeService::SamplingProfiler            VM-wide (sticky)
    //   EntryScopeService::TracePoints                 VM-wide (sticky)
    //   EntryScopeService::Watchdog                    VM-wide (sticky; per-lite delivery = U-T2/annex W)
    //   EntryScopeService::ClearScratchBuffers         THREAD-LOCAL (scratch is per-lite, §A.1.6)
    //   EntryScopeService::FirePrimitiveGigacageEnabled VM-wide (§F.2's gigacage-disable deferred arm, named in §A.1.5)
    //   EntryScopeService::PopListeners                VM-wide (listener list is VM state)
    //   ConcurrentEntryScopeService::*                 VM-wide (CONCURRENT_SAFE; requester may hold NO lite)
    // VM-wide GIL-off requests set the VM-level word AND fan into this VM's
    // registered lites under the registry lock; carrier registration
    // backfills the VM-level word into new lites. Thread-local requests (and
    // all GIL-off servicing/clearing) use the CURRENT lite's bits.
    static constexpr bool isThreadLocalEntryScopeService(EntryScopeService service)
    {
        return service == EntryScopeService::ClearScratchBuffers;
    }

    bool hasAnyEntryScopeServiceRequest()
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            if (std::atomic<uint16_t>* bits = currentLiteEntryScopeServiceBits())
                return bits->load(std::memory_order_relaxed);
        }
        return m_entryScopeServicesRawBits;
    }

    // §A.1.5 registration backfill: copies the VM-level service word into a
    // freshly registered lite's bits (so VM-wide requests made before this
    // thread's first entry are serviced by it too). Caller: the GIL-off
    // carrier/spawn registration sites, after VMLiteRegistry::registerLite.
    JS_EXPORT_PRIVATE void backfillEntryScopeServiceBitsForLiteRegistration(VMLite&);
    void executeEntryScopeServicesOnEntry();
    void executeEntryScopeServicesOnExit();

    void requestEntryScopeService(EntryScopeService service)
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            if (isThreadLocalEntryScopeService(service)) {
                std::atomic<uint16_t>* bits = currentLiteEntryScopeServiceBits();
                RELEASE_ASSERT(bits); // Thread-local services are requested by an entered thread.
                bits->fetch_or(packedServiceBits(service), std::memory_order_relaxed);
            } else
                requestVMWideEntryScopeService(service);
            return;
        }
        entryScopeServices().add(service);
    }
    CONCURRENT_SAFE void requestEntryScopeService(ConcurrentEntryScopeService service)
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            requestVMWideEntryScopeService(service);
            return;
        }
        concurrentEntryScopeServices().add(service);
    }

    enum class SchedulerOptions : uint8_t {
        HasImminentlyScheduledWork = 1 << 0,
    };
    JS_EXPORT_PRIVATE void performOpportunisticallyScheduledTasks(MonotonicTime deadline, OptionSet<SchedulerOptions>);

    Structure* cellButterflyStructure(IndexingType indexingType) { return rawImmutableButterflyStructure(indexingType).get(); }

    // Keep super frequently accessed fields top in VM.
    //
    // GIL-off reroute (same mode-split shape as adaptiveStringSearcherTables()
    // above / RegExp::ovectorSpan(VM&)): disallowVMEntryCount guards the
    // CURRENT THREAD's stack state (VMInquiry property slots, debug
    // ObjectInitializationScope) against reentry ON THAT THREAD — per-thread
    // execution state by meaning (K4 §II.15/.16 class). GIL-off, a shared
    // counter makes thread A's inquiry/init scope fail-stop thread B's
    // CachedCall / vmEntryToJavaScript / microtask checkpoint
    // (VM::checkVMEntryPermission, observed in the AB17c bring-up). The slot
    // accessor routes GIL-off to a per-thread count; GIL-on/flag-off it is
    // byte-identically the member. The member was renamed so any direct
    // spelling is a compile error (keystone pattern); no .asm/JIT reader
    // exists (verified: C++ sites only).
    ALWAYS_INLINE unsigned& disallowVMEntryCountSlot()
    {
        // AB17d (bench I3 follow-up): Config-page gate, not the raw member load.
        if (gilOffWithProcessGate()) [[unlikely]]
            return gilOffPerThreadDisallowVMEntryCount();
        return m_disallowVMEntryCountSharedGILOn;
    }
    JS_EXPORT_PRIVATE static unsigned& gilOffPerThreadDisallowVMEntryCount();
    unsigned m_disallowVMEntryCountSharedGILOn { 0 };
private:
    // SPEC-vmstate §6.3 relocated member: cross-thread by design, deliberately
    // NOT in VMLitePrimitives. Kept just outside the block; name/type/sites
    // unchanged.
    Exception* m_terminationException { nullptr }; // Guarded by m_terminationExceptionLock for creation; relaxed-atomic reads.
    Lock m_terminationExceptionLock;
    Lock m_softReservedZoneSizeLock; // Serializes updateSoftReservedZoneSize's read-modify-write (ErrorHandlingScope save/restore from N Threads).
public:
    // NOTE: When throwing an exception while rolling back the call frame,
    // callFrameForCatch may be equal to topEntryFrame.
    // FIXME: callFrameForCatch should be a void*, because it might not point
    // to a CallFrame. https://bugs.webkit.org/show_bug.cgi?id=160441
    // topCallFrame/topEntryFrame are sometimes treated as a pair in assembly
    // code, making usages of the second one implicit. To find them, look for
    // loadpairq/storepairq of "VM::topCallFrame" in *.asm files.
    //
    // SPEC-vmstate §6.4(1) (M6): VM's Group 1-3 members are declared by
    // expanding the SAME X-macro as VMLitePrimitives (VMLite.h), under this
    // ONE public: label (frozen). Names are unchanged, so every existing
    // spelling (C++, offset fns, asserts, .asm) compiles unchanged; the
    // per-field equivalence asserts below the class pin the two layouts
    // together. Freeze rules L1-L5 (§6.3) apply: do NOT add, remove, or
    // reorder fields here — change FOR_EACH_VMLITE_PRIMITIVE_FIELD (spec
    // revision) or declare new members outside this block.
#define VM_DECLARE_VMLITE_PRIMITIVE_FIELD(type, name) type name { };
    FOR_EACH_VMLITE_PRIMITIVE_FIELD(VM_DECLARE_VMLITE_PRIMITIVE_FIELD)
#undef VM_DECLARE_VMLITE_PRIMITIVE_FIELD

    // SPEC-vmstate §6.4(3): VM doubles as the main thread's physical
    // VMLitePrimitives. Guarded by the equivalence asserts below the class.
    // Consumed by tests and as the GIL-on / flag-off arm of
    // group3Primitives() (the per-lite Group-3 storage selector).
    ALWAYS_INLINE VMLitePrimitives& mainVMLitePrimitives()
    {
        return *std::bit_cast<VMLitePrimitives*>(std::bit_cast<uint8_t*>(this) + OBJECT_OFFSETOF(VM, topCallFrame));
    }

    // UNGIL §A.1.3 (U-T1): the mode-split Group-3 storage selector every
    // same-name VM accessor routes through. GIL-on (m_gilOff == 0, incl.
    // flag-off): the VM member block — bit-identical to today. GIL-off: the
    // CURRENT thread's lite (rematerialized per use, §A.1.2). The VM-block
    // fallback below covers the windows where the GIL-off VM legitimately
    // has no installed same-VM lite on this thread (ctor tail before first
    // entry, ~VM tail after uninstall, the §F.5 nested-foreign-VM window for
    // VM-A accessors — where A's state must not be read anyway): the VM
    // block is inert spare storage there. Recorded in INTEGRATE-ungil.md.
    // NOTE: emission-side selection (LLInt/Baseline/DFG/FTL) does NOT use
    // this — it selects AT CODEGEN TIME on the compiled-for VM's mode
    // (U-T3/U-T4); this helper is the C++ accessor carrier only.
    // Bench item I3 (flag-off residual on transition-heavy-constructor):
    // keep the hot C++ Group-3 accessors (frame tracers, exception checks,
    // stack-limit checks) off the VM cache line that also carries the
    // concurrently-mutated m_entryScopeServicesRawBits word, by deciding on
    // the frozen, read-only Config page and only touching m_gilOff when the
    // process can actually be GIL-off.
    // MECHANISM CAVEAT (AB17d review): the original false-sharing story —
    // "every CONCURRENT_SAFE service fetch_or from a collector/watchdog
    // thread turned the next predicate load into a coherence miss" — is NOT
    // substantiated for the plain flag-off single-threaded bench
    // configuration: the known cross-thread fetch_or writers (Watchdog,
    // NeedStopTheWorld, ResetTerminationRequest) are all inactive there, and
    // the mutator's own non-atomic service writes cannot false-share with
    // itself. The change is justified on the weaker ground of a layout
    // trade, NOT a static op-count reduction (AB17e correction: the earlier
    // "strictly cost-reducing" wording was wrong) — what it buys is that the
    // load hits the frozen read-only Config page instead of a VM line that
    // can carry concurrently-mutated state. As of AB17g item 2 (F1) the
    // gilOffProcess byte is latched in the VM ctor strictly BEFORE the
    // m_gilOff designation (Config::latchGILOffProcess(), VM.cpp), so the
    // former useJSThreads fallback term has been DROPPED: flag-off this
    // predicate is a SINGLE predicted-false byte test on the Config page.
    // The remaining gilOffWithProcessGate() callers are the C++ Group-3
    // accessors (exception checks, frame tracers, soft-stack-limit paths)
    // plus CodeBlock/ScriptExecutable/RegExp/JITOperations paths; the
    // bench-hot Heap::allocationClientForCurrentThread site is NOT one of
    // them — FIX-V5B-F1 already gates it on a single
    // options.useJSThreads byte test (heap/Heap.h). The coherence-miss
    // mechanism IS real in flag-on configurations that have remote writers
    // (GIL'd useJSThreads watchdog / stop requests), which is also why the
    // word is now line-isolated by padding (see
    // m_entryScopeServicesPadBefore/After). Do not cite this comment as
    // evidence the flag-off regression class is closed; the bench gate
    // adjudicates that (Tools/threads/bench-gate.sh, AB17c F1 ledger
    // entry).
    //
    // Equivalence invariant (MUST hold in every reachable state): this
    // predicate returns exactly m_gilOff.
    // Invariant: gilOffProcess == 0 implies m_gilOff == 0 in every reachable
    // state on every thread. Proof obligations:
    //  - g_jscConfig.gilOffProcess == 1: we return m_gilOff verbatim.
    //  - g_jscConfig.gilOffProcess == 0: m_gilOff == 0 is forced, because
    //    m_gilOff's SOLE write site (VM ctor designation, VM.cpp) runs
    //    strictly AFTER Config::latchGILOffProcess() in the same ctor, and
    //    that latch sets the byte whenever VM::isGILOffProcess() holds —
    //    the derivations are identical (contract comment in VM.cpp).
    // Reader enumeration (every class of thread that can evaluate this
    // predicate, and the happens-before edge that orders its byte read
    // after the latch store):
    //  (i) The constructing thread itself — including the ctor's own
    //      JSLockHolder, formerly the only window the dropped fallback term
    //      covered: the latch is sequenced-before the designation and
    //      sequenced-before every later ctor statement; program order
    //      closes the window outright.
    //  (ii) Concurrent embedder threads constructing OTHER VMs: each such
    //      ctor calls Config::latchGILOffProcess() before its first gate
    //      evaluation; std::call_once's return edge synchronizes-with the
    //      winning store, so the byte read is ordered after the store.
    //  (iii) Thread()-spawned mutators: pthread_create alone is NOT a
    //      sufficient HB statement here — the edge is the COMPOSITION of
    //      the spawn-handshake publication of the VM*/VMLite with the
    //      ctor-completion edge of the VM being entered: the spawned thread
    //      obtained its VM through a publication that happens-after the
    //      ctor completed, hence after the latch.
    //  (iv) Helper/registry readers (VMLiteRegistry walkers under the
    //      registry lock, JSLock acquirers, worklist/compiler threads via
    //      queue publication): each holds a VM* obtained through a lock or
    //      queue publication edge that happens-after ctor completion, hence
    //      after the latch.
    // In all four cases the byte is write-once strictly-before-any-VM-
    // publication and never written again (call_once + page freeze), so the
    // plain load is race-free.
    ALWAYS_INLINE bool gilOffWithProcessGate() const
    {
        // Single predicted-false byte test on the frozen read-only Config
        // page (F1: this is the hot flag-off cost; see invariant comment
        // above). gilOffProcess is latched in the VM ctor strictly BEFORE
        // any m_gilOff designation (VM.cpp), so gilOffProcess==0 implies
        // m_gilOff==0 in every reachable state on every thread.
        if (g_jscConfig.gilOffProcess) [[unlikely]]
            return m_gilOff;
        return false;
    }

    ALWAYS_INLINE VMLitePrimitives& group3Primitives()
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            VMLite* lite = VMLite::currentIfExists();
            if (lite && lite->vm == this) [[likely]]
                return lite->primitives;
        }
        return mainVMLitePrimitives();
    }
    ALWAYS_INLINE const VMLitePrimitives& group3Primitives() const
    {
        return const_cast<VM*>(this)->group3Primitives();
    }

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // UNGIL obligation 10 (INTEGRATE-ungil.md; the C++ sibling of the
    // LLInt/JIT group-3 exception split): mode-split selector for the
    // EXCEPTION_SCOPE_VERIFICATION bookkeeping. GIL-on (m_gilOff == 0,
    // incl. flag-off and a second GIL-on VM, U0b): the VM member —
    // bit-identical to the single-mutator behavior. GIL-off: the CURRENT
    // lite's copy (VMLite debug-only L2 tail append), so a spawned thread's
    // ExceptionScope chain anchors in ITS OWN storage and never links
    // through the carrier's stack frames.
    //
    // Fallback arm (gilOff, but no installed same-VM lite): legitimate only
    // in the same windows group3Primitives() documents (ctor tail before
    // first entry, ~VM tail after uninstall) — all of which hold m_lock.
    // The assert below keeps a future non-mutator scope user (GC/compiler
    // thread constructing a Throw/CatchScope against this VM) from silently
    // reopening the shared-word race.
    //
    // NOTE: the ExceptionScope chain write-back is NOT idempotent (unlike
    // the group3Primitives precedent) — scopes must live strictly inside a
    // stable (thread, lite) window (see VMExceptionScopeVerificationState.h).
    ALWAYS_INLINE VMExceptionScopeVerificationState& exceptionScopeVerificationState()
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            VMLite* lite = VMLite::currentIfExists();
            if (lite && lite->vm == this) [[likely]]
                return lite->exceptionScopeVerificationState;
            assertExceptionScopeVerificationFallbackArmIsSafe();
        }
        return m_exceptionScopeVerificationState;
    }
    ALWAYS_INLINE const VMExceptionScopeVerificationState& exceptionScopeVerificationState() const
    {
        return const_cast<VM*>(this)->exceptionScopeVerificationState();
    }
    JS_EXPORT_PRIVATE void assertExceptionScopeVerificationFallbackArmIsSafe();
#endif

    // SPEC-vmstate §6.4.4: the main thread's carrier (tid 0); non-null exactly
    // when the VM was constructed with useVMLite on. Consumed by JSLock (M4)
    // and ~VM.
    ALWAYS_INLINE VMLite* mainVMLite() { return m_mainVMLite.get(); }

    // SPEC-vmstate §6.3 relocated members (names/types/sites unchanged):
    void* maybeReturnPC { nullptr };
    JSPIContext* topJSPIContext { nullptr };
private:

    struct EntryScopeServicesBits {
        OptionSet<EntryScopeService> m_entryScopeServices;
        OptionSet<ConcurrentEntryScopeService, ConcurrencyTag::Atomic> m_concurrentEntryScopeServices;
    };

    // AB17d (bench I3 layout, review finding): isolate the concurrently
    // fetch_or'd service word on its own cache line — 64B of never-accessed
    // padding on each side guarantees no other VM member (the hot Group-3
    // VMLitePrimitives tail above; the m_gilOff/didEnterVM byte group below)
    // shares the word's line REGARDLESS of the allocation's base alignment.
    // alignas(64) is deliberately not used: FastMalloc/TZone allocation does
    // not honor over-aligned types. These members sit AFTER the frozen
    // VMLitePrimitives X-macro span (L1-L5), so no spec revision is needed.
    char m_entryScopeServicesPadBefore[64] { };
    uint16_t m_entryScopeServicesRawBits { 0 };
    char m_entryScopeServicesPadAfter[64] { };
    static_assert(sizeof(EntryScopeServicesBits) == sizeof(m_entryScopeServicesRawBits));

    // UNGIL §A.1.5 (U-T1): the same packing, expressed as raw bit positions,
    // for the per-lite atomic word (VMLite::entryScopeServicesRawBits — the
    // lite stores an opaque std::atomic<uint16_t>; VM owns the packing).
    // EntryScopeService occupies byte 0 (EntryScopeServicesBits' first
    // member), ConcurrentEntryScopeService byte 1.
    static constexpr uint16_t packedServiceBits(EntryScopeService service)
    {
        return static_cast<uint16_t>(service);
    }
    static constexpr uint16_t packedServiceBits(ConcurrentEntryScopeService service)
    {
        return static_cast<uint16_t>(static_cast<uint16_t>(service) << 8);
    }

    // Null when no lite is installed or a foreign VM's lite is current
    // (nested-entry window, §F.5).
    std::atomic<uint16_t>* currentLiteEntryScopeServiceBits()
    {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == this)
            return &lite->entryScopeServicesRawBits;
        return nullptr;
    }

    // §A.1.5 VM-wide fan-out (VM.cpp): sets the VM-level word and fans into
    // every registered same-VM lite under the registry lock. The requester
    // may hold NO lite (CONCURRENT_SAFE).
    JS_EXPORT_PRIVATE void requestVMWideEntryScopeService(EntryScopeService);
    JS_EXPORT_PRIVATE CONCURRENT_SAFE void requestVMWideEntryScopeService(ConcurrentEntryScopeService);

    OptionSet<EntryScopeService>& entryScopeServices()
    {
        auto& services = *std::bit_cast<EntryScopeServicesBits*>(&m_entryScopeServicesRawBits);
        return services.m_entryScopeServices;
    }
    OptionSet<ConcurrentEntryScopeService, ConcurrencyTag::Atomic>& concurrentEntryScopeServices()
    {
        auto& services = *std::bit_cast<EntryScopeServicesBits*>(&m_entryScopeServicesRawBits);
        return services.m_concurrentEntryScopeServices;
    }

public:
    // TSAN family microtask-queue (triage §3.30 tail): with useVMLite on,
    // N mutator threads enter/drain the same VM concurrently, so the plain
    // `bool didEnterVM` was a write-write race between executeCallImpl's
    // ScopeExit (and the other vmEntry ScopeExits) and
    // performMicrotaskCheckpoint's post-drain write — same address, both
    // storing `true`. The flag is monotonic clobberize bookkeeping: C++ entry
    // paths set it; only DFG/FTL-generated code tests and clears it
    // (DFGSpeculativeJIT.cpp / FTLLowerDFGToB3.cpp via AbsoluteAddress /
    // OBJECT_OFFSETOF byte ops). Any interleaving of the racing `= true`
    // stores yields the same value, so ordering is irrelevant — the fix is
    // making the C++ accesses relaxed atomics (defined behavior, identical
    // codegen: relaxed byte mov), NOT a lock. Happens-before for consumers is
    // inherited from VM entry itself; the JIT-side byte ops are outside
    // TSAN's view by design (accepted tradeoff, see TSAN-TRIAGE.md).
    // This wrapper keeps the 1-byte layout and address (JIT pokes it
    // directly) while giving every existing `vm.didEnterVM = true` /
    // `if (vm.didEnterVM)` site relaxed-atomic semantics unchanged at the
    // call site. Flag-off behavior and codegen are unchanged.
    struct DidEnterVMFlag {
        ALWAYS_INLINE DidEnterVMFlag& operator=(bool value)
        {
            m_value.store(value, std::memory_order_relaxed);
            return *this;
        }
        ALWAYS_INLINE operator bool() const { return m_value.load(std::memory_order_relaxed); }

        Atomic<bool> m_value { false };
    };
    static_assert(sizeof(DidEnterVMFlag) == sizeof(bool));

    DidEnterVMFlag didEnterVM;

private:
    bool m_isInService { false };
    // UNGIL §0 U0c: see gilOff() above. Written exactly once, at the top of
    // the VM ctor (before any entry/codegen); never cleared (§10D never
    // clears it — it is not heap state).
    bool m_gilOff { false };
    // ANNEX A36 carrier-map staleness epoch; see vmEpoch() above.
    uint64_t m_vmEpoch { 0 };
    RefPtr<CrossTaskToken> m_crossTaskToken;
    VMIdentifier m_identifier;
    const Ref<JSLock> m_apiLock;
    VMThreadContext m_threadContext;
    const Ref<WTF::RunLoop> m_runLoop;

    WeakRandom m_random;
    WeakRandom m_heapRandom;
    Integrity::Random m_integrityRandom;

    // UNGIL §A.1.5: GIL-off, a thread services/clears the bits on ITS lite
    // (every entered thread received VM-wide bits via the fan-out or the
    // registration backfill). The VM-level word stays the GIL-on storage and
    // the GIL-off backfill source; its transient-bit retirement protocol is
    // refined when the trap fan-out lands (U-T2 rule 3; INTEGRATE-ungil.md).
    bool hasEntryScopeServiceRequest(EntryScopeService service)
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            std::atomic<uint16_t>* bits = currentLiteEntryScopeServiceBits();
            return bits && (bits->load(std::memory_order_relaxed) & packedServiceBits(service));
        }
        return entryScopeServices().contains(service);
    }

    bool hasEntryScopeServiceRequest(ConcurrentEntryScopeService service)
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            std::atomic<uint16_t>* bits = currentLiteEntryScopeServiceBits();
            return bits && (bits->load(std::memory_order_relaxed) & packedServiceBits(service));
        }
        return concurrentEntryScopeServices().contains(service);
    }

    void clearEntryScopeService(EntryScopeService service)
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            if (std::atomic<uint16_t>* bits = currentLiteEntryScopeServiceBits())
                bits->fetch_and(static_cast<uint16_t>(~packedServiceBits(service)), std::memory_order_relaxed);
            return;
        }
        entryScopeServices().remove(service);
    }

    void clearEntryScopeService(ConcurrentEntryScopeService service)
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            if (std::atomic<uint16_t>* bits = currentLiteEntryScopeServiceBits())
                bits->fetch_and(static_cast<uint16_t>(~packedServiceBits(service)), std::memory_order_relaxed);
            return;
        }
        concurrentEntryScopeServices().remove(service);
    }

    WriteBarrier<Structure>& rawImmutableButterflyStructure(IndexingType indexingType) { return cellButterflyStructures[arrayIndexFromIndexingType(indexingType) - NumberOfIndexingShapes]; }

public:
    Heap heap;
    GCClient::Heap clientHeap;

    bool isInService() const { return m_isInService; }

    const HeapCellType& cellHeapCellType() { return heap.cellHeapCellType; }
    const JSDestructibleObjectHeapCellType& destructibleObjectHeapCellType() { return heap.destructibleObjectHeapCellType; };

#if ENABLE(JIT)
    std::unique_ptr<JITSizeStatistics> jitSizeStatistics;
#endif
    
    ALWAYS_INLINE CompleteSubspace& primitiveGigacageAuxiliarySpace() { return heap.primitiveGigacageAuxiliarySpace; }
    ALWAYS_INLINE CompleteSubspace& auxiliarySpace() { return heap.auxiliarySpace; }
    ALWAYS_INLINE CompleteSubspace& immutableButterflyAuxiliarySpace() { return heap.immutableButterflyAuxiliarySpace; }
    ALWAYS_INLINE CompleteSubspace& gigacageAuxiliarySpace(Gigacage::Kind kind) { return heap.gigacageAuxiliarySpace(kind); }
    ALWAYS_INLINE CompleteSubspace& cellSpace() { return heap.cellSpace; }
    ALWAYS_INLINE CompleteSubspace& destructibleObjectSpace() { return heap.destructibleObjectSpace; }
#if ENABLE(WEBASSEMBLY)
    template<SubspaceAccess mode>
    ALWAYS_INLINE GCClient::PreciseSubspace* webAssemblyInstanceSpace() { return heap.webAssemblyInstanceSpace<mode>(); }
#endif

// SPEC-ungil §B / I4: route through the CURRENT thread's client GIL-off
// (Heap::allocationClientForCurrentThread is identity GIL-on/flag-off).
#define DEFINE_ISO_SUBSPACE_ACCESSOR(name, heapCellType, type) \
    ALWAYS_INLINE GCClient::IsoSubspace& name() { return Heap::allocationClientForCurrentThread(*this, clientHeap).name; }

    FOR_EACH_JSC_ISO_SUBSPACE(DEFINE_ISO_SUBSPACE_ACCESSOR)
#undef DEFINE_ISO_SUBSPACE_ACCESSOR

// SPEC-ungil §B / I4: allocation-capable lookups route per-thread.
// SubspaceAccess::Concurrently stays on the VM's original client: it is a
// pure pointer read (never materializes the subspace or touches a
// LocalAllocator) issued by UNSTAMPED non-mutator threads (DFG/FTL compiler
// threads), which must not trip the helper's access-owner tripwire — today's
// behavior, per the Heap.cpp apply-scope note's non-mutator re-validation
// clause.
#define DEFINE_DYNAMIC_ISO_SUBSPACE_ACCESSOR_IMPL(name, heapCellType, type) \
    template<SubspaceAccess mode> \
    ALWAYS_INLINE GCClient::IsoSubspace* name() \
    { \
        if constexpr (mode == SubspaceAccess::Concurrently) \
            return clientHeap.name<mode>(); \
        else \
            return Heap::allocationClientForCurrentThread(*this, clientHeap).name<mode>(); \
    }

#define DEFINE_DYNAMIC_ISO_SUBSPACE_ACCESSOR(name) \
    DEFINE_DYNAMIC_ISO_SUBSPACE_ACCESSOR_IMPL(name, unused, unused2)

    FOR_EACH_JSC_DYNAMIC_ISO_SUBSPACE(DEFINE_DYNAMIC_ISO_SUBSPACE_ACCESSOR_IMPL)

    ALWAYS_INLINE GCClient::IsoSubspace& codeBlockSpace() { return Heap::allocationClientForCurrentThread(*this, clientHeap).codeBlockSpace; }

    DEFINE_DYNAMIC_ISO_SUBSPACE_ACCESSOR(evalExecutableSpace)
    DEFINE_DYNAMIC_ISO_SUBSPACE_ACCESSOR(moduleProgramExecutableSpace)

#undef DEFINE_DYNAMIC_ISO_SUBSPACE_ACCESSOR_IMPL
#undef DEFINE_DYNAMIC_ISO_SUBSPACE_GETTER

    ALWAYS_INLINE GCClient::IsoSubspace& functionExecutableSpace() { return Heap::allocationClientForCurrentThread(*this, clientHeap).functionExecutableSpace; }
    ALWAYS_INLINE GCClient::IsoSubspace& programExecutableSpace() { return Heap::allocationClientForCurrentThread(*this, clientHeap).programExecutableSpace; }
    ALWAYS_INLINE GCClient::IsoSubspace& unlinkedFunctionExecutableSpace() { return Heap::allocationClientForCurrentThread(*this, clientHeap).unlinkedFunctionExecutableSpace; }

    VMType vmType;
    // V7: written by N lites' slow paths and by JIT code via the absolute
    // address baked through addressOfMightBeExecutingTaintedCode() (shared
    // code — must stay ONE VM-level byte, see the addressOfCallFrameForCatch
    // comment about not baking a lite's slot into shared code). Relaxed
    // atomic on the C++ side; the byte layout is unchanged for the baked
    // store8.
    std::atomic<bool> m_mightBeExecutingTaintedCode { false };
    static_assert(sizeof(std::atomic<bool>) == sizeof(bool), "baked JIT store8 relies on this");
    static_assert(std::atomic<bool>::is_always_lock_free);
    ClientData* clientData { nullptr };
#if ENABLE(WEBASSEMBLY)
    Wasm::Context wasmContext;
#endif
    WriteBarrier<Structure> structureStructure;
    WriteBarrier<Structure> structureRareDataStructure;
    WriteBarrier<Structure> stringStructure;
    WriteBarrier<Structure> propertyNameEnumeratorStructure;
    WriteBarrier<Structure> getterSetterStructure;
    WriteBarrier<Structure> customGetterSetterStructure;
    WriteBarrier<Structure> domAttributeGetterSetterStructure;
    WriteBarrier<Structure> scopedArgumentsTableStructure;
    WriteBarrier<Structure> apiWrapperStructure;
    WriteBarrier<Structure> nativeExecutableStructure;
    WriteBarrier<Structure> evalExecutableStructure;
    WriteBarrier<Structure> programExecutableStructure;
    WriteBarrier<Structure> functionExecutableStructure;
#if ENABLE(WEBASSEMBLY)
    WriteBarrier<Structure> pinballCompletionStructure;
    WriteBarrier<Structure> webAssemblyCalleeGroupStructure;
    WriteBarrier<Structure> webAssemblyStreamingContextStructure;
#endif
    WriteBarrier<Structure> moduleProgramExecutableStructure;
    WriteBarrier<Structure> slimPromiseReactionStructure;
    WriteBarrier<Structure> fullPromiseReactionStructure;
    WriteBarrier<Structure> jsMicrotaskDispatcherStructure;
    WriteBarrier<Structure> moduleLoaderStructure;
    WriteBarrier<Structure> moduleRegistryEntryStructure;
    WriteBarrier<Structure> moduleLoadingContextStructure;
    WriteBarrier<Structure> moduleLoaderPayloadStructure;
    WriteBarrier<Structure> moduleGraphLoadingStateStructure;
    WriteBarrier<Structure> promiseCombinatorsContextStructure;
    WriteBarrier<Structure> promiseCombinatorsGlobalContextStructure;
    WriteBarrier<Structure> regExpStructure;
    WriteBarrier<Structure> symbolStructure;
    WriteBarrier<Structure> symbolTableStructure;
    std::array<WriteBarrier<Structure>, NumberOfCopyOnWriteIndexingModes> cellButterflyStructures;
    WriteBarrier<Structure> cellButterflyOnlyAtomStringsStructure;
    WriteBarrier<Structure> sourceCodeStructure;
    WriteBarrier<Structure> structureChainStructure;
    WriteBarrier<Structure> sparseArrayValueMapStructure;
    WriteBarrier<Structure> templateObjectDescriptorStructure;
    WriteBarrier<Structure> unlinkedFunctionExecutableStructure;
    WriteBarrier<Structure> unlinkedProgramCodeBlockStructure;
    WriteBarrier<Structure> unlinkedEvalCodeBlockStructure;
    WriteBarrier<Structure> unlinkedFunctionCodeBlockStructure;
    WriteBarrier<Structure> unlinkedModuleProgramCodeBlockStructure;
    WriteBarrier<Structure> propertyTableStructure;
    WriteBarrier<Structure> functionRareDataStructure;
    WriteBarrier<Structure> exceptionStructure;
    WriteBarrier<Structure> programCodeBlockStructure;
    WriteBarrier<Structure> moduleProgramCodeBlockStructure;
    WriteBarrier<Structure> evalCodeBlockStructure;
    WriteBarrier<Structure> functionCodeBlockStructure;
    WriteBarrier<Structure> hashMapBucketSetStructure;
    WriteBarrier<Structure> hashMapBucketMapStructure;
    WriteBarrier<Structure> bigIntStructure;

    WriteBarrier<JSPropertyNameEnumerator> m_emptyPropertyNameEnumerator;
    WriteBarrier<NativeExecutable> m_promiseResolvingFunctionResolveExecutable;
    WriteBarrier<NativeExecutable> m_promiseResolvingFunctionRejectExecutable;
    WriteBarrier<NativeExecutable> m_promiseFirstResolvingFunctionResolveExecutable;
    WriteBarrier<NativeExecutable> m_promiseFirstResolvingFunctionRejectExecutable;
    WriteBarrier<NativeExecutable> m_promiseResolvingFunctionResolveWithInternalMicrotaskExecutable;
    WriteBarrier<NativeExecutable> m_promiseResolvingFunctionRejectWithInternalMicrotaskExecutable;
    WriteBarrier<NativeExecutable> m_promiseCapabilityExecutorExecutable;
    WriteBarrier<NativeExecutable> m_promiseAllFulfillFunctionExecutable;
    WriteBarrier<NativeExecutable> m_promiseAllSlowFulfillFunctionExecutable;
    WriteBarrier<NativeExecutable> m_promiseAllSettledFulfillFunctionExecutable;
    WriteBarrier<NativeExecutable> m_promiseAllSettledRejectFunctionExecutable;
    WriteBarrier<NativeExecutable> m_promiseAllSettledSlowFulfillFunctionExecutable;
    WriteBarrier<NativeExecutable> m_promiseAllSettledSlowRejectFunctionExecutable;
    WriteBarrier<NativeExecutable> m_promiseAnyRejectFunctionExecutable;
    WriteBarrier<NativeExecutable> m_promiseAnySlowRejectFunctionExecutable;

    WriteBarrier<JSCell> m_orderedHashTableDeletedValue;
    WriteBarrier<JSCell> m_orderedHashTableSentinel;

    WriteBarrier<Structure> m_sentinelStructure;
    WriteBarrier<JSSentinel> m_fastArrayValuesSentinel;
    WriteBarrier<JSSentinel> m_fastArrayKeysSentinel;
    WriteBarrier<JSSentinel> m_fastArrayEntriesSentinel;
    WriteBarrier<JSSentinel> m_fastMapKeysSentinel;
    WriteBarrier<JSSentinel> m_fastMapValuesSentinel;
    WriteBarrier<JSSentinel> m_fastMapEntriesSentinel;
    WriteBarrier<JSSentinel> m_fastSetValuesSentinel;
    WriteBarrier<JSSentinel> m_fastSetEntriesSentinel;
    WriteBarrier<JSSentinel> m_fastStringValuesSentinel;

    WriteBarrier<JSCell> m_cachedSortScratch;
    WriteBarrier<JSCell> m_sortScratchSentinel;

    WriteBarrier<NativeExecutable> m_fastCanConstructBoundExecutable;
    WriteBarrier<NativeExecutable> m_slowCanConstructBoundExecutable;

    Weak<NativeExecutable> m_fastRemoteFunctionExecutable;
    Weak<NativeExecutable> m_slowRemoteFunctionExecutable;

    const Ref<DeferredWorkTimer> deferredWorkTimer;

    JSCell* currentlyDestructingCallbackObject { nullptr };
    const ClassInfo* currentlyDestructingCallbackObjectClassInfo { nullptr };

    AtomStringTable* m_atomStringTable;
    const UniqueRef<WTF::SymbolRegistry> m_symbolRegistry;
    const UniqueRef<WTF::SymbolRegistry> m_privateSymbolRegistry;
    CommonIdentifiers* propertyNames { nullptr };
    const ArgList* emptyList;
    SmallStrings smallStrings;
    NumericStrings numericStrings;
    std::unique_ptr<SimpleStats> machineCodeBytesPerBytecodeWordForBaselineJIT;
    WriteBarrier<JSString> lastCachedString;
    Ref<StringImpl> lastAtomizedIdentifierStringImpl { *StringImpl::empty() };
    Ref<AtomStringImpl> lastAtomizedIdentifierAtomStringImpl { *static_cast<AtomStringImpl*>(StringImpl::empty()) };
    JSONAtomStringCache jsonAtomStringCache;
    KeyAtomStringCache keyAtomStringCache;
    StringSplitCache stringSplitCache;
    Vector<unsigned> stringSplitIndice;
    StringReplaceCache stringReplaceCache;

    bool mightBeExecutingTaintedCode() const { return m_mightBeExecutingTaintedCode.load(std::memory_order_relaxed); }
    bool* addressOfMightBeExecutingTaintedCode() LIFETIME_BOUND { return reinterpret_cast<bool*>(&m_mightBeExecutingTaintedCode); }
    void setMightBeExecutingTaintedCode(bool value = true) { m_mightBeExecutingTaintedCode.store(value, std::memory_order_relaxed); }

    AtomStringTable* atomStringTable() const { return m_atomStringTable; }
    WTF::SymbolRegistry& symbolRegistry() { return m_symbolRegistry.get(); }
    WTF::SymbolRegistry& privateSymbolRegistry() { return m_privateSymbolRegistry.get(); }

    WriteBarrier<JSBigInt> heapBigIntConstantOne;
    WriteBarrier<JSBigInt> heapBigIntConstantZero;

    // Cached multiplicative inverse for BigInt modulo optimization.
    WriteBarrier<JSBigInt> m_cachedBigIntDivisor;
    WriteBarrier<JSBigInt> m_nextCachedBigIntDivisor;
    Vector<UCPURegister> m_bigIntCachedInverse;
    int m_bigIntDivisorCount { 0 };

    JSCell* orderedHashTableDeletedValue()
    {
        return m_orderedHashTableDeletedValue.get();
    }

    JSCell* orderedHashTableSentinel()
    {
        return m_orderedHashTableSentinel.get();
    }

    Structure* sentinelStructure() { return m_sentinelStructure.get(); }
    JSSentinel* fastArrayValuesSentinel() { return m_fastArrayValuesSentinel.get(); }
    JSSentinel* fastArrayKeysSentinel() { return m_fastArrayKeysSentinel.get(); }
    JSSentinel* fastArrayEntriesSentinel() { return m_fastArrayEntriesSentinel.get(); }
    JSSentinel* fastMapKeysSentinel() { return m_fastMapKeysSentinel.get(); }
    JSSentinel* fastMapValuesSentinel() { return m_fastMapValuesSentinel.get(); }
    JSSentinel* fastMapEntriesSentinel() { return m_fastMapEntriesSentinel.get(); }
    JSSentinel* fastSetValuesSentinel() { return m_fastSetValuesSentinel.get(); }
    JSSentinel* fastSetEntriesSentinel() { return m_fastSetEntriesSentinel.get(); }
    JSSentinel* fastStringValuesSentinel() { return m_fastStringValuesSentinel.get(); }

    inline JSPropertyNameEnumerator* emptyPropertyNameEnumerator();

    inline NativeExecutable* promiseResolvingFunctionResolveExecutable();
    inline NativeExecutable* promiseResolvingFunctionRejectExecutable();
    inline NativeExecutable* promiseFirstResolvingFunctionResolveExecutable();
    inline NativeExecutable* promiseFirstResolvingFunctionRejectExecutable();
    inline NativeExecutable* promiseResolvingFunctionResolveWithInternalMicrotaskExecutable();
    inline NativeExecutable* promiseResolvingFunctionRejectWithInternalMicrotaskExecutable();
    inline NativeExecutable* promiseCapabilityExecutorExecutable();
    inline NativeExecutable* promiseAllFulfillFunctionExecutable();
    inline NativeExecutable* promiseAllSlowFulfillFunctionExecutable();
    inline NativeExecutable* promiseAllSettledFulfillFunctionExecutable();
    inline NativeExecutable* promiseAllSettledRejectFunctionExecutable();
    inline NativeExecutable* promiseAllSettledSlowFulfillFunctionExecutable();
    inline NativeExecutable* promiseAllSettledSlowRejectFunctionExecutable();
    inline NativeExecutable* promiseAnyRejectFunctionExecutable();
    inline NativeExecutable* promiseAnySlowRejectFunctionExecutable();

    WeakGCMap<WTF::SymbolImpl*, Symbol, PtrHash<WTF::SymbolImpl*>> symbolImplToSymbolMap;
    WeakGCMap<StringImpl*, JSString, PtrHash<StringImpl*>> atomStringToJSStringMap;
#if ENABLE(WEBASSEMBLY)
    WeakGCMap<const Wasm::RTT*, WebAssemblyGCStructure, PtrHash<const Wasm::RTT*>> wasmGCStructureMap;
#endif

    enum class DeletePropertyMode {
        // Default behaviour of deleteProperty, matching the spec.
        Default,
        // This setting causes deleteProperty to force deletion of all
        // properties including those that are non-configurable (DontDelete).
        IgnoreConfigurable
    };

    DeletePropertyMode deletePropertyMode()
    {
        return m_deletePropertyMode;
    }

    class DeletePropertyModeScope {
    public:
        DeletePropertyModeScope(VM& vm, DeletePropertyMode mode)
            : m_vm(vm)
            , m_previousMode(vm.m_deletePropertyMode)
        {
            m_vm.m_deletePropertyMode = mode;
        }

        ~DeletePropertyModeScope()
        {
            m_vm.m_deletePropertyMode = m_previousMode;
        }

    private:
        VM& m_vm;
        DeletePropertyMode m_previousMode;
    };

    static JS_EXPORT_PRIVATE bool canUseAssembler();
    static bool isInMiniMode()
    {
        return !Options::useJIT() || Options::forceMiniVMMode();
    }

    static bool useUnlinkedCodeBlockJettisoning()
    {
        return Options::useUnlinkedCodeBlockJettisoning() || isInMiniMode();
    }

    static void computeCanUseJIT();

    SourceProviderCache* addSourceProviderCache(SourceProvider*);
    void clearSourceProviderCaches();

    typedef UncheckedKeyHashMap<RefPtr<SourceProvider>, RefPtr<SourceProviderCache>> SourceProviderCacheMap;
    SourceProviderCacheMap sourceProviderCacheMap;
#if ENABLE(JIT)
    std::unique_ptr<JITThunks> jitStubs;
    MacroAssemblerCodeRef<JITThunkPtrTag> getCTIStub(ThunkGenerator);
    MacroAssemblerCodeRef<JITThunkPtrTag> getCTIStub(CommonJITThunkID);
    std::unique_ptr<SharedJITStubSet> m_sharedJITStubs;
#endif
#if ENABLE(FTL_JIT)
    std::unique_ptr<FTL::Thunks> ftlThunks;
#endif

    NativeExecutable* getHostFunction(NativeFunction, ImplementationVisibility, NativeFunction constructor, const String& name);
    NativeExecutable* getHostFunction(NativeFunction, ImplementationVisibility, Intrinsic, NativeFunction constructor, const DOMJIT::Signature*, const String& name);

    NativeExecutable* getBoundFunction(bool isJSFunction, SourceTaintedOrigin taintedness);
    NativeExecutable* getRemoteFunction(bool isJSFunction);

    CodePtr<JSEntryPtrTag> getCTIInternalFunctionTrampolineFor(CodeSpecializationKind);
    MacroAssemblerCodeRef<JSEntryPtrTag> getCTIThrowExceptionFromCallSlowPath();
    MacroAssemblerCodeRef<JITStubRoutinePtrTag> getCTIVirtualCall(CallMode);

    static constexpr ptrdiff_t exceptionOffset()
    {
        return OBJECT_OFFSETOF(VM, m_exception);
    }

    static constexpr ptrdiff_t offsetOfTopCallFrame()
    {
        return OBJECT_OFFSETOF(VM, topCallFrame);
    }

    static constexpr ptrdiff_t callFrameForCatchOffset()
    {
        return OBJECT_OFFSETOF(VM, callFrameForCatch);
    }

    static constexpr ptrdiff_t topEntryFrameOffset()
    {
        return OBJECT_OFFSETOF(VM, topEntryFrame);
    }

    static constexpr ptrdiff_t offsetOfEncodedHostCallReturnValue()
    {
        return OBJECT_OFFSETOF(VM, encodedHostCallReturnValue);
    }

    static constexpr ptrdiff_t offsetOfHeapBarrierThreshold()
    {
        return OBJECT_OFFSETOF(VM, heap) + OBJECT_OFFSETOF(Heap, m_barrierThreshold);
    }

    static constexpr ptrdiff_t offsetOfHeapMutatorShouldBeFenced()
    {
        return OBJECT_OFFSETOF(VM, heap) + OBJECT_OFFSETOF(Heap, m_mutatorShouldBeFenced);
    }

    static constexpr ptrdiff_t offsetOfTraps()
    {
        return OBJECT_OFFSETOF(VM, m_threadContext) + VMThreadContext::offsetOfTraps();
    }

    static constexpr ptrdiff_t offsetOfTrapsBits()
    {
        return offsetOfTraps() + VMTraps::offsetOfTrapsBits();
    }

    static constexpr ptrdiff_t offsetOfSoftStackLimit()
    {
        return offsetOfTraps() + VMTraps::offsetOfSoftStackLimit();
    }

    ALWAYS_INLINE static VM* fromThreadContext(VMThreadContext* context)
    {
        return std::bit_cast<VM*>(std::bit_cast<uint8_t*>(context) - OBJECT_OFFSETOF(VM, m_threadContext));
    }

    ALWAYS_INLINE VMThreadContext* threadContext() { return &m_threadContext; }

    void clearLastException() { group3Primitives().m_lastException = nullptr; } // UNGIL §A.1.3 mode split.

    // UNGIL §A.1.3 NOTE on addressOf*/offset constants (addressOfException,
    // addressOfLastException, addressOfCallFrameForCatch, exceptionOffset,
    // offsetOfTopCallFrame, ...): these bake VM-block addresses/offsets and
    // are GIL-ON EMISSION HELPERS ONLY. A gilOff-mode compilation must NOT
    // consume them — it emits loadVMLite + VMLitePrimitives offsets instead
    // (codegen-time selection on the compiled-for VM's mode; U-T3/U-T4).
    // They deliberately do NOT branch on m_gilOff: returning the CURRENT
    // lite's slot address would bake one thread's storage into shared code.
    CallFrame** addressOfCallFrameForCatch() { return &callFrameForCatch; }

    JSCell** addressOfException() { return reinterpret_cast<JSCell**>(&m_exception); }

    Exception* lastException() const { return group3Primitives().m_lastException; } // UNGIL §A.1.3 mode split.
    JSCell** addressOfLastException() { return reinterpret_cast<JSCell**>(&m_lastException); }

    // This should only be used for code that wants to check for any pending
    // exception without interfering with Throw/CatchScopes.
    Exception* exceptionForInspection() const { return group3Primitives().m_exception; }

    void setFailNextNewCodeBlock() { m_failNextNewCodeBlock.store(true, std::memory_order_relaxed); }
    bool getAndClearFailNextNewCodeBlock()
    {
        // Fast path is a plain byte load — STRICTLY cheaper flag-off than the
        // old unconditional false-store on every CodeBlock install.
        if (!m_failNextNewCodeBlock.load(std::memory_order_relaxed)) [[likely]]
            return false;
        return m_failNextNewCodeBlock.exchange(false, std::memory_order_relaxed);
    }
    
    // UNGIL §A.1.4: per-entry-token lite fields when gilOff (the L7
    // RELEASE_ASSERT at JSLock.cpp didAcquireLock reads through this, so
    // GIL-off it asserts the LITE's slot empty).
    void* stackPointerAtVMEntry() const { return group3Primitives().m_stackPointerAtVMEntry; }
    void setStackPointerAtVMEntry(void*);

    size_t softReservedZoneSize() const { return WTF::atomicLoad(const_cast<size_t*>(&m_currentSoftReservedZoneSize), std::memory_order_relaxed); } // THREADS: see updateSoftReservedZoneSize().
    size_t updateSoftReservedZoneSize(size_t softReservedZoneSize);
    
    static size_t committedStackByteCount();
    inline bool ensureJSStackCapacityFor(Register* newTopOfStack);

    void* stackLimit() { return group3Primitives().m_stackLimit; } // UNGIL §A.1.3 mode split.
    // UNGIL §A.2.2 (AB-17): GIL-off, the soft stack limit is PER-THREAD
    // state on the current lite — C++ readers must use
    // softStackLimitForCurrentThread() (inline form in VMInlines.h; the
    // out-of-line form below for files that cannot include VMInlines.h).
    // The VM-level word serves no-lite threads, wasm instance mirrors, and
    // the GIL-on/flag-off protocol byte-identically.
    ALWAYS_INLINE void* softStackLimit() const { return traps().softStackLimit(); }
    ALWAYS_INLINE void** addressOfSoftStackLimit() { return traps().addressOfSoftStackLimit(); }
    // Flag-off (and GIL-on) this compiles back to the single
    // traps().softStackLimit() load behind one predictable branch — only the
    // gilOff arm is out-of-line. The previous shape (an unconditional
    // cross-TU JS_EXPORT_PRIVATE call) put a call + branch + load on
    // flag-off hot paths (every RegExp match via YarrMatchingContextHolder,
    // rope resolution, JSON/LiteralParser recursion checks), violating the
    // flag-off-identity charter and implicated in the bench-gate red
    // (transition-heavy-constructor +2.4-3.4%).
    ALWAYS_INLINE void* softStackLimitForCurrentThreadSlow() const
    {
        // AB17d (bench I3 follow-up): this is the comment-documented hottest
        // flag-off consumer of the predicate (every RegExp match, rope
        // resolution, JSON recursion check) — decide on the read-only Config
        // page, not the m_gilOff member that shares a line with the
        // concurrently-written service word.
        if (!gilOffWithProcessGate()) [[likely]]
            return softStackLimit();
        return softStackLimitForCurrentThreadGilOffSlow();
    }
    JS_EXPORT_PRIVATE void* softStackLimitForCurrentThreadGilOffSlow() const;

    inline bool isSafeToRecurseSoft() const;
    bool isSafeToRecurse() const
    {
        return isSafeToRecurse(group3Primitives().m_stackLimit); // UNGIL §A.1.3 mode split.
    }

    void* lastStackTop() { return group3Primitives().m_lastStackTop; } // UNGIL §A.1.3 mode split.
    void NODELETE setLastStackTop(const Thread&);
    
#if ENABLE(C_LOOP)
    ALWAYS_INLINE CLoopStack& cloopStack() { return traps().cloopStack(); }
    ALWAYS_INLINE const CLoopStack& cloopStack() const { return traps().cloopStack(); }
    ALWAYS_INLINE void* cloopStackLimit() { return traps().cloopStackLimit(); }
    ALWAYS_INLINE void* currentCLoopStackPointer() const { return traps().currentCLoopStackPointer(); }
#endif

    // SPEC-vmstate §6.4(1)/M6: the Group-2 exception/unwind members formerly
    // declared here (encodedHostCallReturnValue ... osrExitJumpDestination)
    // moved up into the VMLitePrimitives X-macro block near the top of VM.
    // §6.3 relocated member (kept here; deliberately NOT in VMLitePrimitives):
    RegExp* m_executingRegExp { nullptr };

    // The threading protocol here is as follows:
    // - You can call scratchBufferForSize from any thread.
    // - You can only set the ScratchBuffer's activeLength from the main thread.
    // - You can only write to entries in the ScratchBuffer from the main thread.
    //
    // UNGIL §A.1.6 (annex A16): gilOff, this is the NON-BAKED path and
    // dispatches to the CURRENT lite's table by size-class
    // (VMLite::scratchBufferForSize); GIL-on/flag-off keeps the VM-owned
    // buffers and baked addresses.
    ScratchBuffer* scratchBufferForSize(size_t size);
    void clearScratchBuffers();
    bool isScratchBuffer(void*);

    // ANNEX A16 baked-index path (UNGIL U-T1; consumed by U-T4a/U-T4b
    // gilOff-mode codegen, incl. OSR-exit/calleeSaveRegistersBuffer and the
    // JITCode-RESIDENT buffers): allocates a process-wide
    // ScratchBufferRegistry index of `size` bytes and fans a buffer install
    // to every registered lite of this VM (registration backfills late
    // lites), so a buffer exists at (lite, index) before any code emitted
    // against the index can run. gilOff VMs only.
    JS_EXPORT_PRIVATE unsigned allocateBakedScratchBufferIndex(size_t);

    EncodedJSValue* exceptionFuzzingBuffer(size_t size)
    {
        ASSERT(Options::useExceptionFuzz());
        if (!m_exceptionFuzzBuffer)
            m_exceptionFuzzBuffer = MallocPtr<EncodedJSValue, VMMalloc>::malloc(size);
        return m_exceptionFuzzBuffer.get();
    }

    void gatherScratchBufferRoots(ConservativeRoots&);

    static constexpr unsigned expectedMaxActiveSideStateCount = 4;
    void pushCheckpointOSRSideState(std::unique_ptr<CheckpointOSRExitSideState>&&);
    std::unique_ptr<CheckpointOSRExitSideState> popCheckpointOSRSideState(CallFrame* expectedFrame);
    void popAllCheckpointOSRSideStateUntil(CallFrame* targetFrame);
    bool hasCheckpointOSRSideState() const { return m_checkpointSideState.size(); }
    void scanSideState(ConservativeRoots&) const;

    Interpreter interpreter;
    VMEntryScope* entryScope { nullptr };

    JSObject* stringRecursionCheckFirstObject { nullptr };
    UncheckedKeyHashSet<JSObject*> stringRecursionCheckVisitedObjects;

    DateCache dateCache;

    std::unique_ptr<Profiler::Database> m_perBytecodeProfiler;
    RefPtr<TypedArrayController> m_typedArrayController;
    CrossTaskToken* crossTaskToken() const { return m_crossTaskToken.get(); }
    JS_EXPORT_PRIVATE void setCrossTaskToken(RefPtr<CrossTaskToken>&&);
    std::unique_ptr<RegExpCache> m_regExpCache;
    BumpPointerAllocator m_regExpAllocator;
    ConcurrentJSLock m_regExpAllocatorLock;

    const Ref<CompactTDZEnvironmentMap> m_compactVariableMap;

    LazyUniqueRef<VM, HasOwnPropertyCache> m_hasOwnPropertyCache;
    ALWAYS_INLINE HasOwnPropertyCache* hasOwnPropertyCache() { return m_hasOwnPropertyCache.getIfExists(); }
    HasOwnPropertyCache& ensureHasOwnPropertyCache() { return m_hasOwnPropertyCache.get(*this); }

    LazyUniqueRef<VM, MegamorphicCache> m_megamorphicCache;
    ALWAYS_INLINE MegamorphicCache* megamorphicCache() { return m_megamorphicCache.getIfExists(); }
    MegamorphicCache& ensureMegamorphicCache() { return m_megamorphicCache.get(*this); }

    enum class StructureChainIntegrityEvent : uint8_t {
        Add,
        Remove,
        Change,
        Prototype,
    };
    JS_EXPORT_PRIVATE void invalidateStructureChainIntegrity(StructureChainIntegrityEvent);

#if ENABLE(REGEXP_TRACING)
    using RTTraceList = ListHashSet<RegExp*>;
    RTTraceList m_rtTraceList;
    void addRegExpToTrace(RegExp*);
    JS_EXPORT_PRIVATE void dumpRegExpTrace();
#endif

    bool hasTimeZoneChange() { return dateCache.hasTimeZoneChange(); }

    RegExpCache* regExpCache() LIFETIME_BOUND { return m_regExpCache.get(); }

    bool isCollectorBusyOnCurrentThread() { return heap.currentThreadIsDoingGCWork(); }

#if ENABLE(GC_VALIDATION)
    bool isInitializingObject() const;
    const ClassInfo* initializingObjectClass() const;
    void setInitializingObjectClass(const ClassInfo*);
#endif

    JS_EXPORT_PRIVATE bool currentThreadIsHoldingAPILock() const;

    JS_EXPORT_PRIVATE JSLock& apiLock();
    CodeCache* codeCache() LIFETIME_BOUND { return m_codeCache.get(); }
    IntlCache& intlCache() { return *m_intlCache; }

    JS_EXPORT_PRIVATE void whenIdle(Function<void()>&&);

    JS_EXPORT_PRIVATE void deleteAllCode(DeleteAllCodeEffort);
    JS_EXPORT_PRIVATE void deleteAllLinkedCode(DeleteAllCodeEffort);

    void shrinkFootprintWhenIdle();

    WatchpointSet* ensureWatchpointSetForImpureProperty(UniquedStringImpl*);
    
    // FIXME: Use AtomString once it got merged with Identifier.
    JS_EXPORT_PRIVATE void addImpureProperty(UniquedStringImpl*);
    
    InlineWatchpointSet& primitiveGigacageEnabled() LIFETIME_BOUND { return m_primitiveGigacageEnabled; }

    BuiltinExecutables* builtinExecutables() LIFETIME_BOUND { return m_builtinExecutables.get(); }

    bool enableTypeProfiler();
    bool disableTypeProfiler();
    TypeProfilerLog* typeProfilerLog() LIFETIME_BOUND { return m_typeProfilerLog.get(); }
    TypeProfiler* typeProfiler() LIFETIME_BOUND { return m_typeProfiler.get(); }
    JS_EXPORT_PRIVATE void dumpTypeProfilerData();

    FunctionHasExecutedCache* functionHasExecutedCache() LIFETIME_BOUND { return &m_functionHasExecutedCache; }

    ControlFlowProfiler* controlFlowProfiler() LIFETIME_BOUND { return m_controlFlowProfiler.get(); }
    bool enableControlFlowProfiler();
    bool disableControlFlowProfiler();

#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE void queueMicrotask(QueuedTask&&);
#endif
    class JS_EXPORT_PRIVATE DrainMicrotaskDelayScope {
    public:
        explicit DrainMicrotaskDelayScope(VM&);
        ~DrainMicrotaskDelayScope();

        DrainMicrotaskDelayScope(DrainMicrotaskDelayScope&&) = default;
        DrainMicrotaskDelayScope& operator=(DrainMicrotaskDelayScope&&);
        DrainMicrotaskDelayScope(const DrainMicrotaskDelayScope&);
        DrainMicrotaskDelayScope& operator=(const DrainMicrotaskDelayScope&);

    private:
        void NODELETE increment();
        void decrement();

        RefPtr<VM> m_vm;
    };

    MicrotaskQueue& defaultMicrotaskQueue();

    DrainMicrotaskDelayScope drainMicrotaskDelayScope() { return DrainMicrotaskDelayScope { *this }; }
    // UNGIL review fix (GIL-removal round 5): the per-lite depth-0 release
    // drains (JSLock.cpp -> VMLite::drainDefaultMicrotaskQueue) must honor
    // the embedder's DrainMicrotaskDelayScope exactly like VM::drainMicrotasks
    // does, so the count gains a cross-thread reader. Relaxed load matches
    // the field's own comment: a spawned/carrier drain that observes a live
    // scope defers; the scope-closing thread re-drains.
    bool microtaskDrainIsDelayed() const { return !!m_drainMicrotaskDelayScopeCount.load(std::memory_order_relaxed); }
    JS_EXPORT_PRIVATE void drainMicrotasks();
#if USE(BUN_JSC_ADDITIONS)
    void drainMicrotasksForGlobalObject(JSGlobalObject* globalObject);
#endif
    void setOnEachMicrotaskTick(WTF::Function<void(VM&)>&& func) { m_onEachMicrotaskTick = WTF::move(func); }
    void callOnEachMicrotaskTick()
    {
        if (m_onEachMicrotaskTick)
            m_onEachMicrotaskTick(*this);
    }
    void finalizeSynchronousJSExecution()
    {
        ASSERT(currentThreadIsHoldingAPILock());
        m_currentWeakRefVersion.fetch_add(1, std::memory_order_relaxed);
        // V7: GIL-off the taint hint is shared by N lites — one lite's
        // synchronous-execution boundary must not erase another lite's
        // in-flight taint mark. Leave it sticky (over-tainting is the safe
        // direction; the flag is already a conservative "might"). GIL-on /
        // flag-off: byte-identical clear behind the read-only Config-page
        // gate (same pattern as softStackLimitForCurrentThreadSlow).
        if (!gilOffWithProcessGate()) [[likely]]
            setMightBeExecutingTaintedCode(false);
    }

    uintptr_t currentWeakRefVersion() const { return m_currentWeakRefVersion.load(std::memory_order_relaxed); }

    void setGlobalConstRedeclarationShouldThrow(bool globalConstRedeclarationThrow) { m_globalConstRedeclarationShouldThrow = globalConstRedeclarationThrow; }
    ALWAYS_INLINE bool globalConstRedeclarationShouldThrow() const { return m_globalConstRedeclarationShouldThrow; }

    void setShouldBuildPCToCodeOriginMapping() { m_shouldBuildPCToCodeOriginMapping = true; }
    bool shouldBuilderPCToCodeOriginMapping() const { return m_shouldBuildPCToCodeOriginMapping; }

    BytecodeIntrinsicRegistry& bytecodeIntrinsicRegistry() { return *m_bytecodeIntrinsicRegistry; }
    
    ShadowChicken* shadowChicken() { return m_shadowChicken.getIfExists(); }
    ShadowChicken& ensureShadowChicken() { return m_shadowChicken.get(*this); }
    
#if USE(BUN_JSC_ADDITIONS)
    const StackTraceAppenderFunction& onAppendStackTrace() const { return m_onAppendStackTrace; }
    StackTraceAppenderFunction& onAppendStackTrace() { return m_onAppendStackTrace; }
    
    const ErrorInfoFunction& onComputeErrorInfo() const { return m_onComputeErrorInfo; }
    ErrorInfoFunction& onComputeErrorInfo() { return m_onComputeErrorInfo; }
    
    const ErrorInfoFunctionJSValue& onComputeErrorInfoJSValue() const { return m_onComputeErrorInfoJSValue; }
    ErrorInfoFunctionJSValue& onComputeErrorInfoJSValue() { return m_onComputeErrorInfoJSValue; }
    
    const WTF::Function<void(VM&, SourceProvider*, LineColumn&, String&)>& computeLineColumnWithSourcemap() const { return m_computeLineColumnWithSourcemap; }
    WTF::Function<void(VM&, SourceProvider*, LineColumn&, String&)>& computeLineColumnWithSourcemap() { return m_computeLineColumnWithSourcemap; }

    void setOnAppendStackTrace(StackTraceAppenderFunction&& function) { m_onAppendStackTrace = WTF::move(function); }
    void setOnComputeErrorInfo(ErrorInfoFunction&& function) { m_onComputeErrorInfo = WTF::move(function); }
    void setOnComputeErrorInfoJSValue(ErrorInfoFunctionJSValue&& function) { m_onComputeErrorInfoJSValue = WTF::move(function); }
    void setComputeLineColumnWithSourcemap(WTF::Function<void(VM&, SourceProvider*, LineColumn&, String&)>&& function) { m_computeLineColumnWithSourcemap = WTF::move(function); }
#endif
    
    template<typename Func>
    void logEvent(CodeBlock*, const char* summary, const Func& func);

    inline std::optional<RefPtr<Thread>> ownerThread() const; // Defined in VMInlines.h
    inline std::optional<uint64_t> ownerThreadUID() const; // Defined in VMInlines.h

    ALWAYS_INLINE VMTraps& traps() { return m_threadContext.traps(); }
    ALWAYS_INLINE const VMTraps& traps() const { return m_threadContext.traps(); }

    // UNGIL obligation-10 audit follow-up (the trap-word sibling of the
    // group3Primitives() exception split): the NeedExceptionHandling bit
    // must live in the SAME storage domain as the m_exception word it
    // mirrors. GIL-off the exception word is per-lite, so the bit is fired/
    // cleared/read on the CURRENT lite's trap word (§A.2.1) — a VM-wide
    // fire would make thread B's RETURN_IF_EXCEPTION poll observe (and a
    // clearException clear) thread A's bit, desynchronizing the
    // EXCEPTION_ASSERT bit<->word invariant and losing pending exceptions.
    // GIL-on / flag-off: vm.traps(), byte-identical (the config-page gate
    // is the same two not-taken byte tests group3Primitives() pays). The
    // fallback arm (no installed same-VM gilOff lite) deliberately matches
    // group3Primitives()' VM-block fallback so the bit always tracks the
    // storage the exception word itself resolved to.
    //
    // Selector identity (bit<->word same-storage-domain invariant): the
    // lite-selection predicate here is TEXTUALLY the same as
    // group3Primitives()' (lite && lite->vm == this) — a divergent extra
    // lite->gilOff term would let a mis-stamped same-VM lite publish the
    // word per-lite but the bit on the VM trap word. The stamping invariant
    // (lite->gilOff == vm->m_gilOff at every registration site: VM ctor
    // mainVMLite, JSLock carrier lites, ThreadObject spawned lites) is
    // asserted here instead of being silently relied on.
    ALWAYS_INLINE VMTraps& trapsForCurrentThread()
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            VMLite* lite = VMLite::currentIfExists();
            if (lite && lite->vm == this) [[likely]] {
                ASSERT(lite->gilOff == (m_gilOff ? 1 : 0));
                return lite->threadContext.traps();
            }
        }
        return traps();
    }
    ALWAYS_INLINE const VMTraps& trapsForCurrentThread() const
    {
        return const_cast<VM*>(this)->trapsForCurrentThread();
    }

    // RETURN_IF_EXCEPTION's poll gate: GIL-off the current lite's word OR
    // the VM-level word (mirrors handleTrapsForCurrentThreadIfNeeded's
    // lite-then-VM servicing dispatch, which hasExceptionsAfterHandlingTraps
    // runs); flag-off/GIL-on the single VM-word test, unchanged.
    ALWAYS_INLINE bool trapsMaybeNeedHandlingForCurrentThread() const
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            VMLite* lite = VMLite::currentIfExists();
            if (lite && lite->vm == this) [[likely]] {
                // Same selector as trapsForCurrentThread()/group3Primitives()
                // (see the selector-identity note above).
                ASSERT(lite->gilOff == (m_gilOff ? 1 : 0));
                return lite->threadContext.traps().maybeNeedHandling() || traps().maybeNeedHandling();
            }
        }
        return traps().maybeNeedHandling();
    }

    JS_EXPORT_PRIVATE bool hasExceptionsAfterHandlingTraps();

    CONCURRENT_SAFE void notifyNeedDebuggerBreak() { traps().fireTrap(VMTraps::NeedDebuggerBreak); }
    CONCURRENT_SAFE void notifyNeedShellTimeoutCheck() { traps().fireTrap(VMTraps::NeedShellTimeoutCheck); }
    CONCURRENT_SAFE void notifyNeedTermination() { traps().fireTrap(VMTraps::NeedTermination); }
    CONCURRENT_SAFE void notifyNeedWatchdogCheck() { traps().fireTrap(VMTraps::NeedWatchdogCheck); }

    CONCURRENT_SAFE void requestStop()
    {
        requestEntryScopeService(ConcurrentEntryScopeService::NeedStopTheWorld); // FIXME rdar://161576886
        // SA.2.3 rule 3 (stw-watchdog-timeout root cause): GIL-off, generated
        // code and the D9/W1 pollers read each lite's OWN trap word (the
        // AB-17 per-lite split), so a VM-word-only fireTrap never reaches a
        // mutator resident in a JIT loop - the SA.3 conductor then starves
        // into the 30s watchdog with that mutator reported NON-QUIESCENT.
        // fireTrapVMWide fans the bit to every same-VM lite under the
        // registry lock; GIL-on / flag-off it degrades to exactly the old
        // single-word fireTrap (byte-equivalent).
        traps().fireTrapVMWide(VMTraps::NeedStopTheWorld);
    }
    CONCURRENT_SAFE void cancelStop()
    {
        traps().clearTrap(VMTraps::NeedStopTheWorld);
        clearEntryScopeService(ConcurrentEntryScopeService::NeedStopTheWorld); // FIXME rdar://161576886
    }

    void promiseRejected(JSPromise*);

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10: routed through the mode-split accessor (per-lite
    // GIL-off; VM copy GIL-on — bit-identical single-mutator).
    StackTrace* nativeStackTraceOfLastThrow() const LIFETIME_BOUND { return exceptionScopeVerificationState().m_nativeStackTraceOfLastThrow.get(); }
    Thread* throwingThread() const { return exceptionScopeVerificationState().m_throwingThread.get(); }
    bool needExceptionCheck() const { return exceptionScopeVerificationState().m_needExceptionCheck; }
#endif

    WTF::RunLoop& runLoop() const { return m_runLoop; }

    static void NODELETE setCrashOnVMCreation(bool);

    void addLoopHintExecutionCounter(const JSInstruction*);
    uintptr_t* getLoopHintExecutionCounter(const JSInstruction*);
    void removeLoopHintExecutionCounter(const JSInstruction*);

    ALWAYS_INLINE void writeBarrier(const JSCell* from) { heap.writeBarrier(from); }
    ALWAYS_INLINE void writeBarrier(const JSCell* from, JSValue to) { heap.writeBarrier(from, to); }
    ALWAYS_INLINE void writeBarrier(const JSCell* from, JSCell* to) { heap.writeBarrier(from, to); }
    ALWAYS_INLINE void writeBarrierSlowPath(const JSCell* from) { heap.writeBarrierSlowPath(from); }

    ALWAYS_INLINE void mutatorFence() { heap.mutatorFence(); }

#if ENABLE(DFG_DOES_GC_VALIDATION)
    // UNGIL AB18-C: DoesGC validation state is per-mutator when gilOff — the
    // group3Primitives()-style mode split. GIL-on (incl. flag-off): the VM
    // member, bit-identical to today. GIL-off: the CURRENT thread's lite
    // slot (owner-thread-only — every JIT store, OSR-exit C++ setter, and
    // allocation-slow-path verifyCanGC read executes on the owning thread,
    // so program order alone gives the needed ordering; no fences).
    //
    // Fallback arm (gilOff, no installed same-VM lite — ctor/dtor windows,
    // shared-GC conductor/collector verifyCanGC calls from Heap.cpp): the
    // VM-block member, which GIL-off is inert spare storage that trivially
    // passes. That inertness is ENFORCED, not assumed: every baked DFG/FTL
    // emission site is rerouted (loadVMLite -> VMLite::offsetOfDoesGC()), so
    // GIL-off m_doesGC can only ever hold expect-true values — its default
    // encode(true, Uninitialized), or a fallback-arm C++ Special write like
    // throwTerminationException's encode(true, Termination); that legitimate
    // write is why this checks expectDoesGC() rather than bit-exact equality
    // with the default. A missed baked store (per-node stores write
    // expect-false on most nodes) fail-stops here deterministically instead
    // of re-manifesting as a racy cross-thread DoesGC abort.
    ALWAYS_INLINE DoesGCCheck& doesGCCheckSlot()
    {
        if (gilOffWithProcessGate()) [[unlikely]] {
            VMLite* lite = VMLite::currentIfExists();
            if (lite && lite->vm == this) [[likely]]
                return lite->doesGC;
            RELEASE_ASSERT(m_doesGC.expectDoesGC());
        }
        return m_doesGC;
    }
    // GIL-on/flag-off EMISSION only (baked absolute address); gilOff
    // emission arms must not bake this — they emit
    // loadVMLite -> VMLite::offsetOfDoesGC() instead. (The ARM64
    // disassembler annotation may call this in any mode; it only
    // pointer-compares, and gilOff code never bakes this address, so the
    // compare simply never matches.)
    DoesGCCheck* addressOfDoesGC() LIFETIME_BOUND { return &m_doesGC; }
    void setDoesGCExpectation(bool expectDoesGC, unsigned nodeIndex, unsigned nodeOp) { doesGCCheckSlot().set(expectDoesGC, nodeIndex, nodeOp); }
    void setDoesGCExpectation(bool expectDoesGC, DoesGCCheck::Special special) { doesGCCheckSlot().set(expectDoesGC, special); }
    void verifyCanGC() { doesGCCheckSlot().verifyCanGC(*this); }
#else
    DoesGCCheck* addressOfDoesGC() { UNREACHABLE_FOR_PLATFORM(); return nullptr; }
    void setDoesGCExpectation(bool, unsigned, unsigned) { }
    void setDoesGCExpectation(bool, DoesGCCheck::Special) { }
    void verifyCanGC() { }
#endif

    void beginMarking();
    DECLARE_VISIT_AGGREGATE;

    void NODELETE addDebugger(Debugger&);
    void NODELETE removeDebugger(Debugger&);
    template<typename Func>
    void forEachDebugger(const Func&);

    void changeNumberOfActiveJITPlans(int64_t value)
    {
        m_numberOfActiveJITPlans.fetch_add(value, std::memory_order_relaxed);
    }

    int64_t numberOfActiveJITPlans() const { return m_numberOfActiveJITPlans.load(std::memory_order_relaxed); }

    Ref<Waiter> NODELETE syncWaiter();

    void notifyDebuggerHookInjected() { m_isDebuggerHookInjected = true; }
    bool isDebuggerHookInjected() const { return m_isDebuggerHookInjected; }
    int64_t incrementModuleAsyncEvaluationCount() { return m_moduleAsyncEvaluationCount++; }
#if USE(BUN_JSC_ADDITIONS)
    int64_t moduleAsyncEvaluationCount() const { return m_moduleAsyncEvaluationCount; }
#endif

#if ENABLE(WEBASSEMBLY_DEBUGGER)
    JS_EXPORT_PRIVATE Wasm::DebugState* NODELETE debugState();
#endif

private:
    VM(VMType, HeapType, WTF::RunLoop* = nullptr, bool* success = nullptr);
    static VM*& sharedInstanceInternal();
    void createNativeThunk();

    JSPropertyNameEnumerator* emptyPropertyNameEnumeratorSlow();
    NativeExecutable* promiseResolvingFunctionResolveExecutableSlow();
    NativeExecutable* promiseResolvingFunctionRejectExecutableSlow();
    NativeExecutable* promiseFirstResolvingFunctionResolveExecutableSlow();
    NativeExecutable* promiseFirstResolvingFunctionRejectExecutableSlow();
    NativeExecutable* promiseResolvingFunctionResolveWithInternalMicrotaskExecutableSlow();
    NativeExecutable* promiseResolvingFunctionRejectWithInternalMicrotaskExecutableSlow();
    NativeExecutable* promiseCapabilityExecutorExecutableSlow();
    NativeExecutable* promiseAllFulfillFunctionExecutableSlow();
    NativeExecutable* promiseAllSlowFulfillFunctionExecutableSlow();
    NativeExecutable* promiseAllSettledFulfillFunctionExecutableSlow();
    NativeExecutable* promiseAllSettledRejectFunctionExecutableSlow();
    NativeExecutable* promiseAllSettledSlowFulfillFunctionExecutableSlow();
    NativeExecutable* promiseAllSettledSlowRejectFunctionExecutableSlow();
    NativeExecutable* promiseAnyRejectFunctionExecutableSlow();
    NativeExecutable* promiseAnySlowRejectFunctionExecutableSlow();

    void updateStackLimits();

    bool isSafeToRecurse(void* stackLimit) const
    {
        void* curr = currentStackPointer();
        return curr >= stackLimit;
    }

    Exception* exception() const
    {
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
        exceptionScopeVerificationState().m_needExceptionCheck = false; // Obligation 10 mode split (mutable member).
#endif
        // UNGIL §A.1.3 mode split: per-lite when gilOff (a GC visit thread
        // must NOT come through here — the root walk reads each registered
        // lite directly, r6 F5; Heap.cpp Msr/VMExceptions).
        return group3Primitives().m_exception;
    }

    void clearException()
    {
#if ASSERT_ENABLED
        // SPEC-vmstate I15: m_exception/m_lastException are written only by
        // the JSLock holder.
        if (Options::useVMLite())
            ASSERT(currentThreadIsHoldingAPILock());
#endif
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
        // Obligation 10 mode split: the CLEARING thread's own bookkeeping.
        auto& verificationState = exceptionScopeVerificationState();
        verificationState.m_needExceptionCheck = false;
        clearNativeStackTraceOfLastThrow();
        verificationState.m_throwingThread = nullptr;
#endif
        // UNGIL §A.1.3 mode split. Relaxed atomic store for the same reason
        // as VM::setException (tsan-vm-setexception-cross-thread-r3): the
        // word has one sanctioned lock-free reader,
        // hasPendingTerminationException(); a plain nullptr store here would
        // be the same TSAN race from the clearing side.
        WTF::atomicStore(&group3Primitives().m_exception, static_cast<Exception*>(nullptr), std::memory_order_relaxed);
        // Same storage domain as the word above: per-lite GIL-off (see
        // trapsForCurrentThread()), VM word GIL-on — byte-identical.
        trapsForCurrentThread().clearTrap(VMTraps::NeedExceptionHandling);
    }

    JS_EXPORT_PRIVATE void setException(Exception*);

    JS_EXPORT_PRIVATE Exception* throwException(JSGlobalObject*, Exception*);
    JS_EXPORT_PRIVATE Exception* throwException(JSGlobalObject*, JSValue);
    JS_EXPORT_PRIVATE Exception* throwException(JSGlobalObject*, JSObject*);

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    void verifyExceptionCheckNeedIsSatisfied(unsigned depth, ExceptionEventLocation&);
    JS_EXPORT_PRIVATE void clearNativeStackTraceOfLastThrow();
#endif
    
    static void primitiveGigacageDisabledCallback(void*);
    void primitiveGigacageDisabled();

    void callPromiseRejectionCallback(Strong<JSPromise>&);
    void didExhaustMicrotaskQueue();

#if ENABLE(GC_VALIDATION)
    // UNGIL (AB-17 verification-rung fix): an object initialization is a
    // property of ONE thread's stack, and GIL-off N mutators share this VM —
    // a per-VM slot false-positives the tryAllocateCellHelper
    // !isInitializingObject() assertion (thread A mid-FunctionCodeBlock
    // finishCreation while thread B allocates). Thread-local keeps the
    // validation exact in both shapes; GIL-on/flag-off has one mutator per
    // VM at a time, so behavior is unchanged (a thread initializes at most
    // one object at a time regardless of which VM it is entered in).
    static thread_local const ClassInfo* s_initializingObjectClass;
#endif

    // SPEC-vmstate §6.4(1)/M6: m_stackPointerAtVMEntry / m_stackLimit /
    // m_lastStackTop moved up into the VMLitePrimitives X-macro block. §6.3
    // relocated member (interleaved in the old Group-3 range; the §6.4(2)
    // span assert forces it out of the block; name/type/sites unchanged):
    size_t m_currentSoftReservedZoneSize;

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // UNGIL audit K4 table I: the verification bookkeeping (chain anchor +
    // simulated-throw state) is classed PER-LITE (vmstate I15 — throw state
    // is thread-local). INTEGRATE-ungil.md obligation 10 (owner U-T8b):
    // LANDED — this block is now ONE struct member selected through the
    // exceptionScopeVerificationState() mode-split accessor (declared next
    // to group3Primitives() above); GIL-off lites carry their own copy as a
    // debug-only L2 VMLite tail append, NOT part of the frozen
    // VMLitePrimitives ABI.
    //
    // History (IT-1 review round; the first apply attempt was REJECTED 1/3
    // as a truncated diff — the landed change satisfies the conditions
    // below). The race was confirmed real: ExceptionScope's ctor/dtor used
    // to push/pop one
    // VM-shared m_topExceptionScope word, so a spawned lite's scope links
    // m_previousScope into the carrier's stack; the carrier's pop unlinks
    // the spawned scope (chain unwinds to null -> the
    // TopExceptionScope.cpp RELEASE_ASSERT variant) and leaves the spawned
    // ~ThrowScope dereferencing a popped, ASAN-poisoned frame via
    // ExceptionScope::stackPosition() (the deterministic GIL-off
    // stack-use-after-return; VMEntryScope.cpp status item (i)). The
    // transmitted diff, however, was truncated and covered only
    // VM.h/VMLite.h; landing it would have broken EVERY Debug/ASAN build
    // (ENABLE(EXCEPTION_SCOPE_VERIFICATION) = ASSERT || ASAN).
    //
    // Landed as ONE complete change: group3Primitives()-style
    // mode-split accessor (GIL-on / second-VM U0b alias to this block;
    // gilOff lites use an L2 tail append on VMLite AFTER threadContext —
    // debug-only, NOT part of the frozen VMLitePrimitives ABI, no
    // generated-code offset moves) rerouting ALL raw-member sites:
    //   - ExceptionScope.cpp ctor/dtor (the race site itself);
    //   - ThrowScope.cpp simulateThrow + dtor verification;
    //   - TopExceptionScope.cpp RELEASE_ASSERT;
    //   - VM.cpp throwException capture, verifyExceptionCheckNeedIsSatisfied,
    //     clearNativeStackTraceOfLastThrow;
    //   - LockObject.cpp/.h GILParkSavedExecutionState save/clear/restore
    //     (GIL-on arm only — GIL-off carrier takes the §J.2 early-return;
    //     the split makes §J.2's "per-lite words" premise true, so also
    //     fix the now-stale LockObject.cpp rationale comment near the
    //     park-site predicate);
    //   - VM.h getters (nativeStackTraceOfLastThrow/throwingThread/
    //     needExceptionCheck), exception() const, clearException().
    // Required at landing: ASSERT in the accessor's GIL-off fallback arm
    // (no current lite, or lite->vm != this) that the thread is the
    // carrier or holds m_lock — otherwise a future non-mutator scope user
    // silently reopens the shared-word race; and note that a scope whose
    // lifetime straddles a t_currentVMLite install/uninstall resolves
    // DIFFERENT storage in ctor vs dtor (linked-list write-back is not
    // idempotent, unlike the group3Primitives precedent) — keep scopes
    // strictly inside a stable (thread, lite) window.
    // Acceptance gate: clean Debug/ASAN build (the rename turns any missed
    // site into a compile error) + the pinned GIL-off smoke command +
    // ta-wait-thread-gate.js. Release smoke red/green does NOT count
    // against this item — VMEntryScope.cpp status items (ii)/(iii) are
    // separate legs. (Also: the proposal's file list named CatchScope.cpp,
    // which does not exist in this tree — correct the list on resubmit.)
    // The former loose members (m_topExceptionScope,
    // m_simulatedThrowPointLocation/RecursionDepth, m_needExceptionCheck,
    // m_nativeStackTraceOfLastThrow/SimulatedThrow, m_throwingThread) now
    // live in this one struct, with names/types unchanged inside it — the
    // relocation is the rename that turned every raw site into a compile
    // error. ALL access goes through exceptionScopeVerificationState()
    // (mode-split accessor above); this VM copy is the GIL-on / flag-off /
    // fallback-window storage and is bit-identical in behavior to the old
    // members for a single mutator.
    VMExceptionScopeVerificationState m_exceptionScopeVerificationState;
#endif

public:
    SentinelLinkedList<MicrotaskQueue, BasicRawSentinelNode<MicrotaskQueue>> m_microtaskQueues;
private:
    // SPEC-vmstate §6.4.4: main thread's VMLite carrier (tid 0). Created at
    // the END of the VM ctor when useVMLite; registered there via
    // VMLiteRegistry::registerLite (sole writer of VMLite::vm); the ctor
    // NEVER calls VMLite::setCurrent — JSLock::didAcquireLock installs it
    // (M4). Uninstalled+unregistered+destroyed at the TOP of ~VM (I20).
    std::unique_ptr<VMLite> m_mainVMLite;
    // V7: test hook written by $vm/jsc-shell on one lite and consumed at
    // CodeBlock-install time on any lite. Relaxed atomic; the exchange in
    // getAndClearFailNextNewCodeBlock makes the "fail the NEXT code block"
    // contract exactly-once under N mutators.
    std::atomic<bool> m_failNextNewCodeBlock { false };
    bool m_globalConstRedeclarationShouldThrow { true };
    bool m_shouldBuildPCToCodeOriginMapping { false };
    DeletePropertyMode m_deletePropertyMode { DeletePropertyMode::Default };
    HeapAnalyzer* m_activeHeapAnalyzer { nullptr };
    std::unique_ptr<CodeCache> m_codeCache;
    std::unique_ptr<IntlCache> m_intlCache;
    std::unique_ptr<BuiltinExecutables> m_builtinExecutables;
    UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, RefPtr<WatchpointSet>> m_impurePropertyWatchpointSets;
    std::unique_ptr<TypeProfiler> m_typeProfiler;
    std::unique_ptr<TypeProfilerLog> m_typeProfilerLog;
    unsigned m_typeProfilerEnabledCount { 0 };
    Lock m_scratchBufferLock;
    Vector<ScratchBuffer*> m_scratchBuffers;
    size_t m_sizeOfLastScratchBuffer { 0 };
    Vector<std::unique_ptr<CheckpointOSRExitSideState>, expectedMaxActiveSideStateCount> m_checkpointSideState;
    InlineWatchpointSet m_primitiveGigacageEnabled { IsWatched };
    FunctionHasExecutedCache m_functionHasExecutedCache;
    std::unique_ptr<ControlFlowProfiler> m_controlFlowProfiler;
    unsigned m_controlFlowProfilerEnabledCount { 0 };
    MallocPtr<EncodedJSValue, VMMalloc> m_exceptionFuzzBuffer;
    LazyRef<VM, Watchdog> m_watchdog;
    LazyUniqueRef<VM, HeapProfiler> m_heapProfiler;
    LazyUniqueRef<VM, AdaptiveStringSearcherTables> m_stringSearcherTables;
#if ENABLE(SAMPLING_PROFILER)
    const RefPtr<SamplingProfiler> m_samplingProfiler;
#endif
    std::unique_ptr<FuzzerAgent> m_fuzzerAgent;
    LazyUniqueRef<VM, ShadowChicken> m_shadowChicken;
    std::unique_ptr<BytecodeIntrinsicRegistry> m_bytecodeIntrinsicRegistry;
    // UNGIL review fix: GIL-off, drainMicrotasks() (spawned-thread per-lite
    // drains, ThreadObject completion) reads this count cross-thread while
    // DrainMicrotaskDelayScope mutates it on the embedder's carrier. Relaxed
    // atomic retires the TSAN data race; semantically the scope is
    // carrier-scoped state and a spawned drain that observes a live scope
    // still defers (the conservative reading of api 4.6.1 — the carrier's
    // scope exit re-drains, and per-lite queues are drained again at the E.2
    // close ladder once it is wired).
    std::atomic<uint64_t> m_drainMicrotaskDelayScopeCount { 0 };

    // FIXME: We should remove handled promises from this list at GC flip. <https://webkit.org/b/201005>
    Vector<Strong<JSPromise>> m_aboutToBeNotifiedRejectedPromises;

    WTF::Function<void(VM&)> m_onEachMicrotaskTick;
#if USE(BUN_JSC_ADDITIONS)
    ErrorInfoFunction m_onComputeErrorInfo;
    ErrorInfoFunctionJSValue m_onComputeErrorInfoJSValue;
    StackTraceAppenderFunction m_onAppendStackTrace;
    WTF::Function<void(VM&, SourceProvider*, LineColumn&, String&)> m_computeLineColumnWithSourcemap;
#endif
    // V7: bumped at each lite's synchronous-execution boundary; read by other
    // lites and by GC marker threads (JSWeakObjectRef::visitChildren).
    // Relaxed is sufficient: readers only compare for equality against a
    // version they captured on their own thread, and the GC handshake orders
    // the marking-time read.
    std::atomic<uintptr_t> m_currentWeakRefVersion { 0 };

    int64_t m_moduleAsyncEvaluationCount { 0 };

#if USE(BUN_JSC_ADDITIONS)
public:
    struct SynchronousModuleTask {
        InternalMicrotask task;
        uint8_t payload;
        JSValue arg0;
        JSValue arg1;
        JSValue arg2;
    };
    // While non-null, internal-microtask reactions for already-settled promises
    // are appended here instead of the global microtask queue.
    // JSModuleLoader::loadModuleSync points this at a stack-allocated frame and
    // drains it in a loop so require(esm) can load+link+evaluate without
    // yielding to user microtasks and without the O(module-count) C++ recursion
    // that direct re-entry into runInternalMicrotask would cause.
    //
    // The Vector's heap buffer is NOT covered by conservative stack scanning,
    // so VM::visitAggregateImpl walks the full prev-linked chain and marks
    // every queued JSValue. The chain exists because module evaluation can
    // re-enter loadModuleSync (require(esm) inside an evaluated module).
    struct SynchronousModuleQueue {
        Vector<SynchronousModuleTask> tasks;
        SynchronousModuleQueue* prev { nullptr };
    };
    SynchronousModuleQueue* m_synchronousModuleQueue { nullptr };
private:
#endif

    bool m_hasSideData { false };
    // UNGIL review fix (sibling of m_executionForbidden below): GIL-off this
    // is written by whichever thread services NeedTermination (handleTraps'
    // NeedTermination arm runs vm.setHasTerminationRequest() on the
    // servicing thread, spawned or carrier) and by the embedder/watchdog on
    // a carrier, and read cross-thread from the §E.2a drain loop, the D9
    // parked-thread termination polls, and WaiterListManager's sync-wait
    // path. Relaxed atomics: every consumer re-validates against the trap
    // word / termination exception, so no ordering is load-bearing. Unlike
    // m_executionForbidden this flag is NOT monotonic
    // (clearHasTerminationRequest resets it): a host clear racing a
    // concurrent servicer's set can lose the set — the set is re-delivered
    // because the shared NeedTermination trap bit (TERM1.2: left visible
    // while any other lite of the VM is entered) is the channel parked/
    // spinning threads actually poll; this flag is a host-facing latch, not
    // the delivery mechanism. GIL-on/flag-off codegen unchanged.
    std::atomic<bool> m_hasTerminationRequest { false };
    std::atomic<bool> m_executionForbidden { false };
    bool m_executionForbiddenOnTermination { false };
    bool m_isDebuggerHookInjected { false };

#if ENABLE(WEBASSEMBLY_DEBUGGER)
    std::unique_ptr<Wasm::DebugState> m_debugState;
#endif

    Lock m_loopHintExecutionCountLock;
    UncheckedKeyHashMap<const JSInstruction*, std::pair<unsigned, std::unique_ptr<uintptr_t>>> m_loopHintExecutionCounts;

    const Ref<MicrotaskQueue> m_defaultMicrotaskQueue;
    const Ref<Waiter> m_syncWaiter;

    std::atomic<int64_t> m_numberOfActiveJITPlans { 0 };

    Vector<Function<void()>> m_didPopListeners;

#if ENABLE(DFG_DOES_GC_VALIDATION)
    DoesGCCheck m_doesGC;
#endif

    DoublyLinkedList<Debugger> m_debuggers;

    friend class Heap;
    friend class ExceptionScope; // Friend for exception checking purpose only.
    friend class TopExceptionScope; // Friend for exception checking purpose only.
    friend class ThrowScope; // Friend for exception checking purpose only.
    friend class JSDollarVMHelper;
    friend class LLIntOffsetsExtractor;
    friend class SuspendExceptionScope;
    friend class VMTraps;
    friend class GILParkSavedExecutionState; // Phase-1 JS-threads GIL stub: swaps per-thread execution state across parks (LockObject.h).
};

static_assert(OBJECT_OFFSETOF(VM, topEntryFrame) == OBJECT_OFFSETOF(VM, topCallFrame) + sizeof(void*), "We load/store these using a pair instruction");

// SPEC-vmstate §6.4(2) (M6): per-field layout equivalence — VM's X-macro block
// is layout-identical to VMLitePrimitives, so VM can serve as the main
// thread's physical VMLitePrimitives (mainVMLitePrimitives()) and Phase B can
// retarget VM::field accesses VMLitePrimitives-relative without ABI drift.
#define VM_ASSERT_VMLITE_PRIMITIVE_FIELD_OFFSET(type, name) \
    static_assert(OBJECT_OFFSETOF(VM, name) - OBJECT_OFFSETOF(VM, topCallFrame) \
        == OBJECT_OFFSETOF(VMLitePrimitives, name), \
        "VM Group 1-3 member " #name " must not drift from VMLitePrimitives");
FOR_EACH_VMLITE_PRIMITIVE_FIELD(VM_ASSERT_VMLITE_PRIMITIVE_FIELD_OFFSET)
#undef VM_ASSERT_VMLITE_PRIMITIVE_FIELD_OFFSET
// Span assert: the block is exactly one VMLitePrimitives — nothing interleaves
// (this is what forces m_currentSoftReservedZoneSize out, §6.3).
static_assert(OBJECT_OFFSETOF(VM, m_lastStackTop) - OBJECT_OFFSETOF(VM, topCallFrame) + sizeof(void*)
    == sizeof(VMLitePrimitives), "VM Group 1-3 block must span exactly sizeof(VMLitePrimitives)");

#if ENABLE(GC_VALIDATION)
inline const ClassInfo* VM::initializingObjectClass() const
{
    return s_initializingObjectClass;
}

inline bool VM::isInitializingObject() const
{
    return !!s_initializingObjectClass;
}

inline void VM::setInitializingObjectClass(const ClassInfo* initializingObjectClass)
{
    s_initializingObjectClass = initializingObjectClass;
}
#endif

inline Heap* WeakSet::heap() const
{
    return &m_vm->heap;
}

#if !ENABLE(C_LOOP)
extern "C" void SYSV_ABI sanitizeStackForVMImpl(VM*);
#endif

JS_EXPORT_PRIVATE void sanitizeStackForVM(VM&);

} // namespace JSC


namespace WTF {

// Unfortunately we have a lot of code that uses JSC::VM without locally
// verifying its lifetime. Safer CPP checker needs to understand JSC::VM's
// lifetime threaded from JSC entrance. Until that, we explicitly suppress
// Ref<VM> lifetime checking by using ThreadSafeRefCountedWithSuppressingSaferCPPChecking.
template<> struct DefaultRefDerefTraits<JSC::VM> {
    static constexpr bool isDefaultImplementation = false;

    static ALWAYS_INLINE JSC::VM* refIfNotNull(JSC::VM* ptr)
    {
        if (ptr) [[likely]]
            ptr->refSuppressingSaferCPPChecking();
        return ptr;
    }

    static ALWAYS_INLINE JSC::VM& ref(JSC::VM& ref)
    {
        ref.refSuppressingSaferCPPChecking();
        return ref;
    }

    static ALWAYS_INLINE void derefIfNotNull(JSC::VM* ptr)
    {
        if (ptr) [[likely]]
            ptr->derefSuppressingSaferCPPChecking();
    }
};

} // namespace WTF


WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
