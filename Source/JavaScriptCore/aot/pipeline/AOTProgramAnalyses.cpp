/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramAnalyses.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectSummariesInternal.h"
#include "AOTHeat.h"
#include "AOTHeatGraph.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTRecordUsers.h"
#include "AOTSummarizeForInlining.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

// What each body does with its parameters, as it was when the table of borrowed parameters was last made from this.
struct ProgramAnalyses::BorrowingBodiesOfHeat {
    void didChange(std::span<const FunctionID> functions)
    {
        for (FunctionID id : functions) {
            if (id.index() < hasChanged.size())
                hasChanged[id.index()] = 1;
        }
    }

    bool keepsSomethingOf(FunctionID id) const { return id.index() < hasChanged.size() && !hasChanged[id.index()]; }

    BorrowingBodies bodies;
    Vector<uint8_t> hasChanged;
    Vector<uint64_t> stampsOfRecords;
};

template<typename... Results>
static void destroyResultsLater(BuildContext& build, std::unique_ptr<Results>&... results)
{
    auto destroyLater = [&](auto& result) {
        if (result)
            build.destroyLater(std::exchange(result, nullptr));
    };
    (destroyLater(results), ...);
}

ProgramAnalyses::ProgramAnalyses(const Program& program, PassRunner& runner, DerivedFunctions& derivedFunctions)
    : m_program(program)
    , m_runner(runner)
    , m_derivedFunctions(derivedFunctions)
    , m_bodyIndex(makeUnique<BodyIndex>(runner.build().parallelLoops()))
    , m_heatFor(HeatFor::Everything)
    , m_unknownCallGraph(makeUniqueWithoutFastMallocCheck<const CallGraph>())
    , m_unknownEffects(makeUniqueWithoutFastMallocCheck<const EffectSummaries>())
    , m_unknownHeat(makeUniqueWithoutFastMallocCheck<const Heat>())
    , m_unknownInlining(makeUniqueWithoutFastMallocCheck<const InliningSummaries>())
{
    program.setBodyIndex(m_bodyIndex.get());
}

ProgramAnalyses::~ProgramAnalyses()
{
    m_program.setBodyIndex(nullptr);
    destroyResultsLater(m_runner.build(), m_bodyIndex, m_bodyReferences, m_bodyWeights, m_borrowingBodiesOfHeat, m_bodySummaries, m_effects, m_inlining, m_heat, m_callGraph);
}

const CallGraph& ProgramAnalyses::callGraph(const FlowResults& flow) const
{
    if (!m_callGraph) {
        ProgramCachesWithScratchTypes caches(m_program, m_runner.build());
        if (!m_bodyReferences)
            m_bodyReferences = makeUnique<BodyReferences>();
        m_callGraph = makeUniqueWithoutFastMallocCheck<CallGraph>(m_runner.runAnalysis(PassID::BuildCallGraph, m_program, [&] {
            CallGraph graph = buildCallGraph(m_program, flow, m_runner.build(), m_bodyReferences.get());
            m_bodyReferencesAreUpToDate = true;
            return graph;
        }));
    }
    return *m_callGraph;
}

void ProgramAnalyses::prepareToUpdateEffects() const
{
    if (m_bodySummaries)
        return;
    m_bodySummaries = makeUnique<BodySummaries>();
    destroyResultsLater(m_runner.build(), m_effects);
    m_effects = makeUniqueWithoutFastMallocCheck<EffectSummaries>();
}

