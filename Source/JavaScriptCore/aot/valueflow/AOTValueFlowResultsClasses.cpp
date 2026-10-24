/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValueFlowRulesHeap.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
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

struct ClassMemberDefinition {
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
    // All that some evaluation of the class may extend.
    ValueSetID heritage { ValueSets::nothing };
    Vector<ClassMemberDefinition> definitions;
    // The functions whose closure goes from the operation that makes it to something else than the definition of a member. Ascending.
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
    // `this.size = 0` in a constructor is written for the instances. It reaches a prototype object only if somebody calls the function on one, and then flow has it reach those of many classes.
    bool isWrittenForOtherObjectsToo { false };

    auto order() const { return std::tuple { classLayout, key }; }
};

struct WritesToPrototypeObjects {
    std::span<const AssignedValue> of(LayoutID classLayout) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(underNames, classLayout, { }, &AssignedValue::classLayout);
        return { found.begin(), found.end() };
    }

    std::span<const AssignedValue> under(LayoutID classLayout, PropertyKeyID key) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(underNames, std::tuple { classLayout, key }, { }, &AssignedValue::order);
        return { found.begin(), found.end() };
    }

    bool isWrittenUnderOtherNames(LayoutID classLayout) const { return isAnyObjectWrittenUnderOtherNames || classesWrittenUnderOtherNames.quickGet(classLayout.index()); }
    // Whether the `prototype` of the constructor function may come to hold something else than the object that the function is born with.
    bool mayHaveOtherPrototypeObject(LayoutID classLayout) const { return isAnyObjectWrittenUnderOtherNames || functionsWithOtherPrototypeObject.quickGet(classLayout.index()); }

    Vector<AssignedValue> underNames;
    BitVector classesWrittenUnderOtherNames;
    BitVector functionsWithOtherPrototypeObject;
    bool isAnyObjectWrittenUnderOtherNames { false };
};

// What one function of the program says about the classes that it defines.
struct DefiningFunction {
    const Program& program;
    const Function& function;
    // Only the definition of a method asks where a closure goes.
    const UseLists* useLists;
    Vector<ClassDefinition>& classes;
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

static ValueID definitionOfClosure(const Function& function, ValueID closure)
{
    ValueID definition = sameObjectThroughCasts(function, closure);
    return function.at(definition).opcode == Opcode::NewClosure ? definition : ValueID();
}

// Whoever only looks at the closure, as the operation that names it does, takes nothing out of it.
static bool goesOnlyIntoDefinitions(const Function& function, const UseLists& useLists, ValueID closure)
{
    Vector<ValueID, 4> worklist { closure };
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                return false;
            Opcode user = function.at(use.user).opcode;
            OperandUses operandUses = operandUsesOf(user, use.operandIndex);
            if (isCast(user) && operandUses.contains(OperandUse::Returns)) {
                worklist.append(use.user);
                continue;
            }
            if (user == Opcode::DefineMethod && use.operandIndex == OperandIndex::DefineMethod::function)
                continue;
            if (!operandUses.containsOnly(OperandUse::Inspects) || !protocolsOf(user, use.operandIndex).isEmpty())
                return false;
        }
    }
    return true;
}

static void noteOperation(const DefiningFunction& defining, const Value& value, bool runsAtMostOnce)
{
    auto noteCreation = [&](LayoutID layout) {
        ClassDefinition& definition = defining.classes[layout.index()];
        ++definition.numberOfCreations;
        definition.theCreationRunsAtMostOnce = runsAtMostOnce;
    };
    switch (value.opcode) {
    case Opcode::NewClass:
        noteCreation(value.layout());
        return;
    case Opcode::NewClosure:
        if (LayoutID layout = defining.program.at(value.function()).constructedClass; layout && defining.program.at(layout).has(LayoutFlags::IsConstructorFunction))
            noteCreation(layout);
        return;
    case Opcode::DefineMethod: {
        ClassDefinition& definition = defining.classes[value.layout().index()];
        ValueID closure = definitionOfClosure(defining.function, value.functionOperand());
        if (!closure) {
            definition.isFollowed = false;
            return;
        }
        FunctionID defined = defining.function.at(closure).function();
        if (!value.key())
            definition.mayHaveUnknownName[static_cast<unsigned>(value.side())] = true;
        if (!goesOnlyIntoDefinitions(defining.function, *defining.useLists, closure))
            definition.functionsHandedOnAtDefinition.append(defined);
        definition.definitions.append({ value.index(), ClassMember { .key = value.key(), .side = value.side(), .kind = value.methodKind(), .function = defined } });
        return;
    }
    default:
        return;
    }
}

static void noteHeritages(const ValueFlowResultInputs& inputs, FunctionID id, ValueSetMaker& maker, Vector<ClassDefinition>& classes)
{
    const Function& function = inputs.program.at(id);
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::NewClass)
            continue;
        const Value& operation = function.at(statement.site);
        if (!operation.heritageOperand())
            continue;
        ValueSetID& heritage = classes[operation.layout().index()].heritage;
        heritage = maker.unite(heritage, valueOf(inputs, inputs.bodies().operandsOf(statement)[OperandIndex::NewClass::heritage], maker));
    }
}

