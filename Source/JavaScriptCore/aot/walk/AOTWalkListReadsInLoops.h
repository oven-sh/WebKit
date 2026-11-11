/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBitVector.h"
#include "AOTWalk.h"
#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

struct HoistedArrayGuard {
    Reference name;
    ArrayStorage storage { ArrayStorage::Other };
    bool admitsInt32Storage { false };
    bool isWritable { false };
    bool turnsStoreIntoLists { false };
    bool leavesBirthAsItIs { false };
    bool takesInstancesBelow { false };
    bool isAskedForItsStorageAlone { false };
    bool onlyHoldsRows { false };
    WrittenListOfVariable written;
    bool hasAlternateStorage { false };
    ArrayStorage alternateStorage { ArrayStorage::Contiguous };
    bool alternateTakesAnyKind { false };
    bool keepsStorageWithoutAlternate { false };
    bool keepsDoublesWithoutAlternate { false };
    bool isStepped { false };
    ArrayStorage expectedStorage { ArrayStorage::Other };
    bool isNextWalkedForBoxedNumbers { false };
    bool holdsIntegersByLink { false };
};

inline bool isConvertedInPreheader(const HoistedArrayGuard& list)
{
    return (list.storage == ArrayStorage::Double || !hasArrayStorageForm(list.storage)) && list.isWritable && !list.admitsInt32Storage && !list.hasAlternateStorage;
}

