/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTRelocation.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsCoroutines.h"
#include "AOTVector.h"
#include "AsyncContextSwapScope.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSPromise.h"
#include "LinkBuffer.h"
#include "Microtask.h"
#include "MicrotaskQueue.h"
#include "MicrotaskQueueInlines.h"
#include "ObjectConstructor.h"
#include "StackAlignment.h"
#include "Strong.h"
#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <wtf/NeverDestroyed.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> coroutineJobTests();

using JobWords = std::array<uint64_t, 1 + QueuedTask::maxArguments>;

static_assert(sizeof(QueuedTask) == sizeof(JobWords) && QueuedTask::maxArguments == 4);
static_assert(!QueuedTask::offsetOfDispatcher() && QueuedTask::offsetOfArguments() == sizeof(uint64_t));

static constexpr StubKey hostBody = newPromise();
static_assert(bodyOf(hostBody) == hostBody);

static constexpr uint8_t poisonByte = 0xab;
static constexpr uint64_t poisonWord = 0xababababababababULL;
static constexpr JobWords poisonedSlot { poisonWord, poisonWord, poisonWord, poisonWord, poisonWord };

static constexpr StubRegister resolutionRegister = StubRegister::A1;
static constexpr StubRegister contextRegister = StubRegister::A2;
static constexpr StubRegister savedRegister = StubRegister::A1;
static constexpr StubRegister modeRegister = StubRegister::A2;
static constexpr StubRegister valueRegister = StubRegister::A3;

static_assert(noOverlap(gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::A3), gprOf(StubRegister::A4), gprOf(StubRegister::A5), gprOf(StubRegister::T9), GPRInfo::returnValueGPR, instanceGPR));

namespace {

enum class Path : uint64_t { Left, FellThrough };

struct JobEnvironment {
    InstancePrefix prefix;
    Instance instance;
    std::array<uint64_t, 2> operands;
    std::array<uint64_t, 3> registersAfterwards;
};
static_assert(OBJECT_OFFSETOF(JobEnvironment, instance) == sizeof(InstancePrefix));

struct RingState {
    size_t start;
    size_t end;
    size_t capacityMask;
    size_t markedBefore;
    const uint8_t* buffer;
    bool isScheduledToRun;
    Vector<JobWords> slots;

    friend bool operator==(const RingState&, const RingState&) = default;
};

// The words of a MicrotaskQueue that the stubs read and write, at the offsets that the stubs use.
class Ring {
public:
    explicit Ring(MicrotaskQueue& queue)
        : m_queue(queue)
    {
    }

    size_t& start() const { return field<size_t>(MicrotaskQueue::offsetOfStart()); }
    size_t& end() const { return field<size_t>(MicrotaskQueue::offsetOfEnd()); }
    size_t& markedBefore() const { return field<size_t>(MicrotaskQueue::offsetOfMarkedBefore()); }
    size_t capacityMask() const { return field<size_t>(MicrotaskQueue::offsetOfCapacityMask()); }
    uint8_t* buffer() const { return field<uint8_t*>(MicrotaskQueue::offsetOfBuffer()); }
    size_t capacity() const { return buffer() ? capacityMask() + 1 : 0; }
    size_t indexAfter(size_t index, size_t distance = 1) const { return (index + distance) & capacityMask(); }
    size_t indexBefore(size_t index) const { return (index - 1) & capacityMask(); }

    JobWords slot(size_t index) const
    {
        AOT_TEST_CHECK(index < capacity());
        JobWords words;
        memcpySpan(asMutableByteSpan(words), unsafeMakeSpan(buffer() + index * sizeof(QueuedTask), sizeof(QueuedTask)));
        return words;
    }

    JobWords job(size_t position) const
    {
        AOT_TEST_CHECK(position < m_queue.size());
        return slot(indexAfter(start(), position));
    }

    void poison() const { memsetSpan(unsafeMakeSpan(buffer(), capacity() * sizeof(QueuedTask)), poisonByte); }

    RingState state() const
    {
        RingState result { start(), end(), capacityMask(), markedBefore(), buffer(), m_queue.isScheduledToRun(), { } };
        for (size_t index = 0; index < capacity(); ++index)
            result.slots.append(slot(index));
        return result;
    }

private:
    template<typename T>
    T& field(ptrdiff_t offset) const { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(&m_queue) + offset); }

