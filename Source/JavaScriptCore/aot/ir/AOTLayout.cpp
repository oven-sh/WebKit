/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLayout.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned minFieldsToIndex = 4;
static constexpr unsigned minStaticFieldsToIndex = 8;

void Layout::indexFields()
{
    m_indexedFieldCount = static_cast<uint32_t>(fields.size());
    m_fieldsByKey.clear();
    m_keysByHash = 0;
    if (fields.size() >= minFieldsToIndex)
        m_fieldsByKey.fill(IndexedField { }, WTF::roundUpToPowerOfTwo(2 * fields.size()));
    for (uint32_t i = 0; i < fields.size(); ++i)
        addToIndex(i);

    m_indexedStaticFieldCount = static_cast<uint32_t>(staticFields.size());
    m_staticFieldsByKey.clear();
    if (staticFields.size() < minStaticFieldsToIndex)
        return;
    m_staticFieldsByKey.fill(IndexedField { }, staticFieldTableSize());
    uint32_t mask = staticFieldTableSize() - 1;
    for (uint32_t i = 0; i < staticFields.size(); ++i) {
        PropertyKeyID key = staticFields[i].key;
        bool isInTable = false;
        for (uint32_t place = hashOf(key) & mask; key; place = (place + 1) & mask) {
            IndexedField& indexed = m_staticFieldsByKey[place];
            if (indexed.key == key)
                break;
            if (!indexed.key) {
                indexed = { key, i };
                isInTable = true;
                break;
            }
        }
        if (!isInTable)
            m_staticFieldsByKey.append(IndexedField { key, i });
    }
    AOT::stableSort(m_staticFieldsByKey.mutableSpan().subspan(staticFieldTableSize()), { }, &IndexedField::key);
}

Vector<uint32_t, 4> Layout::indexedStaticFieldsThatMayMatch(PropertyKeyID key) const
{
    Vector<uint32_t, 4> found;
    if (std::optional<uint32_t> first = findStaticField(key))
        found.append(*first);
    for (const IndexedField& other : std::ranges::equal_range(staticFieldsOutsideTable(), key, { }, &IndexedField::key))
        found.append(other.field);
    std::ranges::sort(found);
    return found;
}

std::optional<uint32_t> Layout::findStaticField(PropertyKeyID key) const
{
    RELEASE_ASSERT(m_indexedStaticFieldCount == staticFields.size());
    if (!key)
        return std::nullopt;
    if (m_staticFieldsByKey.isEmpty()) {
        for (uint32_t i = 0; i < staticFields.size(); ++i) {
            if (staticFields[i].key == key)
                return i;
        }
        return std::nullopt;
    }
    uint32_t mask = staticFieldTableSize() - 1;
    for (uint32_t place = hashOf(key) & mask;; place = (place + 1) & mask) {
        const IndexedField& indexed = m_staticFieldsByKey[place];
        if (indexed.key == key)
            return indexed.field;
        if (!indexed.key)
            return std::nullopt;
    }
}

void Layout::addToIndex(uint32_t field)
{
    PropertyKeyID key = fields[field].key;
    if (!key)
        return;
    uint32_t hash = hashOf(key);
    m_keysByHash |= 1ull << (hash & 63);
    if (m_fieldsByKey.isEmpty())
        return;
    uint32_t mask = static_cast<uint32_t>(m_fieldsByKey.size()) - 1;
    for (uint32_t place = (hash >> 6) & mask;; place = (place + 1) & mask) {
        IndexedField& indexed = m_fieldsByKey[place];
        if (indexed.key == key)
            return;
        if (!indexed.key) {
            indexed = { key, field };
            return;
        }
    }
}

