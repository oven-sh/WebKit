/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStackTrace.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTBuiltinTable.h"
#include "AOTCallSiteTable.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeValidation.h"
#include "AOTSavedFrame.h"
#include "AOTVector.h"
#include "DeferGC.h"
#include "ExecutableBaseInlines.h"
#include "InternalFieldTuple.h"
#include "Interpreter.h"
#include "JSCInlines.h"
#include "JSFunctionInlines.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "StackFrame.h"
#include "VMEntryRecord.h"
#include "VMInlines.h"
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(static_cast<unsigned>(TraceVisibility::Hidden) < 4 && static_cast<unsigned>(StackTraceBuiltinKind::HostFunction) < 4 && static_cast<unsigned>(ModuleCode) < 4);

FunctionID functionInFramesOf(BuiltinID builtin)
{
    ASSERT(builtin);
    return FunctionID(Image::current()->programTables().numberOfFunctions() + builtin.index());
}

BuiltinID builtinOfFunctionInFrames(FunctionID function)
{
    size_t numberOfFunctions = Image::current()->programTables().numberOfFunctions();
    return function.index() < numberOfFunctions ? BuiltinID() : BuiltinID(static_cast<uint32_t>(function.index() - numberOfFunctions));
}

static constexpr FunctionTraceData traceOfCalledBuiltin {
    .visibilityBits = static_cast<uint16_t>(TraceVisibility::Visible),
    .builtinKindBits = static_cast<uint16_t>(StackTraceBuiltinKind::HostFunction),
    .codeTypeBits = static_cast<uint16_t>(FunctionCode),
    .rejectsOnThrow = false,
    .isStrict = true,
    .isConstructor = false,
    .isAsync = false,
    .isNamedByOwner = false,
    .isNakedConstructor = false,
    .implementsBuiltin = false,
    .isNamedByComputedKey = false,
    .recordsCallee = false,
    .unused = 0,
};

static String nameOfBuiltin(VM& vm, BuiltinID builtin)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    return table.identifierOf(vm, table.at(builtin).name).string();
}

FunctionDescription::FunctionDescription(FunctionID function)
    : m_function(function)
{
    const Image* image = Image::current();
    RELEASE_ASSERT(image && function);
    if (builtinOfFunctionInFrames(function)) [[unlikely]] {
        m_record = &traceOfCalledBuiltin;
        return;
    }
    m_record = &image->programTables().traceOfFunction(function.index());
}

String FunctionDescription::name(VM& vm) const
{
    if (m_record == &traceOfCalledBuiltin)
        return nameOfBuiltin(vm, builtinOfFunctionInFrames(m_function));
    return Image::current()->isObfuscated() ? emptyString() : functionName(vm, m_function, NameForm::AsWritten);
}

String FunctionDescription::nameInFrames(VM& vm, JSCell* callee) const
{
    if (m_record == &traceOfCalledBuiltin)
        return nameOfBuiltin(vm, builtinOfFunctionInFrames(m_function));
    return Image::current()->isObfuscated() ? emptyString() : functionName(vm, m_function, NameForm::InFrames, callee ? asObject(callee) : nullptr);
}

#define AOT_DEFINE_FLAG_OF_TRACE_RECORD(flag) \
    bool FunctionDescription::flag() const { return m_record->flag; }
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isStrict)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isConstructor)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isNakedConstructor)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isNamedByComputedKey)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(recordsCallee)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isAsync)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isNamedByOwner)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(rejectsOnThrow)
#undef AOT_DEFINE_FLAG_OF_TRACE_RECORD

CodeType FunctionDescription::codeType() const
{
    return m_record->codeType();
}

bool FunctionDescription::isInRuntimeModule() const
{
    return m_record != &traceOfCalledBuiltin && functionKindData(m_function).isInRuntimeModule;
}

TraceVisibility FunctionDescription::visibility() const
{
    return m_record->visibility();
}

FunctionID FunctionDescription::sourceFunction() const
{
    return m_record == &traceOfCalledBuiltin ? m_function : sourceFunctionOf(m_function);
}

StackTraceBuiltinKind FunctionDescription::builtinKind() const
{
    return m_record->builtinKind();
}

std::optional<SourcePosition> FunctionDescription::definition() const
{
    if (m_record == &traceOfCalledBuiltin)
        return std::nullopt;
    const Image* image = Image::current();
    for (FunctionID candidate : { m_function, sourceFunction() }) {
        if (auto index = image->imageFunctionOf(candidate))
            return image->positionOf(*index, CallSiteIndex(0));
    }
    return std::nullopt;
}

