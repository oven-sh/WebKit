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
    explicit InlineBuiltinEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateCallInline(const ValueContext& value) { generate(InlineCall(value)); }
    void generateTryCallInline(const TerminatorContext& terminator) { generate(InlineCall(terminator)); }
    void generate(const InlineCall&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
