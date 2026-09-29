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
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonOperations.h"
#include "PythonSequences.h"

// The module itertools: Modules/itertoolsmodule.c of CPython.
//
// Several of its iterators give out a tuple each time, and in CPython give out the same one again if nobody else has kept it. There is no telling that here, and it makes no difference that a program can see.

namespace JSC { namespace Python {

namespace {

#define FOR_EACH_ITERTOOLS_TYPE(v) \
    v(accumulate) v(batched) v(chain) v(combinations) v(compress) v(count) v(combinationsWithReplacement) v(cycle) v(dropwhile) v(filterfalse) v(groupby) v(grouper) v(islice) v(pairwise) v(permutations) v(product) \
    v(repeat) v(starmap) v(takewhile) v(tee) v(teeData) v(zipLongest)

struct ItertoolsState final : NativeState {
    PYTHON_NATIVE_STATE(ItertoolsState);
#define DECLARE(name) WriteBarrier<PyType> name;
    FOR_EACH_ITERTOOLS_TYPE(DECLARE)
#undef DECLARE
};

template<typename Visitor>
void ItertoolsState::visit(Visitor& visitor)
{
#define VISIT(name) visitor.append(name);
    FOR_EACH_ITERTOOLS_TYPE(VISIT)
#undef VISIT
}

ItertoolsState& initializeItertools(JSGlobalObject*);

ItertoolsState& itertoolsState(JSGlobalObject* globalObject)
{
    auto& state = globalObject->pyRealm()->moduleState<ItertoolsState>();
    return state.accumulate ? state : initializeItertools(globalObject);
}

// type->tp_alloc(type, 0), and what the fields are is up to whoever asked.
template<typename State>
PyStateObject* allocate(VM& vm, JSValue type)
{
    return PyStateObject::create(vm, asType(type)->instanceStructure(), makeUnique<State>());
}

// What `return NULL` comes to in a tp_iternext, when nothing has been raised.
#define STOP() return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()))

// (type == base || type->tp_init == base->tp_init) && !_PyArg_NoKeywords(): a class derived from one of these that has an __init__() of its own can be given keywords, which are for that. False if it raised.
bool checkNoKeywords(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, PyType* base, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    PyType* type = asType(args[0]);
    if (type != base && type->lookup(vm, vm.pythonNames().dunder_init) != base->lookup(vm, vm.pythonNames().dunder_init))
        return true;
    return args.checkNoKeywords(globalObject, scope, name);
}

// _PyArg_CheckPositional(). False if it raised.
bool checkPositional(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral name, unsigned count, unsigned minimum, unsigned maximum)
{
    if (count >= minimum && count <= maximum)
        return true;
    if (minimum == maximum)
        raiseTypeError(globalObject, scope, concatenate(name, " expected "_s, minimum, " argument"_s, minimum == 1 ? ""_s : "s"_s, ", got "_s, count));
    else if (count < minimum)
        raiseTypeError(globalObject, scope, concatenate(name, " expected at least "_s, minimum, " argument"_s, minimum == 1 ? ""_s : "s"_s, ", got "_s, count));
    else
        raiseTypeError(globalObject, scope, concatenate(name, " expected at most "_s, maximum, " argument"_s, maximum == 1 ? ""_s : "s"_s, ", got "_s, count));
    return false;
}

// What clinic makes for a __new__() whose parameters are all positional only.
#define CHECK_POSITIONAL_ONLY(member, name, count) \
    if (!checkNoKeywords(globalObject, scope, args, itertoolsState(globalObject).member.get(), name) || !checkPositional(globalObject, scope, name, args.size() - 1, count, count)) \
        return { };

PyTuple* copyOf(JSGlobalObject* globalObject, PyTuple* tuple)
{
    VM& vm = globalObject->vm();
    PyTuple* copy = PyTuple::create(globalObject, tuple->length());
    for (unsigned i = 0; i < tuple->length(); ++i)
        copy->initializeAt(vm, i, tuple->at(i));
    return copy;
}

// PyMem_New(Py_ssize_t, count), all zero. False if it raised.
bool allocateIndices(JSGlobalObject* globalObject, ThrowScope& scope, Vector<int64_t>& indices, int64_t count)
{
    if (count > std::numeric_limits<int32_t>::max() || !indices.tryGrow(static_cast<size_t>(count))) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    indices.fill(0);
    return true;
}

} // anonymous namespace

PYTHON_NATIVE(itertoolsSelf)
{
    return JSValue::encode(callFrame->uncheckedArgument(0));
}

// ---- batched

namespace {
struct BatchedState final : NativeState {
    PYTHON_NATIVE_STATE(BatchedState);
    WriteBarrier<Unknown> iterator;
    int64_t batchSize { 0 };
    bool isStrict { false };
};
template<typename Visitor> void BatchedState::visit(Visitor& visitor) { visitor.append(iterator); }
}

// batched(iterable, n, *, strict=False)
PYTHON_NATIVE(batchedNew)
{
    NATIVE_PROLOGUE();
    auto n = toSsize(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    bool isStrict = false;
    if (JSValue value = args.at(3)) {
        isStrict = isTrue(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (*n < 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "n must be at least one"_s));
    JSValue iterator = getIterator(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = allocate<BatchedState>(vm, args[0]);
    auto& state = object->state<BatchedState>();
    state.iterator.set(vm, object, iterator);
    state.batchSize = *n;
    state.isStrict = isStrict;
    return JSValue::encode(object);
}

PYTHON_NATIVE(batchedNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<BatchedState>(args[0]);
    int64_t n = state.batchSize;
    if (n < 0)
        STOP();
    // CPython makes the tuple first, of the size that was asked for.
    if (n > std::numeric_limits<int32_t>::max())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    auto finish = [&] {
        state.batchSize = -1;
        state.iterator.clear();
    };
    JSValue iterator = state.iterator.get();
    MarkedArgumentBuffer items;
    for (int64_t i = 0; i < n; ++i) {
        JSValue item = iteratorNext(globalObject, iterator);
        if (scope.exception()) [[unlikely]] {
            finish();
            return { };
        }
        if (!item)
            break;
        items.append(item);
    }
    if (static_cast<int64_t>(items.size()) == n)
        return JSValue::encode(PyTuple::createFromArguments(globalObject, items));
    if (!items.size()) {
        finish();
        STOP();
    }
    if (state.isStrict) {
        finish();
        return JSValue::encode(raiseValueError(globalObject, scope, "batched(): incomplete batch"_s));
    }
    return JSValue::encode(PyTuple::createFromArguments(globalObject, items));
}

// ---- pairwise

namespace {
struct PairwiseState final : NativeState {
    PYTHON_NATIVE_STATE(PairwiseState);
    WriteBarrier<Unknown> iterator;
    WriteBarrier<Unknown> old;
};
template<typename Visitor> void PairwiseState::visit(Visitor& visitor)
{
    visitor.append(iterator);
    visitor.append(old);
}
}

PYTHON_NATIVE(pairwiseNew)
{
    NATIVE_PROLOGUE();
    CHECK_POSITIONAL_ONLY(pairwise, "pairwise"_s, 1);
    JSValue iterator = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = allocate<PairwiseState>(vm, args[0]);
    object->state<PairwiseState>().iterator.set(vm, object, iterator);
    return JSValue::encode(object);
}

PYTHON_NATIVE(pairwiseNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<PairwiseState>(self);
    JSValue iterator = state.iterator.get();
    if (!iterator)
        STOP();
    JSValue old = state.old.get();
    if (!old) {
        old = iteratorStep(globalObject, iterator);
        if (!old) {
            state.old.clear();
            state.iterator.clear();
            RETURN_IF_EXCEPTION(scope, { });
            STOP();
        }
        state.old.set(vm, self, old);
        // Asking it can have come back round to this.
        iterator = state.iterator.get();
        if (!iterator) {
            state.old.clear();
            STOP();
        }
    }
    JSValue next = iteratorStep(globalObject, iterator);
    if (!next) {
        state.iterator.clear();
        state.old.clear();
        RETURN_IF_EXCEPTION(scope, { });
        STOP();
    }
    state.old.set(vm, self, next);
    return JSValue::encode(PyTuple::create(globalObject, { old, next }));
}

// ---- groupby

namespace {
struct GroupByState final : NativeState {
    PYTHON_NATIVE_STATE(GroupByState);
    WriteBarrier<Unknown> iterator;
    WriteBarrier<Unknown> keyFunction;
    WriteBarrier<Unknown> targetKey;
    WriteBarrier<Unknown> currentKey;
    WriteBarrier<Unknown> currentValue;
    // Which group is being gone through. It is only ever compared with, so it keeps nothing alive, and each group that is made is put here.
    JSCell* currentGrouper { nullptr };
};
template<typename Visitor> void GroupByState::visit(Visitor& visitor)
{
    visitor.append(iterator);
    visitor.append(keyFunction);
    visitor.append(targetKey);
    visitor.append(currentKey);
    visitor.append(currentValue);
}

struct GrouperState final : NativeState {
    PYTHON_NATIVE_STATE(GrouperState);
    WriteBarrier<Unknown> parent;
    WriteBarrier<Unknown> targetKey;
};
template<typename Visitor> void GrouperState::visit(Visitor& visitor)
{
    visitor.append(parent);
    visitor.append(targetKey);
}

// _grouper_create()
JSValue createGrouper(JSGlobalObject* globalObject, JSValue parent, JSValue targetKey)
{
    VM& vm = globalObject->vm();
    auto* object = allocate<GrouperState>(vm, itertoolsState(globalObject).grouper.get());
    auto& state = object->state<GrouperState>();
    state.parent.set(vm, object, parent);
    state.targetKey.set(vm, object, targetKey);
    stateOf<GroupByState>(parent).currentGrouper = object;
    return object;
}

// groupby_step(). False at the end, and if it raised.
bool stepGroupBy(JSGlobalObject* globalObject, JSCell* self, GroupByState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = iteratorNext(globalObject, state.iterator.get());
    RETURN_IF_EXCEPTION(scope, false);
    if (!value)
        return false;
    JSValue key = value;
    if (!isNone(state.keyFunction.get())) {
        key = call(globalObject, state.keyFunction.get(), value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    state.currentValue.set(vm, self, value);
    state.currentKey.set(vm, self, key);
    return true;
}
}

// groupby(iterable, key=None)
PYTHON_NATIVE(groupByNew)
{
    NATIVE_PROLOGUE();
    JSValue key = args.at(2);
    auto* object = allocate<GroupByState>(vm, args[0]);
    auto& state = object->state<GroupByState>();
    state.keyFunction.set(vm, object, key ? key : jsUndefined());
    JSValue iterator = getIterator(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    state.iterator.set(vm, object, iterator);
    return JSValue::encode(object);
}

PYTHON_NATIVE(groupByNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<GroupByState>(self);
    state.currentGrouper = nullptr;
    // On to the next group.
    while (true) {
        if (state.currentKey) {
            if (!state.targetKey)
                break;
            // What compares them can come back round to this.
            bool isSame = isEqual(globalObject, state.targetKey.get(), state.currentKey.get());
            RETURN_IF_EXCEPTION(scope, { });
            if (!isSame)
                break;
        }
        bool hasMore = stepGroupBy(globalObject, self, state);
        RETURN_IF_EXCEPTION(scope, { });
        if (!hasMore)
            STOP();
    }
    state.targetKey.set(vm, self, state.currentKey.get());
    JSValue grouper = createGrouper(globalObject, self, state.targetKey.get());
    return JSValue::encode(PyTuple::create(globalObject, { state.currentKey.get(), grouper }));
}

// _grouper(parent, tgtkey, /)
PYTHON_NATIVE(grouperNew)
{
    NATIVE_PROLOGUE();
    CHECK_POSITIONAL_ONLY(grouper, "_grouper"_s, 2);
    if (!isInstance(globalObject, args[1], itertoolsState(globalObject).groupby.get()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("_grouper() argument 1 must be itertools.groupby, not "_s, typeNameOfArgument(globalObject, args[1]))));
    return JSValue::encode(createGrouper(globalObject, args[1], args[2]));
}

PYTHON_NATIVE(grouperNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& grouper = stateOf<GrouperState>(self);
    JSCell* parent = grouper.parent.get().asCell();
    auto& state = stateOf<GroupByState>(parent);
    if (state.currentGrouper != self)
        STOP();
    if (!state.currentValue) {
        bool hasMore = stepGroupBy(globalObject, parent, state);
        RETURN_IF_EXCEPTION(scope, { });
        if (!hasMore)
            STOP();
    }
    bool isSame = isEqual(globalObject, grouper.targetKey.get(), state.currentKey.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isSame)
        STOP();
    JSValue result = state.currentValue.get();
    state.currentValue.clear();
    state.currentKey.clear();
    return JSValue::encode(result);
}

// ---- tee

namespace {
// What has been got from the iterator and not yet given out by every one of the iterators that share it is kept in a chain of these, so many to each.
static constexpr int linkCells = 57;

struct TeeDataState final : NativeState {
    PYTHON_NATIVE_STATE(TeeDataState);
    WriteBarrier<Unknown> iterator;
    int countRead { 0 };
    bool isRunning { false };
    WriteBarrier<Unknown> nextLink;
    std::array<WriteBarrier<Unknown>, linkCells> values;
};
template<typename Visitor> void TeeDataState::visit(Visitor& visitor)
{
    visitor.append(iterator);
    visitor.append(nextLink);
    for (auto& value : values)
        visitor.append(value);
}

struct TeeState final : NativeState {
    PYTHON_NATIVE_STATE(TeeState);
    WriteBarrier<Unknown> data;
    int index { 0 };
};
template<typename Visitor> void TeeState::visit(Visitor& visitor) { visitor.append(data); }

// teedataobject_newinternal()
PyStateObject* createTeeData(JSGlobalObject* globalObject, JSValue iterator)
{
    VM& vm = globalObject->vm();
    auto* object = allocate<TeeDataState>(vm, itertoolsState(globalObject).teeData.get());
    object->state<TeeDataState>().iterator.set(vm, object, iterator);
    return object;
}

// tee_copy_impl()
JSValue copyTee(JSGlobalObject* globalObject, JSValue tee)
{
    VM& vm = globalObject->vm();
    auto& from = stateOf<TeeState>(tee);
    auto* object = allocate<TeeState>(vm, typeOf(globalObject, tee));
    auto& state = object->state<TeeState>();
    state.data.set(vm, object, from.data.get());
    state.index = from.index;
    return object;
}

// tee_fromiterable(). Empty if it raised.
JSValue teeFromIterable(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIterator(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* type = itertoolsState(globalObject).tee.get();
    if (isInstance(globalObject, iterator, type))
        return copyTee(globalObject, iterator);
    auto* object = allocate<TeeState>(vm, type);
    object->state<TeeState>().data.set(vm, object, createTeeData(globalObject, iterator));
    return object;
}
}

// _tee_dataobject(iterable, values, next, /)
PYTHON_NATIVE(teeDataNew)
{
    NATIVE_PROLOGUE();
    CHECK_POSITIONAL_ONLY(teeData, "teedataobject"_s, 3);
    if (!isInstance(globalObject, args[2], realm->typeList()))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("teedataobject() argument 2 must be list, not "_s, typeNameOfArgument(globalObject, args[2]))));
    auto invalid = [&] { return JSValue::encode(raiseValueError(globalObject, scope, "Invalid arguments"_s)); };
    auto* object = createTeeData(globalObject, args[1]);
    auto& state = object->state<TeeDataState>();
    JSArray* values = asList(args[2]);
    unsigned length = values->length();
    if (length > linkCells)
        return invalid();
    for (unsigned i = 0; i < length; ++i)
        state.values[i].set(vm, object, values->getIndexQuickly(i));
    state.countRead = static_cast<int>(length);
    JSValue next = args[3];
    if (!isNone(next)) {
        // It has nothing after it unless it is full.
        if (length != linkCells || typeOf(globalObject, next) != itertoolsState(globalObject).teeData.get())
            return invalid();
        state.nextLink.set(vm, object, next);
    }
    return JSValue::encode(object);
}

PYTHON_NATIVE(teeNew)
{
    NATIVE_PROLOGUE();
    CHECK_POSITIONAL_ONLY(tee, "_tee"_s, 1);
    RELEASE_AND_RETURN(scope, JSValue::encode(teeFromIterable(globalObject, args[1])));
}

PYTHON_NATIVE(teeNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<TeeState>(self);
    if (state.index >= linkCells) {
        // teedataobject_jumplink()
        JSCell* dataCell = state.data.get().asCell();
        auto& data = stateOf<TeeDataState>(dataCell);
        if (!data.nextLink)
            data.nextLink.set(vm, dataCell, createTeeData(globalObject, data.iterator.get()));
        state.data.set(vm, self, data.nextLink.get());
        state.index = 0;
    }
    // teedataobject_getitem()
    JSCell* dataCell = state.data.get().asCell();
    auto& data = stateOf<TeeDataState>(dataCell);
    int index = state.index;
    JSValue value;
    if (index < data.countRead)
        value = data.values[index].get();
    else {
        // This one is in front, and gets some more.
        if (data.isRunning)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, "cannot re-enter the tee iterator"_s));
        data.isRunning = true;
        value = iteratorNext(globalObject, data.iterator.get());
        data.isRunning = false;
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            STOP();
        ++data.countRead;
        data.values[index].set(vm, dataCell, value);
    }
    ++state.index;
    return JSValue::encode(value);
}

PYTHON_NATIVE(teeCopy)
{
    return JSValue::encode(copyTee(globalObject, callFrame->uncheckedArgument(0)));
}

// tee(iterable, n=2, /)
PYTHON_NATIVE(itertoolsTee)
{
    NATIVE_PROLOGUE();
    int64_t n = 2;
    if (JSValue value = args.at(1)) {
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        n = *given;
    }
    if (n < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "n must be >= 0"_s));
    if (n > std::numeric_limits<int32_t>::max())
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    if (!n)
        return JSValue::encode(realm->emptyTuple());
    JSValue iterator = getIterator(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue tee = teeFromIterable(globalObject, iterator);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer result;
    result.append(tee);
    for (int64_t i = 1; i < n; ++i) {
        tee = copyTee(globalObject, tee);
        result.append(tee);
    }
    return JSValue::encode(PyTuple::createFromArguments(globalObject, result));
}

// ---- cycle

namespace {
struct CycleState final : NativeState {
    PYTHON_NATIVE_STATE(CycleState);
    WriteBarrier<Unknown> iterator;
    WriteBarrier<JSArray> saved;
    int64_t index { -1 };
};
template<typename Visitor> void CycleState::visit(Visitor& visitor)
{
    visitor.append(iterator);
    visitor.append(saved);
}
}

PYTHON_NATIVE(cycleNew)
{
    NATIVE_PROLOGUE();
    CHECK_POSITIONAL_ONLY(cycle, "cycle"_s, 1);
    JSValue iterator = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = allocate<CycleState>(vm, args[0]);
    auto& state = object->state<CycleState>();
    state.iterator.set(vm, object, iterator);
    state.saved.set(vm, object, newList(globalObject));
    return JSValue::encode(object);
}

PYTHON_NATIVE(cycleNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<CycleState>(args[0]);
    JSArray* saved = state.saved.get();
    if (state.index < 0) {
        JSValue item = iteratorNext(globalObject, state.iterator.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (item) {
            listAppend(globalObject, saved, item);
            RETURN_IF_EXCEPTION(scope, { });
            return JSValue::encode(item);
        }
        state.index = 0;
        state.iterator.clear();
    }
    if (!saved->length())
        STOP();
    JSValue item = saved->getIndexQuickly(static_cast<unsigned>(state.index));
    if (++state.index >= saved->length())
        state.index = 0;
    return JSValue::encode(item);
}

// ---- dropwhile, takewhile and filterfalse

namespace {
struct PredicateState final : NativeState {
    PYTHON_NATIVE_STATE(PredicateState);
    WriteBarrier<Unknown> function;
    WriteBarrier<Unknown> iterator;
    bool hasTurned { false }; // dropwhile has started, or takewhile has stopped.
};
template<typename Visitor> void PredicateState::visit(Visitor& visitor)
{
    visitor.append(function);
    visitor.append(iterator);
}

enum class Predicate : uint8_t { DropWhile, TakeWhile, FilterFalse, StarMap };
}

// dropwhile(predicate, iterable, /), and the others likewise
PYTHON_NATIVE(predicateNew)
{
    NATIVE_PROLOGUE();
    switch (unpack<Predicate>(callFrame, 0)) {
    case Predicate::DropWhile:
        CHECK_POSITIONAL_ONLY(dropwhile, "dropwhile"_s, 2);
        break;
    case Predicate::TakeWhile:
        CHECK_POSITIONAL_ONLY(takewhile, "takewhile"_s, 2);
        break;
    case Predicate::FilterFalse:
        CHECK_POSITIONAL_ONLY(filterfalse, "filterfalse"_s, 2);
        break;
    case Predicate::StarMap:
        CHECK_POSITIONAL_ONLY(starmap, "starmap"_s, 2);
        break;
    }
    JSValue iterator = getIterator(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = allocate<PredicateState>(vm, args[0]);
    auto& state = object->state<PredicateState>();
    state.function.set(vm, object, args[1]);
    state.iterator.set(vm, object, iterator);
    return JSValue::encode(object);
}

PYTHON_NATIVE(dropWhileNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<PredicateState>(args[0]);
    JSValue iterator = state.iterator.get();
    while (true) {
        JSValue item = iteratorStep(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            STOP();
        if (state.hasTurned)
            return JSValue::encode(item);
        JSValue good = call(globalObject, state.function.get(), item);
        RETURN_IF_EXCEPTION(scope, { });
        bool isGood = isTrue(globalObject, good);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isGood) {
            state.hasTurned = true;
            return JSValue::encode(item);
        }
    }
}

PYTHON_NATIVE(takeWhileNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<PredicateState>(args[0]);
    if (state.hasTurned)
        STOP();
    JSValue item = iteratorStep(globalObject, state.iterator.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!item)
        STOP();
    JSValue good = call(globalObject, state.function.get(), item);
    RETURN_IF_EXCEPTION(scope, { });
    bool isGood = isTrue(globalObject, good);
    RETURN_IF_EXCEPTION(scope, { });
    if (isGood)
        return JSValue::encode(item);
    state.hasTurned = true;
    STOP();
}

PYTHON_NATIVE(filterFalseNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<PredicateState>(args[0]);
    JSValue iterator = state.iterator.get();
    JSValue function = state.function.get();
    bool asksTheItem = isNone(function) || function == JSValue(realm->typeBool()->object());
    while (true) {
        JSValue item = iteratorStep(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            STOP();
        JSValue good = item;
        if (!asksTheItem) {
            good = call(globalObject, function, item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        bool isGood = isTrue(globalObject, good);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isGood)
            return JSValue::encode(item);
    }
}

PYTHON_NATIVE(starMapNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<PredicateState>(args[0]);
    JSValue given = iteratorStep(globalObject, state.iterator.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!given)
        STOP();
    PyTuple* tuple = typeOf(globalObject, given) == realm->typeTuple() ? asTuple(given) : tupleFromIterable(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    for (auto& value : tuple->span())
        arguments.append(value.get());
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, state.function.get(), arguments)));
}

// ---- islice

namespace {
struct IsliceState final : NativeState {
    PYTHON_NATIVE_STATE(IsliceState);
    WriteBarrier<Unknown> iterator;
    int64_t next { 0 };
    int64_t stop { -1 };
    int64_t step { 1 };
    int64_t count { 0 };
};
template<typename Visitor> void IsliceState::visit(Visitor& visitor) { visitor.append(iterator); }

// PyNumber_AsSsize_t(value, PyExc_OverflowError), with whatever it raises cleared: -1 then.
int64_t toSsizeOrMinusOne(JSGlobalObject* globalObject, JSValue value, bool& failed)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto result = toIndex(globalObject, value);
    if (scope.exception()) [[unlikely]] {
        failed = !scope.tryClearException();
        return -1;
    }
    return *result;
}
}

// islice(iterable, stop), and islice(iterable, start, stop[, step])
PYTHON_NATIVE(isliceNew)
{
    NATIVE_PROLOGUE();
    if (!checkNoKeywords(globalObject, scope, args, itertoolsState(globalObject).islice.get(), "islice"_s) || !checkPositional(globalObject, scope, "islice"_s, args.size() - 1, 2, 4))
        return { };
    int64_t start = 0;
    int64_t stop = -1;
    int64_t step = 1;
    bool failed = false;
    auto badStop = [&] { return JSValue::encode(raiseValueError(globalObject, scope, "Stop argument for islice() must be None or an integer: 0 <= x <= sys.maxsize."_s)); };
    unsigned count = args.size() - 1;
    JSValue stopValue = count == 2 ? args[2] : args[3];
    if (count > 2 && !isNone(args[2])) {
        start = toSsizeOrMinusOne(globalObject, args[2], failed);
        if (failed)
            return { };
    }
    if (!isNone(stopValue)) {
        stop = toSsizeOrMinusOne(globalObject, stopValue, failed);
        if (failed)
            return { };
        if (stop == -1)
            return badStop();
    }
    if (start < 0 || stop < -1)
        return JSValue::encode(raiseValueError(globalObject, scope, "Indices for islice() must be None or an integer: 0 <= x <= sys.maxsize."_s));
    if (count > 3 && !isNone(args[4])) {
        step = toSsizeOrMinusOne(globalObject, args[4], failed);
        if (failed)
            return { };
    }
    if (step < 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "Step for islice() must be a positive integer or None."_s));
    JSValue iterator = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = allocate<IsliceState>(vm, args[0]);
    auto& state = object->state<IsliceState>();
    state.iterator.set(vm, object, iterator);
    state.next = start;
    state.stop = stop;
    state.step = step;
    return JSValue::encode(object);
}

PYTHON_NATIVE(isliceNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<IsliceState>(args[0]);
    JSValue iterator = state.iterator.get();
    if (!iterator)
        STOP();
    int64_t stop = state.stop;
    auto step = [&] () -> JSValue {
        JSValue item = iteratorStep(globalObject, iterator);
        if (!item)
            state.iterator.clear();
        return item;
    };
    while (state.count < state.next) {
        JSValue item = step();
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            STOP();
        ++state.count;
    }
    if (stop != -1 && state.count >= stop) {
        state.iterator.clear();
        STOP();
    }
    JSValue item = step();
    RETURN_IF_EXCEPTION(scope, { });
    if (!item)
        STOP();
    ++state.count;
    int64_t oldNext = state.next;
    state.next = static_cast<int64_t>(static_cast<uint64_t>(state.next) + static_cast<uint64_t>(state.step));
    if (state.next < oldNext || (stop != -1 && state.next > stop))
        state.next = stop;
    return JSValue::encode(item);
}

// ---- chain

namespace {
struct ChainState final : NativeState {
    PYTHON_NATIVE_STATE(ChainState);
    WriteBarrier<Unknown> source; // What goes through the iterables.
    WriteBarrier<Unknown> active; // What goes through the one that it has got to.
};
template<typename Visitor> void ChainState::visit(Visitor& visitor)
{
    visitor.append(source);
    visitor.append(active);
}

JSValue createChain(VM& vm, JSValue type, JSValue source)
{
    auto* object = allocate<ChainState>(vm, type);
    object->state<ChainState>().source.set(vm, object, source);
    return object;
}
}

// chain(*iterables)
PYTHON_NATIVE(chainNew)
{
    NATIVE_PROLOGUE();
    if (!checkNoKeywords(globalObject, scope, args, itertoolsState(globalObject).chain.get(), "chain"_s))
        return { };
    MarkedArgumentBuffer iterables;
    for (unsigned i = 1; i < args.size(); ++i)
        iterables.append(args[i]);
    JSValue source = getIterator(globalObject, PyTuple::createFromArguments(globalObject, iterables));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(createChain(vm, args[0], source));
}

PYTHON_NATIVE(chainFromIterable)
{
    NATIVE_PROLOGUE();
    JSValue source = getIterator(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(createChain(vm, args[0], source));
}

PYTHON_NATIVE(chainNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<ChainState>(self);
    while (state.source) {
        if (!state.active) {
            JSValue iterable = iteratorNext(globalObject, state.source.get());
            if (!iterable) {
                state.source.clear();
                RETURN_IF_EXCEPTION(scope, { });
                STOP();
            }
            JSValue active = getIterator(globalObject, iterable);
            if (scope.exception()) [[unlikely]] {
                state.source.clear();
                return { };
            }
            state.active.set(vm, self, active);
        }
        JSValue item = iteratorNext(globalObject, state.active.get());
        RETURN_IF_EXCEPTION(scope, { });
        if (item)
            return JSValue::encode(item);
        state.active.clear();
    }
    STOP();
}

// ---- product, combinations, combinations_with_replacement and permutations

namespace {
struct CombinatoricState final : NativeState {
    PYTHON_NATIVE_STATE(CombinatoricState);
    WriteBarrier<PyTuple> pool; // What was given, as a tuple. For product, a tuple of those.
    WriteBarrier<PyTuple> result; // What was given out last.
    Vector<int64_t> indices;
    Vector<int64_t> cycles; // For permutations: for each place in the result, how long until it comes round.
    int64_t r { 0 }; // How long the result is.
    bool isStopped { false };
};
template<typename Visitor> void CombinatoricState::visit(Visitor& visitor)
{
    visitor.append(pool);
    visitor.append(result);
}

// A copy of what was given out last, to be changed and given out next.
PyTuple* nextResult(JSGlobalObject* globalObject, JSCell* self, CombinatoricState& state)
{
    PyTuple* result = copyOf(globalObject, state.result.get());
    state.result.set(globalObject->vm(), self, result);
    return result;
}
}

// product(*iterables, repeat=1)
PYTHON_NATIVE(productNew)
{
    NATIVE_PROLOGUE();
    int64_t repeat = 1;
    if (args.keywordCount()) {
        // PyArg_ParseTupleAndKeywords() of no arguments and the keywords, with "|n:product"
        if (args.keywordCount() > 1)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("product() takes at most 1 keyword argument ("_s, args.keywordCount(), " given)"_s)));
        for (unsigned i = 0; i < args.keywordCount(); ++i) {
            String name = asString(args.keywordName(i))->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (name != "repeat"_s)
                return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("product() got an unexpected keyword argument '"_s, name, '\'')));
            auto given = toSsize(globalObject, args.keywordValue(i));
            RETURN_IF_EXCEPTION(scope, { });
            repeat = *given;
        }
        if (repeat < 0)
            return JSValue::encode(raiseValueError(globalObject, scope, "repeat argument cannot be negative"_s));
    }
    int64_t argumentCount = repeat ? args.size() - 1 : 0;
    if (repeat && static_cast<uint64_t>(argumentCount) > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / sizeof(int64_t) / static_cast<uint64_t>(repeat))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OverflowError, "repeat argument too large"_s));
    int64_t poolCount = argumentCount * repeat;
    auto* object = allocate<CombinatoricState>(vm, args[0]);
    auto& state = object->state<CombinatoricState>();
    if (!allocateIndices(globalObject, scope, state.indices, poolCount))
        return { };
    MarkedArgumentBuffer pools;
    for (int64_t i = 0; i < argumentCount; ++i) {
        PyTuple* pool = tupleFromIterable(globalObject, args[static_cast<unsigned>(i + 1)]);
        RETURN_IF_EXCEPTION(scope, { });
        pools.append(pool);
    }
    for (int64_t i = argumentCount; i < poolCount; ++i)
        pools.append(pools.at(static_cast<unsigned>(i - argumentCount)));
    state.pool.set(vm, object, PyTuple::createFromArguments(globalObject, pools));
    return JSValue::encode(object);
}

