/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVisibleStores.h"

#if ENABLE(AOT)

#include "AOTFlowSiteData.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Kinds = OptionSet<StoredKind>;
using Node = uint32_t;

// Whose code it is. Code without types names its receivers by nothing, so a store in it may mean any object of such code, and no object of checked code.
enum class CodeClass : uint8_t { Checked, Unchecked, Library };
constexpr unsigned numberOfCodeClasses = 3;
constexpr unsigned nodesPerKey = numberOfCodeClasses + 1;

// Where the kinds of a value come from: a node of the graph, or nowhere, because the value alone says them.
class Source {
public:
    Source() = default;
    Source(Kinds kinds)
        : m_bits(isConstantBit | kinds.toRaw())
    {
    }
    Source(StoredKind kind)
        : Source(Kinds { kind })
    {
    }

    static Source fromNode(Node node)
    {
        Source source;
        source.m_bits = node;
        return source;
    }

    bool isConstant() const { return m_bits & isConstantBit; }
    Kinds constant() const { return Kinds::fromRaw(static_cast<uint8_t>(m_bits)); }
    Kinds constantOrNothing() const { return isConstant() ? constant() : Kinds { }; }
    Node node() const { return m_bits; }

private:
    static constexpr uint32_t isConstantBit = 1u << 31;

    uint32_t m_bits { isConstantBit };
};

struct Edge {
    Source from;
    Node to;
};

enum class Rule : uint8_t { BitOperation, IntegerArithmetic };

struct Operation {
    Node result;
    Rule rule;
    Source left;
    Source right;
};

// The fields that an access by name may mean.
struct AccessedFields {
    Source read { StoredKind::Other };
    std::optional<Node> stored;
};

struct FieldWithKey {
    LayoutID layout;
    Node declaration;
};

// What one thread adds to the graph.
struct GraphPart {
    Node nextNode { 0 };
    Node endOfNodes { 0 };
    Vector<Edge> edges;
    Vector<Operation> operations;
    Vector<Source> sourcesOfValues;
};

// A node holds the kinds that reach one place (a field, a variable, a parameter, a result, the stores under a name) or one value of a function.
// Each function is read once, which gives the edges. Then the kinds follow the edges.
class StoreGraph {
public:
    StoreGraph(const Program&, const ParallelLoops&);

    void build();
    void solve();
    Kinds kindsStoredIn(FieldReference field) const { return Kinds::fromRaw(m_kinds[nodeOf(field)]); }

private:
    friend class FunctionReader;

    struct alignas(64) AccessTable {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, AccessedFields> fields WTF_GUARDED_BY_LOCK(lock);
    };
    static constexpr unsigned numberOfAccessTables = 256;
    static constexpr Node nodesTakenAtOnce = 1024;

    Node nodeOf(FieldReference) const;
    Node nodeOfStoresWithoutReceiver(CodeClass code, PropertyKeyID key) const { return m_firstKeyNode + key.index() * nodesPerKey + static_cast<unsigned>(code); }
    Node nodeOfAllStores(PropertyKeyID key) const { return m_firstKeyNode + key.index() * nodesPerKey + numberOfCodeClasses; }
    Node nodeOf(VariableID variable) const { return m_firstVariableNode + variable.index(); }
    Node nodeOfParameter(FunctionID function, unsigned parameter) const { return m_firstParameterOfFunction[function.index()] + parameter; }
    Node nodeOfResult(FunctionID function) const { return m_firstResultNode + function.index(); }

    bool holdsStoresWithoutReceiver(Node node) const { return node >= m_firstKeyNode && node < m_firstVariableNode && (node - m_firstKeyNode) % nodesPerKey != numberOfCodeClasses; }

    CodeClass codeClassOf(const Function&) const;
    CodeClass codeClassOf(LayoutID) const;
    void indexFieldsByKey();
    std::span<const FieldWithKey> fieldsWithKey(PropertyKeyID key) const { return m_fieldsWithKey.subspan(m_firstFieldWithKey[key.index()], m_firstFieldWithKey[key.index() + 1] - m_firstFieldWithKey[key.index()]); }

