/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ObjectCreationEmitter final : public OperationEmitter {
public:
    explicit ObjectCreationEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateDefineOwnProperty(const ValueContext&);
    void generateDefineAccessors(const ValueContext&);

    // Bit i of a mask is about keys[i] and functions[i]. Emits the definitions and jumps to `done`, or to `leftToRunTime` with the object as it was.
    // False, and nothing is emitted, where the bytes of the cell do not say enough.
    bool tryDefineAccessorsInSlots(const ValueContext&, CellPointer object, std::span<const PropertyKeyID> keys, std::span<const ValueID> functions, uint32_t getterMask, uint32_t enumerableMask, LBasicBlock done, LBasicBlock leftToRunTime);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