void Layout::appendCopiesOfAllFieldsOf(LayoutID id, const Layout& base)
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size() && base.m_indexedFieldCount == base.fields.size());
    auto numberOfOwnFields = static_cast<uint32_t>(fields.size());
    fields.appendVector(base.fields);
    uint32_t numberDeclaredByBase = base.copiesWholeFieldListOfBase ? base.ownFieldCount : static_cast<uint32_t>(base.fields.size());
    for (uint32_t index = 0; index < numberDeclaredByBase; ++index) {
        Field& copy = fields[numberOfOwnFields + index];
        copy.flags.add(FieldFlags::IsInherited);
        copy.originLayout = id;
        copy.originField = index;
    }
    if (base.m_fieldsByKey.isEmpty() || 2 * fields.size() > base.m_fieldsByKey.size()) {
        indexFields();
        return;
    }
    m_fieldsByKey = base.m_fieldsByKey;
    for (IndexedField& indexed : m_fieldsByKey)
        indexed.field += indexed.key ? numberOfOwnFields : 0;
    m_keysByHash = base.m_keysByHash;
    m_indexedFieldCount = static_cast<uint32_t>(fields.size());
    for (uint32_t index = 0; index < numberOfOwnFields; ++index)
        addToIndex(index);
}

uint32_t Layout::appendField(Field&& field)
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size());
    uint32_t index = static_cast<uint32_t>(fields.size());
    copiesWholeFieldListOfBase = false;
    fields.append(WTF::move(field));
    if (fields.size() >= minFieldsToIndex && 2 * fields.size() > m_fieldsByKey.size()) {
        indexFields();
        return index;
    }
    ++m_indexedFieldCount;
    addToIndex(index);
    return index;
}

namespace {

class RowOfWrittenBases {
public:
    RowOfWrittenBases(const Program& program, LayoutID first)
        : m_program(program)
        , m_current(first)
        , m_trailing(first)
    {
    }

    LayoutID current() const { return m_current; }

    void advance()
    {
        m_current = writtenBaseOf(m_program.at(m_current));
        if (m_trailingMovesNext)
            m_trailing = writtenBaseOf(m_program.at(m_trailing));
        m_trailingMovesNext = !m_trailingMovesNext;
        if (m_current == m_trailing)
            m_current = { };
    }

private:
    const Program& m_program;
    LayoutID m_current;
    LayoutID m_trailing;
    bool m_trailingMovesNext { false };
};

} // anonymous namespace

std::optional<FieldReference> findFieldAboveInRow(const Program& program, LayoutID id, PropertyKeyID key)
{
    RowOfWrittenBases row(program, id);
    for (row.advance(); row.current(); row.advance()) {
        const Layout& layout = program.at(row.current());
        if (auto field = layout.findField(key))
            return FieldReference { row.current(), *field };
        if (layout.inheritedFields == InheritedFields::AllOfRow)
            break;
    }
    return std::nullopt;
}

BuiltinClassID classMadeByHeritage(const BuiltinTable& builtins, BuiltinID heritage)
{
    if (!heritage)
        return { };
    BuiltinID constructor = builtins.canonicalID(heritage);
    BuiltinClassID instanceClass = builtins.at(constructor).instanceClass;
    if (!instanceClass || instanceClass == BuiltinClasses::Object)
        return { };
    const BuiltinClass& made = builtins.at(instanceClass);
    if (made.hasClosureInstances || made.isPrimitiveWrapper || isFunctionClass(builtins, instanceClass))
        return { };
    if (!made.constructor || builtins.canonicalID(made.constructor) != constructor)
        return { };
    return instanceClass;
}