    MicrotaskQueue& m_queue;
};

} // anonymous namespace

static uint64_t bitsOf(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

static JobWords wordsOf(const QueuedTask& job)
{
    JobWords words;
    memcpySpan(asMutableByteSpan(words), asByteSpan(job));
    return words;
}

static QueuedTask makeJob(JSGlobalObject* globalObject, InternalMicrotask task, JSPromise::Status status, JSValue resolution, JSValue context, JSValue asyncContext = jsUndefined())
{
    return QueuedTask { nullptr, task, static_cast<uint8_t>(status), globalObject, jsUndefined(), resolution, context, asyncContext };
}

namespace {

// A real VM, realm and queue, and an Instance that no program was loaded for. Every condition of the stubs holds until a test changes one.
class JobTestRealm {
    WTF_MAKE_NONCOPYABLE(JobTestRealm);
public:
    JobTestRealm()
        : m_vm(VM::create())
        , m_lock(*m_vm)
        , m_queue(MicrotaskQueue::create(*m_vm))
        , m_environment(makeUniqueWithoutFastMallocCheck<JobEnvironment>())
    {
        m_globalObject.set(vm(), createRealm());
        m_waiter.set(vm(), constructEmptyObject(globalObject()));
        m_queue->setIsScheduledToRun(true);

        Instance& instance = this->instance();
        instance.vm = m_vm.get();
        instance.globalObject = globalObject();
        instance.numberTag = JSValue::NumberTag;
        instance.notCellMask = JSValue::NotCellMask;
        instance.realm.microtaskQueue = m_queue.get();
        instance.coroutines.savedFrameHeader = cellHeaderFor(waiter()->structure());
        instance.coroutines.resumeJobWords[0] = wordsOf(resumeJob(JSPromise::Status::Fulfilled, jsUndefined()))[0];
        instance.coroutines.resumeJobWords[1] = wordsOf(resumeJob(JSPromise::Status::Rejected, jsUndefined()))[0];
    }

    ~JobTestRealm()
    {
        m_queue->clear();
        m_queue = nullptr;
        m_waiter.clear();
        m_globalObject.clear();
        m_vm = nullptr;
    }

    VM& vm() { return *m_vm; }
    JSGlobalObject* globalObject() { return m_globalObject.get(); }
    // Stands in for a SavedFrame of the instance: the stubs tell one by its Structure.
    JSObject* waiter() { return m_waiter.get(); }
    MicrotaskQueue& queue() { return *m_queue; }
    Ring ring() { return Ring(*m_queue); }
    JobEnvironment& environment() { return *m_environment; }
    Instance& instance() { return m_environment->instance; }

    JSGlobalObject* createRealm() { return JSGlobalObject::create(vm(), JSGlobalObject::createStructure(vm(), jsNull())); }
    QueuedTask resumeJob(JSPromise::Status status, JSValue resolution) { return makeJob(globalObject(), InternalMicrotask::CompiledAsyncFunctionResume, status, resolution, waiter()); }

    void emptyRingAt(size_t position)
    {
        Ring ring = this->ring();
        if (!ring.buffer())
            m_queue->enqueue(resumeJob(JSPromise::Status::Fulfilled, jsUndefined()));
        AOT_TEST_CHECK(position < ring.capacity());
        ring.start() = position;
        ring.end() = position;
        ring.markedBefore() = 0;
        ring.poison();
        AOT_TEST_CHECK(m_queue->isEmpty() && m_queue->isScheduledToRun());
    }

private:
    RefPtr<VM> m_vm;
    JSLockHolder m_lock;
    RefPtr<MicrotaskQueue> m_queue;
    std::unique_ptr<JobEnvironment> m_environment;
    Strong<JSGlobalObject> m_globalObject;
    Strong<JSObject> m_waiter;
};

} // anonymous namespace

static const StubCatalog& catalogForThisCPU()
{
    static NeverDestroyed<StubCatalog> catalog(StubOptions { .targetFeatures = CPUFeatureSet::buildTarget() }, StubPolicy { });
    return catalog.get();
}

static Address environmentSlot(size_t offsetInEnvironment, size_t index)
{
    size_t offset = offsetInEnvironment + index * sizeof(uint64_t) - OBJECT_OFFSETOF(JobEnvironment, instance);
    return Address(instanceGPR, static_cast<int32_t>(offset));
}

static Address operandSlot(size_t index) { return environmentSlot(OBJECT_OFFSETOF(JobEnvironment, operands), index); }
static Address registerAfterwardsSlot(size_t index) { return environmentSlot(OBJECT_OFFSETOF(JobEnvironment, registersAfterwards), index); }

static Vector<GPRReg> calleeSavedGPRs()
{
    Vector<GPRReg> registers;
    RegisterSet::vmCalleeSaveRegisters().forEach([&](Reg reg) {
        if (reg.isGPR())
            registers.append(reg.gpr());
    });
    AOT_TEST_CHECK(registers.contains(instanceGPR));
    return registers;
}

static Address calleeSaveSlot(size_t index)
{
    return Address(GPRInfo::callFrameRegister, -static_cast<int32_t>((index + 1) * sizeof(CPURegister)));
}

static void emitEntryFromC(StubAssembler& jit, std::span<const GPRReg> calleeSaves)
{
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(calleeSaves.size() * sizeof(CPURegister))), CCallHelpers::stackPointerRegister);
    for (size_t i = 0; i < calleeSaves.size(); ++i)
        jit.storePtr(calleeSaves[i], calleeSaveSlot(i));
    jit.move(GPRInfo::argumentGPR0, instanceGPR);
