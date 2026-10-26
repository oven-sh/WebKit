/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectSummariesInternal.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <numeric>
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isBodyThatOnlyStartOfProgramEnters(const Program& program, const FlowResults& flow, FunctionID id)
{
    return program.entries.size() == 1 && program.at(program.entries[0].module).body == id && runsAtMostOnce(flow, id);
}

static bool mayBeEnteredByUnknownCode(const Program& program, const FlowResults& flow, FunctionID id, const Function& function, const StoredBodySummary& body)
{
    bool mayBeResumed = body.onReturn().maySuspend();
    if (mayBeResumed)
        return true;
    if (callerStateOf(flow, id) == CallerState::AllKnown || isBodyThatOnlyStartOfProgramEnters(program, flow, id))
        return false;
    return function.mayBeCalledByHost() || !function.hasOnlyDirectCallers();
}

static uint32_t numberOfNode(const SummaryNode& node)
{
    switch (node.part) {
    case SummaryPart::OnReturn:
    case SummaryPart::UnknownCode:
        return 0;
    case SummaryPart::OnThrow:
        return 1;
    case SummaryPart::Parameter:
        return 2 + node.parameter;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StoredBodySummary::StoredBodySummary(InternedLocationMasks& masks, FunctionID function, const BodySummary& body)
    : m_onReturn(masks.compress(body.onReturn))
    , m_onThrow(masks.compress(body.onThrow))
    , m_numberOfParameters(body.parameters.size())
    , m_hasWitness(!!body.unknownCodeWitness)
    , m_followsFieldsOfOperands(body.followsFieldsOfOperands)
    , m_numberOfDependencies(body.dependencies.size())
    , m_witness(body.unknownCodeWitness.value)
{
    struct Dependency {
        uint32_t function;
        uint32_t source;
        uint32_t target;

        auto operator<=>(const Dependency&) const = default;
    };

    RELEASE_ASSERT(body.parameters.size() < 1u << 30);
    RELEASE_ASSERT(!body.unknownCodeWitness || body.unknownCodeWitness.function == function);
    Vector<Dependency, 64> dependencies(body.dependencies.size(), [&](size_t index) {
        const SummaryDependency& dependency = body.dependencies[index];
        RELEASE_ASSERT(dependency.target.function == function && dependency.target.part != SummaryPart::UnknownCode);
        RELEASE_ASSERT((dependency.source.part == SummaryPart::UnknownCode) == !dependency.source.function);
        return Dependency { dependency.source.function ? dependency.source.function.index() + 1 : 0, numberOfNode(dependency.source), numberOfNode(dependency.target) };
    });
    if (!std::ranges::is_sorted(dependencies))
        AOT::sort(dependencies);

    size_t numberOfGroups = 0;
    for (size_t index = 0; index < dependencies.size(); ++index) {
        numberOfGroups += !index || dependencies[index].function != dependencies[index - 1].function;
        m_hasWideDependencies |= dependencies[index].source > 0xffff || dependencies[index].target > 0xffff;
    }
    size_t numberOfWords = 2 * numberOfGroups + dependencies.size() * (m_hasWideDependencies ? 2 : 1);
    RELEASE_ASSERT(numberOfWords < 1u << 31);
    m_numberOfWordsOfDependencies = numberOfWords;

    if (!sizeOfLists())
        return;
    m_lists = MallocPtr<StoredParameterSummary>::malloc(sizeOfLists());
    for (uint32_t parameter = 0; parameter < m_numberOfParameters; ++parameter)
        m_lists.get()[parameter] = StoredParameterSummary::create(masks.compress(body.parameters[parameter].memory), body.parameters[parameter].operand);
    uint32_t* words = reinterpret_cast<uint32_t*>(m_lists.get() + m_numberOfParameters);
    for (size_t first = 0; first < dependencies.size();) {
        size_t end = first;
        while (end < dependencies.size() && dependencies[end].function == dependencies[first].function)
            ++end;
        *words++ = dependencies[first].function;
        *words++ = end - first;
        for (; first < end; ++first) {
            if (m_hasWideDependencies) {
                *words++ = dependencies[first].source;
                *words++ = dependencies[first].target;
            } else
                *words++ = dependencies[first].source << 16 | dependencies[first].target;
        }
    }
}

bool operator==(const StoredBodySummary& a, const StoredBodySummary& b)
{
    return a.m_onReturn == b.m_onReturn
        && a.m_onThrow == b.m_onThrow
        && a.m_numberOfParameters == b.m_numberOfParameters
        && a.m_hasWitness == b.m_hasWitness
        && a.m_followsFieldsOfOperands == b.m_followsFieldsOfOperands
        && a.m_numberOfDependencies == b.m_numberOfDependencies
        && a.m_numberOfWordsOfDependencies == b.m_numberOfWordsOfDependencies
        && a.m_hasWideDependencies == b.m_hasWideDependencies
        && a.m_witness == b.m_witness
        && (!a.sizeOfLists() || !memcmp(a.m_lists.get(), b.m_lists.get(), a.sizeOfLists()));
}

void BodySummaries::didChange(std::span<const FunctionID> functions, Records records, Edits edits)
{
    Vector<uint8_t>& isChanged = edits == Edits::OnlySetStorage ? m_isChangedIfLowered : m_isChanged;
    Vector<uint8_t>& isChangedForTable = edits == Edits::OnlySetStorage ? m_isChangedForTableIfLowered : m_isChangedForTable;
    for (FunctionID id : functions) {
        didTouch(id);
        if (id.index() >= isChanged.size())
            continue;
        isChanged[id.index()] = 1;
        isChangedForTable[id.index()] = 1;
        m_recordSays[id.index()] = std::max(m_recordSays[id.index()], records == Records::SayNoLess ? RecordSays::NoLess : RecordSays::MaybeLess);
    }
}

void BodySummaries::didChangeDoingNoMore(std::span<const FunctionID> functions, Edits edits)
{
    if (functions.empty())
        return;
    Vector<uint8_t>& isChangedForTable = edits == Edits::OnlySetStorage ? m_isChangedForTableIfLowered : m_isChangedForTable;
    for (FunctionID id : functions) {
        didTouch(id);
        if (id.index() >= isChangedForTable.size())
            continue;
        isChangedForTable[id.index()] = 1;
        m_wasLoweredSinceSummarized[id.index()] = 1;
        m_recordSays[id.index()] = std::max(m_recordSays[id.index()], RecordSays::NoLess);
    }
    m_hasUpperBounds = true;
}

void BodySummaries::didLower(std::span<const FunctionID> functions)
{
    if (functions.empty())
        return;
    m_borrowingBodies.didLower(functions);
    for (FunctionID id : functions) {
        didTouch(id);
        if (id.index() < m_wasLoweredSinceSummarized.size())
            m_wasLoweredSinceSummarized[id.index()] = 1;
    }
    m_hasUpperBounds = true;
}

void BodySummaries::countLoweredBodiesWithNewStorageAsChanged()
{
    if (!m_isChangedForTableIfLowered.contains(1))
        return;
    for (unsigned index = 0; index < m_isChangedForTableIfLowered.size(); ++index) {
        if (m_isChangedIfLowered[index] && m_wasLoweredSinceSummarized[index])
            m_isChanged[index] = 1;
        if (m_isChangedForTableIfLowered[index] && m_borrowingBodies.wasLoweredSinceScanned(FunctionID(index)))
            m_isChangedForTable[index] = 1;
    }
}

void BodySummaries::didAddEffects(std::span<const AddedEffects> added)
{
    m_addedEffects.appendContainerWithMapping(added, [&](const AddedEffects& effects) {
        return StoredAddedEffects { effects.function, m_masks->compress(effects.effects) };
    });
}

EffectSummarizer::EffectSummarizer(const Program& program, const FlowResults& flow, BuildContext& build, BodySummaries& kept, Witnesses witnessesWanted)
    : m_program(program)
    , m_flow(flow)
    , m_build(build)
    , m_kept(kept)
    , m_witnessesWanted(witnessesWanted)
    , m_numberOfFunctionsBefore(kept.m_bodies.size())
{
}

void EffectSummarizer::update(EffectSummaries& summaries)
{
    bringSeedUpToDate(summaries);
    finishUpdate(summaries);
}

void EffectSummarizer::bringSeedUpToDate(EffectSummaries& summaries)
{
    PhaseTimes times(m_build, PassID::ComputeEffectSummaries);
    RELEASE_ASSERT(summaries.size() == m_numberOfFunctionsBefore && m_kept.m_isChanged.size() == m_numberOfFunctionsBefore && m_numberOfFunctionsBefore <= m_program.functions.size());
    if (!summaries.m_masks)
        summaries = EffectSummaries(Ref { m_kept.m_masks }, EffectSummaries::Contents::Any);
    RELEASE_ASSERT(summaries.m_masks == m_kept.m_masks.ptr());
    m_kept.countLoweredBodiesWithNewStorageAsChanged();
    findRecordsThatLookDifferent();
    BorrowedParameters borrowed = m_kept.computeBorrowedParametersFromLatestTable(m_program, m_build, m_kept.m_stampsOfRecords.span(), m_recordLooksDifferent.span());
    m_kept.m_wasTouchedSinceLatestTable.clear();
    m_kept.m_stampsOfRecordsAtLatestTable.clear();
    times.didFinish("computeBorrowedParameters"_s);
    updateSeed(WTF::move(borrowed));
    times.didFinish("updateSeed"_s);
}

void EffectSummarizer::finishUpdate(EffectSummaries& summaries)
{
    PhaseTimes times(m_build, PassID::ComputeEffectSummaries);
    summarizeBodies();
    m_kept.m_isChanged.fill(0, m_program.functions.size());
    m_kept.m_isChangedForTable.fill(0, m_program.functions.size());
    m_kept.m_isChangedIfLowered.fill(0, m_program.functions.size());
    m_kept.m_isChangedForTableIfLowered.fill(0, m_program.functions.size());
    m_kept.m_recordSays.fill(BodySummaries::RecordSays::WhatItSaid, m_program.functions.size());
    times.didFinish("summarizeBodies"_s);
    findFunctionsEnteredByUnknownCode();
    if (solveAgain(summaries))
        times.didFinish("solveAgain"_s);
    else
        solveAll(summaries, times);
    if (m_build.options().logging.logsPhaseTimes && !m_isOnlyCheck) [[unlikely]]
        dataLogLn("AOT effect summaries: ", m_program.functions.size(), " functions | ", m_kept.m_numberOfNodes, " nodes | ", m_kept.m_masks->size(), " distinct location masks");
    if (!m_build.options().verifiesAfterEveryPass || m_isOnlyCheck || !m_numberOfFunctionsBefore)
        return;
    if (!m_kept.m_hasUpperBounds)
        checkAgainstSummariesFromNothing(summaries);
    else if (m_program.form() == IRForm::Prepared)
        checkCoversSummariesFromNothing(summaries);
}

namespace {

class Stamp {
public:
    void add(uint64_t word)
    {
        m_stamp = (m_stamp ^ word) * 0x9e3779b97f4a7c15ull;
        m_stamp ^= m_stamp >> 32;
    }

    void add(const DeclaredType& declared)
    {
        add(static_cast<uint64_t>(declared.type.index()) << 32 | declared.site.index());
        add(declared.flags.toRaw());
    }

    void add(const Vector<DeclaredType>& types)
    {
        add(types.size());
        for (const DeclaredType& declared : types)
            add(declared);
    }

    uint64_t value() const { return m_stamp; }

private:
    uint64_t m_stamp { 0 };
};

} // anonymous namespace

static void addPartsBesideRole(Stamp& stamp, const Function& function)
{
    stamp.add(function.flags.toRaw());
    stamp.add(static_cast<uint64_t>(function.length) << 32 | function.constructedClass.index());
    stamp.add(function.derivation.source.index());
    stamp.add(function.captures);
    stamp.add(function.thisType);
    stamp.add(function.parameters);
    stamp.add(function.results);
    stamp.add(function.functionTypes.size());
    for (FunctionID type : function.functionTypes)
        stamp.add(type.index());
}

uint64_t stampOfRecord(const Function& function)
{
    Stamp stamp;
    stamp.add(static_cast<uint64_t>(function.kind) << 32 | static_cast<uint64_t>(function.role) << 16 | static_cast<uint64_t>(function.creator.kind()) << 1 | function.hasBody());
    stamp.add(function.capabilities.toRaw());
    addPartsBesideRole(stamp, function);
    return stamp.value();
}

// The whole record, less what only a pass that is about to rewrite the function itself asks for.
uint64_t stampOfRecordForCallers(const Function& function)
{
    constexpr OptionSet<FunctionCapabilities> askedOnlyToRewriteFunction { FunctionCapabilities::MayChangeSignature, FunctionCapabilities::MayBeCloned };
    FunctionRole role = function.role == FunctionRole::EntryChecks ? FunctionRole::Source : function.role;
    Stamp stamp;
    stamp.add(static_cast<uint64_t>(function.kind) << 32 | static_cast<uint64_t>(role) << 16 | static_cast<uint64_t>(function.isUnsplitCoroutine()) << 1 | function.hasBody());
    stamp.add((function.capabilities - askedOnlyToRewriteFunction).toRaw());
    addPartsBesideRole(stamp, function);
    return stamp.value();
}

uint64_t stampOfEntry(const Function& function)
{
    Stamp stamp;
    stamp.add(static_cast<uint64_t>(function.kind) << 1 | function.hasBody());
    stamp.add(function.flags.toRaw());
    stamp.add(static_cast<uint64_t>(function.length) << 32 | function.constructedClass.index());
    stamp.add(function.captures);
    stamp.add(function.thisType);
    stamp.add(function.parameters);
    return stamp.value();
}

BorrowedParameters BodySummaries::computeBorrowedParametersOfChangedBodies(const Program& program, BuildContext& build)
{
    unsigned numberOfFunctions = program.functions.size();
    unsigned numberOfSummarizedFunctions = m_bodies.size();
    RELEASE_ASSERT(numberOfSummarizedFunctions <= numberOfFunctions && m_isChangedForTable.size() == numberOfSummarizedFunctions && m_stampsOfRecords.size() == numberOfSummarizedFunctions);
    countLoweredBodiesWithNewStorageAsChanged();
    Vector<uint64_t> stampsOfRecords(numberOfFunctions);
    Vector<uint8_t> recordLooksDifferent(numberOfSummarizedFunctions);
    forEachIndexInParallel(build.parallelLoops(), numberOfFunctions, [&](unsigned index) {
        stampsOfRecords[index] = stampOfRecord(program, FunctionID(index));
        if (index < numberOfSummarizedFunctions)
            recordLooksDifferent[index] = stampsOfRecords[index] != m_stampsOfRecords[index];
    });
    BorrowedParameters table = computeBorrowedParametersFromLatestTable(program, build, stampsOfRecords.span(), recordLooksDifferent.span());
    m_wasTouchedSinceLatestTable.fill(0, numberOfFunctions);
    m_stampsOfRecordsAtLatestTable = WTF::move(stampsOfRecords);
    return table;
}

BorrowedParameters BodySummaries::computeBorrowedParametersFromLatestTable(const Program& program, BuildContext& build, std::span<const uint64_t> stampsOfRecords, std::span<const uint8_t> recordLooksDifferent)
{
    if (m_stampsOfRecordsAtLatestTable.isEmpty())
        return computeBorrowedParameters(program, build, m_borrowingBodies, m_isChangedForTable.span(), recordLooksDifferent);

    unsigned numberOfKeptBodies = m_stampsOfRecordsAtLatestTable.size();
    unsigned numberOfSummarizedFunctions = m_isChangedForTable.size();
    RELEASE_ASSERT(numberOfSummarizedFunctions <= numberOfKeptBodies && numberOfKeptBodies <= stampsOfRecords.size() && m_wasTouchedSinceLatestTable.size() == numberOfKeptBodies);
    Vector<uint8_t> hasChangedSince(numberOfKeptBodies);
    Vector<uint8_t> recordLooksDifferentSince(numberOfKeptBodies);
    forEachIndexInParallel(build.parallelLoops(), (numberOfKeptBodies + functionsInChunk - 1) / functionsInChunk, [&](unsigned chunk) {
        for (unsigned index = chunk * functionsInChunk; index < std::min(numberOfKeptBodies, (chunk + 1) * functionsInChunk); ++index) {
            hasChangedSince[index] = (index >= numberOfSummarizedFunctions || m_isChangedForTable[index]) && m_wasTouchedSinceLatestTable[index];
            recordLooksDifferentSince[index] = stampsOfRecords[index] != m_stampsOfRecordsAtLatestTable[index];
        }
    });
    return computeBorrowedParameters(program, build, m_borrowingBodies, hasChangedSince.span(), recordLooksDifferentSince.span());
}

void EffectSummarizer::findRecordsThatLookDifferent()
{
    unsigned numberOfFunctions = m_program.functions.size();
    RELEASE_ASSERT(m_kept.m_stampsOfRecords.size() == m_numberOfFunctionsBefore);
    m_kept.m_stampsOfRecords.grow(numberOfFunctions);
    m_kept.m_stampsOfEntries.grow(numberOfFunctions);
    m_recordLooksDifferent.grow(m_numberOfFunctionsBefore);
    m_recordLooksDifferentToCallers.grow(m_numberOfFunctionsBefore);
    forEachIndexInParallel(m_build.parallelLoops(), numberOfFunctions, [&](unsigned index) {
        uint64_t stamp = stampOfRecord(m_program, FunctionID(index));
        uint64_t stampOfItsEntry = stampOfEntry(m_program, FunctionID(index));
        if (index < m_numberOfFunctionsBefore) {
            m_recordLooksDifferent[index] = stamp != m_kept.m_stampsOfRecords[index];
            m_recordLooksDifferentToCallers[index] = stampOfItsEntry != m_kept.m_stampsOfEntries[index] || (m_recordLooksDifferent[index] && m_kept.m_recordSays[index] != BodySummaries::RecordSays::NoLess);
        }
        m_kept.m_stampsOfRecords[index] = stamp;
        m_kept.m_stampsOfEntries[index] = stampOfItsEntry;
    });
}

void EffectSummarizer::updateSeed(BorrowedParameters&& borrowed)
{
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    Vector<uint64_t> contentsAreCalled(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        contentsAreCalled[index] = parametersWhoseContentsAreCalled(m_program, FunctionID(index), borrowed);
    });

    const Vector<uint64_t>& contentsWereCalled = m_kept.m_parametersWhoseContentsAreCalled;
    RELEASE_ASSERT(contentsWereCalled.size() == m_numberOfFunctionsBefore);
    Vector<uint32_t> functionsWithNewSeed = findIndicesInParallel(loops, m_numberOfFunctionsBefore, [&](unsigned index) {
        FunctionID id(index);
        return borrowed.wordOf(id) != m_kept.m_borrowed.wordOf(id) || contentsAreCalled[index] != contentsWereCalled[index];
    });
    if (!functionsWithNewSeed.isEmpty())
        m_hasNewSeed.fill(0, m_numberOfFunctionsBefore);
    for (uint32_t index : functionsWithNewSeed)
        m_hasNewSeed[index] = 1;

    m_kept.m_borrowed = WTF::move(borrowed);
    m_kept.m_parametersWhoseContentsAreCalled = WTF::move(contentsAreCalled);

    m_kept.m_seed.grow(numberOfFunctions, &loops);
    Vector<uint32_t> functionsToSeed = findIndicesInParallel(loops, numberOfFunctions, [&](unsigned index) {
        return index >= m_numberOfFunctionsBefore || m_recordLooksDifferent[index] || (!m_hasNewSeed.isEmpty() && m_hasNewSeed[index]);
    });
    Vector<uint32_t> numbersOfParameters(functionsToSeed.size());
    forEachIndexInParallel(loops, functionsToSeed.size(), [&](unsigned position) {
        numbersOfParameters[position] = m_program.functions[functionsToSeed[position]].numberOfEntryParameters();
    });
    m_kept.m_seed.reserveParameters(loops, std::accumulate(numbersOfParameters.begin(), numbersOfParameters.end(), static_cast<size_t>(0)));
    for (size_t position = 0; position < functionsToSeed.size(); ++position)
        m_kept.m_seed.setNumberOfParameters(functionsToSeed[position], numbersOfParameters[position]);
    forEachIndexInParallel(loops, functionsToSeed.size(), [&](unsigned position) {
        FunctionID id(functionsToSeed[position]);
        m_kept.m_seed.set(id.index(), createSummaryFromFlow(m_program, m_flow, m_kept.m_borrowed, m_build, id));
    });
    if (m_build.options().verifiesAfterEveryPass) {
        forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
            RELEASE_ASSERT(m_kept.m_seed.of(FunctionID(index)) == createSummaryFromFlow(m_program, m_flow, m_kept.m_borrowed, m_build, FunctionID(index)));
        });
    }
}

