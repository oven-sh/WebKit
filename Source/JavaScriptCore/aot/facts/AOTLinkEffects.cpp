/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkUnions.h"
#include "AOTOpcodeSets.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

struct ComponentsOfLinkEffects {
    uint32_t size() const { return firstMember.size() - 1; }
    std::span<const uint32_t> membersOf(uint32_t component) const LIFETIME_BOUND { return members.span().subspan(firstMember[component], firstMember[component + 1] - firstMember[component]); }

    Vector<uint32_t> componentOfFunction;
    Vector<uint32_t> firstMember;
    Vector<uint32_t> members;
};

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
            uint32_t nextSuccessor = path.last().nextSuccessor;
            uint32_t lowestOfFunction = lowest[function];
            bool hasEntered = false;
            while (nextSuccessor < successors.size()) {
                uint32_t successor = successors[nextSuccessor++].index();
                RELEASE_ASSERT(successor < numberOfFunctions);
                if (discovery[successor] == none) {
                    hasEntered = true;
                    path.last().nextSuccessor = nextSuccessor;
                    lowest[function] = lowestOfFunction;
                    enter(successor);
                    break;
                }
                if (components.componentOfFunction[successor] == none)
                    lowestOfFunction = std::min(lowestOfFunction, discovery[successor]);
            }
            if (hasEntered)
                continue;
            lowest[function] = lowestOfFunction;

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

static constexpr OperandUses usesThatRunCodeNobodyNamesInLinkEffects = usesThatStoreByKey | usesThatReachBeyondOperand | OperandUse::PassesToUserCode;

static bool mayBeObjectForLinkEffects(const LinkType& type)
{
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.hasUnknownObjects;
}

static bool mayBeOtherCallableForLinkEffects(const LinkType& type)
{
    return type.classes != LinkType::Classes::None || type.builtin || type.hasUnknownObjects;
}

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
    case BuiltinCallbackResultUse::Converted:
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

static bool isUnderComputedKeyForLinkEffects(const SummaryAccess& access)
{
    return !access.key && access.keyNode != noSummaryNode && access.subject != noSummaryNode && access.receiver == noSummaryNode && !access.isToContents();
}

class CalleeSummarizerForLinkEffects {
    WTF_MAKE_NONCOPYABLE(CalleeSummarizerForLinkEffects);
public:
    CalleeSummarizerForLinkEffects(const LinkFlow&, LinkTables&);

    void findComponentsOfCallees();

private:
    enum class Answer : uint8_t { NotAsked, No, Yes };
    enum class Belief : uint8_t { Nothing, WhatDoorsAsk, Everything };

    struct CodeFound {
        void add(FunctionID function) { functions.append(function); }

        Vector<FunctionID, 4> functions;
        bool mayBeLost { false };
        bool mayBeUnseen { false };
    };

    struct CodeEntered {
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

    bool isSummarized(uint32_t function) const { return m_isSummarized.get(function); }
    bool isAnyCodeByItsOwnText(FunctionID, const ThinSummary&) const;
    bool isEnteredByNobody(FunctionID) const;
    bool keepsDefaults(FunctionID, const ThinSummary&, OptionSet<SummaryTrait>) const;
    uint32_t nodeOfLostFunctions() const { return m_numberOfFunctions; }
    uint32_t nodeOfSharedCall(uint32_t shared) const { return m_numberOfFunctions + 1 + shared; }
    bool isFunction(uint32_t node) const { return node < m_numberOfFunctions; }
    bool isSharedCall(uint32_t node) const { return node > m_numberOfFunctions; }
    uint32_t numberOfNodesOfCallees() const { return m_numberOfFunctions + 1 + static_cast<uint32_t>(m_flow.calleesOfSharedCalls.size()); }

