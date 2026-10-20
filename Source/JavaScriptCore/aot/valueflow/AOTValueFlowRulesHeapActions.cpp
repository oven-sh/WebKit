/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

// The target finds what the source holds as it finds what it inherits. Getters of the source run once, here, and what they answer lies on the target under a key nobody knows.
void HeapRule::copyProperties(const ValueFlowOperand& target, const ValueFlowOperand& source, bool storesThroughSetters)
{
    ClimbingSet targets = m_context.valueOf(target);
    ClimbingSet sources = m_context.valueOf(source);
    connectThrough(m_universe.knownObjects(), source, m_context.written(known(targets), copiedFromName));
    if (mayBeUnknownObject(targets))
        connect(source, ValueFlowProgramNodes::escaped());

    ValueFlowNodeID copied = m_context.written(known(targets), writtenName(unknownHeapKey, HeapPropertyPart::Value));
    if (mayBeUnknownObject(sources))
        add(copied, unknownValue());
    if (m_context.meet(sources, m_universe.primitives()))
        add(copied, m_universe.anyPrimitive());
    callAccessors(m_context.valueOf(m_context.read(known(sources), foundName(unknownHeapKey, HeapPropertyPart::Getter))), ValueFlowCallKind::Get, { &source, { } }, nullptr, copied);

    // A setter takes what the source itself holds, or what a getter of the source has answered.
    if (storesThroughSetters) {
        callSetters(targets, unknownHeapKey, target, operandOf(m_context.read(known(sources), foundName(unknownHeapKey, HeapPropertyPart::Value))));
        callSetters(targets, unknownHeapKey, target, operandOf(m_context.read(known(targets), writtenName(unknownHeapKey, HeapPropertyPart::Value))));
    }

    noteWrittenMembers(targets, unknownHeapKey);
    noteTrapOfUnknownObject(sources, ValueFlowCallKind::Get);
    noteTrapOfUnknownObject(targets, ValueFlowCallKind::Set);
    if (storesThroughSetters)
        m_context.mark(m_context.restrict(targets, m_universe.objects()), { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation });
    else
        markDefinition(targets, unknownHeapKey, HeapPropertyPart::Value);
    mayThrow();
}

// The members of a descriptor are read like any property, and each may be there.
void HeapRule::defineAsDescribed(const ValueFlowOperand& object, const HeapKey& key, ClimbingSet descriptors)
{
    auto define = [&](HeapPropertyPart part, BuiltinName member) {
        this->define(object, key, part, operandOf(m_context.lookup(descriptors, ValueFlowName::property(m_program.keyOf(member)))));
    };
    define(HeapPropertyPart::Value, BuiltinNames::value);
    define(HeapPropertyPart::Getter, BuiltinNames::get);
    define(HeapPropertyPart::Setter, BuiltinNames::set);
}

void HeapRule::changePrototype(ClimbingSet objects, const ValueFlowOperand& prototype)
{
    write(objects, prototypeLinkName, prototype);
    m_context.mark(m_context.restrict(objects, m_universe.objects()), ObjectMutation::ChangePrototype);
    m_context.mark(m_context.restrict(m_context.valueOf(prototype), m_universe.objects()), ObjectMutation::BecomePrototype);
}

// The function behind an accessor of the table is an object that nobody follows.
void HeapRule::readAccessorFunctions(const ValueFlowOperand& object, const HeapKey& key, HeapPropertyPart part)
{
    ValueFlowNodeID found = m_context.lookup(m_context.valueOf(object), foundName(key, part));
    connect(m_context.masked(found, m_universe.callables()), m_statement.result);
    if (m_context.meet(m_context.valueOf(found), m_universe.builtinObjects()))
        add(m_statement.result, m_universe.unknownObjectAlone());
    add(m_statement.result, m_universe.undefinedValue());
}

