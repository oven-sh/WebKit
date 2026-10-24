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
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSPropertyNameEnumerator.h"
#include "SlotVisitorInlines.h"
#include "StructureChain.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(defaultLimitValue(LimitID::FieldsInKeyOrder, hostArchitecture()) == maxFieldsInOrder);
static_assert(defaultLimitValue(LimitID::KeyOrdersPerLayout, hostArchitecture()) <= maxKeyOrdersPerLayout);
static_assert(maxKeyOrdersPerLayout <= UINT16_MAX);

static constexpr uint32_t initialTransitionTableSize = 1024;
static_assert(!(initialTransitionTableSize & (initialTransitionTableSize - 1)));
// The table of transitions doubles when more than this part of it is taken. Compiled code probes it, so it stays emptier than the tables that only this file reads.
static constexpr uint64_t maxTransitionLoadNumerator = 1;
static constexpr uint64_t maxTransitionLoadDenominator = 2;

// A bit above the order and the count, in the key of a list of properties.
static constexpr uint32_t listsSymbolKeys = orderAndCountMask + 1;
static_assert(!(listsSymbolKeys & orderAndCountMask));

namespace {

// What lies in an arena never moves, and it goes away with the arena.
class Arena {
    WTF_MAKE_NONCOPYABLE(Arena);
public:
    Arena() = default;

    ~Arena()
    {
        for (void* chunk : m_chunks)
            fastFree(chunk);
    }

    template<typename T>
    T* allocate(size_t size)
    {
        static_assert(alignof(T) <= alignof(std::max_align_t));
        uintptr_t start = roundUpToMultipleOf<alignof(T)>(m_top);
        if (start + size > m_end) {
            size_t chunkSize = std::max(size, m_nextChunkSize);
            m_nextChunkSize = std::min(m_nextChunkSize * 2, maxChunkSize);
            void* chunk = fastMalloc(chunkSize);
            m_chunks.append(chunk);
            start = std::bit_cast<uintptr_t>(chunk);
            m_end = start + chunkSize;
        }
        m_top = start + size;
        return std::bit_cast<T*>(start);
    }

    // Only the allocation that was made last can get longer where it is.
    bool tryExtend(const void* endOfAllocation, size_t size)
    {
        if (std::bit_cast<uintptr_t>(endOfAllocation) != m_top || m_end - m_top < size)
            return false;
        m_top += size;
        return true;
    }

private:
    static constexpr size_t firstChunkSize = 512;
    AOT_CLASS_THRESHOLD(size_t, maxChunkSize, 16 * KB);

    Vector<void*, 4> m_chunks;
    uintptr_t m_top { 0 };
    uintptr_t m_end { 0 };
    size_t m_nextChunkSize { firstChunkSize };
};

// Open addressing in zeroed memory: an entry of zeros is free. Nothing is ever removed. The table that a larger one replaces is given back at once.
template<typename Entry>
class FlatTable {
    WTF_MAKE_NONCOPYABLE(FlatTable);
public:
    explicit FlatTable(ZeroedPages& pages)
        : m_pages(pages)
    {
    }

    ~FlatTable()
    {
        release(entries());
    }

    std::span<Entry> entries() const { return { m_entries, m_entries ? static_cast<size_t>(m_mask) + 1 : 0 }; }

    template<typename... Key>
    Entry* find(Key... key) const
    {
        if (!m_entries)
            return nullptr;
        for (uint32_t index = Entry::hashOf(key...) & m_mask; !m_entries[index].isFree(); index = (index + 1) & m_mask) {
            if (m_entries[index].hasKey(key...))
                return m_entries + index;
        }
        return nullptr;
    }

    // No entry has the key yet. The entry stays where it is until the next one is added.
    Entry& add(const Entry& entry)
    {
        ASSERT(!entry.isFree());
        if (m_size + 1 > entries().size() / 4 * 3)
            grow();
        ++m_size;
        return place(entry);
    }

private:
    static constexpr size_t initialCapacity = 8;

