/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTModuleGraph.h"
#include "AOTNativeTest.h"
#include "AOTVector.h"
#include <algorithm>
#include <initializer_list>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

using Flag = ModuleData::Flag;
using Flags = OptionSet<Flag>;

namespace {

constexpr uint32_t defaultLimit = 8;

enum class Body : bool { Absent, Present };
enum class Edge : uint8_t { Static, Deferred, Require };

class Graph {
public:
    unsigned addSourceText(Body body = Body::Absent, Flags wireFlags = { }) { return add(ModuleKind::SourceText, ExternalKind::UncompiledCode, SyntheticExportRule::None, body, wireFlags); }
    unsigned addData(Body body = Body::Absent) { return add(ModuleKind::Data, ExternalKind::UncompiledCode, SyntheticExportRule::None, body, { }); }
    unsigned addLibrarySource() { return add(ModuleKind::RuntimeSource, ExternalKind::UncompiledCode, SyntheticExportRule::RuntimeModule, Body::Present, { }); }
    unsigned addCommonJS(Body body = Body::Present) { return add(ModuleKind::CommonJs, ExternalKind::UncompiledCode, SyntheticExportRule::CommonJs, body, Flag::CapturesOnDemand); }
    // What ES modules import in place of a CommonJS file whose names the build did not find.
    unsigned addFaceThatCaptures() { return add(ModuleKind::SourceText, ExternalKind::UncompiledCode, SyntheticExportRule::CommonJs, Body::Present, { }); }
    unsigned addRuntimeModule() { return add(ModuleKind::Runtime, ExternalKind::UncompiledCode, SyntheticExportRule::RuntimeModule, Body::Absent, { }); }
    unsigned addUncompiledCode() { return add(ModuleKind::External, ExternalKind::UncompiledCode, SyntheticExportRule::None, Body::Absent, { }); }

    void request(unsigned from, unsigned to, Edge edge = Edge::Static)
    {
        m_nodes[from].requests.append(ModuleRequestData::create(to, edge == Edge::Deferred, false, edge == Edge::Require));
    }

    void addRequiredName(unsigned module) { ++m_nodes[module].numberOfRequiredNames; }
    void nameInRequire(unsigned module) { m_modulesNamedByRequire.set(module); }

    void compute(uint32_t maxContractedRequests = defaultLimit)
    {
        ModuleData endRecord { };
        for (Node& node : m_nodes) {
            node.data.firstRequest = endRecord.firstRequest;
            node.data.firstRequiredName = endRecord.firstRequiredName;
            m_modules.append(node.data);
            m_requests.appendVector(node.requests);
            endRecord.firstRequest = Number24(endRecord.firstRequest + node.requests.size());
            endRecord.firstRequiredName = Number24(endRecord.firstRequiredName + node.numberOfRequiredNames);
        }
        m_modules.append(endRecord);
        computeModuleFlags(m_modules.mutableSpan(), m_requests.span());
        m_contracted = contractModuleRequests(m_modules.mutableSpan(), m_requests.span(), m_modulesNamedByRequire, maxContractedRequests);
        AOT_TEST_CHECK(m_modules.last().firstLinkRequest == m_contracted.linkRequests.size());
        AOT_TEST_CHECK(m_modules.last().firstEvaluationRequest == m_contracted.evaluationRequests.size());
    }

    Vector<LinkPlanStepData> planLink(unsigned entryPoint) { return planLinkOfEntryPoint(m_modules.mutableSpan(), m_requests.span(), ModuleID(entryPoint)); }

    unsigned size() const { return m_nodes.size(); }
    ModuleKind kindOf(unsigned module) const { return m_nodes[module].data.kind; }
    const ModuleData& data(unsigned module) const { return m_modules[module]; }
    const ContractedModuleRequests& contracted() const { return m_contracted; }
    bool resolvesNames(unsigned module) const { return !!m_nodes[module].numberOfRequiredNames; }

    std::span<const uint32_t> linkRequests(unsigned module) const { return listOf(m_contracted.linkRequests, module, &ModuleData::firstLinkRequest); }
    std::span<const uint32_t> evaluationRequests(unsigned module) const { return listOf(m_contracted.evaluationRequests, module, &ModuleData::firstEvaluationRequest); }

