/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTLayoutData.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BaseChainSummary {
    bool everyBaseIsProvenOnce { false };
    bool endsWithoutHeritage { false };
};

} // anonymous namespace

static LayoutID provenBaseOf(const Layout& layout)
{
    return layout.flow.baseState == BaseState::Layout ? layout.flow.base : LayoutID();
}

static bool hasNoHeritage(const Layout& layout)
{
    return layout.flow.baseState == BaseState::None && !layout.has(LayoutFlags::HasHeritage);
}

static Vector<BaseChainSummary> summarizeBaseChains(const Program& program)
{
    enum class WalkState : uint8_t { Unvisited, OnPath, Done };

    size_t layoutCount = program.layouts.size();
    Vector<WalkState> states(FillWith { }, layoutCount, WalkState::Unvisited);
    Vector<BaseChainSummary> summaries(layoutCount);
    Vector<LayoutID> path;
    for (size_t index = 0; index < layoutCount; ++index) {
        for (LayoutID id(index); id && states[id.index()] == WalkState::Unvisited; id = provenBaseOf(program.at(id))) {
            states[id.index()] = WalkState::OnPath;
            path.append(id);
        }
        for (; !path.isEmpty(); path.removeLast()) {
            LayoutID id = path.last();
            const Layout& layout = program.at(id);
            LayoutID base = provenBaseOf(layout);
            BaseChainSummary& summary = summaries[id.index()];
            if (!base) {
                summary.everyBaseIsProvenOnce = true;
                summary.endsWithoutHeritage = hasNoHeritage(layout);
            } else if (states[base.index()] == WalkState::Done) {
                summary.everyBaseIsProvenOnce = isEvaluatedOnce(program, program.at(base)) && summaries[base.index()].everyBaseIsProvenOnce;
                summary.endsWithoutHeritage = summaries[base.index()].endsWithoutHeritage;
            }
            states[id.index()] = WalkState::Done;
        }
    }
    return summaries;
}

static bool isCreatedWithItsHolder(const Program& program, ClassSide side, PropertyKeyID key)
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

static bool capturesNothingButClassEnvironment(const Program& program, const Function& constructor, const Function& function)
{
    if (function.captures.isEmpty())
        return true;
    if (function.captures.size() != 1 || constructor.captures.size() != 1)
        return false;
    TypeID captured = function.captures[0].type;
    return captured && captured == constructor.captures[0].type && program.at(captured).environment;
}

static bool canBeCreatedByClassRecord(const Program& program, const Function& constructor, const ClassMember& member)
{
    if (!member.key || program.isIndexLike(member.key) || isCreatedWithItsHolder(program, member.side, member.key))
        return false;
    // The record names a closure as its function is written. SetFunctionName names the closure of the definition.
    const Function& function = program.at(member.function);
    return function.name && capturesNothingButClassEnvironment(program, constructor, function);
}

static ClassCreationPlan planClass(const Program& program, const Layout& layout, const BaseChainSummary& bases)
{
    const Vector<ClassMember>& members = layout.classFlow.members;
    ClassCreationPlan plan;
    plan.members.fill(MemberCreation::AtDefinition, members.size());
    // No NewClass runs for a constructor function.
    if (layout.has(LayoutFlags::IsConstructorFunction))
        return plan;

    // The keys of a side are in definition order, so the record creates the members before the first one it cannot create.
    const Function& constructor = program.at(layout.constructor);
    std::array<bool, numberOfClassSides> recordCreatesSide;
    recordCreatesSide.fill(true);
    for (size_t index = 0; index < members.size(); ++index) {
        bool& recordCreatesNextMember = recordCreatesSide[static_cast<unsigned>(members[index].side)];
        recordCreatesNextMember = recordCreatesNextMember && canBeCreatedByClassRecord(program, constructor, members[index]);
        if (recordCreatesNextMember)
            plan.members[index] = MemberCreation::ByClassRecord;
    }

    bool recordCreatesPrototypeSide = recordCreatesSide[static_cast<unsigned>(ClassSide::Prototype)];
    bool recordCreatesStaticSide = recordCreatesSide[static_cast<unsigned>(ClassSide::Static)];
    plan.createsMembersLazily = isEvaluatedOnce(program, layout) && recordCreatesPrototypeSide;
    plan.createsPrototypeBeforeClassObject = plan.createsMembersLazily && bases.everyBaseIsProvenOnce && bases.endsWithoutHeritage;
    plan.createsClassObjectOnMutation = plan.createsMembersLazily && bases.everyBaseIsProvenOnce && recordCreatesStaticSide;
    return plan;
}

// The guards describe the first evaluation of the class. A second one breaks them all, whether or not it was foreseen.
static bool isClassWithConstantNames(const Layout& layout)
{
    if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived() || layout.has(LayoutFlags::IsConstructorFunction))
        return false;
    return std::ranges::all_of(layout.classFlow.members, [](const ClassMember& member) {
        return !!member.key;
    });
}

// Nothing can come between such a class and what the program text says of it but a change to an object that exists, and that change is reported.
static bool definitionMakesWhatRecordsSay(const Program& program, LayoutID id)
{
    LayoutID current = id;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (!isClassWithConstantNames(layout))
            return false;
        if (hasNoHeritage(layout))
            return true;
        switch (layout.flow.baseState) {
        case BaseState::BuiltinClass:
            return true;
        case BaseState::Unknown:
            if (!layout.base)
                return true;
            break;
        case BaseState::Layout:
            break;
        case BaseState::None:
            return false;
        }
        current = expectedBaseOf(layout);
    }
    return false;
}

Vector<ClassCreationPlan> planClassCreation(const SlotPlanContext& context)
{
    const Program& program = context.program;
    Vector<BaseChainSummary> baseChains = summarizeBaseChains(program);
    Vector<ClassCreationPlan> plans(program.layouts.size());
    uint64_t membersCreatedByClassRecord = 0;
    uint64_t membersCreatedAtDefinition = 0;
    uint64_t classesCreatedLazily = 0;
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !layout.constructor)
            continue;
        ClassCreationPlan& plan = plans[index];
        plan = planClass(program, layout, baseChains[index]);
        plan.guardsHoldWhileClear = definitionMakesWhatRecordsSay(program, LayoutID(index));
        uint64_t byClassRecord = std::ranges::count(plan.members, MemberCreation::ByClassRecord);
        membersCreatedByClassRecord += byClassRecord;
        membersCreatedAtDefinition += plan.members.size() - byClassRecord;
        classesCreatedLazily += plan.createsMembersLazily;
    }

    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::ClassMembersCreatedByClassRecord, membersCreatedByClassRecord);
    statistics.count(StatisticID::ClassMembersCreatedAtDefinition, membersCreatedAtDefinition);
    statistics.count(StatisticID::ClassesCreatedLazily, classesCreatedLazily);
    return plans;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
