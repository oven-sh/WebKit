#include "config.h"

#if USE(BUN_JSC_ADDITIONS)

#include "BunV8HeapSnapshotBuilder.h"

#include "DeferGC.h"
#include "DeferGCInlines.h"
#include "Heap.h"
#include "HeapProfiler.h"
#include "HeapSnapshot.h"
#include "JSCInlines.h"
#include "JSCast.h"
#include "JSLock.h"
#include "JSPromise.h"
#include "JSType.h"
#include "PreventCollectionScope.h"
#include "SourceCode.h"
#include <wtf/HexNumber.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringBuilder.h>

#include "DateInstance.h"
#include "HeapSnapshotBuilder.h"
#include "RegExpObject.h"
#include "HeapSnapshot.h"

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BunV8HeapSnapshotBuilder);

BunV8HeapSnapshotBuilder::BunV8HeapSnapshotBuilder(HeapProfiler& profiler)
    : m_profiler(profiler)
    , m_previousSnapshot(profiler.mostRecentSnapshot())
{
    m_snapshot = makeUnique<HeapSnapshot>(m_previousSnapshot);

    initializeTypeNames();

    // V8's snapshot has a synthetic "(root)" with a "(GC roots)" child; DevTools'
    // calculateDistances BFS uses shortcut edges from root as user roots
    // (distance=1) and assigns baseSystemDistance to anything reachable only
    // through (GC roots). Synthetic IDs occupy 1..kSyntheticIdCount.
    m_nodes.append({
        .id = 1,
        .typeIndex = static_cast<unsigned>(V8NodeType::Synthetic),
        .name = ""_s,
        .selfSize = 0,
        .edges = {},
        .traceLocation = std::nullopt,
        .parentNodeId = std::nullopt,
    });
    m_nodes.append({
        .id = 2,
        .typeIndex = static_cast<unsigned>(V8NodeType::Synthetic),
        .name = "(GC roots)"_s,
        .selfSize = 0,
        .edges = {},
        .traceLocation = std::nullopt,
        .parentNodeId = kRootNodeIndex,
    });

    // Add empty string as first string (index 0)
    m_strings.append(emptyString());
}

void BunV8HeapSnapshotBuilder::initializeTypeNames()
{
    // Initialize node type names
    m_nodeTypeNames.resize(static_cast<unsigned>(V8NodeType::Count));
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Hidden)] = "hidden"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Array)] = "array"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::String)] = "string"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Object)] = "object"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Code)] = "code"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Closure)] = "closure"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::RegExp)] = "regexp"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Number)] = "number"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Native)] = "native"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Synthetic)] = "synthetic"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::ConcatenatedString)] = "concatenated string"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::SlicedString)] = "sliced string"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::Symbol)] = "symbol"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::BigInt)] = "bigint"_s;
    m_nodeTypeNames[static_cast<unsigned>(V8NodeType::ObjectShape)] = "object shape"_s;

    // Build node type map
    for (unsigned i = 0; i < m_nodeTypeNames.size(); ++i)
        m_nodeTypeMap.set(m_nodeTypeNames[i], i);

    // Initialize edge type names
    m_edgeTypeNames.resize(static_cast<unsigned>(V8EdgeType::Count));
    m_edgeTypeNames[static_cast<unsigned>(V8EdgeType::Context)] = "context"_s;
    m_edgeTypeNames[static_cast<unsigned>(V8EdgeType::Element)] = "element"_s;
    m_edgeTypeNames[static_cast<unsigned>(V8EdgeType::Property)] = "property"_s;
    m_edgeTypeNames[static_cast<unsigned>(V8EdgeType::Internal)] = "internal"_s;
    m_edgeTypeNames[static_cast<unsigned>(V8EdgeType::Hidden)] = "hidden"_s;
    m_edgeTypeNames[static_cast<unsigned>(V8EdgeType::Shortcut)] = "shortcut"_s;
    m_edgeTypeNames[static_cast<unsigned>(V8EdgeType::Weak)] = "weak"_s;

    // Build edge type map
    for (unsigned i = 0; i < m_edgeTypeNames.size(); ++i)
        m_edgeTypeMap.set(m_edgeTypeNames[i], i);
}

BunV8HeapSnapshotBuilder::~BunV8HeapSnapshotBuilder() = default;

String BunV8HeapSnapshotBuilder::json()
{
    // SharedGC (T9): main-VM-only — all m_profiler.vm() uses in this file:
    // profiler owned by the main VM, snapshotting runs on its mutator under
    // the API lock (see HeapProfiler::vm(), HeapProfiler.h).
    VM& vm = m_profiler.vm();
    PreventCollectionScope preventCollectionScope(vm.heap);
    {
        ASSERT(!m_profiler.activeHeapAnalyzer());
        m_profiler.setActiveHeapAnalyzer(this);
        vm.heap.collectNow(Sync, CollectionScope::Full);
        m_profiler.setActiveHeapAnalyzer(nullptr);
    }

    m_snapshot->finalize();
    m_profiler.appendSnapshot(WTF::move(m_snapshot));

    JSLockHolder lock { vm };
    DeferGC deferGC(vm);
    return generateV8HeapSnapshot();
}

