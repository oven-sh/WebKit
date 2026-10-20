/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInlineCall.h"
#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class InlineBuiltinEmitter final : public OperationEmitter {
public:
    explicit InlineBuiltinEmitter(CodeGenerator&);

    void generateCallInline(const ValueContext&);
    void generateTryCallInline(const TerminatorContext&);
    void generate(const InlineCall&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
