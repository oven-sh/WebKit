/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class SelectedOperatorEmitter final : public OperationEmitter {
public:
    explicit SelectedOperatorEmitter(CodeGenerator&);

    void generateIntegerBound(const ValueContext&);
    void generateFulfillFreshPromise(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
