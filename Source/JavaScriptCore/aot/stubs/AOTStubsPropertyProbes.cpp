/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTFieldTable.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTModuleNamespace.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeTable.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsConstants.h"
#include "AOTStubsField.h"
#include "AOTStubsKeyed.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "CompleteSubspace.h"
#include "JSCell.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include <array>
#include <bit>
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

static constexpr unsigned locationBitsInCell = 8 * sizeof(PackedSlot::location);
static constexpr unsigned kindShiftInCell = 8 * PackedSlot::offsetOfBiasedKindAndFlags();
static constexpr unsigned nameShiftInCell = 8 * PackedSlot::offsetOfName();
static constexpr unsigned secondPayloadWordShift = 8 * (Slot::offsetOfSecondPayloadWord() - Slot::offsetOfPayload());
static constexpr int32_t staticsGranuleMask = InstanceLayout::granuleBelowInstance - 1;

static_assert(!PackedSlot::offsetOfPayload() && locationShiftInCell == 8 * sizeof(PackedSlot::payload) && secondPayloadWordShift == 8 * sizeof(uint32_t));
static_assert(PackedSlot::upperHalfForOwnInlineValue(1) == 1u << (nameShiftInCell - locationShiftInCell));
static_assert(sizeof(PackedSlot) == 1u << CCallHelpers::TimesEight && sizeof(void*) == 1u << CCallHelpers::TimesEight);
static_assert(StructureRowWords::wordShift == CCallHelpers::TimesFour && HashedSlot::uidHashShift <= CCallHelpers::TimesEight);
static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t));
static_assert(!static_cast<unsigned>(SlotAccess::Read));
static_assert(numberOfSlotKinds <= 8 * sizeof(uint32_t));
static_assert(hasOneBitSet(initialOutOfLineCapacity) && hasOneBitSet(InstanceLayout::granuleBelowInstance));

static constexpr uint32_t computedStorageCapacity(int32_t wordIndex)
{
    uint32_t lastIndex = static_cast<uint32_t>(-wordIndex - 2) | (initialOutOfLineCapacity - 1);
    return 1u << ((0u - clz(lastIndex)) & 31);
}

static_assert([] {
    for (int32_t wordIndex = std::numeric_limits<int8_t>::min(); wordIndex <= Slot::wordIndexFor(firstOutOfLineOffset); ++wordIndex) {
        if (computedStorageCapacity(wordIndex) != Slot::storageCapacityAfterAdding(wordIndex))
            return false;
    }
    return true;
}());

static void computeStorageCapacity(StubAssembler& jit, GPRReg wordIndexAndResult)
{
    jit.neg32(wordIndexAndResult);
    jit.sub32(TrustedImm32(2), wordIndexAndResult);
    jit.or32(TrustedImm32(initialOutOfLineCapacity - 1), wordIndexAndResult);
    jit.countLeadingZeros32(wordIndexAndResult, wordIndexAndResult);
    jit.neg32(wordIndexAndResult);
    jit.lshift32(TrustedImm32(1), wordIndexAndResult, wordIndexAndResult);
}

static constexpr uint32_t roundSizeOfStatics(uint32_t sizeOfStatics)
{
    return (sizeOfStatics + static_cast<uint32_t>(staticsGranuleMask)) & ~static_cast<uint32_t>(staticsGranuleMask);
}

static_assert(InstanceLayout::offsetOfStatics({ 70000, 0, 0 }) == -static_cast<ptrdiff_t>(roundSizeOfStatics(70000)) - static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForPrefixAndPollPage));
static_assert(InstanceLayout::offsetOfCards({ 70000, 0, 0 }) == InstanceLayout::offsetOfStatics({ 70000, 0, 0 }) - static_cast<ptrdiff_t>(roundSizeOfStatics(70000) >> InstanceLayout::cardShift));

static constexpr uint32_t kindsWithPresence(SlotPresence presence)
{
    uint32_t kinds = 0;
    for (Slot::Kind kind : allSlotKinds) {
        if (Slot::presenceOf(kind) == presence)
            kinds |= 1u << static_cast<unsigned>(kind);
    }
    return kinds;
}

static constexpr int32_t offsetOfFrontWords(SlotAccess access)
{
    return static_cast<int32_t>(AOT_INSTANCE_OFFSET(propertyCaches) + PropertyCacheWords::offsetOfFront(access));
}

SlotHandlerOperands operandsOfSlotHandler(SlotHandlerTable table)
{
    const SlotHandlerTableTraits traits = traitsOf(table);
    const bool isStore = traits.operation != SlotOperation::Read;
    SlotHandlerOperands operands { gprOf(firstStubOperandRegister), gprOf(slotHeaderRegister) };
    if (traits.form == SlotForm::Slot)
        operands.payload = gprOf(slotPayloadRegister);
    switch (traits.origin) {
    case SlotOrigin::ById:
    case SlotOrigin::ByIdInStubFrame:
        operands.identifierNumber = gprOf(slotNameRegister);
        if (isStore)
            operands.value = gprOf(storedValueByIdRegister);
        break;
    case SlotOrigin::ByVal:
        operands.keyCell = gprOf(keyCellRegister);
        if (isStore)
            operands.value = gprOf(storedValueByValRegister);
        break;
    }
    return operands;
}

StubSignature signatureOfSlotHandler(SlotHandlerTable table)
{
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    StubSignature signature = isReadTable(table) ? StubSignature::withGPRResult({ }) : StubSignature::withoutResult({ });
    for (GPRReg operand : { operands.receiver, operands.header, operands.payload, operands.identifierNumber, operands.keyCell, operands.value }) {
        if (operand != InvalidGPRReg)
            signature.addOperand(operand);
    }
    return signature;
}

static void extractSignedBits(StubAssembler& jit, GPRReg source, unsigned shift, unsigned width, GPRReg result)
{
#if CPU(ARM64)
    jit.extractSignedBitfield64(source, TrustedImm32(shift), TrustedImm32(width), result);
#else
    jit.lshift64(source, TrustedImm32(64 - shift - width), result);
    jit.rshift64(TrustedImm32(64 - width), result);
#endif
}

static void joinPayloadWords(StubAssembler& jit, GPRReg firstWordSource, GPRReg secondWordAndResult)
{
    RELEASE_ASSERT(firstWordSource != secondWordAndResult);
    jit.lshift64(TrustedImm32(secondPayloadWordShift), secondWordAndResult);
#if CPU(ARM64)
    jit.addZeroExtend64(secondWordAndResult, firstWordSource, secondWordAndResult);
#else
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg firstWord = ownedScratch;
    jit.zeroExtend32ToWord(firstWordSource, firstWord);
    jit.or64(firstWord, secondWordAndResult);
#endif
}

static BaseIndex fieldTableEntry(const FieldTableProbe& probe)
{
    static_assert(sizeof(FieldTableEntry) == 1u << CCallHelpers::TimesFour);
    return BaseIndex(probe.table, probe.index, CCallHelpers::TimesFour);
}

static void locateFieldTableEntry(StubAssembler& jit, const FieldTableProbe& probe, JumpList& isNotCompiledObject)
{
    RELEASE_ASSERT(noOverlap(probe.name, probe.receiver, probe.index, probe.table));

    if (!probe.nameHasColumn)
        isNotCompiledObject.append(jit.branch32(CCallHelpers::Above, probe.name, TrustedImm32(jit.options().maxNameInFieldTable)));
    if (!probe.receiverIsCell)
        isNotCompiledObject.append(jit.branchIfNotCell(probe.receiver));
    if (loadsShapeAsHalfword(jit)) {
        if (!probe.receiverIsCompiledObject)
            isNotCompiledObject.append(jit.branchTest8(CCallHelpers::Zero, Address(probe.receiver, offsetOfShapeWord + 3), TrustedImm32(compiledObjectShapeBit >> 24)));
        loadShapeWordAsIndex(jit, probe.receiver, probe.index);
    } else {
        jit.load32SignedExtendTo64(Address(probe.receiver, offsetOfShapeWord), probe.index);
        if (!probe.receiverIsCompiledObject)
            isNotCompiledObject.append(jit.branchTest32(CCallHelpers::PositiveOrZero, probe.index));
        jit.or64(TrustedImm32(shapeWordIsUnmarked), probe.index);
    }
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldTableEntries)), probe.table);
    jit.load32(BaseIndex(probe.table, probe.index, CCallHelpers::TimesFour, offsetOfRowsByShapeIndex(jit)), probe.index);
    jit.add32(probe.name, probe.index);
}

void loadIndexInFieldTable(StubAssembler& jit, GPRReg object, GPRReg name, GPRReg index, GPRReg table, JumpList& hasNone)
{
    locateFieldTableEntry(jit, { object, name, index, table, false, true }, hasNone);
}
#if CPU(X86_64)
static Jump branchUnlessEntryHasNameKeepingIndex(StubAssembler& jit, const FieldTableProbe& probe)
{
    static_assert(FieldTableEntry::nameShift == 16);
    constexpr int32_t offsetOfName = FieldTableEntry::nameShift / 8;
    jit.m_assembler.cmpw_rm(probe.name, offsetOfName, probe.table, probe.index, CCallHelpers::TimesFour);
    return Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE));
}
#endif

static Jump branchUnlessEntryHasName(StubAssembler& jit, const FieldTableProbe& probe)
{
    static_assert(FieldTableEntry::nameShift == 16);
#if CPU(X86_64)
    Jump hasOtherName = branchUnlessEntryHasNameKeepingIndex(jit, probe);
    jit.load16(fieldTableEntry(probe), probe.index);
    return hasOtherName;
#else
    jit.load32(fieldTableEntry(probe), probe.index);
    jit.lshift32(probe.name, TrustedImm32(FieldTableEntry::nameShift), probe.table);
    jit.xor32(probe.table, probe.index);
    return jit.branch32(CCallHelpers::Above, probe.index, TrustedImm32(FieldTableEntry::maxFormAndOperand));
#endif
}

static Jump branchUnlessForm(StubAssembler& jit, GPRReg operand, FieldTableEntry::Form form)
{
    if (form != FieldTableEntry::Form::InlineValue)
        jit.sub32(TrustedImm32(1 << FieldTableEntry::formShift), operand);
    return jit.branch32(CCallHelpers::Above, operand, TrustedImm32(FieldTableEntry::maxOperand));
}

static void bindArrivalsWithFormAndOperand(StubAssembler& jit, const FieldTableProbe& probe)
{
    if (probe.hasFormAndOperand)
        probe.hasFormAndOperand->link(&jit);
}

static void locateOutOfLineValue(StubAssembler& jit, GPRReg receiver, GPRReg operand, GPRReg extension, JumpList& hasNoStorage)
{
    static_assert(offsetOfFirstExtensionSlot == extensionGranuleSize && extensionGranuleSize == sizeof(EncodedJSValue) && maxExtensionGranules == 64);
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg granuleBits = ownedScratch;
    RELEASE_ASSERT(noOverlap(receiver, operand, extension, granuleBits) && !jit.options().useWideReferences);
    jit.load32(Address(receiver, offsetOfFirstField), extension);
    hasNoStorage.append(jit.branchTest32(CCallHelpers::Zero, extension));
    decodeCell32(jit, extension, extension);
    jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), extension);
    jit.load64(Address(extension, offsetOfExtensionGranuleBits), granuleBits);
    jit.neg32(operand);
    jit.lshift64(operand, granuleBits);
    hasNoStorage.append(jit.branchTest64(CCallHelpers::PositiveOrZero, granuleBits));
    jit.countPopulation64(granuleBits, operand, CCallHelpers::fpTempRegister);
    jit.lshift32(TrustedImm32(WTF::fastLog2(static_cast<unsigned>(sizeof(EncodedJSValue)))), operand);
}

static void branchUnlessNameIsAbsent(StubAssembler& jit, const FieldTableProbe& probe, JumpList& mayBePresent, JumpList* isNotBelowObjectPrototype = nullptr)
{
    static_assert(std::endian::native == std::endian::little);
    const GPRReg structure = probe.index;
    const GPRReg names = probe.table;
    loadStructure(jit, probe.receiver, structure);
    (isNotBelowObjectPrototype ? *isNotBelowObjectPrototype : mayBePresent).append(jit.branchTest8(CCallHelpers::Zero, Address(structure, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesOfObjectPrototypes)), names);
    jit.urshift32(probe.name, TrustedImm32(6), structure);
    jit.load64(BaseIndex(names, structure, CCallHelpers::TimesEight), names);
    mayBePresent.append(branchTestBitAt(jit, CCallHelpers::NonZero, names, probe.name));
}

