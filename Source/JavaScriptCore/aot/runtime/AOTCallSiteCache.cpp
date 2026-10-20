/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallSiteCache.h"

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"
#include "AOTShownFrameFilter.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC::AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CallSiteCache);

CallSiteCache::CallSiteCache() = default;

static uint16_t countShownFrames(const Image& image, InlineFrameReader& reader, CompiledFunctionID code, const CallSiteRecord& site, ShownFrameFilter filter, bool innermostIsTailDeleted)
{
    uint16_t count = 0;
    LogicalFrame frame = innermostLogicalFrame(image, reader, { code, site.callSite, site.inlineFrame, innermostIsTailDeleted });
    while (true) {
        if (filter.shows(FunctionDescription(frame.function).visibility(), frame.isTailDeleted))
            ++count;
        if (!frame.inlineFrame)
            return count;
        frame = callerOfInlinedFrame(image, reader, code, frame);
    }
}

static CallSiteCache::Entry createEntry(const Image& image, InlineFrameReader& reader, const CodeLocation& location, const void* returnAddress)
{
    CallSiteCache::Entry entry;
    entry.address = returnAddress;
    entry.code = CompiledFunctionID(location.function);
    if (auto site = image.callSiteAt(location.function, location.offset)) {
        entry.site = *site;
        entry.handler = image.handlerAt(location.function, site->callSite);
    }

    entry.rejectsOnThrow = FunctionDescription(image.rootFunctionOf(location.function)).rejectsOnThrow();
    if (image.hasFrameAtReturnOffset(location.function, location.offset))
        entry.frame = image.frameOf(location.function);

    LogicalFrame innermost = innermostLogicalFrame(image, reader, { entry.code, entry.site.callSite, entry.site.inlineFrame, false });
    entry.innermostFunction = innermost.function;
    entry.innermostIsInlinedConstructCall = innermost.isInlinedConstructCall;
    entry.innermostVisibility = FunctionDescription(innermost.function).visibility();

    entry.shownFrameCount = countShownFrames(image, reader, entry.code, entry.site, ShownFrameFilter(true), entry.site.isTailCall);
    entry.shownFrameCountOnTop = countShownFrames(image, reader, entry.code, entry.site, ShownFrameFilter(false), false);
    return entry;
}

std::optional<unsigned> CallSiteCache::Entry::shownFrameCountFor(bool hasShownFrame, bool calleeWasEntered) const
{
    if (hasShownFrame) {
        if (!site.isTailCall || calleeWasEntered)
            return shownFrameCount;
        return std::nullopt;
    }
    if (!site.isTailCall || !calleeWasEntered)
        return shownFrameCountOnTop;
    return std::nullopt;
}

const CallSiteCache::Entry* CallSiteCache::resolve(const Image& image, const void* returnAddress)
{
    if (const Entry* entry = find(returnAddress))
        return entry;

    CodeLocation location = image.locate(returnAddress);
    if (location.kind != CodeKind::Function)
        return nullptr;

    if (m_entries.isEmpty())
        m_entries = FixedVector<Entry>(numberOfEntries);

    size_t index = primaryIndex(returnAddress);
    if (m_entries[index].address) {
        size_t secondary = secondaryIndex(returnAddress);
        if (!m_entries[secondary].address)
            index = secondary;
        else {
            m_evictsSecondary = !m_evictsSecondary;
            if (m_evictsSecondary)
                index = secondary;
        }
    }

    Entry& entry = m_entries[index];
    entry = createEntry(image, m_inlineFrames, location, returnAddress);
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

} // namespace JSC::AOT

#endif // ENABLE(AOT)
