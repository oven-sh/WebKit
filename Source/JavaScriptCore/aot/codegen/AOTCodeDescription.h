/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTCompiledCode.h"
#include "AOTMachine.h"
#include "AOTRelocation.h"
#include "AOTVector.h"
#include "CCallHelpers.h"
#include "LineColumn.h"
#include <optional>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallingConventions;
class Program;
struct CodeGenerationInputs;
struct CodeRecords;
struct StackCheckState;

struct EmittedCode {
    ImageLinkBuffer& buffer;
    Machine::Procedure& procedure;
    const StackCheckState& stackChecks;
    Vector<Relocation> relocations;
};

CompiledCode describeCode(BuildContext&, const CodeGenerationInputs&, const Code&, CodeRecords&, EmittedCode&&);
std::optional<LineColumn> positionInBuiltin(const Program&, const CallingConventions&, FunctionID shown);

JS_EXPORT_PRIVATE void printOriginOutsideOperations(PrintStream&, uint32_t origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
