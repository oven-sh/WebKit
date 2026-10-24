/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class EnvironmentEmitter final : public OperationEmitter {
public:
    explicit EnvironmentEmitter(CodeGenerator&);

    void generateGetCapture(const ValueContext&);
    void generateMaterializeEnvironment(const ValueContext&);

    Operand closureCapture(CellPointer closure, FunctionID, unsigned index);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
