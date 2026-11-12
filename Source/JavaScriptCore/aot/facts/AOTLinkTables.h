/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTCellFormat.h"
#include "AOTCodeRole.h"
#include "AOTEffects.h"
#include "AOTFamilies.h"
#include "AOTLargeList.h"
#include "AOTLinkCoroutineTables.h"
#include "AOTLinkKnownEffects.h"
#include "AOTMechanisms.h"
#include "AOTTestRuns.h"
#include "AOTThinSummary.h"
#include "AOTUnionTaggedMakers.h"

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class BodyIndexTable;

struct LinkedParameter {
    TypeID writtenOrSeenToArrive() const { return written ? written : seenToArrive; }

    TypeID written;
    TypeID seenToArrive;
    TypeID proven;
    bool staysAsItsClassMakesIt { false };
    bool isOnlyCalled { false };
    bool isCapturedWhereOnlyCalled { false };
    bool isOnlyLookedAt { false };
};

struct LinkedFunction {
    bool isReached { true };
    bool everyCallerIsKnown { false };
    bool everyCallHasOneCallee { false };
    bool everyTakerOfResultKnowsCallee { false };
    bool hasSingleCallSite { false };
    bool ringsOfTailCallsAreListed { false };
    bool runsAtMostOnce { false };
    bool isIterationCallback { false };
    bool isRepeatedCallback { false };
    bool isNamedInItsCycle { true };
    bool liesUnderNameOrIsLost { false };
    bool isInQueueFromStart { false };
    bool callEntryWalksBody { false };
    bool namesOnlyWhatEveryFunctionLeadsTo { false };
    bool closuresAreChanged { false };
    bool closuresGetOnlyNamesThatAreListed { false };
    bool prototypePropertyStaysAsMade { false };
    bool holdsInt32Promise { false };
    bool neverReturns { false };
    uint32_t cycle { 0 };
    TypeID result;
    uint32_t firstParameter { 0 };
    uint32_t numberOfParameters { 0 };
    static constexpr uint32_t notLeaf = UINT32_MAX;
    uint32_t weightAsLeaf { notLeaf };
    const Effects* keptEffectsOnReturn { &effectsOfAnything };
    const Effects* keptEffectsOnThrow { &effectsOfAnything };

    const Effects& effectsOnReturn() const { return *keptEffectsOnReturn; }
    const Effects& effectsOnThrow() const { return *keptEffectsOnThrow; }
};

struct LinkedCell {
    friend bool operator==(const LinkedCell&, const LinkedCell&) = default;

    TypeID proven;
    bool leavesOutWhatArrives { false };
    bool everyWriterIsKnown { false };
    bool isWrittenAfterConstruction { true };
    bool isStoredInto { true };
    bool staysAsItsClassMakesIt { false };
    bool isFilled { false };
    bool isFilledAtEveryRead { false };
    bool isOfBirth { false };
    bool isFilledByAccessorsAlone { false };
    bool holdsAccessorFromBirth { false };
    bool keepsAccessorOnceLaid { false };
    bool isOnlyGivenUndefinedByMakers { false };
    bool isLeftUndefinedByMakers { false };
    bool isGivenUndefinedOrNullInSight { false };
    bool isGivenByNoMaker { false };
    bool isKeptFromComparisons { false };
    bool isTestedByValue { false };
    OptionSet<FieldForm> formThatDefinitionsCreate;
    bool liesBesideWhatDescriptorsDefine { false };
};

inline bool holdsTagThatNarrowSlotForgets(TypeReader types, const LinkedCell& cell)
{
    if (!cell.isTestedByValue || !cell.everyWriterIsKnown || !cell.proven)
        return false;
    Type proven = types.at(cell.proven);
    bool isBooleanOrInteger = proven.primitives == Primitive::True || proven.primitives == Primitive::False || proven.primitives == Primitive::Integer;
    return isBooleanOrInteger && !proven.hasObjects() && isSingleValue(types, proven);
}

enum class LayoutChange : uint8_t {
    IsFrozen = 1 << 0,
    GainsProperty = 1 << 1,
    LosesProperty = 1 << 2,
    ChangesPrototype = 1 << 3,
    GetsAccessor = 1 << 4,
    GetsObjectAbove = 1 << 5,
};

inline constexpr uint32_t maxAppendedFieldsOutOfLine = maxExtensionGranules;
inline constexpr size_t fieldsOfLargeLayout = 64;
inline size_t maxAppendedFieldsOf(size_t numberOfListedFields) { return numberOfListedFields < fieldsOfLargeLayout ? maxAppendedFieldsOutOfLine : std::max<size_t>(maxAppendedFieldsOutOfLine, numberOfListedFields / 2); }

struct LinkedLayout {
    FamilyID family;
    bool isAllocated { true };
    bool hasDirectInstances { true };
    bool isMadeInSight { true };
    bool reachesUnseenCode { true };
    bool hasLockableInstances { true };
    OptionSet<LayoutChange> possibleChanges { LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor, LayoutChange::GetsObjectAbove };
    bool isRedefinedByLiteralsAlone { false };
    bool isDefinedOnByDescriptorsThatLayValuesAlone { false };
    bool takesCellsOfBase { false };
    bool listsEveryMemberAbove { false };
    bool listsEveryStaticMember { false };
    std::array<bool, 2> namesAreDefinedPastLists { true, true };
    bool hasConstructorFunctionInRow { true };
    bool isFilledUnderComputedKeys { false };
    bool isExtendedAlone { true };
    bool instancesAreLaidAsPrototypes { false };
    bool onlyOwnObjectsStandBelowPrototype { false };
    bool prototypeObjectIsSettledAheadOfEveryObject { false };
    LayoutID heritage;
    bool hasSeveralHeritages { false };
    FunctionID onlyFunctionThatMakesObjects;
};

struct LinkedMember {
    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MethodKind kind { MethodKind::Method };
    uint32_t index { 0 };
    PropertyKeyID key;
    FunctionID function;
    bool everyWriterIsKnown { false };
    FunctionID written;
    FunctionID replacement { };
    bool isReplaced { false };
    bool isAssignedToClass { false };

