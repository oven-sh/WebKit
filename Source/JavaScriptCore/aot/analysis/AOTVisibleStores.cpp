/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVisibleStores.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTEdgeResults.h"
#include "AOTFlowSiteData.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MallocSpan.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Kinds = OptionSet<StoredKind>;
using Node = uint32_t;

constexpr Kinds allKinds { StoredKind::Int32, StoredKind::Undefined, StoredKind::Null, StoredKind::Other, StoredKind::NoInt32 };
constexpr Kinds numberOutsideInt32 { StoredKind::Other, StoredKind::NoInt32 };

// Whose code it is. Code without types names its receivers by nothing, so a store in it may mean any object of such code, and no object of checked code.
enum class CodeClass : uint8_t { Checked, Unchecked, Library };
constexpr unsigned numberOfCodeClasses = 3;
constexpr unsigned nodesPerKey = numberOfCodeClasses + 1;

// A call that may enter more functions than this is one that nobody follows.
constexpr unsigned maxCalleesOfCall = 8;

// A large array whose bytes all threads set, so that no single thread takes all of its page faults.
template<typename T>
class FilledArray {
public:
    FilledArray() = default;

    FilledArray(const ParallelLoops& loops, size_t size, uint8_t byte = 0)
        : m_elements(MallocSpan<T>::malloc(size * sizeof(T)))
    {
        static_assert(std::is_trivially_destructible_v<T>);
        constexpr size_t bytesPerTask = 256 * KB;
        std::span<uint8_t> bytes = unsafeMakeSpan(reinterpret_cast<uint8_t*>(m_elements.mutableSpan().data()), size * sizeof(T));
        forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + bytesPerTask - 1) / bytesPerTask), [&](unsigned task) {
            memsetSpan(bytes.subspan(task * bytesPerTask, std::min(bytesPerTask, bytes.size() - task * bytesPerTask)), byte);
        });
    }

    static FilledArray createWithoutFilling(size_t size)
    {
        FilledArray result;
        result.m_elements = MallocSpan<T>::malloc(size * sizeof(T));
        return result;
    }

    size_t size() const { return m_elements.span().size(); }
    bool isEmpty() const { return !size(); }
    T& operator[](size_t index) { return m_elements[index]; }
    const T& operator[](size_t index) const { return m_elements[index]; }

private:
    MallocSpan<T> m_elements;
};

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

// What the reader of a function asks about the type of a value, by TypeID.
struct TypeFacts {
    LayoutSetID layouts;
    Source valueSource;
    Kinds admittedKinds;
    bool isInt32 : 1 { false };
    bool fitsInt32Slot : 1 { false };
    bool mayBeBigInt : 1 { false };
};

struct Argument {
    Source source;
    bool hasInt32Type { false };
};

enum class Rule : uint8_t { BitOperation, IntegerArithmetic, Narrowing };

struct Operation {
    Node result { 0 };
    Rule rule { Rule::Narrowing };
    Source left;
    Source right;
};

// The fields that an access by name may mean.
struct AccessedFields {
    Source read { StoredKind::Other };
    std::optional<Node> stored;
    bool areRequired { false };
};

struct FieldWithKey {
    LayoutID layout;
    Node declaration { 0 };
    bool isRequired { false };

    friend bool operator==(const FieldWithKey&, const FieldWithKey&) = default;
    friend auto operator<=>(const FieldWithKey&, const FieldWithKey&) = default;
};

struct MethodDefinition {
    LayoutID layout;
    PropertyKeyID key;
    FunctionID function;

    friend bool operator==(const MethodDefinition&, const MethodDefinition&) = default;
    friend auto operator<=>(const MethodDefinition&, const MethodDefinition&) = default;
};

// No thread knows every method while the functions are read, so a call of a method waits until all of them have been.
struct MethodCall {
    LayoutSetID receivers;
    PropertyKeyID key;
    uint32_t firstArgument;
    uint32_t numberOfArguments;
    Node result;
};

struct Callees {
    Vector<FunctionID, 2> functions;
    bool areAllSeen { false };
};

// What one thread adds to the graph.
struct GraphPart {
    Node nextNode { 0 };
    Node endOfNodes { 0 };
    Vector<Edge> edges;
    Vector<Operation> operations;
    Vector<MethodDefinition> methods;
    Vector<MethodCall> methodCalls;
    Vector<Argument> argumentsOfMethodCalls;
    Vector<Node> numbersPassedOtherTypes;
    Vector<Node> fieldsStartedWithOtherTypes;
    Vector<Node> usedAsIntegers;
    Vector<Source> sourcesOfValues;
};

// A node holds the kinds that reach one place (a field, a variable, a parameter, a result, the stores under a name) or one value of a function.
class StoreGraph {
public:
    StoreGraph(const Program&, const ParallelLoops&);

    void build(const VisibleStores::FunctionFilter&);
    void solve();
    void findUsesAsIntegers();
    const TypeFacts& factsOf(TypeID type) const { return m_typeFacts[type.index()]; }
    Kinds kindsStoredIn(FieldReference field) const { return kindsAt(nodeOf(field)); }
    bool isUsedAsInteger(FieldReference field) const { return m_usesAsIntegers[nodeOf(field)].load(std::memory_order_relaxed); }
    Kinds kindsPassedTo(FunctionID function, unsigned parameter) const { return kindsAt(nodeOfParameter(function, parameter)); }
    bool isPassedOtherTypes(FunctionID function, unsigned parameter) const { return m_numbersPassedOtherTypes.quickGet(nodeOfParameter(function, parameter)); }
    bool isStartedWithOtherTypes(FieldReference field) const { return m_fieldsStartedWithOtherTypes.quickGet(nodeOf(field)); }

private:
    friend class FunctionReader;

    struct alignas(64) AccessTable {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, AccessedFields> fields WTF_GUARDED_BY_LOCK(lock);
        UncheckedKeyHashMap<uint64_t, Callees> methods WTF_GUARDED_BY_LOCK(lock);
    };
    static constexpr unsigned numberOfAccessTables = 256;
    static constexpr Node nodesTakenAtOnce = 256;
    static constexpr Node nodesPerShare = 1 << 13;

    // Zero is no key of a table.
    static uint64_t tableKeyOf(LayoutSetID receivers, PropertyKeyID key) { return (static_cast<uint64_t>(receivers.index()) + 1) << 32 | key.index(); }
    AccessTable& tableOf(uint64_t tableKey) { return m_accessTables[WTF::intHash(tableKey) % numberOfAccessTables]; }

    Node nodeOf(FieldReference) const;
    Node nodeOfStoresWithoutReceiver(CodeClass code, PropertyKeyID key) const { return m_firstKeyNode + key.index() * nodesPerKey + static_cast<unsigned>(code); }
    Node nodeOfAllStores(PropertyKeyID key) const { return m_firstKeyNode + key.index() * nodesPerKey + numberOfCodeClasses; }
    Node nodeOf(VariableID variable) const { return m_firstVariableNode + variable.index(); }
    Node nodeOfParameter(FunctionID function, unsigned parameter) const { return m_firstParameterOfFunction[function.index()] + parameter; }
    Node nodeOfResult(FunctionID function) const { return m_firstResultNode + function.index(); }

