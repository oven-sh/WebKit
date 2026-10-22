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

// The orders that the compiler listed lie in the image. Where the object's order is one of them and goes on with this very field, the object lists one key more of the same order:
// nothing is hashed, and there is no first time. The jump is taken with the new order word stored. Falls through, with nothing written, wherever that does not apply. Keeps object and slotKey.
static Jump stepInsideOrderOfImage(StubAssembler& jit, const KeyOrderRegisters& r)
{
    const GPRReg layout = r.scratch;
    const GPRReg limit = jit.scratchRegister();
    RELEASE_ASSERT(noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch, limit));
    static_assert(sizeof(KeyOrderData) == sizeof(uint16_t) && !OBJECT_OFFSETOF(KeyOrderData, length) && sizeof(FieldData) == 1 << 4);
    static_assert(OBJECT_OFFSETOF(FieldData, flags) == sizeof(uint32_t) && std::endian::native == std::endian::little);
    const Address tables(instanceGPR, AOT_INSTANCE_OFFSET(image.tables));
    JumpList doesNotApply;

    jit.load32(Address(r.object, offsetOfOrderWord), r.words);
    doesNotApply.append(jit.branchTest32(CCallHelpers::NonZero, r.words, TrustedImm32(static_cast<int32_t>(~orderAndCountMask))));
    loadStructure(jit, r.object, layout);
    jit.loadPtr(Address(layout, CompiledStructure::offsetOfLayout()), layout);
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
    // An entry from the number of fields on counts extra keys.
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
    jit.add32(TrustedImm32(1 << orderWordCountShift), Address(r.object, offsetOfOrderWord));
    Jump isListed = jit.jump();

    doesNotApply.link(&jit);
    return isListed;
}

// The same for whoever knows the layout and the index of the field in it.
static Jump stepInsideOrderOfImage(StubAssembler& jit, const KeyOrderRegisters& r, GPRReg layout, GPRReg fieldIndex)
{
    RELEASE_ASSERT(noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch, layout, fieldIndex));
    const Address tables(instanceGPR, AOT_INSTANCE_OFFSET(image.tables));
    JumpList doesNotApply;

    jit.load32(Address(r.object, offsetOfOrderWord), r.words);
    doesNotApply.append(jit.branchTest32(CCallHelpers::NonZero, r.words, TrustedImm32(static_cast<int32_t>(~orderAndCountMask))));
    jit.and32(TrustedImm32(orderWordOrderMask), r.words, r.entry);
    doesNotApply.append(jit.branch32(CCallHelpers::AboveOrEqual, r.entry, Address(layout, OBJECT_OFFSETOF(LayoutData, numberOfOrders))));
    jit.lshift32(TrustedImm32(2), r.entry);
    jit.add32(Address(layout, OBJECT_OFFSETOF(LayoutData, ordersOffset)), r.entry);
    jit.addPtr(tables, r.entry);
    jit.load32(Address(r.entry), r.entry);
    jit.addPtr(tables, r.entry);
    jit.urshift32(TrustedImm32(orderWordCountShift), r.words);
    jit.load16(Address(r.entry), r.scratch);
    doesNotApply.append(jit.branch32(CCallHelpers::AboveOrEqual, r.words, r.scratch));
    jit.load16(BaseIndex(r.entry, r.words, CCallHelpers::TimesTwo, sizeof(KeyOrderData)), r.entry);
    doesNotApply.append(jit.branch32(CCallHelpers::NotEqual, r.entry, fieldIndex));
    jit.add32(TrustedImm32(1 << orderWordCountShift), Address(r.object, offsetOfOrderWord));
    Jump isListed = jit.jump();

    doesNotApply.link(&jit);
    return isListed;
}

static void appendFieldToKeyOrderBehind(StubAssembler&, const KeyOrderRegisters&, Jump isListed, JumpList& hasNoTransition);

void appendFieldToKeyOrder(StubAssembler& jit, const KeyOrderRegisters& r, JumpList& hasNoTransition)
{
    appendFieldToKeyOrderBehind(jit, r, stepInsideOrderOfImage(jit, r), hasNoTransition);
}

void appendFieldToKeyOrder(StubAssembler& jit, const KeyOrderRegisters& r, GPRReg layout, GPRReg fieldIndex, JumpList& hasNoTransition)
{
    appendFieldToKeyOrderBehind(jit, r, stepInsideOrderOfImage(jit, r, layout, fieldIndex), hasNoTransition);
}

static void appendFieldToKeyOrderBehind(StubAssembler& jit, const KeyOrderRegisters& r, Jump isListed, JumpList& hasNoTransition)
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
    isListed.link(&jit);
}

namespace {

struct KeyOrderRunRegisters {
    GPRReg object;
    GPRReg slotKeys;
    GPRReg wordsBefore;
    GPRReg run;
    GPRReg scratch;
};

} // anonymous namespace

