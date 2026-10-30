/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionDescription.h"
#include "AOTImage.h"
#include <array>
#include <bit>
#include <optional>
#include <wtf/FixedVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

// The outermost function of a machine frame that stack traces list. A frame has none where it holds only parts of a function that are never listed by themselves.
enum class OutermostVisibleFunction : uint8_t { None, Listed, HiddenBehindTailCall };

class CallSiteCache {
    WTF_MAKE_TZONE_ALLOCATED(CallSiteCache);
    WTF_MAKE_NONCOPYABLE(CallSiteCache);
public:
    CallSiteCache();

    // What the image says about an address that compiled code returns to: a call site of a function, or a place in a stub.
    struct Entry {
        // Of such an entry only address and stubFrameKind say anything.
        bool isStubReturnSite() const { return !code; }
        ALWAYS_INLINE std::optional<unsigned> shownFrameCountFor(bool hasShownFrame, bool calleeWasEntered) const;
        // The frame of an entrypoint that called the body in tail position and stays on the stack to return a known result. The body may have left by a tail call since.
        bool isOfEntrypointAtTailCall() const { return outermostVisibleFunction == OutermostVisibleFunction::None && site.isTailCall(); }
        const HandlerRecord* exceptionHandler() const { return handler && handler->catchesExceptions() ? &*handler : nullptr; }

        const void* address { nullptr };
        CompiledFunctionID code;
        CallSiteRecord site { CallSiteIndex(0), 0 };
        bool innermostIsInlinedConstructCall : 1 { false };
        bool rejectsOnThrow : 1 { false };
        bool isInModuleBody : 1 { false };
        bool innermostIsAsync : 1 { false };
        // Image::calleeOfSiteAt() returns a value.
        bool hasRecordedCallee : 1 { false };
        // Image::constructedCalleeAt() has an answer.
        bool namesConstructedCallee : 1 { false };
        // Of the frames that were inlined around the site and whose callee Image::calleeOfSiteAt() knows: FunctionDescription::recordsCallee() holds of one, recordsCalleeOnceRenamed() holds of one.
        bool hasInlinedFrameRecordingCallee : 1 { false };
        bool hasInlinedFrameRecordingCalleeOnceRenamed : 1 { false };
        // leavesByTailCall(site.calledBuiltin): the frame of the built-in is listed only while its callee has not been entered.
        bool namesTailCallingBuiltin : 1 { false };
        // Once the callee of the site has been entered.
        OutermostVisibleFunction outermostVisibleFunction : 2 { OutermostVisibleFunction::Listed };
        TraceVisibility innermostVisibility { TraceVisibility::Visible };
        StubFrameKind stubFrameKind { StubFrameKind::Plain };
        FunctionID innermostFunction;
        uint16_t shownFrameCount { 0 };
        static constexpr uint8_t tooManyFramesToCount = UINT8_MAX;
        uint8_t shownFrameCountOnTop { 0 };
        // No frame above is shown, and the tail call of the site has taken its frame from the stack.
        uint8_t shownFrameCountOnTopBehindTailCall { 0 };
        std::optional<HandlerRecord> handler;
        ImageFrame frame { };
    };

    // The next resolve() may move or replace the entry that either of the two returns.
    ALWAYS_INLINE const Entry* find(const void* returnAddress) const;
    // Nothing for an address that runs no compiled code.
    const Entry* resolve(const Image&, const void* returnAddress);
    // For an entry of this cache, or none: a call whose stub reads the method. Where the read has no site, the throw site of the call says where it is, and the entry gets that as its firstReadSite.
    const Entry* withMethodReadSite(const Image&, const Entry*);
    std::optional<SourcePosition> positionOf(const Image&, CompiledFunctionID, CallSiteIndex);
    // It remembers the inlined frames of the functions that were asked for lately.
    InlineFrameReader& inlineFrames() { return m_inlineFrames; }

    static constexpr size_t initialNumberOfEntries = 128;
    static constexpr size_t maxNumberOfEntries = 2048;
    static constexpr unsigned numberOfPositions = 16;

private:
    friend struct CallSiteCacheSlots;

    size_t primaryIndex(const void* address) const { return (std::bit_cast<uintptr_t>(address) >> 2) & m_indexMask; }
    size_t secondaryIndex(const void* address) const { return std::bit_cast<uintptr_t>(address) * 0x9e3779b97f4a7c15ull >> m_secondaryIndexShift; }

    void setNumberOfEntries(size_t);
    Entry* findEmptySlot(const void* address);
    Entry& slotForNewEntry(const void* address);

    struct Position {
        CompiledFunctionID code;
        CallSiteIndex callSite;
        SourcePosition position;
    };

    const Entry* m_table { nullptr };
    size_t m_indexMask { 0 };
    unsigned m_secondaryIndexShift { 63 };
    bool m_evictsSecondary { false };
    size_t m_numberOfFilledEntries { 0 };
    size_t m_numberOfEvictions { 0 };
    FixedVector<Entry> m_entries;
    DecodedInlineFrames m_decodedInlineFrames;
    InlineFrameReader m_inlineFrames { m_decodedInlineFrames };
    std::array<Position, numberOfPositions> m_positions;
};
static_assert(sizeof(CallSiteCache::Entry) <= 64);
static_assert(std::has_single_bit(CallSiteCache::initialNumberOfEntries) && std::has_single_bit(CallSiteCache::maxNumberOfEntries));
static_assert(CallSiteCache::initialNumberOfEntries <= CallSiteCache::maxNumberOfEntries);

ALWAYS_INLINE std::optional<unsigned> CallSiteCache::Entry::shownFrameCountFor(bool hasShownFrame, bool calleeWasEntered) const
{
    ASSERT(!isStubReturnSite());
    if (namesTailCallingBuiltin) [[unlikely]]
        return std::nullopt;
    bool isTailDeleted = site.isTailDeleted(calleeWasEntered);
    if (hasShownFrame) {
        if (site.isTailCall() && !isTailDeleted) [[unlikely]]
            return std::nullopt;
        return static_cast<unsigned>(shownFrameCount);
    }
    uint8_t count = isTailDeleted ? shownFrameCountOnTopBehindTailCall : shownFrameCountOnTop;
    if (count == tooManyFramesToCount) [[unlikely]]
        return std::nullopt;
    return static_cast<unsigned>(count);
}

ALWAYS_INLINE const CallSiteCache::Entry* CallSiteCache::find(const void* returnAddress) const
{
    if (!returnAddress || !m_table) [[unlikely]]
        return nullptr;
    const Entry& primary = m_table[primaryIndex(returnAddress)];
    if (primary.address == returnAddress) [[likely]]
        return &primary;
    const Entry& secondary = m_table[secondaryIndex(returnAddress)];
    return secondary.address == returnAddress ? &secondary : nullptr;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
