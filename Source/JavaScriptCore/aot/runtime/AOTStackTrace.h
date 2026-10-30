/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"
#include "AOTImageFormat.h"
#include "AOTLimits.h"
#include "AOTShownFrameFilter.h"
#include "AOTTraceFrame.h"
#include "AOTVector.h"
#include <bit>
#include <memory>
#include <span>
#include <wtf/IterationStatus.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class StackFrame;

namespace AOT {

inline TraceFrame TraceFrame::of(CompiledFrameIterator& frames)
{
    auto& frame = frames.frame();
    return { frame.code, frame.function, frame.callSite, !!frame.inlineFrame, frames.isConstructCall(), false, false, frames.calleeWasEntered() };
}

struct ReturnAddressTrace {
    WTF_ALLOW_STRUCT_COMPACT_POINTERS;

    enum class Flag : uint8_t {
        IsConstructCall = 1 << 0,
        IsAtFirstReadSite = 1 << 1,
    };

    static constexpr unsigned limitBits = 22;
    static constexpr uint32_t maxLimit = (1u << limitBits) - 1;
    static constexpr unsigned maxOmittedFrames = 255;
    static constexpr unsigned returnAddressCountBits = 24;
    static constexpr uint32_t maxNumberOfReturnAddresses = (1u << returnAddressCountBits) - 1;
    static constexpr unsigned framesOnTopBits = 6;
    static constexpr unsigned maxFramesOnTop = (1u << framesOnTopBits) - 1;
    static constexpr unsigned numberOfFlags = 2;
    static constexpr unsigned returnAddressesPerFlagWord = 32 / numberOfFlags;
    static constexpr uint32_t flagsMask = (1u << numberOfFlags) - 1;
    static constexpr uint64_t encodableCodeSize = (1ull << 32) * codeOffsetUnit;

    static constexpr size_t numberOfFlagWords(size_t numberOfReturnAddresses) { return (numberOfReturnAddresses + returnAddressesPerFlagWord - 1) / returnAddressesPerFlagWord; }
    static constexpr size_t allocationSize(size_t numberOfReturnAddresses) { return sizeof(ReturnAddressTrace) + (numberOfReturnAddresses + numberOfFlagWords(numberOfReturnAddresses)) * sizeof(uint32_t); }
    static constexpr unsigned shiftInFlagWord(size_t index) { return static_cast<unsigned>(index % returnAddressesPerFlagWord) * numberOfFlags; }

    static uint32_t encode(const void* address, uintptr_t imageCodeStart)
    {
        uintptr_t offset = std::bit_cast<uintptr_t>(address) - imageCodeStart;
        ASSERT(offset < encodableCodeSize && !(offset % codeOffsetUnit));
        return static_cast<uint32_t>(offset / codeOffsetUnit);
    }
    static const void* decode(uint32_t returnAddress, uintptr_t imageCodeStart) { return std::bit_cast<const void*>(imageCodeStart + static_cast<uintptr_t>(returnAddress) * codeOffsetUnit); }

    size_t sizeInBytes() const { return allocationSize(numberOfReturnAddresses) + sizeOfCalleeTable(callees().size()); }
    std::span<uint32_t> returnAddresses() { return { reinterpret_cast<uint32_t*>(this + 1), numberOfReturnAddresses }; }
    std::span<const uint32_t> returnAddresses() const { return { reinterpret_cast<const uint32_t*>(this + 1), numberOfReturnAddresses }; }
    std::span<uint32_t> flagWords() { return { reinterpret_cast<uint32_t*>(this + 1) + numberOfReturnAddresses, numberOfFlagWords(numberOfReturnAddresses) }; }
    std::span<const uint32_t> flagWords() const { return { reinterpret_cast<const uint32_t*>(this + 1) + numberOfReturnAddresses, numberOfFlagWords(numberOfReturnAddresses) }; }

    // A return address does not say which function object runs in the frame, and a frame of a function whose function objects may be renamed is named by it.
    // Behind the flag words of a trace with such frames stand their number and a row for each, in the order of the addresses.
    struct Callee {
        JSObject* object() const { return std::bit_cast<JSObject*>(static_cast<uintptr_t>(static_cast<uint64_t>(highBits) << 32 | lowBits)); }