PYTHON_NATIVE(productNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<CombinatoricState>(self);
    if (state.isStopped)
        STOP();
    PyTuple* pools = state.pool.get();
    unsigned poolCount = pools->length();
    if (!state.result) {
        // The first of each.
        PyTuple* result = PyTuple::create(globalObject, poolCount);
        for (unsigned i = 0; i < poolCount; ++i)
            result->initializeAt(vm, i, jsUndefined());
        state.result.set(vm, self, result);
        for (unsigned i = 0; i < poolCount; ++i) {
            PyTuple* pool = asTuple(pools->at(i));
            if (!pool->length()) {
                state.isStopped = true;
                STOP();
            }
            result->initializeAt(vm, i, pool->at(0));
        }
        return JSValue::encode(result);
    }
    PyTuple* result = nextResult(globalObject, self, state);
    // From the right, going on to the next to the left only when one has come round.
    int64_t i = static_cast<int64_t>(poolCount) - 1;
    for (; i >= 0; --i) {
        PyTuple* pool = asTuple(pools->at(static_cast<unsigned>(i)));
        if (++state.indices[i] == pool->length()) {
            state.indices[i] = 0;
            result->initializeAt(vm, static_cast<unsigned>(i), pool->at(0));
        } else {
            result->initializeAt(vm, static_cast<unsigned>(i), pool->at(static_cast<unsigned>(state.indices[i])));
            break;
        }
    }
    if (i < 0) {
        state.isStopped = true;
        STOP();
    }
    return JSValue::encode(result);
}

