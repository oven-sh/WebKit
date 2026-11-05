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
    explicit EnvironmentEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateGetCapture(const ValueContext&);
    void generateMarkEnvironmentEscaped(const ValueContext&);
    void generateReleaseEnvironment(const ValueContext&);
    void markEnvironmentsThatClosureKeeps(const ValueContext& closure);

    Operand closureCapture(CellPointer closure, FunctionID, unsigned index);
    TypedPointer linkToNextCellHandedBack(CellPointer, LayoutID);

private:
    TypedPointer byteWithEscapeMarkOf(ValueID environment);
    void markEscaped(ValueID environment);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
