/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsField.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObjectWords.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsBarriers.h"

namespace JSC { namespace AOT {

static_assert(bitsPerSlotKey == 8 * sizeof(uint16_t));
static_assert(sizeof(KeyOrderTransition) == 2 << CCallHelpers::TimesEight);
static_assert([] {
    constexpr uint32_t product = (5u ^ 3u * KeyOrderTransition::hashMultiplier ^ 7u) * KeyOrderTransition::hashMultiplier;
    return KeyOrderTransition::hashOf(3ull << 32 | 5, 7) == (product ^ product >> KeyOrderTransition::hashFoldShift);
}());
static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));
static_assert(areDistinctOnEveryISA(StubRegister::A1, StubRegister::A2, StubRegister::R0, StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12, StubRegister::T13, StubRegister::T14));

static constexpr OperationCall voidOperationCall {
    .returns = OperationCall::Returns::Void,
    .saves = OperationCall::Saves::CallerSavedRegisters,
    .checksException = false,
};

static void computeTransitionHash(StubAssembler& jit, const KeyOrderRegisters& r)
{
    const TrustedImm32 multiplier(static_cast<int32_t>(KeyOrderTransition::hashMultiplier));
    const TrustedImm32 foldShift(KeyOrderTransition::hashFoldShift);
#if CPU(ARM64)
    jit.move(multiplier, r.scratch);
    jit.urshift64(r.words, TrustedImm32(32), r.entry);
    jit.mul32(r.scratch, r.entry);
    jit.xor32(r.words, r.entry);
    jit.xor32(r.slotKey, r.entry);
    jit.mul32(r.scratch, r.entry);
    jit.xorUnsignedRightShift32(r.entry, r.entry, foldShift, r.entry);
#else
    jit.urshift64(r.words, TrustedImm32(32), r.entry);
    jit.mul32(multiplier, r.entry, r.entry);
    jit.xor32(r.words, r.entry);
    jit.xor32(r.slotKey, r.entry);
    jit.mul32(multiplier, r.entry, r.entry);
    jit.urshift32(r.entry, foldShift, r.scratch);
    jit.xor32(r.scratch, r.entry);
#endif
}

static JumpList stepInsideOrderOfImage(StubAssembler& jit, const KeyOrderRegisters& r, std::optional<FieldOfLayout> known)
{
    const GPRReg layout = known ? known->layout : r.scratch;
    std::optional<OwnedScratchRegister> ownedScratch;
    if (!known)
        ownedScratch.emplace(jit);
    const GPRReg limit = known ? r.scratch : ownedScratch->gpr();
    RELEASE_ASSERT(known ? noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch, layout, known->fieldIndex) : noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch, limit));
    static_assert(sizeof(KeyOrderData) == sizeof(uint16_t) && !OBJECT_OFFSETOF(KeyOrderData, length) && sizeof(FieldData) == 1 << 4);
    static_assert(OBJECT_OFFSETOF(FieldData, flags) == sizeof(uint32_t) && std::endian::native == std::endian::little);
    const Address tables(instanceGPR, AOT_INSTANCE_OFFSET(image.tables));
    JumpList doesNotApply;
    JumpList isListed;

    jit.load32(Address(r.object, offsetOfOrderWord), r.words);
    doesNotApply.append(jit.branchTest32(CCallHelpers::NonZero, r.words, TrustedImm32(static_cast<int32_t>(~orderAndCountMask))));
    if (!known) {
        loadStructure(jit, r.object, layout);
        jit.loadPtr(Address(layout, CompiledStructure::offsetOfLayout()), layout);
    }
    jit.and32(TrustedImm32(orderWordOrderMask), r.words, r.entry);
    doesNotApply.append(jit.branch32(CCallHelpers::AboveOrEqual, r.entry, Address(layout, OBJECT_OFFSETOF(LayoutData, numberOfOrders))));
    jit.lshift32(TrustedImm32(2), r.entry);
    jit.add32(Address(layout, OBJECT_OFFSETOF(LayoutData, ordersOffset)), r.entry);
    jit.addPtr(tables, r.entry);
    jit.load32(Address(r.entry), r.entry);
    jit.addPtr(tables, r.entry);
    jit.urshift32(TrustedImm32(orderWordCountShift), r.words);
    jit.load16(Address(r.entry), limit);
    doesNotApply.append(jit.branch32(CCallHelpers::AboveOrEqual, r.words, limit));
    jit.load16(BaseIndex(r.entry, r.words, CCallHelpers::TimesTwo, sizeof(KeyOrderData)), r.entry);
    if (known)
        doesNotApply.append(jit.branch32(CCallHelpers::NotEqual, r.entry, known->fieldIndex));
    else {
        jit.load16(Address(layout, OBJECT_OFFSETOF(LayoutData, numberOfFields)), limit);
        doesNotApply.append(jit.branch32(CCallHelpers::AboveOrEqual, r.entry, limit));
        jit.lshift32(TrustedImm32(4), r.entry);
        jit.addPtr(layout, r.entry);
        jit.load32(Address(r.entry, sizeof(LayoutData) + OBJECT_OFFSETOF(FieldData, slot)), r.words);
        jit.and32(TrustedImm32(SlotData::maxOffset), r.words);
        Jump isInCell = jit.branchTest32(CCallHelpers::Zero, Address(r.entry, sizeof(LayoutData) + OBJECT_OFFSETOF(FieldData, flags)), TrustedImm32(fieldIsOutOfLine));
        jit.or32(TrustedImm32(outOfLineSlotKey), r.words);
        isInCell.link(&jit);
        doesNotApply.append(jit.branch32(CCallHelpers::NotEqual, r.words, r.slotKey));
    }
    jit.add32(TrustedImm32(1 << orderWordCountShift), Address(r.object, offsetOfOrderWord));
    isListed.append(jit.jump());

    doesNotApply.link(&jit);
    return isListed;
}

