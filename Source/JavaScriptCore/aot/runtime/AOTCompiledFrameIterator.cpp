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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool isAdapter(StubFrameKind kind)
{
    switch (kind) {
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::SavedFrameAdapter:
        return true;
    case StubFrameKind::Plain:
    case StubFrameKind::EngineCallee:
    case StubFrameKind::HostCall:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::ArgumentList:
    case StubFrameKind::FirstOfTwoReads:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool entersCallee(StubFrameKind kind)
{
    return kind == StubFrameKind::HostCall || kind == StubFrameKind::EngineCallee;
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

static bool hasZombieCallee(VM& vm, const CallFrame* callFrame, const void* returnAddress)
{
    CallFrame* callee = vm.topCallFrame;
    return callee && callee->callerFrameOrEntryFrame() == callFrame && removeCodePtrTag(callee->rawReturnPC()) == returnAddress && callee->isZombieFrame();
}

static SUPPRESS_ASAN Instance* findEnclosingAdapterInstance(const Image& image, CallFrame* callFrame, const void* pc)
{
    for (MachineFrameIterator<TrustedStack> it(image, callFrame, pc); it; ++it) {
        const MachineFrame& frame = *it;
        if (frame.code.kind == CodeKind::Stub && isAdapter(frame.code.stubFrameKind))
            return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(frame.callFrame) + AdapterFrame::offsetOfInstance);
    }
    return nullptr;
}

static Instance* tryFindInstance(const VMState& state, CallFrame* callFrame, const void* pc)
{
    if (state.instances.size() != 1)
        return findEnclosingAdapterInstance(*state.image, callFrame, pc);
#if ASSERT_ENABLED
    Instance* adapterInstance = findEnclosingAdapterInstance(*state.image, callFrame, pc);
    ASSERT(!adapterInstance || adapterInstance == state.instances.first());
#endif
    return state.instances.first();
}

static CallSiteIndex applyCallSiteOverride(const Image& image, const Instance& instance, const void* callFrame, CompiledFunctionID code, CallSiteIndex site)
{
    for (auto* callSiteOverride = instance.runtime.exceptions.callSiteOverrides; callSiteOverride; callSiteOverride = callSiteOverride->previous()) {
        if (callSiteOverride->frame() != callFrame)
            continue;
        auto item = image.spreadSiteAt(code.index(), site, callSiteOverride->item());
        return item ? *item : site;
    }
    return site;
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
std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreate(VM& vm, CallFrame* callFrame, const void* returnAddress, EntryFrame* entryFrame, OperationState operationState)
{
    if (!vm.hasCompiledCode() || !callFrame || !returnAddress)
        return std::nullopt;
    const VMState& state = vm.aotState();
    const void* pc = removeCodePtrTag(returnAddress);
    bool calleeWasEntered = operationState == OperationState::HasCalled;
    if (!state.callSiteCache->find(pc)) {
        CodeLocation location = state.image->locate(pc);
        if (!location.runsCompiledCode())
            return std::nullopt;
        if (location.kind == CodeKind::Stub)
            calleeWasEntered = (calleeWasEntered || entersCallee(location.stubFrameKind)) && !hasZombieCallee(vm, callFrame, pc);
    }
    return CompiledFrameIterator(vm, Cursor { callFrame, const_cast<void*>(returnAddress), entryFrame }, calleeWasEntered);
}

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreateAtTop(VM& vm)
{
    if (!isTopFrameCompiled(vm))
        return std::nullopt;
    auto* record = reinterpret_cast<const CallerFrameAndPC*>(vm.topCallFrame);
    return tryCreate(vm, record->callerFrame, record->returnPC, vm.topEntryFrame);
}

void CompiledFrameIterator::leaveRun(Cursor& cursor, Exit& exit)
{
    EntryFrame* entryFrameBefore = cursor.entryFrame;
    void* returnPC = cursor.callFrame->rawReturnPC();
    exit.callFrame = cursor.callFrame->callerFrame(cursor.entryFrame);
    exit.entryFrame = cursor.entryFrame;
    bool crossedEntryFrame = cursor.entryFrame != entryFrameBefore;
    exit.returnPC = crossedEntryFrame ? nullptr : returnPC;
    exit.crossedEntryFrame = crossedEntryFrame;
}

bool CompiledFrameIterator::passOverFrame(Cursor& cursor, Exit& exit)
{
    if (cursor.callFrame->callerFrameOrEntryFrame() == cursor.entryFrame) {
        leaveRun(cursor, exit);
        return false;
    }
    cursor.rawPC = cursor.callFrame->rawReturnPC();
    cursor.callFrame = cursor.callFrame->callerFrame();
    return true;
}

const CallSiteCache::Entry* CompiledFrameIterator::findFunctionFrame(Cursor& cursor, Exit& exit, RunEnd& runEnd)
{
    for (;;) {
        const void* pc = removeCodePtrTag(cursor.rawPC);
        if (auto* entry = m_cache->find(pc))
            return entry;
        CodeLocation location = m_image->locate(pc);
        switch (location.kind) {
        case CodeKind::Function:
            return m_cache->resolve(*m_image, pc);
        case CodeKind::NotInImage:
        case CodeKind::RegExp:
        case CodeKind::WebAssembly:
        case CodeKind::FFI:
            exit = { cursor.callFrame, cursor.rawPC, cursor.entryFrame, false };
            return nullptr;
        case CodeKind::Stub:
            break;
        }
        switch (location.stubFrameKind) {
        case StubFrameKind::Plain:
        case StubFrameKind::ArgumentList:
        case StubFrameKind::HostCall:
            break;
        case StubFrameKind::FirstOfTwoReads:
            cursor.isAtFirstReadSite = true;
            break;
        case StubFrameKind::GeneratorBody:
            cursor.generatorStubFrame = cursor.callFrame;
            break;
        case StubFrameKind::EngineCallee:
            cursor.engineCalleeStubFrame = cursor.callFrame;
            break;
        case StubFrameKind::Adapter:
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
    if (entry)
        readMachineFrame(*entry);
    else
        m_isAtEnd = true;
}

void CompiledFrameIterator::readMachineFrame(const CallSiteCache::Entry& entry)
{
    m_callSite = entry;
    const void* pc = removeCodePtrTag(m_cursor.rawPC);
    Instance* instance = m_frame.instance ? m_frame.instance : tryFindInstance(m_vm->aotState(), m_cursor.callFrame, pc);
    RELEASE_ASSERT(instance);
    CallSiteIndex site = m_cursor.isAtFirstReadSite && entry.site.firstReadSite ? *entry.site.firstReadSite : entry.site.callSite;
    site = applyCallSiteOverride(*m_image, *instance, m_cursor.callFrame, entry.code, site);
    m_frame = { instance, m_cursor.callFrame, pc, entry.code, entry.innermostFunction, site, entry.site.inlineFrame, entry.innermostVisibility, entry.site.isTailCall && m_calleeWasEntered };
    m_isInlinedConstructCall = entry.innermostIsInlinedConstructCall;
}

void CompiledFrameIterator::advance()
{
    if (m_isAtEnd)
        return;
    if (!m_frame.inlineFrame) {
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
    const void* returnAddress = removeCodePtrTag(reinterpret_cast<const CallerFrameAndPC*>(frameRecord)->returnPC);
    return isOperationReturnSite(vm.aotState().image->locate(returnAddress));
}

bool isTopFrameCompiled(VM& vm)
{
    return isOperationFrameRecord(vm, vm.topCallFrame);
}

bool isCallerCompiled(VM& vm, const CallFrame* calleeFrame)
{
    return vm.hasCompiledCode() && vm.aotState().image->locate(removeCodePtrTag(calleeFrame->rawReturnPC())).runsCompiledCode();
}

static SUPPRESS_ASAN SampledTop findTopInCompiledCode(const Image& image, const SampledRegisters& registers, const CodeLocation& top, const SuspendedThreadStack& stack)
{
    SampledTop result { static_cast<CallFrame*>(registers.framePointer), registers.pc, nullptr };
    if (!stack.contains(registers.framePointer))
        return result;
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
        if (!callerIsFunction || caller.function != top.function)
            result.callerPCOfFramelessTop = callReturnAddress;
    } else if (callerIsFunction)
        result.pc = callReturnAddress;
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
            return SampledTop { record->callerFrame, returnAddress, nullptr };
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

CallSiteOverride::CallSiteOverride(Instance& instance, const void* frame)
    : m_instance(instance)
    , m_previous(instance.runtime.exceptions.callSiteOverrides)
    , m_frame(frame)
{
    instance.runtime.exceptions.callSiteOverrides = this;
}

CallSiteOverride::~CallSiteOverride()
{
    RELEASE_ASSERT(m_instance.runtime.exceptions.callSiteOverrides == this);
    m_instance.runtime.exceptions.callSiteOverrides = m_previous;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
