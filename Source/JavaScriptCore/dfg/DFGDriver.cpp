/*
 * Copyright (C) 2011-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "DFGDriver.h"

#include "CallVariant.h"
#include "CodeBlock.h"
#include "DFGCapabilities.h"
#include "DFGCommon.h"
#include "DFGJITCode.h"
#include "DFGPlan.h"
#include "DFGThunks.h"
#include "DeferGCInlines.h"
#include "FunctionAllowlist.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JITCode.h"
#include "JITWorklist.h"
#include "JSCellInlines.h"
#include "Options.h"
#include "ThunkGenerators.h"
#include "TypeProfilerLog.h"
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace DFG {

static unsigned numCompilations;

unsigned getNumCompilations()
{
    return numCompilations;
}

#if ENABLE(DFG_JIT)
// Mutator-side half of the fence in inlineFunctionForCapabilityLevel(): complete the lazily materialized state
// (CodeBlock::prepareLazyStateForConcurrentCompilation) of every block the ByteCodeParser may inline into this plan --
// the callees the profiled blocks name so far, transitively, within the inliner's depth and size limits (this mirrors
// InliningPlan::surveyCallSites plus the accessor and recorded-status callees the parser also reaches). A callee that
// gets linked after this ran is just not inlined by this compilation.
static void prepareLazyStateOfInlineCandidates(VM& vm, CodeBlock* codeBlock, CodeBlock* profiledDFGCodeBlock, JITCompilationMode mode)
{
    if (!Options::useThinChildExecutables() && !Options::useLazyFunctionExecutables())
        return; // every block is born prepared
    DeferGCForAWhile deferGC(vm); // raw CodeBlock* / callee cells are held across the ensure*() allocations below
    JITType jitType = isFTL(mode) ? JITType::FTLJIT : JITType::DFGJIT;

    struct Entry {
        CodeBlock* block;
        unsigned depth; // inline depth of `block`; the root is 0, as in InliningPlan
    };
    Vector<Entry, 16> worklist;
    UncheckedKeyHashSet<CodeBlock*> seen;
    auto push = [&](CodeBlock* block, unsigned depth) {
        if (block && seen.add(block).isNewEntry)
            worklist.append({ block, depth });
    };
    CodeBlock* rootBaseline = codeBlock->baselineAlternative();
    push(rootBaseline, 0);
    push(profiledDFGCodeBlock, 0);
    // InlineStackEntry also consults the optimized replacement's IC maps.
    if (CodeBlock* replacement = rootBaseline->replacement(); replacement && JITCode::isOptimizingJIT(replacement->jitType()))
        push(replacement, 0);

    Vector<std::pair<JSCell*, CodeSpecializationKind>, 16> callees;
    // Breadth-first, so a block reachable at several depths is expanded at its shallowest.
    for (unsigned blocksVisited = 0; blocksVisited < worklist.size(); ++blocksVisited) {
        Entry entry = worklist[blocksVisited];
        entry.block->baselineAlternative()->prepareLazyStateForConcurrentCompilation();
        if (blocksVisited + 1 >= Options::maximumGlobalInliningPlanSites())
            break;
        if (entry.depth + 1 >= Options::maximumInliningDepth())
            continue; // InliningPlan::priceCandidate rejects depth >= maximumInliningDepth, where the root's callees are depth 1

        callees.shrink(0);
        entry.block->collectProfiledCallees(callees);
        for (auto [callee, kind] : callees) {
            FunctionExecutable* executable = callee ? CallVariant(callee).functionExecutable() : nullptr;
            if (!executable)
                continue;
            CodeBlock* calleeBlock = executable->baselineCodeBlockFor(kind);
            if (!calleeBlock || !mightInlineFunctionFor(jitType, calleeBlock, kind))
                continue;
            push(calleeBlock, entry.depth + 1);
            if (CodeBlock* replacement = calleeBlock->replacement(); replacement && JITCode::isOptimizingJIT(replacement->jitType()))
                push(replacement, entry.depth + 1);
        }
    }
}

static CompilationResult compileImpl(
    VM& vm, CodeBlock* codeBlock, CodeBlock* profiledDFGCodeBlock, JITCompilationMode mode,
    BytecodeIndex osrEntryBytecodeIndex, Operands<std::optional<JSValue>>&& mustHandleValues,
    Ref<DeferredCompilationCallback>&& callback)
{
    switch (mode) {
    case JITCompilationMode::DFG:
    case JITCompilationMode::UnlinkedDFG:
        if (!Options::bytecodeRangeToDFGCompile().isInRange(codeBlock->instructionsSize()) || !ensureGlobalDFGAllowlist().contains(codeBlock))
            return CompilationResult::CompilationFailed;
        break;
    case JITCompilationMode::FTL:
    case JITCompilationMode::FTLForOSREntry:
#if ENABLE(FTL_JIT)
        if (!Options::bytecodeRangeToFTLCompile().isInRange(codeBlock->instructionsSize()) || !ensureGlobalFTLAllowlist().contains(codeBlock))
            return CompilationResult::CompilationFailed;
        break;
#else
        [[fallthrough]];
#endif
    case JITCompilationMode::Baseline:
    case JITCompilationMode::InvalidCompilation:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    
    numCompilations++;
    
    ASSERT(codeBlock);
    ASSERT(codeBlock->alternative());
    ASSERT(JITCode::isBaselineCode(codeBlock->alternative()->jitType()));
    ASSERT(!profiledDFGCodeBlock || profiledDFGCodeBlock->jitType() == JITType::DFGJIT);
    
    dataLogLnIf(logCompilationChanges(mode), "DFG(Driver) compiling ", *codeBlock, " with ", mode, ", instructions size = ", codeBlock->instructionsSize());
    
    if (vm.typeProfiler())
        vm.typeProfilerLog()->processLogEntries(vm, "Preparing for DFG compilation."_s);

    prepareLazyStateOfInlineCandidates(vm, codeBlock, profiledDFGCodeBlock, mode);
    if (mode != JITCompilationMode::FTLForOSREntry)
        codeBlock->baselineAlternative()->ensureCatchLivenessIsComputedForExecutedCatches(); // the parser makes a catch OSR entrypoint only where the buffer exists

    Ref<Plan> plan = adoptRef(*new Plan(codeBlock, profiledDFGCodeBlock, mode, osrEntryBytecodeIndex, WTF::move(mustHandleValues)));

    plan->setCallback(WTF::move(callback));
    JITWorklist& worklist = JITWorklist::ensureGlobalWorklist();
    dataLogLnIf(Options::useConcurrentJIT() && logCompilationChanges(mode), "Deferring DFG compilation of ", *codeBlock, " with queue length ", worklist.queueLength(), ".\n");
    return worklist.enqueue(WTF::move(plan));
}
#else // ENABLE(DFG_JIT)
static CompilationResult compileImpl(
    VM&, CodeBlock*, CodeBlock*, JITCompilationMode, BytecodeIndex, const Operands<std::optional<JSValue>>&,
    Ref<DeferredCompilationCallback>&&)
{
    return CompilationResult::CompilationFailed;
}
#endif // ENABLE(DFG_JIT)

CompilationResult compile(
    VM& vm, CodeBlock* codeBlock, CodeBlock* profiledDFGCodeBlock, JITCompilationMode mode,
    BytecodeIndex osrEntryBytecodeIndex, Operands<std::optional<JSValue>>&& mustHandleValues,
    Ref<DeferredCompilationCallback>&& callback)
{
    return compileImpl(vm, codeBlock, profiledDFGCodeBlock, mode, osrEntryBytecodeIndex, WTF::move(mustHandleValues), callback.copyRef());
}

} } // namespace JSC::DFG