    Entry& place(const Entry& entry)
    {
        uint32_t index = entry.hash() & m_mask;
        while (!m_entries[index].isFree())
            index = (index + 1) & m_mask;
        m_entries[index] = entry;
        return m_entries[index];
    }

    void grow()
    {
        std::span<Entry> previous = entries();
        size_t capacity = previous.empty() ? initialCapacity : previous.size() * 2;
        RELEASE_ASSERT(capacity - 1 <= UINT32_MAX);
        m_entries = allocate(capacity);
        m_mask = static_cast<uint32_t>(capacity - 1);
        for (const Entry& entry : previous) {
            if (!entry.isFree())
                place(entry);
        }
        release(previous);
    }

    static bool fillsPages(size_t capacity) { return capacity * sizeof(Entry) >= pageSize(); }

    Entry* allocate(size_t capacity)
    {
        size_t size = capacity * sizeof(Entry);
        return static_cast<Entry*>(fillsPages(capacity) ? m_pages.allocate(size, ZeroedPages::FirstUse::AllAtOnce) : fastZeroedMalloc(size));
    }

    void release(std::span<Entry> table)
    {
        if (table.empty())
            return;
        if (fillsPages(table.size()))
            m_pages.release(table.data());
        else
            fastFree(table.data());
    }

    ZeroedPages& m_pages;
    Entry* m_entries { nullptr };
    uint32_t m_mask { 0 };
    uint32_t m_size { 0 };
};

// An order that a layout got at run time. Its entries follow it in the arena.
struct AddedOrder {
    uint16_t* data() { return reinterpret_cast<uint16_t*>(this + 1); }
    std::span<const uint16_t> entries() const { return { reinterpret_cast<const uint16_t*>(this + 1), length }; }

    uint16_t length;
    uint16_t capacity;
    // Of all its entries: it says how many extra keys the whole order positions without a walk over it.
    uint16_t largestEntry;
};

// The orders that one layout got at run time, by number: the first has the number behind the last order of the image. The array has room for the next power of two.
struct OrdersOfLayout {
    static unsigned hashOf(uint32_t shape) { return intHash(shape); }
    unsigned hash() const { return hashOf(shape); }
    bool hasKey(uint32_t otherShape) const { return shape == otherShape; }
    bool isFree() const { return !shape; }

    uint32_t shape;
    uint32_t numberOfAddedOrders;
    AddedOrder** addedOrders;
};

// The order `next` starts with the first entries of another order, as many as the count says, and goes on with `entry`, which that order doesn't.
struct Continuation {
    static unsigned hashOf(uint32_t shape, uint32_t orderAndCount, uint16_t entry) { return intHash(static_cast<uint64_t>((static_cast<uint64_t>(shape) << 32 | orderAndCount) + entry * 0x9e3779b97f4a7c15ull)); }
    unsigned hash() const { return hashOf(shape, orderAndCount, entry); }
    bool hasKey(uint32_t otherShape, uint32_t otherOrderAndCount, uint16_t otherEntry) const { return shape == otherShape && orderAndCount == otherOrderAndCount && entry == otherEntry; }
    bool isFree() const { return !shape; }

    uint32_t shape;
    uint32_t orderAndCount;
    uint16_t entry;
    uint16_t next;
};
static_assert(sizeof(Continuation) == 12);

// What was made once for all objects of one layout that have one order word.
template<typename Value>
struct Listing {
    static unsigned hashOf(uint32_t shape, uint32_t orderWord) { return intHash(static_cast<uint64_t>(shape) << 32 | orderWord); }
    unsigned hash() const { return hashOf(shape, orderWord); }
    bool hasKey(uint32_t otherShape, uint32_t otherOrderWord) const { return shape == otherShape && orderWord == otherOrderWord; }
    bool isFree() const { return !shape; }

