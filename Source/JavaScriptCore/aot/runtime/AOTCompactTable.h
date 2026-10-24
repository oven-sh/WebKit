/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactStorage.h"
#include "AOTSlotRepresentation.h"
#include "AOTThreshold.h"
#include "HeapCellInlines.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/IterationStatus.h>
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSArray;
class JSMap;
class JSSet;

namespace AOT {

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

    enum class Status : uint8_t {
        Live,
        Replaced,
        Cleared,
    };

    enum class SlotKind : uint8_t {
        Tag,
        Short,
        Long,
        Huge,
    };

    static constexpr uint32_t initialCapacity = 4;
    static constexpr uint32_t maxCapacityWithTags = 8;
    AOT_CLASS_THRESHOLD(uint32_t, maxCapacityEmptiedByClear, 32);
    static constexpr uint32_t maxCapacityWithShortSlots = 254;
    static constexpr uint32_t maxCapacityWithLongSlots = (1 << 20) - 2;
    static constexpr uint32_t maxCapacity = (1 << 28) - 2;
    static constexpr uint8_t keysAreWide = 0x80;
    static constexpr uint8_t keysAreInImage = 0x40;
    // Somebody may stand behind an entry of the table: an iterator or a loop that has taken one. Until then whoever holds the table has yet to take its first entry.
    static constexpr uint8_t wasWalked = 0x20;
    static constexpr uint8_t keysShiftMask = 0x1f;
    static constexpr uint8_t indexIsInImage = 0x80;
    static constexpr uint32_t capacityIsInImage = 1u << 31;
    static constexpr uint8_t keysShiftWithTags = 2;
    static_assert(2u << keysShiftWithTags == maxCapacityWithTags);
    // A full table with narrow keys, no deleted entry and fewer entries than this grows by a plan.
    static constexpr uint32_t maxCountWithGrowthPlan = 64;
    static constexpr unsigned growthPlansPerValueWidth = maxCountWithGrowthPlan / 2;
    static constexpr unsigned sizeStepShiftOfGrowthPlan = 56;
    static constexpr unsigned hashBits = 24;
    static constexpr unsigned hashMask = (1u << hashBits) - 1;
    static constexpr unsigned tagShiftOfTags = 16;

    template<SlotKind kind>
    struct SlotFormat {
        static_assert(kind != SlotKind::Tag);
        using Slot = std::conditional_t<kind == SlotKind::Short, uint16_t, uint32_t>;
        static constexpr unsigned entryBits = kind == SlotKind::Short ? 8 : kind == SlotKind::Long ? 20 : 28;
        static constexpr unsigned tagShift = hashBits - (8 * sizeof(Slot) - entryBits);
        static constexpr Slot freeSlot = std::numeric_limits<Slot>::max();
        static uint32_t tagOf(unsigned hash) { return hash >> tagShift; }
    };
    static_assert(SlotFormat<SlotKind::Short>::tagShift == 16 && SlotFormat<SlotKind::Long>::tagShift == 12 && SlotFormat<SlotKind::Huge>::tagShift == 20);
    static_assert(maxCapacityWithShortSlots + 2 == 1u << SlotFormat<SlotKind::Short>::entryBits && maxCapacityWithLongSlots + 2 == 1u << SlotFormat<SlotKind::Long>::entryBits && maxCapacity + 2 == 1u << SlotFormat<SlotKind::Huge>::entryBits);
    static_assert(SlotFormat<SlotKind::Long>::entryBits < hashBits && SlotFormat<SlotKind::Huge>::entryBits > hashBits);

    static constexpr SlotKind slotKindFor(uint32_t capacity)
    {
        if (capacity <= maxCapacityWithTags)
            return SlotKind::Tag;
        if (capacity <= maxCapacityWithShortSlots)
            return SlotKind::Short;
        return capacity <= maxCapacityWithLongSlots ? SlotKind::Long : SlotKind::Huge;
    }

