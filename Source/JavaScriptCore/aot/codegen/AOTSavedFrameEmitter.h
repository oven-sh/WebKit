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
    void generateIsUnmodifiedAsyncGenerator(const ValueContext&);
    void generateStepAsyncGenerator(const ValueContext&);
    void generateAwaitBySavingFrame(const ValueContext&);
    void generateAwaitCallBySavingFrame(const ValueContext&);
    void generateYieldBySavingFrame(const ValueContext&);
    void generateAsyncGeneratorYield(const ValueContext&);
    void generateReturnAsyncResult(const ValueContext&);
    void generatePromiseAllOfElements(const ValueContext&);

    void generateStepGenerator(const ValueContext&);
    void generateIsUnmodifiedGenerator(const ValueContext&);

private:
    struct Suspension {
        StubKey key;
        LValue firstOperand { nullptr };
        LValue secondOperand { nullptr };
        ValueID driver;
    };

    CoroutineKind kindOfFunction() const;
    CellPointer newGenerator(const ValueContext&, CoroutineKind);
    LValue cellOrZero(ValueID driver);
    StubCallResult suspend(const ValueContext&, const Suspension&);
    void suspendGenerator(const ValueContext&, GeneratorSuspension, Boxed value);
    void defineYieldResults(const ValueContext&, StubCallResult);
    void branchUnlessUnmodifiedGenerator(Operand, const Type&, LBasicBlock otherwise);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