    LinkType typeAt(FunctionID function, uint32_t nodeInSummary) const { return typeAt(function, m_flow.summaries.at(function), nodeInSummary); }
    LinkType typeAt(FunctionID, const ThinSummary&, uint32_t nodeInSummary) const;
    Belief beliefAt(FunctionID, const ThinSummary&, uint32_t nodeInSummary) const;
    enum class IndexedKind : uint8_t { Unknown, TypedArray, String };
    IndexedKind indexedKindAt(FunctionID, const ThinSummary&, uint32_t nodeInSummary) const;
    static bool containsOnlyPrimitives(const LinkType&, uint16_t primitives);
    IndexedKind findIndexedAccess(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    bool mayHoldFunction(FlowNodeID field) const;
    bool mayFindFunctionInFieldUnder(const LinkType& subject, PropertyKeyID) const;
    static std::span<const uint32_t> argumentsOf(const ThinSummary& summary, const SummaryCall& call) { return summary.arguments.span().subspan(call.firstArgument, call.numberOfArguments); }
    std::span<const FunctionID> functionsOf(const CodeEntered& code) const LIFETIME_BOUND { return m_functionsEntered.span().subspan(code.firstFunction, code.numberOfFunctions); }
    std::span<const FunctionID> calleesOf(uint32_t function) const LIFETIME_BOUND { return m_callees.span().subspan(m_firstCallee[function], m_firstCallee[function + 1] - m_firstCallee[function]); }
    void prefetchSummariesBehind(std::span<const uint32_t> order, size_t place) const;
    const CodeEntered& codeOfCall(FunctionID function, uint32_t call) const LIFETIME_BOUND { return m_codeOfCalls[m_flow.firstCallOfFunction[function.index()] + call]; }
    const CodeEntered& codeOfAccess(FunctionID function, uint32_t access) const LIFETIME_BOUND { return m_codeOfAccesses[m_firstAccessOfFunction[function.index()] + access]; }

    void indexHolders();
    static Belief beliefInKindOfNode(SummaryNode::Kind);
    void findClassesOfVariables();
    bool holdsClassOfVariable(FlowNodeID) const;
    bool isSuperConstructorRead(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    bool isNewTargetPrototypeRead(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    bool ownsPrototype(FunctionID) const;
    bool ownPrototypes(const LinkType&) const;
    bool closuresAreAsMade(FunctionID) const;
    bool functionPrototypeNeverAnswers(PropertyKeyID) const;
    void lookOffFunction(FunctionID, PropertyKeyID, MethodKind wanted, bool callsWhatIsFound, CodeFound&);
    template<typename Functor> bool forEachFunctionOf(const LinkType&, const Functor&) const;
    template<typename Functor> void forEachStaticFieldFound(FunctionID, PropertyKeyID, const Functor&) const;
    void addEffectsOfStaticFields(const LinkType& subject, PropertyKeyID, SummaryAccess::Kind, Effects&) const;
    static LinkType asDoorsAsk(LinkType);
    void findBeliefInNodes();
    void findLayoutsThatMayFindAnything();
    void findNearestClassesAbove();
    bool mayFindAnything(const LinkType& subject) const;
    bool objectPrototypeNeverAnswers(PropertyKeyID);
    void addFunctionsAt(const LinkType& held, CodeFound&) const;
    void lookInLayout(LayoutID, bool hasOwnFields, PropertyKeyID, MethodKind wanted, bool callsWhatIsFound, CodeFound&) const;
    void findCodeBehindLookup(const LinkType& subject, PropertyKeyID, SummaryAccess::Kind, bool callsWhatIsFound, CodeFound&);
    Effects effectsOfLookupItself(SummaryAccess::Kind, PropertyKeyID) const;
    bool thenLookupCannotThrow(const LinkType& subject, PropertyKeyID) const;

    std::span<const BuiltinID> builtinsEnteredBy(FunctionID, const ThinSummary& summary LIFETIME_BOUND, uint32_t call) const LIFETIME_BOUND;
    template<typename Functor> bool forEachRowOf(BuiltinID, const SummaryCall&, const Functor&) const;
    bool rowSaysAllOfCallsOf(const BuiltinOverload&, BuiltinOperand callee) const;
    bool addEffectsOnOperand(const BuiltinOverload&, BuiltinOperand, const LinkType&, CodeFound&, Effects&) const;
    bool addEffectsOfRow(const BuiltinOverload&, FunctionID, const ThinSummary&, const SummaryCall&, CodeFound&, Effects&) const;
    bool passesNoIterable(FunctionID, const ThinSummary&, const SummaryCall&) const;
    bool addEffectsOfRows(BuiltinID, FunctionID, const ThinSummary&, const SummaryCall&, CodeFound&, Effects&) const;

    static CodeEntered keep(CodeFound&, Vector<FunctionID>& functionsEntered);
    void findCodeEnteredByCall(FunctionID, const ThinSummary&, uint32_t call, CodeFound&);
    void findCodeEntered();
    Effects effectsOfCallItself(FunctionID, const ThinSummary&, uint32_t call) const;
    bool handsObjectToCodeNobodyNames(FunctionID, const ThinSummary&) const;
    bool changeMayRunCode(FunctionID, const SummaryChange&) const;

public:
    void summarizeKnownEffects();
    void writeKnownEffects();

private:
    struct EffectsOfComponentInKnownEffects {
        Effects all { Effects::none() };
        LocationMasks writesOnReturn;
        uint64_t layoutsWritten { 0 };
    };
    enum class WayInKnownEffects : uint8_t { ToNoReturn, ToReturn, ToReturnUnderHandler };
    struct GivenArgumentsInKnownEffects {
        uint32_t parametersTakenAsExpected { 0 };
        uint32_t parametersCalled { 0 };
        Vector<uint32_t> soleObjectParameters;
    };

    static EffectsOfComponentInKnownEffects knownEffectsOfUnknownCode() { return { Effects::unknownCode(), LocationMasks::top(), objectsOfAnyLayout }; }
    uint64_t bitsOfLayoutsAt(const LinkType&);
    uint64_t layoutsWrittenByChange(FunctionID, const SummaryChange&);
    uint64_t layoutsChangedByRowsOf(FunctionID, const ThinSummary&, uint32_t call, const Effects& itself);
    void findLayoutsOfPlainObjects();
    std::span<const LayoutID> layoutsNamedBy(const LinkType& type LIFETIME_BOUND) const LIFETIME_BOUND;
    bool holdsOnlyPlainObjectsOfNamedLayouts(const LinkType&) const;
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
    void addEffectsOfOperatorsLeftApart(FunctionID, const ThinSummary&, EffectsOfComponentInKnownEffects&, GivenArgumentsInKnownEffects&) const;
    std::optional<uint16_t> expectedParameterPrimitives(FunctionID, uint32_t entryParameter) const;
    uint32_t findSoleObjectParameters(FunctionID, const ThinSummary&, uint32_t node, GivenArgumentsInKnownEffects&) const;
    bool readMayMakeClosureOfField(const LinkType& subject, PropertyKeyID) const;
    void addEffectsOfAccessItself(FunctionID, const ThinSummary&, const SummaryAccess&, const LinkType& subject, PropertyKeyID, WayInKnownEffects, EffectsOfComponentInKnownEffects&);
    static std::optional<uint32_t> entryParameterAt(const ThinSummary&, uint32_t node);
    std::optional<LinkType> typeExpectedOfParameter(FunctionID, uint32_t entryParameter) const;
    bool mergeKnownEffectsOfCodeFound(const CodeFound&, uint32_t component, WayInKnownEffects, EffectsOfComponentInKnownEffects&) const;
    void addFunctionsCalledUnder(LayoutID, PropertyKeyID, CodeFound&) const;
    bool judgeAccessGivenArguments(FunctionID, const ThinSummary&, const SummaryAccess&, PropertyKeyID, uint32_t component, WayInKnownEffects, EffectsOfComponentInKnownEffects&, GivenArgumentsInKnownEffects&);
    bool judgeCallGivenArguments(FunctionID, const ThinSummary&, const SummaryCall&, uint32_t component, WayInKnownEffects, EffectsOfComponentInKnownEffects&, GivenArgumentsInKnownEffects&);
    EffectsOfComponentInKnownEffects knownEffectsOfBody(uint32_t function);
    EffectsOfComponentInKnownEffects knownEffectsOfMember(uint32_t component, uint32_t member);
    static void finishKnownEffectsOfCall(EffectsOfComponentInKnownEffects&, bool mayEnterItself);

    Vector<uint64_t> m_layoutsWrittenByFunction;
    EffectsOfComponentInKnownEffects m_knownEffectsOfLostFunctions;
    Vector<EffectsOfComponentInKnownEffects> m_knownEffectsOfSharedCalls;
    Vector<uint64_t> m_bitsOfLayouts;
    Vector<uint64_t> m_bitsOfFamilies;
    BitVector m_variablesThatFirstReadMayFill;
    Vector<BitVector> m_nodesBelievedForLessThanEverything;
    Vector<BitVector> m_nodesBelievedForNothing;
    BitVector m_keysOfFieldsThatHoldOneClosure;
    BitVector m_keysOfFieldsThatMayHoldFunctions;
    BitVector m_layoutsOfPlainObjects;
    Vector<LinkedEffectsGivenArguments> m_effectsGivenArguments;
    Vector<Vector<Effects>> m_blocksOfDistinctEffects;

    struct HandOfSummarizer {
        Vector<LinkedEffectsGivenArguments> effectsGivenArguments;
        DistinctEffects effectsOfFunctions;
    };
    static thread_local HandOfSummarizer* s_hand;
    void prepareKnownEffects();
    void findComponentsThatExpectationsLeadTo();
    void rankComponents();
    void prefetchSummaryOfMemberBehindInRanks(size_t position) const;
    void summarizeKnownEffectsOf(uint32_t component, std::span<const uint32_t> members, size_t positionOfFirstMember);
    void closeKnownEffectsOf(uint32_t component, std::span<const uint32_t> members, EffectsOfComponentInKnownEffects&);
    Vector<uint64_t> m_componentsThatExpectationsLeadTo;
    Vector<uint32_t> m_firstComponentOfRank;
    Vector<uint32_t> m_componentsInRanks;
    Vector<uint32_t> m_firstMemberInRanks;
    Vector<uint32_t> m_membersInRanks;

    bool indexesAreAbsentAboveInstancesOf(BuiltinClassID) const;
    bool indexesAreAbsentAbovePrototype(BuiltinID) const;
    bool instancesAreAsClassMakesThem(const LinkType& instances, PristineAspectSet) const;
    bool surelyTakes(const BuiltinType& accepted, const LinkType&) const;
    bool rowSurelyTakesCall(const BuiltinOverload&, FunctionID, const ThinSummary&, const SummaryCall&) const;
    std::optional<bool> addEffectsOfRowsUpToOneThatSurelyTakes(BuiltinID canonical, FunctionID, const ThinSummary&, const SummaryCall&, CodeFound&, Effects&) const;
    bool copyThatDefinesMayRunCode(FunctionID, const SummaryChange&) const;

    const LinkFlow& m_flow;
    const Program& m_program;
    const BuiltinTable& m_builtins;
    LinkTables& m_tables;
    TypeArena m_types;
    TypingContext m_typing;
    uint32_t m_numberOfFunctions;

    BitVector m_isSummarized;
    bool m_stringsHaveNoOtherIndexes { false };
    BitVector m_variablesOfClasses;
    BitVector m_classesOfVariables;

    Vector<AccessorUnderKey> m_accessors;
    BitVector m_keysOfAccessorFields;
    BitVector m_keysOfOtherCallables;
    BitVector m_layoutsThatMayFindAnything;
    bool m_lostLayoutsMayFindAnything { false };
    BitVector m_familiesThatMayFindAnything;
    Vector<LayoutID> m_nearestAboveWithStaticFields;
    Vector<LayoutID> m_nearestAboveWithNamesOnPrototype;
    BitVector m_classObjectsAreAsMadeFrom;
    Vector<Answer> m_keysThatObjectPrototypeNeverAnswers;

    LargeList<CodeEntered> m_codeOfCalls;
    Vector<uint32_t> m_firstAccessOfFunction;
    LargeList<CodeEntered> m_codeOfAccesses;
    Vector<FunctionID> m_functionsEntered;
    Vector<uint32_t> m_firstCallee;
    Vector<FunctionID> m_callees;
    ComponentsOfLinkEffects m_components;

};

thread_local CalleeSummarizerForLinkEffects::HandOfSummarizer* CalleeSummarizerForLinkEffects::s_hand = nullptr;

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
    EffectsOfComponentInKnownEffects ofAnyCode = knownEffectsOfUnknownCode();
    finishKnownEffectsOfCall(ofAnyCode, true);
    DistinctEffects effectsOfAnyCode;
    const Effects* keptEffectsOfAnyCode = effectsOfAnyCode.keep(ofAnyCode.all);
    m_blocksOfDistinctEffects.appendVector(effectsOfAnyCode.takeBlocks());
    m_firstAccessOfFunction.fill(0, m_numberOfFunctions + 1);
    findClassesOfVariables();
    static_assert(!(indicesInRangeOfLoop % 64));
    forEachRangeOfIndicesInParallel(flow.loops, m_numberOfFunctions, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const Function& function = m_program.functions[index];
            if (!flow.reached.get(index) || !function.hasBody())
                continue;
            FunctionID id(static_cast<uint32_t>(index));
            const ThinSummary& summary = flow.summaries.at(id);
            RELEASE_ASSERT(flow.firstCallOfFunction[index + 1] - flow.firstCallOfFunction[index] == summary.calls.size());
            if (isAnyCodeByItsOwnText(id, summary)) {
                tables.functions[index].keptEffectsOnReturn = keptEffectsOfAnyCode;
                tables.functions[index].keptEffectsOnThrow = keptEffectsOfAnyCode;
                continue;
            }
            m_isSummarized.quickSet(index);
            m_firstAccessOfFunction[index + 1] = summary.accesses.size();
        }
    });

