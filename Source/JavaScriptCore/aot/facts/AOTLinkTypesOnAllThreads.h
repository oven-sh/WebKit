/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"
#include "AOTLinkTypesTurnedOnce.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

struct TypesOfThreadInLink {
    TypeArena* types { nullptr };
    TypesOfFlowTurnedOnce* turned { nullptr };
};

inline bool isOfProgramInLink(TypeID type) { return !liesInThread(type); }

void writeTypedPositionsOnAllThreads(const LinkFlow& flow, TypeTable& table, unsigned numberOfPositions, const Invocable<void(TypesOfThreadInLink)> auto& putToWork, const auto& write)
{
    const ParallelLoops& loops = flow.loopsOfReading();
    if (loops.width() < 4 || ParallelLoops::isInsideLoop()) {
        for (unsigned position = 0; position < numberOfPositions; ++position)
            write(position);
        return;
    }

    static constexpr unsigned positionsInStretch = 256;
    unsigned numberOfStretches = (numberOfPositions + positionsInStretch - 1) / positionsInStretch;
    struct OfWorker {
        std::unique_ptr<TypeArena> types;
        TypesOfFlowTurnedOnce turned;
    };
    auto onAllThreads = [&](const auto& writeWith) {
        TypeTable::RoundWithoutAdditions nobodyAdds(table);
        Vector<OfWorker> workers(loops.width());
        forEachIndexInParallel(loops, numberOfStretches, [&](unsigned stretch, unsigned worker) {
            OfWorker& own = workers[worker];
            if (!own.types)
                own.types = makeUniqueWithoutFastMallocCheck<TypeArena>(table, ArenaKeeps::UntilReset);
            putToWork(TypesOfThreadInLink { own.types.get(), &own.turned });
            unsigned end = std::min(numberOfPositions, (stretch + 1) * positionsInStretch);
            for (unsigned position = stretch * positionsInStretch; position < end; ++position)
                writeWith(position, *own.types);
            putToWork({ });
        });
    };

    auto holdsTypesOfProgramAlone = [&](unsigned position) {
        if constexpr (std::is_void_v<decltype(write(position))>) {
            write(position);
            return false;
        } else
            return write(position);
    };
    enum : uint8_t { IsWritten, MakesTypes, HoldsTypesOfThread };
    Vector<uint8_t> states(FillWith { }, numberOfPositions, uint8_t { IsWritten });
    std::atomic<bool> someHoldTypesOfThread { false };
    onAllThreads([&](unsigned position, const TypeArena& types) {
        size_t before = types.numberMadeAndKeptToItself();
        bool isOfProgram = holdsTypesOfProgramAlone(position);
        if (types.numberMadeAndKeptToItself() != before)
            states[position] = MakesTypes;
        else if (!isOfProgram) {
            states[position] = HoldsTypesOfThread;
            someHoldTypesOfThread.store(true, std::memory_order_relaxed);
        }
    });
    for (unsigned position = 0; position < numberOfPositions; ++position) {
        if (states[position] == MakesTypes)
            write(position);
    }
    if (!someHoldTypesOfThread.load(std::memory_order_relaxed))
        return;
    onAllThreads([&](unsigned position, const TypeArena& types) {
        if (states[position] != HoldsTypesOfThread)
            return;
        write(position);
        RELEASE_ASSERT(!types.numberMadeAndKeptToItself());
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
