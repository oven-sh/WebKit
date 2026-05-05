/*
 *  Copyright (C) 2021 Igalia S.L. All rights reserved.
 *  Copyright (C) 2021 Apple Inc. All rights reserved.
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

#include "config.h"
#include "TemporalNow.h"

#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSObjectInlines.h"
#include "ObjectPrototype.h"
#include "TemporalInstant.h"
#include "TemporalPlainDate.h"
#include "TemporalPlainDateTime.h"
#include "TemporalPlainTime.h"
#include "TemporalTimeZone.h"
#include "TemporalZonedDateTime.h"

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(TemporalNow);

static JSC_DECLARE_HOST_FUNCTION(temporalNowFuncInstant);
static JSC_DECLARE_HOST_FUNCTION(temporalNowFuncTimeZoneId);
static JSC_DECLARE_HOST_FUNCTION(temporalNowFuncZonedDateTimeISO);
static JSC_DECLARE_HOST_FUNCTION(temporalNowFuncPlainDateTimeISO);
static JSC_DECLARE_HOST_FUNCTION(temporalNowFuncPlainDateISO);
static JSC_DECLARE_HOST_FUNCTION(temporalNowFuncPlainTimeISO);

} // namespace JSC

#include "TemporalNow.lut.h"

namespace JSC {

/* Source for TemporalNow.lut.h
@begin temporalNowTable
    instant           temporalNowFuncInstant            DontEnum|Function 0
    timeZoneId        temporalNowFuncTimeZoneId         DontEnum|Function 0
    zonedDateTimeISO  temporalNowFuncZonedDateTimeISO   DontEnum|Function 0
    plainDateTimeISO  temporalNowFuncPlainDateTimeISO   DontEnum|Function 0
    plainDateISO      temporalNowFuncPlainDateISO       DontEnum|Function 0
    plainTimeISO      temporalNowFuncPlainTimeISO       DontEnum|Function 0
@end
*/

const ClassInfo TemporalNow::s_info = { "Temporal.Now"_s, &Base::s_info, &temporalNowTable, nullptr, CREATE_METHOD_TABLE(TemporalNow) };

TemporalNow::TemporalNow(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

TemporalNow* TemporalNow::create(VM& vm, Structure* structure)
{
    TemporalNow* object = new (NotNull, allocateCell<TemporalNow>(vm)) TemporalNow(vm, structure);
    object->finishCreation(vm);
    return object;
}

Structure* TemporalNow::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, globalObject->objectPrototype(), TypeInfo(ObjectType, StructureFlags), info());
}

void TemporalNow::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    JSC_TO_STRING_TAG_WITHOUT_TRANSITION();
}

// https://tc39.es/proposal-temporal/#sec-temporal.now.instant
JSC_DEFINE_HOST_FUNCTION(temporalNowFuncInstant, (JSGlobalObject* globalObject, CallFrame*))
{
    return JSValue::encode(TemporalInstant::tryCreateIfValid(globalObject, ISO8601::ExactTime::now()));
}

// https://tc39.es/proposal-temporal/#sec-temporal.now.timezoneid
// https://tc39.es/proposal-temporal/#sec-temporal-systemtimezoneidentifier
JSC_DEFINE_HOST_FUNCTION(temporalNowFuncTimeZoneId, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    return JSValue::encode(jsNontrivialString(vm, vm.dateCache.defaultTimeZone().toString()));
}

static std::optional<TimeZone> resolveNowTimeZone(JSGlobalObject* globalObject, JSValue argument)
{
    VM& vm = globalObject->vm();
    if (argument.isUndefined())
        return TemporalZonedDateTime::systemTimeZone(vm);
    return TemporalZonedDateTime::toTimeZoneIdentifier(globalObject, argument);
}

// https://tc39.es/proposal-temporal/#sec-temporal.now.zoneddatetimeiso
JSC_DEFINE_HOST_FUNCTION(temporalNowFuncZonedDateTimeISO, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto timeZone = resolveNowTimeZone(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT(timeZone);

    RELEASE_AND_RETURN(scope, JSValue::encode(TemporalZonedDateTime::tryCreateIfValid(globalObject, ISO8601::ExactTime::now(), timeZone.value())));
}

// https://tc39.es/proposal-temporal/#sec-temporal.now.plaindatetimeiso
JSC_DEFINE_HOST_FUNCTION(temporalNowFuncPlainDateTimeISO, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto timeZone = resolveNowTimeZone(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT(timeZone);

    auto [date, time] = TemporalZonedDateTime::getISODateTimeFor(timeZone.value(), ISO8601::ExactTime::now());
    return JSValue::encode(TemporalPlainDateTime::create(vm, globalObject->plainDateTimeStructure(), WTF::move(date), WTF::move(time)));
}

// https://tc39.es/proposal-temporal/#sec-temporal.now.plaindateiso
JSC_DEFINE_HOST_FUNCTION(temporalNowFuncPlainDateISO, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto timeZone = resolveNowTimeZone(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT(timeZone);

    auto [date, time] = TemporalZonedDateTime::getISODateTimeFor(timeZone.value(), ISO8601::ExactTime::now());
    UNUSED_VARIABLE(time);
    return JSValue::encode(TemporalPlainDate::create(vm, globalObject->plainDateStructure(), WTF::move(date)));
}

// https://tc39.es/proposal-temporal/#sec-temporal.now.plaintimeiso
JSC_DEFINE_HOST_FUNCTION(temporalNowFuncPlainTimeISO, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto timeZone = resolveNowTimeZone(globalObject, callFrame->argument(0));
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT(timeZone);

    auto [date, time] = TemporalZonedDateTime::getISODateTimeFor(timeZone.value(), ISO8601::ExactTime::now());
    UNUSED_VARIABLE(date);
    return JSValue::encode(TemporalPlainTime::create(vm, globalObject->plainTimeStructure(), WTF::move(time)));
}

} // namespace JSC