void readFieldByTable(StubAssembler& jit, const FieldTableProbe& probe, GPRReg result, JumpList& otherwise, const ScopedLambda<void()>& finish, const ScopedLambda<void()>* finishWhereAbsent)
{
    using Form = FieldTableEntry::Form;
    const GPRReg operand = probe.index;
    const GPRReg word = probe.table;
    JumpList slotIsEmpty;
    JumpList slotIsSilent;
    auto readValue = [&](auto slot, JumpList& isEmpty) {
#if CPU(X86_64)
        isEmpty.append(jit.branchTest64(CCallHelpers::Zero, slot));
        jit.load64(slot, result);
#else
        jit.load64(slot, word);
        isEmpty.append(jit.branchTest64(CCallHelpers::Zero, word));
        jit.move(word, result);
#endif
        finish();
    };

    locateFieldTableEntry(jit, probe, probe.isNotCompiledObject ? *probe.isNotCompiledObject : otherwise);
    Jump isNoField = branchUnlessEntryHasName(jit, probe);
    Jump isNotInlineValue = branchUnlessForm(jit, operand, Form::InlineValue);
    readValue(BaseIndex(probe.receiver, operand, CCallHelpers::TimesEight), slotIsEmpty);

    unsigned formsTakenOff = 0;
    auto branchUnlessFormIs = [&](Form form) {
        int32_t difference = static_cast<int32_t>(form) - static_cast<int32_t>(formsTakenOff);
        jit.sub32(TrustedImm32(difference * (1 << FieldTableEntry::formShift)), operand);
        formsTakenOff = static_cast<unsigned>(form);
        return jit.branch32(CCallHelpers::Above, operand, TrustedImm32(FieldTableEntry::maxOperand));
    };
    auto readOutOfLineValue = [&] {
        Jump isNotOutOfLineValue = branchUnlessFormIs(Form::OutOfLineValue);
        if (jit.options().useWideReferences)
            otherwise.append(jit.jump());
        else {
            locateOutOfLineValue(jit, probe.receiver, operand, word, slotIsEmpty);
            readValue(BaseIndex(word, operand, CCallHelpers::TimesOne), slotIsEmpty);
        }
        isNotOutOfLineValue.link(&jit);
    };
    auto readDouble = [&](int32_t firstDouble) {
        jit.load64(BaseIndex(probe.receiver, operand, CCallHelpers::TimesEight, -firstDouble * static_cast<int32_t>(sizeof(double))), word);
        boxDoubleBits(jit, PureDoubleBits::branchIfNotFinite(jit, word, operand, slotIsSilent));
        jit.move(word, result);
        finish();
    };

    isNotInlineValue.link(&jit);
    BaseIndex narrowSlot(probe.receiver, operand, CCallHelpers::TimesFour);
    Jump isNotInt32 = branchUnlessFormIs(Form::InlineInt32);
#if CPU(X86_64)
    slotIsSilent.append(jit.branch32(CCallHelpers::LessThan, narrowSlot, TrustedImm32(minInlineInt32)));
    jit.load32(narrowSlot, result);
    jit.or64(numberTag, result);
#else
    jit.load32(narrowSlot, word);
    slotIsSilent.append(jit.branch32(CCallHelpers::LessThan, word, TrustedImm32(minInlineInt32)));
    boxInt32(jit, word, result);
#endif
    finish();

    isNotInt32.link(&jit);
    Jump isNotNarrowCell = branchUnlessFormIs(Form::InlineNarrowCell);
    jit.load32(narrowSlot, word);
    Jump isNotCell = jit.branch32(CCallHelpers::Below, word, TrustedImm32(minCell32));
    decodeCell32(jit, word, result);
    finish();

    isNotCell.link(&jit);
    slotIsSilent.append(jit.branch32(CCallHelpers::Below, word, TrustedImm32(FieldTableEntry::minNarrowValue)));
    jit.move(word, result);
    finish();

    isNotNarrowCell.link(&jit);
    readOutOfLineValue();

    static_assert(static_cast<unsigned>(Form::Field) == static_cast<unsigned>(Form::OutOfLineValue) + 1);
    auto first = [](uint32_t firstOperand) { return static_cast<int32_t>((1u << FieldTableEntry::formShift) + firstOperand); };
    Jump isNotInCell = jit.branch32(CCallHelpers::Below, operand, TrustedImm32(first(FieldTableEntry::firstEncodedByte)));
    Jump isNotEncodedByte = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstBoolean)));
    jit.load8(BaseIndex(probe.receiver, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstEncodedByte)), word);
    slotIsEmpty.append(jit.branchTest32(CCallHelpers::Zero, word));
    jit.move(word, result);
    finish();

    isNotEncodedByte.link(&jit);
    Jump isDouble = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstDouble)));
    jit.load8(BaseIndex(probe.receiver, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstBoolean)), word);
    jit.or32(TrustedImm32(JSValue::ValueFalse), word);
    jit.move(word, result);
    finish();

    isDouble.link(&jit);
    readDouble(first(FieldTableEntry::firstDouble));

    if (!probe.answersMissingFields) {
        otherwise.append(slotIsEmpty);
        otherwise.append(slotIsSilent);
        otherwise.append(isNotInCell);
    } else {
        auto goOnIn = [&](JumpList& jumps, StubKey stub) {
            const StubSignature signature = signatureOf(stub);
            const GPRReg receiverOfStub = signature.operands[0].gpr();
            RELEASE_ASSERT(probe.name == *signature.immediate);
            if (probe.waysOnwardAreCalls) {
                jumps.link(&jit);
                jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true, .registersThatCalleeLoses = registersThatWaysOnwardOfGetByIdLose }, scopedLambda<void()>([&] {
                    jit.move(probe.receiver, receiverOfStub);
                    jit.callStub(stub.withCallerFrame(CallerFrame::StubFrame));
                }));
                return;
            }
            if (probe.receiver == receiverOfStub) {
                jit.linkToStub(jumps, stub);
                return;
            }
            jumps.link(&jit);
            jit.move(probe.receiver, receiverOfStub);
            jit.jumpToStub(stub);
        };
        goOnIn(slotIsEmpty, getByIdOfEmptySlot().withCallerFrame(*probe.answersMissingFields));
        goOnIn(slotIsSilent, getByIdOfSilentSlot().withCallerFrame(*probe.answersMissingFields));
        isNotInCell.link(&jit);
        Jump isNotInExtension = jit.branch32(CCallHelpers::Below, operand, TrustedImm32(first(FieldTableEntry::firstNarrowCellInExtension)));
        JumpList isNarrowCellInExtension;
        isNarrowCellInExtension.append(jit.jump());
        goOnIn(isNarrowCellInExtension, getByIdOfNarrowCellInExtension().withCallerFrame(*probe.answersMissingFields));

        isNotInExtension.link(&jit);
        otherwise.append(jit.branch32(CCallHelpers::Below, operand, TrustedImm32(first(FieldTableEntry::firstInt64))));
        jit.load64(BaseIndex(probe.receiver, operand, CCallHelpers::TimesEight, -first(FieldTableEntry::firstInt64) * static_cast<int32_t>(sizeof(int64_t))), word);
        jit.signExtend32ToPtr(word, operand);
        otherwise.append(jit.branch64(CCallHelpers::NotEqual, word, operand));
        boxInt32(jit, word, result);
        finish();
    }
    if (!probe.answersAbsent) {
        otherwise.append(isNoField);
        return;
    }
    isNoField.link(&jit);
    branchUnlessNameIsAbsent(jit, probe, otherwise);
    if (finishWhereAbsent) {
        (*finishWhereAbsent)();
        return;
    }
    jit.move(TrustedImm64(JSValue::ValueUndefined), result);
    finish();
}

void answerMissingField(StubAssembler& jit, CallerFrame callerFrame, GPRReg object, GPRReg name, GPRReg T10, GPRReg T11, GPRReg T12, GPRReg T13, JumpList& otherwise)
{
    constexpr uint32_t flagsLeftToCaches = fieldIsOutOfLine | fieldMayExistWithoutStorage | fieldIsAccessor | fieldIsPrivate | fieldIsBrand;
    static_assert(sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo);
    RELEASE_ASSERT(noOverlap(object, name, T10, T11, T12, T13));
    const FieldTableProbe probe { object, name, T10, T11, true, true };
    JumpList isAbsent;

    jit.bindEntry(getByIdOfNarrowCellInExtension().withCallerFrame(callerFrame));
    if (jit.options().useWideReferences)
        otherwise.append(jit.jump());
    else {
        constexpr int32_t firstInFirstHalf = static_cast<int32_t>((static_cast<uint32_t>(FieldTableEntry::Form::Field) << FieldTableEntry::formShift) + FieldTableEntry::firstNarrowCellInExtension);
        constexpr int32_t granulesInHalf = FieldTableEntry::firstNarrowCellInSecondHalfOfGranule - FieldTableEntry::firstNarrowCellInExtension;
        const GPRReg operand = probe.index;
        const GPRReg word = probe.table;
        locateFieldTableEntry(jit, probe, otherwise);
        otherwise.append(branchUnlessEntryHasName(jit, probe));
        jit.sub32(TrustedImm32(firstInFirstHalf), operand);
        Jump isInSecondHalf = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(granulesInHalf));
        locateOutOfLineValue(jit, object, operand, word, isAbsent);
        jit.load32(BaseIndex(word, operand, CCallHelpers::TimesOne), word);
        Jump hasWord = jit.jump();

        isInSecondHalf.link(&jit);
        jit.sub32(TrustedImm32(granulesInHalf), operand);
        locateOutOfLineValue(jit, object, operand, word, isAbsent);
        jit.load32(BaseIndex(word, operand, CCallHelpers::TimesOne, sizeof(uint32_t)), word);

        hasWord.link(&jit);
        static_assert(!absentCell32);
        Jump isNoCell = jit.branch32(CCallHelpers::Below, word, TrustedImm32(minCell32));
        decodeCell32(jit, word, jit.result());
        jit.returnToCaller();

        isNoCell.link(&jit);
        isAbsent.append(jit.branchTest32(CCallHelpers::Zero, word));
        otherwise.append(jit.branch32(CCallHelpers::Below, word, TrustedImm32(FieldTableEntry::minNarrowValue)));
        jit.move(word, jit.result());
        jit.returnToCaller();
    }

    jit.bindEntry(getByIdOfSilentSlot().withCallerFrame(callerFrame));
    auto branchIfSomeFieldLiesOutsideItsSlot = [&] {
        return jit.branchTest32(CCallHelpers::NonZero, Address(object, offsetOfOrderWord), TrustedImm32(static_cast<int32_t>(orderWordHasFieldOutsideSlot)));
    };
    otherwise.append(branchIfSomeFieldLiesOutsideItsSlot());
    locateFieldTableEntry(jit, probe, otherwise);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), probe.table);
    jit.load16(BaseIndex(probe.table, probe.index, CCallHelpers::TimesTwo), probe.index);
    jit.and32(TrustedImm32(FieldRecordIndex::indexMask), probe.index);
    const GPRReg record = T12;
    loadStructure(jit, object, record);
    jit.loadPtr(Address(record, CompiledStructure::offsetOfLayout()), record);
    addOffsetOfField(jit, record, probe.index, record);
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, static_cast<int32_t>(sizeof(LayoutData) + OBJECT_OFFSETOF(FieldData, flags))), TrustedImm32(flagsLeftToCaches)));
    const FieldAddressRegisters field { object, T10, T11, T11, T12 };
    jit.load32(Address(record, static_cast<int32_t>(sizeof(LayoutData) + OBJECT_OFFSETOF(FieldData, slot))), field.slotData);
    computeFieldAddress(jit, field, false, otherwise);
    JumpList isPresent;
    JumpList isAbsentInObjectWithEveryFieldInItsSlot;
    branchOnFieldPresence(jit, field, T10, isPresent, isAbsentInObjectWithEveryFieldInItsSlot);

    static_assert(!uncreatedDeferredClosure);
    isPresent.link(&jit);
    auto returnMarkerOf = [&](SlotRepresentation representation) {
        const SlotTraits traits = traitsOf(representation);
        RELEASE_ASSERT(traits.hasUndefined && traits.hasNull);
        Jump isOtherRepresentation = jit.branch32(CCallHelpers::NotEqual, field.representation, TrustedImm32(static_cast<int32_t>(representation)));
        loadSlotBits(jit, traits.width, Address(field.address), T10);
        Jump isUndefined = branchIfBitsEqual(jit, traits.width, T10, traits.undefinedBits);
        Jump isNull = branchIfBitsEqual(jit, traits.width, T10, traits.nullBits);
        otherwise.append(jit.jump());
        isNull.link(&jit);
        jit.move(TrustedImm64(JSValue::ValueNull), jit.result());
        jit.returnToCaller();
        isUndefined.link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
        jit.returnToCaller();
        isOtherRepresentation.link(&jit);
    };
    returnMarkerOf(SlotRepresentation::OptionalInt32);
    returnMarkerOf(SlotRepresentation::OptionalDouble);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, field.representation, TrustedImm32(static_cast<int32_t>(SlotRepresentation::DeferredClosure))));
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, Address(field.address)));
    jit.callOperationAndReturn(Entry::operationAOTMaterializeDeferredClosure, { .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = object, .thirdArgumentIn = name });

    isAbsent.link(&jit);
    jit.bindEntry(getByIdOfEmptySlot().withCallerFrame(callerFrame));
    otherwise.append(branchIfSomeFieldLiesOutsideItsSlot());
    isAbsentInObjectWithEveryFieldInItsSlot.link(&jit);
    JumpList isNotBelowObjectPrototype;
    branchUnlessNameIsAbsent(jit, probe, otherwise, &isNotBelowObjectPrototype);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    isNotBelowObjectPrototype.link(&jit);
    jit.bindEntry(getByIdOfFieldLeftOut().withCallerFrame(callerFrame));
    JumpList isNotInFrontTable;
#if CPU(X86_64)
    constexpr int32_t bitsAsked = PackedSlot::kindMask | PackedSlot::kindFlag;
    constexpr int32_t bitsOfFieldThatIsUndefinedWhereEmpty = ((static_cast<uint8_t>(Slot::Kind::Field) | PackedSlot::kindFlag) ^ PackedSlot::ownInlineValueBits) & bitsAsked;
    static_assert(Slot::kindFlagOf(Slot::Kind::Field) == Slot::absentIsUndefined);
    const SlotOrigin origin = callerFrame == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    const GPRReg cell = T12;
    RELEASE_ASSERT(cell == gprOf(slotHeaderRegister) && object == gprOf(firstStubOperandRegister) && name == gprOf(slotNameRegister));
    jit.load32(Address(object, JSCell::structureIDOffset()), T11);
    locateFrontCell(jit, SlotAccess::Read, T11, name, T10);
    jit.m_assembler.cmpw_rm(name, static_cast<int32_t>(PackedSlot::offsetOfName()), T10, T11, CCallHelpers::TimesEight);
    isNotInFrontTable.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE)));
    jit.load64(BaseIndex(T10, T11, CCallHelpers::TimesEight), cell);
    jit.urshift64(cell, TrustedImm32(kindShiftInCell), T10);
    jit.and32(TrustedImm32(bitsAsked), T10);
    Jump saysSomethingElse = jit.branch32(CCallHelpers::NotEqual, T10, TrustedImm32(bitsOfFieldThatIsUndefinedWhereEmpty));
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    constexpr int32_t bitsOfCellWithoutSlot = (static_cast<uint8_t>(Slot::Kind::Empty) ^ PackedSlot::ownInlineValueBits) & bitsAsked;
    saysSomethingElse.link(&jit);
    isNotInFrontTable.append(jit.branch32(CCallHelpers::Equal, T10, TrustedImm32(bitsOfCellWithoutSlot)));
    jumpToSlotHandler(jit, slotHandlerTable(SlotOperation::Read, origin, SlotForm::PackedSlot), cell, T10);
