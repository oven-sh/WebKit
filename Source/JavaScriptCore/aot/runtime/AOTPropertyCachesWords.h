/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeRecords.h"
#include <array>
#include <optional>
#include <type_traits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

struct alignas(32) HashedSlot {
    static constexpr uintptr_t accessMask = 3;
    static constexpr unsigned ways = 4;
    static constexpr unsigned sizeShift = 5;
    static constexpr uint32_t structureMultiplier = 0x9e3779b1;
    static constexpr unsigned structureHashShift = 16 - sizeShift;
    static constexpr unsigned uidAlignmentShift = 4;
    static constexpr unsigned uidHashShift = sizeShift - uidAlignmentShift;

    static uintptr_t keyFor(const UniquedStringImpl* uid, SlotAccess access) { return std::bit_cast<uintptr_t>(uid) | static_cast<uintptr_t>(access); }
    static constexpr uint64_t offsetMaskFor(uint32_t numberOfEntries) { return static_cast<uint64_t>(numberOfEntries - 1) << sizeShift; }
    // The size of the table comes in with the mask alone, as the last step. HashedSlotTable::grow relies on it: in a table of twice the size an offset is what it was, or that plus the size the table had.
    static uint32_t offsetOfFirstWay(StructureID structureID, const UniquedStringImpl* uid, uint64_t offsetMask)
    {
        uint32_t structureHash = (structureID.bits() * structureMultiplier) >> structureHashShift;
        uint32_t uidHash = static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid)) << uidHashShift;
        return static_cast<uint32_t>((structureHash + uidHash) & offsetMask);
    }
    static constexpr uint32_t offsetOfWay(uint32_t offsetOfFirstWay, unsigned way) { return offsetOfFirstWay ^ (way << sizeShift); }

    bool isEmpty() const { return !key; }
    bool matches(StructureID structureID, uintptr_t expectedKey) const { return key == expectedKey && slot.structureID == structureID; }
    UniquedStringImpl* uid() const { return std::bit_cast<UniquedStringImpl*>(key & ~accessMask); }
    SlotAccess access() const { return static_cast<SlotAccess>(key & accessMask); }

    static constexpr ptrdiff_t offsetOfSlot() { return 0; }
    static constexpr ptrdiff_t offsetOfKey() { return 16; }

    Slot slot;
    uintptr_t key;
};

enum class SlotForm : uint8_t { Slot, PackedSlot };
enum class SlotOrigin : uint8_t { ById, ByIdInStubFrame, ByVal, FreeName, FreeNameForTypeOf };
enum class SlotOperation : uint8_t { Read, StoreStrict, StoreSloppy, Define };