// What createSummariesFromFlow() gives. Of a body that has not changed since the table of borrowed parameters was made for the last heat, what was read then is taken.
EffectSummaries ProgramAnalyses::createSummariesFromFlowWithKeptBodies(const FlowResults& flow) const
{
    BuildContext& build = m_runner.build();
    std::unique_ptr<BorrowingBodiesOfHeat> kept = std::exchange(m_borrowingBodiesOfHeat, nullptr);
    if (!kept && !m_keepsPartsOfBodies)
        return createSummariesFromFlow(m_program, flow, build);
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<BorrowingBodiesOfHeat>();

    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    unsigned numberOfKeptBodies = kept->bodies.size();
    Vector<uint8_t> recordLooksDifferent(numberOfKeptBodies);
    forEachIndexInParallel(loops, numberOfKeptBodies, [&](unsigned index) {
        recordLooksDifferent[index] = stampOfRecord(m_program, FunctionID(index)) != kept->stampsOfRecords[index];
    });
    BorrowedParameters table = computeBorrowedParameters(m_program, build, kept->bodies, kept->hasChanged.span(), recordLooksDifferent.span());
    RELEASE_ASSERT(!build.options().verifiesAfterEveryPass || table == computeBorrowedParameters(m_program, build));
    if (m_keepsPartsOfBodies) {
        kept->hasChanged.fill(0, numberOfFunctions);
        kept->stampsOfRecords.grow(numberOfFunctions);
        forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
            kept->stampsOfRecords[index] = stampOfRecord(m_program, FunctionID(index));
        });
        m_borrowingBodiesOfHeat = WTF::move(kept);
    } else
        destroyResultsLater(build, kept);
    return createSummariesFromFlow(m_program, flow, table, build);
}

const EffectSummaries& ProgramAnalyses::effects(const FlowResults& flow) const
{
    ProgramCachesWithScratchTypes caches(m_program, m_runner.build(), !m_effectsAreUpToDate);
    if (!m_effectsAreUpToDate && m_summarizesNoBodies) {
        m_effectsAreUpToDate = m_runner.runAnalysis(PassID::ComputeEffectSummaries, m_program, [&] {
            m_effects = makeUniqueWithoutFastMallocCheck<EffectSummaries>(createSummariesFromFlowWithKeptBodies(flow));
            return true;
        });
    }
    if (!m_effectsAreUpToDate) {
        prepareToUpdateEffects();
        if (!m_bodyReferences)
            m_bodyReferences = makeUnique<BodyReferences>();
        m_effectsAreUpToDate = m_runner.runAnalysis(PassID::ComputeEffectSummaries, m_program, [&] {
            updateEffectSummaries(m_program, flow, m_runner.build(), *m_bodySummaries, *m_effects, m_bodyReferences.get());
            return true;
        });
    }
    return m_effectsAreUpToDate ? *m_effects : *m_unknownEffects;
}

void ProgramAnalyses::estimateHeatFor(HeatFor heatFor)
{
    m_heatFor = heatFor;
    // A runner that has stopped makes no other.
    if (heatFor == HeatFor::Everything && m_heat && !m_heat->hasWeights() && !m_runner.isStopped()) {
        destroyResultsLater(m_runner.build(), m_heat);
        m_runner.didInvalidate(ProgramResult::Heat);
    }
}

void ProgramAnalyses::dropHeat()
{
    // A runner that has stopped makes no other.
    if (!m_heat || m_runner.isStopped())
        return;
    destroyResultsLater(m_runner.build(), m_inlining, m_heat);
    m_runner.didInvalidate({ ProgramResult::Heat, ProgramResult::InliningSummaries });
}

