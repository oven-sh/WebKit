/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

using FieldList = std::span<const FieldIndex>;
using KeyOrderList = Vector<Vector<FieldIndex>>;

static_assert(defaultLimitValue(LimitID::KeyOrdersPerLayout, hostArchitecture()) == maxKeyOrdersPerLayout);
static_assert(defaultLimitValue(LimitID::FieldsInKeyOrder, hostArchitecture()) == maxFieldsInOrder);

static constexpr auto isLexicographicallyLess = [](FieldList a, FieldList b) {
    return std::ranges::lexicographical_compare(a, b);
};

static bool hasStorage(const PlacementContext& context, LayoutID layout, FieldIndex field)
{
    return context.slots().slot(layout, field).hasStorage();
}

// How many keys of the order lie in front of the fields, where the order lists the fields one behind the other.
static std::optional<size_t> findPlaceInside(FieldList order, FieldList fields)
{
    size_t position = static_cast<size_t>(std::ranges::find(order, fields.front()) - order.begin());
    if (position == order.size() || !spanHasPrefix(order.subspan(position), fields))
        return std::nullopt;
    return position;
}

namespace {

struct CreationOrders {
    // What an object has when the stores outside its constructor begin. The orders of a constructor start at the allocation.
    const KeyOrderList& ordersThatStoresContinue() const { return hasConstructor ? constructorOrders : allocationOrders; }

    // Whether an object gets as far in the order of the declarations as the keys in front of the fields.
    bool reachesPlaceAmongDeclarations(FieldList declaredOrder, FieldList fields) const
    {
        std::optional<size_t> position = findPlaceInside(declaredOrder, fields);
        return position && countsReachedAmongDeclarations.get(*position);
    }

    KeyOrderList allocationOrders;
    KeyOrderList constructorOrders;
    KeyOrderList ordersContinuedByStores;
    KeyOrderList declaredOrders;
    // An object may have this many of the first keys of the order of the declarations, and no other key.
    BitVector countsReachedAmongDeclarations;
    bool hasConstructor { false };
};

struct InheritedField {
    FieldReference declaration;
    FieldIndex field { 0 };
};

class KeyOrderBuilder {
public:
    void append(FieldList fields)
    {
        for (FieldIndex field : fields) {
            if (m_isListed.add(field))
                m_fields.append(field);
        }
    }

    Vector<FieldIndex> take() { return WTF::move(m_fields); }

private:
    Vector<FieldIndex> m_fields;
    BitVector m_isListed;
};

class ConstructorChain {
public:
    ConstructorChain(const PlacementContext& context, LayoutID layout)
        : m_context(context)
        , m_layout(layout)
    {
        const Program& program = context.program;
        for (LayoutID current = layout; current; current = program.at(current).flow.base) {
            RELEASE_ASSERT(m_declaringLayouts.size() < program.layouts.size());
            m_declaringLayouts.append(current);
        }
        m_declaringLayouts.reverse();
        if (m_declaringLayouts.size() == 1)
            return;

        auto& fields = program.at(layout).fields;
        for (FieldIndex field = 0; field < fields.size(); ++field) {
            if (!fields[field].key && fields[field].flags.contains(FieldFlags::IsInherited))
                m_inheritedFieldsWithoutKey.append({ declarationOf(program, { layout, field }), field });
        }
        AOT::sort(m_inheritedFieldsWithoutKey, { }, &InheritedField::declaration);
    }

    Vector<FieldIndex> declaredOrder() const
    {
        KeyOrderBuilder order;
        for (LayoutID declaringLayout : m_declaringLayouts)
            order.append(declaredFieldsOf(declaringLayout).span());
        return order.take();
    }

