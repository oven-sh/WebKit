/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubDescriptor.h"
#include "AOTStubKey.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT {

enum class PresenceQuestion : uint8_t { In, HasOwn };
enum class InstanceOfOperand : uint8_t { Constructor, Prototype };
enum class CreatedFields : uint8_t { One, OneOutOfLine, Run };
enum class SlotExit : uint8_t { Miss, Uncached };
enum class SlotFinisher : uint8_t { ReadField, CallGetter, StoreField, CreateField, CallSetterStrict, CallSetterSloppy, GrowStorage };
// How wide a slot in an Extension is and where in its granule it begins. A slot that reads zero as absent has one, four or eight bytes.
enum class SlotInGranule : uint8_t { Word64, Word32At0, Word32At4, ByteAt0, ByteAt1, ByteAt2, ByteAt3, ByteAt4, ByteAt5, ByteAt6, ByteAt7 };

constexpr unsigned widthOf(SlotInGranule slot) { return slot == SlotInGranule::Word64 ? 8 : slot < SlotInGranule::ByteAt0 ? 4 : 1; }
constexpr unsigned offsetInGranuleOf(SlotInGranule slot)
{
    if (slot >= SlotInGranule::ByteAt0)
        return static_cast<unsigned>(slot) - static_cast<unsigned>(SlotInGranule::ByteAt0);
    return slot == SlotInGranule::Word32At4 ? 4 : 0;
}
constexpr std::optional<SlotInGranule> findSlotInGranule(unsigned offsetInGranule, unsigned width)
{
    for (unsigned form = 0; form <= static_cast<unsigned>(SlotInGranule::ByteAt7); ++form) {
        SlotInGranule slot = static_cast<SlotInGranule>(form);
        if (widthOf(slot) == width && offsetInGranuleOf(slot) == offsetInGranule)
            return slot;
    }
    return std::nullopt;
}

