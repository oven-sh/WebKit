/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTypes.h"
#include "AOTGroupingOnPool.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkLayouts.h"
#include "AOTLinkStringParsers.h"
#include "AOTLinkTypesOnAllThreads.h"
#include "AOTLinkTypesTurnedOnce.h"
#include "AOTLinkUnions.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <numeric>
#include <optional>
#include <utility>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

static LinkType withoutLostBuiltinsInLink(LinkType type)
{
    type.holdsLostBuiltins = false;
    return type;
}

void LinkTables::listCyclesNamed()
{
    size_t numberOfCycles = rankOfCycle.size();
    firstCycleNamed.fill(0, numberOfCycles + 1);
    for (uint32_t awaited : cyclesAwaited)
        ++firstCycleNamed[awaited + 1];
    for (size_t cycle = 1; cycle <= numberOfCycles; ++cycle)
        firstCycleNamed[cycle] += firstCycleNamed[cycle - 1];
    cyclesNamed.grow(cyclesAwaited.size());
    Vector<uint32_t> nextPlaceOfCycle = firstCycleNamed;
    for (uint32_t cycle = 0; cycle < numberOfCycles; ++cycle) {
        for (uint32_t awaited : cyclesAwaitedBy(cycle))
            cyclesNamed[nextPlaceOfCycle[awaited]++] = cycle;
    }
}

bool LinkTables::cycleLeadsTo(uint32_t referrer, uint32_t named) const
{
    if (referrer == named)
        return true;
    // Each step of a way from the one to the other climbs in rank.
    uint32_t lowestRank = rankOfCycle[referrer];
    uint32_t highestRank = rankOfCycle[named];
    if (lowestRank >= highestRank)
        return false;
    if (std::ranges::binary_search(cyclesNamedBy(referrer), named))
        return true;

    // From both ends, each time from the end that has fewer steps to look at: a function names few, and many may name it.
    constexpr unsigned down = 0;
    constexpr unsigned up = 1;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen[2];
    Vector<uint32_t, 32> fronts[2];
    Vector<uint32_t, 32> nextFront;
    seen[down].add(referrer);
    seen[up].add(named);
    fronts[down].append(referrer);
    fronts[up].append(named);
    auto stepsFrom = [&](unsigned end, uint32_t cycle) {
        return end == down ? cyclesNamedBy(cycle) : cyclesAwaitedBy(cycle);
    };
    auto numberOfStepsFrom = [&](unsigned end) {
        size_t numberOfSteps = 0;
        for (uint32_t cycle : fronts[end])
            numberOfSteps += stepsFrom(end, cycle).size();
        return numberOfSteps;
    };
    while (!fronts[down].isEmpty() && !fronts[up].isEmpty()) {
        unsigned end = numberOfStepsFrom(down) <= numberOfStepsFrom(up) ? down : up;
        nextFront.shrink(0);
        for (uint32_t cycle : fronts[end]) {
            for (uint32_t next : stepsFrom(end, cycle)) {
                if (seen[end ^ 1].contains(next))
                    return true;
                if (rankOfCycle[next] <= lowestRank || rankOfCycle[next] >= highestRank)
                    continue;
                if (seen[end].add(next).isNewEntry)
                    nextFront.append(next);
            }
        }
        std::swap(fronts[end], nextFront);
    }
    return false;
}

const LinkedCallThatFindsBuiltin* LinkTables::findCallThatFindsBuiltin(FunctionID caller, ValueID site) const
{
    if (!callersThatFindBuiltins.get(caller.index()))
        return nullptr;
    auto found = std::ranges::lower_bound(callsThatFindBuiltins, std::pair { caller, site }, { }, [](const LinkedCallThatFindsBuiltin& call) {
        return std::pair { call.caller, call.site };
    });
    return found != callsThatFindBuiltins.end() && found->caller == caller && found->site == site ? &*found : nullptr;
}

FunctionID LinkTables::calleeAt(FunctionID caller, ValueID site) const
{
    // Whoever fills `calls` fills the other: a call that is listed and not found would leave nothing where it has to leave something.
    RELEASE_ASSERT(firstCallOfCaller.last() == calls.size());
    if (caller.index() + 1 >= firstCallOfCaller.size())
        return { };
    uint32_t first = firstCallOfCaller[caller.index()];
    std::span<const LinkedCall> callsOfCaller = calls.span().subspan(first, firstCallOfCaller[caller.index() + 1] - first);
    auto found = std::ranges::lower_bound(callsOfCaller, site, { }, &LinkedCall::site);
    return found != callsOfCaller.end() && found->site == site ? found->callee : FunctionID();
}

void LinkTables::indexMembersByKey()
{
    membersByKey.resize(members.size());
    std::iota(membersByKey.begin(), membersByKey.end(), 0);
    std::ranges::sort(membersByKey, { }, [&](uint32_t place) {
        const LinkedMember& member = members[place];
        return std::tuple { member.classLayout, member.side, member.key, place };
    });
    for (BitVector& keys : keysOfMembers)
        keys.clearAll();
    for (const LinkedMember& member : members) {
        if (member.key)
            keysOfMembers[static_cast<size_t>(member.side)].set(member.key.index());
    }
}

std::span<const uint32_t> LinkTables::membersUnder(LayoutID classLayout, ClassSide side, PropertyKeyID key) const
{
    RELEASE_ASSERT(membersByKey.size() == members.size());
    auto found = std::ranges::equal_range(membersByKey, std::tuple { classLayout, side, key }, { }, [&](uint32_t place) {
        const LinkedMember& member = members[place];
        return std::tuple { member.classLayout, member.side, member.key };
    });
    return { found.begin(), found.end() };
}

