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
#include <wtf/Deque.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet literalPieceOperations { Opcode::PropDefine, Opcode::ElemDefine, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::CopyDataProperties };
static constexpr uint32_t literalOperandOfPiece = OperandIndex::FinishObject::object;

static_assert(OperandIndex::PropDefine::object == literalOperandOfPiece && OperandIndex::ElemDefine::object == literalOperandOfPiece);
static_assert(OperandIndex::DefineGetter::object == literalOperandOfPiece && OperandIndex::DefineSetter::object == literalOperandOfPiece);
static_assert(OperandIndex::CopyDataProperties::target == literalOperandOfPiece);

bool isPieceOfLiteral(const Value& operation, ValueID literal)
{
    return literalPieceOperations.contains(operation.opcode) && operation.operand(literalOperandOfPiece) == literal;
}

namespace {

using KeyOrder = Vector<PropertyKeyID>;
using LiteralKeyOrders = Vector<KeyOrder, 2>;

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

struct LiteralCreation {
    uint32_t literal { 0 };
    LayoutID layout;
    Vector<CopiedObjects, 1> copies;
    LiteralKeyOrders orders;
    bool hasOrdersLeftOut { false };
    bool hasOrderNobodyKnows { false };
};

} // anonymous namespace

template<typename List, typename Element>
static void appendIfAbsent(List& list, Element&& element)
{
    if (!list.contains(element))
        list.append(std::forward<Element>(element));
}

static LiteralPiece describePiece(ValueID id, const Value& operation)
{
    switch (operation.opcode) {
    case Opcode::PropDefine:
        return { PieceKind::DefinesKey, operation.key(), id };
    case Opcode::CopyDataProperties:
        return { operation.excludedOperands().empty() ? PieceKind::CopiesProperties : PieceKind::DefinesUnknownProperty, { }, id };
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
    if (!inputs.isReached(id) || !inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::FinishObject })))
        return;

    CFG cfg(function);
    auto forEachOperation = [&](const auto& functor) {
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID value : function.at(block).values)
                functor(value, function.at(value));
        }
    };
    BitVector isFinished(function.values.size());
    BitVector hasPieces(function.values.size());
    forEachOperation([&](ValueID, const Value& operation) {
        bool finishes = operation.opcode == Opcode::FinishObject;
        if (!finishes && !literalPieceOperations.contains(operation.opcode))
            return;
        if (ValueID literal = operation.operand(literalOperandOfPiece))
            (finishes ? isFinished : hasPieces).quickSet(literal.index());
    });
    size_t first = literals.size();
    forEachOperation([&](ValueID value, const Value& operation) {
        if (operation.opcode == Opcode::NewObject && isFinished.quickGet(value.index()) && hasPieces.quickGet(value.index()))
            literals.append(Literal { id, value, { } });
    });
    std::span<Literal> literalsOfFunction = literals.mutableSpan().subspan(first);
    AOT::sort(literalsOfFunction, { }, &Literal::allocation);
    if (literalsOfFunction.empty())
        return;

    forEachOperation([&](ValueID value, const Value& operation) {
        if (!literalPieceOperations.contains(operation.opcode))
            return;
        ValueID allocation = operation.operand(literalOperandOfPiece);
        auto literal = std::ranges::lower_bound(literalsOfFunction, allocation, { }, &Literal::allocation);
        if (literal != literalsOfFunction.end() && literal->allocation == allocation)
            literal->pieces.append(describePiece(value, operation));
    });
}

namespace {

struct LiteralOrderSolver {
    const ValueFlowResultInputs& inputs;
    ValueFlowHeapView& heap;
    Vector<PropertyKeyKind> kindOfKey;
    Vector<FlowLayoutUpdate>& layouts;
    const Program& program { inputs.program };
    uint64_t maxOrders { inputs.build.limit(LimitID::MaxExpandedKeyOrders) };
    Vector<Literal> literals { };
    Vector<LiteralCreation> creations { };
    Vector<Vector<uint32_t>> creationsOfLayout = Vector<Vector<uint32_t>>(layouts.size());
    Vector<Vector<uint32_t>> readersOfLayout = Vector<Vector<uint32_t>>(layouts.size());
    Vector<LiteralKeyOrders> literalOrdersOfLayout = Vector<LiteralKeyOrders>(layouts.size());
    BitVector hasOrdersUnread { };