    Vector<uint32_t> allRequests(unsigned module) const
    {
        return WTF::map(m_nodes[module].requests, [](const ModuleRequestData& request) {
            return request.target();
        });
    }

private:
    struct Node {
        ModuleData data;
        Vector<ModuleRequestData, 2> requests;
        unsigned numberOfRequiredNames { 0 };
    };

    unsigned add(ModuleKind kind, ExternalKind externalKind, SyntheticExportRule exportRule, Body body, Flags wireFlags)
    {
        ModuleData data { };
        data.kind = kind;
        data.externalKind = externalKind;
        data.exportRule = exportRule;
        data.flags = wireFlags.toRaw();
        data.body = optionalNumber24(body == Body::Present ? static_cast<uint32_t>(m_nodes.size()) : UINT32_MAX);
        data.referrerOrRuntimeModule = optionalNumber24(UINT32_MAX);
        m_nodes.append(Node { data, { }, 0 });
        return m_nodes.size() - 1;
    }

    std::span<const uint32_t> listOf(const Vector<uint32_t>& lists, unsigned module, Number24 ModuleData::* first) const
    {
        uint32_t start = m_modules[module].*first;
        return lists.subspan(start, m_modules[module + 1].*first - start);
    }

    Vector<Node> m_nodes;
    Vector<ModuleData> m_modules;
    Vector<ModuleRequestData> m_requests;
    BitVector m_modulesNamedByRequire;
    ContractedModuleRequests m_contracted;
};

class RandomNumbers {
public:
    explicit RandomNumbers(uint32_t seed)
        : m_state(seed)
    {
    }

    uint32_t below(uint32_t limit)
    {
        m_state = m_state * 1664525u + 1013904223u;
        return (m_state >> 8) % limit;
    }

private:
    uint32_t m_state;
};

} // anonymous namespace

static bool isList(std::span<const uint32_t> list, std::initializer_list<uint32_t> expected)
{
    return std::ranges::equal(list, expected);
}

static Vector<uint32_t> copyOf(std::span<const uint32_t> list)
{
    return Vector<uint32_t>(list);
}

// What the link of the run time does in the order in which it does it: it captures the exports of a leaf where it arrives, and resolves the names of a module when it leaves.
template<typename TargetsFunctor>
static void walkLink(const Graph& graph, unsigned module, const TargetsFunctor& targetsOf, BitVector& seen, Vector<uint32_t>& work)
{
    const ModuleData& data = graph.data(module);
    if (!data.needsLink())
        return;
    if (data.capturesExports() || data.isUncompiledCode()) {
        work.append(module);
        return;
    }
    for (uint32_t target : targetsOf(module)) {
        if (!seen.quickSet(target))
            walkLink(graph, target, targetsOf, seen, work);
    }
    if (graph.resolvesNames(module))
        work.append(module);
}

template<typename TargetsFunctor>
static void walkEvaluation(const Graph& graph, unsigned module, const TargetsFunctor& targetsOf, BitVector& seen, Vector<uint32_t>& work)
{
    const ModuleData& data = graph.data(module);
    if (data.isUncompiledCode()) {
        work.append(module);
        return;
    }
    if (!data.hasExportTable())
        return;
    for (uint32_t target : targetsOf(module)) {
        if (!seen.quickSet(target))
            walkEvaluation(graph, target, targetsOf, seen, work);
    }
    if (data.bodyFunction() || data.hasTopLevelAwait())
        work.append(module);
}

template<typename TargetsFunctor>
static Vector<uint32_t> linkWorkFrom(const Graph& graph, unsigned root, const TargetsFunctor& targetsOf)
{
    Vector<uint32_t> work;
    BitVector seen(graph.size());
    seen.quickSet(root);
    walkLink(graph, root, targetsOf, seen, work);
    return work;
}

