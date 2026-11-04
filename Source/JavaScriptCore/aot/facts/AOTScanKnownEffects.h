/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTID.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// What ScanOfBody keeps of the effects of the body that it reads, beside ThinSummary::ownEffects. AOTScanKnownEffectsInlines.h fills it and draws the summary's fields from it.
struct KnownEffectsOfScan {
    struct WritesOfBlock {
        uint32_t block { 0 };
        LocationMasks writes;
    };

    struct Edge {
        uint32_t from { 0 };
        uint32_t to { 0 };
    };

    // A list of the scan: where it begins among the atoms, and how many there are.
    struct Operand {
        uint32_t first { 0 };
        uint32_t count { 0 };
    };

    void reset()
    {
        writesOfBlocks.shrink(0);
        writesOnEveryWay = { };
        writesNoted = { };
        edges.shrink(0);
        blocksThatReturn.shrink(0);
        someBlockEndsWithoutReturning = false;
        whatIsLeftApartMayThrow = false;
        ofOperators = Effects::none();
        operandsOfOperators.shrink(0);
        operatorsStayInOwnEffects = false;
        sitesThatWriteIntoObjects.shrink(0);
    }

    // One block may stand there more than once.
    Vector<WritesOfBlock> writesOfBlocks;
    // Of what no block is known for.
    LocationMasks writesOnEveryWay;
    // All of the two above. Where ThinSummary::ownEffects has come to write more, somebody has merged into it whom this file has not heard of, and nothing is told apart.
    LocationMasks writesNoted;
    // Ascending by `from`.
    Vector<Edge> edges;
    Vector<uint32_t> blocksThatReturn;
    // A Throw, a Rethrow, an Unreachable. Without one every block that is entered leads to a Return or goes round for ever.
    bool someBlockEndsWithoutReturning { false };
    // ThinSummary::ownEffects does not say so any more, and what is thrown has to be counted on all the same.
    bool whatIsLeftApartMayThrow { false };

    Effects ofOperators { Effects::none() };
    Vector<Operand> operandsOfOperators;
    // Some operand comes from where nobody knows, so Link could only say what the scan says.
    bool operatorsStayInOwnEffects { false };

    // The operations that are no accesses and write in heapsOfPartsOfObjects.
    Vector<ValueID> sitesThatWriteIntoObjects;

    // ---- Behind the body.
    // Ascending. And of the stores and definitions, those into contents too: the subject of each says whose object is written.
    Vector<ValueID> sitesOfChanges;
    // ThinSummary::KnownEffectsApart::Lists::operandsOfOperatorsLeftApart, while nobody knows whether the operators are left apart.
    Vector<uint32_t> sourcesOfOperands;
    // By block.
    Vector<bool> leadsToReturn;
    // By parameter of a block.
    Vector<bool> parametersLookedAt;
    Vector<uint32_t> parametersToLookAt;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