    const LayoutFlow& flowOf(LayoutID layout) const { return layouts[layout.index()].flow; }
    LayoutID provenBaseOf(LayoutID layout) const { return flowOf(layout).baseState == BaseState::Layout ? flowOf(layout).base : LayoutID(); }

    CopiedObjects objectsCopiedBy(FunctionID function, ValueID copy)
    {
        const ValueFlowUniverse& universe = inputs.universe();
        CopiedObjects copied;
        std::span<const ValueFlowStatement> statements = inputs.bodies().statementsOf(function.index());
        auto statement = std::ranges::find_if(statements, [&](const ValueFlowStatement& candidate) {
            return candidate.kind == ValueFlowStatementKind::Operation && candidate.site == copy;
        });
        if (statement == statements.end()) {
            copied.mayHaveNoKeys = true;
            return copied;
        }

        ValueSetID sources = inputs.seenValuesOf(heap.maker(), inputs.bodies().operandsOf(*statement)[OperandIndex::CopyDataProperties::source]);
        if (ValueSets::isComplement(sources))
            return copied;
        bool holdsPlaceholder = inputs.sets().meet(sources, universe.placeholdersAlone());
        copied.mayHaveNoKeys = !holdsPlaceholder && inputs.sets().meet(sources, universe.primitives());
        inputs.sets().forEachMember(sources, [&](ValueFlowObjectID object) {
            ValueFlowObjectKind kind = universe.kindOf(object);
            if (kind != ValueFlowObjectKind::Site && kind != ValueFlowObjectKind::Instance)
                return;
            LayoutID layout = universe.layoutOf(object);
            if (layout && hasObjectsThatCodeAlters(program.at(layout).kind))
                copied.layouts.append(layout);
        });
        AOT::sort(copied.layouts);
        removeRepeatedElements(copied.layouts);
        return copied;
    }

    void collectCreations()
    {
        literals = collectInParallel<Literal>(inputs.loops(), static_cast<unsigned>(program.functions.size()), [&](unsigned index, Vector<Literal>& found) {
            collectLiteralsOf(inputs, FunctionID(index), found);
        });
        for (uint32_t index = 0; index < literals.size(); ++index) {
            const Literal& literal = literals[index];
            LayoutID layout = program.at(literal.function).at(literal.allocation).layout();
            if (!hasObjectsThatCodeAlters(program.at(layout).kind) || flowOf(layout).allocationClasses.isEmpty() || !inputs.build.isEnabled(FlowResultKind::ConstructorFieldOrders, layout.index()))
                continue;
            if (flowOf(layout).fieldCreation == FieldCreation::OnlyAtConstruction)
                continue;

            LiteralCreation creation { .literal = index, .layout = layout };
            for (const LiteralPiece& piece : literal.pieces) {
                if (piece.kind == PieceKind::CopiesProperties)
                    creation.copies.append(objectsCopiedBy(literal.function, piece.operation));
            }
            creations.append(WTF::move(creation));
        }
        for (uint32_t index = 0; index < creations.size(); ++index) {
            creationsOfLayout[creations[index].layout.index()].append(index);
            for (const CopiedObjects& copied : creations[index].copies) {
                for (LayoutID source : copied.layouts) {
                    for (LayoutID current = source; current; current = provenBaseOf(current)) {
                        Vector<uint32_t>& readers = readersOfLayout[current.index()];
                        if (readers.isEmpty() || readers.last() != index)
                            readers.append(index);
                    }
                }
            }
        }
    }

    void leaveOrdersUnread(LayoutID layout)
    {
        if (!hasOrdersUnread.set(layout.index()))
            inputs.build.gaveUp(LimitID::MaxExpandedKeyOrders, Subject { .layout = layout });
    }

