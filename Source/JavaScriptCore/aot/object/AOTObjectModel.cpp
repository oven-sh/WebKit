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
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTFieldTable.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTInterpreterSlowPaths.h"
#include "AOTModuleNamespace.h"
#include "AOTObject.h"
#include "AOTObjectModelCaches.h"
#include "AOTPrimitives.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTSlotAccess.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AbstractSlotVisitorInlines.h"
#include "ConservativeRoots.h"
#include "DeferGCInlines.h"
#include "FastMarkAppender.h"
#include "GetterSetter.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "ObjectConstructor.h"
#include "PropertyName.h"
#include "SlotVisitorInlines.h"
#include "Symbol.h"
#include "WeakGCMapInlines.h"
#include <algorithm>
#include <array>
#include <bit>
#include <tuple>
#include <wtf/BitSet.h>
#include <wtf/CommaPrinter.h>
#include <wtf/Hasher.h>
#include <wtf/PrintStream.h>
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
    , memberGuardWatchpoints(state)
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
        return imageText.hasText(StringID(literal)) && imageText.view(StringID(literal)).length() == characters.length() && imageText.equals(StringID(literal), characters);
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
    return !range || (number >= static_cast<double>(range->first) && number <= static_cast<double>(range->second));
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
    if (type.flags & RuntimeType::hasLiteralStringWithoutText) {
        if (std::optional<uint32_t> number = state.strings->findLiteralString(value))
            return std::ranges::contains(literals, state.tables.literalStrings()[*number]);
        if (!(type.flags & RuntimeType::hasLiteralStringWithoutWord))
            return false;
    }
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
    , m_cachedFields(state.tableArena.allocate<CachedField>(numberOfCachedFields))
{
    state.instance.shapeSets = m_tables.shapeSetBits().data();
}

RuntimeLayoutTable::~RuntimeLayoutTable()
{
    for (auto& cached : std::span { m_cachedFields, numberOfCachedFields }) {
        if (cached.name)
            cached.name->deref();
    }
}

const RuntimeType* RuntimeLayoutTable::typeWithIndex(uint32_t typeIndex) const
{
    uint32_t offset = m_tables.offsetOfType(typeIndex);
    return offset ? &typeAt(offset) : nullptr;
}

const FieldData* RuntimeLayoutTable::field(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    if (name->isSymbol() && !layout.hasFieldsWithSymbolKeys && !static_cast<SymbolImpl*>(name)->isPrivate())
        return nullptr;
    CachedField& cached = m_cachedFields[(layout.shape * 0x9e3779b1u ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(name) >> 4)) % numberOfCachedFields];
    if (cached.shape == layout.shape && cached.name == name) [[likely]]
        return cached.field;
    const FieldData* found = findField(vm, layout, name);
    ASSERT(!found || !parseIndex(PropertyName(name)));
    name->ref();
    if (cached.name)
        cached.name->deref();
    cached = { layout.shape, name, found };
    return found;
}

const FieldData* RuntimeLayoutTable::findField(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(name);
    if (!number)
        return nullptr;
    if (m_tables.hasFieldTableColumn(*number) && layout.shape && layout.shape <= m_tables.numberOfFieldTableRows) {
        std::optional<uint16_t> index = findFieldInTable(m_tables, layout.shape, *number);
        ASSERT(!index || layout.fields()[*index].name == *number);
        return index ? &layout.fields()[*index] : nullptr;
    }
    return findFieldWithoutColumn(layout, *number);
}

static constexpr uint16_t noFieldInSlot = UINT16_MAX;
static_assert(maxFieldsInOrder * 2 < 1u << (32 - 12) && maxFieldsInOrder < noFieldInSlot);

static size_t firstSlotOfName(uint32_t identifierNumber, size_t mask)
{
    return (identifierNumber * 0x9e3779b1u >> 12) & mask;
}

static Vector<uint16_t> indexFieldsByName(std::span<const FieldData> fields)
{
    Vector<uint16_t> slots;
    slots.fill(noFieldInSlot, std::bit_ceil(fields.size() * 2));
    size_t mask = slots.size() - 1;
    // Of two fields with one name the first is the one that a walk over the fields finds.
    for (size_t index = 0; index < fields.size(); ++index) {
        size_t slot = firstSlotOfName(fields[index].name, mask);
        while (slots[slot] != noFieldInSlot && fields[slots[slot]].name != fields[index].name)
            slot = (slot + 1) & mask;
        if (slots[slot] == noFieldInSlot)
            slots[slot] = static_cast<uint16_t>(index);
    }
    return slots;
}

