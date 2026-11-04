/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIRFunction.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

std::optional<CoroutineKind> coroutineKindOf(const Function&);
BuiltinClassID resultClassOf(CoroutineKind);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
