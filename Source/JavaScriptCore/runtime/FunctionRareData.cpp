/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "FunctionRareData.h"

#include "BuiltinNames.h"
#include "JSCInlines.h"
#include "JSThreadsSafepoint.h"
#include "ObjectAllocationProfileInlines.h"
#include "UnlinkedFunctionExecutable.h"

#include <wtf/Scope.h>
#include <wtf/Threading.h>

namespace JSC {

const ClassInfo FunctionRareData::s_info = { "FunctionRareData"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(FunctionRareData) };

FunctionRareData* FunctionRareData::create(VM& vm, ExecutableBase* executable)
{
    FunctionRareData* rareData = new (NotNull, allocateCell<FunctionRareData>(vm)) FunctionRareData(vm, executable);
    rareData->finishCreation(vm);
    return rareData;
}

void FunctionRareData::destroy(JSCell* cell)
{
    FunctionRareData* rareData = static_cast<FunctionRareData*>(cell);
    rareData->FunctionRareData::~FunctionRareData();
}

Structure* FunctionRareData::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

template<typename Visitor>
void FunctionRareData::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    FunctionRareData* rareData = uncheckedDowncast<FunctionRareData>(cell);
    ASSERT_GC_OBJECT_INHERITS(cell, info());
    Base::visitChildren(cell, visitor);

    rareData->m_objectAllocationProfile.visitAggregate(visitor);
    rareData->m_internalFunctionAllocationProfile.visitAggregate(visitor);
    visitor.append(rareData->m_boundFunctionStructureID);
    visitor.append(rareData->m_executable);
}

DEFINE_VISIT_CHILDREN(FunctionRareData);

FunctionRareData::FunctionRareData(VM& vm, ExecutableBase* executable)
    : Base(vm, vm.functionRareDataStructure.get())
    , m_objectAllocationProfile()
    // We initialize blind so that changes to the prototype after function creation but before
    // the first allocation don't disable optimizations. This isn't super important, since the
    // function is unlikely to allocate a rare data until the first allocation anyway.
    , m_allocationProfileWatchpointSet(ClearWatchpoint)
    , m_executable(executable, WriteBarrierEarlyInit)
    , m_hasReifiedLength(false)
    , m_hasReifiedName(false)
    , m_hasModifiedLengthForBoundOrNonHostFunction(false)
    , m_hasModifiedNameForBoundOrNonHostFunction(false)
{
}

FunctionRareData::~FunctionRareData() = default;

