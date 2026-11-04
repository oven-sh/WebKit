/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// A row of tests in front of a loop has found the list as lists are made, so that nothing above it answers for a number, and with storage of this kind. Nothing has written a list since: KnownNote::ListTestedInFrontOfLoop.
struct ListTestedInFrontOfLoop {
    // The name under which the row has tested it. It stands in front of the loop.
    Reference name;
    // Other: no kind is written, and none was asked for.
    ArrayStorage storage { ArrayStorage::Other };
    // `storage` is Double, and the list may have Int32 storage in its place.
    bool admitsInt32Storage { false };
};

// ---- For buildArrayReadOfTypedElements(), which builds and is asked ahead
// Of an ElemGet of the body that the walk stands in. THE LOOKS AT LOOPS AND THE WALK ASK THIS ONE FUNCTION. A look asks in front of the loop:
// what it is told holds in every turn only where leavesListsTestedInFrontOfLoopAsTheyAre() says so of the effects that the look ends with, and LooksAtLoops::preview() asks that.
std::optional<ListTestedInFrontOfLoop> findListTestedInFrontOfLoop(Walker&, const BodyValue& read);
void nameKindOfListTestedInFrontOfLoop(Draft& read, const ListTestedInFrontOfLoop&);
// GetArrayStorage and GetArrayLength of the list become operands of the read where build() has put them in front of the turns.
void takeStorageAndLengthFromInFrontOfLoop(Walker&, Draft& read, const ListTestedInFrontOfLoop&);
// `if (i < list.length) list[i]`: the position is an index below the length that the list has where the walk stands.
bool positionIsKnownToLieInsideList(Walker&, Reference list, Reference position);

// ---- For the looks at loops
// What the walk binds to the read where findListTestedInFrontOfLoop() answers: the row of GetArrayElement, with the kind that was tested. Null: the read is no such read, and the row of the ElemGet answers.
TypeID typeReadOutOfListTestedInFrontOfLoop(Walker&, const BodyIndex&, const BodyValue& read, TypeID typeOfList, TypeID typeOfPosition);
bool leavesListsTestedInFrontOfLoopAsTheyAre(const Effects&);

// ---- For Walker::buildLoopTwice()
// While it lives the lists are noted as tested, for a look at the loop as the look at the typed copy will be. The tests are not built yet.
class ListsNotedAsTestedForLook {
    WTF_MAKE_NONCOPYABLE(ListsNotedAsTestedForLook);
public:
    ListsNotedAsTestedForLook(Walker&, std::span<const ListTestedInFrontOfLoop>);
    ~ListsNotedAsTestedForLook();

private:
    Walker& m_walker;
    std::span<const ListTestedInFrontOfLoop> m_lists;
};
struct ListsReadInLoop {
    // `name`: as the list is named in front of the row.
    Vector<ListTestedInFrontOfLoop, 2> lists;
    // A look at the loop has found that, with the lists tested, no turn writes a list or runs other code.
    bool areLeftAsTheyAre { false };
    // buildTestsOfListsReadInLoop() has built the row and told the state.
    bool areTested { false };
};
// The lists from in front of the loop that the turns read under an integer. It builds nothing. A list that may be undefined or null gets a guess of its own, so that the row has a name for it that is a list.
// No guess was found: the lists are listed only where a look says that the loop leaves them as they are, because the loop is then built twice for their sake alone.
ListsReadInLoop findListsReadInLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses);
// Behind the tests of the guesses. Each list is asked whether it is as made and of the kind that is written, each test leads to `closedStart` where it fails, and behind the last one the state is told.
// Nothing is built where a look with the proofs bound finds that some turn writes a list or runs other code.
void buildTestsOfListsReadInLoop(Walker&, BlockID inputHead, std::span<const Reference> entering, ListsReadInLoop&, std::span<const GuessInFrontOfLoop>, std::span<const Reference> proofs, BlockID& closedStart, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