void HeapRule::runPropertyAction(const BuiltinPropertyAction& action, const ValueFlowPropertyActionOperands& operands)
{
    const ValueFlowOperand& object = operands.object;
    const ValueFlowOperand& value = operands.value;
    auto forEachKey = [&](NOESCAPE const Invocable<void(const HeapKey&)> auto& functor) {
        forEachKeyOfAction(action.key, operands.key, functor);
    };

    switch (action.kind) {
    case BuiltinPropertyActionKind::ReadsByKey:
        forEachKey([&](const HeapKey& key) {
            read(object, key);
        });
        return;
    case BuiltinPropertyActionKind::StoresByKey:
        forEachKey([&](const HeapKey& key) {
            store(object, key, value);
        });
        return;
    case BuiltinPropertyActionKind::DefinesName:
        forEachKey([&](const HeapKey& key) {
            define(object, key, HeapPropertyPart::Value, value);
        });
        // The table does not say which attributes native code gives the property.
        m_context.mark(m_context.restrict(m_context.valueOf(object), m_universe.objects()), ObjectMutation::RedefineProperty);
        return;
    case BuiltinPropertyActionKind::DefinesGetter:
        forEachKey([&](const HeapKey& key) {
            define(object, key, HeapPropertyPart::Getter, value);
        });
        return;
    case BuiltinPropertyActionKind::DefinesSetter:
        forEachKey([&](const HeapKey& key) {
            define(object, key, HeapPropertyPart::Setter, value);
        });
        return;
    case BuiltinPropertyActionKind::DeletesByKey:
        forEachKey([&](const HeapKey& key) {
            remove(object, key);
        });
        return;
    case BuiltinPropertyActionKind::TestsByKey:
        forEachKey([&](const HeapKey& key) {
            test(object, key);
        });
        return;
    case BuiltinPropertyActionKind::ReadsGetter:
        forEachKey([&](const HeapKey& key) {
            readAccessorFunctions(object, key, HeapPropertyPart::Getter);
        });
        return;
    case BuiltinPropertyActionKind::ReadsSetter:
        forEachKey([&](const HeapKey& key) {
            readAccessorFunctions(object, key, HeapPropertyPart::Setter);
        });
        return;
    case BuiltinPropertyActionKind::AssignsFrom:
        copyProperties(object, value, true);
        return;
    case BuiltinPropertyActionKind::TakesDescriptor:
        forEachKey([&](const HeapKey& key) {
            defineAsDescribed(object, key, m_context.valueOf(value));
        });
        return;
    case BuiltinPropertyActionKind::TakesDescriptorMap:
        defineAsDescribed(object, unknownHeapKey, m_context.valueOf(m_context.lookup(m_context.valueOf(value), foundName(unknownHeapKey, HeapPropertyPart::Value))));
        return;
    case BuiltinPropertyActionKind::ChangesIntegrity:
        m_context.mark(m_context.restrict(m_context.valueOf(object), m_universe.objects()), ObjectMutation::ChangeIntegrity);
        return;
    case BuiltinPropertyActionKind::ChangesPrototype:
        changePrototype(m_context.valueOf(object), value);
        return;
    case BuiltinPropertyActionKind::BecomesPrototype:
        if (isPassed(value))
            changePrototype(m_context.valueOf(value), object);
        else {
            connect(object, ValueFlowProgramNodes::escaped());
            m_context.mark(m_context.restrict(m_context.valueOf(object), m_universe.objects()), ObjectMutation::BecomePrototype);
        }
        return;
    case BuiltinPropertyActionKind::DescribesOwnProperties:
        // Nobody follows the descriptors: what they hold is in unknown hands, and so is what is read from them.
        connect(object, ValueFlowProgramNodes::escaped());
        if (operands.target)
            add(m_context.written(known(m_context.valueOf(operands.target)), writtenName(unknownHeapKey, HeapPropertyPart::Value)), m_universe.unknownObjectAlone());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void applyPropertyAction(ValueFlowRuleContext& context, const BuiltinPropertyAction& action, const ValueFlowPropertyActionOperands& operands)
{
    ValueFlowStatement statement;
    statement.result = action.kind == BuiltinPropertyActionKind::DescribesOwnProperties ? ValueFlowNodeID() : operands.target;
    statement.thrown = operands.thrown;
    HeapRule(statement, context).runPropertyAction(action, operands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