    bool holdsStoresUnderKey(Node node) const { return node >= m_firstKeyNode && node < m_firstVariableNode; }
    bool isResult(Node node) const { return node >= m_firstResultNode && node - m_firstResultNode < m_program.functions.size(); }
    bool isParameter(Node node) const { return node >= m_firstResultNode + m_program.functions.size() && node < m_numberOfPlaces; }
    bool holdsStoresWithoutReceiver(Node node) const { return node >= m_firstKeyNode && node < m_firstVariableNode && (node - m_firstKeyNode) % nodesPerKey != numberOfCodeClasses; }

    CodeClass codeClassOf(const Function&) const;
    CodeClass codeClassOf(LayoutID) const;
    void computeTypeFacts();
    void indexFieldsByKey();
    std::span<const FieldWithKey> fieldsWithKey(PropertyKeyID key) const { return m_fieldsWithKey.subspan(m_firstFieldWithKey[key.index()], m_firstFieldWithKey[key.index() + 1] - m_firstFieldWithKey[key.index()]); }

    Node addNode(GraphPart&);
    void connect(GraphPart&, Source from, Node to);
    AccessedFields accessedFields(GraphPart&, LayoutSetID receivers, PropertyKeyID);
    AccessedFields findAccessedFields(GraphPart&, LayoutSetID receivers, PropertyKeyID);
    void passArgument(GraphPart&, FunctionID callee, unsigned parameter, Argument);
    void passArguments(GraphPart&, FunctionID callee, std::span<const Argument> arguments);
    Source resultOf(FunctionID callee) const;
    void indexMethods();
    std::span<const MethodDefinition> methodsOf(LayoutID, PropertyKeyID) const LIFETIME_BOUND;
    Callees calledMethods(LayoutSetID receivers, PropertyKeyID);
    Callees findCalledMethods(LayoutSetID receivers, PropertyKeyID) const;
    void connectMethodCalls(GraphPart&);
    void storeConstantData();
    void indexEdges();

    void forEachShareOfNodes(Node count, NOESCAPE const Invocable<void(unsigned share, Node first, Node end)> auto&) const;
    Kinds kindsAt(Node node) const { return Kinds::fromRaw(m_kinds[node].load()); }
    Kinds seenAt(Node) const;
    Kinds seenAt(Source source) const { return source.isConstant() ? source.constant() : seenAt(source.node()); }
    Kinds leaving(Source) const;
    void join(Vector<Node>& worklist, Node, Kinds);
    void enqueue(Vector<Node>& worklist, Node);
    void propagate(Vector<Node>& worklist);
    void propagateFrom(Node endOfSeeds, NOESCAPE const Invocable<bool(Node)> auto& isSeed);

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
    Vector<TypeFacts> m_typeFacts;

    std::atomic<Node> m_numberOfNodes { 0 };
    FilledArray<std::atomic<uint8_t>> m_constantsStoredInPlaces;
    std::array<AccessTable, numberOfAccessTables> m_accessTables;
    Vector<GraphPart> m_parts;
    Vector<MethodDefinition> m_methods;
    BitVector m_numbersPassedOtherTypes;
    BitVector m_fieldsStartedWithOtherTypes;

    FilledArray<std::atomic<uint8_t>> m_kinds;
    Vector<uint32_t> m_firstSuccessor;
    Vector<Node> m_successors;
    FilledArray<uint32_t> m_operationOfValue;
    FilledArray<Operation> m_operations;
    FilledArray<std::atomic<uint8_t>> m_isEnqueued;
    Vector<Node> m_usedAsIntegers;
    FilledArray<std::atomic<uint8_t>> m_usesAsIntegers;
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
    Source sourceOf(ValueID id) const { return id ? m_sources[id.index()] : Source(StoredKind::Other); }
    const TypeFacts& typeFactsOf(ValueID id) const { return m_graph.factsOf(m_function.at(id).type); }
    LayoutSetID expectedLayoutsOf(ValueID id) const { return m_graph.factsOf(m_function.at(id).expectedOrType()).layouts; }
    Argument argumentOf(ValueID id) const { return { sourceOf(id), id && typeFactsOf(id).isInt32 }; }
    void connect(Source from, Node to) { m_graph.connect(m_part, from, to); }

    void readParameters();
    void read(BlockID);
    Source read(const Value&);
    Source readByType(const Value&) const;
    Source readBitOperation(const Value&);
    Source readOperation(Rule, const Value&);
    Source readPropertyRead(const Value&);
    Source readCall(const Value&);
    Source readMethodCall(const Value&);
    Source readDirectCall(const Value&);
    Source readEdgeResult(EdgeResultPosition, ValueID parameter);
    void readMethodDefinition(const Value&);
    void readStoreByName(const Value&);
    void readInitialValues(const Value&);
    void noteUseAsInteger(ValueID);
    void noteUsesAsIntegers(const Value&);
    void noteArgumentsTakenAsIntegers(const Value& call);
    bool hasCountedElements(ValueID object) const;
    void passArguments(const Target&, NOESCAPE const Invocable<Source(uint32_t resultIndex, ValueID parameter)> auto& readResult);
    bool forEachCallee(const Value& call, NOESCAPE const Invocable<void(FunctionID)> auto&) const;
    bool isFieldDefinitionWithoutValue(const Value& store) const;
    Source narrow(Source, TypeID);
    Source unite(std::span<const Source>);

    StoreGraph& m_graph;
    GraphPart& m_part;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    CodeClass m_code;
    Vector<Source>& m_sources;
};

} // anonymous namespace

