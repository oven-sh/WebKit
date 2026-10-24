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
#include <wtf/FixedVector.h>
#include <wtf/OptionSet.h>

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
        // The bits of every slot are all there is to its field, so a copy of the slots is a copy of the fields.
        HasOnlyPlainSlots = 1 << 4,
        // The object has its layout's fields and nothing else, and above it is only the Object.prototype of its realm: a name that is neither a field nor a name of that object finds nothing.
        HasOnlyFieldsBelowObjectPrototype = 1 << 5,
        IsCopiedByWords = 1 << 6,
    };

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.compiledStructureSpace<mode>();
    }

    struct ExtraPropertiesShape {
        Structure* structureOrNull { nullptr };
        uint16_t cellSize { 0 };
    };

    struct ObjectParameters {
        JSGlobalObject* realm { nullptr };
        JSValue prototype;
        const LayoutData* layout { nullptr };
        bool hasOtherPrototype { false };
        bool mayBePrototype { false };
        std::optional<ExtraPropertiesShape> extraProperties;
        JSObject* heldExtraProperties { nullptr };
        std::optional<std::span<const uint16_t>> outOfLineKeyOrder;
    };

    JS_EXPORT_PRIVATE static CompiledStructure* createForObject(VM&, const TypeInfo&, const ClassInfo*, const ObjectParameters&);
    // Copies what is the same in every Structure of the compiled objects of a realm from one that no cell ever had.
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

    StructureID extraPropertiesStructureID() const { return std::bit_cast<StructureID>(hasExtraProperties() ? m_namedStructureAndRareData.extraPropertiesStructureID : 0); }
    Structure* extraPropertiesStructure() const
    {
        StructureID id = extraPropertiesStructureID();
        return id ? id.decode() : nullptr;
    }
    // A dictionary gets a property without becoming another Structure: it says where a property is, and not that one is absent.
    bool listsEveryExtraProperty() const
    {
        Structure* named = extraPropertiesStructure();
        return named && !named->isDictionary();
    }
    uint16_t extraPropertiesCellSize() const { return hasExtraProperties() ? m_extraKeysAndCellSize.extraPropertiesCellSize : 0; }
    // Where the layout has no word for the extra properties, an object that gets some gets a Structure of its own, which holds them.
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

    // The list that KeyOrderTable has for the order word an object of this Structure was last listed with. Lists live as long as the table.
    struct ListedFields {
        static constexpr uint32_t noOrderWord = UINT32_MAX;

        uint32_t orderAndCount { noOrderWord };
        uint32_t size { 0 };
        const FieldAsProperty* fields { nullptr };
    };
    // The order word of an object with extra properties is never equal to orderAndCount, so nobody looks at the rest, which is meaningless for its Structure.
    ListedFields listedFields() const { return { m_listedOrderAndCount, m_listedSize, m_listedFields }; }

    static constexpr uint16_t unknownNumberOfExtraKeys = UINT16_MAX;

    // How many properties of extraPropertiesStructure() have a position among the keys and are no fields of the layout. Both are fixed, so the number is counted once.
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
    // The three of them hold what they are named after only in a Structure that has extra properties.
    static constexpr ptrdiff_t offsetOfExtraPropertiesStructureID() { return OBJECT_OFFSETOF(CompiledStructure, m_namedStructureAndRareData.extraPropertiesStructureID); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesCellSize() { return OBJECT_OFFSETOF(CompiledStructure, m_extraKeysAndCellSize.extraPropertiesCellSize); }
    static constexpr ptrdiff_t offsetOfNumberOfExtraKeys() { return OBJECT_OFFSETOF(CompiledStructure, m_extraKeysAndCellSize.numberOfExtraKeys); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesSlot() { return OBJECT_OFFSETOF(CompiledStructure, m_extraPropertiesSlot); }
    static constexpr ptrdiff_t offsetOfExtensionSlot() { return OBJECT_OFFSETOF(CompiledStructure, m_extensionSlot); }
    static constexpr ptrdiff_t offsetOfEnvironmentShapePage() { return OBJECT_OFFSETOF(CompiledStructure, m_environmentShapePage); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(CompiledStructure, m_flags); }

    template<typename Visitor>
    static void visitAdditionalChildren(JSCell*, Visitor&);

private:
    struct RareData {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RareData);

        FixedVector<uint16_t> outOfLineKeyOrder;
        WriteBarrier<JSObject> heldExtraProperties;
    };

    struct NamedStructureAndRareData {
        uint32_t extraPropertiesStructureID;
        uint32_t lowBitsOfRareData;
    };

    struct ExtraKeysAndCellSize {
        uint16_t numberOfExtraKeys;
        uint16_t extraPropertiesCellSize;
    };

    // A cell size from here on would set, in the word that it shares with the listed order word, the bit by which the order word of an object says that it has extra properties.
    static constexpr uint16_t endOfExtraPropertiesCellSizes = 1 << 12;

    CompiledStructure(VM&, const TypeInfo&, const ClassInfo*, const ObjectParameters&);
    CompiledStructure(VM&, const CompiledStructure& templateStructure, const ObjectParameters&);
    CompiledStructure(VM&, JSGlobalObject*, const TypeInfo&, const ClassInfo*, const EnvironmentShapes&, uint16_t shapePage);

    void initializeForObject(const ObjectParameters&);

    // Only an object without extra properties whose key order has a number is listed by its order word. Only the Structures of the others name a Structure or have rare data.
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
        const FieldAsProperty* m_listedFields;
        NamedStructureAndRareData m_namedStructureAndRareData;
    };
    union {
        uint16_t m_listedSize;
        uint16_t m_highBitsOfRareData;
        uint16_t m_environmentShapePage;
    };
    OptionSet<Flag> m_flags;
    union {
        uint32_t m_listedOrderAndCount;
        ExtraKeysAndCellSize m_extraKeysAndCellSize;
    };
    SlotData m_extraPropertiesSlot { };
    SlotData m_extensionSlot { };
};
#if CPU(ADDRESS64) && !HAVE(36BIT_ADDRESS)
static_assert(sizeof(CompiledStructure) == 144);
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
