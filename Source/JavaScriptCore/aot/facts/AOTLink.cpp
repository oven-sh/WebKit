/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLayoutsByNames.h"
#include "AOTLinkFlow.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTGroupingOnPool.h"
#include "AOTIRProgram.h"
#include "AOTLinkCoroutines.h"
#include "AOTLinkLayouts.h"
#include "AOTLinkTypesOnAllThreads.h"
#include "AOTLinkTypesTurnedOnce.h"
#include "AOTLinkUnions.h"
#include "AOTPhaseTimes.h"
#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

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
    uint32_t lowestRank = rankOfCycle[referrer];
    uint32_t highestRank = rankOfCycle[named];
    if (lowestRank >= highestRank)
        return false;
    if (std::ranges::binary_search(cyclesNamedBy(referrer), named))
        return true;
    std::span<const uint32_t> fewer = cyclesNamedBy(referrer);
    std::span<const uint32_t> more = cyclesAwaitedBy(named);
    if (fewer.size() > more.size())
        std::swap(fewer, more);
    for (uint32_t between : fewer) {
        if (std::ranges::binary_search(more, between))
            return true;
    }

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

static size_t sideOfClassInLinkTables(LayoutID classLayout, ClassSide side)
{
    return 2 * static_cast<size_t>(classLayout.index()) + static_cast<size_t>(side);
}

static constexpr uint32_t fewestClassesAboveAskedByKeyInLinkTables = 16;

static void countClassesAboveInLinkTables(const Program& program, LinkTables& tables)
{
    constexpr uint32_t notCounted = UINT32_MAX;
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    tables.numbersOfClassesAbove.fill(notCounted, numberOfLayouts);
    Vector<uint32_t, 16> waiting;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        for (LayoutID current(index); current && tables.numbersOfClassesAbove[current.index()] == notCounted; current = program.at(current).base) {
            tables.numbersOfClassesAbove[current.index()] = 0;
            waiting.append(current.index());
        }
        while (!waiting.isEmpty()) {
            uint32_t layout = waiting.takeLast();
            LayoutID base = program.layouts[layout].base;
            tables.numbersOfClassesAbove[layout] = base ? tables.numbersOfClassesAbove[base.index()] + 1 : 0;
        }
    }
}

static void numberLayoutsAlongRowsInLinkTables(const Program& program, LinkTables& tables)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    Vector<uint32_t> firstLayoutBelow;
    firstLayoutBelow.fill(0, numberOfLayouts + 2);
    auto placeOfBase = [&](uint32_t index) {
        LayoutID base = program.layouts[index].base;
        return base ? base.index() : numberOfLayouts;
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        ++firstLayoutBelow[placeOfBase(index) + 1];
    for (uint32_t index = 1; index < firstLayoutBelow.size(); ++index)
        firstLayoutBelow[index] += firstLayoutBelow[index - 1];
    Vector<uint32_t> layoutsBelow;
    layoutsBelow.grow(numberOfLayouts);
    Vector<uint32_t> next(firstLayoutBelow);
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        layoutsBelow[next[placeOfBase(index)]++] = index;

    tables.placesOfLayoutsInRows.fill(0, numberOfLayouts);
    tables.endsOfLayoutsBelowInRows.fill(0, numberOfLayouts);
    Vector<uint32_t> open;
    uint32_t nextPlace = 0;
    auto enter = [&](uint32_t index) {
        tables.placesOfLayoutsInRows[index] = nextPlace++;
        next[index] = firstLayoutBelow[index];
        open.append(index);
    };
    for (uint32_t root = firstLayoutBelow[numberOfLayouts]; root < firstLayoutBelow[numberOfLayouts + 1]; ++root) {
        enter(layoutsBelow[root]);
        while (!open.isEmpty()) {
            uint32_t index = open.last();
            if (next[index] == firstLayoutBelow[index + 1]) {
                tables.endsOfLayoutsBelowInRows[index] = nextPlace;
                open.removeLast();
                continue;
            }
            enter(layoutsBelow[next[index]++]);
        }
    }
}

void LinkTables::indexMembersByKey(const Program& program, AskedByKey whoAsksByKey)
{
    membersByKey.resize(members.size());
    std::iota(membersByKey.begin(), membersByKey.end(), 0);
    std::ranges::sort(membersByKey, { }, [&](uint32_t place) {
        const LinkedMember& member = members[place];
        return std::tuple { member.classLayout, member.side, member.key, place };
    });
    firstMemberByKeyOfSide.fill(0, members.isEmpty() ? 1 : sideOfClassInLinkTables(members[membersByKey.last()].classLayout, ClassSide::Static) + 2);
    for (BitVector& keys : keysOfMembers)
        keys.clearAll();
    for (const LinkedMember& member : members) {
        ++firstMemberByKeyOfSide[sideOfClassInLinkTables(member.classLayout, member.side) + 1];
        if (member.key)
            keysOfMembers[static_cast<size_t>(member.side)].set(member.key.index());
    }
    keysOfStaticFields.clearAll();
    for (const Layout& layout : program.layouts) {
        for (const StaticField& field : layout.staticFields) {
            if (field.key && !liesInThread(field.key))
                keysOfStaticFields.set(field.key.index());
        }
    }
    for (size_t side = 1; side < firstMemberByKeyOfSide.size(); ++side)
        firstMemberByKeyOfSide[side] += firstMemberByKeyOfSide[side - 1];

    countClassesAboveInLinkTables(program, *this);
    membersByKeyAlongRows.shrink(0);
    if (whoAsksByKey == AskedByKey::ByLongRowsAlone && std::ranges::none_of(numbersOfClassesAbove, [](uint32_t number) { return number >= fewestClassesAboveAskedByKeyInLinkTables; }))
        return;
    numberLayoutsAlongRowsInLinkTables(program, *this);
    Vector<std::pair<uint64_t, uint32_t>> words(members.size(), [&](size_t place) {
        const LinkedMember& member = members[place];
        return std::pair { static_cast<uint64_t>(member.side) << 63 | static_cast<uint64_t>(member.key.bits()) << 31 | placesOfLayoutsInRows[member.classLayout.index()], static_cast<uint32_t>(place) };
    });
    std::ranges::sort(words);
    membersByKeyAlongRows = words.map([](const std::pair<uint64_t, uint32_t>& word) {
        return word.second;
    });
}

FunctionID LinkTables::methodReplacedBy(FunctionID replacement, PropertyKeyID key) const
{
    auto listed = std::ranges::equal_range(replacedMethods, replacement, { }, &ReplacedMethod::replacement);
    FunctionID found;
    for (const ReplacedMethod& replaced : listed) {
        if (listed.size() > 1 && replaced.key != key)
            continue;
        if (found && found != replaced.written)
            return { };
        found = replaced.written;
    }
    return found;
}

bool LinkTables::replacesMethod(FunctionID replacement, FunctionID written) const
{
    auto listed = std::ranges::equal_range(replacedMethods, replacement, { }, &ReplacedMethod::replacement);
    return std::ranges::contains(listed, written, &ReplacedMethod::written);
}

std::span<const uint32_t> LinkTables::membersUnderKeyAlongRows(ClassSide side, PropertyKeyID key) const
{
    auto underKey = std::ranges::equal_range(membersByKeyAlongRows, std::pair { side, key }, { }, [&](uint32_t place) {
        const LinkedMember& member = members[place];
        return std::pair { member.side, member.key };
    });
    return { underKey.begin(), underKey.end() };
}

static LayoutID nearestClassWithMemberUnderInLinkTables(const LinkTables& tables, LayoutID layout, ClassSide side, PropertyKeyID key, bool& isKnown)
{
    std::span<const uint32_t> underKey = tables.membersUnderKeyAlongRows(side, key);
    uint32_t placeInRows = tables.placesOfLayoutsInRows[layout.index()];
    auto behind = std::ranges::upper_bound(underKey, placeInRows, { }, [&](uint32_t place) {
        return tables.placesOfLayoutsInRows[tables.members[place].classLayout.index()];
    });
    isKnown = true;
    for (uint32_t numberAsked = 0; behind != underKey.begin(); ++numberAsked) {
        LayoutID candidate = tables.members[*--behind].classLayout;
        if (placeInRows < tables.endsOfLayoutsBelowInRows[candidate.index()])
            return candidate;
        if (numberAsked == tables.numbersOfClassesAbove[layout.index()]) {
            isKnown = false;
            return { };
        }
    }
    return { };
}

bool LinkTables::someClassAtOrAboveDeclares(const Program& program, std::span<const StretchOfRows> stretches, ClassSide side, PropertyKeyID key, uint32_t& classesAsked) const
{
    std::span<const uint32_t> underKey = membersUnderKeyAlongRows(side, key);
    if (underKey.empty())
        return false;
    auto placeOfDeclarer = [&](uint32_t place) {
        return placesOfLayoutsInRows[members[place].classLayout.index()];
    };
    return std::ranges::any_of(stretches, [&](const StretchOfRows& stretch) {
        classesAsked += 2;
        auto firstInStretch = std::ranges::lower_bound(underKey, stretch.first, { }, placeOfDeclarer);
        if (firstInStretch != underKey.end() && placeOfDeclarer(*firstInStretch) < stretch.end)
            return true;
        return !findsNothing(program, stretch.layoutAtFirst, side, key, Of::HolderAndAbove);
    });
}

std::span<const uint32_t> LinkTables::membersUnder(LayoutID classLayout, ClassSide side, PropertyKeyID key) const
{
    RELEASE_ASSERT(membersByKey.size() == members.size());
    size_t sideOfClass = sideOfClassInLinkTables(classLayout, side);
    if (sideOfClass + 1 >= firstMemberByKeyOfSide.size())
        return { };
    auto found = std::ranges::equal_range(membersByKey.span().subspan(firstMemberByKeyOfSide[sideOfClass], firstMemberByKeyOfSide[sideOfClass + 1] - firstMemberByKeyOfSide[sideOfClass]), key, { }, [&](uint32_t place) {
        return members[place].key;
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

bool LinkTables::holderIsChangedInSightAlone(const Program& program, LayoutID classLayout, ClassSide side) const
{
    if (classLayout.index() >= layouts.size())
        return false;
    const Layout& written = program.at(classLayout);
    if (written.kind != LayoutKind::Instance || written.isDerived() || written.has(LayoutFlags::IsOfCreatedObject))
        return false;
    const LinkedLayout& linked = layouts[classLayout.index()];
    return !linked.reachesUnseenCode && (side == ClassSide::Prototype ? linked.listsEveryMemberAbove : linked.listsEveryStaticMember);
}

bool LinkTables::mayBeLaidOnPrototypeUnderUnknownKey(const Program& program, LayoutID constructorFunction, PropertyKeyID key) const
{
    auto laid = std::ranges::equal_range(namesOnPrototypes, std::pair { constructorFunction, PropertyKeyID() }, { }, [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    });
    return std::ranges::any_of(laid, [&](const LinkedNameOnPrototype& name) {
        return !name.isUnderSymbolsAlone || !key || program.at(key).kind != PropertyKeyKind::Text;
    });
}

bool LinkTables::memberHoldsOneFunction(const Program& program, LayoutID classLayout, ClassSide side, PropertyKeyID key, FunctionID written) const
{
    if (program.at(classLayout).has(LayoutFlags::IsConstructorFunction)) {
        auto placeOf = [](const LinkedNameOnPrototype& name) {
            return std::pair { name.layout, name.key };
        };
        if (mayBeLaidOnPrototypeUnderUnknownKey(program, classLayout, key))
            return false;
        auto laid = std::ranges::equal_range(namesOnPrototypes, std::pair { classLayout, key }, { }, placeOf);
        return side == ClassSide::Prototype && !laid.empty() && std::ranges::all_of(laid, [&](const LinkedNameOnPrototype& name) {
            return name.function && name.function == written;
        });
    }
    std::span<const uint32_t> places = membersUnder(classLayout, side, key);
    return !places.empty() && std::ranges::all_of(places, [&](uint32_t place) {
        return members[place].holdsOneFunction();
    });
}

std::optional<FindsNothing> LinkTables::findsNothing(const Program& program, LayoutID layout, ClassSide side, PropertyKeyID key, Of of) const
{
    if (!key || layout.index() >= layouts.size())
        return std::nullopt;
    const LinkedLayout& linked = layouts[layout.index()];
    if (of == Of::ObjectItself) {
        if (linked.isFilledUnderComputedKeys || linked.possibleChanges.containsAny({ LayoutChange::GainsProperty, LayoutChange::GetsAccessor }) || findField(program, layout, key) || std::ranges::contains(appendedKeysOf(layout), key))
            return std::nullopt;
        return FindsNothing();
    }
    if (classHolderIsMadeWithName(program, side, key))
        return std::nullopt;
    if (side == ClassSide::Static && program.at(layout).has(LayoutFlags::IsConstructorFunction)) {
        if (key == program.keyOf(BuiltinNames::arguments) || key == program.keyOf(BuiltinNames::caller) || !closuresLackName(program.at(layout).constructor, key))
            return std::nullopt;
        return FindsNothing();
    }
    if (!(side == ClassSide::Prototype ? linked.listsEveryMemberAbove : linked.listsEveryStaticMember) || linked.namesAreDefinedPastLists[static_cast<size_t>(side)])
        return std::nullopt;

    RELEASE_ASSERT(membersByKey.size() == members.size());
    bool someClassHasMemberUnderKey = keysOfMembers[static_cast<size_t>(side)].get(key.index());
    bool asksMembersOfEachClass = someClassHasMemberUnderKey;
    if (someClassHasMemberUnderKey && of == Of::HolderAndAbove && !linked.hasConstructorFunctionInRow && layout.index() < numbersOfClassesAbove.size() && numbersOfClassesAbove[layout.index()] >= fewestClassesAboveAskedByKeyInLinkTables) {
        bool isKnown = false;
        if (nearestClassWithMemberUnderInLinkTables(*this, layout, side, key, isKnown))
            return std::nullopt;
        asksMembersOfEachClass = !isKnown;
    }
    bool asksStaticFields = side == ClassSide::Static && (liesInThread(key) || keysOfStaticFields.get(key.index()));
    if (!asksStaticFields && !asksMembersOfEachClass && !linked.hasConstructorFunctionInRow)
        return FindsNothing();

    auto isLaidOnPrototypeOf = [&](LayoutID function, PropertyKeyID name) {
        return std::ranges::binary_search(namesOnPrototypes, std::pair { function, name }, { }, [](const LinkedNameOnPrototype& laid) {
            return std::pair { laid.layout, laid.key };
        });
    };
    for (LayoutID current = layout; current; current = of == Of::HolderAndAbove ? program.at(current).base : LayoutID()) {
        const Layout& written = program.at(current);
        if (side == ClassSide::Prototype && (isLaidOnPrototypeOf(current, key) || mayBeLaidOnPrototypeUnderUnknownKey(program, current, key)))
            return std::nullopt;
        if (written.has(LayoutFlags::IsConstructorFunction)) {
            if (side == ClassSide::Static)
                return std::nullopt;
            continue;
        }
        if (asksMembersOfEachClass && !membersUnder(current, side, key).empty())
            return std::nullopt;
        if (asksStaticFields && written.findStaticField(key))
            return std::nullopt;
    }
    return FindsNothing();
}

bool LinkTables::findFunctionsLaidAbove(const Program& program, LayoutID layout, PropertyKeyID key, Vector<FunctionID, 4>& functions) const
{
    if (!key || layout.index() >= layouts.size() || !layouts[layout.index()].listsEveryMemberAbove)
        return false;
    auto placeOf = [](const LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    };
    for (LayoutID current = layout; current; current = program.at(current).base) {
        if (mayBeLaidOnPrototypeUnderUnknownKey(program, current, key))
            return false;
        for (const LinkedNameOnPrototype& laid : std::ranges::equal_range(namesOnPrototypes, std::pair { current, key }, { }, placeOf)) {
            if (!laid.function)
                return false;
            functions.append(laid.function);
        }
        if (program.at(current).has(LayoutFlags::IsConstructorFunction))
            continue;
        std::span<const uint32_t> places = membersUnder(current, ClassSide::Prototype, key);
        if (places.empty() && classHolderIsMadeWithName(program, ClassSide::Prototype, key))
            return false;
        for (uint32_t place : places) {
            const LinkedMember& member = members[place];
            if (member.kind != MethodKind::Method || !member.holdsOneFunction())
                return false;
            functions.append(member.function);
        }
    }
    return true;
}

bool LinkTables::findPrimitivesStoredInSuperclasses(const Program& program, LayoutID layout, PropertyKeyID key, Vector<TypeID, 4>& found, bool* mayStopStores) const
{
    if (!key || layout.index() >= layouts.size() || !layouts[layout.index()].listsEveryMemberAbove || layouts[layout.index()].namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Prototype)] || classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return false;
    auto placeOf = [](const LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    };
    for (LayoutID current = layout; current; current = program.at(current).base) {
        if (mayBeLaidOnPrototypeUnderUnknownKey(program, current, key))
            return false;
        for (const LinkedNameOnPrototype& laid : std::ranges::equal_range(namesOnPrototypes, std::pair { current, key }, { }, placeOf)) {
            if (!laid.primitives)
                return false;
            found.append(laid.primitives);
            if (mayStopStores)
                *mayStopStores |= laid.mayBeReadOnly;
        }
        if (!program.at(current).has(LayoutFlags::IsConstructorFunction) && !membersUnder(current, ClassSide::Prototype, key).empty())
            return false;
    }
    return true;
}

bool isSymbolOfLibrary(const Program& program, PropertyKeyID symbol)
{
    const PropertyKey& key = program.at(symbol);
    if (key.kind != PropertyKeyKind::CreatedSymbol || !key.module)
        return false;
    const Module& creator = program.at(key.module);
    return creator.kind == ModuleKind::RuntimeSource || creator.flags.contains(ModuleFlags::IsLibrary);
}

bool LinkTables::lookupUnderKeyMayRunPreparation(const Program& program, PropertyKeyID key) const
{
    if (!isSymbolOfLibrary(program, key))
        return false;
    return everySymbolOfLibraryMayHavePreparation || std::ranges::binary_search(symbolsWithPreparation, key);
}

Vector<PropertyKeyID> LinkFlow::symbolsWithPreparation() const
{
    Vector<PropertyKeyID> symbols;
    for (uint32_t bits : m_symbolsWithPreparation)
        symbols.append(PropertyKeyID::fromBits(bits));
    std::ranges::sort(symbols);
    return symbols;
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

    RELEASE_ASSERT(membersByKey.size() == members.size());
    if (side == ClassSide::Prototype && program.at(layout).has(LayoutFlags::IsConstructorFunction))
        return findMethodStoredFirst(program, layout, key);
    if (!keysOfMembers[static_cast<size_t>(side)].get(key.index()))
        return { };
    std::optional<bool> holderIsMadeWithName;
    LayoutID firstClassAsked = layout;
    if (!layouts[layout.index()].hasConstructorFunctionInRow && layout.index() < numbersOfClassesAbove.size() && numbersOfClassesAbove[layout.index()] >= fewestClassesAboveAskedByKeyInLinkTables && !classHolderIsMadeWithName(program, side, key)) {
        bool isKnown = false;
        LayoutID nearest = nearestClassWithMemberUnderInLinkTables(*this, layout, side, key, isKnown);
        if (isKnown && !nearest)
            return { };
        if (isKnown)
            firstClassAsked = nearest;
    }
    auto isLaidOnPrototypeOf = [&](LayoutID holder) {
        return std::ranges::binary_search(namesOnPrototypes, std::pair { holder, key }, { }, [](const LinkedNameOnPrototype& laid) {
            return std::pair { laid.layout, laid.key };
        });
    };
    for (LayoutID current = firstClassAsked; current; current = program.at(current).base) {
        if (side == ClassSide::Prototype && (program.at(current).has(LayoutFlags::IsConstructorFunction) || isLaidOnPrototypeOf(current)))
            return { };
        MemberFound found;
        unsigned numberFound = 0;
        for (uint32_t place : membersUnder(current, side, key)) {
            const LinkedMember& member = members[place];
            if (!member.holdsOneFunction())
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
            if (!holderIsMadeWithName)
                holderIsMadeWithName = classHolderIsMadeWithName(program, side, key);
            if (*holderIsMadeWithName)
                return { };
            continue;
        }
        if (numberFound != static_cast<unsigned>(!!found.method) + static_cast<unsigned>(!!found.getter) + static_cast<unsigned>(!!found.setter) || (found.method && numberFound > 1))
            return { };
        found.kind = found.method ? MemberFound::Kind::Method : MemberFound::Kind::Accessor;
        return found;
    }
    return { };
}

MemberFound LinkTables::findMethodStoredFirst(const Program& program, LayoutID layout, PropertyKeyID key) const
{
    const LinkedLayout& linked = layouts[layout.index()];
    if (!linked.prototypeObjectIsSettledAheadOfEveryObject || linked.namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Prototype)])
        return { };
    if (mayBeLaidOnPrototypeUnderUnknownKey(program, layout, key) || classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return { };
    auto laid = std::ranges::equal_range(namesOnPrototypes, std::pair { layout, key }, { }, [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    });
    MemberFound found;
    bool isLaidFirst = false;
    for (const LinkedNameOnPrototype& name : laid) {
        if (!name.function || name.mayHoldAccessor || (found.method && found.method != name.function))
            return { };
        found.method = name.function;
        isLaidFirst |= name.isLaidFirst;
    }
    if (!isLaidFirst)
        return { };
    found.kind = MemberFound::Kind::Method;
    return found;
}

LinkTables conservativeLinkTables(const Program& program)
{
    ParallelLoops onOneThread(1);
    return conservativeLinkTables(program, onOneThread);
}

LinkTables conservativeLinkTables(const Program& program, const ParallelLoops& loops)
{
    LinkTables tables;
    size_t numberOfFunctions = program.functions.size();
    size_t numberOfLayouts = program.layouts.size();

    tables.functions.grow(loops, numberOfFunctions);
    tables.firstCallOfCaller.fill(0, numberOfFunctions + 1);
    tables.firstCaptureOfFunction.fill(0, numberOfFunctions + 1);
    forEachRangeOfIndicesInParallel(loops, numberOfFunctions, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const Function& function = program.functions[index];
            tables.functions[index].numberOfParameters = function.numberOfEntryParameters();
            tables.firstCaptureOfFunction[index + 1] = static_cast<uint32_t>(function.captures.size());
        }
    });
    uint32_t numberOfParameters = 0;
    for (size_t index = 0; index < numberOfFunctions; ++index) {
        LinkedFunction& linked = tables.functions[index];
        linked.firstParameter = numberOfParameters;
        numberOfParameters += linked.numberOfParameters;
        tables.firstCaptureOfFunction[index + 1] += tables.firstCaptureOfFunction[index];
    }
    tables.parameters.grow(loops, numberOfParameters);
    tables.captures.grow(loops, tables.firstCaptureOfFunction.last());
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
    tables.fields.grow(loops, numberOfFields);
    tables.variables.grow(loops, program.variables.size());
    tables.layouts.grow(loops, numberOfLayouts);
    return tables;
}

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

    void writeWhatMakesNoType();
    void writeWhatMakesTypes(TypeArena&);
    void findFunctionsThatRunAtMostOnce();
    void appendExclusiveCallees(FunctionID caller, std::span<const LinkedCall> callsOfCaller, std::span<const uint32_t> numberOfCallsThatEnter, Vector<FunctionID>& callees) const;
    FunctionID onlyForwardedCallee(FunctionID caller, const SummaryCall&, const LinkFlow::CalleesOfCall&, std::span<const uint32_t> numberOfCallsThatEnter) const;
    void writeProofsThatMakeNoType();
    void weighLeaves();
    BitVector functionsWhoseCallersAreAllKnown();
    void takeGettersReadOffTables(Vector<LinkedGetterAtRead>&& reads) { m_gettersReadOffTables = WTF::move(reads); }
    bool everyCallerIsKnownOf(size_t function) const { return m_flow.reached.get(function) && !m_flow.lostFunctions.get(function) && !m_flow.escapedFunctions.get(function) && !m_functionsEnteredByHost.get(function) && !m_functionsThatNoSummaryNames.get(function); }
    void goOnWith(const ParallelLoops& loops) { m_loops = &loops; }
    void leaveToProvingReadingWhatItNames(const LinkTables& proving) { m_proving = &proving; }
    void writeReferrers();