#endif

    isNotInFrontTable.link(&jit);
    JumpList isNotFiled;
    returnWhatStandsAboveFieldLeftOut(jit, { { object, name, T10, T11 }, T12, T13 }, isNotFiled);
#if CPU(X86_64)
    jit.linkToStub(isNotFiled, getByIdHashed().withCallerFrame(callerFrame));
#else
    otherwise.append(isNotFiled);
#endif
}

static constexpr int32_t offsetOfWordOfAccessTable(PartOfAccessTable part)
{
    switch (part) {
    case PartOfAccessTable::Reads:
        return static_cast<int32_t>(AOT_INSTANCE_OFFSET(propertyCaches.accessTable.reads));
    case PartOfAccessTable::Calls:
        return static_cast<int32_t>(AOT_INSTANCE_OFFSET(propertyCaches.accessTable.calls));
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr unsigned sizeShiftOfPlaceInAccessTable(PartOfAccessTable part)
{
    switch (part) {
    case PartOfAccessTable::Reads:
        return AccessTable::Read::sizeShift;
    case PartOfAccessTable::Calls:
        return AccessTable::Call::sizeShift;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static_assert(AccessTable::structureIDShift == StructureRowWords::granuleShift && MarkedBlock::atomSize > AccessTable::lowBitsOfStructureID && numberOfArgumentGPRs <= AccessTable::lowBitsOfStructureID);
static_assert(std::endian::native == std::endian::little && AccessTable::nameShiftInKey == 8 * sizeof(uint32_t) && !JSCell::structureIDOffset());

void locatePlaceInAccessTable(StubAssembler& jit, PartOfAccessTable part, const AccessTableRegisters& r, unsigned numberOfArguments)
{
    RELEASE_ASSERT(noOverlap(r.cell, r.name, r.key, r.place, instanceGPR) && numberOfArguments <= AccessTable::lowBitsOfStructureID && (!numberOfArguments || part == PartOfAccessTable::Calls));
    const Address wordOfTable(instanceGPR, offsetOfWordOfAccessTable(part));
    const TrustedImm32 sizeShift(sizeShiftOfPlaceInAccessTable(part));
    jit.load32(Address(r.cell, JSCell::structureIDOffset()), r.key);
#if CPU(ARM64)
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg index = ownedScratch;
    RELEASE_ASSERT(noOverlap(index, r.cell, r.name, r.key, r.place));
    jit.loadPtr(wordOfTable, r.place);
    jit.addUnsignedRightShift32(r.name, r.key, TrustedImm32(AccessTable::structureIDShift), index);
    jit.insertBitField64(index, sizeShift, TrustedImm32(AccessTable::numberOfPlacesShift), r.place);
    if (numberOfArguments)
        jit.add32(TrustedImm32(numberOfArguments), r.key);
    jit.orLeftShift64(r.key, r.name, TrustedImm32(AccessTable::nameShiftInKey), r.key);
#else
    jit.move(r.key, r.place);
    jit.urshift32(TrustedImm32(AccessTable::structureIDShift), r.place);
    jit.add32(r.name, r.place);
    jit.and32(TrustedImm32(AccessTable::indexMask), r.place);
    jit.lshift32(sizeShift, r.place);
    jit.addPtr(wordOfTable, r.place);
    if (numberOfArguments)
        jit.add32(TrustedImm32(numberOfArguments), r.key);
#endif
}

void loadWordBehindKeyInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, ptrdiff_t offsetOfKey, GPRReg word, GPRReg scratch, JumpList& hasOtherKey)
{
    const int32_t offset = static_cast<int32_t>(offsetOfKey);
#if CPU(ARM64)
    RELEASE_ASSERT(noOverlap(word, scratch, r.key, r.cell, r.name));
    jit.loadPair64(r.place, TrustedImm32(offset), scratch, word);
    hasOtherKey.append(jit.branch64(CCallHelpers::NotEqual, scratch, r.key));
#else
    UNUSED_PARAM(scratch);
    RELEASE_ASSERT(noOverlap(word, r.cell, r.name));
    hasOtherKey.append(jit.branch32(CCallHelpers::NotEqual, Address(r.place, offset), r.key));
    hasOtherKey.append(jit.branch32(CCallHelpers::NotEqual, Address(r.place, offset + static_cast<int32_t>(sizeof(uint32_t))), r.name));
    jit.load64(Address(r.place, offset + static_cast<int32_t>(sizeof(uint64_t))), word);
#endif
}

static void storeKeyInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, ptrdiff_t offsetOfKey)
{
    const int32_t offset = static_cast<int32_t>(offsetOfKey);
#if CPU(ARM64)
    jit.store64(r.key, Address(r.place, offset));
#else
    jit.store32(r.key, Address(r.place, offset));
    jit.store32(r.name, Address(r.place, offset + static_cast<int32_t>(sizeof(uint32_t))));
#endif
}

static void storeKeyAndWordInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, ptrdiff_t offsetOfKey, GPRReg word)
{
#if CPU(ARM64)
    jit.storePair64(r.key, word, r.place, TrustedImm32(static_cast<int32_t>(offsetOfKey)));
#else
    storeKeyInAccessTable(jit, r, offsetOfKey);
    jit.store64(word, Address(r.place, static_cast<int32_t>(offsetOfKey + sizeof(uint64_t))));
#endif
}

static void loadHolderOfAnswerInAccessTable(StubAssembler& jit, GPRReg answer, GPRReg holder)
{
#if CPU(ARM64)
    extractBits(jit, answer, AccessTable::answerShift, 8 * sizeof(uint32_t), holder);
#else
    jit.urshift64(answer, TrustedImm32(AccessTable::answerShift), holder);
    jit.zeroExtend32ToWord(holder, holder);
#endif
    decodeCell32(jit, holder, holder);
}

void loadValueInOwnStorageByAnswer(StubAssembler& jit, GPRReg cell, GPRReg answer, GPRReg scratch, GPRReg result)
{
    RELEASE_ASSERT(noOverlap(cell, answer, scratch));
    jit.rshift64(TrustedImm32(AccessTable::answerShift), answer);
    jit.loadPtr(Address(cell, JSObject::butterflyOffset()), scratch);
    jit.load64(BaseIndex(scratch, answer, CCallHelpers::TimesEight), result);
}

void loadValueInStorageOfHolderByAnswer(StubAssembler& jit, GPRReg answer, GPRReg scratch, GPRReg result)
{
    RELEASE_ASSERT(answer != scratch);
    loadHolderOfAnswerInAccessTable(jit, answer, scratch);
    jit.loadPtr(Address(scratch, JSObject::butterflyOffset()), scratch);
    jit.rshift64(TrustedImm32(AccessTable::wordIndexShiftBehindHolder), answer);
    jit.load64(BaseIndex(scratch, answer, CCallHelpers::TimesEight), result);
}

void readByFormOfAnswerInAccessTable(StubAssembler& jit, GPRReg cell, GPRReg answer, GPRReg scratch, GPRReg result, JumpList& findsGetterInOwnStorage, const ScopedLambda<void()>& finish)
{
    Jump isInStorageOfHolder = jit.branchTest64(CCallHelpers::Zero, answer, TrustedImm32(AccessTable::valueIsInOwnStorage));
    findsGetterInOwnStorage.append(jit.branchTest64(CCallHelpers::NonZero, answer, TrustedImm32(AccessTable::valueIsInStorageOfHolder)));
    loadValueInOwnStorageByAnswer(jit, cell, answer, scratch, result);
    finish();

    isInStorageOfHolder.link(&jit);
    loadValueInStorageOfHolderByAnswer(jit, answer, scratch, result);
    finish();
}

void returnValueByAccessTable(StubAssembler& jit, const AccessTableRead& read, JumpList& isNotAnswered)
{
    const AccessTableRegisters& r = read.registers;
    RELEASE_ASSERT(noOverlap(read.word, r.cell, r.name, r.key, r.place));
    JumpList isNoOwnValue;
    locatePlaceInAccessTable(jit, PartOfAccessTable::Reads, r);
    loadWordBehindKeyInAccessTable(jit, r, OBJECT_OFFSETOF(AccessTable::Read, keyOfOwnValue), read.word, read.scratch, isNoOwnValue);
    jit.load64(BaseIndex(r.cell, read.word, CCallHelpers::TimesOne), read.word);
    isNotAnswered.append(jit.branchTest64(CCallHelpers::Zero, read.word));
    jit.move(read.word, jit.result());
    jit.returnToCaller();

    isNoOwnValue.link(&jit);
    loadWordBehindKeyInAccessTable(jit, r, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), read.word, read.scratch, isNotAnswered);
    Jump hasForm = jit.branchTest64(CCallHelpers::NonZero, read.word, TrustedImm32(AccessTable::answerHasForm));
    jit.load64(Address(read.word), jit.result());
    jit.returnToCaller();

    hasForm.link(&jit);
    JumpList findsGetterInOwnStorage;
    readByFormOfAnswerInAccessTable(jit, r.cell, read.word, r.place, jit.result(), findsGetterInOwnStorage, [&] {
        jit.returnToCaller();
    });

    const StubKey call = callGetterOfAnswer(read.waysOnwardAreCalls ? CallerFrame::StubFrame : jit.key().callerFrame());
    const StubSignature signature = signatureOfCallGetterOfAnswer(call);
    const GPRReg receiver = signature.operands[0].gpr();
    const GPRReg getterSetter = read.waysOnwardAreCalls ? gprOf(StubRegister::T10) : r.cell == gprOf(StubRegister::T10) ? jit.clobberOperand(0) : jit.reuseScratch(StubRegister::T10);
    RELEASE_ASSERT(noOverlap(receiver, read.word, r.place) && noOverlap(getterSetter, receiver) && signature.operands[1] == Reg(getterSetter));
    findsGetterInOwnStorage.link(&jit);
    jit.rshift64(TrustedImm32(AccessTable::answerShift), read.word);
    jit.loadPtr(Address(r.cell, JSObject::butterflyOffset()), r.place);
    jit.load64(BaseIndex(r.place, read.word, CCallHelpers::TimesEight), read.word);
    branchUnlessAccessorPair(jit, read.word, isNotAnswered);
    auto bringOperands = [&] {
        jit.move(r.cell, receiver);
        jit.move(read.word, getterSetter);
    };
    if (read.waysOnwardAreCalls) {
        jit.callAndReturnPreservingRegisters({ .callee = StubCallee::CompiledCode, .returnsValue = true, .checksException = true, .registersThatCalleeLoses = registersThatWaysOnwardOfGetByIdLose }, scopedLambda<void()>([&] {
            bringOperands();
            jit.callStub(call);
        }));
        return;
    }
    bringOperands();
    jit.jumpToStub(call);
}

static void locatePlaceOfFieldLeftOutInAccessTable(StubAssembler& jit, const AccessTableRegisters& r)
{
    static_assert(!(AccessTable::alignment % (2 * (AccessTable::indexBitOfFieldLeftOut << AccessTable::Read::sizeShift))) && AccessTable::keyBitOfFieldLeftOut <= AccessTable::lowBitsOfStructureID);
    locatePlaceInAccessTable(jit, PartOfAccessTable::Reads, r);
    jit.xor64(TrustedImm32(AccessTable::indexBitOfFieldLeftOut << AccessTable::Read::sizeShift), r.place);
    jit.or64(TrustedImm32(AccessTable::keyBitOfFieldLeftOut), r.key);
}

void returnWhatStandsAboveFieldLeftOut(StubAssembler& jit, const AccessTableRead& read, JumpList& isNotAnswered)
{
    const AccessTableRegisters& r = read.registers;
    RELEASE_ASSERT(noOverlap(read.word, r.cell, r.name, r.key, r.place));
    locatePlaceOfFieldLeftOutInAccessTable(jit, r);
    loadWordBehindKeyInAccessTable(jit, r, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), read.word, read.scratch, isNotAnswered);
    Jump isInStorageOfHolder = jit.branchTest64(CCallHelpers::NonZero, read.word, TrustedImm32(AccessTable::answerHasForm));
    jit.load64(Address(read.word), jit.result());
    jit.returnToCaller();

    isInStorageOfHolder.link(&jit);
    loadValueInStorageOfHolderByAnswer(jit, read.word, r.place, jit.result());
    jit.returnToCaller();
}

void makeAnswerOfFieldTable(StubAssembler& jit, GPRReg formAndOperand, GPRReg answer)
{
    static_assert(AccessTable::answerOfFieldTable(FieldTableEntry::maxFormAndOperand) <= UINT32_MAX);
    jit.move(formAndOperand, answer);
    jit.lshift32(TrustedImm32(AccessTable::answerShift), answer);
    jit.or32(TrustedImm32(AccessTable::answerHasForm), answer);
}

void makeAnswerInOwnStorage(StubAssembler& jit, GPRReg wordIndexAndAnswer)
{
    jit.lshift64(TrustedImm32(AccessTable::answerShift), wordIndexAndAnswer);
    jit.or64(TrustedImm32(AccessTable::valueIsInOwnStorage | AccessTable::answerHasForm), wordIndexAndAnswer);
}

