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
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeTable.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsConstants.h"
#include "AOTStubsField.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "JSCell.h"
#include "JSObject.h"
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
static_assert(sizeof(SiteCacheState) == sizeof(uint16_t));
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

// InstanceLayout reserves a granule for a size of 0. A slot that names a location in the statics proves that the size is not 0.
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

static std::optional<GPRReg> siteRegisterOf(SlotHandlerTableTraits traits)
{
    switch (traits.origin) {
    case SlotOrigin::FreeName:
    case SlotOrigin::FreeNameForTypeOf:
        return gprOf(traits.operation == SlotOperation::Read ? readSiteCacheRegister : storeSiteCacheRegister);
    case SlotOrigin::ById:
    case SlotOrigin::ByIdInStubFrame:
    case SlotOrigin::ByVal:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    case SlotOrigin::FreeName:
    case SlotOrigin::FreeNameForTypeOf:
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
    const GPRReg site = siteRegisterOf(traitsOf(table)).value_or(InvalidGPRReg);
    StubSignature signature = isReadTable(table) ? StubSignature::withGPRResult({ }) : StubSignature::withoutResult({ });
    for (GPRReg operand : { operands.receiver, operands.header, operands.payload, operands.identifierNumber, operands.keyCell, operands.value, site }) {
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
    const GPRReg firstWord = jit.scratchRegister();
    jit.zeroExtend32ToWord(firstWordSource, firstWord);
    jit.or64(firstWord, secondWordAndResult);
#endif
}

// index holds the row of the receiver's shape where the name is bound, and the row plus the name otherwise.
static BaseIndex fieldTableEntry(const FieldTableProbe& probe, int32_t offset = 0)
{
    static_assert(sizeof(FieldTableEntry) == 1u << CCallHelpers::TimesFour);
    return BaseIndex(probe.table, probe.index, CCallHelpers::TimesFour, offset + static_cast<int32_t>(probe.boundName.value_or(0) * sizeof(FieldTableEntry)));
}

static void locateFieldTableEntry(StubAssembler& jit, const FieldTableProbe& probe, JumpList& isNotCompiledObject)
{
    RELEASE_ASSERT(noOverlap(probe.receiver, probe.index, probe.table) && (probe.boundName || noOverlap(probe.name, probe.receiver, probe.index, probe.table)));
    RELEASE_ASSERT(!probe.boundName || FieldTableEntry::hasColumn(*probe.boundName));

    if (!probe.nameHasColumn)
        isNotCompiledObject.append(jit.branch32(CCallHelpers::Above, probe.name, TrustedImm32(jit.options().maxNameInFieldTable)));
    if (!probe.receiverIsCell)
        isNotCompiledObject.append(jit.branchIfNotCell(probe.receiver));
    jit.load32SignedExtendTo64(Address(probe.receiver, offsetOfShapeWord), probe.index);
    isNotCompiledObject.append(jit.branchTest32(CCallHelpers::PositiveOrZero, probe.index));
    branchUnlessShapeOfThisImage(jit, probe.index, isNotCompiledObject);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldTableEntries)), probe.table);
    jit.load32(BaseIndex(probe.table, probe.index, CCallHelpers::TimesFour), probe.index);
    if (!probe.boundName)
        jit.add32(probe.name, probe.index);
}

// Where the jump is not taken, index holds the form and the operand of the entry.
static Jump branchUnlessEntryHasName(StubAssembler& jit, const FieldTableProbe& probe)
{
    static_assert(FieldTableEntry::nameShift == 16);
#if CPU(X86_64)
    constexpr int32_t offsetOfName = FieldTableEntry::nameShift / 8;
    Jump hasOtherName;
    if (probe.boundName)
        hasOtherName = jit.branch16(CCallHelpers::NotEqual, fieldTableEntry(probe, offsetOfName), TrustedImm32(static_cast<int32_t>(*probe.boundName)));
    else {
        jit.m_assembler.cmpw_rm(probe.name, offsetOfName, probe.table, probe.index, CCallHelpers::TimesFour);
        hasOtherName = Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE));
    }
    jit.load16(fieldTableEntry(probe), probe.index);
    return hasOtherName;
#else
    jit.load32(fieldTableEntry(probe), probe.index);
    if (probe.boundName)
        jit.move(TrustedImm32(static_cast<int32_t>(*probe.boundName << FieldTableEntry::nameShift)), probe.table);
    else
        jit.lshift32(probe.name, TrustedImm32(FieldTableEntry::nameShift), probe.table);
    jit.xor32(probe.table, probe.index);
    return jit.branch32(CCallHelpers::Above, probe.index, TrustedImm32(FieldTableEntry::maxFormAndOperand));
#endif
}

// operand holds a form and an operand, less the forms in front of `form`. Where the form is `form` it is left with the operand alone, otherwise with that less one form.
static Jump branchUnlessForm(StubAssembler& jit, GPRReg operand, FieldTableEntry::Form form)
{
    if (form != FieldTableEntry::Form::InlineValue)
        jit.sub32(TrustedImm32(1 << FieldTableEntry::formShift), operand);
    return jit.branch32(CCallHelpers::Above, operand, TrustedImm32(FieldTableEntry::maxOperand));
}

// Takes the operand, in its register or as knownOperand. Leaves the address of the Extension in extension and the offset of the slot in operand.
static void locateOutOfLineValue(StubAssembler& jit, GPRReg receiver, GPRReg operand, GPRReg extension, JumpList& hasNoStorage, std::optional<uint32_t> knownOperand = std::nullopt)
{
    // The operand is the number of the granule and one more.
    static_assert(offsetOfFirstExtensionSlot == extensionGranuleSize && extensionGranuleSize == sizeof(EncodedJSValue) && maxExtensionGranules == 64);
    const GPRReg granuleBits = jit.scratchRegister();
    RELEASE_ASSERT(noOverlap(receiver, operand, extension, granuleBits) && !jit.options().useWideReferences);
    jit.load32(Address(receiver, offsetOfFirstField), extension);
    hasNoStorage.append(jit.branchTest32(CCallHelpers::Zero, extension));
    decodeCell32(jit, extension, extension);
    jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), extension);
    jit.load64(Address(extension, offsetOfExtensionGranuleBits), granuleBits);
    if (knownOperand)
        jit.lshift64(TrustedImm32(static_cast<int32_t>(-*knownOperand & 63)), granuleBits);
    else {
        jit.neg32(operand);
        jit.lshift64(operand, granuleBits);
    }
    hasNoStorage.append(jit.branchTest64(CCallHelpers::PositiveOrZero, granuleBits));
    jit.countPopulation64(granuleBits, operand, CCallHelpers::fpTempRegister);
    jit.lshift32(TrustedImm32(WTF::fastLog2(static_cast<unsigned>(sizeof(EncodedJSValue)))), operand);
}