    KeyOrderList constructorOrders() const
    {
        KeyOrderList orders(1);
        for (LayoutID declaringLayout : m_declaringLayouts) {
            KeyOrderList nextOrders = constructorOrdersOf(declaringLayout);
            if (nextOrders.isEmpty())
                break;
            uint64_t count = static_cast<uint64_t>(orders.size()) * nextOrders.size();
            if (m_context.build.exceeds(LimitID::KeyOrdersPerLayout, count, { .layout = m_layout }))
                break;
            orders = concatenateEach(orders, nextOrders);
        }
        return orders;
    }

private:
    static KeyOrderList concatenateEach(const KeyOrderList& firstParts, const KeyOrderList& secondParts)
    {
        KeyOrderList result;
        result.reserveInitialCapacity(firstParts.size() * secondParts.size());
        for (auto& firstPart : firstParts) {
            for (auto& secondPart : secondParts) {
                KeyOrderBuilder order;
                order.append(firstPart.span());
                order.append(secondPart.span());
                result.append(order.take());
            }
        }
        return result;
    }

    std::optional<FieldIndex> findField(FieldReference field) const
    {
        if (field.layout == m_layout)
            return field.field;
        const Program& program = m_context.program;
        if (PropertyKeyID key = fieldAt(program, field).key)
            return program.at(m_layout).findField(key);
        FieldReference declaration = declarationOf(program, field);
        auto found = std::ranges::lower_bound(m_inheritedFieldsWithoutKey, declaration, { }, &InheritedField::declaration);
        if (found == m_inheritedFieldsWithoutKey.end() || found->declaration != declaration)
            return std::nullopt;
        return found->field;
    }

    Vector<FieldIndex> storedFieldsOf(LayoutID declaringLayout, FieldList fields) const
    {
        Vector<FieldIndex> result;
        for (FieldIndex field : fields) {
            std::optional<FieldIndex> found = findField({ declaringLayout, field });
            if (found && hasStorage(m_context, m_layout, *found))
                result.append(*found);
        }
        return result;
    }

    Vector<FieldIndex> declaredFieldsOf(LayoutID declaringLayout) const
    {
        auto& fields = m_context.program.at(declaringLayout).fields;
        Vector<FieldIndex> declaredFields;
        for (FieldIndex field = 0; field < fields.size(); ++field) {
            if (!fields[field].flags.contains(FieldFlags::IsInherited))
                declaredFields.append(field);
        }
        return storedFieldsOf(declaringLayout, declaredFields.span());
    }

    KeyOrderList constructorOrdersOf(LayoutID declaringLayout) const
    {
        KeyOrderList orders = m_context.program.at(declaringLayout).flow.constructorOrders.map([&](const FieldOrder& order) {
            return storedFieldsOf(declaringLayout, order.fields.span());
        });
        if (orders.isEmpty() && declaredFieldsOf(declaringLayout).isEmpty())
            orders.grow(1);
        AOT::sort(orders, isLexicographicallyLess, &Vector<FieldIndex>::span);
        removeRepeatedElements(orders);
        return orders;
    }

    const PlacementContext& m_context;
    LayoutID m_layout;
    Vector<LayoutID, 4> m_declaringLayouts;
    Vector<InheritedField> m_inheritedFieldsWithoutKey;
};

} // anonymous namespace

static Vector<FieldIndex> storedInitialFields(const PlacementContext& context, const PlacedAllocation& allocation)
{
    Vector<FieldIndex> result;
    for (FieldIndex field : allocation.initialFields) {
        if (hasStorage(context, allocation.layout, field))
            result.append(field);
    }
    return result;
}

static bool haveCommonField(FieldList a, FieldList b)
{
    return std::ranges::any_of(a, [&](FieldIndex field) {
        return std::ranges::find(b, field) != b.end();
    });
}

static Vector<FieldIndex> concatenate(FieldList first, FieldList second)
{
    KeyOrderBuilder order;
    order.append(first);
    order.append(second);
    return order.take();
}

namespace {

enum class PlaceInOrder : uint8_t { Inside, Behind };

// Takes the places that the code of a store will name, before the orders have numbers, and keeps the keys that an object has behind the store where no other list of the layout has them.
class ContinuedOrderCollector {
public:
    explicit ContinuedOrderCollector(KeyOrderList& continuedOrders)
        : m_continuedOrders(continuedOrders)
    {
    }

    bool isFull() const { return m_keysBefore.size() == PlacedFieldCreation::maxContinuations; }