    LiteralKeyOrders ownOrdersOf(LayoutID id)
    {
        const Vector<Field>& fields = program.at(id).fields;
        LiteralKeyOrders orders;
        for (const FieldOrder& order : flowOf(id).constructorOrders) {
            if (orders.size() >= maxOrders) {
                leaveOrdersUnread(id);
                return orders;
            }
            bool hasOnlyConstantKeys = std::ranges::all_of(order.fields, [&](uint32_t field) {
                return !!fields[field].key;
            });
            if (!hasOnlyConstantKeys)
                continue;
            appendIfAbsent(orders, order.fields.map<KeyOrder>([&](uint32_t field) {
                return fields[field].key;
            }));
        }
        for (const KeyOrder& order : literalOrdersOfLayout[id.index()]) {
            if (orders.size() >= maxOrders) {
                leaveOrdersUnread(id);
                return orders;
            }
            appendIfAbsent(orders, order);
        }
        return orders;
    }

    LiteralKeyOrders ordersOf(LayoutID id)
    {
        LiteralKeyOrders ownOrders = ownOrdersOf(id);
        switch (flowOf(id).baseState) {
        case BaseState::None:
            return ownOrders;
        case BaseState::Layout:
            break;
        case BaseState::BuiltinClass:
        case BaseState::Unknown:
            return { };
        }

        LiteralKeyOrders orders;
        for (const KeyOrder& baseOrder : ordersOf(flowOf(id).base)) {
            for (const KeyOrder& ownOrder : ownOrders) {
                if (orders.size() >= maxOrders) {
                    leaveOrdersUnread(id);
                    return orders;
                }
                KeyOrder order = baseOrder;
                for (PropertyKeyID key : ownOrder)
                    appendIfAbsent(order, key);
                appendIfAbsent(orders, WTF::move(order));
            }
        }
        return orders;
    }

