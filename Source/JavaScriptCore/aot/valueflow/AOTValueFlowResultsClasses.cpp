/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

// After one of these a holder need not hold under a name what the class has defined there.
static constexpr OptionSet<ObjectMutation> mutationsOfMembers {
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::DeleteField,
    ObjectMutation::RedefineProperty,
    ObjectMutation::AddProperty,
    ObjectMutation::StoreAfterCreation,
};

namespace {

struct Definition {
    uint32_t index { 0 };
    ClassMember member;
};

// What the operations of the program say about the class of a layout.
struct ClassDefinition {
    // The operations that create the class object: NewClass, or NewClosure for a constructor function.
    uint32_t numberOfCreations { 0 };
    bool theCreationRunsAtMostOnce { false };
    // Something about the class is not listed here.
    bool isFollowed { true };
    bool mayHaveUnknownName[2] { false, false };
    Vector<Definition> definitions;
};

} // anonymous namespace

static auto memberOrderOf(const ClassMember& member)
{
    return std::tuple { member.side, member.key, member.kind, member.function };
}

static FunctionID functionOfClosure(const Function& function, ValueID closure)
{
    const Value& definition = function.at(sameObjectThroughCasts(function, closure));
    return definition.opcode == Opcode::NewClosure ? definition.function() : FunctionID();
}

static void noteOperation(const Program& program, const Function& function, const Value& value, bool runsAtMostOnce, Vector<ClassDefinition>& classes)
{
    auto noteCreation = [&](LayoutID layout) {
        ClassDefinition& definition = classes[layout.index()];
        ++definition.numberOfCreations;
        definition.theCreationRunsAtMostOnce = runsAtMostOnce;
    };
    switch (value.opcode) {
    case Opcode::NewClass:
        noteCreation(value.layout());
        return;
    case Opcode::NewClosure:
        if (LayoutID layout = program.at(value.function()).constructedClass; layout && program.at(layout).has(LayoutFlags::IsConstructorFunction))
            noteCreation(layout);
        return;
    case Opcode::DefineMethod: {
        ClassDefinition& definition = classes[value.layout().index()];
        FunctionID defined = functionOfClosure(function, value.functionOperand());
        if (!defined) {
            definition.isFollowed = false;
            return;
        }
        if (!value.key())
            definition.mayHaveUnknownName[static_cast<unsigned>(value.side())] = true;
        definition.definitions.append({ value.index(), ClassMember { .key = value.key(), .side = value.side(), .kind = value.methodKind(), .function = defined } });
        return;
    }
    default:
        return;
    }
}

static Vector<ClassDefinition> collectClassDefinitions(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<ClassDefinition> classes(program.layouts.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (isUnsolvedCode(inputs, tables, FunctionID(index))) {
            for (const Value& value : function.values) {
                if (value.opcode == Opcode::NewClass || value.opcode == Opcode::DefineMethod)
                    classes[value.layout().index()].isFollowed = false;
                else if (value.opcode == Opcode::NewClosure && program.at(value.function()).constructedClass)
                    classes[program.at(value.function()).constructedClass.index()].isFollowed = false;
            }
            continue;
        }
        bool definesClass = inputs.isReached(FunctionID(index)) && std::ranges::any_of(function.values, [](const Value& value) {
            return value.opcode == Opcode::NewClass || value.opcode == Opcode::NewClosure || value.opcode == Opcode::DefineMethod;
        });
        if (!definesClass)
            continue;

        const FunctionFlow& flow = tables.functions.functions[index];
        CFG cfg(function);
        std::optional<BlockCycles> cycles;
        if (flow.executions == ExecutionCount::AtMostOnce)
            cycles.emplace(cfg);
        for (BlockID block : cfg.reversePostOrder()) {
            bool runsAtMostOnce = cycles && !cycles->isInCycle(block);
            for (ValueID id : function.at(block).values)
                noteOperation(program, function, function.at(id), runsAtMostOnce, classes);
        }
    }
    // The host creates the function object of a declaration once, with its module.
    for (const Variable& variable : program.variables) {
        LayoutID layout = variable.function ? program.at(variable.function).constructedClass : LayoutID();
        if (!layout || !program.at(layout).has(LayoutFlags::IsConstructorFunction))
            continue;
        ClassDefinition& definition = classes[layout.index()];
        ++definition.numberOfCreations;
        definition.theCreationRunsAtMostOnce = true;
    }
    return classes;
}

