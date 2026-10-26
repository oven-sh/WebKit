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

static_assert(defaultLimitValue(LimitID::KeyOrdersPerLayout, hostArchitecture()) <= maxKeyOrdersPerLayout);
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

static Vector<FieldIndex> concatenate(FieldList first, FieldList second)
{
    Vector<FieldIndex> order;
    BitVector isListed;
    for (FieldList part : { first, second }) {
        for (FieldIndex field : part) {
            if (isListed.add(field))
                order.append(field);
        }
    }
    return order;
}

namespace {

struct CreationOrders {
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
        Vector<FieldIndex> fields;
        for (LayoutID declaringLayout : m_declaringLayouts)
            fields.appendVector(declaredFieldsOf(declaringLayout));
        return concatenate(fields.span(), { });
    }

    KeyOrderList constructorOrders() const
    {
        KeyOrderList orders(1);
        for (LayoutID declaringLayout : m_declaringLayouts) {
            KeyOrderList nextOrders = constructorOrdersOf(declaringLayout);
            if (nextOrders.isEmpty() || m_context.build.exceeds(LimitID::KeyOrdersPerLayout, static_cast<uint64_t>(orders.size()) * nextOrders.size(), { .layout = m_layout }))
                break;
            KeyOrderList longerOrders;
            longerOrders.reserveInitialCapacity(orders.size() * nextOrders.size());
            for (auto& order : orders) {
                for (auto& nextOrder : nextOrders)
                    longerOrders.append(concatenate(order.span(), nextOrder.span()));
            }
            orders = WTF::move(longerOrders);
        }
        return orders;
    }

private:
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

enum class PlaceInOrder : uint8_t { Inside, Behind };

} // anonymous namespace

// The places where the fields that a store, or a run of stores, creates may come next, the most likely place first: the keys in front of the place and the keys with the fields.
static void forEachPlace(const CreationOrders& orders, FieldList created, const Invocable<bool()> auto& isFull, const Invocable<void(FieldList, FieldList, PlaceInOrder)> auto& add)
{
    auto addPlacesInside = [&](const KeyOrderList& list) {
        for (auto& order : list) {
            if (isFull())
                return;
            if (std::optional<size_t> position = findPlaceInside(order.span(), created))
                add(order.span().first(*position), order.span().first(*position + created.size()), PlaceInOrder::Inside);
        }
    };
    addPlacesInside(orders.constructorOrders);
    bool isFilledInDeclaredOrder = !orders.hasConstructor && std::ranges::any_of(orders.declaredOrders, [&](const Vector<FieldIndex>& order) {
        std::optional<size_t> position = findPlaceInside(order.span(), created);
        return position && orders.countsReachedAmongDeclarations.get(*position);
    });
    if (isFilledInDeclaredOrder)
        addPlacesInside(orders.declaredOrders);
    // What an object has when the stores outside its constructor begin. The orders of a constructor start at the allocation.
    for (auto& order : orders.hasConstructor ? orders.constructorOrders : orders.allocationOrders) {
        if (isFull())
            break;
        bool haveCommonField = std::ranges::any_of(order, [&](FieldIndex field) {
            return std::ranges::contains(created, field);
        });
        if (!haveCommonField)
            add(order.span(), concatenate(order.span(), created).span(), PlaceInOrder::Behind);
    }
    addPlacesInside(orders.declaredOrders);
}