    static Structure* createStructure(VM&);
    static Structure* structureOf(VM&);
    static CompactTable* tryCreate(JSGlobalObject*, Width key, Width value, uint32_t minCapacity);
    static CompactTable* tryCreateForFirstEntry(JSGlobalObject*, bool hasValues, JSValue key, JSValue value);
    // In front of an addition in a loop that the collection entered with `sizeAtEntry` entries, that has come by `finished` additions and has `remaining` ones left, this one among them.
    AOT_CLASS_THRESHOLD(uint32_t, largestStepInLoop, 8);
    static void reserveForAdditions(JSGlobalObject*, JSObject* owner, uint64_t remaining, uint64_t finished, uint64_t sizeAtEntry);
    static void ensureInitialTableAllocators(VM&);
    // [static_cast<unsigned>(value width) / 4 * growthPlansPerValueWidth + count / 2]: the word at offsetOfCapacity() of the larger table, and above sizeStepShiftOfGrowthPlan the size step of its allocator.
    static const uint64_t* growthPlans();
    static CompactTable* createInImage(VM&, State&, const TableInImage&);
    static JSSet* createSetInImage(JSGlobalObject*, const TableInImage&);
    static JSMap* createMapInImage(JSGlobalObject*, const TableInImage&);
    static JSSet* tryCreateSetOfStrings(JSGlobalObject*, std::span<const WriteBarrier<Unknown>> values);
    // Null, and nothing done, unless the array has int32 or double storage without a hole.
    static JSSet* tryCreateSetOfNumbers(JSGlobalObject*, JSArray*);
    // The list is a contiguous array. Null, and nothing done, unless each element is an ordinary array of a resolved string and a value.
    static JSMap* tryCreateMapOfStringKeys(JSGlobalObject*, JSArray* pairs);
    static CompactTable* tableOf(JSObject* owner);
    static bool tryClearInPlace(JSObject* owner);
    static CompactTable* tryMoveToHeap(JSGlobalObject*, JSObject* owner);

    Status status() const { return m_status; }
    JSCell* successorOfObsoleteTable() const
    {
        RELEASE_ASSERT(!isLive());
        return *at<JSCell*>(offsetOfUsed());
    }
    bool isLive() const { return m_status == Status::Live; }
    bool isInImage() const
    {
        RELEASE_ASSERT(isLive());
        return m_keysShiftAndFlags & keysAreInImage;
    }
    bool isHeapTable() const { return isLive() && !(m_keysShiftAndFlags & keysAreInImage); }
    bool isFullHeapTable() const { return isHeapTable() && m_used == m_capacity; }
    const TableInImage& tableInImage() const
    {
        RELEASE_ASSERT(isInImage());
        return **at<const TableInImage*>(offsetOfIndex());
    }

    uint32_t size() const
    {
        RELEASE_ASSERT(isLive());
        return m_alive;
    }
    uint32_t used() const
    {
        RELEASE_ASSERT(isLive());
        return m_used;
    }
    bool hasValues() const { return m_valueWidth != Width::None; }
    Width valueWidth() const
    {
        RELEASE_ASSERT(isLive());
        return m_valueWidth;
    }
    uint32_t capacity() const
    {
        RELEASE_ASSERT(isHeapTable());
        return m_capacity;
    }
    Width keyWidth() const
    {
        RELEASE_ASSERT(isHeapTable());
        return m_keysShiftAndFlags & keysAreWide ? Width::Wide : Width::Narrow;
    }

    JSValue keyAt(uint32_t entry) const;
    ALWAYS_INLINE JSValue heapKeyAt(uint32_t entry) const
    {
        ASSERT(isHeapTable());
        if (m_keysShiftAndFlags & keysAreWide)
            return JSValue::decode(at<EncodedJSValue>(offsetOfKeys())[entry]);
        return decodeNarrow(at<uint32_t>(offsetOfKeys())[entry]);
    }
    ALWAYS_INLINE JSValue valueAt(uint32_t entry) const
    {
        RELEASE_ASSERT(isLive());
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
        return decodeNarrow(at<uint32_t>(offsetOfValues())[entry]);
    }

    CompactTable* tryCopy(JSGlobalObject*) const;

    std::optional<uint32_t> find(JSGlobalObject*, JSValue key) const;
    std::optional<uint32_t> find(std::span<const Latin1Character>) const;

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

