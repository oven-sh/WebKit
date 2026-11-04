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

// A TYPE IS NUMBERED AS IT ARRIVES IN THE PROGRAM's TABLE, so one thread writes a table that holds types, position by position: TurnsOfReadings. BUT MOST POSITIONS MAKE NO TYPE: they hold what another holds.
// A loop over positions that are written each by itself goes in three rounds:
//   ALL THREADS LOOK       each writes its positions with an arena that keeps what is new to itself, and notes the positions at which that arena made something
//   ONE THREAD MAKES       it writes those positions again, in their order, with the arena of the turn. The first position that needs a type is among them, whichever thread looked at it: that thread had not made the type before.
//                          What else is among them finds its types made. So the types arrive in the table as they do where one thread writes every position, at any number of threads
//   ALL THREADS WRITE      the other positions. Nothing is new by then: an arena that makes something stops the build
// `write` hears a position up to twice, on any thread, and writes nothing but what belongs to the position. What it makes it makes with the arena and the memo that `putToWork` has handed the thread; null: those of the turn.
struct TypesOfThreadInLink {
    TypeArena* types { nullptr };
    TypesOfFlowTurnedOnce* turned { nullptr };
};

void writeTypedPositionsOnAllThreads(const LinkFlow& flow, TypeTable& table, unsigned numberOfPositions, const Invocable<void(TypesOfThreadInLink)> auto& putToWork, const Invocable<void(unsigned position)> auto& write)
{
    const ParallelLoops& loops = flow.loopsOfReading();
    // Two rounds on all threads cost what one thread spends twice over: with fewer than four nothing is won.
    if (loops.width() < 4 || ParallelLoops::isInsideLoop()) {
        for (unsigned position = 0; position < numberOfPositions; ++position)
            write(position);
        return;
    }

    static constexpr unsigned positionsInStretch = 256;
    unsigned numberOfStretches = (numberOfPositions + positionsInStretch - 1) / positionsInStretch;
    LinkFlow::ViewOfHeldReading* view = LinkFlow::ViewAtWork::current();
    struct OfWorker {
        std::unique_ptr<TypeArena> types;
        TypesOfFlowTurnedOnce turned;
    };
    auto onAllThreads = [&](const auto& writeWith) {
        Vector<OfWorker> workers(loops.width());
        forEachIndexInParallel(loops, numberOfStretches, [&](unsigned stretch, unsigned worker) {
            LinkFlow::ViewAtWork viewAtWork(view);
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

    Vector<uint8_t> makesTypes(FillWith { }, numberOfPositions, uint8_t { 0 });
    onAllThreads([&](unsigned position, const TypeArena& types) {
        size_t before = types.numberMadeAndKeptToItself();
        write(position);
        makesTypes[position] = types.numberMadeAndKeptToItself() != before;
    });
    for (unsigned position = 0; position < numberOfPositions; ++position) {
        if (makesTypes[position])
            write(position);
    }
    onAllThreads([&](unsigned position, const TypeArena& types) {
        if (makesTypes[position])
            return;
        write(position);
        RELEASE_ASSERT(!types.numberMadeAndKeptToItself());
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
