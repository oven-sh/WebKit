/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTNameClass.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTUseLists.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Deque.h>

namespace JSC { namespace AOT { namespace Typed {

// What defines a property of an object literal behind its allocation. Each takes the allocation as this operand.
static constexpr OpcodeSet literalPieceOperations { Opcode::PropDefine, Opcode::ElemDefine, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::CopyDataProperties };
static constexpr uint32_t literalOperandOfPiece = OperandIndex::FinishObject::object;

static_assert(OperandIndex::PropDefine::object == literalOperandOfPiece && OperandIndex::ElemDefine::object == literalOperandOfPiece);
static_assert(OperandIndex::DefineGetter::object == literalOperandOfPiece && OperandIndex::DefineSetter::object == literalOperandOfPiece);
static_assert(OperandIndex::CopyDataProperties::target == literalOperandOfPiece);

namespace {

using KeyOrder = Vector<PropertyKeyID>;
using KeyOrders = Vector<KeyOrder, 2>;

enum class PieceKind : uint8_t { DefinesUnknownProperty, DefinesKey, CopiesProperties };

struct LiteralPiece {
    PieceKind kind { PieceKind::DefinesUnknownProperty };
    PropertyKeyID key;
    ValueID operation;
};

struct Literal {
    FunctionID function;
    ValueID allocation;
    Vector<LiteralPiece, 4> pieces;
};

struct CopiedObjects {
    Vector<LayoutID, 2> layouts;
    bool mayHaveNoKeys { false };
};

// What one literal creates.
struct Creation {
    uint32_t literal { 0 };
    LayoutID layout;
    Vector<CopiedObjects, 1> copies;
    KeyOrders orders;
    bool hasOrdersLeftOut { false };
};

struct CountedKeyOrder {
    const KeyOrder* order { nullptr };
    size_t numberOfCreations { 0 };
};

// The orders are what a walk over own keys tests for first. Nothing rests on their being all the orders that there are.
class LiteralOrderSolver {
public:
    LiteralOrderSolver(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, Vector<PropertyKeyKind>&& kindsOfKeys, Vector<FlowLayoutUpdate>& layouts)
        : m_inputs(inputs)
        , m_heap(heap)
        , m_layouts(layouts)
        , m_maxOrders(inputs.build.limit(LimitID::MaxExpandedKeyOrders))
        , m_creationsOfLayout(layouts.size())
        , m_readersOfLayout(layouts.size())
        , m_literalOrdersOfLayout(layouts.size())
        , m_kindOfKey(WTF::move(kindsOfKeys))
    {
    }

    void run();

private:
    const Program& program() const { return m_inputs.program; }
    const LayoutFlow& flowOf(LayoutID layout) const { return m_layouts[layout.index()].flow; }
    LayoutID provenBaseOf(LayoutID layout) const { return flowOf(layout).baseState == BaseState::Layout ? flowOf(layout).base : LayoutID(); }

    void collectCreations();
    void addCreationOf(uint32_t literal);
    CopiedObjects objectsCopiedBy(FunctionID, ValueID copy);
    void registerReader(uint32_t creation);
    void solve();
    enum class Progress : uint8_t { NothingToRead, MoreToRead };
    Progress evaluate(Creation&);
    void leaveOrdersOut(Creation&);
    void leaveOrdersUnread(LayoutID);

    std::optional<KeyOrder> keysAtAllocation(const Literal&) const;
    KeyOrders copyInto(const KeyOrders&, const CopiedObjects&, const Value& copy);
    KeyOrders ordersOf(LayoutID);
    KeyOrders ownOrdersOf(LayoutID);
    bool isCopied(LayoutID source, PropertyKeyID, const Value& copy) const;
    void appendCopiedKeys(LayoutID source, const KeyOrder& sourceOrder, const Value& copy, KeyOrder& target) const;
    void addOrder(KeyOrders&, KeyOrder&&) const;
    Vector<KeyOrder> sortedOrdersOf(LayoutID) const;