// What the two registers hold in the frame that rawPC runs in.
struct RunStart {
    void* stackPointer;
    CallFrame* framePointer;
    void* rawPC;
    EntryFrame* entryFrame;
    bool isAtFirstReadSite;
    bool calleeWasEntered;
};

// Goes over the machine frames of one run of compiled code as CompiledFrameIterator does. It reads the frame records and the call site cache and keeps only what a capture asks for.
class RunWalk {
public:
    RunWalk(const VMState& state, const RunStart& start)
        : m_cache(*state.callSiteCache)
        , m_image(*state.image)
        , m_stackPointer(start.stackPointer)
        , m_framePointer(start.framePointer)
        , m_rawPC(start.rawPC)
        , m_entryFrame(start.entryFrame)
        , m_isAtFirstReadSite(start.isAtFirstReadSite)
    {
        ASSERT(m_framePointer);
        findFunctionFrame();
    }

    const Image& image() const { return m_image; }
    bool isAtEnd() const { return !m_site; }
    const CallSiteCache::Entry& site() const
    {
        ASSERT(!isAtEnd());
        return *m_site;
    }
    bool isAtFirstReadSite() const { return m_isAtFirstReadSite && site().site.firstReadSite; }
    void* callerStackPointer() const { return stackPointerOfCaller(m_callFrame, bytesAboveReturnAddressOf(site())); }
    const CompiledFrameExit& exit() const
    {
        ASSERT(isAtEnd());
        return m_exit;
    }
    const CompiledFrameIterator::RunEnd& runEnd() const
    {
        ASSERT(isAtEnd());
        return m_runEnd;
    }

    ALWAYS_INLINE void advance()
    {
        ASSERT(!isAtEnd());
        m_isAtFirstReadSite = false;
        if (passOverFrame())
            findFunctionFrame();
    }

private:
    void leaveRun()
    {
        m_site = nullptr;
        EntryFrame* entryFrameBefore = m_entryFrame;
        void* returnPC = m_callFrame->rawReturnPC();
        m_exit.callFrame = m_callFrame->callerFrame(m_entryFrame);
        m_exit.entryFrame = m_entryFrame;
        m_exit.crossedEntryFrame = m_entryFrame != entryFrameBefore;
        m_exit.returnPC = m_exit.crossedEntryFrame ? nullptr : returnPC;
        m_exit.calleeFrame = m_exit.crossedEntryFrame ? nullptr : m_callFrame;
    }

    ALWAYS_INLINE bool passOverFrame()
    {
        if (m_site->frame.hasFrameRecord()) {
            if (m_callFrame->callerFrameOrEntryFrame() == m_entryFrame) [[unlikely]] {
                leaveRun();
                return false;
            }
            m_framePointer = m_callFrame->callerFrame();
        }
        m_rawPC = m_callFrame->rawReturnPC();
        m_stackPointer = stackPointerOfCaller(m_callFrame, bytesAboveReturnAddressOf(*m_site));
        return true;
    }

    bool passOverStubFrame(StubFrameKind kind)
    {
        switch (kind) {
        case StubFrameKind::Plain:
        case StubFrameKind::EngineCallee:
        case StubFrameKind::HostCall:
        case StubFrameKind::GeneratorBody:
        case StubFrameKind::ArgumentList:
        case StubFrameKind::BelowSpilledArguments:
        case StubFrameKind::AtStubEntry:
            break;
        case StubFrameKind::FirstOfTwoReads:
            m_isAtFirstReadSite = true;
            break;
        case StubFrameKind::AsyncGeneratorBody:
            m_isAmongFramesThatWait |= callersWaitForStep(m_callFrame);
            break;
        case StubFrameKind::Adapter:
        case StubFrameKind::HandlerOfJob:
            m_runEnd.adapterFrame = m_callFrame;
            leaveRun();
            return false;
        case StubFrameKind::ConstructAdapter:
            m_runEnd.adapterFrame = m_callFrame;
            m_runEnd.isConstruct = true;
            leaveRun();
            return false;
        case StubFrameKind::SavedFrameAdapter:
            m_runEnd.adapterFrame = m_callFrame;
            m_runEnd.resumesSavedFrame = true;
            leaveRun();
            return false;
        case StubFrameKind::InHostCallFrame:
            RELEASE_ASSERT_NOT_REACHED();
        }
        return passOverFrame();
    }