bool classHolderIsMadeWithName(const Program& program, ClassSide side, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    switch (side) {
    case ClassSide::Prototype:
        return nameClass == NameClass::Constructor;
    case ClassSide::Static:
        return nameClass == NameClass::Length || nameClass == NameClass::Prototype || key == program.keyOf(BuiltinNames::name);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const LinkedNameOnFunction* LinkTables::findNameOnFunction(FunctionID holder, PropertyKeyID key) const
{
    auto placeOf = [](const LinkedNameOnFunction& laid) {
        return std::pair { laid.holder, laid.key };
    };
    auto found = std::ranges::lower_bound(namesOnFunctions, std::pair { holder, key }, { }, placeOf);
    return found != namesOnFunctions.end() && found->holder == holder && found->key == key ? &*found : nullptr;
}

bool LinkTables::closuresLackName(FunctionID function, PropertyKeyID key) const
{
    return function.index() < functions.size() && functions[function.index()].closuresGetOnlyNamesThatAreListed && !findNameOnFunction(function, key);
}

FunctionID LinkTables::findMethodLaidAheadOfObjectsOf(const Program& program, LayoutID layout, PropertyKeyID key) const
{
    if (!key || std::ranges::contains(appendedKeysOf(layout), key))
        return { };
    auto placeOf = [](const LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    };
    size_t numberOfLayoutsLeft = program.layouts.size();
    for (LayoutID current = layout; current && numberOfLayoutsLeft--; current = program.at(current).base) {
        if (!program.at(current).has(LayoutFlags::IsConstructorFunction) || !layouts[current.index()].prototypeObjectIsSettledAheadOfEveryObject)
            return { };
        auto laidHere = std::ranges::equal_range(namesOnPrototypes, std::pair { current, key }, { }, placeOf);
        if (!laidHere.empty())
            return laidHere.size() == 1 && laidHere.front().isLaidAheadOfEveryObject ? laidHere.front().function : FunctionID();
        if (!layouts[current.index()].isLinkedBelowBaseAheadOfEveryObject)
            return { };
    }
    return { };
}

bool LinkTables::findFunctionsLaidAbove(const Program& program, LayoutID layout, PropertyKeyID key, Vector<FunctionID, 4>& functions) const
{
    if (!key || layout.index() >= layouts.size() || !layouts[layout.index()].listsEveryMemberAbove)
        return false;
    auto placeOf = [](const LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    };
    // A row that comes round lists not every member.
    for (LayoutID current = layout; current; current = program.at(current).base) {
        if (program.at(current).has(LayoutFlags::IsConstructorFunction)) {
            if (std::ranges::binary_search(namesOnPrototypes, std::pair { current, PropertyKeyID() }, { }, placeOf))
                return false;
            for (const LinkedNameOnPrototype& laid : std::ranges::equal_range(namesOnPrototypes, std::pair { current, key }, { }, placeOf)) {
                if (!laid.function)
                    return false;
                functions.append(laid.function);
            }
            continue;
        }
        std::span<const uint32_t> places = membersUnder(current, ClassSide::Prototype, key);
        // The lookup may come to what the prototype object was made with, which is no member.
        if (places.empty() && classHolderIsMadeWithName(program, ClassSide::Prototype, key))
            return false;
        for (uint32_t place : places) {
            const LinkedMember& member = members[place];
            if (member.kind != MethodKind::Method || !member.everyWriterIsKnown || !member.function)
                return false;
            functions.append(member.function);
        }
    }
    return true;
}

PropertyKeyID LinkTables::keyAt(FunctionID function, ValueID site) const
{
    auto placeOf = [](const LinkedAccess& access) {
        return std::pair { access.function, access.site };
    };
    auto found = std::ranges::lower_bound(accessesUnderOneKey, std::pair { function, site }, { }, placeOf);
    return found != accessesUnderOneKey.end() && found->function == function && found->site == site ? found->key : PropertyKeyID();
}

const LinkedContentsAtOperation* LinkTables::contentsAtOperation(FunctionID function, ValueID site) const
{
    auto placeOf = [](const LinkedContentsAtOperation& contents) {
        return std::pair { contents.function, contents.site };
    };
    auto found = std::ranges::lower_bound(contentsAtOperations, std::pair { function, site }, { }, placeOf);
    return found != contentsAtOperations.end() && found->function == function && found->site == site ? &*found : nullptr;
}

TypeID LinkTables::resultOfReadByKey(FunctionID function, ValueID site) const
{
    auto placeOf = [](const LinkedResultOfRead& read) {
        return std::pair { read.function, read.site };
    };
    auto found = std::ranges::lower_bound(resultsOfReadsByKey, std::pair { function, site }, { }, placeOf);
    return found != resultsOfReadsByKey.end() && found->function == function && found->site == site ? found->proven : TypeID();
}

FunctionID LinkTables::getterExpectedAt(FunctionID function, ValueID site) const
{
    auto placeOf = [](const LinkedGetterAtRead& read) {
        return std::pair { read.function, read.site };
    };
    auto found = std::ranges::lower_bound(gettersExpectedAtReads, std::pair { function, site }, { }, placeOf);
    return found != gettersExpectedAtReads.end() && found->function == function && found->site == site ? found->getter : FunctionID();
}

bool isSymbolOfLibrary(const Program& program, PropertyKeyID symbol)
{
    const PropertyKey& key = program.at(symbol);
    if (key.kind != PropertyKeyKind::CreatedSymbol || !key.module)
        return false;
    const Module& creator = program.at(key.module);
    return creator.kind == ModuleKind::RuntimeSource || creator.flags.contains(ModuleFlags::IsLibrary);
}

bool isNameInTextOf(const Program& program, PropertyKeyID symbol, FunctionID function)
{
    const PropertyKey& key = program.at(symbol);
    if (key.kind != PropertyKeyKind::CreatedSymbol)
        return true;
    if (!key.module)
        return false;
    return !isSymbolOfLibrary(program, symbol) || isLibraryCode(program, program.at(function));
}

MemberFound LinkTables::findMember(const Program& program, LayoutID layout, ClassSide side, PropertyKeyID key) const
{
    if (!key || layout.index() >= layouts.size() || !(side == ClassSide::Prototype ? layouts[layout.index()].listsEveryMemberAbove : layouts[layout.index()].listsEveryStaticMember))
        return { };

    auto isLaidOnPrototypeOf = [&](LayoutID function, PropertyKeyID name) {
        auto placeOf = [](const LinkedNameOnPrototype& laid) {
            return std::pair { laid.layout, laid.key };
        };
        return std::ranges::binary_search(namesOnPrototypes, std::pair { function, name }, { }, placeOf);
    };
    bool hasPassedConstructorFunction = false;
    // As membersUnder() stops the build: whoever has changed `members` has indexed them.
    RELEASE_ASSERT(membersByKey.size() == members.size());
    bool someClassHasMemberUnderKey = keysOfMembers[static_cast<size_t>(side)].get(key.index());
    // A constructor is copied into the constructors below it, so each store to `this` of a row of classes is asked about once for each class below, and each time for a longer row.
    if (!someClassHasMemberUnderKey && !layouts[layout.index()].hasConstructorFunctionInRow)
        return classHolderIsMadeWithName(program, side, key) ? MemberFound { } : MemberFound { MemberFound::Kind::Nothing, { }, { }, { } };
    std::optional<bool> holderIsMadeWithName;
    // A row that comes round lists not every member.
    for (LayoutID current = layout; current; current = program.at(current).base) {
        if (side == ClassSide::Prototype && program.at(current).has(LayoutFlags::IsConstructorFunction)) {
            if (isLaidOnPrototypeOf(current, key) || isLaidOnPrototypeOf(current, { }))
                return { };
            hasPassedConstructorFunction = true;
            continue;
        }
        MemberFound found;
        unsigned numberFound = 0;
        for (uint32_t place : someClassHasMemberUnderKey ? membersUnder(current, side, key) : std::span<const uint32_t> { }) {
            const LinkedMember& member = members[place];
            if (!member.everyWriterIsKnown || !member.function)
                return { };
            ++numberFound;
            switch (member.kind) {
            case MethodKind::Method:
                found.method = member.function;
                break;
            case MethodKind::Getter:
                found.getter = member.function;
                break;
            case MethodKind::Setter:
                found.setter = member.function;
                break;
            }
        }
        if (!numberFound) {
            // LinkFlow::membersFoundUnder(): what the holder was made with answers, and no kind of MemberFound says so.
            if (!holderIsMadeWithName)
                holderIsMadeWithName = classHolderIsMadeWithName(program, side, key);
            if (*holderIsMadeWithName)
                return { };
            continue;
        }
        if (hasPassedConstructorFunction)
            return { };
        // Of two definitions under one key the later one stands, or both halves of one accessor.
        if (numberFound != static_cast<unsigned>(!!found.method) + static_cast<unsigned>(!!found.getter) + static_cast<unsigned>(!!found.setter) || (found.method && numberFound > 1))
            return { };
        found.kind = found.method ? MemberFound::Kind::Method : MemberFound::Kind::Accessor;
        return found;
    }
    return { MemberFound::Kind::Nothing, { }, { }, { } };
}

LinkTables conservativeLinkTables(const Program& program)
{
    LinkTables tables;
    size_t numberOfFunctions = program.functions.size();
    size_t numberOfLayouts = program.layouts.size();

    tables.functions.fill(LinkedFunction { }, numberOfFunctions);
    tables.firstCallOfCaller.fill(0, numberOfFunctions + 1);
    tables.firstCaptureOfFunction.reserveInitialCapacity(numberOfFunctions + 1);
    uint32_t numberOfParameters = 0;
    uint32_t numberOfCaptures = 0;
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        const Function& function = program.functions[index];
        LinkedFunction& linked = tables.functions[index];
        linked.firstParameter = numberOfParameters;
        linked.numberOfParameters = function.numberOfEntryParameters();
        numberOfParameters += linked.numberOfParameters;
        tables.firstCaptureOfFunction.append(numberOfCaptures);
        numberOfCaptures += static_cast<uint32_t>(function.captures.size());
    }
    tables.firstCaptureOfFunction.append(numberOfCaptures);
    tables.parameters.fill(LinkedParameter { }, numberOfParameters);
    tables.captures.fill(LinkedCell { }, numberOfCaptures);
    tables.firstReferrer.fill(0, numberOfFunctions + 1);
    tables.rankOfCycle.fill(0, 1);
    tables.firstCycleAwaited.fill(0, 2);
    tables.firstCycleNamed.fill(0, 2);

    tables.firstFieldOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    uint32_t numberOfFields = 0;
    for (const Layout& layout : program.layouts) {
        tables.firstFieldOfLayout.append(numberOfFields);
        numberOfFields += static_cast<uint32_t>(layout.fields.size());
    }
    tables.firstFieldOfLayout.append(numberOfFields);
    tables.fields.fill(LinkedCell { }, numberOfFields);
    tables.variables.fill(LinkedCell { }, program.variables.size());
    tables.layouts.fill(LinkedLayout { }, numberOfLayouts);
    return tables;
}

// An accessor runs at a read or a store, an initializer at a construction, the body of a module when the loader says so. No call of a summary stands for that.
static bool isEnteredByCallsAloneForLink(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return true;
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class WriterOfLink {
    WTF_MAKE_NONCOPYABLE(WriterOfLink);
public:
    WriterOfLink(const LinkFlow&, LinkTables&);

    // In this order. Only the second makes types, and the third is for linked() alone.
    void writeWhatMakesNoType();
    void writeWhatMakesTypes(TypeArena&);
    void writeProofsThatMakeNoType();
    // Where it goes on in another lane than it began in.
    void goOnWith(const ParallelLoops& loops) { m_loops = &loops; }
    // Behind writeLayoutsAndCells(): whoever is handed a type by the tables may name what the type names.
    void writeReferrers();

private:
    static constexpr uint32_t none = UINT32_MAX;

    struct SiteOfCall {
        ValueID site;
        FunctionID callee;
        bool isInTailPosition;
        bool mayComeAgain;
    };

    struct FunctionUnderKey {
        PropertyKeyID key;
        FunctionID function;

        friend constexpr auto operator<=>(const FunctionUnderKey&, const FunctionUnderKey&) = default;
    };

    struct FunctionOfClass {
        LayoutID classLayout;
        FunctionID function;

        friend constexpr auto operator<=>(const FunctionOfClass&, const FunctionOfClass&) = default;
    };

    void findFunctionsThatNoSummaryNames();
    void findLayoutsWithUnlistedNames();
    bool mayFindUnlistedNames(const LinkType& receiver) const;
    FunctionID oneCalleeOf(FunctionID caller, const SummaryCall&, const LinkFlow::CalleesOfCall&) const;
    void countCallByName(FunctionID caller, const SummaryCall&, const LinkFlow::CalleesOfCall&, CallsByNameInLink&) const;
    void findRingsOfTailCalls();
    void findFunctionsThatRunAtMostOnce();
    void countCallsByName();
    uint32_t weighAsLeaf(FunctionID, unsigned depth, bool& levelsRanOut);
    void weighLeaves();
    BitVector m_functionsThatAreNoLeaves;
    void writeCalls();
    void writeAccessesUnderOneKey();
    void writeGettersExpectedAtReads();
    void writeCallsThatFindBuiltins();
    void noteCallersThatFindBuiltins();
    void writeResultsOfReadsByKey();
    void writeContentsAtOperations();
    void writeReceiversThatStayAsMade();
    void writeFunctions();
    // AOTLinkExceptionsInlines.h.
    void writeCatches();
    void writeParameters(FunctionID, const LinkedFunction&);
    void indexFunctionsThatAreHeld();
    void makeGroups();

    TypeID unlessAnything(TypeID) const;
    TypeID widened(TypeID);
    // Null: nothing arrives, or anything may.
    bool writesWhatIsExpected() const { return m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls; }
    // As CalleeSummarizerForLinkEffects::instancesAreAsClassMakesThem(): whatever lays a name on such an object, defines an element by a descriptor or puts another object above it has left something under unknown names, or has let the object go.
    bool holdsOnlyInstancesThatStayAsMade(const LinkType& type) const
    {
        if (type.classes != LinkType::Classes::Site || type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.holdsUnseen || type.holdsLostBuiltins || type.builtin)
            return false;
        if (m_flow.hasEscaped(type.site))
            return false;
        FlowNodeID underUnknownNames = m_flow.findContentsOf(type, SlotOfContents::underUnknownNames());
        return !underUnknownNames || (underUnknownNames != m_flow.unseen() && m_flow.typeAt(underUnknownNames).isNothing());
    }
    TypeID typeIDOf(const LinkType&);
    TypeID typeIDOfCapture(FunctionID, uint32_t capture);

    void noteHolder(PropertyKeyID, LayoutID classLayout, FunctionID);
    // Null: none, or nobody says which.
    FunctionID closureAloneAt(FlowNodeID node) const { return m_flow.typeAt(node).functions == LinkType::Functions::Closure ? m_flow.typeAt(node).function : FunctionID(); }
    bool mayTakeArrivals(FunctionID id) const { return m_tables.function(id).everyCallerIsKnown && m_tables.function(id).everyCallHasOneCallee; }
    FunctionID addGroup(LinkedGroup::Kind, uint32_t subject = 0);
    // Each names nodes of LinkTables::referrers: functions and groups. A null id is passed over by whoever is told.
    void forEachNodeArrivingAt(FlowNodeID, const Invocable<void(FunctionID)> auto&) const;
    void forEachFunctionOfType(TypeID, const Invocable<void(FunctionID)> auto&) const;
    void forEachNodeNamedBy(FunctionID referrer, const Invocable<void(FunctionID)> auto&) const;
    void forEachFunctionListedWhateverCameFirst(const LinkFlow::CalleesOfCall&, const Invocable<void(FunctionID)> auto&) const;
    void forEachFunctionNamedBySharedCall(uint32_t shared, const Invocable<void(FunctionID)> auto&) const;
    void forEachMemberOf(const LinkedGroup&, const Invocable<void(FunctionID)> auto&) const;

    const LinkFlow& m_flow;
    const Program& m_program;
    const ParallelLoops& loops() const { return m_loops ? *m_loops : m_flow.loopsOfReading(); }
    const ParallelLoops* m_loops { nullptr };
    // While writeWhatMakesTypes() runs.
    TypeArena* m_typesAtHand { nullptr };
    // AOTLinkTypesOnAllThreads.h.
    static thread_local TypesOfThreadInLink s_typesOfThread;
    TypeArena& typesAtHand() const { return s_typesOfThread.types ? *s_typesOfThread.types : *m_typesAtHand; }
    TypesOfFlowTurnedOnce& typesTurned() { return s_typesOfThread.turned ? *s_typesOfThread.turned : m_typesTurned; }
    void writeFunction(FunctionID);
    TypesOfFlowTurnedOnce m_typesTurned;
    LinkTables& m_tables;
    uint32_t m_numberOfFunctions;

    BitVector m_functionsEnteredByHost;
    // The code of the library behind a built-in, and what has no body: the walk names them where no summary shows it.
    BitVector m_functionsThatNoSummaryNames;
    // A name on some object of the layout, or of a layout of the family, may find what neither a field nor a member lists.
    BitVector m_layoutsWithUnlistedNames;
    BitVector m_familiesWithUnlistedNames;
    bool m_lostLayoutsHaveUnlistedNames { false };
    // Some call that may enter the function is not in LinkTables::calls.
    BitVector m_functionsSharingCalls;

    // Whose closures alone lie in a field, a static field or a member. Ascending, without repeats.
    Vector<FunctionUnderKey> m_functionsUnderKeys;
    // And whose closures the class was given.
    Vector<FunctionOfClass> m_functionsOfClasses;
    Vector<FunctionOfClass> m_functionsThatSlotsOfRecordsMayDefer;

    // The nodes of the groups. Null: there is no such group, because it would have no member or nobody to name it.
    FunctionID m_anybody;
    // By family, and by shared call.
    Vector<FunctionID> m_groupOfConstructors;
    Vector<FunctionID> m_groupOfSharedCall;
    // By LinkedGroup::subject.
    Vector<uint32_t> m_sharedCallsOfGroups;
};

WriterOfLink::WriterOfLink(const LinkFlow& flow, LinkTables& tables)
    : m_flow(flow)
    , m_program(flow.program)
    , m_tables(tables)
    , m_numberOfFunctions(static_cast<uint32_t>(flow.program.functions.size()))
{
    RELEASE_ASSERT(tables.functions.size() == m_numberOfFunctions && flow.summaries.size() == m_numberOfFunctions);
    RELEASE_ASSERT(flow.firstCallOfFunction.size() == m_numberOfFunctions + 1 && flow.calleesOfCalls.size() == flow.firstCallOfFunction.last());
}

void WriterOfLink::writeWhatMakesNoType()
{
    findFunctionsThatNoSummaryNames();
    findLayoutsWithUnlistedNames();
    writeCalls();
    writeAccessesUnderOneKey();
    writeGettersExpectedAtReads();
    writeCallsThatFindBuiltins();
    noteCallersThatFindBuiltins();
    countCallsByName();
}

void WriterOfLink::writeWhatMakesTypes(TypeArena& types)
{
    SetForScope typesOfTurn(m_typesAtHand, &types);
    writeFunctions();
    writeCoroutinesOfLink(m_program, m_flow, types, m_tables);
    writeCatches();
    // In Facts::linked() a proof. In Facts::held() what was seen to come out, whatever else unseen code may have put in: a guess, which is tested at each use.
    writeResultsOfReadsByKey();
    writeContentsAtOperations();
}

// Proofs, which nobody asks Facts::held() for: the walk asks Facts::linked() alone.
void WriterOfLink::writeProofsThatMakeNoType()
{
    RELEASE_ASSERT(!writesWhatIsExpected());
    weighLeaves();
    writeReceiversThatStayAsMade();
    findRingsOfTailCalls();
    findFunctionsThatRunAtMostOnce();
}

void WriterOfLink::findFunctionsThatNoSummaryNames()
{
    m_functionsEnteredByHost = functionsCalledByHost(m_program, loops());
    m_functionsThatNoSummaryNames = m_flow.libraryFunctionsEntered;
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_program.functions[index].hasBody())
            m_functionsThatNoSummaryNames.quickSet(index);
    }
}

