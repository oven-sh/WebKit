/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTGroupCallsByValue.h"
#include "AOTPass.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class StringArena;
class TypeArena;

void createEntrypoints(ProgramEditor&, const OptimizationInputs&, const CallGroups&, BuildContext&);

enum class EntrypointVariants : uint8_t { WithoutNewTarget, WithNewTarget, Both };

EntrypointVariants entrypointVariantsOf(const Function& target);

inline constexpr uint32_t callEntrypointParameter = 0;
inline constexpr uint32_t constructEntrypointParameter = 1;

struct GenericEntrypointRequest {
    FunctionID target;
    bool takesNewTarget { false };
    FunctionID constructEntrypoint;
    FunctionID groupEntrypoint;
    GenericCallers callers { GenericCallers::Unknown };
    bool isEnteredByHost { false };
};

Function createGenericEntrypoint(const AnalysisContext&, TypeArena&, StringArena&, const GenericEntrypointRequest&);

struct GroupSignature {
    CalleeSetID members;
    TypeID thisType;
    Vector<TypeID, 4> parameters;
    TypeID result;
};

// MadeUnlessCallerTests: for an entrypoint that any call by value enters. A tail call may have entered it, and a frame that stays behind for the test is then a frame per call of the chain.
enum class EntrypointResultTest : uint8_t { MadeByEntrypoint, MadeUnlessCallerTests, LeftToReceiver };

GroupSignature createGroupSignature(const Program&, const FlowResults&, TypeArena&, const CallGroups&, const CallGroup&);
Function createGroupEntrypoint(const AnalysisContext&, TypeArena&, FunctionID target, const GroupSignature&, EntrypointResultTest);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
