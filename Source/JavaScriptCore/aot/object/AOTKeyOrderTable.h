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
#include <wtf/Ref.h>

namespace JSC {

class JSCellButterfly;
class JSPropertyNameEnumerator;
class PropertyTable;
class StructureChain;

namespace AOT {

struct State;

struct PlacesOfPositionedKeys {
    static constexpr uint32_t maxNumberOfCounts = 4;

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

struct EnumeratorBelowCompiledObjects {
    static constexpr uint32_t mostPrototypes = 6;
    struct Prototype {
        friend bool operator==(const Prototype&, const Prototype&) = default;

        uint32_t structureID { 0 };
        uint32_t orderWord { 0 };
    };

    uint32_t structureID { 0 };
    uint32_t orderWord { 0 };
    uint32_t numberOfPrototypes { 0 };
    bool everyNameIsEnumerable { false };
    JSPropertyNameEnumerator* enumerator { nullptr };
    std::array<Prototype, mostPrototypes> prototypes { };
};

class KeyOrderTable final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(KeyOrderTable);
public:
    explicit KeyOrderTable(State&);
    ~KeyOrderTable();

    std::span<const uint16_t> entriesInOrder(const LayoutData&, uint32_t order);
    std::span<const uint16_t> entriesListedBy(const LayoutData&, uint32_t orderWord);
    std::optional<uint32_t> orderStartingWith(const LayoutData&, std::span<const uint16_t> entries);
    std::optional<uint32_t> stepInsideOrderOfImage(const LayoutData&, uint32_t orderAndCount, uint16_t entry) const;
    enum class Step : bool { IsPublished, IsNotPublished };
    std::optional<uint32_t> orderAfterCreating(const LayoutData&, uint32_t orderWord, uint16_t entry, Step = Step::IsPublished);
    std::optional<uint32_t> tryAppendToAddedOrder(const LayoutData&, uint32_t orderAndCount, const FieldData&, uint32_t numberOfExtraKeys = 0);
    bool positionsExtraKeys(const LayoutData&, uint32_t orderWord, uint32_t numberOfExtraKeys);
    bool shouldSearchForHeldOrder(const LayoutData&);
    void didFindHeldOrder(const LayoutData&);
    void didMakeHeldOrderAfterFindingOne(const LayoutData&);

    JSCellButterfly* keysInOrder(VM&, const LayoutData&, uint32_t orderWord, KeyListKind = KeyListKind::Strings);
    JSCellButterfly* tryCreateKeyList(VM&, const LayoutData&, std::span<const uint16_t> entries, KeyListKind);
    JSCellButterfly* tryCreateAndRememberKeyList(VM&, const LayoutData&, uint32_t orderWord, KeyListKind listed);
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
    void rememberWhetherOrderLists(uint32_t shape, uint32_t orderAndCount, uint32_t key, bool lists)
    {
        size_t newer = indexOfListedField(shape, orderAndCount, key) & ~static_cast<size_t>(1);
        ListedField& place = m_listedFields[newer];
        if (place.shape != shape || place.orderAndCount != orderAndCount || place.key != key)
            m_listedFields[newer + 1] = place;
        place = { shape, orderAndCount, key, lists ? isListed : isNotListed };
    }
    EnumeratorBelowCompiledObjects& enumeratorBelowCompiledObjects(uint32_t structureID) LIFETIME_BOUND { return m_enumeratorsBelowCompiledObjects[(structureID >> enumeratorHashShift) & (m_enumeratorsBelowCompiledObjects.size() - 1)]; }
    PlacesOfPositionedKeys& placesOfPositionedKeys(const Object& object) LIFETIME_BOUND { return m_placesOfPositionedKeys[std::bit_cast<uintptr_t>(&object) >> 4 & (m_placesOfPositionedKeys.size() - 1)]; }

private:
    struct Storage;
    struct StorageOfOrders;
    struct ListedField {
        uint32_t shape;
        uint32_t orderAndCount;
        uint32_t key;
        uint32_t answer;
    };
    static constexpr uint32_t isNotListed = 1;
    static constexpr uint32_t isListed = 2;
    static size_t indexOfListedField(uint32_t shape, uint32_t orderAndCount, uint32_t key) { return ((shape ^ orderAndCount << 9 ^ key << 17) * 0x9e3779b1u) >> 25; }
    struct FieldFoundLast {
        uint32_t shape;
        uint32_t next;
    };
    struct KeyListsByStructure {
        void clear();

        ObjectWords& words;
    };

    struct FieldsBySlotKey {
        uint32_t numberOfSearches { 0 };
        Vector<uint64_t> keysAndIndexes;
    };

    static Ref<StorageOfOrders> storageOfOrdersFor(State&);
    void growTransitions();
    size_t indexOfFieldAt(const LayoutData&, uint32_t slotKey);

    State& m_state;
    const ProgramTables& m_tables;
    const Ref<StorageOfOrders> m_orders;
    std::unique_ptr<Storage> m_storage;
    KeyListsByStructure m_keyListsByStructure;
    RememberedSlotSet m_writes;
    uint32_t m_numberOfTransitions { 0 };
    std::array<ListedField, 128> m_listedFields { };
    std::array<FieldFoundLast, 64> m_fieldsFoundLast { };
    std::array<PlacesOfPositionedKeys, 8> m_placesOfPositionedKeys { };
    std::array<EnumeratorBelowCompiledObjects, 32> m_enumeratorsBelowCompiledObjects { };
    UncheckedKeyHashMap<uint32_t, FieldsBySlotKey, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fieldsBySlotKey;
    std::optional<ChainDependentOwnerID> m_ownerOfPublishedEnumerators;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
