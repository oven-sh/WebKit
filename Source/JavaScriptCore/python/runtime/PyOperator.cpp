/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _operator, which operator.py takes everything from. (Modules/_operator.c in CPython is the reference.)

// ---- The operators ----

#define OPERATOR_DEFINE_BINARY(name, op) \
    static JSValue operator##name(JSGlobalObject* globalObject, JSValue a, JSValue b) { return binaryOperation(globalObject, BinaryOperator::op, a, b); }
#define OPERATOR_DEFINE_UNARY(name, op) \
    static JSValue operator##name(JSGlobalObject* globalObject, JSValue a) { return unaryOperation(globalObject, UnaryOperator::op, a); }
#define OPERATOR_DEFINE_COMPARISON(name, op) \
    static JSValue operator##name(JSGlobalObject* globalObject, JSValue a, JSValue b) { return richCompare(globalObject, a, b, CompareOperator::op); }

OPERATOR_DEFINE_BINARY(Add, Add)
OPERATOR_DEFINE_BINARY(Sub, Sub)
OPERATOR_DEFINE_BINARY(Mul, Mult)
OPERATOR_DEFINE_BINARY(MatMul, MatMult)
OPERATOR_DEFINE_BINARY(FloorDiv, FloorDiv)
OPERATOR_DEFINE_BINARY(TrueDiv, Div)
OPERATOR_DEFINE_BINARY(Mod, Mod)
OPERATOR_DEFINE_BINARY(Pow, Pow)
OPERATOR_DEFINE_BINARY(LShift, LShift)
OPERATOR_DEFINE_BINARY(RShift, RShift)
OPERATOR_DEFINE_BINARY(And, BitAnd)
OPERATOR_DEFINE_BINARY(Xor, BitXor)
OPERATOR_DEFINE_BINARY(Or, BitOr)
OPERATOR_DEFINE_BINARY(IAdd, InPlaceAdd)
OPERATOR_DEFINE_BINARY(ISub, InPlaceSub)
OPERATOR_DEFINE_BINARY(IMul, InPlaceMult)
OPERATOR_DEFINE_BINARY(IMatMul, InPlaceMatMult)
OPERATOR_DEFINE_BINARY(IFloorDiv, InPlaceFloorDiv)
OPERATOR_DEFINE_BINARY(ITrueDiv, InPlaceDiv)
OPERATOR_DEFINE_BINARY(IMod, InPlaceMod)
OPERATOR_DEFINE_BINARY(IPow, InPlacePow)
OPERATOR_DEFINE_BINARY(ILShift, InPlaceLShift)
OPERATOR_DEFINE_BINARY(IRShift, InPlaceRShift)
OPERATOR_DEFINE_BINARY(IAnd, InPlaceBitAnd)
OPERATOR_DEFINE_BINARY(IXor, InPlaceBitXor)
OPERATOR_DEFINE_BINARY(IOr, InPlaceBitOr)
OPERATOR_DEFINE_UNARY(Neg, Negative)
OPERATOR_DEFINE_UNARY(Pos, Positive)
OPERATOR_DEFINE_UNARY(Invert, Invert)
OPERATOR_DEFINE_UNARY(Not, Not)
OPERATOR_DEFINE_COMPARISON(Lt, Lt)
OPERATOR_DEFINE_COMPARISON(Le, LtE)
OPERATOR_DEFINE_COMPARISON(Eq, Eq)
OPERATOR_DEFINE_COMPARISON(Ne, NotEq)
OPERATOR_DEFINE_COMPARISON(Gt, Gt)
OPERATOR_DEFINE_COMPARISON(Ge, GtE)

static JSValue operatorAbs(JSGlobalObject* globalObject, JSValue a) { return absolute(globalObject, a); }
static JSValue operatorIs(JSGlobalObject*, JSValue a, JSValue b) { return jsBoolean(isIdentical(a, b)); }
static JSValue operatorIsNot(JSGlobalObject*, JSValue a, JSValue b) { return jsBoolean(!isIdentical(a, b)); }
static JSValue operatorIsNone(JSGlobalObject*, JSValue a) { return jsBoolean(isNone(a)); }
static JSValue operatorIsNotNone(JSGlobalObject*, JSValue a) { return jsBoolean(!isNone(a)); }
static JSValue operatorGetItem(JSGlobalObject* globalObject, JSValue a, JSValue b) { return getItem(globalObject, a, b); }