constexpr SlotAccess accessOf(SlotOperation operation)
{
    switch (operation) {
    case SlotOperation::Read:
        return SlotAccess::Read;
    case SlotOperation::StoreStrict:
    case SlotOperation::StoreSloppy:
        return SlotAccess::Store;
    case SlotOperation::Define:
        return SlotAccess::Define;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr SlotOperation operationFor(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return SlotOperation::StoreStrict;
    case PutMode::Sloppy:
        return SlotOperation::StoreSloppy;
    case PutMode::Define:
        return SlotOperation::Define;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

#define FOR_EACH_AOT_SLOT_HANDLER_TABLE(v) \
    v(ReadById, Read, ById, Slot) \
    v(ReadByIdPacked, Read, ById, PackedSlot) \
    v(ReadByIdInStubFrame, Read, ByIdInStubFrame, Slot) \
    v(ReadByIdPackedInStubFrame, Read, ByIdInStubFrame, PackedSlot) \
    v(ReadByVal, Read, ByVal, Slot) \
    v(ReadFreeName, Read, FreeName, Slot) \
    v(ReadFreeNameForTypeOf, Read, FreeNameForTypeOf, Slot) \
    v(StoreStrictById, StoreStrict, ById, Slot) \
    v(StoreStrictByIdPacked, StoreStrict, ById, PackedSlot) \
    v(StoreStrictByVal, StoreStrict, ByVal, Slot) \
    v(StoreStrictFreeName, StoreStrict, FreeName, Slot) \
    v(StoreSloppyById, StoreSloppy, ById, Slot) \
    v(StoreSloppyByIdPacked, StoreSloppy, ById, PackedSlot) \
    v(StoreSloppyByVal, StoreSloppy, ByVal, Slot) \
    v(StoreSloppyFreeName, StoreSloppy, FreeName, Slot) \
    v(DefineById, Define, ById, Slot) \
    v(DefineByIdPacked, Define, ById, PackedSlot) \
    v(DefineByVal, Define, ByVal, Slot)

enum class SlotHandlerTable : uint8_t {
#define AOT_DECLARE_SLOT_HANDLER_TABLE(name, operation, origin, form) name,
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_DECLARE_SLOT_HANDLER_TABLE)
#undef AOT_DECLARE_SLOT_HANDLER_TABLE
};

struct SlotHandlerTableTraits {
    SlotOperation operation;
    SlotOrigin origin;
    SlotForm form;

    friend constexpr bool operator==(SlotHandlerTableTraits, SlotHandlerTableTraits) = default;
};

inline constexpr std::array allSlotHandlerTables {
#define AOT_LIST_SLOT_HANDLER_TABLE(name, operation, origin, form) SlotHandlerTable::name,
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_LIST_SLOT_HANDLER_TABLE)
#undef AOT_LIST_SLOT_HANDLER_TABLE
};
inline constexpr unsigned numberOfSlotHandlerTables = allSlotHandlerTables.size();

constexpr SlotHandlerTableTraits traitsOf(SlotHandlerTable table)
{
    switch (table) {
#define AOT_SLOT_HANDLER_TABLE_TRAITS(name, operation, origin, form) \
    case SlotHandlerTable::name: \
        return { SlotOperation::operation, SlotOrigin::origin, SlotForm::form };
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_SLOT_HANDLER_TABLE_TRAITS)
#undef AOT_SLOT_HANDLER_TABLE_TRAITS
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr std::optional<SlotHandlerTable> tryFindSlotHandlerTable(SlotHandlerTableTraits traits)
{
    for (SlotHandlerTable table : allSlotHandlerTables) {
        if (traitsOf(table) == traits)
            return table;
    }
    return std::nullopt;
}

constexpr SlotHandlerTable slotHandlerTable(SlotOperation operation, SlotOrigin origin, SlotForm form)
{
    std::optional<SlotHandlerTable> table = tryFindSlotHandlerTable({ operation, origin, form });
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(table);
    return *table;
}

struct StructureRowWords {
    static constexpr unsigned granuleShift = 6;
    static constexpr unsigned wordShift = 2;
};

// The names that an object of one Structure, or an object above it, may have: a bit for each, found by the number of the name.
struct AbsenceFilter {
    static constexpr unsigned numberOfFilters = 256;
    static constexpr unsigned sizeShift = 6;
    static constexpr unsigned numberOfWords = 4;
    static constexpr unsigned bitsOfWordShift = 6;

    static constexpr unsigned indexFor(uint32_t structureIDBits) { return (structureIDBits >> StructureRowWords::granuleShift) & (numberOfFilters - 1); }
    void add(uint32_t identifierNumber) { names[(identifierNumber >> bitsOfWordShift) & (numberOfWords - 1)] |= uint64_t { 1 } << (identifierNumber & 63); }
    bool rulesOut(uint32_t identifierNumber) const { return !(names[(identifierNumber >> bitsOfWordShift) & (numberOfWords - 1)] >> (identifierNumber & 63) & 1); }
    static constexpr unsigned numberOfWordsForStores = 2;
    void addNameThatStopsStores(uint32_t identifierNumber) { namesThatStopStores[(identifierNumber >> bitsOfWordShift) & (numberOfWordsForStores - 1)] |= uint64_t { 1 } << (identifierNumber & 63); }
    bool letsStoresPass(uint32_t identifierNumber) const { return !(namesThatStopStores[(identifierNumber >> bitsOfWordShift) & (numberOfWordsForStores - 1)] >> (identifierNumber & 63) & 1); }

    uint32_t structureID;
    uint32_t generation;
    uint64_t names[numberOfWords];
    // A read asks the tables first, and what the filter answers is never filed. So one read in readsBetweenMisses leaves as if the filter had said nothing: a pair that comes again and again gets its slot.
    static constexpr uint32_t readsBetweenMisses = 64;
    uint32_t readsAnswered;
    // A name that an object on some chain gets, loses or has changed is noted by itself, in PropertyCacheWords::namesChangedOnChains. Only a change that is reported without a name starts a new generation of these.
    uint32_t generationForStores;
    // Of the names, those under which an object above may have anything but a data property that can be written. A store to the object passes every other name and creates the object's own property.
    uint64_t namesThatStopStores[numberOfWordsForStores];
};

// The name that a short text is, for a key that is no atom: a piece of a longer string, or a string made a moment ago.
struct NameOfText {
    // Two entries side by side take the texts of one hash, the one that came last in front.
    static constexpr unsigned numberOfPairs = 64;
    static constexpr unsigned numberOfEntries = 2 * numberOfPairs;
    static constexpr unsigned sizeShift = 5;
    static constexpr uint32_t minLength = 2;
    static constexpr uint32_t maxLength = 16;
    static constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
    static constexpr unsigned pairShift = 64 - 6;

    static constexpr unsigned widthOfWordFor(uint32_t length) { return length >= 8 ? 8 : length >= 4 ? 4 : 2; }
    static constexpr unsigned firstIndexFor(uint64_t first, uint64_t last, uint32_t length) { return static_cast<unsigned>((first + 2 * last + length) * multiplier >> pairShift) * 2; }

    uint64_t first;
    uint64_t last;
    uint64_t length;
    UniquedStringImpl* uid;
};

struct MissesOfPairs {
    static constexpr uint32_t numberOfCounters = 1u << 15;
    static constexpr uint32_t missesBetweenFilings = 16;
};

struct FrontWords {
    const uint32_t* rowsForStructureIDZero;
    const PackedSlot* plane;
};

struct PropertyCacheWords {
    FrontWords front[numberOfSlotAccesses];
    const HashedSlot* hashedSlots;
    uint64_t hashedSlotOffsetMask;
    AbsenceFilter* absenceFilters;
    uint32_t absenceFilterGeneration;
    uint32_t absenceFilterGenerationForStores;
    // A bit for the number of every name that the Object.prototype of some realm of the VM has or has had.
    const uint64_t* namesOfObjectPrototypes;
    // A bit for the number of every name under which an object that is a prototype has got anything but a data property that can be written.
    const uint64_t* namesChangedOnChains;
    // As AbsenceFilter::readsAnswered, for the reads that a Structure's flag answers.
    uint32_t absentReadsAnswered;
    // How often a read of a Structure and a name has found them in no table, by a hash of the two.
    uint8_t* missesOfPairs;
    const NameOfText* namesOfTexts;
    uint8_t missIsDueForFiling;
    // What the Structure of a stand-in for the prototype object of a class has as its ClassInfo.
    const void* infoOfClassPrototypes;
    SiteCacheState* siteCacheStates;
    uint8_t missesOfEmptySites;
    uint8_t storesAtEmptySites;
    uint8_t storesToGuardedFields;
    const CustomAccessorRecord* customAccessors;
    uint32_t numberStructureID;
    uint32_t booleanStructureID;
    UniquedStringImpl* hasInstanceUID;
    UniquedStringImpl* prototypeUID;
    JSCell* defaultHasInstanceFunction;
    void* handlers[numberOfSlotHandlerTables][numberOfSlotKinds];

    static constexpr ptrdiff_t offsetOfFront(SlotAccess access) { return OBJECT_OFFSETOF(PropertyCacheWords, front) + static_cast<ptrdiff_t>(static_cast<unsigned>(access) * sizeof(FrontWords)); }
    static constexpr ptrdiff_t offsetOfHandlers(SlotHandlerTable table) { return OBJECT_OFFSETOF(PropertyCacheWords, handlers) + static_cast<ptrdiff_t>(static_cast<unsigned>(table) * numberOfSlotKinds * sizeof(void*)); }
};

static_assert(sizeof(HashedSlot) == 1u << HashedSlot::sizeShift && alignof(HashedSlot) == sizeof(HashedSlot));
static_assert(OBJECT_OFFSETOF(HashedSlot, slot) == HashedSlot::offsetOfSlot() && OBJECT_OFFSETOF(HashedSlot, key) == HashedSlot::offsetOfKey());
static_assert(std::is_trivially_copyable_v<HashedSlot> && std::is_aggregate_v<HashedSlot>);
static_assert(numberOfSlotAccesses <= HashedSlot::accessMask + 1 && alignof(UniquedStringImpl) > HashedSlot::accessMask);
static_assert(hasOneBitSet(HashedSlot::ways));
static_assert(sizeof(FrontWords) == 2 * sizeof(void*) && OBJECT_OFFSETOF(FrontWords, plane) == sizeof(void*));
static_assert(1u << StructureRowWords::wordShift == sizeof(uint32_t));
static_assert(sizeof(AbsenceFilter) == 1u << AbsenceFilter::sizeShift && hasOneBitSet(AbsenceFilter::numberOfFilters) && hasOneBitSet(AbsenceFilter::numberOfWords));
static_assert(std::is_trivially_copyable_v<AbsenceFilter> && std::is_aggregate_v<AbsenceFilter>);
static_assert(sizeof(NameOfText) == 1u << NameOfText::sizeShift && NameOfText::numberOfPairs == 1u << (64 - NameOfText::pairShift));
static_assert(std::is_trivially_default_constructible_v<PropertyCacheWords>);
static_assert(OBJECT_OFFSETOF(PropertyCacheWords, hashedSlotOffsetMask) == OBJECT_OFFSETOF(PropertyCacheWords, hashedSlots) + sizeof(void*));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
