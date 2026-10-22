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
#include "AOTThreads.h"
#include "AOTUseLists.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueIdentity.h"
#include <atomic>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class LocationEvent : uint8_t {
    ReadAsValue = 1 << 0,
    ReadAsCallee = 1 << 1,
    // By something that is no operation that the solve has followed: evaluated text, the loader, code that nobody has solved, whoever holds a namespace object or a class object that nobody follows.
    ReadOutsideOperations = 1 << 2,
    WrittenOutsideOperations = 1 << 3,
    // Of static fields alone: by an operation that names the class object and a key.
    StoredThroughObject = 1 << 4,
};

// The variables, then the fields of the environments, layout by layout.
class Locations {
public:
    explicit Locations(const Program& program)
        : m_numberOfVariables(program.variables.size())
    {
        uint32_t next = m_numberOfVariables;
        m_firstFieldOfLayout = program.layouts.map([&](const Layout& layout) {
            uint32_t first = next;
            next += layout.isEnvironment() ? layout.fields.size() : 0;
            return first;
        });
        m_events.fill(0, next);
        m_numberOfStores.fill(0, next);
    }

    size_t size() const { return m_events.size(); }
    uint32_t of(VariableID variable) const { return variable.index(); }
    uint32_t of(LayoutID environment, uint32_t field) const { return m_firstFieldOfLayout[environment.index()] + field; }

    void note(uint32_t location, LocationEvent event) { std::atomic_ref(m_events[location]).fetch_or(static_cast<uint8_t>(event), std::memory_order_relaxed); }
    void noteStore(uint32_t location) { std::atomic_ref(m_numberOfStores[location]).fetch_add(1, std::memory_order_relaxed); }

    OptionSet<LocationEvent> eventsOf(uint32_t location) const { return OptionSet<LocationEvent>::fromRaw(m_events[location]); }
    uint32_t numberOfStoresOf(uint32_t location) const { return m_numberOfStores[location]; }

private:
    uint32_t m_numberOfVariables;
    Vector<uint32_t> m_firstFieldOfLayout;
    Vector<uint8_t> m_events;
    Vector<uint32_t> m_numberOfStores;
};

} // anonymous namespace

static std::optional<uint32_t> locationReadBy(const Locations& locations, const Value& value)
{
    switch (value.opcode) {
    case Opcode::GetModuleVariable:
        return locations.of(value.variable());
    case Opcode::GetEnvironmentField:
        return locations.of(value.layout(), value.field());
    default:
        return std::nullopt;
    }
}

// What a CommonJS module exports as one value lies in a variable, and the run time reads the variable for an operation that answers the value of the module.
static VariableID exportsVariableReadBy(const Program& program, const Value& value)
{
    switch (value.opcode) {
    case Opcode::RequireModule:
    case Opcode::ImportModule:
    case Opcode::ModuleNamespace: {
        const Module& module = program.at(value.module());
        ModuleID captured = commonJsModuleCapturedBy(program, module);
        return captured ? program.at(captured).exportsVariable : module.exportsVariable;
    }
    default:
        return { };
    }
}

// The creation of an environment stores to the fields that it fills.
static void forEachLocationStoredBy(const Program& program, const Locations& locations, const Value& value, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    switch (value.opcode) {
    case Opcode::PutModuleVariable:
        functor(locations.of(value.variable()));
        return;
    case Opcode::PutEnvironmentField:
        functor(locations.of(value.layout(), value.field()));
        return;
    case Opcode::NewObject:
        if (!program.at(value.layout()).isEnvironment())
            return;
        for (uint32_t field : value.fields())
            functor(locations.of(value.layout(), field));
        return;
    default:
        return;
    }
}