Vector<uint8_t> BunV8HeapSnapshotBuilder::jsonBytes()
{
    VM& vm = m_profiler.vm();
    PreventCollectionScope preventCollectionScope(vm.heap);
    {
        ASSERT(!m_profiler.activeHeapAnalyzer());
        m_profiler.setActiveHeapAnalyzer(this);
        vm.heap.collectNow(Sync, CollectionScope::Full);
        m_profiler.setActiveHeapAnalyzer(nullptr);
    }

    m_snapshot->finalize();
    m_profiler.appendSnapshot(WTF::move(m_snapshot));

    JSLockHolder lock { vm };
    DeferGC deferGC(vm);
    return generateV8HeapSnapshotBytes();
}

void BunV8HeapSnapshotBuilder::analyzeNode(JSCell* cell)
{
    if (!cell)
        return;
    getOrCreateNodeId(cell);
}

unsigned BunV8HeapSnapshotBuilder::analyzeNodeInternal(JSCell* cell)
{
    Locker locker { m_buildingNodeMutex };
    auto typeIndex = getNodeTypeIndex(cell);
    unsigned index = m_nodes.size();

    // Reuse the identifier from a prior snapshot if this cell survived;
    // Heap::removeDeadHeapSnapshotNodes() prunes dead entries on every GC so an
    // address reused for a new object misses here. Only new cells are appended
    // to m_snapshot — that's the contract for HeapSnapshot::appendNode().
    unsigned identifier;
    if (auto existing = m_previousSnapshot ? m_previousSnapshot->nodeForCell(cell) : std::nullopt)
        identifier = existing->identifier;
    else {
        identifier = HeapSnapshotBuilder::getNextObjectIdentifier();
        m_snapshot->appendNode(HeapSnapshotNode(cell, identifier));
    }

    m_nodes.append({
        .cell = cell,
        .id = identifier + kSyntheticIdCount,
        .typeIndex = typeIndex,
        .selfSize = cell->estimatedSizeInBytes(m_profiler.vm()),
        .edges = {},
        .traceLocation = getTraceLocation(cell),
        .parentNodeId = std::nullopt,
    });

    if (cell->type() == GlobalObjectType)
        m_globalObjectNodeIndices.append(index);

    return index;
}

void BunV8HeapSnapshotBuilder::appendSyntheticRootEdges()
{
    Edge rootToGcRoots {};
    rootToGcRoots.fromNodeId = kRootNodeIndex;
    rootToGcRoots.toNodeId = kGcRootsNodeIndex;
    rootToGcRoots.typeIndex = static_cast<unsigned>(V8EdgeType::Element);
    rootToGcRoots.index = 0;
    m_edges.append(WTF::move(rootToGcRoots));

    for (unsigned globalIndex : m_globalObjectNodeIndices) {
        Edge shortcut {};
        shortcut.fromNodeId = kRootNodeIndex;
        shortcut.toNodeId = globalIndex;
        shortcut.typeIndex = static_cast<unsigned>(V8EdgeType::Shortcut);
        m_edges.append(WTF::move(shortcut));
    }
}

void BunV8HeapSnapshotBuilder::analyzeEdge(JSCell* from, JSCell* to, RootMarkReason reason)
{
    ASSERT(to);

    // Avoid trivial edges.
    if (from == to)
        return;

    Locker locker { m_buildingEdgeMutex };
    Edge edge = {};
    edge.fromNodeId = from ? getOrCreateNodeId(from) : kGcRootsNodeIndex;
    edge.toNodeId = getOrCreateNodeId(to);

    // Validate node IDs
    ASSERT(edge.fromNodeId < m_nodes.size());
    ASSERT(edge.toNodeId < m_nodes.size());

    edge.typeIndex = getEdgeTypeIndex(reason);

    // Only track parent-child relationships for non-property and non-element edges
    switch (edge.typeIndex) {
    case static_cast<unsigned>(V8EdgeType::Element):
    case static_cast<unsigned>(V8EdgeType::Property):
    case static_cast<unsigned>(V8EdgeType::Context):
        break;
    default: {
        Locker locker { m_buildingNodeMutex };
        m_nodes[edge.toNodeId].parentNodeId = edge.fromNodeId;
        if (edge.typeIndex == static_cast<unsigned>(V8EdgeType::Hidden)) {
            edge.index = WTF::atomicExchangeAdd(&m_nodes[edge.fromNodeId].edgesCount, 1);
        }
        break;
    }
    }

    m_edges.append(WTF::move(edge));
}