const Heat& ProgramAnalyses::heat(const FlowResults& flow) const
{
    if (!m_heat) {
        BuildContext& build = m_runner.build();
        ProgramCachesWithScratchTypes caches(m_program, build);
        const CallGraph& graph = callGraph(flow);
        const EffectSummaries* summariesFromFlow = m_effectsAreUpToDate && m_summarizesNoBodies ? m_effects.get() : m_seedOfUpdateUnderWay;
        const BodyReferences* references = m_bodyReferencesAreUpToDate ? m_bodyReferences.get() : nullptr;
        std::unique_ptr<BodyWeights> keptWeights = std::exchange(m_bodyWeights, nullptr);
        std::unique_ptr<BodyWeights> madeWeights = m_keepsPartsOfBodies ? makeUnique<BodyWeights>() : nullptr;
        m_heat = makeUniqueWithoutFastMallocCheck<Heat>(m_runner.runAnalysis(PassID::EstimateHeat, m_program, [&] {
            std::optional<EffectSummaries> ownSummaries;
            if (!summariesFromFlow && (m_keepsPartsOfBodies || m_borrowingBodiesOfHeat)) {
                PhaseTimes times(build, PassID::EstimateHeat);
                summariesFromFlow = &ownSummaries.emplace(createSummariesFromFlowWithKeptBodies(flow));
                times.didFinish("createSummariesFromFlow"_s);
            }
            return estimateHeat(m_program, graph, flow, build, summariesFromFlow, references, keptWeights.get(), madeWeights.get(), m_heatFor);
        }));
        m_keepsPartsOfBodies = false;
        m_bodyWeights = WTF::move(madeWeights);
        destroyResultsLater(build, keptWeights);
    }
    return *m_heat;
}

const InliningSummaries& ProgramAnalyses::inlining(const FlowResults& flow) const
{
    if (!m_inlining) {
        ProgramCachesWithScratchTypes caches(m_program, m_runner.build());
        const CallGraph& graph = callGraph(flow);
        const Heat& estimatedHeat = heat(flow);
        m_inlining = makeUniqueWithoutFastMallocCheck<InliningSummaries>(m_runner.runAnalysis(PassID::SummarizeForInlining, m_program, [&] {
            return summarizeForInlining(m_program, graph, estimatedHeat, flow, m_runner.build());
        }));
    }
    return *m_inlining;
}

bool ProgramAnalyses::lacksAnyOf(OptionSet<ProgramResult> needed) const
{
    return (needed.contains(ProgramResult::CallGraph) && !m_callGraph) || (needed.contains(ProgramResult::Heat) && !m_heat) || (needed.contains(ProgramResult::InliningSummaries) && !m_inlining);
}

// The effect summaries ask nothing of the call graph, nor of what is computed from it, much of which runs on one thread.
void ProgramAnalyses::computeEffectsBesideOtherResults(const FlowResults& flow, OptionSet<ProgramResult> needed)
{
    if (m_effectsAreUpToDate || !needed.contains(ProgramResult::EffectSummaries) || !m_runner.mayRunAnalysesTogether() || !lacksAnyOf(needed))
        return;

    // estimateHeat() starts from these summaries, so only the call graph is built beside them.
    if (m_summarizesNoBodies) {
        m_effectsAreUpToDate = m_runner.runAnalysisBeside(PassID::ComputeEffectSummaries, m_program, [&] {
            m_effects = makeUniqueWithoutFastMallocCheck<EffectSummaries>(createSummariesFromFlowWithKeptBodies(flow));
            return true;
        }, [&] {
            callGraph(flow);
        });
        return;
    }

    prepareToUpdateEffects();
    auto computeOtherResults = [&] {
        if (needed.contains(ProgramResult::CallGraph))
            callGraph(flow);
        if (needed.contains(ProgramResult::Heat))
            heat(flow);
        if (needed.contains(ProgramResult::InliningSummaries))
            inlining(flow);
    };

    // estimateHeat() starts from what the summaries start from. They bring it up to date from what they keep, so it is made first and lent.
    if (needed.contains(ProgramResult::Heat) && !m_heat) {
        EffectSummarizer summarizer(m_program, flow, m_runner.build(), *m_bodySummaries, EffectSummarizer::Witnesses::Omitted);
        m_effectsAreUpToDate = m_runner.runAnalysisInTwoSteps(PassID::ComputeEffectSummaries, m_program, [&] {
            summarizer.bringSeedUpToDate(*m_effects);
            m_seedOfUpdateUnderWay = &m_bodySummaries->lendSeed();
        }, [&] {
            summarizer.finishUpdate(*m_effects);
            return true;
        }, computeOtherResults);
        if (std::exchange(m_seedOfUpdateUnderWay, nullptr))
            m_bodySummaries->takeSeedBack();
        return;
    }

    m_effectsAreUpToDate = m_runner.runAnalysisBeside(PassID::ComputeEffectSummaries, m_program, [&] {
        updateEffectSummaries(m_program, flow, m_runner.build(), *m_bodySummaries, *m_effects);
        return true;
    }, computeOtherResults);
}

