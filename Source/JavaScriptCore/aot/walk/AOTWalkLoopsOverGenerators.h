/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

struct RestOfPattern {
    ValueID array;
    ValueID step;
};
bool stepsOfPatternStandInRow(const BodyIndex&, const BodyValue& open, RestOfPattern* = nullptr);

inline constexpr uint32_t maxGeneratorPatternSteps = 4;

struct LoopOverIterator {
    ValueID step;
    BlockID blockOfStep;
    bool stepStandsInHeadOfLoop { false };
};
std::optional<LoopOverIterator> findLoopOverIterator(const BodyIndex&, ValueID open);
void buildFirstReadsThatOpenOfGeneratorLeavesOut(Walker&, Origin, bool asksGeneratorForIterator = true);
void buildFirstReadThatCloseOfGeneratorLeavesOut(Walker&, Origin);
bool buildLoopOverGenerator(Walker&, const BodyValue&);
bool engineAnswersForGenerators(Walker&);
std::optional<BuiltinID> findMemberAboveForIteration(const Walker&, BuiltinID start, BuiltinName, MembersBehindWatch = MembersBehindWatch::AreOpen);
std::optional<BuiltinID> findMemberUnderKey(const Walker&, BuiltinID start, PropertyKeyID, MembersBehindWatch = MembersBehindWatch::AreOpen);
GeneratorStepCheck checkLeftToStepOf(Walker&, Reference generator);
Reference buildStepOfGenerator(Walker&, Reference generator, ResumeMode, Reference sent, GeneratorStepCheck, Origin);
Reference buildResultOfStep(Walker&, Reference value, Reference done, Origin);

struct StepParts {
    Reference answer;
    Reference isAnswered;
    Reference value;
    Reference done;
    bool isRarelyAnswered { false };
};
bool isOnlyReadStep(Walker&, ValueID);
bool hasStepParts(Walker&, Reference);
void appendStepParts(Walker&, Reference, Vector<Reference, 8>& arguments);
void addStepParameters(Walker&, BlockID, Reference whole, bool isRarelyAnswered, Vector<ValueID, 8>& parameters, Origin);
bool isUnlikelyStepResult(Walker&, Reference);
void carryStepParts(Walker&, BlockID inputHead, BlockID header, Vector<Reference, 8>& entering, Vector<ValueID, 8>& parameters, Origin);
void appendCarriedSteps(Walker&, BlockID header, std::span<const Reference> results, Vector<Reference, 8>& arguments);
void appendHeldSteps(Walker&, BlockID header, Vector<Reference, 8>& held);
void addStepEntryParameters(Walker&, BlockID inputHead, BlockID entry, std::span<const Reference> entering, std::span<const Reference> results, Vector<Reference, 8>& arguments);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
