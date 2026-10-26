/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutineTable.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTGenerator.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeValidation.h"
#include "AOTSavedFrame.h"
#include "AOTState.h"
#include "AOTStubFrames.h"
#include "AOTStubsCoroutines.h"
#include "AbstractSlotVisitorInlines.h"
#include "AsyncGeneratorPrototype.h"
#include "GeneratorPrototype.h"
#include "HeapUtil.h"
#include "InternalFunction.h"
#include "IteratorOperations.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSSentinel.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "ObjectConstructor.h"
#include "SlotVisitorInlines.h"
#include "VMInlines.h"
#include <wtf/CompactPointerTuple.h>
#include <wtf/Threading.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

CoroutineTable::CoroutineTable(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_savedFrameEntrypoint(AOT::savedFrameEntrypoint().taggedPtr())
    , m_handlerOfJobEntrypoint(AOT::handlerOfJobEntrypoint().taggedPtr())
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* globalObject = m_instance.globalObject;
    m_generatorStructure = Generator::createStructure(vm, globalObject, globalObject->generatorPrototype());
    m_state.immortalCells->add(m_generatorStructure);
    m_savedFrameStructure = SavedFrame::createStructure(vm, globalObject);
    m_state.immortalCells->add(m_savedFrameStructure);
    m_instance.coroutines.savedFrameHeader = cellHeaderFor(m_savedFrameStructure);
    m_instance.coroutines.emptyGeneratorPrototypeStructureID = createEmptyPrototypeStructure(vm, globalObject->generatorPrototype());
    m_instance.coroutines.emptyAsyncGeneratorPrototypeStructureID = createEmptyPrototypeStructure(vm, globalObject->asyncGeneratorPrototype());
    initializeCoroutineWords(vm);
    state.caches->add(CacheKind::StrongRoots, "coroutine resume state"_s, *this, RootVolatility::GreyedByExecution);
    state.caches->add(CacheKind::WeakByKey, "saved frames"_s, *this);
}

uint32_t CoroutineTable::createEmptyPrototypeStructure(VM& vm, JSObject* parent)
{
    JSObject* probe = constructEmptyObject(m_instance.globalObject, parent);
    probe->didBecomePrototype(vm);
    Structure* empty = probe->structure();
    m_state.immortalCells->add(empty);
    return empty->id().bits();
}

static uint64_t jobWord(JSGlobalObject* globalObject, InternalMicrotask task, JSPromise::Status status, uint8_t flags = 0)
{
    uint16_t taskAndStatus = static_cast<uint16_t>(task) | static_cast<uint16_t>(static_cast<uint8_t>(status) | flags) << 8;
    return CompactPointerTuple<JSCell*, uint16_t>(globalObject, taskAndStatus).data();
}

void CoroutineTable::initializeCoroutineWords(VM& vm)
{
    CoroutineWords& words = m_instance.coroutines;
    words.savedFramesAreListed = validatesRuntime(vm);
    words.resumeJobWords[0] = jobWord(m_instance.globalObject, InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled);
    words.resumeJobWords[1] = jobWord(m_instance.globalObject, InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Rejected);
    words.bodyCallJobWords[0] = jobWord(m_instance.globalObject, InternalMicrotask::AsyncGeneratorBodyCallNormal, JSPromise::Status::Fulfilled);
    words.bodyCallJobWords[1] = jobWord(m_instance.globalObject, InternalMicrotask::AsyncGeneratorBodyCallNormal, JSPromise::Status::Rejected);
    words.yieldAwaitedJobWord = jobWord(m_instance.globalObject, InternalMicrotask::AsyncGeneratorYieldAwaited, JSPromise::Status::Fulfilled);
    words.driverResumeJobWord = jobWord(m_instance.globalObject, InternalMicrotask::AsyncGeneratorDriverResume, JSPromise::Status::Fulfilled);
    words.reactionJobWords[0] = jobWord(m_instance.globalObject, InternalMicrotask::PromiseReactionJob, JSPromise::Status::Fulfilled);
    words.reactionJobWords[1] = jobWord(m_instance.globalObject, InternalMicrotask::PromiseReactionJob, JSPromise::Status::Rejected);
    words.reactionJobWords[2] = jobWord(m_instance.globalObject, InternalMicrotask::PromiseReactionJob, JSPromise::Status::Fulfilled, promiseReactionJobAsyncContextFlag);
    words.reactionJobWords[3] = jobWord(m_instance.globalObject, InternalMicrotask::PromiseReactionJob, JSPromise::Status::Rejected, promiseReactionJobAsyncContextFlag);
    words.performMicrotaskJobWord = jobWord(m_instance.globalObject, InternalMicrotask::BunPerformMicrotaskJob, JSPromise::Status::Pending);
    words.promiseAllResolveJobWord = jobWord(m_instance.globalObject, InternalMicrotask::PromiseAllResolveJob, JSPromise::Status::Fulfilled);
    words.asyncGeneratorAllocator = subspaceFor<JSAsyncGenerator>(vm)->allocatorFor(sizeof(JSAsyncGenerator), AllocatorForMode::EnsureAllocator).localAllocator();
    words.asyncGeneratorSentinel = vm.fastAsyncGeneratorSentinel();
}

