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

enum class OutermostVisibleFunction : uint8_t { None, Listed, HiddenBehindTailCall };

class CallSiteCache {
    WTF_MAKE_TZONE_ALLOCATED(CallSiteCache);
    WTF_MAKE_NONCOPYABLE(CallSiteCache);
public:
    CallSiteCache();

    struct Entry {
        bool isStubReturnSite() const { return !code; }
        ALWAYS_INLINE std::optional<unsigned> shownFrameCountFor(bool hasShownFrame, bool calleeWasEntered) const;
        bool isOfEntrypointAtTailCall() const { return outermostVisibleFunction == OutermostVisibleFunction::None && site.isTailCall(); }
        ALWAYS_INLINE const HandlerRecord* exceptionHandler(const Image& image) const
        {
            if (mayHaveHandler) [[unlikely]]
                findHandler(image);
            return handler ? &*handler : nullptr;
        }

        const void* address { nullptr };
        CompiledFunctionID code;
        CallSiteRecord site { CallSiteIndex(0), 0 };
        bool innermostIsInlinedConstructCall : 1 { false };
        bool rejectsOnThrow : 1 { false };
        bool isInModuleBody : 1 { false };
        bool innermostIsAsync : 1 { false };
        bool namesConstructedCallee : 1 { false };
        bool namesTailCallingBuiltin : 1 { false };
        mutable bool mayHaveHandler : 1 { false };
        OutermostVisibleFunction outermostVisibleFunction : 2 { OutermostVisibleFunction::Listed };
        TraceVisibility innermostVisibility { TraceVisibility::Visible };
        StubFrameKind stubFrameKind { StubFrameKind::Plain };
        FunctionID innermostFunction;
        uint16_t shownFrameCount { 0 };
        static constexpr uint8_t tooManyFramesToCount = UINT8_MAX;
        uint8_t shownFrameCountOnTop { 0 };
        uint8_t shownFrameCountOnTopBehindTailCall { 0 };
        mutable std::optional<HandlerRecord> handler;
        ImageFrame frame { };

    private:
        NEVER_INLINE void findHandler(const Image&) const;
    };

    ALWAYS_INLINE const Entry* find(const void* returnAddress) const;
    const Entry* resolve(const Image&, const void* returnAddress);
    std::optional<SourcePosition> positionOf(const Image&, CompiledFunctionID, CallSiteIndex);
    InlineFrameReader& inlineFrames() { return m_inlineFrames; }

    struct ConstructorFrames {
        const void* address { nullptr };
        uint32_t closureNumber { 0 };
        uint16_t numberOmitted { 0 };
        uint16_t numberShown { 0 };
        bool calleeWasEntered { false };
        bool isInnermostOfRun { false };
        bool hasConstructor { false };
    };
    ConstructorFrames& constructorFramesAt(const void* returnAddress) { return m_constructorFrames[secondaryHash(returnAddress) >> (64 - std::countr_zero(numberOfConstructorFrames))]; }

    static constexpr size_t initialNumberOfEntries = 128;
    static constexpr size_t maxNumberOfEntries = 2048;
    static constexpr unsigned numberOfPositions = 16;
    static constexpr unsigned numberOfConstructorFrames = 32;

private:
    friend struct CallSiteCacheSlots;

    size_t primaryIndex(const void* address) const { return (std::bit_cast<uintptr_t>(address) >> 2) & m_indexMask; }
    static uint64_t secondaryHash(const void* address) { return std::bit_cast<uintptr_t>(address) * 0x9e3779b97f4a7c15ull; }
    size_t secondaryIndex(const void* address) const { return secondaryHash(address) >> m_secondaryIndexShift; }

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
    std::array<ConstructorFrames, numberOfConstructorFrames> m_constructorFrames;
};
static_assert(std::has_single_bit(CallSiteCache::numberOfConstructorFrames));
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