// A read that nothing uses stays where it is, so it counts as a read of the value.
static LocationEvent readEventOf(const Function& function, const UseLists& useLists, ValueID read)
{
    // No code is emitted for a model: native code reads what it reads, and no pass turns its calls into direct calls.
    if (function.role == FunctionRole::Model)
        return LocationEvent::ReadAsValue;

    Vector<ValueID, 4> worklist { read };
    while (!worklist.isEmpty()) {
        std::span<const Use> uses = useLists.usesOf(worklist.takeLast());
        if (uses.empty())
            return LocationEvent::ReadAsValue;
        for (const Use& use : uses) {
            if (use.kind != Use::Kind::Operand)
                return LocationEvent::ReadAsValue;
            Opcode user = function.at(use.user).opcode;
            OperandUses operandUses = operandUsesOf(user, use.operandIndex);
            if (isCast(user) && operandUses.contains(OperandUse::Returns)) {
                worklist.append(use.user);
                continue;
            }
            if (!operandUses.contains(OperandUse::Calls) || operandUses.containsAny({ OperandUse::Retains, OperandUse::Returns }))
                return LocationEvent::ReadAsValue;
        }
    }
    return LocationEvent::ReadAsCallee;
}

static void noteOperationsOf(const Program& program, const Function& function, Locations& locations)
{
    bool needsUseLists = std::ranges::any_of(function.values, [&](const Value& value) {
        return locationReadBy(locations, value).has_value();
    });
    std::optional<CFG> cfg;
    std::optional<UseLists> useLists;
    if (needsUseLists) {
        cfg.emplace(function);
        useLists.emplace(function, *cfg);
    }

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (std::optional<uint32_t> location = locationReadBy(locations, value))
                locations.note(*location, readEventOf(function, *useLists, id));
            if (VariableID exports = exportsVariableReadBy(program, value))
                locations.note(locations.of(exports), LocationEvent::ReadOutsideOperations);
            forEachLocationStoredBy(program, locations, value, [&](uint32_t location) {
                locations.noteStore(location);
            });
        }
    }
}

static void noteEverythingOutsideOperations(Locations& locations, uint32_t location)
{
    locations.note(location, LocationEvent::ReadOutsideOperations);
    locations.note(location, LocationEvent::WrittenOutsideOperations);
}

static void noteOperationsOfUnsolvedCode(const Program& program, const Function& function, Locations& locations)
{
    for (const Value& value : function.values) {
        if (std::optional<uint32_t> location = locationReadBy(locations, value))
            locations.note(*location, LocationEvent::ReadOutsideOperations);
        if (VariableID exports = exportsVariableReadBy(program, value))
            locations.note(locations.of(exports), LocationEvent::ReadOutsideOperations);
        forEachLocationStoredBy(program, locations, value, [&](uint32_t location) {
            locations.note(location, LocationEvent::WrittenOutsideOperations);
        });
    }
}

// A closure that evaluated text leaves behind reads and assigns what its scope binds for as long as it lives.
static void noteEvalScopes(const ValueFlowResultInputs& inputs, Locations& locations)
{
    const Program& program = inputs.program;
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                noteEverythingOutsideOperations(locations, locations.of(binding.variable));
            if (binding.layout && binding.field && program.at(binding.layout).isEnvironment())
                noteEverythingOutsideOperations(locations, locations.of(binding.layout, *binding.field));
        }
    }
}