// An operand that gets more kinds takes nothing away from the result. So the kinds that the graph settles on are the same in whatever order they arrive.
static Kinds apply(Rule rule, Kinds left, Kinds right)
{
    Kinds result;
    switch (rule) {
    case Rule::BitOperation:
        // A BigInt and a number do not mix, so an operand that is no BigInt makes the result an int32.
        if (left - StoredKind::Other || right - StoredKind::Other)
            result.add(StoredKind::Int32);
        if (left.contains(StoredKind::Other) && right.contains(StoredKind::Other))
            result.add(StoredKind::Other);
        return result;
    case Rule::IntegerArithmetic:
        // The sum of two int32s is an integer, and one that leaves the range is rare enough.
        if (left.contains(StoredKind::Int32) && right.contains(StoredKind::Int32))
            result.add(StoredKind::Int32);
        if (left - StoredKind::Int32 || right - StoredKind::Int32)
            result.add(StoredKind::Other);
        if ((left | right).contains(StoredKind::NoInt32))
            result.add(StoredKind::NoInt32);
        return result;
    case Rule::Narrowing:
        return left & right;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Kinds kindsAdmittedBy(const Type& type)
{
    Kinds kinds;
    if (type.primitives & Primitive::Integer && !type.integers.intersectWith(Range::int32()).isEmpty())
        kinds.add(StoredKind::Int32);
    if (type.primitives & Primitive::Undefined)
        kinds.add(StoredKind::Undefined);
    if (type.primitives & Primitive::Null)
        kinds.add(StoredKind::Null);
    if (!type.isInt32OrNullish() || type.primitives & Primitive::Absent)
        kinds.add(StoredKind::Other);
    if (type.primitives & Primitive::Double || (type.primitives & Primitive::Integer && !type.integers.isSubsetOf(Range::int32())))
        kinds.add(StoredKind::NoInt32);
    return kinds;
}

StoreGraph::StoreGraph(const Program& program, const ParallelLoops& loops)
    : m_program(program)
    , m_loops(loops)
    , m_parts(loops.width())
{
    // All threads read the records, which are far apart. One thread adds up the sizes, which lie side by side.
    Node numberOfNodes = 0;
    auto numberNodes = [&](size_t count, NOESCAPE const Invocable<Node(size_t)> auto& numberOfNodesAt) {
        constexpr size_t recordsPerTask = 512;
        Vector<Node> firstNodes(count);
        forEachIndexInParallel(loops, static_cast<unsigned>((count + recordsPerTask - 1) / recordsPerTask), [&](unsigned task) {
            for (size_t index = task * recordsPerTask, end = std::min(count, index + recordsPerTask); index < end; ++index)
                firstNodes[index] = numberOfNodesAt(index);
        });
        for (Node& first : firstNodes)
            first = std::exchange(numberOfNodes, numberOfNodes + first);
        return firstNodes;
    };
    m_firstFieldOfLayout = numberNodes(program.layouts.size(), [&](size_t index) {
        return static_cast<Node>(program.layouts[index].fields.size());
    });
    m_codeClassOfField.grow(numberOfNodes);
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        std::ranges::fill(m_codeClassOfField.mutableSpan().subspan(m_firstFieldOfLayout[index], program.layouts[index].fields.size()), codeClassOf(LayoutID(index)));
    });

    m_firstKeyNode = numberOfNodes;
    numberOfNodes += program.types.numberOfPropertyKeys() * nodesPerKey;
    m_firstVariableNode = numberOfNodes;
    numberOfNodes += program.variables.size();
    m_firstResultNode = numberOfNodes;
    numberOfNodes += program.functions.size();
    m_firstParameterOfFunction = numberNodes(program.functions.size(), [&](size_t index) {
        return static_cast<Node>(program.functions[index].numberOfPositionalParameters());
    });
    m_numberOfPlaces = numberOfNodes;
    m_numberOfNodes.store(numberOfNodes, std::memory_order_relaxed);
    m_constantsStoredInPlaces = FilledArray<std::atomic<uint8_t>>(m_loops, numberOfNodes);
    indexFieldsByKey();
    computeTypeFacts();
}

static Source sourceOfValueWithType(const Type& type)
{
    if (type.isNever())
        return { };
    if (!type.isInt32OrNullish() || type.primitives & Primitive::Absent)
        return type.hasOnlyPrimitives(Primitive::Number) && !kindsAdmittedBy(type).contains(StoredKind::Int32) ? numberOutsideInt32 : Kinds { StoredKind::Other };
    Kinds kinds;
    if (type.primitives & Primitive::Integer)
        kinds.add(StoredKind::Int32);
    if (type.primitives & Primitive::Undefined)
        kinds.add(StoredKind::Undefined);
    if (type.primitives & Primitive::Null)
        kinds.add(StoredKind::Null);
    return kinds;
}

static bool fitsInt32Slot(const Type& type)
{
    return type.isInt32OrNullish() && type.integers.isSubsetOf(Range { minInlineInt32, INT32_MAX });
}

void StoreGraph::computeTypeFacts()
{
    constexpr size_t typesPerTask = 1024;
    size_t numberOfTypes = m_program.types.size();
    m_typeFacts.grow(numberOfTypes);
    forEachIndexInParallel(m_loops, static_cast<unsigned>((numberOfTypes + typesPerTask - 1) / typesPerTask), [&](unsigned task) {
        for (size_t index = task * typesPerTask, end = std::min(numberOfTypes, index + typesPerTask); index < end; ++index) {
            const Type& type = m_program.at(TypeID(index));
            TypeFacts& facts = m_typeFacts[index];
            facts.layouts = type.layouts;
            facts.valueSource = sourceOfValueWithType(type);
            facts.admittedKinds = kindsAdmittedBy(type);
            facts.isInt32 = type.isInt32();
            facts.fitsInt32Slot = fitsInt32Slot(type);
            facts.mayBeBigInt = type.primitives & Primitive::BigInt;
        }
    });
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
    auto forEachFieldWithKey = [&](unsigned layoutIndex, const Invocable<void(uint32_t, const Field&)> auto& functor) {
        const Layout& layout = m_program.layouts[layoutIndex];
        if (layout.isEnvironment())
            return;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (layout.fields[field].key)
                functor(field, layout.fields[field]);
        }
    };

    unsigned numberOfKeys = m_program.types.numberOfPropertyKeys();
    FilledArray<std::atomic<uint32_t>> next(m_loops, numberOfKeys);
    forEachIndexInParallel(m_loops, m_program.layouts.size(), [&](unsigned layoutIndex) {
        forEachFieldWithKey(layoutIndex, [&](uint32_t, const Field& field) {
            next[field.key.index()].fetch_add(1, std::memory_order_relaxed);
        });
    });

    uint32_t numberOfFields = 0;
    m_firstFieldWithKey.fill(0, numberOfKeys + 1);
    for (unsigned key = 0; key < numberOfKeys; ++key) {
        m_firstFieldWithKey[key] = numberOfFields;
        numberOfFields += next[key].exchange(numberOfFields, std::memory_order_relaxed);
    }
    m_firstFieldWithKey[numberOfKeys] = numberOfFields;

    m_fieldsWithKey = Vector<FieldWithKey>(numberOfFields);
    forEachIndexInParallel(m_loops, m_program.layouts.size(), [&](unsigned layoutIndex) {
        forEachFieldWithKey(layoutIndex, [&](uint32_t index, const Field& field) {
            uint32_t position = next[field.key.index()].fetch_add(1, std::memory_order_relaxed);
            m_fieldsWithKey[position] = { LayoutID(layoutIndex), nodeOf({ LayoutID(layoutIndex), index }), field.flags.contains(FieldFlags::IsRequired) };
        });
    });
    forEachIndexInParallel(m_loops, numberOfKeys, [&](unsigned key) {
        std::span<FieldWithKey> fields = m_fieldsWithKey.mutableSpan().subspan(m_firstFieldWithKey[key], m_firstFieldWithKey[key + 1] - m_firstFieldWithKey[key]);
        if (fields.size() > 1)
            AOT::sort(fields);
    });
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
    bool areRequired = true;
    auto add = [&](const FieldWithKey& field) {
        declarations.append(field.declaration);
        areRequired &= field.isRequired;
    };
    if (fields.size() <= members.size()) {
        for (const FieldWithKey& field : fields) {
            if (std::ranges::binary_search(members, field.layout))
                add(field);
        }
    } else {
        for (LayoutID member : members) {
            auto [first, last] = std::ranges::equal_range(fields, member, { }, &FieldWithKey::layout);
            for (; first != last; ++first)
                add(*first);
        }
    }
    AOT::sort(declarations);
    removeRepeatedElements(declarations);

    if (declarations.isEmpty())
        return { };
    if (declarations.size() == 1)
        return { Source::fromNode(declarations[0]), declarations[0], areRequired };

    Node read = addNode(part);
    Node stored = addNode(part);
    for (Node declaration : declarations) {
        part.edges.append({ Source::fromNode(declaration), read });
        part.edges.append({ Source::fromNode(stored), declaration });
    }
    return { Source::fromNode(read), stored, areRequired };
}

