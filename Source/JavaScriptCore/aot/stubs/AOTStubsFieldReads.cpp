/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsField.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClassToken.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTVector.h"
#include "Butterfly.h"
#include "JSCellButterfly.h"
#include "JSObject.h"
#include "Structure.h"
#include <algorithm>
#include <array>
#include <bit>

namespace JSC { namespace AOT {

static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
static_assert(emptyLiteralStringWord > undefinedCell32 && emptyLiteralStringWord > nullCell32 && emptyLiteralStringWord < minCell32);

static bool markersAreEncodedValues(const SlotTraits& traits)
{
    return (!traits.hasAbsent || traits.absentBits == boxedMarkers.absent)
        && (!traits.hasUndefined || traits.undefinedBits == boxedMarkers.undefined)
        && (!traits.hasNull || traits.nullBits == boxedMarkers.null);
}

Jump branchIfBitsEqual(StubAssembler& jit, unsigned width, GPRReg bits, uint64_t expected)
{
    if (width == sizeof(uint64_t)) {
        if (!expected)
            return jit.branchTest64(CCallHelpers::Zero, bits);
        return jit.branch64(CCallHelpers::Equal, bits, TrustedImm64(static_cast<int64_t>(expected)));
    }
    if (!expected)
        return jit.branchTest32(CCallHelpers::Zero, bits);
    return jit.branch32(CCallHelpers::Equal, bits, TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(expected))));
}

void loadExtensionWordOffset(StubAssembler& jit, GPRReg object, GPRReg result)
{
    RELEASE_ASSERT(object != result);
    loadStructure(jit, object, result);
    jit.load32(Address(result, CompiledStructure::offsetOfExtensionSlot()), result);
    jit.and32(TrustedImm32(SlotData::maxOffset), result);
}

void computeFieldAddress(StubAssembler& jit, const FieldAddressRegisters& r, bool isOutOfLine, JumpList& hasNoStorage)
{
    RELEASE_ASSERT(noOverlap(r.object, r.slotData, r.address, r.representation));
    const TrustedImm32 offsetMask(SlotData::maxOffset);

    if (!isOutOfLine) {
        jit.and32(offsetMask, r.slotData, r.address);
        jit.addPtr(r.object, r.address);
    } else {
        RELEASE_ASSERT(noOverlap(r.object, r.slotData, r.extensionWordOffset, r.representation));
        const BaseIndex extensionWord(r.object, r.extensionWordOffset, CCallHelpers::TimesOne);
        const GPRReg count = r.representation;
        if (jit.options().useWideReferences) {
            jit.load64(extensionWord, r.address);
            hasNoStorage.append(jit.branchTest64(CCallHelpers::Zero, r.address));
        } else {
            jit.load32(extensionWord, r.address);
            decodeCell32(jit, r.address, r.address);
            jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), r.address);
        }
        // The bit of the granule goes to the top and the bits of later granules drop out.
        static_assert(offsetOfFirstExtensionSlot == extensionGranuleSize && extensionGranuleSize == 8 && maxExtensionGranules == 64);
        const GPRReg granuleBits = jit.scratchRegister();
        RELEASE_ASSERT(noOverlap(granuleBits, r.object, r.slotData, r.address, count));
        jit.and32(offsetMask, r.slotData, count);
        jit.urshift32(TrustedImm32(3), count);
        jit.neg32(count);
        jit.load64(Address(r.address, offsetOfExtensionGranuleBits), granuleBits);
        jit.lshift64(count, granuleBits);
        hasNoStorage.append(jit.branchTest64(CCallHelpers::PositiveOrZero, granuleBits));
        jit.countPopulation64(granuleBits, count, CCallHelpers::fpTempRegister);
        jit.getEffectiveAddress(BaseIndex(r.address, count, CCallHelpers::TimesEight), r.address);
        jit.and32(TrustedImm32(extensionGranuleSize - 1), r.slotData, count);
        jit.addPtr(count, r.address);
    }
    jit.urshift32(r.slotData, TrustedImm32(SlotData::offsetBits), r.representation);
}

void loadGetterSetterInField(StubAssembler& jit, GPRReg address, GPRReg representation, GPRReg result, JumpList& isAbsent)
{
    static_assert(traitsOf(SlotRepresentation::Cell32).isNarrowReference && traitsOf(SlotRepresentation::OptionalCell32).isNarrowReference);
    static_assert(widthOf(SlotRepresentation::JSValue) == sizeof(EncodedJSValue) && widthOf(SlotRepresentation::Cell) == sizeof(EncodedJSValue));
    JumpList isNarrow;
    isNarrow.append(jit.branch32(CCallHelpers::Equal, representation, TrustedImm32(static_cast<int32_t>(SlotRepresentation::OptionalCell32))));
    isNarrow.append(jit.branch32(CCallHelpers::Equal, representation, TrustedImm32(static_cast<int32_t>(SlotRepresentation::Cell32))));
    jit.load64(Address(address), result);
    isAbsent.append(jit.branchTest64(CCallHelpers::Zero, result));
    Jump isLoaded = jit.jump();

    isNarrow.link(&jit);
    jit.load32(Address(address), result);
    isAbsent.append(jit.branch32(CCallHelpers::Below, result, TrustedImm32(minCell32)));
    decodeCell32(jit, result, result);
    isLoaded.link(&jit);
}

