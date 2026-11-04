/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include "AOTSchemaRecords.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;
class ParallelLoops;

namespace Typed {

class Program;
class TypeArena;

constexpr bool isEnvironmentKind(LayoutKind kind) { return isEnvironmentKindOf(kind); }

// How much of what the written classes above a layout declare the layout lists behind its own fields.
enum class InheritedFields : uint8_t {
    // appendInheritedFields() has not come to the layout.
    NotTaken,
    // What the layout above lists, or nothing where the layout has no room for that (maxFieldsInOrder). The row declares fields that the layout does not list, so findField() goes on up the row.
    SomeOfRow,
    AllOfRow,
};

struct Layout : LayoutRecord {
    bool has(LayoutFlags flag) const { return flags.contains(flag); }
    bool isEnvironment() const { return isEnvironmentKind(kind); }
    bool isDerived() const { return has(LayoutFlags::IsDerived); }

    std::optional<uint32_t> findField(PropertyKeyID) const;
    // The first of that key.
    std::optional<uint32_t> findStaticField(PropertyKeyID) const;
    void indexFields();

    uint32_t appendField(Field&&);

    // `function F() {}` whose prototype object inherits from the one of `class A extends Array`, and every class below F: `new F()` makes an ordinary object, `Reflect.construct(A, [], F)` an array.
    bool mayBeOrdinaryObject { false };
    InheritedFields inheritedFields { InheritedFields::NotTaken };
    // `const counts = {}; counts[word] = 1`: noteLayoutsOfDictionaries(). Plan gives a layout LayoutStorage::OrdinaryObject only where this is set.
    bool holdsOrdinaryObjects { false };

private:
    struct IndexedField {
        PropertyKeyID key;
        uint32_t field { 0 };
    };

    static uint32_t hashOf(PropertyKeyID key)
    {
        uint32_t hash = key.index() * 0x9e3779b1u;
        return hash ^ (hash >> 15);
    }
    void addToIndex(uint32_t field);

    Vector<IndexedField> m_fieldsByKey;
    Vector<IndexedField> m_staticFieldsByKey;
    uint64_t m_keysByHash { 0 };
    uint32_t m_indexedFieldCount { 0 };
    uint32_t m_indexedStaticFieldCount { 0 };
};

inline std::optional<uint32_t> Layout::findField(PropertyKeyID key) const
{
    RELEASE_ASSERT(m_indexedFieldCount == fields.size());
    if (!key)
        return std::nullopt;
    uint32_t hash = hashOf(key);
    if (!(m_keysByHash & (1ull << (hash & 63))))
        return std::nullopt;
    if (m_fieldsByKey.isEmpty()) {
        for (uint32_t i = 0; i < fields.size(); ++i) {
            if (fields[i].key == key)
                return i;
        }
        return std::nullopt;
    }
    uint32_t mask = static_cast<uint32_t>(m_fieldsByKey.size()) - 1;
    for (uint32_t place = (hash >> 6) & mask;; place = (place + 1) & mask) {
        const IndexedField& indexed = m_fieldsByKey[place];
        if (indexed.key == key)
            return indexed.field;
        if (!indexed.key)
            return std::nullopt;
    }
}

struct FieldReference {
    LayoutID layout;
    uint32_t field { 0 };

    friend bool operator==(const FieldReference&, const FieldReference&) = default;
    friend auto operator<=>(const FieldReference&, const FieldReference&) = default;
};

const Field& fieldAt(const Program&, FieldReference);
FieldReference declarationOf(const Program&, FieldReference);
// Along the classes as they are written. That a class has the base it was written with is Facts' to prove: FactTables' class bases.
std::optional<FieldReference> findField(const Program&, LayoutID, PropertyKeyID);
LayoutID writtenBaseOf(const Layout&);
// Only compiled code makes the objects of the layout, and a test of the shape word tells them from every other object: none is a proxy, an ordinary object or a cell of a built-in class.
inline bool isFoundByShapeTest(const Layout& layout)
{
    return (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance || layout.kind == LayoutKind::Container) && !layout.writtenBuiltinBase && !layout.mayBeOrdinaryObject && !layout.holdsOrdinaryObjects;
}

// `class A extends Map`, as the scan of bodies finds it at a NewClass without a written base layout whose heritage is a built-in constant, or a Convert to the type of one built-in.
struct ClassBelowBuiltinClass {
    LayoutID layout;
    BuiltinClassID builtinClass;

    friend constexpr auto operator<=>(const ClassBelowBuiltinClass&, const ClassBelowBuiltinClass&) = default;
};
// The class whose instances `new` of the built-in makes, if a subclass of it has cells of that class: null for Object, for functions, for wrappers of primitives and for what is no constructor of a class.
BuiltinClassID classMadeByHeritage(const BuiltinTable&, BuiltinID heritage);
// At the barrier between the scan and the solve, by one thread. Sets Layout::writtenBuiltinBase and mayBeOrdinaryObject of the classes named and of every class below them.
// It reflects the source, so it never changes afterwards. Whatever a TypeArena remembered before is stale: TypeArena::forgetResults().
// Objects of such a class are cells of the built-in class. They have no shape word, so no test tells them from other instances of the built-in class:
// TypeArena::testablePartOf() and describedByShape() put the class in place of the layout.
void noteSubclassesOfBuiltinClasses(Program&, std::span<const ClassBelowBuiltinClass> sortedWithoutRepeats);

// Behind runFactPasses() and in front of planProgram(), by one thread. Whatever a TypeArena remembered before is stale: TypeArena::forgetResults().
// Sets Layout::holdsOrdinaryObjects of the layouts named: LayoutFact::ObjectsAreDictionaries.
// Objects of such a layout are ordinary objects. They have no slots and no shape word, so no test tells them from other ordinary objects:
// TypeArena::testablePartOf() and describedByShape() put BuiltinClasses::Object in place of the layout.
// The solve runs with the word clear, so a filter whose type names a layout lets through the objects of that layout alone. Behind this call the guard of such a type lets any object through.
// A guard may be stronger than its filter, never weaker: computeDictionaries() says yes for no layout that a type of the wire's list names.
void noteLayoutsOfDictionaries(Program&, const BitVector& layoutsOfDictionaries);

// The type that a position can be held to: the declared type in the words of tests. A by-name store is tested at run time, and any object passes for a class that has no instance test.
// Facts starts the type of a slot of an object from this (FieldFacts::slotType), and Plan takes it where Facts says nothing.
// An allocation tests nothing. Where one lists a value that is not proven to fit and that cannot be stored behind the allocation, Facts widens the slot past this type, to TypeTable::anything: computeValuesStoredApart().
TypeID enforceableTypeOf(TypeArena&, const DeclaredType&);

Layout createDerivedLayout(const Program&, LayoutID base);
LayoutID declaredLayoutOf(const Program&, LayoutID);

void initializeLayouts(Program&, BuildContext&);
void appendInheritedFields(Program&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
