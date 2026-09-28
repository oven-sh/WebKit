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

#include "PyFrame.h"
#include "TopExceptionScope.h"
#include "PythonSequences.h"

// BaseExceptionGroup and ExceptionGroup, and what `except*` is compiled into calls of. It is Objects/exceptions.c of CPython, function for function, and for `except*`
// _PyEval_ExceptionGroupMatch() and _PyEval_CheckExceptStarTypeValid() of Python/ceval.c.

namespace JSC { namespace Python {

static bool isGroup(JSGlobalObject* globalObject, JSValue value)
{
    return isInstance(globalObject, value, globalObject->pyRealm()->typeBaseExceptionGroup());
}

static bool isExceptionClass(JSValue value) { return isClass(value) && asType(value)->isExceptionType(); }
static bool isExceptionInstance(JSGlobalObject* globalObject, JSValue value) { return typeOf(globalObject, value)->isExceptionType(); }

static PyTuple* exceptionsOf(VM& vm, JSValue group)
{
    return uncheckedDowncast<PyTuple>(asObject(group)->getDirect(vm, vm.pythonNames().private_groupExceptions).asCell());
}

// PySequence_Check(): whether it can be indexed as a sequence is, which is to say by a number. What is written in Python and has __getitem__ may be either, and counts unless it is a dict.
bool isSequence(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    PyType* type = typeOf(globalObject, value);
    if (type->isSubtypeOf(realm->typeDict()))
        return false;
    for (auto& entry : type->mro()->span()) {
        PyType* base = asType(entry.get());
        if (!base->getDirect(vm, vm.pythonNames().dunder_getitem))
            continue;
        return base->hasFlag(PyType::IsHeapType) || base->hasFlag(PyType::IsSequence) || base == realm->typeStr() || base == realm->typeBytes() || base == realm->typeByteArray();
    }
    return false;
}

// BaseExceptionGroup_new()
PYTHON_NATIVE(groupNew)
{
    NATIVE_PROLOGUE();
    PyType* givenClass = asType(args[0]);
    if (args.size() != 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("BaseExceptionGroup.__new__() takes exactly 2 arguments ("_s, args.size() - 1, " given)"_s)));
    JSValue message = args[1];
    JSValue given = args[2];
    if (!isInstance(globalObject, message, realm->typeStr()))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("BaseExceptionGroup.__new__() argument 1 must be str, not "_s, isNone(message) ? String("None"_s) : typeName(globalObject, message))));
    if (!isSequence(globalObject, given))
        return JSValue::encode(raiseTypeError(globalObject, scope, "second argument (exceptions) must be a sequence"_s));

    // What it looked like, in case it is something that changes.
    JSValue exceptionsRepr;
    if (!isList(given) && !isTuple(given)) {
        String text = repr(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        exceptionsRepr = jsString(vm, text);
    }
    PyTuple* exceptions = tupleFromIterable(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (!exceptions->length())
        return JSValue::encode(raiseValueError(globalObject, scope, "second argument (exceptions) must be a non-empty sequence"_s));

    bool nestsBaseExceptions = false;
    for (unsigned i = 0; i < exceptions->length(); ++i) {
        JSValue exception = exceptions->at(i);
        if (!isExceptionInstance(globalObject, exception))
            return JSValue::encode(raiseValueError(globalObject, scope, makeString("Item "_s, i, " of second argument (exceptions) is not an exception"_s)));
        nestsBaseExceptions |= !isInstance(globalObject, exception, realm->typeException());
    }

    PyType* type = givenClass;
    if (type == realm->exceptionGroupType()) {
        if (nestsBaseExceptions)
            return JSValue::encode(raiseTypeError(globalObject, scope, "Cannot nest BaseExceptions in an ExceptionGroup"_s));
    } else if (type == realm->typeBaseExceptionGroup()) {
        // If all that is in it is an Exception, so is it.
        if (!nestsBaseExceptions)
            type = realm->exceptionGroupType();
    } else if (nestsBaseExceptions && type->isSubtypeOf(realm->typeException()))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Cannot nest BaseExceptions in '"_s, type->name(), '\'')));

    PyException* self = PyException::create(vm, type);
    self->putDirect(vm, names.private_args, PyTuple::create(globalObject, { message, given }));
    self->putDirect(vm, names.private_groupMessage, message);
    self->putDirect(vm, names.private_groupExceptions, exceptions);
    if (exceptionsRepr)
        self->putDirect(vm, names.private_groupExceptionsRepr, exceptionsRepr);
    return JSValue::encode(self);
}

// _PyExc_CreateExceptionGroup()
static JSValue createGroup(JSGlobalObject* globalObject, const String& message, JSValue exceptions)
{
    return call(globalObject, globalObject->pyRealm()->typeBaseExceptionGroup(), jsString(globalObject->vm(), message), exceptions);
}

