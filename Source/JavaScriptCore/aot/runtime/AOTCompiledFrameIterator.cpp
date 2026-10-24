/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiledFrame.h"

#if ENABLE(AOT)

#include "AOTCompiledCallee.h"
#include "AOTInstance.h"
#include "AOTModules.h"
#include "AOTStackTrace.h"
#include "CallFrameInlines.h"
#include "JSCJSValueInlines.h"
#include "JSCellInlines.h"
#include "SourceOrigin.h"
#include "VM.h"

#if defined(BUN_HARDENED)
#include <link.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool isAdapter(StubFrameKind kind)
{
    switch (kind) {
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::SavedFrameAdapter:
    case StubFrameKind::HandlerOfJob:
        return true;
    case StubFrameKind::Plain:
    case StubFrameKind::EngineCallee:
    case StubFrameKind::HostCall:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::AsyncGeneratorBody:
    case StubFrameKind::ArgumentList:
    case StubFrameKind::FirstOfTwoReads:
    case StubFrameKind::BelowSpilledArguments:
    case StubFrameKind::AtStubEntry:
    case StubFrameKind::InHostCallFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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

#if defined(BUN_HARDENED)
template<typename Cursor>
static ALWAYS_INLINE const CallSiteCache::Entry* findReturnSiteInRun(CallSiteCache& cache, const Image& image, const Cursor& cursor)
{
    const void* pc = removeCodePtrTag(cursor.rawPC);
    if (const CallSiteCache::Entry* site = cache.find(pc)) [[likely]]
        return site;
    return resolveReturnAddressInRun(cache, image, pc, cursor.calleeFrame);
}
#endif

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
    switch (location.kind) {
    case CodeKind::Function:
        return true;
    case CodeKind::Stub:
        return !entersCallee(location.stubFrameKind);
    case CodeKind::NotInImage:
    case CodeKind::RegExp:
    case CodeKind::WebAssembly:
    case CodeKind::FFI:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#if defined(BUN_HARDENED)

static bool isInCodeOfEngine(const void* address)
{
    struct Segments {
        std::array<std::pair<uintptr_t, uintptr_t>, 4> startAndSize { };
    };
    static const Segments segments = [] {
        Segments result;
        // The program itself comes first.
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
            size_t count = 0;
            auto& found = static_cast<Segments*>(data)->startAndSize;
            for (const auto& header : std::span { info->dlpi_phdr, info->dlpi_phnum }) {
                if (header.p_type == PT_LOAD && (header.p_flags & PF_X) && count < found.size())
                    found[count++] = { info->dlpi_addr + header.p_vaddr, header.p_memsz };
            }
            return 1;
        }, &result);
        return result;
    }();
    return std::ranges::any_of(segments.startAndSize, [&](auto segment) {
        return std::bit_cast<uintptr_t>(address) - segment.first < segment.second;
    });
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
#if defined(BUN_HARDENED)
        const CallSiteCache::Entry* site = findReturnSiteInRun(*m_cache, *m_image, cursor);
#else
        const CallSiteCache::Entry* site = findReturnSite(*m_cache, *m_image, removeCodePtrTag(cursor.rawPC));
#endif
        if (!site)
            return nullptr;
        enterFrame(cursor, *site);
        if (site->isStubReturnSite() && isAdapter(site->stubFrameKind))
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

LogicalFrame innermostLogicalFrame(const Image& image, InlineFrameReader& reader, const LogicalFrameSite& site)
{
    if (!site.inlineFrame)
        return { image.rootFunctionOf(site.code.index()), site.callSite, 0, false, site.isTailDeleted };
    const InlineFrameRecord& record = reader.at(image, site.code.index(), site.inlineFrame);
    return { record.function, site.callSite, site.inlineFrame, record.isConstruct, site.isTailDeleted };
}

LogicalFrame callerOfInlinedFrame(const Image& image, InlineFrameReader& reader, CompiledFunctionID code, const LogicalFrame& frame)
{
    ASSERT(frame.inlineFrame);
    InlineFrameRecord record = reader.at(image, code.index(), frame.inlineFrame);
    return innermostLogicalFrame(image, reader, { code, record.callSite, record.parent, record.isTailCall });
}

bool isEntrypointOf(FunctionID candidate, FunctionID sourceFunction)
{
    FunctionDescription description(candidate);
    return description.visibility() != TraceVisibility::Visible && description.sourceFunction() == sourceFunction;
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

bool CompiledFrameIterator::RunEnd::constructs(FunctionID outermost) const
{
    if (!isConstruct)
        return false;
    CalleeBits callee = adapterFrame->callee();
    return callee.isCell() && isClosureOf(callee.asCell(), outermost);
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

const CallSiteCache::Entry* CompiledFrameIterator::findFunctionFrame(Cursor& cursor, Exit& exit, RunEnd& runEnd)
{
    for (;;) {
#if defined(BUN_HARDENED)
        const CallSiteCache::Entry* site = findReturnSiteInRun(*m_cache, *m_image, cursor);
#else
        const CallSiteCache::Entry* site = findReturnSite(*m_cache, *m_image, removeCodePtrTag(cursor.rawPC));
#endif
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
        case StubFrameKind::ArgumentList:
        case StubFrameKind::HostCall:
        case StubFrameKind::BelowSpilledArguments:
        case StubFrameKind::AtStubEntry:
            break;
        case StubFrameKind::FirstOfTwoReads:
            cursor.isAtFirstReadSite = true;
            break;
        case StubFrameKind::GeneratorBody:
            cursor.generatorStubFrame = cursor.callFrame;
            break;
        case StubFrameKind::AsyncGeneratorBody:
            cursor.asyncGeneratorStubFrame = cursor.callFrame;
            cursor.asyncGeneratorBodyFrame = cursor.calleeFrame;
            break;
        case StubFrameKind::EngineCallee:
            cursor.engineCalleeStubFrame = cursor.callFrame;
            break;
        case StubFrameKind::Adapter:
        case StubFrameKind::HandlerOfJob:
            runEnd.adapterFrame = cursor.callFrame;
            leaveRun(cursor, exit);
            return nullptr;
        case StubFrameKind::ConstructAdapter:
            runEnd.adapterFrame = cursor.callFrame;
            runEnd.isConstruct = true;
            leaveRun(cursor, exit);
            return nullptr;
        case StubFrameKind::SavedFrameAdapter:
            runEnd.adapterFrame = cursor.callFrame;
            runEnd.resumesSavedFrame = true;
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
    m_frame = { m_instance, m_cursor.callFrame, removeCodePtrTag(m_cursor.rawPC), entry.code, entry.innermostFunction, site, entry.site.inlineFrame, entry.innermostVisibility, entry.site.isTailCall && m_calleeWasEntered };
    m_isInlinedConstructCall = entry.innermostIsInlinedConstructCall;
    m_hasReadFrame = true;
}

void CompiledFrameIterator::advance()
{
    if (m_isAtEnd)
        return;
    if (!frame().inlineFrame) {
        advanceToCallerMachineFrame();
        return;
    }
    LogicalFrame current { m_frame.function, m_frame.callSite, m_frame.inlineFrame, m_isInlinedConstructCall, m_frame.isTailDeleted };
    LogicalFrame caller = callerOfInlinedFrame(*m_image, m_inlineFrames, m_frame.code, current);
    m_frame.function = caller.function;
    m_frame.callSite = caller.callSite;
    m_frame.inlineFrame = caller.inlineFrame;
    m_frame.isTailDeleted = caller.isTailDeleted;
    m_frame.visibility = FunctionDescription(caller.function).visibility();
    m_isInlinedConstructCall = caller.isInlinedConstructCall;
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
        const CallSiteCache::Entry* entry = findCallerFunctionFrame(cursor, exit, runEnd);
        if (!entry)
            return runEnd.constructs(source);
        if (entry->site.isConstruct)
            return true;
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

bool isCallerCompiled(VM& vm, const CallFrame* calleeFrame)
{
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

static SUPPRESS_ASAN std::optional<SampledTop> findTopBelowNativeCode(const Image& image, const SampledRegisters& registers, CallFrame* topCallFrame, const SuspendedThreadStack& stack)
{
    auto* frame = static_cast<CallFrame*>(registers.framePointer);
    if (registers.linkRegister && stack.contains(frame)
        && removeCodePtrTag(reinterpret_cast<const CallerFrameAndPC*>(frame)->returnPC) != registers.linkRegister
        && image.locate(registers.linkRegister).runsCompiledCode())
        return SampledTop { frame, registers.linkRegister, nullptr };
    const void* pc = registers.pc;
    while (stack.contains(frame)) {
        auto* record = reinterpret_cast<const CallerFrameAndPC*>(frame);
        const void* returnAddress = removeCodePtrTag(record->returnPC);
        CodeLocation returnSite = image.locate(returnAddress);
        if (frame == topCallFrame && !isOperationReturnSite(returnSite))
            return SampledTop { topCallFrame, pc, nullptr };
        if (returnSite.runsCompiledCode())
            return sampledTopAtReturnSite(image, returnSite, returnAddress, stackPointerOfCaller(frame), record->callerFrame);
        if (record->callerFrame <= frame)
            break;
        pc = returnAddress;
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