private:
    static constexpr uint32_t none = UINT32_MAX;

    struct SiteOfCall {
        ValueID site;
        FunctionID callee;
        bool isInTailPosition;
        bool mayComeAgain;
        bool isMadeByProtocol;
        bool asksForIterator;
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
    void findExportsThatStayIn();
    void findFunctionsTakenOutOfRequiredFiles();
    void findFunctionsWrittenOverBuiltins();
    BinarySemaphore m_functionsThatNoSummaryNamesAreFound;
    void findLayoutsWithUnlistedNames();
    bool mayFindUnlistedNames(const LinkType& receiver) const;
    FunctionID oneCalleeOf(FunctionID caller, const SummaryCall&, const LinkFlow::CalleesOfCall&) const;
    void findRingsOfTailCalls();
    uint32_t weighAsLeaf(FunctionID, unsigned depth, bool& levelsRanOut);

    BitVector m_functionsThatAreNoLeaves;
    void writeCalls();
    void writeAccessesUnderOneKey();
    void writeGettersExpectedAtReads();
    void writeFunctionsInRecords();
    void layGettersReadOffTablesAmongRows();
    void writeCallsThatFindBuiltins();
    void noteCallersThatFindBuiltins();
    void writeResultsOfReadsByKey();
    void writeNativeCallResults();
    void writeContentsAtOperations();
    void writeReceiversThatStayAsMade();
    void writeReadsOffViewsOfWrittenLengths();
    void writeFunctions();
    void writeCatches();
    void writeParameters(FunctionID, const LinkedFunction&);
    void indexFunctionsThatAreHeld();
    void makeGroups();

    TypeID unlessAnything(TypeID) const;
    TypeID widened(TypeID);
    void findLiteralsThatMembersAreWrittenIn();
    TypeID typeOfLiteralAndWhatIsCreatedBelow(FunctionID member);
    bool writesWhatIsExpected() const { return m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls; }
    Vector<LinkedGetterAtRead> m_gettersReadOffTables;
    ALWAYS_INLINE bool holdsOnlyInstancesThatStayAsMade(const LinkType& type) const { return holdsOnlyInstancesWithout(type, std::array { SlotOfContents::underUnknownNames(), SlotOfContents::noLongerAsMade(), SlotOfContents::mayHaveNumberNames() }); }
    ALWAYS_INLINE bool holdsOnlyPlainInstances(const LinkType& type) const { return holdsOnlyInstancesWithout(type, std::array { SlotOfContents::underUnknownNames(), SlotOfContents::noLongerAsMade() }); }
    ALWAYS_INLINE bool holdsOnlyInstancesWithout(const LinkType& type, const auto& slots) const
    {
        if (type.classes != LinkType::Classes::Site || type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin)
            return false;
        if (m_flow.hasEscaped(type.site))
            return false;
        return std::ranges::all_of(slots, [&](SlotOfContents slot) {
            FlowNodeID held = m_flow.findContentsOf(type, slot);
            return !held || (held != m_flow.unseen() && m_flow.typeAt(held).isNever());
        });
    }
    TypeID typeIDOf(const LinkType&);
    TypeID typeIDOfCapture(FunctionID, uint32_t capture);

    FunctionID closureAloneAt(FlowNodeID node) const { return m_flow.closureAloneAt(node); }
    bool mayTakeArrivals(FunctionID id) const { return m_tables.function(id).everyCallerIsKnown && m_tables.function(id).everyCallHasOneCallee; }
    FunctionID addGroup(LinkedGroup::Kind, uint32_t subject = 0);
    void forEachNodeArrivingAt(FlowNodeID, const Invocable<void(FunctionID)> auto&) const;
    void forEachFunctionOfType(TypeID, const Invocable<void(FunctionID)> auto&) const;
    template<typename Row>
    class RowsOfRisingFunctions {
    public:
        RowsOfRisingFunctions(std::span<const Row> rows, FunctionID first)
            : m_rows(rows.subspan(std::ranges::lower_bound(rows, first, { }, &Row::function) - rows.begin()))
        {
        }

        std::span<const Row> takeRowsOf(FunctionID function)
        {
            size_t first = 0;
            while (first < m_rows.size() && m_rows[first].function < function)
                ++first;
            size_t end = first;
            while (end < m_rows.size() && m_rows[end].function == function)
                ++end;
            std::span<const Row> taken = m_rows.subspan(first, end - first);
            m_rows = m_rows.subspan(end);
            return taken;
        }

    private:
        std::span<const Row> m_rows;
    };
    struct TablesReadInOrderOfFunctions {
        RowsOfRisingFunctions<LinkedCatch> catches;
        RowsOfRisingFunctions<LinkedGetterAtRead> gettersExpectedAtReads;
        RowsOfRisingFunctions<LinkedResultOfRead> resultsOfReadsByKey;
        RowsOfRisingFunctions<LinkedContentsAtOperation> contentsAtOperations;
    };
    void forEachNodeNamedBy(FunctionID referrer, TablesReadInOrderOfFunctions&, const Invocable<void(FunctionID)> auto&) const;
    void forEachRecordReadUnderUnknownKeyBy(FunctionID reader, const Invocable<void(LayoutID)> auto&) const;
    void forEachFunctionInFieldsOf(LayoutID record, const Invocable<void(FunctionID)> auto&) const;
    void forEachFunctionListedWhateverCameFirst(const LinkFlow::CalleesOfCall&, const Invocable<void(FunctionID)> auto&) const;
    void forEachFunctionNamedBySharedCall(uint32_t shared, const Invocable<void(FunctionID)> auto&) const;
    void forEachMemberOf(const LinkedGroup&, const Invocable<void(FunctionID)> auto&) const;

    const LinkFlow& m_flow;
    const Program& m_program;
    const LinkTables* m_proving { nullptr };
    mutable std::atomic<uint64_t> m_rowsLeftToProvingReading { 0 };
    mutable std::atomic<uint64_t> m_rowsNamedByHeldReading { 0 };
    const ParallelLoops& loops() const { return m_loops ? *m_loops : m_flow.loopsOfReading(); }
    const ParallelLoops* m_loops { nullptr };
    TypeArena* m_typesAtHand { nullptr };
    static thread_local TypesOfThreadInLink s_typesOfThread;
    TypeArena& typesAtHand() const { return s_typesOfThread.types ? *s_typesOfThread.types : *m_typesAtHand; }
    TypesOfFlowTurnedOnce& typesTurned() { return s_typesOfThread.turned ? *s_typesOfThread.turned : m_typesTurned; }
    void writeFunction(FunctionID);
    TypesOfFlowTurnedOnce m_typesTurned;
    LinkTables& m_tables;
    uint32_t m_numberOfFunctions;
    Vector<LayoutID> m_literalsThatMembersAreWrittenIn;
    UncheckedKeyHashMap<uint32_t, Vector<LayoutID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_literalsWithWhatIsCreatedBelow;

    BitVector m_functionsEnteredByHost;
    BitVector m_functionsThatNoSummaryNames;
    BitVector m_functionsThatTextsOfLibraryName;
    BitVector m_layoutsWithUnlistedNames;
    BitVector m_familiesWithUnlistedNames;
    bool m_lostLayoutsHaveUnlistedNames { false };
    BitVector m_functionsSharingCalls;
    BitVector m_functionsSharingSeveralCalls;

    Vector<FunctionUnderKey> m_functionsUnderKeys;
    Vector<FunctionOfClass> m_functionsOfClasses;
    Vector<FunctionOfClass> m_functionsThatSlotsOfRecordsMayDefer;

    FunctionID m_anybody;
    Vector<FunctionID> m_groupOfConstructors;
    Vector<uint32_t> m_firstFamilyRightBelow;
    Vector<FamilyID> m_familiesRightBelow;
    Vector<uint32_t> m_firstLayoutNearestToFamily;
    Vector<LayoutID> m_layoutsNearestToFamily;
    Vector<FunctionID> m_groupOfSharedCall;
    Vector<uint32_t> m_recordsReadUnderUnknownKeys;
    Vector<FunctionID> m_groupOfFunctionsInFields;
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

BitVector WriterOfLink::functionsWhoseCallersAreAllKnown()
{
    m_functionsThatNoSummaryNamesAreFound.wait();
    BitVector known;
    known.ensureSize(m_numberOfFunctions);
    for (size_t index : m_flow.reached) {
        if (everyCallerIsKnownOf(index))
            known.quickSet(index);
    }
    return known;
}

void WriterOfLink::writeWhatMakesNoType()
{
    findFunctionsThatNoSummaryNames();
    m_functionsThatNoSummaryNamesAreFound.signal();
    findLayoutsWithUnlistedNames();
    writeCalls();
    writeAccessesUnderOneKey();
    writeGettersExpectedAtReads();
    writeFunctionsInRecords();
    writeCallsThatFindBuiltins();
    noteCallersThatFindBuiltins();
}

void WriterOfLink::writeWhatMakesTypes(TypeArena& types)
{
    SetForScope typesOfTurn(m_typesAtHand, &types);
    writeFunctions();
    writeCoroutinesOfLink(m_program, m_flow, types, m_tables);
    writeCatches();
    writeResultsOfReadsByKey();
    writeNativeCallResults();
    writeContentsAtOperations();
}

void WriterOfLink::writeProofsThatMakeNoType()
{
    RELEASE_ASSERT(!writesWhatIsExpected());

    findExportsThatStayIn();
    writeReceiversThatStayAsMade();
    writeReadsOffViewsOfWrittenLengths();
    findRingsOfTailCalls();
}

void WriterOfLink::findExportsThatStayIn()
{
    m_tables.exportsThatStayIn = BitVector(m_program.variables.size());
    for (const Module& module : m_program.modules) {
        if (module.kind == ModuleKind::CommonJs && module.exportsVariable && !m_flow.escapedVariables.get(module.exportsVariable.index()))
            m_tables.exportsThatStayIn.quickSet(module.exportsVariable.index());
        if (module.kind != ModuleKind::SourceText || module.flags.contains(ModuleFlags::IsLibrary))
            continue;
        for (const ModuleExport& entry : module.exports) {
            if (entry.kind == ModuleExportKind::Variable && entry.variable && !m_flow.exportsShown.get(entry.variable.index()) && !m_flow.escapedVariables.get(entry.variable.index()))
                m_tables.exportsThatStayIn.quickSet(entry.variable.index());
        }
    }
}

static ValueID valueBehindCastsAndTestsInLink(const BodyIndex& index, ValueID value)
{
    while (value) {
        value = index.stripCasts(value);
        ValueID tested = index.valueNarrowedInto(value);
        if (!tested)
            break;
        value = tested;
    }
    return value;
}

static ValueID recordThatBodyHandsBackInLink(const BodyIndex& index)
{
    ValueID record;
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        BodyTerminator end = index.terminator(BlockID(block));
        if (end.kind() != TerminatorKind::Return)
            continue;
        if (record)
            return { };
        record = end.operand(0);
    }
    return record;
}

namespace {

struct PlaceInTextInLink {
    FunctionID body;
    ValueID value;

    explicit operator bool() const { return !!value; }
};

struct MemberInTextInLink {
    uint32_t body { 0 };
    uint32_t record { 0 };
    uint32_t key { 0 };
    uint32_t store { 0 };
    FunctionID function;

    friend constexpr auto operator<=>(const MemberInTextInLink&, const MemberInTextInLink&) = default;
};

class TextsOfModulesInLink {
public:
    TextsOfModulesInLink(const Program& program, const BodyIndexTable& bodies, const BitVector& reached)
        : m_program(program)
        , m_bodies(bodies)
    {
        m_lastStores.fill(PlaceInTextInLink { }, program.variables.size());
        Vector<PlaceInTextInLink> storesUnderNames;
        for (const Module& module : program.modules) {
            const BodyIndex* index = module.body && reached.get(module.body.index()) ? bodies.find(module.body) : nullptr;
            if (!index)
                continue;
            for (uint32_t number = 0; number < index->numberOfValues(); ++number) {
                if (index->isParameter(ValueID(number)))
                    continue;
                switch (index->opcodeOf(ValueID(number))) {
                case Opcode::PutModuleVariable: {
                    auto store = index->value(ValueID(number)).as<Opcode::PutModuleVariable>();
                    if (program.at(program.at(store.variable()).module).body == module.body)
                        m_lastStores[store.variable().index()] = { module.body, store.valueOperand() };
                    break;
                }
                case Opcode::NewObject:
                case Opcode::PropSet:
                case Opcode::PropDefine:
                    storesUnderNames.append({ module.body, ValueID(number) });
                    break;
                default:
                    break;
                }
            }
        }
        for (PlaceInTextInLink place : storesUnderNames)
            noteMembersLaidAt(place);
        std::ranges::sort(m_members);
        m_membersAreRead = true;
    }

    PlaceInTextInLink lastStoreOf(VariableID variable) const { return m_lastStores[variable.index()]; }

    PlaceInTextInLink recordAt(PlaceInTextInLink place, unsigned linksLeft = 4) const
    {
        const BodyIndex& index = m_bodies.at(place.body);
        ValueID value = valueBehindCastsAndTestsInLink(index, place.value);
        if (!value || index.isParameter(value) || !linksLeft)
            return { };
        switch (index.opcodeOf(value)) {
        case Opcode::FinishObject:
            return recordAt({ place.body, index.value(value).operand(0) }, linksLeft);
        case Opcode::NewObject:
            return index.value(value).as<Opcode::NewObject>().layout() ? PlaceInTextInLink { place.body, value } : PlaceInTextInLink { };
        case Opcode::GetModuleVariable:
            if (PlaceInTextInLink store = lastStoreOf(index.value(value).variable()))
                return recordAt(store, linksLeft - 1);
            return { };
        case Opcode::RequireModule: {
            ModuleID file = index.value(value).as<Opcode::RequireModule>().module();
            const BodyIndex* asked = objectOfLibraryFileIsResultOfBody(m_program, file) ? m_bodies.find(m_program.at(file).body) : nullptr;
            ValueID handedBack = asked ? recordThatBodyHandsBackInLink(*asked) : ValueID();
            return handedBack ? recordAt({ m_program.at(file).body, handedBack }, linksLeft - 1) : PlaceInTextInLink { };
        }
        default:
            return { };
        }
    }

    FunctionID functionAt(PlaceInTextInLink place, unsigned linksLeft = 4) const
    {
        const BodyIndex& index = m_bodies.at(place.body);
        ValueID value = valueBehindCastsAndTestsInLink(index, place.value);
        if (!value || index.isParameter(value) || !linksLeft)
            return { };
        switch (index.opcodeOf(value)) {
        case Opcode::NewClosure:
            return index.value(value).as<Opcode::NewClosure>().function();
        case Opcode::NewClass:
            return functionAt({ place.body, index.value(value).as<Opcode::NewClass>().constructorOperand() }, linksLeft);
        case Opcode::GetModuleVariable: {
            const Variable& variable = m_program.at(index.value(value).variable());
            if (variable.initialization == VariableInitialization::Function)
                return variable.function;
            PlaceInTextInLink store = lastStoreOf(index.value(value).variable());
            return store ? functionAt(store, linksLeft - 1) : FunctionID();
        }
        case Opcode::PropGet: {
            if (!m_membersAreRead || index.value(value).isFreeName())
                return { };
            auto read = index.value(value).as<Opcode::PropGet>();
            PlaceInTextInLink record = recordAt({ place.body, read.objectOperand() }, linksLeft);
            return record ? functionLaidUnder(record, read.key()) : FunctionID();
        }
        default:
            return { };
        }
    }

    void forEachFunctionLaidIn(PlaceInTextInLink record, NOESCAPE const Invocable<void(uint32_t key, FunctionID)> auto& functor) const
    {
        auto first = std::ranges::lower_bound(m_members, std::pair(record.body.index(), record.value.index()), { }, [](const MemberInTextInLink& member) {
            return std::pair(member.body, member.record);
        });
        for (auto member = first; member != m_members.end() && member->body == record.body.index() && member->record == record.value.index(); ++member) {
            bool isLastUnderKey = member + 1 == m_members.end() || (member + 1)->body != member->body || (member + 1)->record != member->record || (member + 1)->key != member->key;
            if (isLastUnderKey && member->function)
                functor(member->key, member->function);
        }
    }

    LayoutID layoutOf(PlaceInTextInLink record) const { return m_bodies.at(record.body).value(record.value).as<Opcode::NewObject>().layout(); }

private:
    FunctionID functionLaidUnder(PlaceInTextInLink record, PropertyKeyID key) const
    {
        FunctionID laid;
        forEachFunctionLaidIn(record, [&](uint32_t under, FunctionID function) {
            if (under == key.bits())
                laid = function;
        });
        return laid;
    }

    void noteMembersLaidAt(PlaceInTextInLink place)
    {
        const BodyIndex& index = m_bodies.at(place.body);
        BodyValue value = index.value(place.value);
        if (value.opcode() == Opcode::NewObject) {
            auto made = value.as<Opcode::NewObject>();
            if (!made.layout())
                return;
            std::span<const uint32_t> fields = made.fields();
            std::span<const ValueID> values = value.variadicOperands();
            const Layout& layout = m_program.at(made.layout());
            for (size_t position = 0; position < fields.size() && position < values.size(); ++position) {
                if (FunctionID function = functionAt({ place.body, values[position] }))
                    m_members.append({ place.body.index(), place.value.index(), layout.fields[fields[position]].key.bits(), place.value.index(), function });
            }
            return;
        }
        bool isStore = value.opcode() == Opcode::PropSet;
        if (isStore && value.isFreeName())
            return;
        PlaceInTextInLink record = recordAt({ place.body, value.operand(0) });
        if (!record || record.body != place.body)
            return;
        PropertyKeyID key = isStore ? value.as<Opcode::PropSet>().key() : value.as<Opcode::PropDefine>().key();
        ValueID stored = isStore ? value.as<Opcode::PropSet>().valueOperand() : value.as<Opcode::PropDefine>().valueOperand();
        m_members.append({ record.body.index(), record.value.index(), key.bits(), place.value.index(), functionAt({ place.body, stored }) });
    }

    const Program& m_program;
    const BodyIndexTable& m_bodies;
    Vector<PlaceInTextInLink> m_lastStores;
    Vector<MemberInTextInLink> m_members;
    bool m_membersAreRead { false };
};

} // anonymous namespace

void WriterOfLink::findFunctionsTakenOutOfRequiredFiles()
{
    TextsOfModulesInLink texts(m_program, bodiesLookedAt(m_flow.looks), m_flow.reached);
    for (uint32_t number = 0; number < m_program.variables.size(); ++number) {
        VariableID variable(number);
        PlaceInTextInLink store = texts.lastStoreOf(variable);
        if (!store || m_program.at(variable).initialization == VariableInitialization::Function)
            continue;
        if (PlaceInTextInLink record = texts.recordAt(store)) {
            if (!m_flow.escapedLayouts.get(texts.layoutOf(record).index()))
                continue;
            texts.forEachFunctionLaidIn(record, [&](uint32_t key, FunctionID function) {
                m_tables.functionsLaidUnderNamesOfVariables.append({ number, key, function });
            });
            continue;
        }
        if (!m_flow.typeAt(m_flow.variable(variable)).hasUnknownObjects)
            continue;
        if (FunctionID taken = texts.functionAt(store))
            m_tables.functionsTakenOutOfRequiredFiles.append({ variable, taken });
    }
    std::ranges::sort(m_tables.functionsLaidUnderNamesOfVariables);
}

void WriterOfLink::findFunctionsWrittenOverBuiltins()
{
    Vector<LinkedFunctionWrittenOverBuiltin>& written = m_tables.functionsWrittenOverBuiltins;
    written.shrink(0);
    for (const WriteFollowedToBuiltin& write : m_tables.lookups.writesFollowedToBuiltins) {
        const BodyIndex* body = bodiesLookedAt(m_flow.looks).find(write.function);
        if (!body || body->isParameter(write.site) || body->opcodeOf(write.site) != Opcode::PropSet)
            continue;
        auto store = body->value(write.site).as<Opcode::PropSet>();
        ValueID stored = body->stripCasts(store.valueOperand());
        FunctionID function = stored && !body->isParameter(stored) && body->opcodeOf(stored) == Opcode::NewClosure ? body->value(stored).function() : FunctionID();
        written.append({ m_program.builtins().canonicalID(write.holder), store.key().bits(), function });
    }
    std::ranges::sort(written);
    written.shrink(std::ranges::unique(written).begin() - written.begin());
    Vector<LinkedFunctionWrittenOverBuiltin> writtenAlone;
    auto isUnderSameName = [](const LinkedFunctionWrittenOverBuiltin& one, const LinkedFunctionWrittenOverBuiltin& other) {
        return one.holder == other.holder && one.key == other.key;
    };
    for (size_t place = 0; place < written.size(); ++place) {
        bool hasNeighbor = (place && isUnderSameName(written[place - 1], written[place])) || (place + 1 < written.size() && isUnderSameName(written[place], written[place + 1]));
        if (written[place].function && !hasNeighbor)
            writtenAlone.append(written[place]);
    }
    written = WTF::move(writtenAlone);
}