void BunV8HeapSnapshotBuilder::analyzePropertyNameEdge(JSCell* from, JSCell* to, UniquedStringImpl* propertyName)
{
    if (!to || !propertyName)
        return;

    Locker locker { m_buildingEdgeMutex };
    Edge edge = {};
    edge.fromNodeId = getOrCreateNodeId(from);
    edge.toNodeId = getOrCreateNodeId(to);
    edge.typeIndex = static_cast<unsigned>(V8EdgeType::Property);
    edge.name = WTF::String(propertyName);
    m_edges.append(WTF::move(edge));
}

void BunV8HeapSnapshotBuilder::analyzeVariableNameEdge(JSCell* from, JSCell* to, UniquedStringImpl* variableName)
{
    if (!to || !variableName)
        return;

    Locker locker { m_buildingEdgeMutex };
    Edge edge = {};
    edge.fromNodeId = getOrCreateNodeId(from);
    edge.toNodeId = getOrCreateNodeId(to);
    edge.typeIndex = static_cast<unsigned>(V8EdgeType::Context);
    edge.name = String(variableName);

    m_edges.append(WTF::move(edge));
}

void BunV8HeapSnapshotBuilder::analyzeIndexEdge(JSCell* from, JSCell* to, uint32_t index)
{
    if (!to)
        return;

    Locker locker { m_buildingEdgeMutex };
    Edge edge = {};
    edge.fromNodeId = getOrCreateNodeId(from);
    edge.toNodeId = getOrCreateNodeId(to);
    edge.typeIndex = static_cast<unsigned>(V8EdgeType::Element);
    edge.index = index;

    m_edges.append(WTF::move(edge));
}

void BunV8HeapSnapshotBuilder::setOpaqueRootReachabilityReasonForCell(JSCell*, ASCIILiteral) {}
void BunV8HeapSnapshotBuilder::setWrappedObjectForCell(JSCell*, void*) {}

void BunV8HeapSnapshotBuilder::setLabelForCell(JSCell* cell, const String& label)
{
    if (!cell || label.isEmpty())
        return;

    Locker locker { m_buildingNodeMutex };
    m_cellLabels.set(cell, label);
}

unsigned BunV8HeapSnapshotBuilder::getOrCreateNodeId(JSCell* cell)
{
    if (!cell)
        return kRootNodeIndex;

    Locker locker { m_cellToNodeIdMutex };
    auto it = m_cellToNodeId.find(cell);
    if (it != m_cellToNodeId.end())
        return it->value;

    unsigned id = analyzeNodeInternal(cell);
    m_cellToNodeId.set(cell, id);
    return id;
}

unsigned BunV8HeapSnapshotBuilder::getNodeTypeIndex(JSCell* cell)
{
    if (!cell)
        return static_cast<unsigned>(V8NodeType::Synthetic);

    if (cell->isString()) {
        JSString* str = uncheckedDowncast<JSString>(cell);
        if (str->isSubstring())
            return static_cast<unsigned>(V8NodeType::SlicedString);
        if (str->isRope())
            return static_cast<unsigned>(V8NodeType::ConcatenatedString);
        return static_cast<unsigned>(V8NodeType::String);
    }

    if (cell->isCallable())
        return static_cast<unsigned>(V8NodeType::Closure);

    switch (cell->type()) {
    case JSType::StructureType: {
        return static_cast<unsigned>(V8NodeType::ObjectShape);
    }
    case JSType::NativeExecutableType:
    case JSType::ProgramExecutableType:
    case JSType::ModuleProgramExecutableType:
    case JSType::EvalExecutableType:
    case JSType::FunctionExecutableType:
    case JSType::UnlinkedFunctionExecutableType:
    case JSType::UnlinkedProgramCodeBlockType:
    case JSType::UnlinkedModuleProgramCodeBlockType:
    case JSType::UnlinkedEvalCodeBlockType:
    case JSType::UnlinkedFunctionCodeBlockType:
    case JSType::CodeBlockType:
    case JSType::GetterSetterType:
    case JSType::CustomGetterSetterType:
    case JSType::APIValueWrapperType:
    case JSType::JSSourceCodeType: {
        return static_cast<unsigned>(V8NodeType::Code);
    }
    case JSType::HeapBigIntType:
        return static_cast<unsigned>(V8NodeType::BigInt);
    case JSType::SymbolType:
        return static_cast<unsigned>(V8NodeType::Symbol);
    case JSType::RegExpObjectType:
        return static_cast<unsigned>(V8NodeType::RegExp);
    // V8 reserves kArray for internal FixedArray backing stores; user-level
    // JSArrays are kObject. DevTools' calculateShallowSizes() zeroes kArray
    // selfSize and reattributes it to the owner, so mis-typing JS arrays here
    // makes every Array show 0 shallow size.
    case JSType::ArrayType:
    case JSType::DerivedArrayType:
    // V8 maps scopes/contexts and Wasm Module/Instance to kObject, not kCode.
    case JSType::StrictEvalActivationType:
    case JSType::ShadowRealmType:
    case JSType::WebAssemblyModuleType:
    case JSType::WebAssemblyInstanceType:
    case JSType::WithScopeType:
    case JSType::GlobalLexicalEnvironmentType:
    case JSType::LexicalEnvironmentType:
    case JSType::ModuleEnvironmentType:
        return static_cast<unsigned>(V8NodeType::Object);

    default: {
        if (static_cast<unsigned>(cell->type()) > static_cast<unsigned>(JSType::LastJSCObjectType)) {
            return static_cast<unsigned>(V8NodeType::Native);
        }

        if (cell->isObject()) {
            return static_cast<unsigned>(V8NodeType::Object);
        }
    }
    case JSType::CellType:
        return static_cast<unsigned>(V8NodeType::Hidden);
    }

    return static_cast<unsigned>(V8NodeType::Native);
}

