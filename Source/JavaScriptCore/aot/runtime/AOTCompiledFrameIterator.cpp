/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiledFrame.h"

#if ENABLE(AOT)

#include "AOTAsyncResumption.h"
#include "AOTBuiltinIDs.h"
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
#include "AOTProgramTables.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTSavedFrame.h"
#include "AOTShownFrameFilter.h"
#include "AOTStackTrace.h"
#include "AOTStubBlob.h"
#include "AOTStubFrames.h"
#include "CallFrameInlines.h"
#include "CodeBlock.h"
#include "ExecutableAllocator.h"
#include "FunctionExecutable.h"
#include "JSCJSValueInlines.h"
#include "JSCellInlines.h"
#include "LLIntData.h"
#include "LLIntPCRanges.h"
#include "SourceOrigin.h"
#include "StackAlignment.h"
#include "StackVisitor.h"
#include "StrongInlines.h"
#include "VM.h"
#include "VMEntryRecord.h"
#include "WasmCodeInImage.h"
#include <algorithm>
#include <array>
#include <bit>
#include <utility>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool entersCallee(StubFrameKind kind)
{
    return kind == StubFrameKind::HostCall || kind == StubFrameKind::EngineCallee;
}

static ALWAYS_INLINE const CallSiteCache::Entry* findReturnSite(CallSiteCache& cache, const Image& image, const void* pc)
{
    if (const CallSiteCache::Entry* site = cache.find(pc)) [[likely]]
        return site;
    return cache.resolve(image, pc);
}

template<typename Cursor>
static ALWAYS_INLINE const CallSiteCache::Entry* findReturnSiteInRun(CallSiteCache& cache, const Image& image, const Cursor& cursor)
{
    const void* pc = removeReturnAddressSignature(cursor.rawPC);
    if (const CallSiteCache::Entry* site = cache.find(pc)) [[likely]]
        return site;
    return resolveReturnAddressInRun(cache, image, pc, cursor.calleeFrame);
}

static ALWAYS_INLINE const CallSiteCache::Entry* findReturnSite(VM& vm, const void* pc)
{
    const VMState& state = vm.aotState();
    return findReturnSite(*state.callSiteCache, *state.image, pc);
}

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

static bool isBehindFaultSite(const Image& image, const void* address)
{
    auto hasSite = [&](FaultSiteKind kind, size_t size) {
        std::optional<FaultSite> site = image.faultSiteAt(static_cast<const uint8_t*>(address) - size);
        return site && site->kind == kind;
    };
    return hasSite(FaultSiteKind::NullCheck, sizeOfNearCall) || (hasFaultingLoopPolls && hasSite(FaultSiteKind::LoopPoll, sizeOfLoopPoll));
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
        return "points into compiled code, but not to the end of a call"_s;
    case CodeKind::NotInImage:
        if (isInCodeOfEngine(returnAddress))
            return { };
        [[fallthrough]];
    case CodeKind::RegExp:
    case CodeKind::WebAssembly:
    case CodeKind::FFI:
        return "points neither into a compiled function nor into the engine's code"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NEVER_INLINE void checkReturnAddressInRun(const Image& image, const void* returnAddress, const CallFrame* callee)
{
    ASCIILiteral fault = faultOfReturnAddressInRun(image, returnAddress);
    if (fault.isNull()) [[likely]]
        return;
    UNUSED_PARAM(callee);
    RuntimeCheckFailure failure("stack walk failed"_s);
    failure.detail("the return address of a frame ", fault);
    failure.report();
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
    return callee && callee->callerFrameOrEntryFrame() == callFrame && callee->returnPCForInspection() == returnAddress && callee->isZombieFrame();
}

SUPPRESS_ASAN Instance* CompiledFrameIterator::findEnclosingAdapterInstance(Cursor cursor, VMState* stateThatRemembersRun) const
{
    const CallFrame* lowestFrame = cursor.framePointer;
    Exit exit;
    do {
        const CallSiteCache::Entry* site = findReturnSiteInRun(*m_cache, *m_image, cursor);
        if (!site)
            return nullptr;
        enterFrame(cursor, *site);
        if (site->isStubReturnSite() && isAdapterFrame(site->stubFrameKind)) {
            Instance* instance = *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(cursor.callFrame) + AdapterFrame::offsetOfInstance);
            if (stateThatRemembersRun && cursor.callFrame->callerFrameOrEntryFrame() == cursor.entryFrame)
                stateThatRemembersRun->instanceOfRecentRun = { stateThatRemembersRun->entriesWithSeveralInstances, cursor.entryFrame, instance, lowestFrame, cursor.callFrame };
            return instance;
        }
        if (stateThatRemembersRun) {
            if (Instance* known = knownInstanceOfRun(*stateThatRemembersRun, cursor.entryFrame, cursor.framePointer)) {
                stateThatRemembersRun->instanceOfRecentRun.lowestFrame = lowestFrame;
                return known;
            }
        }
    } while (passOverFrame(cursor, exit));
    return nullptr;
}

Instance* CompiledFrameIterator::knownInstanceOfRun(const VMState& state, EntryFrame* entryFrame, const CallFrame* framePointer)
{
    const VMState::InstanceOfRun& known = state.instanceOfRecentRun;
    return known.entries == state.entriesWithSeveralInstances && known.entryFrame == entryFrame && known.lowestFrame <= framePointer && framePointer <= known.adapterFrame ? known.instance : nullptr;
}

NEVER_INLINE Instance* CompiledFrameIterator::findInstanceAmongSeveral() const
{
    VMState& state = m_vm->aotState();
    if (Instance* known = knownInstanceOfRun(state, m_cursor.entryFrame, m_cursor.framePointer))
        return known;
    return findEnclosingAdapterInstance(m_cursor, &state);
}

