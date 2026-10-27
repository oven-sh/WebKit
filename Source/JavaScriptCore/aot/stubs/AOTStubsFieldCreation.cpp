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

// The orders that the compiler listed lie in the image. Whoever knows the layout and the index of the field in it saves the way to both.
static Jump stepInsideOrderOfImage(StubAssembler& jit, const KeyOrderRegisters& r, std::optional<FieldOfLayout> known)
{
    const GPRReg layout = known ? known->layout : r.scratch;
    const GPRReg limit = known ? r.scratch : jit.scratchRegister();
    RELEASE_ASSERT(known ? noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch, layout, known->fieldIndex) : noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch, limit));
    static_assert(sizeof(KeyOrderData) == sizeof(uint16_t) && !OBJECT_OFFSETOF(KeyOrderData, length) && sizeof(FieldData) == 1 << 4);
    static_assert(OBJECT_OFFSETOF(FieldData, flags) == sizeof(uint32_t) && std::endian::native == std::endian::little);
    const Address tables(instanceGPR, AOT_INSTANCE_OFFSET(image.tables));
    JumpList doesNotApply;

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
    }
    jit.add32(TrustedImm32(1 << orderWordCountShift), Address(r.object, offsetOfOrderWord));
    Jump isListed = jit.jump();

    doesNotApply.link(&jit);
    return isListed;
}

void appendFieldToKeyOrder(StubAssembler& jit, const KeyOrderRegisters& r, JumpList& hasNoTransition, std::optional<FieldOfLayout> known)
{
    RELEASE_ASSERT(noOverlap(r.object, r.slotKey, r.entry, r.words, r.scratch));
    Jump isListed = stepInsideOrderOfImage(jit, r, known);
    const GPRReg transition = r.scratch;
    const Address wordsBefore(transition, KeyOrderTransition::offsetOfOrderAndShapeWordsBefore());

    jit.load64(Address(r.object, offsetOfOrderWord), r.words);
    Jump hasNoExtraProperties = jit.branchTest32(CCallHelpers::Zero, r.words, TrustedImm32(orderWordHasExtraProperties));
    loadStructure(jit, r.object, r.scratch);
    // What is cached about the objects below a prototype relies on the run time's report of a field that the prototype gets.
    hasNoTransition.append(jit.branchTest32(CCallHelpers::NonZero, Address(r.scratch, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    LayoutData hasNoOrderBits { };
    hasNoOrderBits.keysAreNeverEnumerated = true;
    jit.loadPtr(Address(r.scratch, CompiledStructure::offsetOfLayout()), r.entry);
    Jump hasNoOrder = jit.branchTest32(CCallHelpers::NonZero, Address(r.entry), TrustedImm32(static_cast<int32_t>(hasNoOrderBits.firstWord())));
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
    hasNoOrder.link(&jit);
}

void listCreatedFieldAndReturn(StubAssembler& jit, const KeyOrderRegisters& r, GPRReg value, std::optional<FieldOfLayout> known)
{
    JumpList hasNoTransition;
    appendFieldToKeyOrder(jit, r, hasNoTransition, known);
    returnAfterStoring(jit, r.object, value);

    // The slot key says where the slot is. The storage is there by now, or the field needs none.
    hasNoTransition.link(&jit);
    OperationCall didCreateField = voidOperationCall;
    didCreateField.secondArgumentIn = r.object;
    didCreateField.thirdArgumentIn = r.slotKey;
    jit.callOperationAndReturn(Entry::operationAOTDidCreateFieldAt, didCreateField);
}

void createGranuleOfField(StubAssembler& jit, const FieldStoreRegisters& r, const ScopedLambda<void()>& loadOffsetOfExtensionWord, Label findStorage)
{
    const GPRReg object = r.field.object;
    const GPRReg granule = r.field.representation;
    const GPRReg granuleBit = r.secondScratch;
    const GPRReg extension = r.field.address;
    JumpList cannotAppend;
    jit.and32(TrustedImm32(SlotData::maxOffset), r.field.slotData, granule);
    jit.urshift32(TrustedImm32(3), granule);
    jit.sub32(TrustedImm32(1), granule);
    jit.move(TrustedImm32(1), granuleBit);
    jit.lshift64(granule, granuleBit);
    static_assert(granuleOf(offsetOfFirstExtensionSlot) == offsetOfFirstExtensionSlot / 8 - 1 && extensionGranuleSize == 8);
    loadOffsetOfExtensionWord();
    if (jit.options().useWideReferences) {
        jit.load64(BaseIndex(object, r.field.extensionWordOffset, CCallHelpers::TimesOne), extension);
        cannotAppend.append(jit.branchTest64(CCallHelpers::Zero, extension));
    } else {
        jit.load32(BaseIndex(object, r.field.extensionWordOffset, CCallHelpers::TimesOne), extension);
        cannotAppend.append(jit.branchTest32(CCallHelpers::Zero, extension));
        decodeCell32(jit, extension, extension);
        jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), extension);
    }
    appendGranule(jit, extension, granuleBit, granule, jit.scratchRegister(), cannotAppend);
    jit.jump().linkTo(findStorage, &jit);

    cannotAppend.link(&jit);
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        RELEASE_ASSERT(object != A2 && noOverlap(instanceGPR, A1, A2));
        jit.move(granuleBit, A2);
        jit.move(object, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTEnsureExtensionGranules, jit.reuseScratch(StubRegister::Operation));
    });
    jit.jump().linkTo(findStorage, &jit);
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

