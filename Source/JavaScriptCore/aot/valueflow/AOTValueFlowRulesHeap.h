/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowRuleContext.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

} // namespace Typed

struct BuiltinPropertyAction;

namespace Typed {

// Reads, stores and definitions of properties, elements and private fields, accessors, the contents that objects and arrays are allocated with, copies of properties,
// and the marks on what is deleted from, redefined or given another prototype.
bool isHeapOperation(Opcode);
void runHeapOperation(const ValueFlowStatement&, ValueFlowRuleContext&);

// A statement of kind Lookup. What its result holds goes by the kind of the name in its key:
// PrototypeLink: the set of the key with every object above it, and every object whose properties were copied onto one of those.
// Property, Elements, EveryProperty: what a read under that name through the set may give. The getters it finds are called.
// Getter, GetterUnderUnknownKey, EveryGetter and the same three of setters: the accessors that a read or a store under a name, an index or a key nobody knows finds. An accessor or a data property
// of the table stands there for the function behind it, and the unknown object for an accessor nobody knows.
void runLookup(const ValueFlowStatement&, ValueFlowRuleContext&);

// What whoever holds one of the objects reaches without anybody having written it: the prototype object of a function that can construct, the function that a prototype object names as its
// constructor, the prototype object of the class of an instance. Built-in objects are left out: native code reaches those by itself.
ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext&, ClimbingSet objects);

// What an entry of the table does to the properties of an operand. An operand that is not there has no node and no constant.
struct ValueFlowPropertyActionOperands {
    ValueFlowOperand object;
    // Where the key is an operand: that operand. Where the keys are the elements of an operand: the lists.
    ValueFlowOperand key;
    // What is stored or defined; the descriptor or the map of descriptors; the source of an assignment; the prototype that the object gets; of BecomesPrototype the inheritors, if somebody follows them.
    ValueFlowOperand value;
    // Takes what ReadsByKey, ReadsGetter and ReadsSetter read. Of DescribesOwnProperties: the result.
    ValueFlowNodeID target;
    ValueFlowNodeID thrown;
};
void applyPropertyAction(ValueFlowRuleContext&, const BuiltinPropertyAction&, const ValueFlowPropertyActionOperands&);

// For the results, after the solve: the getters (Get) and setters (Set) that a heap operation of a reached function may run: closures, accessors of the table, and the unknown object
// where an accessor nobody knows may run. The rule runs once more on the finished values, so the context needs a log of its own, which is thrown away.
using ValueFlowAccessorFunctor = ScopedLambda<void(ValueFlowObjectID function, ValueFlowCallKind)>;
void forEachAccessorRunBy(const ValueFlowStatement&, ValueFlowRuleContext&, const ValueFlowAccessorFunctor&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
