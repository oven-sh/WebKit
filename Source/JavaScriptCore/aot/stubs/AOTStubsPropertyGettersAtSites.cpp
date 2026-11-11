/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeTable.h"
#include "GetterSetter.h"
#include "JSCell.h"
#include "JSObject.h"

namespace JSC { namespace AOT {

namespace {

class AnswersOfGetterStub {
public:
    AnswersOfGetterStub(StubAssembler& jit, GPRReg distance, GPRReg place)
        : m_place(place)
    {
        static_assert(InstanceLayout::sizeOfIndexOfSiteAnswers == sizeof(uint16_t));
        jit.load16(BaseIndex(instanceGPR, distance, CCallHelpers::TimesOne), place);
#if !CPU(X86_64)
        jit.getEffectiveAddress(BaseIndex(instanceGPR, place, CCallHelpers::TimesEight), place);
        jit.addPtr(TrustedImm32(static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers)), place);
#endif
    }
#if CPU(X86_64)
    using Word = CCallHelpers::BaseIndex;
    Word word(ptrdiff_t offset) const { return Word(instanceGPR, m_place, CCallHelpers::TimesEight, static_cast<int32_t>(InstanceLayout::offsetOfSiteAnswers + offset)); }
#else
    using Word = CCallHelpers::Address;
    Word word(ptrdiff_t offset) const { return Word(m_place, static_cast<int32_t>(offset)); }
#endif

    Jump loadOperandUnlessKeyDiffers(StubAssembler& jit, GPRReg key, GPRReg operand, [[maybe_unused]] GPRReg keyOfAnswer) const
    {
#if CPU(X86_64)
        Jump differs = jit.branch32(CCallHelpers::NotEqual, word(offsetOfAnswer), key);
        jit.load32(word(offsetOfAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
        return differs;
#else
        RELEASE_ASSERT(noOverlap(key, operand, keyOfAnswer));
        jit.loadPair32(m_place, TrustedImm32(static_cast<int32_t>(offsetOfAnswer)), keyOfAnswer, operand);
        return jit.branch32(CCallHelpers::NotEqual, key, keyOfAnswer);
#endif
    }

    Jump loadFormAndOperandUnlessStructureDiffers(StubAssembler& jit, GPRReg key, GPRReg operand, [[maybe_unused]] GPRReg keyOfAnswer) const
    {
#if CPU(X86_64)
        jit.xor32(word(offsetOfAnswer), key);
        Jump differs = jit.branch32(CCallHelpers::Above, key, TrustedImm32(SiteAnswers::formMask));
        jit.load32(word(offsetOfAnswer + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)), operand);
        return differs;
#else
        jit.loadPair32(m_place, TrustedImm32(static_cast<int32_t>(offsetOfAnswer)), keyOfAnswer, operand);
        jit.xor32(keyOfAnswer, key);
        return jit.branch32(CCallHelpers::Above, key, TrustedImm32(SiteAnswers::formMask));
#endif
    }

    Word operandBorrowedOfOtherAnswer() const { return word(OBJECT_OFFSETOF(SiteAnswers, second) + OBJECT_OFFSETOF(SiteAnswers::Answer, operand)); }

private:
    static constexpr ptrdiff_t offsetOfAnswer = OBJECT_OFFSETOF(SiteAnswers, first);

    GPRReg m_place;
};

} // anonymous namespace

static void loadNameOfPlaceOfGetterOrMark(StubAssembler& jit, GPRReg distance, GPRReg name, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(distance, scratch) && noOverlap(name, scratch));
    static_assert(sizeof(uint16_t) == InstanceLayout::sizeOfIndexOfSiteAnswers && RowDisplacementTable::maxColumn + 1 == InstanceLayout::nameOfSiteStandsBehind);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.namesOfSitesByDistance)), scratch);
    if (name == distance) {
        jit.addPtr(distance, scratch);
        jit.load16(Address(scratch), name);
        return;
    }
    jit.load16(BaseIndex(scratch, distance, CCallHelpers::TimesOne), name);
}

