/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module itertools. (Modules/itertoolsmodule.c in CPython is the reference.)
//
// Every iterator is a PyRecord. What its fields hold is said by the enum in the namespace named after the type.

// ---- What the types have in common ----

static ALWAYS_INLINE PyRecord* itertoolsRecord(JSValue self) { return uncheckedDowncast<PyRecord>(self.asCell()); }

static PyRecord* itertoolsAllocate(JSGlobalObject* globalObject, JSValue cls, unsigned fieldCount)
{
    Runtime& rt = runtime(globalObject);
    return PyRecord::create(rt.vm(), rt.structure<PyRecord>(), PyType::from(cls), fieldCount);
}

static JSValue itertoolsSelf(JSGlobalObject*, JSValue self) { return self; }

static NEVER_INLINE JSValue itertoolsStop(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseStopIteration(globalObject, scope);
}

// An integer that may not fit a number takes two fields.
static ALWAYS_INLINE int64_t itertoolsGetInt64(PyRecord* record, unsigned field)
{
    return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(record->get(field).asInt32())) << 32 | static_cast<uint32_t>(record->get(field + 1).asInt32()));
}

static ALWAYS_INLINE void itertoolsSetInt64(VM& vm, PyRecord* record, unsigned field, int64_t value)
{
    record->set(vm, field, jsNumber(static_cast<int32_t>(static_cast<uint64_t>(value) >> 32)));
    record->set(vm, field + 1, jsNumber(static_cast<int32_t>(static_cast<uint64_t>(value))));
}

// An index into a tuple.
static ALWAYS_INLINE unsigned itertoolsGetIndex(PyRecord* record, unsigned field) { return static_cast<unsigned>(record->get(field).asNumber()); }
static ALWAYS_INLINE void itertoolsSetIndex(VM& vm, PyRecord* record, unsigned field, unsigned value) { record->set(vm, field, jsNumber(value)); }

// A constructor that takes no keywords lets a subclass with an __init__ of its own have them.
static bool itertoolsRejectKeywords(JSGlobalObject* globalObject, JSValue cls, PyType* (*baseType)(JSGlobalObject*), ASCIILiteral name, const CallArgs& args)
{
    if (!args.hasKeywords())
        return true;
    PyType* type = PyType::from(cls);
    PyType* base = baseType(globalObject);
    if (type != base && type->lookup(Special::Init) != base->lookup(Special::Init))
        return true;
    return rejectKeywords(globalObject, name, args);
}

// An argument that is a Py_ssize_t to CPython.
static bool itertoolsSsizeArgument(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        return false;
    }
    result = *converted;
    return true;
}

static bool itertoolsNonNegativeArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral name, int64_t& result)
{
    if (!itertoolsSsizeArgument(globalObject, value, result))
        return false;
    if (result >= 0)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, name, " cannot be negative"_s);
    return false;
}

// No tuple, and no record, has more items than this.
static constexpr int64_t itertoolsMaxLength = 1 << 28;

static bool itertoolsCheckLength(JSGlobalObject* globalObject, int64_t length)
{
    if (length <= itertoolsMaxLength)
        return true;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseWithArgs(globalObject, scope, rt.MemoryError(), { });
    return false;
}

#define ITERTOOLS_DECLARE_TYPE(id) static PyType* itertools##id##Type(JSGlobalObject*)
#define ITERTOOLS_DEFINE_TYPE(id, name, ...) \
    static constexpr TypeDef itertools##id##TypeDef { "itertools." name ""_s, { __VA_ARGS__ }, itertools##id##Methods, { }, { } }; \
    static PyType* itertools##id##Type(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(itertools##id##TypeDef); }

// ---- batched ----

namespace ItertoolsBatched {
enum : unsigned { Iterator, IsStrict, FieldCount }; // number: the size of a batch, or -1 once it is over.
}

static JSValue itertoolsBatchedNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "iterable"_s, "n"_s, "strict"_s };
    JSValue values[3];
    if (failed(scope, unpackKeywordArguments(globalObject, "batched"_s, keywords, 2, 2, false, args, values)))
        return { };
    int64_t size;
    if (failed(scope, itertoolsSsizeArgument(globalObject, values[1], size)))
        return { };
    bool isStrict = values[2] && isTrue(globalObject, values[2]);
    RETURN_IF_EXCEPTION(scope, { });
    if (size < 1)
        return raiseValueError(globalObject, scope, "n must be at least one"_s);
    JSValue iterator = getIter(globalObject, values[0]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsBatched::FieldCount);
    record->set(vm, ItertoolsBatched::Iterator, iterator);
    record->set(vm, ItertoolsBatched::IsStrict, jsBoolean(isStrict));
    record->number = size;
    return record;
}

static JSValue itertoolsBatchedNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    int64_t size = record->number;
    if (size < 0)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    JSValue iterator = record->get(ItertoolsBatched::Iterator);
    auto finish = [&] {
        record->number = -1;
        record->set(vm, ItertoolsBatched::Iterator, { });
    };

    MarkedArgumentBuffer batch;
    while (static_cast<int64_t>(batch.size()) < size) {
        JSValue item = iterNext(globalObject, iterator);
        if (scope.exception()) [[unlikely]] {
            finish();
            return { };
        }
        if (!item)
            break;
        batch.append(item);
    }
    if (static_cast<int64_t>(batch.size()) == size)
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, spanOf(batch)));
    if (batch.isEmpty()) {
        finish();
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    }
    if (record->get(ItertoolsBatched::IsStrict).asBoolean()) {
        finish();
        return raiseValueError(globalObject, scope, "batched(): incomplete batch"_s);
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, spanOf(batch)));
}

static constexpr NativeDef itertoolsBatchedMethods[] = {
    PY_NEW(itertoolsBatchedNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsBatchedNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Batched, "batched", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- pairwise ----

namespace ItertoolsPairwise {
enum : unsigned { Iterator, Old, FieldCount };
}

ITERTOOLS_DECLARE_TYPE(Pairwise);

static JSValue itertoolsPairwiseNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, itertoolsPairwiseType, "pairwise"_s, args)) || failed(scope, checkPositional(globalObject, "pairwise"_s, args, 1, 1)))
        return { };
    JSValue iterator = getIter(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsPairwise::FieldCount);
    record->set(vm, ItertoolsPairwise::Iterator, iterator);
    return record;
}

static JSValue itertoolsPairwiseNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSValue iterator = record->get(ItertoolsPairwise::Iterator);
    if (!iterator)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    JSValue old = record->get(ItertoolsPairwise::Old);
    if (!old) {
        old = iterNext(globalObject, iterator);
        record->set(vm, ItertoolsPairwise::Old, old);
        if (!old) {
            record->set(vm, ItertoolsPairwise::Iterator, { });
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
        }
        // The iterator may have come back here and found the end.
        iterator = record->get(ItertoolsPairwise::Iterator);
        if (!iterator) {
            record->set(vm, ItertoolsPairwise::Old, { });
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
        }
    }
    JSValue next = iterNext(globalObject, iterator);
    if (!next) {
        record->set(vm, ItertoolsPairwise::Iterator, { });
        record->set(vm, ItertoolsPairwise::Old, { });
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    }
    record->set(vm, ItertoolsPairwise::Old, next);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { old, next }));
}

