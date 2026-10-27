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
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueIdentity.h"
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class LocationEvent : uint8_t {
    ReadAsValue = 1 << 0,
    ReadAsCallee = 1 << 1,
    ReadOutsideOperations = 1 << 2,
    WrittenOutsideOperations = 1 << 3,
    StoredThroughObject = 1 << 4,
};

} // anonymous namespace

struct ValueFlowOperationsOnLocations {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowOperationsOnLocations);

    explicit ValueFlowOperationsOnLocations(const Program& program)
    {
        uint32_t next = program.variables.size();
        firstFieldOfLayout = program.layouts.map([&](const Layout& layout) {
            uint32_t first = next;
            next += layout.isEnvironment() ? layout.fields.size() : 0;
            return first;
        });
        events.fill(0, next);
        numberOfStores.fill(0, next);
    }

    uint32_t of(VariableID variable) const { return variable.index(); }
    uint32_t of(LayoutID environment, uint32_t field) const { return firstFieldOfLayout[environment.index()] + field; }

    void note(uint32_t location, LocationEvent event) { AtomicRef(events[location]).fetch_or(static_cast<uint8_t>(event), std::memory_order_relaxed); }
    void noteStore(uint32_t location) { AtomicRef(numberOfStores[location]).fetch_add(1, std::memory_order_relaxed); }
    void noteEverythingOutsideOperations(uint32_t location)
    {
        note(location, LocationEvent::ReadOutsideOperations);
        note(location, LocationEvent::WrittenOutsideOperations);
    }

    Vector<uint32_t> firstFieldOfLayout;
    Vector<uint8_t> events;
    Vector<uint32_t> numberOfStores;
};
using Locations = ValueFlowOperationsOnLocations;

void ValueFlowOperationsOnLocationsDeleter::operator()(ValueFlowOperationsOnLocations* operations) const
{
    delete operations;
}

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

template<typename Read>
static void forEachEventOfReads(const Function& function, std::span<const Read> reads, const auto& siteOf, const auto& note)
{
    if (function.role == FunctionRole::Model) {
        for (const Read& read : reads)
            note(read, LocationEvent::ReadAsValue);
        return;
    }

    Vector<ValueID, 128> readBehind(function.values.size());
    BitVector isUsed(function.values.size());
    BitVector isReadAsValue(function.values.size());
    Vector<ValueID, 16> casts;
    for (const Read& read : reads)
        readBehind[siteOf(read).index()] = siteOf(read);
    auto noteUsesAsValues = [&](std::span<const ValueID> used) {
        for (ValueID value : used) {
            if (ValueID read = value ? readBehind[value.index()] : ValueID())
                isReadAsValue.quickSet(read.index());
        }
    };
    auto visit = [&](const Block& block) {
        if (block.subject)
            noteUsesAsValues(singleElementSpan(block.subject));
        if (block.handler)
            noteUsesAsValues(block.handler->arguments.span());
        for (ValueID user : block.values) {
            Opcode opcode = function.at(user).opcode;
            std::span<const ValueID> operands = function.at(user).operands();
            for (uint32_t index = 0; index < operands.size(); ++index) {
                ValueID read = operands[index] ? readBehind[operands[index].index()] : ValueID();
                if (!read)
                    continue;
                isUsed.quickSet(operands[index].index());
                OperandUses operandUses = operandUsesOf(opcode, index);
                if (isCast(opcode) && operandUses.contains(OperandUse::Returns)) {
                    RELEASE_ASSERT(!readBehind[user.index()] || readBehind[user.index()] == read);
                    if (!readBehind[user.index()])
                        casts.append(user);
                    readBehind[user.index()] = read;
                    continue;
                }
                if (!operandUses.contains(OperandUse::Calls) || operandUses.containsAny({ OperandUse::Retains, OperandUse::Returns }))
                    isReadAsValue.quickSet(read.index());
            }
        }
        noteUsesAsValues(block.terminator.operands());
        for (const Target& target : block.terminator.targets)
            noteUsesAsValues(target.arguments.span());
    };
    if (function.blocks.size() == 1)
        visit(function.blocks[0]);
    else {
        CFG cfg(function);
        for (BlockID block : cfg.reversePostOrder())
            visit(function.at(block));
    }
    for (ValueID cast : casts) {
        if (!isUsed.quickGet(cast.index()))
            isReadAsValue.quickSet(readBehind[cast.index()].index());
    }
    for (const Read& read : reads)
        note(read, isReadAsValue.quickGet(siteOf(read).index()) || !isUsed.quickGet(siteOf(read).index()) ? LocationEvent::ReadAsValue : LocationEvent::ReadAsCallee);
}