// The row of a shape is complete: it has an entry for every field. So behind a mismatch only the state of the object and the names of Object.prototype are left to ask.
static void branchUnlessNameIsAbsent(StubAssembler& jit, const FieldTableProbe& probe, JumpList& mayBePresent, JumpList* isNotBelowObjectPrototype = nullptr)
{
    static_assert(std::endian::native == std::endian::little);
    const GPRReg structure = probe.index;
    const GPRReg names = probe.table;
    loadStructure(jit, probe.receiver, structure);
    (isNotBelowObjectPrototype ? *isNotBelowObjectPrototype : mayBePresent).append(jit.branchTest8(CCallHelpers::Zero, Address(structure, CompiledStructure::offsetOfFlags()), TrustedImm32(static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesOfObjectPrototypes)), names);
    if (probe.boundName) {
        mayBePresent.append(jit.branchTest8(CCallHelpers::NonZero, Address(names, static_cast<int32_t>(*probe.boundName / 8)), TrustedImm32(1 << *probe.boundName % 8)));
        return;
    }
    jit.urshift32(probe.name, TrustedImm32(6), structure);
    jit.load64(BaseIndex(names, structure, CCallHelpers::TimesEight), names);
    // A shift takes the low six bits of its count.
    jit.urshift64(probe.name, names);
    mayBePresent.append(jit.branchTest64(CCallHelpers::NonZero, names, TrustedImm32(1)));
}

void readFieldByTable(StubAssembler& jit, const FieldTableProbe& probe, GPRReg result, JumpList& otherwise, const ScopedLambda<void()>& finish, const ScopedLambda<void()>* finishWhereAbsent)
{
    using Form = FieldTableEntry::Form;
    const GPRReg operand = probe.index;
    const GPRReg word = probe.table;
    // The slot lies in the cell. Empty: it holds what only an empty slot of the form holds. Silent: it does not hold the value, and the record of the field says what it does hold.
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

    locateFieldTableEntry(jit, probe, otherwise);
    if (probe.expectedEntry) {
        const uint32_t expectedOperand = *probe.expectedEntry & FieldTableEntry::maxOperand;
        const bool isInCell = *probe.expectedEntry == FieldTableEntry::create(*probe.boundName, Form::InlineValue, expectedOperand).bits;
        RELEASE_ASSERT(isInCell || *probe.expectedEntry == FieldTableEntry::create(*probe.boundName, Form::OutOfLineValue, expectedOperand).bits);
        if (isInCell || !jit.options().useWideReferences) {
            Jump isOtherEntry = jit.branch32(CCallHelpers::NotEqual, fieldTableEntry(probe), TrustedImm32(static_cast<int32_t>(*probe.expectedEntry)));
            if (isInCell)
                readValue(Address(probe.receiver, static_cast<int32_t>(expectedOperand * sizeof(EncodedJSValue))), slotIsEmpty);
            else {
                locateOutOfLineValue(jit, probe.receiver, operand, word, slotIsEmpty, expectedOperand);
                readValue(BaseIndex(word, operand, CCallHelpers::TimesOne), slotIsEmpty);
            }
            isOtherEntry.link(&jit);
        }
    }
    Jump isNoField = branchUnlessEntryHasName(jit, probe);
    Jump isNotInlineValue = branchUnlessForm(jit, operand, Form::InlineValue);
    readValue(BaseIndex(probe.receiver, operand, CCallHelpers::TimesEight), slotIsEmpty);

    // operand holds the form and the operand of the entry, less this many forms.
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
            // Where the field may exist without storage it is undefined then, which is also the answer where it does not exist and nothing above has the name.
            locateOutOfLineValue(jit, probe.receiver, operand, word, slotIsEmpty);
            readValue(BaseIndex(word, operand, CCallHelpers::TimesOne), slotIsEmpty);
        }
        isNotOutOfLineValue.link(&jit);
    };
    const bool testsOutOfLineValuesSecond = jit.bindingPolicy().readsTestOutOfLineValuesSecond;
    const bool testsDoublesSecond = jit.bindingPolicy().readsTestDoublesSecond;
    // Infinity goes the long way with the NaNs, which may be markers.
    auto readDouble = [&](int32_t firstDouble) {
        jit.load64(BaseIndex(probe.receiver, operand, CCallHelpers::TimesEight, -firstDouble * static_cast<int32_t>(sizeof(double))), word);
        boxDoubleBits(jit, PureDoubleBits::branchIfNotFinite(jit, word, operand, slotIsSilent));
        jit.move(word, result);
        finish();
    };

    isNotInlineValue.link(&jit);
    // The doubles are the last operands of the last form.
    auto readDoubleOrGoOn = [&] {
        RELEASE_ASSERT(!formsTakenOff);
        constexpr int32_t firstDouble = static_cast<int32_t>((static_cast<uint32_t>(Form::Field) << FieldTableEntry::formShift) + FieldTableEntry::firstDouble);
        Jump isNotDouble = jit.branch32(CCallHelpers::Below, operand, TrustedImm32(firstDouble));
        readDouble(firstDouble);
        isNotDouble.link(&jit);
    };
    // Asked for in their order, with nothing taken off, a form needs its upper bound alone, and the load takes the form off:
    // one operand of x86-64 holds a base, an index and a distance. Elsewhere the distance costs more than taking the form off.
#if CPU(X86_64)
    const bool asksInOrder = !testsOutOfLineValuesSecond;
#else
    const bool asksInOrder = false;
