/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedData.h"
#include "IterationKind.h"
#include "JSCellButterfly.h"
#include "JSOrderedHashTable.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include "OperationResult.h"
#include <wtf/UnalignedAccess.h>

namespace JSC {

class JSMap;
class JSSet;

namespace AOT {

struct Instance;
OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES newSetOfStringsGeneric(Instance*, EncodedJSValue list);

namespace Typed {

struct State;
struct TableInImage;

class CompactTable final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    enum class Width : uint8_t {
        None = 0,
        InImage = 1,
        Narrow = 4,
        Wide = 8,
    };

    enum class State : uint8_t {
        Live,
        Iterated,
        Replaced,
        Cleared,
    };

    static constexpr uint32_t initialCapacity = 4;
    static constexpr uint32_t maxKnownGrowthEntries = 64;
    static constexpr unsigned knownGrowthSizeStepShift = 56;
    static std::atomic<uint64_t>* knownGrowths();
    static constexpr uint32_t maxCapacityWithTags = 8;
    static constexpr uint32_t maxCapacityWithShortSlots = 254;
    static constexpr unsigned maxIndexBitsWithShortSlots = 8;
    static constexpr uint8_t keysAreWide = 0x80;
    static constexpr uint8_t keysAreInImage = 0x40;
    static constexpr uint8_t indexIsInImage = 0x80;
    static constexpr uint32_t capacityIsInImage = 1u << 31;
    static constexpr uint8_t keysShiftWithTags = 2;
    static_assert(2u << keysShiftWithTags == maxCapacityWithTags);
    static constexpr uint32_t maxCapacity = (1 << 20) - 2;
    static constexpr unsigned shortSlotEntryBits = 8;
    static constexpr unsigned longSlotEntryBits = 20;
    static constexpr unsigned shortTagShift = 16;
    static constexpr unsigned longTagShift = 12;

    static Structure* createStructure(VM&);
    static CompactTable* create(VM&, Structure*, Width key, Width value, uint32_t minCapacity);
    static CompactTable* createInImage(VM&, Structure*, const TableInImage&, Typed::State*);
    static JSSet* createSetInImage(JSGlobalObject*, const TableInImage&);
    static JSMap* createMapInImage(JSGlobalObject*, const TableInImage&);
    bool isInImage() const { return m_keysShift & keysAreInImage; }
    const TableInImage& tableInImage() const { return **at<const TableInImage*>(offsetOfIndex()); }
    template<typename Traits> static CompactTable* moveToHeap(VM&, JSOrderedHashTable<Traits>*);
    template<typename Traits> static CompactTable* heapTableOf(JSOrderedHashTable<Traits>* owner)
    {
        auto* table = uncheckedDowncast<CompactTable>(owner->m_compactTable.get());
        return table->isInImage() ? moveToHeap(owner->vm(), owner) : table;
    }

    uint32_t size() const { return m_alive; }
    uint32_t used() const { return m_used; }
    uint32_t capacity() const { return m_capacity; }
    Width keyWidth() const { return m_keysShift & keysAreWide ? Width::Wide : Width::Narrow; }
    Width valueWidth() const { return m_valueWidth; }
    State state() const { return m_state; }
    bool isLive() const { return m_state <= State::Iterated; }
    void markAsIterated()
    {
        ASSERT(isLive());
        m_state = State::Iterated;
    }