    Node addNode(GraphPart&);
    void connect(GraphPart&, Source from, Node to);
    AccessedFields accessedFields(GraphPart&, LayoutSetID receivers, PropertyKeyID);
    AccessedFields findAccessedFields(GraphPart&, LayoutSetID receivers, PropertyKeyID);
    void storeConstantData(GraphPart&);
    void indexEdges();

    Kinds seenAt(Node) const;
    Kinds seenAt(Source source) const { return source.isConstant() ? source.constant() : seenAt(source.node()); }
    void join(Node, Kinds);
    void enqueue(Node);
    void propagate();

    const Program& m_program;
    const ParallelLoops& m_loops;
    Vector<Node> m_firstFieldOfLayout;
    Vector<CodeClass> m_codeClassOfField;
    Vector<Node> m_firstParameterOfFunction;
    Node m_firstKeyNode { 0 };
    Node m_firstVariableNode { 0 };
    Node m_firstResultNode { 0 };
    Node m_numberOfPlaces { 0 };
    Vector<uint32_t> m_firstFieldWithKey;
    Vector<FieldWithKey> m_fieldsWithKey;

    std::atomic<Node> m_numberOfNodes { 0 };
    Vector<std::atomic<uint8_t>> m_constantsStoredInPlaces;
    std::array<AccessTable, numberOfAccessTables> m_accessTables;
    Vector<GraphPart> m_parts;

    Vector<uint8_t> m_kinds;
    Vector<uint32_t> m_firstSuccessor;
    Vector<Node> m_successors;
    Vector<uint32_t> m_operationOfValue;
    Vector<Operation> m_operations;
    Vector<Node> m_worklist;
    Vector<bool> m_isEnqueued;
    // A place that nothing seen reaches is one that only unseen code fills.
    bool m_unseenSourcesAreOther { false };
};

class FunctionReader {
public:
    FunctionReader(StoreGraph& graph, GraphPart& part, FunctionID id)
        : m_graph(graph)
        , m_part(part)
        , m_program(graph.m_program)
        , m_id(id)
        , m_function(m_program.at(id))
        , m_code(graph.codeClassOf(m_function))
        , m_sources(part.sourcesOfValues)
    {
    }

    void run();

private:
    Source sourceOf(ValueID id) const { return m_sources[id.index()]; }
    void connect(Source from, Node to) { m_graph.connect(m_part, from, to); }

    void readParameters();
    void read(const Block&);
    Source read(const Value&);
    Source readByType(const Value&) const;
    Source readBitOperation(const Value&);
    Source readOperation(Rule, const Value&);
    Source readPropertyRead(const Value&);
    Source readCall(const Value&);
    void readStoreByName(const Value&);
    void readInitialValues(const Value&);
    void passArguments(const Target&);
    FunctionID calleeOf(const Value&) const;

    StoreGraph& m_graph;
    GraphPart& m_part;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    CodeClass m_code;
    Vector<Source>& m_sources;
};

} // anonymous namespace

