/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedData.h"
#include "JSCellButterfly.h"
#include "JSOrderedHashTable.h"
#include "JSString.h"
#include "MarkedBlockCage.h"

namespace JSC { namespace AOT { namespace Typed {

// The table of a Map or a Set all of whose keys are strings, in a realm that runs a program compiled ahead of time:
// JSOrderedHashTable::m_compactTable. It is made when the first key is added. Lookups, additions, removals, clear() and the size work on it.
// Whatever needs the engine's own storage (an iterator, a key that is no string) replaces it by that first: JSOrderedHashTable::hasStorage().
//
//      0  JSCell
//      8  used: entries written, alive or deleted      12  deleted
//     16  capacity, in entries                         20  log2 of the number of index slots, width of a key, of a value, state
//     24  index
//         keys[capacity]       the string: Cell32, or a pointer. 0: deleted
//         values[capacity]     a JSValue, or its low half if that is at most undefinedCell32, else a Cell32. A Set has none
//
// Entries are in insertion order. Up to maxCapacityWithTags entries the index is a byte of the hash for each entry, in the order of the
// entries. Beyond, it is open-addressed: a slot holds bits of the hash above the number of an entry, all ones if it is free. The capacity
// is even and the size of the index a multiple of 8, so keys and values are aligned. AOTStubs.cpp knows all this.
//
// A table with tags that no iteration has been at is cleared in place: nobody holds the number of an entry of it.
// A table that is replaced (it has grown, was rehashed, was converted) or cleared otherwise stays as it is for whoever iterates over it, but for:
//      8  the successor: a CompactTable or a JSCellButterfly. None if it was cleared: then whatever the owner has now, from its start
//     16  the number of entries that were deleted      24  their numbers, ascending, 32 bits each
class CompactTable final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    // In bytes.
    enum class Width : uint8_t {
        None = 0,
        Narrow = 4,
        Wide = 8,
    };

    enum class State : uint8_t {
        Live,
        // Live, and an iteration may be at it or at a table it has replaced.
        Iterated,
        Replaced,
        Cleared,
    };

    static constexpr uint32_t initialCapacity = 4;
    static constexpr uint32_t maxCapacityWithTags = 8;
    static constexpr uint32_t maxCapacityWithShortSlots = 254;
    static constexpr unsigned maxIndexBitsWithShortSlots = 8;
    static constexpr uint8_t keysAreWide = 0x80;
    static constexpr uint8_t keysShiftWithTags = 2;
    static_assert(2u << keysShiftWithTags == maxCapacityWithTags);
    static constexpr uint32_t maxCapacity = (1 << 20) - 2;
    static constexpr unsigned entryBitsOfShortSlot = 8;
    static constexpr unsigned entryBitsOfLongSlot = 20;
    // A hash has 24 bits. A slot is chosen by the low ones.
    static constexpr unsigned shiftOfShortTag = 16;
    static constexpr unsigned shiftOfLongTag = 12;

    static Structure* createStructure(VM&);
    static CompactTable* create(VM&, Structure*, Width key, Width value, uint32_t leastCapacity);

    uint32_t size() const { return m_alive; }
    uint32_t used() const { return m_used; }
    uint32_t capacity() const { return m_capacity; }
    Width keyWidth() const { return m_keysShift & keysAreWide ? Width::Wide : Width::Narrow; }
    Width valueWidth() const { return m_valueWidth; }
    State state() const { return m_state; }
    bool isLive() const { return m_state <= State::Iterated; }
    // Whoever keeps a Position does this first. next() does not.
    void markAsIterated()
    {
        ASSERT(isLive());
        m_state = State::Iterated;
    }