String BunV8HeapSnapshotBuilder::getDetailedNodeType(JSCell* cell)
{
    if (!cell)
        return "(root)"_s;

    // First check if we have a custom label for this cell
    auto labelIt = m_cellLabels.find(cell);
    if (labelIt != m_cellLabels.end())
        return labelIt->value;

    switch (cell->type()) {
    case JSType::StringType: {
        auto* string = uncheckedDowncast<JSString>(cell);
        static constexpr unsigned heapSnapshotStringLimit = 1024;
        // V8 never resolves cons strings for the node name. We do, because
        // seeing the content is useful when debugging — but only when the
        // resolve is cheap. JSString::length() is O(1) on ropes, so gate on
        // that to avoid flattening huge ropes just to take 1024 chars.
        static constexpr unsigned heapSnapshotRopeResolveLimit = 16384;
        if (string->isRope() && string->length() > heapSnapshotRopeResolveLimit)
            return "(concatenated string)"_s;
        auto value = string->tryGetValue(true);
        if (!value->isEmpty()) {
            if (value->length() > heapSnapshotStringLimit)
                return value->left(heapSnapshotStringLimit);
            return value;
        }
        break;
    }
    case JSType::JSDateType: {
        return "Date"_s;
        break;
    }
    case JSType::RegExpObjectType: {
        auto* object = uncheckedDowncast<RegExpObject>(cell);
        auto* regExp = object->regExp();
        if (!regExp)
            return "RegExp"_s;

        auto source = regExp->toSourceString();
        if (!source.isEmpty()) {
            return source;
        }
        break;
    }
    case JSType::SymbolType: {
        auto* symbol = uncheckedDowncast<Symbol>(cell);
        // Avoid Symbol::description(VM&), which lazily allocates a JSString — heap
        // snapshot traversal must not allocate. Read the SymbolImpl directly.
        auto& uid = symbol->uid();
        if (!uid.isNullSymbol() && !uid.isEmpty()) {
            return makeString("Symbol("_s, StringView(uid), ')');
        }
        break;
    }
    // V8 names contexts "system / Context ..."; the "system / " prefix is what
    // DevTools keys on to grey these out and exclude them from user-object stats
    // while still showing them in retainer chains.
    case JSType::StrictEvalActivationType:
    case JSType::WithScopeType:
    case JSType::GlobalLexicalEnvironmentType:
    case JSType::LexicalEnvironmentType:
    case JSType::ModuleEnvironmentType:
        return makeString("system / "_s, cell->className());
    default: {
        break;
    }
    }

    // Get the base class name
    String name = cell->className();

    if (cell->isObject() && name == JSObject::info()->className) {

        // Skip calculating a class name if this object has a `constructor` own property.
        // These cases are typically F.prototype objects and we want to treat these as
        // "Object" in snapshots and not get the name of the prototype's parent.
        JSObject* object = asObject(cell);
        if (JSGlobalObject* globalObject = object->realmMayBeNull()) {
            PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_profiler.vm());
            if (!object->getOwnPropertySlot(object, globalObject, m_profiler.vm().propertyNames->constructor, slot)) {
                name = JSObject::calculatedClassName(object);
            }
        }
    }

    auto* object = cell->getObject();

    if (object) {
        // For functions, try to get the display name
        if (JSFunction* function = dynamicDowncast<JSFunction>(cell)) {
            String displayName = function->nameWithoutGC(m_profiler.vm());
            if (!displayName.isEmpty())
                return displayName;
        }

        // For functions, try to get the display name
        if (InternalFunction* function = dynamicDowncast<InternalFunction>(cell)) {
            String displayName = function->name();
            if (!displayName.isEmpty())
                return displayName;
        }
    }

    return name;
}

