/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "JSCJSValue.h"
#include "JSObject.h"
#include "MarkedBlockCage.h"
#include "PropertyOffset.h"
#include "StructureID.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class LocalAllocator;
template<typename> class RememberedSlot;

namespace AOT {

enum class SlotAccess : uint8_t { Read, Store, Define };
static constexpr unsigned numberOfSlotAccesses = 3;
inline constexpr std::array allSlotAccesses { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define };
static_assert(allSlotAccesses.size() == numberOfSlotAccesses);

constexpr SlotAccess accessFor(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
    case PutMode::Sloppy:
        return SlotAccess::Store;
    case PutMode::Define:
        return SlotAccess::Define;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

enum class SlotPayload : uint8_t { None, Holder, WordIndex, Field, FieldAndHolder, ExtraProperty, ExtraPropertyAddition, Transition, CustomAccessor, Constant };
enum class SlotLocation : uint8_t { None, WordIndex, HiddenWordOffset };
enum class SlotPresence : uint8_t { Unknown, Present, Absent, ReadsSlot };

enum class SlotKindFlag : uint8_t {
    None = 0,
    AbsentIsUndefined = 1 << 3,
    GrowsStorage = 1 << 4,
    InheritsGetter = 1 << 5,
};

#define FOR_EACH_AOT_SLOT_KIND(v) \
    v(Empty, None, None, Unknown, None) \
    v(Value, Holder, WordIndex, Present, None) \
    v(FarValue, WordIndex, None, Present, None) \
    v(Absent, None, None, Absent, None) \
    v(Getter, Holder, WordIndex, Present, None) \
    v(Setter, Holder, WordIndex, Unknown, None) \
    v(CustomGetter, CustomAccessor, None, Unknown, None) \
    v(CustomSetter, CustomAccessor, None, Unknown, None) \
    v(Field, Field, HiddenWordOffset, ReadsSlot, AbsentIsUndefined) \
    v(FieldGetter, Field, HiddenWordOffset, ReadsSlot, AbsentIsUndefined) \
    v(FieldCreation, Field, HiddenWordOffset, Unknown, None) \
    v(FieldOrInherited, FieldAndHolder, WordIndex, Unknown, InheritsGetter) \
    v(FieldOrInheritedSetter, FieldAndHolder, WordIndex, Unknown, None) \
    v(InheritedField, Holder, None, Unknown, None) \
    v(FieldOrInheritedField, FieldAndHolder, HiddenWordOffset, Unknown, None) \
    v(Variable, Field, None, Present, None) \
    v(ExtraProperty, ExtraProperty, HiddenWordOffset, Present, None) \
    v(ExtraGetter, ExtraProperty, HiddenWordOffset, Present, None) \
    v(ExtraSetter, ExtraProperty, HiddenWordOffset, Unknown, None) \
    v(ExtraPropertyAddition, ExtraPropertyAddition, HiddenWordOffset, Unknown, GrowsStorage) \
    v(Transition, Transition, WordIndex, Unknown, GrowsStorage) \
    v(ArrayLength, None, None, Present, None) \
    v(TypedArrayLength, None, None, Present, None) \
    v(StringLength, None, None, Present, None) \
    v(ClosureLength, None, None, Present, None) \
    v(ClosureName, None, None, Present, None) \
    v(Constant, Constant, None, Present, None) \
    v(Uncacheable, None, None, Unknown, None)

struct alignas(16) Slot {
    enum class Kind : uint8_t {
#define AOT_DECLARE_SLOT_KIND(name, payload, location, presence, kindFlag) name,
        FOR_EACH_AOT_SLOT_KIND(AOT_DECLARE_SLOT_KIND)
#undef AOT_DECLARE_SLOT_KIND
    };

    static constexpr uint8_t isOwn = 1 << 0;
    static constexpr uint8_t isOutOfLine = 1 << 1;
    static constexpr uint8_t dependsOnChain = 1 << 2;
    static constexpr uint8_t absentIsUndefined = static_cast<uint8_t>(SlotKindFlag::AbsentIsUndefined);
    static constexpr uint8_t growsStorage = static_cast<uint8_t>(SlotKindFlag::GrowsStorage);
    static constexpr uint8_t inheritsGetter = static_cast<uint8_t>(SlotKindFlag::InheritsGetter);
    static constexpr uint8_t storageHasIndexingHeader = 1 << 6;
    static constexpr uint8_t inheritedIsOutOfLine = 1 << 7;
    // Of FieldCreation, which never has absentIsUndefined: the key order alone says that the field exists, and its slot admits undefined. A store of undefined that finds no granule lists the key and asks for none.
    static constexpr uint8_t undefinedNeedsNoStorage = absentIsUndefined;
    // Of FieldCreation, for a field in the cell whose slot holds any value as it is and whose type is that of any value, so that the type has nothing to say:
    // in its place stands the order word of the object for which the slot was made, as it was before the field was created, and the location is the number of the order that it had afterwards, with one key more.
    // The Structure says how many extra keys there are, and transitions never change: an object of the Structure with that order word gets the same.
    static constexpr uint8_t remembersOrder = growsStorage;
    // Of Field and FieldOrInherited in a slot for reads, which never grow storage: the field may exist where its slot is empty, it is undefined then, and only the key order says whether it does.
    // Of Field: nothing above the receiver has the name, so a read of the empty slot answers undefined either way. Of FieldOrInherited: the inherited value is the answer where it is undefined too.
    static constexpr uint8_t emptySlotMayBeUndefined = growsStorage;
    static constexpr uint8_t allFlags = UINT8_MAX;

    struct FieldPayload {
        SlotData slot;
        uint32_t type;
    };
    // Where the slot of the field is empty, the property of the holder counts: the location and inheritedIsOutOfLine say where it is. The Structure of the receiver has the offset of the extension word.
    // Of FieldOrInheritedField the holder is asked as that of InheritedField is, and the location is the offset of the receiver's extension word.
    struct FieldAndHolderPayload {
        SlotData slot;
        uint32_t holder;
    };
    struct ExtraPropertyPayload {
        StructureID structureID;
        int32_t wordIndex;
    };
    struct ExtraPropertyAdditionPayload {
        StructureID newStructureID;
        int32_t wordIndex;
    };
    struct TransitionPayload {
        StructureID newStructureID;
        uint32_t newStorageCapacity;
    };
    struct CustomAccessorPayload {
        uint32_t index;
        uint32_t unused;
    };
    struct WordIndexPayload {
        int32_t wordIndex;
        uint32_t unused;
    };

    static constexpr SlotPayload payloadOf(Kind);
    static constexpr SlotLocation locationOf(Kind);
    static constexpr SlotPresence presenceOf(Kind);
    static constexpr uint8_t kindFlagOf(Kind);

    static constexpr int32_t wordIndexFor(PropertyOffset offset)
    {
        if (isInlineOffset(offset))
            return static_cast<int32_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue)) + offset;
        return static_cast<int32_t>(offsetInButterfly(offset));
    }
    static constexpr PropertyOffset offsetFor(int32_t wordIndex)
    {
        if (wordIndex >= 0)
            return wordIndex - static_cast<int32_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
        return firstOutOfLineOffset - wordIndex - 2;
    }
    static constexpr bool fitsInLocation(int32_t wordIndex) { return wordIndex >= std::numeric_limits<int16_t>::min() && wordIndex <= std::numeric_limits<int16_t>::max(); }
    static constexpr uint32_t storageCapacityAfterAdding(int32_t wordIndex)
    {
        uint32_t size = static_cast<uint32_t>(-wordIndex - 1);
        return size <= initialOutOfLineCapacity ? initialOutOfLineCapacity : WTF::roundUpToPowerOfTwo(size);
    }

