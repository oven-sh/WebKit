/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;

// All of it in AOTTestRunsOfProgram.cpp.

// Which modules a mock may stand in the place of, and who may see one. By the records alone, made once a program in time linear in modules + exports + requests.
// A default one says that no module may be mocked.
struct MocksOfProgram {
    // ModuleFlags::MayBeMocked, and the face of a CommonJS file that has it.
    BitVector modulesThatMayBeMocked;
    // By the module whose object is handed on: some module that may be mocked exports it as ModuleExportKind::Namespace.
    BitVector namespacesHandedOnByMockedModules;
    // By module: it may be mocked, or a request of it leads, from request to request, to one that may.
    BitVector modulesThatLeadToMocks;
    bool isAnyModuleMocked { false };
};
MocksOfProgram findMocksOfProgram(const Program&);

// What a read, a call or `in` under Module::exports[indexOfExport] off the namespace object of `exporter` finds may be a mock's. True claims nothing. False: the export is what the records say.
// Variable: the exporter may be mocked, or the variable is another module's and the exporter leads to mocks.
// Namespace: the exporter, the module handed on, or any module that hands it on may be mocked. Indirect: any module may be mocked.
// A name that the module lacks, or any index past the list: the exporter leads to mocks. The names of a mock are those that its factory gives, and a star hands them on.
// IT IS THE ONE QUESTION ABOUT MOCKS for whoever turns `ns.name` into anything else than a read by name. What a spy replaces is not asked here: VariableFlags::IsReplaceable says that.
inline constexpr uint32_t nameThatModuleLacks = UINT32_MAX;
bool mayComeThroughMock(const Program&, const MocksOfProgram&, ModuleID exporter, uint32_t indexOfExport);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