unsigned BunV8HeapSnapshotBuilder::getEdgeTypeIndex(RootMarkReason reason)
{
    switch (reason) {
    case RootMarkReason::None:
        return static_cast<unsigned>(V8EdgeType::Internal);

    // Weak references
    case RootMarkReason::WeakMapSpace:
    case RootMarkReason::WeakSets:
        return static_cast<unsigned>(V8EdgeType::Weak);

    // V8 reserves kContextVariable strictly for closure-captured locals with a
    // variable name. GC root edges use kInternal/kElement; using kContextVariable
    // here makes DevTools render conservative-scan/strong-handle roots as if a
    // closure were retaining the object.
    case RootMarkReason::VMExceptions:
    case RootMarkReason::ExecutableToCodeBlockEdges:
    case RootMarkReason::JITStubRoutines:
    case RootMarkReason::JITWorkList:
    case RootMarkReason::StrongReferences:
    case RootMarkReason::CodeBlocks:
    case RootMarkReason::MarkListSet:
    case RootMarkReason::StrongHandles:
    case RootMarkReason::DOMGCOutput:
    case RootMarkReason::Output:
    case RootMarkReason::ConservativeScan:
    case RootMarkReason::ExternalRememberedSet:
        return static_cast<unsigned>(V8EdgeType::Internal);

    case RootMarkReason::Debugger:
        return static_cast<unsigned>(V8EdgeType::Hidden);

    default:
        return static_cast<unsigned>(V8EdgeType::Internal);
    }
}

unsigned BunV8HeapSnapshotBuilder::getEdgeTypeIndex(const String& type)
{
    auto it = m_edgeTypeMap.find(type);
    if (it != m_edgeTypeMap.end())
        return it->value;
    return static_cast<unsigned>(V8EdgeType::Internal);
}

unsigned BunV8HeapSnapshotBuilder::addString(const String& str)
{
    // Never return 0 for non-empty strings
    if (str.isEmpty())
        return 0;

    // V8 truncates strings to --heap-snapshot-string-limit (default 1024) with no
    // ellipsis suffix and dedupes on the truncated content.
    static constexpr unsigned heapSnapshotStringLimit = 1024;
    String key = str.length() > heapSnapshotStringLimit ? str.left(heapSnapshotStringLimit) : str;

    // Check if string already exists
    unsigned hash = key.hash();
    size_t hashKey = static_cast<size_t>(hash);
    // 32 bits: hash
    // 32 bits: length
    hashKey |= static_cast<size_t>(key.length()) << (sizeof(size_t) * 8 - 32);
    auto it = m_stringsLookupTable.find(hashKey);
    if (it != m_stringsLookupTable.end())
        return it->value;

    unsigned index = m_strings.size();
    m_strings.append(WTF::move(key));
    m_stringsLookupTable.set(hashKey, index);
    return index;
}

std::optional<BunV8HeapSnapshotBuilder::TraceLocation> BunV8HeapSnapshotBuilder::getTraceLocation(JSCell* cell)
{
    if (!cell || !cell->isCallable())
        return std::nullopt;

    JSFunction* function = dynamicDowncast<JSFunction>(cell);
    if (!function || !function->executable() || function->isHostFunction())
        return std::nullopt;

    auto* executable = function->jsExecutable();
    if (!executable)
        return std::nullopt;

    auto* provider = executable->source().provider();
    if (!provider)
        return std::nullopt;

    TraceLocation location;
    location.scriptId = provider->asID();
    location.scriptName = provider->sourceURL();
    if (location.scriptName.isEmpty())
        location.scriptName = String();

    location.line = executable->firstLine();
    location.column = executable->startColumn();
    return { location };
}

void BunV8HeapSnapshotBuilder::TraceLocation::sourcemap(VM&)
{
    if (scriptName.isEmpty()) {
        return;
    }
}

