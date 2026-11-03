/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyRange.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"

namespace JSC {
namespace Python {

// range and slice. (Objects/rangeobject.c and Objects/sliceobject.c in CPython are the reference.)

PY_DEFINE_CELL(PyRange);
PY_DEFINE_CELL(PySlice);

PyRange::PyRange(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyRange* PyRange::create(JSGlobalObject* globalObject, JSValue start, JSValue stop, JSValue step, JSValue length)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* range = new (NotNull, allocateCell<PyRange>(vm)) PyRange(vm, rt.structure<PyRange>(), rt.rangeType());
    range->finishCreation(vm);
    range->m_start.set(vm, range, start);
    range->m_stop.set(vm, range, stop);
    range->m_step.set(vm, range, step);
    range->m_length.set(vm, range, length);
    return range;
}

template<typename Visitor>
void PyRange::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyRange>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_start);
    visitor.append(thisObject->m_stop);
    visitor.append(thisObject->m_step);
    visitor.append(thisObject->m_length);
}

DEFINE_VISIT_CHILDREN(PyRange);

PySlice::PySlice(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PySlice* PySlice::create(JSGlobalObject* globalObject, JSValue start, JSValue stop, JSValue step)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* slice = new (NotNull, allocateCell<PySlice>(vm)) PySlice(vm, rt.structure<PySlice>(), rt.sliceType());
    slice->finishCreation(vm);
    slice->m_start.set(vm, slice, start);
    slice->m_stop.set(vm, slice, stop);
    slice->m_step.set(vm, slice, step);
    return slice;
}

template<typename Visitor>
void PySlice::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PySlice>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_start);
    visitor.append(thisObject->m_stop);
    visitor.append(thisObject->m_step);
}

DEFINE_VISIT_CHILDREN(PySlice);

// ---- range ----

// How many values there are from start up to stop by step.
static JSValue rangeLength(JSGlobalObject* globalObject, JSValue start, JSValue stop, JSValue step)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue low = start;
    JSValue high = stop;
    if (intSign(step) < 0) {
        low = stop;
        high = start;
        step = intNegate(globalObject, step);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (compareInts(low, high) >= 0)
        return jsNumber(0);
    // (high - low - 1) // step + 1
    JSValue difference = intSubtract(globalObject, high, low);
    RETURN_IF_EXCEPTION(scope, { });
    difference = intSubtract(globalObject, difference, jsNumber(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue quotient = intFloorDivide(globalObject, difference, step);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intAdd(globalObject, quotient, jsNumber(1)));
}

static JSValue makeRange(JSGlobalObject* globalObject, JSValue start, JSValue stop, JSValue step)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue length = rangeLength(globalObject, start, stop, step);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyRange::create(globalObject, start, stop, step, length));
}

static JSValue rangeNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "range"_s, args)) || failed(scope, checkPositional(globalObject, "range"_s, args, 1, 3)))
        return { };
    JSValue start = jsNumber(0);
    JSValue step = jsNumber(1);
    JSValue stop;
    if (args.size() == 1) {
        stop = toIndex(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        start = toIndex(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        stop = toIndex(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (args.size() == 3) {
            step = toIndex(globalObject, args[2]);
            RETURN_IF_EXCEPTION(scope, { });
            if (!intSign(step))
                return raiseValueError(globalObject, scope, "range() arg 3 must not be zero"_s);
        }
    }
    RELEASE_AND_RETURN(scope, makeRange(globalObject, start, stop, step));
}

static JSValue rangeRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* range = PyRange::from(self);
    String start = intToString(globalObject, range->start());
    RETURN_IF_EXCEPTION(scope, { });
    String stop = intToString(globalObject, range->stop());
    RETURN_IF_EXCEPTION(scope, { });
    if (!compareInts(range->step(), jsNumber(1)))
        return makeStr(vm, makeString("range("_s, start, ", "_s, stop, ')'));
    String step = intToString(globalObject, range->step());
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("range("_s, start, ", "_s, stop, ", "_s, step, ')'));
}

static JSValue rangeLen(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue length = PyRange::from(self)->length();
    if (!tryInt64(length))
        return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
    return length;
}

static JSValue rangeBool(JSGlobalObject*, JSValue self) { return jsBoolean(!!intSign(PyRange::from(self)->length())); }