static BitVector findCountsReachedAmongDeclarations(const PlacementContext& context, LayoutID layout, FieldList declaredOrder, const KeyOrderList& allocationOrders, std::span<const PlacedFieldCreation> creations)
{
    BitVector isCreatedBehindAllocation;
    for (auto& creation : creations) {
        for (FieldIndex field : creation.fields)
            isCreatedBehindAllocation.set(field);
    }
    for (FieldIndex field : declaredOrder) {
        if (!fieldAt(context.program, declarationOf(context.program, { layout, field })).form.isEmpty())
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

static CreationOrders collectCreationOrders(const PlacementContext& context, const AllocationTable& table, LayoutID layout, std::span<const PlacedFieldCreation> creations)
{
    auto allocations = table.ofLayout(layout);
    CreationOrders orders;
    orders.allocationOrders = KeyOrderList(allocations.size(), [&](size_t i) {
        Vector<FieldIndex> storedFields;
        for (FieldIndex field : table.allocations[allocations[i].index()].initialFields) {
            if (hasStorage(context, layout, field))
                storedFields.append(field);
        }
        return storedFields;
    });
    ConstructorChain chain(context, layout);
    orders.constructorOrders = chain.constructorOrders();
    // Flow names no order where somebody else may get hold of the object before the first store. Then the declarations say what the constructor creates, in its sequence, as for an object without one.
    orders.hasConstructor = context.program.at(declaredLayoutOf(context.program, layout)).constructor && !std::ranges::all_of(orders.constructorOrders, &Vector<FieldIndex>::isEmpty);

    // Whatever fills an object that starts without keys, compiled or not, is expected to go by the declarations.
    bool canStartWithoutKeys = allocations.empty() || std::ranges::any_of(orders.allocationOrders, &Vector<FieldIndex>::isEmpty);
    if (canStartWithoutKeys || !creations.empty()) {
        Vector<FieldIndex> declaredOrder = chain.declaredOrder();
        if (!creations.empty())
            orders.countsReachedAmongDeclarations = findCountsReachedAmongDeclarations(context, layout, declaredOrder.span(), orders.allocationOrders, creations);
        if (!canStartWithoutKeys) {
            size_t numberOfKeysThatStoresReach = 0;
            for (auto& creation : creations) {
                std::optional<size_t> position = findPlaceInside(declaredOrder.span(), creation.fields.span());
                if (position && orders.countsReachedAmongDeclarations.get(*position))
                    numberOfKeysThatStoresReach = std::max(numberOfKeysThatStoresReach, *position + creation.fields.size());
            }
            declaredOrder.shrink(numberOfKeysThatStoresReach);
        }
        orders.declaredOrders.append(WTF::move(declaredOrder));
    }
    // An object that has the keys of its allocation or of its constructor gets more keys from stores: the keys behind a store at the places that its code will name, where no other list of the layout has them.
    for (auto& creation : creations) {
        Vector<FieldList, PlacedFieldCreation::maxContinuations> placesTaken;
        forEachPlace(orders, creation.fields.span(), [&] {
            return placesTaken.size() == PlacedFieldCreation::maxContinuations;
        }, [&](FieldList keysBefore, FieldList keysAfter, PlaceInOrder place) {
            if (placesTaken.containsIf([&](FieldList known) { return equalSpans(known, keysBefore); }))
                return;
            placesTaken.constructAndAppend(keysBefore);
            if (place == PlaceInOrder::Behind)
                orders.ordersContinuedByStores.append(Vector<FieldIndex>(keysAfter));
        });
    }
    return orders;
}

static KeyOrderList computeKeyOrders(const PlacementContext& context, std::span<const KeyOrderList* const> creationOrders, std::span<const KeyOrderTestUse> tests)
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
        BitVector isListed;
        bool canBeKeyOrder = std::ranges::all_of(test.fields, [&](FieldIndex field) {
            return hasStorage(context, test.layout, field) && isListed.add(field);
        });
        if (canBeKeyOrder)
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
    const KeyOrderList* lists[] = { &creationOrders.allocationOrders, &creationOrders.constructorOrders, &creationOrders.declaredOrders, &creationOrders.ordersContinuedByStores };
    for (auto [numberOfLists, includesTests] : { std::pair { 4, true }, std::pair { 3, true }, std::pair { 3, false } }) {
        KeyOrderList orders = computeKeyOrders(context, std::span(lists).first(numberOfLists), includesTests ? tests : std::span<const KeyOrderTestUse> { });
        if (!build.exceeds(LimitID::KeyOrdersPerLayout, orders.size(), subject))
            return orders;
    }
    KeyOrderList orders = computeKeyOrders(context, std::span(lists).first(1), { });
    if (orders.size() <= build.limit(LimitID::KeyOrdersPerLayout))
        return orders;
    build.reportLimitExceeded(LimitID::KeyOrdersPerLayout, orders.size(), subject);
    return std::nullopt;
}

static void collectLayoutKeyOrders(const PlacementContext& context, const AllocationTable& table, LayoutID layout, std::span<PlacedFieldCreation> creations, std::span<const KeyOrderTestUse> tests, KeyOrders& keyOrders, PlacedOrderWords& orderWords)
{
    CreationOrders creationOrders = collectCreationOrders(context, table, layout, creations);
    std::optional<KeyOrderList> orders = chooseKeyOrders(context, layout, creationOrders, tests);
    if (!orders)
        return;
    keyOrders.ofLayout[layout.index()] = WTF::move(*orders);
    for (PlacedFieldCreation& creation : creations) {
        forEachPlace(creationOrders, creation.fields.span(), [&] {
            return creation.continuations.size() == PlacedFieldCreation::maxContinuations;
        }, [&](FieldList keysBefore, FieldList keysAfter, PlaceInOrder) {
            std::optional<uint32_t> orderWordBefore = keyOrders.findOrderWord(layout, keysBefore);
            std::optional<uint32_t> orderWordAfter = keyOrders.findOrderWord(layout, keysAfter);
            if (!orderWordBefore || !orderWordAfter)
                return;
            KeyOrderContinuation continuation { *orderWordBefore, *orderWordAfter };
            if (!creation.continuations.contains(continuation))
                creation.continuations.append(continuation);
        });
    }

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
    keyOrders.placesOfStoresThatMayAdd = sites.placesOfStoresThatMayAdd;
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
