/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/AOTSlotRepresentation.h>
#include <JavaScriptCore/Structure.h>
#include <JavaScriptCore/WriteBarrier.h>
#include <bit>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC { namespace AOT {

struct EnvironmentShapes;
struct FieldAsProperty;
struct LayoutData;

class CompiledStructure final : public Structure {
    using Base = Structure;
public:
    friend class JSC::Structure;

    enum class Flag : uint8_t {
        HasExtraProperties = 1 << 0,
        HasOtherPrototype = 1 << 1,
        HasOutOfLineKeyOrder = 1 << 2,
        HasFieldsWithSymbolKeys = 1 << 3,
        HasOnlyPlainSlots = 1 << 4,
        HasOnlyFieldsBelowObjectPrototype = 1 << 5,
        IsCopiedByWords = 1 << 6,
        HasOnlyConfigurableFields = 1 << 7,
    };
    enum class HeldKeyOrder : uint8_t {
        IsOfOneObject = 1 << 0,
        IsBesideExtraProperties = 1 << 1,
    };
    using KeyOrderEntries = Vector<uint16_t, 16>;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.compiledStructureSpace<mode>();
    }

    struct ObjectParameters {
        JSGlobalObject* realm { nullptr };
        JSValue prototype;
        const LayoutData* layout { nullptr };
        bool hasOtherPrototype { false };
        bool mayBePrototype { false };
        bool hasExtraProperties { false };
        Structure* extraPropertiesStructure { nullptr };
        JSObject* heldExtraProperties { nullptr };
        std::optional<std::span<const uint16_t>> outOfLineKeyOrder;
        bool isFoundByKeyOrder { true };
        std::optional<std::span<const RefPtr<UniquedStringImpl>>> keysNamedByOrder;
    };

    JS_EXPORT_PRIVATE static CompiledStructure* createForObject(VM&, const TypeInfo&, const ClassInfo*, const ObjectParameters&);
    JS_EXPORT_PRIVATE static CompiledStructure* createForObject(VM&, const CompiledStructure& templateStructure, const ObjectParameters&);
    JS_EXPORT_PRIVATE static CompiledStructure* createForEnvironment(VM&, JSGlobalObject*, const TypeInfo&, const ClassInfo*, const EnvironmentShapes&, uint16_t shapePage);
    ~CompiledStructure();
    static void destroy(JSCell*);

    bool describesObject() const { return typeInfo().type() == CompiledObjectType; }
    bool describesEnvironment() const { return typeInfo().type() == CompiledEnvironmentType; }

    const LayoutData& layout() const
    {
        ASSERT(describesObject());
        return *m_layout;
    }
    const EnvironmentShapes& environmentShapes() const
    {
        ASSERT(describesEnvironment());
        return *m_environmentShapes;
    }
    uint16_t environmentShapePage() const { return m_environmentShapePage; }

    OptionSet<Flag> flags() const { return m_flags; }
    bool hasExtraProperties() const { return m_flags.contains(Flag::HasExtraProperties); }
    bool hasOtherPrototype() const { return m_flags.contains(Flag::HasOtherPrototype); }
    bool hasOutOfLineKeyOrder() const { return m_flags.contains(Flag::HasOutOfLineKeyOrder); }
    bool hasOnlyPlainSlots() const { return m_flags.contains(Flag::HasOnlyPlainSlots); }
    bool isCopiedByWords() const { return m_flags.contains(Flag::IsCopiedByWords); }
    bool hasOnlyConfigurableFields() const { return m_flags.contains(Flag::HasOnlyConfigurableFields); }
    void setHasOnlyConfigurableFields() { m_flags.add(Flag::HasOnlyConfigurableFields); }

    StructureID extraPropertiesStructureID() const { return std::bit_cast<StructureID>(hasExtraProperties() ? m_namedStructureAndRareData.extraPropertiesStructureID : 0); }
    Structure* extraPropertiesStructure() const
    {
        StructureID id = extraPropertiesStructureID();
        return id ? id.decode() : nullptr;
    }
    bool listsEveryExtraProperty() const
    {
        Structure* named = extraPropertiesStructure();
        return named && !named->isDictionary();
    }
    uint16_t extraPropertiesCellSize() const { return hasExtraProperties() ? m_extraKeysAndCellSize.extraPropertiesCellSize : 0; }
    void setSuccessorByPrivateName(VM& vm, CompiledStructure* successor) { m_successorByPrivateName.set(vm, this, successor); }
    const uint64_t* wordsOfObjectWithoutFields() const { return m_wordsOfObjectWithoutFields; }
    void setWordsOfObjectWithoutFields(const uint64_t* words) { m_wordsOfObjectWithoutFields = words; }
    JSObject* heldExtraProperties() const
    {
        const RareData* rareData = this->rareData();
        return rareData ? rareData->heldExtraProperties.get() : nullptr;
    }
    std::optional<SlotData> extraPropertiesSlot() const
    {
        if (!m_extraPropertiesSlot.offset)
            return std::nullopt;
        return m_extraPropertiesSlot;
    }
    std::optional<SlotData> extensionSlot() const
    {
        if (!m_extensionSlot.offset)
            return std::nullopt;
        return m_extensionSlot;
    }
    std::span<const uint16_t> outOfLineKeyOrder() const
    {
        ASSERT(hasOutOfLineKeyOrder());
        return rareData()->outOfLineKeyOrder.span();
    }
    bool tryAppendToOutOfLineKeyOrder(uint16_t entry) const
    {
        ASSERT(hasOutOfLineKeyOrder() && !hasKeyOrderOfOneObject());
        RareData* rareData = const_cast<RareData*>(this->rareData());
        KeyOrderEntries& entries = rareData->outOfLineKeyOrder;
        if (rareData->keyOrderHasEnded || entries.size() == entries.capacity())
            return false;
        entries.append(entry);
        return true;
    }
    void endOutOfLineKeyOrder() const
    {
        if (hasOutOfLineKeyOrder())
            const_cast<RareData*>(rareData())->keyOrderHasEnded = true;
    }
    bool keyOrderWasFoundAgain() const
    {
        ASSERT(hasOutOfLineKeyOrder());
        return rareData()->keyOrderWasFoundAgain;
    }
    void didFindKeyOrderAgain() const
    {
        ASSERT(hasOutOfLineKeyOrder());
        const_cast<RareData*>(rareData())->keyOrderWasFoundAgain = true;
    }
    bool hasKeyOrderOfOneObject() const { return m_heldKeyOrder.contains(HeldKeyOrder::IsOfOneObject); }
    std::span<const RefPtr<UniquedStringImpl>> keysNamedByOrder() const
    {
        ASSERT(hasKeyOrderOfOneObject());
        return rareData()->keysNamedByOrder.span();
    }
    void setKeyOrderOfOneObject(KeyOrderEntries&& entries, Vector<RefPtr<UniquedStringImpl>>&& keysNamedByOrder) const
    {
        ASSERT(hasKeyOrderOfOneObject());
        RareData* rareData = const_cast<RareData*>(this->rareData());
        rareData->outOfLineKeyOrder = WTF::move(entries);
        rareData->keysNamedByOrder = WTF::move(keysNamedByOrder);
    }
    void appendToKeyOrderOfOneObject(UniquedStringImpl* lastKeyInFrontOrNull, uint16_t entryOfKey, uint16_t field) const
    {
        ASSERT(hasKeyOrderOfOneObject());
        RareData* rareData = const_cast<RareData*>(this->rareData());
        if (lastKeyInFrontOrNull) {
            rareData->outOfLineKeyOrder.append(entryOfKey);
            rareData->keysNamedByOrder.append(lastKeyInFrontOrNull);
        }
        rareData->outOfLineKeyOrder.append(field);
    }
    void replaceKeyNamedByOrder(size_t index, UniquedStringImpl* key) const
    {
        ASSERT(hasKeyOrderOfOneObject());
        const_cast<RareData*>(rareData())->keysNamedByOrder[index] = key;
    }

    struct ListedFields {
        static constexpr uint32_t noOrderWord = UINT32_MAX;

        uint32_t orderAndCount { noOrderWord };
        uint32_t size { 0 };
        const FieldAsProperty* fields { nullptr };
    };
    ListedFields listedFields() const { return { m_listedOrderAndCount, m_listedSize, m_listedFields }; }

    static constexpr uint16_t unknownNumberOfExtraKeys = UINT16_MAX;

    std::optional<uint16_t> numberOfExtraKeys() const
    {
        if (!hasExtraProperties() || m_extraKeysAndCellSize.numberOfExtraKeys == unknownNumberOfExtraKeys)
            return std::nullopt;
        return m_extraKeysAndCellSize.numberOfExtraKeys;
    }
    void setNumberOfExtraKeys(uint32_t number) const
    {
        ASSERT(hasExtraProperties());
        if (number < unknownNumberOfExtraKeys)
            const_cast<CompiledStructure*>(this)->m_extraKeysAndCellSize.numberOfExtraKeys = static_cast<uint16_t>(number);
    }
    void setListedFields(const ListedFields& listed) const
    {
        ASSERT(holdsListedFields() && listed.size <= UINT16_MAX);
        auto* structure = const_cast<CompiledStructure*>(this);
        structure->m_listedFields = listed.fields;
        structure->m_listedSize = static_cast<uint16_t>(listed.size);
        structure->m_listedOrderAndCount = listed.orderAndCount;
    }

    static constexpr ptrdiff_t offsetOfLayout() { return OBJECT_OFFSETOF(CompiledStructure, m_layout); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesStructureID() { return OBJECT_OFFSETOF(CompiledStructure, m_namedStructureAndRareData.extraPropertiesStructureID); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesCellSize() { return OBJECT_OFFSETOF(CompiledStructure, m_extraKeysAndCellSize.extraPropertiesCellSize); }
    static constexpr ptrdiff_t offsetOfNumberOfExtraKeys() { return OBJECT_OFFSETOF(CompiledStructure, m_extraKeysAndCellSize.numberOfExtraKeys); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesSlot() { return OBJECT_OFFSETOF(CompiledStructure, m_extraPropertiesSlot); }
    static constexpr ptrdiff_t offsetOfExtensionSlot() { return OBJECT_OFFSETOF(CompiledStructure, m_extensionSlot); }
    static constexpr ptrdiff_t offsetOfEnvironmentShapePage() { return OBJECT_OFFSETOF(CompiledStructure, m_environmentShapePage); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(CompiledStructure, m_flags); }
    static constexpr ptrdiff_t offsetOfHeldKeyOrder() { return OBJECT_OFFSETOF(CompiledStructure, m_heldKeyOrder); }
    static constexpr ptrdiff_t offsetOfSuccessorByPrivateName() { return OBJECT_OFFSETOF(CompiledStructure, m_successorByPrivateName); }

    template<typename Visitor>
    static void visitAdditionalChildren(JSCell*, Visitor&);

private:
    struct RareData {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RareData);

        KeyOrderEntries outOfLineKeyOrder;
        Vector<RefPtr<UniquedStringImpl>> keysNamedByOrder;
        WriteBarrier<JSObject> heldExtraProperties;
        bool keyOrderHasEnded { false };
        bool keyOrderWasFoundAgain { false };
    };

    struct NamedStructureAndRareData {
        uint32_t extraPropertiesStructureID;
        uint32_t lowBitsOfRareData;
    };

    struct ExtraKeysAndCellSize {
        uint16_t numberOfExtraKeys;
        uint16_t extraPropertiesCellSize;
    };

    static constexpr uint16_t endOfExtraPropertiesCellSizes = 1 << 12;

    CompiledStructure(VM&, const TypeInfo&, const ClassInfo*, const ObjectParameters&);
    CompiledStructure(VM&, const CompiledStructure& templateStructure, const ObjectParameters&);
    CompiledStructure(VM&, JSGlobalObject*, const TypeInfo&, const ClassInfo*, const EnvironmentShapes&, uint16_t shapePage);

    void initializeForObject(const ObjectParameters&);

    bool holdsListedFields() const { return !m_flags.containsAny({ Flag::HasExtraProperties, Flag::HasOutOfLineKeyOrder }); }
    const RareData* rareData() const
    {
        if (holdsListedFields())
            return nullptr;
        return std::bit_cast<const RareData*>(static_cast<uintptr_t>(m_highBitsOfRareData) << 32 | m_namedStructureAndRareData.lowBitsOfRareData);
    }

    union {
        const LayoutData* m_layout;
        const EnvironmentShapes* m_environmentShapes;
    };
    union {
        const FieldAsProperty* m_listedFields { nullptr };
        NamedStructureAndRareData m_namedStructureAndRareData;
    };
    union {
        uint16_t m_listedSize { 0 };
        uint16_t m_highBitsOfRareData;
        uint16_t m_environmentShapePage;
    };
    OptionSet<Flag> m_flags;
    OptionSet<HeldKeyOrder> m_heldKeyOrder;
    union {
        uint32_t m_listedOrderAndCount { ListedFields::noOrderWord };
        ExtraKeysAndCellSize m_extraKeysAndCellSize;
    };
    SlotData m_extraPropertiesSlot { };
    SlotData m_extensionSlot { };
    WriteBarrier<CompiledStructure> m_successorByPrivateName;
    const uint64_t* m_wordsOfObjectWithoutFields { nullptr };
};
#if CPU(ADDRESS64) && !HAVE(36BIT_ADDRESS)
static_assert(sizeof(CompiledStructure) == 160);
#endif

ALWAYS_INLINE const CompiledStructure* compiledObjectStructureOrNull(const Structure* structure)
{
    if (structure->typeInfo().type() != CompiledObjectType)
        return nullptr;
    SUPPRESS_MEMORY_UNSAFE_CAST return static_cast<const CompiledStructure*>(structure);
}

ALWAYS_INLINE const LayoutData* compiledLayoutOrNull(const Structure* structure)
{
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    return compiled ? &compiled->layout() : nullptr;
}

} } // namespace JSC::AOT

SPECIALIZE_TYPE_TRAITS_BEGIN(JSC::AOT::CompiledStructure)
    static bool isType(const JSC::Structure& from)
    {
        return from.isCompiledStructure();
    }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(AOT)
