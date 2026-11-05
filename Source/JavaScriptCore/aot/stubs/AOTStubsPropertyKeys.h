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
enum class InstanceOfOperand : uint8_t { Constructor, Prototype, NameSeenByScripts, NameOfGlobalObject, PrototypeThatIsBuiltin };
enum class SlotExit : uint8_t { Miss, Uncached };
enum class SlotFinisher : uint8_t { ReadField, CallGetter, StoreField, CreateField, CallSetterStrict, CallSetterSloppy, GrowStorage };
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
constexpr StubKey getByIdKeepingRegisters() { return StubKey(StubFamily::GetByIdKeepingRegisters); }
constexpr StubKey getByIdCalledByCode() { return exists(getByIdKeepingRegisters()) ? getByIdKeepingRegisters() : getById(); }
constexpr std::optional<StubKey> stubThatKeepsFewerRegisters(StubKey key)
{
    if (key.family() != StubFamily::GetByIdKeepingRegisters)
        return std::nullopt;
    return getById().withCallerFrame(key.callerFrame());
}
constexpr StubKey getByIdAfterFrontProbe() { return StubKey(StubFamily::GetByIdAfterFrontProbe); }
constexpr StubKey getByIdHashed() { return StubKey(StubFamily::GetByIdHashed); }
constexpr StubKey getByIdCheckingColumn() { return StubKey(StubFamily::GetByIdCheckingColumn); }
constexpr StubKey getByIdOfSymbolOfRealm() { return StubKey(StubFamily::GetByIdOfSymbolOfRealm); }
constexpr StubKey getByIdOfSilentSlot() { return StubKey(StubFamily::GetByIdOfSilentSlot); }
constexpr StubKey getByIdOfNarrowCellInExtension() { return StubKey(StubFamily::GetByIdOfNarrowCellInExtension); }
constexpr StubKey getByIdOfEmptySlot() { return StubKey(StubFamily::GetByIdOfEmptySlot); }
constexpr StubKey getByIdOfFieldLeftOut() { return StubKey(StubFamily::GetByIdOfFieldLeftOut); }
constexpr StubKey getByIdOfCell() { return StubKey(StubFamily::GetByIdOfCell); }
constexpr StubKey callMethodById(unsigned count, unsigned lentArguments = 0) { return lentArguments ? StubKey(StubFamily::CallMethodByIdLending).withCount(count).withForm(static_cast<uint8_t>(lentArguments - 1)) : StubKey(StubFamily::CallMethodById).withCount(count); }
constexpr StubKey getByIdAtSite() { return StubKey(StubFamily::GetByIdAtSite); }
constexpr StubKey getByIdOfObjectAtSite() { return StubKey(StubFamily::GetByIdOfObjectAtSite); }
constexpr StubKey getByIdAtSiteHashed() { return StubKey(StubFamily::GetByIdAtSiteHashed); }
constexpr StubKey getLengthAtSite() { return StubKey(StubFamily::GetLengthAtSite); }
constexpr StubKey getSizeOfTable() { return StubKey(StubFamily::GetSizeOfTable); }
enum class ReceiverAtSite : uint8_t { IsAnyValue, IsCell };
enum class WayBehindSite : uint8_t { AfterMiss, WithoutMiss, OfFieldLeftOut };
enum class NameAtSite : uint8_t { IsAnyName, HasColumn };
struct ReadAtSiteForm {
    ReceiverAtSite receiver;
    NameAtSite name;
};
struct ReadBehindSiteForm {
    WayBehindSite way;
    NameAtSite name;
};
constexpr StubKey readAtSite(ReceiverAtSite receiver, NameAtSite name) { return StubKey(StubFamily::ReadAtSite).withForm(static_cast<uint8_t>(receiver) + 2 * static_cast<uint8_t>(name)); }
constexpr ReadAtSiteForm readAtSiteFormOf(StubKey key) { return { static_cast<ReceiverAtSite>(key.form() % 2), static_cast<NameAtSite>(key.form() / 2) }; }
constexpr StubKey readTruthAtSite(ReceiverAtSite receiver, NameAtSite name) { return readAtSite(receiver, name).withFamily(StubFamily::ReadTruthAtSite); }
constexpr StubKey readTruthAtSiteOfNameWithoutField(ReceiverAtSite receiver, NameAtSite name) { return readAtSite(receiver, name).withFamily(StubFamily::ReadTruthAtSiteOfNameWithoutField); }
constexpr StubKey readAtSiteOfNameWithoutField(ReceiverAtSite receiver, NameAtSite name) { return readAtSite(receiver, name).withFamily(StubFamily::ReadAtSiteOfNameWithoutField); }
constexpr StubKey readAtSiteUnlessAsMade(NameAtSite name) { return readAtSite(ReceiverAtSite::IsCell, name).withFamily(StubFamily::ReadAtSiteUnlessAsMade); }
static_assert(descriptorOf(StubFamily::ReadAtSiteOfNameWithoutField).numberOfForms == descriptorOf(StubFamily::ReadAtSite).numberOfForms);
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
constexpr StubKey readLengthAtSite(NameAtSite name) { return StubKey(StubFamily::ReadLengthAtSite).withForm(static_cast<uint8_t>(name)); }
constexpr StubKey readSizeOfTableAtSite(NameAtSite name) { return StubKey(StubFamily::ReadSizeOfTableAtSite).withForm(static_cast<uint8_t>(name)); }
constexpr StubKey callMethodAtSite(unsigned count, unsigned lentArguments = 0) { return lentArguments ? StubKey(StubFamily::CallMethodAtSiteLending).withCount(count).withForm(static_cast<uint8_t>(lentArguments - 1)) : StubKey(StubFamily::CallMethodAtSite).withCount(count); }
constexpr StubKey throughProxy(TrapOfProxy trap) { return StubKey(StubFamily::ThroughProxy).withForm(static_cast<uint8_t>(trap)); }
constexpr StubKey inById(PresenceQuestion question) { return StubKey(StubFamily::InById).withForm(static_cast<uint8_t>(question)); }
constexpr StubKey readFieldInExtension(unsigned granule, SlotInGranule slot) { return StubKey(StubFamily::ReadFieldInExtension).withForm(static_cast<uint8_t>(slot)).withCount(granule); }
constexpr StubKey tryGetSuperById() { return StubKey(StubFamily::TryGetSuperById); }
constexpr StubKey getSuperBaseOfClass() { return StubKey(StubFamily::GetSuperBaseOfClass); }
constexpr StubKey instanceOf(InstanceOfOperand operand) { return StubKey(StubFamily::InstanceOf).withForm(static_cast<uint8_t>(operand)); }