void WriterOfLink::findFunctionsThatNoSummaryNames()
{
    m_functionsEnteredByHost = functionsCalledByHost(m_program, loops());
    m_functionsThatNoSummaryNames = m_flow.libraryFunctionsEntered;
    m_functionsThatNoSummaryNames.exclude(m_functionsEnteredByHost);
    for (const RuntimeFunctionEntry& entry : m_program.runtimeFunctions) {
        if (entry.function && !m_flow.libraryFunctionsEntered.get(entry.function.index()))
            m_functionsThatTextsOfLibraryName.set(entry.function.index());
    }
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

bool WriterOfLink::mayFindUnlistedNames(const LinkType& receiver) const
{
    if (receiver.hasUnknownObjects || receiver.builtin || receiver.classes != LinkType::Classes::None)
        return true;

    switch (receiver.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Closure:
        if (m_flow.escapedFunctions.get(receiver.function.index()) || m_flow.lostFunctions.get(receiver.function.index()))
            return true;
        break;
    case LinkType::Functions::Constructors:
    case LinkType::Functions::Set:
    case LinkType::Functions::Lost:
        return true;
    }

    switch (receiver.objects) {
    case LinkType::Objects::None:
        return false;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Prototype:
        return m_layoutsWithUnlistedNames.get(receiver.layout.index());
    case LinkType::Objects::Set:
        return std::ranges::any_of(m_flow.layoutsOfSet(receiver), [&](LayoutID member) {
            return m_layoutsWithUnlistedNames.get(member.index());
        });
    case LinkType::Objects::Family:
    case LinkType::Objects::Prototypes:
        return m_familiesWithUnlistedNames.get(receiver.family.index());
    case LinkType::Objects::Lost:
        return m_lostLayoutsHaveUnlistedNames;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE FunctionID WriterOfLink::oneCalleeOf(FunctionID caller, const SummaryCall& call, const LinkFlow::CalleesOfCall& callees) const
{
    if (!callees.builtins().isEmpty() || callees.mayEnterLost || (callees.mayEnterUnseen && !writesWhatIsExpected()))
        return { };
    FunctionID callee = callees.listed().isEmpty() ? FunctionID() : callees.listed()[0];
    bool listsSeveral = callees.listed().size() > 1;
    for (uint32_t shared : callees.shared()) {
        auto listed = m_flow.calleesOfSharedCallsAsSettled()[shared].listed();
        listsSeveral |= listed.size() > 1 || (listed.size() == 1 && callee && callee != listed[0]);
        if (!listed.isEmpty())
            callee = listed[0];
    }
    if (!callee || listsSeveral || !m_flow.tablesMayName(callee))
        return { };
    const Function& function = m_program.at(callee);
    if (!function.hasBody())
        return { };
    auto typeAt = [&](uint32_t node) -> std::optional<LinkType> {
        if (!node || node == noSummaryNode)
            return std::nullopt;
        return m_flow.typeAt(m_flow.nodeOf(caller, node));
    };

    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        return callee;
    case SummaryCall::Callee::Name: {
        std::optional<LinkType> receiver = typeAt(call.receiver);
        if (!receiver || (!writesWhatIsExpected() && mayFindUnlistedNames(*receiver)))
            return { };
        return isEnteredByCallsAloneForLink(function.kind) ? callee : FunctionID();
    }
    case SummaryCall::Callee::Value: {
        std::optional<LinkType> subject = typeAt(call.subject);
        if (!subject)
            return { };
        LinkType called = *subject;
        called.primitives = 0;
        if (writesWhatIsExpected() && call.isConstruction) {
            called.hasUnknownObjects = false;
            called.holdsLostBuiltins = false;
        }
        LinkType closuresOfCallee;
        closuresOfCallee.functions = LinkType::Functions::Closure;
        closuresOfCallee.function = callee;
        if (called != closuresOfCallee)
            return { };
        return isEnteredByCallsAloneForLink(function.kind) ? callee : FunctionID();
    }
    case SummaryCall::Callee::Builtin:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t WriterOfLink::weighAsLeaf(FunctionID id, unsigned depth, bool& levelsRanOut)
{
    static constexpr unsigned levels = 3;
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

void WriterOfLink::writeCalls()
{
    PropertyKeyID keyOfIterators = m_program.keyOf(m_program.builtins().nameOf(WellKnownSymbol::Iterator));
    Vector<LinkedCall> found = collectInParallel<LinkedCall>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<LinkedCall>& calls) {
        if (!m_flow.reached.get(index))
            return;
        auto noteSharing = [&](FunctionID callee) {
            calls.append(LinkedCall { FunctionID(), ValueID(), callee });
        };
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
            const SummaryCall& made = summary.calls[call];
            FunctionID callee = oneCalleeOf(caller, made, callees);
            sites.append(SiteOfCall { made.site, callee, made.isInTailPosition, made.isInLoop || made.isMadeByProtocol, made.isMadeByProtocol, made.isMadeByProtocol && made.key == keyOfIterators });
            if (callee)
                continue;
            for (FunctionID listed : callees.listed())
                noteSharing(listed);
            for (uint32_t shared : callees.shared())
                noteSharingOfSharedCall(shared);
        }
        if (!std::ranges::is_sorted(sites, { }, &SiteOfCall::site))
            std::ranges::sort(sites, { }, &SiteOfCall::site);

        ValueID siteListedLast;
        size_t end = 0;
        for (size_t first = 0; first < sites.size(); first = end) {
            SiteOfCall leader = sites[first];
            end = first + 1;
            while (end < sites.size() && sites[end].site == leader.site)
                ++end;
            std::span<const SiteOfCall> callsOfSite = sites.span().subspan(first, end - first);
            auto entersOneFunction = [](const SiteOfCall& each) { return !!each.callee; };
            bool oneCallOfProtocolEntersFunction = callsOfSite.size() > 1 && std::ranges::all_of(callsOfSite, &SiteOfCall::isMadeByProtocol) && std::ranges::count_if(callsOfSite, entersOneFunction) == 1
                && std::ranges::find_if(callsOfSite, entersOneFunction)->asksForIterator;
            if (oneCallOfProtocolEntersFunction)
                leader = *std::ranges::find_if(callsOfSite, entersOneFunction);
            if (leader.site && leader.callee && (callsOfSite.size() == 1 || oneCallOfProtocolEntersFunction)) {
                RELEASE_ASSERT(!siteListedLast || siteListedLast < leader.site);
                siteListedLast = leader.site;
                calls.append(LinkedCall { caller, leader.site, leader.callee, leader.isInTailPosition, leader.mayComeAgain, leader.asksForIterator });
                continue;
            }
            for (size_t place = first; place < end; ++place) {
                if (sites[place].callee)
                    noteSharing(sites[place].callee);
            }
        }
    });

    m_functionsSharingCalls = BitVector(m_numberOfFunctions);
    m_functionsSharingSeveralCalls = BitVector(m_numberOfFunctions);
    BitVector sharedCallsOfCallsNotListed(m_flow.calleesOfSharedCallsAsSettled().size());
    for (const LinkedCall& call : found) {
        if (call.caller)
            continue;
        if (!call.callee)
            sharedCallsOfCallsNotListed.quickSet(call.site.index());
        else if (m_functionsSharingCalls.quickSet(call.callee.index()))
            m_functionsSharingSeveralCalls.quickSet(call.callee.index());
    }
    for (size_t shared : sharedCallsOfCallsNotListed) {
        for (FunctionID listed : m_flow.calleesOfSharedCallsAsSettled()[shared].listed()) {
            m_functionsSharingCalls.quickSet(listed.index());
            m_functionsSharingSeveralCalls.quickSet(listed.index());
        }
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

void WriterOfLink::writeAccessesUnderOneKey()
{
    m_tables.symbolsWithPreparation = m_flow.symbolsWithPreparation();
    m_tables.everySymbolOfLibraryMayHavePreparation = m_flow.everySymbolOfLibraryMayHavePreparation();
    m_tables.accessesUnderOneKey = collectInParallel<LinkedAccess>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<LinkedAccess>& accesses) {
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

void WriterOfLink::writeCallsThatFindBuiltins()
{
    if (!writesWhatIsExpected())
        return;
    m_tables.callsThatFindBuiltins = collectInParallel<LinkedCallThatFindsBuiltin>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<LinkedCallThatFindsBuiltin>& calls) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID caller(index);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        uint32_t firstCall = m_flow.firstCallOfFunction[index];
        size_t first = calls.size();
        for (uint32_t call = 0; call < summary.calls.size(); ++call) {
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[firstCall + call];
            if (callees.builtins().size() != 1 || callees.listed().size() > 1 || !callees.shared().isEmpty() || callees.mayEnterLost)
                continue;
            const SummaryCall& record = summary.calls[call];
            if (record.callee != SummaryCall::Callee::Name || record.isMadeByProtocol || record.isConstruction || record.passesList || !record.site || !record.key || !record.receiver || record.receiver == noSummaryNode)
                continue;
            const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(caller, record.receiver));
            if (receiver.objects != LinkType::Objects::Layout || receiver.functions != LinkType::Functions::None || receiver.classes != LinkType::Classes::None || receiver.builtin || !findField(m_program, receiver.layout, record.key))
                continue;
            FunctionID callee = callees.listed().isEmpty() ? FunctionID() : callees.listed()[0];
            if (callee && (!m_flow.tablesMayName(callee) || !m_program.at(callee).hasBody() || !isEnteredByCallsAloneForLink(m_program.at(callee).kind)))
                continue;
            calls.append(LinkedCallThatFindsBuiltin { caller, record.site, callees.builtins()[0], callee });
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

class GettersLaidInLink {
public:
    static ALWAYS_INLINE std::optional<uint32_t> holderOf(const LinkFlow& flow, const LinkType& type)
    {
        if (type.objects != LinkType::Objects::None || type.builtin)
            return std::nullopt;
        if (type.functions == LinkType::Functions::None && type.classes != LinkType::Classes::None)
            return flow.familyOf(type.site).index() << 1;
        if (type.functions == LinkType::Functions::Closure && type.classes == LinkType::Classes::None)
            return type.function.index() << 1 | 1;
        return std::nullopt;
    }

    void note(uint32_t holder, PropertyKeyID key, FunctionID getter)
    {
        if (key)
            m_keys.set(key.index());
        else
            m_someKeyIsUnknown = true;
        uint32_t laid = getter ? getter.index() : several;
        auto underAnyKey = m_underAnyKey.add(holder, laid);
        if (!underAnyKey.isNewEntry && underAnyKey.iterator->value != laid)
            underAnyKey.iterator->value = several;
        auto added = m_getters.add(placeOf(holder, key), laid);
        if (!added.isNewEntry && added.iterator->value != laid)
            added.iterator->value = several;
    }

    bool isEmpty() const { return m_getters.isEmpty(); }
    bool mayAnswer(PropertyKeyID key) const { return m_someKeyIsUnknown || m_keys.get(key.index()); }

    FunctionID findUnderAnyKey(uint32_t holder) const
    {
        auto found = m_underAnyKey.find(holder);
        return found == m_underAnyKey.end() || found->value == several ? FunctionID() : FunctionID(found->value);
    }

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
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_underAnyKey;
    bool m_someKeyIsUnknown { false };
};

} // anonymous namespace

static NEVER_INLINE Vector<LinkedGetterAtRead> findGettersReadOffTablesWhileWaysOfFlowStand(const LinkFlow& flow)
{
    const Program& program = flow.program;
    const BuiltinTable& builtins = program.builtins();
    PropertyKeyID get = program.keyOf(BuiltinNames::get);
    auto takesObjectKeyAndDescriptor = [&](BuiltinID builtin) {
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtins.canonicalID(builtin))) {
            for (const BuiltinPropertyAction& action : overload.flow.propertyActions) {
                auto isArgument = [](const std::optional<BuiltinOperand>& operand, unsigned index) {
                    return operand && operand->kind() == BuiltinOperand::Kind::Argument && operand->argumentIndex() == index;
                };
                if (action.kind == BuiltinPropertyActionKind::TakesDescriptor && isArgument(action.object, 0) && isArgument(action.key.operand, 1) && isArgument(action.value.operand, 2))
                    return true;
            }
        }
        return false;
    };
    auto recordAt = [&](FlowNodeID node) {
        LinkType type = flow.typeAt(node);
        bool isOneRecord = type.objects == LinkType::Objects::Layout && !type.builtin && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None;
        return isOneRecord ? type.layout : LayoutID();
    };
    using NodesSeen = HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    auto forEachNodeNotSeenYetReachedWhile = [&](NodesSeen& seen, FlowNodeID start, auto&& stillHolds, auto&& visit) {
        if (!seen.add(start.index()).isNewEntry)
            return;
        Vector<FlowNodeID, 16> reached { start };
        for (size_t next = 0; next < reached.size(); ++next) {
            FlowNodeID node = reached[next];
            visit(node);
            for (FlowNodeID end : flow.endsOfEdgesOutOf(node)) {
                if (stillHolds(end) && seen.add(end.index()).isNewEntry)
                    reached.append(end);
            }
        }
    };

    struct Filling {
        LayoutID record;
        FlowNodeID target;
        FlowNodeID tables;
    };
    Vector<Filling> fillings;
    auto noteTable = [&](FlowNodeID target, FlowNodeID tables) {
        if (LayoutID record = recordAt(target))
            fillings.append({ record, target, tables });
    };
    constexpr uint32_t several = UINT32_MAX;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> getters;
    auto noteNamesOf = [&](LayoutID record, FlowNodeID tables) {
        flow.forEachLayoutOf(flow.typeAt(tables), [&](LayoutID table) {
            const Layout& layout = program.at(table);
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                PropertyKeyID key = layout.fields[field].key;
                if (!key || liesInThread(key))
                    continue;
                LinkType held = flow.typeAt(flow.field(FieldReference { table, field }));
                bool isOneFunction = held.functions == LinkType::Functions::Closure && !held.builtin && held.objects == LinkType::Objects::None && held.classes == LinkType::Classes::None;
                uint32_t getter = isOneFunction && flow.reached.get(held.function.index()) && program.at(held.function).hasBody() ? held.function.index() : several;
                auto added = getters.add(static_cast<uint64_t>(record.index()) << 32 | key.index(), getter);
                if (!added.isNewEntry && added.iterator->value != getter)
                    added.iterator->value = several;
            }
        });
    };

    struct Helper {
        FunctionID maker;
        FunctionID function;
        uint32_t target;
        uint32_t table;
    };
    Vector<Helper> helpers;
    for (size_t index : flow.reached) {
        FunctionID function(static_cast<uint32_t>(index));
        const ThinSummary& summary = flow.summaries.at(function);
        for (const SummaryChange& change : summary.changes) {
            if (change.kind != SummaryChange::Kind::DefinesByDescriptor || !change.key || !change.descriptorIsInSight || change.getterInSight == noSummaryNode || change.setterInSight != noSummaryNode || liesInThread(change.key))
                continue;
            FlowNodeID target = flow.nodeOf(function, change.subject);
            LayoutID record = recordAt(target);
            if (!record)
                continue;
            const SummaryNode& laid = summary.nodes[change.getterInSight];
            bool isOneFunction = laid.kind == SummaryNode::Kind::Closure && flow.reached.get(laid.first) && program.at(FunctionID(laid.first)).hasBody();
            auto added = getters.add(static_cast<uint64_t>(record.index()) << 32 | change.key.index(), isOneFunction ? laid.first : several);
            if (!added.isNewEntry && added.iterator->value != (isOneFunction ? laid.first : several))
                added.iterator->value = several;
            fillings.append({ record, target, { } });
        }
        if (summary.inspections.isEmpty() || !std::ranges::contains(summary.inspections, ThinSummary::Inspection::Kind::ListsKeys, &ThinSummary::Inspection::kind))
            continue;
        for (const SummaryCall& call : summary.calls) {
            if (call.isConstruction || call.passesList || call.numberOfArguments < 3 || (call.callee != SummaryCall::Callee::Value && call.callee != SummaryCall::Callee::Builtin))
                continue;
            uint32_t target = summary.arguments[call.firstArgument];
            uint32_t nodeOfDescriptor = summary.arguments[call.firstArgument + 2];
            if (target == noSummaryNode || nodeOfDescriptor == noSummaryNode || summary.nodes[nodeOfDescriptor].kind != SummaryNode::Kind::Allocation)
                continue;
            BuiltinID builtin = call.callee == SummaryCall::Callee::Builtin ? call.builtin : call.subject != noSummaryNode ? flow.typeAt(flow.nodeOf(function, call.subject)).builtin : BuiltinID();
            if (!builtin || !takesObjectKeyAndDescriptor(builtin))
                continue;
            const SummaryNode& descriptor = summary.nodes[nodeOfDescriptor];
            std::optional<uint32_t> fieldOfGetter = program.at(LayoutID(descriptor.first)).findField(get);
            if (!fieldOfGetter)
                continue;
            uint32_t table = noSummaryNode;
            unsigned numberOfSources = 0;
            for (const SummaryEdge& edge : summary.edges) {
                const SummaryNode& to = summary.nodes[edge.to];
                if (to.kind != SummaryNode::Kind::Field || to.first != descriptor.first || to.second != *fieldOfGetter)
                    continue;
                ++numberOfSources;
                const SummaryNode& from = summary.nodes[edge.from];
                if (from.kind == SummaryNode::Kind::ResultOfAccess && summary.accesses[from.first].kind == SummaryAccess::Kind::Read && !summary.accesses[from.first].key && summary.accesses[from.first].keyNode != noSummaryNode)
                    table = summary.accesses[from.first].subject;
            }
            if (numberOfSources != 1 || table == noSummaryNode)
                continue;
            bool listsKeysOfTable = std::ranges::any_of(summary.inspections, [&](const ThinSummary::Inspection& inspection) {
                return inspection.kind == ThinSummary::Inspection::Kind::ListsKeys && inspection.subject == table;
            });
            if (!listsKeysOfTable)
                continue;
            uint32_t firstParameter = program.at(function).firstParameterIndex();
            auto isParameter = [&](uint32_t node) {
                return summary.nodes[node].kind == SummaryNode::Kind::Parameter && summary.nodes[node].first >= firstParameter;
            };
            if (!isParameter(target) || !isParameter(table)) {
                noteTable(flow.nodeOf(function, target), flow.nodeOf(function, table));
                continue;
            }
            FunctionID maker = program.at(program.at(function).module).body;
            if (maker && flow.reached.get(maker.index()))
                helpers.append({ maker, function, summary.nodes[target].first - firstParameter, summary.nodes[table].first - firstParameter });
        }
    }
    std::ranges::sort(helpers, { }, [](const Helper& helper) { return std::pair(helper.maker, helper.function); });
    NodesSeen holdersOfHelpersSeen;
    for (size_t first = 0; first < helpers.size();) {
        FunctionID maker = helpers[first].maker;
        size_t end = first;
        while (end < helpers.size() && helpers[end].maker == maker)
            ++end;
        std::span<const Helper> madeThere = helpers.span().subspan(first, end - first);
        first = end;
        const ThinSummary& summaryOfMaker = flow.summaries.at(maker);
        for (uint32_t node = 0; node < summaryOfMaker.nodes.size(); ++node) {
            if (summaryOfMaker.nodes[node].kind != SummaryNode::Kind::Closure)
                continue;
            FunctionID function(summaryOfMaker.nodes[node].first);
            auto made = std::ranges::equal_range(madeThere, function, { }, &Helper::function);
            if (made.empty())
                continue;
            forEachNodeNotSeenYetReachedWhile(holdersOfHelpersSeen, flow.nodeOf(maker, node), [&](FlowNodeID end) {
                LinkType type = flow.typeAt(end);
                return type.functions == LinkType::Functions::Closure && type.function == function;
            }, [&](FlowNodeID holder) {
                for (const FlowObserver& observer : flow.observersOf(holder)) {
                    if (observer.kind != FlowObserver::Kind::CalleeOfCall)
                        continue;
                    const ThinSummary& summaryOfCaller = flow.summaries.at(observer.function);
                    const SummaryCall& entered = summaryOfCaller.calls[observer.record];
                    for (const Helper& helper : made) {
                        if (entered.isConstruction || entered.passesList || std::max(helper.target, helper.table) >= entered.numberOfArguments)
                            continue;
                        uint32_t givenTarget = summaryOfCaller.arguments[entered.firstArgument + helper.target];
                        uint32_t givenTable = summaryOfCaller.arguments[entered.firstArgument + helper.table];
                        if (givenTarget != noSummaryNode && givenTable != noSummaryNode)
                            noteTable(flow.nodeOf(observer.function, givenTarget), flow.nodeOf(observer.function, givenTable));
                    }
                }
            });
        }
    }

    struct Reader {
        LayoutID record;
        FlowObserver observer;
    };
    Vector<Reader> readers;
    BitVector recordsThatAreRead;
    NodesSeen holdersOfTargetsSeen;
    for (const Filling& filling : fillings) {
        forEachNodeNotSeenYetReachedWhile(holdersOfTargetsSeen, filling.target, [&](FlowNodeID end) {
            return recordAt(end) == filling.record;
        }, [&](FlowNodeID holder) {
            for (const FlowObserver& observer : flow.observersOf(holder)) {
                const ThinSummary& summary = flow.summaries.at(observer.function);
                if (observer.kind == FlowObserver::Kind::SubjectOfAccess) {
                    const SummaryAccess& access = summary.accesses[observer.record];
                    if (access.kind == SummaryAccess::Kind::Read && access.key && access.site && !access.isToContents() && access.receiver == noSummaryNode && !liesInThread(access.key)) {
                        readers.append({ filling.record, observer });
                        recordsThatAreRead.set(filling.record.index());
                    }
                    continue;
                }
                if (observer.kind != FlowObserver::Kind::ReceiverOfCall)
                    continue;
                const SummaryCall& call = summary.calls[observer.record];
                if (call.callee == SummaryCall::Callee::Name && call.key && call.site && !liesInThread(call.key)) {
                    readers.append({ filling.record, observer });
                    recordsThatAreRead.set(filling.record.index());
                }
            }
        });
    }
    Vector<LinkedGetterAtRead> reads;
    if (readers.isEmpty())
        return reads;
    for (const Filling& filling : fillings) {
        if (filling.tables && recordsThatAreRead.get(filling.record.index()))
            noteNamesOf(filling.record, filling.tables);
    }
    const BodyIndexTable& bodies = bodiesLookedAt(flow.looks);
    auto getterOf = [&](LayoutID record, PropertyKeyID key) {
        auto found = getters.find(static_cast<uint64_t>(record.index()) << 32 | key.index());
        return found == getters.end() || found->value == several ? FunctionID() : FunctionID(found->value);
    };
    for (const Reader& reader : readers) {
        FunctionID function = reader.observer.function;
        const ThinSummary& summary = flow.summaries.at(function);
        if (reader.observer.kind == FlowObserver::Kind::SubjectOfAccess) {
            const SummaryAccess& access = summary.accesses[reader.observer.record];
            if (FunctionID getter = getterOf(reader.record, access.key))
                reads.append(LinkedGetterAtRead { function, access.site, getter, true });
            continue;
        }
        const SummaryCall& call = summary.calls[reader.observer.record];
        FunctionID getter = getterOf(reader.record, call.key);
        const BodyIndex* body = getter ? bodies.find(function) : nullptr;
        if (!body)
            continue;
        BodyValue called = body->value(call.site);
        ValueID read = called.opcode() == Opcode::Call ? called.calleeOperand() : ValueID();
        if (read && !body->isParameter(read) && body->opcodeOf(read) == Opcode::PropGet && body->value(read).key() == call.key)
            reads.append(LinkedGetterAtRead { function, read, getter, true });
    }
    std::ranges::sort(reads);
    reads.shrink(std::ranges::unique(reads).begin() - reads.begin());
    return reads;
}

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
            const LinkType& getters = m_flow.typeAt(m_flow.nodeOf(function, change.getter));
            bool isOneFunction = getters.functions == LinkType::Functions::Closure && !getters.builtin && getters.objects == LinkType::Objects::None && getters.classes == LinkType::Classes::None;
            laid.note(*holder, change.key, isOneFunction && m_flow.reached.get(getters.function.index()) && m_program.at(getters.function).hasBody() ? getters.function : FunctionID());
        }
    }
    if (laid.isEmpty()) {
        layGettersReadOffTablesAmongRows();
        return;
    }
    m_tables.gettersExpectedAtReads = collectInParallel<LinkedGetterAtRead>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<LinkedGetterAtRead>& reads) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID function(index);
        size_t first = reads.size();
        for (const SummaryAccess& access : m_flow.summaries.at(function).accesses) {
            if (access.kind == SummaryAccess::Kind::Read && !access.key && access.site && access.keyNode != noSummaryNode && !access.keyIsSurelyIndex && access.receiver == noSummaryNode) {
                std::optional<uint32_t> holder = GettersLaidInLink::holderOf(m_flow, m_flow.typeAt(m_flow.nodeOf(function, access.subject)));
                if (FunctionID getter = holder ? laid.findUnderAnyKey(*holder) : FunctionID(); getter && !std::ranges::contains(reads.span().subspan(first), access.site, &LinkedGetterAtRead::site))
                    reads.append(LinkedGetterAtRead { function, access.site, getter });
                continue;
            }
            if (access.kind != SummaryAccess::Kind::Read || !access.key || !access.site || access.isToContents() || access.receiver != noSummaryNode || liesInThread(access.key) || !laid.mayAnswer(access.key))
                continue;
            std::optional<uint32_t> holder = GettersLaidInLink::holderOf(m_flow, m_flow.typeAt(m_flow.nodeOf(function, access.subject)));
            if (FunctionID getter = holder ? laid.find(*holder, access.key) : FunctionID())
                reads.append(LinkedGetterAtRead { function, access.site, getter });
        }
        if (reads.size() - first < 2)
            return;
        std::ranges::sort(reads.mutableSpan().subspan(first));
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
    layGettersReadOffTablesAmongRows();
}

void WriterOfLink::writeFunctionsInRecords()
{
    if (!writesWhatIsExpected())
        return;
    m_tables.functionsInRecords = collectInParallel<LinkedFunctionInRecord>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<LinkedFunctionInRecord>& found) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID storing(index);
        ModuleID file = m_program.at(storing).module;
        const ThinSummary& summary = m_flow.summaries.at(storing);
        auto isRecordOfFile = [&](LayoutID layout) {
            const Layout& record = m_program.at(layout);
            return record.kind == LayoutKind::Record && record.module == file && !record.has(LayoutFlags::IsOfCreatedObject);
        };
        auto forEachFunctionStored = [&](uint32_t stored, NOESCAPE const Invocable<void(FunctionID)> auto& functor) {
            const LinkType& type = m_flow.typeAt(m_flow.nodeOf(storing, stored));
            auto visit = [&](FunctionID id) {
                const Function& function = m_program.at(id);
                if (function.kind == FunctionKind::Plain && function.module == file && function.thisParameterIndex() && function.hasBody())
                    functor(id);
            };
            if (type.functions == LinkType::Functions::Closure)
                visit(type.function);
            else if (type.functions == LinkType::Functions::Set) {
                for (FunctionID member : m_flow.functionsOfSet(type))
                    visit(member);
            }
        };
        for (const SummaryEdge& edge : summary.edges) {
            const SummaryNode& field = summary.nodes[edge.to];
            if (field.kind != SummaryNode::Kind::Field || !isRecordOfFile(LayoutID(field.first)))
                continue;
            forEachFunctionStored(edge.from, [&](FunctionID function) {
                found.append({ function, LayoutID(field.first) });
            });
        }
        for (const SummaryAccess& access : summary.accesses) {
            if (!access.key || access.value == noSummaryNode || !access.laysValueInSightUnderName())
                continue;
            forEachFunctionStored(access.value, [&](FunctionID function) {
                const LinkType& subject = m_flow.typeAt(m_flow.nodeOf(storing, access.subject));
                if (subject.objects != LinkType::Objects::Layout && subject.objects != LinkType::Objects::Set)
                    return;
                m_flow.forEachLayoutOf(subject, [&](LayoutID layout) {
                    if (isRecordOfFile(layout))
                        found.append({ function, layout });
                });
            });
        }
    });
    sortAndRemoveDuplicates(loops(), m_tables.functionsInRecords);
}