    std::optional<uint32_t> find(JSGlobalObject*, JSValue key) const;
    std::optional<uint32_t> find(std::span<const Latin1Character>) const;
    JSString* keyAt(uint32_t entry) const;
    ALWAYS_INLINE JSValue valueAt(uint32_t entry) const
    {
        switch (m_valueWidth) {
        case Width::None:
            return { };
        case Width::InImage:
            return valueInImage(entry);
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
        auto* table = heapTableOf(owner);
        for (uint32_t entry = 0, used = table->m_used; entry < used; ++entry) {
            if (JSString* key = table->keyAt(entry); key && functor(key) == IterationStatus::Done)
                return;
        }
    }
    template<typename CharacterType>
    static ALWAYS_INLINE bool hasCharacters(const StringImpl& impl, std::span<const CharacterType> characters)
    {
        if (impl.length() != characters.size())
            return false;
        size_t length = characters.size_bytes();
        const uint8_t* a = reinterpret_cast<const uint8_t*>(impl.span<CharacterType>().data());
        const uint8_t* b = reinterpret_cast<const uint8_t*>(characters.data());
        auto isSameAt = [&]<typename Word>(size_t offset, Word) ALWAYS_INLINE_LAMBDA {
            return WTF::unalignedLoad<Word>(a + offset) == WTF::unalignedLoad<Word>(b + offset);
        };
        if (length >= 8) {
            for (size_t offset = 0; offset + 8 < length; offset += 8) {
                if (!isSameAt(offset, uint64_t()))
                    return false;
            }
            return isSameAt(length - 8, uint64_t());
        }
        if (length >= 4)
            return isSameAt(0, uint32_t()) && isSameAt(length - 4, uint32_t());
        if (length >= 2)
            return isSameAt(0, uint16_t()) && isSameAt(length - 2, uint16_t());
        return !length || *a == *b;
    }
    template<typename CharacterType>
    static ALWAYS_INLINE bool equalsCharacters(const StringImpl& impl, std::span<const CharacterType> characters)
    {
        if (impl.is8Bit() == (sizeof(CharacterType) == 1))
            return hasCharacters(impl, characters);
        using OtherCharacterType = std::conditional_t<sizeof(CharacterType) == 1, char16_t, Latin1Character>;
        size_t length = characters.size();
        if (impl.length() != length)
            return false;
        const OtherCharacterType* a = impl.span<OtherCharacterType>().data();
        const CharacterType* b = characters.data();
        for (size_t i = 0; i < length; ++i) {
            if (a[i] != b[i])
                return false;
        }
        return true;
    }
    enum class Quickly : uint8_t { Found, Absent, Unknown };
    template<typename CharacterType>
    ALWAYS_INLINE Quickly tryFindCharactersQuickly(std::span<const CharacterType> characters, unsigned hash, uint32_t& found) const
    {
        if (m_keysShift & (keysAreWide | keysAreInImage))
            return Quickly::Unknown;
        auto isAt = [&](const uint32_t* keys, uint32_t entry) ALWAYS_INLINE_LAMBDA {
            uint32_t candidate = keys[entry];
            if (!candidate)
                return false;
            const StringImpl& impl = *uncheckedDowncast<JSString>(MarkedBlockCage::decode(candidate))->tryGetValueImpl();
            return impl.existingHash() == hash && equalsCharacters(impl, characters);
        };
        if (hasTags()) {
            constexpr uint64_t ones = 0x0101010101010101ull;
            const uint32_t* keys = at<uint32_t>(offsetOfIndex() + maxCapacityWithTags);
            uint64_t difference = *at<uint64_t>(offsetOfIndex()) ^ (hash >> shortTagShift) * ones;
            uint32_t used = m_used;
            for (uint64_t flags = (difference - ones) & ~difference & ones << 7; flags; flags &= flags - 1) {
                found = WTF::ctz(flags) / 8;
                if (found >= used)
                    break;
                if (isAt(keys, found))
                    return Quickly::Found;
            }
            return Quickly::Absent;
        }
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        uint32_t mask = (1u << m_indexBits) - 1;
        if (hasShortSlots()) {
            const uint16_t* slots = at<uint16_t>(offsetOfIndex());
            uint32_t tag = hash >> shortTagShift << shortSlotEntryBits;
            for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
                uint32_t slot = slots[i];
                if (slot == UINT16_MAX)
                    return Quickly::Absent;
                found = slot ^ tag;
                if (found < 1u << shortSlotEntryBits && isAt(keys, found))
                    return Quickly::Found;
            }
        }
        const uint32_t* slots = at<uint32_t>(offsetOfIndex());
        uint32_t tag = hash >> longTagShift << longSlotEntryBits;
        for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
            uint32_t slot = slots[i];
            if (slot == UINT32_MAX)
                return Quickly::Absent;
            found = slot ^ tag;
            if (found < 1u << longSlotEntryBits && isAt(keys, found))
                return Quickly::Found;
        }
    }
    ALWAYS_INLINE Quickly tryFindQuickly(JSValue value, uint32_t& found) const
    {
        if (!value.isString())
            return Quickly::Absent;
        JSString* key = asString(value);
        const StringImpl* impl = key->tryGetValueImpl();
        if (!impl || !impl->hasHash() || m_keysShift & (keysAreWide | keysAreInImage))
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
        uint8_t tag = hash >> shortTagShift;
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
            if (slot >> shortSlotEntryBits != tag)
                continue;
            found = slot & ((1 << shortSlotEntryBits) - 1);
            if (Quickly next = compare(keys, found); next != Quickly::Absent)
                return next;
        }
    }

    static constexpr uint64_t placeIsUnknown = 0;
    static constexpr uint64_t keyIsAbsent = 1;
    uint64_t placeOfValue(uint32_t entry) const
    {
        switch (m_valueWidth) {
        case Width::Wide:
            return std::bit_cast<uintptr_t>(at<EncodedJSValue>(offsetOfValues()) + entry);
        case Width::None:
            return std::bit_cast<uintptr_t>(this);
        default:
            return placeIsUnknown;
        }
    }