    ALWAYS_INLINE void findFunctionFrame()
    {
        for (;;) {
            const void* pc = removeCodePtrTag(m_rawPC);
            m_site = m_cache.find(pc);
            if (!m_site) [[unlikely]] {
                m_site = resolveReturnAddressInRun(m_cache, m_image, pc, m_callFrame);
                if (!m_site) {
                    m_exit = { m_framePointer, m_rawPC, m_entryFrame, false, m_callFrame };
                    return;
                }
            }
            if (m_site->isStubReturnSite() && m_site->stubFrameKind == StubFrameKind::InHostCallFrame) [[unlikely]] {
                m_site = nullptr;
                m_exit = { m_framePointer, m_rawPC, m_entryFrame, false, m_callFrame };
                return;
            }
            m_callFrame = frameAtReturnSite(m_site->frame, m_stackPointer, m_framePointer);
            if (!m_site->isStubReturnSite()) [[likely]] {
                if (!m_isAmongFramesThatWait) [[likely]]
                    return;
                if (!passOverFrame())
                    return;
                continue;
            }
            if (!passOverStubFrame(m_site->stubFrameKind))
                return;
        }
    }

    CallSiteCache& m_cache;
    const Image& m_image;
    void* m_stackPointer;
    CallFrame* m_framePointer;
    CallFrame* m_callFrame { nullptr };
    void* m_rawPC;
    EntryFrame* m_entryFrame;
    bool m_isAtFirstReadSite;
    bool m_isAmongFramesThatWait { false };
    const CallSiteCache::Entry* m_site { nullptr };
    CompiledFrameExit m_exit;
    CompiledFrameIterator::RunEnd m_runEnd;
};

struct CapturedRun {
    using Trace = ReturnAddressTrace;
    static constexpr size_t inlineCapacity = 2 * Trace::returnAddressesPerFlagWord;

    void append(uint32_t returnAddress)
    {
        if (!(returnAddresses.size() % Trace::returnAddressesPerFlagWord))
            flagWords.append(0u);
        returnAddresses.append(returnAddress);
    }

    void addFlagToLast(Trace::Flag flag)
    {
        flagWords.last() |= static_cast<uint32_t>(flag) << Trace::shiftInFlagWord(returnAddresses.size() - 1);
    }

    // Both are laid out as in a ReturnAddressTrace.
    Vector<uint32_t, inlineCapacity> returnAddresses;
    Vector<uint32_t, Trace::numberOfFlagWords(inlineCapacity)> flagWords;
    size_t numberShown { 0 };
    size_t numberOmitted { 0 };
    bool foundConstructorToSkip { false };
    bool hasFrameNamedByCallee { false };
};

struct FramesAboveRun {
    size_t numberShown { 0 };
    bool exist { false };
};

static NEVER_INLINE void countLogicalFrames(const Image& image, const CallSiteCache::Entry& site, bool calleeWasEntered, ShownFrameFilter& filter, CapturedRun& run, JSCell*& lastConstructorToSkip)
{
    InlineFrameReader reader;
    for (LogicalFrame frame = innermostLogicalFrame(image, reader, { site.code, site.site.callSite, site.site.inlineFrame, site.site.isTailDeleted(calleeWasEntered), site.site.calledBuiltin });; frame = callerOfInlinedFrame(image, reader, site.code, frame)) {
        if (filter.shows(FunctionDescription(frame.function).visibility(), frame.isTailDeleted)) {
            if (!lastConstructorToSkip)
                ++run.numberShown;
            else {
                ++run.numberOmitted;
                if (isClosureOf(lastConstructorToSkip, frame.function))
                    lastConstructorToSkip = nullptr;
            }
        }
        if (!frame.hasCallerInMachineFrame())
            break;
    }
}

static void captureRun(RunWalk& walk, CapturedRun& run, size_t maxStackSize, JSCell* lastConstructorToSkip, const FramesAboveRun& framesAbove, bool calleeWasEntered, bool hasRecordedCallees)
{
    using Trace = ReturnAddressTrace;
    ASSERT(!walk.isAtEnd());
    uintptr_t codeStart = std::bit_cast<uintptr_t>(walk.image().code().data());
    ASSERT(codeStart == imageCodeStart());
    ShownFrameFilter filter(framesAbove.exist || lastConstructorToSkip);
    run.numberShown = framesAbove.numberShown;
    CompiledFunctionID outermostCode;

    for (; !walk.isAtEnd(); walk.advance(), calleeWasEntered = true) {
        const CallSiteCache::Entry& site = walk.site();
        if (site.site.isConstruct() && !run.returnAddresses.isEmpty())
            run.addFlagToLast(Trace::Flag::IsConstructCall);
        outermostCode = site.code;
        if (hasRecordedCallees && FunctionDescription(walk.image().rootFunctionOf(site.code.index())).recordsCallee()) [[unlikely]] {
            run.hasFrameNamedByCallee = true;
            return;
        }

        run.append(Trace::encode(site.address, codeStart));
        if (walk.isAtFirstReadSite())
            run.addFlagToLast(Trace::Flag::IsAtFirstReadSite);

        std::optional<unsigned> shownFrameCount;
        if (!lastConstructorToSkip)
            shownFrameCount = site.shownFrameCountFor(filter.hasShownFrame(), calleeWasEntered);
        bool mayShowFrame = !shownFrameCount || *shownFrameCount;
        if (run.numberShown >= maxStackSize && mayShowFrame)
            break;

        if (shownFrameCount) [[likely]] {
            run.numberShown += *shownFrameCount;
            if (*shownFrameCount)
                filter = ShownFrameFilter(true);
            continue;
        }
        countLogicalFrames(walk.image(), site, calleeWasEntered, filter, run, lastConstructorToSkip);
    }

    if (walk.isAtEnd() && walk.runEnd().constructs(walk.image().rootFunctionOf(outermostCode.index())))
        run.addFlagToLast(Trace::Flag::IsConstructCall);
    run.foundConstructorToSkip = !lastConstructorToSkip;
}

