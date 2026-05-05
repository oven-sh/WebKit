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

#include "config.h"
#include "TemporalZonedDateTimePrototype.h"

#include "JSBigInt.h"
#include "JSCInlines.h"
#include "TemporalInstant.h"
#include "TemporalPlainDate.h"
#include "TemporalPlainDateTime.h"
#include "TemporalPlainTime.h"
#include "TemporalZonedDateTime.h"

namespace JSC {

static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncEquals);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToString);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToJSON);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToLocaleString);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncValueOf);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToInstant);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToPlainDate);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToPlainTime);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToPlainDateTime);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncWithTimeZone);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterCalendarId);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterTimeZoneId);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterEpochMilliseconds);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterEpochNanoseconds);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterOffsetNanoseconds);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterOffset);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterYear);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMonth);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMonthCode);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDay);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterHour);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMinute);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterSecond);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMillisecond);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMicrosecond);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterNanosecond);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDayOfWeek);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDayOfYear);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterWeekOfYear);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDaysInWeek);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDaysInMonth);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDaysInYear);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMonthsInYear);
static JSC_DECLARE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterInLeapYear);

}

#include "TemporalZonedDateTimePrototype.lut.h"

namespace JSC {

const ClassInfo TemporalZonedDateTimePrototype::s_info = { "Temporal.ZonedDateTime"_s, &Base::s_info, &zonedDateTimePrototypeTable, nullptr, CREATE_METHOD_TABLE(TemporalZonedDateTimePrototype) };

/* Source for TemporalZonedDateTimePrototype.lut.h
@begin zonedDateTimePrototypeTable
  equals             temporalZonedDateTimePrototypeFuncEquals                DontEnum|Function 1
  toString           temporalZonedDateTimePrototypeFuncToString              DontEnum|Function 0
  toJSON             temporalZonedDateTimePrototypeFuncToJSON                DontEnum|Function 0
  toLocaleString     temporalZonedDateTimePrototypeFuncToLocaleString        DontEnum|Function 0
  valueOf            temporalZonedDateTimePrototypeFuncValueOf               DontEnum|Function 0
  toInstant          temporalZonedDateTimePrototypeFuncToInstant             DontEnum|Function 0
  toPlainDate        temporalZonedDateTimePrototypeFuncToPlainDate           DontEnum|Function 0
  toPlainTime        temporalZonedDateTimePrototypeFuncToPlainTime           DontEnum|Function 0
  toPlainDateTime    temporalZonedDateTimePrototypeFuncToPlainDateTime       DontEnum|Function 0
  withTimeZone       temporalZonedDateTimePrototypeFuncWithTimeZone          DontEnum|Function 1
  calendarId         temporalZonedDateTimePrototypeGetterCalendarId          DontEnum|ReadOnly|CustomAccessor
  timeZoneId         temporalZonedDateTimePrototypeGetterTimeZoneId          DontEnum|ReadOnly|CustomAccessor
  epochMilliseconds  temporalZonedDateTimePrototypeGetterEpochMilliseconds   DontEnum|ReadOnly|CustomAccessor
  epochNanoseconds   temporalZonedDateTimePrototypeGetterEpochNanoseconds    DontEnum|ReadOnly|CustomAccessor
  offsetNanoseconds  temporalZonedDateTimePrototypeGetterOffsetNanoseconds   DontEnum|ReadOnly|CustomAccessor
  offset             temporalZonedDateTimePrototypeGetterOffset              DontEnum|ReadOnly|CustomAccessor
  year               temporalZonedDateTimePrototypeGetterYear                DontEnum|ReadOnly|CustomAccessor
  month              temporalZonedDateTimePrototypeGetterMonth               DontEnum|ReadOnly|CustomAccessor
  monthCode          temporalZonedDateTimePrototypeGetterMonthCode           DontEnum|ReadOnly|CustomAccessor
  day                temporalZonedDateTimePrototypeGetterDay                 DontEnum|ReadOnly|CustomAccessor
  hour               temporalZonedDateTimePrototypeGetterHour                DontEnum|ReadOnly|CustomAccessor
  minute             temporalZonedDateTimePrototypeGetterMinute              DontEnum|ReadOnly|CustomAccessor
  second             temporalZonedDateTimePrototypeGetterSecond              DontEnum|ReadOnly|CustomAccessor
  millisecond        temporalZonedDateTimePrototypeGetterMillisecond         DontEnum|ReadOnly|CustomAccessor
  microsecond        temporalZonedDateTimePrototypeGetterMicrosecond         DontEnum|ReadOnly|CustomAccessor
  nanosecond         temporalZonedDateTimePrototypeGetterNanosecond          DontEnum|ReadOnly|CustomAccessor
  dayOfWeek          temporalZonedDateTimePrototypeGetterDayOfWeek           DontEnum|ReadOnly|CustomAccessor
  dayOfYear          temporalZonedDateTimePrototypeGetterDayOfYear           DontEnum|ReadOnly|CustomAccessor
  weekOfYear         temporalZonedDateTimePrototypeGetterWeekOfYear          DontEnum|ReadOnly|CustomAccessor
  daysInWeek         temporalZonedDateTimePrototypeGetterDaysInWeek          DontEnum|ReadOnly|CustomAccessor
  daysInMonth        temporalZonedDateTimePrototypeGetterDaysInMonth         DontEnum|ReadOnly|CustomAccessor
  daysInYear         temporalZonedDateTimePrototypeGetterDaysInYear          DontEnum|ReadOnly|CustomAccessor
  monthsInYear       temporalZonedDateTimePrototypeGetterMonthsInYear        DontEnum|ReadOnly|CustomAccessor
  inLeapYear         temporalZonedDateTimePrototypeGetterInLeapYear          DontEnum|ReadOnly|CustomAccessor
@end
*/

TemporalZonedDateTimePrototype* TemporalZonedDateTimePrototype::create(VM& vm, JSGlobalObject* globalObject, Structure* structure)
{
    auto* prototype = new (NotNull, allocateCell<TemporalZonedDateTimePrototype>(vm)) TemporalZonedDateTimePrototype(vm, structure);
    prototype->finishCreation(vm, globalObject);
    return prototype;
}

Structure* TemporalZonedDateTimePrototype::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

TemporalZonedDateTimePrototype::TemporalZonedDateTimePrototype(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void TemporalZonedDateTimePrototype::finishCreation(VM& vm, JSGlobalObject*)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    JSC_TO_STRING_TAG_WITHOUT_TRANSITION();
}

#define REQUIRE_ZDT(varname, thisValue, methodName) \
    auto* varname = dynamicDowncast<TemporalZonedDateTime>(thisValue); \
    if (!varname) \
        return throwVMTypeError(globalObject, scope, "Temporal.ZonedDateTime.prototype." methodName " called on value that's not a ZonedDateTime"_s)

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.equals
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncEquals, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "equals");

    auto* other = TemporalZonedDateTime::from(globalObject, callFrame->argument(0), jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });

    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(zdt->equals(globalObject, other))));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tostring
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToString, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "toString");
    RELEASE_AND_RETURN(scope, JSValue::encode(jsString(vm, zdt->toString(globalObject, callFrame->argument(0)))));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tojson
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToJSON, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "toJSON");
    return JSValue::encode(jsString(vm, zdt->toString()));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tolocalestring
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToLocaleString, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "toLocaleString");
    return JSValue::encode(jsString(vm, zdt->toString()));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.valueof
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncValueOf, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "Temporal.ZonedDateTime.prototype.valueOf must not be called. To compare ZonedDateTime values, use Temporal.ZonedDateTime.compare"_s);
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toinstant
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToInstant, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "toInstant");
    return JSValue::encode(TemporalInstant::create(vm, globalObject->instantStructure(), zdt->exactTime()));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindate
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToPlainDate, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "toPlainDate");
    auto date = zdt->plainDate();
    return JSValue::encode(TemporalPlainDate::create(vm, globalObject->plainDateStructure(), WTF::move(date)));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaintime
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToPlainTime, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "toPlainTime");
    auto time = zdt->plainTime();
    return JSValue::encode(TemporalPlainTime::create(vm, globalObject->plainTimeStructure(), WTF::move(time)));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindatetime
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncToPlainDateTime, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "toPlainDateTime");
    auto [date, time] = zdt->plainDateTime();
    return JSValue::encode(TemporalPlainDateTime::create(vm, globalObject->plainDateTimeStructure(), WTF::move(date), WTF::move(time)));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withtimezone
JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimePrototypeFuncWithTimeZone, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, callFrame->thisValue(), "withTimeZone");

    auto timeZone = TemporalZonedDateTime::toTimeZoneIdentifier(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT(timeZone);

    return JSValue::encode(TemporalZonedDateTime::create(vm, globalObject->zonedDateTimeStructure(), zdt->exactTime(), timeZone.value()));
}

// --- Getters -----------------------------------------------------------

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterCalendarId, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "calendarId");
    UNUSED_VARIABLE(zdt);
    return JSValue::encode(jsString(vm, String::fromLatin1("iso8601")));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterTimeZoneId, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "timeZoneId");
    return JSValue::encode(jsString(vm, zdt->timeZone().toString()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterEpochMilliseconds, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "epochMilliseconds");
    return JSValue::encode(jsNumber(zdt->exactTime().floorEpochMilliseconds()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterEpochNanoseconds, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "epochNanoseconds");
    RELEASE_AND_RETURN(scope, JSValue::encode(JSBigInt::createFrom(globalObject, zdt->exactTime().epochNanoseconds())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterOffsetNanoseconds, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "offsetNanoseconds");
    return JSValue::encode(jsNumber(static_cast<double>(zdt->offsetNanoseconds())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterOffset, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "offset");
    return JSValue::encode(jsString(vm, ISO8601::formatTimeZoneOffsetString(zdt->offsetNanoseconds())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterYear, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "year");
    return JSValue::encode(jsNumber(zdt->plainDate().year()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMonth, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "month");
    return JSValue::encode(jsNumber(zdt->plainDate().month()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMonthCode, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "monthCode");
    return JSValue::encode(jsNontrivialString(vm, ISO8601::monthCode(zdt->plainDate().month())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDay, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "day");
    return JSValue::encode(jsNumber(zdt->plainDate().day()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterHour, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "hour");
    return JSValue::encode(jsNumber(zdt->plainTime().hour()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMinute, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "minute");
    return JSValue::encode(jsNumber(zdt->plainTime().minute()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterSecond, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "second");
    return JSValue::encode(jsNumber(zdt->plainTime().second()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMillisecond, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "millisecond");
    return JSValue::encode(jsNumber(zdt->plainTime().millisecond()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMicrosecond, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "microsecond");
    return JSValue::encode(jsNumber(zdt->plainTime().microsecond()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterNanosecond, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "nanosecond");
    return JSValue::encode(jsNumber(zdt->plainTime().nanosecond()));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDayOfWeek, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "dayOfWeek");
    return JSValue::encode(jsNumber(ISO8601::dayOfWeek(zdt->plainDate())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDayOfYear, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "dayOfYear");
    return JSValue::encode(jsNumber(ISO8601::dayOfYear(zdt->plainDate())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterWeekOfYear, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "weekOfYear");
    return JSValue::encode(jsNumber(ISO8601::weekOfYear(zdt->plainDate())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDaysInWeek, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "daysInWeek");
    UNUSED_VARIABLE(zdt);
    return JSValue::encode(jsNumber(7));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDaysInMonth, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "daysInMonth");
    auto date = zdt->plainDate();
    return JSValue::encode(jsNumber(ISO8601::daysInMonth(date.year(), date.month())));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterDaysInYear, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "daysInYear");
    return JSValue::encode(jsNumber(isLeapYear(zdt->plainDate().year()) ? 366 : 365));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterMonthsInYear, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "monthsInYear");
    UNUSED_VARIABLE(zdt);
    return JSValue::encode(jsNumber(12));
}

JSC_DEFINE_CUSTOM_GETTER(temporalZonedDateTimePrototypeGetterInLeapYear, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    REQUIRE_ZDT(zdt, JSValue::decode(thisValue), "inLeapYear");
    return JSValue::encode(jsBoolean(isLeapYear(zdt->plainDate().year())));
}

#undef REQUIRE_ZDT

} // namespace JSC
