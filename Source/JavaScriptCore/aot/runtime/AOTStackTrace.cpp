/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStackTrace.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCallSiteTable.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTCompiledCaller.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTImageOfCellScope.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTModules.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeValidation.h"
#include "AOTSavedFrame.h"
#include "AOTStubFrames.h"
#include "AOTVector.h"
#include "BuiltinNames.h"
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
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(static_cast<unsigned>(TraceVisibility::Hidden) < 4 && static_cast<unsigned>(StackTraceBuiltinKind::HostFunction) < 4 && static_cast<unsigned>(ModuleCode) < 4);

FunctionID functionInFramesOf(BuiltinID builtin)
{
    ASSERT(builtin);
    return functionInFramesOf(builtin, Image::current()->programTables().numberOfFunctions());
}

BuiltinID builtinOfFunctionInFrames(FunctionID function)
{
    size_t numberOfFunctions = Image::current()->programTables().numberOfFunctions();
    return function.index() < numberOfFunctions ? BuiltinID() : BuiltinID(static_cast<uint32_t>(function.index() - numberOfFunctions));
}

static constexpr FunctionTraceData traceOfCalledBuiltin(StackTraceBuiltinKind kind)
{
    return {
        .visibilityBits = static_cast<uint16_t>(TraceVisibility::Visible),
        .builtinKindBits = static_cast<uint16_t>(kind),
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
        .recordsCalleeOnceRenamed = false,
    };
}
static constexpr FunctionTraceData traceOfCalledHostFunction = traceOfCalledBuiltin(StackTraceBuiltinKind::HostFunction);
static constexpr FunctionTraceData traceOfCalledJSBuiltin = traceOfCalledBuiltin(StackTraceBuiltinKind::JSBuiltin);

static bool isWrittenInJavaScript(BuiltinID builtin)
{
    std::span<const BuiltinOverloadSummary> overloads = BuiltinTable::singleton().overloadSummariesOf(builtin);
    return !overloads.empty() && std::ranges::all_of(overloads, [](const BuiltinOverloadSummary& overload) {
        return overload.callee == BuiltinCallee::Function;
    });
}

static JSFunction* findPrivateGlobalFunction(VM& vm, std::span<const Latin1Character> name)
{
    const auto& instances = vm.aotState().instances;
    PrivateSymbolImpl* uid = vm.propertyNames->builtinNames().lookUpPrivateName(name);
    if (!uid || instances.isEmpty())
        return nullptr;
    // The first may be the realm of a test file that has ended, which no collection has freed yet.
    JSGlobalObject* realm = instances.last()->globalObject;
    if (JSObject* property = findExistingObject(realm, uid))
        return dynamicDowncast<JSFunction>(property);
    PropertySlot slot(realm, PropertySlot::InternalMethodType::VMInquiry, &vm);
    JSValue variable = symbolTableGet(realm, uid, slot) ? slot.getValue(realm, uid) : JSValue();
    return variable ? dynamicDowncast<JSFunction>(variable) : nullptr;
}

// The end of a GC formats the stack of an error that has a dead function in its frames. No atom can be created and no cell allocated there.
static String nameOfBuiltin(VM& vm, BuiltinID builtin)
{
    if (auto function = BuiltinBindings::nativeFunctionOf(builtin); function && !function->name.isNull())
        return function->name;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName key = table.at(builtin).name;
    auto text = byteCast<Latin1Character>(table.textOf(key));
    // The host chooses the name of a function that it stores under a private name.
    if (table.at(builtin).holder == Builtins::PrivateGlobals) {
        if (JSFunction* function = findPrivateGlobalFunction(vm, text))
            return function->nameWithoutGC(vm);
    }
    if (!table.isSymbolName(key))
        return StringImpl::createWithoutCopying(text);
    return makeString(table.isRegisteredSymbolName(key) ? "["_s : "[Symbol."_s, text, ']');
}

bool g_imagesShareRealms;

unsigned positionOfRunningImageAmongSeveral()
{
    const Image* image = Image::current();
    return image ? image->position() : 0;
}

void ImageOfTraceFrameScope::runImageOf(const TraceFrame& frame)
{
    const Image* running = Image::current();
    if (running->position() == frame.imagePosition) [[likely]]
        return;
    m_previous = running;
    runImageOnThisThread(*running->atPosition(frame.imagePosition));
}

void ImageOfTraceFrameScope::restore()
{
    runImageOnThisThread(*m_previous);
}

FunctionDescription::FunctionDescription(FunctionID function)
    : m_function(function)
{
    const Image* image = Image::current();
    RELEASE_ASSERT(image && function);
    if (BuiltinID builtin = builtinOfFunctionInFrames(function)) [[unlikely]] {
        m_record = isWrittenInJavaScript(builtin) ? &traceOfCalledJSBuiltin : &traceOfCalledHostFunction;
        return;
    }
    m_record = &image->programTables().traceOfFunction(function.index());
}

bool FunctionDescription::isOfCalledBuiltin() const
{
    return m_record == &traceOfCalledHostFunction || m_record == &traceOfCalledJSBuiltin;
}

String FunctionDescription::name(VM& vm) const
{
    if (isOfCalledBuiltin())
        return nameOfBuiltin(vm, builtinOfFunctionInFrames(m_function));
    return Image::current()->isObfuscated() ? emptyString() : functionName(vm, *this, NameForm::AsWritten);
}

String FunctionDescription::nameInFrames(VM& vm, JSCell* callee, bool calleeLiesInFrame) const
{
    if (isOfCalledBuiltin())
        return nameOfBuiltin(vm, builtinOfFunctionInFrames(m_function));
    return Image::current()->isObfuscated() ? emptyString() : functionName(vm, *this, NameForm::InFrames, callee ? asObject(callee) : nullptr, calleeLiesInFrame);
}

String FunctionDescription::calculatedDisplayName(VM& vm, JSCell* callee, bool calleeLiesInFrame) const
{
    if (isOfCalledBuiltin())
        return nameOfBuiltin(vm, builtinOfFunctionInFrames(m_function));
    return Image::current()->isObfuscated() ? emptyString() : functionName(vm, *this, NameForm::CalculatedDisplayName, callee ? asObject(callee) : nullptr, calleeLiesInFrame);
}

#define AOT_DEFINE_FLAG_OF_TRACE_RECORD(flag) \
    bool FunctionDescription::flag() const { return m_record->flag; }
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isStrict)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isConstructor)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isNakedConstructor)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(isNamedByComputedKey)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(recordsCallee)
AOT_DEFINE_FLAG_OF_TRACE_RECORD(recordsCalleeOnceRenamed)
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
    return !isOfCalledBuiltin() && functionKindData(m_function).isInRuntimeModule;
}

TraceVisibility FunctionDescription::visibility() const
{
    return m_record->visibility();
}

FunctionID FunctionDescription::sourceFunction() const
{
    return isOfCalledBuiltin() ? m_function : sourceFunctionOf(m_function);
}

StackTraceBuiltinKind FunctionDescription::builtinKind() const
{
    return m_record->builtinKind();
}

