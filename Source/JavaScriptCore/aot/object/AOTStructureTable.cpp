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
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AbstractSlotVisitorInlines.h"
#include "ConservativeRoots.h"
#include "DeferGCInlines.h"
#include "FastMarkAppender.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "SlotVisitorInlines.h"
#include "WeakGCMapInlines.h"
#include <array>
#include <tuple>
#include <wtf/BitSet.h>
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>
#include <wtf/PrintStream.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// The collector visits no object that lies in a frame, so such an object keeps its Structure alive only if the scan of the stack takes its header for a reference.
static_assert(ConservativeRoots::findsStructuresOfFrameCells);

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
        if (structure.heldExtraProperties() != heldExtraProperties)
            return false;
        if (structure.hasOutOfLineKeyOrder() != outOfLineKeyOrder.has_value())
            return false;
        return !outOfLineKeyOrder || equalSpans(structure.outOfLineKeyOrder(), *outOfLineKeyOrder);
    }

    bool isUsual(const Variant& usual) const
    {
        ASSERT(shape == usual.shape);
        if (mayBePrototype || extraProperties != ExtraProperties::None || outOfLineKeyOrder)
            return false;
        return prototype == usual.prototype && hasOtherPrototype == usual.hasOtherPrototype;
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

    CompiledStructure::ObjectParameters parametersFor(JSGlobalObject* realm, const LayoutData& layout) const
    {
        RELEASE_ASSERT(extraProperties == ExtraProperties::None || !layout.extraPropertiesSlot() == !!heldExtraProperties);
        RELEASE_ASSERT(!heldExtraProperties || extraProperties == ExtraProperties::Unnamed);
        return {
            .realm = realm,
            .prototype = prototype,
            .layout = &layout,
            .hasOtherPrototype = hasOtherPrototype,
            .mayBePrototype = mayBePrototype,
            .extraProperties = extraPropertiesShape(),
            .heldExtraProperties = heldExtraProperties,
            .outOfLineKeyOrder = outOfLineKeyOrder,
        };
    }

    uint32_t shape { 0 };
    JSValue prototype;
    bool hasOtherPrototype { false };
    bool mayBePrototype { false };
    ExtraProperties extraProperties { ExtraProperties::None };
    Structure* extraPropertiesStructure { nullptr };
    JSObject* heldExtraProperties { nullptr };
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

    // A variant ends with what it hangs on, not with the last object that has it: a full collection may find no such object, as in a server between two requests, and the program would learn the shape again.
    // So there are no more of them than the prototypes and the Structures of extra properties that something else keeps alive allow. Nothing bounds the orders in which keys may come: those variants end with their objects.
    template<typename Visitor>
    void visitRoots(Visitor& visitor)
    {
        if (visitor.collectionScope() == CollectionScope::Eden)
            return;
        for (StructureID id : listed) {
            CompiledStructure* variant = decode(id);
            if (visitor.isMarked(variant) || variant->hasOutOfLineKeyOrder())
                continue;
            JSObject* prototype = variant->storedPrototype().getObject();
            Structure* named = variant->extraPropertiesStructure();
            if ((!prototype || visitor.isMarked(prototype)) && (!named || visitor.isMarked(named)))
                visitor.appendUnbarriered(variant);
        }
    }

    WeakGCMap<Variant::Key, CompiledStructure> structures;
    // The values of `structures`. Written under the lock of the caches.
    Vector<StructureID> listed;
    std::array<StructureID, 1u << recentIndexBits> recentByExtraProperties { };
    WTF::BitSet<1u << requestedIndexBits> requestedExtraPropertiesStructures;
};

// Which Structures a run makes: by what sets them apart from the usual one of their layout, for how many layouts, and under how many keys. A key that is made twice had ended in between.
struct StructureTable::MadeStructures {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(MadeStructures);

    static constexpr std::array<ASCIILiteral, 6> differences { "other prototype"_s, "may be a prototype"_s, "named extra properties"_s, "unnamed extra properties"_s, "held by one object"_s, "key order out of line"_s };
    using Counts = UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