    // The number of the entry. A rope is resolved, which may throw, but for a part of another string, as `slice` yields it: that is looked
    // for by its characters where they are, and comes to share those of the key that is found.
    std::optional<uint32_t> find(JSGlobalObject*, JSValue key) const;
    std::optional<uint32_t> find(std::span<const Latin1Character>) const;
    JSString* keyAt(uint32_t entry) const;
    ALWAYS_INLINE JSValue valueAt(uint32_t entry) const
    {
        switch (m_valueWidth) {
        case Width::None:
            return { };
        case Width::Wide:
            return JSValue::decode(at<EncodedJSValue>(offsetOfValues())[entry]);
        case Width::Narrow:
            break;
        }
        static_assert(absentCell32 == static_cast<uint32_t>(JSValue::ValueEmpty) && nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);
        uint32_t index = at<uint32_t>(offsetOfValues())[entry];
        return index <= undefinedCell32 ? JSValue::decode(index) : JSValue(MarkedBlockCage::decode(index));
    }
    std::span<const uint32_t> narrowKeys() const { return { at<uint32_t>(offsetOfKeys()), m_used }; }
    CompactTable* copy(VM&) const;
    template<typename Traits> static void forEachKey(JSOrderedHashTable<Traits>* owner, NOESCAPE const auto& functor)
    {
        auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
        for (uint32_t entry = 0, used = table->m_used; entry < used; ++entry) {
            if (JSString* key = table->keyAt(entry); key && functor(key) == IterationStatus::Done)
                return;
        }
    }
    enum class Quickly : uint8_t { Found, Absent, Unknown };
    ALWAYS_INLINE Quickly tryFindQuickly(JSValue value, uint32_t& found) const
    {
        if (!value.isString())
            return Quickly::Absent;
        JSString* key = asString(value);
        const StringImpl* impl = key->tryGetValueImpl();
        if (!impl || !impl->hasHash() || m_keysShift & keysAreWide)
            return Quickly::Unknown;
        unsigned hash = impl->existingHash();
        uint32_t narrowKey = static_cast<uint32_t>(std::bit_cast<uintptr_t>(key) >> MarkedBlockCage::shift);
        auto compare = [&](const uint32_t* keys, uint32_t entry) ALWAYS_INLINE_LAMBDA {
            uint32_t candidate = keys[entry];
            if (candidate == narrowKey)
                return Quickly::Found;
            if (!candidate)
                return Quickly::Absent;
            const StringImpl* other = uncheckedDowncast<JSString>(MarkedBlockCage::decode(candidate))->tryGetValueImpl();
            if (other == impl)
                return Quickly::Found;
            return other->existingHash() == hash ? Quickly::Unknown : Quickly::Absent;
        };
        uint8_t tag = hash >> shiftOfShortTag;
        if (hasTags()) {
            if (!tag)
                return Quickly::Unknown;
            constexpr uint64_t ones = 0x0101010101010101ull;
            const uint32_t* keys = at<uint32_t>(offsetOfIndex() + maxCapacityWithTags);
            uint64_t difference = *at<uint64_t>(offsetOfIndex()) ^ tag * ones;
            uint64_t flags = (difference - ones) & ~difference & ones << 7;
            if (!flags)
                return Quickly::Absent;
            found = WTF::ctz(flags) / 8;
            if (Quickly first = compare(keys, found); first != Quickly::Absent)
                return first;
            uint32_t used = m_used;
            for (flags &= flags - 1; flags; flags &= flags - 1) {
                found = WTF::ctz(flags) / 8;
                if (found >= used)
                    break;
                if (Quickly next = compare(keys, found); next != Quickly::Absent)
                    return next;
            }
            return Quickly::Absent;
        }
        if (m_indexBits > maxIndexBitsWithShortSlots)
            return Quickly::Unknown;
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        const uint16_t* slots = at<uint16_t>(offsetOfIndex());
        uint32_t mask = (1u << m_indexBits) - 1;
        for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
            uint16_t slot = slots[i];
            if (slot == UINT16_MAX)
                return Quickly::Absent;
            if (slot >> entryBitsOfShortSlot != tag)
                continue;
            found = slot & ((1 << entryBitsOfShortSlot) - 1);
            if (Quickly next = compare(keys, found); next != Quickly::Absent)
                return next;
        }
    }

    enum class CreateIfNeeded : bool { No, Yes };
    template<typename Traits> static bool tryAdd(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue key, JSValue, CreateIfNeeded = CreateIfNeeded::No, uint32_t leastCapacity = initialCapacity);
    // The owner has a table.
    template<typename Traits> static bool remove(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue key);
    template<typename Traits> static void clear(VM&, JSOrderedHashTable<Traits>*);
    template<typename Traits> static void convert(VM&, JSOrderedHashTable<Traits>*);