void WriterOfLink::layGettersReadOffTablesAmongRows()
{
    if (m_gettersReadOffTables.isEmpty())
        return;
    Vector<LinkedGetterAtRead>& rows = m_tables.gettersExpectedAtReads;
    rows.appendVector(m_gettersReadOffTables);
    std::ranges::sort(rows);
    size_t kept = 0;
    for (size_t first = 0; first < rows.size();) {
        size_t end = first + 1;
        while (end < rows.size() && rows[end].function == rows[first].function && rows[end].site == rows[first].site)
            ++end;
        if (end == first + 1)
            rows[kept++] = rows[first];
        first = end;
    }
    rows.shrink(kept);
}

void WriterOfLink::writeReceiversThatStayAsMade()
{
    auto staysAsMade = [&](FunctionID function, uint32_t node) {
        const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(function, node));
        return holdsOnlyInstancesThatStayAsMade(receiver) && m_flow.classOf(receiver.site) != BuiltinClasses::DataView;
    };
    auto isView = [&](FunctionID function, uint32_t node) {
        return m_program.builtins().typedArrayTypeOf(m_flow.classOf(m_flow.typeAt(m_flow.nodeOf(function, node)).site)) != NotTypedArray;
    };
    PropertyKeyID keyOfIterators = m_program.keyOf(m_program.builtins().nameOf(WellKnownSymbol::Iterator));
    PropertyKeyID keyOfApply = m_program.keyOf(m_program.builtins().nameOf(byteCast<uint8_t>("apply"_span)));
    ChunksOfIndices division = divideIntoChunks(m_numberOfFunctions);
    Vector<Vector<LinkedReceiverAsMade>> receiversOfChunks(division.size());
    Vector<Vector<LinkedReceiverAsMade>> iterablesOfChunks(division.size());
    Vector<Vector<LinkedReceiverAsMade>> unchangedOfChunks(division.size());
    Vector<Vector<LinkedReceiverAsMade>> plainOfChunks(division.size());
    auto isPlainList = [&](FunctionID function, uint32_t node) {
        const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(function, node));
        return holdsOnlyPlainInstances(receiver) && m_flow.classOf(receiver.site) == BuiltinClasses::Array;
    };
    Vector<Vector<LinkedReceiverAsMade>> listsWithoutHolesOfChunks(division.size());
    Vector<Vector<LinkedReceiverAsMade>> iterablesWithoutHolesOfChunks(division.size());
    auto isChangedByNobody = [&](FunctionID function, uint32_t node) {
        const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(function, node));
        if (m_flow.classOf(receiver.site) != BuiltinClasses::Array)
            return false;
        FlowNodeID held = m_flow.findContentsOf(receiver, SlotOfContents::isChanged());
        return !held || (held != m_flow.unseen() && m_flow.typeAt(held).isNever());
    };
    auto isListWithoutHoles = [&](FunctionID function, uint32_t node) {
        const LinkType& receiver = m_flow.typeAt(m_flow.nodeOf(function, node));
        return m_flow.classOf(receiver.site) == BuiltinClasses::Array && m_flow.hasNoHoles(receiver);
    };
    forEachIndexInParallel(loops(), division.size(), [&](unsigned chunk) {
        Vector<LinkedReceiverAsMade>& receivers = receiversOfChunks[chunk];
        Vector<LinkedReceiverAsMade>& listsRead = listsWithoutHolesOfChunks[chunk];
        Vector<LinkedReceiverAsMade>& listsIterated = iterablesWithoutHolesOfChunks[chunk];
        Vector<ValueID, 16> sitesOfListsWithHoles;
        Vector<ValueID, 4> sitesOfIterablesWithHoles;
        Vector<ValueID, 16> sitesOfOtherReceivers;
        Vector<ValueID, 16> sitesOfChangedReceivers;
        Vector<ValueID, 16> sitesOfOtherLists;
        Vector<LinkedReceiverAsMade>& iterables = iterablesOfChunks[chunk];
        Vector<ValueID, 4> sitesOfOtherIterables;
        for (uint32_t index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            if (!m_flow.reached.get(index))
                continue;
            FunctionID function(index);
            const ThinSummary& summary = m_flow.summaries.at(function);
            size_t first = receivers.size();
            sitesOfOtherReceivers.shrink(0);
            sitesOfChangedReceivers.shrink(0);
            size_t firstIterable = iterables.size();
            sitesOfOtherIterables.shrink(0);
            size_t firstUnchanged = unchangedOfChunks[chunk].size();
            size_t firstListRead = listsRead.size();
            size_t firstListIterated = listsIterated.size();
            sitesOfListsWithHoles.shrink(0);
            sitesOfIterablesWithHoles.shrink(0);
            auto note = [&](ValueID site, uint32_t node, bool isCall = false) {
                if (staysAsMade(function, node) && (isCall || !isView(function, node))) {
                    receivers.append(LinkedReceiverAsMade { function, site });
                    if (!isCall && isListWithoutHoles(function, node))
                        listsRead.append(LinkedReceiverAsMade { function, site });
                    else if (!isCall)
                        sitesOfListsWithHoles.append(site);
                    if (!isCall && isChangedByNobody(function, node))
                        unchangedOfChunks[chunk].append(LinkedReceiverAsMade { function, site });
                    else
                        sitesOfChangedReceivers.append(site);
                } else
                    sitesOfOtherReceivers.append(site);
            };
            size_t firstPlain = plainOfChunks[chunk].size();
            sitesOfOtherLists.shrink(0);
            for (const SummaryAccess& access : summary.accesses) {
                if (access.kind == SummaryAccess::Kind::Read && access.site && !access.isToContents() && access.receiver == noSummaryNode) {
                    note(access.site, access.subject);
                    if (!access.key && isPlainList(function, access.subject))
                        plainOfChunks[chunk].append(LinkedReceiverAsMade { function, access.site });
                    else
                        sitesOfOtherLists.append(access.site);
                }
            }
            if (Vector<LinkedReceiverAsMade>& plain = plainOfChunks[chunk]; plain.size() != firstPlain) {
                std::ranges::sort(sitesOfOtherLists);
                plain.removeAllMatching([&](const LinkedReceiverAsMade& read) {
                    return std::ranges::binary_search(sitesOfOtherLists, read.site);
                }, firstPlain);
                std::ranges::sort(plain.mutableSpan().subspan(firstPlain));
                auto repeats = std::ranges::unique(plain.mutableSpan().subspan(firstPlain));
                plain.shrink(plain.size() - repeats.size());
            }
            for (const SummaryCall& call : summary.calls) {
                if (call.callee != SummaryCall::Callee::Name || !call.site || call.receiver == noSummaryNode)
                    continue;
                note(call.site, call.receiver, true);
                if (uint32_t list = call.key == keyOfApply && !call.isMadeByProtocol && !call.passesList && call.numberOfArguments == 2 ? summary.arguments[call.firstArgument + 1] : noSummaryNode; list != noSummaryNode) {
                    if (staysAsMade(function, list))
                        iterables.append(LinkedReceiverAsMade { function, call.site });
                    else
                        sitesOfOtherIterables.append(call.site);
                    continue;
                }
                if (!call.isMadeByProtocol || call.key != keyOfIterators)
                    continue;
                if (staysAsMade(function, call.receiver)) {
                    iterables.append(LinkedReceiverAsMade { function, call.site });
                    if (isListWithoutHoles(function, call.receiver))
                        listsIterated.append(LinkedReceiverAsMade { function, call.site });
                    else
                        sitesOfIterablesWithHoles.append(call.site);
                } else
                    sitesOfOtherIterables.append(call.site);
            }
            if (listsIterated.size() != firstListIterated) {
                listsIterated.removeAllMatching([&](const LinkedReceiverAsMade& list) {
                    return sitesOfIterablesWithHoles.contains(list.site) || sitesOfOtherIterables.contains(list.site);
                }, firstListIterated);
                auto listed = listsIterated.mutableSpan().subspan(firstListIterated);
                std::ranges::sort(listed);
                listsIterated.shrink(listsIterated.size() - std::ranges::unique(listed).size());
            }
            if (iterables.size() != firstIterable) {
                iterables.removeAllMatching([&](const LinkedReceiverAsMade& iterable) {
                    return sitesOfOtherIterables.contains(iterable.site);
                }, firstIterable);
                std::ranges::sort(iterables.mutableSpan().subspan(firstIterable));
                auto repeats = std::ranges::unique(iterables.mutableSpan().subspan(firstIterable));
                iterables.shrink(iterables.size() - repeats.size());
            }
            if (receivers.size() == first)
                continue;
            std::ranges::sort(sitesOfOtherReceivers);
            if (listsRead.size() != firstListRead) {
                std::ranges::sort(sitesOfListsWithHoles);
                listsRead.removeAllMatching([&](const LinkedReceiverAsMade& list) {
                    return std::ranges::binary_search(sitesOfListsWithHoles, list.site) || std::ranges::binary_search(sitesOfOtherReceivers, list.site);
                }, firstListRead);
                auto listed = listsRead.mutableSpan().subspan(firstListRead);
                std::ranges::sort(listed);
                listsRead.shrink(listsRead.size() - std::ranges::unique(listed).size());
            }
            if (unchangedOfChunks[chunk].size() != firstUnchanged) {
                std::ranges::sort(sitesOfChangedReceivers);
                unchangedOfChunks[chunk].removeAllMatching([&](const LinkedReceiverAsMade& receiver) {
                    return std::ranges::binary_search(sitesOfOtherReceivers, receiver.site) || std::ranges::binary_search(sitesOfChangedReceivers, receiver.site);
                }, firstUnchanged);
                std::ranges::sort(unchangedOfChunks[chunk].mutableSpan().subspan(firstUnchanged));
                unchangedOfChunks[chunk].shrink(unchangedOfChunks[chunk].size() - std::ranges::unique(unchangedOfChunks[chunk].mutableSpan().subspan(firstUnchanged)).size());
            }
            receivers.removeAllMatching([&](const LinkedReceiverAsMade& receiver) {
                return std::ranges::binary_search(sitesOfOtherReceivers, receiver.site);
            }, first);
            std::ranges::sort(receivers.mutableSpan().subspan(first));
            auto repeats = std::ranges::unique(receivers.mutableSpan().subspan(first));
            receivers.shrink(receivers.size() - repeats.size());
        }
    });
    m_tables.receiversThatStayAsMade = joinChunks<LinkedReceiverAsMade>(loops(), WTF::move(receiversOfChunks));
    m_tables.iterablesThatStayAsMade = joinChunks<LinkedReceiverAsMade>(loops(), WTF::move(iterablesOfChunks));
    m_tables.receiversThatNobodyChanges = joinChunks<LinkedReceiverAsMade>(loops(), WTF::move(unchangedOfChunks));
    m_tables.plainListsRead = joinChunks<LinkedReceiverAsMade>(loops(), WTF::move(plainOfChunks));
    m_tables.listsWithoutHoles = joinChunks<LinkedReceiverAsMade>(loops(), WTF::move(listsWithoutHolesOfChunks));
    m_tables.iterablesWithoutHoles = joinChunks<LinkedReceiverAsMade>(loops(), WTF::move(iterablesWithoutHolesOfChunks));
}

void WriterOfLink::writeReadsOffViewsOfWrittenLengths()
{
    if (writesWhatIsExpected() || !m_program.openedNames.isEmpty())
        return;
    m_tables.readsOffViewsOfWrittenLengths = collectInParallel<LinkedReadOffViews>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<LinkedReadOffViews>& readsOfChunk) {
        if (!m_flow.reached.get(index) || !m_flow.readsOffViewMadeAtSite(FunctionID(index)))
            return;
        FunctionID function(index);
        const ThinSummary& summary = m_flow.summaries.at(function);
        size_t first = readsOfChunk.size();
        Vector<ValueID, 16> sitesOfOtherReads;
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Read || !access.site || access.isToContents() || access.key)
                continue;
            if (uint32_t least = access.receiver == noSummaryNode ? m_flow.leastLengthOfViewsIn(m_flow.typeAt(m_flow.nodeOf(function, access.subject))) : 0)
                readsOfChunk.append(LinkedReadOffViews { function, access.site, least });
            else
                sitesOfOtherReads.append(access.site);
        }
        if (readsOfChunk.size() == first)
            return;
        readsOfChunk.removeAllMatching([&](const LinkedReadOffViews& read) {
            return sitesOfOtherReads.contains(read.site);
        }, first);
        std::ranges::sort(readsOfChunk.mutableSpan().subspan(first));
        auto repeats = std::ranges::unique(readsOfChunk.mutableSpan().subspan(first), [](const LinkedReadOffViews& left, const LinkedReadOffViews& right) {
            return left.site == right.site;
        });
        readsOfChunk.shrink(readsOfChunk.size() - repeats.size());
    });
}

void WriterOfLink::writeResultsOfReadsByKey()
{
    bool textsMayBeOpened = !writesWhatIsExpected() && !m_program.openedNames.isEmpty();
    auto spellsNoText = [&](FunctionID function, const SummaryAccess& access) {
        if (access.keyIsSurelyIndex)
            return true;
        if (access.keyNode == noSummaryNode)
            return false;
        const LinkType& key = m_flow.typeAt(m_flow.nodeOf(function, access.keyNode));
        return key.objects == LinkType::Objects::None && key.functions == LinkType::Functions::None && key.classes == LinkType::Classes::None && !key.builtin && !key.hasUnknownObjects && !key.holdsLostBuiltins
            && !(key.primitives & (LinkType::closed(Primitive::AnyString | Primitive::AnySymbol) | LinkType::oneSymbol));
    };
    struct Read {
        FunctionID function;
        ValueID site;
        FlowNodeID value;
    };
    PropertyKeyID valueOfStep = m_program.keyOf(BuiltinNames::value);
    PropertyKeyID nextOfStep = m_program.keyOf(BuiltinNames::next);
    BitVector layoutsFilledUnderUnknownNames(m_program.layouts.size());
    BitVector lostFamiliesFilledUnderUnknownNames(m_program.layouts.size());
    bool isCheckedAtRead = AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::CheckProofsAtRunTime) && AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::CheckDictionaryReadProofs);
    for (uint32_t layout = 0; layout < m_program.layouts.size() && !writesWhatIsExpected() && !isCheckedAtRead; ++layout) {
        if (!m_flow.hasUnknownKeyStores(LayoutID(layout)))
            continue;
        layoutsFilledUnderUnknownNames.quickSet(layout);
        lostFamiliesFilledUnderUnknownNames.quickSet(m_flow.unitedLayouts.representativeWithoutShortening(layout));
    }
    BitVector familiesFilledUnderUnknownNames;
    if (!layoutsFilledUnderUnknownNames.isEmpty()) {
        familiesFilledUnderUnknownNames = m_flow.families.familiesWithSomeMemberThat([&](LayoutID member) {
            return layoutsFilledUnderUnknownNames.quickGet(member.index());
        });
    }
    auto mayBeFilledUnderUnknownNames = [&](const LinkType& subject) {
        switch (subject.objects) {
        case LinkType::Objects::None:
        case LinkType::Objects::Prototype:
        case LinkType::Objects::Prototypes:
            return false;
        case LinkType::Objects::Layout:
            return layoutsFilledUnderUnknownNames.quickGet(subject.layout.index());
        case LinkType::Objects::Set:
            return std::ranges::any_of(m_flow.layoutsOfSet(subject), [&](LayoutID member) {
                return layoutsFilledUnderUnknownNames.quickGet(member.index());
            });
        case LinkType::Objects::Family:
            return familiesFilledUnderUnknownNames.get(subject.family.index());
        case LinkType::Objects::Lost:
            return subject.layout && lostFamiliesFilledUnderUnknownNames.quickGet(m_flow.unitedLayouts.representativeWithoutShortening(subject.layout.index()));
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    Vector<Read> found = collectInParallel<Read>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<Read>& readsOfChunk) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID function(index);
        const ThinSummary& summary = m_flow.summaries.at(function);
        auto isStepValueRead = [&](const SummaryAccess& access) {
            if (access.key != valueOfStep)
                return false;
            const SummaryNode& subject = summary.nodes[access.subject];
            if (subject.kind != SummaryNode::Kind::ResultOfCall)
                return false;
            const SummaryCall& call = summary.calls[subject.first];
            return call.isMadeByProtocol && call.site == access.site && call.callee == SummaryCall::Callee::Name && call.key == nextOfStep;
        };
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Read || !access.site || access.isToContents() || access.value == noSummaryNode)
                continue;
            if (access.key && !isStepValueRead(access))
                continue;
            if (textsMayBeOpened && (access.key || !spellsNoText(function, access)))
                continue;
            if (!access.key && mayBeFilledUnderUnknownNames(m_flow.typeAt(m_flow.nodeOf(function, access.subject))))
                continue;
            readsOfChunk.append(Read { function, access.site, m_flow.nodeOf(function, access.value) });
        }
    });
    Vector<TypeID> proven;
    proven.fill(TypeID(), found.size());
    writeTypedPositionsOnAllThreads(m_flow, m_flow.program.types, static_cast<unsigned>(found.size()), [](TypesOfThreadInLink types) {
        s_typesOfThread = types;
    }, [&](unsigned place) {
        TypeID seen = typeIDOf(m_flow.typeAt(found[place].value).withoutBoundPast32Bits());
        proven[place] = seen ? typesAtHand().withPrimitives(seen, Primitive::Undefined) : TypeID();
        return isOfProgramInLink(proven[place]);
    });

    Vector<LinkedResultOfRead>& reads = m_tables.resultsOfReadsByKey;
    size_t endOfFunction = 0;
    for (size_t firstOfFunction = 0; firstOfFunction < found.size(); firstOfFunction = endOfFunction) {
        FunctionID function = found[firstOfFunction].function;
        size_t first = reads.size();
        for (; endOfFunction < found.size() && found[endOfFunction].function == function; ++endOfFunction)
            reads.append(LinkedResultOfRead { function, found[endOfFunction].site, proven[endOfFunction] });
        std::ranges::sort(reads.mutableSpan().subspan(first), { }, &LinkedResultOfRead::site);
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

void WriterOfLink::writeNativeCallResults()
{
    if (writesWhatIsExpected())
        return;
    const BuiltinTable& builtins = m_program.builtins();
    m_tables.nativeCallResults = collectInParallel<LinkedNativeCallResult>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<LinkedNativeCallResult>& calls) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID caller(index);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        uint32_t firstCall = m_flow.firstCallOfFunction[index];
        BitVector nativeCalls;
        Vector<ValueID> sites;
        for (uint32_t number = 0; number < summary.calls.size(); ++number) {
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[firstCall + number];
            if (callees.builtins().isEmpty())
                continue;
            const SummaryCall& call = summary.calls[number];
            bool hasOtherCallees = !callees.listed().isEmpty() || !callees.shared().isEmpty() || std::ranges::any_of(callees.builtins().span(), [&](BuiltinID callee) {
                return builtins.at(callee).scope != BuiltinScope::Module;
            });
            if (!hasOtherCallees && call.site && !call.isMadeByProtocol && !call.isConstruction && !call.passesList)
                nativeCalls.set(number);
        }
        if (nativeCalls.isEmpty())
            return;
        for (const SummaryCall& call : summary.calls)
            sites.append(call.site);
        std::ranges::sort(sites);
        size_t first = calls.size();
        for (uint32_t node = 1; node < summary.nodes.size(); ++node) {
            const SummaryNode& result = summary.nodes[node];
            if (result.kind != SummaryNode::Kind::ResultOfCall || result.second || !nativeCalls.get(result.first))
                continue;
            ValueID site = summary.calls[result.first].site;
            const LinkType& given = m_flow.typeAt(m_flow.nodeOf(caller, node));
            bool holdsObjects = given.objects != LinkType::Objects::None || given.functions != LinkType::Functions::None || given.classes != LinkType::Classes::None || given.builtin || given.hasUnknownObjects || given.holdsLostBuiltins;
            if (given.isNever() || holdsObjects || std::ranges::equal_range(sites, site).size() != 1)
                continue;
            if (TypeID proven = typeIDOf(given); proven && holdsOnlyPrimitives(proven))
                calls.append(LinkedNativeCallResult { caller, site, proven });
        }
        std::ranges::sort(calls.mutableSpan().subspan(first), { }, &LinkedNativeCallResult::site);
    });
    for (const LinkedNativeCallResult& call : m_tables.nativeCallResults)
        m_tables.nativeFunctionCallers.set(call.function.index());
}

void WriterOfLink::writeContentsAtOperations()
{
    struct Operation {
        FunctionID function;
        ValueID site;
        FlowNodeID receiver;
    };
    auto isMapOrWeakMap = [](const LinkType& receiver) {
        return receiver.builtinClass == BuiltinClasses::Map || receiver.builtinClass == BuiltinClasses::WeakMap;
    };
    PropertyKeyID onlyNameAskedOfWhatIsExpected = writesWhatIsExpected() ? m_program.keyOf(BuiltinNames::get) : PropertyKeyID();
    BuiltinID arrayFiller = m_program.builtins().makerOfFilledArrays();
    PropertyKeyID nameOfReadByIndex = m_program.keyOf(BuiltinNames::at);
    Vector<Operation> found = collectInParallel<Operation>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<Operation>& operationsOfChunk) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID function(index);
        for (const SummaryCall& call : m_flow.summaries.at(function).calls) {
            if (!call.site || call.receiver == noSummaryNode || !call.receiver || (onlyNameAskedOfWhatIsExpected && call.key != onlyNameAskedOfWhatIsExpected))
                continue;
            FlowNodeID node = m_flow.nodeOf(function, call.receiver);
            if (!m_flow.holdsInstancesOfOneSiteAt(node))
                continue;
            const LinkType& receiver = m_flow.typeAt(node);
            bool mayMakeIteratorOverList = receiver.builtinClass == BuiltinClasses::Array && !call.numberOfArguments;
            bool mayReadElement = receiver.builtinClass == BuiltinClasses::Array && call.numberOfArguments == 1 && call.key == nameOfReadByIndex;
            bool mayRunOverList = false;
            if (receiver.builtinClass == BuiltinClasses::Array && call.numberOfArguments && !writesWhatIsExpected()) {
                uint32_t first = m_flow.summaries.at(function).arguments[call.firstArgument];
                if (first != noSummaryNode && first) {
                    const LinkType& handed = m_flow.typeAt(m_flow.nodeOf(function, first));
                    mayRunOverList = handed.functions == LinkType::Functions::Closure && handed.objects == LinkType::Objects::None && handed.classes == LinkType::Classes::None && !handed.primitives;
                }
            }
            if (receiver.classes != LinkType::Classes::Site || receiver.objects != LinkType::Objects::None || receiver.functions != LinkType::Functions::None || (!isMapOrWeakMap(receiver) && !mayMakeIteratorOverList && !mayReadElement && !mayRunOverList && receiver.builtinClass != BuiltinClasses::Set))
                continue;
            if ((receiver.primitives & ~Primitive::UndefinedOrNull) || (!writesWhatIsExpected() && (receiver.hasUnknownObjects || receiver.holdsLostBuiltins || m_flow.hasEscaped(receiver.site))))
                continue;
            operationsOfChunk.append(Operation { function, call.site, node });
        }
        if (writesWhatIsExpected())
            return;
        const ThinSummary& summary = m_flow.summaries.at(function);
        BuiltinID makerOfLists = m_program.builtins().canonicalID(Builtins::Array);
        for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
            const SummaryNode& made = summary.nodes[index];
            if (made.kind != SummaryNode::Kind::ResultOfCall || made.second)
                continue;
            const SummaryCall& call = summary.calls[made.first];
            if (!call.site || call.callee != SummaryCall::Callee::Builtin || (m_program.builtins().canonicalID(call.builtin) != makerOfLists && (!arrayFiller || call.builtin != arrayFiller)))
                continue;
            FlowNodeID node = m_flow.nodeOf(function, index);
            if (!m_flow.holdsInstancesOfOneSiteAt(node))
                continue;
            const LinkType& list = m_flow.typeAt(node);
            if (list.classes != LinkType::Classes::Site || list.objects != LinkType::Objects::None || list.functions != LinkType::Functions::None || list.builtinClass != BuiltinClasses::Array || list.primitives || m_flow.hasEscaped(list.site) || list.hasUnknownObjects || list.holdsLostBuiltins || list.builtin)
                continue;
            operationsOfChunk.append(Operation { function, call.site, node });
        }
    });

    Vector<LinkedContentsAtOperation>& listed = m_tables.contentsAtOperations;
    auto heldIn = [&](const LinkType& receiver, ContentsSlot slot) -> TypeID {
        FlowNodeID node = m_flow.findContentsOf(receiver, SlotOfContents(slot));
        return node ? unlessAnything(typeIDOf(m_flow.typeAt(node).withoutBoundPast32Bits())) : TypeID();
    };
    size_t endOfFunction = 0;
    for (size_t firstOfFunction = 0; firstOfFunction < found.size(); firstOfFunction = endOfFunction) {
        FunctionID function = found[firstOfFunction].function;
        size_t first = listed.size();
        for (; endOfFunction < found.size() && found[endOfFunction].function == function; ++endOfFunction) {
            const LinkType& receiver = m_flow.typeAt(found[endOfFunction].receiver);
            bool isMap = isMapOrWeakMap(receiver);
            LinkedContentsAtOperation contents { function, found[endOfFunction].site, isMap ? heldIn(receiver, ContentsSlot::Key) : TypeID(), heldIn(receiver, isMap ? ContentsSlot::Value : ContentsSlot::Element) };
            if (contents.key || contents.value)
                listed.append(contents);
        }
        std::ranges::sort(listed.mutableSpan().subspan(first), { }, &LinkedContentsAtOperation::site);
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
    return holdsOnlyPrimitives(type) ? type : widenedToFamily(typesAtHand(), *m_flow.familiesBehindFlow, type, SetsThatNoFamilyHolds::BecomeAnyObject);
}