template<typename TargetsFunctor>
static Vector<uint32_t> evaluationWorkFrom(const Graph& graph, unsigned root, const TargetsFunctor& targetsOf)
{
    Vector<uint32_t> work;
    BitVector seen(graph.size());
    seen.quickSet(root);
    walkEvaluation(graph, root, targetsOf, seen, work);
    return work;
}

static void testModulesWithNothingToRunAreLeftOut()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned importer = graph.addSourceText();
    unsigned leaf = graph.addSourceText();
    unsigned data = graph.addData();
    graph.request(root, importer);
    graph.request(importer, leaf);
    graph.request(importer, data);
    graph.compute();
    AOT_TEST_CHECK(graph.data(root).needsEvaluation());
    AOT_TEST_CHECK(!graph.data(importer).needsEvaluation() && !graph.data(leaf).needsEvaluation() && !graph.data(data).needsEvaluation());
    AOT_TEST_CHECK(graph.contracted().evaluationRequests.isEmpty() && graph.contracted().linkRequests.isEmpty());
    AOT_TEST_CHECK(graph.contracted().modulesOverLimit.isEmpty());
}

static void testWorkThatNeedsEvaluation()
{
    Graph graph;
    unsigned importerOfBody = graph.addSourceText();
    unsigned withBody = graph.addData(Body::Present);
    unsigned importerOfAwait = graph.addSourceText();
    unsigned awaiting = graph.addSourceText(Body::Absent, Flag::HasTopLevelAwait);
    unsigned importerOfUncompiled = graph.addSourceText();
    unsigned uncompiled = graph.addUncompiledCode();
    unsigned importerOfDeferring = graph.addSourceText();
    unsigned deferring = graph.addSourceText();
    unsigned deferred = graph.addSourceText();
    unsigned importerOfNative = graph.addSourceText();
    unsigned native = graph.addRuntimeModule();
    unsigned importerOfLibrarySource = graph.addSourceText();
    unsigned librarySource = graph.addLibrarySource();
    graph.request(importerOfBody, withBody);
    graph.request(importerOfAwait, awaiting);
    graph.request(importerOfUncompiled, uncompiled);
    graph.request(importerOfDeferring, deferring);
    graph.request(deferring, deferred, Edge::Deferred);
    graph.request(importerOfNative, native);
    graph.request(importerOfLibrarySource, librarySource);
    graph.compute();
    for (unsigned module : { importerOfBody, withBody, importerOfAwait, awaiting, importerOfUncompiled, uncompiled, importerOfDeferring, deferring })
        AOT_TEST_CHECK(graph.data(module).needsEvaluation());
    for (unsigned module : { deferred, importerOfNative, native, importerOfLibrarySource, librarySource })
        AOT_TEST_CHECK(!graph.data(module).needsEvaluation());
}

static void testListReplacesModuleInPlace()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned first = graph.addSourceText(Body::Present);
    unsigned barrel = graph.addSourceText();
    unsigned second = graph.addSourceText(Body::Present);
    unsigned third = graph.addSourceText(Body::Present);
    unsigned fourth = graph.addSourceText(Body::Present);
    graph.request(root, first);
    graph.request(root, barrel);
    graph.request(root, fourth);
    graph.request(barrel, second);
    graph.request(barrel, third);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { first, second, third, fourth }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(barrel), { second, third }));
    AOT_TEST_CHECK(graph.evaluationRequests(first).empty());
}

static void testLaterDuplicatesAreDropped()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned firstBarrel = graph.addSourceText();
    unsigned secondBarrel = graph.addSourceText();
    unsigned outerBarrel = graph.addSourceText();
    unsigned first = graph.addSourceText(Body::Present);
    unsigned second = graph.addSourceText(Body::Present);
    graph.request(root, firstBarrel);
    graph.request(root, outerBarrel);
    graph.request(root, first);
    graph.request(firstBarrel, second);
    graph.request(secondBarrel, second);
    graph.request(secondBarrel, first);
    graph.request(outerBarrel, secondBarrel);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { second, first }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(outerBarrel), { second, first }));
}