    static Slot create(StructureID structureID, Kind kind, uint8_t flags, uint16_t location)
    {
        Slot slot { };
        slot.structureID = structureID;
        slot.kind = kind;
        slot.flags = flags;
        slot.location = location;
        return slot;
    }
    static Slot createWithoutPayload(StructureID structureID, Kind kind, uint8_t flags)
    {
        ASSERT(payloadOf(kind) == SlotPayload::None);
        return create(structureID, kind, flags, 0);
    }
    static Slot createInStorage(StructureID, Kind, uint8_t flags, JSCell* holderOrNull, PropertyOffset);
    static Slot createField(StructureID structureID, Kind kind, uint8_t flags, SlotData slotData, uint32_t type, uint16_t extensionWordOffset)
    {
        ASSERT(payloadOf(kind) == SlotPayload::Field);
        Slot slot = create(structureID, kind, flags, extensionWordOffset);
        slot.field = { slotData, type };
        return slot;
    }
    static std::optional<Slot> tryCreateFieldAndHolder(StructureID, Kind, uint8_t flags, SlotData, JSCell* holder, PropertyOffset, uintptr_t cageBase);
    // The holder is a compiled object above the receiver with a field of this name. The slot of that field holds the value in any representation, or it is empty and may be filled without a report.
    // So each read asks the holder as for a property of its own. What it answers is a data property or nothing, which does not depend on the receiver.
    static Slot createInheritedField(StructureID structureID, uint8_t flags, JSCell* holder)
    {
        ASSERT(holder && !(flags & (isOwn | isOutOfLine)));
        Slot slot = create(structureID, Kind::InheritedField, flags, 0);
        slot.holder = holder;
        return slot;
    }
    static std::optional<Slot> tryCreateFieldOrInheritedField(StructureID, uint8_t flags, SlotData, JSCell* holder, uint16_t extensionWordOffset, uintptr_t cageBase);
    static Slot createExtraProperty(StructureID structureID, Kind kind, uint8_t flags, StructureID otherStructureID, PropertyOffset offset, uint16_t extraPropertiesWordOffset)
    {
        ASSERT(payloadOf(kind) == SlotPayload::ExtraProperty);
        Slot slot = create(structureID, kind, isInlineOffset(offset) ? flags : flags | isOutOfLine, extraPropertiesWordOffset);
        slot.extraProperty = { otherStructureID, wordIndexFor(offset) };
        return slot;
    }
    static Slot createExtraPropertyAddition(StructureID structureID, uint8_t flags, StructureID newStructureID, PropertyOffset offset, uint16_t extraPropertiesWordOffset)
    {
        Slot slot = create(structureID, Kind::ExtraPropertyAddition, isInlineOffset(offset) ? flags : flags | isOutOfLine, extraPropertiesWordOffset);
        slot.extraPropertyAddition = { newStructureID, wordIndexFor(offset) };
        return slot;
    }
    static std::optional<Slot> tryCreateTransition(StructureID, uint8_t flags, StructureID newStructureID, PropertyOffset, uint32_t newStorageCapacityOrZero);
    static Slot createCustomAccessor(StructureID structureID, Kind kind, uint8_t flags, uint32_t index)
    {
        ASSERT(payloadOf(kind) == SlotPayload::CustomAccessor);
        Slot slot = create(structureID, kind, flags, 0);
        slot.customAccessor = { index, 0 };
        return slot;
    }
    static Slot createConstant(StructureID structureID, uint8_t flags, JSValue value)
    {
        Slot slot = create(structureID, Kind::Constant, flags, 0);
        slot.constant = JSValue::encode(value);
        return slot;
    }

