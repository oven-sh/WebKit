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

#include "config.h"
#include "CallLinkInfo.h"

#include "BaselineJITRegisters.h"
#include "CCallHelpers.h"
#include "CallFrameShuffleData.h"
#include "DFGJITCode.h"
#include "DisallowMacroScratchRegisterUsage.h"
#include "FunctionCodeBlock.h"
#include "HeapInlines.h"
#include "JITThunks.h"
#include "JSCellInlines.h"
#include "JSThreadsSafepoint.h"
#include "JSWebAssemblyModule.h"
#include "LLIntEntrypoint.h"
#include "LinkBuffer.h"
#include "Opcode.h"
#include "Repatch.h"
#include "RetiredJITArtifacts.h"
#include "ThunkGenerators.h"
#include <mutex>
#include <wtf/Atomics.h>
#include <wtf/ListDump.h>
#include <wtf/NeverDestroyed.h>

namespace JSC {

namespace {

// SPEC-jit section 5.8 retirement: a replaced/unlinked CallLinkRecord is pure
// data; a racing JIT'd fast path may have loaded the old record pointer but
// (per the safepoint-free-window rule G2/I16) cannot hold it across a
// safepoint, so destroying it at epoch expiry is sound. The record's target
// entrypoint stays alive independently via the stub routine refs / CodeBlock
// ownership it was created from (machine code is freed only via section 4.4 /
// R2, never by this path).
class RetiredCallLinkRecord final : public RetiredCallback {
public:
    explicit RetiredCallLinkRecord(CallLinkRecord* record)
        : m_record(record)
    {
    }

