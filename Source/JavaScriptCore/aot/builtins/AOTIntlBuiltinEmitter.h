/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class IntlBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit IntlBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_INTL_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

private:
    void finishWithTextOrMiss(const InlineCall&, CellPointer textOrNull);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