static bool hasOneDefinition(std::span<const ClassMember> membersUnderOneKey)
{
    bool hasMethod = membersUnderOneKey.front().kind == MethodKind::Method;
    bool hasAccessor = membersUnderOneKey.back().kind != MethodKind::Method;
    if (hasMethod && hasAccessor)
        return false;
    return std::ranges::adjacent_find(membersUnderOneKey, { }, &ClassMember::kind) == membersUnderOneKey.end();
}

// Copies of a definition that disagree share an index. Only the copy that runs knows what it defines.
static Vector<ClassMember> listMembersInDefinitionOrder(Vector<Definition>&& definitions, std::span<const ClassMember> namedMembers)
{
    auto orderOf = [](const Definition& definition) {
        return std::tuple { definition.index, memberOrderOf(definition.member) };
    };
    std::ranges::sort(definitions, { }, orderOf);
    definitions.shrink(std::ranges::unique(definitions, { }, orderOf).begin() - definitions.begin());

    Vector<ClassMember> members;
    members.reserveInitialCapacity(definitions.size());
    for (size_t position = 0; position < definitions.size(); ++position) {
        ClassMember member = definitions[position].member;
        bool sharesIndex = (position && definitions[position - 1].index == definitions[position].index)
            || (position + 1 < definitions.size() && definitions[position + 1].index == definitions[position].index);
        if (sharesIndex)
            member.key = { };
        if (member.key) {
            auto named = std::ranges::lower_bound(namedMembers, memberOrderOf(member), { }, memberOrderOf);
            if (named != namedMembers.end() && memberOrderOf(*named) == memberOrderOf(member))
                member = *named;
        }
        members.append(member);
    }
    return members;
}

