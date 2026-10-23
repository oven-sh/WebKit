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
inline constexpr ValueFlowName addedNamesName = ValueFlowName::of(ValueFlowNameKind::AddedNames);
inline constexpr ValueFlowName allocatedAsName = ValueFlowName::of(ValueFlowNameKind::AllocatedAs);
inline constexpr ValueFlowName elementsName = ValueFlowName::of(ValueFlowNameKind::Elements);
inline constexpr ValueFlowName internalFieldsName = ValueFlowName::of(ValueFlowNameKind::InternalFields);

enum class HeapKeyClass : uint8_t {
    Name,
    // A number, or a name that a number converts to.
    Index,
    Unknown,
};

// Which of the receivers of the operation the key is meant for: see HeapRule::forEachKey().
enum class HeapKeyReceivers : uint8_t {
    All,
    BuiltinObjects,
    AllButBuiltinObjects,
};

struct HeapKey {
    HeapKeyClass keyClass { HeapKeyClass::Unknown };
    PropertyKeyID name;
    // `for (const name in object) .. object[name]`: nobody knows the name, and the property that the read finds is one that the enumeration of the same object has listed.
    bool namesEnumerableProperty { false };
    HeapKeyReceivers receivers { HeapKeyReceivers::All };
};

inline constexpr HeapKey indexHeapKey { HeapKeyClass::Index, { } };
inline constexpr HeapKey unknownHeapKey { HeapKeyClass::Unknown, { } };
inline constexpr HeapKey enumeratedHeapKey { HeapKeyClass::Unknown, { }, true };

enum class HeapPropertyPart : uint8_t {
    Value,
    Getter,
    Setter,
};

// The name that a store or a definition under the key writes.
ValueFlowName writtenName(const HeapKey&, HeapPropertyPart);
// The name under which a read or a store with the key looks for what is there.
ValueFlowName foundName(const HeapKey&, HeapPropertyPart);
ValueFlowName nameOfLookupForValue(const HeapKey&);
HeapKey keyOfFoundName(ValueFlowName);

// Whose property a computed key names, and what is put under it.
struct HeapKeyUse {
    const ValueFlowOperand* object { nullptr };
    const ValueFlowOperand* stored { nullptr };
    // Whoever asks takes the receivers of each key through receiversUnder().
    bool tellsReceiversApart { false };
};

// What is passed as `this` to an accessor.
struct HeapReceiver {
    const ValueFlowOperand* operand { nullptr };
    ClimbingSet set;
    // Of a set: the receivers above which an accessor nobody knows may lie. No other receiver is handed to one.
    ClimbingSet belowAccessorNobodyKnows;
    // False where the accessor is looked for on one object and called on another.
    bool isStartOfLookup { true };
};

// The objects that a lookup goes through.
constexpr ValueFlowName everyPropertyOfListedHoldersOfExports { ValueFlowNameKind::EveryProperty, 2 };

struct HeapChain {
    // The key of the lookup that holds the chain.
    ClimbingSet start;
    // The receivers that answer by themselves, so that nothing above them is asked: built-in holders, and functions that are asked for their prototype property.
    ClimbingSet holdersThatAnswer;
    // The chain and those holders, without the placeholders.
    ClimbingSet objects;
    // Those of them whose location under the key is not locked: whatever was written under the key lies in one of these.
    ClimbingSet writableObjects;
    // Among the receivers or above one of them.
    bool holdsUnknownObject { false };
    bool holdsUntrackedObject { false };
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
    ClimbingSet prototypesOf(ClimbingSet start, ValueSetID atCreation = { });
    ClimbingSet objectsAbove(ClimbingSet start);

    // For a rule that is a function of what holds now: see readWhereWritten().
    void askWithoutNodesWhereNothingIsWritten() { m_asksWithoutNodes = !m_context.usesGeneralRulesOnly(); }

    // The accessors are handed to the functor as well. The operation then looks through the chain by itself, where it would share a lookup.
    void reportAccessorsTo(const ValueFlowAccessorFunctor& functor) { m_accessorFunctor = &functor; }

private:
    using Members = Vector<ValueFlowObjectID, 8>;

