/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCodeRole.h"
#include "AOTEffects.h"
#include "AOTFamilies.h"
#include "AOTLinkCoroutineTables.h"
#include "AOTLinkKnownEffects.h"
#include "AOTMechanisms.h"
#include "AOTTestRuns.h"
#include "AOTThinSummary.h"
#include "AOTUnionTaggedMakers.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

struct LinkedParameter {
    TypeID expected;
    bool expectedIsDeclared { false };
    TypeID proven;
    bool staysAsItsClassMakesIt { false };
    bool isOnlyCalled { false };
};

struct LinkedFunction {
    bool isReached { true };
    bool everyCallerIsKnown { false };
    bool everyCallHasOneCallee { false };
    bool ringsOfTailCallsAreListed { false };
    bool runsAtMostOnce { false };
    bool isNamedInItsCycle { true };
    bool isCalledInLoop { false };
    bool liesUnderNameOrIsLost { false };
    bool isInQueueFromStart { false };
    bool namesOnlyWhatEveryFunctionLeadsTo { false };
    bool closuresAreChanged { false };
    bool closuresStayAsMade { false };
    bool closuresGetNoNameButPrototype { false };
    bool closuresGetOnlyNamesThatAreListed { false };
    bool prototypePropertyStaysAsMade { false };
    uint32_t cycle { 0 };
    uint32_t rank { 0 };
    TypeID result;
    Effects effectsOnReturn;
    Effects effectsOnThrow;
    uint32_t firstParameter { 0 };
    uint32_t numberOfParameters { 0 };
    static constexpr uint32_t notLeaf = UINT32_MAX;
    uint32_t weightAsLeaf { notLeaf };
};

struct LinkedCell {
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
    bool isOnlyGivenUndefinedByMakers { false };
    bool isLeftUndefinedByMakers { false };
    bool isGivenByNoMaker { false };
    OptionSet<FieldForm> formThatDefinitionsCreate;
};

enum class LayoutChange : uint8_t {
    IsFrozen = 1 << 0,
    GainsProperty = 1 << 1,
    LosesProperty = 1 << 2,
    ChangesPrototype = 1 << 3,
    GetsAccessor = 1 << 4,
};

inline constexpr uint32_t maxAppendedFieldsOutOfLine = maxExtensionGranules;

struct LinkedLayout {
    FamilyID family;
    bool isAllocated { true };
    bool reachesUnseenCode { true };
    OptionSet<LayoutChange> possibleChanges { LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor };
    bool isRedefinedByLiteralsAlone { false };
    bool listsEveryMemberAbove { false };
    bool listsEveryStaticMember { false };
    bool hasConstructorFunctionInRow { true };
    bool isFilledUnderComputedKeys { false };
    bool keysMayBeListed { true };
    bool onlyOwnObjectsStandBelowPrototype { false };
    bool prototypeObjectIsSettledAheadOfEveryObject { false };
    LayoutID heritage;
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
};

struct LinkedNameOnPrototype {
    LayoutID layout;
    PropertyKeyID key;
    FunctionID function;
    FunctionID expected;
    FunctionID expectedGetter;
    FunctionID expectedSetter;
};

struct LinkedNameOnFunction {
    FunctionID holder;
    PropertyKeyID key;
    FunctionID function;
};

struct LinkTables;
void appendMembersAssignedToPrototype(const Program&, const LinkTables&, LayoutID, Vector<ClassMember>&);

struct MemberFound {
    enum class Kind : uint8_t {
        Unknown,
        Nothing,
        Method,
        Accessor,
    };

    Kind kind { Kind::Unknown };
    FunctionID method;
    FunctionID getter;
    FunctionID setter;
};

struct LinkedGroup {
    enum class Kind : uint8_t {
        Anybody,
        Constructors,
        SharedCall,
    };

    Kind kind { Kind::Anybody };
    uint32_t subject { 0 };
    uint32_t cycle { 0 };
    uint32_t rank { 0 };
};

struct LinkedCall {
    FunctionID caller;
    ValueID site;
    FunctionID callee;
    bool isInTailPosition { false };
    bool mayComeAgain { true };

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

    friend constexpr auto operator<=>(const LinkedGetterAtRead&, const LinkedGetterAtRead&) = default;
};

struct LinkedResultOfRead {
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
    friend constexpr auto operator<=>(const LinkedAccessorsOfField&, const LinkedAccessorsOfField&) = default;
};