    bool isEmpty() const { return kind == Kind::Empty; }
    bool has(uint8_t flag) const { return flags & flag; }
    int32_t wordIndex() const
    {
        ASSERT(locationOf(kind) == SlotLocation::WordIndex);
        return static_cast<int16_t>(location);
    }
    PropertyOffset offsetInStorage() const { return offsetFor(kind == Kind::FarValue ? farValue.wordIndex : wordIndex()); }
    uint16_t hiddenWordOffset() const
    {
        ASSERT(locationOf(kind) == SlotLocation::HiddenWordOffset);
        return location;
    }
    uint64_t payloadBits() const { return std::bit_cast<std::array<uint64_t, 2>>(*this)[1]; }
    uint64_t headerBits() const { return std::bit_cast<std::array<uint64_t, 2>>(*this)[0]; }

    SlotData fieldSlot() const
    {
        static_assert(OBJECT_OFFSETOF(FieldPayload, slot) == OBJECT_OFFSETOF(FieldAndHolderPayload, slot));
        ASSERT(payloadOf(kind) == SlotPayload::Field || payloadOf(kind) == SlotPayload::FieldAndHolder);
        return field.slot;
    }
    bool namesOnlyLiveCells(const Invocable<bool(JSCell*)> auto& isMarked, uintptr_t cageBase) const;