void WriterOfLink::findLayoutsWithUnlistedNames()
{
    static constexpr OptionSet<LayoutChange> changesThatHideMembers { LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor };
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    m_layoutsWithUnlistedNames = BitVector(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        bool hasChanged = m_flow.possibleChangesOf(index).containsAny(changesThatHideMembers);
        if (hasChanged || m_flow.escapedLayoutsAsRead().get(index))
            m_layoutsWithUnlistedNames.quickSet(index);
    }

    m_familiesWithUnlistedNames = m_flow.families.familiesWithSomeMemberThat([&](LayoutID member) {
        return m_layoutsWithUnlistedNames.quickGet(member.index());
    });

    for (size_t index : m_flow.lostLayouts)
        m_lostLayoutsHaveUnlistedNames |= m_layoutsWithUnlistedNames.quickGet(index);
}

// The flow tells a call of each of these when it happens, and the call then may enter unseen code. Here the same is asked of how things stand at the end.
bool WriterOfLink::mayFindUnlistedNames(const LinkType& receiver) const
{
    if (receiver.holdsUnseen || receiver.builtin || receiver.classes != LinkType::Classes::None)
        return true;

    switch (receiver.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        if (m_flow.escapedFunctions.get(receiver.function.index()) || m_flow.lostFunctions.get(receiver.function.index()))
            return true;
        break;
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Lost:
        return true;
    }

    switch (receiver.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Prototype:
        return m_layoutsWithUnlistedNames.get(receiver.layout.index());
    case LinkType::Objects::Family:
    case LinkType::Objects::Prototypes:
        return m_familiesWithUnlistedNames.get(receiver.family.index());
    case LinkType::Objects::Lost:
        return m_lostLayoutsHaveUnlistedNames;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// It says who is entered, not that anybody is: what is called may be no function, and the call of that throws.
FunctionID WriterOfLink::oneCalleeOf(FunctionID caller, const SummaryCall& call, const LinkFlow::CalleesOfCall& callees) const
{
    if (!callees.builtins.isEmpty() || callees.mayEnterLost || (callees.mayEnterUnseen && !writesWhatIsExpected()))
        return { };
    // All that the call lists, itself and by the shared calls that it goes by, is one function.
    FunctionID callee = callees.listed.isEmpty() ? FunctionID() : callees.listed[0];
    bool listsSeveral = callees.listed.size() > 1;
    for (uint32_t shared : callees.shared) {
        const Vector<FunctionID, 1>& listed = m_flow.calleesOfSharedCallsAsSettled()[shared].listed;
        listsSeveral |= listed.size() > 1 || (listed.size() == 1 && callee && callee != listed[0]);
        if (!listed.isEmpty())
            callee = listed[0];
    }
    if (!callee || listsSeveral || !m_flow.tablesMayName(callee))
        return { };
    const Function& function = m_program.at(callee);
    if (!function.hasBody())
        return { };
    // Node 0 holds anything in every summary.
    auto typeAt = [&](uint32_t node) -> const LinkType* {
        return node && node != noSummaryNode ? &m_flow.typeAt(m_flow.nodeOf(caller, node)) : nullptr;
    };

    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        return callee;
    // A getter that the lookup runs is listed too, and it is not what is called.
    case SummaryCall::Callee::Name: {
        const LinkType* receiver = typeAt(call.receiver);
        // What is proven may not hang on a name that no table lists. What is held is no proof: the call was seen to find ONE function under the name, and whoever goes by that tests what is called.
        if (!receiver || (!writesWhatIsExpected() && mayFindUnlistedNames(*receiver)))
            return { };
        return isEnteredByCallsAloneForLink(function.kind) ? callee : FunctionID();
    }
    case SummaryCall::Callee::Value: {
        const LinkType* subject = typeAt(call.subject);
        if (!subject)
            return { };
        LinkType called = *subject;
        called.primitives = 0;
        // What is named beside what is unseen is what was seen to arrive: whoever goes by what is held tests what is called.
        // By value a construction asks the run time for `this`. A plain call by value costs a few instructions more than the test and the direct call do: with those the application's build is 0.4 G dearer (11-03 21:40).
        if (writesWhatIsExpected() && call.isConstruction) {
            called.holdsUnseen = false;
            called.holdsLostBuiltins = false;
        }
        LinkType closuresOfCallee;
        closuresOfCallee.functions = LinkType::Functions::Closure;
        closuresOfCallee.function = callee;
        if (called != closuresOfCallee)
            return { };
        return isEnteredByCallsAloneForLink(function.kind) ? callee : FunctionID();
    }
    // What is listed there is what the native code calls.
    case SummaryCall::Callee::Builtin:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WriterOfLink::countCallByName(FunctionID caller, const SummaryCall& call, const LinkFlow::CalleesOfCall& callees, CallsByNameInLink& counts) const
{
    ++counts.all;
    bool isListed = !!m_tables.calleeAt(caller, call.site);
    LinkType receiver = call.receiver && call.receiver != noSummaryNode ? m_flow.typeAt(m_flow.nodeOf(caller, call.receiver)) : LinkType::anything();
    bool holdsObjects = receiver.objects != LinkType::Objects::None || receiver.functions != LinkType::Functions::None || receiver.classes != LinkType::Classes::None || receiver.builtin || receiver.holdsUnseen;

    size_t numberListed = callees.listed.size();
    for (uint32_t shared : callees.shared)
        numberListed += m_flow.calleesOfSharedCallsAsSettled()[shared].listed.size();

    uint64_t& count = isListed ? counts.listed
        : call.isMadeByProtocol ? counts.ofProtocols
        : receiver.isNothing() ? counts.nothingArrivesAtReceiver
        : !holdsObjects ? counts.onlyPrimitivesArrive
        : receiver.holdsUnseen ? counts.receiverHoldsUnseen
        : receiver.classes != LinkType::Classes::None || receiver.builtin ? counts.receiverIsOfEngine
        : receiver.objects == LinkType::Objects::Lost || receiver.functions == LinkType::Functions::Lost ? counts.receiverIsLost
        : callees.mayEnterUnseen ? counts.mayEnterUnseen
        : callees.mayEnterLost ? counts.mayEnterLost
        : !callees.builtins.isEmpty() ? counts.entersBuiltin
        : !numberListed ? counts.findsNoFunction
        : numberListed > 1 ? counts.findsSeveralFunctions
        : mayFindUnlistedNames(receiver) ? counts.mayFindUnlistedNames
        : !oneCalleeOf(caller, call, callees) ? counts.findsWhatNoCallEnters
        : counts.sharesItsSite;
    ++count;
}

// An answer that is a weight needed no more levels than were left, so it is the same at every depth, and so is a no that no want of levels has a part in: both are kept. A no because the levels ran out is kept at the first level alone.
// So the tables hang on no order.
uint32_t WriterOfLink::weighAsLeaf(FunctionID id, unsigned depth, bool& levelsRanOut)
{
    static constexpr unsigned levels = 3;
    // A larger body is copied by no rule.
    static constexpr uint32_t mostOperationsOfLeaf = 100;
    LinkedFunction& linked = m_tables.functions[id.index()];
    if (linked.weightAsLeaf != LinkedFunction::notLeaf)
        return linked.weightAsLeaf;
    if (m_functionsThatAreNoLeaves.quickGet(id.index()))
        return LinkedFunction::notLeaf;
    const ThinSummary& summary = m_flow.summaries.at(id);
    bool ranOutBelow = false;
    uint32_t weight = summary.weightOfOperations;
    if (!m_flow.reached.get(id.index()) || !m_program.at(id).hasBody() || summary.has(SummaryTrait::HasLoops) || summary.numberOfOperations > mostOperationsOfLeaf)
        weight = LinkedFunction::notLeaf;
    for (const SummaryCall& call : summary.calls) {
        if (weight == LinkedFunction::notLeaf)
            break;
        if (call.callee == SummaryCall::Callee::Builtin || call.isMadeByProtocol)
            continue;
        FunctionID callee = call.isConstruction ? FunctionID() : m_tables.calleeAt(id, call.site);
        if (callee && callee != id && depth == levels) {
            ranOutBelow = true;
            weight = LinkedFunction::notLeaf;
            break;
        }
        uint32_t more = callee && callee != id ? weighAsLeaf(callee, depth + 1, ranOutBelow) : LinkedFunction::notLeaf;
        weight = more == LinkedFunction::notLeaf ? LinkedFunction::notLeaf : weight + more;
    }
    if (weight != LinkedFunction::notLeaf)
        linked.weightAsLeaf = weight;
    else if (!ranOutBelow || !depth)
        m_functionsThatAreNoLeaves.quickSet(id.index());
    levelsRanOut |= ranOutBelow;
    return weight;
}

void WriterOfLink::weighLeaves()
{
    m_functionsThatAreNoLeaves = BitVector(m_numberOfFunctions);
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        bool levelsRanOut = false;
        weighAsLeaf(FunctionID(index), 0, levelsRanOut);
    }
}

void WriterOfLink::countCallsByName()
{
    static_assert(!(sizeof(CallsByNameInLink) % sizeof(uint64_t)));
    Lock lock;
    LinkFlow::ViewOfHeldReading* view = LinkFlow::ViewAtWork::current();
    forEachRangeOfIndicesInParallel(loops(), m_numberOfFunctions, [&](size_t first, size_t end) {
        LinkFlow::ViewAtWork viewAtWork(view);
        CallsByNameInLink counts;
        for (size_t index = first; index < end; ++index) {
            if (!m_flow.reached.get(index))
                continue;
            FunctionID caller(static_cast<uint32_t>(index));
            const ThinSummary& summary = m_flow.summaries.at(caller);
            for (uint32_t call = 0; call < summary.calls.size(); ++call) {
                if (summary.calls[call].callee == SummaryCall::Callee::Name)
                    countCallByName(caller, summary.calls[call], m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[index] + call], counts);
            }
        }
        Locker locker { lock };
        std::span<uint64_t> sums { reinterpret_cast<uint64_t*>(&m_tables.callsByName), sizeof(CallsByNameInLink) / sizeof(uint64_t) };
        std::span<const uint64_t> added { reinterpret_cast<const uint64_t*>(&counts), sums.size() };
        for (size_t place = 0; place < sums.size(); ++place)
            sums[place] += added[place];
    });
}

void WriterOfLink::writeCalls()
{
    // A row without a caller: some call that may enter the callee is not listed.
    Vector<LinkedCall> found = collectInParallel<LinkedCall>(loops(), m_numberOfFunctions, [&, view = LinkFlow::ViewAtWork::current()](unsigned index, Vector<LinkedCall>& calls) {
        LinkFlow::ViewAtWork viewAtWork(view);
        if (!m_flow.reached.get(index))
            return;
        auto noteSharing = [&](FunctionID callee) {
            calls.append(LinkedCall { FunctionID(), ValueID(), callee });
        };
        // And without a callee: the same of all that the shared call of that number lists.
        auto noteSharingOfSharedCall = [&](uint32_t shared) {
            calls.append(LinkedCall { FunctionID(), ValueID(shared), FunctionID() });
        };
        FunctionID caller(index);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        uint32_t firstCall = m_flow.firstCallOfFunction[index];
        RELEASE_ASSERT(m_flow.firstCallOfFunction[index + 1] - firstCall == summary.calls.size());

        Vector<SiteOfCall, 32> sites;
        for (uint32_t call = 0; call < summary.calls.size(); ++call) {
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[firstCall + call];
            FunctionID callee = oneCalleeOf(caller, summary.calls[call], callees);
            sites.append(SiteOfCall { summary.calls[call].site, callee, summary.calls[call].isInTailPosition, summary.calls[call].isInLoop || summary.calls[call].isMadeByProtocol });
            if (callee)
                continue;
            for (FunctionID listed : callees.listed)
                noteSharing(listed);
            for (uint32_t shared : callees.shared)
                noteSharingOfSharedCall(shared);
        }
        std::ranges::sort(sites, { }, &SiteOfCall::site);

        // calleeAt() answers for a site, and the walk leaves there what one call passes. An operation that looks up several names, or one name on several operands, has a call for each.
        size_t end = 0;
        for (size_t first = 0; first < sites.size(); first = end) {
            SiteOfCall leader = sites[first];
            end = first + 1;
            while (end < sites.size() && sites[end].site == leader.site)
                ++end;
            if (leader.site && leader.callee && end == first + 1) {
                calls.append(LinkedCall { caller, leader.site, leader.callee, leader.isInTailPosition, leader.mayComeAgain });
                continue;
            }
            for (size_t place = first; place < end; ++place) {
                if (sites[place].callee)
                    noteSharing(sites[place].callee);
            }
        }
    });

    m_functionsSharingCalls = BitVector(m_numberOfFunctions);
    BitVector sharedCallsOfCallsNotListed(m_flow.calleesOfSharedCallsAsSettled().size());
    for (const LinkedCall& call : found) {
        if (call.caller)
            continue;
        if (call.callee)
            m_functionsSharingCalls.quickSet(call.callee.index());
        else
            sharedCallsOfCallsNotListed.quickSet(call.site.index());
    }
    for (size_t shared : sharedCallsOfCallsNotListed) {
        for (FunctionID listed : m_flow.calleesOfSharedCallsAsSettled()[shared].listed)
            m_functionsSharingCalls.quickSet(listed.index());
    }
    found.removeAllMatching([](const LinkedCall& call) {
        return !call.caller;
    });
    m_tables.calls = WTF::move(found);

    m_tables.firstCallOfCaller.fill(0, m_numberOfFunctions + 1);
    for (const LinkedCall& call : m_tables.calls)
        ++m_tables.firstCallOfCaller[call.caller.index() + 1];
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index)
        m_tables.firstCallOfCaller[index + 1] += m_tables.firstCallOfCaller[index];
}

