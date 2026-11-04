/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkUnions.h"
#include "AOTOpcodeSets.h"
#include "AOTRowsOfLayouts.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

// Numbered in the order in which they were closed: whatever a component leads to has a lower number.
struct ComponentsOfLinkEffects {
    uint32_t size() const { return firstMember.size() - 1; }
    std::span<const uint32_t> membersOf(uint32_t component) const LIFETIME_BOUND { return members.span().subspan(firstMember[component], firstMember[component + 1] - firstMember[component]); }

    Vector<uint32_t> componentOfFunction;
    Vector<uint32_t> firstMember;
    Vector<uint32_t> members;
};

// Tarjan. Roots and successors are taken in ascending order, so the numbers follow from the graph alone.
template<typename SuccessorsOf>
static ComponentsOfLinkEffects findComponentsForLinkEffects(uint32_t numberOfFunctions, const SuccessorsOf& successorsOf)
{
    constexpr uint32_t none = UINT32_MAX;
    struct Visit {
        uint32_t function;
        uint32_t nextSuccessor;
    };

    ComponentsOfLinkEffects components;
    components.componentOfFunction.fill(none, numberOfFunctions);
    components.members.reserveInitialCapacity(numberOfFunctions);
    components.firstMember.append(0);

    Vector<uint32_t> discovery;
    discovery.fill(none, numberOfFunctions);
    Vector<uint32_t> lowest;
    lowest.fill(0, numberOfFunctions);
    Vector<uint32_t> open;
    Vector<Visit> path;
    uint32_t numberOfDiscoveries = 0;

    auto enter = [&](uint32_t function) {
        discovery[function] = numberOfDiscoveries;
        lowest[function] = numberOfDiscoveries++;
        open.append(function);
        path.append(Visit { function, 0 });
    };

    for (uint32_t root = 0; root < numberOfFunctions; ++root) {
        if (discovery[root] != none)
            continue;
        enter(root);
        while (!path.isEmpty()) {
            uint32_t function = path.last().function;
            std::span<const FunctionID> successors = successorsOf(function);
            if (path.last().nextSuccessor < successors.size()) {
                uint32_t successor = successors[path.last().nextSuccessor++].index();
                RELEASE_ASSERT(successor < numberOfFunctions);
                if (discovery[successor] == none)
                    enter(successor);
                else if (components.componentOfFunction[successor] == none)
                    lowest[function] = std::min(lowest[function], discovery[successor]);
                continue;
            }

            path.removeLast();
            if (!path.isEmpty())
                lowest[path.last().function] = std::min(lowest[path.last().function], lowest[function]);
            if (lowest[function] != discovery[function])
                continue;

            uint32_t component = components.size();
            uint32_t member;
            do {
                member = open.takeLast();
                components.componentOfFunction[member] = component;
                components.members.append(member);
            } while (member != function);
            components.firstMember.append(components.members.size());
        }
    }
    return components;
}

// As walk/AOTWalkBuiltins.cpp reads the rows, for operands of which only a LinkType is known.
static constexpr OperandUses usesThatCallOperandInLinkEffects { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly };
static constexpr OperandUses usesThatRunCodeNobodyNamesInLinkEffects {
    OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::TakesDescriptor,
    OperandUse::PassesToUserCode, OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
};
static constexpr OperandUses usesThatChangeShapeInLinkEffects { OperandUse::DefinesName, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype };
static constexpr OperandUses usesThatChangeOperandInLinkEffects {
    OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom,
    OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype,
};
static constexpr OperandUses usesThatHandOperandToNobodyInLinkEffects {
    OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::RetainsContents, OperandUse::WritesContents, OperandUse::MovesElements,
};

static bool mayBeObjectForLinkEffects(const LinkType& type)
{
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.holdsUnseen;
}

// Something that can be called and is no closure of the program: a built-in, a bound function, a function of unseen code.
static bool mayBeOtherCallableForLinkEffects(const LinkType& type)
{
    return type.classes != LinkType::Classes::None || type.builtin || type.holdsUnseen;
}