TypeID WriterOfLink::typeIDOf(const LinkType& type)
{
    if (type.isNever() || withoutLostBuiltinsInLink(type).isAnything())
        return { };
    if (!writesWhatIsExpected())
        return unlessAnything(typesTurned().turn(m_flow, typesAtHand(), type));
    LinkType seen = type;
    seen.hasUnknownObjects = false;
    seen.holdsLostBuiltins = false;
    seen.builtin = { };
    if (seen.functions != LinkType::Functions::None && seen.functions != LinkType::Functions::Closure && seen.functions != LinkType::Functions::Set) {
        seen.functions = LinkType::Functions::Lost;
        seen.function = { };
    }
    if (seen.isNever())
        return { };
    return unlessAnything(typesTurned().turn(m_flow, typesAtHand(), seen));
}

TypeID WriterOfLink::typeIDOfCapture(FunctionID id, uint32_t capture)
{
    const LinkType passed = m_flow.typeAt(m_flow.parameter(id, capture)).withoutBoundPast32Bits();
    const LinkType captured = m_flow.typeAt(m_flow.capture(id, capture)).withoutBoundPast32Bits();
    if (passed == captured || captured.isNever())
        return typeIDOf(passed);
    if (passed.isNever())
        return typeIDOf(captured);
    TypeID first = typeIDOf(passed);
    TypeID second = typeIDOf(captured);
    return first && second ? unlessAnything(joinOf(typesAtHand(), *m_flow.familiesBehindFlow, first, second)) : TypeID();
}

thread_local TypesOfThreadInLink WriterOfLink::s_typesOfThread;

void WriterOfLink::findLiteralsThatMembersAreWrittenIn()
{
    m_literalsThatMembersAreWrittenIn.fill(LayoutID(), m_numberOfFunctions);
    BitVector descriptors(m_program.layouts.size());
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_flow.reached.get(index))
            continue;
        const auto& summary = m_flow.summaries.at(FunctionID(index));
        auto note = [&](uint32_t subject, uint32_t laid) {
            if (subject == noSummaryNode || laid == noSummaryNode || summary.nodes[laid].kind != SummaryNode::Kind::Closure)
                return;
            if (summary.nodes[subject].kind != SummaryNode::Kind::Allocation && summary.nodes[subject].kind != SummaryNode::Kind::Field)
                return;
            LayoutID literal(summary.nodes[subject].first);
            FunctionID member(summary.nodes[laid].first);
            FunctionKind kind = m_program.at(member).kind;
            if (m_program.at(literal).kind == LayoutKind::Record && m_program.at(literal).module && !m_program.at(literal).has(LayoutFlags::IsOfCreatedObject) && (kind == FunctionKind::Method || kind == FunctionKind::Getter || kind == FunctionKind::Setter))
                m_literalsThatMembersAreWrittenIn[member.index()] = literal;
        };
        for (const SummaryChange& change : summary.changes) {
            if (change.kind != SummaryChange::Kind::DefinesByDescriptor)
                continue;
            if (change.lays != SummaryChange::Lays::WhatDescriptorSays)
                note(change.subject, change.other);
            else if (FlowNodeID descriptor = change.other == noSummaryNode ? FlowNodeID() : m_flow.nodeOf(FunctionID(index), change.other)) {
                m_flow.forEachLayoutOf(m_flow.typeAt(descriptor), [&](LayoutID layout) {
                    descriptors.quickSet(layout.index());
                });
            }
        }
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind == SummaryAccess::Kind::Define)
                note(access.subject, access.value);
        }
        for (const SummaryEdge& edge : summary.edges)
            note(edge.to, edge.from);
    }
    for (LayoutID& literal : m_literalsThatMembersAreWrittenIn) {
        if (literal && descriptors.quickGet(literal.index()))
            literal = { };
        if (!literal)
            continue;
        auto added = m_literalsWithWhatIsCreatedBelow.add(literal.index(), Vector<LayoutID>());
        if (!added.isNewEntry)
            continue;
        Vector<LayoutID>& layouts = added.iterator->value;
        if (FlowNodeID below = m_flow.findWhatStandsBelow(literal)) {
            m_flow.forEachLayoutOf(m_flow.typeAt(below), [&](LayoutID created) {
                layouts.append(created);
            });
        }
        if (layouts.isEmpty())
            layouts.append(literal);
        std::ranges::sort(layouts);
        layouts.shrink(std::ranges::unique(layouts).begin() - layouts.begin());
    }
}

TypeID WriterOfLink::typeOfLiteralAndWhatIsCreatedBelow(FunctionID member)
{
    LayoutID literal = m_literalsThatMembersAreWrittenIn[member.index()];
    if (!literal)
        return { };
    TypeID& made = typesTurned().typeOfLiteralWithWhatIsCreatedBelow(literal);
    if (!made)
        made = typesAtHand().forLayouts(m_literalsWithWhatIsCreatedBelow.find(literal.index())->value.span());
    return made;
}

void WriterOfLink::writeFunctions()
{
    findLiteralsThatMembersAreWrittenIn();
    writeTypedPositionsOnAllThreads(m_flow, m_flow.program.types, m_numberOfFunctions, [](TypesOfThreadInLink types) {
        s_typesOfThread = types;
    }, [&](unsigned index) {
        writeFunction(FunctionID(index));
        const LinkedFunction& linked = m_tables.functions[index];
        return isOfProgramInLink(linked.result) && std::ranges::all_of(m_tables.parametersOf(FunctionID(index)), [](const LinkedParameter& parameter) {
            return isOfProgramInLink(parameter.proven) && isOfProgramInLink(parameter.written) && isOfProgramInLink(parameter.seenToArrive);
        });
    });
}

static bool returnsValueOfMemory(const ThinSummary& summary)
{
    static constexpr size_t mostNodesFollowed = 32;
    Vector<uint32_t, 8> nodes;
    for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
        if (summary.nodes[index].kind == SummaryNode::Kind::Result && !summary.nodes[index].first)
            nodes.append(index);
    }
    for (size_t place = 0; place < nodes.size() && place < mostNodesFollowed; ++place) {
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to != nodes[place])
                continue;
            switch (summary.nodes[edge.from].kind) {
            case SummaryNode::Kind::Capture:
            case SummaryNode::Kind::Variable:
            case SummaryNode::Kind::Field:
            case SummaryNode::Kind::Contents:
            case SummaryNode::Kind::ResultOfAccess:
                return true;
            case SummaryNode::Kind::Join:
                if (!nodes.contains(edge.from))
                    nodes.append(edge.from);
                break;
            default:
                break;
            }
        }
    }
    return false;
}

ALWAYS_INLINE void WriterOfLink::writeFunction(FunctionID id)
{
    uint32_t index = id.index();
    LinkedFunction& linked = m_tables.functions[index];
    linked.isReached = m_flow.reached.get(index);
    if (!linked.isReached)
        return;

    const Function& function = m_program.at(id);
    linked.isIterationCallback = m_flow.iterationCallbacks.get(index);
    linked.isRepeatedCallback = m_flow.repeatedCallbacks.get(index);
    linked.everyCallerIsKnown = everyCallerIsKnownOf(index);
    linked.holdsInt32Promise = m_flow.summaries.at(id).has(SummaryTrait::HoldsInt32Promise);
    linked.neverReturns = function.hasBody() && m_flow.summaries.at(id).has(SummaryTrait::NeverReturns);
    bool sharesNoCall = linked.everyCallerIsKnown && !m_functionsSharingCalls.get(index) && !m_functionsThatTextsOfLibraryName.get(index);
    linked.everyCallHasOneCallee = sharesNoCall && isEnteredByCallsAloneForLink(function.kind);
    bool isMethodThatOnlySharesCalls = linked.everyCallerIsKnown && !m_functionsThatTextsOfLibraryName.get(index) && function.kind == FunctionKind::Method;
    linked.everyTakerOfResultKnowsCallee = linked.everyCallHasOneCallee || (sharesNoCall && function.kind == FunctionKind::Getter) || isMethodThatOnlySharesCalls;
    if (function.results.size() == 1 && !function.isCoroutine()) {
        LinkType result = m_flow.typeAt(m_flow.result(id, 0));
        LinkType unbounded = result.withoutBoundPast32Bits();
        bool dropsBound = unbounded.counts != result.counts && function.hasBody() && returnsValueOfMemory(m_flow.summaries.at(id));
        linked.result = typeIDOf(dropsBound ? unbounded : result.asPassed());
        if (std::optional<ProvenInEveryVersion> established = function.results[0].established(); established && !linked.result)
            linked.result = unlessAnything(*established);
    }
    writeParameters(id, linked);
}

BitVector bodiesThatLoaderEntersAtMostOnce(const Program& program)
{
    BitVector bodies(program.functions.size());
    BitVector bodiesOfSeveralModules(program.functions.size());
    BitVector bodiesWrittenInOtherFiles(program.functions.size());
    for (const Module& module : program.modules) {
        if (!module.body)
            continue;
        if (bodies.quickSet(module.body.index()))
            bodiesOfSeveralModules.quickSet(module.body.index());
        const Function& body = program.at(module.body);
        bool isEnteredOnce = body.isModuleBody() && body.module && loaderEntersBodyAtMostOnce(module);
        if (!isEnteredOnce)
            bodiesOfSeveralModules.quickSet(module.body.index());
        else if (isBodyWrittenInAnotherFile(program, module.body))
            bodiesWrittenInOtherFiles.quickSet(module.body.index());
    }
    bodies.exclude(bodiesOfSeveralModules);
    for (size_t body : bodiesWrittenInOtherFiles) {
        FunctionID bodyOfFile = program.at(program.at(FunctionID(static_cast<uint32_t>(body))).module).body;
        if (!bodyOfFile || !bodies.get(bodyOfFile.index()))
            bodies.quickClear(body);
    }
    return bodies;
}

class JumpForest {
    static constexpr uint32_t none = UINT32_MAX;
public:
    explicit JumpForest(const BodyIndex& index)
        : m_index(index)
        , m_roots(FillWith { }, index.numberOfBlocks(), none)
        , m_firstNumbers(FillWith { }, index.numberOfBlocks(), 0u)
        , m_endNumbers(FillWith { }, index.numberOfBlocks(), 0u)
    {
        uint32_t numberOfBlocks = index.numberOfBlocks();
        Vector<uint32_t> firstBlocksBelow(FillWith { }, numberOfBlocks, none);
        Vector<uint32_t> nextBlocksBeside(FillWith { }, numberOfBlocks, none);
        Vector<uint32_t> roots;
        for (uint32_t number = 0; number < numberOfBlocks; ++number) {
            BlockID block(number);
            if (!index.isReachable(block))
                continue;
            BodyTerminator end = index.terminator(block);
            if (end.kind() != TerminatorKind::Jump || index.loopOf(block) || index.block(block).hasHandler()) {
                roots.append(number);
                continue;
            }
            uint32_t target = end.targets()[0].block().index();
            nextBlocksBeside[number] = firstBlocksBelow[target];
            firstBlocksBelow[target] = number;
        }
        uint32_t next = 0;
        Vector<std::pair<uint32_t, uint32_t>> path;
        for (uint32_t root : roots) {
            path.append({ root, firstBlocksBelow[root] });
            m_firstNumbers[root] = next++;
            m_roots[root] = root;
            while (!path.isEmpty()) {
                auto& [block, below] = path.last();
                if (below == none) {
                    m_endNumbers[block] = next;
                    path.removeLast();
                    continue;
                }
                uint32_t entered = std::exchange(below, nextBlocksBeside[below]);
                m_firstNumbers[entered] = next++;
                m_roots[entered] = root;
                path.append({ entered, firstBlocksBelow[entered] });
            }
        }
    }

    bool areMutuallyExclusive(Vector<BlockID>& blocks) const
    {
        std::ranges::sort(blocks, { }, [&](BlockID block) { return m_firstNumbers[block.index()]; });
        for (size_t place = 0; place < blocks.size(); ++place) {
            uint32_t block = blocks[place].index();
            if (!m_index.isReachable(blocks[place]) || m_index.loopOf(blocks[place]) || m_index.block(blocks[place]).hasHandler())
                return false;
            if (place && m_firstNumbers[block] < m_endNumbers[blocks[place - 1].index()])
                return false;
            if (m_roots[block] != m_roots[blocks[0].index()] && !(endsBody(block) && endsBody(blocks[0].index())))
                return false;
        }
        return true;
    }

private:
    bool endsBody(uint32_t block) const
    {
        BlockID root(m_roots[block]);
        bool leavesBlock = false;
        m_index.block(root).forEachOutgoingEdge([&](BodyTarget, bool) { leavesBlock = true; });
        return !leavesBlock;
    }

    const BodyIndex& m_index;
    Vector<uint32_t> m_roots;
    Vector<uint32_t> m_firstNumbers;
    Vector<uint32_t> m_endNumbers;
};

void WriterOfLink::appendExclusiveCallees(FunctionID caller, std::span<const LinkedCall> callsOfCaller, std::span<const uint32_t> numberOfCallsThatEnter, Vector<FunctionID>& callees) const
{
    Vector<LinkedCall> calls;
    for (const LinkedCall& call : callsOfCaller) {
        const LinkedFunction& callee = m_tables.functions[call.callee.index()];
        if (numberOfCallsThatEnter[call.callee.index()] > 1 && callee.everyCallHasOneCallee && !callee.runsAtMostOnce)
            calls.append(call);
    }
    std::ranges::sort(calls, { }, [](const LinkedCall& call) { return std::pair { call.callee, call.site }; });
    const BodyIndex* index = nullptr;
    std::optional<JumpForest> forest;
    size_t end = 0;
    for (size_t first = 0; first < calls.size(); first = end) {
        FunctionID callee = calls[first].callee;
        bool mayComeAgain = false;
        for (end = first; end < calls.size() && calls[end].callee == callee; ++end)
            mayComeAgain |= calls[end].mayComeAgain;
        if (mayComeAgain || end - first != numberOfCallsThatEnter[callee.index()])
            continue;
        if (!forest) {
            index = bodiesLookedAt(m_flow.looks).find(caller);
            if (!index || index->hasIrreducibleFlow())
                return;
            forest.emplace(*index);
        }
        Vector<BlockID> blocks;
        for (const LinkedCall& call : calls.span().subspan(first, end - first))
            blocks.append(index->blockOf(call.site));
        if (forest->areMutuallyExclusive(blocks))
            callees.append(callee);
    }
}

FunctionID WriterOfLink::onlyForwardedCallee(FunctionID caller, const SummaryCall& call, const LinkFlow::CalleesOfCall& callees, std::span<const uint32_t> numberOfCallsThatEnter) const
{
    if (call.callee != SummaryCall::Callee::Name || call.isConstruction || call.isInLoop || call.isMadeByProtocol || call.receiver == noSummaryNode || !call.receiver)
        return { };
    if (call.key != m_program.keyOf(BuiltinNames::call) && call.key != m_program.keyOf(BuiltinNames::apply))
        return { };
    if (callees.listed().size() != 1 || !callees.shared().isEmpty())
        return { };
    FunctionID callee = callees.listed()[0];
    LinkType closuresOfCallee;
    closuresOfCallee.functions = LinkType::Functions::Closure;
    closuresOfCallee.function = callee;
    if (m_flow.typeAt(m_flow.nodeOf(caller, call.receiver)) != closuresOfCallee)
        return { };
    size_t index = callee.index();
    bool isNamedOnce = m_functionsSharingCalls.get(index) && !m_functionsSharingSeveralCalls.get(index) && !numberOfCallsThatEnter[index];
    return isNamedOnce && everyCallerIsKnownOf(index) && !m_functionsThatTextsOfLibraryName.get(index) && isEnteredByCallsAloneForLink(m_program.at(callee).kind) ? callee : FunctionID();
}

