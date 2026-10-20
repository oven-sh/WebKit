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
    Vector<uint32_t> m_fieldsByKey;
    uint32_t m_indexedFieldCount { 0 };
};

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
// Flow proves that nothing fills the field: no store reaches it in an object of a layout that holds it. A declared method that nothing overwrites stays on the prototype.
bool isNeverStored(const Program&, FieldReference);
std::optional<FieldReference> findField(const Program&, LayoutID, PropertyKeyID, BaseChain);
// The class whose prototype object the prototype object of this class is expected to inherit from.
LayoutID expectedBaseOf(const Layout&);
// The class that the heritage of a class is expected to name: the instances hold its fields, its constructor allocates them, and the class object inherits from its class object.
// A constructor function has no heritage: `predictedBase` names the class to whose prototype object seen code links the prototype object of the function, and says nothing else.
LayoutID predictedHeritageOf(const Layout&);
// The built-in class at the end of the proven chain of bases. Its constructor makes the instances, so they are objects of that class and have no slots.
BuiltinClassID builtinBaseClassOf(const Program&, LayoutID);
// Whether the run time knows the only class object. NewClass enters that of a class. No operation enters the closure of a constructor function:
// the closure table creates it, and a closure with captures is one that only the program can create.
bool isEvaluatedOnce(const Program&, const Layout&);
// The function object of such a constructor function is the cell of the closure table, whoever needs it first: NewClosure, an allocation site of its instances or a class below it.
bool hasSingletonClosure(const Program&, FunctionID);

// A derived layout is a declared layout plus the names that the objects of some allocation classes get beside its fields. It has the declared layout as its proven base
// and no class of its own: its objects have the prototype, the members and the brand of the layout it was derived from.
Layout createDerivedLayout(const Program&, LayoutID base);
LayoutID declaredLayoutOf(const Program&, LayoutID);

// The object types that the program declares for the objects of the layout: what their class implements, and the `this` of their constructor function. Each entry holds the layouts below one of them.
Vector<LayoutSetID, 2> declaredFamiliesOf(const Program&, LayoutID);
bool isDeclaredIn(const Program&, std::span<const LayoutSetID> families, PropertyKeyID);

void initializeLayouts(Program&, BuildContext&);
void initializeAddedLayout(Program&, LayoutID);
void appendInheritedFields(Program&, LayoutID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
