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
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

using Flag = ModuleData::Flag;
using Flags = OptionSet<Flag>;

namespace {

enum class Edge : uint8_t { Static, Deferred, DynamicStarExport, Require };

class Graph {
public:
    unsigned addSourceText(Flags wireFlags = { }) { return add(ModuleKind::SourceText, ExternalKind::UncompiledCode, SyntheticExportRule::None, wireFlags); }
    unsigned addData() { return add(ModuleKind::Data, ExternalKind::UncompiledCode, SyntheticExportRule::None, { }); }
    unsigned addCommonJS() { return add(ModuleKind::CommonJs, ExternalKind::UncompiledCode, SyntheticExportRule::CommonJs, { }); }
    unsigned addCommonJSWithoutBoundNames() { return add(ModuleKind::CommonJs, ExternalKind::UncompiledCode, SyntheticExportRule::CommonJs, Flag::CapturesOnDemand); }
    unsigned addRuntimeModule() { return add(ModuleKind::Runtime, ExternalKind::UncompiledCode, SyntheticExportRule::RuntimeModule, { }); }
    unsigned addHostValue() { return add(ModuleKind::External, ExternalKind::HostValue, SyntheticExportRule::CommonJs, { }); }
    unsigned addUncompiledCode() { return add(ModuleKind::External, ExternalKind::UncompiledCode, SyntheticExportRule::None, { }); }

    void request(unsigned from, unsigned to, Edge edge = Edge::Static)
    {
        m_nodes[from].requests.append(ModuleRequestData::create(to, edge == Edge::Deferred, edge == Edge::DynamicStarExport, edge == Edge::Require));
    }

    void addRequiredName(unsigned module) { ++m_nodes[module].numberOfRequiredNames; }
    void addSyntheticExport(unsigned module) { ++m_nodes[module].numberOfSyntheticExports; }
    void addBody(unsigned module) { m_nodes[module].data.body = Number24(0); }

    void compute()
    {
        ModuleData endRecord { };
        for (Node& node : m_nodes) {
            node.data.firstRequest = endRecord.firstRequest;
            node.data.firstSyntheticExport = endRecord.firstSyntheticExport;
            node.data.firstRequiredName = endRecord.firstRequiredName;
            m_modules.append(node.data);
            m_requests.appendVector(node.requests);
            endRecord.firstRequest = Number24(endRecord.firstRequest + node.requests.size());
            endRecord.firstSyntheticExport = Number24(endRecord.firstSyntheticExport + node.numberOfSyntheticExports);
            endRecord.firstRequiredName = Number24(endRecord.firstRequiredName + node.numberOfRequiredNames);
        }
        m_modules.append(endRecord);
        computeModuleFlags(m_modules.mutableSpan(), m_requests.span());
        AOT_TEST_CHECK(!m_modules.last().flags);
    }

    Flags flagsOf(unsigned module) const { return Flags::fromRaw(m_modules[module].flags); }
    bool has(unsigned module, Flags expected) const { return flagsOf(module) == expected; }
    bool isImportedByModule(unsigned module) const { return m_modules[module].isImportedByModule(); }

