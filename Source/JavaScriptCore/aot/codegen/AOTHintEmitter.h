/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class HintEmitter final : public OperationEmitter {
public:
    explicit HintEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateLoopHint(const ValueContext&);
    void generateReleaseHint(const ValueContext&);
    // Where nothing reads the value any more: clears the registers and the slot that hold it. The value has its code, and the code made here names it.
    void release(ValueID);
    void generateClearFrameCell(const ValueContext&);
    void generateRememberOwner(const ValueContext&);
    void pollWhereNoFrameIsSetUp(const OperationContext&);

private:
    void emitPoll(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