// start + index * step
static JSValue rangeItem(JSGlobalObject* globalObject, PyRange* range, JSValue index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue offset = intMultiply(globalObject, index, range->step());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, intAdd(globalObject, range->start(), offset));
}

// As PySlice::indices(), for a length of any size.
static bool longSliceIndices(JSGlobalObject* globalObject, PySlice* slice, JSValue length, JSValue& start, JSValue& stop, JSValue& step)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto convert = [&](JSValue value) -> JSValue {
        if (!hasIndex(globalObject, value))
            return raiseTypeError(globalObject, scope, "slice indices must be integers or None or have an __index__ method"_s);
        return toIndex(globalObject, value);
    };

    bool isNegativeStep = false;
    if (isNone(slice->step()))
        step = jsNumber(1);
    else {
        step = convert(slice->step());
        RETURN_IF_EXCEPTION(scope, false);
        if (!intSign(step)) {
            raiseValueError(globalObject, scope, "slice step cannot be zero"_s);
            return false;
        }
        isNegativeStep = intSign(step) < 0;
    }
    // The bounds for the start and the stop.
    JSValue lower = jsNumber(isNegativeStep ? -1 : 0);
    JSValue upper = length;
    if (isNegativeStep) {
        upper = intAdd(globalObject, length, lower);
        RETURN_IF_EXCEPTION(scope, false);
    }
    auto clamp = [&](JSValue given, JSValue defaultValue) -> JSValue {
        if (isNone(given))
            return defaultValue;
        JSValue value = convert(given);
        RETURN_IF_EXCEPTION(scope, { });
        if (intSign(value) < 0) {
            value = intAdd(globalObject, value, length);
            RETURN_IF_EXCEPTION(scope, { });
            return compareInts(value, lower) < 0 ? lower : value;
        }
        return compareInts(value, upper) > 0 ? upper : value;
    };
    start = clamp(slice->start(), isNegativeStep ? upper : lower);
    RETURN_IF_EXCEPTION(scope, false);
    stop = clamp(slice->stop(), isNegativeStep ? lower : upper);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static JSValue rangeGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* range = PyRange::from(self);
    if (hasIndex(globalObject, key)) {
        JSValue index = toIndex(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (intSign(index) < 0) {
            index = intAdd(globalObject, index, range->length());
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (intSign(index) < 0 || compareInts(index, range->length()) >= 0)
            return raiseIndexError(globalObject, scope, "range object index out of range"_s);
        RELEASE_AND_RETURN(scope, rangeItem(globalObject, range, index));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "range indices must be integers or slices, not "_s, typeName(globalObject, key));
    JSValue start, stop, step;
    if (!longSliceIndices(globalObject, slice, range->length(), start, stop, step))
        return { };
    JSValue newStep = intMultiply(globalObject, range->step(), step);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue newStart = rangeItem(globalObject, range, start);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue newStop = rangeItem(globalObject, range, stop);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeRange(globalObject, newStart, newStop, newStep));
}

static JSValue makeRangeIteratorFrom(JSGlobalObject* globalObject, JSValue start, JSValue step, JSValue length)
{
    VM& vm = getVM(globalObject);
    auto first = tryInt64(start);
    auto stride = tryInt64(step);
    auto count = tryInt64(length);
    if (first && stride && count) {
        // Whether the last value, and one step beyond it, still fit.
        Int128 last = static_cast<Int128>(*first) + static_cast<Int128>(*stride) * *count;
        if (last >= std::numeric_limits<int64_t>::min() && last <= std::numeric_limits<int64_t>::max()) {
            PyIterator* iterator = PyIterator::create(globalObject, PyIterator::Kind::Range, jsUndefined());
            iterator->position = *first;
            iterator->step = *stride;
            iterator->remaining = *count;
            return iterator;
        }
    }
    PyIterator* iterator = PyIterator::create(globalObject, PyIterator::Kind::LongRange, start);
    iterator->setExtra(vm, PyTuple::create(globalObject, { step, length }));
    return iterator;
}

JSValue makeRangeIterator(JSGlobalObject* globalObject, JSValue value)
{
    auto* range = PyRange::from(value);
    return makeRangeIteratorFrom(globalObject, range->start(), range->step(), range->length());
}