void readFieldAndReturn(StubAssembler& jit, const FieldReadRegisters& r, FieldReadExits& exits, std::optional<SlotRepresentation> known)
{
    RELEASE_ASSERT(noOverlap(r.field.object, r.field.address, r.field.representation, r.value, r.number, r.numberScratch));
    RELEASE_ASSERT(noOverlap(r.result, r.field.address, r.field.representation, r.value));

    // The code for a boxed value reads a cell: both have the empty value for "absent". The code for a narrow reference that may be undefined or null reads one that may not: it never holds their markers.
    constexpr SlotTraits boxed = traitsOf(SlotRepresentation::JSValue);
    constexpr SlotTraits cell = traitsOf(SlotRepresentation::Cell);
    static_assert(boxed.width == cell.width && boxed.hasAbsent && cell.hasAbsent && boxed.absentBits == cell.absentBits && !cell.isNarrowReference);
    constexpr SlotTraits optionalCell32 = traitsOf(SlotRepresentation::OptionalCell32);
    constexpr SlotTraits cell32 = traitsOf(SlotRepresentation::Cell32);
    static_assert(optionalCell32.width == cell32.width && optionalCell32.isNarrowReference && cell32.isNarrowReference && optionalCell32.absentBits == cell32.absentBits && !cell32.absentBits);

    JumpList returnsUndefined;
    JumpList returnsNull;
    JumpList returnsNaN;

    RELEASE_ASSERT(!r.slot || (known && representationWithCodeForRead(*known) <= representationsReadAsInt32.back()));
    auto loadBitsInto = [&](GPRReg bits, unsigned width) {
        if (r.slot)
            loadSlotBits(jit, width, *r.slot, bits);
        else
            loadSlotBits(jit, width, Address(r.field.address), bits);
    };
    auto loadBits = [&](unsigned width) {
        loadBitsInto(r.value, width);
    };
    auto returnValue = [&] {
        jit.move(r.value, r.result);
        jit.returnToCaller();
    };
    auto returnInt32 = [&] {
        boxInt32(jit, r.value, r.result);
        jit.returnToCaller();
    };
    auto returnDouble = [&](PureDouble number) {
        boxDouble(jit, number, r.result);
        jit.returnToCaller();
    };
    auto returnNumber = [&](PureDouble number) {
        JumpList notInt32;
        branchIfNotExactInt32(jit, number.fpr(), r.value, notInt32, r.numberScratch);
        returnInt32();
        notInt32.link(&jit);
        returnDouble(number);
    };
    auto branchOnMarkers = [&](const SlotTraits& traits) {
        if (traits.hasAbsent)
            exits.isAbsent.append(branchIfBitsEqual(jit, traits.width, r.value, traits.absentBits));
        if (traits.hasUndefined)
            returnsUndefined.append(branchIfBitsEqual(jit, traits.width, r.value, traits.undefinedBits));
        if (traits.hasNull)
            returnsNull.append(branchIfBitsEqual(jit, traits.width, r.value, traits.nullBits));
    };
    auto readEncodedValue = [&](const SlotTraits& traits) {
        RELEASE_ASSERT(traits.width == sizeof(EncodedJSValue) && markersAreEncodedValues(traits));
        loadBits(traits.width);
        if (traits.hasAbsent)
            exits.isAbsent.append(jit.branchTest64(CCallHelpers::Zero, r.value));
        returnValue();
    };
    auto readReference = [&](const SlotTraits& traits) {
        RELEASE_ASSERT(traits.hasAbsent && markersAreEncodedValues(traits));
        const bool mayBeLiteralString = traits.valueKind == SlotValueKind::LiteralString;

        loadBits(traits.width);
        Jump notCell;
        if (traits.isNarrowReference) {
            notCell = jit.branch32(CCallHelpers::Below, r.value, TrustedImm32(minCell32));
            decodeCell32(jit, r.value, r.result);
        } else {
            notCell = jit.branch64(CCallHelpers::Below, r.value, TrustedImm32(minCell32));
            jit.move(r.value, r.result);
        }
        jit.returnToCaller();

        if (!mayBeLiteralString && !traits.hasUndefined && !traits.hasNull) {
            exits.isAbsent.append(notCell);
            return;
        }

        notCell.link(&jit);
        Jump isLiteralString;
        if (mayBeLiteralString)
            isLiteralString = jit.branch32(CCallHelpers::AboveOrEqual, r.value, TrustedImm32(emptyLiteralStringWord));
        exits.isAbsent.append(jit.branchTest32(CCallHelpers::Zero, r.value));
        returnValue();

        if (!mayBeLiteralString)
            return;
        isLiteralString.link(&jit);
        if (r.instanceOfLiteralStrings != InvalidFPRReg) {
            jit.moveDoubleTo64(r.instanceOfLiteralStrings, r.field.address);
            loadLiteralString(jit, r.value, r.field.address, exits.cannot, r.field.address);
        } else
            loadLiteralString(jit, r.value, r.field.address, exits.cannot);
        returnValue();
    };
    auto readNarrowInteger = [&](const SlotTraits& traits) {
        RELEASE_ASSERT(!traits.hasAbsent && !traits.hasUndefined && !traits.hasNull && traits.width <= sizeof(int32_t));
        const bool isSigned = traits.valueKind == SlotValueKind::SignedInteger;
        const Address slot(r.field.address);

        if (isSigned && traits.width == 1)
            jit.load8SignedExtendTo32(slot, r.value);
        else if (isSigned && traits.width == 2)
            jit.load16SignedExtendTo32(slot, r.value);
        else
            loadSlotBits(jit, traits.width, slot, r.value);
        if (isSigned || traits.width < sizeof(int32_t)) {
            returnInt32();
            return;
        }
        Jump notInt32 = jit.branch32(CCallHelpers::LessThan, r.value, TrustedImm32(0));
        returnInt32();
        notInt32.link(&jit);
        returnDouble(PureDouble::convertInt64(jit, r.value, r.number));
    };
    auto readInt64 = [&](const SlotTraits& traits) {
        RELEASE_ASSERT(traits.valueKind == SlotValueKind::SignedInteger);
        const GPRReg lowHalf = r.field.address;

        jit.load64(Address(r.field.address), r.value);
        jit.signExtend32ToPtr(r.value, lowHalf);
        Jump exceedsInt32 = jit.branch64(CCallHelpers::NotEqual, r.value, lowHalf);
        returnInt32();
        exceedsInt32.link(&jit);
        branchOnMarkers(traits);
        returnDouble(PureDouble::convertInt64(jit, r.value, r.number));
    };
    auto readOptionalInt32 = [&](const SlotTraits& traits) {
        RELEASE_ASSERT(traits.valueKind == SlotValueKind::SignedInteger && traits.width == sizeof(int32_t) && traits.hasAbsent);
        // A slot that is always filled has no markers, so nothing leaves behind the load.
        if (known == SlotRepresentation::Int32) {
            loadBitsInto(r.result, traits.width);
            boxExactInt32(jit, r.result, r.result);
            jit.returnToCaller();
            return;
        }
        loadBits(traits.width);
        Jump isMarker = jit.branch32(CCallHelpers::LessThan, r.value, TrustedImm32(minInlineInt32));
        Label isNumber = jit.label();
        returnInt32();
        isMarker.link(&jit);
        if (!known)
            jit.branch32(CCallHelpers::Equal, r.field.representation, TrustedImm32(static_cast<int32_t>(SlotRepresentation::Int32))).linkTo(isNumber, &jit);
        branchOnMarkers(traits);
        exits.cannot.append(jit.jump());
    };

    auto readAndReturn = [&](SlotRepresentation representation) {
        const SlotTraits traits = traitsOf(representation);
        switch (traits.valueKind) {
        case SlotValueKind::Boxed:
            readEncodedValue(traits);
            return;
        case SlotValueKind::Cell:
            if (traits.isNarrowReference)
                readReference(traits);
            else
                readEncodedValue(traits);
            return;
        case SlotValueKind::LiteralString:
            readReference(traits);
            return;
        case SlotValueKind::SignedInteger:
        case SlotValueKind::UnsignedInteger:
            if (traits.width == sizeof(int64_t))
                readInt64(traits);
            else if (traits.hasAbsent)
                readOptionalInt32(traits);
            else
                readNarrowInteger(traits);
            return;
        case SlotValueKind::Double: {
            jit.loadDouble(Address(r.field.address), r.number);
            Jump isNaN;
            returnNumber(PureDouble::branchIfNaN(jit, r.number, isNaN));
            if (!traits.hasAbsent && !traits.hasUndefined && !traits.hasNull) {
                returnsNaN.append(isNaN);
                return;
            }
            isNaN.link(&jit);
            jit.moveDoubleTo64(r.number, r.value);
            branchOnMarkers(traits);
            returnsNaN.append(jit.jump());
            return;
        }
        case SlotValueKind::Float:
            jit.loadFloat(Address(r.field.address), r.number);
            jit.convertFloatToDouble(r.number, r.number);
            returnNumber(PureDouble::branchIfNaN(jit, r.number, returnsNaN));
            return;
        case SlotValueKind::Boolean:
            jit.load8(Address(r.field.address), r.value);
            if (!traits.hasAbsent) {
                jit.or32(TrustedImm32(JSValue::ValueFalse), r.value, r.result);
                jit.returnToCaller();
                return;
            }
            RELEASE_ASSERT(markersAreEncodedValues(traits));
            exits.isAbsent.append(jit.branchTest32(CCallHelpers::Zero, r.value));
            returnValue();
            return;
        case SlotValueKind::DeferredClosure: {
            RELEASE_ASSERT(traits.hasAbsent && traits.absentBits == absentDeferredClosure);
            jit.load32(Address(r.field.address), r.value);
            Jump isNotCreated = jit.branch32(CCallHelpers::Below, r.value, TrustedImm32(minCell32));
            decodeCell32(jit, r.value, r.result);
            jit.returnToCaller();

            isNotCreated.link(&jit);
            exits.isAbsent.append(branchIfBitsEqual(jit, traits.width, r.value, traits.absentBits));
            exits.cannot.append(jit.jump());
            return;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    if (known)
        readAndReturn(representationWithCodeForRead(*known));
    else
        emitDispatchOnRepresentationForRead(jit, r.field.representation, r.commonRepresentations, readAndReturn);

    returnsUndefined.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), r.result);
    jit.returnToCaller();

    returnsNull.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueNull), r.result);
    jit.returnToCaller();

    returnsNaN.link(&jit);
    boxNaN(jit, r.number, r.result);
    jit.returnToCaller();
}

