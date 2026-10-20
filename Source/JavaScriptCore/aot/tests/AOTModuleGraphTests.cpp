/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTModuleData.h"
#include "AOTModuleGraph.h"
#include "AOTNativeTest.h"
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

using Flag = ModuleData::Flag;
using Flags = OptionSet<Flag>;

namespace {

enum class Edge : uint8_t { Static, Deferred, DynamicStarExport };

class Graph {
public:
    unsigned addSourceText(Flags wireFlags = { }) { return add(ModuleKind::SourceText, ExternalKind::UncompiledCode, SyntheticExportRule::None, wireFlags); }
    unsigned addData() { return add(ModuleKind::Data, ExternalKind::UncompiledCode, SyntheticExportRule::None, { }); }
    unsigned addCommonJS() { return add(ModuleKind::CommonJs, ExternalKind::UncompiledCode, SyntheticExportRule::CommonJs, { }); }
    unsigned addRuntimeModule() { return add(ModuleKind::Runtime, ExternalKind::UncompiledCode, SyntheticExportRule::RuntimeModule, { }); }
    unsigned addHostValue() { return add(ModuleKind::External, ExternalKind::HostValue, SyntheticExportRule::CommonJs, { }); }
    unsigned addUncompiledCode() { return add(ModuleKind::External, ExternalKind::UncompiledCode, SyntheticExportRule::None, { }); }

    void request(unsigned from, unsigned to, Edge edge = Edge::Static)
    {
        m_nodes[from].requests.append(ModuleRequestData { to, edge == Edge::Deferred, edge == Edge::DynamicStarExport });
    }

    void addRequiredName(unsigned module) { ++m_nodes[module].numberOfRequiredNames; }
    void addSyntheticExport(unsigned module) { ++m_nodes[module].numberOfSyntheticExports; }

    void compute()
    {
        ModuleData endRecord { };
        for (Node& node : m_nodes) {
            node.data.firstRequest = endRecord.firstRequest;
            node.data.firstSyntheticExport = endRecord.firstSyntheticExport;
            node.data.firstRequiredName = endRecord.firstRequiredName;
            m_modules.append(node.data);
            m_requests.appendVector(node.requests);
            endRecord.firstRequest += node.requests.size();
            endRecord.firstSyntheticExport += node.numberOfSyntheticExports;
            endRecord.firstRequiredName += node.numberOfRequiredNames;
        }
        m_modules.append(endRecord);
        computeModuleFlags(m_modules.mutableSpan(), m_requests.span());
        AOT_TEST_CHECK(!m_modules.last().flags);
    }

    Flags flagsOf(unsigned module) const { return Flags::fromRaw(m_modules[module].flags); }
    bool has(unsigned module, Flags expected) const { return flagsOf(module) == expected; }

private:
    struct Node {
        ModuleData data;
        Vector<ModuleRequestData, 2> requests;
        unsigned numberOfSyntheticExports { 0 };
        unsigned numberOfRequiredNames { 0 };
    };

    unsigned add(ModuleKind kind, ExternalKind externalKind, SyntheticExportRule exportRule, Flags wireFlags)
    {
        ModuleData data { };
        data.kind = kind;
        data.externalKind = externalKind;
        data.exportRule = exportRule;
        data.flags = wireFlags.toRaw();
        data.body = UINT32_MAX;
        data.referrer = UINT32_MAX;
        data.runtimeModule = UINT32_MAX;
        m_nodes.append(Node { data, { }, 0, 0 });
        return m_nodes.size() - 1;
    }

    Vector<Node> m_nodes;
    Vector<ModuleData> m_modules;
    Vector<ModuleRequestData> m_requests;
};

constexpr Flags plainModule { Flag::IsSynchronous, Flag::IsSimple };

} // anonymous namespace

static void testChain()
{
    Graph graph;
    unsigned root = graph.addSourceText();
    unsigned middle = graph.addSourceText();
    unsigned leaf = graph.addData();
    graph.request(root, middle);
    graph.request(middle, leaf);
    graph.compute();
    AOT_TEST_CHECK(graph.has(root, plainModule) && graph.has(middle, plainModule) && graph.has(leaf, plainModule));

    Graph single;
    unsigned only = single.addSourceText();
    single.compute();
    AOT_TEST_CHECK(single.has(only, plainModule));

    Graph empty;
    empty.compute();
}