    ~RetiredCallLinkRecord() final
    {
        delete m_record;
    }

private:
    CallLinkRecord* m_record;
};

// R4-2 (review round 4): takes VM&, not Heap& — RetiredJITArtifacts resolves
// the epoch heap (the client's SERVER under useSharedGCHeap) internally so a
// client VM's retirement never rides its own idle heap's epoch.
void retireCallLinkRecord(VM& vm, CallLinkRecord* record)
{
    if (!record)
        return;
    RetiredJITArtifacts::retire(vm, std::unique_ptr<RetiredCallback>(new RetiredCallLinkRecord(record)));
}

} // anonymous namespace

// AB18-D: see the declaration in CallLinkInfo.h. Defined here so the
// transition writers in this file and in bytecode/Repatch.cpp share one lock.
RecursiveLock CallLinkInfo::s_callLinkSerializationLock;

// SPEC-jit section 5.8 (F6): publish = fully initialize the new immutable
// record, storeStoreFence, then ONE pointer store. Readers are JIT'd fast
// paths that address-depend through the loaded record pointer (F2), so a
// stale read observes a complete OLD record - benign. The replaced record is
// retired via the epoch facility, never freed inline.
//
// WRITER-WRITER serialization (review round 3, R3-3): the publish protocol
// above is safe against concurrent READERS only. The WRITERS — the slow-path
// linkers (linkMonomorphicCall / setVirtualCall / setStub / linkDirectCall in
// jit/Repatch.cpp) — are unserialized: the std::exchange on the plain m_record
// is NOT atomic, and under N mutators two threads taking the same unlinked
// call site's slow path concurrently could both observe the SAME oldRecord and
// retire it twice (double-delete at epoch expiry), besides tearing the
// unsynchronized m_callee/m_codeBlock/m_mode mirror writes and
// setLastSeenCallee. Sound today ONLY under the phase-1 GIL (single mutator).
// Before GIL removal, slow-path call linking must be serialized per
// CallLinkInfo (e.g. the owner CodeBlock::m_lock around the set* entry points,
// with the m_record swap made a CAS so a losing linker retires its OWN new
// record) — recorded as GIL-removal precondition 11 in
// docs/threads/INTEGRATE-jit.md and tripwired by gilRemovalPreconditionsMet().
//
// AB18-D: precondition 11 is now landed — the slow-path linkers serialize on
// CallLinkInfo::s_callLinkSerializationLock (gilOff), and the m_record swap
// below is an ATOMIC exchange so two racing publishes can never observe the
// same oldRecord (no double-retire) even on a writer path the lock audit
// missed. Each loser retires a distinct displaced record.
void CallLinkInfo::publishRecord(VM& vm, uintptr_t comparand, CodePtr<JSEntryPtrTag> target, CodeBlock* codeBlockToTransfer)
{
    if (!Options::useJSThreads()) [[likely]]
        return;
    auto* record = new CallLinkRecord { comparand, target, codeBlockToTransfer };
    WTF::storeStoreFence();
    auto* oldRecord = m_record.exchange(record);
    retireCallLinkRecord(vm, oldRecord);
}

void CallLinkInfo::clearRecord(VM& vm)
{
    // Unlink: a single nullptr store (monotone). Legal from a running slow
    // path or under STW; always-call (virtual/stub) records are only unlinked
    // under STW/GC (their callers - reset/visitWeak-driven unlinkOrUpgrade -
    // run there, asserted by the section 5.3 machinery).
    if (!Options::useJSThreads()) [[likely]] {
        ASSERT(!m_record.loadRelaxed());
        return;
    }
    // AB18-D: atomic for the same no-double-retire reason as publishRecord.
    auto* oldRecord = m_record.exchange(nullptr);
    retireCallLinkRecord(vm, oldRecord);
}

CallLinkInfo::CallType CallLinkInfo::callTypeFor(OpcodeID opcodeID)
{
    switch (opcodeID) {
    case op_tail_call_varargs:
        return TailCallVarargs;

    case op_call:
    case op_call_ignore_result:
    case op_call_direct_eval:
    case op_iterator_open:
    case op_iterator_next:
        return Call;

    case op_call_varargs:
        return CallVarargs;

    case op_construct:
    case op_super_construct:
        return Construct;

    case op_construct_varargs:
    case op_super_construct_varargs:
        return ConstructVarargs;

    case op_tail_call:
        return TailCall;

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return Call;
}

CallLinkInfo::~CallLinkInfo()
{
    // AB17e F4 (object-lifetime closure of precondition 11): gilOff,
    // DELISTING IS THE FIRST ACT OF DESTRUCTION and the entire teardown is
    // one lock hold. The previous shape (clearStub locked only when stub()
    // was non-null — an unlocked check-then-act — then an UNLOCKED
    // `delete m_record`, with delisting deferred to ~CallLinkInfoBase as the
    // LAST act) left the lifetime race open: a dead caller's CallLinkInfo
    // stays linked on a LIVE callee's m_incomingCalls until destruction, so
    // a locked drain (CodeBlock::unlinkOrUpgradeIncomingCalls, a live-mutator
    // tier-up path) could legitimately begin() this still-listed node and be
    // mid-unlinkOrUpgradeImpl — reading mode/m_callee/m_codeBlock, touching
    // m_record via revertCall/publishRecord — while a lazy-sweep mutator
    // freed m_record (the libpas double-free residual class). Holding
    // s_callLinkSerializationLock from before any member teardown gives the
    // required mutual exclusion: this destructor either delists the node
    // before the drain takes the list, or blocks here until the drain loop
    // ends (the drain holds the lock across its entire traversal). The lock
    // is recursive — sweep can run from allocation inside a locked linker.
    // clearStub's PolymorphicCallStubRoutine::unlinkForcefully per-node
    // remove()s still need the lock regardless (they mutate incoming-calls
    // sentinel lists the locked linkers also mutate). The mode gate is the
    // inline Config-page byte, not the 5-Options VM::isGILOffProcess()
    // re-derivation (flag-off pays one predicted not-taken byte test).
    if (g_jscConfig.gilOffProcess) [[unlikely]] {
        Locker locker { s_callLinkSerializationLock };
        if (isOnList())
            remove();
        clearStub();
        // SPEC-jit section 5.8 (I16): owning code unreachable, and with the
        // node now delisted under the lock no drain can republish or retire
        // m_record; inline delete is sound (destruction can run in
        // heap-internal contexts where RetiredJITArtifacts::retire is not
        // allowed, heap ranks 7-9).
        delete m_record.exchange(nullptr);
        return;
    }
    clearStub();
    // SPEC-jit section 5.8: a CallLinkInfo is destroyed only once its owning
    // code is unreachable (post-R2 conservative scan / CodeBlock sweep), so no
    // JIT'd frame can still hold the record pointer (I16); inline delete is
    // sound here - and destruction can run in heap-internal contexts where
    // RetiredJITArtifacts::retire is not allowed (heap ranks 7-9).
    delete m_record.exchange(nullptr);
}

void CallLinkInfo::clearStub()
{
    if (!stub())
        return;

    m_stub->unlinkForcefully();

    // SPEC-jit section 5.8 (Task 7): flag-on, the shared polymorphic-call
    // thunk - reached through a published always-call record - reloads m_stub
    // from this CallLinkInfo with no intervening safepoint. Nulling the field
    // while other mutators run would give such a racing reader a null stub
    // (the thunk dereferences it unconditionally). So flag-on we only do the
    // unlinkForcefully bookkeeping above and KEEP the pointer published: the
    // RefPtr keeps the (GC-aware, section 4.5 atomic-refcount) routine's data
    // alive, mode() is no longer Polymorphic so nothing dispatches new calls
    // through it once the record is gone, and the ref is released when the
    // stub is replaced (setStub's single-store swap) or when this
    // CallLinkInfo dies. Flag-off: clear eagerly, as today.
    if (!Options::useJSThreads()) [[likely]]
        m_stub = nullptr;
}

void CallLinkInfo::unlinkOrUpgradeImpl(VM& vm, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
{
    // AB18-D: this runs on a LIVE mutator via the tier-up install path
    // (ScriptExecutable::installCode -> CodeBlock::unlinkOrUpgradeIncomingCalls
    // per-node drain) — only jettison is STW. The remove(), the upgrade-arm
    // mirror rewrite + publishRecord, and the relink push below must therefore
    // serialize against the Repatch.cpp linkers on the one link lock
    // (precondition 11). Safe to take here: no caller of unlinkOrUpgradeImpl
    // already holds it (the locked linkers never unlink), and GC/STW callers
    // (visitWeak, jettison) take it uncontended because lock holders never
    // park at a safepoint.
    std::optional<Locker<RecursiveLock>> gilOffLocker;
    if (vm.gilOff()) [[unlikely]]
        gilOffLocker.emplace(s_callLinkSerializationLock);

    // We could be called even if we're not linked anymore because of how polymorphic calls
    // work. Each callsite within the polymorphic call stub may separately ask us to unlink().
    if (isOnList())
        remove();

    dataLogLnIf(Options::dumpDisassembly(), "Unlinking CallLinkInfo: ", RawPointer(this));

    Mode mode = this->mode();
    switch (mode) {
    case Mode::Monomorphic: {
        if (newCodeBlock && oldCodeBlock == m_codeBlock) {
            // Upgrading Monomorphic DataIC with newCodeBlock.
            ArityCheckMode arityCheck = oldCodeBlock->jitCode()->addressForCall(ArityCheckMode::ArityCheckNotRequired) == m_monomorphicCallDestination ? ArityCheckMode::ArityCheckNotRequired : ArityCheckMode::MustCheckArity;
            auto target = newCodeBlock->jitCode()->addressForCall(arityCheck);
            m_codeBlock = newCodeBlock;
            m_monomorphicCallDestination = target;
            // SPEC-jit section 5.8 (F6): the in-place mirror rewrite above is
            // invisible to flag-on fast paths; the relink is published as a
            // NEW record (same callee comparand, new target/codeBlock). A
            // racing reader either uses the complete old record (calling the
            // old, still-valid entrypoint) or the complete new one.
            publishRecord(vm, std::bit_cast<uintptr_t>(m_callee.get()), target, newCodeBlock);
            newCodeBlock->linkIncomingCall(nullptr, this); // This is just relinking. So owner and caller frame can be nullptr.
            return;
        }
        revertCall(vm);
        break;
    }
    case Mode::Polymorphic: {
        revertCall(vm);
        break;
    }
    case Mode::Init:
    case Mode::Virtual: {
        break;
    }
    }

    // Either we were unlinked, in which case we should not have been on any list, or we unlinked
    // ourselves so that we're not on any list anymore.
    RELEASE_ASSERT(!isOnList(), static_cast<unsigned>(mode));
}

void CallLinkInfo::setMonomorphicCallee(VM& vm, JSCell* owner, JSObject* callee, CodeBlock* codeBlock, CodePtr<JSEntryPtrTag> codePtr)
{
    RELEASE_ASSERT(!(std::bit_cast<uintptr_t>(callee) & polymorphicCalleeMask));
    if (Options::useJSThreads()) [[unlikely]] {
        // AB18-D, superseded by AB17c F4 third fix / corrected AB17f: the
        // LLInt monomorphic fast path IS now routed through the published
        // m_record flag-on (LowLevelInterpreter64.asm .opCallThreadedRecord
        // at callHelper ~2918 and doCallVarargs ~3041, behind
        // ifJSThreadsBranch) — no flag-on fast path reads the mirror triple
        // (m_callee / m_codeBlock / m_monomorphicCallDestination) lock-free
        // any more. Flag-on, the mirrors are GC/unlink state: read under the
        // CallLinkInfo link lock by unlinkOrUpgradeImpl (whose upgrade arm
        // RELIES on the mirror rewrite being invisible to flag-on fast
        // paths) and by visitWeak. publishRecord below is the sole flag-on
        // dispatch publication; its ordering contract — and the real
        // weak-memory reader-side gap — is the IT-8 KNOWN RESIDUAL recorded
        // in ScriptExecutable.cpp / publishRecord, not a mirror-order
        // residual here. The payload-first + storeStoreFence + comparand-last
        // mirror order is KEPT as belt-and-braces (it is what makes any
        // future lock-free mirror reader fail safe, and it costs nothing on
        // this already-gated arm); flag-off store order is untouched.
        m_codeBlock = codeBlock;
        m_monomorphicCallDestination = codePtr;
        WTF::storeStoreFence();
        m_callee.set(vm, owner, callee);
    } else {
        m_callee.set(vm, owner, callee);
        m_codeBlock = codeBlock;
        m_monomorphicCallDestination = codePtr;
    }
    // SPEC-jit section 5.8: monomorphic link publishes the record the flag-on
    // fast path dispatches on; the fields above remain the GC mirror.
    publishRecord(vm, std::bit_cast<uintptr_t>(callee), codePtr, codeBlock);
    m_mode.store(static_cast<uint8_t>(Mode::Monomorphic));
}

void CallLinkInfo::clearCallee()
{
    if (Options::useJSThreads()) [[unlikely]] {
        // AB18-D (amended per review; same shape as clearStub's flag-on rule).
        // AB17f note: flag-on fast paths no longer read these mirrors (see
        // setMonomorphicCallee — they dispatch on the published record), so
        // the rationale below now defends only a hypothetical future
        // lock-free mirror reader; the conservative comparand-only clear is
        // kept as belt-and-braces.
        // unpublish ONLY the comparand. A lock-free LLInt mirror reader that
        // matched the old callee just before this clear must still load a
        // CONSISTENT (codeBlock, destination) pair for that callee — nulling
        // either field hands it a torn pair: a null entrypoint (pc==0), or a
        // null CodeBlock transferred into the callee frame next to a live
        // entrypoint (callee-prologue crash). Once the comparand is cleared no
        // new reader matches, so the stale pair is reachable only through the
        // stale match, for which it is correct; the next link rewrites both
        // fields payload-first (see setMonomorphicCallee). The stale
        // m_codeBlock is never dereferenced without a comparand match, and
        // mode() is no longer Monomorphic so GC/unlink paths ignore it.
        m_callee.clear();
        return;
    }
    m_callee.clear();
    m_codeBlock = nullptr;
    m_monomorphicCallDestination = nullptr;
}

JSObject* CallLinkInfo::callee()
{
    RELEASE_ASSERT(!(std::bit_cast<uintptr_t>(m_callee.get()) & polymorphicCalleeMask));
    return m_callee.get();
}

void CallLinkInfo::setLastSeenCallee(VM& vm, const JSCell* owner, JSObject* callee)
{
    if (Options::useJSThreads()) [[unlikely]] {
        // TSAN wave 4 (calllink, SPEC-jit 5.8 / object-model blessed cell-slot
        // race): the READER side of m_lastSeenCallee is already relaxed-atomic
        // (WriteBarrierBase::cell()), but WriteBarrierBase::set routes the
        // WRITE through Traits::exchange — a PLAIN store — so concurrent
        // slow-path profiling writers racing forEachDependentCell /
        // lastSeenCallee() readers were one-sided plain-vs-atomic. Route the
        // store through setWithoutWriteBarrier (relaxed-atomic storeCell) and
        // emit the GC barrier ourselves. lastSeenCallee is pure profiling
        // (§5.7 racy-profiling tolerance: a lost or stale last-seen callee
        // only skews CallLinkStatus heuristics), so relaxed is the blessed
        // shape — not a lock. Flag-off takes the historical set() below,
        // unchanged.
        ASSERT(callee);
        validateCell(callee);
        m_lastSeenCallee.setWithoutWriteBarrier(callee);
        vm.writeBarrier(owner, callee);
        return;
    }
    m_lastSeenCallee.set(vm, owner, callee);
}

JSObject* CallLinkInfo::lastSeenCallee() const
{
    return m_lastSeenCallee.get();
}

bool CallLinkInfo::haveLastSeenCallee() const
{
    return !!m_lastSeenCallee;
}

void CallLinkInfo::visitWeak(VM& vm)
{
    auto handleSpecificCallee = [&] (JSFunction* callee) {
        if (vm.heap.isMarked(callee->executable()))
            setHasSeenClosure();
        else
            setClearedByGC();
    };

    switch (mode()) {
    case Mode::Init:
    case Mode::Virtual:
        break;
    case Mode::Polymorphic: {
        // V7: load the stub ONCE — stub() is now an always-reloading atomic
        // load flag-on, so repeated calls would be a TOCTOU if visitWeak ever
        // ran concurrent with a linker. Today visitWeak runs STW, which is
        // what makes the single load (and everything below) safe.
        if (auto* stub = this->stub()) {
            if (!stub->visitWeak(vm)) {
                dataLogLnIf(Options::verboseOSR(), "At ", codeOrigin(), ", ", RawPointer(this), ": clearing call stub to ", listDump(stub->variants()), ", stub routine ", RawPointer(stub), ".");
                unlinkOrUpgrade(vm, nullptr, nullptr);
                setClearedByGC();
            }
        }
        break;
    }
    case Mode::Monomorphic: {
        auto* callee = m_callee.get();
        if (callee && !vm.heap.isMarked(callee)) {
            if (callee->type() == JSFunctionType) {
                dataLogLnIf(Options::verboseOSR(), "Clearing call to ", RawPointer(callee), " (", static_cast<JSFunction*>(callee)->executable()->hashFor(specializationKind()), ").");
                handleSpecificCallee(static_cast<JSFunction*>(callee));
            } else {
                dataLogLnIf(Options::verboseOSR(), "Clearing call to ", RawPointer(callee), ".");
                setClearedByGC();
            }
            unlinkOrUpgrade(vm, nullptr, nullptr);
        }
        break;
    }
    }

    if (haveLastSeenCallee() && !vm.heap.isMarked(lastSeenCallee())) {
        if (lastSeenCallee()->type() == JSFunctionType)
            handleSpecificCallee(uncheckedDowncast<JSFunction>(lastSeenCallee()));
        else
            setClearedByGC();
        m_lastSeenCallee.clear();
    }
}

void CallLinkInfo::revertCallToStub()
{
    RELEASE_ASSERT(stub());
    // The start of our JIT code is now a jump to the polymorphic stub. Rewrite the first instruction
    // to be what we need for non stub ICs.

    // this runs into some branch compaction crap I'd like to avoid for now. Essentially, the branch
    // doesn't know if it can be compacted or not. So we end up with 28 bytes of machine code, for
    // what in all likelihood fits in 24. So we just splat out the first instruction. Long term, we
    // need something cleaner. But this works on arm64 for now.

    if (Options::useJSThreads()) [[unlikely]] {
        // AB18-D: same rule as clearCallee. In Polymorphic mode the published
        // comparand is the always-call mask; clearing m_callee (slot -> 0)
        // unpublishes it, after which no reader takes .goPolymorphic here. A
        // reader that already matched the mask must still find the stub
        // entrypoint in m_monomorphicCallDestination — never null it while
        // mutators run. The stub routine stays alive (clearStub's flag-on
        // keep-published rule), so the stale entrypoint remains valid.
        m_callee.clear();
        return;
    }
    m_callee.clear();
    m_codeBlock = nullptr;
    m_monomorphicCallDestination = nullptr;
}

// TSAN wave 3 (calllink) triage note: this initializer's sole call site is
// CodeBlock::finishCreation (metadata setup), which completes before the
// CodeBlock is installed/published to any other thread — the plain
// m_owner/m_type/m_callType/m_codeOrigin stores here cannot race. TSAN keys
// naming this frame are expected to carry it in the report's heap-block
// ALLOCATION stack, not an access stack (the same misattribution class as
// the SmallStrings::initializeCommonStrings finding, TSAN-TRIAGE 3.22). If
// an r3+ report ever shows this frame in an ACCESS stack, that is a new
// finding: re-open it, do not bin it against this note.
// [Wave 5 update: r4 did show access-stack pairs against compiler-thread
// specializationKind()/type() readers across metadata-buffer reuse; the
// finding was re-opened and the callType/type byte converted to the
// relaxed-atomic write-once pair below.]
void DataOnlyCallLinkInfo::initialize(VM& vm, CodeBlock* owner, CallType callType, CodeOrigin codeOrigin)
{
    // TSAN wave 4 (calllink): m_owner goes through the relaxed-atomic
    // write-once pair (see CallLinkInfo::owner()) so allocator-reuse report
    // keys cannot pair a plain init store with a stale reader's atomic load.
    WTF::atomicStore(&m_owner, static_cast<JSCell*>(owner), std::memory_order_relaxed);
    // TSAN wave 5 (calllink): the r4 reports DID show this initializer in
    // ACCESS stacks paired with compiler-thread specializationKind()/type()
    // readers ("specializationKind x UnlinkedMetadataTable::link / DataOnly
    // initialize") — the metadata buffer this DataOnlyCallLinkInfo lives in
    // is recycled, so a stale reader's load pairs with the new buffer's init
    // write. Per the wave-3 note below, that re-opened the finding: the
    // packed callType+type byte now goes through the relaxed-atomic
    // write-once pair (single store; see CallLinkInfo.h), the same shape as
    // the m_owner fix above. Write-once still holds: this runs in
    // CodeBlock::finishCreation before the CodeBlock is published.
    storeCallTypeAndType(callType, Type::DataOnly);
    ASSERT(Type::DataOnly == type());
    m_codeOrigin = codeOrigin;
    m_mode.store(static_cast<uint8_t>(Mode::Init));
    if (!Options::useLLIntICs()) [[unlikely]]
        setVirtualCall(vm);
}

std::tuple<CodeBlock*, BytecodeIndex> CallLinkInfo::retrieveCaller(JSCell* owner)
{
    auto* codeBlock = dynamicDowncast<CodeBlock>(owner);
    if (!codeBlock)
        return { };
    CodeOrigin codeOrigin = this->codeOrigin();
    if (auto* baselineCodeBlock = codeOrigin.codeOriginOwner())
        return std::tuple { baselineCodeBlock, codeOrigin.bytecodeIndex() };
    return std::tuple { codeBlock, codeOrigin.bytecodeIndex() };
}

void CallLinkInfo::reset(VM& vm)
{
    // SPEC-jit section 5.8: unlink the record FIRST (single nullptr store) so
    // flag-on fast paths fall back to the default call before the legacy
    // mirrors below are torn down.
    clearRecord(vm);
    if (stub())
        revertCallToStub();
    clearCallee(); // This also clears the inline cache both for data and code-based caches.
    clearSeen();
    clearStub();
    if (isOnList())
        remove();
    m_mode.store(static_cast<uint8_t>(Mode::Init));
}

void CallLinkInfo::revertCall(VM& vm)
{
    if (!Options::useLLIntICs() && type() == CallLinkInfo::Type::DataOnly) [[unlikely]]
        setVirtualCall(vm);
    else
        reset(vm);
}

void CallLinkInfo::setVirtualCall(VM& vm)
{
    // AB17c F4 (precondition 11): not every caller is a locked linker —
    // RepatchInlines.h linkFor's mode switch calls this directly with no
    // lock held, and reset() below remove()s this node from an
    // incoming-calls sentinel list the locked linkers also mutate.
    // Self-lock (recursive — the locked linkers and unlinkOrUpgradeImpl
    // also reach here).
    std::optional<Locker<RecursiveLock>> gilOffLocker;
    if (vm.gilOff()) [[unlikely]]
        gilOffLocker.emplace(s_callLinkSerializationLock);
    reset(vm);
    if (Options::useJSThreads()) [[unlikely]] {
        // AB18-D publication order (cf. setMonomorphicCallee): the always-call
        // mask comparand makes ANY callee match in the LLInt mirror reader, so
        // the payload must be in place before the mask is stored — otherwise a
        // racing reader pairs the mask with the previous (or cleared)
        // destination/codeBlock and dispatches the wrong target.
        m_codeBlock = nullptr; // PolymorphicCallStubRoutine will set CodeBlock inside it.
        m_monomorphicCallDestination = vm.getCTIVirtualCall(callMode()).code().template retagged<JSEntryPtrTag>();
        WTF::storeStoreFence();
        m_callee.clear();
        // TSAN wave 3 (calllink): the sentinel store must go through the same
        // relaxed-atomic slot discipline as every other m_callee access — a
        // plain store here races the WriteBarrier relaxed-atomic readers
        // (forEachDependentCell on a concurrent marking thread, the
        // CallLinkStatus reads). Relaxed is sufficient: the payload it gates
        // is ordered by the storeStoreFence above (mirror order) and by
        // publishRecord's F6 protocol (record order); flag-off takes the
        // plain-store branch below, untouched.
        WTF::atomicStore(std::bit_cast<uintptr_t*>(m_callee.slot()), polymorphicCalleeMask, std::memory_order_relaxed);
    } else {
        m_callee.clear();
        *std::bit_cast<uintptr_t*>(m_callee.slot()) = polymorphicCalleeMask;
        m_codeBlock = nullptr; // PolymorphicCallStubRoutine will set CodeBlock inside it.
        m_monomorphicCallDestination = vm.getCTIVirtualCall(callMode()).code().template retagged<JSEntryPtrTag>();
    }

    // SPEC-jit section 5.8: sentinel comparand (bit 0 set) = always-call; the
    // virtual-call thunk dispatches on the callee itself. codeBlockToTransfer
    // is null, matching today's m_codeBlock for virtual calls.
    publishRecord(vm, polymorphicCalleeMask, m_monomorphicCallDestination, nullptr);

    setClearedByVirtual();
    m_mode.store(static_cast<uint8_t>(Mode::Virtual));
}

JSGlobalObject* CallLinkInfo::globalObjectForSlowPath(JSCell* owner)
{
    auto [codeBlock, bytecodeIndex] = retrieveCaller(owner);
    if (codeBlock)
        return codeBlock->globalObject();
#if ENABLE(WEBASSEMBLY)
    auto* module = dynamicDowncast<JSWebAssemblyModule>(owner);
    if (module)
        return module->realm();
#endif
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

void CallLinkInfo::setStub(VM& vm, Ref<PolymorphicCallStubRoutine>&& newStub)
{
    clearStub();
    if (Options::useJSThreads()) [[unlikely]] {
        // SPEC-jit section 5.8 (Task 7): the shared polymorphic-call thunk
        // reloads m_stub through this CallLinkInfo, so replacement must be ONE
        // raw pointer store with no null window - a racing reader observes the
        // old or the new routine, both alive. clearStub() above kept the old
        // ref (flag-on); we adopt and release it here. The displaced routine
        // is GC-aware with an atomic refcount (section 4.5): even at refcount
        // zero its memory is reclaimed only after a GC conservative scan of
        // all mutator stacks (R2/I7), so a reader mid-dispatch is safe.
        PolymorphicCallStubRoutine** stubSlot = std::bit_cast<PolymorphicCallStubRoutine**>(&m_stub);
        PolymorphicCallStubRoutine* oldStub = *stubSlot; // Plain read is writer-writer safe: writers serialize on s_callLinkSerializationLock.
        PolymorphicCallStubRoutine* incoming = &newStub.leakRef();
        // V7: release store orders the new routine's payload initialization
        // before the publishing store (subsumes the old storeStoreFence, F1
        // pattern) and is a typed atomic so racing thunk/stub() readers are
        // no longer plain-vs-store pairs.
        WTF::atomicStore(stubSlot, incoming, std::memory_order_release);
        if (oldStub)
            oldStub->deref();
    } else
        m_stub = WTF::move(newStub);

    if (Options::useJSThreads()) [[unlikely]] {
        // AB18-D publication order (cf. setVirtualCall): payload before the
        // always-call mask comparand, for the lock-free LLInt mirror reader.
        m_codeBlock = nullptr; // PolymorphicCallStubRoutine will set CodeBlock inside it.
        m_monomorphicCallDestination = m_stub->code().code().retagged<JSEntryPtrTag>();
        WTF::storeStoreFence();
        m_callee.clear();
        // TSAN wave 3 (calllink): atomic sentinel store — same rule and
        // justification as setVirtualCall above.
        WTF::atomicStore(std::bit_cast<uintptr_t*>(m_callee.slot()), polymorphicCalleeMask, std::memory_order_relaxed);
    } else {
        m_callee.clear();
        *std::bit_cast<uintptr_t*>(m_callee.slot()) = polymorphicCalleeMask;
        m_codeBlock = nullptr; // PolymorphicCallStubRoutine will set CodeBlock inside it.
        m_monomorphicCallDestination = m_stub->code().code().retagged<JSEntryPtrTag>();
    }

    // SPEC-jit section 5.8: publish an always-call record targeting the
    // polymorphic stub. The routine is GC-aware and ref'd by this owner
    // (section 4.5 atomic refcounts), so the record's target entrypoint
    // outlives any stale reader of a replaced record.
    publishRecord(vm, polymorphicCalleeMask, m_monomorphicCallDestination, nullptr);

    // The call link info no longer has a call cache apart from the jump to the polymorphic call stub.
    if (isOnList())
        remove();

    m_mode.store(static_cast<uint8_t>(Mode::Polymorphic));
}

#if ENABLE(JIT)

#if USE(JSVALUE64)
// SPEC-jit section 5.8: immutable, never-retired "unlinked" record. The
// flag-on fast path branches here when m_record is null (or the comparand
// mismatches), so the miss path runs the SAME frozen tail as the hit path:
// transfer codeBlockToTransfer (null) to the callee frame, load target ONCE
// (the default-call thunk, which performs the slow-path linking), call it.
static const CallLinkRecord* emptyCallLinkRecord()
{
    static LazyNeverDestroyed<CallLinkRecord> record;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        record.construct(CallLinkRecord { 0, LLInt::defaultCall().code(), nullptr });
    });
    return &record.get();
}
#endif

void CallLinkInfo::emitFastPathImpl(CallLinkInfo* callLinkInfo, CCallHelpers& jit, bool isTailCall, ScopedLambda<void()>&& prepareForTailCall)
{
    if (callLinkInfo)
        jit.move(CCallHelpers::TrustedImmPtr(callLinkInfo), BaselineJITRegisters::Call::callLinkInfoGPR);

#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // SPEC-jit section 5.8 frozen fast path (all tiers/flavors flag-on):
        //   load r = m_record; if (!r) r = empty record (default call);
        //   load c = r->comparand;
        //   if (c == calleeGPR || (c & polymorphicCalleeMask)) hit; else r = empty record;
        //   store r->codeBlockToTransfer -> callee frame; load t = r->target ONCE; call t.
        // No legacy field (m_callee/m_codeBlock/m_monomorphicCallDestination)
        // is read. Every load flows through r (F2 address dependency); t is
        // loaded exactly once and called via register. callLinkInfoGPR is
        // preserved for the default-call thunk; r lives in callTargetGPR.
        CCallHelpers::JumpList found;
        jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfRecord()), BaselineJITRegisters::Call::callTargetGPR);
        auto haveNoRecord = jit.branchTestPtr(CCallHelpers::Zero, BaselineJITRegisters::Call::callTargetGPR);
        if constexpr (isRISCV64()) {
            // See the RISCV64 scratch-register note below: compare against the
            // comparand in memory so MacroAssembler keeps its internal scratch.
            CCallHelpers::Address comparandAddress(BaselineJITRegisters::Call::callTargetGPR, CallLinkRecord::offsetOfComparand());
            found.append(jit.branchPtr(CCallHelpers::Equal, comparandAddress, BaselineJITRegisters::Call::calleeGPR));
            found.append(jit.branchTestPtr(CCallHelpers::NonZero, comparandAddress, CCallHelpers::TrustedImm32(polymorphicCalleeMask)));
        } else {
            GPRReg scratchGPR = jit.scratchRegister();
            DisallowMacroScratchRegisterUsage disallowScratch(jit);
            jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callTargetGPR, CallLinkRecord::offsetOfComparand()), scratchGPR);
            found.append(jit.branchPtr(CCallHelpers::Equal, scratchGPR, BaselineJITRegisters::Call::calleeGPR));
            found.append(jit.branchTestPtr(CCallHelpers::NonZero, scratchGPR, CCallHelpers::TrustedImm32(polymorphicCalleeMask)));
        }
        haveNoRecord.link(&jit);
        jit.move(CCallHelpers::TrustedImmPtr(emptyCallLinkRecord()), BaselineJITRegisters::Call::callTargetGPR);

        found.link(&jit);
        // callTargetGPR holds r here (real record or the empty record). It
        // survives prepareForTailCall, exactly as today's preloaded target.
        if (isTailCall) {
            prepareForTailCall();
            jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callTargetGPR, CallLinkRecord::offsetOfCodeBlockToTransfer()), CCallHelpers::calleeFrameCodeBlockBeforeTailCall());
            jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callTargetGPR, CallLinkRecord::offsetOfTarget()), BaselineJITRegisters::Call::callTargetGPR);
            jit.farJump(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
        } else {
            jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callTargetGPR, CallLinkRecord::offsetOfCodeBlockToTransfer()), CCallHelpers::calleeFrameCodeBlockBeforeCall());
            jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callTargetGPR, CallLinkRecord::offsetOfTarget()), BaselineJITRegisters::Call::callTargetGPR);
            jit.call(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
        }
        return;
    }