AccessedFields StoreGraph::accessedFields(GraphPart& part, LayoutSetID receivers, PropertyKeyID key)
{
    uint64_t tableKey = tableKeyOf(receivers, key);
    AccessTable& table = tableOf(tableKey);
    Locker locker { table.lock };
    auto result = table.fields.add(tableKey, AccessedFields { });
    if (result.isNewEntry)
        result.iterator->value = findAccessedFields(part, receivers, key);
    return result.iterator->value;
}

void StoreGraph::passArgument(GraphPart& part, FunctionID callee, unsigned parameter, Argument argument)
{
    connect(part, argument.source, nodeOfParameter(callee, parameter));
    if (argument.hasInt32Type)
        return;
    const Function& function = m_program.at(callee);
    const DeclaredType* declared = function.declaredEntryParameter(function.firstParameterIndex() + parameter);
    if (declared && declared->type == TypeTable::number)
        part.numbersPassedOtherTypes.append(nodeOfParameter(callee, parameter));
}

void StoreGraph::passArguments(GraphPart& part, FunctionID callee, std::span<const Argument> arguments)
{
    unsigned numberOfParameters = m_program.at(callee).numberOfPositionalParameters();
    for (unsigned index = 0; index < numberOfParameters; ++index)
        passArgument(part, callee, index, index < arguments.size() ? arguments[index] : Argument { StoredKind::Undefined });
}

Source StoreGraph::resultOf(FunctionID callee) const
{
    bool returnsItsResult = !m_program.at(callee).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
    return returnsItsResult ? Source::fromNode(nodeOfResult(callee)) : Source(StoredKind::Other);
}

void StoreGraph::indexMethods()
{
    for (GraphPart& part : m_parts)
        m_methods.appendVector(std::exchange(part.methods, { }));
    AOT::sort(m_methods);
    removeRepeatedElements(m_methods);
}

// What the nearest class defines under the key, from the class of the layout upward.
std::span<const MethodDefinition> StoreGraph::methodsOf(LayoutID layout, PropertyKeyID key) const
{
    LayoutID current = layout;
    for (size_t steps = 0; current && steps < m_program.layouts.size(); ++steps) {
        auto [first, last] = std::ranges::equal_range(m_methods, std::pair { current, key }, { }, [](const MethodDefinition& method) {
            return std::pair { method.layout, method.key };
        });
        if (first != last)
            return { first, last };
        current = expectedBaseOf(m_program.at(current));
    }
    return { };
}

Callees StoreGraph::findCalledMethods(LayoutSetID receivers, PropertyKeyID key) const
{
    const LayoutSet& family = m_program.at(receivers);
    Callees callees { .areAllSeen = !family.hasMembersWithoutLayout };
    for (LayoutID member : family.members) {
        const Layout& layout = m_program.at(member);
        if (layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        // A field of the object hides the method, and holds what the stores put there.
        std::optional<uint32_t> field = layout.findField(key);
        bool mayBeHidden = field && !layout.fields[*field].flags.contains(FieldFlags::IsMethod);
        std::span<const MethodDefinition> methods = methodsOf(member, key);
        if (mayBeHidden || methods.empty())
            callees.areAllSeen = false;
        for (const MethodDefinition& method : methods)
            callees.functions.append(method.function);
    }
    AOT::sort(callees.functions);
    removeRepeatedElements(callees.functions);
    if (callees.functions.isEmpty() || callees.functions.size() > maxCalleesOfCall)
        return { };
    return callees;
}

Callees StoreGraph::calledMethods(LayoutSetID receivers, PropertyKeyID key)
{
    uint64_t tableKey = tableKeyOf(receivers, key);
    AccessTable& table = tableOf(tableKey);
    Locker locker { table.lock };
    auto result = table.methods.add(tableKey, Callees { });
    if (result.isNewEntry)
        result.iterator->value = findCalledMethods(receivers, key);
    return result.iterator->value;
}

void StoreGraph::connectMethodCalls(GraphPart& part)
{
    for (const MethodCall& call : part.methodCalls) {
        Callees callees = calledMethods(call.receivers, call.key);
        std::span<const Argument> arguments = part.argumentsOfMethodCalls.subspan(call.firstArgument, call.numberOfArguments);
        for (FunctionID callee : callees.functions) {
            passArguments(part, callee, arguments);
            connect(part, resultOf(callee), call.result);
        }
        if (!callees.areAllSeen)
            connect(part, StoredKind::Other, call.result);
    }
}

static Kinds kindsOf(const DataNode& node)
{
    if (const DataNode::Number* number = node.tryAs<DataNode::Number>())
        return tryConvertToStrictInt32(number->value) ? StoredKind::Int32 : StoredKind::Other;
    if (node.is<DataNode::Undefined>())
        return StoredKind::Undefined;
    return node.is<DataNode::Null>() ? StoredKind::Null : StoredKind::Other;
}

// Each store puts constant kinds into a place, which changes no part of the graph.
void StoreGraph::storeConstantData()
{
    forEachIndexInParallel(m_loops, m_program.data.size(), [&](unsigned index, unsigned worker) {
        GraphPart& part = m_parts[worker];
        forEachDataNodeInPreorder(m_program.data[index].root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (const DataMember& member : object->members) {
                connect(part, kindsOf(member.value), nodeOfAllStores(member.key));
                if (std::optional<uint32_t> field = object->layout ? m_program.at(object->layout).findField(member.key) : std::nullopt)
                    connect(part, kindsOf(member.value), nodeOf({ object->layout, *field }));
            }
        });
    });
}

void StoreGraph::build(const VisibleStores::FunctionFilter& shouldRead)
{
    storeConstantData();

    Vector<uint32_t> largestFirst = largeFunctions(m_program, m_loops);
    BitVector comesFirst(m_program.functions.size());
    for (uint32_t index : largestFirst)
        comesFirst.quickSet(index);
    unsigned numberFirst = largestFirst.size();
    forEachIndexInParallel(m_loops, numberFirst + m_program.functions.size(), [&](unsigned position, unsigned worker) {
        if (position >= numberFirst && comesFirst.quickGet(position - numberFirst))
            return;
        FunctionID id(position < numberFirst ? largestFirst[position] : position - numberFirst);
        if (m_program.at(id).hasBody() && shouldRead(id, worker))
            FunctionReader(*this, m_parts[worker], id).run();
    });

    indexMethods();
    forEachIndexInParallel(m_loops, m_parts.size(), [&](unsigned index) {
        connectMethodCalls(m_parts[index]);
    });
    m_numbersPassedOtherTypes = BitVector(m_numberOfPlaces);
    m_fieldsStartedWithOtherTypes = BitVector(m_numberOfPlaces);
    for (const GraphPart& part : m_parts) {
        for (Node parameter : part.numbersPassedOtherTypes)
            m_numbersPassedOtherTypes.quickSet(parameter);
        for (Node field : part.fieldsStartedWithOtherTypes)
            m_fieldsStartedWithOtherTypes.quickSet(field);
    }
    indexEdges();
}