static void testModuleThatRequestsItself()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned barrel = graph.addSourceText();
    unsigned leaf = graph.addSourceText(Body::Present);
    graph.request(root, root);
    graph.request(root, barrel);
    graph.request(barrel, barrel);
    graph.request(barrel, leaf);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { leaf }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(barrel), { leaf }));
}

static void testModulesOfACycleStay()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned first = graph.addSourceText();
    unsigned second = graph.addSourceText();
    unsigned leaf = graph.addSourceText(Body::Present);
    graph.request(root, first);
    graph.request(first, second);
    graph.request(second, first);
    graph.request(second, leaf);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { first }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(first), { second }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(second), { first, leaf }));

    Graph withNothingToRun;
    unsigned importer = withNothingToRun.addSourceText(Body::Present);
    unsigned left = withNothingToRun.addSourceText();
    unsigned right = withNothingToRun.addSourceText();
    withNothingToRun.request(importer, left);
    withNothingToRun.request(left, right);
    withNothingToRun.request(right, left);
    withNothingToRun.compute();
    AOT_TEST_CHECK(withNothingToRun.contracted().evaluationRequests.isEmpty());
}

static void testModuleThatReachesAnAwaitStays()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned importer = graph.addSourceText();
    unsigned awaiting = graph.addSourceText(Body::Present, Flag::HasTopLevelAwait);
    graph.request(root, importer);
    graph.request(importer, awaiting);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { importer }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(importer), { awaiting }));
}

static void testModuleWithADeferredRequestHasNoList()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned deferring = graph.addSourceText();
    unsigned eager = graph.addSourceText(Body::Present);
    unsigned deferred = graph.addSourceText(Body::Present);
    graph.request(root, deferring);
    graph.request(deferring, eager);
    graph.request(deferring, deferred, Edge::Deferred);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { deferring }));
    AOT_TEST_CHECK(graph.evaluationRequests(deferring).empty());
}

static void testModuleThatARequireNamesStays()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned outerBarrel = graph.addSourceText();
    unsigned barrel = graph.addSourceText();
    unsigned leaf = graph.addSourceText(Body::Present);
    unsigned librarySource = graph.addLibrarySource();
    graph.request(root, outerBarrel);
    graph.request(outerBarrel, barrel);
    graph.request(barrel, leaf);
    graph.request(barrel, librarySource);
    graph.nameInRequire(barrel);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { barrel }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(outerBarrel), { barrel }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(barrel), { leaf }));
    AOT_TEST_CHECK(isList(graph.linkRequests(root), { librarySource }));
}

static void testModulesThatRanAtLinkAreNotEvaluated()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned librarySource = graph.addLibrarySource();
    unsigned native = graph.addRuntimeModule();
    unsigned uncompiled = graph.addUncompiledCode();
    unsigned leaf = graph.addSourceText(Body::Present);
    graph.request(root, librarySource);
    graph.request(root, native);
    graph.request(root, uncompiled);
    graph.request(root, leaf);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { uncompiled, leaf }));
    AOT_TEST_CHECK(isList(graph.linkRequests(root), { librarySource, native, uncompiled }));
    AOT_TEST_CHECK(graph.linkRequests(librarySource).empty() && graph.evaluationRequests(librarySource).empty());
}

// A CommonJS file runs where the evaluation comes to it, and what it requires runs inside its body.
static void testCommonJSIsEvaluatedInTheOrderOfTheRequests()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned first = graph.addCommonJS();
    unsigned calledByFirst = graph.addCommonJS(Body::Absent);
    unsigned startedByFirst = graph.addCommonJS();
    unsigned leaf = graph.addSourceText(Body::Present);
    graph.request(root, first);
    graph.request(root, leaf);
    graph.request(first, calledByFirst, Edge::Require);
    graph.request(first, startedByFirst, Edge::Require);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { first, leaf }));
    AOT_TEST_CHECK(graph.evaluationRequests(first).empty() && graph.contracted().linkRequests.isEmpty());
    AOT_TEST_CHECK(!graph.data(calledByFirst).needsEvaluation() && graph.data(startedByFirst).needsEvaluation());
}