String BunV8HeapSnapshotBuilder::generateV8HeapSnapshot()
{
    // Extra pass #1: fill in the node names
    for (auto& node : m_nodes) {
        if (node.cell)
            node.name = getDetailedNodeType(node.cell);
        node.edgesCount = 0; // Reset edge counts for deduplication pass
    }

    appendSyntheticRootEdges();

    // Sort edges by fromNodeId to ensure they're grouped correctly
    std::sort(m_edges.begin(), m_edges.end(),
        [](const Edge& a, const Edge& b) {
            // First sort by fromNodeId
            if (a.fromNodeId != b.fromNodeId)
                return a.fromNodeId < b.fromNodeId;

            // Then by typeIndex
            if (a.typeIndex != b.typeIndex)
                return a.typeIndex < b.typeIndex;

            // Then by toNodeId
            if (a.toNodeId != b.toNodeId)
                return a.toNodeId < b.toNodeId;

            // For element/hidden edges, compare by index
            if (a.typeIndex == static_cast<unsigned>(V8EdgeType::Element) || a.typeIndex == static_cast<unsigned>(V8EdgeType::Hidden))
                return a.index < b.index;

            // For named edges, compare by name
            return WTF::codePointCompareLessThan(a.name, b.name);
        });

    // Deduplicate edges in-place and update edge counts
    if (!m_edges.isEmpty()) {
        size_t writeIndex = 1;
        m_nodes[m_edges[0].fromNodeId].edgesCount = 1;

        for (size_t readIndex = 1; readIndex < m_edges.size(); readIndex++) {
            const auto& prev = m_edges[writeIndex - 1];
            const auto& curr = m_edges[readIndex];

            // Check if this is a duplicate edge
            bool isDuplicate = prev.fromNodeId == curr.fromNodeId && prev.toNodeId == curr.toNodeId && prev.typeIndex == curr.typeIndex;

            if (isDuplicate) {
                if (prev.typeIndex == static_cast<unsigned>(V8EdgeType::Element) || prev.typeIndex == static_cast<unsigned>(V8EdgeType::Hidden)) {
                    isDuplicate = prev.index == curr.index;
                } else {
                    isDuplicate = prev.name == curr.name;
                }
            }

            if (!isDuplicate) {
                if (writeIndex != readIndex)
                    m_edges[writeIndex] = WTF::move(m_edges[readIndex]);
                m_nodes[curr.fromNodeId].edgesCount++;
                writeIndex++;
            }
        }

        m_edges.shrink(writeIndex);
    }

    StringBuilder json;
    json.append("{\"snapshot\":{\"meta\":{"_s);

    // Node fields
    json.append("\"node_fields\":[\"type\",\"name\",\"id\",\"self_size\",\"edge_count\",\"trace_node_id\",\"detachedness\"],"_s);

    const unsigned NODE_FIELD_COUNT = 7; // type, name, id, self_size, edge_count, trace_node_id, detachedness

    // Node types
    json.append("\"node_types\":[["_s);
    bool first = true;
    for (const auto& type : m_nodeTypeNames) {
        if (!first)
            json.append(',');
        first = false;
        json.appendQuotedJSONString(type);
    }
    json.append("],\"string\",\"number\",\"number\",\"number\",\"number\",\"number\"],"_s);

    // Edge fields
    json.append("\"edge_fields\":[\"type\",\"name_or_index\",\"to_node\"],"_s);

    // Edge types
    json.append("\"edge_types\":[["_s);
    first = true;
    for (const auto& type : m_edgeTypeNames) {
        if (!first)
            json.append(',');
        first = false;
        json.appendQuotedJSONString(type);
    }
    json.append("],\"string_or_number\",\"node\"],"_s);

    // Trace function fields
    json.append("\"trace_function_info_fields\":[\"function_id\",\"name\",\"script_name\",\"script_id\",\"line\",\"column\"],"_s);

    // Trace node fields
    json.append("\"trace_node_fields\":[\"id\",\"function_info_index\",\"count\",\"size\",\"children\"],"_s);

    // Sample fields
    json.append("\"sample_fields\":[\"timestamp_us\",\"last_assigned_id\"],"_s);

    // Location fields
    json.append("\"location_fields\":[\"object_index\",\"script_id\",\"line\",\"column\"]"_s);

    json.append("},"_s);

    // Count functions with trace info
    // unsigned traceFunctionCount = m_nodes.size();
    unsigned traceFunctionCount = 0;
    // Node and edge counts
    json.append("\"node_count\":"_s);
    json.append(String::number(m_nodes.size()));
    json.append(",\"edge_count\":"_s);
    json.append(String::number(m_edges.size()));
    json.append(",\"trace_function_count\":"_s);
    json.append(String::number(traceFunctionCount));
    json.append("},"_s);

    // Nodes array
    json.append("\"nodes\":["_s);
    for (unsigned i = 0; i < m_nodes.size(); ++i) {
        const auto& node = m_nodes[i];
        if (i)
            json.append(',');

        json.append(String::number(node.typeIndex));
        json.append(',');
        json.append(String::number(addString(node.name)));
        json.append(',');
        json.append(String::number(node.id));
        json.append(',');
        json.append(String::number(node.selfSize));
        json.append(',');
        json.append(String::number(node.edgesCount));
        json.append(',');
        json.append('0'); // trace_node_id
        json.append(",0"_s); // detachedness
    }
    json.append("],\n"_s);

    // Edges array
    json.append("\"edges\":["_s);
    for (unsigned i = 0; i < m_edges.size(); ++i) {
        const auto& edge = m_edges[i];

        // Validate node IDs
        ASSERT(edge.fromNodeId < m_nodes.size());
        ASSERT(edge.toNodeId < m_nodes.size());

        if (i)
            json.append(',');

        json.append(String::number(edge.typeIndex));
        json.append(',');

        switch (edge.typeIndex) {
            // Matches the following from V8:
            //   int edge_name_or_index = edge->type() == HeapGraphEdge::kElement ||
            //                        edge->type() == HeapGraphEdge::kHidden
            //                    ? edge->index()
            //                    : GetStringId(edge->name());
        case static_cast<unsigned>(V8EdgeType::Hidden):
        case static_cast<unsigned>(V8EdgeType::Element):
            json.append(String::number(edge.index));
            break;
        default:
            json.append(String::number(addString(edge.name)));
        }
        json.append(',');

        // Both fromNodeId and toNodeId need to be multiplied by field count
        json.append(String::number(edge.toNodeId * NODE_FIELD_COUNT));
    }
    json.append("],\n"_s);

    // V8's HeapSnapshotJSONSerializer emits these in the order
    // trace_function_infos, trace_tree, samples, locations. DevTools'
    // streaming loader assumes that order when trace_function_count > 0.
    json.append("\"trace_function_infos\":[],\n"_s);
    json.append("\"trace_tree\": [],\n"_s);
    json.append("\"samples\":[],\n"_s);
    json.append("\"locations\":[],\n"_s);

    // Strings table
    json.append("\"strings\":["_s);

    first = true;
    for (const auto& str : m_strings) {
        if (!first)
            json.append(',');
        first = false;
        json.appendQuotedJSONString(str);
    }
    json.append("]\n"_s);

    json.append("}\n"_s);

    return json.toString();
}