// The order of the successors of a node follows from which thread came first, like the numbers of the nodes. The kinds that the graph settles on follow from neither.
void StoreGraph::indexEdges()
{
    Node numberOfNodes = m_numberOfNodes.load(std::memory_order_relaxed);
    Node numberOfValues = numberOfNodes - m_numberOfPlaces;

    // connect() keeps the constants that reach a place out of the edges.
    FilledArray<std::atomic<uint8_t>> constantsOfValues(m_loops, numberOfValues);
    FilledArray<std::atomic<uint32_t>> next(m_loops, numberOfNodes);
    forEachIndexInParallel(m_loops, m_parts.size(), [&](unsigned index) {
        for (const Edge& edge : m_parts[index].edges) {
            if (edge.from.isConstant())
                constantsOfValues[edge.to - m_numberOfPlaces].fetch_or(edge.from.constant().toRaw(), std::memory_order_relaxed);
            else
                next[edge.from.node()].fetch_add(1, std::memory_order_relaxed);
        }
    });

    Vector<uint32_t> firstSuccessorOfShare((numberOfNodes + nodesPerShare - 1) / nodesPerShare);
    forEachShareOfNodes(numberOfNodes, [&](unsigned share, Node first, Node end) {
        uint32_t numberInShare = 0;
        for (Node node = first; node < end; ++node)
            numberInShare += next[node].load(std::memory_order_relaxed);
        firstSuccessorOfShare[share] = numberInShare;
    });
    uint32_t numberOfSuccessors = 0;
    for (uint32_t& first : firstSuccessorOfShare)
        first = std::exchange(numberOfSuccessors, numberOfSuccessors + first);

    m_kinds = FilledArray<std::atomic<uint8_t>>::createWithoutFilling(numberOfNodes);
    m_firstSuccessor = Vector<uint32_t>(numberOfNodes + 1);
    forEachShareOfNodes(numberOfNodes, [&](unsigned share, Node first, Node end) {
        uint32_t firstSuccessor = firstSuccessorOfShare[share];
        for (Node node = first; node < end; ++node) {
            const std::atomic<uint8_t>& constants = node < m_numberOfPlaces ? m_constantsStoredInPlaces[node] : constantsOfValues[node - m_numberOfPlaces];
            m_kinds[node].store(constants.load(std::memory_order_relaxed), std::memory_order_relaxed);
            m_firstSuccessor[node] = firstSuccessor;
            firstSuccessor += next[node].exchange(firstSuccessor, std::memory_order_relaxed);
        }
    });
    m_firstSuccessor[numberOfNodes] = numberOfSuccessors;

    for (const GraphPart& part : m_parts)
        m_usedAsIntegers.appendVector(part.usedAsIntegers);

    uint32_t numberOfOperations = 0;
    Vector<uint32_t> firstOperationOfPart = WTF::map(m_parts, [&](const GraphPart& part) {
        uint32_t first = numberOfOperations;
        numberOfOperations += part.operations.size();
        return first;
    });

    m_successors = Vector<Node>(numberOfSuccessors);
    m_operationOfValue = FilledArray<uint32_t>(m_loops, numberOfValues, 0xff);
    m_operations = FilledArray<Operation>::createWithoutFilling(numberOfOperations);
    forEachIndexInParallel(m_loops, m_parts.size(), [&](unsigned index) {
        GraphPart& part = m_parts[index];
        for (const Edge& edge : part.edges) {
            if (!edge.from.isConstant())
                m_successors[next[edge.from.node()].fetch_add(1, std::memory_order_relaxed)] = edge.to;
        }
        uint32_t operationIndex = firstOperationOfPart[index];
        for (const Operation& operation : part.operations) {
            m_operationOfValue[operation.result - m_numberOfPlaces] = operationIndex;
            m_operations[operationIndex++] = operation;
        }
        part = { };
    });
}

void StoreGraph::forEachShareOfNodes(Node count, NOESCAPE const Invocable<void(unsigned share, Node first, Node end)> auto& functor) const
{
    forEachIndexInParallel(m_loops, (count + nodesPerShare - 1) / nodesPerShare, [&](unsigned share) {
        Node first = share * nodesPerShare;
        functor(share, first, std::min(count, first + nodesPerShare));
    });
}

Kinds StoreGraph::seenAt(Node node) const
{
    Kinds kinds = kindsAt(node);
    if (!kinds && m_unseenSourcesAreOther && node < m_numberOfPlaces && !holdsStoresWithoutReceiver(node))
        return StoredKind::Other;
    return kinds;
}

// That a number is no int32 is what the program shows where it stores the number. A place that many fill, the parameter of a helper above all, would hand it on to everything that passes it.
Kinds StoreGraph::leaving(Source source) const
{
    Kinds kinds = seenAt(source);
    return !source.isConstant() && source.node() < m_numberOfPlaces ? kinds - StoredKind::NoInt32 : kinds;
}

void StoreGraph::enqueue(Vector<Node>& worklist, Node node)
{
    if (!m_isEnqueued[node].exchange(1))
        worklist.append(node);
}

void StoreGraph::join(Vector<Node>& worklist, Node node, Kinds kinds)
{
    if (Kinds::fromRaw(m_kinds[node].load(std::memory_order_relaxed)).containsAll(kinds))
        return;
    m_kinds[node].fetch_or(kinds.toRaw());
    enqueue(worklist, node);
}

void StoreGraph::propagate(Vector<Node>& worklist)
{
    while (!worklist.isEmpty()) {
        Node node = worklist.takeLast();
        m_isEnqueued[node].store(0);
        Kinds kinds = leaving(Source::fromNode(node));

        if (holdsStoresWithoutReceiver(node)) {
            auto storing = static_cast<CodeClass>((node - m_firstKeyNode) % nodesPerKey);
            for (const FieldWithKey& field : fieldsWithKey(PropertyKeyID((node - m_firstKeyNode) / nodesPerKey))) {
                CodeClass declaring = m_codeClassOfField[field.declaration];
                if (storing == CodeClass::Checked || declaring == CodeClass::Library || (storing == CodeClass::Unchecked && declaring == CodeClass::Unchecked))
                    join(worklist, field.declaration, kinds);
            }
        }

        for (Node successor : m_successors.subspan(m_firstSuccessor[node], m_firstSuccessor[node + 1] - m_firstSuccessor[node])) {
            uint32_t operationIndex = successor < m_numberOfPlaces ? UINT32_MAX : m_operationOfValue[successor - m_numberOfPlaces];
            if (operationIndex == UINT32_MAX) {
                join(worklist, successor, kinds);
                continue;
            }
            const Operation& operation = m_operations[operationIndex];
            join(worklist, successor, apply(operation.rule, leaving(operation.left), leaving(operation.right)));
        }
    }
}

