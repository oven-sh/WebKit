/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class OwnPropertyBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit OwnPropertyBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_OWN_PROPERTY_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

private:
    Boxed firstArgumentOrUndefined(const InlineCall&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