// One operation leaves one access with a node for its key.
void WriterOfLink::writeAccessesUnderOneKey()
{
    m_tables.accessesUnderOneKey = collectInParallel<LinkedAccess>(loops(), m_numberOfFunctions, [&, view = LinkFlow::ViewAtWork::current()](unsigned index, Vector<LinkedAccess>& accesses) {
        LinkFlow::ViewAtWork viewAtWork(view);
        if (!m_flow.reached.get(index))
            return;
        FunctionID function(index);
        size_t first = accesses.size();
        for (const SummaryAccess& access : m_flow.summaries.at(function).accesses) {
            if (access.key || !access.site)
                continue;
            if (PropertyKeyID key = m_flow.keyOf(function, access))
                accesses.append(LinkedAccess { function, access.site, key });
        }
        for (const ThinSummary::AccessUnderConstantKey& access : m_flow.summaries.at(function).accessesUnderConstantKeys)
            accesses.append(LinkedAccess { function, access.site, access.key });
        std::ranges::sort(accesses.mutableSpan().subspan(first));
    });
}

// From what lies at the nodes when the flow has ended: no observer, and nothing hangs on the order of the worklist.
// The type at the field stops naming a built-in where it meets what is unseen, so once the record has escaped nothing that is held shows it. What the call has found while it was named is kept with the call.
void WriterOfLink::writeCallsThatFindBuiltins()
{
    if (!writesWhatIsExpected())
        return;
    m_tables.callsThatFindBuiltins = collectInParallel<LinkedCallThatFindsBuiltin>(loops(), m_numberOfFunctions, [&, view = LinkFlow::ViewAtWork::current()](unsigned index, Vector<LinkedCallThatFindsBuiltin>& calls) {
        LinkFlow::ViewAtWork viewAtWork(view);
        if (!m_flow.reached.get(index))
            return;
        FunctionID caller(index);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        uint32_t firstCall = m_flow.firstCallOfFunction[index];
        size_t first = calls.size();
        for (uint32_t call = 0; call < summary.calls.size(); ++call) {
            // Nearly every call says no here.
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[firstCall + call];
            if (callees.builtins.size() != 1 || callees.listed.size() > 1 || !callees.shared.isEmpty() || callees.mayEnterLost)
                continue;
            const SummaryCall& record = summary.calls[call];
            if (record.callee != SummaryCall::Callee::Name || record.isMadeByProtocol || record.isConstruction || record.passesList || !record.site || !record.key || !record.receiver || record.receiver == noSummaryNode)
                continue;
            // What lies above the objects of the program, and what the engine's own objects answer to a name, the rows of the classes say.
            const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(caller, record.receiver));
            if (receiver.objects != LinkType::Objects::Layout || receiver.functions != LinkType::Functions::None || receiver.classes != LinkType::Classes::None || receiver.builtin || !findField(m_program, receiver.layout, record.key))
                continue;
            FunctionID callee = callees.listed.isEmpty() ? FunctionID() : callees.listed[0];
            if (callee && (!m_flow.tablesMayName(callee) || !m_program.at(callee).hasBody() || !isEnteredByCallsAloneForLink(m_program.at(callee).kind)))
                continue;
            calls.append(LinkedCallThatFindsBuiltin { caller, record.site, callees.builtins[0], callee });
        }
        std::ranges::sort(calls.mutableSpan().subspan(first));
        for (size_t place = first; place + 1 < calls.size();) {
            if (calls[place].site != calls[place + 1].site) {
                ++place;
                continue;
            }
            ValueID shared = calls[place].site;
            calls.removeAllMatching([&](const LinkedCallThatFindsBuiltin& listed) {
                return listed.caller == caller && listed.site == shared;
            }, first);
        }
    });
}

void WriterOfLink::noteCallersThatFindBuiltins()
{
    for (const LinkedCallThatFindsBuiltin& call : m_tables.callsThatFindBuiltins)
        m_tables.callersThatFindBuiltins.set(call.caller.index());
}

namespace {

// WHICH FUNCTION DESCRIPTORS LAY AS A GETTER (SummaryChange::getter), BY WHAT IT IS LAID ON AND BY THE NAME, read off the records when the flow has ended.
// `Object.defineProperty(holder, "area", { get: .. })` speaks of reads of `area` and of no other name. A definition under a key that nobody knows speaks of every name that no definition names.
// AN EXPECTATION: whoever goes by it looks the name up and tests what it finds.
class GettersLaidInLink {
public:
    // The family of an allocation site, or the closures of one function. Nothing: the type names something else, or several kinds of objects.
    static std::optional<uint32_t> holderOf(const LinkFlow& flow, const LinkType& type)
    {
        if (type.objects != LinkType::Objects::None || type.builtin)
            return std::nullopt;
        if (type.functions == LinkType::Functions::None && type.classes != LinkType::Classes::None)
            return flow.familyOf(type.site).index() << 1;
        if (type.functions == LinkType::Functions::Closure && type.classes == LinkType::Classes::None)
            return type.function.index() << 1 | 1;
        return std::nullopt;
    }

    // A null function: nobody can tell what is laid.
    void note(uint32_t holder, PropertyKeyID key, FunctionID getter)
    {
        if (key)
            m_keys.set(key.index());
        else
            m_someKeyIsUnknown = true;
        uint32_t laid = getter ? getter.index() : several;
        auto added = m_getters.add(placeOf(holder, key), laid);
        if (!added.isNewEntry && added.iterator->value != laid)
            added.iterator->value = several;
    }

    bool isEmpty() const { return m_getters.isEmpty(); }
    bool mayAnswer(PropertyKeyID key) const { return m_someKeyIsUnknown || m_keys.get(key.index()); }

    FunctionID find(uint32_t holder, PropertyKeyID key) const
    {
        auto found = m_keys.get(key.index()) ? m_getters.find(placeOf(holder, key)) : m_getters.end();
        if (found == m_getters.end() && m_someKeyIsUnknown)
            found = m_getters.find(placeOf(holder, { }));
        return found == m_getters.end() || found->value == several ? FunctionID() : FunctionID(found->value);
    }

private:
    static constexpr uint32_t several = UINT32_MAX;
    static uint64_t placeOf(uint32_t holder, PropertyKeyID key) { return static_cast<uint64_t>(holder) << 32 | (key ? key.index() : UINT32_MAX - 2); }

    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_getters;
    BitVector m_keys;
    bool m_someKeyIsUnknown { false };
};

} // anonymous namespace

void WriterOfLink::writeGettersExpectedAtReads()
{
    if (!writesWhatIsExpected())
        return;
    GettersLaidInLink laid;
    for (size_t index : m_flow.reached) {
        FunctionID function(static_cast<uint32_t>(index));
        for (const SummaryChange& change : m_flow.summaries.at(function).changes) {
            if (change.kind != SummaryChange::Kind::DefinesByDescriptor || change.getter == noSummaryNode || (change.key && liesInThread(change.key)))
                continue;
            std::optional<uint32_t> holder = GettersLaidInLink::holderOf(m_flow, m_flow.typeAt(m_flow.nodeOf(function, change.subject)));
            if (!holder)
                continue;
            // What was seen to arrive, whatever else unseen code may have laid: the object has reached it with the descriptor.
            const LinkType& getters = m_flow.typeAt(m_flow.nodeOf(function, change.getter));
            bool isOneFunction = getters.functions == LinkType::Functions::Closure && !getters.builtin && getters.objects == LinkType::Objects::None && getters.classes == LinkType::Classes::None;
            laid.note(*holder, change.key, isOneFunction && m_flow.reached.get(getters.function.index()) && m_program.at(getters.function).hasBody() ? getters.function : FunctionID());
        }
    }
    if (laid.isEmpty())
        return;
    m_tables.gettersExpectedAtReads = collectInParallel<LinkedGetterAtRead>(loops(), m_numberOfFunctions, [&, view = LinkFlow::ViewAtWork::current()](unsigned index, Vector<LinkedGetterAtRead>& reads) {
        LinkFlow::ViewAtWork viewAtWork(view);
        if (!m_flow.reached.get(index))
            return;
        FunctionID function(index);
        size_t first = reads.size();
        for (const SummaryAccess& access : m_flow.summaries.at(function).accesses) {
            if (access.kind != SummaryAccess::Kind::Read || !access.key || !access.site || access.isToContents() || access.receiver != noSummaryNode || liesInThread(access.key) || !laid.mayAnswer(access.key))
                continue;
            std::optional<uint32_t> holder = GettersLaidInLink::holderOf(m_flow, m_flow.typeAt(m_flow.nodeOf(function, access.subject)));
            if (FunctionID getter = holder ? laid.find(*holder, access.key) : FunctionID())
                reads.append(LinkedGetterAtRead { function, access.site, getter });
        }
        std::ranges::sort(reads.mutableSpan().subspan(first));
        // An operation that reads several names, or one name off several operands, has an access for each.
        for (size_t place = first; place + 1 < reads.size();) {
            if (reads[place].site != reads[place + 1].site) {
                ++place;
                continue;
            }
            ValueID shared = reads[place].site;
            reads.removeAllMatching([&](const LinkedGetterAtRead& read) {
                return read.function == function && read.site == shared;
            }, first);
        }
    });
}

void WriterOfLink::writeReceiversThatStayAsMade()
{
    const BuiltinTable& builtins = m_program.builtins();
    auto staysAsMade = [&](FunctionID function, uint32_t node) {
        const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(function, node));
        if (!holdsOnlyInstancesThatStayAsMade(receiver))
            return false;
        BuiltinClassID builtinClass = m_flow.classOf(receiver.site);
        return builtins.typedArrayTypeOf(builtinClass) == NotTypedArray && builtinClass != BuiltinClasses::DataView;
    };
    Vector<LinkedReceiverAsMade>& receivers = m_tables.receiversThatStayAsMade;
    Vector<ValueID, 16> sitesOfOtherReceivers;
    // ScanOfBody::openIterator(): whoever iterates calls the method under @@iterator on what is iterated over.
    PropertyKeyID keyOfIterators = m_program.keyOf(builtins.nameOf(WellKnownSymbol::Iterator));
    Vector<LinkedReceiverAsMade>& iterables = m_tables.iterablesThatStayAsMade;
    Vector<ValueID, 4> sitesOfOtherIterables;
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_flow.reached.get(index))
            continue;
        FunctionID function(index);
        const ThinSummary& summary = m_flow.summaries.at(function);
        size_t first = receivers.size();
        sitesOfOtherReceivers.shrink(0);
        size_t firstIterable = iterables.size();
        sitesOfOtherIterables.shrink(0);
        auto note = [&](ValueID site, uint32_t node) {
            if (staysAsMade(function, node))
                receivers.append(LinkedReceiverAsMade { function, site });
            else
                sitesOfOtherReceivers.append(site);
        };
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind == SummaryAccess::Kind::Read && access.site && !access.isToContents() && access.receiver == noSummaryNode)
                note(access.site, access.subject);
        }
        for (const SummaryCall& call : summary.calls) {
            if (call.callee != SummaryCall::Callee::Name || !call.site || call.receiver == noSummaryNode)
                continue;
            note(call.site, call.receiver);
            if (!call.isMadeByProtocol || call.key != keyOfIterators)
                continue;
            if (staysAsMade(function, call.receiver))
                iterables.append(LinkedReceiverAsMade { function, call.site });
            else
                sitesOfOtherIterables.append(call.site);
        }
        if (iterables.size() != firstIterable) {
            // An operation may iterate over several operands: each has to stay as made.
            iterables.removeAllMatching([&](const LinkedReceiverAsMade& iterable) {
                return sitesOfOtherIterables.contains(iterable.site);
            }, firstIterable);
            std::ranges::sort(iterables.mutableSpan().subspan(firstIterable));
            auto repeats = std::ranges::unique(iterables.mutableSpan().subspan(firstIterable));
            iterables.shrink(iterables.size() - repeats.size());
        }
        if (receivers.size() == first)
            continue;
        // An operation may read off several operands, and a protocol may look names up at the site of another operation: each has to stay as made.
        std::ranges::sort(sitesOfOtherReceivers);
        receivers.removeAllMatching([&](const LinkedReceiverAsMade& receiver) {
            return std::ranges::binary_search(sitesOfOtherReceivers, receiver.site);
        }, first);
        std::ranges::sort(receivers.mutableSpan().subspan(first));
        auto repeats = std::ranges::unique(receivers.mutableSpan().subspan(first));
        receivers.shrink(receivers.size() - repeats.size());
    }
}