    enum class CreateIfNeeded : bool { No, Yes };
    template<typename Traits> static bool tryAddAbsent(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSString* key, JSValue);
    static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES newSetOfStringsQuickly(Instance*, EncodedJSValue list);
    template<typename Traits> static bool tryAdd(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue key, JSValue, CreateIfNeeded = CreateIfNeeded::No, uint32_t minCapacity = initialCapacity);
    template<typename Traits> static bool remove(JSGlobalObject*, JSOrderedHashTable<Traits>*, JSValue key);
    template<typename Traits> static void clear(VM&, JSOrderedHashTable<Traits>*);
    template<typename Traits> static void convert(VM&, JSOrderedHashTable<Traits>*);

    struct Position {
        JSCell* table { nullptr };
        uint32_t entry { 0 };
        JSValue key;
        JSValue value;
    };
    template<typename Traits> static Position next(VM&, JSOrderedHashTable<Traits>*, JSCell* table, uint32_t entry);
    template<typename Traits, typename Iterator> static Position stepIterator(VM&, Iterator*);
    static JSCell* copyPosition(VM&, JSCell*);

    static constexpr ptrdiff_t offsetOfUsed() { return OBJECT_OFFSETOF(CompactTable, m_used); }
    static constexpr ptrdiff_t offsetOfAlive() { return OBJECT_OFFSETOF(CompactTable, m_alive); }
    static constexpr ptrdiff_t offsetOfCapacity() { return OBJECT_OFFSETOF(CompactTable, m_capacity); }
    static constexpr ptrdiff_t offsetOfIndexBits() { return OBJECT_OFFSETOF(CompactTable, m_indexBits); }
    static constexpr ptrdiff_t offsetOfKeysShift() { return OBJECT_OFFSETOF(CompactTable, m_keysShift); }
    static constexpr ptrdiff_t offsetOfValueWidth() { return OBJECT_OFFSETOF(CompactTable, m_valueWidth); }
    static constexpr ptrdiff_t offsetOfState() { return OBJECT_OFFSETOF(CompactTable, m_state); }
    static constexpr size_t initialSize(Width value) { return sizeof(CompactTable) + maxCapacityWithTags + initialCapacity * (static_cast<size_t>(Width::Narrow) + static_cast<size_t>(value)); }
    static constexpr uint64_t initialCapacityAndKinds(Width value) { return initialCapacity | static_cast<uint64_t>(keysShiftWithTags) << 40 | static_cast<uint64_t>(value) << 48; }
    static constexpr ptrdiff_t offsetOfIndex() { return sizeof(CompactTable); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

private:
    friend struct TableInImage;

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
    template<typename CharacterType> std::optional<uint32_t> findCharacters(std::span<const CharacterType>) const;
    std::optional<uint32_t> findInImage(JSGlobalObject*, JSString*) const;
    JS_EXPORT_PRIVATE JSValue valueInImage(uint32_t entry) const;
    void addToIndex(uint32_t entry, unsigned hash);
    void fillIndex(uint32_t count);
    template<typename Slot> uint32_t fillWithHashedStrings(std::span<const WriteBarrier<Unknown>>);
    void append(VM&, JSString* key, unsigned hash, JSValue);
    CompactTable* copyWithCapacity(VM&, Width value) const;
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

struct TableInImage {
    uint32_t offsetInData;
    uint32_t unused;
    uint32_t count;
    uint32_t alive;
    uint32_t capacity;
    uint8_t indexBits;
    uint8_t keysShift;
    bool hasValues;
    uint8_t alsoUnused;

    const Data& data() const { return *reinterpret_cast<const Data*>(reinterpret_cast<const uint8_t*>(this) - offsetInData); }
    const uint8_t* index() const { return reinterpret_cast<const uint8_t*>(this + 1); }
    const uint32_t* strings() const { return reinterpret_cast<const uint32_t*>(index() + (static_cast<size_t>(2) << keysShift)); }
    const uint64_t* values() const { return reinterpret_cast<const uint64_t*>(strings() + capacity); }
    std::span<const Latin1Character> textAt(uint32_t entry) const { return byteCast<Latin1Character>(data().stringInImage(strings()[entry])); }
    template<typename Matches> ALWAYS_INLINE std::optional<uint32_t> findWithHash(unsigned hash, NOESCAPE const Matches& matches) const
    {
        if (!indexBits) {
            constexpr uint64_t ones = 0x0101010101010101ull;
            uint64_t difference = WTF::unalignedLoad<uint64_t>(index()) ^ (hash >> CompactTable::shortTagShift) * ones;
            for (uint64_t flags = (difference - ones) & ~difference & ones << 7; flags; flags &= flags - 1) {
                uint32_t entry = WTF::ctz(flags) / 8;
                if (entry >= count)
                    break;
                if (matches(entry))
                    return entry;
            }
            return std::nullopt;
        }
        uint32_t mask = (1u << indexBits) - 1;
        if (capacity <= CompactTable::maxCapacityWithShortSlots) {
            const uint16_t* slots = reinterpret_cast<const uint16_t*>(index());
            uint32_t tag = hash >> CompactTable::shortTagShift << CompactTable::shortSlotEntryBits;
            for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
                uint32_t slot = slots[i];
                if (slot == UINT16_MAX)
                    return std::nullopt;
                if (uint32_t entry = slot ^ tag; entry < 1u << CompactTable::shortSlotEntryBits && matches(entry))
                    return entry;
            }
        }
        const uint32_t* slots = reinterpret_cast<const uint32_t*>(index());
        uint32_t tag = hash >> CompactTable::longTagShift << CompactTable::longSlotEntryBits;
        for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
            uint32_t slot = slots[i];
            if (slot == UINT32_MAX)
                return std::nullopt;
            if (uint32_t entry = slot ^ tag; entry < 1u << CompactTable::longSlotEntryBits && matches(entry))
                return entry;
        }
    }
    ALWAYS_INLINE std::optional<uint32_t> find(const StringImpl& key) const
    {
        return findWithHash(key.hash(), [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            return CompactTable::equalsCharacters(key, textAt(entry));
        });
    }
    JS_EXPORT_PRIVATE static Vector<uint8_t> encode(std::span<const std::pair<uint32_t, std::span<const uint8_t>>> keys, std::span<const uint64_t> values);
    static constexpr ptrdiff_t offsetOfOffsetInData() { return OBJECT_OFFSETOF(TableInImage, offsetInData); }
};
static_assert(sizeof(TableInImage) == sizeof(CompactTable) && OBJECT_OFFSETOF(TableInImage, count) == CompactTable::offsetOfUsed() && OBJECT_OFFSETOF(TableInImage, capacity) == CompactTable::offsetOfCapacity() && OBJECT_OFFSETOF(TableInImage, indexBits) == CompactTable::offsetOfIndexBits());
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

} // namespace Typed
struct Instance;
OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stepIterationInFrameGeneric(Instance*, EncodedJSValue* words);
namespace Typed {

class CollectionIteration {
public:
    static JSCellButterfly* tryOpen(JSGlobalObject*, JSValue iterable);
    static bool isOne(JSCellButterfly* record) { return record->get(1).isInt32(); }
    static constexpr unsigned numberOfWords = 5;
    static constexpr unsigned firstPartWord = numberOfWords;
    static constexpr unsigned numberOfWordsWithParts = firstPartWord + 2;
    struct InFrame {
        static InFrame* at(EncodedJSValue* words) { return reinterpret_cast<InFrame*>(words); }
        JSValue get(unsigned word) const { return JSValue::decode(words[word]); }
        void setIndex(VM&, unsigned word, JSValue value) { words[word] = JSValue::encode(value); }
        EncodedJSValue words[numberOfWordsWithParts];
    };
    static bool tryOpen(JSGlobalObject*, JSValue iterable, InFrame*);
    static bool isOne(InFrame* record) { return record->get(1).isInt32(); }
    static bool step(JSGlobalObject*, InFrame*);
    static OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stepQuickly(Instance*, EncodedJSValue* words);
    static constexpr int32_t mayBeSteppedQuickly = 1024;
    static constexpr int32_t hasFastStepAfterStart = 2048;
    static bool mayStepQuickly(const EncodedJSValue* words)
    {
        static_assert(std::endian::native == std::endian::little && mayBeSteppedQuickly >> 8 && mayBeSteppedQuickly >> 8 <= UINT8_MAX);
        return reinterpret_cast<const uint8_t*>(words + 1)[1] & mayBeSteppedQuickly >> 8;
    }
    static void close(JSGlobalObject*, InFrame*);
    static constexpr int32_t iteratesArrayEntriesByIndex = 256;
    static constexpr int32_t iteratesGenerator = 16;
    static constexpr int32_t iterationKindMask = 3;
    static constexpr int32_t iteratesSet = 4;
    static constexpr int32_t iteratesThroughIterator = 8;
    static constexpr int32_t iteratesArray = 32;
    static constexpr int32_t takesPairsApart = 4096;
    static constexpr int32_t modeOfMap = static_cast<int32_t>(IterationKind::Entries);
    static constexpr int32_t modeOfSet = static_cast<int32_t>(IterationKind::Values) | iteratesSet | hasFastStepAfterStart;
    static constexpr unsigned structureOfResultsWord = 3;
    static constexpr unsigned offsetsInResultsWord = 4;
    static bool step(JSGlobalObject*, JSCellButterfly* record);
    static void close(JSGlobalObject*, JSCellButterfly* record);
    static bool tryAppendTo(JSGlobalObject*, JSArray*, JSValue iterable);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
