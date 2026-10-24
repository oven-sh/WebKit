/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTStubsCoroutines.h"

namespace JSC { namespace AOT { namespace Typed {

class PromiseBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit PromiseBuiltinEmitter(CodeGenerator&);

    void generatePromiseResolve(const InlineCall&);
    void generatePromiseThen(const InlineCall&);
    void generatePromiseCatch(const InlineCall&);

private:
    Boxed argumentOrUndefined(const InlineCall&, unsigned index);
    bool isFunction(const InlineCall&, unsigned index);
    bool isNoObject(const InlineCall&, unsigned index);
    CellPointer thenWithOneHandler(const InlineCall&, PromiseHandler, unsigned index);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