#endif
#if USE(JSVALUE32_64)
    // We need this on JSVALUE32_64 only as on JSVALUE64 a pointer comparison in the DataIC fast
    // path catches this.
    auto failed = jit.branchIfNotCell(BaselineJITRegisters::Call::calleeJSR);
#endif

    // For RISCV64, scratch register usage here collides with MacroAssembler's internal usage
    // that's necessary for the test-and-branch operation but is avoidable by loading from the callee
    // address for each branch operation. Other MacroAssembler implementations handle this better by
    // using a wider range of scratch registers or more potent branching instructions.
    CCallHelpers::JumpList found;
    jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfMonomorphicCallDestination()), BaselineJITRegisters::Call::callTargetGPR);
    if constexpr (isRISCV64()) {
        CCallHelpers::Address calleeAddress(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfCallee());
        found.append(jit.branchPtr(CCallHelpers::Equal, calleeAddress, BaselineJITRegisters::Call::calleeGPR));
        found.append(jit.branchTestPtr(CCallHelpers::NonZero, calleeAddress, CCallHelpers::TrustedImm32(polymorphicCalleeMask)));
    } else {
        GPRReg scratchGPR = jit.scratchRegister();
        DisallowMacroScratchRegisterUsage disallowScratch(jit);
        jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfCallee()), scratchGPR);
        found.append(jit.branchPtr(CCallHelpers::Equal, scratchGPR, BaselineJITRegisters::Call::calleeGPR));
        found.append(jit.branchTestPtr(CCallHelpers::NonZero, scratchGPR, CCallHelpers::TrustedImm32(polymorphicCalleeMask)));
    }