#endif
    auto branchUnlessNextFormIs = [&](Form form) {
        if (!asksInOrder)
            return branchUnlessFormIs(form);
        RELEASE_ASSERT(!formsTakenOff);
        return jit.branch32(CCallHelpers::Above, operand, TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(form) << FieldTableEntry::formShift | FieldTableEntry::maxOperand)));
    };
    auto narrowSlot = [&](Form form) {
        int32_t formInOperand = asksInOrder ? static_cast<int32_t>(static_cast<uint32_t>(form) << FieldTableEntry::formShift) : 0;
        return BaseIndex(probe.receiver, operand, CCallHelpers::TimesFour, -formInOperand * static_cast<int32_t>(sizeof(uint32_t)));
    };
    if (testsDoublesSecond)
        readDoubleOrGoOn();
    if (testsOutOfLineValuesSecond)
        readOutOfLineValue();
    Jump isNotInt32 = branchUnlessNextFormIs(Form::InlineInt32);
    jit.load32(narrowSlot(Form::InlineInt32), word);
    slotIsSilent.append(jit.branch32(CCallHelpers::LessThan, word, TrustedImm32(minInlineInt32)));
    boxInt32(jit, word, result);
    finish();

    isNotInt32.link(&jit);
    const bool testsDoublesThird = asksInOrder && !testsDoublesSecond;
    if (testsDoublesThird)
        readDoubleOrGoOn();
    Jump isNotNarrowCell = branchUnlessNextFormIs(Form::InlineNarrowCell);
    jit.load32(narrowSlot(Form::InlineNarrowCell), word);
    Jump isNotCell = jit.branch32(CCallHelpers::Below, word, TrustedImm32(minCell32));
    decodeCell32(jit, word, result);
    finish();

    isNotCell.link(&jit);
    slotIsSilent.append(jit.branch32(CCallHelpers::Below, word, TrustedImm32(FieldTableEntry::minNarrowValue)));
    jit.move(word, result);
    finish();

    isNotNarrowCell.link(&jit);
    if (testsOutOfLineValuesSecond) {
        jit.sub32(TrustedImm32(1 << FieldTableEntry::formShift), operand);
        formsTakenOff = static_cast<unsigned>(Form::OutOfLineValue);
    } else
        readOutOfLineValue();
    RELEASE_ASSERT(formsTakenOff == static_cast<unsigned>(Form::OutOfLineValue));

    // The form Field is the last one. What is left of the entry is one form and its operand.
    static_assert(static_cast<unsigned>(Form::Field) == static_cast<unsigned>(Form::OutOfLineValue) + 1);
    auto first = [](uint32_t firstOperand) { return static_cast<int32_t>((1u << FieldTableEntry::formShift) + firstOperand); };
    Jump isNotInCell = jit.branch32(CCallHelpers::Below, operand, TrustedImm32(first(FieldTableEntry::firstEncodedByte)));
    Jump isNotEncodedByte = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstBoolean)));
    jit.load8(BaseIndex(probe.receiver, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstEncodedByte)), word);
    slotIsEmpty.append(jit.branchTest32(CCallHelpers::Zero, word));
    jit.move(word, result);
    finish();

    isNotEncodedByte.link(&jit);
    Jump isDouble;
    const bool testsDoublesLast = !testsDoublesSecond && !testsDoublesThird;
    if (testsDoublesLast)
        isDouble = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstDouble)));
    jit.load8(BaseIndex(probe.receiver, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstBoolean)), word);
    jit.or32(TrustedImm32(JSValue::ValueFalse), word);
    jit.move(word, result);
    finish();

    if (testsDoublesLast) {
        isDouble.link(&jit);
        readDouble(first(FieldTableEntry::firstDouble));
    }

    if (!probe.answersMissingFields) {
        otherwise.append(slotIsEmpty);
        otherwise.append(slotIsSilent);
        otherwise.append(isNotInCell);
    } else {
        auto goOnIn = [&](JumpList& jumps, StubKey stub) {
            const StubSignature signature = signatureOf(stub);
            const GPRReg receiverOfStub = signature.operands[0].gpr();
            const GPRReg nameOfStub = *signature.immediate;
            RELEASE_ASSERT(probe.receiver != nameOfStub && (probe.boundName || probe.name == nameOfStub));
            if (probe.receiver == receiverOfStub && !probe.boundName) {
                jit.linkToStub(jumps, stub);
                return;
            }
            jumps.link(&jit);
            jit.move(probe.receiver, receiverOfStub);
            if (probe.boundName)
                jit.move(TrustedImm32(static_cast<int32_t>(*probe.boundName)), nameOfStub);
            jit.jumpToStub(stub);
        };
        goOnIn(slotIsEmpty, getByIdOfEmptySlot().withCallerFrame(*probe.answersMissingFields));
        goOnIn(slotIsSilent, getByIdOfSilentSlot().withCallerFrame(*probe.answersMissingFields));
        isNotInCell.link(&jit);
        Jump isNotInExtension = jit.branch32(CCallHelpers::Below, operand, TrustedImm32(first(FieldTableEntry::firstNarrowCellInExtension)));
        JumpList isNarrowCellInExtension;
        isNarrowCellInExtension.append(jit.jump());
        goOnIn(isNarrowCellInExtension, getByIdOfNarrowCellInExtension().withCallerFrame(*probe.answersMissingFields));

        // A sum that has outgrown int32 goes the long way with the markers.
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

void answerMissingField(StubAssembler& jit, CallerFrame callerFrame, GPRReg object, GPRReg name, GPRReg T10, GPRReg T11, GPRReg T12, JumpList& otherwise)
{
    constexpr unsigned fieldSizeShift = WTF::fastLog2(static_cast<unsigned>(sizeof(FieldData)));
    constexpr uint32_t flagsLeftToCaches = fieldIsOutOfLine | fieldMayExistWithoutStorage | fieldIsAccessor | fieldIsPrivate | fieldIsBrand;
    static_assert(sizeof(FieldData) == 1u << fieldSizeShift && sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo);
    RELEASE_ASSERT(noOverlap(object, name, T10, T11, T12));
    const FieldTableProbe probe { object, name, T10, T11, std::nullopt, true, true };
    JumpList isAbsent;

    // The entry says that the field holds a 32-bit reference and in which half of which granule of the Extension. One piece of code for every stub that finds such an entry: it finds it again.
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

        // No closure that is made at its first use lies in an Extension, so zero says one thing here.
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

    // Whether the slot is empty is for the record of the field to say: the word of an empty reference is also that of a closure that is not made yet.
    // Where some field of the object lies outside its slot, the row of the Structure says where.
    jit.bindEntry(getByIdOfSilentSlot().withCallerFrame(callerFrame));
    auto branchIfSomeFieldLiesOutsideItsSlot = [&] {
        return jit.branchTest32(CCallHelpers::NonZero, Address(object, offsetOfOrderWord), TrustedImm32(static_cast<int32_t>(orderWordHasFieldOutsideSlot)));
    };
    otherwise.append(branchIfSomeFieldLiesOutsideItsSlot());
    locateFieldTableEntry(jit, probe, otherwise);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), probe.table);
    jit.load16(BaseIndex(probe.table, probe.index, CCallHelpers::TimesTwo), probe.index);
    jit.and32(TrustedImm32(FieldRecordIndex::indexMask), probe.index);
    jit.lshift32(TrustedImm32(fieldSizeShift), probe.index);
    const GPRReg record = T12;
    loadStructure(jit, object, record);
    jit.loadPtr(Address(record, CompiledStructure::offsetOfLayout()), record);
    jit.addPtr(probe.index, record);
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, static_cast<int32_t>(sizeof(LayoutData) + OBJECT_OFFSETOF(FieldData, flags))), TrustedImm32(flagsLeftToCaches)));
    const FieldAddressRegisters field { object, T10, T11, T11, T12 };
    jit.load32(Address(record, static_cast<int32_t>(sizeof(LayoutData) + OBJECT_OFFSETOF(FieldData, slot))), field.slotData);
    computeFieldAddress(jit, field, false, otherwise);
    JumpList isPresent;
    JumpList isAbsentInObjectWithEveryFieldInItsSlot;
    branchOnFieldPresence(jit, field, T10, isPresent, isAbsentInObjectWithEveryFieldInItsSlot);

    // A closure that is made at its first use. The object and the name say all that making it takes, so nothing is looked up and nothing is filed.
    static_assert(!uncreatedDeferredClosure);
    isPresent.link(&jit);
    // `class C { n; }`: a number slot that says undefined or null, or a double that is no value.
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
#if CPU(X86_64)
    JumpList isNotBelowObjectPrototype;
    branchUnlessNameIsAbsent(jit, probe, otherwise, &isNotBelowObjectPrototype);
