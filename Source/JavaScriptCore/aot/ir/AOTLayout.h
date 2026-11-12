/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTID.h"
#include "AOTSchemaEnums.h"
#include "AOTSchemaRecords.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

class BuildContext;
class BuiltinTable;
class ParallelLoops;

namespace Typed {

class Program;
class TypeArena;

constexpr bool isEnvironmentKind(LayoutKind kind) { return isEnvironmentKindOf(kind); }

enum class InheritedFields : uint8_t {
    NotTaken,
    SomeOfRow,
    AllOfRow,
};

struct Layout : LayoutRecord {
    bool has(LayoutFlags flag) const { return flags.contains(flag); }
    bool isEnvironment() const { return isEnvironmentKind(kind); }
    bool isDerived() const { return has(LayoutFlags::IsDerived); }

    std::optional<uint32_t> findField(PropertyKeyID) const;
    std::optional<uint32_t> findStaticField(PropertyKeyID) const;
    template<typename Functor> void forEachStaticFieldThatMayMatch(PropertyKeyID, const Functor&) const;
    void indexFields();

    uint32_t appendField(Field&&);
    void appendCopiesOfAllFieldsOf(LayoutID, const Layout&);

    bool mayBeOrdinaryObject { false };
    BuiltinClassID mixedBuiltinBase;
    LayoutID mixedSubclass;
    InheritedFields inheritedFields { InheritedFields::NotTaken };
    bool copiesWholeFieldListOfBase { false };
    bool fieldsChangedAfterLayoutsBelowTookThem { false };
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
    uint32_t staticFieldTableSize() const { return WTF::roundUpToPowerOfTwo(2 * static_cast<uint32_t>(staticFields.size())); }
    std::span<const IndexedField> staticFieldsOutsideTable() const { return m_staticFieldsByKey.span().subspan(staticFieldTableSize()); }
    Vector<uint32_t, 4> indexedStaticFieldsThatMayMatch(PropertyKeyID) const;

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
    std::span<const IndexedField> fieldsByKey = m_fieldsByKey.span();
    uint32_t mask = static_cast<uint32_t>(fieldsByKey.size()) - 1;
    for (uint32_t place = (hash >> 6) & mask;; place = (place + 1) & mask) {
        const IndexedField& indexed = fieldsByKey[place];
        if (indexed.key == key)
            return indexed.field;
        if (!indexed.key)
            return std::nullopt;
    }
}

template<typename Functor>
void Layout::forEachStaticFieldThatMayMatch(PropertyKeyID key, const Functor& functor) const
{
    RELEASE_ASSERT(m_indexedStaticFieldCount == staticFields.size());
    if (m_staticFieldsByKey.isEmpty()) {
        for (const StaticField& field : staticFields) {
            if (field.key == key)
                functor(field);
        }
        return;
    }
    for (uint32_t field : indexedStaticFieldsThatMayMatch(key))
        functor(staticFields[field]);
}

struct FieldReference {
    LayoutID layout;
    uint32_t field { 0 };

    friend bool operator==(const FieldReference&, const FieldReference&) = default;
    friend auto operator<=>(const FieldReference&, const FieldReference&) = default;
};

inline const Field& fieldAt(const Program&, FieldReference);
inline FieldReference declarationOf(const Program&, FieldReference);
inline std::optional<FieldReference> findField(const Program&, LayoutID, PropertyKeyID);
std::optional<FieldReference> findFieldAboveInRow(const Program&, LayoutID, PropertyKeyID);
inline LayoutID writtenBaseOf(const Layout& layout) { return layout.base; }
inline bool isDefinedByClass(const Layout& layout) { return layout.kind == LayoutKind::Instance && layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction); }
inline bool isFoundByShapeTest(const Layout& layout)
{
    return (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance || layout.kind == LayoutKind::Container) && !layout.writtenBuiltinBase && !layout.mixedBuiltinBase && !layout.mayBeOrdinaryObject && !layout.holdsOrdinaryObjects;
}
ALWAYS_INLINE BuiltinClassID builtinClassOfInstances(const Layout& layout) { return layout.writtenBuiltinBase ? layout.writtenBuiltinBase : layout.mixedBuiltinBase; }

ALWAYS_INLINE bool isKnownByStructureOfClass(const Layout& layout)
{
    if (layout.kind != LayoutKind::Instance || !builtinClassOfInstances(layout) || (layout.base && !layout.mixedBuiltinBase) || !isDefinedByClass(layout) || layout.isDerived())
        return false;
    return std::ranges::all_of(layout.fields, [&](const Field& field) {
        bool isBrand = field.flags.containsAny({ FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods });
        return field.key && !field.form.contains(FieldForm::IsAccessor) && (isBrand || !field.flags.contains(FieldFlags::IsHidden));
    });
}

inline bool isToldByStructureOfClass(const Field& field)
{
    return field.flags.contains(FieldFlags::IsRequired) && !field.flags.containsAny({ FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden });
}

struct ClassBelowBuiltinClass {
    LayoutID layout;
    BuiltinClassID builtinClass;

    friend constexpr auto operator<=>(const ClassBelowBuiltinClass&, const ClassBelowBuiltinClass&) = default;
};
BuiltinClassID classMadeByHeritage(const BuiltinTable&, BuiltinID heritage);
void noteSubclassesOfBuiltinClasses(Program&, std::span<const ClassBelowBuiltinClass> sortedWithoutRepeats);
void noteSubclassesOfMixins(Program&, std::span<const ClassBelowBuiltinClass> sortedWithoutRepeats);

void noteLayoutsOfDictionaries(Program&, const BitVector& layoutsOfDictionaries);

TypeID enforceableTypeOf(TypeArena&, const DeclaredType&);

Layout createDerivedLayout(const Program&, LayoutID base);
inline LayoutID declaredLayoutOf(const Program&, LayoutID);

void initializeLayouts(Program&, BuildContext&);
void appendInheritedFields(Program&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