    m_stringsHaveNoOtherIndexes = indexesAreAbsentAbovePrototype(m_builtins.canonicalID(Builtins::StringPrototype));
    findBeliefInNodes();
    indexHolders();
    findLayoutsThatMayFindAnything();
    findNearestClassesAbove();
    findCodeEntered();
}

bool CalleeSummarizerForLinkEffects::keepsDefaults(FunctionID id, const ThinSummary& summary, OptionSet<SummaryTrait> traits) const
{
    const Function& function = m_program.at(id);
    bool isAsyncFunction = function.has(FunctionFlags::IsAsync) && !function.has(FunctionFlags::IsGenerator) && !function.isModuleBody();
    return (function.isCoroutine() && !isAsyncFunction) || summary.traits.containsAny(traits);
}

bool CalleeSummarizerForLinkEffects::isAnyCodeByItsOwnText(FunctionID id, const ThinSummary& summary) const
{
    if (keepsDefaults(id, summary, { SummaryTrait::Suspends, SummaryTrait::HasDirectEval }) || summary.ownEffectsMayCallUnknownCode())
        return true;
    return std::ranges::any_of(summary.calls, [&](const SummaryCall& call) {
        if (call.callee != SummaryCall::Callee::Value)
            return false;
        SummaryNode::Kind callee = summary.nodes[call.subject].kind;
        return callee != SummaryNode::Kind::Closure && callee != SummaryNode::Kind::Parameter && !holdsClassOfVariable(m_flow.nodeOf(id, call.subject)) && !m_flow.typeAt(m_flow.nodeOf(id, call.subject)).isNever();
    });
}

void CalleeSummarizerForLinkEffects::findClassesOfVariables()
{
    m_variablesOfClasses.ensureSize(m_program.variables.size());
    m_classesOfVariables.ensureSize(m_numberOfFunctions);
    constexpr uint16_t beforeDeclaration = Primitive::Undefined | Primitive::Absent;
    for (const Module& module : m_program.modules) {
        if (!module.body || !m_flow.reached.get(module.body.index()))
            continue;
        const ThinSummary& summary = m_flow.summaries.at(module.body);
        for (const SummaryEdge& edge : summary.edges) {
            if (summary.nodes[edge.to].kind != SummaryNode::Kind::Variable || summary.nodes[edge.from].kind != SummaryNode::Kind::Closure)
                continue;
            VariableID variable(summary.nodes[edge.to].first);
            FunctionID constructor(summary.nodes[edge.from].first);
            const LinkedCell& cell = m_tables.variables[variable.index()];
            if (!m_program.at(variable).flags.contains(VariableFlags::IsImmutable) || !cell.everyWriterIsKnown || cell.isWrittenAfterConstruction || !m_flow.classOfConstructor(constructor))
                continue;
            LinkType held = m_flow.typeAt(m_flow.variable(variable));
            if (held.functions != LinkType::Functions::Closure || held.function != constructor || held.objects != LinkType::Objects::None || mayBeOtherCallableForLinkEffects(held) || held.holdsLostBuiltins || (held.primitives & ~beforeDeclaration))
                continue;
            m_variablesOfClasses.quickSet(variable.index());
            m_classesOfVariables.quickSet(constructor.index());
        }
    }
}

bool CalleeSummarizerForLinkEffects::holdsClassOfVariable(FlowNodeID node) const
{
    LinkType held = m_flow.typeAt(node);
    return held.functions == LinkType::Functions::Closure && m_classesOfVariables.quickGet(held.function.index());
}

bool CalleeSummarizerForLinkEffects::isSuperConstructorRead(FunctionID id, const ThinSummary& summary, const SummaryAccess& access) const
{
    if (access.kind != SummaryAccess::Kind::Read || !access.key || access.subject == noSummaryNode || access.receiver != noSummaryNode || access.isToContents() || m_flow.nameClassOf(access.key) != NameClass::Proto)
        return false;
    const SummaryNode& subject = summary.nodes[access.subject];
    if (subject.kind != SummaryNode::Kind::Parameter || m_program.at(id).entryParameterKind(subject.first) != EntryParameter::Callee)
        return false;
    LayoutID layout = m_flow.classOfConstructor(id);
    LayoutID base = layout ? m_program.at(layout).base : LayoutID();
    return base && m_program.at(base).constructor && m_flow.classOfConstructor(m_program.at(base).constructor) == base;
}

bool CalleeSummarizerForLinkEffects::ownsPrototype(FunctionID id) const
{
    const Function& function = m_program.at(id);
    return isClassConstructorOf(function.kind) || (function.kind == FunctionKind::Plain && !function.isCoroutine());
}

template<typename Functor>
bool CalleeSummarizerForLinkEffects::forEachFunctionOf(const LinkType& type, const Functor& functor) const
{
    switch (type.functions) {
    case LinkType::Functions::None:
        return true;
    case LinkType::Functions::Lost:
        return false;
    case LinkType::Functions::Closure:
        functor(type.function);
        return true;
    case LinkType::Functions::Constructors:
        for (LayoutID member : membersOf(m_program, m_flow.families.layoutsOf(type.constructorsOf))) {
            if (FunctionID constructor = m_program.at(member).constructor)
                functor(constructor);
        }
        return true;
    case LinkType::Functions::Set:
        for (FunctionID member : m_flow.functionsOfSet(type))
            functor(member);
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CalleeSummarizerForLinkEffects::ownPrototypes(const LinkType& type) const
{
    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
    if (mayBeOtherCallableForLinkEffects(type) || type.holdsLostBuiltins || type.objects != LinkType::Objects::None || (type.primitives & ~primitivesWithoutMembers) || type.functions == LinkType::Functions::None)
        return false;
    bool ownThem = true;
    return forEachFunctionOf(type, [&](FunctionID function) {
        ownThem &= ownsPrototype(function);
    }) && ownThem;
}

bool CalleeSummarizerForLinkEffects::isNewTargetPrototypeRead(FunctionID id, const ThinSummary& summary, const SummaryAccess& access) const
{
    if (access.kind != SummaryAccess::Kind::Read || access.key != m_program.keyOf(BuiltinNames::prototype) || access.subject == noSummaryNode || access.receiver != noSummaryNode || access.isToContents())
        return false;
    const SummaryNode& subject = summary.nodes[access.subject];
    return subject.kind == SummaryNode::Kind::Parameter && m_program.at(id).entryParameterKind(subject.first) == EntryParameter::NewTarget
        && m_tables.function(id).everyCallerIsKnown && ownPrototypes(m_flow.typeAt(m_flow.nodeOf(id, access.subject)));
}

auto CalleeSummarizerForLinkEffects::beliefInKindOfNode(SummaryNode::Kind kind) -> Belief
{
    switch (kind) {
    case SummaryNode::Kind::Constant:
    case SummaryNode::Kind::Allocation:
    case SummaryNode::Kind::ClassInstance:
    case SummaryNode::Kind::Closure:
    case SummaryNode::Kind::Builtin:
    case SummaryNode::Kind::Join:
    case SummaryNode::Kind::ResultOfAccess:
    case SummaryNode::Kind::Parameter:
    case SummaryNode::Kind::Variable:
        return Belief::Everything;
    case SummaryNode::Kind::Capture:
    case SummaryNode::Kind::Field:
    case SummaryNode::Kind::ResultOfCall:
    case SummaryNode::Kind::ResultOfOperator:
    case SummaryNode::Kind::ResultOfConversion:
        return Belief::WhatDoorsAsk;
    case SummaryNode::Kind::Unknown:
    case SummaryNode::Kind::Result:
    case SummaryNode::Kind::Thrown:
    case SummaryNode::Kind::Member:
    case SummaryNode::Kind::Contents:
        return Belief::Nothing;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LinkType CalleeSummarizerForLinkEffects::asDoorsAsk(LinkType linked)
{
    if (linked.hasUnknownObjects || linked.holdsLostBuiltins || linked.builtin || linked.classes != LinkType::Classes::None)
        return LinkType::anything();
    if (linked.objects == LinkType::Objects::Prototype || linked.objects == LinkType::Objects::Prototypes || linked.objects == LinkType::Objects::Lost)
        return LinkType::anything();
    if (linked.functions == LinkType::Functions::None)
        return linked;
    if (linked.objects != LinkType::Objects::None)
        return LinkType::anything();
    linked.functions = LinkType::Functions::Lost;
    return linked;
}

void CalleeSummarizerForLinkEffects::findBeliefInNodes()
{
    m_nodesBelievedForLessThanEverything.grow(m_numberOfFunctions);
    m_nodesBelievedForNothing.grow(m_numberOfFunctions);
    forEachRangeOfIndicesInParallel(m_flow.loops, m_numberOfFunctions, [&](size_t first, size_t end) {
        Vector<uint32_t> endOfNodesLedToBy;
        Vector<uint32_t> nodesLedTo;
        Vector<uint32_t> toVisit;
        Vector<uint32_t> elementReads;
        for (size_t index = first; index < end; ++index) {
            if (!isSummarized(index))
                continue;
            FunctionID id(static_cast<uint32_t>(index));
            const ThinSummary& summary = m_flow.summaries.at(id);
            uint32_t numberOfNodes = summary.nodes.size();
            std::optional<unsigned> calleeParameter = m_program.at(id).calleeParameterIndex();
            auto kindOf = [&](uint32_t node) { return summary.nodes[node].kind; };
            auto forEachLink = [&](const auto& functor) {
                for (const SummaryEdge& edge : summary.edges) {
                    if (kindOf(edge.to) == SummaryNode::Kind::Join)
                        functor(edge.from, edge.to);
                }
                for (uint32_t node = 0; node < numberOfNodes; ++node) {
                    if (kindOf(node) == SummaryNode::Kind::ResultOfAccess && summary.accesses[summary.nodes[node].first].subject != noSummaryNode)
                        functor(summary.accesses[summary.nodes[node].first].subject, node);
                }
            };
            endOfNodesLedToBy.fill(0, numberOfNodes + 1);
            uint32_t numberOfLinks = 0;
            forEachLink([&](uint32_t from, uint32_t) {
                ++endOfNodesLedToBy[from + 1];
                ++numberOfLinks;
            });
            for (uint32_t node = 0; node < numberOfNodes; ++node)
                endOfNodesLedToBy[node + 1] += endOfNodesLedToBy[node];
            nodesLedTo.fill(0, numberOfLinks);
            forEachLink([&](uint32_t from, uint32_t to) {
                nodesLedTo[endOfNodesLedToBy[from]++] = to;
            });

            BitVector& less = m_nodesBelievedForLessThanEverything[index];
            BitVector& nothing = m_nodesBelievedForNothing[index];
            less.ensureSize(numberOfNodes);
            nothing.ensureSize(numberOfNodes);
            auto beliefIn = [&](uint32_t node) {
                if (!less.quickGet(node))
                    return beliefInKindOfNode(kindOf(node));
                return nothing.quickGet(node) ? Belief::Nothing : Belief::WhatDoorsAsk;
            };
            auto lower = [&](uint32_t node, Belief belief) {
                if (beliefIn(node) <= belief)
                    return;
                less.quickSet(node);
                if (belief == Belief::Nothing)
                    nothing.quickSet(node);
                toVisit.append(node);
            };
            toVisit.shrink(0);
            elementReads.shrink(0);
            for (uint32_t node = 0; node < numberOfNodes; ++node) {
                if (kindOf(node) == SummaryNode::Kind::ResultOfAccess) {
                    const SummaryAccess& access = summary.accesses[summary.nodes[node].first];
                    if (access.kind == SummaryAccess::Kind::Read && isUnderComputedKeyForLinkEffects(access)) {
                        elementReads.append(node);
                        continue;
                    }
                    if (calleeParameter && isSuperConstructorRead(id, summary, access))
                        continue;
                    bool isReadByName = access.kind == SummaryAccess::Kind::Read && !access.isToContents() && access.receiver == noSummaryNode && access.subject != noSummaryNode;
                    LinkType subject = isReadByName ? asDoorsAsk(m_flow.typeAt(m_flow.nodeOf(id, access.subject))) : LinkType::anything();
                    lower(node, subject.hasUnknownObjects || subject.functions != LinkType::Functions::None ? Belief::Nothing : Belief::WhatDoorsAsk);
                } else if (kindOf(node) == SummaryNode::Kind::Parameter || kindOf(node) == SummaryNode::Kind::Variable) {
                    bool holdsOneClass = kindOf(node) == SummaryNode::Kind::Parameter ? summary.nodes[node].first == calleeParameter : m_variablesOfClasses.quickGet(summary.nodes[node].first);
                    if (!holdsOneClass)
                        lower(node, Belief::WhatDoorsAsk);
                } else if (beliefInKindOfNode(kindOf(node)) != Belief::Everything)
                    toVisit.append(node);
            }
            auto visitAll = [&] {
                while (!toVisit.isEmpty()) {
                    uint32_t node = toVisit.takeLast();
                    Belief belief = beliefIn(node);
                    for (uint32_t place = node ? endOfNodesLedToBy[node - 1] : 0; place < endOfNodesLedToBy[node]; ++place) {
                        uint32_t ledTo = nodesLedTo[place];
                        if (kindOf(ledTo) == SummaryNode::Kind::Join || belief == Belief::Nothing)
                            lower(ledTo, belief);
                    }
                }
            };
            visitAll();
            for (bool hasChanged = !elementReads.isEmpty(); hasChanged;) {
                hasChanged = elementReads.removeAllMatching([&](uint32_t read) {
                    if (findIndexedAccess(id, summary, summary.accesses[summary.nodes[read].first]) != IndexedKind::Unknown)
                        return false;
                    lower(read, Belief::Nothing);
                    return true;
                });
                visitAll();
            }
        }
    });
}

bool CalleeSummarizerForLinkEffects::mayHoldFunction(FlowNodeID field) const
{
    LinkType held = asDoorsAsk(m_flow.typeAt(field));
    return held.hasUnknownObjects || held.functions != LinkType::Functions::None;
}

bool CalleeSummarizerForLinkEffects::mayFindFunctionInFieldUnder(const LinkType& subject, PropertyKeyID key) const
{
    if (!m_keysOfFieldsThatMayHoldFunctions.quickGet(key.index()))
        return false;
    auto mayFindItIn = [&](LayoutID layout) {
        std::optional<FieldReference> field = findField(m_program, layout, key);
        FlowNodeID appended = m_flow.findAppendedField(layout, key);
        return (field && mayHoldFunction(m_flow.field(*field))) || (appended && mayHoldFunction(appended));
    };
    switch (subject.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
        return false;
    case LinkType::Objects::Layout:
        return mayFindItIn(subject.layout);
    case LinkType::Objects::Set:
        return std::ranges::any_of(m_flow.layoutsOfSet(subject), mayFindItIn);
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto CalleeSummarizerForLinkEffects::beliefAt(FunctionID function, const ThinSummary& summary, uint32_t nodeInSummary) const -> Belief
{
    ASSERT(isSummarized(function.index()));
    if (m_nodesBelievedForLessThanEverything[function.index()].get(nodeInSummary))
        return m_nodesBelievedForNothing[function.index()].get(nodeInSummary) ? Belief::Nothing : Belief::WhatDoorsAsk;
    return beliefInKindOfNode(summary.nodes[nodeInSummary].kind);
}

bool CalleeSummarizerForLinkEffects::containsOnlyPrimitives(const LinkType& type, uint16_t primitives)
{
    return !type.isNever() && !mayBeObjectForLinkEffects(type) && !type.holdsLostBuiltins && !(type.primitives & ~LinkType::closed(primitives));
}

auto CalleeSummarizerForLinkEffects::indexedKindAt(FunctionID id, const ThinSummary& summary, uint32_t nodeInSummary) const -> IndexedKind
{
    if (beliefAt(id, summary, nodeInSummary) == Belief::Nothing)
        return IndexedKind::Unknown;
    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
    const LinkType& held = m_flow.typeAt(m_flow.nodeOf(id, nodeInSummary));
    if (containsOnlyPrimitives(held, Primitive::AnyString | primitivesWithoutMembers))
        return m_stringsHaveNoOtherIndexes && (held.primitives & LinkType::closed(Primitive::AnyString)) ? IndexedKind::String : IndexedKind::Unknown;
    if ((held.primitives & ~primitivesWithoutMembers) || held.objects != LinkType::Objects::None || held.functions != LinkType::Functions::None || held.hasUnknownObjects || held.holdsLostBuiltins || held.builtin || !m_flow.namesClassesOf(held))
        return IndexedKind::Unknown;
    uint32_t numberOfClasses = 0;
    bool areTypedArrays = true;
    m_flow.forEachClassOf(held, [&](const LinkType& instances) {
        TypedArrayType typedArray = m_builtins.typedArrayTypeOf(instances.builtinClass);
        areTypedArrays &= isTypedView(typedArray) && !isBigInt(typedArray);
        ++numberOfClasses;
    });
    return areTypedArrays && numberOfClasses == 1 ? IndexedKind::TypedArray : IndexedKind::Unknown;
}

auto CalleeSummarizerForLinkEffects::findIndexedAccess(FunctionID id, const ThinSummary& summary, const SummaryAccess& access) const -> IndexedKind
{
    if ((access.kind != SummaryAccess::Kind::Read && access.kind != SummaryAccess::Kind::Store) || !isUnderComputedKeyForLinkEffects(access))
        return IndexedKind::Unknown;
    IndexedKind kind = indexedKindAt(id, summary, access.subject);
    if (kind == IndexedKind::Unknown || !containsOnlyPrimitives(typeAt(id, summary, access.keyNode), Primitive::Number))
        return IndexedKind::Unknown;
    if (access.kind == SummaryAccess::Kind::Read)
        return kind;
    bool storesNumber = kind == IndexedKind::TypedArray && access.value && access.value != noSummaryNode && containsOnlyPrimitives(typeAt(id, summary, access.value), Primitive::NumberLike);
    return storesNumber ? kind : IndexedKind::Unknown;
}

LinkType CalleeSummarizerForLinkEffects::typeAt(FunctionID function, const ThinSummary& summary, uint32_t nodeInSummary) const
{
    if (nodeInSummary != noSummaryNode) {
        Belief belief = beliefAt(function, summary, nodeInSummary);
        if (belief == Belief::Nothing)
            return LinkType::anything();
        LinkType linked = m_flow.typeAt(m_flow.nodeOf(function, nodeInSummary));
        return belief == Belief::Everything ? linked : asDoorsAsk(linked);
    }
    LinkType undefined;
    undefined.primitives = Primitive::Undefined;
    return undefined;
}

void CalleeSummarizerForLinkEffects::indexHolders()
{
    uint32_t numberOfKeys = m_program.types.numberOfPropertyKeys();
    m_keysOfAccessorFields.ensureSize(numberOfKeys);
    m_keysOfOtherCallables.ensureSize(numberOfKeys);
    m_keysOfFieldsThatHoldOneClosure.ensureSize(numberOfKeys);
    m_keysOfFieldsThatMayHoldFunctions.ensureSize(numberOfKeys);
    m_keysThatObjectPrototypeNeverAnswers.fill(Answer::NotAsked, numberOfKeys);

    static constexpr uint8_t runsAtReads = 1 << 0;
    static constexpr uint8_t runsAtStores = 1 << 1;
    static constexpr uint8_t mayBeOtherCallable = 1 << 2;
    static constexpr uint8_t isAccessorField = 1 << 3;
    static constexpr uint8_t holdsOneClosure = 1 << 4;
    static constexpr uint8_t mayHoldFunctions = 1 << 5;
    struct HolderUnderKey {
        PropertyKeyID key;
        FlowNodeID holder;
        uint8_t traits;
    };
    auto traitsOfCallableAt = [&](FlowNodeID holder) -> uint8_t {
        return mayBeOtherCallableForLinkEffects(m_flow.typeAt(holder)) ? mayBeOtherCallable : 0;
    };
    auto note = [&](const HolderUnderKey& noted) {
        if (noted.traits & (runsAtReads | runsAtStores))
            m_accessors.append(AccessorUnderKey { noted.key, noted.holder, !!(noted.traits & runsAtReads), !!(noted.traits & runsAtStores) });
        if (noted.traits & mayBeOtherCallable)
            m_keysOfOtherCallables.quickSet(noted.key.index());
        if (noted.traits & isAccessorField)
            m_keysOfAccessorFields.quickSet(noted.key.index());
        if (noted.traits & holdsOneClosure)
            m_keysOfFieldsThatHoldOneClosure.quickSet(noted.key.index());
        if (noted.traits & mayHoldFunctions)
            m_keysOfFieldsThatMayHoldFunctions.quickSet(noted.key.index());
    };
    auto noteAll = [&](const Vector<Vector<HolderUnderKey>>& chunks) {
        for (const Vector<HolderUnderKey>& chunk : chunks)
            std::ranges::for_each(chunk, note);
    };

    noteAll(collectChunksInParallel<HolderUnderKey>(m_flow.loops, m_numberOfFunctions, [&](unsigned index, Vector<HolderUnderKey>& found) {
        for (const SummaryMember& member : m_flow.summaries.at(FunctionID(index)).members) {
            if (member.side != ClassSide::Prototype)
                continue;
            FlowNodeID holder = m_flow.member(member.classLayout, member.side, member.index);
            if (!member.key)
                continue;
            uint8_t traits = member.kind == MethodKind::Getter ? runsAtReads : member.kind == MethodKind::Setter ? runsAtStores : traitsOfCallableAt(holder);
            if (traits)
                found.append(HolderUnderKey { member.key, holder, traits });
        }
    }));
    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        if (name.key)
            note(HolderUnderKey { name.key, name.stored, traitsOfCallableAt(name.stored) });
        if (name.isAccessorAsWritten) {
            note(HolderUnderKey { name.key, name.getter, runsAtReads });
            note(HolderUnderKey { name.key, name.setter, runsAtStores });
        }
    }
    noteAll(collectChunksInParallel<HolderUnderKey>(m_flow.loops, static_cast<unsigned>(m_program.layouts.size()), [&](unsigned index, Vector<HolderUnderKey>& found) {
        const Layout& layout = m_program.layouts[index];
        uint32_t numberOfFields = baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index)) ? layout.ownFieldCount : static_cast<uint32_t>(layout.fields.size());
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            PropertyKeyID key = layout.fields[field].key;
            FlowNodeID holder = m_flow.field({ LayoutID(index), field });
            if (!key)
                continue;
            uint8_t traits = layout.fields[field].form.contains(FieldForm::IsAccessor) ? isAccessorField : traitsOfCallableAt(holder);
            if (m_flow.typeAt(holder).functions == LinkType::Functions::Closure)
                traits |= holdsOneClosure;
            if (mayHoldFunction(holder))
                traits |= mayHoldFunctions;
            if (traits)
                found.append(HolderUnderKey { key, holder, traits });
        }
    }));
    m_flow.forEachAppendedField([&](PropertyKeyID key, FlowNodeID field) {
        if (key && mayHoldFunction(field))
            m_keysOfFieldsThatMayHoldFunctions.quickSet(key.index());
    });
    AOT::stableSort(m_accessors, { }, &AccessorUnderKey::key);

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
        bool mayFindAnything = !isFoundByShapeTest(m_program.layouts[index]) || m_flow.escapedLayouts.get(index)
            || m_tables.layouts[index].possibleChanges.containsAny({ LayoutChange::GetsAccessor, LayoutChange::ChangesPrototype })
            || m_flow.namesMayBeIncomplete(LayoutID(index), ClassSide::Prototype);
        if (mayFindAnything)
            byItself.quickSet(index);
    }

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

void CalleeSummarizerForLinkEffects::findNearestClassesAbove()
{
    uint32_t numberOfLayouts = m_program.layouts.size();
    auto baseOf = [&](LayoutID id) { return m_program.at(id).base; };
    m_nearestAboveWithStaticFields = findNearestWantedAbove(numberOfLayouts, baseOf, [&](LayoutID id) { return !m_program.at(id).staticFields.isEmpty(); });
    m_nearestAboveWithNamesOnPrototype = findNearestWantedAbove(numberOfLayouts, baseOf, [&](LayoutID id) { return m_flow.hasNameOnPrototypeOf(id); });
    m_classObjectsAreAsMadeFrom = answerAlongRows(numberOfLayouts, true, [&](LayoutID id) -> StepAlongRow {
        const Layout& layout = m_program.at(id);
        FunctionID constructor = layout.constructor;
        if (!constructor || m_flow.classOfConstructor(constructor) != id || !closuresAreAsMade(constructor) || m_tables.layouts[id.index()].possibleChanges.contains(LayoutChange::ChangesPrototype))
            return { false, { } };
        if (!layout.base)
            return { true, { } };
        return { std::nullopt, layout.base };
    });
}

bool CalleeSummarizerForLinkEffects::closuresAreAsMade(FunctionID holder) const
{
    return !m_flow.escapedFunctions.get(holder.index()) && m_flow.closuresInheritFunctionPrototype(holder);
}

bool CalleeSummarizerForLinkEffects::mayFindAnything(const LinkType& subject) const
{
    switch (subject.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Prototype:
        return m_layoutsThatMayFindAnything.get(subject.layout.index());
    case LinkType::Objects::Set:
        return std::ranges::any_of(m_flow.layoutsOfSet(subject), [&](LayoutID member) {
            return m_layoutsThatMayFindAnything.get(member.index());
        });
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
        return m_familiesThatMayFindAnything.get(subject.family.index());
    case LinkType::Objects::Lost:
        return m_lostLayoutsMayFindAnything;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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

bool CalleeSummarizerForLinkEffects::functionPrototypeNeverAnswers(PropertyKeyID key) const
{
    BuiltinID holder = m_builtins.canonicalID(Builtins::FunctionPrototype);
    return !findOwnMember(m_program, holder, key) && listsEveryMemberUnder(m_program, holder, key) && isClosedKey(m_program, holder, key);
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
    case LinkType::Functions::Set:
        for (FunctionID member : m_flow.functionsOfSet(held))
            found.add(member);
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
    for (const LinkFlow::FoundMember& member : m_flow.findMembers(layout, ClassSide::Prototype, key))
        lookAt(m_flow.typeAt(m_flow.member(member.classLayout, ClassSide::Prototype, member.index)), member.kind == wanted);
    uint32_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = m_nearestAboveWithNamesOnPrototype[current.index()], --stepsLeft) {
        const LinkFlow::NameOnPrototype* name = m_flow.findNameOnPrototype(current, key);
        if (!name)
            continue;
        lookAt(m_flow.typeAt(name->stored), false);
        if (name->isAccessorAsWritten && wanted != MethodKind::Method)
            lookAt(m_flow.typeAt(wanted == MethodKind::Getter ? name->getter : name->setter), true);
    }
}

template<typename Functor>
void CalleeSummarizerForLinkEffects::forEachStaticFieldFound(FunctionID function, PropertyKeyID key, const Functor& functor) const
{
    uint32_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = m_flow.classOfConstructor(function); current && stepsLeft; current = m_nearestAboveWithStaticFields[current.index()], --stepsLeft) {
        m_program.at(current).forEachStaticFieldThatMayMatch(key, [&](const StaticField& field) {
            functor(field.variable);
        });
    }
}

void CalleeSummarizerForLinkEffects::lookOffFunction(FunctionID function, PropertyKeyID key, MethodKind wanted, bool callsWhatIsFound, CodeFound& found)
{
    if (wanted == MethodKind::Getter && !callsWhatIsFound && key == m_program.keyOf(BuiltinNames::prototype) && ownsPrototype(function))
        return;
    LayoutID classLayout = m_flow.classOfConstructor(function);
    bool isOther = !classLayout && (isClassConstructorOf(m_program.at(function).kind) || callsWhatIsFound);
    if (isOther || m_flow.isDefinedByDescriptorSomewhere(key) || (classLayout && m_flow.namesMayBeIncomplete(classLayout, ClassSide::Static))) {
        found.mayBeUnseen = true;
        return;
    }
    if (!closuresAreAsMade(function) || (classLayout && !m_classObjectsAreAsMadeFrom.quickGet(classLayout.index()))) {
        found.mayBeUnseen = true;
        return;
    }

    bool findsField = false;
    forEachStaticFieldFound(function, key, [&](VariableID) {
        findsField = true;
    });
    bool findsMember = false;
    bool lookupGoesOn = true;
    if (classLayout) {
        LinkFlow::FoundMembers members = m_flow.findMembers(classLayout, ClassSide::Static, key);
        lookupGoesOn = members.lookupGoesOnAboveClasses();
        for (const LinkFlow::FoundMember& member : members) {
            findsMember = true;
            const LinkType& held = m_flow.typeAt(m_flow.member(member.classLayout, ClassSide::Static, member.index));
            if (member.kind == wanted) {
                addFunctionsAt(held, found);
                found.mayBeUnseen |= callsWhatIsFound;
            } else if (callsWhatIsFound && (member.kind != MethodKind::Method || held.functions != LinkType::Functions::Closure || held.objects != LinkType::Objects::None || mayBeOtherCallableForLinkEffects(held)))
                found.mayBeUnseen = true;
        }
    }
    if (callsWhatIsFound && (findsField || !findsMember || lookupGoesOn))
        found.mayBeUnseen = true;
    if (lookupGoesOn && (!functionPrototypeNeverAnswers(key) || !objectPrototypeNeverAnswers(key)))
        found.mayBeUnseen = true;
}

void CalleeSummarizerForLinkEffects::findCodeBehindLookup(const LinkType& subject, PropertyKeyID key, SummaryAccess::Kind kind, bool callsWhatIsFound, CodeFound& found)
{
    if (subject.isNever())
        return;
    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
    if (subject.functions != LinkType::Functions::None) {
        bool isFunction = subject.objects == LinkType::Objects::None && !mayBeOtherCallableForLinkEffects(subject) && !subject.holdsLostBuiltins && !(subject.primitives & ~primitivesWithoutMembers);
        MethodKind wanted = kind == SummaryAccess::Kind::Store ? MethodKind::Setter : MethodKind::Getter;
        bool isLookedUp = isFunction && key && !m_program.isIndexLike(key) && kind != SummaryAccess::Kind::Define && kind != SummaryAccess::Kind::Has && forEachFunctionOf(subject, [&](FunctionID function) {
            lookOffFunction(function, key, wanted, callsWhatIsFound, found);
        });
        found.mayBeUnseen |= !isLookedUp;
        return;
    }
    bool isOfProgram = !mayBeOtherCallableForLinkEffects(subject) && subject.functions == LinkType::Functions::None && !(subject.primitives & ~primitivesWithoutMembers);
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
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(subject))
            lookInLayout(member, true, key, wanted, callsWhatIsFound, found);
        return;
    case LinkType::Objects::Prototype:
        lookInLayout(subject.layout, false, key, wanted, callsWhatIsFound, found);
        return;
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        break;
    }

    for (const AccessorUnderKey& accessor : std::ranges::equal_range(m_accessors, key, { }, &AccessorUnderKey::key)) {
        if (!(wanted == MethodKind::Getter ? accessor.runsAtReads : accessor.runsAtStores))
            continue;
        addFunctionsAt(m_flow.typeAt(accessor.holder), found);
        found.mayBeUnseen |= callsWhatIsFound;
    }
    if (m_keysOfAccessorFields.get(key.index()) || (callsWhatIsFound && m_keysOfOtherCallables.get(key.index())))
        found.mayBeUnseen = true;
}

bool CalleeSummarizerForLinkEffects::thenLookupCannotThrow(const LinkType& subject, PropertyKeyID key) const
{
    bool asksForThen = key && std::ranges::any_of(m_program.builtins().keysReadBy(Protocol::Thenable), [&](BuiltinName name) { return m_program.keyOf(name) == key; });
    if (!asksForThen || subject.primitives || subject.hasUnknownObjects || subject.holdsLostBuiltins || subject.builtin || subject.classes != LinkType::Classes::None || subject.functions != LinkType::Functions::None)
        return false;
    auto holdsPlainObjects = [&](LayoutID layout) {
        LayoutKind kind = m_program.at(layout).kind;
        bool isPlain = kind == LayoutKind::Instance || kind == LayoutKind::Record;
        for (LayoutID current = layout; current && isPlain; current = m_program.at(current).base)
            isPlain = !builtinClassOfInstances(m_program.at(current));
        return isPlain;
    };
    if (subject.objects == LinkType::Objects::Layout)
        return holdsPlainObjects(subject.layout);
    return subject.objects == LinkType::Objects::Set && std::ranges::all_of(m_flow.layoutsOfSet(subject), holdsPlainObjects);
}

Effects CalleeSummarizerForLinkEffects::effectsOfLookupItself(SummaryAccess::Kind kind, PropertyKeyID key) const
{
    constexpr Effects ofDefinition = declaredBoundOf(NamedEffectSet::DefinesProperty);
    constexpr AbstractHeapSet heapsUnderKeys { AbstractHeap::ObjectFields, AbstractHeap::NamedProperties };

    Effects effects = declaredBoundOf(NamedEffectSet::Allocates);
    effects.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
    effects.reads.merge(ofDefinition.reads);
    effects.reads.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards });
    effects.writes.merge(Effects::ofReadByNameThatRunsNoCode().writes);
    effects.mayThrow = ThrowEffect(true);
    switch (kind) {
    case SummaryAccess::Kind::Read:
    case SummaryAccess::Kind::Has:
        return effects;
    case SummaryAccess::Kind::Store:
    case SummaryAccess::Kind::Define:
        break;
    }
    effects.writes.add(AbstractHeap::WrittenTypes);
    if (!key) {
        effects.writes.merge(ofDefinition.writes);
        return effects;
    }
    effects.writes.merge(ofDefinition.writes.without(heapsUnderKeys));
    effects.writes.add(m_program, MemoryLocation::objectField(key));
    effects.writes.add(m_program, MemoryLocation::at(AbstractHeap::NamedProperties, key.index()));
    return effects;
}

