/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTHeat.h"
#include "AOTPropertyAccess.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveAccesses.h"
#include "AOTValuePredictions.h"
#include <algorithm>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// A store may create its property. What is read may be left out by the object. What is called or looked into has to be there, or the function throws.
enum class NameDemand : uint8_t { Stored, Read, Required };

struct NamedUse {
    ValueID subject;
    ValueID access;
    ValueID receiver;
    PropertyKeyID key;
    LayoutSetID stamp;
    NameDemand demand { NameDemand::Stored };
};

using NameList = SortedSet<PropertyKeyID, 8>;

} // anonymous namespace

// A private name is found on the objects of its class alone. A name that arrays, functions, all objects or an object of the host may answer to tells no layout from them.
static bool tellsLayoutsApart(const Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        return program.nameClassOf(key) == NameClass::Ordinary && !program.builtinNameOf(key);
    case PropertyKeyKind::PrivateName:
        return true;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    Vector<ValueID> subjects(function.values.size(), [&](size_t index) {
        return identities.identityOf(ValueID(index));
    });

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

static bool throwsOnUndefined(const Value& user, uint32_t operandIndex)
{
    Opcode opcode = user.opcode;
    if (isNamedAccess(opcode))
        return *receiverOf(opcode) == operandIndex;
    if (elementAccessOf(opcode))
        return OperandPosition::object(opcode) == operandIndex;
    return (opcode == Opcode::Call && operandIndex == OperandIndex::Call::callee) || (opcode == Opcode::CallList && operandIndex == OperandIndex::CallList::callee);
}

static bool isCalledOrLookedInto(FunctionPassContext& context, ValueID read)
{
    const Function& function = context.function;
    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        for (const Use& use : context.analyses.useLists().usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = function.at(use.user);
            if (isCast(user.opcode) && !use.operandIndex)
                worklist.append(use.user);
            else if (throwsOnUndefined(user, use.operandIndex))
                return true;
        }
    }
    return false;
}

static NameDemand demandOf(FunctionPassContext& context, ValueID id, PropertyKeyID key)
{
    Access access = accessOf(context.function.at(id).opcode);
    // An access under a private name throws where the object lacks it, unless it defines it.
    if (context.program.at(key).kind == PropertyKeyKind::PrivateName)
        return access == Access::Define ? NameDemand::Stored : NameDemand::Required;
    if (access != Access::Read)
        return NameDemand::Stored;
    return isCalledOrLookedInto(context, id) ? NameDemand::Required : NameDemand::Read;
}

static Vector<NamedUse> collectNamedUsesOfUnknownObjects(FunctionPassContext& context)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();

    Vector<NamedUse> uses;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        // Where a test has failed, the copy that takes over is seldom entered, and the object there is none of those that were expected.
        if (frequencies.classOf(block) != BlockFrequency::Normal)
            continue;
        // A read by name is a short call, which cold code keeps. Under a private name it is an operation of the run time.
        bool keepsCallsByName = isCold(heatClassOf(context, block));
        for (ValueID id : function.at(block).values) {
            const Value& access = function.at(id);
            Opcode opcode = access.opcode;
            if (!isNamedAccess(opcode) || (FlagMask::isFreeName(opcode) && access.isFreeName()))
                continue;
            ValueID receiver = access.operand(*receiverOf(opcode));
            const Type& type = program.typeOf(function, receiver);
            PropertyKeyID key = keyOfNamedAccess(program, access);
            if (!type.anyObject || type.layouts || !tellsLayoutsApart(program, key))
                continue;
            if (keepsCallsByName && program.at(key).kind != PropertyKeyKind::PrivateName)
                continue;
            uses.append(NamedUse { { }, id, receiver, key, PayloadWord::receivers(opcode) ? access.receivers() : LayoutSetID(), demandOf(context, id, key) });
        }
    }
    return uses;
}

static bool isExpectedToFindMember(const Program& program, const Function& function, const NamedUse& use)
{
    const Value& receiver = function.at(use.receiver);
    if (receiver.predictionOrType() == receiver.type && !use.stamp)
        return false;
    std::span<const LayoutID> holders = program.layoutsWithName(use.key);
    return std::ranges::any_of(receiverCandidates(program, function, use.receiver, use.stamp).predicted, [&](LayoutID layout) {
        return std::ranges::binary_search(holders, layout);
    });
}

// A class and the layouts below it have an inherited field in one slot, and a test for all of them is one test. So they count once.
static bool hasFewDeclarations(const Program& program, PropertyKeyID name, size_t maxDeclarations)
{
    LayoutList declarers;
    for (LayoutID declarer : program.declarersOfName(name)) {
        if (declarers.add(declarer) && declarers.size() > maxDeclarations)
            return false;
    }
    return true;
}

static bool hasFewDeclarationsAmong(const Program& program, const LayoutList& layouts, PropertyKeyID name, size_t maxDeclarations)
{
    std::span<const LayoutID> holders = program.layoutsWithName(name);
    LayoutList declarers;
    for (LayoutID layout : layouts) {
        auto holder = std::ranges::lower_bound(holders, layout);
        if (holder == holders.end() || *holder != layout)
            continue;
        if (declarers.add(program.declarersOfName(name)[holder - holders.begin()]) && declarers.size() > maxDeclarations)
            return false;
    }
    return true;
}

