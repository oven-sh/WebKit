/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTLayoutData.h"
#include "AOTRuntimeRecords.h"
#include "AOTStubsAccessTableWords.h"
#include <array>
#include <bit>
#include <type_traits>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

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
enum class SlotOrigin : uint8_t { ById, ByIdInStubFrame, ByVal };
enum class SlotOperation : uint8_t { Read, StoreStrict, StoreSloppy, Define };

constexpr SlotAccess accessOf(SlotOperation operation) { return operation == SlotOperation::Read ? SlotAccess::Read : operation == SlotOperation::Define ? SlotAccess::Define : SlotAccess::Store; }
constexpr SlotOperation operationFor(PutMode mode) { return mode == PutMode::Strict ? SlotOperation::StoreStrict : mode == PutMode::Sloppy ? SlotOperation::StoreSloppy : SlotOperation::Define; }

#define FOR_EACH_AOT_SLOT_HANDLER_TABLE(v) \
    v(ReadById, Read, ById, Slot) \
    v(ReadByIdPacked, Read, ById, PackedSlot) \
    v(ReadByIdInStubFrame, Read, ByIdInStubFrame, Slot) \
    v(ReadByIdPackedInStubFrame, Read, ByIdInStubFrame, PackedSlot) \
    v(ReadByVal, Read, ByVal, Slot) \
    v(StoreStrictById, StoreStrict, ById, Slot) \
    v(StoreStrictByIdPacked, StoreStrict, ById, PackedSlot) \
    v(StoreStrictByVal, StoreStrict, ByVal, Slot) \
    v(StoreSloppyById, StoreSloppy, ById, Slot) \
    v(StoreSloppyByIdPacked, StoreSloppy, ById, PackedSlot) \
    v(StoreSloppyByVal, StoreSloppy, ByVal, Slot) \
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

struct UsesOfSubstringAsKey {
    static constexpr uint8_t one = 1 << 3;
    static constexpr uint8_t two = 2 * one;
};

struct KeyedValuePlace {
    static constexpr unsigned numberOfPlacesPerAccess = 512;
    static constexpr unsigned numberOfPlaces = 2 * numberOfPlacesPerAccess;
    static constexpr unsigned sizeShift = 4;
    static constexpr uint32_t offsetMask = (numberOfPlacesPerAccess - 1) << sizeShift;

    static constexpr int32_t offsetOfFirst(SlotAccess access)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(access == SlotAccess::Read || access == SlotAccess::Store);
        return access == SlotAccess::Read ? 0 : numberOfPlacesPerAccess << sizeShift;
    }

    static uint32_t offsetFor(const JSCell* key, StructureID structureID) { return (static_cast<uint32_t>(std::bit_cast<uintptr_t>(key)) + structureID.bits()) & offsetMask; }

    static constexpr uint32_t absenceBit = 1;
    static constexpr uint32_t fieldBit = 2;
    static constexpr uint32_t extraPropertyBits = absenceBit | fieldBit;
    static constexpr uint32_t getterBit = 4;
    static constexpr uint32_t notOwnBits = getterBit | absenceBit;
    static constexpr uint32_t getterAboveBits = getterBit | fieldBit;
    static constexpr uint32_t globalObjectBit = 1;
    static constexpr uint32_t additionBit = 4;
    static constexpr uint32_t fieldOfAnyValueBits = additionBit | absenceBit;
    static constexpr uint32_t narrowReferenceBit = 1u << 31;

    struct ExtraProperty {
        uint16_t hiddenWordOffset;
        int16_t wordIndex;
    };

    JSCell* key;
    uint32_t structureID;
    union {
        int32_t wordIndex;
        uint32_t slotData;
        uint32_t offsetOfSlot;
        uint32_t wordInCellCage;
        ExtraProperty extraProperty;
    };
};

struct alignas(16) RestOfPlaceForStores {
    static constexpr ptrdiff_t distanceFromPlace = KeyedValuePlace::numberOfPlacesPerAccess << KeyedValuePlace::sizeShift;

    union {
        uint32_t structureIDAfterAddition;
        uint32_t typeOfField;
    };
};