static void testLinkFollowsRequire()
{
    Graph graph;
    unsigned root = graph.addCommonJS();
    unsigned required = graph.addSourceText(Body::Present);
    unsigned librarySource = graph.addLibrarySource();
    unsigned back = graph.addCommonJS();
    graph.request(root, required, Edge::Require);
    graph.request(required, librarySource);
    graph.request(required, back);
    graph.request(back, root, Edge::Require);
    graph.compute();
    for (unsigned module : { root, required, back })
        AOT_TEST_CHECK(graph.data(module).needsLink());
    // The three are one cycle for a link, so none of them is replaced by its list.
    AOT_TEST_CHECK(isList(graph.linkRequests(root), { required }) && isList(graph.linkRequests(required), { librarySource, back }) && isList(graph.linkRequests(back), { root }));
    AOT_TEST_CHECK(graph.data(required).isSimple() && isList(graph.evaluationRequests(required), { back }));

    Vector<LinkPlanStepData> plan = graph.planLink(root);
    AOT_TEST_CHECK(plan.size() == 1 && plan[0].module == librarySource && !plan[0].resolvesNames);
}

static void testFaceThatCapturesIsAStepAndIsPassed()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned face = graph.addFaceThatCaptures();
    unsigned file = graph.addCommonJS(Body::Absent);
    unsigned required = graph.addSourceText(Body::Present);
    unsigned librarySource = graph.addLibrarySource();
    graph.request(root, face);
    graph.request(face, file, Edge::Require);
    graph.request(file, required, Edge::Require);
    graph.request(required, librarySource);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.linkRequests(root), { face }) && isList(graph.linkRequests(face), { librarySource }));
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { face }) && graph.evaluationRequests(face).empty());

    Vector<LinkPlanStepData> plan = graph.planLink(root);
    AOT_TEST_CHECK(plan.size() == 2 && plan[0].module == face && plan[1].module == librarySource && !plan[0].resolvesNames && !plan[1].resolvesNames);
    AOT_TEST_CHECK(graph.data(face).isLinkedWithEntryPoint());
}

static void testLinkPassesThroughModulesWithoutLinkWork()
{
    Graph graph;
    unsigned root = graph.addSourceText(Body::Present);
    unsigned withBody = graph.addSourceText(Body::Present);
    unsigned withNames = graph.addSourceText();
    unsigned withoutLink = graph.addSourceText(Body::Present);
    unsigned first = graph.addLibrarySource();
    unsigned second = graph.addRuntimeModule();
    graph.request(root, withoutLink);
    graph.request(root, withBody);
    graph.request(root, withNames);
    graph.request(withBody, first);
    graph.request(withNames, first);
    graph.request(withNames, second);
    graph.addRequiredName(withNames);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.linkRequests(root), { first, withNames }));
    AOT_TEST_CHECK(isList(graph.linkRequests(withBody), { first }));
    AOT_TEST_CHECK(isList(graph.linkRequests(withNames), { first, second }));
    AOT_TEST_CHECK(graph.linkRequests(withoutLink).empty());
    AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { withoutLink, withBody }));
}

static void testLinkVisitsEveryModuleThatReachesUncompiledCode()
{
    Graph graph;
    unsigned root = graph.addSourceText();
    unsigned importer = graph.addSourceText();
    unsigned sibling = graph.addSourceText();
    unsigned uncompiled = graph.addUncompiledCode();
    unsigned librarySource = graph.addLibrarySource();
    graph.request(root, importer);
    graph.request(root, sibling);
    graph.request(importer, uncompiled);
    graph.request(sibling, librarySource);
    graph.compute();
    AOT_TEST_CHECK(isList(graph.linkRequests(root), { importer, librarySource }));
    AOT_TEST_CHECK(isList(graph.linkRequests(importer), { uncompiled }));
}

