/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStackTrace.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTFunctionNames.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTRuntimeValidation.h"
#include "DeferGC.h"
#include "ExecutableBaseInlines.h"
#include "Interpreter.h"
#include "JSCJSValueInlines.h"
#include "JSFunctionInlines.h"
#include "StackFrame.h"
#include "VMInlines.h"
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using ReturnAddressFlags = OptionSet<ReturnAddressTrace::Flag>;

struct CapturedRun {
    Vector<uint32_t, 32> returnAddresses;
    Vector<ReturnAddressFlags, 32> flags;
    size_t numberShown { 0 };
    size_t numberOmitted { 0 };
    bool foundConstructorToSkip { false };
};

struct FramesAboveRun {
    size_t numberShown { 0 };
    bool exist { false };
};

static CapturedRun captureRun(CompiledFrameIterator& frames, size_t maxStackSize, JSCell* lastConstructorToSkip, const FramesAboveRun& framesAbove)
{
    using Trace = ReturnAddressTrace;
    uintptr_t codeStart = imageCodeStart();
    ShownFrameFilter filter(framesAbove.exist || lastConstructorToSkip);
    CapturedRun run;
    run.numberShown = framesAbove.numberShown;
    CompiledFunctionID outermostCode;

    while (!frames.isAtEnd()) {
        const auto& site = frames.callSite();
        if (site.site.isConstruct && !run.flags.isEmpty())
            run.flags.last().add(Trace::Flag::IsConstructCall);
        outermostCode = site.code;

        std::optional<unsigned> shownFrameCount;
        if (!lastConstructorToSkip)
            shownFrameCount = site.shownFrameCountFor(filter.hasShownFrame(), frames.calleeWasEntered());

        run.returnAddresses.append(Trace::encode(site.address, codeStart));
        run.flags.append(frames.isAtFirstReadSite() ? ReturnAddressFlags { Trace::Flag::IsAtFirstReadSite } : ReturnAddressFlags { });
        bool mayShowFrame = !shownFrameCount || *shownFrameCount;
        if (run.numberShown >= maxStackSize && mayShowFrame)
            break;

        if (shownFrameCount) {
            run.numberShown += *shownFrameCount;
            if (*shownFrameCount)
                filter = ShownFrameFilter(true);
            frames.advanceToCallerMachineFrame();
            continue;
        }
        for (bool wasRoot = false; !wasRoot; frames.advance()) {
            auto& frame = frames.frame();
            wasRoot = !frame.inlineFrame;
            if (!filter.shows(frame.visibility, frame.isTailDeleted))
                continue;
            if (!lastConstructorToSkip) {
                ++run.numberShown;
                continue;
            }
            ++run.numberOmitted;
            if (frame.isActivationOf(lastConstructorToSkip))
                lastConstructorToSkip = nullptr;
        }
    }

    if (frames.isAtEnd() && !run.flags.isEmpty() && frames.runEnd().constructs(Image::current()->rootFunctionOf(outermostCode.index())))
        run.flags.last().add(Trace::Flag::IsConstructCall);
    run.foundConstructorToSkip = !lastConstructorToSkip;
    return run;
}

static bool isSameFrame(const StackFrame& expected, const StackFrame& actual)
{
    if (expected.isCompiledFrame() != actual.isCompiledFrame() || expected.isAsyncFrame() != actual.isAsyncFrame())
        return false;
    if (expected.isCompiledFrame()) {
        const TraceFrame& left = expected.compiledFrame();
        const TraceFrame& right = actual.compiledFrame();
        return left.code == right.code && left.function == right.function && left.callSite.bits() == right.callSite.bits() && left.isInlined == right.isInlined && left.isConstructCall == right.isConstructCall;
    }
    if (expected.callee() != actual.callee() || expected.codeBlock() != actual.codeBlock() || expected.isWasmFrame() != actual.isWasmFrame() || expected.hasBytecodeIndex() != actual.hasBytecodeIndex())
        return false;
    return !expected.hasBytecodeIndex() || expected.bytecodeIndex() == actual.bytecodeIndex();
}

static FunctionID compiledFunctionAt(const Vector<StackFrame>& frames, size_t index)
{
    if (index >= frames.size() || !frames[index].isCompiledFrame())
        return { };
    return frames[index].compiledFrame().function;
}

static void validateCapturedTrace(VM& vm, JSCell* owner, const ReturnAddressTrace& trace, const Vector<StackFrame>* otherFrames, size_t maxStackSize, JSCell* lastConstructorToSkip)
{
    Vector<StackFrame> expected;
    vm.interpreter.getStackTrace(owner, expected, 0, maxStackSize, lastConstructorToSkip);

    Vector<StackFrame> actual;
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
    memcpySpan(trace.returnAddresses(), run.returnAddresses.span());
    zeroSpan(trace.flagWords());
    for (size_t i = 0; i < run.flags.size(); ++i)
        trace.addFlagsAt(i, run.flags[i]);
}

static bool fitsInTrace(const CapturedRun& run)
{
    return run.numberShown <= ReturnAddressTrace::maxLimit && run.numberOmitted <= ReturnAddressTrace::maxOmittedFrames && run.returnAddresses.size() <= ReturnAddressTrace::maxNumberOfReturnAddresses;
}