static constexpr NativeDef itertoolsPairwiseMethods[] = {
    PY_NEW(itertoolsPairwiseNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsPairwiseNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Pairwise, "pairwise", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- groupby ----

namespace ItertoolsGroupBy {
enum : unsigned { Iterator, KeyFunction, TargetKey, CurrentKey, CurrentValue, CurrentGrouper, GrouperType, FieldCount };
}
namespace ItertoolsGrouper {
enum : unsigned { Parent, TargetKey, FieldCount };
}

ITERTOOLS_DECLARE_TYPE(GroupBy);
ITERTOOLS_DECLARE_TYPE(Grouper);

static JSValue itertoolsGroupByNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "iterable"_s, "key"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "groupby"_s, keywords, 1, 2, false, args, values)))
        return { };
    JSValue iterator = getIter(globalObject, values[0]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsGroupBy::FieldCount);
    record->set(vm, ItertoolsGroupBy::Iterator, iterator);
    record->set(vm, ItertoolsGroupBy::KeyFunction, values[1] ? values[1] : jsUndefined());
    record->set(vm, ItertoolsGroupBy::GrouperType, itertoolsGrouperType(globalObject));
    return record;
}

// Takes the next value and its key. False: there is none, or an exception.
static bool itertoolsGroupByStep(JSGlobalObject* globalObject, PyRecord* groupBy)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = iterNext(globalObject, groupBy->get(ItertoolsGroupBy::Iterator));
    RETURN_IF_EXCEPTION(scope, false);
    if (!value)
        return false;
    JSValue key = value;
    JSValue keyFunction = groupBy->get(ItertoolsGroupBy::KeyFunction);
    if (!isNone(keyFunction)) {
        key = call(globalObject, keyFunction, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    groupBy->set(vm, ItertoolsGroupBy::CurrentValue, value);
    groupBy->set(vm, ItertoolsGroupBy::CurrentKey, key);
    return true;
}

static JSValue itertoolsMakeGrouper(JSGlobalObject* globalObject, PyRecord* groupBy, JSValue targetKey)
{
    VM& vm = getVM(globalObject);
    PyRecord* grouper = itertoolsAllocate(globalObject, groupBy->get(ItertoolsGroupBy::GrouperType), ItertoolsGrouper::FieldCount);
    grouper->set(vm, ItertoolsGrouper::Parent, groupBy);
    grouper->set(vm, ItertoolsGrouper::TargetKey, targetKey);
    groupBy->set(vm, ItertoolsGroupBy::CurrentGrouper, grouper);
    return grouper;
}

static JSValue itertoolsGroupByNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    record->set(vm, ItertoolsGroupBy::CurrentGrouper, { });
    // On to the next group.
    while (true) {
        JSValue currentKey = record->get(ItertoolsGroupBy::CurrentKey);
        JSValue targetKey = record->get(ItertoolsGroupBy::TargetKey);
        if (currentKey) {
            if (!targetKey)
                break;
            bool isSame = isEqual(globalObject, targetKey, currentKey);
            RETURN_IF_EXCEPTION(scope, { });
            if (!isSame)
                break;
        }
        bool stepped = itertoolsGroupByStep(globalObject, record);
        RETURN_IF_EXCEPTION(scope, { });
        if (!stepped)
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    }
    JSValue key = record->get(ItertoolsGroupBy::CurrentKey);
    record->set(vm, ItertoolsGroupBy::TargetKey, key);
    JSValue grouper = itertoolsMakeGrouper(globalObject, record, key);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { key, grouper }));
}

static constexpr NativeDef itertoolsGroupByMethods[] = {
    PY_NEW(itertoolsGroupByNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsGroupByNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(GroupBy, "groupby", TypeFlag::Base, TypeFlag::OwnLayout)

static JSValue itertoolsGrouperNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, itertoolsGrouperType, "_grouper"_s, args)) || failed(scope, checkPositional(globalObject, "_grouper"_s, args, 2, 2)))
        return { };
    PyRecord* parent = PyRecord::tryFrom(args[0], itertoolsGroupByType(globalObject));
    if (!parent)
        return raiseTypeError(globalObject, scope, "_grouper() argument 1 must be itertools.groupby, not "_s, isNone(args[0]) ? "None"_str : typeName(globalObject, args[0]));
    RELEASE_AND_RETURN(scope, itertoolsMakeGrouper(globalObject, parent, args[1]));
}

static JSValue itertoolsGrouperNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    PyRecord* groupBy = itertoolsRecord(record->get(ItertoolsGrouper::Parent));
    // Once the groupby has moved on, the group is over.
    if (groupBy->get(ItertoolsGroupBy::CurrentGrouper) != JSValue(record))
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    if (!groupBy->get(ItertoolsGroupBy::CurrentValue)) {
        bool stepped = itertoolsGroupByStep(globalObject, groupBy);
        RETURN_IF_EXCEPTION(scope, { });
        if (!stepped)
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    }
    bool isSame = isEqual(globalObject, record->get(ItertoolsGrouper::TargetKey), groupBy->get(ItertoolsGroupBy::CurrentKey));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isSame)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    JSValue value = groupBy->get(ItertoolsGroupBy::CurrentValue);
    groupBy->set(vm, ItertoolsGroupBy::CurrentValue, { });
    groupBy->set(vm, ItertoolsGroupBy::CurrentKey, { });
    // The comparison may have come back here and taken it.
    if (!value)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    return value;
}

static constexpr NativeDef itertoolsGrouperMethods[] = {
    PY_NEW(itertoolsGrouperNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsGrouperNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Grouper, "_grouper", TypeFlag::OwnLayout)

// ---- tee ----
//
// The iterators tee() returns share a chain of blocks of the values read so far. A block no iterator has yet to go through
// is garbage.

namespace ItertoolsTeeData {
static constexpr unsigned linkCells = 57;
enum : unsigned { Iterator, NextLink, IsRunning, FirstValue, FieldCount = FirstValue + linkCells }; // number: how many values have been read.
}
namespace ItertoolsTee {
enum : unsigned { Data, FieldCount }; // number: the index in the block of the next value.
}

ITERTOOLS_DECLARE_TYPE(TeeData);
ITERTOOLS_DECLARE_TYPE(Tee);

static PyRecord* itertoolsMakeTeeData(JSGlobalObject* globalObject, JSValue iterator)
{
    VM& vm = getVM(globalObject);
    PyRecord* data = itertoolsAllocate(globalObject, itertoolsTeeDataType(globalObject), ItertoolsTeeData::FieldCount);
    data->set(vm, ItertoolsTeeData::Iterator, iterator);
    data->set(vm, ItertoolsTeeData::IsRunning, jsBoolean(false));
    return data;
}

static JSValue itertoolsTeeDataNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, itertoolsTeeDataType, "teedataobject"_s, args)) || failed(scope, checkPositional(globalObject, "teedataobject"_s, args, 3, 3)))
        return { };
    if (!isList(args[1]))
        return raiseTypeError(globalObject, scope, "teedataobject() argument 2 must be list, not "_s, isNone(args[1]) ? "None"_str : typeName(globalObject, args[1]));
    JSArray* values = asList(args[1]);
    JSValue next = args[2];
    unsigned length = listLength(values);
    bool isValid = length <= ItertoolsTeeData::linkCells;
    if (isValid && !isNone(next))
        isValid = length == ItertoolsTeeData::linkCells && PyRecord::is(next) && itertoolsRecord(next)->type() == itertoolsTeeDataType(globalObject);
    if (!isValid)
        return raiseValueError(globalObject, scope, "Invalid arguments"_s);
    PyRecord* data = itertoolsMakeTeeData(globalObject, args[0]);
    for (unsigned i = 0; i < length; ++i)
        data->set(vm, ItertoolsTeeData::FirstValue + i, listGet(values, i));
    data->number = length;
    if (!isNone(next))
        data->set(vm, ItertoolsTeeData::NextLink, next);
    return data;
}

