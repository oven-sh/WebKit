/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTClosure.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramTables.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAllocateSlow, JSCell*, (Instance* instance, uint32_t allocation))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAllocateSlow);
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocate(*instance, allocation);
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]] {
        if (CallFrame* frameThatReturns = findReturnIntoFunctionWithoutHandlers(vm, std::bit_cast<CallFrame*>(frameRecord))) {
            JSCell* cell = instance->state->objects->structures.allocate(vm, allocation);
            throwOutOfMemoryErrorAtReturnOf(*instance, frameThatReturns);
            OPERATION_RETURN(scope, cell);
        }
    }
    OPERATION_RETURN(scope, instance->state->objects->structures.allocateAsCompiledCodeDoes(vm, allocation));
}

// For an object that lives in a frame. No cell is allocated: one that is dropped at once would lie in the heap with a header and without its fields.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetHeaderOfAllocation, uint64_t, (Instance* instance, uint32_t allocation))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetHeaderOfAllocation);
    OPERATION_RETURN(scope, instance->state->objects->structures.fillAllocationSite(vm, allocation).header);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMaterializeEnvironment, JSCell*, (Instance* instance, uint32_t allocation, const EncodedJSValue* source))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMaterializeEnvironment);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocate(*instance, allocation);
    const ProgramTables& tables = instance->tables();
    JSCell* cell = instance->state->objects->structures.allocate(vm, allocation);
    size_t size = tables.environmentForShape(tables.allocations()[allocation].shape).size;
    memcpy(reinterpret_cast<uint8_t*>(cell) + sizeof(JSCell), reinterpret_cast<const uint8_t*>(source) + sizeof(JSCell), size - sizeof(JSCell));
    vm.mutatorFence();
    OPERATION_RETURN(scope, cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewClosureSlow, JSCell*, (Instance* instance, uint64_t request, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewClosureSlow);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    ObjectModel& objects = *instance->state->objects;
    uint64_t captures[] = { first, second, third, fourth };
    RELEASE_ASSERT(ClosureRequest::countOf(request) <= (ClosureRequest::isSlim(request) ? ClosureRequest::maxSlimClosureCaptures : ClosureRequest::maxNumberOfCaptures));
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocateClosure(*instance, request);
    JSCell* cell;
    auto word = [&](ptrdiff_t offset) -> uint64_t& {
        return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(cell) + offset);
    };
    if (ClosureRequest::isSlim(request)) {
        cell = SlimClosure::createForCompiledCode(vm, objects.closures.slimClosureStructure(vm, FunctionID(ClosureRequest::functionOf(request))), SlimClosure::allocationSize(ClosureRequest::countOf(request)));
        for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
            word(SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
        WTF::storeStoreFence();
        word(SlimClosure::offsetOfWord()) = instance->closures.closureWords[ClosureRequest::functionOf(request)];
        WTF::storeStoreFence();
        OPERATION_RETURN(scope, cell);
    }
    cell = objects.structures.allocate(vm, ClosureRequest::allocationOf(request));
    word(JSObject::butterflyOffset()) = 0;
    word(JSCallee::offsetOfScopeChain()) = std::bit_cast<uint64_t>(instance->globalObject);
    word(JSFunction::offsetOfExecutableOrRareData()) = std::bit_cast<uint64_t>(instance->closures.executables[ClosureRequest::executableOf(request)]);
    for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
        word(Closure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
    WTF::storeStoreFence();
    word(Closure::offsetOfEntry()) = instance->closures.closureWords[ClosureRequest::functionOf(request)];
    WTF::storeStoreFence();
    OPERATION_RETURN(scope, cell);
}

// The closure is slim and lies in a frame. Nothing but the call that is under way has it.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMoveClosureToHeap, JSCell*, (Instance* instance, JSCell* closureInFrame))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMoveClosureToHeap);
    const uint64_t* source = reinterpret_cast<const uint64_t*>(closureInFrame);
    uint64_t closureWord = source[SlimClosure::offsetOfWord() / sizeof(uint64_t)];
    FunctionID function(ClosureWord::functionOf(closureWord));
    unsigned count = captureCountsOf(function).captures;
    RELEASE_ASSERT(count <= ClosureRequest::maxSlimClosureCaptures);
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocateClosure(*instance, function.index() | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift);
    JSCell* cell = SlimClosure::createForCompiledCode(vm, instance->state->objects->closures.slimClosureStructure(vm, function), SlimClosure::allocationSize(count));
    uint64_t* target = reinterpret_cast<uint64_t*>(cell);
    for (unsigned slot = 0; slot < count; ++slot)
        target[SlimClosure::offsetOfCaptures() / sizeof(uint64_t) + slot] = source[SlimClosure::offsetOfCaptures() / sizeof(uint64_t) + slot];
    WTF::storeStoreFence();
    target[SlimClosure::offsetOfWord() / sizeof(uint64_t)] = closureWord;
    WTF::storeStoreFence();
    OPERATION_RETURN(scope, cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewMap, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewMap);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocateMap(*instance);
    Structure* structure = globalObject->mapStructure();
    prepareInlineAllocation<JSMap>(instance, InlineAllocation::Map, structure);
    OPERATION_RETURN(scope, JSMap::create(vm, structure));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewSet, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSet);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocateSet(*instance);
    Structure* structure = globalObject->setStructure();
    prepareInlineAllocation<JSSet>(instance, InlineAllocation::Set, structure);
    OPERATION_RETURN(scope, JSSet::create(vm, structure));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBarrierSlow, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBarrierSlowPreservingRegisters, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRememberElement, void, (Instance* instance, JSCell* array, EncodedJSValue element))
{
    JSObject* owner = cellArgument<JSObject>(array, Entry::operationAOTRememberElement);
    JSValue value = JSValue::decode(element);
    if (value && value.isCell())
        instance->vm->heap.writeBarrierSlowPath(owner, value.asCell());
    else
        instance->vm->writeBarrierSlowPath(owner);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