void noteSubclassesOfBuiltinClasses(Program& program, std::span<const ClassBelowBuiltinClass> sortedWithoutRepeats)
{
    program.types.invalidateMemosAtRest();
    if (sortedWithoutRepeats.empty())
        return;

    size_t numberOfLayouts = program.layouts.size();
    Vector<BuiltinClassID> classRightAbove(numberOfLayouts);
    for (const ClassBelowBuiltinClass& written : sortedWithoutRepeats) {
        BuiltinClassID& builtinClass = classRightAbove[written.layout.index()];
        RELEASE_ASSERT(written.builtinClass && (!builtinClass || builtinClass == written.builtinClass));
        builtinClass = written.builtinClass;
    }
    auto extendsBuiltinClass = [&](LayoutID id) {
        return !!classRightAbove[id.index()];
    };
    auto layoutAbove = [&](LayoutID id) {
        return writtenBaseOf(program.at(id));
    };

    Vector<LayoutID> nearestAbove = findNearestWantedAbove(numberOfLayouts, layoutAbove, extendsBuiltinClass);
    AnswersAlongRows liesAtOrBelowConstructorFunction(numberOfLayouts, false);
    auto askConstructorFunction = [&](LayoutID id) -> StepAlongRow {
        LayoutID above = layoutAbove(id);
        if (extendsBuiltinClass(id) || !above)
            return { false, { } };
        if (program.at(id).has(LayoutFlags::IsConstructorFunction))
            return { true, { } };
        return { std::nullopt, above };
    };

    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        BuiltinClassID builtinClass = classRightAbove[index];
        if (LayoutID nearest = nearestAbove[index]; !builtinClass && nearest)
            builtinClass = classRightAbove[nearest.index()];
        Layout& layout = program.layouts[index];
        layout.writtenBuiltinBase = builtinClass;
        layout.mayBeOrdinaryObject = builtinClass && liesAtOrBelowConstructorFunction.answerFor(LayoutID(index), askConstructorFunction);
        if (!builtinClass)
            continue;
        program.someClassExtendsArray |= builtinClass == BuiltinClasses::Array;
        program.someClassExtendsIterable |= builtinClass && (builtinClass == BuiltinClasses::Array || builtinClass == BuiltinClasses::Map || builtinClass == BuiltinClasses::Set || isTypedView(program.builtins().typedArrayTypeOf(builtinClass)));
        program.someClassExtendsView |= isTypedView(program.builtins().typedArrayTypeOf(builtinClass));
    }
}

void noteSubclassesOfMixins(Program& program, std::span<const ClassBelowBuiltinClass> sortedWithoutRepeats)
{
    BitVector hasSeveralSubclasses(program.layouts.size());
    for (const ClassBelowBuiltinClass& written : sortedWithoutRepeats) {
        Layout& layout = program.layouts[written.layout.index()];
        if (layout.writtenBuiltinBase || !layout.base)
            continue;
        LayoutID mixin = layout.base;
        while (LayoutID above = program.at(mixin).base)
            mixin = above;
        Layout& topmost = program.layouts[mixin.index()];
        if (topmost.writtenBuiltinBase || !topmost.has(LayoutFlags::HasHeritage) || layout.mixedBuiltinBase)
            continue;
        layout.mixedBuiltinBase = written.builtinClass;
        if (topmost.mixedSubclass)
            hasSeveralSubclasses.quickSet(mixin.index());
        topmost.mixedSubclass = written.layout;
        program.someClassExtendsMixin = true;
    }
    hasSeveralSubclasses.forEachSetBit([&](size_t mixin) {
        program.layouts[mixin].mixedSubclass = { };
    });
    if (!program.someClassExtendsMixin)
        return;
    auto isTopmostOfMixin = [](const Layout& layout) {
        return layout.kind == LayoutKind::Instance && layout.has(LayoutFlags::HasHeritage) && !layout.base && !layout.writtenBuiltinBase;
    };
    Vector<uint32_t, 4> withSubclass;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].mixedSubclass)
            withSubclass.append(index);
    }
    for (Layout& copy : program.layouts) {
        if (!isTopmostOfMixin(copy) || copy.mixedSubclass || !copy.name)
            continue;
        LayoutID subclass;
        for (uint32_t index : withSubclass) {
            const Layout& mixin = program.layouts[index];
            if (mixin.name != copy.name || mixin.module != copy.module)
                continue;
            if (subclass) {
                subclass = { };
                break;
            }
            subclass = mixin.mixedSubclass;
        }
        copy.mixedSubclass = subclass;
    }
}