// It is converted, settles a promise or is asked for a member: each may run code that an object among the answers names.
static bool rowLooksIntoAnswerInLinkEffects(BuiltinCallbackResultUse use)
{
    switch (use) {
    case BuiltinCallbackResultUse::Ignored:
    case BuiltinCallbackResultUse::Tested:
    case BuiltinCallbackResultUse::Returned:
        return false;
    case BuiltinCallbackResultUse::Escapes:
    case BuiltinCallbackResultUse::Stored:
    case BuiltinCallbackResultUse::Flattened:
    case BuiltinCallbackResultUse::AskedWhetherIterable:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isFunctionForLinkEffects(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return true;
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class CalleeSummarizerForLinkEffects {
    WTF_MAKE_NONCOPYABLE(CalleeSummarizerForLinkEffects);
public:
    CalleeSummarizerForLinkEffects(const LinkFlow&, LinkTables&);

    void findComponentsOfCallees();
    // Both go by findComponentsOfCallees(), callees first, and neither reads what the other writes.
    void summarizeEffects();
    void summarizeValues();
    // Behind summarizeEffects().
    void findVariablesInitializedAtEveryRead();
    TypesFoundOfCallees takeTypesFound() { return WTF::move(m_typesFound); }

private:
    enum class Fate : uint8_t {
        IsReturned = 1 << 0,
        // It is stored, thrown or captured, or handed to code that may do any of that.
        Leaves = 1 << 1,
    };

    enum class Answer : uint8_t { NotAsked, No, Yes };

    struct EntryPositions {
        std::optional<unsigned> thisValue;
        std::optional<unsigned> callee;
        std::optional<unsigned> newTarget;
        unsigned firstPositional { 0 };
        unsigned numberOfPositional { 0 };
        unsigned numberOfEntryParameters { 0 };
    };

    // What the functions that a shared call lists do with what they are handed, taken together. The default: nothing is known of them.
    struct OperandsOfSharedCall {
        bool everyCalleeIsSummarized { false };
        bool thisValueEscapes { true };
        bool calleeEscapes { true };
        bool newTargetEscapes { true };
        bool everyResultIsFresh { false };
        bool someCalleeTakesThisFromConstruction { true };
        // By argument. One behind the last escapes.
        Vector<bool, 4> argumentEscapes;
    };

    // What a call or an access may enter, beside what the operation does itself.
    struct CodeFound {
        void add(FunctionID function) { functions.append(function); }

        // keep() drops the repeats.
        Vector<FunctionID, 4> functions;
        bool mayBeLost { false };
        // Then nobody says what runs.
        bool mayBeUnseen { false };
    };

    struct CodeEntered {
        // Into m_functionsEntered.
        uint32_t firstFunction { 0 };
        uint32_t numberOfFunctions { 0 };
        bool mayBeLost { false };
        bool mayBeUnseen { false };

        bool isNone() const { return !numberOfFunctions && !mayBeLost && !mayBeUnseen; }
    };

    struct AccessorUnderKey {
        PropertyKeyID key;
        FlowNodeID holder;
        bool runsAtReads { false };
        bool runsAtStores { false };
    };

    // Of any other function every entry stays at its default.
    bool isSummarized(uint32_t function) const { return m_isSummarized.get(function); }
    bool keepsDefaults(FunctionID, const ThinSummary&, OptionSet<SummaryTrait>) const;
    uint32_t nodeOfLostFunctions() const { return m_numberOfFunctions; }
    // A SHARED CALL OF THE FLOW IS A NODE HERE TOO, between the calls that go by it and what it lists. It stands among the functions that such a call enters, and what all that it lists do together is summarized once.
    uint32_t nodeOfSharedCall(uint32_t shared) const { return m_numberOfFunctions + 1 + shared; }
    bool isFunction(uint32_t node) const { return node < m_numberOfFunctions; }
    bool isSharedCall(uint32_t node) const { return node > m_numberOfFunctions; }
    uint32_t numberOfNodesOfCallees() const { return m_numberOfFunctions + 1 + static_cast<uint32_t>(m_flow.calleesOfSharedCalls.size()); }

    LinkType typeAt(FunctionID, uint32_t nodeInSummary) const;
    static std::span<const uint32_t> argumentsOf(const ThinSummary& summary, const SummaryCall& call) { return summary.arguments.span().subspan(call.firstArgument, call.numberOfArguments); }
    std::span<const FunctionID> functionsOf(const CodeEntered& code) const LIFETIME_BOUND { return m_functionsEntered.span().subspan(code.firstFunction, code.numberOfFunctions); }
    std::span<const FunctionID> calleesOf(uint32_t function) const LIFETIME_BOUND { return m_callees.span().subspan(m_firstCallee[function], m_firstCallee[function + 1] - m_firstCallee[function]); }
    void prefetchSummaryOfMemberBehind(size_t place) const;
    const CodeEntered& codeOfCall(FunctionID function, uint32_t call) const LIFETIME_BOUND { return m_codeOfCalls[m_flow.firstCallOfFunction[function.index()] + call]; }
    const CodeEntered& codeOfAccess(FunctionID function, uint32_t access) const LIFETIME_BOUND { return m_codeOfAccesses[m_firstAccessOfFunction[function.index()] + access]; }

    // ---- What a lookup finds
    void indexHolders();
    void findLayoutsThatMayFindAnything();
    bool mayFindAnything(const LinkType& subject);
    bool readsFixedPartOfClass(const LinkType& subject, PropertyKeyID, bool hasOtherReceiver);
    bool objectPrototypeNeverAnswers(PropertyKeyID);
    void addFunctionsAt(const LinkType& held, CodeFound&) const;
    void lookInLayout(LayoutID, bool hasOwnFields, PropertyKeyID, MethodKind wanted, bool callsWhatIsFound, CodeFound&) const;
    void findCodeBehindLookup(const LinkType& subject, PropertyKeyID, SummaryAccess::Kind, bool hasOtherReceiver, bool callsWhatIsFound, CodeFound&);
    Effects effectsOfLookupItself(SummaryAccess::Kind, PropertyKeyID) const;

    // ---- What the rows of a built-in say
    std::span<const BuiltinID> builtinsEnteredBy(FunctionID, const ThinSummary& summary LIFETIME_BOUND, uint32_t call) const LIFETIME_BOUND;
    template<typename Functor> bool forEachRowOf(BuiltinID, const SummaryCall&, const Functor&) const;
    bool rowSaysAllOfCallsOf(const BuiltinOverload&, BuiltinOperand callee) const;
    bool addEffectsOnOperand(const BuiltinOverload&, BuiltinOperand, const LinkType&, CodeFound&, Effects&) const;
    bool addEffectsOfRow(const BuiltinOverload&, FunctionID, const ThinSummary&, const SummaryCall&, CodeFound&, Effects&) const;
    bool passesNoIterable(FunctionID, const ThinSummary&, const SummaryCall&) const;
    bool addEffectsOfRows(BuiltinID, FunctionID, const ThinSummary&, const SummaryCall&, CodeFound&, Effects&) const;
    bool rowsHandOperandToNobody(BuiltinID, const SummaryCall&, BuiltinOperand) const;
    bool rowsGiveFreshResult(BuiltinID, const SummaryCall&) const;

    // ---- Effects
    // `firstFunction` counts from the start of the list that is handed in.
    static CodeEntered keep(CodeFound&, Vector<FunctionID>& functionsEntered);
    void findCodeEnteredByCall(FunctionID, const ThinSummary&, uint32_t call, CodeFound&);
    void findCodeEntered();
    Effects effectsOfCallItself(FunctionID, const ThinSummary&, uint32_t call) const;
    bool handsObjectToCodeNobodyNames(FunctionID, const ThinSummary&) const;
    bool changeMayRunCode(FunctionID, const SummaryChange&) const;
    void mergeEffectsOfCodeOutside(const CodeEntered&, uint32_t component, const ComponentsOfLinkEffects&, Effects&) const;
    Effects effectsOfBodyAndOfCodeOutside(uint32_t function, const ComponentsOfLinkEffects&) const;

    // ---- The order in which a program starts
    struct StartOfProgram {
        enum class StateOfModule : uint8_t { NotStarted, Followed, NotFollowed };
        static constexpr uint32_t never = UINT32_MAX;

        // One start after the other is walked on these tables. A walk lists what it writes, and costs what it has been to.
        ModuleID entry;
        Vector<StateOfModule> modules;
        Vector<uint32_t> modulesStarted;
        // By function, with the node of the lost functions behind them: the earliest tick at which anything may enter it.
        Vector<uint32_t> firstEntry;
        Vector<uint32_t> nodesEntered;
        BitVector bodiesFollowed;
        // Somebody who has run names it, so code that nobody knows may have got hold of it.
        BitVector isAvailable;
        Vector<uint32_t> functionsMadeAvailable;
        Vector<uint32_t> availableToUnseenCode;
        // By variable: the tick of the store that ends its dead zone.
        Vector<uint32_t> endOfDeadZone;
        Vector<uint32_t> variablesInitialized;
        Vector<uint32_t> worklist;
        uint32_t clock { 1 };
        uint32_t lastTickOfUnseenCode { 0 };
        // From this tick on the other entries may have been loaded: whatever they lead to, and this start has not been to, counts as evaluated or entered then.
        uint32_t tickOfOtherEntries { never };
        // The first walk of a program with several entries: they count as leading to everything, and nobody has to find what they lead to.
        bool otherEntriesLeadToEverything { false };
        // Something that the program made has been handed to it. Until then it has nothing of the program to enter.
        bool unseenCodeHoldsObjects { false };

        // Of all the starts that were walked on these tables.
        BitVector isInitializedInSomeStart;
        BitVector isReadTooEarly;
        // Where the other entries lead to everything: its dead zone ends behind their tick, so whether it is read too early hangs on what they lead to.
        BitVector isLeftOpen;
        bool someModuleIsLeftOpen { false };
        // By module: in how many of the starts in which another entry leads to it the start has been there itself.
        Vector<uint32_t> startsThatCameFirst;
        uint32_t numberOfStartsWithOtherEntries { 0 };
    };
    // What evaluating an entry at a time that nobody knows leads to, with unseen code at every step: the same in every start.
    struct LedTo {
        static constexpr uint32_t byNothing = UINT32_MAX;
        static constexpr uint32_t bySeveral = UINT32_MAX - 1;

        bool isEmpty() const { return entryOfModule.isEmpty(); }

        // The module of the one entry that leads there.
        Vector<uint32_t> entryOfModule;
        Vector<uint32_t> entryOfNode;
        // The one module whose body leads to the node, by calls and names alone.
        Vector<uint32_t> moduleOfNode;
        // By the module of an entry: the functions which that entry alone leads to and which may call unknown code. Then of all entries.
        Vector<uint32_t> callersOfUnknownCodeOfEntry;
        uint32_t callersOfUnknownCode { 0 };
        // By variable, one more than there are variables: the functions that read it, without the body of its own module. Into readers.
        Vector<uint32_t> firstReader;
        Vector<uint32_t> readers;
    };
    struct RecordAtSite {
        enum class Kind : uint8_t { Call, Access, Change, UnknownCode };
        uint32_t site { 0 };
        Kind kind { Kind::Call };
        uint32_t index { 0 };
    };
    bool isFollowedAtStart(ModuleID) const;
    // `F.prototype.m = function () { }`: A NAME THAT THE PROGRAM LAYS ON THE PROTOTYPE OBJECT OF A CONSTRUCTOR FUNCTION IS ONE MORE VARIABLE, of the module of the function, numbered behind the variables by its place in
    // LinkTables::namesOnPrototypes. The store ends its dead zone, and whoever enters the function reads it: no object of the layout is made without that.
    // So are, of each constructor function, behind the names: THE STORE OF AN OBJECT MADE TO BE ITS PROTOTYPE OBJECT, which whoever reads `prototype` off the function reads too, and THE LINK of its prototype object below that of its base.
    // Whoever enters a function reads what is said of it and of every function above it in the row.
    // And, last, A NAME THAT THE PROGRAM LAYS ON A FUNCTION ITSELF (LinkTables::namesOnFunctions): whoever reads or calls it off the function reads it.
    size_t firstNameOnFunction() const { return m_program.variables.size() + m_tables.namesOnPrototypes.size() + 2 * m_layoutsOfConstructorFunctions.size(); }
    size_t numberOfVariablesAndNames() const { return firstNameOnFunction() + m_tables.namesOnFunctions.size(); }
    std::optional<uint32_t> numberOfNameOnFunction(const LinkType& subject, PropertyKeyID) const;
    void lookAtNameOnFunctionAtStart(StartOfProgram&, FunctionID body, const LinkType& subject, PropertyKeyID, bool isStore, bool endsDeadZones) const;
    void forEachNameOnFunctionReadBy(FunctionID reader, const Invocable<void(uint32_t)> auto&) const;
    // The keys of the rows under which one function is proven: no other row is asked about.
    BitVector m_keysOfNamesOnFunctions;
    enum class SaidOfPrototypeObject : uint8_t { IsStored, IsLinked };
    std::optional<uint32_t> numberOf(SaidOfPrototypeObject, LayoutID) const;
    const LinkedFunction& constructorFunctionOf(LayoutID layout) const { return m_tables.functions[m_program.at(layout).constructor.index()]; }
    bool prototypeObjectIsSettledAtStart(const StartOfProgram&, LayoutID) const;
    LinkType typeOfPrototypeObjectOf(LayoutID) const;
    void lookAtPrototypePropertyAtStart(StartOfProgram&, FunctionID body, const SummaryAccess&, bool endsDeadZones) const;
    void endDeadZoneOfLinkLaidBy(StartOfProgram&, FunctionID body, const SummaryChange&, bool makesObjectHere) const;
    void forEachFunctionWhosePrototypeIsReadBy(FunctionID reader, const Invocable<void(uint32_t)> auto&) const;
    Vector<LayoutID> m_layoutsOfConstructorFunctions;
    bool m_someFunctionIsGivenItsPrototypeObject { false };
    ModuleID moduleOfVariableOrName(size_t) const;
    void forEachNameReadByEntering(uint32_t function, const Invocable<void(uint32_t)> auto&) const;
    void endDeadZoneOfNameLaidBy(StartOfProgram&, FunctionID body, const SummaryAccess&) const;
    void forEachPlaceLedToBy(uint32_t node, const Invocable<void(uint32_t)> auto& toModule, const Invocable<void(uint32_t)> auto& toNode) const;
    void findWhatEntriesLeadTo();
    bool mayCallUnknownCodeAtStart(uint32_t function) const;
    bool otherEntriesLeadTo(const StartOfProgram& start, uint32_t ledToBy) const { return ledToBy != LedTo::byNothing && ledToBy != start.entry.index(); }
    StartOfProgram::StateOfModule stateAtStart(const StartOfProgram&, ModuleID) const;
    bool isEnteredByOtherEntries(const StartOfProgram&, uint32_t node) const;
    void findModulesThatMayWait();
    void walkStart(StartOfProgram&, const BodyIndexTable&, ModuleID entry, std::span<const uint32_t> firstVariableOfModule, std::span<const uint32_t> variablesOfModules) const;
    void makeAvailableAtStart(StartOfProgram&, FunctionID namer) const;
    void makeFunctionsBornBehindNamespaceAvailableAtStart(StartOfProgram&, ModuleID) const;
    void enterAtStart(StartOfProgram&, uint32_t function) const;
    void enterAtStart(StartOfProgram&, const CodeEntered&) const;
    enum class HandedToUnseenCode : uint8_t { Nothing, WhatProgramMade };
    void letUnseenCodeRunAtStart(StartOfProgram&, HandedToUnseenCode) const;
    HandedToUnseenCode handedOverBy(FunctionID, const ThinSummary&, const SummaryCall&) const;
    HandedToUnseenCode handedOverBy(FunctionID, const SummaryAccess&) const;
    void evaluateSometimeFrom(StartOfProgram&, ModuleID) const;
    void evaluateAtStart(StartOfProgram&, const BodyIndexTable&, ModuleID) const;
    void followBodyAtStart(StartOfProgram&, const BodyIndexTable&, const BodyIndex&, FunctionID body) const;
    // A BODY THAT ONE CALL ENTERS ONCE IS PART OF THE START. `(function () { .. })()`, `function setUp() { .. }  setUp();`: every caller of the function is known, one site in the whole program may enter it,
    // that site is a plain call of the function itself, and it stands where the start passes once: in a block of a followed body that every way to a Return passes, outside of loops and handlers.
    // So the body runs once, there and then, and what it lays in such a block of its own is laid from that tick on. Null: the call enters as ever.
    const BodyIndex* bodyFollowedInPlaceOf(const StartOfProgram&, const BodyIndexTable&, FunctionID caller, const SummaryCall&, const CodeEntered&) const;
    void countSitesThatEnter();
    // By function: the calls and the accesses that may enter it, no further than two.
    Vector<uint8_t> m_numberOfSitesThatEnter;

    // ---- Values
    void leave(uint32_t node)
    {
        if (node != noSummaryNode)
            m_fates[node].add(Fate::Leaves);
    }
    bool escapesIn(FunctionID callee, std::optional<unsigned> entryParameter) const;
    void noteEdges(const ThinSummary&);
    void noteOperandsOfCalls(FunctionID, const ThinSummary&);
    void noteOperandsOfAccessesAndChanges(FunctionID, const ThinSummary&);
    void carryFatesBackThroughJoins(const ThinSummary&);
    bool everyCalleeReturnsFreshResult(FunctionID, const ThinSummary&, uint32_t call) const;
    bool isFreshAndOnlyReturned(FunctionID, const ThinSummary&, uint32_t node) const;
    void summarizeValues(FunctionID);
    void summarizeOperandsOfSharedCall(uint32_t shared);

    // ---- AOTLinkKnownEffectsInlines.h
public:
    // In the place of summarizeEffects().
    void summarizeKnownEffects();
    // Behind the lanes: LinkTables::known.
    void writeKnownEffects();

private:
    // What a function, a shared call or the lost functions do. `all`: by whichever exit.
    struct EffectsOfComponentInKnownEffects {
        Effects all { Effects::none() };
        LocationMasks writesOnReturn;
        uint64_t layoutsWritten { 0 };
    };
    // Where a record of a summary stands.
    enum class WayInKnownEffects : uint8_t { ToNoReturn, ToReturn, ToReturnUnderHandler };
    struct GivenArgumentsInKnownEffects {
        uint32_t parametersTakenAsExpected { 0 };
        uint32_t parametersCalled { 0 };
    };
    struct CallOfSeveralInKnownEffects {
        FunctionID caller;
        ValueID site;
        // Null: nobody takes what the call gives.
        FlowNodeID result;
        EffectsOfComponentInKnownEffects effects;
        bool entersOwnComponent { false };
        // The shared call that is all that it enters, or the number of shared calls for the lost functions. UINT32_MAX: it enters something else.
        uint32_t sharesEffectsWith { UINT32_MAX };
    };

    static EffectsOfComponentInKnownEffects knownEffectsOfUnknownCode() { return { Effects::unknownCode(), LocationMasks::top(), objectsOfAnyLayout }; }
    uint64_t bitsOfLayoutsAt(const LinkType&);
    uint64_t layoutsWrittenByChange(FunctionID, const SummaryChange&);
    uint64_t layoutsChangedByRowsOf(FunctionID, const ThinSummary&, uint32_t call, const Effects& itself);
    void findLayoutsOfPlainObjects();
    bool holdsOnlyPlainObjectsOfOneLayout(const LinkType&) const;
    bool listingOfKeysMayRunCode(FunctionID, const ThinSummary&) const;
    bool storeMayFindCode(LayoutID, PropertyKeyID) const;
    bool copyMayRunCode(FunctionID, const SummaryChange&) const;
    bool membersOfInstancesAreAsTableLists(const LinkType& receiver, PropertyKeyID) const;
    bool addLibraryFunctionBehindRow(const BuiltinOverload&, CodeFound&) const;
    bool callStillWaits(FunctionID, const SummaryCall&) const;
    bool callNeverRuns(FunctionID, const SummaryCall&) const;
    static void takeKnownEffects(const Effects& all, const LocationMasks& writesOnReturn, uint64_t layoutsWritten, WayInKnownEffects, EffectsOfComponentInKnownEffects& into);
    void takeKnownEffectsOfFunction(FunctionID callee, WayInKnownEffects, EffectsOfComponentInKnownEffects& into) const;
    bool mergeKnownEffectsOfCodeOutside(const CodeEntered&, uint32_t component, WayInKnownEffects, EffectsOfComponentInKnownEffects&) const;
    void addEffectsOfOperatorsLeftApart(FunctionID, const ThinSummary&, EffectsOfComponentInKnownEffects&) const;
    void addEffectsOfAccessItself(FunctionID, const ThinSummary&, const SummaryAccess&, const LinkType& subject, PropertyKeyID, WayInKnownEffects, EffectsOfComponentInKnownEffects&);
    static std::optional<uint32_t> entryParameterAt(const ThinSummary&, uint32_t node);
    std::optional<LinkType> typeExpectedOfParameter(FunctionID, uint32_t entryParameter) const;
    bool mergeKnownEffectsOfCodeFound(const CodeFound&, uint32_t component, WayInKnownEffects, EffectsOfComponentInKnownEffects&) const;
    void addFunctionsCalledUnder(LayoutID, PropertyKeyID, CodeFound&) const;
    bool judgeAccessGivenArguments(FunctionID, const ThinSummary&, const SummaryAccess&, PropertyKeyID, uint32_t component, WayInKnownEffects, EffectsOfComponentInKnownEffects&, GivenArgumentsInKnownEffects&);
    bool judgeCallGivenArguments(FunctionID, const ThinSummary&, const SummaryCall&, uint32_t component, WayInKnownEffects, EffectsOfComponentInKnownEffects&, GivenArgumentsInKnownEffects&);
    static bool mayBeCallOfSeveral(const SummaryCall&, const CodeEntered&);
    void indexCallsOf(FunctionID, const ThinSummary&);
    void noteIfCallOfSeveral(FunctionID, const ThinSummary&, uint32_t call, const CodeEntered&, const EffectsOfComponentInKnownEffects& ofCall, bool entersOwnComponent);
    EffectsOfComponentInKnownEffects knownEffectsOfBody(uint32_t function);
    static void finishKnownEffectsOfCall(EffectsOfComponentInKnownEffects&, bool mayEnterItself);

    // By function.
    Vector<uint64_t> m_layoutsWrittenByFunction;
    EffectsOfComponentInKnownEffects m_knownEffectsOfLostFunctions;
    // By shared call.
    Vector<EffectsOfComponentInKnownEffects> m_knownEffectsOfSharedCalls;
    // bitsOfLayoutsInKnownEffects(), and by family the bits of its members.
    Vector<uint64_t> m_bitsOfLayouts;
    Vector<uint64_t> m_bitsOfFamilies;
    // By layout: holdsOnlyPlainObjectsOfOneLayout().
    BitVector m_layoutsOfPlainObjects;
    Vector<LinkedEffectsGivenArguments> m_effectsGivenArguments;
    Vector<CallOfSeveralInKnownEffects> m_callsOfSeveral;
    // indexCallsOf(): by call of that summary the node of its first result, and the sites of its calls, ascending.
    FunctionID m_functionWhoseCallsAreIndexed;
    Vector<uint32_t> m_resultNodesOfCalls;
    Vector<ValueID> m_sitesOfCalls;

    // ---- AOTLinkKnownRowsInlines.h
    bool indexesAreAbsentAboveInstancesOf(BuiltinClassID) const;
    bool instancesAreAsClassMakesThem(const LinkType& instances, PristineAspectSet) const;
    bool surelyTakes(const BuiltinType& accepted, const LinkType&) const;
    bool rowSurelyTakesCall(const BuiltinOverload&, FunctionID, const ThinSummary&, const SummaryCall&) const;
    std::optional<bool> addEffectsOfRowsUpToOneThatSurelyTakes(BuiltinID canonical, FunctionID, const ThinSummary&, const SummaryCall&, CodeFound&, Effects&) const;
    bool copyThatDefinesMayRunCode(FunctionID, const SummaryChange&) const;

    const LinkFlow& m_flow;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    LinkTables& m_tables;
    // It keeps nothing: whatever a question of m_typing makes lies here and dies with it. What is to be typed is left in m_typesFound.
    TypeArena m_types;
    TypingContext m_typing;
    TypesFoundOfCallees m_typesFound;
    uint32_t m_numberOfFunctions;

    BitVector m_isSummarized;
    // By function, of those that are summarized.
    Vector<EntryPositions> m_entryPositions;

    // Ascending by key. The getters and setters that the prototype objects of classes hold.
    Vector<AccessorUnderKey> m_accessors;
    // By key: some layout lists an accessor under it. The scan lays the functions of such a field into node 0, so no node says which they are.
    BitVector m_keysOfAccessorFields;
    // By key: some field or method under it may hold what mayBeOtherCallableForLinkEffects().
    BitVector m_keysOfOtherCallables;
    // By layout: a lookup on an object of it may come to an accessor, a trap or an object that nobody has listed.
    BitVector m_layoutsThatMayFindAnything;
    bool m_lostLayoutsMayFindAnything { false };
    BitVector m_familiesThatMayFindAnything;
    Vector<Answer> m_keysThatObjectPrototypeNeverAnswers;

    // In the order of LinkFlow::calleesOfCalls.
    Vector<CodeEntered> m_codeOfCalls;
    // By function, one more than there are functions. Then in the order of ThinSummary::accesses.
    Vector<uint32_t> m_firstAccessOfFunction;
    Vector<CodeEntered> m_codeOfAccesses;
    Vector<FunctionID> m_functionsEntered;
    Effects m_effectsOfLostFunctions { Effects::none() };
    // Both by shared call.
    Vector<Effects> m_effectsOfSharedCalls;
    Vector<OperandsOfSharedCall> m_operandsOfSharedCalls;
    // By node: the functions, nodeOfLostFunctions(), and the shared calls. Into m_callees.
    Vector<uint32_t> m_firstCallee;
    Vector<FunctionID> m_callees;
    ComponentsOfLinkEffects m_components;
    // Empty where the program has one entry, and until somebody has to know.
    LedTo m_ledTo;
    // It has a top-level await, or something below it has. WHETHER A MODULE WAITS IS READ OFF THE IMPORTS AS THEY ARE WRITTEN, NEVER OFF THE ORDER IN WHICH A START COMES TO THEM:
    // whoever imports a member of a cycle waits for the root of the cycle, and a module that a site has evaluated at a time that nobody knows makes its importers wait as any other.
    // A member of a cycle below whose root something suspends runs at once, and counts as waiting: it is not followed.
    BitVector m_modulesThatMayWait;
    // `export function f() { .. }`: the variable is born as the function at the link, and no operation of any body names it. Those that unseen code finds on a namespace object (LinkFlow::variablesReadByUnseenCode).
    Vector<uint32_t> m_functionsBornInSightOfUnseenCode;

    // By node of the summary at hand.
    Vector<OptionSet<Fate>> m_fates;
    Vector<uint32_t> m_firstSourceOfJoin;
    Vector<uint32_t> m_sourcesOfJoins;
    Vector<uint32_t> m_joinsToVisit;
};

CalleeSummarizerForLinkEffects::CalleeSummarizerForLinkEffects(const LinkFlow& flow, LinkTables& tables)
    : m_flow(flow)
    , m_program(flow.program)
    , m_builtins(flow.program.builtins())
    , m_tables(tables)
    , m_types(flow.program.types, ArenaKeeps::UntilReset)
    , m_typing { flow.program, m_types, PositionTypes::knowingNothing() }
    , m_numberOfFunctions(tables.functions.size())
{
    RELEASE_ASSERT(m_numberOfFunctions == m_program.functions.size() && flow.summaries.size() == m_numberOfFunctions);
    RELEASE_ASSERT(flow.firstCallOfFunction.size() == m_numberOfFunctions + 1 && flow.calleesOfCalls.size() == flow.firstCallOfFunction.last());
    m_isSummarized.ensureSize(m_numberOfFunctions);
    m_entryPositions.fill(EntryPositions { }, m_numberOfFunctions);
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        const Function& function = m_program.functions[index];
        if (!flow.reached.get(index) || !function.hasBody())
            continue;
        RELEASE_ASSERT(flow.firstCallOfFunction[index + 1] - flow.firstCallOfFunction[index] == flow.summaries.at(FunctionID(index)).calls.size());
        m_isSummarized.quickSet(index);
        m_entryPositions[index] = { function.thisParameterIndex(), function.calleeParameterIndex(), function.newTargetParameterIndex(), function.firstParameterIndex(), function.numberOfPositionalParameters(), function.numberOfEntryParameters() };
    }

    indexHolders();
    findLayoutsThatMayFindAnything();
    findCodeEntered();
}

// One record stands for the call that starts a coroutine and for each of its resumptions. No operation of the body shows what the ramp allocates or what settling the promise looks for,
// and the saved frame holds the parameters when the call has returned.
bool CalleeSummarizerForLinkEffects::keepsDefaults(FunctionID id, const ThinSummary& summary, OptionSet<SummaryTrait> traits) const
{
    return m_program.at(id).isCoroutine() || summary.traits.containsAny(traits);
}

LinkType CalleeSummarizerForLinkEffects::typeAt(FunctionID function, uint32_t nodeInSummary) const
{
    if (nodeInSummary != noSummaryNode)
        return m_flow.typeAt(m_flow.nodeOf(function, nodeInSummary));
    LinkType undefined;
    undefined.primitives = Primitive::Undefined;
    return undefined;
}

// ---- What a lookup finds

void CalleeSummarizerForLinkEffects::indexHolders()
{
    uint32_t numberOfKeys = m_program.types.numberOfPropertyKeys();
    m_keysOfAccessorFields.ensureSize(numberOfKeys);
    m_keysOfOtherCallables.ensureSize(numberOfKeys);
    m_keysThatObjectPrototypeNeverAnswers.fill(Answer::NotAsked, numberOfKeys);

    auto note = [&](PropertyKeyID key, FlowNodeID holder, bool runsAtReads, bool runsAtStores) {
        if (!key)
            return;
        if (runsAtReads || runsAtStores)
            m_accessors.append(AccessorUnderKey { key, holder, runsAtReads, runsAtStores });
        else if (mayBeOtherCallableForLinkEffects(m_flow.typeAt(holder)))
            m_keysOfOtherCallables.quickSet(key.index());
    };

    // A lookup on a class object claims nothing, so its side is left out.
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        for (const SummaryMember& member : m_flow.summaries.at(FunctionID(index)).members) {
            if (member.side == ClassSide::Prototype)
                note(member.key, m_flow.member(member.classLayout, member.side, member.index), member.kind == MethodKind::Getter, member.kind == MethodKind::Setter);
        }
    }
    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes())
        note(name.key, name.stored, false, false);
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        const Layout& layout = m_program.layouts[index];
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            PropertyKeyID key = layout.fields[field].key;
            if (key && layout.fields[field].form.contains(FieldForm::IsAccessor))
                m_keysOfAccessorFields.quickSet(key.index());
            else
                note(key, m_flow.field({ LayoutID(index), field }), false, false);
        }
    }
    std::ranges::stable_sort(m_accessors, { }, &AccessorUnderKey::key);

    forEachRangeOfIndicesInParallel(m_flow.loops, numberOfKeys, [&](size_t first, size_t end) {
        for (size_t key = first; key < end; ++key)
            objectPrototypeNeverAnswers(PropertyKeyID(key));
    });
}

void CalleeSummarizerForLinkEffects::findLayoutsThatMayFindAnything()
{
    uint32_t numberOfLayouts = m_program.layouts.size();
    RELEASE_ASSERT(m_tables.layouts.size() == numberOfLayouts);
    BitVector byItself(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        // Of any other layout native code makes objects too: a proxy, an ordinary object, a cell of a built-in class.
        // writeLayoutsAndCells() has run. Beside LinkFlow::possibleChanges its table holds what is done through a lost value and what a store under `__proto__` does.
        bool mayFindAnything = !isFoundByShapeTest(m_program.layouts[index]) || m_flow.escapedLayouts.get(index)
            || m_tables.layouts[index].possibleChanges.containsAny({ LayoutChange::GetsAccessor, LayoutChange::ChangesPrototype })
            || m_flow.namesMayBeIncomplete(LayoutID(index), ClassSide::Prototype);
        if (mayFindAnything)
            byItself.quickSet(index);
    }

    // What is laid on the prototype object of a class is found from every class below it.
    m_layoutsThatMayFindAnything = answerAlongRows(numberOfLayouts, false, [&](LayoutID id) -> StepAlongRow {
        if (byItself.quickGet(id.index()))
            return { true, { } };
        LayoutID above = writtenBaseOf(m_program.at(id));
        if (!above)
            return { false, { } };
        return { std::nullopt, above };
    });

    for (size_t index : m_flow.lostLayouts)
        m_lostLayoutsMayFindAnything |= m_layoutsThatMayFindAnything.get(index);
    m_familiesThatMayFindAnything = m_flow.families.familiesWithSomeMemberThat([&](LayoutID member) {
        return m_layoutsThatMayFindAnything.get(member.index());
    });
}

