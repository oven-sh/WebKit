/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStubsCoroutines.h"

namespace JSC { namespace AOT { namespace Typed {

class SavedFrameEmitter final : public OperationEmitter {
public:
    explicit SavedFrameEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateNewGenerator(const ValueContext&);
    void generateNewAsyncGenerator(const ValueContext&);
    void generateTakeGeneratorPrototype(const ValueContext&);
    void generateIsGeneratorUnstarted(const ValueContext&);
    void generateIsGeneratorRunning(const ValueContext&);
    void generateMarkGeneratorRunning(const ValueContext&);
    void generateTakeGeneratorStepStatus(const ValueContext&);
    void generateIsUnmodifiedAsyncGenerator(const ValueContext&);
    void generateStepAsyncGenerator(const ValueContext&);
    void generateAwaitMayContinue(const ValueContext&);
    void generateAwaitBySavingFrame(const ValueContext&);
    void generateAwaitCallBySavingFrame(const ValueContext&);
    void generateYieldBySavingFrame(const ValueContext&);
    // AOTSavedFrameEmitterLeaving.cpp
    void generateLeave(const ValueContext&);
    void generateLeaveCopies(const ValueContext&);
    void leaveForSlowBodies(const ValueContext&, bool comesBack);
    void generateAsyncGeneratorYield(const ValueContext&);
    void generateReturnAsyncResult(const ValueContext&);
    void generatePromiseAllOfElements(const ValueContext&);

    void generateStepGenerator(const ValueContext&);
    void generateIsUnmodifiedGenerator(const ValueContext&);

    // Values of a long async function that live in words of the heap, so that a suspension does not copy them.
    void generateNewHeapWords(const ValueContext&);
    void generateGetHeapFrameWord(const ValueContext&);
    void generatePutHeapFrameWord(const ValueContext&);

    // What lives across a yield of a generator lies in the words of a heap frame: a yield is a few stores and a return, and a step is a call of the resume entry.
    void generateNewHeapFrame(const ValueContext&);
    void generateGetResumeIndex(const ValueContext&);
    void generateSuspendInHeapFrame(const ValueContext&);

private:
    CoroutineKind kindOfFunction() const;
    CellPointer allocateHeapFrame(uint32_t numberOfWords, Pointer resumeAddress, LBasicBlock slowPath);
    CellPointer allocateGenerator(std::optional<CellPointer> closure, LBasicBlock slowPath);
    LValue cellOrZero(ValueID driver);
    LValue driverAsPassed(ValueID driver);
    TypedPointer stateOf(CellPointer generator);
    Pointer framePointerAtThisPoint();
    enum class RunsCodeOfAwaited : bool { InFrameOfFunction, BehindFunction };
    StubCallResult suspend(const ValueContext&, StubKey, std::initializer_list<LValue> operandsInFrontOfSiteWord, ValueID driver, RunsCodeOfAwaited = RunsCodeOfAwaited::InFrameOfFunction);
    void suspendGenerator(const ValueContext&, GeneratorSuspension);
    void defineValueAndWord(const ValueContext&, StubCallResult);
    void branchUnlessUnmodifiedGenerator(Operand, const Type&, LBasicBlock otherwise);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
