/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTValueFlow.h"
#include "AOTValueFlowHeapTables.h"
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
class ValueFlowUniverse;
class ValueSetMaker;

// The code of the program that native code runs on every object that reaches an operand, whichever call the object came through. All operands with the same behavior share one node, the hub
// of the behavior, and one statement.
struct ValueFlowBehavior {
    Protocols protocols;
    // The protocols also run on whatever an object in the hub holds and on whatever they hand out.
    bool isTransitive { false };
    std::span<const BuiltinName> namedMembers;
    std::span<const BuiltinName> calledMembers;
};

// What one solve computes from the table of built-ins before its first round. Nothing in it moves afterwards, so every thread may read it.
class ValueFlowBuiltinTables {
    WTF_MAKE_NONCOPYABLE(ValueFlowBuiltinTables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowBuiltinTables);
public:
    // The behaviors and the lists by name come from the table alone: the bodies are reduced with them. The sets wait for the universe, which is numbered after the bodies.
    explicit ValueFlowBuiltinTables(const BuiltinTable&);
    void makeSets(const Program&, const ValueFlowUniverse&, ValueSetMaker&);

    // What a value of the type may be. A part that stands for an operand, or for what an operand holds, admits everything.
    ValueSetID maskOf(BuiltinTypeID type) const { return m_maskOfType[type.index()]; }
    // As a value: the primitives that native code makes of the type. The literals of a type are listed, of any other string only the kind.
    ValueSetID primitivesOf(BuiltinTypeID type) const { return m_primitivesOfType[type.index()]; }
    // What is sure to be of the type: a test for the type never lets it go on to the next test.
    ValueSetID membersSurelyOf(BuiltinTypeID type) const { return m_membersSurelyOfType[type.index()]; }
    // What is sure to be of the type for as long as it has the behavior it was made with: the instances where the type asks for pristine ones.
    ValueSetID membersOfWhilePristine(BuiltinTypeID type) const { return m_membersOfTypeWhilePristine[type.index()]; }

    // None for a parameter, or an operand of an operation, without protocols that run code of the program.
    ValueFlowBehaviorID behaviorOf(const BuiltinParameter&) const;
    ValueFlowBehaviorID behaviorOf(const OperandInfo&) const;
    const ValueFlowBehavior& at(ValueFlowBehaviorID behavior) const LIFETIME_BOUND { return m_behaviors[behavior.index()]; }
    unsigned numberOfBehaviors() const { return m_behaviors.size(); }
    // An object is converted to a primitive, and nothing else happens to it.
    ValueFlowBehaviorID conversionToPrimitive() const { return m_conversionToPrimitive; }

    const ValueFlowHeapTables& heap() const LIFETIME_BOUND { return *m_heap; }

private:
    ValueFlowBehaviorID intern(const ValueFlowBehavior&);

    Vector<ValueSetID> m_maskOfType;
    Vector<ValueSetID> m_membersSurelyOfType;
    Vector<ValueSetID> m_membersOfTypeWhilePristine;
    Vector<ValueSetID> m_primitivesOfType;
    Vector<ValueFlowBehavior> m_behaviors;
    HashMap<const BuiltinParameter*, ValueFlowBehaviorID> m_behaviorOfParameter;
    HashMap<const OperandInfo*, ValueFlowBehaviorID> m_behaviorOfOperand;
    ValueFlowBehaviorID m_conversionToPrimitive;
    std::unique_ptr<ValueFlowHeapTables> m_heap;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
