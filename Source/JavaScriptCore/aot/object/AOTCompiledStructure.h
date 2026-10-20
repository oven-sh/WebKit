/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/AOTSlotRepresentation.h>
#include <JavaScriptCore/Structure.h>
#include <JavaScriptCore/WriteBarrier.h>
#include <optional>
#include <span>
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
    };

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.compiledStructureSpace();
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
        std::optional<std::span<const uint16_t>> outOfLineKeyOrder;
    };

    JS_EXPORT_PRIVATE static CompiledStructure* createForObject(VM&, const TypeInfo&, const ClassInfo*, const ObjectParameters&);
    JS_EXPORT_PRIVATE static CompiledStructure* createForEnvironment(VM&, JSGlobalObject*, const TypeInfo&, const ClassInfo*, const EnvironmentShapes&, uint16_t shapePage);
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

    Structure* extraPropertiesStructure() const { return m_extraPropertiesStructure.get(); }
    StructureID extraPropertiesStructureID() const { return m_extraPropertiesStructure.value(); }
    // A dictionary gets a property without becoming another Structure: it says where a property is, and not that one is absent.
    bool listsEveryExtraProperty() const
    {
        Structure* named = extraPropertiesStructure();
        return named && !named->isDictionary();
    }
    uint16_t extraPropertiesCellSize() const { return m_extraPropertiesCellSize; }
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
        return m_outOfLineKeyOrder.span();
    }

    // The list that KeyOrderTable has for the order word an object of this Structure was last listed with. Lists live as long as the table.
    struct ListedFields {
        static constexpr uint32_t noOrderWord = UINT32_MAX;

        uint32_t orderAndCount { noOrderWord };
        uint32_t size { 0 };
        const FieldAsProperty* fields { nullptr };
    };
    const ListedFields& listedFields() const { return m_listedFields; }

    static constexpr uint16_t unknownNumberOfExtraKeys = UINT16_MAX;

    // How many properties of extraPropertiesStructure() have a position among the keys and are no fields of the layout. Both are fixed, so the number is counted once.
    std::optional<uint16_t> numberOfExtraKeys() const
    {
        if (m_numberOfExtraKeys == unknownNumberOfExtraKeys)
            return std::nullopt;
        return m_numberOfExtraKeys;
    }
    void setNumberOfExtraKeys(uint32_t number) const
    {
        if (number < unknownNumberOfExtraKeys)
            m_numberOfExtraKeys = static_cast<uint16_t>(number);
    }
    void setListedFields(const ListedFields& listed) const { m_listedFields = listed; }

    static constexpr ptrdiff_t offsetOfLayout() { return OBJECT_OFFSETOF(CompiledStructure, m_layout); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesStructureID() { return OBJECT_OFFSETOF(CompiledStructure, m_extraPropertiesStructure); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesSlot() { return OBJECT_OFFSETOF(CompiledStructure, m_extraPropertiesSlot); }
    static constexpr ptrdiff_t offsetOfExtensionSlot() { return OBJECT_OFFSETOF(CompiledStructure, m_extensionSlot); }
    static constexpr ptrdiff_t offsetOfExtraPropertiesCellSize() { return OBJECT_OFFSETOF(CompiledStructure, m_extraPropertiesCellSize); }
    static constexpr ptrdiff_t offsetOfNumberOfExtraKeys() { return OBJECT_OFFSETOF(CompiledStructure, m_numberOfExtraKeys); }
    static constexpr ptrdiff_t offsetOfEnvironmentShapePage() { return OBJECT_OFFSETOF(CompiledStructure, m_environmentShapePage); }
    static constexpr ptrdiff_t offsetOfFlags() { return OBJECT_OFFSETOF(CompiledStructure, m_flags); }

    template<typename Visitor>
    static void visitAdditionalChildren(JSCell*, Visitor&);

private:
    CompiledStructure(VM&, const TypeInfo&, const ClassInfo*, const ObjectParameters&);
    CompiledStructure(VM&, JSGlobalObject*, const TypeInfo&, const ClassInfo*, const EnvironmentShapes&, uint16_t shapePage);

    union {
        const LayoutData* m_layout;
        const EnvironmentShapes* m_environmentShapes;
    };
    WriteBarrierStructureID m_extraPropertiesStructure;
    SlotData m_extraPropertiesSlot { };
    SlotData m_extensionSlot { };
    uint16_t m_extraPropertiesCellSize { 0 };
    uint16_t m_environmentShapePage { 0 };
    mutable uint16_t m_numberOfExtraKeys { unknownNumberOfExtraKeys };
    OptionSet<Flag> m_flags;
    FixedVector<uint16_t> m_outOfLineKeyOrder;
    mutable ListedFields m_listedFields;
};

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