#else
    branchUnlessNameIsAbsent(jit, probe, otherwise);
#endif
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

#if CPU(X86_64)
    // An instance of a class or of a constructor function. The front table says the same of a pair that a read was filed for: a field whose empty slot reads as undefined, for as long as nothing above changes.
    // The handler of such a cell finds the slot and looks into it to learn what is known here. This is the probe of the front table, so whatever else the cell says goes on from here as it does from there.
    constexpr int32_t bitsAsked = PackedSlot::kindMask | PackedSlot::kindFlag;
    constexpr int32_t bitsOfFieldThatIsUndefinedWhereEmpty = ((static_cast<uint8_t>(Slot::Kind::Field) | PackedSlot::kindFlag) ^ PackedSlot::ownInlineValueBits) & bitsAsked;
    static_assert(Slot::kindFlagOf(Slot::Kind::Field) == Slot::absentIsUndefined);
    const SlotOrigin origin = callerFrame == CallerFrame::StubFrame ? SlotOrigin::ByIdInStubFrame : SlotOrigin::ById;
    const GPRReg cell = T12;
    RELEASE_ASSERT(cell == gprOf(slotHeaderRegister) && object == gprOf(firstStubOperandRegister) && name == gprOf(slotNameRegister));
    isNotBelowObjectPrototype.link(&jit);
    jit.load32(Address(object, JSCell::structureIDOffset()), T11);
    locateFrontCell(jit, SlotAccess::Read, T11, name, T10);
    jit.m_assembler.cmpw_rm(name, static_cast<int32_t>(PackedSlot::offsetOfName()), T10, T11, CCallHelpers::TimesEight);
    jit.linkToStub(Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE)), getByIdHashed().withCallerFrame(callerFrame));
    jit.load64(BaseIndex(T10, T11, CCallHelpers::TimesEight), cell);
    jit.urshift64(cell, TrustedImm32(kindShiftInCell), T10);
    jit.and32(TrustedImm32(bitsAsked), T10);
    Jump saysSomethingElse = jit.branch32(CCallHelpers::NotEqual, T10, TrustedImm32(bitsOfFieldThatIsUndefinedWhereEmpty));
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    saysSomethingElse.link(&jit);
    jumpToSlotHandler(jit, slotHandlerTable(SlotOperation::Read, origin, SlotForm::PackedSlot), cell, T10);
#endif
}

void returnFieldByTable(StubAssembler& jit, const FieldTableProbe& probe, JumpList& otherwise)
{
    readFieldByTable(jit, probe, jit.result(), otherwise, [&] {
        jit.returnToCaller();
    });
}

void storeFieldByTable(StubAssembler& jit, const FieldTableProbe& probe, GPRReg value, GPRReg scratch, SlotOperation operation, JumpList& otherwise, JumpList* isSampled)
{
    using Form = FieldTableEntry::Form;
    static_assert(sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo && FieldRecordIndex::isStoredWithoutTypeTest == 0x80 << 8);
    RELEASE_ASSERT(operation != SlotOperation::Read && noOverlap(value, scratch, probe.receiver, probe.index, probe.table) && signatureOfWriteBarrier(writeBarrier()).operands[0] == Reg(probe.receiver));
    RELEASE_ASSERT(probe.boundName || scratch != probe.name);
    const GPRReg operand = probe.index;
    const GPRReg extension = probe.table;
    JumpList isEmptySlot;
    auto storeValue = [&](BaseIndex slot, JumpList& isEmpty) {
        isEmpty.append(jit.branchTest64(CCallHelpers::Zero, slot));
        jit.store64(value, slot);
        jit.linkToStub(jit.branchIfCell(value), writeBarrier());
        jit.returnToCaller();
    };

    locateFieldTableEntry(jit, probe, otherwise);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), scratch);
    const int32_t offsetOfBoundName = static_cast<int32_t>(probe.boundName.value_or(0) * sizeof(FieldRecordIndex));
    Jump isStoredByItsRecord = jit.branchTest8(CCallHelpers::Zero, BaseIndex(scratch, probe.index, CCallHelpers::TimesTwo, 1 + offsetOfBoundName), TrustedImm32(0x80));
    auto sample = [&] {
        if (!isSampled)
            return;
        static_assert(sizeof(PropertyCacheWords::storesAtEmptySites) == sizeof(uint8_t));
        constexpr int32_t offsetOfCounter = AOT_INSTANCE_OFFSET(propertyCaches.storesAtEmptySites);
#if CPU(ARM64)
        const GPRReg counter = jit.scratchRegister();
        jit.load8(Address(instanceGPR, offsetOfCounter), counter);
        jit.add32(TrustedImm32(1), counter);
        jit.store8(counter, Address(instanceGPR, offsetOfCounter));
        isSampled->append(jit.branchTest32(CCallHelpers::NonZero, counter, TrustedImm32(UINT8_MAX + 1)));
#else
        jit.m_assembler.addb_im(1, offsetOfCounter, instanceGPR);
        isSampled->append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
    };
    otherwise.append(branchUnlessEntryHasName(jit, probe));
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, Address(probe.receiver, offsetOfOrderWord), TrustedImm32(integrityBitsBlockingStore(operation))));
    Jump isNotInlineValue = branchUnlessForm(jit, operand, Form::InlineValue);
    storeValue(BaseIndex(probe.receiver, operand, CCallHelpers::TimesEight), isEmptySlot);

    isNotInlineValue.link(&jit);
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

    // There are many of these probes, so what is left stands once, in a stub of its own.
    const StubKey booleanOrDouble = putBooleanOrDoubleField(putModeOf(operation));
    const StubSignature signature = signatureOf(booleanOrDouble);
    RELEASE_ASSERT(signature.operands[0] == Reg(probe.receiver) && signature.operands[1] == Reg(value) && signature.operands[2] == Reg(operand) && (probe.boundName || probe.name == *signature.immediate));
    isNotOutOfLineValue.link(&jit);
    // That stub creates a field the long way, for which a filled site is the shorter one.
    sample();
    if (probe.boundName)
        jit.move(TrustedImm32(static_cast<int32_t>(*probe.boundName)), jit.reuseScratch(slotNameRegister));
    jit.jumpToStub(booleanOrDouble);

    // Only a store at a site is sampled.
    const StubKey refusedValue = putValueThatSlotRefuses(putModeOf(operation), isSampled);
    isRefusedByInt32Slot.link(&jit);
    if (probe.boundName)
        jit.move(TrustedImm32(static_cast<int32_t>(*probe.boundName)), jit.reuseScratch(slotNameRegister));
    jit.jumpToStub(refusedValue);

    // Any other field: the stub that is told the record of the field tests the value and stores it, or creates the field.
    const GPRReg offsetOfRecord = jit.reuseScratch(slotNameRegister);
    // The entry has the name. Its index is found once more: the form and the operand have taken its place.
    isEmptySlot.link(&jit);
    FieldTableProbe again = probe;
    again.nameHasColumn = true;
    again.receiverIsCell = true;
    locateFieldTableEntry(jit, again, otherwise);
    jit.load16(BaseIndex(scratch, probe.index, CCallHelpers::TimesTwo, offsetOfBoundName), scratch);
    jit.and32(TrustedImm32(FieldRecordIndex::indexMask), scratch);
    Jump hasIndex = jit.jump();

    isStoredByItsRecord.link(&jit);
    jit.load16(BaseIndex(scratch, probe.index, CCallHelpers::TimesTwo, offsetOfBoundName), scratch);
    otherwise.append(branchUnlessEntryHasName(jit, probe));
    hasIndex.link(&jit);
    sample();
    static_assert(sizeof(FieldData) == 1 << 4);
    jit.lshift32(TrustedImm32(4), scratch);
    jit.add32(TrustedImm32(sizeof(LayoutData)), scratch, offsetOfRecord);
    jit.jumpToStub(putKnownField(putModeOf(operation)));
}