static Kinds apply(Rule rule, Kinds left, Kinds right)
{
    bool lacksOperand = !left || !right;
    switch (rule) {
    case Rule::BitOperation: {
        // A BigInt and a number do not mix, so an operand that is no BigInt makes the result an int32.
        auto isNoBigInt = [](Kinds kinds) {
            return kinds && !kinds.contains(StoredKind::Other);
        };
        if (isNoBigInt(left) || isNoBigInt(right))
            return StoredKind::Int32;
        return lacksOperand ? Kinds { } : Kinds { StoredKind::Other };
    }
    case Rule::IntegerArithmetic: {
        // The sum of two int32s is an integer, and one that leaves the range is rare enough.
        auto mayBeOnlyInt32 = [](Kinds kinds) {
            return !kinds || kinds == Kinds { StoredKind::Int32 };
        };
        if (!mayBeOnlyInt32(left) || !mayBeOnlyInt32(right))
            return StoredKind::Other;
        return lacksOperand ? Kinds { } : Kinds { StoredKind::Int32 };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StoreGraph::StoreGraph(const Program& program, const ParallelLoops& loops)
    : m_program(program)
    , m_loops(loops)
    , m_parts(loops.width())
{
    Node numberOfNodes = 0;
    m_firstFieldOfLayout = WTF::map(program.layouts, [&](const Layout& layout) {
        Node first = numberOfNodes;
        numberOfNodes += layout.fields.size();
        return first;
    });
    m_codeClassOfField.reserveInitialCapacity(numberOfNodes);
    for (size_t index = 0; index < program.layouts.size(); ++index)
        m_codeClassOfField.appendUsingFunctor(program.layouts[index].fields.size(), [code = codeClassOf(LayoutID(index))](size_t) { return code; });

    m_firstKeyNode = numberOfNodes;
    numberOfNodes += program.types.numberOfPropertyKeys() * nodesPerKey;
    m_firstVariableNode = numberOfNodes;
    numberOfNodes += program.variables.size();
    m_firstResultNode = numberOfNodes;
    numberOfNodes += program.functions.size();
    m_firstParameterOfFunction = WTF::map(program.functions, [&](const Function& function) {
        Node first = numberOfNodes;
        numberOfNodes += function.numberOfPositionalParameters();
        return first;
    });
    m_numberOfPlaces = numberOfNodes;
    m_numberOfNodes.store(numberOfNodes, std::memory_order_relaxed);
    m_constantsStoredInPlaces = Vector<std::atomic<uint8_t>>(numberOfNodes);
    indexFieldsByKey();
}

// The field of an interface and the fields that implement it are one place.
Node StoreGraph::nodeOf(FieldReference reference) const
{
    for (size_t steps = 0; steps < m_program.layouts.size(); ++steps) {
        const Field& field = fieldAt(m_program, reference);
        if (!field.originLayout || !field.originField)
            break;
        reference = { field.originLayout, *field.originField };
    }
    return m_firstFieldOfLayout[reference.layout.index()] + reference.field;
}

CodeClass StoreGraph::codeClassOf(const Function& function) const
{
    if (function.role == FunctionRole::Library)
        return CodeClass::Library;
    return function.has(FunctionFlags::IsChecked) ? CodeClass::Checked : CodeClass::Unchecked;
}

CodeClass StoreGraph::codeClassOf(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    if (layout.constructor)
        return codeClassOf(m_program.at(layout.constructor));
    if (!layout.module)
        return CodeClass::Library;
    const Module& declaringModule = m_program.at(layout.module);
    if (declaringModule.flags.contains(ModuleFlags::IsLibrary) || !declaringModule.body)
        return CodeClass::Library;
    return codeClassOf(m_program.at(declaringModule.body));
}

// Under each key, in the order of the layouts. No access by name means a variable.
void StoreGraph::indexFieldsByKey()
{
    m_firstFieldWithKey.fill(0, m_program.types.numberOfPropertyKeys() + 1);
    for (const Layout& layout : m_program.layouts) {
        if (layout.isEnvironment())
            continue;
        for (const Field& field : layout.fields) {
            if (field.key)
                ++m_firstFieldWithKey[field.key.index() + 1];
        }
    }
    for (size_t key = 1; key < m_firstFieldWithKey.size(); ++key)
        m_firstFieldWithKey[key] += m_firstFieldWithKey[key - 1];

    Vector<uint32_t> next = m_firstFieldWithKey;
    m_fieldsWithKey = Vector<FieldWithKey>(m_firstFieldWithKey.last());
    for (size_t index = 0; index < m_program.layouts.size(); ++index) {
        const Layout& layout = m_program.layouts[index];
        if (layout.isEnvironment())
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (PropertyKeyID key = layout.fields[field].key)
                m_fieldsWithKey[next[key.index()]++] = { LayoutID(index), nodeOf({ LayoutID(index), field }) };
        }
    }
}

Node StoreGraph::addNode(GraphPart& part)
{
    if (part.nextNode == part.endOfNodes) {
        part.nextNode = m_numberOfNodes.fetch_add(nodesTakenAtOnce, std::memory_order_relaxed);
        part.endOfNodes = part.nextNode + nodesTakenAtOnce;
    }
    return part.nextNode++;
}

void StoreGraph::connect(GraphPart& part, Source from, Node to)
{
    if (!from.isConstant()) {
        if (from.node() != to)
            part.edges.append({ from, to });
        return;
    }
    if (!from.constant())
        return;
    if (to >= m_numberOfPlaces) {
        part.edges.append({ from, to });
        return;
    }
    std::atomic<uint8_t>& stored = m_constantsStoredInPlaces[to];
    if (!Kinds::fromRaw(stored.load(std::memory_order_relaxed)).containsAll(from.constant()))
        stored.fetch_or(from.constant().toRaw(), std::memory_order_relaxed);
}

AccessedFields StoreGraph::findAccessedFields(GraphPart& part, LayoutSetID receivers, PropertyKeyID key)
{
    std::span<const FieldWithKey> fields = fieldsWithKey(key);
    std::span<const LayoutID> members = m_program.at(receivers).members.span();
    Vector<Node, 8> declarations;
    if (fields.size() <= members.size()) {
        for (const FieldWithKey& field : fields) {
            if (std::ranges::binary_search(members, field.layout))
                declarations.append(field.declaration);
        }
    } else {
        for (LayoutID member : members) {
            auto [first, last] = std::ranges::equal_range(fields, member, { }, &FieldWithKey::layout);
            for (; first != last; ++first)
                declarations.append(first->declaration);
        }
    }
    std::ranges::sort(declarations);
    removeRepeatedElements(declarations);

    if (declarations.isEmpty())
        return { };
    if (declarations.size() == 1)
        return { Source::fromNode(declarations[0]), declarations[0] };

    Node read = addNode(part);
    Node stored = addNode(part);
    for (Node declaration : declarations) {
        part.edges.append({ Source::fromNode(declaration), read });
        part.edges.append({ Source::fromNode(stored), declaration });
    }
    return { Source::fromNode(read), stored };
}

AccessedFields StoreGraph::accessedFields(GraphPart& part, LayoutSetID receivers, PropertyKeyID key)
{
    // Zero is no key of the table.
    uint64_t tableKey = (static_cast<uint64_t>(receivers.index()) + 1) << 32 | key.index();
    AccessTable& table = m_accessTables[WTF::intHash(tableKey) % numberOfAccessTables];
    Locker locker { table.lock };
    auto result = table.fields.add(tableKey, AccessedFields { });
    if (result.isNewEntry)
        result.iterator->value = findAccessedFields(part, receivers, key);
    return result.iterator->value;
}

static Kinds kindsOf(const DataNode& node)
{
    if (const DataNode::Number* number = node.tryAs<DataNode::Number>())
        return tryConvertToStrictInt32(number->value) ? StoredKind::Int32 : StoredKind::Other;
    if (node.is<DataNode::Undefined>())
        return StoredKind::Undefined;
    return node.is<DataNode::Null>() ? StoredKind::Null : StoredKind::Other;
}

void StoreGraph::storeConstantData(GraphPart& part)
{
    for (const Data& data : m_program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (const DataMember& member : object->members) {
                connect(part, kindsOf(member.value), nodeOfAllStores(member.key));
                if (std::optional<uint32_t> field = m_program.at(object->layout).findField(member.key))
                    connect(part, kindsOf(member.value), nodeOf({ object->layout, *field }));
            }
        });
    }
}

