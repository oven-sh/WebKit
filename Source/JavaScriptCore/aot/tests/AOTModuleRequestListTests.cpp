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
#include <initializer_list>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

using Flag = ModuleData::Flag;
using Flags = OptionSet<Flag>;

namespace {

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

    void compute()
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
    }

    Vector<LinkPlanStepData> planLink(unsigned entryPoint) { return planLinkOfEntryPoint(m_modules.mutableSpan(), m_requests.span(), ModuleID(entryPoint)); }

    unsigned size() const { return m_nodes.size(); }
    ModuleKind kindOf(unsigned module) const { return m_nodes[module].data.kind; }
    const ModuleData& data(unsigned module) const { return m_modules[module]; }
    bool resolvesNames(unsigned module) const { return !!m_nodes[module].numberOfRequiredNames; }

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

    Vector<Node> m_nodes;
    Vector<ModuleData> m_modules;
    Vector<ModuleRequestData> m_requests;
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

// What the link of the run time does in the order in which it does it: it captures the exports of a leaf where it arrives, and resolves the names of a module when it leaves.
static void walkLink(const Graph& graph, unsigned module, BitVector& seen, Vector<uint32_t>& work)
{
    const ModuleData& data = graph.data(module);
    if (!data.needsLink())
        return;
    if (data.capturesExports() || data.isUncompiledCode()) {
        work.append(module);
        return;
    }
    for (uint32_t target : graph.allRequests(module)) {
        if (!seen.quickSet(target))
            walkLink(graph, target, seen, work);
    }
    if (graph.resolvesNames(module))
        work.append(module);
}

static Vector<uint32_t> linkWorkFrom(const Graph& graph, unsigned root)
{
    Vector<uint32_t> work;
    BitVector seen(graph.size());
    seen.quickSet(root);
    walkLink(graph, root, seen, work);
    return work;
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
    AOT_TEST_CHECK(graph.data(required).isSimple());

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

    Vector<LinkPlanStepData> plan = graph.planLink(root);
    AOT_TEST_CHECK(plan.size() == 2 && plan[0].module == face && plan[1].module == librarySource && !plan[0].resolvesNames && !plan[1].resolvesNames);
    AOT_TEST_CHECK(graph.data(face).isLinkedWithEntryPoint());
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
        graph.compute();
        for (unsigned root = 0; root < numberOfModules; ++root) {
            Graph withPlan = graph;
            Vector<LinkPlanStepData> plan = withPlan.planLink(root);
            const ModuleData& rootData = graph.data(root);
            if (!rootData.needsLink() || rootData.capturesExports() || rootData.reachesUncompiledCode()) {
                AOT_TEST_CHECK(plan.isEmpty());
                continue;
            }
            Vector<uint32_t> linkWork = linkWorkFrom(graph, root);
            AOT_TEST_CHECK(plan.size() == linkWork.size());
            for (unsigned step = 0; step < plan.size(); ++step)
                AOT_TEST_CHECK(plan[step].module == linkWork[step] && plan[step].resolvesNames == !graph.data(linkWork[step]).capturesExports());
        }
    }
}

std::span<const NativeTest> moduleRequestListTests()
{
    static constexpr NativeTest tests[] = {
        { "module-request-lists/work-that-needs-evaluation"_s, testWorkThatNeedsEvaluation },
        { "module-request-lists/link-follows-require"_s, testLinkFollowsRequire },
        { "module-request-lists/face-that-captures-is-a-step-and-is-passed"_s, testFaceThatCapturesIsAStepAndIsPassed },
        { "module-request-lists/long-chain"_s, testLongChain },
        { "module-request-lists/link-plan"_s, testLinkPlan },
        { "module-request-lists/entry-points-without-link-plan"_s, testEntryPointsWithoutLinkPlan },
        { "module-request-lists/walks-of-random-graphs-do-the-same-work"_s, testWalksOfRandomGraphsDoTheSameWork },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
