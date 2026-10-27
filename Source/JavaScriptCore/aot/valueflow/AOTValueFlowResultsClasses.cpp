/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTClaimsAboutClassesAndClosures.h"
#include "AOTLedgerOfClassesAndClosures.h"
#include "AOTLedgerVariables.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueIdentity.h"
#include <tuple>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

namespace Claims = ClaimsAboutClassesAndClosures;

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
    PropertyKeyID key;
    ValueSetID value;
    MethodKind kind { MethodKind::Method };
    bool isWrittenForOtherObjectsToo { false };
};

using AnswersAboutSets = UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

} // anonymous namespace

static auto memberOrderOf(const ClassMember& member)
{
    return std::tuple { member.side, member.key, member.kind, member.function };
}

static ValueSetID throughMaskOf(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, ValueSetID valuesOfNode, ValueSetMaker& maker)
{
    return ValueFlowRuleContext::throughMaskOf(operand, operand.constant ? maker.unite(operand.constant, valuesOfNode) : valuesOfNode, inputs.solver(), maker);
}

static ValueSetID possibleValuesOf(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, ValueSetMaker& maker)
{
    return throughMaskOf(inputs, operand, operand.node ? inputs.solver().possibleValuesOf(operand.node) : ValueSets::nothing, maker);
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

// The name that a computed key is whenever its member is defined: one text that names no element, or one symbol.
static PropertyKeyID findOnlyNameAmong(const ValueFlowResultInputs& inputs, ValueSetID keys)
{
    ValueFlowObjectID only = inputs.sets().onlyMember(keys);
    PropertyKeyID key = only ? inputs.universe().keyOf(only) : PropertyKeyID();
    if (!key)
        return { };
    switch (inputs.program.at(key).kind) {
    case PropertyKeyKind::Text:
        return inputs.program.isIndexLike(key) ? PropertyKeyID() : key;
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return key;
    case PropertyKeyKind::CreatedSymbol:
        return inputs.symbolsCreatedOnce && inputs.symbolsCreatedOnce->get(key.index()) ? key : PropertyKeyID();
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<FlowMemberKey> findKeysOfMembers(const ValueFlowResultInputs& inputs, FunctionID id, ValueSetMaker& maker)
{
    Vector<FlowMemberKey> keys;
    if (!inputs.build.isEnabled(FlowResultKind::LiteralValues, id.index()))
        return keys;
    const Function& function = inputs.program.at(id);
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::DefineMethod || function.at(statement.site).key())
            continue;
        PropertyKeyID key = findOnlyNameAmong(inputs, possibleValuesOf(inputs, inputs.bodies().operandsOf(statement)[OperandIndex::DefineMethod::computedKey], maker));
        // The prototype property of a class cannot be redefined: that definition throws, and one with a written key never does.
        if (!key || (function.at(statement.site).side() == ClassSide::Static && inputs.program.nameClassOf(key) == NameClass::Prototype))
            continue;
        keys.append({ statement.site, key });
    }
    AOT::sort(keys, { }, &FlowMemberKey::definition);
    return keys;
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

        tables.application->bodies[index].keysOfMembers = findKeysOfMembers(inputs, FunctionID(index), maker);
        const Vector<FlowMemberKey>& keysOfMembers = tables.application->bodies[index].keysOfMembers;
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
                ValueID closure = sameObjectThroughCasts(function, value.functionOperand()).valueInFrontOfFacts();
                if (function.at(closure).opcode != Opcode::NewClosure) {
                    definition.isFollowed = false;
                    continue;
                }
                FunctionID defined = function.at(closure).function();
                const FlowMemberKey* found = value.key() ? nullptr : findByValue(keysOfMembers.span(), id, &FlowMemberKey::definition);
                PropertyKeyID key = found ? found->key : value.key();
                if (!key)
                    definition.mayHaveUnknownName[static_cast<unsigned>(value.side())] = true;
                if (!useLists)
                    useLists.emplace(function, cfg);
                bool goesOnlyIntoDefinitions = everyUseThroughCasts(function, *useLists, closure, true, [](const Use& use, Opcode user, OperandUses operandUses) {
                    if (user == Opcode::DefineMethod && use.operandIndex == OperandIndex::DefineMethod::function)
                        return true;
                    if (user == Opcode::SetFunctionName && use.operandIndex == OperandIndex::SetFunctionName::function)
                        return true;
                    return operandUses.containsOnly(OperandUse::Inspects) && protocolsOf(user, use.operandIndex).isEmpty();
                });
                if (!goesOnlyIntoDefinitions)
                    definition.functionsHandedOnAtDefinition.append(defined);
                definition.definitions.append({ value.index(), ClassMember { .key = key, .side = value.side(), .kind = value.methodKind(), .function = defined } });
            }
        }
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::NewClass || !function.at(statement.site).heritageOperand())
                continue;
            ValueSetID& heritage = classes[function.at(statement.site).layout().index()].heritage;
            heritage = maker.unite(heritage, possibleValuesOf(inputs, inputs.bodies().operandsOf(statement)[OperandIndex::NewClass::heritage], maker));
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
        ValueSetID value = solver.possibleValuesOf(ValueFlowNodeID(firstMadeNode + position));
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
            if (flowRuleOf(statement.opcode) != FlowRule::Heap)
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

