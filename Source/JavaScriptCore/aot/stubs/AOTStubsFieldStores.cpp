/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsField.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTOperationData.h"
#include "AOTStubsStrings.h"
#include "JSCJSValue.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT {

static constexpr unsigned representationShift = 32;

static_assert(std::ranges::all_of(allMembersOf<SlotRepresentation>, [](SlotRepresentation representation) {
    SlotTraits traits = traitsOf(representation);
    return (!traits.hasAbsent || traits.width == 1 || traits.width == 4 || traits.width == 8)
        && (traits.valueKind != SlotValueKind::Boxed || traits.width == sizeof(EncodedJSValue))
        && traits.hasUndefined == traits.hasNull;
}));

static constexpr uint32_t representationsWithUndefinedAndNull = [] {
    static_assert(numberOfEnumerators<SlotRepresentation> <= 32);
    uint32_t representations = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        if (traitsOf(representation).hasUndefined)
            representations |= 1u << static_cast<unsigned>(representation);
    }
    return representations;
}();

namespace {

enum class StoredValue : uint8_t { IsNotCell, IsCell, MayBeCell };

struct Marker {
    bool exists;
    int32_t encodedValue;
    uint64_t bits;
};

} // namespace

void storeField(StubAssembler& jit, const FieldStoreRegisters& r, FieldStoreEnd end, FieldTypeTest typeTest, JumpList* stored, FieldStoreExits& exits)
{
    RELEASE_ASSERT(noOverlap(r.field.object, r.field.address, r.field.representation, r.value, r.type, r.scratch, r.secondScratch, instanceGPR));
    RELEASE_ASSERT((end == FieldStoreEnd::JoinList) == !!stored);
    const GPRReg value = r.value;
    const Address slot(r.field.address);
    std::optional<Label> storedCell;
    JumpList typeIsNotAccepted;

    // The first store of what may be a cell emits the barrier, and the later ones jump to it.
    auto didStore = [&](StoredValue kind) {
        if (end == FieldStoreEnd::JoinList)
            stored->append(jit.jump());
        else if (kind == StoredValue::IsNotCell)
            jit.returnToCaller();
        else if (!storedCell)
            storedCell = returnAfterStoring(jit, r.field.object, value);
        else if (kind == StoredValue::IsCell)
            jit.jump().linkTo(*storedCell, &jit);
        else {
            jit.branchIfCell(value).linkTo(*storedCell, &jit);
            jit.returnToCaller();
        }
    };
    auto branchIfSlotIsAbsent = [&](const SlotTraits& traits) {
        switch (traits.width) {
        case 1:
            return jit.branch8(CCallHelpers::Equal, slot, TrustedImm32(static_cast<int32_t>(traits.absentBits)));
        case 4:
            return jit.branch32(CCallHelpers::Equal, slot, TrustedImm32(static_cast<int32_t>(traits.absentBits)));
        case 8:
            if (!traits.absentBits)
                return jit.branchTest64(CCallHelpers::Zero, slot);
            return jit.branch64(CCallHelpers::Equal, slot, TrustedImm64(static_cast<int64_t>(traits.absentBits)));
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto storeBits = [&](unsigned width, uint64_t bits) {
        switch (width) {
        case 1:
            jit.store8(TrustedImm32(static_cast<int32_t>(bits)), slot);
            return;
        case 2:
            jit.store16(TrustedImm32(static_cast<int32_t>(bits)), slot);
            return;
        case 4:
            jit.store32(TrustedImm32(static_cast<int32_t>(bits)), slot);
            return;
        case 8:
            jit.store64(TrustedImm64(static_cast<int64_t>(bits)), slot);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto storeUndefinedOrNull = [&](const SlotTraits& traits) {
        const Marker markers[] = {
            { traits.hasUndefined, JSValue::ValueUndefined, traits.undefinedBits },
            { traits.hasNull, JSValue::ValueNull, traits.nullBits },
        };
        for (const Marker& marker : markers) {
            if (!marker.exists)
                continue;
            Jump isOtherValue = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(marker.encodedValue));
            storeBits(traits.width, marker.bits);
            didStore(StoredValue::IsNotCell);
            isOtherValue.link(&jit);
        }
        exits.cannot.append(jit.jump());
    };
    auto storeCell = [&](const SlotTraits& traits) {
        if (traits.isNarrowReference) {
            encodeCell32(jit, value, r.scratch);
            jit.store32(r.scratch, slot);
        } else
            jit.store64(value, slot);
        didStore(StoredValue::IsCell);
    };
    auto storeCellField = [&](const SlotTraits& traits) {
        Jump isNotCell = jit.branchIfNotCell(value);
        storeCell(traits);

        isNotCell.link(&jit);
        storeUndefinedOrNull(traits);
    };
    auto storeLiteralStringField = [&](const SlotTraits& traits) {
        const GPRReg word = r.type;
        JumpList isEmpty;
        JumpList isStoredAsCell;
        Jump isNotCell = jit.branchIfNotCell(value);
        exits.cannot.append(jit.branchIfNotString(value));
        findLiteralStringWord(jit, { .string = value, .impl = r.scratch, .entry = word, .tables = r.secondScratch, .maxLength = r.field.representation }, isEmpty, isStoredAsCell, exits.cannot);
        if (traits.isNarrowReference)
            jit.store32(word, slot);
        else
            jit.store64(word, slot);
        didStore(StoredValue::IsNotCell);

        isEmpty.link(&jit);
        storeBits(traits.width, emptyLiteralStringWord);
        didStore(StoredValue::IsNotCell);

        isStoredAsCell.link(&jit);
        storeCell(traits);

        isNotCell.link(&jit);
        storeUndefinedOrNull(traits);
    };
    auto branchUnlessInt32Fits = [&](const SlotTraits& traits) {
        const GPRReg extended = r.scratch;
        const bool isSigned = traits.valueKind == SlotValueKind::SignedInteger;
        switch (traits.width) {
        case 1:
            if (isSigned)
                jit.signExtend8To32(value, extended);
            else
                jit.zeroExtend8To32(value, extended);
            exits.cannot.append(jit.branch32(CCallHelpers::NotEqual, extended, value));
            return;
        case 2:
            if (isSigned)
                jit.signExtend16To32(value, extended);
            else
                jit.zeroExtend16To32(value, extended);
            exits.cannot.append(jit.branch32(CCallHelpers::NotEqual, extended, value));
            return;
        case 4:
        case 8:
            if (!isSigned)
                exits.cannot.append(jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(0)));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto storeInt32 = [&](unsigned width) {
        switch (width) {
        case 1:
            jit.store8(value, slot);
            return;
        case 2:
            jit.store16(value, slot);
            return;
        case 4:
            jit.store32(value, slot);
            return;
        case 8:
            jit.signExtend32ToPtr(value, r.scratch);
            jit.store64(r.scratch, slot);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto storeIntegerField = [&](const SlotTraits& traits) {
        static_assert(absentInt32 < minInlineInt32 && undefinedInt32 < minInlineInt32 && nullInt32 < minInlineInt32);
        Jump isNotInt32 = jit.branchIfNotInt32(value);
        if (traits.width == sizeof(int32_t) && traits.hasAbsent)
            exits.cannot.append(jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(minInlineInt32)));
        else
            branchUnlessInt32Fits(traits);
        storeInt32(traits.width);
        didStore(StoredValue::IsNotCell);

        isNotInt32.link(&jit);
        storeUndefinedOrNull(traits);
    };
    auto storeDoubleField = [&](const SlotTraits& traits) {
        const FPRReg number = r.number;
        Jump isNotInt32 = jit.branchIfNotInt32(value);
        jit.convertInt32ToDouble(value, number);
        jit.storeDouble(number, slot);
        didStore(StoredValue::IsNotCell);

        isNotInt32.link(&jit);
        Jump isNotNumber = jit.branchIfNotNumber(value);
        unboxDouble(jit, value, r.scratch, number);
        jit.storeDouble(number, slot);
        didStore(StoredValue::IsNotCell);

        isNotNumber.link(&jit);
        storeUndefinedOrNull(traits);
    };
    auto storeFloatField = [&] {
        const GPRReg doubleBits = r.scratch;
        const GPRReg floatBits = r.secondScratch;
        const GPRReg roundTripBits = r.type;
        const FPRReg number = r.number;
        exits.cannot.append(jit.branchIfNotNumber(value));
        toDouble(jit, value, doubleBits, number);
        jit.moveDoubleTo64(number, doubleBits);
        jit.convertDoubleToFloat(number, number);
        jit.moveFloatTo32(number, floatBits);
        jit.convertFloatToDouble(number, number);
        jit.moveDoubleTo64(number, roundTripBits);
        exits.cannot.append(jit.branch64(CCallHelpers::NotEqual, doubleBits, roundTripBits));
        jit.store32(floatBits, slot);
        didStore(StoredValue::IsNotCell);
    };
    auto storeBooleanField = [&](const SlotTraits& traits) {
        static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1) && JSValue::ValueTrue <= UINT8_MAX);
        static_assert(traitsOf(SlotRepresentation::OptionalBoolean).undefinedBits == JSValue::ValueUndefined && traitsOf(SlotRepresentation::OptionalBoolean).nullBits == JSValue::ValueNull);
        const GPRReg scratch = r.scratch;
        const bool storesEncodedValue = traits.hasAbsent;
        jit.and64(TrustedImm32(~1), value, scratch);
        Jump isNotBoolean = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm32(JSValue::ValueFalse));
        if (storesEncodedValue)
            jit.store8(value, slot);
        else {
            jit.and32(TrustedImm32(1), value, scratch);
            jit.store8(scratch, slot);
        }
        didStore(StoredValue::IsNotCell);

        isNotBoolean.link(&jit);
        storeUndefinedOrNull(traits);
    };
    auto storeWithRepresentation = [&](SlotRepresentation representation) {
        const SlotTraits traits = traitsOf(representation);
        if (end == FieldStoreEnd::ReturnAfterBarrier && traits.hasAbsent)
            exits.wasAbsent.append(branchIfSlotIsAbsent(traits));
        switch (traits.valueKind) {
        case SlotValueKind::Boxed:
            jit.store64(value, slot);
            didStore(StoredValue::MayBeCell);
            return;
        case SlotValueKind::Cell:
            storeCellField(traits);
            return;
        case SlotValueKind::LiteralString:
            storeLiteralStringField(traits);
            return;
        case SlotValueKind::SignedInteger:
        case SlotValueKind::UnsignedInteger:
            storeIntegerField(traits);
            return;
        case SlotValueKind::Double:
            storeDoubleField(traits);
            return;
        case SlotValueKind::Float:
            storeFloatField();
            return;
        case SlotValueKind::Boolean:
            storeBooleanField(traits);
            return;
        case SlotValueKind::DeferredClosure:
            exits.cannot.append(jit.jump());
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto unpackRepresentation = [&] {
        jit.urshift64(r.type, TrustedImm32(representationShift), r.field.representation);
        jit.zeroExtend32ToWord(r.type, r.type);
    };
    // branchUnlessTypeAccepts() takes three scratch registers and reads only bits 0 to 31 of type, so bits 32 to 63 carry the representation across it.
    auto branchUnlessFieldTypeAccepts = [&](JumpList& isNotAccepted) {
        jit.zeroExtend32ToWord(r.type, r.type);
        jit.lshift64(TrustedImm32(representationShift), r.field.representation);
        jit.or64(r.field.representation, r.type);
        branchUnlessTypeAccepts(jit, { value, r.type, r.field.representation, r.scratch, r.secondScratch }, isNotAccepted, isNotAccepted);
        unpackRepresentation();
    };
    auto jumpIfSlotHoldsValueAsMarker = [&](Label isAdmitted) {
        Jump isNeitherUndefinedNorNull = jit.branchIfNotOther(value, r.scratch);
        // The mark of the cell may answer for the raw value of the field, and Object::store() knows for which fields it does. A cell that has the mark, as every object has until its constructor is through, takes the marker here.
        Jump cellHasNoMark = jit.branchTest8(CCallHelpers::Zero, CCallHelpers::Address(r.field.object, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeInfoPerCellBit));
        LayoutData layoutWithSuchField { };
        layoutWithSuchField.hasSlotWithUnusedMarkers = 1;
        static_assert(sizeof(LayoutData) % sizeof(uint32_t) == 0);
        uint32_t hasSlotWithUnusedMarkers = std::bit_cast<std::array<uint32_t, sizeof(LayoutData) / sizeof(uint32_t)>>(layoutWithSuchField)[0];
        RELEASE_ASSERT(hasOneBitSet(hasSlotWithUnusedMarkers));
        loadStructure(jit, r.field.object, r.scratch);
        jit.loadPtr(Address(r.scratch, CompiledStructure::offsetOfLayout()), r.scratch);
        Jump someFieldTakesNoMarker = jit.branchTest32(CCallHelpers::NonZero, Address(r.scratch), TrustedImm32(static_cast<int32_t>(hasSlotWithUnusedMarkers)));
        jit.move(TrustedImm32(static_cast<int32_t>(representationsWithUndefinedAndNull)), r.scratch);
        jit.urshift32(r.field.representation, r.scratch);
        jit.branchTest32(CCallHelpers::NonZero, r.scratch, TrustedImm32(1)).linkTo(isAdmitted, &jit);
        someFieldTakesNoMarker.link(&jit);
        cellHasNoMark.link(&jit);
        isNeitherUndefinedNorNull.link(&jit);
    };

    if (typeTest == FieldTypeTest::IsEmitted)
        branchUnlessFieldTypeAccepts(typeIsNotAccepted);
    Label isAdmitted = jit.label();
    emitDispatchOnRepresentation(jit, r.field.representation, storeWithRepresentation);

    switch (end) {
    case FieldStoreEnd::JoinList:
        // Behind a type test this is the store to a variable, which has no other place to go to: its type is all that its slot may hold.
        exits.cannot.append(typeIsNotAccepted);
        return;
    case FieldStoreEnd::ReturnAfterBarrier:
        RELEASE_ASSERT(typeTest == FieldTypeTest::IsEmitted);
        typeIsNotAccepted.link(&jit);
        unpackRepresentation();
        jumpIfSlotHoldsValueAsMarker(isAdmitted);
        branchOnFieldPresence(jit, r.field, r.scratch, exits.cannot, exits.wasAbsentAndIsNotAccepted);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
