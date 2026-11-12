/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectModel.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTCellFormat.h"
#include "AOTCheckedSpan.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTFieldTable.h"
#include "AOTFunctionTables.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTImage.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTInterpreterSlowPaths.h"
#include "AOTModuleNamespace.h"
#include "AOTObject.h"
#include "AOTObjectModelCaches.h"
#include "AOTPrimitives.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeReporter.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AbstractSlotVisitorInlines.h"
#include "ConservativeRoots.h"
#include "DeferGCInlines.h"
#include "FastMarkAppender.h"
#include "GetterSetter.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "PropertyName.h"
#include "SlotVisitorInlines.h"
#include "Symbol.h"
#include <algorithm>
#include <array>
#include <bit>
#include <wtf/BitSet.h>
#include <wtf/Hasher.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringView.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

ObjectModel::ObjectModel(State& state)
    : layouts(state)
    , structures(state)
    , keyOrders(state)
    , closures(state)
    , classes(state)
    , caches(makeUnique<ObjectModelCaches>(state))
{
}

ObjectModel::~ObjectModel() = default;

ObjectModelCaches::ObjectModelCaches(State& state)
    : movePlans(state)
    , assignmentPlans(state)
    , propertyLists(state)
    , getterSources(state)
    , classStructures(state)
{
}

ObjectModelCaches::~ObjectModelCaches() = default;

GetterSourceLists::GetterSourceLists(State& state)
{
    state.caches->add(CacheKind::ClearedEachCollection, "lists of sources with getters"_s, *this);
}

enum class FieldKeyKind : uint8_t { String, Symbol, PrivateName };