// BaseExceptionGroup_str()
PYTHON_NATIVE(groupStr)
{
    NATIVE_PROLOGUE();
    String message = str(globalObject, asObject(args[0])->getDirect(vm, names.private_groupMessage));
    RETURN_IF_EXCEPTION(scope, { });
    unsigned count = exceptionsOf(vm, args[0])->length();
    return JSValue::encode(jsString(vm, makeString(message, " ("_s, count, " sub-exception"_s, count > 1 ? "s"_s : ""_s, ')')));
}

// BaseExceptionGroup_repr()
PYTHON_NATIVE(groupRepr)
{
    NATIVE_PROLOGUE();
    JSObject* self = asObject(args[0]);
    String exceptions;
    if (JSValue saved = self->getDirect(vm, names.private_groupExceptionsRepr))
        exceptions = asString(saved)->value(globalObject);
    else {
        // What is in it, made to look like what it was given. That can have changed since, or been replaced.
        JSValue arguments = self->getDirect(vm, names.private_args);
        bool wasList = arguments && isTuple(arguments) && asTuple(arguments)->length() > 1 && isList(asTuple(arguments)->at(1));
        JSValue shown = exceptionsOf(vm, self);
        if (wasList) {
            shown = listFromIterable(globalObject, shown);
            RETURN_IF_EXCEPTION(scope, { });
        }
        exceptions = repr(globalObject, shown);
    }
    RETURN_IF_EXCEPTION(scope, { });
    String message = repr(globalObject, self->getDirect(vm, names.private_groupMessage));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, makeString(typeOf(globalObject, self)->name(), '(', message, ", "_s, exceptions, ')')));
}

// BaseExceptionGroup.derive()
PYTHON_NATIVE(groupDerive)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, realm->typeBaseExceptionGroup(), asObject(args[0])->getDirect(vm, names.private_groupMessage), args[1])));
}

// exceptiongroup_subset(): a group like `original` with only these in it. Empty if there are none, and if it raises.
static JSValue subset(JSGlobalObject* globalObject, JSObject* original, JSArray* exceptions)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    if (!exceptions->length())
        return { };
    JSValue derive = getAttribute(globalObject, original, Identifier::fromString(vm, "derive"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = call(globalObject, derive, exceptions);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isGroup(globalObject, result))
        return raiseTypeError(globalObject, scope, "derive must return an instance of BaseExceptionGroup"_s);

    JSObject* group = asObject(result);
    for (const Identifier* field : { &names.private_traceback, &names.private_context, &names.private_cause }) {
        if (JSValue value = original->getDirect(vm, *field); value && !isNone(value))
            group->putDirect(vm, *field, value);
        else if (field != &names.private_traceback)
            group->deleteProperty(globalObject, *field);
    }
    // As PyException_SetCause() does, whatever the cause.
    group->putDirect(vm, names.private_suppressContext, jsBoolean(true));

    JSValue notes = getAttributeIfPresent(globalObject, original, names.dunder_notes);
    RETURN_IF_EXCEPTION(scope, { });
    // Each part has a list of its own. What is not a sequence should not have been there, and this is no place to say so.
    if (notes && isSequence(globalObject, notes)) {
        JSArray* copy = listFromIterable(globalObject, notes);
        RETURN_IF_EXCEPTION(scope, { });
        setAttribute(globalObject, group, names.dunder_notes, copy);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return group;
}

namespace {

struct Matcher {
    enum class Kind : uint8_t {
        Type, // A class of exception, or a tuple of them.
        Predicate, // What is called with an exception says whether it matches.
        Instances, // Those that are no groups and are among these. It is how what was raised again is put back where it was.
    };
    Kind kind;
    JSValue value;
    const UncheckedKeyHashSet<JSCell*>* instances { nullptr };
};

struct SplitResult {
    JSValue match;
    JSValue rest;
};

} // anonymous namespace

// get_matcher_type()
static std::optional<Matcher> matcherFor(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isCallable(globalObject, value) && !isClass(value))
        return Matcher { Matcher::Kind::Predicate, value };
    if (isExceptionClass(value))
        return Matcher { Matcher::Kind::Type, value };
    if (isTuple(value) && typeOf(globalObject, value) == globalObject->pyRealm()->typeTuple()
        && std::ranges::all_of(asTuple(value)->span(), [] (auto& item) { return isExceptionClass(item.get()); }))
        return Matcher { Matcher::Kind::Type, value };
    raiseTypeError(globalObject, scope, "expected an exception type, a tuple of exception types, or a callable (other than a class)"_s);
    return std::nullopt;
}