void branchOnFieldPresence(StubAssembler& jit, const FieldAddressRegisters& r, GPRReg scratch, JumpList& isPresent, JumpList& isAbsent)
{
    RELEASE_ASSERT(noOverlap(r.address, r.representation, scratch));

    struct AbsenceTest {
        uint8_t width;
        uint64_t absentBits;
        JumpList representations;
    };
    Vector<AbsenceTest, 8> tests;
    for (SlotRepresentation representation : representationsInTestOrder) {
        const SlotTraits traits = traitsOf(representation);
        if (!traits.hasAbsent)
            continue;
        size_t index = tests.findIf([&](const AbsenceTest& test) {
            return test.width == traits.width && test.absentBits == traits.absentBits;
        });
        if (index == notFound) {
            index = tests.size();
            tests.append(AbsenceTest { traits.width, traits.absentBits, { } });
        }
        tests[index].representations.append(jit.branch32(CCallHelpers::Equal, r.representation, TrustedImm32(static_cast<int32_t>(representation))));
    }
    isPresent.append(jit.jump());

    for (const AbsenceTest& test : tests) {
        test.representations.link(&jit);
        loadSlotBits(jit, test.width, Address(r.address), scratch);
        isAbsent.append(branchIfBitsEqual(jit, test.width, scratch, test.absentBits));
        isPresent.append(jit.jump());
    }
}

