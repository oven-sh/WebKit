/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeGenerator.h"
#include "AOTCompiledCode.h"
#include "AOTMachine.h"

namespace JSC { namespace AOT { namespace Typed {

// What a thread of Compile owns. The arena is reset in front of each unit.
struct CodeGenerationWorker {
    BuildContext& build;
    TypeArena& types;
    Machine::Arena& arena;
};

// Emit, allocate, encode, describe: from swept Code to bytes, on the calling thread. A pure function of (code, inputs).
// Empty after a failure, which the build's diagnostics hold.
CompiledCode generateCode(const CodeGenerationWorker&, const CodeGenerationInputs&, const Code&, CodeGenerationTotals&);

// AOTStraightEmitter.h
class BodyIndex;
bool hasStraightForm(const CodeGenerationInputs&, CodeUnit, const BodyIndex&);
// The Code names the unit and holds nothing. Nothing: some operation turned out to have no straight form, and the unit goes the other way.
std::optional<CompiledCode> generateStraightCode(const CodeGenerationWorker&, const CodeGenerationInputs&, const Code&, const BodyIndex&, CodeGenerationTotals&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