static JSValue rangeIter(JSGlobalObject* globalObject, JSValue self) { return makeRangeIterator(globalObject, self); }

static JSValue rangeReversed(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* range = PyRange::from(self);
    JSValue lastIndex = intSubtract(globalObject, range->length(), jsNumber(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue last = rangeItem(globalObject, range, lastIndex);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue step = intNegate(globalObject, range->step());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeRangeIteratorFrom(globalObject, last, step, range->length()));
}

// For an int, by arithmetic.
static bool rangeContainsInt(JSGlobalObject* globalObject, PyRange* range, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool inBounds = intSign(range->step()) > 0
        ? compareInts(range->start(), value) <= 0 && compareInts(value, range->stop()) < 0
        : compareInts(range->stop(), value) < 0 && compareInts(value, range->start()) <= 0;
    if (!inBounds)
        return false;
    JSValue offset = intSubtract(globalObject, value, range->start());
    RETURN_IF_EXCEPTION(scope, false);
    JSValue remainder = intModulo(globalObject, offset, range->step());
    RETURN_IF_EXCEPTION(scope, false);
    return !intSign(remainder);
}

static ALWAYS_INLINE bool isPlainInt(JSValue value) { return isExactInt(value) || value.isBoolean(); }

// The index of the first item that equals the value, going through them all. -1: none.
static int64_t findByIteration(JSGlobalObject* globalObject, JSValue sequence, JSValue value, uint64_t* count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t index = 0;
    int64_t found = -1;
    forEach(globalObject, sequence, [&](JSValue item) {
        bool equal = isEqual(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (equal) {
            if (!count) {
                found = index;
                return false;
            }
            ++*count;
        }
        ++index;
        return true;
    });
    return found;
}

static JSValue rangeContains(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found;
    if (isPlainInt(value))
        found = rangeContainsInt(globalObject, PyRange::from(self), intValue(value));
    else
        found = findByIteration(globalObject, self, value, nullptr) >= 0;
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

static JSValue rangeCount(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isPlainInt(value)) {
        bool found = rangeContainsInt(globalObject, PyRange::from(self), intValue(value));
        RETURN_IF_EXCEPTION(scope, { });
        return jsNumber(found ? 1 : 0);
    }
    uint64_t count = 0;
    findByIteration(globalObject, self, value, &count);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, count));
}

static JSValue rangeIndex(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* range = PyRange::from(self);
    if (isPlainInt(value)) {
        bool found = rangeContainsInt(globalObject, range, intValue(value));
        RETURN_IF_EXCEPTION(scope, { });
        if (!found)
            return raiseValueError(globalObject, scope, "range.index(x): x not in range"_s);
        JSValue offset = intSubtract(globalObject, intValue(value), range->start());
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, intFloorDivide(globalObject, offset, range->step()));
    }
    int64_t index = findByIteration(globalObject, self, value, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return raiseValueError(globalObject, scope, "sequence.index(x): x not in sequence"_s);
    RELEASE_AND_RETURN(scope, makeInt(globalObject, index));
}

static bool rangesAreEqual(PyRange* a, PyRange* b)
{
    if (a == b)
        return true;
    if (compareInts(a->length(), b->length()))
        return false;
    if (!intSign(a->length()))
        return true;
    if (compareInts(a->start(), b->start()))
        return false;
    if (!compareInts(a->length(), jsNumber(1)))
        return true;
    return !compareInts(a->step(), b->step());
}

template<bool wantEqual>
static JSValue rangeEquality(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!PyRange::is(other))
        return runtime(globalObject).notImplemented();
    return jsBoolean(rangesAreEqual(PyRange::from(self), PyRange::from(other)) == wantEqual);
}

static JSValue rangeHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* range = PyRange::from(self);
    // Of what decides equality.
    JSValue start = jsUndefined();
    JSValue step = jsUndefined();
    if (intSign(range->length())) {
        start = range->start();
        if (compareInts(range->length(), jsNumber(1)))
            step = range->step();
    }
    int64_t result = hashOfTuple(globalObject, PyTuple::create(globalObject, { range->length(), start, step }));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
}

static JSValue rangeReduce(JSGlobalObject* globalObject, JSValue self)
{
    auto* range = PyRange::from(self);
    return PyTuple::create(globalObject, { range->type(), PyTuple::create(globalObject, { range->start(), range->stop(), range->step() }) });
}