static constexpr NativeDef itertoolsTeeDataMethods[] = {
    PY_NEW(itertoolsTeeDataNew, "($type, *args, **kwargs)"),
};
ITERTOOLS_DEFINE_TYPE(TeeData, "_tee_dataobject", TypeFlag::OwnLayout)

static PyRecord* itertoolsMakeTee(JSGlobalObject* globalObject, PyType* type, JSValue data, int64_t index)
{
    PyRecord* tee = itertoolsAllocate(globalObject, type, ItertoolsTee::FieldCount);
    tee->set(getVM(globalObject), ItertoolsTee::Data, data);
    tee->number = index;
    return tee;
}

static JSValue itertoolsTeeCopy(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = itertoolsRecord(self);
    return itertoolsMakeTee(globalObject, record->type(), record->get(ItertoolsTee::Data), record->number);
}

static JSValue itertoolsTeeFromIterable(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    if (PyRecord::tryFrom(iterator, itertoolsTeeType(globalObject)))
        RELEASE_AND_RETURN(scope, itertoolsTeeCopy(globalObject, iterator));
    RELEASE_AND_RETURN(scope, itertoolsMakeTee(globalObject, itertoolsTeeType(globalObject), itertoolsMakeTeeData(globalObject, iterator), 0));
}

static JSValue itertoolsTeeNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, itertoolsTeeType, "_tee"_s, args)) || failed(scope, checkPositional(globalObject, "_tee"_s, args, 1, 1)))
        return { };
    RELEASE_AND_RETURN(scope, itertoolsTeeFromIterable(globalObject, args[0]));
}

static JSValue itertoolsTeeNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    PyRecord* data = itertoolsRecord(record->get(ItertoolsTee::Data));
    if (record->number >= ItertoolsTeeData::linkCells) {
        JSValue link = data->get(ItertoolsTeeData::NextLink);
        if (!link) {
            link = itertoolsMakeTeeData(globalObject, data->get(ItertoolsTeeData::Iterator));
            data->set(vm, ItertoolsTeeData::NextLink, link);
        }
        record->set(vm, ItertoolsTee::Data, link);
        record->number = 0;
        data = itertoolsRecord(link);
    }

    unsigned index = static_cast<unsigned>(record->number);
    JSValue value;
    if (index < data->number)
        value = data->get(ItertoolsTeeData::FirstValue + index);
    else {
        // This one leads the others.
        ASSERT(index == data->number);
        if (data->get(ItertoolsTeeData::IsRunning).asBoolean())
            return raiseRuntimeError(globalObject, scope, "cannot re-enter the tee iterator"_s);
        data->set(vm, ItertoolsTeeData::IsRunning, jsBoolean(true));
        value = iterNext(globalObject, data->get(ItertoolsTeeData::Iterator));
        data->set(vm, ItertoolsTeeData::IsRunning, jsBoolean(false));
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
        data->set(vm, ItertoolsTeeData::FirstValue + index, value);
        ++data->number;
    }
    ++record->number;
    return value;
}

static constexpr NativeDef itertoolsTeeMethods[] = {
    PY_NEW(itertoolsTeeNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsTeeNext, "($self, /)"),
    PY_METHOD("__copy__", itertoolsTeeCopy, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Tee, "_tee", TypeFlag::OwnLayout, TypeFlag::HasWeakref)

static JSValue itertoolsTeeFunction(JSGlobalObject* globalObject, JSValue iterable, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = 2;
    if (countValue && failed(scope, itertoolsNonNegativeArgument(globalObject, countValue, "n"_s, count)))
        return { };
    if (failed(scope, itertoolsCheckLength(globalObject, count)))
        return { };
    if (!count)
        RELEASE_AND_RETURN(scope, runtime(globalObject).emptyTuple());
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue tee = itertoolsTeeFromIterable(globalObject, iterator);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* result = PyTuple::create(globalObject, static_cast<unsigned>(count));
    result->initializeAt(vm, 0, tee);
    for (unsigned i = 1; i < count; ++i) {
        tee = itertoolsTeeCopy(globalObject, tee);
        result->initializeAt(vm, i, tee);
    }
    return result;
}

// ---- cycle ----

namespace ItertoolsCycle {
enum : unsigned { Iterator, Saved, FieldCount }; // number: the index in what was saved, or -1 the first time around.
}

ITERTOOLS_DECLARE_TYPE(Cycle);

static JSValue itertoolsCycleNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, itertoolsCycleType, "cycle"_s, args)) || failed(scope, checkPositional(globalObject, "cycle"_s, args, 1, 1)))
        return { };
    JSValue iterator = getIter(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* saved = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsCycle::FieldCount);
    record->set(vm, ItertoolsCycle::Iterator, iterator);
    record->set(vm, ItertoolsCycle::Saved, saved);
    record->number = -1;
    return record;
}

static JSValue itertoolsCycleNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSArray* saved = asList(record->get(ItertoolsCycle::Saved));
    if (record->number < 0) {
        JSValue item = iterNext(globalObject, record->get(ItertoolsCycle::Iterator));
        RETURN_IF_EXCEPTION(scope, { });
        if (item) {
            listAppend(globalObject, saved, item);
            RETURN_IF_EXCEPTION(scope, { });
            return item;
        }
        record->number = 0;
        record->set(vm, ItertoolsCycle::Iterator, { });
    }
    unsigned length = listLength(saved);
    if (!length)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    JSValue item = listGet(saved, static_cast<unsigned>(record->number));
    if (++record->number >= length)
        record->number = 0;
    return item;
}

static constexpr NativeDef itertoolsCycleMethods[] = {
    PY_NEW(itertoolsCycleNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsCycleNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Cycle, "cycle", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- What is made from a function and an iterable: dropwhile, takewhile, starmap, filterfalse ----

namespace ItertoolsFunctional {
enum : unsigned { Function, Iterator, FieldCount }; // number: for dropwhile, whether it has started; for takewhile, whether it has stopped.
}

static JSValue itertoolsFunctionalNew(JSGlobalObject* globalObject, JSValue cls, PyType* (*baseType)(JSGlobalObject*), ASCIILiteral name, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, baseType, name, args)) || failed(scope, checkPositional(globalObject, name, args, 2, 2)))
        return { };
    JSValue iterator = getIter(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsFunctional::FieldCount);
    record->set(vm, ItertoolsFunctional::Function, args[0]);
    record->set(vm, ItertoolsFunctional::Iterator, iterator);
    return record;
}

