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
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "RememberedSlot.h"
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSCellButterfly;
class JSPropertyNameEnumerator;
class PropertyTable;
class StructureChain;

namespace AOT {

struct State;

struct PlacesOfPositionedKeys {
    AOT_CLASS_THRESHOLD(uint32_t, maxNumberOfCounts, 4);

    const Object* object;
    const PropertyTable* table;
    uint32_t structureID;
    uint32_t orderWord;
    uint32_t numberOfRemovedEntries;
    uint32_t numberOfCounts;
    std::array<uint32_t, maxNumberOfCounts> counts;
    std::array<uint32_t, maxNumberOfCounts> lastEntries;
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
    // The entries of an order word that lists every own key of the objects that have it, which are fields all of them, or null. Compiled code finds the answer without a call from then on.
    const uint16_t* publishEntriesOfFields(const LayoutData&, uint32_t orderWord);
    std::optional<uint32_t> orderStartingWith(const LayoutData&, std::span<const uint16_t> entries);
    enum class Step : bool { IsPublished, IsNotPublished };
    std::optional<uint32_t> orderAfterCreating(const LayoutData&, uint32_t orderWord, uint16_t entry, Step = Step::IsPublished);
    bool positionsExtraKeys(const LayoutData&, uint32_t orderWord, uint32_t numberOfExtraKeys);

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
    PlacesOfPositionedKeys& placesOfPositionedKeys(const Object& object) LIFETIME_BOUND { return m_placesOfPositionedKeys[std::bit_cast<uintptr_t>(&object) >> 4 & (m_placesOfPositionedKeys.size() - 1)]; }

    void rememberWhetherOrderLists(uint32_t shape, uint32_t orderAndCount, uint32_t key, bool lists)
    {
        size_t newer = indexOfListedField(shape, orderAndCount, key) & ~static_cast<size_t>(1);
        ListedField& place = m_listedFields[newer];
        if (place.shape != shape || place.orderAndCount != orderAndCount || place.key != key)
            m_listedFields[newer + 1] = place;
        place = { shape, orderAndCount, key, lists ? isListed : isNotListed };
    }

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

    void growTransitions();

    State& m_state;
    const ProgramTables& m_tables;
    std::unique_ptr<Storage> m_storage;
    RememberedSlotSet m_writes;
    uint32_t m_numberOfTransitions { 0 };
    std::array<ListedField, 128> m_listedFields { };
    struct FieldFoundLast {
        uint32_t shape;
        uint32_t next;
    };
    std::array<FieldFoundLast, 64> m_fieldsFoundLast { };
    std::array<PlacesOfPositionedKeys, 8> m_placesOfPositionedKeys { };
    size_t indexOfFieldAt(const LayoutData&, uint32_t slotKey);
    UncheckedKeyHashMap<uint32_t, Vector<uint64_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fieldsBySlotKey;
    std::optional<ChainDependentOwnerID> m_ownerOfPublishedEnumerators;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