constexpr StubKey getById() { return StubKey(StubFamily::GetById); }
// getById() under a name that most layouts hold in a slot that holds no boxed value.
constexpr StubKey getByIdAskingFieldTableFirst() { return StubKey(StubFamily::GetByIdAskingFieldTableFirst); }
constexpr StubKey getByIdAfterFrontProbe() { return StubKey(StubFamily::GetByIdAfterFrontProbe); }
constexpr StubKey getByIdHashed() { return StubKey(StubFamily::GetByIdHashed); }
// Takes any name. It goes on as getById() where the number has a column in the front tables, else as getByIdHashed().
constexpr StubKey getByIdCheckingColumn() { return StubKey(StubFamily::GetByIdCheckingColumn); }
constexpr StubKey getByIdOfSymbolOfRealm() { return StubKey(StubFamily::GetByIdOfSymbolOfRealm); }
constexpr StubKey getByIdOfSilentSlot() { return StubKey(StubFamily::GetByIdOfSilentSlot); }
constexpr StubKey getByIdOfNarrowCellInExtension() { return StubKey(StubFamily::GetByIdOfNarrowCellInExtension); }
constexpr StubKey getByIdOfEmptySlot() { return StubKey(StubFamily::GetByIdOfEmptySlot); }
// The same of an object that has every field in its slot and something else than Object.prototype above it.
constexpr StubKey getByIdOfFieldLeftOut() { return StubKey(StubFamily::GetByIdOfFieldLeftOut); }
// The receiver is a cell that the table of fields has not answered for, and the name has a column in the front tables. It asks the access table first.
constexpr StubKey getByIdBehindFieldTable() { return StubKey(StubFamily::GetByIdBehindFieldTable); }
// The call convention without a callee, for at most as many arguments as there are registers. The immediate is the number of any name: `this` is asked for it, and what it answers is called.
constexpr StubKey callMethodById(unsigned count) { return StubKey(StubFamily::CallMethodById).withCount(count); }
// The second operand is the address of the Slot of the site. The name has a column in the front tables.
constexpr StubKey getByIdAtSite() { return StubKey(StubFamily::GetByIdAtSite); }
// The same for a receiver that is a cell.
constexpr StubKey getByIdOfObjectAtSite() { return StubKey(StubFamily::GetByIdOfObjectAtSite); }
// Takes any name.
constexpr StubKey getByIdAtSiteHashed() { return StubKey(StubFamily::GetByIdAtSiteHashed); }
// Takes a name that has a column in the front tables: it leaves for getById() where the receiver is neither an array nor a string.
constexpr StubKey getLengthAtSite() { return StubKey(StubFamily::GetLengthAtSite); }
// The same with the operands of getByIdAtSite(), for which it leaves.
constexpr StubKey getLengthWithSite() { return StubKey(StubFamily::GetLengthWithSite); }
enum class ReceiverAtSite : uint8_t { IsAnyValue, IsCell };
enum class WayBehindSite : uint8_t { AfterMiss, WithoutMiss, OfFieldLeftOut };
// Whether the front tables have a column for the name of the place: what goes on behind the place need not ask then.
enum class NameAtSite : uint8_t { IsAnyName, HasColumn };
struct ReadAtSiteForm {
    ReceiverAtSite receiver;
    NameAtSite name;
};
struct ReadBehindSiteForm {
    WayBehindSite way;
    NameAtSite name;
};
// The immediate is the distance from the instance to the SiteAnswers of the place, which are asked first. Behind them the name of the place is read by the stubs that take a name.
constexpr StubKey readAtSite(ReceiverAtSite receiver, NameAtSite name) { return StubKey(StubFamily::ReadAtSite).withForm(static_cast<uint8_t>(receiver) + 2 * static_cast<uint8_t>(name)); }
constexpr ReadAtSiteForm readAtSiteFormOf(StubKey key) { return { static_cast<ReceiverAtSite>(key.form() % 2), static_cast<NameAtSite>(key.form() / 2) }; }
// The forms of a slot in the cell that holds no boxed value and for which a place has an answer: SiteAnswers::bitTurnedForSlotInCell(). Each stands for the representation whose code reads it.
inline constexpr std::array narrowFieldsAtSites { SlotRepresentation::OptionalCell32, SlotRepresentation::OptionalInt32, SlotRepresentation::OptionalDouble, SlotRepresentation::OptionalBoolean, SlotRepresentation::OptionalInt64 };
// readAtSite() under a name that most layouts hold in such a slot: the answer of that form is asked for first.
constexpr std::optional<StubKey> tryReadAtSiteOfNarrowField(SlotRepresentation field, ReceiverAtSite receiver, NameAtSite name)
{
    auto place = std::ranges::find(narrowFieldsAtSites, field);
    if (place == narrowFieldsAtSites.end())
        return std::nullopt;
    return readAtSite(receiver, name).withFamily(StubFamily::ReadAtSiteOfNarrowField).withCount(static_cast<unsigned>(place - narrowFieldsAtSites.begin()));
}
static_assert(narrowFieldsAtSites.size() == descriptorOf(StubFamily::ReadAtSiteOfNarrowField).maxCount + 1 && descriptorOf(StubFamily::ReadAtSiteOfNarrowField).numberOfForms == descriptorOf(StubFamily::ReadAtSite).numberOfForms);
// The operand and the immediate of readAtSite(). AfterMiss: the receiver is a cell for which the place has no answer, which counts toward the next filling of the place.
// OfFieldLeftOut: the receiver is what getByIdOfFieldLeftOut() takes, and the name has a column.
constexpr uint8_t formOfReadBehindSiteOfFieldLeftOut = 4;
constexpr StubKey readBehindSite(WayBehindSite way, NameAtSite name)
{
    if (way == WayBehindSite::OfFieldLeftOut) {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(name == NameAtSite::HasColumn);
        return StubKey(StubFamily::ReadBehindSite).withForm(formOfReadBehindSiteOfFieldLeftOut);
    }
    return StubKey(StubFamily::ReadBehindSite).withForm(static_cast<uint8_t>(way) + 2 * static_cast<uint8_t>(name));
}
constexpr ReadBehindSiteForm readBehindSiteFormOf(StubKey key)
{
    if (key.form() == formOfReadBehindSiteOfFieldLeftOut)
        return { WayBehindSite::OfFieldLeftOut, NameAtSite::HasColumn };
    return { static_cast<WayBehindSite>(key.form() % 2), static_cast<NameAtSite>(key.form() / 2) };
}
// readAtSite() for the name `length`: of an array or a string it is read without a lookup.
constexpr StubKey readLengthAtSite(NameAtSite name) { return StubKey(StubFamily::ReadLengthAtSite).withForm(static_cast<uint8_t>(name)); }
// callMethodById() with the immediate of readAtSite().
constexpr StubKey callMethodAtSite(unsigned count) { return StubKey(StubFamily::CallMethodAtSite).withCount(count); }
// The operands: the proxy, the key as its trap takes it, and of a store the value and whether the code is strict, else the header of the slot that led here or zero.
constexpr StubKey throughProxy(TrapOfProxy trap) { return StubKey(StubFamily::ThroughProxy).withForm(static_cast<uint8_t>(trap)); }
constexpr StubKey inById(PresenceQuestion question) { return StubKey(StubFamily::InById).withForm(static_cast<uint8_t>(question)); }
// The operand is a compiled object that keeps the word of its Extension at offsetOfFirstField. The result is the bytes of the slot with zeros above them, or zero where the object has no Extension or the Extension lacks the granule.
constexpr StubKey readFieldInExtension(unsigned granule, SlotInGranule slot) { return StubKey(StubFamily::ReadFieldInExtension).withForm(static_cast<uint8_t>(slot)).withCount(granule); }
constexpr StubKey tryGetSuperById() { return StubKey(StubFamily::TryGetSuperById); }
constexpr StubKey getSuperBaseOfClass() { return StubKey(StubFamily::GetSuperBaseOfClass); }
constexpr StubKey instanceOf(InstanceOfOperand operand) { return StubKey(StubFamily::InstanceOf).withForm(static_cast<uint8_t>(operand)); }

