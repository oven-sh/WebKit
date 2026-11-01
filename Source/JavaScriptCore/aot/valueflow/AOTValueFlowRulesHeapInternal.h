/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTPrototypesAtCreation.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTVector.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr ValueFlowCellName addedNamesName { ValueFlowNameKind::AddedNames };
inline constexpr ValueFlowCellName internalFieldsName { ValueFlowNameKind::InternalFields };
inline constexpr ValueFlowName everyPropertyOfListedHoldersOfExports { ValueFlowNameKind::EveryProperty, 2 };
inline constexpr ValueFlowName everyPropertyUnderKeyOfProgramAlone { ValueFlowNameKind::EveryProperty, 8 };

enum class HeapKeyClass : uint8_t {
    Name,
    Index,
    Unknown,
};

enum class HeapKeyReceivers : uint8_t { All, BuiltinObjects, AllButBuiltinObjects };

struct HeapKey {
    HeapKeyClass keyClass { HeapKeyClass::Unknown };
    PropertyKeyID name;
    bool namesEnumerableProperty { false };
    HeapKeyReceivers receivers { HeapKeyReceivers::All };
    bool isComputed { false };
    bool namesNoSymbol { false };
    bool namesSymbolAlone { false };
    // With namesEnumerableProperty: the key is no name of anything that the engine has put on the objects asked, hidden by a name of the program or not.
    bool namesNoPropertyOfEngine { false };

    bool mayNameNonEnumerable(PropertyKeyID other) const { return (keyClass == HeapKeyClass::Unknown && !namesEnumerableProperty) || (keyClass == HeapKeyClass::Name && name == other); }
};

inline constexpr HeapKey indexHeapKey { HeapKeyClass::Index, { } };
inline constexpr HeapKey unknownHeapKey { HeapKeyClass::Unknown, { } };
inline constexpr HeapKey enumeratedHeapKey { HeapKeyClass::Unknown, { }, true };
inline constexpr HeapKey heapKeyOfProgramAlone { HeapKeyClass::Unknown, { }, true, HeapKeyReceivers::All, false, false, false, true };

enum class HeapPropertyPart : uint8_t { Value, Getter, Setter };

ValueFlowName writtenName(const HeapKey&, HeapPropertyPart);
ValueFlowName foundName(const HeapKey&, HeapPropertyPart);
ValueFlowName nameOfLookupForValue(const HeapKey&);
HeapKey keyOfFoundName(ValueFlowName);

struct HeapKeyUse {
    const ValueFlowOperand* object { nullptr };
    const ValueFlowOperand* stored { nullptr };
    bool tellsReceiversApart { false };
    bool isRead { false };
    bool tellsEveryNameApart { false };
};

struct HeapReceiver {
    const ValueFlowOperand* operand { nullptr };
    ClimbingSet set;
    ClimbingSet belowAccessorNobodyKnows;
    bool isStartOfLookup { true };
    const HeapKey* keyLookedUp { nullptr };
};

struct HeapChain {
    ClimbingSet start;
    ClimbingSet holdersThatAnswer;
    ClimbingSet objects;
    ClimbingSet writableObjects;
    bool holdsUnknownObject { false };
    bool holdsUntrackedObject { false };
};

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
    void readOwnPropertiesNobodyWrote(ClimbingSet holders, const HeapKey&, ValueFlowNodeID result);
    void readPropertiesNobodyWroteOnSourcesOfCopies(const HeapChain&, const HeapKey&, ValueFlowNodeID result);
    void readOwnPropertiesNobodyWrote(ClimbingSet holders, std::optional<PropertyKeyID> key, ValueFlowNodeID result) { readOwnPropertiesNobodyWrote(holders, key ? keyNamed(*key) : unknownHeapKey, result); }
    enum class GettersAreRunBy : uint8_t { Read, Walk, Printer };
    void runGettersFoundThroughUnknownObject(const HeapKey&, GettersAreRunBy);
    ClimbingSet prototypesOf(ClimbingSet start, ValueSetID atCreation = { });
    ValueFlowNodeID exportsOfHoldersListedBesideUnknownObject(const ValueFlowOperand& object);
    ValueFlowNodeID exportsOfHoldersIn(const ValueFlowOperand& object);
    ClimbingSet objectsAbove(ClimbingSet start) { return m_context.unite(withTargetsOfProxiesThatWalksPass(prototypesOf(start)), valueOfNodeIfAny(readWhereWritten(known(start), copiedFromName))); }
    ClimbingSet withTargetsOfProxiesThatWalksPass(ClimbingSet above);

    void askWithoutNodesWhereNothingIsWritten() { m_asksWithoutNodes = !m_context.usesGeneralRulesOnly(); }