void StoreGraph::propagateFrom(Node endOfSeeds, NOESCAPE const Invocable<bool(Node)> auto& isSeed)
{
    forEachShareOfNodes(endOfSeeds, [&](unsigned, Node first, Node end) {
        Vector<Node> worklist;
        for (Node node = first; node < end; ++node) {
            if (isSeed(node))
                enqueue(worklist, node);
        }
        propagate(worklist);
    });
}

void StoreGraph::solve()
{
    m_isEnqueued = FilledArray<std::atomic<uint8_t>>(m_loops, m_kinds.size());
    propagateFrom(m_kinds.size(), [&](Node node) {
        return !!kindsAt(node);
    });

    m_unseenSourcesAreOther = true;
    propagateFrom(m_numberOfPlaces, [&](Node place) {
        return !kindsAt(place) && !holdsStoresWithoutReceiver(place);
    });
}

// Against the edges, from the uses to the places that the values come from.
void StoreGraph::findUsesAsIntegers()
{
    constexpr uint8_t mayReachCallers = 1 << 0;
    constexpr uint8_t cameThroughResult = 1 << 1;

    Node numberOfNodes = m_kinds.size();
    m_usesAsIntegers = FilledArray<std::atomic<uint8_t>>(m_loops, numberOfNodes);
    if (m_usedAsIntegers.isEmpty())
        return;

    FilledArray<std::atomic<uint32_t>> next(m_loops, numberOfNodes);
    forEachShareOfNodes(m_successors.size(), [&](unsigned, Node first, Node end) {
        for (Node successor : m_successors.subspan(first, end - first))
            next[successor].fetch_add(1, std::memory_order_relaxed);
    });
    Vector<uint32_t> firstPredecessorOfShare((numberOfNodes + nodesPerShare - 1) / nodesPerShare);
    forEachShareOfNodes(numberOfNodes, [&](unsigned share, Node first, Node end) {
        uint32_t numberInShare = 0;
        for (Node node = first; node < end; ++node)
            numberInShare += next[node].load(std::memory_order_relaxed);
        firstPredecessorOfShare[share] = numberInShare;
    });
    uint32_t numberOfPredecessors = 0;
    for (uint32_t& first : firstPredecessorOfShare)
        first = std::exchange(numberOfPredecessors, numberOfPredecessors + first);
    Vector<uint32_t> firstPredecessor(numberOfNodes + 1);
    forEachShareOfNodes(numberOfNodes, [&](unsigned share, Node first, Node end) {
        uint32_t position = firstPredecessorOfShare[share];
        for (Node node = first; node < end; ++node) {
            firstPredecessor[node] = position;
            position += next[node].exchange(position, std::memory_order_relaxed);
        }
    });
    firstPredecessor[numberOfNodes] = numberOfPredecessors;
    Vector<Node> predecessors(m_successors.size());
    forEachShareOfNodes(numberOfNodes, [&](unsigned, Node first, Node end) {
        for (Node node = first; node < end; ++node) {
            for (Node successor : m_successors.subspan(m_firstSuccessor[node], m_firstSuccessor[node + 1] - m_firstSuccessor[node]))
                predecessors[next[successor].fetch_add(1, std::memory_order_relaxed)] = node;
        }
    });

    constexpr size_t usesPerShare = 1 << 10;
    forEachIndexInParallel(m_loops, (m_usedAsIntegers.size() + usesPerShare - 1) / usesPerShare, [&](unsigned share) {
        Vector<Node> worklist;
        auto mark = [&](Node node, uint8_t ways) {
            if ((m_usesAsIntegers[node].load(std::memory_order_relaxed) & ways) == ways)
                return;
            if ((m_usesAsIntegers[node].fetch_or(ways) & ways) != ways)
                worklist.append(node);
        };
        size_t first = share * usesPerShare;
        for (Node node : m_usedAsIntegers.subspan(first, std::min(usesPerShare, m_usedAsIntegers.size() - first)))
            mark(node, mayReachCallers);
        while (!worklist.isEmpty()) {
            Node node = worklist.takeLast();
            uint8_t ways = m_usesAsIntegers[node].load();
            if (holdsStoresUnderKey(node))
                continue;
            if (node >= m_numberOfPlaces) {
                uint32_t operationIndex = m_operationOfValue[node - m_numberOfPlaces];
                if (operationIndex != UINT32_MAX && m_operations[operationIndex].rule == Rule::BitOperation)
                    continue;
            }
            if (isResult(node))
                ways = cameThroughResult;
            else if (isParameter(node))
                ways &= mayReachCallers;
            if (!ways)
                continue;
            for (Node predecessor : predecessors.subspan(firstPredecessor[node], firstPredecessor[node + 1] - firstPredecessor[node]))
                mark(predecessor, ways);
        }
    });
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
    return value.type ? m_graph.factsOf(value.type).valueSource : Source();
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
        hasOperandThatIsNoBigInt |= !typeFactsOf(operand).mayBeBigInt;
    });
    if (hasOperandThatIsNoBigInt)
        return StoredKind::Int32;
    return readOperation(Rule::BitOperation, value);
}

Source FunctionReader::readPropertyRead(const Value& value)
{
    if (value.isFreeName())
        return StoredKind::Other;
    LayoutSetID receivers = expectedLayoutsOf(value.operand(*receiverOf(value.opcode)));
    if (!receivers)
        return Source::fromNode(m_graph.nodeOfAllStores(value.key()));
    return m_graph.accessedFields(m_part, receivers, value.key()).read;
}

// What is left of a value that has passed a test or a cast.
Source FunctionReader::narrow(Source source, TypeID type)
{
    if (!type)
        return source;
    Kinds admitted = m_graph.factsOf(type).admittedKinds;
    if (source.isConstant())
        return source.constant() & admitted;
    if (admitted == allKinds)
        return source;
    // Nothing tells the other values apart.
    if (!(admitted - StoredKind::Other))
        return admitted;

    Node result = m_graph.addNode(m_part);
    m_part.operations.append({ result, Rule::Narrowing, source, admitted });
    m_part.edges.append({ source, result });
    return Source::fromNode(result);
}

Source FunctionReader::unite(std::span<const Source> sources)
{
    if (sources.size() == 1)
        return sources[0];
    Node result = m_graph.addNode(m_part);
    for (Source source : sources)
        connect(source, result);
    return Source::fromNode(result);
}

// Answers whether the program shows every function that the call may enter.
bool FunctionReader::forEachCallee(const Value& call, NOESCAPE const Invocable<void(FunctionID)> auto& functor) const
{
    static_assert(!OperandIndex::Call::callee && !OperandIndex::Construct::callee && !OperandIndex::SuperConstruct::parent);
    ValueID operand = call.operand(0);
    if (!operand)
        return false;

    const Value& callee = m_function.at(sameObjectThroughCasts(m_function, operand));
    if (callee.opcode == Opcode::NewClosure) {
        functor(callee.function());
        return true;
    }
    if (callee.opcode == Opcode::GetModuleVariable) {
        const Variable& variable = m_program.at(callee.variable());
        if (variable.flags.contains(VariableFlags::IsImmutable) && variable.function) {
            functor(variable.function);
            return true;
        }
    }

    // A function type admits every function. Those that it lists are the ones that the program shows.
    const Type& type = m_program.expectedOrTypeOf(m_function, operand);
    bool listsOnlyFunctions = !type.anyObject && type.classes.isEmpty() && !type.layouts && type.builtins.isEmpty();
    if (!listsOnlyFunctions || type.functions.isEmpty() || type.functions.size() > maxCalleesOfCall)
        return false;
    for (FunctionID function : type.functions)
        functor(function);
    return true;
}