static JSValue rangeGetStart(JSGlobalObject*, JSValue self) { return PyRange::from(self)->start(); }
static JSValue rangeGetStop(JSGlobalObject*, JSValue self) { return PyRange::from(self)->stop(); }
static JSValue rangeGetStep(JSGlobalObject*, JSValue self) { return PyRange::from(self)->step(); }

static constexpr NativeDef rangeMethods[] = {
    PY_NEW(rangeNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", rangeRepr, "($self, /)"),
    PY_SLOT("__hash__", rangeHash, "($self, /)"),
    PY_SLOT("__bool__", rangeBool, "($self, /)"),
    PY_SLOT("__eq__", rangeEquality<true>, "($self, value, /)"),
    PY_SLOT("__ne__", rangeEquality<false>, "($self, value, /)"),
    PY_SLOT("__len__", rangeLen, "($self, /)"),
    PY_SLOT("__getitem__", rangeGetItem, "($self, key, /)"),
    PY_SLOT("__contains__", rangeContains, "($self, key, /)"),
    PY_SLOT("__iter__", rangeIter, "($self, /)"),
    PY_METHOD("__reversed__", rangeReversed, "($self, /)"),
    PY_METHOD("__reduce__", rangeReduce, "($self, /)"),
    PY_METHOD("count", rangeCount, "($self, object, /)"),
    PY_METHOD("index", rangeIndex, "($self, object, /)"),
};

static constexpr NativeGetSet rangeGetSets[] = {
    PY_MEMBER("start", rangeGetStart),
    PY_MEMBER("stop", rangeGetStop),
    PY_MEMBER("step", rangeGetStep),
};

// ---- slice ----

bool PySlice::unpack(JSGlobalObject* globalObject, Indices& result) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr int64_t max = std::numeric_limits<int64_t>::max();
    static constexpr int64_t min = std::numeric_limits<int64_t>::min();
    auto convert = [&](JSValue value, int64_t& converted) {
        if (value.isInt32()) [[likely]] {
            converted = value.asInt32();
            return true;
        }
        if (!hasIndex(globalObject, value)) {
            raiseTypeError(globalObject, scope, "slice indices must be integers or have an __index__ method"_s);
            return false;
        }
        converted = toSsize(globalObject, value, nullptr);
        return !scope.exception();
    };

    int64_t step = 1;
    if (!isNone(m_step.get())) {
        if (!convert(m_step.get(), step))
            return false;
        if (!step) {
            raiseValueError(globalObject, scope, "slice step cannot be zero"_s);
            return false;
        }
        // So that it can be negated.
        step = std::max(step, -max);
    }
    int64_t start = step < 0 ? max : 0;
    if (!isNone(m_start.get()) && !convert(m_start.get(), start))
        return false;
    int64_t stop = step < 0 ? min : max;
    if (!isNone(m_stop.get()) && !convert(m_stop.get(), stop))
        return false;
    result.start = start;
    result.stop = stop;
    result.step = step;
    result.length = 0;
    return true;
}

void PySlice::adjust(int64_t length, Indices& result)
{
    int64_t step = result.step;
    auto adjustIndex = [&](int64_t& index) {
        if (index < 0) {
            index += length;
            if (index < 0)
                index = step < 0 ? -1 : 0;
        } else if (index >= length)
            index = step < 0 ? length - 1 : length;
    };
    adjustIndex(result.start);
    adjustIndex(result.stop);
    if (step < 0)
        result.length = result.stop < result.start ? (result.start - result.stop - 1) / (-step) + 1 : 0;
    else
        result.length = result.start < result.stop ? (result.stop - result.start - 1) / step + 1 : 0;
}

static JSValue sliceNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "slice"_s, args)) || failed(scope, checkPositional(globalObject, "slice"_s, args, 1, 3)))
        return { };
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, PySlice::create(globalObject, jsUndefined(), args[0], jsUndefined()));
    RELEASE_AND_RETURN(scope, PySlice::create(globalObject, args[0], args[1], args.size() == 3 ? args[2] : jsUndefined()));
}