    struct Totals {
        uint32_t made { 0 };
        uint32_t layouts { 0 };
        uint32_t keys { 0 };
    };

    static unsigned kindOf(const Variant& variant)
    {
        return variant.hasOtherPrototype
            | variant.mayBePrototype << 1
            | (variant.extraProperties == Variant::ExtraProperties::Named) << 2
            | (variant.extraProperties == Variant::ExtraProperties::Unnamed) << 3
            | !!variant.heldExtraProperties << 4
            | variant.outOfLineKeyOrder.has_value() << 5;
    }

    static void printKind(PrintStream& out, unsigned kind)
    {
        if (!kind) {
            out.print("as the layout says"_s);
            return;
        }
        CommaPrinter comma(" + "_s);
        for (unsigned bit = 0; bit < differences.size(); ++bit) {
            if (kind & 1u << bit)
                out.print(comma, differences[bit]);
        }
    }

    void note(const LayoutData& layout, const Variant& variant)
    {
        unsigned kind = kindOf(variant);
        Totals& total = totals[kind];
        ++total.made;
        if (madeByLayoutAndKind.add(static_cast<uint64_t>(kind) << 32 | layout.index, 0).iterator->value++ == 0)
            ++total.layouts;
        auto [shapeAndFlags, prototype, extraPropertiesStructure, orderHash] = variant.key();
        uint64_t key = computeHash(shapeAndFlags, prototype, extraPropertiesStructure, orderHash, variant.heldExtraProperties);
        if (madeByKey.add(static_cast<uint64_t>(kind) << 32 | key, 0).iterator->value++ == 0)
            ++total.keys;
    }

    std::array<Totals, 1u << differences.size()> totals { };
    Counts madeByLayoutAndKind;
    Counts madeByKey;
};

void StructureTable::dumpStructuresMade(PrintStream& out) const
{
    if (!m_made)
        return;
    for (unsigned kind = 0; kind < m_made->totals.size(); ++kind) {
        const MadeStructures::Totals& total = m_made->totals[kind];
        if (!total.made)
            continue;
        out.print(total.made, "\tstructures:"_s);
        MadeStructures::printKind(out, kind);
        out.println(" | layouts "_s, total.layouts, " | keys "_s, total.keys);
    }

    constexpr size_t numberOfLayoutsListed = 40;
    Vector<std::pair<uint32_t, uint64_t>> mostOften;
    for (auto& [layoutAndKind, made] : m_made->madeByLayoutAndKind) {
        if (made > 1)
            mostOften.append({ made, layoutAndKind });
    }
    std::ranges::sort(mostOften, std::ranges::greater { });
    for (auto [made, layoutAndKind] : mostOften.span().first(std::min(mostOften.size(), numberOfLayoutsListed))) {
        out.print(made, "\tstructures of one layout:L"_s, static_cast<uint32_t>(layoutAndKind), " "_s);
        MadeStructures::printKind(out, layoutAndKind >> 32);
        out.println();
    }
}

static std::span<const uint8_t> allocateAllocationSiteRecords(State& state)
{
    size_t numberOfAllocations = state.tables.allocations().size();
    size_t sizeOfRecords = (numberOfAllocations + 1) * sizeof(AllocationSiteRecord);
    uint8_t* block = state.tableArena.allocate<uint8_t>(sizeOfRecords + numberOfAllocations * sizeof(int32_t));
    state.instance.objects.distancesToAllocationSites = reinterpret_cast<int32_t*>(block + sizeOfRecords);
    return { block, sizeOfRecords };
}

