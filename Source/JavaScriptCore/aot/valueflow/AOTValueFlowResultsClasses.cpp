/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueIdentity.h"
#include <tuple>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> mutationsOfMembers { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation };
static constexpr unsigned prototypeSide = static_cast<unsigned>(ClassSide::Prototype);
static constexpr unsigned staticSide = static_cast<unsigned>(ClassSide::Static);

namespace {

struct ClassMemberDefinition {
    uint32_t index { 0 };
    ClassMember member;
};

struct ClassDefinition {
    bool isCreatedAtMostOnce() const { return !numberOfCreations || (numberOfCreations == 1 && theCreationRunsAtMostOnce); }

    uint32_t numberOfCreations { 0 };
    bool theCreationRunsAtMostOnce { false };
    bool isFollowed { true };
    bool mayHaveUnknownName[2] { false, false };
    ValueSetID heritage { ValueSets::nothing };
    Vector<ClassMemberDefinition> definitions;
    Vector<FunctionID> functionsHandedOnAtDefinition;
};

struct BaseOfClass {
    BaseState state { BaseState::Unknown };
    LayoutID base;
    BuiltinClassID baseClass;

    friend bool operator==(const BaseOfClass&, const BaseOfClass&) = default;
};

struct AssignedValue {
    LayoutID classLayout;
    PropertyKeyID key;
    ValueSetID value;
    MethodKind kind { MethodKind::Method };
    bool isWrittenForOtherObjectsToo { false };

    auto order() const { return std::tuple { classLayout, key }; }
};

struct WritesToPrototypeObjects {
    static constexpr uint8_t isWrittenUnderOtherNamesFlag = 1;
    static constexpr uint8_t hasOtherPrototypeObjectFlag = 2;

    std::span<const AssignedValue> of(LayoutID classLayout) const LIFETIME_BOUND { return std::span(std::ranges::equal_range(underNames, classLayout, { }, &AssignedValue::classLayout)); }
    std::span<const AssignedValue> under(LayoutID classLayout, PropertyKeyID key) const LIFETIME_BOUND { return std::span(std::ranges::equal_range(underNames, std::tuple { classLayout, key }, { }, &AssignedValue::order)); }
    bool isWrittenUnderOtherNames(LayoutID classLayout) const { return isAnyObjectWrittenUnderOtherNames || (flagsOfClass[classLayout.index()] & isWrittenUnderOtherNamesFlag); }
    bool mayHaveOtherPrototypeObject(LayoutID classLayout) const { return isAnyObjectWrittenUnderOtherNames || (flagsOfClass[classLayout.index()] & hasOtherPrototypeObjectFlag); }

    Vector<AssignedValue> underNames;
    Vector<uint8_t> flagsOfClass;
    bool isAnyObjectWrittenUnderOtherNames { false };
};

} // anonymous namespace

static auto memberOrderOf(const ClassMember& member)
{
    return std::tuple { member.side, member.key, member.kind, member.function };
}

static ValueSetID valueOf(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, ValueSetMaker& maker)
{
    ValueSetID value = operand.constant ? operand.constant : ValueSets::nothing;
    if (operand.node)
        value = maker.unite(value, inputs.solver().valueOf(operand.node));
    return ValueFlowRuleContext::throughMaskOf(operand, value, inputs.solver(), maker);
}

static bool everyUseThroughCasts(const Function& function, const UseLists& useLists, ValueID value, bool mayBeUnused, NOESCAPE const Invocable<bool(const Use&, Opcode, OperandUses)> auto& admits)
{
    Vector<ValueID, 4> worklist { value };
    while (!worklist.isEmpty()) {
        std::span<const Use> uses = useLists.usesOf(worklist.takeLast());
        if (uses.empty() && !mayBeUnused)
            return false;
        for (const Use& use : uses) {
            if (use.kind != Use::Kind::Operand)
                return false;
            Opcode user = function.at(use.user).opcode;
            OperandUses operandUses = operandUsesOf(user, use.operandIndex);
            if (isCast(user) && operandUses.contains(OperandUse::Returns))
                worklist.append(use.user);
            else if (!admits(use, user, operandUses))
                return false;
        }
    }
    return true;
}

static Vector<ClassDefinition> collectClassDefinitions(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, ValueSetMaker& maker)
{
    const Program& program = inputs.program;
    Vector<ClassDefinition> classes(program.layouts.size());
    auto classConstructedByClosure = [&](const Value& value) {
        return value.opcode == Opcode::NewClosure ? program.at(value.function()).constructedClass : LayoutID();
    };
    Vector<uint32_t> functionsToLookAt = findIndicesInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::ClassDefinition, ValueFlowNotedOperation::NewClosure })))
            return false;
        if (!isUnsolvedCode(inputs, tables, FunctionID(index)) && !inputs.isReached(FunctionID(index)))
            return false;
        return std::ranges::any_of(program.functions[index].values, [&](const Value& value) {
            return value.opcode == Opcode::NewClass || value.opcode == Opcode::DefineMethod || classConstructedByClosure(value);
        });
    });
    for (uint32_t index : functionsToLookAt) {
        const Function& function = program.functions[index];
        if (isUnsolvedCode(inputs, tables, FunctionID(index))) {
            for (const Value& value : function.values) {
                if (LayoutID layout = value.opcode == Opcode::NewClass || value.opcode == Opcode::DefineMethod ? value.layout() : classConstructedByClosure(value))
                    classes[layout.index()].isFollowed = false;
            }
            continue;
        }

        CFG cfg(function);
        std::optional<UseLists> useLists;
        std::optional<BlockCycles> cycles;
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                LayoutID created = value.opcode == Opcode::NewClass ? value.layout() : classConstructedByClosure(value);
                if (created && (value.opcode == Opcode::NewClass || program.at(created).has(LayoutFlags::IsConstructorFunction))) {
                    if (!cycles && tables.functions.functions[index].executions == ExecutionCount::AtMostOnce)
                        cycles.emplace(cfg);
                    ++classes[created.index()].numberOfCreations;
                    classes[created.index()].theCreationRunsAtMostOnce = cycles && !cycles->isInCycle(block);
                }
                if (value.opcode != Opcode::DefineMethod)
                    continue;
                ClassDefinition& definition = classes[value.layout().index()];
                ValueID closure = sameObjectThroughCasts(function, value.functionOperand());
                if (function.at(closure).opcode != Opcode::NewClosure) {
                    definition.isFollowed = false;
                    continue;
                }
                FunctionID defined = function.at(closure).function();
                if (!value.key())
                    definition.mayHaveUnknownName[static_cast<unsigned>(value.side())] = true;
                if (!useLists)
                    useLists.emplace(function, cfg);
                bool goesOnlyIntoDefinitions = everyUseThroughCasts(function, *useLists, closure, true, [](const Use& use, Opcode user, OperandUses operandUses) {
                    if (user == Opcode::DefineMethod && use.operandIndex == OperandIndex::DefineMethod::function)
                        return true;
                    return operandUses.containsOnly(OperandUse::Inspects) && protocolsOf(user, use.operandIndex).isEmpty();
                });
                if (!goesOnlyIntoDefinitions)
                    definition.functionsHandedOnAtDefinition.append(defined);
                definition.definitions.append({ value.index(), ClassMember { .key = value.key(), .side = value.side(), .kind = value.methodKind(), .function = defined } });
            }
        }
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::NewClass || !function.at(statement.site).heritageOperand())
                continue;
            ValueSetID& heritage = classes[function.at(statement.site).layout().index()].heritage;
            heritage = maker.unite(heritage, valueOf(inputs, inputs.bodies().operandsOf(statement)[OperandIndex::NewClass::heritage], maker));
        }
    }
    for (const Variable& variable : program.variables) {
        LayoutID layout = variable.function ? program.at(variable.function).constructedClass : LayoutID();
        if (!layout || !program.at(layout).has(LayoutFlags::IsConstructorFunction))
            continue;
        ++classes[layout.index()].numberOfCreations;
        classes[layout.index()].theCreationRunsAtMostOnce = true;
    }
    for (ClassDefinition& definition : classes) {
        AOT::sort(definition.functionsHandedOnAtDefinition);
        removeRepeatedElements(definition.functionsHandedOnAtDefinition);
    }
    return classes;
}