#if USE(JSVALUE32_64)
    failed.link(&jit);
#endif
    jit.move(CCallHelpers::TrustedImmPtr(LLInt::defaultCall().code().taggedPtr()), BaselineJITRegisters::Call::callTargetGPR);

    found.link(&jit);
    if (isTailCall) {
        prepareForTailCall();
        jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfCodeBlock()), CCallHelpers::calleeFrameCodeBlockBeforeTailCall());
        jit.farJump(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
    } else {
        jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfCodeBlock()), CCallHelpers::calleeFrameCodeBlockBeforeCall());
        jit.call(BaselineJITRegisters::Call::callTargetGPR, JSEntryPtrTag);
    }
    return;
}

void CallLinkInfo::emitDataICFastPath(CCallHelpers& jit)
{
    emitFastPathImpl(nullptr, jit, false, nullptr);
}

void CallLinkInfo::emitTailCallDataICFastPath(CCallHelpers& jit, ScopedLambda<void()>&& prepareForTailCall)
{
    emitFastPathImpl(nullptr, jit, true, WTF::move(prepareForTailCall));
}

void CallLinkInfo::emitFastPath(CCallHelpers& jit, CompileTimeCallLinkInfo callLinkInfo)
{
    if (std::holds_alternative<OptimizingCallLinkInfo*>(callLinkInfo))
        return std::get<OptimizingCallLinkInfo*>(callLinkInfo)->emitFastPath(jit);

    return CallLinkInfo::emitDataICFastPath(jit);
}