static FieldKeyKind keyKindOf(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::Text:
        return FieldKeyKind::String;
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return FieldKeyKind::Symbol;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return FieldKeyKind::PrivateName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const ImageText& textOf(const State& state)
{
    return state.instance.program->text();
}

static bool matchesAnyLiteral(const ImageText& imageText, std::span<const uint32_t> literals, StringView characters)
{
    return std::ranges::any_of(literals, [&](uint32_t literal) {
        return imageText.equals(StringID(literal), characters);
    });
}

static NEVER_INLINE bool ropeMatchesAnyLiteral(JSGlobalObject* globalObject, const ImageText& imageText, std::span<const uint32_t> literals, JSString* rope)
{
    unsigned length = rope->length();
    bool hasLiteralWithLength = std::ranges::any_of(literals, [&](uint32_t literal) {
        return imageText.hasText(StringID(literal)) && imageText.view(StringID(literal)).length() == length;
    });
    if (!hasLiteralWithLength)
        return false;
    if (rope->isSubstring()) {
        auto characters = rope->view(globalObject);
        return matchesAnyLiteral(imageText, literals, characters.data);
    }
    if (rope->is8Bit()) {
        Vector<Latin1Character, 64> characters(length);
        rope->resolveToBuffer(characters.mutableSpan());
        return matchesAnyLiteral(imageText, literals, StringView(characters.span()));
    }
    Vector<char16_t, 64> characters(length);
    rope->resolveToBuffer(characters.mutableSpan());
    return matchesAnyLiteral(imageText, literals, StringView(characters.span()));
}

static bool numberHasType(const RuntimeType& type, double number)
{
    PrimitiveSet kind = Typed::primitiveOfNumber(number);
    if (!type.primitiveSet().contains(kind))
        return false;
    if (kind != PrimitiveSet::Integer)
        return true;
    auto range = type.integerRange();
    if (!range)
        return true;
    int64_t integer = static_cast<int64_t>(number);
    return integer >= range->first && integer <= range->second;
}

static bool stringHasType(const State& state, const RuntimeType& type, JSString* value)
{
    JSGlobalObject* globalObject = state.instance.globalObject;
    OptionSet<PrimitiveSet> primitives = type.primitiveSet();
    auto literals = type.literalStrings();
    if (literals.empty())
        return primitives.contains(PrimitiveSet::String);
    if (!primitives.containsAny({ PrimitiveSet::String, PrimitiveSet::LiteralString }))
        return false;
    if (StringImpl* impl = value->tryGetValueImpl())
        return matchesAnyLiteral(textOf(state), literals, StringView(*impl));
    return ropeMatchesAnyLiteral(globalObject, textOf(state), literals, value);
}

static bool inheritsBuiltinClass(JSCell* cell, BuiltinClassID builtinClass)
{
    const ClassInfo* info = BuiltinBindings::instanceClassInfoOf(builtinClass);
    return info && cell->inherits(info);
}

static bool hasSharingMode(JSCell* cell, ArrayBufferSharingMode sharingMode)
{
    auto* buffer = dynamicDowncast<JSArrayBuffer>(cell);
    return buffer && buffer->sharingMode() == sharingMode;
}

static bool isInstanceOfBuiltinClass(JSCell* cell, BuiltinClassID id)
{
    const BuiltinClass& builtinClass = BuiltinTable::singleton().at(id);
    switch (builtinClass.instanceTest) {
    case BuiltinInstanceTest::None:
        return false;
    case BuiltinInstanceTest::JSTypeEquals:
        return cell->type() == builtinClass.type;
    case BuiltinInstanceTest::ClassInfoInherits:
        return inheritsBuiltinClass(cell, id);
    case BuiltinInstanceTest::SharingModeEquals:
        return hasSharingMode(cell, builtinClass.sharingMode);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

RuntimeLayoutTable::RuntimeLayoutTable(State& state)
    : m_state(state)
    , m_tables(state.tables)
    , m_cachedFields(state.tableArena.allocate<CachedField>(maxCachedFields))
{
    state.instance.shapeSets = m_tables.shapeSetBits().data();
}

RuntimeLayoutTable::~RuntimeLayoutTable()
{
    for (auto& cached : cachedFields()) {
        if (cached.name)
            cached.name->deref();
    }
}

const FieldData* RuntimeLayoutTable::field(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    if (name->isSymbol() && !layout.hasFieldsWithSymbolKeys && !static_cast<SymbolImpl*>(name)->isPrivate())
        return nullptr;
    CachedField& cached = placeOfCachedField(layout.shape, name);
    if (cached.shape == layout.shape && cached.name == name) [[likely]]
        return cached.field;
    const FieldData* found = findField(vm, layout, name);
    ASSERT(!found || !parseIndex(PropertyName(name)));
    name->ref();
    if (cached.name)
        cached.name->deref();
    cached = { layout.shape, name, found };
    if (++m_fieldsCachedSinceGrowth > m_maskOfCachedFields / 2 && m_maskOfCachedFields + 1 < maxCachedFields) [[unlikely]]
        growCachedFields();
    return found;
}

NEVER_INLINE void RuntimeLayoutTable::growCachedFields()
{
    Vector<CachedField> previous(cachedFields());
    zeroSpan(cachedFields());
    m_maskOfCachedFields = std::min(4 * m_maskOfCachedFields + 3, maxCachedFields - 1);
    m_fieldsCachedSinceGrowth = 0;
    for (const CachedField& cached : previous) {
        if (!cached.name)
            continue;
        CachedField& place = placeOfCachedField(cached.shape, cached.name);
        if (place.name)
            place.name->deref();
        place = cached;
    }
}

const FieldData* RuntimeLayoutTable::findField(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    std::optional<uint32_t> number = identifiersOf(m_state.instance).numberOf(name);
    if (!number)
        return nullptr;
    if (m_tables.hasFieldTableColumn(*number) && layout.shape && layout.shape <= m_tables.numberOfFieldTableRows) {
        std::optional<uint16_t> index = findFieldInTable(m_tables, layout.shape, *number);
        ASSERT(!index || layout.fields()[*index].name == *number);
        return index ? &layout.fields()[*index] : nullptr;
    }
    return findFieldWithoutColumn(layout, *number);
}

static constexpr uint32_t emptySlot = UINT32_MAX;

static size_t firstSlotOfName(uint32_t identifierNumber, size_t numberOfSlots)
{
    return identifierNumber * 0x9e3779b1u >> (32 - std::countr_zero(numberOfSlots));
}

template<typename Record>
Vector<uint32_t> RuntimeLayoutTable::indexByName(std::span<const Record> records)
{
    Vector<uint32_t> slots;
    slots.fill(emptySlot, std::bit_ceil(records.size() * 2));
    size_t mask = slots.size() - 1;
    for (size_t index = 0; index < records.size(); ++index) {
        if (!hasLiteralName(records[index]))
            continue;
        size_t slot = firstSlotOfName(records[index].name, slots.size());
        while (slots[slot] != emptySlot && records[slots[slot]].name != records[index].name)
            slot = (slot + 1) & mask;
        if (slots[slot] == emptySlot)
            slots[slot] = static_cast<uint32_t>(index);
    }
    return slots;
}

template<typename Record>
const Record* RuntimeLayoutTable::findInIndex(std::span<const Record> records, uint32_t indexKey, uint32_t identifierNumber)
{
    auto found = m_recordsByName.find(indexKey);
    if (found == m_recordsByName.end())
        found = m_recordsByName.add(indexKey, indexByName(records)).iterator;
    const Vector<uint32_t>& slots = found->value;
    size_t mask = slots.size() - 1;
    for (size_t slot = firstSlotOfName(identifierNumber, slots.size()); slots[slot] != emptySlot; slot = (slot + 1) & mask) {
        const Record& record = records[slots[slot]];
        if (record.name == identifierNumber)
            return &record;
    }
    return nullptr;
}

template const FieldData* RuntimeLayoutTable::findInIndex(std::span<const FieldData>, uint32_t, uint32_t);
template const MethodData* RuntimeLayoutTable::findInIndex(std::span<const MethodData>, uint32_t, uint32_t);

const FieldData* RuntimeLayoutTable::findFieldWithoutColumn(const LayoutData& layout, uint32_t identifierNumber)
{
    return findByName(layout.fields(), layout.index * 3 + 1, identifierNumber);
}

UniquedStringImpl* RuntimeLayoutTable::nameOf(VM& vm, const FieldData& field)
{
    return identifiersOf(m_state.instance).uid(field.name);
}

bool RuntimeLayoutTable::hasSymbolKey(const FieldData& field) const
{
    return keyKindOf(textOf(m_state).kindOfIdentifier(field.name)) == FieldKeyKind::Symbol;
}

bool RuntimeLayoutTable::hasStringKey(const FieldData& field) const
{
    return keyKindOf(textOf(m_state).kindOfIdentifier(field.name)) == FieldKeyKind::String && !field.isPrivate();
}

Object* RuntimeLayoutTable::compiledObject(JSValue value) const
{
    return compiledObjectOrNull(value);
}

bool RuntimeLayoutTable::isOfThisRealm(const JSCell* cell) const
{
    return cell->structure()->realm() == m_state.instance.globalObject;
}

bool RuntimeLayoutTable::valueHasType(const RuntimeType& type, JSValue value) const
{
    if (!value)
        return false;
    OptionSet<PrimitiveSet> primitives = type.primitiveSet();
    if (value.isUndefined())
        return primitives.contains(PrimitiveSet::Undefined);
    if (value.isNull())
        return primitives.contains(PrimitiveSet::Null);
    if (value.isBoolean())
        return primitives.contains(value.asBoolean() ? PrimitiveSet::True : PrimitiveSet::False);
    if (value.isNumber())
        return numberHasType(type, value.asNumber());
    if (value.isBigInt())
        return primitives.contains(PrimitiveSet::BigInt);
    if (!value.isCell())
        return false;
    JSCell* cell = value.asCell();
    if (cell->isString())
        return stringHasType(m_state, type, asString(cell));
    if (cell->isSymbol()) {
        if (primitives.contains(PrimitiveSet::Symbol))
            return true;
    } else if (!cell->isObject())
        return false;
    return objectHasType(type, cell);
}

bool RuntimeLayoutTable::objectHasType(const RuntimeType& type, JSCell* cell) const
{
    std::span<const RuntimeCellTest> tests = type.testsOfMoreThanOneFunction();
    for (const RuntimeCellTest& test : tests) {
        if (cellPassesTest(test, cell))
            return true;
    }
    if (tests.size() == type.numberOfCellTests)
        return false;
    std::optional<uint32_t> closureNumber = closureNumberOf(cell);
    return closureNumber && isOfThisRealm(cell) && type.listsClosureNumber(*closureNumber);
}

bool RuntimeLayoutTable::slotTakesHalf(const FieldData& field, const JSCell* getterOrSetter)
{
    if (!field.takesAccessorsOfLibraryAlone() || !getterOrSetter)
        return true;
    std::optional<FunctionID> function = functionOf(getterOrSetter);
    return function && functionKindData(*function).isInRuntimeModule;
}

std::optional<AcceptedValue> RuntimeLayoutTable::accept(const FieldData& field, JSValue value) const
{
    if (field.isAccessor()) {
        if (!value || !value.isCell() || value.asCell()->type() != GetterSetterType)
            return std::nullopt;
        auto* accessor = uncheckedDowncast<GetterSetter>(value.asCell());
        bool getterIsNotLaidYet = field.getterIsProven && !(field.flags & fieldIsNotConfigurable) && accessor->isGetterNull();
        if (!getterIsNotLaidYet && !valueHasType(typeOf(field), accessor->getter()))
            return std::nullopt;
        if (!slotTakesHalf(field, accessor->isGetterNull() ? nullptr : accessor->getter()) || !slotTakesHalf(field, accessor->isSetterNull() ? nullptr : accessor->setter()))
            return std::nullopt;
        return AcceptedValue(value);
    }
    if (!slotAdmits(field, value))
        return std::nullopt;
    return AcceptedValue(value);
}

bool RuntimeLayoutTable::cellPassesTest(const RuntimeCellTest& test, JSCell* cell) const
{
    switch (test.kind) {
    case CellTestKind::IsObject:
        return cell->isObject();
    case CellTestKind::IsCallable:
        return cell->isCallable();
    case CellTestKind::CellTypeIs:
        return test.cellTypes().contains(cell->type());
    case CellTestKind::ShapeIn: {
        Object* object = compiledObject(cell);
        return object && isOfThisRealm(cell) && passes(m_tables.shapeTestAt(test.shapeTestOffset()), object->shape());
    }
    case CellTestKind::InheritsClassInfo:
        return inheritsBuiltinClass(cell, test.builtinClass());
    case CellTestKind::IsPristine:
        return isInstanceOfBuiltinClass(cell, test.builtinClass()) && cell->structure()->isPristine();
    case CellTestKind::IsBuiltinCell: {
        DeferGCForAWhile deferGC(*m_state.instance.vm);
        return cell == m_state.builtinCells->cell(test.builtinCellNumber());
    }
    case CellTestKind::IsClosureOf:
        return closureNumberOf(cell) == test.closureNumber() && isOfThisRealm(cell);
    case CellTestKind::IsSymbol:
        return cell->isSymbol() && &asSymbol(cell)->uid() == identifiersOf(m_state.instance).tryGetIdentifierConcurrently(test.symbolKeyNumber());
    case CellTestKind::SharingModeIs:
        return hasSharingMode(cell, test.sharingMode());
    case CellTestKind::IsNamespaceOf: {
        auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
        return moduleNamespace && moduleNamespace->hasNamespaceLayoutOf(test.module());
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static_assert(ConservativeRoots::findsStructuresOfFrameCells);

struct StructureTable::VariantMaps {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(VariantMaps);

    struct FiledVariant {
        uint32_t hash { 0 };
        StructureID structure;
    };

    static constexpr uint32_t keyMayBePrototype = 1u << 31;
    static constexpr uint32_t keyHasUnnamedExtraProperties = 1u << 30;
    static constexpr uint32_t keyHasSpareWordBit = 1u << 28;
    static constexpr unsigned recentIndexBits = 7;
    static constexpr unsigned requestedIndexBits = 12;

    static uint32_t hashOf(const Variant& variant)
    {
        uint32_t shapeAndFlags = variant.layout->shape;
        RELEASE_ASSERT(shapeAndFlags < keyHasSpareWordBit);
        if (variant.mayBePrototype)
            shapeAndFlags |= keyMayBePrototype;
        if (variant.hasExtraProperties && !variant.extraPropertiesStructure)
            shapeAndFlags |= keyHasUnnamedExtraProperties;
        if (variant.hasSpareWord)
            shapeAndFlags |= keyHasSpareWordBit;
        uint32_t keyOrder = variant.outOfLineKeyOrder ? 1 | computeHash(*variant.outOfLineKeyOrder) : 0;
        uint64_t prototypeAndExtraProperties = std::bit_cast<uintptr_t>(variant.prototype.getObject()) ^ std::rotl(static_cast<uint64_t>(std::bit_cast<uintptr_t>(variant.extraPropertiesStructure)), 32);
        uint32_t hash = WTF::intHash(static_cast<uint64_t>((static_cast<uint64_t>(shapeAndFlags) << 32 | keyOrder) ^ prototypeAndExtraProperties * 0x9e3779b97f4a7c15ull));
        return std::max(hash, 1u);
    }

    static bool describes(const Variant& variant, const CompiledStructure& structure)
    {
        return structure.hasOtherPrototype() == variant.hasOtherPrototype && describesWithOtherPrototype(variant, structure);
    }

    static bool describesWithOtherPrototype(const Variant& variant, const CompiledStructure& structure)
    {
        if (structure.layout().shape != variant.layout->shape || structure.storedPrototype() != variant.prototype)
            return false;
        if (structure.mayBePrototype() != variant.mayBePrototype || structure.reportsReplacements() != variant.reportsReplacements || structure.hasSpareWord() != variant.hasSpareWord)
            return false;
        if (structure.hasExtraProperties() != variant.hasExtraProperties || structure.extraPropertiesStructure() != variant.extraPropertiesStructure)
            return false;
        if (structure.heldExtraProperties() != variant.heldExtraProperties)
            return false;
        if (structure.hasOutOfLineKeyOrder() != variant.outOfLineKeyOrder.has_value())
            return false;
        if (variant.keysNamedByOrder || structure.hasKeyOrderOfOneObject())
            return false;
        if (!variant.outOfLineKeyOrder)
            return true;
        std::span<const uint16_t> held = structure.outOfLineKeyOrder();
        std::span<const uint16_t> asked = *variant.outOfLineKeyOrder;
        if (countOf(outOfLineOrderWordFor(asked.size())) && held.size() > asked.size())
            held = held.first(asked.size());
        return equalSpans(held, asked);
    }

    static CompiledStructure* decode(StructureID id) { return WTF::opaque(uncheckedDowncast<CompiledStructure>(id.decode())); }
    static unsigned recentIndex(const Variant& variant) { return ((variant.extraPropertiesStructure ? variant.extraPropertiesStructure->id().bits() : 0) * 0x9e3779b1u ^ variant.layout->shape * 0x85ebca6bu) >> (32 - recentIndexBits); }
    static unsigned requestedIndex(const Structure& extraPropertiesStructure) { return extraPropertiesStructure.id().bits() * 0x9e3779b1u >> (32 - requestedIndexBits); }

    CompiledStructure* findRecent(const Variant& variant) const
    {
        StructureID id = recentByExtraProperties[recentIndex(variant)];
        if (!id)
            return nullptr;
        CompiledStructure* recent = decode(id);
        return describes(variant, *recent) ? recent : nullptr;
    }

    void setRecent(const Variant& variant, const CompiledStructure& structure)
    {
        recentByExtraProperties[recentIndex(variant)] = structure.id();
    }

    size_t firstPlaceOf(uint32_t hash) const { return hash & (filed.size() - 1); }
    size_t placeBehind(size_t place) const { return (place + 1) & (filed.size() - 1); }

    CompiledStructure* find(const Variant& variant, uint32_t hash) const
    {
        if (filed.isEmpty())
            return nullptr;
        for (size_t place = firstPlaceOf(hash); filed[place].hash; place = placeBehind(place)) {
            if (filed[place].hash != hash || !filed[place].structure)
                continue;
            CompiledStructure* structure = decode(filed[place].structure);
            if (describes(variant, *structure))
                return structure;
        }
        return nullptr;
    }

    CompiledStructure* findPreferringPristine(const Variant& variant, uint32_t hash) const
    {
        if (filed.isEmpty())
            return nullptr;
        CompiledStructure* found = nullptr;
        for (size_t place = firstPlaceOf(hash); filed[place].hash; place = placeBehind(place)) {
            if (filed[place].hash != hash || !filed[place].structure)
                continue;
            CompiledStructure* structure = decode(filed[place].structure);
            if (!describesWithOtherPrototype(variant, *structure))
                continue;
            if (!structure->hasOtherPrototype())
                return structure;
            found = structure;
        }
        return found;
    }

    void file(const Variant& variant, uint32_t hash, StructureID structure)
    {
        if (4 * (numberOfUsedSlots + 1) > 3 * filed.size())
            makeRoom();
        filedSinceCollection.append({ hash, structure });
        FiledVariant* free = nullptr;
        size_t place = firstPlaceOf(hash);
        for (; filed[place].hash; place = placeBehind(place)) {
            FiledVariant& taken = filed[place];
            if (!taken.structure) {
                if (!free)
                    free = &taken;
                continue;
            }
            if (taken.hash == hash && describes(variant, *decode(taken.structure))) {
                taken.structure = structure;
                return;
            }
        }
        if (!free) {
            free = &filed[place];
            ++numberOfUsedSlots;
        }
        *free = { hash, structure };
    }

    void makeRoom()
    {
        constexpr size_t fewestPlaces = 64;
        size_t numberAlive = std::ranges::count_if(filed, [](const FiledVariant& variant) {
            return !!variant.structure;
        });
        size_t size = std::max(fewestPlaces, 2 * numberAlive >= filed.size() ? 2 * filed.size() : filed.size());
        Vector<FiledVariant> previous = std::exchange(filed, Vector<FiledVariant>(FillWith { }, size, FiledVariant { }));
        numberOfUsedSlots = numberAlive;
        for (const FiledVariant& variant : previous) {
            if (!variant.structure)
                continue;
            size_t place = firstPlaceOf(variant.hash);
            while (filed[place].hash)
                place = placeBehind(place);
            filed[place] = variant;
        }
    }

    void forgetDead(VM& vm, CollectionScope scope)
    {
        auto isDead = [&](StructureID id) {
            return !vm.heap.isMarked(id.decode());
        };
        if (scope == CollectionScope::Eden) {
            for (const FiledVariant& young : filedSinceCollection) {
                if (!young.hash || !isDead(young.structure))
                    continue;
                for (size_t place = firstPlaceOf(young.hash); filed[place].hash; place = placeBehind(place)) {
                    if (filed[place].structure == young.structure) {
                        filed[place].structure = { };
                        break;
                    }
                }
            }
        } else {
            for (FiledVariant& variant : filed) {
                if (variant.structure && isDead(variant.structure))
                    variant.structure = { };
            }
        }
        filedSinceCollection.shrink(0);
        usualOfLivingPrototypes.removeAllMatching(isDead);
    }

    template<typename Visitor>
    void visitRoots(Visitor& visitor)
    {
        if (visitor.collectionScope() == CollectionScope::Eden)
            return;
        auto visit = [&](StructureID id) {
            CompiledStructure* variant = decode(id);
            if (visitor.isMarked(variant) || variant->hasOutOfLineKeyOrder())
                return;
            JSObject* prototype = variant->storedPrototype().getObject();
            Structure* named = variant->extraPropertiesStructure();
            if ((!prototype || visitor.isMarked(prototype)) && (!named || visitor.isMarked(named)))
                visitor.appendUnbarriered(variant);
        };
        for (const FiledVariant& variant : filed) {
            if (variant.structure)
                visit(variant.structure);
        }
        for (StructureID id : usualOfLivingPrototypes)
            visit(id);
    }

    Vector<FiledVariant> filed;
    size_t numberOfUsedSlots { 0 };
    Vector<StructureID> usualOfLivingPrototypes;
    Vector<FiledVariant> filedSinceCollection;
    std::array<StructureID, 1u << recentIndexBits> recentByExtraProperties { };
    WTF::BitSet<1u << requestedIndexBits> requestedExtraPropertiesStructures;
};

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
    , m_variants(makeUnique<VariantMaps>())
    , m_writtenStructures(asBytes(usualStructureIDs()), structuresRegionShift)
    , m_writtenRecords(allocateAllocationSiteRecords(state), recordsRegionShift)
{
    m_environmentShapes.tables = &m_tables;
    m_shapesThatLiveWhilePrototypesLive.ensureSize(m_tables.firstEnvironmentShape);
    m_weakShapes.ensureSize(m_tables.firstEnvironmentShape);
    m_allocationsThatMadeObjects.ensureSize(m_tables.allocations().size());
    m_shapesWithGrownCells.ensureSize(m_tables.firstEnvironmentShape);
    static_assert(OBJECT_OFFSETOF(AllocatorWithoutRoom, freeList) == LocalAllocator::offsetOfFreeList());
    *state.instance.objects.emptyAllocationSite() = emptyAllocationSiteRecord();
    state.caches->add(CacheKind::StrongRoots, "structures of layouts"_s, *this, RootVolatility::GreyedByExecution);
    state.caches->add(CacheKind::WeakByKey, "structures of layouts that have ended"_s, *this);
    state.caches->add(CacheKind::StrongRoots, "variants of the structures of layouts"_s, *m_variants);
    state.caches->addClearedEachCollection("recent structures by extra properties"_s, asWritableBytes(std::span { m_variants->recentByExtraProperties }));
    state.caches->addClearedEachCollection("recent structures with a spare word"_s, asWritableBytes(std::span { m_recentGrownStructures }));
    state.instance.objects.prototypeTransitions = state.tableArena.allocate<PrototypeTransition>(2 * numberOfPrototypeTransitions);
    state.caches->addClearedEachCollection("prototype transitions"_s, asWritableBytes(std::span { state.instance.objects.prototypeTransitions, 2 * numberOfPrototypeTransitions }));
    state.instance.objects.copiesWithNames = state.tableArena.allocate<uint8_t>(sizeOfCopiesWithNames);
    state.caches->addClearedEachCollection("copies with names"_s, std::span { state.instance.objects.copiesWithNames, sizeOfCopiesWithNames });
}

StructureTable::~StructureTable() = default;

std::span<AllocationSiteRecord> StructureTable::allocationSiteRecords() const
{
    size_t numberOfAllocations = m_tables.allocations().size();
    return { m_state.instance.objects.emptyAllocationSite() - numberOfAllocations, numberOfAllocations + 1 };
}

template<typename Appender>
ALWAYS_INLINE void StructureTable::visitUsualStructures(Appender& appender, size_t begin, size_t end)
{
    std::span<uint32_t> structureIDs = usualStructureIDs();
    RememberedSlotSet::forEachEntryInRegion(structureIDs, begin, end, [&](uint32_t& structureID) ALWAYS_INLINE_LAMBDA {
        size_t shape = &structureID - structureIDs.data();
        if (structureID && !m_shapesThatLiveWhilePrototypesLive.quickGet(shape) && !m_weakShapes.quickGet(shape))
            appender.appendUnbarriered(std::bit_cast<StructureID>(structureID).decode());
    });
}

ALWAYS_INLINE_INTO_OTHER_FILES bool StructureTable::livesWhilePrototypeLives(const AllocationSiteRecord& record) const
{
    uint32_t shape = shapeOf(shapeWordWithoutMark(static_cast<uint32_t>(record.orderAndShapeWords >> 32)));
    return shape < m_tables.firstEnvironmentShape && m_shapesThatLiveWhilePrototypesLive.quickGet(shape);
}

template<typename Appender>
ALWAYS_INLINE void StructureTable::visitRecords(Appender& appender, size_t begin, size_t end)
{
    RememberedSlotSet::forEachEntryInRegion(allocationSiteRecords(), begin, end, [&](const AllocationSiteRecord& record) ALWAYS_INLINE_LAMBDA {
        if (uint32_t structureID = static_cast<uint32_t>(record.header); structureID && !livesWhilePrototypeLives(record))
            appender.appendUnbarriered(std::bit_cast<StructureID>(structureID).decode());
    });
}

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
    for (const VariantMaps::FiledVariant& filed : m_variants->filedSinceCollection) {
        CompiledStructure* variant = VariantMaps::decode(filed.structure);
        if (variant->hasOutOfLineKeyOrder())
            continue;
        JSObject* prototype = variant->storedPrototype().getObject();
        if (!prototype || visitor.isMarked(prototype))
            visitor.appendUnbarriered(variant);
    }
}

template void StructureTable::visitRoots(AbstractSlotVisitor&);
template void StructureTable::visitRoots(SlotVisitor&);

size_t StructureTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    {
        Locker locker { m_state.caches->lock() };
        m_variants->forgetDead(vm, scope);
    }
    disarmDeadGrownCells(vm);
    forgetStructuresThatDied(vm, scope);
    return 0;
}

void StructureTable::forgetStructuresThatDied(VM& vm, CollectionScope scope)
{
    auto isDead = [&](uint32_t structureID) {
        return structureID && !vm.heap.isMarked(std::bit_cast<StructureID>(structureID).decode());
    };
    bool isEden = scope == CollectionScope::Eden;
    auto forgetDead = [&](std::span<const uint32_t> shapes) {
        for (uint32_t shape : shapes) {
            if (isDead(m_structures[shape]))
                m_structures[shape] = 0;
        }
    };
    if (isEden)
        forgetDead(m_shapesGivenStructuresSinceCollection);
    else {
        forgetDead(m_listOfShapesThatLiveWhilePrototypesLive);
        forgetDead(m_listOfWeakShapes);
    }
    for (AllocationSiteRecord* record : isEden ? m_recordsGivenStructuresSinceCollection : m_listOfRecordsThatLiveWhilePrototypesLive) {
        if (isDead(static_cast<uint32_t>(record->header)))
            *record = emptyAllocationSiteRecord();
    }
    m_shapesGivenStructuresSinceCollection.shrink(0);
    m_recordsGivenStructuresSinceCollection.shrink(0);
}

bool StructureTable::noteWhetherLivesWhilePrototypeLives(const LayoutData& layout)
{
    if (!livesWhilePrototypeLives(layout))
        return false;
    if (!m_shapesThatLiveWhilePrototypesLive.quickSet(layout.shape))
        m_listOfShapesThatLiveWhilePrototypesLive.append(layout.shape);
    return true;
}

void StructureTable::didNameStructureThatLivesWhilePrototypeLives(AllocationSiteRecord& record)
{
    size_t number = m_state.instance.objects.emptyAllocationSite() - &record;
    if (!m_recordsThatLiveWhilePrototypesLive.set(number))
        m_listOfRecordsThatLiveWhilePrototypesLive.append(&record);
    m_recordsGivenStructuresSinceCollection.append(&record);
}

enum class AllocatedCellKind : uint8_t { Object, Closure, Environment };

static AllocatedCellKind allocatedCellKindOf(const ProgramTables& tables, uint32_t shape)
{
    if (shape < tables.firstEnvironmentShape)
        return AllocatedCellKind::Object;
    switch (tables.environmentForShape(shape).kind) {
    case LayoutKind::Closure:
        return AllocatedCellKind::Closure;
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
        return AllocatedCellKind::Environment;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Structure* StructureTable::structure(VM& vm, uint32_t shape)
{
    switch (allocatedCellKindOf(m_tables, shape)) {
    case AllocatedCellKind::Object:
        return objectStructure(vm, shape);
    case AllocatedCellKind::Closure:
        return m_state.objects->closures.closureStructure(vm, 0);
    case AllocatedCellKind::Environment:
        return m_environmentShapes.structureFor(vm, m_state.instance, shape);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE_INTO_OTHER_FILES CompiledStructure* StructureTable::usualStructureIfExists(uint32_t shape) const
{
    RELEASE_ASSERT(shape < m_tables.firstEnvironmentShape);
    uint32_t known = m_structures[shape];
    return known ? VariantMaps::decode(std::bit_cast<StructureID>(known)) : nullptr;
}

ALWAYS_INLINE_INTO_OTHER_FILES StructureTable::Variant StructureTable::usualVariantOf(VM& vm, const LayoutData& layout)
{
    JSGlobalObject* realm = m_state.instance.globalObject;
    Variant usual { .realm = realm, .prototype = realm->objectPrototype(), .layout = &layout };
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

ALWAYS_INLINE_INTO_OTHER_FILES CompiledStructure* StructureTable::create(VM& vm, const LayoutData& layout, const Variant& variant)
{
    JSGlobalObject* realm = m_state.instance.globalObject;
    CompiledStructure*& templateStructure = m_templateStructures[variant.hasOtherPrototype];
    if (!templateStructure) {
        templateStructure = Object::createStructure(vm, { .realm = realm, .prototype = jsNull(), .layout = &layout, .hasOtherPrototype = variant.hasOtherPrototype });
        m_state.immortalCells->add(templateStructure);
    }
    if (JSObject* object = variant.prototype.getObject(); object && !object->structure()->mayBePrototype())
        object->didBecomePrototype(vm);
    countEvent(vm, RuntimeCounterID::CompiledStructuresCreated);
    ++m_numberOfStructuresMade;
    RELEASE_ASSERT(!variant.hasSpareWord || !layout.hasExtraPropertiesSlot);
    RELEASE_ASSERT(!variant.hasExtraProperties || !(layout.hasExtraPropertiesSlot || variant.hasSpareWord) == !!variant.heldExtraProperties);
    RELEASE_ASSERT(!variant.heldExtraProperties || (variant.hasExtraProperties && !variant.extraPropertiesStructure));
    Variant parameters = variant;
    parameters.realm = realm;
    CompiledStructure* structure = CompiledStructure::createForObject(vm, *templateStructure, parameters);
    bool hasNonConfigurableField = variant.hasExtraProperties && std::ranges::any_of(layout.fields(), [](const FieldData& field) {
        return field.attributes() & PropertyAttribute::DontDelete;
    });
    if (variant.hasExtraProperties && !hasNonConfigurableField)
        structure->setHasOnlyConfigurableFields();
    return structure;
}

ALWAYS_INLINE_INTO_OTHER_FILES CompiledStructure* StructureTable::createUsual(VM& vm, const LayoutData& layout, const Variant& usual)
{
    CompiledStructure* created = create(vm, layout, usual);
    uint32_t& known = m_structures[layout.shape];
    if (m_weakShapes.quickGet(layout.shape)) {
        Locker locker { m_state.caches->lock() };
        m_shapesGivenStructuresSinceCollection.append(layout.shape);
    } else if (noteWhetherLivesWhilePrototypeLives(layout)) {
        Locker locker { m_state.caches->lock() };
        m_shapesGivenStructuresSinceCollection.append(layout.shape);
        m_variants->filedSinceCollection.append({ 0, created->id() });
        m_variants->usualOfLivingPrototypes.append(created->id());
    }
    known = created->id().bits();
    m_writtenStructures.record(&known);
    if (!m_shapeHasStructure.set(layout.shape))
        m_shapesWithStructure.append(layout.shape);
    if (usual.hasOtherPrototype)
        m_state.objects->classes.didChangeInstancePrototype(vm, layout, usual.prototype);
    return created;
}

ALWAYS_INLINE_INTO_OTHER_FILES CompiledStructure* StructureTable::usualStructure(VM& vm, uint32_t shape)
{
    holdUsualStructure(shape);
    if (CompiledStructure* known = usualStructureIfExists(shape))
        return known;
    RELEASE_ASSERT(m_tables.hasLayoutForShape(shape));
    const LayoutData& layout = m_tables.layoutForShape(shape);
    return createUsual(vm, layout, usualVariantOf(vm, layout));
}

void StructureTable::holdUsualStructure(uint32_t shape)
{
    if (!m_weakShapes.quickGet(shape)) [[likely]]
        return;
    m_weakShapes.quickClear(shape);
    if (m_structures[shape])
        m_writtenStructures.record(&m_structures[shape]);
}

CompiledStructure* StructureTable::structureForFirstObject(VM& vm, uint32_t shape)
{
    if (CompiledStructure* known = usualStructureIfExists(shape))
        return known;
    const LayoutData& layout = m_tables.layoutForShape(shape);
    if (!m_weakShapes.quickSet(shape))
        m_listOfWeakShapes.append(shape);
    return createUsual(vm, layout, usualVariantOf(vm, layout));
}

CompiledStructure* StructureTable::objectStructure(VM& vm, uint32_t shape)
{
    return usualStructure(vm, shape);
}

CompiledStructure* StructureTable::objectStructure(VM& vm, uint32_t shape, JSValue prototype)
{
    RELEASE_ASSERT(m_tables.hasLayoutForShape(shape));
    return findOrCreate(vm, Variant { .prototype = prototype, .layout = &m_tables.layoutForShape(shape) });
}

StructureTable::Variant StructureTable::variantOf(const CompiledStructure& structure)
{
    Variant variant {
        .prototype = structure.storedPrototype(),
        .layout = &structure.layout(),
        .hasOtherPrototype = structure.hasOtherPrototype(),
        .mayBePrototype = structure.mayBePrototype(),
        .reportsReplacements = structure.reportsReplacements(),
        .hasSpareWord = structure.hasSpareWord(),
    };
    if (structure.hasExtraProperties()) {
        variant.hasExtraProperties = true;
        variant.extraPropertiesStructure = structure.extraPropertiesStructure();
        variant.heldExtraProperties = structure.heldExtraProperties();
    }
    if (structure.hasOutOfLineKeyOrder())
        variant.outOfLineKeyOrder = structure.outOfLineKeyOrder();
    if (structure.hasKeyOrderOfOneObject())
        variant.keysNamedByOrder = structure.keysNamedByOrder();
    return variant;
}

StructureTable::Variant StructureTable::variantWithKeyOrderOf(const CompiledStructure& structure)
{
    structure.endOutOfLineKeyOrder();
    return variantOf(structure);
}

CompiledStructure* StructureTable::find(const Variant& variant) const
{
    if (!isListed(variant))
        return nullptr;
    if (CompiledStructure* usual = usualStructureIfExists(variant.layout->shape); usual && VariantMaps::describes(variant, *usual))
        return usual;
    CompiledStructure* found = m_variants->find(variant, VariantMaps::hashOf(variant));
    if (!found)
        return nullptr;
    if (variant.outOfLineKeyOrder)
        found->didFindKeyOrderAgain();
    return found;
}

CompiledStructure* StructureTable::findOrCreate(VM& vm, const Variant& requested)
{
    // outOfLineKeyOrder may point into a Structure that nothing else keeps alive.
    // A collection that is put off runs at the next poll or allocation, not when this returns: this frame and the caller's hold Variants,
    // and an empty std::optional leaves its payload unwritten, so the conservative scan would keep whatever stale pointers lie there alive.
    DeferGCForAWhile deferGC(vm);
    const LayoutData& layout = *requested.layout;
    holdUsualStructure(layout.shape);
    CompiledStructure* usualStructure = usualStructureIfExists(layout.shape);
    RELEASE_ASSERT(m_tables.hasLayoutForShape(layout.shape));
    Variant usual = usualStructure ? variantOf(*usualStructure) : usualVariantOf(vm, layout);
    Variant variant = requested;
    bool isListedVariant = isListed(variant);
    bool mayBeFiled = (usual.prototype == variant.prototype || !m_state.objects->classes.takeWordThatNothingHasAskedBelow(variant.prototype)) && isListedVariant;
    uint32_t hash = isListedVariant ? VariantMaps::hashOf(variant) : 0;
    CompiledStructure* existing = nullptr;
    if (!variant.hasOtherPrototype && !usual.hasOtherPrototype && usual.prototype != variant.prototype) {
        if (mayBeFiled)
            existing = m_variants->findPreferringPristine(variant, hash);
        JSObject* firstAbove = variant.prototype.getObject();
        if (!existing)
            variant.hasOtherPrototype = !firstAbove || !m_state.objects->classes.takesObjectsLaidBelowPrototype(vm, layout, firstAbove);
    } else {
        variant.hasOtherPrototype |= usual.hasOtherPrototype;
        if (mayBeFiled)
            existing = usualStructure && VariantMaps::describes(variant, *usualStructure) ? usualStructure : m_variants->find(variant, hash);
    }
    if (existing) {
        if (variant.outOfLineKeyOrder && existing != usualStructure)
            existing->didFindKeyOrderAgain();
        return existing;
    }
    ASSERT(layout.shape == usual.layout->shape);
    if (!variant.mayBePrototype && !variant.hasExtraProperties && !variant.hasSpareWord && !variant.outOfLineKeyOrder && variant.prototype == usual.prototype && variant.hasOtherPrototype == usual.hasOtherPrototype)
        return createUsual(vm, layout, usual);
    CompiledStructure* created = create(vm, layout, variant);
    if (isListedVariant) {
        Locker locker { m_state.caches->lock() };
        m_variants->file(variant, hash, created->id());
    }
    if (variant.hasOtherPrototype && layout.isClass())
        m_state.objects->classes.didChangeInstancePrototype(vm, layout, variant.prototype);
    return created;
}

CompiledStructure* StructureTable::withPrototype(VM& vm, const CompiledStructure& current, JSValue prototype)
{
    EncodedJSValue encodedPrototype = JSValue::encode(prototype);
    uint32_t before = current.id().bits();
    std::span transitions { m_state.instance.objects.prototypeTransitions, 2 * numberOfPrototypeTransitions };
    PrototypeTransition& recent = transitions[indexOfPrototypeTransition(before, encodedPrototype)];
    if (recent.before == before && recent.prototype == encodedPrototype)
        return VariantMaps::decode(std::bit_cast<StructureID>(recent.after));
    PrototypeTransition& recentBelowLaidObjects = transitions[numberOfPrototypeTransitions + indexOfPrototypeTransition(before, encodedPrototype)];
    if (recentBelowLaidObjects.before == before && recentBelowLaidObjects.prototype == encodedPrototype)
        return VariantMaps::decode(std::bit_cast<StructureID>(recentBelowLaidObjects.after));

    Variant variant = variantWithKeyOrderOf(current);
    variant.prototype = prototype;
    CompiledStructure* structure = findOrCreate(vm, variant);
    constexpr uint32_t pristineBitInBlob = IsPristine << 16;
    if (structure->isPristine() && structure->typeInfoBlob() == (current.typeInfoBlob() | pristineBitInBlob) && !current.mayBePrototype() && !belongsToOneObject(variant))
        recent = { encodedPrototype, before, structure->id().bits() };
    else if (!structure->isPristine() && structure->typeInfoBlob() == current.typeInfoBlob() && !current.mayBePrototype() && !belongsToOneObject(variant))
        recentBelowLaidObjects = { encodedPrototype, before, structure->id().bits() };
    return structure;
}

CompiledStructure* StructureTable::asPrototype(VM& vm, const CompiledStructure& current)
{
    Variant variant = variantWithKeyOrderOf(current);
    variant.mayBePrototype = true;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withReportedReplacements(VM& vm, const CompiledStructure& current)
{
    ASSERT(current.mayBePrototype());
    Variant variant = variantWithKeyOrderOf(current);
    variant.reportsReplacements = true;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withExtraProperties(VM& vm, const CompiledStructure& current, Structure* extraPropertiesStructureOrNull)
{
    Variant variant = variantWithKeyOrderOf(current);
    Structure* named = extraPropertiesStructureOrNull;
    if (!named || !named->propertyAccessesAreCacheable() || !current.extraPropertiesSlot()) {
        if (variant.hasExtraProperties && !variant.extraPropertiesStructure)
            return const_cast<CompiledStructure*>(&current);
        variant.hasExtraProperties = true;
        variant.extraPropertiesStructure = nullptr;
        if (!current.extraPropertiesSlot())
            return findOrCreate(vm, variant);
        if (CompiledStructure* recent = m_variants->findRecent(variant))
            return recent;
        CompiledStructure* structure = findOrCreate(vm, variant);
        m_variants->setRecent(variant, *structure);
        return structure;
    }
    variant.hasExtraProperties = true;
    variant.extraPropertiesStructure = named;
    if (CompiledStructure* recent = m_variants->findRecent(variant))
        return recent;
    CompiledStructure* structure = findOrCreate(vm, variant);
    m_variants->setRecent(variant, *structure);
    m_variants->requestedExtraPropertiesStructures.set(VariantMaps::requestedIndex(*named));
    return structure;
}

CompiledStructure* StructureTable::existingWithExtraProperties(const CompiledStructure& current, Structure& extraPropertiesStructure)
{
    if (!current.extraPropertiesSlot() || !m_variants->requestedExtraPropertiesStructures.get(VariantMaps::requestedIndex(extraPropertiesStructure)))
        return nullptr;
    Variant variant = variantWithKeyOrderOf(current);
    variant.hasExtraProperties = true;
    variant.extraPropertiesStructure = &extraPropertiesStructure;
    if (CompiledStructure* recent = m_variants->findRecent(variant))
        return recent;
    CompiledStructure* existing = find(variant);
    if (existing)
        m_variants->setRecent(variant, *existing);
    return existing;
}

CompiledStructure* StructureTable::holdingExtraProperties(VM& vm, const CompiledStructure& current, JSObject* extraProperties)
{
    ASSERT(!current.extraPropertiesSlot() && !current.hasExtraProperties());
    Variant variant = variantWithKeyOrderOf(current);
    variant.hasExtraProperties = true;
    variant.heldExtraProperties = extraProperties;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withoutHeldStructure(VM& vm, const CompiledStructure& current)
{
    ASSERT(current.heldStructure());
    return findOrCreate(vm, variantWithKeyOrderOf(current));
}

static uint32_t sizeWithSpareWord(const LayoutData& layout)
{
    return WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(layout.size + static_cast<uint32_t>(sizeof(uint64_t)));
}

void StructureTable::growFutureCells(const LayoutData& layout)
{
    if (layout.kind() != LayoutKind::Instance || m_shapesWithGrownCells.quickSet(layout.shape))
        return;
    for (AllocationSiteRecord& record : std::span { m_state.instance.objects.emptyAllocationSite() - m_numberOfAllocationSiteRecords, m_numberOfAllocationSiteRecords }) {
        if (record.header && shapeOf(shapeWordWithoutMark(static_cast<uint32_t>(record.orderAndShapeWords >> 32))) == layout.shape)
            record.allocator = grownCellAllocator(record);
    }
}

void* StructureTable::grownCellAllocator(AllocationSiteRecord& record)
{
    static_assert(OBJECT_OFFSETOF(GrownCells, freeList) == LocalAllocator::offsetOfFreeList());
    std::unique_ptr<GrownCells>& grown = m_grownCells.add(&record, nullptr).iterator->value;
    if (!grown)
        grown = makeUnique<GrownCells>();
    grown->allocatorAndMark = 0;
    return grown.get();
}

void StructureTable::disarmDeadGrownCells(VM& vm)
{
    for (auto& grown : m_grownCells.values()) {
        if (grown->allocatorAndMark && !vm.heap.isMarked(std::bit_cast<StructureID>(static_cast<uint32_t>(grown->record.header)).decode()))
            grown->allocatorAndMark = 0;
    }
}

CompiledStructure* StructureTable::withSpareWord(VM& vm, const CompiledStructure& current)
{
    ASSERT(!current.extraPropertiesSlot() && !current.hasExtraProperties());
    GrownStructure& recent = m_recentGrownStructures[grownStructureSlot(current.id())];
    if (recent.before == current.id())
        return VariantMaps::decode(recent.after);
    Variant variant = variantWithKeyOrderOf(current);
    variant.hasSpareWord = true;
    CompiledStructure* grown = findOrCreate(vm, variant);
    recent = { current.id(), grown->id() };
    return grown;
}

CompiledStructure* StructureTable::withOutOfLineKeyOrder(VM& vm, const CompiledStructure& current, std::span<const uint16_t> entries, HeldKeyOrder heldKeyOrder)
{
    Variant variant = variantOf(current);
    variant.outOfLineKeyOrder = entries;
    variant.keysNamedByOrder = std::nullopt;
    variant.isFoundByKeyOrder = heldKeyOrder == HeldKeyOrder::IsFoundByOthers;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withKeyOrderOfOneObject(VM& vm, const CompiledStructure& current, std::span<const uint16_t> entries, std::span<const RefPtr<UniquedStringImpl>> keysNamedByOrder)
{
    Variant variant = variantOf(current);
    variant.outOfLineKeyOrder = entries;
    variant.keysNamedByOrder = keysNamedByOrder;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withNumberedKeyOrder(VM& vm, const CompiledStructure& current)
{
    Variant variant = variantOf(current);
    variant.outOfLineKeyOrder = std::nullopt;
    variant.keysNamedByOrder = std::nullopt;
    return findOrCreate(vm, variant);
}

const StructureTable::StepOfKeyOrder* StructureTable::findStepOfKeyOrder(StructureID before, uint32_t orderAndCountBefore, uint16_t field, KindOfStep kind) const
{
    if (!m_stepsOfKeyOrders)
        return nullptr;
    uint32_t fieldAndKind = field << 2 | static_cast<uint32_t>(kind);
    size_t newer = placeOfStep(before, orderAndCountBefore, fieldAndKind);
    for (size_t place : { newer, newer + 1 }) {
        const StepOfKeyOrder& step = m_stepsOfKeyOrders[place];
        if (step.before == before && step.orderAndCountBefore == orderAndCountBefore && step.fieldAndKind == fieldAndKind)
            return &step;
    }
    return nullptr;
}

void StructureTable::rememberStepOfKeyOrder(StructureID before, uint32_t orderAndCountBefore, uint16_t field, KindOfStep kind, StructureID after, uint32_t orderAndCountAfter)
{
    if (!m_stepsOfKeyOrders) {
        m_stepsOfKeyOrders = static_cast<StepOfKeyOrder*>(m_state.pages.allocate(sizeof(StepOfKeyOrder) << logMaxSteps));
        m_state.caches->addClearedEachCollection("steps of key orders"_s, asWritableBytes(std::span { m_stepsOfKeyOrders, size_t { 1 } << logMaxSteps }));
    }
    putKeyOrderStep({ before, orderAndCountBefore, field << 2 | static_cast<uint32_t>(kind), after, orderAndCountAfter });
    if (++m_stepsPutSinceGrowth > 1u << m_logNumberOfSteps >> 1 && m_logNumberOfSteps < logMaxSteps) [[unlikely]]
        growKeyOrderSteps();
}

void StructureTable::putKeyOrderStep(const StepOfKeyOrder& step)
{
    size_t newer = placeOfStep(step.before, step.orderAndCountBefore, step.fieldAndKind);
    m_stepsOfKeyOrders[newer + 1] = m_stepsOfKeyOrders[newer];
    m_stepsOfKeyOrders[newer] = step;
}

NEVER_INLINE void StructureTable::growKeyOrderSteps()
{
    std::span used { m_stepsOfKeyOrders, size_t { 1 } << m_logNumberOfSteps };
    Vector<StepOfKeyOrder> previous(used);
    std::ranges::fill(used, StepOfKeyOrder { });
    m_logNumberOfSteps += 2;
    static_assert(!((logMaxSteps - logInitialSteps) % 2));
    m_stepsPutSinceGrowth = 0;
    for (size_t index = previous.size(); index--;) {
        if (previous[index].before)
            putKeyOrderStep(previous[index]);
    }
}

JSCellButterfly* StructureTable::findKeysOfOrderWithoutNumber(StructureID structure, uint32_t count, KeyListKind kind) const
{
    if (!m_keysOfOrdersWithoutNumber)
        return nullptr;
    uint32_t countAndKind = count << 2 | static_cast<uint32_t>(kind);
    const KeysOfOrderWithoutNumber& kept = m_keysOfOrdersWithoutNumber[placeOfKeys(structure, countAndKind)];
    return kept.structure == structure && kept.countAndKind == countAndKind ? kept.list : nullptr;
}

void StructureTable::rememberKeysOfOrderWithoutNumber(StructureID structure, uint32_t count, KeyListKind kind, JSCellButterfly* list)
{
    if (!m_keysOfOrdersWithoutNumber) {
        m_keysOfOrdersWithoutNumber = static_cast<KeysOfOrderWithoutNumber*>(m_state.pages.allocate(numberOfKeyListsRemembered * sizeof(KeysOfOrderWithoutNumber)));
        m_state.caches->addClearedEachCollection("keys of orders without a number"_s, asWritableBytes(std::span { m_keysOfOrdersWithoutNumber, numberOfKeyListsRemembered }));
    }
    uint32_t countAndKind = count << 2 | static_cast<uint32_t>(kind);
    m_keysOfOrdersWithoutNumber[placeOfKeys(structure, countAndKind)] = { structure, countAndKind, list };
}

static uint64_t orderAndShapeWordsFor(const AllocationData& allocation)
{
    uint32_t shapeWord = shapeWordOf(allocation.shape);
    return static_cast<uint64_t>(allocation.isBornUnfinished ? shapeWord & ~shapeWordIsUnmarked : shapeWord) << 32 | allocation.orderWord();
}

static bool fillsSameRecord(const ProgramTables& tables, uint32_t first, uint32_t second)
{
    const AllocationData a = tables.allocations()[first];
    const AllocationData b = tables.allocations()[second];
    return a.shape == b.shape && a.orderWord() == b.orderWord() && a.size() == b.size() && a.isBornUnfinished == b.isBornUnfinished && tables.constructorOfAllocation(first) == tables.constructorOfAllocation(second);
}

AllocationSiteRecord* StructureTable::recordOfSameAllocation(uint32_t number) const
{
    const AllocationTable allocations = m_tables.allocations();
    const ObjectWords& words = m_state.instance.objects;
    const AllocationData allocation = allocations[number];
    auto recordOf = [&](uint32_t other) -> AllocationSiteRecord* {
        AllocationSiteRecord* record = words.allocationSite(other);
        return record && fillsSameRecord(m_tables, other, number) ? record : nullptr;
    };
    for (uint32_t other = number; other && allocations[other - 1].shape == allocation.shape; --other) {
        if (AllocationSiteRecord* record = recordOf(other - 1))
            return record;
    }
    for (uint32_t other = number + 1; other < allocations.size() && allocations[other].shape == allocation.shape; ++other) {
        if (AllocationSiteRecord* record = recordOf(other))
            return record;
    }
    return nullptr;
}

ALWAYS_INLINE_INTO_OTHER_FILES AllocationSiteRecord& StructureTable::ensureRecord(uint32_t number, uint32_t shape)
{
    ObjectWords& words = m_state.instance.objects;
    if (AllocationSiteRecord* record = words.allocationSite(number))
        return *record;
    bool isFirstOfShape = shape < m_tables.firstEnvironmentShape && !m_structures[shape];
    AllocationSiteRecord* record = isFirstOfShape ? nullptr : recordOfSameAllocation(number);
    if (!record) {
        record = words.emptyAllocationSite() - ++m_numberOfAllocationSiteRecords;
        *record = emptyAllocationSiteRecord();
    }
    words.distancesToAllocationSites[number] = safeCast<int32_t>(reinterpret_cast<char*>(record) - reinterpret_cast<char*>(words.emptyAllocationSite()));
    return *record;
}

AllocationSiteRecord& StructureTable::ensureRecord(uint32_t number)
{
    return ensureRecord(number, m_tables.allocations().checkedAt(number).shape);
}

AllocationSiteRecord& StructureTable::fillAllocationSite(VM& vm, uint32_t number)
{
    const AllocationData allocation = m_tables.allocations().checkedAt(number);
    AllocationSiteRecord& record = ensureRecord(number, allocation.shape);
    if (record.header)
        return record;
    AllocatedCellKind kind = allocatedCellKindOf(m_tables, allocation.shape);
    Structure* structure = nullptr;
    if (allocation.isConstructed) {
        ASSERT(kind == AllocatedCellKind::Object);
        ObjectModel& objects = *m_state.objects;
        FunctionID function(m_tables.constructorOfAllocation(number));
        JSCell* constructor = objects.closures.closureIfCreated(function);
        if (objects.classes.isMarkOfEvaluationThatDied(constructor))
            constructor = nullptr;
        if (!constructor && objects.closures.hasSingletonClosure(function)) {
            const LayoutData& constructedClass = *objects.classes.classConstructedBy(function);
            if (!constructedClass.createsPrototypeBeforeClassObject || !objects.classes.classPrototypeOfInstances(vm, constructedClass))
                constructor = objects.closures.closure(vm, function);
        }
        if (constructor)
            structure = objectStructure(vm, allocation.shape, objects.classes.prototypeForInstancesOf(vm, asObject(constructor)));
    }
    if (!structure)
        structure = kind == AllocatedCellKind::Object ? usualStructure(vm, allocation.shape) : this->structure(vm, allocation.shape);
    if (kind == AllocatedCellKind::Object)
        m_state.objects->classes.holdMethodsStoredFirst(vm, m_tables.layoutForShape(allocation.shape), structure->storedPrototype());
    CompleteSubspace& space = kind == AllocatedCellKind::Object ? vm.cellSpace() : *vm.heap.aotClosureSpace<SubspaceAccess::OnMainThread>();
    void* allocator = space.allocatorFor(allocation.size(), AllocatorForMode::EnsureAllocator).localAllocator();
    record = AllocationSiteRecord {
        .header = kind == AllocatedCellKind::Environment ? Environment::headerFor(structure, allocation.shape) : cellHeaderFor(structure),
        .orderAndShapeWords = orderAndShapeWordsFor(allocation),
        .allocator = kind == AllocatedCellKind::Object && m_shapesWithGrownCells.quickGet(allocation.shape) ? grownCellAllocator(record) : allocator ? allocator : &m_allocatorWithoutRoom,
        .prototype = kind == AllocatedCellKind::Object && !m_tables.layoutForShape(allocation.shape).impossibleMutations().contains(ObjectMutation::ConstructDirectly) ? static_cast<uint64_t>(JSValue::encode(structure->storedPrototype())) : 0,
    };
    m_writtenRecords.record(&record);
    if (kind == AllocatedCellKind::Object && noteWhetherLivesWhilePrototypeLives(m_tables.layoutForShape(allocation.shape)))
        didNameStructureThatLivesWhilePrototypeLives(record);
    return record;
}

void StructureTable::predictStructure(VM&, uint32_t number, CompiledStructure* structure)
{
    const AllocationData allocation = m_tables.allocations()[number];
    if (!allocation.isConstructed || structure->layout().shape != allocation.shape || structure->layout().evaluation() == EvaluationState::ProvenOnce || structure->hasSpareWord())
        return;
    CompiledStructure* usual = usualStructureIfExists(allocation.shape);
    AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!usual || !record || !usual->hasOtherPrototype() || structureIDOf(number) != usual->id())
        return;
    record->header = cellHeaderFor(structure);
    record->prototype = static_cast<uint64_t>(JSValue::encode(structure->storedPrototype()));
    m_writtenRecords.record(record);
    if (noteWhetherLivesWhilePrototypeLives(structure->layout()))
        didNameStructureThatLivesWhilePrototypeLives(*record);
}

std::optional<StructureID> StructureTable::structureIDOf(uint32_t number) const
{
    const AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!record || !record->header)
        return std::nullopt;
    return std::bit_cast<StructureID>(static_cast<uint32_t>(record->header));
}

void StructureTable::forgetStructuresOf(const LayoutData& classLayout)
{
    m_state.objects->classes.forgetInstancesBelowPrototypes();
    const ClassTable& classes = m_state.objects->classes;
    auto belongsToClass = [&](uint32_t shape) {
        return m_tables.hasLayoutForShape(shape) && classes.classOfInstances(m_tables.layoutForShape(shape)).index == classLayout.index;
    };
    for (uint32_t shape : m_shapesWithStructure) {
        if (m_structures[shape] && belongsToClass(shape))
            m_structures[shape] = 0;
    }

    for (AllocationSiteRecord& record : std::span { m_state.instance.objects.emptyAllocationSite() - m_numberOfAllocationSiteRecords, m_numberOfAllocationSiteRecords }) {
        uint32_t shape = shapeOf(shapeWordWithoutMark(static_cast<uint32_t>(record.orderAndShapeWords >> 32)));
        if (record.header && shape < m_tables.firstEnvironmentShape && belongsToClass(shape))
            record = emptyAllocationSiteRecord();
    }
}

static Structure* structureOf(const AllocationSiteRecord& record)
{
    return WTF::opaque(std::bit_cast<StructureID>(static_cast<uint32_t>(record.header)).decode());
}

bool StructureTable::takeFirstObjectOf(uint32_t number)
{
    const AllocationData allocation = m_tables.allocations().checkedAt(number);
    if (allocation.isConstructed || allocatedCellKindOf(m_tables, allocation.shape) != AllocatedCellKind::Object || m_tables.layoutForShape(allocation.shape).kind() != LayoutKind::Record)
        return false;
    return !m_allocationsThatMadeObjects.quickSet(number);
}

ALWAYS_INLINE_INTO_OTHER_FILES AllocationSiteRecord StructureTable::recordForObject(VM& vm, uint32_t number)
{
    AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (record && record->header) [[likely]]
        return *record;
    if (!takeFirstObjectOf(number))
        return fillAllocationSite(vm, number);
    const AllocationData allocation = m_tables.allocations()[number];
    AllocationSiteRecord result = emptyAllocationSiteRecord();
    result.header = cellHeaderFor(structureForFirstObject(vm, allocation.shape));
    result.orderAndShapeWords = orderAndShapeWordsFor(allocation);
    return result;
}

JSCell* StructureTable::allocate(VM& vm, uint32_t number)
{
    Structure* structure = structureOf(recordForObject(vm, number));
    const AllocationData allocation = m_tables.allocations()[number];
    switch (allocatedCellKindOf(m_tables, allocation.shape)) {
    case AllocatedCellKind::Object: {
        Object* object = Object::create(vm, structure, m_tables.layoutForShape(allocation.shape), allocation.size(), allocation.orderWord());
        object->setIsInBirth(allocation.isBornUnfinished);
        ensureStillAliveHere(structure);
        return object;
    }
    case AllocatedCellKind::Closure:
        return Closure::createForCompiledCode(vm, structure, allocation.size());
    case AllocatedCellKind::Environment:
        return Environment::create(vm, structure, m_tables.environmentForShape(allocation.shape), allocation.size());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSCell* StructureTable::allocateAsCompiledCodeDoes(VM& vm, uint32_t number)
{
    static_assert(offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && sizeof(JSCell) == sizeof(uint64_t));
    const AllocationData allocation = m_tables.allocations()[number];
    if (allocation.shape >= m_tables.firstEnvironmentShape)
        return allocate(vm, number);
    const AllocationSiteRecord record = recordForObject(vm, number);
    Structure* structure = structureOf(record);
    Structure* structureOfCell = structure;
    size_t size = allocation.size();
    if (m_shapesWithGrownCells.quickGet(allocation.shape)) [[unlikely]] {
        const LayoutData& layout = m_tables.layoutForShape(allocation.shape);
        if (size == layout.size) {
            structureOfCell = WTF::opaque(withSpareWord(vm, *uncheckedDowncast<CompiledStructure>(structure)));
            size = sizeWithSpareWord(layout);
            AllocationSiteRecord* site = m_state.instance.objects.allocationSite(number);
            LocalAllocator* allocator = vm.cellSpace().allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
            auto grown = site && site->header == record.header && allocator ? m_grownCells.find(site) : m_grownCells.end();
            if (grown != m_grownCells.end()) {
                grown->value->record = { cellHeaderFor(structureOfCell), record.orderAndShapeWords, nullptr, record.prototype };
                grown->value->allocatorAndMark = std::bit_cast<uintptr_t>(allocator) | GrownCells::isArmed;
            }
        }
    }
    auto* words = static_cast<uint64_t*>(vm.cellSpace().allocate(vm, size, nullptr, AllocationFailureMode::Assert));
    words[0] = structureOfCell == structure ? record.header : cellHeaderFor(structureOfCell);
    words[1] = record.orderAndShapeWords;
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(words + 2), size - 2 * sizeof(uint64_t) });
    ensureStillAliveHere(structure);
    ensureStillAliveHere(structureOfCell);
    return reinterpret_cast<JSCell*>(words);
}

CompiledStructure::CompiledStructure(VM& vm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const ObjectParameters& parameters)
    : Structure(vm, StructureVariant::Compiled, parameters.realm, parameters.prototype, typeInfo, classInfo)
    , m_layout(parameters.layout)
{
    ASSERT(typeInfo.type() == CompiledObjectType);
    setIsQuickPropertyAccessAllowedForEnumeration(false);
    setDidPreventExtensions(false);
    setDidTransition(true);
    setHasAnyKindOfGetterSetterProperties(true);
    setHasReadOnlyOrGetterSetterPropertiesExcludingProto(true);
    setHasNonEnumerableProperties(true);
    setHasNonConfigurableProperties(true);
    setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
    initializeForObject(parameters);
}

CompiledStructure::CompiledStructure(VM& vm, const CompiledStructure& templateStructure, const ObjectParameters& parameters)
    : Structure(vm, templateStructure, parameters.prototype)
    , m_layout(parameters.layout)
{
    ASSERT(templateStructure.describesObject() && templateStructure.realm() == parameters.realm);
    initializeForObject(parameters);
}

CompiledStructure::CompiledStructure(VM& vm, JSGlobalObject* realm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const EnvironmentShapes& shapes, uint16_t shapePage)
    : Structure(vm, StructureVariant::Compiled, realm, jsNull(), typeInfo, classInfo)
    , m_environmentShapes(&shapes)
    , m_environmentShapePage(shapePage)
{
    ASSERT(typeInfo.type() == CompiledEnvironmentType);
}

CompiledStructure::~CompiledStructure()
{
    delete rareData();
}

static size_t roomForKeyOrder(const CompiledStructure::ObjectParameters& parameters)
{
    constexpr size_t smallestRoom = 16;
    size_t size = parameters.outOfLineKeyOrder->size();
    size_t numberOfFields = parameters.layout->numberOfFields;
    size_t longest = std::min<size_t>(parameters.hasExtraProperties ? 2 * numberOfFields + 1 : numberOfFields, maxFieldsInOrder);
    return std::max(size, std::min(longest, std::max(2 * size, smallestRoom)));
}

void CompiledStructure::initializeForObject(const ObjectParameters& parameters)
{
    static_assert(std::endian::native == std::endian::little);
    static_assert(static_cast<uint32_t>(endOfExtraPropertiesCellSizes) << 16 == orderWordHasExtraProperties);
    static_assert(ListedFields::noOrderWord != outOfLineOrderWord);
    ASSERT(m_layout && (parameters.hasExtraProperties || (!parameters.heldExtraProperties && !parameters.extraPropertiesStructure)));

    if (std::optional<SlotData> slot = m_layout->extraPropertiesSlot())
        m_extraPropertiesSlot = *slot;
    else if (parameters.hasSpareWord)
        m_extraPropertiesSlot = SlotData::create(m_layout->size, hiddenReferenceRepresentation(parameters.realm->aotInstance()->options().useWideReferences()));
    if (std::optional<SlotData> slot = m_layout->extensionSlot())
        m_extensionSlot = *slot;

    if (m_layout->hasFieldsWithSymbolKeys)
        m_flags.add(Flag::HasFieldsWithSymbolKeys);
    if (m_layout->hasOnlyPlainSlots)
        m_flags.add(Flag::HasOnlyPlainSlots);
    if (m_layout->isCopiedByWords)
        m_flags.add(Flag::IsCopiedByWords);
    RELEASE_ASSERT(isPristine() == parameters.hasOtherPrototype);
    if (parameters.hasOtherPrototype)
        m_flags.add(Flag::HasOtherPrototype);
    if (parameters.hasExtraProperties)
        m_flags.add(Flag::HasExtraProperties);
    if (parameters.outOfLineKeyOrder) {
        m_flags.add(Flag::HasOutOfLineKeyOrder);
        if (parameters.hasExtraProperties)
            m_furtherFlags.add(FurtherFlag::HasKeyOrderWithExtraProperties);
    }
    if (!parameters.hasOtherPrototype && !parameters.hasExtraProperties && parameters.prototype == JSValue(parameters.realm->objectPrototype()))
        m_flags.add(Flag::HasOnlyFieldsBelowObjectPrototype);
    setMayBePrototype(parameters.mayBePrototype);
    if (parameters.reportsReplacements)
        m_furtherFlags.add(FurtherFlag::ReportsReplacements);
    if (holdsListedFields())
        return;

    std::unique_ptr<RareData> rareData;
    if (parameters.outOfLineKeyOrder || parameters.heldExtraProperties) {
        rareData = makeUnique<RareData>();
        if (parameters.outOfLineKeyOrder) {
            if (!parameters.keysNamedByOrder)
                rareData->outOfLineKeyOrder.reserveInitialCapacity(roomForKeyOrder(parameters));
            rareData->outOfLineKeyOrder.append(*parameters.outOfLineKeyOrder);
        }
        if (parameters.keysNamedByOrder) {
            ASSERT(parameters.outOfLineKeyOrder);
            rareData->keysNamedByOrder = Vector<RefPtr<UniquedStringImpl>>(*parameters.keysNamedByOrder);
            m_furtherFlags.add(FurtherFlag::HasKeyOrderOfOneObject);
        }
        rareData->heldExtraProperties.setWithoutWriteBarrier(parameters.heldExtraProperties);
    }
    uintptr_t bitsOfRareData = std::bit_cast<uintptr_t>(rareData.release());
    RELEASE_ASSERT(!(bitsOfRareData >> 48));
    m_highBitsOfRareData = static_cast<uint16_t>(bitsOfRareData >> 32);
    m_namedStructureAndRareData = { 0, static_cast<uint32_t>(bitsOfRareData) };

    if (parameters.hasExtraProperties) {
        uint16_t cellSize = 0;
        if (Structure* named = parameters.extraPropertiesStructure) {
            cellSize = static_cast<uint16_t>(WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(JSFinalObject::allocationSize(named->inlineCapacity())));
            m_namedStructureAndRareData.extraPropertiesStructureID = named->id().bits();
        }
        RELEASE_ASSERT(cellSize < endOfExtraPropertiesCellSizes);
        m_extraKeysAndCellSize = { unknownNumberOfExtraKeys, cellSize };
    }
}

void CompiledStructure::setHeldStructure(VM& vm, Structure* structure) const
{
    ASSERT(heldExtraProperties() && heldExtraProperties()->structure() == structure && !structure->isDictionary());
    const_cast<RareData*>(rareData())->heldStructure.set(vm, this, structure);
}

static CompiledStructure* finishCreationOfObjectStructure(VM& vm, CompiledStructure* structure)
{
    if (structure->extraPropertiesStructure() || structure->heldExtraProperties())
        vm.writeBarrier(structure);
    return structure;
}

CompiledStructure* CompiledStructure::createForObject(VM& vm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const ObjectParameters& parameters)
{
    ASSERT(vm.structureStructure);
    CompiledStructure* structure = new (NotNull, allocateCell<CompiledStructure>(vm)) CompiledStructure(vm, typeInfo, classInfo, parameters);
    structure->finishCreation(vm);
    return finishCreationOfObjectStructure(vm, structure);
}

CompiledStructure* CompiledStructure::createForObject(VM& vm, const CompiledStructure& templateStructure, const ObjectParameters& parameters)
{
    CompiledStructure* structure = new (NotNull, allocateCell<CompiledStructure>(vm)) CompiledStructure(vm, templateStructure, parameters);
    structure->finishCreation(vm);
    return finishCreationOfObjectStructure(vm, structure);
}

CompiledStructure* CompiledStructure::createForEnvironment(VM& vm, JSGlobalObject* realm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const EnvironmentShapes& shapes, uint16_t shapePage)
{
    ASSERT(vm.structureStructure);
    CompiledStructure* structure = new (NotNull, allocateCell<CompiledStructure>(vm)) CompiledStructure(vm, realm, typeInfo, classInfo, shapes, shapePage);
    structure->finishCreation(vm);
    return structure;
}

void CompiledStructure::destroy(JSCell* cell)
{
    static_cast<CompiledStructure*>(cell)->CompiledStructure::~CompiledStructure();
}

template<typename Visitor>
void CompiledStructure::visitAdditionalChildren(JSCell* cell, Visitor& visitor)
{
    CompiledStructure* structure = uncheckedDowncast<CompiledStructure>(uncheckedDowncast<Structure>(cell));
    if (Structure* named = structure->extraPropertiesStructure())
        visitor.appendUnbarriered(named);
    if (const RareData* rareData = structure->rareData()) {
        visitor.append(rareData->heldExtraProperties);
        if (Structure* listed = rareData->heldStructure.get())
            visitor.appendUnbarriered(listed);
    }
    visitor.append(structure->m_successorByPrivateName);
}

template void CompiledStructure::visitAdditionalChildren(JSCell*, AbstractSlotVisitor&);
template void CompiledStructure::visitAdditionalChildren(JSCell*, SlotVisitor&);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