static void appendUTF8BytesQuotedJSON(Vector<uint8_t>& out, const WTF::String& str)
{
    out.append('"');
    if (!str.isEmpty()) {
        auto utf8 = str.utf8();
        auto data = utf8.span();
        for (size_t i = 0; i < data.size(); ++i) {
            uint8_t ch = data[i];
            if (ch < 0x20) {
                // Control characters need \uXXXX escaping (matches escapedFormsForJSON behavior)
                switch (ch) {
                case '\b': out.append('\\'); out.append('b'); break;
                case '\t': out.append('\\'); out.append('t'); break;
                case '\n': out.append('\\'); out.append('n'); break;
                case '\f': out.append('\\'); out.append('f'); break;
                case '\r': out.append('\\'); out.append('r'); break;
                default:
                    out.append('\\');
                    out.append('u');
                    out.append('0');
                    out.append('0');
                    out.append(upperNibbleToLowercaseASCIIHexDigit(ch));
                    out.append(lowerNibbleToLowercaseASCIIHexDigit(ch));
                    break;
                }
            } else if (ch == '"') {
                out.append('\\');
                out.append('"');
            } else if (ch == '\\') {
                out.append('\\');
                out.append('\\');
            } else {
                out.append(ch);
            }
        }
    }
    out.append('"');
}

static void appendASCIILiteral(Vector<uint8_t>& out, const char* str, size_t length)
{
    out.append(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(str), length));
}

static void appendASCII(Vector<uint8_t>& out, ASCIILiteral literal)
{
    appendASCIILiteral(out, literal.characters(), literal.length());
}

static void appendUnsigned(Vector<uint8_t>& out, size_t value)
{
    // Fast path for small numbers
    if (value == 0) {
        out.append('0');
        return;
    }

    // Max digits for size_t (20 digits for 64-bit)
    char buf[20];
    int pos = sizeof(buf);
    while (value > 0) {
        buf[--pos] = '0' + (value % 10);
        value /= 10;
    }
    out.append(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&buf[pos]), sizeof(buf) - pos));
}

