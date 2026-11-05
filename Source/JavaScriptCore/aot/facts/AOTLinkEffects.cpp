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

static bool mayBeObjectForLinkEffects(const LinkType& type)
{
    return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.holdsUnseen;
}

static bool mayBeOtherCallableForLinkEffects(const LinkType& type)
{
    return type.classes != LinkType::Classes::None || type.builtin || type.holdsUnseen;
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

private:
    enum class Answer : uint8_t { NotAsked, No, Yes };

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
    bool keepsDefaults(FunctionID, const ThinSummary&, OptionSet<SummaryTrait>) const;
    uint32_t nodeOfLostFunctions() const { return m_numberOfFunctions; }
    uint32_t nodeOfSharedCall(uint32_t shared) const { return m_numberOfFunctions + 1 + shared; }
    bool isFunction(uint32_t node) const { return node < m_numberOfFunctions; }
    bool isSharedCall(uint32_t node) const { return node > m_numberOfFunctions; }
    uint32_t numberOfNodesOfCallees() const { return m_numberOfFunctions + 1 + static_cast<uint32_t>(m_flow.calleesOfSharedCalls.size()); }

    LinkType typeAt(FunctionID, uint32_t nodeInSummary) const;
    static std::span<const uint32_t> argumentsOf(const ThinSummary& summary, const SummaryCall& call) { return summary.arguments.span().subspan(call.firstArgument, call.numberOfArguments); }
    std::span<const FunctionID> functionsOf(const CodeEntered& code) const LIFETIME_BOUND { return m_functionsEntered.span().subspan(code.firstFunction, code.numberOfFunctions); }
    std::span<const FunctionID> calleesOf(uint32_t function) const LIFETIME_BOUND { return m_callees.span().subspan(m_firstCallee[function], m_firstCallee[function + 1] - m_firstCallee[function]); }
    void prefetchSummariesBehind(std::span<const uint32_t> order, size_t place) const;
    const CodeEntered& codeOfCall(FunctionID function, uint32_t call) const LIFETIME_BOUND { return m_codeOfCalls[m_flow.firstCallOfFunction[function.index()] + call]; }
    const CodeEntered& codeOfAccess(FunctionID function, uint32_t access) const LIFETIME_BOUND { return m_codeOfAccesses[m_firstAccessOfFunction[function.index()] + access]; }

    void indexHolders();
    void findLayoutsThatMayFindAnything();
    bool mayFindAnything(const LinkType& subject) const;
    bool readsFixedPartOfClass(const LinkType& subject, PropertyKeyID, bool hasOtherReceiver);
    bool objectPrototypeNeverAnswers(PropertyKeyID);
    void addFunctionsAt(const LinkType& held, CodeFound&) const;
    void lookInLayout(LayoutID, bool hasOwnFields, PropertyKeyID, MethodKind wanted, bool callsWhatIsFound, CodeFound&) const;
    void findCodeBehindLookup(const LinkType& subject, PropertyKeyID, SummaryAccess::Kind, bool hasOtherReceiver, bool callsWhatIsFound, CodeFound&);
    Effects effectsOfLookupItself(SummaryAccess::Kind, PropertyKeyID) const;

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
    void addEffectsOfOperatorsLeftApart(FunctionID, const ThinSummary&, EffectsOfComponentInKnownEffects&) const;
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
    BitVector m_keysOfFieldsThatHoldOneClosure;
    BitVector m_layoutsOfPlainObjects;
    Vector<LinkedEffectsGivenArguments> m_effectsGivenArguments;

    struct HandOfSummarizer {
        Vector<LinkedEffectsGivenArguments> effectsGivenArguments;
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

    Vector<AccessorUnderKey> m_accessors;
    BitVector m_keysOfAccessorFields;
    BitVector m_keysOfOtherCallables;
    BitVector m_layoutsThatMayFindAnything;
    bool m_lostLayoutsMayFindAnything { false };
    BitVector m_familiesThatMayFindAnything;
    Vector<Answer> m_keysThatObjectPrototypeNeverAnswers;

    Vector<CodeEntered> m_codeOfCalls;
    Vector<uint32_t> m_firstAccessOfFunction;
    Vector<CodeEntered> m_codeOfAccesses;
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
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        const Function& function = m_program.functions[index];
        if (!flow.reached.get(index) || !function.hasBody())
            continue;
        RELEASE_ASSERT(flow.firstCallOfFunction[index + 1] - flow.firstCallOfFunction[index] == flow.summaries.at(FunctionID(index)).calls.size());
        m_isSummarized.quickSet(index);
    }

    indexHolders();
    findLayoutsThatMayFindAnything();
    findCodeEntered();
}

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