void makeAnswerOfGetterInOwnStorage(StubAssembler& jit, GPRReg wordIndexAndAnswer)
{
    jit.lshift64(TrustedImm32(AccessTable::answerShift), wordIndexAndAnswer);
    jit.or64(TrustedImm32(AccessTable::getterIsInOwnStorage | AccessTable::answerHasForm), wordIndexAndAnswer);
}

void makeAnswerInStorageOfHolder(StubAssembler& jit, GPRReg frontCell, GPRReg answer)
{
    constexpr unsigned bitsAboveIndex = 64 - locationShiftInCell - locationBitsInCell;
    static_assert(locationBitsInCell == 8 && AccessTable::wordIndexShiftBehindHolder - AccessTable::answerShift == locationShiftInCell && cell32Shift == CCallHelpers::TimesEight);
    jit.lshift64(frontCell, TrustedImm32(bitsAboveIndex), answer);
    jit.rshift64(TrustedImm32(bitsAboveIndex - AccessTable::answerShift), answer);
    jit.or64(TrustedImm32(AccessTable::valueIsInStorageOfHolder | AccessTable::answerHasForm), answer);
}

static constexpr uint32_t dictionaryKindsForAccessTable = Structure::s_dictionaryKindMask << Structure::s_dictionaryKindShift;

static Jump branchIfStructureHasBitsForAccessTable(StubAssembler& jit, GPRReg cell, GPRReg scratch, uint32_t bits)
{
    loadStructure(jit, cell, scratch);
    return jit.branchTest32(CCallHelpers::NonZero, Address(scratch, Structure::bitFieldOffset()), TrustedImm32(static_cast<int32_t>(bits)));
}

static void listKeyThatDependsOnChains(StubAssembler& jit, GPRReg place, ptrdiff_t offsetOfKey, GPRReg scratch)
{
    const Address numberOfKeys(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.accessTable.numberOfKeysThatDependOnChains));
    RELEASE_ASSERT(noOverlap(place, scratch, instanceGPR));
    jit.load32(numberOfKeys, scratch);
    Jump hasNoRoom = jit.branch32(CCallHelpers::AboveOrEqual, scratch, TrustedImm32(AccessTable::maxKeysListed));
    jit.lshift32(TrustedImm32(3), scratch);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.accessTable.keysThatDependOnChains)), scratch);
    if (offsetOfKey)
        jit.addPtr(TrustedImm32(static_cast<int32_t>(offsetOfKey)), place);
    jit.storePtr(place, Address(scratch));
    jit.add32(TrustedImm32(1), numberOfKeys);
    Jump isListed = jit.jump();

    hasNoRoom.link(&jit);
    jit.store32(TrustedImm32(AccessTable::maxKeysListed + 1), numberOfKeys);
    isListed.link(&jit);
}

void fileOwnValueInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg offsetInCell)
{
    RELEASE_ASSERT(noOverlap(offsetInCell, r.cell, r.name, r.key, r.place));
    const OwnedScratchRegister heldPlace(jit, r.place);
    locatePlaceInAccessTable(jit, PartOfAccessTable::Reads, r);
    storeKeyAndWordInAccessTable(jit, r, OBJECT_OFFSETOF(AccessTable::Read, keyOfOwnValue), offsetInCell);
}

static void fileOtherAnswerInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg answer)
{
    RELEASE_ASSERT(noOverlap(answer, r.cell, r.name, r.key, r.place));
    const OwnedScratchRegister heldPlace(jit, r.place);
    locatePlaceInAccessTable(jit, PartOfAccessTable::Reads, r);
    storeKeyAndWordInAccessTable(jit, r, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), answer);
}

void fileOwnValueOfOrdinaryCellInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg offsetInCell)
{
    Jump isDictionary = branchIfStructureHasBitsForAccessTable(jit, r.cell, r.key, dictionaryKindsForAccessTable);
    fileOwnValueInAccessTable(jit, r, offsetInCell);
    isDictionary.link(&jit);
}

void fileOwnValueInStorageInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg answer)
{
    Jump isDictionary = branchIfStructureHasBitsForAccessTable(jit, r.cell, r.key, dictionaryKindsForAccessTable);
    fileOtherAnswerInAccessTable(jit, r, answer);
    isDictionary.link(&jit);
}

static void fileAnswerAboutChainInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg answer, JumpList& filesNothing)
{
    filesNothing.append(branchIfStructureHasBitsForAccessTable(jit, r.cell, r.key, dictionaryKindsForAccessTable));
    fileOtherAnswerInAccessTable(jit, r, answer);
    listKeyThatDependsOnChains(jit, r.place, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), r.key);
    filesNothing.link(&jit);
}

void fileValueAboveInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg slot)
{
    const GPRReg holder = r.place;
    JumpList filesNothing;
    filesNothing.append(jit.branchIfNotCell(r.cell));
    filesNothing.append(branchIfStructureHasBitsForAccessTable(jit, holder, r.key, dictionaryKindsForAccessTable));
    fileAnswerAboutChainInAccessTable(jit, r, slot, filesNothing);
}

void fileValueInStorageOfHolderInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg answer)
{
    const GPRReg holder = r.place;
    JumpList filesNothing;
    filesNothing.append(jit.branchIfNotCell(r.cell));
    loadHolderOfAnswerInAccessTable(jit, answer, holder);
    filesNothing.append(branchIfStructureHasBitsForAccessTable(jit, holder, r.key, dictionaryKindsForAccessTable));
    fileAnswerAboutChainInAccessTable(jit, r, answer, filesNothing);
}

void fileAbsenceInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg answer)
{
    JumpList filesNothing;
    filesNothing.append(jit.branchIfNotCell(r.cell));
    jit.addPtr(TrustedImm32(static_cast<int32_t>(AOT_INSTANCE_OFFSET(propertyCaches.accessTable.undefinedValue))), instanceGPR, answer);
    fileAnswerAboutChainInAccessTable(jit, r, answer, filesNothing);
}

static void fileAnswerAboveFieldLeftOutInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg answer)
{
    RELEASE_ASSERT(noOverlap(answer, r.cell, r.name, r.key, r.place));
    locatePlaceOfFieldLeftOutInAccessTable(jit, r);
    storeKeyAndWordInAccessTable(jit, r, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), answer);
    listKeyThatDependsOnChains(jit, r.place, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), r.key);
}

void fileValueAboveFieldLeftOutInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg holder, GPRReg answer)
{
    Jump filesNothing = branchIfStructureHasBitsForAccessTable(jit, holder, r.key, dictionaryKindsForAccessTable);
    fileAnswerAboveFieldLeftOutInAccessTable(jit, r, answer);
    filesNothing.link(&jit);
}

void fileAbsenceAboveFieldLeftOutInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, GPRReg answer)
{
    jit.addPtr(TrustedImm32(static_cast<int32_t>(AOT_INSTANCE_OFFSET(propertyCaches.accessTable.undefinedValue))), instanceGPR, answer);
    fileAnswerAboveFieldLeftOutInAccessTable(jit, r, answer);
}

void fileCallInAccessTable(StubAssembler& jit, const AccessTableRegisters& r, unsigned numberOfArguments, GPRReg callee, GPRReg closureWord)
{
    RELEASE_ASSERT(noOverlap(callee, closureWord, r.cell, r.name, r.key, r.place));
    locatePlaceInAccessTable(jit, PartOfAccessTable::Calls, r, numberOfArguments);
    storeKeyInAccessTable(jit, r, OBJECT_OFFSETOF(AccessTable::Call, key));
    jit.storePair64(callee, closureWord, r.place, TrustedImm32(OBJECT_OFFSETOF(AccessTable::Call, callee)));
    const GPRReg read = closureWord;
    const GPRReg slot = r.key;
    locatePlaceInAccessTable(jit, PartOfAccessTable::Reads, { r.cell, r.name, r.key, read });
    jit.load64(Address(read, OBJECT_OFFSETOF(AccessTable::Read, otherAnswer)), slot);
    jit.store64(slot, Address(r.place, OBJECT_OFFSETOF(AccessTable::Call, slotAbove)));
    listKeyThatDependsOnChains(jit, r.place, OBJECT_OFFSETOF(AccessTable::Call, key), r.key);
}

void returnFieldByTable(StubAssembler& jit, const FieldTableProbe& probe, JumpList& otherwise)
{
    readFieldByTable(jit, probe, jit.result(), otherwise, [&] {
        jit.returnToCaller();
    });
}

void storeFieldByTable(StubAssembler& jit, const FieldTableProbe& probe, GPRReg value, GPRReg scratch, SlotOperation operation, JumpList& otherwise)
{
    using Form = FieldTableEntry::Form;
    static_assert(sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo && FieldRecordIndex::isStoredWithoutTypeTest == 0x80 << 8);
    RELEASE_ASSERT(operation != SlotOperation::Read && noOverlap(value, scratch, probe.receiver, probe.index, probe.table) && signatureOfWriteBarrier(writeBarrier()).operands[0] == Reg(probe.receiver));
    RELEASE_ASSERT(scratch != probe.name && probe.keyInAccessTable == InvalidGPRReg);
    const GPRReg operand = probe.index;
    const GPRReg extension = probe.table;
    JumpList isEmptySlot;
    JumpList isEmptySlotThatTakesValue;
    auto storeValue = [&](BaseIndex slot, JumpList& isEmpty) {
        isEmpty.append(jit.branchTest64(CCallHelpers::Zero, slot));
        jit.store64(value, slot);
        jit.linkToStub(jit.branchIfCell(value), writeBarrier());
        jit.returnToCaller();
    };

    locateFieldTableEntry(jit, probe, otherwise);
#if CPU(X86_64)
    otherwise.append(branchUnlessEntryHasNameKeepingIndex(jit, probe));
    auto loadEntryOfName = [&] {
        jit.load16(fieldTableEntry(probe), probe.index);
    };
#else
    auto loadEntryOfName = [&] {
        otherwise.append(branchUnlessEntryHasName(jit, probe));
    };
#endif
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), scratch);
    Jump isStoredByItsRecord = jit.branchTest8(CCallHelpers::Zero, BaseIndex(scratch, probe.index, CCallHelpers::TimesTwo, 1), TrustedImm32(0x80));
    loadEntryOfName();
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, Address(probe.receiver, offsetOfOrderWord), TrustedImm32(integrityBitsBlockingStore(operation))));
    Jump isNotInlineValue = branchUnlessForm(jit, operand, Form::InlineValue);
    storeValue(BaseIndex(probe.receiver, operand, CCallHelpers::TimesEight), isEmptySlotThatTakesValue);

    isNotInlineValue.link(&jit);
    bindArrivalsWithFormAndOperand(jit, probe);
    Jump isNotInt32Field = branchUnlessForm(jit, operand, Form::InlineInt32);
    const BaseIndex int32Slot(probe.receiver, operand, CCallHelpers::TimesFour);
    JumpList isRefusedByInt32Slot;
    isRefusedByInt32Slot.append(jit.branchIfNotInt32(value));
    isRefusedByInt32Slot.append(jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(minInlineInt32)));
    isEmptySlot.append(jit.branch32(CCallHelpers::Equal, int32Slot, TrustedImm32(absentInt32)));
    jit.store32(value, int32Slot);
    jit.returnToCaller();

    isNotInt32Field.link(&jit);
    static_assert(static_cast<unsigned>(Form::OutOfLineValue) == static_cast<unsigned>(Form::InlineInt32) + 2);
    jit.sub32(TrustedImm32(1 << FieldTableEntry::formShift), operand);
    Jump isNotOutOfLineValue = branchUnlessForm(jit, operand, Form::OutOfLineValue);
    if (jit.options().useWideReferences)
        otherwise.append(jit.jump());
    else {
        locateOutOfLineValue(jit, probe.receiver, operand, extension, isEmptySlot);
        storeValue(BaseIndex(extension, operand, CCallHelpers::TimesOne), isEmptySlot);
    }

    const StubKey booleanOrDouble = putBooleanOrDoubleField(putModeOf(operation));
    const StubSignature signature = signatureOf(booleanOrDouble);
    RELEASE_ASSERT(signature.operands[0] == Reg(probe.receiver) && signature.operands[1] == Reg(value) && signature.operands[2] == Reg(operand) && probe.name == *signature.immediate);
    isNotOutOfLineValue.link(&jit);
    jit.jumpToStub(booleanOrDouble);

    const StubKey refusedValue = putValueThatSlotRefuses(putModeOf(operation));
    const StubSignature signatureOfRefusedValue = signatureOf(refusedValue);
    RELEASE_ASSERT(signatureOfRefusedValue.operands[0] == Reg(probe.receiver) && signatureOfRefusedValue.operands[1] == Reg(value) && probe.name == *signatureOfRefusedValue.immediate);
    isRefusedByInt32Slot.link(&jit);
    jit.jumpToStub(refusedValue);

    const GPRReg offsetOfRecord = jit.reuseScratch(slotNameRegister);
    isEmptySlotThatTakesValue.link(&jit);
    {
        FieldTableProbe again = probe;
        again.nameHasColumn = true;
        again.receiverIsCell = true;
        locateFieldTableEntry(jit, again, otherwise);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), scratch);
        jit.load16(BaseIndex(scratch, probe.index, CCallHelpers::TimesTwo), scratch);
        jit.and32(TrustedImm32(FieldRecordIndex::indexMask), scratch);
        loadOffsetOfFieldRecord(jit, scratch, offsetOfRecord);
        jit.jumpToStub(putKnownFieldThatTakesAnyValue(putModeOf(operation)));
    }
    isEmptySlot.link(&jit);
    FieldTableProbe again = probe;
    again.nameHasColumn = true;
    again.receiverIsCell = true;
    locateFieldTableEntry(jit, again, otherwise);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), scratch);
    jit.load16(BaseIndex(scratch, probe.index, CCallHelpers::TimesTwo), scratch);
    jit.and32(TrustedImm32(FieldRecordIndex::indexMask), scratch);
    Jump hasIndex = jit.jump();

    isStoredByItsRecord.link(&jit);
    const GPRReg entryInPlaceOfType = jit.reuseScratch(StubRegister::T10);
    const GPRReg kind = jit.reuseScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(entryInPlaceOfType, kind, value, scratch, probe.receiver, probe.name, probe.index, probe.table));
    jit.move(probe.index, entryInPlaceOfType);
    jit.load16(BaseIndex(scratch, probe.index, CCallHelpers::TimesTwo), scratch);
    loadEntryOfName();
    if (!jit.options().useWideReferences) {
        static_assert(!traitsOf(SlotRepresentation::Cell32).absentBits && !traitsOf(SlotRepresentation::OptionalCell32).absentBits);
        static_assert(traitsOf(SlotRepresentation::OptionalCell32).undefinedBits == JSValue::ValueUndefined && traitsOf(SlotRepresentation::OptionalCell32).nullBits == JSValue::ValueNull);
        JumpList goesByItsRecord;
        const BaseIndex boxedSlot(probe.receiver, operand, CCallHelpers::TimesEight);
        const BaseIndex narrowSlot(probe.receiver, operand, CCallHelpers::TimesFour);
        static_assert(!static_cast<unsigned>(Form::InlineValue) && static_cast<unsigned>(Form::InlineNarrowCell) == 2);
        auto leaveUnlessStoreIsKnownToBeTaken = [&] {
            goesByItsRecord.append(jit.branchTest32(CCallHelpers::NonZero, Address(probe.receiver, offsetOfOrderWord), TrustedImm32(integrityBitsBlockingStore(operation))));
            if (!jit.options().hasOneInstance) {
                loadStructure(jit, probe.receiver, kind);
                goesByItsRecord.append(branchIfNotOfThisRealm(jit, kind));
            }
            jit.or32(TrustedImm32(static_cast<int32_t>(AcceptedKindOfValue::isEntryOfFieldTable)), entryInPlaceOfType);
            branchUnlessKindOfValueIsAccepted(jit, value, entryInPlaceOfType, kind, probe.table, goesByItsRecord);
            if (!jit.options().checksProofs)
                return;
            const GPRReg record = probe.table;
            const GPRReg type = entryInPlaceOfType;
            constexpr uint32_t flagsOfFieldsStoredOtherwise = fieldAttributeFlags | fieldIsPrivate | fieldIsBrand | fieldIsOutOfLine;
            JumpList differs;
            loadStructure(jit, probe.receiver, record);
            jit.loadPtr(Address(record, CompiledStructure::offsetOfLayout()), record);
            addOffsetOfField(jit, record, scratch, record);
            differs.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, sizeof(LayoutData) + OBJECT_OFFSETOF(FieldData, flags)), TrustedImm32(flagsOfFieldsStoredOtherwise | fieldTypeIsLeftToRunTime)));
            loadTypeOfFieldRecord(jit, record, sizeof(LayoutData), type);
            branchUnlessTypeAccepts(jit, { value, type, kind, probe.table, scratch, probe.receiver }, differs, differs);
            Jump agrees = jit.jump();
            differs.link(&jit);
            jit.breakpoint();
            agrees.link(&jit);
        };
        Jump isNotBoxed = jit.branch32(CCallHelpers::Above, operand, TrustedImm32(FieldTableEntry::maxOperand));
        goesByItsRecord.append(jit.branchTest64(CCallHelpers::Zero, boxedSlot));
        leaveUnlessStoreIsKnownToBeTaken();
        jit.store64(value, boxedSlot);
        jit.linkToStub(jit.branchIfCell(value), writeBarrier());
        jit.returnToCaller();

        isNotBoxed.link(&jit);
        jit.sub32(TrustedImm32(static_cast<int32_t>(Form::InlineNarrowCell) << FieldTableEntry::formShift), operand);
        goesByItsRecord.append(jit.branch32(CCallHelpers::Above, operand, TrustedImm32(FieldTableEntry::maxOperand)));
        goesByItsRecord.append(jit.branchTest32(CCallHelpers::Zero, narrowSlot));
        leaveUnlessStoreIsKnownToBeTaken();
        Jump isMarker = jit.branchIfNotCell(value);
        encodeCellOfProgram(jit, value, kind);
        jit.store32(kind, narrowSlot);
        jit.jumpToStub(writeBarrier());

        isMarker.link(&jit);
        jit.store32(value, narrowSlot);
        jit.returnToCaller();
        goesByItsRecord.link(&jit);
    }
    hasIndex.link(&jit);
    loadOffsetOfFieldRecord(jit, scratch, offsetOfRecord);
    jit.jumpToStub(putKnownField(putModeOf(operation)));
}