    bool holdsOneFunction() const { return everyWriterIsKnown && function; }
};

struct ReplacedMethod {
    FunctionID replacement;
    FunctionID written;
    PropertyKeyID key;

    friend constexpr auto operator<=>(const ReplacedMethod&, const ReplacedMethod&) = default;
};

struct LinkedNameOnPrototype {
    LayoutID layout;
    PropertyKeyID key;
    FunctionID function;
    FunctionID expected;
    FunctionID expectedGetter;
    FunctionID expectedSetter;
    bool isStoredIntoFromOutside { false };
    bool isUnderSymbolsAlone { false };
    bool mayHoldAccessor { false };
    TypeID primitives;
    TypeID seen;
    bool mayBeReadOnly { true };
    bool isLaidFirst { false };
};

struct LinkedAssignment {
    std::pair<LayoutID, PropertyKeyID> place() const { return { layout, key }; }

    LayoutID layout;
    PropertyKeyID key;
    FunctionID method;
};

struct LinkedNameOnFunction {
    FunctionID holder;
    PropertyKeyID key;
    FunctionID expected;
    TypeID seen;
};

struct LinkedNameBesideFields {
    LayoutID layout;
    PropertyKeyID key;

    friend constexpr auto operator<=>(const LinkedNameBesideFields&, const LinkedNameBesideFields&) = default;
};

struct LinkTables;
void appendMembersAssignedByProgram(const Program&, const LinkTables&, LayoutID, Vector<ClassMember>&);

struct MemberFound {
    enum class Kind : uint8_t {
        Unknown,
        Method,
        Accessor,
    };

    Kind kind { Kind::Unknown };
    FunctionID method;
    FunctionID getter;
    FunctionID setter;
};

class FindsNothing {
    friend struct LinkTables;
    FindsNothing() = default;
};

enum class Of : uint8_t { ObjectItself, HolderItself, HolderAndAbove };

struct LinkedGroup {
    enum class Kind : uint8_t {
        Anybody,
        Constructors,
        FunctionsInFields,
        SharedCall,
    };

    Kind kind { Kind::Anybody };
    uint32_t subject { 0 };
    uint32_t cycle { 0 };
};

struct LinkedCall {
    FunctionID caller;
    ValueID site;
    FunctionID callee;
    bool isInTailPosition { false };
    bool mayComeAgain { true };
    bool asksForIterator { false };

    friend constexpr auto operator<=>(const LinkedCall&, const LinkedCall&) = default;
};

struct LinkedCallThatFindsBuiltin {
    FunctionID caller;
    ValueID site;
    BuiltinID builtin;
    FunctionID callee;

    friend constexpr auto operator<=>(const LinkedCallThatFindsBuiltin&, const LinkedCallThatFindsBuiltin&) = default;
};

enum class OrderOfWorklist : uint8_t { NewestFirst, OldestFirst, Scattered };

enum class WhatUnseenCodeDoes : uint8_t { Anything, OnlyReadsAndCalls };

struct LinkedAccess {
    FunctionID function;
    ValueID site;
    PropertyKeyID key;

    friend constexpr auto operator<=>(const LinkedAccess&, const LinkedAccess&) = default;
};

struct LinkedGetterAtRead {
    FunctionID function;
    ValueID site;
    FunctionID getter;
    bool isHeardInLoopsAlone { false };

    friend constexpr auto operator<=>(const LinkedGetterAtRead&, const LinkedGetterAtRead&) = default;
};

struct LinkedFunctionInRecord {
    FunctionID function;
    LayoutID record;

    friend constexpr auto operator<=>(const LinkedFunctionInRecord&, const LinkedFunctionInRecord&) = default;
};

struct LinkedResultOfRead {
    FunctionID function;
    ValueID site;
    TypeID proven;
};

struct LinkedNativeCallResult {
    FunctionID function;
    ValueID site;
    TypeID proven;
};

struct LinkedContentsAtOperation {
    FunctionID function;
    ValueID site;
    TypeID key;
    TypeID value;
};

struct LinkedReceiverAsMade {
    FunctionID function;
    ValueID site;

    friend constexpr auto operator<=>(const LinkedReceiverAsMade&, const LinkedReceiverAsMade&) = default;
};

inline std::optional<uint32_t> indexWrittenAtInLinkTables(const SummaryAccess& access)
{
    return access.kind == SummaryAccess::Kind::Read && access.writtenIndexOfRead != noSummaryNode ? std::optional(access.writtenIndexOfRead) : std::nullopt;
}

struct LinkedReadOffViews {
    FunctionID function;
    ValueID site;
    uint32_t leastLength { 0 };

    friend bool operator==(const LinkedReadOffViews&, const LinkedReadOffViews&) = default;
    friend auto operator<=>(const LinkedReadOffViews&, const LinkedReadOffViews&) = default;
};

struct LinkedVariableStoredFirst {
    FunctionID body;
    FunctionID functionWhoseUseCounts;
    VariableID variable;

    friend constexpr auto operator<=>(const LinkedVariableStoredFirst&, const LinkedVariableStoredFirst&) = default;
};

struct LinkedMethodStoredFirst {
    FunctionID holder;
    PropertyKeyID key;
    FunctionID method;

    friend constexpr auto operator<=>(const LinkedMethodStoredFirst&, const LinkedMethodStoredFirst&) = default;
};

struct LinkedMethodOnPrototype {
    FunctionID body;
    ValueID site;

    friend constexpr auto operator<=>(const LinkedMethodOnPrototype&, const LinkedMethodOnPrototype&) = default;
};

struct LinkedCatch {
    FunctionID function;
    BlockID handler;
    TypeID caught;
};

struct LinkedAccessorsOfField {
    FieldReference field;
    FunctionID getter;
    FunctionID setter;