// An export is a variable and a property of the namespace object at once, and a static field is a variable and a property of the class object.
static void noteAccessesThroughObjects(const ValueFlowResultInputs& inputs, Locations& locations)
{
    Vector<ValueFlowAccessThroughObject> reads;
    for (const ValueFlowAccessThroughObject& access : inputs.variablesOnObjects->accesses) {
        uint32_t location = locations.of(access.variable);
        switch (access.kind) {
        case ValueFlowAccessThroughObjectKind::Read:
            reads.append(access);
            break;
        case ValueFlowAccessThroughObjectKind::Observation:
            locations.note(location, access.function ? LocationEvent::ReadAsValue : LocationEvent::ReadOutsideOperations);
            break;
        case ValueFlowAccessThroughObjectKind::Store:
            locations.noteStore(location);
            locations.note(location, LocationEvent::StoredThroughObject);
            if (!access.function)
                locations.note(location, LocationEvent::WrittenOutsideOperations);
            break;
        }
    }

    // The uses of the values of a function are listed once for all its reads.
    std::ranges::sort(reads, { }, &ValueFlowAccessThroughObject::function);
    for (size_t index = 0; index < reads.size();) {
        FunctionID id = reads[index].function;
        const Function& function = inputs.program.at(id);
        CFG cfg(function);
        UseLists useLists(function, cfg);
        for (; index < reads.size() && reads[index].function == id; ++index)
            locations.note(locations.of(reads[index].variable), readEventOf(function, useLists, reads[index].site));
    }
}

static void noteStaticFields(const ValueFlowResultInputs& inputs, Locations& locations)
{
    const ValueFlowVariablesOnObjects& variablesOnObjects = *inputs.variablesOnObjects;
    for (size_t index : variablesOnObjects.staticFieldsInUnknownHands)
        noteEverythingOutsideOperations(locations, locations.of(VariableID(static_cast<uint32_t>(index))));
    for (size_t index : variablesOnObjects.staticFieldsOfEscapedClassObjects)
        locations.note(locations.of(VariableID(static_cast<uint32_t>(index))), LocationEvent::ReadOutsideOperations);
    for (const BitVector* written : { &variablesOnObjects.deletedStaticFields, &variablesOnObjects.redefinedStaticFields, &variablesOnObjects.staticFieldsStoredOtherTypes, &variablesOnObjects.staticFieldsStoredByBuiltins }) {
        for (size_t index : *written)
            locations.note(locations.of(VariableID(static_cast<uint32_t>(index))), LocationEvent::WrittenOutsideOperations);
    }
}

static UnprovenCause causeOfUnknownWriter(const ValueFlowVariablesOnObjects& variablesOnObjects, uint32_t variable)
{
    if (variablesOnObjects.staticFieldsInUnknownHands.quickGet(variable))
        return UnprovenCause::UnknownWriter;
    if (variablesOnObjects.deletedStaticFields.quickGet(variable))
        return UnprovenCause::FieldDeleted;
    if (variablesOnObjects.redefinedStaticFields.quickGet(variable))
        return UnprovenCause::PropertyRedefined;
    return variablesOnObjects.staticFieldsStoredOtherTypes.quickGet(variable) ? UnprovenCause::StoreOfOtherType : UnprovenCause::UnknownWriter;
}

static void forEachExportedVariable(const Module& module, NOESCAPE const Invocable<void(VariableID)> auto& functor)
{
    for (const ModuleExport& entry : module.exports) {
        if (entry.kind == ModuleExportKind::Variable && entry.variable)
            functor(entry.variable);
    }
}