static Vector<ClassDefinition> collectClassDefinitions(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, ValueSetMaker& maker)
{
    const Program& program = inputs.program;
    Vector<ClassDefinition> classes(program.layouts.size());
    Vector<uint32_t> functionsToLookAt = findIndicesInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::ClassDefinition, ValueFlowNotedOperation::NewClosure })))
            return false;
        bool isUnsolved = isUnsolvedCode(inputs, tables, FunctionID(index));
        if (!isUnsolved && !inputs.isReached(FunctionID(index)))
            return false;
        return std::ranges::any_of(program.functions[index].values, [&](const Value& value) {
            if (value.opcode == Opcode::NewClass || value.opcode == Opcode::DefineMethod)
                return true;
            return value.opcode == Opcode::NewClosure && program.at(value.function()).constructedClass;
        });
    });
    for (uint32_t index : functionsToLookAt) {
        const Function& function = program.functions[index];
        // Nobody has followed the values through such code: whatever it takes has escaped, and whatever it makes is unknown.
        if (isUnsolvedCode(inputs, tables, FunctionID(index))) {
            for (const Value& value : function.values) {
                if (value.opcode == Opcode::NewClass || value.opcode == Opcode::DefineMethod)
                    classes[value.layout().index()].isFollowed = false;
                else if (value.opcode == Opcode::NewClosure && program.at(value.function()).constructedClass)
                    classes[program.at(value.function()).constructedClass.index()].isFollowed = false;
            }
            continue;
        }
        // Most functions that make a closure make none of a constructor function.
        bool definesMethod = false;
        bool definesClass = false;
        if (inputs.isReached(FunctionID(index))) {
            for (const Value& value : function.values) {
                definesMethod |= value.opcode == Opcode::DefineMethod;
                definesClass |= value.opcode == Opcode::NewClass;
                if (value.opcode != Opcode::NewClosure)
                    continue;
                LayoutID layout = program.at(value.function()).constructedClass;
                definesClass |= layout && program.at(layout).has(LayoutFlags::IsConstructorFunction);
            }
        }
        if (!definesClass && !definesMethod)
            continue;

        const FunctionFlow& flow = tables.functions.functions[index];
        CFG cfg(function);
        std::optional<UseLists> useLists;
        if (definesMethod)
            useLists.emplace(function, cfg);
        DefiningFunction defining { program, function, useLists ? &*useLists : nullptr, classes };
        std::optional<BlockCycles> cycles;
        if (flow.executions == ExecutionCount::AtMostOnce)
            cycles.emplace(cfg);
        for (BlockID block : cfg.reversePostOrder()) {
            bool runsAtMostOnce = cycles && !cycles->isInCycle(block);
            for (ValueID id : function.at(block).values)
                noteOperation(defining, function.at(id), runsAtMostOnce);
        }
        noteHeritages(inputs, FunctionID(index), maker, classes);
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
    for (ClassDefinition& definition : classes) {
        AOT::sort(definition.functionsHandedOnAtDefinition);
        removeRepeatedElements(definition.functionsHandedOnAtDefinition);
    }
    return classes;
}

static BaseOfClass baseFromBuiltin(const BuiltinTable& builtins, BuiltinID builtin)
{
    BuiltinID constructor = builtins.canonicalID(builtin);
    BuiltinClassID instanceClass = builtins.at(constructor).instanceClass;
    BuiltinID classConstructor = instanceClass ? builtins.at(instanceClass).constructor : BuiltinID();
    if (!classConstructor || builtins.canonicalID(classConstructor) != constructor)
        return { };
    return { BaseState::BuiltinClass, { }, instanceClass };
}

static BaseOfClass baseFromObject(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = inputs.universe();
    if (FunctionID function = universe.closureFunction(object)) {
        LayoutID layout = inputs.program.at(function).constructedClass;
        return layout ? BaseOfClass { BaseState::Layout, layout, { } } : BaseOfClass { };
    }
    if (universe.kindOf(object) == ValueFlowObjectKind::Builtin)
        return baseFromBuiltin(inputs.program.builtins(), universe.builtinOf(object));
    return { };
}

static BaseOfClass computeBase(const ValueFlowResultInputs& inputs, LayoutID id, const ClassDefinition& definition)
{
    const Layout& layout = inputs.program.at(id);
    const ValueSets& sets = inputs.sets();
    if (layout.isDerived())
        return { BaseState::Layout, layout.flow.base, { } };
    if (!layout.has(LayoutFlags::HasHeritage))
        return { BaseState::None, { }, { } };
    if (!definition.isFollowed || !inputs.build.isEnabled(FlowResultKind::ClassBases, id.index()))
        return { };

    ValueSetID heritage = definition.heritage;
    if (ValueSets::isComplement(heritage) || sets.isEmpty(heritage))
        return { };

    // A heritage that is a primitive other than null makes the definition of the class throw, so no class has it.
    bool mayBeNull = sets.contains(heritage, ValueFlowUniverse::primitive(PrimitiveSet::Null));
    std::optional<BaseOfClass> joined;
    sets.forEachMember(heritage, [&](ValueFlowObjectID member) {
        if (!inputs.universe().isObject(member))
            return;
        BaseOfClass base = baseFromObject(inputs, member);
        if (!joined)
            joined = base;
        else if (*joined != base)
            joined = BaseOfClass { };
    });
    if (mayBeNull)
        return joined ? BaseOfClass { } : BaseOfClass { BaseState::None, { }, { } };
    return joined.value_or(BaseOfClass { });
}

static LayoutID provenBaseOf(const BaseOfClass& base)
{
    return base.state == BaseState::Layout ? base.base : LayoutID();
}

