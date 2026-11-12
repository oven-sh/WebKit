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
#include <array>
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
    AllocationData allocation(uint32_t number) const { return m_tables.allocations()[number]; }
    const RuntimeType& typeAt(uint32_t byteOffset) const { return m_tables.typeAt(byteOffset); }
    const RuntimeType& typeOf(const FieldData& field) const { return typeAt(m_tables.offsetOfTypeOf(field)); }

    const FieldData* field(VM&, const LayoutData&, UniquedStringImpl*);
    const FieldData* staticField(const LayoutData& classLayout, uint32_t identifierNumber) { return findByName(classLayout.staticFields(), classLayout.index * 3 + 2, identifierNumber); }
    const MethodData* staticMethod(const LayoutData& classLayout, uint32_t identifierNumber) { return findByName(classLayout.staticMethods(), classLayout.index * 3 + 3, identifierNumber); }
    UniquedStringImpl* nameOf(VM&, const FieldData&);

    bool valueHasType(const RuntimeType&, JSValue) const;
    bool objectHasType(const RuntimeType&, JSCell*) const;
    bool cellPassesTest(const RuntimeCellTest&, JSCell*) const;
    bool fieldAccepts(const FieldData& field, JSValue value) const { return field.isBrand() || valueHasType(typeOf(field), value); }
    bool slotAdmits(const FieldData& field, JSValue value) const
    {
        if (field.isAccessor())
            return false;
        return fieldAccepts(field, value) || (field.slotHoldsUndefinedAndNull && value.isUndefinedOrNull());
    }
    std::optional<AcceptedValue> accept(const FieldData&, JSValue) const;
    static bool slotTakesHalf(const FieldData&, const JSCell* getterOrSetter);
    bool passes(const ShapeTestData& test, uint32_t shape) const { return m_tables.passes(test, shape); }
    Object* compiledObject(JSValue) const;
    bool isOfThisRealm(const JSCell*) const;

    bool hasSymbolKey(const FieldData&) const;
    bool hasStringKey(const FieldData&) const;