void appendFieldToKeyOrder(StubAssembler& jit, const KeyOrderRegisters& r, JumpList& hasNoTransition, std::optional<FieldOfLayout> known)
{
    RELEASE_ASSERT(noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch));
    JumpList isListed = stepInsideOrderOfImage(jit, r, known);
    const GPRReg transition = r.scratch;
    const Address wordsBefore(transition, KeyOrderTransition::offsetOfOrderAndShapeWordsBefore());

    jit.load64(Address(r.object, offsetOfOrderWord), r.words);
    Jump hasNoExtraProperties = jit.branchTest32(CCallHelpers::Zero, r.words, TrustedImm32(orderWordHasExtraProperties));
    loadStructure(jit, r.object, r.scratch);
    hasNoTransition.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.scratch, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    jit.load16(Address(r.scratch, CompiledStructure::offsetOfNumberOfExtraKeys()), r.scratch);
    hasNoTransition.append(jit.branch32(CCallHelpers::Equal, r.scratch, TrustedImm32(CompiledStructure::unknownNumberOfExtraKeys)));
    static_assert(transitionKeyFor(5, 3) == (5 | 3 << bitsPerSlotKey));
    jit.lshift32(TrustedImm32(bitsPerSlotKey), r.scratch);
    jit.or32(r.scratch, r.slotKey);
    hasNoExtraProperties.link(&jit);
    jit.and64(TrustedImm64(static_cast<int64_t>(orderCountAndShapeMask)), r.words);
    jit.or64(TrustedImm64(static_cast<int64_t>(orderAndShapeWordsAreUnmarked)), r.words);
    computeTransitionHash(jit, r);

    Label probe = jit.label();
    jit.and32(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.keyOrderTransitionMask)), r.entry);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.keyOrderTransitions)), transition);
    jit.getEffectiveAddress(BaseIndex(transition, r.entry, CCallHelpers::TimesEight), transition);
    jit.getEffectiveAddress(BaseIndex(transition, r.entry, CCallHelpers::TimesEight), transition);
    Jump hasOtherWords = jit.branch64(CCallHelpers::NotEqual, wordsBefore, r.words);
    Jump isFound = jit.branch32(CCallHelpers::Equal, Address(transition, KeyOrderTransition::offsetOfKey()), r.slotKey);
    Label advance = jit.label();
    jit.add32(TrustedImm32(1), r.entry);
    jit.jump().linkTo(probe, &jit);

    hasOtherWords.link(&jit);
    Jump isNotPublished = jit.branchTest64(CCallHelpers::Zero, wordsBefore);
    jit.jump().linkTo(advance, &jit);

    isNotPublished.link(&jit);
    jit.and32(TrustedImm32(slotKeyMask), r.slotKey);
    hasNoTransition.append(jit.jump());

    isFound.link(&jit);
    jit.load32(Address(transition, KeyOrderTransition::offsetOfOrderAndCountAfter()), r.entry);
    jit.load32(Address(r.object, offsetOfOrderWord), r.scratch);
    jit.and32(TrustedImm32(static_cast<int32_t>(~orderAndCountMask)), r.scratch);
    jit.or32(r.entry, r.scratch);
    jit.store32(r.scratch, Address(r.object, offsetOfOrderWord));
    jit.and32(TrustedImm32(slotKeyMask), r.slotKey);
    isListed.link(&jit);
}