// Where a store through the table of fields goes on behind the form OutOfLineValue.
StubSignature signatureOfPutBooleanOrDoubleField(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(storedValueByIdRegister));
    signature.addOperand(gprOf(StubRegister::T11));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfPutValueThatSlotRefuses(StubKey key)
{
    StubSignature signature;
    signature.addOperand(gprOf(firstStubOperandRegister));
    signature.addOperand(gprOf(storedValueByIdRegister));
    if (key.form() >= numberOfPutModes)
        signature.addOperand(gprOf(storeSiteCacheRegister));
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

// The field leaves its slot. What that made of the last object in the same state it makes of this one: see Object::tryRepeat().
void generatePutValueThatSlotRefuses(StubAssembler& jit)
{
    static_assert(sizeof(StructureID) == sizeof(uint32_t) && MarkedSpace::sizeStep == 2 * sizeof(Allocator));
    const PutMode mode = static_cast<PutMode>(jit.key().form() % numberOfPutModes);
    const bool isAtSite = jit.key().form() >= numberOfPutModes;
    const GPRReg object = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg name = jit.immediate();
    const GPRReg offsetOfSlot = jit.allocateScratch(StubRegister::T11);
    const GPRReg exit = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg extra = jit.allocateScratch(StubRegister::T14);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(object, value, offsetOfSlot, name, exit, scratch, extra, allocator, instanceGPR, gprOf(storeSiteCacheRegister)));
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
    // None until the run time has seen a store by name.
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
    // A slot may keep undefined and null: RuntimeLayoutTable::slotAdmits() knows.
    cannot.append(jit.branchIfOther(value, scratch));
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, offsetOfSlot)), offsetOfSlot);
    auto branchIfSlotIsWide = [&] {
        return jit.branchTest8(CCallHelpers::NonZero, memberOfExit(OBJECT_OFFSETOF(FieldExit, widthOfSlot)), TrustedImm32(sizeof(uint64_t)));
    };

    // The object has the field, or lacks it, as the other object did.
    Jump slotIsWide = branchIfSlotIsWide();
    jit.load32(slot, scratch);
    Jump isLoaded = jit.jump();
    slotIsWide.link(&jit);
    jit.load64(slot, scratch);
    isLoaded.link(&jit);
    Jump isInSlot = jit.branch64(CCallHelpers::NotEqual, scratch, memberOfExit(OBJECT_OFFSETOF(FieldExit, absentBits)));
    cannot.append(jit.branchTest8(CCallHelpers::NonZero, memberOfExit(OBJECT_OFFSETOF(FieldExit, wasInSlot))));
    // A store that creates the field asks what is above the object. Nothing there has changed since the run time asked for a store to an object of this Structure.
    if (mode != PutMode::Define) {
        jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.absenceFilterGenerationForStores)), scratch);
        cannot.append(jit.branch32(CCallHelpers::NotEqual, memberOfExit(OBJECT_OFFSETOF(FieldExit, generationForStores)), scratch));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.namesChangedOnChains)), scratch);
        jit.urshift32(name, TrustedImm32(6), extra);
        jit.load64(BaseIndex(scratch, extra, CCallHelpers::TimesEight), scratch);
        jit.urshift64(name, scratch);
        cannot.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm32(1)));
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
    const Address flagsOfCell(object, JSCell::typeInfoFlagsOffset());
#if CPU(X86_64)
    jit.or8(TrustedImm32(TypeInfoPerCellBit), flagsOfCell);
#else
    jit.load8(flagsOfCell, scratch);
    jit.or32(TrustedImm32(TypeInfoPerCellBit), scratch);
    jit.store8(scratch, flagsOfCell);
#endif
    jit.load64(memberOfExit(OBJECT_OFFSETOF(FieldExit, absentBits)), scratch);
    Jump isWide = branchIfSlotIsWide();
    jit.store32(scratch, slot);
    Jump isCleared = jit.jump();
    isWide.link(&jit);
    jit.store64(scratch, slot);
    isCleared.link(&jit);
    jit.load32(memberOfExit(OBJECT_OFFSETOF(FieldExit, structureAfter)), scratch);
    jit.store32(scratch, Address(object, JSCell::structureIDOffset()));
    bumpRuntimeCounter(jit, RuntimeCounterID::FieldsLeftSlotByStoreOfOtherType, scratch);
    jit.returnToCaller();

    if (isAtSite) {
        jit.linkToStub(cannot, putByIdAtSiteBehindFieldTable(mode));
        return;
    }
    cannot.link(&jit);
    jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
    jit.jumpToStub(putByIdBehindFieldTable(mode));
}