void returnFieldPresence(StubAssembler& jit, const CachedSlot& slot, const FieldAddressRegisters& field, JumpList& isUnknown)
{
    RELEASE_ASSERT(slot.kind() == Slot::Kind::Field);
    JumpList isPresent;
    JumpList isAbsent;

    slot.loadFirstPayloadWord(jit, field.slotData);
    Jump isOutOfLine = slot.branchIfFlag(jit, Slot::isOutOfLine);
    computeFieldAddress(jit, field, false, isAbsent);
    Label hasAddress = jit.label();
    branchOnFieldPresence(jit, field, field.slotData, isPresent, isAbsent);

    isOutOfLine.link(&jit);
    slot.loadHiddenWordOffset(jit, field.extensionWordOffset);
    computeFieldAddress(jit, field, true, isAbsent);
    jit.jump().linkTo(hasAddress, &jit);

    isPresent.link(&jit);
    jit.move(TrustedImm32(1), jit.result());
    jit.returnToCaller();

    isAbsent.link(&jit);
    isUnknown.append(slot.branchUnlessFlag(jit, Slot::absentIsUndefined));
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();
}

StubSignature signatureOfPrivateGet(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(firstStubOperandRegister), gprOf(StubRegister::A1) });
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

static Address slotOfButterfly(GPRReg butterfly, unsigned slot)
{
    return Address(butterfly, static_cast<int32_t>(JSCellButterfly::offsetOfData() + slot * sizeof(EncodedJSValue)));
}

// At a location above zero from the object, below zero from its butterfly. The location is extended in place.
static void loadAtLocation(StubAssembler& jit, GPRReg object, GPRReg location, GPRReg storage, GPRReg result)
{
    jit.signExtend32ToPtr(location, location);
    Jump isInObject = jit.branchPtr(CCallHelpers::GreaterThan, location, CCallHelpers::TrustedImmPtr(nullptr));
    jit.loadPtr(Address(object, JSObject::butterflyOffset()), storage);
    jit.load64(BaseIndex(storage, location, CCallHelpers::TimesOne), result);
    Jump done = jit.jump();
    isInObject.link(&jit);
    jit.load64(BaseIndex(object, location, CCallHelpers::TimesOne), result);
    done.link(&jit);
}

static constexpr unsigned shiftOfRecordsOfField = WTF::fastLog2(ClassToken::slotsPerField * static_cast<unsigned>(sizeof(EncodedJSValue)));
static_assert(ClassToken::slotsPerField * sizeof(EncodedJSValue) == 1u << shiftOfRecordsOfField);

