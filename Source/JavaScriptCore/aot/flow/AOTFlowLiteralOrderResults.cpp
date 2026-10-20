/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTNameClass.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Deque.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

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
    uint32_t siteInSummary { 0 };
    Vector<LiteralPiece, 4> pieces;
};

struct CopiedObjects {
    Vector<LayoutID, 2> layouts;
    bool mayHaveNoKeys { false };
};

// What one literal creates in one instance of its function.
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

class LiteralOrderSolver {
public:
    LiteralOrderSolver(const FlowFamilyInputs& inputs, std::span<LayoutFlow> layouts)
        : m_inputs(inputs)
        , m_layouts(layouts)
        , m_maxOrders(inputs.build().limit(LimitID::MaxExpandedKeyOrders))
        , m_creationsOfLayout(layouts.size())
        , m_readersOfLayout(layouts.size())
        , m_literalOrdersOfLayout(layouts.size())
    {
    }

    FlowLiteralKeyOrders run();

private:
    const Program& program() const { return m_inputs.program(); }

    void collectCreations();
    void addCreationsOf(uint32_t literal);
    LayoutID layoutCreatedBy(const Literal&, FlowInstanceID) const;
    CopiedObjects objectsCopiedBy(const Value& copy, FlowInstanceID) const;
    void listAllocatedFields(const Literal&);
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