StructureTable::StructureTable(State& state)
    : m_state(state)
    , m_tables(state.tables)
    , m_structures(state.tableArena.allocate<uint32_t>(state.tables.firstEnvironmentShape))
    , m_variants(makeUnique<VariantMaps>(*state.instance.vm))
    , m_writtenStructures(asBytes(usualStructureIDs()), structuresRegionShift)
    , m_writtenRecords(allocateAllocationSiteRecords(state), recordsRegionShift)
{
    m_environmentShapes.tables = &m_tables;
    if (Options::aotCountsPath())
        m_made = makeUnique<MadeStructures>();
    static_assert(OBJECT_OFFSETOF(AllocatorWithoutRoom, freeList) == LocalAllocator::offsetOfFreeList());
    *state.instance.objects.emptyAllocationSite() = emptyAllocationSiteRecord();
    state.caches->add(CacheKind::StrongRoots, "structures of layouts"_s, *this, RootVolatility::GreyedByExecution);
    state.caches->add(CacheKind::WeakByKey, "structures of layouts that have ended"_s, *this);
    state.caches->add(CacheKind::StrongRoots, "variants of the structures of layouts"_s, *m_variants);
    state.caches->addClearedEachCollection("recent structures by extra properties"_s, asWritableBytes(std::span { m_variants->recentByExtraProperties }));
    state.instance.objects.prototypeTransitions = state.tableArena.allocate<PrototypeTransition>(numberOfPrototypeTransitions);
    state.caches->addClearedEachCollection("prototype transitions"_s, asWritableBytes(std::span { state.instance.objects.prototypeTransitions, numberOfPrototypeTransitions }));
}

StructureTable::~StructureTable() = default;

std::span<AllocationSiteRecord> StructureTable::allocationSiteRecords() const
{
    size_t numberOfAllocations = m_tables.allocations().size();
    return { m_state.instance.objects.emptyAllocationSite() - numberOfAllocations, numberOfAllocations + 1 };
}

// The objects of many layouts live shorter than the time between two collections. A Structure that ended with them would take along what every cache and every plan has learned about it.
// The usual Structure of a layout and the one in the record of an allocation site last as long as the program: there is one for each, as a function that the engine interprets keeps the Structure of each of its literals.
// A variant lasts through an Eden collection, except for one whose own prototype is as young as it is: both may belong to objects that are made once. A full collection ends every variant that no object has.
template<typename Appender>
ALWAYS_INLINE void StructureTable::visitUsualStructures(Appender& appender, size_t begin, size_t end)
{
    RememberedSlotSet::forEachEntryInRegion(usualStructureIDs(), begin, end, [&](uint32_t structureID) ALWAYS_INLINE_LAMBDA {
        if (structureID)
            appender.appendUnbarriered(std::bit_cast<StructureID>(structureID).decode());
    });
}

template<typename Appender>
ALWAYS_INLINE void StructureTable::visitRecords(Appender& appender, size_t begin, size_t end)
{
    RememberedSlotSet::forEachEntryInRegion(allocationSiteRecords(), begin, end, [&](const AllocationSiteRecord& record) ALWAYS_INLINE_LAMBDA {
        if (uint32_t structureID = static_cast<uint32_t>(record.header))
            appender.appendUnbarriered(std::bit_cast<StructureID>(structureID).decode());
    });
}

// A full collection looks at every entry of both tables. Neither table moves, so the markers share that work among themselves.
template<typename Visitor>
void StructureTable::visitRoots(Visitor& visitor)
{
    bool isDeferred = m_writtenStructures.tryDeferFullVisit(visitor, [](SlotVisitor& visitor, void* owner, size_t begin, size_t end) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            static_cast<StructureTable*>(owner)->visitUsualStructures(appender, begin, end);
        });
    }, this);
    if (!isDeferred) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            m_writtenStructures.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) ALWAYS_INLINE_LAMBDA {
                visitUsualStructures(appender, begin, end);
            });
        });
    }
    isDeferred = m_writtenRecords.tryDeferFullVisit(visitor, [](SlotVisitor& visitor, void* owner, size_t begin, size_t end) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            static_cast<StructureTable*>(owner)->visitRecords(appender, begin, end);
        });
    }, this);
    if (!isDeferred) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            m_writtenRecords.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) ALWAYS_INLINE_LAMBDA {
                visitRecords(appender, begin, end);
            });
        });
    }
    if (visitor.collectionScope() != CollectionScope::Eden)
        return;
    for (StructureID id : m_variantsMadeSinceCollection) {
        CompiledStructure* variant = VariantMaps::decode(id);
        JSObject* prototype = variant->storedPrototype().getObject();
        if (!variant->hasOtherPrototype() || !prototype || visitor.isMarked(prototype))
            visitor.appendUnbarriered(variant);
    }
}

