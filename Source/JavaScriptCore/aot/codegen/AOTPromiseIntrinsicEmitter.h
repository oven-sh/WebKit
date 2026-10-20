/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class PromiseIntrinsicEmitter final : public OperationEmitter {
public:
    explicit PromiseIntrinsicEmitter(CodeGenerator&);

    void generateNewPromise(const ValueContext&);
    void generateCreatePromise(const ValueContext&);
    void generateResolvePromise(const ValueContext&);
    void generateRejectPromise(const ValueContext&);

    CellPointer newPromise(const OperationContext&);
    Condition hasPromiseStructure(CellPointer);
    void branchUnlessFulfillable(const OperationContext&, CellPointer promise, Boxed value, const Type& valueType, LBasicBlock otherwise);
    void fulfillWithoutBarrier(CellPointer promise, Boxed value);
    void fulfillAtStore(const ValueContext& store, CellPointer promise, Boxed value, const Type& valueType);

private:
    Condition isPromiseConstructor(Boxed);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