        // Which of returnAddresses(), and LogicalFrame::inlineFrame there.
        uint32_t returnAddress;
        uint32_t inlineFrame;
        // Null: TraceFrame::calleeLiesInFrame.
        uint32_t lowBits;
        uint32_t highBits;
    };
    static constexpr size_t sizeOfCalleeTable(size_t numberOfCallees) { return numberOfCallees ? sizeof(uint32_t) + numberOfCallees * sizeof(Callee) : 0; }
    uint32_t* calleeTable() { return reinterpret_cast<uint32_t*>(this + 1) + numberOfReturnAddresses + numberOfFlagWords(numberOfReturnAddresses); }
    std::span<const Callee> callees() const
    {
        if (!hasCallees) [[likely]]
            return { };
        const uint32_t* table = const_cast<ReturnAddressTrace*>(this)->calleeTable();
        return { reinterpret_cast<const Callee*>(table + 1), table[0] };
    }

    // In a VM that runs several images a trace may hold several runs, each of one image: compiled code of one run has called the run in front of it, with no frame that is listed between.
    // Each address counts from the code of its own image. Behind the function objects, in such a VM only, stand the number of runs and a record for each.
    struct Run {
        uint32_t endOfReturnAddresses;
        uint32_t imagePosition;
    };
    static constexpr size_t sizeOfRunTable(size_t numberOfRuns) { return numberOfRuns ? sizeof(uint32_t) + numberOfRuns * sizeof(Run) : 0; }
    uint32_t* runTable() { return calleeTable() + sizeOfCalleeTable(callees().size()) / sizeof(uint32_t); }
    std::span<const Run> runs() const
    {
        const uint32_t* table = const_cast<ReturnAddressTrace*>(this)->runTable();
        return { reinterpret_cast<const Run*>(table + 1), table[0] };
    }

    OptionSet<Flag> flagsAt(size_t index) const { return OptionSet<Flag>::fromRaw(static_cast<uint8_t>(flagWords()[index / returnAddressesPerFlagWord] >> shiftInFlagWord(index) & flagsMask)); }
    void addFlagsAt(size_t index, OptionSet<Flag> flags) { flagWords()[index / returnAddressesPerFlagWord] |= static_cast<uint32_t>(flags.toRaw()) << shiftInFlagWord(index); }

