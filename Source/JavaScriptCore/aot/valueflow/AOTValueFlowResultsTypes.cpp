/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowApply.h"
#include "AOTFlowRecords.h"
#include "AOTIRPrinter.h"
#include "AOTResultRules.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <atomic>
#include <wtf/Lock.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned variablesPerTask = 256;

namespace {

// What is counted while the sets become types. A set that names an object outside the type as read shows an unsound cast, or a fault in the type.
struct SetsOutsideTypes {
    explicit SetsOutsideTypes(Vector<String>* lines)
        : listsThem(lines)
        , lines(lines)
    {
    }

    void note(const Program&, const String& place, const Type& held, const Type& asRead);

    std::atomic<uint64_t> count { 0 };
    // Of the positions that may hold an object as read: those whose set is complete, and the others.
    std::atomic<uint64_t> provenPositions { 0 };
    std::atomic<uint64_t> unprovenPositions { 0 };
    std::atomic<uint64_t> narrowedPositions { 0 };
    const bool listsThem;
    Lock lock;
    Vector<String>* lines WTF_GUARDED_BY_LOCK(lock);
};

// Only the start of a location puts `absent` into its set. No read sees it where a store comes before every read.
enum class DeadZone : uint8_t { MayBeObserved, IsNeverObserved };

static DeadZone deadZoneOf(const LocationFlow& flow)
{
    return flow.initialization == InitializationState::Unknown ? DeadZone::MayBeObserved : DeadZone::IsNeverObserved;
}

// The subject of the switches is the variable, the layout or the function that the positions belong to.
class SetConverter {
public:
    SetConverter(const ValueFlowResultInputs& inputs, const BitVector& layoutsThatKeepTheirShape, TypeArena& types, SetsOutsideTypes& setsOutsideTypes, uint32_t switchSubject)
        : m_inputs(inputs)
        , m_layoutsThatKeepTheirShape(layoutsThatKeepTheirShape)
        , m_types(types)
        , m_setsOutsideTypes(setsOutsideTypes)
        , m_switchSubject(switchSubject)
    {
    }

    TypeArena& types() { return m_types; }

    // The part of the type as read that the set leaves, where the set is complete: it holds nothing that may be any object. Nothing where the set is not complete,
    // where nothing has arrived, and where the set says no more than the type.
    // `place` says in the words of the IR text where the set arrives.
    std::optional<Type> narrow(const Type& asRead, ValueSetID, NOESCAPE const Invocable<String()> auto& place, DeadZone = DeadZone::MayBeObserved);
    FlowPositionUpdate narrowPosition(const DeclaredType&, ValueSetID, NOESCAPE const Invocable<String()> auto& place, DeadZone = DeadZone::MayBeObserved);

private:
    bool isEnabled(FlowResultKind kind) const { return m_inputs.build.isEnabled(kind, m_switchSubject); }
    void add(Type&, Vector<LayoutID, 8>& layouts, ValueFlowObjectID);
    Type typeOf(ValueSetID);

    const ValueFlowResultInputs& m_inputs;
    const BitVector& m_layoutsThatKeepTheirShape;
    TypeArena& m_types;
    SetsOutsideTypes& m_setsOutsideTypes;
    uint32_t m_switchSubject;
};

} // anonymous namespace

void SetsOutsideTypes::note(const Program& program, const String& place, const Type& held, const Type& asRead)
{
    StringPrintStream line;
    line.print(place, " holds "_s);
    printType(line, program, held);
    line.print(", declared "_s);
    printType(line, program, asRead);
    Locker locker { lock };
    lines->append(line.toString());
}