// PyErr_GivenExceptionMatches(), of an exception and what has been seen to be a class of them or a tuple of those
static bool givenExceptionMatches(JSGlobalObject* globalObject, JSValue exception, JSValue pattern)
{
    if (!isTuple(pattern))
        return isInstance(globalObject, exception, asType(pattern));
    return std::ranges::any_of(asTuple(pattern)->span(), [&] (auto& item) { return givenExceptionMatches(globalObject, exception, item.get()); });
}

// exceptiongroup_split_check_match()
static bool matches(JSGlobalObject* globalObject, JSValue exception, const Matcher& matcher)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    switch (matcher.kind) {
    case Matcher::Kind::Type:
        return givenExceptionMatches(globalObject, exception, matcher.value);
    case Matcher::Kind::Predicate: {
        JSValue result = call(globalObject, matcher.value, exception);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    }
    case Matcher::Kind::Instances:
        return !isGroup(globalObject, exception) && matcher.instances->contains(exception.asCell());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// exceptiongroup_split_recursive(). The rest is only worked out if it is wanted.
static SplitResult split(JSGlobalObject* globalObject, JSValue exception, const Matcher& matcher, bool constructRest)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isMatch = matches(globalObject, exception, matcher);
    RETURN_IF_EXCEPTION(scope, { });
    if (isMatch)
        return { exception, { } };
    if (!isGroup(globalObject, exception))
        return { { }, constructRest ? exception : JSValue() };

    // Some of it may.
    PyTuple* exceptions = exceptionsOf(vm, exception);
    JSArray* matchList = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* restList = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < exceptions->length(); ++i) {
        if (!vm.isSafeToRecurse()) [[unlikely]] {
            raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded in exceptiongroup_split_recursive"_s);
            return { };
        }
        SplitResult result = split(globalObject, exceptions->at(i), matcher, constructRest);
        RETURN_IF_EXCEPTION(scope, { });
        if (result.match) {
            matchList->push(globalObject, result.match);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (result.rest) {
            restList->push(globalObject, result.rest);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    SplitResult result;
    result.match = subset(globalObject, asObject(exception), matchList);
    RETURN_IF_EXCEPTION(scope, { });
    if (constructRest) {
        result.rest = subset(globalObject, asObject(exception), restList);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static JSValue orNone(JSValue value) { return value ? value : jsUndefined(); }

// BaseExceptionGroup.split()
PYTHON_NATIVE(groupSplit)
{
    NATIVE_PROLOGUE();
    auto matcher = matcherFor(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    SplitResult result = split(globalObject, args[0], *matcher, true);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { orNone(result.match), orNone(result.rest) }));
}

// BaseExceptionGroup.subgroup()
PYTHON_NATIVE(groupSubgroup)
{
    NATIVE_PROLOGUE();
    auto matcher = matcherFor(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    SplitResult result = split(globalObject, args[0], *matcher, false);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(orNone(result.match));
}

// collect_exception_group_leaf_ids()
static bool collectLeaves(JSGlobalObject* globalObject, JSValue exception, UncheckedKeyHashSet<JSCell*>& leaves)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(exception))
        return true;
    if (!isGroup(globalObject, exception)) {
        leaves.add(exception.asCell());
        return true;
    }
    for (auto& item : exceptionsOf(vm, exception)->span()) {
        if (!vm.isSafeToRecurse()) [[unlikely]] {
            raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded in collect_exception_group_leaf_ids"_s);
            return false;
        }
        bool succeeded = collectLeaves(globalObject, item.get(), leaves);
        RETURN_IF_EXCEPTION(scope, false);
        ASSERT_UNUSED(succeeded, succeeded);
    }
    return true;
}

// is_same_exception_metadata(). It looks at a field for the notes too, which nothing ever sets: they are an attribute.
static bool hasSameMetadata(VM& vm, JSValue first, JSValue second)
{
    auto& names = vm.pythonNames();
    for (const Identifier* field : { &names.private_traceback, &names.private_cause, &names.private_context }) {
        if (orNone(asObject(first)->getDirect(vm, *field)) != orNone(asObject(second)->getDirect(vm, *field)))
            return false;
    }
    return true;
}

// _PyExc_PrepReraiseStar(): what to raise when a `try` with `except*` clauses is done, or None. `original` is what was caught, and `exceptions` a list of what the clauses raised, or raised
// again, and of what none of them handled.
JSValue prepareReraiseStar(JSGlobalObject* globalObject, JSValue original, JSArray* exceptions)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!exceptions->length())
        return jsUndefined();
    // It was wrapped for the one clause that can have handled it.
    if (!isGroup(globalObject, original))
        return exceptions->getIndexQuickly(0);

    JSArray* raised = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, { });
    // exception_group_projection(): what was raised again keeps the place that it had.
    UncheckedKeyHashSet<JSCell*> leaves;
    for (unsigned i = 0; i < exceptions->length(); ++i) {
        JSValue exception = exceptions->getIndexQuickly(i);
        if (isNone(exception))
            continue;
        if (hasSameMetadata(vm, exception, original))
            collectLeaves(globalObject, exception, leaves);
        else
            raised->push(globalObject, exception);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue raisedAgain = split(globalObject, original, Matcher { Matcher::Kind::Instances, { }, &leaves }, false).match;
    RETURN_IF_EXCEPTION(scope, { });

    if (!raised->length())
        return orNone(raisedAgain);
    if (raisedAgain) {
        raised->push(globalObject, raisedAgain);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (raised->length() > 1)
        RELEASE_AND_RETURN(scope, createGroup(globalObject, emptyString(), raised));
    return raised->getIndexQuickly(0);
}

// _PyEval_CheckExceptStarTypeValid()
static bool checkStarType(JSGlobalObject* globalObject, JSValue pattern)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyType* group = globalObject->pyRealm()->typeBaseExceptionGroup();
    auto each = [&] (const auto& function) {
        if (!isTuple(pattern))
            return function(pattern);
        return std::ranges::all_of(asTuple(pattern)->span(), [&] (auto& item) { return function(item.get()); });
    };
    if (!each(isExceptionClass)) {
        raiseTypeError(globalObject, scope, "catching classes that do not inherit from BaseException is not allowed"_s);
        return false;
    }
    if (!each([&] (JSValue item) { return !asType(item)->isSubtypeOf(group); })) {
        raiseTypeError(globalObject, scope, "catching ExceptionGroup with except* is not allowed. Use except instead."_s);
        return false;
    }
    return true;
}

