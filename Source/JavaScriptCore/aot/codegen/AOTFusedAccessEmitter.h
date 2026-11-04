/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class FusedAccessEmitter final : public OperationEmitter {
public:
    explicit FusedAccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generatePropGetChain(const ValueContext&);
    void generatePropGetPair(const ValueContext&);
    void generatePropGetAndTest(const ValueContext&);
    void generateCallMethod(const ValueContext&);
    void generateTryPutByIds(const ValueContext&);

private:
    Condition tryPutByIds(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
