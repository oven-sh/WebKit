/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonDateTime.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonIO.h"
#include "PythonOperations.h"
#include "PythonSignatures.h"
#include "PythonText.h"

// datetime.time

namespace JSC { namespace Python {

JSValue newTimeOfDay(JSGlobalObject* globalObject, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    checkTimeArguments(globalObject, hour, minute, second, microsecond, fold);
    RETURN_IF_EXCEPTION(scope, { });
    checkTZInfo(globalObject, tzinfo);
    RETURN_IF_EXCEPTION(scope, { });
    if (!type)
        type = dateTimeModuleState(globalObject).timeType.get();
    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<TimeOfDayState>());
    auto& self = object->state<TimeOfDayState>();
    self.data = { static_cast<uint8_t>(hour), static_cast<uint8_t>(minute), static_cast<uint8_t>(second), static_cast<uint8_t>(microsecond >> 16), static_cast<uint8_t>(microsecond >> 8 & 0xFF), static_cast<uint8_t>(microsecond & 0xFF) };
    self.fold = fold;
    if (!isNone(tzinfo))
        self.tzinfo.set(vm, object, tzinfo);
    return object;
}

JSValue callClassWithFold(JSGlobalObject* globalObject, JSValue cls, MarkedArgumentBuffer& arguments, int fold)
{
    VM& vm = globalObject->vm();
    if (!fold)
        return call(globalObject, cls, arguments);
    arguments.append(jsNumber(fold));
    KeywordNames* keywordNames = KeywordNames::create(vm, CopyOnWriteArrayWithContiguous, 1);
    keywordNames->setIndex(vm, 0, internedString(vm, Identifier::fromString(vm, "fold"_s)));
    return callWithKeywords(globalObject, cls, arguments, keywordNames);
}

JSValue newTimeOfDayOfClass(JSGlobalObject* globalObject, int hour, int minute, int second, int microsecond, JSValue tzinfo, int fold, JSValue cls)
{
    if (cls == dateTimeModuleState(globalObject).timeType->object())
        return newTimeOfDay(globalObject, hour, minute, second, microsecond, tzinfo, fold);
    MarkedArgumentBuffer arguments;
    for (int field : { hour, minute, second, microsecond })
        arguments.append(jsNumber(field));
    arguments.append(tzinfo);
    return callClassWithFold(globalObject, cls, arguments, fold);
}

namespace {

bool isSaneHour(unsigned hour) { return (hour & 0x7F) < 24; }

// time_new()
PYTHON_NATIVE(timeOfDayNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    // What pickle calls it with is what __reduce__() gave.
    if (unsigned given = args.size() - 1; given >= 1 && given <= 2) {
        auto state = pickledState(globalObject, args.at(1), TimeOfDayState::size, 0, isSaneHour, "time"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (state) {
            // time_from_pickle()
            JSValue tzinfo = given == 2 ? args.at(2) : jsUndefined();
            if (!isNone(tzinfo) && !isTZInfo(globalObject, tzinfo))
                return JSValue::encode(raiseTypeError(globalObject, scope, "bad tzinfo state arg"_s));
            auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<TimeOfDayState>());
            auto& self = object->state<TimeOfDayState>();
            memcpySpan(std::span(self.data), state->span());
            if (!isNone(tzinfo))
                self.tzinfo.set(vm, object, tzinfo);
            if (self.data[0] & 1 << 7) {
                self.data[0] -= 128;
                self.fold = 1;
            }
            return JSValue::encode(object);
        }
    }

    // PyArg_ParseTupleAndKeywords(), of "|iiiiO$i": whether there are too many is seen first, and then each is made what it can be as it is come to. Whether too many were given by position is seen at the $.
    if (args.size() - 1 + args.keywordCount() > 6) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    int fields[4] = { 0, 0, 0, 0 };
    parseIntArguments(globalObject, args, 1, fields, 0);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue tzinfo = args.at(5) ? args.at(5) : jsUndefined();
    if (args.size() - 1 > 5) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    int fold = 0;
    parseIntArguments(globalObject, args, 6, { &fold, 1 }, 0);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeOfDay(globalObject, fields[0], fields[1], fields[2], fields[3], tzinfo, fold, type)));
}

