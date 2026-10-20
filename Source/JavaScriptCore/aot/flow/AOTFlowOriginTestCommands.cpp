/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowNames.h"
#include "AOTIRPrinter.h"
#include <algorithm>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/Dominators.h>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr size_t maxOriginsWithReach = 200;
constexpr size_t maxPrintedOrigins = 1000;
constexpr unsigned maxPrintedOperationLength = 200;
constexpr size_t maxPrintedMeetingPoints = 60;
constexpr size_t maxPrintedOriginsOfMeetingPoint = 60;

using NodePair = std::pair<uint32_t, uint32_t>;

class Adjacency {
public:
    Adjacency(uint32_t numberOfNodes, std::span<const NodePair> ascendingPairs)
        : m_firstTarget(FillWith { }, numberOfNodes + 1, 0u)
    {
        for (auto [from, to] : ascendingPairs)
            ++m_firstTarget[from + 1];
        for (uint32_t node = 0; node < numberOfNodes; ++node)
            m_firstTarget[node + 1] += m_firstTarget[node];
        m_targets = WTF::map(ascendingPairs, [](NodePair pair) {
            return pair.second;
        });
    }

    uint32_t numberOfNodes() const { return m_firstTarget.size() - 1; }

    std::span<const uint32_t> targetsOf(uint32_t node) const LIFETIME_BOUND
    {
        return m_targets.subspan(m_firstTarget[node], m_firstTarget[node + 1] - m_firstTarget[node]);
    }

private:
    Vector<uint32_t> m_firstTarget;
    Vector<uint32_t> m_targets;
};

using PathNode = ID<struct FlowUnknownObjectPathNodeTag>;

// The ways the unknown object takes, in the form WTF::Dominators reads. Node 0 is the root, then comes a node for each origin, then each node of the flow graph that some origin reaches.
class PathGraph {
public:
    using Node = PathNode;
    using Set = IndexSet<PathNode>;
    template<typename T> using Map = IndexMap<PathNode, T>;
    using List = Vector<PathNode, 4>;

    PathGraph(uint32_t numberOfNodes, std::span<const NodePair> ascendingEdges, std::span<const NodePair> ascendingReversedEdges)
        : m_firstSuccessor(offsetsOf(numberOfNodes, ascendingEdges))
        , m_successors(targetsOf(ascendingEdges))
        , m_firstPredecessor(offsetsOf(numberOfNodes, ascendingReversedEdges))
        , m_predecessors(targetsOf(ascendingReversedEdges))
    {
    }

    PathNode root() const { return PathNode(0u); }
    unsigned numNodes() const { return m_firstSuccessor.size() - 1; }
    unsigned index(PathNode node) const { return node.index(); }
    PathNode node(unsigned index) const { return PathNode(index); }
    template<typename T> Map<T> newMap() const { return Map<T>(numNodes()); }
    std::span<const PathNode> successors(PathNode node) const LIFETIME_BOUND { return m_successors.subspan(m_firstSuccessor[node.index()], m_firstSuccessor[node.index() + 1] - m_firstSuccessor[node.index()]); }
    std::span<const PathNode> predecessors(PathNode node) const LIFETIME_BOUND { return m_predecessors.subspan(m_firstPredecessor[node.index()], m_firstPredecessor[node.index() + 1] - m_firstPredecessor[node.index()]); }
    unsigned dump(PathNode node) const { return node.index(); }
    void dump(PrintStream& out) const { out.print(numNodes(), " nodes"_s); }

private:
    static Vector<uint32_t> offsetsOf(uint32_t numberOfNodes, std::span<const NodePair> ascendingEdges)
    {
        Vector<uint32_t> offsets(FillWith { }, numberOfNodes + 1, 0u);
        for (auto [from, to] : ascendingEdges)
            ++offsets[from + 1];
        for (uint32_t node = 0; node < numberOfNodes; ++node)
            offsets[node + 1] += offsets[node];
        return offsets;
    }

    static Vector<PathNode> targetsOf(std::span<const NodePair> ascendingEdges)
    {
        return WTF::map(ascendingEdges, [](NodePair edge) {
            return PathNode(edge.second);
        });
    }