    const FlowFamilyInputs& m_inputs;
    std::span<LayoutFlow> m_layouts;
    uint64_t m_maxOrders;
    Vector<Literal> m_literals;
    Vector<Creation> m_creations;
    Vector<Vector<uint32_t>> m_creationsOfLayout;
    Vector<Vector<uint32_t>> m_readersOfLayout;
    // What a copy from an object of the layout reads beside the constructor orders: the first distinct orders that the creations of the layout got.
    Vector<KeyOrders> m_literalOrdersOfLayout;
    BitVector m_hasOrdersUnread;
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

static void collectLiteralsOf(const FlowFamilyInputs& inputs, FunctionID id, Vector<Literal>& literals)
{
    const FlowSummary& summary = inputs.graph().summaryOf(id);
    size_t first = literals.size();
    for (uint32_t site = 0; site < summary.sites.size(); ++site) {
        if (summary.sites[site].isCompletedByPieces)
            literals.append(Literal { id, summary.sites[site].value, site, { } });
    }
    if (literals.size() == first)
        return;

    const Function& function = inputs.program().at(id);
    std::span<Literal> literalsOfFunction = literals.mutableSpan().subspan(first);
    CFG cfg(function);
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

LayoutID LiteralOrderSolver::layoutCreatedBy(const Literal& literal, FlowInstanceID instance) const
{
    const FlowObjectTable& objects = m_inputs.objects();
    if (m_inputs.graph().at(instance).state == FlowInstanceState::Created)
        return { };
    FlowObjectID object = m_inputs.graph().objectOf(instance, literal.siteInSummary);
    if (!object || object == unknownFlowObject || !m_inputs.factsOf(object).events.contains(FlowObjectEvent::Allocated))
        return { };
    AllocationClassID allocationClass = objects.at(object).allocationClass;
    if (!allocationClass || m_inputs.hierarchy.hasOnlyOrdinaryObjects(allocationClass))
        return { };
    return objects.at(allocationClass).layout;
}

CopiedObjects LiteralOrderSolver::objectsCopiedBy(const Value& copy, FlowInstanceID instance) const
{
    const FlowObjectTable& objects = m_inputs.objects();
    CopiedObjects copied;
    FlowNodeID source = m_inputs.graph().nodeOf(instance, copy.sourceOperand());
    if (!source) {
        copied.mayHaveNoKeys = true;
        return copied;
    }

    const FlowValue& value = m_inputs.graph().valueOf(source);
    copied.mayHaveNoKeys = value.primitives && value.hasOnlyTrackedObjects();
    Vector<AllocationClassID, 8> allocationClasses;
    value.objects.forEach([&](FlowObjectID object) {
        const FlowObject& record = objects.at(object);
        if (record.kind == FlowObjectKind::Allocation && record.allocationClass)
            allocationClasses.append(record.allocationClass);
    });
    std::ranges::sort(allocationClasses);
    removeRepeatedElements(allocationClasses);
    for (AllocationClassID allocationClass : allocationClasses) {
        LayoutID layout = objects.at(allocationClass).layout;
        if (layout && !m_inputs.hierarchy.hasOnlyOrdinaryObjects(allocationClass))
            copied.layouts.append(layout);
    }
    std::ranges::sort(copied.layouts);
    removeRepeatedElements(copied.layouts);
    return copied;
}

// An allocation lists fields of the declared layout, whichever layout of the family it names.
void LiteralOrderSolver::listAllocatedFields(const Literal& literal)
{
    const Value& allocation = program().at(literal.function).at(literal.allocation);
    FieldOrder order { Vector<uint32_t>(allocation.fields()) };
    Vector<FieldOrder>& orders = m_layouts[allocation.layout().index()].constructorOrders;
    if (!orders.contains(order))
        orders.append(WTF::move(order));
}

void LiteralOrderSolver::addCreationsOf(uint32_t index)
{
    const Literal& literal = m_literals[index];
    const Function& function = program().at(literal.function);
    m_inputs.graph().forEachInstanceOf(literal.function, [&](FlowInstanceID instance) {
        LayoutID layout = layoutCreatedBy(literal, instance);
        if (!layout || !m_inputs.isEnabled(FlowResultKind::ConstructorFieldOrders, layout.index()))
            return;
        // Nothing creates a field of such a layout behind an allocation, so the pieces add no key, and whoever relies on that finds every order listed.
        if (m_layouts[layout.index()].fieldCreation == FieldCreation::OnlyAtConstruction) {
            listAllocatedFields(literal);
            return;
        }

        Creation creation { .literal = index, .layout = layout };
        for (const LiteralPiece& piece : literal.pieces) {
            if (piece.kind == PieceKind::CopiesProperties)
                creation.copies.append(objectsCopiedBy(function.at(piece.operation), instance));
        }
        m_creations.append(WTF::move(creation));
    });
}

void LiteralOrderSolver::registerReader(uint32_t index)
{
    const Creation& creation = m_creations[index];
    m_creationsOfLayout[creation.layout.index()].append(index);
    for (const CopiedObjects& copied : creation.copies) {
        for (LayoutID source : copied.layouts) {
            for (LayoutID current = source; current; current = m_inputs.hierarchy.baseOf(current).base) {
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
        addCreationsOf(index);
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
    if (m_hasOrdersUnread.add(layout.index()))
        m_inputs.build().gaveUp(LimitID::MaxExpandedKeyOrders, Subject { .layout = layout });
}

// No walk over own keys tests more orders than the limit, so no copy reads more of them.
KeyOrders LiteralOrderSolver::ownOrdersOf(LayoutID id)
{
    const Vector<Field>& fields = program().at(id).fields;
    KeyOrders orders;
    for (const FieldOrder& order : m_layouts[id.index()].constructorOrders) {
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
    const FlowBaseResult& base = m_inputs.hierarchy.baseOf(id);
    switch (base.state) {
    case BaseState::None:
        return ownOrders;
    case BaseState::Layout:
        break;
    case BaseState::BuiltinClass:
    case BaseState::Unknown:
        return { };
    }

    KeyOrders orders;
    for (const KeyOrder& baseOrder : ordersOf(base.base)) {
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
    for (LayoutID current = source; current; current = m_inputs.hierarchy.baseOf(current).base) {
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
        if (program().at(key).kind == PropertyKeyKind::Text && isCopied(source, key, copy))
            appendIfAbsent(target, key);
    }
    for (PropertyKeyID key : sourceOrder) {
        if (isSymbolKind(program().at(key).kind) && isCopied(source, key, copy))
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
    m_inputs.build().gaveUp(LimitID::MaxExpandedKeyOrders, Subject { .function = literal.function, .value = literal.allocation, .layout = creation.layout });
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
        if (isQueued.add(creation))
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
    std::ranges::sort(listedOrders, [](const KeyOrder* a, const KeyOrder* b) {
        return std::ranges::lexicographical_compare(*a, *b);
    });

    Vector<CountedKeyOrder> countedOrders;
    for (const KeyOrder* order : listedOrders) {
        if (countedOrders.isEmpty() || *countedOrders.last().order != *order)
            countedOrders.append({ order, 0 });
        ++countedOrders.last().numberOfCreations;
    }
    std::ranges::sort(countedOrders, [](const CountedKeyOrder& a, const CountedKeyOrder& b) {
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

FlowLiteralKeyOrders LiteralOrderSolver::run()
{
    collectCreations();
    solve();
    return FlowLiteralKeyOrders(m_layouts.size(), [&](size_t index) {
        return sortedOrdersOf(LayoutID(index));
    });
}

FlowLiteralKeyOrders computeLiteralKeyOrders(const FlowFamilyInputs& inputs, std::span<LayoutFlow> layouts)
{
    return LiteralOrderSolver(inputs, layouts).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
