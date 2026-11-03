/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyException.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyDescriptors.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyMonitoring.h"
#include "PyStr.h"
#include "PyTypes.h"
#include <wtf/HashSet.h>

namespace JSC {
namespace Python {

// BaseExceptionGroup and ExceptionGroup, and what `except*` does with them.
// (Objects/exceptions.c and Python/ceval.c in CPython are the reference.)

// After ExceptionField::exceptionGroupMessage and exceptionGroupExceptions: the repr() of the sequence the group was made
// from, if that was neither a list nor a tuple. Empty otherwise.
static constexpr unsigned groupSequenceTextField = 2;

static bool isExceptionGroupInstance(JSGlobalObject* globalObject, JSValue value)
{
    auto* exception = PyException::tryFrom(value);
    return exception && exception->type()->isSubtypeOf(runtime(globalObject).BaseExceptionGroup());
}

static PyTuple* groupExceptions(JSGlobalObject* globalObject, PyException* group)
{
    JSValue exceptions = group->field(ExceptionField::exceptionGroupExceptions);
    return exceptions ? PyTuple::from(exceptions) : runtime(globalObject).emptyTuple();
}

static JSValue groupMessage(JSGlobalObject* globalObject, PyException* group)
{
    JSValue message = group->field(ExceptionField::exceptionGroupMessage);
    return message ? message : JSValue(runtime(globalObject).names.empty());
}

// Whether the object can be indexed with integers, as far as its type tells.
static bool groupIsSequence(JSGlobalObject* globalObject, JSValue value)
{
    if (isList(value) || PyTuple::is(value) || isStr(value))
        return true;
    if (PyDict::is(value))
        return false;
    PyType* type = typeOf(globalObject, value);
    JSValue method = type->lookup(Special::GetItem);
    if (!method || isNone(method))
        return false;
    return type->isHeapType() || !type->has(TypeFlag::Mapping);
}

static bool groupIsExceptionClass(JSValue value)
{
    PyType* type = PyType::tryFrom(value);
    return type && type->has(TypeFlag::ExceptionSubclass);
}

// BaseExceptionGroup(message, exceptions)
static JSValue groupMake(JSGlobalObject* globalObject, JSValue message, JSValue exceptions)
{
    return call(globalObject, runtime(globalObject).BaseExceptionGroup(), message, exceptions);
}

// ---- The methods ----

static JSValue groupNew(JSGlobalObject* globalObject, JSValue clsValue, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (args.size() != 2)
        return raiseTypeError(globalObject, scope, "BaseExceptionGroup.__new__() takes exactly 2 arguments ("_s, args.size(), " given)"_s);
    JSValue message = args[0];
    JSValue given = args[1];
    if (!isStr(message))
        return raiseTypeError(globalObject, scope, "BaseExceptionGroup.__new__() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, message));
    if (!groupIsSequence(globalObject, given))
        return raiseTypeError(globalObject, scope, "second argument (exceptions) must be a sequence"_s);

    // The sequence may change later, so what it looks like is noted now.
    JSValue sequenceText;
    if (!isList(given) && !PyTuple::is(given)) {
        sequenceText = repr(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyTuple* exceptions = tupleFromIterable(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (!exceptions->length())
        return raiseValueError(globalObject, scope, "second argument (exceptions) must be a non-empty sequence"_s);

    bool nestsBaseExceptions = false;
    for (unsigned i = 0; i < exceptions->length(); ++i) {
        JSValue exception = exceptions->at(i);
        if (!PyException::is(exception))
            return raiseValueError(globalObject, scope, "Item "_s, i, " of second argument (exceptions) is not an exception"_s);
        bool isOrdinary = isInstance(globalObject, exception, rt.Exception());
        RETURN_IF_EXCEPTION(scope, { });
        if (!isOrdinary)
            nestsBaseExceptions = true;
    }

    PyType* cls = PyType::from(clsValue);
    if (cls == rt.ExceptionGroup()) {
        if (nestsBaseExceptions)
            return raiseTypeError(globalObject, scope, "Cannot nest BaseExceptions in an ExceptionGroup"_s);
    } else if (cls == rt.BaseExceptionGroup()) {
        // A group of nothing but Exceptions is an Exception.
        if (!nestsBaseExceptions)
            cls = rt.ExceptionGroup();
    } else if (nestsBaseExceptions) {
        bool isOrdinary = isSubclass(globalObject, cls, rt.Exception());
        RETURN_IF_EXCEPTION(scope, { });
        if (isOrdinary)
            return raiseTypeError(globalObject, scope, "Cannot nest BaseExceptions in '"_s, cls->name(), '\'');
    }

    PyException* group = PyException::create(vm, rt.structure<PyException>(), cls);
    group->setArgs(vm, PyTuple::create(globalObject, args.positional()));
    group->setField(vm, ExceptionField::exceptionGroupMessage, message);
    group->setField(vm, ExceptionField::exceptionGroupExceptions, exceptions);
    group->setField(vm, groupSequenceTextField, sequenceText);
    return group;
}

static JSValue groupInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.hasKeywords())
        return raiseTypeError(globalObject, scope, typeName(globalObject, self), "() takes no keyword arguments"_s);
    PyException::from(self)->setArgs(vm, PyTuple::create(globalObject, args.positional()));
    return jsUndefined();
}

static JSValue groupStr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* group = PyException::from(self);
    JSString* message = str(globalObject, groupMessage(globalObject, group));
    RETURN_IF_EXCEPTION(scope, { });
    String text = message->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned count = groupExceptions(globalObject, group)->length();
    return makeStr(vm, makeString(text, " ("_s, count, " sub-exception"_s, count > 1 ? "s"_s : ""_s, ')'));
}

static JSValue groupRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* group = PyException::from(self);

    JSString* sequenceText;
    if (JSValue saved = group->field(groupSequenceTextField))
        sequenceText = asString(saved);
    else {
        // What the group holds, made to look like what it was given.
        JSValue exceptions = groupExceptions(globalObject, group);
        PyTuple* args = group->args();
        if (args->length() == 2 && isList(args->at(1))) {
            exceptions = listFromIterable(globalObject, exceptions);
            RETURN_IF_EXCEPTION(scope, { });
        }
        sequenceText = repr(globalObject, exceptions);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSString* messageText = repr(globalObject, groupMessage(globalObject, group));
    RETURN_IF_EXCEPTION(scope, { });
    String message = messageText->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String sequence = sequenceText->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(group->type()->name(), '(', message, ", "_s, sequence, ')'));
}

static JSValue groupDerive(JSGlobalObject* globalObject, JSValue self, JSValue exceptions)
{
    return groupMake(globalObject, groupMessage(globalObject, PyException::from(self)), exceptions);
}

// A group like `original` that holds the exceptions in the list. Empty, with no exception, if the list is.
static JSValue groupSubset(JSGlobalObject* globalObject, PyException* original, JSArray* exceptions)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!listLength(exceptions))
        return { };