void CallLinkInfo::emitTailCallFastPath(CCallHelpers& jit, CompileTimeCallLinkInfo callLinkInfo, ScopedLambda<void()>&& prepareForTailCall)
{
    if (std::holds_alternative<OptimizingCallLinkInfo*>(callLinkInfo))
        return std::get<OptimizingCallLinkInfo*>(callLinkInfo)->emitTailCallFastPath(jit, WTF::move(prepareForTailCall));

    return CallLinkInfo::emitTailCallDataICFastPath(jit, WTF::move(prepareForTailCall));
}

void OptimizingCallLinkInfo::emitFastPath(CCallHelpers& jit)
{
    RELEASE_ASSERT(!isTailCall());
    emitFastPathImpl(this, jit, isTailCall(), nullptr);
}

void OptimizingCallLinkInfo::emitTailCallFastPath(CCallHelpers& jit, ScopedLambda<void()>&& prepareForTailCall)
{
    RELEASE_ASSERT(isTailCall());
    emitFastPathImpl(this, jit, isTailCall(), WTF::move(prepareForTailCall));
}

#if ENABLE(DFG_JIT)
void OptimizingCallLinkInfo::initializeFromDFGUnlinkedCallLinkInfo(VM&, const DFG::UnlinkedCallLinkInfo& unlinkedCallLinkInfo, CodeBlock* owner)
{
    // TSAN wave 4 (calllink): relaxed-atomic write-once m_owner store — same
    // rule as DataOnlyCallLinkInfo::initialize / the CallLinkInfo ctor.
    WTF::atomicStore(&m_owner, static_cast<JSCell*>(owner), std::memory_order_relaxed);
    m_codeOrigin = unlinkedCallLinkInfo.codeOrigin;
    // TSAN wave 5 (calllink): routed through the relaxed-atomic packed byte
    // (write-once before this info is reachable by other lites).
    setCallType(unlinkedCallLinkInfo.callType);
}
#endif