static bool isLaidOnEveryClassObject(const Program& program, PropertyKeyID key)
{
    return key == program.keyOf(BuiltinNames::prototype) || key == program.keyOf(BuiltinNames::name) || key == program.keyOf(BuiltinNames::length);
}

static std::optional<MethodKind> kindOfMemberUnder(ValueFlowNameKind kind)
{
    ValueFlowNameShape shape = shapeOf(kind);
    if (shape.reach != ValueFlowNameReach::OneKey || !shape.isValueOrAccessor())
        return std::nullopt;
    return shape.sort == ValueFlowNameSort::Value ? MethodKind::Method : shape.sort == ValueFlowNameSort::Getter ? MethodKind::Getter : MethodKind::Setter;
}

static bool isWrittenForOtherObjectsToo(const ValueFlowResultInputs& inputs, const LedgerOfClassesAndClosures& ledger, ValueSetID through, AnswersAboutSets& answers)
{
    auto answer = answers.add(through.index(), false);
    if (!answer.isNewEntry)
        return answer.iterator->value;
    const ValueFlowUniverse& universe = inputs.universe();
    bool reachesOthers = false;
    LayoutID classWritten;
    inputs.sets().forEachMember(through, [&](ValueFlowObjectID holder) {
        ValueFlowObjectKind kind = universe.kindOf(holder);
        reachesOthers |= kind != ValueFlowObjectKind::Prototype && kind != ValueFlowObjectKind::Site;
        LayoutID classLayout = universe.classLayoutOf(holder);
        if (!classLayout)
            classLayout = ledger.classesOfStoredPrototypeObjects().get(holder.index());
        if (!classLayout)
            return;
        reachesOthers |= classWritten && classWritten != classLayout;
        classWritten = classLayout;
    });
    answer.iterator->value = reachesOthers;
    return reachesOthers;
}

// What was seen to be written under names on the prototype object of a class and on the objects that the program stores in its place, by key.
static Vector<AssignedValue> listWhatIsAssigned(const ValueFlowResultInputs& inputs, const LedgerOfClassesAndClosures& ledger, LayoutID classLayout, AnswersAboutSets& answers)
{
    Vector<AssignedValue> assigned;
    auto list = [&](ValueFlowObjectID holder) {
        for (const Write& write : ledger.writesFiledOn(holder)) {
            if (std::optional<MethodKind> kind = kindOfMemberUnder(write.under.kind))
                assigned.append({ PropertyKeyID(write.under.payload), write.arrives, *kind, isWrittenForOtherObjectsToo(inputs, ledger, write.through, answers) });
        }
    };
    list(inputs.universe().prototypeOf(classLayout));
    for (ValueFlowObjectID stored : ledger.prototypeObjectsStoredFor(classLayout))
        list(stored);
    std::ranges::stable_sort(assigned, { }, &AssignedValue::key);
    return assigned;
}