BuiltinClassID classOfViewBesideInstancesBelowIt(const Walker&, TypeID);
bool loopTakesInstancesBelowViewForItsLength(Walker&, BlockID inputHead, Reference view);
bool lengthOfViewTakesInstancesBelow(Walker&, TypeID typeOfView);
Reference buildTestOfViewOrInstanceBelow(Walker&, Reference view, Origin);
void noteThatLengthOfViewIsFoundAbove(Walker&, Reference view);
void noteThatViewKeepsItsLength(Walker&, Reference view, Reference test);
void askAgainWhetherLengthOfViewIsFoundAbove(Walker&, Reference view, Origin);
std::optional<HoistedArrayGuard> findHoistedArrayGuard(Walker&, const BodyValue& read);
bool isExpectedToBeView(Walker&, ValueID unbuiltList);
bool convertsCheckedCounter(Walker&, const BodyValue& operation, BlockID inputHead, std::span<const Reference> entering);
std::optional<HoistedArrayGuard> findHoistedArrayGuard(Walker&, Reference list);
std::optional<HoistedArrayGuard> findRowHolderGuard(Walker&, const BodyValue& read);
std::optional<HoistedArrayGuard> findListTestedForStoresInFrontOfLoop(Walker&, Reference list);
std::optional<HoistedArrayGuard> tryGuardNumericArrayInIteration(Walker&, const BodyValue& read, Reference list);
bool isExpectedToBeListOfNumbers(Walker&, TypeID expected);
bool turnsReadRowsOfNumbersThatTheyFind(Walker&, BlockID inputHead, std::span<const HoistedArrayGuard>, std::span<const HoistedSpeculation>);
bool isFreshAllocation(Walker&, Reference name);
bool canGuardArrayInIteration(Walker&, Reference list);
bool guardPristineArrayInIteration(Walker&, Reference list, Origin);
Reference askWhetherListIsAsMade(Walker&, Reference list, Origin);
bool isMathCall(const Walker&, const BodyIndex&, const BodyValue& operation);
Reference askWhetherStorageOfListIsOfKind(Walker&, Reference list, ArrayStorage, Origin, bool isWritable = false, bool admitsInt32Storage = false);
std::optional<ArrayStorage> kindOfRowsFoundBeforeLoop(Walker&, Reference row);
void noteKindOfStorageThatTurnFound(Walker&, Reference list, ArrayStorage, bool isWritable = false, bool admitsInt32Storage = false, bool turnsStoreIntoLists = false);
std::optional<HoistedArrayGuard> tryHoistArrayGuardForCall(Walker&, const BodyValue& call, Reference list, TypeID contents);
std::optional<HoistedArrayGuard> tryHoistDestructuredArrayGuard(Walker&, const BodyValue& open, Reference list);
std::optional<HoistedArrayGuard> tryGuardDestructuredArrayInIteration(Walker&, const BodyValue& open, Reference list);
bool isDestructuredArrayLikelyNumeric(Walker&, const BodyValue& open, Reference list);
bool tryHoistInt32Guard(Walker&, const BodyValue& read, Reference list);
void retestArrayForInt32s(Walker&, const BodyValue& read);
std::optional<HoistedArrayGuard> tryHoistArrayGuardForAccess(Walker&, const BodyValue& read, Reference list, ArrayStorage kindStoredInto = ArrayStorage::Other, bool goesByKindOfStorage = false);
std::optional<PassedTest> tryHoistElementPresenceCheck(Walker&, const BodyValue& read, const HoistedArrayGuard&, Reference list, Reference position);
struct WhatReadsFromInFrontOfLoopFind {
    BitVector found;
    BitVector refused;
};
bool readsListFoundByReadFromInFront(const Program&, const BodyIndex&, const BodyValue& operation, uint32_t firstValueOfLoop, WhatReadsFromInFrontOfLoopFind&);
void hoistLeadingInitializationChecks(Walker&, BlockID inputHead);
void readListsOfStableBindingsInFrontOfLoop(Walker&, BlockID inputHead);
Reference fieldValueInPreheader(Walker&, const BodyIndex&, ValueID read, uint32_t firstValueOfLoop);
void readFieldArraysInPreheader(Walker&, BlockID inputHead);
void readRowArraysInPreheader(Walker&, BlockID inputHead);
Reference nameOfListThatEntersLoopAround(Walker&, const BodyIndex&, const BodyValue& access);
std::optional<uint64_t> findArrayGuard(Walker&, const WalkState&, Reference array);
void noteJoinedArrayGuard(Walker&, Reference joined, uint64_t guard);
struct ChosenArray {
    Reference name;
    ValueID written;
};
Vector<ChosenArray, 3> findChosenArrays(Walker&, const BodyIndex&, ValueID array, uint32_t firstValueOfLoop);
void forEachElementAccessInInlinedCallee(Walker&, const BodyValue& call, NOESCAPE const ScopedLambda<void(ValueID argument, ValueID key)>&);
bool copyOfCalleeReadsUnderKeyWhatNobodyKnows(Walker&, const BodyValue& call, uint32_t firstValueOfLoop);
ValueID readOfVariableThatFindsReceiverInLoop(const Program&, const BodyIndex&, const BodyValue& access, uint32_t firstValueOfLoop);
bool storesListedNumberIntoView(Walker&, const BodyIndex&, const BodyValue& store, TypeID holder, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard>, std::span<const HoistedSpeculation>, std::span<const Reference> proofs);
bool guardRowInIteration(Walker&, Reference row, Origin);
TypeID seenUnderVariableKey(Walker&, const BodyValue& read);
bool rowsHoldIntegers(Walker&, const BodyValue& access);
bool storesNumberIntoRow(Walker&, const BodyIndex&, const BodyValue& store, TypeID stored, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard> lists);
bool storesIntoListTestedInFrontOfLoop(Walker&, const BodyIndex&, const BodyValue& store, uint32_t firstValueOfLoop, std::span<const HoistedArrayGuard>);
void nameKindOfListTestedInFrontOfLoop(Draft& read, const HoistedArrayGuard&);
void takeStorageAndLengthFromInFrontOfLoop(Walker&, Draft& read, const HoistedArrayGuard&);
bool positionIsKnownToLieInsideList(Walker&, Reference list, Reference position);
Reference storageOfListFromInFrontOfLoop(Walker&, const HoistedArrayGuard&, Origin);

