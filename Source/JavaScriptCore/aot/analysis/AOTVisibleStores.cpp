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

enum class CodeClass : uint8_t { Checked, Unchecked, Library };
constexpr unsigned numberOfCodeClasses = 3;
constexpr unsigned nodesPerKey = numberOfCodeClasses + 1;

constexpr unsigned maxCalleesOfCall = 8;

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
    T& operator[](size_t index) { return m_elements[index]; }
    const T& operator[](size_t index) const { return m_elements[index]; }

private:
    MallocSpan<T> m_elements;
};

class StoreSource {
public:
    StoreSource() = default;
    StoreSource(Kinds kinds)
        : m_bits(isConstantBit | kinds.toRaw())
    {
    }
    StoreSource(StoredKind kind)
        : StoreSource(Kinds { kind })
    {
    }

    static StoreSource fromNode(Node node)
    {
        StoreSource source;
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

struct StoreEdge {
    StoreSource from;
    Node to;
};

struct TypeFacts {
    LayoutSetID layouts;
    StoreSource valueSource;
    Kinds admittedKinds;
    bool isInt32 : 1 { false };
    bool fitsInt32Slot : 1 { false };
    bool mayBeBigInt : 1 { false };
};

struct Argument {
    StoreSource source;
    bool hasInt32Type { false };
};

enum class StoredKindRule : uint8_t { BitOperation, IntegerArithmetic, Narrowing };

struct Operation {
    Node result { 0 };
    StoredKindRule rule { StoredKindRule::Narrowing };
    StoreSource left;
    StoreSource right;
};

struct AccessedFields {
    StoreSource read { StoredKind::Other };
    std::optional<Node> stored;
    bool areRequired { false };
};

struct FieldWithKey {
    LayoutID layout;
    Node declaration { 0 };
    bool isRequired { false };

    friend auto operator<=>(const FieldWithKey&, const FieldWithKey&) = default;
};

struct MethodDefinition {
    LayoutID layout;
    PropertyKeyID key;
    FunctionID function;

    friend auto operator<=>(const MethodDefinition&, const MethodDefinition&) = default;
};

struct StoringMethodCall {
    LayoutSetID receivers;
    PropertyKeyID key;
    uint32_t firstArgument;
    uint32_t numberOfArguments;
    Node result;
};

struct StoreCallees {
    Vector<FunctionID, 2> functions;
    bool areAllSeen { false };
};

struct GraphPart {
    Node nextNode { 0 };
    Node endOfNodes { 0 };
    Vector<StoreEdge> edges;
    Vector<Operation> operations;
    Vector<MethodDefinition> methods;
    Vector<StoringMethodCall> methodCalls;
    Vector<Argument> argumentsOfMethodCalls;
    Vector<Node> numbersPassedOtherTypes;
    Vector<Node> fieldsStartedWithOtherTypes;
    Vector<Node> usedAsIntegers;
    Vector<StoreSource> sourcesOfValues;
};

Kinds apply(StoredKindRule rule, Kinds left, Kinds right)
{
    Kinds result;
    switch (rule) {
    case StoredKindRule::BitOperation:
        if (left - StoredKind::Other || right - StoredKind::Other)
            result.add(StoredKind::Int32);
        if (left.contains(StoredKind::Other) && right.contains(StoredKind::Other))
            result.add(StoredKind::Other);
        return result;
    case StoredKindRule::IntegerArithmetic:
        if (left.contains(StoredKind::Int32) && right.contains(StoredKind::Int32))
            result.add(StoredKind::Int32);
        if (left - StoredKind::Int32 || right - StoredKind::Int32)
            result.add(StoredKind::Other);
        if ((left | right).contains(StoredKind::NoInt32))
            result.add(StoredKind::NoInt32);
        return result;
    case StoredKindRule::Narrowing:
        return left & right;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Kinds kindsAdmittedBy(const Type& type)
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

StoreSource sourceOfValueWithType(const Type& type)
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

Kinds kindsOfData(const DataNode& node)
{
    if (const DataNode::Number* number = node.tryAs<DataNode::Number>())
        return tryConvertToStrictInt32(number->value) ? StoredKind::Int32 : StoredKind::Other;
    if (node.is<DataNode::Undefined>())
        return StoredKind::Undefined;
    return node.is<DataNode::Null>() ? StoredKind::Null : StoredKind::Other;
}

class StoreGraph {
public:
    struct alignas(64) AccessTable {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, AccessedFields> fields WTF_GUARDED_BY_LOCK(lock);
        UncheckedKeyHashMap<uint64_t, StoreCallees> methods WTF_GUARDED_BY_LOCK(lock);
    };
    static constexpr unsigned numberOfAccessTables = 256;
    static constexpr Node nodesTakenAtOnce = 256;
    static constexpr Node nodesPerShare = 1 << 13;

    void build(const VisibleStores::FunctionFilter&);
    const TypeFacts& factsOf(TypeID type) const { return m_typeFacts[type.index()]; }

    static uint64_t tableKeyOf(LayoutSetID receivers, PropertyKeyID key) { return (static_cast<uint64_t>(receivers.index()) + 1) << 32 | key.index(); }
    AccessTable& tableOf(uint64_t tableKey) { return m_accessTables[WTF::intHash(tableKey) % numberOfAccessTables]; }

    Node nodeOfStoresWithoutReceiver(CodeClass code, PropertyKeyID key) const { return m_firstKeyNode + key.index() * nodesPerKey + static_cast<unsigned>(code); }
    Node nodeOfAllStores(PropertyKeyID key) const { return m_firstKeyNode + key.index() * nodesPerKey + numberOfCodeClasses; }
    Node nodeOf(VariableID variable) const { return m_firstVariableNode + variable.index(); }
    Node nodeOfParameter(FunctionID function, unsigned parameter) const { return m_firstParameterOfFunction[function.index()] + parameter; }
    Node nodeOfResult(FunctionID function) const { return m_firstResultNode + function.index(); }

    bool holdsStoresUnderKey(Node node) const { return node >= m_firstKeyNode && node < m_firstVariableNode; }
    bool isResult(Node node) const { return node >= m_firstResultNode && node - m_firstResultNode < m_program.functions.size(); }
    bool isParameter(Node node) const { return node >= m_firstResultNode + m_program.functions.size() && node < m_numberOfPlaces; }
    bool holdsStoresWithoutReceiver(Node node) const { return node >= m_firstKeyNode && node < m_firstVariableNode && (node - m_firstKeyNode) % nodesPerKey != numberOfCodeClasses; }

    std::span<const FieldWithKey> fieldsWithKey(PropertyKeyID key) const { return m_fieldsWithKey.subspan(m_firstFieldWithKey[key.index()], m_firstFieldWithKey[key.index() + 1] - m_firstFieldWithKey[key.index()]); }
    Kinds kindsAt(Node node) const { return Kinds::fromRaw(m_kinds[node].load()); }
    Kinds seenAt(StoreSource source) const { return source.isConstant() ? source.constant() : seenAt(source.node()); }

    StoreGraph(const Program& program, const ParallelLoops& loops)
        : m_program(program)
        , m_loops(loops)
        , m_parts(loops.width())
    {
        Node numberOfNodes = 0;
        auto numberNodes = [&](size_t count, NOESCAPE const Invocable<Node(size_t)> auto& numberOfNodesAt) {
            Vector<Node> firstNodes(count);
            forEachInShares(count, 512, [&](Node index) { firstNodes[index] = numberOfNodesAt(index); });
            for (Node& first : firstNodes)
                first = std::exchange(numberOfNodes, numberOfNodes + first);
            return firstNodes;
        };
        m_firstFieldOfLayout = numberNodes(program.layouts.size(), [&](size_t index) { return static_cast<Node>(program.layouts[index].fields.size()); });
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
            forEachFieldWithKey(layoutIndex, [&](uint32_t, const Field& field) { next[field.key.index()].fetch_add(1, std::memory_order_relaxed); });
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

        m_typeFacts.grow(program.types.size());
        forEachInShares(program.types.size(), 1024, [&](Node index) {
            const Type& type = program.at(TypeID(index));
            TypeFacts& facts = m_typeFacts[index];
            facts.layouts = type.layouts;
            facts.valueSource = sourceOfValueWithType(type);
            facts.admittedKinds = kindsAdmittedBy(type);
            facts.isInt32 = type.isInt32();
            facts.fitsInt32Slot = type.isInt32OrNullish() && type.integers.isSubsetOf(Range { minInlineInt32, INT32_MAX });
            facts.mayBeBigInt = type.primitives & Primitive::BigInt;
        });
    }

    Node nodeOf(FieldReference reference) const
    {
        for (size_t steps = 0; steps < m_program.layouts.size(); ++steps) {
            const Field& field = fieldAt(m_program, reference);
            if (!field.originLayout || !field.originField)
                break;
            reference = { field.originLayout, *field.originField };
        }
        return m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    }

    CodeClass codeClassOf(const Function& function) const
    {
        if (function.role == FunctionRole::Library)
            return CodeClass::Library;
        return function.has(FunctionFlags::IsChecked) ? CodeClass::Checked : CodeClass::Unchecked;
    }

    CodeClass codeClassOf(LayoutID id) const
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

    Node addNode(GraphPart& part)
    {
        if (part.nextNode == part.endOfNodes) {
            part.nextNode = m_numberOfNodes.fetch_add(nodesTakenAtOnce, std::memory_order_relaxed);
            part.endOfNodes = part.nextNode + nodesTakenAtOnce;
        }
        return part.nextNode++;
    }

    void connect(GraphPart& part, StoreSource from, Node to)
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

    AccessedFields accessedFields(GraphPart& part, LayoutSetID receivers, PropertyKeyID key)
    {
        uint64_t tableKey = tableKeyOf(receivers, key);
        AccessTable& table = tableOf(tableKey);
        Locker locker { table.lock };
        return table.fields.ensure(tableKey, [&]() -> AccessedFields {
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
                return { StoreSource::fromNode(declarations[0]), declarations[0], areRequired };

            Node read = addNode(part);
            Node stored = addNode(part);
            for (Node declaration : declarations) {
                part.edges.append({ StoreSource::fromNode(declaration), read });
                part.edges.append({ StoreSource::fromNode(stored), declaration });
            }
            return { StoreSource::fromNode(read), stored, areRequired };
        }).iterator->value;
    }

    void passArgument(GraphPart& part, FunctionID callee, unsigned parameter, Argument argument)
    {
        connect(part, argument.source, nodeOfParameter(callee, parameter));
        if (argument.hasInt32Type)
            return;
        const Function& function = m_program.at(callee);
        const DeclaredType* declared = function.declaredEntryParameter(function.firstParameterIndex() + parameter);
        if (declared && declared->written() == TypeTable::number)
            part.numbersPassedOtherTypes.append(nodeOfParameter(callee, parameter));
    }

    void passArguments(GraphPart& part, FunctionID callee, std::span<const Argument> arguments)
    {
        unsigned numberOfParameters = m_program.at(callee).numberOfPositionalParameters();
        for (unsigned index = 0; index < numberOfParameters; ++index)
            passArgument(part, callee, index, index < arguments.size() ? arguments[index] : Argument { StoredKind::Undefined });
    }

    StoreSource resultOf(FunctionID callee) const
    {
        bool returnsItsResult = !m_program.at(callee).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
        return returnsItsResult ? StoreSource::fromNode(nodeOfResult(callee)) : StoreSource(StoredKind::Other);
    }

    std::span<const MethodDefinition> methodsOf(LayoutID layout, PropertyKeyID key) const LIFETIME_BOUND
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

    StoreCallees calledMethods(LayoutSetID receivers, PropertyKeyID key)
    {
        uint64_t tableKey = tableKeyOf(receivers, key);
        AccessTable& table = tableOf(tableKey);
        Locker locker { table.lock };
        return table.methods.ensure(tableKey, [&]() -> StoreCallees {
            const LayoutSet& family = m_program.at(receivers);
            StoreCallees callees { .areAllSeen = !family.hasMembersWithoutLayout };
            for (LayoutID member : family.members) {
                const Layout& layout = m_program.at(member);
                if (layout.flow.allocation == AllocationState::NeverAllocated)
                    continue;
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
        }).iterator->value;
    }

    void forEachShare(Node count, Node perShare, NOESCAPE const Invocable<void(unsigned share, Node first, Node end)> auto& functor) const
    {
        forEachIndexInParallel(m_loops, (count + perShare - 1) / perShare, [&](unsigned share) {
            Node first = share * perShare;
            functor(share, first, std::min(count, first + perShare));
        });
    }

    void forEachInShares(Node count, Node perShare, NOESCAPE const Invocable<void(Node)> auto& functor) const
    {
        forEachShare(count, perShare, [&](unsigned, Node first, Node end) {
            for (Node index = first; index < end; ++index)
                functor(index);
        });
    }

    Vector<uint32_t> firstPositions(FilledArray<std::atomic<uint32_t>>& next, NOESCAPE const Invocable<void(Node)> auto& visit) const
    {
        Node numberOfNodes = next.size();
        Vector<uint32_t> firstOfShare((numberOfNodes + nodesPerShare - 1) / nodesPerShare);
        forEachShare(numberOfNodes, nodesPerShare, [&](unsigned share, Node first, Node end) {
            uint32_t numberInShare = 0;
            for (Node node = first; node < end; ++node)
                numberInShare += next[node].load(std::memory_order_relaxed);
            firstOfShare[share] = numberInShare;
        });
        uint32_t numberOfEntries = 0;
        for (uint32_t& first : firstOfShare)
            first = std::exchange(numberOfEntries, numberOfEntries + first);

        Vector<uint32_t> firstOfNode(numberOfNodes + 1);
        forEachShare(numberOfNodes, nodesPerShare, [&](unsigned share, Node first, Node end) {
            uint32_t position = firstOfShare[share];
            for (Node node = first; node < end; ++node) {
                visit(node);
                firstOfNode[node] = position;
                position += next[node].exchange(position, std::memory_order_relaxed);
            }
        });
        firstOfNode[numberOfNodes] = numberOfEntries;
        return firstOfNode;
    }

    Kinds seenAt(Node node) const
    {
        Kinds kinds = kindsAt(node);
        if (!kinds && m_unseenSourcesAreOther && node < m_numberOfPlaces && !holdsStoresWithoutReceiver(node))
            return StoredKind::Other;
        return kinds;
    }

    Kinds leaving(StoreSource source) const
    {
        Kinds kinds = seenAt(source);
        return !source.isConstant() && source.node() < m_numberOfPlaces ? kinds - StoredKind::NoInt32 : kinds;
    }

    void enqueue(Vector<Node>& worklist, Node node)
    {
        if (!m_isEnqueued[node].exchange(1))
            worklist.append(node);
    }

    void join(Vector<Node>& worklist, Node node, Kinds kinds)
    {
        if (Kinds::fromRaw(m_kinds[node].load(std::memory_order_relaxed)).containsAll(kinds))
            return;
        m_kinds[node].fetch_or(kinds.toRaw());
        enqueue(worklist, node);
    }

    void propagate(Vector<Node>& worklist)
    {
        while (!worklist.isEmpty()) {
            Node node = worklist.takeLast();
            m_isEnqueued[node].store(0);
            Kinds kinds = leaving(StoreSource::fromNode(node));

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

    void propagateFrom(Node endOfSeeds, NOESCAPE const Invocable<bool(Node)> auto& isSeed)
    {
        forEachShare(endOfSeeds, nodesPerShare, [&](unsigned, Node first, Node end) {
            Vector<Node> worklist;
            for (Node node = first; node < end; ++node) {
                if (isSeed(node))
                    enqueue(worklist, node);
            }
            propagate(worklist);
        });
    }

    void solve()
    {
        m_isEnqueued = FilledArray<std::atomic<uint8_t>>(m_loops, m_kinds.size());
        propagateFrom(m_kinds.size(), [&](Node node) { return !!kindsAt(node); });

        m_unseenSourcesAreOther = true;
        propagateFrom(m_numberOfPlaces, [&](Node place) { return !kindsAt(place) && !holdsStoresWithoutReceiver(place); });
    }

    void findUsesAsIntegers()
    {
        constexpr uint8_t mayReachCallers = 1 << 0;
        constexpr uint8_t cameThroughResult = 1 << 1;

        Node numberOfNodes = m_kinds.size();
        m_usesAsIntegers = FilledArray<std::atomic<uint8_t>>(m_loops, numberOfNodes);
        if (m_usedAsIntegers.isEmpty())
            return;

        FilledArray<std::atomic<uint32_t>> next(m_loops, numberOfNodes);
        forEachShare(m_successors.size(), nodesPerShare, [&](unsigned, Node first, Node end) {
            for (Node successor : m_successors.subspan(first, end - first))
                next[successor].fetch_add(1, std::memory_order_relaxed);
        });
        Vector<uint32_t> firstPredecessor = firstPositions(next, [](Node) { });
        Vector<Node> predecessors(m_successors.size());
        forEachInShares(numberOfNodes, nodesPerShare, [&](Node node) {
            for (Node successor : m_successors.subspan(m_firstSuccessor[node], m_firstSuccessor[node + 1] - m_firstSuccessor[node]))
                predecessors[next[successor].fetch_add(1, std::memory_order_relaxed)] = node;
        });

        forEachShare(m_usedAsIntegers.size(), 1 << 10, [&](unsigned, Node first, Node end) {
            Vector<Node> worklist;
            auto mark = [&](Node node, uint8_t ways) {
                if ((m_usesAsIntegers[node].load(std::memory_order_relaxed) & ways) == ways)
                    return;
                if ((m_usesAsIntegers[node].fetch_or(ways) & ways) != ways)
                    worklist.append(node);
            };
            for (Node node : m_usedAsIntegers.subspan(first, end - first))
                mark(node, mayReachCallers);
            while (!worklist.isEmpty()) {
                Node node = worklist.takeLast();
                uint8_t ways = m_usesAsIntegers[node].load();
                if (holdsStoresUnderKey(node))
                    continue;
                if (node >= m_numberOfPlaces) {
                    uint32_t operationIndex = m_operationOfValue[node - m_numberOfPlaces];
                    if (operationIndex != UINT32_MAX && m_operations[operationIndex].rule == StoredKindRule::BitOperation)
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
    bool m_unseenSourcesAreOther { false };
};

class StoreFunctionReader {
public:
    StoreFunctionReader(StoreGraph& graph, GraphPart& part, FunctionID id)
        : m_graph(graph)
        , m_part(part)
        , m_program(graph.m_program)
        , m_id(id)
        , m_function(m_program.at(id))
        , m_code(graph.codeClassOf(m_function))
        , m_sources(part.sourcesOfValues)
    {
    }

    StoreSource sourceOf(ValueID id) const { return id ? m_sources[id.index()] : StoreSource(StoredKind::Other); }
    const TypeFacts& typeFactsOf(ValueID id) const { return m_graph.factsOf(m_function.at(id).type); }
    LayoutSetID expectedLayoutsOf(ValueID id) const { return m_graph.factsOf(m_function.at(id).expectedOrType().predicted()).layouts; }
    Argument argumentOf(ValueID id) const { return { sourceOf(id), id && typeFactsOf(id).isInt32 }; }
    void connect(StoreSource from, Node to) { m_graph.connect(m_part, from, to); }

    StoreSource readOperation(StoredKindRule rule, const Value& value)
    {
        StoreSource left = sourceOf(value.operand(0));
        StoreSource right = value.numberOfOperands() > 1 ? sourceOf(value.operand(1)) : left;
        if (left.isConstant() && right.isConstant())
            return apply(rule, left.constant(), right.constant());
        if (Kinds decided = apply(rule, left.constantOrNothing(), right.constantOrNothing()))
            return decided;

        Node result = m_graph.addNode(m_part);
        m_part.operations.append({ result, rule, left, right });
        if (!left.isConstant())
            m_part.edges.append({ left, result });
        if (!right.isConstant() && right.node() != left.node())
            m_part.edges.append({ right, result });
        return StoreSource::fromNode(result);
    }

    StoreSource narrow(StoreSource source, TypeID type)
    {
        if (!type)
            return source;
        Kinds admitted = m_graph.factsOf(type).admittedKinds;
        if (source.isConstant())
            return source.constant() & admitted;
        if (admitted == allKinds)
            return source;
        if (!(admitted - StoredKind::Other))
            return admitted;

        Node result = m_graph.addNode(m_part);
        m_part.operations.append({ result, StoredKindRule::Narrowing, source, admitted });
        m_part.edges.append({ source, result });
        return StoreSource::fromNode(result);
    }

    bool forEachCallee(const Value& call, NOESCAPE const Invocable<void(FunctionID)> auto& functor) const
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

        const Type& type = m_program.expectedOrTypeOf(m_function, operand);
        bool listsOnlyFunctions = !type.anyObject && type.classes.isEmpty() && !type.layouts && type.builtins.isEmpty();
        if (!listsOnlyFunctions || type.functions.isEmpty() || type.functions.size() > maxCalleesOfCall)
            return false;
        for (FunctionID function : type.functions)
            functor(function);
        return true;
    }

    StoreSource readCall(const Value& call)
    {
        Vector<Argument, 8> arguments = WTF::map<8>(call.variadicOperands(), [&](ValueID argument) { return argumentOf(argument); });
        Vector<StoreSource, maxCalleesOfCall> results;
        bool seesEveryCallee = forEachCallee(call, [&](FunctionID callee) {
            m_graph.passArguments(m_part, callee, arguments.span());
            results.append(m_graph.resultOf(callee));
        });
        if (call.opcode != Opcode::Call)
            return StoredKind::Other;
        if (!seesEveryCallee)
            return readMethodCall(call);
        if (results.size() == 1)
            return results[0];
        Node result = m_graph.addNode(m_part);
        for (StoreSource source : results)
            connect(source, result);
        return StoreSource::fromNode(result);
    }

    StoreSource readMethodCall(const Value& call)
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
        return StoreSource::fromNode(result);
    }

    void readMethodDefinition(const Value& definition)
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

    bool isFieldDefinitionWithoutValue(const Value& store) const
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

    void readStoreByName(const Value& store)
    {
        if (store.opcode == Opcode::PropSet && store.isFreeName())
            return;
        PropertyKeyID key = store.key();
        StoreSource stored = sourceOf(store.operand(*storedOf(store.opcode)));

        LayoutSetID receivers = expectedLayoutsOf(store.operand(*receiverOf(store.opcode)));
        if (!receivers) {
            connect(stored, m_graph.nodeOfAllStores(key));
            connect(stored, m_graph.nodeOfStoresWithoutReceiver(m_code, key));
            return;
        }

        AccessedFields fields = m_graph.accessedFields(m_part, receivers, key);
        if (fields.areRequired && isFieldDefinitionWithoutValue(store))
            return;
        connect(stored, m_graph.nodeOfAllStores(key));
        if (fields.stored)
            connect(stored, *fields.stored);
    }

    void noteUseAsInteger(ValueID id)
    {
        StoreSource source = sourceOf(id);
        if (!source.isConstant())
            m_part.usedAsIntegers.append(source.node());
    }

    bool hasCountedElements(ValueID object) const
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

    void noteArgumentsTakenAsIntegers(const Value& call)
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
                if (parameter.conversion != BuiltinConversion::ToIntegerOrInfinity && parameter.conversion != BuiltinConversion::ToLength && parameter.conversion != BuiltinConversion::ToIndex)
                    continue;
                for (unsigned index = 0; index < arguments.size(); ++index) {
                    if (parameter.operand.coversArgument(index))
                        noteUseAsInteger(arguments[index]);
                }
            }
        }
    }

    StoreSource read(const Value& value)
    {
        static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property && OperandIndex::ElemGet::property == OperandIndex::ElemDelete::property);
        if (isCast(value.opcode))
            return narrow(sourceOf(value.operand(0)), value.type);

        switch (value.opcode) {
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::BitNot:
        case Opcode::LShift:
        case Opcode::RShift: {
            bool hasOperandThatIsNoBigInt = false;
            value.forEachOperand([&](ValueID operand, uint32_t) { hasOperandThatIsNoBigInt |= !typeFactsOf(operand).mayBeBigInt; });
            return hasOperandThatIsNoBigInt ? StoreSource(StoredKind::Int32) : readOperation(StoredKindRule::BitOperation, value);
        }
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Increment:
        case Opcode::Decrement:
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
            return readOperation(StoredKindRule::IntegerArithmetic, value);
        case Opcode::Div:
        case Opcode::Pow:
            return numberOutsideInt32;
        case Opcode::PropGet: {
            if (value.isFreeName())
                return StoredKind::Other;
            LayoutSetID receivers = expectedLayoutsOf(value.operand(*receiverOf(value.opcode)));
            if (!receivers)
                return StoreSource::fromNode(m_graph.nodeOfAllStores(value.key()));
            return m_graph.accessedFields(m_part, receivers, value.key()).read;
        }
        case Opcode::GetModuleVariable:
            return StoreSource::fromNode(m_graph.nodeOf(value.variable()));
        case Opcode::GetEnvironmentField:
            return StoreSource::fromNode(m_graph.nodeOf(FieldReference { value.layout(), value.field() }));
        case Opcode::Call:
            noteArgumentsTakenAsIntegers(value);
            [[fallthrough]];
        case Opcode::Construct:
        case Opcode::SuperConstruct:
            return readCall(value);
        case Opcode::CallDirect: {
            FunctionID id = value.function();
            const Function& callee = m_program.at(id);
            std::span<const ValueID> arguments = value.variadicOperands();
            unsigned first = callee.firstParameterIndex();
            unsigned numberOfParameters = callee.numberOfPositionalParameters();
            for (unsigned index = 0; index < numberOfParameters; ++index)
                m_graph.passArgument(m_part, id, index, first + index < arguments.size() ? argumentOf(arguments[first + index]) : Argument { StoredKind::Other });
            return m_graph.resultOf(id);
        }
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
        case Opcode::NewObject: {
            std::span<const ValueID> initialValues = value.variadicOperands();
            for (unsigned index = 0; index < initialValues.size(); ++index) {
                FieldReference field { value.layout(), value.fields()[index] };
                connect(sourceOf(initialValues[index]), m_graph.nodeOf(field));
                if (initialValues[index] && !typeFactsOf(initialValues[index]).fitsInt32Slot)
                    m_part.fieldsStartedWithOtherTypes.append(m_graph.nodeOf(field));
                if (PropertyKeyID key = fieldAt(m_program, field).key)
                    connect(sourceOf(initialValues[index]), m_graph.nodeOfAllStores(key));
            }
            return StoredKind::Other;
        }
        case Opcode::ElemGet:
        case Opcode::ElemSet:
        case Opcode::ElemHas:
        case Opcode::ElemDelete:
            if (hasCountedElements(value.operand(*receiverOf(value.opcode))))
                noteUseAsInteger(value.operand(OperandIndex::ElemGet::property));
            [[fallthrough]];
        default:
            return value.type ? m_graph.factsOf(value.type).valueSource : StoreSource();
        }
    }

    void passArguments(const Target& target, NOESCAPE const Invocable<StoreSource(uint32_t resultIndex, ValueID parameter)> auto& readResult)
    {
        const auto& parameters = m_function.at(target.block).parameters;
        uint32_t numberOfArguments = target.arguments.size();
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            StoreSource parameter = sourceOf(parameters[index]);
            if (!parameter.isConstant())
                connect(index < numberOfArguments ? sourceOf(target.arguments[index]) : readResult(index - numberOfArguments, parameters[index]), parameter.node());
        }
    }

    void run()
    {
        m_sources.fill(StoreSource(), m_function.values.size());
        const auto& entryParameters = m_function.blocks[0].parameters;
        unsigned first = m_function.firstParameterIndex();
        unsigned numberOfPositionalParameters = m_function.numberOfPositionalParameters();
        for (unsigned index = 0; index < entryParameters.size(); ++index) {
            bool isPositional = index >= first && index - first < numberOfPositionalParameters;
            m_sources[entryParameters[index].index()] = isPositional ? StoreSource::fromNode(m_graph.nodeOfParameter(m_id, index - first)) : StoreSource(StoredKind::Other);
        }
        for (const Block& block : m_function.blocks.subspan(1)) {
            for (ValueID parameter : block.parameters)
                m_sources[parameter.index()] = StoreSource::fromNode(m_graph.addNode(m_part));
        }
        for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
            BlockID id(blockIndex);
            const Block& block = m_function.at(id);
            for (ValueID value : block.values)
                m_sources[value.index()] = read(m_function.at(value));

            const Terminator& terminator = block.terminator;
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                passArguments(terminator.targets[targetIndex], [&](uint32_t resultIndex, ValueID parameter) -> StoreSource {
                    std::optional<EdgeResult> result = tryGetEdgeResult(m_function, EdgeResultPosition { id, targetIndex, resultIndex });
                    if (!result || result->kind != EdgeResultKind::NarrowedOperand)
                        return StoredKind::Other;
                    return narrow(sourceOf(result->subject), m_function.at(parameter).type);
                });
            }
            if (block.handler)
                passArguments(*block.handler, [](uint32_t, ValueID) { return StoreSource(StoredKind::Other); });

            if (terminator.kind == TerminatorKind::Return) {
                ValueID result = terminator.operand(OperandIndex::Return::value);
                connect(result ? sourceOf(result) : StoreSource(StoredKind::Undefined), m_graph.nodeOfResult(m_id));
            } else if (terminator.kind == TerminatorKind::ReturnCall)
                connect(sourceOf(terminator.operand(OperandIndex::ReturnCall::call)), m_graph.nodeOfResult(m_id));
        }
    }

    StoreGraph& m_graph;
    GraphPart& m_part;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    CodeClass m_code;
    Vector<StoreSource>& m_sources;
};

} // anonymous namespace

void StoreGraph::build(const VisibleStores::FunctionFilter& shouldRead)
{
    forEachIndexInParallel(m_loops, m_program.data.size(), [&](unsigned index, unsigned worker) {
        GraphPart& part = m_parts[worker];
        forEachDataNodeInPreorder(m_program.data[index].root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (const DataMember& member : object->members) {
                connect(part, kindsOfData(member.value), nodeOfAllStores(member.key));
                if (std::optional<uint32_t> field = object->layout ? m_program.at(object->layout).findField(member.key) : std::nullopt)
                    connect(part, kindsOfData(member.value), nodeOf({ object->layout, *field }));
            }
        });
    });

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
            StoreFunctionReader(*this, m_parts[worker], id).run();
    });

    for (GraphPart& part : m_parts)
        m_methods.appendVector(std::exchange(part.methods, { }));
    AOT::sort(m_methods);
    removeRepeatedElements(m_methods);
    forEachIndexInParallel(m_loops, m_parts.size(), [&](unsigned index) {
        GraphPart& part = m_parts[index];
        for (const StoringMethodCall& call : part.methodCalls) {
            StoreCallees callees = calledMethods(call.receivers, call.key);
            std::span<const Argument> arguments = part.argumentsOfMethodCalls.subspan(call.firstArgument, call.numberOfArguments);
            for (FunctionID callee : callees.functions) {
                passArguments(part, callee, arguments);
                connect(part, resultOf(callee), call.result);
            }
            if (!callees.areAllSeen)
                connect(part, StoredKind::Other, call.result);
        }
    });
    m_numbersPassedOtherTypes = BitVector(m_numberOfPlaces);
    m_fieldsStartedWithOtherTypes = BitVector(m_numberOfPlaces);
    for (const GraphPart& part : m_parts) {
        for (Node parameter : part.numbersPassedOtherTypes)
            m_numbersPassedOtherTypes.quickSet(parameter);
        for (Node field : part.fieldsStartedWithOtherTypes)
            m_fieldsStartedWithOtherTypes.quickSet(field);
    }

    Node numberOfNodes = m_numberOfNodes.load(std::memory_order_relaxed);
    Node numberOfValues = numberOfNodes - m_numberOfPlaces;

    FilledArray<std::atomic<uint8_t>> constantsOfValues(m_loops, numberOfValues);
    FilledArray<std::atomic<uint32_t>> next(m_loops, numberOfNodes);
    forEachIndexInParallel(m_loops, m_parts.size(), [&](unsigned index) {
        for (const StoreEdge& edge : m_parts[index].edges) {
            if (edge.from.isConstant())
                constantsOfValues[edge.to - m_numberOfPlaces].fetch_or(edge.from.constant().toRaw(), std::memory_order_relaxed);
            else
                next[edge.from.node()].fetch_add(1, std::memory_order_relaxed);
        }
    });

    m_kinds = FilledArray<std::atomic<uint8_t>>::createWithoutFilling(numberOfNodes);
    m_firstSuccessor = firstPositions(next, [&](Node node) {
        const std::atomic<uint8_t>& constants = node < m_numberOfPlaces ? m_constantsStoredInPlaces[node] : constantsOfValues[node - m_numberOfPlaces];
        m_kinds[node].store(constants.load(std::memory_order_relaxed), std::memory_order_relaxed);
    });

    for (const GraphPart& part : m_parts)
        m_usedAsIntegers.appendVector(part.usedAsIntegers);

    uint32_t numberOfOperations = 0;
    Vector<uint32_t> firstOperationOfPart = WTF::map(m_parts, [&](const GraphPart& part) {
        uint32_t first = numberOfOperations;
        numberOfOperations += part.operations.size();
        return first;
    });

    m_successors = Vector<Node>(m_firstSuccessor.last());
    m_operationOfValue = FilledArray<uint32_t>(m_loops, numberOfValues, 0xff);
    m_operations = FilledArray<Operation>::createWithoutFilling(numberOfOperations);
    forEachIndexInParallel(m_loops, m_parts.size(), [&](unsigned index) {
        GraphPart& part = m_parts[index];
        for (const StoreEdge& edge : part.edges) {
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

VisibleStores::VisibleStores(const Program& program, const ParallelLoops& loops, NOESCAPE const FunctionFilter& shouldRead)
{
    StoreGraph graph(program, loops);
    graph.build(shouldRead);
    graph.solve();
    graph.findUsesAsIntegers();
    m_fieldsUsedAsIntegers = Vector<BitVector>(program.layouts.size());
    m_fieldsStartedWithOtherTypes = Vector<BitVector>(program.layouts.size());
    m_kinds = Vector<Vector<Kinds>>(program.layouts.size());
    graph.forEachInShares(program.layouts.size(), 512, [&](Node layout) {
        m_kinds[layout] = Vector<Kinds>(program.layouts[layout].fields.size(), [&](size_t field) {
            Node node = graph.nodeOf({ LayoutID(layout), static_cast<uint32_t>(field) });
            if (graph.m_usesAsIntegers[node].load(std::memory_order_relaxed))
                m_fieldsUsedAsIntegers[layout].set(field);
            if (graph.m_fieldsStartedWithOtherTypes.quickGet(node))
                m_fieldsStartedWithOtherTypes[layout].set(field);
            return graph.kindsAt(node);
        });
    });
    m_parameterKinds = Vector<Vector<Kinds>>(program.functions.size());
    m_parametersPassedOtherTypes = Vector<BitVector>(program.functions.size());
    graph.forEachInShares(program.functions.size(), 512, [&](Node function) {
        m_parameterKinds[function] = Vector<Kinds>(program.functions[function].numberOfPositionalParameters(), [&](size_t parameter) {
            Node node = graph.nodeOfParameter(FunctionID(function), static_cast<unsigned>(parameter));
            if (graph.m_numbersPassedOtherTypes.quickGet(node))
                m_parametersPassedOtherTypes[function].set(parameter);
            return graph.kindsAt(node);
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