Vector<ReasonToSummarize> EffectSummarizer::findReasonsToSummarize() const
{
    const ParallelLoops& loops = m_build.parallelLoops();
    const Vector<uint8_t>& isChanged = m_kept.m_isChanged;
    unsigned numberOfFunctions = m_program.functions.size();

    Vector<uint8_t> looksDifferentToCallers = m_recordLooksDifferentToCallers;
    if (!m_hasNewSeed.isEmpty()) {
        for (unsigned index = 0; index < m_numberOfFunctionsBefore; ++index)
            looksDifferentToCallers[index] |= m_hasNewSeed[index];
    }
    const bool someFunctionLooksDifferent = looksDifferentToCallers.contains(1);

    Vector<ReasonToSummarize> reasons(FillWith { }, numberOfFunctions, ReasonToSummarize::IsNew);
    forEachIndexInParallel(loops, m_numberOfFunctionsBefore, [&](unsigned index) {
        auto namesFunctionThatLooksDifferent = [&] {
            bool namesOne = false;
            m_kept.m_bodies[index].forEachFunctionInDependencies(FunctionID(index), [&](FunctionID function) {
                namesOne |= !!looksDifferentToCallers[function.index()];
            });
            return namesOne;
        };
        auto hasNewSeed = [&](FunctionID function) {
            return function.index() < m_hasNewSeed.size() && m_hasNewSeed[function.index()];
        };
        if (isChanged[index] || m_recordLooksDifferent[index])
            reasons[index] = ReasonToSummarize::HasChanged;
        else if (!m_hasNewSeed.isEmpty() && m_hasNewSeed[index])
            reasons[index] = ReasonToSummarize::HasNewSeed;
        else if (someFunctionLooksDifferent && namesFunctionThatLooksDifferent())
            reasons[index] = ReasonToSummarize::NamesFunctionThatLooksDifferent;
        else if (!m_hasNewSeed.isEmpty() && std::ranges::any_of(m_kept.m_functionsOfClosuresMade[index], hasNewSeed))
            reasons[index] = ReasonToSummarize::MakesClosureWithNewSeed;
        else
            reasons[index] = ReasonToSummarize::None;
    });
    return reasons;
}

