/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpectedTypesByUse.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinUses.h"
#include "AOTFlowResults.h"
#include "AOTHeat.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTPropertyAccess.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveAccesses.h"
#include "AOTSort.h"
#include <algorithm>
#include <ranges>
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// A store may create its property. What is read may be left out by the object. What is called or looked into has to be there, or the function throws.
enum class NameDemand : uint8_t { Stored, Read, Required };

struct NamedUse {
    ValueID subject;
    ValueID access;
    ValueID receiver;
    BlockID block;
    PropertyKeyID key;
    LayoutSetID stamp;
    NameDemand demand { NameDemand::Stored };
    bool isOnEveryPath { true };
    bool callsBuiltinMethod { false };
    bool isAnsweredByBuiltins { false };
};

struct NameLimits {
    size_t maxDeclarations { 0 };
    size_t maxLayouts { 0 };
};

struct LayoutsToTest {
    LayoutList layouts;
    bool leavesOutDeclarations { false };
};

enum class CountedUses : uint8_t { OnEveryPath, All };

using ExpectedValues = Vector<std::pair<ValueID, TypeID>>;
using NameList = SortedSet<PropertyKeyID, 8>;
using BuiltinClassList = SortedSet<BuiltinClassID, 4>;

// The names asked on every path from the head of a block to a return. Without a value, no path from there counts.
using AskedNames = std::optional<BitVector>;

} // anonymous namespace

static bool isOrdinaryText(const Program& program, PropertyKeyID key)
{
    return program.at(key).kind == PropertyKeyKind::Text && program.nameClassOf(key) == NameClass::Ordinary;
}

// A private name is found on the objects of its class alone, and a symbol that the program makes on the objects that the program puts it on.
// A name that the program also defines on objects without a layout does not say that the object has one.
static bool tellsLayoutsApart(const Program& program, PropertyKeyID key)
{
    PropertyKeyKind kind = program.at(key).kind;
    if (kind == PropertyKeyKind::Text)
        return isOrdinaryText(program, key) && !program.builtinNameOf(key) && !program.isDefinedOnObjectsWithoutKnownLayout(key);
    return kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::CreatedSymbol;
}

static std::optional<std::pair<ValueID, PropertyKeyID>> propertyReadBy(const Program& program, const Value& value)
{
    Opcode opcode = value.opcode;
    if (accessOf(opcode) != Access::Read)
        return std::nullopt;
    if (isNamedAccess(opcode)) {
        if (FlagMask::isFreeName(opcode) && value.isFreeName())
            return std::nullopt;
        return std::pair { value.operand(*receiverOf(opcode)), keyOfNamedAccess(program, value) };
    }
    if (!PayloadWord::key(opcode) || !OperandPosition::object(opcode))
        return std::nullopt;
    return std::pair { value.objectOperand(), value.key() };
}

// The value that stands for every value the function uses as one: a value behind its casts and tests, and what is read from one object under one name.
static Vector<ValueID> findSubjects(FunctionPassContext& context)
{
    const Function& function = context.function;
    const ValueIdentities& identities = context.analyses.identities();
    Vector<ValueID> subjects(function.values.size(), [&](size_t index) { return identities.identityOf(ValueID(index)); });

    HashMap<std::pair<ValueID, PropertyKeyID>, ValueID> firstReads;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (subjects[id.index()] != id)
                continue;
            if (auto read = propertyReadBy(context.program, function.at(id)))
                subjects[id.index()] = firstReads.add({ subjects[subjects[read->first.index()].index()], read->second }, id).iterator->value;
        }
    }
    for (ValueID& subject : subjects)
        subject = subjects[subject.index()];
    return subjects;
}

// Whether an operation that takes the value as an operand, as it is or behind casts, matches.
static bool anyUserBehindCasts(FunctionAnalyses& analyses, ValueID value, NOESCAPE const Invocable<bool(const Value& user, uint32_t operandIndex)> auto& matches)
{
    Vector<ValueID, 4> worklist { value };
    while (!worklist.isEmpty()) {
        for (const Use& use : analyses.useLists().usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = analyses.function().at(use.user);
            if (isCast(user.opcode) && !use.operandIndex)
                worklist.append(use.user);
            else if (matches(user, use.operandIndex))
                return true;
        }
    }
    return false;
}

static bool isCalleeOf(const Value& user, uint32_t operandIndex)
{
    return (user.opcode == Opcode::Call && operandIndex == OperandIndex::Call::callee) || (user.opcode == Opcode::CallList && operandIndex == OperandIndex::CallList::callee);
}

static bool throwsOnUndefined(const Value& user, uint32_t operandIndex)
{
    if (isNamedAccess(user.opcode))
        return *receiverOf(user.opcode) == operandIndex;
    if (elementAccessOf(user.opcode))
        return OperandPosition::object(user.opcode) == operandIndex;
    return isCalleeOf(user, operandIndex);
}

static NameDemand demandOf(FunctionPassContext& context, ValueID id, PropertyKeyID key)
{
    Access access = accessOf(context.function.at(id).opcode);
    // An access under a private name throws where the object lacks it, unless it defines it.
    if (context.program.at(key).kind == PropertyKeyKind::PrivateName)
        return access == Access::Define ? NameDemand::Stored : NameDemand::Required;
    if (access != Access::Read)
        return NameDemand::Stored;
    return anyUserBehindCasts(context.analyses, id, throwsOnUndefined) ? NameDemand::Required : NameDemand::Read;
}