// Only a step between two numbered orders of an object without flags in its order word is remembered.
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
    static_assert(indexOfKeyOrderStepCache(5, 9ull << 32) == 5 + (9 << 5));
    jit.urshift64(wordsBefore, TrustedImm32(keyOrderStepCacheShift), scratch);
    jit.add32(placePlusOne, scratch);
    jit.and32(TrustedImm32(static_cast<int32_t>(numberOfKeyOrderStepCaches - 1)), scratch);
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

enum class AfterCreation : uint8_t { Returns, EnsuresStorage, RemembersStep };

// The second operand is one slot key, or a run of them with the first one in the lowest bits.
static void generateCreation(StubAssembler& jit, bool isRun, AfterCreation after)
{
    RELEASE_ASSERT(!isRun || after != AfterCreation::EnsuresStorage);
    const GPRReg object = jit.operand(0);
    const GPRReg keys = isRun ? jit.clobberOperand(1) : jit.operand(1);
    const GPRReg words = jit.allocateScratch(StubRegister::T9);
    const GPRReg entry = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg slotKey = isRun ? jit.allocateScratch(StubRegister::T13) : keys;
    const GPRReg wordsBefore = jit.allocateScratch(StubRegister::T10);
    const GPRReg run = entry;
    const KeyOrderRegisters keyOrder { object, slotKey, entry, words, scratch };
    JumpList isListed;
    JumpList hasNoTransition;

    auto locateKeyOrderRun = [&] {
        static_assert(indexOfKeyOrderRun(3ull << 32 | 5, 9ull << 32 | 16) == (3 ^ 5 ^ 16));
        jit.urshift64(wordsBefore, TrustedImm32(32), run);
        jit.xor32(wordsBefore, run);
        jit.xor32(keys, run);
        jit.and32(TrustedImm32(numberOfKeyOrderRuns - 1), run);
        jit.lshift32(TrustedImm32(logSizeOfKeyOrderRun), run);
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.keyOrderRuns)), run);
    };
    auto finishListedRun = [&] {
        if (after == AfterCreation::Returns)
            jit.returnToCaller();
        else
            isListed.append(jit.jump());
    };

    // A run that is remembered is taken. Elsewhere wordsBefore holds what is remembered below: zero for an object with extra properties, whose runs are not remembered.
    RELEASE_ASSERT(noOverlap(object, keys, wordsBefore, run, scratch));
    JumpList isNotRemembered;
    // An entry holds the two words without the flags of the order word. So the words of an object that has no flag set, as nearly every object, are compared as they are.
    JumpList mayHaveFlags;
    jit.load64(Address(object, offsetOfOrderWord), wordsBefore);
    locateKeyOrderRun();
    mayHaveFlags.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), wordsBefore));
    mayHaveFlags.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfSlotKeys()), keys));
    jit.load32(Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()), scratch);
    jit.store32(scratch, Address(object, offsetOfOrderWord));
    finishListedRun();

    mayHaveFlags.link(&jit);
    Jump hasExtraProperties = jit.branchTest32(CCallHelpers::NonZero, wordsBefore, TrustedImm32(orderWordHasExtraProperties));
    jit.move(wordsBefore, scratch);
    jit.and64(TrustedImm64(static_cast<int64_t>(orderCountAndShapeMask)), wordsBefore);
    locateKeyOrderRun();
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()), wordsBefore));
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, Address(run, KeyOrderRun::offsetOfSlotKeys()), keys));
    jit.and32(TrustedImm32(static_cast<int32_t>(~orderAndCountMask)), scratch);
    jit.or32(Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()), scratch);
    jit.store32(scratch, Address(object, offsetOfOrderWord));
    finishListedRun();

    hasExtraProperties.link(&jit);
    jit.move(TrustedImm32(0), wordsBefore);
    isNotRemembered.link(&jit);