static BaseOfClass computeBase(const ValueFlowResultInputs& inputs, LayoutID id, const ClassDefinition& definition)
{
    const Layout& layout = inputs.program.at(id);
    const BuiltinTable& builtins = inputs.program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    ValueSetID heritage = definition.heritage;
    if (layout.isDerived())
        return { BaseState::Layout, layout.flow.base, { } };
    if (!layout.has(LayoutFlags::HasHeritage))
        return { BaseState::None, { }, { } };
    if (!definition.isFollowed || !inputs.build.isEnabled(FlowResultKind::ClassBases, id.index()) || ValueSets::isComplement(heritage) || sets.isEmpty(heritage))
        return { };

    std::optional<BaseOfClass> joined;
    sets.forEachMember(heritage, [&](ValueFlowObjectID member) {
        if (!universe.isObject(member))
            return;
        BaseOfClass base;
        if (FunctionID function = universe.closureFunction(member)) {
            if (LayoutID constructed = inputs.program.at(function).constructedClass)
                base = { BaseState::Layout, constructed, { } };
        } else if (universe.kindOf(member) == ValueFlowObjectKind::Builtin) {
            BuiltinID constructor = builtins.canonicalID(universe.builtinOf(member));
            BuiltinClassID instanceClass = builtins.at(constructor).instanceClass;
            BuiltinID classConstructor = instanceClass ? builtins.at(instanceClass).constructor : BuiltinID();
            if (classConstructor && builtins.canonicalID(classConstructor) == constructor)
                base = { BaseState::BuiltinClass, { }, instanceClass };
        }
        joined = !joined || *joined == base ? base : BaseOfClass { };
    });
    if (sets.contains(heritage, ValueFlowUniverse::primitive(PrimitiveSet::Null)))
        return joined ? BaseOfClass { } : BaseOfClass { BaseState::None, { }, { } };
    return joined.value_or(BaseOfClass { });
}

static LayoutID provenBaseOf(const BaseOfClass& base)
{
    return base.state == BaseState::Layout ? base.base : LayoutID();
}

static uint64_t countFieldsWithInheritedOnes(const Program& program, std::span<const BaseOfClass> bases, LayoutID id)
{
    uint64_t numberOfUnnamedFields = 0;
    Vector<PropertyKeyID> keys;
    for (LayoutID current = id; current; current = provenBaseOf(bases[current.index()])) {
        for (const Field& field : program.at(current).fields) {
            if (field.key)
                keys.append(field.key);
            else
                ++numberOfUnnamedFields;
        }
    }
    AOT::sort(keys);
    removeRepeatedElements(keys);
    return numberOfUnnamedFields + keys.size();
}

static void invalidateBasesWhoseFieldsHaveNoRoom(const ValueFlowResultInputs& inputs, Vector<BaseOfClass>& bases)
{
    const Program& program = inputs.program;
    uint64_t maxFields = inputs.build.limit(LimitID::FieldsInKeyOrder);
    Vector<uint64_t> fieldCounts(FillWith { }, bases.size(), static_cast<uint64_t>(0));
    BitVector isCounted(bases.size());
    Vector<LayoutID, 16> path;
    for (size_t index = 0; index < bases.size(); ++index) {
        for (LayoutID current(static_cast<uint32_t>(index)); current && !isCounted.quickSet(current.index()); current = provenBaseOf(bases[current.index()]))
            path.append(current);
        while (!path.isEmpty()) {
            LayoutID id = path.takeLast();
            uint64_t& count = fieldCounts[id.index()];
            count = program.at(id).fields.size();
            LayoutID base = provenBaseOf(bases[id.index()]);
            if (!base)
                continue;
            count += fieldCounts[base.index()];
            if (count <= maxFields || program.at(id).isDerived())
                continue;
            count = countFieldsWithInheritedOnes(program, bases.span(), id);
            if (!inputs.build.exceeds(LimitID::FieldsInKeyOrder, count, Subject { .layout = id }))
                continue;
            bases[id.index()] = BaseOfClass { };
            count = program.at(id).fields.size();
        }
    }
}

