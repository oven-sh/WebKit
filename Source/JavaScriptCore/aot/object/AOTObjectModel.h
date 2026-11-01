/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosureTable.h"
#include "AOTCompiledStructure.h"
#include "AOTEnvironment.h"
#include "AOTKeyOrderTable.h"
#include "AOTObjectWords.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeType.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "FreeList.h"
#include "JSCJSValue.h"
#include "LocalAllocator.h"
#include "RememberedSlot.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class AcceptedValue;
class Object;
struct ObjectModelCaches;
struct State;

class RuntimeLayoutTable {
    WTF_MAKE_NONCOPYABLE(RuntimeLayoutTable);
public:
    explicit RuntimeLayoutTable(State&);
    ~RuntimeLayoutTable();

    const LayoutData& layoutForShape(uint32_t shape) const { return m_tables.layoutForShape(shape); }
    bool hasLayoutForShape(uint32_t shape) const { return m_tables.hasLayoutForShape(shape); }
    const AllocationData& allocation(uint32_t number) const { return m_tables.allocations()[number]; }
    const RuntimeType& typeAt(uint32_t byteOffset) const { return m_tables.typeAt(byteOffset); }
    const RuntimeType& typeOf(const FieldData& field) const { return typeAt(field.type); }
    const RuntimeType* typeWithIndex(uint32_t typeIndex) const;

    const FieldData* field(VM&, const LayoutData&, UniquedStringImpl*);
    UniquedStringImpl* nameOf(VM&, const FieldData&);

    bool valueHasType(const RuntimeType&, JSValue) const;
    bool objectHasType(const RuntimeType&, JSCell*) const;
    bool cellPassesTest(const RuntimeCellTest&, JSCell*) const;
    bool shapeHasType(const RuntimeType&, uint32_t shape) const;
    // Only the engine stores a brand. What it stores is a class token, which is no JavaScript value, so no type describes it.
    bool fieldAccepts(const FieldData& field, JSValue value) const { return field.isBrand() || valueHasType(typeOf(field), value); }
    bool slotAdmits(const FieldData& field, JSValue value) const
    {
        if (field.isAccessor())
            return false;
        return fieldAccepts(field, value) || (field.slotHoldsUndefinedAndNull && value.isUndefinedOrNull());
    }
    std::optional<AcceptedValue> accept(const FieldData&, JSValue) const;
    bool passes(const ShapeTestData& test, uint32_t shape) const { return m_tables.passes(test, shape); }
    Object* compiledObject(JSValue) const;
    // A layout or a function that a type lists is of this instance. Another realm that runs the image has objects of the same shapes and closures of the same numbers.
    bool isOfThisRealm(const JSCell*) const;

    bool hasSymbolKey(const FieldData&) const;
    bool hasStringKey(const FieldData&) const;
    bool mayBeAbsent(const FieldData&) const;
    bool everyObjectHadRemovedField(const LayoutData&) const;
    // What an object of the layout holds behind its header when Object::create() has made it and nothing is stored yet: the same bytes every time. Kept for whoever makes such objects again and again.
    struct BodyAtBirth {
        std::span<const uint8_t> span() const { return cell.span().subspan(sizeof(JSCell), size); }

        // The cell in whole chunks of sixteen bytes, with zeros for its header and behind its last byte.
        Vector<uint8_t> cell;
        size_t size { 0 };
        bool isBornUnfinished { false };
    };
    const BodyAtBirth* bodyAtBirthIfKnown(const LayoutData& layout) const
    {
        auto found = m_bodiesAtBirth.find(layout.index + 1);
        return found == m_bodiesAtBirth.end() ? nullptr : &found->value;
    }
    void rememberBodyAtBirth(const LayoutData& layout, std::span<const uint8_t> bytes, bool isBornUnfinished)
    {
        Vector<uint8_t> cell;
        cell.fill(0, WTF::roundUpToMultipleOf<16>(sizeof(JSCell) + bytes.size()));
        memcpySpan(cell.mutableSpan().subspan(sizeof(JSCell)), bytes);
        m_bodiesAtBirth.add(layout.index + 1, BodyAtBirth { WTF::move(cell), bytes.size(), isBornUnfinished });
    }
    void didReadAbsentField(VM& vm, const LayoutData& layout, UniquedStringImpl* name) const
    {
        if (!m_tables.removedFields().empty()) [[unlikely]]
            failIfFieldWasRemoved(vm, layout, name);
    }
    void willAddProperty(VM& vm, const LayoutData& layout, UniquedStringImpl* name) const
    {
        if (!m_tables.addedNames().empty()) [[unlikely]]
            failIfNameIsNotListed(vm, layout, name);
    }

private:
    struct CachedField {
        uint32_t shape;
        UniquedStringImpl* name;
        const FieldData* field;
    };
    static constexpr unsigned numberOfCachedFields = 4096;

