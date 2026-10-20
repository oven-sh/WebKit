/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileFunctions.h"

#if ENABLE(AOT)

#include "AOTCodeSizeEstimate.h"
#include "AOTFunctionCodeGenerator.h"
#include "B3Generate.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void compileFunctions(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, std::span<const FunctionID> ids, std::span<CompiledCode> results, CodeGenerationTotals& totals)
{
    RELEASE_ASSERT(ids.size() == results.size());

    Vector<std::unique_ptr<FunctionCodeGenerator>, 16> generators;
    Vector<B3::Procedure*, 16> procedures;
    for (FunctionID id : ids) {
        auto generator = FunctionCodeGenerator::create(worker, inputs, id, totals);
        if (generator)
            procedures.append(&generator->procedure());
        generators.append(WTF::move(generator));
    }

    B3::prepareForGeneration(procedures.span());

    for (size_t index = 0; index < ids.size(); ++index) {
        results[index] = generators[index] ? generators[index]->finish() : CompiledCode { };
        results[index].function = ids[index];
        generators[index] = nullptr;
    }
}

uint32_t estimatedCodeSize(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    uint64_t size = 0;
    for (const Block& block : function.blocks)
        size += estimatedCodeBytes(program, function, block);
    return static_cast<uint32_t>(std::min<uint64_t>(size, UINT32_MAX));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
