/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "AOTStubDescriptor.h"
#include "AOTStubKey.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT {

enum class SecondReceiver : uint8_t { FirstResult, FirstReceiver, Other };
enum class PresenceQuestion : uint8_t { In, HasOwn };
enum class FreeNameRead : uint8_t { Read, TypeOf };
enum class InstanceOfOperand : uint8_t { Constructor, Prototype };
enum class CreatedFields : uint8_t { One, OneOutOfLine, Run };
enum class CreatedFieldsAtPlace : uint8_t { One, Run };
enum class SlotExit : uint8_t { Miss, Uncached };
enum class SlotFinisher : uint8_t { ReadField, CallGetter, StoreField, CreateField, CallSetterStrict, CallSetterSloppy, GrowStorage };

constexpr StubKey getById() { return StubKey(StubFamily::GetById); }
constexpr StubKey getByIdAfterFrontProbe() { return StubKey(StubFamily::GetByIdAfterFrontProbe); }
constexpr StubKey getByIdHashed() { return StubKey(StubFamily::GetByIdHashed); }
constexpr StubKey getByIdOfSilentSlot() { return StubKey(StubFamily::GetByIdOfSilentSlot); }
constexpr StubKey getByIdOfNarrowCellInExtension() { return StubKey(StubFamily::GetByIdOfNarrowCellInExtension); }
constexpr StubKey getByIdOfEmptySlot() { return StubKey(StubFamily::GetByIdOfEmptySlot); }
constexpr StubKey getByIdBehindFieldTable() { return StubKey(StubFamily::GetByIdBehindFieldTable); }
// For a name that has a column in the front tables: RowDisplacementTable::hasColumn(). So is getLengthAtSite().
constexpr StubKey getByIdAtSite() { return StubKey(StubFamily::GetByIdAtSite); }
// getByIdAtSite() behind its test for a cell.
constexpr StubKey getByIdOfObjectAtSite() { return StubKey(StubFamily::GetByIdOfObjectAtSite); }
constexpr StubKey getByIdAtSiteHashed() { return StubKey(StubFamily::GetByIdAtSiteHashed); }
constexpr StubKey getLengthAtSite() { return StubKey(StubFamily::GetLengthAtSite); }
// The operands: the proxy, the key as its trap takes it, and of a store the value and whether the code is strict.
constexpr StubKey throughProxy(TrapOfProxy trap) { return StubKey(StubFamily::ThroughProxy).withForm(static_cast<uint8_t>(trap)); }
constexpr StubKey inById(PresenceQuestion question) { return StubKey(StubFamily::InById).withForm(static_cast<uint8_t>(question)); }
constexpr StubKey getByIdChain() { return StubKey(StubFamily::GetByIdChain); }
constexpr StubKey getByIdPair(SecondReceiver receiver) { return StubKey(StubFamily::GetByIdPair).withForm(static_cast<uint8_t>(receiver)); }
constexpr StubKey getByIdIsTruthy() { return StubKey(StubFamily::GetByIdIsTruthy); }
constexpr StubKey getByIdEqualsLiteral(unsigned literalLength) { return StubKey(StubFamily::GetByIdEqualsLiteral).withCount(literalLength); }
constexpr StubKey callMethodById(unsigned numberOfArguments) { return StubKey(StubFamily::CallMethodById).withCount(numberOfArguments); }
constexpr StubKey tryGetSuperById() { return StubKey(StubFamily::TryGetSuperById); }
constexpr StubKey getSuperBaseOfClass() { return StubKey(StubFamily::GetSuperBaseOfClass); }
constexpr StubKey getFreeName(FreeNameRead read) { return StubKey(StubFamily::GetFreeName).withForm(static_cast<uint8_t>(read)); }
constexpr StubKey instanceOf(InstanceOfOperand operand) { return StubKey(StubFamily::InstanceOf).withForm(static_cast<uint8_t>(operand)); }

