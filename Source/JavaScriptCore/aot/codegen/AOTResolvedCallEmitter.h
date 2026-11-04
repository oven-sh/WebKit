/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ResolvedCallEmitter final : public OperationEmitter {
public:
    explicit ResolvedCallEmitter(CodeGenerator&);

    void generateResolveCallTarget(const ValueContext&);
    void generateCallResolved(const ValueContext&);
    void generateRegExpMatchLiteral(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