static void computeBaseResults(const ValueFlowResultInputs& inputs, std::span<const ClassDefinition> classes, FlowApplication& application)
{
    Vector<BaseOfClass> bases(classes.size(), [&](size_t index) {
        return computeBase(inputs, LayoutID(static_cast<uint32_t>(index)), classes[index]);
    });
    BitVector liesOnCycle(bases.size());
    for (size_t index = 0; index < bases.size(); ++index) {
        LayoutID above = provenBaseOf(bases[index]);
        for (size_t steps = 0; above && above.index() != index && steps < bases.size(); ++steps)
            above = provenBaseOf(bases[above.index()]);
        liesOnCycle.quickSet(index, !!above && above.index() == index);
    }
    for (size_t index : liesOnCycle)
        bases[index] = BaseOfClass { };
    invalidateBasesWhoseFieldsHaveNoRoom(inputs, bases);
    for (size_t index = 0; index < bases.size(); ++index) {
        LayoutFlow& flow = application.layouts[index].flow;
        flow.baseState = bases[index].state;
        flow.base = bases[index].base;
        flow.baseClass = bases[index].baseClass;
    }
}

static ValueSetID findClosuresTakenOutOfMembers(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, ValueSetID closuresOfMembers)
{
    struct Carrier {
        uint32_t indexOfMadeNodes;
        ValueSetID closures;
    };
    struct UsesInFunction {
        FunctionID function;
        std::optional<CFG> cfg;
        std::optional<UseLists> useLists;
    };
    constexpr ValueFlowKindsOfMadeNodes kindsOfCarriers = kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup });
    const ValueFlowSolver& solver = inputs.solver();
    const ParallelLoops& loops = inputs.loops();

    uint32_t firstMadeNode = solver.input().numberOfNodes;
    ValueSetMakers makers(inputs);
    Vector<Carrier> carriers = collectWithMakers<Carrier>(inputs, makers, solver.graph().numberOfNodes() - firstMadeNode, [&](unsigned position, ValueSetMaker& makerOfThread, Vector<Carrier>& found) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(firstMadeNode + position));
        ValueSetID value = solver.valueOf(ValueFlowNodeID(firstMadeNode + position));
        if ((kindsOfCarriers >> static_cast<unsigned>(key->kind) & 1) && inputs.sets().meet(value, closuresOfMembers))
            found.append({ solver.indexOfMadeNodes(*key), makerOfThread.intersect(value, closuresOfMembers) });
    });
    if (carriers.isEmpty())
        return ValueSets::nothing;
    parallelSort(loops, carriers.mutableSpan(), [](const Carrier& a, const Carrier& b) {
        return a.indexOfMadeNodes < b.indexOfMadeNodes;
    });
    BitVector isCarrier(solver.numberOfRecordsOfMadeNodes());
    for (const Carrier& carrier : carriers)
        isCarrier.quickSet(carrier.indexOfMadeNodes);
    Vector<uint8_t> isTakenFrom(FillWith { }, carriers.size(), static_cast<uint8_t>(0));
    Vector<UsesInFunction> usesOfWorker(loops.width());

    auto leavesClosuresInPlace = [&](UsesInFunction& uses, ValueFlowStatementID id, const ValueFlowStatement& statement, ValueFlowNameKind read) {
        switch (statement.kind) {
        case ValueFlowStatementKind::Lookup: {
            ValueFlowNameKind own = solver.keyOf(id).name().kind;
            return shapeOf(own).isValueOrAccessor() || own == ValueFlowNameKind::PrototypeLink;
        }
        case ValueFlowStatementKind::Operation: {
            if (!isHeapOperation(statement.opcode))
                return false;
            if (shapeOf(read).isAccessor())
                return true;
            const Function& function = inputs.program.at(statement.function);
            if ((statement.opcode != Opcode::PropGet && statement.opcode != Opcode::ElemGet && statement.opcode != Opcode::SuperGet) || function.role == FunctionRole::Model)
                return false;
            if (uses.function != statement.function) {
                uses.function = statement.function;
                uses.useLists.reset();
                uses.cfg.emplace(function);
                uses.useLists.emplace(function, *uses.cfg);
            }
            return everyUseThroughCasts(function, *uses.useLists, statement.site, false, [](const Use&, Opcode, OperandUses operandUses) {
                return operandUses.contains(OperandUse::Calls) && !operandUses.containsAny({ OperandUse::Retains, OperandUse::Returns });
            });
        }
        case ValueFlowStatementKind::CallGroup:
        case ValueFlowStatementKind::Behavior:
        case ValueFlowStatementKind::Escape:
        case ValueFlowStatementKind::ModuleEvaluation:
        case ValueFlowStatementKind::Coroutine:
        case ValueFlowStatementKind::HostRoots:
        case ValueFlowStatementKind::UsedBuiltins:
        case ValueFlowStatementKind::UnseenCode:
        case ValueFlowStatementKind::ModulesLoadedByName:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto noteStatement = [&](unsigned worker, ValueFlowStatementID id, const ValueFlowStatement& statement) {
        inputs.forEachIndexOfMadeNodesAskedForBy(id, kindsOfCarriers, false, [&](uint32_t index) {
            if (!isCarrier.quickGet(index) || solver.statementOfMadeNodesAt(index) == id || isLookupOfCalleeByCall(inputs, statement, solver.keyOfMadeNodesAt(index)))
                return;
            AtomicRef isTaken(isTakenFrom[std::ranges::lower_bound(carriers, index, { }, &Carrier::indexOfMadeNodes) - carriers.begin()]);
            if (!isTaken.load(std::memory_order_relaxed) && !leavesClosuresInPlace(usesOfWorker[worker], id, statement, solver.keyOfMadeNodesAt(index).name().kind))
                isTaken.store(1, std::memory_order_relaxed);
        });
    };

    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    forEachIndexInParallel(loops, inputs.bodies().numberOfBodies(), [&](unsigned body, unsigned worker) {
        if (body != inputs.bodies().hostBody() && !inputs.isReached(FunctionID(body)))
            return;
        for (uint32_t index = ranges[body].statement; index < ranges[body + 1].statement; ++index)
            noteStatement(worker, ValueFlowStatementID(index), statements[index]);
    });
    constexpr uint32_t statementsPerTask = 1024;
    uint32_t numberOfStatements = solver.numberOfStatements();
    forEachIndexInParallel(loops, (numberOfStatements - statements.size() + statementsPerTask - 1) / statementsPerTask, [&](unsigned task, unsigned worker) {
        uint32_t first = statements.size() + task * statementsPerTask;
        for (uint32_t index = first; index < std::min(numberOfStatements, first + statementsPerTask); ++index) {
            if (!solver.isAtRest(ValueFlowStatementID(index)))
                noteStatement(worker, ValueFlowStatementID(index), solver.statementAt(ValueFlowStatementID(index)));
        }
    });

    ValueSetID taken = ValueSets::nothing;
    for (size_t index = 0; index < carriers.size(); ++index) {
        if (isTakenFrom[index])
            taken = maker.unite(taken, carriers[index].closures);
    }
    return taken;
}

static Vector<ClassMember> listMembersInDefinitionOrder(Vector<ClassMemberDefinition>&& definitions, std::span<const ClassMember> namedMembers)
{
    auto orderOf = [](const ClassMemberDefinition& definition) {
        return std::tuple { definition.index, memberOrderOf(definition.member) };
    };
    AOT::sort(definitions, { }, orderOf);
    definitions.shrink(std::ranges::unique(definitions, { }, orderOf).begin() - definitions.begin());

    return Vector<ClassMember>(definitions.size(), [&](size_t position) {
        ClassMember member = definitions[position].member;
        if ((position && definitions[position - 1].index == definitions[position].index) || (position + 1 < definitions.size() && definitions[position + 1].index == definitions[position].index))
            member.key = { };
        auto named = std::ranges::lower_bound(namedMembers, memberOrderOf(member), { }, memberOrderOf);
        return member.key && named != namedMembers.end() && memberOrderOf(*named) == memberOrderOf(member) ? *named : member;
    });
}

static void listWhatIsStoredOverMembers(const ValueFlowResultInputs& inputs, std::span<const AssignedValue> written, Vector<ClassMember>& namedMembers)
{
    PropertyKeyID constructorKey = inputs.program.keyOf(BuiltinNames::constructor);
    for (const AssignedValue& entry : written) {
        if (entry.key == constructorKey)
            continue;
        auto add = [&](FunctionID function) {
            ClassMember member { .key = entry.key, .side = ClassSide::Prototype, .kind = function ? entry.kind : MethodKind::Method, .function = function };
            if (!std::ranges::contains(namedMembers, memberOrderOf(member), memberOrderOf))
                namedMembers.append(member);
        };
        if (ValueSets::isComplement(entry.value)) {
            add({ });
            continue;
        }
        inputs.sets().forEachMember(entry.value, [&](ValueFlowObjectID member) {
            add(inputs.universe().closureFunction(member));
        });
    }
    AOT::sort(namedMembers, { }, memberOrderOf);
}

static ClassFlow computeClassFlow(const ValueFlowResultInputs& inputs, LayoutID id, ClassDefinition&& definition, ValueSetID closuresTakenOutOfMembers, const WritesToPrototypeObjects& writes)
{
    const Program& program = inputs.program;
    const Layout& layout = program.at(id);
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    BuildContext& build = inputs.build;
    ValueFlowObjectID prototype = universe.prototypeOf(id);
    PropertyKeyID lengthKey = program.keyOf(BuiltinNames::length);
    PropertyKeyID constructorKey = program.keyOf(BuiltinNames::constructor);
    OptionSet<ObjectMutation> mutations[2];
    mutations[prototypeSide] = markedMutationsOf(inputs, prototype);
    mutations[staticSide] = markedMutationsOf(inputs, universe.closureOf(layout.constructor));
    bool sharesNameWithStaticField = std::ranges::any_of(definition.definitions, [&](const ClassMemberDefinition& entry) {
        return entry.member.side == ClassSide::Static && (!entry.member.key || std::ranges::contains(layout.staticFields, entry.member.key, &StaticField::key));
    });
    if (!sharesNameWithStaticField && inputs.variablesOnObjects->classObjectsStoredUnderStaticFieldsAlone.quickGet(id.index()))
        mutations[staticSide].remove({ ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation });
    definition.mayHaveUnknownName[staticSide] |= std::ranges::contains(layout.staticFields, PropertyKeyID(), &StaticField::key);

    ClassFlow flow;
    if (build.isEnabled(FlowResultKind::ClassEvaluations, id.index()) && definition.isCreatedAtMostOnce())
        flow.evaluations = ExecutionCount::AtMostOnce;

    bool followsMutations = build.isEnabled(FlowResultKind::HolderMutations, id.index());
    bool followsUses = build.isEnabled(FlowResultKind::ObjectUses, id.index());
    bool isOnlyStoredToUnderNames = mutations[prototypeSide].containsAny(mutationsOfMembers)
        && !mutations[prototypeSide].containsAny({ ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty })
        && !writes.isWrittenUnderOtherNames(id) && !inputs.hasEscaped(prototype) && !inputs.isLost(prototype);
    bool listsEveryStoredName = followsMutations && isOnlyStoredToUnderNames;
    auto keepsMembersUnder = [&](ClassSide side, std::span<const ClassMember> membersUnderOneKey, PropertyKeyID key) {
        if (!mutations[static_cast<unsigned>(side)].containsAny(mutationsOfMembers))
            return true;
        if (side != ClassSide::Prototype || !isOnlyStoredToUnderNames || key == lengthKey)
            return false;
        return std::ranges::all_of(writes.under(id, key), [&](const AssignedValue& written) {
            return !ValueSets::isComplement(written.value) && !sets.anyMember(written.value, [&](ValueFlowObjectID member) {
                FunctionID function = universe.closureFunction(member);
                return !function || std::ranges::none_of(membersUnderOneKey, [&](const ClassMember& defined) {
                    return defined.kind == written.kind && defined.function == function;
                });
            });
        });
    };
    auto isOnlyCalled = [&](FunctionID function) {
        ValueFlowObjectID closure = universe.closureOf(function);
        return !program.at(function).has(FunctionFlags::TakesCallee) && !std::ranges::binary_search(definition.functionsHandedOnAtDefinition, function) && !inputs.hasEscaped(closure) && !inputs.isLost(closure)
            && !inputs.isHeldByNativeCode(closure) && markedMutationsOf(inputs, closure).isEmpty() && !sets.contains(closuresTakenOutOfMembers, closure);
    };

    Vector<ClassMember> namedMembers;
    for (const ClassMemberDefinition& entry : definition.definitions) {
        if (entry.member.key)
            namedMembers.append(entry.member);
    }
    AOT::sort(namedMembers, { }, memberOrderOf);
    namedMembers.shrink(std::ranges::unique(namedMembers, { }, memberOrderOf).begin() - namedMembers.begin());
    if (listsEveryStoredName)
        listWhatIsStoredOverMembers(inputs, writes.of(id), namedMembers);
    for (size_t first = 0, end = 0; first < namedMembers.size(); first = end) {
        while (end < namedMembers.size() && namedMembers[end].side == namedMembers[first].side && namedMembers[end].key == namedMembers[first].key)
            ++end;
        std::span<ClassMember> group = namedMembers.mutableSpan().subspan(first, end - first);
        unsigned side = static_cast<unsigned>(group.front().side);
        bool holdsWhatWasDefined = followsMutations && !definition.mayHaveUnknownName[side] && keepsMembersUnder(group.front().side, group, group.front().key);
        bool isEveryValueData = group.back().kind == MethodKind::Method;
        bool hasOneDefinition = (isEveryValueData || group.front().kind != MethodKind::Method) && std::ranges::adjacent_find(group, { }, &ClassMember::kind) == group.end();
        bool isThereFromTheStart = std::ranges::all_of(group, [&](const ClassMember& member) {
            return std::ranges::contains(definition.definitions, memberOrderOf(member), [](const ClassMemberDefinition& entry) {
                return memberOrderOf(entry.member);
            });
        });
        for (ClassMember& member : group) {
            if (holdsWhatWasDefined && isThereFromTheStart && hasOneDefinition)
                member.mutability = MemberMutability::Fixed;
            member.isWritableData = holdsWhatWasDefined && isEveryValueData && !mutations[side].contains(ObjectMutation::ChangeIntegrity);
            bool isDefinedByClass = std::ranges::contains(definition.definitions, member.function, [](const ClassMemberDefinition& entry) {
                return entry.member.function;
            });
            if (followsUses && member.function && isDefinedByClass && isOnlyCalled(member.function))
                member.closureUse = ClosureUse::CalledOnly;
        }
    }

    if (followsMutations) {
        bool definesConstructorName = std::ranges::any_of(namedMembers, [&](const ClassMember& member) {
            return member.side == ClassSide::Prototype && member.key == constructorKey;
        });
        if (!mutations[prototypeSide].containsAny({ ObjectMutation::ChangePrototype, ObjectMutation::ReachUnknownJavaScript }))
            flow.prototypeLink = MemberMutability::Fixed;
        if (!mutations[staticSide].containsAny({ ObjectMutation::ChangePrototype, ObjectMutation::ReachUnknownJavaScript }))
            flow.staticLink = MemberMutability::Fixed;
        ClassMember constructorAsDefined { .key = constructorKey, .side = ClassSide::Prototype, .kind = MethodKind::Method, .function = layout.constructor };
        bool keepsConstructor = keepsMembersUnder(ClassSide::Prototype, singleElementSpan(constructorAsDefined), constructorKey);
        if (!definesConstructorName && !definition.mayHaveUnknownName[prototypeSide] && keepsConstructor)
            flow.constructorProperty = MemberMutability::Fixed;
        flow.prototypeProperty = MemberMutability::Fixed;
    }
    if (build.isEnabled(FlowResultKind::OwnNames, id.index()) && followsMutations) {
        flow.prototypeNamesAreComplete = !definition.mayHaveUnknownName[prototypeSide] && (!mutations[prototypeSide].containsAny(mutationsOfMembers) || (listsEveryStoredName && writes.under(id, lengthKey).empty() && !mutations[prototypeSide].contains(ObjectMutation::ReachNativeCode)));
        flow.staticNamesAreComplete = !definition.mayHaveUnknownName[staticSide] && !mutations[staticSide].containsAny(mutationsOfMembers);
    }
    flow.members = listMembersInDefinitionOrder(WTF::move(definition.definitions), namedMembers.span());
    flow.namedMembers = WTF::move(namedMembers);
    return flow;
}

static std::optional<MethodKind> kindOfMemberUnder(ValueFlowNameKind kind)
{
    ValueFlowNameShape shape = shapeOf(kind);
    if (shape.reach != ValueFlowNameReach::OneKey || !shape.isValueOrAccessor())
        return std::nullopt;
    return shape.sort == ValueFlowNameSort::Value ? MethodKind::Method : shape.sort == ValueFlowNameSort::Getter ? MethodKind::Getter : MethodKind::Setter;
}

static LayoutID findClassOfConstructorFunction(const ValueFlowResultInputs& inputs, ValueFlowObjectID closure)
{
    FunctionID function = inputs.universe().closureFunction(closure);
    LayoutID constructed = function ? inputs.program.at(function).constructedClass : LayoutID();
    return constructed && inputs.program.at(constructed).has(LayoutFlags::IsConstructorFunction) ? constructed : LayoutID();
}

using HandedToObjectCreate = Vector<std::pair<LayoutID, ValueSetID>>;

static HandedToObjectCreate collectWhatObjectCreateIsHanded(const ValueFlowResultInputs& inputs, ValueSetMaker& maker)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    HandedToObjectCreate handedToObjectCreate;
    for (uint32_t index = program.numberOfLayoutsAsRead(); index < program.layouts.size(); ++index) {
        if (!program.layouts[index].has(LayoutFlags::IsOfCreatedObject))
            continue;
        ValueSetID handed = ValueSets::nothing;
        bool isKnown = false;
        inputs.sets().forEachMember(universe.objectsWith(LayoutID(index)), [&](ValueFlowObjectID created) {
            const ValueFlowSite* site = universe.kindOf(created) == ValueFlowObjectKind::Site ? universe.siteOf(created) : nullptr;
            FunctionID id = site ? universe.functionOf(created) : FunctionID();
            if (!id || !inputs.isReached(id))
                return;
            const Function& function = program.at(id);
            for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
                if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::SetLiteralPrototype || function.at(statement.site).objectOperand() != site->value)
                    continue;
                handed = maker.unite(handed, valueOf(inputs, inputs.bodies().operandsOf(statement)[OperandIndex::SetLiteralPrototype::prototype], maker));
                isKnown = true;
            }
        });
        if (isKnown && !ValueSets::isComplement(handed))
            handedToObjectCreate.append({ LayoutID(index), handed });
    }
    return handedToObjectCreate;
}