static void testListOverTheLimitStaysWithItsModule()
{
    for (uint32_t limit : { 0u, 1u, 2u, 3u }) {
        Graph graph;
        unsigned root = graph.addSourceText(Body::Present);
        unsigned barrel = graph.addSourceText();
        unsigned emptyBarrel = graph.addSourceText();
        unsigned first = graph.addSourceText(Body::Present);
        unsigned second = graph.addSourceText(Body::Present);
        unsigned third = graph.addSourceText(Body::Present);
        unsigned librarySource = graph.addLibrarySource();
        graph.request(root, emptyBarrel);
        graph.request(root, barrel);
        graph.request(emptyBarrel, librarySource);
        for (unsigned target : { first, second, third })
            graph.request(barrel, target);
        graph.addRequiredName(root);
        graph.compute(limit);
        AOT_TEST_CHECK(isList(graph.evaluationRequests(barrel), { first, second, third }));
        AOT_TEST_CHECK(graph.evaluationRequests(emptyBarrel).empty());
        if (limit >= 3)
            AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { first, second, third }));
        else
            AOT_TEST_CHECK(isList(graph.evaluationRequests(root), { barrel }));

        if (limit >= 1)
            AOT_TEST_CHECK(isList(graph.linkRequests(root), { librarySource }));
        else
            AOT_TEST_CHECK(isList(graph.linkRequests(root), { emptyBarrel }));

        Vector<uint32_t> expectedOverLimit;
        if (!limit)
            expectedOverLimit.append(emptyBarrel);
        if (limit < 3)
            expectedOverLimit.append(barrel);
        AOT_TEST_CHECK(graph.contracted().modulesOverLimit == expectedOverLimit);
    }
}

static void testManyImportersOfOneLongList()
{
    constexpr unsigned numberOfImporters = 1000;
    constexpr unsigned numberOfTargets = 100;
    Graph graph;
    unsigned barrel = graph.addSourceText();
    for (unsigned index = 0; index < numberOfTargets; ++index)
        graph.request(barrel, graph.addSourceText(Body::Present));
    for (unsigned index = 0; index < numberOfImporters; ++index)
        graph.request(graph.addSourceText(Body::Present), barrel);
    graph.compute();
    AOT_TEST_CHECK(graph.contracted().evaluationRequests.size() == numberOfImporters + numberOfTargets);
}

static void testLongChain()
{
    constexpr unsigned numberOfModules = 100000;
    Graph graph;
    for (unsigned module = 0; module + 2 < numberOfModules; ++module)
        graph.addSourceText();
    unsigned last = graph.addSourceText(Body::Present);
    unsigned librarySource = graph.addLibrarySource();
    for (unsigned module = 0; module < last; ++module)
        graph.request(module, module + 1);
    graph.request(last, librarySource);
    graph.compute();
    for (unsigned module = 0; module < last; ++module) {
        AOT_TEST_CHECK(isList(graph.evaluationRequests(module), { last }));
        AOT_TEST_CHECK(isList(graph.linkRequests(module), { librarySource }));
    }
    AOT_TEST_CHECK(graph.evaluationRequests(last).empty());

    Vector<LinkPlanStepData> plan = graph.planLink(0);
    AOT_TEST_CHECK(plan.size() == 1 && plan[0].module == librarySource && !plan[0].resolvesNames);
    for (unsigned module = 0; module <= last; ++module)
        AOT_TEST_CHECK(graph.data(module).isLinkedWithEntryPoint());
}