enum class HalfOfKeyOrderRuns : bool { OfIndex, Other };

static void locateKeyOrderRun(StubAssembler& jit, GPRReg wordsBefore, GPRReg keys, GPRReg run, HalfOfKeyOrderRuns half = HalfOfKeyOrderRuns::OfIndex)
{
    static_assert(indexOfKeyOrderRun(3ull << 32 | 5, 9ull << 32 | 16) == (3 ^ 5 ^ 16));
    jit.urshift64(wordsBefore, TrustedImm32(32), run);
    jit.xor32(wordsBefore, run);
    jit.xor32(keys, run);
    if (half == HalfOfKeyOrderRuns::Other)
        jit.xor32(TrustedImm32(numberOfKeyOrderRuns / 2), run);
    jit.and32(TrustedImm32(numberOfKeyOrderRuns - 1), run);
    jit.lshift32(TrustedImm32(logSizeOfKeyOrderRun), run);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.keyOrderRuns)), run);
}

void listCreatedField(StubAssembler& jit, const KeyOrderRegisters& r, JumpList& hasNoTransition, std::optional<FieldOfLayout> known)
{
    const GPRReg run = r.entry;
    const Address orderWord(r.object, offsetOfOrderWord);
    JumpList isNotRemembered;
    jit.load64(orderWord, r.words);
    locateKeyOrderRun(jit, r.words, r.slotKey, run);
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), r.words));
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfSlotKeys()), r.slotKey));
    jit.load32(Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()), r.scratch);
    jit.store32(r.scratch, orderWord);
    JumpList isListed;
    isListed.append(jit.jump());

    const GPRReg keys = r.scratch;
    auto loadKeysBesideExtraKeys = [&](JumpList& hasNone) {
        static_assert(CompiledStructure::unknownNumberOfExtraKeys <= slotKeyMask);
        jit.and32(TrustedImm32(static_cast<int32_t>(~orderAndCountMask)), r.words, keys);
        hasNone.append(jit.branch32(CCallHelpers::NotEqual, keys, TrustedImm32(orderWordHasExtraProperties)));
        loadStructure(jit, r.object, keys);
        hasNone.append(jit.branchTest32(CCallHelpers::NonZero, Address(keys, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
        jit.load16(Address(keys, CompiledStructure::offsetOfNumberOfExtraKeys()), keys);
        hasNone.append(jit.branch32(CCallHelpers::Equal, keys, TrustedImm32(CompiledStructure::unknownNumberOfExtraKeys)));
        jit.or32(TrustedImm32(1 << bitsPerSlotKey), keys);
        jit.lshift64(TrustedImm32(2 * bitsPerSlotKey), keys);
        jit.or64(r.slotKey, keys);
    };
    isNotRemembered.link(&jit);
    JumpList isNotRememberedBesideExtraKeys;
    loadKeysBesideExtraKeys(isNotRememberedBesideExtraKeys);
    locateKeyOrderRun(jit, r.words, r.slotKey, run, HalfOfKeyOrderRuns::Other);
    isNotRememberedBesideExtraKeys.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), r.words));
    isNotRememberedBesideExtraKeys.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfSlotKeys()), keys));
    jit.load32(Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()), r.scratch);
    jit.store32(r.scratch, orderWord);
    isListed.append(jit.jump());

    isNotRememberedBesideExtraKeys.link(&jit);
    const GPRReg isStepTaken = r.entry;
    jit.preserving({ r.words }, [&] {
        JumpList isLeftToRunTime;
        appendFieldToKeyOrder(jit, r, isLeftToRunTime, known);
        jit.move(TrustedImm32(1), isStepTaken);
        Jump isDone = jit.jump();
        isLeftToRunTime.link(&jit);
        jit.move(TrustedImm32(0), isStepTaken);
        isDone.link(&jit);
    });
    hasNoTransition.append(jit.branchTest32(CCallHelpers::Zero, isStepTaken));
    Jump hasFlags = jit.branchTest32(CCallHelpers::NonZero, r.words, TrustedImm32(static_cast<int32_t>(~orderAndCountMask)));
    locateKeyOrderRun(jit, r.words, r.slotKey, run);
    jit.store64(r.words, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()));
    jit.store64(r.slotKey, Address(run, KeyOrderRun::offsetOfSlotKeys()));
    jit.load32(orderWord, r.scratch);
    jit.store32(r.scratch, Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()));
    isListed.append(jit.jump());

    hasFlags.link(&jit);
    loadKeysBesideExtraKeys(isListed);
    locateKeyOrderRun(jit, r.words, r.slotKey, run, HalfOfKeyOrderRuns::Other);
    jit.store64(r.words, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()));
    jit.store64(keys, Address(run, KeyOrderRun::offsetOfSlotKeys()));
    jit.load32(orderWord, r.scratch);
    jit.store32(r.scratch, Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()));
    isListed.link(&jit);
}