    bool getterIsProven { false };
    bool setterIsProven { false };
    bool getterMayNotBeLaidYet { false };
    friend constexpr auto operator<=>(const LinkedAccessorsOfField&, const LinkedAccessorsOfField&) = default;
};

struct TiesOfPoolsInLinkFlow {
    uint64_t all { 0 };
    uint64_t ofRowsOfUnequalLength { 0 };
    uint64_t behindAnotherOfSameSettling { 0 };
};

struct SizeOfLinkFlow {
    uint64_t nodes { 0 };
    uint64_t edges { 0 };
    uint64_t observers { 0 };
    uint64_t nodesTakenOffWorklist { 0 };
    uint64_t observersTold { 0 };
    uint64_t rounds { 0 };
    TiesOfPoolsInLinkFlow tiesOfPools;
};

struct WriteFollowedToBuiltin {
    FunctionID function;
    ValueID site;
    BuiltinID holder;
    Vector<PropertyKeyID> namesBrought { };
    bool knowsNamesBrought { false };
};

struct LinkedCallOfBuiltin {
    FunctionID caller;
    ValueID site;
    BuiltinID row;
    bool isConstruction { false };
};

struct LookupsPastProgramObjects {
    BitVector namesRead;
    BitVector namesCalled;
    BitVector namesCalledWithOperands;
    BitVector namesCalledByNativeCodeOffUnseenObjects;
    BitVector namesSpelledByProgram;
    BitVector builtinsRead;
    BitVector builtinsCalled;
    BitVector builtinsConstructed;
    BitVector builtinsCalledByCode;
    BitVector builtinsLetGo;
    BitVector builtinsUsedViaFlow;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> builtinsFoundWhereFlowBroughtHolders;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> constructorsBelowBuiltins;
    BitVector builtinsCalledOutOfSight;
    BitVector builtinsCalledWithoutRow;
    BitVector builtinsCalledWithOperandsNobodyRead;
    BitVector holdersReadUnderKeysNobodyKnows;
    BitVector rowsApplied;
    BitVector builtinsWithCallsLeftToUnseen;
    BitVector builtinsWithCallsThatMayRunCode;
    UncheckedKeyHashSet<uint64_t> conditionsThatOperandsMayMeet;
    Vector<LinkedCallOfBuiltin> callsOfBuiltinsAtSites;
    // MUST BE TRUE OF THE PROGRAM THAT RUNS: each is locked at the start (Facts::builtinMembersLockedAtStartUp()), with every mechanism withheld too: what is reached hangs on it.
    BitVector membersTakenForConstants;
    BitVector membersWritten;
    Vector<WriteFollowedToBuiltin> writesFollowedToBuiltins;
    BitVector holdersWrittenUnderAnyName;
    BitVector holdersStoredOnUnderAnyName;
    BitVector holdersWhoseMembersAreDeleted;
    BitVector holdersHandedWhatIsNoTextNumberOrBoolean;
    bool objectsNobodyKnowsAreHandedWhatIsNoTextNumberOrBoolean { false };
    BitVector lostBuiltins;
    BitVector builtinsAtUnseen;
    BitVector builtinsPassedOptionThatRunsCode;
    BitVector classesWithInstancesAtUnseen;
    BitVector classesWhoseInstancesHaveEscaped;
    BitVector classesHandedToFormatters;
    bool formattersAreHandedInstancesOfSeveralClasses { false };
    bool formattersAreHandedObjects { false };
    bool instancesLoseTheirConstructor { false };
    bool formattersMayCallMethodsOfInstances { false };
    BitVector typesHandedToHandlers;
    BitVector entriesThatHandHostNothing;
    BitVector entriesThatHostStartsByCallInSight;
    BitVector namesReadThroughLostBuiltins;
    BitVector namesWrittenThroughLostBuiltins;
    BitVector namesWrittenThroughUnseenByProgram;
    BitVector namesWrittenThroughUnseenByLibrary;
    bool writesUnderAnyNameThroughUnseenByProgram { false };
    bool writesUnderAnyNameThroughLostBuiltins { false };
    BitVector namesWrittenThroughLostBuiltinsBesideProduct;
    BitVector namesWrittenThroughUnseenBesideProduct;
    bool writesUnderAnyNameThroughUnseenBesideProduct { false };
    bool writesUnderAnyNameThroughLostBuiltinsBesideProduct { false };
    bool climbsFromLostBuiltins { false };
    bool climbsFromUnseenObjects { false };
    bool lostBuiltinsReachUnseen { false };
    bool readsUnderKeysNobodyKnows { false };
    bool readsUnderNamesThatLoopsGave { false };
    UncheckedKeyHashSet<uint64_t> readsOfProgramUnderKeysNobodyKnows;
    bool wereRuntimeNamesDeferred { false };
    BitVector staticallyUsedBuiltins;
    BitVector staticallyInstantiatedClasses;
    bool nativeCodeMayHoldObjects { false };
};

class FieldsOfNumbers {
public:
    void add(LayoutID declarer, PropertyKeyID key) { m_fields.add(wordOf(declarer, key)); }
    void merge(const FieldsOfNumbers& other)
    {
        for (uint64_t field : other.m_fields)
            m_fields.add(field);
    }
    bool contains(LayoutID declarer, PropertyKeyID key) const { return m_fields.contains(wordOf(declarer, key)); }
    bool names(const Program&, LayoutID, PropertyKeyID) const;

private:
    static uint64_t wordOf(LayoutID declarer, PropertyKeyID key) { return (static_cast<uint64_t>(declarer.index()) + 1) << 32 | key.index(); }

    UncheckedKeyHashSet<uint64_t> m_fields;
};

class NamesOfModules {
public:
    void add(ModuleID module, PropertyKeyID key) { m_names.add(wordOf(module, key)); }
    void merge(const NamesOfModules& other)
    {
        for (uint64_t name : other.m_names)
            m_names.add(name);
    }
    bool contains(ModuleID module, PropertyKeyID key) const { return m_names.contains(wordOf(module, key)); }

private:
    static uint64_t wordOf(ModuleID module, PropertyKeyID key) { return (static_cast<uint64_t>(module.index()) + 1) << 32 | key.index(); }

