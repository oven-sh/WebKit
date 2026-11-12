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

    size_t sizeInBytes() const { return allocationSize(numberOfReturnAddresses); }
    std::span<uint32_t> returnAddresses() { return { reinterpret_cast<uint32_t*>(this + 1), numberOfReturnAddresses }; }
    std::span<const uint32_t> returnAddresses() const { return { reinterpret_cast<const uint32_t*>(this + 1), numberOfReturnAddresses }; }
    std::span<uint32_t> flagWords() { return { reinterpret_cast<uint32_t*>(this + 1) + numberOfReturnAddresses, numberOfFlagWords(numberOfReturnAddresses) }; }
    std::span<const uint32_t> flagWords() const { return { reinterpret_cast<const uint32_t*>(this + 1) + numberOfReturnAddresses, numberOfFlagWords(numberOfReturnAddresses) }; }

    OptionSet<Flag> flagsAt(size_t index) const { return OptionSet<Flag>::fromRaw(static_cast<uint8_t>(flagWords()[index / returnAddressesPerFlagWord] >> shiftInFlagWord(index) & flagsMask)); }
    void addFlagsAt(size_t index, OptionSet<Flag> flags) { flagWords()[index / returnAddressesPerFlagWord] |= static_cast<uint32_t>(flags.toRaw()) << shiftInFlagWord(index); }

    Instance* instance;
    uint32_t limit : limitBits;
    uint32_t isFull : 1;
    uint32_t omittedFrameCount : 8;
    uint32_t numberOfReturnAddresses : returnAddressCountBits;
    uint32_t numberOfFramesOnTop : framesOnTopBits;
    uint32_t isBelowOtherFrames : 1;
    uint32_t calleeOfFirstFrameWasEntered : 1;
};
static_assert(defaultLimitValue(LimitID::CodeBytes, hostArchitecture()) <= ReturnAddressTrace::encodableCodeSize);
static_assert(static_cast<uint32_t>(ReturnAddressTrace::Flag::IsAtFirstReadSite) <= ReturnAddressTrace::flagsMask);
static_assert(!(sizeof(ReturnAddressTrace) % sizeof(uint32_t)));
static_assert(sizeof(ReturnAddressTrace) == 2 * sizeof(uint64_t));

enum class TopFramePolicy : bool { Include, Skip };
JS_EXPORT_PRIVATE ReturnAddressTrace* tryCaptureStackTrace(VM&, JSCell* owner, size_t maxStackSize, std::unique_ptr<WTF::Vector<StackFrame>>& otherFrames, JSCell* lastConstructorToSkip = nullptr, TopFramePolicy = TopFramePolicy::Include);
JS_EXPORT_PRIVATE bool beginsAt(const ReturnAddressTrace&, const void* returnAddress);
JS_EXPORT_PRIVATE void forEachShownFrame(const ReturnAddressTrace&, const ScopedLambda<void(const TraceFrame&)>&);
JS_EXPORT_PRIVATE void forEachShownFrame(CompiledFunctionID, const CallSiteRecord&, ShownFrameFilter&, const ScopedLambda<void(const TraceFrame&)>&);
struct RunListing {
    size_t maxResultsSize;
    size_t framesToSkip;
    JSCell* caller;
    const ScopedLambda<bool(const StackFrame&)>* filter;
    bool hasReachedFrame { false };
    bool hasListedFrame { false };
    bool isAboveFramesThatWait { false };
};
JS_EXPORT_PRIVATE IterationStatus appendShownFramesOfRun(VM&, CompiledFrameIterator&, ShownFrameFilter&, WTF::Vector<StackFrame>& results, RunListing&);
enum class HiddenFrameOnTop : bool { IsLeftOut, IsShown };
std::optional<TraceFrame> findShownFrameOnTop(VM&, HiddenFrameOnTop);
JS_EXPORT_PRIVATE bool tryGetStackTrace(VM&, JSCell* owner, WTF::Vector<StackFrame>& results, size_t framesToSkip, size_t maxStackSize, const ScopedLambda<bool(const StackFrame&)>* wanted = nullptr);
JS_EXPORT_PRIVATE std::optional<TraceFrame> tryGetClassCallWithoutNewFrame(VM&, CallFrame*);
std::optional<TraceFrame> makeElidedConstructorFrame(const Image&, FunctionID constructed);
JS_EXPORT_PRIVATE std::optional<SourcePosition> positionOf(const TraceFrame&);
JS_EXPORT_PRIVATE std::optional<SourcePosition> definitionOf(const TraceFrame&);
JS_EXPORT_PRIVATE LineColumn obfuscatedPositionOf(const TraceFrame&);
JS_EXPORT_PRIVATE String sourceURLOf(ModuleID);
JS_EXPORT_PRIVATE String displayPathOf(ModuleID);
JS_EXPORT_PRIVATE String sourceURLOf(const SourcePosition&);
JS_EXPORT_PRIVATE String displayPathOf(const SourcePosition&);
String sourceProviderURLOf(ModuleID);
String sourceProviderURLOf(const TraceFrame&, const std::optional<SourcePosition>&);
JS_EXPORT_PRIVATE JSCell* livenessToken(Instance*);

} // namespace AOT
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
