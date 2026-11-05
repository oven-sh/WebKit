/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDateBuiltinEmitter.h"

#if ENABLE(AOT)

#include "DateInstance.h"
#include <wtf/DateMath.h>

namespace JSC { namespace AOT { namespace Typed {

DateBuiltinEmitter::DateBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

const Machine::AbstractHeap& DateBuiltinEmitter::cachedDateTimeIn(TimeZone zone) const
{
    switch (zone) {
    case TimeZone::Local:
        return m_heaps.DateInstance_cachedGregorianDateTime;
    case TimeZone::UTC:
        return m_heaps.DateInstance_cachedGregorianDateTimeUTC;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number DateBuiltinEmitter::timeValueOf(CellPointer date)
{
    return Number(m_out.loadDouble(date.value(), m_heaps.DateInstance_internalNumber));
}

void DateBuiltinEmitter::generateDateNow(const InlineCall& call)
{
    finish(call, callOperation<Number>(call.context(), RuntimeOperation::DateNow, { }));
}

void DateBuiltinEmitter::generateDateGetTime(const InlineCall& call)
{
    finish(call, timeValueOf(cellOperand(call.receiver())));
}

void DateBuiltinEmitter::generateDateGetMilliseconds(const InlineCall& call)
{
    Number time = timeValueOf(cellOperand(call.receiver()));
    finish(call, sub(time, mul(floor(div(time, msPerSecond)), msPerSecond)));
}

void DateBuiltinEmitter::generateDateSetTime(const InlineCall& call)
{
    CellPointer date = cellOperand(call.receiver());
    Number unclippedTime = numberOperand(call.argument(0));
    Condition isInRange = lessThanOrEqual(abs(unclippedTime), WTF::maxECMAScriptTime);
    Number time = select(isInRange, add(trunc(unclippedTime), 0), PNaN);
    m_out.storeDouble(time.value(), date.value(), m_heaps.DateInstance_internalNumber);
    LValue stale = m_out.constInt64(PlainGregorianDateTime::staleMarker().payload());
    m_out.store64(stale, date.value(), m_heaps.DateInstance_cachedGregorianDateTime);
    m_out.store64(stale, date.value(), m_heaps.DateInstance_cachedGregorianDateTimeUTC);
    finish(call, time);
}

void DateBuiltinEmitter::generateDateToISOString(const InlineCall& call)
{
    Number timeValue = timeValueOf(cellOperand(call.receiver()));
    missUnless(call, equal(timeValue, timeValue));
    CellPointer text = callOperation<CellPointer>(call.context(), RuntimeOperation::DateToISOString, { timeValue });
    missUnless(call, Condition(m_out.notNull(text.value())));
    finish(call, text);
}

DateBuiltinEmitter::BitField DateBuiltinEmitter::bitFieldOf(DateField field)
{
    using DateTime = PlainGregorianDateTime;
    switch (field) {
    case DateField::FullYear:
        return { DateTime::yearOffset, DateTime::yearWidth, true, { DateTime::minYear, DateTime::maxYear } };
    case DateField::Month:
        return { DateTime::monthOffset, DateTime::monthWidth, false, { 0, 11 } };
    case DateField::Date:
        return { DateTime::monthDayOffset, DateTime::monthDayWidth, false, { 1, 31 } };
    case DateField::Day:
        return { DateTime::weekDayOffset, DateTime::weekDayWidth, false, { 0, 6 } };
    case DateField::Hours:
        return { DateTime::hourOffset, DateTime::hourWidth, false, { 0, 23 } };
    case DateField::Minutes:
        return { DateTime::minuteOffset, DateTime::minuteWidth, false, { 0, 59 } };
    case DateField::Seconds:
        return { DateTime::secondOffset, DateTime::secondWidth, false, { 0, 59 } };
    case DateField::TimezoneOffset:
        return { DateTime::utcOffsetInMinuteOffset, DateTime::utcOffsetInMinuteWidth, true, { -DateTime::maxUTCOffsetInMinute, DateTime::maxUTCOffsetInMinute } };
    case DateField::Year:
        return { DateTime::yearOffset, DateTime::yearWidth, true, { DateTime::minYear - 1900, DateTime::maxYear - 1900 } };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void DateBuiltinEmitter::generateDateGetField(const InlineCall& call)
{
    DateField field = call.parameter<DateField>(0);
    TimeZone zone = call.parameter<TimeZone>(1);
    bool hasOnlyLocalForm = field == DateField::TimezoneOffset || field == DateField::Year;
    RELEASE_ASSERT(!hasOnlyLocalForm || zone == TimeZone::Local);

    BitField bitField = bitFieldOf(field);
    Integer payload(m_out.load64(cellOperand(call.receiver()).value(), cachedDateTimeIn(zone)));
    missUnless(call, hasBits(payload, static_cast<int64_t>(PlainGregorianDateTime::validityMask)));
    LValue bits = bitField.isSigned
        ? m_out.aShr(m_out.shl(payload.value(), 64 - bitField.width - bitField.offset), 64 - bitField.width)
        : m_out.bitAnd(m_out.lShr(payload.value(), bitField.offset), (1ULL << bitField.width) - 1);
    Integer result(bits);
    if (field == DateField::TimezoneOffset)
        result = neg(result);
    else if (field == DateField::Year)
        result = sub(result, integer(1900));
    finish(call, result, bitField.range);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
