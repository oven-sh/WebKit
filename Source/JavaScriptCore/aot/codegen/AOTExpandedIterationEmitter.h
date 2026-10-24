/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ExpandedIterationEmitter final : public OperationEmitter {
public:
    explicit ExpandedIterationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateDestructureIterable(const ValueContext&);
    void generateEnumeratorGetByVal(const ValueContext&);
    void generateRequireAttachedTypedArray(const ValueContext&);
    void generateCreateAsyncFromSyncIterator(const ValueContext&);
    void generateThrowNotIterable(const ValueContext& value) { callVoidOperation(value, RuntimeOperation::ThrowNotIterable, { in<Boxed>(value->iterableOperand()), word32(value->isSpread()) }); }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
