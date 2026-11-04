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
    // Encoded.
    Machine::Procedure& procedure;
    const StackCheckState& stackChecks;
    Vector<Relocation> relocations;
};

// Everything that is left of the unit: AOTCompiledCode.h. It empties the records.
CompiledCode describeCode(BuildContext&, const CodeGenerationInputs&, const Code&, CodeRecords&, EmittedCode&&);

// BytesOutsideOperations::origin in two words: `Reload Edge` | `End Return` | `Other Nowhere`.
JS_EXPORT_PRIVATE void printOriginOutsideOperations(PrintStream&, uint32_t origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