struct SizeOfLinkFlow {
    uint64_t nodes { 0 };
    uint64_t edges { 0 };
    uint64_t observers { 0 };
    uint64_t nodesTakenOffWorklist { 0 };
    uint64_t observersTold { 0 };
    uint64_t rounds { 0 };
};

struct WriteFollowedToBuiltin {
    FunctionID function;
    ValueID site;
    BuiltinID holder;
    Vector<PropertyKeyID> namesBrought { };
    bool knowsNamesBrought { false };
};

struct LookupsPastProgramObjects {
    BitVector namesRead;
    BitVector namesCalled;
    BitVector namesSpelledByProgram;
    BitVector builtinsRead;
    BitVector builtinsCalled;
    BitVector builtinsLetGo;
    BitVector builtinsCalledOutOfSight;
    // MUST BE TRUE OF THE PROGRAM THAT RUNS: each is locked at the start (Facts::builtinMembersLockedAtStartUp()), with every mechanism withheld too: what is reached hangs on it.
    BitVector membersTakenForConstants;
    BitVector membersWritten;
    Vector<WriteFollowedToBuiltin> writesFollowedToBuiltins;
    BitVector holdersWrittenUnderAnyName;
    BitVector holdersHandedWhatIsNoTextNumberOrBoolean;
    bool objectsNobodyKnowsAreHandedWhatIsNoTextNumberOrBoolean { false };
    BitVector lostBuiltins;
    BitVector builtinsAtUnseen;
    BitVector builtinsPassedOptionThatRunsCode;
    BitVector classesWithInstancesAtUnseen;
    BitVector namesReadThroughLostBuiltins;
    BitVector namesWrittenThroughLostBuiltins;
    BitVector namesWrittenThroughUnseenByProgram;
    BitVector namesWrittenThroughUnseenByLibrary;
    bool writesUnderAnyNameThroughUnseenByProgram { false };
    bool writesUnderAnyNameThroughLostBuiltins { false };
    bool climbsFromLostBuiltins { false };
    bool climbsFromUnseenObjects { false };
    bool lostBuiltinsReachUnseen { false };
    bool readsUnderKeysNobodyKnows { false };
    bool nativeCodeMayHoldObjects { false };
};

class FieldsOfNumbers {
public:
    void add(LayoutID declarer, PropertyKeyID key) { m_fields.add(wordOf(declarer, key)); }
    bool contains(LayoutID declarer, PropertyKeyID key) const { return m_fields.contains(wordOf(declarer, key)); }
    bool names(const Program&, LayoutID, PropertyKeyID) const;

private:
    static uint64_t wordOf(LayoutID declarer, PropertyKeyID key) { return (static_cast<uint64_t>(declarer.index()) + 1) << 32 | key.index(); }

    UncheckedKeyHashSet<uint64_t> m_fields;
};

struct LinkTables {
    BitVector exportsThatStayIn;
    Vector<LinkedFunction> functions;
    Vector<LinkedParameter> parameters;
    Vector<LinkedCall> calls;
    Vector<LinkedCallThatFindsBuiltin> callsThatFindBuiltins;
    BitVector callersThatFindBuiltins;
    Vector<uint32_t> firstCallOfCaller;
    Vector<LinkedAccess> accessesUnderOneKey;
    Vector<LinkedGetterAtRead> gettersExpectedAtReads;
    Vector<LinkedCoroutine> coroutines;
    const LinkedCoroutine* findCoroutine(FunctionID) const;
    Vector<LinkedResultOfRead> resultsOfReadsByKey;
    Vector<LinkedContentsAtOperation> contentsAtOperations;
    Vector<LinkedReceiverAsMade> receiversThatStayAsMade;
    Vector<LinkedReceiverAsMade> iterablesThatStayAsMade;
    FieldsOfNumbers fieldsReadAsIntegers;
    FieldsOfNumbers fieldsReadForArithmetic;
    FieldsOfNumbers fieldsStoredWhatIsNoInt32;
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
    Vector<LinkedCell> fields;
    Vector<uint32_t> firstAppendedFieldOfLayout;
    Vector<PropertyKeyID> appendedKeys;
    Vector<uint32_t> firstKeyAppendedByHeldAloneOfLayout;
    Vector<PropertyKeyID> keysAppendedByHeldAlone;
    BitVector keysAppendedByHeldAloneThatLieInCell;
    Vector<LinkedCell> appendedFields;
    BitVector appendedFieldsThatSomeWrittenTypeRequires;
    BitVector appendedFieldsThatTypeOfThisRequires;
    BitVector appendedFieldsNamedMostOften;
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
    Vector<LinkedCell> variables;
    Vector<uint32_t> firstCaptureOfFunction;
    Vector<LinkedCell> captures;
    Vector<LinkedLayout> layouts;
    Vector<LinkedMember> members;
    Vector<uint32_t> membersByKey;
    std::array<BitVector, 2> keysOfMembers;
    Vector<LinkedNameOnPrototype> namesOnPrototypes;
    Vector<TaggedMaker> taggedMakers;
    SizeOfLinkFlow sizeOfFlow;
    LookupsPastProgramObjects lookups;
    bool unseenCodeCanRun { true };

