/*
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
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
#include "JITInlineCacheGenerator.h"

#if ENABLE(JIT)

#include "BaselineJITRegisters.h"
#include "CCallHelpers.h"
#include "CacheableIdentifierInlines.h"
#include "CodeBlock.h"
#include "DFGJITCompiler.h"
#include "ICStats.h"
#include "InlineAccess.h"
#include "JITInlines.h"
#include "LinkBuffer.h"
#include "MacroAssemblerPrinter.h"
#include "PropertyInlineCache.h"

namespace JSC {

static void emitICStatsChainFlushProbe(CCallHelpers& jit, GPRReg propertyCacheGPR)
{
    if constexpr (ICStatsInternal::traceHandlerChains) {
        jit.probeDebug([=](Probe::Context& context) {
            auto* propertyCache = context.gpr<PropertyInlineCache*>(propertyCacheGPR);
            unsigned chainLength = 0;
            for (auto* handler = propertyCache->firstHandler(); handler; handler = handler->next())
                chainLength++;
            ICStats::singleton().startNewChain(chainLength);
        });
    }
}

JITInlineCacheGenerator::JITInlineCacheGenerator(CodeBlock*, CompileTimePropertyInlineCache propertyCache, JITType, CodeOrigin, AccessType accessType)
    : m_accessType(accessType)
{
    WTF::visit(WTF::makeVisitor(
        [&](PropertyInlineCache* propertyCache) {
            m_propertyCache = propertyCache;
        },
        [&](BaselineUnlinkedPropertyInlineCache* propertyCache) {
            m_unlinkedPropertyCache = propertyCache;
        }
#if ENABLE(DFG_JIT)
        ,
        [&](DFG::UnlinkedPropertyInlineCache* propertyCache) {
            m_unlinkedPropertyCache = propertyCache;
        }
#endif
        ), propertyCache);
}

void JITInlineCacheGenerator::finalize(
    LinkBuffer& fastPath, LinkBuffer& slowPath, CodeLocationLabel<JITStubRoutinePtrTag> start)
{
    ASSERT(m_propertyCache);
    if (auto* handlerIC = dynamicDowncast<HandlerPropertyInlineCache>(*m_propertyCache)) {
        // FTL handler-IC site (useHandlerICInFTL): there is no inline slab and no
        // patchable slow-path call; record where the IC continues and where its
        // out-of-line slow path (the late path emitted by the FTL lowering)
        // begins. The slow-path handler thunk dispatches to slowPathStartLocation
        // only when re-entering the IC's miss path is required; the initial
        // handler itself is installed on the main thread at plan finalization
        // (HandlerPropertyInlineCache::initializeHandlerForOptimizingJIT).
        UNUSED_PARAM(start);
        handlerIC->doneLocation = fastPath.locationOf<JSInternalPtrTag>(m_done);
        handlerIC->slowPathStartLocation = slowPath.locationOf<JITStubRoutinePtrTag>(m_slowPathBegin);
        return;
    }
    auto& repatchingIC = downcast<RepatchingPropertyInlineCache>(*m_propertyCache);
    repatchingIC.startLocation = start;
    m_propertyCache->doneLocation = fastPath.locationOf<JSInternalPtrTag>(m_done);
    repatchingIC.m_slowPathCallLocation = slowPath.locationOf<JSInternalPtrTag>(m_slowPathCall);
    m_propertyCache->slowPathStartLocation = slowPath.locationOf<JITStubRoutinePtrTag>(m_slowPathBegin);
}

void JITInlineCacheGenerator::generateDataICFastPath(CCallHelpers& jit, GPRReg propertyCacheGPR)
{
    m_start = jit.label();
    emitICStatsChainFlushProbe(jit, propertyCacheGPR);
    jit.loadPtr(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfHandler()), GPRInfo::handlerGPR);
    jit.call(CCallHelpers::Address(GPRInfo::handlerGPR, InlineCacheHandler::offsetOfCallTarget()), JITStubRoutinePtrTag);
    m_done = jit.label();
}

JITByIdGenerator::JITByIdGenerator(
    CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, AccessType accessType,
    JSValueRegs base, JSValueRegs value)
    : JITInlineCacheGenerator(codeBlock, propertyCache, jitType, codeOrigin, accessType)
    , m_base(base)
    , m_value(value)
{
}

void JITByIdGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    JITInlineCacheGenerator::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

void JITByIdGenerator::generateFastCommon(CCallHelpers& jit, size_t inlineICSize)
{
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    jit.padBeforePatch(); // On ARMv7, this ensures that the patchable jump does not make the inline code too large.
    m_start = jit.label();
    size_t startSize = jit.m_assembler.buffer().codeSize();
    m_slowPathJump = jit.jump();
    size_t jumpSize = jit.m_assembler.buffer().codeSize() - startSize;
    size_t nopsToEmitInBytes = inlineICSize - jumpSize;
    jit.emitNops(nopsToEmitInBytes);
    ASSERT(jit.m_assembler.buffer().codeSize() - startSize == inlineICSize);
    m_done = jit.label();
}

JITGetByIdGenerator::JITGetByIdGenerator(
    CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSite, const RegisterSet& usedRegisters,
    CacheableIdentifier propertyName, JSValueRegs base, JSValueRegs value, GPRReg propertyCacheGPR, AccessType accessType, CacheType cacheType)
    : JITByIdGenerator(codeBlock, propertyCache, jitType, codeOrigin, accessType, base, value)
    , m_isLengthAccess(codeBlock && propertyName.uid() == codeBlock->vm().propertyNames->length.impl())
    , m_cacheType(cacheType)
{
    RELEASE_ASSERT(base.payloadGPR() != value.tagGPR());
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, cacheType, codeOrigin, callSite, usedRegisters, propertyName, base, value, propertyCacheGPR);
    }, propertyCache);
}

#if USE(JSVALUE64) && CPU(LITTLE_ENDIAN)
// SPEC-jit section 4.2 (Task 4), flag-on single-load reader of the inlined
// fast-path unit. Emits:
//
//     load64  [propertyCache + packedSelfWord], wordGPR   // {offset, id} - ONE load
//     load32  [base + structureID], scratch1GPR
//     lshift64 scratch1GPR, #32
//     xor64   wordGPR, scratch1GPR        // high 32 = id ^ cellID; low 32 = offset
//     move    scratch1GPR, wordGPR
//     urshift64 wordGPR, #32
//     branchTest64 NonZero, wordGPR -> miss
//     // hit: scratch1GPR == zero-extended offset from the SAME 64-bit load
//
// Both halves come from one relaxed 64-bit load, so a concurrent single-word
// publish/invalidate (PropertyInlineCache::setInlineAccessSelfState/
// clearInlineAccessSelfState) can never be observed as a valid structure id
// paired with a mismatched offset (I6). The flag-off compare-then-reload form
// below is unsound under concurrency on ARM64: a compare/branch does not order
// the subsequent offset load (F2). wordGPR is clobbered; on a hit, scratch1GPR
// holds the offset. Flag-on is 64-bit little-endian only (SPEC-jit D8): the id
// is the high half of the packed word.
static CCallHelpers::Jump emitPackedInlineAccessCheckThreaded(CCallHelpers& jit, GPRReg propertyCacheGPR, GPRReg baseGPR, GPRReg scratch1GPR, GPRReg wordGPR)
{
    ASSERT(Options::useJSThreads());
    ASSERT(wordGPR != InvalidGPRReg && wordGPR != scratch1GPR && wordGPR != baseGPR && wordGPR != propertyCacheGPR);
    jit.load64(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfPackedInlineAccessSelfWord()), wordGPR);
    jit.load32(CCallHelpers::Address(baseGPR, JSCell::structureIDOffset()), scratch1GPR);
    jit.lshift64(CCallHelpers::TrustedImm32(32), scratch1GPR);
    jit.xor64(wordGPR, scratch1GPR);
    jit.move(scratch1GPR, wordGPR);
    jit.urshift64(CCallHelpers::TrustedImm32(32), wordGPR);
    return jit.branchTest64(CCallHelpers::NonZero, wordGPR, wordGPR);
}
#endif // USE(JSVALUE64) && CPU(LITTLE_ENDIAN)

static void generateGetByIdInlineAccessBaselineDataIC(CCallHelpers& jit, GPRReg propertyCacheGPR, JSValueRegs baseJSR, GPRReg scratch1GPR, GPRReg scratch2GPR, JSValueRegs resultJSR, CacheType cacheType)
{
    CCallHelpers::JumpList slowCases;
    CCallHelpers::JumpList doneCases;
    UNUSED_PARAM(scratch2GPR);

    switch (cacheType) {
    case CacheType::GetByIdSelf: {
#if USE(JSVALUE64) && CPU(LITTLE_ENDIAN)
        if (Options::useJSThreads()) [[unlikely]] {
            // Single 64-bit load of {byIdSelfOffset, structureID} (section 4.2/I6).
            // scratch2GPR (never resultJSR: it may alias baseJSR) holds the word.
            slowCases.append(emitPackedInlineAccessCheckThreaded(jit, propertyCacheGPR, baseJSR.payloadGPR(), scratch1GPR, scratch2GPR));
            // SPEC-jit section 5.5 (Task 8): out-of-line loads go through the
            // butterfly READ choke point (scratch2GPR = storage scratch;
            // resultJSR may alias baseJSR). Predicate failures dispatch to
            // the handler chain like any other miss.
            // R7/F7 (review round 1): on the hit path scratch1GPR holds
            // (cellSID<<32 ^ packedWord) >> 0 with a provably-zero high half,
            // i.e. the zero-extended offset, and it is DATA-DEPENDENT on the
            // cell structureID load inside emitPackedInlineAccessCheckThreaded.
            // Passing it as structureIDGPR makes the ARM64 butterfly load
            // address-dependent on the structureID load (the OM pairs this
            // with its butterfly-before-structureID store fence), closing the
            // fresh-{sid,offset}-word + stale-butterfly OOB window this site
            // previously had (it passed InvalidGPRReg).
            jit.loadProperty(baseJSR.payloadGPR(), scratch1GPR, resultJSR, scratch2GPR, slowCases, scratch1GPR);
            doneCases.append(jit.jump());
            break;
        }
#endif
        jit.load32(CCallHelpers::Address(baseJSR.payloadGPR(), JSCell::structureIDOffset()), scratch1GPR);
        slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch1GPR, CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfInlineAccessBaseStructureID())));
        jit.load32(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfByIdSelfOffset()), scratch1GPR);
        jit.loadProperty(baseJSR.payloadGPR(), scratch1GPR, resultJSR);
        doneCases.append(jit.jump());
        break;
    }
    case CacheType::GetByIdPrototype: {
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 4.2 (Task 4): holder-bearing inlined forms are
            // disabled flag-on - m_inlineHolder cannot pack into the 64-bit
            // unit, and a separate holder load could pair a fresh
            // {offset, structureID} word with a stale holder.
            // HandlerPropertyInlineCache::setInlinedHandler never installs
            // this shape under the flag, so emit no inline fast path here:
            // every access dispatches through the handler chain below (F2).
            break;
        }
        jit.load32(CCallHelpers::Address(baseJSR.payloadGPR(), JSCell::structureIDOffset()), scratch1GPR);
        slowCases.append(jit.branch32(CCallHelpers::NotEqual, scratch1GPR, CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfInlineAccessBaseStructureID())));
        jit.load32(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfByIdSelfOffset()), scratch1GPR);
        jit.loadPtr(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfInlineHolder()), resultJSR.payloadGPR());
        jit.loadProperty(resultJSR.payloadGPR(), scratch1GPR, resultJSR);
        doneCases.append(jit.jump());
        break;
    }
    case CacheType::ArrayLength: {
        if (Options::useJSThreads()) [[unlikely]] {
            // SPEC-jit section 5.5 (Task 8): apply the READ choke point to
            // the length load (ArrayLength is reachable for AS arrays, so the
            // conservative SW=1 => handler-dispatch form is used).
            jit.load8(CCallHelpers::Address(baseJSR.payloadGPR(), JSCell::indexingTypeAndMiscOffset()), scratch1GPR);
            slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch1GPR, CCallHelpers::TrustedImm32(IsArray)));
            slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch1GPR, CCallHelpers::TrustedImm32(IndexingShapeMask)));
            slowCases.append(jit.loadButterflyForRead(baseJSR.payloadGPR(), scratch1GPR, CCallHelpers::ConcurrentButterflyShape::MaybeArrayStorage));
            jit.load32(CCallHelpers::Address(scratch1GPR, ArrayStorage::lengthOffset()), scratch1GPR);
            slowCases.append(jit.branch32(CCallHelpers::LessThan, scratch1GPR, CCallHelpers::TrustedImm32(0)));
            jit.boxInt32(scratch1GPR, resultJSR);
            doneCases.append(jit.jump());
            break;
        }
        jit.load8(CCallHelpers::Address(baseJSR.payloadGPR(), JSCell::indexingTypeAndMiscOffset()), scratch1GPR);
        slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch1GPR, CCallHelpers::TrustedImm32(IsArray)));
        slowCases.append(jit.branchTest32(CCallHelpers::Zero, scratch1GPR, CCallHelpers::TrustedImm32(IndexingShapeMask)));
        jit.loadPtr(CCallHelpers::Address(baseJSR.payloadGPR(), JSObject::butterflyOffset()), scratch1GPR);
        jit.load32(CCallHelpers::Address(scratch1GPR, ArrayStorage::lengthOffset()), scratch1GPR);
        slowCases.append(jit.branch32(CCallHelpers::LessThan, scratch1GPR, CCallHelpers::TrustedImm32(0)));
        jit.boxInt32(scratch1GPR, resultJSR);
        doneCases.append(jit.jump());
        break;
    }
    default:
        break;
    }

    slowCases.link(&jit);
    emitICStatsChainFlushProbe(jit, propertyCacheGPR);
    jit.loadPtr(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfHandler()), GPRInfo::handlerGPR);
    jit.call(CCallHelpers::Address(GPRInfo::handlerGPR, InlineCacheHandler::offsetOfCallTarget()), JITStubRoutinePtrTag);
    doneCases.link(&jit);
}

void JITGetByIdGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    generateFastCommon(jit, m_isLengthAccess ? InlineAccess::sizeForLengthAccess() : InlineAccess::sizeForPropertyAccess());
}

void JITGetByIdGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    m_start = jit.label();

    using BaselineJITRegisters::GetById::baseJSR;
    using BaselineJITRegisters::GetById::resultJSR;
    using BaselineJITRegisters::GetById::propertyCacheGPR;
    using BaselineJITRegisters::GetById::scratch1GPR;
    using BaselineJITRegisters::GetById::scratch2GPR;

    generateGetByIdInlineAccessBaselineDataIC(jit, propertyCacheGPR, baseJSR, scratch1GPR, scratch2GPR, resultJSR, m_cacheType);

    m_done = jit.label();
}

JITGetByIdWithThisGenerator::JITGetByIdWithThisGenerator(
    CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSite, const RegisterSet& usedRegisters,
    CacheableIdentifier propertyName, JSValueRegs value, JSValueRegs base, JSValueRegs thisRegs, GPRReg propertyCacheGPR)
    : JITByIdGenerator(codeBlock, propertyCache, jitType, codeOrigin, AccessType::GetByIdWithThis, base, value)
{
    RELEASE_ASSERT(thisRegs.payloadGPR() != thisRegs.tagGPR());
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, AccessType::GetByIdWithThis, CacheType::GetByIdSelf, codeOrigin, callSite, usedRegisters, propertyName, value, base, thisRegs, propertyCacheGPR);
    }, propertyCache);
}

void JITGetByIdWithThisGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    generateFastCommon(jit, InlineAccess::sizeForPropertyAccess());
}

void JITGetByIdWithThisGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    m_start = jit.label();

    using BaselineJITRegisters::GetByIdWithThis::baseJSR;
    using BaselineJITRegisters::GetByIdWithThis::resultJSR;
    using BaselineJITRegisters::GetByIdWithThis::propertyCacheGPR;
    using BaselineJITRegisters::GetByIdWithThis::scratch1GPR;
    using BaselineJITRegisters::GetByIdWithThis::scratch2GPR;

    generateGetByIdInlineAccessBaselineDataIC(jit, propertyCacheGPR, baseJSR, scratch1GPR, scratch2GPR, resultJSR, CacheType::GetByIdSelf);

    m_done = jit.label();
}

JITPutByIdGenerator::JITPutByIdGenerator(
    CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSite, const RegisterSet& usedRegisters, CacheableIdentifier propertyName,
    JSValueRegs base, JSValueRegs value, GPRReg propertyCacheGPR, GPRReg scratch,
    AccessType accessType)
        : JITByIdGenerator(codeBlock, propertyCache, jitType, codeOrigin, accessType, base, value)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::PutByIdReplace, codeOrigin, callSite, usedRegisters, propertyName, base, value, propertyCacheGPR, scratch);
    }, propertyCache);
}

// scratch3GPR: a true scratch distinct from base/value/propertyCache/scratch1,
// used only by the useJSThreads single-load form (section 4.2); pass
// InvalidGPRReg when unavailable (flag-off / JSVALUE32_64).
static void generatePutByIdInlineAccessBaselineDataIC(CCallHelpers& jit, GPRReg propertyCacheGPR, JSValueRegs baseJSR, JSValueRegs valueJSR, GPRReg scratch1GPR, GPRReg scratch2GPR, GPRReg scratch3GPR)
{
    UNUSED_PARAM(scratch3GPR);
#if USE(JSVALUE64) && CPU(LITTLE_ENDIAN)
    if (Options::useJSThreads()) [[unlikely]] {
        // Single 64-bit load of {byIdSelfOffset, structureID} (section 4.2/I6).
        // scratch2GPR may alias baseJSR (Baseline passes base as the
        // storeProperty scratch), so the packed word lives in scratch3GPR.
        auto doNotInlineAccess = emitPackedInlineAccessCheckThreaded(jit, propertyCacheGPR, baseJSR.payloadGPR(), scratch1GPR, scratch3GPR);
        // SPEC-jit section 5.5 (Task 8): inline-offset stores are
        // cell-internal and need no predicate; out-of-line stores need the
        // WRITE choke point, but this register file leaves no GPR pair for
        // {storage, TID tag} with baseJSR preserved on the miss path - so
        // out-of-line offsets dispatch to the putByIdReplaceHandler (which
        // carries the full predicate). Inventory: INTEGRATE-jit.md Task 8.
        CCallHelpers::JumpList notInline;
        notInline.append(jit.branch32(CCallHelpers::GreaterThanOrEqual, scratch1GPR, CCallHelpers::TrustedImm32(firstOutOfLineOffset)));
        jit.storeProperty(valueJSR, baseJSR.payloadGPR(), scratch1GPR, scratch2GPR);
        auto done = jit.jump();
        doNotInlineAccess.link(&jit);
        notInline.link(&jit);
        emitICStatsChainFlushProbe(jit, propertyCacheGPR);
        jit.loadPtr(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfHandler()), GPRInfo::handlerGPR);
        jit.call(CCallHelpers::Address(GPRInfo::handlerGPR, InlineCacheHandler::offsetOfCallTarget()), JITStubRoutinePtrTag);
        done.link(&jit);
        return;
    }
#endif
    jit.load32(CCallHelpers::Address(baseJSR.payloadGPR(), JSCell::structureIDOffset()), scratch1GPR);
    auto doNotInlineAccess = jit.branch32(CCallHelpers::NotEqual, scratch1GPR, CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfInlineAccessBaseStructureID()));
    jit.load32(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfByIdSelfOffset()), scratch1GPR);
    // The second scratch can be the same to baseJSR.
    jit.storeProperty(valueJSR, baseJSR.payloadGPR(), scratch1GPR, scratch2GPR);
    auto done = jit.jump();
    doNotInlineAccess.link(&jit);
    emitICStatsChainFlushProbe(jit, propertyCacheGPR);
    jit.loadPtr(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfHandler()), GPRInfo::handlerGPR);
    jit.call(CCallHelpers::Address(GPRInfo::handlerGPR, InlineCacheHandler::offsetOfCallTarget()), JITStubRoutinePtrTag);
    done.link(&jit);
}

void JITPutByIdGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::PutById::baseJSR;
    using BaselineJITRegisters::PutById::valueJSR;
    using BaselineJITRegisters::PutById::propertyCacheGPR;
    using BaselineJITRegisters::PutById::scratch1GPR;

#if USE(JSVALUE64)
    // Dead at the IC site: handler stubs may clobber it (BaselineJITRegisters
    // "Required for HandlerIC" assert), so no live value can be in it here.
    constexpr GPRReg scratch3GPR = BaselineJITRegisters::PutById::scratch2GPR;
#else
    constexpr GPRReg scratch3GPR = InvalidGPRReg;
#endif

    m_start = jit.label();
    // The second scratch can be the same to baseJSR. In Baseline JIT, we clobber the baseJSR to save registers.
    generatePutByIdInlineAccessBaselineDataIC(jit, propertyCacheGPR, baseJSR, valueJSR, scratch1GPR, baseJSR.payloadGPR(), scratch3GPR);
    m_done = jit.label();
}

void JITPutByIdGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    generateFastCommon(jit, InlineAccess::sizeForPropertyReplace());
}

JITDelByValGenerator::JITDelByValGenerator(CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex, AccessType accessType, const RegisterSet& usedRegisters, JSValueRegs base, JSValueRegs property, JSValueRegs result, GPRReg propertyCacheGPR)
    : Base(codeBlock, propertyCache, jitType, codeOrigin, accessType)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, base, property, result, propertyCacheGPR);
    }, propertyCache);
}

void JITDelByValGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

void JITDelByValGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::DelByVal::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}

void JITDelByValGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

JITDelByIdGenerator::JITDelByIdGenerator(CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex, AccessType accessType, const RegisterSet& usedRegisters, CacheableIdentifier propertyName, JSValueRegs base, JSValueRegs result, GPRReg propertyCacheGPR)
    : Base(codeBlock, propertyCache, jitType, codeOrigin, accessType)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, propertyName, base, result, propertyCacheGPR);
    }, propertyCache);
}

void JITDelByIdGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

void JITDelByIdGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::DelById::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}

void JITDelByIdGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

JITInByValGenerator::JITInByValGenerator(CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex, AccessType accessType, const RegisterSet& usedRegisters, JSValueRegs base, JSValueRegs property, JSValueRegs result, GPRReg arrayProfileGPR, GPRReg propertyCacheGPR)
    : Base(codeBlock, propertyCache, jitType, codeOrigin, accessType)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, base, property, result, arrayProfileGPR, propertyCacheGPR);
    }, propertyCache);
}

void JITInByValGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

void JITInByValGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::InByVal::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}

void JITInByValGenerator::finalize(
    LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_start.isSet());
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

JITInByIdGenerator::JITInByIdGenerator(
    CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSite, const RegisterSet& usedRegisters,
    CacheableIdentifier propertyName, JSValueRegs base, JSValueRegs value, GPRReg propertyCacheGPR)
    : JITByIdGenerator(codeBlock, propertyCache, jitType, codeOrigin, AccessType::InById, base, value)
{
    RELEASE_ASSERT(base.payloadGPR() != value.tagGPR());
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, AccessType::InById, CacheType::InByIdSelf, codeOrigin, callSite, usedRegisters, propertyName, base, value, propertyCacheGPR);
    }, propertyCache);
}

static void generateInByIdInlineAccessBaselineDataIC(CCallHelpers& jit, GPRReg propertyCacheGPR, JSValueRegs baseJSR, GPRReg scratch1GPR, JSValueRegs resultJSR)
{
    // SPEC-jit section 4.2 note: this inline fast path reads ONLY the
    // structure-id half of the packed unit (offsetOfInlineAccessBaseStructureID
    // == packed word + 4) and uses no offset, so there is no id/offset pair to
    // tear (I6 is about the pair). A racing single-word publish/invalidate is
    // observed as either the old id or the new id, both of which were valid
    // published "structure has the property" facts - sound flag-on as-is.
    jit.load32(CCallHelpers::Address(baseJSR.payloadGPR(), JSCell::structureIDOffset()), scratch1GPR);
    auto skipInlineAccess = jit.branch32(CCallHelpers::NotEqual, scratch1GPR, CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfInlineAccessBaseStructureID()));
    jit.boxBoolean(true, resultJSR);
    auto finished = jit.jump();
    skipInlineAccess.link(&jit);
    emitICStatsChainFlushProbe(jit, propertyCacheGPR);
    jit.loadPtr(CCallHelpers::Address(propertyCacheGPR, PropertyInlineCache::offsetOfHandler()), GPRInfo::handlerGPR);
    jit.call(CCallHelpers::Address(GPRInfo::handlerGPR, InlineCacheHandler::offsetOfCallTarget()), JITStubRoutinePtrTag);
    finished.link(&jit);
}

void JITInByIdGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    generateFastCommon(jit, InlineAccess::sizeForPropertyAccess());
}

void JITInByIdGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::InById::baseJSR;
    using BaselineJITRegisters::InById::resultJSR;
    using BaselineJITRegisters::InById::propertyCacheGPR;
    using BaselineJITRegisters::InById::scratch1GPR;

    m_start = jit.label();
    generateInByIdInlineAccessBaselineDataIC(jit, propertyCacheGPR, baseJSR, scratch1GPR, resultJSR);
    m_done = jit.label();
}

JITInstanceOfGenerator::JITInstanceOfGenerator(
    CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex,
    const RegisterSet& usedRegisters, GPRReg result, GPRReg value, GPRReg prototype, GPRReg propertyCacheGPR,
    bool prototypeIsKnownObject)
    : JITInlineCacheGenerator(codeBlock, propertyCache, jitType, codeOrigin, AccessType::InstanceOf)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, AccessType::InstanceOf, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, result, value, prototype, propertyCacheGPR, prototypeIsKnownObject);
    }, propertyCache);
}

void JITInstanceOfGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

void JITInstanceOfGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::Instanceof::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}

void JITInstanceOfGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

JITGetByValGenerator::JITGetByValGenerator(CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex, AccessType accessType, const RegisterSet& usedRegisters, JSValueRegs base, JSValueRegs property, JSValueRegs result, GPRReg arrayProfileGPR, GPRReg propertyCacheGPR)
    : Base(codeBlock, propertyCache, jitType, codeOrigin, accessType)
    , m_base(base)
    , m_result(result)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, base, property, result, arrayProfileGPR, propertyCacheGPR);
    }, propertyCache);
}

void JITGetByValGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

void JITGetByValGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::GetByVal::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}

void JITGetByValGenerator::generateEmptyPath(CCallHelpers& jit)
{
    m_start = jit.label();
    m_done = jit.label();
}

void JITGetByValGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

JITGetByValWithThisGenerator::JITGetByValWithThisGenerator(CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex, AccessType accessType, const RegisterSet& usedRegisters, JSValueRegs base, JSValueRegs property, JSValueRegs thisRegs, JSValueRegs result, GPRReg arrayProfileGPR, GPRReg propertyCacheGPR)
    : Base(codeBlock, propertyCache, jitType, codeOrigin, accessType)
    , m_base(base)
    , m_result(result)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, base, property, thisRegs, result, arrayProfileGPR, propertyCacheGPR);
    }, propertyCache);
}

void JITGetByValWithThisGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

#if USE(JSVALUE64)
void JITGetByValWithThisGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::GetByValWithThis::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}
#endif

void JITGetByValWithThisGenerator::generateEmptyPath(CCallHelpers& jit)
{
    m_start = jit.label();
    m_done = jit.label();
}

void JITGetByValWithThisGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

JITPutByValGenerator::JITPutByValGenerator(CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex, AccessType accessType, const RegisterSet& usedRegisters, JSValueRegs base, JSValueRegs property, JSValueRegs value, GPRReg arrayProfileGPR, GPRReg propertyCacheGPR)
    : Base(codeBlock, propertyCache, jitType, codeOrigin, accessType)
    , m_base(base)
    , m_value(value)
{
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, base, property, value, arrayProfileGPR, propertyCacheGPR);
    }, propertyCache);
}

void JITPutByValGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

void JITPutByValGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::PutByVal::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}

void JITPutByValGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

JITPrivateBrandAccessGenerator::JITPrivateBrandAccessGenerator(CodeBlock* codeBlock, CompileTimePropertyInlineCache propertyCache, JITType jitType, CodeOrigin codeOrigin, CallSiteIndex callSiteIndex, AccessType accessType, const RegisterSet& usedRegisters, JSValueRegs base, JSValueRegs brand, GPRReg propertyCacheGPR)
    : Base(codeBlock, propertyCache, jitType, codeOrigin, accessType)
{
    ASSERT(accessType == AccessType::CheckPrivateBrand || accessType == AccessType::SetPrivateBrand);
    WTF::visit([&](auto* propertyCache) {
        setUpPropertyInlineCache(*propertyCache, codeBlock, accessType, CacheType::Unset, codeOrigin, callSiteIndex, usedRegisters, base, brand, propertyCacheGPR);
    }, propertyCache);
}

void JITPrivateBrandAccessGenerator::generateFastPath(CCallHelpers& jit)
{
    ASSERT(m_propertyCache);
    ASSERT(is<RepatchingPropertyInlineCache>(*m_propertyCache));
    m_start = jit.label();
    m_slowPathJump = jit.patchableJump();
    m_done = jit.label();
}

void JITPrivateBrandAccessGenerator::generateDataICFastPath(CCallHelpers& jit)
{
    using BaselineJITRegisters::PrivateBrand::propertyCacheGPR;
    JITInlineCacheGenerator::generateDataICFastPath(jit, propertyCacheGPR);
}

void JITPrivateBrandAccessGenerator::finalize(LinkBuffer& fastPath, LinkBuffer& slowPath)
{
    ASSERT(m_propertyCache);
    Base::finalize(fastPath, slowPath, fastPath.locationOf<JITStubRoutinePtrTag>(m_start));
    // Note: m_propertyCache may be either kind here; the base finalize dispatches
    // on it (handler ICs are used by FTL sites under useHandlerICInFTL).
}

} // namespace JSC

#endif // ENABLE(JIT)

