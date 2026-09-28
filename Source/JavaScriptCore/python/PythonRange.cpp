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
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PythonNumbers.h"

// range, and what a slice selects of something as long as a range can be. It follows CPython's Objects/rangeobject.c and Objects/sliceobject.c.

namespace JSC {

PyRange::PyRange(VM& vm, Structure* structure, JSValue start, JSValue stop, JSValue step, JSValue length)
    : Base(vm, structure)
    , m_start(start, WriteBarrierEarlyInit)
    , m_stop(stop, WriteBarrierEarlyInit)
    , m_step(step, WriteBarrierEarlyInit)
    , m_length(length, WriteBarrierEarlyInit)
{
    auto smallStart = Python::tryInt64(start);
    auto smallStop = Python::tryInt64(stop);
    auto smallStep = Python::tryInt64(step);
    auto smallLength = Python::tryInt64(length);
    // The least int64 will not do for a step, since going the other way is by its negative, which does not fit.
    if (!smallStart || !smallStop || !smallStep || !smallLength || *smallStep == std::numeric_limits<int64_t>::min())
        return;
    // Every item is between the ends, so none of them overflows.
    m_isSmall = true;
    m_smallStart = *smallStart;
    m_smallStop = *smallStop;
    m_smallStep = *smallStep;
    m_smallLength = *smallLength;
}

namespace Python {

// ---- Arithmetic on ints of any size

static JSValue add(JSGlobalObject* globalObject, JSValue a, JSValue b) { return numberBinaryOperation(globalObject, BinaryOperator::Add, a, b); }
static JSValue subtract(JSGlobalObject* globalObject, JSValue a, JSValue b) { return numberBinaryOperation(globalObject, BinaryOperator::Sub, a, b); }
static JSValue multiply(JSGlobalObject* globalObject, JSValue a, JSValue b) { return numberBinaryOperation(globalObject, BinaryOperator::Mult, a, b); }
static JSValue floorDivide(JSGlobalObject* globalObject, JSValue a, JSValue b) { return numberBinaryOperation(globalObject, BinaryOperator::FloorDiv, a, b); }
static JSValue modulo(JSGlobalObject* globalObject, JSValue a, JSValue b) { return numberBinaryOperation(globalObject, BinaryOperator::Mod, a, b); }

// Less than, equal to or greater than zero.
int compareInts(JSValue a, JSValue b)
{
    bool isUnordered = false;
    return *numberCompare(classify(a), classify(b), isUnordered);
}

static bool isNegative(JSValue value) { return compareInts(value, jsNumber(0)) < 0; }

// start + index * step
static JSValue itemAt(JSGlobalObject* globalObject, PyRange* range, JSValue index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue offset = multiply(globalObject, index, range->step());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, add(globalObject, range->start(), offset));
}

} // namespace Python