static ClassFlow computeClassFlow(const ValueFlowResultInputs& inputs, LayoutID id, ClassDefinition&& definition)
{
    const Program& program = inputs.program;
    const Layout& layout = program.at(id);
    BuildContext& build = inputs.build;
    OptionSet<ObjectMutation> mutationsOfHolder[2];
    mutationsOfHolder[static_cast<unsigned>(ClassSide::Prototype)] = markedMutationsOf(inputs, inputs.universe().prototypeOf(id));
    mutationsOfHolder[static_cast<unsigned>(ClassSide::Static)] = markedMutationsOf(inputs, inputs.universe().closureOf(layout.constructor));
    auto mutationsOn = [&](ClassSide side) {
        return mutationsOfHolder[static_cast<unsigned>(side)];
    };
    definition.mayHaveUnknownName[static_cast<unsigned>(ClassSide::Static)] |= std::ranges::any_of(layout.staticFields, [](const StaticField& field) {
        return !field.key;
    });

    ClassFlow flow;
    if (build.isEnabled(FlowResultKind::ClassEvaluations, id.index()) && (!definition.numberOfCreations || (definition.numberOfCreations == 1 && definition.theCreationRunsAtMostOnce)))
        flow.evaluations = ExecutionCount::AtMostOnce;

    bool followsMutations = build.isEnabled(FlowResultKind::HolderMutations, id.index());
    Vector<ClassMember> namedMembers;
    for (const Definition& entry : definition.definitions) {
        if (entry.member.key)
            namedMembers.append(entry.member);
    }
    std::ranges::sort(namedMembers, { }, memberOrderOf);
    namedMembers.shrink(std::ranges::unique(namedMembers, { }, memberOrderOf).begin() - namedMembers.begin());
    for (size_t first = 0; first < namedMembers.size();) {
        size_t end = first + 1;
        while (end < namedMembers.size() && namedMembers[end].side == namedMembers[first].side && namedMembers[end].key == namedMembers[first].key)
            ++end;
        std::span<ClassMember> group = namedMembers.mutableSpan().subspan(first, end - first);
        ClassSide side = group.front().side;
        bool holdsWhatWasDefined = followsMutations && !definition.mayHaveUnknownName[static_cast<unsigned>(side)] && !mutationsOn(side).containsAny(mutationsOfMembers);
        for (ClassMember& member : group) {
            if (holdsWhatWasDefined && hasOneDefinition(group))
                member.mutability = MemberMutability::Fixed;
            // A store through an object below passes a method, unless the holder is frozen.
            member.isWritableData = holdsWhatWasDefined && member.kind == MethodKind::Method && group.size() == 1 && !mutationsOn(side).contains(ObjectMutation::ChangeIntegrity);
        }
        first = end;
    }

    if (followsMutations) {
        PropertyKeyID constructorKey = program.keyOf(BuiltinNames::constructor);
        bool definesConstructorName = std::ranges::any_of(namedMembers, [&](const ClassMember& member) {
            return member.side == ClassSide::Prototype && member.key == constructorKey;
        });
        if (!mutationsOn(ClassSide::Prototype).containsAny({ ObjectMutation::ChangePrototype, ObjectMutation::ReachUnknownJavaScript }))
            flow.prototypeLink = MemberMutability::Fixed;
        if (!mutationsOn(ClassSide::Static).containsAny({ ObjectMutation::ChangePrototype, ObjectMutation::ReachUnknownJavaScript }))
            flow.staticLink = MemberMutability::Fixed;
        if (!definesConstructorName && !definition.mayHaveUnknownName[static_cast<unsigned>(ClassSide::Prototype)] && !mutationsOn(ClassSide::Prototype).containsAny(mutationsOfMembers))
            flow.constructorProperty = MemberMutability::Fixed;
        // No code can change the property of a class.
        flow.prototypeProperty = MemberMutability::Fixed;
    }
    if (build.isEnabled(FlowResultKind::OwnNames, id.index()) && followsMutations) {
        flow.prototypeNamesAreComplete = !definition.mayHaveUnknownName[static_cast<unsigned>(ClassSide::Prototype)] && !mutationsOn(ClassSide::Prototype).containsAny(mutationsOfMembers);
        flow.staticNamesAreComplete = !definition.mayHaveUnknownName[static_cast<unsigned>(ClassSide::Static)] && !mutationsOn(ClassSide::Static).containsAny(mutationsOfMembers);
    }
    flow.members = listMembersInDefinitionOrder(WTF::move(definition.definitions), namedMembers.span());
    flow.namedMembers = WTF::move(namedMembers);
    return flow;
}

void computeClassResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    Vector<ClassDefinition> classes = collectClassDefinitions(inputs, tables);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!layout.constructor || !classes[index].isFollowed)
            continue;
        // The members of a constructor function are assigned, and nobody follows the order of the assignments: only how often the function object is created is said.
        if (layout.has(LayoutFlags::IsConstructorFunction)) {
            const ClassDefinition& definition = classes[index];
            bool isCreatedAtMostOnce = !definition.numberOfCreations || (definition.numberOfCreations == 1 && definition.theCreationRunsAtMostOnce);
            if (isCreatedAtMostOnce && inputs.build.isEnabled(FlowResultKind::ClassEvaluations, index))
                application.layouts[index].classFlow.evaluations = ExecutionCount::AtMostOnce;
            continue;
        }
        application.layouts[index].classFlow = computeClassFlow(inputs, LayoutID(index), WTF::move(classes[index]));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
