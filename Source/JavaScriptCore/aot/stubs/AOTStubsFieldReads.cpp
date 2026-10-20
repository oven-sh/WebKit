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
#include "AOTStubHelpers.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
static_assert(emptyLiteralStringWord > undefinedCell32 && emptyLiteralStringWord > nullCell32 && emptyLiteralStringWord < minCell32);

struct SharedReadResults {
    JumpList isUndefined;
    JumpList isNull;
    JumpList isNaN;
};

static bool markersAreEncodedValues(const SlotTraits& traits)
{
    return (!traits.hasAbsent || traits.absentBits == boxedMarkers.absent)
        && (!traits.hasUndefined || traits.undefinedBits == boxedMarkers.undefined)
        && (!traits.hasNull || traits.nullBits == boxedMarkers.null);
}

static void loadSlotBits(StubAssembler& jit, unsigned width, Address slot, GPRReg bits)
{
    switch (width) {
    case 1:
        jit.load8(slot, bits);
        return;
    case 2:
        jit.load16(slot, bits);
        return;
    case 4:
        jit.load32(slot, bits);
        return;
    case 8:
        jit.load64(slot, bits);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Jump branchIfBitsEqual(StubAssembler& jit, unsigned width, GPRReg bits, uint64_t expected)
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

static void branchOnMarkers(StubAssembler& jit, const SlotTraits& traits, GPRReg bits, FieldReadExits& exits, SharedReadResults& shared)
{
    if (traits.hasAbsent)
        exits.isAbsent.append(branchIfBitsEqual(jit, traits.width, bits, traits.absentBits));
    if (traits.hasUndefined)
        shared.isUndefined.append(branchIfBitsEqual(jit, traits.width, bits, traits.undefinedBits));
    if (traits.hasNull)
        shared.isNull.append(branchIfBitsEqual(jit, traits.width, bits, traits.nullBits));
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
        // The bit of the granule goes to the top and the bits of later granules drop out. What is left counts the granule and those before it,
        // which is also the number of words from the start of the Extension to the granule. A shift takes its amount modulo 64.
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

static void returnValue(StubAssembler& jit, const FieldReadRegisters& r)
{
    jit.move(r.value, r.result);
    jit.returnToCaller();
}

static void returnInt32(StubAssembler& jit, const FieldReadRegisters& r)
{
    boxInt32(jit, r.value, r.result);
    jit.returnToCaller();
}

static void returnDouble(StubAssembler& jit, const FieldReadRegisters& r)
{
    boxDouble(jit, r.number, r.result);
    jit.returnToCaller();
}

static void returnNumber(StubAssembler& jit, const FieldReadRegisters& r)
{
    JumpList notInt32;
    branchIfNotExactInt32(jit, r.number, r.value, notInt32, r.numberScratch);
    returnInt32(jit, r);
    notInt32.link(&jit);
    returnDouble(jit, r);
}

static void readEncodedValue(StubAssembler& jit, const SlotTraits& traits, const FieldReadRegisters& r, FieldReadExits& exits)
{
    RELEASE_ASSERT(traits.width == sizeof(EncodedJSValue) && markersAreEncodedValues(traits));
    jit.load64(Address(r.field.address), r.value);
    if (traits.hasAbsent)
        exits.isAbsent.append(jit.branchTest64(CCallHelpers::Zero, r.value));
    returnValue(jit, r);
}

static void readReference(StubAssembler& jit, const SlotTraits& traits, const FieldReadRegisters& r, FieldReadExits& exits)
{
    RELEASE_ASSERT(traits.hasAbsent && markersAreEncodedValues(traits));
    const bool mayBeLiteralString = traits.valueKind == SlotValueKind::LiteralString;

    loadSlotBits(jit, traits.width, Address(r.field.address), r.value);
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
    returnValue(jit, r);

    if (!mayBeLiteralString)
        return;
    isLiteralString.link(&jit);
    loadLiteralString(jit, r.value, r.field.address, exits.cannot);
    returnValue(jit, r);
}

static void readNarrowInteger(StubAssembler& jit, const SlotTraits& traits, const FieldReadRegisters& r)
{
    RELEASE_ASSERT(!traits.hasAbsent && !traits.hasUndefined && !traits.hasNull);
    const bool isSigned = traits.valueKind == SlotValueKind::SignedInteger;
    const Address slot(r.field.address);

    switch (traits.width) {
    case 1:
        if (isSigned)
            jit.load8SignedExtendTo32(slot, r.value);
        else
            jit.load8(slot, r.value);
        returnInt32(jit, r);
        return;
    case 2:
        if (isSigned)
            jit.load16SignedExtendTo32(slot, r.value);
        else
            jit.load16(slot, r.value);
        returnInt32(jit, r);
        return;
    case 4: {
        jit.load32(slot, r.value);
        if (isSigned) {
            returnInt32(jit, r);
            return;
        }
        Jump notInt32 = jit.branch32(CCallHelpers::LessThan, r.value, TrustedImm32(0));
        returnInt32(jit, r);
        notInt32.link(&jit);
        jit.convertInt64ToDouble(r.value, r.number);
        returnDouble(jit, r);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void readInt64(StubAssembler& jit, const SlotTraits& traits, const FieldReadRegisters& r, FieldReadExits& exits, SharedReadResults& shared)
{
    RELEASE_ASSERT(traits.valueKind == SlotValueKind::SignedInteger);
    const GPRReg lowHalf = r.field.address;

    jit.load64(Address(r.field.address), r.value);
    jit.signExtend32ToPtr(r.value, lowHalf);
    Jump exceedsInt32 = jit.branch64(CCallHelpers::NotEqual, r.value, lowHalf);
    returnInt32(jit, r);
    exceedsInt32.link(&jit);
    branchOnMarkers(jit, traits, r.value, exits, shared);
    jit.convertInt64ToDouble(r.value, r.number);
    returnDouble(jit, r);
}

static void readInt32OrDouble(StubAssembler& jit, const SlotTraits& traits, const FieldReadRegisters& r, FieldReadExits& exits, SharedReadResults& shared)
{
    jit.load32(Address(r.field.address), r.value);
    Jump notInlineNumber = jit.branch32(CCallHelpers::LessThan, r.value, TrustedImm32(minInlineInt32));
    returnInt32(jit, r);
    notInlineNumber.link(&jit);
    branchOnMarkers(jit, traits, r.value, exits, shared);
    exits.cannot.append(jit.jump());
}

static void readDouble(StubAssembler& jit, const SlotTraits& traits, const FieldReadRegisters& r, FieldReadExits& exits, SharedReadResults& shared)
{
    jit.loadDouble(Address(r.field.address), r.number);
    Jump isNaN = jit.branchIfNaN(r.number);
    returnNumber(jit, r);
    if (!traits.hasAbsent && !traits.hasUndefined && !traits.hasNull) {
        shared.isNaN.append(isNaN);
        return;
    }
    isNaN.link(&jit);
    jit.moveDoubleTo64(r.number, r.value);
    branchOnMarkers(jit, traits, r.value, exits, shared);
    shared.isNaN.append(jit.jump());
}

static void readFloat(StubAssembler& jit, const FieldReadRegisters& r, SharedReadResults& shared)
{
    jit.loadFloat(Address(r.field.address), r.number);
    jit.convertFloatToDouble(r.number, r.number);
    shared.isNaN.append(jit.branchIfNaN(r.number));
    returnNumber(jit, r);
}

static void readBoolean(StubAssembler& jit, const SlotTraits& traits, const FieldReadRegisters& r, FieldReadExits& exits)
{
    jit.load8(Address(r.field.address), r.value);
    if (!traits.hasAbsent) {
        jit.or32(TrustedImm32(JSValue::ValueFalse), r.value, r.result);
        jit.returnToCaller();
        return;
    }
    RELEASE_ASSERT(markersAreEncodedValues(traits));
    exits.isAbsent.append(jit.branchTest32(CCallHelpers::Zero, r.value));
    returnValue(jit, r);
}

static void readDeferredClosure(StubAssembler& jit, const FieldReadRegisters& r, FieldReadExits& exits)
{
    jit.load32(Address(r.field.address), r.value);
    exits.cannot.append(jit.branch32(CCallHelpers::Below, r.value, TrustedImm32(minCell32)));
    decodeCell32(jit, r.value, r.result);
    jit.returnToCaller();
}

static void emitReadOf(StubAssembler& jit, SlotRepresentation representation, const FieldReadRegisters& r, FieldReadExits& exits, SharedReadResults& shared)
{
    const SlotTraits traits = traitsOf(representation);
    switch (traits.valueKind) {
    case SlotValueKind::Boxed:
        readEncodedValue(jit, traits, r, exits);
        return;
    case SlotValueKind::Cell:
        if (traits.isNarrowReference)
            readReference(jit, traits, r, exits);
        else
            readEncodedValue(jit, traits, r, exits);
        return;
    case SlotValueKind::LiteralString:
        readReference(jit, traits, r, exits);
        return;
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
        if (traits.width == sizeof(int64_t))
            readInt64(jit, traits, r, exits, shared);
        else if (traits.hasAbsent)
            readInt32OrDouble(jit, traits, r, exits, shared);
        else
            readNarrowInteger(jit, traits, r);
        return;
    case SlotValueKind::Int32OrDouble:
        readInt32OrDouble(jit, traits, r, exits, shared);
        return;
    case SlotValueKind::Double:
        readDouble(jit, traits, r, exits, shared);
        return;
    case SlotValueKind::Float:
        readFloat(jit, r, shared);
        return;
    case SlotValueKind::Boolean:
        readBoolean(jit, traits, r, exits);
        return;
    case SlotValueKind::DeferredClosure:
        readDeferredClosure(jit, r, exits);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void readFieldAndReturn(StubAssembler& jit, const FieldReadRegisters& r, FieldReadExits& exits)
{
    RELEASE_ASSERT(noOverlap(r.field.object, r.field.address, r.field.representation, r.value, r.number, r.numberScratch));
    RELEASE_ASSERT(noOverlap(r.result, r.field.address, r.field.representation, r.value));

    SharedReadResults shared;
    emitDispatchOnRepresentation(jit, r.field.representation, [&](SlotRepresentation representation) {
        emitReadOf(jit, representation, r, exits, shared);
    });

    shared.isUndefined.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), r.result);
    jit.returnToCaller();

    shared.isNull.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueNull), r.result);
    jit.returnToCaller();

    shared.isNaN.link(&jit);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(boxedNaN)), r.result);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
