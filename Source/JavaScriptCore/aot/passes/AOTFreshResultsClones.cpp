/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFreshResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTProgramCaches.h"
#include "AOTRemoveUnusedResults.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ResultSet {
    BitVector retained;
    bool isReadByCode { false };
};

using ResultSets = Vector<ResultSet>;

struct ResultVersion {
    Function function;
    Vector<ResultCallSite> sites;
};

struct CommittedVersion {
    const BitVector* retained { nullptr };
    FunctionID function;
    TypeID singleResultType;
};

struct CommittedVersions {
    Vector<Vector<CommittedVersion>> byOriginal;
    Vector<ResultCallSiteList> callers;
    unsigned numberOfClones { 0 };
};

} // anonymous namespace

AOT_THRESHOLD(size_t, maxGrowthDenominator, 8);

static Vector<ResultSets> collectResultSets(size_t functionCount, const Vector<ResultCallSiteList>& callers, BuildContext& build, const Invocable<bool(FunctionID)> auto& getsCode)
{
    uint64_t maxSets = build.limit(LimitID::MaxResultClones);
    Vector<ResultSets> resultSets(functionCount);
    BitVector exceedsMaxSets(functionCount);
    for (auto& list : callers) {
        bool isReadByCode = getsCode(list.caller);
        for (auto& site : list.sites) {
            BitVector retained = site.readResults;
            if (retained.isEmpty())
                retained.set(0);
            size_t callee = site.callee.index();
            auto& sets = resultSets[callee];
            if (exceedsMaxSets.quickGet(callee))
                continue;
            size_t known = sets.findIf([&](const ResultSet& set) {
                return set.retained == retained;
            });
            if (known != notFound) {
                sets[known].isReadByCode |= isReadByCode;
                continue;
            }
            if (sets.size() < maxSets) {
                sets.append(ResultSet { WTF::move(retained), isReadByCode });
                continue;
            }
            exceedsMaxSets.quickSet(callee);
            sets.clear();
            build.gaveUp(LimitID::MaxResultClones, Subject { .function = site.callee });
        }
    }
    return resultSets;
}

static Vector<FunctionID> findCloneCandidates(const Program& program, const CallGraph& callGraph, const BuildContext& build, const Vector<ResultSets>& resultSets)
{
    Vector<FunctionID> candidates;
    for (size_t index = 0; index < resultSets.size(); ++index) {
        FunctionID id(index);
        if (resultSets[index].size() < 2)
            continue;
        if (canEditResults(program, callGraph, id) != ResultEditing::Allowed || callGraph.recursion(id) != Recursion::NotRecursive)
            continue;
        if (build.isEnabled(PassID::SplitFreshResults, id))
            candidates.append(id);
    }
    return candidates;
}

static size_t numberOfListedValues(const Function& function)
{
    size_t size = 0;
    for (auto& block : function.blocks)
        size += block.values.size();
    return size;
}

static Vector<ResultVersion> tryCreateVersions(const PassRunner& runner, const Program& program, const OptimizationInputs& inputs, FunctionID original, const ResultSets& resultSets, TaskArenas& arenas)
{
    auto createVersion = [&](const BitVector& retained) {
        Function version = program.at(original);
        retainResults(version, retained);
        simplifyEditedFunction(runner, program, inputs, original, version, arenas);
        return version;
    };
    BitVector allRead;
    for (auto& set : resultSets)
        allRead.merge(set.retained);
    size_t sharedSize = numberOfListedValues(createVersion(allRead));
    size_t maxTotalSize = sharedSize + sharedSize / maxGrowthDenominator;

    size_t totalSize = 0;
    Vector<ResultVersion> versions;
    for (auto& set : resultSets) {
        Function version = createVersion(set.retained);
        if (set.isReadByCode)
            totalSize += numberOfListedValues(version);
        if (totalSize > maxTotalSize)
            return { };
        versions.append({ WTF::move(version), { } });
    }
    for (auto& version : versions)
        version.sites = collectResultCallSites(program, version.function);
    return versions;
}