    Vector<uint32_t> m_firstSuccessor;
    Vector<PathNode> m_successors;
    Vector<uint32_t> m_firstPredecessor;
    Vector<PathNode> m_predecessors;
};

// In the scope of the application the rest of the program is left out, and a node out there that hands the object in is an origin like any other.
enum class OriginScope : uint8_t { Program, Application };

// A witness without a function stands for a node that no operation names: its value is the index of the node.
struct OriginCounts {
    Witness witness;
    FlowNodeID firstNode;
    uint32_t entries { 0 };
};

struct OriginEntry {
    Witness witness;
    FlowNodeID node;
};

struct Origins {
    Vector<OriginCounts> origins;
    Vector<NodePair> entriesByOrigin;
    Vector<NodePair> ascendingPaths;
};

// What one child of the root dominates: an origin with all that it alone leads to, or a node where the ways of several origins meet with all that lies behind that node alone.
// The same origins reach every node of a region. Region n is origin n as long as there are origins, and the regions after those are the meeting points.
struct Region {
    uint64_t accesses { 0 };
    uint64_t nodes { 0 };
    FlowNodeID top;
};

struct Regions {
    Vector<Region> regions;
    Vector<NodePair> ascendingEdges;
    uint64_t nodesWithUnknownObject { 0 };
    uint64_t accessesThroughUnknownObject { 0 };
    uint64_t accessesWithoutOrigin { 0 };
};

} // anonymous namespace

static auto witnessKey(const Witness& witness)
{
    return std::tuple { witness.function, witness.value };
}

static Vector<NodePair> collectPaths(const FlowGraph& graph, const FlowRecords& records)
{
    auto holdsUnknownObject = [&](FlowNodeID node) {
        return graph.valueOf(node).hasUnknownObject();
    };

    Vector<NodePair> paths;
    for (uint32_t index = 0; index < graph.numberOfNodes(); ++index) {
        FlowNodeID node(index);
        FlowNodeKind kind = graph.kindOf(node);
        if (!holdsUnknownObject(node) || kind == FlowNodeKind::NativeCode || kind == FlowNodeKind::UnknownJavaScript)
            continue;
        graph.forEachCopyTarget(node, [&](FlowNodeID target) {
            if (target != node && holdsUnknownObject(target))
                paths.append({ index, target.index() });
        });
    }
    for (const FlowUnknownObjectDerivation& derivation : records.unknownObjectDerivations())
        paths.append({ derivation.subject.index(), derivation.result.index() });

    std::ranges::sort(paths);
    paths.shrink(std::ranges::unique(paths).begin() - paths.begin());
    return paths;
}

static Vector<NodePair> reversed(std::span<const NodePair> paths)
{
    Vector<NodePair> result = WTF::map(paths, [](NodePair pair) {
        return NodePair { pair.second, pair.first };
    });
    std::ranges::sort(result);
    return result;
}

static Vector<uint32_t> countAccessesByReceiver(const FlowGraph& graph)
{
    Vector<uint32_t> accesses(FillWith { }, graph.numberOfNodes(), 0u);
    for (uint32_t index = 0; index < graph.numberOfInstances(); ++index) {
        FlowInstanceID instance(index);
        if (graph.at(instance).state == FlowInstanceState::Created || graph.isHost(instance))
            continue;
        auto count = [&](FlowLocalID receiver) {
            if (receiver)
                ++accesses[graph.nodeOf(instance, receiver).index()];
        };
        const FlowStatements& statements = graph.summaryOf(instance).statements;
        for (const FlowFieldAccess& access : statements.fieldAccesses)
            count(access.receiver);
        for (const FlowPropertyAccess& access : statements.propertyAccesses)
            count(access.receiver);
    }
    return accesses;
}

static void sortAndRemoveRepeated(Vector<NodePair>& pairs)
{
    std::ranges::sort(pairs);
    pairs.shrink(std::ranges::unique(pairs).begin() - pairs.begin());
}