    Instance* instance;
    uint32_t limit : limitBits;
    uint32_t hasCallees : 1;
    // The trace and the frames that its owner keeps beside it are as many as the limit allows, so the stack may go on below them.
    uint32_t isFull : 1;
    uint32_t omittedFrameCount : 8;
    uint32_t numberOfReturnAddresses : returnAddressCountBits;
    // Frames of host functions and of interpreted code that the compiled code called. The owner of the trace keeps them in front of the frames below the compiled code.
    uint32_t numberOfFramesOnTop : framesOnTopBits;
    uint32_t isBelowOtherFrames : 1;
    uint32_t calleeOfFirstFrameWasEntered : 1;
};
static_assert(defaultLimitValue(LimitID::CodeBytes, hostArchitecture()) <= ReturnAddressTrace::encodableCodeSize);
static_assert(static_cast<uint32_t>(ReturnAddressTrace::Flag::IsAtFirstReadSite) <= ReturnAddressTrace::flagsMask);
static_assert(!(sizeof(ReturnAddressTrace) % sizeof(uint32_t)));
static_assert(sizeof(ReturnAddressTrace) == 2 * sizeof(uint64_t) && alignof(ReturnAddressTrace::Callee) == alignof(uint32_t));

// The engine's own constructors of errors are called like any host function, and leave their own frame out of the trace.
enum class FrameOnTop : bool { IsListed, IsLeftOut };
JS_EXPORT_PRIVATE ReturnAddressTrace* tryCaptureStackTrace(VM&, JSCell* owner, size_t maxStackSize, std::unique_ptr<WTF::Vector<StackFrame>>& otherFrames, JSCell* lastConstructorToSkip = nullptr, FrameOnTop = FrameOnTop::IsListed);
JS_EXPORT_PRIVATE bool beginsAt(const ReturnAddressTrace&, const void* returnAddress);
// `callee`: the function object that names the frame, where the trace holds one.
JS_EXPORT_PRIVATE void forEachShownFrame(const ReturnAddressTrace&, const ScopedLambda<void(const TraceFrame&, JSObject* callee)>&);
// The owner of a trace holds its function objects as it holds the frames beside the trace.
JS_EXPORT_PRIVATE bool hasUnmarkedCallee(VM&, const ReturnAddressTrace&);
template<typename Visitor>
void visitCallees(Visitor& visitor, const ReturnAddressTrace& trace)
{
    for (const ReturnAddressTrace::Callee& callee : trace.callees()) {
        if (JSObject* object = callee.object())
            visitor.appendUnbarriered(object);
    }
}
JS_EXPORT_PRIVATE void forEachShownFrame(CompiledFunctionID, const CallSiteRecord&, ShownFrameFilter&, const ScopedLambda<void(const TraceFrame&)>&, bool calleeWasEntered = false);
// What Interpreter::appendStackTrace wants of a run of compiled code, and what became of it.
struct RunListing {
    JSCell* owner;
    size_t maxResultsSize;
    size_t framesToSkip;
    // Null once its frame has been passed.
    JSCell* caller;
    const ScopedLambda<bool(const StackFrame&)>* filter;
    // A frame came that is neither skipped nor above the caller.
    bool hasReachedFrame { false };
    // It was appended, unless the filter refused it.
    bool hasListedFrame { false };
    // CompiledFrameIterator::isAboveFramesThatWait() of the frame of an async function.
    bool isAboveFramesThatWait { false };
};
// Appends the frames that are shown of the run that the iterator stands at the start of. Unless that ends the walk, the iterator is at the end of the run afterwards.
JS_EXPORT_PRIVATE IterationStatus appendShownFramesOfRun(VM&, CompiledFrameIterator&, ShownFrameFilter&, WTF::Vector<StackFrame>& results, RunListing&);
// The frames that Interpreter::getStackTrace lists, where compiled code is on top of the stack or one host function that it called.
JS_EXPORT_PRIVATE bool tryGetStackTrace(VM&, JSCell* owner, WTF::Vector<StackFrame>& results, size_t framesToSkip, size_t maxStackSize, const ScopedLambda<bool(const StackFrame&)>* wanted = nullptr);
// Ends the process unless a walk that visits each compiled frame by itself lists the same frames.
void validateFrames(VM&, JSCell* owner, const WTF::Vector<StackFrame>& actual, size_t maxStackSize, JSCell* lastConstructorToSkip, size_t framesToSkip = 0);
JS_EXPORT_PRIVATE std::optional<TraceFrame> tryGetFrameOfClassCalledWithoutNew(VM&, CallFrame*);
// What a stack trace lists for a function that was constructed and whose frame a tail call has taken from the stack. `enteredSince` is the code that stands in its place, if compiled code does.
std::optional<TraceFrame> frameOfConstructedFunctionThatLeft(const Image&, FunctionID constructed, CompiledFunctionID enteredSince);
// What a stack trace lists for a plain function while a stub makes `this` for it: reading `prototype` of new.target may run any code. Bytecode makes `this` in the frame of the function.
std::optional<TraceFrame> frameOfFunctionWhoseThisIsMade(const Image&, FunctionID constructed);
JS_EXPORT_PRIVATE std::optional<SourcePosition> positionOf(const TraceFrame&);
// Where the function of the frame is written.
JS_EXPORT_PRIVATE std::optional<SourcePosition> definitionOf(const TraceFrame&);
JS_EXPORT_PRIVATE LineColumn obfuscatedPositionOf(const TraceFrame&);
JS_EXPORT_PRIVATE String sourceURLOf(ModuleID);
// What the engine writes for the file in the lines `name@url:line:column`.
String sourceProviderURLOf(ModuleID);
JS_EXPORT_PRIVATE String describeCodeAddresses(std::span<const void* const>);
JS_EXPORT_PRIVATE JSCell* livenessToken(Instance*);

} // namespace AOT
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
