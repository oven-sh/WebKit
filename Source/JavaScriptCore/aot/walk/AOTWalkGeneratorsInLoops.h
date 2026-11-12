/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTCode.h"
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class Walker;
struct LinkedCall;

inline constexpr OpcodeSet opcodesOfGeneratorsInLoops { Opcode::Call, Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread, Opcode::InitialYield, Opcode::Yield, Opcode::PropGet, Opcode::InstanceOf };

struct GeneratorTakenByConsumer {
    Reference holderOfReturn;
    ValueID stepInFront;
    uint32_t inlineFrame { 0 };
    ValueID taker;
    ValueID step;
    BlockID inputHead;
    FunctionID function;
    uint32_t numberOfYields { 0 };
    uint32_t callPosition { 0 };
    uint64_t numberOfOperationsOfRegion { 0 };
    ValueID call;
    uint32_t frameOfCall { 0 };
    bool regionHoldsIterations { false };
    bool hasBackEdge { false };
    bool takerCallsMethodItself { false };
    bool isSteppedByHand { false };
    bool headPrecedesLoop { false };
    std::optional<uint32_t> placeOfResult;
    BlockID blockOfStep;
    Vector<BlockID, 4> blocksBeforeStep;
    uint32_t numberOfPatternSteps { 0 };
    ValueID restArray;
    Vector<ValueID, 4> stepsInRow;
    Reference state;
    Vector<Reference, 8> entryArguments;
    Vector<BlockID, 8> region;
    Vector<ValueID, 8> valuesReadBehindRegion;
    Vector<ValueID, 8> valuesThatRegionNamesFromOutside;
};

struct LoopThatStepsByHand {
    BlockID inputHead;
    BlockID blockOfStep;
    ValueID step;
    ValueID generator;
    ValueID stepInFront;
    std::optional<uint32_t> placeOfResult;
    Vector<BlockID, 4> blocksBeforeStep;
    bool frontIsSeen { false };
    bool loopIsSeenBeforeStep { false };
};

struct StepsInRow {
    ValueID generator;
    Vector<ValueID, 4> steps;
    bool frontIsSeen { false };
};

struct GeneratorWalkedInConsumer {
    Reference holderOfReturn;
    bool countsSteps { false };
    bool headPrecedesLoop { false };
    uint32_t frameOfConsumer { 0 };
    uint32_t frameOfGenerator { 0 };
    uint32_t frameOfTurn { UINT32_MAX };
    uint32_t numberOfLoopsAroundTurn { 0 };
    ValueID call;
    uint32_t frameOfCall { 0 };
    BlockID inputHead;
    ValueID step;
    Reference state;
    Reference list;
    uint32_t numberOfYields { 0 };
    uint64_t numberOfOperationsOfRegion { 0 };
    bool regionHoldsIterations { false };
    bool isSteppedByHand { false };
    std::optional<uint32_t> placeOfResult;
    BlockID blockOfStep;
    Vector<BlockID, 4> blocksBeforeStep;
    uint32_t numberOfPatternSteps { 0 };
    Vector<ValueID, 4> stepsInRow;
    Vector<ValueID, 4> valuesKeptOverSteps;
    Vector<uint32_t, 4> inlineFramesOfSteps;
    uint32_t inlineFrameOfCall { 0 };
    uint32_t inlineFrameOfStep { 0 };
    Vector<BlockID, 8> region;
    Vector<TypeID, 4> typesOfCarriedValues;
    Vector<BlockID, 2> headsThatCarryValues;
    Vector<BlockID, 2> inputHeadsThatCarryValues;
    Vector<Vector<Reference, 4>, 2> carriedAtReturns;
    uint32_t namesAtHead { 0 };
    Vector<std::pair<ValueID, Reference>, 8> bindingsBeforeLoop;
    ValueID yieldStoodAt;
};

std::optional<LoopThatStepsByHand> findLoopThatStepsByHand(const Program&, const BodyIndex&, ValueID generatorCall);
bool onlyStepsParameter(const Program&, const BodyIndex&, ValueID parameter);
std::optional<StepsInRow> findStepsInRow(const Program&, const BodyIndex&, ValueID generatorCall);
bool callMayEnterCopyOfBody(Walker&, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands);
Vector<Reference, 8> buildEntryArgumentsOfCopy(Walker&, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands);
bool operationsFitWindowOfCallWrittenInLoop(const Walker&, uint64_t numberOfOperations);
bool calleeIsCopiedInLoopsAlone(const Walker&, FunctionID callee, size_t numberOfOperands);
bool nobodyLooksAboveGeneratorsOf(const Walker&, FunctionID generatorFunction, uint32_t* numberOfListedCalls);
bool nobodyLooksAboveGeneratorsOf(const Walker&, FunctionID generatorFunction, uint32_t* numberOfListedCalls, const ScopedLambda<bool(const BodyIndex&, const LinkedCall&, FunctionID caller)>& passesForIteration);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