static ClassFlow computeClassFlow(const ValueFlowResultInputs& inputs, LayoutID id, ClassDefinition&& definition, ValueSetID closuresTakenOutOfMembers, const LedgerOfClassesAndClosures& ledger, AnswersAboutSets& answers)
{
    const Program& program = inputs.program;
    const Layout& layout = program.at(id);
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    BuildContext& build = inputs.build;
    PropertyKeyID lengthKey = program.keyOf(BuiltinNames::length);
    PropertyKeyID constructorKey = program.keyOf(BuiltinNames::constructor);
    ValueFlowObjectID holders[2];
    holders[prototypeSide] = universe.prototypeOf(id);
    holders[staticSide] = universe.closureOf(layout.constructor);
    Writers writersOfEveryOwnProperty[2] { ledger.writersOfEveryOwnProperty(holders[0]), ledger.writersOfEveryOwnProperty(holders[1]) };
    bool sharesNameWithStaticField = std::ranges::any_of(definition.definitions, [&](const ClassMemberDefinition& entry) {
        return entry.member.side == ClassSide::Static && (!entry.member.key || std::ranges::contains(layout.staticFields, entry.member.key, &StaticField::key));
    });
    // On a class object only the stores of its static fields are looked through.
    bool looksThroughStores[2];
    looksThroughStores[prototypeSide] = true;
    looksThroughStores[staticSide] = !sharesNameWithStaticField && inputs.ledgerOfVariables->isClassObjectStoredUnderStaticFieldsAlone(id);
    definition.mayHaveUnknownName[staticSide] |= std::ranges::contains(layout.staticFields, PropertyKeyID(), &StaticField::key);

    ClassFlow flow;
    if (build.isEnabled(FlowResultKind::ClassEvaluations, id.index()) && definition.isCreatedAtMostOnce())
        flow.evaluations = ExecutionCount::AtMostOnce;

    bool followsMutations = build.isEnabled(FlowResultKind::HolderMutations, id.index());
    bool followsUses = build.isEnabled(FlowResultKind::ObjectUses, id.index());
    bool isAsMade[2];
    for (unsigned side : { prototypeSide, staticSide }) {
        isAsMade[side] = MemberMutability::Fixed == Claims::mutabilityOf(writersOfEveryOwnProperty[side], [](const Write& write) {
            return write.form == WriteForm::Freeze;
        });
    }
    bool isNeverFrozen[2] { Claims::isNeverFrozen(writersOfEveryOwnProperty[0]), Claims::isNeverFrozen(writersOfEveryOwnProperty[1]) };
    auto keepsMembersUnder = [&](ClassSide side, std::span<const ClassMember> membersUnderOneKey, PropertyKeyID key) {
        if (!looksThroughStores[static_cast<unsigned>(side)] && !isAsMade[static_cast<unsigned>(side)])
            return false;
        return MemberMutability::Fixed == Claims::mutabilityOf(ledger.writersOfProperty(holders[static_cast<unsigned>(side)], key), [&](const Write& write) {
            std::optional<MethodKind> kind = kindOfMemberUnder(write.under.kind);
            return looksThroughStores[static_cast<unsigned>(side)] && kind && !ValueSets::isComplement(write.arrives) && !sets.anyMember(write.arrives, [&](ValueFlowObjectID member) {
                FunctionID function = universe.closureFunction(member);
                return !function || std::ranges::none_of(membersUnderOneKey, [&](const ClassMember& defined) {
                    return defined.kind == *kind && defined.function == function;
                });
            });
        });
    };
    auto isOnlyCalled = [&](FunctionID function) {
        if (program.at(function).has(FunctionFlags::TakesCallee) || std::ranges::binary_search(definition.functionsHandedOnAtDefinition, function))
            return false;
        return Claims::useOfClosure(ledger.reachersOf(universe.closureOf(function), closuresTakenOutOfMembers).value_or(Reachers { { }, everyActor })) == ClosureUse::CalledOnly;
    };

    Vector<ClassMember> namedMembers;
    for (const ClassMemberDefinition& entry : definition.definitions) {
        if (entry.member.key)
            namedMembers.append(entry.member);
    }
    AOT::sort(namedMembers, { }, memberOrderOf);
    namedMembers.shrink(std::ranges::unique(namedMembers, { }, memberOrderOf).begin() - namedMembers.begin());
    if (followsMutations && Claims::isStoredToUnderNamesAlone(writersOfEveryOwnProperty[prototypeSide]))
        listWhatIsStoredOverMembers(inputs, listWhatIsAssigned(inputs, ledger, id, answers).span(), namedMembers);
    Vector<decltype(memberOrderOf(std::declval<const ClassMember&>())), 16> definedMembers(definition.definitions.size(), [&](size_t index) {
        return memberOrderOf(definition.definitions[index].member);
    });
    AOT::sort(definedMembers);
    Vector<FunctionID, 16> definedFunctions(definition.definitions.size(), [&](size_t index) {
        return definition.definitions[index].member.function;
    });
    AOT::sort(definedFunctions);
    for (size_t first = 0, end = 0; first < namedMembers.size(); first = end) {
        while (end < namedMembers.size() && namedMembers[end].side == namedMembers[first].side && namedMembers[end].key == namedMembers[first].key)
            ++end;
        std::span<ClassMember> group = namedMembers.mutableSpan().subspan(first, end - first);
        unsigned side = static_cast<unsigned>(group.front().side);
        bool holdsWhatWasDefined = followsMutations && !definition.mayHaveUnknownName[side] && keepsMembersUnder(group.front().side, group, group.front().key);
        bool isEveryValueData = group.back().kind == MethodKind::Method;
        bool hasOneDefinition = (isEveryValueData || group.front().kind != MethodKind::Method) && std::ranges::adjacent_find(group, { }, &ClassMember::kind) == group.end();
        bool isThereFromTheStart = std::ranges::all_of(group, [&](const ClassMember& member) {
            return std::ranges::binary_search(definedMembers, memberOrderOf(member));
        });
        for (ClassMember& member : group) {
            if (holdsWhatWasDefined && isThereFromTheStart && hasOneDefinition)
                member.mutability = MemberMutability::Fixed;
            member.isWritableData = holdsWhatWasDefined && isEveryValueData && isNeverFrozen[side];
            bool isDefinedByClass = std::ranges::binary_search(definedFunctions, member.function);
            if (followsUses && member.function && isDefinedByClass && isOnlyCalled(member.function))
                member.closureUse = ClosureUse::CalledOnly;
        }
    }

    auto isNamedMember = [&](ClassSide side, PropertyKeyID key) {
        auto found = std::ranges::lower_bound(namedMembers, std::tuple { side, key }, { }, [](const ClassMember& member) {
            return std::tuple { member.side, member.key };
        });
        return found != namedMembers.end() && found->side == side && found->key == key;
    };
    if (followsMutations) {
        flow.prototypeLink = Claims::mutabilityOf(ledger.writersOfPrototypeLink(holders[prototypeSide]));
        flow.staticLink = Claims::mutabilityOf(ledger.writersOfPrototypeLink(holders[staticSide]));
        ClassMember constructorAsDefined { .key = constructorKey, .side = ClassSide::Prototype, .kind = MethodKind::Method, .function = layout.constructor };
        bool keepsConstructor = keepsMembersUnder(ClassSide::Prototype, singleElementSpan(constructorAsDefined), constructorKey);
        if (!isNamedMember(ClassSide::Prototype, constructorKey) && !definition.mayHaveUnknownName[prototypeSide] && keepsConstructor)
            flow.constructorProperty = MemberMutability::Fixed;
        // The language lets nobody write the prototype property of a class.
        flow.prototypeProperty = MemberMutability::Fixed;
    }
    if (build.isEnabled(FlowResultKind::OwnNames, id.index()) && followsMutations) {
        flow.prototypeNamesAreComplete = !definition.mayHaveUnknownName[prototypeSide] && Claims::namesAreComplete(writersOfEveryOwnProperty[prototypeSide], Claims::Redefinitions::MayAddNames, [&](PropertyKeyID key) {
            return key != lengthKey && (key == constructorKey || isNamedMember(ClassSide::Prototype, key));
        });
        flow.staticNamesAreComplete = !definition.mayHaveUnknownName[staticSide] && Claims::namesAreComplete(writersOfEveryOwnProperty[staticSide], Claims::Redefinitions::MayAddNames, [&](PropertyKeyID key) {
            return looksThroughStores[staticSide] && (isNamedMember(ClassSide::Static, key) || std::ranges::contains(layout.staticFields, key, &StaticField::key) || isLaidOnEveryClassObject(program, key));
        });
    }
    flow.members = listMembersInDefinitionOrder(WTF::move(definition.definitions), namedMembers.span());
    flow.namedMembers = WTF::move(namedMembers);
    return flow;
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

static void noteWhatConstructorFunctionKeeps(const ValueFlowResultInputs& inputs, LayoutID id, const LedgerOfClassesAndClosures& ledger, bool listsEveryName, ClassFlow& flow)
{
    const Layout& layout = inputs.program.at(id);
    if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, id.index()))
        return;
    bool followsNames = inputs.build.isEnabled(FlowResultKind::OwnNames, id.index());

    ValueFlowObjectID closure = inputs.universe().closureOf(layout.constructor);
    if (!Claims::isInSeenHandsAlone(ledger.handsOpenOn(closure)))
        return;
    flow.staticLink = Claims::mutabilityOf(ledger.writersOfPrototypeLink(closure));
    flow.staticNamesAreComplete = followsNames && Claims::namesAreComplete(ledger.writersOfEveryOwnProperty(closure), Claims::Redefinitions::MayAddNames, [](PropertyKeyID) {
        return false;
    });

    ValueFlowObjectID prototype = inputs.universe().prototypeOf(id);
    MemberMutability prototypeProperty = Claims::mutabilityOf(ledger.writersOfProperty(closure, inputs.program.keyOf(BuiltinNames::prototype)), [&](const Write& write) {
        return write.under.kind == ValueFlowNameKind::Property && !ValueSets::isComplement(write.arrives) && !inputs.sets().anyMember(write.arrives, [&](ValueFlowObjectID stored) {
            return stored != prototype;
        });
    });
    if (prototypeProperty != MemberMutability::Fixed)
        return;
    if (inputs.program.at(layout.constructor).captures.isEmpty())
        flow.prototypeProperty = MemberMutability::Fixed;

    if (!Claims::isInSeenHandsAlone(ledger.handsOpenOn(prototype)))
        return;
    if (!layout.base)
        flow.prototypeLink = Claims::mutabilityOf(ledger.writersOfPrototypeLink(prototype));
    flow.prototypeNamesAreComplete = followsNames && listsEveryName && Claims::namesAreComplete(ledger.writersOfEveryOwnProperty(prototype), Claims::Redefinitions::AreListedUnderTheirNames, [](PropertyKeyID) {
        return true;
    });
}

