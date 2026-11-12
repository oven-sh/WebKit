/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

struct HoistedCounterCheck {
    uint32_t position { 0 };
    TypeID tested;
    bool isCheckedOnBackEdge { false };
    bool testMayWellFail { false };
    bool isSum { false };
};

struct AddendOfKey {
    ValueID value;
    bool isSubtracted { false };
};

struct AccessUnderCounterOfLoop {
    ValueID access;
    ValueID view;
    Reference fieldView;
    VariableID variable;
    Vector<AddendOfKey, 2> addends;
    bool isOfList { false };
    bool iterationChecksAddendSign { false };
};

struct CounterHeldBelowRoomOfViews {
    uint32_t position { 0 };
    ValueID relation;
    ValueID bound;
    ValueID viewWhoseLengthIsBound;
    ValueID boundElement;
    Reference boundFieldView;
    VariableID boundVariable;
    bool isBoundByVariable { false };
    ValueID stepThatMayBeNegative;
    Vector<AccessUnderCounterOfLoop, 4> accesses;
};

struct AccessBesideCounter {
    ValueID view;
    uint32_t position { 0 };
    Range addends;
    Range steps;
    ValueID step;
    bool isSubtracted { false };
};

struct AccessesNearCounter {
    uint32_t counter { 0 };
    ValueID bound;
    bool counterRises { true };
    bool admitsEqual { false };
    Vector<AccessBesideCounter, 4> accesses;
};