    uint32_t shape;
    uint32_t orderWord;
    Value value;
};

struct CachedEnumerator {
    JSPropertyNameEnumerator* enumerator;
    StructureChain* prototypeChain;
};

// The properties follow it in the arena, and the text of their JSON keys follows them.
struct alignas(FieldAsProperty) FieldList {
    std::span<FieldAsProperty> fields() { return { reinterpret_cast<FieldAsProperty*>(this + 1), size }; }
    std::span<const FieldAsProperty> fields() const { return { reinterpret_cast<const FieldAsProperty*>(this + 1), size }; }

    size_t size;
};

} // anonymous namespace

// The orders of a layout form a tree.
struct KeyOrderTable::Storage {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Storage);
    WTF_MAKE_NONCOPYABLE(Storage);

    struct Place {
        uint32_t order;
        uint32_t count;
    };

    Storage(ZeroedPages&, const ProgramTables&);

    std::span<const uint16_t> entriesOf(const LayoutData&, const OrdersOfLayout&, uint32_t order) const;
    OrdersOfLayout& ordersOf(const LayoutData&);
    Place follow(const LayoutData&, const OrdersOfLayout&, std::span<const uint16_t> entries) const;
    AddedOrder* createOrder(std::span<const uint16_t> entries, size_t capacity);
    void addOrder(OrdersOfLayout&, std::span<const uint16_t> entries);
    void append(AddedOrder*&, std::span<const uint16_t> entries);
    const FieldList* createFieldList(std::span<const FieldAsProperty>);

    const ProgramTables& tables;
    // Orders have an arena of their own, so that the order that an object under construction extends is most often the last allocation.
    Arena orderArena;
    Arena listArena;
    FlatTable<OrdersOfLayout> layouts;
    OrdersOfLayout* ordersAskedForLast { nullptr };
    FlatTable<Continuation> continuations;
    FlatTable<Listing<JSCellButterfly*>> keyLists;
    FlatTable<Listing<CachedEnumerator>> enumerators;
    FlatTable<Listing<const FieldList*>> propertyLists;
};

KeyOrderTable::Storage::Storage(ZeroedPages& pages, const ProgramTables& tables)
    : tables(tables)
    , layouts(pages)
    , continuations(pages)
    , keyLists(pages)
    , enumerators(pages)
    , propertyLists(pages)
{
}

std::span<const uint16_t> KeyOrderTable::Storage::entriesOf(const LayoutData& layout, const OrdersOfLayout& orders, uint32_t order) const
{
    if (order < layout.numberOfOrders)
        return tables.keyOrder(layout, order).entries();
    uint32_t index = order - layout.numberOfOrders;
    RELEASE_ASSERT(index < orders.numberOfAddedOrders);
    return orders.addedOrders[index]->entries();
}

OrdersOfLayout& KeyOrderTable::Storage::ordersOf(const LayoutData& layout)
{
    ASSERT(layout.shape);
    // One object under construction asks several times for each of its fields.
    if (ordersAskedForLast && ordersAskedForLast->shape == layout.shape) [[likely]]
        return *ordersAskedForLast;
    if (OrdersOfLayout* known = layouts.find(layout.shape)) [[likely]] {
        ordersAskedForLast = known;
        return *known;
    }
    ASSERT(layout.numberOfOrders <= maxKeyOrdersPerLayout);
    // Nobody else adds to the table, so what is remembered stays where it is until here.
    OrdersOfLayout& orders = layouts.add({ layout.shape, 0, nullptr });
    ordersAskedForLast = &orders;
    if (!layout.numberOfOrders)
        addOrder(orders, { });
    for (uint32_t order = 1; order < layout.numberOfOrders; ++order) {
        std::span<const uint16_t> entries = tables.keyOrder(layout, order).entries();
        Place reached = follow(layout, orders, entries);
        if (reached.count < entries.size())
            continuations.add({ layout.shape, orderWordOf(reached.order, reached.count), entries[reached.count], static_cast<uint16_t>(order) });
    }
    return orders;
}