OptimizationInputs ProgramAnalyses::inputs(const FlowResults& flow, OptionSet<ProgramResult> needed, EffectSummariesFor summariesFor)
{
    if (summariesFor == EffectSummariesFor::Nobody && needed.contains(ProgramResult::EffectSummaries)) {
        needed.remove(ProgramResult::EffectSummaries);
        m_runner.didCompute(ProgramResult::EffectSummaries);
    }
    ProgramCachesWithScratchTypes caches(m_program, m_runner.build(), lacksAnyOf(needed) || (needed.contains(ProgramResult::EffectSummaries) && !m_effectsAreUpToDate));
    computeEffectsBesideOtherResults(flow, needed);
    return {
        flow,
        needed.contains(ProgramResult::CallGraph) ? callGraph(flow) : *m_unknownCallGraph,
        needed.contains(ProgramResult::Heat) ? heat(flow) : *m_unknownHeat,
        needed.contains(ProgramResult::InliningSummaries) ? inlining(flow) : *m_unknownInlining,
        needed.contains(ProgramResult::EffectSummaries) ? effects(flow) : *m_unknownEffects,
        &m_derivedFunctions,
    };
}

const BodySummaries* ProgramAnalyses::summariesOfBodiesAsTheyAre() const
{
    if (!m_effectsAreUpToDate || m_summarizesNoBodies || !m_bodySummaries || !m_bodySummaries->describesFunctionsAsTheyAre(m_program.functions.size()))
        return nullptr;
    if (m_runner.build().options().verifiesAfterEveryPass)
        RELEASE_ASSERT(m_bodySummaries->borrowedParameters() == computeBorrowedParameters(m_program, m_runner.build()));
    return m_bodySummaries.get();
}

const EffectSummaries* ProgramAnalyses::keptSummariesFromFlow() const
{
    const BodySummaries* summaries = summariesOfBodiesAsTheyAre();
    return summaries ? &summaries->seed() : nullptr;
}

BorrowedParameters ProgramAnalyses::borrowedParameters() const
{
    if (const BodySummaries* summaries = summariesOfBodiesAsTheyAre())
        return summaries->borrowedParameters();
    BuildContext& build = m_runner.build();
    ProgramCachesWithScratchTypes caches(m_program, build);
    if (m_summarizesNoBodies || !m_bodySummaries)
        return computeBorrowedParameters(m_program, build);
    BorrowedParameters table = m_bodySummaries->computeBorrowedParametersOfChangedBodies(m_program, build);
    if (build.options().verifiesAfterEveryPass)
        RELEASE_ASSERT(table == computeBorrowedParameters(m_program, build));
    return table;
}

std::unique_ptr<EffectSummaries> ProgramAnalyses::takeEffects(const FlowResults& flow)
{
    effects(flow);
    m_runner.didInvalidate(ProgramResult::EffectSummaries);
    bool wereUpToDate = std::exchange(m_effectsAreUpToDate, false);
    destroyResultsLater(m_runner.build(), m_bodySummaries);
    std::unique_ptr<EffectSummaries> result = std::exchange(m_effects, nullptr);
    return wereUpToDate ? WTF::move(result) : makeUniqueWithoutFastMallocCheck<EffectSummaries>();
}

std::unique_ptr<Heat> ProgramAnalyses::takeHeat(const FlowResults& flow)
{
    heat(flow);
    m_runner.didInvalidate(ProgramResult::Heat);
    return std::exchange(m_heat, nullptr);
}

