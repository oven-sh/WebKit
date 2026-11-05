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

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

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

JS_EXPORT_PRIVATE void printOriginOutsideOperations(PrintStream&, uint32_t origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