enum class Combinatoric : uint8_t { Product, Combinations, WithReplacement, Permutations };

PYTHON_NATIVE(combinatoricSizeOf)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& state = stateOf<CombinatoricState>(args[0]);
    int64_t size = typeOf(globalObject, args[0])->basicSize();
    switch (unpack<Combinatoric>(callFrame, 0)) {
    case Combinatoric::Product:
        size += static_cast<int64_t>(state.pool->length()) * 8;
        break;
    case Combinatoric::Combinations:
    case Combinatoric::WithReplacement:
        size += state.r * 8;
        break;
    case Combinatoric::Permutations:
        size += (static_cast<int64_t>(state.pool->length()) + state.r) * 8;
        break;
    }
    return JSValue::encode(intFromInt64(globalObject, size));
}

// combinations(iterable, r), and combinations_with_replacement(iterable, r)
PYTHON_NATIVE(combinationsNew)
{
    bool isWithReplacement = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto r = toSsize(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* pool = tupleFromIterable(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t n = pool->length();
    if (*r < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "r must be non-negative"_s));
    auto* object = allocate<CombinatoricState>(vm, args[0]);
    auto& state = object->state<CombinatoricState>();
    if (!allocateIndices(globalObject, scope, state.indices, *r))
        return { };
    for (int64_t i = 0; i < *r; ++i)
        state.indices[i] = isWithReplacement ? 0 : i;
    state.pool.set(vm, object, pool);
    state.r = *r;
    state.isStopped = isWithReplacement ? !n && *r : *r > n;
    return JSValue::encode(object);
}

// The first that is given out: what the indices say.
static JSValue firstResult(JSGlobalObject* globalObject, JSCell* self, CombinatoricState& state)
{
    VM& vm = globalObject->vm();
    PyTuple* pool = state.pool.get();
    PyTuple* result = PyTuple::create(globalObject, static_cast<unsigned>(state.r));
    for (int64_t i = 0; i < state.r; ++i)
        result->initializeAt(vm, static_cast<unsigned>(i), pool->at(static_cast<unsigned>(state.indices[i])));
    state.result.set(vm, self, result);
    return result;
}

PYTHON_NATIVE(combinationsNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<CombinatoricState>(self);
    if (state.isStopped)
        STOP();
    if (!state.result)
        return JSValue::encode(firstResult(globalObject, self, state));
    PyTuple* pool = state.pool.get();
    auto& indices = state.indices;
    int64_t n = pool->length();
    int64_t r = state.r;
    PyTuple* result = nextResult(globalObject, self, state);
    // From the right, the first that is not as far on as it can go.
    int64_t i = r - 1;
    while (i >= 0 && indices[i] == i + n - r)
        --i;
    if (i < 0) {
        state.isStopped = true;
        STOP();
    }
    // It goes on by one, and each to the right of it is one more than the one before, which keeps them in order.
    ++indices[i];
    for (int64_t j = i + 1; j < r; ++j)
        indices[j] = indices[j - 1] + 1;
    for (; i < r; ++i)
        result->initializeAt(vm, static_cast<unsigned>(i), pool->at(static_cast<unsigned>(indices[i])));
    return JSValue::encode(result);
}

PYTHON_NATIVE(combinationsWithReplacementNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<CombinatoricState>(self);
    if (state.isStopped)
        STOP();
    if (!state.result)
        return JSValue::encode(firstResult(globalObject, self, state));
    PyTuple* pool = state.pool.get();
    auto& indices = state.indices;
    int64_t n = pool->length();
    int64_t r = state.r;
    PyTuple* result = nextResult(globalObject, self, state);
    int64_t i = r - 1;
    while (i >= 0 && indices[i] == n - 1)
        --i;
    if (i < 0) {
        state.isStopped = true;
        STOP();
    }
    // It goes on by one, and all to the right of it are the same.
    int64_t index = indices[i] + 1;
    JSValue element = pool->at(static_cast<unsigned>(index));
    for (; i < r; ++i) {
        indices[i] = index;
        result->initializeAt(vm, static_cast<unsigned>(i), element);
    }
    return JSValue::encode(result);
}

// permutations(iterable, r=None)
PYTHON_NATIVE(permutationsNew)
{
    NATIVE_PROLOGUE();
    PyTuple* pool = tupleFromIterable(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t n = pool->length();
    int64_t r = n;
    if (JSValue value = args.at(2); value && !isNone(value)) {
        if (!isInstance(globalObject, value, realm->typeInt()))
            return JSValue::encode(raiseTypeError(globalObject, scope, "Expected int as r"_s));
        auto given = toSsize(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        r = *given;
    }
    if (r < 0)
        return JSValue::encode(raiseValueError(globalObject, scope, "r must be non-negative"_s));
    auto* object = allocate<CombinatoricState>(vm, args[0]);
    auto& state = object->state<CombinatoricState>();
    if (!allocateIndices(globalObject, scope, state.indices, n) || !allocateIndices(globalObject, scope, state.cycles, r))
        return { };
    for (int64_t i = 0; i < n; ++i)
        state.indices[i] = i;
    for (int64_t i = 0; i < r; ++i)
        state.cycles[i] = n - i;
    state.pool.set(vm, object, pool);
    state.r = r;
    state.isStopped = r > n;
    return JSValue::encode(object);
}

PYTHON_NATIVE(permutationsNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<CombinatoricState>(self);
    if (state.isStopped)
        STOP();
    if (!state.result)
        return JSValue::encode(firstResult(globalObject, self, state));
    PyTuple* pool = state.pool.get();
    auto& indices = state.indices;
    auto& cycles = state.cycles;
    int64_t n = pool->length();
    int64_t r = state.r;
    if (!n) {
        state.isStopped = true;
        STOP();
    }
    PyTuple* result = nextResult(globalObject, self, state);
    // The rightmost has one less to go, and when one has none it comes round and it is the turn of the one to its left.
    int64_t i = r - 1;
    for (; i >= 0; --i) {
        if (!--cycles[i]) {
            // indices[i:] = indices[i+1:] + indices[i:i+1]
            int64_t index = indices[i];
            for (int64_t j = i; j < n - 1; ++j)
                indices[j] = indices[j + 1];
            indices[n - 1] = index;
            cycles[i] = n - i;
        } else {
            int64_t j = cycles[i];
            std::swap(indices[i], indices[n - j]);
            for (int64_t k = i; k < r; ++k)
                result->initializeAt(vm, static_cast<unsigned>(k), pool->at(static_cast<unsigned>(indices[k])));
            break;
        }
    }
    if (i < 0) {
        state.isStopped = true;
        STOP();
    }
    return JSValue::encode(result);
}

// ---- accumulate

namespace {
struct AccumulateState final : NativeState {
    PYTHON_NATIVE_STATE(AccumulateState);
    WriteBarrier<Unknown> total;
    WriteBarrier<Unknown> iterator;
    WriteBarrier<Unknown> function;
    WriteBarrier<Unknown> initial;
};
template<typename Visitor> void AccumulateState::visit(Visitor& visitor)
{
    visitor.append(total);
    visitor.append(iterator);
    visitor.append(function);
    visitor.append(initial);
}
}

// accumulate(iterable, func=None, *, initial=None)
PYTHON_NATIVE(accumulateNew)
{
    NATIVE_PROLOGUE();
    JSValue iterator = getIterator(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = allocate<AccumulateState>(vm, args[0]);
    auto& state = object->state<AccumulateState>();
    if (JSValue function = args.at(2); function && !isNone(function))
        state.function.set(vm, object, function);
    state.iterator.set(vm, object, iterator);
    JSValue initial = args.at(3);
    state.initial.set(vm, object, initial ? initial : jsUndefined());
    return JSValue::encode(object);
}

PYTHON_NATIVE(accumulateNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<AccumulateState>(self);
    if (!isNone(state.initial.get())) {
        state.total.set(vm, self, state.initial.get());
        state.initial.set(vm, self, jsUndefined());
        return JSValue::encode(state.total.get());
    }
    JSValue value = iteratorStep(globalObject, state.iterator.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        STOP();
    if (!state.total) {
        state.total.set(vm, self, value);
        return JSValue::encode(value);
    }
    JSValue total = state.function ? call(globalObject, state.function.get(), state.total.get(), value) : binaryOperation(globalObject, BinaryOperator::Add, false, state.total.get(), value);
    RETURN_IF_EXCEPTION(scope, { });
    state.total.set(vm, self, total);
    return JSValue::encode(total);
}

// ---- compress

namespace {
struct CompressState final : NativeState {
    PYTHON_NATIVE_STATE(CompressState);
    WriteBarrier<Unknown> data;
    WriteBarrier<Unknown> selectors;
};
template<typename Visitor> void CompressState::visit(Visitor& visitor)
{
    visitor.append(data);
    visitor.append(selectors);
}
}

// compress(data, selectors)
PYTHON_NATIVE(compressNew)
{
    NATIVE_PROLOGUE();
    JSValue data = getIterator(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue selectors = getIterator(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = allocate<CompressState>(vm, args[0]);
    auto& state = object->state<CompressState>();
    state.data.set(vm, object, data);
    state.selectors.set(vm, object, selectors);
    return JSValue::encode(object);
}

PYTHON_NATIVE(compressNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<CompressState>(args[0]);
    JSValue data = state.data.get();
    JSValue selectors = state.selectors.get();
    while (true) {
        // In this order, which is which of the two gets to raise something first.
        JSValue datum = iteratorStep(globalObject, data);
        RETURN_IF_EXCEPTION(scope, { });
        if (!datum)
            STOP();
        JSValue selector = iteratorStep(globalObject, selectors);
        RETURN_IF_EXCEPTION(scope, { });
        if (!selector)
            STOP();
        bool isSelected = isTrue(globalObject, selector);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSelected)
            return JSValue::encode(datum);
    }
}

// ---- count

namespace {
// While it is counting up by one from an int that fits, it is the number that is kept. Otherwise, and once that has got as far as it goes, `count` is the largest there is and it is done with objects, so that it can
// count in floats, in fractions, and past any size.
struct CountState final : NativeState {
    PYTHON_NATIVE_STATE(CountState);
    int64_t count { 0 };
    WriteBarrier<Unknown> longCount;
    WriteBarrier<Unknown> longStep;
};
template<typename Visitor> void CountState::visit(Visitor& visitor)
{
    visitor.append(longCount);
    visitor.append(longStep);
}
static constexpr int64_t largestCount = std::numeric_limits<int64_t>::max();
}

// count(start=0, step=1)
PYTHON_NATIVE(countNew)
{
    NATIVE_PROLOGUE();
    JSValue longCount = args.at(1);
    JSValue longStep = args.at(2);
    if ((longCount && !isNumber(globalObject, longCount)) || (longStep && !isNumber(globalObject, longStep)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "a number is required"_s));
    auto isInt = [&] (JSValue value) { return isInstance(globalObject, value, realm->typeInt()); };
    bool isFast = (!longCount || isInt(longCount)) && (!longStep || isInt(longStep));
    int64_t count = 0;
    if (!longCount)
        longCount = jsNumber(0);
    else if (isFast) {
        auto small = tryInt64(toInt(globalObject, longCount));
        RETURN_IF_EXCEPTION(scope, { });
        if (small)
            count = *small;
        else
            isFast = false;
    }
    if (!longStep)
        longStep = jsNumber(1);
    if (isFast) {
        auto step = tryInt64(toInt(globalObject, longStep));
        RETURN_IF_EXCEPTION(scope, { });
        if (!step || *step != 1)
            isFast = false;
    }
    auto* object = allocate<CountState>(vm, args[0]);
    auto& state = object->state<CountState>();
    state.count = isFast ? count : largestCount;
    if (!isFast)
        state.longCount.set(vm, object, longCount);
    state.longStep.set(vm, object, longStep);
    return JSValue::encode(object);
}

PYTHON_NATIVE(countNext)
{
    NATIVE_PROLOGUE();
    JSCell* self = args[0].asCell();
    auto& state = stateOf<CountState>(self);
    if (state.count != largestCount)
        return JSValue::encode(intFromInt64(globalObject, state.count++));
    // count_nextlong()
    JSValue longCount = state.longCount ? state.longCount.get() : intFromInt64(globalObject, largestCount);
    JSValue steppedUp = binaryOperation(globalObject, BinaryOperator::Add, false, longCount, state.longStep.get());
    RETURN_IF_EXCEPTION(scope, { });
    state.longCount.set(vm, self, steppedUp);
    return JSValue::encode(longCount);
}

PYTHON_NATIVE(countRepr)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<CountState>(args[0]);
    String name = typeOf(globalObject, args[0])->nameWithoutModule(globalObject);
    if (!state.longCount)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', state.count, ')'))));
    String count = repr(globalObject, state.longCount.get());
    RETURN_IF_EXCEPTION(scope, { });
    // A step is not shown if it is the int 1.
    if (isInstance(globalObject, state.longStep.get(), realm->typeInt())) {
        auto step = tryInt64(toInt(globalObject, state.longStep.get()));
        RETURN_IF_EXCEPTION(scope, { });
        if (step && *step == 1)
            RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', count, ')'))));
    }
    String step = repr(globalObject, state.longStep.get());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', count, ", "_s, step, ')'))));
}

// ---- repeat

namespace {
struct RepeatState final : NativeState {
    PYTHON_NATIVE_STATE(RepeatState);
    WriteBarrier<Unknown> element;
    int64_t count { -1 }; // How many more times. Less than nothing is for ever.
};
template<typename Visitor> void RepeatState::visit(Visitor& visitor) { visitor.append(element); }
}

// repeat(object [,times])
PYTHON_NATIVE(repeatNew)
{
    NATIVE_PROLOGUE();
    int64_t count = -1;
    JSValue times = args.at(2);
    if (times) {
        auto given = toSsize(globalObject, times);
        RETURN_IF_EXCEPTION(scope, { });
        count = std::max<int64_t>(*given, 0);
    }
    auto* object = allocate<RepeatState>(vm, args[0]);
    auto& state = object->state<RepeatState>();
    state.element.set(vm, object, args.at(1));
    state.count = count;
    return JSValue::encode(object);
}

PYTHON_NATIVE(repeatNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<RepeatState>(args[0]);
    if (!state.count)
        STOP();
    if (state.count > 0)
        --state.count;
    return JSValue::encode(state.element.get());
}

PYTHON_NATIVE(repeatRepr)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<RepeatState>(args[0]);
    String name = typeOf(globalObject, args[0])->nameWithoutModule(globalObject);
    String element = repr(globalObject, state.element.get());
    RETURN_IF_EXCEPTION(scope, { });
    if (state.count == -1)
        RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', element, ')'))));
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(name, '(', element, ", "_s, state.count, ')'))));
}

