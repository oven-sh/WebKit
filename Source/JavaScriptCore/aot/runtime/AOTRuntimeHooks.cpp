/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeHooks.h"

#if ENABLE(AOT)

#include "AOTBuiltinModules.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTClosure.h"
#include "AOTErrorMessages.h"
#include "AOTGenerator.h"
#include "AOTInstance.h"
#include "BuiltinExecutables.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "MicrotaskQueueInlines.h"

#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

namespace JSC::AOT::RuntimeHooks {

void willInitializeRealm(JSGlobalObject* realm)
{
    if (runsCompiledProgram(realm))
        reserveInstance(realm);
}

String messageOfNameRefusedOnBuiltinFunction(PropertyName name)
{
    return assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid());
}

String messageOfPrototypeRefusedOnBuiltinFunction()
{
    return prototypeChangeOfLockedBuiltinMessage("a built-in function"_s);
}

namespace {

class RefusalsOfHostFunctions final : public ImmutablePropertyNames::OutsideNames {
public:
    bool contains(UniquedStringImpl*) const final { return false; }
    String storeErrorMessage(UniquedStringImpl* uid) const final { return messageOfNameRefusedOnBuiltinFunction(uid); }
    String setPrototypeErrorMessage() const final { return messageOfPrototypeRefusedOnBuiltinFunction(); }
    bool throwsInSloppyMode() const final { return true; }
};

} // anonymous namespace

// Compiled code takes `f.call`, `f.apply`, `f.bind` and `x instanceof f` on a function of the engine for the members of Function.prototype (BuiltinTable::isFunctionOfEngine).
Structure* structureOfHostFunctions(JSGlobalObject* realm, Structure* structure)
{
    if (!locksBuiltins(realm))
        return structure;
    VM& vm = realm->vm();
    IdentifierSet names;
    for (const Identifier& name : { Identifier::fromString(vm, "call"_s), Identifier::fromString(vm, "apply"_s), vm.propertyNames->bind, vm.propertyNames->hasInstanceSymbol })
        names.add(name.impl());
    Ref<ImmutablePropertyNames> closed = ImmutablePropertyNames::create(WTF::move(names), ImmutablePropertyNames::Part::Prototype, { }, adoptRef(*new RefusalsOfHostFunctions));
    return Structure::makeListedPropertiesImmutableTransition(vm, structure, closed.get());
}

bool refusesNameOnNativeConstructor(JSGlobalObject* realm, PropertyName name)
{
    return !name.isPrivateName() && Closure::isNameThatBuiltinFunctionsRefuse(realm->vm(), name.uid()) && locksBuiltins(realm);
}

void didCreateRealm(JSGlobalObject* realm)
{
    refuseEvaluationUnlessAdmitted(realm);
    Instance* instance = Instance::of(realm);
    // The engine's text of next(), return() and throw() takes the engine's own generators alone. A host brings the three compiled.
    if (!instance || (!BuiltinBindings::hasHost() && !hasBuiltinFunction(realm->vm(), static_cast<uint32_t>(BuiltinCodeIndex::generatorPrototypeNextCode))))
        installGeneratorMethodsForCompiledGenerators(realm);
    if (!instance)
        return;

    Instance::forEachOf(realm, [&](Instance& each) {
        makeBuiltinsImmutable(each);
        Instance::create(realm);
        // A host installs its globals after this and then calls hostDidCreateRealm().
        if (!BuiltinBindings::hasHost())
            didInstallBuiltins(each);
    });
}

void didChangeMicrotaskQueueing(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        instance->realm.microtaskQueue = &instance->globalObject->microtaskQueue();
        instance->awaitMustSuspend = !instance->globalObject->canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries() || vm.m_synchronousModuleQueue;
        if (instance->awaitMustSuspend)
            instance->coroutines.frameThatMayContinue = nullptr;
    }
}

CONCURRENT_SAFE void setLoopPollsEnabled(VM& vm, bool enabled)
{
    VMState::PolledInstances& polled = vm.aotState().polledInstances;
    Locker locker { polled.lock };
    for (Instance* instance : polled.instances)
        setPollsEnabled(*instance, enabled);
}

} // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