StubSignature signatureOfPutBooleanOrDoubleField(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(storedValueByIdRegister));
    signature.addOperand(gprOf(StubRegister::T11));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfPutValueThatSlotRefuses(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(storedValueByIdRegister));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

void generatePutValueThatSlotRefuses(StubAssembler& jit)
{
    static_assert(sizeof(StructureID) == sizeof(uint32_t) && MarkedSpace::sizeStep == 2 * sizeof(Allocator));
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const GPRReg object = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg name = jit.immediate();
    const GPRReg offsetOfSlot = jit.allocateScratch(StubRegister::T11);
    const GPRReg exit = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg extra = jit.allocateScratch(StubRegister::T14);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(object, value, offsetOfSlot, name, exit, scratch, extra, allocator, instanceGPR));
    const BaseIndex slot(object, offsetOfSlot, CCallHelpers::TimesOne);
    const Address orderWord(object, offsetOfOrderWord);
    auto memberOfExit = [&](ptrdiff_t offset) {
        return Address(exit, static_cast<int32_t>(offset));
    };
    JumpList cannot;

    jit.load32(Address(object, JSCell::structureIDOffset()), scratch);
    jit.urshift32(scratch, TrustedImm32(StructureRowWords::granuleShift), exit);
    jit.add32(name, exit);
    jit.and32(TrustedImm32(FieldExit::numberOfPlaces - 1), exit);
    jit.lshift32(TrustedImm32(FieldExit::sizeShift), exit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.fieldExits)), allocator);
    cannot.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    jit.addPtr(allocator, exit);
    cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, structureBefore)), scratch));
    cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, name)), name));
    jit.load32(orderWord, scratch);
    cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, orderWordBefore)), scratch));
    cannot.append(branchIfBarrierIsNeeded(jit, object, scratch));
    cannot.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(frameCellState)));
    cannot.append(jit.branchTest8(CCallHelpers::Zero, memberOfExit(OBJECT_OFFSETOF(FieldExit, widthOfSlot))));
    cannot.append(jit.branchIfOther(value, scratch));
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, offsetOfSlot)), offsetOfSlot);
    auto branchIfSlotIsWide = [&] {
        return jit.branchTest8(CCallHelpers::NonZero, memberOfExit(OBJECT_OFFSETOF(FieldExit, widthOfSlot)), TrustedImm32(sizeof(uint64_t)));
    };

    Jump slotIsWide = branchIfSlotIsWide();
    jit.load32(slot, scratch);
    Jump isLoaded = jit.jump();
    slotIsWide.link(&jit);
    jit.load64(slot, scratch);
    isLoaded.link(&jit);
    Jump isInSlot = jit.branch64(CCallHelpers::NotEqual, scratch, memberOfExit(OBJECT_OFFSETOF(FieldExit, absentBits)));
    cannot.append(jit.branchTest8(CCallHelpers::NonZero, memberOfExit(OBJECT_OFFSETOF(FieldExit, wasInSlot))));
    if (mode != PutMode::Define) {
        jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilterGenerationForStores)), scratch);
        cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, generationForStores)), scratch));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesChangedOnChains)), scratch);
        jit.urshift32(name, TrustedImm32(6), extra);
        jit.load64(BaseIndex(scratch, extra, CCallHelpers::TimesEight), scratch);
        cannot.append(branchTestBitAt(jit, CCallHelpers::NonZero, scratch, name));
    }
    Jump isCreated = jit.jump();
    isInSlot.link(&jit);
    cannot.append(jit.branchTest8(CCallHelpers::Zero, memberOfExit(OBJECT_OFFSETOF(FieldExit, wasInSlot))));
    isCreated.link(&jit);

    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, extraPropertiesBefore)), scratch);
    Jump hasNone = jit.branchTest32(CCallHelpers::Zero, scratch);
    jit.load16(memberOfExit(OBJECT_OFFSETOF(FieldExit, offsetOfExtraPropertiesWord)), extra);
    loadExtraProperties(jit, object, extra, extra, cannot);
    cannot.append(jit.branch32(CCallHelpers::NotEqual, Address(extra, JSCell::structureIDOffset()), scratch));
    cannot.append(branchIfBarrierIsNeeded(jit, extra, scratch));
    Jump hasExtraProperties = jit.jump();

    hasNone.link(&jit);
    jit.load16(memberOfExit(OBJECT_OFFSETOF(FieldExit, extraPropertiesCellSize)), scratch);
    jit.urshift32(TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, scratch, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    cannot.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, extra, allocator, scratch, cannot);
    jit.store32(TrustedImm32(JSFinalObject::defaultTypeInfoBlob()), Address(extra, JSCell::indexingTypeAndMiscOffset()));
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), Address(extra, JSObject::butterflyOffset()));
    jit.load16(memberOfExit(OBJECT_OFFSETOF(FieldExit, extraPropertiesCellSize)), scratch);
    Label clears = jit.label();
    jit.sub32(TrustedImm32(sizeof(EncodedJSValue)), scratch);
    jit.store64(TrustedImm32(0), BaseIndex(extra, scratch, CCallHelpers::TimesOne));
    jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(JSObject::offsetOfInlineStorage())).linkTo(clears, &jit);

    hasExtraProperties.link(&jit);
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, offset)), scratch);
    jit.store64(value, BaseIndex(extra, scratch, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, extraPropertiesAfter)), scratch);
    jit.store32(scratch, Address(extra, JSCell::structureIDOffset()));
    fenceAfterAllocation(jit);
    jit.load16(memberOfExit(OBJECT_OFFSETOF(FieldExit, offsetOfExtraPropertiesWord)), allocator);
    if (jit.options().useWideReferences)
        jit.store64(extra, BaseIndex(object, allocator, CCallHelpers::TimesOne));
    else {
        encodeCell32(jit, extra, scratch);
        jit.store32(scratch, BaseIndex(object, allocator, CCallHelpers::TimesOne));
    }
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, orderWordAfter)), scratch);
    jit.store32(scratch, orderWord);
    setFlagThatFieldHasLeft(jit, object, scratch);
    Jump neverBearsMark = jit.branchTest8(CCallHelpers::Zero, memberOfExit(OBJECT_OFFSETOF(FieldExit, widthOfSlot)), TrustedImm32(FieldExit::objectMayBearMark));
    setMarkOfCell(jit, object, scratch);
    neverBearsMark.link(&jit);
    jit.load64(memberOfExit(OBJECT_OFFSETOF(FieldExit, absentBits)), scratch);
    Jump isWide = branchIfSlotIsWide();
    jit.store32(scratch, slot);
    Jump isCleared = jit.jump();
    isWide.link(&jit);
    jit.store64(scratch, slot);
    isCleared.link(&jit);
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, structureAfter)), scratch);
    jit.store32(scratch, Address(object, JSCell::structureIDOffset()));
    jit.returnToCaller();

    cannot.link(&jit);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    jit.jumpToStub(putByIdBehindFieldTable(mode));
}

void repeatFieldExitFoundBySlotAndReturn(StubAssembler& jit, const FieldExitBySlotRegisters& r, JumpList& cannot)
{
    const GPRReg object = r.object;
    const GPRReg offsetOfSlot = r.addressOfSlot;
    const GPRReg exit = r.exit;
    const GPRReg scratch = r.scratch;
    const GPRReg extra = r.extra;
    RELEASE_ASSERT(noOverlap(object, r.value, offsetOfSlot, exit, scratch, extra, instanceGPR));
    const BaseIndex slot(object, offsetOfSlot, CCallHelpers::TimesOne);
    const Address orderWord(object, offsetOfOrderWord);
    auto memberOfExit = [&](ptrdiff_t offset) {
        return Address(exit, static_cast<int32_t>(offset));
    };
    jit.subPtr(object, offsetOfSlot);
    jit.load32(Address(object, JSCell::structureIDOffset()), scratch);
    jit.urshift32(scratch, TrustedImm32(StructureRowWords::granuleShift), exit);
    jit.urshift32(offsetOfSlot, TrustedImm32(2), extra);
    jit.add32(extra, exit);
    jit.and32(TrustedImm32(FieldExit::numberOfPlacesBySlot - 1), exit);
    jit.load8(BaseIndex(instanceGPR, exit, CCallHelpers::TimesOne, AOT_INSTANCE_OFFSET(propertyCaches.placesOfFieldExitsBySlot)), exit);
    jit.lshift32(TrustedImm32(FieldExit::sizeShift), exit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.fieldExits)), extra);
    cannot.append(jit.branchTestPtr(CCallHelpers::Zero, extra));
    jit.addPtr(extra, exit);
    cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, structureBefore)), scratch));
    cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, offsetOfSlot)), offsetOfSlot));
    cannot.append(jit.branchTest8(CCallHelpers::Zero, memberOfExit(OBJECT_OFFSETOF(FieldExit, widthOfSlot))));
    cannot.append(jit.branchTest8(CCallHelpers::Zero, memberOfExit(OBJECT_OFFSETOF(FieldExit, wasInSlot))));
    jit.load32(orderWord, scratch);
    cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, orderWordBefore)), scratch));
    cannot.append(branchIfBarrierIsNeeded(jit, object, scratch));
    cannot.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(frameCellState)));
    cannot.append(jit.branchIfOther(r.value, scratch));
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, extraPropertiesBefore)), scratch);
    cannot.append(jit.branchTest32(CCallHelpers::Zero, scratch));
    jit.load16(memberOfExit(OBJECT_OFFSETOF(FieldExit, offsetOfExtraPropertiesWord)), extra);
    loadExtraProperties(jit, object, extra, extra, cannot);
    cannot.append(jit.branch32(CCallHelpers::NotEqual, Address(extra, JSCell::structureIDOffset()), scratch));
    cannot.append(branchIfBarrierIsNeeded(jit, extra, scratch));

    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, offset)), scratch);
    jit.store64(r.value, BaseIndex(extra, scratch, CCallHelpers::TimesEight, JSObject::offsetOfInlineStorage()));
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, extraPropertiesAfter)), scratch);
    jit.store32(scratch, Address(extra, JSCell::structureIDOffset()));
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, orderWordAfter)), scratch);
    jit.store32(scratch, orderWord);
    setFlagThatFieldHasLeft(jit, object, scratch);
    Jump neverBearsMark = jit.branchTest8(CCallHelpers::Zero, memberOfExit(OBJECT_OFFSETOF(FieldExit, widthOfSlot)), TrustedImm32(FieldExit::objectMayBearMark));
    setMarkOfCell(jit, object, scratch);
    neverBearsMark.link(&jit);
    jit.load64(memberOfExit(OBJECT_OFFSETOF(FieldExit, absentBits)), scratch);
    Jump isWide = jit.branchTest8(CCallHelpers::NonZero, memberOfExit(OBJECT_OFFSETOF(FieldExit, widthOfSlot)), TrustedImm32(sizeof(uint64_t)));
    jit.store32(scratch, slot);
    Jump isCleared = jit.jump();
    isWide.link(&jit);
    jit.store64(scratch, slot);
    isCleared.link(&jit);
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, structureAfter)), scratch);
    jit.store32(scratch, Address(object, JSCell::structureIDOffset()));
    jit.returnToCaller();
}