bool CalleeSummarizerForLinkEffects::mayFindAnything(const LinkType& subject)
{
    switch (subject.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Prototype:
        return m_layoutsThatMayFindAnything.get(subject.layout.index());
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
        return m_familiesThatMayFindAnything.get(subject.family.index());
    case LinkType::Objects::Lost:
        return m_lostLayoutsMayFindAnything;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Every lookup that the objects of the program do not answer ends there. It neither has the key nor can ever get it: the run time refuses the store.
bool CalleeSummarizerForLinkEffects::objectPrototypeNeverAnswers(PropertyKeyID key)
{
    Answer& answer = m_keysThatObjectPrototypeNeverAnswers[key.index()];
    if (answer == Answer::NotAsked) {
        BuiltinID holder = m_builtins.canonicalID(Builtins::ObjectPrototype);
        bool neverAnswers = !findOwnMember(m_program, holder, key) && listsEveryMemberUnder(m_program, holder, key) && isClosedKey(m_program, holder, key);
        answer = neverAnswers ? Answer::Yes : Answer::No;
    }
    return answer == Answer::Yes;
}

void CalleeSummarizerForLinkEffects::addFunctionsAt(const LinkType& held, CodeFound& found) const
{
    switch (held.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        found.add(held.function);
        break;
    case LinkType::Functions::Constructors:
        for (LayoutID member : membersOf(m_program, m_flow.families.layoutsOf(held.constructorsOf))) {
            if (FunctionID constructor = m_program.at(member).constructor)
                found.add(constructor);
        }
        break;
    case LinkType::Functions::Lost:
        found.mayBeLost = true;
        break;
    }
    if (mayBeOtherCallableForLinkEffects(held))
        found.mayBeUnseen = true;
}

void CalleeSummarizerForLinkEffects::lookInLayout(LayoutID layout, bool hasOwnFields, PropertyKeyID key, MethodKind wanted, bool callsWhatIsFound, CodeFound& found) const
{
    auto lookAt = [&](const LinkType& held, bool isRun) {
        if (isRun) {
            addFunctionsAt(held, found);
            // Nobody here follows what the getter answers with.
            found.mayBeUnseen |= callsWhatIsFound;
        } else if (callsWhatIsFound && mayBeOtherCallableForLinkEffects(held))
            found.mayBeUnseen = true;
    };

    if (hasOwnFields) {
        if (std::optional<FieldReference> field = findField(m_program, layout, key)) {
            if (fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor))
                found.mayBeUnseen = true;
            else
                lookAt(m_flow.typeAt(m_flow.field(*field)), false);
        }
    }
    // The object may not have the field yet, so what stands above it is found too.
    for (const LinkFlow::FoundMember& member : m_flow.findMembers(layout, ClassSide::Prototype, key))
        lookAt(m_flow.typeAt(m_flow.member(member.classLayout, ClassSide::Prototype, member.index)), member.kind == wanted);
    // And what the program has stored on the prototype objects of the constructor functions on the way.
    uint32_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
        if (const LinkFlow::NameOnPrototype* name = m_flow.findNameOnPrototype(current, key))
            lookAt(m_flow.typeAt(name->stored), false);
    }
}

// `prototype` of a class is a value of the class object's own that nobody can define anew.
// `__proto__` off a prototype object that is as it was written comes to the engine's getter on Object.prototype, and the link above the home object of `super` is read without any lookup.
bool CalleeSummarizerForLinkEffects::readsFixedPartOfClass(const LinkType& subject, PropertyKeyID key, bool hasOtherReceiver)
{
    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
    if (mayBeOtherCallableForLinkEffects(subject) || (subject.primitives & ~primitivesWithoutMembers))
        return false;
    bool holdsNoObject = subject.objects == LinkType::Objects::None;
    bool holdsNoFunction = subject.functions == LinkType::Functions::None;
    LayoutID classOfOnlyClassObject = subject.functions == LinkType::Functions::Closure && isClassConstructorOf(m_program.at(subject.function).kind) ? m_program.at(subject.function).constructedClass : LayoutID();
    auto staysAsWrittenWithoutTheName = [&](LayoutID classLayout, ClassSide side) {
        return !m_layoutsThatMayFindAnything.get(classLayout.index()) && !m_flow.namesMayBeIncomplete(classLayout, side) && m_flow.findsPrototypeLinkOfSubject(classLayout, side, key, hasOtherReceiver);
    };
    switch (m_program.nameClassOf(key)) {
    case NameClass::Prototype:
        return holdsNoObject && (classOfOnlyClassObject || subject.functions == LinkType::Functions::Constructors);
    case NameClass::Proto:
        if (holdsNoObject)
            return classOfOnlyClassObject && staysAsWrittenWithoutTheName(classOfOnlyClassObject, ClassSide::Static);
        return holdsNoFunction && subject.objects == LinkType::Objects::Prototype && staysAsWrittenWithoutTheName(subject.layout, ClassSide::Prototype);
    default:
        return false;
    }
}

// Less than everything is claimed only of objects of the program's own layouts, under a written key. Of an array, a string, a function or a built-in nobody here knows what was laid on it or above it.
void CalleeSummarizerForLinkEffects::findCodeBehindLookup(const LinkType& subject, PropertyKeyID key, SummaryAccess::Kind kind, bool hasOtherReceiver, bool callsWhatIsFound, CodeFound& found)
{
    if (subject.isNothing())
        return;
    if (kind == SummaryAccess::Kind::Read && key && !callsWhatIsFound && readsFixedPartOfClass(subject, key, hasOtherReceiver))
        return;
    // A lookup on one of these throws.
    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
    bool isOfProgram = !mayBeOtherCallableForLinkEffects(subject) && subject.functions == LinkType::Functions::None && !(subject.primitives & ~primitivesWithoutMembers);
    // A member that the table of built-ins lists on Object.prototype is found by callObserved(): builtinsEnteredBy().
    bool isFoundByCall = callsWhatIsFound && key && findOwnMember(m_program, m_builtins.canonicalID(Builtins::ObjectPrototype), key);
    if (!isOfProgram || !key || m_program.isIndexLike(key) || mayFindAnything(subject) || (!isFoundByCall && !objectPrototypeNeverAnswers(key))) {
        found.mayBeUnseen = true;
        return;
    }

    MethodKind wanted = MethodKind::Getter;
    switch (kind) {
    case SummaryAccess::Kind::Read:
        break;
    case SummaryAccess::Kind::Store:
        wanted = MethodKind::Setter;
        break;
    case SummaryAccess::Kind::Define:
    case SummaryAccess::Kind::Has:
        return;
    }

    switch (subject.objects) {
    case LinkType::Objects::None:
        return;
    case LinkType::Objects::Layout:
        lookInLayout(subject.layout, true, key, wanted, callsWhatIsFound, found);
        return;
    case LinkType::Objects::Prototype:
        lookInLayout(subject.layout, false, key, wanted, callsWhatIsFound, found);
        return;
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        break;
    }

    // Whatever any layout has under the key.
    for (const AccessorUnderKey& accessor : std::ranges::equal_range(m_accessors, key, { }, &AccessorUnderKey::key)) {
        if (!(wanted == MethodKind::Getter ? accessor.runsAtReads : accessor.runsAtStores))
            continue;
        addFunctionsAt(m_flow.typeAt(accessor.holder), found);
        found.mayBeUnseen |= callsWhatIsFound;
    }
    if (m_keysOfAccessorFields.get(key.index()) || (callsWhatIsFound && m_keysOfOtherCallables.get(key.index())))
        found.mayBeUnseen = true;
}

// Of a lookup that enters no code. A store that finds no setter does no more than the definition of a property, whose row says what that is, and a read reads what that reads.
Effects CalleeSummarizerForLinkEffects::effectsOfLookupItself(SummaryAccess::Kind kind, PropertyKeyID key) const
{
    constexpr Effects ofDefinition = declaredBoundOf(Opcode::DefineGetter);
    constexpr AbstractHeapSet heapsUnderKeys { AbstractHeap::ObjectFields, AbstractHeap::NamedProperties };

    Effects effects = declaredBoundOf(NamedEffectSet::Allocates);
    effects.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
    effects.reads.merge(ofDefinition.reads);
    effects.reads.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards });
    effects.mayThrow = ThrowEffect(true);
    switch (kind) {
    case SummaryAccess::Kind::Read:
    case SummaryAccess::Kind::Has:
        return effects;
    case SummaryAccess::Kind::Store:
    case SummaryAccess::Kind::Define:
        break;
    }
    if (!key) {
        effects.writes.merge(ofDefinition.writes);
        return effects;
    }
    effects.writes.merge(ofDefinition.writes.without(heapsUnderKeys));
    effects.writes.add(m_program, MemoryLocation::objectField(key));
    effects.writes.add(m_program, MemoryLocation::at(AbstractHeap::NamedProperties, key.index()));
    return effects;
}

// ---- What the rows of a built-in say

// The one that the scan has bound, which may be null, or those that callObserved() has found under the name or at the subject.
std::span<const BuiltinID> CalleeSummarizerForLinkEffects::builtinsEnteredBy(FunctionID id, const ThinSummary& summary, uint32_t index) const
{
    const SummaryCall& call = summary.calls[index];
    if (call.callee == SummaryCall::Callee::Builtin)
        return std::span<const BuiltinID>(&call.builtin, 1);
    return m_flow.calleesOfCalls[m_flow.firstCallOfFunction[id.index()] + index].builtins.span();
}

// False: there is no row to go by, or the functor has said false.
template<typename Functor>
bool CalleeSummarizerForLinkEffects::forEachRowOf(BuiltinID builtin, const SummaryCall& call, const Functor& functor) const
{
    if (!builtin)
        return false;
    BuiltinCallMode mode = call.isConstruction ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
    bool hasRow = false;
    for (const BuiltinOverload& row : m_builtins.overloadsOf(m_builtins.canonicalID(builtin))) {
        if (row.mode != mode)
            continue;
        hasRow = true;
        if (row.isUndeclared() || !functor(row))
            return false;
    }
    return hasRow;
}

// The call does what the functions do, whatever they are handed, and then touches only what the row names.
bool CalleeSummarizerForLinkEffects::rowSaysAllOfCallsOf(const BuiltinOverload& row, BuiltinOperand callee) const
{
    bool rowListsCall = false;
    for (const BuiltinCallback& callback : row.flow.callbacks) {
        if (callback.callee != callee)
            continue;
        if (callback.constructs || callback.afterwards != BuiltinCallbackAftermath::TouchesOnlyWhatRowNames || rowLooksIntoAnswerInLinkEffects(callback.resultUse))
            return false;
        rowListsCall = true;
    }
    return rowListsCall;
}

bool CalleeSummarizerForLinkEffects::addEffectsOnOperand(const BuiltinOverload& row, BuiltinOperand position, const LinkType& type, CodeFound& found, Effects& effects) const
{
    if (type.isNothing())
        return true;
    bool mayBeObject = mayBeObjectForLinkEffects(type);
    // The native code reads no operand that its row does not take: `Math` in `Math.floor(x)`.
    const BuiltinParameter* parameter = m_builtins.parameterFor(row, position);
    if (!parameter)
        return true;

    if (readsCharactersOfString(parameter->conversion) && (type.primitives & Primitive::AnyString))
        effects.merge(declaredBoundOf(NamedEffectSet::ResolvesRopes));
    if (!parameter->protocols.isEmpty()) {
        if (mayBeObject)
            return false;
        Effects ofProtocols = effectsOfProtocolsOn(m_typing, parameter->protocols, type.primitivesAsType());
        if (ofProtocols.mayRunOtherCode())
            return false;
        effects.merge(ofProtocols);
    }
    if (!mayBeObject)
        return true;

    if (parameter->uses.containsAny(usesThatRunCodeNobodyNamesInLinkEffects) || parameter->roles.contains(BuiltinOperandRole::DescribedWhenRefused))
        return false;
    if (parameter->uses.containsAny(usesThatCallOperandInLinkEffects)) {
        if (type.objects != LinkType::Objects::None || mayBeOtherCallableForLinkEffects(type) || !rowSaysAllOfCallsOf(row, parameter->operand))
            return false;
        addFunctionsAt(type, found);
        effects.mayThrow = ThrowEffect(true);
    }
    if (parameter->uses.containsAny(usesThatChangeShapeInLinkEffects))
        effects.writes.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards, AbstractHeap::ShapeWords, AbstractHeap::OrderWords });
    return true;
}

// Never narrower than CallInWalkBuiltins::effectsOfRow() of walk/AOTWalkBuiltins.cpp. False: nobody says what the call does.
bool CalleeSummarizerForLinkEffects::addEffectsOfRow(const BuiltinOverload& row, FunctionID id, const ThinSummary& summary, const SummaryCall& call, CodeFound& found, Effects& effects) const
{
    if (row.libraryFunction())
        return addLibraryFunctionBehindRow(row, found);
    if (row.libraryFunction() || (row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode)))
        return false;
    bool callsHandlers = std::ranges::any_of(row.flow.handlerCalls, [](const BuiltinHandlerCall& handler) {
        return !isCalledAfterReturn(handler.times);
    });
    if (callsHandlers)
        return false;

    // The generator of the table derives heaps from uses, so a row that changes or reads an operand names a heap for it.
    OperandUses uses;
    if (row.receiver)
        uses.add(row.receiver->uses);
    for (const BuiltinParameter& parameter : row.parameters)
        uses.add(parameter.uses);
    if ((uses.containsAny(usesThatChangeOperandInLinkEffects) && row.effects.writes.isEmpty()) || (uses.contains(OperandUse::ReadsContents) && row.effects.reads.isEmpty()))
        return false;
    // A row names nothing only where it has no operand either, and what such a function answers comes out of some state.
    if (!row.receiver && row.parameters.empty() && row.effects.reads.isEmpty() && row.effects.writes.isEmpty())
        return false;

    effects.reads.add(row.effects.reads);
    effects.writes.add(row.effects.writes);
    switch (row.effects.capability) {
    case BuiltinCapability::None:
        break;
    case BuiltinCapability::Heap:
    case BuiltinCapability::JS: {
        Effects ofHeap = declaredBoundOf(NamedEffectSet::Allocates);
        ofHeap.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
        ofHeap.merge(declaredBoundOf(NamedEffectSet::ResolvesRopes));
        ofHeap.mayThrow = ThrowEffect::onlyForLackOfMemory();
        effects.merge(ofHeap);
        break;
    }
    }
    // The error is allocated on the way to the handler.
    if (row.throws()) {
        effects.mayThrow = ThrowEffect(true);
        effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    }
    if (m_builtins.has(BuiltinMark::MayTerminateProcess, row.builtin))
        effects.mayBeTerminated = true;

    if (call.receiver != noSummaryNode && !addEffectsOnOperand(row, BuiltinOperand::receiver(), typeAt(id, call.receiver), found, effects))
        return false;
    std::span<const uint32_t> arguments = argumentsOf(summary, call);
    for (uint32_t argument = 0; argument < arguments.size(); ++argument) {
        if (argument > BuiltinOperand::maxArgumentIndex || !addEffectsOnOperand(row, BuiltinOperand::argument(argument), typeAt(id, arguments[argument]), found, effects))
            return false;
    }
    return true;
}

bool CalleeSummarizerForLinkEffects::passesNoIterable(FunctionID id, const ThinSummary& summary, const SummaryCall& call) const
{
    std::span<const uint32_t> arguments = argumentsOf(summary, call);
    if (arguments.empty())
        return true;
    LinkType first = typeAt(id, arguments[0]);
    return !first.isNothing() && !mayBeObjectForLinkEffects(first) && !(first.primitives & ~Primitive::UndefinedOrNull);
}

bool CalleeSummarizerForLinkEffects::addEffectsOfRows(BuiltinID named, FunctionID id, const ThinSummary& summary, const SummaryCall& call, CodeFound& found, Effects& effects) const
{
    // The rows of a construction have the callee as new.target.
    if (!named || call.passesList || call.newTarget != noSummaryNode)
        return false;
    // A test run may have put a function of its own in its place.
    BuiltinID builtin = m_builtins.canonicalID(named);
    if (m_program.isUnlocked(named) || m_program.isUnlocked(builtin))
        return false;
    const Builtin& entry = m_builtins.at(builtin);
    if (entry.flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode }) || m_builtins.has(BuiltinMark::RunsWaitingJobs, builtin))
        return false;
    // The call reads it off its holder itself. That may run an accessor, and the first read of a member of the host may run the code that makes it.
    if (call.callee != SummaryCall::Callee::Builtin && (!isFunctionForLinkEffects(entry.kind) || isMadeWithCodeOfLibrary(m_builtins, builtin)))
        return false;
    // The rows behind the first that surely takes what is passed say nothing of the call.
    if (std::optional<bool> answer = addEffectsOfRowsUpToOneThatSurelyTakes(builtin, id, summary, call, found, effects))
        return *answer;
    // `new Map(pairs)` calls the `set` that it finds on the new map, and no row names that member. Without an iterable the constructor never reads it.
    if (call.isConstruction && entry.instanceClass && m_builtins.at(entry.instanceClass).constructorAdder && !passesNoIterable(id, summary, call))
        return false;
    return forEachRowOf(builtin, call, [&](const BuiltinOverload& row) {
        return addEffectsOfRow(row, id, summary, call, found, effects);
    });
}

bool CalleeSummarizerForLinkEffects::rowsHandOperandToNobody(BuiltinID builtin, const SummaryCall& call, BuiltinOperand position) const
{
    return forEachRowOf(builtin, call, [&](const BuiltinOverload& row) {
        const BuiltinParameter* parameter = m_builtins.parameterFor(row, position);
        return parameter && parameter->uses.containsOnly(usesThatHandOperandToNobodyInLinkEffects) && parameter->protocols.isEmpty();
    });
}

bool CalleeSummarizerForLinkEffects::rowsGiveFreshResult(BuiltinID builtin, const SummaryCall& call) const
{
    return forEachRowOf(builtin, call, [&](const BuiltinOverload& row) {
        return row.result.type && !row.resultIsHeld() && m_builtins.at(row.result.type).freshness == BuiltinFreshness::Yes;
    });
}

// ---- Effects

CalleeSummarizerForLinkEffects::CodeEntered CalleeSummarizerForLinkEffects::keep(CodeFound& found, Vector<FunctionID>& functionsEntered)
{
    std::ranges::sort(found.functions);
    auto repeats = std::ranges::unique(found.functions);
    found.functions.shrink(repeats.begin() - found.functions.begin());
    CodeEntered code { static_cast<uint32_t>(functionsEntered.size()), static_cast<uint32_t>(found.functions.size()), found.mayBeLost, found.mayBeUnseen };
    functionsEntered.appendVector(found.functions);
    return code;
}

void CalleeSummarizerForLinkEffects::findCodeEnteredByCall(FunctionID id, const ThinSummary& summary, uint32_t index, CodeFound& found)
{
    const SummaryCall& call = summary.calls[index];
    const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCalls[m_flow.firstCallOfFunction[id.index()] + index];
    for (FunctionID callee : callees.listed)
        found.add(callee);
    for (uint32_t shared : callees.shared)
        found.add(FunctionID(nodeOfSharedCall(shared)));
    found.mayBeLost = callees.mayEnterLost;
    found.mayBeUnseen = callees.mayEnterUnseen;

    // What the table of built-ins answers for a primitive or a built-in never changes, so there callObserved() is taken at its word. All else is asked again of how things stand at the end.
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        if (callStillWaits(id, call))
            break;
        if (call.function)
            found.add(call.function);
        else
            found.mayBeUnseen = true;
        break;
    case SummaryCall::Callee::Name: {
        LinkType receiver = typeAt(id, call.receiver);
        receiver.primitives = 0;
        receiver.builtin = { };
        if (membersOfInstancesAreAsTableLists(receiver, call.key))
            break;
        findCodeBehindLookup(receiver, call.key, SummaryAccess::Kind::Read, false, true, found);
        break;
    }
    case SummaryCall::Callee::Value: {
        LinkType subject = typeAt(id, call.subject);
        subject.builtin = { };
        addFunctionsAt(subject, found);
        break;
    }
    case SummaryCall::Callee::Builtin:
        break;
    }

    Effects ofRows = Effects::none();
    for (BuiltinID builtin : builtinsEnteredBy(id, summary, index)) {
        if (!addEffectsOfRows(builtin, id, summary, call, found, ofRows))
            found.mayBeUnseen = true;
    }
}