    const ValueFlowResultInputs& m_inputs;
    ValueFlowHeapView& m_heap;
    Vector<FlowLayoutUpdate>& m_layouts;
    uint64_t m_maxOrders;
    Vector<Literal> m_literals;
    Vector<Creation> m_creations;
    Vector<Vector<uint32_t>> m_creationsOfLayout;
    Vector<Vector<uint32_t>> m_readersOfLayout;
    // What a copy from an object of the layout reads beside the constructor orders: the first distinct orders that the creations of the layout got.
    Vector<KeyOrders> m_literalOrdersOfLayout;
    BitVector m_hasOrdersUnread;
    Vector<PropertyKeyKind> m_kindOfKey;
};

} // anonymous namespace

static void appendIfAbsent(KeyOrder& order, PropertyKeyID key)
{
    if (!order.contains(key))
        order.append(key);
}

static LiteralPiece describePiece(ValueID id, const Value& operation)
{
    switch (operation.opcode) {
    case Opcode::PropDefine:
        return { PieceKind::DefinesKey, operation.key(), id };
    case Opcode::CopyDataProperties:
        if (operation.excludedOperands().empty())
            return { PieceKind::CopiesProperties, { }, id };
        return { PieceKind::DefinesUnknownProperty, { }, id };
    case Opcode::ElemDefine:
    case Opcode::DefineGetter:
    case Opcode::DefineSetter:
        return { PieceKind::DefinesUnknownProperty, { }, id };
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static void collectLiteralsOf(const ValueFlowResultInputs& inputs, FunctionID id, Vector<Literal>& literals)
{
    const Function& function = inputs.program.at(id);
    if (!inputs.isReached(id))
        return;
    if (!inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::FinishObject })))
        return;

    CFG cfg(function);
    BitVector isFinished(function.values.size());
    BitVector hasPieces(function.values.size());
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            const Value& operation = function.at(value);
            bool finishes = operation.opcode == Opcode::FinishObject;
            if (!finishes && !literalPieceOperations.contains(operation.opcode))
                continue;
            if (ValueID literal = operation.operand(literalOperandOfPiece))
                (finishes ? isFinished : hasPieces).quickSet(literal.index());
        }
    }
    size_t first = literals.size();
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (function.at(value).opcode == Opcode::NewObject && isFinished.quickGet(value.index()) && hasPieces.quickGet(value.index()))
                literals.append(Literal { id, value, { } });
        }
    }
    std::span<Literal> literalsOfFunction = literals.mutableSpan().subspan(first);
    AOT::sort(literalsOfFunction, { }, &Literal::allocation);
    if (literalsOfFunction.empty())
        return;

    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            const Value& operation = function.at(value);
            if (!literalPieceOperations.contains(operation.opcode))
                continue;
            auto literal = std::ranges::find(literalsOfFunction, operation.operand(literalOperandOfPiece), &Literal::allocation);
            if (literal != literalsOfFunction.end())
                literal->pieces.append(describePiece(value, operation));
        }
    }
}

CopiedObjects LiteralOrderSolver::objectsCopiedBy(FunctionID function, ValueID copy)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    CopiedObjects copied;
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statementsOf(function.index());
    auto statement = std::ranges::find_if(statements, [&](const ValueFlowStatement& candidate) {
        return candidate.kind == ValueFlowStatementKind::Operation && candidate.site == copy;
    });
    if (statement == statements.end()) {
        copied.mayHaveNoKeys = true;
        return copied;
    }

    ValueSetID sources = m_heap.valueOf(m_inputs.bodies().operandsOf(*statement)[OperandIndex::CopyDataProperties::source]);
    if (ValueSets::isComplement(sources))
        return copied;
    bool holdsPlaceholder = m_inputs.sets().meet(sources, universe.placeholdersAlone());
    copied.mayHaveNoKeys = !holdsPlaceholder && m_inputs.sets().meet(sources, universe.primitives());
    m_inputs.sets().forEachMember(sources, [&](ValueFlowObjectID object) {
        ValueFlowObjectKind kind = universe.kindOf(object);
        if (kind != ValueFlowObjectKind::Site && kind != ValueFlowObjectKind::Instance)
            return;
        LayoutID layout = universe.layoutOf(object);
        if (layout && hasObjectsThatCodeAlters(program().at(layout).kind))
            copied.layouts.append(layout);
    });
    AOT::sort(copied.layouts);
    removeRepeatedElements(copied.layouts);
    return copied;
}

