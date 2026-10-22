/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTSelectTypedResultsTargets.h"
#include "AOTValueFlow.h"
#include "AOTValueFlowHeapTables.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
class ValueFlowUniverse;
class ValueSetMaker;
class ValueSets;

// The code of the program that native code runs on every object that reaches an operand, whichever call the object came through.
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
    void makeSets(const Program&, const ValueFlowUniverse&, ValueSets&, const ParallelLoops&, NOESCAPE const ScopedLambda<void()>& runBesideSets);

    // What a value of the type may be. A part that stands for an operand, or for what an operand holds, admits everything.
    ValueSetID maskOf(BuiltinTypeID type) const { return m_maskOfType[type.index()]; }
    // As a value: the primitives that native code makes of the type. The literals of a type are listed, of any other string only the kind.
    ValueSetID primitivesOf(BuiltinTypeID type) const { return m_primitivesOfType[type.index()]; }
    // The class and every class of the table below it (BuiltinTable::isSameOrSubclassOf()), in ascending order.
    std::span<const BuiltinClassID> classesAtOrBelow(BuiltinClassID builtinClass) const LIFETIME_BOUND { return m_classesAtOrBelow[builtinClass.index()].span(); }
    // What is sure to be of the type: a test for the type never lets it go on to the next test.
    ValueSetID membersSurelyOf(BuiltinTypeID type) const { return m_membersSurelyOfType[type.index()]; }
    // What is sure to be of the type for as long as it has the behavior it was made with: the instances where the type asks for pristine ones.
    ValueSetID membersOfWhilePristine(BuiltinTypeID type) const { return m_membersOfTypeWhilePristine[type.index()]; }
    // What stands for the class under ValueFlowNameKind::AllocatedAs: its prototype object, where it has one that no other class has.
    ValueFlowObjectID objectStandingFor(BuiltinClassID builtinClass) const { return m_objectStandingForClass[builtinClass.index()]; }
    // The same two for what a built-in has allocated: what stands for the classes whose instances are sure to be of the type, and for those whose instances are while they are pristine.
    ValueSetID classesSurelyOf(BuiltinTypeID type) const { return m_classesSurelyOfType[type.index()]; }
    ValueSetID classesOfWhilePristine(BuiltinTypeID type) const { return m_classesOfTypeWhilePristine[type.index()]; }

    // None for a parameter, or an operand of an operation, without protocols that run code of the program.
    ValueFlowBehaviorID behaviorOf(const BuiltinParameter&) const;
    ValueFlowBehaviorID behaviorOf(const OperandInfo&) const;
    const ValueFlowBehavior& at(ValueFlowBehaviorID behavior) const LIFETIME_BOUND { return m_behaviors[behavior.index()]; }
    unsigned numberOfBehaviors() const { return m_behaviors.size(); }
    // An object is converted to a primitive, and nothing else happens to it.
    ValueFlowBehaviorID conversionToPrimitive() const { return m_conversionToPrimitive; }

    // The functions whose result a type can shape, as JSON.parse, and the names under which the table lists them.
    bool hasTypedResult(BuiltinID function) const { return m_functionsWithTypedResults.get(function.index()); }
    std::span<const BuiltinName> namesOfFunctionsWithTypedResults() const LIFETIME_BOUND { return m_namesOfFunctionsWithTypedResults.span(); }
    const HeadLayoutsOfFamilies& headLayoutsOfFamilies() const LIFETIME_BOUND { return m_headLayoutsOfFamilies; }

    // What the entry holds comes from code of the library that is in the program: a row of the entry itself names the function, without a condition, and says that the function or what a call of it answers is the value.
    bool holdsValueFromLibrary(BuiltinID entry) const { return m_entriesHoldingValueFromLibrary.get(entry.index()); }
    // The functions of the library that are such values and that the table names.
    ValueSetID closuresThatEntriesDescribe() const { return m_closuresThatEntriesDescribe; }
    // None for any other object.
    BuiltinID entryDescribing(ValueFlowObjectID closure) const;
    // See isCodeThatRecordsDescribe() in AOTValueFlowBuiltins.h.
    bool isCodeThatRecordsDescribe(FunctionID function) const { return m_codeThatRecordsDescribe.get(function.index()); }

    const ValueFlowHeapTables& heap() const LIFETIME_BOUND { return *m_heap; }

private:
    ValueFlowBehaviorID intern(const ValueFlowBehavior&);

    Vector<ValueSetID> m_maskOfType;
    Vector<ValueSetID> m_membersSurelyOfType;
    Vector<ValueSetID> m_membersOfTypeWhilePristine;
    Vector<ValueFlowObjectID> m_objectStandingForClass;
    Vector<ValueSetID> m_classesSurelyOfType;
    Vector<ValueSetID> m_classesOfTypeWhilePristine;
    Vector<ValueSetID> m_primitivesOfType;
    Vector<Vector<BuiltinClassID, 1>> m_classesAtOrBelow;
    Vector<ValueFlowBehavior> m_behaviors;
    HashMap<const BuiltinParameter*, ValueFlowBehaviorID> m_behaviorOfParameter;
    HashMap<const OperandInfo*, ValueFlowBehaviorID> m_behaviorOfOperand;
    ValueFlowBehaviorID m_conversionToPrimitive;
    BitVector m_functionsWithTypedResults;
    Vector<BuiltinName> m_namesOfFunctionsWithTypedResults;
    BitVector m_entriesHoldingValueFromLibrary;
    BitVector m_codeThatRecordsDescribe;
    struct DescribedClosure {
        ValueFlowObjectID closure;
        BuiltinID entry;
    };
    Vector<DescribedClosure> m_describedClosures;
    ValueSetID m_closuresThatEntriesDescribe;
    HeadLayoutsOfFamilies m_headLayoutsOfFamilies;
    std::unique_ptr<ValueFlowHeapTables> m_heap;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