static JSValue sliceRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* slice = PySlice::from(self);
    StrBuilder builder;
    builder.append("slice("_s);
    JSValue parts[] = { slice->start(), slice->stop(), slice->step() };
    for (unsigned i = 0; i < 3; ++i) {
        if (i)
            builder.append(", "_s);
        JSString* text = repr(globalObject, parts[i]);
        RETURN_IF_EXCEPTION(scope, { });
        auto view = text->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(view.data);
    }
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static PyTuple* sliceAsTuple(JSGlobalObject* globalObject, JSValue value)
{
    auto* slice = PySlice::from(value);
    return PyTuple::create(globalObject, { slice->start(), slice->stop(), slice->step() });
}

template<CompareOperator op>
static JSValue sliceCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!PySlice::is(other))
        return runtime(globalObject).notImplemented();
    if (self == other)
        return jsBoolean(op == CompareOperator::Eq || op == CompareOperator::LtE || op == CompareOperator::GtE);
    return richCompare(globalObject, sliceAsTuple(globalObject, self), sliceAsTuple(globalObject, other), op);
}

static JSValue sliceHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // As hashOfTuple(), but that the length is not mixed in at the end.
    auto* slice = PySlice::from(self);
    uint64_t accumulator = 2870177450012600261ULL;
    for (JSValue part : { slice->start(), slice->stop(), slice->step() }) {
        int64_t lane = hash(globalObject, part);
        RETURN_IF_EXCEPTION(scope, { });
        accumulator += static_cast<uint64_t>(lane) * 14029467366897019727ULL;
        accumulator = (accumulator << 31) | (accumulator >> 33);
        accumulator *= 11400714785074694791ULL;
    }
    if (accumulator == static_cast<uint64_t>(-1))
        accumulator = 1546275796;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<int64_t>(accumulator)));
}

static JSValue sliceIndices(JSGlobalObject* globalObject, JSValue self, JSValue lengthValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue length = toIndex(globalObject, lengthValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (intSign(length) < 0)
        return raiseValueError(globalObject, scope, "length should not be negative"_s);
    JSValue start, stop, step;
    if (!longSliceIndices(globalObject, PySlice::from(self), length, start, stop, step))
        return { };
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { start, stop, step }));
}

static JSValue sliceReduce(JSGlobalObject* globalObject, JSValue self)
{
    return PyTuple::create(globalObject, { PySlice::from(self)->type(), sliceAsTuple(globalObject, self) });
}

static JSValue sliceGetStart(JSGlobalObject*, JSValue self) { return PySlice::from(self)->start(); }
static JSValue sliceGetStop(JSGlobalObject*, JSValue self) { return PySlice::from(self)->stop(); }
static JSValue sliceGetStep(JSGlobalObject*, JSValue self) { return PySlice::from(self)->step(); }

static constexpr NativeDef sliceMethods[] = {
    PY_NEW(sliceNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", sliceRepr, "($self, /)"),
    PY_SLOT("__hash__", sliceHash, "($self, /)"),
    PY_SLOT("__eq__", sliceCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", sliceCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", sliceCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", sliceCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", sliceCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", sliceCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_METHOD("indices", sliceIndices, "($self, object, /)"),
    PY_METHOD("__reduce__", sliceReduce, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet sliceGetSets[] = {
    PY_MEMBER("start", sliceGetStart),
    PY_MEMBER("stop", sliceGetStop),
    PY_MEMBER("step", sliceGetStep),
};

void initializeRangeTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.rangeType()->initializeBuiltin(globalObject, { "range"_s, { TypeFlag::Sequence }, rangeMethods, rangeGetSets, "range(stop) -> range object\nrange(start, stop[, step]) -> range object\n\nReturn an object that produces a sequence of integers from start (inclusive)\nto stop (exclusive) by step.  range(i, j) produces i, i+1, i+2, ..., j-1.\nstart defaults to 0, and stop is omitted!  range(4) produces 0, 1, 2, 3.\nThese are exactly the valid indices for a list of 4 elements.\nWhen step is given, it specifies the increment (or decrement)."_s }, rt.typeType(), rt.objectType());
    rt.sliceType()->initializeBuiltin(globalObject, { "slice"_s, { }, sliceMethods, sliceGetSets, "slice(stop)\nslice(start, stop[, step])\n\nCreate a slice object.  This is used for extended slicing (e.g. a[0:10:2])."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC
