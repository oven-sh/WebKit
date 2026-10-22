/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallSiteCache.h"

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"
#include "AOTShownFrameFilter.h"
#include <utility>
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

    FunctionID rootFunction = image.rootFunctionOf(location.function);
    FunctionDescription root(rootFunction);
    entry.rejectsOnThrow = root.rejectsOnThrow();
    if (!entry.site.inlineFrame) {
        entry.innermostFunction = rootFunction;
        entry.innermostVisibility = root.visibility();
        entry.shownFrameCount = ShownFrameFilter(true).shows(entry.innermostVisibility, entry.site.isTailCall);
        entry.shownFrameCountOnTop = ShownFrameFilter(false).shows(entry.innermostVisibility, false);
        return entry;
    }

    LogicalFrame innermost = innermostLogicalFrame(image, reader, { entry.code, entry.site.callSite, entry.site.inlineFrame, false });
    entry.innermostFunction = innermost.function;
    entry.innermostIsInlinedConstructCall = innermost.isInlinedConstructCall;
    entry.innermostVisibility = FunctionDescription(innermost.function).visibility();

    entry.shownFrameCount = countShownFrames(image, reader, entry.code, entry.site, ShownFrameFilter(true), entry.site.isTailCall);
    entry.shownFrameCountOnTop = countShownFrames(image, reader, entry.code, entry.site, ShownFrameFilter(false), false);
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
