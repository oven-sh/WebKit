/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTLimits.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class EffectSummaries;
class FlowResults;

enum class CallResults : uint8_t { MayAliasOperands, NotFollowed };
// The verifier judges code that the passes have finished, so a limit that the build lowers stays as usual for it.
enum class WorkLimits : bool { OfBuild, NeverLowered };

struct AnalysisContext {
    BuildContext& build;
    const Program& program;
    const FlowResults* flow { nullptr };
    const EffectSummaries* effects { nullptr };
    FunctionID id { };
    CallResults callResults { CallResults::MayAliasOperands };
    WorkLimits workLimits { WorkLimits::OfBuild };

    uint64_t limit(LimitID) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
