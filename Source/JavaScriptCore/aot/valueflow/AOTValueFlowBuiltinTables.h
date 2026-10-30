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
#include "AOTVector.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

struct ValueFlowBehavior {
    Protocols protocols;
    bool isTransitive { false };
    std::span<const BuiltinName> namedMembers;
    std::span<const BuiltinName> calledMembers;
    bool describesRefusedOperands { false };
    // The walk over keys, values and getters passes functions by.
    bool namesFunctionsOnly { false };
    // Of the objects that it meets it converts those that Number, String and their like have made. A proxy is none of them.
    bool convertsOnlyBoxedPrimitives { false };
    // In place of a proxy it takes the target, and asks no trap.
    bool printsTargetsOfProxies { false };
};

// Whether an object that was made as an instance of one class, or of a class below it, may pass the test for another.
bool mayBeInstanceOf(const BuiltinTable&, BuiltinClassID madeAs, BuiltinClassID tested);

class ValueFlowBuiltinTables {
    WTF_MAKE_NONCOPYABLE(ValueFlowBuiltinTables);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowBuiltinTables);
public:
    ValueFlowBuiltinTables(const BuiltinTable&, uint64_t mostObjectsOfTypedResult);
    void makeSets(const Program&, const ValueFlowUniverse&, ValueSets&, const ParallelLoops&, NOESCAPE const ScopedLambda<void()>& runBesideSets);

    ValueSetID maskOf(BuiltinTypeID type) const { return m_maskOfType[type.index()]; }
    ValueSetID acceptedValues(const BuiltinParameter* parameter) const
    {
        if (!parameter || !parameter->accepted || parameter->conversion != BuiltinConversion::None)
            return ValueSets::anything;
        return maskOf(parameter->accepted);
    }
    ValueSetID primitivesOf(BuiltinTypeID type) const { return m_primitivesOfType[type.index()]; }
    std::span<const BuiltinClassID> classesAtOrBelow(BuiltinClassID builtinClass) const LIFETIME_BOUND { return m_classesAtOrBelow[builtinClass.index()].span(); }
    ValueSetID membersSurelyOf(BuiltinTypeID type) const { return m_membersSurelyOfType[type.index()]; }
    ValueSetID membersOfWhilePristine(BuiltinTypeID type) const { return m_membersOfTypeWhilePristine[type.index()]; }
    ValueFlowObjectID objectStandingFor(BuiltinClassID builtinClass) const { return m_objectStandingForClass[builtinClass.index()]; }
    ValueSetID classesSurelyOf(BuiltinTypeID type) const { return m_classesSurelyOfType[type.index()]; }
    ValueSetID classesOfWhilePristine(BuiltinTypeID type) const { return m_classesOfTypeWhilePristine[type.index()]; }
    // An object passes the test for the type only if it was made as one of these. `anything` where the class that an object was made as does not decide the test.
    ValueSetID classesThatMayBeOf(BuiltinTypeID type) const { return m_classesThatMayBeOfType[type.index()]; }
    ValueSetID objectsStandingForClasses() const { return m_objectsStandingForClasses; }

    ValueFlowBehaviorID behaviorOf(const BuiltinParameter& parameter) const { return m_behaviorOfParameter.get(&parameter); }
    ValueFlowBehaviorID behaviorOf(const OperandInfo& operand) const { return m_behaviorOfOperand.get(&operand); }
    // The same behavior for a call that may convert what it prints: a proxy is asked like any object.
    ValueFlowBehaviorID behaviorThatAsksProxies(ValueFlowBehaviorID behavior) const { return m_behaviorsThatAskProxies.get(behavior.index()); }
    const ValueFlowBehavior& at(ValueFlowBehaviorID behavior) const LIFETIME_BOUND { return m_behaviors[behavior.index()]; }
    unsigned numberOfBehaviors() const { return m_behaviors.size(); }
    ValueFlowBehaviorID conversionToPrimitive() const { return m_conversionToPrimitive; }
    ValueFlowBehaviorID listOfKeysThatTrapAnswers() const { return m_listOfKeysThatTrapAnswers; }
    ValueFlowBehaviorID descriptorThatTrapAnswers() const { return m_descriptorThatTrapAnswers; }
    ValueFlowBehaviorID descriptionOfRefusedOperands() const { return m_descriptionOfRefusedOperands; }
    ValueFlowBehaviorID formatterOfMessages() const { return m_formatterOfMessages; }

    bool hasTypedResult(BuiltinID function) const { return m_functionsWithTypedResults.get(function.index()); }
    std::span<const BuiltinName> namesOfFunctionsWithTypedResults() const LIFETIME_BOUND { return m_namesOfFunctionsWithTypedResults.span(); }
    const HeadLayoutsOfFamilies& headLayoutsOfFamilies() const LIFETIME_BOUND { return m_headLayoutsOfFamilies; }
    uint64_t mostObjectsOfTypedResult() const { return m_mostObjectsOfTypedResult; }

    bool holdsValueFromLibrary(BuiltinID entry) const { return m_entriesHoldingValueFromLibrary.get(entry.index()); }
    ValueSetID closuresThatEntriesDescribe() const { return m_closuresThatEntriesDescribe; }
    BuiltinID entryDescribing(ValueFlowObjectID closure) const;
    bool isCodeThatRecordsDescribe(FunctionID function) const { return m_codeThatRecordsDescribe.get(function.index()); }
    // Rows name the function as one that their entry calls, each of those entries is declared, and the host calls it for nothing else.
    bool isCalledForDeclaredEntriesAlone(RuntimeFunctionID function) const { return m_functionsCalledForDeclaredEntriesAlone.get(function.index()); }
    // What a call of the entry does to its operands the statements of its code say, as of any function of the program.
    bool isDescribedByCodeAlone(BuiltinID entry) const { return m_entriesDescribedByCodeAlone.get(entry.index()); }

    const ValueFlowHeapTables& heap() const LIFETIME_BOUND { return *m_heap; }

