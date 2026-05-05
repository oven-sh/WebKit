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
#include "TemporalZonedDateTimeConstructor.h"

#include "JSBigInt.h"
#include "JSCInlines.h"
#include "TemporalZonedDateTime.h"
#include "TemporalZonedDateTimePrototype.h"

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(TemporalZonedDateTimeConstructor);

static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimeConstructorFuncFrom);
static JSC_DECLARE_HOST_FUNCTION(temporalZonedDateTimeConstructorFuncCompare);

}

#include "TemporalZonedDateTimeConstructor.lut.h"

namespace JSC {

const ClassInfo TemporalZonedDateTimeConstructor::s_info = { "Function"_s, &Base::s_info, &temporalZonedDateTimeConstructorTable, nullptr, CREATE_METHOD_TABLE(TemporalZonedDateTimeConstructor) };

/* Source for TemporalZonedDateTimeConstructor.lut.h
@begin temporalZonedDateTimeConstructorTable
  from     temporalZonedDateTimeConstructorFuncFrom     DontEnum|Function 1
  compare  temporalZonedDateTimeConstructorFuncCompare  DontEnum|Function 2
@end
*/

TemporalZonedDateTimeConstructor* TemporalZonedDateTimeConstructor::create(VM& vm, Structure* structure, TemporalZonedDateTimePrototype* prototype)
{
    auto* constructor = new (NotNull, allocateCell<TemporalZonedDateTimeConstructor>(vm)) TemporalZonedDateTimeConstructor(vm, structure);
    constructor->finishCreation(vm, prototype);
    return constructor;
}

Structure* TemporalZonedDateTimeConstructor::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(InternalFunctionType, StructureFlags), info());
}

static JSC_DECLARE_HOST_FUNCTION(callTemporalZonedDateTime);
static JSC_DECLARE_HOST_FUNCTION(constructTemporalZonedDateTime);

TemporalZonedDateTimeConstructor::TemporalZonedDateTimeConstructor(VM& vm, Structure* structure)
    : Base(vm, structure, callTemporalZonedDateTime, constructTemporalZonedDateTime)
{
}

void TemporalZonedDateTimeConstructor::finishCreation(VM& vm, TemporalZonedDateTimePrototype* prototype)
{
    Base::finishCreation(vm, 2, "ZonedDateTime"_s, PropertyAdditionMode::WithoutStructureTransition);
    putDirectWithoutTransition(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    prototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, this, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

// https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime
JSC_DEFINE_HOST_FUNCTION(constructTemporalZonedDateTime, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* newTarget = asObject(callFrame->newTarget());
    Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, zonedDateTimeStructure, newTarget, callFrame->jsCallee());
    RETURN_IF_EXCEPTION(scope, { });

    // 2. Set epochNanoseconds to ? ToBigInt(epochNanoseconds).
    JSValue epochNsValue = callFrame->argument(0);
    JSValue epochNs = epochNsValue.toBigInt(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    Int128 total { 0 };
    bool bigIntTooLong = false;
#if USE(BIGINT32)
    if (epochNs.isBigInt32())
        total = epochNs.bigInt32AsInt32();
    else
#endif
    {
        JSBigInt* bigint = asHeapBigInt(epochNs);
        if constexpr (sizeof(JSBigInt::Digit) == 4) {
            Int128 d0 { bigint->length() > 0 ? bigint->digit(0) : 0 };
            Int128 d1 { bigint->length() > 1 ? bigint->digit(1) : 0 };
            Int128 d2 { bigint->length() > 2 ? bigint->digit(2) : 0 };
            total = d2 << 64 | d1 << 32 | d0;
            bigIntTooLong = bigint->length() > 3;
        } else {
            ASSERT(sizeof(JSBigInt::Digit) == 8);
            if (bigint->length() > 1 && (bigint->digit(1) & 0x8000'0000'0000'0000)) {
                total = 0;
                bigIntTooLong = true;
            } else {
                Int128 low { bigint->length() > 0 ? bigint->digit(0) : 0 };
                Int128 high { bigint->length() > 1 ? bigint->digit(1) : 0 };
                total = high << 64 | low;
                bigIntTooLong = bigint->length() > 2;
            }
        }
        if (bigint->sign())
            total = -total;
    }

    ISO8601::ExactTime exactTime { total };
    if (bigIntTooLong || !exactTime.isValid()) {
        throwRangeError(globalObject, scope, "epoch nanoseconds is outside of the supported range for Temporal.ZonedDateTime"_s);
        return { };
    }

    // 4. Let timeZone be ? ToTemporalTimeZoneIdentifier(timeZoneLike).
    auto timeZone = TemporalZonedDateTime::toTimeZoneIdentifier(globalObject, callFrame->argument(1));
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT(timeZone);

    // 5. Let calendar be ? ToTemporalCalendarIdentifier(calendarLike, "iso8601").
    JSValue calendarLike = callFrame->argument(2);
    if (!calendarLike.isUndefined()) {
        auto calendarString = calendarLike.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        auto calendarID = TemporalCalendar::isBuiltinCalendar(calendarString);
        if (!calendarID) {
            throwRangeError(globalObject, scope, "invalid calendar"_s);
            return { };
        }
        if (calendarID.value() != iso8601CalendarID()) {
            throwRangeError(globalObject, scope, "only the iso8601 calendar is currently supported"_s);
            return { };
        }
    }

    RELEASE_AND_RETURN(scope, JSValue::encode(TemporalZonedDateTime::tryCreateIfValid(globalObject, exactTime, timeZone.value(), structure)));
}

JSC_DEFINE_HOST_FUNCTION(callTemporalZonedDateTime, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return JSValue::encode(throwConstructorCannotBeCalledAsFunctionTypeError(globalObject, scope, "ZonedDateTime"_s));
}

JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimeConstructorFuncFrom, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(TemporalZonedDateTime::from(globalObject, callFrame->argument(0), callFrame->argument(1)));
}

JSC_DEFINE_HOST_FUNCTION(temporalZonedDateTimeConstructorFuncCompare, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSValue::encode(TemporalZonedDateTime::compare(globalObject, callFrame->argument(0), callFrame->argument(1)));
}

} // namespace JSC
