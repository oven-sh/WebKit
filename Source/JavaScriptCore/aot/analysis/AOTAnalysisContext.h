/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class EffectSummaries;
class FlowResults;

enum class CallResults : uint8_t { MayAliasOperands, NotFollowed };

struct AnalysisContext {
    BuildContext& build;
    const Program& program;
    const FlowResults* flow { nullptr };
    const EffectSummaries* effects { nullptr };
    FunctionID id { };
    CallResults callResults { CallResults::MayAliasOperands };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