    // An iteration is at a table and the number of an entry. No table: at the start of whatever the owner has.
    struct Position {
        // None: the iteration is over.
        JSCell* table { nullptr };
        uint32_t entry { 0 };
        JSValue key;
        JSValue value;
    };
    // The first entry that is alive from there on. The table may have been replaced, any number of times, also by the engine's storage.
    template<typename Traits> static Position next(VM&, JSOrderedHashTable<Traits>*, JSCell* table, uint32_t entry);
    // Of a JSMapIterator or a JSSetIterator that isAtCompactTable(). While it is at such a table it has no storage, and from its first step
    // on its Field::Entry holds a cell with the table and the number of the entry to go on from.
    template<typename Traits, typename Iterator> static Position stepIterator(VM&, Iterator*);
    static JSCell* copyOfPosition(VM&, JSCell*);

    static constexpr ptrdiff_t offsetOfUsed() { return OBJECT_OFFSETOF(CompactTable, m_used); }
    static constexpr ptrdiff_t offsetOfAlive() { return OBJECT_OFFSETOF(CompactTable, m_alive); }
    static constexpr ptrdiff_t offsetOfCapacity() { return OBJECT_OFFSETOF(CompactTable, m_capacity); }
    static constexpr ptrdiff_t offsetOfIndexBits() { return OBJECT_OFFSETOF(CompactTable, m_indexBits); }
    static constexpr ptrdiff_t offsetOfKeysShift() { return OBJECT_OFFSETOF(CompactTable, m_keysShift); }
    static constexpr ptrdiff_t offsetOfValueWidth() { return OBJECT_OFFSETOF(CompactTable, m_valueWidth); }
    static constexpr ptrdiff_t offsetOfState() { return OBJECT_OFFSETOF(CompactTable, m_state); }
    // Of a table as compiled code makes it for a first entry: initialCapacity, tags, narrow keys. The second: the eight bytes from the capacity on.
    static constexpr size_t sizeAtBirth(Width value) { return sizeof(CompactTable) + maxCapacityWithTags + initialCapacity * (static_cast<size_t>(Width::Narrow) + static_cast<size_t>(value)); }
    static constexpr uint64_t capacityAndKindsAtBirth(Width value) { return initialCapacity | static_cast<uint64_t>(keysShiftWithTags) << 40 | static_cast<uint64_t>(value) << 48; }
    static constexpr ptrdiff_t offsetOfIndex() { return sizeof(CompactTable); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

private:
    CompactTable(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    static unsigned indexBitsFor(uint32_t capacity);
    static size_t offsetOfKeys(uint32_t capacity);
    static size_t allocationSize(uint32_t capacity, Width key, Width value);

    template<typename T> T* at(size_t offset) const { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(const_cast<CompactTable*>(this)) + offset); }
    size_t offsetOfKeys() const { return offsetOfIndex() + (static_cast<size_t>(2) << (m_keysShift & ~keysAreWide)); }
    size_t offsetOfValues() const { return offsetOfKeys() + static_cast<size_t>(m_capacity) * static_cast<size_t>(keyWidth()); }
    bool hasTags() const { return !m_indexBits; }
    uint32_t deleted() const { return m_used - m_alive; }
    bool hasShortSlots() const { return m_capacity <= maxCapacityWithShortSlots; }
    template<typename Visitor> void visitEntries(Visitor&, uint32_t first, uint32_t last) const;

    static bool canHold(Width, JSValue);
    void setKey(uint32_t entry, JSString*);
    void setValue(uint32_t entry, JSValue);
    template<typename Matches> std::optional<uint32_t> findWithHash(unsigned hash, NOESCAPE const Matches&) const;
    void addToIndex(uint32_t entry, unsigned hash);
    // Of a table that has no entries yet, but for the keys of that many, none of them deleted.
    void fillIndex(uint32_t count);
    // Behind all entries. There is room.
    void append(VM&, JSString* key, unsigned hash, JSValue);
    // A table with the entries of this one that are alive, and room for one more at least. None: that would be too many.
    CompactTable* copyWithRoom(VM&, Width value) const;
    void becomeObsolete(VM&, State, JSCell* successor);

    JSCell*& successor() { return *at<JSCell*>(OBJECT_OFFSETOF(CompactTable, m_used)); }
    uint32_t& numberOfDeletedEntries() { return m_capacity; }
    uint32_t* deletedEntries() { return at<uint32_t>(offsetOfIndex()); }

    uint32_t m_used { 0 };
    uint32_t m_alive { 0 };
    uint32_t m_capacity { 0 };
    uint8_t m_indexBits { 0 };
    uint8_t m_keysShift { 0 };
    Width m_valueWidth { Width::Wide };
    State m_state { State::Live };
};

static_assert(sizeof(CompactTable) == 24);
static_assert(CompactTable::offsetOfAlive() == JSOrderedHashTable<MapTraits>::offsetOfAliveInCompactTable && CompactTable::offsetOfAlive() == CompactTable::offsetOfUsed() + 4 && CompactTable::offsetOfIndexBits() == CompactTable::offsetOfCapacity() + 4);
static_assert(CompactTable::offsetOfKeysShift() == CompactTable::offsetOfCapacity() + 5 && CompactTable::offsetOfValueWidth() == CompactTable::offsetOfCapacity() + 6 && CompactTable::offsetOfState() == CompactTable::offsetOfCapacity() + 7);

template<typename Traits, typename Iterator>
CompactTable::Position CompactTable::stepIterator(VM& vm, Iterator* iterator)
{
    using Field = typename Iterator::Field;
    JSValue at = iterator->internalField(Field::Entry).get();
    JSCellButterfly* record = at.isCell() ? uncheckedDowncast<JSCellButterfly>(at.asCell()) : nullptr;
    Position position = next<Traits>(vm, iterator->iteratedObject(), record ? record->get(0).asCell() : nullptr, record ? record->get(1).asUInt32() : 0);
    if (!position.table) {
        iterator->close(vm);
        return position;
    }
    if (position.table->classInfo() != info()) {
        iterator->setStorage(vm, position.table);
        iterator->setEntry(vm, position.entry + 1);
        return position;
    }
    uncheckedDowncast<CompactTable>(position.table)->markAsIterated();
    if (!record) {
        record = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 2);
        iterator->internalField(Field::Entry).set(vm, iterator, record);
    }
    record->setIndex(vm, 0, position.table);
    record->setIndex(vm, 1, jsNumber(position.entry + 1));
    return position;
}

// A `for..of` in compiled code over a Map, a Set, one of their iterators, an iterator of an array or a generator of the program, where nothing
// can observe how it is done. No iterator is made, and no result objects. The record is what operationAOTTypedIterOpen() yields, with an int32 for the `next` method.
class CollectionIteration {
public:
    // None: the value is no such thing.
    static JSCellButterfly* tryOpen(JSGlobalObject*, JSValue iterable);
    static bool isOne(JSCellButterfly* record) { return record->get(1).isInt32(); }
    static constexpr unsigned numberOfWords = 5;
    struct InFrame {
        static InFrame* at(EncodedJSValue* words) { return reinterpret_cast<InFrame*>(words); }
        JSValue get(unsigned word) const { return JSValue::decode(words[word]); }
        void setIndex(VM&, unsigned word, JSValue value) { words[word] = JSValue::encode(value); }
        EncodedJSValue words[numberOfWords];
    };
    static bool tryOpen(JSGlobalObject*, JSValue iterable, InFrame*);
    static bool isOne(InFrame* record) { return record->get(1).isInt32(); }
    static bool step(JSGlobalObject*, InFrame*);
    static void close(JSGlobalObject*, InFrame*);
    // The mode, word 1, of a record of two words that compiled code makes in the place of `array.entries()` and steps through by itself: word 0 is the array. step() never sees one; there is nothing
    // to close. See Lowering::findEntriesOfArrays().
    static constexpr int32_t isOfEntriesOfArrayByIndex = 256;
    // False: it is over, or an exception was thrown. Else the record has the value where every record has it.
    static bool step(JSGlobalObject*, JSCellButterfly* record);
    // The loop is left before it is over: what `return` of the iterator does. It may throw.
    static void close(JSGlobalObject*, JSCellButterfly* record);
    // `[...iterable]`: all that the iteration yields goes behind what the array has, where nothing can observe how that is done: a Set, a Map, one of their
    // iterators, a string. False: nothing was done.
    static bool tryAppendTo(JSGlobalObject*, JSArray*, JSValue iterable);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
