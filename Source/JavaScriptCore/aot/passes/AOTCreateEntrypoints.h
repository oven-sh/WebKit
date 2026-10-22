/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTPass.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class StringArena;
class TypeArena;

void createEntrypoints(ProgramEditor&, const OptimizationInputs&, BuildContext&);

enum class EntrypointVariants : uint8_t { WithoutNewTarget, WithNewTarget, Both };

EntrypointVariants entrypointVariantsOf(const Program&, const Function& target);
// A constructor function that does not read new.target has a second entrypoint too. `new` of a value enters it where the function is its own new.target:
// it allocates the object with the layout of the function, as a `new` that names the function does, and returns what was constructed. It is handed no `this`.
bool hasAllocatingConstructEntrypoint(const Program&, const Function& target);

inline constexpr uint32_t callEntrypointParameter = 0;
inline constexpr uint32_t constructEntrypointParameter = 1;
inline constexpr uint32_t allocatingConstructEntrypointParameter = 2;

struct GenericEntrypointRequest {
    FunctionID target;
    bool takesNewTarget { false };
    bool allocatesThis { false };
    FunctionID constructEntrypoint;
    bool isEnteredByHost { false };
};

Function createGenericEntrypoint(const AnalysisContext&, TypeArena&, StringArena&, const GenericEntrypointRequest&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
