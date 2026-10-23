/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileFunctions.h"

#if ENABLE(AOT)

#include "AOTCodeSizeEstimate.h"
#include "AOTFunctionCodeGenerator.h"
#include "AOTVector.h"
#include "B3Generate.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void compileFunctions(const CodeGenerationWorker& worker, const CodeGenerationInputs& inputs, std::span<const FunctionID> ids, std::span<CompiledCode> results, CodeGenerationTotals& totals)
{
    RELEASE_ASSERT(ids.size() == results.size());

    Vector<std::unique_ptr<FunctionCodeGenerator>, 16> generators;
    Vector<B3::Procedure*, 16> procedures;
    for (size_t index = 0; index < ids.size(); ++index) {
        FunctionID id = ids[index];
        results[index] = { };
        auto generator = FunctionCodeGenerator::create(worker, inputs, id, totals);
        if (generator && !generator->generatesDirectly())
            procedures.append(&generator->procedure());
        generators.append(WTF::move(generator));
    }

    B3::prepareForGeneration(procedures.span());

    for (size_t index = 0; index < ids.size(); ++index) {
        if (generators[index]) {
            CompiledCode code = generators[index]->finish();
            while (generators[index] && generators[index]->didFailToGenerateDirectly()) {
                generators[index] = FunctionCodeGenerator::create(worker, inputs, ids[index], totals, generators[index]->nextDirectCodeAttempt());
                if (!generators[index])
                    break;
                if (!generators[index]->generatesDirectly())
                    B3::prepareForGeneration(generators[index]->procedure());
                code = generators[index]->finish();
            }
            results[index] = WTF::move(code);
        }
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