static void loadNameBehindMarkOfPlaceOfGetter(StubAssembler& jit, GPRReg distance, GPRReg name, GPRReg scratch)
{
    if (name == distance)
        jit.load32(Address(scratch, sizeof(uint16_t)), name);
    else
        jit.load32(BaseIndex(scratch, distance, CCallHelpers::TimesOne, sizeof(uint16_t)), name);
}

static void loadNameOfPlaceOfGetter(StubAssembler& jit, GPRReg distance, GPRReg name, GPRReg scratch)
{
    loadNameOfPlaceOfGetterOrMark(jit, distance, name, scratch);
    Jump standsThere = jit.branch32(CCallHelpers::BelowOrEqual, name, TrustedImm32(RowDisplacementTable::maxColumn));
    loadNameBehindMarkOfPlaceOfGetter(jit, distance, name, scratch);
    standsThere.link(&jit);
}

static void fillAnswersOfPlaceOfGetter(StubAssembler& jit, GPRReg object, GPRReg name, GPRReg distance)
{
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        const GPRReg A3 = jit.reuseScratch(StubRegister::A3);
        const GPRReg A4 = jit.reuseScratch(StubRegister::A4);
        RELEASE_ASSERT(noOverlap(A3, name, object) && A2 != object);
        jit.move(distance, A3);
        jit.move(name, A2);
        jit.move(object, A1);
        jit.move(instanceGPR, A0);
        jit.move(TrustedImm32(static_cast<int32_t>(SiteUse::Call)), A4);
        jit.callOperation(Entry::operationAOTFillSiteAnswers, jit.reuseScratch(StubRegister::Operation));
    });
}

StubSignature signatureOfGetterAtSite(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(firstStubOperandRegister) });
    signature.immediate = gprOf(shortStubImmediateRegister);
#if CPU(X86_64)
    return signature.fillingKeptRegisterInFront(X86Registers::edx);
#else
    return signature;
#endif
}

