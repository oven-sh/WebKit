/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyReferences;
struct ReferencesOfBody;
class BodyIndex;
class BodySummaries;
class BodyWeights;
struct AddedEffects;
class BorrowedParameters;
class PassRunner;

enum class EffectSummariesFor : uint8_t { Reader, Nobody };
enum class HeatFor : uint8_t;

class ProgramAnalyses {
    WTF_MAKE_NONCOPYABLE(ProgramAnalyses);
public:
    ProgramAnalyses(const Program&, PassRunner&, DerivedFunctions&);
    ~ProgramAnalyses();

    const CallGraph& callGraph(const FlowResults&) const;
    const EffectSummaries& effects(const FlowResults&) const;
    const Heat& heat(const FlowResults&) const;
    const InliningSummaries& inlining(const FlowResults&) const;
    // For nobody: the inputs hold effect summaries that know nothing, and those of the program stay as they are.
    OptimizationInputs inputs(const FlowResults&, OptionSet<ProgramResult> needed, EffectSummariesFor = EffectSummariesFor::Reader);
    // For a loop that rewrites nearly every body: until a body changes, the effect summaries say what flow says of the parameters and nothing of the bodies.
    // They hold, and no body is read for summaries that are out of date when the loop ends.
    void summarizeNoBodiesUntilTheyChange() { m_summarizesNoBodies = !m_bodySummaries; }
    // For a program pass that asks for heat, drops it and leaves most bodies as they are. What the heat that is made next takes from each body by itself is kept:
    // how often the blocks run, and what the body does with its parameters. The heat after that takes it for the bodies that have not changed, and nothing is kept any longer.
    void keepPartsOfBodiesForNextHeat() { m_keepsPartsOfBodies = !m_heat; }
    // Who reads the heat from here on. A heat that lacks what they read is dropped.
    void estimateHeatFor(HeatFor);

    // What createSummariesFromFlow() gives, where the effect summaries are up to date and were computed from it. Null otherwise.
    const EffectSummaries* keptSummariesFromFlow() const;
    BorrowedParameters borrowedParameters() const;

    std::unique_ptr<EffectSummaries> takeEffects(const FlowResults&);
    std::unique_ptr<Heat> takeHeat(const FlowResults&);

    // Program::bodyIndex() while this lives.
    BodyIndex& bodyIndex() { return *m_bodyIndex; }
    // Somebody is about to change bodies without an editor, and calls invalidate() when that is done.
    void willChangeBodiesWithoutEditor();
    void willChangeBodiesThroughOwnEditors();

    // Anything in the program may have changed.
    void invalidate(OptionSet<ProgramResult>);
    // Only what the editor gave out may have changed.
    void invalidate(OptionSet<ProgramResult> results, const ProgramEditor& editor) { invalidate(results, editor, EditedFunctions::DoAnything, Effects::none()); }
    // Where a function does no more than before but for the added effects, and its summary covers those, the summary stays, as an upper bound. Any other function is read again.
    void invalidate(OptionSet<ProgramResult>, const ProgramEditor&, EditedFunctions, const Effects& addedByEdits);
    // Only these functions may have changed, and no record that functions share.
    void invalidate(OptionSet<ProgramResult>, std::span<const FunctionID> changedFunctions);
    // The operations of the functions were replaced by others that do no more, but for what is listed apart. With that, what a summary says of a function stays an upper bound.
    void didLower(std::span<const FunctionID> functions, std::span<const AddedEffects>);
    // For each function, or null: what collectReferencesOf() gives for the function as it is now. The next call graph does not read those bodies.
    void takeReferencesOfBodies(std::span<ReferencesOfBody* const> byFunction);

private:
    struct BorrowingBodiesOfHeat;

    void prepareToUpdateEffects() const;
    EffectSummaries createSummariesFromFlowWithKeptBodies(const FlowResults&) const;
    void didChangeBodies(std::span<const FunctionID>);
    const BodySummaries* summariesOfBodiesAsTheyAre() const;
    void computeEffectsBesideOtherResults(const FlowResults&, OptionSet<ProgramResult> needed);
    void destroyResults(OptionSet<ProgramResult>);

    const Program& m_program;
    PassRunner& m_runner;
    DerivedFunctions& m_derivedFunctions;
    const std::unique_ptr<BodyIndex> m_bodyIndex;
    bool m_bodiesAreGivenOutWithoutEditor { false };
    mutable std::unique_ptr<CallGraph> m_callGraph;
    mutable std::unique_ptr<BodyReferences> m_bodyReferences;
    // A call graph was made from them, and nothing was reported as changed since.
    mutable bool m_bodyReferencesAreUpToDate { false };
    // Stay when they are out of date: the next computation brings them up to date.
    mutable std::unique_ptr<EffectSummaries> m_effects;
    mutable bool m_effectsAreUpToDate { false };
    bool m_summarizesNoBodies { false };
    mutable std::unique_ptr<BodySummaries> m_bodySummaries;
    // BodySummaries::seed() while an update of the effect summaries has brought it up to date and is still reading bodies.
    const EffectSummaries* m_seedOfUpdateUnderWay { nullptr };
    mutable std::unique_ptr<Heat> m_heat;
    mutable bool m_keepsPartsOfBodies { false };
    HeatFor m_heatFor;
    mutable std::unique_ptr<BodyWeights> m_bodyWeights;
    mutable std::unique_ptr<BorrowingBodiesOfHeat> m_borrowingBodiesOfHeat;
    mutable std::unique_ptr<InliningSummaries> m_inlining;
    const std::unique_ptr<const CallGraph> m_unknownCallGraph;
    const std::unique_ptr<const EffectSummaries> m_unknownEffects;
    const std::unique_ptr<const Heat> m_unknownHeat;
    const std::unique_ptr<const InliningSummaries> m_unknownInlining;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
