/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesHeap.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr ValueFlowName prototypeLinkName = ValueFlowName::of(ValueFlowNameKind::PrototypeLink);
inline constexpr ValueFlowName copiedFromName = ValueFlowName::of(ValueFlowNameKind::CopiedFrom);
inline constexpr ValueFlowName elementsName = ValueFlowName::of(ValueFlowNameKind::Elements);
inline constexpr ValueFlowName internalFieldsName = ValueFlowName::of(ValueFlowNameKind::InternalFields);

enum class HeapKeyClass : uint8_t {
    Name,
    // A number, or a name that a number converts to.
    Index,
    Unknown,
};

struct HeapKey {
    HeapKeyClass keyClass { HeapKeyClass::Unknown };
    PropertyKeyID name;
};

inline constexpr HeapKey indexHeapKey { HeapKeyClass::Index, { } };
inline constexpr HeapKey unknownHeapKey { HeapKeyClass::Unknown, { } };

enum class HeapPropertyPart : uint8_t {
    Value,
    Getter,
    Setter,
};

// The name that a store or a definition under the key writes.
ValueFlowName writtenName(const HeapKey&, HeapPropertyPart);
// The name under which a read or a store with the key looks for what is there.
ValueFlowName foundName(const HeapKey&, HeapPropertyPart);
HeapKey keyOfFoundName(ValueFlowName);

// What is passed as `this` to an accessor.
struct HeapReceiver {
    const ValueFlowOperand* operand { nullptr };
    ClimbingSet set;
};

// The objects that a lookup goes through.
struct HeapChain {
    // The key of the lookup that holds the chain.
    ClimbingSet start;
    // The built-in holders among the receivers that answer by themselves: nothing above them is asked.
    ClimbingSet holdersThatAnswer;
    // The chain and those holders, without the unknown object.
    ClimbingSet objects;
    // Those of them whose location under the key is not locked: whatever was written under the key lies in one of these.
    ClimbingSet writableObjects;
    bool holdsUnknownObject { false };
};

