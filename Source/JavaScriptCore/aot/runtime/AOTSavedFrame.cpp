/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedFrame.h"

#if ENABLE(AOT)

#include "AOTCoroutineTable.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTState.h"
#include "AOTStubFrames.h"
#include "HeapUtil.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo SavedFrame::s_info = { "SavedFrame"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SavedFrame) };

Structure* SavedFrame::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
}

CallData SavedFrame::getCallData(JSCell* cell)
{
    JSGlobalObject* realm = uncheckedDowncast<SavedFrame>(cell)->realm();
    return CallData::forCompiledCode(Instance::of(realm)->state->coroutines->savedFrameEntrypoint(), realm);
}

SavedFrame* SavedFrame::forFrame(const Instance& instance, const uint64_t* frame)
{
    if (std::bit_cast<const void*>(frame[1]) != instance.stubs.returnAddress(StubFrameKind::SavedFrameAdapter))
        return nullptr;
    return uncheckedDowncast<SavedFrame>(std::bit_cast<CallFrame*>(frame[0])->jsCallee());
}

SavedFrame* SavedFrame::create(VM& vm, Instance& instance, CoroutineKind kind, size_t numberOfWords)
{
    CoroutineTable& coroutineTable = *instance.state->coroutines;
    Structure* structure = coroutineTable.savedFrameStructure();
    SavedFrame* saved = new (NotNull, allocateCell<SavedFrame>(vm, allocationSize(numberOfWords))) SavedFrame(vm, structure, kind, static_cast<uint32_t>(numberOfWords));
    saved->finishCreation(vm);
    saved->m_promiseOrWaiter = saved;
    coroutineTable.didCreateSavedFrame(vm, saved);
    return saved;
}

SavedFrame* SavedFrame::saveAsyncFunction(VM& vm, Instance& instance, const SuspendedMachineFrame& machineFrame)
{
    SavedFrame* saved = forFrame(instance, machineFrame.frame);
    if (!saved) {
        saved = create(vm, instance, CoroutineKind::AsyncFunction, machineFrame.numberOfWords());
        uint32_t promiseLocation = promiseLocationOf(machineFrame.words->siteWord);
        RELEASE_ASSERT(preservedRegister != InvalidGPRReg || promiseLocation != locationInRegister);
        uint64_t promise = promiseLocation == locationInRegister ? machineFrame.words->preservedRegister : machineFrame.frame[-static_cast<intptr_t>(promiseLocation)];
        if (promise) {
            saved->m_promiseOrWaiter = std::bit_cast<JSCell*>(promise);
            saved->m_continuation = Continuation::Promise;
        }
    }
    saved->save(vm, machineFrame, SuspensionKind::Await);
    return saved;
}

void SavedFrame::save(VM& vm, const SuspendedMachineFrame& machineFrame, SuspensionKind kind)
{
    size_t numberOfWords = machineFrame.numberOfWords();
    uint64_t siteWord = machineFrame.words->siteWord;
    RELEASE_ASSERT(m_numberOfWords == numberOfWords && !m_liveSlotMapOffsetPlusOne);
    willWrite();
    uint64_t* words = this->words();
    for (size_t i = 0; i < numberOfWords; ++i)
        words[i] = machineFrame.bottom[i];
    if constexpr (preservedRegister != InvalidGPRReg)
        m_preservedRegister = machineFrame.words->preservedRegister;
    m_numberOfSavedRegisterPairs = static_cast<uint8_t>(savedRegistersOrPairsOf(siteWord));
    m_resumeAddress = machineFrame.resumeAddress;
    m_flags = kind == SuspensionKind::Yield ? isSuspendedAtYieldFlag : 0;
    m_liveSlotMapOffsetPlusOne = liveSlotMapOffsetOf(siteWord) + 1;
    didWrite();
    vm.writeBarrier(this);
}

std::optional<SavedFrameMap> SavedFrame::liveSlotMap(uint32_t offsetPlusOne) const
{
    const Image* image = Image::current();
    // The byte before a return address is in the calling function. The return address itself may start the next one.
    CodeLocation where = image->locate(static_cast<const uint8_t*>(m_resumeAddress) - 1);
    if (where.kind != CodeKind::Function)
        return std::nullopt;
    return SavedFrameMap::tryCreate(image->frameMapWords(where.function), offsetPlusOne - 1);
}