void WriterOfLink::findFunctionsThatRunAtMostOnce()
{
    Vector<uint32_t> numberOfCallsThatEnter(FillWith { }, m_numberOfFunctions, 0u);
    for (const LinkedCall& call : m_tables.calls)
        ++numberOfCallsThatEnter[call.callee.index()];
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index)
        m_tables.functions[index].hasSingleCallSite = numberOfCallsThatEnter[index] == 1 && m_tables.functions[index].everyCallHasOneCallee;

    Vector<FunctionID> functionsToFollow;
    for (size_t body : bodiesThatLoaderEntersAtMostOnce(m_program)) {
        if (m_flow.reached.get(body))
            functionsToFollow.append(FunctionID(static_cast<uint32_t>(body)));
    }
    while (!functionsToFollow.isEmpty()) {
        FunctionID caller = functionsToFollow.takeLast();
        if (std::exchange(m_tables.functions[caller.index()].runsAtMostOnce, true))
            continue;
        auto calls = std::ranges::equal_range(m_tables.calls, caller, { }, &LinkedCall::caller);
        for (const LinkedCall& call : calls) {
            if (!call.mayComeAgain && numberOfCallsThatEnter[call.callee.index()] == 1 && m_tables.functions[call.callee.index()].everyCallHasOneCallee)
                functionsToFollow.append(call.callee);
        }
        appendExclusiveCallees(caller, std::span { calls.begin(), calls.end() }, numberOfCallsThatEnter.span(), functionsToFollow);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        uint32_t firstCall = m_flow.firstCallOfFunction[caller.index()];
        for (uint32_t call = 0; call < summary.calls.size(); ++call) {
            if (FunctionID callee = onlyForwardedCallee(caller, summary.calls[call], m_flow.calleesOfCallsAsSettled()[firstCall + call], numberOfCallsThatEnter.span()))
                functionsToFollow.append(callee);
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

void WriterOfLink::findRingsOfTailCalls()
{
    struct TailCall {
        uint32_t from;
        uint32_t to;
        bool entersBody;
    };
    auto mayReturnRawValue = [&](FunctionID id) {
        return m_tables.functions[id.index()].everyCallHasOneCallee && mayReturnInFormOfItsOwn(m_program.at(id));
    };
    uint32_t anyLostFunction = m_numberOfFunctions;
    Vector<TailCall> tailCalls = collectInParallel<TailCall>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<TailCall>& tailCalls) {
        if (m_flow.lostFunctions.get(index))
            tailCalls.append(TailCall { anyLostFunction, index, false });
        if (!m_flow.reached.get(index))
            return;
        FunctionID caller(index);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        for (uint32_t place = 0; place < summary.calls.size(); ++place) {
            const SummaryCall& call = summary.calls[place];
            if (!call.isInTailPosition || (call.callee == SummaryCall::Callee::Builtin && callsOfBuiltinComeBack(m_program, call.builtin)))
                continue;
            if (FunctionID callee = call.passesList ? FunctionID() : m_tables.calleeAt(caller, call.site)) {
                const Function& entered = m_program.at(callee);
                bool entersBody = call.callee == SummaryCall::Callee::Binding && canBeCalledByValueOf(entered.role) && !entered.has(FunctionCapabilities::RecordsCallee) && mayReturnRawValue(callee) && mayReturnRawValue(caller);
                tailCalls.append(TailCall { index, callee.index(), entersBody });
                continue;
            }
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[index] + place];
            for (FunctionID callee : callees.listed())
                tailCalls.append(TailCall { index, callee.index(), false });
            for (uint32_t shared : callees.shared())
                tailCalls.append(TailCall { index, anyLostFunction + 1 + shared, false });
            if (callees.mayEnterLost)
                tailCalls.append(TailCall { index, anyLostFunction, false });
        }
    });
    uint32_t numberOfSharedCalls = static_cast<uint32_t>(m_flow.calleesOfSharedCallsAsSettled().size());
    for (uint32_t shared = 0; shared < numberOfSharedCalls; ++shared) {
        for (FunctionID callee : m_flow.calleesOfSharedCallsAsSettled()[shared].listed())
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

ALWAYS_INLINE void WriterOfLink::writeParameters(FunctionID id, const LinkedFunction& linked)
{
    const Function& function = m_program.at(id);
    std::span<LinkedParameter> parameters = m_tables.parameters.mutableSpan().subspan(linked.firstParameter, linked.numberOfParameters);
    RELEASE_ASSERT(parameters.size() == function.numberOfEntryParameters());
    uint32_t restParameter = function.has(FunctionFlags::HasRestParameter) ? function.firstParameterIndex() + function.numberOfPositionalParameters() : none;
    std::span<const LinkedParameter> inProvingReading = m_proving ? m_proving->parametersOf(id) : std::span<const LinkedParameter>();

    for (uint32_t index = 0; index < parameters.size(); ++index) {
        LinkedParameter& parameter = parameters[index];
        const LinkType& held = m_flow.typeAt(m_flow.parameter(id, index));
        const LinkType arrived = m_flow.withoutObjectsThatHaveEscaped(held);
        const DeclaredType* declared = function.declaredEntryParameter(index);

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
            if (m_proving)
                parameter.written = inProvingReading[index].written;
            else
                parameter.written = declared && !declared->isEstablished() ? unlessAnything(widened(declared->written())) : TypeID();
            break;
        case EntryParameter::This:
        case EntryParameter::Parameter: {
            prove(index == restParameter ? TypeID() : typeIDOf(arrived.asPassed()));
            if (!m_tables.classesPassedToParameters.isEmpty() && index != restParameter && (!parameter.proven || typesAtHand().isUninformative(parameter.proven) || typesAtHand().at(parameter.proven).hasUnboundedUnknown() || (typesAtHand().at(parameter.proven).primitives & Primitive::All) == Primitive::All)) {
                if (auto passed = m_tables.classesPassedToParameters.find(m_flow.parameter(id, index).index()); passed != m_tables.classesPassedToParameters.end())
                    parameter.proven = typesAtHand().withPrimitives(typesAtHand().builtinClass(BuiltinClassID(passed->value - 1)), Primitive::UndefinedOrNull);
            }
            parameter.staysAsItsClassMakesIt = !writesWhatIsExpected() && linked.everyCallerIsKnown && index != restParameter && holdsOnlyInstancesThatStayAsMade(arrived);
            bool isThis = function.entryParameterKind(index) == EntryParameter::This;
            TypeID written;
            if (m_proving && !isThis)
                written = inProvingReading[index].written;
            else {
                written = declared && !declared->isEstablished() ? unlessAnything(widened(declared->written())) : TypeID();
                if (!written)
                    written = unlessAnything(widened(typeExpectedByLibraryFunction(m_program, typesAtHand(), id, index)));
                if (!written && isThis)
                    written = typeOfLiteralAndWhatIsCreatedBelow(id);
            }
            LinkType seen = held;
            seen.hasUnknownObjects = false;
            seen.holdsLostBuiltins = false;
            seen.counts = LinkType::Counts::Unknown;
            if (FlowNodeID heirs = function.entryParameterKind(index) == EntryParameter::This ? m_flow.findHeirsThatRun(id) : FlowNodeID(); heirs && !m_flow.typeAt(heirs).isNever())
                seen = m_flow.typeAt(heirs);
            TypeID seenToArrive = typeIDOf(seen);
            parameter.written = written;
            parameter.seenToArrive = typesAtHand().isUninformative(seenToArrive) ? TypeID() : seenToArrive;
            break;
        }
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

void WriterOfLink::indexFunctionsThatAreHeld()
{
    struct Holders {
        Vector<FunctionUnderKey> functionsUnderKeys;
        Vector<FunctionOfClass> functionsOfClasses;
        Vector<FunctionOfClass> functionsThatSlotsOfRecordsMayDefer;
        Vector<uint32_t> recordsReadUnderUnknownKeys;
        Vector<LinkedGlobalStore> globalStores;

        void note(PropertyKeyID key, LayoutID classLayout, FunctionID held)
        {
            if (!held)
                return;
            if (key)
                functionsUnderKeys.append(FunctionUnderKey { key, held });
            if (classLayout)
                functionsOfClasses.append(FunctionOfClass { classLayout, held });
        }
    };
    Vector<Holders> holdersOfWorkers(loops().width());
    auto forEachIndexOnAllThreads = [&](size_t count, const auto& functor) {
        forEachIndexInParallel(loops(), static_cast<unsigned>(numberOfRangesOfIndices(count)), [&](unsigned range, unsigned worker) {
            size_t end = std::min(count, (static_cast<size_t>(range) + 1) * indicesInRangeOfLoop);
            for (size_t index = static_cast<size_t>(range) * indicesInRangeOfLoop; index < end; ++index)
                functor(static_cast<uint32_t>(index), holdersOfWorkers[worker]);
        });
    };

    uint32_t globalObject = m_program.builtins().canonicalID(Builtins::GlobalThis).index();
    m_tables.listsGlobalStores = !writesWhatIsExpected();
    m_tables.globalStores.shrink(0);
    forEachIndexOnAllThreads(m_numberOfFunctions, [&](uint32_t index, Holders& holders) {
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        for (uint32_t place = 0; m_tables.listsGlobalStores && place < summary.accesses.size(); ++place) {
            const SummaryAccess& access = summary.accesses[place];
            if (access.kind != SummaryAccess::Kind::Store || !access.key || !isGlobalObject(m_program, m_tables, summary, access.subject, globalObject))
                continue;
            holders.globalStores.append({ FunctionID(index), place });
            if (access.isToContents() || access.value == noSummaryNode || !m_flow.reached.get(index))
                continue;
            const SummaryNode& stored = summary.nodes[access.value];
            if (stored.kind == SummaryNode::Kind::Closure)
                holders.note(access.key, { }, FunctionID(stored.first));
            else if (stored.kind == SummaryNode::Kind::Variable)
                holders.note(access.key, { }, closureAloneAt(m_flow.variable(VariableID(stored.first))));
        }
        for (const SummaryMember& member : summary.members) {
            if (const SummaryNode& given = summary.nodes[member.value]; given.kind == SummaryNode::Kind::Closure)
                holders.note(member.key, member.classLayout, FunctionID(given.first));
            holders.note(member.key, member.classLayout, closureAloneAt(m_flow.member(member.classLayout, member.side, member.index)));
        }
        if (m_flow.reached.get(index)) {
            for (const SummaryEdge& edge : summary.edges) {
                const SummaryNode& laid = summary.nodes[edge.from];
                const SummaryNode& field = summary.nodes[edge.to];
                if (laid.kind == SummaryNode::Kind::Closure && field.kind == SummaryNode::Kind::Field && !m_program.at(LayoutID(field.first)).isEnvironment())
                    holders.note(fieldAt(m_program, { LayoutID(field.first), field.second }).key, { }, FunctionID(laid.first));
            }
            forEachRecordReadUnderUnknownKeyBy(FunctionID(index), [&](LayoutID record) {
                if (holders.recordsReadUnderUnknownKeys.isEmpty() || holders.recordsReadUnderUnknownKeys.last() != record.index())
                    holders.recordsReadUnderUnknownKeys.append(record.index());
            });
        }
    });
    Holders& holders = holdersOfWorkers[0];
    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        holders.note(name.key, name.layout, closureAloneAt(name.stored));
        holders.note(name.key, name.layout, closureAloneAt(name.storedInSight));
    }
    for (const LinkedNameOnPrototype& name : m_tables.namesOnPrototypes) {
        for (FunctionID laid : { name.function, name.expected, name.expectedGetter, name.expectedSetter })
            holders.note(name.key, name.layout, laid);
    }
    for (const LinkedNameOnFunction& name : m_tables.namesOnFunctions)
        holders.note(name.key, classOfConstructorFunction(m_program, name.holder), name.expected);
    for (const LinkedAssignment& assignment : m_tables.lastAssignments)
        holders.note(assignment.key, assignment.layout, assignment.method);
    for (const LinkedNameOnPrototype& name : m_tables.accessorsAssignedToClasses) {
        holders.note(name.key, name.layout, name.expectedGetter);
        holders.note(name.key, name.layout, name.expectedSetter);
    }
    forEachIndexOnAllThreads(m_program.layouts.size(), [&](uint32_t index, Holders& holders) {
        const Layout& layout = m_program.layouts[index];
        uint32_t numberOfFields = m_tables.layouts[index].takesCellsOfBase ? layout.ownFieldCount : static_cast<uint32_t>(layout.fields.size());
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            PropertyKeyID key = layout.fields[field].key;
            if (!key)
                continue;
            FieldReference reference { LayoutID(index), field };
            holders.note(key, { }, closureAloneAt(m_flow.field(reference)));
            const LinkedCell& cell = m_tables.field(reference);
            bool constructionAloneFillsRecord = layout.kind == LayoutKind::Record && cell.everyWriterIsKnown && !cell.isWrittenAfterConstruction;
            forEachFunctionOfType(cell.proven, [&](FunctionID held) {
                holders.note(key, { }, held);
                if (constructionAloneFillsRecord && m_program.at(held).captures.size() == 1)
                    holders.functionsThatSlotsOfRecordsMayDefer.append(FunctionOfClass { LayoutID(index), held });
            });
        }
        for (const StaticField& field : layout.staticFields) {
            if (!field.key || !field.variable)
                continue;
            holders.note(field.key, { }, closureAloneAt(m_flow.variable(field.variable)));
            forEachFunctionOfType(m_tables.variable(field.variable).proven, [&](FunctionID held) {
                holders.note(field.key, { }, held);
            });
        }
    });
    m_flow.forEachAppendedField([&](PropertyKeyID key, FlowNodeID holder) {
        holders.note(key, { }, closureAloneAt(holder));
    });
    for (size_t place = 0; place < m_tables.appendedKeys.size(); ++place) {
        forEachFunctionOfType(m_tables.appendedFields[place].proven, [&](FunctionID held) {
            holders.note(m_tables.appendedKeys[place], { }, held);
        });
    }

    for (Holders& ofWorker : holdersOfWorkers) {
        m_functionsUnderKeys.appendVector(ofWorker.functionsUnderKeys);
        m_functionsOfClasses.appendVector(ofWorker.functionsOfClasses);
        m_functionsThatSlotsOfRecordsMayDefer.appendVector(ofWorker.functionsThatSlotsOfRecordsMayDefer);
        m_recordsReadUnderUnknownKeys.appendVector(ofWorker.recordsReadUnderUnknownKeys);
        m_tables.globalStores.appendVector(ofWorker.globalStores);
    }
    std::ranges::sort(m_tables.globalStores);
    std::ranges::sort(m_recordsReadUnderUnknownKeys);
    m_recordsReadUnderUnknownKeys.shrink(std::ranges::unique(m_recordsReadUnderUnknownKeys).begin() - m_recordsReadUnderUnknownKeys.begin());
    sortAndRemoveDuplicates(loops(), m_functionsUnderKeys);
    sortAndRemoveDuplicates(loops(), m_functionsOfClasses);
    sortAndRemoveDuplicates(loops(), m_functionsThatSlotsOfRecordsMayDefer);

    auto putInQueueFromStart = [&](FunctionID id) {
        if (everyCallerIsKnownOf(id.index()) && m_flow.classOfConstructor(id))
            return;
        m_tables.functions[id.index()].liesUnderNameOrIsLost = m_program.at(id).hasBody();
        m_tables.functions[id.index()].isInQueueFromStart = m_flow.reached.get(id.index()) && m_program.at(id).hasBody() && !mayTakeArrivals(id);
    };
    for (const FunctionUnderKey& held : m_functionsUnderKeys)
        putInQueueFromStart(held.function);
    for (size_t lost : m_flow.lostFunctions)
        putInQueueFromStart(FunctionID(static_cast<uint32_t>(lost)));
    for (const LinkedMember& above : m_tables.membersAboveCreatedLayouts)
        putInQueueFromStart(above.function);
    if (!writesWhatIsExpected()) {
        findFunctionsTakenOutOfRequiredFiles();
        for (auto [variable, function] : m_tables.functionsTakenOutOfRequiredFiles)
            putInQueueFromStart(function);
        for (const LinkedFunctionUnderName& laid : m_tables.functionsLaidUnderNamesOfVariables)
            putInQueueFromStart(laid.function);
        findFunctionsWrittenOverBuiltins();
        for (const LinkedFunctionWrittenOverBuiltin& written : m_tables.functionsWrittenOverBuiltins)
            putInQueueFromStart(written.function);
    }
    FunctionID sorts = functionThatSorts(m_program);
    for (size_t entered : m_flow.libraryFunctionsEntered) {
        if (m_functionsEnteredByHost.get(entered) || (sorts && sorts.index() == entered))
            putInQueueFromStart(FunctionID(static_cast<uint32_t>(entered)));
    }
    Vector<FunctionID> textsInQueue;
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (m_tables.functions[index].isInQueueFromStart)
            textsInQueue.append(FunctionID(index));
    }
    while (!textsInQueue.isEmpty()) {
        for (FunctionID named : m_flow.summaries.at(textsInQueue.takeLast()).functionsNamed) {
            if (!m_functionsThatTextsOfLibraryName.get(named.index()) || m_tables.function(named).isInQueueFromStart)
                continue;
            putInQueueFromStart(named);
            if (m_tables.function(named).isInQueueFromStart)
                textsInQueue.append(named);
        }
    }
}

FunctionID WriterOfLink::addGroup(LinkedGroup::Kind kind, uint32_t subject)
{
    m_tables.groups.append(LinkedGroup { kind, subject, 0 });
    return FunctionID(m_tables.numberOfNodesOfReferrers() - 1);
}

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
    if (!familiesWhoseConstructorsArrive.isEmpty()) {
        const Families& families = m_flow.families;
        uint32_t numberOfFamilies = families.size();
        uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
        m_firstFamilyRightBelow.fill(0, numberOfFamilies + 2);
        m_firstLayoutNearestToFamily.fill(0, numberOfFamilies + 2);
        for (uint32_t family = 0; family < numberOfFamilies; ++family) {
            if (FamilyID above = families.nearestFamilyAbove(FamilyID(family)))
                ++m_firstFamilyRightBelow[above.index() + 2];
        }
        for (uint32_t layout = 0; layout < numberOfLayouts; ++layout) {
            if (FamilyID nearest = families.nearestFamilyOf(LayoutID(layout)))
                ++m_firstLayoutNearestToFamily[nearest.index() + 2];
        }
        for (uint32_t next = 2; next < numberOfFamilies + 2; ++next) {
            m_firstFamilyRightBelow[next] += m_firstFamilyRightBelow[next - 1];
            m_firstLayoutNearestToFamily[next] += m_firstLayoutNearestToFamily[next - 1];
        }
        m_familiesRightBelow.grow(m_firstFamilyRightBelow.last());
        m_layoutsNearestToFamily.grow(m_firstLayoutNearestToFamily.last());
        for (uint32_t family = 0; family < numberOfFamilies; ++family) {
            if (FamilyID above = families.nearestFamilyAbove(FamilyID(family)))
                m_familiesRightBelow[m_firstFamilyRightBelow[above.index() + 1]++] = FamilyID(family);
        }
        for (uint32_t layout = 0; layout < numberOfLayouts; ++layout) {
            if (FamilyID nearest = families.nearestFamilyOf(LayoutID(layout)))
                m_layoutsNearestToFamily[m_firstLayoutNearestToFamily[nearest.index() + 1]++] = LayoutID(layout);
        }
    }

    m_groupOfFunctionsInFields.fill(FunctionID(), m_program.layouts.size());
    for (uint32_t record : m_recordsReadUnderUnknownKeys) {
        bool namesSomebody = false;
        forEachFunctionInFieldsOf(LayoutID(record), [&](FunctionID) {
            namesSomebody = true;
        });
        if (namesSomebody)
            m_groupOfFunctionsInFields[record] = addGroup(LinkedGroup::Kind::FunctionsInFields, record);
    }

    BitVector sharedCallsInUse(m_flow.calleesOfSharedCallsAsSettled().size());
    for (const LinkFlow::CalleesOfCall& callees : m_flow.calleesOfCallsAsSettled()) {
        for (uint32_t shared : callees.shared())
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

void WriterOfLink::forEachFunctionListedWhateverCameFirst(const LinkFlow::CalleesOfCall& callees, const Invocable<void(FunctionID)> auto& name) const
{
    for (FunctionID callee : callees.listed()) {
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
    case LinkedGroup::Kind::Constructors: {
        Vector<uint32_t, 16> families { group.subject };
        while (!families.isEmpty()) {
            uint32_t family = families.takeLast();
            for (LayoutID member : m_layoutsNearestToFamily.span().subspan(m_firstLayoutNearestToFamily[family], m_firstLayoutNearestToFamily[family + 1] - m_firstLayoutNearestToFamily[family]))
                name(m_program.at(member).constructor);
            for (FamilyID below : m_familiesRightBelow.span().subspan(m_firstFamilyRightBelow[family], m_firstFamilyRightBelow[family + 1] - m_firstFamilyRightBelow[family])) {
                if (FunctionID groupBelow = m_groupOfConstructors[below.index()])
                    name(groupBelow);
                else
                    families.append(below.index());
            }
        }
        return;
    }
    case LinkedGroup::Kind::FunctionsInFields:
        forEachFunctionInFieldsOf(LayoutID(group.subject), [&](FunctionID member) {
            RELEASE_ASSERT(member.index() < m_numberOfFunctions);
            name(member);
        });
        return;
    case LinkedGroup::Kind::SharedCall:
        forEachFunctionNamedBySharedCall(m_sharedCallsOfGroups[group.subject], name);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
    case LinkType::Functions::Set:
        for (FunctionID member : m_flow.functionsOfSet(held)) {
            if (!m_flow.lostFunctions.get(member.index()))
                name(member);
        }
        return;
    case LinkType::Functions::Lost:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WriterOfLink::forEachFunctionOfType(TypeID type, const Invocable<void(FunctionID)> auto& name) const
{
    if (!type || holdsOnlyPrimitives(type))
        return;
    TypeReader reader(m_program);
    bool namesOneFunction = !!singleFunctionOf(reader, reader.at(type));
    forEachFunctionOf(reader, reader.at(type), [&](FunctionID named) {
        if (namesOneFunction || !m_flow.lostFunctions.get(named.index()))
            name(named);
    });
    const BuiltinTable& builtins = m_program.builtins();
    for (const ClassPart& part : classPartsOf(reader, reader.at(type))) {
        if (!part.hasWrittenContents())
            continue;
        for (BuiltinSlotID slot : builtins.at(part.builtinClass).slots) {
            if (const BuiltinSlot& word = builtins.at(slot); word.wordOfCell == BuiltinWordOfCell::Followed && word.indexInType)
                forEachFunctionOfType(part.writtenContents[*word.indexInType], name);
        }
    }
}

void WriterOfLink::forEachRecordReadUnderUnknownKeyBy(FunctionID reader, const Invocable<void(LayoutID)> auto& functor) const
{
    std::span<const uint32_t> nodesOfSummary = m_flow.nodesOfSummaryOf(reader);
    for (const SummaryAccess& access : m_flow.summaries.at(reader).accesses) {
        if (access.kind != SummaryAccess::Kind::Read || access.isToContents() || access.key)
            continue;
        const LinkType& subject = m_flow.typeAt(FlowNodeID(nodesOfSummary[access.subject]));
        if (subject.objects != LinkType::Objects::Layout && subject.objects != LinkType::Objects::Set)
            continue;
        m_flow.forEachLayoutOf(subject, [&](LayoutID layout) {
            if (m_program.at(layout).kind == LayoutKind::Record)
                functor(layout);
        });
    }
}

void WriterOfLink::forEachFunctionInFieldsOf(LayoutID record, const Invocable<void(FunctionID)> auto& name) const
{
    for (uint32_t field = 0; field < m_program.at(record).fields.size(); ++field)
        forEachFunctionOfType(m_tables.field({ record, field }).proven, name);
}

void WriterOfLink::forEachNodeNamedBy(FunctionID referrer, TablesReadInOrderOfFunctions& tables, const Invocable<void(FunctionID)> auto& name) const
{
    const Function& function = m_program.at(referrer);

    if (LayoutID constructed = function.constructedClass; constructed && m_program.at(constructed).constructor == referrer) {
        for (const FunctionOfClass& member : std::ranges::equal_range(m_functionsOfClasses, constructed, { }, &FunctionOfClass::classLayout))
            name(member.function);
        LayoutID base = writtenBaseOf(m_program.at(constructed));
        if (!base && constructed.index() < m_tables.layouts.size())
            base = m_tables.layouts[constructed.index()].heritage;
        if (base)
            name(m_program.at(base).constructor);
    }

    if (!m_flow.reached.get(referrer.index()))
        return;
    name(m_anybody);

    const LinkTables* proving = m_proving;
    uint64_t rowsLeft = 0;
    uint64_t rowsNamed = 0;
    auto partsFromProvingReading = [&](bool parts) {
        if (!proving)
            return true;
        ++(parts ? rowsNamed : rowsLeft);
        return parts;
    };
    auto nameFunctionsOf = [&](TypeID type, const Invocable<TypeID()> auto& inProvingReading) {
        if (type && (!proving || partsFromProvingReading(type != inProvingReading())))
            forEachFunctionOfType(type, name);
    };

    const ThinSummary& summary = m_flow.summaries.at(referrer);
    if (!proving) {
        for (FunctionID named : summary.functionsNamed)
            name(named);
    }

    const LinkedFunction& linked = m_tables.function(referrer);
    bool takesArrivals = mayTakeArrivals(referrer);
    std::span<const LinkedParameter> parameters = m_tables.parametersOf(referrer);
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        nameFunctionsOf(parameters[index].proven, [&] {
            return proving->parametersOf(referrer)[index].proven;
        });
        nameFunctionsOf(parameters[index].writtenOrSeenToArrive(), [&] {
            return proving->parametersOf(referrer)[index].writtenOrSeenToArrive();
        });
        if (const DeclaredType* declared = proving ? nullptr : function.declaredEntryParameter(index)) {
            if (std::optional<ProvenInEveryVersion> established = declared->established())
                forEachFunctionOfType(*established, name);
        }
        if (takesArrivals)
            forEachNodeArrivingAt(m_flow.parameter(referrer, index), name);
    }
    for (uint32_t capture = 0; capture < function.captures.size(); ++capture) {
        nameFunctionsOf(m_tables.capture(referrer, capture).proven, [&] {
            return proving->capture(referrer, capture).proven;
        });
    }
    nameFunctionsOf(linked.result, [&] {
        return proving->function(referrer).result;
    });
    for (const LinkedCatch& row : tables.catches.takeRowsOf(referrer))
        forEachFunctionOfType(row.caught, name);

    for (const LinkedCall& call : m_tables.callsOf(referrer))
        name(call.callee);
    uint32_t firstCall = m_flow.firstCallOfFunction[referrer.index()];
    for (uint32_t index = 0; index < summary.calls.size(); ++index) {
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[firstCall + index];
        if (const SummaryCall& call = summary.calls[index]; call.isConstruction) {
            if (call.callee == SummaryCall::Callee::Builtin) {
                if (!proving)
                    name(findNakedConstructorOf(m_program, call.builtin));
            } else if (call.subject != noSummaryNode)
                name(findNakedConstructorOf(m_program, m_flow.typeAt(FlowNodeID(m_flow.nodesOfSummaryOf(referrer)[call.subject])).builtin));
        }
        forEachFunctionListedWhateverCameFirst(callees, name);
        for (uint32_t shared : callees.shared())
            name(m_groupOfSharedCall[shared]);
    }

    forEachFunctionNamedByCoroutinesCalledBy(m_program, m_tables, referrer, scopedLambda<void(FunctionID)>([&](FunctionID named) {
        name(named);
    }));

    for (const LinkedGetterAtRead& read : tables.gettersExpectedAtReads.takeRowsOf(referrer))
        name(read.getter);
    for (const LinkedResultOfRead& read : tables.resultsOfReadsByKey.takeRowsOf(referrer))
        forEachFunctionOfType(read.proven, name);
    for (const LinkedContentsAtOperation& contents : tables.contentsAtOperations.takeRowsOf(referrer)) {
        forEachFunctionOfType(contents.key, name);
        forEachFunctionOfType(contents.value, name);
    }

    std::span<const uint32_t> nodesOfSummary = m_flow.nodesOfSummaryOf(referrer);
    if (!writesWhatIsExpected() && !m_tables.accessorsExpectedInFields.isEmpty()) {
        auto nameAccessorsFoundUnder = [&](uint32_t holder, PropertyKeyID key) {
            if (!key || liesInThread(key) || !m_tables.keysOfAccessorsExpectedInFields.get(key.index()))
                return;
            const LinkType& subject = m_flow.typeAt(FlowNodeID(nodesOfSummary[holder]));
            if (subject.objects != LinkType::Objects::Layout && subject.objects != LinkType::Objects::Set)
                return;
            m_flow.forEachLayoutOf(subject, [&](LayoutID layout) {
                std::optional<FieldReference> field = findField(m_program, layout, key);
                if (const LinkedAccessorsOfField* laid = field ? m_tables.findAccessorsExpectedIn(*field) : nullptr) {
                    name(laid->getter);
                    name(laid->setter);
                }
            });
        };
        for (const SummaryAccess& access : summary.accesses) {
            if (!access.isToContents())
                nameAccessorsFoundUnder(access.subject, access.key);
        }
        for (const SummaryCall& call : summary.calls) {
            if (call.callee == SummaryCall::Callee::Name && call.receiver && call.receiver != noSummaryNode)
                nameAccessorsFoundUnder(call.receiver, call.key);
        }
    }
    forEachRecordReadUnderUnknownKeyBy(referrer, [&](LayoutID record) {
        name(m_groupOfFunctionsInFields[record.index()]);
    });
    for (uint32_t index = 1; index < summary.nodes.size(); ++index) {
        if (FlowNodeID inFlow(nodesOfSummary[index]); !proving || m_flow.hasUnknownObjectsAt(inFlow))
            name(closureAloneAt(inFlow));

        const SummaryNode& node = summary.nodes[index];
        switch (node.kind) {
        case SummaryNode::Kind::Variable:
            nameFunctionsOf(m_tables.variable(VariableID(node.first)).proven, [&] {
                return proving->variable(VariableID(node.first)).proven;
            });
            break;
        case SummaryNode::Kind::Field:
            nameFunctionsOf(m_tables.field({ LayoutID(node.first), node.second }).proven, [&] {
                return proving->field({ LayoutID(node.first), node.second }).proven;
            });
            break;
        case SummaryNode::Kind::Constant:
            if (!proving)
                forEachFunctionOfType(TypeID::fromBits(node.first), name);
            break;
        case SummaryNode::Kind::Closure:
            if (!proving)
                name(FunctionID(node.first));
            break;
        case SummaryNode::Kind::Allocation: {
            const Layout& declared = m_program.at(declaredLayoutOf(m_program, LayoutID(node.first)));
            if (declared.kind == LayoutKind::Instance && !proving)
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

    if (proving) {
        m_rowsLeftToProvingReading.fetch_add(rowsLeft, std::memory_order_relaxed);
        m_rowsNamedByHeldReading.fetch_add(rowsNamed, std::memory_order_relaxed);
        return;
    }
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
    Vector<Vector<uint64_t>> namingsOfChunks(division.size());
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk) {
        Vector<uint64_t>& namings = namingsOfChunks[chunk];
        Vector<uint32_t, 64> namedByOne;
        BitVector isNamed(numberOfNodes);
        FunctionID firstOfChunk(division.firstIndexOf[chunk]);
        TablesReadInOrderOfFunctions tables {
            { m_tables.catches.span(), firstOfChunk },
            { m_tables.gettersExpectedAtReads.span(), firstOfChunk },
            { m_tables.resultsOfReadsByKey.span(), firstOfChunk },
            { m_tables.contentsAtOperations.span(), firstOfChunk },
        };
        for (uint32_t index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            namedByOne.shrink(0);
            auto name = [&](FunctionID named) {
                if (named && !isNamed.quickSet(named.index()))
                    namedByOne.append(named.index());
            };
            if (FunctionID referrer(index); m_tables.isGroup(referrer))
                forEachMemberOf(m_tables.group(referrer), name);
            else
                forEachNodeNamedBy(referrer, tables, name);
            for (uint32_t named : namedByOne) {
                isNamed.quickClear(named);
                namings.append(static_cast<uint64_t>(named) << 32 | index);
            }
        }
    });
    Vector<uint64_t> namings = joinChunks<uint64_t>(loops, WTF::move(namingsOfChunks));

    sortRowsByGroup(loops, namings, numberOfNodes);
    m_tables.firstReferrer = firstRowsOfGroups(loops, numberOfNodes, namings.size(), [&](size_t row) {
        return static_cast<uint32_t>(namings[row] >> 32);
    });
    m_tables.referrers.grow(namings.size());
    forEachRangeOfIndicesInParallel(loops, namings.size(), [&](size_t first, size_t end) {
        for (size_t row = first; row < end; ++row)
            m_tables.referrers[row] = FunctionID(static_cast<uint32_t>(namings[row]));
    });
    if (m_proving) {
        m_flow.build.statistics().count(StatisticID::SpeculatedReferrerRowsSkipped, m_rowsLeftToProvingReading.load());
        m_flow.build.statistics().count(StatisticID::SpeculatedReferrerRowsAdded, m_rowsNamedByHeldReading.load());
    }
}

bool isGlobalObject(const Program& program, const LinkTables& link, const ThinSummary& summary, uint32_t node, uint32_t globalObject)
{
    const SummaryNode& subject = summary.nodes[node];
    if (subject.kind == SummaryNode::Kind::Builtin)
        return subject.first == globalObject;
    if (subject.kind == SummaryNode::Kind::Variable) {
        TypeID held = link.variable(VariableID(subject.first)).proven;
        BuiltinID named = held && !holdsOnlyPrimitives(held) ? singleBuiltinOf(TypeReader(program), program.at(held)) : BuiltinID();
        return named && program.builtins().canonicalID(named).index() == globalObject;
    }
    if (subject.kind != SummaryNode::Kind::ResultOfAccess)
        return false;
    const SummaryAccess& read = summary.accesses[subject.first];
    if (read.kind != SummaryAccess::Kind::Read || !read.key)
        return false;
    const SummaryNode& holder = summary.nodes[read.subject];
    if (holder.kind != SummaryNode::Kind::Builtin || holder.first != globalObject)
        return false;
    BuiltinName name = program.builtinNameOf(read.key);
    BuiltinID named = name ? program.builtins().findGlobal(name) : BuiltinID();
    return named && program.builtins().canonicalID(named).index() == globalObject;
}

void layReferrersOfHeldReadingOver(LinkTables& tables, LinkTables& held, const ParallelLoops& loops)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(tables.functions.size());
    RELEASE_ASSERT(held.functions.size() == numberOfFunctions && tables.firstReferrer.size() == tables.numberOfNodesOfReferrers() + 1 && held.firstReferrer.size() == held.numberOfNodesOfReferrers() + 1);
    auto nameOfGroup = [](const LinkedGroup& group) {
        return (static_cast<uint64_t>(group.kind) << 32 | group.subject) + 1;
    };
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
        std::span<uint32_t> all = referrers.mutableSpan();
        std::span<uint32_t> ofHeldReading = all.subspan(numberOfOwn);
        std::ranges::sort(all.subspan(endOfHeldFunctions));
        std::ranges::inplace_merge(ofHeldReading, ofHeldReading.begin() + (endOfHeldFunctions - numberOfOwn));
        std::ranges::inplace_merge(all, all.begin() + numberOfOwn);
        referrers.shrink(std::ranges::unique(referrers).begin() - referrers.begin());
    };

    Vector<uint32_t> firstReferrer;
    firstReferrer.fill(0, numberOfNodes + 1);
    Vector<FunctionID> all = collectInParallel<FunctionID>(loops, numberOfNodes, [&](unsigned node, Vector<FunctionID>& gathered) {
        Vector<uint32_t, 64> referrers;
        gather(node, referrers);
        firstReferrer[node + 1] = static_cast<uint32_t>(referrers.size());
        for (uint32_t referrer : referrers)
            gathered.append(FunctionID(referrer));
    });
    for (uint32_t node = 0; node < numberOfNodes; ++node)
        firstReferrer[node + 1] += firstReferrer[node];
    RELEASE_ASSERT(all.size() == firstReferrer[numberOfNodes]);
    tables.firstReferrer = WTF::move(firstReferrer);
    tables.referrers = WTF::move(all);

    held.groups = { };
    held.referrers = { };
    held.firstReferrer.fill(0, numberOfFunctions + 1);
}

static void printBreachOfClosureInLink(PrintStream& out, LinkFlow& flow, const BreachOfClosure& breach)
{
    if (breach.node) {
        flow.describe(out, breach.node);
        out.print(" climbs from ", breach.before, " to ", breach.after);
    } else
        out.print(breach.set, " grows");
    if (!breach.goneOver) {
        out.println(" | follows from the others");
        return;
    }
    out.print(" | brought by ");
    if (breach.told.function)
        out.print(breach.told, ", told at ");
    else
        out.print("an edge out of ");
    flow.describe(out, breach.goneOver);
    out.println();
}

static void stopWhereFlowIsNotClosedInLink(LinkFlow& flow, BuildContext& build)
{
    Vector<BreachOfClosure> breaches = flow.findBreachesOfClosure();
    if (breaches.isEmpty())
        return;
    StringPrintStream first;
    printBreachOfClosureInLink(first, flow, breaches[0]);
    build.diagnostics().add(InternalError {
        .kind = InternalErrorKind::FlowEndedTooEarly,
        .count = static_cast<uint32_t>(breaches.size()),
        .detail = first.toString(),
    });
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
            printBreachOfClosureInLink(out, flow, breach);
        }
    }
}