static bool isWrittenByApplication(const Program& program, FunctionID function)
{
    const Module& module = program.at(program.at(function).module);
    bool isSourceOfProgram = module.kind == ModuleKind::SourceText || module.kind == ModuleKind::CommonJs;
    return isSourceOfProgram && !module.flags.contains(ModuleFlags::IsLibrary);
}

static BitVector findNodesInScope(const FlowSolvedProgram& solved, OriginScope scope)
{
    const FlowGraph& graph = *solved.graph;
    const Program& program = solved.program;
    BitVector nodesInScope(graph.numberOfNodes());
    for (uint32_t index = 0; index < graph.numberOfNodes(); ++index) {
        if (scope == OriginScope::Program) {
            nodesInScope.quickSet(index);
            continue;
        }
        FunctionID function = graph.functionChargedFor(FlowNodeID(index));
        if (!function || graph.isHost(function))
            continue;
        if (isWrittenByApplication(program, function))
            nodesInScope.quickSet(index);
    }
    return nodesInScope;
}

static Witness witnessOfNodeOutOfScope(const FlowGraph& graph, FlowNodeID node)
{
    if (graph.kindOf(node) == FlowNodeKind::Local) {
        FlowInstanceID instance = graph.instanceOf(node);
        Witness witness = graph.witnessOf(instance, graph.summaryOf(instance).originOf(graph.localOf(node)));
        if (hasFunction(witness))
            return witness;
    }
    return Witness { .function = Wire::none, .value = node.index() };
}

// One witness is one origin here, however many times the records list it.
static Origins collectOrigins(const FlowSolvedProgram& solved, OriginScope scope)
{
    const FlowGraph& graph = *solved.graph;
    const FlowRecords& records = solved.records;
    BitVector nodesInScope = findNodesInScope(solved, scope);

    Vector<OriginEntry> entries;
    for (const FlowUnknownObjectEntry& entry : records.unknownObjectEntries()) {
        if (nodesInScope.quickGet(entry.node.index()))
            entries.append({ records.unknownObjectOrigins()[entry.origin.index()].witness, entry.node });
    }

    Origins result;
    for (auto [from, to] : collectPaths(graph, records)) {
        if (!nodesInScope.quickGet(to))
            continue;
        if (nodesInScope.quickGet(from))
            result.ascendingPaths.append({ from, to });
        else
            entries.append({ witnessOfNodeOutOfScope(graph, FlowNodeID(from)), FlowNodeID(to) });
    }

    auto keyOf = [](const OriginEntry& entry) {
        return std::tuple { witnessKey(entry.witness), entry.node };
    };
    std::ranges::sort(entries, { }, keyOf);
    for (size_t index = 0; index < entries.size(); ++index) {
        const OriginEntry& entry = entries[index];
        if (index && keyOf(entries[index - 1]) == keyOf(entry))
            continue;
        if (result.origins.isEmpty() || witnessKey(result.origins.last().witness) != witnessKey(entry.witness))
            result.origins.append(OriginCounts { .witness = entry.witness, .firstNode = entry.node });
        ++result.origins.last().entries;
        result.entriesByOrigin.append({ static_cast<uint32_t>(result.origins.size() - 1), entry.node.index() });
    }
    return result;
}

// The path node of each node of the flow graph that an origin reaches, and zero for any other.
static Vector<uint32_t> numberReachedNodes(const Adjacency& paths, std::span<const NodePair> entriesByOrigin, uint32_t firstPathNode)
{
    Vector<uint32_t> pathNodeOf(FillWith { }, paths.numberOfNodes(), 0u);
    Vector<uint32_t> worklist;
    uint32_t nextPathNode = firstPathNode;
    auto reach = [&](uint32_t node) {
        if (pathNodeOf[node])
            return;
        pathNodeOf[node] = nextPathNode++;
        worklist.append(node);
    };
    for (auto [origin, node] : entriesByOrigin)
        reach(node);
    while (!worklist.isEmpty()) {
        for (uint32_t target : paths.targetsOf(worklist.takeLast()))
            reach(target);
    }
    return pathNodeOf;
}

