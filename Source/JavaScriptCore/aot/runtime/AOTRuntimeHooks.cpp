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
#include "ObjectPropertyCondition.h"

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

void stopBecauseRealmHasABadTime()
{
    dataLogLn("AOT: the realm of a compiled program is having a bad time: an indexed accessor or a read-only index has come to lie above its arrays, which its locked built-ins refuse. Compiled code would go on making arrays that pass it by.");
    CRASH();
}

namespace {

struct DeferredWatchpoints {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(DeferredWatchpoints);

    struct Watchpoint {
        ObjectPropertyCondition key;
        InlineWatchpointSet& set;
        DeferredWatchpointKind kind;
    };

    JSGlobalObject* realm;
    bool takesMore { true };
    std::unique_ptr<DeferredWatchpoints> ofRealmBeingCreatedAround;
    Vector<Watchpoint, 128> watchpoints;
};

} // anonymous namespace

static thread_local DeferredWatchpoints* deferredWatchpointsOfThread { nullptr };

void willWatchBuiltins(JSGlobalObject* realm)
{
    if (!realm->aotInstance())
        return;
    deferredWatchpointsOfThread = new DeferredWatchpoints { realm, true, std::unique_ptr<DeferredWatchpoints>(deferredWatchpointsOfThread), { } };
}

static bool hasDeferredWatchpoints(JSGlobalObject* realm)
{
    return deferredWatchpointsOfThread && deferredWatchpointsOfThread->realm == realm;
}

void didWatchBuiltins(JSGlobalObject* realm)
{
    if (hasDeferredWatchpoints(realm))
        deferredWatchpointsOfThread->takesMore = false;
}

bool defersWatchpoints(JSGlobalObject* realm)
{
    return hasDeferredWatchpoints(realm) && deferredWatchpointsOfThread->takesMore;
}

bool deferWatchpoint(JSGlobalObject* realm, const ObjectPropertyCondition& key, InlineWatchpointSet& set, DeferredWatchpointKind kind)
{
    if (!defersWatchpoints(realm))
        return false;
    deferredWatchpointsOfThread->watchpoints.append({ key, set, kind });
    return true;
}

static bool holdsAndIsKeptByLock(const ObjectPropertyCondition& key)
{
    Structure* structure = key.object()->structure();
    if (!structure->hasImmutableProperties() || structure->needImpurePropertyWatchpoint())
        return false;
    switch (key.kind()) {
    case PropertyCondition::Equivalence:
    case PropertyCondition::Absence:
    case PropertyCondition::AbsenceOfIndexedProperties:
        return key.condition().isStillValidAssumingImpurePropertyWatchpoint(Concurrency::MainThread, structure, key.object());
    default:
        return false;
    }
}

void installDeferredWatchpoints(JSGlobalObject* realm)
{
    if (!hasDeferredWatchpoints(realm))
        return;
    std::unique_ptr<DeferredWatchpoints> deferred(deferredWatchpointsOfThread);
    deferredWatchpointsOfThread = deferred->ofRealmBeingCreatedAround.release();
    for (auto& watchpoint : deferred->watchpoints) {
        if (holdsAndIsKeptByLock(watchpoint.key))
            continue;
        if (!watchpoint.key.isWatchable(PropertyCondition::EnsureWatchability))
            watchpoint.set.fireAll(realm->vm(), "A built-in changed while its realm was created");
        else if (watchpoint.kind == DeferredWatchpointKind::PropertyChange)
            realm->installObjectPropertyChangeAdaptiveWatchpoint(watchpoint.key, watchpoint.set);
        else
            realm->installObjectAdaptiveStructureWatchpoint(watchpoint.key, watchpoint.set);
    }
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
    if (!BuiltinBindings::hasHost())
        installDeferredWatchpoints(realm);
}

void didChangeMicrotaskQueueing(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        instance->realm.microtaskQueue = &instance->globalObject->microtaskQueue();
        instance->awaitMustSuspend = !instance->globalObject->canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries() || vm.m_synchronousModuleQueue;
        if (instance->awaitMustSuspend)
            instance->coroutines.frameThatMayContinue = nullptr;
    }
    if (vm.hostObservesMicrotaskBoundaries() && vm.aotState().modulesWhoseFilesRunAheadOfLoader) [[unlikely]]
        runFilesThatWaitInLoader(vm);
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