static Vector<FunctionID> functionsOfClosuresMadeBy(const Function& function)
{
    Vector<FunctionID> functions;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::NewClosure)
                functions.append(value.function());
        }
    }
    AOT::sort(functions);
    removeRepeatedElements(functions);
    return functions;
}

void EffectSummarizer::summarizeBodies()
{
    AOT_THRESHOLD(size_t, fewestValuesOfLongFunction, 4096);
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();

    auto summarize = [&](unsigned index, BodyReferenceCollector* references = nullptr) {
        FunctionID id(index);
        EffectContext context { m_program, m_kept.m_seed };
        std::unique_ptr<FunctionAnalyses> analyses = createAnalysesForSummary(m_build, context, m_flow, id, m_program.functions[index]);
        return summarizeBodyLocally(context, id, *analyses, references);
    };
    auto summarizeAndGiveReferences = [&](unsigned index) {
        if (!m_references)
            return summarize(index);
        BodyReferenceCollector references(m_program);
        BodySummary body = summarize(index, &references);
        if (references.hasReadBody())
            m_references->take(m_program, FunctionID(index), references.finish(m_flow, FunctionID(index)), m_kept.m_stampsOfRecords[index]);
        return body;
    };

    Vector<ReasonToSummarize> reasons = findReasonsToSummarize();
    Vector<uint32_t> functionsToSummarize = findIndicesInParallel(loops, numberOfFunctions, [&](unsigned index) {
        return reasons[index] != ReasonToSummarize::None;
    });
    Vector<uint64_t> longFunctions;
    for (uint32_t position = 0; position < functionsToSummarize.size(); ++position) {
        size_t numberOfValues = m_program.functions[functionsToSummarize[position]].values.size();
        if (numberOfValues >= fewestValuesOfLongFunction)
            longFunctions.append((UINT32_MAX - static_cast<uint64_t>(numberOfValues)) << 32 | position);
    }
    AOT::sort(longFunctions);
    Vector<uint32_t> longestFirst = longFunctions.map([](uint64_t key) {
        return static_cast<uint32_t>(key);
    });

    growWithRoomToSpareInParallel(loops, m_kept.m_bodies, numberOfFunctions);
    growWithRoomToSpareInParallel(loops, m_kept.m_functionsOfClosuresMade, numberOfFunctions);
    m_kept.m_wasLoweredSinceSummarized.insertFill(m_numberOfFunctionsBefore, 0, numberOfFunctions - m_numberOfFunctionsBefore);
    if (m_references)
        m_references->prepareToTake(m_program, loops, m_kept.m_stampsOfRecords.span());
    Vector<uint8_t> doesSomethingElse(FillWith { }, functionsToSummarize.size(), static_cast<uint8_t>(0));
    Vector<Vector<FunctionID>> functionsNamedForTheFirstTime(functionsToSummarize.size());
    forEachIndexInParallel(loops, functionsToSummarize.size(), longestFirst.span(), [&](unsigned position) {
        unsigned index = functionsToSummarize[position];
        BodySummary body = summarizeAndGiveReferences(index);
        StoredBodySummary stored(m_kept.m_masks, FunctionID(index), body);
        m_kept.m_wasLoweredSinceSummarized[index] = 0;
        m_kept.m_functionsOfClosuresMade[index] = functionsOfClosuresMadeBy(m_program.functions[index]);
        if (index < m_numberOfFunctionsBefore && stored == m_kept.m_bodies[index])
            return;
        doesSomethingElse[position] = 1;
        Vector<FunctionID, 32> functionsNamedBefore;
        m_kept.m_bodies[index].forEachFunctionInDependencies(FunctionID(index), [&](FunctionID named) {
            functionsNamedBefore.append(named);
        });
        stored.forEachFunctionInDependencies(FunctionID(index), [&](FunctionID named) {
            if (!std::ranges::binary_search(functionsNamedBefore, named))
                functionsNamedForTheFirstTime[position].append(named);
        });
        m_kept.m_bodies[index] = WTF::move(stored);
    });
    if (m_build.options().verifiesAfterEveryPass) {
        forEachIndexInParallel(loops, m_numberOfFunctionsBefore, [&](unsigned index) {
            RELEASE_ASSERT(reasons[index] != ReasonToSummarize::None || m_kept.m_wasLoweredSinceSummarized[index] || m_kept.m_bodies[index] == StoredBodySummary(m_kept.m_masks, FunctionID(index), summarize(index)));
        });
    }

    const bool updatesUserLists = m_kept.m_isSolved && static_cast<size_t>(std::ranges::count(doesSomethingElse, 1)) <= maxIncrementalSolveSize();
    CountsByReason numberOfBodiesSummarizedAsBefore { };
    for (uint32_t position = 0; position < functionsToSummarize.size(); ++position) {
        uint32_t index = functionsToSummarize[position];
        if (!doesSomethingElse[position]) {
            numberOfBodiesSummarizedAsBefore[static_cast<unsigned>(reasons[index])] += m_program.functions[index].hasBody();
            continue;
        }
        m_changedFunctions.append(index);
        if (!updatesUserLists)
            continue;
        for (FunctionID named : functionsNamedForTheFirstTime[position]) {
            m_kept.m_usersAddedSince.ensure(named.index(), [] {
                return Vector<FunctionID> { };
            }).iterator->value.append(FunctionID(index));
        }
    }

    if (!m_kept.m_addedEffects.isEmpty()) {
        InternedLocationMasks& masks = m_kept.m_masks;
        auto mergedWith = [&](CompactEffects effects, CompactEffects added) {
            return CompactEffects(masks.merge(effects.reads(), added.reads()), masks.merge(effects.writes(), added.writes()), effects.flags() | added.flags());
        };
        CompactEffects cachedAddedEffects;
        CompactEffects cachedMemory;
        for (const StoredAddedEffects& added : std::exchange(m_kept.m_addedEffects, { })) {
            unsigned index = added.function.index();
            if (reasons[index] != ReasonToSummarize::None)
                continue;
            StoredBodySummary& body = m_kept.m_bodies[index];
            CompactEffects onReturn = mergedWith(body.onReturn(), added.effects);
            CompactEffects onThrow = mergedWith(body.onThrow(), added.effects);
            bool doesMore = onReturn != body.onReturn() || onThrow != body.onThrow();
            body.setOnReturn(onReturn);
            body.setOnThrow(onThrow);
            if (added.effects != cachedAddedEffects) {
                cachedAddedEffects = added.effects;
                cachedMemory = masks.compress(OperandMemory { masks.at(added.effects.reads()).restrictedTo(perObjectHeaps), masks.at(added.effects.writes()).restrictedTo(perObjectHeaps) });
            }
            for (StoredParameterSummary& parameter : body.mutableParameters()) {
                CompactEffects merged = mergedWith(parameter.memory, cachedMemory);
                doesMore |= merged != parameter.memory;
                parameter.memory = merged;
            }
            if (doesMore)
                m_changedFunctions.append(index);
        }
        AOT::sort(m_changedFunctions);
        removeRepeatedElements(m_changedFunctions);
    }

    if (m_isOnlyCheck)
        return;
    m_build.statistics().count(StatisticID::BodySummariesKept, numberOfFunctions - functionsToSummarize.size());
    m_build.statistics().count(StatisticID::BodySummariesComputed, functionsToSummarize.size());
    if (m_build.options().logging.logsPhaseTimes) [[unlikely]]
        logReasons(reasons.span(), numberOfBodiesSummarizedAsBefore);
}

