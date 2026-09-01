/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2004-2023 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 *
 */

#include "config.h"
#include "DateInstance.h"

#include "JSCConfig.h"
#include "JSCInlines.h"
#include "JSDateMath.h"

namespace JSC {

const ClassInfo DateInstance::s_info = { "Date"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(DateInstance) };

DateInstance::DateInstance(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void DateInstance::finishCreation(VM& vm, double time)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    m_internalNumber = timeClip(time);
}

// An instance that has never decomposed anything still holds the time value it was built with, so
// some other Date may well have decomposed the same value already. Once it has been walked forward
// its values are its own and the shared memo can only miss.
static DateCache::UseSharedCache useSharedCacheFor(PlainGregorianDateTime cached)
{
    return cached.hasNeverBeenComputed() ? DateCache::UseSharedCache::Yes : DateCache::UseSharedCache::No;
}

// GIL-off (g_jscConfig.gilOffProcess): the per-instance breakdown cache is BYPASSED
// (SPEC-ungil §N.3). The cell is reachable from N mutator threads; a breakdown computed
// from one thread's snapshot of the time value and stored here could land after another
// thread's setInternalNumber() marked the words stale, leaving a breakdown that no longer
// matches m_internalNumber with nothing to invalidate it. So GIL-off these functions
// compute from the caller's snapshot `milli` through the DateCache (which GIL-off runs on
// the calling thread's own instance, JSDateMath.h live()) and never write
// m_cachedGregorianDateTime{,UTC}:
// the words stay "never computed" for the life of the process, the inline fast paths here
// and in the DFG/FTL always miss into this slow path, and DateCache is never told a local
// breakdown was cached (so the time-zone-change sweep of the Date space never runs GIL-off).
// The latch is fixed before any JS runs. GIL-on / flag-off: the original caching path.
PlainGregorianDateTime DateInstance::calculateGregorianDateTime(DateCache& cache, double milli) const
{
    if (std::isnan(milli))
        return { };

    if (g_jscConfig.gilOffProcess) [[unlikely]]
        return cache.msToGregorianDateTime(milli, TimeType::LocalTime);

    m_cachedGregorianDateTime = cache.msToGregorianDateTime(milli, TimeType::LocalTime, useSharedCacheFor(m_cachedGregorianDateTime));
    cache.noteCachedLocalGregorianDateTime();
    return m_cachedGregorianDateTime;
}

PlainGregorianDateTime DateInstance::calculateGregorianDateTimeUTC(DateCache& cache, double milli) const
{
    if (std::isnan(milli))
        return { };

    if (g_jscConfig.gilOffProcess) [[unlikely]]
        return cache.msToGregorianDateTime(milli, TimeType::UTCTime);

    m_cachedGregorianDateTimeUTC = cache.msToGregorianDateTime(milli, TimeType::UTCTime, useSharedCacheFor(m_cachedGregorianDateTimeUTC));
    return m_cachedGregorianDateTimeUTC;
}

} // namespace JSC