static void testLinkPlan()
{
    Graph graph;
    unsigned entryPoint = graph.addSourceText(Body::Present, Flag::IsEntryPoint);
    unsigned otherEntryPoint = graph.addSourceText(Body::Present, Flag::IsEntryPoint);
    unsigned withNames = graph.addSourceText();
    unsigned between = graph.addSourceText();
    unsigned withoutLink = graph.addSourceText(Body::Present);
    unsigned notReached = graph.addSourceText();
    unsigned first = graph.addLibrarySource();
    unsigned second = graph.addRuntimeModule();
    unsigned third = graph.addLibrarySource();
    graph.request(entryPoint, withoutLink);
    graph.request(entryPoint, withNames);
    graph.request(entryPoint, otherEntryPoint);
    graph.request(entryPoint, first);
    graph.request(withNames, between);
    graph.request(withNames, second);
    graph.request(between, first);
    graph.request(between, entryPoint);
    graph.request(otherEntryPoint, third);
    graph.request(notReached, third);
    graph.addRequiredName(withNames);
    graph.addRequiredName(entryPoint);
    graph.compute();

    Vector<LinkPlanStepData> plan = graph.planLink(entryPoint);
    AOT_TEST_CHECK(plan.size() == 5);
    AOT_TEST_CHECK(plan[0].module == first && !plan[0].resolvesNames);
    AOT_TEST_CHECK(plan[1].module == second && !plan[1].resolvesNames);
    AOT_TEST_CHECK(plan[2].module == withNames && plan[2].resolvesNames);
    AOT_TEST_CHECK(plan[3].module == third && !plan[3].resolvesNames);
    AOT_TEST_CHECK(plan[4].module == entryPoint && plan[4].resolvesNames);

    for (unsigned module : { entryPoint, withNames, between })
        AOT_TEST_CHECK(graph.data(module).isLinkedWithEntryPoint());
    for (unsigned module : { otherEntryPoint, withoutLink, notReached, first, second, third })
        AOT_TEST_CHECK(!graph.data(module).isLinkedWithEntryPoint());
    AOT_TEST_CHECK(graph.data(entryPoint).hasLinkPlan() && !graph.data(otherEntryPoint).hasLinkPlan() && !graph.data(withNames).hasLinkPlan());
}

static void testEntryPointsWithoutLinkPlan()
{
    Graph graph;
    unsigned withoutLink = graph.addSourceText(Body::Present, Flag::IsEntryPoint);
    unsigned leaf = graph.addSourceText(Body::Present);
    unsigned librarySource = graph.addLibrarySource();
    unsigned reachesUncompiledCode = graph.addSourceText(Body::Present, Flag::IsEntryPoint);
    unsigned importer = graph.addSourceText();
    unsigned uncompiled = graph.addUncompiledCode();
    graph.request(withoutLink, leaf);
    graph.request(reachesUncompiledCode, librarySource);
    graph.request(reachesUncompiledCode, importer);
    graph.request(importer, uncompiled);
    graph.compute();
    for (unsigned entryPoint : { withoutLink, librarySource, reachesUncompiledCode })
        AOT_TEST_CHECK(graph.planLink(entryPoint).isEmpty());
    for (unsigned module = 0; module < graph.size(); ++module)
        AOT_TEST_CHECK(!graph.data(module).isLinkedWithEntryPoint());
}

static void addRandomModule(Graph& graph, RandomNumbers& random)
{
    switch (random.below(16)) {
    case 0:
    case 1:
        graph.addLibrarySource();
        return;
    case 2:
        graph.addRuntimeModule();
        return;
    case 3:
        graph.addData(random.below(2) ? Body::Present : Body::Absent);
        return;
    case 4:
        if (!random.below(4)) {
            graph.addUncompiledCode();
            return;
        }
        break;
    default:
        break;
    }
    Flags wireFlags = random.below(24) ? Flags { } : Flags { Flag::HasTopLevelAwait };
    unsigned module = graph.addSourceText(random.below(3) ? Body::Absent : Body::Present, wireFlags);
    if (!random.below(5))
        graph.addRequiredName(module);
    if (!random.below(8))
        graph.nameInRequire(module);
}

static void addRandomRequests(Graph& graph, RandomNumbers& random, unsigned module)
{
    if (graph.kindOf(module) != ModuleKind::SourceText)
        return;
    BitVector requested(graph.size());
    for (uint32_t count = random.below(5); count; --count) {
        // Mostly forward, so that many modules are alone in their component.
        uint32_t target = random.below(4) ? module + random.below(graph.size() - module) : random.below(graph.size());
        if (!requested.quickSet(target))
            graph.request(module, target);
    }
}