void EffectSummarizer::logReasons(std::span<const ReasonToSummarize> reasons, const CountsByReason& numberOfBodiesSummarizedAsBefore) const
{
    CountsByReason counts { };
    for (unsigned index = 0; index < reasons.size(); ++index) {
        if (reasons[index] == ReasonToSummarize::None || m_program.functions[index].hasBody())
            ++counts[static_cast<unsigned>(reasons[index])];
    }
    auto countOf = [&](ReasonToSummarize reason) {
        return counts[static_cast<unsigned>(reason)];
    };
    auto sameOf = [&](ReasonToSummarize reason) {
        return numberOfBodiesSummarizedAsBefore[static_cast<unsigned>(reason)];
    };
    dataLogLn("AOT body summaries: kept ", countOf(ReasonToSummarize::None), " | of bodies (and of those with the summary they had): new ", countOf(ReasonToSummarize::IsNew),
        " | changed ", countOf(ReasonToSummarize::HasChanged), " (", sameOf(ReasonToSummarize::HasChanged), ")",
        " | new seed ", countOf(ReasonToSummarize::HasNewSeed), " (", sameOf(ReasonToSummarize::HasNewSeed), ")",
        " | names a function that looks different ", countOf(ReasonToSummarize::NamesFunctionThatLooksDifferent), " (", sameOf(ReasonToSummarize::NamesFunctionThatLooksDifferent), ")",
        " | makes a closure with a new seed ", countOf(ReasonToSummarize::MakesClosureWithNewSeed), " (", sameOf(ReasonToSummarize::MakesClosureWithNewSeed), ")");
}

void EffectSummarizer::findFunctionsEnteredByUnknownCode()
{
    unsigned numberOfFunctions = m_program.functions.size();
    BitVector isCalledIndirectly = functionsCalledIndirectly(m_flow, numberOfFunctions);
    m_kept.m_isEnteredByUnknownCode.insertFill(m_kept.m_isEnteredByUnknownCode.size(), 0, numberOfFunctions - m_kept.m_isEnteredByUnknownCode.size());
    Vector<uint32_t> functionsWithAnotherAnswer = findIndicesInParallel(m_build.parallelLoops(), numberOfFunctions, [&](unsigned index) {
        const Function& function = m_program.functions[index];
        bool isEntered = function.hasBody() && (isCalledIndirectly.get(index) || mayBeEnteredByUnknownCode(m_program, m_flow, FunctionID(index), function, m_kept.m_bodies[index]));
        return isEntered != !!m_kept.m_isEnteredByUnknownCode[index];
    });
    if (functionsWithAnotherAnswer.isEmpty())
        return;
    for (uint32_t index : functionsWithAnotherAnswer)
        m_kept.m_isEnteredByUnknownCode[index] ^= 1;
    m_changedFunctions.appendVector(functionsWithAnotherAnswer);
    AOT::sort(m_changedFunctions);
    removeRepeatedElements(m_changedFunctions);
    m_entryFlippedFunctions = WTF::move(functionsWithAnotherAnswer);
}