ReturnAddressTrace* tryCaptureStackTrace(VM& vm, JSCell* owner, size_t maxStackSize, std::unique_ptr<Vector<StackFrame>>& otherFrames, JSCell* lastConstructorToSkip)
{
    if (!vm.hasCompiledCode() || !vm.topCallFrame || !maxStackSize)
        return nullptr;
    std::optional<AssertNoGC> assertNoGC { std::in_place };

    Vector<StackFrame> results;
    FramesAboveRun framesAbove;
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames) {
        if (lastConstructorToSkip)
            return nullptr;
        vm.interpreter.appendStackTrace(owner, results, { .callFrame = vm.topCallFrame, .compiledFramesBelow = &frames }, maxStackSize);
        if (!frames || results.size() > ReturnAddressTrace::maxFramesOnTop)
            return nullptr;
        framesAbove = { results.size(), true };
    }
    if (frames->isAtEnd() || frames->frame().instance->runtime.exceptions.callSiteOverrides)
        return nullptr;
    Instance* instance = frames->frame().instance;
    EntryFrame* entryFrame = frames->entryFrame();
    bool calleeOfFirstFrameWasEntered = frames->calleeWasEntered();

    CapturedRun run = captureRun(*frames, maxStackSize, lastConstructorToSkip, framesAbove);
    if (run.returnAddresses.isEmpty() || !run.numberShown || !run.foundConstructorToSkip || !fitsInTrace(run))
        return nullptr;

    assertNoGC.reset();
    auto* trace = static_cast<ReturnAddressTrace*>(vm.auxiliarySpace().allocate(vm, ReturnAddressTrace::allocationSize(run.returnAddresses.size()), nullptr, AllocationFailureMode::ReturnNull));
    assertNoGC.emplace();
    if (!trace)
        return nullptr;
    fillTrace(*trace, instance, run, maxStackSize, framesAbove, calleeOfFirstFrameWasEntered);

    Vector<StackFrame> framesBelow;
    vm.interpreter.appendStackTrace(owner, framesBelow, {
        .compiledFrameExit = frames->isAtEnd() ? &frames->exit() : nullptr,
        .entryFrameOfFramesAbove = entryFrame,
        .numberOfFramesAbove = std::min(run.numberShown, maxStackSize),
    }, maxStackSize);
    results.appendVector(WTF::move(framesBelow));
    if (!results.isEmpty()) {
        results.shrinkToFit();
        otherFrames = makeUnique<Vector<StackFrame>>(WTF::move(results));
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
        ReturnAddressFlags flags = trace.flagsAt(i);
        const CallSiteCache::Entry* entry = cache.resolve(image, ReturnAddressTrace::decode(returnAddresses[i], codeStart));
        RELEASE_ASSERT(entry);
        CallSiteIndex site = flags.contains(ReturnAddressTrace::Flag::IsAtFirstReadSite) && entry->site.firstReadSite ? *entry->site.firstReadSite : entry->site.callSite;
        bool isTailDeleted = entry->site.isTailCall && (i || trace.calleeOfFirstFrameWasEntered);
        for (LogicalFrame frame = innermostLogicalFrame(image, reader, { entry->code, site, entry->site.inlineFrame, isTailDeleted });; frame = callerOfInlinedFrame(image, reader, entry->code, frame)) {
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
                    pending = TraceFrame { entry->code, frame.function, frame.callSite, !!frame.inlineFrame, false };
                    pendingSource = description.sourceFunction();
                }
            }

            if (pending && (frame.inlineFrame ? frame.isInlinedConstructCall : flags.contains(ReturnAddressTrace::Flag::IsConstructCall))) {
                pending->isConstructCall = true;
                flush();
            }
            if (!frame.inlineFrame)
                break;
        }
    }
    flush();
}

void forEachShownFrame(CompiledFunctionID code, CallSiteIndex callSite, ShownFrameFilter& filter, const ScopedLambda<void(const TraceFrame&)>& functor)
{
    const Image& image = *Image::current();
    unsigned inlineFrame = 0;
    if (image.function(code.index()).has(ImageFunctionFlag::HasInlineFrames) && callSite.bits()) {
        SiteFrames frames(image, code.index());
        frames.skip(callSite.bits() - 1);
        inlineFrame = frames.next();
    }

    InlineFrameReader reader;
    for (LogicalFrame frame = innermostLogicalFrame(image, reader, { code, callSite, inlineFrame, false });; frame = callerOfInlinedFrame(image, reader, code, frame)) {
        if (filter.shows(FunctionDescription(frame.function).visibility(), frame.isTailDeleted))
            functor(TraceFrame { code, frame.function, frame.callSite, !!frame.inlineFrame, frame.isInlinedConstructCall });
        if (!frame.inlineFrame)
            break;
    }
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
    if (!module)
        return emptyString();
    StringView path = modulePath(module);
    if (path.is8Bit())
        return StringImpl::createWithoutCopying(path.span8());
    return StringImpl::createWithoutCopying(path.span16());
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
        auto site = image->callSiteAt(location.function, location.offset);
        forEachShownFrame(CompiledFunctionID(location.function), site ? site->callSite : CallSiteIndex(0), filter, [&](const TraceFrame& frame) {
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