// From what lies at the nodes when the flow has ended.
void WriterOfLink::writeResultsOfReadsByKey()
{
    // A hole and the place behind the end ask upward. The run time refuses every index on the prototypes, but for what a test run has opened: then nothing is said.
    if (!writesWhatIsExpected() && !m_program.openedNames.isEmpty())
        return;
    Vector<LinkedResultOfRead>& reads = m_tables.resultsOfReadsByKey;
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_flow.reached.get(index))
            continue;
        FunctionID function(index);
        size_t first = reads.size();
        for (const SummaryAccess& access : m_flow.summaries.at(function).accesses) {
            if (access.kind != SummaryAccess::Kind::Read || access.key || !access.site || access.isToContents() || access.value == noSummaryNode)
                continue;
            // Null where nothing has arrived, too: then the read may stand where nothing ever comes, and nobody gains.
            TypeID seen = typeIDOf(m_flow.typeAt(m_flow.nodeOf(function, access.value)));
            reads.append(LinkedResultOfRead { function, access.site, seen ? typesAtHand().withPrimitives(seen, Primitive::Undefined) : TypeID() });
        }
        std::ranges::sort(reads.mutableSpan().subspan(first), { }, &LinkedResultOfRead::site);
        // An operation that reads off several operands has an access for each: it gives what any of them gives, and nothing is said of it here.
        size_t kept = first;
        for (size_t place = first; place < reads.size();) {
            size_t end = place + 1;
            while (end < reads.size() && reads[end].site == reads[place].site)
                ++end;
            if (end == place + 1 && reads[place].proven)
                reads[kept++] = reads[place];
            place = end;
        }
        reads.shrink(kept);
    }
}

// From what lies at the nodes when the flow has ended.
void WriterOfLink::writeContentsAtOperations()
{
    if (writesWhatIsExpected())
        return;
    Vector<LinkedContentsAtOperation>& listed = m_tables.contentsAtOperations;
    auto heldIn = [&](const LinkType& receiver, ContentsSlot slot) -> TypeID {
        FlowNodeID node = m_flow.findContentsOf(receiver, SlotOfContents(slot));
        return node ? unlessAnything(typeIDOf(m_flow.typeAt(node))) : TypeID();
    };
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_flow.reached.get(index))
            continue;
        FunctionID function(index);
        size_t first = listed.size();
        for (const SummaryCall& call : m_flow.summaries.at(function).calls) {
            if (!call.site || call.receiver == noSummaryNode || !call.receiver)
                continue;
            const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(function, call.receiver));
            bool isMap = receiver.builtinClass == BuiltinClasses::Map || receiver.builtinClass == BuiltinClasses::WeakMap;
            if (receiver.classes != LinkType::Classes::Site || receiver.objects != LinkType::Objects::None || receiver.functions != LinkType::Functions::None || (!isMap && receiver.builtinClass != BuiltinClasses::Set))
                continue;
            if ((receiver.primitives & ~Primitive::UndefinedOrNull) || m_flow.hasEscaped(receiver.site))
                continue;
            LinkedContentsAtOperation contents { function, call.site, isMap ? heldIn(receiver, ContentsSlot::Key) : TypeID(), heldIn(receiver, isMap ? ContentsSlot::Value : ContentsSlot::Element) };
            if (contents.key || contents.value)
                listed.append(contents);
        }
        std::ranges::sort(listed.mutableSpan().subspan(first), { }, &LinkedContentsAtOperation::site);
        // Several calls at one site: a protocol beside a call. Nothing is said of it.
        size_t kept = first;
        for (size_t place = first; place < listed.size();) {
            size_t end = place + 1;
            while (end < listed.size() && listed[end].site == listed[place].site)
                ++end;
            if (end == place + 1)
                listed[kept++] = listed[place];
            place = end;
        }
        listed.shrink(kept);
    }
}

TypeID WriterOfLink::unlessAnything(TypeID type) const
{
    if (!type || holdsOnlyPrimitives(type))
        return type;
    return typesAtHand().at(type).isAnything() ? TypeID() : type;
}

TypeID WriterOfLink::widened(TypeID type)
{
    if (!type || isNeverType(type))
        return { };
    return holdsOnlyPrimitives(type) ? type : widenedToFamily(typesAtHand(), m_flow.families, type);
}

// What is held of a position that this file writes is an expectation, which is tested at each use: what was seen to arrive, whatever else unseen code may bring.
TypeID WriterOfLink::typeIDOf(const LinkType& type)
{
    if (type.isNothing() || withoutLostBuiltinsInLink(type).isAnything())
        return { };
    if (!writesWhatIsExpected())
        return unlessAnything(typesTurned().turn(m_flow, typesAtHand(), type));
    // Whoever tests for a function mentions its code: LinkTables::referrers of Facts::linked() leads to what either table names (layReferrersOfHeldReadingOver()).
    // It names no member of the table: the built-ins in reach are those of Facts::linked() alone.
    LinkType seen = type;
    seen.holdsUnseen = false;
    seen.holdsLostBuiltins = false;
    seen.builtin = { };
    if (seen.functions != LinkType::Functions::None && seen.functions != LinkType::Functions::Closure)
        seen.functions = LinkType::Functions::Lost;
    if (seen.isNothing())
        return { };
    return unlessAnything(typesTurned().turn(m_flow, typesAtHand(), seen));
}

// A call by binding hands a capture over as an entry parameter, and a closure brings what it was made with.
TypeID WriterOfLink::typeIDOfCapture(FunctionID id, uint32_t capture)
{
    const LinkType& passed = m_flow.typeAt(m_flow.parameter(id, capture));
    const LinkType& captured = m_flow.typeAt(m_flow.capture(id, capture));
    if (passed == captured || captured.isNothing())
        return typeIDOf(passed);
    if (passed.isNothing())
        return typeIDOf(captured);
    TypeID first = typeIDOf(passed);
    TypeID second = typeIDOf(captured);
    return first && second ? unlessAnything(joinOf(typesAtHand(), m_flow.families, first, second)) : TypeID();
}

thread_local TypesOfThreadInLink WriterOfLink::s_typesOfThread;

void WriterOfLink::writeFunctions()
{
    writeTypedPositionsOnAllThreads(m_flow, m_flow.program.types, m_numberOfFunctions, [](TypesOfThreadInLink types) {
        s_typesOfThread = types;
    }, [&](unsigned index) {
        writeFunction(FunctionID(index));
    });
}

void WriterOfLink::writeFunction(FunctionID id)
{
    uint32_t index = id.index();
    LinkedFunction& linked = m_tables.functions[index];
    linked.isReached = m_flow.reached.get(index);
    if (!linked.isReached)
        return;

    const Function& function = m_program.at(id);
    bool isLost = m_flow.lostFunctions.get(index);
    linked.everyCallerIsKnown = !isLost && !m_flow.escapedFunctions.get(index) && !m_functionsEnteredByHost.get(index) && !m_functionsThatNoSummaryNames.get(index);
    // The calls of unseen code are listed nowhere.
    linked.everyCallHasOneCallee = linked.everyCallerIsKnown && !m_functionsSharingCalls.get(index) && isEnteredByCallsAloneForLink(function.kind);
    linked.everyEntryIsConstruction = linked.everyCallerIsKnown && function.takesThisFromConstruction() && !m_flow.functionsEnteredByPlainCalls.get(index);
    // A coroutine hands its caller a promise or a generator.
    if (function.results.size() == 1 && !function.isCoroutine()) {
        linked.result = typeIDOf(m_flow.typeAt(m_flow.result(id, 0)));
        if (std::optional<ProvenInEveryVersion> established = function.results[0].established(); established && !linked.result)
            linked.result = unlessAnything(*established);
    }
    linked.thrown = typeIDOf(m_flow.typeAt(m_flow.thrown(id)));
    writeParameters(id, linked);
}

BitVector bodiesThatLoaderEntersAtMostOnce(const Program& program)
{
    // The loader enters a body for each module that names it.
    BitVector bodies(program.functions.size());
    BitVector bodiesOfSeveralModules(program.functions.size());
    for (const Module& module : program.modules) {
        if (!module.body)
            continue;
        if (bodies.quickSet(module.body.index()))
            bodiesOfSeveralModules.quickSet(module.body.index());
        const Function& body = program.at(module.body);
        bool isEnteredOnce = body.isModuleBody() && body.module && &program.at(body.module) == &module && isEvaluatedAtMostOnceOf(module.kind)
            && !module.flags.containsAny({ ModuleFlags::RunsAgain, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked });
        if (!isEnteredOnce)
            bodiesOfSeveralModules.quickSet(module.body.index());
    }
    bodies.exclude(bodiesOfSeveralModules);
    return bodies;
}

// LinkedFunction::runsAtMostOnce. From the bodies of modules downward, so no ring of calls is ever come to: each of its members waits for another.
void WriterOfLink::findFunctionsThatRunAtMostOnce()
{
    constexpr uint8_t several = 2;
    Vector<uint8_t> numberOfCallsThatEnter(FillWith { }, m_numberOfFunctions, uint8_t { 0 });
    for (const LinkedCall& call : m_tables.calls) {
        uint8_t& number = numberOfCallsThatEnter[call.callee.index()];
        number = std::min<uint8_t>(number + 1, several);
        if (call.mayComeAgain)
            m_tables.functions[call.callee.index()].isCalledInLoop = true;
    }

    Vector<FunctionID> functionsToFollow;
    for (size_t body : bodiesThatLoaderEntersAtMostOnce(m_program)) {
        // A site that evaluates a module is a call by binding. The body begins by asking whether it has begun.
        if (m_flow.reached.get(body))
            functionsToFollow.append(FunctionID(static_cast<uint32_t>(body)));
    }
    while (!functionsToFollow.isEmpty()) {
        FunctionID caller = functionsToFollow.takeLast();
        m_tables.functions[caller.index()].runsAtMostOnce = true;
        for (const LinkedCall& call : std::ranges::equal_range(m_tables.calls, caller, { }, &LinkedCall::caller)) {
            if (!call.mayComeAgain && numberOfCallsThatEnter[call.callee.index()] == 1 && m_tables.functions[call.callee.index()].everyCallHasOneCallee)
                functionsToFollow.append(call.callee);
        }
    }
}

bool mayReturnInFormOfItsOwn(const Function& function)
{
    if (function.isCoroutine() || function.results.size() != 1 || function.has(FunctionFlags::HasNewTarget))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
        return !function.has(FunctionFlags::HasThis) || !function.constructedClass;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool callsOfBuiltinComeBack(const Program& program, BuiltinID builtin)
{
    const BuiltinTable& table = program.builtins();
    if (!builtin || !table.contains(builtin))
        return false;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(builtin))) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        for (const BuiltinCallback& callback : overload.flow.callbacks) {
            if (callback.resultUse == BuiltinCallbackResultUse::Returned)
                return false;
        }
    }
    return true;
}