void CalleeSummarizerForLinkEffects::indexHolders()
{
    uint32_t numberOfKeys = m_program.types.numberOfPropertyKeys();
    m_keysOfAccessorFields.ensureSize(numberOfKeys);
    m_keysOfOtherCallables.ensureSize(numberOfKeys);
    m_keysOfFieldsThatHoldOneClosure.ensureSize(numberOfKeys);
    m_keysThatObjectPrototypeNeverAnswers.fill(Answer::NotAsked, numberOfKeys);

    auto note = [&](PropertyKeyID key, FlowNodeID holder, bool runsAtReads, bool runsAtStores) {
        if (!key)
            return;
        if (runsAtReads || runsAtStores)
            m_accessors.append(AccessorUnderKey { key, holder, runsAtReads, runsAtStores });
        else if (mayBeOtherCallableForLinkEffects(m_flow.typeAt(holder)))
            m_keysOfOtherCallables.quickSet(key.index());
    };

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
            if (key && m_flow.typeAt(m_flow.field({ LayoutID(index), field })).functions == LinkType::Functions::Closure)
                m_keysOfFieldsThatHoldOneClosure.quickSet(key.index());
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
    for (LayoutID current = layout; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
        if (const LinkFlow::NameOnPrototype* name = m_flow.findNameOnPrototype(current, key))
            lookAt(m_flow.typeAt(name->stored), false);
    }
}

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

void CalleeSummarizerForLinkEffects::findCodeBehindLookup(const LinkType& subject, PropertyKeyID key, SummaryAccess::Kind kind, bool hasOtherReceiver, bool callsWhatIsFound, CodeFound& found)
{
    if (subject.isNothing())
        return;
    if (kind == SummaryAccess::Kind::Read && key && !callsWhatIsFound && readsFixedPartOfClass(subject, key, hasOtherReceiver))
        return;
    constexpr uint16_t primitivesWithoutMembers = Primitive::UndefinedOrNull | Primitive::Absent;
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

Effects CalleeSummarizerForLinkEffects::effectsOfLookupItself(SummaryAccess::Kind kind, PropertyKeyID key) const
{
    constexpr Effects ofDefinition = declaredBoundOf(NamedEffectSet::DefinesProperty);
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

std::span<const BuiltinID> CalleeSummarizerForLinkEffects::builtinsEnteredBy(FunctionID id, const ThinSummary& summary, uint32_t index) const
{
    const SummaryCall& call = summary.calls[index];
    if (call.callee == SummaryCall::Callee::Builtin)
        return std::span<const BuiltinID>(&call.builtin, 1);
    return m_flow.calleesOfCalls[m_flow.firstCallOfFunction[id.index()] + index].builtins.span();
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
    if (type.isNothing())
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
    if ((uses.containsAny(usesThatChangeOperandInLinkEffects) && row.effects.writes.isEmpty()) || (uses.contains(OperandUse::ReadsContents) && row.effects.reads.isEmpty()))
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
    return !first.isNothing() && !mayBeObjectForLinkEffects(first) && !(first.primitives & ~Primitive::UndefinedOrNull);
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
    for (FunctionID callee : callees.listed)
        found.add(callee);
    for (uint32_t shared : callees.shared)
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

Effects CalleeSummarizerForLinkEffects::effectsOfCallItself(FunctionID id, const ThinSummary& summary, uint32_t index) const
{
    const SummaryCall& call = summary.calls[index];
    Effects effects = Effects::none();
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
    case SummaryCall::Callee::Builtin:
        break;
    case SummaryCall::Callee::Name:
        if (!typeAt(id, call.receiver).isNothing()) {
            effects = effectsOfLookupItself(SummaryAccess::Kind::Read, call.key);
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
    if (subject.isNothing())
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
        m_callees.appendVector(shared.listed);
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

void rankReferrersFirst(LinkTables& tables)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(tables.functions.size());
    uint32_t numberOfNodes = tables.numberOfNodesOfReferrers();
    if (tables.referrers.isEmpty() || tables.firstReferrer.size() != numberOfNodes + 1)
        return;

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
                group.rank = rank;
                continue;
            }
            LinkedFunction& function = tables.functions[member];
            function.cycle = cycle;
            function.rank = rank;
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
    }
}

} } } // namespace JSC::AOT::Typed

#include "AOTLinkKnownEffectsInlines.h"

#include "AOTLinkKnownRowsInlines.h"

#endif // ENABLE(AOT)
