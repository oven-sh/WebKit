/*
 * Copyright (C) 2008-2024 Apple Inc. All rights reserved.
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
#include "PropertyInlineCache.h"

#include "BaselineJITRegisters.h"
#include "CacheableIdentifierInlines.h"
#include "DFGJITCode.h"
#include "InlineCacheCompiler.h"
#include "JSThreadsSafepoint.h"
#include "Repatch.h"
#include "RetiredJITArtifacts.h"
#include <wtf/Atomics.h>
#include <wtf/ThreadSanitizerSupport.h>

namespace JSC {

#if ENABLE(JIT)

namespace PropertyInlineCacheInternal {
static constexpr bool verbose = false;
}

PropertyInlineCache::~PropertyInlineCache() = default;

void PropertyInlineCache::setInlineAccessSelfState(VM& vm, CodeBlock* codeBlock, Structure* structure, PropertyOffset offset)
{
    if (Options::useJSThreads()) [[unlikely]] {
        // Review round 1: a packed self word keyed on a DICTIONARY structure
        // is unsound (same structureID across butterfly growth; see the
        // dictionary gate in addAccessCase). All flag-on feeders are
        // addAccessCase-admitted handlers, so this cannot fire; keep it as a
        // tripwire for any future direct caller.
        RELEASE_ASSERT(!structure->isDictionary());
        // SPEC-jit section 4.2 (Task 4)/F3: publish the {offset, structureID}
        // pair as ONE aligned 64-bit store. A JIT'd reader on another thread
        // does one relaxed 64-bit load of the same word (compare id half, use
        // offset half), so it sees either the old pair or the new pair, never
        // a valid id with a mismatched offset (I6). Writers are serialized by
        // CodeBlock::m_lock / pre-publication initialization (section 5.7
        // rule 6), so no CAS is needed. The GC write barrier that
        // WriteBarrierStructureID::set() used to provide is preserved by
        // barriering the owning CodeBlock after the store (section 4.2).
        m_packedSelfWord.store(packedInlineAccessSelfWord(structure->id(), offset), std::memory_order_relaxed);
        vm.writeBarrier(codeBlock);
        return;
    }
    m_inlineAccessBaseStructureID.set(vm, codeBlock, structure);
    byIdSelfOffset = offset;
}

void PropertyInlineCache::clearInlineAccessSelfState()
{
    if (Options::useJSThreads()) [[unlikely]] {
        // Section 4.2: invalidation = one 64-bit store {0, StructureID()}.
        // Structure id 0 never matches a live cell, so this is barrier-free
        // and ABA-safe; a racing reader either matched the old pair (and uses
        // its coherent offset) or misses and takes the handler chain.
        m_packedSelfWord.store(0, std::memory_order_relaxed);
        return;
    }
    // Flag-off: exactly today's invalidation - clear the id half only;
    // byIdSelfOffset is unreachable once the structure id is zero.
    m_inlineAccessBaseStructureID.clear();
}

void PropertyInlineCache::initGetByIdSelf(const ConcurrentJSLockerBase& locker, CodeBlock* codeBlock, Structure* inlineAccessBaseStructure, PropertyOffset offset)
{
    ASSERT(m_cacheType == CacheType::Unset);
    ASSERT(hasConstantIdentifier(accessType));
    setCacheType(locker, CacheType::GetByIdSelf);
    setInlineAccessSelfState(codeBlock->vm(), codeBlock, inlineAccessBaseStructure, offset);
}

void PropertyInlineCache::initArrayLength(const ConcurrentJSLockerBase& locker)
{
    ASSERT(m_cacheType == CacheType::Unset);
    setCacheType(locker, CacheType::ArrayLength);
}

void PropertyInlineCache::initStringLength(const ConcurrentJSLockerBase& locker)
{
    ASSERT(m_cacheType == CacheType::Unset);
    setCacheType(locker, CacheType::StringLength);
}

void PropertyInlineCache::initPutByIdReplace(const ConcurrentJSLockerBase& locker, CodeBlock* codeBlock, Structure* inlineAccessBaseStructure, PropertyOffset offset)
{
    ASSERT(m_cacheType == CacheType::Unset);
    ASSERT(hasConstantIdentifier(accessType));
    setCacheType(locker, CacheType::PutByIdReplace);
    setInlineAccessSelfState(codeBlock->vm(), codeBlock, inlineAccessBaseStructure, offset);
}

void PropertyInlineCache::initInByIdSelf(const ConcurrentJSLockerBase& locker, CodeBlock* codeBlock, Structure* inlineAccessBaseStructure, PropertyOffset offset)
{
    ASSERT(m_cacheType == CacheType::Unset);
    ASSERT(hasConstantIdentifier(accessType));
    setCacheType(locker, CacheType::InByIdSelf);
    setInlineAccessSelfState(codeBlock->vm(), codeBlock, inlineAccessBaseStructure, offset);
}

namespace {

// Epoch-deferred destruction of a RepatchingPropertyInlineCache's
// PolymorphicAccess (SPEC-jit section 4.4): pure data (a list of
// Ref<AccessCase>); any executable memory reachable through it is owned by
// GC-aware stub routines whose deletion rides the jettisoned-stub-routine
// machinery (R2/I7), never epoch expiry.
class RetiredPolymorphicAccess final : public RetiredCallback {
public:
    explicit RetiredPolymorphicAccess(std::unique_ptr<PolymorphicAccess>&& stub)
        : m_stub(WTF::move(stub))
    {
    }

private:
    std::unique_ptr<PolymorphicAccess> m_stub;
};

} // anonymous namespace

void PropertyInlineCache::deref(VM& vm, DisarmClearingWatchpoints disarm)
{
    if (Options::useJSThreads()) [[unlikely]] {
        // TSAN ic-stubinfo x-malloc family (SPEC-jit §4.4/I7,
        // "ICSlowPathCallFrameTracer x malloc" stub-reuse pairs): deref() is
        // the jettison/teardown-time neutralization hook (CodeBlock::jettison
        // and ~CodeBlock), after which the IC's own references to the handler
        // chain and the inlined unit handler are eventually dropped inline by
        // ~PropertyInlineCache. A sibling mutator inside its safepoint-free
        // window (G2/I16) can still be dispatching through those nodes, so
        // handler DATA must only die at heap epoch >= retire+1 AND refcount
        // zero (section 4.4). Take an EXTRA Ref on the installed chains here
        // and route it through RetiredJITArtifacts: the published slots stay
        // installed (no null window for racing JIT'd readers; dispatch keeps
        // working until invalidation, I21), and the epoch's reference
        // guarantees the node memory outlives every straggler even after the
        // destructor's inline release. Machine code reachable through each
        // node's Ref<GCAwareJITStubRoutine> still rides the jettison
        // machinery and waits for R2's conservative scan (I7) — the epoch
        // only extends the DATA lifetime. Retiring the same chain twice
        // (jettison then destruction) just holds two epoch refs; harmless.
        // AB18-E/G: the VM& is the caller's, never re-derived via
        // codeBlock->vm().
        // AB18-F amendment (thread-closeout final review): the chains retired
        // here remain INSTALLED. The caller decides whether their clearing
        // watchpoints may be disarmed: only ~CodeBlock (owner dying) passes
        // Yes; jettison and reset pass No so a later watched-set fire still
        // resets the still-dispatching chain (see RetiredJITArtifacts.h).
        if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
            if (handlerIC->m_inlinedHandler)
                RetiredJITArtifacts::retireHandlerChain(vm, RefPtr<InlineCacheHandler> { handlerIC->m_inlinedHandler }, disarm);
        }
        if (m_handler)
            RetiredJITArtifacts::retireHandlerChain(vm, RefPtr<InlineCacheHandler> { m_handler }, disarm);
    }
    if (auto* repatchingIC = dynamicDowncast<RepatchingPropertyInlineCache>(*this)) {
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.1 ("Jettison-time IC deref() same")/I9: never
            // free JIT-reachable IC dispatch state inline; a resumed mutator
            // can still be executing this (jettisoned) code until its next
            // invalidation point (I21), so the data must survive until every
            // mutator has crossed a safepoint. Flag-on this branch should be
            // unreachable anyway (I3: no RepatchingPropertyInlineCache is ever
            // constructed), but stay safe rather than assume.
            if (repatchingIC->m_stub)
                RetiredJITArtifacts::retire(vm, std::unique_ptr<RetiredCallback>(new RetiredPolymorphicAccess(std::exchange(repatchingIC->m_stub, nullptr))));
            return;
        }
        repatchingIC->m_stub.reset();
    }
    UNUSED_PARAM(vm);
}

void PropertyInlineCache::aboutToDie()
{
    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        if (handlerIC->m_inlinedHandler)
            handlerIC->m_inlinedHandler->aboutToDie();
    }

    if (auto* cursor = m_handler.get()) {
        while (cursor) {
            cursor->aboutToDie();
            cursor = cursor->next();
        }
    }
}

AccessGenerationResult PropertyInlineCache::upgradeForPolyProtoIfNecessary(const GCSafeConcurrentJSLocker&, VM&, CodeBlock*, const Vector<AccessCase*, 16>& list, AccessCase& caseToAdd)
{
    // This method will add the casesToAdd to the list one at a time while preserving the
    // invariants:
    // - If a newly added case canReplace() any existing case, then the existing case is removed before
    //   the new case is added. Removal doesn't change order of the list. Any number of existing cases
    //   can be removed via the canReplace() rule.
    // - Cases in the list always appear in ascending order of time of addition. Therefore, if you
    //   cascade through the cases in reverse order, you will get the most recent cases first.
    // - If this method fails (returns null, doesn't add the cases), then both the previous case list
    //   and the previous stub are kept intact and the new cases are destroyed. It's OK to attempt to
    //   add more things after failure.

    if (accessType != AccessType::InstanceOf) {
        bool shouldReset = false;
        AccessGenerationResult resetResult(AccessGenerationResult::ResetStubAndFireWatchpoints);
        auto considerPolyProtoReset = [&] (Structure* a, Structure* b) {
            if (Structure::shouldConvertToPolyProto(a, b)) {
                // For now, we only reset if this is our first time invalidating this watchpoint.
                // The reason we don't immediately fire this watchpoint is that we may be already
                // watching the poly proto watchpoint, which if fired, would destroy us. We let
                // the person handling the result to do a delayed fire.
                ASSERT(a->rareData()->sharedPolyProtoWatchpoint().get() == b->rareData()->sharedPolyProtoWatchpoint().get());
                if (a->rareData()->sharedPolyProtoWatchpoint()->isStillValid()) {
                    shouldReset = true;
                    resetResult.addWatchpointToFire(*a->rareData()->sharedPolyProtoWatchpoint(), StringFireDetail("Detected poly proto optimization opportunity."));
                }
            }
        };

        for (auto& existingCase : list) {
            Structure* a = caseToAdd.structure();
            Structure* b = existingCase->structure();
            considerPolyProtoReset(a, b);
        }

        if (shouldReset)
            return resetResult;
    }
    return AccessGenerationResult::Buffered;
}

AccessGenerationResult PropertyInlineCache::addAccessCase(const GCSafeConcurrentJSLocker& locker, JSGlobalObject* globalObject, CodeBlock* codeBlock, ECMAMode ecmaMode, CacheableIdentifier ident, RefPtr<AccessCase> accessCase)
{
    checkConsistency();

    VM& vm = codeBlock->vm();
    ASSERT(vm.heap.isDeferred());

    if (!accessCase)
        return AccessGenerationResult::GaveUp;

    // SPEC-jit §5.5 / THREAD.md dictionary rule (review round 1): never install
    // an AccessCase keyed on a DICTIONARY base structure flag-on. A dictionary
    // keeps the same structureID while its owner adds properties (butterfly
    // realloc + larger offsets), so the structureID-check + R7 dependency
    // soundness argument for handler self/replace fast paths proves nothing:
    // a racing reader could pair a matching structureID with a stale, smaller
    // butterfly => OOB read (Load) or OOB WRITE (Replace). THREAD.md requires
    // dictionary reads/writes to take the structure lock - i.e. the generic
    // C++ path, never a generated fast path. This is the single funnel for
    // every repatch-driven IC (Get/Put/In/Delete/...), and it also covers the
    // inlined packed self word (setInlinedHandler), which is only fed from
    // handlers admitted here. The mirroring LLInt rule lives in
    // LLIntSlowPaths.cpp (threaded publish gates).
    if (Options::useJSThreads() && accessCase->structure() && accessCase->structure()->isDictionary()) [[unlikely]]
        return AccessGenerationResult::GaveUp;

    AccessGenerationResult result = ([&](Ref<AccessCase>&& accessCase) -> AccessGenerationResult {
        dataLogLnIf(PropertyInlineCacheInternal::verbose, "Adding access case: ", accessCase);

        if (is<HandlerPropertyInlineCache>(*this)) {
            auto list = listedAccessCases(locker);
            auto result = upgradeForPolyProtoIfNecessary(locker, vm, codeBlock, list, accessCase.get());
            dataLogLnIf(PropertyInlineCacheInternal::verbose, "Had stub, result: ", result);

            if (result.shouldResetStubAndFireWatchpoints())
                return result;

            if (!result.buffered()) {
                clearBufferedStructures();
                return result;
            }
            setCacheType(locker, CacheType::Stub);

            RELEASE_ASSERT(!result.generatedSomeCode());

            // If we didn't buffer any cases then bail. If this made no changes then we'll just try again
            // subject to cool-down.
            if (!result.buffered()) {
                dataLogLnIf(PropertyInlineCacheInternal::verbose, "Didn't buffer anything, bailing.");
                clearBufferedStructures();
                return result;
            }

            InlineCacheCompiler compiler(codeBlock->jitType(), vm, globalObject, ecmaMode, *this);
            return compiler.compileHandler(locker, WTF::move(list), codeBlock, accessCase.get());
        }

        auto& repatchingIC = downcast<RepatchingPropertyInlineCache>(*this);
        AccessGenerationResult result;
        if (repatchingIC.m_stub) {
            result = repatchingIC.m_stub->addCases(locker, vm, codeBlock, *this, nullptr, accessCase);
            dataLogLnIf(PropertyInlineCacheInternal::verbose, "Had stub, result: ", result);

            if (result.shouldResetStubAndFireWatchpoints())
                return result;

            if (!result.buffered()) {
                clearBufferedStructures();
                return result;
            }
        } else {
            std::unique_ptr<PolymorphicAccess> access = makeUnique<PolymorphicAccess>();
            result = access->addCases(locker, vm, codeBlock, *this, AccessCase::fromPropertyInlineCache(vm, codeBlock, ident, *this), accessCase);

            dataLogLnIf(PropertyInlineCacheInternal::verbose, "Created stub, result: ", result);

            if (result.shouldResetStubAndFireWatchpoints())
                return result;

            if (!result.buffered()) {
                clearBufferedStructures();
                return result;
            }

            setCacheType(locker, CacheType::Stub);
            repatchingIC.m_stub = WTF::move(access);
        }

        ASSERT(m_cacheType == CacheType::Stub);
        RELEASE_ASSERT(!result.generatedSomeCode());

        // If we didn't buffer any cases then bail. If this made no changes then we'll just try again
        // subject to cool-down.
        if (!result.buffered()) {
            dataLogLnIf(PropertyInlineCacheInternal::verbose, "Didn't buffer anything, bailing.");
            clearBufferedStructures();
            return result;
        }

        // The buffering countdown tells us if we should be repatching now.
        if (uint8_t bufferingCountdownValue = bufferingCountdown.loadRelaxed()) {
            dataLogLnIf(PropertyInlineCacheInternal::verbose, "Countdown is too high: ", bufferingCountdownValue, ".");
            return result;
        }

        // Forget the buffered structures so that all future attempts to cache get fully handled by the
        // PolymorphicAccess.
        clearBufferedStructures();

        InlineCacheCompiler compiler(codeBlock->jitType(), vm, globalObject, ecmaMode, *this);
        result = compiler.compile(locker, *repatchingIC.m_stub, codeBlock);

        dataLogLnIf(PropertyInlineCacheInternal::verbose, "Regeneration result: ", result);

        RELEASE_ASSERT(!result.buffered());

        if (!result.generatedSomeCode())
            return result;

        // Repatching IC: When we first transition to becoming a Stub, we might still be running the inline
        // access code. That's because when we first transition to becoming a Stub, we may
        // be buffered, and we have not yet generated any code. Once the Stub finally generates
        // code, we're no longer running the inline access code, so we can then clear out
        // m_inlineAccessBaseStructureID. The reason we don't clear m_inlineAccessBaseStructureID while
        // we're buffered is because we rely on it to reset during GC if m_inlineAccessBaseStructureID
        // is collected.
        clearInlineAccessSelfState();

        // If we generated some code then we don't want to attempt to repatch in the future until we
        // gather enough cases.
        bufferingCountdown.storeRelaxed(static_cast<uint8_t>(Options::repatchBufferingCountdown()));
        return result;
    })(accessCase.releaseNonNull());
    if (result.generatedSomeCode()) {
        if (is<HandlerPropertyInlineCache>(*this))
            prependHandler(vm, codeBlock, Ref { *result.handler() }, result.generatedMegamorphicCode());
        else
            rewireStubAsJumpInAccess(vm, codeBlock, Ref { *result.handler() });
    }

    vm.writeBarrier(codeBlock);
    return result;
}

void PropertyInlineCache::reset(const ConcurrentJSLockerBase& locker, VM& vm, CodeBlock* codeBlock)
{
    clearBufferedStructures();
    clearInlineAccessSelfState();
    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        if (handlerIC->m_inlinedHandler) {
            // Review round 3 (R3-2): same I9 discipline as
            // initializeWithUnitHandler / resetStubAsJumpInAccess — under
            // useJSThreads a JIT'd reader on another thread may be inside its
            // safepoint-free window (G2) holding pointers into the displaced
            // inlined unit handler, so it must never be dropped inline. This
            // matters even though some reset() callers run world-stopped:
            // reset() is also reachable flag-on OUTSIDE any stop
            // (fireWatchpointsAndClearStubIfNeeded in Repatch.cpp on the
            // addAccessCase ResetStubAndFireWatchpoints path, and
            // PropertyInlineCacheClearingWatchpoint). Note this also means
            // the displacedInlinedHandler retire arm in
            // resetStubAsJumpInAccess never sees a handler when reached via
            // reset() — m_inlinedHandler is already null and retired HERE by
            // the time the per-accessType reset functions run; that arm
            // remains live for its other callers.
            RefPtr<InlineCacheHandler> displacedInlinedHandler;
            if (Options::useJSThreads()) [[unlikely]]
                displacedInlinedHandler = handlerIC->m_inlinedHandler;
            handlerIC->clearInlinedHandler(codeBlock);
            // AB18-E rule: retire against the CALLER's VM&, never
            // codeBlock->vm() — deriving the retire VM through the cell's
            // MarkedBlock is the stale-owner pattern behind the
            // DirectCallLinkInfo::retireRecord UAF (sig-1); any future reset
            // path reached through retired/leaked IC state would reproduce
            // it byte-for-byte.
            if (displacedInlinedHandler) [[unlikely]]
                RetiredJITArtifacts::retireHandlerChain(vm, WTF::move(displacedInlinedHandler), DisarmClearingWatchpoints::Yes);
        }
    }

    if (m_cacheType == CacheType::Unset)
        return;

    // This can be called from GC destructor calls, so we don't try to do a full dump
    // of the CodeBlock.
    dataLogLnIf(Options::verboseOSR(), "Clearing structure cache (kind ", static_cast<int>(accessType), ") in ", RawPointer(codeBlock), ".");

    switch (accessType) {
    case AccessType::TryGetById:
        resetGetBy(vm, codeBlock, *this, GetByKind::TryById);
        break;
    case AccessType::GetById:
        resetGetBy(vm, codeBlock, *this, GetByKind::ById);
        break;
    case AccessType::GetByIdWithThis:
        resetGetBy(vm, codeBlock, *this, GetByKind::ByIdWithThis);
        break;
    case AccessType::GetByIdDirect:
        resetGetBy(vm, codeBlock, *this, GetByKind::ByIdDirect);
        break;
    case AccessType::GetByVal:
        resetGetBy(vm, codeBlock, *this, GetByKind::ByVal);
        break;
    case AccessType::GetByValWithThis:
        resetGetBy(vm, codeBlock, *this, GetByKind::ByValWithThis);
        break;
    case AccessType::GetPrivateName:
        resetGetBy(vm, codeBlock, *this, GetByKind::PrivateName);
        break;
    case AccessType::GetPrivateNameById:
        resetGetBy(vm, codeBlock, *this, GetByKind::PrivateNameById);
        break;
    case AccessType::PutByIdStrict:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByIdStrict);
        break;
    case AccessType::PutByIdSloppy:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByIdSloppy);
        break;
    case AccessType::PutByIdDirectStrict:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByIdDirectStrict);
        break;
    case AccessType::PutByIdDirectSloppy:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByIdDirectSloppy);
        break;
    case AccessType::PutByValStrict:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByValStrict);
        break;
    case AccessType::PutByValSloppy:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByValSloppy);
        break;
    case AccessType::PutByValDirectStrict:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByValDirectStrict);
        break;
    case AccessType::PutByValDirectSloppy:
        resetPutBy(vm, codeBlock, *this, PutByKind::ByValDirectSloppy);
        break;
    case AccessType::DefinePrivateNameById:
        resetPutBy(vm, codeBlock, *this, PutByKind::DefinePrivateNameById);
        break;
    case AccessType::SetPrivateNameById:
        resetPutBy(vm, codeBlock, *this, PutByKind::SetPrivateNameById);
        break;
    case AccessType::DefinePrivateNameByVal:
        resetPutBy(vm, codeBlock, *this, PutByKind::DefinePrivateNameByVal);
        break;
    case AccessType::SetPrivateNameByVal:
        resetPutBy(vm, codeBlock, *this, PutByKind::SetPrivateNameByVal);
        break;
    case AccessType::InById:
        resetInBy(vm, codeBlock, *this, InByKind::ById);
        break;
    case AccessType::InByVal:
        resetInBy(vm, codeBlock, *this, InByKind::ByVal);
        break;
    case AccessType::HasPrivateName:
        resetInBy(vm, codeBlock, *this, InByKind::PrivateName);
        break;
    case AccessType::HasPrivateBrand:
        resetHasPrivateBrand(vm, codeBlock, *this);
        break;
    case AccessType::InstanceOf:
        resetInstanceOf(vm, codeBlock, *this);
        break;
    case AccessType::DeleteByIdStrict:
        resetDelBy(vm, codeBlock, *this, DelByKind::ByIdStrict);
        break;
    case AccessType::DeleteByIdSloppy:
        resetDelBy(vm, codeBlock, *this, DelByKind::ByIdSloppy);
        break;
    case AccessType::DeleteByValStrict:
        resetDelBy(vm, codeBlock, *this, DelByKind::ByValStrict);
        break;
    case AccessType::DeleteByValSloppy:
        resetDelBy(vm, codeBlock, *this, DelByKind::ByValSloppy);
        break;
    case AccessType::CheckPrivateBrand:
        resetCheckPrivateBrand(vm, codeBlock, *this);
        break;
    case AccessType::SetPrivateBrand:
        resetSetPrivateBrand(vm, codeBlock, *this);
        break;
    }

    // AB18-F: use the caller's VM& (same rule as the inlined-handler retire
    // above) — `deref(codeBlock->vm())` re-derived the retire VM through the
    // owner cell's MarkedBlock header, the exact sig-1 stale-owner pattern
    // this function's own comment bans. DisarmClearingWatchpoints::No: the
    // owner is alive and the head now installed is the fresh slow-path-only
    // chain published by the per-accessType reset above (the displaced chain
    // was already retired WITH disarm at its displacement site); disarming an
    // installed live-owner chain here would skip future IC resets.
    deref(vm, DisarmClearingWatchpoints::No);
    setCacheType(locker, CacheType::Unset);
}

template<typename Visitor>
void PropertyInlineCache::visitAggregateImpl(Visitor& visitor)
{
    // §10.4 "x identifier": GC-thread read of m_identifier may race the IC's
    // link-time initialization on a mutator; take one relaxed snapshot.
    CacheableIdentifier identifierSnapshot = identifier();
    if (!identifierSnapshot) {
        Locker locker { m_bufferedStructuresLock };
        WTF::switchOn(m_bufferedStructures,
            [&](std::monostate) { },
            [&](Vector<StructureID>&) { },
            [&](Vector<std::tuple<StructureID, CacheableIdentifier>>& structures) {
                for (auto& [bufferedStructureID, bufferedCacheableIdentifier] : structures)
                    bufferedCacheableIdentifier.visitAggregate(visitor);
            });
    } else
        identifierSnapshot.visitAggregate(visitor);

    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        if (handlerIC->m_inlinedHandler)
            handlerIC->m_inlinedHandler->visitAggregate(visitor);
    }
    if (auto* cursor = m_handler.get()) {
        while (cursor) {
            cursor->visitAggregate(visitor);
            cursor = cursor->next();
        }
    }

    if (auto* repatchingIC = dynamicDowncast<RepatchingPropertyInlineCache>(*this)) {
        if (repatchingIC->m_stub)
            repatchingIC->m_stub->visitAggregate(visitor);
    }
}

DEFINE_VISIT_AGGREGATE(PropertyInlineCache);

void PropertyInlineCache::visitWeak(const ConcurrentJSLockerBase& locker, CodeBlock* codeBlock)
{
    VM& vm = codeBlock->vm();
    {
        Locker locker { m_bufferedStructuresLock };
        WTF::switchOn(m_bufferedStructures,
            [&](std::monostate) { },
            [&](Vector<StructureID>& structures) {
                structures.removeAllMatching([&](StructureID structureID) {
                    return !vm.heap.isMarked(structureID.decode());
                });
            },
            [&](Vector<std::tuple<StructureID, CacheableIdentifier>>& structures) {
                structures.removeAllMatching([&](auto& tuple) {
                    return !vm.heap.isMarked(std::get<0>(tuple).decode());
                });
            });
    }

    bool isValid = true;
    if (Structure* structure = inlineAccessBaseStructure())
        isValid &= vm.heap.isMarked(structure);

    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        if (handlerIC->m_inlinedHandler)
            isValid &= handlerIC->m_inlinedHandler->visitWeak(vm);
    }
    if (auto* cursor = m_handler.get()) {
        while (cursor) {
            isValid &= cursor->visitWeak(vm);
            cursor = cursor->next();
        }
    }

    if (auto* repatchingIC = dynamicDowncast<RepatchingPropertyInlineCache>(*this)) {
        if (repatchingIC->m_stub)
            isValid &= repatchingIC->m_stub->visitWeak(vm);
    }

    if (isValid)
        return;

    reset(locker, vm, codeBlock);
    resetByGC = true;
}

template<typename Visitor>
void PropertyInlineCache::propagateTransitions(Visitor& visitor)
{
    if (Structure* structure = inlineAccessBaseStructure())
        structure->markIfCheap(visitor);

    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        if (handlerIC->m_inlinedHandler)
            handlerIC->m_inlinedHandler->propagateTransitions(visitor);
        if (auto* cursor = m_handler.get()) {
            while (cursor) {
                cursor->propagateTransitions(visitor);
                cursor = cursor->next();
            }
        }
    } else if (auto* repatchingIC = dynamicDowncast<RepatchingPropertyInlineCache>(*this)) {
        if (repatchingIC->m_stub)
            repatchingIC->m_stub->propagateTransitions(visitor);
    }
}

template void PropertyInlineCache::propagateTransitions(AbstractSlotVisitor&);
template void PropertyInlineCache::propagateTransitions(SlotVisitor&);

CallLinkInfo* PropertyInlineCache::callLinkInfoAt(const ConcurrentJSLocker& locker, unsigned index, const AccessCase& accessCase)
{
    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        if (handlerIC->m_inlinedHandler) {
            if (handlerIC->m_inlinedHandler->accessCase() == &accessCase)
                return downcast<InlineCacheHandlerWithJSCall>(*handlerIC->m_inlinedHandler).callLinkInfo(locker);
        }

        if (auto* cursor = m_handler.get()) {
            while (cursor) {
                if (cursor->accessCase() == &accessCase)
                    return downcast<InlineCacheHandlerWithJSCall>(*cursor).callLinkInfo(locker);
                cursor = cursor->next();
            }
        }
        return nullptr;
    }

    if (!m_handler)
        return nullptr;
    if (!m_handler->stubRoutine())
        return nullptr;
    return m_handler->stubRoutine()->callLinkInfoAt(locker, index);
}

PropertyInlineCacheSummary PropertyInlineCache::summary(const ConcurrentJSLocker& locker, VM& vm) const
{
    PropertyInlineCacheSummary takesSlowPath = PropertyInlineCacheSummary::TakesSlowPath;
    PropertyInlineCacheSummary simple = PropertyInlineCacheSummary::Simple;
    auto list = listedAccessCases(locker);
    for (unsigned i = 0; i < list.size(); ++i) {
        AccessCase& access = *list.at(i);
        if (access.doesCalls(vm)) {
            takesSlowPath = PropertyInlineCacheSummary::TakesSlowPathAndMakesCalls;
            simple = PropertyInlineCacheSummary::MakesCalls;
            break;
        }
    }
    if (list.size() == 1) {
        switch (list.at(0)->type()) {
        case AccessCase::LoadMegamorphic:
        case AccessCase::IndexedMegamorphicLoad:
        case AccessCase::StoreMegamorphic:
        case AccessCase::IndexedMegamorphicStore:
        case AccessCase::InMegamorphic:
        case AccessCase::IndexedMegamorphicIn:
            return PropertyInlineCacheSummary::Megamorphic;
        default:
            break;
        }
    }

    if (tookSlowPath || sawNonCell)
        return takesSlowPath;

    if (!everConsidered)
        return PropertyInlineCacheSummary::NoInformation;

    return simple;
}

PropertyInlineCacheSummary PropertyInlineCache::summary(const ConcurrentJSLocker& locker, VM& vm, const PropertyInlineCache* propertyCache)
{
    if (!propertyCache)
        return PropertyInlineCacheSummary::NoInformation;

    return propertyCache->summary(locker, vm);
}

bool PropertyInlineCache::containsPC(void* pc) const
{
    // m_inlinedHandler is not having special out-of-inline code, so we do not care.
    if (auto* cursor = m_handler.get()) {
        while (cursor) {
            if (cursor->containsPC(pc))
                return true;
            cursor = cursor->next();
        }
    }
    return false;
}

ALWAYS_INLINE void PropertyInlineCache::setCacheType(const ConcurrentJSLockerBase&, CacheType newCacheType)
{
    m_cacheType = newCacheType;
}

static CodePtr<OperationPtrTag> NODELETE slowOperationFromUnlinkedPropertyInlineCache(const UnlinkedPropertyInlineCache& unlinkedPropertyCache)
{
    switch (unlinkedPropertyCache.accessType) {
    case AccessType::DeleteByValStrict:
        return operationDeleteByValStrictOptimize;
    case AccessType::DeleteByValSloppy:
        return operationDeleteByValSloppyOptimize;
    case AccessType::DeleteByIdStrict:
        return operationDeleteByIdStrictOptimize;
    case AccessType::DeleteByIdSloppy:
        return operationDeleteByIdSloppyOptimize;
    case AccessType::GetByVal:
        return operationGetByValOptimize;
    case AccessType::InstanceOf:
        return operationInstanceOfOptimize;
    case AccessType::InByVal:
        return operationInByValOptimize;
    case AccessType::InById:
        return operationInByIdOptimize;
    case AccessType::GetById:
        return operationGetByIdOptimize;
    case AccessType::TryGetById:
        return operationTryGetByIdOptimize;
    case AccessType::GetByIdDirect:
        return operationGetByIdDirectOptimize;
    case AccessType::GetByIdWithThis:
        return operationGetByIdWithThisOptimize;
    case AccessType::GetByValWithThis:
        return operationGetByValWithThisOptimize;
    case AccessType::HasPrivateName:
        return operationHasPrivateNameOptimize;
    case AccessType::HasPrivateBrand:
        return operationHasPrivateBrandOptimize;
    case AccessType::GetPrivateName:
        return operationGetPrivateNameOptimize;
    case AccessType::GetPrivateNameById:
        return operationGetPrivateNameByIdOptimize;
    case AccessType::PutByIdStrict:
        return operationPutByIdStrictOptimize;
    case AccessType::PutByIdSloppy:
        return operationPutByIdSloppyOptimize;
    case AccessType::PutByIdDirectStrict:
        return operationPutByIdDirectStrictOptimize;
    case AccessType::PutByIdDirectSloppy:
        return operationPutByIdDirectSloppyOptimize;
    case AccessType::PutByValStrict:
        return operationPutByValStrictOptimize;
    case AccessType::PutByValSloppy:
        return operationPutByValSloppyOptimize;
    case AccessType::PutByValDirectStrict:
        return operationDirectPutByValStrictOptimize;
    case AccessType::PutByValDirectSloppy:
        return operationDirectPutByValSloppyOptimize;
    case AccessType::DefinePrivateNameById:
        return operationPutByIdDefinePrivateFieldStrictOptimize;
    case AccessType::SetPrivateNameById:
        return operationPutByIdSetPrivateFieldStrictOptimize;
    case AccessType::DefinePrivateNameByVal:
        return operationPutByValDefinePrivateFieldOptimize;
    case AccessType::SetPrivateNameByVal:
        return operationPutByValSetPrivateFieldOptimize;
    case AccessType::SetPrivateBrand:
        return operationSetPrivateBrandOptimize;
    case AccessType::CheckPrivateBrand:
        return operationCheckPrivateBrandOptimize;
    }
    return { };
}

void PropertyInlineCache::initializePredefinedRegisters()
{
    switch (accessType) {
    case AccessType::DeleteByValStrict:
    case AccessType::DeleteByValSloppy:
        m_baseGPR = BaselineJITRegisters::DelByVal::baseJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::DelByVal::propertyJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::DelByVal::resultJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::DelByVal::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_baseTagGPR = BaselineJITRegisters::DelByVal::baseJSR.tagGPR();
        m_extraTagGPR = BaselineJITRegisters::DelByVal::propertyJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::DelByVal::resultJSR.tagGPR();
#endif
        break;
    case AccessType::DeleteByIdStrict:
    case AccessType::DeleteByIdSloppy:
        m_baseGPR = BaselineJITRegisters::DelById::baseJSR.payloadGPR();
        m_extraGPR = InvalidGPRReg;
        m_valueGPR = BaselineJITRegisters::DelById::resultJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::DelById::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_baseTagGPR = BaselineJITRegisters::DelById::baseJSR.tagGPR();
        m_extraTagGPR = InvalidGPRReg;
        m_valueTagGPR = BaselineJITRegisters::DelById::resultJSR.tagGPR();
#endif
        break;
    case AccessType::GetByVal:
    case AccessType::GetPrivateName:
        m_baseGPR = BaselineJITRegisters::GetByVal::baseJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::GetByVal::propertyJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::GetByVal::resultJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::GetByVal::propertyCacheGPR;
        if (accessType == AccessType::GetByVal)
            m_arrayProfileGPR = BaselineJITRegisters::GetByVal::profileGPR;
#if USE(JSVALUE32_64)
        m_baseTagGPR = BaselineJITRegisters::GetByVal::baseJSR.tagGPR();
        m_extraTagGPR = BaselineJITRegisters::GetByVal::propertyJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::GetByVal::resultJSR.tagGPR();
#endif
        break;
    case AccessType::InstanceOf:
        prototypeIsKnownObject = false;
        m_baseGPR = BaselineJITRegisters::Instanceof::valueJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::Instanceof::resultJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::Instanceof::protoJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::Instanceof::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_baseTagGPR = BaselineJITRegisters::Instanceof::valueJSR.tagGPR();
        m_valueTagGPR = InvalidGPRReg;
        m_extraTagGPR = BaselineJITRegisters::Instanceof::protoJSR.tagGPR();
#endif
        break;
    case AccessType::InByVal:
    case AccessType::HasPrivateName:
    case AccessType::HasPrivateBrand:
        m_baseGPR = BaselineJITRegisters::InByVal::baseJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::InByVal::propertyJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::InByVal::resultJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::InByVal::propertyCacheGPR;
        if (accessType == AccessType::InByVal)
            m_arrayProfileGPR = BaselineJITRegisters::InByVal::profileGPR;
#if USE(JSVALUE32_64)
        m_baseTagGPR = BaselineJITRegisters::InByVal::baseJSR.tagGPR();
        m_extraTagGPR = BaselineJITRegisters::InByVal::propertyJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::InByVal::resultJSR.tagGPR();
#endif
        break;
    case AccessType::InById:
        m_extraGPR = InvalidGPRReg;
        m_baseGPR = BaselineJITRegisters::InById::baseJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::InById::resultJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::InById::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_extraTagGPR = InvalidGPRReg;
        m_baseTagGPR = BaselineJITRegisters::InById::baseJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::InById::resultJSR.tagGPR();
#endif
        break;
    case AccessType::TryGetById:
    case AccessType::GetByIdDirect:
    case AccessType::GetById:
    case AccessType::GetPrivateNameById:
        m_extraGPR = InvalidGPRReg;
        m_baseGPR = BaselineJITRegisters::GetById::baseJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::GetById::resultJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::GetById::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_extraTagGPR = InvalidGPRReg;
        m_baseTagGPR = BaselineJITRegisters::GetById::baseJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::GetById::resultJSR.tagGPR();
#endif
        break;
    case AccessType::GetByIdWithThis:
        m_baseGPR = BaselineJITRegisters::GetByIdWithThis::baseJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::GetByIdWithThis::resultJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::GetByIdWithThis::thisJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::GetByIdWithThis::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_baseTagGPR = BaselineJITRegisters::GetByIdWithThis::baseJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::GetByIdWithThis::resultJSR.tagGPR();
        m_extraTagGPR = BaselineJITRegisters::GetByIdWithThis::thisJSR.tagGPR();
#endif
        break;
    case AccessType::GetByValWithThis:
#if USE(JSVALUE64)
        m_baseGPR = BaselineJITRegisters::GetByValWithThis::baseJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::GetByValWithThis::resultJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::GetByValWithThis::thisJSR.payloadGPR();
        m_extra2GPR = BaselineJITRegisters::GetByValWithThis::propertyJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::GetByValWithThis::propertyCacheGPR;
        m_arrayProfileGPR = BaselineJITRegisters::GetByValWithThis::profileGPR;
#else
        // Registers are exhausted, we cannot have this IC on 32bit.
        RELEASE_ASSERT_NOT_REACHED();
#endif
        break;
    case AccessType::PutByIdStrict:
    case AccessType::PutByIdSloppy:
    case AccessType::PutByIdDirectStrict:
    case AccessType::PutByIdDirectSloppy:
    case AccessType::DefinePrivateNameById:
    case AccessType::SetPrivateNameById:
        m_extraGPR = InvalidGPRReg;
        m_baseGPR = BaselineJITRegisters::PutById::baseJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::PutById::valueJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::PutById::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_extraTagGPR = InvalidGPRReg;
        m_baseTagGPR = BaselineJITRegisters::PutById::baseJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::PutById::valueJSR.tagGPR();
#endif
        break;
    case AccessType::PutByValStrict:
    case AccessType::PutByValSloppy:
    case AccessType::PutByValDirectStrict:
    case AccessType::PutByValDirectSloppy:
    case AccessType::DefinePrivateNameByVal:
    case AccessType::SetPrivateNameByVal:
        m_baseGPR = BaselineJITRegisters::PutByVal::baseJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::PutByVal::propertyJSR.payloadGPR();
        m_valueGPR = BaselineJITRegisters::PutByVal::valueJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::PutByVal::propertyCacheGPR;
        if (accessType != AccessType::DefinePrivateNameByVal && accessType != AccessType::SetPrivateNameByVal)
            m_arrayProfileGPR = BaselineJITRegisters::PutByVal::profileGPR;
#if USE(JSVALUE32_64)
        m_baseTagGPR = BaselineJITRegisters::PutByVal::baseJSR.tagGPR();
        m_extraTagGPR = BaselineJITRegisters::PutByVal::propertyJSR.tagGPR();
        m_valueTagGPR = BaselineJITRegisters::PutByVal::valueJSR.tagGPR();
#endif
        break;
    case AccessType::SetPrivateBrand:
    case AccessType::CheckPrivateBrand:
        m_valueGPR = InvalidGPRReg;
        m_baseGPR = BaselineJITRegisters::PrivateBrand::baseJSR.payloadGPR();
        m_extraGPR = BaselineJITRegisters::PrivateBrand::propertyJSR.payloadGPR();
        m_propertyCacheGPR = BaselineJITRegisters::PrivateBrand::propertyCacheGPR;
#if USE(JSVALUE32_64)
        m_valueTagGPR = InvalidGPRReg;
        m_baseTagGPR = BaselineJITRegisters::PrivateBrand::baseJSR.tagGPR();
        m_extraTagGPR = BaselineJITRegisters::PrivateBrand::propertyJSR.tagGPR();
#endif
        break;
    }
}

void HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache(VM& vm, CodeBlock* codeBlock, const BaselineUnlinkedPropertyInlineCache& unlinkedPropertyCache)
{
    ASSERT(!isCompilationThread());
    accessType = unlinkedPropertyCache.accessType;
    preconfiguredCacheType = unlinkedPropertyCache.preconfiguredCacheType;
    switch (preconfiguredCacheType) {
    case CacheType::ArrayLength:
        m_cacheType = CacheType::ArrayLength;
        break;
    default:
        break;
    }
    doneLocation = unlinkedPropertyCache.doneLocation;
    // TSAN ic-stubinfo "ctor-publication x identifier" (§10.4): relaxed store,
    // pairing with the relaxed identifier() reader (see PropertyInlineCache.h).
    setIdentifierConcurrently(unlinkedPropertyCache.m_identifier);
    m_globalObject = codeBlock->globalObject();
    callSiteIndex = CallSiteIndex(BytecodeIndex(unlinkedPropertyCache.bytecodeIndex.offset()));
    codeOrigin = CodeOrigin(unlinkedPropertyCache.bytecodeIndex);
    initializeWithUnitHandler(vm, codeBlock, InlineCacheCompiler::generateSlowPathHandler(vm, accessType));
    propertyIsInt32 = unlinkedPropertyCache.propertyIsInt32;
    canBeMegamorphic = unlinkedPropertyCache.canBeMegamorphic;

    if (unlinkedPropertyCache.canBeMegamorphic)
        bufferingCountdown.storeRelaxed(1);

    usedRegisters = RegisterSet::stubUnavailableRegisters().toScalarRegisterSet();

    m_slowOperation = slowOperationFromUnlinkedPropertyInlineCache(unlinkedPropertyCache);
    initializePredefinedRegisters();
}

#if ENABLE(DFG_JIT)
void HandlerPropertyInlineCache::initializeFromDFGUnlinkedPropertyInlineCache(CodeBlock* codeBlock, const DFG::UnlinkedPropertyInlineCache& unlinkedPropertyCache)
{
    ASSERT(!isCompilationThread());
    accessType = unlinkedPropertyCache.accessType;
    preconfiguredCacheType = unlinkedPropertyCache.preconfiguredCacheType;
    switch (preconfiguredCacheType) {
    case CacheType::ArrayLength:
        m_cacheType = CacheType::ArrayLength;
        break;
    default:
        break;
    }
    doneLocation = unlinkedPropertyCache.doneLocation;
    // TSAN ic-stubinfo "ctor-publication x identifier" (§10.4): relaxed store,
    // pairing with the relaxed identifier() reader (see PropertyInlineCache.h).
    setIdentifierConcurrently(unlinkedPropertyCache.m_identifier);
    callSiteIndex = unlinkedPropertyCache.callSiteIndex;
    codeOrigin = unlinkedPropertyCache.codeOrigin;
    if (codeOrigin.inlineCallFrame())
        m_globalObject = baselineCodeBlockForInlineCallFrame(codeOrigin.inlineCallFrame())->globalObject();
    else
        m_globalObject = codeBlock->globalObject();
    // AB18-G: derivation here is LINK-time — this codeBlock is being
    // installed right now, provably live, and the unit-handler install has no
    // displaced chain to retire. The VM& flows from here into every deeper
    // retire path instead of being re-derived there.
    VM& vm = codeBlock->vm();
    initializeWithUnitHandler(vm, codeBlock, InlineCacheCompiler::generateSlowPathHandler(vm, accessType));

    propertyIsInt32 = unlinkedPropertyCache.propertyIsInt32;
    propertyIsSymbol = unlinkedPropertyCache.propertyIsSymbol;
    propertyIsString = unlinkedPropertyCache.propertyIsString;
    prototypeIsKnownObject = unlinkedPropertyCache.prototypeIsKnownObject;
    canBeMegamorphic = unlinkedPropertyCache.canBeMegamorphic;

    if (unlinkedPropertyCache.canBeMegamorphic)
        bufferingCountdown.storeRelaxed(1);

    usedRegisters = RegisterSet::stubUnavailableRegisters().toScalarRegisterSet();

    m_slowOperation = slowOperationFromUnlinkedPropertyInlineCache(unlinkedPropertyCache);
    initializePredefinedRegisters();
}
#endif

void HandlerPropertyInlineCache::initializeHandlerForOptimizingJIT(CodeBlock* codeBlock)
{
    ASSERT(!isCompilationThread());
    ASSERT(JSC::JITCode::isOptimizingJIT(codeBlock->jitType()));
    // The FTL lowering / JITInlineCacheGenerator already populated accessType,
    // identifier, codeOrigin, callSiteIndex, registers (the Baseline data-IC
    // conventions), doneLocation and slowPathStartLocation; m_slowOperation was
    // assigned when the site's slow path was emitted. All that is left is the
    // initial handler: the shared slow-path handler, which calls m_slowOperation
    // with the Baseline data-IC argument registers.
    ASSERT(!!m_slowOperation);
    // AB18-G: LINK-time derivation on a provably-live codeBlock (see
    // initializeFromDFGUnlinkedPropertyInlineCache above).
    VM& vm = codeBlock->vm();
    initializeWithUnitHandler(vm, codeBlock, InlineCacheCompiler::generateSlowPathHandler(vm, accessType));
}

void HandlerPropertyInlineCache::setInlinedHandler(CodeBlock* codeBlock, Ref<InlineCacheHandler>&& handler)
{
    ASSERT(!m_inlinedHandler);
    VM& vm = codeBlock->vm();
    m_inlinedHandler = WTF::move(handler);
    m_inlinedHandler->addOwner(codeBlock);
    // F1 (SPEC-jit section 5.1): the handler payload (and its stub code) is
    // fully initialized before the inlined fast-path fields below make it
    // observable to JIT'd code. Single-word atomicity of the
    // {byIdSelfOffset, m_inlineAccessBaseStructureID} pair is section 4.2
    // (Task 4); this fence only orders payload init before field publish.
    WTF::storeStoreFence();
    switch (m_inlinedHandler->cacheType()) {
    case CacheType::GetByIdSelf: {
        // SPEC-jit section 4.2: holder-free self-access only; the inlined
        // fast-path state must be exactly the packable {offset, structureID}
        // pair under useJSThreads.
        ASSERT(!Options::useJSThreads() || !m_inlinedHandler->holder());
        setInlineAccessSelfState(vm, codeBlock, m_inlinedHandler->structureID().decode(), m_inlinedHandler->offset());
        break;
    }
    case CacheType::GetByIdPrototype: {
        // SPEC-jit section 4.2: holder-bearing inlined forms are disabled
        // flag-on (m_inlineHolder cannot pack into the 64-bit unit; a reader
        // could pair a fresh {offset, structureID} word with a stale holder).
        // prependHandler never routes them here under useJSThreads; such
        // accesses dispatch through the handler chain instead (F2).
        RELEASE_ASSERT(!Options::useJSThreads());
        m_inlineAccessBaseStructureID.set(vm, codeBlock, m_inlinedHandler->structureID().decode());
        byIdSelfOffset = m_inlinedHandler->offset();
        m_inlineHolder = m_inlinedHandler->holder();
        break;
    }
    case CacheType::PutByIdReplace: {
        ASSERT(!Options::useJSThreads() || !m_inlinedHandler->holder());
        setInlineAccessSelfState(vm, codeBlock, m_inlinedHandler->structureID().decode(), m_inlinedHandler->offset());
        break;
    }
    case CacheType::InByIdSelf: {
        ASSERT(!Options::useJSThreads() || !m_inlinedHandler->holder());
        setInlineAccessSelfState(vm, codeBlock, m_inlinedHandler->structureID().decode(), m_inlinedHandler->offset());
        break;
    }
    case CacheType::ArrayLength:
    case CacheType::StringLength:
    case CacheType::Unset:
    case CacheType::Stub:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
}

void HandlerPropertyInlineCache::clearInlinedHandler(CodeBlock* codeBlock)
{
    m_inlinedHandler->removeOwner(codeBlock);
    m_inlinedHandler = nullptr;
    clearInlineAccessSelfState();
}

// Review round 2 (R2-1): replace the published chain head with ONE raw
// pointer store and no null window. The flag-on dispatch fast path emitted
// for every Baseline/DFG/FTL handler IC is
//   loadPtr [propertyCache + offsetOfHandler] -> handlerGPR
//   call    [handlerGPR + offsetOfCallTarget]
// with NO null check (JITInlineCacheGenerator.cpp; the invariant is that a
// slow-path handler is always installed) — and WTF::RefPtr move
// construction/assignment nulls the SOURCE slot (RefPtr.h: m_ptr(o.leakRef())),
// so any `WTF::move(m_handler)` before the publishing store opens a window in
// which a racing JIT'd reader calls through address null. This is the same
// no-null-window idiom as CallLinkInfo::setStub. Contract: the caller must
// already hold (a copy of) any displaced reference it wants to keep alive
// (e.g. for RetiredJITArtifacts); we deref the slot's own old reference only
// AFTER the new head is published, so a chain kept alive through the new
// node's m_next (prependHandler) or a displacedHead copy survives.
static void publishHandlerChainHead(RefPtr<InlineCacheHandler>& headSlot, Ref<InlineCacheHandler>&& newHead)
{
    InlineCacheHandler** rawSlot = std::bit_cast<InlineCacheHandler**>(&headSlot);
    InlineCacheHandler* oldHead = *rawSlot;
    InlineCacheHandler* incoming = &newHead.leakRef();
#if TSAN_ENABLED
    // TSAN §4.4 retired-artifact audit (TSAN-RESULTS residual 1, closed
    // 2026-06-09): publish a TSAN-visible release edge for the new head.
    // JIT'd readers consume the head with an address dependency (F2) and
    // hand the node/CallLinkInfo pointers to the C++ slow paths baked into
    // register state, so TSAN never sees the real ordering; without these
    // annotations it pairs the ctor's plain init stores (and the allocator's
    // block writes) on this thread against the reader thread's accesses in
    // ICSlowPathCallFrameTracer / ownerForSlowPath, on memory the audit
    // proved live (every flag-on dealloc path of a published handler routes
    // through RetiredJITArtifacts::retireHandlerChain, which flag-on never
    // frees — epochCoversEveryJSThread). AFTER sides:
    // ICSlowPathCallFrameTracer (JITOperations.cpp, keyed on the
    // PropertyInlineCache) and CallLinkInfo::ownerForSlowPath (keyed on the
    // embedded CallLinkInfo). Annotating only the NEW node is sufficient:
    // a prepended head reaches the old chain through m_next, and each old
    // node was annotated at its own publication. No-op outside TSAN.
    TSAN_ANNOTATE_HAPPENS_BEFORE(incoming);
    if (auto* withJSCall = dynamicDowncast<InlineCacheHandlerWithJSCall>(*incoming))
        TSAN_ANNOTATE_HAPPENS_BEFORE(std::bit_cast<uint8_t*>(withJSCall) + InlineCacheHandlerWithJSCall::offsetOfCallLinkInfo());
#endif
    // F1/I5: every store initializing the new head (m_next included) must be
    // visible before the publishing store makes the node reachable from JIT'd
    // code. Readers order their loads with an address dependency through the
    // head pointer (F2).
    WTF::storeStoreFence();
    *rawSlot = incoming;
    if (oldHead)
        oldHead->deref();
}

void PropertyInlineCache::initializeWithUnitHandler(VM& vm, CodeBlock* codeBlock, Ref<InlineCacheHandler>&& handler)
{
    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        // SPEC-jit section 5.1/section 4.4: under useJSThreads a JIT'd reader on
        // another thread can be inside its safepoint-free handler window (G2)
        // holding pointers into the displaced state, so nothing displaced here
        // may be freed inline; keep a Ref across the swap and route it through
        // the heap's safepoint epoch afterwards.
        RefPtr<InlineCacheHandler> displacedInlinedHandler;
        if (handlerIC->m_inlinedHandler) {
            if (Options::useJSThreads()) [[unlikely]]
                displacedInlinedHandler = handlerIC->m_inlinedHandler;
            handlerIC->clearInlinedHandler(codeBlock);
        }
        ASSERT(!handlerIC->m_inlinedHandler);
        if (m_handler)
            m_handler->removeOwner(codeBlock);
        if (Options::useJSThreads()) [[unlikely]] {
            // R2-1: COPY (never move out of) m_handler. `WTF::move(m_handler)`
            // would null the published slot before the publishing store, and
            // racing JIT'd readers call through it with no null check; see
            // publishHandlerChainHead above. The copy keeps the displaced
            // chain alive across the publish; it is then routed through the
            // safepoint epoch (section 4.4), never freed inline.
            RefPtr<InlineCacheHandler> displacedHead = m_handler;
            publishHandlerChainHead(m_handler, WTF::move(handler));
            m_handler->addOwner(codeBlock);
            // R4-2: pass the VM; RetiredJITArtifacts resolves the epoch heap
            // (the client's SERVER under useSharedGCHeap) internally.
            // AB18-G: the VM& is the caller's (AB18-E rule) — no
            // codeBlock->vm() derivation on this retire path.
            RetiredJITArtifacts::retireHandlerChain(vm, WTF::move(displacedHead), DisarmClearingWatchpoints::Yes);
            RetiredJITArtifacts::retireHandlerChain(vm, WTF::move(displacedInlinedHandler), DisarmClearingWatchpoints::Yes);
        } else {
            // F1/I5: the new handler's payload (and anything reachable through
            // it) must be visible before the publishing store makes it
            // JIT-reachable. (Flag-off there is exactly one mutator, so the
            // transient null in RefPtr move-assignment is unobservable.)
            WTF::storeStoreFence();
            m_handler = WTF::move(handler);
            m_handler->addOwner(codeBlock);
        }
    } else {
        // Repatching IC (FTL, flag-off only; I3). Fence anyway per F1: the
        // handler becomes reachable from patched code.
        WTF::storeStoreFence();
        m_handler = WTF::move(handler);
    }
}

void PropertyInlineCache::prependHandler(VM& vm, CodeBlock* codeBlock, Ref<InlineCacheHandler>&& handler, bool isMegamorphic)
{
    auto& handlerIC = downcast<HandlerPropertyInlineCache>(*this);
    if (isMegamorphic) {
        initializeWithUnitHandler(vm, codeBlock, WTF::move(handler));
        return;
    }

    if (!handlerIC.m_inlinedHandler) {
        if (preconfiguredCacheType != CacheType::Unset && preconfiguredCacheType == handler->cacheType()) {
            // SPEC-jit section 4.2 (Task 4): holder-bearing inlined forms are
            // disabled under useJSThreads - GetByIdPrototype needs
            // m_inlineHolder, which cannot pack into the single-load
            // {offset, structureID} unit. Fall through to the chain prepend
            // below; the handler still dispatches correctly via the chain (F2),
            // we only lose the call-site inlined fast path for this shape.
            bool holderBearing = handler->cacheType() == CacheType::GetByIdPrototype;
            if (!Options::useJSThreads() || !holderBearing) [[likely]] {
                handlerIC.setInlinedHandler(codeBlock, WTF::move(handler));
                return;
            }
        }
    }

    // R2-1: COPY the current head into the new node's m_next (set exactly once
    // here and immutable until reset, section 4.1). `setNext(WTF::move(m_handler))`
    // would null the published m_handler slot until the publish below — and
    // this path runs under CodeBlock::m_lock from operation*Optimize slow
    // paths while other mutators execute the same shared code lock-free, so a
    // racing reader in that window would call through a null handler. The
    // copy keeps the old head alive; publishHandlerChainHead (which carries
    // the F1/I5 fence) drops the slot's own reference only after the new head
    // is visible, so readers observe either the old head or the new head,
    // both fully initialized.
    handler->setNext(RefPtr<InlineCacheHandler> { m_handler });
    publishHandlerChainHead(m_handler, WTF::move(handler));
    m_handler->addOwner(codeBlock);
}

void PropertyInlineCache::rewireStubAsJumpInAccess(VM& vm, CodeBlock* codeBlock, Ref<InlineCacheHandler>&& handler)
{
    ASSERT(!isHandlerIC());
    // Repatching ICs exist only in the FTL, and with useHandlerICInFTL the FTL
    // allocates handler ICs exclusively (FTL::State::addPropertyInlineCache), so
    // this in-place code rewrite is unreachable (SPEC-jit §5.2: FTL's
    // rewireStubAsJumpInAccess is dropped when handler ICs are complete).
    RELEASE_ASSERT(!Options::useHandlerICInFTL());
    // SPEC-jit I2: replaceWithJump patches reachable code; with useJSThreads on
    // this site is unreachable anyway (I3 forbids RepatchingPropertyInlineCache),
    // but the world-stopped discipline is asserted at the patching site itself.
    JSThreadsSafepoint::assertPatchingIsSafe(vm);
    CodeLocationLabel label { handler->callTarget() };
    initializeWithUnitHandler(vm, codeBlock, WTF::move(handler));
    CCallHelpers::replaceWithJump(downcast<RepatchingPropertyInlineCache>(*this).startLocation.retagged<JSInternalPtrTag>(), label);
}

void PropertyInlineCache::resetStubAsJumpInAccess(VM& vm, CodeBlock* codeBlock)
{
    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        // SPEC-jit section 4.1/section 5.1/I9: install a fresh slow-path-only
        // head; the old chain is never freed inline. Under useJSThreads a
        // JIT'd reader on another thread may be dispatching through the old
        // nodes inside its safepoint-free window (G2/I16), so the displaced
        // chain (and any displaced inlined unit handler) is handed to
        // RetiredJITArtifacts: node data is freed only after every mutator
        // crosses a safepoint (section 4.4); the machine code held via each
        // node's Ref<GCAwareJITStubRoutine> drops into the jettison machinery
        // and waits for R2's conservative scan (I7).
        RefPtr<InlineCacheHandler> displacedInlinedHandler;
        if (handlerIC->m_inlinedHandler) {
            if (Options::useJSThreads()) [[unlikely]]
                displacedInlinedHandler = handlerIC->m_inlinedHandler;
            handlerIC->clearInlinedHandler(codeBlock);
        }
        auto* cursor = m_handler.get();
        while (cursor) {
            cursor->removeOwner(codeBlock);
            cursor = cursor->next();
        }
        // removeOwner() stays (section 4.1): ownership bookkeeping is dropped
        // now; only the *memory* outlives the reset, via the epoch.
        Ref<InlineCacheHandler> slowPathHandler = InlineCacheCompiler::generateSlowPathHandler(vm, accessType);
        if (Options::useJSThreads()) [[unlikely]] {
            // R2-1: COPY, never move out of, m_handler (no null window for
            // racing JIT'd readers; see publishHandlerChainHead). The callers
            // of this reset run world-stopped today (watchpoint fires / GC
            // resets), so this site was latent, but it must not share the
            // broken move-from-member idiom.
            RefPtr<InlineCacheHandler> displacedHead = m_handler;
            publishHandlerChainHead(m_handler, WTF::move(slowPathHandler));
            // R4-2: pass the VM; RetiredJITArtifacts resolves the epoch heap.
            // AB18-G: the VM& is the caller's (AB18-E rule) — no
            // codeBlock->vm() derivation on this retire path.
            RetiredJITArtifacts::retireHandlerChain(vm, WTF::move(displacedHead), DisarmClearingWatchpoints::Yes);
            RetiredJITArtifacts::retireHandlerChain(vm, WTF::move(displacedInlinedHandler), DisarmClearingWatchpoints::Yes);
            return;
        }
        WTF::storeStoreFence(); // F1/I5
        m_handler = WTF::move(slowPathHandler);
        return;
    }

    rewireStubAsJumpInAccess(vm, codeBlock, InlineCacheHandler::createNonHandlerSlowPath(slowPathStartLocation));
}

Vector<AccessCase*, 16> PropertyInlineCache::listedAccessCases(const AbstractLocker&) const
{
    Vector<AccessCase*, 16> cases;
    if (auto* repatchingIC = dynamicDowncast<RepatchingPropertyInlineCache>(*this)) {
        if (repatchingIC->m_stub) {
            for (unsigned i = 0; i < repatchingIC->m_stub->size(); ++i)
                cases.append(&repatchingIC->m_stub->at(i));
            return cases;
        }
    }

    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*this)) {
        if (handlerIC->m_inlinedHandler) {
            if (auto* access = handlerIC->m_inlinedHandler->accessCase())
                cases.append(access);
        }
    }

    if (auto* cursor = m_handler.get()) {
        while (cursor) {
            if (auto* access = cursor->accessCase())
                cases.append(access);
            cursor = cursor->next();
        }
    }

    return cases;
}

#if ASSERT_ENABLED
void PropertyInlineCache::checkConsistency()
{
    switch (accessType) {
    case AccessType::GetByIdWithThis:
        // We currently use a union for both "thisGPR" and "propertyGPR". If this were
        // not the case, we'd need to take one of them out of the union.
        RELEASE_ASSERT(hasConstantIdentifier(accessType));
        break;
    case AccessType::GetByValWithThis:
    default:
        break;
    }
}
#endif // ASSERT_ENABLED

#endif // ENABLE(JIT)

} // namespace JSC
