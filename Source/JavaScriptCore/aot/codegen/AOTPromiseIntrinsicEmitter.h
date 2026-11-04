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
    explicit PromiseIntrinsicEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateNewPromise(const ValueContext& value) { define(value, newPromise(value)); }
    void generateCreatePromise(const ValueContext&);
    void generateResolvePromise(const ValueContext&);
    void generateRejectPromise(const ValueContext& value) { callVoidOperation(value, RuntimeOperation::RejectPromise, { in<CellPointer>(value->promiseOperand()), in<Boxed>(value->valueOperand()) }); }

    CellPointer newPromise(const OperationContext&);
    Condition hasPromiseStructure(CellPointer);
    void fulfillWithoutBarrier(CellPointer promise, Boxed value);
    void fulfillAtStore(const ValueContext& store, CellPointer promise, Boxed value, const Type& valueType);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