using ClassesOfStoredPrototypeObjects = UncheckedKeyHashMap<uint32_t, LayoutID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

static ClassesOfStoredPrototypeObjects collectStoredPrototypeObjects(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex, const HandedToObjectCreate& handedToObjectCreate)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    ClassesOfStoredPrototypeObjects classes;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> fewestHandedWith;
    auto noteObjectsHanded = [&](LayoutID classLayout, ValueSetID handed) {
        unsigned numberHanded = 0;
        sets.forEachMember(handed, [&](ValueFlowObjectID stored) {
            numberHanded += universe.kindOf(stored) == ValueFlowObjectKind::Site;
        });
        sets.forEachMember(handed, [&](ValueFlowObjectID stored) {
            if (universe.kindOf(stored) != ValueFlowObjectKind::Site)
                return;
            auto fewest = fewestHandedWith.add(stored.index(), numberHanded);
            if (fewest.isNewEntry || numberHanded < fewest.iterator->value) {
                fewest.iterator->value = numberHanded;
                classes.set(stored.index(), classLayout);
            } else if (numberHanded == fewest.iterator->value && classes.get(stored.index()) != classLayout)
                classes.set(stored.index(), LayoutID());
        });
    };
    for (const auto& [layout, handed] : handedToObjectCreate)
        noteObjectsHanded(layout, handed);
    const ValueFlowHeapIndex::NodesWrittenUnderName* written = heapIndex.findNodesWrittenUnder(ValueFlowName::property(inputs.program.keyOf(BuiltinNames::prototype)));
    for (const ValueFlowHeapIndex::WrittenNode& node : written ? written->nodes.span() : std::span<const ValueFlowHeapIndex::WrittenNode>()) {
        if (ValueSets::isComplement(node.objects) || ValueSets::isComplement(node.value))
            continue;
        sets.forEachMember(node.objects, [&](ValueFlowObjectID holder) {
            noteObjectsHanded(findClassOfConstructorFunction(inputs, holder), node.value);
        });
    }
    return classes;
}