// LinkedFunction::ringsOfTailCallsAreListed. Only round a ring can a row of calls in tail position go on without end, so only there does each of them have to be a jump.
// Round a ring each result holds the next, so all are one type and one form: it takes a call that goes through an entry by value to part them, or a member that returns boxed values whatever the others do.
void WriterOfLink::findRingsOfTailCalls()
{
    struct TailCall {
        uint32_t from;
        uint32_t to;
        bool entersBody;
    };
    // What computeCallingConventions() asks of a function by itself. `[3].map(odd)`: a call of `odd` is not listed, so it returns boxed values, and so does whoever stands in a ring with it.
    auto mayReturnRawValue = [&](FunctionID id) {
        return m_tables.functions[id.index()].everyCallHasOneCallee && mayReturnInFormOfItsOwn(m_program.at(id));
    };
    // One node stands for whichever of the lost functions a call may enter.
    uint32_t anyLostFunction = m_numberOfFunctions;
    Vector<TailCall> tailCalls;
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (m_flow.lostFunctions.get(index))
            tailCalls.append(TailCall { anyLostFunction, index, false });
        if (!m_flow.reached.get(index))
            continue;
        FunctionID caller(index);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        for (uint32_t place = 0; place < summary.calls.size(); ++place) {
            const SummaryCall& call = summary.calls[place];
            // Native code comes back, and so does whatever it calls. Where its row forwards the call to an operand the walk puts the operand's call in its place: what is listed there is entered by value.
            if (!call.isInTailPosition || (call.callee == SummaryCall::Callee::Builtin && callsOfBuiltinComeBack(m_program, call.builtin)))
                continue;
            // The walk lays a list out only where it knows its length, and else leaves the call as it is written.
            if (FunctionID callee = call.passesList ? FunctionID() : m_tables.calleeAt(caller, call.site)) {
                const Function& entered = m_program.at(callee);
                // Only a call that is written as a direct one is sure to be one in every unit. Whether a call of a value enters the body is decided unit by unit, by what the walk knows of the callee there, and a slow version knows nothing.
                bool entersBody = call.callee == SummaryCall::Callee::Binding && canBeCalledByValueOf(entered.role) && !entered.has(FunctionCapabilities::RecordsCallee) && mayReturnRawValue(callee) && mayReturnRawValue(caller);
                tailCalls.append(TailCall { index, callee.index(), entersBody });
                continue;
            }
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[index] + place];
            for (FunctionID callee : callees.listed)
                tailCalls.append(TailCall { index, callee.index(), false });
            for (uint32_t shared : callees.shared)
                tailCalls.append(TailCall { index, anyLostFunction + 1 + shared, false });
            if (callees.mayEnterLost)
                tailCalls.append(TailCall { index, anyLostFunction, false });
        }
    }
    // A node for each shared call, between those who go by it and what it lists.
    uint32_t numberOfSharedCalls = static_cast<uint32_t>(m_flow.calleesOfSharedCallsAsSettled().size());
    for (uint32_t shared = 0; shared < numberOfSharedCalls; ++shared) {
        for (FunctionID callee : m_flow.calleesOfSharedCallsAsSettled()[shared].listed)
            tailCalls.append(TailCall { anyLostFunction + 1 + shared, callee.index(), false });
    }

    uint32_t numberOfNodes = m_numberOfFunctions + 1 + numberOfSharedCalls;
    std::ranges::sort(tailCalls, { }, &TailCall::from);
    Vector<uint32_t> targets;
    Vector<uint32_t> firstTargetOf;
    targets.reserveInitialCapacity(tailCalls.size());
    firstTargetOf.reserveInitialCapacity(numberOfNodes + 1);
    for (const TailCall& tailCall : tailCalls) {
        while (firstTargetOf.size() <= tailCall.from)
            firstTargetOf.append(static_cast<uint32_t>(targets.size()));
        targets.append(tailCall.to);
    }
    while (firstTargetOf.size() <= numberOfNodes)
        firstTargetOf.append(static_cast<uint32_t>(targets.size()));
    StronglyConnectedComponents rings = computeStronglyConnectedComponents(numberOfNodes, [&](unsigned node) {
        return targets.span().subspan(firstTargetOf[node], firstTargetOf[node + 1] - firstTargetOf[node]);
    });

    BitVector ringsEnteredByValue(rings.numberOfComponents);
    for (const TailCall& tailCall : tailCalls) {
        if (!tailCall.entersBody && rings.componentOf[tailCall.from] == rings.componentOf[tailCall.to])
            ringsEnteredByValue.quickSet(rings.componentOf[tailCall.from]);
    }
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index)
        m_tables.functions[index].ringsOfTailCallsAreListed = m_flow.reached.get(index) && !ringsEnteredByValue.quickGet(rings.componentOf[index]);
}

void WriterOfLink::writeParameters(FunctionID id, const LinkedFunction& linked)
{
    const Function& function = m_program.at(id);
    std::span<LinkedParameter> parameters = m_tables.parameters.mutableSpan().subspan(linked.firstParameter, linked.numberOfParameters);
    RELEASE_ASSERT(parameters.size() == function.numberOfEntryParameters());
    // The entry makes the list, at no node of any summary.
    uint32_t restParameter = function.has(FunctionFlags::HasRestParameter) ? function.firstParameterIndex() + function.numberOfPositionalParameters() : none;

    for (uint32_t index = 0; index < parameters.size(); ++index) {
        LinkedParameter& parameter = parameters[index];
        const LinkType arrived = m_flow.withoutObjectsThatHaveEscaped(m_flow.typeAt(m_flow.parameter(id, index)));
        const DeclaredType* declared = function.declaredEntryParameter(index);

        // The compiler made such a position and lays down every store into it: what is written holds.
        TypeID established;
        if (declared) {
            if (std::optional<ProvenInEveryVersion> byCompiler = declared->established())
                established = *byCompiler;
        }
        auto prove = [&](TypeID byFlow) {
            if (!linked.everyCallerIsKnown && !writesWhatIsExpected())
                return;
            bool flowSaysMore = !established || (byFlow && typesAtHand().isSubset(byFlow, established));
            parameter.proven = flowSaysMore ? byFlow : established;
        };

        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            prove(typeIDOfCapture(id, index));
            break;
        case EntryParameter::This:
        case EntryParameter::Parameter: {
            prove(index == restParameter ? TypeID() : typeIDOf(arrived));
            parameter.staysAsItsClassMakesIt = !writesWhatIsExpected() && linked.everyCallerIsKnown && index != restParameter && holdsOnlyInstancesThatStayAsMade(arrived);
            TypeID written = declared && !declared->isEstablished() ? unlessAnything(widened(declared->written())) : TypeID();
            // The text of a built-in writes nothing of what it takes. The first of its rows does.
            if (!written)
                written = unlessAnything(widened(typeExpectedByLibraryFunction(m_program, typesAtHand(), id, index)));
            LinkType seen = arrived;
            seen.holdsUnseen = false;
            seen.holdsLostBuiltins = false;
            parameter.expected = written ? written : withoutLostBuiltinsInLink(arrived).isAnything() ? TypeID() : typeIDOf(seen);
            parameter.expectedIsDeclared = !!written;
            break;
        }
        // Whoever enters lays down what it passes there: undefined, the callee, or what stands at SummaryCall::newTarget.
        case EntryParameter::Callee:
        case EntryParameter::NewTarget:
            prove(typeIDOf(arrived));
            break;
        case EntryParameter::ArgumentCount:
        case EntryParameter::HiddenRest:
            break;
        }
    }
}

// WHO MAY NAME A FUNCTION. The walk mentions a unit of F in the code of G because it knows of some value, or of some field of an operation, that it is F. That knowledge comes into a walk by three doors and no other.
// Each is gone through whole here, so by induction G leads to whatever a walk of G knows:
//   the body      whatever the fields of its operations name: ThinSummary::functionsNamed, which the schema's own visitor fills. In a copy of a callee: what the callee names, and G leads to the callee
//   the tables    every type that they hold of a position that the body reads, and of what a callee gives: forEachFunctionOfType(). A place where closures are lost, or class objects are several, is `function` in the tables and names nobody
//   arrivals      what a caller had proven of an argument. The caller leads to that function, which says nothing of G. But the value may be there in some run, so it lies within what Link saw arrive at the parameter,
//                 however much narrower it is: forEachNodeArrivingAt()
// What the state knows, what a test narrows and what a fold leaves are made of those.
// WHAT NOBODY CAN LIST HAS NO REFERRER, AND NEEDS NONE. A guess is no knowledge: where it is a layout, the run behind its test finds by name what the flow never saw arrive. And a closure that is lost to Link may be one function to a caller.
// With a node for each name and one for the lost, whoever looks a name up led to whoever lies under it anywhere, and nearly every function of a large program stood in one cycle, where nothing is ordered.
// An edge is there for the unit that a mention may bring. So those functions are in the queue from the start: LinkedFunction::isInQueueFromStart. The walk may call one wherever it finds it.
// It copies one only where the graph leads to it or the function names nothing but such functions, because a copy mentions what the callee names: mayBeWalkedAtInlining().
// A function that the listed calls alone enter is none of them: wherever its closure goes Link has it alone, so whoever can know it names it.
// Whatever else a guess may come to expect, the walk asks before it takes a value for a function: LinkTables::mayBeMentionedBy().
// UnitQueue holds each mention of finished code that may bring a unit against the graph, in every build.

void WriterOfLink::noteHolder(PropertyKeyID key, LayoutID classLayout, FunctionID held)
{
    if (!held)
        return;
    if (key)
        m_functionsUnderKeys.append(FunctionUnderKey { key, held });
    if (classLayout)
        m_functionsOfClasses.append(FunctionOfClass { classLayout, held });
}

void WriterOfLink::indexFunctionsThatAreHeld()
{
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        for (const SummaryMember& member : summary.members) {
            // The record of the class names what the class was given, whatever was stored over it since. A lookup by name finds only what the tables say lies there.
            if (const SummaryNode& given = summary.nodes[member.value]; given.kind == SummaryNode::Kind::Closure)
                noteHolder(member.key, member.classLayout, FunctionID(given.first));
            noteHolder(member.key, member.classLayout, closureAloneAt(m_flow.member(member.classLayout, member.side, member.index)));
        }
    }
    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        noteHolder(name.key, name.layout, closureAloneAt(name.stored));
        noteHolder(name.key, name.layout, closureAloneAt(name.storedInSight));
    }
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        const Layout& layout = m_program.layouts[index];
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            PropertyKeyID key = layout.fields[field].key;
            if (!key)
                continue;
            FieldReference reference { LayoutID(index), field };
            noteHolder(key, { }, closureAloneAt(m_flow.field(reference)));
            const LinkedCell& cell = m_tables.field(reference);
            bool constructionAloneFillsRecord = layout.kind == LayoutKind::Record && cell.everyWriterIsKnown && !cell.isWrittenAfterConstruction;
            forEachFunctionOfType(cell.proven, [&](FunctionID held) {
                noteHolder(key, { }, held);
                if (constructionAloneFillsRecord && m_program.at(held).captures.size() == 1)
                    m_functionsThatSlotsOfRecordsMayDefer.append(FunctionOfClass { LayoutID(index), held });
            });
        }
        for (const StaticField& field : layout.staticFields) {
            if (!field.key || !field.variable)
                continue;
            noteHolder(field.key, { }, closureAloneAt(m_flow.variable(field.variable)));
            forEachFunctionOfType(m_tables.variable(field.variable).proven, [&](FunctionID held) {
                noteHolder(field.key, { }, held);
            });
        }
    }
    m_flow.forEachAppendedField([&](PropertyKeyID key, FlowNodeID holder) {
        noteHolder(key, { }, closureAloneAt(holder));
    });
    for (size_t place = 0; place < m_tables.appendedKeys.size(); ++place) {
        forEachFunctionOfType(m_tables.appendedFields[place].proven, [&](FunctionID held) {
            noteHolder(m_tables.appendedKeys[place], { }, held);
        });
    }

    auto sortWithoutRepeats = [](auto& list) {
        std::ranges::sort(list);
        auto repeats = std::ranges::unique(list);
        list.shrink(repeats.begin() - list.begin());
    };
    sortWithoutRepeats(m_functionsUnderKeys);
    sortWithoutRepeats(m_functionsOfClasses);
    sortWithoutRepeats(m_functionsThatSlotsOfRecordsMayDefer);

    auto putInQueueFromStart = [&](FunctionID id) {
        m_tables.functions[id.index()].liesUnderNameOrIsLost = m_program.at(id).hasBody();
        m_tables.functions[id.index()].isInQueueFromStart = m_flow.reached.get(id.index()) && m_program.at(id).hasBody() && !mayTakeArrivals(id);
    };
    for (const FunctionUnderKey& held : m_functionsUnderKeys)
        putInQueueFromStart(held.function);
    for (size_t lost : m_flow.lostFunctions)
        putInQueueFromStart(FunctionID(static_cast<uint32_t>(lost)));
}

FunctionID WriterOfLink::addGroup(LinkedGroup::Kind kind, uint32_t subject)
{
    m_tables.groups.append(LinkedGroup { kind, subject, 0, 0 });
    return FunctionID(m_tables.numberOfNodesOfReferrers() - 1);
}

