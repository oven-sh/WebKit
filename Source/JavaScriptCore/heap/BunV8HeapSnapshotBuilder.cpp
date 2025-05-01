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
#include <wtf/text/WYHash.h>

static unsigned generateHashID(JSCell* cell, void* optionalHashId)
{
    // Attempt to use the wrapped object as the hash id if it exists
    // If it doesn't exist, use the cell pointer since that's the best we can do.
    if (optionalHashId == nullptr) {
        optionalHashId = cell;
    }

    // We hash:
    // - void* optionalHashId
    // - cell->type()
    // - cell->classInfo() (pointer)
    uintptr_t pointerNumber = reinterpret_cast<uintptr_t>(optionalHashId);
    char bytesToHash[sizeof(uintptr_t) * 2 + 1];
    std::span<char> span { bytesToHash, sizeof(bytesToHash) };
    memcpy(span.data(), &pointerNumber, sizeof(uintptr_t));
    span[sizeof(uintptr_t)] = cell->type();
    uintptr_t classInfoPtr = reinterpret_cast<uintptr_t>(cell->classInfo());
    memcpy(&span[sizeof(uintptr_t) + 1], &classInfoPtr, sizeof(uintptr_t));
    return WTF::WYHash::computeHashAndMaskTop8Bits<char>(span);
}

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BunV8HeapSnapshotBuilder);