void FunctionRareData::initializeObjectAllocationProfile(VM& vm, JSGlobalObject* globalObject, JSObject* prototype, size_t inlineCapacity, JSFunction* constructor)
{
    auto doInitialize = [&](JSObject* prototype, size_t inlineCapacity) {
    initializeAllocationProfileWatchpointSet();
    // For class constructors, we deploy a heuristics which counts private and public fields as a part of inlineCapacity.
    // Right now, static-analyzer in the BytecodeGenerator cannot know these properties because they are separate CodeBlock
    // from the normal constructors. This offers a bit better heuristics than just directly using inlineCapacity
    if (constructor) {
        size_t fieldCount = 0;
        JSObject* current = constructor;
        constexpr size_t maxSuperDepth = 32;
        for (size_t depth = 0; current && depth < maxSuperDepth; ++depth) {
            JSFunction* currentFunction = dynamicDowncast<JSFunction>(current);
            if (!currentFunction)
                break;

            auto* executableBase = currentFunction->executable();
            if (!executableBase)
                break;

            auto* executable = dynamicDowncast<FunctionExecutable>(executableBase);
            if (!executable || !executable->isClassConstructorFunction())
                break;

            JSValue initializerValue = currentFunction->getDirect(vm, vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName());
            if (initializerValue) {
                if (JSFunction* initializerFunction = dynamicDowncast<JSFunction>(initializerValue)) {
                    if (FunctionExecutable* initializerExec = initializerFunction->jsExecutable()) {
                        if (UnlinkedFunctionExecutable* unlinkedExec = initializerExec->unlinkedExecutable()) {
                            if (const auto* defs = unlinkedExec->classElementDefinitions())
                                fieldCount += defs->size();
                        }
                    }
                }
            }

            JSValue prototype = currentFunction->getPrototypeDirect();
            if (!prototype)
                break;
            current = dynamicDowncast<JSObject>(prototype);
        }
        inlineCapacity = std::max(fieldCount, inlineCapacity);
    }
    m_objectAllocationProfile.initializeProfile(vm, globalObject, this, prototype, inlineCapacity, constructor, this);
    };

    if (vm.gilOff()) [[unlikely]] {
        // UNGIL AB18-R1-F (create-this proto race), mode split. GIL-off,
        // JSFunction::ensureRareDataAndObjectAllocationProfile()'s
        // isObjectAllocationProfileInitialized() check is unsynchronized, so
        // two mutators can both reach here for the same FunctionRareData and
        // run initializeProfile() concurrently (torn (structure, prototype)
        // publish). Serialize initialization on this cell's lock.
        //
        // The claim is a tryLock POLL LOOP, not a blocking lock(): the winner
        // allocates (Structure, watchpoints) inside the critical section and
        // can park at a safepoint while holding the lock, so a loser blocked
        // in lock() would neither publish nor acknowledge a stop-the-world
        // request and would trip the STW watchdog. Instead the loser
        // alternates parkSitePollAndParkForStopTheWorld() (so it cooperates
        // with any Class-A/GC stop window that targets this VM) with yield,
        // and leaves as soon as the winner's publish becomes visible. A loser
        // therefore NEVER returns with a still-null profile: every caller of
        // ensureRareDataAndObjectAllocationProfile() (including DFG
        // operationCreateThis, which dereferences the profile's structure
        // unconditionally) observes an initialized profile on return, exactly
        // as GIL-on.
        //
        // HB edge for staleness: the prototype passed in was computed by the
        // caller OUTSIDE this lock (JSFunction::initializeRareData /
        // allocateAndInitializeRareData), so a racing .prototype write plus
        // watchpoint fire (FunctionRareData::clear(), which is a no-op on a
        // still-null profile) could otherwise slip between the caller's read
        // and our publish, leaving a stale pair re-armed. We close that by
        // RE-READING prototypeForConstruction() under the lock; clear()
        // (below) takes the same lock when it can, and when it cannot it
        // still fires the watchpoint set, and the gilOff consumer
        // (slow_path_create_this) validates the published pair against the
        // live .prototype before trusting it.
        while (!cellLock().tryLock()) {
            if (isObjectAllocationProfileInitialized())
                return;
            if (!JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld(vm))
                Thread::yield();
        }
        {
            auto unlocker = WTF::makeScopeExit([&] { cellLock().unlock(); });
            if (isObjectAllocationProfileInitialized())
                return; // Lost a completed race; the published pair wins.
            JSObject* freshPrototype = constructor ? constructor->prototypeForConstruction(vm, globalObject) : prototype;
            doInitialize(freshPrototype, inlineCapacity);
        }
        return;
    }
    doInitialize(prototype, inlineCapacity);
}

void FunctionRareData::clear(const char* reason)
{
    VM& vm = this->vm();
    if (vm.gilOff()) [[unlikely]] {
        // UNGIL AB18-R1-F: serialize with a concurrent
        // initializeObjectAllocationProfile() when possible, but NEVER block:
        // clear() runs from AllocationProfileClearingWatchpoint /
        // .prototype-write watchpoint fires, which can execute as the
        // conductor of a Class-A stop while the lock holder is parked at a
        // safepoint inside its allocation — blocking (or poll-looping) here
        // would wedge the stop and trip the watchdog. On a lost tryLock we
        // skip the profile nulling (an initializer is mid-publish; pointer
        // stores cannot tear, and the gilOff consumer in
        // slow_path_create_this validates the (structure, prototype)
        // snapshot against the live .prototype before trusting it, so a
        // stale or mixed pair degrades to the spec-faithful uncached path)
        // but STILL fire the watchpoint set below, so no JIT code keeps a
        // burned-in stale structure. fireAll stays outside the cell lock:
        // it runs the Class-A stop machinery and must not be nested inside
        // a cell lock that parked waiters depend on.
        if (cellLock().tryLock()) {
            auto unlocker = WTF::makeScopeExit([&] { cellLock().unlock(); });
            m_objectAllocationProfile.clear();
            m_internalFunctionAllocationProfile.clear();
        }
        m_allocationProfileWatchpointSet.fireAll(vm, reason);
        return;
    }
    m_objectAllocationProfile.clear();
    m_internalFunctionAllocationProfile.clear();
    m_allocationProfileWatchpointSet.fireAll(vm, reason);
}

void FunctionRareData::AllocationProfileClearingWatchpoint::fireInternal(VM&, const FireDetail&)
{
    m_rareData->clear("AllocationProfileClearingWatchpoint fired.");
}

}
