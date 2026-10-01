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
#include "PythonImport.h"
#include "PythonOperations.h"
#include "PythonSignatures.h"
#include "PythonText.h"
#include "PythonTime.h"

// datetime.date, and datetime.IsoCalendarDate

namespace JSC { namespace Python {

JSValue newDate(JSGlobalObject* globalObject, int year, int month, int day, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    checkDateArguments(globalObject, year, month, day);
    RETURN_IF_EXCEPTION(scope, { });
    if (!type)
        type = dateTimeModuleState(globalObject).dateType.get();
    auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<DateState>());
    object->state<DateState>().setDate(year, month, day);
    return object;
}

JSValue newDateOfClass(JSGlobalObject* globalObject, int year, int month, int day, JSValue cls)
{
    auto& state = dateTimeModuleState(globalObject);
    // The two that need not be called
    if (cls == state.dateType->object())
        return newDate(globalObject, year, month, day);
    if (cls == state.dateTimeType->object())
        return newDateTime(globalObject, year, month, day, 0, 0, 0, 0, jsUndefined(), 0);
    return call(globalObject, cls, jsNumber(year), jsNumber(month), jsNumber(day));
}

void checkIsStrArgument(JSGlobalObject* globalObject, ASCIILiteral function, unsigned position, JSValue value)
{
    if (stringIn(value))
        return;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    raiseTypeError(globalObject, scope, concatenate(function, "() argument "_s, position, " must be str, not "_s, typeNameOfArgument(globalObject, value)));
}

namespace {

bool isSaneMonth(unsigned month) { return month - 1 < 12; } // MONTH_IS_SANE()

// date_new()
PYTHON_NATIVE(dateNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    // What pickle calls it with is what __reduce__() gave.
    if (args.size() - 1 == 1) {
        auto state = pickledState(globalObject, args.at(1), DateState::dateSize, 2, isSaneMonth, "date"_s);
        RETURN_IF_EXCEPTION(scope, { });
        if (state) {
            // date_from_pickle()
            auto* object = PyStateObject::create(vm, type->instanceStructure(), makeUnique<DateState>());
            memcpySpan(std::span(object->state<DateState>().data), state->span());
            return JSValue::encode(object);
        }
    }

    // PyArg_ParseTupleAndKeywords(), of "iii", which sees first whether there are too many
    int fields[3];
    if (args.size() - 1 + args.keywordCount() > 3) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    parseIntArguments(globalObject, args, 1, fields, 3);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    RELEASE_AND_RETURN(scope, JSValue::encode(newDate(globalObject, fields[0], fields[1], fields[2], type)));
}

// date_fromtimestamp()
PYTHON_NATIVE(dateFromTimestamp)
{
    NATIVE_PROLOGUE();
    auto when = objectToTimeT(globalObject, args[1], TimeRounding::Floor);
    RETURN_IF_EXCEPTION(scope, { });
    struct tm tm;
    breakDownTime(globalObject, *when, BrokenDownAs::Local, tm);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateOfClass(globalObject, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, args[0])));
}

// date_today()
PYTHON_NATIVE(dateToday)
{
    NATIVE_PROLOGUE();
    // time_time()
    JSValue function = importModuleAttribute(globalObject, "time"_s, "time"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue now = call(globalObject, function);
    RETURN_IF_EXCEPTION(scope, { });
    // It is a method of the class, so this may not be date.fromtimestamp(). It may be datetime's, which wants all that time.time() has to give.
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, args[0], names.attribute_fromtimestamp, now)));
}

// date_fromordinal()
PYTHON_NATIVE(dateFromOrdinal)
{
    NATIVE_PROLOGUE();
    auto ordinal = toCIntOfFormat(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (*ordinal < 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "ordinal must be >= 1"_s));
    int year;
    int month;
    int day;
    ordinalToDate(*ordinal, year, month, day);
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateOfClass(globalObject, year, month, day, args[0])));
}

// date_fromisoformat()
PYTHON_NATIVE(dateFromISOFormat)
{
    NATIVE_PROLOGUE();
    JSString* string = stringIn(args[1]);
    if (!string)
        return JSValue::encode(raiseTypeError(globalObject, scope, "fromisoformat: argument must be str"_s));
    String characters = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto text = utf8ForParsing(characters);
    int year = 0;
    int month = 0;
    int day = 0;
    int result = -1;
    if (text && (text->length() == 7 || text->length() == 8 || text->length() == 10))
        result = parseISODate(text->data(), text->length(), year, month, day);
    if (result < 0)
        RELEASE_AND_RETURN(scope, JSValue::encode(raiseInvalidISOFormat(globalObject, scope, args[1])));
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateOfClass(globalObject, year, month, day, args[0])));
}