static void noteModules(const ValueFlowResultInputs& inputs, std::span<const ModuleFlow> modules, Locations& locations)
{
    const Program& program = inputs.program;
    BitVector hasExportsReadByName(program.modules.size());
    for (const Entry& entry : program.entries) {
        if (!entry.hostReads.isEmpty())
            hasExportsReadByName.quickSet(entry.module.index());
    }
    // At run time ResolveExport reads the export entries of these modules to decide which names exist, also an entry whose variable no code reads.
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        for (const ModuleRequest& request : module.requests) {
            if (!request.isDynamicStarExport)
                continue;
            hasExportsReadByName.quickSet(index);
            hasExportsReadByName.quickSet(request.target.index());
        }
        for (const RequiredName& requiredName : module.requiredNames)
            hasExportsReadByName.quickSet(requiredName.module.index());
    }

    // Before it answers the namespace object, require() reads two exports by their names: Modules::namespaceForRequire().
    BuiltinName moduleExportsName = program.builtins().nameOf(byteCast<uint8_t>("module.exports"_s.span()));
    auto isReadByRequire = [&](const ModuleExport& entry) {
        BuiltinName name = program.builtinName(entry.name);
        return name && (name == BuiltinNames::__esModule || name == moduleExportsName);
    };

    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        bool hasCode = module.body && program.at(module.body).hasBody();
        if (module.requireResult == RequireResult::Namespace) {
            for (const ModuleExport& entry : module.exports) {
                if (entry.kind == ModuleExportKind::Variable && entry.variable && isReadByRequire(entry))
                    locations.note(locations.of(entry.variable), LocationEvent::ReadOutsideOperations);
            }
        }
        // Native code reads exports of a module of the library by name, and a require that answers the default export reads its variable. Neither creates the namespace object.
        bool isReadWithoutNamespace = module.kind == ModuleKind::RuntimeSource || module.requireResult == RequireResult::DefaultExport;
        forEachExportedVariable(module, [&](VariableID variable) {
            // Whoever reads a namespace object by name alone is listed among the accesses through objects.
            if (hasExportsReadByName.quickGet(index) || isReadWithoutNamespace || modules[index].namespaceUse == NamespaceUse::Unknown)
                locations.note(locations.of(variable), LocationEvent::ReadOutsideOperations);
            // The loader fills the exports of a module without code.
            if (!hasCode)
                locations.note(locations.of(variable), LocationEvent::WrittenOutsideOperations);
        });
        for (const SyntheticExport& entry : module.syntheticExports) {
            if (entry.variable)
                noteEverythingOutsideOperations(locations, locations.of(entry.variable));
        }
        // The loader reads the value of the CommonJS file when it links the module that takes its names from it.
        if (ModuleID captured = commonJsModuleCapturedBy(program, module))
            locations.note(locations.of(program.at(captured).exportsVariable), LocationEvent::ReadOutsideOperations);
    }

    // The host may ask what an entry point exports, and so may whoever names a module at run time.
    for (const Entry& entry : program.entries) {
        if (VariableID exports = program.at(entry.module).exportsVariable)
            locations.note(locations.of(exports), LocationEvent::ReadOutsideOperations);
    }
    // The `exports` of a module object shows the variable: whoever holds the object reads and stores the variable through it.
    const ValueFlowUniverse& universe = inputs.universe();
    inputs.sets().forEachMember(inputs.solver().valueOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ModuleObjectsHandedOut)), [&](ValueFlowObjectID moduleObject) {
        ModuleID module = universe.moduleOf(moduleObject);
        if (VariableID exports = module ? program.at(module).exportsVariable : VariableID())
            noteEverythingOutsideOperations(locations, locations.of(exports));
    });
}

// The one operation that stores to a location runs at most once if it lies outside every cycle of a function that does.
static BitVector findLocationsStoredAtMostOnce(const ValueFlowResultInputs& inputs, std::span<const FunctionFlow> functions, const Locations& locations)
{
    const Program& program = inputs.program;
    BitVector storedAtMostOnce(locations.size());
    Vector<uint32_t> found = collectInParallel<uint32_t>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        const Function& function = program.functions[index];
        if (functions[index].executions != ExecutionCount::AtMostOnce || !inputs.isReached(FunctionID(index)))
            return;
        CFG cfg(function);
        BlockCycles cycles(cfg);
        for (BlockID block : cfg.reversePostOrder()) {
            if (cycles.isInCycle(block))
                continue;
            for (ValueID id : function.at(block).values) {
                forEachLocationStoredBy(program, locations, function.at(id), [&](uint32_t location) {
                    if (locations.numberOfStoresOf(location) == 1)
                        found.append(location);
                });
            }
        }
    });
    for (uint32_t location : found)
        storedAtMostOnce.quickSet(location);
    return storedAtMostOnce;
}

namespace {

struct LocationFlows {
    const ValueFlowResultInputs& inputs;
    const Locations& locations;
    const BitVector& storedAtMostOnce;