void generatePutBooleanOrDoubleField(StubAssembler& jit)
{
    static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1) && JSValue::ValueTrue <= UINT8_MAX);
    static_assert(traitsOf(SlotRepresentation::OptionalBoolean).width == 1 && !traitsOf(SlotRepresentation::OptionalBoolean).absentBits);
    static_assert(traitsOf(SlotRepresentation::OptionalDouble).absentBits == absentDouble && !traitsOf(SlotRepresentation::Double).hasAbsent);
    static_assert(sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo);
    const PutMode mode = static_cast<PutMode>(jit.key().form());
    const GPRReg object = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg operand = jit.clobberOperand(2);
    const GPRReg name = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const FPRReg number = CCallHelpers::fpTempRegister;
    RELEASE_ASSERT(noOverlap(object, value, operand, name, scratch));
    auto first = [](uint32_t firstOperand) { return static_cast<int32_t>((1u << FieldTableEntry::formShift) + firstOperand); };
    JumpList isStoredByItsRecord;
    JumpList isCreated;
    auto leaveUnlessBoolean = [&] {
        jit.and64(TrustedImm32(~1), value, scratch);
        isStoredByItsRecord.append(jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm32(JSValue::ValueFalse)));
    };
    auto goOnWithRecord = [&](StubKey stub) {
        JumpList isNotCompiledObject;
        locateFieldTableEntry(jit, { object, name, operand, scratch, true, true }, isNotCompiledObject);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), scratch);
        jit.load16(BaseIndex(scratch, operand, CCallHelpers::TimesTwo), scratch);
        jit.and32(TrustedImm32(FieldRecordIndex::indexMask), scratch);
        loadOffsetOfFieldRecord(jit, scratch, name);
        jit.jumpToStub(stub);
        isNotCompiledObject.link(&jit);
        jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
        jit.jumpToStub(putByIdBehindFieldTable(mode));
    };

    Jump isDouble = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstDouble)));
    isStoredByItsRecord.append(jit.branch32(CCallHelpers::Below, operand, TrustedImm32(first(FieldTableEntry::firstEncodedByte))));
    Jump isNotEncodedByte = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstBoolean)));
    const BaseIndex encodedByteSlot(object, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstEncodedByte));
    leaveUnlessBoolean();
    isCreated.append(jit.branchTest8(CCallHelpers::Zero, encodedByteSlot));
    jit.store8(value, encodedByteSlot);
    jit.returnToCaller();

    isNotEncodedByte.link(&jit);
    leaveUnlessBoolean();
    jit.and32(TrustedImm32(1), value, scratch);
    jit.store8(scratch, BaseIndex(object, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstBoolean)));
    jit.returnToCaller();

    isDouble.link(&jit);
    isStoredByItsRecord.append(jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::endOfDoubles))));
    const BaseIndex doubleSlot(object, operand, CCallHelpers::TimesEight, -first(FieldTableEntry::firstDouble) * static_cast<int32_t>(sizeof(double)));
    isStoredByItsRecord.append(jit.branchIfNotNumber(value));
    jit.load64(doubleSlot, scratch);
    isCreated.append(jit.branch64(CCallHelpers::Equal, scratch, TrustedImm64(static_cast<int64_t>(absentDouble))));
    Jump isNotInt32 = jit.branchIfNotInt32(value);
    jit.convertInt32ToDouble(value, number);
    jit.storeDouble(number, doubleSlot);
    jit.returnToCaller();

    isNotInt32.link(&jit);
    unboxDouble(jit, value, scratch, number);
    jit.storeDouble(number, doubleSlot);
    jit.returnToCaller();

    isCreated.link(&jit);
    goOnWithRecord(createKnownField(mode));

    isStoredByItsRecord.link(&jit);
    goOnWithRecord(putKnownField(mode));
}

void loadSlotKind(StubAssembler& jit, const ProbedSlot& slot, GPRReg kind)
{
    switch (slot.form) {
    case SlotForm::Slot:
        extractBits(jit, slot.header, Slot::kindShiftInHeader, 8 * sizeof(Slot::Kind), kind);
        return;
    case SlotForm::PackedSlot:
        extractBits(jit, slot.header, kindShiftInCell, PackedSlot::kindBits, kind);
        jit.xor32(TrustedImm32(PackedSlot::ownInlineValueBits & PackedSlot::kindMask), kind);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void loadOwnInlineValue(StubAssembler& jit, GPRReg object, GPRReg cell, GPRReg result)
{
#if CPU(X86_64)
    jit.zeroExtend32ToWord(cell, cell);
    jit.load64(BaseIndex(object, cell, CCallHelpers::TimesEight), result);
#else
    jit.load64(BaseIndex(object, cell, CCallHelpers::TimesEight, 0, CCallHelpers::Extend::ZExt32), result);
#endif
}

static void loadShortWordIndex(StubAssembler& jit, SlotForm form, GPRReg header, GPRReg result)
{
    switch (form) {
    case SlotForm::Slot:
        jit.rshift64(header, TrustedImm32(Slot::locationShiftInHeader), result);
        return;
    case SlotForm::PackedSlot:
        extractSignedBits(jit, header, locationShiftInCell, locationBitsInCell, result);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct FlagBit {
    unsigned position;
    bool isInverted;
};

} // namespace

static FlagBit flagBitInHeader(uint8_t slotFlag)
{
    RELEASE_ASSERT(hasOneBitSet(slotFlag) && (slotFlag & Slot::allFlags));
    return { Slot::flagsShiftInHeader + getLSBSet(slotFlag), false };
}

static FlagBit flagBitInCell(uint8_t packedFlag)
{
    RELEASE_ASSERT(hasOneBitSet(packedFlag) && !(packedFlag & PackedSlot::kindMask));
    return { kindShiftInCell + getLSBSet(packedFlag), !!(PackedSlot::ownInlineValueBits & packedFlag) };
}

static uint8_t packedFlagFor(Slot::Kind kind, uint8_t slotFlag)
{
    if (slotFlag == Slot::isOwn)
        return PackedSlot::isOwn;
    if (slotFlag == Slot::isOutOfLine)
        return PackedSlot::isOutOfLine;
    RELEASE_ASSERT(slotFlag == Slot::kindFlagOf(kind));
    return PackedSlot::kindFlag;
}

static Jump branchTestFlag(StubAssembler& jit, CCallHelpers::ResultCondition condition, GPRReg word, FlagBit flag)
{
    if (flag.isInverted)
        condition = CCallHelpers::invert(condition);
#if CPU(X86_64)
    return jit.branchTestBit64(condition, word, TrustedImm32(flag.position));
#else
    return jit.branchTest64(condition, word, TrustedImm64(static_cast<int64_t>(1ull << flag.position)));
#endif
}

Jump CachedSlot::branchIfFlag(StubAssembler& jit, uint8_t slotFlag) const
{
    return branchTestFlag(jit, CCallHelpers::NonZero, m_header, m_form == SlotForm::Slot ? flagBitInHeader(slotFlag) : flagBitInCell(packedFlagFor(m_kind, slotFlag)));
}

Jump CachedSlot::branchUnlessFlag(StubAssembler& jit, uint8_t slotFlag) const
{
    return branchTestFlag(jit, CCallHelpers::Zero, m_header, m_form == SlotForm::Slot ? flagBitInHeader(slotFlag) : flagBitInCell(packedFlagFor(m_kind, slotFlag)));
}

void CachedSlot::loadOwnWordIndex(StubAssembler& jit, GPRReg result) const
{
    const SlotPayload payload = Slot::payloadOf(m_kind);
    if (payload == SlotPayload::WordIndex) {
        RELEASE_ASSERT(m_form == SlotForm::Slot);
        jit.signExtend32ToPtr(m_payload, result);
        return;
    }
    RELEASE_ASSERT(payload == SlotPayload::Holder || payload == SlotPayload::Transition || payload == SlotPayload::Removal);
    if (payload == SlotPayload::Holder && m_form == SlotForm::PackedSlot)
        jit.signExtend32ToPtr(m_header, result);
    else
        loadShortWordIndex(jit, m_form, m_header, result);
}

void CachedSlot::loadInheritedWordIndex(StubAssembler& jit, GPRReg result) const
{
    const SlotPayload payload = Slot::payloadOf(m_kind);
    RELEASE_ASSERT(payload == SlotPayload::Holder || payload == SlotPayload::FieldAndHolder || payload == SlotPayload::Transition);
    loadShortWordIndex(jit, m_form, m_header, result);
}

void CachedSlot::loadHolder(StubAssembler& jit, GPRReg result) const
{
    if (Slot::payloadOf(m_kind) == SlotPayload::FieldAndHolder) {
        loadSecondPayloadWord(jit, result);
        decodeCell32(jit, result, result);
        return;
    }
    RELEASE_ASSERT(Slot::payloadOf(m_kind) == SlotPayload::Holder);
    switch (m_form) {
    case SlotForm::Slot:
        jit.move(m_payload, result);
        return;
    case SlotForm::PackedSlot:
#if CPU(ARM64)
        jit.m_assembler.add<64>(result, cellCageBaseGPR, m_header, ARM64Assembler::UXTW, cell32Shift);
#else
        jit.zeroExtend32ToWord(m_header, result);
        decodeCell32(jit, result, result);
#endif
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CachedSlot::loadHiddenWordOffset(StubAssembler& jit, GPRReg result) const
{
    RELEASE_ASSERT(Slot::locationOf(m_kind) == SlotLocation::HiddenWordOffset);
    switch (m_form) {
    case SlotForm::Slot:
        jit.urshift64(m_header, TrustedImm32(Slot::locationShiftInHeader), result);
        return;
    case SlotForm::PackedSlot:
        RELEASE_ASSERT(Slot::payloadOf(m_kind) != SlotPayload::ExtraPropertyAddition);
        jit.urshift64(m_header, TrustedImm32(locationShiftInCell - PackedSlot::hiddenWordOffsetShift), result);
        jit.and32(TrustedImm32(((1 << locationBitsInCell) - 1) << PackedSlot::hiddenWordOffsetShift), result);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

GPRReg CachedSlot::registerOfFirstPayloadWord() const
{
    const SlotPayload payload = Slot::payloadOf(m_kind);
    if (payload == SlotPayload::Field || payload == SlotPayload::Transition || payload == SlotPayload::CustomAccessor)
        return m_form == SlotForm::Slot ? m_payload : m_header;
    RELEASE_ASSERT(m_form == SlotForm::Slot && (payload == SlotPayload::WordIndex || payload == SlotPayload::FieldAndHolder || payload == SlotPayload::ExtraPropertyAddition || payload == SlotPayload::Removal || payload == SlotPayload::OrderWords));
    return m_payload;
}

void CachedSlot::loadFirstPayloadWord(StubAssembler& jit, GPRReg result) const
{
    jit.zeroExtend32ToWord(registerOfFirstPayloadWord(), result);
}

void CachedSlot::loadSecondPayloadWord(StubAssembler& jit, GPRReg result) const
{
    const SlotPayload payload = Slot::payloadOf(m_kind);
    const bool mayBePacked = payload == SlotPayload::Field || payload == SlotPayload::ExtraProperty || payload == SlotPayload::Transition;
    if (m_form == SlotForm::Slot) {
        RELEASE_ASSERT(mayBePacked || payload == SlotPayload::FieldAndHolder || payload == SlotPayload::ExtraPropertyAddition || payload == SlotPayload::Removal || payload == SlotPayload::OrderWords);
        jit.urshift64(m_payload, TrustedImm32(secondPayloadWordShift), result);
        return;
    }
    RELEASE_ASSERT(mayBePacked);
    if (payload == SlotPayload::Field)
        jit.move(TrustedImm32(0), result);
    else if (payload == SlotPayload::ExtraProperty)
        jit.zeroExtend32ToWord(m_header, result);
    else {
        loadShortWordIndex(jit, m_form, m_header, result);
        computeStorageCapacity(jit, result);
    }
}

void CachedSlot::loadVariableBase(StubAssembler& jit, GPRReg receiver, GPRReg result, GPRReg scratch) const
{
    RELEASE_ASSERT(m_kind == Slot::Kind::Variable && noOverlap(receiver, result, scratch));
    RELEASE_ASSERT(m_form == SlotForm::PackedSlot || noOverlap(m_payload, receiver, result, scratch));
    if (!jit.options().hasModuleInstances) {
        loadStaticsBase(jit, result, scratch);
        return;
    }
    Jump isInModuleInstance = branchIfFlag(jit, Slot::isInModuleInstance);
    loadStaticsBase(jit, result, scratch);
    JumpList hasBase;
    hasBase.append(jit.jump());

    isInModuleInstance.link(&jit);
    if (m_form == SlotForm::Slot) {
        loadSecondPayloadWord(jit, result);
        Jump belongsToReceiver = jit.branchTest32(CCallHelpers::Zero, result);
        decodeCell32(jit, result, result);
        hasBase.append(jit.jump());
        belongsToReceiver.link(&jit);
    }
    jit.loadPtr(Address(receiver, ModuleNamespace::offsetOfModuleInstance()), result);
    hasBase.link(&jit);
}

void CachedSlot::loadPayloadBits(StubAssembler& jit, GPRReg result) const
{
    if (m_form == SlotForm::Slot) {
        jit.move(m_payload, result);
        return;
    }
    RELEASE_ASSERT(result != m_header);
    switch (Slot::payloadOf(m_kind)) {
    case SlotPayload::None:
    case SlotPayload::Key:
        jit.move(TrustedImm32(0), result);
        return;
    case SlotPayload::Holder: {
        jit.move(TrustedImm32(0), result);
        Jump isOwn = branchIfFlag(jit, Slot::isOwn);
        loadHolder(jit, result);
        isOwn.link(&jit);
        return;
    }
    case SlotPayload::Field:
    case SlotPayload::CustomAccessor:
        loadFirstPayloadWord(jit, result);
        return;
    case SlotPayload::ExtraProperty:
        jit.lshift64(m_header, TrustedImm32(secondPayloadWordShift), result);
        return;
    case SlotPayload::Transition: {
        jit.move(TrustedImm32(0), result);
        Jump keepsStorage = branchUnlessFlag(jit, Slot::growsStorage);
        loadSecondPayloadWord(jit, result);
        keepsStorage.link(&jit);
        joinPayloadWords(jit, m_header, result);
        return;
    }
    case SlotPayload::WordIndex:
    case SlotPayload::FieldAndHolder:
    case SlotPayload::ExtraPropertyAddition:
    case SlotPayload::Removal:
    case SlotPayload::OrderWords:
    case SlotPayload::Constant:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void loadStructureIDOfReceiver(StubAssembler& jit, GPRReg receiver, GPRReg structureID, JumpList& hasNoStructure, const ScopedLambda<void()>& probeCell)
{
    RELEASE_ASSERT(noOverlap(receiver, structureID, instanceGPR));
    RELEASE_ASSERT(!Thread::currentSingleton().stack().contains(&hasNoStructure));
    Jump isNotCell = jit.branchIfNotCell(receiver);
    probeCell();
    jit.load32(Address(receiver, JSCell::structureIDOffset()), structureID);
    Label hasID = jit.label();
    jit.addOutOfLinePart([isNotCell, hasID, receiver, structureID, exits = &hasNoStructure](StubAssembler& jit) {
        isNotCell.link(&jit);
        Jump isNumber = jit.branchIfNumber(receiver);
        exits->append(jit.branchIfNotBoolean(receiver, structureID));
        jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.booleanStructureID)), structureID);
        jit.jump().linkTo(hasID, &jit);

        isNumber.link(&jit);
        jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.numberStructureID)), structureID);
        jit.jump().linkTo(hasID, &jit);
    });
}

#if CPU(X86_64)
void locateFrontCell(StubAssembler& jit, SlotAccess access, GPRReg structureIDAndIndex, GPRReg column, GPRReg plane)
{
    const int32_t front = offsetOfFrontWords(access);
    RELEASE_ASSERT(noOverlap(structureIDAndIndex, column, plane, instanceGPR));
    jit.urshift32(TrustedImm32(StructureRowWords::granuleShift), structureIDAndIndex);
    jit.loadPtr(Address(instanceGPR, front + static_cast<int32_t>(OBJECT_OFFSETOF(FrontWords, rowsForStructureIDZero))), plane);
    jit.load32(BaseIndex(plane, structureIDAndIndex, CCallHelpers::TimesFour), structureIDAndIndex);
    jit.loadPtr(Address(instanceGPR, front + static_cast<int32_t>(OBJECT_OFFSETOF(FrontWords, plane))), plane);
    jit.add32(column, structureIDAndIndex);
}
#endif

void loadFrontCell(StubAssembler& jit, SlotAccess access, const FrontProbeRegisters& r)
{
    static_assert(!OBJECT_OFFSETOF(FrontWords, rowsForStructureIDZero) && OBJECT_OFFSETOF(FrontWords, plane) == sizeof(void*));
    RELEASE_ASSERT(noOverlap(r.structureID, r.column, r.row, r.cell, r.scratch, instanceGPR));
    jit.loadPair64(instanceGPR, TrustedImm32(offsetOfFrontWords(access)), r.scratch, r.cell);
    jit.urshift32(r.structureID, TrustedImm32(StructureRowWords::granuleShift), r.row);
    jit.load32(BaseIndex(r.scratch, r.row, CCallHelpers::TimesFour), r.row);
    jit.add64(r.column, r.row);
    jit.load64(BaseIndex(r.cell, r.row, CCallHelpers::TimesEight), r.cell);
}

Jump branchUnlessOwnInlineValue(StubAssembler& jit, GPRReg column, GPRReg cell, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(column, cell, scratch));
#if CPU(ARM64)
    jit.xorLeftShift64(cell, column, TrustedImm32(nameShiftInCell), scratch);
    jit.urshift64(TrustedImm32(locationShiftInCell), scratch);
    return jit.branchTest64(CCallHelpers::NonZero, scratch);
#else
    jit.lshift64(column, TrustedImm32(nameShiftInCell), scratch);
    jit.xor64(cell, scratch);
    jit.urshift64(TrustedImm32(locationShiftInCell), scratch);
    return Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE));
#endif
}

