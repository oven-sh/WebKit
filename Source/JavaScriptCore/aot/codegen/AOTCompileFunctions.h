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

struct CodeGenerationWorker {
    BuildContext& build;
    TypeArena& types;
    Machine::Arena& arena;
};

CompiledCode generateCode(const CodeGenerationWorker&, const CodeGenerationInputs&, const Code&, CodeGenerationTotals&);

class BodyIndex;
bool hasStraightForm(const CodeGenerationInputs&, CodeUnit, const BodyIndex&);
std::optional<CompiledCode> generateStraightCode(const CodeGenerationWorker&, const CodeGenerationInputs&, const Code&, const BodyIndex&, CodeGenerationTotals&);
std::optional<CompiledCode> generateEntryWrittenByHand(const CodeGenerationWorker&, const CodeGenerationInputs&, const Code&, NOESCAPE const ScopedLambda<TypeID(unsigned)>& typeAskedOfEntryParameter, CodeGenerationTotals&, bool nobodyEnters);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