// In an order that follows from the program alone.
void WriterOfLink::makeGroups()
{
    m_tables.groups.shrink(0);
    if (!m_functionsThatNoSummaryNames.isEmpty())
        m_anybody = addGroup(LinkedGroup::Kind::Anybody);

    m_groupOfConstructors.fill(FunctionID(), m_flow.families.size());
    BitVector familiesWhoseConstructorsArrive(m_flow.families.size());
    for (size_t index : m_flow.reached) {
        FunctionID id(static_cast<uint32_t>(index));
        if (!mayTakeArrivals(id))
            continue;
        for (uint32_t parameter = 0; parameter < m_tables.function(id).numberOfParameters; ++parameter) {
            if (const LinkType& arrived = m_flow.typeAt(m_flow.parameter(id, parameter)); arrived.functions == LinkType::Functions::Constructors)
                familiesWhoseConstructorsArrive.quickSet(arrived.constructorsOf.index());
        }
    }
    for (size_t family : familiesWhoseConstructorsArrive)
        m_groupOfConstructors[family] = addGroup(LinkedGroup::Kind::Constructors, static_cast<uint32_t>(family));

    // Those that some call goes by in the end and that name somebody, in the order of what they stand for.
    BitVector sharedCallsInUse(m_flow.calleesOfSharedCallsAsSettled().size());
    for (const LinkFlow::CalleesOfCall& callees : m_flow.calleesOfCallsAsSettled()) {
        for (uint32_t shared : callees.shared)
            sharedCallsInUse.quickSet(shared);
    }
    for (size_t shared : sharedCallsInUse) {
        bool namesSomebody = false;
        forEachFunctionNamedBySharedCall(static_cast<uint32_t>(shared), [&](FunctionID) {
            namesSomebody = true;
        });
        if (namesSomebody)
            m_sharedCallsOfGroups.append(static_cast<uint32_t>(shared));
    }
    std::ranges::sort(m_sharedCallsOfGroups, { }, [&](uint32_t shared) {
        return m_flow.subjectsOfSharedCalls[shared];
    });
    m_groupOfSharedCall.fill(FunctionID(), m_flow.calleesOfSharedCallsAsSettled().size());
    for (uint32_t place = 0; place < m_sharedCallsOfGroups.size(); ++place)
        m_groupOfSharedCall[m_sharedCallsOfGroups[place]] = addGroup(LinkedGroup::Kind::SharedCall, place);
}

// WHAT A CALL LISTS HANGS ON WHAT IT SAW FIRST: one closure in front of the others, a member in front of unseen code, a family on the way to what is lost. All those are lost or lie under a name, and several calls share them:
// they are in the queue from the start. The one callee of a call is named by the call itself, so a function that is the one callee of every call that may enter it is passed over here too.
void WriterOfLink::forEachFunctionListedWhateverCameFirst(const LinkFlow::CalleesOfCall& callees, const Invocable<void(FunctionID)> auto& name) const
{
    for (FunctionID callee : callees.listed) {
        if (!m_tables.function(callee).isInQueueFromStart && !mayTakeArrivals(callee))
            name(callee);
    }
}

void WriterOfLink::forEachFunctionNamedBySharedCall(uint32_t shared, const Invocable<void(FunctionID)> auto& name) const
{
    forEachFunctionListedWhateverCameFirst(m_flow.calleesOfSharedCallsAsSettled()[shared], name);
}

void WriterOfLink::forEachMemberOf(const LinkedGroup& group, const Invocable<void(FunctionID)> auto& name) const
{
    switch (group.kind) {
    case LinkedGroup::Kind::Anybody:
        for (size_t member : m_functionsThatNoSummaryNames)
            name(FunctionID(static_cast<uint32_t>(member)));
        return;
    case LinkedGroup::Kind::Constructors:
        m_program.at(m_flow.families.layoutsOf(FamilyID(group.subject))).forEach([&](LayoutID member) {
            name(m_program.at(member).constructor);
        });
        return;
    case LinkedGroup::Kind::SharedCall:
        forEachFunctionNamedBySharedCall(m_sharedCallsOfGroups[group.subject], name);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// One class object among several may be one function to a caller. So may a value that is lost to Link, which is in the queue from the start.
void WriterOfLink::forEachNodeArrivingAt(FlowNodeID node, const Invocable<void(FunctionID)> auto& name) const
{
    const LinkType& held = m_flow.typeAt(node);
    switch (held.functions) {
    case LinkType::Functions::None:
        return;
    case LinkType::Functions::Closure:
        name(held.function);
        return;
    case LinkType::Functions::Constructors:
        name(m_groupOfConstructors[held.constructorsOf.index()]);
        return;
    case LinkType::Functions::Lost:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// By any thread: every type of the tables lies in the program's table.
void WriterOfLink::forEachFunctionOfType(TypeID type, const Invocable<void(FunctionID)> auto& name) const
{
    if (!type || holdsOnlyPrimitives(type))
        return;
    TypeReader reader(m_program);
    forEachFunctionOf(reader, reader.at(type), [&](FunctionID named) {
        name(named);
    });
}

// It is enough that the referrer leads to a function from node to node: a copy of a callee mentions what the callee names.
void WriterOfLink::forEachNodeNamedBy(FunctionID referrer, const Invocable<void(FunctionID)> auto& name) const
{
    const Function& function = m_program.at(referrer);

    // Whoever names a class names its constructor. The record of the class names its members and the class above it.
    if (LayoutID constructed = function.constructedClass; constructed && m_program.at(constructed).constructor == referrer) {
        for (const FunctionOfClass& member : std::ranges::equal_range(m_functionsOfClasses, constructed, { }, &FunctionOfClass::classLayout))
            name(member.function);
        if (LayoutID base = writtenBaseOf(m_program.at(constructed)))
            name(m_program.at(base).constructor);
    }

    // Of a function that is never entered only the generic entry gets code, which names nothing.
    if (!m_flow.reached.get(referrer.index()))
        return;
    name(m_anybody);

    const ThinSummary& summary = m_flow.summaries.at(referrer);
    for (FunctionID named : summary.functionsNamed)
        name(named);

    const LinkedFunction& linked = m_tables.function(referrer);
    bool takesArrivals = mayTakeArrivals(referrer);
    std::span<const LinkedParameter> parameters = m_tables.parametersOf(referrer);
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        forEachFunctionOfType(parameters[index].proven, name);
        forEachFunctionOfType(parameters[index].expected, name);
        if (takesArrivals)
            forEachNodeArrivingAt(m_flow.parameter(referrer, index), name);
    }
    for (uint32_t capture = 0; capture < function.captures.size(); ++capture)
        forEachFunctionOfType(m_tables.capture(referrer, capture).proven, name);
    // Whoever calls it is handed these, and leads here.
    forEachFunctionOfType(linked.result, name);
    forEachFunctionOfType(linked.thrown, name);
    for (const LinkedCatch& row : std::ranges::equal_range(m_tables.catches, referrer, { }, &LinkedCatch::function))
        forEachFunctionOfType(row.caught, name);

    uint32_t firstCall = m_flow.firstCallOfFunction[referrer.index()];
    for (uint32_t index = 0; index < summary.calls.size(); ++index) {
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[firstCall + index];
        name(m_tables.calleeAt(referrer, summary.calls[index].site));
        forEachFunctionListedWhateverCameFirst(callees, name);
        for (uint32_t shared : callees.shared)
            name(m_groupOfSharedCall[shared]);
    }

    forEachFunctionNamedByCoroutinesCalledBy(m_tables, TypeReader(m_program), referrer, scopedLambda<void(FunctionID)>([&](FunctionID named) {
        name(named);
    }));

    for (const LinkedGetterAtRead& read : std::ranges::equal_range(m_tables.gettersExpectedAtReads, referrer, { }, &LinkedGetterAtRead::function))
        name(read.getter);
    for (const LinkedResultOfRead& read : std::ranges::equal_range(m_tables.resultsOfReadsByKey, referrer, { }, &LinkedResultOfRead::function))
        forEachFunctionOfType(read.proven, name);
    for (const LinkedContentsAtOperation& contents : std::ranges::equal_range(m_tables.contentsAtOperations, referrer, { }, &LinkedContentsAtOperation::function)) {
        forEachFunctionOfType(contents.key, name);
        forEachFunctionOfType(contents.value, name);
    }

    // Node 0 holds anything in every summary.
    std::span<const uint32_t> nodesOfSummary = m_flow.nodesOfSummaryOf(referrer);
    // `meter.value`, where objects of one layout arrive in whose field literals lay one function: the walk calls it in the place of the access. callProvenAccessor(), callAccessorInSlot().
    // The graph of the held reading is laid over this one (layReferrersOfHeldReadingOver()), and the types name the same objects in both: once is enough.
    if (!writesWhatIsExpected() && !m_tables.accessorsExpectedInFields.isEmpty()) {
        for (const SummaryAccess& access : summary.accesses) {
            if (access.isToContents() || !access.key || liesInThread(access.key) || !m_tables.keysOfAccessorsExpectedInFields.get(access.key.index()))
                continue;
            const LinkType& subject = m_flow.typeAt(FlowNodeID(nodesOfSummary[access.subject]));
            std::optional<FieldReference> field = subject.objects == LinkType::Objects::Layout ? findField(m_program, subject.layout, access.key) : std::nullopt;
            if (const LinkedAccessorsOfField* laid = field ? m_tables.findAccessorsExpectedIn(*field) : nullptr) {
                name(laid->getter);
                name(laid->setter);
            }
        }
    }
    for (uint32_t index = 1; index < summary.nodes.size(); ++index) {
        name(closureAloneAt(FlowNodeID(nodesOfSummary[index])));

        const SummaryNode& node = summary.nodes[index];
        switch (node.kind) {
        case SummaryNode::Kind::Variable:
            forEachFunctionOfType(m_tables.variable(VariableID(node.first)).proven, name);
            break;
        case SummaryNode::Kind::Field:
            forEachFunctionOfType(m_tables.field({ LayoutID(node.first), node.second }).proven, name);
            break;
        // The scan makes some of these three that no field of an operation spells.
        case SummaryNode::Kind::Constant:
            forEachFunctionOfType(TypeID::fromBits(node.first), name);
            break;
        case SummaryNode::Kind::Closure:
            name(FunctionID(node.first));
            break;
        case SummaryNode::Kind::Allocation: {
            const Layout& declared = m_program.at(declaredLayoutOf(m_program, LayoutID(node.first)));
            if (declared.kind == LayoutKind::Instance)
                name(declared.constructor);
            for (const FunctionOfClass& deferred : std::ranges::equal_range(m_functionsThatSlotsOfRecordsMayDefer, LayoutID(node.first), { }, &FunctionOfClass::classLayout))
                name(deferred.function);
            break;
        }
        case SummaryNode::Kind::Unknown:
        case SummaryNode::Kind::Parameter:
        case SummaryNode::Kind::Result:
        case SummaryNode::Kind::Thrown:
        case SummaryNode::Kind::Capture:
        case SummaryNode::Kind::Member:
        case SummaryNode::Kind::Contents:
        case SummaryNode::Kind::ClassInstance:
        case SummaryNode::Kind::Builtin:
        case SummaryNode::Kind::Join:
        case SummaryNode::Kind::ResultOfCall:
        case SummaryNode::Kind::ResultOfAccess:
        case SummaryNode::Kind::ResultOfOperator:
        case SummaryNode::Kind::ResultOfConversion:
            break;
        }
    }

    // The start of a module evaluates what the module imports. A site that evaluates a module is a call by binding.
    if (!function.module || m_program.at(function.module).body != referrer)
        return;
    for (const ModuleRequest& request : m_program.at(function.module).requests)
        name(m_program.at(request.target).body);
}

void WriterOfLink::writeReferrers()
{
    indexFunctionsThatAreHeld();
    makeGroups();

    const ParallelLoops& loops = this->loops();
    uint32_t numberOfNodes = m_tables.numberOfNodesOfReferrers();
    ChunksOfIndices division = divideIntoChunks(numberOfNodes);
    // The node that is named above its referrer.
    Vector<Vector<uint64_t>> namingsOfChunks(division.size());
    LinkFlow::ViewOfHeldReading* view = LinkFlow::ViewAtWork::current();
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk) {
        LinkFlow::ViewAtWork viewAtWork(view);
        Vector<uint64_t>& namings = namingsOfChunks[chunk];
        // What one referrer names, each as often as it is named.
        Vector<uint32_t, 64> namedByOne;
        for (uint32_t index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            namedByOne.shrink(0);
            auto name = [&](FunctionID named) {
                if (named)
                    namedByOne.append(named.index());
            };
            if (FunctionID referrer(index); m_tables.isGroup(referrer))
                forEachMemberOf(m_tables.group(referrer), name);
            else
                forEachNodeNamedBy(referrer, name);
            std::ranges::sort(namedByOne);
            uint32_t last = none;
            for (uint32_t named : namedByOne) {
                if (named != std::exchange(last, named))
                    namings.append(static_cast<uint64_t>(named) << 32 | index);
            }
        }
    });
    Vector<uint64_t> namings = joinChunks<uint64_t>(loops, WTF::move(namingsOfChunks));

    // The referrers come in ascending order and the rows of a group stay in the order in which they came, so each list is ascending.
    sortRowsByGroup(loops, namings, numberOfNodes);
    m_tables.firstReferrer = firstRowsOfGroups(loops, numberOfNodes, namings.size(), [&](size_t row) {
        return static_cast<uint32_t>(namings[row] >> 32);
    });
    m_tables.referrers.grow(namings.size());
    forEachRangeOfIndicesInParallel(loops, namings.size(), [&](size_t first, size_t end) {
        for (size_t row = first; row < end; ++row)
            m_tables.referrers[row] = FunctionID(static_cast<uint32_t>(namings[row]));
    });
}

// Functions have one number in both graphs. A group that both have (the same kind and subject) is one node, and its members and referrers are those of both. The others come behind, in their order: that of a shared call is a place in its own reading.
void layReferrersOfHeldReadingOver(LinkTables& tables, LinkTables& held, const ParallelLoops& loops)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(tables.functions.size());
    RELEASE_ASSERT(held.functions.size() == numberOfFunctions && tables.firstReferrer.size() == tables.numberOfNodesOfReferrers() + 1 && held.firstReferrer.size() == held.numberOfNodesOfReferrers() + 1);
    auto nameOfGroup = [](const LinkedGroup& group) {
        return (static_cast<uint64_t>(group.kind) << 32 | group.subject) + 1;
    };
    // What the held reading has alone under a name a guess may test for. Who takes what callers leave goes by the proven reading.
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        LinkedFunction& function = tables.functions[index];
        function.liesUnderNameOrIsLost |= held.functions[index].liesUnderNameOrIsLost;
        function.isInQueueFromStart = function.liesUnderNameOrIsLost && function.isReached && !(function.everyCallerIsKnown && function.everyCallHasOneCallee);
    }

    uint32_t numberOfOwnNodes = tables.numberOfNodesOfReferrers();
    uint32_t numberOfSharedCalls = 0;
    HashMap<uint64_t, uint32_t> nodesOfGroups;
    for (uint32_t index = 0; index < tables.groups.size(); ++index) {
        if (tables.groups[index].kind == LinkedGroup::Kind::SharedCall)
            ++numberOfSharedCalls;
        else
            nodesOfGroups.add(nameOfGroup(tables.groups[index]), numberOfFunctions + index);
    }
    Vector<uint32_t> nodeOfHeldGroup(held.groups.size());
    for (uint32_t index = 0; index < held.groups.size(); ++index) {
        LinkedGroup group { .kind = held.groups[index].kind, .subject = held.groups[index].subject };
        if (group.kind == LinkedGroup::Kind::SharedCall)
            group.subject = numberOfSharedCalls++;
        else if (auto found = nodesOfGroups.find(nameOfGroup(group)); found != nodesOfGroups.end()) {
            nodeOfHeldGroup[index] = found->value;
            continue;
        }
        nodeOfHeldGroup[index] = tables.numberOfNodesOfReferrers();
        tables.groups.append(group);
    }
    uint32_t numberOfNodes = tables.numberOfNodesOfReferrers();
    static constexpr uint32_t none = UINT32_MAX;
    Vector<uint32_t> heldNodeOf;
    heldNodeOf.fill(none, numberOfNodes);
    for (uint32_t index = 0; index < numberOfFunctions; ++index)
        heldNodeOf[index] = index;
    for (uint32_t index = 0; index < held.groups.size(); ++index)
        heldNodeOf[nodeOfHeldGroup[index]] = numberOfFunctions + index;

    auto gather = [&](uint32_t node, Vector<uint32_t, 64>& referrers) {
        referrers.shrink(0);
        if (node < numberOfOwnNodes) {
            for (FunctionID referrer : tables.referrersOf(FunctionID(node)))
                referrers.append(referrer.index());
        }
        size_t numberOfOwn = referrers.size();
        size_t endOfHeldFunctions = numberOfOwn;
        if (heldNodeOf[node] != none) {
            for (FunctionID referrer : held.referrersOf(FunctionID(heldNodeOf[node]))) {
                referrers.append(referrer.index() < numberOfFunctions ? referrer.index() : nodeOfHeldGroup[referrer.index() - numberOfFunctions]);
                endOfHeldFunctions += referrer.index() < numberOfFunctions;
            }
        }
        if (referrers.size() == numberOfOwn)
            return;
        // Each reading lists the referrers of a node in ascending order, the functions in front of the groups, and only the groups of the held reading have other numbers here.
        // One node has a hundred thousand referrers, and it is gathered twice: merged, it no longer keeps one thread busy while the others wait.
        std::span<uint32_t> all = referrers.mutableSpan();
        std::span<uint32_t> ofHeldReading = all.subspan(numberOfOwn);
        std::ranges::sort(all.subspan(endOfHeldFunctions));
        std::ranges::inplace_merge(ofHeldReading, ofHeldReading.begin() + (endOfHeldFunctions - numberOfOwn));
        std::ranges::inplace_merge(all, all.begin() + numberOfOwn);
        referrers.shrink(std::ranges::unique(referrers).begin() - referrers.begin());
    };

    Vector<uint32_t> firstReferrer;
    firstReferrer.fill(0, numberOfNodes + 1);
    forEachRangeOfIndicesInParallel(loops, numberOfNodes, [&](size_t first, size_t end) {
        Vector<uint32_t, 64> referrers;
        for (size_t node = first; node < end; ++node) {
            gather(static_cast<uint32_t>(node), referrers);
            firstReferrer[node + 1] = static_cast<uint32_t>(referrers.size());
        }
    });
    for (uint32_t node = 0; node < numberOfNodes; ++node)
        firstReferrer[node + 1] += firstReferrer[node];
    Vector<FunctionID> all(firstReferrer[numberOfNodes]);
    forEachRangeOfIndicesInParallel(loops, numberOfNodes, [&](size_t first, size_t end) {
        Vector<uint32_t, 64> referrers;
        for (size_t node = first; node < end; ++node) {
            gather(static_cast<uint32_t>(node), referrers);
            for (size_t position = 0; position < referrers.size(); ++position)
                all[firstReferrer[node] + position] = FunctionID(referrers[position]);
        }
    });
    tables.firstReferrer = WTF::move(firstReferrer);
    tables.referrers = WTF::move(all);

    held.groups = { };
    held.referrers = { };
    held.firstReferrer.fill(0, numberOfFunctions + 1);
}

