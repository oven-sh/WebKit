/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class GuardValueEmitter final : public OperationEmitter {
public:
    explicit GuardValueEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateAllGuards(const ValueContext&) { }
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
