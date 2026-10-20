/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEvalScope.h"

#if ENABLE(AOT)

#include "BuiltinNames.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

const ClassInfo EvalScope::s_info = { "EvalScope"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(EvalScope) };

EvalScope::EvalScope(VM& vm, Structure* structure, JSScope* next, JSObject* bindings, JSValue newTarget)
    : Base(vm, structure, next)
    , m_bindings(bindings, WriteBarrierEarlyInit)
    , m_newTarget(newTarget, WriteBarrierEarlyInit)
{
}

Structure* EvalScope::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(StrictEvalActivationType, StructureFlags), info());
}

EvalScope* EvalScope::create(VM& vm, Structure* structure, JSScope* next, JSObject* bindings, JSValue newTarget)
{
    EvalScope* scope = new (NotNull, allocateCell<EvalScope>(vm)) EvalScope(vm, structure, next, bindings, newTarget);
    scope->finishCreation(vm);
    return scope;
}

template<typename Visitor>
void EvalScope::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* scope = uncheckedDowncast<EvalScope>(cell);
    Base::visitChildren(scope, visitor);
    visitor.append(scope->m_bindings);
    visitor.append(scope->m_newTarget);
}

DEFINE_VISIT_CHILDREN(EvalScope);

bool EvalScope::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto* scope = uncheckedDowncast<EvalScope>(object);
    if (name == vm.propertyNames->builtinNames().newTargetLocalPrivateName()) {
        JSValue newTarget = scope->m_newTarget.get();
        if (!newTarget)
            return false;
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete | PropertyAttribute::DontEnum, newTarget);
        return true;
    }
    JSObject* bindings = scope->m_bindings.get();
    return bindings->methodTable()->getOwnPropertySlot(bindings, globalObject, name, slot);
}

bool EvalScope::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    JSObject* bindings = uncheckedDowncast<EvalScope>(cell)->m_bindings.get();
    PutPropertySlot bindingsSlot(bindings, slot.isStrictMode());
    return bindings->methodTable()->put(bindings, globalObject, name, value, bindingsSlot);
}

bool EvalScope::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot&)
{
    return !uncheckedDowncast<EvalScope>(cell)->m_bindings->hasOwnProperty(globalObject, name);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