static BuiltinClassList builtinClassesWithMethod(const Program& program, PropertyKeyID key)
{
    BuiltinClassList classes;
    BuiltinMemberName name = builtinMemberNameOf(program, key);
    if (!name || name.isPrivateAlias)
        return classes;
    for (const MemberOfClasses& method : methodsNamed(program.builtins(), name.name)) {
        for (BuiltinClassID builtinClass : method.classes)
            classes.add(builtinClass);
    }
    return classes;
}

static bool hasCalleesListedByFlow(const Program& program, const Value& call)
{
    if (!Value::opcodesWithCallees.contains(call.opcode) || !call.callees())
        return false;
    const CalleeSet& callees = program.at(call.callees());
    return callees.isComplete || !callees.functions.isEmpty();
}

static bool isCalledByNameAlone(FunctionAnalyses& analyses, const Program& program, ValueID read)
{
    return isOnlyCalled(analyses, read) && !anyUserBehindCasts(analyses, read, [&](const Value& user, uint32_t) { return hasCalleesListedByFlow(program, user); });
}

static bool callsBuiltinMethod(FunctionPassContext& context, ValueID read, PropertyKeyID key)
{
    return context.function.at(read).opcode == Opcode::PropGet && isCalledByNameAlone(context.analyses, context.program, read) && !builtinClassesWithMethod(context.program, key).isEmpty();
}

// What is called only behind a join counts as not called: this feeds a guess.
static bool isCalledInPlace(FunctionPassContext& context, ValueID read)
{
    return anyUserBehindCasts(context.analyses, read, isCalleeOf);
}

static bool isOfOtherKindInBuiltins(FunctionPassContext& context, ValueID access, PropertyKeyID key)
{
    const BuiltinTable& builtins = context.program.builtins();
    BuiltinMemberName name = builtinMemberNameOf(context.program, key);
    if (!isOrdinaryText(context.program, key) || !name || name.isPrivateAlias)
        return false;
    bool asksForMethod = accessOf(context.function.at(access).opcode) == Access::Read && isCalledInPlace(context, access);
    std::span<const BuiltinClassID> classes = builtins.classesWithInstanceMember(name.name);
    if (asksForMethod && classes.empty())
        return false;
    return std::ranges::all_of(classes, [&](BuiltinClassID builtinClass) {
        BuiltinMemberLookup found = builtins.findForInstance(builtinClass, name.name);
        return found.outcome == BuiltinMemberLookup::Outcome::Found && (builtins.at(builtins.canonicalID(found.member)).kind == BuiltinKind::Method) != asksForMethod;
    });
}

static Vector<NamedUse> collectNamedUsesOfUnknownObjects(FunctionPassContext& context)
{
    const Program& program = context.program;
    const Function& function = context.function;
    Vector<NamedUse> uses;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        // Where a test has failed, the copy that takes over is seldom entered, and the object there is none of those that were expected.
        if (function.at(block).frequency != BlockFrequency::Normal)
            continue;
        std::optional<bool> isColdBlock;
        for (ValueID id : function.at(block).values) {
            const Value& access = function.at(id);
            Opcode opcode = access.opcode;
            if (!isNamedAccess(opcode) || (FlagMask::isFreeName(opcode) && access.isFreeName()))
                continue;
            RELEASE_ASSERT(testSiteOpcodes.contains(opcode));
            ValueID receiver = access.operand(*receiverOf(opcode));
            const Type& type = program.typeOf(function, receiver);
            PropertyKeyID key = keyOfNamedAccess(program, access);
            if (!type.anyObject || type.layouts)
                continue;
            // A test for the layouts that have the name would turn such an object away, and what stands behind the test would run in the copy.
            if (PayloadWord::accessSite(opcode) && access.accessSite() && context.inputs.flow.lossOf(access.accessSite()).seesConstructedObject)
                continue;
            // A read by name is a short call, which cold code keeps. Under a private name it is an operation of the run time.
            if (!isColdBlock)
                isColdBlock = isCold(heatClassOf(context, block));
            if (*isColdBlock && program.at(key).kind != PropertyKeyKind::PrivateName && (tellsLayoutsApart(program, key) || !callsBuiltinMethod(context, id, key)))
                continue;
            // `Promise.resolve` through a variable that is expected to hold the constructor: flow has followed the one object, and it answers.
            if (TypeID expected = function.at(receiver).expected(); expected && !context.types.at(expected).builtins.isEmpty())
                continue;
            bool tellsLayouts = tellsLayoutsApart(program, key);
            bool isBuiltinMethodCall = !tellsLayouts && callsBuiltinMethod(context, id, key);
            // Among the layouts such a name tells as much as any other. Whether the object has a layout at all is for the other names to say.
            bool isAnsweredByBuiltins = !tellsLayouts && !isBuiltinMethodCall && !isOfOtherKindInBuiltins(context, id, key);
            if (isAnsweredByBuiltins && !isOrdinaryText(program, key))
                continue;
            uses.append(NamedUse { { }, id, receiver, block, key, PayloadWord::receivers(opcode) ? access.receivers() : LayoutSetID(), demandOf(context, id, key), true, isBuiltinMethodCall, isAnsweredByBuiltins });
        }
    }
    return uses;
}