// The operands: the object and the token of the evaluation of the class. The immediate: the index of the field in the record of the layout that declares it.
constexpr StubKey privateGet() { return StubKey(StubFamily::PrivateGet); }
// For a caller that knows where an object of the declaring layout keeps the field: in the cell, in this representation, in a class that is evaluated once. The upper half of the immediate is the offset of the slot.
constexpr unsigned bitsOfFieldInPrivateGet = 16;
static_assert(descriptorOf(StubFamily::PrivateGet).maxCount == numberOfEnumerators<SlotRepresentation>);
constexpr StubKey privateGetFromSlot(SlotRepresentation representation) { return privateGet().withCount(static_cast<unsigned>(representation) + 1); }
constexpr std::optional<SlotRepresentation> representationKnownTo(StubKey privateGet) { return privateGet.count() ? std::optional(static_cast<SlotRepresentation>(privateGet.count() - 1)) : std::nullopt; }

constexpr StubKey putById(PutMode mode) { return StubKey(StubFamily::PutById).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdAfterFrontProbe(PutMode mode) { return StubKey(StubFamily::PutByIdAfterFrontProbe).withForm(static_cast<uint8_t>(mode)); }
// Takes any name. It goes on as putById() where the number has a column in the front tables.
constexpr StubKey putByIdHashed(PutMode mode) { return StubKey(StubFamily::PutByIdHashed).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdOfSymbolOfRealm(PutMode mode) { return StubKey(StubFamily::PutByIdOfSymbolOfRealm).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdBehindFieldTable(PutMode mode) { return StubKey(StubFamily::PutByIdBehindFieldTable).withForm(static_cast<uint8_t>(mode)); }
// Takes any name. The third operand is the address of the Slot of the site.
constexpr StubKey putByIdAtSite(PutMode mode) { return StubKey(StubFamily::PutByIdAtSite).withForm(static_cast<uint8_t>(mode)); }
// The receiver is a compiled object. The immediate: how far behind the LayoutData of its layout the FieldData of the field lies.
constexpr StubKey putKnownField(PutMode mode) { return StubKey(StubFamily::PutKnownField).withForm(static_cast<uint8_t>(mode)); }
// For a caller that knows that the slot lies in the cell and holds any value as it is, and that the type of the field is that of any value.
constexpr StubKey putKnownFieldThatTakesAnyValue(PutMode mode) { return putKnownField(mode).withCount(1); }
// For a caller that knows that the slot lies in the cell and holds a double, and that the type of the field accepts every number.
constexpr StubKey putKnownFieldAcceptingAnyNumber(PutMode mode) { return putKnownField(mode).withCount(2); }
// The same for a slot that holds any value as it is.
constexpr StubKey putKnownFieldKeepingNumbersAsValues(PutMode mode) { return putKnownField(mode).withCount(3); }
constexpr StubKey putByIdOnce(PutMode mode) { return StubKey(StubFamily::PutByIdOnce).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey createKnownField(PutMode mode) { return StubKey(StubFamily::CreateKnownField).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putBooleanOrDoubleField(PutMode mode) { return StubKey(StubFamily::PutBooleanOrDoubleField).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putValueThatSlotRefuses(PutMode mode) { return StubKey(StubFamily::PutValueThatSlotRefuses).withForm(static_cast<uint8_t>(mode)); }

constexpr StubKey didCreateField(CreatedFields fields) { return StubKey(StubFamily::DidCreateField).withForm(static_cast<uint8_t>(fields)); }
// For a caller that knows the slot key, which is part of the key of the stub: the site moves the object and nothing else.
constexpr StubKey didCreateFieldOfSlot(CreatedFields fields)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(fields != CreatedFields::Run);
    return StubKey(StubFamily::DidCreateFieldOfSlot).withForm(static_cast<uint8_t>(fields));
}
enum class PlaceOfCreatedSlot : uint8_t { InCell, OutOfLine };
// One of each group that is tested and stored by the same code.
inline constexpr std::array representationsStoredByPutSlotOrAdd {
    SlotRepresentation::JSValue, SlotRepresentation::Cell32, SlotRepresentation::LiteralString32, SlotRepresentation::OptionalBoolean,
    SlotRepresentation::DeferredClosure, SlotRepresentation::OptionalInt32, SlotRepresentation::OptionalInt64, SlotRepresentation::OptionalDouble,
};
constexpr SlotRepresentation representationWithCodeForPutSlotOrAdd(SlotRepresentation representation)
{
    if (representation == SlotRepresentation::Cell || representation == SlotRepresentation::LiteralString)
        return SlotRepresentation::JSValue;
    return representation == SlotRepresentation::OptionalCell32 ? SlotRepresentation::Cell32 : representation;
}
// The operands: a compiled object, and the value in the form that compiled code has for what the slot holds, a narrow reference for a slot that defers a closure. The slot can say that the field is absent.
// Stores the value, gives a slot out of line its granule, lists the key where the slot was empty, and runs the barrier of the object whatever the value is.
constexpr unsigned countForPutSlotOrAdd(SlotRepresentation representation)
{
    auto found = std::ranges::find(representationsStoredByPutSlotOrAdd, representationWithCodeForPutSlotOrAdd(representation));
    ASSERT_UNDER_CONSTEXPR_CONTEXT(found != representationsStoredByPutSlotOrAdd.end());
    return static_cast<unsigned>(found - representationsStoredByPutSlotOrAdd.begin());
}
// The key of the slot is the immediate, which is part of the key of the stub.
constexpr StubKey putSlotOrAdd(PlaceOfCreatedSlot place, SlotRepresentation representation) { return StubKey(StubFamily::PutSlotOrAdd).withForm(static_cast<uint8_t>(place)).withCount(countForPutSlotOrAdd(representation)); }
// The key of a slot in the cell is a third operand, for receivers that keep the field at different offsets.
constexpr StubKey putSlotOrAddAtKeyInRegister(SlotRepresentation representation) { return StubKey(StubFamily::PutSlotOrAddAtKeyInRegister).withCount(countForPutSlotOrAdd(representation)); }
constexpr PlaceOfCreatedSlot placeOfSlotCreatedBy(StubKey putSlotOrAdd) { return static_cast<PlaceOfCreatedSlot>(putSlotOrAdd.form()); }
// Of either family.
constexpr SlotRepresentation representationStoredBy(StubKey putSlotOrAdd) { return representationsStoredByPutSlotOrAdd[putSlotOrAdd.count()]; }
static_assert(descriptorOf(StubFamily::PutSlotOrAdd).maxCount + 1 == representationsStoredByPutSlotOrAdd.size() && descriptorOf(StubFamily::PutSlotOrAddAtKeyInRegister).maxCount + 1 == representationsStoredByPutSlotOrAdd.size());
static_assert([] {
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotRepresentation withCode = representationWithCodeForPutSlotOrAdd(representation);
        if (canLeaveSlot(representation) != std::ranges::contains(representationsStoredByPutSlotOrAdd, withCode))
            return false;
        if (widthOf(representation) != widthOf(withCode) || traitsOf(representation).absentBits != traitsOf(withCode).absentBits)
            return false;
    }
    return true;
}());

// AreKept: a second copy of the read by a key, which saves the registers of the caller around whatever it calls, so a loop around such a read keeps its numbers in registers.
enum class RegistersOfCaller : bool { AreLost, AreKept };
constexpr StubKey getByValCached(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedKeepingRegisters : StubFamily::GetByValCached); }
constexpr StubKey getByValCachedCells(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedCellsKeepingRegisters : StubFamily::GetByValCachedCells); }
// The base and the key are cells, the StructureID of the base is loaded, and the KeyedValuePlace of the two is located and is of the key.
constexpr StubKey getByValCachedAtPlace(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedAtPlaceKeepingRegisters : StubFamily::GetByValCachedAtPlace); }
// The base and the key are cells, the StructureID of the base is loaded, and no KeyedValuePlace is of the key.
constexpr StubKey getByValCachedBehindPlaces(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedBehindPlacesKeepingRegisters : StubFamily::GetByValCachedBehindPlaces); }
// The operands: the base and two boxed primitives whose conversion to a string runs no code. The result: what the base has under their texts joined. It keeps the registers of its caller.
enum class BaseOfKeyedRead : uint8_t { IsCell, IsAnyValue };
constexpr StubKey getByValOfKeyOfPair(BaseOfKeyedRead base) { return StubKey(StubFamily::GetByValOfKeyOfPair).withForm(static_cast<uint8_t>(base)); }
// The operands: the object that the search starts at, the key and the receiver.
constexpr StubKey getSuperByVal() { return StubKey(StubFamily::GetSuperByVal); }
constexpr StubKey inByValCached(PresenceQuestion question) { return StubKey(StubFamily::InByValCached).withForm(static_cast<uint8_t>(question)); }
// PresenceQuestion::HasOwn, answered with a boxed boolean: for a call of Object.prototype.hasOwnProperty that is closed.
constexpr StubKey hasOwnByValCachedBoxed() { return StubKey(StubFamily::InByValCached).withForm(static_cast<uint8_t>(PresenceQuestion::HasOwn) + 1); }
// The second operand is the number of the name.
constexpr StubKey deleteByIdCached(PutMode mode)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(mode != PutMode::Define);
    return StubKey(StubFamily::DeleteByIdCached).withForm(static_cast<uint8_t>(mode));
}
constexpr StubKey deleteByValCached(PutMode mode)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(mode != PutMode::Define);
    return StubKey(StubFamily::DeleteByValCached).withForm(static_cast<uint8_t>(mode));
}
constexpr StubKey putByValCached(PutMode mode)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(mode != PutMode::Define);
    return StubKey(StubFamily::PutByValCached).withForm(static_cast<uint8_t>(mode));
}
constexpr StubKey defineByValCached() { return StubKey(StubFamily::DefineByValCached); }
constexpr StubKey toPropertyKey() { return StubKey(StubFamily::ToPropertyKey); }