#if CPU(ARM64)
    jit.emitMaterializeTagCheckRegisters();
#endif
}

static void emitReturnToC(StubAssembler& jit, std::span<const GPRReg> calleeSaves)
{
    for (size_t i = 0; i < calleeSaves.size(); ++i)
        jit.loadPtr(calleeSaveSlot(i), calleeSaves[i]);
    jit.emitFunctionEpilogue();
    jit.returnToCaller();
}

namespace {

// A function of C++ around code that falls through or jumps to a list. It says which, and stores the given registers on both paths.
class Snippet {
public:
    using Emitter = ScopedLambda<void(StubAssembler&, JumpList& leaves)>;

    Snippet(std::initializer_list<StubRegister> registersStoredAfterwards, const Emitter& emit)
    {
        const StubCatalog& catalog = catalogForThisCPU();
        AOT_TEST_CHECK(entriesOf(hostBody).size() == 1 && !signatureFor(hostBody).numberOfOperands);
        AOT_TEST_CHECK(registersStoredAfterwards.size() <= std::tuple_size_v<decltype(JobEnvironment::registersAfterwards)>);

        Vector<GPRReg> calleeSaves = calleeSavedGPRs();
        StubAssembler assembler(catalog.options(), catalog);
        StubRecording recording = assembler.generateForTesting(hostBody, [&](StubAssembler& jit) {
            emitEntryFromC(jit, calleeSaves.span());

            JumpList leaves;
            emit(jit, leaves);
            jit.move(TrustedImm32(static_cast<int32_t>(Path::FellThrough)), jit.result());
            Jump done = jit.jump();
            leaves.link(&jit);
            jit.move(TrustedImm32(static_cast<int32_t>(Path::Left)), jit.result());
            done.link(&jit);

            size_t index = 0;
            for (StubRegister stored : registersStoredAfterwards)
                jit.store64(gprOf(stored), registerAfterwardsSlot(index++));
            emitReturnToC(jit, calleeSaves.span());
        });
        AOT_TEST_CHECK(recording.references.isEmpty() && recording.symbols.isEmpty());
        AOT_TEST_CHECK(recording.entriesUsed.isEmpty() && !recording.leavesStubs && !recording.hasUnknownEffects);

        ImageLinkBuffer buffer(assembler, LinkBuffer::Profile::Thunk);
        AOT_TEST_CHECK(!buffer.didFailToAllocate());
        buffer.finalize(false, "coroutine job snippet"_s);
        AOT_TEST_CHECK(assembler.boundEntries().size() == 1 && !assembler.relocations().numberOfRelocations());
        m_offsetOfEntry = buffer.linkBuffer().offsetOf(assembler.boundEntries()[0].label);
        m_code.emplace(buffer.bytes());
    }