static void invalidateCyclicBases(Vector<BaseOfClass>& bases)
{
    enum class WalkState : uint8_t { Unvisited, OnPath, Done };

    Vector<WalkState> states(FillWith { }, bases.size(), WalkState::Unvisited);
    Vector<LayoutID, 16> path;
    for (size_t index = 0; index < bases.size(); ++index) {
        path.shrink(0);
        LayoutID current(static_cast<uint32_t>(index));
        while (current && states[current.index()] == WalkState::Unvisited) {
            states[current.index()] = WalkState::OnPath;
            path.append(current);
            current = provenBaseOf(bases[current.index()]);
        }

        bool closesCycle = current && states[current.index()] == WalkState::OnPath;
        size_t firstOnCycle = closesCycle ? path.find(current) : path.size();
        for (size_t position = 0; position < path.size(); ++position) {
            states[path[position].index()] = WalkState::Done;
            if (position >= firstOnCycle)
                bases[path[position].index()] = BaseOfClass { };
        }
    }
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

// The objects of a class whose base is proven hold the fields of the classes above in slots, and an object has only so many. Below a base nobody has proven they are extra properties.
static void invalidateBasesWhoseFieldsHaveNoRoom(const ValueFlowResultInputs& inputs, Vector<BaseOfClass>& bases)
{
    const Program& program = inputs.program;
    uint64_t maxFields = inputs.build.limit(LimitID::FieldsInKeyOrder);
    // No fewer than the objects of the layout hold.
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
    invalidateCyclicBases(bases);
    invalidateBasesWhoseFieldsHaveNoRoom(inputs, bases);
    for (size_t index = 0; index < bases.size(); ++index) {
        LayoutFlow& flow = application.layouts[index].flow;
        flow.baseState = bases[index].state;
        flow.base = bases[index].base;
        flow.baseClass = bases[index].baseClass;
    }
}

namespace {

// A method is defined on its holder and found there by lookups. Whoever asks for a lookup gets the closure, and only an operation whose result is called and nothing else leaves it where it is.
class ClosuresTakenOutOfMembers {
public:
    ClosuresTakenOutOfMembers(const ValueFlowResultInputs&, ValueSetMaker&, ValueSetID closuresOfMembers);

    bool contains(ValueFlowObjectID closure) const { return m_inputs.sets().contains(m_taken, closure); }

private:
    // A node that a rule has asked for and that holds the closure of a member.
    struct Carrier {
        ValueFlowMadeNodeKind kind;
        uint8_t detail;
        uint32_t payload;
        uint32_t set;
        ValueSetID closures;

        auto order() const { return std::tuple { kind, detail, payload, set }; }
    };

    struct Walk {
        BitVector carriersTakenFrom;
        // Of the function whose statements are looked at.
        FunctionID functionWithUseLists;
        std::optional<CFG> cfg;
        std::optional<UseLists> useLists;
    };

    void collectCarriers(ValueSetID closuresOfMembers);
    const Carrier* findCarrier(const ValueFlowNodeKey&) const;
    void noteStatement(Walk&, ValueFlowStatementID, const ValueFlowStatement&) const;
    bool leavesClosuresInPlace(Walk&, ValueFlowStatementID, const ValueFlowStatement&, ValueFlowNameKind) const;
    bool isOnlyCalled(Walk&, FunctionID, ValueID read) const;

    const ValueFlowResultInputs& m_inputs;
    ValueSetMaker& m_maker;
    Vector<Carrier> m_carriers;
    BitVector m_isCarrier;
    ValueSetID m_taken { ValueSets::nothing };
};

} // anonymous namespace

static bool namesAccessors(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
        return true;
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMember:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool namesValuesOfProperties(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::EveryProperty:
        return true;
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMember:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ClosuresTakenOutOfMembers::ClosuresTakenOutOfMembers(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, ValueSetID closuresOfMembers)
    : m_inputs(inputs)
    , m_maker(maker)
{
    const ValueFlowSolver& solver = inputs.solver();
    collectCarriers(closuresOfMembers);
    if (m_carriers.isEmpty())
        return;

    Vector<Walk> walks(inputs.loops().width());
    for (Walk& walk : walks)
        walk.carriersTakenFrom = BitVector(m_carriers.size());

    std::span<const ValueFlowStatement> statements = inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = inputs.bodies().ranges();
    forEachIndexInParallel(inputs.loops(), inputs.bodies().numberOfBodies(), [&](unsigned body, unsigned worker) {
        bool isHost = body == inputs.bodies().hostBody();
        if (!isHost && !inputs.isReached(FunctionID(body)))
            return;
        for (uint32_t index = ranges[body].statement; index < ranges[body + 1].statement; ++index)
            noteStatement(walks[worker], ValueFlowStatementID(index), statements[index]);
    });

    constexpr uint32_t statementsPerTask = 1024;
    uint32_t firstMadeStatement = statements.size();
    uint32_t numberOfMadeStatements = solver.numberOfStatements() - firstMadeStatement;
    forEachIndexInParallel(inputs.loops(), (numberOfMadeStatements + statementsPerTask - 1) / statementsPerTask, [&](unsigned task, unsigned worker) {
        uint32_t end = firstMadeStatement + std::min(numberOfMadeStatements, (task + 1) * statementsPerTask);
        for (uint32_t index = firstMadeStatement + task * statementsPerTask; index < end; ++index) {
            ValueFlowStatementID id(index);
            if (!solver.isAtRest(id))
                noteStatement(walks[worker], id, solver.statementAt(id));
        }
    });

    BitVector carriersTakenFrom(m_carriers.size());
    for (const Walk& walk : walks)
        carriersTakenFrom.merge(walk.carriersTakenFrom);
    for (size_t index : carriersTakenFrom)
        m_taken = m_maker.unite(m_taken, m_carriers[index].closures);
}

void ClosuresTakenOutOfMembers::collectCarriers(ValueSetID closuresOfMembers)
{
    const ValueFlowSolver& solver = m_inputs.solver();
    uint32_t firstMadeNode = solver.input().numberOfNodes;
    ValueSetMakers makers(m_inputs);
    struct CarrierAt {
        Carrier carrier;
        uint32_t indexOfMadeNodes;
    };
    Vector<CarrierAt> found = collectWithMakers<CarrierAt>(m_inputs, makers, solver.graph().numberOfNodes() - firstMadeNode, [&](unsigned position, ValueSetMaker& maker, Vector<CarrierAt>& carriers) {
        ValueFlowNodeID node(firstMadeNode + position);
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(node);
        if (key->kind != ValueFlowMadeNodeKind::Read && key->kind != ValueFlowMadeNodeKind::Lookup)
            return;
        ValueSetID value = solver.valueOf(node);
        if (m_inputs.sets().meet(value, closuresOfMembers))
            carriers.append({ { key->kind, key->detail, key->payload, key->set.index(), maker.intersect(value, closuresOfMembers) }, solver.indexOfMadeNodes(*key) });
    });
    for (const CarrierAt& entry : found)
        m_isCarrier.set(entry.indexOfMadeNodes);
    m_isCarrier.ensureSize(solver.numberOfRecordsOfMadeNodes());
    m_carriers = Vector<Carrier>(found.size(), [&](size_t index) {
        return found[index].carrier;
    });
    parallelSort(m_inputs.loops(), m_carriers.mutableSpan(), [](const Carrier& a, const Carrier& b) {
        return a.order() < b.order();
    });
}

auto ClosuresTakenOutOfMembers::findCarrier(const ValueFlowNodeKey& key) const -> const Carrier*
{
    std::tuple order { key.kind, key.detail, key.payload, key.set.index() };
    auto found = std::ranges::lower_bound(m_carriers, order, { }, &Carrier::order);
    return found != m_carriers.end() && found->order() == order ? &*found : nullptr;
}

// A read that nothing uses stays where it is, so it counts as a read of the value.
bool ClosuresTakenOutOfMembers::isOnlyCalled(Walk& walk, FunctionID id, ValueID read) const
{
    const Function& function = m_inputs.program.at(id);
    // No code is emitted for a model: native code reads what it reads, and no pass turns its calls into direct calls.
    if (function.role == FunctionRole::Model)
        return false;
    if (walk.functionWithUseLists != id) {
        walk.functionWithUseLists = id;
        walk.useLists.reset();
        walk.cfg.emplace(function);
        walk.useLists.emplace(function, *walk.cfg);
    }

    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        std::span<const Use> uses = walk.useLists->usesOf(worklist.takeLast());
        if (uses.empty())
            return false;
        for (const Use& use : uses) {
            if (use.kind != Use::Kind::Operand)
                return false;
            Opcode user = function.at(use.user).opcode;
            OperandUses operandUses = operandUsesOf(user, use.operandIndex);
            if (isCast(user) && operandUses.contains(OperandUse::Returns)) {
                worklist.append(use.user);
                continue;
            }
            if (!operandUses.contains(OperandUse::Calls) || operandUses.containsAny({ OperandUse::Retains, OperandUse::Returns }))
                return false;
        }
    }
    return true;
}

bool ClosuresTakenOutOfMembers::leavesClosuresInPlace(Walk& walk, ValueFlowStatementID id, const ValueFlowStatement& statement, ValueFlowNameKind read) const
{
    switch (statement.kind) {
    case ValueFlowStatementKind::Lookup: {
        // What a lookup of a property or of accessors hands on is in its own result, and whoever has asked for that is looked at by itself.
        ValueFlowNameKind own = m_inputs.solver().keyOf(id).name().kind;
        return namesValuesOfProperties(own) || namesAccessors(own) || own == ValueFlowNameKind::PrototypeLink;
    }
    case ValueFlowStatementKind::Operation:
        if (!isHeapOperation(statement.opcode))
            return false;
        // An operation of the heap calls the accessors that it finds and hands none out.
        if (namesAccessors(read))
            return true;
        switch (statement.opcode) {
        case Opcode::PropGet:
        case Opcode::ElemGet:
        case Opcode::SuperGet:
            return isOnlyCalled(walk, statement.function, statement.site);
        default:
            return false;
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
}

void ClosuresTakenOutOfMembers::noteStatement(Walk& walk, ValueFlowStatementID id, const ValueFlowStatement& statement) const
{
    const ValueFlowSolver& solver = m_inputs.solver();
    m_inputs.forEachIndexOfMadeNodesAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::Read, ValueFlowMadeNodeKind::Lookup }), false, [&](uint32_t index) {
        if (!m_isCarrier.quickGet(index))
            return;
        const ValueFlowNodeKey& key = solver.keyOfMadeNodesAt(index);
        // The statement of a lookup asks for the node of that lookup.
        if (solver.statementOfMadeNodesAt(index) == id || isLookupOfCalleeByCall(m_inputs, statement, key))
            return;
        const Carrier* carrier = findCarrier(key);
        if (!carrier || walk.carriersTakenFrom.quickGet(carrier - m_carriers.begin()))
            return;
        if (!leavesClosuresInPlace(walk, id, statement, key.name().kind))
            walk.carriersTakenFrom.quickSet(carrier - m_carriers.begin());
    });
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
static Vector<ClassMember> listMembersInDefinitionOrder(Vector<ClassMemberDefinition>&& definitions, std::span<const ClassMember> namedMembers)
{
    auto orderOf = [](const ClassMemberDefinition& definition) {
        return std::tuple { definition.index, memberOrderOf(definition.member) };
    };
    AOT::sort(definitions, { }, orderOf);
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

// The closure is made for the definition, lies on the holder, and comes out of there only to be called. Nothing is done to it, and it does not get hold of itself.
static bool isOnlyCalled(const ValueFlowResultInputs& inputs, const ClassDefinition& definition, const ClosuresTakenOutOfMembers& taken, FunctionID function)
{
    ValueFlowObjectID closure = inputs.universe().closureOf(function);
    if (inputs.program.at(function).has(FunctionFlags::TakesCallee) || std::ranges::binary_search(definition.functionsHandedOnAtDefinition, function))
        return false;
    if (inputs.hasEscaped(closure) || inputs.isLost(closure) || inputs.isHeldByNativeCode(closure))
        return false;
    return markedMutationsOf(inputs, closure).isEmpty() && !taken.contains(closure);
}

static bool isOneOfDefinedClosures(const ValueFlowResultInputs& inputs, std::span<const ClassMember> membersUnderOneKey, MethodKind kind, ValueFlowObjectID written)
{
    FunctionID function = inputs.universe().closureFunction(written);
    return function && std::ranges::any_of(membersUnderOneKey, [&](const ClassMember& member) {
        return member.kind == kind && member.function == function;
    });
}

static bool holdsOnlyDefinedClosures(const ValueFlowResultInputs& inputs, std::span<const ClassMember> membersUnderOneKey, std::span<const AssignedValue> writtenUnderTheKey)
{
    return std::ranges::all_of(writtenUnderTheKey, [&](const AssignedValue& written) {
        if (ValueSets::isComplement(written.value))
            return false;
        bool isDefined = true;
        inputs.sets().forEachMember(written.value, [&](ValueFlowObjectID member) {
            isDefined &= isOneOfDefinedClosures(inputs, membersUnderOneKey, written.kind, member);
        });
        return isDefined;
    });
}

static void listWhatIsStoredOverMembers(const ValueFlowResultInputs& inputs, std::span<const AssignedValue> written, Vector<ClassMember>& namedMembers)
{
    size_t numberOfDefinedMembers = namedMembers.size();
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
    if (namedMembers.size() != numberOfDefinedMembers)
        AOT::sort(namedMembers, { }, memberOrderOf);
}

static ClassFlow computeClassFlow(const ValueFlowResultInputs& inputs, LayoutID id, ClassDefinition&& definition, const ClosuresTakenOutOfMembers& taken, const WritesToPrototypeObjects& writes)
{
    const Program& program = inputs.program;
    const Layout& layout = program.at(id);
    BuildContext& build = inputs.build;
    OptionSet<ObjectMutation> mutationsOfHolder[2];
    mutationsOfHolder[static_cast<unsigned>(ClassSide::Prototype)] = markedMutationsOf(inputs, inputs.universe().prototypeOf(id));
    mutationsOfHolder[static_cast<unsigned>(ClassSide::Static)] = markedMutationsOf(inputs, inputs.universe().closureOf(layout.constructor));
    // A store under the name of a static field adds no name to the class object that declares the field, and it leaves a member under another name what it was.
    bool sharesNameWithStaticField = std::ranges::any_of(definition.definitions, [&](const ClassMemberDefinition& entry) {
        return entry.member.side == ClassSide::Static && (!entry.member.key || std::ranges::contains(layout.staticFields, entry.member.key, &StaticField::key));
    });
    if (!sharesNameWithStaticField && inputs.variablesOnObjects->classObjectsStoredUnderStaticFieldsAlone.quickGet(id.index()))
        mutationsOfHolder[static_cast<unsigned>(ClassSide::Static)].remove({ ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation });
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
    bool followsUses = build.isEnabled(FlowResultKind::ObjectUses, id.index());
    ValueFlowObjectID prototype = inputs.universe().prototypeOf(id);
    bool isOnlyStoredToUnderNames = mutationsOn(ClassSide::Prototype).containsAny(mutationsOfMembers)
        && !mutationsOn(ClassSide::Prototype).containsAny({ ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty })
        && !writes.isWrittenUnderOtherNames(id) && !inputs.hasEscaped(prototype) && !inputs.isLost(prototype);
    PropertyKeyID lengthKey = program.keyOf(BuiltinNames::length);
    auto keepsMembersUnder = [&](ClassSide side, std::span<const ClassMember> membersUnderOneKey, PropertyKeyID key) {
        if (!mutationsOn(side).containsAny(mutationsOfMembers))
            return true;
        return side == ClassSide::Prototype && isOnlyStoredToUnderNames && key != lengthKey && holdsOnlyDefinedClosures(inputs, membersUnderOneKey, writes.under(id, key));
    };

    Vector<ClassMember> namedMembers;
    for (const ClassMemberDefinition& entry : definition.definitions) {
        if (entry.member.key)
            namedMembers.append(entry.member);
    }
    AOT::sort(namedMembers, { }, memberOrderOf);
    namedMembers.shrink(std::ranges::unique(namedMembers, { }, memberOrderOf).begin() - namedMembers.begin());
    if (followsMutations && isOnlyStoredToUnderNames)
        listWhatIsStoredOverMembers(inputs, writes.of(id), namedMembers);
    bool listsEveryStoredName = followsMutations && isOnlyStoredToUnderNames;
    for (size_t first = 0; first < namedMembers.size();) {
        size_t end = first + 1;
        while (end < namedMembers.size() && namedMembers[end].side == namedMembers[first].side && namedMembers[end].key == namedMembers[first].key)
            ++end;
        std::span<ClassMember> group = namedMembers.mutableSpan().subspan(first, end - first);
        ClassSide side = group.front().side;
        bool hasKnownNames = followsMutations && !definition.mayHaveUnknownName[static_cast<unsigned>(side)];
        bool holdsWhatWasDefined = hasKnownNames && keepsMembersUnder(side, group, group.front().key);
        bool isEveryValueData = std::ranges::all_of(group, [](const ClassMember& member) {
            return member.kind == MethodKind::Method;
        });
        // What the definition of the class puts on the holder is there from the first moment that anybody has the holder. A store comes at some time: until then a lookup passes the holder by.
        bool isThereFromTheStart = std::ranges::all_of(group, [&](const ClassMember& member) {
            return std::ranges::contains(definition.definitions, memberOrderOf(member), [](const ClassMemberDefinition& entry) {
                return memberOrderOf(entry.member);
            });
        });
        for (ClassMember& member : group) {
            if (holdsWhatWasDefined && isThereFromTheStart && hasOneDefinition(group))
                member.mutability = MemberMutability::Fixed;
            // A store through an object below passes a method, unless the holder is frozen.
            member.isWritableData = holdsWhatWasDefined && isEveryValueData && !mutationsOn(side).contains(ObjectMutation::ChangeIntegrity);
            bool isDefinedByClass = std::ranges::contains(definition.definitions, member.function, [](const ClassMemberDefinition& entry) {
                return entry.member.function;
            });
            if (followsUses && member.function && isDefinedByClass && isOnlyCalled(inputs, definition, taken, member.function))
                member.closureUse = ClosureUse::CalledOnly;
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
        ClassMember constructorAsDefined { .key = constructorKey, .side = ClassSide::Prototype, .kind = MethodKind::Method, .function = layout.constructor };
        bool keepsConstructor = keepsMembersUnder(ClassSide::Prototype, singleElementSpan(constructorAsDefined), constructorKey);
        if (!definesConstructorName && !definition.mayHaveUnknownName[static_cast<unsigned>(ClassSide::Prototype)] && keepsConstructor)
            flow.constructorProperty = MemberMutability::Fixed;
        // No code can change the property of a class.
        flow.prototypeProperty = MemberMutability::Fixed;
    }
    if (build.isEnabled(FlowResultKind::OwnNames, id.index()) && followsMutations) {
        flow.prototypeNamesAreComplete = !definition.mayHaveUnknownName[static_cast<unsigned>(ClassSide::Prototype)] && (!mutationsOn(ClassSide::Prototype).containsAny(mutationsOfMembers) || (listsEveryStoredName && writes.under(id, lengthKey).empty() && !mutationsOn(ClassSide::Prototype).contains(ObjectMutation::ReachNativeCode)));
        flow.staticNamesAreComplete = !definition.mayHaveUnknownName[static_cast<unsigned>(ClassSide::Static)] && !mutationsOn(ClassSide::Static).containsAny(mutationsOfMembers);
    }
    flow.members = listMembersInDefinitionOrder(WTF::move(definition.definitions), namedMembers.span());
    flow.namedMembers = WTF::move(namedMembers);
    return flow;
}

static ValueSetID closuresOfDefinedMembers(const ValueFlowResultInputs& inputs, std::span<const ClassDefinition> classes, ValueSetMaker& maker)
{
    Vector<ValueFlowObjectID> closures;
    for (const ClassDefinition& definition : classes) {
        if (!definition.isFollowed)
            continue;
        for (const ClassMemberDefinition& entry : definition.definitions)
            closures.append(inputs.universe().closureOf(entry.member.function));
    }
    return maker.of(closures.span());
}

static std::optional<MethodKind> kindOfMemberUnder(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Property:
        return MethodKind::Method;
    case ValueFlowNameKind::Getter:
        return MethodKind::Getter;
    case ValueFlowNameKind::Setter:
        return MethodKind::Setter;
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMember:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayNameAnyMember(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::CopiedFrom:
        return true;
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::RedefinedMember:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `const members = (Point.prototype = {})`: the object of an allocation site that is stored as the prototype object of one constructor function. No class for an object that two functions get.
using ClassesOfStoredPrototypeObjects = UncheckedKeyHashMap<uint32_t, LayoutID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

static LayoutID findClassOfConstructorFunction(const ValueFlowResultInputs& inputs, ValueFlowObjectID closure)
{
    FunctionID function = inputs.universe().closureFunction(closure);
    LayoutID constructed = function ? inputs.program.at(function).constructedClass : LayoutID();
    return constructed && inputs.program.at(constructed).has(LayoutFlags::IsConstructorFunction) ? constructed : LayoutID();
}

// By layout of what Object.create makes: what the call is handed. The link of an object that has escaped holds whatever unknown code may put above it, and says nothing about its birth.
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

static ClassesOfStoredPrototypeObjects collectStoredPrototypeObjects(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex, const HandedToObjectCreate& handedToObjectCreate)
{
    ClassesOfStoredPrototypeObjects classes;
    // `Object.create(members)`: the object stands to what the call makes as the prototype object of a constructor function stands to what the function makes.
    // Whoever may be handed the fewest objects says most about each of them: a helper that is handed the objects of many callers takes none of them away from a caller.
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> fewestHandedWith;
    auto noteObjectsHanded = [&](LayoutID classLayout, ValueSetID handed) {
        unsigned numberHanded = 0;
        inputs.sets().forEachMember(handed, [&](ValueFlowObjectID stored) {
            numberHanded += inputs.universe().kindOf(stored) == ValueFlowObjectKind::Site;
        });
        inputs.sets().forEachMember(handed, [&](ValueFlowObjectID stored) {
            if (inputs.universe().kindOf(stored) != ValueFlowObjectKind::Site)
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
    if (!written)
        return classes;
    for (const ValueFlowHeapIndex::WrittenNode& node : written->nodes) {
        if (ValueSets::isComplement(node.objects) || ValueSets::isComplement(node.value))
            continue;
        inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID holder) {
            noteObjectsHanded(findClassOfConstructorFunction(inputs, holder), node.value);
        });
    }
    return classes;
}

static WritesToPrototypeObjects collectWritesToPrototypeObjects(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex, const HandedToObjectCreate& handedToObjectCreate)
{
    PropertyKeyID prototypeKey = inputs.program.keyOf(BuiltinNames::prototype);
    ClassesOfStoredPrototypeObjects storedPrototypeObjects = collectStoredPrototypeObjects(inputs, heapIndex, handedToObjectCreate);
    auto classHeldBy = [&](ValueFlowObjectID holder) {
        if (LayoutID classLayout = inputs.universe().classLayoutOf(holder))
            return classLayout;
        return storedPrototypeObjects.get(holder.index());
    };
    auto note = [&](WritesToPrototypeObjects& writes, ValueFlowName name, const ValueFlowHeapIndex::WrittenNode& node) {
        std::optional<MethodKind> kind = kindOfMemberUnder(name.kind);
        ValueSetID value = node.value;
        // "-2" and "1.5" are written where the elements are, and are names of properties in every object but a typed array. None of them is `prototype`.
        bool namesElementOrNumberLikeName = name.kind == ValueFlowNameKind::Elements;
        if (ValueSets::isComplement(node.objects)) {
            if (!namesElementOrNumberLikeName)
                writes.isAnyObjectWrittenUnderOtherNames = true;
            return;
        }
        if (!namesElementOrNumberLikeName && (!kind || PropertyKeyID(name.payload) == prototypeKey)) {
            inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID holder) {
                LayoutID constructed = findClassOfConstructorFunction(inputs, holder);
                if (!constructed)
                    return;
                bool holdsOther = !kind || *kind != MethodKind::Method || ValueSets::isComplement(value);
                if (!holdsOther) {
                    inputs.sets().forEachMember(value, [&](ValueFlowObjectID stored) {
                        holdsOther |= stored != inputs.universe().prototypeOf(constructed);
                    });
                }
                if (holdsOther)
                    writes.functionsWithOtherPrototypeObject.quickSet(constructed.index());
            });
        }
        // `Point.prototype` may also be what the program has stored there: an object of some allocation site.
        LayoutID firstClass;
        bool isWrittenForOtherObjectsToo = false;
        inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID holder) {
            ValueFlowObjectKind kindOfHolder = inputs.universe().kindOf(holder);
            isWrittenForOtherObjectsToo |= kindOfHolder != ValueFlowObjectKind::Prototype && kindOfHolder != ValueFlowObjectKind::Site;
            LayoutID classLayout = classHeldBy(holder);
            if (!classLayout)
                return;
            isWrittenForOtherObjectsToo |= firstClass && firstClass != classLayout;
            firstClass = classLayout;
        });
        LayoutID lastClass;
        inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID holder) {
            LayoutID classLayout = classHeldBy(holder);
            if (!classLayout || classLayout == lastClass)
                return;
            lastClass = classLayout;
            if (kind)
                writes.underNames.append({ classLayout, PropertyKeyID(name.payload), value, *kind, isWrittenForOtherObjectsToo });
            else
                writes.classesWrittenUnderOtherNames.quickSet(classLayout.index());
        });
    };

    Vector<ValueFlowHeapIndex::Stretch> stretches = heapIndex.stretchesOfNodesWrittenUnder([](ValueFlowName name) {
        return kindOfMemberUnder(name.kind) || mayNameAnyMember(name.kind) || name.kind == ValueFlowNameKind::Elements;
    });
    Vector<WritesToPrototypeObjects> writesOfWorker(inputs.loops().width());
    for (WritesToPrototypeObjects& writes : writesOfWorker) {
        writes.classesWrittenUnderOtherNames.ensureSize(inputs.program.layouts.size());
        writes.functionsWithOtherPrototypeObject.ensureSize(inputs.program.layouts.size());
    }
    forEachIndexInParallel(inputs.loops(), stretches.size(), [&](unsigned position, unsigned worker) {
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[position].nodes)
            note(writesOfWorker[worker], stretches[position].name, node);
    });
    WritesToPrototypeObjects writes = WTF::move(writesOfWorker[0]);
    for (WritesToPrototypeObjects& others : writesOfWorker.mutableSpan().subspan(1)) {
        writes.underNames.appendVector(WTF::move(others.underNames));
        writes.classesWrittenUnderOtherNames.merge(others.classesWrittenUnderOtherNames);
        writes.functionsWithOtherPrototypeObject.merge(others.functionsWithOtherPrototypeObject);
        writes.isAnyObjectWrittenUnderOtherNames |= others.isAnyObjectWrittenUnderOtherNames;
    }
    parallelSort(inputs.loops(), writes.underNames.mutableSpan(), [](const AssignedValue& a, const AssignedValue& b) {
        return a.order() < b.order();
    });
    return writes;
}