void StoreGraph::build()
{
    storeConstantData(m_parts[0]);

    Vector<uint32_t> largestFirst = largeFunctions(m_program, m_loops);
    BitVector comesFirst(m_program.functions.size());
    for (uint32_t index : largestFirst)
        comesFirst.quickSet(index);
    unsigned numberFirst = largestFirst.size();
    forEachIndexInParallel(m_loops, numberFirst + m_program.functions.size(), [&](unsigned position, unsigned worker) {
        if (position >= numberFirst && comesFirst.quickGet(position - numberFirst))
            return;
        FunctionID id(position < numberFirst ? largestFirst[position] : position - numberFirst);
        if (m_program.at(id).hasBody())
            FunctionReader(*this, m_parts[worker], id).run();
    });
    indexEdges();
}

void StoreGraph::indexEdges()
{
    Node numberOfNodes = m_numberOfNodes.load(std::memory_order_relaxed);
    m_kinds.fill(0, numberOfNodes);
    for (Node place = 0; place < m_numberOfPlaces; ++place)
        m_kinds[place] = m_constantsStoredInPlaces[place].load(std::memory_order_relaxed);

    m_firstSuccessor.fill(0, numberOfNodes + 1);
    size_t numberOfOperations = 0;
    for (const GraphPart& part : m_parts) {
        numberOfOperations += part.operations.size();
        for (const Edge& edge : part.edges) {
            if (edge.from.isConstant())
                m_kinds[edge.to] |= edge.from.constant().toRaw();
            else
                ++m_firstSuccessor[edge.from.node() + 1];
        }
    }
    for (size_t node = 1; node < m_firstSuccessor.size(); ++node)
        m_firstSuccessor[node] += m_firstSuccessor[node - 1];

    Vector<uint32_t> next = m_firstSuccessor;
    m_successors = Vector<Node>(m_firstSuccessor.last());
    m_operationOfValue.fill(UINT32_MAX, numberOfNodes - m_numberOfPlaces);
    m_operations.reserveInitialCapacity(numberOfOperations);
    for (GraphPart& part : m_parts) {
        for (const Edge& edge : part.edges) {
            if (!edge.from.isConstant())
                m_successors[next[edge.from.node()]++] = edge.to;
        }
        for (const Operation& operation : part.operations) {
            m_operationOfValue[operation.result - m_numberOfPlaces] = m_operations.size();
            m_operations.append(operation);
        }
        part = { };
    }
}