// A boolean goes into the slot of a boolean and a number into the slot of a double.
void generatePutBooleanOrDoubleField(StubAssembler& jit)
{
    static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1) && JSValue::ValueTrue <= UINT8_MAX);
    static_assert(traitsOf(SlotRepresentation::OptionalBoolean).width == 1 && !traitsOf(SlotRepresentation::OptionalBoolean).absentBits);
    static_assert(traitsOf(SlotRepresentation::OptionalDouble).absentBits == absentDouble && !traitsOf(SlotRepresentation::Double).hasAbsent);
    static_assert(sizeof(FieldData) == 1 << 4 && sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo);
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
    // The index of the entry is found once more: the form and the operand have taken its place.
    auto goOnWithRecord = [&](StubKey stub) {
        JumpList isNotCompiledObject;
        locateFieldTableEntry(jit, { object, name, operand, scratch, std::nullopt, true, true }, isNotCompiledObject);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), scratch);
        jit.load16(BaseIndex(scratch, operand, CCallHelpers::TimesTwo), scratch);
        jit.and32(TrustedImm32(FieldRecordIndex::indexMask), scratch);
        jit.lshift32(TrustedImm32(4), scratch);
        jit.add32(TrustedImm32(sizeof(LayoutData)), scratch, name);
        jit.jumpToStub(stub);
        isNotCompiledObject.link(&jit);
        jit.linkToStub(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)), putByIdHashed(mode));
        jit.jumpToStub(putByIdBehindFieldTable(mode));
    };

    isStoredByItsRecord.append(jit.branch32(CCallHelpers::Below, operand, TrustedImm32(first(FieldTableEntry::firstEncodedByte))));
    Jump isNotEncodedByte = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstBoolean)));
    const BaseIndex encodedByteSlot(object, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstEncodedByte));
    leaveUnlessBoolean();
    isCreated.append(jit.branchTest8(CCallHelpers::Zero, encodedByteSlot));
    jit.store8(value, encodedByteSlot);
    jit.returnToCaller();

    isNotEncodedByte.link(&jit);
    Jump isNotBoolean = jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::firstDouble)));
    leaveUnlessBoolean();
    jit.and32(TrustedImm32(1), value, scratch);
    jit.store8(scratch, BaseIndex(object, operand, CCallHelpers::TimesOne, -first(FieldTableEntry::firstBoolean)));
    jit.returnToCaller();

    isNotBoolean.link(&jit);
    isStoredByItsRecord.append(jit.branch32(CCallHelpers::AboveOrEqual, operand, TrustedImm32(first(FieldTableEntry::endOfDoubles))));
    const BaseIndex doubleSlot(object, operand, CCallHelpers::TimesEight, -first(FieldTableEntry::firstDouble) * static_cast<int32_t>(sizeof(double)));
    isStoredByItsRecord.append(jit.branchIfNotNumber(value));
    // No number has the bits of the marker, so a slot that is never empty does not hold them.
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
    // The part below runs after the generator has returned.
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

void loadFrontRow(CCallHelpers& jit, SlotAccess access, GPRReg structureID, GPRReg row, GPRReg scratch)
{
    const int32_t front = offsetOfFrontWords(access);
    RELEASE_ASSERT(noOverlap(structureID, row, scratch, instanceGPR));
    jit.urshift32(structureID, TrustedImm32(StructureRowWords::granuleShift), row);
    jit.loadPtr(Address(instanceGPR, front + static_cast<int32_t>(OBJECT_OFFSETOF(FrontWords, rowsForStructureIDZero))), scratch);
    jit.load32(BaseIndex(scratch, row, CCallHelpers::TimesFour), row);
    jit.loadPtr(Address(instanceGPR, front + static_cast<int32_t>(OBJECT_OFFSETOF(FrontWords, plane))), scratch);
    jit.getEffectiveAddress(BaseIndex(scratch, row, CCallHelpers::TimesEight), row);
}

#if CPU(X86_64)
void loadFrontRowOfCell(CCallHelpers& jit, SlotAccess access, GPRReg cell, GPRReg row, GPRReg scratch)
{
    const int32_t front = offsetOfFrontWords(access);
    RELEASE_ASSERT(noOverlap(cell, row, scratch, instanceGPR));
    jit.load32(Address(cell, JSCell::structureIDOffset()), row);
    jit.urshift32(TrustedImm32(StructureRowWords::granuleShift), row);
    jit.loadPtr(Address(instanceGPR, front + static_cast<int32_t>(OBJECT_OFFSETOF(FrontWords, rowsForStructureIDZero))), scratch);
    jit.load32(BaseIndex(scratch, row, CCallHelpers::TimesFour), row);
    jit.loadPtr(Address(instanceGPR, front + static_cast<int32_t>(OBJECT_OFFSETOF(FrontWords, plane))), scratch);
    jit.getEffectiveAddress(BaseIndex(scratch, row, CCallHelpers::TimesEight), row);
}

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

