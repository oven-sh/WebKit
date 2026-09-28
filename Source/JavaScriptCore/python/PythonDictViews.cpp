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

#include "PyTuple.h"
#include "PythonSequences.h"

// dict.keys(), dict.values() and dict.items(), which are the end of CPython's Objects/dictobject.c, and mappingproxy, which is in Objects/descrobject.c.

namespace JSC { namespace Python {

// ---- The views

static PyDict* dictOfView(JSValue view) { return uncheckedDowncast<PyDict>(uncheckedDowncast<PyNativeObject>(view.asCell())->field(0).asCell()); }

static bool isView(JSGlobalObject* globalObject, JSValue value, BuiltinType type)
{
    return tryNativeObject(value) && typeOf(globalObject, value) == globalObject->pyRealm()->type(type);
}

// keys() and items() are like sets. values() is not.
static bool isSetLikeView(JSGlobalObject* globalObject, JSValue value)
{
    return isView(globalObject, value, BuiltinType::DictKeys) || isView(globalObject, value, BuiltinType::DictItems);
}

static bool viewContains(JSGlobalObject* globalObject, JSValue view, JSValue item)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isView(globalObject, view, BuiltinType::DictKeys))
        RELEASE_AND_RETURN(scope, dictOfView(view)->contains(globalObject, item));
    if (!isTuple(item) || asTuple(item)->length() != 2)
        return false;
    JSValue value = dictOfView(view)->get(globalObject, asTuple(item)->at(0));
    RETURN_IF_EXCEPTION(scope, false);
    if (!value)
        return false;
    RELEASE_AND_RETURN(scope, isEqual(globalObject, value, asTuple(item)->at(1)));
}

PYTHON_NATIVE(viewIter)
{
    auto kind = unpack<PyIterator::Kind>(callFrame, 0);
    return JSValue::encode(PyIterator::create(globalObject, kind, dictOfView(callFrame->argument(0))));
}

PYTHON_NATIVE(viewLen)
{
    UNUSED_PARAM(globalObject);
    return JSValue::encode(jsNumber(dictOfView(callFrame->argument(0))->size()));
}

PYTHON_NATIVE(viewRepr)
{
    NATIVE_PROLOGUE();
    JSArray* items = listFromIterable(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    String text = repr(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate(typeName(globalObject, args[0]), '(', text, ')'))));
}

PYTHON_NATIVE(viewContainsMethod)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(viewContains(globalObject, args[0], args[1]))));
}

static bool areAllContainedIn(JSGlobalObject* globalObject, JSValue these, JSValue other)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = true;
    forEach(globalObject, these, [&] (JSValue item) {
        result = contains(globalObject, other, item);
        return result && !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, false);
    return result;
}

PYTHON_NATIVE(viewCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue other = args[1];
    if (!isSet(other) && !isSetLikeView(globalObject, other))
        RETURN_NOT_IMPLEMENTED();
    int64_t size = dictOfView(self)->size();
    int64_t otherSize = length(globalObject, other);
    RETURN_IF_EXCEPTION(scope, { });
    bool result = false;
    switch (op) {
    case ComparisonOperator::Eq:
    case ComparisonOperator::NotEq:
        result = size == otherSize && areAllContainedIn(globalObject, self, other);
        if (op == ComparisonOperator::NotEq)
            result = !result;
        break;
    case ComparisonOperator::Lt:
        result = size < otherSize && areAllContainedIn(globalObject, self, other);
        break;
    case ComparisonOperator::LtE:
        result = size <= otherSize && areAllContainedIn(globalObject, self, other);
        break;
    case ComparisonOperator::Gt:
        result = size > otherSize && areAllContainedIn(globalObject, other, self);
        break;
    case ComparisonOperator::GtE:
        result = size >= otherSize && areAllContainedIn(globalObject, other, self);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result));
}

static PySet* newSetFrom(JSGlobalObject* globalObject, JSValue iterable)
{
    return setFromIterable(globalObject, globalObject->pyRealm()->structureFor(BuiltinType::Set), iterable);
}