Instance* CompiledFrameIterator::tryFindInstance() const
{
    const VMState& state = m_vm->aotState();
    if (state.instances.size() != 1)
        return findInstanceAmongSeveral();
#if ASSERT_ENABLED
    static std::atomic<unsigned> questions;
    if (!(questions.fetch_add(1, std::memory_order_relaxed) % 1024)) {
        Instance* adapterInstance = findEnclosingAdapterInstance(m_cursor, nullptr);
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
    if (FunctionDescription(function).isHelperOfCompiler()) [[unlikely]]
        return compilerHelpersModule;
    if (instance->vm->aotState().image->isObfuscated()) {
        ModuleID module = moduleShownByObfuscatedImage(function);
        return module ? sourceURLOf(module) : String();
    }
    auto position = findPosition(*this);
    return position && position->source ? sourceURLOf(position->shown()) : String();
}

LineColumn CompiledFrame::lineColumn() const
{
    if (instance->vm->aotState().image->isObfuscated())
        return obfuscatedPositionOf(TraceFrame { code, function, callSite, !!inlineFrame, false });
    auto position = findPosition(*this);
    return position ? position->shown().lineColumn : LineColumn { };
}

SourceOrigin CompiledFrame::sourceOrigin() const
{
    auto position = findPosition(*this);
    return position && position->source ? modulesOf(*instance).sourceOrigin(position->source) : SourceOrigin();
}

bool CompiledFrame::isOfBuiltin() const
{
    return FunctionDescription(function).builtinKind() != StackTraceBuiltinKind::None;
}

bool CompiledFrame::runsCodeOfFile() const
{
    if (isOfBuiltin())
        return false;
    if (visibility == TraceVisibility::Visible)
        return true;
    return sourceFunctionOf(function) == function && !functionKindData(function).isInRuntimeModule;
}

bool findCallerOrigin(CompiledFrameIterator& frames, SourceOrigin& origin)
{
    for (; !frames.isAtEnd(); frames.advance()) {
        const CompiledFrame& frame = frames.frame();
        if (frames.isAmongFramesThatWait() || !frame.runsCodeOfFile())
            continue;
        if (frame.isTailDeleted && !(frame.visibility == TraceVisibility::Visible && frames.isConstructCall()))
            continue;
        origin = frame.sourceOrigin();
        return true;
    }
    return false;
}

bool CompiledFrame::isActivationOf(JSCell* cell) const
{
    return isClosureOf(cell, function);
}

std::optional<ModuleID> CompiledFrame::evaluatingModule() const
{
    if (FunctionDescription(function).codeType() != ModuleCode && !functionKindData(function).isCommonJSModule)
        return std::nullopt;
    return sourceModule();
}

std::optional<ModuleID> CompiledFrame::sourceModule() const
{
    auto position = findPosition(*this);
    return position && position->source ? std::optional(position->source) : std::nullopt;
}

void forEachEvaluatingModuleOnStack(VM& vm, CallFrame* callFrame, const ScopedLambda<void(ModuleID)>& functor)
{
    StackVisitor::visit(callFrame, vm, [&](auto& visitor) {
        if constexpr (std::remove_cvref_t<decltype(visitor)>::visitsCompiledFrames) {
            if (visitor->isCompiledFrame()) {
                const CompiledFrame& frame = visitor->compiledFrame();
                if (std::optional<ModuleID> module = frame.evaluatingModule())
                    functor(*module);
            }
        }
        return IterationStatus::Continue;
    });
}

std::optional<ModuleID> Modules::callingModule() const
{
    VM& vm = *m_instance.vm;
    std::optional<ModuleID> module;
    if (!vm.topCallFrame)
        return module;
    StackVisitor::visit(vm.topCallFrame, vm, [&](auto& visitor) {
        if constexpr (std::remove_cvref_t<decltype(visitor)>::visitsCompiledFrames) {
            if (visitor->isCompiledFrame())
                module = visitor->compiledFrame().sourceModule();
        }
        return IterationStatus::Done;
    });
    return module;
}

bool Modules::isHostRequestStale() const
{
    VM& vm = *m_instance.vm;
    if (!vm.topCallFrame)
        return false;
    bool isInterpreted = false;
    bool isCompiledCodeEnteredSince = false;
    StackVisitor::visit(vm.topCallFrame, vm, [&](auto& visitor) {
        if constexpr (std::remove_cvref_t<decltype(visitor)>::visitsCompiledFrames) {
            if (visitor->isCompiledFrame()) {
                const CompiledFrame& frame = visitor->compiledFrame();
                if (frame.visibility != TraceVisibility::Visible)
                    return IterationStatus::Continue;
                isCompiledCodeEnteredSince = m_currentHostRequest.wasMadeBeforeEntering(frame.callFrame);
                return IterationStatus::Done;
            }
        }
        switch (visitor->codeType()) {
        case StackVisitor::Frame::CodeType::Function:
            if (static_cast<FunctionExecutable*>(visitor->codeBlock()->ownerExecutable())->isPrivateBuiltinFunction())
                return IterationStatus::Continue;
            [[fallthrough]];
        case StackVisitor::Frame::CodeType::Eval:
        case StackVisitor::Frame::CodeType::Module:
        case StackVisitor::Frame::CodeType::Global:
            isInterpreted = true;
            return IterationStatus::Done;
        case StackVisitor::Frame::CodeType::Native:
        case StackVisitor::Frame::CodeType::Wasm:
            return IterationStatus::Continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    return isInterpreted || isCompiledCodeEnteredSince;
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

std::optional<TraceFrame> makeElidedConstructorFrame(const Image& image, FunctionID constructed)
{
    FunctionID function = sourceFunctionOf(constructed);
    if (FunctionDescription(function).visibility() != TraceVisibility::Visible)
        return std::nullopt;
    for (FunctionID candidate : { function, constructed }) {
        if (std::optional<uint32_t> code = image.imageFunctionOf(candidate))
            return TraceFrame { CompiledFunctionID(*code), function, CallSiteIndex(0), false, true, false, false, true };
    }
    return std::nullopt;
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

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreate(VM& vm, const Cursor& cursor, OperationState operationState)
{
    if (!vm.hasCompiledCode() || !cursor.framePointer || !cursor.rawPC)
        return std::nullopt;
    const void* pc = removeReturnAddressSignature(cursor.rawPC);
    const CallSiteCache::Entry* site = findReturnSite(vm, pc);
    if (!site || isInHostCallFrame(*site))
        return std::nullopt;
    bool calleeWasEntered = operationState == OperationState::HasCalled;
    if (site->isStubReturnSite())
        calleeWasEntered = site->stubFrameKind == StubFrameKind::PlainFunctionConstructMakesThis || ((calleeWasEntered || entersCallee(site->stubFrameKind)) && !hasZombieCallee(vm, cursor.framePointer, pc));
    return std::optional<CompiledFrameIterator>(std::in_place, vm, cursor, calleeWasEntered);
}

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreate(VM& vm, CallFrame* framePointer, const void* returnAddress, EntryFrame* entryFrame, OperationState operationState, void* stackPointer)
{
    return tryCreate(vm, Cursor { framePointer, const_cast<void*>(returnAddress), entryFrame, nullptr, stackPointer }, operationState);
}

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreateAtCallerOf(VM& vm, CallFrame* callee, EntryFrame* entryFrame)
{
    return tryCreate(vm, Cursor { callee->callerFrame(), callee->rawReturnPC(), entryFrame, callee, callerStackPointer(callee) }, OperationState::MayNotHaveCalled);
}

std::optional<CompiledFrameIterator> CompiledFrameIterator::tryCreateAtTop(VM& vm)
{
    if (!isTopFrameCompiled(vm))
        return std::nullopt;
    return tryCreateAtCallerOf(vm, vm.topCallFrame, vm.topEntryFrame);
}

const void* CompiledFrameIterator::topFrameReturnAddress(VM& vm)
{
    if (!isTopFrameCompiled(vm))
        return nullptr;
    CallFrame* callee = vm.topCallFrame;
    Cursor cursor { callee->callerFrame(), callee->rawReturnPC(), vm.topEntryFrame, callee, callerStackPointer(callee) };
    Exit exit;
    do {
        const void* pc = removeReturnAddressSignature(cursor.rawPC);
        const CallSiteCache::Entry* site = pc ? findReturnSite(vm, pc) : nullptr;
        if (!site)
            return nullptr;
        if (!site->isStubReturnSite())
            return pc;
        switch (site->stubFrameKind) {
        case StubFrameKind::Plain:
        case StubFrameKind::ReturnsAfterOperation:
        case StubFrameKind::BelowSpilledArguments:
        case StubFrameKind::AtStubEntry:
        case StubFrameKind::ModuleStart:
            break;
        case StubFrameKind::Adapter:
        case StubFrameKind::ConstructAdapter:
        case StubFrameKind::SavedFrameAdapter:
        case StubFrameKind::HandlerOfJob:
        case StubFrameKind::EngineCallee:
        case StubFrameKind::HostCall:
        case StubFrameKind::GeneratorBody:
        case StubFrameKind::AsyncGeneratorBody:
        case StubFrameKind::ArgumentList:
        case StubFrameKind::FirstOfTwoReads:
        case StubFrameKind::MethodRead:
        case StubFrameKind::PlainFunctionConstruct:
        case StubFrameKind::PlainFunctionConstructMakesThis:
        case StubFrameKind::ConstructAdapterMakesThis:
        case StubFrameKind::InHostCallFrame:
            return nullptr;
        }
        enterFrame(cursor, *site);
    } while (passOverFrame(cursor, exit));
    return nullptr;
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
    cursor.stackPointer = callerStackPointer(cursor.callFrame, cursor.bytesAboveReturnAddress);
    cursor.rawPC = cursor.callFrame->rawReturnPC();
    return true;
}

NEVER_INLINE bool CompiledFrameIterator::passOverMethodReadFrame(Cursor& cursor, Exit& exit)
{
    cursor.isAtFirstReadSite = true;
    return passOverFrame(cursor, exit);
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
        const Instance* instance = m_instance;
        if (!instance) [[unlikely]] {
            instance = tryFindInstance();
            site = findReturnSiteInRun(*m_cache, *m_image, cursor);
            RELEASE_ASSERT(site && site->isStubReturnSite());
        }
        if (instance)
            cursor.isAmongFramesThatWait |= cursor.callFrame == std::bit_cast<const CallFrame*>(frameWithAwaitingCallers(*instance));
        switch (site->stubFrameKind) {
        case StubFrameKind::InHostCallFrame:
            RELEASE_ASSERT_NOT_REACHED();
        case StubFrameKind::Plain:
        case StubFrameKind::ReturnsAfterOperation:
        case StubFrameKind::ArgumentList:
        case StubFrameKind::HostCall:
        case StubFrameKind::BelowSpilledArguments:
        case StubFrameKind::AtStubEntry:
        case StubFrameKind::ModuleStart:
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
        case StubFrameKind::PlainFunctionConstructMakesThis:
            cursor.plainFunctionConstructFrame = cursor.callFrame;
            cursor.constructFrameMakesThis = true;
            break;
        case StubFrameKind::Adapter:
        case StubFrameKind::HandlerOfJob:
        case StubFrameKind::ConstructAdapter:
        case StubFrameKind::ConstructAdapterMakesThis:
        case StubFrameKind::SavedFrameAdapter:
            runEnd = { cursor.callFrame, isConstructAdapterFrame(site->stubFrameKind), site->stubFrameKind == StubFrameKind::SavedFrameAdapter, site->stubFrameKind == StubFrameKind::HandlerOfJob, site->stubFrameKind == StubFrameKind::ConstructAdapterMakesThis };
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
    bool isAtRead = m_cursor.isAtFirstReadSite && entry.site.firstReadSite;
    CallSiteIndex site = isAtRead ? *entry.site.firstReadSite : entry.site.callSite;
    m_frame = { m_instance, m_cursor.callFrame, removeReturnAddressSignature(m_cursor.rawPC), entry.code, entry.innermostFunction, site, entry.site.inlineFrame, entry.innermostVisibility, entry.site.isTailDeleted(m_calleeWasEntered && !isAtRead) };
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

void CompiledFrameIterator::collectElidedConstructorFrame()
{
    if (m_isAtEnd) {
        if (m_runEnd.makesThis) [[unlikely]]
            collectAdapterConstructorFrame();
        return;
    }
    if (!m_cursor.constructFrameMakesThis || !m_callSite.site.isConstruct())
        return;
    if (FunctionID constructed = functionConstructedAt(*m_image, m_callSite, m_cursor.plainFunctionConstructFrame))
        m_elidedConstructorFrame = makeElidedConstructorFrame(*m_image, constructed);
}

void CompiledFrameIterator::collectAdapterConstructorFrame()
{
    FunctionID constructed = m_runEnd.constructedFunction();
    if (!constructed)
        return;
    m_elidedConstructorFrame = makeElidedConstructorFrame(*m_image, constructed);
    if (!m_elidedConstructorFrame)
        return;
    m_hasNoMachineFrame = true;
    m_isAtEnd = false;
    m_instance = *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(m_runEnd.adapterFrame) + AdapterFrame::offsetOfInstance);
    m_frame = { m_instance, m_runEnd.adapterFrame, removeReturnAddressSignature(m_cursor.rawPC), { }, m_elidedConstructorFrame->function, CallSiteIndex(0) };
    m_hasReadFrame = true;
}

void CompiledFrameIterator::advance()
{
    if (m_isAtEnd)
        return;
    if (m_elidedConstructorFrame) [[unlikely]] {
        m_elidedConstructorFrame = std::nullopt;
        m_isAtEnd = m_hasNoMachineFrame;
        return;
    }
    frame();
    if (!m_frame.inlineFrame) {
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
    m_cursor.constructFrameMakesThis = false;
    m_cursor.isAmongFramesThatWait |= m_instance && m_cursor.callFrame && m_cursor.callFrame == std::bit_cast<const CallFrame*>(frameWithAwaitingCallers(*m_instance));
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

bool isOperationFrameRecord(VM& vm, const CallFrame* frameRecord)
{
    if (!vm.hasCompiledCode() || !frameRecord)
        return false;
    const CallSiteCache::Entry* site = findReturnSite(vm, removeReturnAddressSignature(reinterpret_cast<const CallerFrameAndPC*>(frameRecord)->returnPC));
    return site && isOperationReturnSite(*site);
}

std::span<const uint8_t> compiledCodeOf(VM& vm)
{
    return vm.hasCompiledCode() ? vm.aotState().image->code() : std::span<const uint8_t> { };
}

bool isCallStubReturnSite(VM& vm, const void* returnPC)
{
    const CallSiteCache::Entry* site = vm.aotState().callSiteCache->find(returnPC);
    return site && site->isStubReturnSite() && entersCallee(site->stubFrameKind);
}

bool isTopFrameCompiled(VM& vm)
{
    return isOperationFrameRecord(vm, vm.topCallFrame);
}

bool isCallerCompiled(VM& vm, const CallFrame* calleeFrame, EntryFrame* entryFrame)
{
    if (calleeFrame->callerFrameOrEntryFrame() == entryFrame)
        return isOperationFrameRecord(vm, vmEntryRecord(entryFrame)->prevTopCallFrame());
    return vm.hasCompiledCode() && findReturnSite(vm, calleeFrame->returnPCForInspection());
}

static SUPPRESS_ASAN ImageFrame frameAtSampledReturnSite(const Image& image, const SuspendedThreadStack& stack, const CodeLocation& site, const void* stackPointer)
{
    if (site.kind != CodeKind::Function)
        return frameAtStubReturnSite(site.stubFrameKind);
    ImageFrame frame = image.frameAtReturnOffset(site.function, site.offset);
    // See leavesCallerWithoutFrame(). To throw or to poll, such a function gets a frame record.
    if (isX86_64() && !image.hasFrameAtReturnOffset(site.function, site.offset) && stack.contains(stackPointer) && image.locate(*static_cast<void* const*>(stackPointer)).runsCompiledCode())
        frame.calleeSavesStart |= ImageFrame::empty().calleeSavesStart;
    return frame;
}

static bool isAlignedForCall(const void* stackPointer)
{
    return !(std::bit_cast<uintptr_t>(stackPointer) % stackAlignmentBytes());
}

// What lies behind a byte that differs may not be mapped.
static bool isAt(const void* pc, std::initializer_list<uint8_t> bytes)
{
    auto* code = static_cast<const uint8_t*>(pc);
    for (uint8_t byte : bytes) {
        if (*code++ != byte)
            return false;
    }
    return true;
}

static bool isAtReturn(const void* pc) { return isAt(pc, { 0xc3 }); }
static bool isAtMoveOfStackPointerToFramePointer(const void* pc) { return isAt(pc, { 0x48, 0x89, 0xe5 }); }
static bool isAtMoveOfFramePointerToStackPointer(const void* pc) { return isAt(pc, { 0xc9 }) || isAt(pc, { 0x48, 0x89, 0xec }); }

static bool isAtSaveOfRegisterToTopOfStack(const void* pc)
{
    auto* code = static_cast<const uint8_t*>(pc);
    return (code[0] | 0x04) == 0x4c && code[1] == 0x89 && (code[2] & 0xc7) == 0x04 && code[3] == 0x24 && !isAtReturn(code + 4);
}

// See emitSpillArgumentRegisters() and emitDropSpilledArgumentRegisters().
static constexpr unsigned sizeOfDropOfSpilledArguments = 4;
static bool isAtDropOfSpilledArguments(const void* pc) { return isAt(pc, { 0x48, 0x83, 0xc4, sizeOfSpilledArgumentRegisters }); }

static std::optional<unsigned> numberOfPushesAheadOfPushOfReturnAddress(const void* pc)
{
    auto* code = static_cast<const uint8_t*>(pc);
    if (isAtDropOfSpilledArguments(code))
        code += sizeOfDropOfSpilledArguments;
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count) {
        if (isAt(code, { 0x41, 0x53 }))
            return count;
        code += *code == 0x41;
        if (*code++ >> 3 != 0x50 >> 3)
            break;
    }
    return std::nullopt;
}

// By the pushes, pops and additions from here on that set up a frame, or take it down.
static ptrdiff_t bytesThatStackPointerIsAboutToMove(const void* pc)
{
    auto* code = static_cast<const uint8_t*>(pc);
    ptrdiff_t result = 0;
    while (true) {
        ptrdiff_t bytes;
        if (const uint8_t* opcode = code + (*code == 0x41); *opcode >> 4 == 0x5) {
            bytes = *opcode & 8 ? sizeof(void*) : -sizeof(void*);
            code = opcode + 1;
            if (bytes > 0 && numberOfPushesAheadOfPushOfReturnAddress(code))
                return result;
        } else if (code[0] == 0x48 && (code[1] == 0x83 || code[1] == 0x81) && (code[2] == 0xc4 || code[2] == 0xec)) {
            bool hasByteImmediate = code[1] == 0x83;
            bytes = hasByteImmediate ? static_cast<int8_t>(code[3]) : WTF::unalignedLoad<int32_t>(code + 3);
            if (code[2] == 0xec)
                bytes = -bytes;
            code += hasByteImmediate ? 4 : 7;
        } else
            return result;
        if (result && (result > 0) != (bytes > 0))
            return result;
        result += bytes;
    }
}

struct CallOfCodeWithoutFrame {
    const void* returnAddress { nullptr };
    void* const* wordAboveReturnAddress { nullptr };
    bool hasSpilledArguments { false };
};

static SUPPRESS_ASAN CallOfCodeWithoutFrame findCallOfCodeWithoutFrame(const Image& image, const SampledRegisters& registers, const CodeLocation& top, const SuspendedThreadStack& stack)
{
    auto* word = static_cast<void* const*>(registers.stackPointer);
    if (isARM64()) {
        const void* linkRegister = removeReturnAddressSignature(registers.linkRegister);
        if (linkRegister == removeReturnAddressSignature(static_cast<const CallerFrameAndPC*>(registers.framePointer)->returnPC))
            return { };
        return { linkRegister, word };
    }
    if (!top.runsCompiledCode()) {
        word += stack.contains(word) && *word == registers.framePointer;
        if (!stack.contains(word))
            return { };
        return { *word, word + 1 };
    }
    auto* code = static_cast<const uint8_t*>(registers.pc);
    auto hasDroppedSpilledArguments = [&](unsigned bytesBehindDrop) {
        return top.offset >= sizeOfDropOfSpilledArguments + bytesBehindDrop && isAtDropOfSpilledArguments(code - sizeOfDropOfSpilledArguments - bytesBehindDrop);
    };
    if (auto pushes = numberOfPushesAheadOfPushOfReturnAddress(code)) {
        if (*pushes)
            return { registers.macroScratchRegister, word + numberOfArgumentGPRs - *pushes };
        return { registers.macroScratchRegister, word, !hasDroppedSpilledArguments(0) };
    }
    // Nothing may have been written there since the room was made.
    if (isAtMoveOfFramePointerToStackPointer(code) || isAtSaveOfRegisterToTopOfStack(code))
        return { };
    if (top.kind == CodeKind::Function && bytesThatStackPointerIsAboutToMove(code) && !isAtMoveOfStackPointerToFramePointer(code + 1))
        return { };
    word += isAtMoveOfStackPointerToFramePointer(code);
    if (!stack.contains(word))
        return { };
    bool spillsArguments = top.kind == CodeKind::Function && image.frameAtReturnOffset(top.function, top.offset).liesBelowSpilledArguments();
    constexpr unsigned sizeOfPushOrPopOfReturnAddress = 2;
    bool isAboutToSpill = isAt(code, { 0x41, 0x5b }) && numberOfPushesAheadOfPushOfReturnAddress(code + sizeOfPushOrPopOfReturnAddress).value_or(0);
    return { *word, word + 1, spillsArguments && !isAboutToSpill && !hasDroppedSpilledArguments(sizeOfPushOrPopOfReturnAddress) };
}

static SUPPRESS_ASAN std::optional<SampledTop> findTopWithoutFrameRecord(const Image& image, const SampledRegisters& registers, const CodeLocation& top, const SuspendedThreadStack& stack)
{
    const ImageFrame& frame = image.frameOf(top.function);
    if (frame.hasFrameRecord() || !stack.contains(registers.stackPointer))
        return std::nullopt;
    if (numberOfPushesAheadOfPushOfReturnAddress(registers.pc))
        return std::nullopt;
    uintptr_t stackPointer = std::bit_cast<uintptr_t>(registers.stackPointer);
    ptrdiff_t bytesToMove = bytesThatStackPointerIsAboutToMove(registers.pc);
    bool countsFromReturnAddress = bytesToMove > 0 || (!bytesToMove && !isAlignedForCall(registers.stackPointer));
    auto* record = std::bit_cast<const CallerFrameAndPC*>(stackPointer + bytesToMove + (countsFromReturnAddress ? -sizeof(void*) : frame.frameSizeInBytes()));
    if (!stack.contains(&record->returnPC) || !image.locate(record->returnPC).runsCompiledCode())
        return std::nullopt;
    return SampledTop { std::bit_cast<CallFrame*>(record), registers.pc, nullptr, static_cast<CallFrame*>(registers.framePointer) };
}

struct CalleeOfCall {
    bool names(const CodeLocation& site) const { return function && site.kind == CodeKind::Function && site.function == *function; }

    bool mayBeFunction { false };
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
    case StubFrameKind::ModuleStart:
        return true;
    case StubFrameKind::Plain:
    case StubFrameKind::ReturnsAfterOperation:
    case StubFrameKind::EngineCallee:
    case StubFrameKind::HostCall:
    case StubFrameKind::FirstOfTwoReads:
    case StubFrameKind::MethodRead:
    case StubFrameKind::BelowSpilledArguments:
    case StubFrameKind::PlainFunctionConstructMakesThis:
    case StubFrameKind::ConstructAdapterMakesThis:
    case StubFrameKind::AtStubEntry:
    case StubFrameKind::InHostCallFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Null if there is none. See generateArgumentThunks() and sizeOfMoveInFrontOfStub.
static const uint8_t* instructionBehindMoveOrJump(const uint8_t* code)
{
    if (*code == 0xeb)
        return code + 2 + static_cast<int8_t>(code[1]);
    if (*code == 0xe9)
        return code + 5 + WTF::unalignedLoad<int32_t>(code + 1);
    bool hasPrefix = *code >> 4 == 0x4;
    bool isWide = hasPrefix && (*code & 8);
    code += hasPrefix;
    uint8_t opcode = *code++;
    if (opcode >> 3 == 0xb8 >> 3)
        return code + (isWide ? 8 : 4);
    if (opcode >> 3 == 0x90 >> 3)
        return code;
    if (opcode != 0x31 && opcode != 0x87 && opcode != 0x89 && opcode != 0x8b && opcode != 0xc7)
        return nullptr;
    unsigned mode = *code >> 6;
    bool hasScaleIndexBase = mode != 3 && (*code & 7) == 4;
    return code + 1 + hasScaleIndexBase + (mode == 1 ? 1 : mode == 2 ? 4 : 0) + (opcode == 0xc7 ? 4 : 0);
}

static bool leadsToStubThatMayJumpToFunction(const Image& image, const uint8_t* code)
{
    constexpr unsigned maxInstructions = 16;
    constexpr unsigned maxSizeOfInstruction = 15;
    for (unsigned count = 0; count < maxInstructions && code; ++count, code = instructionBehindMoveOrJump(code)) {
        CodeLocation location = image.locate(code);
        if (location.kind != CodeKind::Stub || code + maxSizeOfInstruction > std::to_address(image.code().end()))
            return false;
        if (image.isStubThatMayJumpToFunction(location.offset))
            return true;
    }
    return false;
}

// See enterGetterAndReturn().
static bool isBehindAdditionOfAddressOfCode(const uint8_t* code)
{
    constexpr uint8_t opcodeOfAdd = 0x03;
    int32_t offset = AOT_INSTANCE_OFFSET(image.code);
    if (offset <= std::numeric_limits<int8_t>::max())
        return code[-3] == opcodeOfAdd && code[-1] == offset;
    return code[-6] == opcodeOfAdd && WTF::unalignedLoad<int32_t>(code - 4) == offset;
}

// See toCodeAddress().
static bool isBehindCallOfOperationOnARM64(const CodeLocation& returnSite, const void* returnAddress)
{
    if (returnSite.kind == CodeKind::Function || returnSite.offset < 2 * sizeof(uint32_t))
        return false;
    auto* end = static_cast<const uint8_t*>(returnAddress);
    bool isBLR = (WTF::unalignedLoad<uint32_t>(end - sizeof(uint32_t)) & 0xfffffc1f) == 0xd63f0000;
    bool isBehindAdditionOfOffsetOfCode = (WTF::unalignedLoad<uint32_t>(end - 2 * sizeof(uint32_t)) & 0xffe0fc00) == 0x8b204800;
    return isBLR && !isBehindAdditionOfOffsetOfCode;
}

static CalleeOfCall findCalleeOfCallEndingAt(const Image& image, const CodeLocation& returnSite, const void* returnAddress)
{
    if (!returnSite.runsCompiledCode())
        return { };
    if (returnSite.stubFrameKind != StubFrameKind::Plain)
        return { callsFunction(returnSite.stubFrameKind) };
    if (isARM64())
        return { !isBehindCallOfOperationOnARM64(returnSite, returnAddress) };
    constexpr uint8_t opcodeOfNearCall = 0xe8;
    constexpr uint8_t opcodeOfGroup5 = 0xff;
    auto* end = static_cast<const uint8_t*>(returnAddress);
    if (returnSite.offset >= sizeOfNearCall && end[-static_cast<ptrdiff_t>(sizeOfNearCall)] == opcodeOfNearCall) {
        int32_t distance;
        memcpy(&distance, end - sizeof(distance), sizeof(distance));
        auto* targetAddress = std::bit_cast<const uint8_t*>(std::bit_cast<uintptr_t>(end) + distance);
        CodeLocation target = image.locate(targetAddress);
        if (target.kind == CodeKind::Function)
            return { true, target.function };
        if (target.kind != CodeKind::NotInImage)
            return { leadsToStubThatMayJumpToFunction(image, targetAddress) };
    }
    if (returnSite.offset < 2 || end[-2] != opcodeOfGroup5 || end[-1] >> 3 != 0x1a)
        return { };
    constexpr unsigned maxSizeOfAdditionAndCall = 10;
    return { returnSite.kind == CodeKind::Function || (returnSite.offset >= maxSizeOfAdditionAndCall && isBehindAdditionOfAddressOfCode(end - 2 - (end[-3] == 0x41))) };
}

// The thread is suspended, so a chain of frames that holds this far is taken to hold.
static constexpr unsigned maxSampledFramesToCheck = 256;

struct FrameAtSite {
    CodeLocation site;
    const void* stackPointer;
    bool isAsFarAsWasChecked { false };
};

static SUPPRESS_ASAN std::optional<FrameAtSite> findFrameWithRecordAt(const Image& image, const SuspendedThreadStack& stack, FrameAtSite frameAtSite, const CallFrame* frameRecord)
{
    for (unsigned count = 0; count < maxSampledFramesToCheck; ++count) {
        const CodeLocation site = frameAtSite.site;
        const void* stackPointer = frameAtSite.stackPointer;
        bool isFunction = site.kind == CodeKind::Function;
        ImageFrame frame = frameAtSampledReturnSite(image, stack, site, stackPointer);
        if (!isAlignedForCall(stackPointer) && !frame.isEmpty())
            return std::nullopt;
        if (frame.hasFrameRecord()) {
            // That of a stub is as large as what it passes on the stack.
            if (isFunction ? std::bit_cast<uintptr_t>(stackPointer) + frame.frameSizeInBytes() - frame.bytesAboveFrameRecord() != std::bit_cast<uintptr_t>(frameRecord) : stackPointer > frameRecord)
                return std::nullopt;
            return frameAtSite;
        }
        CallFrame* callFrame = frameAtReturnSite(frame, stackPointer, nullptr);
        if (callFrame >= frameRecord)
            return std::nullopt;
        const void* returnAddress = removeReturnAddressSignature(reinterpret_cast<const CallerFrameAndPC*>(callFrame)->returnPC);
        CodeLocation caller = image.locate(returnAddress);
        if (!caller.runsCompiledCode() || !(isFunction ? findCalleeOfCallEndingAt(image, caller, returnAddress).mayBeFunction : callEndsAt(image.code(), returnAddress)))
            return std::nullopt;
        frameAtSite = { caller, callerStackPointer(callFrame, isFunction ? bytesAboveReturnAddressOf(frame) : bytesAboveReturnAddressOf(site.stubFrameKind)) };
    }
    frameAtSite.isAsFarAsWasChecked = true;
    return frameAtSite;
}

static bool isCodeWithFramesOfEngine(const void* pc)
{
#if ENABLE(WEBASSEMBLY_OMGJIT)
    if (Wasm::CodeInImage::contains(pc))
        return true;
#endif
    return isJITPC(const_cast<void*>(pc)) || LLInt::isLLIntPC(const_cast<void*>(pc));
}

// Not every call from compiled code stores topCallFrame, so it may point at what is a frame of native code by now.
static bool mayBeReturnSiteOfFrameOfEngine(const CodeLocation& site, const void* returnAddress)
{
    return site.runsCompiledCode() ? !isOperationReturnSite(site) : isCodeWithFramesOfEngine(returnAddress);
}

struct OwnerOfFrameRecord {
    bool mayBe(const FrameAtSite& frame) const
    {
        if (frame.isAsFarAsWasChecked)
            return true;
        StubFrameKind kind = frame.site.stubFrameKind;
        if (frame.site.kind == CodeKind::Function || kind == StubFrameKind::ArgumentList || kind == StubFrameKind::PlainFunctionConstruct)
            return callee.mayBeFunction;
        if (isAdapterFrame(kind))
            return mayBeAdapter && static_cast<const uint8_t*>(frame.stackPointer) + AdapterFrame::saveAreaSize <= frameRecord;
        return isCalledByCompiledCode;
    }

    const void* frameRecord;
    CalleeOfCall callee;
    bool isCalledByCompiledCode;
    bool mayBeAdapter;
};

static SUPPRESS_ASAN OwnerOfFrameRecord findOwnerOfFrameRecord(const Image& image, const SuspendedThreadStack& stack, const CallFrame* frameRecord)
{
    auto* record = reinterpret_cast<const CallerFrameAndPC*>(frameRecord);
    const void* returnAddress = removeReturnAddressSignature(record->returnPC);
    CodeLocation returnSite = image.locate(returnAddress);
    // See emitEnterAdapter().
    bool isCalledByEngine = returnSite.runsCompiledCode() ? returnSite.stubFrameKind == StubFrameKind::EngineCallee : isCodeWithFramesOfEngine(returnAddress);
    auto* codeBlock = reinterpret_cast<void* const*>(frameRecord) + static_cast<int>(CallFrameSlot::codeBlock);
    return { frameRecord, findCalleeOfCallEndingAt(image, returnSite, returnAddress), returnSite.runsCompiledCode(), isCalledByEngine && stack.contains(codeBlock) && !*codeBlock };
}

// What has been called at the site may have spilled the argument registers above the return address. Null if the frame cannot be there.
static SUPPRESS_ASAN const void* findEndOfFrameAtSite(const Image& image, const SuspendedThreadStack& stack, const CodeLocation& site, const void* wordAboveReturnAddress, const CallFrame* frameRecord, bool calleeHasSpilledArguments = false)
{
    if (!stack.contains(frameRecord))
        return nullptr;
    OwnerOfFrameRecord owner = findOwnerOfFrameRecord(image, stack, frameRecord);
    for (bool isAboveSpilledArguments : { calleeHasSpilledArguments, !calleeHasSpilledArguments }) {
        const void* endOfFrame = static_cast<const uint8_t*>(wordAboveReturnAddress) + (isAboveSpilledArguments ? sizeOfSpilledArgumentRegisters : 0);
        auto frameWithRecord = findFrameWithRecordAt(image, stack, { site, endOfFrame }, frameRecord);
        if (frameWithRecord && owner.mayBe(*frameWithRecord))
            return endOfFrame;
    }
    return nullptr;
}

enum class EndOfFrameIsKnown : bool { No, Yes };

static SUPPRESS_ASAN SampledTop sampledTopAtReturnSite(const Image& image, const SuspendedThreadStack& stack, const CodeLocation& site, const void* returnAddress, const void* stackPointer, CallFrame* framePointer, EndOfFrameIsKnown endOfFrameIsKnown = EndOfFrameIsKnown::No)
{
    ImageFrame frame = frameAtSampledReturnSite(image, stack, site, stackPointer);
    if (frame.hasFrameRecord())
        return { framePointer, returnAddress, nullptr };
    if (const void* endOfFrame = endOfFrameIsKnown == EndOfFrameIsKnown::No ? findEndOfFrameAtSite(image, stack, site, stackPointer, framePointer) : nullptr)
        stackPointer = endOfFrame;
    return { frameAtReturnSite(frame, stackPointer, framePointer), returnAddress, nullptr, framePointer };
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
    auto [callReturnAddress, wordAboveReturnAddress, hasSpilledArguments] = findCallOfCodeWithoutFrame(image, registers, top, stack);
    const void* stackPointerOfCaller = wordAboveReturnAddress;
    CodeLocation caller = image.locate(callReturnAddress);
    if (!caller.runsCompiledCode() || !(callEndsAt(image.code(), callReturnAddress) || isBehindFaultSite(image, callReturnAddress)))
        return result;
    if (top.kind != CodeKind::Stub && !isAlignedForCall(stackPointerOfCaller))
        return result;
    bool callerIsFunction = caller.kind == CodeKind::Function;
    EndOfFrameIsKnown endOfFrameIsKnown = EndOfFrameIsKnown::No;
    if (isX86_64() && callerIsFunction) {
        stackPointerOfCaller = findEndOfFrameAtSite(image, stack, caller, stackPointerOfCaller, result.callFrame, hasSpilledArguments);
        if (!stackPointerOfCaller)
            return result;
        endOfFrameIsKnown = EndOfFrameIsKnown::Yes;
    }
    if (top.kind == CodeKind::Function) {
        if (!callerIsFunction || caller.function != top.function) {
            result = sampledTopAtReturnSite(image, stack, caller, registers.pc, stackPointerOfCaller, result.callFrame, endOfFrameIsKnown);
            result.framelessTopCallerPC = callReturnAddress;
        }
    } else if (callerIsFunction || (top.kind != CodeKind::Stub && findOwnerOfFrameRecord(image, stack, result.callFrame).mayBeAdapter))
        return sampledTopAtReturnSite(image, stack, caller, callReturnAddress, stackPointerOfCaller, result.callFrame, endOfFrameIsKnown);
    return result;
}

static SUPPRESS_ASAN std::optional<SampledTop> findCallerOfCodeWithoutFrameRecord(const Image& image, const SuspendedThreadStack& stack, void* const* firstWord, CallFrame* frameRecord, const OwnerOfFrameRecord& owner, CodeKind kindOfCode)
{
    if (!stack.contains(firstWord))
        return std::nullopt;
    bool isStub = kindOfCode == CodeKind::Stub;
    std::optional<SampledTop> caller;
    const void* endOfFrameWithRecord = nullptr;
    bool framesOfSeveralSizesFit = false;
    // Either the frames of the code are large, or many without a record lie above them.
    constexpr ptrdiff_t wordsSearchedAtEitherEnd = 32 * KB / sizeof(void*);
    auto* end = reinterpret_cast<void* const*>(frameRecord);
    for (void* const* word = firstWord; word < end; word += stackAlignmentRegisters()) {
        if (word - firstWord == wordsSearchedAtEitherEnd && end - word > wordsSearchedAtEitherEnd)
            word += roundDownToMultipleOf<stackAlignmentRegisters()>(end - word - wordsSearchedAtEitherEnd);
        CodeLocation site = image.locate(*word);
        if (!site.runsCompiledCode() || !callEndsAt(image.code(), *word))
            continue;
        if (!isStub && findCalleeOfCallEndingAt(image, site, *word).mayBeFunction)
            continue;
        if (site.kind != CodeKind::Function) {
            if (!frameAtStubReturnSite(site.stubFrameKind).hasFrameRecord())
                continue;
            if (isStub)
                return std::nullopt; // The frame record is that of the stub which has called it.
            framesOfSeveralSizesFit = true;
            continue;
        }
        std::optional<FrameAtSite> frameWithRecord = findFrameWithRecordAt(image, stack, { site, word + 1 }, frameRecord);
        if (!frameWithRecord || !owner.mayBe(*frameWithRecord))
            continue;
        if (isStub || frameWithRecord->isAsFarAsWasChecked || owner.callee.names(frameWithRecord->site))
            return sampledTopAtReturnSite(image, stack, site, *word, word + 1, frameRecord, EndOfFrameIsKnown::Yes);
        if (!caller) {
            caller = sampledTopAtReturnSite(image, stack, site, *word, word + 1, frameRecord, EndOfFrameIsKnown::Yes);
            endOfFrameWithRecord = frameWithRecord->stackPointer;
        } else if (frameWithRecord->stackPointer != endOfFrameWithRecord)
            framesOfSeveralSizesFit = true;
    }
    return framesOfSeveralSizesFit ? std::nullopt : caller;
}

static SUPPRESS_ASAN SampledTop findTopInStub(const Image& image, const SampledRegisters& registers, const CodeLocation& top, const SuspendedThreadStack& stack)
{
    SampledTop result = findTopInCompiledCode(image, registers, top, stack);
    if (!isX86_64() || result.pc != registers.pc || !stack.contains(result.callFrame))
        return result;
    // What it saves under its return address takes up an even number of words. With a frame record there is one more, so that what a
    // function has left in the place of the stub's frame is passed over.
    bool isBetweenTwoWords = isAlignedForCall(registers.stackPointer) && bytesThatStackPointerIsAboutToMove(registers.pc) % stackAlignmentBytes();
    size_t wordsToPassOver = isAtSaveOfRegisterToTopOfStack(registers.pc) ? stackAlignmentRegisters() : isBetweenTwoWords;
    if (auto caller = findCallerOfCodeWithoutFrameRecord(image, stack, static_cast<void**>(registers.stackPointer) + wordsToPassOver, result.callFrame, findOwnerOfFrameRecord(image, stack, result.callFrame), CodeKind::Stub))
        return *caller;
    auto* record = reinterpret_cast<const CallerFrameAndPC*>(result.callFrame);
    SampledTop caller = sampledTopAtReturnSite(image, stack, image.locate(record->returnPC), record->returnPC, callerStackPointer(result.callFrame), record->callerFrame);
    return caller.framePointer ? caller : result;
}

// What is left of a frame that has returned leads to the word under a live frame, and that holds the return address of a later call.
static SUPPRESS_ASAN bool leadsToEndOfRun(const Image& image, const SuspendedThreadStack& stack, const SampledTop& top, const void* entryFrame)
{
    MachineFrameIterator frames(image, top.callFrame, top.pc, stack, top.framePointer);
    for (unsigned count = 0; count < maxSampledFramesToCheck; ++count, ++frames) {
        if (!frames || (*frames).callFrame >= entryFrame)
            return false;
        const MachineFrame& frame = *frames;
        auto* record = reinterpret_cast<const CallerFrameAndPC*>(frame.callFrame);
        if (frame.hasFrameRecord && record->callerFrame == entryFrame)
            return true;
        void* returnAddress = removeReturnAddressSignature(record->returnPC);
        CodeLocation caller = image.locate(returnAddress);
        if (!caller.runsCompiledCode())
            return caller.kind != CodeKind::NotInImage || (isCodeWithFramesOfEngine(returnAddress) && !LLInt::isVMEntryPC(returnAddress));
        if (frame.code.kind == CodeKind::Function && !findCalleeOfCallEndingAt(image, caller, returnAddress).mayBeFunction)
            return false;
    }
    return true;
}

static SUPPRESS_ASAN std::optional<SampledTop> findTopAtRecordOfNativeCode(const Image& image, const SuspendedThreadStack& stack, void* const* word, const void* entryFrame)
{
    auto* record = reinterpret_cast<const CallerFrameAndPC*>(word);
    CodeLocation site = image.locate(record->returnPC);
    if (!isOperationReturnSite(site) || !callEndsAt(image.code(), record->returnPC) || findCalleeOfCallEndingAt(image, site, record->returnPC).mayBeFunction)
        return std::nullopt;
    const void* stackPointer = word + CallerFrameAndPC::sizeInRegisters;
    SampledTop top = sampledTopAtReturnSite(image, stack, site, record->returnPC, stackPointer, record->callerFrame);
    if (top.callFrame < stackPointer || (site.kind == CodeKind::Function && !top.framePointer && !findFrameWithRecordAt(image, stack, { site, stackPointer }, top.callFrame)))
        return std::nullopt;
    if (!leadsToEndOfRun(image, stack, top, entryFrame))
        return std::nullopt;
    return top;
}

// The frame pointer does not lead to it if code that is not ours uses it like any other register, or in a frame in the form of Windows.
static SUPPRESS_ASAN std::optional<SampledTop> searchForRecordOfNativeCode(const Image& image, const SuspendedThreadStack& stack, const SampledRegisters& registers, CallFrame* topCallFrame, const void* entryFrame)
{
    if (!isX86_64() || !stack.contains(entryFrame) || !stack.contains(registers.stackPointer))
        return std::nullopt;

    constexpr size_t maxWordsToSearch = 16 * KB / sizeof(void*);
    auto* begin = static_cast<void* const*>(registers.stackPointer);
    auto* end = std::min(begin + maxWordsToSearch, static_cast<void* const*>(entryFrame));
    if (auto* recordAtTop = reinterpret_cast<void* const*>(topCallFrame); recordAtTop >= begin && recordAtTop < entryFrame && stack.contains(recordAtTop)) {
        if (auto top = findTopAtRecordOfNativeCode(image, stack, recordAtTop, entryFrame))
            return top;
        auto* record = reinterpret_cast<const CallerFrameAndPC*>(recordAtTop);
        CodeLocation site = image.locate(record->returnPC);
        // vmEntryToNative() and LLInt::handleHostCall() clear the return address.
        bool isFrameWithoutReturnAddress = !record->returnPC && record->callerFrame > topCallFrame && record->callerFrame <= entryFrame;
        if (site.stubFrameKind == StubFrameKind::HostCall) {
            if (leadsToEndOfRun(image, stack, { record->callerFrame, record->returnPC, nullptr }, entryFrame))
                end = std::min(end, reinterpret_cast<void* const*>(record->callerFrame) + 1);
        } else if (isFrameWithoutReturnAddress || mayBeReturnSiteOfFrameOfEngine(site, record->returnPC))
            end = std::min(end, recordAtTop);
    }
    // What enters the VM has stored topCallFrame.
    if (!registers.pc)
        return std::nullopt;
    for (void* const* word = begin; word < end; ++word) {
        if (auto top = findTopAtRecordOfNativeCode(image, stack, word, entryFrame))
            return top;
    }
    return std::nullopt;
}

// For a callee that is no function, and for eval(). Nothing has called it, so no frame pointer leads to it.
static SUPPRESS_ASAN bool isFrameThatSlowPathHasLaidOut(const SuspendedThreadStack& stack, const CallFrame* frame, const CallFrame* caller)
{
    if (!stack.contains(frame))
        return false;
    auto* record = reinterpret_cast<const CallerFrameAndPC*>(frame);
    if (record->callerFrame != caller)
        return false;
    const void* returnPC = record->returnPC;
    return !returnPC || returnPC == LLInt::getOpcode(llint_generic_return_point) || returnPC == LLInt::getOpcodeWide16(llint_generic_return_point) || returnPC == LLInt::getOpcodeWide32(llint_generic_return_point);
}

static SUPPRESS_ASAN std::optional<SampledTop> findTopBelowNativeCode(const Image& image, const SampledRegisters& registers, CallFrame* topCallFrame, const void* entryFrame, const SuspendedThreadStack& stack)
{
    auto* frame = static_cast<CallFrame*>(registers.framePointer);
    const void* linkRegister = removeReturnAddressSignature(registers.linkRegister);
    if (linkRegister && stack.contains(frame) && removeReturnAddressSignature(reinterpret_cast<const CallerFrameAndPC*>(frame)->returnPC) != linkRegister) {
        // What walks the stack leaves addresses there. See removeReturnAddressSignature().
        CodeLocation site = image.locate(linkRegister);
        if (site.runsCompiledCode() && findOwnerOfFrameRecord(image, stack, frame).mayBe({ site, registers.stackPointer }))
            return SampledTop { frame, linkRegister, nullptr };
    }
    const void* pc = registers.pc;
    const void* endOfFrameBelow = registers.stackPointer;
    while (stack.contains(frame) && (!entryFrame || frame < entryFrame)) {
        auto* record = reinterpret_cast<const CallerFrameAndPC*>(frame);
        const void* returnAddress = removeReturnAddressSignature(record->returnPC);
        CodeLocation returnSite = image.locate(returnAddress);
        bool runsCodeOfEngine = isCodeWithFramesOfEngine(pc);
        if (isX86_64() && !runsCodeOfEngine) {
            OwnerOfFrameRecord owner = findOwnerOfFrameRecord(image, stack, frame);
            bool mayBeCompiledCode = owner.mayBeAdapter || owner.callee.mayBeFunction;
            if (auto caller = mayBeCompiledCode ? findCallerOfCodeWithoutFrameRecord(image, stack, static_cast<void* const*>(endOfFrameBelow) + isAlignedForCall(endOfFrameBelow), frame, owner, CodeKind::NotInImage) : std::nullopt)
                return caller;
        }
        if (frame == topCallFrame) {
            // A tail call may not have written the return address yet.
            bool runsInFrameOfEngine = runsCodeOfEngine && !LLInt::isVMEntryPC(const_cast<void*>(pc));
            if (runsInFrameOfEngine ? !isOperationReturnSite(returnSite) : mayBeReturnSiteOfFrameOfEngine(returnSite, returnAddress))
                return SampledTop { topCallFrame, pc, nullptr };
        } else if (runsCodeOfEngine && topCallFrame >= endOfFrameBelow && topCallFrame < frame && isFrameThatSlowPathHasLaidOut(stack, topCallFrame, frame))
            return SampledTop { topCallFrame, nullptr, nullptr };
        if (returnSite.runsCompiledCode())
            return sampledTopAtReturnSite(image, stack, returnSite, returnAddress, callerStackPointer(frame), record->callerFrame);
        if (record->callerFrame <= frame)
            break;
        pc = returnAddress;
        endOfFrameBelow = callerStackPointer(frame);
        frame = record->callerFrame;
    }
    if (frame == entryFrame)
        return std::nullopt;
    return searchForRecordOfNativeCode(image, stack, registers, topCallFrame, entryFrame);
}

// No frame pointer leads to it: the stub lays it out under its own frame record, marks it with an address of its own, and then makes the call.
static SUPPRESS_ASAN bool isFrameOfHostCallMadeBy(const Image& image, const SuspendedThreadStack& stack, const SampledTop& stub, const CallFrame* frame)
{
    if (!stack.contains(frame))
        return false;
    auto* record = reinterpret_cast<const CallerFrameAndPC*>(frame);
    CodeLocation mark = image.locate(removeReturnAddressSignature(record->returnPC));
    CodeLocation returnSite = image.locate(stub.pc);
    if (record->callerFrame != stub.callFrame || mark.stubFrameKind != StubFrameKind::HostCall || returnSite.kind != CodeKind::Stub)
        return false;
    std::span sites = image.stubReturnSites();
    auto nextSite = std::ranges::upper_bound(sites, returnSite.offset, { }, &StubReturnSite::offset);
    return nextSite != sites.begin() && std::prev(nextSite)->offset == mark.offset;
}

std::optional<SampledTop> findSampledTop(const Image& image, const SampledRegisters& registers, CallFrame* topCallFrame, EntryFrame* entryFrame, const StackBounds& bounds)
{
    SuspendedThreadStack stack { bounds };
    CodeLocation top = image.locate(registers.pc);
    if (top.kind == CodeKind::Stub)
        return findTopInStub(image, registers, top, stack);
    if (top.runsCompiledCode())
        return findTopInCompiledCode(image, registers, top, stack);
    if (isX86_64() && registers.pc) {
        if (SampledTop caller = findTopInCompiledCode(image, registers, top, stack); caller.pc != registers.pc)
            return caller;
    }
    std::optional<SampledTop> result = findTopBelowNativeCode(image, registers, topCallFrame, entryFrame, stack);
    if (result && isFrameOfHostCallMadeBy(image, stack, *result, topCallFrame))
        return SampledTop { topCallFrame, nullptr, nullptr };
    return result;
}

#if ENABLE(WEBASSEMBLY_OMGJIT)
SUPPRESS_ASAN CallFrame* findCallerOfFramelessWebAssembly(const SampledRegisters& registers, CalleeBits callee, const StackBounds& bounds)
{
    SuspendedThreadStack stack { bounds };
    auto* frame = static_cast<CallFrame*>(registers.framePointer);
    if (!stack.contains(frame) || !stack.contains(registers.stackPointer))
        return frame;
    auto* record = reinterpret_cast<const CallerFrameAndPC*>(frame);
    // It has one while it sets up the rest of its frame, and while it takes that down.
    bool hasFrameRecord;
    if (isX86_64()) {
        if (isAlignedForCall(registers.stackPointer))
            hasFrameRecord = !isAtMoveOfStackPointerToFramePointer(registers.pc);
        else
            hasFrameRecord = !isCodeWithFramesOfEngine(*static_cast<void* const*>(registers.stackPointer));
    } else
        hasFrameRecord = removeReturnAddressSignature(registers.linkRegister) == removeReturnAddressSignature(record->returnPC) || frame->unsafeCallee().rawPtr() == callee.rawPtr();
    return hasFrameRecord ? record->callerFrame : frame;
}
#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(CallSiteCache);

CallSiteCache::CallSiteCache() = default;

void CallSiteCache::Entry::findHandler(const Image& image) const
{
    handler = image.handlerAt(code.index(), site.callSite);
    mayHaveHandler = false;
}

static uint16_t countShownFrames(const Image& image, InlineFrameReader& reader, CompiledFunctionID code, const CallSiteRecord& site, ShownFrameFilter filter, bool innermostIsTailDeleted)
{
    uint16_t count = 0;
    LogicalFrame frame = innermostLogicalFrame(image, reader, { code, site.callSite, site.inlineFrame, innermostIsTailDeleted, site.calledBuiltin });
    while (true) {
        if (filter.shows(FunctionDescription(frame.function).visibility(), frame.isTailDeleted)) {
            RELEASE_ASSERT(count < std::numeric_limits<decltype(count)>::max());
            ++count;
        }
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

static CallSiteCache::Entry createFunctionEntry(const Image& image, InlineFrameReader& reader, const CodeLocation& location, const void* returnAddress)
{
    CallSiteCache::Entry entry;
    entry.address = returnAddress;
    entry.code = CompiledFunctionID(location.function);
    ReturnSiteInfo returnSite = image.returnSiteInfoAt(location.function, location.offset);
    if (returnSite.site)
        entry.site = *returnSite.site;
    entry.mayHaveHandler = returnSite.mayHaveHandler;
    entry.frame = returnSite.frame;
    FunctionDescription root(image.rootFunctionOf(location.function));
    entry.rejectsOnThrow = root.rejectsOnThrow() && (returnSite.site || !root.makesPromiseOfCall());
    entry.isInModuleBody = root.codeType() == ModuleCode;

    LogicalFrame innermost = innermostLogicalFrame(image, reader, { entry.code, entry.site.callSite, entry.site.inlineFrame, false });
    entry.innermostFunction = innermost.function;
    entry.innermostIsInlinedConstructCall = innermost.isInlinedConstructCall;
    FunctionDescription innermostDescription(innermost.function);
    entry.innermostVisibility = innermostDescription.visibility();
    entry.innermostIsAsync = innermostDescription.isAsync();
    entry.namesConstructedCallee = returnSite.hasConstructedCallees && image.constructedCalleeAt(location.function, entry.site.callSite).has_value();
    entry.namesTailCallingBuiltin = entry.site.calledBuiltin && leavesByTailCall(entry.site.calledBuiltin);
    entry.outermostVisibleFunction = findOutermostVisibleFunction(image, reader, entry, true);
    auto countFrames = [&](ShownFrameFilter filter, bool innermostIsTailDeleted) -> uint16_t {
        if (!entry.site.inlineFrame && !entry.site.calledBuiltin)
            return filter.shows(entry.innermostVisibility, innermostIsTailDeleted);
        return countShownFrames(image, reader, entry.code, entry.site, filter, innermostIsTailDeleted);
    };
    entry.shownFrameCount = countFrames(ShownFrameFilter(true), entry.site.isTailCall());
    entry.shownFrameCountOnTop = std::min<uint16_t>(countFrames(ShownFrameFilter(false), false), CallSiteCache::Entry::tooManyFramesToCount);
    if (entry.site.isTailCall())
        entry.shownFrameCountOnTopBehindTailCall = std::min<uint16_t>(countFrames(ShownFrameFilter(false), true), CallSiteCache::Entry::tooManyFramesToCount);
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
    m_numberOfEvictions = 0;
    for (const Entry& entry : previousEntries) {
        if (!entry.address)
            continue;
        if (Entry* slot = findEmptySlot(entry.address)) {
            *slot = entry;
            ++m_numberOfFilledEntries;
        }
    }
}

CallSiteCache::Entry& CallSiteCache::slotForNewEntry(const void* address)
{
    if (m_entries.isEmpty())
        setNumberOfEntries(initialNumberOfEntries);
    for (;;) {
        if (Entry* slot = findEmptySlot(address)) {
            ++m_numberOfFilledEntries;
            return *slot;
        }
        for (size_t index : { primaryIndex(address), secondaryIndex(address) }) {
            Entry& holder = m_entries[index];
            size_t primaryOfHolder = primaryIndex(holder.address);
            Entry& otherSlot = m_entries[primaryOfHolder == index ? secondaryIndex(holder.address) : primaryOfHolder];
            if (!otherSlot.address) {
                otherSlot = holder;
                ++m_numberOfFilledEntries;
                return holder;
            }
        }
        if (m_entries.size() == maxNumberOfEntries || (m_numberOfFilledEntries * 2 < m_entries.size() && m_numberOfEvictions < m_entries.size()))
            break;
        setNumberOfEntries(m_entries.size() * 2);
    }
    ++m_numberOfEvictions;
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
