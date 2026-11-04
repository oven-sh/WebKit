/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

// For whoever leaves a block otherwise than it is written. The door itself is Walker::endBlockLeadingTo().

// Given that nobody reads what it gives, nobody can tell whether the operation of the body being read has run: by its declared effects as written. Never: an operation without a result, one bound to a frame, a function or a site,
// one that states a type, a Builtin whose first read is seen (firstReadOfNamedBuiltinIsSeen()).
bool isUnseenWhenUnused(const Walker&, const BodyValue&); // AOTWalkBranches.cpp

inline constexpr uint32_t mostBlocksPassedBehindTerminator = 3;

// Where an edge comes to: through blocks that one edge enters, that jump on without a proof, and in which every operation is a NarrowByField or is unused and isUnseenWhenUnused().
struct WayBehindTerminator {
    BlockID from;
    uint32_t targetIndex { 0 };
    BodyTarget target;
    std::array<BlockID, mostBlocksPassedBehindTerminator> passed;
    uint32_t numberOfPassedBlocks { 0 };
    // The target of the jump out of the last of them, or else `target`.
    BodyTarget arrival;
};
WayBehindTerminator wayBehindTarget(const Walker&, BlockID from, uint32_t targetIndex, BodyTarget); // AOTWalkBranches.cpp

// Whichever of the two a run takes, it comes to the same block with the same values (behind casts; a proof brings what it narrows), and nothing has happened on the way.
bool comeToSamePlace(const BodyIndex&, const WayBehindTerminator&, const WayBehindTerminator&); // AOTWalkBranches.cpp

struct CasesLeftOfSwitch {
    // Numbers of written targets, ascending.
    Vector<uint32_t, 16> cases;
    // The written target that takes whatever matches none of `cases`.
    uint32_t rest { 0 };
};
// `asWritten`: the terminator with its subject as built. A case is left out where narrowSubjectByCase() leaves nothing of the proven type of the subject, or where it comes to the same place as the rest.
CasesLeftOfSwitch casesLeftOfSwitch(Walker&, const BodyTerminator&, const TerminatorDraft& asWritten); // AOTWalkBranchesSwitches.cpp

// Role CatchHandler, and each CatchValue | CatchException at its head is unused.
bool readsNothingOfWhatItCatches(const BodyIndex&, BodyBlock handler); // AOTWalkExceptions.cpp

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