uint32_t EffectSummarizer::indexOf(const SummaryNode& node) const
{
    switch (node.part) {
    case SummaryPart::OnReturn:
        return m_kept.m_firstNode[node.function.index()];
    case SummaryPart::OnThrow:
        return m_kept.m_firstNode[node.function.index()] + 1;
    case SummaryPart::Parameter:
        RELEASE_ASSERT(node.parameter + 2 < m_kept.m_numberOfNodesOf[node.function.index()]);
        return m_kept.m_firstNode[node.function.index()] + 2 + node.parameter;
    case SummaryPart::UnknownCode:
        return unknownCodeNode();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t EffectSummarizer::indexOf(FunctionID function, uint32_t nodeOfFunction) const
{
    RELEASE_ASSERT(nodeOfFunction < m_kept.m_numberOfNodesOf[function.index()]);
    return m_kept.m_firstNode[function.index()] + nodeOfFunction;
}

Effects EffectSummarizer::effectsOfUnknownCodeAlone() const
{
    Effects unknown = Effects::unknownCode();
    unknown.writes.remove(compiledCodeHeaps);
    for (unsigned index = 0; index < m_program.variables.size(); ++index) {
        const Variable& variable = m_program.variables[index];
        bool isStoredThroughClassObject = variable.owner && variable.flow.storeNaming != StoreNaming::EveryStoreNamesLocation;
        if (variable.flow.writers != WriterState::AllKnown || isStoredThroughClassObject)
            unknown.writes.add(m_program, MemoryLocation::moduleVariable(VariableID(index)));
    }
    for (unsigned index = 0; index < m_program.layouts.size(); ++index) {
        const Layout& layout = m_program.layouts[index];
        if (!layout.isEnvironment())
            continue;
        LayoutID id(index);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (flowOf(m_program, FieldReference { id, field }).location.writers != WriterState::AllKnown)
                unknown.writes.add(m_program, MemoryLocation::environmentField(id, field));
        }
    }
    return unknown;
}

Effects EffectSummarizer::solvedEffectsOf(uint32_t node) const
{
    Effects effects = m_kept.m_masks->expand(m_kept.m_effects[node]);
    if (m_kept.m_takesInUnknownCode[node])
        effects.merge(m_kept.m_unknownCode);
    return effects;
}

void EffectSummarizer::uniteEffectsEnteredInChunk(unsigned chunk)
{
    unsigned end = std::min<unsigned>(m_program.functions.size(), (chunk + 1) * BodySummaries::functionsInChunk);
    Effects united = Effects::none();
    uint8_t takesInUnknownCode = 0;
    for (unsigned index = chunk * BodySummaries::functionsInChunk; index < end; ++index) {
        if (!m_kept.m_isEnteredByUnknownCode[index])
            continue;
        uint32_t first = m_kept.m_firstNode[index];
        for (uint32_t node = first; node < first + m_kept.m_numberOfNodesOf[index]; ++node) {
            m_kept.m_masks->mergeInto(united, m_kept.m_effects[node]);
            takesInUnknownCode |= m_kept.m_takesInUnknownCode[node];
        }
    }
    m_kept.m_effectsEnteredInChunk[chunk] = united;
    m_kept.m_takesInUnknownCodeInChunk[chunk] = takesInUnknownCode;
}

bool EffectSummarizer::updateEffectsOfUnknownCode()
{
    Effects unknownCode = effectsOfUnknownCodeAlone();
    for (const Effects& part : m_kept.m_effectsEnteredInChunk)
        unknownCode.merge(part);
    if (m_kept.m_takesInUnknownCodeInChunk.contains(1))
        unknownCode.mayThrow = true;
    if (unknownCode == m_kept.m_unknownCode)
        return false;
    m_kept.m_unknownCode = unknownCode;
    return true;
}

uint32_t EffectSummarizer::MemoByMasks::get(uint32_t id, NOESCAPE const Invocable<uint32_t()> auto& compute)
{
    if (id >= m_resultsPlusOne.size())
        return compute();
    std::atomic<uint32_t>& slot = m_resultsPlusOne[id];
    if (uint32_t resultPlusOne = slot.load(std::memory_order_acquire))
        return resultPlusOne - 1;
    uint32_t result = compute();
    slot.store(result + 1, std::memory_order_release);
    return result;
}

void EffectSummarizer::MemoByMasks::reset(size_t size)
{
    m_resultsPlusOne = Vector<std::atomic<uint32_t>>(size);
    for (auto& slot : m_resultsPlusOne)
        slot.store(0, std::memory_order_relaxed);
}

void EffectSummarizer::prepareToAssemble()
{
    size_t numberOfMasks = m_kept.m_masks->size();
    m_readsWithUnknownCode.reset(numberOfMasks);
    m_writesWithUnknownCode.reset(numberOfMasks);
    m_masksOfPerObjectHeaps.reset(numberOfMasks);
    m_compactUnknownCode = m_kept.m_masks->compress(m_kept.m_unknownCode);
}

CompactEffects EffectSummarizer::compactSolvedEffectsOf(uint32_t node)
{
    InternedLocationMasks& masks = m_kept.m_masks;
    CompactEffects effects = m_kept.m_effects[node];
    if (!m_kept.m_takesInUnknownCode[node])
        return effects;
    uint32_t reads = m_readsWithUnknownCode.get(effects.reads(), [&] {
        return masks.merge(effects.reads(), m_compactUnknownCode.reads());
    });
    uint32_t writes = m_writesWithUnknownCode.get(effects.writes(), [&] {
        return masks.merge(effects.writes(), m_compactUnknownCode.writes());
    });
    return CompactEffects(reads, writes, effects.flags() | m_compactUnknownCode.flags());
}

void EffectSummarizer::assemble(EffectSummaries& summaries, FunctionID id)
{
    constexpr uint8_t flagsOfBothEdges = CompactEffects::MayThrow | CompactEffects::MayThrowByRule | CompactEffects::MayCollect | CompactEffects::MayBeTerminated;
    InternedLocationMasks& masks = m_kept.m_masks;
    auto restrictedToPerObjectHeaps = [&](uint32_t masksOfNode) {
        return m_masksOfPerObjectHeaps.get(masksOfNode, [&] {
            return masks.intern(masks.at(masksOfNode).restrictedTo(perObjectHeaps));
        });
    };

    uint32_t onReturn = indexOf(SummaryNode::onReturn(id));
    uint32_t onThrow = indexOf(SummaryNode::onThrow(id));
    StoredFunctionEffects effects;
    effects.normal = compactSolvedEffectsOf(onReturn).withoutSuspension();
    effects.handler = compactSolvedEffectsOf(onThrow).withoutSuspension();
    uint8_t sharedFlags = (effects.normal.flags() | effects.handler.flags()) & flagsOfBothEdges;
    effects.normal = effects.normal.withFlags(effects.normal.flags() | sharedFlags);
    effects.handler = sharedFlags & CompactEffects::MayThrow ? effects.handler.withFlags(effects.handler.flags() | sharedFlags) : CompactEffects();

    std::span<const StoredParameterSummary> parametersOfBody = m_kept.m_bodies[id.index()].parameters();
    std::span<StoredParameterSummary> parameters = summaries.parametersOf(id.index());
    RELEASE_ASSERT(parameters.size() == parametersOfBody.size());
    uint32_t readsThroughParameters = InternedLocationMasks::empty;
    uint32_t writesThroughParameters = InternedLocationMasks::empty;
    for (uint32_t parameter = 0; parameter < parameters.size(); ++parameter) {
        CompactEffects memory = compactSolvedEffectsOf(onReturn + 2 + parameter);
        uint32_t reads = restrictedToPerObjectHeaps(memory.reads());
        uint32_t writes = restrictedToPerObjectHeaps(memory.writes());
        parameters[parameter] = parametersOfBody[parameter];
        parameters[parameter].memory = CompactEffects(reads, writes);
        readsThroughParameters = masks.merge(readsThroughParameters, reads);
        writesThroughParameters = masks.merge(writesThroughParameters, writes);
    }
    effects.throughParameters = CompactEffects(readsThroughParameters, writesThroughParameters);
    effects.computeEffectsOnCall(masks);

    UnknownCodeWitness witness;
    if (m_witnessesWanted == Witnesses::Found && (effects.normal.mayCallUnknownCode() || effects.handler.mayCallUnknownCode()))
        witness = m_witnesses[onReturn] ? m_witnesses[onReturn] : m_witnesses[onThrow];
    summaries.set(id.index(), effects, witness);

    if (m_build.options().verifiesAfterEveryPass) [[unlikely]] {
        Vector<ParameterSummary> expectedParameters(parameters.size(), [&](size_t parameter) {
            Effects memory = solvedEffectsOf(onReturn + 2 + parameter);
            return ParameterSummary { parametersOfBody[parameter].operand(), OperandMemory { memory.reads.restrictedTo(perObjectHeaps), memory.writes.restrictedTo(perObjectHeaps) } };
        });
        FunctionSummary expected = createFunctionSummary(solvedEffectsOf(onReturn), solvedEffectsOf(onThrow), WTF::move(expectedParameters), witness);
        FunctionSummaryView assembled = summaries.of(id);
        RELEASE_ASSERT(assembled == expected && assembled.onCall() == expected.onCall());
    }
}

size_t EffectSummarizer::maxIncrementalSolveSize() const
{
    AOT_THRESHOLD(size_t, fewestFunctionsSolvedAgain, 64);
    AOT_THRESHOLD(size_t, smallestShareOfProgramSolvedWhole, 8);
    return std::max<size_t>(fewestFunctionsSolvedAgain, m_program.functions.size() / smallestShareOfProgramSolvedWhole);
}

void EffectSummarizer::forEachUser(uint32_t index, NOESCAPE const Invocable<void(FunctionID)> auto& functor) const
{
    if (index + 1 < m_kept.m_firstUser.size()) {
        for (FunctionID user : m_kept.m_users.subspan(m_kept.m_firstUser[index], m_kept.m_firstUser[index + 1] - m_kept.m_firstUser[index]))
            functor(user);
    }
    if (auto added = m_kept.m_usersAddedSince.find(index); added != m_kept.m_usersAddedSince.end()) {
        for (FunctionID user : added->value)
            functor(user);
    }
}

void EffectSummarizer::forEachSourceFunction(FunctionID user, NOESCAPE const Invocable<void(FunctionID)> auto& functor) const
{
    Vector<uint32_t, 16> unknownCodeTargets;
    m_kept.m_bodies[user.index()].forEachGroupOfDependencies([&](FunctionID source, const StoredBodySummary::DependencyGroup& group) {
        if (!source) {
            group.forEach([&](uint32_t, uint32_t ownNode) {
                unknownCodeTargets.append(ownNode);
            });
            return;
        }
        if (source == user)
            return;
        bool hasEdge = !m_kept.m_isEnteredByUnknownCode[source.index()];
        if (!hasEdge) {
            group.forEach([&](uint32_t, uint32_t ownNode) {
                hasEdge |= !unknownCodeTargets.contains(ownNode);
            });
        }
        if (hasEdge)
            functor(source);
    });
}

bool EffectSummarizer::hasEdgeFrom(FunctionID user, FunctionID function) const
{
    if (!m_kept.m_isEnteredByUnknownCode[function.index()])
        return true;
    Vector<uint32_t, 16> unknownCodeTargets;
    bool hasEdge = false;
    m_kept.m_bodies[user.index()].forEachGroupOfDependencies([&](FunctionID source, const StoredBodySummary::DependencyGroup& group) {
        if (source && source != function)
            return;
        group.forEach([&](uint32_t, uint32_t ownNode) {
            if (!source)
                unknownCodeTargets.append(ownNode);
            else
                hasEdge |= !unknownCodeTargets.contains(ownNode);
        });
    });
    return hasEdge;
}

static constexpr uint8_t settled = 0;
static constexpr uint8_t affected = 1;
static constexpr uint8_t pending = 2;

Vector<uint8_t> EffectSummarizer::markTransitiveUsers(std::span<const uint32_t> functions) const
{
    const ParallelLoops& loops = m_build.parallelLoops();
    Vector<uint8_t> states(FillWith { }, m_program.functions.size(), settled);
    for (uint32_t index : functions)
        states[index] = affected;
    Vector<uint32_t> frontier(functions);
    while (!frontier.isEmpty()) {
        frontier = collectInParallel<uint32_t>(loops, frontier.size(), [&](unsigned position, Vector<uint32_t>& found) {
            FunctionID function(frontier[position]);
            forEachUser(function.index(), [&](FunctionID user) {
                AtomicRef userState(states[user.index()]);
                if (userState.load(std::memory_order_relaxed) || !hasEdgeFrom(user, function))
                    return;
                if (!userState.exchange(affected, std::memory_order_relaxed))
                    found.append(user.index());
            });
        });
    }
    return states;
}

std::optional<Vector<uint32_t>> EffectSummarizer::addAffectedSources(std::span<const uint32_t> functions, Vector<uint8_t>& states, size_t limit) const
{
    Vector<uint32_t> found(functions);
    for (size_t position = 0; position < found.size() && found.size() <= limit; ++position) {
        forEachSourceFunction(FunctionID(found[position]), [&](FunctionID source) {
            if (states[source.index()] != affected)
                return;
            states[source.index()] = pending;
            found.append(source.index());
        });
    }
    if (found.size() > limit) {
        for (uint32_t index : found.subspan(functions.size()))
            states[index] = affected;
        return std::nullopt;
    }
    AOT::sort(found);
    return found;
}

void EffectSummarizer::numberNodesOfChangedFunctions()
{
    unsigned numberOfFunctions = m_program.functions.size();
    m_kept.m_firstNode.grow(numberOfFunctions);
    m_kept.m_numberOfNodesOf.insertFill(m_kept.m_numberOfNodesOf.size(), 0, numberOfFunctions - m_kept.m_numberOfNodesOf.size());
    uint32_t numberOfNodes = m_kept.m_numberOfNodes;
    for (uint32_t index : m_changedFunctions) {
        uint32_t numberOfOwnNodes = 2 + m_kept.m_bodies[index].numberOfParameters();
        if (m_kept.m_numberOfNodesOf[index] == numberOfOwnNodes)
            continue;
        m_kept.m_firstNode[index] = numberOfNodes;
        m_kept.m_numberOfNodesOf[index] = numberOfOwnNodes;
        numberOfNodes += numberOfOwnNodes;
        m_renumberedFunctions.append(index);
    }
    if (numberOfNodes == m_kept.m_numberOfNodes)
        return;
    m_kept.m_numberOfNodes = numberOfNodes;
    if (m_kept.m_effects.size() < numberOfNodes)
        growInParallel(m_build.parallelLoops(), m_kept.m_effects, numberOfNodes);
    m_kept.m_takesInUnknownCode.grow(numberOfNodes);
}

static void forEachPosition(const ParallelLoops& loops, size_t count, const Invocable<void(unsigned)> auto& functor)
{
    AOT_THRESHOLD(size_t, fewestForThreads, 512);
    if (count >= fewestForThreads) {
        forEachIndexInParallel(loops, static_cast<unsigned>(count), functor);
        return;
    }
    for (unsigned position = 0; position < count; ++position)
        functor(position);
}

static constexpr uint32_t isNotSolvedAnew = UINT32_MAX;

Vector<uint32_t> EffectSummarizer::solveFunctions(std::span<const uint32_t> functions, Vector<uint32_t>& firstPositionOf)
{
    const ParallelLoops& loops = m_build.parallelLoops();
    InternedLocationMasks& masks = m_kept.m_masks;
    uint32_t count = 0;
    for (uint32_t index : functions) {
        firstPositionOf[index] = count;
        count += m_kept.m_numberOfNodesOf[index];
    }

    Vector<Effects> effects = createVectorInParallel(loops, count, Effects::none());
    Vector<uint8_t> namesUnknownCode(FillWith { }, count, static_cast<uint8_t>(0));
    Vector<uint32_t> firstSource(FillWith { }, count + 1, 0u);
    forEachPosition(loops, functions.size(), [&](unsigned position) {
        uint32_t index = functions[position];
        const StoredBodySummary& body = m_kept.m_bodies[index];
        uint32_t first = firstPositionOf[index];
        RELEASE_ASSERT(body.numberOfParameters() + 2 == m_kept.m_numberOfNodesOf[index]);
        effects[first] = masks.expand(body.onReturn().withoutSuspension());
        effects[first + 1] = masks.expand(body.onThrow().withoutSuspension());
        for (uint32_t parameter = 0; parameter < body.numberOfParameters(); ++parameter)
            effects[first + 2 + parameter] = masks.expand(body.parameters()[parameter].memory);
        body.forEachGroupOfDependencies([&](FunctionID source, const StoredBodySummary::DependencyGroup& group) {
            if (source)
                return;
            group.forEach([&](uint32_t, uint32_t targetNode) {
                namesUnknownCode[first + targetNode] = 1;
            });
        });
    });
    Vector<uint8_t> takesInUnknownCode = namesUnknownCode;
    auto forEachListedSourceOf = [&](uint32_t index, const auto& functor) {
        uint32_t first = firstPositionOf[index];
        m_kept.m_bodies[index].forEachGroupOfDependencies([&](FunctionID source, const StoredBodySummary::DependencyGroup& group) {
            if (!source)
                return;
            bool isEnteredByUnknownCode = m_kept.m_isEnteredByUnknownCode[source.index()];
            group.forEach([&](uint32_t sourceNode, uint32_t targetNode) {
                bool comesThroughUnknownCode = isEnteredByUnknownCode && namesUnknownCode[first + targetNode];
                if (!comesThroughUnknownCode)
                    functor(source, sourceNode, first + targetNode);
            });
        });
    };
    forEachPosition(loops, functions.size(), [&](unsigned position) {
        forEachListedSourceOf(functions[position], [&](FunctionID function, uint32_t sourceNode, uint32_t target) {
            if (firstPositionOf[function.index()] != isNotSolvedAnew) {
                ++firstSource[target];
                return;
            }
            uint32_t source = indexOf(function, sourceNode);
            masks.mergeInto(effects[target], m_kept.m_effects[source]);
            takesInUnknownCode[target] |= m_kept.m_takesInUnknownCode[source];
        });
    });
    uint32_t numberOfSources = 0;
    for (uint32_t position = 0; position < count; ++position)
        numberOfSources += std::exchange(firstSource[position], numberOfSources);
    firstSource[count] = numberOfSources;
    Vector<uint32_t> sources(numberOfSources);
    Vector<uint32_t> nextSource(firstSource.span().first(count));
    forEachPosition(loops, functions.size(), [&](unsigned position) {
        forEachListedSourceOf(functions[position], [&](FunctionID function, uint32_t sourceNode, uint32_t target) {
            if (firstPositionOf[function.index()] == isNotSolvedAnew)
                return;
            RELEASE_ASSERT(sourceNode < m_kept.m_numberOfNodesOf[function.index()]);
            sources[nextSource[target]++] = firstPositionOf[function.index()] + sourceNode;
        });
    });
    auto sourcesAt = [&](unsigned position) {
        return sources.subspan(firstSource[position], firstSource[position + 1] - firstSource[position]);
    };

    StronglyConnectedComponents components = computeStronglyConnectedComponents(count, sourcesAt);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        Effects united = Effects::none();
        uint8_t unitedTakesInUnknownCode = 0;
        std::span<const uint32_t> members = components.membersOf(component);
        for (uint32_t member : members) {
            united.merge(effects[member]);
            unitedTakesInUnknownCode |= takesInUnknownCode[member];
            for (uint32_t source : sourcesAt(member)) {
                if (components.componentOf[source] == component)
                    continue;
                united.merge(effects[source]);
                unitedTakesInUnknownCode |= takesInUnknownCode[source];
            }
        }
        if (components.isInCycle.quickGet(members[0]))
            united.mayThrow = true;
        for (uint32_t member : members) {
            effects[member] = united;
            takesInUnknownCode[member] = unitedTakesInUnknownCode;
        }
    }

    Vector<uint8_t> effectsChanged(FillWith { }, functions.size(), static_cast<uint8_t>(0));
    forEachPosition(loops, functions.size(), [&](unsigned position) {
        uint32_t index = functions[position];
        uint32_t firstNode = m_kept.m_firstNode[index];
        uint32_t firstPosition = firstPositionOf[index];
        bool isDifferent = std::ranges::binary_search(m_renumberedFunctions, index);
        for (uint32_t ownNode = 0; ownNode < m_kept.m_numberOfNodesOf[index]; ++ownNode) {
            CompactEffects solvedEffects = masks.compress(effects[firstPosition + ownNode]);
            uint8_t solvedTakesInUnknownCode = takesInUnknownCode[firstPosition + ownNode];
            isDifferent = isDifferent || solvedEffects != m_kept.m_effects[firstNode + ownNode] || solvedTakesInUnknownCode != m_kept.m_takesInUnknownCode[firstNode + ownNode];
            m_kept.m_effects[firstNode + ownNode] = solvedEffects;
            m_kept.m_takesInUnknownCode[firstNode + ownNode] = solvedTakesInUnknownCode;
        }
        effectsChanged[position] = isDifferent;
    });

    Vector<uint32_t> functionsWithChangedEffects;
    for (size_t position = 0; position < functions.size(); ++position) {
        firstPositionOf[functions[position]] = isNotSolvedAnew;
        if (effectsChanged[position])
            functionsWithChangedEffects.append(functions[position]);
    }
    return functionsWithChangedEffects;
}