void generateGetterAtSite(StubAssembler& jit)
{
    constexpr SiteUse use = SiteUse::Call;
    using Form = SiteAnswers::Form;
    const GPRReg object = jit.operand(0);
    const GPRReg result = jit.result();
    const GPRReg distance = jit.immediate();
#if CPU(X86_64)
    const GPRReg place = jit.allocateScratch(StubRegister::T11);
    const GPRReg key = jit.allocateScratch(StubRegister::T13);
    const GPRReg operand = jit.allocateScratch(StubRegister::T14);
    const GPRReg keyOfAnswer = InvalidGPRReg;
    const GPRReg nameBehindMiss = key;
    const GPRReg scratchBehindMiss = place;
#else
    const GPRReg key = jit.allocateScratch(StubRegister::T11);
    const GPRReg operand = jit.allocateScratch(StubRegister::T13);
    const GPRReg keyOfAnswer = jit.allocateScratch(StubRegister::T12);
    const GPRReg place = jit.allocateScratch(StubRegister::T14);
    const GPRReg nameBehindMiss = operand;
    const GPRReg scratchBehindMiss = key;
#endif
    RELEASE_ASSERT(noOverlap(object, distance, place, key, operand));
    JumpList isNoPair;
    auto returnGetterOf = [&](GPRReg getterSetter) {
        branchUnlessAccessorPair(jit, getterSetter, isNoPair);
        jit.loadPtr(Address(getterSetter, GetterSetter::offsetOfGetter()), result);
        jit.returnToCaller();
    };
    auto branchUnlessForm = [&](Form form) {
        return jit.branch32(CCallHelpers::NotEqual, key, TrustedImm32(static_cast<int32_t>(SiteAnswers::bitsOf(use, form))));
    };

    constexpr int32_t bitsOfCommonForm = static_cast<int32_t>(SiteAnswers::bitsOf(use, Form::OwnGetterInStorage));
    const AnswersOfGetterStub answers(jit, distance, place);
    jit.load32(Address(object, JSCell::structureIDOffset()), key);
    jit.or32(TrustedImm32(bitsOfCommonForm), key);
    Jump hasOtherKey = answers.loadOperandUnlessKeyDiffers(jit, key, operand, keyOfAnswer);
    jit.loadPtr(Address(object, JSObject::butterflyOffset()), key);
    jit.signExtend32ToPtr(operand, operand);
    jit.loadPtr(BaseIndex(key, operand, CCallHelpers::TimesEight), key);
    returnGetterOf(key);

    hasOtherKey.link(&jit);
    jit.xor32(TrustedImm32(bitsOfCommonForm), key);
    Jump isNotAnswered = answers.loadFormAndOperandUnlessStructureDiffers(jit, key, operand, keyOfAnswer);
    Jump isNotAbove = branchUnlessForm(Form::GetterInWordOfCage);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight), key);
    returnGetterOf(key);

    isNotAbove.link(&jit);
    Jump isNotInStorageOfHolder = branchUnlessForm(Form::GetterInStorageOfHolder);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, operand, CCallHelpers::TimesEight, JSObject::butterflyOffset()), operand);
    jit.load32(answers.operandBorrowedOfOtherAnswer(), key);
    jit.signExtend32ToPtr(key, key);
    jit.loadPtr(BaseIndex(operand, key, CCallHelpers::TimesEight), key);
    returnGetterOf(key);

    isNotInStorageOfHolder.link(&jit);
    Jump isNotInCell = branchUnlessForm(Form::OwnGetterInCell);
    jit.loadPtr(BaseIndex(object, operand, CCallHelpers::TimesOne), key);
    returnGetterOf(key);

    auto loadExtraProperties = [&] {
        jit.zeroExtend8To32(operand, key);
        jit.load32(BaseIndex(object, key, CCallHelpers::TimesFour), key);
        isNoPair.append(jit.branchTest32(CCallHelpers::Zero, key));
        jit.rshift32(TrustedImm32(SiteAnswers::indexShiftInOperandOfExtraValue), operand);
    };
    isNotInCell.link(&jit);
    Jump isNotExtraInCell = branchUnlessForm(Form::ExtraGetterInCellAtCall);
    loadExtraProperties();
    jit.add32(operand, key);
    jit.loadPtr(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight), key);
    returnGetterOf(key);

    isNotExtraInCell.link(&jit);
    Jump isNoGetter = branchUnlessForm(Form::ExtraGetterInStorageAtCall);
    loadExtraProperties();
    jit.loadPtr(BaseIndex(cellCageBaseGPR, key, CCallHelpers::TimesEight, JSObject::butterflyOffset()), key);
    jit.signExtend32ToPtr(operand, operand);
    jit.loadPtr(BaseIndex(key, operand, CCallHelpers::TimesEight), key);
    returnGetterOf(key);

    isNoGetter.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);
    jit.returnToCaller();

    isNotAnswered.link(&jit);
    isNoPair.link(&jit);
    RELEASE_ASSERT(noOverlap(object, distance, nameBehindMiss, scratchBehindMiss, instanceGPR));
    JumpList isSample;
    countMissOfPlace(jit, distance, scratchBehindMiss, isSample);
    Label isCounted = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) mutable {
        isSample.link(&jit);
        goOnWhileSamplesOfPlaceAreLeftOut(jit, scratchBehindMiss, isCounted);
        loadNameOfPlaceOfGetter(jit, distance, nameBehindMiss, scratchBehindMiss);
        fillAnswersOfPlaceOfGetter(jit, object, nameBehindMiss, distance);
        jit.jump().linkTo(isCounted, &jit);
    });
    checkStubOperands(getterByAccessTable(), { object });
    loadNameOfPlaceOfGetter(jit, distance, distance == gprOf(stubImmediateRegister) ? distance : jit.allocateScratch(stubImmediateRegister), scratchBehindMiss);
    jit.jumpToStub(getterByAccessTable());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
