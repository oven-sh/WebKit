/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeTable.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "ObjectConstructor.h"
#include <wtf/DataLog.h>
#include <wtf/WTFProcess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAllocateSlow, JSCell*, (Instance* instance, uint32_t allocation))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAllocateSlow);
    if (allocation & allocatesInstanceOfClassBelow) [[unlikely]] {
        OPERATION_RETURN(scope, instance->state->objects->classes.createInstanceOfClassBelow(vm, allocation & ~allocatesInstanceOfClassBelow));
    }
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocate(*instance, allocation);
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]]
        vm.heap.cage().tryTakeBackOutOfMemoryError(vm);
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
    DeferTerminationForAWhile deferTermination(vm);
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
        cell = SlimClosure::createForCompiledCode(vm, objects.closures.slimClosureStructure(kindOfClosureNumber(ClosureRequest::closureNumberOf(request))), SlimClosure::allocationSize(ClosureRequest::countOf(request)));
        for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
            word(SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
        WTF::storeStoreFence();
        word(SlimClosure::offsetOfWord()) = instance->functionTables.closureWords[ClosureRequest::closureNumberOf(request)];
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
    word(Closure::offsetOfEntry()) = instance->functionTables.closureWords[ClosureRequest::closureNumberOf(request)];
    WTF::storeStoreFence();
    OPERATION_RETURN(scope, cell);
}

// The closure is slim and lies in a frame. Nothing but the call that is under way has it.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMoveClosureToHeap, JSCell*, (Instance* instance, JSCell* closureInFrame))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMoveClosureToHeap);
    const uint64_t* source = reinterpret_cast<const uint64_t*>(closureInFrame);
    uint64_t closureWord = source[SlimClosure::offsetOfWord() / sizeof(uint64_t)];
    uint32_t closureNumber = ClosureWord::closureNumberOf(closureWord);
    unsigned count = captureCountsOfClosureNumber(closureNumber).captures;
    RELEASE_ASSERT(count <= ClosureRequest::maxSlimClosureCaptures);
    if (AllocationCounts::isEnabled()) [[unlikely]]
        AllocationCounts::willAllocateClosure(*instance, closureNumber | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift);
    JSCell* cell = SlimClosure::createForCompiledCode(vm, instance->state->objects->closures.slimClosureStructure(kindOfClosureNumber(closureNumber)), SlimClosure::allocationSize(count));
    uint64_t* target = reinterpret_cast<uint64_t*>(cell);
    for (unsigned slot = 0; slot < count; ++slot)
        target[SlimClosure::offsetOfCaptures() / sizeof(uint64_t) + slot] = source[SlimClosure::offsetOfCaptures() / sizeof(uint64_t) + slot];
    WTF::storeStoreFence();
    target[SlimClosure::offsetOfWord() / sizeof(uint64_t)] = closureWord;
    WTF::storeStoreFence();
    OPERATION_RETURN(scope, cell);
}

#define AOT_DEFINE_NEW_COLLECTION(operation, name, structureOfRealm) \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operation, JSCell*, (Instance* instance)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::operation); \
        DeferTerminationForAWhile deferTermination(vm); \
        if (AllocationCounts::isEnabled()) [[unlikely]] \
            AllocationCounts::willAllocate ## name(*instance); \
        Structure* structure = globalObject->structureOfRealm(); \
        prepareInlineAllocation<JS ## name>(instance, InlineAllocation::name, structure); \
        OPERATION_RETURN(scope, JS ## name::create(vm, structure)); \
    }
AOT_DEFINE_NEW_COLLECTION(operationAOTNewMap, Map, mapStructure)
AOT_DEFINE_NEW_COLLECTION(operationAOTNewSet, Set, setStructure)
#undef AOT_DEFINE_NEW_COLLECTION

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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringConstant, JSCell*, (Instance* instance, uint32_t index))
{
    LeafOperationScope leaf(*instance->vm);
    return instance->state->strings->constant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewStringConstant, JSCell*, (Instance* instance, uint32_t index))
{
    LeafOperationScope leaf(*instance->vm);
    if (index & ProgramStrings::isPositionOfFunctionName)
        return instance->state->strings->functionNameAt(*instance->vm, index & ~ProgramStrings::isPositionOfFunctionName);
    return instance->state->strings->createConstant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTKeyConstant, JSCell*, (Instance* instance, uint32_t index))
{
    VM& vm = *instance->vm;
    LeafOperationScope leaf(vm);
    DeferGCForAWhile deferGC(vm);
    return instance->state->strings->keyCell(vm, index);
}

// Compiled code reads through the cell without a test.
static void exitIfRealmLacksHostOf(State& state, uint32_t numberOfCell)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(state.tables.builtinCells()[numberOfCell], table);
    RELEASE_ASSERT(key);
    if (BuiltinBindings::provides(table.at(key->builtin()).origin))
        return;
    dataLogLn("AOT: the program needs the host's ", table.textOf(key->builtin()), ", and this realm has no host");
    exitProcess(4);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBuiltinCell, JSCell*, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTBuiltinCell);
    JSCell* cell = instance->state->builtinCells->cell(index);
    if (!cell) [[unlikely]]
        exitIfRealmLacksHostOf(*instance->state, index);
    OPERATION_RETURN(scope, cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBigIntConstant, EncodedJSValue, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTBigIntConstant);
    DeferTerminationForAWhile deferTermination(vm);
    OPERATION_RETURN(scope, JSValue::encode(instance->state->strings->bigIntConstant(globalObject, index)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSingletonClosure, JSCell*, (Instance* instance, uint32_t closureNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTSingletonClosure);
    OPERATION_RETURN(scope, instance->state->objects->closures.closure(vm, closureFunctionWithNumber(closureNumber)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewTemplateObject, JSCell*, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewTemplateObject);
    ProgramStrings& strings = *instance->state->strings;
    const auto& templateObject = requestAt<TemplateObjectRequest>(instance, request);
    MarkedArgumentBuffer cookedStrings;
    MarkedArgumentBuffer rawStrings;
    for (const auto& part : unsafeMakeSpan(templateObject.parts, templateObject.count)) {
        cookedStrings.append(part.cookedPlusOne ? JSValue(strings.constant(vm, part.cookedPlusOne - 1)) : jsUndefined());
        rawStrings.append(strings.constant(vm, part.raw));
    }
    if (cookedStrings.hasOverflowed() || rawStrings.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    JSArray* cooked = constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), cookedStrings);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* raw = constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), rawStrings);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    objectConstructorFreeze(globalObject, raw);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    cooked->putDirect(vm, vm.propertyNames->raw, raw, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    objectConstructorFreeze(globalObject, cooked);
    OPERATION_RETURN(scope, cooked);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