static WritesToPrototypeObjects collectWritesToPrototypeObjects(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex, const ClassesOfStoredPrototypeObjects& classesOfStoredPrototypeObjects)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    PropertyKeyID prototypeKey = inputs.program.keyOf(BuiltinNames::prototype);
    WritesToPrototypeObjects writes;
    writes.flagsOfClass.fill(0, inputs.program.layouts.size());
    auto setFlag = [&](LayoutID classLayout, uint8_t flag) {
        AtomicRef(writes.flagsOfClass[classLayout.index()]).fetch_or(flag, std::memory_order_relaxed);
    };

    Vector<ValueFlowHeapIndex::Stretch> stretches = heapIndex.stretchesOfNodesWrittenUnder([](ValueFlowName name) {
        return kindOfMemberUnder(name.kind) || shapeOf(name.kind).mayBeAnyMember() || name.kind == ValueFlowNameKind::Elements;
    });
    writes.underNames = collectInParallel<AssignedValue>(inputs.loops(), stretches.size(), [&](unsigned position, Vector<AssignedValue>& found) {
        ValueFlowName name = stretches[position].name;
        std::optional<MethodKind> kind = kindOfMemberUnder(name.kind);
        bool namesElementOrNumberLikeName = name.kind == ValueFlowNameKind::Elements;
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[position].nodes) {
            if (ValueSets::isComplement(node.objects)) {
                if (!namesElementOrNumberLikeName)
                    AtomicRef(writes.isAnyObjectWrittenUnderOtherNames).store(true, std::memory_order_relaxed);
                continue;
            }
            Vector<LayoutID, 4> classesWritten;
            bool isWrittenForOtherObjectsToo = false;
            sets.forEachMember(node.objects, [&](ValueFlowObjectID holder) {
                LayoutID constructed = !namesElementOrNumberLikeName && (!kind || PropertyKeyID(name.payload) == prototypeKey) ? findClassOfConstructorFunction(inputs, holder) : LayoutID();
                bool holdsOther = constructed && (kind != MethodKind::Method || ValueSets::isComplement(node.value) || sets.anyMember(node.value, [&](ValueFlowObjectID stored) {
                    return stored != universe.prototypeOf(constructed);
                }));
                if (holdsOther)
                    setFlag(constructed, WritesToPrototypeObjects::hasOtherPrototypeObjectFlag);
                ValueFlowObjectKind kindOfHolder = universe.kindOf(holder);
                isWrittenForOtherObjectsToo |= kindOfHolder != ValueFlowObjectKind::Prototype && kindOfHolder != ValueFlowObjectKind::Site;
                LayoutID classLayout = universe.classLayoutOf(holder);
                if (!classLayout)
                    classLayout = classesOfStoredPrototypeObjects.get(holder.index());
                if (classLayout && (classesWritten.isEmpty() || classesWritten.last() != classLayout))
                    classesWritten.append(classLayout);
            });
            for (LayoutID classLayout : classesWritten) {
                if (kind)
                    found.append({ classLayout, PropertyKeyID(name.payload), node.value, *kind, isWrittenForOtherObjectsToo || classesWritten.size() > 1 });
                else
                    setFlag(classLayout, WritesToPrototypeObjects::isWrittenUnderOtherNamesFlag);
            }
        }
    });
    parallelSort(inputs.loops(), writes.underNames.mutableSpan(), [](const AssignedValue& a, const AssignedValue& b) {
        return a.order() < b.order();
    });
    return writes;
}

