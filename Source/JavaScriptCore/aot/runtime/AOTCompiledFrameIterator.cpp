/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiledFrame.h"

#if ENABLE(AOT)

#include "AOTCallSiteCache.h"
#include "AOTCompiledCallee.h"
#include "AOTCoroutineWords.h"
#include "AOTFaultTable.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTGenerator.h"
#include "AOTImageFormat.h"
#include "AOTInstance.h"
#include "AOTLoadedObject.h"
#include "AOTModules.h"
#include "AOTOperationData.h"
#include "AOTProgramTables.h"
#include "AOTSavedFrame.h"
#include "AOTShownFrameFilter.h"
#include "AOTStackTrace.h"
#include "AOTStubFrames.h"
#include "CallFrameInlines.h"
#include "JSCJSValueInlines.h"
#include "JSCellInlines.h"
#include "SourceOrigin.h"
#include "VM.h"
#include "VMEntryRecord.h"
#include <bit>
#include <utility>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool entersCallee(StubFrameKind kind)
{
    return kind == StubFrameKind::HostCall || kind == StubFrameKind::EngineCallee;
}

// Nothing for an address that runs no compiled code. Only for the thread that runs the VM: the sampling profiler asks the image.
static ALWAYS_INLINE const CallSiteCache::Entry* findReturnSite(CallSiteCache& cache, const Image& image, const void* pc)
{
    if (const CallSiteCache::Entry* site = cache.find(pc)) [[likely]]
        return site;
    return cache.resolve(image, pc);
}

template<typename Cursor>
static ALWAYS_INLINE const CallSiteCache::Entry* findReturnSiteInRun(CallSiteCache& cache, const Image& image, const Cursor& cursor)
{
    const void* pc = removeCodePtrTag(cursor.rawPC);
    if (const CallSiteCache::Entry* site = cache.find(pc)) [[likely]]
        return site;
    return resolveReturnAddressInRun(cache, image, pc, cursor.calleeFrame);
}

static ALWAYS_INLINE const CallSiteCache::Entry* findReturnSite(VM& vm, const void* pc)
{
    const VMState& state = vm.aotState();
    return findReturnSite(*state.callSiteCache, *state.image, pc);
}

// The frame at such a site is read as a frame of the engine.
static bool isInHostCallFrame(const CallSiteCache::Entry& site)
{
    return site.isStubReturnSite() && site.stubFrameKind == StubFrameKind::InHostCallFrame;
}

static bool isOperationReturnSite(const CallSiteCache::Entry& site)
{
    return !site.isStubReturnSite() || !entersCallee(site.stubFrameKind);
}

static bool isOperationReturnSite(const CodeLocation& location)
{
    return location.kind == CodeKind::Function || (location.kind == CodeKind::Stub && !entersCallee(location.stubFrameKind));
}

static bool callEndsAt(std::span<const uint8_t> code, const void* address)
{
    size_t offset = static_cast<const uint8_t*>(address) - code.data();
#if CPU(X86_64)
    constexpr uint8_t opcodeOfNearCall = 0xe8;
    constexpr uint8_t opcodeOfGroup5 = 0xff;
    if (offset >= sizeOfNearCall && code[offset - sizeOfNearCall] == opcodeOfNearCall)
        return true;
    for (size_t length : { 2, 3, 4, 6, 7 }) {
        if (offset >= length && code[offset - length] == opcodeOfGroup5 && (code[offset - length + 1] >> 3 & 7) == 2)
            return true;
    }
    return false;
#elif CPU(ARM64)
    if (offset < sizeof(uint32_t) || offset % sizeof(uint32_t))
        return false;
    uint32_t instruction;
    memcpy(&instruction, code.data() + offset - sizeof(uint32_t), sizeof(uint32_t));
    bool isBL = (instruction & 0xfc000000) == 0x94000000;
    bool isBLR = (instruction & 0xfffffc1f) == 0xd63f0000;
    return isBL || isBLR;
#else
#error "Not implemented on this CPU"
#endif
}

#if defined(BUN_HARDENED)

static bool isInCodeOfEngine(const void* address)
{
    struct Segments {
        std::array<std::pair<uintptr_t, uintptr_t>, 4> startAndSize { };
    };
    static const Segments segments = [] {
        Segments result;
        size_t count = 0;
        const LoadedObject& executable = loadedExecutable();
        for (const ProgramHeader& header : executable.programHeaders) {
            if (header.p_type == PT_LOAD && (header.p_flags & PF_X) && count < result.startAndSize.size())
                result.startAndSize[count++] = { executable.loadBias + header.p_vaddr, header.p_memsz };
        }
        return result;
    }();
    return std::ranges::any_of(segments.startAndSize, [&](auto segment) {
        return std::bit_cast<uintptr_t>(address) - segment.first < segment.second;
    });
}

// The fault handler enters a stub as if the instruction that faulted had called it.
static bool isBehindFaultSite(const Image& image, const void* address)
{
    auto hasSite = [&](FaultSiteKind kind, size_t size) {
        std::optional<FaultSite> site = image.faultSiteAt(static_cast<const uint8_t*>(address) - size);
        return site && site->kind == kind;
    };
    return hasSite(FaultSiteKind::NullCheck, sizeOfNearCall) || (hasFaultingLoopPolls && hasSite(FaultSiteKind::LoopPoll, sizeOfLoopPoll));
}

