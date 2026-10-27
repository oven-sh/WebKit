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
    explicit HintEmitter(CodeGenerator&);

    void generateLoopHint(const ValueContext&);
    void pollWhereNoFrameIsSetUp(const OperationContext&);

private:
    Expected<bool> endsWithinLengthOfData(BlockID);
    bool allocatesWhereErrorsAreCaught(BlockID);
    void emitPoll(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
