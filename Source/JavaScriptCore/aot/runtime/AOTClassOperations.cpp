/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
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
    RELEASE_ASSERT(closure->classData() == &classLayoutAt(classes, classIndex));
    classes.defineClass(globalObject, closure, JSValue::decode(heritage), JSValue::decode(prototypeParent));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, closure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetClassPrototype, JSCell*, (Instance* instance, JSCell* classObject))
{
    constexpr Entry entry = Entry::operationAOTGetClassPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    JSValue prototype = cellArgument<Closure>(classObject, entry)->prototypeProperty(vm);
    if (!prototype || !prototype.isObject()) [[unlikely]]
        crashOnWrongOperand(entry);
    JSCell* result = asObject(prototype);
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetConstructPrototype, JSCell*, (Instance* instance, EncodedJSValue newTarget))
{
    constexpr Entry entry = Entry::operationAOTGetConstructPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* target = objectArgument(newTarget, entry);
    if (auto* closure = dynamicDowncast<Closure>(target); closure && canReadPrototypeDirectly(*instance, *closure))
        OPERATION_RETURN(scope, instance->state->objects->classes.prototypeForInstancesOf(vm, closure));
    JSValue prototype = target->get(globalObject, vm.propertyNames->prototype);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (prototype.isObject())
        OPERATION_RETURN(scope, asObject(prototype));
    JSGlobalObject* realm = getFunctionRealm(globalObject, target);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, realm->objectPrototype());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSetConstructPrototype, JSCell*, (Instance* instance, JSCell* object, JSCell* prototype, uint32_t allocation))
{
    constexpr Entry entry = Entry::operationAOTSetConstructPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    ObjectModel& objects = *instance->state->objects;
    Object* allocated = cellArgument<Object>(object, entry);
    JSObject* result = objects.classes.instanceWithPrototype(vm, allocated, cellArgument<JSObject>(prototype, entry));
    if (result == allocated)
        objects.structures.predictStructure(vm, allocation, uncheckedDowncast<CompiledStructure>(result->structure()));
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateThis, JSCell*, (Instance* instance, JSCell* constructor, JSCell* newTarget))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCreateThis);
    JSObject* constructorObject = cellArgument<JSObject>(constructor, Entry::operationAOTCreateThis);
    JSObject* newTargetObject = cellArgument<JSObject>(newTarget, Entry::operationAOTCreateThis);
    OPERATION_RETURN(scope, instance->state->objects->classes.createThis(globalObject, constructorObject, newTargetObject));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