template<typename Visitor>
void SavedFrame::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitFields(uncheckedDowncast<SavedFrame>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(SavedFrame);

uint64_t* SavedFrame::addressOfWordAt(uint32_t location)
{
    if (location == locationInRegister)
        return &m_preservedRegister;
    return &words()[m_numberOfWords - location];
}

void SavedFrame::poisonDeadUnlistedWords(VM& vm)
{
    static_assert(locationHolding(2 * 3) == 3 && !isInUpperHalf(2 * 3) && locationHolding(2 * 3 - 1) == 3 && isInUpperHalf(2 * 3 - 1));
    if (isOnStack())
        return;
    std::optional<SavedFrameMap> map = liveSlotMap(m_liveSlotMapOffsetPlusOne);
    RELEASE_ASSERT(map);
    auto poisonedIfDead = [&](uint32_t half) {
        return HeapUtil::classifyNarrowReference(vm.heap, half) == HeapUtil::WordKind::UnmarkedCell ? poisonedNarrowReference : half;
    };
    auto poisonDeadUnlistedWord = [&](uint32_t location) {
        if (map->listsValue(location))
            return;
        bool hasUpperNarrowLocation = location != locationInRegister;
        bool listsLowerHalf = map->listsNarrowReference(2 * location);
        bool listsUpperHalf = hasUpperNarrowLocation && map->listsNarrowReference(2 * location - 1);
        uint64_t& word = *addressOfWordAt(location);
        if (!listsLowerHalf && !listsUpperHalf && HeapUtil::classifyWord(vm.heap, std::bit_cast<const void*>(word)) == HeapUtil::WordKind::UnmarkedCell) {
            word = poisonedWord;
            return;
        }
        uint32_t lowerHalf = static_cast<uint32_t>(word);
        uint32_t upperHalf = static_cast<uint32_t>(word >> 32);
        if (!listsLowerHalf)
            lowerHalf = poisonedIfDead(lowerHalf);
        if (hasUpperNarrowLocation && !listsUpperHalf)
            upperHalf = poisonedIfDead(upperHalf);
        word = static_cast<uint64_t>(upperHalf) << 32 | lowerHalf;
    };
    willWrite();
    for (uint32_t location = 1; location <= m_numberOfWords; ++location)
        poisonDeadUnlistedWord(location);
    if constexpr (preservedRegister != InvalidGPRReg)
        poisonDeadUnlistedWord(locationInRegister);
    didWrite();
}

JSPromise* SavedFrame::promise() const
{
    return m_continuation == Continuation::Promise ? uncheckedDowncast<JSPromise>(m_promiseOrWaiter) : nullptr;
}

SavedFrame* SavedFrame::waiter() const
{
    return m_continuation == Continuation::Waiter ? uncheckedDowncast<SavedFrame>(m_promiseOrWaiter) : nullptr;
}

void SavedFrame::setWaiter(VM& vm, SavedFrame* waiter, JSValue asyncContext)
{
    RELEASE_ASSERT(m_continuation == Continuation::None);
    ASSERT(m_promiseOrWaiter == this);
    waiter->m_asyncContext = JSValue::encode(asyncContext ? asyncContext : jsUndefined());
    vm.writeBarrier(waiter);
    m_promiseOrWaiter = waiter;
    m_continuation = Continuation::Waiter;
    vm.writeBarrier(this);
}

void SavedFrame::fulfill(JSGlobalObject* globalObject, VM& vm, JSValue value)
{
    switch (m_continuation) {
    case Continuation::Waiter: {
        auto* waiter = uncheckedDowncast<SavedFrame>(m_promiseOrWaiter);
        globalObject->queueMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), value, waiter, JSValue::decode(waiter->m_asyncContext));
        return;
    }
    case Continuation::None:
    case Continuation::Promise:
        ensurePromise(globalObject, vm)->fulfill(vm, value);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrame::reject(JSGlobalObject* globalObject, VM& vm, JSValue error)
{
    switch (m_continuation) {
    case Continuation::Waiter: {
        auto* waiter = uncheckedDowncast<SavedFrame>(m_promiseOrWaiter);
        globalObject->queueMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, static_cast<uint8_t>(JSPromise::Status::Rejected), jsUndefined(), error, waiter, JSValue::decode(waiter->m_asyncContext));
        return;
    }
    case Continuation::None:
    case Continuation::Promise:
        ensurePromise(globalObject, vm)->reject(vm, error);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSPromise* SavedFrame::ensurePromise(JSGlobalObject* globalObject, VM& vm)
{
    if (JSPromise* promise = this->promise())
        return promise;
    ASSERT(m_continuation != Continuation::None || m_promiseOrWaiter == this);
    SavedFrame* waiter = this->waiter();
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    if (waiter)
        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, nullptr, waiter, JSValue::decode(waiter->m_asyncContext));
    m_promiseOrWaiter = promise;
    m_continuation = Continuation::Promise;
    vm.writeBarrier(this);
    return promise;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