// By function, on all threads: each looks at how the flow has left things and writes its own rows. The answers that are remembered by family and by key are all there by now.
void CalleeSummarizerForLinkEffects::findCodeEntered()
{
    const ParallelLoops& loops = m_flow.loops;
    m_codeOfCalls.fill(CodeEntered { }, m_flow.calleesOfCalls.size());
    m_firstAccessOfFunction.reserveInitialCapacity(m_numberOfFunctions + 1);
    uint32_t numberOfAccesses = 0;
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        m_firstAccessOfFunction.append(numberOfAccesses);
        if (isSummarized(index))
            numberOfAccesses += m_flow.summaries.at(FunctionID(index)).accesses.size();
    }
    m_firstAccessOfFunction.append(numberOfAccesses);
    m_codeOfAccesses.fill(CodeEntered { }, numberOfAccesses);

    ChunksOfIndices division = divideIntoChunks(m_numberOfFunctions);
    Vector<Vector<FunctionID>> functionsOfChunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk) {
        Vector<FunctionID>& functionsEntered = functionsOfChunks[chunk];
        for (uint32_t index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            if (!isSummarized(index))
                continue;
            FunctionID id(index);
            const ThinSummary& summary = m_flow.summaries.at(id);
            for (uint32_t call = 0; call < summary.calls.size(); ++call) {
                CodeFound found;
                findCodeEnteredByCall(id, summary, call, found);
                m_codeOfCalls[m_flow.firstCallOfFunction[index] + call] = keep(found, functionsEntered);
            }
            for (uint32_t access = 0; access < summary.accesses.size(); ++access) {
                CodeFound found;
                // ThinSummary::ownEffects has what an access to contents does.
                if (!summary.accesses[access].isToContents())
                    findCodeBehindLookup(typeAt(id, summary.accesses[access].subject), m_flow.keyOf(id, summary.accesses[access]), summary.accesses[access].kind, summary.accesses[access].receiver != noSummaryNode, false, found);
                m_codeOfAccesses[m_firstAccessOfFunction[index] + access] = keep(found, functionsEntered);
            }
        }
    });

    Vector<uint32_t> firstFunctionOfChunk;
    firstFunctionOfChunk.reserveInitialCapacity(division.size());
    uint32_t numberOfFunctionsEntered = 0;
    for (const Vector<FunctionID>& functionsEntered : functionsOfChunks) {
        firstFunctionOfChunk.append(numberOfFunctionsEntered);
        numberOfFunctionsEntered += functionsEntered.size();
    }
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk) {
        uint32_t first = division.firstIndexOf[chunk];
        uint32_t end = division.firstIndexOf[chunk + 1];
        for (uint32_t call = m_flow.firstCallOfFunction[first]; call < m_flow.firstCallOfFunction[end]; ++call)
            m_codeOfCalls[call].firstFunction += firstFunctionOfChunk[chunk];
        for (uint32_t access = m_firstAccessOfFunction[first]; access < m_firstAccessOfFunction[end]; ++access)
            m_codeOfAccesses[access].firstFunction += firstFunctionOfChunk[chunk];
    });
    m_functionsEntered = joinChunks<FunctionID>(loops, WTF::move(functionsOfChunks));
}

// What the operation does before any body is entered. ThinSummary::ownEffects leaves the calls out.
Effects CalleeSummarizerForLinkEffects::effectsOfCallItself(FunctionID id, const ThinSummary& summary, uint32_t index) const
{
    const SummaryCall& call = summary.calls[index];
    Effects effects = Effects::none();
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
    case SummaryCall::Callee::Builtin:
        break;
    // Nothing arrives at the receiver: the operation is never run, or what it converts or awaits is never an object, and nothing is looked up.
    case SummaryCall::Callee::Name:
        if (!typeAt(id, call.receiver).isNothing())
            effects = effectsOfLookupItself(SummaryAccess::Kind::Read, call.key);
        break;
    case SummaryCall::Callee::Value:
        effects.mayThrow = ThrowEffect(true);
        break;
    }
    for (BuiltinID builtin : builtinsEnteredBy(id, summary, index)) {
        CodeFound found;
        if (!addEffectsOfRows(builtin, id, summary, call, found, effects))
            return Effects::unknownCode();
    }
    if (call.passesList) {
        // The list may be too long for the stack, which no check of the stack at an entry covers.
        effects.mayThrow = ThrowEffect(true);
        effects.reads.add(AbstractHeapSet { AbstractHeap::Elements, AbstractHeap::ArrayStorage });
    }
    if (call.isConstruction) {
        effects.mayThrow = ThrowEffect(true);
        effects.merge(declaredBoundOf(Opcode::GetConstructPrototype));
        effects.merge(declaredBoundOf(Opcode::NewObject));
    }
    return effects;
}

void CalleeSummarizerForLinkEffects::mergeEffectsOfCodeOutside(const CodeEntered& code, uint32_t component, const ComponentsOfLinkEffects& components, Effects& effects) const
{
    for (FunctionID callee : functionsOf(code)) {
        if (components.componentOfFunction[callee.index()] == component)
            continue;
        if (isSharedCall(callee.index())) {
            effects.merge(m_effectsOfSharedCalls[callee.index() - nodeOfSharedCall(0)]);
            continue;
        }
        const LinkedFunction& linked = m_tables.function(callee);
        effects.merge(linked.effectsOnReturn);
        effects.merge(linked.effectsOnThrow);
    }
    if (code.mayBeLost && components.componentOfFunction[nodeOfLostFunctions()] != component)
        effects.merge(m_effectsOfLostFunctions);
}

// ThinSummary::ownEffects is made of the declared bounds, which leave out the code that an operation runs on an operand (ir/AOTEffectsOfOperations.h).
// Where no call by name stands for that code the scan lays the operand into node 0 and marks the edge: an object that goes there may have its code run there and then.
// An edge without the mark only lets the value go: what is thrown, what is stored where nobody looks.
bool CalleeSummarizerForLinkEffects::handsObjectToCodeNobodyNames(FunctionID id, const ThinSummary& summary) const
{
    return std::ranges::any_of(summary.edges, [&](const SummaryEdge& edge) {
        return edge.runsCodeOfValue && summary.nodes[edge.to].kind == SummaryNode::Kind::Unknown && mayBeObjectForLinkEffects(m_flow.passedBy(typeAt(id, edge.from), edge.passes));
    });
}