// The operands: the object and the token of the evaluation of the class. The immediate: the index of the field in the record of the layout that declares it.
constexpr StubKey privateGet() { return StubKey(StubFamily::PrivateGet); }

constexpr StubKey storeElementOfCompiledObject(PutMode mode) { return StubKey(StubFamily::StoreElementOfCompiledObject).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putById(PutMode mode) { return StubKey(StubFamily::PutById).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdAfterFrontProbe(PutMode mode) { return StubKey(StubFamily::PutByIdAfterFrontProbe).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdHashed(PutMode mode) { return StubKey(StubFamily::PutByIdHashed).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdBehindFieldTable(PutMode mode) { return StubKey(StubFamily::PutByIdBehindFieldTable).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdAtSite(PutMode mode) { return StubKey(StubFamily::PutByIdAtSite).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByIdAtSiteBehindFieldTable(PutMode mode) { return StubKey(StubFamily::PutByIdAtSiteBehindFieldTable).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putKnownField(PutMode mode) { return StubKey(StubFamily::PutKnownField).withForm(static_cast<uint8_t>(mode)); }
// For a caller that knows that the slot lies in the cell and holds any value as it is, and that the type of the field is that of any value.
constexpr StubKey putKnownFieldThatTakesAnyValue(PutMode mode) { return putKnownField(mode).withCount(1); }
// For a caller that knows the slot is inline and holds a double, and that the field's type accepts any number.
constexpr StubKey putKnownFieldAcceptingAnyNumber(PutMode mode) { return putKnownField(mode).withCount(2); }
constexpr StubKey putByIdOnce(PutMode mode) { return StubKey(StubFamily::PutByIdOnce).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey createKnownField(PutMode mode) { return StubKey(StubFamily::CreateKnownField).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putBooleanOrDoubleField(PutMode mode) { return StubKey(StubFamily::PutBooleanOrDoubleField).withForm(static_cast<uint8_t>(mode)); }
constexpr unsigned numberOfPutModes = 3;
constexpr StubKey putValueThatSlotRefuses(PutMode mode, bool isAtSite) { return StubKey(StubFamily::PutValueThatSlotRefuses).withForm(static_cast<uint8_t>(mode) + (isAtSite ? numberOfPutModes : 0)); }
constexpr StubKey setFreeName(PutMode mode)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(mode != PutMode::Define);
    return StubKey(StubFamily::SetFreeName).withForm(static_cast<uint8_t>(mode));
}

constexpr StubKey didCreateField(CreatedFields fields) { return StubKey(StubFamily::DidCreateField).withForm(static_cast<uint8_t>(fields)); }
constexpr StubKey didCreateFieldAtPlace(CreatedFieldsAtPlace fields) { return StubKey(StubFamily::DidCreateFieldAtPlace).withForm(static_cast<uint8_t>(fields)); }
constexpr StubKey rememberKeyOrderStep() { return StubKey(StubFamily::RememberKeyOrderStep); }

constexpr StubKey getByValCached() { return StubKey(StubFamily::GetByValCached); }
constexpr StubKey getByValCachedCells() { return StubKey(StubFamily::GetByValCachedCells); }
constexpr StubKey getSuperByVal() { return StubKey(StubFamily::GetSuperByVal); }
constexpr StubKey inByValCached(PresenceQuestion question) { return StubKey(StubFamily::InByValCached).withForm(static_cast<uint8_t>(question)); }
constexpr StubKey deleteByIdCached(PutMode mode) { return StubKey(StubFamily::DeleteByIdCached).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey deleteByValCached(PutMode mode) { return StubKey(StubFamily::DeleteByValCached).withForm(static_cast<uint8_t>(mode)); }
constexpr StubKey putByValCached(PutMode mode)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(mode != PutMode::Define);
    return StubKey(StubFamily::PutByValCached).withForm(static_cast<uint8_t>(mode));
}
constexpr StubKey defineByValCached() { return StubKey(StubFamily::DefineByValCached); }
constexpr StubKey toPropertyKey() { return StubKey(StubFamily::ToPropertyKey); }

constexpr unsigned secondNameShift = 16;
constexpr uint32_t pairedNames(uint32_t first, uint32_t second)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(first <= PackedSlot::maxName && second <= PackedSlot::maxName);
    return first | second << secondNameShift;
}

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

static_assert(!static_cast<uint8_t>(PutMode::Strict) && static_cast<uint8_t>(PutMode::Sloppy) == 1 && static_cast<uint8_t>(PutMode::Define) == 2);
static_assert(PackedSlot::maxName == (1u << secondNameShift) - 1);
static_assert([] {
    for (StubKey lastFormKey : {
        getById(), getByIdAfterFrontProbe(), getByIdHashed(), getByIdOfSilentSlot(), getByIdOfEmptySlot(), getByIdOfNarrowCellInExtension(), getByIdBehindFieldTable(), getByIdAtSite(), getByIdOfObjectAtSite(), getByIdAtSiteHashed(), getLengthAtSite(), inById(PresenceQuestion::HasOwn),
        getByIdChain(), getByIdPair(SecondReceiver::Other), getByIdIsTruthy(), getByIdEqualsLiteral(1),
        callMethodById(0), tryGetSuperById(), getSuperBaseOfClass(),
        getFreeName(FreeNameRead::TypeOf), setFreeName(PutMode::Sloppy), instanceOf(InstanceOfOperand::Prototype),
        putById(PutMode::Define), putByIdAfterFrontProbe(PutMode::Define), putByIdHashed(PutMode::Define), putByIdBehindFieldTable(PutMode::Define), putByIdAtSite(PutMode::Define), putKnownField(PutMode::Define), putByIdOnce(PutMode::Define), createKnownField(PutMode::Define), putBooleanOrDoubleField(PutMode::Define), putValueThatSlotRefuses(PutMode::Define, true), putByIdAtSiteBehindFieldTable(PutMode::Define),
        didCreateField(CreatedFields::Run), didCreateFieldAtPlace(CreatedFieldsAtPlace::Run),
        getByValCached(), getByValCachedCells(), getSuperByVal(), inByValCached(PresenceQuestion::HasOwn), deleteByIdCached(PutMode::Sloppy), deleteByValCached(PutMode::Sloppy), putByValCached(PutMode::Sloppy), defineByValCached(), toPropertyKey(),
    }) {
        if (!exists(lastFormKey) || lastFormKey.form() + 1u != descriptorOf(lastFormKey.family()).numberOfForms)
            return false;
    }
    return true;
}());
static_assert(exists(getByIdEqualsLiteral(sizeof(uint64_t))) && !exists(getByIdEqualsLiteral(0)) && exists(callMethodById(numberOfArgumentGPRs)));
static_assert(descriptorOf(StubFamily::ReadSlot).maxCount + 1 == numberOfSlotKindsWithHandlers && descriptorOf(StubFamily::StoreSlot).maxCount + 1 == numberOfSlotKindsWithHandlers);
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
static_assert(descriptorOf(StubFamily::ReadSlot).numberOfForms == 5 && descriptorOf(StubFamily::StoreSlot).numberOfForms == 11 && numberOfSlotHandlerTables == 5 + 2 + 11);
static_assert(descriptorOf(StubFamily::ReadSlotExit).numberOfForms == 8 && descriptorOf(StubFamily::StoreSlotExit).numberOfForms == 16);
static_assert(descriptorOf(StubFamily::SlotCannot).numberOfForms == static_cast<unsigned>(SlotFinisher::GrowStorage) + 1);
static_assert(exists(slotCannot(SlotFinisher::ReadField, SlotOrigin::ByIdInStubFrame)) && exists(getById().withCallerFrame(CallerFrame::StubFrame)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