void ProgramAnalyses::willChangeBodies(BodiesAreChanged bodiesAreChanged)
{
    m_bodyIndex->growTo(m_program.functions.size());
    if (bodiesAreChanged != BodiesAreChanged::ThroughEditors)
        m_bodyIndex->didGiveOutAll();
    m_bodiesAreGivenOutWithoutEditor = true;
}

void ProgramAnalyses::invalidate(OptionSet<ProgramResult> results)
{
    m_bodyIndex->growTo(m_program.functions.size());
    // Whoever has said that it changes bodies may have written the rows of bodies behind its last change to them. Those count from here on.
    if (std::exchange(m_bodiesAreGivenOutWithoutEditor, false))
        m_bodyIndex->didEndAllEdits();
    else
        m_bodyIndex->forgetAll();
    m_effectsAreUpToDate = false;
    destroyResultsLater(m_runner.build(), m_bodySummaries, m_effects, m_bodyReferences, m_bodyWeights, m_borrowingBodiesOfHeat);
    destroyResults(results | ProgramResult::EffectSummaries);
}

// A caller that alone holds an object it passes looks only at what the summary says of the parameter.
static bool covers(const Program& program, FunctionID id, const FunctionSummaryView& summary, const Effects& effects)
{
    if (effects.isPure())
        return true;
    EffectsOnEdges elsewhere = summary.elsewhere();
    if (!effects.isSubsetOf(elsewhere.normal) || (elsewhere.mayThrow() && !effects.isSubsetOf(elsewhere.handler)))
        return false;
    OperandMemory memory { effects.reads.restrictedTo(perObjectHeaps), effects.writes.restrictedTo(perObjectHeaps) };
    const Function& function = program.at(id);
    if (!function.hasBody())
        return true;
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    if (parameters.size() != summary.numberOfParameters())
        return false;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (!mayBeReference(program.typeOf(function, parameters[index])))
            continue;
        // An operation that a pass adds looks at its operand, which may be a parameter that the body only handed on: MarkEnvironmentEscaped.
        if (!summary.operand(index).uses.contains(OperandUse::Inspects))
            return false;
        OperandMemory memoryOfParameter = summary.memory(index);
        if (!memory.reads.isSubsetOf(memoryOfParameter.reads) || !memory.writes.isSubsetOf(memoryOfParameter.writes))
            return false;
    }
    return true;
}

