/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCoroutines.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

std::optional<CoroutineKind> coroutineKindOf(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    bool isGenerator = function.has(FunctionFlags::IsGenerator);
    if (isAsync && isGenerator)
        return CoroutineKind::AsyncGenerator;
    if (isAsync)
        return CoroutineKind::AsyncFunction;
    if (isGenerator)
        return CoroutineKind::Generator;
    return std::nullopt;
}

BuiltinClassID resultClassOf(CoroutineKind kind)
{
    switch (kind) {
    case CoroutineKind::Generator:
        return BuiltinClasses::Generator;
    case CoroutineKind::AsyncFunction:
        return BuiltinClasses::Promise;
    case CoroutineKind::AsyncGenerator:
        return BuiltinClasses::AsyncGenerator;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