// The place of the entries in the tree or, where the count falls short of them, the place of the longest part that has one.
auto KeyOrderTable::Storage::follow(const LayoutData& layout, const OrdersOfLayout& orders, std::span<const uint16_t> entries) const -> Place
{
    uint32_t order = 0;
    uint32_t count = 0;
    std::span<const uint16_t> listed = entriesOf(layout, orders, order);
    while (count < entries.size()) {
        if (count >= listed.size() || listed[count] != entries[count]) {
            const Continuation* continuation = continuations.find(layout.shape, orderWordOf(order, count), entries[count]);
            if (!continuation)
                break;
            order = continuation->next;
            listed = entriesOf(layout, orders, order);
            ASSERT(equalSpans(listed.first(count + 1), entries.first(count + 1)));
        }
        ++count;
    }
    return { order, count };
}

AddedOrder* KeyOrderTable::Storage::createOrder(std::span<const uint16_t> entries, size_t capacity)
{
    RELEASE_ASSERT(entries.size() <= capacity && capacity <= maxFieldsInOrder);
    auto* order = orderArena.allocate<AddedOrder>(sizeof(AddedOrder) + capacity * sizeof(uint16_t));
    order->length = static_cast<uint16_t>(entries.size());
    order->capacity = static_cast<uint16_t>(capacity);
    order->largestEntry = entries.empty() ? 0 : std::ranges::max(entries);
    memcpySpan(std::span { order->data(), entries.size() }, entries);
    return order;
}

void KeyOrderTable::Storage::addOrder(OrdersOfLayout& orders, std::span<const uint16_t> entries)
{
    uint32_t index = orders.numberOfAddedOrders;
    bool isFull = !(index & (index - 1));
    if (isFull) {
        size_t capacity = index ? static_cast<size_t>(index) * 2 : 1;
        auto** grown = listArena.allocate<AddedOrder*>(capacity * sizeof(AddedOrder*));
        memcpySpan(std::span { grown, index }, std::span { orders.addedOrders, index });
        orders.addedOrders = grown;
    }
    orders.addedOrders[index] = createOrder(entries, entries.size());
    ++orders.numberOfAddedOrders;
}

// An order that has to move leaves its entries behind as they are, for whoever still reads them.
void KeyOrderTable::Storage::append(AddedOrder*& order, std::span<const uint16_t> entries)
{
    size_t length = order->length + entries.size();
    RELEASE_ASSERT(length <= maxFieldsInOrder);
    if (length > order->capacity) {
        if (orderArena.tryExtend(order->data() + order->capacity, (length - order->capacity) * sizeof(uint16_t)))
            order->capacity = static_cast<uint16_t>(length);
        else
            order = createOrder(order->entries(), std::max<size_t>(length, std::min<size_t>(order->length * 2, maxFieldsInOrder)));
    }
    memcpySpan(std::span { order->data() + order->length, entries.size() }, entries);
    order->length = static_cast<uint16_t>(length);
    for (uint16_t entry : entries)
        order->largestEntry = std::max(order->largestEntry, entry);
}

static bool hasJSONKey(const UniquedStringImpl& name)
{
    return !name.isSymbol() && name.is8Bit();
}

static bool isWrittenAsItIsInJSON(std::span<const Latin1Character> name)
{
    return std::ranges::none_of(name, [](Latin1Character character) {
        return character < ' ' || character == '"' || character == '\\';
    });
}