static void noteOperationsOf(const Program& program, const Function& function, Locations& locations)
{
    Vector<ValueID, 32> reads;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (locationReadBy(locations, value))
                reads.append(id);
            if (VariableID exports = exportsVariableReadBy(program, value))
                locations.note(locations.of(exports), LocationEvent::ReadOutsideOperations);
            forEachLocationStoredBy(program, locations, value, [&](uint32_t location) {
                locations.noteStore(location);
            });
        }
    }
    if (reads.isEmpty())
        return;
    forEachEventOfReads(function, reads.span(), std::identity { }, [&](ValueID read, LocationEvent event) {
        locations.note(*locationReadBy(locations, function.at(read)), event);
    });
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

static void noteEvalScopes(const Program& program, Locations& locations)
{
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                locations.noteEverythingOutsideOperations(locations.of(binding.variable));
            if (binding.layout && binding.field && program.at(binding.layout).isEnvironment())
                locations.noteEverythingOutsideOperations(locations.of(binding.layout, *binding.field));
        }
    }
}

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

    AOT::sort(reads, { }, &ValueFlowAccessThroughObject::function);
    for (size_t first = 0, end = 0; first < reads.size(); first = end) {
        FunctionID id = reads[first].function;
        while (end < reads.size() && reads[end].function == id)
            ++end;
        forEachEventOfReads(inputs.program.at(id), reads.span().subspan(first, end - first), [](const ValueFlowAccessThroughObject& read) {
            return read.site;
        }, [&](const ValueFlowAccessThroughObject& read, LocationEvent event) {
            locations.note(locations.of(read.variable), event);
        });
    }
}

static void noteStaticFields(const ValueFlowVariablesOnObjects& variablesOnObjects, Locations& locations)
{
    for (size_t index : variablesOnObjects.staticFieldsInUnknownHands)
        locations.noteEverythingOutsideOperations(locations.of(VariableID(static_cast<uint32_t>(index))));
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

static void noteModules(const ValueFlowResultInputs& inputs, std::span<const ModuleFlow> modules, Locations& locations)
{
    const Program& program = inputs.program;
    BitVector hasExportsReadByName(program.modules.size());
    for (const Entry& entry : program.entries) {
        if (!entry.hostReads.isEmpty())
            hasExportsReadByName.quickSet(entry.module.index());
    }
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

    BuiltinName moduleExportsName = program.builtins().nameOf(byteCast<uint8_t>("module.exports"_s.span()));
    auto isReadByRequire = [&](const ModuleExport& entry) {
        BuiltinName name = program.builtinName(entry.name);
        return name && (name == BuiltinNames::__esModule || name == moduleExportsName);
    };

    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        bool hasCode = module.body && program.at(module.body).hasBody();
        bool isEveryExportRead = hasExportsReadByName.quickGet(index) || module.kind == ModuleKind::RuntimeSource || module.requireResult == RequireResult::DefaultExport || modules[index].namespaceUse == NamespaceUse::Unknown;
        for (const ModuleExport& entry : module.exports) {
            if (entry.kind != ModuleExportKind::Variable || !entry.variable)
                continue;
            if (isEveryExportRead || (module.requireResult == RequireResult::Namespace && isReadByRequire(entry)))
                locations.note(locations.of(entry.variable), LocationEvent::ReadOutsideOperations);
            if (!hasCode)
                locations.note(locations.of(entry.variable), LocationEvent::WrittenOutsideOperations);
        }
        for (const SyntheticExport& entry : module.syntheticExports) {
            if (entry.variable)
                locations.noteEverythingOutsideOperations(locations.of(entry.variable));
        }
        if (ModuleID captured = commonJsModuleCapturedBy(program, module))
            locations.note(locations.of(program.at(captured).exportsVariable), LocationEvent::ReadOutsideOperations);
    }

    for (const Entry& entry : program.entries) {
        if (VariableID exports = program.at(entry.module).exportsVariable)
            locations.note(locations.of(exports), LocationEvent::ReadOutsideOperations);
    }
    const ValueFlowUniverse& universe = inputs.universe();
    inputs.sets().forEachMember(inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::ModuleObjectsHandedOut)), [&](ValueFlowObjectID moduleObject) {
        ModuleID module = universe.moduleOf(moduleObject);
        if (VariableID exports = module ? program.at(module).exportsVariable : VariableID())
            locations.noteEverythingOutsideOperations(locations.of(exports));
    });
}

