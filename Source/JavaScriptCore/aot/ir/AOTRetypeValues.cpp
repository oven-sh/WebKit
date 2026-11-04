/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTTypePropagation.h"

namespace JSC { namespace AOT { namespace Typed {

PassOutcome retypeValues(FunctionPassContext& context)
{
    if (!context.function.hasBody())
        return PassOutcome::Unchanged;
    TypePropagationOutcome outcome = typeChangedValues(TypePropagationInputs { context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo });
    return outcome.didChange() ? PassOutcome::Changed : PassOutcome::Unchanged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