// A type names a layout only with a proof that the shape stays. Without one an object with a layout is an object like any other.
void SetConverter::add(Type& type, Vector<LayoutID, 8>& layouts, ValueFlowObjectID member)
{
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = m_inputs.universe();

    switch (universe.kindOf(member)) {
    case ValueFlowObjectKind::Primitive: {
        uint16_t kind = static_cast<uint16_t>(1u << member.index());
        if (kind == Primitive::LiteralString)
            kind = Primitive::String;
        else if (kind == Primitive::LiteralSymbol)
            kind = Primitive::Symbol;
        type.primitives |= kind & Primitive::Defined;
        return;
    }
    case ValueFlowObjectKind::Key: {
        // A symbol that an operation creates is another one each time the operation runs, so no symbol is listed.
        const PropertyKey& key = program.at(universe.keyOf(member));
        if (key.kind == PropertyKeyKind::Text)
            type.stringLiterals.add(key.text);
        else
            type.primitives |= isSymbolKind(key.kind) ? Primitive::Symbol : Primitive::Symbol | Primitive::String;
        return;
    }
    case ValueFlowObjectKind::Closure:
        type.functions.add(universe.closureFunction(member));
        return;
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin: {
        // What stands for all that a native binding answers is no object of the realm, and no type names it.
        BuiltinID builtin = builtins.canonicalID(universe.builtinOf(member));
        if (isObjectKind(builtins.at(builtin).kind))
            type.builtins.add(builtin);
        else
            type.anyObject = type.anyFunction = true;
        return;
    }
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::Site: {
        LayoutID layout = universe.layoutOf(member);
        if (layout && m_layoutsThatKeepTheirShape.quickGet(layout.index())) {
            layouts.append(layout);
            return;
        }
        BuiltinClassID builtinClass = layout ? BuiltinClassID() : universe.classOf(member);
        if (!builtinClass)
            type.anyObject = type.anyFunction = true;
        else if (builtins.at(builtinClass).hasClosureInstances)
            type.anyFunction = true;
        else
            type = type.unionWith(Type::builtinClass(builtinClass), m_types);
        return;
    }
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
        type.anyObject = type.anyFunction = true;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type SetConverter::typeOf(ValueSetID set)
{
    BuildContext& build = m_inputs.build;
    Type type;
    Vector<LayoutID, 8> layouts;
    m_inputs.sets().forEachMember(set, [&](ValueFlowObjectID member) {
        add(type, layouts, member);
    });
    std::ranges::sort(layouts);
    removeRepeatedElements(layouts);
    bool namesLayouts = layouts.size() <= build.limit(LimitID::MaxLayoutsInType) && isEnabled(FlowResultKind::ProvenLayoutSets);
    if (!layouts.isEmpty() && !namesLayouts)
        type.anyObject = type.anyFunction = true;

    if (type.primitives & Primitive::Integer)
        type.integers = Range::full();
    bool listsLiterals = isEnabled(FlowResultKind::LiteralValues) && !(type.primitives & Primitive::String) && type.stringLiterals.size() <= build.limit(LimitID::MaxLiteralsInType);
    if (!listsLiterals && !type.stringLiterals.isEmpty()) {
        type.primitives |= Primitive::String;
        type.stringLiterals = { };
    }
    if (!type.functions.isEmpty() && (!isEnabled(FlowResultKind::ExactFunctions) || type.functions.size() > build.limit(LimitID::MaxFunctionsInType)))
        type.anyFunction = true;
    if (type.builtins.size() > build.limit(LimitID::MaxBuiltinsInType))
        type.anyObject = type.anyFunction = true;
    type.canonicalize();
    if (!layouts.isEmpty() && namesLayouts)
        return type.unionWith(Type::forLayouts(layouts.span(), m_types), m_types);
    return type;
}

std::optional<Type> SetConverter::narrow(const Type& asRead, ValueSetID set, NOESCAPE const Invocable<String()> auto& place, DeadZone deadZone)
{
    const Program& program = m_inputs.program;
    if (!asRead.isJavaScriptValue() || ValueSets::isComplement(set) || m_inputs.sets().isEmpty(set) || !isEnabled(FlowResultKind::NarrowedTypes))
        return std::nullopt;
    if (m_inputs.sets().meet(set, m_inputs.universe().objectsThatMayBeAnything()))
        return std::nullopt;

    Type held = typeOf(set);
    if (!isEnabled(FlowResultKind::RemovedNullish))
        held.primitives |= asRead.primitives & Primitive::UndefinedOrNull;
    if (deadZone == DeadZone::IsNeverObserved)
        held.primitives &= ~Primitive::Absent;
    if (!isEnabled(FlowResultKind::Initialization))
        held.primitives |= asRead.primitives & Primitive::Absent;
    // The meet would drop an object that is there.
    if (!held.withOnlyObjects().isSubsetOf(asRead, program)) {
        m_setsOutsideTypes.count.fetch_add(1, std::memory_order_relaxed);
        if (m_setsOutsideTypes.listsThem)
            m_setsOutsideTypes.note(program, place(), held, asRead);
        return std::nullopt;
    }
    Type narrowed = held.intersectWith(asRead, m_types);
    if (narrowed.isNever() || narrowed == asRead)
        return std::nullopt;
    return narrowed;
}

FlowPositionUpdate SetConverter::narrowPosition(const DeclaredType& declared, ValueSetID set, NOESCAPE const Invocable<String()> auto& place, DeadZone deadZone)
{
    const Program& program = m_inputs.program;
    if (program.at(declared.type).hasObjects() && !m_inputs.sets().isEmpty(set)) {
        bool isComplete = !ValueSets::isComplement(set) && !m_inputs.sets().meet(set, m_inputs.universe().objectsThatMayBeAnything());
        (isComplete ? m_setsOutsideTypes.provenPositions : m_setsOutsideTypes.unprovenPositions).fetch_add(1, std::memory_order_relaxed);
    }
    std::optional<Type> narrowed = narrow(program.at(declared.type), set, place, deadZone);
    if (!narrowed)
        return { };
    m_setsOutsideTypes.narrowedPositions.fetch_add(1, std::memory_order_relaxed);
    // Nothing is predicted here. A prediction as read that shares nothing with what arrives is taken back: it has to overlap the type of its position.
    bool predictionNeverArrives = declared.predicted && !program.at(declared.predicted).overlaps(*narrowed, program);
    return { m_types.intern(*narrowed), predictionNeverArrives ? TypeTable::never : TypeID() };
}

// What is written by something other than an operation is not asked of the set.
static void narrowEnvironmentFields(const ValueFlowResultInputs& inputs, LayoutID id, SetConverter& converter, FlowLayoutUpdate& update)
{
    const Layout& layout = inputs.program.at(id);
    if (!layout.isEnvironment())
        return;
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        if (update.fields[index].flow.location.writers == WriterState::AllKnown)
            update.fields[index].position = converter.narrowPosition(layout.fields[index].declared, inputs.solver().valueOf(inputs.bodies().nodes().environmentField(id, index)), [&] {
                return makeString('L', id.index(), '.', index);
            }, deadZoneOf(update.fields[index].flow.location));
    }
}

// A field is absent until it is stored, which no store says. A value outside the type of a field takes the field out of its slot, so that no such value is ever stored is a result of its own.
static void narrowFieldsOfObjects(const ValueFlowResultInputs& inputs, LayoutID id, const ValueFlowFieldContents& contents, SetConverter& converter, FlowLayoutUpdate& update)
{
    const Program& program = inputs.program;
    const Layout& layout = program.at(id);
    if (!hasObjectsThatCodeAlters(layout.kind))
        return;
    ValueSetMaker maker(inputs.bodies().sets());
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        FieldFlow& flow = update.fields[index].flow;
        if (flow.location.writers != WriterState::AllKnown)
            continue;
        ValueSetID stored = contents.stored[id.index()][index];
        const Type& asRead = program.at(layout.fields[index].declared.type);
        bool startsAbsent = asRead.primitives & Primitive::Absent && !inputs.sets().isEmpty(stored);
        update.fields[index].position = converter.narrowPosition(layout.fields[index].declared, startsAbsent ? maker.unite(stored, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Absent))) : stored, [&] {
            return makeString('L', id.index(), '.', index);
        });

        if (inputs.build.isEnabled(FlowResultKind::HolderMutations, id.index()) && (asRead.isAnything() || inputs.sets().isSubset(stored, inputs.universe().membersSurelyInside(maker, program, asRead))))
            flow.stability.add(FieldStability::IsNeverStoredWithOtherType);
        if (flow.location.initialization == InitializationState::SetAtCreation && flow.stability == everyFlagOf<FieldStability>())
            flow.occupancy = Occupancy::AlwaysFilled;
    }
}

