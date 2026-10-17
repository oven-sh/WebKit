/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTDateTimeWriter.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "DateInstance.h"
#include "JSCInlines.h"
#include <wtf/DateMath.h>

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDateNow, double, (Instance* instance))
{
    return instance->globalObject->jsDateNow();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDateToISOString, JSCell*, (Instance* instance, double timeValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr double firstTimeValueOfYear0 = -62167219200000.0;
    constexpr double firstTimeValueOfYear10000 = 253402300800000.0;
    bool hasFourDigitYear = timeValue >= firstTimeValueOfYear0 && timeValue < firstTimeValueOfYear10000;
    std::span<Latin1Character> characters;
    auto text = StringImpl::createUninitialized(hasFourDigitYear ? maxISODateTimeLength - 3 : maxISODateTimeLength, characters);
    Latin1Character* end = writeISODateTime(characters.data(), timeValue);
    ASSERT_UNUSED(end, end == characters.data() + characters.size());
    return jsNontrivialString(vm, String { WTF::move(text) });
}

namespace Typed {

using namespace B3;

std::optional<Writes> writesOfInlineDate(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::DateNow:
    case InlineBuiltin::DateGetTime:
    case InlineBuiltin::DateGetMilliseconds:
        return writesIf(arguments.empty(), Writes::Nothing);
    case InlineBuiltin::DateGetField:
    case InlineBuiltin::DateToISOString:
        return writesIf(arguments.empty(), Writes::RuntimeState);
    case InlineBuiltin::DateSetTime:
        return writesIf(arguments.size() == 1 && arguments[0].isNumber(), Writes::RuntimeState);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineDate(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& type = typeOf(result);
    auto& builtin = m_builtins.at(value.builtin());
    if (which == InlineBuiltin::DateNow)
        return numberResult(AOT_TYPED_OPERATION(DateNow), type);

    CellPointer date = nonNullish(value.child(0), m_program.stringOf(builtin.nameName));
    auto field = [&](ptrdiff_t offset) {
        return m_out.address(m_heaps.AOTTyped_runtimeState, date.value(), offset);
    };
    switch (which) {
    case InlineBuiltin::DateGetTime:
        return numberResult(Number(m_out.loadDouble(field(DateInstance::offsetOfInternalNumber()))), type);
    case InlineBuiltin::DateGetMilliseconds: {
        Number time(m_out.loadDouble(field(DateInstance::offsetOfInternalNumber())));
        return numberResult(sub(time, mul(floor(div(time, number(msPerSecond))), number(msPerSecond))), type);
    }
    case InlineBuiltin::DateSetTime: {
        Number given = in<Number>(value.arguments[0]);
        Number time = select(lessThanOrEqual(abs(given), number(WTF::maxECMAScriptTime)), add(trunc(given), number(0)), number(PNaN));
        m_out.storeDouble(time.value(), field(DateInstance::offsetOfInternalNumber()));
        LValue stale = m_out.constInt64(PlainGregorianDateTime::staleMarker().payload());
        m_out.store64(stale, field(DateInstance::offsetOfCachedGregorianDateTime()));
        m_out.store64(stale, field(DateInstance::offsetOfCachedGregorianDateTimeUTC()));
        didEmitWrite(Writes::RuntimeState);
        return numberResult(time, type);
    }
    case InlineBuiltin::DateToISOString: {
        Rep rep = repOf(type);
        LBasicBlock isInvalid = newBlockForGenericCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        Number timeValue(m_out.loadDouble(field(DateInstance::offsetOfInternalNumber())));
        orElse(equal(timeValue, timeValue), isInvalid);
        join.add(ensure(AOT_TYPED_OPERATION(DateToISOString, timeValue), m_program.at(m_program.types.string), type));
        if (!isSharedGenericPath(isInvalid)) {
            m_out.appendTo(isInvalid);
            Boxed genericPath = callBuiltinFunction(value, OperationEffects { writes, true });
            join.add(guaranteesResultType(m_program, m_function, m_function.at(result)) ? convert(genericPath, type, rep) : ensure(genericPath, type));
        }
        return join.finish();
    }
    case InlineBuiltin::DateGetField:
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }

    using DateTime = PlainGregorianDateTime;
    struct Part {
        ASCIILiteral local;
        ASCIILiteral utc;
        unsigned offset;
        unsigned width;
        bool isSigned;
        Range range;
    };
    static constexpr Part parts[] = {
        { "getFullYear"_s, "getUTCFullYear"_s, DateTime::yearOffset, DateTime::yearWidth, true, { DateTime::minYear, DateTime::maxYear } },
        { "getMonth"_s, "getUTCMonth"_s, DateTime::monthOffset, DateTime::monthWidth, false, { 0, 11 } },
        { "getDate"_s, "getUTCDate"_s, DateTime::monthDayOffset, DateTime::monthDayWidth, false, { 1, 31 } },
        { "getDay"_s, "getUTCDay"_s, DateTime::weekDayOffset, DateTime::weekDayWidth, false, { 0, 6 } },
        { "getHours"_s, "getUTCHours"_s, DateTime::hourOffset, DateTime::hourWidth, false, { 0, 23 } },
        { "getMinutes"_s, "getUTCMinutes"_s, DateTime::minuteOffset, DateTime::minuteWidth, false, { 0, 59 } },
        { "getSeconds"_s, "getUTCSeconds"_s, DateTime::secondOffset, DateTime::secondWidth, false, { 0, 59 } },
        { "getTimezoneOffset"_s, { }, DateTime::utcOffsetInMinuteOffset, DateTime::utcOffsetInMinuteWidth, true, { -DateTime::maxUTCOffsetInMinute, DateTime::maxUTCOffsetInMinute } },
        { "getYear"_s, { }, DateTime::yearOffset, DateTime::yearWidth, true, { DateTime::minYear - 1900, DateTime::maxYear - 1900 } },
    };
    const Part* part = nullptr;
    bool isUTC = false;
    for (auto& candidate : parts) {
        if (builtin.name == candidate.local || (!candidate.utc.isNull() && builtin.name == candidate.utc)) {
            part = &candidate;
            isUTC = builtin.name != candidate.local;
        }
    }
    RELEASE_ASSERT(part);

    Rep rep = repOf(type);
    LBasicBlock isNotComputed = newBlockForGenericCall(value);
    Join join(m_out, rep, b3TypeOf(rep));
    LValue payload = m_out.load64(field(isUTC ? DateInstance::offsetOfCachedGregorianDateTimeUTC() : DateInstance::offsetOfCachedGregorianDateTime()));
    orElse(Word32(m_out.testNonZero64(payload, integer(DateTime::validityMask).value())), isNotComputed);
    LValue bits = part->isSigned
        ? m_out.aShr(m_out.shl(payload, m_out.constInt32(64 - part->width - part->offset)), m_out.constInt32(64 - part->width))
        : m_out.bitAnd(m_out.lShr(payload, m_out.constInt32(part->offset)), m_out.constInt64((1ULL << part->width) - 1));
    Integer yielded(bits);
    if (builtin.name == "getTimezoneOffset"_s)
        yielded = neg(yielded);
    else if (builtin.name == "getYear"_s)
        yielded = sub(yielded, integer(1900));
    join.add(ensure(yielded, Type::integer(part->range), type));

    if (!isSharedGenericPath(isNotComputed)) {
        m_out.appendTo(isNotComputed);
        Boxed genericPath = callBuiltinFunction(value, OperationEffects { writes, true });
        join.add(guaranteesResultType(m_program, m_function, m_function.at(result)) ? convert(genericPath, type, rep) : ensure(genericPath, type));
    }
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