Jump branchIfCellNameDiffers(StubAssembler& jit, GPRReg column, GPRReg cell, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(column, cell, scratch));
#if CPU(ARM64)
    jit.subUnsignedRightShift64(column, cell, TrustedImm32(nameShiftInCell), scratch);
    return jit.branchTest64(CCallHelpers::NonZero, scratch);
#else
    jit.urshift64(cell, TrustedImm32(nameShiftInCell), scratch);
    return jit.branch32(CCallHelpers::NotEqual, scratch, column);
#endif
}

void loadProgramSymbol(StubAssembler& jit, GPRReg identifierNumber, GPRReg uid, JumpList& isNotCreated)
{
    static_assert(sizeof(ProgramSymbol) == 2u << CCallHelpers::TimesEight);
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg scratch = ownedScratch;
    RELEASE_ASSERT(noOverlap(identifierNumber, uid, scratch, instanceGPR));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programSymbols)), uid);
    jit.load32(Address(uid, OBJECT_OFFSETOF(ProgramSymbols, mask)), scratch);
    jit.loadPtr(Address(uid, OBJECT_OFFSETOF(ProgramSymbols, entries)), uid);
    jit.and32(identifierNumber, scratch);
    jit.lshift32(TrustedImm32(1), scratch);
    jit.getEffectiveAddress(BaseIndex(uid, scratch, CCallHelpers::TimesEight), uid);
    Label nextEntry = jit.label();
    jit.load32(Address(uid, OBJECT_OFFSETOF(ProgramSymbol, number)), scratch);
    isNotCreated.append(jit.branchTest32(CCallHelpers::Zero, scratch));
    Jump isFound = jit.branch32(CCallHelpers::Equal, scratch, identifierNumber);
    jit.addPtr(TrustedImm32(sizeof(ProgramSymbol)), uid);
    jit.jump().linkTo(nextEntry, &jit);

    isFound.link(&jit);
    jit.loadPtr(Address(uid, OBJECT_OFFSETOF(ProgramSymbol, uid)), uid);
}

void loadIdentifier(StubAssembler& jit, GPRReg identifierNumber, GPRReg uid, JumpList& isNotCreated)
{
    static_assert(sizeof(UniquedStringImpl) == 3u << CCallHelpers::TimesEight);
    std::optional<OwnedScratchRegister> ownedScratch;
    ownedScratch.emplace(jit);
    const GPRReg scratch = ownedScratch->gpr();
    RELEASE_ASSERT(noOverlap(identifierNumber, uid, scratch, instanceGPR));
    JumpList isAtomOfImage;
    if (jit.options().isPositionIndependent) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), scratch);
        jit.load32(BaseIndex(scratch, identifierNumber, CCallHelpers::TimesFour), uid);
        ownedScratch.reset();
        isAtomOfImage.append(jit.branchTest32(CCallHelpers::NonZero, uid));

        std::array<GPRReg, 3> spare;
        size_t numberOfSpares = 0;
        for (StubRegister candidate : { StubRegister::T10, StubRegister::T11, StubRegister::T12, StubRegister::T13, StubRegister::T14 }) {
            if (numberOfSpares < spare.size() && noOverlap(gprOf(candidate), identifierNumber, uid, scratch))
                spare[numberOfSpares++] = gprOf(candidate);
        }
        RELEASE_ASSERT(numberOfSpares == spare.size());
        jit.preserving(spare, [&] {
            const auto [record, lazyHeaders, word] = spare;
            JumpList cannot;
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(lazyStrings.headers)), lazyHeaders);
            createLazyHeader(jit, { identifierNumber, record, uid, lazyHeaders, word }, 0, cannot);
            Jump isCreated = jit.jump();
            cannot.link(&jit);
            jit.move(TrustedImm32(0), uid);
            isCreated.link(&jit);
        });
        isAtomOfImage.append(jit.branchTest32(CCallHelpers::NonZero, uid));
    } else {
        jit.getEffectiveAddress(BaseIndex(identifierNumber, identifierNumber, CCallHelpers::TimesTwo), uid);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), scratch);
        jit.getEffectiveAddress(BaseIndex(scratch, uid, CCallHelpers::TimesEight), uid);
        ownedScratch.reset();
        isAtomOfImage.append(jit.branchTest32(CCallHelpers::NonZero, Address(uid, StringImpl::lengthMemoryOffset())));
    }

    loadProgramSymbol(jit, identifierNumber, uid, isNotCreated);
    if (jit.options().isPositionIndependent) {
        Jump isSymbol = jit.jump();

        isAtomOfImage.link(&jit);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(lazyStrings.headers)), uid);
        isSymbol.link(&jit);
    } else {
        isAtomOfImage.link(&jit);
    }
}

void findHashedSlot(StubAssembler& jit, SlotAccess access, const HashedProbeRegisters& r, JumpList& miss)
{
    constexpr int32_t offsetOfTable = AOT_INSTANCE_OFFSET(propertyCaches.hashedSlots);
    constexpr ptrdiff_t offsetOfStructureID = HashedSlot::offsetOfSlot() + Slot::offsetOfStructureID();
    RELEASE_ASSERT(noOverlap(r.structureID, r.uid, r.entry, r.header, r.payload, instanceGPR));
    RELEASE_ASSERT(noOverlap(r.structureID, r.uid, r.entry, r.header, r.scratch, instanceGPR));
    RELEASE_ASSERT(r.kind == InvalidGPRReg || (noOverlap(r.kind, r.entry, r.header, r.payload, instanceGPR) && r.kind != r.scratch));

    jit.mul32(TrustedImm32(static_cast<int32_t>(HashedSlot::structureMultiplier)), r.structureID, r.header);
    jit.urshift32(TrustedImm32(HashedSlot::structureHashShift), r.header);
#if CPU(X86_64)
    auto inEntry = [&](ptrdiff_t offset) { return BaseIndex(r.scratch, r.entry, CCallHelpers::TimesOne, static_cast<int32_t>(offset)); };
    jit.x86Lea32(BaseIndex(r.header, r.uid, static_cast<CCallHelpers::Scale>(HashedSlot::uidHashShift)), r.entry);
    jit.and64(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.hashedSlotOffsetMask)), r.entry);
    jit.loadPtr(Address(instanceGPR, offsetOfTable), r.scratch);
#else
    auto inEntry = [&](ptrdiff_t offset) { return Address(r.header, static_cast<int32_t>(offset)); };
    jit.addLeftShift32(r.header, r.uid, TrustedImm32(HashedSlot::uidHashShift), r.entry);
    jit.loadPair64(instanceGPR, TrustedImm32(offsetOfTable), r.scratch, r.header);
    jit.and64(r.header, r.entry);
#endif
    if (access != SlotAccess::Read)
        jit.or64(TrustedImm32(static_cast<int32_t>(access)), r.uid);

    JumpList isFound;
    for (unsigned way = 0; way < HashedSlot::ways; ++way) {
        if (way)
            jit.xor32(TrustedImm32(HashedSlot::offsetOfWay(0, way) ^ HashedSlot::offsetOfWay(0, way - 1)), r.entry);
#if CPU(ARM64)
        jit.add64(r.scratch, r.entry, r.header);
#endif
        Jump hasOtherKey = jit.branchPtr(CCallHelpers::NotEqual, inEntry(HashedSlot::offsetOfKey()), r.uid);
        if (way + 1 < HashedSlot::ways) {
            isFound.append(jit.branch32(CCallHelpers::Equal, inEntry(offsetOfStructureID), r.structureID));
            hasOtherKey.link(&jit);
            continue;
        }
        miss.append(hasOtherKey);
        miss.append(jit.branch32(CCallHelpers::NotEqual, inEntry(offsetOfStructureID), r.structureID));
    }
    isFound.link(&jit);
