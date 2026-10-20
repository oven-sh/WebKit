/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class StaticsEmitter final : public OperationEmitter {
public:
    explicit StaticsEmitter(CodeGenerator&);

    TypedPointer inStatics(const B3::AbstractHeap&, uint32_t offsetInStatics);
    TypedPointer inStatics(const B3::AbstractHeap&, StaticsOffset);
    TypedPointer addressOf(VariableID);
    Operand loadVariable(VariableID);
    Operand loadEncodedVariable(VariableID);
    void storeVariable(VariableID, Operand, const Type& storedType);
    TypedPointer guardByteOf(GuardBitID);
    uint8_t guardMaskOf(GuardBitID) const;
    TypedPointer runtimeCounter(RuntimeCounterID);
    TypedPointer branchCounter(uint32_t number);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