// date_fromisocalendar()
PYTHON_NATIVE(dateFromISOCalendar)
{
    NATIVE_PROLOGUE();
    int fields[3];
    if (args.size() - 1 + args.keywordCount() > 3) {
        checkArgumentsSlow(globalObject, callFrame);
        return { };
    }
    parseIntArguments(globalObject, args, 1, fields, 3);
    if (scope.exception()) [[unlikely]] {
        if (catchException(globalObject, BuiltinType::OverflowError))
            raiseValueError(globalObject, scope, "ISO calendar component out of range"_s);
        return { };
    }
    if (!checkArgumentsSlow(globalObject, callFrame))
        return { };
    auto [year, week, day] = fields;
    int month;
    switch (isoToDate(year, week, day, year, month, day)) {
    case -4:
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("year must be in "_s, minYear, ".."_s, maxYear, ", not "_s, year)));
    case -2:
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("Invalid week: "_s, week)));
    case -3:
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("Invalid weekday: "_s, day, " (range is [1, 7])"_s)));
    default:
        break;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateOfClass(globalObject, year, month, day, args[0])));
}

// date_strptime()
PYTHON_NATIVE(dateStrptime)
{
    NATIVE_PROLOGUE();
    checkIsStrArgument(globalObject, "strptime"_s, 1, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    checkIsStrArgument(globalObject, "strptime"_s, 2, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(callStrptime(globalObject, "_strptime_datetime_date"_s, args[0], args[1], args[2])));
}

// add_date_timedelta()
JSValue addTimeDeltaToDate(JSGlobalObject* globalObject, JSValue date, const TimeDeltaState& delta, bool negate)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto& self = stateOf<DateState>(date);
    int year = self.year();
    int month = self.month();
    // It cannot be too much for an int, there being fewer than 1e9 days in a timedelta.
    int day = self.day() + (negate ? -delta.days : delta.days);
    normalizeDate(globalObject, year, month, day);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newDateOfClass(globalObject, year, month, day, typeOf(globalObject, date)->object()));
}

// date_add()
PYTHON_NATIVE(dateAdd)
{
    NATIVE_PROLOGUE();
    auto [left, right] = operandsOfSlot(callFrame);
    if (tryDateTime(left) || tryDateTime(right))
        RETURN_NOT_IMPLEMENTED();
    if (tryDate(left)) {
        if (auto* delta = tryTimeDelta(right))
            RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDate(globalObject, left, *delta, false)));
    } else if (auto* delta = tryTimeDelta(left)) {
        // The one on the right is a date, or this would not have been called.
        RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDate(globalObject, right, *delta, false)));
    }
    RETURN_NOT_IMPLEMENTED();
}

// date_subtract()
PYTHON_NATIVE(dateSubtract)
{
    NATIVE_PROLOGUE();
    auto [left, right] = operandsOfSlot(callFrame);
    if (tryDateTime(left) || tryDateTime(right))
        RETURN_NOT_IMPLEMENTED();
    if (auto* self = tryDate(left)) {
        if (auto* other = tryDate(right)) {
            int difference = dateToOrdinal(self->year(), self->month(), self->day()) - dateToOrdinal(other->year(), other->month(), other->day());
            RELEASE_AND_RETURN(scope, JSValue::encode(newTimeDelta(globalObject, difference, 0, 0, false)));
        }
        if (auto* delta = tryTimeDelta(right))
            RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDate(globalObject, left, *delta, true)));
    }
    RETURN_NOT_IMPLEMENTED();
}

PYTHON_NATIVE(dateRepr)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(typeName(globalObject, args[0]), '(', self.year(), ", "_s, self.month(), ", "_s, self.day(), ')'))));
}

PYTHON_NATIVE(dateISOFormat)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    return JSValue::encode(jsString(vm, makeString(pad('0', 4, self.year()), '-', pad('0', 2, self.month()), '-', pad('0', 2, self.day()))));
}

// date_str()
PYTHON_NATIVE(dateStr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, args[0], names.attribute_isoformat)));
}

PYTHON_NATIVE(dateCTime)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(jsString(vm, formatCTime(stateOf<DateState>(args[0]), 0, 0, 0)));
}

// date_strftime()
PYTHON_NATIVE(dateStrftime)
{
    NATIVE_PROLOGUE();
    checkIsStrArgument(globalObject, "strftime"_s, 1, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    // What is derived from it has this too, and it is the timetuple() of that class that is wanted.
    JSValue tuple = callMethodNamed(globalObject, args[0], names.attribute_timetuple);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(wrapStrftime(globalObject, args[0], stringIn(args[1]), tuple, args[0])));
}