void ProgramAnalyses::invalidate(OptionSet<ProgramResult> results, const ProgramEditor& editor, EditedFunctions editedFunctions, const Effects& addedByEdits)
{
    for (FunctionID id : editor.changedFunctions())
        m_bodyIndex->didEndEdits(id);
    Vector<FunctionID> changedFunctions = editor.functionsWithChangedOperations();
    Vector<LayoutID> changedLayouts = editor.changedLayouts();
    Vector<FieldReference> changedFields = editor.changedFields();
    Vector<VariableID> changedVariables = editor.changedVariables();
    ChangedRecords changedRecords { changedLayouts.span(), changedFields.span(), changedVariables.span() };
    size_t numberOfEditedFunctions = changedFunctions.size();
    // The references say which bodies came to a record by way of the fields of others. Without them nobody knows who sees a record that changed.
    if (!m_bodyReferences && !changedRecords.isEmpty())
        destroyResultsLater(m_runner.build(), m_bodyWeights, m_borrowingBodiesOfHeat);
    if ((m_bodySummaries || m_bodyReferences) && !changedRecords.isEmpty()) {
        constexpr size_t functionsPerShare = 4096;
        Vector<uint8_t> followsFieldsOfOperands(m_program.functions.size());
        // Behind a loop over all functions nearly every function counts as changed already. Nothing comes of finding out that it also sees a record that has changed.
        Vector<uint8_t> isKept(m_program.functions.size());
        forEachIndexInParallel(m_runner.build().parallelLoops(), static_cast<unsigned>((followsFieldsOfOperands.size() + functionsPerShare - 1) / functionsPerShare), [&](unsigned share) {
            for (size_t index = share * functionsPerShare; index < std::min(followsFieldsOfOperands.size(), (share + 1) * functionsPerShare); ++index) {
                FunctionID id(index);
                followsFieldsOfOperands[index] = (m_bodySummaries && m_bodySummaries->followsFieldsOfOperands(id)) || (m_bodyReferences && m_bodyReferences->followsFieldsOfOperands(id));
                isKept[index] = (m_bodySummaries && m_bodySummaries->keepsSomethingOf(id)) || (m_bodyReferences && m_bodyReferences->keepsSomethingOf(id))
                    || (m_bodyWeights && m_bodyWeights->keepsSomethingOf(id)) || (m_borrowingBodiesOfHeat && m_borrowingBodiesOfHeat->keepsSomethingOf(id));
            }
        });
        changedFunctions.appendVector(findFunctionsSeeing(m_program, changedRecords, followsFieldsOfOperands.span(), isKept.span(), m_runner.build().parallelLoops()));
    }
    Vector<FunctionID> loweredFunctions = editor.functionsThatWereOnlyLowered();
    if (m_runner.build().options().logging.logsPhaseTimes) [[unlikely]] {
        dataLogLn("AOT edits of a program pass: functions ", numberOfEditedFunctions, " | only lowered ", loweredFunctions.size(), " | layouts ", changedLayouts.size(), " | fields ", changedFields.size(), " | variables ", changedVariables.size(),
            " | functions that see those ", changedFunctions.size() - numberOfEditedFunctions);
    }
    if (m_runner.build().validates())
        changedFunctions.appendVector(std::exchange(loweredFunctions, { }));
    if (m_bodySummaries)
        m_bodySummaries->didLower(loweredFunctions.span());
    didChangeBodies(loweredFunctions.span());
    if (editedFunctions == EditedFunctions::DoAnything || !m_bodySummaries || !m_effects || m_runner.build().validates()) {
        invalidate(results, changedFunctions.span());
        return;
    }

    Vector<FunctionID> functionsToReadAgain;
    Vector<FunctionID> keptFunctions;
    Vector<FunctionID> functionsWithNewStorageToReadAgain;
    Vector<FunctionID> keptFunctionsWithNewStorage;
    Vector<AddedEffects> added;
    Vector<uint8_t> isReadAgainAt(changedFunctions.size());
    forEachIndexInParallel(m_runner.build().parallelLoops(), changedFunctions.size(), [&](unsigned position) {
        FunctionID id = changedFunctions[position];
        isReadAgainAt[position] = (editedFunctions == EditedFunctions::DoLess && position < numberOfEditedFunctions) || !m_effects->contains(id) || !covers(m_program, id, m_effects->of(id), addedByEdits);
    });
    for (size_t position = 0; position < changedFunctions.size(); ++position) {
        FunctionID id = changedFunctions[position];
        bool isReadAgain = isReadAgainAt[position];
        bool hasOnlyNewStorage = position < numberOfEditedFunctions && editor.wasGivenOutOnlyForStorage(id);
        if (isReadAgain) {
            (hasOnlyNewStorage ? functionsWithNewStorageToReadAgain : functionsToReadAgain).append(id);
            continue;
        }
        (hasOnlyNewStorage ? keptFunctionsWithNewStorage : keptFunctions).append(id);
        if (!addedByEdits.isPure())
            added.append(AddedEffects { id, addedByEdits });
    }
    if (m_runner.build().options().logging.logsPhaseTimes) [[unlikely]] {
        dataLogLn("AOT edits that do no more: summaries kept ", keptFunctions.size() + keptFunctionsWithNewStorage.size(), " | read again ", functionsToReadAgain.size(),
            " | read again if lowered, only storage was set ", functionsWithNewStorageToReadAgain.size(), " | of those kept, only storage was set ", keptFunctionsWithNewStorage.size());
    }
    m_bodySummaries->didChangeDoingNoMore(keptFunctions.span());
    m_bodySummaries->didChangeDoingNoMore(keptFunctionsWithNewStorage.span(), BodySummaries::Edits::OnlySetStorage);
    m_bodySummaries->didAddEffects(added.span());
    m_bodySummaries->didChange(functionsToReadAgain.span(), BodySummaries::Records::SayNoLess);
    m_bodySummaries->didChange(functionsWithNewStorageToReadAgain.span(), BodySummaries::Records::SayNoLess, BodySummaries::Edits::OnlySetStorage);
    didChangeBodies(changedFunctions.span());
    destroyResults(results);
}