void listCreatedFieldAndReturn(StubAssembler& jit, const KeyOrderRegisters& r, GPRReg value, std::optional<FieldOfLayout> known)
{
    JumpList hasNoTransition;
    listCreatedField(jit, r, hasNoTransition, known);
    returnAfterStoring(jit, r.object, value);

    hasNoTransition.link(&jit);
    OperationCall didCreateField = voidOperationCall;
    didCreateField.secondArgumentIn = r.object;
    didCreateField.thirdArgumentIn = r.slotKey;
    jit.callOperationAndReturn(Entry::operationAOTDidCreateFieldAt, didCreateField);
}

void createGranuleOfField(StubAssembler& jit, const FieldStoreRegisters& r, const WaysToFieldInExtension& ways, FirstExtension firstExtension)
{
    const GPRReg object = r.field.object;
    const GPRReg granule = r.field.representation;
    const GPRReg granuleBit = r.secondScratch;
    const GPRReg extension = r.field.address;
    JumpList cannotInsert;
    jit.urshift32(r.field.slotData, TrustedImm32(3), granule);
    jit.sub32(TrustedImm32(1), granule);
    jit.move(TrustedImm32(1), granuleBit);
    jit.lshift64(granule, granuleBit);
    static_assert(granuleOf(offsetOfFirstExtensionSlot) == offsetOfFirstExtensionSlot / 8 - 1 && extensionGranuleSize == 8 && maxExtensionGranules == 64);
    if (jit.options().useWideReferences)
        cannotInsert.append(jit.branchTest64(CCallHelpers::Zero, extension));
    else if (firstExtension == FirstExtension::IsAllocatedInLine)
        allocateFirstExtensionWithOneGranule(jit, object, granuleBit, extension, granule, r.scratch, ways.fromGranule, cannotInsert);
    insertGranule(jit, extension, granuleBit, granule, OwnedScratchRegister(jit), cannotInsert);
    jit.move(granule, extension);
    jit.jump().linkTo(ways.fromGranule, &jit);

    cannotInsert.link(&jit);
    const StubKey grow(StubFamily::AllocateExtensionSlow);
    const StubSignature signature = signatureOfAllocateExtensionSlow(grow);
    const GPRReg owner = signature.operands[0].gpr();
    const GPRReg ownerForBarrier = signatureOfWriteBarrier(writeBarrier()).operands[0].gpr();
    Vector<GPRReg, 7> kept;
    for (GPRReg written : { extensionMadeBySlowStub, ownerForBarrier, owner, signature.operands[1].gpr(), gprOf(StubRegister::T9), gprOf(StubRegister::T11), gprOf(StubRegister::T12) }) {
        if (written != extension && written != granule && written != granuleBit)
            kept.appendIfNotContains(written);
    }
    const GPRReg state = gprOf(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(extension, owner, ownerForBarrier, state));
    jit.emitFunctionPrologue();
    jit.preserving(kept.span(), [&] {
        jit.shuffleRegisters<GPRReg, 2>({ object, granuleBit }, { owner, signature.operands[1].gpr() });
        jit.callStub(grow);
        jit.move(extensionMadeBySlowStub, extension);
        jit.load8(Address(owner, JSCell::cellStateOffset()), state);
        Jump needsNoBarrier = jit.branch32(CCallHelpers::Above, state, Address(instanceGPR, AOT_INSTANCE_OFFSET(barrierThreshold)));
        jit.move(owner, ownerForBarrier);
        jit.callStub(writeBarrier());
        needsNoBarrier.link(&jit);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
    jit.jump().linkTo(ways.fromExtension, &jit);
}

StubSignature signatureOfDidCreateField(StubKey) { return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }); }

