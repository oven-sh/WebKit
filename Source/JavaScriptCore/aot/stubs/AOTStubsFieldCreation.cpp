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
#include "AOTObjectWords.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"

namespace JSC { namespace AOT {

static_assert(bitsPerSlotKey == 8 * sizeof(uint16_t));
static_assert(sizeof(KeyOrderTransition) == 2 << CCallHelpers::TimesEight);
static_assert([] {
    constexpr uint32_t product = (5u ^ 3u * KeyOrderTransition::hashMultiplier ^ 7u) * KeyOrderTransition::hashMultiplier;
    return KeyOrderTransition::hashOf(3ull << 32 | 5, 7) == (product ^ product >> KeyOrderTransition::hashFoldShift);
}());
static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));

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

void appendFieldToKeyOrder(StubAssembler& jit, const KeyOrderRegisters& r, JumpList& hasNoTransition)
{
    RELEASE_ASSERT(noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch));
    const GPRReg transition = r.scratch;
    const Address wordsBefore(transition, KeyOrderTransition::offsetOfOrderAndShapeWordsBefore());

    jit.load64(Address(r.object, offsetOfOrderWord), r.words);
    Jump hasNoExtraProperties = jit.branchTest32(CCallHelpers::Zero, r.words, TrustedImm32(orderWordHasExtraProperties));
    loadStructure(jit, r.object, r.scratch);
    // What is cached about the objects below a prototype relies on the run time's report of a field that the prototype gets.
    hasNoTransition.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.scratch, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    jit.load16(Address(r.scratch, CompiledStructure::offsetOfNumberOfExtraKeys()), r.scratch);
    hasNoTransition.append(jit.branch32(CCallHelpers::Equal, r.scratch, TrustedImm32(CompiledStructure::unknownNumberOfExtraKeys)));
    jit.lshift32(TrustedImm32(bitsPerSlotKey), r.scratch);
    jit.or32(r.scratch, r.slotKey);
    hasNoExtraProperties.link(&jit);
    jit.and64(TrustedImm64(static_cast<int64_t>(orderCountAndShapeMask)), r.words);
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
}

static void ensureStorageAndReturn(StubAssembler& jit, GPRReg object, GPRReg granuleBit, GPRReg wordOffset, GPRReg storage)
{
    RELEASE_ASSERT(noOverlap(object, granuleBit, wordOffset, storage));
    const BaseIndex extensionWord(object, wordOffset, CCallHelpers::TimesOne);

    JumpList slowCases;
    loadExtensionWordOffset(jit, object, wordOffset);
    if (jit.options().useWideReferences) {
        jit.load64(extensionWord, storage);
        slowCases.append(jit.branchTest64(CCallHelpers::Zero, storage));
        jit.load64(Address(storage, offsetOfExtensionGranuleBits), storage);
    } else {
        jit.load32(extensionWord, storage);
        decodeCell32(jit, storage, storage);
        jit.load64(Address(storage, offsetOfEmptyExtensionInCage + offsetOfExtensionGranuleBits), storage);
    }
    slowCases.append(jit.branchTest64(CCallHelpers::Zero, storage, granuleBit));
    jit.returnToCaller();

    slowCases.link(&jit);
    OperationCall ensureStorage = voidOperationCall;
    ensureStorage.thirdArgumentIn = granuleBit;
    jit.callOperationAndReturn(Entry::operationAOTEnsureExtensionGranules, ensureStorage);
}

static void generateSingleCreation(StubAssembler& jit, bool ensuresStorage)
{
    const GPRReg object = jit.operand(0);
    const GPRReg slotKey = jit.operand(1);
    const GPRReg words = jit.allocateScratch(StubRegister::T9);
    const GPRReg entry = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);

    JumpList hasNoTransition;
    appendFieldToKeyOrder(jit, { object, slotKey, entry, words, scratch }, hasNoTransition);
    if (ensuresStorage)
        ensureStorageAndReturn(jit, object, jit.operand(2), words, entry);
    else
        jit.returnToCaller();

    hasNoTransition.link(&jit);
    jit.exitOnMiss(ensuresStorage ? Entry::operationAOTDidCreateFieldOutOfLineAt : Entry::operationAOTDidCreateFieldAt, voidOperationCall);
}

static void generateRunOfCreations(StubAssembler& jit)
{
    const GPRReg object = jit.operand(0);
    const GPRReg remainingKeys = jit.clobberOperand(1);
    const GPRReg words = jit.allocateScratch(StubRegister::T9);
    const GPRReg entry = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg slotKey = jit.allocateScratch(StubRegister::T13);

#if CPU(ARM64)
    const GPRReg allKeys = jit.allocateScratch(StubRegister::T15);
    jit.move(remainingKeys, allKeys);
#else
    jit.push(remainingKeys);
#endif

    JumpList hasNoTransition;
    Label nextKey = jit.label();
    jit.and32(TrustedImm32(slotKeyMask), remainingKeys, slotKey);
    appendFieldToKeyOrder(jit, { object, slotKey, entry, words, scratch }, hasNoTransition);
    jit.urshift64(TrustedImm32(bitsPerSlotKey), remainingKeys);
    jit.branchTest64(CCallHelpers::NonZero, remainingKeys).linkTo(nextKey, &jit);
#if CPU(X86_64)
    jit.addPtr(TrustedImm32(sizeof(CPURegister)), CCallHelpers::stackPointerRegister);
#endif
    jit.returnToCaller();

    hasNoTransition.link(&jit);
#if CPU(ARM64)
    jit.move(allKeys, remainingKeys);
#else
    jit.pop(remainingKeys);
#endif
    jit.exitOnMiss(Entry::operationAOTDidCreateFieldsAt, voidOperationCall);
}

StubSignature signatureOfDidCreateField(StubKey key)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    switch (static_cast<CreatedFields>(key.form())) {
    case CreatedFields::One:
    case CreatedFields::Run:
        return signature;
    case CreatedFields::OneOutOfLine:
        signature.addOperand(gprOf(StubRegister::R0));
        return signature;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateDidCreateField(StubAssembler& jit)
{
    switch (static_cast<CreatedFields>(jit.key().form())) {
    case CreatedFields::One:
        generateSingleCreation(jit, false);
        return;
    case CreatedFields::OneOutOfLine:
        generateSingleCreation(jit, true);
        return;
    case CreatedFields::Run:
        generateRunOfCreations(jit);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