    static constexpr ptrdiff_t offsetOfStructureID() { return 0; }
    static constexpr ptrdiff_t offsetOfKind() { return 4; }
    static constexpr ptrdiff_t offsetOfFlags() { return 5; }
    static constexpr ptrdiff_t offsetOfLocation() { return 6; }
    static constexpr ptrdiff_t offsetOfPayload() { return 8; }
    static constexpr ptrdiff_t offsetOfSecondPayloadWord() { return 12; }
    static constexpr unsigned kindShiftInHeader = 32;
    static constexpr unsigned flagsShiftInHeader = 40;
    static constexpr unsigned locationShiftInHeader = 48;

    StructureID structureID;
    Kind kind;
    uint8_t flags;
    uint16_t location;
    union {
        JSCell* holder;
        WordIndexPayload farValue;
        FieldPayload field;
        FieldAndHolderPayload fieldAndHolder;
        ExtraPropertyPayload extraProperty;
        ExtraPropertyAdditionPayload extraPropertyAddition;
        TransitionPayload transition;
        CustomAccessorPayload customAccessor;
        EncodedJSValue constant;
    };
};

using SiteCache = RememberedSlot<Slot>*;

inline constexpr std::array allSlotKinds {
#define AOT_LIST_SLOT_KIND(name, payload, location, presence, kindFlag) Slot::Kind::name,
    FOR_EACH_AOT_SLOT_KIND(AOT_LIST_SLOT_KIND)
#undef AOT_LIST_SLOT_KIND
};
inline constexpr unsigned numberOfSlotKinds = allSlotKinds.size();

