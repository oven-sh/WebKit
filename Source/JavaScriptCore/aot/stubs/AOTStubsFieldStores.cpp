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
#include "AOTStubsKeyed.h"
#include "JSCJSValue.h"
#include <algorithm>
#include <array>
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

void storeField(StubAssembler& jit, const FieldStoreRegisters& r, FieldStoreEnd end, FieldTypeTest typeTest, JumpList* stored, FieldStoreExits& exits, const ScopedLambda<void(GPRReg classOfSlot)>* rememberStore)
{
    using ClassOfSlot = AcceptedKindOfValue::ClassOfSlot;
    RELEASE_ASSERT(!rememberStore || (end == FieldStoreEnd::ReturnAfterBarrier && typeTest != FieldTypeTest::HasPassed));
    std::array<JumpList, 4> storedInSlotOfClass;
    auto goOnToRememberStore = [&](ClassOfSlot classOfSlot) {
        storedInSlotOfClass[static_cast<size_t>(classOfSlot)].append(jit.jump());
    };
    RELEASE_ASSERT(noOverlap(r.field.object, r.field.address, r.field.representation, r.value, r.type, r.scratch, r.secondScratch, instanceGPR));
    RELEASE_ASSERT((end == FieldStoreEnd::JoinList) == !!stored);
    const GPRReg value = r.value;
    const Address slot(r.field.address);
    std::optional<Label> storedCell;
    JumpList typeIsNotAccepted;

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
    auto branchIfSlotIsAbsent = [&](SlotRepresentation representation) {
        const uint64_t bits = bitsBeforeCreation(representation);
        switch (widthOf(representation)) {
        case 1:
            return jit.branch8(CCallHelpers::Equal, slot, TrustedImm32(static_cast<int32_t>(bits)));
        case 4:
            return jit.branch32(CCallHelpers::Equal, slot, TrustedImm32(static_cast<int32_t>(bits)));
        case 8:
            if (!bits)
                return jit.branchTest64(CCallHelpers::Zero, slot);
            return jit.branch64(CCallHelpers::Equal, slot, TrustedImm64(static_cast<int64_t>(bits)));
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
    auto storeUndefinedOrNull = [&](const SlotTraits& traits, std::optional<ClassOfSlot> remembered = std::nullopt) {
        const Marker markers[] = {
            { traits.hasUndefined, JSValue::ValueUndefined, traits.undefinedBits },
            { traits.hasNull, JSValue::ValueNull, traits.nullBits },
        };
        for (const Marker& marker : markers) {
            if (!marker.exists)
                continue;
            Jump isOtherValue = jit.branch64(CCallHelpers::NotEqual, value, TrustedImm32(marker.encodedValue));
            storeBits(traits.width, marker.bits);
            if (remembered)
                goOnToRememberStore(*remembered);
            else
                didStore(StoredValue::IsNotCell);
            isOtherValue.link(&jit);
        }
        exits.cannot.append(jit.jump());
    };
    auto storeCell = [&](const SlotTraits& traits) {
        if (traits.isNarrowReference) {
            encodeCellOfProgram(jit, value, r.scratch);
            jit.store32(r.scratch, slot);
        } else
            jit.store64(value, slot);
        didStore(StoredValue::IsCell);
    };
    auto storeCellField = [&](const SlotTraits& traits, SlotRepresentation representation) {
        Jump isNotCell = jit.branchIfNotCell(value);
        if (!rememberStore) {
            storeCell(traits);

            isNotCell.link(&jit);
            storeUndefinedOrNull(traits);
            return;
        }
        const ClassOfSlot classOfSlot = representation == SlotRepresentation::Cell ? ClassOfSlot::Cell : representation == SlotRepresentation::Cell32 ? ClassOfSlot::Cell32 : ClassOfSlot::OptionalCell32;
        RELEASE_ASSERT(classOfSlot != ClassOfSlot::OptionalCell32 || representation == SlotRepresentation::OptionalCell32);
        if (traits.isNarrowReference) {
            encodeCellOfProgram(jit, value, r.scratch);
            jit.store32(r.scratch, slot);
        } else
            jit.store64(value, slot);
        goOnToRememberStore(classOfSlot);

        isNotCell.link(&jit);
        storeUndefinedOrNull(traits, classOfSlot);
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
        if (end == FieldStoreEnd::ReturnAfterBarrier && mayBeCreatedByStore(representation))
            exits.wasAbsent.append(branchIfSlotIsAbsent(representation));
        switch (traits.valueKind) {
        case SlotValueKind::Boxed:
            jit.store64(value, slot);
            if (rememberStore) {
                RELEASE_ASSERT(representation == SlotRepresentation::JSValue);
                goOnToRememberStore(ClassOfSlot::JSValue);
                return;
            }
            didStore(StoredValue::MayBeCell);
            return;
        case SlotValueKind::Cell:
            storeCellField(traits, representation);
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
    auto branchUnlessFieldTypeAccepts = [&](JumpList& isNotAccepted) {
        if (typeTest != FieldTypeTest::IsEmittedForSlot)
            jit.zeroExtend32ToWord(r.type, r.type);
        jit.lshift64(TrustedImm32(representationShift), r.field.representation);
        jit.or64(r.field.representation, r.type);
        const GPRReg holder = end == FieldStoreEnd::ReturnAfterBarrier ? r.field.object : InvalidGPRReg;
        branchUnlessTypeAccepts(jit, { value, r.type, r.field.representation, r.scratch, r.secondScratch, holder }, isNotAccepted, isNotAccepted);
        unpackRepresentation();
    };
    auto jumpIfSlotHoldsValueAsMarker = [&](Label isAdmitted) {
        Jump isNeitherUndefinedNorNull = jit.branchIfNotOther(value, r.scratch);
        Jump cellHasNoMark = branchUnlessCellBearsMark(jit, r.field.object);
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
        Jump hasNoMarkers = jit.branchTest32(CCallHelpers::Zero, r.scratch, TrustedImm32(1));
        if (rememberStore)
            jit.move(TrustedImm32(0), r.type);
        jit.jump().linkTo(isAdmitted, &jit);
        hasNoMarkers.link(&jit);
        someFieldTakesNoMarker.link(&jit);
        cellHasNoMark.link(&jit);
        isNeitherUndefinedNorNull.link(&jit);
    };

    if (typeTest != FieldTypeTest::HasPassed)
        branchUnlessFieldTypeAccepts(typeIsNotAccepted);
    Label isAdmitted = jit.label();
    emitDispatchOnRepresentation(jit, r.field.representation, storeWithRepresentation);

    if (rememberStore) {
        JumpList hasClass;
        for (size_t classOfSlot = 0; classOfSlot < storedInSlotOfClass.size(); ++classOfSlot) {
            storedInSlotOfClass[classOfSlot].link(&jit);
            jit.move(TrustedImm32(static_cast<int32_t>(classOfSlot)), r.secondScratch);
            hasClass.append(jit.jump());
        }
        hasClass.link(&jit);
        (*rememberStore)(r.secondScratch);
        didStore(StoredValue::MayBeCell);
    }

    switch (end) {
    case FieldStoreEnd::JoinList:
        RELEASE_ASSERT(typeTest != FieldTypeTest::IsEmittedForSlot);
        exits.cannot.append(typeIsNotAccepted);
        return;
    case FieldStoreEnd::ReturnAfterBarrier:
        RELEASE_ASSERT(typeTest != FieldTypeTest::HasPassed);
        typeIsNotAccepted.link(&jit);
        unpackRepresentation();
        jumpIfSlotHoldsValueAsMarker(isAdmitted);
        branchOnFieldPresence(jit, r.field, r.scratch, exits.cannot, exits.wasAbsentAndIsNotAccepted, exits.cannot);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