    UncheckedKeyHashSet<uint64_t> m_names;
};

struct LinkedFunctionUnderName {
    uint32_t variable { 0 };
    uint32_t key { 0 };
    FunctionID function;

    friend constexpr auto operator<=>(const LinkedFunctionUnderName&, const LinkedFunctionUnderName&) = default;
};

struct LinkedFunctionWrittenOverBuiltin {
    BuiltinID holder;
    uint32_t key { 0 };
    FunctionID function;

    friend constexpr auto operator<=>(const LinkedFunctionWrittenOverBuiltin&, const LinkedFunctionWrittenOverBuiltin&) = default;
};

struct LinkedGlobalStore {
    FunctionID function;
    uint32_t access;

    friend constexpr auto operator<=>(const LinkedGlobalStore&, const LinkedGlobalStore&) = default;
};

class Program;
struct LinkTables;
bool hasLockableInstances(const Program&, const LinkTables&, TypeID testedType);

struct LinkedFirstStores {
    std::span<const LinkedVariableStoredFirst> variablesStoredFirstIn(FunctionID body) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(variablesStoredFirst, body, { }, &LinkedVariableStoredFirst::body);
        return { found.begin(), found.end() };
    }

    Vector<LinkedVariableStoredFirst> variablesStoredFirst;
    Vector<SummaryClassInVariable> classesInVariables;
    Vector<LinkedMethodOnPrototype> methodsOnPrototypes;
};

struct LinkTables : LinkedFirstStores {
    std::unique_ptr<Families> familiesBehindFlow;
    BitVector exportsThatStayIn;
    Vector<std::pair<VariableID, FunctionID>> functionsTakenOutOfRequiredFiles;
    Vector<LinkedGlobalStore> globalStores;
    bool listsGlobalStores { false };
    Vector<LinkedFunctionUnderName> functionsLaidUnderNamesOfVariables;
    Vector<LinkedFunctionWrittenOverBuiltin> functionsWrittenOverBuiltins;
    BitVector modulesWhoseValueStaysIn;
    BitVector modulesWhoseRecordIsHandedOutFrozen;
    LargeList<LinkedFunction> functions;
    LargeList<LinkedParameter> parameters;
    Vector<LinkedCall> calls;
    Vector<LinkedCallThatFindsBuiltin> callsThatFindBuiltins;
    BitVector callersThatFindBuiltins;
    Vector<uint32_t> firstCallOfCaller;
    Vector<LinkedAccess> accessesUnderOneKey;
    Vector<PropertyKeyID> symbolsWithPreparation;
    bool everySymbolOfLibraryMayHavePreparation { true };
    bool lookupUnderKeyMayRunPreparation(const Program&, PropertyKeyID) const;
    Vector<LinkedGetterAtRead> gettersExpectedAtReads;
    Vector<LinkedFunctionInRecord> functionsInRecords;
    Vector<LinkedCoroutine> coroutines;
    const LinkedCoroutine* findCoroutine(FunctionID) const;
    Vector<LinkedResultOfRead> resultsOfReadsByKey;
    Vector<LinkedNativeCallResult> nativeCallResults;
    BitVector nativeFunctionCallers;
    Vector<LinkedContentsAtOperation> contentsAtOperations;
    Vector<LinkedReceiverAsMade> receiversThatStayAsMade;
    Vector<LinkedReceiverAsMade> receiversThatNobodyChanges;
    Vector<LinkedReceiverAsMade> iterablesThatStayAsMade;
    Vector<LinkedReceiverAsMade> listsWithoutHoles;
    Vector<LinkedReceiverAsMade> plainListsRead;
    Vector<LinkedReceiverAsMade> iterablesWithoutHoles;
    Vector<LinkedReadOffViews> readsOffViewsOfWrittenLengths;
    FieldsOfNumbers fieldsReadAsIntegers;
    NamesOfModules namesReadAsIntegers;
    FieldsOfNumbers fieldsReadForArithmetic;
    FieldsOfNumbers fieldsStoredWhatIsNoInt32;
    FieldsOfNumbers fieldsUsedInProducts;
    BitVector functionsDoneInIterations;
    BitVector functionsCalledInLoops;
    BitVector keysCalledInLoops;
    bool elementsAreCalledInLoops { false };
    BitVector functionsInElements;
    bool isCalledInLoops(FunctionID function) const { return functionsCalledInLoops.get(function.index()) || functionsInElements.get(function.index()); }
    BitVector functionsCalledByValueInLoops;
    BitVector constructorsCalledInLoops;
    BitVector functionsCalledInLoopsOfOwnFile;
    FieldsOfNumbers fieldsStoredCounts;
    BitVector keysStoredWhatIsNoInt32;
    Vector<LinkedAccessorsOfField> accessorsExpectedInFields;
    BitVector keysOfAccessorsExpectedInFields;
    BitVector keysOfStaticFields;
    Vector<NameAddedByTests> namesAddedByTests;
    Vector<VariableID> exportsSpiedOnThatWireLeavesShut;
    LinkKnownEffects known;
    Vector<LinkedCatch> catches;
    Vector<LinkedGroup> groups;
    Vector<uint32_t> firstReferrer;
    Vector<FunctionID> referrers;
    Vector<uint32_t> rankOfCycle;
    Vector<uint32_t> firstCycleAwaited;
    Vector<uint32_t> cyclesAwaited;
    Vector<uint32_t> firstCycleNamed;
    Vector<uint32_t> cyclesNamed;
    Vector<uint32_t> firstFieldOfLayout;
    LargeList<LinkedCell> fields;
    Vector<uint32_t> firstAppendedFieldOfLayout;
    Vector<PropertyKeyID> appendedKeys;
    Vector<uint32_t> firstKeyAppendedByHeldAloneOfLayout;
    Vector<PropertyKeyID> keysAppendedByHeldAlone;
    BitVector keysAppendedByHeldAloneThatLieInCell;
    Vector<LinkedCell> appendedFields;
    uint32_t numberOfCellsExpectedByNamesAsked { 0 };
    BitVector appendedFieldsThatSomeWrittenTypeRequires;
    BitVector appendedFieldsThatTypeOfThisRequires;
    BitVector appendedFieldsLaidLate;
    BitVector recordsThatSpreadsFill;
    BitVector keysAppendedByHeldAloneLaidLate;
    BitVector appendedFieldsThatTakeWordLeftInCell;
    BitVector layoutsFillingLateFieldsTogether;
    BitVector appendedFieldsThatFollowersOfMakersStore;
    BitVector keysAppendedByHeldAloneThatFollowersOfMakersStore;
    Vector<LinkedNameBesideFields> namesStoredBesideFields;
    Vector<uint32_t> firstRunOfMakersOfLayout;
    Vector<uint32_t> firstKeyOfRunOfMakers;
    Vector<PropertyKeyID> keysOfRunsOfMakers;
    struct RunBehindTaggedMakers {
        FunctionID body;
        ValueID firstStore;
        uint32_t firstKey { 0 };
        uint32_t numberOfKeys { 0 };
        uint32_t firstMaker { 0 };
        uint32_t numberOfMakers { 0 };
    };
    Vector<RunBehindTaggedMakers> runsBehindTaggedMakers;
    Vector<PropertyKeyID> keysOfRunsBehindTaggedMakers;
    Vector<LayoutID> makersOfRunsBehindTaggedMakers;
    LargeList<LinkedCell> variables;
    Vector<uint32_t> firstCaptureOfFunction;
    LargeList<LinkedCell> captures;
    LargeList<LinkedLayout> layouts;
    Vector<LinkedMember> members;
    Vector<LinkedMember> membersAboveCreatedLayouts;
    Vector<ReplacedMethod> replacedMethods;
    Vector<LinkedAssignment> lastAssignments;
    Vector<LinkedNameOnPrototype> accessorsAssignedToClasses;
    BitVector functionsReplacingMethods;
    bool someAccessorIsReplaced { false };
    Vector<uint32_t> membersByKey;
    Vector<uint32_t> firstMemberByKeyOfSide;
    Vector<uint32_t> membersByKeyAlongRows;
    Vector<uint32_t> placesOfLayoutsInRows;
    Vector<uint32_t> endsOfLayoutsBelowInRows;
    Vector<uint32_t> numbersOfClassesAbove;
    std::array<BitVector, 2> keysOfMembers;
    Vector<LinkedNameOnPrototype> namesOnPrototypes;
    Vector<LayoutID> rootsOfClassRows;
    Vector<std::pair<FunctionID, PropertyKeyID>> functionsWrittenRightOfNamesOnPrototypes;
    Vector<TaggedMaker> taggedMakers;
    SizeOfLinkFlow sizeOfFlow;
    LookupsPastProgramObjects lookups;
    bool unseenCodeCanRun { true };