static void testSelfImportIsSimple()
{
    Graph graph;
    unsigned module = graph.addSourceText();
    unsigned importer = graph.addSourceText();
    graph.request(module, module);
    graph.request(importer, module);
    graph.compute();
    AOT_TEST_CHECK(graph.has(module, plainModule) && graph.has(importer, plainModule));
}

static void testModulesOfACycleAreNotSimple()
{
    Graph graph;
    unsigned importer = graph.addSourceText();
    unsigned first = graph.addSourceText();
    unsigned second = graph.addSourceText();
    unsigned leaf = graph.addSourceText();
    graph.request(importer, first);
    graph.request(first, second);
    graph.request(second, first);
    graph.request(second, leaf);
    graph.compute();
    AOT_TEST_CHECK(graph.has(first, Flag::IsSynchronous) && graph.has(second, Flag::IsSynchronous));
    AOT_TEST_CHECK(graph.has(importer, plainModule) && graph.has(leaf, plainModule));

    Graph throughThree;
    unsigned a = throughThree.addSourceText();
    unsigned b = throughThree.addSourceText();
    unsigned c = throughThree.addSourceText();
    throughThree.request(a, b);
    throughThree.request(b, c);
    throughThree.request(c, a, Edge::Deferred);
    throughThree.compute();
    AOT_TEST_CHECK(throughThree.has(a, Flag::IsSynchronous) && throughThree.has(b, Flag::IsSynchronous) && throughThree.has(c, { Flag::IsSynchronous, Flag::HasDeferredRequest }));
}

static void testTopLevelAwaitAtALeaf()
{
    Graph graph;
    unsigned root = graph.addSourceText();
    unsigned importer = graph.addSourceText();
    unsigned sibling = graph.addSourceText();
    unsigned leafOfSibling = graph.addSourceText();
    unsigned awaiting = graph.addSourceText(Flag::HasTopLevelAwait);
    graph.request(root, sibling);
    graph.request(root, importer);
    graph.request(importer, awaiting);
    graph.request(sibling, leafOfSibling);
    graph.compute();
    AOT_TEST_CHECK(graph.has(awaiting, Flag::HasTopLevelAwait));
    AOT_TEST_CHECK(graph.has(importer, Flag::IsSimple) && graph.has(root, Flag::IsSimple));
    AOT_TEST_CHECK(graph.has(sibling, plainModule) && graph.has(leafOfSibling, plainModule));
}

static void testDeferredEdgeToTopLevelAwait()
{
    Graph graph;
    unsigned root = graph.addSourceText();
    unsigned importer = graph.addSourceText();
    unsigned awaiting = graph.addSourceText(Flag::HasTopLevelAwait);
    graph.request(root, importer);
    graph.request(importer, awaiting, Edge::Deferred);
    graph.compute();
    AOT_TEST_CHECK(graph.has(importer, { Flag::IsSimple, Flag::HasDeferredRequest }) && graph.has(root, Flag::IsSimple));

    Graph withoutAwait;
    unsigned plainImporter = withoutAwait.addSourceText();
    unsigned plainLeaf = withoutAwait.addSourceText();
    withoutAwait.request(plainImporter, plainLeaf, Edge::Deferred);
    withoutAwait.compute();
    AOT_TEST_CHECK(withoutAwait.has(plainImporter, plainModule | Flag::HasDeferredRequest) && withoutAwait.has(plainLeaf, plainModule));
}