#if CPU(ARM64)
    const GPRReg allKeys = isRun ? jit.allocateScratch(StubRegister::T15) : InvalidGPRReg;
#endif
    auto restoreKeys = [&] {
#if CPU(ARM64)
        jit.move(allKeys, keys);
#else
        jit.pop(keys);
#endif
    };
    if (!isRun)
        appendFieldToKeyOrder(jit, keyOrder, hasNoTransition);
    else {
#if CPU(ARM64)
        jit.move(keys, allKeys);
#else
        jit.push(keys);
#endif
        Label nextKey = jit.label();
        jit.and32(TrustedImm32(slotKeyMask), keys, slotKey);
        appendFieldToKeyOrder(jit, keyOrder, hasNoTransition);
        jit.urshift64(TrustedImm32(bitsPerSlotKey), keys);
        jit.branchTest64(CCallHelpers::NonZero, keys).linkTo(nextKey, &jit);
        restoreKeys();
    }

    Jump isNotToBeRemembered = jit.branchTest64(CCallHelpers::Zero, wordsBefore);
    locateKeyOrderRun();
    jit.store64(wordsBefore, Address(run, KeyOrderRun::offsetOfOrderAndShapeWordsBefore()));
    jit.store64(keys, Address(run, KeyOrderRun::offsetOfSlotKeys()));
    jit.load32(Address(object, offsetOfOrderWord), scratch);
    jit.and32(TrustedImm32(static_cast<int32_t>(orderAndCountMask)), scratch);
    jit.store32(scratch, Address(run, KeyOrderRun::offsetOfOrderAndCountAfter()));
    isNotToBeRemembered.link(&jit);

    isListed.link(&jit);
    if (after == AfterCreation::EnsuresStorage)
        ensureStorageAndReturn(jit, object, jit.operand(2), words, entry);
    else if (after == AfterCreation::RemembersStep)
        rememberKeyOrderStepAndReturn(jit, object, wordsBefore, jit.clobberOperand(2), entry, scratch);
    else
        jit.returnToCaller();

    hasNoTransition.link(&jit);
    if (isRun) {
        restoreKeys();
        jit.exitOnMiss(Entry::operationAOTDidCreateFieldsAt, voidOperationCall);
    } else
        jit.exitOnMiss(after == AfterCreation::EnsuresStorage ? Entry::operationAOTDidCreateFieldOutOfLineAt : Entry::operationAOTDidCreateFieldAt, voidOperationCall);
}

// The third operand: the number of the place plus one. Where the run time has to be asked the step is not remembered: the next object at the place finds the answer in this stub, and then it is.
StubSignature signatureOfDidCreateFieldAtPlace(StubKey)
{
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::R0) });
}

StubSignature signatureOfDidCreateField(StubKey key)
{
    const CreatedFields fields = static_cast<CreatedFields>(key.form());
    RELEASE_ASSERT(fields <= CreatedFields::Run);
    if (fields == CreatedFields::OneOutOfLine)
        return signatureOfDidCreateFieldAtPlace(key);
    return StubSignature::withoutResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) });
}

void generateDidCreateField(StubAssembler& jit)
{
    const CreatedFields fields = static_cast<CreatedFields>(jit.key().form());
    RELEASE_ASSERT(fields <= CreatedFields::Run);
    generateCreation(jit, fields == CreatedFields::Run, fields == CreatedFields::OneOutOfLine ? AfterCreation::EnsuresStorage : AfterCreation::Returns);
}

void generateDidCreateFieldAtPlace(StubAssembler& jit)
{
    const CreatedFieldsAtPlace fields = static_cast<CreatedFieldsAtPlace>(jit.key().form());
    RELEASE_ASSERT(fields <= CreatedFieldsAtPlace::Run);
    generateCreation(jit, fields == CreatedFieldsAtPlace::Run, AfterCreation::RemembersStep);
}

StubSignature signatureOfRememberKeyOrderStep(StubKey key)
{
    return signatureOfDidCreateFieldAtPlace(key);
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