static bool areFew(const Program& program, const LayoutList& layouts, std::span<const NamedUse> uses, size_t maxDeclarations)
{
    return std::ranges::all_of(uses, [&](const NamedUse& use) {
        return hasFewDeclarationsAmong(program, layouts, use.key, maxDeclarations);
    });
}

static LayoutList layoutsWithAnyOf(const Program& program, const NameList& names, size_t maxDeclarations)
{
    LayoutList layouts;
    for (PropertyKeyID name : names) {
        if (!hasFewDeclarations(program, name, maxDeclarations))
            return { };
        layouts.unite(LayoutList(program.layoutsWithName(name)));
    }
    return layouts;
}

// The object is expected to have every name it is asked for. Where no layout has them all, the names that an object may lack count no longer.
// Where that leaves no layout either, each access expects a layout that has its own name.
static LayoutList layoutsExpectedFrom(const Program& program, std::span<const NamedUse> uses, size_t maxDeclarations, size_t maxLayouts)
{
    NameList names;
    for (NameDemand weakestDemand : { NameDemand::Stored, NameDemand::Read, NameDemand::Required }) {
        names.clear();
        for (const NamedUse& use : uses) {
            // A name that no layout has says nothing about layouts.
            if (use.demand >= weakestDemand && !program.layoutsWithName(use.key).empty())
                names.add(use.key);
        }
        if (names.isEmpty())
            break;
        bool everyNameIsCommon = std::ranges::all_of(names, [&](PropertyKeyID name) {
            return program.layoutsWithName(name).size() > maxLayouts;
        });
        if (everyNameIsCommon)
            return { };
        LayoutList layouts = allocatedLayoutsWith(program, names.span());
        if (!layouts.isEmpty())
            return layouts;
    }

    for (const NamedUse& use : uses)
        names.add(use.key);
    return layoutsWithAnyOf(program, names, maxDeclarations);
}

static bool resolvesSomeUse(FunctionPassContext& context, std::span<const NamedUse> uses, const LayoutList& layouts)
{
    return std::ranges::any_of(uses, [&](const NamedUse& use) {
        std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, context.function.at(use.access));
        if (!query)
            return false;
        query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
        query->isOnlyCalled = context.function.at(use.access).opcode == Opcode::PropGet && isOnlyCalled(context.analyses, use.access);
        query->receivers.level = KnowledgeLevel::Predicted;
        query->receivers.predicted = layouts;
        return planPropertyAccess({ context.program, &context.inputs.flow }, *query).isResolved();
    });
}

static bool isAllocatedHere(FunctionPassContext& context, ValueID value)
{
    const Function& function = context.function;
    return !proofsOfAllocation(context.program, function, function.at(context.analyses.identityOf(value))).isEmpty();
}

static void predictLayoutsOf(FunctionPassContext& context, std::span<const NamedUse> uses, Vector<ValuePrediction>& predictions)
{
    const Program& program = context.program;
    Function& function = context.function;
    bool isFollowed = std::ranges::any_of(uses, [&](const NamedUse& use) {
        return isExpectedToFindMember(program, function, use);
    });
    if (isFollowed || isAllocatedHere(context, uses[0].receiver))
        return;

    size_t maxDeclarations = static_cast<size_t>(context.limit(LimitID::MaxDeclarationsPredictedFromNames));
    size_t maxLayouts = static_cast<size_t>(context.limit(LimitID::MaxAccessPlanMembers));
    if (uses.size() == 1 && !hasFewDeclarations(program, uses[0].key, maxDeclarations))
        return;
    LayoutList layouts = layoutsExpectedFrom(program, uses, maxDeclarations, maxLayouts);
    if (layouts.isEmpty() || layouts.size() > maxLayouts || !areFew(program, layouts, uses, maxDeclarations))
        return;
    if (!resolvesSomeUse(context, uses, layouts))
        return;

    TypeID expected = context.types.intern(Type::forLayouts(layouts.span(), context.types));
    for (const NamedUse& use : uses) {
        bool isListed = predictions.containsIf([&](const ValuePrediction& prediction) {
            return prediction.value == use.receiver;
        });
        if (!isListed)
            predictions.append(ValuePrediction { use.receiver, expected });
    }
}

// Of an object that nobody could follow, the names it is asked for are all there is to go by. Which layouts have a name is known, and where they are few the object is expected to be one of them.
bool predictLayoutsByNames(FunctionPassContext& context)
{
    Vector<NamedUse> uses = collectNamedUsesOfUnknownObjects(context);
    if (uses.isEmpty())
        return false;

    Vector<ValueID> subjects = findSubjects(context);
    for (NamedUse& use : uses)
        use.subject = subjects[use.receiver.index()];
    std::ranges::stable_sort(uses, { }, &NamedUse::subject);

    Vector<ValuePrediction> predictions;
    for (size_t first = 0; first < uses.size();) {
        size_t next = first;
        while (next < uses.size() && uses[next].subject == uses[first].subject)
            ++next;
        predictLayoutsOf(context, uses.subspan(first, next - first), predictions);
        first = next;
    }
    return predictValues(context, predictions.span());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
