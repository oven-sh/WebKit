/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKeyOrderTable.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImmortalCells.h"
#include "AOTLimits.h"
#include "AOTObjectModel.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AbstractSlotVisitorInlines.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSPropertyNameEnumerator.h"
#include "SlotVisitorInlines.h"
#include "StructureChain.h"
#include <wtf/FixedVector.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(defaultLimitValue(LimitID::FieldsInKeyOrder, hostArchitecture()) == maxFieldsInOrder);
static_assert(defaultLimitValue(LimitID::KeyOrdersPerLayout, hostArchitecture()) == maxKeyOrdersPerLayout);
static_assert(maxKeyOrdersPerLayout <= UINT16_MAX);

static constexpr uint32_t initialTransitionTableSize = 1024;
static_assert(!(initialTransitionTableSize & (initialTransitionTableSize - 1)));

static constexpr uint64_t emptyFieldsHash = 0xcbf29ce484222325ull;

static uint64_t hashWithField(uint64_t hash, uint16_t field)
{
    return (hash ^ (field + 1)) * 0x100000001b3ull;
}

static uint64_t keyOf(uint64_t hash)
{
    return hash >> 1;
}

struct KeyOrderTable::OrderTable {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(OrderTable);

    using Map = UncheckedKeyHashMap<uint64_t, uint16_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    template<typename T> using ByOrderWord = UncheckedKeyHashMap<uint32_t, T, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    struct ExtraKeysInOrder {
        uint32_t count { 0 };
        uint16_t end { 0 };
        ExtraKeyCounter counter { 0 };
    };

    struct CachedEnumerator {
        JSPropertyNameEnumerator* enumerator { nullptr };
        StructureChain* prototypeChain { nullptr };
    };

    struct FieldList {
        FieldList() = default;
        explicit FieldList(Vector<FieldAsProperty>&&);

        FixedVector<FieldAsProperty> fields;
        FixedVector<Latin1Character> jsonKeys;
    };

    void updateExtraKeys(const LayoutData&, std::span<const uint16_t> entries, uint32_t order);
    void didAppendEntry(const LayoutData&, std::span<const uint16_t> entries, uint32_t order);

    Vector<Vector<uint16_t>> added;
    Vector<uint64_t> hashesOfAdded;
    Map byPrefix;
    bool hasImageOrderPrefixes { false };
    Map transitions;
    ByOrderWord<JSCellButterfly*> keyLists;
    ByOrderWord<CachedEnumerator> enumerators;
    ByOrderWord<FieldList> propertyLists;
    ByOrderWord<FieldList> propertyListsWithSymbolKeys;
    Vector<ExtraKeysInOrder> extraKeysInAddedOrders;
    Vector<std::optional<uint16_t>> fieldsBySlot;
    Vector<std::optional<uint16_t>> fieldsByExtensionSlot;
};

KeyOrderTable::OrderTable::FieldList::FieldList(Vector<FieldAsProperty>&& list)
    : fields(WTF::move(list))
{
    Vector<Latin1Character, 128> text;
    Vector<size_t, 16> sizes;
    for (auto& property : fields) {
        StringBuilder key;
        if (!property.name->isSymbol()) {
            key.append(',');
            key.appendQuotedJSONString(String(property.name));
            key.append(':');
        }
        bool fitsLatin1Text = key.is8Bit();
        if (fitsLatin1Text)
            text.append(key.span8());
        sizes.append(fitsLatin1Text ? key.length() : 0);
    }
    text.insertFill(text.size(), 0, FieldAsProperty::jsonKeyBlockSize);
    jsonKeys = FixedVector<Latin1Character>(WTF::move(text));

    std::span<const Latin1Character> remaining = jsonKeys.span();
    for (size_t index = 0; index < fields.size(); ++index) {
        fields[index].jsonKey = remaining.first(sizes[index]);
        remaining = remaining.subspan(sizes[index]);
    }
}

