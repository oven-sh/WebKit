/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeapHooks.h"

#if ENABLE(AOT)

#include "AOTCompiledCallee.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "Collector.h"
#include "HeapInlines.h"
#include "JSObject.h"
#include "MarkedBlockCage.h"
#include "SimpleMarkingConstraint.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include <type_traits>
#include <wtf/Locker.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

// The constraints are added before the heap joins its collector, so that its first collection has them. Until then other heaps' collections run them too.
static bool shouldVisitRoots(VM& vm, const AbstractSlotVisitor& visitor)
{
    return vm.hasCompiledCode() && visitor.collector().hasHeap(vm.heap);
}

template<typename Visitor>
static void visitMarkingRoots(VM& vm, Visitor& visitor)
{
    VMState& state = vm.aotState();
    state.caches->visitRoots(visitor, RootVolatility::GreyedByMarking);

    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        if (!visitor.isMarked(livenessToken(instance)))
            continue;
        CacheRegistry& caches = *instance->state->caches;
        caches.visitRoots(visitor, RootVolatility::GreyedByMarking);
        // "At" runs once per stop of the mutator, so it misses a realm first reached during draining. The GC verifier always needs these roots.
        if (std::is_same_v<Visitor, AbstractSlotVisitor> || !caches.hasVisitedExecutionRoots())
            caches.visitRoots(visitor, RootVolatility::GreyedByExecution);
    }
}

template<typename Visitor>
static void visitExecutionRoots(VM& vm, Visitor& visitor)
{
    VMState& state = vm.aotState();
    state.caches->visitRoots(visitor, RootVolatility::GreyedByExecution);

    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        if (visitor.isMarked(livenessToken(instance)))
            instance->state->caches->visitRoots(visitor, RootVolatility::GreyedByExecution);
    }
}

void addMarkingConstraints(VM& vm)
{
    vm.heap.addMarkingConstraint(makeUnique<SimpleMarkingConstraint>(
        "Ao"_s, "Instances of Statically Compiled Code"_s,
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([&vm] (auto& visitor) {
            if (!shouldVisitRoots(vm, visitor))
                return;
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::CodeBlocks);
            visitMarkingRoots(vm, visitor);
        })),
        ConstraintVolatility::GreyedByMarking,
        ConstraintConcurrency::Concurrent,
        ConstraintParallelism::Sequential));
    vm.heap.addMarkingConstraint(makeUnique<SimpleMarkingConstraint>(
        "At"_s, "Static Variables of Compiled Code"_s,
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([&vm] (auto& visitor) {
            if (!shouldVisitRoots(vm, visitor))
                return;
            SetRootMarkReasonScope rootScope(visitor, RootMarkReason::CodeBlocks);
            visitExecutionRoots(vm, visitor);
        })),
        ConstraintVolatility::GreyedByExecution,
        ConstraintConcurrency::Concurrent,
        ConstraintParallelism::Sequential));
}

void initializeCollectorWords(VM& vm, Instance& instance)
{
    instance.barrierThreshold = vm.heap.barrierThreshold();
    instance.mutatorShouldBeFenced = vm.heap.mutatorShouldBeFenced();
    instance.cellCageBase = vm.heap.cage().base();
    instance.heap.storedCellLog = &vm.heap.storedCellLog();
}

namespace HeapHooks {

void finalize(VM& vm, CollectionScope scope)
{
    VMState& state = vm.aotState();
    {
        Locker locker { state.instancesLock };
        state.instances.removeAllMatching([&](Instance* instance) {
            if (vm.heap.isMarked(livenessToken(instance)))
                return false;
            state.instancesToDestroy.append(instance);
            return true;
        });
    }

    CacheValidation validation = state.validatesRuntime ? CacheValidation::On : CacheValidation::Off;
    state.caches->finalize(vm, scope, validation);
    for (Instance* instance : state.instances)
        instance->state->caches->finalize(vm, scope, validation);
}

void didChangeBarrierThreshold(VM& vm, unsigned threshold, bool mutatorShouldBeFenced)
{
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        instance->barrierThreshold = threshold;
        instance->mutatorShouldBeFenced = mutatorShouldBeFenced;
    }
}

void willAnalyzeHeap(VM& vm)
{
    VMState& state = vm.aotState();
    state.caches->willAnalyzeHeap(vm);
    for (Instance* instance : state.instances)
        instance->state->caches->willAnalyzeHeap(vm);
}

String nameOfCompiledCallee(JSCell* cell)
{
    return CompiledCallee::isCompiledCallee(cell) ? CompiledCallee::name(asObject(cell)) : String();
}

void crashOnCellVisitMismatch(VM& vm, const JSCell* cell)
{
    crashOnValidationFailure(vm, { ValidationFailureKind::VisitedCellsDiffer, { }, { }, cell });
}

} // namespace HeapHooks

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