// Nearly every name is written between its quotes as it is. A list is made for whoever walks the fields of an order first, most often a copy, which reads no key.
const FieldList* KeyOrderTable::Storage::createFieldList(std::span<const FieldAsProperty> properties)
{
    constexpr size_t sizeAroundName = 4;
    Vector<String, 4> escapedKeys;
    Vector<unsigned, 32> sizes;
    size_t sizeOfKeys = 0;
    for (auto& property : properties) {
        unsigned size = 0;
        if (hasJSONKey(*property.name)) {
            size = property.name->length() + sizeAroundName;
            if (!isWrittenAsItIsInJSON(property.name->span8())) [[unlikely]] {
                StringBuilder key;
                key.append(',');
                key.appendQuotedJSONString(String(property.name));
                key.append(':');
                RELEASE_ASSERT(!key.hasOverflowed() && key.is8Bit());
                size = key.length();
                escapedKeys.append(key.toString());
            }
        }
        sizes.append(size);
        sizeOfKeys += size;
    }

    size_t textSize = sizeOfKeys + FieldAsProperty::jsonKeyBlockSize;
    auto* list = listArena.allocate<FieldList>(sizeof(FieldList) + properties.size_bytes() + textSize);
    list->size = properties.size();
    std::span<FieldAsProperty> fields = list->fields();
    std::span<Latin1Character> remaining { reinterpret_cast<Latin1Character*>(fields.data() + fields.size()), textSize };
    size_t numberOfEscapedKeysWritten = 0;
    for (size_t index = 0; index < fields.size(); ++index) {
        std::span<Latin1Character> key = remaining.first(sizes[index]);
        remaining = remaining.subspan(sizes[index]);
        UniquedStringImpl* name = properties[index].name;
        // An escape makes a key longer.
        if (key.size() && key.size() == name->length() + sizeAroundName) {
            key[0] = ',';
            key[1] = '"';
            memcpySpan(key.subspan(2, name->length()), name->span8());
            key[key.size() - 2] = '"';
            key[key.size() - 1] = ':';
        } else if (key.size())
            memcpySpan(key, escapedKeys[numberOfEscapedKeysWritten++].span8());
        const FieldData* field = properties[index].field;
        bool isReadFromSlot = !field->isOutOfLine() && !key.empty();
        new (NotNull, &fields[index]) FieldAsProperty { name, field, key, field->slot.offset, isReadFromSlot ? field->slot.representation() : SlotRepresentation::DeferredClosure };
    }
    zeroSpan(remaining);
    return list;
}

KeyOrderTable::KeyOrderTable(State& state)
    : m_state(state)
    , m_tables(state.tables)
    , m_storage(makeUnique<Storage>(state.pages, state.tables))
{
    Instance& instance = state.instance;
    instance.objects.keyLists = state.tableArena.allocate<KeyListCacheEntry>(numberOfKeyLists);
    instance.objects.emptyKeyList = JSArray::tryCreate(*instance.vm, instance.globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 0);
    RELEASE_ASSERT(instance.objects.emptyKeyList);
    state.immortalCells->add(instance.objects.emptyKeyList);
    instance.objects.keyOrderTransitions = static_cast<KeyOrderTransition*>(state.pages.allocate(initialTransitionTableSize * sizeof(KeyOrderTransition)));
    instance.objects.keyOrderTransitionMask = initialTransitionTableSize - 1;
    instance.objects.keyOrderRuns = static_cast<KeyOrderRun*>(state.pages.allocate(numberOfKeyOrderRuns * sizeof(KeyOrderRun)));
    instance.objects.enumerators = state.tableArena.allocate<EnumeratorCacheEntry>(numberOfCachedEnumerators);
    state.caches->add(CacheKind::StrongRoots, "key lists"_s, *this);
    // A StructureID may name another Structure after a collection.
    state.caches->addClearedEachCollection("published enumerators"_s, asWritableBytes(std::span { instance.objects.enumerators, numberOfCachedEnumerators }));
}

KeyOrderTable::~KeyOrderTable() = default;

std::span<const uint16_t> KeyOrderTable::entriesInOrder(const LayoutData& layout, uint32_t order)
{
    ASSERT(order != orderStoredOutOfLine);
    if (order < layout.numberOfOrders)
        return m_tables.keyOrder(layout, order).entries();
    // A layout without an order in the image has the empty order as its first, and a new object names it before anything has asked for the orders of the layout.
    return m_storage->entriesOf(layout, m_storage->ordersOf(layout), order);
}