static void narrowVariable(const ValueFlowResultInputs& inputs, VariableID id, SetConverter& converter, FlowVariableUpdate& update)
{
    if (update.flow.writers == WriterState::AllKnown)
        update.position = converter.narrowPosition(inputs.program.at(id).declared, inputs.solver().valueOf(inputs.bodies().nodes().variable(id)), [&] {
            return makeString('g', id.index());
        }, deadZoneOf(update.flow));
}

// Every closure stores its captures, also one that is never called, and the node of a capture holds what the calls hand on. So a capture keeps its type.
static void narrowEntryParameters(const ValueFlowResultInputs& inputs, FunctionID id, SetConverter& converter, FlowSignatureUpdate& update)
{
    const Function& function = inputs.program.at(id);
    if (!function.mayChangeSignature())
        return;
    Vector<FlowPositionUpdate> parameters(function.numberOfEntryParameters());
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        const DeclaredType* declared = function.declaredEntryParameter(index);
        if (declared && function.entryParameterKind(index) != EntryParameter::Capture)
            parameters[index] = converter.narrowPosition(*declared, inputs.solver().valueOf(inputs.bodies().nodes().entryParameter(id, index)), [&] {
                return makeString('f', id.index(), " entry parameter "_s, index);
            });
    }
    if (!std::ranges::all_of(parameters, &FlowPositionUpdate::isEmpty))
        update.entryParameters = WTF::move(parameters);
}