ITERTOOLS_DECLARE_TYPE(DropWhile);
ITERTOOLS_DECLARE_TYPE(TakeWhile);
ITERTOOLS_DECLARE_TYPE(StarMap);
ITERTOOLS_DECLARE_TYPE(FilterFalse);

static JSValue itertoolsDropWhileNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return itertoolsFunctionalNew(globalObject, cls, itertoolsDropWhileType, "dropwhile"_s, args); }
static JSValue itertoolsTakeWhileNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return itertoolsFunctionalNew(globalObject, cls, itertoolsTakeWhileType, "takewhile"_s, args); }
static JSValue itertoolsStarMapNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return itertoolsFunctionalNew(globalObject, cls, itertoolsStarMapType, "starmap"_s, args); }
static JSValue itertoolsFilterFalseNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return itertoolsFunctionalNew(globalObject, cls, itertoolsFilterFalseType, "filterfalse"_s, args); }

static JSValue itertoolsDropWhileNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSValue iterator = record->get(ItertoolsFunctional::Iterator);
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
        if (record->number)
            return item;
        JSValue good = call(globalObject, record->get(ItertoolsFunctional::Function), item);
        RETURN_IF_EXCEPTION(scope, { });
        bool isGood = isTrue(globalObject, good);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isGood) {
            record->number = 1;
            return item;
        }
    }
}

static JSValue itertoolsTakeWhileNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    if (record->number)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    JSValue item = iterNext(globalObject, record->get(ItertoolsFunctional::Iterator));
    RETURN_IF_EXCEPTION(scope, { });
    if (!item)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    JSValue good = call(globalObject, record->get(ItertoolsFunctional::Function), item);
    RETURN_IF_EXCEPTION(scope, { });
    bool isGood = isTrue(globalObject, good);
    RETURN_IF_EXCEPTION(scope, { });
    if (isGood)
        return item;
    record->number = 1;
    RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
}