// The child of the root that dominates each path node, by the index of that child.
static Vector<uint32_t> findDominatingChildrenOfRoot(PathGraph& graph)
{
    WTF::Dominators<PathGraph> dominators(graph);
    Vector<uint32_t> childOf(FillWith { }, graph.numNodes(), 0u);
    Vector<uint32_t, 32> climbed;
    for (uint32_t index = 1; index < graph.numNodes(); ++index) {
        uint32_t node = index;
        while (!childOf[node]) {
            uint32_t parent = dominators.idom(PathNode(node)).index();
            if (!parent) {
                childOf[node] = node;
                break;
            }
            climbed.append(node);
            node = parent;
        }
        for (uint32_t below : climbed)
            childOf[below] = childOf[node];
        climbed.shrink(0);
    }
    return childOf;
}

static Regions computeRegions(const FlowGraph& graph, std::span<const NodePair> paths, std::span<const NodePair> entriesByOrigin, uint32_t numberOfOrigins, std::span<const uint32_t> accesses)
{
    uint32_t firstPathNode = numberOfOrigins + 1;
    Vector<uint32_t> pathNodeOf = numberReachedNodes(Adjacency(graph.numberOfNodes(), paths), entriesByOrigin, firstPathNode);

    Vector<NodePair> edges;
    for (uint32_t origin = 0; origin < numberOfOrigins; ++origin)
        edges.append({ 0, origin + 1 });
    for (auto [origin, node] : entriesByOrigin)
        edges.append({ origin + 1, pathNodeOf[node] });
    for (auto [from, to] : paths) {
        if (pathNodeOf[from])
            edges.append({ pathNodeOf[from], pathNodeOf[to] });
    }
    sortAndRemoveRepeated(edges);
    uint32_t numberOfPathNodes = firstPathNode + static_cast<uint32_t>(std::ranges::count_if(pathNodeOf, [](uint32_t pathNode) {
        return !!pathNode;
    }));
    PathGraph pathGraph(numberOfPathNodes, edges.span(), reversed(edges.span()).span());
    Vector<uint32_t> childOf = findDominatingChildrenOfRoot(pathGraph);

    Regions result;
    result.regions.grow(numberOfOrigins);
    Vector<uint32_t> regionOfChild(FillWith { }, numberOfPathNodes, UINT32_MAX);
    auto regionOf = [&](uint32_t pathNode) {
        uint32_t child = childOf[pathNode];
        if (child <= numberOfOrigins)
            return child - 1;
        if (regionOfChild[child] == UINT32_MAX) {
            regionOfChild[child] = result.regions.size();
            result.regions.append(Region { });
        }
        return regionOfChild[child];
    };

    for (uint32_t node = 0; node < graph.numberOfNodes(); ++node) {
        if (!graph.valueOf(FlowNodeID(node)).hasUnknownObject())
            continue;
        ++result.nodesWithUnknownObject;
        result.accessesThroughUnknownObject += accesses[node];
        if (!pathNodeOf[node]) {
            result.accessesWithoutOrigin += accesses[node];
            continue;
        }
        Region& region = result.regions[regionOf(pathNodeOf[node])];
        ++region.nodes;
        region.accesses += accesses[node];
        if (childOf[pathNodeOf[node]] == pathNodeOf[node])
            region.top = FlowNodeID(node);
    }
    for (auto [from, to] : edges) {
        if (from && regionOf(from) != regionOf(to))
            result.ascendingEdges.append({ regionOf(from), regionOf(to) });
    }
    sortAndRemoveRepeated(result.ascendingEdges);
    return result;
}

// Every region that a walk from the start along the edges finds, the start included.
static Vector<uint32_t> collectReachedRegions(const Adjacency& edges, uint32_t start)
{
    BitVector isVisited(edges.numberOfNodes());
    Vector<uint32_t> reached { start };
    isVisited.quickSet(start);
    for (size_t next = 0; next < reached.size(); ++next) {
        for (uint32_t target : edges.targetsOf(reached[next])) {
            if (!isVisited.quickSet(target))
                reached.append(target);
        }
    }
    return reached;
}