static void testUncompiledLeaf()
{
    Graph graph;
    unsigned root = graph.addSourceText();
    unsigned importer = graph.addSourceText();
    unsigned sibling = graph.addSourceText();
    unsigned uncompiled = graph.addUncompiledCode();
    graph.request(root, importer);
    graph.request(root, sibling);
    graph.request(importer, uncompiled);
    graph.compute();
    AOT_TEST_CHECK(graph.has(uncompiled, { Flag::NeedsLink, Flag::ReachesUncompiledCode }));
    AOT_TEST_CHECK(graph.has(importer, { Flag::IsSimple, Flag::NeedsLink, Flag::ReachesUncompiledCode }));
    AOT_TEST_CHECK(graph.has(root, { Flag::IsSimple, Flag::NeedsLink, Flag::ReachesUncompiledCode }));
    AOT_TEST_CHECK(graph.has(sibling, plainModule));
}

static void testLeafWhoseExportsAreCaptured()
{
    for (unsigned (Graph::*addLeaf)() : { &Graph::addCommonJS, &Graph::addRuntimeModule, &Graph::addHostValue }) {
        Graph graph;
        unsigned root = graph.addSourceText();
        unsigned importer = graph.addSourceText();
        unsigned sibling = graph.addSourceText();
        unsigned leaf = (graph.*addLeaf)();
        graph.request(root, sibling);
        graph.request(root, importer);
        graph.request(importer, leaf);
        graph.compute();
        AOT_TEST_CHECK(graph.has(leaf, { Flag::IsSynchronous, Flag::NeedsLink }));
        AOT_TEST_CHECK(graph.has(importer, { Flag::IsSynchronous, Flag::IsSimple, Flag::NeedsLink }));
        AOT_TEST_CHECK(graph.has(root, { Flag::IsSynchronous, Flag::IsSimple, Flag::NeedsLink }));
        AOT_TEST_CHECK(graph.has(sibling, plainModule));
    }
}

static void testListsAreLinkWork()
{
    for (void (Graph::*addEntry)(unsigned) : { &Graph::addRequiredName, &Graph::addSyntheticExport }) {
        Graph graph;
        unsigned root = graph.addSourceText();
        unsigned before = graph.addSourceText();
        unsigned withList = graph.addSourceText();
        unsigned after = graph.addSourceText();
        graph.request(root, before);
        graph.request(root, withList);
        graph.request(withList, after);
        (graph.*addEntry)(withList);
        graph.compute();
        AOT_TEST_CHECK(graph.has(withList, { Flag::IsSynchronous, Flag::IsSimple, Flag::NeedsLink }));
        AOT_TEST_CHECK(graph.has(root, { Flag::IsSynchronous, Flag::IsSimple, Flag::NeedsLink }));
        AOT_TEST_CHECK(graph.has(before, plainModule) && graph.has(after, plainModule));
    }
}

static void testCycleMemberReachesAnAwait()
{
    for (bool hasAwait : { true, false }) {
        Graph graph;
        unsigned importer = graph.addSourceText();
        unsigned first = graph.addSourceText();
        unsigned second = graph.addSourceText();
        unsigned third = graph.addSourceText();
        unsigned between = graph.addSourceText();
        unsigned leaf = graph.addSourceText(hasAwait ? Flags { Flag::HasTopLevelAwait } : Flags { });
        graph.request(importer, first);
        graph.request(first, second);
        graph.request(second, third);
        graph.request(third, first);
        graph.request(second, between);
        graph.request(between, leaf);
        graph.compute();
        Flags ifSynchronous = hasAwait ? Flags { } : Flags { Flag::IsSynchronous };
        for (unsigned member : { first, second, third })
            AOT_TEST_CHECK(graph.has(member, ifSynchronous));
        AOT_TEST_CHECK(graph.has(importer, ifSynchronous | Flag::IsSimple) && graph.has(between, ifSynchronous | Flag::IsSimple));
    }
}

static void testCycleMemberAwaits()
{
    Graph graph;
    unsigned first = graph.addSourceText();
    unsigned second = graph.addSourceText(Flag::HasTopLevelAwait);
    unsigned uncompiled = graph.addUncompiledCode();
    graph.request(first, second);
    graph.request(second, first);
    graph.request(first, uncompiled);
    graph.compute();
    AOT_TEST_CHECK(graph.has(first, { Flag::NeedsLink, Flag::ReachesUncompiledCode }));
    AOT_TEST_CHECK(graph.has(second, { Flag::HasTopLevelAwait, Flag::NeedsLink, Flag::ReachesUncompiledCode }));
}

