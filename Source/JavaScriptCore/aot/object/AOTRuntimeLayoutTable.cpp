/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeLayoutTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTCompiledCallee.h"
#include "AOTFieldTable.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTObject.h"
#include "AOTPrimitives.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "GetterSetter.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "PropertyName.h"
#include "Symbol.h"
#include <algorithm>
#include <bit>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

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
    case PropertyKeyKind::PrivateName:
        return FieldKeyKind::PrivateName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const ImageText& text()
{
    return Image::current()->text();
}

static bool matchesAnyLiteral(std::span<const uint32_t> literals, StringView characters)
{
    const ImageText& imageText = text();
    return std::ranges::any_of(literals, [&](uint32_t literal) {
        return imageText.view(StringID(literal)).length() == characters.length() && imageText.equals(StringID(literal), characters);
    });
}

static NEVER_INLINE bool ropeMatchesAnyLiteral(JSGlobalObject* globalObject, std::span<const uint32_t> literals, JSString* rope)
{
    unsigned length = rope->length();
    const ImageText& imageText = text();
    bool hasLiteralWithLength = std::ranges::any_of(literals, [&](uint32_t literal) {
        return imageText.view(StringID(literal)).length() == length;
    });
    if (!hasLiteralWithLength)
        return false;
    if (rope->isSubstring()) {
        auto characters = rope->view(globalObject);
        return matchesAnyLiteral(literals, characters.data);
    }
    if (rope->is8Bit()) {
        Vector<Latin1Character, 64> characters(length);
        rope->resolveToBuffer(characters.mutableSpan());
        return matchesAnyLiteral(literals, StringView(characters.span()));
    }
    Vector<char16_t, 64> characters(length);
    rope->resolveToBuffer(characters.mutableSpan());
    return matchesAnyLiteral(literals, StringView(characters.span()));
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

static bool stringHasType(JSGlobalObject* globalObject, const RuntimeType& type, JSString* value)
{
    OptionSet<PrimitiveSet> primitives = type.primitiveSet();
    auto literals = type.literalStrings();
    if (literals.empty())
        return primitives.contains(PrimitiveSet::String);
    if (!primitives.containsAny({ PrimitiveSet::String, PrimitiveSet::LiteralString }))
        return false;
    if (StringImpl* impl = value->tryGetValueImpl())
        return matchesAnyLiteral(literals, StringView(*impl));
    return ropeMatchesAnyLiteral(globalObject, literals, value);
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
    if (FieldTableEntry::hasColumn(*number) && layout.shape && layout.shape <= m_tables.numberOfFieldTableRows) {
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

// A store under the name of a removed field creates an ordinary property, which breaks no proof: the proof is that nothing reads the field.
void RuntimeLayoutTable::failIfFieldWasRemoved(VM& vm, const LayoutData& layout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(name);
    if (!number || !std::ranges::binary_search(m_tables.removedFields(), RemovedFieldData { layout.index, *number }))
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
    return keyKindOf(text().identifier(field.name).kind()) == FieldKeyKind::Symbol;
}

bool RuntimeLayoutTable::hasStringKey(const FieldData& field) const
{
    return keyKindOf(text().identifier(field.name).kind()) == FieldKeyKind::String && !field.isPrivate();
}

bool RuntimeLayoutTable::hasPositionAmongKeys(const LayoutData& layout, UniquedStringImpl* name) const
{
    return !name->isSymbol() || (!static_cast<SymbolImpl*>(name)->isPrivate() && layout.hasFieldsWithSymbolKeys);
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
        return stringHasType(m_state.instance.globalObject, type, asString(cell));
    if (cell->isSymbol()) {
        if (primitives.contains(PrimitiveSet::Symbol))
            return true;
    } else if (!cell->isObject())
        return false;
    return objectHasType(type, cell);
}

bool RuntimeLayoutTable::objectHasType(const RuntimeType& type, JSCell* cell) const
{
    // A member that holds one of many functions has a test for each of them.
    bool hasAskedForFunction = false;
    std::optional<FunctionID> function;
    for (const RuntimeCellTest& test : type.cellTests()) {
        if (test.kind != CellTestKind::IsClosureOf) {
            if (cellPassesTest(test, cell))
                return true;
            continue;
        }
        if (!std::exchange(hasAskedForFunction, true))
            function = functionOf(cell);
        if (function == test.function())
            return true;
    }
    return false;
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
        return functionOf(cell) == test.function();
    case CellTestKind::IsSymbol:
        // A symbol that nothing has made yet is the uid of no cell.
        return cell->isSymbol() && &asSymbol(cell)->uid() == m_state.instance.vm->aotState().identifiers->tryGetIdentifierConcurrently(test.symbolKeyNumber());
    case CellTestKind::SharingModeIs:
        return hasSharingMode(cell, test.sharingMode());
    case CellTestKind::IsNamespaceOf: {
        auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
        return moduleNamespace && moduleNamespace->hasNamespaceLayoutOf(test.module());
    }
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
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