static bool isSameFrame(const StackFrame& expected, const StackFrame& actual)
{
    if (expected.isCompiledFrame() != actual.isCompiledFrame() || expected.isAsyncFrame() != actual.isAsyncFrame())
        return false;
    if (expected.isCompiledFrame()) {
        const TraceFrame& left = expected.compiledFrame();
        const TraceFrame& right = actual.compiledFrame();
        return left.code == right.code && left.function == right.function && left.callSite.bits() == right.callSite.bits() && left.isInlined == right.isInlined && left.isConstructCall == right.isConstructCall && left.isClassCalledWithoutNew == right.isClassCalledWithoutNew;
    }
    if (expected.callee() != actual.callee() || expected.codeBlock() != actual.codeBlock() || expected.isWasmFrame() != actual.isWasmFrame() || expected.hasBytecodeIndex() != actual.hasBytecodeIndex())
        return false;
    return !expected.hasBytecodeIndex() || expected.bytecodeIndex() == actual.bytecodeIndex();
}

static FunctionID compiledFunctionAt(const WTF::Vector<StackFrame>& frames, size_t index)
{
    if (index >= frames.size() || !frames[index].isCompiledFrame())
        return { };
    return frames[index].compiledFrame().function;
}

static void validateCapturedTrace(VM& vm, JSCell* owner, const ReturnAddressTrace& trace, const WTF::Vector<StackFrame>* otherFrames, size_t maxStackSize, JSCell* lastConstructorToSkip)
{
    WTF::Vector<StackFrame> expected;
    vm.interpreter.getStackTrace(owner, expected, 0, maxStackSize, lastConstructorToSkip);

    WTF::Vector<StackFrame> actual;
    size_t firstOtherFrame = trace.numberOfFramesOnTop;
    if (firstOtherFrame)
        actual.append(otherFrames->span().first(firstOtherFrame));
    forEachShownFrame(trace, [&](const TraceFrame& frame) {
        actual.append(StackFrame(frame));
    });
    if (otherFrames) {
        for (size_t i = firstOtherFrame; i < otherFrames->size(); ++i)
            actual.append(otherFrames->at(i));
    }

    size_t numberCompared = std::min(expected.size(), actual.size());
    size_t index = 0;
    while (index < numberCompared && isSameFrame(expected[index], actual[index]))
        ++index;
    if (index == expected.size() && index == actual.size())
        return;
    FunctionID function = compiledFunctionAt(expected, index);
    if (!function)
        function = compiledFunctionAt(actual, index);
    crashOnValidationFailure(vm, { ValidationFailureKind::StackTraceDiffers, function, { }, { } });
}

static void fillTrace(ReturnAddressTrace& trace, Instance* instance, const CapturedRun& run, size_t maxStackSize, const FramesAboveRun& framesAbove, bool calleeOfFirstFrameWasEntered)
{
    trace.instance = instance;
    trace.limit = std::min<size_t>(maxStackSize, ReturnAddressTrace::maxLimit);
    trace.omittedFrameCount = run.numberOmitted;
    trace.numberOfReturnAddresses = run.returnAddresses.size();
    trace.numberOfFramesOnTop = framesAbove.numberShown;
    trace.isBelowOtherFrames = framesAbove.exist;
    trace.calleeOfFirstFrameWasEntered = calleeOfFirstFrameWasEntered;
    ASSERT(run.flagWords.size() == ReturnAddressTrace::numberOfFlagWords(run.returnAddresses.size()));
    memcpySpan(trace.returnAddresses(), run.returnAddresses.span());
    memcpySpan(trace.flagWords(), run.flagWords.span());
}