namespace {

class NamesLaidHeldAgainstTablesInLink {
public:
    NamesLaidHeldAgainstTablesInLink(const LinkFlow& flow, const LinkTables& tables)
        : m_flow(flow)
        , m_program(flow.program)
        , m_tables(tables)
        , m_hasInstance(m_program.keyOf(m_program.builtins().nameOf(WellKnownSymbol::HasInstance)))
    {
    }

    void run()
    {
        for (uint32_t index = 0; index < m_flow.summaries.size(); ++index) {
            for (const SummaryMember& member : m_flow.summaries.at(FunctionID(index)).members) {
                if (!member.isRedefined)
                    m_members.add(sideOf(member.classLayout, member.side), Vector<Member, 4> { }).iterator->value.append(Member { member.key, member.kind });
            }
        }
        m_layoutsRightBelow.grow(m_program.layouts.size());
        for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
            if (LayoutID base = m_program.layouts[index].base)
                m_layoutsRightBelow[base.index()].append(LayoutID(index));
        }
        for (size_t index : m_flow.reached) {
            FunctionID function(static_cast<uint32_t>(index));
            for (const SummaryAccess& access : m_flow.summaries.at(function).accesses)
                judge(function, access);
        }
    }

    uint32_t numberOfBreaches() const { return m_numberOfBreaches; }
    const String& firstBreach() const LIFETIME_BOUND { return m_firstBreach; }

private:
    using Kind = SummaryAccess::Kind;

    struct Member {
        PropertyKeyID key;
        MethodKind kind { MethodKind::Method };
    };

    static uint64_t sideOf(LayoutID layout, ClassSide side) { return 2 * static_cast<uint64_t>(layout.index()) + static_cast<uint64_t>(side); }

    bool classDeclaresMemberUnder(LayoutID layout, ClassSide side, PropertyKeyID key) const
    {
        auto declared = m_members.find(sideOf(layout, side));
        return declared != m_members.end() && std::ranges::any_of(declared->value, [&](const Member& member) {
            return !member.key || member.key == key;
        });
    }

    bool findsAccessorFrom(LayoutID layout, ClassSide side, PropertyKeyID key) const
    {
        for (LayoutID current = layout; current; current = m_program.at(current).base) {
            auto declared = m_members.find(sideOf(current, side));
            if (declared == m_members.end())
                continue;
            bool methodOfThatNameHidesTheRest = false;
            for (const Member& member : declared->value) {
                if (key && member.key && member.key != key)
                    continue;
                if (member.kind != MethodKind::Method)
                    return true;
                methodOfThatNameHidesTheRest |= key && member.key == key;
            }
            if (methodOfThatNameHidesTheRest)
                return false;
        }
        return false;
    }

    void forEachLayoutThatLooksUpThrough(LayoutID holder, ClassSide side, PropertyKeyID key, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        Vector<LayoutID, 16> left { holder };
        while (!left.isEmpty()) {
            LayoutID layout = left.takeLast();
            functor(layout);
            for (LayoutID below : m_layoutsRightBelow[layout.index()]) {
                if (!classDeclaresMemberUnder(below, side, key))
                    left.append(below);
            }
        }
    }

    void note(FunctionID function, const SummaryAccess& access, LayoutID layout, ASCIILiteral what)
    {
        if (!m_numberOfBreaches++)
            m_firstBreach = makeString('f', function.index(), " v"_s, access.site.index(), ", L"_s, layout.index(), ": "_s, what);
    }

    void judge(FunctionID function, const SummaryAccess& access)
    {
        if (access.isToContents() || (access.kind != Kind::Define && access.kind != Kind::Store))
            return;
        PropertyKeyID key = m_flow.keyOf(function, access);
        if (key && (m_program.at(key).kind == PropertyKeyKind::PrivateName || m_flow.nameClassOf(key) == NameClass::Proto))
            return;
        if (!key && access.keyIsSurelyIndex)
            return;

        if (FunctionID entered = m_tables.calleeAt(function, access.site); access.kind == Kind::Define && entered && m_program.at(entered).kind == FunctionKind::Setter)
            note(function, access, { }, "a definition is listed as a call of a setter"_s);

        LinkType subject = m_flow.typeAt(m_flow.nodeOf(function, access.subject));
        bool isDefinition = access.kind == Kind::Define;
        if (subject.objects != LinkType::Objects::None && m_objectsAsked.add(std::tuple { static_cast<uint32_t>(subject.objects), subject.layout.bits(), subject.family.bits(), key.bits(), isDefinition }).isNewEntry) {
            m_flow.forEachLayoutOf(subject, [&](LayoutID layout) {
                if (m_layoutsAsked.add(std::tuple { layout.index() + 1, key.bits(), isDefinition }).isNewEntry)
                    judgeOnObjects(function, access, key, layout);
            });
        }
        if (!key)
            return;
        if (subject.objects == LinkType::Objects::Prototype)
            judgeOnHolder(function, access, key, subject.layout, ClassSide::Prototype);
        else if (subject.objects == LinkType::Objects::Prototypes) {
            m_program.at(m_flow.families.layoutsOf(subject.family)).forEach([&](LayoutID layout) {
                judgeOnHolder(function, access, key, layout, ClassSide::Prototype);
            });
        }
        if (subject.functions == LinkType::Functions::Closure) {
            if (LayoutID classLayout = m_flow.classOfConstructor(subject.function))
                judgeOnHolder(function, access, key, classLayout, ClassSide::Static);
        } else if (subject.functions == LinkType::Functions::Constructors) {
            m_program.at(m_flow.families.layoutsOf(subject.constructorsOf)).forEach([&](LayoutID layout) {
                FunctionID constructor = m_program.at(layout).constructor;
                if (constructor && m_flow.classOfConstructor(constructor) == layout)
                    judgeOnHolder(function, access, key, layout, ClassSide::Static);
            });
        }
    }

    void judgeOnObjects(FunctionID function, const SummaryAccess& access, PropertyKeyID key, LayoutID id)
    {
        LayoutKind kind = m_program.at(id).kind;
        if (kind != LayoutKind::Record && kind != LayoutKind::Instance && kind != LayoutKind::Container && kind != LayoutKind::BuiltinResult)
            return;
        const LinkedLayout& linked = m_tables.layout(id);
        if (linked.reachesUnseenCode || !linked.isAllocated)
            return;
        if (key && (findField(m_program, id, key) || std::ranges::contains(m_tables.appendedKeysOf(id), key)))
            return;
        bool findsAccessor = findsAccessorFrom(id, ClassSide::Prototype, key);
        if (access.kind == Kind::Define && findsAccessor && (key || !linked.possibleChanges.contains(LayoutChange::GainsProperty)))
            note(function, access, id, "a definition lays a name that no list of the layout holds, an accessor is found above under it, and the layout stays in sight"_s);
        else if (key && !findsAccessor && !linked.isFilledUnderComputedKeys && !linked.possibleChanges.contains(LayoutChange::GainsProperty))
            note(function, access, id, "a name that no list of the layout holds is laid on its objects, and they are said to gain no property"_s);
    }

    void judgeOnHolder(FunctionID function, const SummaryAccess& access, PropertyKeyID key, LayoutID holder, ClassSide side)
    {
        if (!m_holdersAsked.add(std::tuple { holder.index() + 1, static_cast<uint32_t>(side), key.bits(), access.kind == Kind::Define }).isNewEntry)
            return;
        if (side == ClassSide::Static && m_program.at(holder).findStaticField(key))
            return;
        bool findsAccessor = findsAccessorFrom(holder, side, key);
        if (access.kind == Kind::Store && findsAccessor)
            return;
        if (findsAccessor) {
            forEachLayoutThatLooksUpThrough(holder, side, key, [&](LayoutID below) {
                if (below.index() < m_tables.layouts.size() && m_tables.findMember(m_program, below, side, key).kind == MemberFound::Kind::Accessor)
                    note(function, access, below, "a definition on a prototype object or a class object hides an accessor, and a lookup through it is still said to find the accessor"_s);
            });
            return;
        }
        if (classDeclaresMemberUnder(holder, side, key) || (side == ClassSide::Static && key != m_hasInstance))
            return;
        forEachLayoutThatLooksUpThrough(holder, side, key, [&](LayoutID below) {
            if (below.index() >= m_tables.layouts.size() || m_tables.layout(below).reachesUnseenCode || !m_tables.layout(below).isAllocated)
                return;
            if (m_tables.findsNothing(m_program, below, side, key, Of::HolderAndAbove))
                note(function, access, below, "a name is laid on a prototype object or a class object, and a lookup through it is said to find nothing"_s);
        });
    }

    const LinkFlow& m_flow;
    const Program& m_program;
    const LinkTables& m_tables;
    PropertyKeyID m_hasInstance;
    HashMap<uint64_t, Vector<Member, 4>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_members;
    Vector<Vector<LayoutID, 1>> m_layoutsRightBelow;
    HashSet<std::tuple<uint32_t, uint32_t, uint32_t, bool>> m_holdersAsked;
    HashSet<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, bool>> m_objectsAsked;
    HashSet<std::tuple<uint32_t, uint32_t, bool>> m_layoutsAsked;
    uint32_t m_numberOfBreaches { 0 };
    String m_firstBreach;
};

} // anonymous namespace

static void stopWhereNameIsLaidThatTablesDeny(const LinkFlow& flow, const LinkTables& tables)
{
    NamesLaidHeldAgainstTablesInLink judge(flow, tables);
    judge.run();
    if (!judge.numberOfBreaches())
        return;
    flow.build.diagnostics().add(InternalError {
        .kind = InternalErrorKind::FlowMissesDefinedProperty,
        .count = judge.numberOfBreaches(),
        .detail = judge.firstBreach(),
    });
}

static void writeTablesOfProvingReading(Program&, Mechanisms, std::unique_ptr<LinkFlow>&, TurnsOfReadings*, LinkTables&);
static LinkTables writeTablesOfProvingReading(Program& program, Mechanisms mechanisms, std::unique_ptr<LinkFlow>& flow)
{
    LinkTables tables;
    writeTablesOfProvingReading(program, mechanisms, flow, nullptr, tables);
    return tables;
}

struct ProgramAsItIsInLink {
    LinkTables tables;
    BitVector resultsThatNothingArrivesAt;
    BitVector parametersThatNothingArrivesAt;
    BitVector variablesThatNothingArrivesAt;
    BitVector fieldsThatNothingArrivesAt;
    Vector<uint64_t> callsThatEnterNothing;
    std::unique_ptr<LinkFlow> flow;
};

static bool givesMoreToRunOnProgramAsItIs(const BuildContext& build)
{
    return build.options().shuffleSeed >> 31;
}

static ProgramAsItIsInLink linkProgramAsItIs(Program& program, const ThinSummaries& summaries, const Families& families, const LooksAtBodies& looks, Mechanisms mechanisms, BuildContext& build, OrderOfWorklist order, decltype(LinkFlow::setsDeclaredInProgram) setsDeclaredInProgram)
{
    std::unique_ptr<LinkFlow> flow = makeUniqueWithoutFastMallocCheck<LinkFlow>(program, summaries, families, looks, mechanisms, build, order);
    flow->setsDeclaredInProgram = setsDeclaredInProgram;
    flow->isGivenMore = flow->isGivenMore && givesMoreToRunOnProgramAsItIs(build);
    flow->reachEntries();
    seedCellsSharedWithUnseenCode(*flow);
    flow->run();
    flow->forgetWaysOfFlow();

    ProgramAsItIsInLink asItIs { .tables = writeTablesOfProvingReading(program, mechanisms, flow) };
    const LinkTables& tables = asItIs.tables;
    auto nothingArrivesAt = [&](FlowNodeID node) {
        return flow->typeAt(node).isNever();
    };
    auto listsNothing = [](const LinkFlow::CalleesOfCall& callees) {
        return callees.listed().span().empty() && callees.builtins().span().empty() && !callees.mayEnterLost && !callees.mayEnterUnseen;
    };
    for (size_t index : flow->reached) {
        FunctionID id(static_cast<uint32_t>(index));
        const Function& function = program.at(id);
        const LinkedFunction& linked = tables.functions[index];
        if (function.results.size() == 1 && !function.isCoroutine() && nothingArrivesAt(flow->result(id, 0)))
            asItIs.resultsThatNothingArrivesAt.set(index);
        for (uint32_t parameter = 0; parameter < linked.numberOfParameters; ++parameter) {
            if (nothingArrivesAt(flow->parameter(id, parameter)))
                asItIs.parametersThatNothingArrivesAt.set(linked.firstParameter + parameter);
        }
        const ThinSummary& summary = summaries.at(id);
        for (uint32_t call = 0; call < summary.calls.size(); ++call) {
            const LinkFlow::CalleesOfCall& callees = flow->calleesOfCallsAsSettled()[flow->firstCallOfFunction[index] + call];
            bool entersNothing = listsNothing(callees) && std::ranges::all_of(callees.shared().span(), [&](uint32_t shared) {
                return listsNothing(flow->calleesOfSharedCallsAsSettled()[shared]);
            });
            if (entersNothing && summary.calls[call].site)
                asItIs.callsThatEnterNothing.append(LinkTables::placeOfSite(id, summary.calls[call].site));
        }
    }
    std::ranges::sort(asItIs.callsThatEnterNothing);
    for (uint32_t index = 0; index < tables.variables.size(); ++index) {
        if (nothingArrivesAt(flow->variable(VariableID(index))))
            asItIs.variablesThatNothingArrivesAt.set(index);
    }
    for (uint32_t layout = 0; layout + 1 < tables.firstFieldOfLayout.size(); ++layout) {
        uint32_t first = tables.firstFieldOfLayout[layout];
        for (uint32_t cell = first; cell < tables.firstFieldOfLayout[layout + 1]; ++cell) {
            if (nothingArrivesAt(flow->field({ LayoutID(layout), cell - first })))
                asItIs.fieldsThatNothingArrivesAt.set(cell);
        }
    }
    flow->builtinsInReach().putBackLookups(asItIs.tables.lookups);
    asItIs.flow = WTF::move(flow);
    return asItIs;
}

LinkTables linkBeforeEvaluation(const LinkFlow& flowAtStart, WrappedBodies wrappedBodies)
{
    std::unique_ptr<LinkFlow> flow = makeUniqueWithoutFastMallocCheck<LinkFlow>(flowAtStart.program, flowAtStart.summaries, flowAtStart.families, flowAtStart.looks, flowAtStart.mechanisms, flowAtStart.build, flowAtStart.orderOfWorklist());
    flow->setsDeclaredInProgram = flowAtStart.setsDeclaredInProgram;
    flow->leavesOutDirectEvals = true;
    flow->listsStoresOfWrappedBodies = wrappedBodies == WrappedBodies::StoreLikeModuleBodies;
    flow->reachEntries();
    seedCellsSharedWithUnseenCode(*flow);
    flow->run();
    flow->forgetWaysOfFlow();
    return writeTablesOfProvingReading(flowAtStart.program, flowAtStart.mechanisms, flow);
}

static void stopWhereLinkFindsLessFromMore(Program& program, BuildContext& build, const ProgramAsItIsInLink& linkedAsItIs, const LinkTables& givenMore)
{
    const LinkTables& asItIs = linkedAsItIs.tables;
    TypeArena types(program.types, ArenaKeeps::InProgram);
    uint32_t numberOfAnswers = 0;
    String first;
    auto note = [&](ASCIILiteral kind, size_t number, ASCIILiteral answer) {
        if (!numberOfAnswers++)
            first = makeString(kind, number, answer);
    };
    auto typeSaysMore = [&](TypeID ofMore, TypeID ofProgram, bool nothingArrivesInProgram) {
        return ofMore && !nothingArrivesInProgram && (!ofProgram || !types.isSubset(ofProgram, ofMore));
    };
    auto saysMoreOfInstances = [&](bool staysAsMadeGivenMore, bool staysAsMadeInProgram, TypeID ofProgram, bool nothingArrivesInProgram) {
        bool noObjectArrivesInProgram = nothingArrivesInProgram || (ofProgram && !types.at(ofProgram).hasObjects());
        return staysAsMadeGivenMore && !staysAsMadeInProgram && !noObjectArrivesInProgram;
    };
    auto cellSaysMore = [&](const LinkedCell& ofMore, const LinkedCell& ofProgram, bool nothingArrivesInProgram) {
        if (ofMore.leavesOutWhatArrives || ofProgram.leavesOutWhatArrives)
            return false;
        if (ofMore.everyWriterIsKnown && (!ofProgram.everyWriterIsKnown || typeSaysMore(ofMore.proven, ofProgram.proven, nothingArrivesInProgram)))
            return true;
        if (saysMoreOfInstances(ofMore.staysAsItsClassMakesIt, ofProgram.staysAsItsClassMakesIt, ofProgram.proven, nothingArrivesInProgram))
            return true;
        if (!nothingArrivesInProgram && ((ofMore.isFilled && !ofProgram.isFilled) || (ofMore.isFilledAtEveryRead && !ofProgram.isFilledAtEveryRead)))
            return true;
        if (ofMore.isKeptFromComparisons && !ofProgram.isKeptFromComparisons)
            return true;
        return (ofProgram.isWrittenAfterConstruction && !ofMore.isWrittenAfterConstruction) || (ofProgram.isStoredInto && !ofMore.isStoredInto);
    };

    if (asItIs.unseenCodeCanRun && !givenMore.unseenCodeCanRun)
        note("all "_s, asItIs.functions.size(), " functions: whether unseen code can run"_s);

    bool capturesLieAlike = asItIs.firstCaptureOfFunction == givenMore.firstCaptureOfFunction;
    for (size_t index = 0; index < std::min(asItIs.functions.size(), givenMore.functions.size()); ++index) {
        const LinkedFunction& ofProgram = asItIs.functions[index];
        const LinkedFunction& ofMore = givenMore.functions[index];
        if (!ofProgram.isReached)
            continue;
        if (!ofMore.isReached) {
            note("f"_s, index, " is not reached"_s);
            continue;
        }
        if ((ofMore.everyCallerIsKnown && !ofProgram.everyCallerIsKnown) || (ofMore.everyCallHasOneCallee && !ofProgram.everyCallHasOneCallee) || (ofMore.everyTakerOfResultKnowsCallee && !ofProgram.everyTakerOfResultKnowsCallee) || (ofMore.runsAtMostOnce && !ofProgram.runsAtMostOnce))
            note("f"_s, index, ": who calls it"_s);
        if (typeSaysMore(ofMore.result, ofProgram.result, linkedAsItIs.resultsThatNothingArrivesAt.get(index)))
            note("f"_s, index, ": its result"_s);
        for (uint32_t parameter = 0; ofMore.everyCallerIsKnown && parameter < std::min(ofMore.numberOfParameters, ofProgram.numberOfParameters); ++parameter) {
            const LinkedParameter& parameterOfMore = givenMore.parameters[ofMore.firstParameter + parameter];
            const LinkedParameter& parameterOfProgram = asItIs.parameters[ofProgram.firstParameter + parameter];
            bool nothingArrivesInProgram = linkedAsItIs.parametersThatNothingArrivesAt.get(ofProgram.firstParameter + parameter);
            if (typeSaysMore(parameterOfMore.proven, parameterOfProgram.proven, nothingArrivesInProgram)
                || saysMoreOfInstances(parameterOfMore.staysAsItsClassMakesIt, parameterOfProgram.staysAsItsClassMakesIt, parameterOfProgram.proven, nothingArrivesInProgram))
                note("f"_s, index, ": a parameter"_s);
        }
        if (!capturesLieAlike || index + 1 >= asItIs.firstCaptureOfFunction.size())
            continue;
        uint32_t firstCapture = asItIs.firstCaptureOfFunction[index];
        for (uint32_t capture = firstCapture; capture < asItIs.firstCaptureOfFunction[index + 1]; ++capture) {
            if (cellSaysMore(givenMore.captures[capture], asItIs.captures[capture], linkedAsItIs.parametersThatNothingArrivesAt.get(ofProgram.firstParameter + (capture - firstCapture))))
                note("f"_s, index, ": a capture"_s);
        }
    }

    for (size_t index = 0; index < std::min(asItIs.variables.size(), givenMore.variables.size()); ++index) {
        if (cellSaysMore(givenMore.variables[index], asItIs.variables[index], linkedAsItIs.variablesThatNothingArrivesAt.get(index)))
            note("variable "_s, index, ""_s);
    }

    bool fieldsLieAlike = asItIs.firstFieldOfLayout == givenMore.firstFieldOfLayout;
    for (size_t index = 0; index < std::min(asItIs.layouts.size(), givenMore.layouts.size()); ++index) {
        const LinkedLayout& ofProgram = asItIs.layouts[index];
        const LinkedLayout& ofMore = givenMore.layouts[index];
        if (!ofProgram.isAllocated)
            continue;
        if (!ofMore.isAllocated || (ofProgram.hasDirectInstances && !ofMore.hasDirectInstances) || (ofProgram.reachesUnseenCode && !ofMore.reachesUnseenCode) || !ofMore.possibleChanges.containsAll(ofProgram.possibleChanges))
            note("layout "_s, index, ": what befalls its objects"_s);
        if (ofMore.onlyFunctionThatMakesObjects && ofMore.onlyFunctionThatMakesObjects != ofProgram.onlyFunctionThatMakesObjects)
            note("layout "_s, index, ": who makes its objects"_s);
        if (!fieldsLieAlike || index + 1 >= asItIs.firstFieldOfLayout.size())
            continue;
        for (uint32_t field = asItIs.firstFieldOfLayout[index]; field < asItIs.firstFieldOfLayout[index + 1]; ++field) {
            if (cellSaysMore(givenMore.fields[field], asItIs.fields[field], linkedAsItIs.fieldsThatNothingArrivesAt.get(field)))
                note("layout "_s, index, ": a field"_s);
        }
    }

    size_t place = 0;
    for (const LinkedCall& ofMore : givenMore.calls) {
        uint64_t site = LinkTables::placeOfSite(ofMore.caller, ofMore.site);
        while (place < asItIs.calls.size() && LinkTables::placeOfSite(asItIs.calls[place].caller, asItIs.calls[place].site) < site)
            ++place;
        bool isListedInProgram = place < asItIs.calls.size() && LinkTables::placeOfSite(asItIs.calls[place].caller, asItIs.calls[place].site) == site;
        bool mayEnterOthersInProgram = isListedInProgram
            ? asItIs.calls[place].callee != ofMore.callee
            : asItIs.functions[ofMore.caller.index()].isReached && !std::ranges::binary_search(linkedAsItIs.callsThatEnterNothing, site);
        if (mayEnterOthersInProgram)
            note("f"_s, ofMore.caller.index(), ": whom a call enters"_s);
    }

    if (!numberOfAnswers)
        return;
    build.diagnostics().add(InternalError {
        .kind = InternalErrorKind::FlowNotMonotonic,
        .count = numberOfAnswers,
        .detail = first,
    });
}