void returnByFrontCell(StubAssembler& jit, GPRReg receiver, GPRReg column, GPRReg plane, GPRReg index, GPRReg cell, JumpList& hasOtherName, const ScopedLambda<void()>* countMissAtSite)
{
    constexpr int32_t biasedBitsOfAbsent = static_cast<uint8_t>(Slot::Kind::Absent) ^ PackedSlot::ownInlineValueBits;
    static_assert(!Slot::kindFlagOf(Slot::Kind::Absent) && PackedSlot::offsetOfBiasedKindAndFlags() == PackedSlot::offsetOfLocation() + 1 && !PackedSlot::upperHalfForOwnInlineValue(0));
    RELEASE_ASSERT(noOverlap(receiver, column, plane, index, cell));
    auto inCell = [&](ptrdiff_t offset) { return BaseIndex(plane, index, CCallHelpers::TimesEight, static_cast<int32_t>(offset)); };

    jit.m_assembler.cmpw_rm(column, static_cast<int32_t>(PackedSlot::offsetOfName()), plane, index, CCallHelpers::TimesEight);
    hasOtherName.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionNE)));
    Jump isNotOwnInlineValue = jit.branch16(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfLocation()), TrustedImm32(0));
    if (countMissAtSite) {
        jit.urshift64(TrustedImm32(Slot::locationShiftInHeader), cell);
        Jump liesElsewhere = jit.branch32(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfPayload()), cell);
        jit.load64(BaseIndex(receiver, cell, CCallHelpers::TimesEight), jit.result());
        jit.returnToCaller();

        liesElsewhere.link(&jit);
        (*countMissAtSite)();
    }
    jit.load32(inCell(PackedSlot::offsetOfPayload()), cell);
    jit.load64(BaseIndex(receiver, cell, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    isNotOwnInlineValue.link(&jit);
    if (countMissAtSite)
        (*countMissAtSite)();
    Jump isNotAbsent = jit.branch8(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfBiasedKindAndFlags()), TrustedImm32(biasedBitsOfAbsent));
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    isNotAbsent.link(&jit);
    // `error.message`: an own value in the butterfly needs no handler.
    constexpr int32_t upperBitsOfOwnOutOfLineValue = PackedSlot::isOutOfLine << 8;
    Jump needsHandler = jit.branch16(CCallHelpers::NotEqual, inCell(PackedSlot::offsetOfLocation()), TrustedImm32(upperBitsOfOwnOutOfLineValue));
    jit.load32SignedExtendTo64(inCell(PackedSlot::offsetOfPayload()), cell);
    jit.loadPtr(Address(receiver, JSObject::butterflyOffset()), plane);
    jit.load64(BaseIndex(plane, cell, CCallHelpers::TimesEight), jit.result());
    jit.returnToCaller();

    needsHandler.link(&jit);
    jit.load64(inCell(0), cell);
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

void loadIdentifier(StubAssembler& jit, GPRReg identifierNumber, GPRReg uid, JumpList& isNotCreated)
{
    static_assert(sizeof(UniquedStringImpl) == 3u << CCallHelpers::TimesEight && sizeof(ProgramSymbol) == 2u << CCallHelpers::TimesEight);
    const GPRReg scratch = jit.scratchRegister();
    RELEASE_ASSERT(noOverlap(identifierNumber, uid, scratch, instanceGPR));
    JumpList isAtomOfImage;
    if (jit.options().isPositionIndependent) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), scratch);
        jit.load32(BaseIndex(scratch, identifierNumber, CCallHelpers::TimesFour), uid);
        isAtomOfImage.append(jit.branchTest32(CCallHelpers::NonZero, uid));

        // The first use of the name.
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
        isAtomOfImage.append(jit.branchTest32(CCallHelpers::NonZero, Address(uid, StringImpl::lengthMemoryOffset())));
    }

    // A symbol, a private name or the empty text: ProgramSymbols::find().
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

void jumpToSlotHandlerOfKind(StubAssembler& jit, SlotHandlerTable table, GPRReg header, GPRReg scratch)
{
    const SlotHandlerTableTraits traits = traitsOf(table);
    const SlotHandlerOperands operands = operandsOfSlotHandler(table);
    const std::array handlers = slotHandlerKeys(table);
    const int32_t offsetOfHandlers = static_cast<int32_t>(Instance::offsetOfSlotHandlers(table));
    RELEASE_ASSERT(header == operands.header);
    RELEASE_ASSERT(noOverlap(scratch, operands.receiver, operands.header, operands.payload, operands.value, operands.keyCell, operands.identifierNumber, siteRegisterOf(traits).value_or(InvalidGPRReg), instanceGPR));
#if CPU(ARM64)
    static_assert(sizeof(Instance) / sizeof(void*) <= 1 << 12, "The index of a handler is the immediate of one add.");
    jit.add32(TrustedImm32(offsetOfHandlers / static_cast<int32_t>(sizeof(void*))), scratch);
    jit.jumpThroughHandlerTable(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesEight), handlers);
#else
    jit.jumpThroughHandlerTable(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesEight, offsetOfHandlers), handlers);
#endif
}

void callSlotHandler(StubAssembler& jit, SlotHandlerTable table, GPRReg kind)
{
    const std::array handlers = slotHandlerKeys(table);
    const int32_t offsetOfHandlers = static_cast<int32_t>(Instance::offsetOfSlotHandlers(table));
#if CPU(ARM64) && defined(BUN_HARDENED)
    jit.add32(TrustedImm32(offsetOfHandlers / static_cast<int32_t>(sizeof(void*))), kind);
    jit.callThroughHandlerTable(BaseIndex(instanceGPR, kind, CCallHelpers::TimesEight), handlers);
#else
    jit.callThroughHandlerTable(BaseIndex(instanceGPR, kind, CCallHelpers::TimesEight, offsetOfHandlers), handlers);
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

void loadValueBySlot(StubAssembler& jit, const CachedSlot& slot, GPRReg receiver, GPRReg storage, GPRReg wordIndex, GPRReg result)
{
    RELEASE_ASSERT(Slot::payloadOf(slot.kind()) == SlotPayload::Holder);
    RELEASE_ASSERT(noOverlap(receiver, storage, wordIndex) && noOverlap(slot.header(), storage));
    if (slot.form() == SlotForm::Slot) {
        // An own property has no holder: Slot::createInStorage().
        slot.loadHolder(jit, storage);
        jit.moveConditionallyTest64(CCallHelpers::Zero, storage, storage, receiver, storage);
#if CPU(X86_64)
        jit.m_assembler.btw_ir(flagBitInHeader(Slot::isOutOfLine).position, slot.header());
        jit.m_assembler.cmovq_mr(X86Assembler::ConditionC, JSObject::butterflyOffset(), storage, storage);
#else
        Jump isInline = slot.branchUnlessFlag(jit, Slot::isOutOfLine);
        jit.loadPtr(Address(storage, JSObject::butterflyOffset()), storage);
        isInline.link(&jit);
#endif
        slot.loadOwnWordIndex(jit, wordIndex);
        jit.load64(BaseIndex(storage, wordIndex, CCallHelpers::TimesEight), result);
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

void returnPresenceBySlot(StubAssembler& jit, PresenceQuestion question, const ProbedSlot& slot, GPRReg receiver, Label asksReceiver, JumpList& isUnknown, KindOfProbedSlot whereKindIs)
{
    constexpr auto bitOf = [](Slot::Kind kind) { return 1u << static_cast<unsigned>(kind); };
    const bool asksIn = question == PresenceQuestion::In;
    // A slot that leaves the answer to an object above says that the receiver itself has no such property.
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
        const GPRReg mask = jit.scratchRegister();
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

        // The own properties of a global proxy are those of its target, which a slot names as their holder.
        isInHolder.link(&jit);
        isUnknown.append(jit.branch8(CCallHelpers::Equal, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(GlobalProxyType)));
        jit.move(TrustedImm32(0), result);
        break;
    }
    }
    jit.returnToCaller();

    isAbsent.link(&jit);
    jit.move(TrustedImm32(0), result);
    jit.returnToCaller();
}

void loadStaticsBase(StubAssembler& jit, GPRReg staticsBase, GPRReg roundedSizeOfStatics, GPRReg instance)
{
    constexpr int32_t offsetOfSize = OBJECT_OFFSETOF(ProgramTables, instanceSizes) + OBJECT_OFFSETOF(InstanceSizes, sizeOfStatics);
    RELEASE_ASSERT(noOverlap(staticsBase, roundedSizeOfStatics, instanceGPR) && instance != roundedSizeOfStatics);
    jit.loadPtr(Address(instance, AOT_INSTANCE_OFFSET(image.tables)), roundedSizeOfStatics);
    jit.load32(Address(roundedSizeOfStatics, offsetOfSize), roundedSizeOfStatics);
    jit.add32(TrustedImm32(staticsGranuleMask), roundedSizeOfStatics);
    jit.and32(TrustedImm32(~staticsGranuleMask), roundedSizeOfStatics);
    jit.sub64(instance, roundedSizeOfStatics, staticsBase);
    jit.sub64(TrustedImm32(static_cast<int32_t>(InstanceLayout::bytesReservedForPrefixAndPollPage)), staticsBase);
}

void loadStaticsBaseOfVariable(StubAssembler& jit, const CachedSlot& slot, GPRReg staticsBase, GPRReg scratch, FieldReadRegisters& read)
{
    if (!jit.options().sharesRealms) {
        loadStaticsBase(jit, staticsBase, scratch);
        return;
    }
    // PropertyAnswer::imageOfVariable.
    slot.loadHiddenWordOffset(jit, scratch);
    jit.move(instanceGPR, staticsBase);
    Jump isOfThisInstance = jit.branchTest32(CCallHelpers::Zero, scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), staticsBase);
    jit.loadPtr(Address(staticsBase, JSGlobalObject::offsetOfAOTInstance()), staticsBase);
    Label next = jit.label();
    jit.sub32(TrustedImm32(1 << PackedSlot::hiddenWordOffsetShift), scratch);
    Jump isFound = jit.branchTest32(CCallHelpers::Zero, scratch);
    jit.loadPtr(Address(staticsBase, AOT_INSTANCE_OFFSET(above)), staticsBase);
    jit.jump().linkTo(next, &jit);
    isFound.link(&jit);
    isOfThisInstance.link(&jit);
    read.instanceOfLiteralStrings = read.numberScratch;
    jit.move64ToDouble(staticsBase, read.instanceOfLiteralStrings);
    loadStaticsBase(jit, staticsBase, scratch, staticsBase);
}