struct AcceptedKindOfValue {
    static constexpr unsigned numberOfEntries = 256;
    static constexpr unsigned sizeShift = 3;
    static constexpr uint32_t leastWordThatIsKind = closureWordMarker;
    static constexpr unsigned structureIDShift = 6;
    static constexpr uint32_t kindOfCompiledClosureZero = 1u << 28;
    static constexpr uint32_t kindOfInt32 = 1u << 29;
    static constexpr uint32_t kindOfDouble = kindOfInt32 + 1;
    static constexpr uint32_t kindOfImmediateZero = kindOfInt32 + 16;
    static constexpr uint32_t indexMask = numberOfEntries - 1;

    enum class ClassOfSlot : uint8_t { JSValue, Cell, Cell32, OptionalCell32 };
    static constexpr uint32_t classesWithNarrowReferences = 2;

    static constexpr uint32_t isEntryOfFieldTable = 1u << 31;
    uint32_t type;
    uint32_t kind;
};

struct MissesOfPairs {
    static constexpr uint32_t numberOfCounters = 1u << 15;
    static constexpr uint32_t missesBetweenFilings = 16;
    static constexpr uint32_t missesBetweenFilingsOfDictionaries = 64;
};

struct PlaceOfOrdinaryConstructor {
    static constexpr unsigned numberOfPlacesLog2 = 6;
    static constexpr unsigned numberOfPlaces = 1u << numberOfPlacesLog2;
    static constexpr unsigned placeThatIsNeverRead = numberOfPlaces;
    static constexpr int32_t asksFilter = 1;

    static constexpr unsigned indexFor(uint32_t structureIDBits) { return structureIDBits * HashedSlot::structureMultiplier >> (32 - numberOfPlacesLog2); }
};

struct PrototypeChainFilter {
    static constexpr unsigned numberOfFiltersLog2 = 8;
    static constexpr unsigned numberOfFilters = 1u << numberOfFiltersLog2;
    static constexpr unsigned numberOfBitsLog2 = 6;

    uint32_t structureID;
    uint64_t objects;
};

enum class TrapOfProxy : uint8_t { Get, Has, Set };
inline constexpr unsigned numberOfTrapsOfProxies = 3;

struct ProxyAccessWords {
    JSObject* proxy;
    JSCell* key;
    JSObject* target;
    JSObject* handler;
    EncodedJSValue storedValue;
    uint64_t storeIsStrictOrHeaderOfSlot;
};

struct ReadGroupCache {
    static constexpr unsigned numberOfRows = 4;
    static constexpr unsigned numberOfPlaces = 6;
    static constexpr int16_t placeIsAskedOfStub = 0;
    static constexpr int16_t placeIsNotKnown = INT16_MIN;
    static constexpr uint32_t firstWait = 16;
    static constexpr uint8_t maxDoublings = 10;
    static constexpr uint8_t numberOfSamples = 16;
    static constexpr uint32_t markOfHiddenRow = 1;
    static constexpr uint32_t markOfExtension = 1;
    static constexpr uint32_t markOfStorage = 2;

    struct Row {
        uint32_t structure;
        uint32_t structureThatHoldsAll;
        int16_t places[numberOfPlaces];
        uint16_t numberOfReads;
        uint16_t wordOfExtension;

        void claim(uint32_t claimedBy, uint16_t wordOfExtension);
        void hide();
        void show();
    };

    void giveUp();

    uint32_t hasGivenUp;
    uint32_t missesLeft;
    Row rows[numberOfRows];
    uint32_t keys[numberOfPlaces];
    uint8_t unused;
    uint8_t doublings;
    uint8_t samplesLeft;
    uint8_t samplesThatHadRow;
    uint8_t cameInLastSample[numberOfRows];
};
static_assert(sizeof(ReadGroupCache::Row) == 24 && sizeof(ReadGroupCache) == 136 && !OBJECT_OFFSETOF(ReadGroupCache, hasGivenUp));

inline void ReadGroupCache::Row::claim(uint32_t claimedBy, uint16_t word)
{
    structure = claimedBy;
    wordOfExtension = word;
    structureThatHoldsAll = 0;
    std::ranges::fill(places, placeIsNotKnown);
}

inline void ReadGroupCache::Row::hide()
{
    structure |= markOfHiddenRow;
    structureThatHoldsAll = 0;
}