Source FunctionReader::readCall(const Value& call)
{
    Vector<Argument, 8> arguments = WTF::map<8>(call.variadicOperands(), [&](ValueID argument) {
        return argumentOf(argument);
    });
    Vector<Source, maxCalleesOfCall> results;
    bool seesEveryCallee = forEachCallee(call, [&](FunctionID callee) {
        m_graph.passArguments(m_part, callee, arguments.span());
        results.append(m_graph.resultOf(callee));
    });
    if (call.opcode != Opcode::Call)
        return StoredKind::Other;
    return seesEveryCallee ? unite(results.span()) : readMethodCall(call);
}

Source FunctionReader::readMethodCall(const Value& call)
{
    ValueID operand = call.operand(OperandIndex::Call::callee);
    if (!operand)
        return StoredKind::Other;
    const Value& callee = m_function.at(sameObjectThroughCasts(m_function, operand));
    if (callee.opcode != Opcode::PropGet || callee.isFreeName())
        return StoredKind::Other;
    LayoutSetID receivers = expectedLayoutsOf(callee.operand(*receiverOf(callee.opcode)));
    if (!receivers)
        return StoredKind::Other;

    std::span<const ValueID> arguments = call.variadicOperands();
    Node result = m_graph.addNode(m_part);
    m_part.methodCalls.append({ receivers, callee.key(), static_cast<uint32_t>(m_part.argumentsOfMethodCalls.size()), static_cast<uint32_t>(arguments.size()), result });
    for (ValueID argument : arguments)
        m_part.argumentsOfMethodCalls.append(argumentOf(argument));
    return Source::fromNode(result);
}

Source FunctionReader::readDirectCall(const Value& call)
{
    FunctionID id = call.function();
    const Function& callee = m_program.at(id);
    std::span<const ValueID> arguments = call.variadicOperands();
    unsigned first = callee.firstParameterIndex();
    unsigned numberOfParameters = callee.numberOfPositionalParameters();
    for (unsigned index = 0; index < numberOfParameters; ++index)
        m_graph.passArgument(m_part, id, index, first + index < arguments.size() ? argumentOf(arguments[first + index]) : Argument { StoredKind::Other });
    return m_graph.resultOf(id);
}

void FunctionReader::readMethodDefinition(const Value& definition)
{
    if (definition.side() != ClassSide::Prototype || definition.methodKind() != MethodKind::Method || !definition.key())
        return;
    ValueID operand = definition.operand(OperandIndex::DefineMethod::function);
    if (!operand)
        return;
    const Value& function = m_function.at(sameObjectThroughCasts(m_function, operand));
    if (function.opcode == Opcode::NewClosure)
        m_part.methods.append({ definition.layout(), definition.key(), function.function() });
}

// `f;` in the body of a class.
bool FunctionReader::isFieldDefinitionWithoutValue(const Value& store) const
{
    if (store.opcode != Opcode::PropDefine || m_function.kind != FunctionKind::Initializer)
        return false;
    ValueID receiver = store.operand(*receiverOf(store.opcode));
    ValueID stored = store.operand(*storedOf(store.opcode));
    const auto& entryParameters = m_function.blocks[0].parameters;
    std::optional<unsigned> thisIndex = m_function.thisParameterIndex();
    if (!receiver || !stored || !thisIndex || *thisIndex >= entryParameters.size())
        return false;
    return sameObjectThroughCasts(m_function, receiver) == entryParameters[*thisIndex]
        && m_function.at(sameObjectThroughCasts(m_function, stored)).opcode == Opcode::Undefined;
}

void FunctionReader::readStoreByName(const Value& store)
{
    if (store.opcode == Opcode::PropSet && store.isFreeName())
        return;
    PropertyKeyID key = store.key();
    Source stored = sourceOf(store.operand(*storedOf(store.opcode)));

    LayoutSetID receivers = expectedLayoutsOf(store.operand(*receiverOf(store.opcode)));
    if (!receivers) {
        connect(stored, m_graph.nodeOfAllStores(key));
        connect(stored, m_graph.nodeOfStoresWithoutReceiver(m_code, key));
        return;
    }

    AccessedFields fields = m_graph.accessedFields(m_part, receivers, key);
    // The constructor goes on to store what the type of the field asks for, and whoever reads the field earlier finds the marker in its slot.
    if (fields.areRequired && isFieldDefinitionWithoutValue(store))
        return;
    connect(stored, m_graph.nodeOfAllStores(key));
    if (fields.stored)
        connect(stored, *fields.stored);
}

void FunctionReader::noteUseAsInteger(ValueID id)
{
    Source source = sourceOf(id);
    if (!source.isConstant())
        m_part.usedAsIntegers.append(source.node());
}

bool FunctionReader::hasCountedElements(ValueID object) const
{
    if (!object)
        return false;
    BuiltinReceiver receiver = builtinReceiverOf(m_program, m_program.expectedOrTypeOf(m_function, object).withoutNullish());
    switch (receiver.kind) {
    case BuiltinReceiverKind::Primitive:
        return receiver.builtinClass == BuiltinClasses::String;
    case BuiltinReceiverKind::Instance:
        return receiver.builtinClass == BuiltinClasses::Array;
    case BuiltinReceiverKind::Unknown:
    case BuiltinReceiverKind::BuiltinObject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool makesInteger(BuiltinConversion conversion)
{
    return conversion == BuiltinConversion::ToIntegerOrInfinity || conversion == BuiltinConversion::ToLength || conversion == BuiltinConversion::ToIndex;
}

void FunctionReader::noteArgumentsTakenAsIntegers(const Value& call)
{
    ValueID operand = call.operand(OperandIndex::Call::callee);
    if (!operand)
        return;
    const Value& callee = m_function.at(sameObjectThroughCasts(m_function, operand));
    if (callee.opcode != Opcode::PropGet || callee.isFreeName())
        return;
    BuiltinMemberName name = builtinMemberNameOf(m_program, callee.key());
    if (!name || name.isPrivateAlias)
        return;

    const BuiltinTable& table = m_program.builtins();
    BuiltinReceiver receiver = builtinReceiverOf(m_program, m_program.expectedOrTypeOf(m_function, callee.operand(*receiverOf(callee.opcode))).withoutNullish());
    BuiltinMemberLookup found;
    switch (receiver.kind) {
    case BuiltinReceiverKind::Unknown:
        return;
    case BuiltinReceiverKind::BuiltinObject:
        found = table.findMember(receiver.object, name.name);
        break;
    case BuiltinReceiverKind::Instance:
    case BuiltinReceiverKind::Primitive:
        found = table.findForInstance(receiver.builtinClass, name.name);
        break;
    }
    if (found.outcome != BuiltinMemberLookup::Outcome::Found)
        return;

    std::span<const ValueID> arguments = call.variadicOperands();
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(found.member))) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        for (const BuiltinParameter& parameter : overload.parameters) {
            if (!makesInteger(parameter.conversion))
                continue;
            for (unsigned index = 0; index < arguments.size(); ++index) {
                if (parameter.operand.coversArgument(index))
                    noteUseAsInteger(arguments[index]);
            }
        }
    }
}

