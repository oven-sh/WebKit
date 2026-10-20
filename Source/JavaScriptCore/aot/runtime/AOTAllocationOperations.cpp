/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramTables.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAllocateSlow, JSCell*, (Instance* instance, uint32_t allocation))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAllocateSlow);
    OPERATION_RETURN(scope, instance->state->objects->structures.allocate(vm, allocation));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMaterializeEnvironment, JSCell*, (Instance* instance, uint32_t allocation, const EncodedJSValue* source))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMaterializeEnvironment);
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
    ObjectModel& objects = *instance->state->objects;
    uint64_t captures[] = { first, second, third, fourth };
    RELEASE_ASSERT(ClosureRequest::countOf(request) <= (ClosureRequest::isSlim(request) ? ClosureRequest::maxSlimClosureCaptures : ClosureRequest::maxNumberOfCaptures));
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewMap, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewMap);
    Structure* structure = globalObject->mapStructure();
    prepareInlineAllocation<JSMap>(instance, InlineAllocation::Map, structure);
    OPERATION_RETURN(scope, JSMap::create(vm, structure));
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