bool CalleeSummarizerForLinkEffects::changeMayRunCode(FunctionID id, const SummaryChange& change) const
{
    const LinkType subject = typeAt(id, change.subject);
    if (subject.isNothing())
        return false;
    switch (change.kind) {
    // A getter of the source answers the copy, and no edge leads the source into node 0.
    case SummaryChange::Kind::CopiesInto:
        return copyMayRunCode(id, change);
    // On a proxy each of these runs a trap.
    case SummaryChange::Kind::Freezes:
    case SummaryChange::Kind::Deletes:
    case SummaryChange::Kind::DefinesByDescriptor:
    case SummaryChange::Kind::ChangesPrototype:
        return mayBeOtherCallableForLinkEffects(subject);
    case SummaryChange::Kind::BecomesPrototype:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Effects CalleeSummarizerForLinkEffects::effectsOfBodyAndOfCodeOutside(uint32_t function, const ComponentsOfLinkEffects& components) const
{
    FunctionID id(function);
    const ThinSummary& summary = m_flow.summaries.at(id);
    // Listing the keys of a proxy runs a trap, and so does a walk up a chain that a proxy stands in: ThinSummary::inspections.
    if (keepsDefaults(id, summary, { SummaryTrait::Suspends, SummaryTrait::HasDirectEval }) || someInspectionMayRunCode(m_flow, m_tables, id))
        return Effects::unknownCode();
    if (handsObjectToCodeNobodyNames(id, summary))
        return Effects::unknownCode();
    for (const SummaryChange& change : summary.changes) {
        if (changeMayRunCode(id, change))
            return Effects::unknownCode();
    }

    uint32_t component = components.componentOfFunction[function];
    Effects effects = summary.ownEffects;
    for (uint32_t call = 0; call < summary.calls.size(); ++call) {
        const CodeEntered& code = codeOfCall(id, call);
        if (code.mayBeUnseen)
            return Effects::unknownCode();
        effects.merge(effectsOfCallItself(id, summary, call));
        mergeEffectsOfCodeOutside(code, component, components, effects);
    }
    for (uint32_t access = 0; access < summary.accesses.size(); ++access) {
        const CodeEntered& code = codeOfAccess(id, access);
        if (code.mayBeUnseen)
            return Effects::unknownCode();
        if (!summary.accesses[access].isToContents())
            effects.merge(effectsOfLookupItself(summary.accesses[access].kind, m_flow.keyOf(id, summary.accesses[access])));
        mergeEffectsOfCodeOutside(code, component, components, effects);
    }
    return effects;
}

// ---- Values

bool CalleeSummarizerForLinkEffects::escapesIn(FunctionID callee, std::optional<unsigned> entryParameter) const
{
    if (!entryParameter)
        return false;
    std::span<const LinkedParameter> parameters = m_tables.parametersOf(callee);
    return *entryParameter >= parameters.size() || parameters[*entryParameter].escapes;
}

void CalleeSummarizerForLinkEffects::noteEdges(const ThinSummary& summary)
{
    for (const SummaryEdge& edge : summary.edges) {
        switch (summary.nodes[edge.to].kind) {
        case SummaryNode::Kind::Result:
            m_fates[edge.from].add(Fate::IsReturned);
            break;
        // carryFatesBackThroughJoins().
        case SummaryNode::Kind::Join:
            break;
        case SummaryNode::Kind::Unknown:
        case SummaryNode::Kind::Parameter:
        case SummaryNode::Kind::Thrown:
        case SummaryNode::Kind::Capture:
        case SummaryNode::Kind::Variable:
        case SummaryNode::Kind::Field:
        case SummaryNode::Kind::Member:
        case SummaryNode::Kind::Contents:
        case SummaryNode::Kind::Constant:
        case SummaryNode::Kind::Allocation:
        case SummaryNode::Kind::ClassInstance:
        case SummaryNode::Kind::Closure:
        case SummaryNode::Kind::Builtin:
        case SummaryNode::Kind::ResultOfCall:
        case SummaryNode::Kind::ResultOfAccess:
        case SummaryNode::Kind::ResultOfOperator:
        case SummaryNode::Kind::ResultOfConversion:
            leave(edge.from);
            break;
        }
    }
}

void CalleeSummarizerForLinkEffects::noteOperandsOfCalls(FunctionID id, const ThinSummary& summary)
{
    for (uint32_t index = 0; index < summary.calls.size(); ++index) {
        const SummaryCall& call = summary.calls[index];
        const CodeEntered& code = codeOfCall(id, index);
        std::span<const uint32_t> arguments = argumentsOf(summary, call);
        auto leaveEveryOperand = [&] {
            leave(call.subject);
            leave(call.receiver);
            for (uint32_t argument : arguments)
                leave(argument);
        };

        leave(call.newTarget);
        if (code.mayBeUnseen || code.mayBeLost || call.passesList) {
            leaveEveryOperand();
            continue;
        }
        for (BuiltinID builtin : builtinsEnteredBy(id, summary, index)) {
            leave(call.subject);
            if (!rowsHandOperandToNobody(builtin, call, BuiltinOperand::receiver()))
                leave(call.receiver);
            for (uint32_t argument = 0; argument < arguments.size(); ++argument) {
                if (!rowsHandOperandToNobody(builtin, call, BuiltinOperand::argument(argument)))
                    leave(arguments[argument]);
            }
        }
        // The functions entered are those that the native code calls, with what the rows say.
        if (call.callee == SummaryCall::Callee::Builtin)
            continue;

        for (FunctionID callee : functionsOf(code)) {
            if (isSharedCall(callee.index())) {
                const OperandsOfSharedCall& operands = m_operandsOfSharedCalls[callee.index() - nodeOfSharedCall(0)];
                if (!operands.everyCalleeIsSummarized) {
                    leaveEveryOperand();
                    break;
                }
                if (operands.thisValueEscapes)
                    leave(call.receiver);
                if (operands.calleeEscapes || (call.isConstruction && call.newTarget == noSummaryNode && operands.newTargetEscapes))
                    leave(call.subject);
                for (uint32_t argument = 0; argument < arguments.size(); ++argument) {
                    if (argument >= operands.argumentEscapes.size() || operands.argumentEscapes[argument])
                        leave(arguments[argument]);
                }
                continue;
            }
            if (!isSummarized(callee.index())) {
                leaveEveryOperand();
                break;
            }
            const EntryPositions& positions = m_entryPositions[callee.index()];
            if (call.callee == SummaryCall::Callee::Binding) {
                // The operands of CallDirect are the callee's entry parameters, in their order.
                for (uint32_t argument = 0; argument < arguments.size(); ++argument) {
                    if (arguments.size() != positions.numberOfEntryParameters || escapesIn(callee, argument))
                        leave(arguments[argument]);
                }
                continue;
            }
            if (escapesIn(callee, positions.thisValue))
                leave(call.receiver);
            // Where a construction names no new.target the callee itself arrives there.
            bool calleeIsNewTarget = call.isConstruction && call.newTarget == noSummaryNode;
            if (escapesIn(callee, positions.callee) || (calleeIsNewTarget && escapesIn(callee, positions.newTarget)))
                leave(call.subject);
            // An argument behind the last positional parameter goes into a list, out of which anybody may read it.
            for (uint32_t argument = 0; argument < arguments.size(); ++argument) {
                if (argument >= positions.numberOfPositional || escapesIn(callee, positions.firstPositional + argument))
                    leave(arguments[argument]);
            }
        }
    }
}

void CalleeSummarizerForLinkEffects::noteOperandsOfAccessesAndChanges(FunctionID id, const ThinSummary& summary)
{
    for (uint32_t index = 0; index < summary.accesses.size(); ++index) {
        const SummaryAccess& access = summary.accesses[index];
        // An accessor is handed the object that it was found on, or the receiver of `super.x`.
        if (!codeOfAccess(id, index).isNone()) {
            leave(access.subject);
            leave(access.receiver);
        }
        // An object among the keys is converted by code of its own.
        if (!access.isToContents())
            leave(access.keyNode);
        // What is no promise is what the operation goes on with.
        if (access.whatHoldsNothing != noSummaryNode)
            leave(access.subject);
        switch (access.kind) {
        case SummaryAccess::Kind::Store:
        case SummaryAccess::Kind::Define:
            leave(access.value);
            break;
        case SummaryAccess::Kind::Read:
        case SummaryAccess::Kind::Has:
            break;
        }
    }
    for (const SummaryChange& change : summary.changes) {
        leave(change.subject);
        // What is put below a prototype holds it, and is handed to nobody by that.
        if (change.kind != SummaryChange::Kind::BecomesPrototype)
            leave(change.other);
    }
}

// What becomes of a join becomes of all that arrives at it. A fate is only ever added, so a join is visited at most once for each.
void CalleeSummarizerForLinkEffects::carryFatesBackThroughJoins(const ThinSummary& summary)
{
    uint32_t numberOfNodes = summary.nodes.size();
    auto entersJoin = [&](const SummaryEdge& edge) {
        return summary.nodes[edge.to].kind == SummaryNode::Kind::Join;
    };

    m_firstSourceOfJoin.fill(0, numberOfNodes + 2);
    for (const SummaryEdge& edge : summary.edges) {
        if (entersJoin(edge))
            ++m_firstSourceOfJoin[edge.to + 2];
    }
    for (uint32_t node = 0; node < numberOfNodes; ++node)
        m_firstSourceOfJoin[node + 2] += m_firstSourceOfJoin[node + 1];
    m_sourcesOfJoins.fill(0, m_firstSourceOfJoin.last());
    for (const SummaryEdge& edge : summary.edges) {
        if (entersJoin(edge))
            m_sourcesOfJoins[m_firstSourceOfJoin[edge.to + 1]++] = edge.from;
    }

    m_joinsToVisit.shrink(0);
    for (uint32_t node = 0; node < numberOfNodes; ++node) {
        if (summary.nodes[node].kind == SummaryNode::Kind::Join && !m_fates[node].isEmpty())
            m_joinsToVisit.append(node);
    }
    while (!m_joinsToVisit.isEmpty()) {
        uint32_t join = m_joinsToVisit.takeLast();
        for (uint32_t place = m_firstSourceOfJoin[join]; place < m_firstSourceOfJoin[join + 1]; ++place) {
            uint32_t source = m_sourcesOfJoins[place];
            if (m_fates[source].containsAll(m_fates[join]))
                continue;
            m_fates[source].add(m_fates[join]);
            if (summary.nodes[source].kind == SummaryNode::Kind::Join)
                m_joinsToVisit.append(source);
        }
    }
}

bool CalleeSummarizerForLinkEffects::everyCalleeReturnsFreshResult(FunctionID id, const ThinSummary& summary, uint32_t index) const
{
    const SummaryCall& call = summary.calls[index];
    const CodeEntered& code = codeOfCall(id, index);
    if (code.mayBeUnseen || code.mayBeLost)
        return false;
    std::span<const BuiltinID> builtins = builtinsEnteredBy(id, summary, index);
    bool everyBuiltinDoes = std::ranges::all_of(builtins, [&](BuiltinID builtin) {
        return rowsGiveFreshResult(builtin, call);
    });
    if (!everyBuiltinDoes)
        return false;
    if (call.callee == SummaryCall::Callee::Builtin)
        return true;
    if (builtins.empty() && !code.numberOfFunctions)
        return false;
    // The construct call of a plain function hands back the object that the entry made unless the function returns another.
    return std::ranges::all_of(functionsOf(code), [&](FunctionID callee) {
        if (isSharedCall(callee.index())) {
            const OperandsOfSharedCall& operands = m_operandsOfSharedCalls[callee.index() - nodeOfSharedCall(0)];
            return operands.everyResultIsFresh && !(call.isConstruction && operands.someCalleeTakesThisFromConstruction);
        }
        return m_tables.function(callee).resultIsFresh && !(call.isConstruction && m_program.at(callee).takesThisFromConstruction());
    });
}

bool CalleeSummarizerForLinkEffects::isFreshAndOnlyReturned(FunctionID id, const ThinSummary& summary, uint32_t node) const
{
    if (m_fates[node].contains(Fate::Leaves))
        return false;
    switch (summary.nodes[node].kind) {
    case SummaryNode::Kind::Allocation:
        // A module has one namespace object, whoever names it.
        return m_program.at(LayoutID(summary.nodes[node].first)).kind != LayoutKind::ModuleNamespace;
    case SummaryNode::Kind::ClassInstance:
        return true;
    case SummaryNode::Kind::ResultOfCall:
        return everyCalleeReturnsFreshResult(id, summary, summary.nodes[node].first);
    case SummaryNode::Kind::Unknown:
    case SummaryNode::Kind::Parameter:
    case SummaryNode::Kind::Result:
    case SummaryNode::Kind::Thrown:
    case SummaryNode::Kind::Capture:
    case SummaryNode::Kind::Variable:
    case SummaryNode::Kind::Field:
    case SummaryNode::Kind::Member:
    case SummaryNode::Kind::Contents:
    case SummaryNode::Kind::Constant:
    case SummaryNode::Kind::Closure:
    case SummaryNode::Kind::Builtin:
    case SummaryNode::Kind::Join:
    case SummaryNode::Kind::ResultOfAccess:
    case SummaryNode::Kind::ResultOfOperator:
    case SummaryNode::Kind::ResultOfConversion:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A member of the component that has not had its turn still stands at its defaults, which claim nothing. So each answer rests on answers that hold, in a cycle too.
void CalleeSummarizerForLinkEffects::summarizeValues(FunctionID id)
{
    const ThinSummary& summary = m_flow.summaries.at(id);
    if (keepsDefaults(id, summary, { SummaryTrait::Suspends, SummaryTrait::HasDirectEval, SummaryTrait::UsesArguments }))
        return;

    m_fates.fill({ }, summary.nodes.size());
    noteEdges(summary);
    noteOperandsOfCalls(id, summary);
    noteOperandsOfAccessesAndChanges(id, summary);
    carryFatesBackThroughJoins(summary);

    LinkedFunction& linked = m_tables.functions[id.index()];
    std::span<LinkedParameter> parameters = m_tables.parameters.mutableSpan().subspan(linked.firstParameter, linked.numberOfParameters);
    // The closure goes on holding what it captured.
    uint32_t numberOfCaptures = m_program.at(id).captures.size();
    for (uint32_t parameter = numberOfCaptures; parameter < parameters.size(); ++parameter)
        parameters[parameter].escapes = false;

    bool returnsSomething = false;
    bool everySourceIsFresh = true;
    for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
        const SummaryNode& node = summary.nodes[index];
        if (node.kind == SummaryNode::Kind::Parameter && node.first >= numberOfCaptures && node.first < parameters.size() && !m_fates[index].isEmpty())
            parameters[node.first].escapes = true;
        if (node.kind == SummaryNode::Kind::Join || !m_fates[index].contains(Fate::IsReturned))
            continue;
        returnsSomething = true;
        everySourceIsFresh = everySourceIsFresh && isFreshAndOnlyReturned(id, summary, index);
    }
    linked.resultIsFresh = returnsSomething && everySourceIsFresh;
}

void CalleeSummarizerForLinkEffects::findComponentsOfCallees()
{
    // One node more than there are functions stands for the lost functions together: whoever may enter one of them leads to it, and it leads to each.
    // So what they do is united once, behind all of them and in front of all who enter them.
    m_firstCallee.reserveInitialCapacity(numberOfNodesOfCallees() + 1);
    m_callees.reserveInitialCapacity(m_functionsEntered.size() + m_numberOfFunctions);
    for (uint32_t function = 0; function < m_numberOfFunctions; ++function) {
        m_firstCallee.append(m_callees.size());
        if (!isSummarized(function))
            continue;
        bool mayEnterLost = false;
        auto add = [&](const CodeEntered& code) {
            m_callees.append(functionsOf(code));
            mayEnterLost |= code.mayBeLost;
        };
        for (uint32_t call = m_flow.firstCallOfFunction[function]; call < m_flow.firstCallOfFunction[function + 1]; ++call)
            add(m_codeOfCalls[call]);
        for (uint32_t access = m_firstAccessOfFunction[function]; access < m_firstAccessOfFunction[function + 1]; ++access)
            add(m_codeOfAccesses[access]);
        if (mayEnterLost)
            m_callees.append(FunctionID(nodeOfLostFunctions()));
    }
    m_firstCallee.append(m_callees.size());
    // A call of a lost closure reaches every one of them, so one that is not reached is entered by nobody.
    for (size_t function : m_flow.lostFunctions) {
        if (m_flow.reached.get(function))
            m_callees.append(FunctionID(static_cast<uint32_t>(function)));
    }
    for (const LinkFlow::CalleesOfCall& shared : m_flow.calleesOfSharedCalls) {
        m_firstCallee.append(m_callees.size());
        m_callees.appendVector(shared.listed);
    }
    m_firstCallee.append(m_callees.size());
    m_effectsOfSharedCalls.fill(Effects { }, m_flow.calleesOfSharedCalls.size());
    m_operandsOfSharedCalls.fill(OperandsOfSharedCall { }, m_flow.calleesOfSharedCalls.size());

    m_components = findComponentsForLinkEffects(numberOfNodesOfCallees(), [&](uint32_t function) {
        return calleesOf(function);
    });
}

// The order is known, and a summary lies in a dozen places: those of the members that come next are on their way.
void CalleeSummarizerForLinkEffects::prefetchSummaryOfMemberBehind(size_t place) const
{
    static constexpr size_t distanceOfSummary = 8;
    static constexpr size_t distanceOfRecords = 4;
    auto memberAt = [&](size_t position) -> std::optional<uint32_t> {
        if (position >= m_components.members.size() || !isFunction(m_components.members[position]))
            return std::nullopt;
        return m_components.members[position];
    };
    if (std::optional<uint32_t> member = memberAt(place + distanceOfSummary)) {
        const char* summary = reinterpret_cast<const char*>(&m_flow.summaries.at(FunctionID(*member)));
        for (size_t offset = 0; offset < sizeof(ThinSummary); offset += 64)
            __builtin_prefetch(summary + offset);
        __builtin_prefetch(&m_flow.firstCallOfFunction[*member]);
        __builtin_prefetch(&m_firstAccessOfFunction[*member]);
        __builtin_prefetch(&m_tables.functions[*member]);
    }
    if (std::optional<uint32_t> member = memberAt(place + distanceOfRecords)) {
        auto prefetch = [](const auto& list) {
            const char* address = reinterpret_cast<const char*>(list.data());
            for (const char* end = address + list.size_bytes(); address < end; address += 64)
                __builtin_prefetch(address);
        };
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(*member));
        prefetch(summary.nodes.span());
        prefetch(summary.edges.span());
        prefetch(summary.calls.span());
        prefetch(summary.arguments.span());
        prefetch(summary.accesses.span());
        prefetch(m_codeOfCalls.span().subspan(m_flow.firstCallOfFunction[*member], m_flow.firstCallOfFunction[*member + 1] - m_flow.firstCallOfFunction[*member]));
        prefetch(m_codeOfAccesses.span().subspan(m_firstAccessOfFunction[*member], m_firstAccessOfFunction[*member + 1] - m_firstAccessOfFunction[*member]));
    }
}

void CalleeSummarizerForLinkEffects::summarizeEffects()
{
    const ComponentsOfLinkEffects& components = m_components;
    uint32_t componentOfLostFunctions = components.componentOfFunction[nodeOfLostFunctions()];
    for (uint32_t component = 0; component < components.size(); ++component) {
        std::span<const uint32_t> members = components.membersOf(component);
        // A function that is not summarized was given no callees above, so it is alone in its component.
        if (isFunction(members.front()) && !isSummarized(members.front()))
            continue;

        Effects effects = Effects::none();
        for (size_t place = components.firstMember[component]; place < components.firstMember[component + 1]; ++place) {
            prefetchSummaryOfMemberBehind(place);
            uint32_t member = components.members[place];
            if (isFunction(member)) {
                effects.merge(effectsOfBodyAndOfCodeOutside(member, components));
                continue;
            }
            for (FunctionID lost : calleesOf(member)) {
                if (components.componentOfFunction[lost.index()] == component)
                    continue;
                effects.merge(m_tables.function(lost).effectsOnReturn);
                effects.merge(m_tables.function(lost).effectsOnThrow);
            }
        }
        bool mayEnterItself = members.size() > 1 || std::ranges::any_of(calleesOf(members.front()), [&](FunctionID callee) {
            return callee.index() == members.front();
        });
        // No caller's check of the stack covers recursive calls: the overflow is an error like any other.
        effects.mayThrow |= mayEnterItself ? ThrowEffect(true) : ThrowEffect::onlyStackOverflow();
        // The check of the stack at frame setup is a poll too, and the collector may stop the thread there.
        effects.mayCollect = true;
        // A call never suspends its caller. The sweep and the emitters take an operation that may suspend for a suspension of its own frame.
        effects.maySuspend = false;

        if (component == componentOfLostFunctions)
            m_effectsOfLostFunctions = effects;
        for (uint32_t member : members) {
            if (isSharedCall(member))
                m_effectsOfSharedCalls[member - nodeOfSharedCall(0)] = effects;
            if (!isFunction(member))
                continue;
            LinkedFunction& linked = m_tables.functions[member];
            linked.effectsOnReturn = effects;
            linked.effectsOnThrow = effects;
        }
    }
}

void CalleeSummarizerForLinkEffects::summarizeValues()
{
    for (size_t place = 0; place < m_components.members.size(); ++place) {
        prefetchSummaryOfMemberBehind(place);
        uint32_t member = m_components.members[place];
        if (isSharedCall(member))
            summarizeOperandsOfSharedCall(member - nodeOfSharedCall(0));
        else if (isFunction(member) && isSummarized(member))
            summarizeValues(FunctionID(member));
    }
}

// Behind all that it lists, but for those that lead back to it, of which nothing is known yet.
void CalleeSummarizerForLinkEffects::summarizeOperandsOfSharedCall(uint32_t shared)
{
    OperandsOfSharedCall operands;
    operands.everyCalleeIsSummarized = true;
    operands.thisValueEscapes = false;
    operands.calleeEscapes = false;
    operands.newTargetEscapes = false;
    operands.everyResultIsFresh = true;
    operands.someCalleeTakesThisFromConstruction = false;
    bool isFirst = true;
    for (FunctionID callee : m_flow.calleesOfSharedCalls[shared].listed) {
        if (!isSummarized(callee.index())) {
            m_operandsOfSharedCalls[shared] = OperandsOfSharedCall { };
            return;
        }
        const EntryPositions& positions = m_entryPositions[callee.index()];
        operands.thisValueEscapes |= escapesIn(callee, positions.thisValue);
        operands.calleeEscapes |= escapesIn(callee, positions.callee);
        operands.newTargetEscapes |= escapesIn(callee, positions.newTarget);
        operands.everyResultIsFresh &= m_tables.function(callee).resultIsFresh;
        operands.someCalleeTakesThisFromConstruction |= m_program.at(callee).takesThisFromConstruction();
        // An argument behind the last positional parameter goes into a list, out of which anybody may read it.
        if (std::exchange(isFirst, false))
            operands.argumentEscapes.fill(false, positions.numberOfPositional);
        else if (positions.numberOfPositional < operands.argumentEscapes.size())
            operands.argumentEscapes.shrink(positions.numberOfPositional);
        for (uint32_t argument = 0; argument < operands.argumentEscapes.size(); ++argument)
            operands.argumentEscapes[argument] |= escapesIn(callee, positions.firstPositional + argument);
    }
    m_operandsOfSharedCalls[shared] = WTF::move(operands);
}

// Over the graph of LinkTables::referrers, groups and all. It leaves the cycle and the rank of each node, and the order of cycles.
void rankReferrersFirst(LinkTables& tables)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(tables.functions.size());
    // Without referrers every function stays in the one cycle of the defaults, where nothing is ordered.
    uint32_t numberOfNodes = tables.numberOfNodesOfReferrers();
    if (tables.referrers.isEmpty() || tables.firstReferrer.size() != numberOfNodes + 1)
        return;

    // A node leads to its referrers, so their cycles are closed first and have the lower numbers.
    ComponentsOfLinkEffects cycles = findComponentsForLinkEffects(numberOfNodes, [&](uint32_t node) {
        return tables.referrersOf(FunctionID(node));
    });

    constexpr uint32_t none = UINT32_MAX;
    Vector<uint32_t>& rankOfCycle = tables.rankOfCycle;
    Vector<uint32_t>& cyclesAwaited = tables.cyclesAwaited;
    rankOfCycle.fill(0, cycles.size());
    cyclesAwaited.shrink(0);
    tables.firstCycleAwaited.shrink(0);
    tables.firstCycleAwaited.reserveCapacity(cycles.size() + 1);
    // By cycle: the last that was found to wait for it.
    Vector<uint32_t> lastToAwait;
    lastToAwait.fill(none, cycles.size());

    for (uint32_t cycle = 0; cycle < cycles.size(); ++cycle) {
        std::span<const uint32_t> members = cycles.membersOf(cycle);
        size_t firstAwaited = cyclesAwaited.size();
        tables.firstCycleAwaited.append(static_cast<uint32_t>(firstAwaited));
        // One more than the highest rank among the cycles that name it. Cycles of one rank never name one another.
        uint32_t rank = 0;
        bool namesItself = members.size() > 1;
        for (uint32_t member : members) {
            for (FunctionID referrer : tables.referrersOf(FunctionID(member))) {
                uint32_t cycleOfReferrer = cycles.componentOfFunction[referrer.index()];
                if (cycleOfReferrer == cycle) {
                    namesItself = true;
                    continue;
                }
                if (std::exchange(lastToAwait[cycleOfReferrer], cycle) == cycle)
                    continue;
                rank = std::max(rank, rankOfCycle[cycleOfReferrer] + 1);
                cyclesAwaited.append(cycleOfReferrer);
            }
        }
        std::ranges::sort(cyclesAwaited.mutableSpan().subspan(firstAwaited));
        rankOfCycle[cycle] = rank;

        for (uint32_t member : members) {
            if (member >= numberOfFunctions) {
                LinkedGroup& group = tables.groups[member - numberOfFunctions];
                group.cycle = cycle;
                group.rank = rank;
                continue;
            }
            LinkedFunction& function = tables.functions[member];
            function.cycle = cycle;
            function.rank = rank;
            // Where several are tied together each of them has a referrer among the others.
            function.isNamedInItsCycle = namesItself;
        }
    }
    tables.firstCycleAwaited.append(static_cast<uint32_t>(cyclesAwaited.size()));
    tables.listCyclesNamed();

    for (LinkedFunction& function : tables.functions)
        function.namesOnlyWhatEveryFunctionLeadsTo = true;
    for (uint32_t node = 0; node < numberOfNodes; ++node) {
        if (node < numberOfFunctions ? tables.functions[node].isInQueueFromStart : tables.groups[node - numberOfFunctions].kind == LinkedGroup::Kind::Anybody)
            continue;
        for (FunctionID referrer : tables.referrersOf(FunctionID(node))) {
            if (referrer.index() < numberOfFunctions && referrer.index() != node)
                tables.functions[referrer.index()].namesOnlyWhatEveryFunctionLeadsTo = false;
        }
    }
}

// ---- The order in which a program starts

// The loader evaluates what a module imports in the order of Module::requests and then enters its body, on one thread: that order is followed here from each entry.
// The clock ticks at each operation of such a body that runs whenever the body gets that far: in a block that every way to a Return passes, outside of loops. What stands in another block runs, if at all,
// behind the last such block above it. A function is stamped with the earliest tick at which anything may enter it, a variable with the tick of the store that ends its dead zone.
// Code that nobody knows runs only while an operation has entered it. It enters nothing before something that the program made was handed to it, and then only what somebody who has run has named.
// What throws its way out of a followed body ends the start, since no handler stands around the loader's list. Behind that the host calls what was handed to it, which is stamped.
// JavaScript that nobody has seen may then call whatever it finds, so where such code can run it counts as running at every tick.
// Unseen code may also load every other entry by its name, and every file that is found by its name (ModuleFlags::IsFoundByName). From its first tick on, what those lead to counts as evaluated and entered, but for what the start has been to itself.
// No start walks through the other entries: a thousand entries are a thousand short walks. At first they count as leading to everything. Where that leaves a variable open that nothing else stands against,
// what they lead to is found once for the program (findWhatEntriesLeadTo()), which is at least what a walk through them at that tick would come to, and the starts are walked again.
bool CalleeSummarizerForLinkEffects::isFollowedAtStart(ModuleID id) const
{
    const Module& module = m_program.at(id);
    if (module.kind != ModuleKind::SourceText || !module.body || !m_flow.reached.get(module.body.index()))
        return false;
    if (module.flags.containsAny({ ModuleFlags::HasTopLevelAwait, ModuleFlags::RunsAgain, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked }))
        return false;
    const Function& body = m_program.at(module.body);
    return body.isModuleBody() && body.module == id && !body.isCoroutine() && std::ranges::none_of(module.requests, &ModuleRequest::isHoisted);
}

ModuleID CalleeSummarizerForLinkEffects::moduleOfVariableOrName(size_t number) const
{
    if (number < m_program.variables.size())
        return m_program.variables[number].module;
    if (number >= firstNameOnFunction()) {
        const LinkedNameOnFunction& name = m_tables.namesOnFunctions[number - firstNameOnFunction()];
        return name.function ? m_program.at(name.holder).module : ModuleID();
    }
    if (size_t first = m_program.variables.size() + m_tables.namesOnPrototypes.size(); number >= first) {
        LayoutID layout = m_layoutsOfConstructorFunctions[(number - first) / 2];
        bool isAsked = (number - first) % 2 ? !!m_program.at(layout).base : constructorFunctionOf(layout).prototypePropertyIsStoredOnceWithObjectMadeForIt;
        return isAsked ? m_program.at(m_program.at(layout).constructor).module : ModuleID();
    }
    const LinkedNameOnPrototype& name = m_tables.namesOnPrototypes[number - m_program.variables.size()];
    FunctionID constructor = m_program.at(name.layout).constructor;
    return name.key && name.function && constructor && m_flow.isOfConstructorFunction(name.layout) ? m_program.at(constructor).module : ModuleID();
}

void CalleeSummarizerForLinkEffects::forEachNameReadByEntering(uint32_t function, const Invocable<void(uint32_t)> auto& functor) const
{
    size_t numberOfLayoutsLeft = m_program.layouts.size();
    for (LayoutID layout = m_flow.layoutOfConstructorFunction(FunctionID(function)); layout && m_flow.isOfConstructorFunction(layout) && numberOfLayoutsLeft--; layout = m_program.at(layout).base) {
        auto names = std::ranges::equal_range(m_tables.namesOnPrototypes, layout, { }, &LinkedNameOnPrototype::layout);
        for (const LinkedNameOnPrototype& name : names)
            functor(static_cast<uint32_t>(m_program.variables.size() + (&name - m_tables.namesOnPrototypes.begin())));
        if (std::optional<uint32_t> stored = numberOf(SaidOfPrototypeObject::IsStored, layout)) {
            functor(*stored);
            functor(*stored + 1);
        }
    }
}

std::optional<uint32_t> CalleeSummarizerForLinkEffects::numberOf(SaidOfPrototypeObject said, LayoutID layout) const
{
    auto found = std::ranges::lower_bound(m_layoutsOfConstructorFunctions, layout);
    if (found == m_layoutsOfConstructorFunctions.end() || *found != layout)
        return std::nullopt;
    return static_cast<uint32_t>(m_program.variables.size() + m_tables.namesOnPrototypes.size() + 2 * (found - m_layoutsOfConstructorFunctions.begin()) + (said == SaidOfPrototypeObject::IsLinked));
}

bool CalleeSummarizerForLinkEffects::prototypeObjectIsSettledAtStart(const StartOfProgram& start, LayoutID layout) const
{
    const LinkedFunction& function = constructorFunctionOf(layout);
    return function.prototypePropertyStaysAsMade || (function.prototypePropertyIsStoredOnceWithObjectMadeForIt && start.endOfDeadZone[*numberOf(SaidOfPrototypeObject::IsStored, layout)] != StartOfProgram::never);
}

LinkType CalleeSummarizerForLinkEffects::typeOfPrototypeObjectOf(LayoutID layout) const
{
    LinkType type;
    type.objects = LinkType::Objects::Prototype;
    type.layout = layout;
    return type;
}

// `Below.prototype = Object.create(Base.prototype)`: until that store has run, `prototype` holds the object that the function was made with. Whoever reads it by then may keep that object, which the types do not tell from the other.
// EVERY READ COUNTS, whatever operation makes it: `class Round extends Shape` reads Shape.prototype and lays Round.prototype below what it finds. But for the scan's own, which stands for the object that is stored.
void CalleeSummarizerForLinkEffects::lookAtPrototypePropertyAtStart(StartOfProgram& start, FunctionID body, const SummaryAccess& access, bool endsDeadZones) const
{
    if (!m_someFunctionIsGivenItsPrototypeObject || access.isToContents())
        return;
    PropertyKeyID key = m_flow.keyOf(body, access);
    if (key && m_program.nameClassOf(key) != NameClass::Prototype)
        return;
    LinkType subject = typeAt(body, access.subject);
    if (subject.functions != LinkType::Functions::Closure)
        return;
    LayoutID layout = m_flow.layoutOfConstructorFunction(subject.function);
    if (!layout || !constructorFunctionOf(layout).prototypePropertyIsStoredOnceWithObjectMadeForIt)
        return;
    uint32_t number = *numberOf(SaidOfPrototypeObject::IsStored, layout);
    if (start.endOfDeadZone[number] != StartOfProgram::never)
        return;
    if (access.kind == SummaryAccess::Kind::Store && access.storesObjectMadeForIt) {
        LinkType closures;
        closures.functions = LinkType::Functions::Closure;
        closures.function = subject.function;
        if (endsDeadZones && key && subject == closures && moduleOfVariableOrName(number) == m_program.at(body).module) {
            start.endOfDeadZone[number] = start.clock;
            start.variablesInitialized.append(number);
        }
        return;
    }
    if (!access.standsForObjectMadeAtSite)
        start.isReadTooEarly.set(number);
}

// The prototype object of the layout, and nothing else, is laid below the settled prototype object of the base, and nothing else. `makesObjectHere`: it is the object that the one store is going to lay under `prototype`.
void CalleeSummarizerForLinkEffects::endDeadZoneOfLinkLaidBy(StartOfProgram& start, FunctionID body, const SummaryChange& change, bool makesObjectHere) const
{
    if (change.kind != SummaryChange::Kind::ChangesPrototype || change.other == noSummaryNode)
        return;
    LinkType subject = typeAt(body, change.subject);
    if (!subject.layout || !(subject == typeOfPrototypeObjectOf(subject.layout)) || !m_flow.isOfConstructorFunction(subject.layout))
        return;
    LayoutID base = m_program.at(subject.layout).base;
    if (!base || !m_flow.isOfConstructorFunction(base) || !(typeAt(body, change.other) == typeOfPrototypeObjectOf(base)) || !prototypeObjectIsSettledAtStart(start, base))
        return;
    if (!prototypeObjectIsSettledAtStart(start, subject.layout) && !(makesObjectHere && constructorFunctionOf(subject.layout).prototypePropertyIsStoredOnceWithObjectMadeForIt))
        return;
    uint32_t number = *numberOf(SaidOfPrototypeObject::IsLinked, subject.layout);
    if (moduleOfVariableOrName(number) != m_program.at(body).module || start.endOfDeadZone[number] != StartOfProgram::never)
        return;
    start.endOfDeadZone[number] = start.clock;
    start.variablesInitialized.append(number);
}

std::optional<uint32_t> CalleeSummarizerForLinkEffects::numberOfNameOnFunction(const LinkType& subject, PropertyKeyID key) const
{
    if (!key || subject.functions != LinkType::Functions::Closure || !m_keysOfNamesOnFunctions.get(key.index()))
        return std::nullopt;
    const LinkedNameOnFunction* name = m_tables.findNameOnFunction(subject.function, key);
    if (!name || !name->function)
        return std::nullopt;
    return static_cast<uint32_t>(firstNameOnFunction() + (name - m_tables.namesOnFunctions.begin()));
}

void CalleeSummarizerForLinkEffects::lookAtNameOnFunctionAtStart(StartOfProgram& start, FunctionID body, const LinkType& subject, PropertyKeyID key, bool isStore, bool endsDeadZones) const
{
    std::optional<uint32_t> number = numberOfNameOnFunction(subject, key);
    if (!number || start.endOfDeadZone[*number] != StartOfProgram::never)
        return;
    if (!isStore) {
        start.isReadTooEarly.set(*number);
        return;
    }
    LinkType closures;
    closures.functions = LinkType::Functions::Closure;
    closures.function = subject.function;
    if (!endsDeadZones || !(subject == closures) || moduleOfVariableOrName(*number) != m_program.at(body).module)
        return;
    start.endOfDeadZone[*number] = start.clock;
    start.variablesInitialized.append(*number);
}

void CalleeSummarizerForLinkEffects::forEachNameOnFunctionReadBy(FunctionID reader, const Invocable<void(uint32_t)> auto& functor) const
{
    if (m_keysOfNamesOnFunctions.isEmpty())
        return;
    const ThinSummary& summary = m_flow.summaries.at(reader);
    for (const SummaryAccess& access : summary.accesses) {
        if (access.isToContents() || !access.key || !m_keysOfNamesOnFunctions.get(access.key.index()))
            continue;
        if (std::optional<uint32_t> number = numberOfNameOnFunction(typeAt(reader, access.subject), access.key))
            functor(*number);
    }
    for (const SummaryCall& call : summary.calls) {
        if (call.callee != SummaryCall::Callee::Name || !call.key || call.receiver == noSummaryNode || !m_keysOfNamesOnFunctions.get(call.key.index()))
            continue;
        if (std::optional<uint32_t> number = numberOfNameOnFunction(typeAt(reader, call.receiver), call.key))
            functor(*number);
    }
}

void CalleeSummarizerForLinkEffects::forEachFunctionWhosePrototypeIsReadBy(FunctionID reader, const Invocable<void(uint32_t)> auto& functor) const
{
    if (!m_someFunctionIsGivenItsPrototypeObject)
        return;
    for (const SummaryAccess& access : m_flow.summaries.at(reader).accesses) {
        if (access.isToContents() || (access.key && m_program.nameClassOf(access.key) != NameClass::Prototype))
            continue;
        LinkType subject = typeAt(reader, access.subject);
        if (subject.functions != LinkType::Functions::Closure)
            continue;
        LayoutID layout = m_flow.layoutOfConstructorFunction(subject.function);
        if (layout && constructorFunctionOf(layout).prototypePropertyIsStoredOnceWithObjectMadeForIt)
            functor(*numberOf(SaidOfPrototypeObject::IsStored, layout));
    }
}

// The subject is the prototype object of that one function and nothing else, and the store stands in the body of the module of the function.
void CalleeSummarizerForLinkEffects::endDeadZoneOfNameLaidBy(StartOfProgram& start, FunctionID body, const SummaryAccess& access) const
{
    if (!access.laysValueInSightUnderName())
        return;
    PropertyKeyID key = m_flow.keyOf(body, access);
    LinkType subject = typeAt(body, access.subject);
    LinkType prototypeObject;
    prototypeObject.objects = LinkType::Objects::Prototype;
    prototypeObject.layout = subject.layout;
    if (!key || !subject.layout || !(subject == prototypeObject) || !m_flow.isOfConstructorFunction(subject.layout) || !prototypeObjectIsSettledAtStart(start, subject.layout))
        return;
    auto placeOf = [](const LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    };
    for (const LinkedNameOnPrototype& name : std::ranges::equal_range(m_tables.namesOnPrototypes, std::pair { subject.layout, key }, { }, placeOf)) {
        uint32_t number = static_cast<uint32_t>(m_program.variables.size() + (&name - m_tables.namesOnPrototypes.begin()));
        if (moduleOfVariableOrName(number) != m_program.at(body).module || start.endOfDeadZone[number] != StartOfProgram::never)
            continue;
        start.endOfDeadZone[number] = start.clock;
        start.variablesInitialized.append(number);
    }
}

bool CalleeSummarizerForLinkEffects::mayCallUnknownCodeAtStart(uint32_t function) const
{
    const LinkedFunction& linked = m_tables.functions[function];
    return !isSummarized(function) || linked.effectsOnReturn.mayCallUnknownCode || linked.effectsOnThrow.mayCallUnknownCode;
}

// What enterAtStart() and makeAvailableAtStart() go on to from a node while unseen code runs and holds objects.
void CalleeSummarizerForLinkEffects::forEachPlaceLedToBy(uint32_t node, const Invocable<void(uint32_t)> auto& toModule, const Invocable<void(uint32_t)> auto& toNode) const
{
    if (isFunction(node)) {
        for (FunctionID named : m_flow.summaries.at(FunctionID(node)).functionsNamed) {
            const Function& function = m_program.at(named);
            if (function.isModuleBody()) {
                if (function.module && named.index() != node)
                    toModule(function.module.index());
                continue;
            }
            bool isEnteredWithoutCall = function.kind == FunctionKind::Getter || function.kind == FunctionKind::Setter;
            if (m_flow.reached.get(named.index()) && (!m_tables.functions[named.index()].everyCallerIsKnown || isEnteredWithoutCall))
                toNode(named.index());
        }
    }
    for (FunctionID callee : calleesOf(node))
        toNode(callee.index());
}

void CalleeSummarizerForLinkEffects::findWhatEntriesLeadTo()
{
    uint32_t numberOfNodes = numberOfNodesOfCallees();
    uint32_t numberOfModules = static_cast<uint32_t>(m_program.modules.size());
    // A mark goes from nothing to one to several, so each place is left at most twice.
    Vector<uint32_t> worklist;
    auto arrive = [&](Vector<uint32_t>& marks, uint32_t place, uint32_t placeInWorklist, uint32_t mark) {
        uint32_t& held = marks[place];
        if (held == mark || held == LedTo::bySeveral)
            return;
        held = held == LedTo::byNothing ? mark : LedTo::bySeveral;
        worklist.append(placeInWorklist);
    };

    // In the worklist the modules stand behind the nodes.
    m_ledTo.entryOfModule.fill(LedTo::byNothing, numberOfModules);
    m_ledTo.entryOfNode.fill(LedTo::byNothing, numberOfNodes);
    for (const Entry& entry : m_program.entries)
        arrive(m_ledTo.entryOfModule, entry.module.index(), numberOfNodes + entry.module.index(), entry.module.index());
    // A file that is found by its name answers to it as a further entry does.
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (m_program.at(ModuleID(index)).flags.contains(ModuleFlags::IsFoundByName))
            arrive(m_ledTo.entryOfModule, index, numberOfNodes + index, index);
    }
    while (!worklist.isEmpty()) {
        uint32_t place = worklist.takeLast();
        uint32_t mark = place < numberOfNodes ? m_ledTo.entryOfNode[place] : m_ledTo.entryOfModule[place - numberOfNodes];
        auto toModule = [&](uint32_t module) {
            arrive(m_ledTo.entryOfModule, module, numberOfNodes + module, mark);
        };
        auto toNode = [&](uint32_t node) {
            arrive(m_ledTo.entryOfNode, node, node, mark);
        };
        if (place < numberOfNodes) {
            forEachPlaceLedToBy(place, toModule, toNode);
            continue;
        }
        const Module& module = m_program.at(ModuleID(place - numberOfNodes));
        for (const ModuleRequest& request : module.requests)
            toModule(request.target.index());
        if (module.body && m_flow.reached.get(module.body.index()))
            toNode(module.body.index());
        // Whoever has loaded the module has its namespace object.
        for (const ModuleExport& exported : module.exports) {
            FunctionID born = exported.variable ? m_program.at(exported.variable).function : FunctionID();
            if (born && m_flow.reached.get(born.index()) && !m_tables.functions[born.index()].everyCallerIsKnown)
                toNode(born.index());
        }
    }

    m_ledTo.moduleOfNode.fill(LedTo::byNothing, numberOfNodes);
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        const Module& module = m_program.at(ModuleID(index));
        if (m_ledTo.entryOfModule[index] != LedTo::byNothing && module.body && m_flow.reached.get(module.body.index()))
            arrive(m_ledTo.moduleOfNode, module.body.index(), module.body.index(), index);
    }
    while (!worklist.isEmpty()) {
        uint32_t node = worklist.takeLast();
        uint32_t mark = m_ledTo.moduleOfNode[node];
        forEachPlaceLedToBy(node, [](uint32_t) { }, [&](uint32_t next) {
            arrive(m_ledTo.moduleOfNode, next, next, mark);
        });
    }

    m_ledTo.callersOfUnknownCodeOfEntry.fill(0, numberOfModules);
    size_t numberOfVariables = numberOfVariablesAndNames();
    m_ledTo.firstReader.fill(0, numberOfVariables + 1);
    auto forEachVariableRead = [&](const auto& functor) {
        for (uint32_t function = 0; function < m_numberOfFunctions; ++function) {
            if (m_ledTo.entryOfNode[function] == LedTo::byNothing)
                continue;
            forEachNameReadByEntering(function, [&](uint32_t name) {
                functor(function, name);
            });
            forEachFunctionWhosePrototypeIsReadBy(FunctionID(function), [&](uint32_t stored) {
                functor(function, stored);
            });
            forEachNameOnFunctionReadBy(FunctionID(function), [&](uint32_t name) {
                functor(function, name);
            });
            const Function& reader = m_program.at(FunctionID(function));
            for (const SummaryNode& node : m_flow.summaries.at(FunctionID(function)).nodes) {
                // Where the body of its own module is entered at a time that nobody knows, the module is not followed, and none of its variables counts.
                if (node.kind == SummaryNode::Kind::Variable && !(reader.isModuleBody() && reader.module == m_program.variables[node.first].module))
                    functor(function, node.first);
            }
        }
    };
    for (uint32_t function = 0; function < m_numberOfFunctions; ++function) {
        uint32_t entry = m_ledTo.entryOfNode[function];
        if (entry == LedTo::byNothing || !mayCallUnknownCodeAtStart(function))
            continue;
        ++m_ledTo.callersOfUnknownCode;
        if (entry != LedTo::bySeveral)
            ++m_ledTo.callersOfUnknownCodeOfEntry[entry];
    }
    forEachVariableRead([&](uint32_t, uint32_t variable) {
        ++m_ledTo.firstReader[variable + 1];
    });
    for (size_t variable = 0; variable < numberOfVariables; ++variable)
        m_ledTo.firstReader[variable + 1] += m_ledTo.firstReader[variable];
    m_ledTo.readers.fill(0, m_ledTo.firstReader.last());
    Vector<uint32_t> nextReader(m_ledTo.firstReader.span().first(numberOfVariables));
    forEachVariableRead([&](uint32_t function, uint32_t variable) {
        m_ledTo.readers[nextReader[variable]++] = function;
    });
}