bool EffectSummarizer::solveAgain(EffectSummaries& summaries)
{
    AOT_THRESHOLD(unsigned, maxSearches, 3);
    AOT_THRESHOLD(size_t, minExtraBudget, 64);
    AOT_THRESHOLD(size_t, extraBudgetDivisor, 64);
    if (!m_kept.m_isSolved || m_witnessesWanted == Witnesses::Found)
        return false;

    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    if (m_changedFunctions.size() > maxIncrementalSolveSize())
        return false;

    Vector<uint32_t> pendingFunctions = m_changedFunctions;
    for (uint32_t index : m_entryFlippedFunctions) {
        forEachUser(index, [&](FunctionID user) {
            pendingFunctions.append(user.index());
        });
    }
    AOT::sort(pendingFunctions);
    removeRepeatedElements(pendingFunctions);
    const size_t extraBudget = std::max(minExtraBudget, numberOfFunctions / extraBudgetDivisor);
    const size_t budget = std::min(maxIncrementalSolveSize(), pendingFunctions.size() + extraBudget);
    if (pendingFunctions.size() > budget)
        return false;

    Vector<uint32_t> functions;
    Vector<uint8_t> states;
    unsigned numberOfSearches = 0;
    bool statesAreStale = false;
    auto recomputeStates = [&] {
        states = markTransitiveUsers(pendingFunctions.span());
        for (uint32_t index : pendingFunctions)
            states[index] = pending;
        for (uint32_t index : functions)
            states[index] = settled;
        statesAreStale = false;
        ++numberOfSearches;
    };
    recomputeStates();

    numberNodesOfChangedFunctions();
    Vector<uint32_t> firstPositionOf(FillWith { }, numberOfFunctions, isNotSolvedAnew);
    unsigned numberOfRounds = 0;
    auto giveUp = [&] {
        if (m_build.options().logging.logsPhaseTimes) [[unlikely]]
            dataLogLn("AOT effect summaries: ", m_changedFunctions.size(), " functions do something else | gave up after solving ", functions.size(), " | rounds ", numberOfRounds, " | searches ", numberOfSearches);
        return false;
    };
    while (!pendingFunctions.isEmpty()) {
        size_t remainingBudget = budget - functions.size();
        size_t batchLimit = std::min(remainingBudget, 4 * pendingFunctions.size() + 1024);
        std::optional<Vector<uint32_t>> batch = addAffectedSources(pendingFunctions.span(), states, batchLimit);
        if (!batch && pendingFunctions.size() > extraBudget)
            return giveUp();
        if (!batch && statesAreStale && numberOfSearches < maxSearches) {
            recomputeStates();
            continue;
        }
        if (!batch) {
            Vector<uint32_t> readyFunctions;
            for (uint32_t index : pendingFunctions) {
                bool isReady = true;
                forEachSourceFunction(FunctionID(index), [&](FunctionID source) {
                    isReady &= states[source.index()] == settled;
                });
                if (isReady)
                    readyFunctions.append(index);
            }
            if (!readyFunctions.isEmpty()) {
                AOT::sort(readyFunctions);
                batch = WTF::move(readyFunctions);
            } else if (batchLimit < remainingBudget)
                batch = addAffectedSources(pendingFunctions.span(), states, remainingBudget);
        }
        if (!batch || batch->size() > remainingBudget)
            return giveUp();

        Vector<uint32_t> functionsWithChangedEffects = solveFunctions(batch->span(), firstPositionOf);
        for (uint32_t index : *batch)
            states[index] = settled;
        functions.appendVector(*batch);
        statesAreStale = true;
        ++numberOfRounds;

        pendingFunctions.removeAllMatching([&](uint32_t index) {
            return states[index] != pending;
        });
        for (uint32_t index : functionsWithChangedEffects) {
            forEachUser(index, [&](FunctionID user) {
                if (states[user.index()] != affected || !hasEdgeFrom(user, FunctionID(index)))
                    return;
                states[user.index()] = pending;
                pendingFunctions.append(user.index());
            });
        }
    }
    AOT::sort(functions);

    unsigned numberOfChunks = (numberOfFunctions + BodySummaries::functionsInChunk - 1) / BodySummaries::functionsInChunk;
    m_kept.m_effectsEnteredInChunk.insertFill(m_kept.m_effectsEnteredInChunk.size(), Effects::none(), numberOfChunks - m_kept.m_effectsEnteredInChunk.size());
    m_kept.m_takesInUnknownCodeInChunk.insertFill(m_kept.m_takesInUnknownCodeInChunk.size(), 0, numberOfChunks - m_kept.m_takesInUnknownCodeInChunk.size());
    Vector<uint32_t> chunks;
    for (uint32_t index : functions) {
        uint32_t chunk = index / BodySummaries::functionsInChunk;
        if (chunks.isEmpty() || chunks.last() != chunk)
            chunks.append(chunk);
    }
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned position) {
        uniteEffectsEnteredInChunk(chunks[position]);
    });

    summaries.grow(numberOfFunctions);
    for (uint32_t index : functions)
        summaries.setNumberOfParameters(index, m_kept.m_bodies[index].numberOfParameters());
    bool unknownCodeDoesSomethingElse = updateEffectsOfUnknownCode();
    prepareToAssemble();
    if (unknownCodeDoesSomethingElse) {
        summaries.m_unknownCode = m_kept.m_unknownCode;
        forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
            assemble(summaries, FunctionID(index));
        });
    } else {
        forEachPosition(loops, functions.size(), [&](unsigned position) {
            assemble(summaries, FunctionID(functions[position]));
        });
    }
    if (m_build.options().logging.logsPhaseTimes) [[unlikely]]
        dataLogLn("AOT effect summaries: ", m_changedFunctions.size(), " functions do something else | solved again ", functions.size(), " | rounds ", numberOfRounds, " | searches ", numberOfSearches);
    return true;
}

void EffectSummarizer::numberAllNodes()
{
    unsigned numberOfFunctions = m_program.functions.size();
    m_kept.m_firstNode.grow(numberOfFunctions);
    m_kept.m_numberOfNodesOf.grow(numberOfFunctions);
    uint32_t firstNode = 0;
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        uint32_t numberOfOwnNodes = 2 + m_kept.m_bodies[index].numberOfParameters();
        m_kept.m_firstNode[index] = firstNode;
        m_kept.m_numberOfNodesOf[index] = numberOfOwnNodes;
        firstNode += numberOfOwnNodes;
    }
    m_kept.m_numberOfNodes = firstNode;
}

void EffectSummarizer::solveAll(EffectSummaries& summaries, PhaseTimes& times)
{
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    numberAllNodes();
    createNodes();
    times.didFinish("createNodes"_s);
    if (m_witnessesWanted == Witnesses::Omitted)
        propagateEffects(times);
    else {
        runTasksInParallel(loops, {
            [&] {
                propagateEffects(times);
            },
            [&] {
                propagateWitnesses();
            },
        });
        times.didFinish("propagateWitnesses"_s);
    }
    summaries.clear();
    summaries.grow(numberOfFunctions, &m_build.parallelLoops());
    if (m_witnessesWanted == Witnesses::Found)
        summaries.m_witnesses.grow(numberOfFunctions);
    summaries.setParameterCounts([&](unsigned index) {
        return m_kept.m_bodies[index].numberOfParameters();
    }, &m_build.parallelLoops());
    summaries.m_unknownCode = m_kept.m_unknownCode;
    prepareToAssemble();
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        assemble(summaries, FunctionID(index));
    });
    if (!m_kept.m_isSolved || m_changedFunctions.size() > maxIncrementalSolveSize())
        listUsers();
    m_kept.m_isSolved = true;
    times.didFinish("assemble"_s);
}