// A read that names the variable finds it without the namespace object.
static void listExportsReadByName(const ValueFlowResultInputs& inputs, const Locations& locations, std::span<ModuleFlow> modules)
{
    const Program& program = inputs.program;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (modules[index].namespaceUse != NamespaceUse::NamedReadsOnly)
            continue;
        Vector<VariableID> listed;
        for (const ModuleExport& entry : program.modules[index].exports) {
            if (entry.kind != ModuleExportKind::Variable || !entry.variable)
                continue;
            bool isReadByHost = OptionSet<LocationEvent>::fromRaw(locations.events[locations.of(entry.variable)]).contains(LocationEvent::ReadOutsideOperations);
            if (isReadByHost || !inputs.variablesOnObjects->accessesTo(entry.variable).empty())
                listed.append(entry.variable);
        }
        AOT::sort(listed);
        removeRepeatedElements(listed);
        modules[index].exportsReadByName = WTF::move(listed);
    }
}

static BitVector findLocationsStoredAtMostOnce(const ValueFlowResultInputs& inputs, std::span<const FunctionFlow> functions, const Locations& locations)
{
    const Program& program = inputs.program;
    BitVector storedAtMostOnce(locations.events.size());
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
                    if (locations.numberOfStores[location] == 1)
                        found.append(location);
                });
            }
        }
    });
    for (uint32_t location : found)
        storedAtMostOnce.quickSet(location);
    return storedAtMostOnce;
}

std::unique_ptr<ValueFlowOperationsOnLocations, ValueFlowOperationsOnLocationsDeleter> noteOperationsOnLocations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    std::unique_ptr<ValueFlowOperationsOnLocations, ValueFlowOperationsOnLocationsDeleter> locations(new ValueFlowOperationsOnLocations(program));
    forEachIndexInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        if (isUnsolvedCode(inputs, tables, id))
            noteOperationsOfUnsolvedCode(program, program.functions[index], *locations);
        else if (inputs.isReached(id))
            noteOperationsOf(program, program.functions[index], *locations);
    });
    return locations;
}

void computeLocationResults(const ValueFlowResultInputs& inputs, ValueFlowOperationsOnLocations& locations, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;

    noteEvalScopes(program, locations);
    for (uint32_t index = 0; index < program.modules.size(); ++index)
        tables.wholeProgram.modules[index].namespaceUse = inputs.variablesOnObjects->namespaceUses[index];
    noteModules(inputs, tables.wholeProgram.modules.span(), locations);
    noteAccessesThroughObjects(inputs, locations);
    noteStaticFields(*inputs.variablesOnObjects, locations);
    listExportsReadByName(inputs, locations, tables.wholeProgram.modules.mutableSpan());

    BitVector storedAtMostOnce = findLocationsStoredAtMostOnce(inputs, tables.functions.functions.span(), locations);
    auto flowOf = [&](uint32_t location, uint32_t switchSubject) {
        auto events = OptionSet<LocationEvent>::fromRaw(locations.events[location]);
        LocationFlow flow;
        if (!inputs.build.isEnabled(FlowResultKind::StoreCounts, switchSubject))
            flow.cause = UnprovenCause::ResultDisabled;
        else if (!events.contains(LocationEvent::WrittenOutsideOperations)) {
            flow.writers = WriterState::AllKnown;
            if (!events.contains(LocationEvent::StoredThroughObject))
                flow.storeNaming = StoreNaming::EveryStoreNamesLocation;
            if (!locations.numberOfStores[location])
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
    };

    forEachIndexInParallel(inputs.loops(), program.variables.size(), [&](unsigned index) {
        LocationFlow& flow = application.variables[index].flow;
        flow = flowOf(locations.of(VariableID(index)), index);
        if (flow.cause == UnprovenCause::UnknownWriter)
            flow.cause = causeOfUnknownWriter(*inputs.variablesOnObjects, index);
    });
    forEachIndexInParallel(inputs.loops(), program.layouts.size(), [&](unsigned index) {
        if (!program.layouts[index].isEnvironment())
            return;
        Vector<FlowFieldUpdate>& fields = application.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field)
            fields[field].flow.location = flowOf(locations.of(LayoutID(index), field), index);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
