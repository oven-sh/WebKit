/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTObject.h"
#include "AOTObjectWords.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "RememberedSlot.h"
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSCellButterfly;
class JSPropertyNameEnumerator;
class PropertyTable;
class StructureChain;

namespace AOT {

struct State;

// Where, in the table of the extra properties of one object, the extra keys end that each count of its key order counts. A program that deletes the extra keys of an object one after the other asks for the place of each.
struct PlacesOfPositionedKeys {
    static constexpr uint32_t maxNumberOfCounts = 4;

    const Object* object;
    const PropertyTable* table;
    uint32_t structureID;
    uint32_t orderWord;
    uint32_t numberOfRemovedEntries;
    uint32_t numberOfCounts;
    // The numbers of extra keys that stand in front of some field, ascending, and for each the entry of the last of them.
    std::array<uint32_t, maxNumberOfCounts> counts;
    std::array<uint32_t, maxNumberOfCounts> lastEntries;
    // Of the key that Object::placeOfExtraKey() was last asked for, or 0.
    uint32_t entryOfKeyAskedFor;
    uint32_t keyAskedForIsExtraKey;
};

class KeyOrderTable final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(KeyOrderTable);
public:
    explicit KeyOrderTable(State&);
    ~KeyOrderTable();

    // What entriesInOrder() and propertiesInOrder() return stays where it is, unchanged, for as long as the table lives.
    std::span<const uint16_t> entriesInOrder(const LayoutData&, uint32_t order);
    std::optional<uint32_t> orderStartingWith(const LayoutData&, std::span<const uint16_t> entries);
    // The step that compiled code takes without consulting a table: to the next key of an order that the image lists.
    std::optional<uint32_t> stepInsideOrderOfImage(const LayoutData&, uint32_t orderWord, uint16_t entry) const;
    enum class Step : bool { IsPublished, IsNotPublished };
    std::optional<uint32_t> orderAfterCreating(const LayoutData&, uint32_t orderWord, uint16_t entry, Step = Step::IsPublished);
    // For an object without extra keys that has every key of an order added at run time: most objects that have left the orders of the image get all their later fields this way.
    std::optional<uint32_t> tryAppendToAddedOrder(const LayoutData&, uint32_t orderWord, const FieldData&);
    // Whether the entries of the order word count that many extra keys, so that a field listed next comes behind all of them.
    bool positionsExtraKeys(const LayoutData&, uint32_t orderWord, uint32_t numberOfExtraKeys);
    // For an object that leaves the numbered orders: whether to look for a Structure that holds its order. Where nearly every object that found one went on to an order that nobody held, most of them make their own at once.
    bool shouldSearchForHeldOrder(const LayoutData&);
    void didFindHeldOrder(const LayoutData&);
    void didMakeHeldOrderAfterFindingOne(const LayoutData&);

    // Neither lists an order with an extra key or with a field that is anything but a writable, enumerable, configurable data property.
    JSCellButterfly* keysInOrder(VM&, const LayoutData&, uint32_t orderWord, KeyListKind = KeyListKind::Strings);
    std::optional<std::span<const FieldAsProperty>> propertiesInOrder(VM&, const LayoutData&, uint32_t orderWord, Object::SymbolKeys = Object::SymbolKeys::Skip);
    JSPropertyNameEnumerator* enumerator(const LayoutData&, uint32_t orderWord, StructureChain* prototypeChain) const;
    void setEnumerator(VM&, const LayoutData&, uint32_t orderWord, JSPropertyNameEnumerator*, StructureChain* prototypeChain);
    void publishEnumerator(const Object&, JSPropertyNameEnumerator*);
    void dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) const final;

    const FieldData* fieldAt(const LayoutData&, uint32_t slotKey);
    std::optional<uint32_t> publishedTransition(const LayoutData&, uint32_t orderAndCountBefore, const FieldData&, uint32_t numberOfExtraKeys = 0) const;
    void publishTransition(const LayoutData&, uint32_t orderAndCountBefore, const FieldData&, uint32_t orderAndCountAfter, uint32_t numberOfExtraKeys = 0);

    template<typename Visitor> void visitRoots(Visitor&);

    // Whether the numbered order lists the field of that name. The entries that an order word counts never change, so neither does the answer.
    ALWAYS_INLINE std::optional<bool> rememberedWhetherOrderLists(uint32_t shape, uint32_t orderAndCount, uint32_t key) const
    {
        size_t newer = indexOfListedField(shape, orderAndCount, key) & ~static_cast<size_t>(1);
        for (size_t place : { newer, newer + 1 }) {
            const ListedField& remembered = m_listedFields[place];
            if (remembered.shape == shape && remembered.orderAndCount == orderAndCount && remembered.key == key && remembered.answer)
                return remembered.answer == isListed;
        }
        return std::nullopt;
    }
    void rememberWhetherOrderLists(uint32_t shape, uint32_t orderAndCount, uint32_t key, bool lists)
    {
        size_t newer = indexOfListedField(shape, orderAndCount, key) & ~static_cast<size_t>(1);
        ListedField& place = m_listedFields[newer];
        if (place.shape != shape || place.orderAndCount != orderAndCount || place.key != key)
            m_listedFields[newer + 1] = place;
        place = { shape, orderAndCount, key, lists ? isListed : isNotListed };
    }
    PlacesOfPositionedKeys& placesOfPositionedKeys(const Object& object) LIFETIME_BOUND { return m_placesOfPositionedKeys[std::bit_cast<uintptr_t>(&object) >> 4 & (m_placesOfPositionedKeys.size() - 1)]; }

private:
    struct Storage;
    struct ListedField {
        uint32_t shape;
        uint32_t orderAndCount;
        uint32_t key;
        uint32_t answer;
    };
    static constexpr uint32_t isNotListed = 1;
    static constexpr uint32_t isListed = 2;
    static size_t indexOfListedField(uint32_t shape, uint32_t orderAndCount, uint32_t key) { return ((shape ^ orderAndCount << 9 ^ key << 17) * 0x9e3779b1u) >> 25; }
    // The field that fieldAt() found last in a layout. Fields are mostly created in the order of the layout, so the next one asked for stands right behind it.
    struct FieldFoundLast {
        uint32_t shape;
        uint32_t next;
    };
    // Nothing else holds a list that is cached by Structure, and the number of a Structure that has ended is given out again.
    struct KeyListsByStructure {
        void clear();

        ObjectWords& words;
    };

    void growTransitions();
    size_t indexOfFieldAt(const LayoutData&, uint32_t slotKey);

    State& m_state;
    const ProgramTables& m_tables;
    std::unique_ptr<Storage> m_storage;
    KeyListsByStructure m_keyListsByStructure;
    RememberedSlotSet m_writes;
    uint32_t m_numberOfTransitions { 0 };
    std::array<ListedField, 128> m_listedFields { };
    std::array<FieldFoundLast, 64> m_fieldsFoundLast { };
    std::array<PlacesOfPositionedKeys, 8> m_placesOfPositionedKeys { };
    // By shape, for a layout with many fields: the slot key of each field above its index, sorted. Made when a field of the layout is first looked for far from the last one.
    UncheckedKeyHashMap<uint32_t, Vector<uint64_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fieldsBySlotKey;
    std::optional<ChainDependentOwnerID> m_ownerOfPublishedEnumerators;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