void EffectSummarizer::createNodes()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    unsigned numberOfNodes = m_kept.m_numberOfNodes + 1;
    const Vector<uint8_t>& isEnteredByUnknownCode = m_kept.m_isEnteredByUnknownCode;

    Vector<uint32_t> firstSourceIn(numberOfFunctions);
    Vector<uint32_t> firstNodeEntered(numberOfFunctions);
    uint64_t numberOfDependencies = 0;
    uint64_t numberOfNodesEntered = 0;
    for (unsigned index = 0; index < numberOfFunctions; ++index) {
        firstSourceIn[index] = static_cast<uint32_t>(numberOfDependencies);
        firstNodeEntered[index] = static_cast<uint32_t>(numberOfNodesEntered);
        numberOfDependencies += m_kept.m_bodies[index].numberOfDependencies();
        numberOfNodesEntered += isEnteredByUnknownCode[index] ? m_kept.m_numberOfNodesOf[index] : 0;
    }
    RELEASE_ASSERT(numberOfDependencies + numberOfNodesEntered <= UINT32_MAX);

    m_kept.m_effects.clear();
    growInParallel(loops, m_kept.m_effects, numberOfNodes);
    if (m_witnessesWanted == Witnesses::Found)
        m_witnesses.grow(numberOfNodes);
    growInParallel(loops, m_firstSource, numberOfNodes);
    growInParallel(loops, m_endOfSources, numberOfNodes);
    growInParallel(loops, m_sources, numberOfDependencies + numberOfNodesEntered);

    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        FunctionID id(index);
        const StoredBodySummary& body = m_kept.m_bodies[index];
        unsigned onReturn = indexOf(SummaryNode::onReturn(id));
        unsigned onThrow = indexOf(SummaryNode::onThrow(id));
        unsigned numberOfOwnNodes = m_kept.m_numberOfNodesOf[index];

        m_kept.m_effects[onReturn] = body.onReturn().withoutSuspension();
        m_kept.m_effects[onThrow] = body.onThrow().withoutSuspension();
        if (m_witnessesWanted == Witnesses::Found) {
            if (body.onReturn().mayCallUnknownCode())
                m_witnesses[onReturn] = body.unknownCodeWitness(id);
            if (body.onThrow().mayCallUnknownCode())
                m_witnesses[onThrow] = body.unknownCodeWitness(id);
        }
        for (uint32_t parameter = 0; parameter < body.numberOfParameters(); ++parameter)
            m_kept.m_effects[onReturn + 2 + parameter] = body.parameters()[parameter].memory;

        Vector<uint8_t, 16> takesInUnknownCode;
        takesInUnknownCode.fill(0, numberOfOwnNodes);
        body.forEachGroupOfDependencies([&](FunctionID source, const StoredBodySummary::DependencyGroup& group) {
            if (source)
                return;
            group.forEach([&](uint32_t, uint32_t ownNode) {
                takesInUnknownCode[ownNode] = 1;
            });
        });
        auto forEachListedSource = [&](const auto& functor) {
            body.forEachGroupOfDependencies([&](FunctionID source, const StoredBodySummary::DependencyGroup& group) {
                bool isEntered = source && isEnteredByUnknownCode[source.index()];
                group.forEach([&](uint32_t sourceNode, uint32_t ownNode) {
                    bool comesThroughUnknownCode = isEntered && takesInUnknownCode[ownNode];
                    if (!comesThroughUnknownCode)
                        functor(source, sourceNode, ownNode);
                });
            });
        };

        Vector<uint32_t, 16> nextSourceOf;
        nextSourceOf.fill(0, numberOfOwnNodes);
        forEachListedSource([&](FunctionID, uint32_t, uint32_t ownNode) {
            ++nextSourceOf[ownNode];
        });
        uint32_t first = firstSourceIn[index];
        for (unsigned ownNode = 0; ownNode < numberOfOwnNodes; ++ownNode) {
            uint32_t count = nextSourceOf[ownNode];
            m_firstSource[onReturn + ownNode] = first;
            nextSourceOf[ownNode] = first;
            first += count;
        }
        forEachListedSource([&](FunctionID source, uint32_t sourceNode, uint32_t ownNode) {
            if (m_witnessesWanted == Witnesses::Found && !source)
                m_witnesses[onReturn + ownNode] = body.unknownCodeWitness(id);
            m_sources[nextSourceOf[ownNode]++] = source ? indexOf(source, sourceNode) : unknownCodeNode();
        });
        for (unsigned ownNode = 0; ownNode < numberOfOwnNodes; ++ownNode)
            m_endOfSources[onReturn + ownNode] = nextSourceOf[ownNode];

        if (isEnteredByUnknownCode[index]) {
            uint32_t firstEntered = static_cast<uint32_t>(numberOfDependencies) + firstNodeEntered[index];
            for (unsigned ownNode = 0; ownNode < numberOfOwnNodes; ++ownNode)
                m_sources[firstEntered + ownNode] = onReturn + ownNode;
        }
    });

    m_kept.m_effects[unknownCodeNode()] = m_kept.m_masks->compress(effectsOfUnknownCodeAlone());
    m_firstSource[unknownCodeNode()] = static_cast<uint32_t>(numberOfDependencies);
    m_endOfSources[unknownCodeNode()] = static_cast<uint32_t>(numberOfDependencies + numberOfNodesEntered);
}

std::span<const uint32_t> EffectSummarizer::sourcesOf(unsigned node) const
{
    return m_sources.subspan(m_firstSource[node], m_endOfSources[node] - m_firstSource[node]);
}

void EffectSummarizer::takeInSources(uint32_t node)
{
    InternedLocationMasks& masks = m_kept.m_masks;
    CompactEffects own = m_kept.m_effects[node];
    Effects effects = masks.expand(own);
    bool hasTakenIn = false;
    uint8_t takesInUnknownCode = m_kept.m_takesInUnknownCode[node];
    for (uint32_t source : sourcesOf(node)) {
        if (source == unknownCodeNode()) {
            takesInUnknownCode = 1;
            continue;
        }
        if (!m_isResolved[source])
            continue;
        takesInUnknownCode |= m_kept.m_takesInUnknownCode[source];
        if (m_kept.m_effects[source] == own)
            continue;
        masks.mergeInto(effects, m_kept.m_effects[source]);
        hasTakenIn = true;
    }
    if (hasTakenIn)
        m_kept.m_effects[node] = masks.compress(effects);
    m_kept.m_takesInUnknownCode[node] = takesInUnknownCode;
}

Vector<uint32_t> EffectSummarizer::resolveNodesBelowCycles()
{
    AOT_THRESHOLD(size_t, fewestNodesWorthARound, 2048);
    constexpr uint32_t nodesInShare = 4096;
    const ParallelLoops& loops = m_build.parallelLoops();

    struct UnresolvedNode {
        uint32_t node;
        uint32_t firstSourceToLookAt;
    };
    struct Share {
        Vector<UnresolvedNode> unresolved;
        Vector<uint32_t> resolvedInThisRound;
    };
    uint32_t numberOfNodes = unknownCodeNode();
    Vector<Share> shares((numberOfNodes + nodesInShare - 1) / nodesInShare);
    auto resolveIfReady = [&](Share& share, UnresolvedNode& entry) {
        std::span<const uint32_t> sources = sourcesOf(entry.node);
        uint32_t index = entry.firstSourceToLookAt;
        while (index < sources.size() && (sources[index] == unknownCodeNode() || m_isResolved[sources[index]]))
            ++index;
        entry.firstSourceToLookAt = index;
        if (index < sources.size())
            return false;
        takeInSources(entry.node);
        share.resolvedInThisRound.append(entry.node);
        return true;
    };
    for (bool isFirstRound = true;; isFirstRound = false) {
        std::atomic<size_t> numberResolvedInThisRound { 0 };
        forEachIndexInParallel(loops, shares.size(), [&](unsigned index) {
            Share& share = shares[index];
            if (isFirstRound) {
                uint32_t first = index * nodesInShare;
                for (uint32_t node = first, end = std::min(numberOfNodes, first + nodesInShare); node < end; ++node) {
                    UnresolvedNode entry { node, 0 };
                    if (!resolveIfReady(share, entry))
                        share.unresolved.append(entry);
                }
            } else {
                share.unresolved.removeAllMatching([&](UnresolvedNode& entry) {
                    return resolveIfReady(share, entry);
                });
            }
            numberResolvedInThisRound.fetch_add(share.resolvedInThisRound.size(), std::memory_order_relaxed);
        });
        forEachIndexInParallel(loops, shares.size(), [&](unsigned index) {
            for (uint32_t node : shares[index].resolvedInThisRound)
                m_isResolved[node] = 1;
            shares[index].resolvedInThisRound.shrink(0);
        });
        if (numberResolvedInThisRound.load(std::memory_order_relaxed) < fewestNodesWorthARound)
            break;
    }

    size_t numberLeft = 0;
    for (const Share& share : shares)
        numberLeft += share.unresolved.size();
    Vector<uint32_t> unresolved = createVectorWithCapacityInParallel<uint32_t>(loops, numberLeft);
    for (const Share& share : shares) {
        for (const UnresolvedNode& entry : share.unresolved)
            unresolved.append(entry.node);
    }
    forEachIndexInParallel(loops, shares.size(), [&](unsigned index) {
        shares[index] = { };
    });
    return unresolved;
}

