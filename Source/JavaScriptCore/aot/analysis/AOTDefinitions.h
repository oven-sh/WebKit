/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <optional>
#include <span>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class UseLists;

// The record of a class has the key, the kind and the function of a member. It creates the closure as the function is written, over nothing or over the environment of the class.
bool canClassRecordCreate(const Program&, const Function& constructor, const ClassMember&);

// The record of such a class says all that its definition does.
bool isDefinedByClassRecord(const Program&, LayoutID);
// The constructor of the built-in class that the class is proven to extend, where the run time can find that object whenever it makes the class. None otherwise.
BuiltinID builtinParentNamedByClassRecord(const Program&, const Layout&);

// One definition of such a class, as it stands in a function.
struct ClassDefinitionSite {
    LayoutID layout;
    // NewClass. Whatever else holds the class object hands this value on.
    ValueID classObject;
    // What the record stands for: the closures of the constructor and of the members, the definitions of the members, the look at the heritage.
    Vector<ValueID, 8> describedByRecord;
    // FinishClass and the casts of the class object, each behind the value that it hands on.
    Vector<ValueID, 2> copies;
    // The operations that store the class object or a copy into a variable.
    Vector<ValueID, 2> stores;
};

// Nothing where an operation of the definition does more than the record says: it names a function by a computed key, hands a closure to something else, defines a member that flow has not listed.
std::optional<ClassDefinitionSite> findClassDefinitionSite(const Program&, const Function&, const UseLists&, ValueID newClass);

enum class DefinitionKind : uint8_t { Function, Class, RegExp, TemplateObject, ClassToken, Object, Array };
inline constexpr unsigned numberOfDefinitionKinds = 7;

ASCIILiteral nameOf(DefinitionKind);

// An allocation in a module body, outside its loops, that does nothing but allocate and whose operands are constants and definitions.
struct Definition {
    static constexpr uint32_t noHolder = UINT32_MAX;

    DefinitionKind kind { DefinitionKind::Function };
    FunctionID moduleBody;
    ValueID allocation;
    // The first binding that is stored once, with this. A later one is another name of the same definition.
    VariableID binding;
    // The definition that has this one as a member.
    uint32_t holder { noHolder };
    uint32_t usesThatGoPast { 0 };
    uint32_t observingUsesInModuleBodies { 0 };
    uint32_t observingUsesInFunctions { 0 };
    // The first operation of a module body that needs the object. None: a terminator or an edge.
    std::optional<Opcode> observerInModuleBody;

    bool isObserved() const { return observingUsesInModuleBodies || observingUsesInFunctions; }
};

// Reads the program as the front end wrote it and asks no flow analysis.
class Definitions {
public:
    Definitions(const Program&, BuildContext&);

    std::span<const Definition> all() const LIFETIME_BOUND { return m_definitions.span(); }
    // A member is made with the object that holds it unless that object has a compiled layout, so it counts as observed where its holder is.
    bool isMadeAtStart(const Definition&) const;
    bool isEverMade(const Definition&) const;

    JS_EXPORT_PRIVATE void dump(PrintStream&, const Program&) const;
    JS_EXPORT_PRIVATE void dumpCounts(PrintStream&) const;

private:
    Vector<Definition> m_definitions;
    // For the allocations of module bodies that are no definitions: how often an operand made by each operation was the first one that the build does not know.
    Vector<uint32_t> m_unknownOperands;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