BunV8HeapSnapshotBuilder::BunV8HeapSnapshotBuilder(HeapProfiler& profiler)
    : m_profiler(profiler)
{
    initializeTypeNames();

    // Initialize with synthetic root node
    m_nodes.append({
        .id = 0,
        .typeIndex = static_cast<unsigned>(V8NodeType::Synthetic),
        .name = "(root)"_s,
        .selfSize = 0,
        .edges = {},
        .traceLocation = std::nullopt,
        .parentNodeId = std::nullopt,
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
    m_profiler.clearSnapshots();
    VM& vm = m_profiler.vm();
    PreventCollectionScope preventCollectionScope(vm.heap);
    {

        ASSERT(!m_profiler.activeHeapAnalyzer());
        m_profiler.setActiveHeapAnalyzer(this);

        vm.heap.collectNow(Sync, CollectionScope::Full);
        m_profiler.setActiveHeapAnalyzer(nullptr);
    }

    JSLockHolder lock { vm };
    DeferGC deferGC(vm);
    return generateV8HeapSnapshot();
}

void BunV8HeapSnapshotBuilder::analyzeNode(JSCell* cell)
{
    if (!cell)
        return;

    {
        Locker locker { m_cellToNodeIdMutex };
        if (auto existingId = m_cellToNodeId.get(cell)) {
            return;
        }
    }

    unsigned id = analyzeNodeInternal(cell, nullptr);

    {
        Locker locker { m_cellToNodeIdMutex };
        m_cellToNodeId.set(cell, id);
    }
}

unsigned BunV8HeapSnapshotBuilder::analyzeNodeInternal(JSCell* cell, void* optionalHashId)
{

    Locker locker { m_buildingNodeMutex };
    auto typeIndex = getNodeTypeIndex(cell);
    unsigned id = m_nodes.size();

    m_nodes.append({
        .cell = cell,
        .id = generateHashID(cell, optionalHashId),
        .typeIndex = typeIndex,
        .selfSize = cell->estimatedSizeInBytes(m_profiler.vm()),
        .edges = {},
        .traceLocation = getTraceLocation(cell),
        .parentNodeId = std::nullopt,
    });

    return id;
}

void BunV8HeapSnapshotBuilder::analyzeEdge(JSCell* from, JSCell* to, RootMarkReason reason)
{
    ASSERT(to);

    // Avoid trivial edges.
    if (from == to)
        return;

    Locker locker { m_buildingEdgeMutex };
    Edge edge = {};
    edge.fromNodeId = from ? getOrCreateNodeId(from) : 0;
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

    m_edges.append(WTFMove(edge));
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
    m_edges.append(WTFMove(edge));
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

    m_edges.append(WTFMove(edge));
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

    m_edges.append(WTFMove(edge));
}

void BunV8HeapSnapshotBuilder::setOpaqueRootReachabilityReasonForCell(JSCell*, ASCIILiteral) {}
void BunV8HeapSnapshotBuilder::setWrappedObjectForCell(JSCell* cell, void* wrappedObject)
{
    unsigned id = getOrCreateNodeId(cell, wrappedObject);

    // TODO: make this one lock instead of two.
    Locker locker { m_buildingNodeMutex };
    m_nodes[id].id = generateHashID(cell, wrappedObject);
}

void BunV8HeapSnapshotBuilder::setLabelForCell(JSCell* cell, const String& label)
{
    if (!cell || label.isEmpty())
        return;

    Locker locker { m_buildingNodeMutex };
    m_cellLabels.set(cell, label);
}

unsigned BunV8HeapSnapshotBuilder::getOrCreateNodeId(JSCell* cell, void* optionalHashId)
{
    if (!cell)
        return 0; // Only return 0 for root

    Locker locker { m_cellToNodeIdMutex };
    auto it = m_cellToNodeId.find(cell);
    if (it != m_cellToNodeId.end())
        return it->value;

    unsigned id = analyzeNodeInternal(cell, optionalHashId);
    m_cellToNodeId.set(cell, id);
    return id;
}

unsigned BunV8HeapSnapshotBuilder::getNodeTypeIndex(JSCell* cell)
{
    if (!cell)
        return static_cast<unsigned>(V8NodeType::Synthetic);

    if (cell->isString()) {
        JSString* str = jsCast<JSString*>(cell);
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
    case JSType::StrictEvalActivationType:
    case JSType::ShadowRealmType:
    case JSType::WebAssemblyModuleType:
    case JSType::WebAssemblyInstanceType:
    case JSType::GetterSetterType:
    case JSType::CustomGetterSetterType:
    case JSType::APIValueWrapperType:
    case JSType::JSSourceCodeType:
    case JSType::JSScriptFetchParametersType:
    case JSType::WithScopeType:
    case JSType::GlobalLexicalEnvironmentType:
    case JSType::LexicalEnvironmentType:
    case JSType::ModuleEnvironmentType: {
        return static_cast<unsigned>(V8NodeType::Code);
    }
    case JSType::HeapBigIntType:
        return static_cast<unsigned>(V8NodeType::BigInt);
    case JSType::SymbolType:
        return static_cast<unsigned>(V8NodeType::Symbol);
    case JSType::RegExpObjectType:
        return static_cast<unsigned>(V8NodeType::RegExp);
    case JSType::ArrayType:
    case JSType::DerivedArrayType:
        return static_cast<unsigned>(V8NodeType::Array);

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

String BunV8HeapSnapshotBuilder::getDetailedNodeType(JSCell* cell, bool recurse)
{
    if (!cell)
        return "(root)"_s;

    // First check if we have a custom label for this cell
    auto labelIt = m_cellLabels.find(cell);
    if (labelIt != m_cellLabels.end())
        return labelIt->value;

    switch (cell->type()) {
    case JSType::StringType: {
        auto* string = jsCast<JSString*>(cell);
        auto value = string->tryGetValue(true);
        if (!value->isEmpty()) {
            return value;
        }
        break;
    }
    case JSType::JSDateType: {
        return "Date"_s;
        break;
    }
    case JSType::RegExpObjectType: {
        auto* object = jsCast<RegExpObject*>(cell);
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
        auto* symbol = jsCast<Symbol*>(cell);
        auto description = symbol->description();
        if (!description.isEmpty()) {
            return makeString("Symbol("_s, description, ')');
        }
        break;
    }
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
        if (JSGlobalObject* globalObject = object->globalObject()) {
            PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_profiler.vm());
            if (!object->getOwnPropertySlot(object, globalObject, m_profiler.vm().propertyNames->constructor, slot)) {
                name = JSObject::calculatedClassName(object);
            }
        }
    }

    if (JSPromise* promise = jsDynamicCast<JSPromise*>(cell)) {
        auto& vm = promise->globalObject()->vm();
        switch (promise->status(vm)) {
        case JSPromise::Status::Pending:
            return "Promise (pending)"_s;
        case JSPromise::Status::Fulfilled: {
            JSValue result = promise->result(vm);
            if (result.isCell() && recurse) {
                // set recurse to false to make sure we don't infinitely expand promises
                return makeString("Promise (fulfilled: "_s, getDetailedNodeType(result.asCell(), false), ")"_s);
            }
            return "Promise (fulfilled)"_s;
        }
        case JSPromise::Status::Rejected:
            return "Promise (rejected)"_s;
        }
    }

    auto* object = cell->getObject();

    if (object) {
        // For arrays, include the length
        if (JSArray* array = jsDynamicCast<JSArray*>(cell)) {
            return makeString("Array ("_s, array->length(), ")"_s);
        }

        // For functions, try to get the display name
        if (JSFunction* function = jsDynamicCast<JSFunction*>(cell)) {
            String displayName = function->nameWithoutGC(m_profiler.vm());
            if (!displayName.isEmpty())
                return displayName;
        }

        // For functions, try to get the display name
        if (InternalFunction* function = jsDynamicCast<InternalFunction*>(cell)) {
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

    // Context-related edges
    case RootMarkReason::VMExceptions:
    case RootMarkReason::ExecutableToCodeBlockEdges:
    case RootMarkReason::JITStubRoutines:
    case RootMarkReason::JITWorkList:
    case RootMarkReason::StrongReferences:
    case RootMarkReason::CodeBlocks:
    case RootMarkReason::MarkedJSValueRefArray:
    case RootMarkReason::MarkListSet:
    case RootMarkReason::StrongHandles:
    case RootMarkReason::DOMGCOutput:
    case RootMarkReason::Output:
    case RootMarkReason::ConservativeScan:
    case RootMarkReason::ExternalRememberedSet:
        return static_cast<unsigned>(V8EdgeType::Context);

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

    // Check if string already exists
    unsigned hash = str.hash();
    size_t hashKey = static_cast<size_t>(hash);
    // 32 bits: hash
    // 32 bits: length
    hashKey |= static_cast<size_t>(str.length()) << (sizeof(size_t) * 8 - 32);
    auto it = m_stringsLookupTable.find(hashKey);
    if (it != m_stringsLookupTable.end())
        return it->value;

    unsigned index = m_strings.size();
    m_strings.append(str);
    m_stringsLookupTable.set(hashKey, index);
    return index;
}

std::optional<BunV8HeapSnapshotBuilder::TraceLocation> BunV8HeapSnapshotBuilder::getTraceLocation(JSCell* cell)
{
    if (!cell || !cell->isCallable())
        return std::nullopt;

    JSFunction* function = jsDynamicCast<JSFunction*>(cell);
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
        node.name = getDetailedNodeType(node.cell);
        node.edgesCount = 0; // Reset edge counts for deduplication pass
    }

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
                    m_edges[writeIndex] = WTFMove(m_edges[readIndex]);
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

    // Trace function info array
    // We don't implement this yet
    json.append("\"trace_function_infos\":[],\n"_s);

    // Samples array
    // We don't implement this yet
    json.append("\"samples\":[],\n"_s);

    // Locations array - maps nodes to their source locations
    // We don't implement this yet
    json.append("\"locations\":[],\n"_s);

    // Trace tree - represents allocation stack traces
    // We don't implement this yet
    json.append("\"trace_tree\": [],\n"_s);

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

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