    const LinkedFunction& function(FunctionID id) const LIFETIME_BOUND { return functions[id.index()]; }
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
    bool standsOutsideCycles(CodeUnit unit) const { return namesOnlyUnitsOfItsFunction(unit.role) && functions[unit.function.index()].isInQueueFromStart; }
    bool leadsTo(CodeUnit referrer, FunctionID function) const { return leadsTo(referrer.function, function) && (!standsOutsideCycles(referrer) || functions[function.index()].isInQueueFromStart); }
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
    bool isAppendedFieldNamedMostOften(FieldReference field) const
    {
        std::optional<size_t> place = findPlaceOfAppendedField(field);
        return place && appendedFieldsNamedMostOften.get(*place);
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
    FunctionID calleeAt(FunctionID caller, ValueID site) const;
    bool onlyCallsWhatArrivesAt(FunctionID function, unsigned entryParameter) const
    {
        if (function.index() >= functions.size())
            return false;
        std::span<const LinkedParameter> arriving = parametersOf(function);
        return entryParameter < arriving.size() && arriving[entryParameter].isOnlyCalled;
    }
    const LinkedCallThatFindsBuiltin* findCallThatFindsBuiltin(FunctionID caller, ValueID site) const LIFETIME_BOUND;
    PropertyKeyID keyAt(FunctionID, ValueID site) const;
    TypeID resultOfReadByKey(FunctionID, ValueID site) const;
    const LinkedContentsAtOperation* contentsAtOperation(FunctionID, ValueID site) const;
    bool receiverStaysAsMadeAt(FunctionID function, ValueID site) const
    {
        auto placeOf = [](const LinkedReceiverAsMade& listed) {
            return static_cast<uint64_t>(listed.function.index()) << 32 | listed.site.index();
        };
        return std::ranges::binary_search(receiversThatStayAsMade, placeOf({ function, site }), { }, placeOf);
    }
    bool iterableStaysAsMadeAt(FunctionID function, ValueID site) const { return std::ranges::binary_search(iterablesThatStayAsMade, LinkedReceiverAsMade { function, site }); }
    const LinkedAccessorsOfField* findAccessorsExpectedIn(FieldReference) const LIFETIME_BOUND;
    TypeID caughtAt(FunctionID, BlockID handler) const;
    void indexMembersByKey();
    std::span<const uint32_t> membersUnder(LayoutID classLayout, ClassSide, PropertyKeyID) const LIFETIME_BOUND;
    MemberFound findMember(const Program&, LayoutID, ClassSide, PropertyKeyID) const;
    bool findFunctionsLaidAbove(const Program&, LayoutID, PropertyKeyID, Vector<FunctionID, 4>&) const;
    Vector<LinkedNameOnFunction> namesOnFunctions;
    const LinkedNameOnFunction* findNameOnFunction(FunctionID holder, PropertyKeyID) const LIFETIME_BOUND;
    bool closuresLackName(FunctionID, PropertyKeyID) const;
};

bool isNameInTextOf(const Program&, PropertyKeyID symbol, FunctionID);
bool isSymbolOfLibrary(const Program&, PropertyKeyID);

LinkTables conservativeLinkTables(const Program&);
LinkTables conservativeLinkTables(const Program&, const ParallelLoops&);
void findParametersThatAreOnlyCalled(const Program&, const BodyIndexTable&, const ThinSummaries&, const ParallelLoops&, Mechanisms, LinkTables&);

BitVector bodiesThatLoaderEntersAtMostOnce(const Program&);

bool mayReturnInFormOfItsOwn(const Function&);
bool callsOfBuiltinComeBack(const Program&, BuiltinID);

bool classHolderIsMadeWithName(const Program&, ClassSide, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