PYTHON_NATIVE(dateISOWeekday)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    return JSValue::encode(jsNumber(weekdayOf(self.year(), self.month(), self.day()) + 1));
}

// ---- IsoCalendarDate

// iso_calendar_date_new_impl()
JSValue newISOCalendarDate(JSGlobalObject* globalObject, PyType* type, int year, int week, int weekday)
{
    VM& vm = globalObject->vm();
    PyTuple* tuple = PyTuple::create(vm, type->instanceStructure(), 3);
    tuple->initializeAt(vm, 0, jsNumber(year));
    tuple->initializeAt(vm, 1, jsNumber(week));
    tuple->initializeAt(vm, 2, jsNumber(weekday));
    return tuple;
}

PYTHON_NATIVE(isoCalendarDateNew)
{
    NATIVE_PROLOGUE();
    int fields[3];
    for (unsigned i = 0; i < 3; ++i) {
        auto converted = toCInt(globalObject, args[i + 1]);
        RETURN_IF_EXCEPTION(scope, { });
        fields[i] = *converted;
    }
    return JSValue::encode(newISOCalendarDate(globalObject, asType(args[0]), fields[0], fields[1], fields[2]));
}

PYTHON_NATIVE(isoCalendarDateRepr)
{
    NATIVE_PROLOGUE();
    PyTuple* self = asTuple(args[0]);
    String parts[3];
    for (unsigned i = 0; i < 3; ++i) {
        parts[i] = str(globalObject, self->at(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(typeName(globalObject, self), "(year="_s, parts[0], ", week="_s, parts[1], ", weekday="_s, parts[2], ')'))));
}

// iso_calendar_date_reduce(): it is a tuple that comes back.
PYTHON_NATIVE(isoCalendarDateReduce)
{
    NATIVE_PROLOGUE();
    PyTuple* self = asTuple(args[0]);
    PyTuple* items = PyTuple::create(globalObject, { self->at(0), self->at(1), self->at(2) });
    return JSValue::encode(PyTuple::create(globalObject, { realm->typeTuple()->object(), PyTuple::create(globalObject, { items }) }));
}

// date_isocalendar()
PYTHON_NATIVE(dateISOCalendar)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    int year = self.year();
    int week1Monday = isoWeek1Monday(year);
    int today = dateToOrdinal(year, self.month(), self.day());
    int day;
    int week = floorDivide(today - week1Monday, 7, day);
    if (week < 0) {
        --year;
        week1Monday = isoWeek1Monday(year);
        week = floorDivide(today - week1Monday, 7, day);
    } else if (week >= 52 && today >= isoWeek1Monday(year + 1)) {
        ++year;
        week = 0;
    }
    return JSValue::encode(newISOCalendarDate(globalObject, dateTimeModuleState(globalObject).isoCalendarDateType.get(), year, week + 1, day + 1));
}

// ---- The rest of date

// date_richcompare()
PYTHON_NATIVE(dateCompare)
{
    NATIVE_PROLOGUE();
    // A datetime is a date, and to go by its date alone would not do. So the two are no more compared than if neither were derived from the other, unless a class derived from them sees to it.
    auto* other = tryDate(args[1]);
    if (!other || other->isDateTime)
        RETURN_NOT_IMPLEMENTED();
    auto& self = stateOf<DateState>(args[0]);
    return JSValue::encode(comparisonResult(compareBytes(std::span(self.data).first(DateState::dateSize), std::span(other->data).first(DateState::dateSize)), unpack<ComparisonOperator>(callFrame, 0)));
}

PYTHON_NATIVE(dateTimeTuple)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    RELEASE_AND_RETURN(scope, JSValue::encode(buildStructTime(globalObject, self.year(), self.month(), self.day(), 0, 0, 0, -1)));
}

// datetime_date_replace_impl()
PYTHON_NATIVE(dateReplace)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    int fields[3] = { self.year(), self.month(), self.day() };
    for (unsigned i = 0; i < 3; ++i) {
        if (JSValue given = args.at(i + 1)) {
            auto converted = toCInt(globalObject, given);
            RETURN_IF_EXCEPTION(scope, { });
            fields[i] = *converted;
        }
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(newDateOfClass(globalObject, fields[0], fields[1], fields[2], typeOf(globalObject, args[0])->object())));
}

PYTHON_NATIVE(dateHash)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    if (self.hash == -1)
        self.hash = hashOfBytes(std::span(self.data).first(DateState::dateSize));
    return JSValue::encode(intFromInt64(globalObject, self.hash));
}