static AskedNames namesAskedBehind(const Terminator& terminator, const Vector<AskedNames>& askedFrom)
{
    if (returnsToCaller(terminator.kind))
        return BitVector();
    AskedNames asked;
    for (const Target& target : terminator.targets) {
        const AskedNames& askedFromTarget = askedFrom[target.block.index()];
        if (!askedFromTarget)
            continue;
        if (asked)
            asked->filter(*askedFromTarget);
        else
            asked = *askedFromTarget;
    }
    return asked;
}

// A path through a block that is marked as seldom entered does not count, nor does one that ends in a throw or leaves for a handler.
static Vector<AskedNames> findNamesAskedOnEveryPath(FunctionPassContext& context, const Vector<BitVector>& askedIn)
{
    Vector<AskedNames> askedFrom(context.function.blocks.size());
    for (bool didChange = true; std::exchange(didChange, false);) {
        for (BlockID id : context.analyses.cfg().reversePostOrder() | std::views::reverse) {
            const Block& block = context.function.at(id);
            AskedNames asked = block.frequency == BlockFrequency::Normal ? namesAskedBehind(block.terminator, askedFrom) : std::nullopt;
            if (!asked)
                continue;
            asked->merge(askedIn[id.index()]);
            if (asked == askedFrom[id.index()])
                continue;
            askedFrom[id.index()] = WTF::move(asked);
            didChange = true;
        }
    }
    return askedFrom;
}

static void forEachSubject(std::span<NamedUse> usesBySubject, NOESCAPE const Invocable<void(std::span<NamedUse>)> auto& functor)
{
    while (!usesBySubject.empty()) {
        size_t count = std::ranges::find_if_not(usesBySubject, [&](const NamedUse& use) { return use.subject == usesBySubject[0].subject; }) - usesBySubject.begin();
        functor(usesBySubject.first(count));
        usesBySubject = usesBySubject.subspan(count);
    }
}

// An object that takes the other path is never asked for a name that one path alone asks for, so such a name tells nothing about the objects that arrive.
static void findUsesOnEveryPath(FunctionPassContext& context, std::span<NamedUse> usesBySubject)
{
    struct BranchingSubject {
        std::span<NamedUse> uses;
        uint32_t firstName { 0 };
        Vector<PropertyKeyID, 8> keys;

        uint32_t nameOf(const NamedUse& use) const { return firstName + static_cast<uint32_t>(keys.find(use.key)); }
    };
    Vector<BranchingSubject, 4> subjects;
    uint32_t numberOfNames = 0;
    forEachSubject(usesBySubject, [&](std::span<NamedUse> uses) {
        if (std::ranges::all_of(uses, [&](const NamedUse& use) { return use.block == uses[0].block; }))
            return;
        BranchingSubject subject { uses, numberOfNames, { } };
        for (const NamedUse& use : uses)
            subject.keys.appendIfNotContains(use.key);
        if (subject.keys.size() < 2)
            return;
        numberOfNames += subject.keys.size();
        subjects.append(WTF::move(subject));
    });
    if (subjects.isEmpty())
        return;

    Vector<BitVector> askedIn(context.function.blocks.size());
    for (const BranchingSubject& subject : subjects) {
        for (const NamedUse& use : subject.uses)
            askedIn[use.block.index()].set(subject.nameOf(use));
    }
    Vector<AskedNames> askedFrom = findNamesAskedOnEveryPath(context, askedIn);

    const Dominators& dominators = context.analyses.dominators();
    for (const BranchingSubject& subject : subjects) {
        BlockID commonDominator = subject.uses[0].block;
        for (const NamedUse& use : subject.uses)
            commonDominator = dominators.nearestCommonDominator(commonDominator, use.block);
        const AskedNames& asked = askedFrom[commonDominator.index()];
        if (!asked)
            continue;
        for (NamedUse& use : subject.uses)
            use.isOnEveryPath = asked->get(subject.nameOf(use));
    }
}

// A stamp that is not complete lists what flow saw arrive beside what it did not see. It joins what the names say and does not stand for it.
static bool isExpectedToFindMember(const Program& program, const Function& function, const NamedUse& use)
{
    LayoutSetID stamp = use.stamp && program.at(use.stamp).isComplete ? use.stamp : LayoutSetID();
    if (!function.at(use.receiver).expected() && !stamp)
        return false;
    std::span<const LayoutID> holders = program.layoutsWithName(use.key);
    return std::ranges::any_of(receiverCandidates(program, function, use.receiver, stamp).predicted, [&](LayoutID layout) { return std::ranges::binary_search(holders, layout); });
}

static LayoutID declarerOf(const Program& program, LayoutID layout, PropertyKeyID name)
{
    std::span<const LayoutID> holders = program.layoutsWithName(name);
    auto holder = std::ranges::lower_bound(holders, layout);
    if (holder == holders.end() || *holder != layout)
        return { };
    return program.declarersOfName(name)[holder - holders.begin()];
}