const FieldData* RuntimeLayoutTable::findFieldWithoutColumn(const LayoutData& layout, uint32_t identifierNumber)
{
    constexpr size_t minNumberOfFieldsToIndex = 12;
    std::span<const FieldData> fields = layout.fields();
    if (fields.size() < minNumberOfFieldsToIndex) {
        for (const FieldData& field : fields) {
            if (field.name == identifierNumber)
                return &field;
        }
        return nullptr;
    }
    auto found = m_fieldsByName.find(layout.index + 1);
    if (found == m_fieldsByName.end())
        found = m_fieldsByName.add(layout.index + 1, indexFieldsByName(fields)).iterator;
    const Vector<uint16_t>& slots = found->value;
    size_t mask = slots.size() - 1;
    for (size_t slot = firstSlotOfName(identifierNumber, mask); slots[slot] != noFieldInSlot; slot = (slot + 1) & mask) {
        const FieldData& field = fields[slots[slot]];
        if (field.name == identifierNumber)
            return &field;
    }
    return nullptr;
}

static thread_local bool heapSnapshotLooksForConstructor;

HeapHooks::LookForConstructorBySnapshot::LookForConstructorBySnapshot()
{
    ASSERT(!heapSnapshotLooksForConstructor);
    heapSnapshotLooksForConstructor = true;
}

HeapHooks::LookForConstructorBySnapshot::~LookForConstructorBySnapshot()
{
    heapSnapshotLooksForConstructor = false;
}

// A store under the name of a removed field creates an ordinary property, which breaks no proof: the proof is that nothing reads the field.
void RuntimeLayoutTable::failIfFieldWasRemoved(VM& vm, const LayoutData& layout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(name);
    if (!number || !std::ranges::binary_search(m_tables.removedFields(), RemovedFieldData { layout.index, *number }))
        return;
    if (heapSnapshotLooksForConstructor && name == vm.propertyNames->constructor.impl())
        return;
    dataLogLn("AOT: a read names the field ", String(name), " of layout ", layout.index, ", which was removed as unread");
    crashOnValidationFailure(vm, { ValidationFailureKind::RemovedFieldNamed, { }, { }, JSValue() });
}

// The lists have names only, no index and no private name.
void RuntimeLayoutTable::failIfNameIsNotListed(VM& vm, const LayoutData& layout, UniquedStringImpl* name) const
{
    std::span<const AddedNameData> listed = m_tables.addedNames();
    if (!std::ranges::binary_search(listed, AddedNameData { layout.index, AddedNameData::listIsComplete }) || parseIndex(*name) || PropertyName(name).isPrivateName())
        return;
    std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(name);
    if (number && std::ranges::binary_search(listed, AddedNameData { layout.index, *number }))
        return;
    if (number && std::ranges::binary_search(listed, AddedNameData { layout.index, AddedNameData::getsSharedNames }) && std::ranges::binary_search(listed, AddedNameData { AddedNameData::layoutOfSharedNames, *number }))
        return;
    dataLogLn("AOT: an object of layout ", layout.index, " gets the property ", String(name), ", which is not among the names that its objects get");
    crashOnValidationFailure(vm, { ValidationFailureKind::ImpossibleMutation, { }, { }, JSValue() });
}

bool RuntimeLayoutTable::everyObjectHadRemovedField(const LayoutData& layout) const
{
    return std::ranges::binary_search(m_tables.removedFields(), RemovedFieldData { layout.index, RemovedFieldData::everyObjectHadOne });
}

UniquedStringImpl* RuntimeLayoutTable::nameOf(VM& vm, const FieldData& field)
{
    return vm.aotState().identifiers->uid(field.name);
}

bool RuntimeLayoutTable::hasSymbolKey(const FieldData& field) const
{
    return keyKindOf(textOf(m_state).identifier(field.name).kind()) == FieldKeyKind::Symbol;
}

bool RuntimeLayoutTable::hasStringKey(const FieldData& field) const
{
    return keyKindOf(textOf(m_state).identifier(field.name).kind()) == FieldKeyKind::String && !field.isPrivate();
}

bool RuntimeLayoutTable::mayBeAbsent(const FieldData& field) const
{
    return traitsOf(field.slot.representation()).hasAbsent || field.isOutOfLine();
}

Object* RuntimeLayoutTable::compiledObject(JSValue value) const
{
    Object* object = compiledObjectOrNull(value);
    return object && &object->state().tables == &m_tables ? object : nullptr;
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
    return closureNumber && type.listsClosureNumber(*closureNumber);
}