KeyOrderTable::KeyOrderTable(State& state)
    : m_state(state)
    , m_tables(state.tables)
{
    Instance& instance = state.instance;
    instance.objects.keyLists = state.tableArena.allocate<KeyListCacheEntry>(numberOfKeyLists);
    instance.objects.emptyKeyList = JSArray::tryCreate(*instance.vm, instance.globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 0);
    RELEASE_ASSERT(instance.objects.emptyKeyList);
    state.immortalCells->add(instance.objects.emptyKeyList);
    instance.objects.keyOrderTransitions = static_cast<KeyOrderTransition*>(state.pages.allocate(initialTransitionTableSize * sizeof(KeyOrderTransition)));
    instance.objects.keyOrderTransitionMask = initialTransitionTableSize - 1;
    instance.objects.enumerators = state.tableArena.allocate<EnumeratorCacheEntry>(numberOfCachedEnumerators);
    state.caches->add(CacheKind::StrongRoots, "key lists"_s, *this);
    // A StructureID may name another Structure after a collection.
    state.caches->addClearedEachCollection("published enumerators"_s, asWritableBytes(std::span { instance.objects.enumerators, numberOfCachedEnumerators }));
}

KeyOrderTable::~KeyOrderTable() = default;

std::span<const uint16_t> KeyOrderTable::entriesInOrder(const LayoutData& layout, uint32_t order)
{
    ASSERT(order != orderStoredOutOfLine);
    if (order >= layout.numberOfOrders)
        return orderTableFor(layout).added[order - layout.numberOfOrders].span();
    return m_tables.keyOrder(layout, order).entries();
}

std::optional<std::span<const FieldAsProperty>> KeyOrderTable::propertiesInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord, Object::SymbolKeys symbolKeys)
{
    orderWord &= orderAndCountMask;
    OrderTable& table = orderTableFor(layout);
    bool includesSymbolKeys = symbolKeys == Object::SymbolKeys::Include && layout.hasFieldsWithSymbolKeys;
    auto& lists = includesSymbolKeys ? table.propertyListsWithSymbolKeys : table.propertyLists;
    if (auto known = lists.find(orderWord); known != lists.end()) [[likely]]
        return known->value.fields.span();
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    Vector<FieldAsProperty> list;
    for (uint16_t index : entriesInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index >= layout.numberOfFields)
            return std::nullopt;
        const FieldData& field = layout.fields()[index];
        if (layouts.hasStringKey(field) || (includesSymbolKeys && layouts.hasSymbolKey(field)))
            list.append({ layouts.nameOf(vm, field), &field, { } });
    }
    return lists.add(orderWord, OrderTable::FieldList(WTF::move(list))).iterator->value.fields.span();
}

JSCellButterfly* KeyOrderTable::keysInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    orderWord &= orderAndCountMask;
    auto publish = [&](JSCellButterfly* list) {
        uint32_t shapeWord = shapeWordOf(layout.shape);
        m_state.instance.objects.keyLists[indexOfKeyList(shapeWord, orderWord)] = { orderWord, shapeWord, list };
        return list;
    };
    OrderTable& table = orderTableFor(layout);
    if (JSCellButterfly* known = table.keyLists.get(orderWord))
        return publish(known);
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    Vector<uint16_t, 16> listed;
    for (uint16_t index : entriesInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index >= layout.numberOfFields)
            return nullptr;
        if (layouts.hasStringKey(layout.fields()[index]))
            listed.append(index);
    }
    JSCellButterfly* list = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, listed.size());
    if (!list)
        return nullptr;
    for (unsigned i = 0; i < listed.size(); ++i)
        list->setIndex(vm, i, jsOwnedString(vm, String(layouts.nameOf(vm, layout.fields()[listed[i]]))));
    {
        Locker locker { m_state.caches->lock() };
        table.keyLists.add(orderWord, list);
        m_writes.recordAll();
    }
    return publish(list);
}