static bool fitsInTrace(const CapturedRun& run)
{
    return run.numberShown <= ReturnAddressTrace::maxLimit && run.numberOmitted <= ReturnAddressTrace::maxOmittedFrames && run.returnAddresses.size() <= ReturnAddressTrace::maxNumberOfReturnAddresses;
}

static std::optional<RunStart> findRunAtTop(VM& vm)
{
    if (!isTopFrameCompiled(vm))
        return std::nullopt;
    CallFrame* operationRecord = vm.topCallFrame;
    return RunStart { stackPointerOfCaller(operationRecord), operationRecord->callerFrame(), operationRecord->rawReturnPC(), vm.topEntryFrame, false, false };
}

void recordCallee(VM& vm, JSObject* closure)
{
    CallFrame* operationRecord = vm.topCallFrame;
    RunWalk walk(vm.aotState(), { stackPointerOfCaller(operationRecord), operationRecord->callerFrame(), operationRecord->rawReturnPC(), vm.topEntryFrame, false, false });
    RELEASE_ASSERT(!walk.isAtEnd());
    FunctionTables::of(vm).recordCallee(vm, walk.callerStackPointer(), closure);
}

static NEVER_INLINE std::optional<RunStart> findRunBelowOtherFrames(VM& vm, JSCell* owner, WTF::Vector<StackFrame>& framesOnTop, size_t maxStackSize)
{
    std::optional<CompiledFrameIterator> frames;
    vm.interpreter.appendStackTrace(owner, framesOnTop, { .callFrame = vm.topCallFrame, .compiledFramesBelow = &frames }, maxStackSize);
    if (!frames || frames->isAtEnd() || framesOnTop.size() > ReturnAddressTrace::maxFramesOnTop)
        return std::nullopt;
    return RunStart { frames->stackPointer(), frames->framePointer(), const_cast<void*>(frames->callSite().address), frames->entryFrame(), frames->isAtFirstReadSite(), frames->calleeWasEntered() };
}

static Instance* findInstance(VM& vm, const RunStart& start)
{
    const VMState& state = vm.aotState();
    if (state.instances.size() == 1) [[likely]]
        return state.instances.first();
    auto frames = CompiledFrameIterator::tryCreate(vm, start.framePointer, start.rawPC, start.entryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, start.stackPointer);
    return frames && !frames->isAtEnd() ? frames->instance() : nullptr;
}

// Interpreter::appendStackTrace adds three things below a run: the frames that the way out of the run leads to, those of the embedder, and those of suspended async functions.
static bool mayHaveFramesBelow(VM& vm, const RunWalk& walk, EntryFrame* entryFrameOfRun, bool hasRoom)
{
    if (hasRoom) {
        if (walk.exit().callFrame)
            return true;
#if USE(BUN_JSC_ADDITIONS)
        if (vm.onAppendStackTrace())
            return true;
#else
        UNUSED_PARAM(vm);
#endif
    }
    if (!Options::useAsyncStackTrace() || !entryFrameOfRun)
        return false;
    JSCell* context = vmEntryRecord(entryFrameOfRun)->m_context;
    return context && JSC::Interpreter::asyncFunctionIn(context);
}