void LiteralOrderSolver::addCreationOf(uint32_t index)
{
    const Literal& literal = m_literals[index];
    LayoutID layout = program().at(literal.function).at(literal.allocation).layout();
    if (!hasObjectsThatCodeAlters(program().at(layout).kind) || flowOf(layout).allocationClasses.isEmpty() || !m_inputs.build.isEnabled(FlowResultKind::ConstructorFieldOrders, layout.index()))
        return;
    // Nothing creates a field of such a layout behind an allocation, so the pieces add no key, and the orders of the allocations are all there are.
    if (flowOf(layout).fieldCreation == FieldCreation::OnlyAtConstruction)
        return;

    Creation creation { .literal = index, .layout = layout };
    for (const LiteralPiece& piece : literal.pieces) {
        if (piece.kind == PieceKind::CopiesProperties)
            creation.copies.append(objectsCopiedBy(literal.function, piece.operation));
    }
    m_creations.append(WTF::move(creation));
}

void LiteralOrderSolver::registerReader(uint32_t index)
{
    const Creation& creation = m_creations[index];
    m_creationsOfLayout[creation.layout.index()].append(index);
    for (const CopiedObjects& copied : creation.copies) {
        for (LayoutID source : copied.layouts) {
            for (LayoutID current = source; current; current = provenBaseOf(current)) {
                Vector<uint32_t>& readers = m_readersOfLayout[current.index()];
                if (readers.isEmpty() || readers.last() != index)
                    readers.append(index);
            }
        }
    }
}

void LiteralOrderSolver::collectCreations()
{
    unsigned numberOfFunctions = static_cast<unsigned>(program().functions.size());
    m_literals = collectInParallel<Literal>(m_inputs.loops(), numberOfFunctions, [&](unsigned index, Vector<Literal>& literals) {
        collectLiteralsOf(m_inputs, FunctionID(index), literals);
    });
    for (uint32_t index = 0; index < m_literals.size(); ++index)
        addCreationOf(index);
    for (uint32_t index = 0; index < m_creations.size(); ++index)
        registerReader(index);
}

std::optional<KeyOrder> LiteralOrderSolver::keysAtAllocation(const Literal& literal) const
{
    const Value& allocation = program().at(literal.function).at(literal.allocation);
    const Vector<Field>& fields = program().at(allocation.layout()).fields;
    KeyOrder keys;
    for (uint32_t field : allocation.fields()) {
        if (!fields[field].key)
            return std::nullopt;
        keys.append(fields[field].key);
    }
    return keys;
}

void LiteralOrderSolver::addOrder(KeyOrders& orders, KeyOrder&& order) const
{
    if (!orders.contains(order))
        orders.append(WTF::move(order));
}

void LiteralOrderSolver::leaveOrdersUnread(LayoutID layout)
{
    if (!m_hasOrdersUnread.set(layout.index()))
        m_inputs.build.gaveUp(LimitID::MaxExpandedKeyOrders, Subject { .layout = layout });
}

// No walk over own keys tests more orders than the limit, so no copy reads more of them.
KeyOrders LiteralOrderSolver::ownOrdersOf(LayoutID id)
{
    const Vector<Field>& fields = program().at(id).fields;
    KeyOrders orders;
    for (const FieldOrder& order : flowOf(id).constructorOrders) {
        if (orders.size() >= m_maxOrders) {
            leaveOrdersUnread(id);
            return orders;
        }
        bool hasOnlyConstantKeys = std::ranges::all_of(order.fields, [&](uint32_t field) {
            return !!fields[field].key;
        });
        if (!hasOnlyConstantKeys)
            continue;
        addOrder(orders, order.fields.map<KeyOrder>([&](uint32_t field) {
            return fields[field].key;
        }));
    }
    for (const KeyOrder& order : m_literalOrdersOfLayout[id.index()]) {
        if (orders.size() >= m_maxOrders) {
            leaveOrdersUnread(id);
            return orders;
        }
        addOrder(orders, KeyOrder(order));
    }
    return orders;
}