// The two usual reads of a private name.
void generatePrivateGet(StubAssembler& jit)
{
    constexpr unsigned fieldSizeShift = WTF::fastLog2(static_cast<unsigned>(sizeof(FieldData)));
    static_assert(sizeof(FieldData) == 1u << fieldSizeShift);
    using WordsOfLayout = std::array<uint32_t, sizeof(LayoutData) / sizeof(uint32_t)>;
    LayoutData withEveryEvaluationBit { };
    withEveryEvaluationBit.evaluationBits = 3;
    LayoutData evaluatedOnce { };
    evaluatedOnce.evaluationBits = static_cast<uint32_t>(EvaluationState::ProvenOnce);
    const uint32_t evaluationMask = std::bit_cast<WordsOfLayout>(withEveryEvaluationBit)[0];
    const uint32_t provenOnce = std::bit_cast<WordsOfLayout>(evaluatedOnce)[0];
    RELEASE_ASSERT(evaluationMask && provenOnce && !(provenOnce & ~evaluationMask));

    const GPRReg object = jit.operand(0);
    const GPRReg token = jit.operand(1);
    const GPRReg field = jit.immediate();
    const GPRReg T10 = jit.allocateScratch(StubRegister::T10);
    const GPRReg T11 = jit.allocateScratch(StubRegister::T11);
    const GPRReg T12 = jit.allocateScratch(StubRegister::T12);
    RELEASE_ASSERT(noOverlap(object, token, field, T10, T11, T12));
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    JumpList goesToRunTime;
    const std::optional<SlotRepresentation> knownRepresentation = representationKnownTo(jit.key());
    const FieldReadRegisters registers {
        .field = { .object = object, .slotData = T10, .extensionWordOffset = T11, .address = T11, .representation = T12 },
        .value = T10,
        .result = jit.result(),
        .number = number,
        .numberScratch = jit.allocateScratch(FPRInfo::fpRegT1),
    };

    // By the record of the Structure of `object`, which is no compiled object.
    auto readAtKnownLocationAndReturn = [&](JumpList& isNotKnown) {
        const GPRReg records = T12;
        const GPRReg location = T11;
        const GPRReg storage = T10;
        constexpr unsigned bytesOfRecord = ClassToken::slotsPerRecord * sizeof(EncodedJSValue);
        static_assert(ClassToken::recordsProbedByCompiledCode == 2);
        jit.load64(slotOfButterfly(token, ClassToken::knownStructuresSlot), records);
        isNotKnown.append(jit.branchTest64(CCallHelpers::Zero, records));
        jit.zeroExtend32ToWord(field, location);
        jit.lshift64(TrustedImm32(shiftOfRecordsOfField), location);
        jit.addPtr(location, records);
        jit.load32(Address(object, JSCell::structureIDOffset()), storage);
        Jump isFirst = jit.branch32(CCallHelpers::Equal, storage, slotOfButterfly(records, ClassToken::StructureNumber));
        isNotKnown.append(jit.branch32(CCallHelpers::NotEqual, storage, slotOfButterfly(records, ClassToken::slotsPerRecord + ClassToken::StructureNumber)));
        jit.addPtr(TrustedImm32(bytesOfRecord), records);
        isFirst.link(&jit);

        jit.load32(slotOfButterfly(records, ClassToken::LocationOfBrand), location);
        Jump needsNoBrand = jit.branchTest32(CCallHelpers::Zero, location);
        loadAtLocation(jit, object, location, storage, location);
        isNotKnown.append(jit.branch64(CCallHelpers::NotEqual, location, token));
        needsNoBrand.link(&jit);
        jit.load32(slotOfButterfly(records, ClassToken::LocationOfValue), location);
        loadAtLocation(jit, object, location, storage, jit.result());
        jit.returnToCaller();
    };

    goesToRunTime.append(jit.branchIfNotCell(object));
    JumpList isNotCompiledObject = branchIfNotCompiledObjectOfThisImage(jit, object);
    Jump isOfOtherClass;
    Label readsFieldOfRecord;
    {
        const GPRReg layout = T12;
        const GPRReg record = T11;
        loadStructure(jit, object, layout);
        // Another realm that runs the image has evaluated the class for itself.
        if (!jit.options().hasOneInstance) {
            jit.move(layout, T10);
            goesToRunTime.append(branchIfNotOfThisRealm(jit, T10));
        }
        jit.loadPtr(Address(layout, CompiledStructure::offsetOfLayout()), layout);
        // The number is an int32 in a JSValue: its lower half is the number.
        jit.load32(slotOfButterfly(token, ClassToken::classIndexSlot), T10);
        isOfOtherClass = jit.branch32(CCallHelpers::NotEqual, T10, Address(layout, OBJECT_OFFSETOF(LayoutData, index)));
        if (!knownRepresentation) {
            jit.load32(Address(layout), T10);
            jit.and32(TrustedImm32(static_cast<int32_t>(evaluationMask)), T10);
            goesToRunTime.append(jit.branch32(CCallHelpers::NotEqual, T10, TrustedImm32(static_cast<int32_t>(provenOnce))));
            jit.zeroExtend32ToWord(field, record);
            jit.lshift64(TrustedImm32(fieldSizeShift), record);
            jit.addPtr(layout, record);
            jit.addPtr(TrustedImm32(sizeof(LayoutData)), record);
        }

        FieldReadExits exits;
        if (knownRepresentation) {
            jit.urshift32(field, TrustedImm32(bitsOfFieldInPrivateGet), registers.field.address);
            jit.addPtr(object, registers.field.address);
        } else {
            readsFieldOfRecord = jit.label();
            goesToRunTime.append(jit.branchTest32(CCallHelpers::NonZero, Address(record, OBJECT_OFFSETOF(FieldData, flags)), TrustedImm32(fieldMayExistWithoutStorage | fieldIsAccessor)));
            jit.load32(Address(record, OBJECT_OFFSETOF(FieldData, slot)), registers.field.slotData);
            Jump isOutOfLine = jit.branchTest32(CCallHelpers::NonZero, Address(record, OBJECT_OFFSETOF(FieldData, flags)), TrustedImm32(fieldIsOutOfLine));
            computeFieldAddress(jit, registers.field, false, exits.isAbsent);
            Jump hasAddress = jit.jump();
            isOutOfLine.link(&jit);
            loadExtensionWordOffset(jit, object, registers.field.extensionWordOffset);
            computeFieldAddress(jit, registers.field, true, exits.isAbsent);
            hasAddress.link(&jit);
        }
        readFieldAndReturn(jit, registers, exits, knownRepresentation);
        goesToRunTime.append(exits.isAbsent);
        goesToRunTime.append(exits.cannot);
    }

    // An instance of a class below this one, whose parent the compiler does not know.
    isOfOtherClass.link(&jit);
    if (knownRepresentation) {
        jit.and32(TrustedImm32((1 << bitsOfFieldInPrivateGet) - 1), field);
        jit.jumpToStub(privateGet());
    } else {
        // Its layout has a field for the name, which the run time has found before.
        const GPRReg layout = T12;
        const GPRReg layouts = T11;
        const GPRReg place = T12;
        const GPRReg record = T11;
        JumpList hasNoPlace;
        jit.load32(Address(layout, OBJECT_OFFSETOF(LayoutData, index)), layouts);
        jit.lshift64(TrustedImm32(32), layouts);
        jit.or64(T10, layouts);
        jit.load32(Address(layout, OBJECT_OFFSETOF(LayoutData, index)), T10);
        jit.xor32(field, T10);
        jit.and32(TrustedImm32(numberOfPrivateFieldPlaces - 1), T10);
        jit.lshift32(TrustedImm32(shiftOfPrivateFieldPlace), T10);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.privateFieldPlaces)), place);
        jit.addPtr(T10, place);
        hasNoPlace.append(jit.branch64(CCallHelpers::NotEqual, layouts, Address(place, OBJECT_OFFSETOF(PrivateFieldPlace, layouts))));
        hasNoPlace.append(jit.branch32(CCallHelpers::NotEqual, field, Address(place, OBJECT_OFFSETOF(PrivateFieldPlace, declaredField))));
        jit.loadPtr(Address(place, OBJECT_OFFSETOF(PrivateFieldPlace, field)), record);
        jit.jump().linkTo(readsFieldOfRecord, &jit);
        hasNoPlace.link(&jit);
    }
    // Its layout lacks the names of this class, and they lie among the extra properties.
    if (!knownRepresentation) {
        const GPRReg extraProperties = T10;
        const GPRReg hiddenWordOffset = T11;
        const FPRReg instance = number;
        goesToRunTime.append(jit.branchTest32(CCallHelpers::Zero, Address(object, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
        loadStructure(jit, object, T12);
        jit.load32(Address(T12, CompiledStructure::offsetOfExtraPropertiesSlot()), hiddenWordOffset);
        goesToRunTime.append(jit.branchTest32(CCallHelpers::Zero, hiddenWordOffset, TrustedImm32(SlotData::maxOffset)));
        jit.and32(TrustedImm32(SlotData::maxOffset), hiddenWordOffset);
        loadExtraProperties(jit, object, hiddenWordOffset, extraProperties, goesToRunTime);
        jit.move64ToDouble(object, instance);
        jit.move(extraProperties, object);
        JumpList isNotKnown;
        readAtKnownLocationAndReturn(isNotKnown);
        isNotKnown.link(&jit);
        jit.moveDoubleTo64(instance, object);
        goesToRunTime.append(jit.jump());
    }

    isNotCompiledObject.link(&jit);
    if (knownRepresentation)
        jit.and32(TrustedImm32((1 << bitsOfFieldInPrivateGet) - 1), field);
    readAtKnownLocationAndReturn(goesToRunTime);

    goesToRunTime.link(&jit);
    if (knownRepresentation)
        jit.and32(TrustedImm32((1 << bitsOfFieldInPrivateGet) - 1), field);
    jit.callOperationAndReturn(Entry::operationAOTPrivateGet, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = object,
        .thirdArgumentIn = token,
        .fourthArgumentIn = field,
    });
}

// Runs `emit`, which falls through where it has done the work of the operation and leaves by its list where it has not. A call that preserves registers may write that of its immediate alone, so there the scratch registers are saved around the work.
static PreludeExit emitPreludeWithScratches(StubAssembler& jit, std::initializer_list<StubRegister> scratches, NOESCAPE const ScopedLambda<void(JumpList& callsOperation)>& emit, NOESCAPE const ScopedLambda<void()>& leave)
{
    JumpList callsOperation;
    if (jit.key().family() == StubFamily::CallOperation) {
        for (StubRegister scratch : scratches)
            jit.allocateScratch(scratch);
        emit(callsOperation);
        leave();
        callsOperation.link(&jit);
        return PreludeExit::CallsOperation;
    }

    const GPRReg immediate = *jit.signature().immediate;
    jit.recordClobbers(RegisterSet(immediate));
    Vector<GPRReg, 8> saved;
    for (StubRegister scratch : scratches) {
        if (gprOf(scratch) != immediate)
            saved.append(gprOf(scratch));
    }
    jit.preserving(saved.span(), [&] {
        for (StubRegister scratch : scratches) {
            if (gprOf(scratch) != immediate)
                jit.allocateScratch(scratch);
        }
        emit(callsOperation);
        jit.move(TrustedImm32(0), immediate);
        Jump isDone = jit.jump();
        callsOperation.link(&jit);
        jit.move(TrustedImm32(1), immediate);
        isDone.link(&jit);
    });
    Jump isNotDone = jit.branchTest32(CCallHelpers::NonZero, immediate);
    jit.returnToCaller();
    isNotDone.link(&jit);
    return PreludeExit::CallsOperation;
}

static bool canHavePreludeWithScratches(StubAssembler& jit)
{
    StubFamily family = jit.key().family();
    return (family == StubFamily::CallOperation || family == StubFamily::CallOperationPreservingRegisters) && jit.signature().result == StubResult::None;
}

// The extra properties of a compiled object, any other object itself.
static void loadHolderOfNamesWithoutField(StubAssembler& jit, GPRReg object, GPRReg holder, GPRReg structure, GPRReg hiddenWordOffset, JumpList& hasNone)
{
    jit.move(object, holder);
    Jump isNotCompiledObject = jit.branchIfNotType(object, CompiledObjectType);
    hasNone.append(jit.branchTest32(CCallHelpers::Zero, Address(object, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties)));
    loadStructure(jit, object, structure);
    jit.load32(Address(structure, CompiledStructure::offsetOfExtraPropertiesSlot()), hiddenWordOffset);
    hasNone.append(jit.branchTest32(CCallHelpers::Zero, hiddenWordOffset, TrustedImm32(SlotData::maxOffset)));
    jit.and32(TrustedImm32(SlotData::maxOffset), hiddenWordOffset);
    loadExtraProperties(jit, object, hiddenWordOffset, holder, hasNone);
    isNotCompiledObject.link(&jit);
}

// The usual store under a private name that no field holds, found as generatePrivateGet() finds it.
PreludeExit emitPrivateSetPrelude(StubAssembler& jit)
{
    if (!canHavePreludeWithScratches(jit))
        return PreludeExit::CallsOperation;

    constexpr GPRReg object = operationArgumentGPR(1);
    constexpr GPRReg token = operationArgumentGPR(2);
    constexpr GPRReg field = operationArgumentGPR(3);
    constexpr GPRReg value = operationArgumentGPR(4);
    constexpr GPRReg records = gprOf(StubRegister::T13);
    constexpr GPRReg location = gprOf(StubRegister::T11);
    constexpr GPRReg storage = gprOf(StubRegister::T12);
    constexpr GPRReg owner = gprOf(firstStubOperandRegister);
    static_assert(noOverlap(object, token, field, value, records, location, storage, owner, instanceGPR));
    constexpr unsigned bytesOfRecord = ClassToken::slotsPerRecord * sizeof(EncodedJSValue);
    static_assert(ClassToken::recordsProbedByCompiledCode == 2);
    bool leavesThroughBarrier = jit.key().family() == StubFamily::CallOperation;

    return emitPreludeWithScratches(jit, { StubRegister::T13, StubRegister::T11, StubRegister::T12, firstStubOperandRegister }, [&](JumpList& callsOperation) {
        callsOperation.append(jit.branchIfNotCell(object));
        loadHolderOfNamesWithoutField(jit, object, owner, storage, location, callsOperation);

        jit.load64(slotOfButterfly(token, ClassToken::knownStructuresSlot), records);
        callsOperation.append(jit.branchTest64(CCallHelpers::Zero, records));
        jit.zeroExtend32ToWord(field, location);
        jit.lshift64(TrustedImm32(shiftOfRecordsOfField), location);
        jit.addPtr(location, records);
        jit.load32(Address(owner, JSCell::structureIDOffset()), storage);
        Jump isFirst = jit.branch32(CCallHelpers::Equal, storage, slotOfButterfly(records, ClassToken::StructureNumber));
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, storage, slotOfButterfly(records, ClassToken::slotsPerRecord + ClassToken::StructureNumber)));
        jit.addPtr(TrustedImm32(bytesOfRecord), records);
        isFirst.link(&jit);

        jit.load32(slotOfButterfly(records, ClassToken::LocationOfBrand), location);
        Jump needsNoBrand = jit.branchTest32(CCallHelpers::Zero, location);
        loadAtLocation(jit, owner, location, storage, location);
        callsOperation.append(jit.branch64(CCallHelpers::NotEqual, location, token));
        needsNoBrand.link(&jit);
        // The stub of the barrier writes registers.
        if (!leavesThroughBarrier)
            callsOperation.append(branchIfBarrierIsNeeded(jit, owner, storage));

        jit.load32(slotOfButterfly(records, ClassToken::LocationOfValue), location);
        jit.signExtend32ToPtr(location, location);
        jit.move(owner, storage);
        Jump isInObject = jit.branchPtr(CCallHelpers::GreaterThan, location, CCallHelpers::TrustedImmPtr(nullptr));
        jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
        isInObject.link(&jit);
        jit.store64(value, BaseIndex(storage, location, CCallHelpers::TimesOne));
    }, [&] {
        jit.jumpToStub(writeBarrier());
    });
}