PyRange* PyRange::create(JSGlobalObject* globalObject, JSValue start, JSValue stop, JSValue step)
{
    using namespace Python;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // How many steps there are from the lower end to short of the upper one.
    JSValue low = start;
    JSValue high = stop;
    JSValue stride = step;
    if (isNegative(step)) {
        low = stop;
        high = start;
        stride = subtract(globalObject, jsNumber(0), step);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    JSValue length = jsNumber(0);
    if (compareInts(low, high) < 0) {
        JSValue span = subtract(globalObject, high, low);
        RETURN_IF_EXCEPTION(scope, nullptr);
        span = subtract(globalObject, span, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, nullptr);
        length = floorDivide(globalObject, span, stride);
        RETURN_IF_EXCEPTION(scope, nullptr);
        length = add(globalObject, length, jsNumber(1));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    auto* range = new (NotNull, allocateCell<PyRange>(vm)) PyRange(vm, globalObject->pyRealm()->structureFor(BuiltinType::Range), start, stop, step, length);
    range->finishCreation(vm);
    return range;
}

// _PySlice_GetLongIndices()
bool PySlice::indices(JSGlobalObject* globalObject, JSValue length, JSValue& start, JSValue& stop, JSValue& step) const
{
    using namespace Python;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto evaluate = [&] (JSValue value) -> JSValue {
        if (!isInt(value) && !value.isBoolean() && !typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index))
            return raiseTypeError(globalObject, scope, "slice indices must be integers or None or have an __index__ method"_s);
        RELEASE_AND_RETURN(scope, toInt(globalObject, value));
    };

    step = jsNumber(1);
    if (!isNone(m_step.get())) {
        step = evaluate(m_step.get());
        RETURN_IF_EXCEPTION(scope, false);
        if (step.isInt32() && !step.asInt32()) {
            raiseValueError(globalObject, scope, "slice step cannot be zero"_s);
            return false;
        }
    }
    bool isBackwards = isNegative(step);

    // The least and the most that either end can be.
    JSValue lower = jsNumber(isBackwards ? -1 : 0);
    JSValue upper = length;
    if (isBackwards) {
        upper = add(globalObject, length, lower);
        RETURN_IF_EXCEPTION(scope, false);
    }
    auto clamp = [&] (JSValue given, JSValue whenAbsent) -> JSValue {
        if (isNone(given))
            return whenAbsent;
        JSValue value = evaluate(given);
        RETURN_IF_EXCEPTION(scope, { });
        if (isNegative(value)) {
            value = add(globalObject, value, length);
            RETURN_IF_EXCEPTION(scope, { });
            return compareInts(value, lower) < 0 ? lower : value;
        }
        return compareInts(value, upper) > 0 ? upper : value;
    };
    start = clamp(m_start.get(), isBackwards ? upper : lower);
    RETURN_IF_EXCEPTION(scope, false);
    stop = clamp(m_stop.get(), isBackwards ? lower : upper);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

namespace Python {

static PyRange* asRange(JSValue value) { return uncheckedDowncast<PyRange>(value.asCell()); }

// ---- What the operators come down to

int64_t rangeLength(JSGlobalObject* globalObject, PyRange* range)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto length = tryInt64(range->length())) [[likely]]
        return *length;
    raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C ssize_t"_s);
    return -1;
}

bool rangesAreEqual(PyRange* a, PyRange* b)
{
    // As the sequences that they are: the same items, however they were come by.
    if (a == b)
        return true;
    if (compareInts(a->length(), b->length()))
        return false;
    if (a->isEmpty())
        return true;
    if (compareInts(a->start(), b->start()))
        return false;
    if (a->length().isInt32() && a->length().asInt32() == 1)
        return true;
    return !compareInts(a->step(), b->step());
}

// For an int or a bool, which takes arithmetic and no more.
static bool rangeContainsInt(JSGlobalObject* globalObject, PyRange* range, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (range->isSmall()) {
        auto small = tryInt64(value);
        if (!small)
            return false;
        int64_t n = *small;
        bool isInside = range->smallStep() > 0 ? n >= range->smallStart() && n < range->smallStop() : n <= range->smallStart() && n > range->smallStop();
        // Between the ends, so how far it is from the start is less than how far apart they are, which may still be more than fits if signed.
        return isInside && !((range->smallStep() > 0 ? static_cast<uint64_t>(n) - static_cast<uint64_t>(range->smallStart()) : static_cast<uint64_t>(range->smallStart()) - static_cast<uint64_t>(n)) % (range->smallStep() > 0 ? static_cast<uint64_t>(range->smallStep()) : 0 - static_cast<uint64_t>(range->smallStep())));
    }
    bool isInside = isNegative(range->step())
        ? compareInts(value, range->start()) <= 0 && compareInts(range->stop(), value) < 0
        : compareInts(range->start(), value) <= 0 && compareInts(value, range->stop()) < 0;
    if (!isInside)
        return false;
    JSValue offset = subtract(globalObject, value, range->start());
    RETURN_IF_EXCEPTION(scope, false);
    JSValue remainder = modulo(globalObject, offset, range->step());
    RETURN_IF_EXCEPTION(scope, false);
    return remainder.isInt32() && !remainder.asInt32();
}

JSValue rangeGetItem(JSGlobalObject* globalObject, PyRange* range, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* slice = trySlice(key)) {
        JSValue start;
        JSValue stop;
        JSValue step;
        if (!slice->indices(globalObject, range->length(), start, stop, step))
            return { };
        JSValue newStep = multiply(globalObject, range->step(), step);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue newStart = itemAt(globalObject, range, start);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue newStop = itemAt(globalObject, range, stop);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, PyRange::create(globalObject, newStart, newStop, newStep));
    }
    if (!isInt(key) && !key.isBoolean() && !typeOf(globalObject, key)->lookup(vm, vm.pythonNames().dunder_index))
        return raiseTypeError(globalObject, scope, makeString("range indices must be integers or slices, not "_s, typeName(globalObject, key)));
    JSValue index = toInt(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });

    if (auto small = tryInt64(index); small && range->isSmall()) [[likely]] {
        int64_t i = *small < 0 ? *small + range->smallLength() : *small;
        if (i < 0 || i >= range->smallLength())
            return raise(globalObject, scope, BuiltinType::IndexError, "range object index out of range"_s);
        RELEASE_AND_RETURN(scope, intFromInt64(globalObject, static_cast<int64_t>(static_cast<uint64_t>(range->smallStart()) + static_cast<uint64_t>(i) * static_cast<uint64_t>(range->smallStep()))));
    }
    if (isNegative(index)) {
        index = add(globalObject, index, range->length());
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isNegative(index) || compareInts(index, range->length()) >= 0)
        return raise(globalObject, scope, BuiltinType::IndexError, "range object index out of range"_s);
    RELEASE_AND_RETURN(scope, itemAt(globalObject, range, index));
}