inline void ReadGroupCache::Row::show()
{
    structure &= ~markOfHiddenRow;
    bool knowsAll = numberOfReads;
    bool allLieInCell = true;
    for (unsigned index = 0; index < numberOfReads; ++index) {
        knowsAll &= places[index] != placeIsAskedOfStub && places[index] != placeIsNotKnown;
        allLieInCell &= places[index] > 0;
    }
    structureThatHoldsAll = 0;
    if (knowsAll && allLieInCell)
        structureThatHoldsAll = structure | (wordOfExtension ? markOfExtension : 0);
    else if (knowsAll && !wordOfExtension)
        structureThatHoldsAll = structure | markOfStorage;
}

inline void ReadGroupCache::giveUp()
{
    hasGivenUp = 1;
    for (Row& row : rows)
        row.structureThatHoldsAll = 0;
}

struct FrontWords {
    const uint32_t* rowsForStructureIDZero;
    const PackedSlot* plane;
};

enum class AnswerHandlerTable : uint8_t { Read, ReadOfTruth, ReadOfFurtherAnswer, ReadOfTruthOfFurtherAnswer, ReadUnderNameOfField, ReadOfTruthUnderNameOfField, ReadOfFurtherAnswerUnderNameOfField, ReadOfTruthOfFurtherAnswerUnderNameOfField };
inline constexpr std::array allAnswerHandlerTables { AnswerHandlerTable::Read, AnswerHandlerTable::ReadOfTruth, AnswerHandlerTable::ReadOfFurtherAnswer, AnswerHandlerTable::ReadOfTruthOfFurtherAnswer, AnswerHandlerTable::ReadUnderNameOfField, AnswerHandlerTable::ReadOfTruthUnderNameOfField, AnswerHandlerTable::ReadOfFurtherAnswerUnderNameOfField, AnswerHandlerTable::ReadOfTruthOfFurtherAnswerUnderNameOfField };
constexpr AnswerHandlerTable answerHandlerTable(bool asksTruth, bool isFurtherAnswer, SiteUse use) { return static_cast<AnswerHandlerTable>(asksTruth + 2 * isFurtherAnswer + 4 * (use == SiteUse::ReadUnderNameOfField)); }
constexpr bool asksTruth(AnswerHandlerTable table) { return static_cast<unsigned>(table) % 2; }
constexpr bool isOfFurtherAnswers(AnswerHandlerTable table) { return static_cast<unsigned>(table) / 2 % 2; }
constexpr SiteUse siteUseOf(AnswerHandlerTable table) { return static_cast<unsigned>(table) / 4 ? SiteUse::ReadUnderNameOfField : SiteUse::Read; }

using SlotHandlers = void*[numberOfSlotHandlerTables + allAnswerHandlerTables.size()][numberOfSlotKindsWithHandlers];

enum class ReflectSetResult : size_t { Refused, Stored, IsOrdinaryStore };

struct ForwardingProxyHandlers {
    static constexpr unsigned size = 4;
    static constexpr unsigned sizeShift = 4;
    static constexpr uint32_t offsetMask = (size - 1) << sizeShift;

    struct Entry {
        uint64_t wordAfterHeader;
        uint32_t structureID;
    };

    static uint32_t offsetFor(uint32_t structureID) { return structureID & offsetMask; }

    Entry entries[size];
};
static_assert(sizeof(ForwardingProxyHandlers::Entry) == 1u << ForwardingProxyHandlers::sizeShift);

struct StringSwitchTarget {
    static constexpr unsigned numberOfTargets = 128;
    static constexpr unsigned stringShift = 4;
    static constexpr unsigned instructionShift = 10;

    static unsigned placeOf(const void* string, const void* instruction)
    {
        auto address = std::bit_cast<uintptr_t>(instruction);
        return ((std::bit_cast<uintptr_t>(string) >> stringShift) + (address >> instructionShift) + address) % numberOfTargets;
    }

    const void* string;
    const void* instruction;
    intptr_t offset;
};

struct FunctionBirth {
    static constexpr unsigned numberOfBirths = 128;
    static constexpr unsigned executableShift = 4;

    static unsigned placeOf(const void* executable) { return (std::bit_cast<uintptr_t>(executable) >> executableShift) % numberOfBirths; }

    const void* executable;
    uint64_t header;
    void* allocator;
};

struct ReplacedMember {
    static constexpr unsigned numberOfPlaces = 4;
    static constexpr unsigned sizeShift = 5;

    const JSCell* holder;
    EncodedJSValue* word;
    uint32_t structureID;
    uint32_t name;
    uint64_t padding;
};

struct PropertyCacheWords {
    FrontWords front[numberOfSlotAccesses];
    const HashedSlot* hashedSlots;
    uint64_t hashedSlotOffsetMask;
    AbsenceFilter* absenceFilters;
    uint32_t absenceFilterGenerationForStores;
    uint32_t changesUnderNamesOnChains;
    uint32_t creationGeneration;
    const uint64_t* namesOfObjectPrototypes;
    const uint64_t* objectPrototypeStoreStoppers;
    const uint64_t* namesChangedOnChains;
    uint8_t* missesOfPairs;
    const NameOfText* namesOfTexts;
    uint8_t missIsDueForFiling;
    uint8_t placesSayAbsence;
    uint8_t placesTakeReplacementsAsUnwatched;
    uint8_t pairsHavePlaces;
    uint8_t hasCachedSlotValues;
    uint8_t hasRunSinceClear;
    uint32_t timesAbsencesWereForgotten;
    uint32_t slowPathReadCount;
    const void* infoOfClassPrototypes;
    SiteCacheState* siteCacheStates;
    uint8_t missesOfEmptySites;
    uint8_t missesOfPlacesOfPairs;
    uint8_t hitsOfFurtherAnswers;
    uint32_t firstWordOfWideAnswers;
    uint32_t wordOfCageThatHoldsUndefined;
    const CustomAccessorRecord* customAccessors;
    uint32_t numberStructureID;
    uint32_t booleanStructureID;
    uint32_t numbersOfNamesOfTraps[numberOfTrapsOfProxies];
    UniquedStringImpl* hasInstanceUID;
    UniquedStringImpl* prototypeUID;
    UniquedStringImpl* lengthUID;
    UniquedStringImpl* nameUID;
    const void* lookupOfOrdinaryObjects;
    const void* lookupOfArrays;
    const void* lookupOfFunctions;
    ForwardingProxyHandlers forwardingProxyHandlers;
    JSCell* defaultHasInstanceFunction;
    JSCell* holderOfDefaultHasInstanceFunction;
    const FieldExit* fieldExits;
    const FieldReturn* fieldReturns;
    uint8_t placesOfFieldExitsBySlot[FieldExit::numberOfPlacesBySlot];
    AccessTableWords accessTable;
#if !defined(BUN_HARDENED)
    SlotHandlers handlers;
#endif
    uint64_t headersOfPrototypeSlotsOfOrdinaryConstructors[PlaceOfOrdinaryConstructor::numberOfPlaces + 1];
    static constexpr unsigned shiftOfPlaceOfStructureBelowBuiltinClass = 4;
    static constexpr unsigned numberOfStructuresBelowBuiltinClasses = 64;
    uint32_t structuresBelowBuiltinClasses[numberOfStructuresBelowBuiltinClasses];

    static constexpr unsigned shiftOfErrorWithMessage = 4;
    static constexpr unsigned numberOfErrorsWithMessage = 64;
    uint32_t errorsWithMessage[numberOfErrorsWithMessage];

    PrototypeChainFilter* prototypeChainFilters;
    uint8_t prototypeChainFiltersMayBeFilled;

    const StringSwitchTarget* switchTargets;
    const FunctionBirth* functionBirths;
    void* clonedArgumentsAllocator;

    ReplacedMember replacedMembers[ReplacedMember::numberOfPlaces];

    uint32_t& placeOfErrorWithMessage(uint32_t structureID) { return errorsWithMessage[structureID >> shiftOfErrorWithMessage & (numberOfErrorsWithMessage - 1)]; }
    uint32_t& placeOfStructureBelowBuiltinClass(uint32_t structureID) { return structuresBelowBuiltinClasses[structureID >> shiftOfPlaceOfStructureBelowBuiltinClass & (numberOfStructuresBelowBuiltinClasses - 1)]; }
    void forgetPrototypeChainFilters()
    {
        if (!std::exchange(prototypeChainFiltersMayBeFilled, 0))
            return;
        zeroSpan(unsafeMakeSpan(prototypeChainFilters, PrototypeChainFilter::numberOfFilters));
    }
    void forgetReplacedMembers()
    {
        for (ReplacedMember& place : replacedMembers)
            place = { };
    }
    void forgetReplacedMembersOf(const JSCell* holder)
    {
        for (ReplacedMember& place : replacedMembers) {
            if (place.holder == holder)
                place = { };
        }
    }
    static constexpr ptrdiff_t offsetOfFront(SlotAccess access) { return OBJECT_OFFSETOF(PropertyCacheWords, front) + static_cast<ptrdiff_t>(static_cast<unsigned>(access) * sizeof(FrontWords)); }
};