static std::optional<FunctionID> findFunctionOfAccessorIn(const ValueFlowResultInputs& inputs, ValueSetID value)
{
    if (ValueSets::isComplement(value))
        return std::nullopt;
    bool isKnown = true;
    FunctionID function;
    inputs.sets().forEachMember(value, [&](ValueFlowObjectID member) {
        if (member == ValueFlowUniverse::primitive(PrimitiveSet::Undefined))
            return;
        FunctionID defined = inputs.universe().closureFunction(member);
        isKnown &= defined && !function;
        function = defined;
    });
    return isKnown ? std::optional(function) : std::nullopt;
}

enum class StoresUnderAccessors : uint8_t { MayDefineValues, RunSetters };

static bool listAssignedMembers(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, std::span<const AssignedValue> assigned, ClassFlow& flow, StoresUnderAccessors stores = StoresUnderAccessors::MayDefineValues)
{
    ValueSetID allButUndefined = ValueSets::complement(inputs.universe().undefinedValue());
    PropertyKeyID constructorKey = inputs.program.keyOf(BuiltinNames::constructor);
    bool listsEveryName = true;
    for (size_t first = 0, end = 0; first < assigned.size(); first = end) {
        PropertyKeyID key = assigned[first].key;
        ValueSetID values[3] { ValueSets::nothing, ValueSets::nothing, ValueSets::nothing };
        bool isStoredIntoPrototypeObjectsAlone = false;
        for (; end < assigned.size() && assigned[end].key == key; ++end) {
            ValueSetID& ofKind = values[assigned[end].kind == MethodKind::Method ? 0 : assigned[end].kind == MethodKind::Getter ? 1 : 2];
            ofKind = maker.unite(ofKind, assigned[end].value);
            isStoredIntoPrototypeObjectsAlone |= !assigned[end].isWrittenForOtherObjectsToo;
        }
        if (key == constructorKey)
            continue;
        if (!isStoredIntoPrototypeObjectsAlone) {
            listsEveryName = false;
            continue;
        }

        auto list = [&](MethodKind kind, FunctionID function) {
            ClassMember member { .key = key, .side = ClassSide::Prototype, .kind = kind, .function = function };
            flow.namedMembers.append(member);
            if (function)
                flow.members.append(member);
        };
        if (values[1] == ValueSets::nothing && values[2] == ValueSets::nothing) {
            ValueSetID defined = maker.intersect(values[0], allButUndefined);
            ValueFlowObjectID only = ValueSets::isComplement(defined) ? ValueFlowObjectID() : inputs.sets().onlyMember(defined);
            FunctionID function = only ? inputs.universe().closureFunction(only) : FunctionID();
            if (function || stores == StoresUnderAccessors::MayDefineValues)
                list(MethodKind::Method, function);
            continue;
        }
        std::optional<FunctionID> getter = findFunctionOfAccessorIn(inputs, values[1]);
        std::optional<FunctionID> setter = findFunctionOfAccessorIn(inputs, values[2]);
        bool mayBeData = stores == StoresUnderAccessors::MayDefineValues && inputs.sets().meet(values[0], allButUndefined);
        if (mayBeData || !getter || !setter || (!*getter && !*setter)) {
            list(MethodKind::Method, { });
            continue;
        }
        if (*getter)
            list(MethodKind::Getter, *getter);
        if (*setter)
            list(MethodKind::Setter, *setter);
    }
    return listsEveryName;
}