LinkTables link(std::unique_ptr<LinkFlow>&& flowAtStart, TurnsOfReadings* turns)
{
    std::unique_ptr<LinkFlow> ownedFlow = WTF::move(flowAtStart);
    LinkFlow& flow = *ownedFlow;
    RELEASE_ASSERT(flow.loops.areThoseOfCurrentThread());
    Program& program = flow.program;
    Mechanisms mechanisms = flow.mechanisms;
    BuildContext& build = flow.build;
    std::optional<ProgramAsItIsInLink> asItIs;
    if (build.options().checksFlowMonotonicity) [[unlikely]]
        asItIs = linkProgramAsItIs(program, flow.summaries, flow.families, flow.looks, mechanisms, build, flow.orderOfWorklist(), flow.setsDeclaredInProgram);
    if (asItIs && givesMoreToRunOnProgramAsItIs(build)) [[unlikely]]
        flow.isGivenMore = false;
    StepTimes steps(build, "facts"_s);
    flow.reachEntries();
    seedCellsSharedWithUnseenCode(flow);
    steps.didFinish("Link: entries are reached"_s);
    flow.run();
    steps.didFinish("Link: the flow runs"_s);
    build.statistics().count(StatisticID::PreparedSymbolsAccessedByValue, flow.numberOfSymbolsThatGoByValue());
    if (String sleeper = flow.describeSleeperThatMayFindSomething(); !sleeper.isNull()) [[unlikely]]
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::FlowEndedTooEarly, .count = 1, .detail = sleeper });
    if (build.options().checksFlowFixpoint)
        stopWhereFlowIsNotClosedInLink(flow, build);
    if (build.options().recorded.validates) [[unlikely]] {
        if (Vector<String> found = flow.findWhatWasTakenAndNeitherHoldsNorWasTold(flow); !found.isEmpty())
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::StaleFlowAssumption, .count = static_cast<uint32_t>(found.size()), .detail = found[0] });
    }
    if (asItIs) [[unlikely]] {
        if (Vector<String> found = flow.findWhatWasTakenAndNeitherHoldsNorWasTold(*asItIs->flow); !found.isEmpty())
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::FlowNotMonotonic, .count = static_cast<uint32_t>(found.size()), .detail = found[0] });
        asItIs->flow = nullptr;
    }
    if (build.askedOfFlowAtRest()) [[unlikely]]
        build.askedOfFlowAtRest()(flow);
    Vector<LinkedGetterAtRead> gettersReadOffTables;
    if (turns && mechanisms.contains(Mechanism::DirectCalls))
        gettersReadOffTables = findGettersReadOffTablesWhileWaysOfFlowStand(flow);
    steps.didFinish("Link: what is asked of the flow at rest"_s);
    flow.forgetWaysOfFlow();
    steps.didFinish("Link: the ways of the flow are forgotten"_s);
    if (!turns) {
        LinkTables tables = writeTablesOfProvingReading(program, mechanisms, ownedFlow);
        if (asItIs) [[unlikely]]
            stopWhereLinkFindsLessFromMore(program, build, *asItIs, tables);
        return tables;
    }

    std::unique_ptr<LinkFlow::ViewOfHeldReading> view = flow.makeViewOfHeldReading();
    SizeOfLinkFlow sizeOfFlow = flow.size();
    bool unseenCodeCanRun = flow.builtinsInReach().unseenCodeCanRun();
    LinkTables tables;
    steps.didFinish("Link: the view of the held reading"_s);
    runLanes(flow.loops, 2, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
        if (!lane) {
            writeTablesOfProvingReading(program, mechanisms, ownedFlow, turns, tables);
            return;
        }
        {
            view->loops = &loopsOfLane;
            LinkFlow::ViewAtWork viewAtWork(view.get());
            turns->held = conservativeLinkTables(program, loopsOfLane);
            turns->held.sizeOfFlow = sizeOfFlow;
            turns->held.unseenCodeCanRun = unseenCodeCanRun;
            LinkTables& expected = turns->held;
            if (!mechanisms.containsAny({ Mechanism::DirectCalls, Mechanism::TypedOperations })) {
                for (size_t index = 0; index < expected.functions.size(); ++index)
                    expected.functions[index].isReached = flow.reached.get(index);
                return;
            }
            view->functionsThatTablesMayName = &flow.reached;
            WriterOfLink writer(flow, expected);
            writer.takeGettersReadOffTables(WTF::move(gettersReadOffTables));
            lookAtSummariesForLayoutsAndCells(flow, expected);
            writer.writeWhatMakesNoType();
            writeLayoutsAndCellsAheadOfTypes(flow, writer.functionsWhoseCallersAreAllKnown());
            turns->provingTablesAreWritten.wait();
            writer.leaveToProvingReadingWhatItNames(*turns->proving);
            {
                TypeArena types(program.types, ArenaKeeps::InProgram);
                writer.writeWhatMakesTypes(types);
                writeLayoutsAndCells(flow, types);
                writeTablesOfUnions(flow, expected);
            }
            writer.writeReferrers();
        }
        destroyInBackground(build.backgroundJobs(), WTF::move(view));
    });
    steps.didFinish("Link: the tables of both readings"_s);
    if (asItIs) [[unlikely]]
        stopWhereLinkFindsLessFromMore(program, build, *asItIs, tables);
    return tables;
}

static void writeTablesOfProvingReading(Program& program, Mechanisms mechanisms, std::unique_ptr<LinkFlow>& ownedFlow, TurnsOfReadings* turns, LinkTables& tables)
{
    LinkFlow& flow = *ownedFlow;
    flow.settleTypes();
    flow.settleSharedCalls();

    tables = conservativeLinkTables(program, flow.loops);
    tables.sizeOfFlow = flow.size();
    tables.modulesWhoseValueStaysIn = flow.modulesWhoseValueStaysInSight;
    flow.stopUnlessRecordsHandedOutFrozenStayAsBorn();
    tables.modulesWhoseRecordIsHandedOutFrozen = flow.modulesWhoseRecordIsHandedOutFrozen;
    tables.modulesWhoseRecordIsHandedOutFrozen.filter(flow.modulesWhoseValueStaysInSight);
    tables.unseenCodeCanRun = flow.builtinsInReach().unseenCodeCanRun();
    flow.takeOverCallsWhoseRowsLinkTells();
    tables.lookups = flow.builtinsInReach().takeLookups();
    tables.lookups.writesFollowedToBuiltins = flow.takeWritesFollowedToBuiltins();
    tables.namesAddedByTests = flow.testRuns().takeNamesAdded();
    tables.exportsSpiedOnThatWireLeavesShut = flow.testRuns().takeExportsSpiedOnThatWireLeavesShut();
    bool writesMoreThanReach = mechanisms.containsAny({ Mechanism::DirectCalls, Mechanism::TypedOperations });

    auto endTurnOfProvingReading = [&] {
        if (!turns)
            return;
        turns->proving = &tables;
        turns->provingTablesAreWritten.signal();
    };

    if (!writesMoreThanReach) {
        for (size_t index = 0; index < tables.functions.size(); ++index)
            tables.functions[index].isReached = flow.reached.get(index);
        endTurnOfProvingReading();
        return;
    }

    WriterOfLink writer(flow, tables);
    {
        TypeArena types(program.types, ArenaKeeps::InProgram);
        runLanes(flow.loops, {
            [&] {
                StepTimes stepsOfWriter(flow.build, "facts"_s);
                runLanes(flow.loops, {
                    [&] {
                        StepTimes stepsOfLane(flow.build, "facts"_s);
                        writer.writeWhatMakesNoType();
                        stepsOfLane.didFinish("Link: what makes no type"_s);
                    },
                    [&] {
                        StepTimes stepsOfLane(flow.build, "facts"_s);
                        flow.nameLayoutsThatMet(types);
                        stepsOfLane.didFinish("Link: layouts that met are named"_s);
                        RELEASE_ASSERT(flow.setsDeclaredInProgram);
                        tables.familiesBehindFlow = makeUniqueWithoutFastMallocCheck<Families>(program, flow.setsDeclaredInProgram->span(), flow.layoutsThatMet(), flow.build.parallelLoops());
                        flow.familiesBehindFlow = tables.familiesBehindFlow.get();
                        flow.forgetNamesThatAreNoFamilies(*tables.familiesBehindFlow);
                        stepsOfLane.didFinish("Link: families behind the flow"_s);
                    },
                });
                stepsOfWriter.didFinish("Link: what makes no type beside the families behind the flow"_s);
                writer.writeWhatMakesTypes(types);
                stepsOfWriter.didFinish("Link: what makes types"_s);
            },
            [&] {
                StepTimes stepsOfLane(flow.build, "facts"_s);
                lookAtSummariesForLayoutsAndCells(flow, tables);
                writeLayoutsAndCellsAheadOfTypes(flow, writer.functionsWhoseCallersAreAllKnown());
                stepsOfLane.didFinish("Link: layouts and cells ahead of types"_s);
            },
        });
        writeLayoutsAndCells(flow, types);
        writeTablesOfUnions(flow, tables);
    }
    endTurnOfProvingReading();
    writer.findFunctionsThatRunAtMostOnce();
    runLanes(flow.loops, 3, [&](unsigned lane, const ParallelLoops& loopsOfLane) {
        if (!lane) {
            summarizeCallees(flow, tables);
            return;
        }
        if (lane == 2) {
            writer.weighLeaves();
            return;
        }
        writer.goOnWith(loopsOfLane);
        writer.writeProofsThatMakeNoType();
        writer.writeReferrers();
    });
    if (flow.build.validates())
        stopWhereNameIsLaidThatTablesDeny(flow, tables);
    if (turns)
        turns->provingFlow = WTF::move(ownedFlow);
    else
        writePrototypeObjectsThatAreSettled(flow, tables);
}

NamesAskedOfContentsOfFields gatherNamesAskedOfContentsOfFields(const Program& program, const LinkFlow& flow, const LinkTables& held)
{
    struct NameAsked {
        uint64_t cell { 0 };
        uint32_t name { 0 };
    };
    struct CellFilled {
        uint64_t value { 0 };
        uint64_t cell { 0 };
    };
    TypeReader types(program);
    auto isUninformative = [&](TypeID proven) {
        if (!proven || isNeverType(proven))
            return true;
        if (holdsOnlyPrimitives(proven))
            return (primitivesInID(proven) & Primitive::All) == Primitive::All;
        const Type& type = types.recordOf(proven);
        return type.hasUnboundedUnknown() || (!type.hasObjects() && (type.primitives & Primitive::All) == Primitive::All);
    };
    auto holdsSeveralClasses = [&](TypeID proven) {
        if (!proven || isNeverType(proven) || holdsOnlyPrimitives(proven))
            return false;
        const Type& type = types.recordOf(proven);
        return type.layouts && !holdsOneMember(type.layouts);
    };
    auto addName = [](Vector<uint64_t>& names, uint32_t name) {
        while (name / 64 >= names.size())
            names.append(0);
        names[name / 64] |= 1ull << name % 64;
    };
    auto isAmong = [](std::span<const uint64_t> names, PropertyKeyID key) {
        uint32_t name = key.bits();
        return name / 64 < names.size() && (names[name / 64] >> name % 64 & 1);
    };
    Vector<uint64_t> uninformativeFieldNames;
    for (uint32_t layout = 0; layout + 1 < held.firstAppendedFieldOfLayout.size(); ++layout) {
        std::span<const PropertyKeyID> appended = held.appendedKeysOf(LayoutID(layout));
        for (size_t place = 0; place < appended.size(); ++place) {
            if (!liesInThread(appended[place]) && (isUninformative(held.appendedFields[held.firstAppendedFieldOfLayout[layout] + place].proven) || holdsSeveralClasses(held.appendedFields[held.firstAppendedFieldOfLayout[layout] + place].proven)))
                addName(uninformativeFieldNames, appended[place].bits());
        }
    }
    if (uninformativeFieldNames.isEmpty())
        return { };
    Vector<NameAsked> asked;
    Vector<CellFilled> filled;
    Vector<uint32_t> functionsThatReadUnderSuchNames;
    auto forEachCellUnder = [&](FunctionID function, uint32_t holder, PropertyKeyID field, const Invocable<void(uint64_t)> auto& functor) {
        LinkType type = flow.typeAt(flow.nodeOf(function, holder));
        if (type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Set)
            return;
        flow.forEachLayoutOf(type, [&](LayoutID layout) {
            if (layout.index() + 1 < held.firstAppendedFieldOfLayout.size() && std::ranges::contains(held.appendedKeysOf(layout), field))
                functor(static_cast<uint64_t>(layout.index()) << 32 | field.bits());
        });
    };
    for (uint32_t number = 0; number < program.functions.size(); ++number) {
        if (!flow.reached.get(number))
            continue;
        FunctionID function(number);
        const ThinSummary& summary = flow.summaries.at(function);
        bool readsUnderSuchName = false;
        for (const SummaryAccess& access : summary.accesses.span()) {
            if (!isAmong(uninformativeFieldNames.span(), access.key))
                continue;
            readsUnderSuchName |= access.kind == SummaryAccess::Kind::Read;
            if (access.kind != SummaryAccess::Kind::Store)
                continue;
            std::span<const SummaryNode> nodes = summary.nodes.span();
            if (access.value >= nodes.size() || nodes[access.value].kind == SummaryNode::Kind::Constant)
                continue;
            forEachCellUnder(function, access.subject, access.key, [&](uint64_t cell) {
                filled.append(CellFilled { static_cast<uint64_t>(number) << 32 | access.value, cell });
            });
        }
        if (readsUnderSuchName)
            functionsThatReadUnderSuchNames.append(number);
    }
    if (filled.isEmpty())
        return { };

    Vector<uint64_t> namesOfCellsThatAreFilled;
    for (const CellFilled& entry : filled)
        addName(namesOfCellsThatAreFilled, static_cast<uint32_t>(entry.cell));
    for (uint32_t number : functionsThatReadUnderSuchNames) {
        FunctionID function(number);
        const ThinSummary& summary = flow.summaries.at(function);
        std::span<const SummaryAccess> accesses = summary.accesses.span();
        bool readsOutOfFilledCell = false;
        for (const SummaryAccess& access : accesses)
            readsOutOfFilledCell |= access.kind == SummaryAccess::Kind::Read && isAmong(namesOfCellsThatAreFilled.span(), access.key);
        if (!readsOutOfFilledCell)
            continue;
        std::span<const SummaryNode> nodes = summary.nodes.span();
        auto note = [&](uint32_t subject, PropertyKeyID name) {
            if (!name || subject >= nodes.size() || nodes[subject].kind != SummaryNode::Kind::ResultOfAccess)
                return;
            const SummaryAccess& read = accesses[nodes[subject].first];
            if (read.kind != SummaryAccess::Kind::Read || !isAmong(namesOfCellsThatAreFilled.span(), read.key))
                return;
            forEachCellUnder(function, read.subject, read.key, [&](uint64_t cell) {
                asked.append(NameAsked { cell, name.bits() });
            });
        };
        for (const SummaryAccess& access : accesses) {
            if (access.kind == SummaryAccess::Kind::Read)
                note(access.subject, access.key);
        }
        for (const SummaryCall& call : summary.calls.span()) {
            if (call.callee != SummaryCall::Callee::Name || call.isMadeByProtocol)
                continue;
            note(call.subject, call.key);
            note(call.receiver, call.key);
        }
    }

    NamesAskedOfContentsOfFields result;
    auto sortAndDropRepeats = [](auto& list) {
        std::ranges::sort(list);
        list.shrink(std::ranges::unique(list).begin() - list.begin());
    };
    for (const NameAsked& row : asked)
        result.cells.append(row.cell);
    for (const CellFilled& row : filled)
        result.cells.append(row.cell);
    sortAndDropRepeats(result.cells);
    auto placeOf = [&](uint64_t cell) {
        return static_cast<uint32_t>(std::ranges::lower_bound(result.cells, cell) - result.cells.begin());
    };
    Vector<uint32_t> groupOf;
    for (uint32_t place = 0; place < result.cells.size(); ++place)
        groupOf.append(place);
    auto find = [&](uint32_t place) {
        while (groupOf[place] != place)
            place = groupOf[place] = groupOf[groupOf[place]];
        return place;
    };
    std::ranges::sort(filled, { }, [](const CellFilled& row) {
        return std::pair(row.value, row.cell);
    });
    for (size_t index = 1; index < filled.size(); ++index) {
        if (filled[index].value != filled[index - 1].value)
            continue;
        uint32_t one = find(placeOf(filled[index].cell));
        uint32_t other = find(placeOf(filled[index - 1].cell));
        groupOf[std::max(one, other)] = std::min(one, other);
    }
    for (const NameAsked& row : asked)
        result.namesOfGroups.append({ find(placeOf(row.cell)), row.name });
    for (const CellFilled& row : filled)
        result.valuesOfGroups.append({ find(placeOf(row.cell)), row.value });
    for (uint32_t place = 0; place < result.cells.size(); ++place)
        result.cellsOfGroups.append({ find(place), place });
    sortAndDropRepeats(result.namesOfGroups);
    sortAndDropRepeats(result.valuesOfGroups);
    std::ranges::sort(result.cellsOfGroups);
    return result;
}

void expectContentsOfFieldsByNamesAsked(Program& program, const LinkFlow& flow, const NamesAskedOfContentsOfFields& asked, const LayoutsByNames& byNames, LinkTables& held)
{
    TypeArena types(program.types, ArenaKeeps::InProgram);
    for (size_t next = 0; next < asked.namesOfGroups.size();) {
        uint32_t group = asked.namesOfGroups[next].first;
        Vector<PropertyKeyID, 16> names;
        Vector<PropertyKeyID, 16> namesThatChoose;
        for (; next < asked.namesOfGroups.size() && asked.namesOfGroups[next].first == group; ++next) {
            PropertyKeyID name = PropertyKeyID::fromBits(asked.namesOfGroups[next].second);
            if (!byNames.isNameOfSomeLayout(name))
                continue;
            names.append(name);
            BuiltinName ofBuiltin = liesInThread(name) ? BuiltinName() : program.builtinNameOf(name);
            if (!ofBuiltin || program.builtins().classesWithInstanceMember(ofBuiltin).empty())
                namesThatChoose.append(name);
        }
        if (namesThatChoose.size() < 2)
            continue;
        LayoutID only;
        bool severalAnswer = false;
        for (uint32_t holder : byNames.holdersOfRarestOf(namesThatChoose.span())) {
            if (byNames.isInstance(LayoutID(holder)) && byNames.hasAll(holder, names.span()))
                severalAnswer |= !!std::exchange(only, LayoutID(holder));
        }
        if (!only || severalAnswer)
            continue;
        bool mayBeStored = false;
        for (auto value = std::ranges::lower_bound(asked.valuesOfGroups, std::pair<uint32_t, uint64_t>(group, 0)); value != asked.valuesOfGroups.end() && value->first == group && !mayBeStored; ++value) {
            LinkType stored = flow.typeAt(flow.nodeOf(FunctionID(static_cast<uint32_t>(value->second >> 32)), static_cast<uint32_t>(value->second)));
            mayBeStored = stored.hasUnknownObjects && flow.escapedLayouts.get(only.index());
            flow.forEachLayoutOf(stored, [&](LayoutID member) {
                mayBeStored |= member == only;
            });
        }
        if (!mayBeStored)
            continue;
        for (auto member = std::ranges::lower_bound(asked.cellsOfGroups, std::pair<uint32_t, uint32_t>(group, 0)); member != asked.cellsOfGroups.end() && member->first == group; ++member) {
            LayoutID layout(static_cast<uint32_t>(asked.cells[member->second] >> 32));
            std::span<const PropertyKeyID> appended = held.appendedKeysOf(layout);
            auto place = std::ranges::find(appended, PropertyKeyID::fromBits(static_cast<uint32_t>(asked.cells[member->second])));
            LinkedCell& cell = held.appendedFields[held.firstAppendedFieldOfLayout[layout.index()] + (place - appended.begin())];
            TypeID chosen = types.forLayout(only);
            if (cell.proven && !types.isUninformative(cell.proven) && (types.withoutNullish(cell.proven) == chosen || !types.isSubset(chosen, cell.proven)))
                continue;
            cell.proven = chosen;
            cell.everyWriterIsKnown = true;
            cell.leavesOutWhatArrives = false;
            ++held.numberOfCellsExpectedByNamesAsked;
        }
    }
}

void finishReadingsOfLink(Program& program, Mechanisms mechanisms, TurnsOfReadings& turns, LinkTables& proven, LinkTables& held)
{
    if (!mechanisms.containsAny({ Mechanism::DirectCalls, Mechanism::TypedOperations }))
        return;
    writePrototypeObjectsThatAreSettled(*turns.provingFlow, proven);
    BackgroundJobs& backgroundJobs = turns.provingFlow->build.backgroundJobs();
    destroyInBackground(backgroundJobs, WTF::move(turns.provingFlow));
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