std::optional<AcceptedValue> RuntimeLayoutTable::accept(const FieldData& field, JSValue value) const
{
    // The slot of an accessor field holds its GetterSetter. The type of the field is that of the getter: compiled code calls what it finds in a filled slot as one of the functions that the type lists.
    if (field.isAccessor()) {
        if (!value || !value.isCell() || value.asCell()->type() != GetterSetterType)
            return std::nullopt;
        if (!valueHasType(typeOf(field), uncheckedDowncast<GetterSetter>(value.asCell())->getter()))
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
        return object && passes(m_tables.shapeTestAt(test.shapeTestOffset()), object->shape());
    }
    case CellTestKind::InheritsClassInfo:
        return inheritsBuiltinClass(cell, test.builtinClass());
    case CellTestKind::IsPristine:
        return isInstanceOfBuiltinClass(cell, test.builtinClass()) && cell->structure()->isPristine();
    case CellTestKind::IsBuiltinCell: {
        // The built-in may have arrived by a generic path before compiled code filled its word.
        DeferGCForAWhile deferGC(*m_state.instance.vm);
        return cell == m_state.builtinCells->cell(test.builtinCellNumber());
    }
    case CellTestKind::IsClosureOf:
        return closureNumberOf(cell) == test.closureNumber();
    case CellTestKind::IsSymbol:
        // A symbol that nothing has made yet is the uid of no cell.
        return cell->isSymbol() && &asSymbol(cell)->uid() == m_state.instance.vm->aotState().identifiers->tryGetIdentifierConcurrently(test.symbolKeyNumber());
    case CellTestKind::SharingModeIs:
        return hasSharingMode(cell, test.sharingMode());
    case CellTestKind::IsNamespaceOf: {
        auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
        return moduleNamespace && moduleNamespace->hasNamespaceLayoutOf(test.module());
    }
    case CellTestKind::CarriesBrandOfClass:
        return m_state.objects->classes.carriesBrandOfClass(cell->structure(), test.rankOfClass());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool RuntimeLayoutTable::shapeHasType(const RuntimeType& type, uint32_t shape) const
{
    return std::ranges::any_of(type.cellTests(), [&](const RuntimeCellTest& test) {
        switch (test.kind) {
        case CellTestKind::IsObject:
            return true;
        case CellTestKind::CellTypeIs:
            return test.cellTypes().contains(CompiledObjectType);
        case CellTestKind::ShapeIn:
            return passes(m_tables.shapeTestAt(test.shapeTestOffset()), shape);
        case CellTestKind::IsCallable:
        case CellTestKind::InheritsClassInfo:
        case CellTestKind::IsPristine:
        case CellTestKind::IsBuiltinCell:
        case CellTestKind::IsClosureOf:
        case CellTestKind::IsSymbol:
        case CellTestKind::SharingModeIs:
        case CellTestKind::IsNamespaceOf:
        case CellTestKind::CarriesBrandOfClass:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

// The collector visits no object that lies in a frame, so such an object keeps its Structure alive only if the scan of the stack takes its header for a reference.
static_assert(ConservativeRoots::findsStructuresOfFrameCells);

struct StructureTable::VariantMaps {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(VariantMaps);

    using Key = std::tuple<uint32_t, JSObject*, Structure*, uint32_t>;

    static constexpr uint32_t keyMayBePrototype = 1u << 31;
    static constexpr uint32_t keyHasUnnamedExtraProperties = 1u << 30;
    static constexpr uint32_t keyHasOtherPrototype = 1u << 29;
    static constexpr unsigned recentIndexBits = 7;
    static constexpr unsigned requestedIndexBits = 12;

    static Key keyOf(const Variant& variant)
    {
        uint32_t shapeAndFlags = variant.layout->shape;
        RELEASE_ASSERT(shapeAndFlags < keyHasOtherPrototype);
        if (variant.mayBePrototype)
            shapeAndFlags |= keyMayBePrototype;
        if (variant.hasExtraProperties && !variant.extraPropertiesStructure)
            shapeAndFlags |= keyHasUnnamedExtraProperties;
        if (variant.hasOtherPrototype)
            shapeAndFlags |= keyHasOtherPrototype;
        return { shapeAndFlags, variant.prototype.getObject(), variant.extraPropertiesStructure, variant.outOfLineKeyOrder ? 1 | computeHash(*variant.outOfLineKeyOrder) : 0 };
    }

    static bool describes(const Variant& variant, const CompiledStructure& structure)
    {
        if (structure.layout().shape != variant.layout->shape || structure.storedPrototype() != variant.prototype)
            return false;
        if (structure.hasOtherPrototype() != variant.hasOtherPrototype || structure.mayBePrototype() != variant.mayBePrototype)
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

    explicit VariantMaps(VM& vm)
        : structures(vm)
    {
    }

    // A variant may have no object yet or no more. Whoever gets one out of these tables holds it by this pointer alone, which the compiler must not make anew from the ID behind an allocation.
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

    // A variant ends with what it hangs on, not with the last object that has it: a full collection may find no such object, as in a server between two requests, and the program would learn the shape again.
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

    struct FiledVariant {
        Key key;
        StructureID id;
    };

    WeakGCMap<Key, CompiledStructure> structures;
    // The values of `structures`. Written under the lock of the caches.
    Vector<StructureID> listed;
    Vector<FiledVariant> filedSinceCollection;
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
            | !!variant.extraPropertiesStructure << 2
            | (variant.hasExtraProperties && !variant.extraPropertiesStructure) << 3
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
        auto [shapeAndFlags, prototype, extraPropertiesStructure, orderHash] = VariantMaps::keyOf(variant);
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
    AOT::sort(mostOften, std::ranges::greater { });
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
    state.instance.objects.copiesWithNames = state.tableArena.allocate<uint8_t>(sizeOfCopiesWithNames);
    state.caches->addClearedEachCollection("copies with names"_s, std::span { state.instance.objects.copiesWithNames, sizeOfCopiesWithNames });
}

StructureTable::~StructureTable() = default;

std::span<AllocationSiteRecord> StructureTable::allocationSiteRecords() const
{
    size_t numberOfAllocations = m_tables.allocations().size();
    return { m_state.instance.objects.emptyAllocationSite() - numberOfAllocations, numberOfAllocations + 1 };
}

// The objects of many layouts live shorter than the time between two collections.
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
    for (const VariantMaps::FiledVariant& filed : m_variants->filedSinceCollection) {
        CompiledStructure* variant = VariantMaps::decode(filed.id);
        if (variant->hasOutOfLineKeyOrder())
            continue;
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
        for (const VariantMaps::FiledVariant& filed : m_variants->filedSinceCollection) {
            if (!vm.heap.isMarked(filed.id.decode()) && !m_variants->structures.get(filed.key))
                m_variants->structures.remove(filed.key);
        }
        m_variants->filedSinceCollection.shrink(0);
        m_variants->listed.removeAllMatching([&](StructureID id) {
            return !vm.heap.isMarked(id.decode());
        });
    }
    // Whatever region of the two tables a collection looks at, it has taken every entry there for a root: none has ended.
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
    validateStructuresBelowAwaitedPrototype(vm);
    return removed;
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

ALWAYS_INLINE CompiledStructure* StructureTable::usualStructureIfExists(uint32_t shape) const
{
    RELEASE_ASSERT(shape < m_tables.firstEnvironmentShape);
    uint32_t known = m_structures[shape];
    return known ? VariantMaps::decode(std::bit_cast<StructureID>(known)) : nullptr;
}

NEVER_INLINE JSObject* StructureTable::awaitedPrototype(VM& vm)
{
    if (!m_awaitedPrototype) {
        m_awaitedPrototype = constructEmptyObject(vm, m_state.instance.globalObject->nullPrototypeObjectStructure());
        m_state.immortalCells->add(m_awaitedPrototype);
    }
    return m_awaitedPrototype;
}

ALWAYS_INLINE StructureTable::Variant StructureTable::usualVariantOf(VM& vm, const LayoutData& layout)
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
        else {
            usual.hasOtherPrototype = true;
            // The first object that Object.create makes at some place is allocated before its class has a prototype object. Where an object may still define the class, it is born below an object
            // that no program holds. So the comparison that compiled code makes is the only one: whatever the call is handed is another object, and the operation hears of it. That object
            // leaves this Structure there, and takes it along: leaveAwaitedPrototype().
            if (layout.evaluation() == EvaluationState::ProvenOnce && m_state.objects->classes.awaitsPrototypeOfCreatedObjects(layout))
                usual.prototype = awaitedPrototype(vm);
        }
        return usual;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE CompiledStructure* StructureTable::create(VM& vm, const LayoutData& layout, const Variant& variant)
{
    JSGlobalObject* realm = m_state.instance.globalObject;
    CompiledStructure*& templateStructure = m_templateStructures[variant.hasOtherPrototype];
    if (!templateStructure) {
        templateStructure = Object::createStructure(vm, { .realm = realm, .prototype = jsNull(), .layout = &layout, .hasOtherPrototype = variant.hasOtherPrototype });
        m_state.immortalCells->add(templateStructure);
    }
    if (JSObject* object = variant.prototype.getObject(); object && !object->structure()->mayBePrototype())
        object->didBecomePrototype(vm);
    count(vm, RuntimeCounterID::CompiledStructuresMade);
    if (m_made) [[unlikely]]
        m_made->note(layout, variant);
    RELEASE_ASSERT(!variant.hasExtraProperties || !layout.extraPropertiesSlot() == !!variant.heldExtraProperties);
    RELEASE_ASSERT(!variant.heldExtraProperties || (variant.hasExtraProperties && !variant.extraPropertiesStructure));
    if (variant.realm == realm)
        return CompiledStructure::createForObject(vm, *templateStructure, variant);
    Variant parameters = variant;
    parameters.realm = realm;
    return CompiledStructure::createForObject(vm, *templateStructure, parameters);
}

ALWAYS_INLINE CompiledStructure* StructureTable::createUsual(VM& vm, const LayoutData& layout, const Variant& usual)
{
    CompiledStructure* created = create(vm, layout, usual);
    uint32_t& known = m_structures[layout.shape];
    known = created->id().bits();
    m_writtenStructures.record(&known);
    if (!m_shapeHasStructure.set(layout.shape))
        m_shapesWithStructure.append(layout.shape);
    // The first object that Object.create makes at some place is allocated before its class has a prototype object, and nobody is told of it. Where an object may define the class, no object
    // keeps this Structure: it is the one below m_awaitedPrototype. The other classes of created objects have no guards.
    if (usual.hasOtherPrototype && !(layout.isClass() && m_state.objects->classes.awaitsPrototypeOfCreatedObjects(layout)))
        m_state.objects->classes.didChangeInstancePrototype(vm, layout, usual.prototype);
    return created;
}

ALWAYS_INLINE CompiledStructure* StructureTable::usualStructure(VM& vm, uint32_t shape)
{
    if (CompiledStructure* known = usualStructureIfExists(shape))
        return known;
    RELEASE_ASSERT(m_tables.hasLayoutForShape(shape));
    const LayoutData& layout = m_tables.layoutForShape(shape);
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
    if (belongsToOneObject(variant))
        return nullptr;
    if (CompiledStructure* usual = usualStructureIfExists(variant.layout->shape); usual && VariantMaps::describes(variant, *usual))
        return usual;
    CompiledStructure* found = m_variants->structures.get(VariantMaps::keyOf(variant));
    return found && VariantMaps::describes(variant, *found) ? found : nullptr;
}

CompiledStructure* StructureTable::findOrCreate(VM& vm, const Variant& requested)
{
    // outOfLineKeyOrder may point into a Structure that nothing else keeps alive.
    // A collection that is put off runs at the next poll or allocation, not when this returns: this frame and the caller's hold Variants,
    // and an empty std::optional leaves its payload unwritten, so the conservative scan would keep whatever stale pointers lie there alive.
    DeferGCForAWhile deferGC(vm);
    const LayoutData& layout = *requested.layout;
    CompiledStructure* usualStructure = usualStructureIfExists(layout.shape);
    RELEASE_ASSERT(m_tables.hasLayoutForShape(layout.shape));
    // Where every object of the layout has another Structure, nobody needs the usual one.
    Variant usual = usualStructure ? variantOf(*usualStructure) : usualVariantOf(vm, layout);
    Variant variant = requested;
    // Compiled code tests the flag after the read that relies on it, so an object that had it keeps it.
    variant.hasOtherPrototype |= usual.hasOtherPrototype || usual.prototype != variant.prototype;
    if (CompiledStructure* existing = find(variant))
        return existing;
    ASSERT(layout.shape == usual.layout->shape);
    if (!variant.mayBePrototype && !variant.hasExtraProperties && !variant.outOfLineKeyOrder && variant.prototype == usual.prototype && variant.hasOtherPrototype == usual.hasOtherPrototype)
        return createUsual(vm, layout, usual);
    CompiledStructure* created = create(vm, layout, variant);
    if (!belongsToOneObject(variant)) {
        VariantMaps::Key key = VariantMaps::keyOf(variant);
        m_variants->structures.set(key, created);
        Locker locker { m_state.caches->lock() };
        m_variants->filedSinceCollection.append({ key, created->id() });
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
        return VariantMaps::decode(std::bit_cast<StructureID>(recent.after));

    Variant variant = variantWithKeyOrderOf(current);
    variant.prototype = prototype;
    CompiledStructure* structure = findOrCreate(vm, variant);
    // Compiled code replays a transition by storing the StructureID and setting IsPristine, which a compiled object has where its prototype is not its layout's, in the cell: it looks for no cycle and tells nobody who depends on the chain.
    constexpr uint32_t pristineBitInBlob = IsPristine << 16;
    if (structure->isPristine() && structure->typeInfoBlob() == (current.typeInfoBlob() | pristineBitInBlob) && !current.mayBePrototype() && !belongsToOneObject(variant))
        recent = { encodedPrototype, before, structure->id().bits() };
    return structure;
}

// The object that was born below m_awaitedPrototype takes that Structure along: no other object has it, and no record of a place names it. The class has no prototype object then, so the
// objects that follow are born below Object.prototype, as those of a layout without a class are. Whoever makes that Structure tells the class of objects below another prototype: a class that
// awaits its prototype object as before need not hear of them again. No transition is recorded, so compiled code replays none from a Structure that an object has on its way to the operation.
CompiledStructure* StructureTable::leaveAwaitedPrototype(VM& vm, const CompiledStructure& atBirth, JSValue prototype)
{
    const LayoutData& layout = atBirth.layout();
    ASSERT(isBelowAwaitedPrototype(atBirth) && usualStructureIfExists(layout.shape) == &atBirth);
    Variant variant = variantWithKeyOrderOf(atBirth);
    variant.prototype = prototype;
    CompiledStructure* structure = findOrCreate(vm, variant);

    JSGlobalObject* realm = m_state.instance.globalObject;
    uint32_t& known = m_structures[layout.shape];
    known = 0;
    known = findOrCreate(vm, Variant { .realm = realm, .prototype = realm->objectPrototype(), .layout = &layout, .hasOtherPrototype = true })->id().bits();
    m_writtenStructures.record(&known);
    return structure;
}

CompiledStructure* StructureTable::asPrototype(VM& vm, const CompiledStructure& current)
{
    Variant variant = variantWithKeyOrderOf(current);
    variant.mayBePrototype = true;
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

// Such a Structure belongs to one object: no table lists it.
CompiledStructure* StructureTable::holdingExtraProperties(VM& vm, const CompiledStructure& current, JSObject* extraProperties)
{
    ASSERT(!current.extraPropertiesSlot() && !current.hasExtraProperties());
    Variant variant = variantWithKeyOrderOf(current);
    variant.hasExtraProperties = true;
    variant.heldExtraProperties = extraProperties;
    return findOrCreate(vm, variant);
}

CompiledStructure* StructureTable::withOutOfLineKeyOrder(VM& vm, const CompiledStructure& current, std::span<const uint16_t> entries)
{
    Variant variant = variantOf(current);
    variant.outOfLineKeyOrder = entries;
    variant.keysNamedByOrder = std::nullopt;
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

static constexpr uint64_t orderAndShapeWordsFor(uint32_t shape, uint32_t orderWord)
{
    return static_cast<uint64_t>(shapeWordOf(shape)) << 32 | orderWord;
}

static bool fillsSameRecord(const ProgramTables& tables, uint32_t first, uint32_t second)
{
    const AllocationData& a = tables.allocations()[first];
    const AllocationData& b = tables.allocations()[second];
    return a.shape == b.shape && a.orderWord() == b.orderWord() && a.size() == b.size() && a.isBornUnfinished == b.isBornUnfinished && tables.constructorOfAllocation(first) == tables.constructorOfAllocation(second);
}

// Allocations that list other fields may ask for the same cell. The image lists the allocations of one shape next to each other.
AllocationSiteRecord* StructureTable::recordOfSameAllocation(uint32_t number) const
{
    std::span<const AllocationData> allocations = m_tables.allocations();
    const ObjectWords& words = m_state.instance.objects;
    const AllocationData& allocation = allocations[number];
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

ALWAYS_INLINE AllocationSiteRecord& StructureTable::ensureRecord(uint32_t number, uint32_t shape)
{
    ObjectWords& words = m_state.instance.objects;
    if (AllocationSiteRecord* record = words.allocationSite(number))
        return *record;
    // Nearly every record is the first of its shape, and then there is no other to find: no object has been made yet, so the shape has no Structure.
    bool isFirstOfShape = shape < m_tables.firstEnvironmentShape && !m_structures[shape];
    AllocationSiteRecord* record = isFirstOfShape ? nullptr : recordOfSameAllocation(number);
    if (!record) {
        record = words.emptyAllocationSite() - ++m_numberOfAllocationSiteRecords;
        *record = emptyAllocationSiteRecord();
    }
    words.distancesToAllocationSites[number] = safeCast<int32_t>(reinterpret_cast<char*>(record) - reinterpret_cast<char*>(words.emptyAllocationSite()));
    return *record;
}

static uint64_t objectHeaderFor(Structure* structure, const AllocationData& allocation)
{
    static_assert(JSCell::typeInfoFlagsOffset() == 6);
    return cellHeaderFor(structure) | (allocation.isBornUnfinished ? static_cast<uint64_t>(TypeInfoPerCellBit) << 48 : 0);
}

AllocationSiteRecord& StructureTable::ensureRecord(uint32_t number)
{
    return ensureRecord(number, m_tables.allocations()[number].shape);
}

AllocationSiteRecord& StructureTable::fillAllocationSite(VM& vm, uint32_t number)
{
    // Until it is filled, compiled code finds no room at the record, as at one whose Structure was forgotten.
    const AllocationData& allocation = m_tables.allocations()[number];
    AllocationSiteRecord& record = ensureRecord(number, allocation.shape);
    if (record.header)
        return record;
    AllocatedCellKind kind = allocatedCellKindOf(m_tables, allocation.shape);
    Structure* structure = nullptr;
    if (allocation.isConstructed) {
        ASSERT(kind == AllocatedCellKind::Object);
        ObjectModel& objects = *m_state.objects;
        FunctionID function(m_tables.constructorOfAllocation(number));
        // Without a class object that the run time knows, the operation names the prototype. Instances of a class whose prototype is made first need no class object.
        JSCell* constructor = objects.closures.closureIfCreated(function);
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
    CompleteSubspace& space = kind == AllocatedCellKind::Object ? vm.cellSpace() : *vm.heap.aotClosureSpace<SubspaceAccess::OnMainThread>();
    // No size class holds a cell beyond MarkedSpace::largeCutoff, so no allocator does. Compiled code finds no room and leaves every such cell to the operation.
    void* allocator = space.allocatorFor(allocation.size(), AllocatorForMode::EnsureAllocator).localAllocator();
    // No record keeps the Structure below m_awaitedPrototype. The object that is born with it leaves it in the operation behind its allocation, and whoever allocates there next asks again.
    bool leavesRecordEmpty = isBelowAwaitedPrototype(*structure);
    AllocationSiteRecord& filled = leavesRecordEmpty ? m_recordBelowAwaitedPrototype : record;
    filled = AllocationSiteRecord {
        .header = kind == AllocatedCellKind::Environment ? Environment::headerFor(structure, allocation.shape) : objectHeaderFor(structure, allocation),
        .orderAndShapeWords = orderAndShapeWordsFor(allocation.shape, allocation.orderWord()),
        .allocator = allocator ? allocator : &m_allocatorWithoutRoom,
        .prototype = kind == AllocatedCellKind::Object ? static_cast<uint64_t>(JSValue::encode(structure->storedPrototype())) : 0,
    };
    if (leavesRecordEmpty) [[unlikely]]
        return filled;
    m_writtenRecords.record(&record);
    count(vm, RuntimeCounterID::AllocationSiteRecordsFilled);
    return record;
}

void StructureTable::predictStructure(VM&, uint32_t number, CompiledStructure* structure)
{
    // Where the class is evaluated once, an operation that names no prototype may share the record.
    const AllocationData& allocation = m_tables.allocations()[number];
    if (!allocation.isConstructed || structure->layout().shape != allocation.shape || structure->layout().evaluation() == EvaluationState::ProvenOnce)
        return;
    CompiledStructure* usual = usualStructureIfExists(allocation.shape);
    AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!usual || !record || !usual->hasOtherPrototype() || structureIDOf(number) != usual->id())
        return;
    record->header = objectHeaderFor(structure, allocation);
    record->prototype = static_cast<uint64_t>(JSValue::encode(structure->storedPrototype()));
    m_writtenRecords.record(record);
}

std::optional<StructureID> StructureTable::structureIDOf(uint32_t number) const
{
    const AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!record || !record->header)
        return std::nullopt;
    return std::bit_cast<StructureID>(static_cast<uint32_t>(record->header));
}

// A Structure below m_awaitedPrototype is for an object that the operation behind its allocation has not seen yet. No record of a place keeps one, and a class that has its prototype object has
// none: what Object.create makes there may take the Structure of its record and ask nobody.
void StructureTable::validateStructuresBelowAwaitedPrototype(VM& vm) const
{
    if (!m_awaitedPrototype)
        return;
    auto structureBelowAwaitedPrototype = [&](uint32_t structureIDOrZero) -> const CompiledStructure* {
        Structure* structure = structureIDOrZero ? std::bit_cast<StructureID>(structureIDOrZero).decode() : nullptr;
        return structure && isBelowAwaitedPrototype(*structure) ? uncheckedDowncast<CompiledStructure>(structure) : nullptr;
    };
    const ClassTable& classes = m_state.objects->classes;
    for (uint32_t structureID : usualStructureIDs()) {
        const CompiledStructure* structure = structureBelowAwaitedPrototype(structureID);
        if (!structure)
            continue;
        const LayoutData& classLayout = classes.classOfInstances(structure->layout());
        if (ClassTable::isClassOfCreatedObjects(classLayout) && !classes.classPrototypeIfExists(classLayout))
            continue;
        dataLogLn("AOT: the objects of layout ", structure->layout().index, ", shape ", structure->layout().shape, ", are still born below the object that stands for an awaited prototype, and their class awaits none");
        crashOnValidationFailure(vm, { ValidationFailureKind::OperationPreconditionBroken, { }, { }, structure });
    }
    std::span<AllocationSiteRecord> records = allocationSiteRecords();
    for (const AllocationSiteRecord& record : records) {
        const CompiledStructure* structure = structureBelowAwaitedPrototype(static_cast<uint32_t>(record.header));
        if (!structure)
            continue;
        dataLogLn("AOT: record ", &record - records.data(), " of the allocation sites names the Structure below the object that stands for an awaited prototype, of layout ", structure->layout().index, ", shape ", structure->layout().shape);
        crashOnValidationFailure(vm, { ValidationFailureKind::OperationPreconditionBroken, { }, { }, structure });
    }
}

// The records of the places of a class of created objects name what its objects are born with, and it has one layout. While no object of the class has been made, and until one has left the
// Structure below m_awaitedPrototype, no record names a Structure of the class: the table has the only entry.
void StructureTable::forgetStructuresOfCreatedObjects(VM& vm, const LayoutData& classLayout)
{
    ASSERT(ClassTable::isClassOfCreatedObjects(classLayout));
    uint32_t& known = m_structures[classLayout.shape];
    bool hasNoRecord = !known || isBelowAwaitedPrototype(*VariantMaps::decode(std::bit_cast<StructureID>(known)));
    if (hasNoRecord) {
        known = 0;
        if (!validatesRuntime(vm)) [[likely]]
            return;
    }
    if (forgetStructuresOf(classLayout) && hasNoRecord) [[unlikely]] {
        dataLogLn("AOT: a record of an allocation site, or the table of Structures, named a Structure of layout ", classLayout.index, " before any object had left the one that its objects are born with");
        crashOnValidationFailure(vm, { ValidationFailureKind::OperationPreconditionBroken, { }, { }, JSValue() });
    }
}

bool StructureTable::forgetStructuresOf(const LayoutData& classLayout)
{
    // The shapes of the layouts that were derived from the class are numbered wherever the tests of the program want them.
    const ClassTable& classes = m_state.objects->classes;
    auto belongsToClass = [&](uint32_t shape) {
        return m_tables.hasLayoutForShape(shape) && classes.classOfInstances(m_tables.layoutForShape(shape)).index == classLayout.index;
    };
    bool hasForgotten = false;
    for (uint32_t shape : m_shapesWithStructure) {
        if (m_structures[shape] && belongsToClass(shape)) {
            m_structures[shape] = 0;
            hasForgotten = true;
        }
    }

    // Allocations that share a record have the same shape, which the record stores once it is filled.
    for (AllocationSiteRecord& record : std::span { m_state.instance.objects.emptyAllocationSite() - m_numberOfAllocationSiteRecords, m_numberOfAllocationSiteRecords }) {
        uint32_t shape = shapeWordOf(static_cast<uint32_t>(record.orderAndShapeWords >> 32));
        if (record.header && shape < m_tables.firstEnvironmentShape && belongsToClass(shape)) {
            record = emptyAllocationSiteRecord();
            hasForgotten = true;
        }
    }
    return hasForgotten;
}

static Structure* structureOf(const AllocationSiteRecord& record)
{
    return std::bit_cast<StructureID>(static_cast<uint32_t>(record.header)).decode();
}

ALWAYS_INLINE AllocationSiteRecord& StructureTable::filledRecord(VM& vm, uint32_t number)
{
    AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (record && record->header) [[likely]]
        return *record;
    return fillAllocationSite(vm, number);
}

JSCell* StructureTable::allocate(VM& vm, uint32_t number)
{
    Structure* structure = structureOf(filledRecord(vm, number));
    const AllocationData& allocation = m_tables.allocations()[number];
    switch (allocatedCellKindOf(m_tables, allocation.shape)) {
    case AllocatedCellKind::Object: {
        Object* object = Object::create(vm, structure, m_tables.layoutForShape(allocation.shape), allocation.size(), allocation.orderWord());
        object->setPerCellBit(allocation.isBornUnfinished);
        return object;
    }
    case AllocatedCellKind::Closure:
        return Closure::createForCompiledCode(vm, structure, allocation.size());
    case AllocatedCellKind::Environment:
        return Environment::create(vm, structure, m_tables.environmentForShape(allocation.shape), allocation.size());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Compiled code goes on behind the operation as behind its own allocation: it writes every slot of the object that is not to hold zero before anybody else can see one.
JSCell* StructureTable::allocateAsCompiledCodeDoes(VM& vm, uint32_t number)
{
    static_assert(offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && sizeof(JSCell) == sizeof(uint64_t));
    const AllocationData& allocation = m_tables.allocations()[number];
    if (allocation.shape >= m_tables.firstEnvironmentShape)
        return allocate(vm, number);
    const AllocationSiteRecord record = filledRecord(vm, number);
    Structure* structure = structureOf(record);
    auto* words = static_cast<uint64_t*>(vm.cellSpace().allocate(vm, allocation.size(), nullptr, AllocationFailureMode::Assert));
    words[0] = record.header;
    words[1] = record.orderAndShapeWords;
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(words + 2), allocation.size() - 2 * sizeof(uint64_t) });
    ensureStillAliveHere(structure);
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
    AOT_THRESHOLD(size_t, smallestRoom, 16);
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
            m_heldKeyOrder.add(HeldKeyOrder::IsBesideExtraProperties);
    }
    if (!parameters.hasOtherPrototype && !parameters.hasExtraProperties && parameters.prototype == JSValue(parameters.realm->objectPrototype()))
        m_flags.add(Flag::HasOnlyFieldsBelowObjectPrototype);
    setMayBePrototype(parameters.mayBePrototype);
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
            m_heldKeyOrder.add(HeldKeyOrder::IsOfOneObject);
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
    if (const RareData* rareData = structure->rareData())
        visitor.append(rareData->heldExtraProperties);
    visitor.append(structure->m_successorByPrivateName);
}

template void CompiledStructure::visitAdditionalChildren(JSCell*, AbstractSlotVisitor&);
template void CompiledStructure::visitAdditionalChildren(JSCell*, SlotVisitor&);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