// One run of a rule of the heap.
class HeapRule {
public:
    HeapRule(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
        : m_statement(statement)
        , m_context(context)
        , m_program(context.program())
        , m_builtins(context.program().builtins())
        , m_universe(context.universe())
        , m_tables(context.universe().builtinTables().heap())
        , m_operands(context.operandsOf(statement))
    {
    }

    void runOperation();
    void runLookup();
    void runPropertyAction(const BuiltinPropertyAction&, const ValueFlowPropertyActionOperands&);
    ClimbingSet objectsHeldFromCreation(ClimbingSet objects);

    // The accessors are handed to the functor as well. The operation then looks through the chain by itself, where it would share a lookup.
    void reportAccessorsTo(const ValueFlowAccessorFunctor& functor) { m_accessorFunctor = &functor; }

private:
    using Members = Vector<ValueFlowObjectID, 8>;

    const Value& operation() const { return m_program.at(m_statement.function).at(m_statement.site); }
    const ValueFlowOperand& operand(uint32_t index) const { return m_operands[index]; }
    static bool isPassed(const ValueFlowOperand& operand) { return operand.node || (operand.constant && operand.constant != ValueSets::nothing); }
    static ValueFlowOperand operandOf(ValueFlowNodeID node) { return { node, { }, { } }; }

    ValueSetID kinds(uint16_t primitives);
    ValueSetID unknownValue() { return Typed::unknownValue(m_context); }
    ValueSetID only(ValueFlowObjectID object) { return m_context.maker().of(object); }
    ClimbingSet unknownObjectAlone() const { return ClimbingSet(m_universe.unknownObjectAlone()); }
    ClimbingSet known(ClimbingSet set) { return m_context.restrict(set, m_universe.knownObjects()); }
    bool mayBeUnknownObject(ClimbingSet set) const { return m_context.contains(set, ValueFlowUniverse::unknownObject()); }
    // The statement is due again only when another of the objects escapes.
    ClimbingSet escapedAmong(ClimbingSet objects) { return m_context.valueWithin(objects, ValueFlowProgramNodes::escaped()); }
    bool hasEscaped(ClimbingSet objects) { return holdsAnything(escapedAmong(objects)); }

    void connect(ValueFlowNodeID from, ValueFlowNodeID to);
    void connect(const ValueFlowOperand& from, ValueFlowNodeID to);
    void connectThrough(ValueSetID mask, const ValueFlowOperand& from, ValueFlowNodeID to);
    void add(ValueFlowNodeID to, ValueSetID constant);
    void add(ValueFlowNodeID to, ClimbingSet);
    void pass(const HeapReceiver&, ValueFlowNodeID to);
    void use(BuiltinID builtin, OptionSet<BuiltinUse> uses) { m_context.use(builtin, uses); }
    void mayThrow() { add(m_statement.thrown, unknownValue()); }
    void answerBoolean() { add(m_statement.result, m_tables.booleanKinds()); }
    bool holdsAnything(ClimbingSet set) const { return m_context.meet(set, ValueSets::anything); }

    HeapKey keyNamed(PropertyKeyID) const;
    void forEachKey(const ValueFlowOperand& property, NOESCAPE const Invocable<void(const HeapKey&)> auto&);
    void forEachKeyOfOperation(uint32_t computedKeyIndex, NOESCAPE const Invocable<void(const HeapKey&)> auto&);
    void forEachKeyOfAction(const BuiltinKey&, const ValueFlowOperand& keyOperand, NOESCAPE const Invocable<void(const HeapKey&)> auto&);

    ClimbingSet startOfLookup(ClimbingSet receivers);
    HeapChain chainFrom(ClimbingSet receivers);
    HeapChain chainFrom(ClimbingSet receivers, const HeapKey&);
    // None unless something was stored through the unknown object under the name and an object that the store may have reached is among those asked about.
    ValueFlowNodeID storedThroughUnknownObject(const HeapChain&, ValueFlowName);
    ValueFlowNodeID storedThroughUnknownObjectInto(ClimbingSet objects, ValueFlowName);
    void letPrototypeObjectsOfTableEscape();
    void appendPrototypeAtCreation(ValueFlowObjectID, Members&) const;
    void appendPrototypeOf(BuiltinClassID, Members&) const;
    ValueSetID prototypesAtCreationOf(ClimbingSet);

    ValueFlowObjectID objectOfMember(BuiltinID member, ModuleID) const;
    BuiltinID memberUnder(BuiltinID holder, PropertyKeyID) const;
    // The members that the built-in holders among the objects have under the key. The module is that of a holder of which each module has its own: its members are that module's too.
    void forEachMemberUnder(ClimbingSet objects, const HeapKey&, NOESCAPE const Invocable<void(BuiltinID member, ModuleID)> auto&);
    ClimbingSet holdersFoundThroughUnknownObject(const HeapKey&);
    bool mayHoldUndeclaredProperty(ClimbingSet objects, const HeapKey&) const;

    void collectChain();
    void collectAccessors(BuiltinCallMode);
    void noteUseOfMembers(const HeapKey&, BuiltinUse);
    void readThroughChain(ClimbingSet start, const HeapKey&, const HeapReceiver&, ValueFlowNodeID result);
    void readMember(BuiltinID member, ModuleID, ValueFlowNodeID result, Members& getters);
    void readMembers(ClimbingSet objects, const HeapKey&, ValueFlowNodeID result, Members& getters);
    void readPropertiesAtCreation(ClimbingSet objects, const HeapKey&, ValueFlowNodeID result);
    bool splitLookupThroughUnknownObject();
    void callAccessors(ClimbingSet functions, ValueFlowCallKind, const HeapReceiver&, const ValueFlowOperand* value, ValueFlowNodeID result);
    bool mayBeAccessorNobodyKnows(ClimbingSet functions) const { return mayBeUnknownObject(functions) || m_context.contains(functions, m_tables.undescribedNativeCode()); }
    bool splitInTwo(ClimbingSet, ClimbingSet& first, ClimbingSet& second);
    void noteTrapOfUnknownObject(ClimbingSet objects, ValueFlowCallKind);

    void read(const ValueFlowOperand& object, const HeapKey&);
    void readOwn(ClimbingSet holders, ValueFlowName, ValueFlowNodeID result);
    void readOwn(const ValueFlowOperand& object, ValueFlowName name, ValueFlowNodeID result) { readOwn(m_context.valueOf(object), name, result); }
    void convertStoredElement(ClimbingSet receivers, const HeapKey&, const ValueFlowOperand& value);
    void test(const ValueFlowOperand& object, const HeapKey&);
    void write(ClimbingSet objects, ValueFlowName, const ValueFlowOperand& value);
    void noteWrittenMembers(ClimbingSet receivers, const HeapKey&);
    void noteDeletedMembers(ClimbingSet receivers, const HeapKey&);
    void markStore(ClimbingSet receivers, const HeapKey&, OptionSet<ObjectMutation> otherMarks);
    void markDefinition(ClimbingSet receivers, const HeapKey&, HeapPropertyPart);
    void define(const ValueFlowOperand& object, const HeapKey&, HeapPropertyPart, const ValueFlowOperand& value);
    void store(const ValueFlowOperand& object, const HeapKey&, const ValueFlowOperand& value);
    void callSetters(ClimbingSet receivers, const HeapKey&, const ValueFlowOperand& receiver, const ValueFlowOperand& value);
    void callSettersOfReceivers(ClimbingSet start, ValueFlowName, const ValueFlowOperand& value);
    void remove(const ValueFlowOperand& object, const HeapKey&);
    void copyProperties(const ValueFlowOperand& target, const ValueFlowOperand& source, bool storesThroughSetters);
    void defineAsDescribed(const ValueFlowOperand& object, const HeapKey&, ClimbingSet descriptors);
    void changePrototype(ClimbingSet objects, const ValueFlowOperand& prototype);
    void readAccessorFunctions(const ValueFlowOperand& object, const HeapKey&, HeapPropertyPart);

    void runNewObject();
    void runNewArray();
    void runData();
    void runTruncateNewArray();
    void runCreateIteratorResult();
    void runGetByValWithThis();
    void runCreateArguments(const ValueFlowOperand& count, const ValueFlowOperand& rest, std::span<const ValueFlowOperand> parameters, const ValueFlowOperand* callee);
    void runScopeHasBinding();
    void runGetSuperBase();
    void runSuperGet();
    void runSuperSet();

    const ValueFlowStatement& m_statement;
    ValueFlowRuleContext& m_context;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    const ValueFlowUniverse& m_universe;
    const ValueFlowHeapTables& m_tables;
    std::span<const ValueFlowOperand> m_operands;
    const ValueFlowAccessorFunctor* m_accessorFunctor { nullptr };
};

// An object is converted to a key by code that the object names, and the key is whatever that code answers.
void HeapRule::forEachKey(const ValueFlowOperand& property, NOESCAPE const Invocable<void(const HeapKey&)> auto& functor)
{
    ClimbingSet keys = m_context.valueOf(property);
    connect(property, m_context.hub(m_universe.builtinTables().conversionToPrimitive()));

    // What is done under a key nobody knows holds for every name and every index.
    if (m_context.meet(keys, m_tables.keysNobodyKnows())) {
        functor(unknownHeapKey);
        return;
    }

    bool namesIndex = m_context.meet(keys, m_tables.numberKinds());
    m_context.forEachMember(m_context.restrict(keys, m_universe.keys()), [&](ValueFlowObjectID constant) {
        HeapKey key = keyNamed(m_universe.keyOf(constant));
        if (key.keyClass == HeapKeyClass::Index)
            namesIndex = true;
        else
            functor(key);
    });
    if (namesIndex)
        functor(indexHeapKey);
}

void HeapRule::forEachKeyOfOperation(uint32_t computedKeyIndex, NOESCAPE const Invocable<void(const HeapKey&)> auto& functor)
{
    if (PropertyKeyID name = operation().key())
        functor(keyNamed(name));
    else
        forEachKey(operand(computedKeyIndex), functor);
}

void HeapRule::forEachKeyOfAction(const BuiltinKey& key, const ValueFlowOperand& keyOperand, NOESCAPE const Invocable<void(const HeapKey&)> auto& functor)
{
    switch (key.kind) {
    case BuiltinKeyKind::None:
        return;
    case BuiltinKeyKind::Any:
        functor(unknownHeapKey);
        return;
    case BuiltinKeyKind::Indexes:
        functor(indexHeapKey);
        return;
    case BuiltinKeyKind::Names:
        for (BuiltinName name : key.names)
            functor(keyNamed(m_program.keyOf(name)));
        return;
    case BuiltinKeyKind::Operand:
        forEachKey(keyOperand, functor);
        return;
    case BuiltinKeyKind::ElementsOf: {
        ClimbingSet lists = m_context.valueOf(keyOperand);
        if (mayBeUnknownObject(lists) || hasEscaped(known(lists)))
            functor(unknownHeapKey);
        else
            forEachKey(operandOf(m_context.read(known(lists), elementsName)), functor);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void HeapRule::forEachMemberUnder(ClimbingSet objects, const HeapKey& key, NOESCAPE const Invocable<void(BuiltinID member, ModuleID)> auto& functor)
{
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        m_context.forEachMember(m_context.restrict(objects, m_tables.under(key.name).holders), [&](ValueFlowObjectID holder) {
            if (BuiltinID member = memberUnder(m_universe.builtinOf(holder), key.name))
                functor(member, m_universe.moduleOf(holder));
        });
        return;
    case HeapKeyClass::Index:
        return;
    case HeapKeyClass::Unknown:
        m_context.forEachMember(m_context.restrict(objects, m_tables.holdersWithMembers()), [&](ValueFlowObjectID holder) {
            for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder)))
                functor(member, m_universe.moduleOf(holder));
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