JSValue rangeIterator(JSGlobalObject* globalObject, PyRange* range)
{
    if (range->isSmall()) [[likely]]
        return PyIterator::create(globalObject, PyIterator::Kind::Range, JSValue(), JSValue(), range->smallStart(), range->smallLength(), range->smallStep());
    return PyIterator::create(globalObject, PyIterator::Kind::LongRange, range, jsNumber(0));
}

JSValue nextOfLongRange(JSGlobalObject* globalObject, PyRange* range, JSValue index)
{
    if (compareInts(index, range->length()) >= 0)
        return { };
    return itemAt(globalObject, range, index);
}

// ---- Methods

// range(stop), range(start, stop[, step])
PYTHON_NATIVE(rangeNew)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "range"_s))
        return { };
    if (args.size() < 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "range expected at least 1 argument, got 0"_s));
    if (args.size() > 4)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("range expected at most 3 arguments, got "_s, args.size() - 1)));
    JSValue values[3] = { jsNumber(0), jsNumber(0), jsNumber(1) };
    for (unsigned i = 1; i < args.size(); ++i) {
        values[args.size() == 2 ? 1 : i - 1] = toInt(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (values[2].isInt32() && !values[2].asInt32())
        return JSValue::encode(raiseValueError(globalObject, scope, "range() arg 3 must not be zero"_s));
    RELEASE_AND_RETURN(scope, JSValue::encode(PyRange::create(globalObject, values[0], values[1], values[2])));
}

PYTHON_NATIVE(rangeBool)
{
    return JSValue::encode(jsBoolean(!asRange(callFrame->argument(0))->isEmpty()));
}

PYTHON_NATIVE(rangeReversed)
{
    NATIVE_PROLOGUE();
    PyRange* range = asRange(args[0]);
    if (range->isSmall()) [[likely]] {
        uint64_t last = static_cast<uint64_t>(range->smallStart()) + (static_cast<uint64_t>(range->smallLength()) - 1) * static_cast<uint64_t>(range->smallStep());
        return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::Range, JSValue(), JSValue(), static_cast<int64_t>(last), range->smallLength(), -range->smallStep()));
    }
    // The range that has the same items the other way about: from the last, to one step before the first.
    JSValue lastIndex = subtract(globalObject, range->length(), jsNumber(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue last = itemAt(globalObject, range, lastIndex);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue beforeFirst = subtract(globalObject, range->start(), range->step());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue step = subtract(globalObject, jsNumber(0), range->step());
    RETURN_IF_EXCEPTION(scope, { });
    PyRange* reversed = PyRange::create(globalObject, last, beforeFirst, step);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::LongRange, reversed, jsNumber(0)));
}

