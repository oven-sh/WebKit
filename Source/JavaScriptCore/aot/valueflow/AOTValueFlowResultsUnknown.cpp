/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowRecords.h"
#include "AOTFlowStamps.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static AccessKind accessKindOf(const Value& value)
{
    switch (value.opcode) {
    case Opcode::PropGet:
    case Opcode::ElemGet:
    case Opcode::SuperGet:
    case Opcode::PrivateGet:
        return AccessKind::Read;
    case Opcode::PropSet:
    case Opcode::ElemSet:
    case Opcode::SuperSet:
    case Opcode::PrivateSet:
        return AccessKind::Store;
    case Opcode::PropDefine:
    case Opcode::ElemDefine:
    case Opcode::PrivateDefine:
        return AccessKind::Define;
    case Opcode::PropHas:
    case Opcode::ElemHas:
    case Opcode::PrivateHas:
        return AccessKind::Has;
    case Opcode::PropDelete:
    case Opcode::ElemDelete:
        return AccessKind::Delete;
    case Opcode::Call:
    case Opcode::CallList:
        return AccessKind::Call;
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        return AccessKind::Construct;
    case Opcode::CallDirect:
        return value.isConstruction() ? AccessKind::Construct : AccessKind::Call;
    default:
        return AccessKind::Unknown;
    }
}

static void fillAccessSites(const Program& program, FlowAccessSiteNumbering& numbering, const ParallelLoops& loops, FlowSiteTables& sites)
{
    sites.accessSites.grow(numbering.numberOfSites());
    numbering.valueOfSite.grow(numbering.numberOfSites());
    sites.callsOfBoundFunctions.fill(false, numbering.numberOfSites());
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
            AccessSiteID site = numbering.siteOf(id, ordinal);
            numbering.valueOfSite[site.index()] = value;
            sites.accessSites[site.index()] = AccessSite { id, function.locationOf(value), accessKindOf(function.at(value)) };
            sites.callsOfBoundFunctions[site.index()] = PayloadWord::callees(function.at(value).opcode).has_value();
        });
    });
}

static void fillApplication(const Program& program, BuildContext& build, FlowApplication& application)
{
    application.functionCountAsRead = program.functions.size();
    runTasksInParallel(build.parallelLoops(), {
        [&] {
            application.bodies = Vector<FlowBodyUpdate>(program.functions.size());
        },
        [&] {
            application.accessSites = numberAccessSites(program, build);
        },
        [&] {
            application.signatures = Vector<FlowSignatureUpdate>(program.functions.size());
        },
        [&] {
            application.layouts = Vector<FlowLayoutUpdate>(program.layouts.size());
            forEachIndexInParallel(build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
                const Layout& layout = program.layouts[index];
                FlowLayoutUpdate& update = application.layouts[index];
                update.flow = layout.flow;
                update.classFlow = layout.classFlow;
                update.fields = Vector<FlowFieldUpdate>(layout.fields.size(), [&](size_t field) {
                    return FlowFieldUpdate { { }, layout.fields[field].flow };
                });
            });
        },
        [&] {
            application.variables = Vector<FlowVariableUpdate>(program.variables.size(), [&](size_t index) {
                return FlowVariableUpdate { { }, program.variables[index].flow };
            });
        },
    });
}

static Vector<BuiltinUseRecord> listEveryUseOfEveryBuiltin(const BuiltinTable& builtins)
{
    return Vector<BuiltinUseRecord>(builtins.builtinCount(), [](size_t index) {
        return BuiltinUseRecord { static_cast<uint32_t>(index), static_cast<BuiltinUse>(everyFlagOf<BuiltinUse>().toRaw()), noWitness };
    });
}

static Vector<FeatureUseRecord> listEveryFeatureOfBuiltins(const BuiltinTable& builtins)
{
    Vector<uint32_t> features;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        for (const BuiltinFeatureUse& line : builtins.featureUsesOf(BuiltinID(index)))
            features.append(static_cast<uint32_t>(line.feature));
    }
    AOT::sort(features);
    removeRepeatedElements(features);
    return features.map([](uint32_t feature) {
        return FeatureUseRecord { .feature = feature };
    });
}

FlowResultTables createUnknownResultTables(const Program& program, BuildContext& build)
{
    FlowResultTables tables;
    tables.program = &program;
    tables.application = std::unique_ptr<FlowApplication, FlowApplicationDeleter>(new FlowApplication);
    tables.functions.functionCountAsRead = program.functions.size();
    runTasksInParallel(build.parallelLoops(), {
        [&] {
            fillApplication(program, build, *tables.application);
        },
        [&] {
            tables.functions.functions = Vector<FunctionFlow>(program.functions.size());
        },
        [&] {
            tables.functions.parameterSummaries = Vector<Vector<OperandSummary>>(program.functions.size());
        },
    });
    fillAccessSites(program, tables.application->accessSites, build.parallelLoops(), tables.sites);

    tables.builtins.uses = listEveryUseOfEveryBuiltin(program.builtins());
    tables.builtins.featureUses = listEveryFeatureOfBuiltins(program.builtins());
    tables.builtins.localeUses.append(FlowLocaleUse { });
    tables.wholeProgram.modules = Vector<ModuleFlow>(program.modules.size());
    // The one cause that opens the whole program: text is evaluated, modules are loaded by name, the loader's answers are replaced. The tables stay as they are only in a build with such an option.
    tables.wholeProgram.unseenCode.append(UnseenCodeRecord { .cause = UnseenCodeCause::OptionAdmitsCode });
    return tables;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