void markStaticsCard(StubAssembler& jit, GPRReg staticsBase, GPRReg roundedSizeOfStatics, GPRReg offsetInStatics)
{
    RELEASE_ASSERT(noOverlap(staticsBase, roundedSizeOfStatics, offsetInStatics));
    jit.urshift32(TrustedImm32(ProgramStatics::cardShift), roundedSizeOfStatics);
    jit.urshift32(TrustedImm32(ProgramStatics::cardShift), offsetInStatics);
    jit.sub64(roundedSizeOfStatics, offsetInStatics);
    jit.store8(TrustedImm32(ProgramStatics::cardIsDirty), BaseIndex(staticsBase, offsetInStatics, CCallHelpers::TimesOne));
}

Jump branchIfSiteIsFilled(StubAssembler& jit, GPRReg site)
{
    return jit.branchTest64(CCallHelpers::NonZero, Address(site));
}

void countSiteCacheMiss(StubAssembler& jit, GPRReg site, GPRReg scratch, JumpList& isDueForRefill)
{
    const Address states(instanceGPR, AOT_INSTANCE_OFFSET(propertyCaches.siteCacheStates));
    RELEASE_ASSERT(noOverlap(site, scratch, instanceGPR));
#if CPU(ARM64)
    const GPRReg missesLeft = jit.scratchRegister();
    jit.urshift64(site, TrustedImm32(SiteCacheState::slotAddressShift), scratch);
    jit.and64(TrustedImm32(SiteCacheState::offsetMask), scratch);
    jit.loadPtr(states, missesLeft);
    jit.add64(missesLeft, scratch);
    jit.load16(Address(scratch), missesLeft);
    jit.sub32(TrustedImm32(1), missesLeft);
    jit.store16(missesLeft, Address(scratch));
    isDueForRefill.append(jit.branchTest32(CCallHelpers::NonZero, missesLeft, TrustedImm32(UINT16_MAX + 1)));
#else
    static_assert(SiteCacheState::offsetMask == (UINT16_MAX & ~(alignof(Slot) - 1)) >> SiteCacheState::slotAddressShift);
    jit.zeroExtend16To32(site, scratch);
    jit.urshift32(TrustedImm32(SiteCacheState::slotAddressShift), scratch);
    jit.add64(states, scratch);
    jit.m_assembler.subw_im(1, 0, scratch);
    isDueForRefill.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
}

void countMissOfEmptySite(StubAssembler& jit, GPRReg site, GPRReg scratch, JumpList& isDueForFill)
{
    constexpr int32_t step = SiteCacheState::sampleStep;
    constexpr int32_t offsetOfCounter = AOT_INSTANCE_OFFSET(propertyCaches.missesOfEmptySites);
    RELEASE_ASSERT(noOverlap(site, scratch, instanceGPR));
#if CPU(ARM64)
    jit.load8(Address(instanceGPR, offsetOfCounter), scratch);
    jit.add32(TrustedImm32(step), scratch);
    jit.store8(scratch, Address(instanceGPR, offsetOfCounter));
    isDueForFill.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(UINT8_MAX + 1)));
#else
    jit.m_assembler.addb_im(step, offsetOfCounter, instanceGPR);
    isDueForFill.append(Jump(jit.m_assembler.jCC(X86Assembler::ConditionC)));
#endif
}

void refillSiteCache(StubAssembler& jit, GPRReg receiver, GPRReg identifierNumber, GPRReg site, SlotAccess access)
{
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        const GPRReg A3 = jit.reuseScratch(StubRegister::A3);
        const GPRReg A4 = jit.reuseScratch(StubRegister::A4);
        RELEASE_ASSERT(noOverlap(A3, identifierNumber, receiver) && A2 != receiver);
        jit.move(site, A3);
        jit.move(identifierNumber, A2);
        jit.move(receiver, A1);
        jit.move(instanceGPR, A0);
        jit.move(TrustedImm32(static_cast<int32_t>(access)), A4);
        jit.callOperation(Entry::operationAOTFillSiteCache, jit.reuseScratch(StubRegister::Operation));
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