template void StructureTable::visitRoots(AbstractSlotVisitor&);
template void StructureTable::visitRoots(SlotVisitor&);

size_t StructureTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    {
        Locker locker { m_state.caches->lock() };
        m_variantsMadeSinceCollection.shrink(0);
        m_variants->listed.removeAllMatching([&](StructureID id) {
            return !vm.heap.isMarked(id.decode());
        });
    }
    // Whatever region of the two tables a collection looks at, it has taken every entry there for a root: none has ended.
    // A validating build looks all the same, after an Eden collection at everything: an entry that was written without a record may have.
    if (!validatesRuntime(vm)) [[likely]]
        return 0;

    size_t removed = 0;
    auto isDead = [&](uint32_t structureIDOrZero) {
        return structureIDOrZero && !vm.heap.isMarked(std::bit_cast<StructureID>(structureIDOrZero).decode());
    };
    m_writtenStructures.forEachRegionToSweep(scope, [&](size_t begin, size_t end) {
        RememberedSlotSet::forEachEntryInRegion(usualStructureIDs(), begin, end, [&](uint32_t& structureID) {
            if (!isDead(structureID))
                return;
            structureID = 0;
            ++removed;
        });
    });
    // Compiled code takes the slow path at a record whose allocator has no room, and the slow path fills a record without a header.
    m_writtenRecords.forEachRegionToSweep(scope, [&](size_t begin, size_t end) {
        RememberedSlotSet::forEachEntryInRegion(allocationSiteRecords(), begin, end, [&](AllocationSiteRecord& record) {
            if (!isDead(static_cast<uint32_t>(record.header)))
                return;
            record = emptyAllocationSiteRecord();
            ++removed;
        });
    });
    return removed;
}

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

CompiledStructure* StructureTable::usualStructureIfExists(uint32_t shape) const
{
    RELEASE_ASSERT(shape < m_tables.firstEnvironmentShape);
    uint32_t known = m_structures[shape];
    return known ? VariantMaps::decode(std::bit_cast<StructureID>(known)) : nullptr;
}