// A class and the layouts below it have an inherited field in one slot, and a test for all of them is one test.
static LayoutsToTest keepLikeliestDeclarations(FunctionPassContext& context, const LayoutList& layouts, std::span<const NamedUse> uses, size_t maxDeclarations, const LayoutList& layoutsMetElsewhere)
{
    if (layouts.size() <= maxDeclarations)
        return { layouts, false };

    const Program& program = context.program;
    const Heat& heat = context.inputs.heat;
    NameList names;
    for (const NamedUse& use : uses)
        names.add(use.key);

    ModuleID module = context.function.module;
    PropertyKeyID nameOfSubject;
    if (auto read = propertyReadBy(program, context.function.at(context.analyses.identityOf(uses[0].receiver))))
        nameOfSubject = read->second;
    // What flow saw arrive at one of the uses does arrive there, whatever else does.
    LayoutList layoutsSeenToArrive;
    for (const NamedUse& use : uses) {
        if (use.stamp)
            layoutsSeenToArrive.unite(LayoutList(program.at(use.stamp).members.span()));
    }
    using Likelihood = std::tuple<bool, bool, bool, bool, double>;
    using RankedLayout = std::pair<Likelihood, LayoutID>;
    Vector<RankedLayout, 16> likeliestFirst(layouts.size(), [&](size_t index) {
        LayoutID layout = layouts.span()[index];
        return RankedLayout { Likelihood { nameOfSubject && heat.isStoredUnder(layout, nameOfSubject), layoutsSeenToArrive.contains(layout), layoutsMetElsewhere.contains(layout), module && (program.at(layout).module == module || heat.isAllocatedIn(layout, module)), heat.allocationsOf(layout) }, layout };
    });
    AOT::stableSort(likeliestFirst, std::ranges::greater { }, &RankedLayout::first);

    Vector<LayoutID, 16> kept;
    Vector<LayoutList, 8> declarersOfNames(names.size());
    Vector<LayoutID, 8> declarers(names.size());
    for (auto [likelihood, layout] : likeliestFirst) {
        bool hasRoom = true;
        for (size_t index = 0; index < names.size(); ++index) {
            declarers[index] = declarerOf(program, layout, names[index]);
            hasRoom &= !declarers[index] || declarersOfNames[index].size() < maxDeclarations || declarersOfNames[index].contains(declarers[index]);
        }
        if (!hasRoom)
            continue;
        for (size_t index = 0; index < names.size(); ++index) {
            if (declarers[index])
                declarersOfNames[index].add(declarers[index]);
        }
        kept.append(layout);
    }
    return { LayoutList(kept.span()), kept.size() < layouts.size() };
}

// The object is expected to have every name that the uses that count ask it for.
static LayoutList layoutsExpectedFrom(const Program& program, std::span<const NamedUse> uses, CountedUses countedUses, size_t maxLayouts)
{
    // A name that this many layouts have tells none of them from the others.
    auto isCommon = [&](PropertyKeyID name) { return program.layoutsWithName(name).size() > maxLayouts; };
    NameList names;
    for (NameDemand weakestDemand : { NameDemand::Stored, NameDemand::Read, NameDemand::Required }) {
        names.clear();
        for (const NamedUse& use : uses) {
            if (countedUses == CountedUses::OnEveryPath && !use.isOnEveryPath)
                continue;
            // A name that no layout has says nothing about layouts.
            if (use.demand >= weakestDemand && !program.layoutsWithName(use.key).empty())
                names.add(use.key);
        }
        if (names.isEmpty())
            break;
        if (std::ranges::all_of(names, isCommon))
            return { };
        LayoutList layouts = allocatedLayoutsWith(program, names.span(), maxLayouts);
        if (!layouts.isEmpty())
            return layouts;
    }

    for (const NamedUse& use : uses)
        names.add(use.key);
    LayoutList layoutsWithAnyName;
    for (PropertyKeyID name : names) {
        if (isCommon(name))
            continue;
        for (LayoutID layout : program.layoutsWithName(name)) {
            if (hasMemberToFindUnder(program, layout, name))
                layoutsWithAnyName.add(layout);
        }
    }
    return layoutsWithAnyName;
}

static bool resolvesSomeUse(FunctionPassContext& context, std::span<const NamedUse> uses, const LayoutList& layouts)
{
    return std::ranges::any_of(uses, [&](const NamedUse& use) {
        const Value& access = context.function.at(use.access);
        std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access);
        if (!query)
            return false;
        query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
        query->isOnlyCalled = access.opcode == Opcode::PropGet && isOnlyCalled(context.analyses, use.access);
        query->receivers.level = KnowledgeLevel::Expected;
        query->receivers.predicted = layouts;
        PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
        // `events.removeListener === undefined`, `if (value.then)`: whoever reads a name and does not call what he got asks whether it is there.
        bool readsMethodAsValue = plan.form == AccessForm::Method && !query->isOnlyCalled;
        return plan.isResolved() && !readsMethodAsValue;
    });
}

static bool isAllocatedHere(FunctionPassContext& context, ValueID value)
{
    return !proofsOfAllocation(context.program, context.function, context.function.at(context.analyses.identityOf(value))).isEmpty();
}