static JSValue operatorTruth(JSGlobalObject* globalObject, JSValue a)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = isTrue(globalObject, a);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue operatorIndex(JSGlobalObject* globalObject, JSValue a)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = toIndex(globalObject, a);
    RETURN_IF_EXCEPTION(scope, { });
    // An int and nothing more, whatever subclass of it __index__ came up with.
    return intValue(result);
}

static JSValue operatorSetItem(JSGlobalObject* globalObject, JSValue a, JSValue b, JSValue c)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    setItem(globalObject, a, b, c);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue operatorDelItem(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    delItem(globalObject, a, b);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue operatorContains(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = contains(globalObject, a, b);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

// ---- Searching what can be iterated over ----

template<typename Function>
static bool operatorSearch(JSGlobalObject* globalObject, JSValue iterable, JSValue value, const Function& found)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIter(globalObject, iterable);
    if (!iterator) {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            raiseTypeError(globalObject, scope, "argument of type '"_s, typeName(globalObject, iterable), "' is not iterable"_s);
        return false;
    }
    for (int64_t index = 0;; ++index) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return true;
        bool isSame = isEqual(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (isSame && !found(index))
            return true;
    }
}

static JSValue operatorCountOf(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = 0;
    if (failed(scope, operatorSearch(globalObject, a, b, [&](int64_t) { ++count; return true; })))
        return { };
    RELEASE_AND_RETURN(scope, makeInt(globalObject, count));
}

static JSValue operatorIndexOf(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    int64_t result = -1;
    if (!operatorSearch(globalObject, a, b, [&](int64_t index) { result = index; return false; }))
        return { };
    if (result >= 0)
        return makeInt(globalObject, result);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseValueError(globalObject, scope, "sequence.index(x): x not in sequence"_s);
}

// ---- Concatenation ----

static bool operatorIsSequenceOperation(JSValue method)
{
    auto* native = method ? PyNativeFunction::tryFrom(method) : nullptr;
    return native && (native->def().flags & SequenceOperation);
}

// Whether it can be indexed as a sequence is, as far as its type tells.
static bool operatorIsSequence(JSGlobalObject* globalObject, JSValue value)
{
    if (isDict(value))
        return false;
    PyType* type = typeOf(globalObject, value);
    return type->lookup(Special::GetItem) && (type->isHeapType() || !type->has(TypeFlag::Mapping));
}

// a + b as numbers are added: __add__, __radd__. NotImplemented if neither will.
static JSValue operatorAddAsNumbers(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue notImplemented = rt.notImplemented();
    PyType* typeA = rt.typeOf(a);
    PyType* typeB = rt.typeOf(b);
    JSValue left = typeA->lookup(Special::Add);
    if (operatorIsSequenceOperation(left))
        left = JSValue();
    JSValue right = typeB != typeA ? typeB->lookup(Special::RAdd) : JSValue();

    if (right && typeB->isSubtypeOf(typeA) && right != typeA->lookup(Special::RAdd)) {
        JSValue result = callSpecial(globalObject, right, b, a);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
        right = JSValue();
    }
    if (left) {
        JSValue result = callSpecial(globalObject, left, a, b);
        RETURN_IF_EXCEPTION(scope, { });
        if (result != notImplemented)
            return result;
    }
    if (right)
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, right, b, a));
    return notImplemented;
}