void noteLayoutsOfDictionaries(Program& program, const BitVector& layoutsOfDictionaries)
{
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Layout& layout = program.layouts[index];
        layout.holdsOrdinaryObjects = layoutsOfDictionaries.get(index);
        RELEASE_ASSERT(!layout.holdsOrdinaryObjects || !layout.writtenBuiltinBase);
    }
    program.types.invalidateMemosAtRest();
}

TypeID enforceableTypeOf(TypeArena& types, const DeclaredType& declared)
{
    if (declared.isExpected())
        return types.table().provenTypeOf(declared);
    return types.testablePartOf(declared.written());
}

Layout createDerivedLayout(const Program& program, LayoutID base)
{
    const Layout& declared = program.at(base);
    Layout layout;
    layout.kind = declared.kind;
    layout.name = declared.name;
    layout.module = declared.module;
    layout.position = declared.position;
    layout.base = base;
    layout.flags = LayoutFlags::IsDerived;
    layout.writtenBuiltinBase = declared.writtenBuiltinBase;
    layout.mixedBuiltinBase = declared.mixedBuiltinBase;
    layout.mayBeOrdinaryObject = declared.mayBeOrdinaryObject;
    return layout;
}

void initializeLayouts(Program& program, BuildContext& build)
{
    forEachIndexInParallel(build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        Layout& layout = program.layouts[index];
        if (!layout.ownFieldCount) {
            auto firstInherited = std::ranges::find_if(layout.fields, [](const Field& field) {
                return field.flags.contains(FieldFlags::IsInherited);
            });
            layout.ownFieldCount = static_cast<uint32_t>(firstInherited - layout.fields.begin());
        }
        layout.indexFields();
    });
}

static std::optional<FieldReference> declarationOfFieldTakenFromWrittenBase(const Program& program, LayoutID id, FieldReference fieldOfBase)
{
    if (program.at(id).findField(fieldAt(program, fieldOfBase).key))
        return std::nullopt;
    FieldReference declaration = declarationOf(program, fieldOfBase);
    if (declaration.layout == id)
        return std::nullopt;
    return declaration;
}

static bool isCopyOfFieldOfWrittenBase(const Program& program, LayoutID id, uint32_t place, FieldReference fieldOfBase)
{
    const Layout& layout = program.at(id);
    if (place >= layout.fields.size())
        return false;
    const Field& copy = layout.fields[place];
    const Field& copied = fieldAt(program, fieldOfBase);
    FieldReference declaration = declarationOf(program, fieldOfBase);
    OptionSet<FieldFlags> flagsOfCopy = copied.flags;
    flagsOfCopy.add(FieldFlags::IsInherited);
    return copy.flags == flagsOfCopy && copy.originLayout == declaration.layout && copy.originField == declaration.field
        && copy.key == copied.key && copy.computedKey == copied.computedKey && copy.form == copied.form && copy.declared == copied.declared && copy.variableName == copied.variableName
        && (!copy.key || layout.findField(copy.key) == place);
}

static bool hasRoomForFieldsOfWrittenBase(const Program& program, LayoutID id, LayoutID base)
{
    const Layout& layout = program.at(id);
    size_t numberOfFields = layout.fields.size();
    auto numberOfFieldsOfBase = static_cast<uint32_t>(program.at(base).fields.size());
    if (!isObjectLayoutOf(layout.kind) || numberOfFields + numberOfFieldsOfBase <= maxFieldsInOrder)
        return true;
    for (uint32_t index = 0; index < numberOfFieldsOfBase; ++index) {
        if (declarationOfFieldTakenFromWrittenBase(program, id, { base, index }))
            ++numberOfFields;
    }
    return numberOfFields <= maxFieldsInOrder;
}

