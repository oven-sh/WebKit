/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeHooks.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTChainDependents.h"
#include "AOTClosure.h"
#include "AOTErrorMessages.h"
#include "AOTGenerator.h"
#include "AOTInstance.h"
#include "AOTRealmStartup.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "MicrotaskQueueInlines.h"

#if CPU(X86_64) && OS(LINUX)
#include <sys/mman.h>
#include <wtf/PageBlock.h>
#endif

namespace JSC::AOT::RuntimeHooks {

void willInitializeRealm(JSGlobalObject* realm)
{
    if (runsCompiledProgram(realm))
        reserveInstance(realm);
}

namespace {

class RefusalsOfHostFunctions final : public ImmutablePropertyNames::OutsideNames {
public:
    bool contains(UniquedStringImpl*) const final { return false; }
    String messageOfRefusal(UniquedStringImpl* uid) const final { return assignmentToNameThatBuiltinFunctionInheritsMessage(uid); }
    String messageOfRefusedPrototype() const final { return prototypeChangeOfLockedBuiltinMessage("a built-in function"_s); }
    bool refusalAlwaysThrows() const final { return true; }
};

} // anonymous namespace

// Compiled code takes `f.call`, `f.apply`, `f.bind` and `x instanceof f` on a function of the engine for the members of Function.prototype (BuiltinTable::isFunctionOfEngine).
// One list for all the native ones, on the Structure they share: nothing is done per function. A built-in function that the library writes in JavaScript refuses the names by itself: Closure::put().
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

bool keepsPrototypeOfNativeConstructors(JSGlobalObject* realm)
{
    return locksBuiltins(realm);
}

String messageOfNameRefusedOnBuiltinFunction(PropertyName name)
{
    return assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid());
}

String messageOfPrototypeRefusedOnBuiltinFunction()
{
    return prototypeChangeOfLockedBuiltinMessage("a built-in function"_s);
}

void didCreateRealm(JSGlobalObject* realm)
{
    refuseEvaluationUnlessAdmitted(realm);
    Instance* instance = Instance::of(realm);
    if (!instance) {
        installGeneratorMethodsForCompiledGenerators(realm);
        return;
    }

    makeBuiltinsImmutable(*instance);
    Instance::create(realm);
    // A host installs its globals after this and then calls hostDidCreateRealm().
    if (!BuiltinBindings::hasHost())
        didInstallBuiltins(*instance);
}

void didCreateClass(JSGlobalObject* realm, const LazyClassStructure& lazyStructure, Structure* structure, JSObject* prototype, JSObject* constructorOrNull)
{
    AOT::didCreateClass(realm, lazyStructure, structure, prototype, constructorOrNull);
}

void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue value)
{
    AOT::didCreateStaticProperty(holder, key, value);
}

void didChangePrototypeChain(VM& vm, JSObject* changed, UniquedStringImpl* nameOrNull)
{
    didChangeChainObject(vm, changed, nameOrNull);
}

void didFlattenStructure(VM& vm, Structure* structure)
{
    AOT::didFlattenStructure(vm, structure);
}

void didChangeMicrotaskQueueing(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        instance->realm.microtaskQueue = &instance->globalObject->microtaskQueue();
        instance->awaitMustSuspend = !instance->globalObject->canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries();
        if (instance->awaitMustSuspend)
            instance->coroutines.frameThatMayContinue = nullptr;
    }
}

void didChangeGlobalLexicalBindings(JSGlobalObject* realm)
{
    AOT::didChangeGlobalLexicalBindings(realm);
}

void didRefuseChange(JSGlobalObject* realm, JSObject* object, UniquedStringImpl* keyOrNull, RefusedChange change)
{
    AOT::didRefuseChange(realm, object, keyOrNull, change);
}

String messageOfRefusalByLockedObject(JSGlobalObject* realm, JSObject* object, UniquedStringImpl* key, RefusedChange change)
{
    return AOT::messageOfRefusalByLockedObject(realm, object, key, change);
}

CONCURRENT_SAFE void setLoopPollsEnabled(VM& vm, bool enabled)
{
#if CPU(X86_64) && OS(LINUX)
    VMState::LoopPollPages& loopPollPages = vm.aotState().loopPollPages;
    Locker locker { loopPollPages.lock };
    for (void* page : loopPollPages.pages)
        RELEASE_ASSERT(!mprotect(page, WTF::pageSize(), enabled ? PROT_NONE : PROT_READ | PROT_WRITE));
#else
    UNUSED_PARAM(vm);
    UNUSED_PARAM(enabled);
#endif
}

} // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