ReturnAddressTrace* tryCaptureStackTrace(VM& vm, JSCell* owner, size_t maxStackSize, std::unique_ptr<WTF::Vector<StackFrame>>& otherFrames, JSCell* lastConstructorToSkip)
{
    if (!vm.hasCompiledCode() || !vm.topCallFrame || !maxStackSize)
        return nullptr;
    std::optional<AssertNoGC> assertNoGC { std::in_place };

    WTF::Vector<StackFrame> results;
    FramesAboveRun framesAbove;
    std::optional<RunStart> start = findRunAtTop(vm);
    if (!start) {
        if (lastConstructorToSkip)
            return nullptr;
        start = findRunBelowOtherFrames(vm, owner, results, maxStackSize);
        if (!start)
            return nullptr;
        framesAbove = { results.size(), true };
    }
    // A return address does not say which function object runs in the frame.
    bool hasRecordedCallees = FunctionTables::of(vm).hasRecordedCallees();
    Instance* instance = findInstance(vm, *start);
    if (!instance)
        return nullptr;

    RunWalk walk(vm.aotState(), *start);
    if (walk.isAtEnd())
        return nullptr;
    CapturedRun run;
    captureRun(walk, run, maxStackSize, lastConstructorToSkip, framesAbove, start->calleeWasEntered, hasRecordedCallees);
    if (run.hasFrameNamedByCallee || !run.numberShown || !run.foundConstructorToSkip || !fitsInTrace(run))
        return nullptr;

    assertNoGC.reset();
    auto* trace = static_cast<ReturnAddressTrace*>(vm.auxiliarySpace().allocate(vm, ReturnAddressTrace::allocationSize(run.returnAddresses.size()), nullptr, AllocationFailureMode::ReturnNull));
    assertNoGC.emplace();
    if (!trace)
        return nullptr;
    fillTrace(*trace, instance, run, maxStackSize, framesAbove, start->calleeWasEntered);

    // The frames below go behind the frames on top, so the frames on top count as results and not as frames above.
    size_t numberOfFramesAbove = std::min(run.numberShown, maxStackSize);
    if (mayHaveFramesBelow(vm, walk, start->entryFrame, numberOfFramesAbove < maxStackSize)) {
        vm.interpreter.appendStackTrace(owner, results, {
            .compiledFrameExit = walk.isAtEnd() ? &walk.exit() : nullptr,
            .entryFrameOfFramesAbove = start->entryFrame,
            .numberOfFramesAbove = numberOfFramesAbove - framesAbove.numberShown,
        }, maxStackSize);
    }
    if (!results.isEmpty()) {
        results.shrinkToFit();
        otherFrames = makeUnique<WTF::Vector<StackFrame>>(WTF::move(results));
    }

    if (validatesRuntime(vm)) [[unlikely]]
        validateCapturedTrace(vm, owner, *trace, otherFrames.get(), maxStackSize, lastConstructorToSkip);
    return trace;
}

void forEachShownFrame(const ReturnAddressTrace& trace, const ScopedLambda<void(const TraceFrame&)>& functor)
{
    const Image& image = *Image::current();
    CallSiteCache& cache = *trace.instance->vm->aotState().callSiteCache;
    uintptr_t codeStart = imageCodeStart();
    InlineFrameReader reader;
    ShownFrameFilter filter(trace.isBelowOtherFrames || trace.omittedFrameCount);
    unsigned numberToOmit = trace.omittedFrameCount;
    size_t numberShown = trace.numberOfFramesOnTop;
    std::optional<TraceFrame> pending;
    FunctionID pendingSource;
    auto flush = [&] {
        if (pending)
            functor(*std::exchange(pending, std::nullopt));
    };

    auto returnAddresses = trace.returnAddresses();
    for (size_t i = 0; i < returnAddresses.size(); ++i) {
        auto flags = trace.flagsAt(i);
        const CallSiteCache::Entry* entry = cache.resolve(image, ReturnAddressTrace::decode(returnAddresses[i], codeStart));
        RELEASE_ASSERT(entry && !entry->isStubReturnSite());
        CompiledFunctionID code = entry->code;
        CallSiteRecord record = entry->site;
        CallSiteIndex site = flags.contains(ReturnAddressTrace::Flag::IsAtFirstReadSite) && record.firstReadSite ? *record.firstReadSite : record.callSite;
        bool isTailDeleted = record.isTailDeleted(i || trace.calleeOfFirstFrameWasEntered);
        for (LogicalFrame frame = innermostLogicalFrame(image, reader, { code, site, record.inlineFrame, isTailDeleted, record.calledBuiltin });; frame = callerOfInlinedFrame(image, reader, code, frame)) {
            if (pending && !isEntrypointOf(frame.function, pendingSource))
                flush();

            FunctionDescription description(frame.function);
            if (filter.shows(description.visibility(), frame.isTailDeleted)) {
                if (numberToOmit)
                    --numberToOmit;
                else {
                    flush();
                    if (numberShown == trace.limit)
                        return;
                    ++numberShown;
                    pending = TraceFrame { code, frame.function, frame.callSite, !!frame.inlineFrame, false };
                    pendingSource = description.sourceFunction();
                }
            }

            if (pending && !frame.isOfCalledBuiltin && (frame.inlineFrame ? frame.isInlinedConstructCall : flags.contains(ReturnAddressTrace::Flag::IsConstructCall))) {
                pending->isConstructCall = true;
                flush();
            }
            if (!frame.hasCallerInMachineFrame())
                break;
        }
    }
    flush();
}

void forEachShownFrame(CompiledFunctionID code, const CallSiteRecord& site, ShownFrameFilter& filter, const ScopedLambda<void(const TraceFrame&)>& functor)
{
    const Image& image = *Image::current();
    InlineFrameReader reader;
    for (LogicalFrame frame = innermostLogicalFrame(image, reader, { code, site.callSite, site.inlineFrame, false });; frame = callerOfInlinedFrame(image, reader, code, frame)) {
        if (filter.shows(FunctionDescription(frame.function).visibility(), frame.isTailDeleted))
            functor(TraceFrame { code, frame.function, frame.callSite, !!frame.inlineFrame, frame.isInlinedConstructCall });
        if (!frame.inlineFrame)
            break;
    }
}

