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
    // By something that is no operation of a function that the solve has reached: evaluated text, the loader, a store to the property of a class object, code that nobody has solved.
    ReadOutsideOperations = 1 << 2,
    WrittenOutsideOperations = 1 << 3,
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
    bool readsLocation = std::ranges::any_of(function.values, [&](const Value& value) {
        return locationReadBy(locations, value).has_value();
    });
    std::optional<CFG> cfg;
    std::optional<UseLists> useLists;
    if (readsLocation) {
        cfg.emplace(function);
        useLists.emplace(function, *cfg);
    }

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (std::optional<uint32_t> location = locationReadBy(locations, value))
                locations.note(*location, readEventOf(function, *useLists, id));
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

static bool hasNamespaceOfProgram(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Code that names a module at run time gets the namespace object of any module.
static void findNamespacesNeverCreated(const ValueFlowResultInputs& inputs, Vector<ModuleFlow>& modules)
{
    if (unseenCodeCanRun(inputs.solution) || modulesAreLoadedByName(inputs.solution))
        return;
    ValueSetID created = inputs.solver().valueOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::CreatedNamespaces));
    for (uint32_t index = 0; index < modules.size(); ++index) {
        if (!hasNamespaceOfProgram(inputs.program.modules[index].kind) || !inputs.build.isEnabled(FlowResultKind::ObjectUses, index))
            continue;
        if (!inputs.sets().contains(created, inputs.universe().namespaceOf(ModuleID(index))))
            modules[index].namespaceUse = NamespaceUse::NeverCreated;
    }
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

    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        bool hasCode = module.body && program.at(module.body).hasBody();
        // Native code reads exports of a module of the library by name, and a require that answers the default export reads its variable. Neither creates the namespace object.
        bool isReadWithoutNamespace = module.kind == ModuleKind::RuntimeSource || module.requireResult == RequireResult::DefaultExport;
        forEachExportedVariable(module, [&](VariableID variable) {
            if (hasExportsReadByName.quickGet(index) || isReadWithoutNamespace || modules[index].namespaceUse != NamespaceUse::NeverCreated)
                locations.note(locations.of(variable), LocationEvent::ReadOutsideOperations);
            // The loader fills the exports of a module without code.
            if (!hasCode)
                locations.note(locations.of(variable), LocationEvent::WrittenOutsideOperations);
        });
        for (const SyntheticExport& entry : module.syntheticExports) {
            if (entry.variable)
                noteEverythingOutsideOperations(locations, locations.of(entry.variable));
        }
    }
}

// The one operation that stores to a location runs at most once if it lies outside every cycle of a function that does.
static BitVector findLocationsStoredAtMostOnce(const ValueFlowResultInputs& inputs, std::span<const FunctionFlow> functions, const Locations& locations)
{
    const Program& program = inputs.program;
    BitVector storedAtMostOnce(locations.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (functions[index].executions != ExecutionCount::AtMostOnce || !inputs.isReached(FunctionID(index)))
            continue;
        CFG cfg(function);
        BlockCycles cycles(cfg);
        for (BlockID block : cfg.reversePostOrder()) {
            if (cycles.isInCycle(block))
                continue;
            for (ValueID id : function.at(block).values) {
                forEachLocationStoredBy(program, locations, function.at(id), [&](uint32_t location) {
                    if (locations.numberOfStoresOf(location) == 1)
                        storedAtMostOnce.quickSet(location);
                });
            }
        }
    }
    return storedAtMostOnce;
}

// Whoever has read a function may call it: a lookup through the unknown object is a read of what it finds, and no call lists that.
static constexpr OptionSet<BuiltinUse> usesThatMayCall { BuiltinUse::Read, BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped };

// Only a weak reference shows when an object dies. Without one no program can tell whether a location that nobody reads has kept an object alive.
static bool canObserveLifetimes(const ValueFlowResultInputs& inputs)
{
    const BuiltinTable& builtins = inputs.program.builtins();
    if (unseenCodeCanRun(inputs.solution))
        return true;
    BuiltinID takingTargets[] { builtins.canonicalID(Builtins::WeakRef), builtins.canonicalID(Builtins::FinalizationRegistryPrototypeRegister) };
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        if (inputs.solver().usesOf(builtin).containsAny(usesThatMayCall) && std::ranges::contains(takingTargets, builtins.canonicalID(builtin)))
            return true;
    }
    return false;
}

namespace {

struct LocationFlows {
    const ValueFlowResultInputs& inputs;
    const Locations& locations;
    const BitVector& storedAtMostOnce;
    ValueSetID valuesWithObservedLifetime;

    // The subject of a switch is the variable, or the layout of the field.
    LocationFlow flowOf(uint32_t location, ValueFlowNodeID node, uint32_t switchSubject) const
    {
        OptionSet<LocationEvent> events = locations.eventsOf(location);
        LocationFlow flow;
        flow.mayKeepObservedObjectAlive = inputs.sets().meet(inputs.solver().valueOf(node), valuesWithObservedLifetime);
        if (!inputs.build.isEnabled(FlowResultKind::StoreCounts, switchSubject))
            flow.cause = UnprovenCause::ResultDisabled;
        else if (!events.contains(LocationEvent::WrittenOutsideOperations)) {
            flow.writers = WriterState::AllKnown;
            flow.storeNaming = StoreNaming::EveryStoreNamesLocation;
            if (!locations.numberOfStoresOf(location))
                flow.stores = StoreCount::Never;
            else if (storedAtMostOnce.quickGet(location))
                flow.stores = StoreCount::AtMostOnce;
            flow.isUnchangedDuringActivation = flow.stores == StoreCount::Never;
        } else
            flow.cause = UnprovenCause::UnknownWriter;
        bool isReadAsValue = events.containsAny({ LocationEvent::ReadAsValue, LocationEvent::ReadOutsideOperations });
        if (!isReadAsValue && !flow.mayKeepObservedObjectAlive && inputs.build.isEnabled(FlowResultKind::ObjectUses, switchSubject))
            flow.reads = events.contains(LocationEvent::ReadAsCallee) ? ReadState::OnlyCalled : ReadState::NeverRead;
        return flow;
    }
};

} // anonymous namespace

void computeLocationResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowProgramNodes& nodes = inputs.bodies().nodes();
    FlowApplication& application = *tables.application;

    Locations locations(program);
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        if (inputs.isReached(FunctionID(index)))
            noteOperationsOf(program, program.functions[index], locations);
        else if (isUnsolvedCode(inputs, tables, FunctionID(index)))
            noteOperationsOfUnsolvedCode(program, program.functions[index], locations);
    });
    noteEvalScopes(inputs, locations);
    findNamespacesNeverCreated(inputs, tables.wholeProgram.modules);
    noteModules(inputs, tables.wholeProgram.modules.span(), locations);
    // A static field is a variable and a property of the class object at once.
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        if (program.variables[index].owner)
            noteEverythingOutsideOperations(locations, locations.of(VariableID(index)));
    }

    BitVector storedAtMostOnce = findLocationsStoredAtMostOnce(inputs, tables.functions.functions.span(), locations);
    // A weak reference takes an object or a symbol.
    ValueSetMaker maker(inputs.bodies().sets());
    ValueSetID symbols = maker.unite(maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Symbol)), inputs.universe().keys());
    LocationFlows flows { inputs, locations, storedAtMostOnce, canObserveLifetimes(inputs) ? maker.unite(inputs.universe().objects(), symbols) : ValueSets::nothing };

    forEachIndexInParallel(inputs.loops(), program.variables.size(), [&](unsigned index) {
        application.variables[index].flow = flows.flowOf(locations.of(VariableID(index)), nodes.variable(VariableID(index)), index);
    });
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        LayoutID id(index);
        if (!program.layouts[index].isEnvironment())
            return;
        Vector<FlowFieldUpdate>& fields = application.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field)
            fields[field].flow.location = flows.flowOf(locations.of(id, field), nodes.environmentField(id, field), index);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
