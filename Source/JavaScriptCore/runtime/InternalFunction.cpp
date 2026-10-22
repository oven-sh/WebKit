/*
 *  Copyright (C) 1999-2002 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2004-2023 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "InternalFunction.h"

#include "Debugger.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "ProxyObject.h"
#include "VMInlines.h"

#if ENABLE(AOT)
#include "AOTCompiledCallee.h"
#include "AOTRuntimeHooks.h"
#endif

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(InternalFunction);

const ClassInfo InternalFunction::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(InternalFunction) };

#if ENABLE(AOT)
// Function.prototype is such a function too, and has the names as members of its own: they stay what its own lock makes of them.
static bool refusesAsNewProperty(VM& vm, InternalFunction* function, PropertyName propertyName)
{
    return AOT::RuntimeHooks::refusesNameOnNativeConstructor(function->globalObject(), propertyName) && !isValidOffset(function->structure()->get(vm, propertyName));
}

bool InternalFunction::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    InternalFunction* function = uncheckedDowncast<InternalFunction>(cell);
    if (slot.thisValue() == JSValue(function) && refusesAsNewProperty(vm, function, propertyName)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        // Function.prototype[Symbol.hasInstance] is read-only, so the language itself refuses that store, and says nothing of it in sloppy code.
        bool isTold = slot.isStrictMode() || (slot.context() != PutPropertySlot::ReflectSet && propertyName != vm.propertyNames->hasInstanceSymbol);
        return typeError(globalObject, scope, isTold, AOT::RuntimeHooks::messageOfNameRefusedOnBuiltinFunction(propertyName));
    }
    return Base::put(cell, globalObject, propertyName, value, slot);
}

bool InternalFunction::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    if (refusesAsNewProperty(vm, uncheckedDowncast<InternalFunction>(object), propertyName)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return typeError(globalObject, scope, shouldThrow, AOT::RuntimeHooks::messageOfNameRefusedOnBuiltinFunction(propertyName));
    }
    return Base::defineOwnProperty(object, globalObject, propertyName, descriptor, shouldThrow);
}

bool InternalFunction::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    if (object->getPrototypeDirect() != prototype && AOT::RuntimeHooks::keepsPrototypeOfNativeConstructors(uncheckedDowncast<InternalFunction>(object)->globalObject())) [[unlikely]] {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        return typeError(globalObject, scope, shouldThrowIfCantSet, AOT::RuntimeHooks::messageOfPrototypeRefusedOnBuiltinFunction());
    }
    return Base::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
}
#endif

InternalFunction::InternalFunction(VM& vm, Structure* structure, NativeFunction functionForCall, NativeFunction functionForConstruct)
    : Base(vm, structure)
    , m_functionForCall(toTagged(functionUnlessRemoved(functionForCall)))
    , m_functionForConstruct(functionForConstruct ? toTagged(functionUnlessRemoved(functionForConstruct)) : callHostFunctionAsConstructor)
    , m_globalObject(structure->realm(), WriteBarrierEarlyInit)
{
    ASSERT_WITH_MESSAGE(m_functionForCall, "[[Call]] must be implemented");
    ASSERT(m_functionForConstruct);

    ASSERT(is<InternalFunction>(this));
    // JSCell::{getCallData,getConstructData} relies on the following conditions.
    ASSERT(methodTable()->getCallData == InternalFunction::info()->methodTable.getCallData);
    ASSERT(methodTable()->getConstructData == InternalFunction::info()->methodTable.getConstructData);
    ASSERT(type() == InternalFunctionType || type() == NullSetterFunctionType);
}

void InternalFunction::finishCreation(VM& vm, unsigned length, const String& name, PropertyAdditionMode nameAdditionMode)
{
    Base::finishCreation(vm);

    JSString* nameString = jsString(vm, name);
    m_originalName.set(vm, this, nameString);
    // The enumeration order is length followed by name. So, we make sure to add the properties in that order.
    if (nameAdditionMode == PropertyAdditionMode::WithStructureTransition) {
        putDirect(vm, vm.propertyNames->length, jsNumber(length), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
        putDirect(vm, vm.propertyNames->name, nameString, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    } else {
        putDirectWithoutTransition(vm, vm.propertyNames->length, jsNumber(length), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
        putDirectWithoutTransition(vm, vm.propertyNames->name, nameString, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    }

    vm.forEachDebugger([&] (Debugger& debugger) {
        debugger.didCreateInternalFunction(*this);
    });
}

template<typename Visitor>
void InternalFunction::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    InternalFunction* thisObject = uncheckedDowncast<InternalFunction>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    
    visitor.append(thisObject->m_originalName);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, InternalFunction);

String InternalFunction::name()
{
#if ASSERT_ENABLED
    auto gcOwnedData = m_originalName->tryGetValue();
    const String& name = gcOwnedData;
    ASSERT(name); // m_originalName was built from a String, and hence, there is no rope to resolve.
    return name;
#else
    return m_originalName->tryGetValue();
#endif
}

String InternalFunction::displayName(VM& vm)
{
    JSValue displayName = getDirect(vm, vm.propertyNames->displayName);
    
    if (displayName && isJSString(displayName))
        return asString(displayName)->tryGetValue();
    
    return String();
}

CallData InternalFunction::getCallData(JSCell* cell)
{
    // Keep this function OK for invocation from concurrent compilers.
    auto* function = uncheckedDowncast<InternalFunction>(cell);
    ASSERT(function->m_functionForCall);

    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = function->m_functionForCall;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

CallData InternalFunction::getConstructData(JSCell* cell)
{
    // Keep this function OK for invocation from concurrent compilers.
    CallData constructData;
    auto* function = uncheckedDowncast<InternalFunction>(cell);
    if (function->m_functionForConstruct != callHostFunctionAsConstructor) {
        constructData.type = CallData::Type::Native;
        constructData.native.function = function->m_functionForConstruct;
        constructData.native.isBoundFunction = false;
        constructData.native.isWasm = false;
    }
    return constructData;
}

String InternalFunction::calculatedDisplayName(VM& vm)
{
    String explicitName = displayName(vm);
    
    if (!explicitName.isEmpty())
        return explicitName;
    
    return name();
}

static ALWAYS_INLINE bool retainsInternalFunctionAllocationStructure(JSFunction* function)
{
    if (function->canUseAllocationProfiles())
        return true;
    if (!function->isCompiledClosure())
        return false;
#if ENABLE(AOT)
    switch (AOT::CompiledCallee::kind(function)) {
    case AOT::CompiledCallee::FunctionKind::Class:
    case AOT::CompiledCallee::FunctionKind::Generator:
    case AOT::CompiledCallee::FunctionKind::AsyncGenerator:
        return true;
    case AOT::CompiledCallee::FunctionKind::Builtin:
        return AOT::isCompiledClassObject(function);
    case AOT::CompiledCallee::FunctionKind::Sloppy:
    case AOT::CompiledCallee::FunctionKind::Strict:
    case AOT::CompiledCallee::FunctionKind::Arrow:
    case AOT::CompiledCallee::FunctionKind::Method:
    case AOT::CompiledCallee::FunctionKind::Async:
        return false;
    }
#endif
    RELEASE_ASSERT_NOT_REACHED();
}

Structure* InternalFunction::createSubclassStructure(JSGlobalObject* globalObject, JSObject* newTarget, Structure* baseClass)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalObject* baseGlobalObject = baseClass->realm();

    ASSERT(baseClass->hasMonoProto());

    // newTarget may be an InternalFunction if we were called from Reflect.construct.
    JSFunction* targetFunction = dynamicDowncast<JSFunction>(newTarget);

    if (!targetFunction || !retainsInternalFunctionAllocationStructure(targetFunction)) [[unlikely]] {
        JSValue prototypeValue = newTarget->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, nullptr);
        // .prototype getter could have triggered having a bad time so need to recheck array structures.
        if (baseGlobalObject->isHavingABadTime()) [[unlikely]] {
            if (baseGlobalObject->isOriginalArrayStructure(baseClass))
                baseClass = baseGlobalObject->arrayStructureForIndexingTypeDuringAllocation(baseClass->indexingType());
        }
        if (JSObject* prototype = dynamicDowncast<JSObject>(prototypeValue)) {
            // This only happens if someone Reflect.constructs our builtin constructor with another builtin constructor or weird .prototype property on a
            // JSFunction as the new.target. Thus, we don't care about the cost of looking up the structure from our hash table every time.
            return baseGlobalObject->structureCache().emptyStructureForPrototypeFromBaseStructure(baseGlobalObject, prototype, baseClass);
        }
        getFunctionRealm(globalObject, newTarget);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return baseClass;
    }

    FunctionRareData* rareData = targetFunction->ensureRareData(vm);
    Structure* structure = rareData->internalFunctionAllocationStructure();
    if (structure && structure->classInfoForCells() == baseClass->classInfoForCells() && structure->realm() == baseGlobalObject) [[likely]]
        return structure;

    // .prototype can't be a getter if we canUseAllocationProfiles().
    JSValue prototypeValue = targetFunction->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, nullptr);

    if (JSObject* prototype = dynamicDowncast<JSObject>(prototypeValue))
        return rareData->createInternalFunctionAllocationStructureFromBase(vm, baseGlobalObject, prototype, baseClass);

    return baseClass;
}

InternalFunction* InternalFunction::createFunctionThatMasqueradesAsUndefined(VM& vm, JSGlobalObject* globalObject, unsigned length, const String& name, NativeFunction nativeFunction)
{
    Structure* structure = Structure::create(vm, globalObject, globalObject->objectPrototype(), TypeInfo(InternalFunctionType, InternalFunction::StructureFlags | MasqueradesAsUndefined), InternalFunction::info());
    globalObject->masqueradesAsUndefinedWatchpointSet().fireAll(globalObject->vm(), "Allocated masquerading object");
    InternalFunction* function = new (NotNull, allocateCell<InternalFunction>(vm)) InternalFunction(vm, structure, nativeFunction);
    function->finishCreation(vm, length, name, PropertyAdditionMode::WithoutStructureTransition);
    return function;
}

JSGlobalObject* tryGetFunctionRealm(JSObject* object)
{
    ASSERT(object->isCallable());

    while (true) {
        if (object->inherits<JSBoundFunction>()) {
            object = uncheckedDowncast<JSBoundFunction>(object)->targetFunction();
            continue;
        }
        if (object->inherits<JSRemoteFunction>()) {
            object = uncheckedDowncast<JSRemoteFunction>(object)->targetFunction();
            continue;
        }

        if (object->type() == ProxyObjectType) {
            auto* proxy = uncheckedDowncast<ProxyObject>(object);
            if (proxy->isRevoked())
                return nullptr;
            object = proxy->target();
            continue;
        }

        return object->realm();
    }
}

// https://tc39.es/ecma262/#sec-getfunctionrealm
JSGlobalObject* getFunctionRealm(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (auto* realm = tryGetFunctionRealm(object))
        return realm;
    throwTypeError(globalObject, scope, "Cannot get function realm from revoked Proxy"_s);
    return nullptr;
}


} // namespace JSC