static JSValue operatorConcatenate(JSGlobalObject* globalObject, JSValue a, JSValue b, bool inPlace)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(a);

    // A built-in sequence has its say first, and the last word.
    JSValue add = type->lookup(Special::Add);
    JSValue inPlaceAdd = inPlace ? type->lookup(Special::IAdd) : JSValue();
    if (operatorIsSequenceOperation(add)) {
        if (operatorIsSequenceOperation(inPlaceAdd))
            RELEASE_AND_RETURN(scope, callSpecial(globalObject, inPlaceAdd, a, b));
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, add, a, b));
    }

    // A class written in Python has only __add__ to say it with.
    if (operatorIsSequence(globalObject, a) && operatorIsSequence(globalObject, b)) {
        JSValue result = rt.notImplemented();
        if (inPlaceAdd) {
            result = callSpecial(globalObject, inPlaceAdd, a, b);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (result == rt.notImplemented()) {
            result = operatorAddAsNumbers(globalObject, a, b);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (result != rt.notImplemented())
            return result;
    }
    return raiseTypeError(globalObject, scope, '\'', type->nameInMessages(), "' object can't be concatenated"_s);
}

static JSValue operatorConcat(JSGlobalObject* globalObject, JSValue a, JSValue b) { return operatorConcatenate(globalObject, a, b, false); }
static JSValue operatorIConcat(JSGlobalObject* globalObject, JSValue a, JSValue b) { return operatorConcatenate(globalObject, a, b, true); }

// ---- length_hint, call, _compare_digest ----

static JSValue operatorLengthHint(JSGlobalObject* globalObject, JSValue object, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t fallback = 0;
    if (defaultValue) {
        JSValue index = toIndex(globalObject, defaultValue);
        RETURN_IF_EXCEPTION(scope, { });
        auto converted = tryInt64(index);
        if (!converted)
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        fallback = *converted;
    }
    int64_t result = lengthHint(globalObject, object, fallback);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
}

static JSValue operatorCall(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "call"_s, args, 1, std::numeric_limits<unsigned>::max())))
        return { };
    RELEASE_AND_RETURN(scope, call(globalObject, args[0], args.withoutFirst()));
}

// How long it takes depends on the length of `b` and on nothing else.
static NEVER_INLINE bool operatorCompareInConstantTime(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    const volatile uint8_t* right = b.data();
    const volatile uint8_t* left = a.size() == b.size() ? a.data() : b.data();
    volatile uint8_t difference = a.size() != b.size();
    for (size_t i = 0; i < b.size(); ++i)
        difference = difference | (left[i] ^ right[i]);
    return !difference;
}

static JSValue operatorCompareDigest(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(a) && isStr(b)) {
        String left = strValue(a)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        String right = strValue(b)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!left.containsOnlyASCII() || !right.containsOnlyASCII())
            return raiseTypeError(globalObject, scope, "comparing strings with non-ASCII characters is not supported"_s);
        CString leftBytes = left.ascii();
        CString rightBytes = right.ascii();
        return jsBoolean(operatorCompareInConstantTime(byteCast<uint8_t>(leftBytes.span()), byteCast<uint8_t>(rightBytes.span())));
    }
    if (!isBytesLike(a) && !isBytesLike(b))
        return raiseTypeError(globalObject, scope, "unsupported operand types(s) or combination of types: '"_s, typeName(globalObject, a), "' and '"_s, typeName(globalObject, b), '\'');
    std::span<const uint8_t> left;
    std::span<const uint8_t> right;
    if (!getBytesLike(globalObject, a, left) || !getBytesLike(globalObject, b, right))
        return { };
    return jsBoolean(operatorCompareInConstantTime(left, right));
}

// ---- What itemgetter, attrgetter and methodcaller have in common ----
//
// The instances are PyRecords.

static ALWAYS_INLINE PyRecord* operatorRecord(JSValue self) { return uncheckedDowncast<PyRecord>(self.asCell()); }

static JSValue operatorGetterTextSignature(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "(obj, /)"_s); }

static constexpr NativeGetSet operatorGetterGetSets[] = {
    { "__text_signature__"_s, operatorGetterTextSignature, nullptr },
};

// name(first, second), or name(only)
static JSValue operatorReprWithArguments(JSGlobalObject* globalObject, JSValue self, ASCIILiteral name, std::span<const JSValue> arguments, PyTuple* keywordNames = nullptr)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, makeString(name, "(...)"_s));
    size_t positionalCount = arguments.size() - (keywordNames ? keywordNames->length() : 0);
    StrBuilder builder;
    builder.append(name, '(');
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i)
            builder.append(", "_s);
        if (i >= positionalCount)
            builder.append(stringOf(asString(keywordNames->at(i - positionalCount))), '=');
        JSString* text = repr(globalObject, arguments[i]);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(text->value(globalObject).data);
        RETURN_IF_EXCEPTION(scope, { });
    }
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- itemgetter ----

namespace OperatorItemGetter {
enum : unsigned { Items, FieldCount }; // A tuple.
}

static PyType* operatorItemGetterType(JSGlobalObject*);