static Vector<Vector<ResultVersion>> createVersions(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner, const Vector<FunctionID>& candidates, const Vector<ResultSets>& resultSets)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = runner.build().parallelLoops();
    Vector<Vector<ResultVersion>> versions(candidates.size());
    InterningPhases phases(editor, candidates.size());
    {
        ProgramCaches caches(program, runner.build());
        forEachIndexInParallel(loops, candidates.size(), [&](unsigned task) {
            FunctionID original = candidates[task];
            TaskArenas arenas(phases, task);
            versions[task] = tryCreateVersions(runner, program, inputs, original, resultSets[original.index()], arenas);
        });
    }
    BodyRemaps remaps = phases.finish();
    if (remaps.isIdentity())
        return versions;
    forEachIndexInParallel(loops, candidates.size(), [&](unsigned task) {
        for (auto& version : versions[task])
            applyRemaps(remaps, version.function);
    });
    return versions;
}

static void initializeCloneDerivation(Function& clone)
{
    if (clone.derivation.source)
        return;
    clone.derivation.parameterMap = Vector<ParameterOrigin>(clone.numberOfEntryParameters(), [](size_t index) {
        return ParameterOrigin { static_cast<uint32_t>(index) };
    });
}

static CommittedVersions commitVersions(ProgramEditor& editor, const Vector<FunctionID>& candidates, const Vector<ResultSets>& resultSets, Vector<Vector<ResultVersion>>&& versions)
{
    CommittedVersions committed;
    committed.byOriginal.grow(resultSets.size());
    for (size_t task = 0; task < candidates.size(); ++task) {
        FunctionID original = candidates[task];
        for (size_t index = 0; index < versions[task].size(); ++index) {
            auto& version = versions[task][index];
            auto& results = version.function.results;
            TypeID singleResultType = results.size() == 1 ? editor.program().provenTypeOf(results[0]) : TypeID();
            FunctionID id = original;
            if (!index)
                editor.replaceFunction(original, WTF::move(version.function));
            else {
                initializeCloneDerivation(version.function);
                id = editor.addFunctionLike(original, WTF::move(version.function));
                ++committed.numberOfClones;
            }
            committed.byOriginal[original.index()].append({ &resultSets[original.index()][index].retained, id, singleResultType });
            if (!version.sites.isEmpty())
                committed.callers.append({ id, WTF::move(version.sites) });
        }
    }
    return committed;
}

static const CommittedVersion& findVersion(const Vector<CommittedVersion>& versions, const BitVector& readResults)
{
    const CommittedVersion* smallest = nullptr;
    for (auto& version : versions) {
        BitVector missing = readResults;
        missing.exclude(*version.retained);
        if (missing.isEmpty() && (!smallest || version.retained->bitCount() < smallest->retained->bitCount()))
            smallest = &version;
    }
    RELEASE_ASSERT(smallest);
    return *smallest;
}

static void retargetCallSites(ProgramEditor& editor, const ResultCallSiteList& list, const CommittedVersions& committed)
{
    Vector<RetargetedCallSite, 8> retargeted;
    for (auto& site : list.sites) {
        auto& versions = committed.byOriginal[site.callee.index()];
        if (versions.isEmpty())
            continue;
        auto& version = findVersion(versions, site.readResults);
        retargeted.append({ site.call, version.function, version.retained, version.singleResultType });
    }
    if (!retargeted.isEmpty())
        retargetResultCallSites(editor.at(list.caller), retargeted.span());
}

void FreshResultSplitter::cloneWorkersByReadResults()
{
    Vector<ResultCallSiteList> callers = collectAllResultCallSites(program(), build());
    Vector<ResultSets> resultSets = collectResultSets(program().functions.size(), callers, build(), [&](FunctionID caller) {
        return !isWrapperThatNoCodeNames(caller);
    });
    Vector<FunctionID> candidates = findCloneCandidates(program(), m_inputs.callGraph, build(), resultSets);
    if (candidates.isEmpty()) {
        build().destroyLater(WTF::move(resultSets));
        build().destroyLater(WTF::move(callers));
        return;
    }

    CommittedVersions committed = commitVersions(m_editor, candidates, resultSets, createVersions(m_editor, m_inputs, m_runner, candidates, resultSets));
    if (committed.numberOfClones) {
        callers.removeAllMatching([&](auto& list) {
            return !committed.byOriginal[list.caller.index()].isEmpty();
        });
        callers.appendVector(WTF::move(committed.callers));
        forEachIndexInParallel(build().parallelLoops(), callers.size(), [&](unsigned index) {
            retargetCallSites(m_editor, callers[index], committed);
        });
        build().statistics().count(StatisticID::ResultClonesCreated, committed.numberOfClones);
    }
    build().destroyLater(WTF::move(committed));
    build().destroyLater(WTF::move(resultSets));
    build().destroyLater(WTF::move(callers));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