    Path run(JobEnvironment& environment) const
    {
        return static_cast<Path>(m_code->call<uint64_t>(m_offsetOfEntry, &environment.instance));
    }

private:
    std::optional<ImageTests::ExecutableCopy> m_code;
    uint32_t m_offsetOfEntry { 0 };
};

struct JobKind {
    InternalMicrotask task;
    JSPromise::Status status;
};

struct TakenJob {
    Path path;
    uint64_t saved;
    uint32_t mode;
    uint64_t value;
};

} // anonymous namespace

static Snippet assemblePush(JobKind kind)
{
    return Snippet({ resolutionRegister, contextRegister }, [&](StubAssembler& jit, JumpList& needsOperation) {
        const GPRReg resolution = jit.allocateScratch(resolutionRegister);
        const GPRReg context = jit.allocateScratch(contextRegister);
        const JobRegisters registers {
            .queue = jit.allocateScratch(StubRegister::A3),
            .slot = jit.allocateScratch(StubRegister::A4),
            .next = jit.allocateScratch(StubRegister::A5),
            .scratch = jit.allocateScratch(StubRegister::T9),
        };
        jit.load64(operandSlot(0), resolution);
        jit.load64(operandSlot(1), context);
        emitReserveJob(jit, registers, needsOperation);
        emitFillJob(jit, { .task = kind.task, .status = kind.status, .resolution = resolution, .context = context }, registers);
    });
}

static Snippet assemblePushOfResumeJob(JSPromise::Status status)
{
    return assemblePush({ InternalMicrotask::CompiledAsyncFunctionResume, status });
}

static Snippet assembleTake()
{
    return Snippet({ savedRegister, modeRegister, valueRegister }, [](StubAssembler& jit, JumpList& noJob) {
        const GPRReg saved = jit.allocateScratch(savedRegister);
        const GPRReg mode = jit.allocateScratch(modeRegister);
        const GPRReg value = jit.allocateScratch(valueRegister);
        const HeadJobRegisters head {
            .queue = jit.allocateScratch(StubRegister::A4),
            .slot = jit.allocateScratch(StubRegister::T9),
            .word = jit.allocateScratch(StubRegister::A5),
        };
        emitLoadHeadJob(jit, head, noJob);
        jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(ResumeMode::Next)), mode);
        CCallHelpers::Jump isFulfilled = jit.branch64(CCallHelpers::Equal, head.word, CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.resumeJobWords)));
        noJob.append(jit.branch64(CCallHelpers::NotEqual, head.word, CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(coroutines.resumeJobWords) + sizeof(uint64_t))));
        jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(ResumeMode::Throw)), mode);
        isFulfilled.link(&jit);
        emitTakeFunctionResumeJob(jit, head, saved, value, noJob);
    });
}

// Whichever way the code goes, what comes next still needs the operands.
static Path push(JobTestRealm& realm, const Snippet& snippet, JSValue resolution)
{
    JobEnvironment& environment = realm.environment();
    environment.operands = { bitsOf(resolution), bitsOf(realm.waiter()) };
    environment.registersAfterwards = { };
    Path path = snippet.run(environment);
    AOT_TEST_CHECK(environment.registersAfterwards[0] == environment.operands[0]);
    AOT_TEST_CHECK(environment.registersAfterwards[1] == environment.operands[1]);
    return path;
}

static void checkPushLeaves(JobTestRealm& realm, const Snippet& snippet)
{
    RingState before = realm.ring().state();
    AOT_TEST_CHECK(push(realm, snippet, jsNumber(1)) == Path::Left);
    AOT_TEST_CHECK(realm.ring().state() == before);
}

static TakenJob take(JobTestRealm& realm, const Snippet& snippet)
{
    JobEnvironment& environment = realm.environment();
    environment.registersAfterwards = { };
    Path path = snippet.run(environment);
    auto [saved, mode, value] = environment.registersAfterwards;
    return { path, saved, static_cast<uint32_t>(mode), value };
}

static void checkTaken(JobTestRealm& realm, const TakenJob& taken, ResumeMode mode, JSValue value)
{
    AOT_TEST_CHECK(taken.path == Path::FellThrough);
    AOT_TEST_CHECK(taken.saved == bitsOf(realm.waiter()));
    AOT_TEST_CHECK(taken.mode == static_cast<uint32_t>(mode));
    AOT_TEST_CHECK(taken.value == bitsOf(value));
}