std::optional<std::span<const FieldAsProperty>> KeyOrderTable::propertiesInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord, Object::SymbolKeys symbolKeys)
{
    orderWord &= orderAndCountMask;
    bool includesSymbolKeys = symbolKeys == Object::SymbolKeys::Include && layout.hasFieldsWithSymbolKeys;
    uint32_t key = orderWord | (includesSymbolKeys ? listsSymbolKeys : 0);
    if (auto* known = m_storage->propertyLists.find(layout.shape, key)) [[likely]]
        return known->value->fields();
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    Vector<FieldAsProperty, 32> list;
    for (uint16_t index : entriesInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index >= layout.numberOfFields)
            return std::nullopt;
        const FieldData& field = layout.fields()[index];
        if (field.attributes())
            return std::nullopt;
        if (layouts.hasStringKey(field) || (includesSymbolKeys && layouts.hasSymbolKey(field)))
            list.append({ layouts.nameOf(vm, field), &field, { }, 0, SlotRepresentation::DeferredClosure });
    }
    const FieldList* created = m_storage->createFieldList(list.span());
    m_storage->propertyLists.add({ layout.shape, key, created });
    return created->fields();
}

JSCellButterfly* KeyOrderTable::keysInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    orderWord &= orderAndCountMask;
    auto publish = [&](JSCellButterfly* list) {
        uint32_t shapeWord = shapeWordOf(layout.shape);
        m_state.instance.objects.keyLists[indexOfKeyList(shapeWord, orderWord)] = { orderWord, shapeWord, list };
        return list;
    };
    if (auto* known = m_storage->keyLists.find(layout.shape, orderWord))
        return publish(known->value);
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    Vector<uint16_t, 16> listed;
    for (uint16_t index : entriesInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index >= layout.numberOfFields || layout.fields()[index].attributes())
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
        m_storage->keyLists.add({ layout.shape, orderWord, list });
        m_writes.recordAll();
    }
    return publish(list);
}

bool KeyOrderTable::positionsExtraKeys(const LayoutData& layout, uint32_t orderWord, uint32_t numberOfExtraKeys)
{
    ASSERT(!isOrderStoredOutOfLine(orderWord));
    uint32_t order = orderOf(orderWord);
    if (order < layout.numberOfOrders)
        return !numberOfExtraKeys;
    // An object under construction stands at the end of the order that grows with it.
    const AddedOrder& added = *m_storage->ordersOf(layout).addedOrders[order - layout.numberOfOrders];
    ExtraKeyCounter positioned(layout.numberOfFields);
    if (countOf(orderWord) == added.length && added.largestEntry < maxKeyOrderEntry) {
        if (added.largestEntry >= layout.numberOfFields)
            positioned.add(added.largestEntry);
    } else
        positioned.addAll(added.entries().first(countOf(orderWord)));
    return positioned.count() >= numberOfExtraKeys;
}

std::optional<uint32_t> KeyOrderTable::orderStartingWith(const LayoutData& layout, std::span<const uint16_t> entries)
{
    VM& vm = *m_state.instance.vm;
    if (entries.size() > runtimeLimitValue(vm, LimitID::FieldsInKeyOrder)) {
        countLimitHit(vm, LimitID::FieldsInKeyOrder);
        return std::nullopt;
    }
    Storage& storage = *m_storage;
    OrdersOfLayout& orders = storage.ordersOf(layout);
    auto [order, count] = storage.follow(layout, orders, entries);
    uint32_t size = static_cast<uint32_t>(entries.size());
    if (count == size)
        return orderWordOf(order, size);
    for (uint16_t entry : entries.subspan(count))
        RELEASE_ASSERT(entry <= maxKeyOrderEntry);
    // The walk ended at the end of an order that was added at run time: that order grows.
    if (order >= layout.numberOfOrders) {
        AddedOrder*& reached = orders.addedOrders[order - layout.numberOfOrders];
        if (reached->length == count) {
            storage.append(reached, entries.subspan(count));
            return orderWordOf(order, size);
        }
    }
    uint32_t added = layout.numberOfOrders + orders.numberOfAddedOrders;
    if (added >= runtimeLimitValue(vm, LimitID::KeyOrdersPerLayout)) {
        countLimitHit(vm, LimitID::KeyOrdersPerLayout);
        return std::nullopt;
    }
    storage.addOrder(orders, entries);
    storage.continuations.add({ layout.shape, orderWordOf(order, count), entries[count], static_cast<uint16_t>(added) });
    return orderWordOf(added, size);
}

