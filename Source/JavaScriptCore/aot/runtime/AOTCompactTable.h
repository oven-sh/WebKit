/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompactStorage.h"
#include "AOTSlotRepresentation.h"
#include "HeapCellInlines.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Int128.h>
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
    static constexpr uint32_t minUsedToShrink = 32;
    static constexpr bool isMostlyDeleted(uint32_t alive, uint32_t used) { return used >= minUsedToShrink && alive * 4 < used; }
    static constexpr uint32_t maxCapacityEmptiedByClear = 32;
    static constexpr uint32_t minLargeTableEntries = 512;
    static constexpr uint32_t maxCapacityWithShortSlots = 254;
    static constexpr uint32_t maxCapacityWithLongSlots = 3 << 22;
    static constexpr uint32_t maxCapacity = 3 << 26;
    static constexpr uint8_t keysAreWide = 0x80;
    static constexpr uint8_t keysAreInImage = 0x40;
    static constexpr uint32_t capacityIsInImage = 1u << 31;
    static constexpr uint8_t wasWalked = 0x20;
    static constexpr uint8_t keysShiftMask = 0x1f;
    static constexpr uint8_t indexIsHuge = 0x80;
    static constexpr uint8_t numberOfIndexBits = 0x3f;
    static constexpr uint8_t keysShiftWithTags = 2;
    static_assert(2u << keysShiftWithTags == maxCapacityWithTags);
    static constexpr uint32_t maxCountWithGrowthPlan = 64;
    static constexpr unsigned growthPlansPerValueWidth = maxCountWithGrowthPlan / 2;
    static constexpr unsigned sizeStepShiftOfGrowthPlan = 56;
    static constexpr unsigned hashBits = 24;
    static constexpr unsigned hashMask = (1u << hashBits) - 1;
    static constexpr uint64_t keyMultiplier = 0x9e3779b97f4a7c15;
    static ALWAYS_INLINE unsigned hashOfKeyBits(uint64_t bits)
    {
        UInt128 product = static_cast<UInt128>(bits) * static_cast<UInt128>(keyMultiplier);
        return static_cast<unsigned>(static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64)) & hashMask;
    }
    static constexpr unsigned tagShiftOfTags = 16;

    static constexpr unsigned slotsPerGroup = 8;
    static constexpr unsigned tagShiftOfSlots = hashBits - 6;
    static constexpr uint8_t takenSlot = 0x80;
    static constexpr uint8_t keyLeftHomeSlot = 0x01;
    static constexpr uint8_t tagOfSlot(unsigned hash) { return static_cast<uint8_t>(takenSlot | hash >> tagShiftOfSlots << 1); }
    static constexpr bool hasTag(uint8_t tagInSlot, uint8_t tag) { return static_cast<uint8_t>(tagInSlot ^ tag) <= keyLeftHomeSlot; }

    template<SlotKind kind>
    struct SlotFormat {
        static_assert(kind != SlotKind::Tag);
        static constexpr unsigned entryBytes = kind == SlotKind::Short ? 1 : kind == SlotKind::Long ? 3 : 7;

        static ALWAYS_INLINE uint32_t entryAt(const uint8_t* index, unsigned indexBits, uint32_t slot)
        {
            const uint8_t* entry = index + (static_cast<size_t>(1) << indexBits) + static_cast<size_t>(slot) * entryBytes;
            if constexpr (kind == SlotKind::Short)
                return *entry;
            uint32_t word = WTF::unalignedLoad<uint32_t>(entry);
            return kind == SlotKind::Long ? word & 0xffffff : word;
        }

        static ALWAYS_INLINE void setEntryAt(uint8_t* index, unsigned indexBits, uint32_t slot, uint32_t number)
        {
            uint8_t* entry = index + (static_cast<size_t>(1) << indexBits) + static_cast<size_t>(slot) * entryBytes;
            for (unsigned byte = 0; byte < std::min(entryBytes, 4u); ++byte)
                entry[byte] = static_cast<uint8_t>(number >> (8 * byte));
        }
    };
    static_assert(maxCapacityWithShortSlots < 1u << 8 && maxCapacityWithLongSlots < 1u << 24);

    static constexpr SlotKind slotKindFor(uint32_t capacity)
    {
        if (capacity <= maxCapacityWithTags)
            return SlotKind::Tag;
        if (capacity <= maxCapacityWithShortSlots)
            return SlotKind::Short;
        return capacity <= maxCapacityWithLongSlots ? SlotKind::Long : SlotKind::Huge;
    }

    template<typename Functor>
    static ALWAYS_INLINE decltype(auto) withSlotKind(SlotKind kind, NOESCAPE const Functor& functor)
    {
        switch (kind) {
        case SlotKind::Tag:
            return functor(std::integral_constant<SlotKind, SlotKind::Tag> { });
        case SlotKind::Short:
            return functor(std::integral_constant<SlotKind, SlotKind::Short> { });
        case SlotKind::Long:
            return functor(std::integral_constant<SlotKind, SlotKind::Long> { });
        case SlotKind::Huge:
            return functor(std::integral_constant<SlotKind, SlotKind::Huge> { });
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static Structure* createStructure(VM&);
    static Structure* structureOf(VM&);
    static CompactTable* tryCreate(JSGlobalObject*, Width key, Width value, uint32_t minCapacity);
    static CompactTable* tryCreateForFirstEntry(JSGlobalObject*, bool hasValues, JSValue key, JSValue value);
    static constexpr uint32_t largestStepInLoop = 8;
    static void reserveForAdditions(JSGlobalObject*, JSObject* owner, uint64_t remaining, uint64_t finished, uint64_t sizeAtEntry);
    static void ensureInitialTableAllocators(VM&);
    JS_EXPORT_PRIVATE static const uint64_t* growthPlans();
    static JSSet* tryCreateSetOfStrings(JSGlobalObject*, std::span<const WriteBarrier<Unknown>> values);
    static JSSet* tryCreateSetOfNumbers(JSGlobalObject*, JSArray*);
    static JSMap* tryCreateMapOfStringKeys(JSGlobalObject*, JSArray* pairs);
    static CompactTable* tableOf(JSObject* owner);
    static bool tryClearInPlace(JSObject* owner);
    static CompactTable* createInImage(VM&, State&, const TableInImage&);
    static JSSet* createSetInImage(JSGlobalObject*, State&, const TableInImage&);
    static JSMap* createMapInImage(JSGlobalObject*, State&, const TableInImage&);
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
    const TableInImage& tableInImage() const
    {
        RELEASE_ASSERT(isInImage());
        return **at<const TableInImage*>(offsetOfIndex());
    }
    bool isFull() const { return isLive() && m_used == m_capacity; }

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
        RELEASE_ASSERT(isLive());
        return m_capacity;
    }
    Width keyWidth() const
    {
        RELEASE_ASSERT(isLive());
        return m_keysShiftAndFlags & keysAreWide ? Width::Wide : Width::Narrow;
    }

    JSValue keyAt(uint32_t entry) const;
    ALWAYS_INLINE JSValue uncheckedKeyAt(uint32_t entry) const
    {
        ASSERT(isLive() && !isInImage());
        if (m_keysShiftAndFlags & keysAreWide)
            return JSValue::decode(at<EncodedJSValue>(offsetOfKeys())[entry]);
        return decodeNarrow(at<uint32_t>(offsetOfKeys())[entry]);
    }
    ALWAYS_INLINE JSValue valueAt(uint32_t entry) const
    {
        RELEASE_ASSERT(isLive());
        if (m_keysShiftAndFlags & keysAreInImage) [[unlikely]]
            return valueInImage(entry);
        switch (m_valueWidth) {
        case Width::None:
            return { };
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
    std::optional<uint32_t> findInline(std::span<const Latin1Character>) const;

    enum class KeyPresence : bool { Absent, Present };
    static bool canCompareKeys(JSSet*, JSSet* other);
    static bool hasAllKeys(JSSet*, JSSet* other, KeyPresence);
    static JSSet* trySelectKeys(JSGlobalObject*, JSSet*, JSSet* other, KeyPresence);
    static JSSet* tryUnite(JSGlobalObject*, JSSet*, JSSet* other);
    static JSSet* trySubtractBothWays(JSGlobalObject*, JSSet*, JSSet* other);

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

    bool answersByCharacters() const { return hasNarrowKeys(); }

    ALWAYS_INLINE Lookup tryFindQuickly(JSString* key) const
    {
        const StringImpl* impl = key->tryGetValueImpl();
        if (!impl || !impl->hasHash() || !hasNarrowKeys())
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

    QuickLookup tryFindSubstringInImage(JSRopeString* key, StringView characters, unsigned hash, bool isMembershipTest, JSValue& value) const;
    template<typename CharacterType>
    ALWAYS_INLINE Lookup tryFindCharactersQuickly(std::span<const CharacterType> characters, unsigned hash) const
    {
        if (!hasNarrowKeys())
            return { };
        uintptr_t base = cageBaseOf(this);
        const uint32_t* keys = at<uint32_t>(offsetOfKeys());
        auto found = findWithHash(hash, [&](uint32_t entry) ALWAYS_INLINE_LAMBDA {
            const StringImpl* impl = stringKeyOrNull(base, keys[entry]);
            return impl && impl->existingHash() == hash && equalsCharacters(*impl, characters);
        });
        return found ? Lookup { QuickLookup::Found, *found } : Lookup { QuickLookup::Absent, 0 };
    }

    template<typename CharacterType> int64_t placeOfValueUnder(std::span<const CharacterType>) const;

    static constexpr uint32_t probeStartFor(unsigned indexBits, unsigned hash)
    {
        unsigned spreadShift = indexBits > hashBits ? indexBits - hashBits : 0;
        return hash << spreadShift & ((1u << indexBits) - 1);
    }

    static constexpr uint64_t onesOfTags = 0x0101010101010101ull;
    static ALWAYS_INLINE uint64_t freeSlotsAmong(uint64_t tags) { return ~tags & onesOfTags << 7; }
    static constexpr uint64_t belowDifferenceOfOtherTags = (keyLeftHomeSlot + 1) * onesOfTags;
    static ALWAYS_INLINE uint64_t slotsWithTagAmong(uint64_t tags, uint8_t tag)
    {
        uint64_t difference = tags ^ tag * onesOfTags;
        return (difference - belowDifferenceOfOtherTags) & ~difference & onesOfTags << 7;
    }

    static ALWAYS_INLINE bool canFreeSlot(const uint8_t* tags, uint32_t slot)
    {
        return !(tags[slot] & keyLeftHomeSlot) && freeSlotsAmong(WTF::unalignedLoad<uint64_t>(tags + (slot & ~(slotsPerGroup - 1))));
    }

    static constexpr uint32_t noSlot = std::numeric_limits<uint32_t>::max();
    static uint32_t nextSlotWithTag(const uint8_t* tags, unsigned indexBits, uint8_t tag, uint32_t homeSlot, uint32_t previous);
    static uint32_t findFreeSlotBehindHome(const uint8_t* tags, unsigned indexBits, uint32_t homeSlot);

    template<SlotKind kind, typename Matches>
    static ALWAYS_INLINE std::optional<uint32_t> probeIndex(const uint8_t* index, unsigned indexBits, uint32_t used, unsigned hash, NOESCAPE const Matches& matches)
    {
        if constexpr (kind == SlotKind::Tag) {
            uint64_t difference = WTF::unalignedLoad<uint64_t>(index) ^ (hash >> tagShiftOfTags) * onesOfTags;
            for (uint64_t flags = (difference - onesOfTags) & ~difference & onesOfTags << 7; flags; flags &= flags - 1) {
                uint32_t entry = WTF::ctz(flags) / 8;
                if (entry >= used)
                    break;
                if (matches(entry))
                    return entry;
            }
            return std::nullopt;
        } else {
            const uint8_t* tags = index;
            uint32_t homeSlot = probeStartFor(indexBits, hash);
            uint8_t tag = tagOfSlot(hash);
            uint8_t tagAtHome = tags[homeSlot];
            if (hasTag(tagAtHome, tag)) {
                uint32_t entry = SlotFormat<kind>::entryAt(index, indexBits, homeSlot);
                if (matches(entry))
                    return entry;
            }
            if (!(tagAtHome & keyLeftHomeSlot))
                return std::nullopt;
            for (uint32_t slot = nextSlotWithTag(tags, indexBits, tag, homeSlot, noSlot); slot != noSlot; slot = nextSlotWithTag(tags, indexBits, tag, homeSlot, slot)) {
                uint32_t entry = SlotFormat<kind>::entryAt(index, indexBits, slot);
                if (matches(entry))
                    return entry;
            }
            return std::nullopt;
        }
    }

    static ALWAYS_INLINE uint32_t entryAt(SlotKind kind, const uint8_t* index, unsigned indexBits, uint32_t slot)
    {
        return withSlotKind(kind, [&](auto constantKind) ALWAYS_INLINE_LAMBDA -> uint32_t {
            if constexpr (constantKind() == SlotKind::Tag)
                return slot;
            else
                return SlotFormat<constantKind()>::entryAt(index, indexBits, slot);
        });
    }

    template<typename Matches>
    static ALWAYS_INLINE std::optional<uint32_t> probeIndex(SlotKind kind, const uint8_t* index, unsigned indexBits, uint32_t used, unsigned hash, NOESCAPE const Matches& matches)
    {
        if (kind == SlotKind::Tag)
            return probeIndex<SlotKind::Tag>(index, indexBits, used, hash, matches);
        const uint8_t* tags = index;
        uint32_t homeSlot = probeStartFor(indexBits, hash);
        uint8_t tag = tagOfSlot(hash);
        uint8_t tagAtHome = tags[homeSlot];
        if (hasTag(tagAtHome, tag)) {
            uint32_t entry = entryAt(kind, index, indexBits, homeSlot);
            if (matches(entry))
                return entry;
        }
        if (!(tagAtHome & keyLeftHomeSlot))
            return std::nullopt;
        for (uint32_t slot = nextSlotWithTag(tags, indexBits, tag, homeSlot, noSlot); slot != noSlot; slot = nextSlotWithTag(tags, indexBits, tag, homeSlot, slot)) {
            uint32_t entry = entryAt(kind, index, indexBits, slot);
            if (matches(entry))
                return entry;
        }
        return std::nullopt;
    }

    template<SlotKind kind>
    static ALWAYS_INLINE void insertIntoIndex(uint8_t* index, unsigned indexBits, uint32_t entry, unsigned hash)
    {
        if constexpr (kind == SlotKind::Tag)
            index[entry] = static_cast<uint8_t>(hash >> tagShiftOfTags);
        else {
            uint8_t* tags = index;
            uint32_t slot = probeStartFor(indexBits, hash);
            if (tags[slot]) {
                tags[slot] |= keyLeftHomeSlot;
                slot = findFreeSlotBehindHome(tags, indexBits, slot);
            }
            SlotFormat<kind>::setEntryAt(index, indexBits, slot, entry);
            tags[slot] = tagOfSlot(hash);
        }
    }

    static void insertIntoIndex(SlotKind, uint8_t* index, unsigned indexBits, uint32_t entry, unsigned hash);

    static unsigned indexBitsFor(uint32_t capacity);
    static constexpr uint8_t markedIndexBits(unsigned indexBits) { return static_cast<uint8_t>(indexBits > hashBits ? indexBits | indexIsHuge : indexBits); }
    static constexpr uint8_t keysShiftFor(unsigned indexBits, uint32_t capacity)
    {
        if (!indexBits)
            return keysShiftWithTags;
        return static_cast<uint8_t>(indexBits + (capacity > maxCapacityWithShortSlots) + (capacity > maxCapacityWithLongSlots));
    }

    static bool add(JSGlobalObject*, JSObject* owner, JSValue key, JSValue value);
    static bool remove(JSGlobalObject*, JSObject* owner, JSValue key);
    JS_EXPORT_PRIVATE static uint32_t& deletedPrefixOf(JSObject* owner);
    JS_EXPORT_PRIVATE uint32_t numberOfTakenSlots() const;
    static void clear(JSGlobalObject*, JSObject* owner);
    static CompactStorage::Position next(VM&, JSCell* storage, uint32_t entry);
    static CompactStorage::Position nextBehindDeletedEntry(VM&, JSCell* storage, uint32_t entry);
    enum class KeysCopied : uint8_t { None, Values, Int32s };
    static KeysCopied tryCopyKeys(JSCell* storage, std::span<EncodedJSValue> copies);
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

    JS_EXPORT_PRIVATE static uint32_t firstEntryBehindDeletedOnes(const void* keys, bool keysAreWide, uint32_t entry, uint32_t used);
    enum class DeletedEntries : bool { AreReadOneByOne, ArePassedByInSteps };
    static constexpr uint32_t entriesBetweenScans = 8;
    static constexpr uint32_t deletedEntriesReadByStub = 64;
    template<DeletedEntries deletedEntries = DeletedEntries::ArePassedByInSteps, typename Functor>
    static void forEachEntryFrom(VM& vm, JSCell* storage, uint32_t entry, NOESCAPE const Functor& functor)
    {
        auto* table = uncheckedDowncast<CompactTable>(storage);
        if (!table->isLive() || table->isInImage()) [[unlikely]] {
            for (CompactStorage::Position position = next(vm, storage, entry); position.storage; position = next(vm, position.storage, position.entry + 1)) {
                if (!functor(position.entry, position.key, position.value))
                    return;
            }
            return;
        }
        uint32_t used = table->m_used;
        bool hasValues = table->hasValues();
        auto columnAt = [table](size_t offset, bool isWide) {
            return [table, isWide, narrow = table->at<uint32_t>(offset), wide = table->at<EncodedJSValue>(offset)](uint32_t entry) ALWAYS_INLINE_LAMBDA {
                return isWide ? JSValue::decode(wide[entry]) : table->decodeNarrow(narrow[entry]);
            };
        };
        auto keyAt = columnAt(table->offsetOfKeys(), table->m_keysShiftAndFlags & keysAreWide);
        auto valueAt = columnAt(table->offsetOfValues(), table->m_valueWidth == Width::Wide);
        for (; entry < used; ++entry) {
            if (JSValue key = keyAt(entry)) {
                if (!functor(entry, key, hasValues ? valueAt(entry) : JSValue()))
                    return;
            } else if constexpr (deletedEntries == DeletedEntries::ArePassedByInSteps) {
                if (!(entry % entriesBetweenScans))
                    entry = firstEntryBehindDeletedOnes(table->at<uint8_t>(table->offsetOfKeys()), table->m_keysShiftAndFlags & keysAreWide, entry + 1, used) - 1;
            }
        }
    }

    template<typename Functor>
    static void forEachEntry(VM& vm, JSCell* storage, NOESCAPE const Functor& functor)
    {
        forEachEntryFrom<DeletedEntries::AreReadOneByOne>(vm, storage, 0, [&](uint32_t, JSValue key, JSValue value) ALWAYS_INLINE_LAMBDA {
            functor(key, value);
            return true;
        });
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

    static uint32_t capacityFor(uint32_t minCapacity, Width key, Width value);
    static uint32_t capacityAfterGrowth(uint32_t alive, uint32_t used);
    static size_t offsetOfKeys(uint32_t capacity);
    static size_t allocationSize(uint32_t capacity, Width key, Width value);
    static unsigned hashOfStoredKey(JSValue);
    static Width widthFor(VM&, JSValue);
    static bool canHold(VM&, Width, JSValue);
    static void setStorage(VM&, JSObject* owner, JSCell* storage);

    template<typename T> T* at(size_t offset) const { return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(const_cast<CompactTable*>(this)) + offset); }
    unsigned indexBits() const { return m_indexBits & numberOfIndexBits; }
    size_t offsetOfKeys() const { return offsetOfIndex() + (static_cast<size_t>(2) << (m_keysShiftAndFlags & keysShiftMask)); }
    size_t offsetOfValues() const { return offsetOfKeys() + static_cast<size_t>(m_capacity) * static_cast<size_t>(m_keysShiftAndFlags & keysAreWide ? Width::Wide : Width::Narrow); }
    SlotKind slotKind() const { return slotKindFor(m_capacity); }
    uint32_t deleted() const { return m_used - m_alive; }
    bool hasNarrowKeys() const { return isLive() && !(m_keysShiftAndFlags & (keysAreWide | keysAreInImage)); }
    State& stateOfTableInImage() const
    {
        ASSERT(isInImage());
        return **at<State*>(offsetOfIndex() + sizeof(void*));
    }
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
        ASSERT(isLive() && !isInImage());
        return probeIndex(slotKind(), at<uint8_t>(offsetOfIndex()), indexBits(), m_used, hash, matches);
    }
    template<typename CharacterType> std::optional<uint32_t> findCharacters(std::span<const CharacterType>) const;
    std::optional<uint32_t> findString(JSGlobalObject*, JSString*) const;
    std::optional<uint32_t> findOtherKey(JSGlobalObject*, JSValue normalizedKey) const;
    bool hasStoredKey(JSValue) const;
    Vector<uint32_t, 64> selectEntries(const CompactTable& other, KeyPresence) const;
    static JSSet* tryCreateSetOfEntries(JSGlobalObject*, const CompactTable&, std::span<const uint32_t>, const CompactTable& second, std::span<const uint32_t> entriesOfSecond);
    std::optional<uint32_t> findInImage(JSGlobalObject*, JSString*) const;
    JS_EXPORT_PRIVATE JSValue valueInImage(uint32_t entry) const;
    void addToIndex(uint32_t entry, unsigned hash);
    void removeFromIndex(uint32_t entry, unsigned hash);
    void fillIndex(uint32_t count);
    template<SlotKind kind> void fillLargeTableIndex(uint8_t* index, unsigned indexBits, uint32_t count);
    uint32_t copyAliveEntriesTo(CompactTable&) const;
    template<SlotKind> uint32_t fillWithHashedStrings(uintptr_t cageBase, std::span<const WriteBarrier<Unknown>>);
    template<SlotKind> uint32_t fillWithPairsOfHashedStrings(uintptr_t cageBase, std::span<const WriteBarrier<Unknown>>);
    template<SlotKind> uint32_t fillWithNumbers(std::span<const EncodedJSValue>);
    void append(VM&, JSValue key, unsigned hash, JSValue value);
    void resolveSubstringKey(JSRopeString*) const;
    bool tryAddQuickly(JSGlobalObject*, JSValue normalizedKey, JSValue);
    bool tryAddNumberQuickly(VM&, JSValue key, JSValue value);
    CompactTable* tryCopyWithWidths(JSGlobalObject*, Width key, Width value, std::optional<uint32_t> capacity = std::nullopt) const;
    std::optional<unsigned> hashIfAbsent(JSValue normalizedKey) const;
    CompactTable* copyWithAbsentKey(VM&, JSValue key, unsigned hash, JSValue) const;
    void becomeObsolete(VM&, Status, JSCell* successor);
    void giveBackPagesBehind(size_t offset);
    static void shrink(JSGlobalObject*, JSObject* owner);
    void skipObsoleteSuccessors();
    void removeAllEntries();
    bool tryCompactInPlace(VM&, JSObject* owner);

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
static_assert(static_cast<int8_t>(CompactTable::markedIndexBits(CompactTable::hashBits + 1)) < 0 && CompactTable::markedIndexBits(CompactTable::hashBits) == CompactTable::hashBits && !(CompactTable::indexIsHuge & CompactTable::numberOfIndexBits));
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
        visitor.appendUnbarriered(uncheckedKeyAt(entry));
        visitor.appendUnbarriered(valueAt(entry));
    }
}

template<typename Visitor>
ALWAYS_INLINE void CompactTable::visitFields(CompactTable* table, Visitor& visitor)
{
    Locker locker { table->cellLock() };
    if (!table->isLive()) {
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