// The result of a coroutine as its callers see it is the promise or the generator, and the node holds what the body returns.
static void narrowResults(const ValueFlowResultInputs& inputs, FunctionID id, SetConverter& converter, FlowSignatureUpdate& update)
{
    const Function& function = inputs.program.at(id);
    if (!function.mayChangeSignature() || function.isUnsplitCoroutine())
        return;
    Vector<FlowPositionUpdate, 1> results(function.results.size(), [&](size_t index) {
        return converter.narrowPosition(function.results[index], inputs.solver().valueOf(inputs.bodies().nodes().result(id, index)), [&] {
            return makeString('f', id.index(), " result "_s, index);
        });
    });
    if (!std::ranges::all_of(results, &FlowPositionUpdate::isEmpty))
        update.results = WTF::move(results);
}

// The values that the reduced body names: what an operation gives and what it takes, what a branch tests and what a cast claims. Of the others it keeps nothing.
// The read of a location has the type of the location.
static bool hasTypeOfNarrowedLocation(const FlowApplication& application, const Value& value)
{
    switch (value.opcode) {
    case Opcode::GetModuleVariable:
        return application.variables[value.variable().index()].flow.writers == WriterState::AllKnown;
    case Opcode::GetEnvironmentField:
        return application.layouts[value.layout().index()].fields[value.field()].flow.location.writers == WriterState::AllKnown;
    default:
        return false;
    }
}

static void proveValuesOfOperations(const ValueFlowResultInputs& inputs, FunctionID id, SetConverter& converter, const FlowApplication& application, FlowBodyUpdate& update)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    ValueSetMaker maker(inputs.bodies().sets());
    BitVector isDecided(function.values.size());

    auto setOf = [&](const ValueFlowOperand& operand) {
        ValueSetID held = operand.constant ? operand.constant : ValueSets::nothing;
        if (operand.node)
            held = maker.unite(held, inputs.solver().valueOf(operand.node));
        return maker.intersect(held, operand.mask ? operand.mask : ValueSets::anything);
    };
    auto prove = [&](ValueID value, ValueSetID held) {
        if (isDecided.quickSet(value.index()) || hasTypeOfNarrowedLocation(application, function.at(value)))
            return;
        const Type& asRead = program.typeOf(function, value);
        std::optional<Type> narrowed = converter.narrow(asRead, held, [&] {
            return makeString('f', id.index(), ":v"_s, value.index());
        });
        // Later passes answer a lookup by name from the records, also where its narrower type buys nothing. In a validating build the cast brings the test that keeps the lookup.
        bool isTestedLookup = narrowed && inputs.build.validates() && function.at(value).opcode == Opcode::PropGet && !asRead.isSubsetOf(*narrowed, program);
        if (narrowed && (isTestedLookup || isWorthNarrowing(program, asRead, *narrowed)))
            update.proofs.append(FlowValueProof { value, converter.types().intern(*narrowed), { } });
    };
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation)
            continue;
        const Value& operation = function.at(statement.site);
        if (statement.result && !numberOfResultsOf(program, function, operation))
            prove(statement.site, inputs.solver().valueOf(statement.result));

        std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
        if (operands.size() != operation.numberOfOperands())
            continue;
        for (uint32_t index = 0; index < operands.size(); ++index) {
            if (operation.operand(index))
                prove(operation.operand(index), setOf(operands[index]));
        }
    }

    // What a branch tests and what a cast claims. A claim that every member of the set meets tests nothing.
    for (const ValueFlowObservedValue& observed : std::ranges::equal_range(inputs.bodies().observedValues(), id, { }, &ValueFlowObservedValue::function)) {
        if (!observed.test)
            continue;
        const Value& test = function.at(observed.test);
        ValueSetID held = setOf(observed.operand);
        prove(test.operand(0), held);
        if (test.opcode != Opcode::Claim || !inputs.build.isEnabled(FlowResultKind::NarrowedTypes, id.index()) || inputs.sets().isEmpty(held))
            continue;
        if (inputs.sets().isSubset(held, inputs.universe().membersSurelyInside(maker, program, program.at(test.castType()))))
            update.claims.append(FlowClaimUpdate { .claim = observed.test, .isRedundant = true });
    }
    std::ranges::sort(update.proofs, { }, &FlowValueProof::value);
    std::ranges::sort(update.claims, { }, &FlowClaimUpdate::claim);
}