static void noteWhatConstructorFunctionKeeps(const ValueFlowResultInputs& inputs, LayoutID id, const WritesToPrototypeObjects& writes, bool listsEveryName, ClassFlow& flow)
{
    const Layout& layout = inputs.program.at(id);
    if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, id.index()))
        return;
    bool followsNames = inputs.build.isEnabled(FlowResultKind::OwnNames, id.index());
    auto isInSeenHandsAlone = [&](ValueFlowObjectID object, OptionSet<ObjectMutation> mutations) {
        return !mutations.contains(ObjectMutation::ReachUnknownJavaScript) && !inputs.hasEscaped(object) && !inputs.isLost(object);
    };

    ValueFlowObjectID closure = inputs.universe().closureOf(layout.constructor);
    OptionSet<ObjectMutation> mutationsOfFunction = markedMutationsOf(inputs, closure);
    if (!isInSeenHandsAlone(closure, mutationsOfFunction))
        return;
    if (!mutationsOfFunction.contains(ObjectMutation::ChangePrototype))
        flow.staticLink = MemberMutability::Fixed;
    flow.staticNamesAreComplete = followsNames && !mutationsOfFunction.containsAny(mutationsOfMembers);

    if (writes.mayHaveOtherPrototypeObject(id) || mutationsOfFunction.contains(ObjectMutation::RedefineProperty))
        return;
    if (inputs.program.at(layout.constructor).captures.isEmpty())
        flow.prototypeProperty = MemberMutability::Fixed;

    ValueFlowObjectID prototype = inputs.universe().prototypeOf(id);
    OptionSet<ObjectMutation> mutations = markedMutationsOf(inputs, prototype);
    if (!isInSeenHandsAlone(prototype, mutations))
        return;
    if (!layout.base && !mutations.contains(ObjectMutation::ChangePrototype))
        flow.prototypeLink = MemberMutability::Fixed;
    bool mayBeWrittenByNativeCode = mutations.containsAny(mutationsOfMembers) && mutations.contains(ObjectMutation::ReachNativeCode);
    flow.prototypeNamesAreComplete = followsNames && listsEveryName && !writes.isWrittenUnderOtherNames(id) && !mayBeWrittenByNativeCode;
}

static void noteClassesOfCreatedObjects(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const HandedToObjectCreate& handedToObjectCreate, const ClassesOfStoredPrototypeObjects& holders, const WritesToPrototypeObjects& writes, FlowApplication& application)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    if (handedToObjectCreate.isEmpty())
        return;
    enum class State : uint8_t { Unseen, Known, Unknown };
    Vector<State> states(FillWith { }, program.layouts.size(), State::Unseen);
    Vector<LayoutID> bases(program.layouts.size());
    for (const auto& [layout, handed] : handedToObjectCreate) {
        State& state = states[layout.index()];
        inputs.sets().forEachMember(handed, [&, layout = layout](ValueFlowObjectID stored) {
            if (universe.kindOf(stored) != ValueFlowObjectKind::Site)
                return;
            LayoutID layoutOfStored = universe.layoutOf(stored);
            if (!layoutOfStored || holders.get(stored.index()) != layout || inputs.isLost(stored))
                return;
            LayoutID base = program.at(layoutOfStored).has(LayoutFlags::IsOfCreatedObject) ? layoutOfStored : LayoutID();
            if (state == State::Known && bases[layout.index()] != base)
                state = State::Unknown;
            if (state == State::Unseen) {
                state = State::Known;
                bases[layout.index()] = base;
            }
        });
    }
    for (bool changed = true; std::exchange(changed, false);) {
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            if (states[index] == State::Known && bases[index] && (states[bases[index].index()] != State::Known || bases[index].index() == index)) {
                states[index] = State::Unknown;
                changed = true;
            }
        }
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (states[index] != State::Known || !inputs.build.isEnabled(FlowResultKind::ClassEvaluations, index))
            continue;
        ClassFlow& flow = application.layouts[index].classFlow;
        flow.evaluations = ExecutionCount::AtMostOnce;
        flow.prototypeBase = bases[index];
        listAssignedMembers(inputs, maker, writes.of(LayoutID(index)), flow, StoresUnderAccessors::RunSetters);
    }
    for (const auto& [holder, classLayout] : holders) {
        if (!classLayout || states[classLayout.index()] != State::Known)
            continue;
        LayoutID layout = universe.layoutOf(ValueFlowObjectID(holder));
        if (!layout)
            continue;
        application.layouts[layout.index()].objectsArePrototypesOfCreatedObjects = true;
        for (const auto& [created, handed] : handedToObjectCreate) {
            if (created != classLayout)
                continue;
            unsigned numberHanded = 0;
            inputs.sets().forEachMember(handed, [&](ValueFlowObjectID other) {
                numberHanded += universe.kindOf(other) != ValueFlowObjectKind::Primitive;
            });
            application.layouts[layout.index()].isHandedToObjectCreateAlone |= numberHanded == 1;
        }
    }
}

