/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class VM;

namespace AOT {

class Closure;

// The record of a class names a variable for each static field that the class declares under a constant name (FieldData::slot).
// While the class object is not made (Closure::isLazyClassObject()), a field that has been defined lies there, as a plain data property would: writable, enumerable, configurable.
// An empty variable says that the field does not lie in it. Making the class object moves the fields onto it, behind its methods and in the order of the record, and empties the variables for good.
// So a class object that is made has its static fields as plain properties and nowhere else, and compiled code names none of these variables.
namespace StaticFields {

// Empty where no field of that name lies in its variable.
JSValue tryRead(const Closure& classObject, UniquedStringImpl*);
// A store to a field that lies in its variable.
bool tryStore(Closure& classObject, UniquedStringImpl*, JSValue);
// The definition of a field that lies in its variable with a descriptor that leaves it plain. Without a value the field keeps the one that it has.
bool tryRedefine(Closure& classObject, UniquedStringImpl*, JSValue valueOrEmpty);
// The definition of a plain data property under the name of a field that the class object lacks.
bool tryCreate(Closure& classObject, UniquedStringImpl*, JSValue);
// For whoever has just given the class object its prototype property and its methods.
void moveToProperties(VM&, Closure& classObject);

} // namespace StaticFields

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