    // The subject of a switch is the variable, or the layout of the field.
    LocationFlow flowOf(uint32_t location, uint32_t switchSubject) const
    {
        OptionSet<LocationEvent> events = locations.eventsOf(location);
        LocationFlow flow;
        if (!inputs.build.isEnabled(FlowResultKind::StoreCounts, switchSubject))
            flow.cause = UnprovenCause::ResultDisabled;
        else if (!events.contains(LocationEvent::WrittenOutsideOperations)) {
            flow.writers = WriterState::AllKnown;
            if (!events.contains(LocationEvent::StoredThroughObject))
                flow.storeNaming = StoreNaming::EveryStoreNamesLocation;
            if (!locations.numberOfStoresOf(location))
                flow.stores = StoreCount::Never;
            else if (storedAtMostOnce.quickGet(location))
                flow.stores = StoreCount::AtMostOnce;
            flow.isUnchangedDuringActivation = flow.stores == StoreCount::Never;
        } else
            flow.cause = UnprovenCause::UnknownWriter;
        bool isReadAsValue = events.containsAny({ LocationEvent::ReadAsValue, LocationEvent::ReadOutsideOperations });
        if (!isReadAsValue && inputs.build.isEnabled(FlowResultKind::ObjectUses, switchSubject))
            flow.reads = events.contains(LocationEvent::ReadAsCallee) ? ReadState::OnlyCalled : ReadState::NeverRead;
        return flow;
    }
};

} // anonymous namespace

struct ValueFlowOperationsOnLocations {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowOperationsOnLocations);

    explicit ValueFlowOperationsOnLocations(const Program& program)
        : locations(program)
    {
    }

    Locations locations;
};

void ValueFlowOperationsOnLocationsDeleter::operator()(ValueFlowOperationsOnLocations* operations) const
{
    delete operations;
}

std::unique_ptr<ValueFlowOperationsOnLocations, ValueFlowOperationsOnLocationsDeleter> noteOperationsOnLocations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    std::unique_ptr<ValueFlowOperationsOnLocations, ValueFlowOperationsOnLocationsDeleter> operations(new ValueFlowOperationsOnLocations(program));
    Locations& locations = operations->locations;
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        // What such code stores the solve has not followed, and how often it stores nobody has counted.
        if (isUnsolvedCode(inputs, tables, id))
            noteOperationsOfUnsolvedCode(program, program.functions[index], locations);
        else if (inputs.isReached(id))
            noteOperationsOf(program, program.functions[index], locations);
    });
    return operations;
}

void computeLocationResults(const ValueFlowResultInputs& inputs, ValueFlowOperationsOnLocations& operations, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;

    Locations& locations = operations.locations;
    noteEvalScopes(inputs, locations);
    for (uint32_t index = 0; index < program.modules.size(); ++index)
        tables.wholeProgram.modules[index].namespaceUse = inputs.variablesOnObjects->namespaceUses[index];
    noteModules(inputs, tables.wholeProgram.modules.span(), locations);
    noteAccessesThroughObjects(inputs, locations);
    noteStaticFields(inputs, locations);

    BitVector storedAtMostOnce = findLocationsStoredAtMostOnce(inputs, tables.functions.functions.span(), locations);
    LocationFlows flows { inputs, locations, storedAtMostOnce };

    forEachIndexInParallel(inputs.loops(), program.variables.size(), [&](unsigned index) {
        LocationFlow& flow = application.variables[index].flow;
        flow = flows.flowOf(locations.of(VariableID(index)), index);
        if (flow.cause == UnprovenCause::UnknownWriter)
            flow.cause = causeOfUnknownWriter(*inputs.variablesOnObjects, index);
    });
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!program.layouts[index].isEnvironment())
            return;
        Vector<FlowFieldUpdate>& fields = application.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field)
            fields[field].flow.location = flows.flowOf(locations.of(id, field), index);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
