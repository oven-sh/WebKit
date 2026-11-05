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

struct KnownEffectsOfScan {
    struct WritesOfBlock {
        uint32_t block { 0 };
        LocationMasks writes;
    };

    struct Edge {
        uint32_t from { 0 };
        uint32_t to { 0 };
    };

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

    Vector<WritesOfBlock> writesOfBlocks;
    LocationMasks writesOnEveryWay;
    LocationMasks writesNoted;
    Vector<Edge> edges;
    Vector<uint32_t> blocksThatReturn;
    bool someBlockEndsWithoutReturning { false };
    bool whatIsLeftApartMayThrow { false };

    Effects ofOperators { Effects::none() };
    Vector<Operand> operandsOfOperators;
    bool operatorsStayInOwnEffects { false };

    Vector<ValueID> sitesThatWriteIntoObjects;

    Vector<ValueID> sitesOfChanges;
    Vector<uint32_t> sourcesOfOperands;
    Vector<bool> leadsToReturn;
    Vector<bool> parametersLookedAt;
    Vector<uint32_t> parametersToLookAt;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