static void appendFieldsOfWrittenBase(Program& program, LayoutID id)
{
    Layout& layout = program.at(id);
    if (layout.inheritedFields != InheritedFields::NotTaken)
        return;
    layout.inheritedFields = InheritedFields::SomeOfRow;
    LayoutID base = writtenBaseOf(layout);
    if (!hasRoomForFieldsOfWrittenBase(program, id, base))
        return;

    const Layout& baseLayout = program.at(base);
    bool baseListsOwnFieldsAndDirectCopiesAlone = baseLayout.copiesWholeFieldListOfBase || (!writtenBaseOf(baseLayout) && baseLayout.ownFieldCount == baseLayout.fields.size());
    if (baseListsOwnFieldsAndDirectCopiesAlone && layout.fields.size() == layout.ownFieldCount && std::ranges::none_of(layout.fields, [&](const Field& field) { return baseLayout.findField(field.key).has_value(); })) {
        layout.appendCopiesOfAllFieldsOf(base, baseLayout);
        layout.copiesWholeFieldListOfBase = true;
        if (!writtenBaseOf(baseLayout) || baseLayout.inheritedFields == InheritedFields::AllOfRow)
            layout.inheritedFields = InheritedFields::AllOfRow;
        return;
    }
    bool listsFieldsOfBaseInOrder = true;
    for (uint32_t index = 0; index < baseLayout.fields.size(); ++index) {
        uint32_t place = layout.ownFieldCount + index;
        std::optional<FieldReference> declaration = declarationOfFieldTakenFromWrittenBase(program, id, { base, index });
        if (!declaration) {
            listsFieldsOfBaseInOrder = listsFieldsOfBaseInOrder && isCopyOfFieldOfWrittenBase(program, id, place, { base, index });
            continue;
        }
        listsFieldsOfBaseInOrder = listsFieldsOfBaseInOrder && layout.fields.size() == place;
        Field field = baseLayout.fields[index];
        field.flags.add(FieldFlags::IsInherited);
        field.originLayout = declaration->layout;
        field.originField = declaration->field;
        layout.appendField(WTF::move(field));
    }
    layout.copiesWholeFieldListOfBase = listsFieldsOfBaseInOrder && layout.fields.size() - layout.ownFieldCount == baseLayout.fields.size();
    if (!writtenBaseOf(baseLayout) || baseLayout.inheritedFields == InheritedFields::AllOfRow)
        layout.inheritedFields = InheritedFields::AllOfRow;
}

static void appendFieldsOfSeveralParents(Program& program, LayoutID id)
{
    if (program.at(id).inheritedFields != InheritedFields::NotTaken)
        return;
    Vector<LayoutID, 4> parents;
    forEachOfSeveralParents(program, id, [&](LayoutID parent) {
        parents.append(parent);
    });
    if (parents.isEmpty())
        return;
    program.at(id).inheritedFields = InheritedFields::SomeOfRow;
    size_t numberOfFields = program.at(id).fields.size();
    for (LayoutID parent : parents) {
        appendInheritedFields(program, parent);
        numberOfFields += program.at(parent).fields.size();
    }
    if (numberOfFields > maxFieldsInOrder)
        return;
    for (LayoutID parent : parents) {
        const Layout& above = program.at(parent);
        for (uint32_t index = 0; index < above.fields.size(); ++index) {
            std::optional<FieldReference> declaration = declarationOfFieldTakenFromWrittenBase(program, id, { parent, index });
            if (!declaration)
                continue;
            Field field = above.fields[index];
            field.flags.add(FieldFlags::IsInherited);
            field.flags.remove(FieldFlags::IsRequired);
            field.originLayout = declaration->layout;
            field.originField = declaration->field;
            program.at(id).appendField(WTF::move(field));
        }
    }
    program.at(id).inheritedFields = InheritedFields::AllOfRow;
}

void appendInheritedFields(Program& program, LayoutID id)
{
    Vector<LayoutID, 8> pending;
    for (RowOfWrittenBases row(program, id); row.current(); row.advance()) {
        const Layout& layout = program.at(row.current());
        appendFieldsOfSeveralParents(program, row.current());
        if (!writtenBaseOf(layout) || layout.inheritedFields != InheritedFields::NotTaken)
            break;
        pending.append(row.current());
    }
    while (!pending.isEmpty())
        appendFieldsOfWrittenBase(program, pending.takeLast());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