    const LinkedFunction& function(FunctionID id) const LIFETIME_BOUND { return functions[id.index()]; }
    static constexpr uint32_t numbersStoredUnderName = (1u << 31) - 1;
    static constexpr uint32_t integersStoredUnderName = (1u << 31) - 2;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> classesStoredUnderNames;
    static constexpr uint64_t layoutAndName(LayoutID layout, PropertyKeyID key) { return static_cast<uint64_t>(layout.index()) << 32 | key.bits(); }
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> classesPassedToParameters;
    std::span<const LinkedParameter> parametersOf(FunctionID id) const LIFETIME_BOUND { return parameters.span().subspan(function(id).firstParameter, function(id).numberOfParameters); }
    std::span<const FunctionID> referrersOf(FunctionID node) const LIFETIME_BOUND { return referrers.span().subspan(firstReferrer[node.index()], firstReferrer[node.index() + 1] - firstReferrer[node.index()]); }
    uint32_t numberOfNodesOfReferrers() const { return static_cast<uint32_t>(functions.size() + groups.size()); }
    bool isGroup(FunctionID node) const { return node.index() >= functions.size(); }
    const LinkedGroup& group(FunctionID node) const LIFETIME_BOUND { return groups[node.index() - functions.size()]; }
    std::span<const uint32_t> cyclesAwaitedBy(uint32_t cycle) const LIFETIME_BOUND { return cyclesAwaited.span().subspan(firstCycleAwaited[cycle], firstCycleAwaited[cycle + 1] - firstCycleAwaited[cycle]); }
    std::span<const uint32_t> cyclesNamedBy(uint32_t cycle) const LIFETIME_BOUND { return cyclesNamed.span().subspan(firstCycleNamed[cycle], firstCycleNamed[cycle + 1] - firstCycleNamed[cycle]); }
    void listCyclesNamed();
    bool cycleLeadsTo(uint32_t referrer, uint32_t named) const;
    bool leadsTo(FunctionID referrer, FunctionID function) const { return cycleLeadsTo(functions[referrer.index()].cycle, functions[function.index()].cycle); }
    bool bodyTakesWhatCallersLeave(FunctionID id) const { return functions[id.index()].everyCallerIsKnown && functions[id.index()].everyCallHasOneCallee && !functions[id.index()].isNamedInItsCycle; }
    bool entryByValueMayBeClosed(FunctionID id) const { return functions[id.index()].isReached && functions[id.index()].everyCallerIsKnown && functions[id.index()].everyCallHasOneCallee && !functions[id.index()].holdsInt32Promise; }
    bool standsOutsideCycles(CodeUnit unit) const { return namesOnlyUnitsOfItsFunction(unit.role) && functions[unit.function.index()].isInQueueFromStart && !(unit.role == CodeRole::GenericCallEntry && functions[unit.function.index()].callEntryWalksBody); }
    bool leadsTo(CodeUnit referrer, FunctionID function) const { return leadsTo(referrer.function, function) && (!standsOutsideCycles(referrer) || functions[function.index()].isInQueueFromStart); }
    FunctionID functionTakenOutOfRequiredFileBy(VariableID variable) const
    {
        auto found = std::ranges::lower_bound(functionsTakenOutOfRequiredFiles, variable, { }, &std::pair<VariableID, FunctionID>::first);
        return found != functionsTakenOutOfRequiredFiles.end() && found->first == variable ? found->second : FunctionID();
    }
    FunctionID functionLaidUnderNameOf(VariableID variable, PropertyKeyID key) const
    {
        auto found = std::ranges::lower_bound(functionsLaidUnderNamesOfVariables, std::pair(variable.index(), key.bits()), { }, [](const LinkedFunctionUnderName& laid) {
            return std::pair(laid.variable, laid.key);
        });
        return found != functionsLaidUnderNamesOfVariables.end() && found->variable == variable.index() && found->key == key.bits() ? found->function : FunctionID();
    }
    bool isWrittenOverBuiltin(FunctionID function) const
    {
        return std::ranges::any_of(functionsWrittenOverBuiltins, [&](const LinkedFunctionWrittenOverBuiltin& written) { return written.function == function; });
    }
    FunctionID functionWrittenOverBuiltin(BuiltinID holder, PropertyKeyID key) const
    {
        auto found = std::ranges::lower_bound(functionsWrittenOverBuiltins, std::pair(holder, key.bits()), { }, [](const LinkedFunctionWrittenOverBuiltin& written) {
            return std::pair(written.holder, written.key);
        });
        return found != functionsWrittenOverBuiltins.end() && found->holder == holder && found->key == key.bits() ? found->function : FunctionID();
    }
    bool mayBeMentionedBy(CodeUnit referrer, FunctionID function) const { return functions[function.index()].isInQueueFromStart || leadsTo(referrer, function); }
    const LinkedCell& field(FieldReference field) const LIFETIME_BOUND { return fields[firstFieldOfLayout[field.layout.index()] + field.field]; }
    std::span<const PropertyKeyID> appendedKeysOf(LayoutID id) const LIFETIME_BOUND
    {
        size_t layout = id.index();
        if (layout + 1 >= firstAppendedFieldOfLayout.size())
            return { };
        return appendedKeys.span().subspan(firstAppendedFieldOfLayout[layout], firstAppendedFieldOfLayout[layout + 1] - firstAppendedFieldOfLayout[layout]);
    }
    std::span<const PropertyKeyID> keysAppendedByHeldAloneOf(LayoutID id) const LIFETIME_BOUND
    {
        size_t layout = id.index();
        if (layout + 1 >= firstKeyAppendedByHeldAloneOfLayout.size())
            return { };
        return keysAppendedByHeldAlone.span().subspan(firstKeyAppendedByHeldAloneOfLayout[layout], firstKeyAppendedByHeldAloneOfLayout[layout + 1] - firstKeyAppendedByHeldAloneOfLayout[layout]);
    }
    std::span<const LinkedNameBesideFields> namesStoredBesideFieldsOf(LayoutID id) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(namesStoredBesideFields, id, { }, &LinkedNameBesideFields::layout);
        return { found.begin(), found.end() };
    }
    uint32_t numberOfAppendedKeysOfPlan(LayoutID id) const { return static_cast<uint32_t>(appendedKeysOf(id).size() + keysAppendedByHeldAloneOf(id).size()); }
    Vector<PropertyKeyID, 16> appendedKeysOfPlan(LayoutID id) const
    {
        Vector<PropertyKeyID, 16> keys;
        keys.append(appendedKeysOf(id));
        keys.append(keysAppendedByHeldAloneOf(id));
        return keys;
    }
    ALWAYS_INLINE const LinkedCell* findCellOfField(FieldReference field) const LIFETIME_BOUND
    {
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size())
            return nullptr;
        uint32_t numberListed = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout];
        if (field.field < numberListed)
            return &fields[firstFieldOfLayout[layout] + field.field];
        uint32_t place = field.field - numberListed;
        return place < appendedKeysOf(field.layout).size() ? &appendedFields[firstAppendedFieldOfLayout[layout] + place] : nullptr;
    }
    TypeID provenOfField(FieldReference field) const
    {
        const LinkedCell* cell = findCellOfField(field);
        return cell && cell->everyWriterIsKnown ? cell->proven : TypeID();
    }
    std::optional<size_t> findPlaceOfAppendedField(FieldReference field) const
    {
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size())
            return std::nullopt;
        uint32_t numberListed = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout];
        if (field.field < numberListed || field.field - numberListed >= appendedKeysOf(field.layout).size())
            return std::nullopt;
        return firstAppendedFieldOfLayout[layout] + (field.field - numberListed);
    }
    bool isAppendedFieldRequiredBySomeWrittenType(FieldReference field) const
    {
        std::optional<size_t> place = findPlaceOfAppendedField(field);
        return place && appendedFieldsThatSomeWrittenTypeRequires.get(*place);
    }
    bool isStoredByFollowerOfMaker(FieldReference field) const
    {
        if (std::optional<size_t> place = findPlaceOfAppendedField(field))
            return appendedFieldsThatFollowersOfMakersStore.get(*place);
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size() || layout + 1 >= firstKeyAppendedByHeldAloneOfLayout.size())
            return false;
        size_t numberInFront = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout] + appendedKeysOf(field.layout).size();
        if (field.field < numberInFront || field.field - numberInFront >= keysAppendedByHeldAloneOf(field.layout).size())
            return false;
        return keysAppendedByHeldAloneThatFollowersOfMakersStore.get(firstKeyAppendedByHeldAloneOfLayout[layout] + (field.field - numberInFront));
    }
    bool takesWordLeftInCell(FieldReference field) const
    {
        std::optional<size_t> place = findPlaceOfAppendedField(field);
        return place && appendedFieldsThatTakeWordLeftInCell.get(*place);
    }
    bool isAppendedFieldLaidLate(FieldReference field) const
    {
        if (std::optional<size_t> place = findPlaceOfAppendedField(field))
            return appendedFieldsLaidLate.get(*place);
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size() || layout + 1 >= firstKeyAppendedByHeldAloneOfLayout.size())
            return false;
        size_t numberInFront = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout] + appendedKeysOf(field.layout).size();
        return field.field >= numberInFront && field.field - numberInFront < keysAppendedByHeldAloneOf(field.layout).size() && keysAppendedByHeldAloneLaidLate.get(firstKeyAppendedByHeldAloneOfLayout[layout] + (field.field - numberInFront));
    }
    bool isAppendedFieldRequiredByTypeOfThis(FieldReference field) const
    {
        std::optional<size_t> place = findPlaceOfAppendedField(field);
        if (place)
            return appendedFieldsThatTypeOfThisRequires.get(*place);
        size_t layout = field.layout.index();
        if (layout + 1 >= firstFieldOfLayout.size() || layout + 1 >= firstKeyAppendedByHeldAloneOfLayout.size())
            return false;
        size_t numberInFront = firstFieldOfLayout[layout + 1] - firstFieldOfLayout[layout] + appendedKeysOf(field.layout).size();
        return field.field >= numberInFront && field.field - numberInFront < keysAppendedByHeldAloneOf(field.layout).size() && keysAppendedByHeldAloneThatLieInCell.get(firstKeyAppendedByHeldAloneOfLayout[layout] + (field.field - numberInFront));
    }
    void forEachRunOfMakers(LayoutID id, NOESCAPE const Invocable<void(std::span<const PropertyKeyID>)> auto& functor) const
    {
        size_t layout = id.index();
        if (layout + 1 >= firstRunOfMakersOfLayout.size())
            return;
        for (uint32_t run = firstRunOfMakersOfLayout[layout]; run < firstRunOfMakersOfLayout[layout + 1]; ++run)
            functor(keysOfRunsOfMakers.span().subspan(firstKeyOfRunOfMakers[run], firstKeyOfRunOfMakers[run + 1] - firstKeyOfRunOfMakers[run]));
    }
    const LinkedCell& variable(VariableID id) const LIFETIME_BOUND { return variables[id.index()]; }
    const LinkedCell& capture(FunctionID id, uint32_t capture) const LIFETIME_BOUND { return captures[firstCaptureOfFunction[id.index()] + capture]; }
    const LinkedLayout& layout(LayoutID id) const LIFETIME_BOUND { return layouts[id.index()]; }
    template<typename Listed, typename Place>
    static ALWAYS_INLINE const Listed* findListedUnder(std::span<const Listed> ascending, Place place, const auto& placeOf)
    {
        size_t length = ascending.size();
        if (!length)
            return nullptr;
        auto found = ascending.begin();
        while (length > 1) {
            size_t half = length / 2;
            found += placeOf(found[half - 1]) < place ? half : 0;
            length -= half;
        }
        return placeOf(*found) == place ? &*found : nullptr;
    }
    static uint64_t placeOfSite(FunctionID function, ValueID site) { return static_cast<uint64_t>(function.index()) << 32 | site.index(); }
    ALWAYS_INLINE FunctionID calleeAt(FunctionID caller, ValueID site) const
    {
        RELEASE_ASSERT(firstCallOfCaller.last() == calls.size());
        if (caller.index() + 1 >= firstCallOfCaller.size())
            return { };
        uint32_t first = firstCallOfCaller[caller.index()];
        const LinkedCall* found = findListedUnder(calls.span().subspan(first, firstCallOfCaller[caller.index() + 1] - first), site, [](const LinkedCall& call) {
            return call.site;
        });
        return found ? found->callee : FunctionID();
    }
    ALWAYS_INLINE std::span<const LinkedCall> callsOf(FunctionID caller) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(firstCallOfCaller.last() == calls.size());
        if (caller.index() + 1 >= firstCallOfCaller.size())
            return { };
        uint32_t first = firstCallOfCaller[caller.index()];
        return calls.span().subspan(first, firstCallOfCaller[caller.index() + 1] - first);
    }
    bool onlyCallsWhatArrivesAt(FunctionID function, unsigned entryParameter) const
    {
        if (function.index() >= functions.size())
            return false;
        std::span<const LinkedParameter> arriving = parametersOf(function);
        return entryParameter < arriving.size() && arriving[entryParameter].isOnlyCalled;
    }
    bool onlyLooksAtWhatArrivesAt(FunctionID function, unsigned entryParameter) const
    {
        if (function.index() >= functions.size())
            return false;
        std::span<const LinkedParameter> arriving = parametersOf(function);
        return entryParameter < arriving.size() && arriving[entryParameter].isOnlyLookedAt;
    }
    bool capturesWhatItOnlyCalls(FunctionID function) const
    {
        return function.index() < functions.size() && std::ranges::any_of(parametersOf(function), &LinkedParameter::isCapturedWhereOnlyCalled);
    }
    const LinkedCallThatFindsBuiltin* findCallThatFindsBuiltin(FunctionID caller, ValueID site) const LIFETIME_BOUND
    {
        if (!callersThatFindBuiltins.get(caller.index()))
            return nullptr;
        return findListedUnder(callsThatFindBuiltins.span(), placeOfSite(caller, site), [](const LinkedCallThatFindsBuiltin& call) {
            return placeOfSite(call.caller, call.site);
        });
    }
    PropertyKeyID keyAt(FunctionID function, ValueID site) const
    {
        const LinkedAccess* found = findListedUnder(accessesUnderOneKey.span(), placeOfSite(function, site), [](const LinkedAccess& access) {
            return placeOfSite(access.function, access.site);
        });
        return found ? found->key : PropertyKeyID();
    }
    TypeID nativeCallResult(FunctionID function, ValueID site) const
    {
        if (!nativeFunctionCallers.get(function.index()))
            return { };
        const LinkedNativeCallResult* found = findListedUnder(nativeCallResults.span(), placeOfSite(function, site), [](const LinkedNativeCallResult& call) {
            return placeOfSite(call.function, call.site);
        });
        return found ? found->proven : TypeID();
    }
    TypeID resultOfReadByKey(FunctionID function, ValueID site) const
    {
        const LinkedResultOfRead* found = findListedUnder(resultsOfReadsByKey.span(), placeOfSite(function, site), [](const LinkedResultOfRead& read) {
            return placeOfSite(read.function, read.site);
        });
        return found ? found->proven : TypeID();
    }
    const LinkedContentsAtOperation* contentsAtOperation(FunctionID function, ValueID site) const
    {
        return findListedUnder(contentsAtOperations.span(), placeOfSite(function, site), [](const LinkedContentsAtOperation& contents) {
            return placeOfSite(contents.function, contents.site);
        });
    }
    bool receiverStaysAsMadeAt(FunctionID function, ValueID site) const
    {
        auto placeOf = [](const LinkedReceiverAsMade& listed) {
            return static_cast<uint64_t>(listed.function.index()) << 32 | listed.site.index();
        };
        return std::ranges::binary_search(receiversThatStayAsMade, placeOf({ function, site }), { }, placeOf);
    }
    bool nobodyChangesReceiverAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(receiversThatNobodyChanges, LinkedReceiverAsMade { function, site }); }
    uint32_t leastLengthOfViewsReadAt(FunctionID function, ValueID site) const
    {
        auto found = std::ranges::lower_bound(readsOffViewsOfWrittenLengths, LinkedReadOffViews { function, site, 0 });
        return found != readsOffViewsOfWrittenLengths.end() && found->function == function && found->site == site ? found->leastLength : 0;
    }
    bool readsPlainListAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(plainListsRead, LinkedReceiverAsMade { function, site }); }
    bool listHasNoHolesAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(listsWithoutHoles, LinkedReceiverAsMade { function, site }); }
    bool iterableHasNoHolesAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(iterablesWithoutHoles, LinkedReceiverAsMade { function, site }); }
    bool iterableStaysAsMadeAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(iterablesThatStayAsMade, LinkedReceiverAsMade { function, site }); }
    const LinkedAccessorsOfField* findAccessorsExpectedIn(FieldReference) const LIFETIME_BOUND;
    TypeID caughtAt(FunctionID, BlockID handler) const;
    enum class AskedByKey : bool { ByLongRowsAlone, ByWalkToo };
    void indexMembersByKey(const Program&, AskedByKey = AskedByKey::ByLongRowsAlone);
    std::span<const uint32_t> membersUnder(LayoutID classLayout, ClassSide, PropertyKeyID) const LIFETIME_BOUND;
    FunctionID methodReplacedBy(FunctionID replacement, PropertyKeyID) const;
    bool replacesMethod(FunctionID replacement, FunctionID written) const;
    MemberFound findMember(const Program&, LayoutID, ClassSide, PropertyKeyID) const;
    MemberFound findMethodStoredFirst(const Program&, LayoutID, PropertyKeyID) const;
    bool findFunctionsLaidAbove(const Program&, LayoutID, PropertyKeyID, Vector<FunctionID, 4>&) const;
    bool findPrimitivesStoredInSuperclasses(const Program&, LayoutID, PropertyKeyID, Vector<TypeID, 4>&, bool* mayStopStores = nullptr) const;
    bool mayBeLaidOnPrototypeUnderUnknownKey(const Program&, LayoutID constructorFunction, PropertyKeyID) const;
    bool holderIsChangedInSightAlone(const Program&, LayoutID classLayout, ClassSide) const;
    bool memberHoldsOneFunction(const Program&, LayoutID classLayout, ClassSide, PropertyKeyID, FunctionID written) const;
    std::optional<FindsNothing> findsNothing(const Program&, LayoutID, ClassSide, PropertyKeyID, Of) const;
    std::span<const uint32_t> membersUnderKeyAlongRows(ClassSide, PropertyKeyID) const;
    struct StretchOfRows {
        uint32_t first;
        uint32_t end;
        LayoutID layoutAtFirst;
    };
    bool someClassAtOrAboveDeclares(const Program&, std::span<const StretchOfRows>, ClassSide, PropertyKeyID, uint32_t& classesAsked) const;
    Vector<LinkedNameOnFunction> namesOnFunctions;
    const LinkedNameOnFunction* findNameOnFunction(FunctionID holder, PropertyKeyID) const LIFETIME_BOUND;
    bool closuresLackName(FunctionID, PropertyKeyID) const;
};

