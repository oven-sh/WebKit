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
    explicit StaticsEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    TypedPointer inStatics(const B3::AbstractHeap& heap, uint32_t offsetInStatics) { return m_out.address(heap, m_statics, offsetInStatics); }
    TypedPointer inStatics(const B3::AbstractHeap& heap, StaticsOffset offset) { return inStatics(heap, offset.value()); }
    TypedPointer addressOf(VariableID);
    Operand loadVariable(VariableID);
    Operand loadEncodedVariable(VariableID);
    void storeVariable(VariableID, Operand, const Type& storedType);
    TypedPointer guardByteOf(GuardBitID);
    uint8_t guardMaskOf(GuardBitID) const;
    TypedPointer branchCounter(uint32_t number);
    void fillVariablesOfEnteredFile();
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