// time_strptime()
PYTHON_NATIVE(timeOfDayStrptime)
{
    NATIVE_PROLOGUE();
    checkIsStrArgument(globalObject, "strptime"_s, 1, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    checkIsStrArgument(globalObject, "strptime"_s, 2, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(callStrptime(globalObject, "_strptime_datetime_time"_s, args[0], args[1], args[2])));
}

PYTHON_NATIVE(timeOfDayUTCOffset)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callUTCOffset(globalObject, stateOf<TimeOfDayState>(args[0]).tzinfoOrNone(), jsUndefined())));
}

PYTHON_NATIVE(timeOfDayDST)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callDST(globalObject, stateOf<TimeOfDayState>(args[0]).tzinfoOrNone(), jsUndefined())));
}

PYTHON_NATIVE(timeOfDayTZName)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTZName(globalObject, stateOf<TimeOfDayState>(args[0]).tzinfoOrNone(), jsUndefined())));
}

// time_repr()
PYTHON_NATIVE(timeOfDayRepr)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeOfDayState>(args[0]);
    String type = typeName(globalObject, args[0]);
    String result;
    if (self.microsecond())
        result = concatenate(type, '(', self.hour(), ", "_s, self.minute(), ", "_s, self.second(), ", "_s, self.microsecond(), ')');
    else if (self.second())
        result = concatenate(type, '(', self.hour(), ", "_s, self.minute(), ", "_s, self.second(), ')');
    else
        result = concatenate(type, '(', self.hour(), ", "_s, self.minute(), ')');
    if (self.tzinfo) {
        result = withTZInfoKeyword(globalObject, result, self.tzinfo.get());
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, withFoldKeyword(result, self.fold))));
}

PYTHON_NATIVE(timeOfDayStr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, args[0], names.attribute_isoformat)));
}

// time_isoformat()
PYTHON_NATIVE(timeOfDayISOFormat)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeOfDayState>(args[0]);
    auto specification = timeSpecificationFrom(globalObject, args.at(1), 1, self.microsecond());
    RETURN_IF_EXCEPTION(scope, { });
    StringBuilder builder;
    appendTimeAsISO(builder, *specification, self.hour(), self.minute(), self.second(), self.microsecond());
    if (self.tzinfo) {
        String offset = formatUTCOffset(globalObject, ":"_s, self.tzinfo.get(), jsUndefined());
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(offset);
    }
    return JSValue::encode(jsString(vm, builder.toString()));
}