PYTHON_NATIVE(dateToOrdinalMethod)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    return JSValue::encode(jsNumber(dateToOrdinal(self.year(), self.month(), self.day())));
}

PYTHON_NATIVE(dateWeekday)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    return JSValue::encode(jsNumber(weekdayOf(self.year(), self.month(), self.day())));
}

// date_reduce()
PYTHON_NATIVE(dateReduce)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<DateState>(args[0]);
    // date_getstate()
    PyTuple* state = PyTuple::create(globalObject, { newBytes(globalObject, std::span<const uint8_t>(self.data).first(DateState::dateSize)) });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), state }));
}

} // namespace

// date_format()
PYTHON_SHARED_NATIVE(dateFormat)
{
    NATIVE_PROLOGUE();
    checkIsStrArgument(globalObject, "__format__"_s, 1, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    // With nothing to go by, it is str(self).
    if (!stringIn(args[1])->length()) {
        String text = str(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, text)));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(callMethodNamed(globalObject, args[0], names.attribute_strftime, args[1])));
}

void initializeDateTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = dateTimeModuleState(globalObject);

    PyType* type = createBuiltinType(globalObject, "datetime.date"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    state.dateType.set(vm, realm, type);
    addMethods(globalObject, type, {
        { "__new__"_s, dateNew, Kind::New, 0, "?($type, /, year, month, day)"_s, Arguments::AreThoseOfTheClassButNotChecked },
        { "fromtimestamp"_s, dateFromTimestamp, Kind::ClassMethod },
        { "fromordinal"_s, dateFromOrdinal, Kind::ClassMethod, 0, "($type, ordinal, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "fromisoformat"_s, dateFromISOFormat, Kind::ClassMethod },
        { "fromisocalendar"_s, dateFromISOCalendar, Kind::ClassMethod, 0, "($type, /, year, week, day)"_s, Arguments::AreNotChecked },
        { "strptime"_s, dateStrptime, Kind::ClassMethod, 0, "($type, string, format, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "today"_s, dateToday, Kind::ClassMethod },
        { "ctime"_s, dateCTime },
        { "strftime"_s, dateStrftime, Kind::Method, 0, "($self, /, format)"_s },
        { "__format__"_s, dateFormat, Kind::Method, 0, "($self, format, /)"_s, Arguments::AreCheckedAsByParseTuple },
        { "timetuple"_s, dateTimeTuple },
        { "isocalendar"_s, dateISOCalendar },
        { "isoformat"_s, dateISOFormat },
        { "isoweekday"_s, dateISOWeekday },
        { "toordinal"_s, dateToOrdinalMethod },
        { "weekday"_s, dateWeekday },
        { "replace"_s, dateReplace },
        { "__replace__"_s, dateReplace, Kind::Method, 0, "replace($self, /, year=unchanged, month=unchanged, day=unchanged)"_s },
        { "__reduce__"_s, dateReduce },
        { "__repr__"_s, dateRepr },
        { "__str__"_s, dateStr },
        { "__hash__"_s, dateHash },
        { "__add__"_s, dateAdd, Kind::Wrapper, pack(false) },
        { "__radd__"_s, dateAdd, Kind::Wrapper, pack(true) },
        { "__sub__"_s, dateSubtract, Kind::Wrapper, pack(false) },
        { "__rsub__"_s, dateSubtract, Kind::Wrapper, pack(true) },
    });
    addComparisons(globalObject, type, dateCompare);
    addGetSet(globalObject, type, "year"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).year()); });
    addGetSet(globalObject, type, "month"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).month()); });
    addGetSet(globalObject, type, "day"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<DateState>(self).day()); });

    PyType* isoCalendarDate = createBuiltinType(globalObject, "datetime.IsoCalendarDate"_s, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence | PyType::IsDerivedFromBuiltin);
    state.isoCalendarDateType.set(vm, realm, isoCalendarDate);
    addMethods(globalObject, isoCalendarDate, {
        { "__new__"_s, isoCalendarDateNew, Kind::New, 0, "IsoCalendarDate($type, /, year, week, weekday)"_s, Arguments::AreThoseOfTheClass },
        { "__repr__"_s, isoCalendarDateRepr },
        { "__reduce__"_s, isoCalendarDateReduce },
    });
    addGetSet(globalObject, isoCalendarDate, "year"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asTuple(self)->at(0); });
    addGetSet(globalObject, isoCalendarDate, "week"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asTuple(self)->at(1); });
    addGetSet(globalObject, isoCalendarDate, "weekday"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asTuple(self)->at(2); });
}

} } // namespace JSC::Python
