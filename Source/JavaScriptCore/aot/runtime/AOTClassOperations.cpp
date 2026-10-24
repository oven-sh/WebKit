/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTHeapHooks.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "InternalFunction.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static const LayoutData& classLayoutAt(const ClassTable& classes, uint32_t classIndex)
{
    const LayoutData* classLayout = classes.classWithIndex(classIndex);
    RELEASE_ASSERT(classLayout);
    return *classLayout;
}

static bool canReadPrototypeDirectly(const Instance& instance, const Closure& closure)
{
    return Instance::of(closure.structure()->realm()) == &instance && FunctionTables::hasPrototypePropertyAtCreation(closure.kind());
}

// What these operations make on demand stays on the heap and names the function: the prototype object as its constructor, the remembered allocation as its owner.
static ALWAYS_INLINE void validateFunctionIsNotInFrame(VM& vm, const JSCell* functionCell)
{
    if (validatesRuntime(vm)) [[unlikely]]
        validateFunctionCellIsNotInFrame(vm, functionCell);
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveHeritage, EncodedJSValue, (Instance* instance, EncodedJSValue heritage))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveHeritage);
    OPERATION_RETURN(scope, JSValue::encode(instance->state->objects->classes.resolveHeritage(globalObject, JSValue::decode(heritage))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewClass, JSCell*, (Instance* instance, uint32_t request, EncodedJSValue constructor, EncodedJSValue heritage, EncodedJSValue prototypeParent))
{
    constexpr Entry entry = Entry::operationAOTNewClass;
    AOT_OPERATION_BEGIN(instance, entry);
    ClassTable& classes = instance->state->objects->classes;
    uint32_t classIndex = requestAt<NewClassRequest>(instance, request).classIndex;
    Closure* closure = cellArgument<Closure>(objectArgument(constructor, entry), entry);
    validateFunctionIsNotInFrame(vm, closure);
    const LayoutData& classLayout = classLayoutAt(classes, classIndex);
    RELEASE_ASSERT(closure->function() == FunctionID(classLayout.classData().classConstructor));
    classes.defineClass(globalObject, closure, classLayout, JSValue::decode(heritage), JSValue::decode(prototypeParent));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, closure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetClassPrototype, JSCell*, (Instance* instance, JSCell* classObject))
{
    constexpr Entry entry = Entry::operationAOTGetClassPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    validateFunctionIsNotInFrame(vm, classObject);
    JSValue prototype = cellArgument<Closure>(classObject, entry)->prototypeProperty(vm);
    if (!prototype || !prototype.isObject()) [[unlikely]]
        crashOnWrongOperand(entry);
    JSCell* result = asObject(prototype);
    OPERATION_RETURN(scope, result);
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES getConstructPrototypeByLookup(Instance*, EncodedJSValue newTarget);

JSC_DEFINE_JIT_OPERATION(operationAOTGetConstructPrototype, JSCell*, (Instance* instance, EncodedJSValue newTarget))
{
    if (JSValue target = JSValue::decode(newTarget); target.isCell()) [[likely]] {
        if (JSObject* prototype = instance->state->objects->classes.tryReadPrototypePropertyAtRememberedPlace(target.asCell())) [[likely]]
            return { prototype, nullptr };
    }
    [[clang::musttail]] return getConstructPrototypeByLookup(instance, newTarget);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES getConstructPrototypeByLookup(Instance* instance, EncodedJSValue newTarget)
{
    constexpr Entry entry = Entry::operationAOTGetConstructPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* target = objectArgument(newTarget, entry);
    validateFunctionIsNotInFrame(vm, target);
    if (auto* closure = dynamicDowncast<Closure>(target); closure && canReadPrototypeDirectly(*instance, *closure)) {
        ClassTable& classes = instance->state->objects->classes;
        JSObject* prototype = classes.prototypeForInstancesOf(vm, closure);
        classes.rememberPlaceOfPrototypeProperty(vm, closure, prototype);
        OPERATION_RETURN(scope, prototype);
    }
    JSValue prototype = target->get(globalObject, vm.propertyNames->prototype);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (prototype.isObject())
        OPERATION_RETURN(scope, asObject(prototype));
    JSGlobalObject* realm = getFunctionRealm(globalObject, target);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, realm->objectPrototype());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSetConstructPrototype, JSCell*, (Instance* instance, JSCell* object, JSCell* prototype, uint32_t allocation, uint32_t holdsNoField))
{
    constexpr Entry entry = Entry::operationAOTSetConstructPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGC deferGC(vm);
    ObjectModel& objects = *instance->state->objects;
    Object* allocated = cellArgument<Object>(object, entry);
    JSObject* result = objects.classes.instanceWithPrototype(vm, allocated, cellArgument<JSObject>(prototype, entry), !!holdsNoField);
    if (result == allocated)
        objects.structures.predictStructure(vm, allocation, uncheckedDowncast<CompiledStructure>(result->structure()));
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateThis, JSCell*, (Instance* instance, JSCell* constructor, JSCell* newTarget))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateThis);
    JSObject* constructorObject = cellArgument<JSObject>(constructor, Entry::operationAOTCreateThis);
    JSObject* newTargetObject = cellArgument<JSObject>(newTarget, Entry::operationAOTCreateThis);
    validateFunctionIsNotInFrame(vm, constructorObject);
    validateFunctionIsNotInFrame(vm, newTargetObject);
    OPERATION_RETURN(scope, instance->state->objects->classes.createThis(globalObject, constructorObject, newTargetObject));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
