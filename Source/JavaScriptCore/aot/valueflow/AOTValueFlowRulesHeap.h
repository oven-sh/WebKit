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
// PrototypeLink: the set of the key with every object above it, and every object whose properties were copied onto one of those. A placeholder above an object is listed, and nothing that lies above the placeholder.
// Of the untracked object alone: the chains of all objects that the analysis has stopped following.
// Property, Elements, EveryProperty: what a read under that name through the set may give. The getters it finds are called.
// Getter, GetterUnderUnknownKey, EveryGetter and the same three of setters: the accessors that a read or a store under a name, an index or a key nobody knows finds. An accessor or a data property
// of the table stands there for the function behind it, and the unknown object for an accessor nobody knows.
// A placeholder in the key is looked at by itself. The unknown object answers the unknown value. The untracked object is an object of the program, so it answers by name: whatever lies under the name on any of them.
// No lookup answers a member of the table that lets in unseen code: a read of seen code finds it by itself.
void runLookup(const ValueFlowStatement&, ValueFlowRuleContext&);

// What whoever holds one of the objects reaches without anybody having written it: the prototype object of a function that can construct, and the function that a prototype object names as its constructor.
// What lies above an object is not among these: whoever holds an instance finds through it what lies on the prototype object of its class, and native code reaches the built-in objects by itself.
ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext&, ClimbingSet objects);
// What lies right above the objects, none of which is a placeholder: what they inherit from, and not the objects themselves.
ClimbingSet objectsRightAbove(ValueFlowRuleContext&, ClimbingSet knownObjects);

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
    // Of TakesDescriptorMap, where the map is an object literal that goes nowhere else: all the names that it holds a descriptor under. Of AssignsFrom, where the source is such a literal: all its names.
    std::optional<std::span<const PropertyKeyID>> namesInMap { };
    // Of TakesDescriptorMap: the literal holds descriptors under names that it computes, too.
    bool mapHasComputedNames { false };
};
void applyPropertyAction(ValueFlowRuleContext&, const BuiltinPropertyAction&, const ValueFlowPropertyActionOperands&);

// For the results, after the solve: the getters (Get) and setters (Set) that a heap operation of a reached function may run: closures, accessors of the table, the untracked object
// where a function may run that the analysis has stopped following, and the unknown object where an accessor nobody knows may run.
// The rule runs once more on the finished values, so the context needs a log of its own, which is thrown away.
using ValueFlowAccessorFunctor = ScopedLambda<void(ValueFlowObjectID function, ValueFlowCallKind)>;
void forEachAccessorRunBy(const ValueFlowStatement&, ValueFlowRuleContext&, const ValueFlowAccessorFunctor&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