    const FieldData* findField(VM&, const LayoutData&, UniquedStringImpl*);
    const FieldData* findFieldWithoutColumn(const LayoutData&, uint32_t identifierNumber);
    void failIfFieldWasRemoved(VM&, const LayoutData&, UniquedStringImpl*) const;
    void failIfNameIsNotListed(VM&, const LayoutData&, UniquedStringImpl*) const;

    State& m_state;
    const ProgramTables& m_tables;
    CachedField* m_cachedFields { nullptr };
    // By the index of a layout plus one: where each name of a wide layout lies among its fields, made at the first lookup of a name without a column.
    UncheckedKeyHashMap<uint32_t, Vector<uint16_t>> m_fieldsByName;
    // By the index of a layout plus one.
    UncheckedKeyHashMap<uint32_t, BodyAtBirth> m_bodiesAtBirth;
};

// The table and the records of the allocation sites name Structures and keep none of them alive through a full collection: the objects do that.
class StructureTable {
    WTF_MAKE_NONCOPYABLE(StructureTable);
public:
    explicit StructureTable(State&);
    ~StructureTable();

    Structure* structure(VM&, uint32_t shape);
    CompiledStructure* objectStructure(VM&, uint32_t shape);
    CompiledStructure* objectStructure(VM&, uint32_t shape, JSValue prototype);

    CompiledStructure* withPrototype(VM&, const CompiledStructure&, JSValue prototype);
    CompiledStructure* asPrototype(VM&, const CompiledStructure&);
    CompiledStructure* withExtraProperties(VM&, const CompiledStructure&, Structure* extraPropertiesStructureOrNull);
    CompiledStructure* existingWithExtraProperties(const CompiledStructure&, Structure& extraPropertiesStructure);
    CompiledStructure* holdingExtraProperties(VM&, const CompiledStructure&, JSObject* extraProperties);
    enum class HeldKeyOrder : bool { IsFoundByOthers, IsFoundByNobody };
    CompiledStructure* withOutOfLineKeyOrder(VM&, const CompiledStructure&, std::span<const uint16_t> entries, HeldKeyOrder = HeldKeyOrder::IsFoundByOthers);
    CompiledStructure* withKeyOrderOfOneObject(VM&, const CompiledStructure&, std::span<const uint16_t> entries, std::span<const RefPtr<UniquedStringImpl>> keysNamedByOrder);
    CompiledStructure* withNumberedKeyOrder(VM&, const CompiledStructure&);

    // Where an object went that was given a field or lost one and found no numbered order for its keys: from the Structure and the order word that said its keys to those that say them now.
    // The first such object walks the numbered orders and searches the held ones. The next one with the same keys takes the same step.
    struct StepOfKeyOrder {
        StructureID before;
        uint32_t orderAndCountBefore;
        uint32_t fieldAndKind;
        StructureID after;
        uint32_t orderAndCountAfter;
    };
    enum class KindOfStep : bool { Creates, Removes };
    const StepOfKeyOrder* findStepOfKeyOrder(StructureID before, uint32_t orderAndCountBefore, uint16_t field, KindOfStep) const;
    void rememberStepOfKeyOrder(StructureID before, uint32_t orderAndCountBefore, uint16_t field, KindOfStep, StructureID after, uint32_t orderAndCountAfter);

    JSCell* allocate(VM&, uint32_t allocation);
    // An object has its header, its order word and its shape word, as behind the stub, and zeros behind them.
    JSCell* allocateAsCompiledCodeDoes(VM&, uint32_t allocation);
    AllocationSiteRecord& fillAllocationSite(VM&, uint32_t allocation);
    // A site that had no class object to take a prototype from predicts the first one that its operation named.
    void predictStructure(VM&, uint32_t allocation, CompiledStructure*);
    std::optional<StructureID> structureIDOf(uint32_t allocation) const;
    // Whether it found one to forget.
    bool forgetStructuresOf(const LayoutData& classLayout);
    // For a class of created objects that gets its prototype object. It looks at the records of the places only where one of them can name a Structure of the class.
    void forgetStructuresOfCreatedObjects(VM&, const LayoutData& classLayout);
    // The first object of a class of created objects that awaits its prototype object is born with such a Structure: see usualVariantOf(). No object is handed on with it, and no record
    // of a place keeps it.
    bool isBelowAwaitedPrototype(const Structure& structure) const { return m_awaitedPrototype && structure.storedPrototype() == JSValue(m_awaitedPrototype); }
    // For the operation behind Object.create, which alone sees such an object. It answers the Structure of the object below the prototype that the call was handed.
    CompiledStructure* leaveAwaitedPrototype(VM&, const CompiledStructure& atBirth, JSValue prototype);

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

    void dumpStructuresMade(PrintStream&) const;

private:
    using Variant = CompiledStructure::ObjectParameters;
    struct VariantMaps;
    struct MadeStructures;

    static constexpr unsigned structuresRegionShift = 6;
    static constexpr unsigned recordsRegionShift = 8;

    // What compiled code takes for a LocalAllocator: it has no room and no next interval.
    struct AllocatorWithoutRoom {
        uint8_t bytesBeforeFreeList[LocalAllocator::offsetOfFreeList()];
        FreeList freeList { 0 };
    };