static LayoutList withoutLayoutsOfOtherKind(FunctionPassContext& context, std::span<const NamedUse> uses, const LayoutList& layouts)
{
    const Program& program = context.program;
    auto holdsOnlyFunctions = [&](const FieldReference& field) {
        const Type& slotType = program.at(slotTypeOf(program, fieldAt(program, field)));
        return !slotType.isUndefinedOrNull() && !(slotType.primitives & ~Primitive::UndefinedOrNull) && slotType.withOnlyObjects().isSubsetOf(Type::function(), program);
    };
    LayoutList kept = layouts;
    for (const NamedUse& use : uses) {
        bool asksForMethod = use.demand == NameDemand::Required && isCalledInPlace(context, use.access);
        if (use.demand == NameDemand::Required && !asksForMethod)
            continue;
        LayoutList layoutsOfOtherKind;
        for (LayoutID layout : kept) {
            std::optional<FieldReference> field = findField(program, layout, use.key, BaseChain::Predicted);
            if (field && holdsOnlyFunctions(*field) != asksForMethod)
                layoutsOfOtherKind.add(layout);
        }
        if (layoutsOfOtherKind.size() < kept.size())
            kept.subtract(layoutsOfOtherKind);
    }
    return kept;
}

// The names asked on every path leave the layouts in that a name asked on one path would take out. Where they leave more layouts than a test is made for, every name counts.
static LayoutsToTest findLayoutsToTestFor(FunctionPassContext& context, std::span<const NamedUse> uses, const NameLimits& limits, const LayoutList& layoutsMetElsewhere)
{
    auto choose = [&](CountedUses countedUses) -> LayoutsToTest {
        LayoutList layoutsWithNames = layoutsExpectedFrom(context.program, uses, countedUses, limits.maxLayouts);
        if (layoutsWithNames.isEmpty() || layoutsWithNames.size() > limits.maxLayouts)
            return { };
        LayoutsToTest chosen = keepLikeliestDeclarations(context, withoutLayoutsOfOtherKind(context, uses, layoutsWithNames), uses, limits.maxDeclarations, layoutsMetElsewhere);
        if (chosen.layouts.isEmpty() || !resolvesSomeUse(context, uses, chosen.layouts))
            return { };
        return chosen;
    };
    auto takesEveryLayout = [](const LayoutsToTest& chosen) { return !chosen.layouts.isEmpty() && !chosen.leavesOutDeclarations; };

    LayoutsToTest fromEveryPath = choose(CountedUses::OnEveryPath);
    if (takesEveryLayout(fromEveryPath) || std::ranges::all_of(uses, &NamedUse::isOnEveryPath))
        return fromEveryPath;
    LayoutsToTest fromEveryName = choose(CountedUses::All);
    if (fromEveryPath.layouts.isEmpty() || takesEveryLayout(fromEveryName))
        return fromEveryName;
    return fromEveryPath;
}

static void expectOfReceivers(std::span<const NamedUse> uses, TypeID expected, ExpectedValues& expectedValues)
{
    for (const NamedUse& use : uses) {
        if (!expectedValues.containsIf([&](const auto& listed) { return listed.first == use.receiver; }))
            expectedValues.append({ use.receiver, expected });
    }
}

static std::optional<Type> receiversAmong(FunctionPassContext& context, const BuiltinClassList& classes, std::span<const PropertyKeyID> names)
{
    size_t maxReceivers = context.limit(LimitID::MaxDispatchCases);
    LayoutList layouts = allocatedLayoutsWith(context.program, names, maxReceivers);
    if (classes.size() + layouts.size() > maxReceivers)
        return std::nullopt;

    Type receivers = layouts.isEmpty() ? Type::never() : Type::forLayouts(layouts.span(), context.types);
    for (BuiltinClassID builtinClass : classes) {
        std::optional<Type> instances = testedTypeOfInstances(context.program, builtinClass, Type::anything());
        if (!instances)
            return std::nullopt;
        receivers = receivers.unionWith(*instances, context.types);
    }
    if (receivers.isNever())
        return std::nullopt;
    return receivers;
}

static std::optional<Type> receiversWithMethod(FunctionPassContext& context, PropertyKeyID key)
{
    return receiversAmong(context, builtinClassesWithMethod(context.program, key), singleElementSpan(key));
}

static std::optional<Type> receiversWithEveryMethod(FunctionPassContext& context, std::span<const NamedUse> uses, CountedUses countedUses)
{
    std::optional<BuiltinClassList> classes;
    NameList names;
    for (const NamedUse& use : uses) {
        if (countedUses == CountedUses::OnEveryPath && !use.isOnEveryPath)
            continue;
        if (use.callsBuiltinMethod) {
            BuiltinClassList classesWithMethod = builtinClassesWithMethod(context.program, use.key);
            if (classes)
                classes->intersect(classesWithMethod);
            else
                classes = WTF::move(classesWithMethod);
            names.add(use.key);
        } else if (use.demand == NameDemand::Required && !context.program.layoutsWithName(use.key).empty()) {
            classes = BuiltinClassList();
            names.add(use.key);
        }
    }
    return classes ? receiversAmong(context, *classes, names.span()) : std::nullopt;
}

static bool expectReceiversOfBuiltinMethods(FunctionPassContext& context, std::span<const NamedUse> uses, ExpectedValues& expectedValues)
{
    std::optional<Type> receivers = receiversWithEveryMethod(context, uses, CountedUses::OnEveryPath);
    if (!receivers)
        receivers = receiversWithEveryMethod(context, uses, CountedUses::All);
    for (size_t index = 0; !receivers && index < uses.size(); ++index) {
        if (uses[index].callsBuiltinMethod)
            receivers = receiversWithMethod(context, uses[index].key);
    }
    if (!receivers)
        return false;
    expectOfReceivers(uses, context.types.intern(*receivers), expectedValues);
    return true;
}