    void add(FieldList keysBefore, FieldList keysAfter, PlaceInOrder place)
    {
        if (m_keysBefore.containsIf([&](FieldList known) { return equalSpans(known, keysBefore); }))
            return;
        m_keysBefore.constructAndAppend(keysBefore);
        if (place == PlaceInOrder::Behind)
            m_continuedOrders.append(Vector<FieldIndex>(keysAfter));
    }

private:
    KeyOrderList& m_continuedOrders;
    Vector<FieldList, PlacedFieldCreation::maxContinuations> m_keysBefore;
};

class ContinuationCollector {
public:
    ContinuationCollector(const KeyOrders& keyOrders, PlacedFieldCreation& creation)
        : m_keyOrders(keyOrders)
        , m_creation(creation)
    {
    }

    bool isFull() const { return m_creation.continuations.size() == PlacedFieldCreation::maxContinuations; }

    // An order that went over the limit is not in the image.
    void add(FieldList keysBefore, FieldList keysAfter, PlaceInOrder)
    {
        std::optional<uint32_t> orderWordBefore = m_keyOrders.findOrderWord(m_creation.layout, keysBefore);
        std::optional<uint32_t> orderWordAfter = m_keyOrders.findOrderWord(m_creation.layout, keysAfter);
        if (!orderWordBefore || !orderWordAfter)
            return;
        KeyOrderContinuation continuation { *orderWordBefore, *orderWordAfter };
        if (!m_creation.continuations.contains(continuation))
            m_creation.continuations.append(continuation);
    }

private:
    const KeyOrders& m_keyOrders;
    PlacedFieldCreation& m_creation;
};

} // anonymous namespace

static void collectPlacesInside(const KeyOrderList& orders, FieldList created, auto& collector)
{
    for (auto& order : orders) {
        if (collector.isFull())
            return;
        if (std::optional<size_t> position = findPlaceInside(order.span(), created))
            collector.add(order.span().first(*position), order.span().first(*position + created.size()), PlaceInOrder::Inside);
    }
}

static void collectPlacesBehind(const KeyOrderList& orders, FieldList created, auto& collector)
{
    for (auto& order : orders) {
        if (collector.isFull())
            return;
        if (!haveCommonField(order.span(), created))
            collector.add(order.span(), concatenate(order.span(), created).span(), PlaceInOrder::Behind);
    }
}

// The places where the fields that a store, or a run of stores, creates may come next, the most likely place first.
static void collectPlaces(const CreationOrders& orders, FieldList created, auto& collector)
{
    collectPlacesInside(orders.constructorOrders, created, collector);
    bool isFilledInDeclaredOrder = !orders.hasConstructor && std::ranges::any_of(orders.declaredOrders, [&](const Vector<FieldIndex>& order) {
        return orders.reachesPlaceAmongDeclarations(order.span(), created);
    });
    if (isFilledInDeclaredOrder)
        collectPlacesInside(orders.declaredOrders, created, collector);
    collectPlacesBehind(orders.ordersThatStoresContinue(), created, collector);
    collectPlacesInside(orders.declaredOrders, created, collector);
}

// An object that has the keys of its allocation or of its constructor gets more keys from stores.
static KeyOrderList continueOrders(const CreationOrders& orders, std::span<const PlacedFieldCreation> creations)
{
    KeyOrderList result;
    for (auto& creation : creations) {
        ContinuedOrderCollector collector(result);
        collectPlaces(orders, creation.fields.span(), collector);
    }
    return result;
}

// An object has what its allocation lists.
static BitVector findCountsReachedAmongDeclarations(const PlacementContext& context, LayoutID layout, FieldList declaredOrder, const KeyOrderList& allocationOrders, std::span<const PlacedFieldCreation> creations)
{
    const Program& program = context.program;
    BitVector isCreatedBehindAllocation;
    for (auto& creation : creations) {
        for (FieldIndex field : creation.fields)
            isCreatedBehindAllocation.set(field);
    }
    for (FieldIndex field : declaredOrder) {
        if (!fieldAt(program, declarationOf(program, { layout, field })).form.isEmpty())
            isCreatedBehindAllocation.set(field);
    }

    BitVector counts;
    for (auto& listedFields : allocationOrders) {
        if (!listedFields.isEmpty() && !spanHasPrefix(declaredOrder, listedFields.span()))
            continue;
        size_t count = listedFields.size();
        while (counts.add(count) && count < declaredOrder.size() && isCreatedBehindAllocation.get(declaredOrder[count]))
            ++count;
    }
    return counts;
}