    AllocationSiteRecord emptyAllocationSiteRecord() { return { .header = 0, .orderAndShapeWords = 0, .allocator = &m_allocatorWithoutRoom, .prototype = 0 }; }

    std::span<uint32_t> usualStructureIDs() const { return { m_structures, m_tables.firstEnvironmentShape }; }
    std::span<AllocationSiteRecord> allocationSiteRecords() const;

    CompiledStructure* usualStructureIfExists(uint32_t shape) const;
    CompiledStructure* usualStructure(VM&, uint32_t shape);
    Variant usualVariantOf(VM&, const LayoutData&);
    JSObject* awaitedPrototype(VM&);
    void validateStructuresBelowAwaitedPrototype(VM&) const;
    CompiledStructure* createUsual(VM&, const LayoutData&, const Variant&);
    CompiledStructure* create(VM&, const LayoutData&, const Variant&);
    CompiledStructure* find(const Variant&) const;
    CompiledStructure* findOrCreate(VM&, const Variant&);
    static Variant variantOf(const CompiledStructure&);
    // For a Structure that differs in something else than the key order.
    static Variant variantWithKeyOrderOf(const CompiledStructure&);
    // No table lists such a Structure.
    static bool belongsToOneObject(const Variant& variant) { return variant.heldExtraProperties || variant.keysNamedByOrder; }
    static bool isListed(const Variant& variant) { return !belongsToOneObject(variant) && variant.isFoundByKeyOrder; }

    template<typename Appender> void visitUsualStructures(Appender&, size_t begin, size_t end);
    template<typename Appender> void visitRecords(Appender&, size_t begin, size_t end);

    AllocationSiteRecord* recordOfSameAllocation(uint32_t allocation) const;
    AllocationSiteRecord& ensureRecord(uint32_t allocation);
    AllocationSiteRecord& ensureRecord(uint32_t allocation, uint32_t shape);
    AllocationSiteRecord& filledRecord(VM&, uint32_t allocation);

    State& m_state;
    const ProgramTables& m_tables;
    EnvironmentShapes m_environmentShapes;
    uint32_t* m_structures { nullptr };
    // Each shape once, whether or not its entry in m_structures is still set.
    Vector<uint32_t> m_shapesWithStructure;
    BitVector m_shapeHasStructure;
    uint32_t m_numberOfAllocationSiteRecords { 0 };
    AllocatorWithoutRoom m_allocatorWithoutRoom { };
    // No cell has them. Every other Structure of a compiled object starts as a copy of one: the second is for objects with another prototype than their layout's.
    std::array<CompiledStructure*, 2> m_templateStructures { };
    // Stands where the prototype object that a class of created objects awaits will stand: the first object of the class is born below it. No program holds it, so no call of
    // Object.create is handed it. This is not the lazy prototype object of a class that MemberGuardWatchpoints speaks of.
    JSObject* m_awaitedPrototype { nullptr };
    // What fillAllocationSite() hands out in place of the record of a place whose object is born below m_awaitedPrototype. The record itself stays without a header, and nothing
    // visits this one: the Structure that it names is the usual one of its layout, which is a root.
    AllocationSiteRecord m_recordBelowAwaitedPrototype { };
    std::unique_ptr<VariantMaps> m_variants;
    using StepsOfKeyOrders = std::array<StepOfKeyOrder, 4096>;
    // Of the newer of two steps.
    static size_t placeOfStep(StructureID before, uint32_t orderAndCountBefore, uint32_t fieldAndKind) { return ((before.bits() >> 4 ^ orderAndCountBefore * 0x9e3779b1u ^ fieldAndKind * 0x85ebca6bu) * 0x9e3779b1u) >> 20 & ~static_cast<size_t>(1); }
    // Made by the first step that is remembered. A collection forgets them all.
    std::unique_ptr<StepsOfKeyOrders> m_stepsOfKeyOrders;
    // In a run with --aotCountsPath.
    std::unique_ptr<MadeStructures> m_made;
    RememberedSlotSet m_writtenStructures;
    RememberedSlotSet m_writtenRecords;
};

struct ObjectModel {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ObjectModel);
    WTF_MAKE_NONCOPYABLE(ObjectModel);

    explicit ObjectModel(State&);
    ~ObjectModel();

    RuntimeLayoutTable layouts;
    StructureTable structures;
    KeyOrderTable keyOrders;
    ClosureTable closures;
    ClassTable classes;
    std::unique_ptr<ObjectModelCaches> caches;
};

inline const FieldData* Object::fieldNamed(State& state, PropertyName name) const
{
    // Whoever has a state at hand passes it to save the way to it. It may be the caller's, and the object of another image.
    if (state.sharesRealm) [[unlikely]]
        return this->state().objects->layouts.field(*state.instance.vm, layout(), name.uid());
    return state.objects->layouts.field(*state.instance.vm, layout(), name.uid());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