static void printWitness(PrintStream& out, const Witness& witness)
{
    if (!hasFunction(witness)) {
        out.print("n"_s, witness.value);
        return;
    }
    out.print("f"_s, witness.function);
    if (witness.value != Wire::none)
        out.print(":v"_s, witness.value);
}

static void printOperation(PrintStream& out, const Program& program, const Witness& witness)
{
    if (witness.function >= program.functions.size())
        return;
    const Function& function = program.at(FunctionID(witness.function));
    IRPrinter printer(out, program);
    out.print(" | "_s);
    printer.print(function.name);
    out.print(" in "_s);
    printer.print(program.at(function.module).displayPath);
    if (witness.value == Wire::none)
        return;

    // The types in the text of one operation of a large program fill pages.
    StringPrintStream operation;
    printValue(operation, program, function, ValueID(witness.value));
    String text = operation.toString();
    out.print(" | "_s, StringView(text).left(maxPrintedOperationLength));
}

static void printOrigin(PrintStream& out, const FlowSolvedProgram& solved, const OriginCounts& origin, const Region& alone, const Region* reached)
{
    out.print("origin "_s);
    printWitness(out, origin.witness);
    out.print(" alone="_s, alone.accesses, " nodesAlone="_s, alone.nodes);
    if (reached)
        out.print(" accesses="_s, reached->accesses, " nodes="_s, reached->nodes);
    out.print(" entries="_s, origin.entries, " into "_s);
    solved.graph->dumpNameOf(out, origin.firstNode);
    if (hasFunction(origin.witness))
        printOperation(out, solved.program, origin.witness);
    else {
        out.print(" | "_s);
        solved.graph->dumpNameOf(out, FlowNodeID(origin.witness.value));
    }
    out.println();
}

static Vector<uint32_t> sortByWeight(std::span<const Region> regions, uint32_t first, uint32_t end)
{
    Vector<uint32_t> order;
    for (uint32_t index = first; index < end; ++index)
        order.append(index);
    std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
        return std::tuple { regions[b].accesses, regions[b].nodes, a } < std::tuple { regions[a].accesses, regions[a].nodes, b };
    });
    return order;
}

// The heaviest first, by what goes with the origin alone. What an origin reaches together with others is added up for the first of them only: it takes a walk for each.
static void printOrigins(PrintStream& out, const FlowSolvedProgram& solved, std::span<const OriginCounts> origins, std::span<const Region> regions, const Adjacency& regionEdges)
{
    size_t numberOfPrintedOrigins = 0;
    Vector<uint32_t> order = sortByWeight(regions, 0, origins.size());
    for (uint32_t origin : order.span().first(std::min(maxPrintedOrigins, order.size()))) {
        if (numberOfPrintedOrigins++ >= maxOriginsWithReach) {
            printOrigin(out, solved, origins[origin], regions[origin], nullptr);
            continue;
        }
        Region reached;
        for (uint32_t region : collectReachedRegions(regionEdges, origin)) {
            reached.accesses += regions[region].accesses;
            reached.nodes += regions[region].nodes;
        }
        printOrigin(out, solved, origins[origin], regions[origin], &reached);
    }
}

// A meeting point is freed only when every origin that reaches it is gone.
static void printMeetingPoints(PrintStream& out, const FlowGraph& graph, std::span<const OriginCounts> origins, const Regions& computed)
{
    std::span<const Region> regions = computed.regions.span();
    Adjacency reversedRegionEdges(regions.size(), reversed(computed.ascendingEdges.span()).span());
    Vector<uint32_t> order = sortByWeight(regions, origins.size(), regions.size());
    for (uint32_t meetingPoint : order.span().first(std::min(maxPrintedMeetingPoints, order.size()))) {
        Vector<uint32_t> reaching = collectReachedRegions(reversedRegionEdges, meetingPoint);
        reaching.removeAllMatching([&](uint32_t region) {
            return region >= origins.size();
        });
        std::ranges::sort(reaching);

        out.print("meeting-point accesses="_s, regions[meetingPoint].accesses, " nodes="_s, regions[meetingPoint].nodes, " origins="_s, reaching.size(), " at "_s);
        graph.dumpNameOf(out, regions[meetingPoint].top);
        out.print(" |"_s);
        for (uint32_t origin : reaching.span().first(std::min(maxPrintedOriginsOfMeetingPoint, reaching.size()))) {
            out.print(" "_s);
            printWitness(out, origins[origin].witness);
        }
        out.println();
    }
}

