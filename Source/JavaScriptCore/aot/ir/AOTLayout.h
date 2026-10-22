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
#include <optional>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;
struct Value;

constexpr bool isEnvironmentKind(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return true;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline constexpr uint32_t maxFieldsAlwaysInline = 14;

struct Layout : LayoutRecord {
    bool has(LayoutFlags flag) const { return flags.contains(flag); }
    bool isEnvironment() const { return isEnvironmentKind(kind); }
    bool isDerived() const { return has(LayoutFlags::IsDerived); }
    // Placement may move a field that is created late into the Extension, which only a store that may allocate can create.
    bool mayKeepFieldsOutOfLine() const { return fields.size() > maxFieldsAlwaysInline; }

    std::optional<uint32_t> findField(PropertyKeyID) const;
    void indexFields();

    uint32_t appendField(Field&&);

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

    // Open addressing: a power of two of places, at most half of them taken. A free place has no key.
    Vector<IndexedField> m_fieldsByKey;
    // One bit for each key, by its hash: most layouts that are asked for a name do not have it.
    uint64_t m_keysByHash { 0 };
    uint32_t m_indexedFieldCount { 0 };
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

enum class BaseChain : uint8_t { Proven, Predicted };

const Field& fieldAt(const Program&, FieldReference);
FieldReference declarationOf(const Program&, FieldReference);
const FieldFlow& flowOf(const Program&, FieldReference);
// What flow has proven of the field in every object of the layout for the whole life of the object.
bool isNeverRedefined(const Program&, FieldReference);
bool isNeverDeleted(const Program&, FieldReference);
// No store takes the property out of the slot. That undefined and null are never stored is more, and only FieldStability::IsNeverStoredWithOtherType says it.
bool isNeverMovedOutByStore(const Program&, FieldReference);
// The type of the field is only expected: nothing tests a store against it, and there is no value that the slot would turn away. A slot that defers a closure holds objects alone.
bool slotTakesEveryValue(const Program&, const Field&);
// An empty slot says that the object has no such property.
bool staysInSlot(const Program&, FieldReference);
bool staysFilled(const Program&, FieldReference);
bool isFilledAtBirth(const Program&, FieldReference);
bool keepsStateOfBirth(const Program&, FieldReference);
// With something that may be another value than undefined and null.
bool isFilledWithValueAtBirth(const Program&, FieldReference);
bool hasFieldsFilledAtBirth(const Program&, LayoutID);
// Flow proves that nothing fills the field: no store reaches it in an object of a layout that holds it. A declared method that nothing overwrites stays on the prototype.
bool isNeverStored(const Program&, FieldReference);
std::optional<FieldReference> findField(const Program&, LayoutID, PropertyKeyID, BaseChain);
// The class whose prototype object the prototype object of this class is expected to inherit from.
LayoutID expectedBaseOf(const Layout&);
// The class that the heritage of a class is expected to name: the instances hold its fields, its constructor allocates them, and the class object inherits from its class object.
// A constructor function has no heritage: `base` names the class to whose prototype object seen code links the prototype object of the function, and says nothing else.
LayoutID predictedHeritageOf(const Layout&);
// The built-in class at the end of the proven chain of bases. Its constructor makes the instances, so they are objects of that class and have no slots.
BuiltinClassID builtinBaseClassOf(const Program&, LayoutID);
// A built-in or unknown base class allocates the instances itself, and they have no slots.
bool hasOnlyCompiledBases(const Program&, LayoutID);
// Whether the run time knows the only class object. NewClass enters that of a class. No operation enters the closure of a constructor function:
// the closure table creates it, and a closure with captures is one that only the program can create.
bool isEvaluatedOnce(const Program&, const Layout&);
// No operation defines the class of a constructor function. The first construction does, where the function is created once; the guards of any other constructor function are never set.
bool guardsAreNeverSet(const Program&, const Layout&);
// A function gets its prototype property at the first lookup of `prototype` or at its first construction. The guards of a constructor function describe the prototype object
// of the first closure that got one, so a second closure that gets one ends them. A function that is created once has no second closure, and NewClass reports a class.
bool prototypePropertyMayEndGuards(const Program&, FunctionID);
// The function object of such a constructor function is the cell of the closure table, whoever needs it first: NewClosure, an allocation site of its instances or a class below it.
bool hasSingletonClosure(const Program&, FunctionID);
// No new object: the cell of the closure table for the function, which the table fills where it is empty. So reads a method as a value: the run time makes the closure of a declared method
// by the table, and a guard of a member that the program stored itself is set only once the table holds that closure.
bool namesClosureOfTable(const Program&, const Value& closure);

// A derived layout is a declared layout plus the names that the objects of some allocation classes get beside its fields. It has the declared layout as its proven base
// and no class of its own: its objects have the prototype, the members and the brand of the layout it was derived from.
Layout createDerivedLayout(const Program&, LayoutID base);
LayoutID declaredLayoutOf(const Program&, LayoutID);

// The object types that the program declares for the objects of the layout: what their class implements, and the `this` of their constructor function. Each entry holds the layouts below one of them.
Vector<LayoutSetID, 2> declaredFamiliesOf(const Program&, LayoutID);
// A type is a view: where nothing allocates it, it stands in no set of layouts, not even in its own, so the families do not hold the names that the types have.
// The records whose sets list a layout, ascending: the views through which the program may see its objects. Answers by index, for the declared layouts whose bit is set.
Vector<Vector<LayoutID>> declaredViewsOf(const Program&, const BitVector& declaredLayouts);

// Leaves the tables of the program that go by class and by name to the caller: they leave out the layouts that are never allocated.
void initializeLayouts(Program&, BuildContext&);
void initializeAddedLayout(Program&, LayoutID);
void appendInheritedFields(Program&, LayoutID);

// For a program as read. An object type that the program declares is a layout, and its family holds what the program allocates and converts to it. The layout itself has objects
// only if something creates them: an operation, a constant or an expected result that names it, or a built-in that creates what its result is converted to.
// The families keep the layouts that are marked here, and whoever goes through a family leaves them out.
void markUnallocatedLayouts(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
