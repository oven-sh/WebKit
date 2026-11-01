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
// A saved frame is out of the program's reach, so what a suspension holds has gone nowhere. Only the verifier judges by that: a pass that kept a guard by it would keep it for a stage that
// reads the object out of a frame in the heap, where it comes from outside.
enum class SavedFrames : bool { AreUnknownMemory, AreOutOfReach };

struct AnalysisContext {
    BuildContext& build;
    const Program& program;
    const FlowResults* flow { nullptr };
    const EffectSummaries* effects { nullptr };
    FunctionID id { };
    CallResults callResults { CallResults::MayAliasOperands };
    WorkLimits workLimits { WorkLimits::OfBuild };
    SavedFrames savedFrames { SavedFrames::AreUnknownMemory };

    // The analyses of the verifier and of the emitter of run-time validation. They look closer at what has escaped than a pass does: every build would pay for that look, and only a build that checks itself asks for it.
    bool onlyJudges() const { return savedFrames == SavedFrames::AreOutOfReach; }
    uint64_t limit(LimitID) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