template<typename Visitor>
void CoroutineTable::visitRoots(Visitor& visitor)
{
    CoroutineWords& words = m_instance.coroutines;
    visitor.appendUnbarriered(words.waiterToResume);
    visitor.appendUnbarriered(JSValue::decode(words.waiterResolution));
    visitor.appendUnbarriered(JSValue::decode(words.waiterAsyncContext));
}

template void CoroutineTable::visitRoots(AbstractSlotVisitor&);
template void CoroutineTable::visitRoots(SlotVisitor&);

size_t CoroutineTable::removeDeadEntries(VM& vm, CollectionScope)
{
    // A dead word goes back onto the stack with the frame. There it must not name the next tenant of a cell that this collection frees.
    const uint64_t replacement = validatesRuntime(vm) ? SavedFrame::poisonedWord : 0;
    for (ListOfVisitedFrames& list : m_visitedFramesWithDeadWords) {
        Locker locker { list.lock };
        constexpr size_t prefetchDistance = 8;
        for (size_t index = 0; index < list.frames.size(); ++index) {
            if (index + prefetchDistance < list.frames.size()) {
                __builtin_prefetch(list.frames[index + prefetchDistance].frame, 1);
                __builtin_prefetch(list.frames[index + prefetchDistance].frame + 1, 1);
            }
            const VisitedFrameWithDeadWords& visited = list.frames[index];
            visited.frame->overwriteDeadWords(visited.version, SavedFrameMap::fromFirstWord(visited.firstWordOfMap), replacement);
        }
        list.frames.clear();
    }

    size_t numberOfDeadFrames = m_savedFramesForValidation.removeAllMatching([&](SavedFrame* frame) {
        return !vm.heap.isMarked(frame);
    });
    for (SavedFrame* frame : m_savedFramesForValidation)
        frame->poisonDeadUnlistedWords(vm);
    return numberOfDeadFrames;
}

void CoroutineTable::didVisitFrameWithDeadWords(SavedFrame* frame, uint32_t version, const uint32_t* firstWordOfMap)
{
    ListOfVisitedFrames& list = m_visitedFramesWithDeadWords[Thread::currentSingleton().uid() % m_visitedFramesWithDeadWords.size()];
    Locker locker { list.lock };
    list.frames.append({ frame, firstWordOfMap, version });
}

void CoroutineTable::didCreateSavedFrame(VM& vm, SavedFrame* frame)
{
    if (validatesRuntime(vm)) [[unlikely]]
        m_savedFramesForValidation.append(frame);
}

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

SavedFrame* SavedFrame::create(VM& vm, Instance& instance, CoroutineKind kind, size_t numberOfWords, uint32_t savedRegistersOrPairs, uint32_t skippedAtBottom)
{
    RELEASE_ASSERT(numberOfWords <= maxNumberOfWords && !(savedRegistersOrPairs >> savedRegisterBits) && skippedAtBottom <= maxNumberOfWords);
    CoroutineTable& coroutineTable = *instance.state->coroutines;
    SavedFrame* saved = new (NotNull, allocateCell<SavedFrame>(vm, allocationSize(numberOfWords))) SavedFrame(vm, coroutineTable.savedFrameStructure(), kind, static_cast<uint16_t>(numberOfWords), static_cast<uint8_t>(savedRegistersOrPairs), static_cast<uint16_t>(skippedAtBottom));
    saved->finishCreation(vm);
    saved->m_promiseOrWaiter = saved;
    coroutineTable.didCreateSavedFrame(vm, saved);
    return saved;
}