    JSValue derived = callMethod(globalObject, original, internString(vm, "derive"_s), exceptions);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isExceptionGroupInstance(globalObject, derived))
        return raiseTypeError(globalObject, scope, "derive must return an instance of BaseExceptionGroup"_s);

    PyException* group = PyException::from(derived);
    if (JSValue traceback = original->traceback())
        group->setTraceback(vm, traceback);
    group->setContext(vm, original->context());
    group->setCause(vm, original->cause());
    group->setSuppressContext(true);

    JSValue notes = getAttrIfPresent(globalObject, original, rt.names.dNotes());
    RETURN_IF_EXCEPTION(scope, { });
    // This is no place to complain of notes that are not a list.
    if (notes && groupIsSequence(globalObject, notes)) {
        JSArray* copy = listFromIterable(globalObject, notes);
        RETURN_IF_EXCEPTION(scope, { });
        setAttr(globalObject, group, rt.names.dNotes(), copy);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return group;
}

namespace {

struct GroupMatcher {
    enum class Kind : uint8_t {
        Type, // value: an exception class, or a tuple of them
        Predicate, // value: a function that is true of the exceptions that match
        Leaves, // leaves: the exceptions, groups aside, that match
    };
    Kind kind;
    JSValue value;
    const UncheckedKeyHashSet<JSCell*>* leaves { nullptr };
};

}

// For the argument of split() and subgroup(). False: a TypeError.
static bool groupMatcherFor(JSGlobalObject* globalObject, JSValue value, GroupMatcher& matcher)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    matcher.value = value;
    if (!PyType::is(value) && isCallable(globalObject, value)) {
        matcher.kind = GroupMatcher::Kind::Predicate;
        return true;
    }
    matcher.kind = GroupMatcher::Kind::Type;
    if (groupIsExceptionClass(value))
        return true;
    if (auto* tuple = PyTuple::tryFrom(value); tuple && tuple->type() == runtime(globalObject).tupleType()) {
        bool allAreClasses = true;
        for (unsigned i = 0; i < tuple->length(); ++i)
            allAreClasses &= groupIsExceptionClass(tuple->at(i));
        if (allAreClasses)
            return true;
    }
    raiseTypeError(globalObject, scope, "expected an exception type, a tuple of exception types, or a callable (other than a class)"_s);
    return false;
}

static bool groupMatches(JSGlobalObject* globalObject, JSValue exception, const GroupMatcher& matcher)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (matcher.kind) {
    case GroupMatcher::Kind::Type:
        RELEASE_AND_RETURN(scope, exceptionMatches(globalObject, exception, matcher.value));
    case GroupMatcher::Kind::Predicate: {
        JSValue result = call(globalObject, matcher.value, exception);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    }
    case GroupMatcher::Kind::Leaves:
        RELEASE_AND_RETURN(scope, !isExceptionGroupInstance(globalObject, exception) && matcher.leaves->contains(exception.asCell()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Divides the exception in what matches and, if `wantsRest`, what does not. Either comes out empty if there is no such part.
// False: an exception.
static bool groupSplitRecursive(JSGlobalObject* globalObject, JSValue exception, const GroupMatcher& matcher, bool wantsRest, JSValue& match, JSValue& rest)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    match = JSValue();
    rest = JSValue();

    bool matches = groupMatches(globalObject, exception, matcher);
    RETURN_IF_EXCEPTION(scope, false);
    if (matches) {
        match = exception;
        return true;
    }
    if (!isExceptionGroupInstance(globalObject, exception)) {
        if (wantsRest)
            rest = exception;
        return true;
    }

    // Some of it may match.
    PyException* group = PyException::from(exception);
    PyTuple* exceptions = groupExceptions(globalObject, group);
    JSArray* matchList = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    JSArray* restList = nullptr;
    if (wantsRest) {
        restList = newList(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    for (unsigned i = 0; i < exceptions->length(); ++i) {
        if (!vm.isSafeToRecurseSoft()) {
            raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded in exceptiongroup_split_recursive"_s);
            return false;
        }
        JSValue itemMatch;
        JSValue itemRest;
        groupSplitRecursive(globalObject, exceptions->at(i), matcher, wantsRest, itemMatch, itemRest);
        RETURN_IF_EXCEPTION(scope, false);
        if (itemMatch) {
            listAppend(globalObject, matchList, itemMatch);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (itemRest) {
            listAppend(globalObject, restList, itemRest);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }

    JSValue matchGroup = groupSubset(globalObject, group, matchList);
    RETURN_IF_EXCEPTION(scope, false);
    if (wantsRest) {
        rest = groupSubset(globalObject, group, restList);
        RETURN_IF_EXCEPTION(scope, false);
    }
    match = matchGroup;
    return true;
}

static JSValue groupSplit(JSGlobalObject* globalObject, JSValue self, JSValue matcherValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    GroupMatcher matcher;
    if (failed(scope, groupMatcherFor(globalObject, matcherValue, matcher)))
        return { };
    JSValue match;
    JSValue rest;
    if (failed(scope, groupSplitRecursive(globalObject, self, matcher, true, match, rest)))
        return { };
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { match ? match : jsUndefined(), rest ? rest : jsUndefined() }));
}

static JSValue groupSubgroup(JSGlobalObject* globalObject, JSValue self, JSValue matcherValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    GroupMatcher matcher;
    if (failed(scope, groupMatcherFor(globalObject, matcherValue, matcher)))
        return { };
    JSValue match;
    JSValue rest;
    if (failed(scope, groupSplitRecursive(globalObject, self, matcher, false, match, rest)))
        return { };
    return match ? match : jsUndefined();
}

static JSValue groupGetMessage(JSGlobalObject* globalObject, JSValue self) { return groupMessage(globalObject, PyException::from(self)); }
static JSValue groupGetExceptions(JSGlobalObject* globalObject, JSValue self) { return groupExceptions(globalObject, PyException::from(self)); }

static constexpr NativeDef groupMethods[] = {
    PY_NEW(groupNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", groupInit, "($self, *args, **kwargs)"),
    PY_SLOT("__str__", groupStr, "($self, /)"),
    PY_SLOT("__repr__", groupRepr, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
    PY_METHOD("derive", groupDerive, "($self, excs, /)"),
    PY_METHOD("split", groupSplit, "($self, matcher_value, /)"),
    PY_METHOD("subgroup", groupSubgroup, "($self, matcher_value, /)"),
};

static constexpr NativeGetSet groupGetSets[] = {
    PY_MEMBER("message", groupGetMessage),
    PY_MEMBER("exceptions", groupGetExceptions),
};

// ---- except* ----

// What follows `except*` must be exception classes, none of them a class of groups. False: a TypeError.
static bool groupCheckExceptStarType(JSGlobalObject* globalObject, JSValue type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue single[] = { type };
    std::span<const JSValue> classes = single;
    if (auto* tuple = PyTuple::tryFrom(type))
        classes = tuple->span();
    for (JSValue cls : classes) {
        if (!groupIsExceptionClass(cls)) {
            raiseTypeError(globalObject, scope, "catching classes that do not inherit from BaseException is not allowed"_s);
            return false;
        }
    }
    for (JSValue cls : classes) {
        bool isGroupClass = isSubclass(globalObject, cls, rt.BaseExceptionGroup());
        RETURN_IF_EXCEPTION(scope, false);
        if (isGroupClass) {
            raiseTypeError(globalObject, scope, "catching ExceptionGroup with except* is not allowed. Use except instead."_s);
            return false;
        }
    }
    ensureStillAliveHere(type); // `classes` may point into it.
    return true;
}

JSValue matchExceptionGroup(JSGlobalObject* globalObject, JSValue exception, JSValue type, JSValue& rest)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    rest = jsUndefined();
    if (failed(scope, groupCheckExceptStarType(globalObject, type)))
        return { };
    // An earlier clause caught everything.
    if (isNone(exception))
        return jsUndefined();

    bool matches = exceptionMatches(globalObject, exception, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (matches) {
        if (isExceptionGroupInstance(globalObject, exception))
            return exception;
        // The clause always sees a group.
        JSValue wrapped = groupMake(globalObject, rt.names.empty(), PyTuple::create(globalObject, { exception }));
        RETURN_IF_EXCEPTION(scope, { });
        if (PyFrame* frame = currentFrame(globalObject))
            PyException::from(wrapped)->setTraceback(vm, PyTraceback::create(globalObject, { }, frame, frame->lineNumber(), shownOffset(frame->bytecodeOffset())));
        return wrapped;
    }

    if (!isExceptionGroupInstance(globalObject, exception)) {
        rest = exception;
        return jsUndefined();
    }
    JSValue pair = callMethod(globalObject, exception, internString(vm, "split"_s), type);
    RETURN_IF_EXCEPTION(scope, { });
    auto* tuple = PyTuple::tryFrom(pair);
    if (!tuple || tuple->type() != rt.tupleType())
        return raiseTypeError(globalObject, scope, typeName(globalObject, exception), ".split must return a tuple, not "_s, typeName(globalObject, pair));
    if (tuple->length() < 2)
        return raiseTypeError(globalObject, scope, typeName(globalObject, exception), ".split must return a 2-tuple, got tuple of size "_s, tuple->length());
    rest = tuple->at(1);
    return tuple->at(0);
}

// False: a RecursionError.
static bool groupCollectLeaves(JSGlobalObject* globalObject, JSValue exception, UncheckedKeyHashSet<JSCell*>& leaves)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(exception))
        return true;
    if (!isExceptionGroupInstance(globalObject, exception)) {
        leaves.add(exception.asCell());
        return true;
    }
    PyTuple* exceptions = groupExceptions(globalObject, PyException::from(exception));
    for (unsigned i = 0; i < exceptions->length(); ++i) {
        if (!vm.isSafeToRecurseSoft()) {
            raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded in collect_exception_group_leaf_ids"_s);
            return false;
        }
        if (failed(scope, groupCollectLeaves(globalObject, exceptions->at(i), leaves)))
            return false;
    }
    return true;
}

// The part of the group made of the leaves that are in any of the groups in the list, or None.
static JSValue groupProjection(JSGlobalObject* globalObject, JSValue group, JSArray* keep)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UncheckedKeyHashSet<JSCell*> leaves;
    for (unsigned i = 0; i < listLength(keep); ++i) {
        if (failed(scope, groupCollectLeaves(globalObject, listGet(keep, i), leaves)))
            return { };
    }
    GroupMatcher matcher { GroupMatcher::Kind::Leaves, { }, &leaves };
    JSValue match;
    JSValue rest;
    if (failed(scope, groupSplitRecursive(globalObject, group, matcher, false, match, rest)))
        return { };
    return match ? match : jsUndefined();
}

JSValue prepareReraiseStar(JSGlobalObject* globalObject, JSValue originalValue, JSValue raisedValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* all = asList(raisedValue);
    unsigned count = listLength(all);
    if (!count)
        return jsUndefined();
    // An exception that was not a group was wrapped, and at most one clause ran.
    if (!isExceptionGroupInstance(globalObject, originalValue))
        return listGet(all, 0);

    // What was raised again is a part of the original, and has all but its exceptions in common with it.
    PyException* original = PyException::from(originalValue);
    auto isPartOfOriginal = [&](PyException* exception) {
        return exception->traceback() == original->traceback() && exception->cause() == original->cause() && exception->context() == original->context();
    };
    JSArray* raised = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* reraised = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < count; ++i) {
        JSValue exception = listGet(all, i);
        if (isNone(exception))
            continue;
        listAppend(globalObject, isPartOfOriginal(PyException::from(exception)) ? reraised : raised, exception);
        RETURN_IF_EXCEPTION(scope, { });
    }

    // That keeps the structure it had in the original. What is new goes beside it, in a new group.
    JSValue reraisedGroup = groupProjection(globalObject, originalValue, reraised);
    RETURN_IF_EXCEPTION(scope, { });
    if (!listLength(raised))
        return reraisedGroup;
    if (!isNone(reraisedGroup)) {
        listAppend(globalObject, raised, reraisedGroup);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (listLength(raised) == 1)
        return listGet(raised, 0);
    RELEASE_AND_RETURN(scope, groupMake(globalObject, rt.names.empty(), raised));
}

// ---- The types ----

void initializeExceptionGroupTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();

    // Both have been made as every other exception type is, with one base and nothing of their own.
    PyType* base = rt.BaseExceptionGroup();
    base->add(TypeFlag::OwnLayout);
    base->setSolidBase(vm, base);
    base->dict()->set(vm, rt.names.dDoc(), makeStr(vm, "A combination of multiple unrelated exceptions."_s));
    base->addMethods(globalObject, groupMethods);
    base->addGetSets(globalObject, groupGetSets);

    PyType* group = rt.ExceptionGroup();
    group->setBases(vm, PyTuple::create(globalObject, { JSValue(base), JSValue(rt.Exception()) }));
    group->setMRO(vm, PyTuple::create(globalObject, { JSValue(group), JSValue(base), JSValue(rt.Exception()), JSValue(rt.BaseException()), JSValue(rt.objectType()) }));
    group->setSolidBase(vm, base);
    // CPython makes it by calling type().
    group->add(TypeFlag::Mutable);
    group->add(TypeFlag::HasWeakref);
    group->dict()->set(vm, rt.names.dModule(), rt.names.builtins());
    // So its __weakref__ is the descriptor that all classes made that way share.
    JSValue weakrefDescriptor = rt.slot(Runtime::Slot::InstanceWeakrefDescriptor);
    if (!weakrefDescriptor) {
        weakrefDescriptor = PyGetSetDescriptor::create(globalObject, weakrefGetSet()[0], rt.objectType());
        rt.setSlot(Runtime::Slot::InstanceWeakrefDescriptor, weakrefDescriptor);
    }
    group->dict()->set(vm, rt.names.dWeakref(), weakrefDescriptor);
    rt.Exception()->addSubclass(globalObject, group);
    group->didChange(vm);
}

} // namespace Python
} // namespace JSC
