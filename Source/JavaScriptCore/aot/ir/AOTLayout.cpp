/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLayout.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTRowsOfLayouts.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>
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
    m_staticFieldsByKey.fill(IndexedField { }, WTF::roundUpToPowerOfTwo(2 * staticFields.size()));
    uint32_t mask = static_cast<uint32_t>(m_staticFieldsByKey.size()) - 1;
    for (uint32_t i = 0; i < staticFields.size(); ++i) {
        PropertyKeyID key = staticFields[i].key;
        if (!key)
            continue;
        for (uint32_t place = hashOf(key) & mask;; place = (place + 1) & mask) {
            IndexedField& indexed = m_staticFieldsByKey[place];
            if (indexed.key == key)
                break;
            if (!indexed.key) {
                indexed = { key, i };
                break;
            }
        }
    }
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
    uint32_t mask = static_cast<uint32_t>(m_staticFieldsByKey.size()) - 1;
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

uint32_t Layout::appendField(Field&& field)
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size());
    uint32_t index = static_cast<uint32_t>(fields.size());
    fields.append(WTF::move(field));
    if (fields.size() >= minFieldsToIndex && 2 * fields.size() > m_fieldsByKey.size()) {
        indexFields();
        return index;
    }
    ++m_indexedFieldCount;
    addToIndex(index);
    return index;
}

const Field& fieldAt(const Program& program, FieldReference reference)
{
    return program.at(reference.layout).fields[reference.field];
}

FieldReference declarationOf(const Program& program, FieldReference reference)
{
    for (size_t steps = 0; fieldAt(program, reference).flags.contains(FieldFlags::IsInherited); ++steps) {
        RELEASE_ASSERT(steps < program.layouts.size());
        const Field& field = fieldAt(program, reference);
        reference = { field.originLayout, *field.originField };
    }
    return reference;
}

LayoutID writtenBaseOf(const Layout& layout)
{
    return layout.base;
}

namespace {

// `class A extends B {}` beside `class B extends A {}` compiles, and throws when it runs. A row that comes back to where it has been ends there, behind a look at each of its layouts.
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
    // Half as far up the row.
    LayoutID m_trailing;
    bool m_trailingMovesNext { false };
};

} // anonymous namespace

std::optional<FieldReference> findField(const Program& program, LayoutID id, PropertyKeyID key)
{
    for (RowOfWrittenBases row(program, id); row.current(); row.advance()) {
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
        // The cells of a layout are of one class.
        RELEASE_ASSERT(written.builtinClass && (!builtinClass || builtinClass == written.builtinClass));
        builtinClass = written.builtinClass;
    }
    auto extendsBuiltinClass = [&](LayoutID id) {
        return !!classRightAbove[id.index()];
    };
    auto layoutAbove = [&](LayoutID id) {
        return writtenBaseOf(program.at(id));
    };

    // In a ring, and on the way into one, the layout named need not extend a built-in class.
    Vector<LayoutID> nearestAbove = findNearestWantedAbove(numberOfLayouts, layoutAbove, extendsBuiltinClass);
    BitVector liesAtOrBelowConstructorFunction = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        LayoutID above = layoutAbove(id);
        if (extendsBuiltinClass(id) || !above)
            return { false, { } };
        if (program.at(id).has(LayoutFlags::IsConstructorFunction))
            return { true, { } };
        return { std::nullopt, above };
    });

    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        BuiltinClassID builtinClass = classRightAbove[index];
        if (LayoutID nearest = nearestAbove[index]; !builtinClass && nearest)
            builtinClass = classRightAbove[nearest.index()];
        Layout& layout = program.layouts[index];
        layout.writtenBuiltinBase = builtinClass;
        layout.mayBeOrdinaryObject = builtinClass && liesAtOrBelowConstructorFunction.quickGet(index);
    }
}

void noteLayoutsOfDictionaries(Program& program, const BitVector& layoutsOfDictionaries)
{
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        Layout& layout = program.layouts[index];
        layout.holdsOrdinaryObjects = layoutsOfDictionaries.get(index);
        // The cells of a layout are of one class.
        RELEASE_ASSERT(!layout.holdsOrdinaryObjects || !layout.writtenBuiltinBase);
    }
    program.types.invalidateMemosAtRest();
}

TypeID enforceableTypeOf(TypeArena& types, const DeclaredType& declared)
{
    // An expected position takes whatever arrives, and data of the image may lie there that no test has seen.
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
    layout.mayBeOrdinaryObject = declared.mayBeOrdinaryObject;
    return layout;
}

LayoutID declaredLayoutOf(const Program& program, LayoutID id)
{
    LayoutID current = id;
    while (current && program.at(current).isDerived())
        current = writtenBaseOf(program.at(current));
    return current;
}

void initializeLayouts(Program& program, BuildContext& build)
{
    forEachIndexInParallel(build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        Layout& layout = program.layouts[index];
        // As RecordChecker::didRead() counts them: zero may be the count of a layout whose fields all come from its bases.
        if (!layout.ownFieldCount) {
            auto firstInherited = std::ranges::find_if(layout.fields, [](const Field& field) {
                return field.flags.contains(FieldFlags::IsInherited);
            });
            layout.ownFieldCount = static_cast<uint32_t>(firstInherited - layout.fields.begin());
        }
        layout.indexFields();
    });
}

// None: the layout lists a field under the key, or a ring of written bases has brought the field back to the layout that declares it.
static std::optional<FieldReference> declarationOfFieldTakenFromWrittenBase(const Program& program, LayoutID id, FieldReference fieldOfBase)
{
    if (program.at(id).findField(fieldAt(program, fieldOfBase).key))
        return std::nullopt;
    FieldReference declaration = declarationOf(program, fieldOfBase);
    if (declaration.layout == id)
        return std::nullopt;
    return declaration;
}

// The order word of an object counts its fields in twelve bits.
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
    // Facts counts the same fields and proves no base for a class without room for them.
    if (!hasRoomForFieldsOfWrittenBase(program, id, base))
        return;

    const Layout& baseLayout = program.at(base);
    // In the order of the base: a tagged layout has no fields of its own and is allocated under the field indexes of its maker.
    for (uint32_t index = 0; index < baseLayout.fields.size(); ++index) {
        std::optional<FieldReference> declaration = declarationOfFieldTakenFromWrittenBase(program, id, { base, index });
        if (!declaration)
            continue;
        Field field = baseLayout.fields[index];
        field.flags.add(FieldFlags::IsInherited);
        field.originLayout = declaration->layout;
        field.originField = declaration->field;
        layout.appendField(WTF::move(field));
    }
    if (!writtenBaseOf(baseLayout) || baseLayout.inheritedFields == InheritedFields::AllOfRow)
        layout.inheritedFields = InheritedFields::AllOfRow;
}

void appendInheritedFields(Program& program, LayoutID id)
{
    Vector<LayoutID, 8> pending;
    for (RowOfWrittenBases row(program, id); row.current(); row.advance()) {
        const Layout& layout = program.at(row.current());
        if (!writtenBaseOf(layout) || layout.inheritedFields != InheritedFields::NotTaken)
            break;
        pending.append(row.current());
    }
    // A layout takes the fields of the layout above it, so that one has all of its own first.
    while (!pending.isEmpty())
        appendFieldsOfWrittenBase(program, pending.takeLast());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