void generateDidCreateField(StubAssembler& jit)
{
    const GPRReg object = jit.operand(0);
    const GPRReg slotKey = jit.operand(1);
    const GPRReg words = jit.allocateScratch(StubRegister::T9);
    const GPRReg entry = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg wordsBefore = jit.allocateScratch(StubRegister::T10);
    const GPRReg run = entry;
    const KeyOrderRegisters keyOrder { object, slotKey, entry, words, scratch };
    JumpList hasNoTransition;

    RELEASE_ASSERT(noOverlap(object, slotKey, wordsBefore, run, scratch));
    JumpList isNotRemembered;
    JumpList mayHaveFlags;
    jit.load64(Address(object, offsetOfOrderWord), wordsBefore);
    locateKeyOrderRun(jit, wordsBefore, slotKey, run);
    mayHaveFlags.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), wordsBefore));
    mayHaveFlags.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfSlotKeys()), slotKey));
    jit.load32(Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()), scratch);
    jit.store32(scratch, Address(object, offsetOfOrderWord));
    jit.returnToCaller();

    mayHaveFlags.link(&jit);
    Jump hasExtraProperties = jit.branchTest32(CCallHelpers::NonZero, wordsBefore, TrustedImm32(orderWordHasExtraProperties));
    jit.move(wordsBefore, scratch);
    jit.and64(TrustedImm64(static_cast<int64_t>(orderCountAndShapeMask)), wordsBefore);
    locateKeyOrderRun(jit, wordsBefore, slotKey, run);
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), wordsBefore));
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfSlotKeys()), slotKey));
    jit.and32(TrustedImm32(static_cast<int32_t>(~orderAndCountMask)), scratch);
    jit.or32(Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()), scratch);
    jit.store32(scratch, Address(object, offsetOfOrderWord));
    jit.returnToCaller();

    hasExtraProperties.link(&jit);
    listCreatedField(jit, keyOrder, hasNoTransition, std::nullopt);
    jit.returnToCaller();

    isNotRemembered.link(&jit);
    appendFieldToKeyOrder(jit, keyOrder, hasNoTransition);
    locateKeyOrderRun(jit, wordsBefore, slotKey, run);
    jit.store64(wordsBefore, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()));
    jit.store64(slotKey, Address(run, KeyOrderRun::offsetOfSlotKeys()));
    jit.load32(Address(object, offsetOfOrderWord), scratch);
    jit.and32(TrustedImm32(static_cast<int32_t>(orderAndCountMask)), scratch);
    jit.store32(scratch, Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()));
    jit.returnToCaller();

    hasNoTransition.link(&jit);
    jit.exitOnMiss(Entry::operationAOTDidCreateFieldAt, voidOperationCall);
}