// `twin.index`, and nothing else is asked of the twin.
static LayoutList layoutsMetThatAnswer(FunctionPassContext& context, std::span<const NamedUse> uses, const LayoutList& layoutsMet)
{
    bool isExpectedToBeSomeObject = std::ranges::any_of(uses, [&](const NamedUse& use) {
        TypeID expected = context.function.at(use.receiver).expected();
        return expected && !context.types.at(expected).withOnlyObjects().isNever();
    });
    LayoutList layouts;
    if (isExpectedToBeSomeObject)
        return layouts;
    for (LayoutID layout : layoutsMet) {
        if (std::ranges::all_of(uses, [&](const NamedUse& use) { return std::ranges::binary_search(context.program.layoutsWithName(use.key), layout); }))
            layouts.add(layout);
    }
    return layouts;
}

// Answers whether the subject is settled. One whose names have more declarations than a test is made for waits until the layouts of the other subjects are known.
static bool expectLayoutsOf(FunctionPassContext& context, std::span<NamedUse> usesOfSubject, LayoutList& layoutsMet, bool waitsForOtherSubjects, ExpectedValues& expectedValues)
{
    Function& function = context.function;
    auto expectLayouts = [&](std::span<const NamedUse> uses, const LayoutList& layouts) {
        expectOfReceivers(uses, context.types.intern(Type::forLayouts(layouts.span(), context.types)), expectedValues);
    };
    bool isFollowed = std::ranges::any_of(usesOfSubject, [&](const NamedUse& use) { return isExpectedToFindMember(context.program, function, use); });
    // In a checked function what is expected of a value is what the checker says of it. Where that lists layouts the names have nothing to add: a name that none of them holds is found above them.
    bool typeListsLayouts = function.has(FunctionFlags::IsChecked) && std::ranges::any_of(usesOfSubject, [&](const NamedUse& use) {
        TypeID expected = function.at(use.receiver).expected();
        return expected && context.types.at(expected).layouts;
    });
    if (isFollowed || typeListsLayouts || isAllocatedHere(context, usesOfSubject[0].receiver))
        return true;

    auto builtinMethodCalls = std::ranges::stable_partition(usesOfSubject, std::logical_not { }, &NamedUse::callsBuiltinMethod);
    if (!builtinMethodCalls.empty() && expectReceiversOfBuiltinMethods(context, usesOfSubject, expectedValues))
        return true;
    std::span<const NamedUse> uses = usesOfSubject.first(usesOfSubject.size() - builtinMethodCalls.size());
    if (std::ranges::all_of(uses, &NamedUse::isAnsweredByBuiltins)) {
        if (uses.empty())
            return true;
        if (waitsForOtherSubjects)
            return false;
        LayoutList layouts = layoutsMetThatAnswer(context, uses, layoutsMet);
        if (!layouts.isEmpty())
            expectLayouts(uses, layouts);
        return true;
    }

    NameLimits limits { static_cast<size_t>(context.limit(LimitID::MaxDeclarationsPredictedFromNames)), static_cast<size_t>(context.limit(LimitID::MaxAccessPlanMembers)) };
    LayoutsToTest chosen = findLayoutsToTestFor(context, uses, limits, layoutsMet);
    if (chosen.layouts.isEmpty())
        return true;
    if (chosen.leavesOutDeclarations) {
        if (waitsForOtherSubjects)
            return false;
        context.gaveUp(LimitID::MaxDeclarationsPredictedFromNames, uses[0].access);
    } else if (chosen.layouts.size() <= limits.maxDeclarations)
        layoutsMet.unite(chosen.layouts);

    expectLayouts(uses, chosen.layouts);
    return true;
}

// The layouts of the receivers that the function knows or expects already. A receiver that may be one of many tells nothing.
static void addLayoutsOfKnownReceivers(FunctionPassContext& context, LayoutList& met)
{
    const Function& function = context.function;
    size_t maxLayoutsOfReceiver = context.limit(LimitID::MaxDeclarationsPredictedFromNames);
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& access = function.at(id);
            std::optional<uint32_t> receiver = receiverOf(access.opcode);
            if (!receiver || !access.operand(*receiver))
                continue;
            ReceiverCandidates candidates = receiverCandidates(context.program, function, access.operand(*receiver));
            for (const LayoutList* layouts : { &candidates.proven, &candidates.predicted }) {
                if (layouts->size() <= maxLayoutsOfReceiver)
                    met.unite(*layouts);
            }
        }
    }
}

// The type of a checked position is what every value that is passed to it was tested for, so it stays.
static DeclaredType* findUncheckedEntryParameter(Function& function, ValueID value)
{
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span().first(function.numberOfEntryParameters());
    auto parameter = std::ranges::find(parameters, value);
    if (parameter == parameters.end())
        return nullptr;
    DeclaredType* declared = function.declaredEntryParameter(static_cast<unsigned>(parameter - parameters.begin()));
    bool isUnchecked = declared && (declared->flags.contains(PositionFlags::IsExpected) || declared->type == TypeTable::anything);
    return isUnchecked ? declared : nullptr;
}