// No two slots of a layout overlap, and a field without storage has no slot key: one field at the most has the key.
const FieldData* KeyOrderTable::fieldAt(const LayoutData& layout, uint32_t slotKey)
{
    if (slotKey == noSlotKey)
        return nullptr;
    std::span<const FieldData> fields = layout.fields();
    FieldFoundLast& last = m_fieldsFoundLast[layout.shape % m_fieldsFoundLast.size()];
    size_t first = last.shape == layout.shape && last.next < fields.size() ? last.next : 0;
    auto hasKey = [&](const FieldData& field) {
        return slotKeyFor(field.slot.offset, field.isOutOfLine()) == slotKey;
    };
    AOT_THRESHOLD(size_t, longestWalk, 16);
    auto fieldsBehind = fields.subspan(first, std::min(longestWalk, fields.size() - first));
    auto foundBehind = std::ranges::find_if(fieldsBehind, hasKey);
    size_t index = first + (foundBehind - fieldsBehind.begin());
    if (foundBehind == fieldsBehind.end())
        index = fields.size() <= 2 * longestWalk ? std::ranges::find_if(fields, hasKey) - fields.begin() : indexOfFieldAt(layout, slotKey);
    RELEASE_ASSERT(index < fields.size());
    last = { layout.shape, static_cast<uint32_t>(index) + 1 };
    return &fields[index];
}

size_t KeyOrderTable::indexOfFieldAt(const LayoutData& layout, uint32_t slotKey)
{
    std::span<const FieldData> fields = layout.fields();
    Vector<uint64_t>& keysAndIndexes = m_fieldsBySlotKey.add(layout.shape, Vector<uint64_t> { }).iterator->value;
    if (keysAndIndexes.isEmpty()) {
        for (size_t index = 0; index < fields.size(); ++index) {
            if (uint32_t key = slotKeyFor(fields[index].slot.offset, fields[index].isOutOfLine()); key != noSlotKey)
                keysAndIndexes.append(static_cast<uint64_t>(key) << 32 | index);
        }
        AOT::sort(keysAndIndexes);
        keysAndIndexes.shrinkToFit();
    }
    auto found = std::ranges::lower_bound(keysAndIndexes, static_cast<uint64_t>(slotKey) << 32);
    RELEASE_ASSERT(found != keysAndIndexes.end() && *found >> 32 == slotKey);
    return static_cast<uint32_t>(*found);
}