static void locateKeyOrderRun(StubAssembler& jit, const KeyOrderRunRegisters& r)
{
    static_assert(indexOfKeyOrderRun(3ull << 32 | 5, 9ull << 32 | 16) == (3 ^ 5 ^ 16));
    jit.urshift64(r.wordsBefore, TrustedImm32(32), r.run);
    jit.xor32(r.wordsBefore, r.run);
    jit.xor32(r.slotKeys, r.run);
    jit.and32(TrustedImm32(numberOfKeyOrderRuns - 1), r.run);
    jit.lshift32(TrustedImm32(logSizeOfKeyOrderRun), r.run);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.keyOrderRuns)), r.run);
}

// Goes on with finish where the run is remembered. Elsewhere wordsBefore holds what rememberKeyOrderRun takes: zero for an object with extra properties, whose runs are not remembered.
static void takeRememberedKeyOrderRun(StubAssembler& jit, const KeyOrderRunRegisters& r, const ScopedLambda<void()>& finish)
{
    RELEASE_ASSERT(noOverlap(r.object, r.slotKeys, r.wordsBefore, r.run, r.scratch));
    JumpList isNotRemembered;
    // An entry holds the two words without the flags of the order word. So the words of an object that has no flag set, as nearly every object, are compared as they are.
    JumpList mayHaveFlags;
    jit.load64(Address(r.object, offsetOfOrderWord), r.wordsBefore);
    locateKeyOrderRun(jit, r);
    mayHaveFlags.append(jit.branch64(CCallHelpers::NotEqual, Address(r.run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), r.wordsBefore));
    mayHaveFlags.append(jit.branch64(CCallHelpers::NotEqual, Address(r.run, KeyOrderRun::offsetOfSlotKeys()), r.slotKeys));
    jit.load32(Address(r.run, KeyOrderRun::offsetOfOrderAndCountAfter()), r.scratch);
    jit.store32(r.scratch, Address(r.object, offsetOfOrderWord));
    finish();

    mayHaveFlags.link(&jit);
    Jump hasExtraProperties = jit.branchTest32(CCallHelpers::NonZero, r.wordsBefore, TrustedImm32(orderWordHasExtraProperties));
    jit.move(r.wordsBefore, r.scratch);
    jit.and64(TrustedImm64(static_cast<int64_t>(orderCountAndShapeMask)), r.wordsBefore);
    locateKeyOrderRun(jit, r);
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(r.run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), r.wordsBefore));
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(r.run, KeyOrderRun::offsetOfSlotKeys()), r.slotKeys));
    jit.and32(TrustedImm32(static_cast<int32_t>(~orderAndCountMask)), r.scratch);
    jit.or32(Address(r.run, KeyOrderRun::offsetOfOrderAndCountAfter()), r.scratch);
    jit.store32(r.scratch, Address(r.object, offsetOfOrderWord));
    finish();

    hasExtraProperties.link(&jit);
    jit.move(TrustedImm32(0), r.wordsBefore);
    isNotRemembered.link(&jit);
}

static void rememberKeyOrderRun(StubAssembler& jit, const KeyOrderRunRegisters& r)
{
    Jump isNotRemembered = jit.branchTest64(CCallHelpers::Zero, r.wordsBefore);
    locateKeyOrderRun(jit, r);
    jit.store64(r.wordsBefore, Address(r.run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()));
    jit.store64(r.slotKeys, Address(r.run, KeyOrderRun::offsetOfSlotKeys()));
    jit.load32(Address(r.object, offsetOfOrderWord), r.scratch);
    jit.and32(TrustedImm32(static_cast<int32_t>(orderAndCountMask)), r.scratch);
    jit.store32(r.scratch, Address(r.run, KeyOrderRun::offsetOfOrderAndCountAfter()));
    isNotRemembered.link(&jit);
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

// Only a step between two numbered orders of an object without flags in its order word is remembered. A flag that is set stays set, so the word behind the step says it for both.
// wordsBefore: the order and shape words in front of the step, or zero for nothing to remember. Overwrites placePlusOne.
static void rememberKeyOrderStepAndReturn(StubAssembler& jit, GPRReg object, GPRReg wordsBefore, GPRReg placePlusOne, GPRReg orderWordAfter, GPRReg scratch)
{
    static_assert(sizeof(KeyOrderStepCache) == 1 << 4);
    static_assert(!OBJECT_OFFSETOF(KeyOrderStepCache, orderWordBefore) && OBJECT_OFFSETOF(KeyOrderStepCache, shapeWord) == sizeof(uint32_t));
    RELEASE_ASSERT(noOverlap(object, wordsBefore, placePlusOne, orderWordAfter, scratch));
    JumpList done;
    done.append(jit.branchTest64(CCallHelpers::Zero, wordsBefore));
    jit.load32(Address(object, offsetOfOrderWord), orderWordAfter);
    jit.or32(wordsBefore, orderWordAfter, scratch);
    done.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(static_cast<int32_t>(~orderAndCountMask))));
    for (GPRReg orderWord : { wordsBefore, orderWordAfter }) {
        jit.and32(TrustedImm32(static_cast<int32_t>(orderWordOrderMask)), orderWord, scratch);
        done.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(static_cast<int32_t>(orderStoredOutOfLine))));
    }
    jit.sub32(TrustedImm32(1), placePlusOne);
    jit.and32(TrustedImm32(static_cast<int32_t>(numberOfKeyOrderStepCaches - 1)), placePlusOne, scratch);
    jit.lshift32(TrustedImm32(4), scratch);
    jit.addPtr(instanceGPR, scratch);
    jit.urshift32(TrustedImm32(logNumberOfKeyOrderStepCaches), placePlusOne);
    jit.add32(TrustedImm32(1), placePlusOne);
    constexpr int32_t offsetOfSteps = InstanceLayout::offsetOfKeyOrderSteps;
    jit.store64(wordsBefore, Address(scratch, offsetOfSteps + OBJECT_OFFSETOF(KeyOrderStepCache, orderWordBefore)));
    jit.store32(orderWordAfter, Address(scratch, offsetOfSteps + OBJECT_OFFSETOF(KeyOrderStepCache, orderWordAfter)));
    jit.store32(placePlusOne, Address(scratch, offsetOfSteps + OBJECT_OFFSETOF(KeyOrderStepCache, turnPlusOne)));
    done.link(&jit);
    jit.returnToCaller();
}

