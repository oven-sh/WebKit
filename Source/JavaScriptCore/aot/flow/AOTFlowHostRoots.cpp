/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static void enterHost(FlowSolver& solver)
{
    FlowUnseenCodeState& state = solver.callState().unseenCode;
    state.hostInstance = solver.enter(solver.graph().hostFunction(), FlowContext::Proven);
    state.nativeCodeStatement = { FlowStatementKind::UnseenCodeEvent, 0 };
}

static void noteHostReads(FlowSolver& solver, const Entry& entry)
{
    const Module& module = solver.program().at(entry.module);
    for (const HostRead& read : entry.hostReads) {
        auto found = std::ranges::find(module.exports, read.exportName, &ModuleExport::name);
        if (found == module.exports.end() || found->kind != ModuleExportKind::Variable)
            continue;
        solver.noteLocationEvent(solver.graph().variable(found->variable), FlowLocationEvent::ReadByUnknownCode);
    }
}

static void evaluateEntries(FlowSolver& solver)
{
    for (const Entry& entry : solver.program().entries) {
        didRequestModuleOfProgram(solver, entry.module);
        evaluateModule(solver, entry.module, noWitness);
        noteHostReads(solver, entry);
    }
}

void addRoots(FlowSolver& solver)
{
    enterHost(solver);
    if (solver.build().options().admitsUnseenCode())
        raiseUnseenCode(solver, UnseenCodeCause::OptionAdmitsCode, BuiltinID(), noWitness);

    evaluateEntries(solver);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
