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
    return !traits.hasAbsent || traits.width == 1 || traits.width == 4 || traits.width == 8;
}));
static_assert(std::ranges::all_of(allMembersOf<SlotRepresentation>, [](SlotRepresentation representation) {
    SlotTraits traits = traitsOf(representation);
    return traits.valueKind != SlotValueKind::Boxed || traits.width == sizeof(EncodedJSValue);
}));
static_assert(std::ranges::all_of(allMembersOf<SlotRepresentation>, [](SlotRepresentation representation) {
    return traitsOf(representation).hasUndefined == traitsOf(representation).hasNull;
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

struct FieldStore {
    Address slot() const { return Address(registers.field.address); }

    StubAssembler& jit;
    const FieldStoreRegisters& registers;
    FieldStoreEnd end;
    JumpList* stored;
    FieldStoreExits& exits;
    std::optional<Label> storedCell;
};

struct Marker {
    bool exists;
    int32_t encodedValue;
    uint64_t bits;
};

} // namespace

static void returnAfterBarrier(FieldStore& store, StoredValue stored)
{
    StubAssembler& jit = store.jit;
    if (!store.storedCell) {
        store.storedCell = returnAfterStoring(jit, store.registers.field.object, store.registers.value);
        return;
    }
    switch (stored) {
    case StoredValue::IsCell:
        jit.jump().linkTo(*store.storedCell, &jit);
        return;
    case StoredValue::MayBeCell:
        jit.branchIfCell(store.registers.value).linkTo(*store.storedCell, &jit);
        jit.returnToCaller();
        return;
    case StoredValue::IsNotCell:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void didStore(FieldStore& store, StoredValue stored)
{
    StubAssembler& jit = store.jit;
    switch (store.end) {
    case FieldStoreEnd::JoinList:
        store.stored->append(jit.jump());
        return;
    case FieldStoreEnd::ReturnAfterBarrier:
        if (stored == StoredValue::IsNotCell)
            jit.returnToCaller();
        else
            returnAfterBarrier(store, stored);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Jump branchIfSlotIsAbsent(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    switch (traits.width) {
    case 1:
        return jit.branch8(CCallHelpers::Equal, store.slot(), TrustedImm32(static_cast<int32_t>(traits.absentBits)));
    case 4:
        return jit.branch32(CCallHelpers::Equal, store.slot(), TrustedImm32(static_cast<int32_t>(traits.absentBits)));
    case 8:
        if (!traits.absentBits)
            return jit.branchTest64(CCallHelpers::Zero, store.slot());
        return jit.branch64(CCallHelpers::Equal, store.slot(), TrustedImm64(static_cast<int64_t>(traits.absentBits)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeBits(FieldStore& store, unsigned width, uint64_t bits)
{
    StubAssembler& jit = store.jit;
    switch (width) {
    case 1:
        jit.store8(TrustedImm32(static_cast<int32_t>(bits)), store.slot());
        return;
    case 2:
        jit.store16(TrustedImm32(static_cast<int32_t>(bits)), store.slot());
        return;
    case 4:
        jit.store32(TrustedImm32(static_cast<int32_t>(bits)), store.slot());
        return;
    case 8:
        jit.store64(TrustedImm64(static_cast<int64_t>(bits)), store.slot());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeUndefinedOrNull(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    const Marker markers[] = {
        { traits.hasUndefined, JSValue::ValueUndefined, traits.undefinedBits },
        { traits.hasNull, JSValue::ValueNull, traits.nullBits },
    };
    for (const Marker& marker : markers) {
        if (!marker.exists)
            continue;
        Jump isOtherValue = jit.branch64(CCallHelpers::NotEqual, store.registers.value, TrustedImm32(marker.encodedValue));
        storeBits(store, traits.width, marker.bits);
        didStore(store, StoredValue::IsNotCell);
        isOtherValue.link(&jit);
    }
    store.exits.cannot.append(jit.jump());
}

static void storeCell(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    if (traits.isNarrowReference) {
        encodeCell32(jit, store.registers.value, store.registers.scratch);
        jit.store32(store.registers.scratch, store.slot());
    } else
        jit.store64(store.registers.value, store.slot());
    didStore(store, StoredValue::IsCell);
}

static void storeCellField(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    Jump isNotCell = jit.branchIfNotCell(store.registers.value);
    storeCell(store, traits);

    isNotCell.link(&jit);
    storeUndefinedOrNull(store, traits);
}

static void storeLiteralStringWord(FieldStore& store, const SlotTraits& traits, JumpList& isStoredAsCell)
{
    StubAssembler& jit = store.jit;
    const FieldStoreRegisters& r = store.registers;
    const GPRReg word = r.type;
    JumpList isEmpty;

    findLiteralStringWord(jit, { .string = r.value, .impl = r.scratch, .entry = word, .tables = r.secondScratch, .maxLength = r.field.representation }, isEmpty, isStoredAsCell, store.exits.cannot);
    if (traits.isNarrowReference)
        jit.store32(word, store.slot());
    else
        jit.store64(word, store.slot());
    didStore(store, StoredValue::IsNotCell);

    isEmpty.link(&jit);
    storeBits(store, traits.width, emptyLiteralStringWord);
    didStore(store, StoredValue::IsNotCell);
}

static void storeLiteralStringField(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    JumpList isStoredAsCell;
    Jump isNotCell = jit.branchIfNotCell(store.registers.value);
    store.exits.cannot.append(jit.branchIfNotString(store.registers.value));
    storeLiteralStringWord(store, traits, isStoredAsCell);

    isStoredAsCell.link(&jit);
    storeCell(store, traits);

    isNotCell.link(&jit);
    storeUndefinedOrNull(store, traits);
}

static void branchUnlessInt32Fits(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    const GPRReg value = store.registers.value;
    const GPRReg extended = store.registers.scratch;
    const bool isSigned = traits.valueKind == SlotValueKind::SignedInteger;
    switch (traits.width) {
    case 1:
        if (isSigned)
            jit.signExtend8To32(value, extended);
        else
            jit.zeroExtend8To32(value, extended);
        store.exits.cannot.append(jit.branch32(CCallHelpers::NotEqual, extended, value));
        return;
    case 2:
        if (isSigned)
            jit.signExtend16To32(value, extended);
        else
            jit.zeroExtend16To32(value, extended);
        store.exits.cannot.append(jit.branch32(CCallHelpers::NotEqual, extended, value));
        return;
    case 4:
    case 8:
        if (!isSigned)
            store.exits.cannot.append(jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(0)));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeInt32(FieldStore& store, unsigned width)
{
    StubAssembler& jit = store.jit;
    const GPRReg value = store.registers.value;
    switch (width) {
    case 1:
        jit.store8(value, store.slot());
        return;
    case 2:
        jit.store16(value, store.slot());
        return;
    case 4:
        jit.store32(value, store.slot());
        return;
    case 8:
        jit.signExtend32ToPtr(value, store.registers.scratch);
        jit.store64(store.registers.scratch, store.slot());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void storeIntegerField(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    Jump isNotInt32 = jit.branchIfNotInt32(store.registers.value);
    branchUnlessInt32Fits(store, traits);
    storeInt32(store, traits.width);
    didStore(store, StoredValue::IsNotCell);

    isNotInt32.link(&jit);
    storeUndefinedOrNull(store, traits);
}

static void storeOptionalInt32Field(FieldStore& store, const SlotTraits& traits)
{
    static_assert(absentInt32 < minInlineInt32 && undefinedInt32 < minInlineInt32 && nullInt32 < minInlineInt32);
    StubAssembler& jit = store.jit;
    const GPRReg value = store.registers.value;
    Jump isNotInt32 = jit.branchIfNotInt32(value);
    store.exits.cannot.append(jit.branch32(CCallHelpers::LessThan, value, TrustedImm32(minInlineInt32)));
    jit.store32(value, store.slot());
    didStore(store, StoredValue::IsNotCell);

    isNotInt32.link(&jit);
    storeUndefinedOrNull(store, traits);
}

static void storeDoubleField(FieldStore& store, const SlotTraits& traits)
{
    StubAssembler& jit = store.jit;
    const GPRReg value = store.registers.value;
    const FPRReg number = store.registers.number;
    Jump isNotInt32 = jit.branchIfNotInt32(value);
    jit.convertInt32ToDouble(value, number);
    jit.storeDouble(number, store.slot());
    didStore(store, StoredValue::IsNotCell);

    isNotInt32.link(&jit);
    Jump isNotNumber = jit.branchIfNotNumber(value);
    unboxDouble(jit, value, store.registers.scratch, number);
    jit.storeDouble(number, store.slot());
    didStore(store, StoredValue::IsNotCell);

    isNotNumber.link(&jit);
    storeUndefinedOrNull(store, traits);
}

static void storeFloatField(FieldStore& store)
{
    StubAssembler& jit = store.jit;
    const GPRReg value = store.registers.value;
    const GPRReg doubleBits = store.registers.scratch;
    const GPRReg floatBits = store.registers.secondScratch;
    const GPRReg roundTripBits = store.registers.type;
    const FPRReg number = store.registers.number;
    store.exits.cannot.append(jit.branchIfNotNumber(value));
    toDouble(jit, value, doubleBits, number);
    jit.moveDoubleTo64(number, doubleBits);
    jit.convertDoubleToFloat(number, number);
    jit.moveFloatTo32(number, floatBits);
    jit.convertFloatToDouble(number, number);
    jit.moveDoubleTo64(number, roundTripBits);
    store.exits.cannot.append(jit.branch64(CCallHelpers::NotEqual, doubleBits, roundTripBits));
    jit.store32(floatBits, store.slot());
    didStore(store, StoredValue::IsNotCell);
}

static void storeBooleanField(FieldStore& store, const SlotTraits& traits)
{
    static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1) && JSValue::ValueTrue <= UINT8_MAX);
    static_assert(traitsOf(SlotRepresentation::OptionalBoolean).undefinedBits == JSValue::ValueUndefined && traitsOf(SlotRepresentation::OptionalBoolean).nullBits == JSValue::ValueNull);
    StubAssembler& jit = store.jit;
    const GPRReg value = store.registers.value;
    const GPRReg scratch = store.registers.scratch;
    const bool storesEncodedValue = traits.hasAbsent;
    jit.and64(TrustedImm32(~1), value, scratch);
    Jump isNotBoolean = jit.branch64(CCallHelpers::NotEqual, scratch, TrustedImm32(JSValue::ValueFalse));
    if (storesEncodedValue)
        jit.store8(value, store.slot());
    else {
        jit.and32(TrustedImm32(1), value, scratch);
        jit.store8(scratch, store.slot());
    }
    didStore(store, StoredValue::IsNotCell);

    isNotBoolean.link(&jit);
    storeUndefinedOrNull(store, traits);
}

static void storeWithRepresentation(FieldStore& store, SlotRepresentation representation)
{
    const SlotTraits traits = traitsOf(representation);
    if (store.end == FieldStoreEnd::ReturnAfterBarrier && traits.hasAbsent)
        store.exits.wasAbsent.append(branchIfSlotIsAbsent(store, traits));
    switch (traits.valueKind) {
    case SlotValueKind::Boxed:
        store.jit.store64(store.registers.value, store.slot());
        didStore(store, StoredValue::MayBeCell);
        return;
    case SlotValueKind::Cell:
        storeCellField(store, traits);
        return;
    case SlotValueKind::LiteralString:
        storeLiteralStringField(store, traits);
        return;
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
        if (traits.width == sizeof(int32_t) && traits.hasAbsent)
            storeOptionalInt32Field(store, traits);
        else
            storeIntegerField(store, traits);
        return;
    case SlotValueKind::Double:
        storeDoubleField(store, traits);
        return;
    case SlotValueKind::Float:
        storeFloatField(store);
        return;
    case SlotValueKind::Boolean:
        storeBooleanField(store, traits);
        return;
    case SlotValueKind::DeferredClosure:
        store.exits.cannot.append(store.jit.jump());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void unpackRepresentation(FieldStore& store)
{
    StubAssembler& jit = store.jit;
    jit.urshift64(store.registers.type, TrustedImm32(representationShift), store.registers.field.representation);
    jit.zeroExtend32ToWord(store.registers.type, store.registers.type);
}

// branchUnlessTypeAccepts() takes three scratch registers and reads only bits 0 to 31 of type, so bits 32 to 63 carry the representation across it.
static void branchUnlessFieldTypeAccepts(FieldStore& store, JumpList& isNotAccepted)
{
    StubAssembler& jit = store.jit;
    const FieldStoreRegisters& r = store.registers;
    jit.zeroExtend32ToWord(r.type, r.type);
    jit.lshift64(TrustedImm32(representationShift), r.field.representation);
    jit.or64(r.field.representation, r.type);
    branchUnlessTypeAccepts(jit, { r.value, r.type, r.field.representation, r.scratch, r.secondScratch }, isNotAccepted, isNotAccepted);
    unpackRepresentation(store);
}

// RuntimeLayoutTable::slotAdmits(): the slot of an object's field that has markers for undefined and null holds them whatever the type of the field says.
static void jumpIfSlotHoldsValueAsMarker(FieldStore& store, Label isAdmitted)
{
    StubAssembler& jit = store.jit;
    const FieldStoreRegisters& r = store.registers;
    Jump isNeitherUndefinedNorNull = jit.branchIfNotOther(r.value, r.scratch);
    // The mark of the cell may answer for the raw value of the field, and Object::store() knows for which fields it does. A cell that has the mark, as every object has until its constructor is through, takes the marker here.
    Jump cellHasNoMark = jit.branchTest8(CCallHelpers::Zero, CCallHelpers::Address(r.field.object, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeInfoPerCellBit));
    LayoutData layoutWithSuchField { };
    layoutWithSuchField.hasFieldThatKeepsStateOfBirth = 1;
    static_assert(sizeof(LayoutData) % sizeof(uint32_t) == 0);
    uint32_t hasFieldThatKeepsStateOfBirth = std::bit_cast<std::array<uint32_t, sizeof(LayoutData) / sizeof(uint32_t)>>(layoutWithSuchField)[0];
    RELEASE_ASSERT(hasOneBitSet(hasFieldThatKeepsStateOfBirth));
    loadStructure(jit, r.field.object, r.scratch);
    jit.loadPtr(Address(r.scratch, CompiledStructure::offsetOfLayout()), r.scratch);
    Jump someFieldTakesNoMarker = jit.branchTest32(CCallHelpers::NonZero, Address(r.scratch), TrustedImm32(static_cast<int32_t>(hasFieldThatKeepsStateOfBirth)));
    jit.move(TrustedImm32(static_cast<int32_t>(representationsWithUndefinedAndNull)), r.scratch);
    jit.urshift32(r.field.representation, r.scratch);
    jit.branchTest32(CCallHelpers::NonZero, r.scratch, TrustedImm32(1)).linkTo(isAdmitted, &jit);
    someFieldTakesNoMarker.link(&jit);
    cellHasNoMark.link(&jit);
    isNeitherUndefinedNorNull.link(&jit);
}

void storeField(StubAssembler& jit, const FieldStoreRegisters& r, FieldStoreEnd end, FieldTypeTest typeTest, JumpList* stored, FieldStoreExits& exits)
{
    RELEASE_ASSERT(noOverlap(r.field.object, r.field.address, r.field.representation, r.value, r.type, r.scratch, r.secondScratch, instanceGPR));
    RELEASE_ASSERT((end == FieldStoreEnd::JoinList) == !!stored);
    FieldStore store { jit, r, end, stored, exits, std::nullopt };
    JumpList typeIsNotAccepted;

    if (typeTest == FieldTypeTest::IsEmitted)
        branchUnlessFieldTypeAccepts(store, typeIsNotAccepted);
    Label isAdmitted = jit.label();
    emitDispatchOnRepresentation(jit, r.field.representation, [&](SlotRepresentation representation) {
        storeWithRepresentation(store, representation);
    });

    switch (end) {
    case FieldStoreEnd::JoinList:
        // Behind a type test this is the store to a variable, which has no other place to go to: its type is all that its slot may hold.
        exits.cannot.append(typeIsNotAccepted);
        return;
    case FieldStoreEnd::ReturnAfterBarrier:
        RELEASE_ASSERT(typeTest == FieldTypeTest::IsEmitted);
        typeIsNotAccepted.link(&jit);
        unpackRepresentation(store);
        jumpIfSlotHoldsValueAsMarker(store, isAdmitted);
        branchOnFieldPresence(jit, r.field, r.scratch, exits.cannot, exits.wasAbsentAndIsNotAccepted);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