static_assert(sizeof(HashedSlot) == 1u << HashedSlot::sizeShift && alignof(HashedSlot) == sizeof(HashedSlot));
static_assert(OBJECT_OFFSETOF(HashedSlot, slot) == HashedSlot::offsetOfSlot() && OBJECT_OFFSETOF(HashedSlot, key) == HashedSlot::offsetOfKey());
static_assert(std::is_trivially_copyable_v<HashedSlot> && std::is_aggregate_v<HashedSlot>);
static_assert(static_cast<unsigned>(SlotAccess::Delete) <= HashedSlot::accessMask && alignof(UniquedStringImpl) > HashedSlot::accessMask);
static_assert(hasOneBitSet(HashedSlot::ways));
static_assert(sizeof(ReplacedMember) == 1u << ReplacedMember::sizeShift && hasOneBitSet(ReplacedMember::numberOfPlaces));
static_assert(sizeof(FrontWords) == 2 * sizeof(void*) && OBJECT_OFFSETOF(FrontWords, plane) == sizeof(void*));
static_assert(1u << StructureRowWords::wordShift == sizeof(uint32_t));
static_assert(sizeof(AbsenceFilter) == 1u << AbsenceFilter::sizeShift && hasOneBitSet(AbsenceFilter::numberOfFilters) && hasOneBitSet(AbsenceFilter::numberOfWordsForStores));
static_assert(std::is_trivially_copyable_v<AbsenceFilter> && std::is_aggregate_v<AbsenceFilter>);
static_assert(sizeof(NameOfText) == 1u << NameOfText::sizeShift && NameOfText::numberOfPairs == 1u << (64 - NameOfText::pairShift));
static_assert(sizeof(KeyedValuePlace) == 1u << KeyedValuePlace::sizeShift && hasOneBitSet(KeyedValuePlace::numberOfPlacesPerAccess));
static_assert(KeyedValuePlace::numberOfPlacesPerAccess <= UINT16_MAX);
static_assert(sizeof(RestOfPlaceForStores) == sizeof(KeyedValuePlace));
static_assert(sizeof(AcceptedKindOfValue) == 1u << AcceptedKindOfValue::sizeShift && hasOneBitSet(AcceptedKindOfValue::numberOfEntries));
static_assert(AcceptedKindOfValue::leastWordThatIsKind < compiledObjectShapeBit && pointerUpperHalfBits < AcceptedKindOfValue::leastWordThatIsKind && bigIntHashMask < AcceptedKindOfValue::leastWordThatIsKind);
static_assert(UINT32_MAX >> AcceptedKindOfValue::structureIDShift < AcceptedKindOfValue::kindOfCompiledClosureZero && AcceptedKindOfValue::kindOfImmediateZero + UINT8_MAX < AcceptedKindOfValue::leastWordThatIsKind);
static_assert(!!(static_cast<uint32_t>(AcceptedKindOfValue::ClassOfSlot::Cell32) & AcceptedKindOfValue::classesWithNarrowReferences) && !!(static_cast<uint32_t>(AcceptedKindOfValue::ClassOfSlot::OptionalCell32) & AcceptedKindOfValue::classesWithNarrowReferences));
static_assert(!(static_cast<uint32_t>(AcceptedKindOfValue::ClassOfSlot::Cell) & AcceptedKindOfValue::classesWithNarrowReferences) && AcceptedKindOfValue::classesWithNarrowReferences << 30 == KeyedValuePlace::narrowReferenceBit);
static_assert(std::is_trivially_default_constructible_v<PropertyCacheWords>);
static_assert(OBJECT_OFFSETOF(PropertyCacheWords, hashedSlotOffsetMask) == OBJECT_OFFSETOF(PropertyCacheWords, hashedSlots) + sizeof(void*));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
