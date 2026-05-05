/*
 * Copyright (C) 2026 Oven Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ISO8601.h"
#include "JSCTimeZone.h"
#include "LazyProperty.h"
#include "TemporalCalendar.h"
#include "TemporalObject.h"

namespace JSC {

// https://tc39.es/proposal-temporal/#sec-temporal-zoneddatetime-objects
class TemporalZonedDateTime final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.temporalZonedDateTimeSpace<mode>();
    }

    static TemporalZonedDateTime* create(VM&, Structure*, ISO8601::ExactTime, TimeZone);
    static TemporalZonedDateTime* tryCreateIfValid(JSGlobalObject*, ISO8601::ExactTime, TimeZone, Structure* = nullptr);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static TemporalZonedDateTime* from(JSGlobalObject*, JSValue, JSValue options);
    static JSValue compare(JSGlobalObject*, JSValue, JSValue);

    // [[EpochNanoseconds]]
    ISO8601::ExactTime exactTime() const { return m_exactTime; }
    // [[TimeZone]]
    TimeZone timeZone() const { return m_timeZone; }
    // [[Calendar]]
    TemporalCalendar* calendar() LIFETIME_BOUND { return m_calendar.get(this); }

    // GetOffsetNanosecondsFor(timeZone, instant)
    int64_t offsetNanoseconds() const;
    // GetISODateTimeFor(timeZone, instant) -> ISO parts
    ISO8601::PlainDate plainDate() const;
    ISO8601::PlainTime plainTime() const;
    std::pair<ISO8601::PlainDate, ISO8601::PlainTime> plainDateTime() const;

    String toString(JSGlobalObject*, JSValue options) const;
    String toString(std::tuple<Precision, unsigned> precision = { Precision::Auto, 0 }, bool showOffset = true, bool showTimeZone = true, bool showCalendar = false) const;

    bool equals(JSGlobalObject*, TemporalZonedDateTime* other);

    // https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
    static std::optional<TimeZone> toTimeZoneIdentifier(JSGlobalObject*, JSValue);
    static int64_t getTimeZoneOffsetNanoseconds(const TimeZone&, ISO8601::ExactTime);
    static std::pair<ISO8601::PlainDate, ISO8601::PlainTime> getISODateTimeFor(const TimeZone&, ISO8601::ExactTime);
    static TimeZone systemTimeZone(VM&);

    // https://tc39.es/proposal-temporal/#sec-temporal-getpossibleepochnanoseconds
    static Vector<ISO8601::ExactTime, 2> getPossibleEpochNanoseconds(const TimeZone&, ISO8601::PlainDate, ISO8601::PlainTime);
    // https://tc39.es/proposal-temporal/#sec-temporal-disambiguatepossibleepochns
    static std::optional<ISO8601::ExactTime> disambiguatePossibleEpochNanoseconds(JSGlobalObject*, Vector<ISO8601::ExactTime, 2>&&, const TimeZone&, ISO8601::PlainDate, ISO8601::PlainTime, TemporalDisambiguation);
    // https://tc39.es/proposal-temporal/#sec-temporal-getepochnanosecondsfor
    static std::optional<ISO8601::ExactTime> getEpochNanosecondsFor(JSGlobalObject*, const TimeZone&, ISO8601::PlainDate, ISO8601::PlainTime, TemporalDisambiguation);

private:
    TemporalZonedDateTime(VM&, Structure*, ISO8601::ExactTime, TimeZone);
    void finishCreation(VM&);

    static TemporalZonedDateTime* toZonedDateTime(JSGlobalObject*, JSValue, JSValue options);

    ISO8601::ExactTime m_exactTime;
    TimeZone m_timeZone;
    LazyProperty<TemporalZonedDateTime, TemporalCalendar> m_calendar;
};

// https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezoneoffsetnanoseconds
int64_t getNamedTimeZoneOffsetNanoseconds(TimeZoneID, ISO8601::ExactTime);

// https://tc39.es/proposal-temporal/#sec-temporal-getisopartsfromepoch
std::pair<ISO8601::PlainDate, ISO8601::PlainTime> getISOPartsFromEpoch(ISO8601::ExactTime);

} // namespace JSC