static JSValue operatorItemGetterNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "itemgetter"_s, args)))
        return { };
    if (!args.size() && failed(scope, checkPositional(globalObject, "itemgetter"_s, args, 1, 1)))
        return { };
    PyRecord* record = PyRecord::create(globalObject, operatorItemGetterType(globalObject), OperatorItemGetter::FieldCount);
    record->set(getVM(globalObject), OperatorItemGetter::Items, PyTuple::create(globalObject, args.positional()));
    return record;
}

static JSValue operatorItemGetterCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "itemgetter"_s, args)) || failed(scope, checkPositional(globalObject, "itemgetter"_s, args, 1, 1)))
        return { };
    JSValue object = args[0];
    PyTuple* items = PyTuple::from(operatorRecord(self)->get(OperatorItemGetter::Items));
    unsigned count = items->length();
    if (count == 1)
        RELEASE_AND_RETURN(scope, getItem(globalObject, object, items->at(0)));
    PyTuple* result = PyTuple::create(globalObject, count);
    for (unsigned i = 0; i < count; ++i) {
        JSValue value = getItem(globalObject, object, items->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        result->initializeAt(vm, i, value);
    }
    return result;
}

static JSValue operatorItemGetterRepr(JSGlobalObject* globalObject, JSValue self)
{
    PyTuple* items = PyTuple::from(operatorRecord(self)->get(OperatorItemGetter::Items));
    JSValue result = operatorReprWithArguments(globalObject, self, "operator.itemgetter"_s, items->span());
    ensureStillAliveHere(items);
    return result;
}

static JSValue operatorItemGetterReduce(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = operatorRecord(self);
    return PyTuple::create(globalObject, { record->type(), record->get(OperatorItemGetter::Items) });
}

static constexpr NativeDef operatorItemGetterMethods[] = {
    PY_NEW(operatorItemGetterNew, "($type, item, /, *items)"),
    PY_SLOT("__call__", operatorItemGetterCall, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", operatorItemGetterRepr, "($self, /)"),
    PY_METHOD("__reduce__", operatorItemGetterReduce, "($self, /)"),
};
static constexpr TypeDef operatorItemGetterTypeDef {
    "operator.itemgetter"_s, { TypeFlag::OwnLayout }, operatorItemGetterMethods, operatorGetterGetSets,
    "Return a callable object that fetches the given item(s) from its operand.\n"
    "After f = itemgetter(2), the call f(r) returns r[2].\n"
    "After g = itemgetter(2, 5, 3), the call g(r) returns (r[2], r[5], r[3])"_s
};
static PyType* operatorItemGetterType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(operatorItemGetterTypeDef); }

// ---- attrgetter ----

namespace OperatorAttrGetter {
enum : unsigned { Names, Paths, FieldCount }; // A tuple of the names as they were given, and one of what they stand for: a str, or for "a.b" a tuple of str.
}

static PyType* operatorAttrGetterType(JSGlobalObject*);

static JSValue operatorAttrGetterNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "attrgetter"_s, args)))
        return { };
    if (!args.size() && failed(scope, checkPositional(globalObject, "attrgetter"_s, args, 1, 1)))
        return { };
    unsigned count = args.size();
    PyTuple* names = PyTuple::create(globalObject, count);
    PyTuple* paths = PyTuple::create(globalObject, count);
    for (unsigned i = 0; i < count; ++i) {
        if (!isStr(args[i]))
            return raiseTypeError(globalObject, scope, "attribute name must be a string"_s);
        String name = strValue(args[i])->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        names->initializeAt(vm, i, internString(vm, name));
        if (name.find('.') == notFound) {
            paths->initializeAt(vm, i, names->at(i));
            continue;
        }
        MarkedArgumentBuffer parts;
        for (auto part : StringView(name).splitAllowingEmptyEntries('.'))
            parts.append(internString(vm, part.toString()));
        paths->initializeAt(vm, i, PyTuple::create(globalObject, spanOf(parts)));
    }
    PyRecord* record = PyRecord::create(globalObject, operatorAttrGetterType(globalObject), OperatorAttrGetter::FieldCount);
    record->set(vm, OperatorAttrGetter::Names, names);
    record->set(vm, OperatorAttrGetter::Paths, paths);
    return record;
}