static JSValue itertoolsStarMapNext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSValue item = iterNext(globalObject, record->get(ItertoolsFunctional::Iterator));
    RETURN_IF_EXCEPTION(scope, { });
    if (!item)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    PyTuple* arguments = PyTuple::tryFrom(item);
    if (!arguments || arguments->type() != rt.tupleType()) {
        arguments = tupleFromIterable(globalObject, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue result = call(globalObject, record->get(ItertoolsFunctional::Function), CallArgs(arguments->span()));
    ensureStillAliveHere(arguments);
    return result;
}

static JSValue itertoolsFilterFalseNext(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSValue iterator = record->get(ItertoolsFunctional::Iterator);
    JSValue function = record->get(ItertoolsFunctional::Function);
    bool usesTruth = isNone(function) || function == JSValue(rt.boolType());
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
        JSValue good = item;
        if (!usesTruth) {
            good = call(globalObject, function, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        bool isGood = isTrue(globalObject, good);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isGood)
            return item;
    }
}

static constexpr NativeDef itertoolsDropWhileMethods[] = {
    PY_NEW(itertoolsDropWhileNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsDropWhileNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(DropWhile, "dropwhile", TypeFlag::Base, TypeFlag::OwnLayout)

static constexpr NativeDef itertoolsTakeWhileMethods[] = {
    PY_NEW(itertoolsTakeWhileNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsTakeWhileNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(TakeWhile, "takewhile", TypeFlag::Base, TypeFlag::OwnLayout)

static constexpr NativeDef itertoolsStarMapMethods[] = {
    PY_NEW(itertoolsStarMapNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsStarMapNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(StarMap, "starmap", TypeFlag::Base, TypeFlag::OwnLayout)

static constexpr NativeDef itertoolsFilterFalseMethods[] = {
    PY_NEW(itertoolsFilterFalseNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsFilterFalseNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(FilterFalse, "filterfalse", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- islice ----

namespace ItertoolsISlice {
enum : unsigned { Iterator, Next, Stop = Next + 2, Step = Stop + 2, FieldCount = Step + 2 }; // number: how many items have been taken from the iterator.
}

ITERTOOLS_DECLARE_TYPE(ISlice);

static JSValue itertoolsISliceNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, itertoolsISliceType, "islice"_s, args)) || failed(scope, checkPositional(globalObject, "islice"_s, args, 2, 4)))
        return { };

    // -1, and no exception, for what is not an integer that fits.
    auto convert = [&](JSValue value) -> int64_t {
        int64_t result = toSsize(globalObject, value, rt.OverflowError());
        if (scope.exception()) {
            takePendingException(globalObject);
            return -1;
        }
        return result;
    };
    static constexpr ASCIILiteral stopMessage = "Stop argument for islice() must be None or an integer: 0 <= x <= sys.maxsize."_s;
    int64_t start = 0;
    int64_t stop = -1;
    int64_t step = 1;
    JSValue stopValue = args.size() == 2 ? args[1] : args[2];
    if (args.size() > 2 && !isNone(args[1]))
        start = convert(args[1]);
    if (!isNone(stopValue)) {
        stop = convert(stopValue);
        if (stop == -1)
            return raiseValueError(globalObject, scope, stopMessage);
    }
    if (start < 0 || stop < -1)
        return raiseValueError(globalObject, scope, "Indices for islice() must be None or an integer: 0 <= x <= sys.maxsize."_s);
    if (args.size() == 4 && !isNone(args[3]))
        step = convert(args[3]);
    if (step < 1)
        return raiseValueError(globalObject, scope, "Step for islice() must be a positive integer or None."_s);

    JSValue iterator = getIter(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsISlice::FieldCount);
    record->set(vm, ItertoolsISlice::Iterator, iterator);
    itertoolsSetInt64(vm, record, ItertoolsISlice::Next, start);
    itertoolsSetInt64(vm, record, ItertoolsISlice::Stop, stop);
    itertoolsSetInt64(vm, record, ItertoolsISlice::Step, step);
    return record;
}

static JSValue itertoolsISliceNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSValue iterator = record->get(ItertoolsISlice::Iterator);
    if (!iterator)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    auto finish = [&]() -> JSValue {
        record->set(vm, ItertoolsISlice::Iterator, { });
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    };
    int64_t stop = itertoolsGetInt64(record, ItertoolsISlice::Stop);
    while (record->number < itertoolsGetInt64(record, ItertoolsISlice::Next)) {
        if (!iterNext(globalObject, iterator))
            return finish();
        ++record->number;
    }
    if (stop != -1 && record->number >= stop)
        return finish();
    JSValue item = iterNext(globalObject, iterator);
    if (!item)
        return finish();
    ++record->number;
    int64_t oldNext = itertoolsGetInt64(record, ItertoolsISlice::Next);
    int64_t next = static_cast<int64_t>(static_cast<uint64_t>(oldNext) + static_cast<uint64_t>(itertoolsGetInt64(record, ItertoolsISlice::Step)));
    if (next < oldNext || (stop != -1 && next > stop))
        next = stop;
    itertoolsSetInt64(vm, record, ItertoolsISlice::Next, next);
    return item;
}

static constexpr NativeDef itertoolsISliceMethods[] = {
    PY_NEW(itertoolsISliceNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsISliceNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(ISlice, "islice", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- chain ----

namespace ItertoolsChain {
enum : unsigned { Source, Active, FieldCount };
}

ITERTOOLS_DECLARE_TYPE(Chain);

static JSValue itertoolsMakeChain(JSGlobalObject* globalObject, JSValue cls, JSValue iterables)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue source = getIter(globalObject, iterables);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsChain::FieldCount);
    record->set(vm, ItertoolsChain::Source, source);
    return record;
}

static JSValue itertoolsChainNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, itertoolsRejectKeywords(globalObject, cls, itertoolsChainType, "chain"_s, args)))
        return { };
    RELEASE_AND_RETURN(scope, itertoolsMakeChain(globalObject, cls, PyTuple::create(globalObject, args.positional())));
}

static JSValue itertoolsChainFromIterable(JSGlobalObject* globalObject, JSValue cls, JSValue iterable) { return itertoolsMakeChain(globalObject, cls, iterable); }

static JSValue itertoolsChainNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    while (JSValue source = record->get(ItertoolsChain::Source)) {
        JSValue active = record->get(ItertoolsChain::Active);
        if (!active) {
            JSValue iterable = iterNext(globalObject, source);
            if (!iterable) {
                record->set(vm, ItertoolsChain::Source, { });
                RETURN_IF_EXCEPTION(scope, { });
                break;
            }
            active = getIter(globalObject, iterable);
            if (!active) {
                record->set(vm, ItertoolsChain::Source, { });
                return { };
            }
            record->set(vm, ItertoolsChain::Active, active);
        }
        JSValue item = iterNext(globalObject, active);
        RETURN_IF_EXCEPTION(scope, { });
        if (item)
            return item;
        record->set(vm, ItertoolsChain::Active, { });
    }
    RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
}

static constexpr NativeDef itertoolsChainMethods[] = {
    PY_NEW(itertoolsChainNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsChainNext, "($self, /)"),
    PY_CLASSMETHOD("from_iterable", itertoolsChainFromIterable, "($type, iterable, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};
ITERTOOLS_DEFINE_TYPE(Chain, "chain", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- product ----

namespace ItertoolsProduct {
enum : unsigned { Pools, Result, FirstIndex }; // An index for each pool follows. number: whether it has stopped.
}

static JSValue itertoolsProductNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t repeat = 1;
    if (args.hasKeywords()) {
        static constexpr ASCIILiteral keywords[] = { "repeat"_s };
        JSValue repeatValue;
        if (failed(scope, unpackKeywordArguments(globalObject, "product"_s, keywords, 0, 0, false, CallArgs(args.values() + args.size(), 0, args.keywordNames()), &repeatValue)))
            return { };
        if (repeatValue && failed(scope, itertoolsSsizeArgument(globalObject, repeatValue, repeat)))
            return { };
        if (repeat < 0)
            return raiseValueError(globalObject, scope, "repeat argument cannot be negative"_s);
    }
    int64_t argumentCount = repeat ? args.size() : 0;
    if (repeat && static_cast<uint64_t>(argumentCount) > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / sizeof(int64_t) / static_cast<uint64_t>(repeat))
        return raiseOverflowError(globalObject, scope, "repeat argument too large"_s);
    int64_t poolCount = argumentCount * repeat;
    if (failed(scope, itertoolsCheckLength(globalObject, poolCount)))
        return { };

    PyTuple* pools = PyTuple::create(globalObject, static_cast<unsigned>(poolCount));
    for (unsigned i = 0; i < argumentCount; ++i) {
        PyTuple* pool = tupleFromIterable(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        pools->initializeAt(vm, i, pool);
    }
    for (unsigned i = static_cast<unsigned>(argumentCount); i < poolCount; ++i)
        pools->initializeAt(vm, i, pools->at(i - static_cast<unsigned>(argumentCount)));

    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsProduct::FirstIndex + static_cast<unsigned>(poolCount));
    record->set(vm, ItertoolsProduct::Pools, pools);
    for (unsigned i = 0; i < poolCount; ++i)
        itertoolsSetIndex(vm, record, ItertoolsProduct::FirstIndex + i, 0);
    return record;
}

static JSValue itertoolsProductNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* record = itertoolsRecord(self);
    if (record->number)
        return itertoolsStop(globalObject);
    PyTuple* pools = PyTuple::from(record->get(ItertoolsProduct::Pools));
    unsigned poolCount = pools->length();
    PyTuple* result;
    if (JSValue previous = record->get(ItertoolsProduct::Result)) {
        // As an odometer: the rightmost index moves, and one that goes back to zero moves the one on its left.
        result = PyTuple::create(globalObject, PyTuple::from(previous)->span());
        unsigned i = poolCount;
        while (i) {
            --i;
            PyTuple* pool = PyTuple::from(pools->at(i));
            unsigned index = itertoolsGetIndex(record, ItertoolsProduct::FirstIndex + i) + 1;
            bool rollsOver = index == pool->length();
            if (rollsOver)
                index = 0;
            itertoolsSetIndex(vm, record, ItertoolsProduct::FirstIndex + i, index);
            result->initializeAt(vm, i, pool->at(index));
            if (!rollsOver)
                break;
            if (!i) {
                record->number = 1;
                return itertoolsStop(globalObject);
            }
        }
        if (!poolCount) {
            record->number = 1;
            return itertoolsStop(globalObject);
        }
    } else {
        result = PyTuple::create(globalObject, poolCount);
        for (unsigned i = 0; i < poolCount; ++i) {
            PyTuple* pool = PyTuple::from(pools->at(i));
            if (!pool->length()) {
                record->number = 1;
                return itertoolsStop(globalObject);
            }
            result->initializeAt(vm, i, pool->at(0));
        }
    }
    record->set(vm, ItertoolsProduct::Result, result);
    return result;
}

static JSValue itertoolsProductSizeOf(JSGlobalObject*, JSValue self)
{
    return jsNumber(48 + 8.0 * PyTuple::from(itertoolsRecord(self)->get(ItertoolsProduct::Pools))->length());
}

static constexpr NativeDef itertoolsProductMethods[] = {
    PY_NEW(itertoolsProductNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsProductNext, "($self, /)"),
    PY_METHOD("__sizeof__", itertoolsProductSizeOf, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Product, "product", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- combinations, combinations_with_replacement ----

namespace ItertoolsCombinations {
enum : unsigned { Pool, Result, Length, FirstIndex = Length + 2 }; // An index for each item of a result follows. number: whether it has stopped.
}

static JSValue itertoolsCombinationsNewImpl(JSGlobalObject* globalObject, JSValue cls, ASCIILiteral name, bool withReplacement, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "iterable"_s, "r"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, name, keywords, 2, 2, false, args, values)))
        return { };
    int64_t length;
    if (failed(scope, itertoolsNonNegativeArgument(globalObject, values[1], "r"_s, length)))
        return { };
    PyTuple* pool = tupleFromIterable(globalObject, values[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, itertoolsCheckLength(globalObject, length)))
        return { };
    bool isStopped = withReplacement ? !pool->length() && length : length > pool->length();
    // What has stopped has no use for the indices.
    unsigned indexCount = isStopped ? 0 : static_cast<unsigned>(length);
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsCombinations::FirstIndex + indexCount);
    record->set(vm, ItertoolsCombinations::Pool, pool);
    itertoolsSetInt64(vm, record, ItertoolsCombinations::Length, length);
    for (unsigned i = 0; i < indexCount; ++i)
        itertoolsSetIndex(vm, record, ItertoolsCombinations::FirstIndex + i, withReplacement ? 0 : i);
    record->number = isStopped;
    return record;
}

static JSValue itertoolsCombinationsNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return itertoolsCombinationsNewImpl(globalObject, cls, "combinations"_s, false, args); }
static JSValue itertoolsCombinationsWithReplacementNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return itertoolsCombinationsNewImpl(globalObject, cls, "combinations_with_replacement"_s, true, args); }

