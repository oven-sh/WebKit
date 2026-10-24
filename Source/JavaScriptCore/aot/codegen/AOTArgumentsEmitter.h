/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ArgumentsEmitter final : public OperationEmitter {
public:
    explicit ArgumentsEmitter(CodeGenerator&);

    void generateGetArgument(const ValueContext&);
    void generateGetRestLength(const ValueContext&);
    void generateCreateRest(const ValueContext&);
    void generateGetMyArgumentByVal(const ValueContext&);
    void generateCallForwardVarargs(const ValueContext&);
    void generateConstructSuperForwardVarargs(const ValueContext&);

private:
    Word32 restLength(uint32_t firstArgument);
    Pointer restList(uint32_t firstArgument);
    Boxed loadArgument(Pointer list, Integer index);
    Integer indexOfBoxedKey(Boxed key, LBasicBlock isNotIndex);
    Boxed readFromMaterializedList(const ValueContext&, Boxed key);
    void emitTailCallWithList(Boxed callee, Boxed thisValue, Word32 count, Pointer list);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