KeyOrderTable::OrderTable& KeyOrderTable::orderTableFor(const LayoutData& layout)
{
    if (OrderTable* table = m_orderTables.get(layout.shape)) [[likely]]
        return *table;
    ASSERT(layout.numberOfOrders <= maxKeyOrdersPerLayout);
    auto created = makeUnique<OrderTable>();
    if (!layout.numberOfOrders) {
        created->added.append(Vector<uint16_t>());
        created->hashesOfAdded.append(emptyFieldsHash);
        created->extraKeysInAddedOrders.append(OrderTable::ExtraKeysInOrder { });
        created->byPrefix.add(keyOf(emptyFieldsHash), 0);
    }
    Locker locker { m_state.caches->lock() };
    return *m_orderTables.add(layout.shape, WTF::move(created)).iterator->value;
}

bool KeyOrderTable::positionsExtraKeys(const LayoutData& layout, uint32_t orderWord, uint32_t numberOfExtraKeys)
{
    ASSERT(!isOrderStoredOutOfLine(orderWord));
    uint32_t order = orderOf(orderWord);
    OrderTable::ExtraKeysInOrder positioned;
    if (order >= layout.numberOfOrders)
        positioned = orderTableFor(layout).extraKeysInAddedOrders[order - layout.numberOfOrders];
    return countOf(orderWord) >= positioned.end && positioned.count >= numberOfExtraKeys;
}

std::optional<uint32_t> KeyOrderTable::orderStartingWith(const LayoutData& layout, std::span<const uint16_t> entries, PrefixRegistration registration)
{
    VM& vm = *m_state.instance.vm;
    if (entries.size() > runtimeLimitValue(vm, LimitID::FieldsInKeyOrder)) {
        countLimitHit(vm, LimitID::FieldsInKeyOrder);
        return std::nullopt;
    }
    OrderTable& table = orderTableFor(layout);
    if (!table.hasImageOrderPrefixes) {
        table.hasImageOrderPrefixes = true;
        for (uint32_t order = 0; order < layout.numberOfOrders; ++order) {
            uint64_t hash = emptyFieldsHash;
            table.byPrefix.add(keyOf(hash), static_cast<uint16_t>(order));
            for (uint16_t entry : entriesInOrder(layout, order)) {
                hash = hashWithField(hash, entry);
                table.byPrefix.add(keyOf(hash), static_cast<uint16_t>(order));
            }
        }
    }
    Vector<uint64_t, 16> hashes { emptyFieldsHash };
    for (uint16_t entry : entries) {
        RELEASE_ASSERT(entry <= maxKeyOrderEntry);
        hashes.append(hashWithField(hashes.last(), entry));
    }
    auto find = [&](size_t length) -> std::optional<uint16_t> {
        auto found = table.byPrefix.find(keyOf(hashes[length]));
        if (found == table.byPrefix.end())
            return std::nullopt;
        auto all = entriesInOrder(layout, found->value);
        if (all.size() < length || !equalSpans(all.first(length), entries.first(length)))
            return std::nullopt;
        return found->value;
    };
    if (auto order = find(entries.size()))
        return orderWordOf(*order, entries.size());
    if (auto order = entries.empty() ? std::nullopt : find(entries.size() - 1); order && *order >= layout.numberOfOrders) {
        auto& all = table.added[*order - layout.numberOfOrders];
        if (all.size() + 1 == entries.size()) {
            all.append(entries.back());
            table.hashesOfAdded[*order - layout.numberOfOrders] = hashes.last();
            table.byPrefix.add(keyOf(hashes.last()), *order);
            table.didAppendEntry(layout, all.span(), *order);
            return orderWordOf(*order, entries.size());
        }
    }
    uint32_t order = layout.numberOfOrders + table.added.size();
    if (order >= runtimeLimitValue(vm, LimitID::KeyOrdersPerLayout)) {
        countLimitHit(vm, LimitID::KeyOrdersPerLayout);
        return std::nullopt;
    }
    table.added.append(Vector<uint16_t>(entries));
    table.hashesOfAdded.append(hashes.last());
    for (uint64_t hash : registration == PrefixRegistration::EveryPrefix ? hashes.span() : hashes.span().last(1))
        table.byPrefix.add(keyOf(hash), static_cast<uint16_t>(order));
    table.updateExtraKeys(layout, entries, order);
    return orderWordOf(order, entries.size());
}

