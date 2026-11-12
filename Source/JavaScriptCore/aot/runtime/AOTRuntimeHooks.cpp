/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeHooks.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTBuiltinModules.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTClosure.h"
#include "AOTErrorMessages.h"
#include "AOTGenerator.h"
#include "AOTInstance.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTState.h"
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

String lockedBuiltinPropertyErrorMessage(PropertyName name)
{
    return assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid());
}

String lockedBuiltinPrototypeErrorMessage()
{
    return prototypeChangeOfLockedBuiltinMessage("a built-in function"_s);
}

namespace {

class RefusalsOfHostFunctions final : public ImmutablePropertyNames::OutsideNames {
public:
    bool contains(UniquedStringImpl*) const final { return false; }
    String storeErrorMessage(UniquedStringImpl* uid) const final { return lockedBuiltinPropertyErrorMessage(uid); }
    String setPrototypeErrorMessage() const final { return lockedBuiltinPrototypeErrorMessage(); }
    bool throwsInSloppyMode() const final { return true; }
};

} // anonymous namespace

Structure* structureForHostFunctions(JSGlobalObject* realm, Structure* structure)
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

bool rejectsNativeConstructorProperty(JSGlobalObject* realm, PropertyName name)
{
    return !name.isPrivateName() && Closure::isNameThatBuiltinFunctionsRefuse(realm->vm(), name.uid()) && locksBuiltins(realm);
}

static bool isBinaryDataClass(JSGlobalObject* realm, JSObject* constructor)
{
    if (realm->arrayBufferConstructor(ArrayBufferSharingMode::Default) == constructor)
        return true;
#define RETURN_IF_CONSTRUCTOR(name) \
    if (realm->typedArrayConstructorConcurrently(Type##name) == constructor) \
        return true;
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(RETURN_IF_CONSTRUCTOR)
#undef RETURN_IF_CONSTRUCTOR
    return false;
}

bool acceptsHasInstanceOfHost(InternalFunction* constructor, PropertyName name, JSValue method)
{
    JSGlobalObject* realm = constructor->globalObject();
    Instance* instance = realm->aotInstance();
    if (name != realm->vm().propertyNames->hasInstanceSymbol || !method || !instance || !instance->state || !instance->state->acceptsForeignBinaryData)
        return false;
    auto* function = dynamicDowncast<JSFunction>(method);
    return function && function->isBuiltinFunction() && isBinaryDataClass(realm, constructor);
}

void crashOnHavingABadTime()
{
    RuntimeCheckFailure failure("array prototype check failed"_s);
    failure.detail("an indexed accessor or a read-only indexed property was added to the prototype chain of arrays, which the locked built-ins should have refused");
    failure.report();
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
    rejectEvaluationUnlessAllowed(realm);
    Instance* instance = Instance::of(realm);
    if (!instance || (!BuiltinBindings::hasHost() && !hasBuiltinFunction(realm->vm(), static_cast<uint32_t>(BuiltinCodeIndex::generatorPrototypeNextCode))))
        installGeneratorMethodsForCompiledGenerators(realm);
    if (!instance) {
        for (Instance* other : realm->vm().aotState().instances) {
            if (other->state) {
                realm->setAOTInstanceOfTables(other);
                break;
            }
        }
        return;
    }

    makeBuiltinsImmutable(*instance);
    Instance::create(realm);
    if (BuiltinBindings::hasHost())
        return;
    didInstallBuiltins(*instance);
    installDeferredWatchpoints(realm);
}

void willRecompileRegExp(VM& vm)
{
    for (Instance* instance : vm.aotState().instances)
        instance->regExpsAreRecompiled = true;
}

void didMoveStorageOf(VM& vm, JSCell* view)
{
    for (Instance* instance : vm.aotState().instances)
        instance->state->statics->didMoveStorageOf(view);
}

void didChangeMicrotaskQueueing(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        instance->realm.microtaskQueue = &instance->globalObject->microtaskQueue();
        instance->awaitMustSuspend = !instance->globalObject->canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries() || vm.m_synchronousModuleQueue;
        if (instance->awaitMustSuspend)
            takeAwayRightToContinue(*instance);
    }
    if (vm.hostObservesMicrotaskBoundaries() && vm.aotState().modulesWithEagerLoads) [[unlikely]]
        transferEagerLoadsToLoader(vm);
}

CONCURRENT_SAFE void setLoopPollsEnabled(VM& vm, bool enabled)
{
    VMState::PolledInstances& polled = vm.aotState().polledInstances;
    Locker locker { polled.lock };
    for (Instance* instance : polled.instances)
        setPollsEnabled(*instance, enabled);
}

constinit std::atomic<bool> wasNoHelperThreadsScopeEntered { false };
static thread_local unsigned t_noHelperThreadsScopes;

NoHelperThreadsScope::NoHelperThreadsScope()
{
    wasNoHelperThreadsScopeEntered.store(true, std::memory_order_relaxed);
    ++t_noHelperThreadsScopes;
}

NoHelperThreadsScope::~NoHelperThreadsScope()
{
    --t_noHelperThreadsScopes;
}

bool isThreadInNoHelperThreadsScope()
{
    return t_noHelperThreadsScopes;
}

} // namespace JSC::AOT::RuntimeHooks

#endif // ENABLE(AOT)
