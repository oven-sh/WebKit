/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

struct HoistedCounterCheck {
    uint32_t position { 0 };
    TypeID tested;
    bool isCheckedOnBackEdge { false };
    bool testMayWellFail { false };
    bool isSum { false };
};

struct AccessUnderCounterOfLoop {
    ValueID access;
    ValueID view;
    ValueID addend;
    bool isOfList { false };
};

struct CounterHeldBelowRoomOfViews {
    uint32_t position { 0 };
    ValueID relation;
    ValueID bound;
    ValueID viewWhoseLengthIsBound;
    ValueID stepThatMayBeNegative;
    Vector<AccessUnderCounterOfLoop, 4> accesses;
};

CounterHeldBelowRoomOfViews findCounterHeldBelowRoomOfViews(Walker&, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(uint32_t position)>& typeThatEnters, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront, NOESCAPE const ScopedLambda<bool(ValueID)>& isListOnlyReadInLoop);
BitVector namesOfCountersAskedForInt32InFrontOfLoop(Walker&, BlockID inputHead, std::span<const Reference> entering);
Vector<HoistedCounterCheck, 2> findHoistedCounterChecks(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses);
bool isNumberCarriedBesideTextOrBigInt(const Walker&, Reference enters, TypeID carried);

struct HoistedVariableCheck {
    VariableID variable;
    TypeID tested;
    StatisticID counted;
};
void guessDivisorsOfIntegersInFrontOfLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables);

enum class WhatEntersHead : uint8_t { IsInteger, MayBeAskedInFront };
struct NumberThatDivisionsBringRound {
    Range integers;
    bool isDividedInFront { true };
    Vector<ValueID, 2> remainders;
};
std::optional<NumberThatDivisionsBringRound> findNumberThatDivisionsBringRound(Walker&, BlockID inputHead, uint32_t position, std::span<const TypeID> entering, WhatEntersHead);
bool operatorsOnBitsAloneBringRound(Walker&, BlockID inputHead, uint32_t position, std::span<const TypeID> entering);

struct LoopBoundComparison {
    ValueID relation;
    ValueID number;
    VariableID variable;
};
LoopBoundComparison findLoopBoundComparison(Walker&, BlockID inputHead, const Effects& ofTurns, std::span<const Reference> entering, std::span<const HoistedCounterCheck> askedInFront);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