void printBreachesOfClosureOfLink(PrintStream& out, Program& program, const ThinSummaries& summaries, const Families& families, const LooksAtBodies& looks, Mechanisms mechanisms, BuildContext& build)
{
    static constexpr std::pair<OrderOfWorklist, ASCIILiteral> orders[] {
        { OrderOfWorklist::NewestFirst, "newest first"_s },
        { OrderOfWorklist::OldestFirst, "oldest first"_s },
        { OrderOfWorklist::Scattered, "scattered"_s },
    };
    for (auto [order, name] : orders) {
        LinkFlow flow(program, summaries, families, looks, mechanisms, build, order);
        flow.reachEntries();
        seedCellsSharedWithUnseenCode(flow);
        for (const BreachOfClosure& breach : flow.findBreachesOfClosure()) {
            out.print(name, ": ");
            if (breach.node) {
                flow.describe(out, breach.node);
                out.print(" climbs from ", breach.before, " to ", breach.after);
            } else
                out.print(breach.set, " grows");
            if (!breach.goneOver)
                out.println(" | follows from the others");
            else {
                out.print(" | brought by ");
                if (breach.told.function)
                    out.print(breach.told, ", told at ");
                else
                    out.print("an edge out of ");
                flow.describe(out, breach.goneOver);
                out.println();
            }
        }
    }
}

static LinkTables writeTablesOfProvingReading(Program&, Mechanisms, std::unique_ptr<LinkFlow>&, TurnsOfReadings*);

LinkTables link(Program& program, const ThinSummaries& summaries, const Families& families, const LooksAtBodies& looks, Mechanisms mechanisms, BuildContext& build, OrderOfWorklist order, TurnsOfReadings* turns)
{
    RELEASE_ASSERT(summaries.size() == program.functions.size());
    std::unique_ptr<LinkFlow> ownedFlow = makeUniqueWithoutFastMallocCheck<LinkFlow>(program, summaries, families, looks, mechanisms, build, order);
    LinkFlow& flow = *ownedFlow;
    flow.reachEntries();
    seedCellsSharedWithUnseenCode(flow);
    flow.run();
    flow.forgetWaysOfFlow();
    if (!turns)
        return writeTablesOfProvingReading(program, mechanisms, ownedFlow, turns);

    // held(), by the same writers, out of the same flow, beside linked().
    std::unique_ptr<LinkFlow::ViewOfHeldReading> view = flow.makeViewOfHeldReading();
    turns->held = conservativeLinkTables(program);
    turns->held.sizeOfFlow = flow.size();
    turns->held.unseenCodeCanRun = flow.builtinsInReach().unseenCodeCanRun();
    LinkTables tables;
    runLanes(flow.loops, 2, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
        if (!lane) {
            tables = writeTablesOfProvingReading(program, mechanisms, ownedFlow, turns);
            return;
        }
        // It waits for the lane in front of it alone.
        {
            view->loops = &loopsOfLane;
            LinkFlow::ViewAtWork viewAtWork(view.get());
            LinkTables& expected = turns->held;
            if (!mechanisms.containsAny({ Mechanism::DirectCalls, Mechanism::TypedOperations })) {
                for (size_t index = 0; index < expected.functions.size(); ++index)
                    expected.functions[index].isReached = flow.reached.get(index);
                return;
            }
            // All that makes no type, while linked()'s writers have the table of types.
            view->functionsThatTablesMayName = &flow.reached;
            WriterOfLink writer(flow, expected);
            lookAtSummariesForLayoutsAndCells(flow, expected);
            writer.writeWhatMakesNoType();
            turns->provingTablesAreWritten.wait();
            {
                TypeArena types(program.types, ArenaKeeps::InProgram);
                writer.writeWhatMakesTypes(types);
                writeLayoutsAndCells(flow, types, expected);
                writeTablesOfUnions(flow, expected);
            }
            writer.writeReferrers();
        }
    });
    return tables;
}

static LinkTables writeTablesOfProvingReading(Program& program, Mechanisms mechanisms, std::unique_ptr<LinkFlow>& ownedFlow, TurnsOfReadings* turns)
{
    LinkFlow& flow = *ownedFlow;
    flow.settleTypes();
    flow.settleSharedCalls();

    LinkTables tables = conservativeLinkTables(program);
    tables.sizeOfFlow = flow.size();
    tables.unseenCodeCanRun = flow.builtinsInReach().unseenCodeCanRun();
    tables.lookups = flow.builtinsInReach().takeLookups();
    tables.lookups.writesFollowedToBuiltins = flow.takeWritesFollowedToBuiltins();
    tables.namesAddedByTests = flow.testRuns().takeNamesAdded();
    tables.exportsSpiedOnThatWireLeavesShut = flow.testRuns().takeExportsSpiedOnThatWireLeavesShut();
    bool writesMoreThanReach = mechanisms.containsAny({ Mechanism::DirectCalls, Mechanism::TypedOperations });

    auto endTurnOfProvingReading = [&] {
        if (turns)
            turns->provingTablesAreWritten.signal();
    };

    if (!writesMoreThanReach) {
        for (size_t index = 0; index < tables.functions.size(); ++index)
            tables.functions[index].isReached = flow.reached.get(index);
        endTurnOfProvingReading();
        return tables;
    }

    WriterOfLink writer(flow, tables);
    {
        // Types made here are the program's: Link runs before Compile, and its arena commits into the program's table.
        TypeArena types(program.types, ArenaKeeps::InProgram);
        // Neither reads what the other writes, and the look makes no type.
        runLanes(flow.loops, {
            [&] {
                writer.writeWhatMakesNoType();
                writer.writeWhatMakesTypes(types);
            },
            [&] {
                lookAtSummariesForLayoutsAndCells(flow, tables);
            },
        });
        writeLayoutsAndCells(flow, types, tables);
        writeTablesOfUnions(flow, tables);
    }
    endTurnOfProvingReading();
    // Neither reads what the other writes, and neither makes a type.
    runLanes(flow.loops, 2, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
        if (!lane) {
            summarizeCallees(flow, tables);
            return;
        }
        writer.goOnWith(loopsOfLane);
        writer.writeProofsThatMakeNoType();
        writer.writeReferrers();
    });
    // What a callee does, what becomes of what it is passed and in what order the program starts are proofs. One of them shows in the types of held() too: finishReadingsOfLink().
    if (turns)
        turns->provingFlow = WTF::move(ownedFlow);
    else
        writeTypesFoundOfCallees(flow, tables);
    return tables;
}

void finishReadingsOfLink(Program& program, Mechanisms mechanisms, TurnsOfReadings& turns, LinkTables& proven, LinkTables& held)
{
    if (!mechanisms.containsAny({ Mechanism::DirectCalls, Mechanism::TypedOperations }))
        return;
    writeTypesFoundOfCallees(*turns.provingFlow, proven);
    turns.provingFlow = nullptr;
    // A variable that is proven never to be read in its dead zone is not read there whatever else is held of it.
    TypeArena types(program.types, ArenaKeeps::InProgram);
    for (size_t variable = 0; variable < held.variables.size(); ++variable) {
        TypeID provenOfVariable = proven.variables[variable].proven;
        LinkedCell& cell = held.variables[variable];
        if (cell.proven && provenOfVariable && types.withoutPrimitives(provenOfVariable, Primitive::Absent) == provenOfVariable)
            cell.proven = types.withoutPrimitives(cell.proven, Primitive::Absent);
    }
}

} } } // namespace JSC::AOT::Typed

#include "AOTLinkExceptionsInlines.h"

#endif // ENABLE(AOT)