std::span<const BuiltinID> CalleeSummarizerForLinkEffects::builtinsEnteredBy(FunctionID id, const ThinSummary& summary, uint32_t index) const
{
    const SummaryCall& call = summary.calls[index];
    if (call.callee == SummaryCall::Callee::Builtin)
        return std::span<const BuiltinID>(&call.builtin, 1);
    return m_flow.calleesOfCalls[m_flow.firstCallOfFunction[id.index()] + index].builtins().span();
}

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
    if (type.isNever())
        return true;
    bool mayBeObject = mayBeObjectForLinkEffects(type);
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
    if (parameter->uses.containsAny(usesThatCallOperand)) {
        if (type.objects != LinkType::Objects::None || mayBeOtherCallableForLinkEffects(type) || !rowSaysAllOfCallsOf(row, parameter->operand))
            return false;
        addFunctionsAt(type, found);
        effects.mayThrow = ThrowEffect(true);
    }
    if (parameter->uses.containsAny(usesThatChangeShape))
        effects.writes.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards, AbstractHeap::ShapeWords, AbstractHeap::OrderWords });
    if (parameter->uses.contains(OperandUse::DefinesName))
        effects.writes.add(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::WrittenTypes });
    return true;
}

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

    OperandUses uses;
    if (row.receiver)
        uses.add(row.receiver->uses);
    for (const BuiltinParameter& parameter : row.parameters)
        uses.add(parameter.uses);
    if ((uses.containsAny(usesThatChangeOperand) && row.effects.writes.isEmpty()) || (uses.contains(OperandUse::ReadsContents) && row.effects.reads.isEmpty()))
        return false;
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
    return !first.isNever() && !mayBeObjectForLinkEffects(first) && !(first.primitives & ~Primitive::UndefinedOrNull);
}