void CalleeSummarizerForLinkEffects::findModulesThatMayWait()
{
    uint32_t numberOfModules = static_cast<uint32_t>(m_program.modules.size());
    m_modulesThatMayWait.ensureSize(numberOfModules);
    Vector<uint32_t> worklist;
    for (uint32_t index = 0; index < numberOfModules; ++index) {
        if (m_program.at(ModuleID(index)).flags.contains(ModuleFlags::HasTopLevelAwait)) {
            m_modulesThatMayWait.set(index);
            worklist.append(index);
        }
    }
    if (worklist.isEmpty())
        return;
    auto forEachModuleAwaited = [&](const auto& functor) {
        for (uint32_t index = 0; index < numberOfModules; ++index) {
            for (const ModuleRequest& request : m_program.at(ModuleID(index)).requests) {
                // `import defer * as space from "./slow.mjs"`: what suspends below a module that is deferred is evaluated at once, and the importer waits for it.
                if (request.kind == ModuleRequestKind::Import)
                    functor(index, request.target.index());
            }
        }
    };
    Vector<uint32_t> firstImporter;
    firstImporter.fill(0, numberOfModules + 1);
    forEachModuleAwaited([&](uint32_t, uint32_t target) {
        ++firstImporter[target + 1];
    });
    for (uint32_t index = 0; index < numberOfModules; ++index)
        firstImporter[index + 1] += firstImporter[index];
    Vector<uint32_t> importers;
    importers.fill(0, firstImporter.last());
    Vector<uint32_t> nextImporter(firstImporter.span().first(numberOfModules));
    forEachModuleAwaited([&](uint32_t importer, uint32_t target) {
        importers[nextImporter[target]++] = importer;
    });
    while (!worklist.isEmpty()) {
        uint32_t module = worklist.takeLast();
        for (uint32_t importer : importers.span().subspan(firstImporter[module], firstImporter[module + 1] - firstImporter[module])) {
            if (!m_modulesThatMayWait.set(importer))
                worklist.append(importer);
        }
    }
}

auto CalleeSummarizerForLinkEffects::stateAtStart(const StartOfProgram& start, ModuleID id) const -> StartOfProgram::StateOfModule
{
    StartOfProgram::StateOfModule state = start.modules[id.index()];
    if (state != StartOfProgram::StateOfModule::NotStarted || start.tickOfOtherEntries == StartOfProgram::never)
        return state;
    if (start.otherEntriesLeadToEverything)
        return StartOfProgram::StateOfModule::NotFollowed;
    return !m_ledTo.isEmpty() && otherEntriesLeadTo(start, m_ledTo.entryOfModule[id.index()]) ? StartOfProgram::StateOfModule::NotFollowed : state;
}

// At tickOfOtherEntries. A body that the start has evaluated itself was not entered then, nor was what that body alone leads to.
bool CalleeSummarizerForLinkEffects::isEnteredByOtherEntries(const StartOfProgram& start, uint32_t node) const
{
    if (start.tickOfOtherEntries == StartOfProgram::never)
        return false;
    if (start.otherEntriesLeadToEverything)
        return true;
    if (m_ledTo.isEmpty() || !otherEntriesLeadTo(start, m_ledTo.entryOfNode[node]))
        return false;
    uint32_t module = m_ledTo.moduleOfNode[node];
    if (module == LedTo::bySeveral || module == LedTo::byNothing)
        return true;
    return start.modules[module] == StartOfProgram::StateOfModule::NotStarted && otherEntriesLeadTo(start, m_ledTo.entryOfModule[module]);
}

