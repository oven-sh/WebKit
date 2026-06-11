/*
 * Copyright (C) 2016-2020 Apple Inc. All rights reserved.
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
#include "CCallHelpers.h"

#if ENABLE(JIT)

#include "ConcurrentButterflyOperations.h"
#include "JITOperations.h"
#include "JSObject.h"
#include "LinkBuffer.h"
#include "MaxFrameExtentForSlowPathCall.h"
#include "ShadowChicken.h"
#include <wtf/TZoneMallocInlines.h>

// Object-model workstream tag constants (SPEC-objectmodel section 2, consumed
// via SPEC-jit R3). THREADS-INTEGRATE(jit): when runtime/ConcurrentButterfly.h
// is absent in a partial tree, identical local constants below keep this
// compiling; the static_asserts pin the frozen encoding either way.
#if __has_include("ConcurrentButterfly.h")
#include "ConcurrentButterfly.h"
#define JSC_CCALLHELPERS_HAS_CONCURRENT_BUTTERFLY 1
#endif

#if OS(DARWIN) && ENABLE(FAST_TLS_JIT)
#include <wtf/FastTLS.h>
#endif

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CCallHelpers);

void CCallHelpers::logShadowChickenProloguePacket(GPRReg shadowPacket, GPRReg scratch1, GPRReg scope)
{
    storePtr(GPRInfo::callFrameRegister, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, frame)));
    loadPtr(Address(GPRInfo::callFrameRegister, OBJECT_OFFSETOF(CallerFrameAndPC, callerFrame)), scratch1);
    storePtr(scratch1, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, callerFrame)));
    loadPtr(addressFor(CallFrameSlot::callee), scratch1);
    storePtr(scratch1, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, callee)));
    storePtr(scope, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, scope)));
}

void CCallHelpers::ensureShadowChickenPacket(VM& vm, GPRReg shadowPacket, GPRReg scratch1NonArgGPR, GPRReg scratch2)
{
    ShadowChicken* shadowChicken = vm.shadowChicken();
    RELEASE_ASSERT(shadowChicken);
    ASSERT(!RegisterSet::argumentGPRs().contains(scratch1NonArgGPR, IgnoreVectors));
    move(TrustedImmPtr(shadowChicken->addressOfLogCursor()), scratch1NonArgGPR);
    loadPtr(Address(scratch1NonArgGPR), shadowPacket);
    Jump ok = branchPtr(Below, shadowPacket, TrustedImmPtr(shadowChicken->logEnd()));
    setupArguments<decltype(operationProcessShadowChickenLog)>(TrustedImmPtr(&vm));
    prepareCallOperation(vm);
    move(TrustedImmPtr(tagCFunction<OperationPtrTag>(operationProcessShadowChickenLog)), scratch1NonArgGPR);
    call(scratch1NonArgGPR, OperationPtrTag);
    move(TrustedImmPtr(shadowChicken->addressOfLogCursor()), scratch1NonArgGPR);
    loadPtr(Address(scratch1NonArgGPR), shadowPacket);
    ok.link(this);
    addPtr(TrustedImm32(sizeof(ShadowChicken::Packet)), shadowPacket, scratch2);
    storePtr(scratch2, Address(scratch1NonArgGPR));
}


template <typename CodeBlockType>
void CCallHelpers::logShadowChickenTailPacketImpl(GPRReg shadowPacket, JSValueRegs thisRegs, GPRReg scope, CodeBlockType codeBlock, CallSiteIndex callSiteIndex)
{
    storePtr(GPRInfo::callFrameRegister, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, frame)));
    storePtr(TrustedImmPtr(ShadowChicken::Packet::tailMarker()), Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, callee)));
    storeValue(thisRegs, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, thisValue)));
    storePtr(scope, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, scope)));
    storePtr(codeBlock, Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, codeBlock)));
    store32(TrustedImm32(callSiteIndex.bits()), Address(shadowPacket, OBJECT_OFFSETOF(ShadowChicken::Packet, callSiteIndex)));
}

void CCallHelpers::logShadowChickenTailPacket(GPRReg shadowPacket, JSValueRegs thisRegs, GPRReg scope, GPRReg codeBlock, CallSiteIndex callSiteIndex)
{
    logShadowChickenTailPacketImpl(shadowPacket, thisRegs, scope, codeBlock, callSiteIndex);
}

// ===========================================================================
// SPEC-jit section 5.5 (Task 8): TID/SW butterfly choke points.
// ===========================================================================

namespace CCallHelpersConcurrentButterfly {

#if defined(JSC_CCALLHELPERS_HAS_CONCURRENT_BUTTERFLY)
[[maybe_unused]] static constexpr uint64_t tagMask = JSC::butterflyTagMask;
[[maybe_unused]] static constexpr uint64_t pointerMask = JSC::butterflyPointerMask;
[[maybe_unused]] static constexpr uint64_t swBit = JSC::butterflySWBit;
#else
// Frozen encoding (SPEC-objectmodel section 2): bit 63 = SW, bits 62..48 =
// TID, low 48 = payload; TID == 0x7fff (=> top16 == 0xffff with SW) =
// segmented.
[[maybe_unused]] static constexpr uint64_t tagMask = 0xffff000000000000ULL;
[[maybe_unused]] static constexpr uint64_t pointerMask = 0x0000ffffffffffffULL;
[[maybe_unused]] static constexpr uint64_t swBit = 1ULL << 63;
#endif
// Unsigned compare trick: top16 == 0xffff <=> tagged >= 0xffff << 48.
[[maybe_unused]] static constexpr uint64_t segmentedFloor = 0xffff000000000000ULL;
[[maybe_unused]] static constexpr uint64_t tidTagSpan = 1ULL << 48; // (tagged ^ tidTag) < 2^48 <=> tag bits match

static_assert(tagMask == 0xffff000000000000ULL);
static_assert(pointerMask == 0x0000ffffffffffffULL);
static_assert(swBit == 0x8000000000000000ULL);

} // namespace CCallHelpersConcurrentButterfly

void CCallHelpers::loadButterflyTIDTag(GPRReg destGPR)
{
#if OS(LINUX) && (CPU(X86_64) || CPU(ARM64))
    loadFromELFTLS64(butterflyTIDTagELFTLSOffset(), destGPR);
#elif OS(DARWIN) && ENABLE(FAST_TLS_JIT)
    loadFromTLS64(fastTLSOffsetForKey(butterflyTIDTagTLSKey()), destGPR);
#else
    // D8/App. R5: no JIT-visible TLS mechanism; useJSThreads is unsupported
    // on this platform, so emission must never get here.
    UNUSED_PARAM(destGPR);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

void CCallHelpers::maskButterflyTag(GPRReg destGPR)
{
#if USE(JSVALUE64)
    and64(TrustedImm64(static_cast<int64_t>(CCallHelpersConcurrentButterfly::pointerMask)), destGPR);
#else
    UNUSED_PARAM(destGPR);
    RELEASE_ASSERT_NOT_REACHED(); // flag-on requires 64-bit (D8)
#endif
}

#if USE(JSVALUE64)

// R7/F7: on ARM64, order structure-check -> butterfly load with an address
// dependency (eor sid,sid -> 0, folded into the load's base). No-op x86-64.
// Uses destGPR as the dependency temp, so requires dest != base.
//
// Review round 1: when the caller has no register still holding the compared
// structureID (structureIDGPR == InvalidGPRReg, or it aliases dest), the
// dependency is built from a RE-LOAD of the cell's structureID through
// destGPR itself, so EVERY choke-routed site gets the dependency without
// needing a spare register. The re-load is sound by per-location coherence:
// it cannot return an older value than the load the guard compared, so
// ordering the butterfly load after the re-load still pairs it with the OM's
// butterfly-before-structureID store fence. (This closes the Task-8 gap-1
// inventory for all CCallHelpers-routed Baseline/handler sites; the only
// remaining unwired shape is dest == base, where no temp exists - those
// sites are enumerated in docs/threads/INTEGRATE-jit.md.)
static void loadButterflyWithStructureDependency(CCallHelpers& jit, GPRReg baseGPR, GPRReg destGPR, GPRReg structureIDGPR)
{
#if CPU(ARM64)
    if (destGPR != baseGPR) {
        if (structureIDGPR != InvalidGPRReg && destGPR != structureIDGPR)
            jit.xor64(structureIDGPR, structureIDGPR, destGPR); // 0, data-dependent on the compared structureID
        else {
            jit.load32(CCallHelpers::Address(baseGPR, JSCell::structureIDOffset()), destGPR); // R7 re-load (coherence-sound)
            jit.xor64(destGPR, destGPR, destGPR); // 0, data-dependent on the re-load
        }
        jit.addPtr(baseGPR, destGPR);
        jit.loadPtr(CCallHelpers::Address(destGPR, JSObject::butterflyOffset()), destGPR);
        return;
    }
#else
    UNUSED_PARAM(structureIDGPR);
#endif
    jit.loadPtr(CCallHelpers::Address(baseGPR, JSObject::butterflyOffset()), destGPR);
}

// Appends to slowCases iff the SW=1 object is ArrayStorage/SlowPut-shaped
// (AS-rule / I20); falls through otherwise. Requires indexingScratchGPR.
static void emitArrayStorageShapeCheck(CCallHelpers& jit, GPRReg baseGPR, GPRReg indexingScratchGPR, CCallHelpers::JumpList& slowCases)
{
    jit.load8(CCallHelpers::Address(baseGPR, JSCell::indexingTypeAndMiscOffset()), indexingScratchGPR);
    jit.and32(CCallHelpers::TrustedImm32(IndexingShapeMask), indexingScratchGPR);
    jit.sub32(CCallHelpers::TrustedImm32(ArrayStorageShape), indexingScratchGPR);
    slowCases.append(jit.branch32(CCallHelpers::BelowOrEqual, indexingScratchGPR, CCallHelpers::TrustedImm32(SlowPutArrayStorageShape - ArrayStorageShape)));
}

auto CCallHelpers::loadButterflyForRead(GPRReg baseGPR, GPRReg destGPR, ConcurrentButterflyShape shape, GPRReg indexingScratchGPR, GPRReg structureIDGPR) -> JumpList
{
    using namespace CCallHelpersConcurrentButterfly;
    JumpList slowCases;
    if (!Options::useJSThreads()) [[likely]] {
        loadPtr(Address(baseGPR, JSObject::butterflyOffset()), destGPR);
        return slowCases; // I1: today's single load, nothing else
    }

    loadButterflyWithStructureDependency(*this, baseGPR, destGPR, structureIDGPR);

    // Segmented (top16 == 0xffff) => slow: the generic path performs the
    // dependent spine load (regime 2).
    slowCases.append(branch64(AboveOrEqual, destGPR, TrustedImm64(static_cast<int64_t>(segmentedFloor))));

    switch (shape) {
    case ConcurrentButterflyShape::KnownNonArrayStorage:
        // AS-rule clause (c): AS modes statically excluded; SW state is
        // irrelevant for reads.
        break;
    case ConcurrentButterflyShape::KnownArrayStorage:
        // SW=1 AS reads take the locked slow path (I20). SW = bit 63.
        slowCases.append(branchTest64(Signed, destGPR, destGPR));
        break;
    case ConcurrentButterflyShape::MaybeArrayStorage:
        if (indexingScratchGPR != InvalidGPRReg) {
            ASSERT(indexingScratchGPR != baseGPR && indexingScratchGPR != destGPR);
            ASSERT(destGPR != baseGPR);
            Jump notSharedWritten = branchTest64(PositiveOrZero, destGPR, destGPR);
            emitArrayStorageShapeCheck(*this, baseGPR, indexingScratchGPR, slowCases);
            notSharedWritten.link(this);
        } else {
            // Conservative: SW=1 => slow (sound superset; see the Task 8
            // inventory in INTEGRATE-jit.md for the per-site disposition).
            slowCases.append(branchTest64(Signed, destGPR, destGPR));
        }
        break;
    }

    maskButterflyTag(destGPR); // I14(a): mask ALWAYS kept (D6)
    return slowCases;
}

auto CCallHelpers::loadButterflyForWrite(GPRReg baseGPR, GPRReg destGPR, GPRReg tidScratchGPR, ConcurrentButterflyShape shape, GPRReg indexingScratchGPR, GPRReg structureIDGPR) -> JumpList
{
    using namespace CCallHelpersConcurrentButterfly;
    JumpList slowCases;
    if (!Options::useJSThreads()) [[likely]] {
        loadPtr(Address(baseGPR, JSObject::butterflyOffset()), destGPR);
        return slowCases; // I1
    }

    RELEASE_ASSERT(tidScratchGPR != InvalidGPRReg && tidScratchGPR != baseGPR && tidScratchGPR != destGPR);

    loadButterflyWithStructureDependency(*this, baseGPR, destGPR, structureIDGPR);

    // (1) Segmented => slow.
    slowCases.append(branch64(AboveOrEqual, destGPR, TrustedImm64(static_cast<int64_t>(segmentedFloor))));

    // (2) Owner: tag bits == per-thread R5 tag (pre-shifted, SW=0), i.e.
    // (tagged ^ tidTag) < 2^48. The fused TID compare is NEVER elided (D9).
    loadButterflyTIDTag(tidScratchGPR);
    xor64(destGPR, tidScratchGPR);
    Jump owner = branch64(Below, tidScratchGPR, TrustedImm64(static_cast<int64_t>(tidTagSpan)));

    switch (shape) {
    case ConcurrentButterflyShape::KnownNonArrayStorage:
        // (3) Foreign + SW=1, not AS => mask + store. (4) Foreign + SW=0 =>
        // slow (the generic op runs ensureSharedWriteBit and the store).
        slowCases.append(branchTest64(PositiveOrZero, destGPR, destGPR));
        break;
    case ConcurrentButterflyShape::KnownArrayStorage:
        // Any non-owner AS write => locked slow path (I20); owner SW=0 AS
        // stores are the sound residual (AS-COPY, history section 17).
        slowCases.append(jump());
        break;
    case ConcurrentButterflyShape::MaybeArrayStorage:
        if (indexingScratchGPR != InvalidGPRReg) {
            ASSERT(indexingScratchGPR != baseGPR && indexingScratchGPR != destGPR && indexingScratchGPR != tidScratchGPR);
            ASSERT(destGPR != baseGPR);
            slowCases.append(branchTest64(PositiveOrZero, destGPR, destGPR)); // (4) foreign SW=0
            emitArrayStorageShapeCheck(*this, baseGPR, indexingScratchGPR, slowCases); // (3) AS => slow
        } else {
            // Conservative: every foreign write => slow (sound superset).
            slowCases.append(jump());
        }
        break;
    }

    owner.link(this);
    maskButterflyTag(destGPR); // I14(a)
    return slowCases;
}

void CCallHelpers::loadProperty(GPRReg object, GPRReg offset, JSValueRegs result, GPRReg storageScratch, JumpList& slowCases, GPRReg structureIDGPR)
{
    if (!Options::useJSThreads()) [[likely]] {
        AssemblyHelpers::loadProperty(object, offset, result);
        return;
    }

    ASSERT(noOverlap(offset, result));
    ASSERT(storageScratch != object && storageScratch != offset && storageScratch != InvalidGPRReg);
    Jump isInline = branch32(LessThan, offset, TrustedImm32(firstOutOfLineOffset));

    slowCases.append(loadButterflyForRead(object, storageScratch, ConcurrentButterflyShape::MaybeArrayStorage, InvalidGPRReg, structureIDGPR));
    neg32(offset);
    signExtend32ToPtr(offset, offset);
    Jump ready = jump();

    isInline.link(this);
    addPtr(
        TrustedImm32(
            static_cast<int32_t>(JSObject::offsetOfInlineStorage()) -
            (static_cast<int32_t>(firstOutOfLineOffset) - 2) * static_cast<int32_t>(sizeof(EncodedJSValue))),
        object, storageScratch);

    ready.link(this);

    loadValue(
        BaseIndex(
            storageScratch, offset, TimesEight, (firstOutOfLineOffset - 2) * sizeof(EncodedJSValue)),
        result);
}

void CCallHelpers::storeProperty(JSValueRegs value, GPRReg object, GPRReg offset, GPRReg scratch, GPRReg tidScratch, JumpList& slowCases, GPRReg structureIDGPR)
{
    if (!Options::useJSThreads()) [[likely]] {
        AssemblyHelpers::storeProperty(value, object, offset, scratch);
        return;
    }

    ASSERT(noOverlap(offset, scratch));
    ASSERT(noOverlap(value, scratch));
    ASSERT(noOverlap(offset, tidScratch));
    ASSERT(noOverlap(value, tidScratch));
    ASSERT(scratch != object && tidScratch != object && scratch != tidScratch);
    Jump isInline = branch32(LessThan, offset, TrustedImm32(firstOutOfLineOffset));

    slowCases.append(loadButterflyForWrite(object, scratch, tidScratch, ConcurrentButterflyShape::MaybeArrayStorage, InvalidGPRReg, structureIDGPR));
    neg32(offset);
    signExtend32ToPtr(offset, offset);
    Jump ready = jump();

    isInline.link(this);
    addPtr(
        TrustedImm32(
            static_cast<int32_t>(JSObject::offsetOfInlineStorage()) -
            (static_cast<int32_t>(firstOutOfLineOffset) - 2) * static_cast<int32_t>(sizeof(EncodedJSValue))),
        object, scratch);

    ready.link(this);

    storeValue(
        value,
        BaseIndex(scratch, offset, TimesEight, (firstOutOfLineOffset - 2) * sizeof(EncodedJSValue)));
}

#else // !USE(JSVALUE64)

auto CCallHelpers::loadButterflyForRead(GPRReg baseGPR, GPRReg destGPR, ConcurrentButterflyShape, GPRReg, GPRReg) -> JumpList
{
    RELEASE_ASSERT(!Options::useJSThreads()); // D8: 64-bit only flag-on
    loadPtr(Address(baseGPR, JSObject::butterflyOffset()), destGPR);
    return { };
}

auto CCallHelpers::loadButterflyForWrite(GPRReg baseGPR, GPRReg destGPR, GPRReg, ConcurrentButterflyShape, GPRReg, GPRReg) -> JumpList
{
    RELEASE_ASSERT(!Options::useJSThreads());
    loadPtr(Address(baseGPR, JSObject::butterflyOffset()), destGPR);
    return { };
}

void CCallHelpers::loadProperty(GPRReg object, GPRReg offset, JSValueRegs result, GPRReg, JumpList&, GPRReg)
{
    RELEASE_ASSERT(!Options::useJSThreads());
    AssemblyHelpers::loadProperty(object, offset, result);
}

void CCallHelpers::storeProperty(JSValueRegs value, GPRReg object, GPRReg offset, GPRReg scratch, GPRReg, JumpList&, GPRReg)
{
    RELEASE_ASSERT(!Options::useJSThreads());
    AssemblyHelpers::storeProperty(value, object, offset, scratch);
}

#endif // USE(JSVALUE64)

static_assert(!((maxFrameExtentForSlowPathCall + 2 * sizeof(CPURegister)) % 16), "Stack must be aligned after CTI thunk entry");

void CCallHelpers::emitCTIThunkPrologue(bool returnAddressAlreadyTagged)
{
    // Stash frame pointer and return address
    if (!returnAddressAlreadyTagged)
        tagReturnAddress();
#if CPU(X86_64)
    push(X86Registers::ebp); // return address pushed by the call instruction
#elif CPU(ARM64) || CPU(ARM_THUMB2) || CPU(RISCV64)
    pushPair(framePointerRegister, linkRegister);
#else
#   error "Not implemented on platform"
#endif
    // Make enough space on the stack to pass arguments in a call
    if constexpr (!!maxFrameExtentForSlowPathCall)
        subPtr(TrustedImm32(maxFrameExtentForSlowPathCall), stackPointerRegister);
}

void CCallHelpers::emitCTIThunkEpilogue()
{
    // Reset stack
    if constexpr (!!maxFrameExtentForSlowPathCall)
        addPtr(TrustedImm32(maxFrameExtentForSlowPathCall), stackPointerRegister);
    // Restore frame pointer and return address
#if CPU(X86_64)
    pop(X86Registers::ebp); // Return address left on stack
#elif CPU(ARM64) || CPU(ARM_THUMB2) || CPU(RISCV64)
    popPair(framePointerRegister, linkRegister);
#else
#   error "Not implemented on platform"
#endif
}

} // namespace JSC

#endif // ENABLE(JIT)