template<bool withReplacement>
static JSValue itertoolsCombinationsNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* record = itertoolsRecord(self);
    if (record->number)
        return itertoolsStop(globalObject);
    PyTuple* pool = PyTuple::from(record->get(ItertoolsCombinations::Pool));
    unsigned poolLength = pool->length();
    unsigned length = static_cast<unsigned>(itertoolsGetInt64(record, ItertoolsCombinations::Length));
    auto indexAt = [&](unsigned i) { return itertoolsGetIndex(record, ItertoolsCombinations::FirstIndex + i); };
    auto setIndexAt = [&](unsigned i, unsigned index) { itertoolsSetIndex(vm, record, ItertoolsCombinations::FirstIndex + i, index); };

    PyTuple* result;
    if (JSValue previous = record->get(ItertoolsCombinations::Result)) {
        // The rightmost index that is not as far as it can go.
        unsigned i = length;
        while (i && indexAt(i - 1) == (withReplacement ? poolLength - 1 : i - 1 + poolLength - length))
            --i;
        if (!i) {
            record->number = 1;
            return itertoolsStop(globalObject);
        }
        --i;
        result = PyTuple::create(globalObject, PyTuple::from(previous)->span());
        // It moves on, and those on its right come back as far as they can.
        unsigned index = indexAt(i) + 1;
        for (; i < length; ++i) {
            setIndexAt(i, index);
            result->initializeAt(vm, i, pool->at(index));
            if (!withReplacement)
                ++index;
        }
    } else {
        result = PyTuple::create(globalObject, length);
        for (unsigned i = 0; i < length; ++i)
            result->initializeAt(vm, i, pool->at(indexAt(i)));
    }
    record->set(vm, ItertoolsCombinations::Result, result);
    return result;
}

static JSValue itertoolsCombinationsSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    return makeInt(globalObject, static_cast<int64_t>(static_cast<uint64_t>(56) + 8 * static_cast<uint64_t>(itertoolsGetInt64(itertoolsRecord(self), ItertoolsCombinations::Length))));
}

static constexpr NativeDef itertoolsCombinationsMethods[] = {
    PY_NEW(itertoolsCombinationsNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsCombinationsNext<false>, "($self, /)"),
    PY_METHOD("__sizeof__", itertoolsCombinationsSizeOf, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Combinations, "combinations", TypeFlag::Base, TypeFlag::OwnLayout)

static constexpr NativeDef itertoolsCombinationsWithReplacementMethods[] = {
    PY_NEW(itertoolsCombinationsWithReplacementNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsCombinationsNext<true>, "($self, /)"),
    PY_METHOD("__sizeof__", itertoolsCombinationsSizeOf, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(CombinationsWithReplacement, "combinations_with_replacement", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- permutations ----

namespace ItertoolsPermutations {
// An index for each item of the pool follows, then a counter for each item of a result. number: whether it has stopped.
enum : unsigned { Pool, Result, Length, FirstIndex = Length + 2 };
}

static JSValue itertoolsPermutationsNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "iterable"_s, "r"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "permutations"_s, keywords, 1, 2, false, args, values)))
        return { };
    PyTuple* pool = tupleFromIterable(globalObject, values[0]);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned poolLength = pool->length();
    int64_t length = poolLength;
    if (values[1] && !isNone(values[1])) {
        if (!isInt(values[1]))
            return raiseTypeError(globalObject, scope, "Expected int as r"_s);
        auto converted = tryInt64(intValue(values[1]));
        if (!converted)
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        length = *converted;
    }
    if (length < 0)
        return raiseValueError(globalObject, scope, "r must be non-negative"_s);
    if (failed(scope, itertoolsCheckLength(globalObject, length)))
        return { };

    bool isStopped = length > poolLength;
    unsigned indexCount = isStopped ? 0 : poolLength;
    unsigned cycleCount = isStopped ? 0 : static_cast<unsigned>(length);
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsPermutations::FirstIndex + indexCount + cycleCount);
    record->set(vm, ItertoolsPermutations::Pool, pool);
    itertoolsSetInt64(vm, record, ItertoolsPermutations::Length, length);
    for (unsigned i = 0; i < indexCount; ++i)
        itertoolsSetIndex(vm, record, ItertoolsPermutations::FirstIndex + i, i);
    for (unsigned i = 0; i < cycleCount; ++i)
        itertoolsSetIndex(vm, record, ItertoolsPermutations::FirstIndex + indexCount + i, poolLength - i);
    record->number = isStopped;
    return record;
}

static JSValue itertoolsPermutationsNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* record = itertoolsRecord(self);
    if (record->number)
        return itertoolsStop(globalObject);
    PyTuple* pool = PyTuple::from(record->get(ItertoolsPermutations::Pool));
    unsigned poolLength = pool->length();
    unsigned length = static_cast<unsigned>(itertoolsGetInt64(record, ItertoolsPermutations::Length));
    auto indexAt = [&](unsigned i) { return itertoolsGetIndex(record, ItertoolsPermutations::FirstIndex + i); };
    auto setIndexAt = [&](unsigned i, unsigned index) { itertoolsSetIndex(vm, record, ItertoolsPermutations::FirstIndex + i, index); };
    auto cycleAt = [&](unsigned i) { return itertoolsGetIndex(record, ItertoolsPermutations::FirstIndex + poolLength + i); };
    auto setCycleAt = [&](unsigned i, unsigned cycle) { itertoolsSetIndex(vm, record, ItertoolsPermutations::FirstIndex + poolLength + i, cycle); };

    PyTuple* result;
    if (JSValue previous = record->get(ItertoolsPermutations::Result)) {
        result = nullptr;
        // The rightmost counter goes down; one that reaches zero starts again, and the one on its left goes down.
        for (unsigned i = length; i--;) {
            unsigned cycle = cycleAt(i) - 1;
            if (!cycle) {
                // indices[i:] = indices[i+1:] + indices[i:i+1]
                unsigned index = indexAt(i);
                for (unsigned j = i; j + 1 < poolLength; ++j)
                    setIndexAt(j, indexAt(j + 1));
                setIndexAt(poolLength - 1, index);
                setCycleAt(i, poolLength - i);
                continue;
            }
            setCycleAt(i, cycle);
            unsigned index = indexAt(i);
            setIndexAt(i, indexAt(poolLength - cycle));
            setIndexAt(poolLength - cycle, index);
            result = PyTuple::create(globalObject, PyTuple::from(previous)->span());
            for (unsigned k = i; k < length; ++k)
                result->initializeAt(vm, k, pool->at(indexAt(k)));
            break;
        }
        if (!result) {
            record->number = 1;
            return itertoolsStop(globalObject);
        }
    } else {
        result = PyTuple::create(globalObject, length);
        for (unsigned i = 0; i < length; ++i)
            result->initializeAt(vm, i, pool->at(indexAt(i)));
    }
    record->set(vm, ItertoolsPermutations::Result, result);
    return result;
}