static void testWalksOfRandomGraphsDoTheSameWork()
{
    RandomNumbers random(20261019);
    for (unsigned round = 0; round < 4000; ++round) {
        Graph graph;
        unsigned numberOfModules = 1 + random.below(24);
        for (unsigned module = 0; module < numberOfModules; ++module)
            addRandomModule(graph, random);
        for (unsigned module = 0; module < numberOfModules; ++module)
            addRandomRequests(graph, random, module);
        graph.compute(random.below(2) ? random.below(4) : defaultLimit);
        auto allRequests = [&](unsigned module) {
            return graph.allRequests(module);
        };
        for (unsigned root = 0; root < numberOfModules; ++root) {
            Vector<uint32_t> linkWork = linkWorkFrom(graph, root, allRequests);
            AOT_TEST_CHECK(linkWork == linkWorkFrom(graph, root, [&](unsigned module) {
                return copyOf(graph.linkRequests(module));
            }));
            AOT_TEST_CHECK(evaluationWorkFrom(graph, root, allRequests) == evaluationWorkFrom(graph, root, [&](unsigned module) {
                return copyOf(graph.evaluationRequests(module));
            }));

            Graph withPlan = graph;
            Vector<LinkPlanStepData> plan = withPlan.planLink(root);
            const ModuleData& rootData = graph.data(root);
            if (!rootData.needsLink() || rootData.capturesExports() || rootData.reachesUncompiledCode()) {
                AOT_TEST_CHECK(plan.isEmpty());
                continue;
            }
            AOT_TEST_CHECK(plan.size() == linkWork.size());
            for (unsigned step = 0; step < plan.size(); ++step)
                AOT_TEST_CHECK(plan[step].module == linkWork[step] && plan[step].resolvesNames == !graph.data(linkWork[step]).capturesExports());
        }
    }
}

std::span<const NativeTest> moduleRequestListTests()
{
    static constexpr NativeTest tests[] = {
        { "module-request-lists/modules-with-nothing-to-run-are-left-out"_s, testModulesWithNothingToRunAreLeftOut },
        { "module-request-lists/work-that-needs-evaluation"_s, testWorkThatNeedsEvaluation },
        { "module-request-lists/list-replaces-module-in-place"_s, testListReplacesModuleInPlace },
        { "module-request-lists/later-duplicates-are-dropped"_s, testLaterDuplicatesAreDropped },
        { "module-request-lists/module-that-requests-itself"_s, testModuleThatRequestsItself },
        { "module-request-lists/modules-of-a-cycle-stay"_s, testModulesOfACycleStay },
        { "module-request-lists/module-that-reaches-an-await-stays"_s, testModuleThatReachesAnAwaitStays },
        { "module-request-lists/module-with-a-deferred-request-has-no-list"_s, testModuleWithADeferredRequestHasNoList },
        { "module-request-lists/module-that-a-require-names-stays"_s, testModuleThatARequireNamesStays },
        { "module-request-lists/modules-that-ran-at-link-are-not-evaluated"_s, testModulesThatRanAtLinkAreNotEvaluated },
        { "module-request-lists/commonjs-is-evaluated-in-the-order-of-the-requests"_s, testCommonJSIsEvaluatedInTheOrderOfTheRequests },
        { "module-request-lists/link-follows-require"_s, testLinkFollowsRequire },
        { "module-request-lists/face-that-captures-is-a-step-and-is-passed"_s, testFaceThatCapturesIsAStepAndIsPassed },
        { "module-request-lists/link-passes-through-modules-without-link-work"_s, testLinkPassesThroughModulesWithoutLinkWork },
        { "module-request-lists/link-visits-every-module-that-reaches-uncompiled-code"_s, testLinkVisitsEveryModuleThatReachesUncompiledCode },
        { "module-request-lists/list-over-the-limit-stays-with-its-module"_s, testListOverTheLimitStaysWithItsModule },
        { "module-request-lists/many-importers-of-one-long-list"_s, testManyImportersOfOneLongList },
        { "module-request-lists/long-chain"_s, testLongChain },
        { "module-request-lists/link-plan"_s, testLinkPlan },
        { "module-request-lists/entry-points-without-link-plan"_s, testEntryPointsWithoutLinkPlan },
        { "module-request-lists/walks-of-random-graphs-do-the-same-work"_s, testWalksOfRandomGraphsDoTheSameWork },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
