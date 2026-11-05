/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

struct ListTestedInFrontOfLoop {
    Reference name;
    ArrayStorage storage { ArrayStorage::Other };
    bool admitsInt32Storage { false };
    bool isWritable { false };
    bool turnsStoreIntoLists { false };
};

std::optional<ListTestedInFrontOfLoop> findListTestedInFrontOfLoop(Walker&, const BodyValue& read);
std::optional<ListTestedInFrontOfLoop> findListTestedInFrontOfLoop(Walker&, Reference list);
void nameKindOfListTestedInFrontOfLoop(Draft& read, const ListTestedInFrontOfLoop&);
void takeStorageAndLengthFromInFrontOfLoop(Walker&, Draft& read, const ListTestedInFrontOfLoop&);
bool positionIsKnownToLieInsideList(Walker&, Reference list, Reference position);
Reference storageOfListFromInFrontOfLoop(Walker&, const ListTestedInFrontOfLoop&, Origin);

bool holeLeavesTurnBehindRow(const BodyIndex&, const BodyValue& read, const ListTestedInFrontOfLoop&);
TypeID typeReadOutOfListTestedInFrontOfLoop(Walker&, const BodyIndex&, const BodyValue& read, TypeID typeOfList, TypeID typeOfPosition);
bool leavesListsTestedInFrontOfLoopAsTheyAre(const Effects&);

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
    Vector<ListTestedInFrontOfLoop, 2> lists;
    bool areLeftAsTheyAre { false };
    bool areTested { false };
};
ListsReadInLoop findListsReadInLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses);
void buildTestsOfListsReadInLoop(Walker&, BlockID inputHead, std::span<const Reference> entering, ListsReadInLoop&, std::span<const GuessInFrontOfLoop>, std::span<const Reference> proofs, BlockID& closedStart, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
