/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStructureTable.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosureTable.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImmortalCells.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "WeakGCMapInlines.h"
#include <array>
#include <tuple>
#include <wtf/BitSet.h>
#include <wtf/Hasher.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct StructureTable::Variant {
    enum class ExtraProperties : uint8_t { None, Unnamed, Named };
    using Key = std::tuple<uint32_t, JSObject*, Structure*, uint32_t>;

    static constexpr uint32_t keyMayBePrototype = 1u << 31;
    static constexpr uint32_t keyHasUnnamedExtraProperties = 1u << 30;
    static constexpr uint32_t keyHasOtherPrototype = 1u << 29;

    Key key() const
    {
        RELEASE_ASSERT(shape < keyHasOtherPrototype);
        uint32_t shapeAndFlags = shape;
        if (mayBePrototype)
            shapeAndFlags |= keyMayBePrototype;
        if (extraProperties == ExtraProperties::Unnamed)
            shapeAndFlags |= keyHasUnnamedExtraProperties;
        if (hasOtherPrototype)
            shapeAndFlags |= keyHasOtherPrototype;
        return { shapeAndFlags, prototype.getObject(), extraPropertiesStructure, outOfLineKeyOrder ? 1 | computeHash(*outOfLineKeyOrder) : 0 };
    }

    bool describes(const CompiledStructure& structure) const
    {
        if (structure.layout().shape != shape || structure.storedPrototype() != prototype)
            return false;
        if (structure.hasOtherPrototype() != hasOtherPrototype || structure.mayBePrototype() != mayBePrototype)
            return false;
        if (structure.hasExtraProperties() != (extraProperties != ExtraProperties::None) || structure.extraPropertiesStructure() != extraPropertiesStructure)
            return false;
        if (structure.hasOutOfLineKeyOrder() != outOfLineKeyOrder.has_value())
            return false;
        return !outOfLineKeyOrder || equalSpans(structure.outOfLineKeyOrder(), *outOfLineKeyOrder);
    }

    void setExtraProperties(ExtraProperties kind, Structure* named = nullptr)
    {
        ASSERT((kind == ExtraProperties::Named) == !!named);
        extraProperties = kind;
        extraPropertiesStructure = named;
    }

    std::optional<CompiledStructure::ExtraPropertiesShape> extraPropertiesShape() const
    {
        switch (extraProperties) {
        case ExtraProperties::None:
            return std::nullopt;
        case ExtraProperties::Unnamed:
            return CompiledStructure::ExtraPropertiesShape { };
        case ExtraProperties::Named:
            return CompiledStructure::ExtraPropertiesShape { extraPropertiesStructure, static_cast<uint16_t>(WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(JSFinalObject::allocationSize(extraPropertiesStructure->inlineCapacity()))) };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    CompiledStructure* createStructure(VM& vm, JSGlobalObject* realm, const LayoutData& layout) const
    {
        RELEASE_ASSERT(extraProperties == ExtraProperties::None || layout.extraPropertiesSlot());
        if (JSObject* object = prototype.getObject())
            object->didBecomePrototype(vm);
        return Object::createStructure(vm, {
            .realm = realm,
            .prototype = prototype,
            .layout = &layout,
            .hasOtherPrototype = hasOtherPrototype,
            .mayBePrototype = mayBePrototype,
            .extraProperties = extraPropertiesShape(),
            .outOfLineKeyOrder = outOfLineKeyOrder,
        });
    }

    uint32_t shape { 0 };
    JSValue prototype;
    bool hasOtherPrototype { false };
    bool mayBePrototype { false };
    ExtraProperties extraProperties { ExtraProperties::None };
    Structure* extraPropertiesStructure { nullptr };
    std::optional<std::span<const uint16_t>> outOfLineKeyOrder;
};

struct StructureTable::VariantMaps {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(VariantMaps);

    static constexpr unsigned recentIndexBits = 7;
    static constexpr unsigned requestedIndexBits = 12;

    explicit VariantMaps(VM& vm)
        : structures(vm)
    {
    }

    static CompiledStructure* decode(StructureID id) { return uncheckedDowncast<CompiledStructure>(id.decode()); }
    static unsigned recentIndex(const Variant& variant) { return (variant.extraPropertiesStructure->id().bits() * 0x9e3779b1u ^ variant.shape * 0x85ebca6bu) >> (32 - recentIndexBits); }
    static unsigned requestedIndex(const Structure& extraPropertiesStructure) { return extraPropertiesStructure.id().bits() * 0x9e3779b1u >> (32 - requestedIndexBits); }

    CompiledStructure* findRecent(const Variant& variant) const
    {
        StructureID id = recentByExtraProperties[recentIndex(variant)];
        if (!id)
            return nullptr;
        CompiledStructure* recent = decode(id);
        return variant.describes(*recent) ? recent : nullptr;
    }

    void setRecent(const Variant& variant, const CompiledStructure& structure)
    {
        recentByExtraProperties[recentIndex(variant)] = structure.id();
    }

    WeakGCMap<Variant::Key, CompiledStructure> structures;
    std::array<StructureID, 1u << recentIndexBits> recentByExtraProperties { };
    WTF::BitSet<1u << requestedIndexBits> requestedExtraPropertiesStructures;
};

StructureTable::StructureTable(State& state)
    : m_state(state)
    , m_tables(state.tables)
    , m_variants(makeUnique<VariantMaps>(*state.instance.vm))
{
    m_environmentShapes.tables = &m_tables;
    ZeroedTableArena& arena = state.tableArena;
    m_structures = arena.allocate<uint32_t>(m_tables.firstEnvironmentShape);
    size_t numberOfAllocations = m_tables.allocations().size();
    static_assert(OBJECT_OFFSETOF(AllocatorWithoutRoom, freeList) == LocalAllocator::offsetOfFreeList());
    size_t sizeOfRecords = (numberOfAllocations + 1) * sizeof(AllocationSiteRecord);
    uint8_t* block = arena.allocate<uint8_t>(sizeOfRecords + numberOfAllocations * sizeof(int32_t));
    state.instance.objects.distancesToAllocationSites = reinterpret_cast<int32_t*>(block + sizeOfRecords);
    *state.instance.objects.emptyAllocationSite() = emptyAllocationSiteRecord();
    state.caches->addClearedEachCollection("recent structures by extra properties"_s, asWritableBytes(std::span { m_variants->recentByExtraProperties }));
    state.instance.objects.prototypeTransitions = arena.allocate<PrototypeTransition>(numberOfPrototypeTransitions);
    state.caches->addClearedEachCollection("prototype transitions"_s, asWritableBytes(std::span { state.instance.objects.prototypeTransitions, numberOfPrototypeTransitions }));
}

StructureTable::~StructureTable() = default;

Structure* StructureTable::structure(VM& vm, uint32_t shape)
{
    if (shape < m_tables.firstEnvironmentShape)
        return objectStructure(vm, shape);
    switch (m_tables.environmentForShape(shape).kind) {
    case LayoutKind::Closure:
        return m_state.objects->closures.closureStructure(vm, 0);
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
        return m_environmentShapes.structureFor(vm, m_state.instance, shape);
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CompiledStructure* StructureTable::objectStructure(VM& vm, uint32_t shape)
{
    RELEASE_ASSERT(shape < m_tables.firstEnvironmentShape);
    if (uint32_t known = m_structures[shape])
        return VariantMaps::decode(std::bit_cast<StructureID>(known));
    RELEASE_ASSERT(m_tables.hasLayoutForShape(shape));
    const LayoutData& layout = m_tables.layoutForShape(shape);
    JSGlobalObject* realm = m_state.instance.globalObject;
    Variant usual { .shape = shape, .prototype = realm->objectPrototype() };
    switch (layout.kind()) {
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        break;
    case LayoutKind::Instance:
        if (JSObject* classPrototype = m_state.objects->classes.classPrototype(vm, m_state.objects->classes.classOfInstances(layout)))
            usual.prototype = classPrototype;
        else
            usual.hasOtherPrototype = true;
        break;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        RELEASE_ASSERT_NOT_REACHED();
    }
    CompiledStructure* created = usual.createStructure(vm, realm, layout);
    m_structures[shape] = created->id().bits();
    m_state.immortalCells->add(created);
    return created;
}

CompiledStructure* StructureTable::objectStructure(VM& vm, uint32_t shape, JSValue prototype)
{
    return findOrCreate(vm, Variant { .shape = shape, .prototype = prototype });
}

StructureTable::Variant StructureTable::variantOf(const CompiledStructure& structure)
{
    Variant variant {
        .shape = structure.layout().shape,
        .prototype = structure.storedPrototype(),
        .hasOtherPrototype = structure.hasOtherPrototype(),
        .mayBePrototype = structure.mayBePrototype(),
    };
    if (structure.hasExtraProperties()) {
        Structure* named = structure.extraPropertiesStructure();
        variant.setExtraProperties(named ? Variant::ExtraProperties::Named : Variant::ExtraProperties::Unnamed, named);
    }
    if (structure.hasOutOfLineKeyOrder())
        variant.outOfLineKeyOrder = structure.outOfLineKeyOrder();
    return variant;
}

CompiledStructure* StructureTable::find(const Variant& variant) const
{
    if (uint32_t known = m_structures[variant.shape]) {
        CompiledStructure* usual = VariantMaps::decode(std::bit_cast<StructureID>(known));
        if (variant.describes(*usual))
            return usual;
    }
    CompiledStructure* found = m_variants->structures.get(variant.key());
    return found && variant.describes(*found) ? found : nullptr;
}

CompiledStructure* StructureTable::findOrCreate(VM& vm, const Variant& requested)
{
    CompiledStructure* usual = objectStructure(vm, requested.shape);
    Variant variant = requested;
    // Compiled code tests the flag after the read that relies on it, so an object that had it keeps it.
    variant.hasOtherPrototype |= usual->hasOtherPrototype() || usual->storedPrototype() != variant.prototype;
    if (CompiledStructure* existing = find(variant))
        return existing;
    // outOfLineKeyOrder may point into a Structure that nothing else keeps alive.
    DeferGC deferGC(vm);
    CompiledStructure* created = variant.createStructure(vm, m_state.instance.globalObject, usual->layout());
    m_variants->structures.set(variant.key(), created);
    return created;
}

CompiledStructure* StructureTable::withPrototype(VM& vm, const CompiledStructure& current, JSValue prototype)
{
    EncodedJSValue encodedPrototype = JSValue::encode(prototype);
    uint32_t before = current.id().bits();
    PrototypeTransition& recent = std::span { m_state.instance.objects.prototypeTransitions, numberOfPrototypeTransitions }[indexOfPrototypeTransition(before, encodedPrototype)];
    if (recent.before == before && recent.prototype == encodedPrototype)
        return uncheckedDowncast<CompiledStructure>(std::bit_cast<StructureID>(recent.after).decode());

    Variant variant = variantOf(current);
    variant.prototype = prototype;
    CompiledStructure* structure = findOrCreate(vm, variant);
    // Compiled code replays a transition by storing the StructureID: it looks for no cycle and tells nobody who depends on the chain.
    if (structure->typeInfoBlob() == current.typeInfoBlob() && !structure->clearsPristineBitsOf(current) && !current.mayBePrototype())
        recent = { encodedPrototype, before, structure->id().bits() };
    return structure;
}

CompiledStructure* StructureTable::asPrototype(VM& vm, const CompiledStructure& current)
{
    Variant variant = variantOf(current);
    variant.mayBePrototype = true;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withExtraProperties(VM& vm, const CompiledStructure& current, Structure* extraPropertiesStructureOrNull)
{
    Variant variant = variantOf(current);
    Structure* named = extraPropertiesStructureOrNull;
    if (!named || !named->propertyAccessesAreCacheable()) {
        if (variant.extraProperties == Variant::ExtraProperties::Unnamed)
            return const_cast<CompiledStructure*>(&current);
        variant.setExtraProperties(Variant::ExtraProperties::Unnamed);
        return findOrCreate(vm, variant);
    }
    variant.setExtraProperties(Variant::ExtraProperties::Named, named);
    if (CompiledStructure* recent = m_variants->findRecent(variant))
        return recent;
    CompiledStructure* structure = findOrCreate(vm, variant);
    m_variants->setRecent(variant, *structure);
    m_variants->requestedExtraPropertiesStructures.set(VariantMaps::requestedIndex(*named));
    return structure;
}

CompiledStructure* StructureTable::existingWithExtraProperties(const CompiledStructure& current, Structure& extraPropertiesStructure)
{
    Variant variant = variantOf(current);
    variant.setExtraProperties(Variant::ExtraProperties::Named, &extraPropertiesStructure);
    if (CompiledStructure* recent = m_variants->findRecent(variant))
        return recent;
    if (!m_variants->requestedExtraPropertiesStructures.get(VariantMaps::requestedIndex(extraPropertiesStructure)))
        return nullptr;
    CompiledStructure* existing = find(variant);
    if (existing)
        m_variants->setRecent(variant, *existing);
    return existing;
}

CompiledStructure* StructureTable::withOutOfLineKeyOrder(VM& vm, const CompiledStructure& current, std::span<const uint16_t> entries)
{
    Variant variant = variantOf(current);
    variant.outOfLineKeyOrder = entries;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withNumberedKeyOrder(VM& vm, const CompiledStructure& current)
{
    Variant variant = variantOf(current);
    variant.outOfLineKeyOrder = std::nullopt;
    return findOrCreate(vm, variant);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