// time_strftime()
PYTHON_NATIVE(timeOfDayStrftime)
{
    NATIVE_PROLOGUE();
    checkIsStrArgument(globalObject, "strftime"_s, 1, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = stateOf<TimeOfDayState>(args[0]);
    // The year is 1900, which means nothing, for the sake of what strftime() makes of other years.
    PyTuple* tuple = PyTuple::create(globalObject, { jsNumber(1900), jsNumber(1), jsNumber(1), jsNumber(self.hour()), jsNumber(self.minute()), jsNumber(self.second()), jsNumber(0), jsNumber(1), jsNumber(-1) });
    RELEASE_AND_RETURN(scope, JSValue::encode(wrapStrftime(globalObject, args[0], stringIn(args[1]), tuple, jsUndefined())));
}

// time_richcompare()
PYTHON_NATIVE(timeOfDayCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    auto* other = tryTimeOfDay(args[1]);
    if (!other)
        RETURN_NOT_IMPLEMENTED();
    auto& self = stateOf<TimeOfDayState>(args[0]);
    if (self.tzinfoOrNone() == other->tzinfoOrNone())
        return JSValue::encode(comparisonResult(compareBytes(self.data, other->data), op));

    JSValue offset1 = callUTCOffset(globalObject, self.tzinfoOrNone(), jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue offset2 = callUTCOffset(globalObject, other->tzinfoOrNone(), jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    auto* delta1 = tryTimeDelta(offset1);
    auto* delta2 = tryTimeDelta(offset2);
    // It is cheap if neither says how far it is from UTC, or both say the same.
    if (offset1 == offset2 || (delta1 && delta2 && !compareTimeDeltas(*delta1, *delta2)))
        return JSValue::encode(comparisonResult(compareBytes(self.data, other->data), op));
    if (delta1 && delta2) {
        int seconds1 = self.hour() * 3600 + self.minute() * 60 + self.second() - delta1->days * 86400 - delta1->seconds;
        int seconds2 = other->hour() * 3600 + other->minute() * 60 + other->second() - delta2->days * 86400 - delta2->seconds;
        int difference = seconds1 - seconds2;
        if (!difference)
            difference = self.microsecond() - other->microsecond();
        return JSValue::encode(comparisonResult(difference, op));
    }
    if (op == ComparisonOperator::Eq)
        return JSValue::encode(jsBoolean(false));
    if (op == ComparisonOperator::NotEq)
        return JSValue::encode(jsBoolean(true));
    return JSValue::encode(raiseTypeError(globalObject, scope, "can't compare offset-naive and offset-aware times"_s));
}

// time_hash()
PYTHON_NATIVE(timeOfDayHash)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeOfDayState>(args[0]);
    if (self.hash == -1) {
        // In CPython it is a copy with no fold that asks. But what the tzinfo is given is None, so that it never sees which.
        JSValue offset = callUTCOffset(globalObject, self.tzinfoOrNone(), jsUndefined());
        RETURN_IF_EXCEPTION(scope, { });
        // It comes down to the hash of something else.
        if (isNone(offset))
            self.hash = hashOfBytes(self.data);
        else {
            JSValue sinceMidnight = newTimeDelta(globalObject, 0, self.hour() * 3600 + self.minute() * 60 + self.second(), self.microsecond(), true);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue inUTC = subtractTimeDeltas(globalObject, *tryTimeDelta(sinceMidnight), *tryTimeDelta(offset));
            RETURN_IF_EXCEPTION(scope, { });
            int64_t computed = hashOfTimeDelta(globalObject, *tryTimeDelta(inUTC));
            RETURN_IF_EXCEPTION(scope, { });
            self.hash = computed;
        }
    }
    return JSValue::encode(intFromInt64(globalObject, self.hash));
}

// datetime_time_replace_impl()
PYTHON_NATIVE(timeOfDayReplace)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeOfDayState>(args[0]);
    int fields[4] = { self.hour(), self.minute(), self.second(), self.microsecond() };
    for (unsigned i = 0; i < 4; ++i) {
        if (JSValue given = args.at(i + 1)) {
            auto converted = toCInt(globalObject, given);
            RETURN_IF_EXCEPTION(scope, { });
            fields[i] = *converted;
        }
    }
    JSValue tzinfo = args.at(5) ? args.at(5) : self.tzinfoOrNone();
    int fold = self.fold;
    if (JSValue given = args.at(6)) {
        auto converted = toCInt(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        fold = *converted;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeOfDayOfClass(globalObject, fields[0], fields[1], fields[2], fields[3], tzinfo, fold, typeOf(globalObject, args[0])->object())));
}

// time_fromisoformat()
PYTHON_NATIVE(timeOfDayFromISOFormat)
{
    NATIVE_PROLOGUE();
    JSString* string = stringIn(args[1]);
    if (!string)
        return JSValue::encode(raiseTypeError(globalObject, scope, "fromisoformat: argument must be str"_s));
    String characters = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto text = utf8ForParsing(characters);
    if (!text)
        RELEASE_AND_RETURN(scope, JSValue::encode(raiseInvalidISOFormat(globalObject, scope, args[1])));

    // A time by itself is to begin with T, which can be left out so long as it could not be taken for a date.
    const char* p = text->data();
    size_t length = text->length();
    if (*p == 'T') {
        ++p;
        --length;
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    int microsecond = 0;
    int offset = 0;
    int offsetMicrosecond = 0;
    int result = parseISOTime(p, length, hour, minute, second, microsecond, offset, offsetMicrosecond);
    if (result < 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(raiseInvalidISOFormat(globalObject, scope, args[1])));
    if (hour == 24) {
        if (minute || second || microsecond)
            return JSValue::encode(raiseValueError(globalObject, scope, "minute, second, and microsecond must be 0 when hour is 24"_s));
        hour = 0;
    }
    JSValue tzinfo = tzinfoFromISOFormat(globalObject, result, offset, offsetMicrosecond);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeOfDayOfClass(globalObject, hour, minute, second, microsecond, tzinfo, 0, args[0])));
}

// time_reduce_ex() and time_reduce()
PYTHON_NATIVE(timeOfDayReduce)
{
    NATIVE_PROLOGUE();
    int protocol = 2;
    if (unpack<bool>(callFrame, 0)) {
        auto converted = toCIntOfFormat(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        protocol = *converted;
    }
    // time_getstate()
    auto& self = stateOf<TimeOfDayState>(args[0]);
    auto data = self.data;
    if (protocol > 3 && self.fold)
        data[0] |= 1 << 7;
    JSValue base = newBytes(globalObject, std::span<const uint8_t>(data));
    PyTuple* state = self.tzinfo ? PyTuple::create(globalObject, { base, self.tzinfo.get() }) : PyTuple::create(globalObject, { base });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), state }));
}

} // namespace