static void checkNoJobIsTaken(JobTestRealm& realm, const Snippet& snippet)
{
    RingState before = realm.ring().state();
    AOT_TEST_CHECK(take(realm, snippet).path == Path::Left);
    AOT_TEST_CHECK(realm.ring().state() == before);
}

static void checkPushedJob(JobTestRealm& realm, const Snippet& snippet, JobKind kind, size_t position, JSValue resolution)
{
    Ring ring = realm.ring();
    realm.emptyRingAt(position);
    const uint8_t* buffer = ring.buffer();
    size_t capacityMask = ring.capacityMask();
    size_t positionAfter = ring.indexAfter(position);
    QueuedTask expected = makeJob(realm.globalObject(), kind.task, kind.status, resolution, realm.waiter());

    AOT_TEST_CHECK(push(realm, snippet, resolution) == Path::FellThrough);
    AOT_TEST_CHECK(ring.slot(position) == wordsOf(expected));
    AOT_TEST_CHECK(ring.slot(ring.indexBefore(position)) == poisonedSlot && ring.slot(positionAfter) == poisonedSlot);
    AOT_TEST_CHECK(ring.start() == position && ring.end() == positionAfter && realm.queue().size() == 1);
    AOT_TEST_CHECK(ring.buffer() == buffer && ring.capacityMask() == capacityMask && !ring.markedBefore());

    realm.queue().enqueue(WTF::move(expected));
    AOT_TEST_CHECK(ring.buffer() == buffer && realm.queue().size() == 2);
    AOT_TEST_CHECK(ring.slot(positionAfter) == ring.slot(position));
}

static void testPushedJobs()
{
    static constexpr JobKind kinds[] = {
        { InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled },
        { InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Rejected },
        { InternalMicrotask::PromiseAllResolveJob, JSPromise::Status::Fulfilled },
    };

    JobTestRealm realm;
    realm.emptyRingAt(0);
    const size_t lastPosition = realm.ring().capacityMask();
    const JSValue resolutions[] = { jsNumber(42), jsNumber(1.5), jsUndefined(), jsBoolean(true), realm.waiter(), realm.globalObject() };

    for (JobKind kind : kinds) {
        Snippet snippet = assemblePush(kind);
        for (size_t position : { size_t { 0 }, size_t { 3 }, lastPosition - 1, lastPosition }) {
            for (JSValue resolution : resolutions)
                checkPushedJob(realm, snippet, kind, position, resolution);
        }
    }
}

static void testFullRing()
{
    JobTestRealm realm;
    Ring ring = realm.ring();
    Snippet snippet = assemblePushOfResumeJob(JSPromise::Status::Fulfilled);

    realm.emptyRingAt(0);
    const unsigned capacity = static_cast<unsigned>(ring.capacity());
    AOT_TEST_CHECK(capacity >= 4);
    for (unsigned position : { 0u, 1u, capacity / 2, capacity - 1 }) {
        realm.emptyRingAt(position);
        for (unsigned i = 0; i < capacity - 1; ++i)
            AOT_TEST_CHECK(push(realm, snippet, jsNumber(i)) == Path::FellThrough);
        AOT_TEST_CHECK(realm.queue().size() == capacity - 1 && ring.indexAfter(ring.end()) == ring.start());
        AOT_TEST_CHECK(ring.slot(ring.end()) == poisonedSlot);
        checkPushLeaves(realm, snippet);
    }

    realm.queue().enqueue(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(capacity - 1)));
    AOT_TEST_CHECK(ring.capacity() > capacity);
    AOT_TEST_CHECK(push(realm, snippet, jsNumber(capacity)) == Path::FellThrough);
    AOT_TEST_CHECK(realm.queue().size() == capacity + 1);
    for (unsigned i = 0; i <= capacity; ++i)
        AOT_TEST_CHECK(ring.job(i) == wordsOf(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(i))));
}

static void testQueueWithoutBuffer()
{
    JobTestRealm realm;
    Ring ring = realm.ring();
    Snippet snippet = assemblePushOfResumeJob(JSPromise::Status::Fulfilled);

    AOT_TEST_CHECK(!ring.buffer() && !ring.capacityMask() && !ring.start() && !ring.end());
    AOT_TEST_CHECK(realm.queue().isScheduledToRun() && !realm.instance().awaitMustSuspend);
    checkPushLeaves(realm, snippet);
    AOT_TEST_CHECK(realm.queue().isEmpty());

    realm.emptyRingAt(0);
    AOT_TEST_CHECK(push(realm, snippet, jsNumber(1)) == Path::FellThrough);
}