// Bytecode enters the constructor and throws there, so the stack has a frame of the class at the start of its parameters.
std::optional<TraceFrame> tryGetFrameOfClassCalledWithoutNew(VM& vm, CallFrame* callFrame)
{
    if (vm.aotState().classCalledWithoutNew != callFrame) [[likely]]
        return std::nullopt;
    FunctionID function = uncheckedDowncast<Closure>(callFrame->jsCallee())->function();
    if (FunctionDescription(function).visibility() == TraceVisibility::Hidden)
        return std::nullopt;
    const Image& image = *Image::current();
    for (FunctionID candidate : { function, sourceFunctionOf(function) }) {
        if (auto code = image.imageFunctionOf(candidate))
            return TraceFrame { CompiledFunctionID(*code), function, CallSiteIndex(0), false, false, true };
    }
    return std::nullopt;
}

std::optional<SourcePosition> positionOf(const TraceFrame& frame)
{
    const Image& image = *Image::current();
    if (image.isObfuscated())
        return SourcePosition { obfuscatedPositionOf(frame), ModuleID() };
    return image.positionOf(frame.code.index(), frame.callSite);
}

LineColumn obfuscatedPositionOf(const TraceFrame& frame)
{
    return { frame.code.index() + 1, frame.callSite.bits() + 1 };
}

String sourceURLOf(ModuleID module)
{
    return module ? modulePath(module) : emptyString();
}

String describeCodeAddresses(std::span<const void* const> addresses)
{
    const Image* image = Image::current();
    if (!image)
        return { };
    StringBuilder result;
    ShownFrameFilter filter;
    for (const void* address : addresses) {
        CodeLocation location = image->locate(address);
        if (location.kind != CodeKind::Function)
            continue;
        CallSiteRecord site = image->callSiteAt(location.function, location.offset).value_or(CallSiteRecord { CallSiteIndex(0), 0 });
        forEachShownFrame(CompiledFunctionID(location.function), site, filter, [&](const TraceFrame& frame) {
            if (image->isObfuscated()) {
                LineColumn position = obfuscatedPositionOf(frame);
                result.append("    at "_s, position.line, ':', position.column, '\n');
                return;
            }
            bool isModule = FunctionDescription(frame.function).codeType() == ModuleCode;
            String name = isModule ? String() : writtenFunctionName(frame.function);
            auto position = positionOf(frame);
            String sourceLocation = position && position->source ? makeString(sourceURLOf(position->source), ':', position->lineColumn.line, ':', position->lineColumn.column) : String();
            if (isModule)
                result.append("    at "_s, sourceLocation, '\n');
            else if (sourceLocation.isEmpty())
                result.append("    at "_s, name.isEmpty() ? StringView("<anonymous>"_s) : StringView(name), '\n');
            else
                result.append("    at "_s, name.isEmpty() ? StringView("<anonymous>"_s) : StringView(name), " ("_s, sourceLocation, ")\n"_s);
        });
    }
    return result.toString();
}

struct SuspendedAsyncFunction {
    SavedFrame* coroutine { nullptr };
    JSPromise* promise { nullptr };
    JSCell* waiter { nullptr };
};

static ALWAYS_INLINE SavedFrame* tryGetSavedFrame(JSCell* cell)
{
    return cell->type() == ObjectType && cell->classInfo() == SavedFrame::info() ? uncheckedDowncast<SavedFrame>(cell) : nullptr;
}

static SuspendedAsyncFunction findSuspendedAsyncFunction(JSCell* cell)
{
    SavedFrame* saved = tryGetSavedFrame(cell);
    if (!saved || saved->kind() != CoroutineKind::AsyncFunction)
        return { };
    JSPromise* promise = saved->promise();
    return { saved, promise ? promise : saved->promiseOfCombinator(), saved->waiter() };
}

static void appendFrames(VM& vm, const SuspendedAsyncFunction& suspended, WTF::Vector<StackFrame>& results)
{
    if (!suspended.coroutine)
        return;
    const Image& image = *Image::current();
    // The function goes on behind the call that suspended it, so its resume address lies in it.
    const CallSiteCache::Entry* entry = vm.aotState().callSiteCache->resolve(image, suspended.coroutine->resumeAddress());
    if (!entry || entry->isStubReturnSite() || FunctionDescription(image.rootFunctionOf(entry->code.index())).codeType() == ModuleCode)
        return;
    ShownFrameFilter filter(true);
    forEachShownFrame(entry->code, entry->site, filter, [&](const TraceFrame& frame) {
        // A function that is not async cannot wait: it has handed on the promise of its callee and returned. Only its code was taken into the function that waits.
        if (FunctionDescription(frame.function).isAsync())
            results.constructAndAppend(frame, true);
    });
}