// What is expected of an entry parameter is said by its position: it reaches every use and stays when the values are typed again.
static bool expectValues(FunctionPassContext& context, const ExpectedValues& expectedValues, ExpectedTypesByUse& expectations)
{
    Function& function = context.function;
    auto narrowedTypeOf = [&](ValueID id, TypeID expected) {
        const Value& value = function.at(id);
        TypeID narrowed = context.types.intersectionOf(value.type, expected);
        TypeID expectedAlready = value.expected() ? context.types.intersectionOf(narrowed, value.expected()) : TypeTable::never;
        if (expectedAlready != TypeTable::never)
            narrowed = expectedAlready;
        return narrowed == TypeTable::never || narrowed == value.type ? TypeID() : narrowed;
    };

    Vector<ValueID, 4> changedParameters;
    for (auto [value, expected] : expectedValues) {
        TypeID narrowed = narrowedTypeOf(value, expected);
        DeclaredType* declared = narrowed ? findUncheckedEntryParameter(function, value) : nullptr;
        if (!declared)
            continue;
        if (declared->type != narrowed || !declared->flags.contains(PositionFlags::IsExpected)) {
            declared->type = narrowed;
            declared->flags.add(PositionFlags::IsExpected);
            changedParameters.append(value);
        }
        if (context.types.at(narrowed).layouts)
            expectations.didGuessFromNames(value);
    }
    // A cut renumbers the values.
    if (!changedParameters.isEmpty() && narrowValueTypes(TypePropagationInputs { context.program, context.id, function, context.analyses, context.types, context.build, &context.typingMemo }, changedParameters.span()).cutBlocks)
        return true;

    for (auto [value, expected] : expectedValues) {
        TypeID narrowed = narrowedTypeOf(value, expected);
        if (narrowed && narrowed != function.at(value).expected()) {
            expectations.expect(value, narrowed);
            if (context.types.at(narrowed).layouts)
                expectations.didGuessFromNames(value);
        }
    }
    return !changedParameters.isEmpty();
}

static ValueID receiverOfMethodCall(TestSiteReporter& reporter, const Value& use)
{
    if (use.opcode == Opcode::PropGet)
        return isCalledByNameAlone(reporter.analyses(), reporter.program(), reporter.use()) ? use.objectOperand() : ValueID();
    if (use.opcode == Opcode::Call && use.calleeKey() && !hasCalleesListedByFlow(reporter.program(), use))
        return use.thisValueOperand();
    return { };
}

// `handle.flags`, `handle.onabort = undefined`, where the handle is the native object or its stand-in written in JavaScript.
static ValueID receiverIfBuiltinPartHasLockedMember(TestSiteReporter& reporter, ValueID receiver, PropertyKeyID key, BuiltinKind kind)
{
    const Program& program = reporter.program();
    const BuiltinTable& builtins = program.builtins();
    std::optional<Type> expected = reporter.expectedPartOf(receiver);
    BuiltinMemberName name = expected ? builtinMemberNameOf(program, key) : BuiltinMemberName();
    if (!name)
        return { };
    bool reachesLockedMember = std::ranges::any_of(expected->classes, [&](const ClassPart& part) {
        BuiltinMemberLookup found = builtins.findForInstance(part.builtinClass, name.name);
        if (found.outcome != BuiltinMemberLookup::Outcome::Found || builtins.at(builtins.canonicalID(found.member)).kind != kind)
            return false;
        if (kind == BuiltinKind::Method && !hasDirectFunction(builtins, builtins.canonicalID(found.member)))
            return false;
        return isLockedLocation(program, found.holder, key);
    });
    return reachesLockedMember ? receiver : ValueID();
}

static ValueID receiverOfLockedMethodCall(TestSiteReporter& reporter, const Value& use)
{
    if (use.opcode == Opcode::PropGet && use.key() && isOnlyCalled(reporter.analyses(), reporter.use()) && use.objectOperand())
        return receiverIfBuiltinPartHasLockedMember(reporter, use.objectOperand(), use.key(), BuiltinKind::Method);
    if (use.opcode == Opcode::Call && use.calleeKey() && use.thisValueOperand())
        return receiverIfBuiltinPartHasLockedMember(reporter, use.thisValueOperand(), use.calleeKey(), BuiltinKind::Method);
    return { };
}

static ValueID receiverOfAccessorAccess(TestSiteReporter& reporter, const Value& use)
{
    if ((use.opcode != Opcode::PropGet && use.opcode != Opcode::PropSet) || !use.key())
        return { };
    return receiverIfBuiltinPartHasLockedMember(reporter, use.objectOperand(), use.key(), BuiltinKind::Accessor);
}