static JSValue operatorGetAttrByPath(JSGlobalObject* globalObject, JSValue object, JSValue path)
{
    if (path.isString())
        return getAttr(globalObject, object, asString(path));
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* names = PyTuple::from(path);
    for (unsigned i = 0; i < names->length(); ++i) {
        object = getAttr(globalObject, object, asString(names->at(i)));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return object;
}

static JSValue operatorAttrGetterCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "attrgetter"_s, args)) || failed(scope, checkPositional(globalObject, "attrgetter"_s, args, 1, 1)))
        return { };
    JSValue object = args[0];
    PyTuple* paths = PyTuple::from(operatorRecord(self)->get(OperatorAttrGetter::Paths));
    unsigned count = paths->length();
    if (count == 1)
        RELEASE_AND_RETURN(scope, operatorGetAttrByPath(globalObject, object, paths->at(0)));
    PyTuple* result = PyTuple::create(globalObject, count);
    for (unsigned i = 0; i < count; ++i) {
        JSValue value = operatorGetAttrByPath(globalObject, object, paths->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        result->initializeAt(vm, i, value);
    }
    return result;
}

static JSValue operatorAttrGetterRepr(JSGlobalObject* globalObject, JSValue self)
{
    PyTuple* names = PyTuple::from(operatorRecord(self)->get(OperatorAttrGetter::Names));
    JSValue result = operatorReprWithArguments(globalObject, self, "operator.attrgetter"_s, names->span());
    ensureStillAliveHere(names);
    return result;
}

static JSValue operatorAttrGetterReduce(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = operatorRecord(self);
    return PyTuple::create(globalObject, { record->type(), record->get(OperatorAttrGetter::Names) });
}

static constexpr NativeDef operatorAttrGetterMethods[] = {
    PY_NEW(operatorAttrGetterNew, "($type, attr, /, *attrs)"),
    PY_SLOT("__call__", operatorAttrGetterCall, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", operatorAttrGetterRepr, "($self, /)"),
    PY_METHOD("__reduce__", operatorAttrGetterReduce, "($self, /)"),
};
static constexpr TypeDef operatorAttrGetterTypeDef {
    "operator.attrgetter"_s, { TypeFlag::OwnLayout }, operatorAttrGetterMethods, operatorGetterGetSets,
    "Return a callable object that fetches the given attribute(s) from its operand.\n"
    "After f = attrgetter('name'), the call f(r) returns r.name.\n"
    "After g = attrgetter('name', 'date'), the call g(r) returns (r.name, r.date).\n"
    "After h = attrgetter('name.first', 'name.last'), the call h(r) returns\n"
    "(r.name.first, r.name.last)."_s
};
static PyType* operatorAttrGetterType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(operatorAttrGetterTypeDef); }

// ---- methodcaller ----

namespace OperatorMethodCaller {
// Values is a tuple of the name, the positional arguments and the values of the keyword arguments; KeywordNames may be empty.
enum : unsigned { Values, KeywordNames, FieldCount };
}

static PyType* operatorMethodCallerType(JSGlobalObject*);

static JSValue operatorMethodCallerNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!args.size())
        return raiseTypeError(globalObject, scope, "methodcaller needs at least one argument, the method name"_s);
    if (!isStr(args[0]))
        return raiseTypeError(globalObject, scope, "method name must be a string"_s);
    JSString* name = internString(globalObject, strValue(args[0]));
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* values = PyTuple::create(globalObject, std::span { args.values(), args.totalCount() });
    values->initializeAt(vm, 0, name);
    PyRecord* record = PyRecord::create(globalObject, operatorMethodCallerType(globalObject), OperatorMethodCaller::FieldCount);
    record->set(vm, OperatorMethodCaller::Values, values);
    if (args.hasKeywords())
        record->set(vm, OperatorMethodCaller::KeywordNames, args.keywordNames());
    return record;
}

static ALWAYS_INLINE PyTuple* operatorMethodCallerKeywordNames(PyRecord* record)
{
    JSValue names = record->get(OperatorMethodCaller::KeywordNames);
    return names ? uncheckedDowncast<PyTuple>(names.asCell()) : nullptr;
}

