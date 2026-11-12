/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyFactsForTables.h"

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTBuildContext.h"
#include "AOTCurrentWork.h"
#include "AOTFieldGetters.h"
#include "AOTIRProgram.h"
#include "AOTLinkedFunctions.h"
#include "AOTThreads.h"

namespace JSC { namespace AOT {

static BitVector findFunctionsWithFramesForTables(const LinkedFunctions& functions, size_t numberOfFunctions, const ParallelLoops& loops)
{
    auto add = [&](BitVector& found, FunctionID id) {
        RELEASE_ASSERT(id.index() < numberOfFunctions);
        found.quickSet(id.index());
    };

    Vector<BitVector> foundByWorker(loops.width());
    forEachIndexInParallel(loops, static_cast<unsigned>(functions.size()), [&](unsigned position, unsigned worker) {
        BitVector& found = foundByWorker[worker];
        found.ensureSize(numberOfFunctions);

        const CompiledCode& code = functions[position];
        add(found, code.unit.function);
        for (size_t frame = 1; frame < code.sites.inlineFrames.size(); ++frame)
            add(found, code.body.callees[code.sites.inlineFrames[frame].callee].function);
    });

    BitVector all(numberOfFunctions);
    for (const BitVector& found : foundByWorker)
        all.merge(found);
    for (const FusedCallEntry& fused : functions.fusedCallEntries())
        add(all, fused.unit.function);
    return all;
}

static bool onlyReturnsForTables(const Typed::Function& function)
{
    if (function.body.numberOfBlocks() != 1 || function.isCoroutine())
        return false;
    Typed::BodyBlock block = function.body.entry();
    for (Typed::BodyValue value : block.values()) {
        if (value.opcode() != Typed::Opcode::Undefined)
            return false;
    }
    Typed::BodyTerminator terminator = block.terminator();
    if (terminator.kind() != Typed::TerminatorKind::Return)
        return false;
    Typed::ValueID returned = terminator.as<Typed::TerminatorKind::Return>().valueOperand();
    return !returned || (block.holds(returned) && !block.indexOfParameter(returned));
}

BodyFactsForTables collectBodyFactsForTables(const Typed::Program& program, const Typed::Plan&, const LinkedFunctions& functions, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    size_t numberOfFunctions = program.functions.size();

    Vector<uint32_t> functionsThatOnlyReturn = findIndicesInParallel(loops, static_cast<unsigned>(numberOfFunctions), [&](unsigned index) {
        FunctionID id(index);
        const Typed::Function& function = program.at(id);
        if (!function.hasBody())
            return false;
        CurrentWorkScope work(program, id, "image"_s);
        return onlyReturnsForTables(function);
    });

    BodyFactsForTables facts;
    facts.functionsThatOnlyReturn = BitVector(numberOfFunctions);
    for (uint32_t function : functionsThatOnlyReturn)
        facts.functionsThatOnlyReturn.quickSet(function);
    Vector<uint32_t> fieldGetters = findIndicesInParallel(loops, static_cast<unsigned>(numberOfFunctions), [&](unsigned index) {
        return !!Typed::fieldReturnedByGetter(program, program.functions[index]);
    });
    for (uint32_t getter : fieldGetters)
        facts.fieldsReturnedByGetters.append({ FunctionID(getter), Typed::fieldReturnedByGetter(program, program.functions[getter]) });
    facts.functionsWithFrames = findFunctionsWithFramesForTables(functions, numberOfFunctions, loops);
    return facts;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
