/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2008-2021 Apple Inc. All rights reserved.
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "InternalFunction.h"

namespace JSC {

class DatePrototype;
class GetterSetter;

class DateConstructor final : public InternalFunction {
public:
    typedef InternalFunction Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags | HasStaticPropertyTable;

    static DateConstructor* create(VM& vm, Structure* structure, DatePrototype* datePrototype)
    {
        DateConstructor* constructor = new (NotNull, allocateCell<DateConstructor>(vm)) DateConstructor(vm, structure);
        constructor->finishCreation(vm, datePrototype);
        return constructor;
    }

    DECLARE_INFO;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

private:
    DateConstructor(VM&, Structure*);
    void finishCreation(VM&, DatePrototype*);
};
STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(DateConstructor, InternalFunction);

JSObject* constructDate(JSGlobalObject*, JSValue newTarget, const ArgList&);
JSValue dateNowImpl(JSGlobalObject*);

// MakeDay takes a year or a month count of at most 12 * 2^53 in magnitude. Up to there floor(month / 12) is an
// exact Number, so the 64-bit integer arithmetic below is MakeDay's. NaN fails the comparison too.
constexpr static inline bool isYearOrMonthForMakeDay(double value)
{
    return std::abs(value) <= 12 * (maxSafeInteger() + 1);
}

// https://tc39.es/ecma262/#sec-makeday steps 5 to 7, for a year and a month count that pass isYearOrMonthForMakeDay():
// year becomes year + floor(month / 12), month becomes month modulo 12. The split is exact in 64-bit integers. In a
// double, month / 12 rounds beyond 2^53. Returns false when the year does not fit an int32_t.
constexpr static inline bool normalizeYearAndMonthForMakeDay(int64_t& year, int64_t& month)
{
    if (month < 0 || month > 11) {
        int64_t additionalYears = month / 12;
        month %= 12;
        if (month < 0) {
            month += 12;
            --additionalYears;
        }
        year += additionalYears;
    }
    return year == static_cast<int32_t>(year);
}

// https://tc39.es/ecma262/#sec-makeday
constexpr static inline double makeDay(double year, double month, double date)
{
    if (!isYearOrMonthForMakeDay(year) || !isYearOrMonthForMakeDay(month))
        return PNaN;
    int64_t normalizedYear = static_cast<int64_t>(year);
    int64_t normalizedMonth = static_cast<int64_t>(month);
    if (!normalizeYearAndMonthForMakeDay(normalizedYear, normalizedMonth))
        return PNaN;
    double days = dateToDaysFrom1970(static_cast<int32_t>(normalizedYear), static_cast<int32_t>(normalizedMonth), 1);
    return days + date - 1;
}

// https://tc39.es/ecma262/#sec-makedate
constexpr static inline double makeDate(double day, double time)
{
#if COMPILER(CLANG)
#pragma STDC FP_CONTRACT OFF
#endif
    return (day * msPerDay) + time;
}

// https://tc39.es/ecma262/#sec-maketime
constexpr static inline double makeTime(double hour, double min, double sec, double ms)
{
#if COMPILER(CLANG)
#pragma STDC FP_CONTRACT OFF
#endif
    return (((hour * msPerHour) + min * msPerMinute) + sec * msPerSecond) + ms;
}
} // namespace JSC
