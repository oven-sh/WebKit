/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTWalk.h"
#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

struct ListTestedInFrontOfLoop {
    Reference name;
    ArrayStorage storage { ArrayStorage::Other };
    bool admitsInt32Storage { false };
    bool isWritable { false };
    bool turnsStoreIntoLists { false };
    bool leavesBirthAsItIs { false };
    bool takesInstancesBelow { false };
    bool isAskedForItsStorageAlone { false };
    WrittenListOfVariable written;
    bool kindMayWellBeAnother { false };
    ArrayStorage kindAskedOtherwise { ArrayStorage::Contiguous };
    bool keepsKindWhereNoCopyIsWalkedForAnother { false };
};

inline bool isConvertedInFrontOfTypedCopy(const ListTestedInFrontOfLoop& list)
{
    return (list.storage == ArrayStorage::Double || !hasArrayStorageForm(list.storage)) && list.isWritable && !list.admitsInt32Storage;
}

BuiltinClassID classOfViewBesideInstancesBelowIt(const Walker&, TypeID);
bool loopTakesInstancesBelowViewForItsLength(Walker&, BlockID inputHead, Reference view);
bool lengthOfViewTakesInstancesBelow(Walker&, TypeID typeOfView);
Reference buildTestOfViewOrInstanceBelow(Walker&, Reference view, Origin);
void noteThatLengthOfViewIsFoundAbove(Walker&, Reference view);
void askAgainWhetherLengthOfViewIsFoundAbove(Walker&, Reference view, Origin);
std::optional<ListTestedInFrontOfLoop> findListTestedInFrontOfLoop(Walker&, const BodyValue& read);
std::optional<ListTestedInFrontOfLoop> findListTestedInFrontOfLoop(Walker&, Reference list);
std::optional<ListTestedInFrontOfLoop> findListTestedForStoresInFrontOfLoop(Walker&, Reference list);
std::optional<ListTestedInFrontOfLoop> askListOfNumbersInTurnOfTypedCopy(Walker&, const BodyValue& read, Reference list);
bool isExpectedToBeListOfNumbers(Walker&, TypeID expected);
bool turnsReadRowsOfNumbersThatTheyFind(Walker&, BlockID inputHead, std::span<const ListTestedInFrontOfLoop>, std::span<const GuessInFrontOfLoop>);
bool turnOfTypedCopyMayAskList(Walker&, Reference list);
bool goOnWhereListIsAsMadeOrBeginTurnAgain(Walker&, Reference list, Origin);
Reference askWhetherListIsAsMade(Walker&, Reference list, Origin);
Reference askWhetherStorageOfListIsOfKind(Walker&, Reference list, ArrayStorage, Origin, bool isWritable = false);
void noteKindOfStorageThatTurnFound(Walker&, Reference list, ArrayStorage, bool isWritable = false);
std::optional<ListTestedInFrontOfLoop> askListInFrontOfTypedCopy(Walker&, const BodyValue& read, Reference list, ArrayStorage kindStoredInto = ArrayStorage::Other, bool goesByKindOfStorage = false);
std::optional<PassedTest> askElementInFrontOfTypedCopy(Walker&, const BodyValue& read, const ListTestedInFrontOfLoop&, Reference list, Reference position);
struct WhatReadsFromInFrontOfLoopFind {
    BitVector found;
    BitVector refused;
};
bool readsListFoundByReadFromInFront(const Program&, const BodyIndex&, const BodyValue& operation, uint32_t firstValueOfLoop, WhatReadsFromInFrontOfLoopFind&);
void readListsOfStableBindingsInFrontOfLoop(Walker&, BlockID inputHead);
Reference nameOfListThatEntersLoopAround(Walker&, const BodyIndex&, const BodyValue& access);
bool copyOfCalleeReadsUnderKeyWhatNobodyKnows(Walker&, const BodyValue& call, uint32_t firstValueOfLoop);
ValueID readOfVariableThatFindsReceiverInLoop(const BodyIndex&, const BodyValue& access, uint32_t firstValueOfLoop);
bool storesIntoListTestedInFrontOfLoop(Walker&, const BodyIndex&, const BodyValue& store, uint32_t firstValueOfLoop, std::span<const ListTestedInFrontOfLoop>);
void nameKindOfListTestedInFrontOfLoop(Draft& read, const ListTestedInFrontOfLoop&);
void takeStorageAndLengthFromInFrontOfLoop(Walker&, Draft& read, const ListTestedInFrontOfLoop&);
bool positionIsKnownToLieInsideList(Walker&, Reference list, Reference position);
Reference storageOfListFromInFrontOfLoop(Walker&, const ListTestedInFrontOfLoop&, Origin);

bool holeLeavesTurnBehindRow(Walker&, const BodyValue& read, const ListTestedInFrontOfLoop&);
enum class WhatStandsBesideRead : uint8_t { Number, NumberOrNothing };
bool onlyUserTakesUndefinedForNaNInTypedElements(Walker&, const BodyValue& read, WhatStandsBesideRead = WhatStandsBesideRead::Number);
bool readLeavesTurnAtHoleBehindRow(Walker&, const BodyValue& read);
bool leavesListsTestedInFrontOfLoopAsTheyAre(const Walker&, const Effects&);
bool leavesListsTestedInFrontOfLoopAsTheyAre(const Walker&, const Effects&, std::span<const ListTestedInFrontOfLoop>, const Walker::ObjectsMadeHereThatLoopMentions&);
bool leavesListsTestedInFrontOfLoopAsTheyAre(Walker&, const Effects&, std::span<const ListTestedInFrontOfLoop>, BlockID inputHead);
Vector<ValueID, 2> namesInFrontThatLoopMentions(const Walker&, const BodyIndex&, LoopID, uint32_t firstValueOfLoop, Reference value);
bool readsLengthOfViewTestedInFrontOfLoop(Walker&, const BodyValue& read);

struct ListsReadInLoop {
    Vector<ListTestedInFrontOfLoop, 2> lists;
    bool areLeftAsTheyAre { false };
    bool areTested { false };
    BlockID whereKindIsAnother;
};
void reserveRoomOfListsThatLoopFills(Walker&, BlockID inputHead, std::span<const Reference> entering);
Reference literalThatCallPushesOntoInEveryTurn(Walker&, const BodyValue& call, BlockID inputHead);
ListsReadInLoop findListsReadInLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses);
void buildTestsOfListsReadInLoop(Walker&, BlockID inputHead, std::span<const Reference> entering, ListsReadInLoop&, std::span<const GuessInFrontOfLoop>, std::span<const Reference> proofs, BlockID& closedStart, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