constexpr SlotPayload Slot::payloadOf(Kind kind)
{
    switch (kind) {
#define AOT_SLOT_KIND_PAYLOAD(name, payload, location, presence, kindFlag) \
    case Kind::name: \
        return SlotPayload::payload;
    FOR_EACH_AOT_SLOT_KIND(AOT_SLOT_KIND_PAYLOAD)
#undef AOT_SLOT_KIND_PAYLOAD
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr SlotLocation Slot::locationOf(Kind kind)
{
    switch (kind) {
#define AOT_SLOT_KIND_LOCATION(name, payload, location, presence, kindFlag) \
    case Kind::name: \
        return SlotLocation::location;
    FOR_EACH_AOT_SLOT_KIND(AOT_SLOT_KIND_LOCATION)
#undef AOT_SLOT_KIND_LOCATION
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr SlotPresence Slot::presenceOf(Kind kind)
{
    switch (kind) {
#define AOT_SLOT_KIND_PRESENCE(name, payload, location, presence, kindFlag) \
    case Kind::name: \
        return SlotPresence::presence;
    FOR_EACH_AOT_SLOT_KIND(AOT_SLOT_KIND_PRESENCE)
#undef AOT_SLOT_KIND_PRESENCE
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr uint8_t Slot::kindFlagOf(Kind kind)
{
    switch (kind) {
#define AOT_SLOT_KIND_FLAG(name, payload, location, presence, kindFlag) \
    case Kind::name: \
        return static_cast<uint8_t>(SlotKindFlag::kindFlag);
    FOR_EACH_AOT_SLOT_KIND(AOT_SLOT_KIND_FLAG)
#undef AOT_SLOT_KIND_FLAG
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline Slot Slot::createInStorage(StructureID structureID, Kind kind, uint8_t flags, JSCell* holderOrNull, PropertyOffset offset)
{
    ASSERT(payloadOf(kind) == SlotPayload::Holder && locationOf(kind) == SlotLocation::WordIndex);
    ASSERT(!!holderOrNull == !(flags & isOwn));
    int32_t index = wordIndexFor(offset);
    if (!isInlineOffset(offset))
        flags |= isOutOfLine;
    if (fitsInLocation(index)) [[likely]] {
        Slot slot = create(structureID, kind, flags, static_cast<uint16_t>(static_cast<int16_t>(index)));
        slot.holder = holderOrNull;
        return slot;
    }
    if (kind != Kind::Value || holderOrNull)
        return createWithoutPayload(structureID, Kind::Uncacheable, flags & dependsOnChain);
    Slot slot = create(structureID, Kind::FarValue, flags, 0);
    slot.farValue = { index, 0 };
    return slot;
}

inline std::optional<Slot> Slot::tryCreateFieldAndHolder(StructureID structureID, Kind kind, uint8_t flags, SlotData slotData, JSCell* holder, PropertyOffset offset, uintptr_t cageBase)
{
    ASSERT(payloadOf(kind) == SlotPayload::FieldAndHolder && locationOf(kind) == SlotLocation::WordIndex);
    int32_t index = wordIndexFor(offset);
    if (!fitsInLocation(index) || MarkedBlockCage::baseOf(holder) != cageBase)
        return std::nullopt;
    if (!isInlineOffset(offset))
        flags |= inheritedIsOutOfLine;
    Slot slot = create(structureID, kind, flags, static_cast<uint16_t>(static_cast<int16_t>(index)));
    slot.fieldAndHolder = { slotData, MarkedBlockCage::encode(cageBase, holder) };
    return slot;
}

inline std::optional<Slot> Slot::tryCreateFieldOrInheritedField(StructureID structureID, uint8_t flags, SlotData slotData, JSCell* holder, uint16_t extensionWordOffset, uintptr_t cageBase)
{
    if (MarkedBlockCage::baseOf(holder) != cageBase)
        return std::nullopt;
    Slot slot = create(structureID, Kind::FieldOrInheritedField, flags, extensionWordOffset);
    slot.fieldAndHolder = { slotData, MarkedBlockCage::encode(cageBase, holder) };
    return slot;
}

inline std::optional<Slot> Slot::tryCreateTransition(StructureID structureID, uint8_t flags, StructureID newStructureID, PropertyOffset offset, uint32_t newStorageCapacityOrZero)
{
    int32_t index = wordIndexFor(offset);
    if (!fitsInLocation(index))
        return std::nullopt;
    if (!isInlineOffset(offset))
        flags |= isOutOfLine;
    ASSERT(newStorageCapacityOrZero || !(flags & storageHasIndexingHeader));
    if (newStorageCapacityOrZero) {
        ASSERT(newStorageCapacityOrZero == storageCapacityAfterAdding(index));
        flags |= growsStorage;
    }
    Slot slot = create(structureID, Kind::Transition, flags | isOwn, static_cast<uint16_t>(static_cast<int16_t>(index)));
    slot.transition = { newStructureID, newStorageCapacityOrZero };
    return slot;
}

bool isStructureIDMarked(StructureID id, const Invocable<bool(JSCell*)> auto& isMarked)
{
    return isMarked(std::bit_cast<JSCell*>(id.decode()));
}

bool Slot::namesOnlyLiveCells(const Invocable<bool(JSCell*)> auto& isMarked, uintptr_t cageBase) const
{
    if (kind == Kind::Empty)
        return true;
    if (structureID && !isStructureIDMarked(structureID, isMarked))
        return false;
    switch (payloadOf(kind)) {
    case SlotPayload::None:
    case SlotPayload::WordIndex:
    case SlotPayload::Field:
    case SlotPayload::CustomAccessor:
        return true;
    case SlotPayload::Holder:
        return !holder || isMarked(holder);
    case SlotPayload::FieldAndHolder:
        return isMarked(MarkedBlockCage::decode(cageBase, fieldAndHolder.holder));
    case SlotPayload::ExtraProperty:
        return !extraProperty.structureID || isStructureIDMarked(extraProperty.structureID, isMarked);
    case SlotPayload::ExtraPropertyAddition:
        return isStructureIDMarked(extraPropertyAddition.newStructureID, isMarked);
    case SlotPayload::Transition:
        return isStructureIDMarked(transition.newStructureID, isMarked);
    case SlotPayload::Constant: {
        JSValue value = JSValue::decode(constant);
        return !value.isCell() || isMarked(value.asCell());
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct PackedSlot {
    static constexpr unsigned kindBits = 5;
    static constexpr uint8_t kindMask = (1 << kindBits) - 1;
    static constexpr uint8_t isOwn = 1 << kindBits;
    static constexpr uint8_t isOutOfLine = 2 << kindBits;
    static constexpr uint8_t kindFlag = 4 << kindBits;
    static constexpr uint8_t ownInlineValueBits = static_cast<uint8_t>(Slot::Kind::Value) | isOwn;
    static constexpr unsigned hiddenWordOffsetShift = 2;
    static constexpr uint16_t noName = 0;
    static constexpr uint32_t maxName = UINT16_MAX;

    static std::optional<PackedSlot> tryPack(const Slot&, uint16_t name, uintptr_t cageBase);
    Slot unpack(StructureID, uintptr_t cageBase, bool dependsOnChain) const;

    bool isEmpty() const { return name == noName; }
    uint8_t unbiasedKindAndFlags() const { return biasedKindAndFlags ^ ownInlineValueBits; }
    Slot::Kind kind() const { return static_cast<Slot::Kind>(unbiasedKindAndFlags() & kindMask); }
    bool canOutliveWhatItNames() const;
    static constexpr uint32_t upperHalfForOwnInlineValue(uint16_t name) { return static_cast<uint32_t>(name) << 16; }

    static constexpr ptrdiff_t offsetOfPayload() { return 0; }
    static constexpr ptrdiff_t offsetOfLocation() { return 4; }
    static constexpr ptrdiff_t offsetOfBiasedKindAndFlags() { return 5; }
    static constexpr ptrdiff_t offsetOfName() { return 6; }

    uint32_t payload;
    uint8_t location;
    uint8_t biasedKindAndFlags;
    uint16_t name;
};

inline std::optional<PackedSlot> PackedSlot::tryPack(const Slot& slot, uint16_t name, uintptr_t cageBase)
{
    ASSERT(name != noName);
    uint8_t carriedFlag = Slot::kindFlagOf(slot.kind);
    if (slot.flags & ~(Slot::isOwn | Slot::isOutOfLine | Slot::dependsOnChain | carriedFlag))
        return std::nullopt;
    Slot::Kind kind = slot.kind;
    uint32_t payload = 0;
    uint8_t location = 0;
    auto packWordIndex = [&](int32_t wordIndex) {
        if (wordIndex < std::numeric_limits<int8_t>::min() || wordIndex > std::numeric_limits<int8_t>::max())
            return false;
        location = static_cast<uint8_t>(static_cast<int8_t>(wordIndex));
        return true;
    };
    auto packHiddenWordOffset = [&] {
        uint32_t offset = slot.hiddenWordOffset();
        if (offset & ((1 << hiddenWordOffsetShift) - 1) || offset >> hiddenWordOffsetShift > UINT8_MAX)
            return false;
        location = static_cast<uint8_t>(offset >> hiddenWordOffsetShift);
        return true;
    };
    switch (Slot::payloadOf(kind)) {
    case SlotPayload::None:
        break;
    case SlotPayload::Holder:
        if (!slot.holder)
            payload = static_cast<uint32_t>(slot.wordIndex());
        else {
            if (MarkedBlockCage::baseOf(slot.holder) != cageBase || (Slot::locationOf(kind) == SlotLocation::WordIndex && !packWordIndex(slot.wordIndex())))
                return std::nullopt;
            payload = MarkedBlockCage::encode(cageBase, slot.holder);
        }
        break;
    case SlotPayload::WordIndex:
        kind = Slot::Kind::Value;
        payload = static_cast<uint32_t>(slot.farValue.wordIndex);
        break;
    case SlotPayload::Field:
        if (slot.field.type || (Slot::locationOf(kind) == SlotLocation::HiddenWordOffset && !packHiddenWordOffset()))
            return std::nullopt;
        payload = slot.field.slot.encoded();
        break;
    case SlotPayload::ExtraProperty:
        if (slot.extraProperty.structureID || !packHiddenWordOffset())
            return std::nullopt;
        payload = static_cast<uint32_t>(slot.extraProperty.wordIndex);
        break;
    case SlotPayload::Transition:
        if (!packWordIndex(slot.wordIndex()))
            return std::nullopt;
        payload = slot.transition.newStructureID.bits();
        break;
    case SlotPayload::CustomAccessor:
        payload = slot.customAccessor.index;
        break;
    case SlotPayload::FieldAndHolder:
    case SlotPayload::ExtraPropertyAddition:
    case SlotPayload::Constant:
        return std::nullopt;
    }
    uint8_t kindAndFlags = static_cast<uint8_t>(kind);
    if (slot.flags & Slot::isOwn)
        kindAndFlags |= isOwn;
    if (slot.flags & Slot::isOutOfLine)
        kindAndFlags |= isOutOfLine;
    if (carriedFlag && slot.flags & carriedFlag)
        kindAndFlags |= kindFlag;
    return PackedSlot { payload, location, static_cast<uint8_t>(kindAndFlags ^ ownInlineValueBits), name };
}

inline bool PackedSlot::canOutliveWhatItNames() const
{
    switch (Slot::payloadOf(kind())) {
    case SlotPayload::Holder:
        return !(unbiasedKindAndFlags() & isOwn);
    case SlotPayload::Transition:
    case SlotPayload::CustomAccessor:
        return true;
    case SlotPayload::None:
    case SlotPayload::WordIndex:
    case SlotPayload::Field:
    case SlotPayload::ExtraProperty:
        return false;
    case SlotPayload::FieldAndHolder:
    case SlotPayload::ExtraPropertyAddition:
    case SlotPayload::Constant:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline Slot PackedSlot::unpack(StructureID structureID, uintptr_t cageBase, bool dependsOnChain) const
{
    if (isEmpty())
        return Slot { };
    uint8_t kindAndFlags = unbiasedKindAndFlags();
    Slot::Kind kind = this->kind();
    uint8_t flags = dependsOnChain ? Slot::dependsOnChain : 0;
    if (kindAndFlags & isOwn)
        flags |= Slot::isOwn;
    if (kindAndFlags & isOutOfLine)
        flags |= Slot::isOutOfLine;
    if (kindAndFlags & kindFlag)
        flags |= Slot::kindFlagOf(kind);
    int32_t shortWordIndex = static_cast<int8_t>(location);
    uint16_t hiddenWordOffset = static_cast<uint16_t>(location << hiddenWordOffsetShift);
    switch (Slot::payloadOf(kind)) {
    case SlotPayload::None:
        return Slot::create(structureID, kind, flags, 0);
    case SlotPayload::Holder: {
        if (kind == Slot::Kind::InheritedField)
            return Slot::createInheritedField(structureID, flags, MarkedBlockCage::decode(cageBase, payload));
        if (flags & Slot::isOwn)
            return Slot::createInStorage(structureID, kind, flags & ~Slot::isOutOfLine, nullptr, Slot::offsetFor(static_cast<int32_t>(payload)));
        return Slot::createInStorage(structureID, kind, flags & ~Slot::isOutOfLine, MarkedBlockCage::decode(cageBase, payload), Slot::offsetFor(shortWordIndex));
    }
    case SlotPayload::Field:
        return Slot::createField(structureID, kind, flags, SlotData::decode(payload), 0, hiddenWordOffset);
    case SlotPayload::ExtraProperty:
        return Slot::createExtraProperty(structureID, kind, flags & ~Slot::isOutOfLine, StructureID(), Slot::offsetFor(static_cast<int32_t>(payload)), hiddenWordOffset);
    case SlotPayload::Transition:
        return *Slot::tryCreateTransition(structureID, flags & ~(Slot::isOutOfLine | Slot::growsStorage), std::bit_cast<StructureID>(payload), Slot::offsetFor(shortWordIndex), flags & Slot::growsStorage ? Slot::storageCapacityAfterAdding(shortWordIndex) : 0);
    case SlotPayload::CustomAccessor:
        return Slot::createCustomAccessor(structureID, kind, flags, payload);
    case SlotPayload::WordIndex:
    case SlotPayload::FieldAndHolder:
    case SlotPayload::ExtraPropertyAddition:
    case SlotPayload::Constant:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct SiteCacheState {
    static constexpr unsigned maxRefillCount = 3;
    static constexpr unsigned slotAddressShift = 3;
    // Sites share the states by address: what a state says only decides when a filled site asks to be filled again.
    static constexpr unsigned numberOfStates = 4096;
    static constexpr uint32_t offsetMask = (numberOfStates - 1) * 2;
    // The misses of all empty sites with lookup tables behind them are counted together: the counter goes up by this step, and the miss that wraps it is a sample. A site is filled at its second sample.
    static constexpr unsigned missesOfEmptySitesPerSample = 32;
    static constexpr unsigned sampleStep = (UINT8_MAX + 1) / missesOfEmptySitesPerSample;
    static constexpr uint8_t missesBeforeRefill(unsigned refillCount) { return static_cast<uint8_t>((4u << (2 * std::min(refillCount, maxRefillCount))) - 1); }

    static constexpr ptrdiff_t offsetOfMissesLeft() { return 1; }

    uint8_t refillCount;
    uint8_t missesLeft;
};

struct CustomAccessorRecord {
    void* function;
    JSObject* slotBase;
    JSGlobalObject* globalObject;
    uint32_t offsetOfCachedValue;
    uint32_t unused;
};

static_assert(static_cast<unsigned>(SlotAccess::Define) + 1 == numberOfSlotAccesses);

static_assert(sizeof(Slot) == 16 && alignof(Slot) == 16);
static_assert(OBJECT_OFFSETOF(Slot, structureID) == Slot::offsetOfStructureID());
static_assert(OBJECT_OFFSETOF(Slot, kind) == Slot::offsetOfKind());
static_assert(OBJECT_OFFSETOF(Slot, flags) == Slot::offsetOfFlags());
static_assert(OBJECT_OFFSETOF(Slot, location) == Slot::offsetOfLocation());
static_assert(OBJECT_OFFSETOF(Slot, holder) == Slot::offsetOfPayload());
static_assert(OBJECT_OFFSETOF(Slot, field.type) == Slot::offsetOfSecondPayloadWord());
static_assert(Slot::kindShiftInHeader == 8 * Slot::offsetOfKind());
static_assert(Slot::flagsShiftInHeader == 8 * Slot::offsetOfFlags());
static_assert(Slot::locationShiftInHeader == 8 * Slot::offsetOfLocation());
static_assert((Slot::isOwn | Slot::isOutOfLine | Slot::dependsOnChain | Slot::absentIsUndefined | Slot::growsStorage | Slot::inheritsGetter | Slot::storageHasIndexingHeader | Slot::inheritedIsOutOfLine) == Slot::allFlags);
static_assert(!static_cast<unsigned>(Slot::Kind::Empty));
// Whoever asks whether a property is there reads the slot of either in the same way, and tells the two from all other kinds by one comparison.
static_assert((static_cast<unsigned>(Slot::Kind::Field) | 1) == static_cast<unsigned>(Slot::Kind::FieldGetter) && !(static_cast<unsigned>(Slot::Kind::Field) & 1));
static_assert(numberOfSlotKinds <= 1u << PackedSlot::kindBits);
static_assert(std::is_aggregate_v<Slot> && std::is_trivially_copyable_v<Slot>);
static_assert(Slot::offsetFor(Slot::wordIndexFor(0)) == 0);
static_assert(Slot::offsetFor(Slot::wordIndexFor(firstOutOfLineOffset)) == firstOutOfLineOffset);
static_assert(Slot::wordIndexFor(firstOutOfLineOffset) == -2);
static_assert(Slot::storageCapacityAfterAdding(Slot::wordIndexFor(firstOutOfLineOffset)) == initialOutOfLineCapacity);
static_assert(Slot::storageCapacityAfterAdding(Slot::wordIndexFor(firstOutOfLineOffset + 4)) == 8);

static_assert(sizeof(PackedSlot) == 8 && std::is_trivially_copyable_v<PackedSlot>);
static_assert(OBJECT_OFFSETOF(PackedSlot, payload) == PackedSlot::offsetOfPayload());
static_assert(OBJECT_OFFSETOF(PackedSlot, location) == PackedSlot::offsetOfLocation());
static_assert(OBJECT_OFFSETOF(PackedSlot, biasedKindAndFlags) == PackedSlot::offsetOfBiasedKindAndFlags());
static_assert(OBJECT_OFFSETOF(PackedSlot, name) == PackedSlot::offsetOfName());

static_assert(sizeof(SiteCacheState) == 2);
static_assert(OBJECT_OFFSETOF(SiteCacheState, missesLeft) == SiteCacheState::offsetOfMissesLeft());
static_assert(sizeof(Slot) >> SiteCacheState::slotAddressShift == sizeof(SiteCacheState));
static_assert(SiteCacheState::missesBeforeRefill(SiteCacheState::maxRefillCount) == 255);

static_assert(sizeof(CustomAccessorRecord) == 32);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