// The keys of an object are those that the constructor of its base creates, then those of its own layout.
KeyOrders LiteralOrderSolver::ordersOf(LayoutID id)
{
    KeyOrders ownOrders = ownOrdersOf(id);
    switch (flowOf(id).baseState) {
    case BaseState::None:
        return ownOrders;
    case BaseState::Layout:
        break;
    case BaseState::BuiltinClass:
    case BaseState::Unknown:
        return { };
    }

    KeyOrders orders;
    for (const KeyOrder& baseOrder : ordersOf(flowOf(id).base)) {
        for (const KeyOrder& ownOrder : ownOrders) {
            if (orders.size() >= m_maxOrders) {
                leaveOrdersUnread(id);
                return orders;
            }
            KeyOrder order = baseOrder;
            for (PropertyKeyID key : ownOrder)
                appendIfAbsent(order, key);
            addOrder(orders, WTF::move(order));
        }
    }
    return orders;
}

bool LiteralOrderSolver::isCopied(LayoutID source, PropertyKeyID key, const Value& copy) const
{
    if (std::ranges::contains(copy.excludedKeys(), key))
        return false;
    for (LayoutID current = source; current; current = provenBaseOf(current)) {
        const Layout& layout = program().at(current);
        if (std::optional<uint32_t> field = layout.findField(key))
            return !layout.fields[*field].flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden });
    }
    return true;
}

// [[OwnPropertyKeys]] lists strings in the order of their creation, then symbols.
void LiteralOrderSolver::appendCopiedKeys(LayoutID source, const KeyOrder& sourceOrder, const Value& copy, KeyOrder& target) const
{
    for (PropertyKeyID key : sourceOrder) {
        if (m_kindOfKey[key.index()] == PropertyKeyKind::Text && isCopied(source, key, copy))
            appendIfAbsent(target, key);
    }
    for (PropertyKeyID key : sourceOrder) {
        if (isSymbolKind(m_kindOfKey[key.index()]) && isCopied(source, key, copy))
            appendIfAbsent(target, key);
    }
}

KeyOrders LiteralOrderSolver::copyInto(const KeyOrders& orders, const CopiedObjects& copied, const Value& copy)
{
    KeyOrders result;
    for (LayoutID source : copied.layouts) {
        for (const KeyOrder& sourceOrder : ordersOf(source)) {
            for (const KeyOrder& order : orders) {
                KeyOrder extended = order;
                appendCopiedKeys(source, sourceOrder, copy, extended);
                addOrder(result, WTF::move(extended));
            }
        }
        // The caller keeps the first orders up to the limit, so a copy from any number of layouts costs no more than one from a few.
        if (result.size() > m_maxOrders)
            return result;
    }
    if (copied.mayHaveNoKeys) {
        for (const KeyOrder& order : orders)
            addOrder(result, KeyOrder(order));
    }
    return result;
}

void LiteralOrderSolver::leaveOrdersOut(Creation& creation)
{
    if (creation.hasOrdersLeftOut)
        return;
    creation.hasOrdersLeftOut = true;
    const Literal& literal = m_literals[creation.literal];
    m_inputs.build.gaveUp(LimitID::MaxExpandedKeyOrders, Subject { .function = literal.function, .value = literal.allocation, .layout = creation.layout });
}