StubSignature signatureOfDidCreateFieldOfSlot(StubKey)
{
    StubSignature signature = StubSignature::withoutResult({ gprOf(StubRegister::A1) });
    signature.immediate = gprOf(StubRegister::A2);
    return signature;
}

void generateDidCreateFieldOfSlot(StubAssembler& jit)
{
    const std::optional<uint32_t> knownSlotKey = jit.key().immediate();
    const GPRReg object = jit.operand(0);
    const GPRReg slotKey = jit.reuseScratch(StubRegister::A2);
    if (knownSlotKey) {
        RELEASE_ASSERT(*knownSlotKey != noSlotKey && *knownSlotKey <= slotKeyMask);
        jit.move(TrustedImm32(static_cast<int32_t>(*knownSlotKey)), slotKey);
    }
    checkStubOperands(didCreateField(), { object, slotKey });
    jit.jumpToStub(didCreateField());
}

StubSignature signatureOfPutSlotOrAdd(StubKey key)
{
    StubSignature signature = StubSignature::withoutResult({ gprOf(firstStubOperandRegister) });
    if (representationStoredBy(key) == SlotRepresentation::OptionalDouble)
        signature.addOperand(FPRInfo::argumentFPR0);
    else
        signature.addOperand(gprOf(StubRegister::A1));
    signature.immediate = gprOf(StubRegister::A2);
    return signature;
}

StubSignature signatureOfPutSlotOrAddAtKeyInRegister(StubKey key)
{
    StubSignature signature = signatureOfPutSlotOrAdd(putSlotOrAdd(PlaceOfCreatedSlot::InCell, representationStoredBy(key)));
    signature.addOperand(*std::exchange(signature.immediate, std::nullopt));
    return signature;
}

void generatePutSlotOrAddAtKeyInRegister(StubAssembler& jit)
{
    const StubKey takesKeyAsImmediate = putSlotOrAdd(PlaceOfCreatedSlot::InCell, representationStoredBy(jit.key()));
    const GPRReg slotKey = jit.clobberOperand(2);
    RELEASE_ASSERT(signatureOfPutSlotOrAdd(takesKeyAsImmediate).immediate == slotKey);
    jit.jumpToStub(takesKeyAsImmediate);
}

static void returnBehindBarrierOfCreatedField(StubAssembler& jit, GPRReg object, GPRReg state)
{
    Jump isWithinThreshold = branchIfBarrierIsNeeded(jit, object, state);
    jit.returnToCaller();

    isWithinThreshold.link(&jit);
    checkStubOperands(writeBarrier(), { object });
    jit.jumpToStub(writeBarrier());
}

static void narrowOptionalIntegerOfCreatedField(StubAssembler& jit, GPRReg integerOrMarker, GPRReg result)
{
    constexpr unsigned markerShift = 48;
    constexpr uint32_t firstMarker = static_cast<uint32_t>(undefinedInteger >> markerShift);
    static_assert(nullInteger == undefinedInteger + (1ll << markerShift) && absentInteger == undefinedInteger + (2ll << markerShift));
    static_assert(nullInt32 == undefinedInt32 + 1 && maxSlotInteger < undefinedInteger);
    jit.move(integerOrMarker, result);
    Jump isInteger = jit.branch64(CCallHelpers::LessThanOrEqual, integerOrMarker, TrustedImm64(maxSlotInteger));
    jit.urshift64(integerOrMarker, TrustedImm32(markerShift), result);
    jit.add32(TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(undefinedInt32) - firstMarker)), result);
    Jump isUndefinedOrNull = jit.branch32(CCallHelpers::NotEqual, result, TrustedImm32(nullInt32 + 1));
    jit.move(TrustedImm32(absentInt32), result);
    isUndefinedOrNull.link(&jit);
    isInteger.link(&jit);
}