bool isSuspendedAsyncFunction(JSCell* cell)
{
    return findSuspendedAsyncFunction(cell).coroutine && !isSuspendedModuleBody(cell->vm(), cell);
}

bool isSuspendedModuleBody(VM& vm, JSValue coroutine)
{
    SavedFrame* saved = coroutine.isCell() ? tryGetSavedFrame(coroutine.asCell()) : nullptr;
    if (!saved)
        return false;
    const Image& image = *Image::current();
    const CallSiteCache::Entry* entry = vm.aotState().callSiteCache->resolve(image, saved->resumeAddress());
    return entry && !entry->isStubReturnSite() && FunctionDescription(image.rootFunctionOf(entry->code.index())).codeType() == ModuleCode;
}

JSPromise* asyncFunctionPromise(JSCell* coroutine)
{
    return findSuspendedAsyncFunction(coroutine).promise;
}

Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine)
{
    AssertNoGC assertNoGC;
    Vector<JSPromise*, 4> promises;
    if (auto* tuple = coroutine ? dynamicDowncast<InternalFieldTuple>(coroutine) : nullptr) {
        JSValue context = tuple->getInternalField(0);
        coroutine = context && context.isCell() ? context.asCell() : nullptr;
    }
    if (!coroutine)
        return promises;
    Vector<JSCell*, 4> work { coroutine };
    UncheckedKeyHashSet<JSCell*> visited;
    for (size_t i = 0; i < work.size(); ++i) {
        JSCell* current = work[i];
        if (!visited.add(current).isNewEntry)
            continue;
        if (auto* promise = dynamicDowncast<JSPromise>(current))
            promises.append(promise);
        else if (JSPromise* own = asyncFunctionPromise(current))
            promises.append(own);
        else if (auto* saved = tryGetSavedFrame(current); saved && saved->waiter())
            work.append(saved->waiter());
    }
    return promises;
}

void appendSuspendedAsyncFunction(VM& vm, JSCell*, WTF::Vector<StackFrame>& results, JSCell* coroutine)
{
    appendFrames(vm, findSuspendedAsyncFunction(coroutine), results);
}

JSCell* appendDirectAsyncCallers(VM& vm, JSCell*, WTF::Vector<StackFrame>& results, JSCell* coroutine, size_t maxStackSize)
{
    constexpr uint16_t mask = JSPromise::stateMask | JSPromise::inlineReactionKindMask | JSPromise::inlineReactionMicrotaskMask;
    constexpr auto isAwaitedWith = [](InternalMicrotask task) {
        return static_cast<uint16_t>(static_cast<uint16_t>(JSPromise::Status::Pending)
            | static_cast<uint16_t>(JSPromise::InlineReactionKind::InternalMicrotask) << JSPromise::inlineReactionKindShift
            | static_cast<uint16_t>(task) << JSPromise::inlineReactionMicrotaskShift);
    };
    SuspendedAsyncFunction current = findSuspendedAsyncFunction(coroutine);
    if (!current.coroutine)
        return coroutine;
    for (;;) {
        JSValue context = current.waiter;
        if (!current.waiter) {
            if (!current.promise)
                return nullptr;
            uint16_t flags = current.promise->flags();
            bool isAwaitedByCompiledCode = (flags & mask) == isAwaitedWith(InternalMicrotask::CompiledAsyncFunctionResume) || (flags & mask) == isAwaitedWith(InternalMicrotask::CompiledModuleBodyResume);
            if (!isAwaitedByCompiledCode || results.size() >= maxStackSize) {
                bool hasNoAwaiters = (flags & JSPromise::stateMask) != static_cast<uint16_t>(JSPromise::Status::Pending) || (!(flags & JSPromise::inlineReactionKindMask) && !current.promise->payloadCell());
                return hasNoAwaiters ? nullptr : current.coroutine;
            }
            context = current.promise->inlineReactionContext();
        } else if (results.size() >= maxStackSize)
            return nullptr;
        if (!context || !context.isCell())
            return current.coroutine;
        SuspendedAsyncFunction caller = findSuspendedAsyncFunction(context.asCell());
        if (!caller.coroutine)
            return current.coroutine;
        current = caller;
        appendFrames(vm, current, results);
        if (results.size() > maxStackSize)
            results.shrink(maxStackSize);
    }
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