private:
    using Members = Vector<ValueFlowObjectID, 8>;

    const Value& operation() const { return m_context.operationOf(m_statement); }
    const ValueFlowOperand& operand(uint32_t index) const { return m_operands[index]; }
    ClimbingSet m_proxiesAskedByTheirTraps;
    static bool isPassed(const ValueFlowOperand& operand) { return operand.node || (operand.constant && operand.constant != ValueSets::nothing); }
    static ValueFlowOperand operandOf(ValueFlowNodeID node) { return { node, { }, { } }; }

    ValueSetID kinds(uint16_t primitives);
    ValueSetID unknownValue(ValueFlowUnknownSource source) { return Typed::unknownValue(m_context, source); }
    ValueSetID unknownObject(ValueFlowUnknownSource source) { return m_context.maker().intersect(unknownValue(source), m_universe.objects()); }
    ValueSetID only(ValueFlowObjectID object) { return m_context.maker().of(object); }
    ClimbingSet unknownObjectAlone() const { return ClimbingSet(m_universe.unknownObjectAlone()); }
    ClimbingSet untrackedObjectAlone() const { return ClimbingSet(m_universe.untrackedObjectAlone()); }
    ClimbingSet known(ClimbingSet set) { return knownObjectsThatAreFollowed(m_context, set); }
    bool mayBeUnknownObject(ClimbingSet set) const { return m_context.contains(set, ValueFlowUniverse::unknownObject()); }
    bool mayBeUntrackedObject(ClimbingSet set) const { return m_context.contains(set, ValueFlowUniverse::untrackedObject()); }
    ClimbingSet escapedAmong(ClimbingSet objects) { return m_context.valueWithin(objects, ValueFlowProgramNodes::escaped()); }
    ClimbingSet lostAmong(ClimbingSet objects) { return m_context.valueWithin(objects, ValueFlowProgramNodes::lost()); }
    bool hasEscaped(ClimbingSet objects) { return m_context.holdsAnyOf(objects, ValueFlowProgramNodes::escaped()); }
    bool isAnswerThatNoReadLists(ClimbingSet objects) { return m_context.holdsAnyOf(objects, m_context.nodes().ofHost(ValueFlowHostNode::AnswersThatNoReadLists)); }
    void noteAnswersThatNoReadLists(const Members&);
    void noteAnswersOfReadUnderUnknownKey(ValueFlowObjectID holder);
    bool answersUnknownObjectUnderUnknownKeys(ValueFlowObjectID holder) { return isNativeModuleThatNobodyNames(holder) || m_builtins.canonicalID(m_universe.builtinOf(holder)) == Builtins::Bun; }
    bool mayBeAnsweredWithoutBeingListed(BuiltinID member) const { return isObjectKind(m_builtins.at(member).kind) && m_builtins.at(member).kind != BuiltinKind::Prototype; }
    bool mayBeReachedThroughUnknownObject(ClimbingSet objects)
    {
        if (hasEscaped(objects))
            return true;
        return isHostFlagRaised(m_context, ValueFlowHostNode::ObjectNobodyKnowsIsWritten) && (m_context.meet(objects, m_tables.classes()) || isAnswerThatNoReadLists(objects));
    }
    bool hasBeenLost(ClimbingSet objects) { return m_context.holdsAnyOf(objects, ValueFlowProgramNodes::lost()); }
    bool hostNodeHoldsUnknownObject(ValueFlowHostNode node, std::optional<bool>& answer)
    {
        if (!answer)
            answer = mayBeUnknownObject(m_context.valueOf(m_context.nodes().ofHost(node)));
        return *answer;
    }
    bool doesUnseenCodeRun() { return hostNodeHoldsUnknownObject(ValueFlowHostNode::UnseenCodeRuns, m_doesUnseenCodeRun); }
    bool doesUnseenCodeChangeRealm() { return hostNodeHoldsUnknownObject(ValueFlowHostNode::UnseenCodeChangesRealm, m_doesUnseenCodeChangeRealm); }
    bool isOperationOfFunction() { return m_statement.function || m_context.functionOfStatement(); }

    void connect(ValueFlowNodeID from, ValueFlowNodeID to)
    {
        if (from && to)
            m_context.connect(from, to);
    }
    void connect(const ValueFlowOperand& from, ValueFlowNodeID to)
    {
        if (to)
            m_context.connect(from, to);
    }
    void connectThrough(ValueSetID mask, const ValueFlowOperand& from, ValueFlowNodeID to);
    void add(ValueFlowNodeID to, auto set)
    {
        if (to)
            m_context.add(to, set);
    }
    void passToAccessorNobodyKnows(const HeapReceiver&, ValueSetID mask);
    void use(BuiltinID builtin, OptionSet<BuiltinUse> uses) { m_context.use(builtin, uses); }
    void noteTestOfMembers(const ValueFlowOperand& object, const HeapKey&);
    ValueFlowOperand withoutProxiesAskedByTheirTraps(const ValueFlowOperand& object);
    ValueFlowObjectID nameThatGetTrapsAreFollowedFor(ClimbingSet traps, const HeapKey&);
    void askProxiesByTheirTrap(const HeapKey&, BuiltinName trap, ValueFlowNodeID result, const ValueFlowOperand* stored = nullptr);
    void readThroughProxies(ClimbingSet proxies, const HeapKey&, ValueFlowNodeID result);
    ValueFlowOperand withoutProxiesEnumeratedByTheirTraps(const ValueFlowOperand& object);