static JSValue intersect(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The view may be on the right of the &.
    if (!isSetLikeView(globalObject, self))
        std::swap(self, other);
    int64_t size = dictOfView(self)->size();

    // What is gone through is the smaller of the two, where that can be told.
    if (isExactly(globalObject, other, globalObject->pyRealm()->typeSet()) && size <= static_cast<int64_t>(uncheckedDowncast<PySet>(other.asCell())->size())) {
        PySet* result = PySet::create(globalObject);
        forEach(globalObject, self, [&] (JSValue item) {
            bool isIn = contains(globalObject, other, item);
            if (isIn && !scope.exception())
                result->add(globalObject, item);
            return !scope.exception();
        });
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    }
    if (isSetLikeView(globalObject, other) && static_cast<int64_t>(dictOfView(other)->size()) > size)
        std::swap(self, other);

    PySet* result = PySet::create(globalObject);
    forEach(globalObject, other, [&] (JSValue item) {
        bool isIn = viewContains(globalObject, self, item);
        if (isIn && !scope.exception())
            result->add(globalObject, item);
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// The pairs that are in one and not in the other, without making a set of either.
static JSValue itemsInOnlyOne(JSGlobalObject* globalObject, PyDict* first, PyDict* second)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* left = PyDict::create(globalObject);
    first->forEach(globalObject, [&] (JSValue key, JSValue value) {
        left->set(globalObject, key, value);
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    PySet* result = PySet::create(globalObject);
    MarkedArgumentBuffer pairs;
    second->forEach(globalObject, [&] (JSValue key, JSValue value) {
        pairs.append(key);
        pairs.append(value);
        return true;
    });
    for (size_t i = 0; i < pairs.size(); i += 2) {
        JSValue key = pairs.at(i);
        JSValue value = pairs.at(i + 1);
        JSValue ofFirst = left->get(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        bool isInBoth = ofFirst && isEqual(globalObject, ofFirst, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (isInBoth)
            left->remove(globalObject, key);
        else
            result->add(globalObject, PyTuple::create(globalObject, { key, value }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    left->forEach(globalObject, [&] (JSValue key, JSValue value) {
        result->add(globalObject, PyTuple::create(globalObject, { key, value }));
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

// view - other, other - view and so on. As in CPython it is one function for either order, and only one of the two need be a view.
PYTHON_NATIVE(viewOperation)
{
    auto op = unpack<BinaryOperator>(callFrame, 0);
    bool isReflected = unpack<bool>(callFrame, 1);
    NATIVE_PROLOGUE();
    JSValue left = args[isReflected ? 1 : 0];
    JSValue right = args[isReflected ? 0 : 1];
    if (op == BinaryOperator::BitAnd)
        RELEASE_AND_RETURN(scope, JSValue::encode(intersect(globalObject, left, right)));
    if (op == BinaryOperator::BitXor && isView(globalObject, left, BuiltinType::DictItems) && isView(globalObject, right, BuiltinType::DictItems))
        RELEASE_AND_RETURN(scope, JSValue::encode(itemsInOnlyOne(globalObject, dictOfView(left), dictOfView(right))));
    PySet* result = newSetFrom(globalObject, left);
    RETURN_IF_EXCEPTION(scope, { });
    PySet* other = isSet(right) ? uncheckedDowncast<PySet>(right.asCell()) : newSetFrom(globalObject, right);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(setOperation(globalObject, op, true, result, other)));
}

PYTHON_NATIVE(viewIsDisjoint)
{
    NATIVE_PROLOGUE();
    JSValue self = args[0];
    JSValue other = args[1];
    if (self == other)
        return JSValue::encode(jsBoolean(!dictOfView(self)->size()));
    // What is gone through is the smaller of the two, where that can be told.
    if (isSet(other) || isSetLikeView(globalObject, other)) {
        int64_t otherSize = length(globalObject, other);
        RETURN_IF_EXCEPTION(scope, { });
        if (otherSize > static_cast<int64_t>(dictOfView(self)->size()))
            std::swap(self, other);
    }
    bool result = true;
    forEach(globalObject, other, [&] (JSValue item) {
        result = !contains(globalObject, self, item);
        return result && !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(result));
}

// ---- mappingproxy

static JSValue mappingOfProxy(JSValue proxy) { return uncheckedDowncast<PyNativeObject>(proxy.asCell())->field(0); }
static bool isProxy(JSGlobalObject* globalObject, JSValue value) { return isView(globalObject, value, BuiltinType::MappingProxy); }

PYTHON_NATIVE(proxyNew)
{
    NATIVE_PROLOGUE();
    if (!typeOf(globalObject, args[1])->lookup(vm, names.dunder_getitem) || isList(args[1]) || isTuple(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("mappingproxy() argument must be a mapping, not "_s, typeName(globalObject, args[1]))));
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::MappingProxy, args[1]));
}

PYTHON_NATIVE(proxyGetItem)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(getItem(globalObject, mappingOfProxy(args[0]), args[1])));
}

PYTHON_NATIVE(proxyContains)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(jsBoolean(contains(globalObject, mappingOfProxy(args[0]), args[1]))));
}

PYTHON_NATIVE(proxyLen)
{
    NATIVE_PROLOGUE();
    int64_t size = length(globalObject, mappingOfProxy(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, size));
}

PYTHON_NATIVE(proxyIter)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(getIterator(globalObject, mappingOfProxy(args[0]))));
}

PYTHON_NATIVE(proxyRepr)
{
    NATIVE_PROLOGUE();
    String text = repr(globalObject, mappingOfProxy(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(strOrMemoryError(globalObject, concatenate("mappingproxy("_s, text, ')'))));
}

PYTHON_NATIVE(proxyStr)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(jsString(vm, str(globalObject, mappingOfProxy(args[0])))));
}

PYTHON_NATIVE(proxyHash)
{
    NATIVE_PROLOGUE();
    int64_t result = hash(globalObject, mappingOfProxy(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, result));
}

// The mapping is compared with the other as it is, which if it is a proxy too asks its own mapping in its turn.
PYTHON_NATIVE(proxyCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, mappingOfProxy(args[0]), args[1])));
}