private:
    Vector<ValueSetID> m_maskOfType;
    Vector<ValueSetID> m_membersSurelyOfType;
    Vector<ValueSetID> m_membersOfTypeWhilePristine;
    Vector<ValueFlowObjectID> m_objectStandingForClass;
    Vector<ValueSetID> m_classesSurelyOfType;
    Vector<ValueSetID> m_classesOfTypeWhilePristine;
    Vector<ValueSetID> m_classesThatMayBeOfType;
    ValueSetID m_objectsStandingForClasses;
    Vector<ValueSetID> m_primitivesOfType;
    Vector<Vector<BuiltinClassID, 1>> m_classesAtOrBelow;
    Vector<ValueFlowBehavior> m_behaviors;
    HashMap<const BuiltinParameter*, ValueFlowBehaviorID> m_behaviorOfParameter;
    HashMap<const OperandInfo*, ValueFlowBehaviorID> m_behaviorOfOperand;
    HashMap<uint32_t, ValueFlowBehaviorID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_behaviorsThatAskProxies;
    ValueFlowBehaviorID m_conversionToPrimitive;
    ValueFlowBehaviorID m_descriptionOfRefusedOperands;
    std::array<BuiltinName, 1> m_namesReadFromRefusedOperands { BuiltinNames::constructor };
    ValueFlowBehaviorID m_listOfKeysThatTrapAnswers;
    ValueFlowBehaviorID m_descriptorThatTrapAnswers;
    ValueFlowBehaviorID m_formatterOfMessages;
    BitVector m_functionsWithTypedResults;
    Vector<BuiltinName> m_namesOfFunctionsWithTypedResults;
    BitVector m_entriesHoldingValueFromLibrary;
    BitVector m_codeThatRecordsDescribe;
    BitVector m_functionsCalledForDeclaredEntriesAlone;
    BitVector m_entriesDescribedByCodeAlone;
    Vector<std::pair<ValueFlowObjectID, BuiltinID>> m_describedClosures;
    ValueSetID m_closuresThatEntriesDescribe;
    HeadLayoutsOfFamilies m_headLayoutsOfFamilies;
    uint64_t m_mostObjectsOfTypedResult;
    std::unique_ptr<ValueFlowHeapTables> m_heap;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