constexpr bool isReadTable(SlotHandlerTable table) { return traitsOf(table).operation == SlotOperation::Read; }
constexpr CallerFrame callerFrameOf(SlotOrigin origin) { return origin == SlotOrigin::ByIdInStubFrame ? CallerFrame::StubFrame : CallerFrame::HasFrame; }
constexpr SlotOrigin originInCallerWithFrame(SlotOrigin origin) { return origin == SlotOrigin::ByIdInStubFrame ? SlotOrigin::ById : origin; }

constexpr uint8_t computeFormInHandlerFamily(SlotHandlerTable table)
{
    SlotHandlerTableTraits traits = traitsOf(table);
    traits.origin = originInCallerWithFrame(traits.origin);
    uint8_t form = 0;
    for (SlotHandlerTable other : allSlotHandlerTables) {
        SlotHandlerTableTraits otherTraits = traitsOf(other);
        if (otherTraits == traits)
            return form;
        if (isReadTable(other) == isReadTable(table) && otherTraits.origin != SlotOrigin::ByIdInStubFrame)
            ++form;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline constexpr std::array<uint8_t, numberOfSlotHandlerTables> formsInHandlerFamily = [] {
    std::array<uint8_t, numberOfSlotHandlerTables> forms { };
    for (SlotHandlerTable table : allSlotHandlerTables)
        forms[static_cast<unsigned>(table)] = computeFormInHandlerFamily(table);
    return forms;
}();

constexpr uint8_t formInHandlerFamily(SlotHandlerTable table) { return formsInHandlerFamily[static_cast<unsigned>(table)]; }

constexpr StubKey slotHandlerFor(SlotHandlerTable table, Slot::Kind kind)
{
    return StubKey(isReadTable(table) ? StubFamily::ReadSlot : StubFamily::StoreSlot)
        .withForm(formInHandlerFamily(table))
        .withCount(static_cast<unsigned>(kind))
        .withCallerFrame(callerFrameOf(traitsOf(table).origin));
}

constexpr SlotHandlerTable handlerTableOf(StubKey handler)
{
    bool isRead = handler.family() == StubFamily::ReadSlot;
    ASSERT_UNDER_CONSTEXPR_CONTEXT(isRead || handler.family() == StubFamily::StoreSlot);
    for (SlotHandlerTable table : allSlotHandlerTables) {
        if (isReadTable(table) == isRead && formInHandlerFamily(table) == handler.form() && callerFrameOf(traitsOf(table).origin) == handler.callerFrame())
            return table;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr Slot::Kind slotKindOf(StubKey handler) { return static_cast<Slot::Kind>(handler.count()); }

constexpr std::array<StubKey, numberOfSlotKindsWithHandlers> slotHandlerKeys(SlotHandlerTable table)
{
    std::array<StubKey, numberOfSlotKindsWithHandlers> keys { };
    for (Slot::Kind kind : slotKindsWithHandlers)
        keys[static_cast<unsigned>(kind)] = slotHandlerFor(table, kind);
    return keys;
}

constexpr uint8_t formOfSlotExit(SlotOperation operation, SlotOrigin origin, SlotExit exit)
{
    uint8_t pair = 0;
    bool isRead = operation == SlotOperation::Read;
    for (SlotHandlerTable table : allSlotHandlerTables) {
        SlotHandlerTableTraits traits = traitsOf(table);
        if (traits.form != SlotForm::Slot || traits.origin == SlotOrigin::ByIdInStubFrame || (traits.operation == SlotOperation::Read) != isRead)
            continue;
        if (traits.operation == operation && traits.origin == originInCallerWithFrame(origin))
            return static_cast<uint8_t>(2 * pair + static_cast<uint8_t>(exit));
        ++pair;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr StubKey slotExit(SlotOperation operation, SlotOrigin origin, SlotExit exit)
{
    return StubKey(operation == SlotOperation::Read ? StubFamily::ReadSlotExit : StubFamily::StoreSlotExit)
        .withForm(formOfSlotExit(operation, origin, exit))
        .withCallerFrame(callerFrameOf(origin));
}

constexpr StubKey slotExit(SlotHandlerTable table, SlotExit exit)
{
    SlotHandlerTableTraits traits = traitsOf(table);
    return slotExit(traits.operation, traits.origin, exit);
}

constexpr StubKey slotCannot(SlotFinisher finisher, SlotOrigin origin)
{
    return StubKey(StubFamily::SlotCannot).withForm(static_cast<uint8_t>(finisher)).withCallerFrame(callerFrameOf(origin));
}

// A read by name has found the GetterSetter without a slot: AccessTable::answerOfGetterInOwnStorage().
constexpr StubKey callGetterOfAnswer(CallerFrame callerFrame) { return StubKey(StubFamily::CallGetterOfAnswer).withCallerFrame(callerFrame); }
// The function that such an answer leads to, for code that calls it itself. Undefined: the table has no such answer for the object and the name.
constexpr StubKey getterByAccessTable() { return StubKey(StubFamily::GetterByAccessTable); }
// The same out of the answers of the place, with the immediate of readAtSite(). Behind a miss: getterByAccessTable().
constexpr StubKey getterAtSite(NameAtSite name) { return StubKey(StubFamily::GetterAtSite).withForm(static_cast<uint8_t>(name)); }
static_assert(exists(getterAtSite(NameAtSite::HasColumn)) && getterAtSite(NameAtSite::HasColumn).form() + 1u == descriptorOf(StubFamily::GetterAtSite).numberOfForms);

static_assert(!static_cast<uint8_t>(PutMode::Strict) && static_cast<uint8_t>(PutMode::Sloppy) == 1 && static_cast<uint8_t>(PutMode::Define) == 2);
static_assert([] {
    for (StubKey lastFormKey : {
        getById(), getByIdAfterFrontProbe(), getByIdHashed(), getByIdCheckingColumn(), getByIdOfSymbolOfRealm(), getByIdOfSilentSlot(), getByIdOfEmptySlot(), getByIdOfFieldLeftOut(), getByIdOfNarrowCellInExtension(), getLengthAtSite(), throughProxy(TrapOfProxy::Set), inById(PresenceQuestion::HasOwn),
        readFieldInExtension(0, SlotInGranule::ByteAt7),
        tryGetSuperById(), getSuperBaseOfClass(),
        instanceOf(InstanceOfOperand::Prototype),
        callMethodById(0),
        readAtSite(ReceiverAtSite::IsCell, NameAtSite::HasColumn), readBehindSite(WayBehindSite::OfFieldLeftOut, NameAtSite::HasColumn), readLengthAtSite(NameAtSite::HasColumn), callMethodAtSite(0),
        getByIdBehindFieldTable(), getByIdAtSite(), getByIdOfObjectAtSite(), getByIdAtSiteHashed(), getLengthWithSite(), putByIdAtSite(PutMode::Define),
        putById(PutMode::Define), putByIdAfterFrontProbe(PutMode::Define), putByIdHashed(PutMode::Define), putByIdOfSymbolOfRealm(PutMode::Define), putByIdBehindFieldTable(PutMode::Define), putKnownField(PutMode::Define), putByIdOnce(PutMode::Define), createKnownField(PutMode::Define), putBooleanOrDoubleField(PutMode::Define), putValueThatSlotRefuses(PutMode::Define),
        didCreateField(CreatedFields::Run), didCreateFieldOfSlot(CreatedFields::OneOutOfLine), putSlotOrAdd(PlaceOfCreatedSlot::OutOfLine, SlotRepresentation::JSValue), putSlotOrAddAtKeyInRegister(SlotRepresentation::JSValue),
        getByValCached(), getByValCachedCells(), getByValCachedAtPlace(), getByValCachedBehindPlaces(),
        getByValCached(RegistersOfCaller::AreKept), getByValCachedCells(RegistersOfCaller::AreKept), getByValCachedAtPlace(RegistersOfCaller::AreKept), getByValCachedBehindPlaces(RegistersOfCaller::AreKept), getByValOfKeyOfPair(BaseOfKeyedRead::IsAnyValue), getSuperByVal(),
        hasOwnByValCachedBoxed(), deleteByIdCached(PutMode::Sloppy), deleteByValCached(PutMode::Sloppy), putByValCached(PutMode::Sloppy), defineByValCached(), toPropertyKey(),
    }) {
        if (!exists(lastFormKey) || lastFormKey.form() + 1u != descriptorOf(lastFormKey.family()).numberOfForms)
            return false;
    }
    return true;
}());
static_assert(putKnownFieldKeepingNumbersAsValues(PutMode::Define).count() == descriptorOf(StubFamily::PutKnownField).maxCount && !exists(createKnownField(PutMode::Define).withCount(1)));
static_assert(!exists(getById().withImmediate(1)));
static_assert(exists(didCreateFieldOfSlot(CreatedFields::OneOutOfLine).withImmediate(1)) && !exists(didCreateField(CreatedFields::One).withImmediate(1)));
static_assert(exists(putSlotOrAdd(PlaceOfCreatedSlot::OutOfLine, SlotRepresentation::OptionalDouble).withImmediate(1)));
static_assert(descriptorOf(StubFamily::ReadSlot).maxCount + 1 == numberOfSlotKindsWithHandlers && descriptorOf(StubFamily::StoreSlot).maxCount + 1 == numberOfSlotKindsWithHandlers);
static_assert(findSlotInGranule(0, 8) == SlotInGranule::Word64 && findSlotInGranule(4, 4) == SlotInGranule::Word32At4 && findSlotInGranule(7, 1) == SlotInGranule::ByteAt7 && !findSlotInGranule(2, 2) && !findSlotInGranule(4, 8));
static_assert([] {
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (StubKey handler : slotHandlerKeys(table)) {
            if (!exists(handler) || descriptorOf(handler.family()).linkage != StubLinkage::Handler || handlerTableOf(handler) != table)
                return false;
        }
        if (!exists(slotExit(table, SlotExit::Miss)) || !exists(slotExit(table, SlotExit::Uncached)))
            return false;
    }
    return true;
}());
static_assert([] {
    constexpr std::array<ASCIILiteral, numberOfSlotHandlerTables> tableNames {
#define AOT_NAME_SLOT_HANDLER_TABLE(name, operation, origin, form) #name ""_s,
        FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_NAME_SLOT_HANDLER_TABLE)
#undef AOT_NAME_SLOT_HANDLER_TABLE
    };
    auto formNameOf = [](StubKey key) {
        return descriptorOf(key.family()).formNames[key.form()];
    };
    auto endsWith = [](ASCIILiteral name, ASCIILiteral suffix) {
        return name.length() >= suffix.length() && std::ranges::equal(name.span().last(suffix.length()), suffix.span());
    };
    auto isConcatenationOf = [&](ASCIILiteral name, ASCIILiteral prefix, ASCIILiteral suffix) {
        return name.length() == prefix.length() + suffix.length() && std::ranges::equal(name.span().first(prefix.length()), prefix.span()) && endsWith(name, suffix);
    };
    for (SlotHandlerTable table : allSlotHandlerTables) {
        SlotHandlerTableTraits traits = traitsOf(table);
        if (traits.origin == SlotOrigin::ByIdInStubFrame)
            continue;
        ASCIILiteral handlerForm = formNameOf(slotHandlerFor(table, Slot::Kind::Empty));
        if (!endsWith(tableNames[static_cast<unsigned>(table)], handlerForm))
            return false;
        if (traits.form != SlotForm::Slot)
            continue;
        if (!isConcatenationOf(formNameOf(slotExit(table, SlotExit::Miss)), handlerForm, "Miss"_s) || !isConcatenationOf(formNameOf(slotExit(table, SlotExit::Uncached)), handlerForm, "Uncached"_s))
            return false;
    }
    return true;
}());
static_assert(descriptorOf(StubFamily::ReadSlot).numberOfForms == 3 && descriptorOf(StubFamily::StoreSlot).numberOfForms == 9 && numberOfSlotHandlerTables == 3 + 2 + 9);
static_assert(descriptorOf(StubFamily::ReadSlotExit).numberOfForms == 4 && descriptorOf(StubFamily::StoreSlotExit).numberOfForms == 12);
static_assert(descriptorOf(StubFamily::SlotCannot).numberOfForms == static_cast<unsigned>(SlotFinisher::GrowStorage) + 1);
static_assert(exists(slotCannot(SlotFinisher::ReadField, SlotOrigin::ByIdInStubFrame)) && exists(getById().withCallerFrame(CallerFrame::StubFrame)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
