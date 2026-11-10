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
    unsigned addCapturingFacade() { return add(ModuleKind::SourceText, ExternalKind::UncompiledCode, SyntheticExportRule::CommonJs, Body::Present, { }); }
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
        Typed::StronglyConnectedComponents components = computeModuleFlags(m_modules.mutableSpan(), m_requests.span());
        LinkRequestsOfModules linkRequests = contractLinkRequests(m_modules.mutableSpan(), m_requests.span(), components);
        layLinkRequests(m_modules.mutableSpan(), linkRequests);
        m_linkRequests = linkRequests.requests.map([](uint32_t module) { return static_cast<uint16_t>(module); });
    }

    Vector<LinkPlanStepData> planLink(unsigned entryPoint) { return planLinkOfEntryPoint(m_modules.mutableSpan(), m_requests.span(), ModuleID(entryPoint)); }
    Vector<uint32_t> group(uint32_t endOfNumbers) { return groupModulesLoadedTogether(m_modules.mutableSpan(), m_requests.span(), findESModulesOfProgram(m_modules.span()), endOfNumbers); }

    unsigned size() const { return m_nodes.size(); }
    ModuleKind kindOf(unsigned module) const { return m_nodes[module].data.kind; }
    const ModuleData& data(unsigned module) const { return m_modules[module]; }
    ModuleNumbers linkRequests() const { return { m_linkRequests.span(), m_modules.size() }; }
    std::span<const ModuleRequestData> requestsOfAllModules() const { return m_requests.span(); }
    bool resolvesNames(unsigned module) const { return !!m_nodes[module].numberOfRequiredNames; }

    std::span<const ModuleRequestData> requests(unsigned module) const { return m_nodes[module].requests.span(); }

    Vector<uint32_t> imports(unsigned module) const
    {
        Vector<uint32_t> targets;
        for (const ModuleRequestData& request : m_nodes[module].requests) {
            if (!request.isRequire && !request.isHoisted)
                targets.append(request.target());
        }
        return targets;
    }

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
        data.referrerRuntimeModuleOrLoadGroup = optionalNumber24(UINT32_MAX);
        m_nodes.append(Node { data, { }, 0 });
        return m_nodes.size() - 1;
    }

    Vector<Node> m_nodes;
    Vector<ModuleData> m_modules;
    Vector<ModuleRequestData> m_requests;
    Vector<uint16_t> m_linkRequests;
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

namespace {

struct LoadsOfGraph {
    uint32_t moduleLoadedUnder(uint32_t target) const { return graph.data(target).isUncompiledCode() ? loadedInPlaceOfUncompiledCode : target; }
    uint32_t headOf(uint32_t module) const { return graph.data(module).isHeadOfLoadGroup() ? module : graph.data(module).numberInLoadGroup(); }
    bool isLoadedByGroups(uint32_t module) const { return (loadedGroups[headOf(module) / 64] >> (headOf(module) % 64) & 1) || loadedBesideGroups.get(module); }

    void loadOneByOne(uint32_t root)
    {
        Vector<uint32_t> found;
        if (!loadedOneByOne.set(root))
            found.append(root);
        while (!found.isEmpty()) {
            for (uint32_t target : graph.imports(found.takeLast())) {
                uint32_t imported = moduleLoadedUnder(target);
                if (graph.data(imported).isESModuleOfProgram() && !loadedOneByOne.set(imported))
                    found.append(imported);
            }
        }
    }

    const ModuleData& data(uint32_t module) const { return graph.data(module); }
    std::span<const ModuleRequestData> requests(uint32_t module) const { return graph.requests(module); }
    std::span<const uint32_t> groupsLoadedWith(uint32_t head) const
    {
        uint32_t first = graph.data(head).numberInLoadGroup();
        return first ? lists.subspan(first, lists[first - 1]) : std::span<const uint32_t>();
    }
    uint32_t moduleLoadedBy(const ModuleRequestData& request) const { return moduleLoadedUnder(request.target()); }

    const Graph& graph;
    std::span<const uint32_t> lists;
    uint32_t loadedInPlaceOfUncompiledCode;
    BitVector loadedOneByOne { };
    Vector<uint64_t> loadedGroups { };
    BitVector loadedBesideGroups { };
};

} // anonymous namespace

