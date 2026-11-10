/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTCode.h"

namespace JSC { namespace AOT { namespace Typed {

class Walker;

inline constexpr OpcodeSet opcodesOfGeneratorsInLoops { Opcode::Call, Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread, Opcode::InitialYield, Opcode::Yield };

struct GeneratorTakenByConsumer {
    uint32_t inlineFrame { 0 };
    ValueID taker;
    ValueID step;
    BlockID inputHead;
    FunctionID function;
    uint32_t numberOfYields { 0 };
    uint32_t callPosition { 0 };
    uint64_t numberOfOperationsOfRegion { 0 };
    bool regionHoldsIterations { false };
    bool hasBackEdge { false };
    bool takerCallsMethodItself { false };
    Reference state;
    Vector<Reference, 8> entryArguments;
    Vector<BlockID, 8> region;
    Vector<ValueID, 8> valuesReadBehindRegion;
    Vector<ValueID, 8> valuesThatRegionNamesFromOutside;
};

struct GeneratorWalkedInConsumer {
    uint32_t frameOfConsumer { 0 };
    uint32_t frameOfGenerator { 0 };
    uint32_t frameOfTurn { UINT32_MAX };
    uint32_t numberOfLoopsAroundTurn { 0 };
    BlockID inputHead;
    ValueID step;
    Reference state;
    Reference list;
    uint32_t numberOfYields { 0 };
    uint64_t numberOfOperationsOfRegion { 0 };
    bool regionHoldsIterations { false };
    Vector<BlockID, 8> region;
    Vector<TypeID, 4> typesOfCarriedValues;
    Vector<BlockID, 2> headsThatCarryValues;
    Vector<BlockID, 2> inputHeadsThatCarryValues;
    Vector<Vector<Reference, 4>, 2> carriedAtReturns;
    uint32_t namesAtHead { 0 };
    Vector<std::pair<ValueID, Reference>, 8> bindingsBeforeLoop;
    ValueID yieldStoodAt;
};

bool callMayEnterCopyOfBody(Walker&, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands);
Vector<Reference, 8> buildEntryArgumentsOfCopy(Walker&, const Draft& site, FunctionID callee, std::span<const Reference> passedOperands);
bool operationsFitWindowOfCallWrittenInLoop(const Walker&, uint64_t numberOfOperations);
bool calleeIsCopiedInLoopsAlone(const Walker&, FunctionID callee, size_t numberOfOperands);
bool nobodyLooksAboveGeneratorsOf(const Walker&, FunctionID generatorFunction, uint32_t* numberOfListedCalls);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