Kinds StoreGraph::seenAt(Node node) const
{
    Kinds kinds = Kinds::fromRaw(m_kinds[node]);
    if (!kinds && m_unseenSourcesAreOther && node < m_numberOfPlaces && !holdsStoresWithoutReceiver(node))
        return StoredKind::Other;
    return kinds;
}

void StoreGraph::enqueue(Node node)
{
    if (m_isEnqueued[node])
        return;
    m_isEnqueued[node] = true;
    m_worklist.append(node);
}

void StoreGraph::join(Node node, Kinds kinds)
{
    if (Kinds::fromRaw(m_kinds[node]).containsAll(kinds))
        return;
    m_kinds[node] |= kinds.toRaw();
    enqueue(node);
}

void StoreGraph::propagate()
{
    while (!m_worklist.isEmpty()) {
        Node node = m_worklist.takeLast();
        m_isEnqueued[node] = false;
        Kinds kinds = seenAt(node);

        if (holdsStoresWithoutReceiver(node)) {
            auto storing = static_cast<CodeClass>((node - m_firstKeyNode) % nodesPerKey);
            for (const FieldWithKey& field : fieldsWithKey(PropertyKeyID((node - m_firstKeyNode) / nodesPerKey))) {
                CodeClass declaring = m_codeClassOfField[field.declaration];
                if (storing == CodeClass::Checked || declaring == CodeClass::Library || (storing == CodeClass::Unchecked && declaring == CodeClass::Unchecked))
                    join(field.declaration, kinds);
            }
        }

        for (Node successor : m_successors.subspan(m_firstSuccessor[node], m_firstSuccessor[node + 1] - m_firstSuccessor[node])) {
            uint32_t operationIndex = successor < m_numberOfPlaces ? UINT32_MAX : m_operationOfValue[successor - m_numberOfPlaces];
            if (operationIndex == UINT32_MAX) {
                join(successor, kinds);
                continue;
            }
            const Operation& operation = m_operations[operationIndex];
            join(successor, apply(operation.rule, seenAt(operation.left), seenAt(operation.right)));
        }
    }
}

void StoreGraph::solve()
{
    m_isEnqueued.fill(false, m_kinds.size());
    for (Node node = 0; node < m_kinds.size(); ++node) {
        if (m_kinds[node])
            enqueue(node);
    }
    propagate();

    m_unseenSourcesAreOther = true;
    for (Node place = 0; place < m_numberOfPlaces; ++place) {
        if (!m_kinds[place] && !holdsStoresWithoutReceiver(place))
            enqueue(place);
    }
    propagate();
}

void FunctionReader::readParameters()
{
    const auto& entryParameters = m_function.blocks[0].parameters;
    unsigned first = m_function.firstParameterIndex();
    unsigned numberOfPositionalParameters = m_function.numberOfPositionalParameters();
    for (unsigned index = 0; index < entryParameters.size(); ++index) {
        bool isPositional = index >= first && index - first < numberOfPositionalParameters;
        m_sources[entryParameters[index].index()] = isPositional ? Source::fromNode(m_graph.nodeOfParameter(m_id, index - first)) : Source(StoredKind::Other);
    }
    for (const Block& block : m_function.blocks.subspan(1)) {
        for (ValueID parameter : block.parameters)
            m_sources[parameter.index()] = Source::fromNode(m_graph.addNode(m_part));
    }
}