static void testGroupsOfRandomGraphsAreLoadedAsTheirModulesAre()
{
    RandomNumbers random(20261105);
    unsigned numberBelowHeads = 0;
    unsigned numberInCycles = 0;
    unsigned numberThatRequestUncompiledCode = 0;
    unsigned numberWithoutRoomForTheirLists = 0;
    unsigned numberOfGraphsWithoutRoomForTheirModules = 0;
    for (unsigned round = 0; round < 4000; ++round) {
        Graph graph;
        unsigned numberOfModules = 1 + random.below(24);
        for (unsigned module = 0; module < numberOfModules; ++module)
            addRandomModule(graph, random);
        for (unsigned module = 0; module < numberOfModules; ++module) {
            addRandomRequests(graph, random, module);
            if (graph.kindOf(module) == ModuleKind::SourceText && !random.below(6))
                graph.request(module, random.below(numberOfModules), Edge::Require);
        }
        graph.compute();
        uint32_t endOfNumbers = ModuleData::endOfNumbersInLoadGroups;
        if (uint32_t draw = random.below(4); draw < 2)
            endOfNumbers = draw ? numberOfModules + random.below(6) : 1 + random.below(numberOfModules);
        Vector<uint32_t> lists = graph.group(endOfNumbers);
        numberOfGraphsWithoutRoomForTheirModules += numberOfModules > endOfNumbers;
        LoadsOfGraph loads { graph, lists.span(), random.below(numberOfModules) };
        loads.loadedGroups.fill(0, numberOfModules / 64 + 1);
        for (unsigned load = 0; load < numberOfModules; ++load) {
            uint32_t root = random.below(numberOfModules);
            if (!graph.data(root).isESModuleOfProgram())
                continue;
            loads.loadOneByOne(root);
            noteGroupsLoadedWith(loads, root, loads.loadedGroups, loads.loadedBesideGroups);
            for (unsigned module = 0; module < numberOfModules; ++module)
                AOT_TEST_CHECK(!graph.data(module).isESModuleOfProgram() || loads.loadedOneByOne.get(module) == loads.isLoadedByGroups(module));
        }
        for (unsigned module = 0; module < numberOfModules; ++module) {
            if (!graph.data(module).isESModuleOfProgram())
                continue;
            numberBelowHeads += graph.data(module).placeInLoadGroup() == ModuleData::PlaceInLoadGroup::BelowHead;
            numberWithoutRoomForTheirLists += numberOfModules <= endOfNumbers && graph.data(module).placeInLoadGroup() == ModuleData::PlaceInLoadGroup::BelowHead && graph.data(module).numberInLoadGroup() == module;
            numberInCycles += graph.data(module).placeInLoadGroup() == ModuleData::PlaceInLoadGroup::InCycleWithHead;
            numberThatRequestUncompiledCode += graph.data(module).placeInLoadGroup() == ModuleData::PlaceInLoadGroup::HeadThatRequestsUncompiledCode;
        }
    }
    AOT_TEST_CHECK(numberBelowHeads > 1000 && numberInCycles > 1000 && numberThatRequestUncompiledCode > 100 && numberWithoutRoomForTheirLists > 100 && numberOfGraphsWithoutRoomForTheirModules > 100);
}

namespace {

struct StepsOfTestWalk {
    struct Step {
        uint32_t module;
        StepOfLinkWalk kind;
        bool resolvesNames;
        friend bool operator==(const Step&, const Step&) = default;
    };

    uint32_t size() const { return static_cast<uint32_t>(steps.size()); }
    void append(uint32_t module, StepOfLinkWalk kind, bool resolvesNames) { steps.append(Step { module, kind, resolvesNames }); }
    uint32_t moduleAt(uint32_t step) const { return steps[step].module; }
    void leave(uint32_t enter, bool resolvesNames) { append(steps[enter].module, StepOfLinkWalk::Leave, resolvesNames); }
    void onlyBecomesLinked(uint32_t enter)
    {
        modulesThatOnlyBecomeLinked.append(steps[enter].module);
        steps.shrink(enter);
    }

    Vector<Step> steps;
    Vector<uint32_t> modulesThatOnlyBecomeLinked;
};

struct LinksOfGraph {
    LinksOfGraph(const Graph& graph, bool loadsModulesIndividually)
        : graph(graph)
        , goesOneByOne(loadsModulesIndividually)
    {
        settled.fill(0, graph.size() / 64 + 1);
    }

    const ModuleData& data(uint32_t module) const { return graph.data(module); }
    uint32_t leaderOf(uint32_t module) const { return graph.linkRequests()[data(module).firstLinkRequest]; }
    bool isUnlinked(uint32_t module) const
    {
        if (!data(module).needsLink() || hasStatusOfItsOwn.get(module))
            return false;
        return goesOneByOne || !data(module).onlyPassesLinkOn() || !hasStatusOfItsOwn.get(leaderOf(module));
    }
    bool isSettledForLinks(uint32_t module) const
    {
        bool isSettled = !data(module).needsLink() || (!data(module).capturesExports() && !data(module).isUncompiledCode() && !isUnlinked(module));
        if (isSettled)
            settled[module / 64] |= 1ull << (module % 64);
        return isSettled;
    }
    std::span<const uint64_t> modulesSettledForLinks() const { return settled.span(); }
    bool loadsModulesIndividually() const { return goesOneByOne; }
    bool goesByAllRequests(uint32_t module) const { return goesOneByOne || leadersThatGoByAllRequests.get(module); }
    void didGetStateOfItsOwn(uint32_t module) const
    {
        if (!goesOneByOne)
            leadersThatGoByAllRequests.set(leaderOf(module));
    }
    ModuleNumbers linkRequests() const { return graph.linkRequests(); }
    uint32_t endOfLinkRequests(uint32_t module) const { return data(module + 1).firstLinkRequest; }
    std::span<const ModuleRequestData> requests() const { return graph.requestsOfAllModules(); }
    uint32_t endOfRequests(uint32_t module) const { return data(module + 1).firstRequest; }
    bool resolvesNames(uint32_t module) const { return !data(module).capturesExports() && graph.resolvesNames(module); }