void FunctionReader::noteUsesAsIntegers(const Value& value)
{
    static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property && OperandIndex::ElemGet::property == OperandIndex::ElemDelete::property);
    switch (value.opcode) {
    case Opcode::ElemGet:
    case Opcode::ElemSet:
    case Opcode::ElemHas:
    case Opcode::ElemDelete:
        if (hasCountedElements(value.operand(*receiverOf(value.opcode))))
            noteUseAsInteger(value.operand(OperandIndex::ElemGet::property));
        return;
    case Opcode::Call:
        noteArgumentsTakenAsIntegers(value);
        return;
    default:
        return;
    }
}

void FunctionReader::readInitialValues(const Value& allocation)
{
    std::span<const ValueID> initialValues = allocation.variadicOperands();
    for (unsigned index = 0; index < initialValues.size(); ++index) {
        FieldReference field { allocation.layout(), allocation.fields()[index] };
        connect(sourceOf(initialValues[index]), m_graph.nodeOf(field));
        if (initialValues[index] && !typeFactsOf(initialValues[index]).fitsInt32Slot)
            m_part.fieldsStartedWithOtherTypes.append(m_graph.nodeOf(field));
        if (PropertyKeyID key = fieldAt(m_program, field).key)
            connect(sourceOf(initialValues[index]), m_graph.nodeOfAllStores(key));
    }
}

Source FunctionReader::read(const Value& value)
{
    if (isCast(value.opcode))
        return narrow(sourceOf(value.operand(0)), value.type);

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
    case Opcode::Div:
    case Opcode::Pow:
        return numberOutsideInt32;
    case Opcode::PropGet:
        return readPropertyRead(value);
    case Opcode::GetModuleVariable:
        return Source::fromNode(m_graph.nodeOf(value.variable()));
    case Opcode::GetEnvironmentField:
        return Source::fromNode(m_graph.nodeOf(FieldReference { value.layout(), value.field() }));
    case Opcode::Call:
    case Opcode::Construct:
    case Opcode::SuperConstruct:
        return readCall(value);
    case Opcode::CallDirect:
        return readDirectCall(value);
    case Opcode::DefineMethod:
        readMethodDefinition(value);
        return { };
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

// A value that has passed a test is the tested value.
Source FunctionReader::readEdgeResult(EdgeResultPosition position, ValueID parameter)
{
    std::optional<EdgeResult> result = tryGetEdgeResult(m_function, position);
    if (!result || result->kind != EdgeResultKind::NarrowedOperand)
        return StoredKind::Other;
    return narrow(sourceOf(result->subject), m_function.at(parameter).type);
}

// The parameters behind those that take the arguments take what the edge itself gives.
void FunctionReader::passArguments(const Target& target, NOESCAPE const Invocable<Source(uint32_t resultIndex, ValueID parameter)> auto& readResult)
{
    const auto& parameters = m_function.at(target.block).parameters;
    uint32_t numberOfArguments = target.arguments.size();
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        Source parameter = sourceOf(parameters[index]);
        if (!parameter.isConstant())
            connect(index < numberOfArguments ? sourceOf(target.arguments[index]) : readResult(index - numberOfArguments, parameters[index]), parameter.node());
    }
}

void FunctionReader::read(BlockID id)
{
    const Block& block = m_function.at(id);
    for (ValueID value : block.values) {
        m_sources[value.index()] = read(m_function.at(value));
        noteUsesAsIntegers(m_function.at(value));
    }

    const Terminator& terminator = block.terminator;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        passArguments(terminator.targets[targetIndex], [&](uint32_t resultIndex, ValueID parameter) {
            return readEdgeResult({ id, targetIndex, resultIndex }, parameter);
        });
    }
    if (block.handler) {
        passArguments(*block.handler, [](uint32_t, ValueID) {
            return Source(StoredKind::Other);
        });
    }

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
    for (uint32_t block = 0; block < m_function.blocks.size(); ++block)
        read(BlockID(block));
}

VisibleStores::VisibleStores(const Program& program, const ParallelLoops& loops)
    : VisibleStores(program, loops, [](FunctionID, unsigned) { return true; })
{
}

VisibleStores::VisibleStores(const Program& program, const ParallelLoops& loops, NOESCAPE const FunctionFilter& shouldRead)
{
    StoreGraph graph(program, loops);
    graph.build(shouldRead);
    graph.solve();
    graph.findUsesAsIntegers();
    constexpr size_t perShare = 512;
    auto forEachIndex = [&](size_t count, const auto& functor) {
        forEachIndexInParallel(loops, (count + perShare - 1) / perShare, [&](unsigned share) {
            for (size_t index = share * perShare; index < std::min(count, (share + 1) * perShare); ++index)
                functor(index);
        });
    };
    m_fieldsUsedAsIntegers = Vector<BitVector>(program.layouts.size());
    m_fieldsStartedWithOtherTypes = Vector<BitVector>(program.layouts.size());
    m_kinds = Vector<Vector<OptionSet<StoredKind>>>(program.layouts.size());
    forEachIndex(program.layouts.size(), [&](size_t layout) {
        for (uint32_t field = 0; field < program.layouts[layout].fields.size(); ++field) {
            if (graph.isUsedAsInteger({ LayoutID(layout), field }))
                m_fieldsUsedAsIntegers[layout].set(field);
            if (graph.isStartedWithOtherTypes({ LayoutID(layout), field }))
                m_fieldsStartedWithOtherTypes[layout].set(field);
        }
        m_kinds[layout] = Vector<OptionSet<StoredKind>>(program.layouts[layout].fields.size(), [&](size_t field) {
            return graph.kindsStoredIn({ LayoutID(layout), static_cast<uint32_t>(field) });
        });
    });
    m_parameterKinds = Vector<Vector<OptionSet<StoredKind>>>(program.functions.size());
    m_parametersPassedOtherTypes = Vector<BitVector>(program.functions.size());
    forEachIndex(program.functions.size(), [&](size_t function) {
        m_parameterKinds[function] = Vector<OptionSet<StoredKind>>(program.functions[function].numberOfPositionalParameters(), [&](size_t parameter) {
            return graph.kindsPassedTo(FunctionID(function), static_cast<unsigned>(parameter));
        });
        for (unsigned parameter = 0; parameter < program.functions[function].numberOfPositionalParameters(); ++parameter) {
            if (graph.isPassedOtherTypes(FunctionID(function), parameter))
                m_parametersPassedOtherTypes[function].set(parameter);
        }
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