bool CalleeSummarizerForLinkEffects::addEffectsOfRows(BuiltinID named, FunctionID id, const ThinSummary& summary, const SummaryCall& call, CodeFound& found, Effects& effects) const
{
    if (!named || call.passesList || call.newTarget != noSummaryNode)
        return false;
    BuiltinID builtin = m_builtins.canonicalID(named);
    if (m_program.isUnlocked(named) || m_program.isUnlocked(builtin))
        return false;
    const Builtin& entry = m_builtins.at(builtin);
    if (entry.flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode }) || m_builtins.has(BuiltinMark::RunsWaitingJobs, builtin))
        return false;
    if (call.callee != SummaryCall::Callee::Builtin && (!isFunctionForLinkEffects(entry.kind) || isMadeWithCodeOfLibrary(m_builtins, builtin)))
        return false;
    if (std::optional<bool> answer = addEffectsOfRowsUpToOneThatSurelyTakes(builtin, id, summary, call, found, effects))
        return *answer;
    if (call.isConstruction && entry.instanceClass && m_builtins.at(entry.instanceClass).constructorAdder && !passesNoIterable(id, summary, call))
        return false;
    return forEachRowOf(builtin, call, [&](const BuiltinOverload& row) {
        return addEffectsOfRow(row, id, summary, call, found, effects);
    });
}

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
    for (FunctionID callee : callees.listed())
        found.add(callee);
    for (uint32_t shared : callees.shared())
        found.add(FunctionID(nodeOfSharedCall(shared)));
    found.mayBeLost = callees.mayEnterLost;
    found.mayBeUnseen = callees.mayEnterUnseen;

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
        LinkType receiver = typeAt(id, summary, call.receiver);
        if (receiver.hasUnknownObjects || (call.key && mayFindFunctionInFieldUnder(receiver, call.key)))
            found.mayBeUnseen = true;
        receiver.primitives = 0;
        receiver.builtin = { };
        if (membersOfInstancesAreAsTableLists(receiver, call.key))
            break;
        findCodeBehindLookup(receiver, call.key, SummaryAccess::Kind::Read, true, found);
        break;
    }
    case SummaryCall::Callee::Value: {
        if (beliefAt(id, summary, call.subject) != Belief::Everything) {
            found.mayBeUnseen = true;
            break;
        }
        LinkType subject = typeAt(id, summary, call.subject);
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

void CalleeSummarizerForLinkEffects::findCodeEntered()
{
    const ParallelLoops& loops = m_flow.loops;
    m_codeOfCalls.grow(loops, m_flow.calleesOfCalls.size());
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index)
        m_firstAccessOfFunction[index + 1] += m_firstAccessOfFunction[index];
    m_codeOfAccesses.grow(loops, m_firstAccessOfFunction.last());

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
            bool isConstructor = isClassConstructorOf(m_program.at(id).kind);
            for (uint32_t access = 0; access < summary.accesses.size(); ++access) {
                CodeFound found;
                const SummaryAccess& looked = summary.accesses[access];
                bool isOfConstruction = isConstructor && (isSuperConstructorRead(id, summary, looked) || isNewTargetPrototypeRead(id, summary, looked));
                if (!looked.isToContents() && !isOfConstruction && findIndexedAccess(id, summary, looked) == IndexedKind::Unknown)
                    findCodeBehindLookup(typeAt(id, summary, summary.accesses[access].subject), m_flow.keyOf(id, summary.accesses[access]), summary.accesses[access].kind, false, found);
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

Effects CalleeSummarizerForLinkEffects::effectsOfCallItself(FunctionID id, const ThinSummary& summary, uint32_t index) const
{
    const SummaryCall& call = summary.calls[index];
    Effects effects = Effects::none();
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
    case SummaryCall::Callee::Builtin:
        break;
    case SummaryCall::Callee::Name:
        if (!typeAt(id, call.receiver).isNever()) {
            effects = effectsOfLookupItself(SummaryAccess::Kind::Read, call.key);
            if (call.isMadeByProtocol && thenLookupCannotThrow(typeAt(id, call.receiver), call.key))
                effects.mayThrow = ThrowEffect(false);
            if (call.key && readMayMakeClosureOfField(typeAt(id, call.receiver), call.key))
                effects.writes.add(m_program, MemoryLocation::objectField(call.key));
        }
        break;
    case SummaryCall::Callee::Value:
        effects.mayThrow = ThrowEffect(true);
        break;
    }
    for (BuiltinID builtin : builtinsEnteredBy(id, summary, index)) {
        CodeFound found;
        if (!addEffectsOfRows(builtin, id, summary, call, found, effects))
            return Effects::unknownCode();
        if (standsForFirstReadOffHolder(m_program.builtins(), builtin, false))
            effects.merge(declaredBoundInTable(opcodeOfFirstReadOffHolder(m_program.builtins(), builtin)));
    }
    if (call.passesList) {
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

bool CalleeSummarizerForLinkEffects::handsObjectToCodeNobodyNames(FunctionID id, const ThinSummary& summary) const
{
    return std::ranges::any_of(summary.edges, [&](const SummaryEdge& edge) {
        return edge.runsCodeOfValue && summary.nodes[edge.to].kind == SummaryNode::Kind::Unknown && mayBeObjectForLinkEffects(m_flow.passedBy(typeAt(id, edge.from), edge.passes));
    });
}

bool CalleeSummarizerForLinkEffects::changeMayRunCode(FunctionID id, const SummaryChange& change) const
{
    const LinkType subject = typeAt(id, change.subject);
    if (subject.isNever())
        return false;
    switch (change.kind) {
    case SummaryChange::Kind::CopiesInto:
        return copyMayRunCode(id, change);
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

void CalleeSummarizerForLinkEffects::findComponentsOfCallees()
{
    const ParallelLoops& loops = m_flow.loops;
    auto forEachCodeEnteredBy = [&](size_t function, const auto& functor) {
        for (uint32_t call = m_flow.firstCallOfFunction[function]; call < m_flow.firstCallOfFunction[function + 1]; ++call)
            functor(m_codeOfCalls[call]);
        for (uint32_t access = m_firstAccessOfFunction[function]; access < m_firstAccessOfFunction[function + 1]; ++access)
            functor(m_codeOfAccesses[access]);
    };
    m_firstCallee.reserveInitialCapacity(numberOfNodesOfCallees() + 1);
    m_firstCallee.fill(0, m_numberOfFunctions + 1);
    forEachRangeOfIndicesInParallel(loops, m_numberOfFunctions, [&](size_t first, size_t end) {
        for (size_t function = first; function < end; ++function) {
            if (!isSummarized(static_cast<uint32_t>(function)))
                continue;
            uint32_t numberOfCallees = 0;
            bool mayEnterLost = false;
            forEachCodeEnteredBy(function, [&](const CodeEntered& code) {
                numberOfCallees += code.numberOfFunctions;
                mayEnterLost |= code.mayBeLost;
            });
            m_firstCallee[function + 1] = numberOfCallees + mayEnterLost;
        }
    });
    for (uint32_t function = 0; function < m_numberOfFunctions; ++function)
        m_firstCallee[function + 1] += m_firstCallee[function];
    m_callees.reserveInitialCapacity(std::max<size_t>(m_functionsEntered.size() + m_numberOfFunctions, m_firstCallee.last()));
    m_callees.grow(m_firstCallee.last());
    forEachRangeOfIndicesInParallel(loops, m_numberOfFunctions, [&](size_t first, size_t end) {
        for (size_t function = first; function < end; ++function) {
            uint32_t place = m_firstCallee[function];
            if (place == m_firstCallee[function + 1])
                continue;
            bool mayEnterLost = false;
            forEachCodeEnteredBy(function, [&](const CodeEntered& code) {
                for (FunctionID callee : functionsOf(code))
                    m_callees[place++] = callee;
                mayEnterLost |= code.mayBeLost;
            });
            if (mayEnterLost)
                m_callees[place++] = FunctionID(nodeOfLostFunctions());
            RELEASE_ASSERT(place == m_firstCallee[function + 1]);
        }
    });
    for (size_t function : m_flow.lostFunctions) {
        if (m_flow.reached.get(function))
            m_callees.append(FunctionID(static_cast<uint32_t>(function)));
    }
    for (const LinkFlow::CalleesOfCall& shared : m_flow.calleesOfSharedCalls) {
        m_firstCallee.append(m_callees.size());
        m_callees.append(shared.listed().span());
    }
    m_firstCallee.append(m_callees.size());

    m_components = findComponentsForLinkEffects(numberOfNodesOfCallees(), [&](uint32_t function) {
        return calleesOf(function);
    });
}

void CalleeSummarizerForLinkEffects::prefetchSummariesBehind(std::span<const uint32_t> order, size_t place) const
{
    static constexpr size_t distanceOfSummary = 8;
    static constexpr size_t distanceOfRecords = 4;
    auto memberAt = [&](size_t position) -> std::optional<uint32_t> {
        if (position >= order.size() || !isFunction(order[position]))
            return std::nullopt;
        return order[position];
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

void CalleeSummarizerForLinkEffects::prefetchSummaryOfMemberBehindInRanks(size_t position) const
{
    prefetchSummariesBehind(m_membersInRanks.span(), position);
}

static bool removeOptionalReferrersInCycles(LinkTables& tables, const ComponentsOfLinkEffects& cycles)
{
    Vector<uint64_t> removed = std::exchange(tables.optionalReferrers, { });
    removed.removeAllMatching([&](uint64_t row) {
        return cycles.componentOfFunction[LinkTables::functionOfRow(row).index()] != cycles.componentOfFunction[LinkTables::referrerOfRow(row).index()];
    });
    if (removed.isEmpty())
        return false;
    size_t next = 0;
    uint32_t numberKept = 0;
    uint32_t place = 0;
    for (uint32_t node = 0; node + 1 < tables.firstReferrer.size(); ++node) {
        uint32_t end = tables.firstReferrer[node + 1];
        tables.firstReferrer[node] = numberKept;
        for (; place < end; ++place) {
            FunctionID referrer = tables.referrers[place];
            if (next < removed.size() && removed[next] == LinkTables::rowOfReferrer(FunctionID(node), referrer))
                ++next;
            else
                tables.referrers[numberKept++] = referrer;
        }
    }
    tables.firstReferrer.last() = numberKept;
    tables.referrers.shrink(numberKept);
    RELEASE_ASSERT(next == removed.size());
    return true;
}

void rankReferrersFirst(LinkTables& tables)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(tables.functions.size());
    uint32_t numberOfNodes = tables.numberOfNodesOfReferrers();
    if (tables.referrers.isEmpty() || tables.firstReferrer.size() != numberOfNodes + 1)
        return;

    auto findCycles = [&] {
        return findComponentsForLinkEffects(numberOfNodes, [&](uint32_t node) {
            return tables.referrersOf(FunctionID(node));
        });
    };
    ComponentsOfLinkEffects cycles = findCycles();
    if (removeOptionalReferrersInCycles(tables, cycles))
        cycles = findCycles();

    constexpr uint32_t none = UINT32_MAX;
    Vector<uint32_t>& rankOfCycle = tables.rankOfCycle;
    Vector<uint32_t>& cyclesAwaited = tables.cyclesAwaited;
    rankOfCycle.fill(0, cycles.size());
    cyclesAwaited.shrink(0);
    tables.firstCycleAwaited.shrink(0);
    tables.firstCycleAwaited.reserveCapacity(cycles.size() + 1);
    Vector<uint32_t> lastToAwait;
    lastToAwait.fill(none, cycles.size());

    for (uint32_t cycle = 0; cycle < cycles.size(); ++cycle) {
        std::span<const uint32_t> members = cycles.membersOf(cycle);
        size_t firstAwaited = cyclesAwaited.size();
        tables.firstCycleAwaited.append(static_cast<uint32_t>(firstAwaited));
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
                continue;
            }
            LinkedFunction& function = tables.functions[member];
            function.cycle = cycle;
            function.isNamedInItsCycle = namesItself;
        }
    }
    tables.firstCycleAwaited.append(static_cast<uint32_t>(cyclesAwaited.size()));
    tables.listCyclesNamed();

    for (LinkedFunction& function : tables.functions)
        function.namesOnlyWhatEveryFunctionLeadsTo = true;
    BitVector groupsThatNameMore(tables.groups.size());
    auto everyFunctionLeadsTo = [&](uint32_t node) {
        if (node < numberOfFunctions)
            return tables.functions[node].isInQueueFromStart;
        LinkedGroup::Kind kind = tables.groups[node - numberOfFunctions].kind;
        return kind == LinkedGroup::Kind::Anybody || (kind == LinkedGroup::Kind::FunctionsInFields && !groupsThatNameMore.get(node - numberOfFunctions));
    };
    for (uint32_t node = 0; node < numberOfNodes; ++node) {
        if (everyFunctionLeadsTo(node))
            continue;
        for (FunctionID referrer : tables.referrersOf(FunctionID(node))) {
            if (referrer.index() >= numberOfFunctions)
                groupsThatNameMore.set(referrer.index() - numberOfFunctions);
            else if (referrer.index() != node)
                tables.functions[referrer.index()].namesOnlyWhatEveryFunctionLeadsTo = false;
        }
    }
}

void summarizeCallees(const LinkFlow& flow, LinkTables& tables)
{
    CalleeSummarizerForLinkEffects summarizer(flow, tables);
    summarizer.findComponentsOfCallees();
    summarizer.summarizeKnownEffects();
    summarizer.writeKnownEffects();
}

void writePrototypeObjectsThatAreSettled(const LinkFlow& flow, LinkTables& tables)
{
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
        layout.prototypeObjectIsSettledAheadOfEveryObject = function.prototypePropertyStaysAsMade;
        if (!flow.findsPlainProperties(record.constructor))
            continue;
        for (const SummaryStoreOnPrototype& laid : std::ranges::equal_range(flow.storesBeforeInstances, record.constructor, { }, &SummaryStoreOnPrototype::function)) {
            if (laid.kind == SummaryStoreOnPrototype::Kind::LiteralBeforeInstances)
                layout.prototypeLiteralIsSettled = function.prototypePropertyStaysAsLiteral;
            else if (laid.kind == SummaryStoreOnPrototype::Kind::ParentBeforeInstances)
                layout.parentIsSettled |= record.base && flow.layoutOfConstructorFunction(laid.method) == record.base;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#include "AOTLinkKnownEffectsInlines.h"

#include "AOTLinkKnownRowsInlines.h"

#endif // ENABLE(AOT)