    StepsOfTestWalk walkFrom(uint32_t root) const
    {
        StepsOfTestWalk steps;
        walkModulesToLink(*this, root, steps);
        return steps;
    }

    void finish(const StepsOfTestWalk& walk)
    {
        if (walk.steps.isEmpty())
            return;
        for (const StepsOfTestWalk::Step& step : walk.steps) {
            if (step.kind == StepOfLinkWalk::Leave)
                hasStatusOfItsOwn.set(step.module);
        }
        for (uint32_t module : walk.modulesThatOnlyBecomeLinked)
            hasStatusOfItsOwn.set(module);
    }

    const Graph& graph;
    bool goesOneByOne;
    BitVector hasStatusOfItsOwn { };
    mutable Vector<uint64_t> settled { };
    mutable BitVector leadersThatGoByAllRequests { };
};

} // anonymous namespace

static void testLinksOfRandomGraphsGoTheSameWayThroughLeaders()
{
    RandomNumbers random(20261105);
    unsigned modulesThatOnlyPassOn = 0;
    unsigned stepsLeftOut = 0;
    unsigned modulesWithNothingToDo = 0;
    unsigned walksThatBeginBelowLeaders = 0;
    unsigned leadersThatWentByAllRequests = 0;
    for (unsigned round = 0; round < 4000; ++round) {
        Graph graph;
        unsigned numberOfModules = 1 + random.below(40);
        for (unsigned module = 0; module < numberOfModules; ++module)
            addRandomModule(graph, random);
        for (unsigned module = 0; module < numberOfModules; ++module)
            addRandomRequests(graph, random, module);
        graph.compute();
        for (unsigned module = 0; module < numberOfModules; ++module)
            modulesThatOnlyPassOn += graph.data(module).onlyPassesLinkOn();

        LinksOfGraph byModules(graph, true);
        LinksOfGraph byLeaders(graph, false);
        for (unsigned turn = 0; turn < 8; ++turn) {
            uint32_t root = random.below(numberOfModules);
            walksThatBeginBelowLeaders += graph.data(root).onlyPassesLinkOn() && byLeaders.isUnlinked(root);
            StepsOfTestWalk walkByModules = byModules.walkFrom(root);
            StepsOfTestWalk walkByLeaders = byLeaders.walkFrom(root);
            AOT_TEST_CHECK(walkByModules.modulesThatOnlyBecomeLinked.isEmpty());
            if (random.below(4)) {
                byModules.finish(walkByModules);
                byLeaders.finish(walkByLeaders);
            }
            AOT_TEST_CHECK(walkByLeaders.steps.size() <= walkByModules.steps.size());
            stepsLeftOut += walkByModules.steps.size() - walkByLeaders.steps.size();
            modulesWithNothingToDo += walkByLeaders.modulesThatOnlyBecomeLinked.size();
            BitVector hasStepsByLeaders;
            for (const StepsOfTestWalk::Step& step : walkByLeaders.steps)
                hasStepsByLeaders.set(step.module);
            auto isLeftOutByLeaders = [&](const StepsOfTestWalk::Step& step) { return !hasStepsByLeaders.get(step.module); };
            for (const StepsOfTestWalk::Step& step : walkByModules.steps)
                AOT_TEST_CHECK(!isLeftOutByLeaders(step) || step.kind == StepOfLinkWalk::Enter || (step.kind == StepOfLinkWalk::Leave && !step.resolvesNames));
            walkByModules.steps.removeAllMatching(isLeftOutByLeaders);
            AOT_TEST_CHECK(walkByModules.steps == walkByLeaders.steps);
            for (unsigned module = 0; module < numberOfModules; ++module)
                AOT_TEST_CHECK(byModules.isUnlinked(module) == byLeaders.isUnlinked(module));
        }
        leadersThatWentByAllRequests += byLeaders.leadersThatGoByAllRequests.bitCount();
    }
    AOT_TEST_CHECK(modulesThatOnlyPassOn > 4000 && stepsLeftOut > 4000 && modulesWithNothingToDo > 400 && walksThatBeginBelowLeaders > 400 && leadersThatWentByAllRequests > 400);
}

std::span<const NativeTest> moduleRequestListTests()
{
    static constexpr NativeTest tests[] = {
        { "module-request-lists/groups-of-random-graphs-are-loaded-as-their-modules-are"_s, testGroupsOfRandomGraphsAreLoadedAsTheirModulesAre },
        { "module-request-lists/links-of-random-graphs-go-the-same-way-through-leaders"_s, testLinksOfRandomGraphsGoTheSameWayThroughLeaders },
        { "module-request-lists/walks-of-random-graphs-do-the-same-work"_s, testWalksOfRandomGraphsDoTheSameWork },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