SavedFrame* SavedFrame::create(VM& vm, Instance& instance, CoroutineKind kind, const SuspendedMachineFrame& machineFrame)
{
    uint64_t siteWord = machineFrame.words->siteWord;
    return create(vm, instance, kind, machineFrame.numberOfWords(), savedRegistersOrPairsOf(siteWord), skippedAtBottomOf(siteWord));
}

SavedFrame* SavedFrame::createForAsyncFunction(VM& vm, Instance& instance, const SuspendedMachineFrame& machineFrame)
{
    SavedFrame* saved = create(vm, instance, CoroutineKind::AsyncFunction, machineFrame);
    uint64_t siteWord = machineFrame.words->siteWord;
    uint32_t promiseLocation = promiseLocationOf(siteWord);
    RELEASE_ASSERT(preservedRegister != InvalidGPRReg || promiseLocation != locationInRegister);
    uint64_t promise = promiseLocation == locationInRegister ? machineFrame.words->preservedRegister : machineFrame.frame[-static_cast<intptr_t>(promiseLocation)];
    if (JSValue::decode(static_cast<EncodedJSValue>(promise)).isCell()) {
        saved->m_promiseOrWaiter = std::bit_cast<JSCell*>(promise);
        saved->m_continuation = Continuation::Promise;
    }
    return saved;
}

SavedFrame* SavedFrame::saveAsyncFunction(VM& vm, Instance& instance, const SuspendedMachineFrame& machineFrame)
{
    SavedFrame* saved = forFrame(instance, machineFrame.frame);
    if (!saved)
        saved = createForAsyncFunction(vm, instance, machineFrame);
    saved->save(vm, machineFrame, SuspensionKind::Await);
    return saved;
}

void SavedFrame::save(VM& vm, const SuspendedMachineFrame& machineFrame, SuspensionKind kind)
{
    size_t numberOfWords = machineFrame.numberOfWords();
    uint64_t siteWord = machineFrame.words->siteWord;
    RELEASE_ASSERT(m_numberOfWords == numberOfWords && !m_liveSlotMapOffsetPlusOne);
    RELEASE_ASSERT(m_numberOfSavedRegisterPairs == savedRegistersOrPairsOf(siteWord) && m_numberOfWordsSkippedAtBottom == skippedAtBottomOf(siteWord));
    willWrite();
    uint64_t* words = this->words();
    const uint64_t* source = machineFrame.firstSavedWord();
    for (size_t i = 0; i < numberOfWords; ++i)
        words[i] = source[i];
    if constexpr (preservedRegister != InvalidGPRReg)
        m_preservedRegister = machineFrame.words->preservedRegister;
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
    return SavedFrameMap::tryCreate(image->frameMapWords(where.function), (offsetPlusOne & ~hasNoDeadWordsToOverwrite) - 1);
}

void SavedFrame::overwriteDeadWordsAtEndOfCollection(uint32_t versionAtVisit, const SavedFrameMap& map)
{
    Instance::of(realm())->state->coroutines->didVisitFrameWithDeadWords(this, versionAtVisit, map.firstWord());
}

void SavedFrame::overwriteDeadWords(uint32_t versionAtVisit, const SavedFrameMap& map, uint64_t replacement)
{
    if (m_version != versionAtVisit)
        return;
    RELEASE_ASSERT(!isOnStack());
    willWrite();
    forEachDeadWord(map, [&](uint64_t& word) {
        word = replacement;
        return true;
    });
    m_liveSlotMapOffsetPlusOne |= hasNoDeadWordsToOverwrite;
    didWrite();
}