PYTHON_NATIVE(proxyOr)
{
    bool isReflected = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue left = args[isReflected ? 1 : 0];
    JSValue right = args[isReflected ? 0 : 1];
    if (isProxy(globalObject, left))
        left = mappingOfProxy(left);
    if (isProxy(globalObject, right))
        right = mappingOfProxy(right);
    RELEASE_AND_RETURN(scope, JSValue::encode(binaryOperation(globalObject, BinaryOperator::BitOr, false, left, right)));
}

PYTHON_NATIVE(proxyInPlaceOr)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("'|=' is not supported by "_s, typeName(globalObject, args[0]), "; use '|' instead"_s)));
}

// get, keys, values, items, copy and __reversed__: whatever the mapping does.
PYTHON_NATIVE(proxyForward)
{
    static constexpr ASCIILiteral methods[] = { "get"_s, "keys"_s, "values"_s, "items"_s, "copy"_s, "__reversed__"_s };
    unsigned which = unpack<unsigned>(callFrame, 0);
    NATIVE_PROLOGUE();
    JSValue function = getAttribute(globalObject, mappingOfProxy(args[0]), Identifier::fromString(vm, methods[which]));
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    for (unsigned i = 1; i < args.size(); ++i)
        arguments.append(args[i]);
    // get() is always given a default.
    if (!which && args.size() == 2)
        arguments.append(jsUndefined());
    RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, function, arguments)));
}

void initializeDictViews(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Iterator = PyIterator::Kind;

    struct View {
        PyType* type;
        Iterator forward;
        Iterator reverse;
    };
    for (auto& view : { View { realm->typeDictKeys(), Iterator::DictKeys, Iterator::DictReverseKeys }, View { realm->typeDictValues(), Iterator::DictValues, Iterator::DictReverseValues }, View { realm->typeDictItems(), Iterator::DictItems, Iterator::DictReverseItems } }) {
        addMethods(globalObject, view.type, {
            { "__iter__"_s, viewIter, Kind::Method, pack(view.forward) },
            { "__reversed__"_s, viewIter, Kind::Method, pack(view.reverse) },
            { "__len__"_s, viewLen },
            { "__repr__"_s, viewRepr },
        });
        addGetSet(globalObject, view.type, "mapping"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
            return PyNativeObject::create(globalObject, BuiltinType::MappingProxy, dictOfView(self));
        });
    }
    for (PyType* view : { realm->typeDictKeys(), realm->typeDictItems() }) {
        addMethods(globalObject, view, {
            { "__contains__"_s, viewContainsMethod },
            { "isdisjoint"_s, viewIsDisjoint },
            { "__and__"_s, viewOperation, Kind::Method, pack(BinaryOperator::BitAnd, false) },
            { "__rand__"_s, viewOperation, Kind::Method, pack(BinaryOperator::BitAnd, true) },
            { "__or__"_s, viewOperation, Kind::Method, pack(BinaryOperator::BitOr, false) },
            { "__ror__"_s, viewOperation, Kind::Method, pack(BinaryOperator::BitOr, true) },
            { "__sub__"_s, viewOperation, Kind::Method, pack(BinaryOperator::Sub, false) },
            { "__rsub__"_s, viewOperation, Kind::Method, pack(BinaryOperator::Sub, true) },
            { "__xor__"_s, viewOperation, Kind::Method, pack(BinaryOperator::BitXor, false) },
            { "__rxor__"_s, viewOperation, Kind::Method, pack(BinaryOperator::BitXor, true) },
        });
        addComparisons(globalObject, view, viewCompare);
        view->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
    }

    PyType* proxy = realm->typeMappingProxy();
    addMethods(globalObject, proxy, {
        { "__new__"_s, proxyNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__getitem__"_s, proxyGetItem },
        { "__contains__"_s, proxyContains },
        { "__len__"_s, proxyLen },
        { "__iter__"_s, proxyIter },
        { "__repr__"_s, proxyRepr },
        { "__str__"_s, proxyStr },
        { "__hash__"_s, proxyHash },
        { "__or__"_s, proxyOr, Kind::Method, pack(false) },
        { "__ror__"_s, proxyOr, Kind::Method, pack(true) },
        { "__ior__"_s, proxyInPlaceOr },
        { "get"_s, proxyForward, Kind::Method, pack(0u) },
        { "keys"_s, proxyForward, Kind::Method, pack(1u) },
        { "values"_s, proxyForward, Kind::Method, pack(2u) },
        { "items"_s, proxyForward, Kind::Method, pack(3u) },
        { "copy"_s, proxyForward, Kind::Method, pack(4u) },
        { "__reversed__"_s, proxyForward, Kind::Method, pack(5u) },
    });
    addComparisons(globalObject, proxy, proxyCompare);
}

} } // namespace JSC::Python