    bool isCopied(LayoutID source, PropertyKeyID key, const Value& copy) const
    {
        if (std::ranges::contains(copy.excludedKeys(), key))
            return false;
        for (LayoutID current = source; current; current = provenBaseOf(current)) {
            const Layout& layout = program.at(current);
            if (std::optional<uint32_t> field = layout.findField(key))
                return !layout.fields[*field].flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden });
        }
        return true;
    }

    LiteralKeyOrders copyInto(const LiteralKeyOrders& orders, const CopiedObjects& copied, const Value& copy)
    {
        LiteralKeyOrders result;
        for (LayoutID source : copied.layouts) {
            for (const KeyOrder& sourceOrder : ordersOf(source)) {
                for (const KeyOrder& order : orders) {
                    KeyOrder extended = order;
                    for (PropertyKeyID key : sourceOrder) {
                        PropertyKeyKind kind = kindOfKey[key.index()];
                        if ((kind == PropertyKeyKind::Text || isSymbolKind(kind)) && isCopied(source, key, copy))
                            appendIfAbsent(extended, key);
                    }
                    appendIfAbsent(result, WTF::move(extended));
                }
            }
            if (result.size() > maxOrders)
                return result;
        }
        if (copied.mayHaveNoKeys) {
            for (const KeyOrder& order : orders)
                appendIfAbsent(result, order);
        }
        return result;
    }

    void leaveOrdersOut(LiteralCreation& creation)
    {
        if (std::exchange(creation.hasOrdersLeftOut, true))
            return;
        const Literal& literal = literals[creation.literal];
        inputs.build.gaveUp(LimitID::MaxExpandedKeyOrders, Subject { .function = literal.function, .value = literal.allocation, .layout = creation.layout });
    }

    bool evaluate(LiteralCreation& creation)
    {
        if (creation.hasOrdersLeftOut && creation.orders.size() >= maxOrders)
            return false;

        const Literal& literal = literals[creation.literal];
        const Function& function = program.at(literal.function);
        const Value& allocation = function.at(literal.allocation);
        const Vector<Field>& fields = program.at(allocation.layout()).fields;
        KeyOrder keysAtAllocation;
        for (uint32_t field : allocation.fields()) {
            if (!fields[field].key) {
                creation.hasOrderNobodyKnows = true;
                return false;
            }
            keysAtAllocation.append(fields[field].key);
        }

        LiteralKeyOrders orders { WTF::move(keysAtAllocation) };
        // The keys of an order of many keys, by the index of the order: a literal may define thousands. Empty: not listed yet.
        using KeySet = UncheckedKeyHashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
        constexpr size_t mostKeysSearchedOneByOne = 16;
        Vector<KeySet, 2> keysOfLongOrders;
        auto define = [&](size_t index, PropertyKeyID key) {
            KeyOrder& order = orders[index];
            if (order.size() <= mostKeysSearchedOneByOne) {
                appendIfAbsent(order, key);
                return;
            }
            if (keysOfLongOrders.size() <= index)
                keysOfLongOrders.grow(index + 1);
            KeySet& keys = keysOfLongOrders[index];
            if (keys.isEmpty()) {
                for (PropertyKeyID listed : order)
                    keys.add(listed.index());
            }
            if (keys.add(key.index()).isNewEntry)
                order.append(key);
        };
        size_t nextCopy = 0;
        for (const LiteralPiece& piece : literal.pieces) {
            switch (piece.kind) {
            case PieceKind::DefinesUnknownProperty:
                creation.hasOrderNobodyKnows = true;
                return false;
            case PieceKind::DefinesKey:
                for (size_t index = 0; index < orders.size(); ++index)
                    define(index, piece.key);
                break;
            case PieceKind::CopiesProperties:
                orders = copyInto(orders, creation.copies[nextCopy++], function.at(piece.operation));
                keysOfLongOrders.clear();
                break;
            }
            if (orders.size() > maxOrders) {
                leaveOrdersOut(creation);
                orders.shrink(maxOrders);
            }
        }

        bool hasMoreToRead = false;
        LiteralKeyOrders& ordersToRead = literalOrdersOfLayout[creation.layout.index()];
        for (KeyOrder& order : orders) {
            if (creation.orders.contains(order))
                continue;
            if (creation.orders.size() >= maxOrders) {
                leaveOrdersOut(creation);
                break;
            }
            if (!ordersToRead.contains(order)) {
                if (ordersToRead.size() < maxOrders) {
                    ordersToRead.append(order);
                    hasMoreToRead = true;
                } else
                    leaveOrdersUnread(creation.layout);
            }
            creation.orders.append(WTF::move(order));
        }
        return hasMoreToRead;
    }

    void solve()
    {
        Deque<uint32_t> queue;
        BitVector isQueued(creations.size());
        auto enqueue = [&](uint32_t creation) {
            if (!isQueued.set(creation))
                queue.append(creation);
        };
        for (uint32_t index = 0; index < creations.size(); ++index)
            enqueue(index);

        while (!queue.isEmpty()) {
            uint32_t index = queue.takeFirst();
            isQueued.clear(index);
            if (!evaluate(creations[index]))
                continue;
            for (uint32_t reader : readersOfLayout[creations[index].layout.index()])
                enqueue(reader);
        }
    }

    // The definitions of a literal run one after the other, each once, so a literal without a copy has one order: that of its text.
    bool listsOrderOfEveryObject(LayoutID id) const
    {
        if (!layouts[id.index()].createsFieldsOnlyInLiterals || hasOrdersUnread.get(id.index()))
            return false;
        return std::ranges::all_of(creationsOfLayout[id.index()], [&](uint32_t index) {
            const LiteralCreation& creation = creations[index];
            return creation.copies.isEmpty() && !creation.hasOrdersLeftOut && !creation.hasOrderNobodyKnows;
        });
    }

    Vector<KeyOrder> sortedOrdersOf(LayoutID id) const
    {
        Vector<const KeyOrder*> listedOrders;
        for (uint32_t creation : creationsOfLayout[id.index()]) {
            for (const KeyOrder& order : creations[creation].orders)
                listedOrders.append(&order);
        }
        AOT::sort(listedOrders, [](const KeyOrder* a, const KeyOrder* b) {
            return std::ranges::lexicographical_compare(*a, *b);
        });

        struct CountedKeyOrder {
            const KeyOrder* order { nullptr };
            size_t numberOfCreations { 0 };
        };
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
};

} // anonymous namespace

void computeLiteralKeyOrders(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, Vector<PropertyKeyKind>&& kindsOfKeys, FlowResultTables& tables)
{
    LiteralOrderSolver solver { inputs, heap, WTF::move(kindsOfKeys), tables.application->layouts };
    solver.collectCreations();
    solver.solve();
    for (uint32_t index = 0; index < solver.layouts.size(); ++index) {
        solver.layouts[index].literalKeyOrders = solver.sortedOrdersOf(LayoutID(index));
        if (solver.listsOrderOfEveryObject(LayoutID(index)))
            solver.layouts[index].flow.fieldCreation = FieldCreation::OnlyBeforeObjectIsFinished;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
