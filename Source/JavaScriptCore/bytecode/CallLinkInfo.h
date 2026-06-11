/*
 * Copyright (C) 2012-2018 Apple Inc. All rights reserved.
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

#include "BaselineJITRegisters.h"
#include "CallFrame.h"
#include "CallFrameShuffleData.h"
#include "CallLinkInfoBase.h"
#include "CallMode.h"
#include "CodeLocation.h"
#include "CodeOrigin.h"
#include "CodeSpecializationKind.h"
#include "Options.h"
#include "PolymorphicCallStubRoutine.h"
#include "WriteBarrier.h"
#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/RecursiveLockAdapter.h>
#include <wtf/ScopedLambda.h>
#include <wtf/ThreadSanitizerSupport.h>

namespace JSC {
namespace DFG {
struct UnlinkedCallLinkInfo;
}

class CCallHelpers;
class ExecutableBase;
class FunctionCodeBlock;
class JSFunction;
class OptimizingCallLinkInfo;
class PolymorphicCallStubRoutine;
enum OpcodeID : unsigned;

struct CallFrameShuffleData;
struct UnlinkedCallLinkInfo;
struct BaselineUnlinkedCallLinkInfo;

using CompileTimeCallLinkInfo = Variant<OptimizingCallLinkInfo*, BaselineUnlinkedCallLinkInfo*, DFG::UnlinkedCallLinkInfo*>;

// SPEC-jit section 5.8 (Task 7): the single published call-link record.
//
// Guard/payload word-pair protocols are unsound under N mutators (a racing
// reader can pair a new guard with an old target), so with shared-memory
// threads enabled (Options::useJSThreads()) every JIT'd call fast path flows
// through ONE published pointer to an immutable record:
//
//   load r = m_record; if (!r) use the empty record (default call);
//   load c = r->comparand;
//   if (c == calleeGPR || (c & polymorphicCalleeMask)) {
//       store r->codeBlockToTransfer -> callee frame;
//       load t = r->target ONCE; call t;
//   } else use the empty record (default call);
//
// c == callee cell => monomorphic; c with bit 0 set (polymorphicCalleeMask)
// => always-call (virtual/polymorphic-stub dispatch, today's bit-test, G10);
// direct calls skip the comparand check entirely. All reads go THROUGH r so
// ARM64 readers are ordered by the address dependency (F2); a stale read
// observes a complete OLD record, which is benign.
//
// Records are immutable after publish (F6: fully initialize, then
// WTF::storeStoreFence(), then a single m_record pointer store), heap-allocated
// at link time, and freed only via RetiredJITArtifacts (SPEC-jit section 4.4)
// once every mutator has crossed a safepoint, except when the owning
// CallLinkInfo itself is destroyed (its code is already unreachable by then).
//
// GC: comparand is a RAW word - never dereferenced, never visited, no
// WriteBarrier. The legacy mirror fields (m_callee/m_codeBlock/
// m_monomorphicCallDestination; Direct: m_target/m_codeBlock) remain the sole
// GC roots/weak references and stay in sync with the record under the existing
// locks; visitWeak/unlinkOrUpgrade read the mirrors as today and additionally
// null or republish m_record on clear/relink.
struct CallLinkRecord {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CallLinkRecord);

    uintptr_t comparand { 0 }; // Callee cell, or sentinel: bit 0 (CallLinkInfo::polymorphicCalleeMask) = always-call.
    CodePtr<JSEntryPtrTag> target { }; // Entrypoint (monomorphic/virtual/stub/direct).
    CodeBlock* codeBlockToTransfer { nullptr }; // Stored to the callee frame by the fast path.

    static constexpr ptrdiff_t offsetOfComparand() { return OBJECT_OFFSETOF(CallLinkRecord, comparand); }
    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(CallLinkRecord, target); }
    static constexpr ptrdiff_t offsetOfCodeBlockToTransfer() { return OBJECT_OFFSETOF(CallLinkRecord, codeBlockToTransfer); }
};

#if CPU(ADDRESS64)
static_assert(CallLinkRecord::offsetOfComparand() == 0);
static_assert(CallLinkRecord::offsetOfTarget() == 8);
static_assert(CallLinkRecord::offsetOfCodeBlockToTransfer() == 16);
static_assert(sizeof(CallLinkRecord) == 24);
#endif

class CallLinkInfo : public CallLinkInfoBase {
public:
    friend class LLIntOffsetsExtractor;

    static constexpr uint8_t maxProfiledArgumentCountIncludingThisForVarargs = UINT8_MAX;

    enum class Type : uint8_t {
        DataOnly,
        Optimizing,
    };

    enum class Mode : uint8_t {
        Init,
        Monomorphic,
        Polymorphic,
        Virtual,
    };

    static constexpr uintptr_t polymorphicCalleeMask = 1;

    // AB18-D / GIL-removal precondition 11 (docs/threads/INTEGRATE-jit.md):
    // gilOff, ALL slow-path call-link transition writers — and every
    // m_incomingCalls push/remove they perform — serialize on this single
    // process-wide lock: linkMonomorphicCall / linkPolymorphicCall /
    // linkDirectCall (bytecode/Repatch.cpp) and unlinkOrUpgradeImpl (both the
    // CallLinkInfo and DirectCallLinkInfo flavors, which covers the upgrade
    // relink push and the per-node work of the non-STW
    // ScriptExecutable::installCode drain). ONE lock, not per-CodeBlock pairs:
    // call linking is a rare slow path, and a single lock is deadlock-free by
    // construction (holders never reach a safepoint poll). AB17c F4: now a
    // RECURSIVE lock — destruction-context removers (~CallLinkInfoBase on a
    // lazy-sweep mutator, reachable from allocation inside a LOCKED linker,
    // e.g. linkPolymorphicCallImpl's stub allocation sweeping a dying
    // CodeBlock) must also serialize on it, and the recursive acquire is the
    // only deadlock-free way to admit them. The takeFrom HEAD-rewrite
    // residual was closed by the AB18-C locker in
    // CodeBlock::unlinkOrUpgradeIncomingCalls. Static member: no layout
    // change.
    static RecursiveLock s_callLinkSerializationLock;


    static CallType NODELETE callTypeFor(OpcodeID opcodeID);

    static bool isVarargsCallType(CallType callType)
    {
        switch (callType) {
        case CallVarargs:
        case ConstructVarargs:
        case TailCallVarargs:
            return true;

        default:
            return false;
        }
    }

    ~CallLinkInfo();
    
    static CodeSpecializationKind specializationKindFor(CallType callType)
    {
        return specializationFromIsConstruct(callType == Construct || callType == ConstructVarargs || callType == DirectConstruct);
    }
    // TSAN wave 5 (calllink, ruling: concurrent-accessor): specializationKind/
    // callMode/type read the packed callType+type byte on DFG compiler threads
    // (CallLinkStatus::computeFor*, bytecode/Repatch.cpp) with no happens-before
    // against the byte's initialization writes once the LLInt metadata buffer
    // (DataOnlyCallLinkInfo lives in UnlinkedMetadataTable storage) or the
    // CallLinkInfo allocation is recycled — the r4 "specializationKind x
    // UnlinkedMetadataTable::link / CallLinkInfo ctor / DataOnly initialize"
    // report keys. The byte is write-once before publication (ctor /
    // initialize / setUpCall, all pre-reachability), so the fix shape is the
    // relaxed-atomic pair (same as the wave-4 m_owner fix), NOT a lock.
    // Flag-off codegen is unchanged: a relaxed byte load/store compiles to the
    // same plain byte moves as the old bit-field accesses.
    CodeSpecializationKind specializationKind() const
    {
        return specializationKindFor(callType());
    }

    CallMode callMode() const
    {
        return callModeFor(callType());
    }

    bool isTailCall() const
    {
        return callMode() == CallMode::Tail;
    }
    
    NearCallMode nearCallMode() const
    {
        return isTailCall() ? NearCallMode::Tail : NearCallMode::Regular;
    }

    bool isVarargs() const
    {
        return isVarargsCallType(callType());
    }

    bool isLinked() const { return mode() != Mode::Init && mode() != Mode::Virtual; }
    void unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock);

#if ENABLE(JIT)
protected:
    static void emitFastPathImpl(CallLinkInfo*, CCallHelpers&, bool isTailCall, ScopedLambda<void()>&& prepareForTailCall);
public:
    static void emitDataICFastPath(CCallHelpers&);
    static void emitTailCallDataICFastPath(CCallHelpers&, ScopedLambda<void()>&& prepareForTailCall);

    static void emitFastPath(CCallHelpers&, CompileTimeCallLinkInfo);
    static void emitTailCallFastPath(CCallHelpers&, CompileTimeCallLinkInfo, ScopedLambda<void()>&& prepareForTailCall);
#endif

    void NODELETE revertCallToStub();

    void setMonomorphicCallee(VM&, JSCell*, JSObject* callee, CodeBlock*, CodePtr<JSEntryPtrTag>);
    void NODELETE clearCallee();
    JSObject* NODELETE callee();

    void setLastSeenCallee(VM&, const JSCell* owner, JSObject* callee);
    JSObject* NODELETE lastSeenCallee() const;
    bool NODELETE haveLastSeenCallee() const;
    
    void setExecutableDuringCompilation(ExecutableBase*);
    ExecutableBase* executable();
    
    void setStub(VM&, Ref<PolymorphicCallStubRoutine>&&);
    void clearStub();

    void setVirtualCall(VM&);

    void revertCall(VM&);

    PolymorphicCallStubRoutine* stub() const
    {
        // SPEC-jit section 5.8 (Task 7): flag-on, a non-Polymorphic
        // CallLinkInfo may still retain a displaced routine in m_stub purely
        // to keep the pointer published for racing JIT'd thunk readers (see
        // clearStub()); logically there is no stub then.
        //
        // V7 code-lifecycle: flag-on, m_stub is published by setStub's atomic
        // release store, so racing readers load it atomically here. The
        // mode()-then-stub() pair has NO reader-side happens-before: this is
        // safe only because (a) every C++ caller null-checks the result and
        // (b) m_stub is never unpublished while the flag is on (clearStub's
        // keep-published rule), so a stale non-null pointer always names a
        // live routine.
        //
        // TSAN wave 3 (calllink, SPEC-jit 5.8/F6): the load is ACQUIRE, not
        // relaxed. The C++ consumers behind this accessor (CallLinkStatus on
        // a DFG compiler thread reading hasEdges()/edges()/variants(),
        // CallLinkInfo::forEachDependentCell on a concurrent marking thread)
        // read the routine's fields with NO address-dependency guarantee
        // from the C++ compiler, so they need a real synchronizes-with edge
        // against the routine's construction (which completes before
        // setStub's release publish). Acquire pairs with that release store
        // and orders the ctor's slot/header/vptr writes before every
        // field read through the returned pointer — this is the existing
        // concurrent-accessor fix shape, not a lock. The LLInt polymorphic
        // thunk's m_record->m_stub load pair (LowLevelInterpreter.asm) still
        // has no address/acquire dependency — that remains the IT-8
        // weak-memory residual; this change does NOT close it (asm is
        // outside TSAN's view; covered by the object-model protocol tests).
        if (Options::useJSThreads()) [[unlikely]] {
            if (mode() != Mode::Polymorphic)
                return nullptr;
            auto* stubSlot = std::bit_cast<PolymorphicCallStubRoutine**>(const_cast<RefPtr<PolymorphicCallStubRoutine>*>(&m_stub));
            return WTF::atomicLoad(stubSlot, std::memory_order_acquire);
        }
        return m_stub.get();
    }

    bool seenOnce()
    {
        return m_flags.loadRelaxed() & hasSeenShouldRepatchFlag;
    }

    void clearSeen()
    {
        m_flags.exchangeAnd(static_cast<uint8_t>(~hasSeenShouldRepatchFlag));
    }

    void setSeen()
    {
        m_flags.exchangeOr(hasSeenShouldRepatchFlag);
    }

    bool hasSeenClosure()
    {
        return m_flags.loadRelaxed() & hasSeenClosureFlag;
    }

    void setHasSeenClosure()
    {
        m_flags.exchangeOr(hasSeenClosureFlag);
    }

    bool clearedByGC()
    {
        return m_flags.loadRelaxed() & clearedByGCFlag;
    }

    void setClearedByGC()
    {
        m_flags.exchangeOr(clearedByGCFlag);
    }

    bool clearedByVirtual()
    {
        return m_flags.loadRelaxed() & clearedByVirtualFlag;
    }

    void setClearedByVirtual()
    {
        m_flags.exchangeOr(clearedByVirtualFlag);
    }
    
    void setCallType(CallType callType)
    {
        // TSAN wave 5 (calllink): relaxed RMW (load + store) preserving the
        // Type bit. The byte is write-once before this CallLinkInfo is
        // reachable by other lites, so the non-atomic RMW shape is sufficient;
        // each individual access is atomic so racing stale readers are
        // defined behavior, not UB.
        uint8_t packed = loadCallTypeAndType();
        WTF::atomicStore(&m_callTypeAndType, static_cast<uint8_t>((packed & ~callTypeBitsMask) | (static_cast<uint8_t>(callType) & callTypeBitsMask)), std::memory_order_relaxed);
    }

    CallType callType() const
    {
        return static_cast<CallType>(loadCallTypeAndType() & callTypeBitsMask);
    }

    static constexpr ptrdiff_t offsetOfMaxArgumentCountIncludingThisForVarargs()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_maxArgumentCountIncludingThisForVarargs);
    }

    // TSAN wave 5 (calllink, SPEC §5.7 racy-profiling tolerance): this word is
    // written by the LLInt C++ slow path (LLIntSlowPaths setUpCall varargs) on
    // N mutators and read by CallLinkStatus on DFG compiler threads, all
    // lock-free — pure profiling, where a lost or stale maximum only biases a
    // varargs-inlining heuristic. Relaxed atomic pair; the Baseline JIT's
    // store8 through offsetOfMaxArgumentCountIncludingThisForVarargs stays a
    // plain emitted store (documented §0 TSAN JIT-blindness tradeoff). Field
    // type/layout unchanged (uint8_t), so the emitted-code offset contract and
    // flag-off codegen (relaxed byte moves == plain byte moves) hold.
    uint32_t maxArgumentCountIncludingThisForVarargs()
    {
        return WTF::atomicLoad(&m_maxArgumentCountIncludingThisForVarargs, std::memory_order_relaxed);
    }

    void updateMaxArgumentCountIncludingThisForVarargs(unsigned argumentCountIncludingThisForVarargs)
    {
        if (WTF::atomicLoad(&m_maxArgumentCountIncludingThisForVarargs, std::memory_order_relaxed) < argumentCountIncludingThisForVarargs)
            WTF::atomicStore(&m_maxArgumentCountIncludingThisForVarargs, static_cast<uint8_t>(std::min<unsigned>(argumentCountIncludingThisForVarargs, maxProfiledArgumentCountIncludingThisForVarargs)), std::memory_order_relaxed);
    }

    static constexpr ptrdiff_t offsetOfSlowPathCount()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_slowPathCount);
    }

    static constexpr ptrdiff_t offsetOfCallee()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_callee);
    }

    static constexpr ptrdiff_t offsetOfCodeBlock()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_codeBlock);
    }

    static constexpr ptrdiff_t offsetOfMonomorphicCallDestination()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_monomorphicCallDestination);
    }

    static constexpr ptrdiff_t offsetOfStub()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_stub);
    }

    static constexpr ptrdiff_t offsetOfRecord()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_record);
    }

    uint32_t slowPathCount()
    {
        // TSAN wave 5 (calllink, SPEC §5.7 racy-profiling tolerance): the
        // writers are the LLInt asm slow-path counter bump and the JIT
        // slow-path thunk (both outside TSAN's view, §0 tradeoff); this C++
        // read runs on DFG compiler threads (CallLinkStatus). Relaxed atomic
        // load so the cross-thread read of a concurrently-bumped advisory
        // counter is defined; a stale count only skews the couldTakeSlowPath
        // heuristic. Field type/layout unchanged (asm names
        // CallLinkInfo::m_slowPathCount directly).
        return WTF::atomicLoad(&m_slowPathCount, std::memory_order_relaxed);
    }

    CodeOrigin codeOrigin() const { return m_codeOrigin; }

    // TSAN wave 3 (calllink, SPEC-jit 5.8): this can run on a concurrent
    // marking thread (CodeBlock visit) racing the locked slow-path linkers.
    // Single-load discipline throughout: stub() is loaded ONCE (it is an
    // always-reloading atomic accessor flag-on, so a second call after a
    // racing mode flip could return null — TOCTOU null deref); the callee
    // and last-seen-callee slots are loaded ONCE through the WriteBarrier
    // relaxed-atomic accessors. The callee slot may transiently hold the
    // always-call sentinel (polymorphicCalleeMask, bit 0) between a racing
    // setVirtualCall/setStub slot store and its mode publication — a raw
    // non-cell word that must never reach a GC functor, so it is skipped
    // (flag-off, single mutator: the sentinel is only ever observed with
    // mode Virtual/Polymorphic, which this isLinked()/stub() path already
    // excludes, so the guard never fires and behavior is unchanged).
    template<typename Functor>
    void forEachDependentCell(const Functor& functor) const
    {
        if (isLinked()) {
            if (auto* stub = this->stub())
                stub->forEachDependentCell(functor);
            else if (JSObject* callee = m_callee.get()) {
                if (!(std::bit_cast<uintptr_t>(callee) & polymorphicCalleeMask))
                    functor(callee);
            }
        }
        if (JSObject* lastSeenCallee = m_lastSeenCallee.get())
            functor(lastSeenCallee);
    }

    void visitWeak(VM&);

    // TSAN wave 5 (calllink): relaxed-atomic read of the packed
    // callType+type byte (see specializationKind() / the field comment).
    Type type() const { return static_cast<Type>((loadCallTypeAndType() >> typeBitShift) & 1); }

    Mode mode() const { return static_cast<Mode>(m_mode.loadRelaxed()); }

    // TSAN wave 4 (calllink, ruling: concurrent-accessor): m_owner reads can
    // run on threads with no TSAN-visible happens-before against the owning
    // object's initialization stores (the r3 "CallLinkInfo ctor x
    // ownerForSlowPath" pairs are allocator-reuse keyed on this address: the
    // old object's slow-path read vs the NEW object's ctor write after the
    // block is recycled). m_owner is write-once before this CallLinkInfo is
    // reachable by any other lite (ctor / pre-publication initialize), so a
    // relaxed atomic pair is the correct shape — codegen-identical to the
    // plain access flag-off, defined behavior flag-on. Not a lock.
    JSCell* owner() const { return WTF::atomicLoad(const_cast<JSCell**>(&m_owner), std::memory_order_relaxed); }

    JSCell* ownerForSlowPath(CallFrame* calleeFrame);

    JSGlobalObject* globalObjectForSlowPath(JSCell* owner);

    std::tuple<CodeBlock*, BytecodeIndex> retrieveCaller(JSCell* owner);

protected:
    CallLinkInfo(Type type, JSCell* owner, CodeOrigin codeOrigin)
        : CallLinkInfoBase(CallSiteType::CallLinkInfo)
        , m_codeOrigin(codeOrigin)
    {
        // TSAN wave 4 (calllink): relaxed-atomic write-once store, pairing
        // with the relaxed loads in owner()/ownerForSlowPath — see the
        // owner() comment (allocator-reuse ctor x reader pairs). The object
        // is not reachable by other lites during construction, so relaxed is
        // sufficient; flag-off codegen is identical to the plain init.
        WTF::atomicStore(&m_owner, owner, std::memory_order_relaxed);
        // TSAN wave 5 (calllink): relaxed-atomic write-once store of the
        // packed callType+type byte — same allocator-reuse report class as
        // m_owner ("CallLinkInfo ctor x specializationKind": a stale
        // compiler-thread reader's atomic load pairing with the recycled
        // block's new ctor write). See the specializationKind() comment.
        storeCallTypeAndType(CallType::None, type);
        ASSERT(type == this->type());
    }

    void reset(VM&);

    // SPEC-jit section 5.8 writers (F6): every transition (first link,
    // monomorphic upgrade, setVirtualCall/setStub) publishes a NEW immutable
    // record - init record -> storeStoreFence -> single m_record store - and
    // retires the replaced one via RetiredJITArtifacts (section 4.4). Unlink is
    // a single nullptr store (monotone; legal from a running slow path or
    // under STW). No-ops flag-off (I1: no records exist flag-off).
    void publishRecord(VM&, uintptr_t comparand, CodePtr<JSEntryPtrTag> target, CodeBlock* codeBlockToTransfer);
    void clearRecord(VM&);

    // V7 code-lifecycle: this used to be ONE packed bit-field word, so every
    // flag write was a plain RMW of the whole word. gilOff, a lock-free
    // slow-path setSeen() on lite A could load the word, lose lite B's
    // concurrent (locked) setStub()/setVirtualCall() m_mode update, and store
    // the stale mode back — a LOST mode publication, not just a TSAN report
    // (see CallLinkInfo.cpp publishRecord comment block). Split into
    // independent memory locations: the write-once identity fields share a
    // byte of their own (m_type at construction, m_callType at
    // initialize/setUpCall, both before this CallLinkInfo is reachable by
    // other lites; wave 5 made that byte's accesses relaxed-atomic — see
    // m_callTypeAndType below); the monotone flags become a relaxed-atomic flag byte
    // (RMWs via exchangeOr/exchangeAnd so visitWeak's writers cannot erase a
    // racing slow-path setSeen and vice versa); m_mode becomes its own atomic
    // byte. m_mode readers tolerate staleness: the record they gate is
    // published via publishRecord's fence + atomic exchange, and m_stub is
    // never unpublished flag-on (clearStub keep-published rule). Packing
    // m_callType/m_type into one byte keeps sizeof(CallLinkInfo) unchanged
    // (the frozen "+8B per call op" D7 budget note below still holds).
    static constexpr uint8_t hasSeenShouldRepatchFlag = 1 << 0;
    static constexpr uint8_t hasSeenClosureFlag = 1 << 1;
    static constexpr uint8_t clearedByGCFlag = 1 << 2;
    static constexpr uint8_t clearedByVirtualFlag = 1 << 3;
    Atomic<uint8_t> m_flags { 0 };
    // TSAN wave 5 (calllink, ruling: concurrent-accessor): m_callType (4 bits)
    // and m_type (1 bit) were plain bit-fields sharing this byte — every write
    // was a plain byte RMW racing the lock-free compiler-thread readers
    // (specializationKind/type/callMode via CallLinkStatus) across metadata-
    // buffer/allocator reuse. Repacked into one manually-masked byte accessed
    // exclusively through relaxed atomics (loadCallTypeAndType/
    // storeCallTypeAndType/setCallType). Same size, same write-once
    // discipline; flag-off codegen unchanged (relaxed byte moves).
    static constexpr uint8_t callTypeBitsMask = 0b1111;
    static constexpr unsigned typeBitShift = 4;
    uint8_t m_callTypeAndType { static_cast<uint8_t>(CallType::None) }; // bits 0-3: CallType, write-once before publication; bit 4: Type, write-once at construction.

    uint8_t loadCallTypeAndType() const
    {
        return WTF::atomicLoad(const_cast<uint8_t*>(&m_callTypeAndType), std::memory_order_relaxed);
    }
    void storeCallTypeAndType(CallType callType, Type type)
    {
        WTF::atomicStore(&m_callTypeAndType, static_cast<uint8_t>((static_cast<uint8_t>(callType) & callTypeBitsMask) | (static_cast<uint8_t>(type) << typeBitShift)), std::memory_order_relaxed);
    }

    Atomic<uint8_t> m_mode { static_cast<uint8_t>(Mode::Init) }; // Mode
    uint8_t m_maxArgumentCountIncludingThisForVarargs { 0 }; // For varargs: the profiled maximum number of arguments. For direct: the number of stack slots allocated for arguments.
    uint32_t m_slowPathCount { 0 };

    CodeBlock* m_codeBlock { nullptr }; // This is weakly held. And cleared whenever m_monomorphicCallDestination is changed.
    CodePtr<JSEntryPtrTag> m_monomorphicCallDestination { nullptr };
    WriteBarrier<JSObject> m_callee;
    WriteBarrier<JSObject> m_lastSeenCallee;
    RefPtr<PolymorphicCallStubRoutine> m_stub;
    JSCell* m_owner { nullptr };
    CodeOrigin m_codeOrigin { };
    // SPEC-jit section 5.8 frozen placement: appended as the LAST member of the
    // CallLinkInfo data (one shared offset for the data-IC fast path emitted by
    // emitFastPathImpl, serving both DataOnlyCallLinkInfo - LLInt/Baseline
    // bytecode metadata, +8B per call op, unconditional per D7 - and
    // OptimizingCallLinkInfo). Null = unlinked. Flag-off this stays null
    // forever and no emitted sequence reads it (I1). V7: typed Atomic —
    // layout-identical; publish/clear were already atomic exchanges through a
    // bit_cast, this just makes the type say so.
    Atomic<CallLinkRecord*> m_record { nullptr };
};

class DataOnlyCallLinkInfo final : public CallLinkInfo {
public:
    DataOnlyCallLinkInfo()
        : CallLinkInfo(Type::DataOnly, nullptr, CodeOrigin { })
    {
    }

    void initialize(VM&, CodeBlock*, CallType, CodeOrigin);
};

struct UnlinkedCallLinkInfo { };

struct BaselineUnlinkedCallLinkInfo : public JSC::UnlinkedCallLinkInfo {
    BytecodeIndex bytecodeIndex; // Currently, only used by baseline, so this can trivially produce a CodeOrigin.
    CodeLocationLabel<JSInternalPtrTag> doneLocation;

#if ENABLE(JIT)
    void setUpCall(CallLinkInfo::CallType) { }
#endif
};

#if ENABLE(JIT)

class DirectCallLinkInfo final : public CallLinkInfoBase {
    WTF_MAKE_NONCOPYABLE(DirectCallLinkInfo);
public:
    DirectCallLinkInfo(CodeOrigin codeOrigin, UseDataIC useDataIC, JSCell* owner, ExecutableBase* executable)
        : CallLinkInfoBase(CallSiteType::DirectCall)
        , m_codeOrigin(codeOrigin)
        , m_executable(executable)
    {
        // TSAN wave 5 (calllink): relaxed-atomic write-once store of the
        // packed callType+useDataIC byte — same allocator-reuse
        // "ctor x specializationKind" report class as CallLinkInfo (these
        // nodes stay reachable from a callee's incoming-calls drain past
        // their owner's death, and the recycled block's ctor write pairs
        // with a stale reader's atomic load). Flag-off codegen unchanged.
        storeCallTypeAndDataIC(CallType::None, useDataIC);
        // TSAN wave 4 (calllink): relaxed-atomic write-once m_owner store,
        // pairing with the relaxed load in owner() — same allocator-reuse
        // report class as CallLinkInfo (these nodes stay reachable from a
        // callee's incoming-calls drain past their owner's death).
        WTF::atomicStore(&m_owner, owner, std::memory_order_relaxed);
        // SPEC-jit I3/section 5.8: with shared-memory threads enabled, direct
        // calls must use data ICs - UseDataIC::No fast paths patch machine code
        // in place (repatchNearCall/replaceWithJump), which is forbidden under
        // concurrent execution (I2).
        RELEASE_ASSERT(!Options::useJSThreads() || useDataIC == UseDataIC::Yes);
    }

    ~DirectCallLinkInfo()
    {
        // AB17e F4 (object-lifetime closure): delist FIRST, before any member
        // teardown. The I16 argument below covers JIT'd FRAMES only — a
        // locked drain (CodeBlock::unlinkOrUpgradeIncomingCalls) reaches this
        // node through the CALLEE's incoming list even though the owner is
        // dead, and its unlinkOrUpgradeImpl reads m_target/m_codeBlock/
        // m_record. removeOnDestruction acquires the link lock
        // unconditionally gilOff, so we either delist before any drain
        // observes the node or block until the drain loop ends; after the
        // locked delist the object is unreachable from any list and the
        // teardown below cannot race a drain.
        if (g_jscConfig.gilOffProcess) [[unlikely]]
            removeOnDestruction();
        m_target = { };
        m_codeBlock = nullptr;
        // SPEC-jit section 5.8: a DirectCallLinkInfo is destroyed only once its
        // owning code is unreachable (post-R2 conservative scan / CodeBlock
        // sweep), so no JIT'd frame can still hold the record pointer (I16);
        // inline delete is sound here, unlike replacement/unlink which must go
        // through RetiredJITArtifacts.
        delete m_record.exchange(nullptr);
    }

    void setCallType(CallType callType)
    {
        // TSAN wave 5 (calllink): relaxed RMW (load + store) preserving the
        // UseDataIC bit; write-once before publication, each access atomic so
        // racing stale readers (repatchSpeculatively's compiler-thread
        // specializationKind across allocator reuse) are defined behavior.
        uint8_t packed = loadCallTypeAndDataIC();
        WTF::atomicStore(&m_callTypeAndDataIC, static_cast<uint8_t>((packed & ~callTypeBitsMask) | (static_cast<uint8_t>(callType) & callTypeBitsMask)), std::memory_order_relaxed);
    }

    CallType callType() const
    {
        return static_cast<CallType>(loadCallTypeAndDataIC() & callTypeBitsMask);
    }

    CallMode callMode() const
    {
        return callModeFor(callType());
    }

    bool isTailCall() const
    {
        return callMode() == CallMode::Tail;
    }

    CodeSpecializationKind specializationKind() const
    {
        return specializationFromIsConstruct(callType() == DirectConstruct);
    }

    void setSlowPathStart(CodeLocationLabel<JSInternalPtrTag> slowPathStart)
    {
        m_slowPathStart = slowPathStart;
    }

    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(DirectCallLinkInfo, m_target); };
    static constexpr ptrdiff_t offsetOfCodeBlock() { return OBJECT_OFFSETOF(DirectCallLinkInfo, m_codeBlock); };
    static constexpr ptrdiff_t offsetOfRecord() { return OBJECT_OFFSETOF(DirectCallLinkInfo, m_record); };

    // TSAN wave 4 (calllink): relaxed-atomic load of the write-once m_owner —
    // see CallLinkInfo::owner().
    JSCell* owner() const { return WTF::atomicLoad(const_cast<JSCell**>(&m_owner), std::memory_order_relaxed); }

    // AB18-F: only meaningful under CallLinkInfo::s_callLinkSerializationLock
    // gilOff, where it names the CodeBlock whose m_incomingCalls list this
    // node sits on whenever isOnList() (every push/remove of the node and
    // every m_codeBlock write happens under that lock gilOff).
    CodeBlock* codeBlock() const { return m_codeBlock; }

    void unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock);

    void visitWeak(VM&);

    CodeOrigin codeOrigin() const { return m_codeOrigin; }
    bool isDataIC() const { return static_cast<UseDataIC>((loadCallTypeAndDataIC() >> useDataICBitShift) & 1) == UseDataIC::Yes; }

    MacroAssembler::JumpList emitDirectFastPath(CCallHelpers&);
    MacroAssembler::JumpList emitDirectTailCallFastPath(CCallHelpers&, ScopedLambda<void()>&& prepareForTailCall);
    void setCallTarget(VM&, CodeBlock*, CodeLocationLabel<JSEntryPtrTag>);
    void NODELETE setMaxArgumentCountIncludingThis(unsigned);
    unsigned maxArgumentCountIncludingThis() const { return m_maxArgumentCountIncludingThis; }

    void reset(VM&);

    void validateSpeculativeRepatchOnMainThread(VM&);

private:
    CodeLocationLabel<JSInternalPtrTag> slowPathStart() const { return m_slowPathStart; }
    CodeLocationLabel<JSInternalPtrTag> fastPathStart() const { return m_fastPathStart; }

    void initialize();
    void repatchSpeculatively();

    // SPEC-jit section 5.8 (direct calls are data-IC-only flag-on, I3): record
    // publish/unlink mirroring CallLinkInfo::publishRecord/clearRecord. Direct
    // records carry no comparand (the fast path skips the comparand check).
    // No-ops flag-off. The VM& is the retiring mutator's VM (R4-2:
    // RetiredJITArtifacts resolves the epoch heap from it) — it must NOT be
    // derived from m_owner, which may be a dead cell by the time a drain
    // reaches this node through a callee's incoming-calls list (the
    // retireOptimizedJITCode leak keeps the node alive past its owner).
    void publishRecord(VM&, CodePtr<JSEntryPtrTag> target, CodeBlock* codeBlockToTransfer);
    void clearRecord(VM&);
    void retireRecord(VM&, CallLinkRecord*);

    CodeBlock* NODELETE retrieveCodeBlock(FunctionExecutable*);
    CodePtr<JSEntryPtrTag> retrieveCodePtr(const ConcurrentJSLocker&, CodeBlock*);

    // TSAN wave 5 (calllink, ruling: concurrent-accessor): m_callType (4 bits)
    // and m_useDataIC (1 bit) were plain bit-fields sharing this byte —
    // repacked into one manually-masked byte accessed exclusively through
    // relaxed atomics (see CallLinkInfo::m_callTypeAndType for the full
    // rationale). Same size; write-once before publication; flag-off codegen
    // unchanged (relaxed byte moves).
    static constexpr uint8_t callTypeBitsMask = 0b1111;
    static constexpr unsigned useDataICBitShift = 4;
    uint8_t m_callTypeAndDataIC { static_cast<uint8_t>(CallType::None) }; // bits 0-3: CallType; bit 4: UseDataIC.

    uint8_t loadCallTypeAndDataIC() const
    {
        return WTF::atomicLoad(const_cast<uint8_t*>(&m_callTypeAndDataIC), std::memory_order_relaxed);
    }
    void storeCallTypeAndDataIC(CallType callType, UseDataIC useDataIC)
    {
        WTF::atomicStore(&m_callTypeAndDataIC, static_cast<uint8_t>((static_cast<uint8_t>(callType) & callTypeBitsMask) | (static_cast<uint8_t>(useDataIC) << useDataICBitShift)), std::memory_order_relaxed);
    }

    unsigned m_maxArgumentCountIncludingThis { 0 };
    CodePtr<JSEntryPtrTag> m_target;
    CodeBlock* m_codeBlock { nullptr }; // This is weakly held. And cleared whenever m_target is changed.
    CodeOrigin m_codeOrigin { };
    CodeLocationLabel<JSInternalPtrTag> m_slowPathStart;
    CodeLocationLabel<JSInternalPtrTag> m_fastPathStart;
    CodeLocationDataLabelPtr<JSInternalPtrTag> m_codeBlockLocation;
    CodeLocationNearCall<JSInternalPtrTag> m_callLocation NO_UNIQUE_ADDRESS;
    JSCell* m_owner;
    ExecutableBase* m_executable { nullptr }; // This is weakly held. DFG / FTL CommonData already ensures this.
    // SPEC-jit section 5.8 frozen placement: LAST member. Null = unlinked;
    // flag-off this stays null forever (I1). V7: typed Atomic —
    // layout-identical (see CallLinkInfo::m_record).
    Atomic<CallLinkRecord*> m_record { nullptr };
};

class OptimizingCallLinkInfo final : public CallLinkInfo {
public:
    friend class CallLinkInfo;

    OptimizingCallLinkInfo()
        : CallLinkInfo(Type::Optimizing, nullptr, CodeOrigin { })
    {
    }

    OptimizingCallLinkInfo(CodeOrigin codeOrigin, JSCell* owner)
        : CallLinkInfo(Type::Optimizing, owner, codeOrigin)
    {
    }

    void setUpCall(CallType callType)
    {
        // TSAN wave 5 (calllink): routed through the relaxed-atomic packed
        // byte (write-once before this info is reachable by other lites).
        setCallType(callType);
    }

    void NODELETE initializeFromDFGUnlinkedCallLinkInfo(VM&, const DFG::UnlinkedCallLinkInfo&, CodeBlock*);

private:
    void emitFastPath(CCallHelpers&);
    void emitTailCallFastPath(CCallHelpers&, ScopedLambda<void()>&& prepareForTailCall);

    CodeLocationNearCall<JSInternalPtrTag> m_callLocation NO_UNIQUE_ADDRESS;
};

#endif

inline JSCell* CallLinkInfo::ownerForSlowPath(CallFrame* calleeFrame)
{
    // TSAN §4.4 retired-artifact audit (TSAN-RESULTS residual 1, closed
    // 2026-06-09): pairs with the TSAN_ANNOTATE_HAPPENS_BEFORE on this
    // CallLinkInfo at handler publication (publishHandlerChainHead,
    // PropertyInlineCache.cpp). A CallLinkInfo embedded in an
    // InlineCacheHandlerWithJSCall reaches the call slow paths
    // (operationDefaultCall et al.) materialized by the handler's JIT'd
    // stub, so the real ordering edge (handler ctor -> storeStoreFence ->
    // chain-head store -> dependent loads in the stub) is invisible to TSAN
    // and the ctor's plain init stores (incl. the DataOnlyCallLinkInfo
    // memset) would otherwise pair against this thread's later owner() load
    // / setMonomorphicCallee store. Lifetime is proven by the audit AS
    // AMENDED at the closeout final review (TSAN-TRIAGE §17.2 incl. row 16):
    // every flag-on deallocation of a published handler routes through
    // RetiredJITArtifacts::retireHandlerChain (which flag-on never frees —
    // epochCoversEveryJSThread); §5.8 records ride retireCallLinkRecord;
    // metadata-embedded DataOnlyCallLinkInfos are leaked with their
    // MetadataTable by ~CodeBlock's flag-on ref-escape (row 16 — previously
    // a bypass: the table teardown ran their destructors inline); and
    // ~CallLinkInfo otherwise runs only for unreachable owners under
    // s_callLinkSerializationLock. No-op outside TSAN builds.
    TSAN_ANNOTATE_HAPPENS_AFTER(this);
    // TSAN wave 4 (calllink): single relaxed-atomic load of the write-once
    // m_owner (see owner()); load ONCE so the null check and the return use
    // the same value.
    if (JSCell* owner = this->owner())
        return owner;

    // Right now, IC (Getter, Setter, Proxy IC etc.) / WasmToJS sets nullptr intentionally since we would like to share IC / WasmToJS thunk eventually.
    // However, in that case, each IC's data side will have CallLinkInfo.
    // At that time, they should have appropriate owner. So this is a hack only for now.
    // This should always works since IC only performs regular-calls and it never does tail-calls.
    return calleeFrame->callerFrame()->codeOwnerCell();
}

} // namespace JSC