StructureTable::Variant StructureTable::usualVariantOf(VM& vm, const LayoutData& layout)
{
    Variant usual { .shape = layout.shape, .prototype = m_state.instance.globalObject->objectPrototype() };
    switch (layout.kind()) {
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return usual;
    case LayoutKind::Instance:
        if (JSObject* classPrototype = m_state.objects->classes.classPrototypeOfInstances(vm, m_state.objects->classes.classOfInstances(layout)))
            usual.prototype = classPrototype;
        else
            usual.hasOtherPrototype = true;
        return usual;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CompiledStructure* StructureTable::create(VM& vm, const LayoutData& layout, const Variant& variant)
{
    JSGlobalObject* realm = m_state.instance.globalObject;
    CompiledStructure*& templateStructure = m_templateStructures[variant.hasOtherPrototype];
    if (!templateStructure) {
        templateStructure = Object::createStructure(vm, { .realm = realm, .prototype = jsNull(), .layout = &layout, .hasOtherPrototype = variant.hasOtherPrototype });
        m_state.immortalCells->add(templateStructure);
    }
    if (JSObject* object = variant.prototype.getObject())
        object->didBecomePrototype(vm);
    count(vm, RuntimeCounterID::CompiledStructuresMade);
    if (m_made) [[unlikely]]
        m_made->note(layout, variant);
    return CompiledStructure::createForObject(vm, *templateStructure, variant.parametersFor(realm, layout));
}

CompiledStructure* StructureTable::createUsual(VM& vm, const LayoutData& layout, const Variant& usual)
{
    CompiledStructure* created = create(vm, layout, usual);
    uint32_t& known = m_structures[layout.shape];
    known = created->id().bits();
    m_writtenStructures.record(&known);
    if (usual.hasOtherPrototype)
        m_state.objects->classes.didChangeInstancePrototype(vm, layout, usual.prototype);
    return created;
}

CompiledStructure* StructureTable::objectStructure(VM& vm, uint32_t shape)
{
    if (CompiledStructure* known = usualStructureIfExists(shape))
        return known;
    RELEASE_ASSERT(m_tables.hasLayoutForShape(shape));
    const LayoutData& layout = m_tables.layoutForShape(shape);
    return createUsual(vm, layout, usualVariantOf(vm, layout));
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
        variant.heldExtraProperties = structure.heldExtraProperties();
    }
    if (structure.hasOutOfLineKeyOrder())
        variant.outOfLineKeyOrder = structure.outOfLineKeyOrder();
    return variant;
}

CompiledStructure* StructureTable::find(const Variant& variant) const
{
    if (variant.heldExtraProperties)
        return nullptr;
    if (CompiledStructure* usual = usualStructureIfExists(variant.shape); usual && variant.describes(*usual))
        return usual;
    CompiledStructure* found = m_variants->structures.get(variant.key());
    return found && variant.describes(*found) ? found : nullptr;
}

CompiledStructure* StructureTable::findOrCreate(VM& vm, const Variant& requested)
{
    // outOfLineKeyOrder may point into a Structure that nothing else keeps alive.
    DeferGC deferGC(vm);
    CompiledStructure* usualStructure = usualStructureIfExists(requested.shape);
    RELEASE_ASSERT(m_tables.hasLayoutForShape(requested.shape));
    const LayoutData& layout = m_tables.layoutForShape(requested.shape);
    // Where every object of the layout has another Structure, nobody needs the usual one.
    Variant usual = usualStructure ? variantOf(*usualStructure) : usualVariantOf(vm, layout);
    Variant variant = requested;
    // Compiled code tests the flag after the read that relies on it, so an object that had it keeps it.
    variant.hasOtherPrototype |= usual.hasOtherPrototype || usual.prototype != variant.prototype;
    if (CompiledStructure* existing = find(variant))
        return existing;
    if (variant.isUsual(usual))
        return createUsual(vm, layout, usual);
    CompiledStructure* created = create(vm, layout, variant);
    if (!variant.heldExtraProperties) {
        m_variants->structures.set(variant.key(), created);
        Locker locker { m_state.caches->lock() };
        m_variantsMadeSinceCollection.append(created->id());
        m_variants->listed.append(created->id());
    }
    if (variant.hasOtherPrototype)
        m_state.objects->classes.didChangeInstancePrototype(vm, layout, variant.prototype);
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
    // Compiled code replays a transition by storing the StructureID and setting IsPristine, which a compiled object has where its prototype is not its layout's, in the cell: it looks for no cycle and tells nobody who depends on the chain.
    constexpr uint32_t pristineBitInBlob = IsPristine << 16;
    if (structure->isPristine() && structure->typeInfoBlob() == (current.typeInfoBlob() | pristineBitInBlob) && !current.mayBePrototype() && !current.heldExtraProperties())
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
    if (!named || !named->propertyAccessesAreCacheable() || !current.extraPropertiesSlot()) {
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
    if (!current.extraPropertiesSlot())
        return nullptr;
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

// Such a Structure belongs to one object: no table lists it.
CompiledStructure* StructureTable::holdingExtraProperties(VM& vm, const CompiledStructure& current, JSObject* extraProperties)
{
    ASSERT(!current.extraPropertiesSlot() && !current.hasExtraProperties());
    Variant variant = variantOf(current);
    variant.setExtraProperties(Variant::ExtraProperties::Unnamed);
    variant.heldExtraProperties = extraProperties;
    return findOrCreate(vm, variant);
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