static void testStatesInWhichNoJobIsPushed()
{
    JobTestRealm realm;
    Snippet snippet = assemblePushOfResumeJob(JSPromise::Status::Fulfilled);
    realm.emptyRingAt(0);

    realm.instance().awaitMustSuspend = 1;
    checkPushLeaves(realm, snippet);
    realm.instance().awaitMustSuspend = 0;

    realm.queue().setIsScheduledToRun(false);
    checkPushLeaves(realm, snippet);
    realm.queue().setIsScheduledToRun(true);

    AOT_TEST_CHECK(push(realm, snippet, jsNumber(1)) == Path::FellThrough);
    AOT_TEST_CHECK(realm.queue().size() == 1);
}

static void checkTakenJob(JobTestRealm& realm, const Snippet& snippet, size_t position, size_t markedBefore, JSPromise::Status status)
{
    Ring ring = realm.ring();
    realm.emptyRingAt(position);
    realm.queue().enqueue(realm.resumeJob(status, jsNumber(7)));
    realm.queue().enqueue(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(8)));
    ring.markedBefore() = markedBefore;

    RingState expected = ring.state();
    expected.start = ring.indexAfter(position);
    expected.markedBefore = markedBefore ? markedBefore - 1 : 0;

    checkTaken(realm, take(realm, snippet), status == JSPromise::Status::Rejected ? ResumeMode::Throw : ResumeMode::Next, jsNumber(7));
    AOT_TEST_CHECK(ring.state() == expected);
    AOT_TEST_CHECK(realm.queue().size() == 1);
    AOT_TEST_CHECK(ring.job(0) == wordsOf(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(8))));
}

static void testTakenResumeJobs()
{
    JobTestRealm realm;
    Snippet snippet = assembleTake();
    realm.emptyRingAt(0);
    const size_t lastPosition = realm.ring().capacityMask();

    for (size_t position : { size_t { 0 }, size_t { 3 }, lastPosition - 1, lastPosition }) {
        for (size_t markedBefore : { size_t { 0 }, size_t { 1 }, size_t { 2 } }) {
            for (JSPromise::Status status : { JSPromise::Status::Fulfilled, JSPromise::Status::Rejected })
                checkTakenJob(realm, snippet, position, markedBefore, status);
        }
    }
}

static void testJobsThatStayInTheQueue()
{
    JobTestRealm realm;
    Snippet snippet = assembleTake();
    JSGlobalObject* otherRealm = realm.createRealm();
    auto enqueueAlone = [&](QueuedTask&& job) {
        realm.emptyRingAt(0);
        realm.queue().enqueue(WTF::move(job));
    };

    realm.emptyRingAt(0);
    checkNoJobIsTaken(realm, snippet);
    realm.emptyRingAt(realm.ring().capacityMask());
    checkNoJobIsTaken(realm, snippet);

    for (InternalMicrotask task : { InternalMicrotask::AsyncFunctionResume, InternalMicrotask::PromiseReactionJob, InternalMicrotask::AsyncGeneratorDriverResume }) {
        enqueueAlone(makeJob(realm.globalObject(), task, JSPromise::Status::Fulfilled, jsNumber(1), realm.waiter()));
        checkNoJobIsTaken(realm, snippet);
    }

    enqueueAlone(makeJob(otherRealm, InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled, jsNumber(1), realm.waiter()));
    checkNoJobIsTaken(realm, snippet);

    AOT_TEST_CHECK(otherRealm->structure() != realm.waiter()->structure());
    enqueueAlone(makeJob(realm.globalObject(), InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled, jsNumber(1), otherRealm));
    checkNoJobIsTaken(realm, snippet);

    enqueueAlone(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(1)));
    realm.instance().awaitMustSuspend = 1;
    checkNoJobIsTaken(realm, snippet);
    realm.instance().awaitMustSuspend = 0;

    // Where async contexts are tracked a job runs under the one that it has captured. Another than the current one is put in place if whoever runs the jobs puts back afterwards what was there. Else the job stays for the job loop.
    realm.vm().m_asyncContextTrackingEnabled = true;
    enqueueAlone(makeJob(realm.globalObject(), InternalMicrotask::CompiledAsyncFunctionResume, JSPromise::Status::Fulfilled, jsNumber(1), realm.waiter(), jsNumber(7)));
    checkNoJobIsTaken(realm, snippet);
    AOT_TEST_CHECK(AsyncContextSwapScope::current(realm.vm(), realm.globalObject()).isUndefined());
    realm.instance().coroutines.asyncContextIsPutBack = 1;
    checkTaken(realm, take(realm, snippet), ResumeMode::Next, jsNumber(1));
    AOT_TEST_CHECK(realm.queue().isEmpty());
    AOT_TEST_CHECK(AsyncContextSwapScope::current(realm.vm(), realm.globalObject()) == jsNumber(7));
    enqueueAlone(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(1)));
    checkTaken(realm, take(realm, snippet), ResumeMode::Next, jsNumber(1));
    AOT_TEST_CHECK(AsyncContextSwapScope::current(realm.vm(), realm.globalObject()).isUndefined());
    realm.instance().coroutines.asyncContextIsPutBack = 0;
    enqueueAlone(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(1)));
    checkTaken(realm, take(realm, snippet), ResumeMode::Next, jsNumber(1));
    AOT_TEST_CHECK(realm.queue().isEmpty());
    realm.vm().m_asyncContextTrackingEnabled = false;

    enqueueAlone(realm.resumeJob(JSPromise::Status::Fulfilled, jsNumber(1)));
    checkTaken(realm, take(realm, snippet), ResumeMode::Next, jsNumber(1));
    AOT_TEST_CHECK(realm.queue().isEmpty());
}