static void noteClassesOfCreatedObjects(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const LedgerOfClassesAndClosures& ledger, AnswersAboutSets& answers, FlowApplication& application)
{
    const LedgerOfClassesAndClosures::HandedToObjectCreate& handedToObjectCreate = ledger.handedToObjectCreate();
    const LedgerOfClassesAndClosures::ClassesOfStoredPrototypeObjects& holders = ledger.classesOfStoredPrototypeObjects();
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
        listAssignedMembers(inputs, maker, listWhatIsAssigned(inputs, ledger, LayoutID(index), answers).span(), flow, StoresUnderAccessors::RunSetters);
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
static Vector<LayoutID> collectBasesLinkedByProgram(const ValueFlowResultInputs& inputs, const LedgerOfClassesAndClosures& ledger)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    Vector<LayoutID> bases(program.layouts.size());
    auto classOfPrototypeObject = [&](ValueFlowObjectID object) {
        return universe.kindOf(object) == ValueFlowObjectKind::Prototype ? universe.classLayoutOf(object) : LayoutID();
    };
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID derived(index);
        if (!program.at(derived).has(LayoutFlags::IsConstructorFunction))
            continue;
        bool hasSeveral = false;
        for (const Write& write : ledger.writersOfPrototypeLink(universe.prototypeOf(derived)).seen().predicted()) {
            if (ValueSets::isComplement(write.seenToArrive))
                continue;
            inputs.sets().forEachMember(write.seenToArrive, [&](ValueFlowObjectID linked) {
                LayoutID base = classOfPrototypeObject(linked);
                // A factory that is handed its own result links two functions of the same text.
                if (!base || base == derived)
                    return;
                hasSeveral |= bases[index] && bases[index] != base;
                bases[index] = base;
            });
        }
        if (hasSeveral)
            bases[index] = LayoutID();
    }
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