    const Value& operation() const { return m_program.at(m_statement.function).at(m_statement.site); }
    const ValueFlowOperand& operand(uint32_t index) const { return m_operands[index]; }
    static bool isPassed(const ValueFlowOperand& operand) { return operand.node || (operand.constant && operand.constant != ValueSets::nothing); }
    static ValueFlowOperand operandOf(ValueFlowNodeID node) { return { node, { }, { } }; }

    ValueSetID kinds(uint16_t primitives);
    ValueSetID unknownValue(ValueFlowUnknownSource source) { return Typed::unknownValue(m_context, source); }
    ValueSetID unknownObject(ValueFlowUnknownSource source) { return m_context.maker().intersect(unknownValue(source), m_universe.objects()); }
    ValueSetID only(ValueFlowObjectID object) { return m_context.maker().of(object); }
    ClimbingSet unknownObjectAlone() const { return ClimbingSet(m_universe.unknownObjectAlone()); }
    ClimbingSet untrackedObjectAlone() const { return ClimbingSet(m_universe.untrackedObjectAlone()); }
    // Without either placeholder.
    ClimbingSet known(ClimbingSet set) { return m_context.restrict(set, m_universe.knownObjects()); }
    bool mayBeUnknownObject(ClimbingSet set) const { return m_context.contains(set, ValueFlowUniverse::unknownObject()); }
    bool mayBeUntrackedObject(ClimbingSet set) const { return m_context.contains(set, ValueFlowUniverse::untrackedObject()); }
    // The statement is due again only when another of the objects escapes, or is no longer followed.
    ClimbingSet escapedAmong(ClimbingSet objects) { return m_context.valueWithin(objects, ValueFlowProgramNodes::escaped()); }
    ClimbingSet lostAmong(ClimbingSet objects) { return m_context.valueWithin(objects, ValueFlowProgramNodes::lost()); }
    bool hasEscaped(ClimbingSet objects) { return m_context.holdsAnyOf(objects, ValueFlowProgramNodes::escaped()); }
    bool hasBeenLost(ClimbingSet objects) { return m_context.holdsAnyOf(objects, ValueFlowProgramNodes::lost()); }
    bool doesUnseenCodeRun() { return mayBeUnknownObject(m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::UnseenCodeRuns))); }
    bool doesUnseenCodeChangeRealm() { return mayBeUnknownObject(m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::UnseenCodeChangesRealm))); }
    // The statement that runs is an operation of a function, of the program or of the library. A statement that a rule has made belongs to nobody.
    bool isOperationOfFunction();

    void connect(ValueFlowNodeID from, ValueFlowNodeID to);
    void connect(const ValueFlowOperand& from, ValueFlowNodeID to);
    void connectThrough(ValueSetID mask, const ValueFlowOperand& from, ValueFlowNodeID to);
    void add(ValueFlowNodeID to, ValueSetID constant);
    void add(ValueFlowNodeID to, ClimbingSet);
    void pass(const HeapReceiver&, ValueFlowNodeID to);
    void passToAccessorNobodyKnows(const HeapReceiver&, ValueSetID mask);
    void use(BuiltinID builtin, OptionSet<BuiltinUse> uses) { m_context.use(builtin, uses); }
    void noteTestOfMembers(const ValueFlowOperand& object, const HeapKey&);
    void mayThrow() { add(m_statement.thrown, unknownValue(ValueFlowUnknownSource::ThrownByRunTime)); }
    void answerBoolean() { add(m_statement.result, m_tables.booleanKinds()); }
    bool holdsAnything(ClimbingSet set) const { return m_context.meet(set, ValueSets::anything); }
    bool holdsAnything(ValueFlowNodeID node) { return m_context.holdsAnything(node); }
    // What read() answers, or none while that node could hold nothing.
    ValueFlowNodeID readWhereWritten(ClimbingSet objects, ValueFlowName name)
    {
        if (m_asksWithoutNodes && !m_context.mayFindSomethingWritten(objects, name.kind))
            return { };
        return m_context.read(objects, name);
    }
    ClimbingSet valueOfNodeIfAny(ValueFlowNodeID node) { return node ? m_context.valueOf(node) : ClimbingSet(); }

    HeapKey keyNamed(PropertyKeyID) const;
    HeapKeyReceivers receiversOfSymbolNobodyKnows(ClimbingSet keys, const HeapKeyUse&);
    ValueFlowOperand receiversUnder(const ValueFlowOperand& object, const HeapKey&);
    void forEachKey(const ValueFlowOperand& property, NOESCAPE const Invocable<void(const HeapKey&)> auto&, HeapKeyUse = { });
    void forEachKeyOfOperation(uint32_t computedKeyIndex, NOESCAPE const Invocable<void(const HeapKey&)> auto&, HeapKeyUse = { });
    void forEachKeyOfAction(const BuiltinKey&, const ValueFlowOperand& keyOperand, NOESCAPE const Invocable<void(const HeapKey&)> auto&, HeapKeyUse = { });

    ClimbingSet startOfLookup(ClimbingSet receivers);
    // What the lookups of the receivers start from: the receivers without the placeholders, and each placeholder that is among them by itself.
    void forEachStartOfLookup(ClimbingSet receivers, NOESCAPE const Invocable<void(ClimbingSet)> auto&);
    HeapChain chainFrom(ClimbingSet receivers);
    HeapChain chainFrom(ClimbingSet receivers, const HeapKey&);
    std::optional<ClimbingSet> chainUpToPrototypeObjectsOfEntries(ClimbingSet start, ValueFlowNodeID wholeChain, const HeapKey&);
    HeapChain chainThatGoesUpFrom(ClimbingSet receivers, const HeapKey&);
    bool mayInheritSetter(ClimbingSet objects);
    ValueSetID holdersWithLockedLocationUnder(const HeapKey&) const;
    ValueSetID holdersOfUndeclaredPropertiesUnder(const HeapKey&);
    ValueSetID holdersWithMemberUnder(const HeapKey&) const;
    // None unless something was stored through the placeholder under the name and an object that the store may have reached is among those asked about.
    ValueFlowNodeID storedThroughUnknownObjectInto(ClimbingSet objects, ValueFlowName);
    ValueFlowNodeID storedThroughUntrackedObjectInto(ClimbingSet objects, ValueFlowName);
    void forEachNodeStoredThroughPlaceholdersInto(ClimbingSet objects, ValueFlowName, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto&);
    // What lies under the key in the objects themselves, and what was stored there through a placeholder.
    void forEachNodeHoldingValueUnder(const HeapKey&, ClimbingSet objects, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto&);
    // The untracked object may be any object that the analysis has stopped following. So it holds under a name whatever lies under that name on one of them or above it, and whatever was stored under it through a placeholder.
    void forEachNodeReadByName(ValueFlowName, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto&);
    void letPrototypeObjectsOfLanguageEscape();
    void appendPrototypeAtCreation(ValueFlowObjectID, Members&) const;
    void appendPrototypeOf(BuiltinClassID, Members&) const;
    ValueSetID prototypesAtCreationOf(ClimbingSet);

    ValueFlowObjectID objectOfMember(BuiltinID member, ModuleID) const;
    BuiltinID memberUnder(BuiltinID holder, PropertyKeyID) const;
    // The members that the built-in holders among the objects have under the key. The module is that of a holder of which each module has its own: its members are that module's too.
    void forEachMemberUnder(ClimbingSet objects, const HeapKey&, NOESCAPE const Invocable<void(BuiltinID member, ModuleID)> auto&);
    ClimbingSet holdersFoundThroughUnknownObject(const HeapKey&);
    ClimbingSet prototypeObjectsAboveEscapedEntries(ValueSetID holders);
    ClimbingSet holdersOfAccessorsThatRunOnUnknownObject(const HeapKey&);
    // Those of the objects that are, or lie above, an object that the analysis has stopped following.
    ClimbingSet foundThroughUntrackedObject(ValueSetID objects);
    bool isHandedOutThroughPlaceholder(BuiltinID member, const HeapKey&);
    bool mayHoldUndeclaredProperty(ClimbingSet objects, const HeapKey&);
    // While callAccessors() runs for a read: native code that nothing describes stands only for what the engine has put on an instance of one of its own classes, as `cause` on an error.
    bool m_undescribedNativeCodeIsOfEngine { false };
    bool isNameThatTableDoesNotListOnGlobalObject(const HeapKey&) const;
    bool namesObjectOfAccess() const;
    ClimbingSet settersFoundFrom(ClimbingSet start, const HeapKey&);
    bool holdsGlobalObjectOnlyWhatStatementsHaveStored();
    bool answers(BuiltinID member) const;
    bool answersWithMember(ValueFlowObjectID holder, PropertyKeyID) const;
    bool endsAtMemberOfTable(ClimbingSet entries, BuiltinName);
    bool surelyComesToMember(ClimbingSet start, PropertyKeyID);

    void collectChain();
    void collectChainOfUntrackedObject();
    void collectAccessors(BuiltinCallMode);
    void noteUseOfMembers(const HeapKey&, BuiltinUse);
    void readThroughChain(ClimbingSet start, const HeapKey&, const HeapReceiver&, ValueFlowNodeID result);
    void letNativeCodeRead(ClimbingSet entries, const ValueFlowNameReadByNativeCode&);
    void readMember(BuiltinID member, ModuleID, ValueFlowNodeID result, Members& getters);
    void readMembers(ClimbingSet objects, ClimbingSet start, const HeapKey&, ValueFlowNodeID result, Members& getters);
    void readEvaluators(const ValueFlowOperand& object, const HeapKey&);
    bool isNativeModuleThatNobodyNames(ValueFlowObjectID holder);
    bool isMemberOfBun(BuiltinID member) const;
    bool mayNamePropertyAtCreation(const HeapKey&) const;
    void readPropertiesAtCreation(ClimbingSet objects, const HeapKey&, ValueFlowNodeID result);
    void handOutPrototypeObjectsAtCreation(ClimbingSet receivers, const HeapKey&);
    bool splitLookupThroughPlaceholders();
    void callAccessors(ClimbingSet functions, ValueFlowCallKind, const HeapReceiver&, const ValueFlowOperand* value, ValueFlowNodeID result);
    bool mayBeAccessorNobodyKnows(ClimbingSet functions) const { return mayBeUnknownObject(functions) || m_context.contains(functions, m_tables.undescribedNativeCode()); }
    bool mayHoldAccessorNobodyKnows(ClimbingSet chain, const HeapKey&, HeapPropertyPart);
    bool endsAtPropertyOfCreation(ValueFlowObjectID receiver, const HeapKey&);
    ClimbingSet receiversBelowAccessorNobodyKnows(ClimbingSet start, const HeapKey&, HeapPropertyPart);
    void noteTrapOfUnknownObject(ClimbingSet objects, ValueFlowCallKind);

    void read(const ValueFlowOperand& object, const HeapKey&);
    void readOwn(ClimbingSet holders, ValueFlowName, ValueFlowNodeID result);
    void readOwn(const ValueFlowOperand& object, ValueFlowName name, ValueFlowNodeID result) { readOwn(m_context.valueOf(object), name, result); }
    void convertStoredValue(ClimbingSet receivers, const HeapKey&, const ValueFlowOperand& value);
    void test(const ValueFlowOperand& object, const HeapKey&);
    void write(ClimbingSet objects, ValueFlowName, const ValueFlowOperand& value);
    void noteWrittenMembers(ClimbingSet receivers, const HeapKey&, bool areOnlyListed = false);
    void noteMembersLookedForBeforeChange(ClimbingSet receivers, const HeapKey&, bool makesAllOnceFound = false);
    void noteDeletedMembers(ClimbingSet receivers, const HeapKey&);
    // What stands for the class and for the classes below it under ValueFlowNameKind::AllocatedAs.
    ValueSetID standingForClassAndThoseBelow(BuiltinClassID);
    // Among the objects: what built-ins have allocated as instances of the classes that these objects stand for, and of no other.
    ValueSetID allocatedByBuiltinsAs(ClimbingSet objects, ValueSetID standingForClasses);
    // All that has a property of its own under the name for life, as far as the objects are concerned.
    ValueSetID instancesThatKeepProperty(ClimbingSet objects, PropertyKeyID);
    void markRedefinition(ClimbingSet receivers, const HeapKey&);
    void markStore(ClimbingSet receivers, const HeapKey&, OptionSet<ObjectMutation> otherMarks);
    void markDefinition(ClimbingSet receivers, const HeapKey&, HeapPropertyPart);
    void define(const ValueFlowOperand& object, const HeapKey&, HeapPropertyPart, const ValueFlowOperand& value);
    ValueSetID objectsThatListingNamesUnder(const HeapKey&);
    ClimbingSet builtinObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    ClimbingSet unlockedObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    ClimbingSet hostObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers);
    ClimbingSet holdersOfExportsListedBesideUnknownObjectInProgram(const ValueFlowOperand& object, ClimbingSet receivers);
    bool m_keepsHoldersOfExportsItStartsFrom { false };
    void notePrototypeObjectsListedAtWrite(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    void store(const ValueFlowOperand& object, const HeapKey&, const ValueFlowOperand& value);
    void callSetters(ClimbingSet receivers, const HeapKey&, const ValueFlowOperand& receiver, const ValueFlowOperand& value, bool receiverIsStartOfLookup = true);
    void callSettersOfReceivers(ClimbingSet start, const HeapKey&, const ValueFlowOperand& value);
    void remove(const ValueFlowOperand& object, const HeapKey&);
    // A spread | Object.assign | Object.defineProperties with the descriptors of the source.
    enum class CopiedProperties : uint8_t { AreDefinedAsValues, AreAssigned, AreDefinedAsTheyStand };
    void copyProperties(const ValueFlowOperand& target, const ValueFlowOperand& source, CopiedProperties);
    bool readsRestArgumentOfInstance();
    bool readsUnderNameOfEnumerationOfSameObject();
    bool isAsEnumerationHasSeenIt(const HeapChain&);
    void readWhatNamesOfProgramMayHaveHidden(const HeapChain&, ValueFlowNodeID result, Members& getters);
    bool hasMemberOfHost(ValueFlowObjectID holder) const;
    void defineAsDescribed(const ValueFlowOperand& object, const HeapKey&, ClimbingSet descriptors);
    void changePrototype(ClimbingSet objects, const ValueFlowOperand& prototype);
    enum class PropertySearch : uint8_t { OwnProperty, WholeChain };
    bool holdsOnlyWhatWasWritten(ClimbingSet receivers);
    void readAccessorFunctions(const ValueFlowOperand& object, const HeapKey&, HeapPropertyPart, PropertySearch);

    ClimbingSet allocateUnder(const ValueFlowOperand& prototype);
    void runNewObject();
    void runCopyObject();
    void runNewArray();
    void runNewTemplateObject();
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
    bool m_asksWithoutNodes { false };
};