void ProgramAnalyses::invalidate(OptionSet<ProgramResult> results, std::span<const FunctionID> changedFunctions)
{
    if (m_bodySummaries)
        m_bodySummaries->didChange(changedFunctions);
    didChangeBodies(changedFunctions);
    destroyResults(results);
}

void ProgramAnalyses::didChangeBodies(std::span<const FunctionID> functions)
{
    if (m_bodyReferences)
        m_bodyReferences->didChange(functions);
    if (m_bodyWeights)
        m_bodyWeights->didChange(functions);
    if (m_borrowingBodiesOfHeat)
        m_borrowingBodiesOfHeat->didChange(functions);
}

// A validating build compares each summary with what the body says at the end, and the analysis of a body says less of operations that were lowered.
void ProgramAnalyses::didLower(std::span<const FunctionID> functions, std::span<const AddedEffects> addedEffects)
{
    if (m_runner.build().validates()) {
        invalidate(resultsAboutBodies, functions);
        return;
    }
    if (m_bodySummaries) {
        m_bodySummaries->didLower(functions);
        m_bodySummaries->didAddEffects(addedEffects);
    }
    didChangeBodies(functions);
    destroyResults(addedEffects.empty() ? resultsAboutBodies - ProgramResult::EffectSummaries : resultsAboutBodies);
}

void ProgramAnalyses::takeReferencesOfBodies(std::span<const std::unique_ptr<ReferencesOfBody>> byFunction)
{
    RELEASE_ASSERT(byFunction.size() == m_program.functions.size());
    if (m_callGraph)
        return;
    const ParallelLoops& loops = m_runner.build().parallelLoops();
    if (!m_bodyReferences)
        m_bodyReferences = makeUnique<BodyReferences>();
    ParallelArray<uint64_t> stampsOfRecords(loops, byFunction.size(), [&](unsigned index) {
        return stampOfRecord(m_program, FunctionID(index));
    });
    m_bodyReferences->prepareToTake(m_program, loops, stampsOfRecords.span());
    forEachIndexInParallel(loops, byFunction.size(), [&](unsigned index) {
        if (byFunction[index])
            m_bodyReferences->take(m_program, FunctionID(index), WTF::move(*byFunction[index]), stampsOfRecords[index]);
    });
}

void ProgramAnalyses::destroyResults(OptionSet<ProgramResult> results)
{
    m_bodyReferencesAreUpToDate = false;
    if (results.containsAny({ ProgramResult::CallGraph, ProgramResult::Heat }))
        results.add(ProgramResult::InliningSummaries);

    BuildContext& build = m_runner.build();
    if (results.contains(ProgramResult::InliningSummaries))
        destroyResultsLater(build, m_inlining);
    if (results.contains(ProgramResult::Heat))
        destroyResultsLater(build, m_heat);
    if (results.contains(ProgramResult::CallGraph))
        destroyResultsLater(build, m_callGraph);
    if (results.contains(ProgramResult::EffectSummaries)) {
        m_effectsAreUpToDate = false;
        m_summarizesNoBodies = false;
    }
    m_runner.didInvalidate(results & resultsComputedOnDemand);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
