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
};

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

    ValueFlowBehaviorID behaviorOf(const BuiltinParameter& parameter) const { return m_behaviorOfParameter.get(&parameter); }
    ValueFlowBehaviorID behaviorOf(const OperandInfo& operand) const { return m_behaviorOfOperand.get(&operand); }
    const ValueFlowBehavior& at(ValueFlowBehaviorID behavior) const LIFETIME_BOUND { return m_behaviors[behavior.index()]; }
    unsigned numberOfBehaviors() const { return m_behaviors.size(); }
    ValueFlowBehaviorID conversionToPrimitive() const { return m_conversionToPrimitive; }
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
    Vector<ValueSetID> m_primitivesOfType;
    Vector<Vector<BuiltinClassID, 1>> m_classesAtOrBelow;
    Vector<ValueFlowBehavior> m_behaviors;
    HashMap<const BuiltinParameter*, ValueFlowBehaviorID> m_behaviorOfParameter;
    HashMap<const OperandInfo*, ValueFlowBehaviorID> m_behaviorOfOperand;
    ValueFlowBehaviorID m_conversionToPrimitive;
    ValueFlowBehaviorID m_descriptionOfRefusedOperands;
    std::array<BuiltinName, 1> m_namesReadFromRefusedOperands { BuiltinNames::constructor };
    ValueFlowBehaviorID m_formatterOfMessages;
    BitVector m_functionsWithTypedResults;
    Vector<BuiltinName> m_namesOfFunctionsWithTypedResults;
    BitVector m_entriesHoldingValueFromLibrary;
    BitVector m_codeThatRecordsDescribe;
    BitVector m_entriesDescribedByCodeAlone;
    Vector<std::pair<ValueFlowObjectID, BuiltinID>> m_describedClosures;
    ValueSetID m_closuresThatEntriesDescribe;
    HeadLayoutsOfFamilies m_headLayoutsOfFamilies;
    uint64_t m_mostObjectsOfTypedResult;
    std::unique_ptr<ValueFlowHeapTables> m_heap;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