// The usual definition of a private name that no field holds: an object of the same Structure has taken the name before.
PreludeExit emitPrivateDefinePrelude(StubAssembler& jit)
{
    if (!canHavePreludeWithScratches(jit))
        return PreludeExit::CallsOperation;

    constexpr GPRReg object = operationArgumentGPR(1);
    constexpr GPRReg token = operationArgumentGPR(2);
    constexpr GPRReg field = operationArgumentGPR(3);
    constexpr GPRReg value = operationArgumentGPR(4);
    constexpr GPRReg definition = gprOf(StubRegister::T13);
    constexpr GPRReg place = gprOf(StubRegister::T11);
    constexpr GPRReg storage = gprOf(StubRegister::T12);
    constexpr GPRReg structureAfter = gprOf(StubRegister::T9);
    // What takes the name: the object itself, or the extra properties of a compiled object.
    constexpr GPRReg owner = gprOf(firstStubOperandRegister);
    static_assert(noOverlap(object, token, field, value, definition, place, storage, structureAfter, owner, instanceGPR));
    RELEASE_ASSERT(signatureOfWriteBarrier(writeBarrier()).operands[0] == Reg(owner));
    constexpr int32_t bytesOfDefinition = ClassToken::slotsPerDefinition * sizeof(EncodedJSValue);
    auto slotOfDefinition = [&](ClassToken::DefinitionSlot slot, unsigned half = 0) {
        return slotOfButterfly(definition, ClassToken::slotOfDefinition(0, 0) + slot).withOffset(static_cast<int32_t>(half * sizeof(uint16_t)));
    };
    bool leavesThroughBarrier = jit.key().family() == StubFamily::CallOperation;

    // `place` holds the number of the Structure that the owner has before.
    auto findDefinition = [&](JumpList& isNotKnown) {
        JumpList isFound;
        jit.load64(slotOfButterfly(token, ClassToken::knownStructuresSlot), definition);
        isNotKnown.append(jit.branchTest64(CCallHelpers::Zero, definition));
        jit.zeroExtend32ToWord(field, storage);
        jit.lshift64(TrustedImm32(shiftOfRecordsOfField), storage);
        jit.addPtr(storage, definition);
        for (unsigned probed = 0; probed + 1 < ClassToken::definitionsPerField; ++probed) {
            isFound.append(jit.branch32(CCallHelpers::Equal, place, slotOfDefinition(ClassToken::NumberOfStructureBefore)));
            jit.addPtr(TrustedImm32(bytesOfDefinition), definition);
        }
        isNotKnown.append(jit.branch32(CCallHelpers::NotEqual, place, slotOfDefinition(ClassToken::NumberOfStructureBefore)));
        isFound.link(&jit);
    };
    auto checkBrand = [&](JumpList& isOfOtherEvaluation) {
        jit.load16SignedExtendTo32(slotOfDefinition(ClassToken::Places, 1), place);
        Jump needsNoBrand = jit.branchTest32(CCallHelpers::Zero, place);
        loadAtLocation(jit, owner, place, storage, place);
        isOfOtherEvaluation.append(jit.branch64(CCallHelpers::NotEqual, place, token));
        needsNoBrand.link(&jit);
    };
    // structureAfter holds the number of the next Structure of the owner. Nothing is written where it cannot.
    auto defineInOwner = [&](GPRReg oldCapacity, JumpList& cannot) {
        jit.load16SignedExtendTo32(slotOfDefinition(ClassToken::Places), place);
        Jump growsStorage = jit.branchTest32(CCallHelpers::NonZero, place, TrustedImm32(1));
        jit.signExtend32ToPtr(place, place);
        jit.move(owner, storage);
        Jump isInObject = jit.branchPtr(CCallHelpers::GreaterThan, place, CCallHelpers::TrustedImmPtr(nullptr));
        jit.loadPtr(Address(owner, JSObject::butterflyOffset()), storage);
        isInObject.link(&jit);
        jit.store64(value, BaseIndex(storage, place, CCallHelpers::TimesOne));
        jit.store32(structureAfter, Address(owner, JSCell::structureIDOffset()));
        Jump isDefined = jit.jump();

        growsStorage.link(&jit);
        const GPRReg newCapacity = place;
        jit.urshift32(TrustedImm32(1), newCapacity);
        jit.move(TrustedImm32(0), oldCapacity);
        Jump isFirstStorage = jit.branch32(CCallHelpers::Equal, newCapacity, TrustedImm32(initialOutOfLineCapacity));
        jit.urshift32(newCapacity, TrustedImm32(1), oldCapacity);
        isFirstStorage.link(&jit);
        growOutOfLineStorage(jit, {
            .object = owner,
            .newStructureID = structureAfter,
            .oldCapacity = oldCapacity,
            .newCapacity = newCapacity,
            .storage = storage,
            .scratch = definition,
            .addedValue = value,
        }, cannot);
        isDefined.link(&jit);
    };
    // Runs `emit` with two more registers to write. `place` says afterwards whether it left by its list.
    auto withTokenAndField = [&](JumpList& callsOperation, const ScopedLambda<void(JumpList& cannot)>& emit) {
        jit.preserving({ token, field }, [&] {
            JumpList cannot;
            emit(cannot);
            jit.move(TrustedImm32(0), place);
            Jump isDecided = jit.jump();
            cannot.link(&jit);
            jit.move(TrustedImm32(1), place);
            isDecided.link(&jit);
        });
        callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, place));
    };

    return emitPreludeWithScratches(jit, { StubRegister::T13, StubRegister::T11, StubRegister::T12, StubRegister::T9, firstStubOperandRegister }, [&](JumpList& callsOperation) {
        JumpList isDefined;
        callsOperation.append(jit.branchIfNotCell(object));
        Jump isCompiledObject = jit.branchIfType(object, CompiledObjectType);

        jit.move(object, owner);
        jit.load32(Address(object, JSCell::structureIDOffset()), place);
        findDefinition(callsOperation);
        checkBrand(callsOperation);
        // The stub of the barrier writes registers.
        if (!leavesThroughBarrier)
            callsOperation.append(branchIfBarrierIsNeeded(jit, owner, storage));
        jit.load64(slotOfDefinition(ClassToken::StructureAfter), structureAfter);
        jit.emitEncodeStructureID(structureAfter, structureAfter);
        withTokenAndField(callsOperation, [&](JumpList& cannot) {
            defineInOwner(token, cannot);
        });
        isDefined.append(jit.jump());

        // The name goes among the extra properties. The Structure of the instance names theirs, and what it leads on to names their next one.
        isCompiledObject.link(&jit);
        const GPRReg successor = structureAfter;
        loadStructure(jit, object, storage);
        jit.loadPtr(Address(storage, CompiledStructure::offsetOfSuccessorByPrivateName()), successor);
        callsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, successor));
        callsOperation.append(branchIfBarrierIsNeeded(jit, object, place));
        Jump hasNoExtraProperties = jit.branchTest32(CCallHelpers::Zero, Address(object, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties));

        jit.load32(Address(successor, CompiledStructure::offsetOfExtraPropertiesSlot()), place);
        jit.and32(TrustedImm32(SlotData::maxOffset), place);
        loadExtraProperties(jit, object, place, owner, callsOperation);
        jit.load32(Address(owner, JSCell::structureIDOffset()), place);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, place, Address(storage, CompiledStructure::offsetOfExtraPropertiesStructureID())));
        findDefinition(callsOperation);
        checkBrand(callsOperation);
        if (!leavesThroughBarrier)
            callsOperation.append(branchIfBarrierIsNeeded(jit, owner, storage));
        jit.load64(slotOfDefinition(ClassToken::StructureAfter), storage);
        jit.emitEncodeStructureID(storage, storage);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, storage, Address(successor, CompiledStructure::offsetOfExtraPropertiesStructureID())));
        withTokenAndField(callsOperation, [&](JumpList& cannot) {
            constexpr GPRReg structureOfInstance = field;
            jit.emitEncodeStructureID(successor, structureOfInstance);
            jit.move(storage, structureAfter);
            defineInOwner(token, cannot);
            jit.store32(structureOfInstance, Address(object, JSCell::structureIDOffset()));
        });
        isDefined.append(jit.jump());

        // The first name outside the fields: the extra properties are made with it in their first word.
        hasNoExtraProperties.link(&jit);
        callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(object, offsetOfOrderWord), TrustedImm32(orderWordIsNotExtensible)));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.emptyExtraPropertiesStructure)), place);
        jit.emitEncodeStructureID(place, place);
        findDefinition(callsOperation);
        jit.load64(slotOfDefinition(ClassToken::StructureAfter), storage);
        jit.emitEncodeStructureID(storage, storage);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, storage, Address(successor, CompiledStructure::offsetOfExtraPropertiesStructureID())));
        withTokenAndField(callsOperation, [&](JumpList& cannot) {
            constexpr GPRReg size = token;
            constexpr GPRReg allocator = definition;
            static_assert(MarkedSpace::sizeStep == 2 * sizeof(Allocator));
            jit.load16(Address(successor, CompiledStructure::offsetOfExtraPropertiesCellSize()), size);
            jit.urshift32(size, TrustedImm32(1), place);
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
            jit.loadPtr(BaseIndex(allocator, place, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
            cannot.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
            allocateCell(jit, owner, allocator, place, cannot);
            jit.store32(storage, Address(owner, JSCell::structureIDOffset()));
            jit.store32(TrustedImm32(JSFinalObject::defaultTypeInfoBlob()), Address(owner, JSCell::indexingTypeAndMiscOffset()));
            jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), Address(owner, JSObject::butterflyOffset()));
            jit.store64(value, Address(owner, JSObject::offsetOfInlineStorage()));
            Label clears = jit.label();
            jit.sub32(TrustedImm32(sizeof(EncodedJSValue)), size);
            jit.store64(TrustedImm32(0), BaseIndex(owner, size, CCallHelpers::TimesOne));
            jit.branch32(CCallHelpers::Above, size, TrustedImm32(JSObject::offsetOfInlineStorage() + sizeof(EncodedJSValue))).linkTo(clears, &jit);
            fenceAfterAllocation(jit);
            jit.load32(Address(successor, CompiledStructure::offsetOfExtraPropertiesSlot()), place);
            jit.and32(TrustedImm32(SlotData::maxOffset), place);
            if (jit.options().useWideReferences)
                jit.store64(owner, BaseIndex(object, place, CCallHelpers::TimesOne));
            else {
                encodeCell32(jit, owner, storage);
                jit.store32(storage, BaseIndex(object, place, CCallHelpers::TimesOne));
            }
            jit.or32(TrustedImm32(orderWordHasExtraProperties), Address(object, offsetOfOrderWord));
            jit.emitEncodeStructureID(successor, storage);
            jit.store32(storage, Address(object, JSCell::structureIDOffset()));
        });

        isDefined.link(&jit);
    }, [&] {
        jit.jumpToStub(writeBarrier());
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