    enum class QuickLookup : uint8_t { Unknown, Found, Absent };
    struct Lookup {
        QuickLookup result { QuickLookup::Unknown };
        uint32_t entry { 0 };
    };

    static ALWAYS_INLINE const StringImpl* stringKeyOrNull(uintptr_t cageBase, uint32_t narrowKey)
    {
        if (narrowKey < minCell32)
            return nullptr;
        JSCell* cell = MarkedBlockCage::decode(cageBase, narrowKey);
        return cell->isString() ? uncheckedDowncast<JSString>(cell)->tryGetValueImpl() : nullptr;
    }

    ALWAYS_INLINE Lookup tryFindQuickly(JSString* key) const
    {
        const StringImpl* impl = key->tryGetValueImpl();
        if (!impl || !impl->hasHash() || !hasNarrowHeapKeys())
            return { };
        unsigned hash = impl->existingHash();
        uintptr_t base = cageBaseOf(this);
        uint32_t narrowKey = MarkedBlockCage::encode(base, key);
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        bool isUnknown = false;
        auto found = findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            uint32_t candidate = keys[entry];
            if (candidate == narrowKey)
                return true;
            const StringImpl* other = stringKeyOrNull(base, candidate);
            if (!other)
                return false;
            if (other == impl)
                return true;
            if (other->existingHash() != hash)
                return false;
            isUnknown = true;
            return true;
        });
        if (isUnknown)
            return { };
        return found ? Lookup { QuickLookup::Found, *found } : Lookup { QuickLookup::Absent, 0 };
    }

    template<typename CharacterType>
    ALWAYS_INLINE Lookup tryFindCharactersQuickly(std::span<const CharacterType> characters, unsigned hash) const
    {
        if (!hasNarrowHeapKeys())
            return { };
        uintptr_t base = cageBaseOf(this);
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        auto found = findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            const StringImpl* impl = stringKeyOrNull(base, keys[entry]);
            return impl && impl->existingHash() == hash && equalsCharacters(*impl, characters);
        });
        return found ? Lookup { QuickLookup::Found, *found } : Lookup { QuickLookup::Absent, 0 };
    }

    static constexpr uint32_t probeStartFor(unsigned indexBits, unsigned hash)
    {
        unsigned spreadShift = indexBits > hashBits ? indexBits - hashBits : 0;
        return hash << spreadShift & ((1u << indexBits) - 1);
    }

    template<typename Slot, typename AtSlot>
    static ALWAYS_INLINE void forEachSlotInProbeOrder(Slot* slots, unsigned indexBits, unsigned hash, NOESCAPE const AtSlot& atSlot)
    {
        uint32_t mask = (1u << indexBits) - 1;
        for (uint32_t i = probeStartFor(indexBits, hash); atSlot(slots[i]) == IterationStatus::Continue; i = (i + 1) & mask) { }
    }

    template<SlotKind kind, typename Matches>
    static ALWAYS_INLINE std::optional<uint32_t> probeIndex(const uint8_t* index, unsigned indexBits, uint32_t used, unsigned hash, NOESCAPE const Matches& matches)
    {
        if constexpr (kind == SlotKind::Tag) {
            constexpr uint64_t ones = 0x0101010101010101ull;
            uint64_t difference = WTF::unalignedLoad<uint64_t>(index) ^ (hash >> tagShiftOfTags) * ones;
            for (uint64_t flags = (difference - ones) & ~difference & ones << 7; flags; flags &= flags - 1) {
                uint32_t entry = WTF::ctz(flags) / 8;
                if (entry >= used)
                    break;
                if (matches(entry))
                    return entry;
            }
            return std::nullopt;
        } else {
            using Format = SlotFormat<kind>;
            uint32_t tag = Format::tagOf(hash) << Format::entryBits;
            std::optional<uint32_t> found;
            forEachSlotInProbeOrder(reinterpret_cast<const typename Format::Slot*>(index), indexBits, hash, [&](const typename Format::Slot& slot) ALWAYS_INLINE_LAMBDA {
                if (slot == Format::freeSlot)
                    return IterationStatus::Done;
                uint32_t entry = slot ^ tag;
                if (entry >= 1u << Format::entryBits || !matches(entry))
                    return IterationStatus::Continue;
                found = entry;
                return IterationStatus::Done;
            });
            return found;
        }
    }

    template<typename Matches>
    static ALWAYS_INLINE std::optional<uint32_t> probeIndex(SlotKind kind, const uint8_t* index, unsigned indexBits, uint32_t used, unsigned hash, NOESCAPE const Matches& matches)
    {
        switch (kind) {
        case SlotKind::Tag:
            return probeIndex<SlotKind::Tag>(index, indexBits, used, hash, matches);
        case SlotKind::Short:
            return probeIndex<SlotKind::Short>(index, indexBits, used, hash, matches);
        case SlotKind::Long:
            return probeIndex<SlotKind::Long>(index, indexBits, used, hash, matches);
        case SlotKind::Huge:
            return probeIndex<SlotKind::Huge>(index, indexBits, used, hash, matches);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    template<SlotKind kind>
    static ALWAYS_INLINE void insertIntoIndex(uint8_t* index, unsigned indexBits, uint32_t entry, unsigned hash)
    {
        if constexpr (kind == SlotKind::Tag)
            index[entry] = static_cast<uint8_t>(hash >> tagShiftOfTags);
        else {
            using Format = SlotFormat<kind>;
            forEachSlotInProbeOrder(reinterpret_cast<typename Format::Slot*>(index), indexBits, hash, [&](typename Format::Slot& slot) ALWAYS_INLINE_LAMBDA {
                if (slot != Format::freeSlot)
                    return IterationStatus::Continue;
                slot = static_cast<typename Format::Slot>(Format::tagOf(hash) << Format::entryBits | entry);
                return IterationStatus::Done;
            });
        }
    }

    static void insertIntoIndex(SlotKind, uint8_t* index, unsigned indexBits, uint32_t entry, unsigned hash);

    static unsigned indexBitsFor(uint32_t capacity);
    static constexpr uint8_t keysShiftFor(unsigned indexBits, uint32_t capacity)
    {
        if (!indexBits)
            return keysShiftWithTags;
        return static_cast<uint8_t>(indexBits + (capacity > maxCapacityWithShortSlots));
    }

    static bool add(JSGlobalObject*, JSObject* owner, JSValue key, JSValue value);
    static bool remove(JSGlobalObject*, JSObject* owner, JSValue key);
    static void clear(JSGlobalObject*, JSObject* owner);
    static CompactStorage::Position next(VM&, JSCell* storage, uint32_t entry);
    // The keys, or the values, that a walk by next() from the entry on finds. Of the other column nothing is made.
    template<typename Functor>
    static void forEachInColumn(VM& vm, JSCell* storage, uint32_t entry, bool wantsKeys, NOESCAPE const Functor& functor)
    {
        auto* table = uncheckedDowncast<CompactTable>(storage);
        if (!table->isLive()) [[unlikely]] {
            for (CompactStorage::Position position = next(vm, storage, entry); position.storage; position = next(vm, position.storage, position.entry + 1))
                functor(wantsKeys ? position.key : position.value);
            return;
        }
        uint32_t used = table->m_used;
        if (entry < used)
            table->m_keysShiftAndFlags |= wasWalked;
        // No entry of a table in the image is deleted: the table moves to the heap first.
        if (table->isInImage()) {
            for (; entry < used; ++entry)
                functor(wantsKeys ? table->keyAt(entry) : table->valueAt(entry));
            return;
        }
        auto columnAt = [table](size_t offset, bool isWide) {
            return [table, isWide, narrow = table->at<uint32_t>(offset), wide = table->at<EncodedJSValue>(offset)](uint32_t entry) ALWAYS_INLINE_LAMBDA {
                return isWide ? JSValue::decode(wide[entry]) : table->decodeNarrow(narrow[entry]);
            };
        };
        auto keyAt = columnAt(table->offsetOfKeys(), table->m_keysShiftAndFlags & keysAreWide);
        auto valueAt = columnAt(table->offsetOfValues(), table->m_valueWidth == Width::Wide);
        for (; entry < used; ++entry) {
            if (JSValue key = keyAt(entry))
                functor(wantsKeys ? key : valueAt(entry));
        }
    }

    template<typename Visitor> ALWAYS_INLINE static void visitFields(CompactTable*, Visitor&);

    static constexpr ptrdiff_t offsetOfUsed() { return OBJECT_OFFSETOF(CompactTable, m_used); }
    static constexpr ptrdiff_t offsetOfAliveCount() { return OBJECT_OFFSETOF(CompactTable, m_alive); }
    static constexpr ptrdiff_t offsetOfCapacity() { return OBJECT_OFFSETOF(CompactTable, m_capacity); }
    static constexpr ptrdiff_t offsetOfIndexBits() { return OBJECT_OFFSETOF(CompactTable, m_indexBits); }
    static constexpr ptrdiff_t offsetOfKeysShift() { return OBJECT_OFFSETOF(CompactTable, m_keysShiftAndFlags); }
    static constexpr ptrdiff_t offsetOfValueWidth() { return OBJECT_OFFSETOF(CompactTable, m_valueWidth); }
    static constexpr ptrdiff_t offsetOfStatus() { return OBJECT_OFFSETOF(CompactTable, m_status); }
    static constexpr ptrdiff_t offsetOfIndex() { return sizeof(CompactTable); }
    static constexpr size_t initialSize(Width key, Width value) { return sizeof(CompactTable) + maxCapacityWithTags + initialCapacity * (static_cast<size_t>(key) + static_cast<size_t>(value)); }
    static constexpr uint64_t initialCapacityAndKinds(Width key, Width value) { return initialCapacity | static_cast<uint64_t>(keysShiftWithTags | (key == Width::Wide ? keysAreWide : 0)) << 40 | static_cast<uint64_t>(value) << 48; }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

private:
    CompactTable(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    State& stateOfTableInImage() const
    {
        ASSERT(isInImage());
        return **at<State*>(offsetOfIndex() + sizeof(void*));
    }

    static uint32_t capacityFor(uint32_t minCapacity, Width key, Width value);
    static uint32_t capacityAfterGrowth(uint32_t alive, uint32_t used);
    static size_t offsetOfKeys(uint32_t capacity);
    static size_t allocationSize(uint32_t capacity, Width key, Width value);
    static unsigned hashOfStoredKey(JSValue);
    static Width widthFor(VM&, JSValue);
    static bool canHold(VM&, Width, JSValue);
    static void setStorage(VM&, JSObject* owner, JSCell* storage);

    template<typename T> T* at(size_t offset) const { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(const_cast<CompactTable*>(this)) + offset); }
    size_t offsetOfKeys() const { return offsetOfIndex() + (static_cast<size_t>(2) << (m_keysShiftAndFlags & keysShiftMask)); }
    size_t offsetOfValues() const { return offsetOfKeys() + static_cast<size_t>(m_capacity) * static_cast<size_t>(m_keysShiftAndFlags & keysAreWide ? Width::Wide : Width::Narrow); }
    SlotKind slotKind() const { return slotKindFor(m_capacity); }
    uint32_t deleted() const { return m_used - m_alive; }
    bool hasNarrowHeapKeys() const { return isHeapTable() && !(m_keysShiftAndFlags & keysAreWide); }
    ALWAYS_INLINE JSValue decodeNarrow(uint32_t word) const
    {
        return word < minCell32 ? JSValue::decode(word) : JSValue(MarkedBlockCage::decode(cageBaseOf(this), word));
    }
    uint32_t encodeNarrow(JSValue) const;
    template<typename Visitor> void visitEntries(Visitor&, uint32_t first, uint32_t last) const;

    void setKey(uint32_t entry, JSValue);
    void setValue(uint32_t entry, JSValue);
    template<typename Matches>
    ALWAYS_INLINE std::optional<uint32_t> findWithHash(unsigned hash, NOESCAPE const Matches& matches) const
    {
        ASSERT(isHeapTable());
        return probeIndex(slotKind(), at<uint8_t>(offsetOfIndex()), m_indexBits, m_used, hash, matches);
    }
    template<typename CharacterType> std::optional<uint32_t> findCharacters(std::span<const CharacterType>) const;
    std::optional<uint32_t> findString(JSGlobalObject*, JSString*) const;
    std::optional<uint32_t> findOtherKey(JSGlobalObject*, JSValue normalizedKey) const;
    std::optional<uint32_t> findInImage(JSGlobalObject*, JSString*) const;
    JS_EXPORT_PRIVATE JSValue valueInImage(uint32_t entry) const;
    void addToIndex(uint32_t entry, unsigned hash);
    template<SlotKind> void fillIndexOfKind(uint32_t count);
    void fillIndex(uint32_t count);
    uint32_t copyAliveEntriesTo(CompactTable&) const;
    template<SlotKind> uint32_t fillWithHashedStrings(uintptr_t cageBase, std::span<const WriteBarrier<Unknown>>);
    template<SlotKind> uint32_t fillWithPairsOfHashedStrings(uintptr_t cageBase, std::span<const WriteBarrier<Unknown>>);
    template<SlotKind> uint32_t fillWithNumbers(std::span<const EncodedJSValue>);
    void append(VM&, JSValue key, unsigned hash, JSValue value);
    enum class QuickAddition : uint8_t { Unknown, Done };
    QuickAddition tryAddQuickly(JSGlobalObject*, JSValue normalizedKey, JSValue);
    CompactTable* tryCopyWithWidths(JSGlobalObject*, Width key, Width value, std::optional<uint32_t> capacity = std::nullopt) const;
    CompactTable* copyWithAbsentKey(VM&, JSString* key, JSValue) const;
    void becomeObsolete(VM&, Status, JSCell* successor);
    void skipObsoleteSuccessors();
    void removeAllEntries();

    JSCell*& successor()
    {
        RELEASE_ASSERT(!isLive());
        return *at<JSCell*>(offsetOfUsed());
    }
    uint32_t& numberOfDeletedEntries()
    {
        RELEASE_ASSERT(!isLive());
        return m_capacity;
    }
    uint32_t* deletedEntries()
    {
        RELEASE_ASSERT(!isLive());
        return at<uint32_t>(offsetOfIndex());
    }

    uint32_t m_used { 0 };
    uint32_t m_alive { 0 };
    uint32_t m_capacity { 0 };
    uint8_t m_indexBits { 0 };
    uint8_t m_keysShiftAndFlags { 0 };
    Width m_valueWidth { Width::Wide };
    Status m_status { Status::Live };
};

static_assert(sizeof(CompactTable) == 24);
static_assert(StringImpl::s_flagCount == 8 && CompactTable::tagShiftOfTags + 8 == CompactTable::hashBits);
static_assert(JSValue::ValueTrue < minCell32 && JSValue::ValueFalse < minCell32 && undefinedCell32 < minCell32);

template<typename Visitor>
ALWAYS_INLINE void CompactTable::visitEntries(Visitor& visitor, uint32_t first, uint32_t last) const
{
    if (keyWidth() == Width::Narrow && m_valueWidth != Width::Wide) [[likely]] {
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        if (m_valueWidth == Width::None) {
            for (uint32_t entry = first; entry < last; ++entry)
                visitNarrowReference(visitor, this, keys[entry]);
            return;
        }
        const uint32_t* values = at<uint32_t>(offsetOfValues());
        for (uint32_t entry = first; entry < last; ++entry) {
            visitNarrowReference(visitor, this, keys[entry]);
            visitNarrowReference(visitor, this, values[entry]);
        }
        return;
    }
    for (uint32_t entry = first; entry < last; ++entry) {
        visitor.appendUnbarriered(heapKeyAt(entry));
        visitor.appendUnbarriered(valueAt(entry));
    }
}

template<typename Visitor>
ALWAYS_INLINE void CompactTable::visitFields(CompactTable* table, Visitor& visitor)
{
    Locker locker { table->cellLock() };
    if (!table->isLive()) {
        // A word left on the stack may name a table that was replaced long ago. It keeps the table that the owner has now, and none between the two.
        table->skipObsoleteSuccessors();
        if (JSCell* next = table->successor())
            visitor.appendUnbarriered(next);
        return;
    }
    if (table->isInImage())
        return;
    uint32_t used = table->m_used;
    WTF::loadLoadFence();
    table->visitEntries(visitor, 0, used);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