ASCIILiteral faultOfReturnAddressInRun(const Image& image, const void* returnAddress)
{
    CodeLocation location = image.locate(returnAddress);
    switch (location.kind) {
    case CodeKind::Stub:
        if (location.stubFrameKind != StubFrameKind::Plain)
            return { };
        [[fallthrough]];
    case CodeKind::Function:
        if (callEndsAt(image.code(), returnAddress) || isBehindFaultSite(image, returnAddress))
            return { };
        return "points into compiled code, at a place that no call returns to"_s;
    case CodeKind::NotInImage:
        if (isInCodeOfEngine(returnAddress))
            return { };
        [[fallthrough]];
    case CodeKind::RegExp:
    case CodeKind::WebAssembly:
    case CodeKind::FFI:
        return "points neither into a compiled function nor into the code of the engine"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NEVER_INLINE void checkReturnAddressInRun(const Image& image, const void* returnAddress, const CallFrame* callee)
{
    ASCIILiteral fault = faultOfReturnAddressInRun(image, returnAddress);
    if (fault.isNull()) [[likely]]
        return;
    dataLogLn("AOT: a walk over the stack has lost it: the word ", RawPointer(returnAddress), ", which lies where the frame at ", RawPointer(callee), " has its return address, ", fault);
    CRASH();
}

NEVER_INLINE const CallSiteCache::Entry* resolveReturnAddressInRun(CallSiteCache& cache, const Image& image, const void* returnAddress, const CallFrame* callee)
{
    checkReturnAddressInRun(image, returnAddress, callee);
    return cache.resolve(image, returnAddress);
}

#endif // defined(BUN_HARDENED)

static bool hasZombieCallee(VM& vm, const CallFrame* callFrame, const void* returnAddress)
{
    CallFrame* callee = vm.topCallFrame;
    return callee && callee->callerFrameOrEntryFrame() == callFrame && removeCodePtrTag(callee->rawReturnPC()) == returnAddress && callee->isZombieFrame();
}

SUPPRESS_ASAN Instance* CompiledFrameIterator::findEnclosingAdapterInstance(Cursor cursor) const
{
    Exit exit;
    do {
        const CallSiteCache::Entry* site = findReturnSiteInRun(*m_cache, *m_image, cursor);
        if (!site)
            return nullptr;
        enterFrame(cursor, *site);
        if (site->isStubReturnSite() && isAdapterFrame(site->stubFrameKind))
            return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(cursor.callFrame) + AdapterFrame::offsetOfInstance);
    } while (passOverFrame(cursor, exit));
    return nullptr;
}

Instance* CompiledFrameIterator::tryFindInstance() const
{
    const VMState& state = m_vm->aotState();
    if (state.instances.size() != 1)
        return findEnclosingAdapterInstance(m_cursor);
#if ASSERT_ENABLED
    // The walk is as long as the stack is deep, and an error that passes a handler in every frame asks at each of them.
    static std::atomic<unsigned> questions;
    if (!(questions.fetch_add(1, std::memory_order_relaxed) % 1024)) {
        Instance* adapterInstance = findEnclosingAdapterInstance(m_cursor);
        ASSERT(!adapterInstance || adapterInstance == state.instances.first());
    }
#endif
    return state.instances.first();
}

bool leavesByTailCall(BuiltinID builtin)
{
    return builtin == Builtins::FunctionPrototypeCall || builtin == Builtins::FunctionPrototypeApply;
}

LogicalFrame innermostLogicalFrame(const Image& image, InlineFrameReader& reader, const LogicalFrameSite& site)
{
    if (site.calledBuiltin)
        return { functionInFramesOf(site.calledBuiltin), site.callSite, site.inlineFrame, false, site.calleeWasEntered && leavesByTailCall(site.calledBuiltin), true, site.isTailDeleted };
    if (!site.inlineFrame)
        return { image.rootFunctionOf(site.code.index()), site.callSite, 0, false, site.isTailDeleted };
    const InlineFrameRecord& record = reader.at(image, site.code.index(), site.inlineFrame);
    if (record.function.index() >= image.programTables().numberOfFunctions()) [[unlikely]]
        return { record.function, record.callSite, record.parent, false, false, true, record.isTailCall };
    return { record.function, site.callSite, site.inlineFrame, record.isConstruct, site.isTailDeleted };
}

LogicalFrame callerOfInlinedFrame(const Image& image, InlineFrameReader& reader, CompiledFunctionID code, const LogicalFrame& frame)
{
    if (frame.isOfCalledBuiltin)
        return innermostLogicalFrame(image, reader, { code, frame.callSite, frame.inlineFrame, frame.callerIsTailDeleted });
    ASSERT(frame.inlineFrame);
    InlineFrameRecord record = reader.at(image, code.index(), frame.inlineFrame);
    return innermostLogicalFrame(image, reader, { code, record.callSite, record.parent, record.isTailCall });
}

bool callersWaitForStep(CallFrame* frameOfStepStub)
{
    SavedFrame* body = savedFrameOfGenerator(std::bit_cast<JSObject* const*>(frameOfStepStub)[asyncGeneratorSlotInStubFrame]);
    return body && body->hasPassedAwaitOnStack();
}

bool isEntrypointOf(FunctionID candidate, FunctionID sourceFunction)
{
    FunctionDescription description(candidate);
    return description.visibility() != TraceVisibility::Visible && description.sourceFunction() == sourceFunction;
}

// The tests at the head of a function leave by a tail call into its body, which runs under the same function object.
static bool isFormThatLeftByTailCall(FunctionID candidate, FunctionID sourceFunction, bool leftByTailCall)
{
    return leftByTailCall && FunctionDescription(candidate).sourceFunction() == sourceFunction;
}

std::optional<CalleeOfSite> calleeOfCopiedFrame(const Image& image, InlineFrameReader& reader, CompiledFunctionID code, LogicalFrame frame)
{
    if (frame.isOfCalledBuiltin || !frame.inlineFrame)
        return std::nullopt;
    FunctionID source = FunctionDescription(frame.function).sourceFunction();
    while (frame.inlineFrame) {
        InlineFrameRecord record = reader.at(image, code.index(), frame.inlineFrame);
        if (std::optional<CalleeOfSite> callee = image.calleeOfSiteAt(code.index(), record.callSite))
            return callee;
        frame = callerOfInlinedFrame(image, reader, code, frame);
        if (!isEntrypointOf(frame.function, source) && !isFormThatLeftByTailCall(frame.function, source, record.isTailCall))
            return std::nullopt;
    }
    return std::nullopt;
}

bool hasOnlyEntrypointsBelow(const Image& image, InlineFrameReader& reader, CompiledFunctionID code, LogicalFrame frame, FunctionID source)
{
    while (frame.inlineFrame) {
        bool leftByTailCall = reader.at(image, code.index(), frame.inlineFrame).isTailCall;
        frame = callerOfInlinedFrame(image, reader, code, frame);
        if (!isEntrypointOf(frame.function, source) && !isFormThatLeftByTailCall(frame.function, source, leftByTailCall))
            return false;
    }
    return true;
}

static std::optional<SourcePosition> findPosition(const CompiledFrame& frame)
{
    const VMState& state = frame.instance->vm->aotState();
    return state.callSiteCache->positionOf(*state.image, frame.code, frame.callSite);
}

JSGlobalObject* CompiledFrame::globalObject() const
{
    return instance->globalObject;
}

String CompiledFrame::sourceURL() const
{
    auto position = findPosition(*this);
    return position && position->source ? sourceURLOf(position->source) : String();
}

LineColumn CompiledFrame::lineColumn() const
{
    if (instance->vm->aotState().image->isObfuscated())
        return obfuscatedPositionOf(TraceFrame { code, function, callSite, !!inlineFrame, false });
    auto position = findPosition(*this);
    return position ? position->lineColumn : LineColumn { };
}

SourceOrigin CompiledFrame::sourceOrigin() const
{
    auto position = findPosition(*this);
    return position && position->source ? modulesOf(*instance).sourceOrigin(position->source) : SourceOrigin();
}

bool CompiledFrame::isActivationOf(JSCell* cell) const
{
    return isClosureOf(cell, function);
}

CompiledFrameIterator::CompiledFrameIterator(VM& vm, const Cursor& cursor, bool calleeWasEntered)
    : m_vm(&vm)
    , m_image(vm.aotState().image)
    , m_cache(vm.aotState().callSiteCache.get())
    , m_cursor(cursor)
    , m_calleeWasEntered(calleeWasEntered)
{
    enterMachineFrame(findFunctionFrame(m_cursor, m_exit, m_runEnd));
}

FunctionID findFunctionConstructedAt(const Image& image, const CallSiteCache::Entry& site, CallFrame* plainFunctionConstructFrame)
{
    if (plainFunctionConstructFrame)
        return functionOf(*std::bit_cast<JSCell**>(std::bit_cast<uintptr_t>(plainFunctionConstructFrame) + PlainFunctionConstructFrame::offsetOfCallee)).value_or(FunctionID());
    return image.constructedCalleeAt(site.code.index(), site.site.callSite).value_or(FunctionID());
}

bool isCodeOf(const Image& image, CompiledFunctionID code, FunctionID function)
{
    return sourceFunctionOf(image.rootFunctionOf(code.index())) == sourceFunctionOf(function);
}

std::optional<TraceFrame> frameOfConstructedFunctionThatLeft(const Image& image, FunctionID constructed, CompiledFunctionID enteredSince)
{
    FunctionID function = sourceFunctionOf(constructed);
    // Nothing on the stack says which of several calls it left by. The callee that stands there now speaks for the call that names it.
    uint32_t calleeOnStack = enteredSince ? sourceFunctionOf(image.rootFunctionOf(enteredSince.index())).index() : TailCallPlace::noCallee;
    std::optional<uint32_t> found;
    uint32_t number = 0;
    forEachTailCallPlace(image.programTables().placesOfTailCalls(), [&](const TailCallPlace& place) {
        if (place.function > function.index())
            return IterationStatus::Done;
        if (place.function == function.index()) {
            if (!found)
                found = number;
            if (place.callee != TailCallPlace::noCallee && place.callee == calleeOnStack) {
                found = number;
                return IterationStatus::Done;
            }
        }
        ++number;
        return IterationStatus::Continue;
    });
    if (!found)
        return std::nullopt;
    uint32_t numberInFrames = image.header().counts.functions + *found;
    return TraceFrame { CompiledFunctionID(numberInFrames), function, CallSiteIndex(0), false, true, false, false, true, false };
}

bool CompiledFrameIterator::RunEnd::constructs(FunctionID outermost) const
{
    if (!isConstruct)
        return false;
    CalleeBits callee = adapterFrame->callee();
    return callee.isCell() && isClosureOf(callee.asCell(), outermost);
}

FunctionID CompiledFrameIterator::RunEnd::constructedFunction() const
{
    if (!isConstruct)
        return { };
    CalleeBits callee = adapterFrame->callee();
    return callee.isCell() ? functionOf(callee.asCell()).value_or(FunctionID()) : FunctionID();
}

// An operation that a function calls at the site of a tail call stands for the callee, a built-in. What the operation calls runs after the tail call.
std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreate(VM& vm, const Cursor& cursor, OperationState operationState)
{
    if (!vm.hasCompiledCode() || !cursor.framePointer || !cursor.rawPC)
        return std::nullopt;
    const void* pc = removeCodePtrTag(cursor.rawPC);
    const CallSiteCache::Entry* site = findReturnSite(vm, pc);
    if (!site || isInHostCallFrame(*site))
        return std::nullopt;
    bool calleeWasEntered = operationState == OperationState::HasCalled;
    if (site->isStubReturnSite())
        calleeWasEntered = (calleeWasEntered || entersCallee(site->stubFrameKind)) && !hasZombieCallee(vm, cursor.framePointer, pc);
    return std::optional<CompiledFrameIterator>(std::in_place, vm, cursor, calleeWasEntered);
}

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreate(VM& vm, CallFrame* framePointer, const void* returnAddress, EntryFrame* entryFrame, OperationState operationState, void* stackPointer)
{
    return tryCreate(vm, Cursor { framePointer, const_cast<void*>(returnAddress), entryFrame, nullptr, stackPointer }, operationState);
}

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreateAtCallerOf(VM& vm, CallFrame* callee, EntryFrame* entryFrame)
{
    return tryCreate(vm, Cursor { callee->callerFrame(), callee->rawReturnPC(), entryFrame, callee, stackPointerOfCaller(callee) }, OperationState::MayNotHaveCalled);
}

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreateAtTop(VM& vm)
{
    if (!isTopFrameCompiled(vm))
        return std::nullopt;
    return tryCreateAtCallerOf(vm, vm.topCallFrame, vm.topEntryFrame);
}

void CompiledFrameIterator::enterFrame(Cursor& cursor, const CallSiteCache::Entry& site)
{
    cursor.hasFrameRecord = site.frame.hasFrameRecord();
    cursor.bytesAboveReturnAddress = bytesAboveReturnAddressOf(site);
    RELEASE_ASSERT(cursor.hasFrameRecord || cursor.stackPointer);
    cursor.callFrame = frameAtReturnSite(site.frame, cursor.stackPointer, cursor.framePointer);
}

void CompiledFrameIterator::leaveRun(Cursor& cursor, Exit& exit)
{
    ASSERT(cursor.hasFrameRecord);
    EntryFrame* entryFrameBefore = cursor.entryFrame;
    void* returnPC = cursor.callFrame->rawReturnPC();
    exit.callFrame = cursor.callFrame->callerFrame(cursor.entryFrame);
    exit.entryFrame = cursor.entryFrame;
    bool crossedEntryFrame = cursor.entryFrame != entryFrameBefore;
    exit.returnPC = crossedEntryFrame ? nullptr : returnPC;
    exit.crossedEntryFrame = crossedEntryFrame;
    exit.calleeFrame = crossedEntryFrame ? nullptr : cursor.callFrame;
}

bool CompiledFrameIterator::passOverFrame(Cursor& cursor, Exit& exit)
{
    if (cursor.hasFrameRecord) {
        if (cursor.callFrame->callerFrameOrEntryFrame() == cursor.entryFrame) {
            leaveRun(cursor, exit);
            return false;
        }
        cursor.framePointer = cursor.callFrame->callerFrame();
    }
    cursor.calleeFrame = cursor.callFrame;
    cursor.stackPointer = stackPointerOfCaller(cursor.callFrame, cursor.bytesAboveReturnAddress);
    cursor.rawPC = cursor.callFrame->rawReturnPC();
    return true;
}

NEVER_INLINE bool CompiledFrameIterator::passOverMethodReadFrame(Cursor& cursor, Exit& exit)
{
    cursor.isAtFirstReadSite = true;
    if (!passOverFrame(cursor, exit))
        return false;
    m_cache->withMethodReadSite(*m_image, findReturnSiteInRun(*m_cache, *m_image, cursor));
    return true;
}

const CallSiteCache::Entry* CompiledFrameIterator::findFunctionFrame(Cursor& cursor, Exit& exit, RunEnd& runEnd)
{
    for (;;) {
        const CallSiteCache::Entry* site = findReturnSiteInRun(*m_cache, *m_image, cursor);
        if (!site || isInHostCallFrame(*site)) {
            exit = { cursor.framePointer, cursor.rawPC, cursor.entryFrame, false, cursor.calleeFrame };
            return nullptr;
        }
        enterFrame(cursor, *site);
        if (!site->isStubReturnSite()) [[likely]]
            return site;
        switch (site->stubFrameKind) {
        case StubFrameKind::InHostCallFrame:
            RELEASE_ASSERT_NOT_REACHED();
        case StubFrameKind::Plain:
        case StubFrameKind::ReturnsAfterOperation:
        case StubFrameKind::ArgumentList:
        case StubFrameKind::HostCall:
        case StubFrameKind::BelowSpilledArguments:
        case StubFrameKind::AtStubEntry:
            break;
        case StubFrameKind::MethodRead:
            if (!passOverMethodReadFrame(cursor, exit))
                return nullptr;
            continue;
        case StubFrameKind::FirstOfTwoReads:
            cursor.isAtFirstReadSite = true;
            break;
        case StubFrameKind::GeneratorBody:
            cursor.generatorStubFrame = cursor.callFrame;
            break;
        case StubFrameKind::AsyncGeneratorBody:
            cursor.asyncGeneratorStubFrame = cursor.callFrame;
            cursor.asyncGeneratorBodyFrame = cursor.calleeFrame;
            cursor.isAmongFramesThatWait |= callersWaitForStep(cursor.callFrame);
            break;
        case StubFrameKind::EngineCallee:
            cursor.engineCalleeStubFrame = cursor.callFrame;
            break;
        case StubFrameKind::PlainFunctionConstruct:
            cursor.plainFunctionConstructFrame = cursor.callFrame;
            break;
        case StubFrameKind::Adapter:
        case StubFrameKind::HandlerOfJob:
        case StubFrameKind::ConstructAdapter:
        case StubFrameKind::SavedFrameAdapter:
            runEnd = { cursor.callFrame, site->stubFrameKind == StubFrameKind::ConstructAdapter, site->stubFrameKind == StubFrameKind::SavedFrameAdapter, site->stubFrameKind == StubFrameKind::HandlerOfJob };
            leaveRun(cursor, exit);
            return nullptr;
        }
        if (!passOverFrame(cursor, exit))
            return nullptr;
    }
}

const CallSiteCache::Entry* CompiledFrameIterator::findCallerFunctionFrame(Cursor& cursor, Exit& exit, RunEnd& runEnd)
{
    return passOverFrame(cursor, exit) ? findFunctionFrame(cursor, exit, runEnd) : nullptr;
}

void CompiledFrameIterator::enterMachineFrame(const CallSiteCache::Entry* entry)
{
    if (!entry) {
        m_isAtEnd = true;
        return;
    }
    m_callSite = *entry;
    if (!m_codeAbove || !entry->isOfEntrypointAtTailCall())
        m_codeAbove = entry->code;
    m_hasReadFrame = false;
    if (!m_instance) [[unlikely]] {
        m_instance = tryFindInstance();
        RELEASE_ASSERT(m_instance);
    }
}

void CompiledFrameIterator::readFrame() const
{
    const CallSiteCache::Entry& entry = m_callSite;
    CallSiteIndex site = m_cursor.isAtFirstReadSite && entry.site.firstReadSite ? *entry.site.firstReadSite : entry.site.callSite;
    m_frame = { m_instance, m_cursor.callFrame, removeCodePtrTag(m_cursor.rawPC), entry.code, entry.innermostFunction, site, entry.site.inlineFrame, entry.innermostVisibility, entry.site.isTailDeleted(m_calleeWasEntered) };
    m_frame.calledBuiltin = entry.site.calledBuiltin;
    if (m_calleeWasEntered && m_frame.calledBuiltin && leavesByTailCall(m_frame.calledBuiltin))
        m_frame.calledBuiltin = { };
    if (m_cursor.isAmongFramesThatWait) {
        m_frame.visibility = TraceVisibility::Hidden;
        m_frame.calledBuiltin = { };
    }
    m_isInlinedConstructCall = entry.innermostIsInlinedConstructCall;
    m_hasReadFrame = true;
}

std::optional<TraceFrame> CompiledFrameIterator::findConstructedFunctionThatLeft()
{
    Cursor cursor = m_cursor;
    cursor.plainFunctionConstructFrame = nullptr;
    Exit exit;
    RunEnd runEnd;
    const CallSiteCache::Entry* caller = findCallerFunctionFrame(cursor, exit, runEnd);
    FunctionID constructed;
    if (!caller)
        constructed = runEnd.constructedFunction();
    else if (caller->site.isConstruct())
        constructed = functionConstructedAt(*m_image, *caller, cursor.plainFunctionConstructFrame);
    if (!constructed || isCodeOf(*m_image, m_codeAbove, constructed))
        return std::nullopt;
    return frameOfConstructedFunctionThatLeft(*m_image, constructed, m_codeAbove);
}

void CompiledFrameIterator::listConstructedFunctionsThatLeft()
{
    m_listsConstructedFunctionsThatLeft = true;
    if (m_isAtEnd || !m_cursor.plainFunctionConstructFrame || !m_callSite.site.isConstruct())
        return;
    if (FunctionID constructed = functionConstructedAt(*m_image, m_callSite, m_cursor.plainFunctionConstructFrame))
        m_constructedFunctionThatLeft = frameOfConstructedFunctionThatLeft(*m_image, constructed, { });
    m_constructedFunctionLeftOnTop = m_constructedFunctionThatLeft.has_value();
}

void CompiledFrameIterator::advance()
{
    if (m_isAtEnd)
        return;
    if (m_constructedFunctionThatLeft) [[unlikely]] {
        m_constructedFunctionThatLeft = std::nullopt;
        if (std::exchange(m_constructedFunctionLeftOnTop, false))
            return;
        advanceToCallerMachineFrame();
        return;
    }
    m_calleeFunction = frame().function;
    if (!m_frame.inlineFrame) {
        if (m_listsConstructedFunctionsThatLeft && !m_cursor.isAmongFramesThatWait) {
            m_constructedFunctionThatLeft = findConstructedFunctionThatLeft();
            if (m_constructedFunctionThatLeft) [[unlikely]]
                return;
        }
        advanceToCallerMachineFrame();
        return;
    }
    LogicalFrame current { m_frame.function, m_frame.callSite, m_frame.inlineFrame, m_isInlinedConstructCall, m_frame.isTailDeleted };
    LogicalFrame caller = callerOfInlinedFrame(*m_image, m_inlineFrames, m_frame.code, current);
    m_frame.calledBuiltin = { };
    if (caller.isOfCalledBuiltin) [[unlikely]] {
        if (!m_cursor.isAmongFramesThatWait)
            m_frame.calledBuiltin = builtinOfFunctionInFrames(caller.function);
        caller = callerOfInlinedFrame(*m_image, m_inlineFrames, m_frame.code, caller);
    }
    m_frame.function = caller.function;
    m_frame.callSite = caller.callSite;
    m_frame.inlineFrame = caller.inlineFrame;
    m_frame.isTailDeleted = caller.isTailDeleted;
    m_frame.visibility = m_cursor.isAmongFramesThatWait ? TraceVisibility::Hidden : FunctionDescription(caller.function).visibility();
    m_isInlinedConstructCall = caller.isInlinedConstructCall;
}

bool CompiledFrameIterator::tailCalledOwnBody() const
{
    return m_calleeFunction && isFormThatLeftByTailCall(frame().function, FunctionDescription(m_calleeFunction).sourceFunction(), m_frame.isTailDeleted);
}

bool CompiledFrameIterator::isAboveFramesThatWait() const
{
    if (m_isAtEnd || m_cursor.isAmongFramesThatWait)
        return false;
    CompiledFrameIterator callers(*this);
    callers.advanceToCallerMachineFrame();
    return callers.m_cursor.isAmongFramesThatWait;
}

void CompiledFrameIterator::advanceToCallerMachineFrame()
{
    if (m_isAtEnd)
        return;
    m_cursor.isAtFirstReadSite = false;
    m_cursor.generatorStubFrame = nullptr;
    m_cursor.asyncGeneratorStubFrame = nullptr;
    m_cursor.asyncGeneratorBodyFrame = nullptr;
    m_cursor.engineCalleeStubFrame = nullptr;
    m_cursor.plainFunctionConstructFrame = nullptr;
    m_calleeWasEntered = true;
    enterMachineFrame(findCallerFunctionFrame(m_cursor, m_exit, m_runEnd));
}

bool CompiledFrameIterator::isConstructCall()
{
    FunctionID source = FunctionDescription(frame().function).sourceFunction();
    LogicalFrame logical { m_frame.function, m_frame.callSite, m_frame.inlineFrame, m_isInlinedConstructCall, m_frame.isTailDeleted };
    while (logical.inlineFrame) {
        if (logical.isInlinedConstructCall)
            return true;
        logical = callerOfInlinedFrame(*m_image, m_inlineFrames, m_frame.code, logical);
        if (!isEntrypointOf(logical.function, source))
            return false;
    }

    Cursor cursor = m_cursor;
    Exit exit;
    RunEnd runEnd;
    for (;;) {
        cursor.plainFunctionConstructFrame = nullptr;
        const CallSiteCache::Entry* entry = findCallerFunctionFrame(cursor, exit, runEnd);
        if (!entry)
            return runEnd.constructs(source);
        if (entry->site.isConstruct()) {
            FunctionID constructed = functionConstructedAt(*m_image, *entry, cursor.plainFunctionConstructFrame);
            return !constructed || sourceFunctionOf(constructed) == source;
        }
        if (entry->innermostVisibility == TraceVisibility::Visible)
            return false;
        LogicalFrame caller { entry->innermostFunction, entry->site.callSite, entry->site.inlineFrame, entry->innermostIsInlinedConstructCall, false };
        for (;;) {
            if (!isEntrypointOf(caller.function, source))
                return false;
            if (!caller.inlineFrame)
                break;
            if (caller.isInlinedConstructCall)
                return true;
            caller = callerOfInlinedFrame(*m_image, m_inlineFrames, entry->code, caller);
        }
    }
}

std::optional<JSObject*> CompiledFrameIterator::recordedCallee()
{
    FunctionTables& tables = FunctionTables::of(*m_vm);
    if (!tables.framesMayBeNamedByCallee()) [[likely]]
        return std::nullopt;

    // The entrypoint records the callee. Its frame, or the one that took its place in a tail call, is the outermost of the hidden frames of the function.
    FunctionID source = FunctionDescription(frame().function).sourceFunction();
    LogicalFrame logical { m_frame.function, m_frame.callSite, m_frame.inlineFrame, m_isInlinedConstructCall, m_frame.isTailDeleted };
    auto calleeSaidBy = [&](CalleeOfSite callee, const CallFrame* machineFrame) -> std::optional<JSObject*> {
        return callee.isOwnFunctionObject() ? nullptr : tables.calleeKeptInFrame(*m_vm, machineFrame, callee, source);
    };
    if (std::optional<CalleeOfSite> callee = calleeOfCopiedFrame(*m_image, m_inlineFrames, m_frame.code, logical))
        return calleeSaidBy(*callee, m_cursor.callFrame);
    if (!hasOnlyEntrypointsBelow(*m_image, m_inlineFrames, m_frame.code, logical, source))
        return std::nullopt;

    Cursor cursor = m_cursor;
    Exit exit;
    RunEnd runEnd;
    const void* stackPointer = callerStackPointer();
    while (const CallSiteCache::Entry* entry = findCallerFunctionFrame(cursor, exit, runEnd)) {
        if (entry->hasRecordedCallee)
            return calleeSaidBy(*m_image->calleeOfSiteAt(entry->code.index(), entry->site.callSite), cursor.callFrame);
        // A function object of the same function that is called by value in tail position has a record of its own.
        if (!isEntrypointOf(entry->innermostFunction, source) && (!isFormThatLeftByTailCall(entry->innermostFunction, source, entry->site.isTailCall()) || tables.recordedCalleeOfCallFrom(stackPointer, source)))
            break;
        LogicalFrame caller { entry->innermostFunction, entry->site.callSite, entry->site.inlineFrame, entry->innermostIsInlinedConstructCall, false };
        if (std::optional<CalleeOfSite> callee = calleeOfCopiedFrame(*m_image, m_inlineFrames, entry->code, caller))
            return calleeSaidBy(*callee, cursor.callFrame);
        if (!hasOnlyEntrypointsBelow(*m_image, m_inlineFrames, entry->code, caller, source))
            break;
        stackPointer = AOT::stackPointerOfCaller(cursor.callFrame, cursor.bytesAboveReturnAddress);
    }
    if (runEnd.resumesSavedFrame)
        return std::nullopt;
    // Who enters compiled code from outside comes by no stub that records, and has the function object in his own frame.
    if (runEnd.adapterFrame && tables.recordsCalleesOf(source)) {
        JSObject* callee = runEnd.adapterFrame->jsCallee();
        std::optional<FunctionID> function = functionOf(callee);
        return function && sourceFunctionOf(*function) == source ? std::optional(callee) : std::nullopt;
    }
    return tables.recordedCalleeOfCallFrom(stackPointer, source);
}

bool isOperationFrameRecord(VM& vm, const CallFrame* frameRecord)
{
    if (!vm.hasCompiledCode() || !frameRecord)
        return false;
    const CallSiteCache::Entry* site = findReturnSite(vm, removeCodePtrTag(reinterpret_cast<const CallerFrameAndPC*>(frameRecord)->returnPC));
    return site && isOperationReturnSite(*site);
}

bool isTopFrameCompiled(VM& vm)
{
    return isOperationFrameRecord(vm, vm.topCallFrame);
}

bool isCallerCompiled(VM& vm, const CallFrame* calleeFrame, EntryFrame* entryFrame)
{
    if (calleeFrame->callerFrameOrEntryFrame() == entryFrame)
        return isOperationFrameRecord(vm, vmEntryRecord(entryFrame)->prevTopCallFrame());
    return vm.hasCompiledCode() && findReturnSite(vm, removeCodePtrTag(calleeFrame->rawReturnPC()));
}

static SampledTop sampledTopAtReturnSite(const Image& image, const CodeLocation& site, const void* returnAddress, const void* stackPointer, CallFrame* framePointer)
{
    ImageFrame frame = site.kind == CodeKind::Function ? image.frameAtReturnOffset(site.function, site.offset) : frameAtStubReturnSite(site.stubFrameKind);
    if (frame.hasFrameRecord())
        return { framePointer, returnAddress, nullptr };
    return { frameAtReturnSite(frame, stackPointer, framePointer), returnAddress, nullptr, framePointer };
}

// The tables say where a function has its frame at return addresses only, and a sample is taken anywhere: the frame is where a return address into compiled code lies above it.
static SUPPRESS_ASAN std::optional<SampledTop> findTopWithoutFrameRecord(const Image& image, const SampledRegisters& registers, const CodeLocation& top, const SuspendedThreadStack& stack)
{
    const ImageFrame& frame = image.frameOf(top.function);
    if (frame.hasFrameRecord() || !stack.contains(registers.stackPointer))
        return std::nullopt;
    uintptr_t stackPointer = std::bit_cast<uintptr_t>(registers.stackPointer);
    std::array<uintptr_t, 2> candidates { stackPointer + frame.frameSizeInBytes(), stackPointer - sizeof(void*) };
    if (!image.hasFrameAt(top.function, top.offset))
        std::swap(candidates[0], candidates[1]);
    for (uintptr_t candidate : candidates) {
        auto* record = std::bit_cast<const CallerFrameAndPC*>(candidate);
        if (stack.contains(&record->returnPC) && image.locate(removeCodePtrTag(record->returnPC)).runsCompiledCode())
            return SampledTop { std::bit_cast<CallFrame*>(candidate), registers.pc, nullptr, static_cast<CallFrame*>(registers.framePointer) };
    }
    return std::nullopt;
}

static SUPPRESS_ASAN SampledTop findTopInCompiledCode(const Image& image, const SampledRegisters& registers, const CodeLocation& top, const SuspendedThreadStack& stack)
{
    SampledTop result { static_cast<CallFrame*>(registers.framePointer), registers.pc, nullptr };
    if (!stack.contains(registers.framePointer))
        return result;
    if (top.kind == CodeKind::Function) {
        if (auto topWithoutFrameRecord = findTopWithoutFrameRecord(image, registers, top, stack))
            return *topWithoutFrameRecord;
    }
    auto* record = static_cast<const CallerFrameAndPC*>(registers.framePointer);
    const void* callReturnAddress = registers.linkRegister;
    if (!isARM64())
        callReturnAddress = stack.contains(registers.stackPointer) ? *static_cast<void* const*>(registers.stackPointer) : nullptr;
    if (!callReturnAddress || callReturnAddress == removeCodePtrTag(record->returnPC))
        return result;
    CodeLocation caller = image.locate(callReturnAddress);
    if (!caller.runsCompiledCode())
        return result;
    bool callerIsFunction = caller.kind == CodeKind::Function;
    if (top.kind == CodeKind::Function) {
        if (!callerIsFunction || caller.function != top.function) {
            // The frame is that of the caller.
            result = sampledTopAtReturnSite(image, caller, registers.pc, static_cast<void**>(registers.stackPointer) + 1, result.callFrame);
            result.callerPCOfFramelessTop = callReturnAddress;
        }
    } else if (callerIsFunction)
        return sampledTopAtReturnSite(image, caller, callReturnAddress, static_cast<void**>(registers.stackPointer) + 1, result.callFrame);
    return result;
}

// The function that a call entered, if the call records it.
struct CalleeOfCall {
    bool names(const CodeLocation& site) const { return function && site.kind == CodeKind::Function && site.function == *function; }

    bool mayBeFunction { false };
    // For a call that names its callee.
    std::optional<uint32_t> function { };
};

static bool callsFunction(StubFrameKind kind)
{
    switch (kind) {
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::SavedFrameAdapter:
    case StubFrameKind::HandlerOfJob:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::AsyncGeneratorBody:
    case StubFrameKind::ArgumentList:
    case StubFrameKind::PlainFunctionConstruct:
        return true;
    case StubFrameKind::Plain:
    case StubFrameKind::ReturnsAfterOperation:
    case StubFrameKind::EngineCallee:
    case StubFrameKind::HostCall:
    case StubFrameKind::FirstOfTwoReads:
    case StubFrameKind::MethodRead:
    case StubFrameKind::BelowSpilledArguments:
    case StubFrameKind::AtStubEntry:
    case StubFrameKind::InHostCallFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// x86-64 only. A function calls another by address, through a register, or through a stub that receives the callee the way a function does.
// Any other stub does the work itself or continues into native code.
static CalleeOfCall findCalleeOfCallEndingAt(const Image& image, const CodeLocation& returnSite, const void* returnAddress)
{
    if (returnSite.kind != CodeKind::Function)
        return { callsFunction(returnSite.stubFrameKind) };
    constexpr uint8_t opcodeOfNearCall = 0xe8;
    constexpr uint8_t opcodeOfGroup5 = 0xff;
    auto* end = static_cast<const uint8_t*>(returnAddress);
    if (returnSite.offset >= sizeOfNearCall && end[-static_cast<ptrdiff_t>(sizeOfNearCall)] == opcodeOfNearCall) {
        int32_t distance;
        memcpy(&distance, end - sizeof(distance), sizeof(distance));
        CodeLocation target = image.locate(std::bit_cast<const void*>(std::bit_cast<uintptr_t>(end) + distance));
        if (target.kind == CodeKind::Function)
            return { true, target.function };
        if (target.kind != CodeKind::NotInImage)
            return { target.kind == CodeKind::Stub && image.usesCallConvention(target.offset) };
    }
    return { returnSite.offset >= 2 && end[-2] == opcodeOfGroup5 && end[-1] >> 3 == 0x1a };
}

// stackPointer is the register's value at the site.
struct FrameAtSite {
    CodeLocation site;
    const void* stackPointer;
};

// The frame whose record is at that address: the given frame, or the first one above it that has a record. nullopt if that frame's record is elsewhere.
static SUPPRESS_ASAN std::optional<FrameAtSite> findFrameWithRecordAt(const Image& image, FrameAtSite frameAtSite, const CallFrame* frameRecord)
{
    while (true) {
        auto [site, stackPointer] = frameAtSite;
        bool isFunction = site.kind == CodeKind::Function;
        ImageFrame frame = isFunction ? image.frameAtReturnOffset(site.function, site.offset) : frameAtStubReturnSite(site.stubFrameKind);
        if (frame.hasFrameRecord()) {
            if (std::bit_cast<uintptr_t>(stackPointer) + frame.frameSizeInBytes() - frame.bytesAboveFrameRecord() != std::bit_cast<uintptr_t>(frameRecord))
                return std::nullopt;
            return frameAtSite;
        }
        CallFrame* callFrame = frameAtReturnSite(frame, stackPointer, nullptr);
        if (callFrame >= frameRecord)
            return std::nullopt;
        const void* returnAddress = removeCodePtrTag(reinterpret_cast<const CallerFrameAndPC*>(callFrame)->returnPC);
        CodeLocation caller = image.locate(returnAddress);
        if (!caller.runsCompiledCode() || !callEndsAt(image.code(), returnAddress))
            return std::nullopt;
        if (isFunction) {
            CalleeOfCall callee = findCalleeOfCallEndingAt(image, caller, returnAddress);
            if (!callee.mayBeFunction || (callee.function && !callee.names(site)))
                return std::nullopt;
        }
        frameAtSite = { caller, stackPointerOfCaller(callFrame, isFunction ? bytesAboveReturnAddressOf(frame) : bytesAboveReturnAddressOf(site.stubFrameKind)) };
    }
}

// Native code need not keep frame records. The first record above such code belongs to the compiled function that called it, or to one of that function's callers.
// Only where a call pushes its return address: the caller's frame ends right above that word, so the frame size tells whether the record is the caller's own.
// A function that has returned leaves stale return addresses behind, and the next callee puts its record where that function's was, so the call sites decide. The word follows a call
// into native code. If the call that the record returns to also goes to native code, the record belongs to the native code; if it names its callee, that function's word
// is the right one. A word of a function that it does not name is accepted only if no other frame size fits: the function there pushed one, and frames of one size end at one word.
static SUPPRESS_ASAN std::optional<SampledTop> findCallerOfCodeWithoutFrameRecord(const Image& image, const SuspendedThreadStack& stack, const void* stackPointer, CallFrame* frameRecord, const CodeLocation& returnSite, const void* returnAddress)
{
    if (!isX86_64() || !stack.contains(stackPointer))
        return std::nullopt;
    CalleeOfCall callee = findCalleeOfCallEndingAt(image, returnSite, returnAddress);
    if (!callee.mayBeFunction)
        return std::nullopt;
    std::optional<SampledTop> caller;
    const void* endOfFrameWithRecord = nullptr;
    bool framesOfSeveralSizesFit = false;
    for (auto* word = static_cast<void* const*>(stackPointer); std::bit_cast<uintptr_t>(word) < std::bit_cast<uintptr_t>(frameRecord); ++word) {
        CodeLocation site = image.locate(*word);
        if (site.kind != CodeKind::Function || !callEndsAt(image.code(), *word) || findCalleeOfCallEndingAt(image, site, *word).mayBeFunction)
            continue;
        std::optional<FrameAtSite> frameWithRecord = findFrameWithRecordAt(image, { site, word + 1 }, frameRecord);
        if (!frameWithRecord)
            continue;
        if (callee.names(frameWithRecord->site))
            return sampledTopAtReturnSite(image, site, *word, word + 1, frameRecord);
        if (!caller) {
            caller = sampledTopAtReturnSite(image, site, *word, word + 1, frameRecord);
            endOfFrameWithRecord = frameWithRecord->stackPointer;
        } else if (frameWithRecord->stackPointer != endOfFrameWithRecord)
            framesOfSeveralSizesFit = true;
    }
    return framesOfSeveralSizesFit ? std::nullopt : caller;
}

static SUPPRESS_ASAN std::optional<SampledTop> findTopBelowNativeCode(const Image& image, const SampledRegisters& registers, CallFrame* topCallFrame, const SuspendedThreadStack& stack)
{
    auto* frame = static_cast<CallFrame*>(registers.framePointer);
    const void* linkRegister = removeReturnAddressSignature(registers.linkRegister);
    if (linkRegister && stack.contains(frame)
        && removeReturnAddressSignature(reinterpret_cast<const CallerFrameAndPC*>(frame)->returnPC) != linkRegister
        && image.locate(linkRegister).runsCompiledCode())
        return SampledTop { frame, linkRegister, nullptr };
    const void* pc = registers.pc;
    const void* endOfFrameBelow = registers.stackPointer;
    while (stack.contains(frame)) {
        auto* record = reinterpret_cast<const CallerFrameAndPC*>(frame);
        const void* returnAddress = removeReturnAddressSignature(record->returnPC);
        CodeLocation returnSite = image.locate(returnAddress);
        if (frame == topCallFrame && !isOperationReturnSite(returnSite))
            return SampledTop { topCallFrame, pc, nullptr };
        if (returnSite.runsCompiledCode()) {
            if (auto caller = findCallerOfCodeWithoutFrameRecord(image, stack, endOfFrameBelow, frame, returnSite, returnAddress))
                return caller;
            return sampledTopAtReturnSite(image, returnSite, returnAddress, stackPointerOfCaller(frame), record->callerFrame);
        }
        if (record->callerFrame <= frame)
            break;
        pc = returnAddress;
        endOfFrameBelow = stackPointerOfCaller(frame);
        frame = record->callerFrame;
    }
    return std::nullopt;
}

std::optional<SampledTop> findSampledTop(const Image& image, const SampledRegisters& registers, CallFrame* topCallFrame, const StackBounds& bounds)
{
    SuspendedThreadStack stack { bounds };
    CodeLocation top = image.locate(registers.pc);
    if (top.runsCompiledCode())
        return findTopInCompiledCode(image, registers, top, stack);
    return findTopBelowNativeCode(image, registers, topCallFrame, stack);
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(CallSiteCache);

CallSiteCache::CallSiteCache() = default;

static uint16_t countShownFrames(const Image& image, InlineFrameReader& reader, CompiledFunctionID code, const CallSiteRecord& site, ShownFrameFilter filter, bool innermostIsTailDeleted)
{
    uint16_t count = 0;
    LogicalFrame frame = innermostLogicalFrame(image, reader, { code, site.callSite, site.inlineFrame, innermostIsTailDeleted, site.calledBuiltin });
    while (true) {
        if (filter.shows(FunctionDescription(frame.function).visibility(), frame.isTailDeleted))
            ++count;
        if (!frame.hasCallerInMachineFrame())
            return count;
        frame = callerOfInlinedFrame(image, reader, code, frame);
    }
}

OutermostVisibleFunction findOutermostVisibleFunction(const Image& image, InlineFrameReader& reader, const CallSiteCache::Entry& entry, bool calleeWasEntered)
{
    const CallSiteRecord& site = entry.site;
    if (!site.isTailCall() && !site.inlineFrame && entry.innermostVisibility == TraceVisibility::Visible) [[likely]]
        return OutermostVisibleFunction::Listed;
    auto outermost = OutermostVisibleFunction::None;
    for (LogicalFrame frame = innermostLogicalFrame(image, reader, { entry.code, site.callSite, site.inlineFrame, site.isTailDeleted(calleeWasEntered), site.calledBuiltin, calleeWasEntered });; frame = callerOfInlinedFrame(image, reader, entry.code, frame)) {
        if (FunctionDescription(frame.function).visibility() == TraceVisibility::Visible)
            outermost = frame.isTailDeleted ? OutermostVisibleFunction::HiddenBehindTailCall : OutermostVisibleFunction::Listed;
        if (!frame.hasCallerInMachineFrame())
            return outermost;
    }
}

static void noteInlinedFramesWithRecordedCallees(const Image& image, InlineFrameReader& reader, CallSiteCache::Entry& entry, LogicalFrame frame)
{
    if (!image.hasCalleesOfSites(entry.code.index()))
        return;
    for (; frame.inlineFrame; frame = callerOfInlinedFrame(image, reader, entry.code, frame)) {
        if (!image.calleeOfSiteAt(entry.code.index(), reader.at(image, entry.code.index(), frame.inlineFrame).callSite))
            continue;
        FunctionDescription description(frame.function);
        entry.hasInlinedFrameRecordingCallee |= description.recordsCallee();
        entry.hasInlinedFrameRecordingCalleeOnceRenamed |= description.recordsCalleeOnceRenamed();
    }
}

static CallSiteCache::Entry createFunctionEntry(const Image& image, InlineFrameReader& reader, const CodeLocation& location, const void* returnAddress)
{
    CallSiteCache::Entry entry;
    entry.address = returnAddress;
    entry.code = CompiledFunctionID(location.function);
    ReturnSiteInfo returnSite = image.returnSiteInfoAt(location.function, location.offset);
    if (returnSite.site)
        entry.site = *returnSite.site;
    entry.handler = returnSite.handler;
    entry.frame = returnSite.frame;
    FunctionDescription root(image.rootFunctionOf(location.function));
    entry.rejectsOnThrow = root.rejectsOnThrow();
    entry.isInModuleBody = root.codeType() == ModuleCode;

    LogicalFrame innermost = innermostLogicalFrame(image, reader, { entry.code, entry.site.callSite, entry.site.inlineFrame, false });
    entry.innermostFunction = innermost.function;
    entry.innermostIsInlinedConstructCall = innermost.isInlinedConstructCall;
    FunctionDescription innermostDescription(innermost.function);
    entry.innermostVisibility = innermostDescription.visibility();
    entry.innermostIsAsync = innermostDescription.isAsync();
    entry.hasRecordedCallee = image.calleeOfSiteAt(location.function, entry.site.callSite).has_value();
    entry.namesConstructedCallee = image.constructedCalleeAt(location.function, entry.site.callSite).has_value();
    noteInlinedFramesWithRecordedCallees(image, reader, entry, innermost);
    entry.namesTailCallingBuiltin = entry.site.calledBuiltin && leavesByTailCall(entry.site.calledBuiltin);
    entry.outermostVisibleFunction = findOutermostVisibleFunction(image, reader, entry, true);
    entry.shownFrameCount = countShownFrames(image, reader, entry.code, entry.site, ShownFrameFilter(true), entry.site.isTailCall());
    entry.shownFrameCountOnTop = std::min<uint16_t>(countShownFrames(image, reader, entry.code, entry.site, ShownFrameFilter(false), false), CallSiteCache::Entry::tooManyFramesToCount);
    if (entry.site.isTailCall())
        entry.shownFrameCountOnTopBehindTailCall = std::min<uint16_t>(countShownFrames(image, reader, entry.code, entry.site, ShownFrameFilter(false), true), CallSiteCache::Entry::tooManyFramesToCount);
    return entry;
}

static CallSiteCache::Entry createStubEntry(const CodeLocation& location, const void* returnAddress)
{
    CallSiteCache::Entry entry;
    entry.address = returnAddress;
    entry.stubFrameKind = location.stubFrameKind;
    entry.frame = frameAtStubReturnSite(location.stubFrameKind);
    return entry;
}

CallSiteCache::Entry* CallSiteCache::findEmptySlot(const void* address)
{
    Entry& primary = m_entries[primaryIndex(address)];
    if (!primary.address)
        return &primary;
    Entry& secondary = m_entries[secondaryIndex(address)];
    return secondary.address ? nullptr : &secondary;
}

void CallSiteCache::setNumberOfEntries(size_t numberOfEntries)
{
    ASSERT(std::has_single_bit(numberOfEntries) && numberOfEntries > m_entries.size());
    FixedVector<Entry> previousEntries = std::exchange(m_entries, FixedVector<Entry>(numberOfEntries));
    m_table = m_entries.span().data();
    m_indexMask = numberOfEntries - 1;
    m_secondaryIndexShift = 64 - static_cast<unsigned>(std::countr_zero(numberOfEntries));
    m_numberOfFilledEntries = 0;
    for (const Entry& entry : previousEntries) {
        if (!entry.address)
            continue;
        if (Entry* slot = findEmptySlot(entry.address)) {
            *slot = entry;
            ++m_numberOfFilledEntries;
        }
    }
}

// An entry that is pushed out comes back into its other slot the next time it is asked for, which settles as long as less than half of the slots are taken. From there on the table doubles.
CallSiteCache::Entry& CallSiteCache::slotForNewEntry(const void* address)
{
    if (m_entries.isEmpty())
        setNumberOfEntries(initialNumberOfEntries);
    for (;;) {
        if (Entry* slot = findEmptySlot(address)) {
            ++m_numberOfFilledEntries;
            return *slot;
        }
        if (m_entries.size() == maxNumberOfEntries || m_numberOfFilledEntries * 2 < m_entries.size())
            break;
        setNumberOfEntries(m_entries.size() * 2);
    }
    m_evictsSecondary = !m_evictsSecondary;
    return m_entries[m_evictsSecondary ? secondaryIndex(address) : primaryIndex(address)];
}

const CallSiteCache::Entry* CallSiteCache::resolve(const Image& image, const void* returnAddress)
{
    if (const Entry* entry = find(returnAddress))
        return entry;

    CodeLocation location = image.locate(returnAddress);
    if (!location.runsCompiledCode())
        return nullptr;

    Entry& entry = slotForNewEntry(returnAddress);
    entry = location.kind == CodeKind::Stub ? createStubEntry(location, returnAddress) : createFunctionEntry(image, m_inlineFrames, location, returnAddress);
    return &entry;
}

const CallSiteCache::Entry* CallSiteCache::withMethodReadSite(const Image& image, const Entry* entry)
{
    if (!entry || entry->isStubReturnSite() || entry->site.firstReadSite)
        return entry;
    std::optional<FaultSite> throwSite = image.faultSiteAt(static_cast<const uint8_t*>(entry->address) - sizeOfNearCall);
    if (!throwSite || throwSite->kind != FaultSiteKind::NullCheck)
        return entry;
    ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(throwSite->throwSite[0]);
    if (word.isCallSite() && word.registerNumber)
        const_cast<Entry*>(entry)->site.firstReadSite = firstReadSiteInFrontOf(entry->site.callSite, word.registerNumber - 1);
    return entry;
}

std::optional<SourcePosition> CallSiteCache::positionOf(const Image& image, CompiledFunctionID code, CallSiteIndex site)
{
    Position& remembered = m_positions[(code.index() * 31 + site.bits()) % numberOfPositions];
    if (remembered.code == code && remembered.callSite == site)
        return remembered.position;
    auto position = image.positionOf(code.index(), site);
    if (position)
        remembered = { code, site, *position };
    return position;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
