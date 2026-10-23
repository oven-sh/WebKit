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

bool isHeapOperation(Opcode);
void runHeapOperation(const ValueFlowStatement&, ValueFlowRuleContext&);

// A statement of kind Lookup.
void runLookup(const ValueFlowStatement&, ValueFlowRuleContext&);

// What whoever holds one of the objects reaches without anybody having written it: the prototype object of a function that can construct, and the function that a prototype object names as its constructor.
ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext&, ClimbingSet objects);
// What lies right above the objects, none of which is a placeholder: what they inherit from, and not the objects themselves.
ClimbingSet objectsRightAbove(ValueFlowRuleContext&, ClimbingSet knownObjects);
// Holds every export of the modules of the library that the operand lists beside the unknown object, where a function of the program walks the operand. No node where it lists none.
ValueFlowNodeID exportsOfHoldersListedBesideUnknownObject(ValueFlowRuleContext&, const ValueFlowOperand&);

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

// For the results, after the solve: the getters (Get) and setters (Set) that a heap operation of a reached function may run.
using ValueFlowAccessorFunctor = ScopedLambda<void(ValueFlowObjectID function, ValueFlowCallKind)>;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
