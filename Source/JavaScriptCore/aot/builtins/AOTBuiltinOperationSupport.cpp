/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinOperationSupport.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTObjectWords.h"
#include "AOTState.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static JSValue objectOf(Instance* instance, BuiltinID id)
{
    JSCell* cell = id ? instance->state->builtinCells->findObject(id) : nullptr;
    return cell ? JSValue(cell) : jsUndefined();
}

JSValue callMethodInEngine(Instance* instance, BuiltinID function, JSValue receiver, const ArgList& arguments)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue callee = objectOf(instance, function);
    auto callData = JSC::getCallData(callee);
    if (callData.type == CallData::Type::None) {
        throwException(globalObject, scope, createNotAFunctionError(globalObject, callee));
        return { };
    }
    JSValue result = call(globalObject, callee, callData, receiver, arguments);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue callInEngineOn(Instance* instance, BuiltinID function, JSValue receiver, JSValue first, JSValue second, JSValue third)
{
    MarkedArgumentBuffer list;
    if (first)
        list.append(first);
    if (second)
        list.append(second);
    if (third)
        list.append(third);
    return callMethodInEngine(instance, function, receiver, list);
}

JSValue callInEngine(Instance* instance, BuiltinID function, JSValue first, JSValue second, JSValue third)
{
    return callInEngineOn(instance, function, objectOf(instance, BuiltinTable::singleton().at(function).holder), first, second, third);
}

JSValue callMethodInEngine(Instance* instance, BuiltinID function, JSValue receiver, JSValue first, JSValue second)
{
    return callInEngineOn(instance, function, receiver, first, second, { });
}

static bool isOriginalStructureOfLazyClass(JSGlobalObject* realm, Structure* structure)
{
    if (structure->typeInfo().type() == JSDateType)
        return structure == realm->dateStructureConcurrently();
    return realm->isOriginalTypedArrayStructure(structure, false);
}

void markPristineIfOriginal(Instance& instance, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->isPristine() || !isOriginalStructureOfLazyClass(instance.globalObject, structure))
        return;
    structure->tryMarkPristine(*instance.vm);
}

void prepareFinalObjectAllocation(Instance* instance, InlineAllocation kind, Structure* structure)
{
    auto& data = instance->allocation.inlineAllocations[static_cast<unsigned>(kind)];
    if (data.allocator) [[likely]]
        return;
    size_t size = JSFinalObject::allocationSize(structure->inlineCapacity());
    auto* allocator = subspaceFor<JSFinalObject>(*instance->vm)->allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
    RELEASE_ASSERT(allocator->cellSize() >= size);
    instance->state->immortalCells->add(structure);
    data.header = cellHeaderFor(structure);
    data.allocator = allocator;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
