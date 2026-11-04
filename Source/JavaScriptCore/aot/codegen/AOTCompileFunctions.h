/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeGenerator.h"
#include "AOTCompiledCode.h"
#include <span>

namespace JSC {

namespace B3 {
class CompilationScratch;
}

namespace AOT { namespace Typed {

struct CodeGenerationWorker {
    BuildContext& build;
    TypeArena& types;
    B3::CompilationScratch& scratch;
};

void compileFunctions(const CodeGenerationWorker&, const CodeGenerationInputs&, std::span<const FunctionID> ids, std::span<CompiledCode> results, CodeGenerationTotals&);
uint32_t estimatedCodeSize(const Program&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
