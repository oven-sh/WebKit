/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class FunctionBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit FunctionBuiltinEmitter(CodeGenerator&);

    void generateFunctionBind(const InlineCall&);
    void generateReflectApply(const InlineCall&);
    void generateFunctionApply(const InlineCall&);

private:
    void missUnlessCallableClosure(const InlineCall&, CellPointer target, const Type&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
