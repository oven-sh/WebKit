/*
 * Copyright (C) 2016-2025 Apple Inc. All rights reserved.
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

#if ENABLE(JIT)

#include "JITPlan.h"
#include "JITWorklistThread.h"
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class CodeBlock;
class VM;

class JITWorklist {
    WTF_MAKE_NONCOPYABLE(JITWorklist);
    WTF_MAKE_TZONE_ALLOCATED(JITWorklist);

    friend class JITWorklistThread;

public:
    ~JITWorklist();

    static JITWorklist& ensureGlobalWorklist();
    static JITWorklist* NODELETE existingGlobalWorklistOrNull();

    // THREADS §5.7.3 (SPEC-jit Task 12): enqueueing a plan whose key is already present
    // cancels the new plan under *m_lock and returns CompilationDeferred (dedup backstop
    // for racy tier-up triggers; not flag-gated).
    CompilationResult enqueue(Ref<JITPlan>);
    size_t queueLength() const;

    void suspendAllThreads();
    void resumeAllThreads();

    enum State { NotKnown, Compiling, Compiled };
    State compilationState(VM&, JITCompilationKey);

    State completeAllReadyPlansForVM(VM&, JITCompilationKey = JITCompilationKey());
    void requestTemporaryStop();

    // This is equivalent to:
    // worklist->waitUntilAllPlansForVMAreReady(vm);
    // worklist->completeAllReadyPlansForVM(vm);
    void completeAllPlansForVM(VM&);

    void cancelAllPlansForVM(VM&);

    void removeDeadPlans(VM&);

    unsigned NODELETE setMaximumNumberOfConcurrentDFGCompilations(unsigned);
    unsigned NODELETE setMaximumNumberOfConcurrentFTLCompilations(unsigned);

    // Only called on the main thread after suspending all threads.
    template<typename Visitor>
    void visitWeakReferences(Visitor&);

    template<typename Visitor>
    void iterateCodeBlocksForGC(Visitor&, VM&, NOESCAPE const Function<void(CodeBlock*)>&);

    void dump(PrintStream&) const;

private:
    JITWorklist();

    void wakeThreads(const AbstractLocker&, unsigned enqueuedTier);
    unsigned planLoad(JITPlan&);

    size_t queueLength(const AbstractLocker&) const;
    size_t NODELETE totalOngoingCompilations(const AbstractLocker&) const;

    void waitUntilAllPlansForVMAreReady(VM&);

    template<typename MatchFunction>
    void removeMatchingPlansForVM(VM&, const MatchFunction&);

    State removeAllReadyPlansForVM(VM&, Vector<Ref<JITPlan>, 8>&, JITCompilationKey);

    void dump(const AbstractLocker&, PrintStream&) const;

    unsigned m_numberOfActiveThreads { 0 };
    unsigned m_totalLoad { 0 }; // Total load of the queues and ongoing compilations
    std::array<unsigned, static_cast<size_t>(JITPlan::Tier::Count)> m_ongoingCompilationsPerTier { 0, 0, 0 };
    std::array<unsigned, static_cast<size_t>(JITPlan::Tier::Count)> m_maximumNumberOfConcurrentCompilationsPerTier;
    std::array<unsigned, static_cast<size_t>(JITPlan::Tier::Count)> m_loadWeightsPerTier;

    Vector<Ref<JITWorklistThread>> m_threads;

    // Used to inform the thread about what work there is left to do.
    std::array<Deque<RefPtr<JITPlan>>, static_cast<size_t>(JITPlan::Tier::Count)> m_queues;

    // Used to answer questions about the current state of a code block. This
    // is particularly great for the cti_optimize OSR slow path, which wants
    // to know: did I get here because a better version of me just got
    // compiled?
    UncheckedKeyHashMap<JITCompilationKey, RefPtr<JITPlan>> m_plans;

    // UNGIL AB18-R1-A/B: plans claimed for finalize (removed from m_plans,
    // install not yet published). Guarded by *m_lock. Two duties:
    //   (A) enqueue's dedup backstop keeps rejecting duplicate plans for the
    //       key through the removal->install window (formerly the file-local
    //       finalizingKeys() set in JITWorklist.cpp);
    //   (B) GC ROOTS: iterateCodeBlocksForGC / visitWeakReferences walk these
    //       plans too. GIL-off, Plan::finalize() has park points that release
    //       heap access (the GILOffCompilationLocker contended spin parks via
    //       parkSitePollAndParkForStopTheWorld; reallyAdd can fire a Class-A
    //       stop), so a sibling-conducted shared GC can run while the plan is
    //       out of m_plans — without this table its CodeBlock/alternative are
    //       swept under the finalizing mutator (mc-safe-gcwait-vs-classa-stop
    //       UAF family). GIL-on / flag-off: invariantly empty.
    UncheckedKeyHashMap<JITCompilationKey, RefPtr<JITPlan>> m_finalizingPlans;

    // Used to quickly find which plans have been compiled and are ready to
    // be completed.
    Vector<Ref<JITPlan>, 16> m_readyPlans;

    Lock m_suspensionLock;
    Box<Lock> m_lock;

    const Ref<AutomaticThreadCondition> m_planEnqueued;
    Condition m_planCompiledOrCancelled;
};

} // namespace JSC

#endif // ENABLE(JIT)