// Whoever names a module may hold its namespace object, and has what that shows, whoever has run: `hub[key]`, `cond ? hub.f : other`. What is read there meets what nobody knows, and a settled type no longer names it.
// As escapeExportsOf() goes: an export may be the namespace object of another module, and a star may hand on what only the run tells.
void CalleeSummarizerForLinkEffects::makeFunctionsBornBehindNamespaceAvailableAtStart(StartOfProgram& start, ModuleID first) const
{
    Vector<ModuleID, 8> modules { first };
    auto add = [&](ModuleID id) {
        if (id && !modules.contains(id))
            modules.append(id);
    };
    for (size_t place = 0; place < modules.size(); ++place) {
        const Module& module = m_program.at(modules[place]);
        for (const ModuleExport& exported : module.exports) {
            add(exported.module);
            FunctionID born = exported.variable ? m_program.at(exported.variable).function : FunctionID();
            if (!born || !m_flow.reached.get(born.index()) || m_tables.functions[born.index()].everyCallerIsKnown || start.isAvailable.set(born.index()))
                continue;
            start.functionsMadeAvailable.append(born.index());
            if (start.lastTickOfUnseenCode == start.clock)
                enterAtStart(start, born.index());
            else
                start.availableToUnseenCode.append(born.index());
        }
        for (const ModuleRequest& request : module.requests) {
            if (request.isDynamicStarExport)
                add(request.target);
        }
    }
}

void CalleeSummarizerForLinkEffects::makeAvailableAtStart(StartOfProgram& start, FunctionID namer) const
{
    for (FunctionID named : m_flow.summaries.at(namer).functionsNamed) {
        const Function& function = m_program.at(named);
        // In front of the question below: a body that asks for the namespace object of its own module is available by then.
        if (function.isModuleBody() && function.module)
            makeFunctionsBornBehindNamespaceAvailableAtStart(start, function.module);
        if (start.isAvailable.set(named.index()))
            continue;
        start.functionsMadeAvailable.append(named.index());
        // A site evaluates the module at a time that nobody knows.
        if (function.isModuleBody()) {
            if (function.module && named != namer)
                evaluateSometimeFrom(start, function.module);
            continue;
        }
        // A read, a store or a copy enters an accessor, and no call is written there. The flow may know all such places of one that an operation lays (`{ get rate() { .. } }`, `static get rate() { .. }`),
        // but no list of callees has them: whoever looks the name up counts as entering code that nobody knows, so that is what the accessor is entered by.
        bool isEnteredWithoutCall = function.kind == FunctionKind::Getter || function.kind == FunctionKind::Setter;
        if (!m_flow.reached.get(named.index()) || (m_tables.functions[named.index()].everyCallerIsKnown && !isEnteredWithoutCall))
            continue;
        if (start.lastTickOfUnseenCode == start.clock)
            enterAtStart(start, named.index());
        else
            start.availableToUnseenCode.append(named.index());
    }
}

void CalleeSummarizerForLinkEffects::enterAtStart(StartOfProgram& start, uint32_t first) const
{
    // Whoever is entered again may enter unseen code again, which can have got hold of more since. The effects hold for all that the function enters in turn.
    if (isFunction(first) && mayCallUnknownCodeAtStart(first))
        letUnseenCodeRunAtStart(start, HandedToUnseenCode::WhatProgramMade);
    if (start.firstEntry[first] != StartOfProgram::never || isEnteredByOtherEntries(start, first))
        return;
    start.firstEntry[first] = start.clock;
    start.nodesEntered.append(first);
    bool isOutermost = start.worklist.isEmpty();
    start.worklist.append(first);
    if (!isOutermost)
        return;
    // The list is never empty while somebody below appends to it.
    for (size_t place = 0; place < start.worklist.size(); ++place) {
        uint32_t function = start.worklist[place];
        if (isFunction(function))
            makeAvailableAtStart(start, FunctionID(function));
        for (FunctionID callee : calleesOf(function))
            enterAtStart(start, callee.index());
    }
    start.worklist.shrink(0);
}

void CalleeSummarizerForLinkEffects::enterAtStart(StartOfProgram& start, const CodeEntered& code) const
{
    if (code.mayBeLost)
        enterAtStart(start, nodeOfLostFunctions());
    for (FunctionID entered : functionsOf(code))
        enterAtStart(start, entered.index());
}

// An object of the engine's classes may hold what the program made. A built-in brings it nothing new.
// `const f = given || (() => late)`: what has escaped is among what is unseen, and a settled type no longer names it (LinkFlow::settleTypes()). So what is unseen may be the closure itself, with no unseen code in between.
static bool mayBeMadeByProgramForLinkEffects(const LinkType& type)
{
    return type.holdsUnseen || type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None;
}

auto CalleeSummarizerForLinkEffects::handedOverBy(FunctionID id, const ThinSummary& summary, const SummaryCall& call) const -> HandedToUnseenCode
{
    auto isMadeByProgram = [&](uint32_t node) {
        return node != noSummaryNode && mayBeMadeByProgramForLinkEffects(typeAt(id, node));
    };
    bool handsOver = call.passesList || isMadeByProgram(call.subject) || isMadeByProgram(call.receiver) || isMadeByProgram(call.newTarget) || std::ranges::any_of(argumentsOf(summary, call), isMadeByProgram);
    return handsOver ? HandedToUnseenCode::WhatProgramMade : HandedToUnseenCode::Nothing;
}

auto CalleeSummarizerForLinkEffects::handedOverBy(FunctionID id, const SummaryAccess& access) const -> HandedToUnseenCode
{
    auto isMadeByProgram = [&](uint32_t node) {
        return node != noSummaryNode && mayBeMadeByProgramForLinkEffects(typeAt(id, node));
    };
    bool storesWhatProgramMade = access.kind != SummaryAccess::Kind::Read && access.kind != SummaryAccess::Kind::Has && isMadeByProgram(access.value);
    bool handsOver = isMadeByProgram(access.subject) || isMadeByProgram(access.receiver) || isMadeByProgram(access.keyNode) || storesWhatProgramMade;
    return handsOver ? HandedToUnseenCode::WhatProgramMade : HandedToUnseenCode::Nothing;
}

void CalleeSummarizerForLinkEffects::letUnseenCodeRunAtStart(StartOfProgram& start, HandedToUnseenCode handed) const
{
    // A further entry point answers to its name: `require(path)`, with a path that the build does not know, loads it here and now, while the modules that wait for this one are unfinished. It takes nothing of the program to do that.
    if (start.tickOfOtherEntries == StartOfProgram::never) {
        start.tickOfOtherEntries = start.clock;
        if (start.otherEntriesLeadToEverything || (!m_ledTo.isEmpty() && m_ledTo.callersOfUnknownCode > m_ledTo.callersOfUnknownCodeOfEntry[start.entry.index()]))
            start.unseenCodeHoldsObjects = true;
    }
    if (handed == HandedToUnseenCode::WhatProgramMade)
        start.unseenCodeHoldsObjects = true;
    if (!start.unseenCodeHoldsObjects || start.lastTickOfUnseenCode == start.clock)
        return;
    start.lastTickOfUnseenCode = start.clock;
    Vector<uint32_t> available = std::exchange(start.availableToUnseenCode, { });
    for (uint32_t function : available)
        enterAtStart(start, function);
}

void CalleeSummarizerForLinkEffects::evaluateSometimeFrom(StartOfProgram& start, ModuleID id) const
{
    if (stateAtStart(start, id) != StartOfProgram::StateOfModule::NotStarted)
        return;
    start.modules[id.index()] = StartOfProgram::StateOfModule::NotFollowed;
    start.modulesStarted.append(id.index());
    const Module& module = m_program.at(id);
    for (const ModuleRequest& request : module.requests)
        evaluateSometimeFrom(start, request.target);
    if (module.body && m_flow.reached.get(module.body.index()))
        enterAtStart(start, module.body.index());
    else
        letUnseenCodeRunAtStart(start, HandedToUnseenCode::Nothing);
}

void CalleeSummarizerForLinkEffects::evaluateAtStart(StartOfProgram& start, const BodyIndexTable& bodies, ModuleID id) const
{
    if (start.modules[id.index()] != StartOfProgram::StateOfModule::NotStarted)
        return;
    // Its body waits for a module that suspends, so the loop of events turns in front of it, and whatever does not wait overtakes it.
    bool waits = m_modulesThatMayWait.get(id.index());
    // Nobody says that unseen code has loaded it: the loader may evaluate it here and now.
    if (stateAtStart(start, id) != StartOfProgram::StateOfModule::NotStarted) {
        start.someModuleIsLeftOpen |= waits && start.otherEntriesLeadToEverything;
        return;
    }
    const Module& module = m_program.at(id);
    // Unseen code that finds the file by its name may have evaluated it, caught what it threw and gone on.
    if (module.flags.contains(ModuleFlags::IsFoundByName) && start.lastTickOfUnseenCode) {
        evaluateSometimeFrom(start, id);
        return;
    }
    // The loader of the run time loads such a graph in steps, and an imported CommonJS file runs in one of them: what it requires is evaluated ahead of what Module::requests lists in front of the file. No prefix is written for it.
    // Whatever leads to such a module is one itself, so the whole graph goes without an order.
    if (module.flags.contains(ModuleFlags::IsLoadedByLoader)) {
        evaluateSometimeFrom(start, id);
        return;
    }
    const BodyIndex* index = isFollowedAtStart(id) ? bodies.find(module.body) : nullptr;
    if (index && index->hasIrreducibleFlow())
        index = nullptr;

    // The loader evaluates what it imports here and now, whatever its own body is like.
    start.modules[id.index()] = StartOfProgram::StateOfModule::Followed;
    start.modulesStarted.append(id.index());
    for (const ModuleRequest& request : module.requests) {
        if (request.kind != ModuleRequestKind::Import)
            continue;
        if (request.phase == ImportPhase::Evaluation)
            evaluateAtStart(start, bodies, request.target);
        else
            evaluateSometimeFrom(start, request.target);
    }
    if (waits || !index) {
        start.modules[id.index()] = StartOfProgram::StateOfModule::NotFollowed;
        ++start.clock;
        if (waits || !module.body || !m_flow.reached.get(module.body.index()))
            letUnseenCodeRunAtStart(start, HandedToUnseenCode::Nothing);
        if (module.body && m_flow.reached.get(module.body.index()))
            enterAtStart(start, module.body.index());
        return;
    }
    // A site in what ran meanwhile may have evaluated it.
    if (start.firstEntry[module.body.index()] != StartOfProgram::never) {
        start.modules[id.index()] = StartOfProgram::StateOfModule::NotFollowed;
        return;
    }
    followBodyAtStart(start, bodies, *index, module.body);
}

void CalleeSummarizerForLinkEffects::countSitesThatEnter()
{
    m_numberOfSitesThatEnter.fill(0, m_numberOfFunctions);
    auto count = [&](const CodeEntered& code) {
        for (FunctionID entered : functionsOf(code)) {
            // A call that several share stands in the list as a node of its own. Whoever is entered through one is not asked about: everyCallHasOneCallee.
            if (!isFunction(entered.index()))
                continue;
            uint8_t& sites = m_numberOfSitesThatEnter[entered.index()];
            sites += sites < 2;
        }
    };
    for (const CodeEntered& code : m_codeOfCalls)
        count(code);
    for (const CodeEntered& code : m_codeOfAccesses)
        count(code);
}

const BodyIndex* CalleeSummarizerForLinkEffects::bodyFollowedInPlaceOf(const StartOfProgram& start, const BodyIndexTable& bodies, FunctionID caller, const SummaryCall& call, const CodeEntered& entered) const
{
    if (entered.numberOfFunctions != 1 || entered.mayBeLost || entered.mayBeUnseen || call.isConstruction || call.isMadeByProtocol || call.passesList || call.waitsFor != noSummaryNode)
        return nullptr;
    FunctionID callee = functionsOf(entered)[0];
    if (!isFunction(callee.index()))
        return nullptr;
    // A built-in that is handed the function may enter it any number of times.
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        if (call.function != callee)
            return nullptr;
        break;
    case SummaryCall::Callee::Value: {
        LinkType closures;
        closures.functions = LinkType::Functions::Closure;
        closures.function = callee;
        if (call.builtin || call.subject == noSummaryNode || !(typeAt(caller, call.subject) == closures))
            return nullptr;
        break;
    }
    case SummaryCall::Callee::Name:
    case SummaryCall::Callee::Builtin:
        return nullptr;
    }
    const Function& function = m_program.at(callee);
    if (function.isCoroutine() || (function.kind != FunctionKind::Plain && function.kind != FunctionKind::Arrow) || function.module != m_program.at(caller).module)
        return nullptr;
    if (!m_tables.functions[callee.index()].everyCallerIsKnown || !m_tables.functions[callee.index()].everyCallHasOneCallee || m_numberOfSitesThatEnter[callee.index()] != 1)
        return nullptr;
    if (start.firstEntry[callee.index()] != StartOfProgram::never || isEnteredByOtherEntries(start, callee.index()))
        return nullptr;
    const BodyIndex* index = bodies.find(callee);
    return index && !index->hasIrreducibleFlow() ? index : nullptr;
}

// `o.m(args)`: the operation that looks `m` up. The scan leaves one record of the two, at the call. Null: the call looks no name up in an operation of its own.
static ValueID lookupOfCallByNameForLinkEffects(const BodyIndex& index, const SummaryCall& call)
{
    if (call.callee != SummaryCall::Callee::Name || call.isMadeByProtocol)
        return { };
    BodyValue site = index.value(call.site);
    ValueID callee;
    site.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (site.operandUses(operandIndex).contains(OperandUse::Calls))
            callee = operand;
    });
    while (callee && inputOperationsThatHandOn.contains(index.opcodeOf(callee)))
        callee = index.value(callee).operand(0);
    return callee && index.opcodeOf(callee) == Opcode::PropGet ? callee : ValueID();
}

void CalleeSummarizerForLinkEffects::followBodyAtStart(StartOfProgram& start, const BodyIndexTable& bodies, const BodyIndex& index, FunctionID body) const
{
    const ThinSummary& summary = m_flow.summaries.at(body);
    ModuleID module = m_program.at(body).module;
    ++start.clock;
    start.firstEntry[body.index()] = start.clock;
    start.nodesEntered.append(body.index());
    start.bodiesFollowed.set(body.index());
    if (!start.isAvailable.set(body.index()))
        start.functionsMadeAvailable.append(body.index());
    makeAvailableAtStart(start, body);

    // Of these nobody says where in the body they are.
    bool unseenCodeRunsAnywhere = !isSummarized(body.index()) || keepsDefaults(body, summary, { SummaryTrait::Suspends, SummaryTrait::HasDirectEval }) || someInspectionMayRunCode(m_flow, m_tables, body) || handsObjectToCodeNobodyNames(body, summary);

    Vector<RecordAtSite> records;
    for (uint32_t call = 0; call < summary.calls.size(); ++call) {
        records.append(RecordAtSite { summary.calls[call].site.index(), RecordAtSite::Kind::Call, call });
        // The name is looked up in front of the arguments: what the lookup runs finds nothing that they lay. Such a call is never followed in place, so whoever is entered here too is only entered earlier.
        if (ValueID lookup = lookupOfCallByNameForLinkEffects(index, summary.calls[call]))
            records.append(RecordAtSite { lookup.index(), RecordAtSite::Kind::Call, call });
    }
    uint32_t callsOfConversion = static_cast<uint32_t>(m_builtins.keysReadBy(Protocol::ToPrimitive).size());
    for (const ThinSummary::FurtherConversion& further : summary.furtherConversions) {
        for (uint32_t call = further.firstCall; call < further.firstCall + callsOfConversion; ++call)
            records.append(RecordAtSite { further.site.index(), RecordAtSite::Kind::Call, call });
    }
    for (uint32_t access = 0; access < summary.accesses.size(); ++access)
        records.append(RecordAtSite { summary.accesses[access].site.index(), RecordAtSite::Kind::Access, access });
    for (uint32_t change = 0; change < summary.changes.size(); ++change)
        records.append(RecordAtSite { summary.changes[change].site.index(), RecordAtSite::Kind::Change, change });
    for (ValueID site : summary.sitesThatRunUnknownCode)
        records.append(RecordAtSite { site.index(), RecordAtSite::Kind::UnknownCode, 0 });
    std::ranges::stable_sort(records, { }, &RecordAtSite::site);

    auto isEntered = [&](BlockID block) {
        return index.isReachable(block) && !std::ranges::binary_search(summary.blocksNeverEntered, block);
    };
    auto run = [&](const BodyValue& value, bool endsDeadZones) {
        if (unseenCodeRunsAnywhere)
            letUnseenCodeRunAtStart(start, HandedToUnseenCode::WhatProgramMade);
        else if (m_tables.unseenCodeCanRun)
            letUnseenCodeRunAtStart(start, HandedToUnseenCode::Nothing);
        for (const RecordAtSite& record : std::ranges::equal_range(records, value.id().index(), { }, &RecordAtSite::site)) {
            switch (record.kind) {
            case RecordAtSite::Kind::Call:
                if (codeOfCall(body, record.index).mayBeUnseen || effectsOfCallItself(body, summary, record.index).mayCallUnknownCode)
                    letUnseenCodeRunAtStart(start, handedOverBy(body, summary, summary.calls[record.index]));
                if (const BodyIndex* inPlace = endsDeadZones ? bodyFollowedInPlaceOf(start, bodies, body, summary.calls[record.index], codeOfCall(body, record.index)) : nullptr)
                    followBodyAtStart(start, bodies, *inPlace, functionsOf(codeOfCall(body, record.index))[0]);
                else
                    enterAtStart(start, codeOfCall(body, record.index));
                if (const SummaryCall& call = summary.calls[record.index]; call.callee == SummaryCall::Callee::Name && call.receiver != noSummaryNode && call.key && m_keysOfNamesOnFunctions.get(call.key.index()))
                    lookAtNameOnFunctionAtStart(start, body, typeAt(body, call.receiver), call.key, false, false);
                break;
            case RecordAtSite::Kind::Access:
                if (codeOfAccess(body, record.index).mayBeUnseen)
                    letUnseenCodeRunAtStart(start, handedOverBy(body, summary.accesses[record.index]));
                enterAtStart(start, codeOfAccess(body, record.index));
                if (endsDeadZones)
                    endDeadZoneOfNameLaidBy(start, body, summary.accesses[record.index]);
                lookAtPrototypePropertyAtStart(start, body, summary.accesses[record.index], endsDeadZones);
                if (const SummaryAccess& access = summary.accesses[record.index]; !access.isToContents() && access.key && m_keysOfNamesOnFunctions.get(access.key.index()))
                    lookAtNameOnFunctionAtStart(start, body, typeAt(body, access.subject), access.key, access.laysValueInSightUnderName(), endsDeadZones);
                break;
            case RecordAtSite::Kind::Change:
                if (changeMayRunCode(body, summary.changes[record.index]))
                    letUnseenCodeRunAtStart(start, HandedToUnseenCode::WhatProgramMade);
                if (endsDeadZones)
                    endDeadZoneOfLinkLaidBy(start, body, summary.changes[record.index], std::ranges::any_of(std::ranges::equal_range(records, value.id().index(), { }, &RecordAtSite::site), [&](const RecordAtSite& other) {
                        return other.kind == RecordAtSite::Kind::Access && summary.accesses[other.index].standsForObjectMadeAtSite;
                    }));
                break;
            case RecordAtSite::Kind::UnknownCode:
                letUnseenCodeRunAtStart(start, HandedToUnseenCode::WhatProgramMade);
                break;
            }
        }
        if (value.opcode() == Opcode::GetModuleVariable) {
            VariableID variable = value.as<Opcode::GetModuleVariable>().variable();
            if (start.endOfDeadZone[variable.index()] == StartOfProgram::never)
                start.isReadTooEarly.set(variable.index());
        } else if (value.opcode() == Opcode::PutModuleVariable && endsDeadZones) {
            VariableID variable = value.as<Opcode::PutModuleVariable>().variable();
            if (m_program.at(variable).module == module && start.endOfDeadZone[variable.index()] == StartOfProgram::never) {
                start.endOfDeadZone[variable.index()] = start.clock;
                start.variablesInitialized.append(variable.index());
            }
        }
    };

    // The blocks that every way passes behind which the caller goes on, from the entry downward. `return f();` is such a way.
    // What is thrown out of a followed body ends the start: no handler stands round it.
    BlockID lowest;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        BodyTerminator end = index.terminator(block);
        if (!isEntered(block) || end.numberOfTargets() || end.kind() == TerminatorKind::Throw || end.kind() == TerminatorKind::Rethrow || end.kind() == TerminatorKind::Unreachable)
            continue;
        if (!lowest) {
            lowest = block;
            continue;
        }
        while (!index.dominates(lowest, block))
            lowest = index.immediateDominator(lowest);
    }
    Vector<BlockID, 16> passedByEveryWay;
    for (BlockID block = lowest ? lowest : entryBlock; block; block = index.immediateDominator(block)) {
        if (!index.loopOf(block))
            passedByEveryWay.append(block);
        if (block == entryBlock)
            break;
    }
    passedByEveryWay.reverse();
    if (passedByEveryWay.isEmpty() || passedByEveryWay[0] != entryBlock)
        passedByEveryWay.insert(0, entryBlock);

    for (size_t place = 0; place < passedByEveryWay.size(); ++place) {
        BlockID certain = passedByEveryWay[place];
        // What is thrown in a block with a handler leaves the rest of the block out and goes on in the body.
        bool endsDeadZones = !index.block(certain).hasHandler() && !index.loopOf(certain);
        for (BodyValue value : index.block(certain).values()) {
            ++start.clock;
            run(value, endsDeadZones);
        }
        ++start.clock;
        // What `certain` dominates and the next of them does not. In the order of dominators a block stands in front of all that it dominates, and those stand in a row:
        // so these are the stretch from `certain` to the next, and the stretch behind all that the next dominates. Each block of the body is looked at once.
        BlockID next = place + 1 < passedByEveryWay.size() ? passedByEveryWay[place + 1] : BlockID();
        uint32_t endOfAllBelow = index.lastDominatorNumberBelow(certain) + 1;
        std::pair<uint32_t, uint32_t> stretches[] {
            { index.dominatorNumber(certain) + 1, next ? index.dominatorNumber(next) : endOfAllBelow },
            { next ? index.lastDominatorNumberBelow(next) + 1 : endOfAllBelow, endOfAllBelow },
        };
        for (auto [begin, end] : stretches) {
            for (BlockID block : index.blocksInDominatorOrder().subspan(begin, end - begin)) {
                if (!isEntered(block))
                    continue;
                for (BodyValue value : index.block(block).values())
                    run(value, false);
            }
        }
    }
    ++start.clock;
}

