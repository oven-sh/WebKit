/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallOperations.h"

#if ENABLE(AOT)

#include "AOTFlowEdgesAssembler.h"
#include "AOTFlowNamespaces.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static void addHoistedFunctions(const Program& program, FlowSummaryAssembler& assembler)
{
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.initialization != VariableInitialization::Function || !variable.function)
            continue;

        uint32_t site = assembler.addSite({
            .kind = FlowSiteKind::Closure,
            .origin = FlowSiteOrigin::Operation,
            .closureFunction = variable.function,
            .ordinal = index,
        });
        FlowLocalID closure = assembler.addTemporary();
        assembler.seed(FlowSeed::site(closure, site));
        assembler.add(FlowAllocation { ValueID(), site, FlowLocalID(), false });

        FlowLocalID location = assembler.external(FlowExternalNode::variable(VariableID(index)));
        assembler.copy(closure, location);
        assembler.add(FlowLocationAccess { FlowAccessKind::Store, FlowReadUse::Value, false, ValueID(), location, closure });
        assembler.addClosureCreation({ closure, ValueID(), variable.function, site, FlowClosureUse::StoredOnce, FlowClosureCapture::Nothing });
    }
}

static void addEscapingNamespace(FlowSummaryAssembler& assembler, ModuleID module)
{
    FlowLocalID namespaceObject = assembler.addTemporary();
    assembler.seed(FlowSeed::moduleNamespace(namespaceObject, module));
    assembler.copy(namespaceObject, assembler.external(FlowExternalNode::nativeCode()));
}

static FlowLocalID addExportRead(const Program& program, FlowSummaryAssembler& assembler, const ModuleExport& entry, bool usesOfModulesCount)
{
    FlowLocalID value = assembler.addTemporary();
    switch (entry.kind) {
    case ModuleExportKind::Variable: {
        if (usesOfModulesCount)
            assembler.add(FlowModuleOperation { .kind = FlowModuleOperationKind::Use, .module = program.at(entry.variable).module });
        FlowLocalID location = assembler.external(FlowExternalNode::variable(entry.variable));
        assembler.copy(location, value);
        assembler.add(FlowLocationAccess { FlowAccessKind::Load, FlowReadUse::Value, false, ValueID(), location, value });
        return value;
    }
    case ModuleExportKind::Namespace:
        switch (entry.phase) {
        case ImportPhase::Evaluation:
            assembler.seed(FlowSeed::moduleNamespace(value, entry.module));
            return value;
        case ImportPhase::Defer:
            // The deferred namespace object is not tracked, so what it serves escapes.
            addEscapingNamespace(assembler, entry.module);
            assembler.seed(FlowSeed::unknown(value));
            return value;
        }
        break;
    case ModuleExportKind::Indirect:
        assembler.seed(FlowSeed::unknown(value));
        return value;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowLocalID addModuleValueRead(const Program& program, FlowSummaryAssembler& assembler, ModuleID module, StringID name)
{
    FlowLocalID moduleValue = assembler.addTemporary();
    assembler.add(FlowModuleOperation { .kind = FlowModuleOperationKind::Require, .module = module, .result = moduleValue });

    FlowLocalID value = assembler.addTemporary();
    bool isDefault = name == program.stringOf(BuiltinNames::defaultKeyword);
    if (isDefault)
        assembler.copy(moduleValue, value);
    if (!isDefault || program.at(module).exportRule != SyntheticExportRule::CommonJsIgnoringAnnotation) {
        FlowLocalID key = assembler.addTemporary();
        assembler.seed(FlowSeed::string(key, name));
        assembler.add(FlowElementAccess { .kind = FlowAccessKind::Load, .receiver = moduleValue, .key = key, .value = value });
    }
    return value;
}

static FlowLocalID addUnlistedExportRead(FlowSummaryAssembler& assembler, ModuleID module)
{
    addEscapingNamespace(assembler, module);

    FlowLocalID value = assembler.addTemporary();
    assembler.seed(FlowSeed::unknown(value));
    return value;
}

static FlowLocalID addNamedExportRead(const Program& program, FlowSummaryAssembler& assembler, ModuleID id, StringID name, bool usesOfModulesCount)
{
    const Module& module = program.at(id);
    auto found = std::ranges::find(module.exports, name, &ModuleExport::name);
    if (found != module.exports.end())
        return addExportRead(program, assembler, *found, usesOfModulesCount);
    if (module.exportRule != SyntheticExportRule::None)
        return addModuleValueRead(program, assembler, id, name);

    switch (exportListingOf(program, id)) {
    case FlowOwnListing::MayHaveOthers:
        return addUnlistedExportRead(assembler, id);
    case FlowOwnListing::Complete:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addBuiltinCall(const BuiltinTable& builtins, FlowSummaryAssembler& assembler, const HostRead& read, FlowLocalID value)
{
    BuiltinID builtin = builtins.canonicalID(read.builtin);
    FlowLocalID callee = assembler.addTemporary();
    assembler.seed(FlowSeed::builtin(callee, builtin));

    FlowLocalID thisValue;
    BuiltinID holder = builtins.at(builtin).holder;
    if (holder && isObjectKind(builtins.at(holder).kind)) {
        thisValue = assembler.addTemporary();
        assembler.seed(FlowSeed::builtin(thisValue, holder));
    }

    Vector<FlowLocalID, 4> arguments(read.parameter + 1);
    arguments.last() = value;

    FlowLocalID result = assembler.addTemporary();
    assembler.copy(result, assembler.external(FlowExternalNode::nativeCode()));
    assembler.add(FlowCall {
        .mode = FlowCallMode::Call,
        .callee = callee,
        .thisValue = thisValue,
        .arguments = assembler.addList(arguments.span()),
        .result = result,
    });
}

static void addHostReads(const Program& program, FlowSummaryAssembler& assembler, const Entry& entry, bool usesOfModulesCount)
{
    for (const HostRead& read : entry.hostReads) {
        FlowLocalID value = addNamedExportRead(program, assembler, entry.module, read.exportName, usesOfModulesCount);
        if (!value)
            continue;

        if (read.builtin)
            addBuiltinCall(program.builtins(), assembler, read, value);
        else
            assembler.copy(value, assembler.external(FlowExternalNode::nativeCode()));
    }
}

FlowSummary buildHostSummary(const FlowSummaryInputs& inputs)
{
    const Program& program = inputs.program;
    FlowSummaryAssembler assembler({ FunctionID(program.functions.size()), 0, 0, 0 });
    assembler.add(FlowUnseenCodeEvent { ValueID(), UnseenCodeCause::Unknown });

    addHoistedFunctions(program, assembler);
    for (const Entry& entry : program.entries)
        addHostReads(program, assembler, entry, inputs.hasModulesWithoutSideEffects);
    return assembler.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