// Goes through it comparing each item, as is done for a sequence about which nothing is known, until `visit` says to stop.
template<typename Visit>
static void search(JSGlobalObject* globalObject, PyRange* range, JSValue value, const Visit& visit)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    forEach(globalObject, range, [&] (JSValue item) {
        bool isSame = isIdentical(item, value) || isEqual(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, false);
        return visit(isSame);
    });
}

bool rangeContains(JSGlobalObject* globalObject, PyRange* range, JSValue value)
{
    if (isInt(value) || value.isBoolean())
        return rangeContainsInt(globalObject, range, value);
    bool isFound = false;
    search(globalObject, range, value, [&] (bool isSame) {
        isFound = isSame;
        return !isSame;
    });
    return isFound;
}

PYTHON_NATIVE(rangeIndex)
{
    NATIVE_PROLOGUE();
    PyRange* range = asRange(args[0]);
    JSValue value = args[1];
    if (isInt(value) || value.isBoolean()) {
        bool isFound = rangeContainsInt(globalObject, range, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (isFound) {
            JSValue offset = subtract(globalObject, value, range->start());
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, JSValue::encode(floorDivide(globalObject, offset, range->step())));
        }
        return JSValue::encode(raiseValueError(globalObject, scope, "range.index(x): x not in range"_s));
    }
    int64_t index = 0;
    bool isFound = false;
    search(globalObject, range, value, [&] (bool isSame) {
        isFound = isSame;
        index += !isSame;
        return !isSame;
    });
    RETURN_IF_EXCEPTION(scope, { });
    if (!isFound)
        return JSValue::encode(raiseValueError(globalObject, scope, "sequence.index(x): x not in sequence"_s));
    return JSValue::encode(intFromInt64(globalObject, index));
}

PYTHON_NATIVE(rangeCount)
{
    NATIVE_PROLOGUE();
    PyRange* range = asRange(args[0]);
    JSValue value = args[1];
    if (isInt(value) || value.isBoolean())
        RELEASE_AND_RETURN(scope, JSValue::encode(jsNumber(rangeContainsInt(globalObject, range, value) ? 1 : 0)));
    int64_t count = 0;
    search(globalObject, range, value, [&] (bool isSame) {
        count += isSame;
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, count));
}

// What to call, and with what, to make another like it: for pickle and copy.
PYTHON_NATIVE(rangeReduce)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    PyRange* range = asRange(args[0]);
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, range), PyTuple::create(globalObject, { range->start(), range->stop(), range->step() }) }));
}

// slice.indices(length)
PYTHON_SHARED_NATIVE(sliceIndices)
{
    NATIVE_PROLOGUE();
    JSValue length = toInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (isNegative(length))
        return JSValue::encode(raiseValueError(globalObject, scope, "length should not be negative"_s));
    JSValue start;
    JSValue stop;
    JSValue step;
    if (!uncheckedDowncast<PySlice>(args[0].asCell())->indices(globalObject, length, start, stop, step))
        return { };
    return JSValue::encode(PyTuple::create(globalObject, { start, stop, step }));
}

void initializeRangeType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyType* range = globalObject->pyRealm()->typeRange();
    range->setInstanceStructure(vm, PyRange::createStructure(vm, globalObject, range));
    addMethods(globalObject, range, {
        { "__new__"_s, rangeNew, PyNativeFunction::Kind::New },
        { "__repr__"_s, nativeRepr },
        { "__hash__"_s, nativeHash },
        { "__bool__"_s, rangeBool },
        { "__len__"_s, nativeLen },
        { "__getitem__"_s, nativeGetItem },
        { "__contains__"_s, nativeContains },
        { "__iter__"_s, nativeIter },
        { "__reversed__"_s, rangeReversed },
        { "__reduce__"_s, rangeReduce },
        { "index"_s, rangeIndex },
        { "count"_s, rangeCount },
    });
    addComparisons(globalObject, range);
    addMember(globalObject, range, "start"_s, [] (JSGlobalObject*, JSValue self) { return asRange(self)->start(); });
    addMember(globalObject, range, "stop"_s, [] (JSGlobalObject*, JSValue self) { return asRange(self)->stop(); });
    addMember(globalObject, range, "step"_s, [] (JSGlobalObject*, JSValue self) { return asRange(self)->step(); });
}

} } // namespace JSC::Python
