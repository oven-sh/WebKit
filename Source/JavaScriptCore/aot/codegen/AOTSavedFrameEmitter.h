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
    explicit SavedFrameEmitter(CodeGenerator&);

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
    void generateAsyncGeneratorYield(const ValueContext&);
    void generateReturnAsyncResult(const ValueContext&);
    void generatePromiseAllOfElements(const ValueContext&);

    void generateStepGenerator(const ValueContext&);
    void generateIsUnmodifiedGenerator(const ValueContext&);

private:
    // The operands of the stub in front of the site word.
    struct Suspension {
        StubKey key;
        LValue firstOperand { nullptr };
        LValue secondOperand { nullptr };
        LValue thirdOperand { nullptr };
        ValueID driver;
    };

    CoroutineKind kindOfFunction() const;
    CellPointer newGenerator(const ValueContext&, CoroutineKind);
    CellPointer allocateGenerator(CellPointer closure, LBasicBlock slowPath);
    LValue cellOrZero(ValueID driver);
    LValue driverAsPassed(ValueID driver);
    TypedPointer stateOf(CellPointer generator);
    Pointer framePointerAtThisPoint();
    StubCallResult suspend(const ValueContext&, const Suspension&);
    void suspendGenerator(const ValueContext&, GeneratorSuspension, Boxed value);
    void defineYieldResults(const ValueContext&, StubCallResult);
    void branchUnlessUnmodifiedGenerator(Operand, const Type&, LBasicBlock otherwise);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
