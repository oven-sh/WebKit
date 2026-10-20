/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeHooks.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTChainDependents.h"
#include "AOTInstance.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeCounters.h"
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

void didCreateRealm(JSGlobalObject* realm)
{
    refuseEvaluationUnlessAdmitted(realm);
    Instance* instance = Instance::of(realm);
    if (!instance)
        return;

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

void didClearPristineBits(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        instance->bumpShapeEpoch();
        count(*instance, RuntimeCounterID::ShapeEpochBumps);
    }
}

void didChangeMicrotaskQueueing(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        instance->realm.microtaskQueue = &instance->globalObject->microtaskQueue();
        instance->awaitMustSuspend = !instance->globalObject->canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries();
    }
}

void didChangeGlobalLexicalBindings(JSGlobalObject* realm)
{
    AOT::didChangeGlobalLexicalBindings(realm);
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
