/*
 * Copyright (C) 2016-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(SAMPLING_PROFILER)

#include "CallFrame.h"
#include "CodeBlockHash.h"
#include "JITCode.h"
#include "JSExportMacros.h"
#include "MachineStackMarker.h"
#include "NativeCallee.h"
#include "Options.h"
#include "PCToCodeOriginMap.h"
#include "ThreadManager.h"
#include "WasmCompilationMode.h"
#include "WasmIndexOrName.h"
#include <wtf/Box.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MallocCommon.h>
#include <wtf/Noncopyable.h>
#include <wtf/Stopwatch.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>

namespace JSC {

class VM;
class ExecutableBase;

// =============================================================================
// SPEC-ungil §A.1.7 — IU table: off-thread readers of rerouted Group-3 fields
// (U-T8d deliverable; r9 F7 + r24 F2, NORMATIVE; this header is the anchor
// file per the §IM row "SamplingProfiler.{h,cpp}, VMInspector.cpp =
// §A.1.7/AUD1.K1").
//
// Rule (§A.1.7): every off-thread reader of a field rerouted to
// VMLitePrimitives / per-lite storage under GIL-off (§A.1.3 Group-3 set:
// topCallFrame, topEntryFrame, the Group-2 exception/unwind words, stack
// bookkeeping, scratch buffers, the per-thread microtask queue, lazy regexp
// state) must be one of:
//   (i)   resolve the TARGET thread's lite via VMLiteRegistry (registry
//         locked, target suspended or stopped by a §A.3 / heap §10 stop);
//   (ii)  be REFUSED GIL-off with a defined error;
//   (iii) be proven on-thread (the reader runs on the thread that owns the
//         storage).
//
// SUSPEND RULE (r24 F2, scoped carve-out of the §LK.6 fastMalloc-under-
// registry-lock allowance): while ANY thread is suspended by a (i)-reader,
// the suspending thread performs NO allocation (fastMalloc included) and
// acquires NO lock beyond the already-held registry lock; all sample/trace
// buffers are pre-allocated before suspension. Debug enforcement vehicle:
// SamplingProfiler::WhileTargetSuspendedScope below — it MUST be
// instantiated inside takeSample()'s didSuspend window (the
// SamplingProfiler.cpp half of this row; see PENDING note below).
//
// >>> PENDING (SamplingProfiler.cpp half of U-T8d) <<<
// This header lands the gate predicate, the enforcement scope, and the
// normative table; SamplingProfiler.cpp is NOT yet wired to them. Before
// GIL-off activation (U-T6/U-T9) — and re-checked by the U-T14 re-audit —
// SamplingProfiler.cpp must:
//   1. noticeCurrentThreadAsJSCExecutionThreadWithLock(): early-return
//      (keeping the existing binding) when
//      !shouldBindCurrentThreadAsJSCExecutionThread().
//   2. takeSample(): instantiate WhileTargetSuspendedScope immediately
//      after a successful m_jscExecutionThread->suspend(), destroyed
//      before resume() (m_currentFrames pre-allocation precedes it).
//   3. takeSample()/FrameWalker: GIL-off, resolve the suspended carrier's
//      lite (VMLiteRegistry under its lock, target suspended) and read
//      lite->primitives.topCallFrame / .topEntryFrame and
//      lite->executingRegExp — the VM-member reads below are the GIL-on
//      storage side only (§A.1.3(3): GIL-off the words live in the
//      installed lite; the carrier's lite is a separate allocation,
//      VM::m_mainVMLite, NOT the VM-interior block). This matches
//      VMInspector.cpp's lite-resolved reads (the other §A.1.7 reader of
//      this task) — the two readers agree the carrier's GIL-off storage
//      is the lite.
// Until 1-3 land, GIL-off sampling is NOT safe; GIL-on/flag-off behavior
// is today's, unchanged.
//
// ---- Field: topCallFrame (VMLitePrimitives Group 1) ----
// | reader (file:line)                                | thread            | disposition |
// | SamplingProfiler.cpp:421, :431 (takeSample)       | profiler thread,  | (i) v1 carrier-only (AUD1.K1/SD18) REQUIRED: m_jscExecutionThread binds
// |                                                   | target suspended  |     to carrier (main/embedder) threads only GIL-off — spawned never
// |                                                   |                   |     (shouldBindCurrentThreadAsJSCExecutionThread below); reads must
// |                                                   |                   |     resolve the carrier lite; SUSPEND RULE applies. Wiring: PENDING
// |                                                   |                   |     (.cpp half, items 1-3 above).
// | VMTraps.cpp:170 (invalidateCodeBlocksOnStack)     | mutator (self)    | (iii) on-thread: VMTraps::handleTraps runs on the trapping mutator.
// | VMTraps.cpp:485 (handleTraps NeedDebuggerBreak)   | mutator (self)    | (iii) on-thread.
// | heap/Heap.cpp:2234 (shadowChicken update)         | collector thread  | (i)-under-stop: mutators quiesced by the heap §10 stop; GIL-off the
// |                                                   |                   |     walk iterates the VMLiteRegistry per the §A.1.3 GC-roots rule
// |                                                   |                   |     (per-VM filter). Owner: heap WS / U-T1.
// | debugger/Debugger.cpp:860, :870 (pause path)      | mutator (self)    | (iii) on-thread; GIL-off the debugger is carrier-only (§A.2.7, SD13).
// | tools/VMInspector.cpp dumpRegisters               | debugger/REPL     | (iii) current-lite read GIL-off, (ii) refused otherwise — implemented
// |                                                   |                   |     in VMInspector.cpp (this task).
//
// ---- Field: topEntryFrame (VMLitePrimitives Group 1) ----
// | SamplingProfiler.cpp:89 (FrameWalker ctor)        | profiler thread,  | (i) carrier-only (AUD1.K1) + SUSPEND RULE REQUIRED (the ctor runs
// |                                                   | target suspended  |     inside takeSample's suspended window). Wiring: PENDING (.cpp
// |                                                   |                   |     half, items 1-3 above).
// | VMTraps.cpp:121 (tryInstallTrapBreakpoints)       | mutator (self,    | (iii) on-thread: the SIGUSR handler executes on the target mutator's
// |                                                   | signal context)   |     own thread; async-signal caveats unchanged.
// | VMTraps.cpp:186 (invalidateCodeBlocksOnStack)     | mutator (self)    | (iii) on-thread.
// | debugger/Debugger.cpp:922, :1318, :1343, :1373    | mutator (self)    | (iii) on-thread; SD13 carrier-only GIL-off.
// | tools/VMInspector.cpp dumpRegisters               | debugger/REPL     | (iii)/(ii), implemented in VMInspector.cpp (this task).
//
// ---- Fields: m_exception, m_lastException (Group 2) ----
// | heap/Heap.cpp:3585-class root walk                | collector thread  | (i)-under-stop: §A.1.3 GC-roots rule (NORMATIVE) — the shared
// |   (ConservativeScan/VMExceptions constraint)      |                   |     collection's root/handle visit iterates the VMLiteRegistry under
// |                                                   |                   |     its lock and appends EVERY registered lite's cell fields, filtered
// |                                                   |                   |     to lite->vm == the collecting VM. m_terminationException stays
// |                                                   |                   |     VM-global (deliberately NOT in VMLitePrimitives). Owner: U-T1/heap.
// | (no other off-thread readers found — census       |                   |
// |  U-T8d: all throw/catch/clearException sites are  |                   |
// |  interpreter/JIT/runtime unwind, on-thread)       |                   | (iii) by construction.
//
// ---- Fields: callFrameForCatch, targetMachinePCForThrow,
//      targetInterpreterPCForThrow, targetInterpreterMetadataPCForThrow,
//      targetMachinePCAfterCatch, newCallFrameReturnValue,
//      encodedHostCallReturnValue, targetTryDepthForThrow, osrExitIndex,
//      varargsLength, osrExitJumpDestination (Group 2 unwind words) ----
// | (census U-T8d: readers are LLInt/Baseline/DFG/FTL unwind + OSR paths
// |  on the throwing thread only; no off-thread reader exists)            | (iii) by construction.
//
// ---- Fields: m_stackPointerAtVMEntry, m_stackLimit, m_lastStackTop (Group 3) ----
// | tools/VMInspector.cpp:63-64 (vmForCallFrame)      | debugger/REPL,    | (iii) GIL-off restricted to the CURRENT thread's installed lite;
// |                                                   | any thread        |     cross-VM scan refused with a defined error (ii). Implemented in
// |                                                   |                   |     VMInspector.cpp (this task).
// | tools/JSDollarVM.cpp:3120-:3157                   | mutator (self)    | (iii) on-thread ($vm.callWithStackSize reads its own VM mid-call).
// |   (functionCallWithStackSize)                     |                   |
// | runtime/JSLock.cpp L7 assert + handoff writes     | locking thread    | (iii) on-thread; the L7 RELEASE_ASSERT is GIL-on-only (§A.1.4); the
// |                                                   |                   |     GIL-off token ctor asserts the LITE's slot empty instead.
// | heap conservative scan (MachineStackMarker)       | collector thread  | not a Group-3 FIELD reader: it captures suspended threads' machine
// |                                                   |                   |     state via thread suspension under the heap §10 stop; stack BOUNDS
// |                                                   |                   |     GIL-off come from each lite via the registry walk (owner: heap WS).
//
// ---- Field: m_executingRegExp + lazy regexp stack/match buffers (per-lite Group 4) ----
// | SamplingProfiler.cpp:419, :428 (takeSample)       | profiler thread,  | (i) carrier-only (AUD1.K1) REQUIRED: must resolve the CARRIER lite's
// |                                                   | target suspended  |     executingRegExp; SUSPEND RULE applies. Wiring: PENDING (.cpp
// |                                                   |                   |     half, items 1-3 above).
// | (no other off-thread readers found)               |                   | (iii) by construction (regexp engine reads its own thread's state).
//
// ---- Field: scratch buffers (§A.1.6 per-lite tables) ----
// | VM::gatherScratchBufferRoots (GC root gather)     | collector thread  | (i)-under-stop: per-lite buffer-ownership lists scanned via the
// |                                                   |                   |     registry walk (jit R2). Owner: U-T4/heap WS.
//
// ---- Field: m_microtaskQueue (§E per-lite default queue) ----
// | GC markers iterating VM::m_microtaskQueues        | marker threads    | (i)-under-stop: list MEMBERSHIP under VMLiteRegistry::lock (M11/M12);
// |   (beginMarking/visitAggregateImpl)               |                   |     queue CONTENTS visited only at phases with all mutators stopped.
// |                                                   |                   |     Owner: U-T9.
//
// ---- Field: entryScope / isEntered + service bits (§A.1.5 per-lite) ----
// | VM-wide consumers (e.g. whenIdle, service fan-out)| any thread        | (i): iterate the registry under its lock (§A.1.5); thread-local
// |                                                   |                   |     services use the CURRENT lite (iii). Owner: U23/U-T1.
// | Watchdog / VMTraps service + debugger bits        | watchdog/sender   | carrier-only GIL-off (§A.2.7-8, SD13/SD14 pattern) — (ii) for
// |                                                   | threads           |     non-carrier targets. Owner: U-T2.
//
// This table enumerates call sites only; it never re-rules an audit row
// (K4/N7 dispositions are consumed verbatim). Any NEW off-thread reader of a
// rerouted field must be added here with a disposition before it lands
// (U-T14 re-audit gate).
// =============================================================================

class SamplingProfiler : public ThreadSafeRefCounted<SamplingProfiler> {
    WTF_MAKE_TZONE_ALLOCATED(SamplingProfiler);
public:

    struct UnprocessedStackFrame {
        UnprocessedStackFrame(CodeBlock* codeBlock, CalleeBits callee, CallSiteIndex callSiteIndex)
            : unverifiedCallee(callee)
            , verifiedCodeBlock(codeBlock)
            , callSiteIndex(callSiteIndex)
        { }

        UnprocessedStackFrame(const void* pc)
            : cCodePC(pc)
        { }

        UnprocessedStackFrame() = default;

        const void* cCodePC { nullptr };
        CalleeBits unverifiedCallee;
        CodeBlock* verifiedCodeBlock { nullptr };
        CallSiteIndex callSiteIndex;
        NativeCallee::Category nativeCalleeCategory { NativeCallee::Category::InlineCache };
#if ENABLE(WEBASSEMBLY)
        std::optional<Wasm::IndexOrName> wasmIndexOrName;
#endif
        std::optional<Wasm::CompilationMode> wasmCompilationMode;
#if ENABLE(JIT)
        Box<PCToCodeOriginMap> wasmPCMap;
#endif
    };

    enum class FrameType { 
        Executable,
        Wasm,
        Host,
        RegExp,
        C,
        Unknown,
    };

    struct StackFrame {
        StackFrame(ExecutableBase* executable)
            : frameType(FrameType::Executable)
            , executable(executable)
        { }

        StackFrame()
        { }

        FrameType frameType { FrameType::Unknown };
        const void* cCodePC { nullptr };
        ExecutableBase* executable { nullptr };
        JSObject* callee { nullptr };
        RegExp* regExp { nullptr };
#if ENABLE(WEBASSEMBLY)
        std::optional<Wasm::IndexOrName> wasmIndexOrName;
#endif
        std::optional<Wasm::CompilationMode> wasmCompilationMode;
        BytecodeIndex wasmOffset;

        struct CodeLocation {
            bool hasCodeBlockHash() const
            {
                return codeBlockHash.isSet();
            }

            bool hasBytecodeIndex() const
            {
                return !!bytecodeIndex;
            }

            bool hasExpressionInfo() const
            {
                return lineColumn.line != std::numeric_limits<unsigned>::max()
                    && lineColumn.column != std::numeric_limits<unsigned>::max();
            }

            // These attempt to be expression-level line and column number.
            LineColumn lineColumn { std::numeric_limits<unsigned>::max(), std::numeric_limits<unsigned>::max() };
            BytecodeIndex bytecodeIndex;
            CodeBlockHash codeBlockHash;
            JITType jitType { JITType::None };
            bool isRegExp { false };
        };

        CodeLocation semanticLocation;
        std::optional<std::pair<CodeLocation, CodeBlock*>> machineLocation; // This is non-null if we were inlined. It represents the machine frame we were inlined into.

        bool hasExpressionInfo() const { return semanticLocation.hasExpressionInfo(); }
        unsigned lineNumber() const
        {
            ASSERT(hasExpressionInfo());
            return semanticLocation.lineColumn.line;
        }
        unsigned columnNumber() const
        {
            ASSERT(hasExpressionInfo());
            return semanticLocation.lineColumn.column;
        }

        // These are function-level data.
        String nameFromCallee(VM&);
        String displayName(VM&);
        int NODELETE functionStartLine();
        unsigned NODELETE functionStartColumn();
        std::tuple<SourceProvider*, SourceID> sourceProviderAndID();
        String url();
    };

    struct UnprocessedStackTrace {
        MonotonicTime timestamp;
        Seconds stopwatchTimestamp;
        void* topPC;
        bool topFrameIsLLInt;
        void* llintPC;
        RegExp* regExp;
        Vector<UnprocessedStackFrame> frames;
    };

    struct StackTrace {
        MonotonicTime timestamp;
        Seconds stopwatchTimestamp;
        Vector<StackFrame> frames;
        StackTrace()
        { }
        StackTrace(StackTrace&& other)
            : timestamp(other.timestamp)
            , frames(WTF::move(other.frames))
        { }
    };

    // AUD1.K1 (SD18) v1 carrier-only capture predicate. GIL-on (flag-off OR
    // useThreadGIL, i.e. phase 1) this is unconditionally true: today's
    // rebind-on-acquisition behavior is UNCHANGED. GIL-off, only CARRIER
    // (main/embedder) threads may become m_jscExecutionThread — spawned TS
    // threads are NEVER captured (§A.1.7 v1 "carrier lites only, spawned
    // unsampled"); profiles omit spawned-thread samples (SD18). N-thread
    // capture (per-lite frame buffers + registry iteration under a §A.3
    // stop) is chartered post-ungil.
    //
    // KEYING (why ThreadManager::isJSThreadCurrent, not a lite-tid probe):
    // GIL-off the carrier lite's tid is a TM-allocated unique NONZERO TID
    // from the same 2^15 space as spawned threads (r9 F4 TID SUPERSESSION:
    // vmstate §6.7's "main carrier tid stays 0" is GIL-ON-ONLY), so tid==0
    // does NOT identify the carrier GIL-off — it is also what
    // currentButterflyTID() returns when NO lite is installed, which would
    // admit a spawned thread racing its lite install. isJSThreadCurrent()
    // ("true iff spawned Thread") has no such window: a spawned thread runs
    // setCurrentThreadState BEFORE registering/installing its lite and
    // before its JSLockHolder (ThreadObject.cpp threadMain), so every
    // notice* that can fire on a spawned thread already observes
    // isSpawned. A plain (never-spawned) embedder thread has no spawned
    // ThreadState and binds, matching today's GIL-on multi-embedder
    // rebind semantics restricted to carriers.
    //
    // NOTE (predicate keying, §A.1.3 r27): this keys on the PROCESS-level
    // pair {useJSThreads, useThreadGIL} (the gilOffProcess derivation), not
    // per-VM vm.m_gilOff — for a refusal-class tooling restriction the
    // process-level key is the conservative (strictly stronger) side; a
    // GIL-on VM inside a GIL-off process also samples carrier-only.
    // noticeCurrentThreadAsJSCExecutionThreadWithLock early-returns
    // (keeping the existing binding) when this is false — that consult is
    // WIRED (GIL-removal review round). The takeSample suspend-scope half
    // below remains PENDING, and takeSample stays DORMANT on gilOff VMs via
    // its entryScope gate (AB-22) until the per-lite Group-3 registry
    // resolve lands.
    static bool shouldBindCurrentThreadAsJSCExecutionThread()
    {
        if (!Options::useJSThreads() || Options::useThreadGIL())
            return true;
        return !ThreadManager::isJSThreadCurrent();
    }

    // r24 SUSPEND RULE enforcement vehicle (§A.1.7): MUST be instantiated
    // by takeSample() between a successful Thread::suspend() and the
    // Thread::resume() of the target — that instantiation is the
    // SamplingProfiler.cpp half of U-T8d and is NOT yet wired (PENDING;
    // table header note above). While alive, this thread may not allocate
    // (fastMalloc included; the target may hold the malloc lock) and may
    // not acquire any lock beyond locks already held at suspension
    // (registry lock carve-out) — all sample buffers (m_currentFrames) are
    // pre-allocated before suspension. Debug-enforced via WTF's per-thread
    // malloc-forbid scope; a release no-op, so GIL-on/flag-off behavior is
    // identical. Lock acquisition inside the window is the U-T8d
    // sample-storm corpus arm's job to catch (target spinning in
    // fastMalloc-heavy native code; TSAN + deadlock watchdog — distinct
    // from AUD1.K1's profiler-on + 2 threads U-T2 arm).
    class WhileTargetSuspendedScope {
        WTF_MAKE_NONCOPYABLE(WhileTargetSuspendedScope);
    public:
        WhileTargetSuspendedScope() = default;
    private:
        ForbidMallocUseForCurrentThreadScope m_forbidMalloc;
    };

    SamplingProfiler(VM&, Ref<Stopwatch>&&);
    JS_EXPORT_PRIVATE ~SamplingProfiler();
    void noticeJSLockAcquisition();
    void noticeVMEntry();
    void shutdown();
    template<typename Visitor> void visit(Visitor&) WTF_REQUIRES_LOCK(m_lock);
    Lock& getLock() LIFETIME_BOUND WTF_RETURNS_LOCK(m_lock) { return m_lock; }
    void setTimingInterval(Seconds interval) { m_timingInterval = interval; }
    JS_EXPORT_PRIVATE void start();
    void startWithLock() WTF_REQUIRES_LOCK(m_lock);
    Vector<StackTrace> releaseStackTraces() WTF_REQUIRES_LOCK(m_lock);
    JS_EXPORT_PRIVATE Ref<JSON::Value> stackTracesAsJSON();
    JS_EXPORT_PRIVATE void noticeCurrentThreadAsJSCExecutionThread();
    void noticeCurrentThreadAsJSCExecutionThreadWithLock() WTF_REQUIRES_LOCK(m_lock);
    void processUnverifiedStackTraces() WTF_REQUIRES_LOCK(m_lock);
    void setStopWatch(Ref<Stopwatch>&& stopwatch) WTF_REQUIRES_LOCK(m_lock) { m_stopwatch = WTF::move(stopwatch); }
    void pause() WTF_REQUIRES_LOCK(m_lock);
    void clearData() WTF_REQUIRES_LOCK(m_lock);

    // Used for debugging in the JSC shell/DRT.
    void registerForReportAtExit();
    void reportDataToOptionFile();
    JS_EXPORT_PRIVATE void reportTopFunctions();
    JS_EXPORT_PRIVATE void reportTopFunctions(PrintStream&);
    JS_EXPORT_PRIVATE void reportTopBytecodes();
    JS_EXPORT_PRIVATE void reportTopBytecodes(PrintStream&);

    JS_EXPORT_PRIVATE Thread* NODELETE thread() const;

private:
    void createThreadIfNecessary() WTF_REQUIRES_LOCK(m_lock);
    void timerLoop();
    void takeSample(Seconds& stackTraceProcessingTime) WTF_REQUIRES_LOCK(m_lock);

    Lock m_lock;
    bool m_isPaused WTF_GUARDED_BY_LOCK(m_lock);
    bool m_isShutDown WTF_GUARDED_BY_LOCK(m_lock);
    bool m_needsReportAtExit { false };
    VM& m_vm;
    WeakRandom m_weakRandom;
    Ref<Stopwatch> m_stopwatch WTF_GUARDED_BY_LOCK(m_lock);
    Vector<StackTrace> m_stackTraces WTF_GUARDED_BY_LOCK(m_lock);
    Vector<UnprocessedStackTrace> m_unprocessedStackTraces WTF_GUARDED_BY_LOCK(m_lock);
    Seconds m_timingInterval;
    RefPtr<Thread> m_thread;
    // AUD1.K1: GIL-off this must bind to CARRIER (main/embedder) threads
    // only (gate: shouldBindCurrentThreadAsJSCExecutionThread above —
    // consult in noticeCurrentThreadAsJSCExecutionThreadWithLock is the
    // PENDING .cpp half); spawned TS threads never become the sampled
    // thread (SD18). start/stop/shutdown/reports remain main-thread APIs
    // (SD13/SD14 family). GIL-on: unchanged (rebinds to whichever thread
    // last acquired the JSLock / entered).
    RefPtr<Thread> m_jscExecutionThread WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashSet<JSCell*> m_liveCellPointers WTF_GUARDED_BY_LOCK(m_lock);
    Vector<UnprocessedStackFrame> m_currentFrames WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace JSC

namespace WTF {

void printInternal(PrintStream&, JSC::SamplingProfiler::FrameType);

} // namespace WTF

#endif // ENABLE(SAMPLING_PROFILER)