void EffectSummarizer::resolveNodesInAndAboveCycles(std::span<const uint32_t> nodes)
{
    const ParallelLoops& loops = m_build.parallelLoops();
    InternedLocationMasks& masks = m_kept.m_masks;
    unsigned count = static_cast<unsigned>(nodes.size());
    if (!count)
        return;

    Vector<uint32_t> positionOf(m_kept.m_numberOfNodes);
    Vector<uint32_t> firstSource(count + 1);
    forEachIndexInParallel(loops, count, [&](unsigned position) {
        uint32_t node = nodes[position];
        positionOf[node] = position;
        takeInSources(node);
        firstSource[position] = std::ranges::count_if(sourcesOf(node), [&](uint32_t source) {
            return source != unknownCodeNode() && !m_isResolved[source];
        });
    });
    uint32_t numberOfSources = 0;
    for (unsigned position = 0; position < count; ++position)
        numberOfSources += std::exchange(firstSource[position], numberOfSources);
    firstSource[count] = numberOfSources;
    Vector<uint32_t> sources(numberOfSources);
    forEachIndexInParallel(loops, count, [&](unsigned position) {
        uint32_t next = firstSource[position];
        for (uint32_t source : sourcesOf(nodes[position])) {
            if (source != unknownCodeNode() && !m_isResolved[source])
                sources[next++] = positionOf[source];
        }
    });
    auto sourcesAt = [&](unsigned position) {
        return sources.subspan(firstSource[position], firstSource[position + 1] - firstSource[position]);
    };

    StronglyConnectedComponents components = computeStronglyConnectedComponents(count, sourcesAt);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        Effects united = Effects::none();
        uint8_t takesInUnknownCode = 0;
        std::span<const uint32_t> members = components.membersOf(component);
        for (uint32_t member : members) {
            masks.mergeInto(united, m_kept.m_effects[nodes[member]]);
            takesInUnknownCode |= m_kept.m_takesInUnknownCode[nodes[member]];
            for (uint32_t source : sourcesAt(member)) {
                if (components.componentOf[source] == component)
                    continue;
                masks.mergeInto(united, m_kept.m_effects[nodes[source]]);
                takesInUnknownCode |= m_kept.m_takesInUnknownCode[nodes[source]];
            }
        }
        if (components.isInCycle.quickGet(members[0]))
            united.mayThrow = true;
        CompactEffects compressed = masks.compress(united);
        for (uint32_t member : members) {
            m_kept.m_effects[nodes[member]] = compressed;
            m_kept.m_takesInUnknownCode[nodes[member]] = takesInUnknownCode;
        }
    }
}

void EffectSummarizer::propagateEffects(PhaseTimes& times)
{
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfNodes = m_kept.m_numberOfNodes + 1;
    Vector<Effects> effectsOfNodesAlone;
    if (m_build.options().verifiesAfterEveryPass)
        effectsOfNodesAlone = Vector<Effects>(numberOfNodes, [&](size_t node) {
            return m_kept.m_masks->expand(m_kept.m_effects[node]);
        });

    m_kept.m_takesInUnknownCode.fill(0, numberOfNodes);
    m_isResolved.fill(0, numberOfNodes);
    Vector<uint32_t> nodesInAndAboveCycles = resolveNodesBelowCycles();
    times.didFinish("resolveNodesBelowCycles"_s);
    if (!m_isOnlyCheck) {
        m_build.statistics().count(StatisticID::EffectSummaryNodes, numberOfNodes);
        m_build.statistics().count(StatisticID::EffectSummaryNodesInAndAboveCycles, nodesInAndAboveCycles.size());
    }
    resolveNodesInAndAboveCycles(nodesInAndAboveCycles.span());
    times.didFinish("resolveNodesInAndAboveCycles"_s);

    unsigned numberOfChunks = (m_program.functions.size() + BodySummaries::functionsInChunk - 1) / BodySummaries::functionsInChunk;
    m_kept.m_effectsEnteredInChunk.fill(Effects::none(), numberOfChunks);
    m_kept.m_takesInUnknownCodeInChunk.fill(0, numberOfChunks);
    forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
        uniteEffectsEnteredInChunk(chunk);
    });
    updateEffectsOfUnknownCode();
    times.didFinish("addEffectsOfUnknownCode"_s);

    if (m_build.options().verifiesAfterEveryPass)
        checkEffectsAgainstPlainSolve(effectsOfNodesAlone.span());
}

void EffectSummarizer::checkEffectsAgainstPlainSolve(std::span<const Effects> effectsOfNodesAlone) const
{
    Vector<Effects> effects(effectsOfNodesAlone);
    StronglyConnectedComponents components = computeStronglyConnectedComponents(m_kept.m_numberOfNodes + 1, [&](unsigned node) {
        return sourcesOf(node);
    });
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        Effects united = Effects::none();
        std::span<const uint32_t> members = components.membersOf(component);
        for (uint32_t member : members) {
            united.merge(effects[member]);
            for (uint32_t source : sourcesOf(member)) {
                if (components.componentOf[source] != component)
                    united.merge(effects[source]);
            }
        }
        if (components.isInCycle.quickGet(members[0]))
            united.mayThrow = true;
        for (uint32_t member : members) {
            effects[member] = united;
            RELEASE_ASSERT((member == unknownCodeNode() ? m_kept.m_unknownCode : solvedEffectsOf(member)) == united);
        }
    }
}

void EffectSummarizer::propagateWitnesses()
{
    constexpr uint32_t notReached = UINT32_MAX;
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfNodes = m_kept.m_numberOfNodes + 1;

    Vector<uint32_t> rankOf(FillWith { }, numberOfNodes, notReached);
    Vector<uint32_t> reached = findIndicesInParallel(loops, numberOfNodes, [&](unsigned node) {
        return !!m_witnesses[node];
    });
    Vector<uint32_t> waiting = findIndicesInParallel(loops, numberOfNodes, [&](unsigned node) {
        return !m_witnesses[node];
    });

    while (!reached.isEmpty() && !waiting.isEmpty()) {
        forEachIndexInParallel(loops, reached.size(), [&](unsigned rank) {
            rankOf[reached[rank]] = rank;
        });
        Vector<uint64_t> found = collectInParallel<uint64_t>(loops, waiting.size(), [&](unsigned position, Vector<uint64_t>& foundInChunk) {
            uint32_t node = waiting[position];
            uint32_t first = notReached;
            for (uint32_t source : sourcesOf(node))
                first = std::min(first, rankOf[source]);
            if (first != notReached)
                foundInChunk.append(static_cast<uint64_t>(first) << 32 | node);
        });
        parallelSort(loops, found.mutableSpan(), std::less<uint64_t> { });

        Vector<uint32_t> next(found.size());
        forEachIndexInParallel(loops, found.size(), [&](unsigned rank) {
            uint32_t node = static_cast<uint32_t>(found[rank]);
            m_witnesses[node] = m_witnesses[reached[found[rank] >> 32]];
            next[rank] = node;
        });
        reached = WTF::move(next);
        waiting = collectInParallel<uint32_t>(loops, waiting.size(), [&](unsigned position, Vector<uint32_t>& stillWaiting) {
            if (!m_witnesses[waiting[position]])
                stillWaiting.append(waiting[position]);
        });
    }
}

void EffectSummarizer::listUsers()
{
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned numberOfFunctions = m_program.functions.size();
    m_kept.m_usersAddedSince.clear();
    Vector<uint32_t>& firstUser = m_kept.m_firstUser;
    firstUser.fill(0, numberOfFunctions + 1);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        m_kept.m_bodies[index].forEachFunctionInDependencies(FunctionID(index), [&](FunctionID named) {
            WTF::atomicExchangeAdd(&firstUser[named.index()], 1u, std::memory_order_relaxed);
        });
    });
    uint32_t numberOfUsers = 0;
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        numberOfUsers += std::exchange(firstUser[index], numberOfUsers);
    firstUser[numberOfFunctions] = numberOfUsers;
    m_kept.m_users.resize(numberOfUsers);
    Vector<uint32_t> nextUser(firstUser.span().first(numberOfFunctions));
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        m_kept.m_bodies[index].forEachFunctionInDependencies(FunctionID(index), [&](FunctionID named) {
            m_kept.m_users[WTF::atomicExchangeAdd(&nextUser[named.index()], 1u, std::memory_order_relaxed)] = FunctionID(index);
        });
    });
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        AOT::sort(m_kept.m_users.mutableSubspan(firstUser[index], firstUser[index + 1] - firstUser[index]));
    });
}

void EffectSummarizer::checkAgainstSummariesFromNothing(const EffectSummaries& summaries) const
{
    BodySummaries nothingKept;
    EffectSummaries fromNothing;
    EffectSummarizer summarizer(m_program, m_flow, m_build, nothingKept, Witnesses::Omitted);
    summarizer.setIsOnlyCheck();
    summarizer.update(fromNothing);
    RELEASE_ASSERT(fromNothing.size() == summaries.size() && fromNothing.unknownCode() == summaries.unknownCode());
    for (unsigned index = 0; index < summaries.size(); ++index)
        RELEASE_ASSERT(fromNothing.of(FunctionID(index)) == summaries.of(FunctionID(index)));
}

static bool covers(const OperandSummary& kept, const OperandSummary& fresh)
{
    if (!kept.uses.containsAll(fresh.uses))
        return false;
    if (!fresh.uses.contains(OperandUse::Retains) || kept.retainedIn == RetainedIn::UnknownMemory)
        return true;
    return kept.retainedIn == fresh.retainedIn && kept.retainingOperand == fresh.retainingOperand;
}

static bool covers(const FunctionSummary& kept, const FunctionSummary& fresh)
{
    if (!fresh.elsewhere.isSubsetOf(kept.elsewhere) || kept.parameters.size() != fresh.parameters.size())
        return false;
    for (unsigned parameter = 0; parameter < kept.parameters.size(); ++parameter) {
        const ParameterSummary& keptParameter = kept.parameters[parameter];
        const ParameterSummary& freshParameter = fresh.parameters[parameter];
        if (!covers(keptParameter.operand, freshParameter.operand))
            return false;
        if (!freshParameter.memory.reads.isSubsetOf(keptParameter.memory.reads) || !freshParameter.memory.writes.isSubsetOf(keptParameter.memory.writes))
            return false;
    }
    return true;
}

void EffectSummarizer::checkCoversSummariesFromNothing(const EffectSummaries& summaries) const
{
    BodySummaries nothingKept;
    EffectSummaries fromNothing;
    EffectSummarizer summarizer(m_program, m_flow, m_build, nothingKept, Witnesses::Omitted);
    summarizer.setIsOnlyCheck();
    summarizer.update(fromNothing);
    RELEASE_ASSERT(fromNothing.size() == summaries.size() && fromNothing.unknownCode().isSubsetOf(summaries.unknownCode()));
    for (unsigned index = 0; index < summaries.size(); ++index)
        RELEASE_ASSERT(covers(summaries.of(FunctionID(index)).expanded(), fromNothing.of(FunctionID(index)).expanded()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
