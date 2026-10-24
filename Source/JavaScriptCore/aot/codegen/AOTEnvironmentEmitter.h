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
    void generateMarkEnvironmentEscaped(const ValueContext&);
    void generateReleaseEnvironment(const ValueContext&);

    Operand closureCapture(CellPointer closure, FunctionID, unsigned index);

private:
    TypedPointer escapeMarkOf(CellPointer environment, uint32_t isEscapedOffset);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
