/*
 * Copyright (C) 2012-2019 Apple Inc. All rights reserved.
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

#include "JSExportMacros.h"
#include <wtf/Atomics.h>
#include <wtf/DebugHeap.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/PrintStream.h>
#include <wtf/ScopedLambda.h>
#include <wtf/SentinelLinkedList.h>
#include <wtf/ThreadSanitizerSupport.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC {

// AB18-G: serializes WatchpointSet MEMBERSHIP — every SentinelLinkedList
// link/unlink (WatchpointSet::add / WatchpointSet::take / ~WatchpointSet's
// drain / Watchpoint::~Watchpoint's remove() / the unlink step of
// fireAllWatchpoints / direct node removes such as
// AdaptiveInferredPropertyValueWatchpointBase::fire) plus the
// InlineWatchpointSet thin->fat inflation. Taken ONLY under
// Options::useJSThreads() (flag-off there is one mutator and this lock is
// never acquired, so the flag-off bench profile is unchanged).
//
// Why a single global lock: installs run on N-mutator IC-miss/compile slow
// paths holding only per-CodeBlock locks (e.g. stub->watchpointSet().add for
// SharedJITStubSet-shared stubs, Structure::addTransitionWatchpoint on the
// shared object model), while removals run from retire paths
// (InlineCacheHandler::disarmClearingWatchpointOnRetire via
// RetiredJITArtifacts::retireHandlerChain), lazy-sweep destruction
// (~CodeBlock -> aboutToDie -> m_watchpoint.reset()), and ~Watchpoint
// generally — and ~Watchpoint cannot name its set (a bare node unlink), so a
// per-set lock is unreachable from the destructor. All of these are slow
// paths; a global leaf lock is correct and cheap. LOCK RANK: strict leaf —
// no other lock is ever acquired while holding it, and watchpoint FIRING
// (which can run arbitrary code) never runs under it: Class-A fires are
// stop-conducted (no concurrent mutators) and fireAllWatchpoints only takes
// it for the per-node unlink, releasing it before fire().
JS_EXPORT_PRIVATE extern Lock g_watchpointMembershipLock;

namespace DFG {
struct ArrayBufferViewWatchpointAdaptor;
}

class VM;

class FireDetail {
    void* operator new(size_t) = delete;

public:
    virtual ~FireDetail() = default;
    // This can't be pure virtual as it breaks our Dumpable concept.
    // FIXME: Make this virtual after we stop suppporting the Montery Clang.
    virtual void dump(PrintStream&) const { }

    // SPEC-jit section 5.6 classification override for rare sites: a fire whose
    // detail returns true here is treated as Class B (data-only) for THIS fire
    // even when the set itself is Class A, i.e. it skips the stop-the-world
    // protocol under Options::useJSThreads(). Only override this when the fire
    // provably cannot invalidate any installed machine code (no Watchpoint on
    // the set jettisons/patches/unlinks code, directly or transitively).
    // Default false: every fire is Class A unless proven otherwise (I10).
    virtual bool fireIsDataOnly() const { return false; }
};

class JS_EXPORT_PRIVATE StringFireDetail final : public FireDetail {
public:
    StringFireDetail(const char* string)
        : m_string(string)
    {
    }
    
    void dump(PrintStream& out) const final;

private:
    explicit StringFireDetail(ClangVTableWorkaroundTag);

    const char* m_string;
};

class LazyFireDetail final : public FireDetail {
public:
    LazyFireDetail(ScopedLambda<void(PrintStream&)>& lambda)
        : m_lambda(lambda)
    {
    }

    void dump(PrintStream& out) const final { m_lambda(out); }

private:
    ScopedLambda<void(PrintStream&)>& m_lambda;
};

class WatchpointSet;

#define JSC_WATCHPOINT_TYPES_WITHOUT_JIT(macro) \
    macro(AdaptiveInferredPropertyValueStructure, AdaptiveInferredPropertyValueWatchpointBase::StructureWatchpoint) \
    macro(AdaptiveInferredPropertyValueProperty, AdaptiveInferredPropertyValueWatchpointBase::PropertyWatchpoint) \
    macro(CodeBlockJettisoning, CodeBlockJettisoningWatchpoint) \
    macro(LLIntPrototypeLoadAdaptiveStructure, LLIntPrototypeLoadAdaptiveStructureWatchpoint) \
    macro(FunctionRareDataAllocationProfileClearing, FunctionRareData::AllocationProfileClearingWatchpoint) \
    macro(CachedSpecialPropertyAdaptiveStructure, CachedSpecialPropertyAdaptiveStructureWatchpoint) \
    macro(StructureChainInvalidation, StructureChainInvalidationWatchpoint) \
    macro(ObjectAdaptiveStructure, ObjectAdaptiveStructureWatchpoint) \
    macro(Chained, ChainedWatchpoint) \

#if ENABLE(JIT)
#define JSC_WATCHPOINT_TYPES_WITHOUT_DFG(macro) \
    JSC_WATCHPOINT_TYPES_WITHOUT_JIT(macro) \
    macro(StructureTransitionPropertyInlineCacheClearing, StructureTransitionPropertyInlineCacheClearingWatchpoint) \
    macro(PropertyInlineCacheClearing, PropertyInlineCacheClearingWatchpoint)

#if ENABLE(DFG_JIT)
#define JSC_WATCHPOINT_TYPES(macro) \
    JSC_WATCHPOINT_TYPES_WITHOUT_DFG(macro) \
    macro(AdaptiveStructure, DFG::AdaptiveStructureWatchpoint)
#else
#define JSC_WATCHPOINT_TYPES(macro) \
    JSC_WATCHPOINT_TYPES_WITHOUT_DFG(macro)
#endif

#else
#define JSC_WATCHPOINT_TYPES(macro) \
    JSC_WATCHPOINT_TYPES_WITHOUT_JIT(macro)
#endif

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(Watchpoint);

class Watchpoint : public BasicRawSentinelNode<Watchpoint> {
    WTF_MAKE_NONCOPYABLE(Watchpoint);
    WTF_MAKE_NONMOVABLE(Watchpoint);
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(Watchpoint, Watchpoint);
public:
#define JSC_DEFINE_WATCHPOINT_TYPES(type, _) type,
    enum class Type : uint8_t {
        JSC_WATCHPOINT_TYPES(JSC_DEFINE_WATCHPOINT_TYPES)
    };
#undef JSC_DEFINE_WATCHPOINT_TYPES

    Watchpoint(Type type)
        : m_type(type)
    { }

    void operator delete(Watchpoint*, std::destroying_delete_t);

protected:
    ~Watchpoint();

private:
    friend class WatchpointSet;
    // ArrayBufferViewWatchpointAdaptor can fire watchpoints if it tries to attach a watchpoint to a view but can't allocate the ArrayBuffer.
    friend struct DFG::ArrayBufferViewWatchpointAdaptor;
    void fire(VM&, const FireDetail&);
    template<typename Func>
    void runWithDowncast(const Func&);

    Type m_type;
};

// Make sure that the state can be represented in 2 bits.
enum WatchpointState : uint8_t {
    ClearWatchpoint = 0,
    IsWatched = 1,
    IsInvalidated = 2
};

// SPEC-jit section 5.6 (I10): every watchpoint set is classified at
// construction.
//
// - InvalidatesCode ("Class A", the DEFAULT): firing the set may invalidate
//   installed machine code (jettison CodeBlocks, reset ICs, unlink calls).
//   Under Options::useJSThreads(), Class-A fires ALWAYS run with every mutator
//   stopped: WatchpointSet::fireAllSlow either fires inline when the world is
//   already stopped, or requests a stop via JSThreadsSafepoint::
//   stopTheWorldAndRun and fires inside the closure. There is deliberately NO
//   ">1 mutator" fast gate (G7: VM construction does not synchronize with an
//   in-flight inline fire). Non-owned/runtime sets get this default without
//   any call-site edits (P2).
//
// - DataOnly ("Class B", explicit opt-in at construction): firing only updates
//   plain data that no JIT'd code embeds; the fire runs exactly as today, with
//   no stop. Opt a set into Class B only when every Watchpoint that can ever
//   be added to it is provably code-invalidation-free. (A per-FIRE override
//   for rare sites exists on FireDetail::fireIsDataOnly().)
enum class WatchpointSetClassification : uint8_t {
    InvalidatesCode, // Class A
    DataOnly, // Class B
};

class InlineWatchpointSet;
class DeferredWatchpointFire;
class VM;

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(WatchpointSet);

class WatchpointSet : public ThreadSafeRefCounted<WatchpointSet> {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(WatchpointSet, WatchpointSet);
    friend class LLIntOffsetsExtractor;
    friend class DeferredWatchpointFire;
public:
    // FIXME: In many cases, it would be amazing if this *did* fire the watchpoints. I suspect that
    // this might be hard to get right, but still, it might be awesome.
    JS_EXPORT_PRIVATE ~WatchpointSet(); // Note that this will not fire any of the watchpoints; if you need to know when a WatchpointSet dies then you need a separate mechanism for this.

    static Ref<WatchpointSet> create(WatchpointState state, WatchpointSetClassification classification = WatchpointSetClassification::InvalidatesCode)
    {
        return adoptRef(*new WatchpointSet(state, classification));
    }

    // It is always safe to call this from the main thread.
    // It is also safe to call this from another thread. It may return an old
    // state. Generally speaking, a safe pattern to use in a concurrent compiler
    // thread is:
    // if (watchpoint.isValid()) {
    //     watch(watchpoint);
    //     do optimizations;
    // }
    WatchpointState state() const
    {
        // TSAN wave 2 (triage 3.6): racy-by-design read (JIT spec section 5.6); the
        // relaxed atomic load makes the C++ access defined without adding ordering
        // (states are monotonic, OM spec section 5; stale values are tolerated and
        // re-checked under the Class-A stop, I11).
        // TSAN r11 (reports 14/15/26/27, ctor-vs-compiler-probe): pairs with
        // the HAPPENS_BEFORE at the end of the WatchpointSet constructor —
        // a freshly created set is consume-published (release CAS / fence +
        // relaxed pointer store) and probed lock-free by compiler threads;
        // TSAN cannot model the consume edge, so it pairs the probe against
        // the creator's malloc/ctor writes. The pair is trivially sound: the
        // constructor of any set a reader can reach ran before the reader
        // obtained the pointer. No-op outside TSAN.
        TSAN_ANNOTATE_HAPPENS_AFTER(this);
        return m_state.loadRelaxed();
    }
    
    // It is safe to call this from another thread.  It may return true
    // even if the set actually had been invalidated, but that ought to happen
    // only in the case of races, and should be rare. Guarantees that if you
    // call this after observing something that must imply that the set is
    // invalidated, then you will see this return false. This is ensured by
    // issuing a load-load fence prior to querying the state.
    bool isStillValid() const
    {
        return state() != IsInvalidated;
    }
    // Like isStillValid(), may be called from another thread.
    bool hasBeenInvalidated() const { return !isStillValid(); }
    
    // As a convenience, this will ignore 0. That's because code paths in the DFG
    // that create speculation watchpoints may choose to bail out if speculation
    // had already been terminated.
    void NODELETE add(Watchpoint*);
    
    // Force the watchpoint set to behave as if it was being watched even if no
    // watchpoints have been installed. This will result in invalidation if the
    // watchpoint would have fired. That's a pretty good indication that you
    // probably don't want to set watchpoints, since we typically don't want to
    // set watchpoints that we believe will actually be fired.
    void startWatching()
    {
        ASSERT(m_state.loadRelaxed() != IsInvalidated);
        if (m_state.loadRelaxed() == IsWatched)
            return;
        WTF::storeStoreFence();
        m_state.storeRelaxed(IsWatched);
        WTF::storeStoreFence();
    }

    template <typename T>
    void fireAll(VM& vm, T& fireDetails)
    {
        if (m_state.loadRelaxed() != IsWatched) [[likely]]
            return;
        fireAllSlow(vm, fireDetails);
    }

    void touch(VM& vm, const FireDetail& detail)
    {
        if (state() == ClearWatchpoint)
            startWatching();
        else
            fireAll(vm, detail);
    }
    
    void touch(VM& vm, const char* reason)
    {
        touch(vm, StringFireDetail(reason));
    }
    
    void invalidate(VM& vm, const FireDetail& detail)
    {
        if (state() == IsWatched)
            fireAll(vm, detail);
        m_state.storeRelaxed(IsInvalidated);
    }
    
    void invalidate(VM& vm, const char* reason)
    {
        invalidate(vm, StringFireDetail(reason));
    }
    
    bool isBeingWatched() const
    {
        // Racy-by-design read from compiler threads (same family as state();
        // triage 3.6): relaxed atomic, no ordering implied.
        return m_setIsNotEmpty.loadRelaxed();
    }

    // SPEC-jit section 5.6 / I10: classification is fixed at construction.
    WatchpointSetClassification classification() const
    {
        return m_invalidatesCode.loadRelaxed() ? WatchpointSetClassification::InvalidatesCode : WatchpointSetClassification::DataOnly;
    }
    bool invalidatesCompiledCode() const { return m_invalidatesCode.loadRelaxed(); }

    static constexpr ptrdiff_t offsetOfState() { return OBJECT_OFFSETOF(WatchpointSet, m_state); }

    JS_EXPORT_PRIVATE void fireAllSlow(VM&, const FireDetail&); // Call only if you've checked isWatched.
    JS_EXPORT_PRIVATE void fireAllSlow(VM&, DeferredWatchpointFire* deferredWatchpoints); // Ditto.
    JS_EXPORT_PRIVATE void fireAllSlow(VM&, const char* reason); // Ditto.

protected:
    JS_EXPORT_PRIVATE WatchpointSet(WatchpointState, WatchpointSetClassification = WatchpointSetClassification::InvalidatesCode);

private:
    void fireAllWatchpoints(VM&, const FireDetail&);
    void NODELETE take(WatchpointSet* other);

    // Today's fire body (invalidate state + fire every Watchpoint + F4 fence
    // pair), with no stop-the-world protocol. Flag-off this IS fireAllSlow;
    // flag-on it runs only world-stopped (Class A) or for Class-B fires.
    void fireAllNow(VM&, const FireDetail&);

    // SPEC-jit section 5.6 Class-A protocol: (1) world already stopped =>
    // fire inline; (2) else enqueue on the coalescing queue and request a stop
    // via JSThreadsSafepoint::stopTheWorldAndRun; (3) the draining closure
    // re-checks state() == IsWatched (I11); (4) runs the existing fire body;
    // (5) jettisons triggered by watchpoints run in the SAME closure (they hit
    // CodeBlock::jettison's R1.h already-stopped path); (6) on return the fire
    // is COMPLETE (synchronous completion; RELEASE_ASSERTed).
    void fireAllUnderClassAStop(VM&, const FireDetail&);

    // Coalescing (REQUIRED, section 5.6): drains EVERY queued Class-A fire in
    // one stop; concurrent losers' stopTheWorldAndRun returns only after their
    // queued fire ran (either in the winner's stop or their own).
    static void drainClassAFireQueue();

    friend class InlineWatchpointSet;

    // TSAN wave 2 (triage 3.6): m_state and m_setIsNotEmpty are read racily BY
    // DESIGN from compiler threads and other mutators (JIT spec section 5.6; OM spec
    // section 5: states monotonic, fires only under the Class-A stop, I13). Relaxed
    // Atomic makes those accesses defined C++ without changing flag-off codegen
    // (a relaxed byte load/store compiles to a plain byte load/store); ALL
    // ordering still comes from the pre-existing explicit fences and the
    // stop-the-world protocol, exactly as before. The JIT/LLInt load m_state
    // as a raw byte at offsetOfState(); the static_assert below the class
    // pins that layout.
    // TSAN wave 5 (triage 12.6, REOPENED family 9): ALL THREE members are
    // initialized via relaxed stores in the constructor BODY, not via the
    // Atomic value constructor — the value constructor is a plain (non-atomic)
    // store, and a freshly inflateSlow'd fat set is reachable by lock-free
    // compiler-thread readers as soon as the thin->fat word is published, so
    // the construction writes themselves must be atomic accesses.
    // m_invalidatesCode is in the same boat: it is read lock-free from
    // compiler threads (InlineWatchpointSet::invalidatesCompiledCode ->
    // fat(data)->invalidatesCompiledCode()), so it gets the same relaxed
    // Atomic treatment (still one byte; relaxed byte accesses compile to plain
    // byte accesses, flag-off codegen unchanged). It is immutable after
    // construction (I10) except for the deferred-fire take() transfer.
    Atomic<WatchpointState> m_state;
    Atomic<bool> m_setIsNotEmpty;
    Atomic<int8_t> m_invalidatesCode; // SPEC-jit section 5.6 classification bit; immutable after construction (I10).

    SentinelLinkedList<Watchpoint, BasicRawSentinelNode<Watchpoint>> m_set;
};

// JIT (branch8 at offsetOfState) and LLInt (bbeq WatchpointSet::m_state[...])
// load the state as a single raw byte; the Atomic wrapper must not change that.
static_assert(sizeof(Atomic<WatchpointState>) == 1);

// InlineWatchpointSet is a low-overhead, non-copyable watchpoint set in which
// it is not possible to quickly query whether it is being watched in a single
// branch. There is a fairly simple tradeoff between WatchpointSet and
// InlineWatchpointSet:
//
// Do you have to emit JIT code that rapidly tests whether the watchpoint set
// is being watched?  If so, use WatchpointSet.
//
// Do you need multiple parties to have pointers to the same WatchpointSet?
// If so, use WatchpointSet.
//
// Do you have to allocate a lot of watchpoint sets?  If so, use
// InlineWatchpointSet unless you answered "yes" to the previous questions.
//
// InlineWatchpointSet will use just one pointer-width word of memory unless
// you actually add watchpoints to it, in which case it internally inflates
// to a pointer to a WatchpointSet, and transfers its state to the
// WatchpointSet.

class InlineWatchpointSet {
    WTF_MAKE_NONCOPYABLE(InlineWatchpointSet);
public:
    InlineWatchpointSet(WatchpointState state, WatchpointSetClassification classification = WatchpointSetClassification::InvalidatesCode)
        : m_data { encodeState(state) | (classification == WatchpointSetClassification::DataOnly ? ClassBFlag : 0) }
    {
    }

    ~InlineWatchpointSet()
    {
        if (isThin())
            return;
        freeFat();
    }
    
    // See comment about state() in Watchpoint above.
    WatchpointState state() const
    {
        uintptr_t data = m_data.loadRelaxed();
        if (isFat(data))
            return consumeFat(data)->state();
        return decodeState(data);
    }
    
    // It is safe to call this from another thread.  It may return false
    // even if the set actually had been invalidated, but that ought to happen
    // only in the case of races, and should be rare.
    bool hasBeenInvalidated() const
    {
        return state() == IsInvalidated;
    }
    
    // Like hasBeenInvalidated(), may be called from another thread.
    bool isStillValid() const
    {
        return !hasBeenInvalidated();
    }
    
    void add(Watchpoint*);
    
    void startWatching()
    {
        if (isFat()) {
            protect(fat())->startWatching();
            return;
        }
        ASSERT(decodeState(m_data.loadRelaxed()) != IsInvalidated);
        setThinState(IsWatched);
    }

    // SPEC-jit section 5.6 interception note: the fat case delegates to
    // WatchpointSet::fireAll => fireAllSlow, which carries the Class-A
    // stop-the-world protocol under Options::useJSThreads(). The thin case
    // needs no stop: a thin set has NO Watchpoints installed, and installed
    // machine code can only depend on a set through a Watchpoint (DFG/FTL
    // DesiredWatchpoints inflate at finalization), so a thin fire cannot
    // invalidate any code. Compile-time-only consumers use the
    // isStillValid()/re-check pattern, which the plain invalidating store
    // (plus fence) already serves, exactly as today.
    template <typename T>
    void fireAll(VM& vm, T fireDetails)
    {
        if (isFat()) {
            protect(fat())->fireAll(vm, fireDetails);
            return;
        }
        if (decodeState(m_data.loadRelaxed()) == ClearWatchpoint)
            return;
        setThinState(IsInvalidated);
        WTF::storeStoreFence();
    }

    void invalidate(VM& vm, const FireDetail& detail)
    {
        if (isFat())
            protect(fat())->invalidate(vm, detail);
        else
            setThinState(IsInvalidated);
    }

    // SPEC-jit section 5.6 / I10. Thin: the construction-time classification
    // bit; fat: the inflated set's bit (transferred at inflateSlow).
    bool invalidatesCompiledCode() const
    {
        uintptr_t data = m_data.loadRelaxed();
        if (isFat(data))
            return consumeFat(data)->invalidatesCompiledCode();
        return !(data & ClassBFlag);
    }
    
    JS_EXPORT_PRIVATE void fireAll(VM&, const char* reason);
    
    void touch(VM& vm, const FireDetail& detail)
    {
        if (isFat()) {
            protect(fat())->touch(vm, detail);
            return;
        }
        uintptr_t data = m_data.loadRelaxed();
        if (decodeState(data) == IsInvalidated)
            return;
        WTF::storeStoreFence();
        if (decodeState(data) == ClearWatchpoint)
            setThinState(IsWatched);
        else
            setThinState(IsInvalidated);
        WTF::storeStoreFence();
    }
    
    void touch(VM& vm, const char* reason)
    {
        touch(vm, StringFireDetail(reason));
    }

    // Note that for any watchpoint that is visible from the DFG, it would be incorrect to write code like:
    //
    // if (w.isBeingWatched())
    //     w.fireAll()
    //
    // Concurrently to this, the DFG could do:
    //
    // if (w.isStillValid())
    //     perform optimizations;
    // if (!w.isStillValid())
    //     retry compilation;
    //
    // Note that the DFG algorithm is widespread, and sound, because fireAll() and invalidate() will leave
    // the watchpoint in a !isStillValid() state. Hence, if fireAll() or invalidate() interleaved between
    // the first isStillValid() check and the second one, then it would simply cause the DFG to retry
    // compilation later.
    //
    // But, if you change some piece of state that the DFG might optimize for, but invalidate the
    // watchpoint by doing:
    //
    // if (w.isBeingWatched())
    //     w.fireAll()
    //
    // then the DFG would never know that you invalidated state between the two checks.
    //
    // There are two ways to work around this:
    //
    // - Call fireAll() without a isBeingWatched() check. Then, the DFG will know that the watchpoint has
    //   been invalidated when it does its second check.
    //
    // - Do not expose the watchpoint set to the DFG directly, and have your own way of validating whether
    //   the assumptions that the DFG thread used are still valid when the DFG code is installed.
    bool isBeingWatched() const
    {
        uintptr_t data = m_data.loadRelaxed();
        if (isFat(data))
            return consumeFat(data)->isBeingWatched();
        return false;
    }

    // We expose this because sometimes a client knows its about to start
    // watching this InlineWatchpointSet, hence it'll become inflated regardless.
    // Such clients may find it useful to have a WatchpointSet* pointer, for example,
    // if they collect a Vector of WatchpointSet*.
    WatchpointSet* inflate()
    {
        if (isFat()) [[likely]]
            return fat();
        return inflateSlow();
    }
    
private:
    static constexpr uintptr_t IsThinFlag        = 1;
    static constexpr uintptr_t StateMask         = 6;
    static constexpr uintptr_t StateShift        = 1;
    // SPEC-jit section 5.6 classification bit (thin encoding ONLY: a fat
    // pointer is >= 8-byte aligned so bits 0-2 are zero, but bit 3 of a real
    // pointer is arbitrary; never consult ClassBFlag unless isThin()).
    static constexpr uintptr_t ClassBFlag        = 8;

    static bool isThin(uintptr_t data) { return data & IsThinFlag; }
    static bool isFat(uintptr_t data) { return !isThin(data); }

    // Every thin state store preserves the classification bit (I10).
    // Thin-state stores require the owner's serialization (as before); only
    // the word access itself is (relaxed) atomic so concurrent lock-free
    // readers are defined C++ (triage 3.6).
    void setThinState(WatchpointState state)
    {
        ASSERT(isThin());
        m_data.storeRelaxed(encodeState(state) | (m_data.loadRelaxed() & ClassBFlag));
    }

    static WatchpointState decodeState(uintptr_t data)
    {
        ASSERT(isThin(data));
        return static_cast<WatchpointState>((data & StateMask) >> StateShift);
    }
    
    static uintptr_t encodeState(WatchpointState state)
    {
        return (static_cast<uintptr_t>(state) << StateShift) | IsThinFlag;
    }
    
    bool isThin() const { return isThin(m_data.loadRelaxed()); }
    bool isFat() const { return isFat(m_data.loadRelaxed()); };
    
    static WatchpointSet* fat(uintptr_t data)
    {
        return std::bit_cast<WatchpointSet*>(data);
    }

    // TSAN wave 5 (triage 12.6, REOPENED family 9): consume-ordered read of
    // the fat pointer. inflateSlow publishes the fat pointer with a release
    // CAS, but lock-free readers load m_data RELAXED — without an ordering
    // edge on the reader side, the WatchpointSet's construction writes are not
    // ordered before the dereference and a compiler thread can observe a
    // mid-init set. Dependency::fence(data).consume(...) carries an address
    // dependency from the m_data load to every load through the returned
    // pointer (the mythical C++ consume), pairing with the release publish at
    // zero ordering cost: a compiler fence on TSO, a self-eor on ARM. No
    // acquire load, so flag-off fast paths keep plain loads.
    static WatchpointSet* consumeFat(uintptr_t data)
    {
        ASSERT(isFat(data));
        return Dependency::fence(data).consume(fat(data));
    }

    WatchpointSet* fat()
    {
        uintptr_t data = m_data.loadRelaxed();
        ASSERT(isFat(data));
        return consumeFat(data);
    }

    const WatchpointSet* fat() const
    {
        uintptr_t data = m_data.loadRelaxed();
        ASSERT(isFat(data));
        return consumeFat(data);
    }

    JS_EXPORT_PRIVATE WatchpointSet* inflateSlow();
    JS_EXPORT_PRIVATE void NODELETE freeFat();

    // TSAN wave 2 (triage 3.6): the thin/fat word is read lock-free from
    // compiler threads and other mutators while inflateSlow publishes the fat
    // pointer; relaxed Atomic word, with the inflateSlow publish a release CAS
    // (the WatchpointSet's contents are release-ordered before the fat pointer
    // becomes visible). Flag-off codegen is unchanged: relaxed word
    // loads/stores compile to plain loads/stores.
    Atomic<uintptr_t> m_data;
};

// SPEC-jit section 5.6 deferral: the deferred fireAllSlow overload behaves as
// today (invalidate the source set immediately, transfer its Watchpoints here;
// callers may hold locks at that point — that is the reason deferral exists).
// The actual FIRE happens at the holder's scope exit, after every lock has
// been dropped, via m_watchpointsToFire.fireAll(...) => fireAllSlow, which is
// where the Class-A stop-the-world protocol runs (lock-free by construction).
// take() transfers the source set's classification so a deferred Class-B fire
// stays Class B.
class DeferredWatchpointFire {
    WTF_MAKE_NONCOPYABLE(DeferredWatchpointFire);
public:
    DeferredWatchpointFire()
        : m_watchpointsToFire(ClearWatchpoint)
    {
    }

    JS_EXPORT_PRIVATE void NODELETE takeWatchpointsToFire(WatchpointSet*);

protected:
    WatchpointSet& watchpointsToFire() { return m_watchpointsToFire; }

private:
    WatchpointSet m_watchpointsToFire;
};

} // namespace JSC

namespace WTF {

void printInternal(PrintStream& out, JSC::WatchpointState);

} // namespace WTF