// SPEC-jit section 5.8 (direct flavor): same F6 publish protocol as
// CallLinkInfo::publishRecord; direct fast paths skip the comparand check, so
// the comparand is left 0. The legacy m_target/m_codeBlock mirrors remain the
// GC-visible state. Same R3-3 writer-writer caveat as publishRecord above:
// GIL-removal precondition 11 (docs/threads/INTEGRATE-jit.md).
void DirectCallLinkInfo::publishRecord(VM& vm, CodePtr<JSEntryPtrTag> target, CodeBlock* codeBlockToTransfer)
{
    if (!Options::useJSThreads()) [[likely]]
        return;
    ASSERT(isDataIC()); // I3: flag-on, all DirectCallLinkInfos are data ICs.
    auto* record = new CallLinkRecord { 0, target, codeBlockToTransfer };
    WTF::storeStoreFence();
    // AB18-D: atomic swap — same no-double-retire rule as
    // CallLinkInfo::publishRecord (precondition 11).
    auto* oldRecord = m_record.exchange(record);
    retireRecord(vm, oldRecord);
}

void DirectCallLinkInfo::clearRecord(VM& vm)
{
    if (!Options::useJSThreads()) [[likely]] {
        ASSERT(!m_record.loadRelaxed());
        return;
    }
    // AB18-D: atomic for the same no-double-retire reason as publishRecord.
    retireRecord(vm, m_record.exchange(nullptr));
}