static void generateSingleCreation(StubAssembler& jit, bool ensuresStorage, bool remembersStep = false)
{
    RELEASE_ASSERT(!ensuresStorage || !remembersStep);
    const GPRReg object = jit.operand(0);
    const GPRReg slotKey = jit.operand(1);
    const GPRReg words = jit.allocateScratch(StubRegister::T9);
    const GPRReg entry = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg wordsBefore = jit.allocateScratch(StubRegister::T10);
    const KeyOrderRunRegisters run { object, slotKey, wordsBefore, entry, scratch };

    JumpList isListed;
    takeRememberedKeyOrderRun(jit, run, [&] {
        if (ensuresStorage || remembersStep)
            isListed.append(jit.jump());
        else
            jit.returnToCaller();
    });

    JumpList hasNoTransition;
    appendFieldToKeyOrder(jit, { object, slotKey, entry, words, scratch }, hasNoTransition);
    rememberKeyOrderRun(jit, run);
    isListed.link(&jit);
    if (ensuresStorage)
        ensureStorageAndReturn(jit, object, jit.operand(2), words, entry);
    else if (remembersStep)
        rememberKeyOrderStepAndReturn(jit, object, wordsBefore, jit.clobberOperand(2), entry, scratch);
    else
        jit.returnToCaller();

    hasNoTransition.link(&jit);
    jit.exitOnMiss(ensuresStorage ? Entry::operationAOTDidCreateFieldOutOfLineAt : Entry::operationAOTDidCreateFieldAt, voidOperationCall);
}

static void generateRunOfCreations(StubAssembler& jit, bool remembersStep = false)
{
    const GPRReg object = jit.operand(0);
    const GPRReg remainingKeys = jit.clobberOperand(1);
    const GPRReg words = jit.allocateScratch(StubRegister::T9);
    const GPRReg entry = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg slotKey = jit.allocateScratch(StubRegister::T13);
    const GPRReg wordsBefore = jit.allocateScratch(StubRegister::T10);
    const KeyOrderRunRegisters run { object, remainingKeys, wordsBefore, entry, scratch };

    JumpList isListed;
    takeRememberedKeyOrderRun(jit, run, [&] {
        if (remembersStep)
            isListed.append(jit.jump());
        else
            jit.returnToCaller();
    });

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
#if CPU(ARM64)
    jit.move(allKeys, remainingKeys);
#else
    jit.pop(remainingKeys);
#endif
    rememberKeyOrderRun(jit, run);
    isListed.link(&jit);
    if (remembersStep)
        rememberKeyOrderStepAndReturn(jit, object, wordsBefore, jit.clobberOperand(2), entry, scratch);
    else
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

// The third operand: the number of the place plus one. Where the run time has to be asked the step is not remembered: the next object at the place finds the answer in this stub, and then it is.
StubSignature signatureOfDidCreateFieldAtPlace(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    signature.addOperand(gprOf(StubRegister::R0));
    return signature;
}

void generateDidCreateFieldAtPlace(StubAssembler& jit)
{
    switch (static_cast<CreatedFieldsAtPlace>(jit.key().form())) {
    case CreatedFieldsAtPlace::One:
        generateSingleCreation(jit, false, true);
        return;
    case CreatedFieldsAtPlace::Run:
        generateRunOfCreations(jit, true);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfRememberKeyOrderStep(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    signature.addOperand(gprOf(StubRegister::R0));
    return signature;
}

// The operands: the object behind the step, its order and shape words in front of the step, and the number of the place plus one.
void generateRememberKeyOrderStep(StubAssembler& jit)
{
    const GPRReg object = jit.operand(0);
    const GPRReg wordsBefore = jit.operand(1);
    const GPRReg placePlusOne = jit.clobberOperand(2);
    const GPRReg orderWordAfter = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    rememberKeyOrderStepAndReturn(jit, object, wordsBefore, placePlusOne, orderWordAfter, scratch);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