// A function whose objects take their name at one operation, from a computed key that is one key whenever it runs, has that name as if it were written.
static void nameFunctionsUnderOneKey(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, ValueSetMaker& maker)
{
    const Program& program = inputs.program;
    const ValueFlowHeapTables& heapTables = inputs.universe().builtinTables().heap();
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!mayHoldValueWith(program, id, { Opcode::SetFunctionName }) || !inputs.isReached(id))
            continue;
        const Function& function = program.at(id);
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::SetFunctionName)
                continue;
            const Value& closure = function.at(sameObjectThroughCasts(function, function.at(statement.site).operand(OperandIndex::SetFunctionName::function)));
            if (closure.opcode != Opcode::NewClosure || !heapTables.isNamedAtOneOperation(closure.function()))
                continue;
            ValueFlowObjectID only = inputs.sets().onlyMember(possibleValuesOf(inputs, inputs.bodies().operandsOf(statement)[OperandIndex::SetFunctionName::computedKey], maker));
            PropertyKeyID key = only ? inputs.universe().keyOf(only) : PropertyKeyID();
            if (!key || !tellsNameOfFunction(program.at(key)))
                continue;
            tables.application->signatures[closure.function().index()].keyThatNamesFunction = key;
            tables.application->bodies[index].namesTakenFromOneKey.append(statement.site);
        }
        AOT::sort(tables.application->bodies[index].namesTakenFromOneKey);
    }
}

void computeClassResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    nameFunctionsUnderOneKey(inputs, tables, maker);
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
    const LedgerOfClassesAndClosures& ledger = *inputs.classesAndClosures;
    AnswersAboutSets answers;
    noteClassesOfCreatedObjects(inputs, maker, ledger, answers, application);
    Vector<LayoutID> basesLinkedByProgram = collectBasesLinkedByProgram(inputs, ledger);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        ClassFlow& flow = application.layouts[index].classFlow;
        if (!layout.constructor)
            continue;
        if (!layout.has(LayoutFlags::IsConstructorFunction)) {
            if (classes[index].isFollowed)
                flow = computeClassFlow(inputs, LayoutID(index), WTF::move(classes[index]), closuresTakenOutOfMembers, ledger, answers);
            continue;
        }
        flow.prototypeBase = layout.base ? layout.base : basesLinkedByProgram[index];
        if (classes[index].isFollowed && classes[index].isCreatedAtMostOnce() && inputs.build.isEnabled(FlowResultKind::ClassEvaluations, index)) {
            flow.evaluations = ExecutionCount::AtMostOnce;
            bool listsEveryName = listAssignedMembers(inputs, maker, listWhatIsAssigned(inputs, ledger, LayoutID(index), answers).span(), flow);
            noteWhatConstructorFunctionKeeps(inputs, LayoutID(index), ledger, listsEveryName, flow);
        }
    }
    dropLinkedBasesInCycles(program, basesLinkedByProgram.span(), application);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