template<typename AddressOfSlot>
static void storeInSlotOfCreatedField(StubAssembler& jit, SlotRepresentation representation, AddressOfSlot slot, GPRReg scratch)
{
    if (representation == SlotRepresentation::OptionalDouble) {
        jit.storeDouble(jit.operandFPR(1), slot);
        return;
    }
    GPRReg bits = jit.operand(1);
    if (representation == SlotRepresentation::OptionalInt32) {
        narrowOptionalIntegerOfCreatedField(jit, bits, scratch);
        bits = scratch;
    }
    switch (widthOf(representation)) {
    case 1:
        jit.store8(bits, slot);
        return;
    case 4:
        jit.store32(bits, slot);
        return;
    case 8:
        jit.store64(bits, slot);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generatePutSlotOrAdd(StubAssembler& jit)
{
    static_assert(slotKeyFor(8, false) == 8);
    static_assert(areDistinctOnEveryISA(firstStubOperandRegister, StubRegister::A1, StubRegister::A2, StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12));
    const PlaceOfCreatedSlot place = placeOfSlotCreatedBy(jit.key());
    const SlotRepresentation representation = representationStoredBy(jit.key());
    const unsigned width = widthOf(representation);
    const uint64_t absentBits = traitsOf(representation).absentBits;
    const std::optional<uint32_t> knownSlotKey = jit.key().immediate();

    if (knownSlotKey && place == PlaceOfCreatedSlot::OutOfLine) {
        RELEASE_ASSERT(*knownSlotKey & outOfLineSlotKey);
        jit.jumpToStubTakingImmediate();
        return;
    }

    const GPRReg object = jit.operand(0);
    const GPRReg entry = jit.allocateScratch(StubRegister::T11);

    if (knownSlotKey) {
        RELEASE_ASSERT(*knownSlotKey != noSlotKey && !(*knownSlotKey & outOfLineSlotKey));
        const Address slot(object, static_cast<int32_t>(*knownSlotKey));
        loadSlotBits(jit, width, slot, entry);
        Jump isAbsent = branchIfBitsEqual(jit, width, entry, absentBits);
        storeInSlotOfCreatedField(jit, representation, slot, entry);
        returnBehindBarrierOfCreatedField(jit, object, entry);

        isAbsent.link(&jit);
        jit.jumpToStubTakingImmediate();
        return;
    }

    const GPRReg slotKey = jit.immediate();
    const GPRReg words = jit.allocateScratch(StubRegister::T9);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    auto storeAndReturn = [&](auto slot) {
        JumpList hasNoTransition;
        loadSlotBits(jit, width, slot, words);
        storeInSlotOfCreatedField(jit, representation, slot, entry);
        Jump wasAbsent = branchIfBitsEqual(jit, width, words, absentBits);
        Label isListed = jit.label();
        returnBehindBarrierOfCreatedField(jit, object, entry);

        wasAbsent.link(&jit);
        listCreatedField(jit, { object, slotKey, entry, words, scratch }, hasNoTransition);
        jit.jump().linkTo(isListed, &jit);

        hasNoTransition.link(&jit);
        OperationCall didCreateField = voidOperationCall;
        didCreateField.secondArgumentIn = object;
        didCreateField.thirdArgumentIn = slotKey;
        jit.callOperationAndReturn(Entry::operationAOTDidCreateFieldAt, didCreateField);
    };
    if (place == PlaceOfCreatedSlot::InCell) {
        storeAndReturn(BaseIndex(object, slotKey, CCallHelpers::TimesOne));
        return;
    }

    static_assert(maxExtensionSize <= outOfLineSlotKey && !(outOfLineSlotKey % (maxExtensionGranules * extensionGranuleSize)));
    const GPRReg address = jit.allocateScratch(StubRegister::T10);
    const FieldStoreRegisters registers {
        .field = { .object = object, .slotData = slotKey, .extensionWordOffset = address, .address = address, .representation = words },
        .value = InvalidGPRReg,
        .type = InvalidGPRReg,
        .scratch = scratch,
        .secondScratch = entry,
        .number = InvalidFPRReg,
    };
    JumpList hasNoStorage;
    WaysToFieldInExtension ways;
    loadExtensionWordOffset(jit, object, address);
    computeFieldAddress(jit, registers.field, true, hasNoStorage, &ways, representation);
    storeAndReturn(Address(address));

    hasNoStorage.link(&jit);
    createGranuleOfField(jit, registers, ways, FirstExtension::IsAllocatedInLine);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