static void testPushedJobsAreTakenInOrder()
{
    JobTestRealm realm;
    Snippet pushFulfilled = assemblePushOfResumeJob(JSPromise::Status::Fulfilled);
    Snippet pushRejected = assemblePushOfResumeJob(JSPromise::Status::Rejected);
    Snippet takeJob = assembleTake();

    realm.emptyRingAt(0);
    realm.emptyRingAt(realm.ring().capacityMask());
    AOT_TEST_CHECK(push(realm, pushFulfilled, jsNumber(1)) == Path::FellThrough);
    AOT_TEST_CHECK(push(realm, pushRejected, realm.globalObject()) == Path::FellThrough);
    AOT_TEST_CHECK(push(realm, pushFulfilled, jsUndefined()) == Path::FellThrough);
    AOT_TEST_CHECK(realm.queue().size() == 3);

    checkTaken(realm, take(realm, takeJob), ResumeMode::Next, jsNumber(1));
    checkTaken(realm, take(realm, takeJob), ResumeMode::Throw, realm.globalObject());
    checkTaken(realm, take(realm, takeJob), ResumeMode::Next, jsUndefined());
    AOT_TEST_CHECK(realm.queue().isEmpty());
    checkNoJobIsTaken(realm, takeJob);
}

static Snippet assembleTakeOfReactionJob()
{
    return Snippet({ savedRegister, modeRegister, valueRegister }, [](StubAssembler& jit, JumpList& noJob) {
        const GPRReg promise = jit.allocateScratch(savedRegister);
        const GPRReg handler = jit.allocateScratch(modeRegister);
        const GPRReg argument = jit.allocateScratch(valueRegister);
        const HeadJobRegisters head {
            .queue = jit.allocateScratch(StubRegister::A4),
            .slot = jit.allocateScratch(StubRegister::T9),
            .word = jit.allocateScratch(StubRegister::A5),
        };
        emitLoadHeadJob(jit, head, noJob);
        emitTakeReactionJob(jit, ReactionJob::HasNoAsyncContext, head, promise, handler, argument, noJob);
    });
}