Vector<uint8_t> BunV8HeapSnapshotBuilder::generateV8HeapSnapshotBytes()
{
    // Extra pass #1: fill in the node names
    for (auto& node : m_nodes) {
        if (node.cell)
            node.name = getDetailedNodeType(node.cell);
        node.edgesCount = 0; // Reset edge counts for deduplication pass
    }

    appendSyntheticRootEdges();

    // Sort edges by fromNodeId to ensure they're grouped correctly
    std::sort(m_edges.begin(), m_edges.end(),
        [](const Edge& a, const Edge& b) {
            if (a.fromNodeId != b.fromNodeId)
                return a.fromNodeId < b.fromNodeId;
            if (a.typeIndex != b.typeIndex)
                return a.typeIndex < b.typeIndex;
            if (a.toNodeId != b.toNodeId)
                return a.toNodeId < b.toNodeId;
            if (a.typeIndex == static_cast<unsigned>(V8EdgeType::Element) || a.typeIndex == static_cast<unsigned>(V8EdgeType::Hidden))
                return a.index < b.index;
            return WTF::codePointCompareLessThan(a.name, b.name);
        });

    // Deduplicate edges in-place and update edge counts
    if (!m_edges.isEmpty()) {
        size_t writeIndex = 1;
        m_nodes[m_edges[0].fromNodeId].edgesCount = 1;

        for (size_t readIndex = 1; readIndex < m_edges.size(); readIndex++) {
            const auto& prev = m_edges[writeIndex - 1];
            const auto& curr = m_edges[readIndex];

            bool isDuplicate = prev.fromNodeId == curr.fromNodeId && prev.toNodeId == curr.toNodeId && prev.typeIndex == curr.typeIndex;

            if (isDuplicate) {
                if (prev.typeIndex == static_cast<unsigned>(V8EdgeType::Element) || prev.typeIndex == static_cast<unsigned>(V8EdgeType::Hidden)) {
                    isDuplicate = prev.index == curr.index;
                } else {
                    isDuplicate = prev.name == curr.name;
                }
            }

            if (!isDuplicate) {
                if (writeIndex != readIndex)
                    m_edges[writeIndex] = WTF::move(m_edges[readIndex]);
                m_nodes[curr.fromNodeId].edgesCount++;
                writeIndex++;
            }
        }

        m_edges.shrink(writeIndex);
    }

    Vector<uint8_t> out;
    const unsigned NODE_FIELD_COUNT = 7;

    appendASCII(out, "{\"snapshot\":{\"meta\":{"_s);

    // Node fields
    appendASCII(out, "\"node_fields\":[\"type\",\"name\",\"id\",\"self_size\",\"edge_count\",\"trace_node_id\",\"detachedness\"],"_s);

    // Node types
    appendASCII(out, "\"node_types\":[["_s);
    bool first = true;
    for (const auto& type : m_nodeTypeNames) {
        if (!first)
            out.append(',');
        first = false;
        appendUTF8BytesQuotedJSON(out, type);
    }
    appendASCII(out, "],\"string\",\"number\",\"number\",\"number\",\"number\",\"number\"],"_s);

    // Edge fields
    appendASCII(out, "\"edge_fields\":[\"type\",\"name_or_index\",\"to_node\"],"_s);

    // Edge types
    appendASCII(out, "\"edge_types\":[["_s);
    first = true;
    for (const auto& type : m_edgeTypeNames) {
        if (!first)
            out.append(',');
        first = false;
        appendUTF8BytesQuotedJSON(out, type);
    }
    appendASCII(out, "],\"string_or_number\",\"node\"],"_s);

    // Trace function fields
    appendASCII(out, "\"trace_function_info_fields\":[\"function_id\",\"name\",\"script_name\",\"script_id\",\"line\",\"column\"],"_s);

    // Trace node fields
    appendASCII(out, "\"trace_node_fields\":[\"id\",\"function_info_index\",\"count\",\"size\",\"children\"],"_s);

    // Sample fields
    appendASCII(out, "\"sample_fields\":[\"timestamp_us\",\"last_assigned_id\"],"_s);

    // Location fields
    appendASCII(out, "\"location_fields\":[\"object_index\",\"script_id\",\"line\",\"column\"]"_s);

    appendASCII(out, "},"_s);

    unsigned traceFunctionCount = 0;
    appendASCII(out, "\"node_count\":"_s);
    appendUnsigned(out, m_nodes.size());
    appendASCII(out, ",\"edge_count\":"_s);
    appendUnsigned(out, m_edges.size());
    appendASCII(out, ",\"trace_function_count\":"_s);
    appendUnsigned(out, traceFunctionCount);
    appendASCII(out, "},"_s);

    // Nodes array
    appendASCII(out, "\"nodes\":["_s);
    for (unsigned i = 0; i < m_nodes.size(); ++i) {
        const auto& node = m_nodes[i];
        if (i)
            out.append(',');

        appendUnsigned(out, node.typeIndex);
        out.append(',');
        appendUnsigned(out, addString(node.name));
        out.append(',');
        appendUnsigned(out, node.id);
        out.append(',');
        appendUnsigned(out, node.selfSize);
        out.append(',');
        appendUnsigned(out, node.edgesCount);
        appendASCII(out, ",0,0"_s); // trace_node_id, detachedness
    }
    appendASCII(out, "],\n"_s);

    // Edges array
    appendASCII(out, "\"edges\":["_s);
    for (unsigned i = 0; i < m_edges.size(); ++i) {
        const auto& edge = m_edges[i];

        ASSERT(edge.fromNodeId < m_nodes.size());
        ASSERT(edge.toNodeId < m_nodes.size());

        if (i)
            out.append(',');

        appendUnsigned(out, edge.typeIndex);
        out.append(',');

        switch (edge.typeIndex) {
        case static_cast<unsigned>(V8EdgeType::Hidden):
        case static_cast<unsigned>(V8EdgeType::Element):
            appendUnsigned(out, edge.index);
            break;
        default:
            appendUnsigned(out, addString(edge.name));
        }
        out.append(',');

        appendUnsigned(out, edge.toNodeId * NODE_FIELD_COUNT);
    }
    appendASCII(out, "],\n"_s);

    appendASCII(out, "\"trace_function_infos\":[],\n"_s);
    appendASCII(out, "\"trace_tree\": [],\n"_s);
    appendASCII(out, "\"samples\":[],\n"_s);
    appendASCII(out, "\"locations\":[],\n"_s);

    // Strings table
    appendASCII(out, "\"strings\":["_s);

    first = true;
    for (const auto& str : m_strings) {
        if (!first)
            out.append(',');
        first = false;
        appendUTF8BytesQuotedJSON(out, str);
    }
    appendASCII(out, "]\n"_s);

    appendASCII(out, "}\n"_s);

    return out;
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