void CalleeSummarizerForLinkEffects::walkStart(StartOfProgram& start, const BodyIndexTable& bodies, ModuleID entry, std::span<const uint32_t> firstVariableOfModule, std::span<const uint32_t> variablesOfModules) const
{
    start.entry = entry;
    // They are there from the link on, whoever has run.
    for (uint32_t function : m_functionsBornInSightOfUnseenCode) {
        start.isAvailable.set(function);
        start.functionsMadeAvailable.append(function);
        start.availableToUnseenCode.append(function);
    }
    evaluateAtStart(start, bodies, entry);

    for (uint32_t function : start.nodesEntered) {
        if (!isFunction(function) || start.bodiesFollowed.get(function))
            continue;
        uint32_t firstEntry = start.firstEntry[function];
        for (const SummaryNode& node : m_flow.summaries.at(FunctionID(function)).nodes) {
            if (node.kind == SummaryNode::Kind::Variable && firstEntry <= start.endOfDeadZone[node.first])
                start.isReadTooEarly.set(node.first);
        }
        forEachNameReadByEntering(function, [&](uint32_t name) {
            if (firstEntry <= start.endOfDeadZone[name])
                start.isReadTooEarly.set(name);
        });
        forEachFunctionWhosePrototypeIsReadBy(FunctionID(function), [&](uint32_t stored) {
            if (firstEntry <= start.endOfDeadZone[stored])
                start.isReadTooEarly.set(stored);
        });
        forEachNameOnFunctionReadBy(FunctionID(function), [&](uint32_t name) {
            if (firstEntry <= start.endOfDeadZone[name])
                start.isReadTooEarly.set(name);
        });
    }
    bool otherEntriesWereLoaded = start.tickOfOtherEntries != StartOfProgram::never && (start.otherEntriesLeadToEverything || !m_ledTo.isEmpty());
    if (otherEntriesWereLoaded) {
        ++start.numberOfStartsWithOtherEntries;
        for (uint32_t variable : start.variablesInitialized) {
            if (start.endOfDeadZone[variable] < start.tickOfOtherEntries || start.isReadTooEarly.get(variable))
                continue;
            if (start.otherEntriesLeadToEverything) {
                start.isLeftOpen.set(variable);
                continue;
            }
            for (uint32_t reader : m_ledTo.readers.span().subspan(m_ledTo.firstReader[variable], m_ledTo.firstReader[variable + 1] - m_ledTo.firstReader[variable])) {
                if (start.firstEntry[reader] == StartOfProgram::never && isEnteredByOtherEntries(start, reader)) {
                    start.isReadTooEarly.set(variable);
                    break;
                }
            }
        }
    }
    for (uint32_t module : start.modulesStarted) {
        bool moduleWasFollowed = start.modules[module] == StartOfProgram::StateOfModule::Followed;
        for (uint32_t variable : variablesOfModules.subspan(firstVariableOfModule[module], firstVariableOfModule[module + 1] - firstVariableOfModule[module])) {
            if (moduleWasFollowed && start.endOfDeadZone[variable] != StartOfProgram::never)
                start.isInitializedInSomeStart.set(variable);
            else
                start.isReadTooEarly.set(variable);
        }
        if (otherEntriesWereLoaded && (start.otherEntriesLeadToEverything || otherEntriesLeadTo(start, m_ledTo.entryOfModule[module])))
            ++start.startsThatCameFirst[module];
    }

    for (uint32_t module : start.modulesStarted)
        start.modules[module] = StartOfProgram::StateOfModule::NotStarted;
    for (uint32_t node : start.nodesEntered) {
        start.firstEntry[node] = StartOfProgram::never;
        if (isFunction(node))
            start.bodiesFollowed.clear(node);
    }
    for (uint32_t function : start.functionsMadeAvailable)
        start.isAvailable.clear(function);
    for (uint32_t variable : start.variablesInitialized)
        start.endOfDeadZone[variable] = StartOfProgram::never;
    start.modulesStarted.shrink(0);
    start.nodesEntered.shrink(0);
    start.functionsMadeAvailable.shrink(0);
    start.variablesInitialized.shrink(0);
    start.availableToUnseenCode.shrink(0);
    start.clock = 1;
    start.lastTickOfUnseenCode = 0;
    start.tickOfOtherEntries = StartOfProgram::never;
    start.unseenCodeHoldsObjects = false;
}

void CalleeSummarizerForLinkEffects::findVariablesInitializedAtEveryRead()
{
    const BodyIndexTable& bodies = bodiesLookedAt(m_flow.looks);
    for (size_t layout = 0; layout < m_program.layouts.size(); ++layout) {
        if (!m_flow.isOfConstructorFunction(LayoutID(layout)) || !m_program.layouts[layout].constructor)
            continue;
        m_layoutsOfConstructorFunctions.append(LayoutID(layout));
        m_someFunctionIsGivenItsPrototypeObject |= constructorFunctionOf(LayoutID(layout)).prototypePropertyIsStoredOnceWithObjectMadeForIt;
    }
    for (const LinkedNameOnFunction& name : m_tables.namesOnFunctions) {
        if (name.function)
            m_keysOfNamesOnFunctions.set(name.key.index());
    }
    for (size_t variable : m_flow.variablesReadByUnseenCode) {
        FunctionID born = m_program.variables[variable].function;
        if (born && m_flow.reached.get(born.index()) && !m_tables.functions[born.index()].everyCallerIsKnown)
            m_functionsBornInSightOfUnseenCode.append(born.index());
    }
    size_t numberOfVariables = numberOfVariablesAndNames();
    size_t numberOfModules = m_program.modules.size();

    // Two entries at one module are one start.
    Vector<ModuleID> entries;
    BitVector isEntry(numberOfModules);
    for (const Entry& entry : m_program.entries) {
        if (!isEntry.set(entry.module.index()))
            entries.append(entry.module);
    }
    // It has no start of its own: the host does not ask for it. In every start it is among what unseen code may load.
    bool unseenCodeMayLoadOtherFiles = entries.size() > 1;
    for (size_t index = 0; index < numberOfModules && !unseenCodeMayLoadOtherFiles; ++index)
        unseenCodeMayLoadOtherFiles = !isEntry.get(index) && m_program.modules[index].flags.contains(ModuleFlags::IsFoundByName);
    findModulesThatMayWait();

    Vector<uint32_t> firstVariableOfModule;
    firstVariableOfModule.fill(0, numberOfModules + 1);
    for (uint32_t variable = 0; variable < numberOfVariables; ++variable) {
        if (ModuleID module = moduleOfVariableOrName(variable))
            ++firstVariableOfModule[module.index() + 1];
    }
    for (size_t module = 0; module < numberOfModules; ++module)
        firstVariableOfModule[module + 1] += firstVariableOfModule[module];
    Vector<uint32_t> variablesOfModules;
    variablesOfModules.fill(0, firstVariableOfModule.last());
    Vector<uint32_t> nextVariableOfModule(firstVariableOfModule.span().first(numberOfModules));
    for (uint32_t variable = 0; variable < numberOfVariables; ++variable) {
        if (ModuleID module = moduleOfVariableOrName(variable))
            variablesOfModules[nextVariableOfModule[module.index()]++] = variable;
    }

    BitVector isInitializedInSomeStart;
    BitVector isReadTooEarly;
    BitVector isLeftOpen;
    bool someModuleIsLeftOpen = false;
    Vector<uint32_t> startsThatCameFirst;
    uint32_t numberOfStartsWithOtherEntries = 0;
    BitVector otherEntriesWereLoadedInStartOf;
    bool otherEntriesLeadToEverything = unseenCodeMayLoadOtherFiles;
    countSitesThatEnter();
    auto walkStarts = [&] {
        // Each entry has a start of its own, and a walk writes nothing but the tables that it is walked on: they go side by side, each thread on tables of its own.
        Vector<StartOfProgram> startsOfWorkers(m_flow.loops.width());
        Vector<uint8_t> otherEntriesWereLoaded;
        otherEntriesWereLoaded.fill(0, entries.size());
        forEachIndexInParallel(m_flow.loops, static_cast<unsigned>(entries.size()), [&](unsigned indexOfEntry, unsigned worker) {
            StartOfProgram& start = startsOfWorkers[worker];
            if (start.modules.isEmpty()) {
                start.otherEntriesLeadToEverything = otherEntriesLeadToEverything;
                start.modules.fill(StartOfProgram::StateOfModule::NotStarted, numberOfModules);
                start.firstEntry.fill(StartOfProgram::never, numberOfNodesOfCallees());
                start.bodiesFollowed.ensureSize(m_numberOfFunctions);
                start.isAvailable.ensureSize(m_numberOfFunctions);
                start.endOfDeadZone.fill(StartOfProgram::never, numberOfVariables);
                start.isInitializedInSomeStart.ensureSize(numberOfVariables);
                start.isReadTooEarly.ensureSize(numberOfVariables);
                start.isLeftOpen.ensureSize(numberOfVariables);
                start.startsThatCameFirst.fill(0, numberOfModules);
            }
            uint32_t startsBefore = start.numberOfStartsWithOtherEntries;
            walkStart(start, bodies, entries[indexOfEntry], firstVariableOfModule.span(), variablesOfModules.span());
            otherEntriesWereLoaded[indexOfEntry] = start.numberOfStartsWithOtherEntries != startsBefore;
        });

        isInitializedInSomeStart = BitVector(numberOfVariables);
        isReadTooEarly = BitVector(numberOfVariables);
        isLeftOpen = BitVector(numberOfVariables);
        someModuleIsLeftOpen = false;
        startsThatCameFirst.fill(0, numberOfModules);
        for (const StartOfProgram& start : startsOfWorkers) {
            if (start.modules.isEmpty())
                continue;
            isInitializedInSomeStart.merge(start.isInitializedInSomeStart);
            isReadTooEarly.merge(start.isReadTooEarly);
            isLeftOpen.merge(start.isLeftOpen);
            someModuleIsLeftOpen |= start.someModuleIsLeftOpen;
            for (size_t module = 0; module < numberOfModules; ++module)
                startsThatCameFirst[module] += start.startsThatCameFirst[module];
        }
        numberOfStartsWithOtherEntries = 0;
        otherEntriesWereLoadedInStartOf = BitVector(numberOfModules);
        for (size_t index = 0; index < entries.size(); ++index) {
            if (!otherEntriesWereLoaded[index])
                continue;
            ++numberOfStartsWithOtherEntries;
            otherEntriesWereLoadedInStartOf.set(entries[index].index());
        }
    };
    // A start in which the other entries were loaded, and which has not been to the module in front of them, has not followed it.
    auto everyStartCameFirst = [&](ModuleID module) {
        uint32_t startsOfOthers = numberOfStartsWithOtherEntries;
        if (!otherEntriesLeadToEverything && startsOfOthers) {
            uint32_t entry = m_ledTo.entryOfModule[module.index()];
            if (entry != LedTo::bySeveral && entry != LedTo::byNothing && otherEntriesWereLoadedInStartOf.get(entry))
                --startsOfOthers;
        }
        return startsThatCameFirst[module.index()] == startsOfOthers;
    };
    auto isInitializedAtEveryRead = [&](size_t variable) {
        return !isReadTooEarly.get(variable) && (variable >= m_program.variables.size() || m_tables.variables[variable].proven) && everyStartCameFirst(moduleOfVariableOrName(variable));
    };

    walkStarts();
    if (otherEntriesLeadToEverything) {
        // In one start alone: what that entry alone leads to is followed there, and no other start stands against it.
        bool hangsOnWhatTheyLeadTo = someModuleIsLeftOpen || numberOfStartsWithOtherEntries == 1;
        for (size_t variable : isLeftOpen)
            hangsOnWhatTheyLeadTo |= isInitializedAtEveryRead(variable);
        if (hangsOnWhatTheyLeadTo) {
            findWhatEntriesLeadTo();
            otherEntriesLeadToEverything = false;
            walkStarts();
        }
    }

    for (size_t variable : isInitializedInSomeStart) {
        if (!isInitializedAtEveryRead(variable))
            continue;
        if (variable >= firstNameOnFunction()) {
            m_typesFound.namesOnFunctionsLaidAheadOfEveryRead.append(static_cast<uint32_t>(variable - firstNameOnFunction()));
            continue;
        }
        if (size_t first = m_program.variables.size() + m_tables.namesOnPrototypes.size(); variable >= first) {
            ((variable - first) % 2 ? m_typesFound.linksLaidAheadOfEveryEntry : m_typesFound.prototypeObjectsStoredAheadOfEveryEntry).append(m_layoutsOfConstructorFunctions[(variable - first) / 2].index());
            continue;
        }
        if (variable >= m_program.variables.size()) {
            m_typesFound.namesLaidAheadOfEveryEntry.append(static_cast<uint32_t>(variable - m_program.variables.size()));
            continue;
        }
        // Of a variable the clock claims nothing: a hole in it would be the empty word read raw, and the check that it spared is a compare. In the body that stores, the state of the walk holds what was stored.
    }
}

void summarizeCallees(const LinkFlow& flow, LinkTables& tables)
{
    CalleeSummarizerForLinkEffects summarizer(flow, tables);
    summarizer.findComponentsOfCallees();
    // Neither reads what the other writes.
    runLanes(flow.loops, {
        [&] {
            summarizer.summarizeKnownEffects();
        },
        [&] {
            summarizer.summarizeValues();
        },
    });
    summarizer.writeKnownEffects();
    summarizer.findVariablesInitializedAtEveryRead();
    flow.typesFoundOfCallees = summarizer.takeTypesFound();
}

void writeTypesFoundOfCallees(const LinkFlow& flow, LinkTables& tables)
{
    TypeArena types(flow.program.types, ArenaKeeps::InProgram);
    for (const TypesFoundOfCallees::ResultOfCallOfSeveral& result : flow.typesFoundOfCallees.resultsOfCallsOfSeveral) {
        TypeID type = flow.toTypeID(types, result.type);
        if (type != TypeTable::anything)
            tables.known.callsOfSeveral[result.place].result = type;
    }
    // The clock says when. That nothing else comes to lie under the name, on the prototype object, in front of it on an object, or in the place of the prototype object, the tables say.
    auto placeOf = [](const LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    };
    for (size_t index = 0; index < tables.layouts.size() && index < flow.program.layouts.size(); ++index) {
        LinkedLayout& layout = tables.layouts[index];
        const Layout& record = flow.program.layouts[index];
        if (!record.has(LayoutFlags::IsConstructorFunction) || !record.constructor || layout.reachesUnseenCode || !layout.listsEveryMemberAbove)
            continue;
        if (layout.possibleChanges.containsAny({ LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor }))
            continue;
        if (std::ranges::binary_search(tables.namesOnPrototypes, std::pair { LayoutID(index), PropertyKeyID() }, { }, placeOf))
            continue;
        const LinkedFunction& function = tables.functions[record.constructor.index()];
        layout.prototypeObjectIsSettledAheadOfEveryObject = function.prototypePropertyStaysAsMade
            || (function.prototypePropertyIsStoredOnceWithObjectMadeForIt && std::ranges::binary_search(flow.typesFoundOfCallees.prototypeObjectsStoredAheadOfEveryEntry, static_cast<uint32_t>(index)));
        layout.isLinkedBelowBaseAheadOfEveryObject = layout.prototypeObjectIsSettledAheadOfEveryObject && std::ranges::binary_search(flow.typesFoundOfCallees.linksLaidAheadOfEveryEntry, static_cast<uint32_t>(index));
    }
    flow.typesFoundOfCallees = { };
}

} } } // namespace JSC::AOT::Typed

#include "AOTLinkKnownEffectsInlines.h"

#include "AOTLinkKnownRowsInlines.h"

#endif // ENABLE(AOT)
