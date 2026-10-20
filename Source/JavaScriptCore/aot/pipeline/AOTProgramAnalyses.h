/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include <memory>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class BodySummaries;
class PassRunner;

class ProgramAnalyses {
    WTF_MAKE_NONCOPYABLE(ProgramAnalyses);
public:
    ProgramAnalyses(const Program&, PassRunner&, DerivedFunctions&);
    ~ProgramAnalyses();

    const CallGraph& callGraph(const FlowResults&) const;
    const EffectSummaries& effects(const FlowResults&) const;
    const Heat& heat(const FlowResults&) const;
    const InliningSummaries& inlining(const FlowResults&) const;
    OptimizationInputs inputs(const FlowResults&, OptionSet<ProgramResult> needed);

    std::unique_ptr<EffectSummaries> takeEffects(const FlowResults&);
    std::unique_ptr<Heat> takeHeat(const FlowResults&);

    // Anything in the program may have changed.
    void invalidate(OptionSet<ProgramResult>);
    // Only what the editor gave out may have changed.
    void invalidate(OptionSet<ProgramResult>, const ProgramEditor&);

private:
    void computeEffectsBesideOtherResults(const FlowResults&, OptionSet<ProgramResult> needed);
    void destroyResults(OptionSet<ProgramResult>);

    const Program& m_program;
    PassRunner& m_runner;
    DerivedFunctions& m_derivedFunctions;
    mutable std::unique_ptr<CallGraph> m_callGraph;
    mutable std::unique_ptr<EffectSummaries> m_effects;
    mutable std::unique_ptr<BodySummaries> m_bodySummaries;
    mutable std::unique_ptr<Heat> m_heat;
    mutable std::unique_ptr<InliningSummaries> m_inlining;
    const std::unique_ptr<const CallGraph> m_unknownCallGraph;
    const std::unique_ptr<const EffectSummaries> m_unknownEffects;
    const std::unique_ptr<const Heat> m_unknownHeat;
    const std::unique_ptr<const InliningSummaries> m_unknownInlining;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