#if CPU(X86_64)
    if (r.kind != InvalidGPRReg)
        jit.load8(inEntry(HashedSlot::offsetOfSlot() + Slot::offsetOfKind()), r.kind);
    jit.load64(inEntry(HashedSlot::offsetOfSlot()), r.header);
    jit.load64(inEntry(HashedSlot::offsetOfSlot() + Slot::offsetOfPayload()), r.payload);
#else
    jit.loadPair64(inEntry(HashedSlot::offsetOfSlot()), r.header, r.payload);
    if (r.kind != InvalidGPRReg)
        loadSlotKind(jit, { SlotForm::Slot, r.header }, r.kind);
#endif
}

void jumpToSlotHandler(StubAssembler& jit, SlotHandlerTable table, GPRReg header, GPRReg scratch)
{
    loadSlotKind(jit, { traitsOf(table).form, header }, scratch);
    jumpToSlotHandlerOfKind(jit, table, header, scratch);
}

#if CPU(X86_64)
void callSlotHandler(StubAssembler& jit, SlotHandlerTable table, GPRReg header, GPRReg scratch)
{
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    const std::array handlers = slotHandlerKeys(table);
    RELEASE_ASSERT(header == operands.header);
    RELEASE_ASSERT(noOverlap(scratch, operands.receiver, operands.header, operands.payload, operands.value, operands.keyCell, operands.identifierNumber, instanceGPR));
    loadSlotKind(jit, { traitsOf(table).form, header }, scratch);
    jit.callThroughHandlerTable(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesEight, static_cast<int32_t>(Instance::offsetOfSlotHandlers(table))), handlers, scratch);
}
#endif

void jumpToSlotHandlerOfKind(StubAssembler& jit, SlotHandlerTable table, GPRReg header, GPRReg scratch)
{
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    const std::array handlers = slotHandlerKeys(table);
    const int32_t offsetOfHandlers = static_cast<int32_t>(Instance::offsetOfSlotHandlers(table));
    RELEASE_ASSERT(header == operands.header);
    RELEASE_ASSERT(noOverlap(scratch, operands.receiver, operands.header, operands.payload, operands.value, operands.keyCell, operands.identifierNumber, instanceGPR));
#if CPU(ARM64)
    static_assert(sizeof(Instance) / sizeof(void*) <= 1 << 12, "The index of a handler is the immediate of one add.");
    jit.add32(TrustedImm32(offsetOfHandlers / static_cast<int32_t>(sizeof(void*))), scratch);
    jit.jumpThroughHandlerTable(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesEight), handlers);
#else
    jit.jumpThroughHandlerTable(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesEight, offsetOfHandlers), handlers);
#endif
}

void linkToSlotExit(StubAssembler& jit, JumpList& jumps, SlotHandlerTable table, SlotExit exit)
{
    jit.linkToStub(jumps, slotExit(table, exit));
}

static constexpr bool takesSlotHeader(SlotFinisher finisher)
{
    switch (finisher) {
    case SlotFinisher::ReadField:
    case SlotFinisher::StoreField:
    case SlotFinisher::CreateField:
    case SlotFinisher::GrowStorage:
        return true;
    case SlotFinisher::CallGetter:
    case SlotFinisher::CallSetterStrict:
    case SlotFinisher::CallSetterSloppy:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static void unpackHeader(StubAssembler& jit, const CachedSlot& slot, GPRReg receiver, GPRReg scratch)
{
    const GPRReg header = slot.header();
    const Slot::Kind kind = slot.kind();
    RELEASE_ASSERT(slot.form() == SlotForm::PackedSlot);
    RELEASE_ASSERT(Slot::payloadOf(kind) == SlotPayload::Transition || Slot::payloadOf(kind) == SlotPayload::Field);
    RELEASE_ASSERT(noOverlap(receiver, header, scratch));
    switch (Slot::locationOf(kind)) {
    case SlotLocation::None:
        jit.move(TrustedImm32(0), scratch);
        break;
    case SlotLocation::WordIndex:
        slot.loadOwnWordIndex(jit, scratch);
        break;
    case SlotLocation::HiddenWordOffset:
        slot.loadHiddenWordOffset(jit, scratch);
        break;
    }
    jit.lshift64(TrustedImm32(Slot::locationShiftInHeader), scratch);
    jit.or64(TrustedImm64(static_cast<int64_t>(kind) << Slot::kindShiftInHeader | static_cast<int64_t>(Slot::flagsFixedBy(kind)) << Slot::flagsShiftInHeader), scratch);
    for (uint8_t flag : { Slot::isOwn, Slot::isOutOfLine, Slot::kindFlagOf(kind) }) {
        if (!flag)
            continue;
        Jump isClear = slot.branchUnlessFlag(jit, flag);
        jit.or64(TrustedImm64(static_cast<int64_t>(flag) << Slot::flagsShiftInHeader), scratch);
        isClear.link(&jit);
    }
    jit.load32(Address(receiver, JSCell::structureIDOffset()), header);
    jit.or64(scratch, header);
}

void linkToSlotCannot(StubAssembler& jit, JumpList& jumps, SlotHandlerTable table, SlotFinisher finisher, const CachedSlot& slot)
{
    if (jumps.empty())
        return;
    const SlotHandlerTableTraits traits = traitsOf(table);
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    RELEASE_ASSERT(slot.form() == traits.form && slot.header() == operands.header);
    jumps.link(&jit);
    jumps.clear();
    if (traits.form == SlotForm::PackedSlot) {
        slot.loadPayloadBits(jit, jit.clobberRegister(slotPayloadRegister));
        if (takesSlotHeader(finisher)) {
            jit.clobberRegister(slotHeaderRegister);
            unpackHeader(jit, slot, operands.receiver, jit.clobberRegister(StubRegister::T11));
        }
    }
    if (traits.origin == SlotOrigin::ByVal && operands.value != InvalidGPRReg)
        jit.move(operands.value, jit.clobberRegister(storedValueByIdRegister));
    jit.jumpToStub(slotCannot(finisher, traits.origin));
}

void loadInheritedValueBySlot(StubAssembler& jit, const CachedSlot& slot, GPRReg storage, GPRReg wordIndex, GPRReg result)
{
    RELEASE_ASSERT(noOverlap(slot.header(), storage, wordIndex));
    slot.loadHolder(jit, storage);
    slot.loadInheritedWordIndex(jit, wordIndex);
    Jump isInline = slot.branchUnlessFlag(jit, Slot::inheritedIsOutOfLine);
    jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
    isInline.link(&jit);
    jit.load64(BaseIndex(storage, wordIndex, CCallHelpers::TimesEight), result);
}

static void loadValueOfHolderBySlot(StubAssembler& jit, const CachedSlot& slot, GPRReg holderAndStorage, GPRReg wordIndex, GPRReg result)
{
#if CPU(X86_64)
    jit.m_assembler.btw_ir(flagBitInHeader(Slot::isOutOfLine).position, slot.header());
    jit.m_assembler.cmovq_mr(X86Assembler::ConditionC, JSObject::butterflyOffset(), holderAndStorage, holderAndStorage);
#else
    Jump isInline = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(holderAndStorage, JSObject::butterflyOffset()), holderAndStorage);
    isInline.link(&jit);
#endif
    slot.loadOwnWordIndex(jit, wordIndex);
    jit.load64(BaseIndex(holderAndStorage, wordIndex, CCallHelpers::TimesEight), result);
}

void loadOwnValueBySlot(StubAssembler& jit, const CachedSlot& slot, GPRReg receiver, GPRReg storage, GPRReg wordIndex, GPRReg result)
{
    RELEASE_ASSERT(slot.form() == SlotForm::Slot && Slot::payloadOf(slot.kind()) == SlotPayload::Holder);
    RELEASE_ASSERT(noOverlap(receiver, storage, wordIndex) && noOverlap(slot.header(), storage));
    jit.move(receiver, storage);
    loadValueOfHolderBySlot(jit, slot, storage, wordIndex, result);
}

void loadValueBySlot(StubAssembler& jit, const CachedSlot& slot, GPRReg receiver, GPRReg storage, GPRReg wordIndex, GPRReg result)
{
    RELEASE_ASSERT(Slot::payloadOf(slot.kind()) == SlotPayload::Holder);
    RELEASE_ASSERT(noOverlap(receiver, storage, wordIndex) && noOverlap(slot.header(), storage));
    if (slot.form() == SlotForm::Slot) {
        slot.loadHolder(jit, storage);
        jit.moveConditionallyTest64(CCallHelpers::Zero, storage, storage, receiver, storage);
        loadValueOfHolderBySlot(jit, slot, storage, wordIndex, result);
        return;
    }

    RELEASE_ASSERT(slot.header() != wordIndex);
    Jump isOwn = slot.branchIfFlag(jit, Slot::isOwn);
    slot.loadHolder(jit, storage);
    slot.loadInheritedWordIndex(jit, wordIndex);
    Jump isLocated = jit.jump();

    isOwn.link(&jit);
    jit.move(receiver, storage);
    slot.loadOwnWordIndex(jit, wordIndex);

    isLocated.link(&jit);
    Jump isInline = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
    jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
    isInline.link(&jit);
    jit.load64(BaseIndex(storage, wordIndex, CCallHelpers::TimesEight), result);
}

void returnPresenceBySlot(StubAssembler& jit, PresenceQuestion question, const ProbedSlot& slot, GPRReg receiver, Label asksReceiver, JumpList& isUnknown, KindOfProbedSlot whereKindIs, const ScopedLambda<void()>* rememberThatNothingIsOwn)
{
    constexpr auto bitOf = [](Slot::Kind kind) { return 1u << static_cast<unsigned>(kind); };
    const bool asksIn = question == PresenceQuestion::In;
    const uint32_t presentKinds = kindsWithPresence(SlotPresence::Present) | bitOf(asksIn ? Slot::Kind::FieldOrInherited : Slot::Kind::InheritedField);
    constexpr uint32_t absentKinds = kindsWithPresence(SlotPresence::Absent);
    const GPRReg kind = jit.reuseScratch(StubRegister::T10);
    const GPRReg result = jit.result();
    RELEASE_ASSERT(noOverlap(kind, receiver, slot.header));
    if (whereKindIs == KindOfProbedSlot::IsInHeader)
        loadSlotKind(jit, slot, kind);
#if CPU(X86_64)
    auto branchTestKinds = [&](CCallHelpers::ResultCondition condition, uint32_t kinds) {
        if (hasOneBitSet(kinds))
            return jit.branch32(condition == CCallHelpers::NonZero ? CCallHelpers::Equal : CCallHelpers::NotEqual, kind, TrustedImm32(static_cast<int32_t>(ctz(kinds))));
        const OwnedScratchRegister ownedScratch(jit);
        const GPRReg mask = ownedScratch;
        jit.move(TrustedImm32(static_cast<int32_t>(kinds)), mask);
        return jit.branchTestBit32(condition, mask, kind);
    };
#else
    jit.lshift32(TrustedImm32(1), kind, kind);
    auto branchTestKinds = [&](CCallHelpers::ResultCondition condition, uint32_t kinds) {
        return jit.branchTest32(condition, kind, TrustedImm32(static_cast<int32_t>(kinds)));
    };
#endif
    Jump isAbsent = branchTestKinds(CCallHelpers::NonZero, absentKinds);
    if (asksIn) {
        Jump holderIsNotAsked = branchTestKinds(CCallHelpers::Zero, bitOf(Slot::Kind::InheritedField));
        CachedSlot(slot.form, Slot::Kind::InheritedField, slot.header, slot.payload).loadHolder(jit, receiver);
        jit.jump().linkTo(asksReceiver, &jit);
        holderIsNotAsked.link(&jit);
    }
    isUnknown.append(branchTestKinds(CCallHelpers::Zero, presentKinds));
    switch (question) {
    case PresenceQuestion::In:
        jit.move(TrustedImm32(1), result);
        break;
    case PresenceQuestion::HasOwn: {
        const FlagBit isOwn = slot.form == SlotForm::Slot ? flagBitInHeader(Slot::isOwn) : flagBitInCell(PackedSlot::isOwn);
        Jump isInHolder = branchTestFlag(jit, CCallHelpers::Zero, slot.header, isOwn);
        jit.move(TrustedImm32(1), result);
        jit.returnToCaller();

        isInHolder.link(&jit);
        isUnknown.append(jit.branch8(CCallHelpers::Equal, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(GlobalProxyType)));
        if (rememberThatNothingIsOwn)
            (*rememberThatNothingIsOwn)();
        jit.move(TrustedImm32(0), result);
        break;
    }
    }
    jit.returnToCaller();

    isAbsent.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();
}

void loadStaticsBase(StubAssembler& jit, GPRReg staticsBase, GPRReg roundedSizeOfStatics)
{
    constexpr int32_t offsetOfSize = OBJECT_OFFSETOF(ProgramTables, instanceSizes) + OBJECT_OFFSETOF(InstanceSizes, sizeOfStatics);
    RELEASE_ASSERT(noOverlap(staticsBase, roundedSizeOfStatics, instanceGPR));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), roundedSizeOfStatics);
    jit.load32(Address(roundedSizeOfStatics, offsetOfSize), roundedSizeOfStatics);
    jit.add32(TrustedImm32(staticsGranuleMask), roundedSizeOfStatics);
    jit.and32(TrustedImm32(~staticsGranuleMask), roundedSizeOfStatics);
    jit.sub64(instanceGPR, roundedSizeOfStatics, staticsBase);
    jit.sub64(TrustedImm32(static_cast<int32_t>(InstanceLayout::bytesReservedForPrefixAndPollPage)), staticsBase);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