    void group(std::span<const ModuleImportSite> sites) { m_groupsLoadedWithGroups = groupModulesLoadedTogether(m_modules.mutableSpan(), m_requests.span(), sites); }
    void group(std::initializer_list<ModuleImportSite> sites) { m_groupsLoadedWithGroups = groupModulesLoadedTogether(m_modules.mutableSpan(), m_requests.span(), std::span(sites.begin(), sites.size())); }
    unsigned headOf(unsigned module) const
    {
        uint32_t group = m_modules[module].referrerRuntimeModuleOrLoadGroup;
        return group < m_nodes.size() ? group : module;
    }
    bool areLoadedTogether(std::initializer_list<unsigned> modules) const
    {
        return std::ranges::all_of(modules, [&](unsigned module) {
            return headOf(module) == headOf(*modules.begin());
        });
    }
    // Of the group of the module, each by one of its modules.
    bool groupsLoadedWithItAre(unsigned module, std::initializer_list<unsigned> expected) const
    {
        uint32_t group = m_modules[headOf(module)].referrerRuntimeModuleOrLoadGroup;
        if (group == Number24::max)
            return !expected.size();
        std::span<const uint32_t> list = m_groupsLoadedWithGroups.span().subspan(group - m_nodes.size() + 1, m_groupsLoadedWithGroups[group - m_nodes.size()]);
        return list.size() == expected.size() && std::ranges::all_of(expected, [&](unsigned other) {
            return std::ranges::contains(list, headOf(other));
        });
    }
    bool hasNoLists() const { return m_groupsLoadedWithGroups.isEmpty(); }

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
        data.body = optionalNumber24(UINT32_MAX);
        data.referrerRuntimeModuleOrLoadGroup = optionalNumber24(UINT32_MAX);
        m_nodes.append(Node { data, { }, 0, 0 });
        return m_nodes.size() - 1;
    }