void KeyOrderTable::OrderTable::updateExtraKeys(const LayoutData& layout, std::span<const uint16_t> entries, uint32_t order)
{
    ASSERT(order >= layout.numberOfOrders);
    size_t index = order - layout.numberOfOrders;
    if (index >= extraKeysInAddedOrders.size())
        extraKeysInAddedOrders.grow(index + 1);
    extraKeysInAddedOrders[index] = { .counter = ExtraKeyCounter(layout.numberOfFields) };
    for (size_t size = 1; size <= entries.size(); ++size)
        didAppendEntry(layout, entries.first(size), order);
}

void KeyOrderTable::OrderTable::didAppendEntry(const LayoutData& layout, std::span<const uint16_t> entries, uint32_t order)
{
    if (entries.back() < layout.numberOfFields)
        return;
    ExtraKeysInOrder& extraKeys = extraKeysInAddedOrders[order - layout.numberOfOrders];
    extraKeys.counter.add(entries.back());
    if (extraKeys.counter.count() <= extraKeys.count)
        return;
    extraKeys.count = extraKeys.counter.count();
    extraKeys.end = static_cast<uint16_t>(entries.size());
}

const FieldData* KeyOrderTable::fieldAt(const LayoutData& layout, uint32_t slotKey)
{
    if (slotKey == noSlotKey)
        return nullptr;
    OrderTable& table = orderTableFor(layout);
    if (table.fieldsBySlot.isEmpty()) [[unlikely]] {
        table.fieldsBySlot.fill(std::nullopt, layout.size);
        table.fieldsByExtensionSlot.fill(std::nullopt, layout.extensionSize());
        for (const FieldData& field : layout.fields()) {
            auto& fieldsBySlot = field.isOutOfLine() ? table.fieldsByExtensionSlot : table.fieldsBySlot;
            if (field.slot.offset < fieldsBySlot.size())
                fieldsBySlot[field.slot.offset] = layout.indexOf(field);
        }
    }
    std::optional<uint16_t> index = slotKey & outOfLineSlotKey ? table.fieldsByExtensionSlot[slotKey & ~outOfLineSlotKey] : table.fieldsBySlot[slotKey];
    RELEASE_ASSERT(index && *index < layout.numberOfFields);
    return &layout.fields()[*index];
}

std::optional<uint32_t> KeyOrderTable::orderAfterCreating(const LayoutData& layout, uint32_t orderWord, uint16_t entry)
{
    ASSERT(!isOrderStoredOutOfLine(orderWord));
    uint32_t order = orderOf(orderWord);
    uint32_t count = countOf(orderWord);
    auto publish = [&](uint32_t result) {
        if (entry < layout.numberOfFields)
            publishTransition(layout, orderWord & orderAndCountMask, layout.fields()[entry], result);
        return result;
    };
    auto entries = entriesInOrder(layout, order);
    if (count < entries.size() && entries[count] == entry) [[likely]]
        return publish(orderWordOf(order, count + 1));
    OrderTable& table = orderTableFor(layout);
    uint64_t key = static_cast<uint64_t>(order) << 32 | count << 16 | entry;
    if (auto found = table.transitions.find(key); found != table.transitions.end())
        return publish(orderWordOf(found->value, count + 1));
    // The object is at the end of an order that was added at run time, and no order continues with this entry: that order grows.
    if (order >= layout.numberOfOrders && count == entries.size() && count < runtimeLimitValue(*m_state.instance.vm, LimitID::FieldsInKeyOrder)) {
        RELEASE_ASSERT(entry <= maxKeyOrderEntry);
        size_t index = order - layout.numberOfOrders;
        uint64_t hash = hashWithField(table.hashesOfAdded[index], entry);
        if (table.byPrefix.add(keyOf(hash), static_cast<uint16_t>(order)).isNewEntry) {
            Vector<uint16_t>& all = table.added[index];
            all.append(entry);
            table.hashesOfAdded[index] = hash;
            table.didAppendEntry(layout, all.span(), order);
            return publish(orderWordOf(order, count + 1));
        }
    }
    Vector<uint16_t, 16> next(entries.first(count));
    next.append(entry);
    std::optional<uint32_t> result = orderStartingWith(layout, next.span());
    if (!result)
        return std::nullopt;
    table.transitions.add(key, static_cast<uint16_t>(orderOf(*result)));
    return publish(*result);
}