// `req.headers`, where the request is expected to be the one of the host's library, which makes its headers in a getter, or a stand-in that keeps them in a field.
static std::optional<Type> receiversThatAnswerWithFieldOrGetter(TestSiteReporter& reporter, const Value& use)
{
    if (use.opcode != Opcode::PropGet || !use.key())
        return std::nullopt;
    FunctionPassContext& context = reporter.context();
    // Nearly every read of a program is none of these, and the same few reads stand all over it.
    PropertyAccessCache* cache = context.program.propertyAccessCache();
    std::optional<SiteContent> content = cache ? contentOfNamedAccess(context, use, static_cast<uint8_t>(TestSitePlanner::FieldOrGetter)) : std::nullopt;
    bool isKnownToBeRefused = content && cache->refusedContents.find(*content);
    if (isKnownToBeRefused && !cache->checksAnswers) [[likely]]
        return std::nullopt;
    auto refuse = [&]() -> std::optional<Type> {
        if (content && !isKnownToBeRefused)
            cache->refusedContents.add(*content);
        return std::nullopt;
    };

    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, use);
    if (!query || query->receivers.isProven() || query->receivers.predicted.size() < 2)
        return refuse();
    query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
    const MemberPartition& partition = plan.partition;
    if (!plan.isResolved() || partition.arms.size() < 2 || !partition.rest.isEmpty())
        return refuse();
    bool everyArmHoldsData = std::ranges::all_of(partition.arms, [](const MemberArm& arm) {
        return arm.answer.kind == MemberKind::Field || (arm.answer.kind == MemberKind::Accessor && arm.answer.getter);
    });
    bool isRefusedByArms = !everyArmHoldsData || !partition.findArm(MemberKind::Accessor) || !partition.findArm(MemberKind::Field);
    // A limit that is passed is noted for the read.
    if (context.exceeds(LimitID::MaxDispatchCases, partition.arms.size(), reporter.use())) {
        RELEASE_ASSERT(!isKnownToBeRefused);
        return std::nullopt;
    }
    if (isRefusedByArms)
        return refuse();
    RELEASE_ASSERT(!isKnownToBeRefused);
    return Type::forLayouts(query->receivers.predicted.span(), context.types);
}

void reportUnionReceiverTests(TestSiteReporter& reporter)
{
    reporter.forEachCandidate([&](const Value& use) {
        auto reportTestFor = [&](ValueID receiver, const Type& tested) {
            reporter.report({ makeTypeTest(receiver, reporter.types().intern(tested)) }, reporter.analyses().effectsOf(reporter.use()).normal);
        };
        BlockFrequency frequency = reporter.function().at(reporter.point().block).frequency;
        if (frequency == BlockFrequency::Rare)
            return;
        if (std::optional<Type> receivers = frequency == BlockFrequency::Normal ? receiversThatAnswerWithFieldOrGetter(reporter, use) : std::nullopt) {
            reportTestFor(use.objectOperand(), *receivers);
            return;
        }
        ValueID receiver = receiverOfLockedMethodCall(reporter, use);
        if (!receiver && frequency != BlockFrequency::Normal)
            return;
        if (!receiver)
            receiver = receiverOfMethodCall(reporter, use);
        if (!receiver)
            receiver = receiverOfAccessorAccess(reporter, use);
        if (!receiver || !reporter.typeOf(receiver).anyObject)
            return;
        std::optional<Type> expected = reporter.expectedPartOf(receiver);
        // No test finds an instance of a class without an instance test.
        if (!expected || *expected != expected->withoutClassProofs())
            return;
        Type testable = expected->withoutClassProofs().withUntestableBuiltinsWidened();
        UnionParts parts = unionPartsOf(testable);
        if (parts.size() >= 2 && parts.size() <= static_cast<size_t>(reporter.context().limit(LimitID::MaxDispatchCases)) && isClosedUnion(parts))
            reportTestFor(receiver, testable);
    });
}

void rewriteTestedUnionReceiver(TestSiteRewriter&)
{
}

bool expectsReceiverOfCalledMethod(FunctionPassContext& context, BlockID block, const Value& call)
{
    const Function& function = context.function;
    if (!context.build.recordedOptions().usesExpectedTypes() || function.at(block).frequency != BlockFrequency::Normal)
        return false;

    ValueID read = context.analyses.identityOf(call.calleeOperand());
    const Value& callee = function.at(read);
    PropertyKeyID key = call.calleeKey();
    if (callee.opcode != Opcode::PropGet || callee.key() != key || tellsLayoutsApart(context.program, key) || !callsBuiltinMethod(context, read, key))
        return false;
    ValueID receiver = callee.objectOperand();
    const Type& type = context.program.typeOf(function, receiver);
    if (!type.anyObject || type.layouts || function.at(receiver).expected() || isAllocatedHere(context, receiver))
        return false;
    return !!receiversWithMethod(context, key);
}

// Of an object that nobody could follow, the names it is asked for are all there is to go by. Which layouts have a name is known, and where they are few the object is expected to be one of them.
bool expectLayoutsByNames(FunctionPassContext& context, ExpectedTypesByUse& expectations)
{
    Vector<NamedUse> uses = collectNamedUsesOfUnknownObjects(context);
    if (uses.isEmpty())
        return false;

    Vector<ValueID> subjects = findSubjects(context);
    for (NamedUse& use : uses)
        use.subject = subjects[use.receiver.index()];
    AOT::stableSort(uses, { }, &NamedUse::subject);
    findUsesOnEveryPath(context, uses.mutableSpan());

    ExpectedValues expectedValues;
    LayoutList layoutsMet;
    Vector<std::span<NamedUse>, 4> unsettledSubjects;
    forEachSubject(uses.mutableSpan(), [&](std::span<NamedUse> usesOfSubject) {
        if (!expectLayoutsOf(context, usesOfSubject, layoutsMet, true, expectedValues))
            unsettledSubjects.constructAndAppend(usesOfSubject);
    });
    if (!unsettledSubjects.isEmpty())
        addLayoutsOfKnownReceivers(context, layoutsMet);
    for (std::span<NamedUse> usesOfSubject : unsettledSubjects)
        expectLayoutsOf(context, usesOfSubject, layoutsMet, false, expectedValues);
    return expectValues(context, expectedValues, expectations);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