static JSValue operatorMethodCallerCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "methodcaller"_s, args, 1, 1)) || failed(scope, rejectKeywords(globalObject, "methodcaller"_s, args)))
        return { };
    PyRecord* record = operatorRecord(self);
    PyTuple* values = PyTuple::from(record->get(OperatorMethodCaller::Values));
    PyTuple* keywordNames = operatorMethodCallerKeywordNames(record);
    unsigned positionalCount = values->length() - 1 - (keywordNames ? keywordNames->length() : 0);
    CallArgs stored(values->span().data() + 1, positionalCount, keywordNames);

    JSValue receiver;
    JSValue method = getMethod(globalObject, args[0], asString(values->at(0)), receiver);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = receiver ? withFirstArgument(receiver, stored, [&](const CallArgs& all) { return call(globalObject, method, all); }) : call(globalObject, method, stored);
    ensureStillAliveHere(values);
    return result;
}

static JSValue operatorMethodCallerRepr(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = operatorRecord(self);
    PyTuple* values = PyTuple::from(record->get(OperatorMethodCaller::Values));
    JSValue result = operatorReprWithArguments(globalObject, self, "operator.methodcaller"_s, values->span(), operatorMethodCallerKeywordNames(record));
    ensureStillAliveHere(values);
    return result;
}

static JSValue operatorMethodCallerReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* record = operatorRecord(self);
    PyTuple* values = PyTuple::from(record->get(OperatorMethodCaller::Values));
    PyTuple* keywordNames = operatorMethodCallerKeywordNames(record);
    if (!keywordNames)
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { record->type(), values }));

    // A constructor cannot be handed keywords when unpickling: partial(methodcaller, name, **keywords) can.
    JSValue functools = importModule(globalObject, "functools"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue partial = getAttr(globalObject, functools, internString(vm, "partial"_s));
    RETURN_IF_EXCEPTION(scope, { });
    unsigned positionalCount = values->length() - 1 - keywordNames->length();
    MarkedArgumentBuffer arguments;
    arguments.append(record->type());
    arguments.append(values->at(0));
    for (unsigned i = 1 + positionalCount; i < values->length(); ++i)
        arguments.append(values->at(i));
    JSValue constructor = call(globalObject, partial, CallArgs(valuesOf(arguments), 2, keywordNames));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { constructor, PyTuple::create(globalObject, values->span().subspan(1, positionalCount)) }));
}

static constexpr NativeDef operatorMethodCallerMethods[] = {
    PY_NEW(operatorMethodCallerNew, "($type, name, /, *args, **kwargs)"),
    PY_SLOT("__call__", operatorMethodCallerCall, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", operatorMethodCallerRepr, "($self, /)"),
    PY_METHOD("__reduce__", operatorMethodCallerReduce, "($self, /)"),
};
static constexpr TypeDef operatorMethodCallerTypeDef {
    "operator.methodcaller"_s, { TypeFlag::OwnLayout }, operatorMethodCallerMethods, operatorGetterGetSets,
    "Return a callable object that calls the given method on its operand.\n"
    "After f = methodcaller('name'), the call f(r) returns r.name().\n"
    "After g = methodcaller('name', 'date', foo=1), the call g(r) returns\n"
    "r.name('date', foo=1)."_s
};
static PyType* operatorMethodCallerType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(operatorMethodCallerTypeDef); }

// ---- The module ----

