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

// GIL-off (g_jscConfig.gilOffProcess): the per-instance GregorianDateTime cache is
// BYPASSED (SPEC-ungil §N.3). Rationale: the cell (and the DateInstanceData shared
// across instances via DateInstanceCache) is reachable from N mutator threads, the
// cached GregorianDateTime is multi-word (torn reads), and the lock-free inline fast
// path in DateInstance.h / the DFG-FTL inline fast paths read the cached fields with
// plain loads we cannot make acquire here. Instead, GIL-off we NEVER write m_data or
// any DateInstanceData field on these paths: m_data stays null for the lifetime of
// the instance, so the C++ inline fast path and the JIT'd fast paths (null check on
// offsetOfData) always miss and land in the slow path below, which computes into
// per-thread scratch storage. The two scratch slots (local + UTC) keep a pointer of
// each TimeType valid simultaneously, matching every existing call site (callers use
// or copy the result before the same thread requests another conversion of the same
// TimeType). The latch is fixed before any spawned thread runs JS, so a GIL-off
// process can never observe a non-null m_data written by these functions.
// GIL-on / flag-off: code path is byte-for-byte the original caching path.
static GregorianDateTime& gilOffDateInstanceScratch(TimeType timeType)
{
    static thread_local GregorianDateTime scratchLocalTime;
    static thread_local GregorianDateTime scratchUTCTime;
    return timeType == TimeType::LocalTime ? scratchLocalTime : scratchUTCTime;
}

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

const GregorianDateTime* DateInstance::calculateGregorianDateTime(DateCache& cache, double milli) const
{
    if (std::isnan(milli))
        return nullptr;

    if (g_jscConfig.gilOffProcess) [[unlikely]] {
        GregorianDateTime& scratch = gilOffDateInstanceScratch(TimeType::LocalTime);
        cache.msToGregorianDateTime(milli, TimeType::LocalTime, scratch);
        return &scratch;
    }

    if (!m_data)
        m_data = cache.cachedDateInstanceData(milli);

    if (m_data->m_gregorianDateTimeCachedForMS != milli) {
        cache.msToGregorianDateTime(milli, TimeType::LocalTime, m_data->m_cachedGregorianDateTime);
        m_data->m_gregorianDateTimeCachedForMS = milli;
    }
    return &m_data->m_cachedGregorianDateTime;
}

const GregorianDateTime* DateInstance::calculateGregorianDateTimeUTC(DateCache& cache, double milli) const
{
    if (std::isnan(milli))
        return nullptr;

    if (g_jscConfig.gilOffProcess) [[unlikely]] {
        GregorianDateTime& scratch = gilOffDateInstanceScratch(TimeType::UTCTime);
        cache.msToGregorianDateTime(milli, TimeType::UTCTime, scratch);
        return &scratch;
    }

    if (!m_data)
        m_data = cache.cachedDateInstanceData(milli);

    if (m_data->m_gregorianDateTimeUTCCachedForMS != milli) {
        cache.msToGregorianDateTime(milli, TimeType::UTCTime, m_data->m_cachedGregorianDateTimeUTC);
        m_data->m_gregorianDateTimeUTCCachedForMS = milli;
    }
    return &m_data->m_cachedGregorianDateTimeUTC;
}

} // namespace JSC