bool isNameInTextOf(const Program&, PropertyKeyID symbol, FunctionID);
PropertyKeyID keyOfWellKnownSymbol(const Program&, BuiltinID constant);
PropertyKeyID provenSymbolAt(const Program&, const ThinSummaries&, FunctionID, const BodyIndex&, ValueID);
bool isSymbolOfLibrary(const Program&, PropertyKeyID);

LinkTables conservativeLinkTables(const Program&);
LinkTables conservativeLinkTables(const Program&, const ParallelLoops&);
void findParametersThatAreOnlyCalled(const Program&, const BodyIndexTable&, const ThinSummaries&, const ParallelLoops&, Mechanisms, bool shouldAssumeCalleesRetainNothing, LinkTables&);
void findParametersThatAreOnlyLookedAt(const Program&, const BodyIndexTable&, const ThinSummaries&, Mechanisms, bool shouldAssumeCalleesRetainNothing, LinkTables&);
void carryExpectationOfThisIntoCaptures(const Program&, const BodyIndexTable&, const ThinSummaries&, LinkTables&);

BitVector bodiesThatLoaderEntersAtMostOnce(const Program&);

bool mayReturnInFormOfItsOwn(const Function&);
bool callsOfBuiltinComeBack(const Program&, BuiltinID);

bool classHolderIsMadeWithName(const Program&, ClassSide, PropertyKeyID);

bool isGlobalObject(const Program&, const LinkTables&, const ThinSummary&, uint32_t node, uint32_t globalObject);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
