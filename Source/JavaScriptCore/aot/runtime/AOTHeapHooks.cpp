/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeapHooks.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTCompiledCallee.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTMutatorMarkStackLayout.h"
#include "AOTObject.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "Collector.h"
#include "HeapInlines.h"
#include "HeapUtil.h"
#include "JSCInlines.h"
#include "JSObject.h"
#include "MarkedBlockCage.h"
#include "SimpleMarkingConstraint.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include <type_traits>
#include <wtf/Locker.h>
#include <wtf/Threading.h>
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
    instance.heap.mutatorMarkStack = &HeapUtil::mutatorMarkStack(vm.heap);
    instance.heap.barriersExecuted = &HeapUtil::barriersExecuted(vm.heap);
    auto* headSegmentWord = std::bit_cast<MutatorMarkStackLayout::Segment**>(std::bit_cast<uint8_t*>(instance.heap.mutatorMarkStack) + MutatorMarkStackLayout::offsetOfHeadSegment());
    RELEASE_ASSERT(*headSegmentWord == MutatorMarkStackLayout::headSegmentOf(*instance.heap.mutatorMarkStack));
}

void validateFunctionCellIsNotInFrame(VM& vm, const JSCell* cell)
{
    if (!Thread::currentSingleton().stack().contains(const_cast<JSCell*>(cell))) [[likely]]
        return;
    crashOnValidationFailure(vm, { ValidationFailureKind::FunctionCellInFrameGotProperties, functionOf(cell).value_or(FunctionID()), { }, cell });
}

namespace HeapHooks {

uint8_t sabotage;
static_assert(static_cast<uint8_t>(SabotagedPart::StackNamesNoInnerCell) == static_cast<uint8_t>(Sabotage::StackNamesNoInnerCell));
static_assert(static_cast<uint8_t>(SabotagedPart::NarrowReferenceOnStackNamesNoInnerCell) == static_cast<uint8_t>(Sabotage::NarrowReferenceOnStackNamesNoInnerCell));
static_assert(static_cast<uint8_t>(SabotagedPart::InnerObjectIsVisitedWithoutItsFields) == static_cast<uint8_t>(Sabotage::InnerObjectIsVisitedWithoutItsFields));
static_assert(static_cast<uint8_t>(SabotagedPart::InnerArrayIsVisitedWithoutItsElements) == static_cast<uint8_t>(Sabotage::InnerArrayIsVisitedWithoutItsElements));
static_assert(static_cast<uint8_t>(SabotagedPart::InnerCellVisitedInPlaceStaysWhite) == static_cast<uint8_t>(Sabotage::InnerCellVisitedInPlaceStaysWhite));
static_assert(static_cast<uint8_t>(SabotagedPart::InnerCellIsMarkedWithoutItsOuterCell) == static_cast<uint8_t>(Sabotage::InnerCellIsMarkedWithoutItsOuterCell));
static_assert(static_cast<uint8_t>(SabotagedPart::InnerCellLetGoKeepsItsHeader) == static_cast<uint8_t>(Sabotage::InnerCellLetGoKeepsItsHeader));

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

JSCell* innerCellAround(JSCell* outerCell, const void* address)
{
    size_t distance = static_cast<const uint8_t*>(address) - reinterpret_cast<const uint8_t*>(outerCell);
    JSCell* inner = nullptr;
    for (uint32_t offset : uncheckedDowncast<Object>(outerCell)->layout().offsetsOfInnerObjects()) {
        if (offset <= distance)
            inner = reinterpret_cast<JSCell*>(reinterpret_cast<uint8_t*>(outerCell) + offset);
    }
    if (isSabotaged(SabotagedPart::StackNamesNoInnerCell)) [[unlikely]]
        return nullptr;
    return inner && inner->structureID() ? inner : nullptr;
}

void noteInnerCellsWithoutMark(JSCell* outerCell)
{
    VM& vm = outerCell->vm();
    VMState& state = vm.aotState();
    for (uint32_t offset : uncheckedDowncast<Object>(outerCell)->layout().offsetsOfInnerObjects()) {
        JSCell* inner = reinterpret_cast<JSCell*>(reinterpret_cast<uint8_t*>(outerCell) + offset);
        if (!inner->structureID() || vm.heap.isMarked(inner))
            continue;
        Locker locker { state.innerCellsWithoutMark.lock };
        state.innerCellsWithoutMark.cells.append(inner);
    }
}

void didFinishMarking(VM& vm)
{
    VMState& state = vm.aotState();
    Locker locker { state.innerCellsWithoutMark.lock };
    for (JSCell* inner : state.innerCellsWithoutMark.cells) {
        if (!vm.heap.isMarked(inner) && !isSabotaged(SabotagedPart::InnerCellLetGoKeepsItsHeader))
            *reinterpret_cast<uint64_t*>(inner) = 0;
    }
    state.innerCellsWithoutMark.cells.clear();
}

void crashOnCellVisitMismatch(VM& vm, const JSCell* cell)
{
    crashOnValidationFailure(vm, { ValidationFailureKind::VisitedCellsDiffer, { }, { }, cell });
}

void validateSlotsOf(VM& vm, const JSCell* cell)
{
    validateSlotTypes(vm, *uncheckedDowncast<Object>(cell));
}

} // namespace HeapHooks

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