bool isUnbuiltListOfNumbers(Walker&, ValueID unbuiltList);
bool holeLeavesTurnBehindRow(Walker&, const BodyValue& read, const HoistedArrayGuard&);
bool guardElementPresenceInIteration(Walker&, Draft& read, Reference list, Reference position, const HoistedArrayGuard&);
bool turnTakesElementForKeyInListReadsInLoops(Walker&, ValueID element);
bool hasOnlyOtherArrayReads(Walker&, BlockID inputHead, ValueID made);
bool readsNumericArray(Walker&, BlockID inputHead);
struct HoistedCounterCheck;
void appendCarriedNumbers(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, std::span<const HoistedArrayGuard>, std::span<const HoistedSpeculation>, Vector<HoistedCounterCheck, 2>&);
bool isResultUnused(const BodyIndex&, ValueID result);
enum class WhatStandsBesideRead : uint8_t { Number, NumberOrNothing };
bool onlyUserTakesUndefinedForNaNInTypedElements(Walker&, const BodyValue& read, WhatStandsBesideRead = WhatStandsBesideRead::Number);
bool readLeavesTurnAtHoleBehindRow(Walker&, const BodyValue& read);
bool preservesHoistedArrayGuards(const Walker&, const Effects&);
bool preservesHoistedArrayGuards(const Walker&, const Effects&, std::span<const HoistedArrayGuard>, const Walker::AllocationsUsedInLoop&);
bool preservesHoistedArrayGuards(Walker&, const Effects&, std::span<const HoistedArrayGuard>, BlockID inputHead);

class NamesBeforeLoop {
public:
    NamesBeforeLoop(const Walker& walker, BlockID inputHead)
        : m_walker(walker)
        , m_inputHead(inputHead)
    {
    }

    Vector<ValueID, 2> findNamesOf(Reference value);

private:
    struct Name {
        uint32_t value;
        ValueID id;
    };
    void collectNames();

    const Walker& m_walker;
    BlockID m_inputHead;
    bool m_areNamesCollected { false };
    Vector<Name, 8> m_names;
};

bool readsLengthOfViewTestedInFrontOfLoop(Walker&, const BodyValue& read);

struct ListsReadInLoop {
    Vector<HoistedArrayGuard, 2> lists;
    bool areLeftAsTheyAre { false };
    bool areTested { false };
    bool someListIsTakenUnderCounterAskedInFront { false };
    BlockID alternateStorageEntry;
};

template<typename Entry, Reference Entry::* nameOfEntry>
class FirstIndexByName {
public:
    size_t find(std::span<const Entry> entries, Reference name)
    {
        if (entries.size() <= linearSearchLimit || !mayBeKey(name)) [[likely]] {
            auto first = std::ranges::find(entries, name, nameOfEntry);
            return first == entries.end() ? notFound : static_cast<size_t>(first - entries.begin());
        }
        for (; m_indexedEntryCount < entries.size(); ++m_indexedEntryCount) {
            if (mayBeKey(entries[m_indexedEntryCount].*nameOfEntry))
                m_firstIndices.add((entries[m_indexedEntryCount].*nameOfEntry).bits(), static_cast<uint32_t>(m_indexedEntryCount));
        }
        auto place = m_firstIndices.find(name.bits());
        return place == m_firstIndices.end() ? notFound : place->value;
    }

    void forget()
    {
        m_firstIndices.clear();
        m_indexedEntryCount = 0;
    }

    static constexpr size_t linearSearchLimit = 8;

private:
    static bool mayBeKey(Reference name) { return name.bits() < std::numeric_limits<uint32_t>::max() - 1; }

    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_firstIndices;
    size_t m_indexedEntryCount { 0 };
};

using ArrayLookupByName = FirstIndexByName<HoistedArrayGuard, &HoistedArrayGuard::name>;
using SpeculationLookupByName = FirstIndexByName<HoistedSpeculation, &HoistedSpeculation::value>;

void reserveRoomOfListsThatLoopFills(Walker&, BlockID inputHead, std::span<const Reference> entering);
Reference literalThatCallPushesOntoInEveryTurn(Walker&, const BodyValue& call, BlockID inputHead);
Reference untouchedLiteralOfPush(Walker&, const BodyValue& call, LoopID);
bool callsArrayPush(Walker&, const BodyValue& call, Reference list);
bool isUntouchedLiteral(const BodyIndex&, ValueID array, LoopID);
bool storesUnderSteppedCounter(Walker&, const BodyIndex&, const BodyValue& store);
ListsReadInLoop findListsReadInLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses);
void buildTestsOfListsReadInLoop(Walker&, BlockID inputHead, std::span<const Reference> entering, ListsReadInLoop&, std::span<const HoistedSpeculation>, std::span<const Reference> proofs, BlockID& genericStart, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
