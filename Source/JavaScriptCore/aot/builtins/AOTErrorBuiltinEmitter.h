/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ErrorBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit ErrorBuiltinEmitter(CodeGenerator&);

    void generateNewError(const InlineCall&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