static void testDynamicStarExportIsTheModulesOwn()
{
    Graph graph;
    unsigned importer = graph.addSourceText();
    unsigned barrel = graph.addSourceText();
    unsigned plainTarget = graph.addSourceText();
    unsigned target = graph.addCommonJS();
    graph.request(importer, barrel);
    graph.request(barrel, plainTarget);
    graph.request(barrel, target, Edge::DynamicStarExport);
    graph.compute();
    AOT_TEST_CHECK(graph.has(barrel, { Flag::IsSynchronous, Flag::IsSimple, Flag::NeedsLink, Flag::HasDynamicStarExport }));
    AOT_TEST_CHECK(graph.has(importer, { Flag::IsSynchronous, Flag::IsSimple, Flag::NeedsLink }));
    AOT_TEST_CHECK(graph.has(target, { Flag::IsSynchronous, Flag::NeedsLink }) && graph.has(plainTarget, plainModule));
}

static void testLongChain()
{
    constexpr unsigned numberOfModules = 100000;
    for (bool endsInAwait : { false, true }) {
        Graph graph;
        for (unsigned module = 0; module + 1 < numberOfModules; ++module)
            graph.addSourceText();
        graph.addSourceText(endsInAwait ? Flags { Flag::HasTopLevelAwait } : Flags { });
        for (unsigned module = 0; module + 1 < numberOfModules; ++module)
            graph.request(module, module + 1);
        graph.compute();
        Flags expected = endsInAwait ? Flags { Flag::IsSimple } : plainModule;
        for (unsigned module = 0; module + 1 < numberOfModules; ++module)
            AOT_TEST_CHECK(graph.has(module, expected));
    }

    Graph cycle;
    for (unsigned module = 0; module < numberOfModules; ++module)
        cycle.addSourceText();
    for (unsigned module = 0; module < numberOfModules; ++module)
        cycle.request(module, (module + 1) % numberOfModules);
    cycle.compute();
    for (unsigned module = 0; module < numberOfModules; ++module)
        AOT_TEST_CHECK(cycle.has(module, Flag::IsSynchronous));
}

static void testWireFlagIsUntouched()
{
    Graph graph;
    unsigned awaiting = graph.addSourceText(Flag::HasTopLevelAwait);
    unsigned importer = graph.addSourceText();
    unsigned other = graph.addSourceText();
    graph.request(importer, awaiting);
    graph.compute();
    AOT_TEST_CHECK(graph.flagsOf(awaiting).contains(Flag::HasTopLevelAwait));
    AOT_TEST_CHECK(!graph.flagsOf(importer).contains(Flag::HasTopLevelAwait) && !graph.flagsOf(other).contains(Flag::HasTopLevelAwait));
}

std::span<const NativeTest> moduleGraphTests()
{
    static constexpr NativeTest tests[] = {
        { "module-graph/chain"_s, testChain },
        { "module-graph/self-import-is-simple"_s, testSelfImportIsSimple },
        { "module-graph/modules-of-a-cycle-are-not-simple"_s, testModulesOfACycleAreNotSimple },
        { "module-graph/top-level-await-at-a-leaf"_s, testTopLevelAwaitAtALeaf },
        { "module-graph/deferred-edge-to-top-level-await"_s, testDeferredEdgeToTopLevelAwait },
        { "module-graph/uncompiled-leaf"_s, testUncompiledLeaf },
        { "module-graph/leaf-whose-exports-are-captured"_s, testLeafWhoseExportsAreCaptured },
        { "module-graph/lists-are-link-work"_s, testListsAreLinkWork },
        { "module-graph/cycle-member-reaches-an-await"_s, testCycleMemberReachesAnAwait },
        { "module-graph/cycle-member-awaits"_s, testCycleMemberAwaits },
        { "module-graph/dynamic-star-export-is-the-modules-own"_s, testDynamicStarExportIsTheModulesOwn },
        { "module-graph/long-chain"_s, testLongChain },
        { "module-graph/wire-flag-is-untouched"_s, testWireFlagIsUntouched },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