std::optional<uint32_t> KeyOrderTable::orderAfterCreating(const LayoutData& layout, uint32_t orderWord, uint16_t entry, Step step)
{
    ASSERT(!isOrderStoredOutOfLine(orderWord));
    uint32_t order = orderOf(orderWord);
    uint32_t count = countOf(orderWord);
    auto publish = [&](uint32_t result) {
        if (entry < layout.numberOfFields && step == Step::IsPublished)
            publishTransition(layout, orderWord & orderAndCountMask, layout.fields()[entry], result);
        return result;
    };
    auto entries = entriesInOrder(layout, order);
    if (count < entries.size() && entries[count] == entry) [[likely]]
        return publish(orderWordOf(order, count + 1));
    Storage& storage = *m_storage;
    OrdersOfLayout& orders = storage.ordersOf(layout);
    uint32_t before = orderWordOf(order, count);
    if (const Continuation* known = storage.continuations.find(layout.shape, before, entry))
        return publish(orderWordOf(known->next, count + 1));
    // The end of an order that was added at run time is the place of its entries in the tree, and nothing continues it with this entry: that order grows.
    if (order >= layout.numberOfOrders && count == entries.size() && count < runtimeLimitValue(*m_state.instance.vm, LimitID::FieldsInKeyOrder)) {
        RELEASE_ASSERT(entry <= maxKeyOrderEntry);
        storage.append(orders.addedOrders[order - layout.numberOfOrders], singleElementSpan(entry));
        return publish(orderWordOf(order, count + 1));
    }
    Vector<uint16_t, 16> next(entries.first(count));
    next.append(entry);
    std::optional<uint32_t> result = orderStartingWith(layout, next.span());
    if (!result)
        return std::nullopt;
    // The walk has left one already if the word named the place of its entries in the tree.
    if (!storage.continuations.find(layout.shape, before, entry))
        storage.continuations.add({ layout.shape, before, entry, static_cast<uint16_t>(orderOf(*result)) });
    return publish(*result);
}

JSPropertyNameEnumerator* KeyOrderTable::enumerator(const LayoutData& layout, uint32_t orderWord, StructureChain* prototypeChain) const
{
    auto* cached = m_storage->enumerators.find(layout.shape, orderWord & orderAndCountMask);
    return cached && cached->value.prototypeChain == prototypeChain ? cached->value.enumerator : nullptr;
}

void KeyOrderTable::setEnumerator(VM&, const LayoutData& layout, uint32_t orderWord, JSPropertyNameEnumerator* enumerator, StructureChain* prototypeChain)
{
    orderWord &= orderAndCountMask;
    Locker locker { m_state.caches->lock() };
    if (auto* cached = m_storage->enumerators.find(layout.shape, orderWord))
        cached->value = { enumerator, prototypeChain };
    else
        m_storage->enumerators.add({ layout.shape, orderWord, { enumerator, prototypeChain } });
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
    if (slotKey == noSlotKey)
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
    if (++m_numberOfTransitions * maxTransitionLoadDenominator > (static_cast<uint64_t>(mask) + 1) * maxTransitionLoadNumerator)
        growTransitions();
}

// Compiled code loads the table and the mask from the instance for every probe and holds neither across a call, so nobody reads the table that is given back.
void KeyOrderTable::growTransitions()
{
    ObjectWords& words = m_state.instance.objects;
    std::span<KeyOrderTransition> previous { words.keyOrderTransitions, static_cast<size_t>(words.keyOrderTransitionMask) + 1 };
    size_t size = previous.size() * 2;
    RELEASE_ASSERT(size - 1 <= UINT32_MAX);
    uint32_t mask = static_cast<uint32_t>(size - 1);
    auto* grown = static_cast<KeyOrderTransition*>(m_state.pages.allocate(size * sizeof(KeyOrderTransition), ZeroedPages::FirstUse::AllAtOnce));
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
    m_state.pages.release(previous.data());
}

template<typename Visitor>
void KeyOrderTable::visitRoots(Visitor& visitor)
{
    m_writes.forEachRegionToVisit(visitor, [&](size_t, size_t) {
        for (auto& list : m_storage->keyLists.entries()) {
            if (!list.isFree())
                visitor.appendUnbarriered(list.value);
        }
        for (auto& cached : m_storage->enumerators.entries()) {
            if (cached.isFree())
                continue;
            visitor.appendUnbarriered(cached.value.enumerator);
            visitor.appendUnbarriered(cached.value.prototypeChain);
        }
    });
}

template void KeyOrderTable::visitRoots(AbstractSlotVisitor&);
template void KeyOrderTable::visitRoots(SlotVisitor&);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