static JSValue itertoolsPermutationsSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = itertoolsRecord(self);
    uint64_t count = static_cast<uint64_t>(PyTuple::from(record->get(ItertoolsPermutations::Pool))->length()) + static_cast<uint64_t>(itertoolsGetInt64(record, ItertoolsPermutations::Length));
    return makeInt(globalObject, static_cast<int64_t>(64 + 8 * count));
}

static constexpr NativeDef itertoolsPermutationsMethods[] = {
    PY_NEW(itertoolsPermutationsNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsPermutationsNext, "($self, /)"),
    PY_METHOD("__sizeof__", itertoolsPermutationsSizeOf, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Permutations, "permutations", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- accumulate ----

namespace ItertoolsAccumulate {
enum : unsigned { Total, Iterator, Function, Initial, FieldCount }; // Function is empty for addition; Initial is None once it has been given out.
}

static JSValue itertoolsAccumulateNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "iterable"_s, "func"_s, "initial"_s };
    JSValue values[3];
    if (failed(scope, unpackKeywordArguments(globalObject, "accumulate"_s, keywords, 1, 2, false, args, values)))
        return { };
    JSValue iterator = getIter(globalObject, values[0]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsAccumulate::FieldCount);
    record->set(vm, ItertoolsAccumulate::Iterator, iterator);
    if (values[1] && !isNone(values[1]))
        record->set(vm, ItertoolsAccumulate::Function, values[1]);
    record->set(vm, ItertoolsAccumulate::Initial, values[2] ? values[2] : jsUndefined());
    return record;
}

static JSValue itertoolsAccumulateNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSValue initial = record->get(ItertoolsAccumulate::Initial);
    if (!isNone(initial)) {
        record->set(vm, ItertoolsAccumulate::Total, initial);
        record->set(vm, ItertoolsAccumulate::Initial, jsUndefined());
        return initial;
    }
    JSValue value = iterNext(globalObject, record->get(ItertoolsAccumulate::Iterator));
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    JSValue total = record->get(ItertoolsAccumulate::Total);
    if (!total) {
        record->set(vm, ItertoolsAccumulate::Total, value);
        return value;
    }
    JSValue function = record->get(ItertoolsAccumulate::Function);
    total = function ? call(globalObject, function, total, value) : binaryOperation(globalObject, BinaryOperator::Add, total, value);
    RETURN_IF_EXCEPTION(scope, { });
    record->set(vm, ItertoolsAccumulate::Total, total);
    return total;
}

static constexpr NativeDef itertoolsAccumulateMethods[] = {
    PY_NEW(itertoolsAccumulateNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsAccumulateNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Accumulate, "accumulate", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- compress ----

namespace ItertoolsCompress {
enum : unsigned { Data, Selectors, FieldCount };
}

static JSValue itertoolsCompressNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "data"_s, "selectors"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "compress"_s, keywords, 2, 2, false, args, values)))
        return { };
    JSValue data = getIter(globalObject, values[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue selectors = getIter(globalObject, values[1]);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsCompress::FieldCount);
    record->set(vm, ItertoolsCompress::Data, data);
    record->set(vm, ItertoolsCompress::Selectors, selectors);
    return record;
}

static JSValue itertoolsCompressNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSValue data = record->get(ItertoolsCompress::Data);
    JSValue selectors = record->get(ItertoolsCompress::Selectors);
    while (true) {
        JSValue datum = iterNext(globalObject, data);
        RETURN_IF_EXCEPTION(scope, { });
        if (!datum)
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
        JSValue selector = iterNext(globalObject, selectors);
        RETURN_IF_EXCEPTION(scope, { });
        if (!selector)
            RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
        bool isSelected = isTrue(globalObject, selector);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSelected)
            return datum;
    }
}

static constexpr NativeDef itertoolsCompressMethods[] = {
    PY_NEW(itertoolsCompressNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsCompressNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Compress, "compress", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- count ----
//
// An int that goes up by one is counted in `number`. Anything else, and an int that has outgrown that, is an object that
// the step is added to; `number` is then the largest there is.

namespace ItertoolsCount {
enum : unsigned { Count, Step, FieldCount }; // Count is empty while `number` counts.
static constexpr int64_t slowMode = std::numeric_limits<int64_t>::max();
}

static bool itertoolsIsNumber(JSGlobalObject* globalObject, JSValue value)
{
    PyType* type = typeOf(globalObject, value);
    return type->lookup(Special::Index) || type->lookup(Special::Int) || type->lookup(Special::Float) || PyComplex::is(value);
}

static JSValue itertoolsCountNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "start"_s, "step"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "count"_s, keywords, 0, 2, false, args, values)))
        return { };
    JSValue start = values[0];
    JSValue step = values[1];
    if ((start && !itertoolsIsNumber(globalObject, start)) || (step && !itertoolsIsNumber(globalObject, step)))
        return raiseTypeError(globalObject, scope, "a number is required"_s);

    bool isFast = (!start || isInt(start)) && (!step || isInt(step));
    int64_t count = 0;
    if (start && isFast) {
        if (auto converted = tryInt64(intValue(start)))
            count = *converted;
        else
            isFast = false;
    }
    if (isFast && step)
        isFast = tryInt64(intValue(step)) == 1;

    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsCount::FieldCount);
    record->number = isFast ? count : ItertoolsCount::slowMode;
    if (!isFast)
        record->set(vm, ItertoolsCount::Count, start ? start : jsNumber(0));
    record->set(vm, ItertoolsCount::Step, step ? step : jsNumber(1));
    return record;
}

static JSValue itertoolsCountNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    if (record->number != ItertoolsCount::slowMode) [[likely]]
        RELEASE_AND_RETURN(scope, makeInt(globalObject, record->number++));
    JSValue count = record->get(ItertoolsCount::Count);
    if (!count)
        count = makeInt(globalObject, ItertoolsCount::slowMode);
    JSValue next = binaryOperation(globalObject, BinaryOperator::Add, count, record->get(ItertoolsCount::Step));
    RETURN_IF_EXCEPTION(scope, { });
    record->set(vm, ItertoolsCount::Count, next);
    return count;
}