public:
    void readForReceiver(const ValueFlowOperand& object, const ValueFlowOperand& property, const ValueFlowOperand& receiver);
    void listProxiesByTheirTraps(ClimbingSet proxies);
    ClimbingSet targetsListedInPlaceOf(ClimbingSet proxies);
    void askProxiesForListedValues(ClimbingSet proxies, ValueFlowNodeID result);
    void askProxiesAboutOwnProperty(ClimbingSet proxies, const ValueFlowOperand& property, BuiltinName trap, ValueFlowNodeID descriptor, ValueFlowNodeID result);
private:
    void mayThrow() { add(m_statement.thrown, unknownValue(ValueFlowUnknownSource::ThrownByRunTime)); }
    void answerBoolean() { add(m_statement.result, m_tables.booleanKinds()); }
    bool holdsAnything(ClimbingSet set) const { return m_context.meet(set, ValueSets::anything); }
    bool holdsAnything(ValueFlowNodeID node) { return m_context.holdsAnything(node); }
    ValueFlowNodeID readWhereWritten(ClimbingSet objects, ValueFlowName name, ValueFlowRuleContext::ReadUse use = ValueFlowRuleContext::ReadUse::Any)
    {
        if (m_asksWithoutNodes && !m_context.mayFindSomethingWritten(objects, name.kind))
            return { };
        return m_context.readWhatWasWritten(objects, name, use);
    }
    template<typename Functor>
    void forEachNodeOfGettersLaidUnder(ClimbingSet objects, const HeapKey& key, const Functor& functor)
    {
        functor(readWhereWritten(objects, foundName(key, HeapPropertyPart::Getter), ValueFlowRuleContext::ReadUse::OutsideEnumerations));
        if (key.keyClass != HeapKeyClass::Name || !isHostFlagRaised(m_context, ValueFlowHostNode::GettersAreFoundUnderNamesOfEnumerations))
            return;
        if (ValueFlowNodeID sources = readWhereWritten(objects, gettersCopiedFromName))
            functor(m_context.lookup(known(m_context.valueOf(sources)), foundName(key, HeapPropertyPart::Value)));
    }
    ClimbingSet valueOfNodeIfAny(ValueFlowNodeID node) { return node ? m_context.valueOf(node) : ClimbingSet(); }

    HeapKey keyNamed(PropertyKeyID name) const { return !name ? unknownHeapKey : m_program.isIndexLike(name) ? indexHeapKey : HeapKey { HeapKeyClass::Name, name }; }
    HeapKeyReceivers receiversOfSymbolNobodyKnows(ClimbingSet keys, const HeapKeyUse&);
    ValueFlowOperand receiversUnder(const ValueFlowOperand& object, const HeapKey&);
    ClimbingSet receiversOfWrite(const ValueFlowOperand& object, const HeapKey&);
    void forEachKey(const ValueFlowOperand& property, const ScopedLambda<void(const HeapKey&)>&, HeapKeyUse = { });
    void noteWhatMayLieUnderSymbolThatHostLooksFor(const HeapKey&, const ValueFlowOperand& stored);
    void noteHoldersOfLibraryExportsThatProgramWrites(ClimbingSet receivers, const ValueFlowOperand& stored);
    void forEachKeyOfOperation(uint32_t computedKeyIndex, NOESCAPE const Invocable<void(const HeapKey&)> auto&, HeapKeyUse = { });
    void forEachKeyOfAction(const BuiltinKey&, const ValueFlowOperand& keyOperand, NOESCAPE const Invocable<void(const HeapKey&)> auto&, HeapKeyUse = { });

    ClimbingSet startOfLookup(ClimbingSet receivers);
    void forEachStartOfLookup(ClimbingSet receivers, NOESCAPE const Invocable<void(ClimbingSet)> auto&);
    bool tellsApartStartsThatFindNothing(ClimbingSet start, const HeapKey&) const;
    std::optional<ClimbingSet> startsThatMayFindSomething(ClimbingSet start, const HeapKey&);
    void readFromStartsThatMayFindSomething(ClimbingSet start, const HeapKey&);
    HeapChain chainFrom(ClimbingSet receivers, ValueFlowName nameOfChain = prototypeLinkName);
    HeapChain chainFrom(ClimbingSet receivers, const HeapKey&);
    std::optional<ClimbingSet> chainUpToPrototypeObjectsOfEntries(ClimbingSet start, ValueFlowNodeID wholeChain, const HeapKey&);
    HeapChain chainThatGoesUpFrom(ClimbingSet receivers, const HeapKey&);
    HeapChain chainWithCopiesThatReach(const HeapKey&, ClimbingSet receivers);
    bool mayInheritSetter(ClimbingSet objects);
    bool mayRefuseStoreInSilence(ClimbingSet objects, const HeapKey&);
    ValueSetID holdersWithLockedLocationUnder(const HeapKey&) const;
    bool mayTakeMemberFromCopy(ClimbingSet holders, const HeapKey&);
    ValueSetID holdersOfUndeclaredPropertiesUnder(const HeapKey&);
    ValueSetID holdersWithMemberUnder(const HeapKey&) const;
    ValueFlowNodeID storedThroughUnknownObjectInto(ClimbingSet objects, ValueFlowName);
    ValueFlowNodeID storedThroughUnknownObjectIntoObjectsWithinReach(ClimbingSet objects, ValueFlowName);
    std::array<ValueFlowNodeID, 2> storedByStatementsThroughUnknownObjectInto(ClimbingSet objects, ValueFlowName);
    ValueFlowNodeID storedThroughUntrackedObjectInto(ClimbingSet objects, ValueFlowName);
    void forEachNodeStoredThroughPlaceholdersInto(ClimbingSet objects, ValueFlowName, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto&);
    bool isStoredThroughPlaceholdersInto(ClimbingSet objects, ValueFlowName name)
    {
        bool isStoredThroughUnknownObject = !!storedThroughUnknownObjectInto(objects, name);
        return storedThroughUntrackedObjectInto(objects, name) || isStoredThroughUnknownObject;
    }
    void forEachNodeHolding(ClimbingSet objects, ValueFlowName, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto&);
    void noteName(ValueFlowHostNode, const HeapKey&);
    ValueFlowRuleContext::LookupUse useOfLookupUnder(const HeapKey& key) const { return m_statement.isOnlyCalledOnItsHolder && key.keyClass == HeapKeyClass::Name ? ValueFlowRuleContext::LookupUse::OnlyCalledOnReceiver : ValueFlowRuleContext::LookupUse::Any; }
    void forEachNodeReadByName(ValueFlowName, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto&);
    void letPrototypeObjectsOfLanguageEscape() { add(ValueFlowProgramNodes::escaped(), m_tables.prototypeObjectsOfLanguage()); }
    void appendPrototypeAtCreation(ValueFlowObjectID, Members&) const;
    uint64_t prototypesAtCreationInOneWord(ValueFlowObjectID) const;
    ValueSetID prototypesAtCreationOf(ClimbingSet);
    ValueSetID findPrototypesAtCreationOf(ClimbingSet);

    ValueFlowObjectID objectOfMember(BuiltinID member, ModuleID) const;
    BuiltinID memberUnder(BuiltinID holder, PropertyKeyID) const;
    void forEachMemberUnder(ClimbingSet objects, const HeapKey&, const ScopedLambda<void(BuiltinID member, ModuleID)>&);
    ClimbingSet holdersFoundThroughUnknownObject(const HeapKey&);
    ClimbingSet prototypeObjectsAboveEscapedEntries(std::span<const ValueFlowClassOfEntries>, ClimbingSet holders);
    ClimbingSet foundThroughUntrackedObject(ValueSetID objects);
    ValueFlowNodeID readWhatStaysBehindUnder(ValueFlowName);
    bool mayBeFoundThroughPlaceholder(BuiltinID member);
    bool isHandedOutThroughPlaceholder(BuiltinID member, const HeapKey&);
    bool runsGetterThroughPlaceholder(BuiltinID member, const HeapKey&);
    bool mayHoldUndeclaredProperty(ClimbingSet objects, const HeapKey&);
    bool mayHoldWhatNobodyHasAccountedFor(const HeapChain&, const HeapKey&);
    bool m_undescribedNativeCodeIsOfEngine { false };
    bool m_namesFunctionWhereItIsMade { false };
    bool isNameThatTableDoesNotListOnGlobalObject(const HeapKey&) const;
    bool namesObjectOfAccess() const;
    bool findsOnGlobalObjectWhatStatementsStore(const HeapKey&) const;
    ClimbingSet accessorsFoundFrom(ClimbingSet start, const HeapKey&, HeapPropertyPart);
    ClimbingSet settersOfProgramLaidAbove(ClimbingSet start, const HeapKey&);
    ClimbingSet gettersOfEnumerableProperties(ClimbingSet holders);
    // The images below have run first: what a preload of theirs has stored on the global object no statement of this program shows.
    bool holdsGlobalObjectOnlyWhatStatementsHaveStored() { return !m_context.refusedCode().hasImagesBelow && !doesUnseenCodeRun() && !m_context.holdsAnyOf(ClimbingSet(only(m_universe.objectOf(Builtins::ProcessDlopen))), m_context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)); }
    bool answers(BuiltinID member) const;
    bool answersWithMember(ValueFlowObjectID holder, PropertyKeyID name) const { return answers(memberUnder(m_universe.builtinOf(holder), name)); }
    bool endsAtMemberOfTable(ClimbingSet entries, BuiltinName);
    bool surelyComesToMember(ClimbingSet start, PropertyKeyID);

    void collectChain();
    ValueFlowObjectID onlyObjectIn(ClimbingSet);
    void walkUpFrom(ValueFlowObjectID);
    void collectAccessors(BuiltinCallMode);
    void noteUseOfMembers(const HeapKey&, BuiltinUse);
    ClimbingSet letMembersOfHeldPrototypeObjectsEscape(PropertyKeyID, std::initializer_list<ValueFlowName>);
    void readThroughChain(ClimbingSet start, const HeapKey&, const HeapReceiver&, ValueFlowNodeID result);
    void letNativeCodeRead(ClimbingSet entries, const ValueFlowNameReadByNativeCode&);
    void letConstructorEscapeWithPrototypeObject(BuiltinID member, ModuleID);
    void readMember(BuiltinID member, ModuleID, ValueFlowNodeID result, Members& getters);
    void readMembers(ClimbingSet objects, ClimbingSet start, const HeapKey&, ValueFlowNodeID result, Members& getters);
    void readEvaluators(const ValueFlowOperand& object, const HeapKey&);
    ClimbingSet publicModulesThatMayBe(const ValueFlowOperand& object);
    void readMembersLeftOutThatLoadNothing(const ValueFlowOperand& object);
    bool isNativeModuleThatNobodyNames(ValueFlowObjectID holder) { return m_context.sets().contains(m_tables.objectsOfNativeModules(), holder) && !m_context.holdsAnyOf(ClimbingSet(only(holder)), m_context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins)); }
    bool isMemberOfBun(BuiltinID member) const { return m_builtins.at(member).holder && m_builtins.canonicalID(m_builtins.at(member).holder) == Builtins::Bun; }
    bool isMemberOfBunUsedByNameAlone(BuiltinID member) const;
    void readPropertiesAtCreation(ClimbingSet objects, const HeapKey&, ValueFlowNodeID result);
    void handOutPrototypeObjectsAtCreation(ClimbingSet receivers, const HeapKey&, ValueFlowNodeID result);
    bool splitLookupThroughPlaceholders();
    void callAccessors(ClimbingSet functions, ValueFlowCallKind, const HeapReceiver&, const ValueFlowOperand* value, ValueFlowNodeID result);
    bool mayBeAccessorNobodyKnows(ClimbingSet functions) const { return mayBeUnknownObject(functions) || m_context.contains(functions, m_tables.undescribedNativeCode()); }
    bool mayHoldAccessorNobodyKnows(ClimbingSet chain, const HeapKey&, HeapPropertyPart);
    bool endsAtPropertyOfCreation(ValueFlowObjectID receiver, const HeapKey&);
    ClimbingSet receiversThatLookPastPropertiesOfCreation(ClimbingSet receivers, const HeapKey&);
    ClimbingSet receiversBelowAccessorNobodyKnows(ClimbingSet start, const HeapKey&, HeapPropertyPart);
    ClimbingSet objectsBelowWhatTheyWereCreatedBelow(ClimbingSet objects);

    void read(const ValueFlowOperand& object, const HeapKey&);
    void readOwn(ClimbingSet holders, ValueFlowName, ValueFlowNodeID result);
    void readOwn(const ValueFlowOperand& object, ValueFlowName name, ValueFlowNodeID result) { readOwn(m_context.valueOf(object), name, result); }
    void convertStoredValue(ClimbingSet receivers, const HeapKey&, const ValueFlowOperand& value);
    void test(const ValueFlowOperand& object, const HeapKey&);
    void write(ClimbingSet objects, ValueFlowName, const ValueFlowOperand& value);
    void noteWrittenMembers(ClimbingSet receivers, const HeapKey&, bool areOnlyListed = false);
    void noteMembersLookedForBeforeChange(ClimbingSet receivers, const HeapKey&, bool makesAllOnceFound = false);
    void noteDeletedMembers(ClimbingSet receivers, const HeapKey&);
    ValueSetID standingForClassAndThoseBelow(BuiltinClassID);
    ValueSetID allocatedByBuiltinsAs(ClimbingSet objects, ValueSetID standingForClasses);
    ValueSetID instancesThatKeepProperty(ClimbingSet objects, PropertyKeyID);
    void markRedefinition(ClimbingSet receivers, const HeapKey&);
    void markStore(ClimbingSet receivers, const HeapKey&);
    void markDefinition(ClimbingSet receivers, const HeapKey&, HeapPropertyPart);
    void define(const ValueFlowOperand& object, const HeapKey&, HeapPropertyPart, const ValueFlowOperand& value);
    ValueSetID objectsThatListingNamesUnder(const HeapKey& key) { return key.keyClass == HeapKeyClass::Name ? ValueSets::anything : ValueSets::complement(m_tables.prototypeObjectsThatListingDoesNotName()); }
    ClimbingSet builtinObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    void writeToListedObjects(ClimbingSet listed, ValueFlowName, const ValueFlowOperand& value);
    ClimbingSet unlockedObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    ClimbingSet builtinObjectsGivenByReadsUnderKeysNobodyKnows(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    ClimbingSet prototypeObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    ClimbingSet holdersOfReplaceableMemberThatUnknownObjectMayBe(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    // Bun stays listed beside the unknown object wherever it goes, so that a name read on it or on a copy of it is found. Anything else counts as done to Bun only where Bun is followed from a read under its name.
    ClimbingSet findListedBun(const ValueFlowOperand& object, ClimbingSet receivers) { return object.node && mayBeUnknownObject(receivers) ? m_context.valueWithin(ClimbingSet(only(m_universe.objectOf(Builtins::Bun))), object.node) : ClimbingSet(); }
    bool isBunNamedIn(const ValueFlowOperand& object, const HeapChain&);
    bool holdsNamedBun(const ValueFlowOperand& object) { return object.node && m_context.holdsAnyOf(ClimbingSet(only(ValueFlowUniverse::markOfNamedBun())), object.node); }
    bool holdsMarkOfMemberLeftOut(ValueFlowNodeID node) { return node && m_context.holdsAnyOf(ClimbingSet(only(ValueFlowUniverse::markOfMemberLeftOut())), node); }
    bool holdsMarkOfLoadedBun(ValueFlowNodeID node) { return node && m_context.holdsAnyOf(ClimbingSet(only(ValueFlowUniverse::markOfLoadedBun())), node); }
    // The mark does not go through an object nobody knows. Where the Bun that the program named is in unknown hands, what came out of it and escaped may come back as a value nobody knows.
    bool mayAnswerWhatEscapedOutOfBun() { return m_statement.result && hasEscaped(ClimbingSet(only(ValueFlowUniverse::markOfLoadedBun()))) && hasEscaped(ClimbingSet(only(ValueFlowUniverse::markOfNamedBun()))) && m_context.holdsAnyOf(unknownObjectAlone(), m_statement.result); }
    bool holdsNamedOrLoadedBun(const ValueFlowOperand& object) { return (holdsNamedBun(object) || holdsMarkOfLoadedBun(object.node)) && m_context.holdsAnyOf(ClimbingSet(only(m_universe.objectOf(Builtins::Bun))), object.node); }
    ValueSetID allButBunUnlessNamed(const ValueFlowOperand& object) { return holdsNamedBun(object) ? ValueSets::anything : ValueSets::complement(only(m_universe.objectOf(Builtins::Bun))); }
    ClimbingSet hostObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers) { return object.node && mayBeUnknownObject(receivers) ? m_context.valueWithin(ClimbingSet(m_context.maker().intersect(m_tables.holdersOfMembersThatHostMakes(), allButBunUnlessNamed(object))), object.node) : ClimbingSet(); }
    bool isInProgramCode();
    ClimbingSet holdersOfExportsListedBesideUnknownObjectInProgram(const ValueFlowOperand& object, ClimbingSet receivers) { return object.node && mayBeUnknownObject(receivers) && isInProgramCode() ? m_context.valueWithin(ClimbingSet(m_universe.holdersOfLibraryExports()), object.node) : ClimbingSet(); }
    ClimbingSet instancesBelowLibraryClassesListedBesideUnknownObjectInProgram(const ValueFlowOperand& object, ClimbingSet receivers) { return object.node && mayBeUnknownObject(receivers) && isInProgramCode() ? m_context.valueWithin(ClimbingSet(m_universe.instancesBelowLibraryClasses()), object.node) : ClimbingSet(); }
    void readExportsOfListedHolders(const ValueFlowOperand& object, ClimbingSet receivers);
    void letExportsThatHaveEscapedBeCalled(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    ValueFlowNodeID valueOfDescriptorsInAnswer();
    bool m_keepsHoldersOfExportsItStartsFrom { false };
    bool m_findsOwnPropertiesAlone { false };
    bool m_findsOwnPropertiesListedAbove { false };
    bool m_answersNothingForNameThatIsLacking { false };
    void noteClosuresThatMayBeRenamed(const ValueFlowOperand& object, const HeapKey&, ClimbingSet receivers);
    bool mayBeFunctionByType(const ValueFlowOperand&);
    void notePrototypeObjectsThatWriteMayReach(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey&);
    void store(const ValueFlowOperand& object, const HeapKey&, const ValueFlowOperand& value);
    void callSettersOfReceivers(ClimbingSet start, const HeapKey&, const ValueFlowOperand& value);
    ClimbingSet settersThatSymbolFinds(ClimbingSet setters, ClimbingSet start, const HeapKey&);
    void remove(const ValueFlowOperand& object, const HeapKey&);
    enum class CopiedProperties : uint8_t { AreDefinedAsValues, AreAssigned, AreDefinedAsTheyStand };
    void copyProperties(const ValueFlowOperand& target, const ValueFlowOperand& source, CopiedProperties);
    bool readsRestArgumentOfInstance();
    bool readsKeyOrValueOfPair();
    std::optional<ClimbingSet> objectsAndAllThatTheyInheritFrom(ClimbingSet);
    bool listsNamesOfEnumeration();
    bool forEachNameHeldThatBeginsWith(StringID beginning, ClimbingSet receivers, const ScopedLambda<void(PropertyKeyID)>&);
    bool findsNoPropertyOfEngineUnderKeyBeginningWith(StringID beginning, ClimbingSet receivers);
    void readWhatNamesOfProgramMayHaveHidden(const HeapChain&, ValueFlowNodeID result, Members& getters);
    void readOwnPropertiesListedAbove(ClimbingSet objects, ValueFlowNodeID result, Members& getters);
    bool hasMemberOfHost(ValueFlowObjectID holder) const;
    enum class DefinedObject : bool { IsNew, MayHoldProperty };
    bool saysOnlyWhetherPrototypeOfFunctionIsWritable(const ValueFlowOperand& object, const HeapKey&, ClimbingSet descriptors);
    void defineAsDescribed(const ValueFlowOperand& object, const HeapKey&, ClimbingSet descriptors, DefinedObject, bool gettersAreFoundByName = false);
    bool definesGettersUnderNamesOfEnumeratedParameter(const ValueFlowOperand& object, const ValueFlowStatement* operation);
    void compareWithPropertyThatIsThere(const ValueFlowOperand& defined);
    void noteNameDefinedThroughObjectsNobodyFollows(const ValueFlowOperand& object, const HeapKey&);
    void changePrototype(ClimbingSet objects, const ValueFlowOperand& prototype);
    enum class PropertySearch : uint8_t { OwnProperty, WholeChain };
    bool holdsOnlyWhatWasWritten(ClimbingSet receivers) { return !m_context.meet(receivers, m_context.maker().intersect(m_universe.objects(), ValueSets::complement(m_tables.objectsWithoutPropertiesOfNativeCode()))); }
    void readAccessorFunctions(const ValueFlowOperand& object, const HeapKey&, HeapPropertyPart, PropertySearch);
    bool replaceMembersThroughSettersFoundFrom(ClimbingSet receivers, const HeapKey&, PropertySearch, const ValueFlowOperand* value, bool isGivenAnyValue = false);

    void defineUndefinedOnObjectsNobodyKnows(const ValueFlowOperand& object, const ValueFlowOperand& value);
    ClimbingSet allocateUnder(const ValueFlowOperand& prototype);
    void runCreateArguments(const ValueFlowOperand& count, const ValueFlowOperand& rest, std::span<const ValueFlowOperand> parameters, const ValueFlowOperand* callee);

    const ValueFlowStatement& m_statement;
    ValueFlowRuleContext& m_context;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    const ValueFlowUniverse& m_universe;
    const ValueFlowHeapTables& m_tables;
    std::span<const ValueFlowOperand> m_operands;
    bool m_asksWithoutNodes { false };
    std::optional<bool> m_doesUnseenCodeRun;
    std::optional<bool> m_doesUnseenCodeChangeRealm;
    PropertyKeyID m_nameOfHoldersOfUndeclaredProperties;
    ValueSetID m_holdersOfUndeclaredPropertiesUnderName;
};

inline NEVER_INLINE void HeapRule::forEachKey(const ValueFlowOperand& property, const ScopedLambda<void(const HeapKey&)>& functor, HeapKeyUse use)
{
    ClimbingSet keys = m_context.valueOf(property);
    connect(property, m_context.hub(m_universe.builtinTables().conversionToPrimitive()));

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
    auto nameTextsOfPrimitives = [&] {
        BuiltinName lastText;
        for (auto [kind, text] : textsOfPrimitives) {
            if (text != lastText && m_context.contains(keys, ValueFlowUniverse::primitive(kind))) {
                functor(keyNamed(m_program.keyOf(text)));
                lastText = text;
            }
        }
    };

    if (m_context.meet(otherKeys, m_tables.keysNobodyKnows())) {
        HeapKeyReceivers receiversOfSymbols = receiversOfSymbolNobodyKnows(otherKeys, use);
        if (receiversOfSymbols == HeapKeyReceivers::AllButBuiltinObjects) {
            HeapKey nobodyKnows = unknownHeapKey;
            nobodyKnows.namesNoSymbol = !m_context.meet(otherKeys, m_context.maker().intersect(m_tables.keysNobodyKnows(), ValueSets::complement(only(ValueFlowUniverse::primitive(PrimitiveSet::String)))));
            nobodyKnows.namesSymbolAlone = !m_context.meet(otherKeys, ValueSets::complement(m_universe.symbols()));
            functor(nobodyKnows);
            if (nobodyKnows.namesSymbolAlone)
                nameTextsOfPrimitives();
            if (!use.isRead || !property.node)
                return;
            auto forEachKeyListedAmong = [&](ValueSetID all, NOESCAPE const Invocable<void(const HeapKey&)> auto& take) {
                ClimbingSet listed(all);
                if (!holdsAnything(m_context.membersMissingFrom(property.node, listed)))
                    return;
                m_context.forEachMember(m_context.valueWithin(listed, property.node), [&](ValueFlowObjectID constant) {
                    take(keyNamed(m_universe.keyOf(constant)));
                });
            };
            // A key nobody knows names no member of Bun. A name that comes beside it does. Any string holds every name, and names none.
            forEachKeyListedAmong(m_tables.keysOfMembersOfBun(), functor);
            // Nor does it name a member of an object of the library that is found by name alone. A symbol that comes beside it does.
            forEachKeyListedAmong(m_context.maker().intersect(m_universe.symbols(), m_universe.keys()), [&](const HeapKey& key) {
                if (holdsAnything(m_context.holdersOfLibraryExportsWrittenUnder(key.name)))
                    functor(key);
            });
            return;
        }
        if (receiversOfSymbols == HeapKeyReceivers::BuiltinObjects)
            functor(HeapKey { HeapKeyClass::Unknown, { }, false, HeapKeyReceivers::AllButBuiltinObjects, false, false, true });
        if (use.stored)
            connect(*use.stored, ValueFlowProgramNodes::escaped());
        m_context.forEachMember(ClimbingSet(m_context.maker().intersect(m_universe.symbols(), m_universe.keys())), [&](ValueFlowObjectID constant) {
            HeapKey key = keyNamed(m_universe.keyOf(constant));
            key.receivers = receiversOfSymbols;
            functor(key);
        });
    }

    nameTextsOfPrimitives();

    bool namesIndex = m_context.meet(keys, m_tables.numberKinds());
    AOT_THRESHOLD(uint32_t, mostNamesOfOneKeyToldApart, 32);
    bool countsNamesOfProgramTogether = !use.tellsEveryNameApart && m_context.holdsMoreMembersThan(m_context.restrict(keys, m_universe.keys()), mostNamesOfOneKeyToldApart);
    bool hasNameOfProgram = false;
    m_context.forEachMemberInLine(m_context.restrict(keys, m_universe.keys()), [&](ValueFlowObjectID constant) {
        HeapKey key = keyNamed(m_universe.keyOf(constant));
        if (key.keyClass == HeapKeyClass::Index) {
            namesIndex = true;
            return;
        }
        key.isComputed = true;
        if (countsNamesOfProgramTogether && m_tables.under(key.name).holders == ValueSets::nothing) {
            hasNameOfProgram = true;
            key.receivers = HeapKeyReceivers::BuiltinObjects;
        }
        functor(key);
    });
    if (hasNameOfProgram)
        functor(HeapKey { HeapKeyClass::Unknown, { }, false, HeapKeyReceivers::AllButBuiltinObjects });
    if (namesIndex)
        functor(indexHeapKey);
}

void HeapRule::forEachKeyOfOperation(uint32_t computedKeyIndex, NOESCAPE const Invocable<void(const HeapKey&)> auto& functor, HeapKeyUse use)
{
    if (PropertyKeyID name = m_statement.key)
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
        ClimbingSet lists = m_context.valueOf(keyOperand);
        if (m_context.meet(lists, m_universe.placeholdersAlone()) || hasEscaped(known(lists)) || hasBeenLost(known(lists)))
            functor(unknownHeapKey);
        else
            forEachKey(operandOf(m_context.readWhatWasWritten(known(lists), elementsName)), functor);
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
    if (ValueFlowNodeID stored = storedThroughUnknownObjectIntoObjectsWithinReach(objects, name))
        functor(stored);
    else {
        for (ValueFlowNodeID stored : storedByStatementsThroughUnknownObjectInto(objects, name)) {
            if (stored)
                functor(stored);
        }
    }
    if (ValueFlowNodeID stored = storedThroughUntrackedObjectInto(objects, name))
        functor(stored);
}

void HeapRule::forEachNodeHolding(ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    functor(m_context.readWhatWasWritten(objects, name));
    forEachNodeStoredThroughPlaceholdersInto(objects, name, functor);
}

void HeapRule::forEachNodeReadByName(ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    functor(m_context.readWhatWasWritten(foundThroughUntrackedObject(m_universe.knownObjects()), name));
    functor(m_context.readWhatWasWritten(untrackedObjectAlone(), name));
    functor(m_context.readWhatWasWritten(unknownObjectAlone(), name));
}

inline NEVER_INLINE void HeapRule::forEachMemberUnder(ClimbingSet objects, const HeapKey& key, const ScopedLambda<void(BuiltinID member, ModuleID)>& functor)
{
    m_context.forEachMemberInLine(m_context.restrict(objects, holdersWithMemberUnder(key)), [&](ValueFlowObjectID holder) {
        ModuleID module = m_universe.moduleOf(holder);
        if (key.keyClass == HeapKeyClass::Name) {
            if (BuiltinID member = memberUnder(m_universe.builtinOf(holder), key.name))
                functor(member, module);
            return;
        }
        for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder))) {
            if (key.keyClass != HeapKeyClass::Index || m_tables.hasNumericName(member))
                functor(member, module);
        }
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