static void printOriginsInScope(PrintStream& out, const FlowSolvedProgram& solved, OriginScope scope)
{
    const FlowGraph& graph = *solved.graph;
    Origins collected = collectOrigins(solved, scope);
    std::span<const OriginCounts> origins = collected.origins.span();
    Vector<uint32_t> accesses = countAccessesByReceiver(graph);
    Regions computed = computeRegions(graph, collected.ascendingPaths.span(), collected.entriesByOrigin.span(), origins.size(), accesses.span());

    out.println("nodes="_s, graph.numberOfNodes(), " withUnknownObject="_s, computed.nodesWithUnknownObject, " accessesThroughUnknownObject="_s, computed.accessesThroughUnknownObject, " withoutOrigin="_s, computed.accessesWithoutOrigin, " origins="_s, origins.size(), " meetingPoints="_s, computed.regions.size() - origins.size());
    printOrigins(out, solved, origins, computed.regions.span(), Adjacency(computed.regions.size(), computed.ascendingEdges.span()));
    printMeetingPoints(out, graph, origins, computed);
}

void printUnknownObjectOrigins(PrintStream& out, const FlowSolvedProgram& solved)
{
    printOriginsInScope(out, solved, OriginScope::Program);
}

void printUnknownObjectOriginsOfApplication(PrintStream& out, const FlowSolvedProgram& solved)
{
    printOriginsInScope(out, solved, OriginScope::Application);
}

namespace {

// What the instances of one function say together about the receiver of one access.
struct ReceiverOfSite {
    bool isReached { false };
    bool holdsUnknownObject { false };
    bool holdsUnknownObjectOutsideGeneric { false };
    size_t mostObjectsOutsideGeneric { 0 };
};

struct SiteCounts {
    uint64_t reached { 0 };
    uint64_t throughUnknownObject { 0 };
    uint64_t throughUnknownObjectOutsideGeneric { 0 };
    std::array<uint64_t, 4> bySizeOfSet { };

    void add(const ReceiverOfSite& receiver)
    {
        if (!receiver.isReached)
            return;
        ++reached;
        throughUnknownObject += receiver.holdsUnknownObject;
        throughUnknownObjectOutsideGeneric += receiver.holdsUnknownObjectOutsideGeneric;
        if (receiver.holdsUnknownObjectOutsideGeneric)
            return;
        size_t size = receiver.mostObjectsOutsideGeneric;
        ++bySizeOfSet[size <= 1 ? 0 : size <= 4 ? 1 : size <= 16 ? 2 : 3];
    }

    void dump(PrintStream& out) const
    {
        out.print("reached="_s, reached, " throughUnknownObject="_s, throughUnknownObject, " outsideGeneric="_s, throughUnknownObjectOutsideGeneric);
        out.print(" others by objects at the receiver: upTo1="_s, bySizeOfSet[0], " upTo4="_s, bySizeOfSet[1], " upTo16="_s, bySizeOfSet[2], " more="_s, bySizeOfSet[3]);
    }
};

} // anonymous namespace

