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
    void generateMaterializeEnvironment(const ValueContext&);
    void generateMarkEnvironmentEscaped(const ValueContext&);
    void generateReleaseEnvironment(const ValueContext&);
    void generateRecordCallee(const ValueContext&);
    std::optional<uint32_t> stackWordsOfHandOverFromListFrame() const;
    void generateKeepCalleeInFrame(const ValueContext& value) { keepInFrame(value, in<CellPointer>(value->closureOperand())); }
    void generateKeepEnvironmentOfCalleeInFrame(const ValueContext&);
    void generateAreFunctionNamesUnchanged(const ValueContext& value) { defineTest(value, isZero(loadMutable64(AOT_INSTANCE_OFFSET(callsByValue.isRenamedAtRunTime)))); }

    Operand closureCapture(CellPointer closure, FunctionID, unsigned index);

private:
    void keepInFrame(const ValueContext&, CellPointer);
    TypedPointer escapeMarkOf(CellPointer environment, uint32_t isEscapedOffset);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