template<typename Visitor>
void SavedFrame::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitFields(uncheckedDowncast<SavedFrame>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(SavedFrame);

// A word that the map does not list is dead, or it is no reference: an integer, half a fraction, the header for the next closure, whatever compiled code keeps over the suspension.
void SavedFrame::poisonDeadUnlistedWords(VM& vm)
{
    static_assert(locationHolding(2 * 3) == 3 && !isInUpperHalf(2 * 3) && locationHolding(2 * 3 - 1) == 3 && isInUpperHalf(2 * 3 - 1));
    if (isOnStack())
        return;
    std::optional<SavedFrameMap> map = liveSlotMap(m_liveSlotMapOffsetPlusOne);
    RELEASE_ASSERT(map);
    auto poisonDeadUnlistedWord = [&](uint32_t location) {
        if (map->listsValue(location) || map->listsNarrowReference(2 * location) || (location != locationInRegister && map->listsNarrowReference(2 * location - 1)))
            return;
        uint64_t& word = location == locationInRegister ? m_preservedRegister : words()[m_numberOfWords + numberOfWordsSkippedAtTop() - location];
        if (HeapUtil::classifyWord(vm.heap, std::bit_cast<const void*>(word)) == HeapUtil::WordKind::UnmarkedCell)
            word = poisonedWord;
    };
    willWrite();
    uint32_t lastLocation = m_numberOfWords + numberOfWordsSkippedAtTop();
    for (uint32_t location = numberOfWordsSkippedAtTop() + 1; location <= lastLocation; ++location)
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

JSPromise* SavedFrame::promiseOfCombinator() const
{
    if (continuation() != Continuation::CombinatorElement)
        return nullptr;
    return uncheckedDowncast<JSPromise>(uncheckedDowncast<JSPromiseCombinatorsGlobalContext>(m_promiseOrWaiter)->promise());
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

void SavedFrame::setCombinatorElement(VM& vm, JSCell* globalContext, uint32_t index)
{
    RELEASE_ASSERT(m_continuation == Continuation::None && index < maxCombinatorElements);
    ASSERT(m_promiseOrWaiter == this && globalContext->inherits<JSPromiseCombinatorsGlobalContext>());
    m_promiseOrWaiter = globalContext;
    m_continuation = static_cast<Continuation>(static_cast<uint32_t>(Continuation::CombinatorElement) + index);
    vm.writeBarrier(this);
}

// For a function that waits for a settled result of a call. A synchronous module load resumes the body of a module before it returns.
void SavedFrame::queueResumeJob(JSGlobalObject* globalObject, VM& vm, JSValue resolution, bool isRejected, JSValue asyncContext)
{
    auto status = static_cast<uint8_t>(isRejected ? JSPromise::Status::Rejected : JSPromise::Status::Fulfilled);
    if (vm.m_synchronousModuleQueue && isSuspendedModuleBody(vm, this)) [[unlikely]] {
        vm.m_synchronousModuleQueue->tasks.append({ InternalMicrotask::CompiledAsyncFunctionResume, status, jsUndefined(), resolution, this, asyncContext });
        return;
    }
    globalObject->queueMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, status, jsUndefined(), resolution, this, asyncContext);
}

void SavedFrame::settle(JSGlobalObject* globalObject, VM& vm, JSValue value, bool isRejected)
{
    auto status = static_cast<uint8_t>(isRejected ? JSPromise::Status::Rejected : JSPromise::Status::Fulfilled);
    switch (continuation()) {
    case Continuation::Waiter: {
        auto* waiter = uncheckedDowncast<SavedFrame>(m_promiseOrWaiter);
        waiter->queueResumeJob(globalObject, vm, value, isRejected, JSValue::decode(waiter->m_asyncContext));
        return;
    }
    // The job that the promise of the function would queue, had Promise.all been given one.
    case Continuation::CombinatorElement:
        globalObject->queueMicrotask(vm, InternalMicrotask::PromiseAllResolveJob, status, m_promiseOrWaiter, value, jsNumber(combinatorIndex()));
        return;
    case Continuation::None:
    case Continuation::Promise:
        if (isRejected)
            ensurePromise(globalObject, vm)->reject(vm, value);
        else
            ensurePromise(globalObject, vm)->fulfill(vm, value);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSPromise* SavedFrame::ensurePromise(JSGlobalObject* globalObject, VM& vm)
{
    if (JSPromise* promise = this->promise())
        return promise;
    ASSERT(m_continuation != Continuation::None || m_promiseOrWaiter == this);
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    switch (continuation()) {
    case Continuation::None:
        break;
    case Continuation::Waiter: {
        auto* waiter = uncheckedDowncast<SavedFrame>(m_promiseOrWaiter);
        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledAsyncFunctionResume, nullptr, waiter, JSValue::decode(waiter->m_asyncContext));
        break;
    }
    case Continuation::CombinatorElement:
        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseAllResolveJob, m_promiseOrWaiter, jsNumber(combinatorIndex()));
        break;
    case Continuation::Promise:
        RELEASE_ASSERT_NOT_REACHED();
    }
    m_promiseOrWaiter = promise;
    m_continuation = Continuation::Promise;
    vm.writeBarrier(this);
    return promise;
}

const ClassInfo Generator::s_info = { "Generator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Generator) };

Structure* Generator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

Generator* Generator::create(VM& vm, Structure* structure)
{
    Generator* generator = new (NotNull, allocateCell<Generator>(vm)) Generator(vm, structure);
    generator->finishCreation(vm);
    return generator;
}

bool Generator::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    bool isSet = Base::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
    uncheckedDowncast<Generator>(object)->m_hasOtherPrototype = !object->structure()->storedPrototype().isObject();
    return isSet;
}

template<typename Visitor>
void Generator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitor.appendUnbarriered(uncheckedDowncast<Generator>(cell)->m_savedFrame);
}

DEFINE_VISIT_CHILDREN(Generator);

void Generator::setSavedFrame(VM& vm, SavedFrame* saved)
{
    m_savedFrame = saved;
    vm.writeBarrier(this);
}

JSValue Generator::resume(JSGlobalObject* globalObject, ResumeMode mode, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (m_state) {
    case State::Running:
        throwException(globalObject, scope, createGeneratorIsExecutingError(globalObject));
        return { };
    case State::Completed:
        switch (mode) {
        case ResumeMode::Next:
            return createIteratorResultObject(globalObject, jsUndefined(), true);
        case ResumeMode::Throw:
            throwException(globalObject, scope, value);
            return { };
        case ResumeMode::Return:
            return createIteratorResultObject(globalObject, value, true);
        }
        RELEASE_ASSERT_NOT_REACHED();
    case State::SuspendedAtStart:
    case State::SuspendedAtYield:
        break;
    }
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return { };
    }
    m_state = State::Running;
    JSValue result = enterSavedFrame(vm, *Instance::of(realm()), m_savedFrame, mode, value);
    if (scope.exception()) [[unlikely]] {
        m_state = State::Completed;
        return { };
    }
    // A body that suspended again has left through the suspension stub, which stores SuspendedAtYield.
    bool isDone = m_state == State::Running;
    if (isDone)
        m_state = State::Completed;
    return createIteratorResultObject(globalObject, result, isDone);
}