static FunctionID findOnlyClosureIn(const ValueFlowResultInputs& inputs, ValueSetID value)
{
    if (ValueSets::isComplement(value))
        return { };
    unsigned numberOfMembers = 0;
    FunctionID function;
    inputs.sets().forEachMember(value, [&](ValueFlowObjectID member) {
        ++numberOfMembers;
        function = inputs.universe().closureFunction(member);
    });
    return numberOfMembers == 1 ? function : FunctionID();
}

// The one function whose closure is defined as a getter or as a setter. No function where a descriptor leaves the part out, which defines undefined. None where nobody can say.
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

// A store under the name of an accessor runs the setter. Where the run time looks at the object before it believes the list, a store that nobody follows takes no accessor out of it.
enum class StoresUnderAccessors : uint8_t { MayDefineValues, RunSetters };

// Whether every name but `constructor` is listed.
static bool listAssignedMembers(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, std::span<const AssignedValue> assignedToPrototypeObject, ClassFlow& flow, StoresUnderAccessors stores = StoresUnderAccessors::MayDefineValues)
{
    PropertyKeyID constructorKey = inputs.program.keyOf(BuiltinNames::constructor);
    bool listsEveryName = true;
    for (size_t first = 0; first < assignedToPrototypeObject.size();) {
        size_t end = first;
        ValueSetID value = ValueSets::nothing;
        ValueSetID getters = ValueSets::nothing;
        ValueSetID setters = ValueSets::nothing;
        bool isStoredIntoPrototypeObjectsAlone = false;
        for (; end < assignedToPrototypeObject.size() && assignedToPrototypeObject[end].key == assignedToPrototypeObject[first].key; ++end) {
            ValueSetID& ofKind = assignedToPrototypeObject[end].kind == MethodKind::Method ? value : assignedToPrototypeObject[end].kind == MethodKind::Getter ? getters : setters;
            ofKind = maker.unite(ofKind, assignedToPrototypeObject[end].value);
            isStoredIntoPrototypeObjectsAlone |= !assignedToPrototypeObject[end].isWrittenForOtherObjectsToo;
        }
        PropertyKeyID key = assignedToPrototypeObject[first].key;
        first = end;
        // A name that is listed without a function refuses every access that passes the prototype object by.
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
        bool mayBeAccessor = getters != ValueSets::nothing || setters != ValueSets::nothing;
        if (!mayBeAccessor) {
            // Beside what a descriptor defines as its `value` lies undefined: the rule cannot tell that the descriptor has the member.
            FunctionID function = findOnlyClosureIn(inputs, maker.intersect(value, ValueSets::complement(inputs.universe().undefinedValue())));
            if (function || stores == StoresUnderAccessors::MayDefineValues)
                list(MethodKind::Method, function);
            continue;
        }
        // An accessor that a descriptor defines, with one function for each part that it has, stands for a definition like a method that is stored.
        std::optional<FunctionID> getter = findFunctionOfAccessorIn(inputs, getters);
        std::optional<FunctionID> setter = findFunctionOfAccessorIn(inputs, setters);
        // The rule defines what it finds as the `value` of a descriptor too: undefined, where the descriptor has none.
        bool mayBeData = stores == StoresUnderAccessors::MayDefineValues && inputs.sets().meet(value, ValueSets::complement(inputs.universe().undefinedValue()));
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

// A function is born with Function.prototype above it and with a prototype object in `prototype`, and that object with `constructor` on it and Object.prototype above it.
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

    // To an access and to the guards of the run time the prototype object of the class is whatever `prototype` holds. So the rest is said only where that stays the object the function is born with.
    if (writes.mayHaveOtherPrototypeObject(id) || mutationsOfFunction.contains(ObjectMutation::RedefineProperty))
        return;
    // An allocation that is not handed a prototype object asks the run time for that of its class, and the run time finds the function of a class by itself only where the closure has no captures (ClassTable::hasOneClassObject).
    if (inputs.program.at(layout.constructor).captures.isEmpty())
        flow.prototypeProperty = MemberMutability::Fixed;

    ValueFlowObjectID prototype = inputs.universe().prototypeOf(id);
    OptionSet<ObjectMutation> mutations = markedMutationsOf(inputs, prototype);
    if (!isInSeenHandsAlone(prototype, mutations))
        return;
    // Where the types of the program name a base, that class is expected above the object, and an object that has kept its link has Object.prototype there.
    if (!layout.base && !mutations.contains(ObjectMutation::ChangePrototype))
        flow.prototypeLink = MemberMutability::Fixed;
    // Whatever is stored or defined on the object is written under a name that is listed, unless something is written under a key nobody knows or by native code.
    bool mayBeWrittenByNativeCode = mutations.containsAny(mutationsOfMembers) && mutations.contains(ObjectMutation::ReachNativeCode);
    flow.prototypeNamesAreComplete = followsNames && listsEveryName && !writes.isWrittenUnderOtherNames(id) && !mayBeWrittenByNativeCode;
}

// What Object.create makes has a class where every object that the call is handed is the object of an allocation site that no other class has. The class above is that of the handed objects, where Object.create makes them too.
static void noteClassesOfCreatedObjects(const ValueFlowResultInputs& inputs, ValueSetMaker& maker, const ValueFlowHeapIndex& heapIndex, const HandedToObjectCreate& handedToObjectCreate, const WritesToPrototypeObjects& writes, FlowApplication& application)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    if (handedToObjectCreate.isEmpty())
        return;
    enum class State : uint8_t { Unseen, Known, Unknown };
    Vector<State> states(FillWith { }, program.layouts.size(), State::Unseen);
    Vector<LayoutID> bases(program.layouts.size());
    ClassesOfStoredPrototypeObjects holders = collectStoredPrototypeObjects(inputs, heapIndex, handedToObjectCreate);
    for (const auto& [layout, handed] : handedToObjectCreate) {
        State& state = states[layout.index()];
        // The run time takes the first object that the call is handed for the prototype object of the class, and sets the guards by what it finds there. So an object nobody knows among them takes nothing away.
        inputs.sets().forEachMember(handed, [&, layout = layout](ValueFlowObjectID stored) {
            if (universe.kindOf(stored) != ValueFlowObjectKind::Site)
                return;
            // An object that several calls may be handed is the one that a module exports before its text has run, more often than not.
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
    // A class below one that has none has none.
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

    ClosuresTakenOutOfMembers taken(inputs, maker, closuresOfDefinedMembers(inputs, classes.span(), maker));
    times.didFinish("classes: closures taken out of members"_s);
    std::unique_ptr<ValueFlowHeapIndex> ownHeapIndex = inputs.heapIndex ? nullptr : makeUnique<ValueFlowHeapIndex>(inputs);
    HandedToObjectCreate handedToObjectCreate = collectWhatObjectCreateIsHanded(inputs, maker);
    WritesToPrototypeObjects writes = collectWritesToPrototypeObjects(inputs, inputs.heapIndex ? *inputs.heapIndex : *ownHeapIndex, handedToObjectCreate);
    times.didFinish("classes: writes to prototype objects"_s);
    noteClassesOfCreatedObjects(inputs, maker, inputs.heapIndex ? *inputs.heapIndex : *ownHeapIndex, handedToObjectCreate, writes, application);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!layout.constructor)
            continue;
        // The members of a constructor function are assigned, and nobody follows the order of the assignments.
        if (layout.has(LayoutFlags::IsConstructorFunction)) {
            ClassFlow& flow = application.layouts[index].classFlow;
            flow.prototypeBase = layout.base;
            const ClassDefinition& definition = classes[index];
            bool isCreatedAtMostOnce = !definition.numberOfCreations || (definition.numberOfCreations == 1 && definition.theCreationRunsAtMostOnce);
            if (definition.isFollowed && isCreatedAtMostOnce && inputs.build.isEnabled(FlowResultKind::ClassEvaluations, index)) {
                flow.evaluations = ExecutionCount::AtMostOnce;
                bool listsEveryName = listAssignedMembers(inputs, maker, writes.of(LayoutID(index)), flow);
                noteWhatConstructorFunctionKeeps(inputs, LayoutID(index), writes, listsEveryName, flow);
            }
            continue;
        }
        if (classes[index].isFollowed)
            application.layouts[index].classFlow = computeClassFlow(inputs, LayoutID(index), WTF::move(classes[index]), taken, writes);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