AccessesNearCounter findAccessesNearCounter(Walker&, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(uint32_t position)>& typeThatEnters, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront);
CounterHeldBelowRoomOfViews findCounterHeldBelowRoomOfViews(Walker&, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(uint32_t position)>& typeThatEnters, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront, NOESCAPE const ScopedLambda<TypeID(Reference)>& typeOfNameInFront, NOESCAPE const ScopedLambda<bool(ValueID)>& isListOnlyReadInLoop, NOESCAPE const ScopedLambda<std::optional<int64_t>(ValueID)>& integerInFront, const Effects& ofTurns, ValueID relationBoundByVariable);
struct SteppedKey {
    Reference list;
    uint32_t steppedPosition { 0 };
    uint32_t counterPosition { 0 };
    ValueID bound;
    bool admitsBound { false };
    bool counterRises { false };
};
Vector<SteppedKey, 2> findSteppedKeys(Walker&, BlockID inputHead, NOESCAPE const ScopedLambda<std::optional<Range>(uint32_t)>& findSteps, NOESCAPE const ScopedLambda<Reference(ValueID)>& findTestedArray);
Vector<ValueID, 4> findElementCarriedIndexes(Walker&, BlockID inputHead);
ValueID findUpdatedKey(Walker&, const BodyIndex&, ValueID key);
BitVector namesOfCountersAskedForInt32InFrontOfLoop(Walker&, BlockID inputHead, std::span<const Reference> entering);
bool addendMayBeGuessedInt32(const Walker&, Reference);
BitVector namesOfInnerLoopCounters(Walker&, BlockID inputHead, const LoopPreview&);
BitVector findParametersCheckedForBits(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
Vector<HoistedCounterCheck, 2> findHoistedCounterChecks(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses);
std::optional<HoistedCounterCheck> findCounterOfAnyKind(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering);
bool isNumberCarriedBesideTextOrBigInt(const Walker&, Reference enters, TypeID carried);
TypeID findCarriedNumberType(Walker&, BlockID inputHead, const LoopPreview&, Reference enters, uint32_t position);

struct HoistedVariableCheck {
    VariableID variable;
    TypeID tested;
    StatisticID counted;
    bool isOnlyReckonedWith { false };
    Reference scope { };
    FieldReference field { };
    PropertyKeyID convertedKey { };
};
void hoistKeyFields(Walker&, BlockID inputHead);
void hoistConvertedFields(Walker&, BlockID inputHead);
void speculateOnKeyTerms(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables);
void guessTermsOfRemainders(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, bool mayConvertTexts, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables);
void guessFactorsInVariables(Walker&, BlockID inputHead, const LoopPreview&, Vector<HoistedVariableCheck, 2>& variables);
void guessViewsInVariables(Walker&, BlockID inputHead, Vector<HoistedVariableCheck, 2>& variables);
void findNumbersOnlyReckonedWith(Walker&, BlockID inputHead, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables);
void guessDivisorsOfIntegersInFrontOfLoop(Walker&, BlockID inputHead, const LoopPreview&, std::span<const Reference> entering, Vector<HoistedSpeculation, 4>& guesses, Vector<HoistedVariableCheck, 2>& variables);

enum class WhatEntersHead : uint8_t { IsInteger, MayBeAskedInFront };
struct NumberThatDivisionsBringRound {
    Range integers;
    bool isDividedInFront { true };
    Vector<ValueID, 2> remainders;
};
std::optional<NumberThatDivisionsBringRound> findNumberThatDivisionsBringRound(Walker&, BlockID inputHead, uint32_t position, std::span<const TypeID> entering, WhatEntersHead);
bool operatorsOnBitsAloneBringRound(Walker&, BlockID inputHead, uint32_t position, std::span<const TypeID> entering);
bool searchesNumbersByHalves(Walker&, BlockID inputHead);
bool addsFieldsToIndexes(Walker&, BlockID inputHead, const LoopPreview&);
std::optional<Range> findRangeOfMeetingBound(Walker&, BlockID inputHead, uint32_t position, std::span<const TypeID> entering);
std::optional<Range> findIterationSteps(Walker&, BlockID inputHead, uint32_t position);
struct SummedAddends {
    Range known { Range::one(0) };
    bool hasUnknown { false };
};
std::optional<SummedAddends> findSummedAddends(Walker&, BlockID inputHead, uint32_t position);
bool isSimpleCountedLoop(Walker&, BlockID inputHead);
bool isSummedByUnrolledIterations(Walker&, BlockID inputHead, uint32_t position);
struct InvariantStep {
    ValueID step;
    bool isSubtracted { false };
    Range steps;
};
std::optional<InvariantStep> findInvariantStep(Walker&, BlockID inputHead, uint32_t position);
std::optional<Range> findCounterRange(Walker&, BlockID inputHead, uint32_t position, int64_t start);
bool counterOnlyFalls(Walker&, BlockID inputHead, uint32_t position);

struct EqualityLoopTest {
    uint32_t position { 0 };
    ValueID bound;
    uint32_t exitTarget { 0 };
    bool countsUp { true };
    int64_t largestStep { 1 };
};
std::optional<EqualityLoopTest> findEqualityLoopTest(Walker&, BlockID inputHead);

struct EndLoopTest {
    uint32_t position { 0 };
    BlockID block;
    ValueID stepped;
    Opcode relation { Opcode::Less };
    ValueID bound;
    uint32_t exitTarget { 0 };
    bool isRelation { false };
};
std::optional<EndLoopTest> findEndLoopTest(Walker&, BlockID inputHead);
bool boundsPositions(const Walker&, const EndLoopTest&, Reference bound);
bool benefitsFromHeaderGuard(Walker&, BlockID inputHead, std::span<const Reference> entering);
std::optional<Range> findCounterRange(Walker&, BlockID inputHead, const EqualityLoopTest&, Range entering, bool isExitChecked);
bool counterStaysWithinBound(Walker&, BlockID inputHead, const EqualityLoopTest&, TypeID entering);
bool isMultipliedBelowLength(Walker&, BlockID inputHead, uint32_t position);

struct LoopBoundComparison {
    ValueID relation;
    ValueID number;
    VariableID variable;
};
LoopBoundComparison findLoopBoundComparison(Walker&, BlockID inputHead, const Effects& ofTurns, std::span<const Reference> entering, std::span<const HoistedCounterCheck> askedInFront);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