// An object is converted to a key by code that the object names, and the key is whatever that code answers.
void HeapRule::forEachKey(const ValueFlowOperand& property, NOESCAPE const Invocable<void(const HeapKey&)> auto& functor, HeapKeyUse use)
{
    ClimbingSet keys = m_context.valueOf(property);
    connect(property, m_context.hub(m_universe.builtinTables().conversionToPrimitive()));

    // undefined, null and the booleans are converted to one text each. `const { kState } = require(..)` may find nothing as far as anybody knows, and then `stream[kState]` reads under "undefined".
    constexpr std::pair<PrimitiveSet, NamedBuiltinName> textsOfPrimitives[] {
        { PrimitiveSet::Undefined, BuiltinNames::undefined },
        { PrimitiveSet::Absent, BuiltinNames::undefined },
        { PrimitiveSet::Null, BuiltinNames::null },
        { PrimitiveSet::False, BuiltinNames::falseKeyword },
        { PrimitiveSet::True, BuiltinNames::trueKeyword },
    };
    Vector<ValueFlowObjectID, 5> kindsWithText;
    for (auto [kind, text] : textsOfPrimitives)
        kindsWithText.append(ValueFlowUniverse::primitive(kind));
    ClimbingSet otherKeys = m_context.restrict(keys, ValueSets::complement(m_context.maker().of(kindsWithText.span())));

    // What is done under a key nobody knows holds for every name and every index.
    if (m_context.meet(otherKeys, m_tables.keysNobodyKnows())) {
        HeapKeyReceivers receiversOfSymbols = receiversOfSymbolNobodyKnows(otherKeys, use);
        if (receiversOfSymbols == HeapKeyReceivers::AllButBuiltinObjects) {
            functor(unknownHeapKey);
            return;
        }
        if (receiversOfSymbols == HeapKeyReceivers::BuiltinObjects)
            functor(HeapKey { HeapKeyClass::Unknown, { }, false, HeapKeyReceivers::AllButBuiltinObjects });
        // Native code may look under a symbol that the program does not name.
        if (use.stored)
            connect(*use.stored, ValueFlowProgramNodes::escaped());
        m_context.forEachMember(ClimbingSet(m_context.maker().intersect(m_universe.symbols(), m_universe.keys())), [&](ValueFlowObjectID constant) {
            HeapKey key = keyNamed(m_universe.keyOf(constant));
            key.receivers = receiversOfSymbols;
            functor(key);
        });
    }

    BuiltinName lastText;
    for (auto [kind, text] : textsOfPrimitives) {
        if (text != lastText && m_context.contains(keys, ValueFlowUniverse::primitive(kind))) {
            functor(keyNamed(m_program.keyOf(text)));
            lastText = text;
        }
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

void HeapRule::forEachKeyOfOperation(uint32_t computedKeyIndex, NOESCAPE const Invocable<void(const HeapKey&)> auto& functor, HeapKeyUse use)
{
    if (PropertyKeyID name = operation().key())
        functor(keyNamed(name));
    else
        forEachKey(operand(computedKeyIndex), functor, use);
}

void HeapRule::forEachKeyOfAction(const BuiltinKey& key, const ValueFlowOperand& keyOperand, NOESCAPE const Invocable<void(const HeapKey&)> auto& functor, HeapKeyUse use)
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
        forEachKey(keyOperand, functor, use);
        return;
    case BuiltinKeyKind::ElementsOf: {
        // Nobody lists what a placeholder holds, nor what was stored through one into a list that it stands for.
        ClimbingSet lists = m_context.valueOf(keyOperand);
        if (m_context.meet(lists, m_universe.placeholdersAlone()) || hasEscaped(known(lists)) || hasBeenLost(known(lists)))
            functor(unknownHeapKey);
        else
            forEachKey(operandOf(m_context.read(known(lists), elementsName)), functor);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void HeapRule::forEachStartOfLookup(ClimbingSet receivers, NOESCAPE const Invocable<void(ClimbingSet)> auto& functor)
{
    functor(startOfLookup(receivers));
    if (mayBeUntrackedObject(receivers))
        functor(untrackedObjectAlone());
    if (mayBeUnknownObject(receivers))
        functor(unknownObjectAlone());
}

void HeapRule::forEachNodeStoredThroughPlaceholdersInto(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    if (ValueFlowNodeID stored = storedThroughUnknownObjectInto(objects, name))
        functor(stored);
    if (ValueFlowNodeID stored = storedThroughUntrackedObjectInto(objects, name))
        functor(stored);
}

void HeapRule::forEachNodeHoldingValueUnder(const HeapKey& key, ClimbingSet objects, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    ValueFlowName value = foundName(key, HeapPropertyPart::Value);
    functor(m_context.read(objects, value));
    forEachNodeStoredThroughPlaceholdersInto(objects, value, functor);
}

void HeapRule::forEachNodeReadByName(ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    functor(m_context.read(foundThroughUntrackedObject(m_universe.knownObjects()), name));
    functor(m_context.read(untrackedObjectAlone(), name));
    functor(m_context.read(unknownObjectAlone(), name));
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
        m_context.forEachMember(m_context.restrict(objects, m_tables.holdersWithMembersUnderNumericNames()), [&](ValueFlowObjectID holder) {
            for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder))) {
                if (m_tables.hasNumericName(member))
                    functor(member, m_universe.moduleOf(holder));
            }
        });
        return;
    case HeapKeyClass::Unknown:
        m_context.forEachMember(m_context.restrict(objects, m_tables.holdersWithMembers()), [&](ValueFlowObjectID holder) {
            ModuleID module = m_universe.moduleOf(holder);
            for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder)))
                functor(member, module);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