Source FunctionReader::readByType(const Value& value) const
{
    if (!value.type)
        return { };
    const Type& type = m_program.at(value.type);
    if (type.isNever())
        return { };
    if (!type.isInt32OrNullish() || type.primitives & Primitive::Absent)
        return StoredKind::Other;
    Kinds kinds;
    if (type.primitives & Primitive::Integer)
        kinds.add(StoredKind::Int32);
    if (type.primitives & Primitive::Undefined)
        kinds.add(StoredKind::Undefined);
    if (type.primitives & Primitive::Null)
        kinds.add(StoredKind::Null);
    return kinds;
}

Source FunctionReader::readOperation(Rule rule, const Value& value)
{
    Source left = sourceOf(value.operand(0));
    Source right = value.numberOfOperands() > 1 ? sourceOf(value.operand(1)) : left;
    if (left.isConstant() && right.isConstant())
        return apply(rule, left.constant(), right.constant());
    // One operand may decide the result alone.
    if (Kinds decided = apply(rule, left.constantOrNothing(), right.constantOrNothing()))
        return decided;

    Node result = m_graph.addNode(m_part);
    m_part.operations.append({ result, rule, left, right });
    if (!left.isConstant())
        m_part.edges.append({ left, result });
    if (!right.isConstant() && right.node() != left.node())
        m_part.edges.append({ right, result });
    return Source::fromNode(result);
}

Source FunctionReader::readBitOperation(const Value& value)
{
    bool hasOperandThatIsNoBigInt = false;
    value.forEachOperand([&](ValueID operand, uint32_t) {
        hasOperandThatIsNoBigInt |= !(m_program.typeOf(m_function, operand).primitives & Primitive::BigInt);
    });
    if (hasOperandThatIsNoBigInt)
        return StoredKind::Int32;
    return readOperation(Rule::BitOperation, value);
}

Source FunctionReader::readPropertyRead(const Value& value)
{
    if (value.isFreeName())
        return StoredKind::Other;
    LayoutSetID receivers = m_program.predictionOrTypeOf(m_function, value.operand(*receiverOf(value.opcode))).layouts;
    if (!receivers)
        return Source::fromNode(m_graph.nodeOfAllStores(value.key()));
    return m_graph.accessedFields(m_part, receivers, value.key()).read;
}

FunctionID FunctionReader::calleeOf(const Value& call) const
{
    const Value& callee = m_function.at(sameObjectThroughCasts(m_function, call.operand(0)));
    if (callee.opcode == Opcode::NewClosure)
        return callee.function();
    if (callee.opcode != Opcode::GetModuleVariable)
        return { };
    const Variable& variable = m_program.at(callee.variable());
    return variable.flags.contains(VariableFlags::IsImmutable) ? variable.function : FunctionID();
}

Source FunctionReader::readCall(const Value& call)
{
    FunctionID callee = calleeOf(call);
    if (!callee)
        return StoredKind::Other;

    std::span<const ValueID> arguments = call.variadicOperands();
    unsigned numberOfParameters = m_program.at(callee).numberOfPositionalParameters();
    for (unsigned index = 0; index < numberOfParameters; ++index)
        connect(index < arguments.size() ? sourceOf(arguments[index]) : Source(StoredKind::Undefined), m_graph.nodeOfParameter(callee, index));

    bool returnsItsResult = call.opcode == Opcode::Call && !m_program.at(callee).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
    return returnsItsResult ? Source::fromNode(m_graph.nodeOfResult(callee)) : Source(StoredKind::Other);
}