static size_t countDeclaredKeysThatStoresReach(const CreationOrders& orders, FieldList declaredOrder, std::span<const PlacedFieldCreation> creations)
{
    size_t count = 0;
    for (auto& creation : creations) {
        std::optional<size_t> position = findPlaceInside(declaredOrder, creation.fields.span());
        if (position && orders.countsReachedAmongDeclarations.get(*position))
            count = std::max(count, *position + creation.fields.size());
    }
    return count;
}

static CreationOrders collectCreationOrders(const PlacementContext& context, const AllocationTable& table, LayoutID layout, std::span<const PlacedFieldCreation> creations)
{
    auto allocations = table.ofLayout(layout);
    CreationOrders orders;
    orders.allocationOrders = KeyOrderList(allocations.size(), [&](size_t i) {
        return storedInitialFields(context, table.allocations[allocations[i].index()]);
    });
    ConstructorChain chain(context, layout);
    orders.constructorOrders = chain.constructorOrders();
    // Flow names no order where somebody else may get hold of the object before the first store. Then the declarations say what the constructor creates, in its sequence, as for an object without one.
    orders.hasConstructor = context.program.at(declaredLayoutOf(context.program, layout)).constructor && std::ranges::any_of(orders.constructorOrders, [](const Vector<FieldIndex>& fields) {
        return !fields.isEmpty();
    });

    // Whatever fills an object that starts without keys, compiled or not, is expected to go by the declarations.
    bool canStartWithoutKeys = allocations.empty()
        || std::ranges::any_of(orders.allocationOrders, [](const auto& fields) { return fields.isEmpty(); });
    if (canStartWithoutKeys || !creations.empty()) {
        Vector<FieldIndex> declaredOrder = chain.declaredOrder();
        if (!creations.empty())
            orders.countsReachedAmongDeclarations = findCountsReachedAmongDeclarations(context, layout, declaredOrder.span(), orders.allocationOrders, creations);
        if (!canStartWithoutKeys)
            declaredOrder.shrink(countDeclaredKeysThatStoresReach(orders, declaredOrder.span(), creations));
        orders.declaredOrders.append(WTF::move(declaredOrder));
    }
    orders.ordersContinuedByStores = continueOrders(orders, creations);
    return orders;
}

static bool canBeKeyOrder(const PlacementContext& context, const KeyOrderTestUse& test)
{
    BitVector isListed;
    return std::ranges::all_of(test.fields, [&](FieldIndex field) {
        return hasStorage(context, test.layout, field) && isListed.add(field);
    });
}

static KeyOrderList computeKeyOrders(const PlacementContext& context, std::initializer_list<const KeyOrderList*> creationOrders, std::span<const KeyOrderTestUse> tests)
{
    Vector<FieldList> candidates;
    auto addCandidate = [&](const Vector<FieldIndex>& fields) {
        if (!fields.isEmpty())
            candidates.constructAndAppend(fields.span());
    };
    for (const KeyOrderList* orders : creationOrders) {
        for (auto& fields : *orders)
            addCandidate(fields);
    }
    for (auto& test : tests) {
        if (canBeKeyOrder(context, test))
            addCandidate(test.fields);
    }
    AOT::sort(candidates, isLexicographicallyLess);

    KeyOrderList orders;
    orders.reserveInitialCapacity(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        // In sorted order, a list that equals or is a prefix of any later list is a prefix of the next one.
        if (i + 1 < candidates.size() && spanHasPrefix(candidates[i + 1], candidates[i]))
            continue;
        RELEASE_ASSERT(candidates[i].size() <= maxFieldsInOrder);
        orders.append(Vector<FieldIndex>(candidates[i]));
    }
    return orders;
}