// An order is never taken back, and a layout shows a limited number of them to its readers, so each creation is evaluated a limited number of times.
auto LiteralOrderSolver::evaluate(Creation& creation) -> Progress
{
    if (creation.hasOrdersLeftOut && creation.orders.size() >= m_maxOrders)
        return Progress::NothingToRead;

    const Literal& literal = m_literals[creation.literal];
    const Function& function = program().at(literal.function);
    std::optional<KeyOrder> keys = keysAtAllocation(literal);
    if (!keys)
        return Progress::NothingToRead;

    KeyOrders orders { WTF::move(*keys) };
    size_t nextCopy = 0;
    for (const LiteralPiece& piece : literal.pieces) {
        switch (piece.kind) {
        case PieceKind::DefinesUnknownProperty:
            return Progress::NothingToRead;
        case PieceKind::DefinesKey:
            for (KeyOrder& order : orders)
                appendIfAbsent(order, piece.key);
            break;
        case PieceKind::CopiesProperties:
            orders = copyInto(orders, creation.copies[nextCopy++], function.at(piece.operation));
            break;
        }
        if (orders.size() > m_maxOrders) {
            leaveOrdersOut(creation);
            orders.shrink(m_maxOrders);
        }
    }

    Progress progress = Progress::NothingToRead;
    KeyOrders& ordersToRead = m_literalOrdersOfLayout[creation.layout.index()];
    for (KeyOrder& order : orders) {
        if (creation.orders.contains(order))
            continue;
        if (creation.orders.size() >= m_maxOrders) {
            leaveOrdersOut(creation);
            break;
        }
        if (!ordersToRead.contains(order)) {
            if (ordersToRead.size() < m_maxOrders) {
                ordersToRead.append(order);
                progress = Progress::MoreToRead;
            } else
                leaveOrdersUnread(creation.layout);
        }
        creation.orders.append(WTF::move(order));
    }
    return progress;
}

void LiteralOrderSolver::solve()
{
    Deque<uint32_t> queue;
    BitVector isQueued(m_creations.size());
    auto enqueue = [&](uint32_t creation) {
        if (!isQueued.set(creation))
            queue.append(creation);
    };
    for (uint32_t index = 0; index < m_creations.size(); ++index)
        enqueue(index);

    while (!queue.isEmpty()) {
        uint32_t index = queue.takeFirst();
        isQueued.clear(index);
        if (evaluate(m_creations[index]) == Progress::NothingToRead)
            continue;
        for (uint32_t reader : m_readersOfLayout[m_creations[index].layout.index()])
            enqueue(reader);
    }
}

Vector<KeyOrder> LiteralOrderSolver::sortedOrdersOf(LayoutID id) const
{
    Vector<const KeyOrder*> listedOrders;
    for (uint32_t creation : m_creationsOfLayout[id.index()]) {
        for (const KeyOrder& order : m_creations[creation].orders)
            listedOrders.append(&order);
    }
    AOT::sort(listedOrders, [](const KeyOrder* a, const KeyOrder* b) {
        return std::ranges::lexicographical_compare(*a, *b);
    });

    Vector<CountedKeyOrder> countedOrders;
    for (const KeyOrder* order : listedOrders) {
        if (countedOrders.isEmpty() || *countedOrders.last().order != *order)
            countedOrders.append({ order, 0 });
        ++countedOrders.last().numberOfCreations;
    }
    AOT::sort(countedOrders, [](const CountedKeyOrder& a, const CountedKeyOrder& b) {
        if (a.numberOfCreations != b.numberOfCreations)
            return a.numberOfCreations > b.numberOfCreations;
        if (a.order->size() != b.order->size())
            return a.order->size() > b.order->size();
        return std::ranges::lexicographical_compare(*a.order, *b.order);
    });
    return countedOrders.map([](const CountedKeyOrder& counted) {
        return *counted.order;
    });
}

void LiteralOrderSolver::run()
{
    collectCreations();
    solve();
    for (uint32_t index = 0; index < m_layouts.size(); ++index)
        m_layouts[index].literalKeyOrders = sortedOrdersOf(LayoutID(index));
}

void computeLiteralKeyOrders(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, Vector<PropertyKeyKind>&& kindsOfKeys, FlowResultTables& tables)
{
    LiteralOrderSolver(inputs, heap, WTF::move(kindsOfKeys), tables.application->layouts).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
