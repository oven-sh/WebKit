/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTLayoutData.h"
#include "AOTRuntimeRecords.h"
#include <array>
#include <bit>
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

constexpr SlotAccess accessOf(SlotOperation operation) { return operation == SlotOperation::Read ? SlotAccess::Read : operation == SlotOperation::Define ? SlotAccess::Define : SlotAccess::Store; }
constexpr SlotOperation operationFor(PutMode mode) { return mode == PutMode::Strict ? SlotOperation::StoreStrict : mode == PutMode::Sloppy ? SlotOperation::StoreSloppy : SlotOperation::Define; }

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
    constexpr SlotHandlerTableTraits traits[] = {
#define AOT_SLOT_HANDLER_TABLE_TRAITS(name, operation, origin, form) { SlotOperation::operation, SlotOrigin::origin, SlotForm::form },
        FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_SLOT_HANDLER_TABLE_TRAITS)
#undef AOT_SLOT_HANDLER_TABLE_TRAITS
    };
    return traits[static_cast<unsigned>(table)];
}

constexpr SlotHandlerTable slotHandlerTable(SlotOperation operation, SlotOrigin origin, SlotForm form)
{
    for (SlotHandlerTable table : allSlotHandlerTables) {
        if (traitsOf(table) == SlotHandlerTableTraits { operation, origin, form })
            return table;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct StructureRowWords {
    static constexpr unsigned granuleShift = 6;
    static constexpr unsigned wordShift = 2;
};

struct AbsenceFilter {
    static constexpr unsigned numberOfFilters = 256;
    static constexpr unsigned sizeShift = 5;
    static constexpr unsigned bitsOfWordShift = 6;

    static constexpr unsigned indexFor(uint32_t structureIDBits) { return (structureIDBits >> StructureRowWords::granuleShift) & (numberOfFilters - 1); }
    static constexpr unsigned numberOfWordsForStores = 2;
    void addNameThatStopsStores(uint32_t identifierNumber) { namesThatStopStores[(identifierNumber >> bitsOfWordShift) & (numberOfWordsForStores - 1)] |= uint64_t { 1 } << (identifierNumber & 63); }
    bool letsStoresPass(uint32_t identifierNumber) const { return !(namesThatStopStores[(identifierNumber >> bitsOfWordShift) & (numberOfWordsForStores - 1)] >> (identifierNumber & 63) & 1); }

    uint32_t structureID;
    uint32_t generationForStores;
    uint64_t namesThatStopStores[numberOfWordsForStores];
    // The number of changes under a name that had been reported when the filter was built. While the count is unchanged, the filter is complete for every name.
    uint32_t changesUnderNames;
    uint32_t unused;
};

struct NameOfText {
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

// Where the objects of one Structure keep their own value under one key, found by the cell of the key. Nothing is asked of the key, so a string that is no atom is found as fast as one that is.
// Or that neither such an object nor any object above it has anything under the key.
struct KeyedValuePlace {
    static constexpr unsigned numberOfPlacesPerAccess = 512;
    static constexpr unsigned numberOfPlaces = 2 * numberOfPlacesPerAccess;
    static constexpr unsigned sizeShift = 4;
    static constexpr uint32_t offsetMask = (numberOfPlacesPerAccess - 1) << sizeShift;

    // The places for reads come first, then those for stores that replace the value.
    static constexpr int32_t offsetOfFirst(SlotAccess access)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(access != SlotAccess::Define);
        return access == SlotAccess::Read ? 0 : numberOfPlacesPerAccess << sizeShift;
    }

    static uint32_t offsetFor(const JSCell* key, StructureID structureID) { return (static_cast<uint32_t>(std::bit_cast<uintptr_t>(key)) + structureID.bits()) & offsetMask; }

    // The low bits of a StructureID are zero. What a place says beside the absence bit holds until an object of some chain changes: PropertyCacheWords::placesSayAbsence.
    static constexpr uint32_t absenceBit = 1;
    static constexpr uint32_t fieldBit = 2;
    static constexpr uint32_t extraPropertyBits = absenceBit | fieldBit;
    // Of a place for stores under the Structure of globalThis: the StructureID and the word index are those of the global object.
    static constexpr uint32_t globalObjectBit = 1;
    // Of a place for stores: the objects of the Structure have nothing under the key, a store adds it, and their storage has room. The word index says where the value goes, and
    // RestOfPlaceForStores which Structure the object has then. It holds for as long as the slot of the hashed table that said so: PropertyCacheWords::placesFiledFromSlots.
    static constexpr uint32_t additionBit = 4;
    // Of a place for stores beside the field bit: the slot holds a narrow reference.
    static constexpr uint32_t narrowReferenceBit = 1u << 31;

    // For a compiled object whose Structure identifies the Structure of its extra-property object.
    struct ExtraProperty {
        // The offset in the cell of the reference to the object that holds the property.
        uint16_t hiddenWordOffset;
        // Slot::wordIndexFor() in that object.
        int16_t wordIndex;
    };

    JSCell* key;
    uint32_t structureID;
    union {
        // Slot::wordIndexFor().
        int32_t wordIndex;
        // Beside the field bit: SlotData::bits() of a field that lies in the cell.
        uint32_t slotData;
        // Beside the field bit in a place for stores: how far into the cell lies the slot of a field, and narrowReferenceBit. The slot is of one of AcceptedKindOfValue::ClassOfSlot.
        // RestOfPlaceForStores has the type of the field. It holds for as long as the slot of the hashed table that led to the store: PropertyCacheWords::placesFiledFromSlots.
        uint32_t offsetOfSlot;
        // Beside both bits.
        ExtraProperty extraProperty;
    };
};

// One for each place for stores, behind them and as far apart as they are, so that a stub reaches it from the place by one distance.
struct alignas(16) RestOfPlaceForStores {
    static constexpr ptrdiff_t distanceFromPlace = KeyedValuePlace::numberOfPlacesPerAccess << KeyedValuePlace::sizeShift;

    union {
        // Beside the addition bit.
        uint32_t structureIDAfterAddition;
        // Beside the field bit: AcceptedKindOfValue::type.
        uint32_t typeOfField;
    };
};

// The type of a field has accepted a value of a kind, which was stored in a slot of a class. It accepts every other value of that kind, and every slot of that class holds it in the same way.
// What the stubs ask of a cell they ask of the type of the cell, which its Structure has, and of the word at offsetOfShapeWord: the shape of a compiled object, the function of a slim closure.
// Such a word is the kind. In every other cell that word is below them and nothing is asked of it. Of a closure that is a JSFunction the stubs ask for its function, which is its kind.
// Of any other cell the StructureID is the kind, shifted so that it is below all of these. The kinds of what is no cell lie between.
struct AcceptedKindOfValue {
    static constexpr unsigned numberOfEntries = 256;
    static constexpr unsigned sizeShift = 3;
    static constexpr uint32_t leastWordThatIsKind = closureWordMarker;
    // No two Structures begin within as many bytes.
    static constexpr unsigned structureIDShift = 6;
    // Added to the number of the function.
    static constexpr uint32_t kindOfCompiledClosureZero = 1u << 28;
    static constexpr uint32_t kindOfInt32 = 1u << 29;
    static constexpr uint32_t kindOfDouble = kindOfInt32 + 1;
    // undefined, null, true and false: added to the low word of the encoded value.
    static constexpr uint32_t kindOfImmediateZero = kindOfInt32 + 16;
    // Of the sum of the type and the kind, so that one type in two classes of slots has two entries for a kind.
    static constexpr uint32_t indexMask = numberOfEntries - 1;

    // JSValue, Cell: the value as it is. Cell32, OptionalCell32: a cell as a narrow reference, undefined and null as their low words.
    enum class ClassOfSlot : uint8_t { JSValue, Cell, Cell32, OptionalCell32 };
    static constexpr uint32_t classesWithNarrowReferences = 2;

    // Slot::FieldPayload::type, with the class of the slot in the two bits that are zero in the offset of a record.
    uint32_t type;
    uint32_t kind;
};

enum class TrapOfProxy : uint8_t { Get, Has, Set };
inline constexpr unsigned numberOfTrapsOfProxies = 3;

// What the stub that goes through a proxy keeps in its frame while code of the program runs.
struct ProxyAccessWords {
    JSObject* proxy;
    JSCell* key;
    JSObject* target;
    JSObject* handler;
    EncodedJSValue storedValue;
    // Of a store: whether the code is strict. Of a read: Slot::headerBits() of the slot that led here.
    uint64_t storeIsStrictOrHeaderOfSlot;
};

struct MissesOfPairs {
    static constexpr uint32_t numberOfCounters = 1u << 15;
    static constexpr uint32_t missesBetweenFilings = 16;
    static constexpr uint32_t missesBetweenFilingsOfDictionaries = 64;
};

struct FrontWords {
    const uint32_t* rowsForStructureIDZero;
    const PackedSlot* plane;
};

using SlotHandlers = void*[numberOfSlotHandlerTables][numberOfSlotKindsWithHandlers];

struct PropertyCacheWords {
    FrontWords front[numberOfSlotAccesses];
    const HashedSlot* hashedSlots;
    uint64_t hashedSlotOffsetMask;
    AbsenceFilter* absenceFilters;
    uint32_t absenceFilterGenerationForStores;
    uint32_t changesUnderNamesOnChains;
    const uint64_t* namesOfObjectPrototypes;
    const uint64_t* namesChangedOnChains;
    uint8_t* missesOfPairs;
    const NameOfText* namesOfTexts;
    uint8_t missIsDueForFiling;
    uint8_t placesSayAbsence;
    // A place for stores was filled while nobody watched the Structure for replacements. Such a place holds until somebody does.
    uint8_t placesTakeReplacementsAsUnwatched;
    // StringKeyCache::KeyOfPair: some pair has a place beside its key.
    uint8_t pairsHavePlaces;
    // How often the places that say absence were cleared. A pair that says absence says it for one such count.
    uint32_t timesAbsencesWereForgotten;
    const void* infoOfClassPrototypes;
    SiteCacheState* siteCacheStates;
    uint8_t missesOfEmptySites;
    uint8_t storesAtEmptySites;
    uint8_t storesToGuardedFields;
    // Reads by a pair that has the place of another Structure, in steps of StringKeyCache::KeyOfPair::stepOfMisses.
    uint8_t missesOfPlacesOfPairs;
    // How many places for stores were filled, since the places were last cleared, with what a slot of the hashed table said about an addition or a field. Such a place holds until such a slot leaves that table.
    // Once as many were filled as there are places, one more would only push another out: where a program has that many pairs of a key and a Structure in turns, no place is found again before it is pushed out.
    uint16_t placesFiledFromSlots;
    // How many of them were cleared because a slot left the table. Where it is all of them, no place says what a slot said. The count above goes on: only a collection begins it anew.
    uint16_t placesFiledFromSlotsAndCleared;
    const CustomAccessorRecord* customAccessors;
    uint32_t numberStructureID;
    uint32_t booleanStructureID;
    // By TrapOfProxy. Zero where the image does not have the name.
    uint32_t numbersOfNamesOfTraps[numberOfTrapsOfProxies];
    UniquedStringImpl* hasInstanceUID;
    UniquedStringImpl* prototypeUID;
    UniquedStringImpl* lengthUID;
    UniquedStringImpl* nameUID;
    const void* lookupOfOrdinaryObjects;
    const void* lookupOfArrays;
    const void* lookupOfFunctions;
    JSCell* defaultHasInstanceFunction;
    const FieldExit* fieldExits;
#if !defined(BUN_HARDENED)
    SlotHandlers handlers;
#endif

    static constexpr ptrdiff_t offsetOfFront(SlotAccess access) { return OBJECT_OFFSETOF(PropertyCacheWords, front) + static_cast<ptrdiff_t>(static_cast<unsigned>(access) * sizeof(FrontWords)); }
};

static_assert(sizeof(HashedSlot) == 1u << HashedSlot::sizeShift && alignof(HashedSlot) == sizeof(HashedSlot));
static_assert(OBJECT_OFFSETOF(HashedSlot, slot) == HashedSlot::offsetOfSlot() && OBJECT_OFFSETOF(HashedSlot, key) == HashedSlot::offsetOfKey());
static_assert(std::is_trivially_copyable_v<HashedSlot> && std::is_aggregate_v<HashedSlot>);
static_assert(static_cast<unsigned>(SlotAccess::Delete) <= HashedSlot::accessMask && alignof(UniquedStringImpl) > HashedSlot::accessMask);
static_assert(hasOneBitSet(HashedSlot::ways));
static_assert(sizeof(FrontWords) == 2 * sizeof(void*) && OBJECT_OFFSETOF(FrontWords, plane) == sizeof(void*));
static_assert(1u << StructureRowWords::wordShift == sizeof(uint32_t));
static_assert(sizeof(AbsenceFilter) == 1u << AbsenceFilter::sizeShift && hasOneBitSet(AbsenceFilter::numberOfFilters) && hasOneBitSet(AbsenceFilter::numberOfWordsForStores));
static_assert(std::is_trivially_copyable_v<AbsenceFilter> && std::is_aggregate_v<AbsenceFilter>);
static_assert(sizeof(NameOfText) == 1u << NameOfText::sizeShift && NameOfText::numberOfPairs == 1u << (64 - NameOfText::pairShift));
static_assert(sizeof(KeyedValuePlace) == 1u << KeyedValuePlace::sizeShift && hasOneBitSet(KeyedValuePlace::numberOfPlacesPerAccess));
static_assert(KeyedValuePlace::numberOfPlacesPerAccess <= UINT16_MAX);
// The two counts lie where nothing lay: the words behind them are where they were.
static_assert(OBJECT_OFFSETOF(PropertyCacheWords, customAccessors) == OBJECT_OFFSETOF(PropertyCacheWords, missesOfEmptySites) + sizeof(void*));
static_assert(sizeof(RestOfPlaceForStores) == sizeof(KeyedValuePlace));
static_assert(sizeof(AcceptedKindOfValue) == 1u << AcceptedKindOfValue::sizeShift && hasOneBitSet(AcceptedKindOfValue::numberOfEntries));
static_assert(AcceptedKindOfValue::leastWordThatIsKind < compiledObjectShapeBit && pointerUpperHalfBits < AcceptedKindOfValue::leastWordThatIsKind && bigIntHashMask < AcceptedKindOfValue::leastWordThatIsKind);
static_assert(UINT32_MAX >> AcceptedKindOfValue::structureIDShift < AcceptedKindOfValue::kindOfCompiledClosureZero && AcceptedKindOfValue::kindOfImmediateZero + UINT8_MAX < AcceptedKindOfValue::leastWordThatIsKind);
static_assert(static_cast<uint32_t>(AcceptedKindOfValue::ClassOfSlot::Cell32) & AcceptedKindOfValue::classesWithNarrowReferences && static_cast<uint32_t>(AcceptedKindOfValue::ClassOfSlot::OptionalCell32) & AcceptedKindOfValue::classesWithNarrowReferences);
static_assert(!(static_cast<uint32_t>(AcceptedKindOfValue::ClassOfSlot::Cell) & AcceptedKindOfValue::classesWithNarrowReferences) && AcceptedKindOfValue::classesWithNarrowReferences << 30 == KeyedValuePlace::narrowReferenceBit);
static_assert(std::is_trivially_default_constructible_v<PropertyCacheWords>);
static_assert(OBJECT_OFFSETOF(PropertyCacheWords, hashedSlotOffsetMask) == OBJECT_OFFSETOF(PropertyCacheWords, hashedSlots) + sizeof(void*));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