JSPropertyNameEnumerator* KeyOrderTable::enumerator(const LayoutData& layout, uint32_t orderWord, StructureChain* prototypeChain) const
{
    OrderTable* table = m_orderTables.get(layout.shape);
    if (!table)
        return nullptr;
    OrderTable::CachedEnumerator cached = table->enumerators.get(orderWord & orderAndCountMask);
    return cached.prototypeChain == prototypeChain ? cached.enumerator : nullptr;
}

void KeyOrderTable::setEnumerator(VM&, const LayoutData& layout, uint32_t orderWord, JSPropertyNameEnumerator* enumerator, StructureChain* prototypeChain)
{
    OrderTable& table = orderTableFor(layout);
    Locker locker { m_state.caches->lock() };
    table.enumerators.set(orderWord & orderAndCountMask, OrderTable::CachedEnumerator { enumerator, prototypeChain });
    m_writes.recordAll();
}

// Any other object gets its first index without a report to ChainDependents.
static bool reportsEveryChangeOfKeys(JSObject* prototype)
{
    return prototype->structure()->hasImmutableProperties() || prototype->classInfo() == ClassPrototype::info();
}

void KeyOrderTable::publishEnumerator(const Object& object, JSPropertyNameEnumerator* enumerator)
{
    if (enumerator->cachedStructureID() != object.structureID())
        return;
    ChainObjects chain;
    for (JSObject* prototype = object.getPrototypeDirect().getObject(); prototype; prototype = prototype->getPrototypeDirect().getObject()) {
        if (!reportsEveryChangeOfKeys(prototype))
            return;
        chain.append(prototype);
    }
    if (!m_ownerOfPublishedEnumerators)
        m_ownerOfPublishedEnumerators = m_state.chainDependents->addOwner(*this);
    m_state.chainDependents->add(chain.span(), { nullptr, 0, *m_ownerOfPublishedEnumerators });

    uint32_t structureID = object.structureID().bits();
    m_state.instance.objects.enumerators[indexOfCachedEnumerator(structureID, object.orderWord())] = { object.orderWord(), structureID, enumerator };
}

void KeyOrderTable::dropChainDependent(VM&, uint32_t, UniquedStringImpl*)
{
    zeroSpan(std::span { m_state.instance.objects.enumerators, numberOfCachedEnumerators });
}

bool KeyOrderTable::hasChainDependent(VM&, uint32_t, UniquedStringImpl*) const
{
    return true;
}