void computeTypeResults(const ValueFlowResultInputs& inputs, const BitVector& layoutsThatKeepTheirShape, const ValueFlowFieldContents& fieldContents, TypeTable& types, FlowResultTables& tables, Vector<String>* linesAboutSetsOutsideTypes)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    unsigned numberOfLayouts = program.layouts.size();
    unsigned numberOfVariableTasks = (program.variables.size() + variablesPerTask - 1) / variablesPerTask;
    unsigned firstFunctionTask = numberOfLayouts + numberOfVariableTasks;
    unsigned numberOfTasks = firstFunctionTask + program.functions.size();
    auto forEachVariableOf = [&](unsigned task, const auto& functor) {
        uint32_t first = (task - numberOfLayouts) * variablesPerTask;
        for (uint32_t index = first; index < std::min<uint32_t>(first + variablesPerTask, program.variables.size()); ++index)
            functor(index);
    };

    SetsOutsideTypes setsOutsideTypes(linesAboutSetsOutsideTypes);
    TypeInterningPhase phase(types, numberOfTasks);
    forEachIndexInParallel(inputs.loops(), numberOfTasks, [&](unsigned task) {
        TypeArena arena(phase, task);
        if (task < numberOfLayouts) {
            SetConverter converter(inputs, layoutsThatKeepTheirShape, arena, setsOutsideTypes, task);
            narrowEnvironmentFields(inputs, LayoutID(task), converter, application.layouts[task]);
            narrowFieldsOfObjects(inputs, LayoutID(task), fieldContents, converter, application.layouts[task]);
            return;
        }
        if (task < firstFunctionTask) {
            forEachVariableOf(task, [&](uint32_t index) {
                SetConverter converter(inputs, layoutsThatKeepTheirShape, arena, setsOutsideTypes, index);
                narrowVariable(inputs, VariableID(index), converter, application.variables[index]);
            });
            return;
        }
        FunctionID id(task - firstFunctionTask);
        if (!inputs.isReached(id) || !program.at(id).hasBody())
            return;
        SetConverter converter(inputs, layoutsThatKeepTheirShape, arena, setsOutsideTypes, id.index());
        // Whoever else enters the function passes what nobody has looked at.
        if (tables.functions.functions[id.index()].callers == CallerState::AllKnown)
            narrowEntryParameters(inputs, id, converter, application.signatures[id.index()]);
        narrowResults(inputs, id, converter, application.signatures[id.index()]);
        proveValuesOfOperations(inputs, id, converter, application, application.bodies[id.index()]);
    });

    inputs.build.statistics().count(StatisticID::FlowSetsOutsideTypes, setsOutsideTypes.count.load());
    inputs.build.statistics().count(StatisticID::FlowProvenPositions, setsOutsideTypes.provenPositions.load());
    inputs.build.statistics().count(StatisticID::FlowUnprovenPositions, setsOutsideTypes.unprovenPositions.load());
    inputs.build.statistics().count(StatisticID::FlowNarrowedPositions, setsOutsideTypes.narrowedPositions.load());
    if (linesAboutSetsOutsideTypes)
        std::ranges::sort(*linesAboutSetsOutsideTypes, codePointCompareLessThan);

    TypeRemap remap = phase.finish(StringRemap { });
    forEachIndexInParallel(inputs.loops(), numberOfTasks, [&](unsigned task) {
        if (remap.isIdentityFor(task))
            return;
        if (task < numberOfLayouts) {
            remapTypes(application.layouts[task], remap);
            return;
        }
        if (task < firstFunctionTask) {
            forEachVariableOf(task, [&](uint32_t index) {
                remapTypes(application.variables[index].position, remap);
            });
            return;
        }
        remapTypes(application.signatures[task - firstFunctionTask], remap);
        remapTypes(application.bodies[task - firstFunctionTask], remap);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