std::optional<SourcePosition> FunctionDescription::definition() const
{
    if (isOfCalledBuiltin())
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
    // CompiledFrameIterator::plainFunctionConstructFrameLeft().
    CallFrame* plainFunctionConstructFrame { nullptr };
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
        , m_plainFunctionConstructFrame(start.plainFunctionConstructFrame)
    {
        ASSERT(m_framePointer);
        findFunctionFrame();
    }

    const Image& image() const { return m_image; }
    bool isAtEnd() const { return !m_site; }
    bool isAmongFramesThatWait() const { return m_isAmongFramesThatWait; }
    const CallSiteCache::Entry& site() const
    {
        ASSERT(!isAtEnd());
        return *m_site;
    }
    bool isAtFirstReadSite() const { return m_isAtFirstReadSite && site().site.firstReadSite; }
    // The function that site() has constructed, where the stack says which.
    FunctionID constructedFunction() const { return functionConstructedAt(m_image, site(), m_plainFunctionConstructFrame); }
    bool hasLeftPlainFunctionConstructFrame() const { return m_plainFunctionConstructFrame; }
    void* callerStackPointer() const { return stackPointerOfCaller(m_callFrame, bytesAboveReturnAddressOf(site())); }
    CallFrame* machineFrame() const { return m_callFrame; }
    RunStart position() const { return { m_stackPointer, m_framePointer, m_rawPC, m_entryFrame, m_isAtFirstReadSite, true }; }
    // Another walk has asked the cache, which may have moved its entries.
    void findSiteAgain()
    {
        if (m_site)
            m_site = m_cache.resolve(m_image, removeCodePtrTag(m_rawPC));
        if (m_isAtFirstReadSite)
            m_site = m_cache.withMethodReadSite(m_image, m_site);
    }
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
        m_plainFunctionConstructFrame = nullptr;
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

    NEVER_INLINE bool passOverMethodReadFrame()
    {
        m_isAtFirstReadSite = true;
        if (!passOverFrame())
            return false;
        m_cache.withMethodReadSite(m_image, m_cache.find(removeCodePtrTag(m_rawPC)));
        return true;
    }

    bool passOverStubFrame(StubFrameKind kind)
    {
        switch (kind) {
        case StubFrameKind::Plain:
        case StubFrameKind::ReturnsAfterOperation:
        case StubFrameKind::EngineCallee:
        case StubFrameKind::HostCall:
        case StubFrameKind::GeneratorBody:
        case StubFrameKind::ArgumentList:
        case StubFrameKind::BelowSpilledArguments:
        case StubFrameKind::AtStubEntry:
            break;
        case StubFrameKind::PlainFunctionConstruct:
            m_plainFunctionConstructFrame = m_callFrame;
            break;
        case StubFrameKind::MethodRead:
            return passOverMethodReadFrame();
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
                if (m_isAtFirstReadSite)
                    m_cache.withMethodReadSite(m_image, m_site);
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
    CallFrame* m_plainFunctionConstructFrame { nullptr };
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
    bool hasFrameListedBehindTailCall { false };
};

struct FramesAboveRun {
    size_t numberShown { 0 };
    bool exist { false };
};

static ALWAYS_INLINE void countLogicalFrame(FunctionID function, TraceVisibility visibility, bool isTailDeleted, ShownFrameFilter& filter, CapturedRun& run, JSCell*& lastConstructorToSkip)
{
    if (!filter.shows(visibility, isTailDeleted))
        return;
    if (!lastConstructorToSkip) {
        ++run.numberShown;
        return;
    }
    ++run.numberOmitted;
    if (isClosureOf(lastConstructorToSkip, function))
        lastConstructorToSkip = nullptr;
}

static NEVER_INLINE void countLogicalFrames(const Image& image, InlineFrameReader& reader, const CallSiteCache::Entry& site, bool calleeWasEntered, ShownFrameFilter& filter, CapturedRun& run, JSCell*& lastConstructorToSkip)
{
    for (LogicalFrame frame = innermostLogicalFrame(image, reader, { site.code, site.site.callSite, site.site.inlineFrame, site.site.isTailDeleted(calleeWasEntered), site.site.calledBuiltin, calleeWasEntered });; frame = callerOfInlinedFrame(image, reader, site.code, frame)) {
        countLogicalFrame(frame.function, FunctionDescription(frame.function).visibility(), frame.isTailDeleted, filter, run, lastConstructorToSkip);
        if (!frame.hasCallerInMachineFrame())
            break;
    }
}

// `mayLieBelowOtherRuns`: in front of its addresses may stand those of the runs above, which are of other images.
template<bool mayLieBelowOtherRuns>
static void captureRun(VM& vm, RunWalk& walk, CapturedRun& run, size_t maxStackSize, JSCell* lastConstructorToSkip, const FramesAboveRun& framesAbove, bool calleeWasEntered, bool hasRecordedCallees)
{
    using Trace = ReturnAddressTrace;
    ASSERT(!walk.isAtEnd());
    uintptr_t codeStart = std::bit_cast<uintptr_t>(walk.image().code().data());
    ASSERT(codeStart == imageCodeStart());
    ShownFrameFilter filter(framesAbove.exist || lastConstructorToSkip);
    run.numberShown = framesAbove.numberShown;
    size_t firstOfRun = mayLieBelowOtherRuns ? run.returnAddresses.size() : 0;
    CompiledFunctionID outermostCode;
    // Of the nearest machine frame above that is not Entry::isOfEntrypointAtTailCall().
    CompiledFunctionID codeAbove;
    bool calleeOfFirstFrameWasEntered = calleeWasEntered;
    auto functionAboveIsHiddenBehindTailCall = [&]() ALWAYS_INLINE_LAMBDA {
        CallSiteCache& cache = *vm.aotState().callSiteCache;
        auto outermost = OutermostVisibleFunction::None;
        bool mayHaveMovedEntries = false;
        for (size_t index = run.returnAddresses.size(); index-- > firstOfRun && outermost == OutermostVisibleFunction::None;) {
            const void* address = Trace::decode(run.returnAddresses[index], codeStart);
            const CallSiteCache::Entry* above = cache.find(address);
            if (!above) [[unlikely]] {
                above = cache.resolve(walk.image(), address);
                mayHaveMovedEntries = true;
            }
            outermost = index > firstOfRun || calleeOfFirstFrameWasEntered || !above->site.isTailCall() ? above->outermostVisibleFunction : findOutermostVisibleFunction(walk.image(), cache.inlineFrames(), *above, false);
        }
        if (mayHaveMovedEntries) [[unlikely]]
            walk.findSiteAgain();
        return outermost == OutermostVisibleFunction::HiddenBehindTailCall;
    };
    // False: the run is not listed by return addresses.
    auto didFindConstructionOf = [&](FunctionID constructed) ALWAYS_INLINE_LAMBDA {
        // Where nothing on the stack says which function was constructed, it is the one that the frame above was entered for.
        bool isFunctionAbove = codeAbove && (!constructed || isCodeOf(walk.image(), codeAbove, constructed));
        // A function that is constructed waits for what its last call returns, so it is listed though a tail call of its own hides its frame or has taken it from the stack. Return addresses do not say that.
        if (isFunctionAbove ? functionAboveIsHiddenBehindTailCall() : constructed && frameOfConstructedFunctionThatLeft(walk.image(), constructed, codeAbove)) [[unlikely]] {
            run.hasFrameListedBehindTailCall = true;
            return false;
        }
        if (isFunctionAbove)
            run.addFlagToLast(Trace::Flag::IsConstructCall);
        return true;
    };

    for (;; walk.advance(), calleeWasEntered = true) {
        bool isAtEnd = walk.isAtEnd();
        if (isAtEnd ? walk.runEnd().isConstruct : walk.site().site.isConstruct()) {
            FunctionID constructed = isAtEnd ? walk.runEnd().constructedFunction() : walk.constructedFunction();
            if (isAtEnd && !constructed) {
                if (walk.runEnd().constructs(walk.image().rootFunctionOf(outermostCode.index())))
                    run.addFlagToLast(Trace::Flag::IsConstructCall);
            } else if (!didFindConstructionOf(constructed)) [[unlikely]]
                return;
        }
        if (isAtEnd)
            break;
        const CallSiteCache::Entry& site = walk.site();
        outermostCode = site.code;
        if (!codeAbove || !site.isOfEntrypointAtTailCall())
            codeAbove = outermostCode;
        // Whoever keeps the callee of a frame, only a function that is named by its callee needs it.
        if (hasRecordedCallees && (site.hasInlinedFrameRecordingCallee || (site.hasInlinedFrameRecordingCalleeOnceRenamed && FunctionTables::of(vm).recordsCalleesOfCallsByValue()) || FunctionTables::of(vm).framesAreNamedByCallee(walk.image().rootFunctionOf(site.code.index())))) [[unlikely]] {
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
        if (!site.site.inlineFrame && !site.site.calledBuiltin)
            countLogicalFrame(site.innermostFunction, site.innermostVisibility, site.site.isTailDeleted(calleeWasEntered), filter, run, lastConstructorToSkip);
        else
            countLogicalFrames(walk.image(), vm.aotState().callSiteCache->inlineFrames(), site, calleeWasEntered, filter, run, lastConstructorToSkip);
    }

    run.foundConstructorToSkip = !lastConstructorToSkip;
}

static bool isSameFrame(const StackFrame& expected, const StackFrame& actual)
{
    if (expected.isCompiledFrame() != actual.isCompiledFrame() || expected.isAsyncFrame() != actual.isAsyncFrame())
        return false;
    if (expected.isCompiledFrame()) {
        const TraceFrame& left = expected.compiledFrame();
        const TraceFrame& right = actual.compiledFrame();
        return left.code == right.code && left.function == right.function && left.callSite.bits() == right.callSite.bits() && left.isInlined == right.isInlined && left.isConstructCall == right.isConstructCall && left.isClassCalledWithoutNew == right.isClassCalledWithoutNew && left.isAtFailedTailCall == right.isAtFailedTailCall;
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

void validateFrames(VM& vm, JSCell* owner, const WTF::Vector<StackFrame>& actual, size_t maxStackSize, JSCell* lastConstructorToSkip)
{
    WTF::Vector<StackFrame> expected;
    vm.interpreter.appendStackTrace(owner, expected, { .callFrame = vm.topCallFrame, .caller = lastConstructorToSkip, .visitsEachCompiledFrame = true }, maxStackSize);

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

static void validateCapturedTrace(VM& vm, JSCell* owner, const ReturnAddressTrace& trace, const WTF::Vector<StackFrame>& otherFrames, size_t maxStackSize, JSCell* lastConstructorToSkip)
{
    WTF::Vector<StackFrame> actual;
    size_t firstOtherFrame = trace.numberOfFramesOnTop;
    actual.append(otherFrames.span().first(firstOtherFrame));
    forEachShownFrame(trace, [&](const TraceFrame& frame) {
        actual.append(StackFrame(frame));
    });
    actual.append(otherFrames.span().subspan(firstOtherFrame));
    validateFrames(vm, owner, actual, maxStackSize, lastConstructorToSkip);
}

static void fillTrace(ReturnAddressTrace& trace, Instance* instance, const CapturedRun& run, size_t maxStackSize, const FramesAboveRun& framesAbove, bool calleeOfFirstFrameWasEntered)
{
    trace.instance = instance;
    trace.limit = std::min<size_t>(maxStackSize, ReturnAddressTrace::maxLimit);
    trace.isFull = false;
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

// A built-in function that makes an error stands in one frame on top of the code that called it.
static std::optional<RunStart> findRunBelowHostFunction(VM& vm)
{
    CallFrame* top = vm.topCallFrame;
    if (top->callerFrameOrEntryFrame() == vm.topEntryFrame || top->isZombieFrame() || top->isNativeCalleeFrame() || top->codeBlock() || vm.aotState().classCalledWithoutNew == top)
        return std::nullopt;
    JSCell* callee = top->callee().asCell();
    auto* function = callee ? dynamicDowncast<JSFunction>(callee) : nullptr;
    if (!function || function->executable()->implementationVisibility() != ImplementationVisibility::Public)
        return std::nullopt;
    const CallSiteCache::Entry* site = vm.aotState().callSiteCache->find(removeCodePtrTag(top->rawReturnPC()));
    if (!site || !site->isStubReturnSite() || site->stubFrameKind != StubFrameKind::HostCall)
        return std::nullopt;
    return RunStart { stackPointerOfCaller(top), top->callerFrame(), top->rawReturnPC(), vm.topEntryFrame, false, true };
}

static NEVER_INLINE std::optional<RunStart> findRunBelowOtherFrames(VM& vm, JSCell* owner, WTF::Vector<StackFrame>& framesOnTop, size_t maxStackSize)
{
    std::optional<CompiledFrameIterator> frames;
    vm.interpreter.appendStackTrace(owner, framesOnTop, { .callFrame = vm.topCallFrame, .compiledFramesBelow = &frames }, maxStackSize);
    if (!frames || frames->isAtEnd() || framesOnTop.size() > ReturnAddressTrace::maxFramesOnTop)
        return std::nullopt;
    return RunStart { frames->stackPointer(), frames->framePointer(), const_cast<void*>(frames->callSite().address), frames->entryFrame(), frames->isAtFirstReadSite(), frames->calleeWasEntered(), frames->plainFunctionConstructFrameLeft() };
}

static NEVER_INLINE Instance* findInstanceAmongSeveral(VM& vm, const RunStart& start)
{
    auto frames = CompiledFrameIterator::tryCreate(vm, start.framePointer, start.rawPC, start.entryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, start.stackPointer);
    return frames && !frames->isAtEnd() ? frames->instance() : nullptr;
}

// Interpreter::appendStackTrace adds three things below a run: the frames that the way out of the run leads to, those of the embedder, and those of suspended async functions.
static ALWAYS_INLINE void appendFramesBelow(VM& vm, JSCell* owner, WTF::Vector<StackFrame>& results, const RunWalk& walk, bool hasRoom, JSC::Interpreter::StackTraceStart start, size_t maxStackSize)
{
    auto reserve = [&] {
        if (!results.capacity())
            results.reserveInitialCapacity(std::min(maxStackSize, CapturedRun::inlineCapacity / 2));
    };
    bool mayHaveFramesOnStack = hasRoom && walk.exit().callFrame;
#if USE(BUN_JSC_ADDITIONS)
    mayHaveFramesOnStack |= hasRoom && vm.onAppendStackTrace();
#endif
    if (mayHaveFramesOnStack) {
        reserve();
        start.compiledFrameExit = &walk.exit();
        vm.interpreter.appendStackTrace(owner, results, start, maxStackSize);
        return;
    }
    if (!Options::useAsyncStackTrace() || !start.entryFrameOfFramesAbove)
        return;
    JSCell* awaiter = JSC::Interpreter::asyncFunctionIn(vmEntryRecord(start.entryFrameOfFramesAbove)->m_context);
    if (!awaiter)
        return;
    reserve();
    vm.interpreter.getAsyncStackTrace(owner, results, awaiter, maxStackSize < std::numeric_limits<size_t>::max() - results.size() ? results.size() + maxStackSize : std::numeric_limits<size_t>::max());
}

// What is left to do once the return addresses are there. `runs`: ReturnAddressTrace::encodeRun(), none in a VM of one image.
static ALWAYS_INLINE ReturnAddressTrace* tryMakeTrace(VM& vm, JSCell* owner, size_t maxStackSize, WTF::Vector<StackFrame>& otherFrames, JSCell* lastConstructorToSkip, const Invocable<void*(size_t)> auto& allocate, Instance* instance, const CapturedRun& run, std::span<const uint32_t> runs, const RunWalk& walk, const FramesAboveRun& framesAbove, bool calleeOfFirstFrameWasEntered, EntryFrame* entryFrameOfLastRun)
{
    if (run.hasFrameNamedByCallee || run.hasFrameListedBehindTailCall || !run.numberShown || !run.foundConstructorToSkip || !fitsInTrace(run))
        return nullptr;

    auto* trace = static_cast<ReturnAddressTrace*>(allocate(ReturnAddressTrace::allocationSize(run.returnAddresses.size()) + ReturnAddressTrace::sizeOfRunTable(runs.size())));
    if (!trace)
        return nullptr;
    fillTrace(*trace, instance, run, maxStackSize, framesAbove, calleeOfFirstFrameWasEntered);
    if (!runs.empty()) {
        uint32_t* table = trace->runTable();
        *table = runs.size();
        memcpySpan(std::span(table + 1, runs.size()), runs);
    }

    // The frames below go behind the frames on top, so the frames on top count as results and not as frames above.
    size_t numberOfFramesAbove = std::min(run.numberShown, maxStackSize);
    // The body of an async generator that has gone on behind an await runs above the function that the job entered. Nobody awaits the body.
    EntryFrame* entryFrameOfFramesAbove = walk.isAmongFramesThatWait() ? nullptr : entryFrameOfLastRun;
    appendFramesBelow(vm, owner, otherFrames, walk, numberOfFramesAbove < maxStackSize, {
        .entryFrameOfFramesAbove = entryFrameOfFramesAbove,
        .numberOfFramesAbove = numberOfFramesAbove - framesAbove.numberShown,
    }, maxStackSize);
    trace->isFull = numberOfFramesAbove - framesAbove.numberShown + otherFrames.size() >= maxStackSize;

    if (validatesRuntime(vm)) [[unlikely]]
        validateCapturedTrace(vm, owner, *trace, otherFrames, maxStackSize, lastConstructorToSkip);
    return trace;
}

// The adapter that a run ends at keeps the instance that it entered.
static Instance* instanceAtEndOf(const RunWalk& walk)
{
    if (!walk.isAtEnd() || !walk.runEnd().adapterFrame)
        return nullptr;
    return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(walk.runEnd().adapterFrame) + AdapterFrame::offsetOfInstance);
}

// Compiled code has called the adapter that the run ends at, or an operation of compiled code has entered the VM for it, as for the class of another image that is constructed.
// No frame that is listed lies between, so the run that called is listed by its return addresses too.
static std::optional<RunStart> findRunThatCalled(VM& vm, const RunWalk& walk, EntryFrame* entryFrameOfRun, const Image*& imageOfCaller)
{
    const CompiledFrameExit& exit = walk.exit();
    if (!walk.runEnd().adapterFrame || walk.runEnd().resumesSavedFrame || walk.isAmongFramesThatWait() || !exit.callFrame)
        return std::nullopt;
    CallFrame* callee = exit.calleeFrame;
    if (exit.crossedEntryFrame) {
        // The frames of an async function that waits for this entry would come in between.
        if (!isOperationFrameRecord(vm, exit.callFrame) || (Options::useAsyncStackTrace() && JSC::Interpreter::asyncFunctionIn(vmEntryRecord(entryFrameOfRun)->m_context)))
            return std::nullopt;
        callee = exit.callFrame;
    }
    if (!callee)
        return std::nullopt;
    imageOfCaller = vm.aotState().image->findInListByCodeAddress(removeCodePtrTag(callee->rawReturnPC()));
    if (!imageOfCaller)
        return std::nullopt;
    return RunStart { stackPointerOfCaller(callee), callee->callerFrame(), callee->rawReturnPC(), exit.entryFrame, false, true };
}

// tryCaptureRun() from the run on, in a VM that runs several images: a call from one image into another ends a run, so most of the stack would lie below the first.
static NEVER_INLINE ReturnAddressTrace* tryCaptureRunsOfImages(VM& vm, JSCell* owner, size_t maxStackSize, WTF::Vector<StackFrame>& otherFrames, JSCell* lastConstructorToSkip, const Invocable<void*(size_t)> auto& allocate, const RunStart& start, const FramesAboveRun& framesAbove)
{
    VMState& state = vm.aotState();
    std::optional<RunWalk> walk;
    walk.emplace(state, start);
    if (walk->isAtEnd())
        return nullptr;
    CapturedRun run;
    captureRun<true>(vm, *walk, run, maxStackSize, lastConstructorToSkip, framesAbove, start.calleeWasEntered, FunctionTables::of(vm).framesMayBeNamedByCallee());
    Instance* instance = instanceAtEndOf(*walk);
    if (!instance)
        instance = findInstanceAmongSeveral(vm, start);
    if (!instance)
        return nullptr;
    Vector<uint32_t, 8> runs;
    runs.append(ReturnAddressTrace::encodeRun(run.returnAddresses.size(), walk->image().position()));
    EntryFrame* entryFrameOfLastRun = start.entryFrame;
    {
        // The tables of a run are asked while its image runs.
        const Image* imageOnTop = state.image;
        auto runImageOnTop = makeScopeExit([&] {
            if (state.image != imageOnTop)
                state.run(*imageOnTop);
        });
        while (!run.hasFrameNamedByCallee && !run.hasFrameListedBehindTailCall && walk->isAtEnd()) {
            const Image* imageOfCaller = nullptr;
            std::optional<RunStart> caller = findRunThatCalled(vm, *walk, entryFrameOfLastRun, imageOfCaller);
            if (!caller)
                break;
            if (state.image != imageOfCaller)
                state.run(*imageOfCaller);
            RunWalk walkOfCaller(state, *caller);
            // The way out of the run above says more than a walk that could not begin.
            if (walkOfCaller.isAtEnd() && !walkOfCaller.runEnd().adapterFrame)
                break;
            walk.emplace(walkOfCaller);
            entryFrameOfLastRun = caller->entryFrame;
            // A tail call has left the only function of the run, and its adapter is still there.
            if (walk->isAtEnd())
                continue;
            captureRun<true>(vm, *walk, run, maxStackSize, run.foundConstructorToSkip ? nullptr : lastConstructorToSkip, { run.numberShown, true }, true, FunctionTables::of(vm).framesMayBeNamedByCallee());
            runs.append(ReturnAddressTrace::encodeRun(run.returnAddresses.size(), walk->image().position()));
        }
    }
    return tryMakeTrace(vm, owner, maxStackSize, otherFrames, lastConstructorToSkip, allocate, instance, run, runs.span(), *walk, framesAbove, start.calleeWasEntered, entryFrameOfLastRun);
}

// `otherFrames` gets the frames on top of the run, then those below it. `allocate` gives the memory of the trace.
static ALWAYS_INLINE ReturnAddressTrace* tryCaptureRun(VM& vm, JSCell* owner, size_t maxStackSize, WTF::Vector<StackFrame>& otherFrames, JSCell* lastConstructorToSkip, const Invocable<void*(size_t)> auto& allocate)
{
    if (!vm.hasCompiledCode() || !vm.topCallFrame || !maxStackSize || vm.aotState().stackPointerOfImport)
        return nullptr;

    FramesAboveRun framesAbove;
    std::optional<RunStart> start = findRunAtTop(vm);
    if (!start) {
        if (lastConstructorToSkip)
            return nullptr;
        if (maxStackSize >= 2)
            start = findRunBelowHostFunction(vm);
        if (start) {
            if (!otherFrames.capacity())
                otherFrames.reserveInitialCapacity(1);
            otherFrames.append(StackFrame(vm, owner, vm.topCallFrame->callee().asCell()));
        } else
            start = findRunBelowOtherFrames(vm, owner, otherFrames, maxStackSize);
        if (!start)
            return nullptr;
        framesAbove = { otherFrames.size(), true };
    }
    // A return address does not say which function object runs in the frame.
    FunctionTables& tables = FunctionTables::of(vm);
    bool hasRecordedCallees = tables.framesMayBeNamedByCallee();
    Instance* instance = vm.aotState().onlyInstance;
    if (!instance) [[unlikely]] {
        if (vm.aotState().hasSeveralImages)
            return tryCaptureRunsOfImages(vm, owner, maxStackSize, otherFrames, lastConstructorToSkip, allocate, *start, framesAbove);
        instance = findInstanceAmongSeveral(vm, *start);
        if (!instance)
            return nullptr;
    }

    RunWalk walk(vm.aotState(), *start);
    if (walk.isAtEnd())
        return nullptr;
    CapturedRun run;
    captureRun<false>(vm, walk, run, maxStackSize, lastConstructorToSkip, framesAbove, start->calleeWasEntered, hasRecordedCallees);
    return tryMakeTrace(vm, owner, maxStackSize, otherFrames, lastConstructorToSkip, allocate, instance, run, { }, walk, framesAbove, start->calleeWasEntered, start->entryFrame);
}

ReturnAddressTrace* tryCaptureStackTrace(VM& vm, JSCell* owner, size_t maxStackSize, std::unique_ptr<WTF::Vector<StackFrame>>& otherFrames, JSCell* lastConstructorToSkip)
{
    std::optional<AssertNoGC> assertNoGC { std::in_place };
    WTF::Vector<StackFrame> results;
    auto* trace = tryCaptureRun(vm, owner, maxStackSize, results, lastConstructorToSkip, [&](size_t size) {
        assertNoGC.reset();
        void* memory = vm.auxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::ReturnNull);
        assertNoGC.emplace();
        return memory;
    });
    if (trace && !results.isEmpty()) {
        results.shrinkToFit();
        otherFrames = makeUnique<WTF::Vector<StackFrame>>(WTF::move(results));
    }
    return trace;
}

bool beginsAt(const ReturnAddressTrace& trace, const void* returnAddress)
{
    return trace.numberOfReturnAddresses && ReturnAddressTrace::decode(trace.returnAddresses()[0], imageCodeStart()) == returnAddress;
}

// Turns the machine frames of a run, from the innermost outwards, into the frames that are shown. The hidden frames of a function below its frame say whether it was constructed, so a frame is handed on once they have been seen.
// didShow(frame, logicalFrame) hears of a frame when it is found, emit(frame) gets it and says whether more are wanted.
template<typename DidShow, typename Emit>
class ShownFrameLister {
public:
    ShownFrameLister(const Image& image, InlineFrameReader& reader, ShownFrameFilter filter, unsigned numberToOmit, const DidShow& didShow, const Emit& emit)
        : m_image(image)
        , m_reader(reader)
        , m_filter(filter)
        , m_numberToOmit(numberToOmit)
        , m_didShow(didShow)
        , m_emit(emit)
    {
    }

    ShownFrameFilter filter() const { return m_filter; }
    unsigned numberToOmit() const { return m_numberToOmit; }
    TraceFrame* frameNotHandedOn() { return m_pending ? &*m_pending : nullptr; }

    // All three are false once emit() wants no more.
    ALWAYS_INLINE bool addMachineFrame(const CallSiteCache::Entry& entry, bool isAtFirstReadSite, bool isCaller)
    {
        const CallSiteRecord& record = entry.site;
        CallSiteIndex site = isAtFirstReadSite && record.firstReadSite ? *record.firstReadSite : record.callSite;
        bool isTailDeleted = record.isTailDeleted(isCaller);
        if (record.inlineFrame || record.calledBuiltin || isTailDeleted || entry.innermostVisibility != TraceVisibility::Visible || m_numberToOmit) [[unlikely]]
            return addLogicalFrames(entry.code, record, site, isTailDeleted, isCaller);
        // emit() may ask the cache, which may move the entry.
        TraceFrame frame { entry.code, entry.innermostFunction, site, false, false, false, false, isCaller, false, !isCaller && record.isTailCall() };
        if (!handOn())
            return false;
        m_filter = ShownFrameFilter(true);
        show(frame, { frame.function, site });
        return true;
    }

    ALWAYS_INLINE bool didFindCallerOfMachineFrame(bool isConstructCall)
    {
        if (!isConstructCall || !m_pending) [[likely]]
            return true;
        m_pending->isConstructCall = true;
        // A function that was constructed makes no tail calls, so its frame is still there and is named by the function object.
        m_pending->isAtFailedTailCall = false;
        return handOn();
    }

    // The caller of the machine frame has constructed the function, or, where nothing says which, the one that the machine frame was entered for. `frameIfGone` is what is listed for it where a tail call has taken its frame from the stack.
    NEVER_INLINE bool didFindCallerThatConstructed(FunctionID constructed, std::optional<TraceFrame> frameIfGone)
    {
        auto isOfConstructedFunction = [&](const std::optional<TraceFrame>& frame) {
            return frame && (!constructed || FunctionDescription(frame->function).sourceFunction() == sourceFunctionOf(constructed));
        };
        if (isOfConstructedFunction(m_pending))
            return didFindCallerOfMachineFrame(true);
        if (!handOn())
            return false;
        std::optional<TraceFrame> frame = isOfConstructedFunction(m_hiddenBehindTailCall) ? m_hiddenBehindTailCall : frameIfGone;
        if (!frame)
            return true;
        frame->isConstructCall = true;
        m_filter = ShownFrameFilter(true);
        LogicalFrame logicalFrame { frame->function, frame->callSite };
        logicalFrame.isListedBehindTailCall = true;
        show(*frame, logicalFrame);
        return handOn();
    }

    ALWAYS_INLINE bool handOn()
    {
        if (!m_pending)
            return true;
        TraceFrame frame = *std::exchange(m_pending, std::nullopt);
        return m_emit(frame);
    }

private:
    ALWAYS_INLINE void show(const TraceFrame& frame, const LogicalFrame& logicalFrame)
    {
        m_pending = frame;
        m_pendingSource = { };
        m_hiddenBehindTailCall = std::nullopt;
        m_didShow(*m_pending, logicalFrame);
    }

    bool isEntrypointOfPending(const FunctionDescription& description)
    {
        if (description.visibility() == TraceVisibility::Visible)
            return false;
        if (!m_pendingSource)
            m_pendingSource = FunctionDescription(m_pending->function).sourceFunction();
        return description.sourceFunction() == m_pendingSource;
    }

    NEVER_INLINE bool addLogicalFrames(CompiledFunctionID code, CallSiteRecord record, CallSiteIndex site, bool isTailDeleted, bool isCaller)
    {
        // A built-in that leaves by a tail call is listed only where that call failed.
        bool isAtFailedTailCall = !isCaller && (record.calledBuiltin ? leavesByTailCall(record.calledBuiltin) : record.isTailCall());
        for (LogicalFrame frame = innermostLogicalFrame(m_image, m_reader, { code, site, record.inlineFrame, isTailDeleted, record.calledBuiltin, isCaller });; frame = callerOfInlinedFrame(m_image, m_reader, code, frame), isCaller = true, isAtFailedTailCall = false) {
            FunctionDescription description(frame.function);
            if (m_pending && !isEntrypointOfPending(description) && !handOn())
                return false;

            if (m_filter.shows(description.visibility(), frame.isTailDeleted)) {
                if (m_numberToOmit)
                    --m_numberToOmit;
                else {
                    if (!handOn())
                        return false;
                    show({ code, frame.function, frame.callSite, !!frame.inlineFrame, false, false, false, isCaller, false, isAtFailedTailCall }, frame);
                }
            } else if (frame.isTailDeleted && !frame.isOfCalledBuiltin && description.visibility() == TraceVisibility::Visible)
                m_hiddenBehindTailCall = TraceFrame { code, frame.function, frame.callSite, !!frame.inlineFrame, false, false, false, isCaller, false };
            else if (m_hiddenBehindTailCall && !isEntrypointOf(frame.function, FunctionDescription(m_hiddenBehindTailCall->function).sourceFunction()))
                m_hiddenBehindTailCall = std::nullopt;

            if (m_pending && !frame.isOfCalledBuiltin && frame.inlineFrame && frame.isInlinedConstructCall) {
                m_pending->isConstructCall = true;
                m_pending->isAtFailedTailCall = false;
                if (!handOn())
                    return false;
            }
            if (!frame.hasCallerInMachineFrame())
                return true;
        }
    }

    const Image& m_image;
    InlineFrameReader& m_reader;
    ShownFrameFilter m_filter;
    unsigned m_numberToOmit;
    std::optional<TraceFrame> m_pending;
    FunctionID m_pendingSource;
    // The last frame that a tail call of its function hides, while only entrypoints of the function have come since.
    std::optional<TraceFrame> m_hiddenBehindTailCall;
    const DidShow& m_didShow;
    const Emit& m_emit;
};

// The frames of the addresses from `begin` to `end`, which are of the image that runs. False: no more frames are wanted.
static ALWAYS_INLINE bool showRunOf(const ReturnAddressTrace& trace, size_t begin, size_t end, ShownFrameFilter filter, unsigned& numberToOmit, const auto& emit)
{
    const Image& image = *Image::current();
    CallSiteCache& cache = *trace.instance->vm->aotState().callSiteCache;
    uintptr_t codeStart = imageCodeStart();
    auto didShow = [&](TraceFrame& frame, const LogicalFrame& logicalFrame) ALWAYS_INLINE_LAMBDA {
        frame.calleeLiesInFrame = isCopiedForOwnFunctionObject(image, cache.inlineFrames(), frame.code, logicalFrame);
    };
    ShownFrameLister lister(image, cache.inlineFrames(), filter, numberToOmit, didShow, emit);

    auto returnAddresses = trace.returnAddresses();
    for (size_t i = begin; i < end; ++i) {
        auto flags = trace.flagsAt(i);
        const CallSiteCache::Entry* entry = cache.resolve(image, ReturnAddressTrace::decode(returnAddresses[i], codeStart));
        RELEASE_ASSERT(entry && !entry->isStubReturnSite());
        if (flags.contains(ReturnAddressTrace::Flag::IsAtFirstReadSite)) [[unlikely]]
            cache.withMethodReadSite(image, entry);
        if (!lister.addMachineFrame(*entry, flags.contains(ReturnAddressTrace::Flag::IsAtFirstReadSite), i || trace.calleeOfFirstFrameWasEntered))
            return false;
        if (!lister.didFindCallerOfMachineFrame(flags.contains(ReturnAddressTrace::Flag::IsConstructCall)))
            return false;
    }
    bool wantsMore = lister.handOn();
    numberToOmit = lister.numberToOmit();
    return wantsMore;
}

void forEachShownFrame(const ReturnAddressTrace& trace, const ScopedLambda<void(const TraceFrame&)>& functor)
{
    size_t numberShown = trace.numberOfFramesOnTop;
    if (numberShown >= trace.limit)
        return;
    auto emit = [&](const TraceFrame& frame) {
        functor(frame);
        return ++numberShown < trace.limit;
    };
    ShownFrameFilter filter(trace.isBelowOtherFrames || trace.omittedFrameCount);
    unsigned numberToOmit = trace.omittedFrameCount;
    // One image runs for good where none shares its realms.
    if (!g_imagesShareRealms) [[likely]] {
        showRunOf(trace, 0, trace.numberOfReturnAddresses, filter, numberToOmit, emit);
        return;
    }
    VMState& state = trace.instance->vm->aotState();
    if (!state.hasSeveralImages) {
        RunningImageScope imageOfTrace(state, trace.instance->program);
        showRunOf(trace, 0, trace.numberOfReturnAddresses, filter, numberToOmit, emit);
        return;
    }
    size_t begin = 0;
    for (uint32_t run : trace.runs()) {
        size_t end = ReturnAddressTrace::endOfRun(run);
        RunningImageScope imageOfRun(state, state.parts[ReturnAddressTrace::imagePositionOfRun(run)].image);
        if (!showRunOf(trace, begin, end, filter, numberToOmit, emit))
            return;
        // As captureRun() begins a run that lies below another.
        filter = ShownFrameFilter(true);
        begin = end;
    }
}

static std::optional<JSObject*> findRecordedCallee(VM& vm, const RunStart& machineFrame, unsigned inlineFrame)
{
    auto frames = CompiledFrameIterator::tryCreate(vm, machineFrame.framePointer, removeCodePtrTag(machineFrame.rawPC), machineFrame.entryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, machineFrame.stackPointer);
    RELEASE_ASSERT(frames);
    while (frames->frame().inlineFrame != inlineFrame)
        frames->advance();
    return frames->recordedCallee();
}

struct ShownFrameOfRun {
    TraceFrame frame;
    // CompiledFrameIterator::recordedCallee(), if it is on the heap.
    JSObject* callee { nullptr };
    CallFrame* machineFrame { nullptr };
    // CompiledFrameIterator::isAboveFramesThatWait(), of a frame that is not inlined.
    bool isAboveFramesThatWait { false };
};

// Hands the frames that are shown of the run to the functor. False: the functor wanted no more.
static ALWAYS_INLINE bool forEachShownFrameOfRun(VM& vm, RunWalk& walk, bool calleeWasEntered, ShownFrameFilter& filter, const Invocable<IterationStatus(const ShownFrameOfRun&)> auto& functor)
{
    ASSERT(!walk.isAtEnd());
    FunctionTables& tables = FunctionTables::of(vm);
    bool namesFramesByCallee = tables.framesMayBeNamedByCallee();

    InlineFrameReader& reader = vm.aotState().callSiteCache->inlineFrames();

    // Of the one frame that the lister has not handed on.
    ShownFrameOfRun shown;
    bool isOutermostOfMachineFrame = false;
    bool knowsCallee = true;
    unsigned inlineFrame = 0;
    RunStart position { };
    void* callerStackPointer = nullptr;

    auto setCallee = [&](TraceFrame& frame, std::optional<JSObject*> callee) {
        shown.callee = callee.value_or(nullptr);
        frame.calleeLiesInFrame = callee && !*callee;
        knowsCallee = true;
    };
    auto didShow = [&](TraceFrame& frame, const LogicalFrame& logicalFrame) ALWAYS_INLINE_LAMBDA {
        shown.machineFrame = walk.machineFrame();
        shown.callee = nullptr;
        shown.isAboveFramesThatWait = false;
        isOutermostOfMachineFrame = !logicalFrame.hasCallerInMachineFrame();
        if (!namesFramesByCallee || logicalFrame.isOfCalledBuiltin || logicalFrame.isListedBehindTailCall || frame.isAtFailedTailCall) [[likely]]
            return;
        FunctionID sourceFunction = FunctionDescription(frame.function).sourceFunction();
        if (std::optional<CalleeOfSite> callee = calleeOfCopiedFrame(walk.image(), reader, frame.code, logicalFrame)) {
            setCallee(frame, callee->isOwnFunctionObject() ? nullptr : tables.calleeKeptInFrame(vm, walk.machineFrame(), *callee, sourceFunction));
            return;
        }
        frame.calleeLiesInFrame = false;
        // One marked function in the program brings every frame here, and most frames are of functions that are named as they are written.
        if (!tables.framesAreNamedByCallee(sourceFunction))
            return;
        if (!hasOnlyEntrypointsBelow(walk.image(), reader, frame.code, logicalFrame, sourceFunction))
            return;
        knowsCallee = false;
        inlineFrame = logicalFrame.inlineFrame;
        position = walk.position();
        callerStackPointer = walk.callerStackPointer();
    };
    auto emit = [&](TraceFrame& frame) ALWAYS_INLINE_LAMBDA {
        if (!knowsCallee) [[unlikely]] {
            setCallee(frame, findRecordedCallee(vm, position, inlineFrame));
            walk.findSiteAgain();
        }
        shown.frame = frame;
        return functor(shown) == IterationStatus::Continue;
    };
    ShownFrameLister lister(walk.image(), reader, filter, 0, didShow, emit);

    CompiledFunctionID outermostCode;
    // As in captureRun().
    CompiledFunctionID codeAbove;
    auto didFindConstructionOf = [&](FunctionID constructed) {
        std::optional<TraceFrame> frameIfGone;
        if (constructed && (!codeAbove || !isCodeOf(walk.image(), codeAbove, constructed)))
            frameIfGone = frameOfConstructedFunctionThatLeft(walk.image(), constructed, codeAbove);
        return lister.didFindCallerThatConstructed(constructed, frameIfGone);
    };
    auto didFindConstructSite = [&]() NEVER_INLINE {
        return didFindConstructionOf(walk.constructedFunction());
    };
    // What was constructed has left by a tail call of something that is no compiled function.
    if (walk.hasLeftPlainFunctionConstructFrame() && walk.site().site.isConstruct() && !didFindConstructSite()) [[unlikely]]
        return false;
    for (bool isCaller = calleeWasEntered;; isCaller = true) {
        outermostCode = walk.site().code;
        if (!codeAbove || !walk.site().isOfEntrypointAtTailCall())
            codeAbove = outermostCode;
        isOutermostOfMachineFrame = false;
        if (!lister.addMachineFrame(walk.site(), walk.isAtFirstReadSite(), isCaller))
            return false;
        walk.advance();
        shown.isAboveFramesThatWait = isOutermostOfMachineFrame && walk.isAmongFramesThatWait();
        if (walk.isAtEnd())
            break;
        const CallSiteCache::Entry& caller = walk.site();
        if (!knowsCallee && isOutermostOfMachineFrame) [[unlikely]] {
            // Whatever else stands there is left to CompiledFrameIterator::recordedCallee().
            TraceFrame& frame = *lister.frameNotHandedOn();
            if (caller.hasRecordedCallee) {
                CalleeOfSite callee = *walk.image().calleeOfSiteAt(caller.code.index(), caller.site.callSite);
                setCallee(frame, callee.isOwnFunctionObject() ? nullptr : tables.calleeKeptInFrame(vm, walk.machineFrame(), callee, FunctionDescription(frame.function).sourceFunction()));
            } else if (caller.innermostVisibility == TraceVisibility::Visible && !caller.site.isTailCall())
                setCallee(frame, tables.recordedCalleeOfCallFrom(callerStackPointer, FunctionDescription(frame.function).sourceFunction()));
        }
        if (caller.site.isConstruct() && !didFindConstructSite())
            return false;
    }
    if (FunctionID constructed = walk.runEnd().constructedFunction()) {
        if (!didFindConstructionOf(constructed))
            return false;
    } else if (!lister.didFindCallerOfMachineFrame(walk.runEnd().constructs(walk.image().rootFunctionOf(outermostCode.index()))))
        return false;
    if (!lister.handOn())
        return false;
    filter = lister.filter();
    return true;
}

static ALWAYS_INLINE bool appendShownFramesOfRun(VM& vm, RunWalk& walk, bool calleeWasEntered, ShownFrameFilter& filter, WTF::Vector<StackFrame>& results, RunListing& listing)
{
    void* stackPointerOfImport = vm.aotState().stackPointerOfImport;
    return forEachShownFrameOfRun(vm, walk, calleeWasEntered, filter, [&](const ShownFrameOfRun& shown) ALWAYS_INLINE_LAMBDA {
        if (results.size() >= listing.maxResultsSize)
            return IterationStatus::Done;
        if (listing.framesToSkip) {
            --listing.framesToSkip;
            return IterationStatus::Continue;
        }
        if (listing.caller) {
            if (isClosureOf(listing.caller, shown.frame.function))
                listing.caller = nullptr;
            return IterationStatus::Continue;
        }
        listing.hasReachedFrame = true;
        if (stackPointerOfImport && shown.machineFrame >= stackPointerOfImport) [[unlikely]]
            return IterationStatus::Done;
        listing.hasListedFrame = true;

        if (shown.callee) [[unlikely]]
            results.constructAndAppend(vm, listing.owner, shown.frame, shown.callee);
        else
            results.constructAndAppend(shown.frame);
        if (listing.filter && !(*listing.filter)(results.last()))
            results.removeLast();
        if (shown.isAboveFramesThatWait && FunctionDescription(shown.frame.function).isAsync())
            listing.isAboveFramesThatWait = true;
        return results.size() < listing.maxResultsSize ? IterationStatus::Continue : IterationStatus::Done;
    });
}

IterationStatus appendShownFramesOfRun(VM& vm, CompiledFrameIterator& frames, ShownFrameFilter& filter, WTF::Vector<StackFrame>& results, RunListing& listing)
{
    if (frames.isAtEnd() || frames.isAmongFramesThatWait())
        return IterationStatus::Continue;
    RunWalk walk(vm.aotState(), { frames.stackPointer(), frames.framePointer(), const_cast<void*>(frames.callSite().address), frames.entryFrame(), frames.isAtFirstReadSite(), frames.calleeWasEntered(), frames.plainFunctionConstructFrameLeft() });
    if (!appendShownFramesOfRun(vm, walk, frames.calleeWasEntered(), filter, results, listing))
        return IterationStatus::Done;
    frames.leaveRunAt(walk.exit(), walk.runEnd());
    return IterationStatus::Continue;
}

bool tryGetStackTrace(VM& vm, JSCell* owner, WTF::Vector<StackFrame>& results, size_t framesToSkip, size_t maxStackSize, const ScopedLambda<bool(const StackFrame&)>* wanted)
{
    AssertNoGC assertNoGC;
    if (!vm.hasCompiledCode() || !vm.topCallFrame || !maxStackSize || vm.aotState().stackPointerOfImport)
        return false;
    RunListing listing { owner, maxStackSize, framesToSkip, nullptr, wanted };
    std::optional<RunStart> start = findRunAtTop(vm);
    bool isBelowHostFunction = !start;
    if (isBelowHostFunction) {
        start = findRunBelowHostFunction(vm);
        if (!start)
            return false;
    }
    RunWalk walk(vm.aotState(), *start);
    if (walk.isAtEnd())
        return false;

    if (!results.capacity())
        results.reserveInitialCapacity(std::min(maxStackSize, CapturedRun::inlineCapacity / 2));
    bool hasListedFrame = false;
    if (isBelowHostFunction) {
        if (listing.framesToSkip)
            --listing.framesToSkip;
        else {
            hasListedFrame = true;
            results.constructAndAppend(vm, owner, vm.topCallFrame->callee().asCell());
            if (wanted && !(*wanted)(results.last()))
                results.removeLast();
        }
    }
    ShownFrameFilter filter(isBelowHostFunction);
    bool hasListedAll = results.size() < maxStackSize && appendShownFramesOfRun(vm, walk, start->calleeWasEntered, filter, results, listing);
    hasListedFrame |= listing.hasListedFrame;

    EntryFrame* entryFrameOfFramesAbove = hasListedFrame && !listing.isAboveFramesThatWait ? start->entryFrame : nullptr;
    appendFramesBelow(vm, owner, results, walk, hasListedAll, {
        .entryFrameOfFramesAbove = entryFrameOfFramesAbove,
        .framesToSkip = listing.framesToSkip,
        .filter = wanted,
    }, maxStackSize);
    return true;
}

void forEachShownFrame(CompiledFunctionID code, const CallSiteRecord& site, ShownFrameFilter& filter, const ScopedLambda<void(const TraceFrame&)>& functor, bool calleeWasEntered)
{
    const Image& image = *Image::current();
    InlineFrameReader reader;
    for (LogicalFrame frame = innermostLogicalFrame(image, reader, { code, site.callSite, site.inlineFrame, site.isTailDeleted(calleeWasEntered) });; frame = callerOfInlinedFrame(image, reader, code, frame)) {
        if (filter.shows(FunctionDescription(frame.function).visibility(), frame.isTailDeleted))
            functor(TraceFrame { code, frame.function, frame.callSite, !!frame.inlineFrame, frame.isInlinedConstructCall, false, false, false, isCopiedForOwnFunctionObject(image, reader, code, frame) });
        if (!frame.hasCallerInMachineFrame())
            break;
    }
}

// Bytecode enters the constructor and throws there, so the stack has a frame of the class at the start of its parameters.
std::optional<TraceFrame> tryGetFrameOfClassCalledWithoutNew(VM& vm, CallFrame* callFrame)
{
    if (vm.aotState().classCalledWithoutNew != callFrame) [[likely]]
        return std::nullopt;
    Closure* classObject = uncheckedDowncast<Closure>(callFrame->jsCallee());
    FunctionID function = classObject->function();
    if (FunctionDescription(function).visibility() == TraceVisibility::Hidden)
        return std::nullopt;
    const Image& image = *Image::current();
    for (FunctionID candidate : { function, sourceFunctionOf(function) }) {
        if (auto code = image.imageFunctionOf(candidate))
            return TraceFrame { CompiledFunctionID(*code), function, CallSiteIndex(0), false, false, true };
    }
    // Where every construction is compiled at its place, the constructor has code only as what a construction by value enters.
    CodeLocation entered = image.locate(std::bit_cast<const void*>(ClosureWord::addressOf(classObject->entry(), std::bit_cast<uintptr_t>(image.code().data()))));
    if (entered.kind == CodeKind::Function)
        return TraceFrame { CompiledFunctionID(entered.function), function, CallSiteIndex(0), false, false, true };
    return std::nullopt;
}

std::optional<SourcePosition> definitionOf(const TraceFrame& frame)
{
    if (std::optional<SourcePosition> definition = FunctionDescription(frame.function).definition())
        return definition;
    // A function that has lost parameters is left as a jump to the version without them, or has no code at all. The version is what runs, and it begins where the function does.
    if (!frame.code || frame.isInlined)
        return std::nullopt;
    return Image::current()->positionOf(frame.code.index(), CallSiteIndex(0));
}

std::optional<SourcePosition> positionOf(const TraceFrame& frame)
{
    ImageOfTraceFrameScope imageOfFrame(frame);
    const Image& image = *Image::current();
    if (image.isObfuscated())
        return SourcePosition { obfuscatedPositionOf(frame), ModuleID() };
    if (builtinOfFunctionInFrames(frame.function) && FunctionDescription(frame.function).builtinKind() == StackTraceBuiltinKind::JSBuiltin) [[unlikely]]
        return SourcePosition { startOfBuiltinFunction, ModuleID() };
    std::optional<SourcePosition> position = image.positionOf(frame.code.index(), frame.callSite);
    if (position && position->startIsForCallers) {
        if (frame.isCaller)
            position->lineColumn = position->constructionStart;
        position->constructionStart = { 0, 0 };
        position->startIsForCallers = false;
    }
    if (image.sharesRealms() && position && position->source) [[unlikely]]
        position->source = moduleIDForHost(image, position->source);
    return position;
}

LineColumn obfuscatedPositionOf(const TraceFrame& frame)
{
    return { frame.code.index() + 1, frame.callSite.bits() + 1 };
}

String sourceURLOf(ModuleID module)
{
    return module ? modulePath(module) : emptyString();
}

String sourceProviderURLOf(ModuleID module)
{
    return module ? sourceProviderURL(module) : emptyString();
}

String describeCodeAddresses(std::span<const void* const> addresses)
{
    const Image* imageOfThread = Image::current();
    bool imagesDiffer = g_imagesShareRealms;
    if (!imageOfThread && !imagesDiffer)
        return { };
    // The frames may be of several images. What describes a frame asks the word of this thread, which in a process that is crashing may name any of them, or none.
    auto restoreImageOfThread = makeScopeExit([&] {
        if (imagesDiffer)
            Image::setCurrentOfThread(imageOfThread);
    });
    StringBuilder result;
    ShownFrameFilter filter;
    for (const void* const& address : addresses) {
        const Image* image = imagesDiffer ? Image::findByCodeAddress(address) : imageOfThread;
        if (!image)
            continue;
        if (imagesDiffer)
            Image::setCurrentOfThread(image);
        CodeLocation location = image->locate(address);
        if (location.kind != CodeKind::Function)
            continue;
        CallSiteRecord site = image->callSiteAt(location.function, location.offset).value_or(CallSiteRecord { CallSiteIndex(0), 0 });
        // Every address but the first is a return address, so its callee has started. A tail call that was compiled as a plain call hides its caller, as in Error.stack.
        bool calleeWasEntered = &address != addresses.data();
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
        }, calleeWasEntered);
    }
    return result.toString();
}

struct SuspendedAsyncFunction {
    SavedFrame* coroutine { nullptr };
    JSPromise* promise { nullptr };
    SavedFrame* waiter { nullptr };
};

static ALWAYS_INLINE SavedFrame* tryGetSavedFrame(JSCell* cell)
{
    return cell->type() == ObjectType && cell->classInfo() == SavedFrame::info() ? uncheckedDowncast<SavedFrame>(cell) : nullptr;
}

static ALWAYS_INLINE SuspendedAsyncFunction describeSuspendedAsyncFunction(SavedFrame* saved)
{
    if (saved->kind() != CoroutineKind::AsyncFunction)
        return { };
    switch (saved->continuation()) {
    case SavedFrame::Continuation::None:
        return { saved, nullptr, nullptr };
    case SavedFrame::Continuation::Promise:
        return { saved, uncheckedDowncast<JSPromise>(saved->promiseOrWaiter()), nullptr };
    case SavedFrame::Continuation::Waiter:
        return { saved, nullptr, uncheckedDowncast<SavedFrame>(saved->promiseOrWaiter()) };
    case SavedFrame::Continuation::CombinatorElement:
        return { saved, saved->promiseOfCombinator(), nullptr };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE SuspendedAsyncFunction findSuspendedAsyncFunction(JSCell* cell)
{
    SavedFrame* saved = tryGetSavedFrame(cell);
    return saved ? describeSuspendedAsyncFunction(saved) : SuspendedAsyncFunction { };
}

static NEVER_INLINE void appendInlinedFrames(const CallSiteCache::Entry& entry, WTF::Vector<StackFrame>& results)
{
    ShownFrameFilter filter(true);
    forEachShownFrame(entry.code, entry.site, filter, [&](const TraceFrame& frame) {
        // A function that is not async cannot wait: it has handed on the promise of its callee and returned. Only its code was taken into the function that waits.
        if (FunctionDescription(frame.function).isAsync())
            results.constructAndAppend(frame, true);
    });
}

static ALWAYS_INLINE void appendFramesOfRunningImage(CallSiteCache&, const SuspendedAsyncFunction&, WTF::Vector<StackFrame>& results);

static ALWAYS_INLINE void appendFrames(CallSiteCache& cache, const SuspendedAsyncFunction& suspended, WTF::Vector<StackFrame>& results)
{
    if (!suspended.coroutine)
        return;
    if (VMState& state = suspended.coroutine->vm().aotState(); state.hasSeveralImages) [[unlikely]] {
        // A function that waits may be of any image of the realm.
        RunningImageScope imageOfCoroutine(state, Instance::owning(suspended.coroutine)->program);
        appendFramesOfRunningImage(*state.callSiteCache, suspended, results);
        return;
    }
    appendFramesOfRunningImage(cache, suspended, results);
}

static ALWAYS_INLINE void appendFramesOfRunningImage(CallSiteCache& cache, const SuspendedAsyncFunction& suspended, WTF::Vector<StackFrame>& results)
{
    // The function goes on behind the call that suspended it, so its resume address lies in it.
    const CallSiteCache::Entry* entry = cache.find(suspended.coroutine->resumeAddress());
    if (!entry) [[unlikely]]
        entry = cache.resolve(*Image::current(), suspended.coroutine->resumeAddress());
    if (!entry || entry->isStubReturnSite() || entry->isInModuleBody)
        return;
    if (entry->site.inlineFrame) [[unlikely]] {
        appendInlinedFrames(*entry, results);
        return;
    }
    if (entry->innermostVisibility == TraceVisibility::Visible && entry->innermostIsAsync)
        results.constructAndAppend(TraceFrame { entry->code, entry->innermostFunction, entry->site.callSite, false, false }, true);
}

static ALWAYS_INLINE bool isModuleBody(VM& vm, const SavedFrame& saved)
{
    ImageOfCellScope imageOfCoroutine(vm, &saved);
    const CallSiteCache::Entry* entry = vm.aotState().callSiteCache->resolve(*Image::current(), saved.resumeAddress());
    return entry && entry->isInModuleBody;
}

bool isSuspendedAsyncFunction(JSCell* cell)
{
    SavedFrame* saved = tryGetSavedFrame(cell);
    return saved && saved->kind() == CoroutineKind::AsyncFunction && !isModuleBody(cell->vm(), *saved);
}

bool isSuspendedModuleBody(VM& vm, JSValue coroutine)
{
    SavedFrame* saved = coroutine.isCell() ? tryGetSavedFrame(coroutine.asCell()) : nullptr;
    return saved && isModuleBody(vm, *saved);
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
    appendFrames(*vm.aotState().callSiteCache, findSuspendedAsyncFunction(coroutine), results);
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
    CallSiteCache& cache = *vm.aotState().callSiteCache;
    for (;;) {
        SuspendedAsyncFunction caller;
        if (current.waiter) {
            if (results.size() >= maxStackSize)
                return nullptr;
            caller = describeSuspendedAsyncFunction(current.waiter);
        } else {
            if (!current.promise)
                return nullptr;
            uint16_t flags = current.promise->flags();
            bool isAwaitedByCompiledCode = (flags & mask) == isAwaitedWith(InternalMicrotask::CompiledAsyncFunctionResume) || (flags & mask) == isAwaitedWith(InternalMicrotask::CompiledModuleBodyResume);
            if (!isAwaitedByCompiledCode || results.size() >= maxStackSize) {
                bool hasNoAwaiters = (flags & JSPromise::stateMask) != static_cast<uint16_t>(JSPromise::Status::Pending) || (!(flags & JSPromise::inlineReactionKindMask) && !current.promise->payloadCell());
                return hasNoAwaiters ? nullptr : current.coroutine;
            }
            JSValue context = current.promise->inlineReactionContext();
            if (!context || !context.isCell())
                return current.coroutine;
            caller = findSuspendedAsyncFunction(context.asCell());
        }
        if (!caller.coroutine)
            return current.coroutine;
        current = caller;
        appendFrames(cache, current, results);
        if (results.size() > maxStackSize)
            results.shrink(maxStackSize);
    }
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