// `Object.setPrototypeOf(Derived.prototype, Base.prototype)`, where Base is a parameter of a factory, so no type names a base.
// Returns, by layout of a constructor function, the one other class whose prototype object the program links above its prototype object.
static Vector<LayoutID> collectBasesLinkedByProgram(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<LayoutID> bases(program.layouts.size());
    const ValueFlowHeapIndex::NodesWrittenUnderName* written = heapIndex.findNodesWrittenUnder(prototypeLinkName);
    if (!written)
        return bases;
    auto classOfPrototypeObject = [&](ValueFlowObjectID object) {
        return universe.kindOf(object) == ValueFlowObjectKind::Prototype ? universe.classLayoutOf(object) : LayoutID();
    };
    BitVector hasSeveral(program.layouts.size());
    for (const ValueFlowHeapIndex::WrittenNode& node : written->nodes) {
        if (ValueSets::isComplement(node.objects) || ValueSets::isComplement(node.value))
            continue;
        inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID holder) {
            LayoutID derived = classOfPrototypeObject(holder);
            if (!derived || !program.at(derived).has(LayoutFlags::IsConstructorFunction))
                return;
            inputs.sets().forEachMember(node.value, [&](ValueFlowObjectID linked) {
                LayoutID base = classOfPrototypeObject(linked);
                // A factory that is handed its own result links two functions of the same text.
                if (!base || base == derived)
                    return;
                if (bases[derived.index()] && bases[derived.index()] != base)
                    hasSeveral.quickSet(derived.index());
                bases[derived.index()] = base;
            });
        });
    }
    for (size_t index : hasSeveral)
        bases[index] = LayoutID();
    return bases;
}

// Every walk up the expected bases must end, so a linked base that closes a cycle is dropped.
static void dropLinkedBasesInCycles(const Program& program, std::span<const LayoutID> basesLinkedByProgram, FlowApplication& application)
{
    // The same rule as expectedBaseOf(), on the results that are not applied yet.
    auto expectedBase = [&](LayoutID id) -> LayoutID {
        const LayoutFlow& flow = application.layouts[id.index()].flow;
        switch (flow.baseState) {
        case BaseState::Layout:
            return flow.base;
        case BaseState::Unknown:
            return program.at(id).base;
        case BaseState::None:
            return application.layouts[id.index()].classFlow.prototypeBase;
        case BaseState::BuiltinClass:
            return { };
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID linked = basesLinkedByProgram[index];
        if (!linked || program.layouts[index].base || application.layouts[index].classFlow.prototypeBase != linked)
            continue;
        LayoutID current = linked;
        for (size_t steps = 0; current && steps <= program.layouts.size(); ++steps)
            current = expectedBase(current);
        if (current)
            application.layouts[index].classFlow.prototypeBase = LayoutID();
    }
}

void computeClassResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    Vector<ClassDefinition> classes = collectClassDefinitions(inputs, tables, maker);
    times.didFinish("classes: definitions"_s);
    computeBaseResults(inputs, classes.span(), application);
    times.didFinish("classes: bases"_s);

    Vector<ValueFlowObjectID> closuresOfMembers;
    for (const ClassDefinition& definition : classes) {
        for (const ClassMemberDefinition& entry : definition.isFollowed ? definition.definitions.span() : std::span<const ClassMemberDefinition>())
            closuresOfMembers.append(inputs.universe().closureOf(entry.member.function));
    }
    ValueSetID closuresTakenOutOfMembers = findClosuresTakenOutOfMembers(inputs, maker, maker.of(closuresOfMembers.span()));
    times.didFinish("classes: closures taken out of members"_s);
    HandedToObjectCreate handedToObjectCreate = collectWhatObjectCreateIsHanded(inputs, maker);
    ClassesOfStoredPrototypeObjects classesOfStoredPrototypeObjects = collectStoredPrototypeObjects(inputs, *inputs.heapIndex, handedToObjectCreate);
    WritesToPrototypeObjects writes = collectWritesToPrototypeObjects(inputs, *inputs.heapIndex, classesOfStoredPrototypeObjects);
    times.didFinish("classes: writes to prototype objects"_s);
    noteClassesOfCreatedObjects(inputs, maker, handedToObjectCreate, classesOfStoredPrototypeObjects, writes, application);
    Vector<LayoutID> basesLinkedByProgram = collectBasesLinkedByProgram(inputs, *inputs.heapIndex);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        ClassFlow& flow = application.layouts[index].classFlow;
        if (!layout.constructor)
            continue;
        if (!layout.has(LayoutFlags::IsConstructorFunction)) {
            if (classes[index].isFollowed)
                flow = computeClassFlow(inputs, LayoutID(index), WTF::move(classes[index]), closuresTakenOutOfMembers, writes);
            continue;
        }
        flow.prototypeBase = layout.base ? layout.base : basesLinkedByProgram[index];
        if (classes[index].isFollowed && classes[index].isCreatedAtMostOnce() && inputs.build.isEnabled(FlowResultKind::ClassEvaluations, index)) {
            flow.evaluations = ExecutionCount::AtMostOnce;
            bool listsEveryName = listAssignedMembers(inputs, maker, writes.of(LayoutID(index)), flow);
            noteWhatConstructorFunctionKeeps(inputs, LayoutID(index), writes, listsEveryName, flow);
        }
    }
    dropLinkedBasesInCycles(program, basesLinkedByProgram.span(), application);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