static std::optional<KeyOrderList> chooseKeyOrders(const PlacementContext& context, LayoutID layout, const CreationOrders& creationOrders, std::span<const KeyOrderTestUse> tests)
{
    BuildContext& build = context.build;
    Subject subject { .layout = layout };
    const KeyOrderList* allocationOrders = &creationOrders.allocationOrders;
    const KeyOrderList* constructorOrders = &creationOrders.constructorOrders;
    const KeyOrderList* declaredOrders = &creationOrders.declaredOrders;

    KeyOrderList orders = computeKeyOrders(context, { allocationOrders, constructorOrders, declaredOrders, &creationOrders.ordersContinuedByStores }, tests);
    if (!build.exceeds(LimitID::KeyOrdersPerLayout, orders.size(), subject))
        return orders;
    orders = computeKeyOrders(context, { allocationOrders, constructorOrders, declaredOrders }, tests);
    if (!build.exceeds(LimitID::KeyOrdersPerLayout, orders.size(), subject))
        return orders;
    orders = computeKeyOrders(context, { allocationOrders, constructorOrders, declaredOrders }, { });
    if (!build.exceeds(LimitID::KeyOrdersPerLayout, orders.size(), subject))
        return orders;
    orders = computeKeyOrders(context, { allocationOrders }, { });
    if (orders.size() <= build.limit(LimitID::KeyOrdersPerLayout))
        return orders;
    build.reportLimitExceeded(LimitID::KeyOrdersPerLayout, orders.size(), subject);
    return std::nullopt;
}

static void findContinuations(const KeyOrders& keyOrders, const CreationOrders& creationOrders, PlacedFieldCreation& creation)
{
    ContinuationCollector collector(keyOrders, creation);
    collectPlaces(creationOrders, creation.fields.span(), collector);
}

static void collectLayoutKeyOrders(const PlacementContext& context, const AllocationTable& table, LayoutID layout, std::span<PlacedFieldCreation> creations, std::span<const KeyOrderTestUse> tests, KeyOrders& keyOrders, PlacedOrderWords& orderWords)
{
    CreationOrders creationOrders = collectCreationOrders(context, table, layout, creations);
    std::optional<KeyOrderList> orders = chooseKeyOrders(context, layout, creationOrders, tests);
    if (!orders)
        return;
    keyOrders.ofLayout[layout.index()] = WTF::move(*orders);
    for (PlacedFieldCreation& creation : creations)
        findContinuations(keyOrders, creationOrders, creation);

    auto allocations = table.ofLayout(layout);
    for (size_t i = 0; i < allocations.size(); ++i) {
        std::optional<uint32_t> orderWord = keyOrders.findOrderWord(layout, creationOrders.allocationOrders[i].span());
        RELEASE_ASSERT(orderWord);
        orderWords.ofAllocation[allocations[i].index()] = *orderWord;
    }
}

std::pair<KeyOrders, PlacedOrderWords> collectKeyOrders(const PlacementContext& context, const PlacedSites& sites, const AllocationTable& table)
{
    auto& layouts = context.program.layouts;
    auto& tests = sites.keyOrderTests;
    KeyOrders keyOrders;
    keyOrders.ofLayout.grow(layouts.size());
    keyOrders.fieldCreations = sites.fieldCreations.map([](const FieldCreationUse& use) {
        return PlacedFieldCreation { use.layout, use.fields, { } };
    });
    keyOrders.firstStoreThatMayAdd = sites.firstStoreThatMayAdd;
    PlacedOrderWords orderWords;
    orderWords.ofAllocation.fill(0, table.allocations.size());
    orderWords.ofTest.fill(std::nullopt, tests.size());
    forEachIndexInParallel(context.build.parallelLoops(), layouts.size(), [&](unsigned index) {
        LayoutID layout(index);
        std::span<const KeyOrderTestUse> layoutTests = std::ranges::equal_range(tests, layout, { }, &KeyOrderTestUse::layout);
        std::span<PlacedFieldCreation> creations = std::ranges::equal_range(keyOrders.fieldCreations, layout, { }, &PlacedFieldCreation::layout);
        if (context.isCompiledObject(layout) && !context.slots().layout(layout).keysAreNeverEnumerated)
            collectLayoutKeyOrders(context, table, layout, creations, layoutTests, keyOrders, orderWords);
        for (auto& test : layoutTests)
            orderWords.ofTest[&test - tests.begin()] = keyOrders.findOrderWord(layout, test.fields.span());
    });
    return { WTF::move(keyOrders), WTF::move(orderWords) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
