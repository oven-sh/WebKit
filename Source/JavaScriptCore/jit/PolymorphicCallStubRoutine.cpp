/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
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
#include "PolymorphicCallStubRoutine.h"

#include "CachedCall.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "FullCodeOrigin.h"
#include "JSCJSValueInlines.h"
#include "JSFunctionInlines.h"
#include "LinkBuffer.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

void PolymorphicCallNode::unlinkOrUpgradeImpl(VM& vm, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
{
    // We first remove itself from the linked-list before unlinking callLinkInfo.
    // The reason is that callLinkInfo can potentially link PolymorphicCallNode's stub itself, and it may destroy |this| (the other CallLinkInfo
    // does not do it since it is not chained in PolymorphicCallStubRoutine).
    if (vm.gilOff()) [[unlikely]] {
        // AB17c F4 (precondition 11): this remove() runs on a live mutator
        // (tier-up install drain) and mutates the same sentinel list (a
        // drain-local toBeRemoved, or a CodeBlock's m_incomingCalls) that
        // the Repatch.cpp linkers' locked reset()/push() paths mutate — an
        // unlocked removal here tears the neighbors of a
        // concurrently-removed node (observed: CallLinkInfo::reset()
        // remove() crashing on a half-unlinked node under
        // int-gate-stop-budget). Same lock, same rule as
        // CallLinkInfo::unlinkOrUpgradeImpl. isOnList() MUST be
        // (re-)checked UNDER the lock. Since AB18-C the drain
        // (CodeBlock::unlinkOrUpgradeIncomingCalls) holds this same
        // recursive lock across its entire {takeFrom, isEmpty, begin,
        // unlinkOrUpgrade} traversal, so when we arrive from the drain this
        // acquisition nests cheaply; the re-check stays load-bearing for the
        // destruction-context removers that race us from other threads. The
        // lock is recursive, so the nested m_callLinkInfo->unlinkOrUpgrade
        // below re-acquires cheaply.
        Locker locker { CallLinkInfo::s_callLinkSerializationLock };
        if (isOnList())
            remove();
    } else if (isOnList())
        remove();

    if (!m_cleared) {
        if (!newCodeBlock || !owner()->upgradeIfPossible(vm, oldCodeBlock, newCodeBlock, m_index)) {
            m_cleared = true;
            CallLinkInfo* callLinkInfo = owner()->callLinkInfo();
            dataLogLnIf(Options::dumpDisassembly(), "Unlinking polymorphic call bc#", callLinkInfo->codeOrigin().bytecodeIndex());
            callLinkInfo->unlinkOrUpgrade(vm, oldCodeBlock, newCodeBlock);
        }
    }
}

void PolymorphicCallNode::unlinkForcefully()
{
    // AB17c F4 lock-context CONTRACT (precondition 11): gilOff, the caller
    // must hold CallLinkInfo::s_callLinkSerializationLock — this remove()
    // mutates an incoming-calls sentinel list the locked linkers also
    // mutate. All paths funnel through CallLinkInfo::clearStub, whose
    // callers are the locked linker paths (reset/setStub) and
    // ~CallLinkInfo, which takes the lock itself for this reason
    // (CallLinkInfo.cpp).
    m_cleared = true;
    if (isOnList())
        remove();
}

PolymorphicCallStubRoutine* PolymorphicCallNode::owner()
{
    return std::bit_cast<PolymorphicCallStubRoutine*>(this - m_index + m_totalSize);
}

void PolymorphicCallCase::dump(PrintStream& out) const
{
    out.print("<variant = ", m_variant, ", codeBlock = ", pointerDump(m_codeBlock), ">");
}

// TSAN wave 3 (calllink, SPEC-jit 5.8/F6) construction/publication contract:
// the routine must be FULLY constructed before it becomes reachable by any
// lock-free reader. Reachability gilOff:
// - C++ readers (CallLinkStatus on a DFG compiler thread, CallLinkInfo::
//   forEachDependentCell on a concurrent marking thread) reach the routine
//   only through CallLinkInfo::stub(), an ACQUIRE load pairing with
//   setStub's RELEASE publish — which this constructor fully precedes on
//   the linking thread. That edge orders every slot/header/vptr write below
//   before any field read through the published pointer.
// - The mid-constructor linkIncomingCall pushes below expose interior
//   PolymorphicCallNodes on shared incoming-call lists before construction
//   completes, but that is admissible: every list traverser (the
//   unlinkOrUpgradeIncomingCalls drain, destruction-context removers) holds
//   CallLinkInfo::s_callLinkSerializationLock, and linkPolymorphicCall holds
//   that same lock across linkPolymorphicCallImpl — including this entire
//   constructor (bytecode/Repatch.cpp) — so no traverser can observe a
//   half-built node.
// - The JIT'd polymorphic thunk reaches the routine through the published
//   CallLinkRecord (F6); that asm side is outside TSAN's view (covered by
//   the object-model protocol tests, per the campaign charter).
// Freeing is epoch-safe per SPEC-jit 4.4/4.5: clearStub keeps the pointer
// published flag-on and the GC-aware atomic refcount defers reclamation past
// a conservative scan, so a stale reader never sees freed memory.
PolymorphicCallStubRoutine::PolymorphicCallStubRoutine(unsigned headerSize, unsigned trailingSize, const MacroAssemblerCodeRef<JITStubRoutinePtrTag>& code, VM& vm, JSCell* owner, CallFrame* callerFrame, CallLinkInfo& callLinkInfo, const Vector<CallSlot, 16>& callSlots, bool notUsingCounting, bool isClosureCall)
    : GCAwareJITStubRoutine(Type::PolymorphicCallStubRoutineType, code, owner, /* isCodeImmutable */ true)
    , ButterflyArray<PolymorphicCallStubRoutine, PolymorphicCallNode, CallSlot>(headerSize, trailingSize)
    , m_callLinkInfo(&callLinkInfo)
    , m_notUsingCounting(notUsingCounting)
    , m_isClosureCall(isClosureCall)
{
    for (unsigned index = 0; index < callSlots.size(); ++index) {
        auto& slot = trailingSpan()[index];
        slot = callSlots[index];

        if (callerFrame && !callerFrame->isNativeCalleeFrame())
            dataLogLnIf(shouldDumpDisassemblyFor(callerFrame->codeBlock()), "Linking polymorphic call in ", FullCodeOrigin(callerFrame->codeBlock(), callerFrame->codeOrigin()), " to ", CallVariant(slot.m_calleeOrExecutable), ", codeBlock = ", pointerDump(slot.m_codeBlock));

        auto& callNode = leadingSpan()[index];
        callNode.initialize(index, headerSize);
        if (CodeBlock* codeBlock = slot.m_codeBlock)
            codeBlock->linkIncomingCall(owner, &callNode);

        vm.writeBarrier(owner, slot.m_calleeOrExecutable);
    }

    WTF::storeStoreFence();
    makeGCAware(vm);
}

bool PolymorphicCallStubRoutine::upgradeIfPossible(VM& vm, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock, uint8_t index)
{
    // SPEC-jit 5.8 F6 (AB17c F4): the in-place (slot.m_codeBlock,
    // slot.m_target) rewrite below is a two-word mutation of dispatch state
    // other threads are EXECUTING through (the stub reads both words
    // lock-free); a racing reader can pair the new codeBlock with the old
    // entrypoint — the same torn-pair class as the monomorphic mirror
    // rewrite. GIL-off, refuse the upgrade: the caller falls back to the
    // full unlink (record nulled under the link lock), the next call
    // slow-paths and republishes a fresh, internally-consistent stub, and
    // in-flight executions keep using the OLD matched pair (the replaced
    // code stays executable — only jettison retires it, under STW).
    if (vm.gilOff()) [[unlikely]]
        return false;

    // It is possible that we can just upgrade the CallSlot and continue using this PolymorphicCallStubRoutine instead of unlinking CallLinkInfo.
    auto& callNode = leadingSpan()[index];
    auto& slot = trailingSpan()[index];

    if (callNode.isOnList())
        return false;

    if (slot.m_codeBlock != oldCodeBlock)
        return false;

    auto target = newCodeBlock->jitCode()->addressForCall(slot.m_arityCheckMode);
    slot.m_codeBlock = newCodeBlock;
    slot.m_target = target;
    newCodeBlock->linkIncomingCall(nullptr, &callNode); // This is just relinking. So owner and caller frame can be nullptr.
    return true;
}

CallVariantList PolymorphicCallStubRoutine::variants() const
{
    CallVariantList result;
    forEachDependentCell([&](JSCell* cell) {
        result.append(CallVariant(cell));
    });
    return result;
}

bool PolymorphicCallStubRoutine::hasEdges() const
{
    // The FTL does not count edges in its poly call stub routines. If the FTL went poly call, then
    // it's not meaningful to keep profiling - we can just leave it at that. Remember, the FTL would
    // have had full edge profiling from the DFG, and based on this information, it would have
    // decided to go poly.
    //
    // There probably are very-difficult-to-imagine corner cases where the FTL not doing edge
    // profiling is bad for polyvariant inlining. But polyvariant inlining is profitable sometimes
    // while not having to increment counts is profitable always. So, we let the FTL run faster and
    // not keep counts.
    return !m_notUsingCounting;
}

CallEdgeList PolymorphicCallStubRoutine::edges() const
{
    CallEdgeList result;
    unsigned index = 0;
    forEachDependentCell([&](JSCell* cell) {
        // SPEC-jit 5.7 racy-profiling tolerance (TSAN wave 3): the JIT'd
        // polymorphic stub increments m_count concurrently with this
        // compiler-thread read; a torn/stale count only skews inlining
        // heuristics (profiles select, guards validate — I12). Relaxed
        // atomic load makes the C++ side defined; codegen-identical to the
        // plain load.
        uint32_t count = WTF::atomicLoad(const_cast<uint32_t*>(&trailingSpan()[index].m_count), std::memory_order_relaxed);
        result.append(CallEdge(CallVariant(cell), count));
        ++index;
    });
    return result;
}

void PolymorphicCallStubRoutine::unlinkForcefully()
{
    for (auto& callNode : leadingSpan())
        callNode.unlinkForcefully();
}

bool PolymorphicCallStubRoutine::visitWeakImpl(VM& vm)
{
    bool isStillLive = true;
    for (unsigned i = 0, size = std::size(trailingSpan()) - 1; i < size; ++i) {
        auto& slot = trailingSpan()[i];
        if (!slot.m_calleeOrExecutable) {
            isStillLive = false;
            continue;
        }
        if (!vm.heap.isMarked(slot.m_calleeOrExecutable)) {
            slot = CallSlot();
            isStillLive = false;
            continue;
        }
    }
    return isStillLive;
}

void PolymorphicCallStubRoutine::markRequiredObjectsImpl(AbstractSlotVisitor&)
{
}

void PolymorphicCallStubRoutine::markRequiredObjectsImpl(SlotVisitor&)
{
}

void PolymorphicCallStubRoutine::destroy(PolymorphicCallStubRoutine* derived)
{
    delete derived;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