std::optional<TimeSpecification> timeSpecificationFrom(JSGlobalObject* globalObject, JSValue given, unsigned position, int microsecond)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto automatic = microsecond ? TimeSpecification::Microseconds : TimeSpecification::Seconds;
    if (!given)
        return automatic;
    // The "s" of PyArg_ParseTuple()
    auto text = toTextArgument(globalObject, given, "isoformat"_s, position == 1 ? "argument 1"_s : "argument 2"_s);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*text == "auto"_s)
        return automatic;
    static constexpr ASCIILiteral specifications[] = { "hours"_s, "minutes"_s, "seconds"_s, "milliseconds"_s, "microseconds"_s };
    for (unsigned i = 0; i < std::size(specifications); ++i) {
        if (*text == specifications[i])
            return static_cast<TimeSpecification>(i);
    }
    raiseValueError(globalObject, scope, "Unknown timespec value"_s);
    return std::nullopt;
}

void appendTimeAsISO(StringBuilder& builder, TimeSpecification specification, int hour, int minute, int second, int microsecond)
{
    builder.append(pad('0', 2, hour));
    if (specification >= TimeSpecification::Minutes)
        builder.append(':', pad('0', 2, minute));
    if (specification >= TimeSpecification::Seconds)
        builder.append(':', pad('0', 2, second));
    if (specification == TimeSpecification::Milliseconds)
        builder.append('.', pad('0', 3, microsecond / 1000));
    else if (specification == TimeSpecification::Microseconds)
        builder.append('.', pad('0', 6, microsecond));
}

void initializeTimeOfDayType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    PyType* type = createBuiltinType(globalObject, "datetime.time"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    dateTimeModuleState(globalObject).timeType.set(vm, realm, type);
    addMethods(globalObject, type, {
        { "__new__"_s, timeOfDayNew, Kind::New, 0, "?($type, /, hour=0, minute=0, second=0, microsecond=0, tzinfo=None, *, fold=0)"_s, Arguments::AreThoseOfTheClassButNotChecked },
        { "strptime"_s, timeOfDayStrptime, Kind::ClassMethod, 0, "($type, string, format, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "isoformat"_s, timeOfDayISOFormat, Kind::Method, 0, "($self, /, timespec='auto')"_s },
        { "strftime"_s, timeOfDayStrftime, Kind::Method, 0, "($self, /, format)"_s },
        { "__format__"_s, dateFormat, Kind::Method, 0, "($self, format, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "utcoffset"_s, timeOfDayUTCOffset },
        { "tzname"_s, timeOfDayTZName },
        { "dst"_s, timeOfDayDST },
        { "replace"_s, timeOfDayReplace },
        { "__replace__"_s, timeOfDayReplace, Kind::Method, 0, "replace($self, /, hour=unchanged, minute=unchanged, second=unchanged, microsecond=unchanged, tzinfo=unchanged, *, fold=unchanged)"_s },
        { "fromisoformat"_s, timeOfDayFromISOFormat, Kind::ClassMethod },
        { "__reduce_ex__"_s, timeOfDayReduce, Kind::Method, pack(true), "($self, protocol, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "__reduce__"_s, timeOfDayReduce, Kind::Method, pack(false) },
        { "__repr__"_s, timeOfDayRepr },
        { "__str__"_s, timeOfDayStr },
        { "__hash__"_s, timeOfDayHash },
    });
    addComparisons(globalObject, type, timeOfDayCompare);
    addGetSet(globalObject, type, "hour"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeOfDayState>(self).hour()); });
    addGetSet(globalObject, type, "minute"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeOfDayState>(self).minute()); });
    addGetSet(globalObject, type, "second"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeOfDayState>(self).second()); });
    addGetSet(globalObject, type, "microsecond"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeOfDayState>(self).microsecond()); });
    addGetSet(globalObject, type, "tzinfo"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return stateOf<TimeOfDayState>(self).tzinfoOrNone(); });
    addGetSet(globalObject, type, "fold"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<TimeOfDayState>(self).fold); });
}

} } // namespace JSC::Python
