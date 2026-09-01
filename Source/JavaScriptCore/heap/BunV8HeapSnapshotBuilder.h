#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "HeapAnalyzer.h"
#include <atomic>
#include <functional>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/OverflowPolicy.h>
#include <wtf/SegmentedVector.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {

class JSCell;
class HeapProfiler;
class HeapSnapshot;

class JS_EXPORT_PRIVATE BunV8HeapSnapshotBuilder final : public HeapAnalyzer {
    WTF_MAKE_TZONE_ALLOCATED(BunV8HeapSnapshotBuilder);

public:
    BunV8HeapSnapshotBuilder(HeapProfiler&);
    ~BunV8HeapSnapshotBuilder() final;

    void analyzeNode(JSCell*) final;
    void analyzeEdge(JSCell* from, JSCell* to, RootMarkReason) final;
    void analyzePropertyNameEdge(JSCell* from, JSCell* to, UniquedStringImpl* propertyName) final;
    void analyzeVariableNameEdge(JSCell* from, JSCell* to, UniquedStringImpl* variableName) final;
    void analyzeIndexEdge(JSCell* from, JSCell* to, uint32_t index) final;
    void setOpaqueRootReachabilityReasonForCell(JSCell*, ASCIILiteral) final;
    void setWrappedObjectForCell(JSCell*, void*) final;
    void setLabelForCell(JSCell*, const String&) final;

    // V8 snapshot generation
    void buildSnapshot();
    String json();
    Vector<uint8_t> jsonBytes();

    // True when the snapshot was too large to serialize (edge storage or the
    // JSON output exceeded a container limit). json() returns a null String
    // and jsonBytes() returns an empty Vector in that case, so callers can
    // raise a catchable error instead of crashing the process.
    bool hasOverflowed() const { return m_overflowed.load(std::memory_order_relaxed); }

private:
    static constexpr unsigned kRootNodeIndex = 0;
    static constexpr unsigned kGcRootsNodeIndex = 1;
    // Synthetic-node IDs occupy 1..kSyntheticIdCount; real-object IDs are
    // identifier + kSyntheticIdCount so the spaces never collide.
    static constexpr unsigned kSyntheticIdCount = 2;

    String generateV8HeapSnapshot();
    Vector<uint8_t> generateV8HeapSnapshotBytes();
    unsigned analyzeNodeInternal(JSCell*);
    void appendSyntheticRootEdges();

    // One of these exists per heap cell, so keep it small: a 100M-cell heap
    // allocates 100M of them.
    struct Node {
        JSCell* cell { nullptr };
        String name {};
        size_t selfSize { 0 };
        unsigned id { 0 };
        unsigned typeIndex { 0 };
        unsigned edgesCount { 0 };
    };
    struct Edge {
        unsigned fromNodeId { 0 };
        unsigned toNodeId { 0 };
        unsigned typeIndex { 0 };
        unsigned index { 0 };
        String name {};
    };

    enum class V8NodeType : uint8_t {
        Hidden,
        Array,
        String,
        Object,
        Code,
        Closure,
        RegExp,
        Number,
        Native,
        Synthetic,
        ConcatenatedString,
        SlicedString,
        Symbol,
        BigInt,
        ObjectShape,
        Count
    };

    enum class V8EdgeType : uint8_t {
        Context,
        Element,
        Property,
        Internal,
        Hidden,
        Shortcut,
        Weak,
        Count
    };

    HeapProfiler& m_profiler;
    Lock m_buildingNodeMutex;
    Lock m_buildingEdgeMutex;

    // Node and edge storage. Nodes live in a SegmentedVector: a contiguous
    // Vector caps a single allocation at ~2GiB and CRASH()es past it, which a
    // large heap (tens of millions of cells) hits. Segments also keep element
    // addresses stable for the atomic edge-count updates during marking.
    SegmentedVector<Node, 1024> m_nodes;
    Vector<Edge> m_edges;
    std::atomic<bool> m_overflowed { false };
    Lock m_cellToNodeIdMutex;
    HashMap<JSCell*, unsigned> m_cellToNodeId;
    Vector<unsigned> m_globalObjectNodeIndices;
    std::unique_ptr<HeapSnapshot> m_snapshot;
    HeapSnapshot* m_previousSnapshot { nullptr };

    // TODO: make this not so inefficient
    Vector<String> m_strings;
    HashMap<size_t, unsigned> m_stringsLookupTable;
    // Type mapping
    Vector<String> m_nodeTypeNames;
    Vector<String> m_edgeTypeNames;
    HashMap<String, unsigned> m_nodeTypeMap;
    HashMap<String, unsigned> m_edgeTypeMap;

    // Cell labels
    HashMap<JSCell*, String> m_cellLabels;

    // Helper methods
    unsigned getOrCreateNodeId(JSCell*);
    unsigned getNodeTypeIndex(JSCell*);
    unsigned getEdgeTypeIndex(RootMarkReason);
    unsigned getEdgeTypeIndex(const String& type);
    unsigned addString(const String&);
    void appendEdge(Edge&&);
    void initializeTypeNames();
    String getDetailedNodeType(JSCell*);
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