    Vector<Node> m_nodes;
    Vector<ModuleData> m_modules;
    Vector<ModuleRequestData> m_requests;
    Vector<uint32_t> m_groupsLoadedWithGroups;
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

static void testOnlyWhatAModuleImportsIsImportedByAModule()
{
    Graph graph;
    unsigned root = graph.addSourceText();
    unsigned imported = graph.addSourceText();
    unsigned deferred = graph.addSourceText();
    unsigned required = graph.addSourceText();
    unsigned both = graph.addSourceText();
    graph.request(root, imported);
    graph.request(root, deferred, Edge::Deferred);
    graph.request(root, required, Edge::Require);
    graph.request(root, both, Edge::Require);
    graph.request(imported, both);
    graph.compute();
    AOT_TEST_CHECK(!graph.isImportedByModule(root) && graph.isImportedByModule(imported) && graph.isImportedByModule(deferred));
    AOT_TEST_CHECK(!graph.isImportedByModule(required) && graph.isImportedByModule(both));
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
    AOT_TEST_CHECK(throughThree.has(a, { Flag::IsSynchronous, Flag::NeedsEvaluation }) && throughThree.has(b, { Flag::IsSynchronous, Flag::NeedsEvaluation }));
    AOT_TEST_CHECK(throughThree.has(c, { Flag::IsSynchronous, Flag::HasDeferredRequest, Flag::NeedsEvaluation }));
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
    AOT_TEST_CHECK(graph.has(awaiting, { Flag::HasTopLevelAwait, Flag::NeedsEvaluation }));
    AOT_TEST_CHECK(graph.has(importer, { Flag::IsSimple, Flag::NeedsEvaluation }) && graph.has(root, { Flag::IsSimple, Flag::NeedsEvaluation }));
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
    AOT_TEST_CHECK(graph.has(importer, { Flag::IsSimple, Flag::HasDeferredRequest, Flag::NeedsEvaluation }) && graph.has(root, { Flag::IsSimple, Flag::NeedsEvaluation }));

    Graph withoutAwait;
    unsigned plainImporter = withoutAwait.addSourceText();
    unsigned plainLeaf = withoutAwait.addSourceText();
    withoutAwait.request(plainImporter, plainLeaf, Edge::Deferred);
    withoutAwait.compute();
    AOT_TEST_CHECK(withoutAwait.has(plainImporter, plainModule | Flags { Flag::HasDeferredRequest, Flag::NeedsEvaluation }) && withoutAwait.has(plainLeaf, plainModule));
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
    AOT_TEST_CHECK(graph.has(uncompiled, { Flag::NeedsLink, Flag::ReachesUncompiledCode, Flag::NeedsEvaluation }));
    AOT_TEST_CHECK(graph.has(importer, { Flag::IsSimple, Flag::NeedsLink, Flag::ReachesUncompiledCode, Flag::NeedsEvaluation }));
    AOT_TEST_CHECK(graph.has(root, { Flag::IsSimple, Flag::NeedsLink, Flag::ReachesUncompiledCode, Flag::NeedsEvaluation }));
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

static void testBodyIsEvaluationWork()
{
    Graph graph;
    unsigned root = graph.addSourceText();
    unsigned importer = graph.addSourceText();
    unsigned sibling = graph.addSourceText();
    unsigned withBody = graph.addSourceText();
    graph.request(root, sibling);
    graph.request(root, importer);
    graph.request(importer, withBody);
    graph.addBody(withBody);
    graph.compute();
    for (unsigned module : { root, importer, withBody })
        AOT_TEST_CHECK(graph.has(module, plainModule | Flag::NeedsEvaluation));
    AOT_TEST_CHECK(graph.has(sibling, plainModule));

    // A module of the library runs when its importer is linked.
    Graph linked;
    unsigned importerOfLibrary = linked.addSourceText();
    unsigned library = linked.addRuntimeModule();
    linked.request(importerOfLibrary, library);
    linked.addBody(library);
    linked.compute();
    AOT_TEST_CHECK(linked.has(library, { Flag::IsSynchronous, Flag::NeedsLink }) && linked.has(importerOfLibrary, { Flag::IsSynchronous, Flag::IsSimple, Flag::NeedsLink }));

    // A CommonJS file runs where the evaluation comes to it.
    Graph evaluated;
    unsigned importerOfCommonJS = evaluated.addSourceText();
    unsigned commonJS = evaluated.addCommonJSWithoutBoundNames();
    evaluated.request(importerOfCommonJS, commonJS);
    evaluated.addBody(commonJS);
    evaluated.compute();
    AOT_TEST_CHECK(evaluated.has(commonJS, { Flag::CapturesOnDemand, Flag::IsSynchronous, Flag::NeedsEvaluation }) && evaluated.has(importerOfCommonJS, plainModule | Flag::NeedsEvaluation));
}

// What a module requires runs inside its body, so the module waits for nothing and has nothing more to evaluate on that account. It is linked together with what it requires.
static void testRequireIsFollowedByLinkAlone()
{
    Graph graph;
    unsigned importer = graph.addSourceText();
    unsigned requirer = graph.addCommonJSWithoutBoundNames();
    unsigned awaiting = graph.addSourceText(Flag::HasTopLevelAwait);
    unsigned withBody = graph.addSourceText();
    unsigned importerOfLibrary = graph.addSourceText();
    unsigned library = graph.addRuntimeModule();
    graph.request(importer, requirer);
    graph.request(requirer, awaiting, Edge::Require);
    graph.request(requirer, withBody, Edge::Require);
    graph.request(requirer, importerOfLibrary, Edge::Require);
    graph.request(importerOfLibrary, library);
    graph.addBody(withBody);
    graph.compute();
    AOT_TEST_CHECK(graph.has(requirer, { Flag::CapturesOnDemand, Flag::IsSynchronous, Flag::NeedsLink }));
    AOT_TEST_CHECK(graph.has(importer, plainModule | Flag::NeedsLink));
    AOT_TEST_CHECK(graph.has(importerOfLibrary, plainModule | Flag::NeedsLink) && graph.has(withBody, plainModule | Flag::NeedsEvaluation));
}

static void testRequireMakesNoCycleOfImports()
{
    Graph graph;
    unsigned first = graph.addSourceText();
    unsigned second = graph.addSourceText();
    unsigned library = graph.addRuntimeModule();
    graph.request(first, second);
    graph.request(second, first, Edge::Require);
    graph.request(second, library);
    graph.compute();
    AOT_TEST_CHECK(graph.has(first, plainModule | Flag::NeedsLink) && graph.has(second, plainModule | Flag::NeedsLink));
}

static void testCommonJSWithoutBoundNamesIsNoLinkWork()
{
    Graph graph;
    unsigned importer = graph.addSourceText();
    unsigned commonJS = graph.addCommonJSWithoutBoundNames();
    graph.request(importer, commonJS);
    graph.compute();
    AOT_TEST_CHECK(graph.has(commonJS, { Flag::CapturesOnDemand, Flag::IsSynchronous }) && graph.has(importer, plainModule));
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
        Flags decidedByLeaf = hasAwait ? Flags { Flag::NeedsEvaluation } : Flags { Flag::IsSynchronous };
        for (unsigned member : { first, second, third })
            AOT_TEST_CHECK(graph.has(member, decidedByLeaf));
        AOT_TEST_CHECK(graph.has(importer, decidedByLeaf | Flag::IsSimple) && graph.has(between, decidedByLeaf | Flag::IsSimple));
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
    AOT_TEST_CHECK(graph.has(first, { Flag::NeedsLink, Flag::ReachesUncompiledCode, Flag::NeedsEvaluation }));
    AOT_TEST_CHECK(graph.has(second, { Flag::HasTopLevelAwait, Flag::NeedsLink, Flag::ReachesUncompiledCode, Flag::NeedsEvaluation }));
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
        Flags expected = endsInAwait ? Flags { Flag::IsSimple, Flag::NeedsEvaluation } : plainModule;
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

static void testWhatOneRootLeadsToIsLoadedTogether()
{
    Graph graph;
    unsigned entryPoint = graph.addSourceText(Flag::IsEntryPoint);
    unsigned first = graph.addSourceText();
    unsigned second = graph.addSourceText();
    unsigned file = graph.addCommonJS();
    unsigned imported = graph.addSourceText();
    unsigned below = graph.addSourceText();
    graph.request(entryPoint, first);
    graph.request(entryPoint, file);
    graph.request(first, second);
    graph.request(second, first);
    graph.request(imported, below);
    graph.request(imported, second);
    graph.compute();
    graph.group({ { entryPoint, imported } });
    AOT_TEST_CHECK(graph.areLoadedTogether({ entryPoint, first, second }) && graph.areLoadedTogether({ imported, below }) && !graph.areLoadedTogether({ entryPoint, imported }));
    // Whoever calls that import() runs in a module that was loaded with the entry point.
    AOT_TEST_CHECK(graph.hasNoLists());
}

static void testWhatTwoRootsLeadToIsAGroupOfItsOwn()
{
    Graph graph;
    unsigned entryPoint = graph.addSourceText(Flag::IsEntryPoint);
    unsigned left = graph.addSourceText();
    unsigned right = graph.addSourceText();
    unsigned onlyLeft = graph.addSourceText();
    unsigned shared = graph.addSourceText();
    unsigned belowShared = graph.addSourceText();
    unsigned late = graph.addSourceText();
    unsigned onlyLate = graph.addSourceText();
    graph.request(left, onlyLeft);
    graph.request(onlyLeft, shared);
    graph.request(right, shared);
    graph.request(shared, belowShared);
    graph.request(late, belowShared);
    graph.request(late, onlyLate);
    graph.compute();
    // The last import() stands in a module that only one of the others loads.
    graph.group({ { entryPoint, left }, { entryPoint, right }, { onlyLeft, late } });
    AOT_TEST_CHECK(graph.areLoadedTogether({ left, onlyLeft }) && graph.areLoadedTogether({ shared, belowShared }) && graph.areLoadedTogether({ late, onlyLate }));
    AOT_TEST_CHECK(!graph.areLoadedTogether({ left, shared }) && !graph.areLoadedTogether({ right, shared }) && !graph.areLoadedTogether({ left, right }) && !graph.areLoadedTogether({ late, left }));
    AOT_TEST_CHECK(graph.groupsLoadedWithItAre(left, { shared }) && graph.groupsLoadedWithItAre(right, { shared }));
    AOT_TEST_CHECK(graph.groupsLoadedWithItAre(shared, { }) && graph.groupsLoadedWithItAre(late, { }) && graph.groupsLoadedWithItAre(entryPoint, { }));
}

// More roots than go at once, some below another root, over imports with cycles: modules are together exactly where the same roots come to them first.
static void testManyRootsSplitLikeOneAtATime()
{
    constexpr unsigned numberOfEntryPoints = 150;
    constexpr unsigned numberOfRootsBelow = 100;
    constexpr unsigned numberOfModules = 300;
    Graph graph;
    unsigned top = graph.addSourceText(Flag::IsEntryPoint);
    Vector<unsigned> roots;
    for (unsigned index = 0; index < numberOfEntryPoints; ++index)
        roots.append(graph.addSourceText(Flag::IsEntryPoint));
    for (unsigned index = 0; index < numberOfRootsBelow; ++index)
        roots.append(graph.addSourceText());
    unsigned firstModule = roots.last() + 1;
    for (unsigned index = 0; index < numberOfModules; ++index)
        graph.addSourceText();
    unsigned numberOfNodes = firstModule + numberOfModules;
    Vector<Vector<unsigned>> imports(numberOfNodes);
    uint32_t seed = 12345;
    auto draw = [&](unsigned below) {
        seed = seed * 48271 % 2147483647;
        return seed % below;
    };
    auto import = [&](unsigned from, unsigned to) {
        graph.request(from, to);
        imports[from].append(to);
    };
    for (unsigned count = 0; count < 6; ++count)
        import(top, firstModule + draw(numberOfModules));
    for (unsigned root : roots) {
        for (unsigned count = 0; count < 3; ++count)
            import(root, firstModule + draw(numberOfModules));
    }
    for (unsigned index = 0; index < numberOfModules; ++index) {
        // Mostly toward the end. Every tenth one goes back, which makes cycles.
        unsigned module = firstModule + index;
        if (index + 1 < numberOfModules)
            import(module, module + 1 + draw(std::min(numberOfModules - index - 1, 8u)));
        if (!(index % 10))
            import(module, firstModule + draw(numberOfModules));
    }
    graph.compute();
    Vector<ModuleImportSite> sites;
    for (unsigned index = numberOfEntryPoints; index < roots.size(); ++index)
        sites.append(ModuleImportSite { top, roots[index] });
    graph.group(sites.span());

    auto reachOf = [&](unsigned root) {
        Vector<bool> isReached(FillWith { }, numberOfNodes, false);
        Vector<unsigned> found { root };
        isReached[root] = true;
        while (!found.isEmpty()) {
            for (unsigned target : imports[found.takeLast()]) {
                if (!std::exchange(isReached[target], true))
                    found.append(target);
            }
        }
        return isReached;
    };
    Vector<bool> reachOfTop = reachOf(top);
    Vector<Vector<unsigned>> rootsOf(numberOfNodes);
    for (unsigned module = 0; module < numberOfNodes; ++module) {
        if (reachOfTop[module])
            rootsOf[module].append(top);
    }
    for (unsigned index = 0; index < roots.size(); ++index) {
        Vector<bool> reach = reachOf(roots[index]);
        for (unsigned module = 0; module < numberOfNodes; ++module) {
            if (reach[module] && (index < numberOfEntryPoints || !reachOfTop[module]))
                rootsOf[module].append(roots[index]);
        }
    }
    for (unsigned first = 0; first < numberOfNodes; ++first) {
        for (unsigned second = first + 1; second < numberOfNodes; ++second) {
            if (!rootsOf[first].isEmpty() || !rootsOf[second].isEmpty())
                AOT_TEST_CHECK((rootsOf[first] == rootsOf[second]) == graph.areLoadedTogether({ first, second }));
        }
    }
}

static void testARootThatTwoModulesNameIsBelowNeither()
{
    Graph graph;
    unsigned entryPoint = graph.addSourceText(Flag::IsEntryPoint);
    unsigned left = graph.addSourceText();
    unsigned right = graph.addSourceText();
    unsigned belowLeft = graph.addSourceText();
    unsigned late = graph.addSourceText();
    graph.request(left, belowLeft);
    graph.request(late, belowLeft);
    graph.request(right, late, Edge::Require);
    graph.compute();
    graph.group({ { entryPoint, left }, { entryPoint, right }, { left, late } });
    // The require() may load it while nothing has loaded the other importer.
    AOT_TEST_CHECK(!graph.areLoadedTogether({ left, belowLeft }) && !graph.areLoadedTogether({ late, belowLeft }));
    AOT_TEST_CHECK(graph.groupsLoadedWithItAre(left, { belowLeft }) && graph.groupsLoadedWithItAre(late, { belowLeft }) && graph.groupsLoadedWithItAre(right, { }));
}

static void testWhatAFileNamesIsBelowNoRoot()
{
    Graph graph;
    unsigned entryPoint = graph.addSourceText(Flag::IsEntryPoint);
    unsigned imported = graph.addSourceText();
    unsigned file = graph.addCommonJS();
    unsigned required = graph.addSourceText();
    unsigned shared = graph.addSourceText();
    graph.request(imported, file);
    graph.request(imported, shared);
    graph.request(file, required, Edge::Require);
    graph.request(required, shared);
    graph.compute();
    graph.group({ { entryPoint, imported } });
    // The file runs while the graph that imports it is loaded.
    AOT_TEST_CHECK(!graph.areLoadedTogether({ imported, shared }) && !graph.areLoadedTogether({ required, shared }));
    AOT_TEST_CHECK(graph.groupsLoadedWithItAre(imported, { shared }) && graph.groupsLoadedWithItAre(required, { shared }));
}

std::span<const NativeTest> moduleGraphTests()
{
    static constexpr NativeTest tests[] = {
        { "module-graph/chain"_s, testChain },
        { "module-graph/only-what-a-module-imports-is-imported-by-a-module"_s, testOnlyWhatAModuleImportsIsImportedByAModule },
        { "module-graph/self-import-is-simple"_s, testSelfImportIsSimple },
        { "module-graph/modules-of-a-cycle-are-not-simple"_s, testModulesOfACycleAreNotSimple },
        { "module-graph/top-level-await-at-a-leaf"_s, testTopLevelAwaitAtALeaf },
        { "module-graph/deferred-edge-to-top-level-await"_s, testDeferredEdgeToTopLevelAwait },
        { "module-graph/uncompiled-leaf"_s, testUncompiledLeaf },
        { "module-graph/leaf-whose-exports-are-captured"_s, testLeafWhoseExportsAreCaptured },
        { "module-graph/body-is-evaluation-work"_s, testBodyIsEvaluationWork },
        { "module-graph/require-is-followed-by-link-alone"_s, testRequireIsFollowedByLinkAlone },
        { "module-graph/require-makes-no-cycle-of-imports"_s, testRequireMakesNoCycleOfImports },
        { "module-graph/commonjs-without-bound-names-is-no-link-work"_s, testCommonJSWithoutBoundNamesIsNoLinkWork },
        { "module-graph/lists-are-link-work"_s, testListsAreLinkWork },
        { "module-graph/cycle-member-reaches-an-await"_s, testCycleMemberReachesAnAwait },
        { "module-graph/cycle-member-awaits"_s, testCycleMemberAwaits },
        { "module-graph/dynamic-star-export-is-the-modules-own"_s, testDynamicStarExportIsTheModulesOwn },
        { "module-graph/long-chain"_s, testLongChain },
        { "module-graph/wire-flag-is-untouched"_s, testWireFlagIsUntouched },
        { "module-graph/what-one-root-leads-to-is-loaded-together"_s, testWhatOneRootLeadsToIsLoadedTogether },
        { "module-graph/what-two-roots-lead-to-is-a-group-of-its-own"_s, testWhatTwoRootsLeadToIsAGroupOfItsOwn },
        { "module-graph/many-roots-split-like-one-at-a-time"_s, testManyRootsSplitLikeOneAtATime },
        { "module-graph/a-root-that-two-modules-name-is-below-neither"_s, testARootThatTwoModulesNameIsBelowNeither },
        { "module-graph/what-a-file-names-is-below-no-root"_s, testWhatAFileNamesIsBelowNoRoot },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