void DirectCallLinkInfo::retireRecord(VM& vm, CallLinkRecord* record)
{
    if (!record)
        return;
    // AB18-E (UAF fix): the VM comes from the caller — the retiring mutator's
    // VM (section 4.4; R4-2 — RetiredJITArtifacts resolves the epoch heap, the
    // client's SERVER under useSharedGCHeap, from the VM internally). It must
    // NOT be derived as m_owner->vm(): the retireOptimizedJITCode leak keeps
    // this node alive (and validly on callees' m_incomingCalls lists) past its
    // owner CodeBlock's death, so a drain (unlinkOrUpgradeIncomingCalls under
    // a jettison stop) can reach here with m_owner a swept cell whose
    // MarkedBlock is already freed — m_owner->vm() was a use-after-free
    // (JSTests/threads/jit/ic-publish-reset-loops.js).
    retireCallLinkRecord(vm, record);
}

void DirectCallLinkInfo::reset(VM& vm)
{
    // SPEC-jit section 5.8: unlink the record first (single nullptr store) so
    // the flag-on fast path takes its slow path before the mirrors are
    // cleared.
    clearRecord(vm);
    if (isOnList())
        remove();
#if ENABLE(JIT)
    if (!isDataIC())
        initialize();
#endif
    m_target = { };
    m_codeBlock = nullptr;
}

void DirectCallLinkInfo::unlinkOrUpgradeImpl(VM& vm, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
{
    // AB18-D: same serialization as CallLinkInfo::unlinkOrUpgradeImpl — this
    // runs on a live mutator via the tier-up install drain, and the remove()/
    // relink push below must not race the Repatch.cpp linkers.
    std::optional<Locker<RecursiveLock>> gilOffLocker;
    if (vm.gilOff()) [[unlikely]]
        gilOffLocker.emplace(CallLinkInfo::s_callLinkSerializationLock);

    if (isOnList())
        remove();

    if (!!m_target) {
        if (m_codeBlock && newCodeBlock && oldCodeBlock == m_codeBlock) {
            ArityCheckMode arityCheck = oldCodeBlock->jitCode()->addressForCall(ArityCheckMode::ArityCheckNotRequired) == m_target ? ArityCheckMode::ArityCheckNotRequired : ArityCheckMode::MustCheckArity;
            auto target = newCodeBlock->jitCode()->addressForCall(arityCheck);
            setCallTarget(vm, newCodeBlock, CodeLocationLabel { target });
            newCodeBlock->linkIncomingCall(nullptr, this); // This is just relinking. So owner and caller frame can be nullptr.
            return;
        }
        dataLogLnIf(Options::dumpDisassembly(), "Unlinking CallLinkInfo: ", RawPointer(this));
        reset(vm);
    }

    // Either we were unlinked, in which case we should not have been on any list, or we unlinked
    // ourselves so that we're not on any list anymore.
    RELEASE_ASSERT(!isOnList());
}

void DirectCallLinkInfo::visitWeak(VM& vm)
{
    if (m_codeBlock && !vm.heap.isMarked(m_codeBlock)) {
        dataLogLnIf(Options::verboseOSR(), "Clearing call to ", RawPointer(m_codeBlock), " (", pointerDump(m_codeBlock), ").");
        unlinkOrUpgrade(vm, nullptr, nullptr);
    }
}

CCallHelpers::JumpList DirectCallLinkInfo::emitDirectFastPath(CCallHelpers& jit)
{
    RELEASE_ASSERT(!isTailCall());

    if (isDataIC()) {
        CCallHelpers::JumpList slowPath;
        jit.move(CCallHelpers::TrustedImmPtr(this), BaselineJITRegisters::Call::callLinkInfoGPR);
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.8 frozen fast path, direct flavor: load
            // r = m_record; if (!r) goto slow; no comparand check; store
            // r->codeBlockToTransfer -> callee frame; load t = r->target ONCE;
            // call t. No legacy field (m_target/m_codeBlock) is read; both
            // loads flow through r (F2). r replaces the CallLinkInfo pointer
            // in callLinkInfoGPR once the slow-path branch is resolved.
            {
                GPRReg scratchGPR = jit.scratchRegister();
                DisallowMacroScratchRegisterUsage disallowScratch(jit);
                jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfRecord()), scratchGPR);
                slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, scratchGPR));
                jit.move(scratchGPR, BaselineJITRegisters::Call::callLinkInfoGPR);
            }
            jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, CallLinkRecord::offsetOfCodeBlockToTransfer()), CCallHelpers::calleeFrameCodeBlockBeforeCall());
            jit.call(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, CallLinkRecord::offsetOfTarget()), JSEntryPtrTag);
            return slowPath;
        }
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfTarget())));
        jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfCodeBlock()), CCallHelpers::calleeFrameCodeBlockBeforeCall());
        jit.call(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfTarget()), JSEntryPtrTag);
        return slowPath;
    }

    auto codeBlockStore = jit.storePtrWithPatch(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::calleeFrameCodeBlockBeforeCall());
    auto call = jit.nearCall();
    jit.addLinkTask([=, this] (LinkBuffer& linkBuffer) {
        m_callLocation = linkBuffer.locationOfNearCall<JSInternalPtrTag>(call);
        m_codeBlockLocation = linkBuffer.locationOf<JSInternalPtrTag>(codeBlockStore);
    });
    jit.addLateLinkTask([this](LinkBuffer&) {
        repatchSpeculatively();
    });
    return { };
}

CCallHelpers::JumpList DirectCallLinkInfo::emitDirectTailCallFastPath(CCallHelpers& jit, ScopedLambda<void()>&& prepareForTailCall)
{
    RELEASE_ASSERT(isTailCall());

    if (isDataIC()) {
        CCallHelpers::JumpList slowPath;
        jit.move(CCallHelpers::TrustedImmPtr(this), BaselineJITRegisters::Call::callLinkInfoGPR);
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.8 frozen fast path, direct tail flavor (see
            // emitDirectFastPath). r is moved into callLinkInfoGPR, which
            // survives prepareForTailCall (as today's CallLinkInfo pointer
            // does); both post-shuffle accesses flow through r (F2).
            {
                GPRReg scratchGPR = jit.scratchRegister();
                DisallowMacroScratchRegisterUsage disallowScratch(jit);
                jit.loadPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfRecord()), scratchGPR);
                slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, scratchGPR));
                jit.move(scratchGPR, BaselineJITRegisters::Call::callLinkInfoGPR);
            }
            prepareForTailCall();
            jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, CallLinkRecord::offsetOfCodeBlockToTransfer()), CCallHelpers::calleeFrameCodeBlockBeforeTailCall());
            jit.farJump(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, CallLinkRecord::offsetOfTarget()), JSEntryPtrTag);
            return slowPath;
        }
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfTarget())));
        prepareForTailCall();
        jit.transferPtr(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfCodeBlock()), CCallHelpers::calleeFrameCodeBlockBeforeTailCall());
        jit.farJump(CCallHelpers::Address(BaselineJITRegisters::Call::callLinkInfoGPR, offsetOfTarget()), JSEntryPtrTag);
        return slowPath;
    }

    auto fastPathStart = jit.label();

    // - If we're not yet linked, this is a jump to the slow path.
    // - Once we're linked to a fast path, this goes back to being nops so we fall through to the linked jump.
    jit.emitNops(CCallHelpers::patchableJumpSize());

    prepareForTailCall();
    auto codeBlockStore = jit.storePtrWithPatch(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::calleeFrameCodeBlockBeforeTailCall());
    auto call = jit.nearTailCall();
    jit.addLinkTask([=, this] (LinkBuffer& linkBuffer) {
        m_fastPathStart = linkBuffer.locationOf<JSInternalPtrTag>(fastPathStart);
        m_callLocation = linkBuffer.locationOfNearCall<JSInternalPtrTag>(call);
        m_codeBlockLocation = linkBuffer.locationOf<JSInternalPtrTag>(codeBlockStore);
    });
    jit.addLateLinkTask([this](LinkBuffer&) {
        repatchSpeculatively();
    });
    return { };
}