static void addSitesOf(const FlowGraph& graph, FunctionID function, SiteCounts& counts)
{
    Vector<ReceiverOfSite> receivers;
    graph.forEachInstanceOf(function, [&](FlowInstanceID instance) {
        if (graph.at(instance).state == FlowInstanceState::Created)
            return;
        bool isGeneric = graph.at(instance).context == FlowContext::Generic;
        size_t site = 0;
        auto note = [&](FlowLocalID local) {
            if (receivers.size() <= site)
                receivers.grow(site + 1);
            ReceiverOfSite& receiver = receivers[site++];
            if (!local)
                return;
            const FlowValue& value = graph.valueOf(graph.nodeOf(instance, local));
            if (value.isEmpty())
                return;
            receiver.isReached = true;
            receiver.holdsUnknownObject |= value.hasUnknownObject();
            if (isGeneric)
                return;
            receiver.holdsUnknownObjectOutsideGeneric |= value.hasUnknownObject();
            receiver.mostObjectsOutsideGeneric = std::max(receiver.mostObjectsOutsideGeneric, value.objects.size());
        };
        const FlowStatements& statements = graph.summaryOf(instance).statements;
        for (const FlowFieldAccess& access : statements.fieldAccesses)
            note(access.receiver);
        for (const FlowPropertyAccess& access : statements.propertyAccesses)
            note(access.receiver);
    });
    for (const ReceiverOfSite& receiver : receivers)
        counts.add(receiver);
}

// An access of the text is counted once, however many instances its function has: more instances do not make the numbers larger.
void printAccessSites(PrintStream& out, const FlowSolvedProgram& solved)
{
    const FlowGraph& graph = *solved.graph;
    SiteCounts ofApplication;
    SiteCounts ofLibrary;
    for (uint32_t index = 0; index < solved.program.functions.size(); ++index) {
        FunctionID function(index);
        addSitesOf(graph, function, isWrittenByApplication(solved.program, function) ? ofApplication : ofLibrary);
    }

    std::array<uint64_t, 3> instancesByContext { };
    uint64_t instancesAtCallSites = 0;
    for (uint32_t index = 0; index < graph.numberOfInstances(); ++index) {
        const FlowInstance& instance = graph.at(FlowInstanceID(index));
        if (instance.state == FlowInstanceState::Created)
            continue;
        ++instancesByContext[static_cast<size_t>(instance.context)];
        instancesAtCallSites += instance.isCallSiteInstance();
    }
    out.println("application "_s, ofApplication);
    out.println("library "_s, ofLibrary);
    out.println("instances proven="_s, instancesByContext[static_cast<size_t>(FlowContext::Proven)], " provenUnderGeneric="_s, instancesByContext[static_cast<size_t>(FlowContext::ProvenUnderGeneric)], " generic="_s, instancesByContext[static_cast<size_t>(FlowContext::Generic)], " atCallSites="_s, instancesAtCallSites, " nodes="_s, graph.numberOfNodes(), " objects="_s, solved.objects->size());
}

static void printMutationsOf(PrintStream& out, const FlowSolvedProgram& solved, FlowObjectID object)
{
    const FlowRecords& records = solved.records;
    out.print("  object "_s);
    solved.objects->dump(out, object);
    for (ObjectMutation mutation : records.possibleMutations(object)) {
        out.print(" "_s, nameOf(mutation));
        if (Witness witness = records.witnessOf(object, mutation); hasFunction(witness)) {
            out.print("@"_s);
            printWitness(out, witness);
        }
    }
    if (FlowNodeID link = solved.graph->find(object, FlowOpenPart::PrototypeLink)) {
        StringPrintStream prototypes;
        solved.graph->values().dump(prototypes, solved.graph->valueOf(link));
        out.print(" prototypes="_s, StringView(prototypes.toString()).left(120));
    }
    out.println();
}