static constexpr NativeDef operatorFunctions[] = {
    PY_FUNCTION("truth", operatorTruth, "(a, /)"),
    PY_FUNCTION("contains", operatorContains, "(a, b, /)"),
    PY_FUNCTION("indexOf", operatorIndexOf, "(a, b, /)"),
    PY_FUNCTION("countOf", operatorCountOf, "(a, b, /)"),
    PY_FUNCTION("is_", operatorIs, "(a, b, /)"),
    PY_FUNCTION("is_not", operatorIsNot, "(a, b, /)"),
    PY_FUNCTION("is_none", operatorIsNone, "(a, /)"),
    PY_FUNCTION("is_not_none", operatorIsNotNone, "(a, /)"),
    PY_FUNCTION("index", operatorIndex, "(a, /)"),
    PY_FUNCTION("add", operatorAdd, "(a, b, /)"),
    PY_FUNCTION("sub", operatorSub, "(a, b, /)"),
    PY_FUNCTION("mul", operatorMul, "(a, b, /)"),
    PY_FUNCTION("matmul", operatorMatMul, "(a, b, /)"),
    PY_FUNCTION("floordiv", operatorFloorDiv, "(a, b, /)"),
    PY_FUNCTION("truediv", operatorTrueDiv, "(a, b, /)"),
    PY_FUNCTION("mod", operatorMod, "(a, b, /)"),
    PY_FUNCTION("neg", operatorNeg, "(a, /)"),
    PY_FUNCTION("pos", operatorPos, "(a, /)"),
    PY_FUNCTION("abs", operatorAbs, "(a, /)"),
    PY_FUNCTION("inv", operatorInvert, "(a, /)"),
    PY_FUNCTION("invert", operatorInvert, "(a, /)"),
    PY_FUNCTION("lshift", operatorLShift, "(a, b, /)"),
    PY_FUNCTION("rshift", operatorRShift, "(a, b, /)"),
    PY_FUNCTION("not_", operatorNot, "(a, /)"),
    PY_FUNCTION("and_", operatorAnd, "(a, b, /)"),
    PY_FUNCTION("xor", operatorXor, "(a, b, /)"),
    PY_FUNCTION("or_", operatorOr, "(a, b, /)"),
    PY_FUNCTION("iadd", operatorIAdd, "(a, b, /)"),
    PY_FUNCTION("isub", operatorISub, "(a, b, /)"),
    PY_FUNCTION("imul", operatorIMul, "(a, b, /)"),
    PY_FUNCTION("imatmul", operatorIMatMul, "(a, b, /)"),
    PY_FUNCTION("ifloordiv", operatorIFloorDiv, "(a, b, /)"),
    PY_FUNCTION("itruediv", operatorITrueDiv, "(a, b, /)"),
    PY_FUNCTION("imod", operatorIMod, "(a, b, /)"),
    PY_FUNCTION("ilshift", operatorILShift, "(a, b, /)"),
    PY_FUNCTION("irshift", operatorIRShift, "(a, b, /)"),
    PY_FUNCTION("iand", operatorIAnd, "(a, b, /)"),
    PY_FUNCTION("ixor", operatorIXor, "(a, b, /)"),
    PY_FUNCTION("ior", operatorIOr, "(a, b, /)"),
    PY_FUNCTION("concat", operatorConcat, "(a, b, /)"),
    PY_FUNCTION("iconcat", operatorIConcat, "(a, b, /)"),
    PY_FUNCTION("getitem", operatorGetItem, "(a, b, /)"),
    PY_FUNCTION("setitem", operatorSetItem, "(a, b, c, /)"),
    PY_FUNCTION("delitem", operatorDelItem, "(a, b, /)"),
    PY_FUNCTION("pow", operatorPow, "(a, b, /)"),
    PY_FUNCTION("ipow", operatorIPow, "(a, b, /)"),
    PY_FUNCTION("eq", operatorEq, "(a, b, /)"),
    PY_FUNCTION("ne", operatorNe, "(a, b, /)"),
    PY_FUNCTION("lt", operatorLt, "(a, b, /)"),
    PY_FUNCTION("le", operatorLe, "(a, b, /)"),
    PY_FUNCTION("gt", operatorGt, "(a, b, /)"),
    PY_FUNCTION("ge", operatorGe, "(a, b, /)"),
    PY_FUNCTION("_compare_digest", operatorCompareDigest, "(a, b, /)"),
    PY_FUNCTION("length_hint", operatorLengthHint, "(obj, default=0, /)"),
    PY_FUNCTION("call", operatorCall, "(obj, /, *args, **kwargs)"),
};

void initializeOperatorModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, operatorFunctions);
    module->add(globalObject, "attrgetter"_s, operatorAttrGetterType(globalObject));
    module->add(globalObject, "itemgetter"_s, operatorItemGetterType(globalObject));
    module->add(globalObject, "methodcaller"_s, operatorMethodCallerType(globalObject));
    module->add(globalObject, "__doc__"_s, makeStr(getVM(globalObject),
        "Operator interface.\n\n"
        "This module exports a set of functions implemented in C corresponding\n"
        "to the intrinsic operators of Python.  For example, operator.add(x, y)\n"
        "is equivalent to the expression x+y.  The function names are those\n"
        "used for special methods; variants without leading and trailing\n"
        "'__' are also provided for convenience."_s));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