Structure* structureOfGenerator(JSGlobalObject* globalObject, Instance& instance, JSObject* generatorFunction, CoroutineKind kind)
{
    RELEASE_ASSERT(kind != CoroutineKind::AsyncFunction);
    Structure* base = kind == CoroutineKind::Generator ? instance.state->coroutines->generatorStructure() : globalObject->asyncGeneratorStructure();
    return InternalFunction::createSubclassStructure(globalObject, generatorFunction, base);
}

JSObject* createGenerator(JSGlobalObject* globalObject, Instance& instance, JSObject* generatorFunction, CoroutineKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = structureOfGenerator(globalObject, instance, generatorFunction, kind);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (kind == CoroutineKind::AsyncGenerator)
        return JSAsyncGenerator::create(vm, structure);
    return Generator::create(vm, structure);
}

SavedFrame* savedFrameOfGenerator(JSObject* generator)
{
    if (auto* syncGenerator = dynamicDowncast<Generator>(generator))
        return syncGenerator->savedFrame();
    JSValue frame = uncheckedDowncast<JSAsyncGenerator>(generator)->frame();
    return frame.isUndefined() ? nullptr : uncheckedDowncast<SavedFrame>(frame);
}

void setSavedFrameOfGenerator(VM& vm, JSObject* generator, SavedFrame* saved)
{
    if (auto* syncGenerator = dynamicDowncast<Generator>(generator)) {
        syncGenerator->setSavedFrame(vm, saved);
        return;
    }
    auto* asyncGenerator = uncheckedDowncast<JSAsyncGenerator>(generator);
    asyncGenerator->internalField(JSAsyncGenerator::Field::Frame).set(vm, asyncGenerator, saved);
    asyncGenerator->internalField(JSAsyncGenerator::Field::Next).set(vm, asyncGenerator, saved);
}

void completeGeneratorLeftByThrow(CallFrame* stepStubFrame)
{
    std::bit_cast<Generator* const*>(stepStubFrame)[Generator::generatorSlotInStubFrame]->didThrow();
}

bool isGeneratorObject(JSCell* cell)
{
    return cell->inherits<Generator>();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
