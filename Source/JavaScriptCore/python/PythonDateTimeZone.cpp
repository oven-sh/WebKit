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
#include "PythonOperations.h"
#include "PythonText.h"

// datetime.tzinfo and datetime.timezone

namespace JSC { namespace Python {

namespace {

// ---- tzinfo

// PyType_GenericNew(): whatever it is given is for the __init__() of a class derived from it.
PYTHON_NATIVE(tzinfoNew)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<TZInfoState>()));
}

// tzinfo_nogo(), which is tzinfo_tzname(), tzinfo_utcoffset() and tzinfo_dst()
PYTHON_NATIVE(tzinfoNotImplemented)
{
    NATIVE_PROLOGUE();
    static constexpr ASCIILiteral methods[] = { "tzname"_s, "utcoffset"_s, "dst"_s };
    return JSValue::encode(raise(globalObject, scope, BuiltinType::NotImplementedError, concatenate("a tzinfo subclass must implement "_s, methods[unpack<unsigned>(callFrame, 0)], "()"_s)));
}

// tzinfo_fromutc()
PYTHON_NATIVE(tzinfoFromUTC)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue given = args[1];
    auto* dateTime = tryDateTime(given);
    if (!dateTime)
        return JSValue::encode(raiseTypeError(globalObject, scope, "fromutc: argument must be a datetime"_s));
    if (dateTime->tzinfoOrNone() != self)
        return JSValue::encode(raiseValueError(globalObject, scope, "fromutc: dt.tzinfo is not self"_s));

    JSValue offset = callUTCOffset(globalObject, self, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(offset))
        return JSValue::encode(raiseValueError(globalObject, scope, "fromutc: non-None utcoffset() result required"_s));
    JSValue dst = callDST(globalObject, self, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(dst))
        return JSValue::encode(raiseValueError(globalObject, scope, "fromutc: non-None dst() result required"_s));

    JSValue delta = subtractTimeDeltas(globalObject, *tryTimeDelta(offset), *tryTimeDelta(dst));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = addTimeDeltaToDateTime(globalObject, given, *tryTimeDelta(delta), 1);
    RETURN_IF_EXCEPTION(scope, { });

    dst = callDST(globalObject, self, result);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNone(dst))
        return JSValue::encode(raiseValueError(globalObject, scope, "fromutc: tz.dst() gave inconsistent results; cannot convert"_s));
    if (*tryTimeDelta(dst))
        RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDateTime(globalObject, result, *tryTimeDelta(dst), 1)));
    return JSValue::encode(result);
}

// tzinfo_reduce()
PYTHON_NATIVE(tzinfoReduce)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue getInitArgs = getAttributeIfPresent(globalObject, self, Identifier::fromString(vm, "__getinitargs__"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue arguments;
    if (getInitArgs) {
        arguments = call(globalObject, getInitArgs);
        RETURN_IF_EXCEPTION(scope, { });
    } else
        arguments = PyTuple::create(globalObject, 0);
    JSValue state = getObjectState(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, self)->object(), arguments, state }));
}

// ---- timezone

// timezone_new()
PYTHON_NATIVE(timeZoneNew)
{
    NATIVE_PROLOGUE();
    JSValue offset = args[1];
    if (!tryTimeDelta(offset))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("timezone() argument 1 must be datetime.timedelta, not "_s, typeNameOfArgument(globalObject, offset))));
    JSValue name = args.at(2);
    if (name && !stringIn(name))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("timezone() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, name))));
    RELEASE_AND_RETURN(scope, JSValue::encode(newTimeZone(globalObject, uncheckedDowncast<PyStateObject>(offset.asCell()), name)));
}

// timezone_richcompare()
PYTHON_NATIVE(timeZoneCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    auto* other = tryStateOf<TimeZoneState>(args[1]);
    if (!isEquality(op) || !other)
        RETURN_NOT_IMPLEMENTED();
    return JSValue::encode(comparisonResult(compareTimeDeltas(stateOf<TimeZoneState>(args[0]).offset->state<TimeDeltaState>(), other->offset->state<TimeDeltaState>()), op));
}

PYTHON_NATIVE(timeZoneHash)
{
    NATIVE_PROLOGUE();
    int64_t hash = hashOfTimeDelta(globalObject, stateOf<TimeZoneState>(args[0]).offset->state<TimeDeltaState>());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, hash));
}

// _timezone_check_argument(). It may raise.
void checkTimeZoneArgument(JSGlobalObject* globalObject, JSValue dateTime, ASCIILiteral method)
{
    if (isNone(dateTime) || tryDateTime(dateTime))
        return;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    raiseTypeError(globalObject, scope, concatenate(method, "(dt) argument must be a datetime instance or None, not "_s, typeName(globalObject, dateTime)));
}

PYTHON_NATIVE(timeZoneRepr)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeZoneState>(args[0]);
    String type = typeName(globalObject, args[0]);
    if (args[0] == JSValue(dateTimeModuleState(globalObject).utc.get()))
        return JSValue::encode(jsString(vm, makeString(type, ".utc"_s)));
    String offset = repr(globalObject, self.offset.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!self.name)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(type, '(', offset, ')'))));
    String name = repr(globalObject, self.name.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(type, '(', offset, ", "_s, name, ')'))));
}