void FunctionReader::readStoreByName(const Value& store)
{
    if (store.opcode == Opcode::PropSet && store.isFreeName())
        return;
    PropertyKeyID key = store.key();
    Source stored = sourceOf(store.operand(*storedOf(store.opcode)));
    connect(stored, m_graph.nodeOfAllStores(key));

    LayoutSetID receivers = m_program.predictionOrTypeOf(m_function, store.operand(*receiverOf(store.opcode))).layouts;
    if (!receivers) {
        connect(stored, m_graph.nodeOfStoresWithoutReceiver(m_code, key));
        return;
    }
    if (std::optional<Node> fields = m_graph.accessedFields(m_part, receivers, key).stored)
        connect(stored, *fields);
}

void FunctionReader::readInitialValues(const Value& allocation)
{
    std::span<const ValueID> initialValues = allocation.variadicOperands();
    for (unsigned index = 0; index < initialValues.size(); ++index) {
        FieldReference field { allocation.layout(), allocation.fields()[index] };
        connect(sourceOf(initialValues[index]), m_graph.nodeOf(field));
        if (PropertyKeyID key = fieldAt(m_program, field).key)
            connect(sourceOf(initialValues[index]), m_graph.nodeOfAllStores(key));
    }
}

Source FunctionReader::read(const Value& value)
{
    if (isCast(value.opcode))
        return sourceOf(value.operand(0));

    switch (value.opcode) {
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::BitNot:
    case Opcode::LShift:
    case Opcode::RShift:
        return readBitOperation(value);
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return readOperation(Rule::IntegerArithmetic, value);
    case Opcode::PropGet:
        return readPropertyRead(value);
    case Opcode::GetModuleVariable:
        return Source::fromNode(m_graph.nodeOf(value.variable()));
    case Opcode::GetEnvironmentField:
        return Source::fromNode(m_graph.nodeOf(FieldReference { value.layout(), value.field() }));
    case Opcode::Call:
    case Opcode::Construct:
        return readCall(value);
    case Opcode::PropSet:
    case Opcode::PropDefine:
        readStoreByName(value);
        return { };
    case Opcode::PutModuleVariable:
        connect(sourceOf(value.operand(OperandIndex::PutModuleVariable::value)), m_graph.nodeOf(value.variable()));
        return { };
    case Opcode::PutEnvironmentField:
        connect(sourceOf(value.operand(OperandIndex::PutEnvironmentField::value)), m_graph.nodeOf(FieldReference { value.layout(), value.field() }));
        return { };
    case Opcode::NewObject:
    case Opcode::CloneAs:
        readInitialValues(value);
        return StoredKind::Other;
    default:
        return readByType(value);
    }
}

void FunctionReader::passArguments(const Target& target)
{
    const auto& parameters = m_function.at(target.block).parameters;
    for (unsigned index = 0; index < parameters.size(); ++index) {
        Source parameter = sourceOf(parameters[index]);
        if (!parameter.isConstant())
            connect(index < target.arguments.size() ? sourceOf(target.arguments[index]) : Source(StoredKind::Other), parameter.node());
    }
}

void FunctionReader::read(const Block& block)
{
    for (ValueID id : block.values)
        m_sources[id.index()] = read(m_function.at(id));
    block.forEachTarget([&](const Target& target) {
        passArguments(target);
    });
    const Terminator& terminator = block.terminator;
    if (terminator.kind == TerminatorKind::Return) {
        ValueID result = terminator.operand(OperandIndex::Return::value);
        connect(result ? sourceOf(result) : Source(StoredKind::Undefined), m_graph.nodeOfResult(m_id));
    } else if (terminator.kind == TerminatorKind::ReturnCall)
        connect(sourceOf(terminator.operand(OperandIndex::ReturnCall::call)), m_graph.nodeOfResult(m_id));
}

// A definition comes before its uses in the order of the blocks.
void FunctionReader::run()
{
    m_sources.fill(Source(), m_function.values.size());
    readParameters();
    for (const Block& block : m_function.blocks)
        read(block);
}

VisibleStores::VisibleStores(const Program& program, const ParallelLoops& loops)
{
    StoreGraph graph(program, loops);
    graph.build();
    graph.solve();
    m_kinds = Vector<Vector<OptionSet<StoredKind>>>(program.layouts.size(), [&](size_t layout) {
        return Vector<OptionSet<StoredKind>>(program.layouts[layout].fields.size(), [&](size_t field) {
            return graph.kindsStoredIn({ LayoutID(layout), static_cast<uint32_t>(field) });
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