void DirectCallLinkInfo::initialize()
{
    ASSERT(m_callLocation);
    ASSERT(m_codeBlockLocation);
    // SPEC-jit I2/I3 (section 5.8 DirectCall): code-patching direct-call fast
    // paths only exist for UseDataIC::No, which is forbidden flag-on.
    RELEASE_ASSERT(!Options::useJSThreads());
    if (isTailCall()) {
        RELEASE_ASSERT(fastPathStart());
        CCallHelpers::replaceWithJump(fastPathStart(), slowPathStart());
    } else
        MacroAssembler::repatchNearCall(m_callLocation, slowPathStart());
}

void DirectCallLinkInfo::setCallTarget(VM& vm, CodeBlock* codeBlock, CodeLocationLabel<JSEntryPtrTag> target)
{
    m_codeBlock = codeBlock;
    m_target = target;

    // SPEC-jit section 5.8: publish the new direct record (F6). Relinking to a
    // different CodeBlock (unlinkOrUpgradeImpl) replaces the record; a stale
    // reader completes through the old record's still-valid entrypoint.
    publishRecord(vm, target, codeBlock);

    if (!isDataIC()) {
        // SPEC-jit I2/I3 (section 5.8 DirectCall): this branch patches machine
        // code; unreachable flag-on (UseDataIC::No construction is forbidden).
        RELEASE_ASSERT(!Options::useJSThreads());
        if (isTailCall()) {
            RELEASE_ASSERT(fastPathStart());
            // We reserved this many bytes for the jump at fastPathStart(). Make that
            // code nops now so we fall through to the jump to the fast path.
            CCallHelpers::replaceWithNops(fastPathStart(), CCallHelpers::patchableJumpSize());
        }

        MacroAssembler::repatchNearCall(m_callLocation, target);
        MacroAssembler::repatchPointer(m_codeBlockLocation, codeBlock);
    }
}

void DirectCallLinkInfo::setMaxArgumentCountIncludingThis(unsigned value)
{
    RELEASE_ASSERT(value);
    m_maxArgumentCountIncludingThis = value;
}

CodeBlock* DirectCallLinkInfo::retrieveCodeBlock(FunctionExecutable* functionExecutable)
{
    CodeSpecializationKind kind = specializationKind();
    CodeBlock* codeBlock = functionExecutable->codeBlockFor(kind);
    if (!codeBlock)
        return nullptr;

    CodeBlock* ownerCodeBlock = dynamicDowncast<CodeBlock>(owner());
    if (!ownerCodeBlock)
        return nullptr;

    if (ownerCodeBlock->alternative() == codeBlock)
        return nullptr;

    return codeBlock;
}

CodePtr<JSEntryPtrTag> DirectCallLinkInfo::retrieveCodePtr(const ConcurrentJSLocker& locker, CodeBlock* codeBlock)
{
    unsigned argumentStackSlots = maxArgumentCountIncludingThis();
    ArityCheckMode arityCheckMode = (argumentStackSlots < static_cast<size_t>(codeBlock->numParameters())) ? ArityCheckMode::MustCheckArity : ArityCheckMode::ArityCheckNotRequired;
    return codeBlock->addressForCallConcurrently(locker, arityCheckMode);
}

void DirectCallLinkInfo::repatchSpeculatively()
{
    // SPEC-jit section 5.8: speculative repatching (possibly from a compiler
    // thread, G10) is forbidden with shared-memory threads enabled; flag-on,
    // direct calls are data ICs and never reach this late-link task.
    RELEASE_ASSERT(!Options::useJSThreads());
    // Flag-off only: publishRecord is a no-op, so this VM is unused by the
    // record path; m_executable is held alive by the compilation plan.
    VM& vm = m_executable->vm();

    if (m_executable->isHostFunction()) {
        CodeSpecializationKind kind = specializationKind();
        CodePtr<JSEntryPtrTag> codePtr;
        if (kind == CodeSpecializationKind::CodeForCall)
            codePtr = m_executable->generatedJITCodeWithArityCheckForCall();
        else
            codePtr = m_executable->generatedJITCodeWithArityCheckForConstruct();
        if (codePtr)
            setCallTarget(vm, nullptr, CodeLocationLabel { codePtr });
        else
            initialize();
        return;
    }

    FunctionExecutable* functionExecutable = dynamicDowncast<FunctionExecutable>(m_executable);
    if (!functionExecutable) {
        initialize();
        return;
    }

    auto* codeBlock = retrieveCodeBlock(functionExecutable);
    if (codeBlock) {
        auto codePtr = retrieveCodePtr(ConcurrentJSLocker { codeBlock->m_lock }, codeBlock);
        if (codePtr) {
            m_codeBlock = codeBlock;
            m_target = codePtr;
            // Do not chain |this| to the calle codeBlock concurrently. It will be done in the main thread if the speculatively repatched one is still valid.
            setCallTarget(vm, codeBlock, CodeLocationLabel { codePtr });
            return;
        }
    }

    initialize();
}

void DirectCallLinkInfo::validateSpeculativeRepatchOnMainThread(VM& vm)
{
    constexpr bool verbose = false;
    FunctionExecutable* functionExecutable = dynamicDowncast<FunctionExecutable>(m_executable);
    if (!functionExecutable)
        return;

    auto* codeBlock = retrieveCodeBlock(functionExecutable);
    CodePtr<JSEntryPtrTag> codePtr = nullptr;
    if (codeBlock)
        codePtr = retrieveCodePtr(ConcurrentJSLocker { NoLockingNecessary }, codeBlock);

    if (m_codeBlock != codeBlock || m_target != codePtr) {
        if (codeBlock && codePtr)
            setCallTarget(vm, codeBlock, CodeLocationLabel { codePtr });
        else
            reset(vm);
    } else
        dataLogLnIf(verbose, "Speculative repatching succeeded ", RawPointer(m_codeBlock), " ", m_target);

    if (m_codeBlock)
        m_codeBlock->linkIncomingCall(owner(), this);
}

#endif

} // namespace JSC