// timezone_str()
JSValue nameOfTimeZone(JSGlobalObject* globalObject, JSValue timeZone)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& self = stateOf<TimeZoneState>(timeZone);
    if (self.name)
        return self.name.get();
    auto* offset = &self.offset->state<TimeDeltaState>();
    if (!*offset)
        return jsNontrivialString(vm, "UTC"_s);
    // It is normalized, so it is less than nothing if its days are.
    char sign = '+';
    if (offset->days < 0) {
        sign = '-';
        JSValue negated = negateTimeDelta(globalObject, *offset);
        RETURN_IF_EXCEPTION(scope, { });
        offset = tryTimeDelta(negated);
    }
    int microseconds = offset->microseconds;
    int seconds;
    int minutes = floorDivide(offset->seconds, 60, seconds);
    int hours = floorDivide(minutes, 60, minutes);
    if (microseconds)
        return jsString(vm, makeString("UTC"_s, sign, pad('0', 2, hours), ':', pad('0', 2, minutes), ':', pad('0', 2, seconds), '.', pad('0', 6, microseconds)));
    if (seconds)
        return jsString(vm, makeString("UTC"_s, sign, pad('0', 2, hours), ':', pad('0', 2, minutes), ':', pad('0', 2, seconds)));
    return jsString(vm, makeString("UTC"_s, sign, pad('0', 2, hours), ':', pad('0', 2, minutes)));
}

PYTHON_NATIVE(timeZoneStr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(nameOfTimeZone(globalObject, args[0])));
}

PYTHON_NATIVE(timeZoneTZName)
{
    NATIVE_PROLOGUE();
    checkTimeZoneArgument(globalObject, args[1], "tzname"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(nameOfTimeZone(globalObject, args[0])));
}

PYTHON_NATIVE(timeZoneUTCOffset)
{
    NATIVE_PROLOGUE();
    checkTimeZoneArgument(globalObject, args[1], "utcoffset"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(stateOf<TimeZoneState>(args[0]).offset.get());
}

PYTHON_NATIVE(timeZoneDST)
{
    NATIVE_PROLOGUE();
    checkTimeZoneArgument(globalObject, args[1], "dst"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// timezone_fromutc()
PYTHON_NATIVE(timeZoneFromUTC)
{
    NATIVE_PROLOGUE();
    auto* dateTime = tryDateTime(args[1]);
    if (!dateTime)
        return JSValue::encode(raiseTypeError(globalObject, scope, "fromutc: argument must be a datetime"_s));
    if (dateTime->tzinfo.get() != args[0])
        return JSValue::encode(raiseValueError(globalObject, scope, "fromutc: dt.tzinfo is not self"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(addTimeDeltaToDateTime(globalObject, args[1], stateOf<TimeZoneState>(args[0]).offset->state<TimeDeltaState>(), 1)));
}

PYTHON_NATIVE(timeZoneGetInitArgs)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<TimeZoneState>(args[0]);
    if (!self.name)
        return JSValue::encode(PyTuple::create(globalObject, { self.offset.get() }));
    return JSValue::encode(PyTuple::create(globalObject, { self.offset.get(), self.name.get() }));
}

} // namespace

void initializeTimeZoneTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = dateTimeModuleState(globalObject);

    PyType* tzinfo = createBuiltinType(globalObject, "datetime.tzinfo"_s, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
    tzinfo->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, tzinfo));
    state.tzinfoType.set(vm, realm, tzinfo);
    addMethods(globalObject, tzinfo, {
        { "__new__"_s, tzinfoNew, Kind::New, 0, { }, Arguments::AreNotChecked },
        { "tzname"_s, tzinfoNotImplemented, Kind::Method, pack(0u) },
        { "utcoffset"_s, tzinfoNotImplemented, Kind::Method, pack(1u) },
        { "dst"_s, tzinfoNotImplemented, Kind::Method, pack(2u) },
        { "fromutc"_s, tzinfoFromUTC },
        { "__reduce__"_s, tzinfoReduce },
    });

    PyType* timeZone = createBuiltinType(globalObject, "datetime.timezone"_s, tzinfo, PyType::Layout::Native, 0);
    timeZone->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, timeZone));
    state.timeZoneType.set(vm, realm, timeZone);
    addMethods(globalObject, timeZone, {
        { "__new__"_s, timeZoneNew, Kind::New, 0, "timezone($type, /, offset, name=<unrepresentable>)"_s, Arguments::AreThoseOfTheClass },
        { "__repr__"_s, timeZoneRepr },
        { "__str__"_s, timeZoneStr },
        { "__hash__"_s, timeZoneHash },
        { "tzname"_s, timeZoneTZName },
        { "utcoffset"_s, timeZoneUTCOffset },
        { "dst"_s, timeZoneDST },
        { "fromutc"_s, timeZoneFromUTC },
        { "__getinitargs__"_s, timeZoneGetInitArgs },
    });
    addComparisons(globalObject, timeZone, timeZoneCompare);
}

} } // namespace JSC::Python
