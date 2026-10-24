/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/PrintStream.h>

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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