static void testReactionJobsAreTakenByTheirLastArgument()
{
    JobTestRealm realm;
    Snippet snippet = assembleTakeOfReactionJob();
    JobEnvironment& environment = realm.environment();
    JSPromise* promise = JSPromise::create(realm.vm(), realm.globalObject()->promiseStructure());
    realm.instance().allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].header = cellHeaderFor(promise->structure());
    const JSValue handler = realm.waiter();
    const JSValue argument = jsNumber(7);
    const std::array<uint64_t, 3> taken { bitsOf(promise), bitsOf(handler), bitsOf(argument) };
    auto takeJobWithLastArgument = [&](JSValue last) {
        realm.emptyRingAt(0);
        realm.queue().enqueue(QueuedTask { nullptr, InternalMicrotask::PromiseReactionJob, static_cast<uint8_t>(JSPromise::Status::Fulfilled), realm.globalObject(), promise, handler, argument, last });
        environment.registersAfterwards = { };
        return snippet.run(environment);
    };

    for (JSValue last : { JSValue(), jsUndefined(), jsNull() }) {
        AOT_TEST_CHECK(takeJobWithLastArgument(last) == Path::FellThrough);
        AOT_TEST_CHECK(environment.registersAfterwards == taken);
        AOT_TEST_CHECK(realm.queue().isEmpty());
    }
    for (JSValue last : { jsNumber(0), jsBoolean(false), jsBoolean(true), handler }) {
        AOT_TEST_CHECK(takeJobWithLastArgument(last) == Path::Left);
        AOT_TEST_CHECK(realm.queue().size() == 1);
    }
}

static void testMaskedTestsOfWordsInMemory()
{
    JobTestRealm realm;
    JobEnvironment& environment = realm.environment();
    for (int32_t mask : { -1, 1, 0x10, 0xff, ~0xa, 0x12345, -0x12345 }) {
        Snippet testsAddress({ savedRegister }, [&](StubAssembler& jit, JumpList& hasBitOfMask) {
            hasBitOfMask.append(jit.branchTest64(CCallHelpers::NonZero, operandSlot(0), TrustedImm32(mask)));
        });
        Snippet testsIndexedAddress({ savedRegister }, [&](StubAssembler& jit, JumpList& hasBitOfMask) {
            const GPRReg index = jit.allocateScratch(savedRegister);
            const Address slot = operandSlot(0);
            jit.move(TrustedImm32(sizeof(uint64_t)), index);
            hasBitOfMask.append(jit.branchTest64(CCallHelpers::NonZero, BaseIndex(slot.base, index, CCallHelpers::TimesOne, slot.offset - static_cast<int32_t>(sizeof(uint64_t))), TrustedImm32(mask)));
        });
        for (uint64_t word : { uint64_t { 0 }, uint64_t { 2 }, uint64_t { 8 }, uint64_t { 0xa }, uint64_t { 0x10 }, uint64_t { 0x12345 }, uint64_t { 1 } << 40, ~uint64_t { 0 } }) {
            environment.operands[0] = word;
            Path expected = word & static_cast<uint64_t>(static_cast<int64_t>(mask)) ? Path::Left : Path::FellThrough;
            AOT_TEST_CHECK(testsAddress.run(environment) == expected);
            AOT_TEST_CHECK(testsIndexedAddress.run(environment) == expected);
        }
    }
}

std::span<const NativeTest> coroutineJobTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/coroutine-jobs/pushed-job-equals-queued-task"_s, ImageTests::whenExecutable<testPushedJobs> },
        { "stubs/coroutine-jobs/full-ring"_s, ImageTests::whenExecutable<testFullRing> },
        { "stubs/coroutine-jobs/queue-without-buffer"_s, ImageTests::whenExecutable<testQueueWithoutBuffer> },
        { "stubs/coroutine-jobs/states-in-which-no-job-is-pushed"_s, ImageTests::whenExecutable<testStatesInWhichNoJobIsPushed> },
        { "stubs/coroutine-jobs/taken-resume-jobs"_s, ImageTests::whenExecutable<testTakenResumeJobs> },
        { "stubs/coroutine-jobs/jobs-that-stay-in-the-queue"_s, ImageTests::whenExecutable<testJobsThatStayInTheQueue> },
        { "stubs/coroutine-jobs/pushed-jobs-are-taken-in-order"_s, ImageTests::whenExecutable<testPushedJobsAreTakenInOrder> },
        { "stubs/coroutine-jobs/reaction-jobs-are-taken-by-their-last-argument"_s, ImageTests::whenExecutable<testReactionJobsAreTakenByTheirLastArgument> },
        { "stubs/coroutine-jobs/masked-tests-of-words-in-memory"_s, ImageTests::whenExecutable<testMaskedTestsOfWordsInMemory> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