PYTHON_NATIVE(repeatLengthHint)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<RepeatState>(args[0]);
    if (state.count == -1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "len() of unsized object"_s));
    return JSValue::encode(intFromInt64(globalObject, state.count));
}

// ---- zip_longest

namespace {
struct ZipLongestState final : NativeState {
    PYTHON_NATIVE_STATE(ZipLongestState);
    WriteBarrier<PyTuple> iterators; // None in place of one that has run out.
    WriteBarrier<Unknown> fillValue;
    int64_t activeCount { 0 };
};
template<typename Visitor> void ZipLongestState::visit(Visitor& visitor)
{
    visitor.append(iterators);
    visitor.append(fillValue);
}
}

// zip_longest(*iterables, fillvalue=None)
PYTHON_NATIVE(zipLongestNew)
{
    NATIVE_PROLOGUE();
    JSValue fillValue = jsUndefined();
    if (args.keywordCount()) {
        fillValue = { };
        if (args.keywordCount() == 1) {
            String name = asString(args.keywordName(0))->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (name == "fillvalue"_s)
                fillValue = args.keywordValue(0);
        }
        if (!fillValue)
            return JSValue::encode(raiseTypeError(globalObject, scope, "zip_longest() got an unexpected keyword argument"_s));
    }
    MarkedArgumentBuffer iterators;
    for (unsigned i = 1; i < args.size(); ++i) {
        JSValue iterator = getIterator(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        iterators.append(iterator);
    }
    auto* object = allocate<ZipLongestState>(vm, args[0]);
    auto& state = object->state<ZipLongestState>();
    state.iterators.set(vm, object, PyTuple::createFromArguments(globalObject, iterators));
    state.fillValue.set(vm, object, fillValue);
    state.activeCount = iterators.size();
    return JSValue::encode(object);
}

PYTHON_NATIVE(zipLongestNext)
{
    NATIVE_PROLOGUE();
    auto& state = stateOf<ZipLongestState>(args[0]);
    PyTuple* iterators = state.iterators.get();
    unsigned size = iterators->length();
    if (!size || !state.activeCount)
        STOP();
    MarkedArgumentBuffer items;
    for (unsigned i = 0; i < size; ++i) {
        JSValue iterator = iterators->at(i);
        JSValue item = state.fillValue.get();
        // An iterator is never None, so that will do for there being none.
        if (!isNone(iterator)) {
            item = iteratorNext(globalObject, iterator);
            if (!item) {
                --state.activeCount;
                if (!state.activeCount || scope.exception()) {
                    state.activeCount = 0;
                    RETURN_IF_EXCEPTION(scope, { });
                    STOP();
                }
                item = state.fillValue.get();
                iterators->initializeAt(vm, i, jsUndefined());
            }
        }
        items.append(item);
    }
    return JSValue::encode(PyTuple::createFromArguments(globalObject, items));
}

// ---- The module

namespace {

ItertoolsState& initializeItertools(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<ItertoolsState>();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto create = [&] (WriteBarrier<PyType>& member, ASCIILiteral name, unsigned flags = PyType::IsBaseType) {
        PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, flags);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        member.set(vm, realm, type);
        if (&member != &state.tee)
            addGenericGetAttribute(globalObject, type);
        return type;
    };
    // What each has: how one is made, that it is what goes through itself, and what comes next.
    auto add = [&] (PyType* type, NativeFunction construct, Arguments arguments, NativeFunction next, unsigned data = 0, ASCIILiteral signature = { }) {
        addMethods(globalObject, type, {
            { "__new__"_s, construct, Kind::New, data, signature, arguments },
            { "__iter__"_s, itertoolsSelf },
            { "__next__"_s, next },
        });
    };
    add(create(state.accumulate, "itertools.accumulate"_s), accumulateNew, Arguments::AreThoseOfTheClass, accumulateNext);
    add(create(state.batched, "itertools.batched"_s), batchedNew, Arguments::AreThoseOfTheClass, batchedNext);
    PyType* chain = create(state.chain, "itertools.chain"_s);
    add(chain, chainNew, Arguments::AreNotChecked, chainNext);
    addClassGetItemIfGeneric(globalObject, chain);
    addMethods(globalObject, chain, {
        { "from_iterable"_s, chainFromIterable, Kind::ClassMethod },
    });
    PyType* combinations = create(state.combinations, "itertools.combinations"_s);
    add(combinations, combinationsNew, Arguments::AreThoseOfTheClass, combinationsNext, pack(false));
    addMethods(globalObject, combinations, { { "__sizeof__"_s, combinatoricSizeOf, Kind::Method, pack(Combinatoric::Combinations) } });
    add(create(state.compress, "itertools.compress"_s), compressNew, Arguments::AreThoseOfTheClass, compressNext);
    PyType* count = create(state.count, "itertools.count"_s);
    add(count, countNew, Arguments::AreThoseOfTheClass, countNext);
    addMethods(globalObject, count, { { "__repr__"_s, countRepr } });
    PyType* withReplacement = create(state.combinationsWithReplacement, "itertools.combinations_with_replacement"_s);
    add(withReplacement, combinationsNew, Arguments::AreThoseOfTheClass, combinationsWithReplacementNext, pack(true));
    addMethods(globalObject, withReplacement, { { "__sizeof__"_s, combinatoricSizeOf, Kind::Method, pack(Combinatoric::WithReplacement) } });
    add(create(state.cycle, "itertools.cycle"_s), cycleNew, Arguments::AreNotChecked, cycleNext);
    add(create(state.dropwhile, "itertools.dropwhile"_s), predicateNew, Arguments::AreNotChecked, dropWhileNext, pack(Predicate::DropWhile));
    add(create(state.filterfalse, "itertools.filterfalse"_s), predicateNew, Arguments::AreNotChecked, filterFalseNext, pack(Predicate::FilterFalse));
    add(create(state.groupby, "itertools.groupby"_s), groupByNew, Arguments::AreThoseOfTheClass, groupByNext);
    add(create(state.grouper, "itertools._grouper"_s, 0), grouperNew, Arguments::AreNotChecked, grouperNext);
    add(create(state.islice, "itertools.islice"_s), isliceNew, Arguments::AreNotChecked, isliceNext);
    add(create(state.pairwise, "itertools.pairwise"_s), pairwiseNew, Arguments::AreNotChecked, pairwiseNext);
    PyType* permutations = create(state.permutations, "itertools.permutations"_s);
    add(permutations, permutationsNew, Arguments::AreThoseOfTheClass, permutationsNext);
    addMethods(globalObject, permutations, { { "__sizeof__"_s, combinatoricSizeOf, Kind::Method, pack(Combinatoric::Permutations) } });
    PyType* product = create(state.product, "itertools.product"_s);
    add(product, productNew, Arguments::AreNotChecked, productNext);
    addMethods(globalObject, product, { { "__sizeof__"_s, combinatoricSizeOf, Kind::Method, pack(Combinatoric::Product) } });
    PyType* repeat = create(state.repeat, "itertools.repeat"_s);
    add(repeat, repeatNew, Arguments::AreThoseOfTheClass, repeatNext, 0, "(object, times=<unrepresentable>)"_s);
    addMethods(globalObject, repeat, {
        { "__repr__"_s, repeatRepr },
        { "__length_hint__"_s, repeatLengthHint },
    });
    add(create(state.starmap, "itertools.starmap"_s), predicateNew, Arguments::AreNotChecked, starMapNext, pack(Predicate::StarMap));
    add(create(state.takewhile, "itertools.takewhile"_s), predicateNew, Arguments::AreNotChecked, takeWhileNext, pack(Predicate::TakeWhile));
    PyType* tee = create(state.tee, "itertools._tee"_s, 0);
    add(tee, teeNew, Arguments::AreNotChecked, teeNext);
    addMethods(globalObject, tee, { { "__copy__"_s, teeCopy } });
    PyType* teeData = create(state.teeData, "itertools._tee_dataobject"_s, 0);
    addMethods(globalObject, teeData, { { "__new__"_s, teeDataNew, Kind::New, 0, { }, Arguments::AreNotChecked } });
    add(create(state.zipLongest, "itertools.zip_longest"_s), zipLongestNew, Arguments::AreNotChecked, zipLongestNext);
    return state;
}

} // anonymous namespace

JSObject* createItertoolsModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& state = itertoolsState(globalObject);
    JSObject* module = newBuiltinModule(globalObject, "itertools"_s);
    addFunction(globalObject, module, "tee"_s, itertoolsTee);
#define ADD(name) module->putDirect(vm, Identifier::fromString(vm, state.name->nameWithoutModule(globalObject)), state.name.get());
    FOR_EACH_ITERTOOLS_TYPE(ADD)
#undef ADD
    return module;
}

} } // namespace JSC::Python