// Only the thread that runs the instance adds transitions, so it reads them as compiled code does, without the lock.
std::optional<uint32_t> KeyOrderTable::publishedTransition(const LayoutData& layout, uint32_t orderAndCountBefore, const FieldData& field, uint32_t numberOfExtraKeys) const
{
    ASSERT(!(orderAndCountBefore & ~orderAndCountMask));
    uint32_t slotKey = slotKeyFor(field.slot.offset, field.isOutOfLine());
    if (slotKey == noSlotKey)
        return std::nullopt;
    uint32_t key = transitionKeyFor(slotKey, numberOfExtraKeys);
    uint64_t before = static_cast<uint64_t>(shapeWordOf(layout.shape)) << 32 | orderAndCountBefore;
    const ObjectWords& words = m_state.instance.objects;
    uint32_t mask = words.keyOrderTransitionMask;
    for (uint32_t index = KeyOrderTransition::hashOf(before, key) & mask; !words.keyOrderTransitions[index].isEmpty(); index = (index + 1) & mask) {
        const KeyOrderTransition& known = words.keyOrderTransitions[index];
        if (known.orderAndShapeWordsBefore == before && known.key == key)
            return known.orderAndCountAfter;
    }
    return std::nullopt;
}

void KeyOrderTable::publishTransition(const LayoutData& layout, uint32_t orderAndCountBefore, const FieldData& field, uint32_t orderAndCountAfter, uint32_t numberOfExtraKeys)
{
    ASSERT(!((orderAndCountBefore | orderAndCountAfter) & ~orderAndCountMask));
    uint32_t slotKey = slotKeyFor(field.slot.offset, field.isOutOfLine());
    if (slotKey == noSlotKey || publishedTransition(layout, orderAndCountBefore, field, numberOfExtraKeys))
        return;
    uint32_t key = transitionKeyFor(slotKey, numberOfExtraKeys);
    uint64_t before = static_cast<uint64_t>(shapeWordOf(layout.shape)) << 32 | orderAndCountBefore;
    ObjectWords& words = m_state.instance.objects;
    Locker locker { m_state.caches->lock() };
    uint32_t mask = words.keyOrderTransitionMask;
    uint32_t index = KeyOrderTransition::hashOf(before, key) & mask;
    while (!words.keyOrderTransitions[index].isEmpty()) {
        const KeyOrderTransition& known = words.keyOrderTransitions[index];
        if (known.orderAndShapeWordsBefore == before && known.key == key)
            return;
        index = (index + 1) & mask;
    }
    KeyOrderTransition& transition = words.keyOrderTransitions[index];
    transition.orderAndCountAfter = orderAndCountAfter;
    transition.key = key;
    WTF::storeStoreFence();
    transition.orderAndShapeWordsBefore = before;
    if (++m_numberOfTransitions * 2 > mask + 1)
        growTransitions();
}

void KeyOrderTable::growTransitions()
{
    ObjectWords& words = m_state.instance.objects;
    std::span<const KeyOrderTransition> previous { words.keyOrderTransitions, static_cast<size_t>(words.keyOrderTransitionMask) + 1 };
    size_t size = previous.size() * 2;
    RELEASE_ASSERT(size - 1 <= UINT32_MAX);
    uint32_t mask = static_cast<uint32_t>(size - 1);
    auto* grown = static_cast<KeyOrderTransition*>(m_state.pages.allocate(size * sizeof(KeyOrderTransition)));
    for (const KeyOrderTransition& transition : previous) {
        if (transition.isEmpty())
            continue;
        uint32_t index = KeyOrderTransition::hashOf(transition.orderAndShapeWordsBefore, transition.key) & mask;
        while (!grown[index].isEmpty())
            index = (index + 1) & mask;
        grown[index] = transition;
    }
    words.keyOrderTransitions = grown;
    words.keyOrderTransitionMask = mask;
}

template<typename Visitor>
void KeyOrderTable::visitRoots(Visitor& visitor)
{
    m_writes.forEachRegionToVisit(visitor, [&](size_t, size_t) {
        for (auto& table : m_orderTables.values()) {
            for (JSCellButterfly* list : table->keyLists.values())
                visitor.appendUnbarriered(list);
            for (auto& cached : table->enumerators.values()) {
                visitor.appendUnbarriered(cached.enumerator);
                visitor.appendUnbarriered(cached.prototypeChain);
            }
        }
    });
}

template void KeyOrderTable::visitRoots(AbstractSlotVisitor&);
template void KeyOrderTable::visitRoots(SlotVisitor&);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