static JSValue itertoolsCountRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    String name = record->type()->name();
    JSValue count = record->get(ItertoolsCount::Count);
    if (!count)
        return makeStr(vm, makeString(name, '(', record->number, ')'));
    JSString* countRepr = repr(globalObject, count);
    RETURN_IF_EXCEPTION(scope, { });
    String countText = countRepr->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue step = record->get(ItertoolsCount::Step);
    if (isInt(step) && tryInt64(intValue(step)) == 1)
        return makeStr(vm, makeString(name, '(', countText, ')'));
    JSString* stepRepr = repr(globalObject, step);
    RETURN_IF_EXCEPTION(scope, { });
    String stepText = stepRepr->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(name, '(', countText, ", "_s, stepText, ')'));
}

static constexpr NativeDef itertoolsCountMethods[] = {
    PY_NEW(itertoolsCountNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsCountNext, "($self, /)"),
    PY_SLOT("__repr__", itertoolsCountRepr, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Count, "count", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- repeat ----

namespace ItertoolsRepeat {
enum : unsigned { Element, FieldCount }; // number: how many times are left, or -1 for no end.
}

static JSValue itertoolsRepeatNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "object"_s, "times"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "repeat"_s, keywords, 1, 2, false, args, values)))
        return { };
    int64_t count = -1;
    if (values[1]) {
        if (failed(scope, itertoolsSsizeArgument(globalObject, values[1], count)))
            return { };
        if (count < 0)
            count = 0;
    }
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsRepeat::FieldCount);
    record->set(vm, ItertoolsRepeat::Element, values[0]);
    record->number = count;
    return record;
}

static JSValue itertoolsRepeatNext(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = itertoolsRecord(self);
    if (!record->number)
        return itertoolsStop(globalObject);
    if (record->number > 0)
        --record->number;
    return record->get(ItertoolsRepeat::Element);
}

static JSValue itertoolsRepeatRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    JSString* element = repr(globalObject, record->get(ItertoolsRepeat::Element));
    RETURN_IF_EXCEPTION(scope, { });
    String text = element->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (record->number == -1)
        return makeStr(vm, makeString(record->type()->name(), '(', text, ')'));
    return makeStr(vm, makeString(record->type()->name(), '(', text, ", "_s, record->number, ')'));
}

static JSValue itertoolsRepeatLengthHint(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    if (record->number == -1)
        return raiseTypeError(globalObject, scope, "len() of unsized object"_s);
    RELEASE_AND_RETURN(scope, makeInt(globalObject, record->number));
}

static constexpr NativeDef itertoolsRepeatMethods[] = {
    PY_NEW(itertoolsRepeatNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsRepeatNext, "($self, /)"),
    PY_SLOT("__repr__", itertoolsRepeatRepr, "($self, /)"),
    PY_METHOD("__length_hint__", itertoolsRepeatLengthHint, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(Repeat, "repeat", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- zip_longest ----

namespace ItertoolsZipLongest {
enum : unsigned { FillValue, FirstIterator }; // The iterators follow; one that is exhausted is empty. number: how many are not.
}

static JSValue itertoolsZipLongestNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue fillValue = jsUndefined();
    if (args.hasKeywords()) {
        fillValue = args.keywordCount() == 1 ? args.keyword("fillvalue"_s) : JSValue();
        if (!fillValue)
            return raiseTypeError(globalObject, scope, "zip_longest() got an unexpected keyword argument"_s);
    }
    unsigned count = args.size();
    PyRecord* record = itertoolsAllocate(globalObject, cls, ItertoolsZipLongest::FirstIterator + count);
    record->set(vm, ItertoolsZipLongest::FillValue, fillValue);
    for (unsigned i = 0; i < count; ++i) {
        JSValue iterator = getIter(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        record->set(vm, ItertoolsZipLongest::FirstIterator + i, iterator);
    }
    record->number = count;
    return record;
}

static JSValue itertoolsZipLongestNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = itertoolsRecord(self);
    if (!record->number)
        RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
    unsigned count = record->fieldCount() - ItertoolsZipLongest::FirstIterator;
    PyTuple* result = PyTuple::create(globalObject, count);
    for (unsigned i = 0; i < count; ++i) {
        JSValue item;
        if (JSValue iterator = record->get(ItertoolsZipLongest::FirstIterator + i)) {
            item = iterNext(globalObject, iterator);
            if (!item) {
                --record->number;
                if (scope.exception() || record->number <= 0) {
                    record->number = 0;
                    RETURN_IF_EXCEPTION(scope, { });
                    RELEASE_AND_RETURN(scope, itertoolsStop(globalObject));
                }
                record->set(vm, ItertoolsZipLongest::FirstIterator + i, { });
            }
        }
        result->initializeAt(vm, i, item ? item : record->get(ItertoolsZipLongest::FillValue));
    }
    return result;
}

static constexpr NativeDef itertoolsZipLongestMethods[] = {
    PY_NEW(itertoolsZipLongestNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", itertoolsSelf, "($self, /)"),
    PY_SLOT("__next__", itertoolsZipLongestNext, "($self, /)"),
};
ITERTOOLS_DEFINE_TYPE(ZipLongest, "zip_longest", TypeFlag::Base, TypeFlag::OwnLayout)

// ---- The module ----

static constexpr NativeDef itertoolsFunctions[] = {
    PY_FUNCTION("tee", itertoolsTeeFunction, "(iterable, n=2, /)"),
};

void initializeItertoolsModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, itertoolsFunctions);
    module->add(globalObject, "accumulate"_s, itertoolsAccumulateType(globalObject));
    module->add(globalObject, "batched"_s, itertoolsBatchedType(globalObject));
    module->add(globalObject, "chain"_s, itertoolsChainType(globalObject));
    module->add(globalObject, "combinations"_s, itertoolsCombinationsType(globalObject));
    module->add(globalObject, "combinations_with_replacement"_s, itertoolsCombinationsWithReplacementType(globalObject));
    module->add(globalObject, "compress"_s, itertoolsCompressType(globalObject));
    module->add(globalObject, "count"_s, itertoolsCountType(globalObject));
    module->add(globalObject, "cycle"_s, itertoolsCycleType(globalObject));
    module->add(globalObject, "dropwhile"_s, itertoolsDropWhileType(globalObject));
    module->add(globalObject, "filterfalse"_s, itertoolsFilterFalseType(globalObject));
    module->add(globalObject, "groupby"_s, itertoolsGroupByType(globalObject));
    module->add(globalObject, "_grouper"_s, itertoolsGrouperType(globalObject));
    module->add(globalObject, "islice"_s, itertoolsISliceType(globalObject));
    module->add(globalObject, "pairwise"_s, itertoolsPairwiseType(globalObject));
    module->add(globalObject, "permutations"_s, itertoolsPermutationsType(globalObject));
    module->add(globalObject, "product"_s, itertoolsProductType(globalObject));
    module->add(globalObject, "repeat"_s, itertoolsRepeatType(globalObject));
    module->add(globalObject, "starmap"_s, itertoolsStarMapType(globalObject));
    module->add(globalObject, "takewhile"_s, itertoolsTakeWhileType(globalObject));
    module->add(globalObject, "_tee"_s, itertoolsTeeType(globalObject));
    module->add(globalObject, "_tee_dataobject"_s, itertoolsTeeDataType(globalObject));
    module->add(globalObject, "zip_longest"_s, itertoolsZipLongestType(globalObject));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