constexpr StubKey privateGet() { return StubKey(StubFamily::PrivateGet); }
constexpr unsigned bitsOfFieldInPrivateGet = 16;
static_assert(descriptorOf(StubFamily::PrivateGet).maxCount == numberOfEnumerators<SlotRepresentation>);
constexpr StubKey privateGetFromSlot(SlotRepresentation representation) { return privateGet().withCount(static_cast<unsigned>(representation) + 1); }
constexpr std::optional<SlotRepresentation> representationKnownTo(StubKey privateGet) { return privateGet.count() ? std::optional(static_cast<SlotRepresentation>(privateGet.count() - 1)) : std::nullopt; }

constexpr StubKey putById(PutMode mode) { return StubKey(StubFamily::PutById).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdAfterFrontProbe(PutMode mode) { return StubKey(StubFamily::PutByIdAfterFrontProbe).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdHashed(PutMode mode) { return StubKey(StubFamily::PutByIdHashed).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdOfSymbolOfRealm(PutMode mode) { return StubKey(StubFamily::PutByIdOfSymbolOfRealm).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdBehindFieldTable(PutMode mode) { return StubKey(StubFamily::PutByIdBehindFieldTable).withForm(static_cast<uint8_t>(mode)); }
enum class NameOfStoreAtSite : uint8_t { NamesField, NamesFieldWithColumn, NamesNoField };
constexpr StubKey putByIdAtSite(PutMode mode, NameOfStoreAtSite name) { return StubKey(StubFamily::PutByIdAtSite).withForm(static_cast<uint8_t>(mode)).withCount(static_cast<unsigned>(name)); }
constexpr StubKey putKnownField(PutMode mode) { return StubKey(StubFamily::PutKnownField).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putKnownFieldThatTakesAnyValue(PutMode mode) { return putKnownField(mode).withCount(1); }
constexpr StubKey putKnownFieldAcceptingAnyNumber(PutMode mode) { return putKnownField(mode).withCount(2); }
constexpr StubKey putKnownFieldKeepingNumbersAsValues(PutMode mode) { return putKnownField(mode).withCount(3); }
constexpr StubKey putByIdOnce(PutMode mode) { return StubKey(StubFamily::PutByIdOnce).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey createKnownField(PutMode mode) { return StubKey(StubFamily::CreateKnownField).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putBooleanOrDoubleField(PutMode mode) { return StubKey(StubFamily::PutBooleanOrDoubleField).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putValueThatSlotRefuses(PutMode mode) { return StubKey(StubFamily::PutValueThatSlotRefuses).withForm(static_cast<uint8_t>(mode)); }

constexpr StubKey didCreateField() { return StubKey(StubFamily::DidCreateField); }
constexpr StubKey didCreateFieldOfSlot() { return StubKey(StubFamily::DidCreateFieldOfSlot); }
enum class PlaceOfCreatedSlot : uint8_t { InCell, OutOfLine };
inline constexpr std::array representationsStoredByPutSlotOrAdd {
    SlotRepresentation::JSValue, SlotRepresentation::Cell32, SlotRepresentation::OptionalBoolean,
    SlotRepresentation::DeferredClosure, SlotRepresentation::OptionalInt32, SlotRepresentation::OptionalInt64, SlotRepresentation::OptionalDouble,
};
constexpr SlotRepresentation representationWithCodeForPutSlotOrAdd(SlotRepresentation representation)
{
    if (representation == SlotRepresentation::Cell)
        return SlotRepresentation::JSValue;
    return representation == SlotRepresentation::OptionalCell32 ? SlotRepresentation::Cell32 : representation;
}
constexpr unsigned countForPutSlotOrAdd(SlotRepresentation representation)
{
    auto found = std::ranges::find(representationsStoredByPutSlotOrAdd, representationWithCodeForPutSlotOrAdd(representation));
    ASSERT_UNDER_CONSTEXPR_CONTEXT(found != representationsStoredByPutSlotOrAdd.end());
    return static_cast<unsigned>(found - representationsStoredByPutSlotOrAdd.begin());
}
constexpr StubKey putSlotOrAdd(PlaceOfCreatedSlot place, SlotRepresentation representation) { return StubKey(StubFamily::PutSlotOrAdd).withForm(static_cast<uint8_t>(place)).withCount(countForPutSlotOrAdd(representation)); }
constexpr StubKey putSlotOrAddAtKeyInRegister(SlotRepresentation representation) { return StubKey(StubFamily::PutSlotOrAddAtKeyInRegister).withCount(countForPutSlotOrAdd(representation)); }
constexpr PlaceOfCreatedSlot placeOfSlotCreatedBy(StubKey putSlotOrAdd) { return static_cast<PlaceOfCreatedSlot>(putSlotOrAdd.form()); }
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

enum class RegistersOfCaller : bool { AreLost, AreKept };
constexpr StubKey getByValCached(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedKeepingRegisters : StubFamily::GetByValCached); }
constexpr StubKey getByValCachedCells(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedCellsKeepingRegisters : StubFamily::GetByValCachedCells); }
constexpr StubKey getByValCachedAtPlace(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedAtPlaceKeepingRegisters : StubFamily::GetByValCachedAtPlace); }
constexpr StubKey getByValCachedBehindPlaces(RegistersOfCaller registers = RegistersOfCaller::AreLost) { return StubKey(registers == RegistersOfCaller::AreKept ? StubFamily::GetByValCachedBehindPlacesKeepingRegisters : StubFamily::GetByValCachedBehindPlaces); }
enum class BaseOfKeyedRead : uint8_t { IsCell, IsAnyValue };
constexpr StubKey getByValOfKeyOfPair(BaseOfKeyedRead base) { return StubKey(StubFamily::GetByValOfKeyOfPair).withForm(static_cast<uint8_t>(base)); }
constexpr StubKey getSuperByVal() { return StubKey(StubFamily::GetSuperByVal); }
constexpr StubKey inByValCached(PresenceQuestion question) { return StubKey(StubFamily::InByValCached).withForm(static_cast<uint8_t>(question)); }
constexpr StubKey hasOwnByValCachedBoxed() { return StubKey(StubFamily::InByValCached).withForm(static_cast<uint8_t>(PresenceQuestion::HasOwn) + 1); }
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

constexpr StubKey callGetterOfAnswer(CallerFrame callerFrame) { return StubKey(StubFamily::CallGetterOfAnswer).withCallerFrame(callerFrame); }
constexpr StubKey getterByAccessTable() { return StubKey(StubFamily::GetterByAccessTable); }
constexpr StubKey getterAtSite(NameAtSite name) { return StubKey(StubFamily::GetterAtSite).withForm(static_cast<uint8_t>(name)); }
static_assert(exists(getterAtSite(NameAtSite::HasColumn)) && getterAtSite(NameAtSite::HasColumn).form() + 1u == descriptorOf(StubFamily::GetterAtSite).numberOfForms);

static_assert(!static_cast<uint8_t>(PutMode::Strict) && static_cast<uint8_t>(PutMode::Sloppy) == 1 && static_cast<uint8_t>(PutMode::Define) == 2);
static_assert([] {
    for (StubKey lastFormKey : {
        getById(), getByIdAfterFrontProbe(), getByIdHashed(), getByIdCheckingColumn(), getByIdOfSymbolOfRealm(), getByIdOfSilentSlot(), getByIdOfEmptySlot(), getByIdOfFieldLeftOut(), getByIdOfNarrowCellInExtension(), getLengthAtSite(), throughProxy(TrapOfProxy::Set), inById(PresenceQuestion::HasOwn),
        readFieldInExtension(0, SlotInGranule::ByteAt7),
        tryGetSuperById(), getSuperBaseOfClass(),
        instanceOf(InstanceOfOperand::PrototypeThatIsBuiltin),
        callMethodById(0),
        readAtSite(ReceiverAtSite::IsCell, NameAtSite::HasColumn), readBehindSite(WayBehindSite::OfFieldLeftOut, NameAtSite::HasColumn), readLengthAtSite(NameAtSite::HasColumn), callMethodAtSite(0),
        getByIdOfCell(), getByIdAtSite(), getByIdOfObjectAtSite(), getByIdAtSiteHashed(), putByIdAtSite(PutMode::Sloppy, NameOfStoreAtSite::NamesField),
        putById(PutMode::Define), putByIdAfterFrontProbe(PutMode::Define), putByIdHashed(PutMode::Define), putByIdOfSymbolOfRealm(PutMode::Define), putByIdBehindFieldTable(PutMode::Define), putKnownField(PutMode::Define), putByIdOnce(PutMode::Define), createKnownField(PutMode::Define), putBooleanOrDoubleField(PutMode::Define), putValueThatSlotRefuses(PutMode::Define),
        didCreateField(), didCreateFieldOfSlot(), putSlotOrAdd(PlaceOfCreatedSlot::OutOfLine, SlotRepresentation::JSValue), putSlotOrAddAtKeyInRegister(SlotRepresentation::JSValue),
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
static_assert(putByIdAtSite(PutMode::Sloppy, NameOfStoreAtSite::NamesNoField).count() == descriptorOf(StubFamily::PutByIdAtSite).maxCount);
static_assert(!exists(getById().withImmediate(1)));
static_assert(exists(didCreateFieldOfSlot().withImmediate(1)) && !exists(didCreateField().withImmediate(1)));
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