private:
    struct CachedField {
        uint32_t shape;
        UniquedStringImpl* name;
        const FieldData* field;
    };
    static constexpr unsigned maxCachedFields = 4096;
    static constexpr unsigned initialCachedFields = 256;
    static_assert(hasOneBitSet(maxCachedFields) && hasOneBitSet(initialCachedFields) && initialCachedFields < maxCachedFields);

    CachedField& placeOfCachedField(uint32_t shape, const UniquedStringImpl* name) { return m_cachedFields[(shape * 0x9e3779b1u ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(name) >> 4)) & m_maskOfCachedFields]; }
    std::span<CachedField> cachedFields() { return { m_cachedFields, static_cast<size_t>(m_maskOfCachedFields) + 1 }; }
    void growCachedFields();
    const FieldData* findField(VM&, const LayoutData&, UniquedStringImpl*);
    const FieldData* findFieldWithoutColumn(const LayoutData&, uint32_t identifierNumber);
    static constexpr size_t minRecordsToIndex = 12;
    static bool hasLiteralName(const FieldData&) { return true; }
    static bool hasLiteralName(const MethodData& method) { return !(method.flags & methodHasComputedName); }
    template<typename Record> static Vector<uint32_t> indexByName(std::span<const Record>);
    template<typename Record> const Record* findInIndex(std::span<const Record>, uint32_t indexKey, uint32_t identifierNumber);
    template<typename Record> const Record* findByName(std::span<const Record> records, uint32_t indexKey, uint32_t identifierNumber)
    {
        if (records.size() >= minRecordsToIndex) [[unlikely]]
            return findInIndex(records, indexKey, identifierNumber);
        for (const Record& record : records) {
            if (hasLiteralName(record) && record.name == identifierNumber)
                return &record;
        }
        return nullptr;
    }

    State& m_state;
    const ProgramTables& m_tables;
    CachedField* m_cachedFields { nullptr };
    uint32_t m_maskOfCachedFields { initialCachedFields - 1 };
    uint32_t m_fieldsCachedSinceGrowth { 0 };
    UncheckedKeyHashMap<uint32_t, Vector<uint32_t>> m_recordsByName;
};

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
    CompiledStructure* withReportedReplacements(VM&, const CompiledStructure&);
    CompiledStructure* withExtraProperties(VM&, const CompiledStructure&, Structure* extraPropertiesStructureOrNull);
    CompiledStructure* existingWithExtraProperties(const CompiledStructure&, Structure& extraPropertiesStructure);
    CompiledStructure* holdingExtraProperties(VM&, const CompiledStructure&, JSObject* extraProperties);
    CompiledStructure* withoutHeldStructure(VM&, const CompiledStructure&);
    void growFutureCells(const LayoutData&);
    enum class HeldKeyOrder : bool { IsFoundByOthers, IsFoundByNobody };
    CompiledStructure* withOutOfLineKeyOrder(VM&, const CompiledStructure&, std::span<const uint16_t> entries, HeldKeyOrder = HeldKeyOrder::IsFoundByOthers);
    CompiledStructure* withKeyOrderOfOneObject(VM&, const CompiledStructure&, std::span<const uint16_t> entries, std::span<const RefPtr<UniquedStringImpl>> keysNamedByOrder);
    CompiledStructure* withNumberedKeyOrder(VM&, const CompiledStructure&);

    struct StepOfKeyOrder {
        StructureID before;
        uint32_t orderAndCountBefore;
        uint32_t fieldAndKind;
        StructureID after;
        uint32_t orderAndCountAfter;
    };
    enum class KindOfStep : uint8_t { Creates, Removes, CreatesAtSlot };
    const StepOfKeyOrder* findStepOfKeyOrder(StructureID before, uint32_t orderAndCountBefore, uint16_t field, KindOfStep) const;
    void rememberStepOfKeyOrder(StructureID before, uint32_t orderAndCountBefore, uint16_t field, KindOfStep, StructureID after, uint32_t orderAndCountAfter);
    JSCellButterfly* findKeysOfOrderWithoutNumber(StructureID, uint32_t count, KeyListKind) const;
    void rememberKeysOfOrderWithoutNumber(StructureID, uint32_t count, KeyListKind, JSCellButterfly*);

    JSCell* allocate(VM&, uint32_t allocation);
    JSCell* allocateAsCompiledCodeDoes(VM&, uint32_t allocation);
    AllocationSiteRecord& fillAllocationSite(VM&, uint32_t allocation);
    void predictStructure(VM&, uint32_t allocation, CompiledStructure*);
    std::optional<StructureID> structureIDOf(uint32_t allocation) const;
    size_t numberOfStructuresMade() const { return m_numberOfStructuresMade; }
    void forgetStructuresOf(const LayoutData& classLayout);

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using Variant = CompiledStructure::ObjectParameters;
    struct VariantMaps;

    static constexpr unsigned structuresRegionShift = 6;
    static constexpr unsigned recordsRegionShift = 8;

    struct AllocatorWithoutRoom {
        uint8_t bytesBeforeFreeList[LocalAllocator::offsetOfFreeList()];
        FreeList freeList { 0 };
    };

    AllocationSiteRecord emptyAllocationSiteRecord() { return { .header = 0, .orderAndShapeWords = 0, .allocator = &m_allocatorWithoutRoom, .prototype = 0 }; }

    std::span<uint32_t> usualStructureIDs() const { return { m_structures, m_tables.firstEnvironmentShape }; }
    std::span<AllocationSiteRecord> allocationSiteRecords() const;

    CompiledStructure* usualStructureIfExists(uint32_t shape) const;
    CompiledStructure* usualStructure(VM&, uint32_t shape);
    CompiledStructure* structureForFirstObject(VM&, uint32_t shape);
    bool takeFirstObjectOf(uint32_t allocation);
    void holdUsualStructure(uint32_t shape);
    Variant usualVariantOf(VM&, const LayoutData&);
    CompiledStructure* createUsual(VM&, const LayoutData&, Variant&);
    CompiledStructure* create(VM&, const LayoutData&, Variant&);
    CompiledStructure* find(const Variant&) const;
    CompiledStructure* findOrCreate(VM&, Variant&);
    CompiledStructure* withSpareWord(VM&, const CompiledStructure&);
    static Variant variantOf(const CompiledStructure&);
    static Variant variantWithKeyOrderOf(const CompiledStructure&);
    static bool belongsToOneObject(const Variant& variant) { return variant.heldExtraProperties || variant.keysNamedByOrder; }
    static bool isListed(const Variant& variant) { return !belongsToOneObject(variant) && variant.isFoundByKeyOrder; }

    template<typename Appender> void visitUsualStructures(Appender&, size_t begin, size_t end);
    template<typename Appender> void visitRecords(Appender&, size_t begin, size_t end);
    static bool livesWhilePrototypeLives(const LayoutData& layout) { return layout.isClass() && layout.evaluation() == EvaluationState::PredictedOnce; }
    bool livesWhilePrototypeLives(const AllocationSiteRecord&) const;
    bool noteWhetherLivesWhilePrototypeLives(const LayoutData&);
    void didNameStructureThatLivesWhilePrototypeLives(AllocationSiteRecord&);
    void forgetStructuresThatDied(VM&, CollectionScope);

    AllocationSiteRecord* recordOfSameAllocation(uint32_t allocation) const;
    AllocationSiteRecord& ensureRecord(uint32_t allocation);
    AllocationSiteRecord& ensureRecord(uint32_t allocation, uint32_t shape);
    AllocationSiteRecord recordForObject(VM&, uint32_t allocation);

    State& m_state;
    const ProgramTables& m_tables;
    EnvironmentShapes m_environmentShapes;
    uint32_t* m_structures { nullptr };
    Vector<uint32_t> m_shapesWithStructure;
    BitVector m_shapeHasStructure;
    uint32_t m_numberOfAllocationSiteRecords { 0 };
    size_t m_numberOfStructuresMade { 0 };
    AllocatorWithoutRoom m_allocatorWithoutRoom { };
    std::array<CompiledStructure*, 2> m_templateStructures { };
    std::unique_ptr<VariantMaps> m_variants;
    static constexpr unsigned logMaxSteps = 12;
    static constexpr unsigned logInitialSteps = 8;
    size_t placeOfStep(StructureID before, uint32_t orderAndCountBefore, uint32_t fieldAndKind) const { return ((before.bits() >> 4 ^ orderAndCountBefore * 0x9e3779b1u ^ fieldAndKind * 0x85ebca6bu) * 0x9e3779b1u) >> (32 - m_logNumberOfSteps) & ~static_cast<size_t>(1); }
    void putKeyOrderStep(const StepOfKeyOrder&);
    void growKeyOrderSteps();
    StepOfKeyOrder* m_stepsOfKeyOrders { nullptr };
    unsigned m_logNumberOfSteps { logInitialSteps };
    uint32_t m_stepsPutSinceGrowth { 0 };
    struct KeysOfOrderWithoutNumber {
        StructureID structure;
        uint32_t countAndKind;
        JSCellButterfly* list;
    };
    static constexpr size_t numberOfKeyListsRemembered = 1024;
    static size_t placeOfKeys(StructureID structure, uint32_t countAndKind) { return ((structure.bits() >> 4 ^ countAndKind * 0x85ebca6bu) * 0x9e3779b1u) >> 22; }
    KeysOfOrderWithoutNumber* m_keysOfOrdersWithoutNumber { nullptr };
    RememberedSlotSet m_writtenStructures;
    RememberedSlotSet m_writtenRecords;
    BitVector m_shapesThatLiveWhilePrototypesLive;
    Vector<uint32_t> m_listOfShapesThatLiveWhilePrototypesLive;
    Vector<uint32_t> m_shapesGivenStructuresSinceCollection;
    BitVector m_weakShapes;
    Vector<uint32_t> m_listOfWeakShapes;
    BitVector m_allocationsThatMadeObjects;
    BitVector m_shapesWithGrownCells;
    UncheckedKeyHashMap<AllocationSiteRecord*, std::unique_ptr<GrownCells>> m_grownCells;
    void* grownCellAllocator(AllocationSiteRecord&);
    void disarmDeadGrownCells(VM&);
    struct GrownStructure {
        StructureID before;
        StructureID after;
    };
    static constexpr size_t grownStructureCount = 64;
    static size_t grownStructureSlot(StructureID before) { return (before.bits() >> 4) % grownStructureCount; }
    std::array<GrownStructure, grownStructureCount> m_recentGrownStructures { };
    BitVector m_recordsThatLiveWhilePrototypesLive;
    Vector<AllocationSiteRecord*> m_listOfRecordsThatLiveWhilePrototypesLive;
    Vector<AllocationSiteRecord*> m_recordsGivenStructuresSinceCollection;
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
    return state.objects->layouts.field(*state.instance.vm, layout(), name.uid());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