// What a whole program is too large to show: each instance of one function with the values of its nodes, a long set cut short, the origin of the unknown object in each,
// and what happens to the closures of the function, to the objects it allocates and to the objects in its small sets.
void printFunctionInSolvedProgram(PrintStream& out, const FlowSolvedProgram& solved, FunctionID function)
{
    constexpr unsigned maxPrintedValueLength = 300;
    constexpr unsigned maxObjectsOfSmallSet = 4;
    const FlowGraph& graph = *solved.graph;
    FlowObjectSet objectsInSmallSets;
    graph.forEachInstanceOf(function, [&](FlowInstanceID instance) {
        if (graph.at(instance).state == FlowInstanceState::Created)
            return;
        out.println("instance i"_s, instance.index(), " "_s, nameOf(graph.at(instance).context), graph.at(instance).isCallSiteInstance() ? " at a call site"_s : ""_s);
        for (uint32_t local = 0; local < graph.summaryOf(instance).numberOfLocals; ++local) {
            FlowNodeID node = graph.nodeOf(instance, FlowLocalID(local));
            if (graph.valueOf(node).isEmpty())
                continue;
            if (graph.valueOf(node).objects.size() <= maxObjectsOfSmallSet)
                objectsInSmallSets.unite(graph.valueOf(node).objects);
            StringPrintStream value;
            graph.values().dump(value, graph.valueOf(node));
            String text = value.toString();
            out.print("  "_s);
            graph.dumpNameOf(out, node);
            out.print(" = "_s, StringView(text).left(maxPrintedValueLength));
            if (Witness witness = solved.records.unknownObjectWitness(node); hasFunction(witness)) {
                out.print(" unknown from "_s);
                printWitness(out, witness);
            }
            out.println();
        }
    });
    for (uint32_t index = 0; index < solved.objects->size(); ++index) {
        FlowObjectID object(index);
        bool isClosureOfFunction = solved.objects->kindOf(object) == FlowObjectKind::Closure && solved.objects->at(object).function == function;
        if (solved.objects->allocatingFunctionOf(object) == function || isClosureOfFunction || objectsInSmallSets.contains(object))
            printMutationsOf(out, solved, object);
    }
}

// One line for each mutation and the operation that the records name for it, with the number of objects: the heaviest first.
void printObjectMutationsByWitness(PrintStream& out, const FlowSolvedProgram& solved)
{
    struct MutationCount {
        ObjectMutation mutation;
        Witness witness;
        uint32_t objects { 0 };
        uint32_t allocations { 0 };
        uint32_t closures { 0 };
    };

    const FlowRecords& records = solved.records;
    Vector<MutationCount> counts;
    Vector<MutationCount> totals;
    for (uint32_t index = 0; index < solved.objects->size(); ++index) {
        FlowObjectID object(index);
        FlowObjectKind kind = solved.objects->kindOf(object);
        for (ObjectMutation mutation : records.possibleMutations(object)) {
            counts.append(MutationCount { mutation, records.witnessOf(object, mutation), 1, kind == FlowObjectKind::Allocation, kind == FlowObjectKind::Closure });
            size_t position = totals.findIf([&](const MutationCount& total) {
                return total.mutation == mutation;
            });
            if (position == notFound) {
                position = totals.size();
                totals.append(MutationCount { mutation, noWitness });
            }
            totals[position].objects += 1;
            totals[position].allocations += kind == FlowObjectKind::Allocation;
            totals[position].closures += kind == FlowObjectKind::Closure;
        }
    }
    out.println("objects="_s, solved.objects->size());
    for (const MutationCount& total : totals)
        out.println("total "_s, nameOf(total.mutation), " objects="_s, total.objects, " allocations="_s, total.allocations, " closures="_s, total.closures);
    auto keyOf = [](const MutationCount& count) {
        return std::tuple { count.mutation, witnessKey(count.witness) };
    };
    std::ranges::sort(counts, { }, keyOf);

    size_t numberOfCounts = 0;
    for (const MutationCount& count : counts) {
        if (numberOfCounts && keyOf(counts[numberOfCounts - 1]) == keyOf(count)) {
            MutationCount& merged = counts[numberOfCounts - 1];
            merged.objects += count.objects;
            merged.allocations += count.allocations;
            merged.closures += count.closures;
            continue;
        }
        counts[numberOfCounts++] = count;
    }
    counts.shrink(numberOfCounts);
    std::ranges::sort(counts, [&](const MutationCount& a, const MutationCount& b) {
        return std::tuple { b.objects, keyOf(a) } < std::tuple { a.objects, keyOf(b) };
    });

    for (const MutationCount& count : counts.span().first(std::min(maxPrintedOrigins, counts.size()))) {
        out.print("mutation "_s, nameOf(count.mutation), " objects="_s, count.objects, " allocations="_s, count.allocations, " closures="_s, count.closures, " by "_s);
        if (hasFunction(count.witness))
            printWitness(out, count.witness);
        else
            out.print("nobody"_s);
        printOperation(out, solved.program, count.witness);
        out.println();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