// _PyEval_ExceptionGroupMatch(), for `except* pattern` in the frame: (what of the exception the clause handles, what is left for the clauses after it). Either can be None.
JSValue matchExceptionGroup(JSGlobalObject* globalObject, CallFrame* frame, JSValue exception, JSValue pattern)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto pair = [&] (JSValue match, JSValue rest) { return PyTuple::create(globalObject, { match, rest }); };
    if (!checkStarType(globalObject, pattern))
        return { };
    if (isNone(exception))
        return pair(jsUndefined(), jsUndefined());
    if (givenExceptionMatches(globalObject, exception, pattern)) {
        if (isGroup(globalObject, exception))
            return pair(exception, jsUndefined());
        // One that is in no group is handled as a group of one, which is from here.
        JSValue wrapped = createGroup(globalObject, emptyString(), PyTuple::create(globalObject, { exception }));
        RETURN_IF_EXCEPTION(scope, { });
        addTracebackEntry(globalObject, wrapped, frame, frame->bytecodeIndex());
        return pair(wrapped, jsUndefined());
    }
    if (!isGroup(globalObject, exception))
        return pair(jsUndefined(), exception);

    JSValue method = getAttribute(globalObject, exception, Identifier::fromString(vm, "split"_s));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = call(globalObject, method, pattern);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isTuple(result) || typeOf(globalObject, result) != globalObject->pyRealm()->typeTuple())
        return raiseTypeError(globalObject, scope, makeString(typeName(globalObject, exception), ".split must return a tuple, not "_s, typeName(globalObject, result)));
    // More than two are let by, for what was written before this was looked at.
    if (asTuple(result)->length() < 2)
        return raiseTypeError(globalObject, scope, makeString(typeName(globalObject, exception), ".split must return a 2-tuple, got tuple of size "_s, asTuple(result)->length()));
    return pair(asTuple(result)->at(0), asTuple(result)->at(1));
}

void initializeExceptionGroups(JSGlobalObject* globalObject, JSObject* builtins)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    PyType* group = realm->typeBaseExceptionGroup();
    addMethods(globalObject, group, {
        { "__new__"_s, groupNew, Kind::New },
        { "__str__"_s, groupStr },
        { "__repr__"_s, groupRepr },
        { "derive"_s, groupDerive },
        { "split"_s, groupSplit },
        { "subgroup"_s, groupSubgroup },
    });
    addMember(globalObject, group, "message"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        return asObject(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_groupMessage);
    });
    addMember(globalObject, group, "exceptions"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        return asObject(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_groupExceptions);
    });

    // create_exception_group_class(): it has two bases, and is made as a class statement would make it.
    PyDict* attributes = PyDict::create(globalObject);
    attributes->set(globalObject, jsString(vm, String("__module__"_s)), jsString(vm, String("builtins"_s)));
    JSValue exceptionGroup = call(globalObject, realm->typeType(), jsString(vm, String("ExceptionGroup"_s)), PyTuple::create(globalObject, { group, realm->typeException() }), attributes);
    RELEASE_ASSERT(!scope.exception());
    realm->setExceptionGroupType(vm, asType(exceptionGroup));
    builtins->putDirect(vm, Identifier::fromString(vm, "ExceptionGroup"_s), exceptionGroup);
}

} } // namespace JSC::Python
