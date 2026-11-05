/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyGenerator.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"

namespace JSC {
namespace Python {

// The module builtins. (Python/bltinmodule.c in CPython is the reference.)

// The keyword arguments of a function that takes any number of positional ones. False: one it does not know, a TypeError.
// More of them than it knows are complained of as that, by most functions. False: a TypeError.
static bool checkKeywordCount(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, unsigned most)
{
    if (args.keywordCount() <= most)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, function, "() takes at most "_s, most, " keyword argument"_s, most == 1 ? ""_s : "s"_s, " ("_s, args.keywordCount(), " given)"_s);
    return false;
}

bool parseKeywordArguments(JSGlobalObject* globalObject, ASCIILiteral function, const CallArgs& args, std::span<const ASCIILiteral> names, JSValue* values)
{
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        const StringImpl* keyword = args.keywordName(i)->tryGetValueImpl();
        bool found = false;
        for (size_t j = 0; j < names.size(); ++j) {
            if (WTF::equal(keyword, names[j].span8())) {
                values[j] = args.keywordValue(i);
                found = true;
                break;
            }
        }
        if (!found) {
            raiseUnexpectedKeyword(globalObject, function, stringOf(args.keywordName(i)), names);
            return false;
        }
    }
    return true;
}

// ---- Objects ----

static JSValue builtinAbs(JSGlobalObject* globalObject, JSValue value) { return absolute(globalObject, value); }
static JSValue builtinAscii(JSGlobalObject* globalObject, JSValue value) { return asciiObject(globalObject, value); }
static JSValue builtinRepr(JSGlobalObject* globalObject, JSValue value) { return reprObject(globalObject, value); }
static JSValue builtinCallable(JSGlobalObject* globalObject, JSValue value) { return jsBoolean(isCallable(globalObject, value)); }
static JSValue builtinDivmod(JSGlobalObject* globalObject, JSValue a, JSValue b) { return divmod(globalObject, a, b); }
static JSValue builtinId(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue identity = identityOf(globalObject, value);
    if (identity && failed(scope, audit(globalObject, "builtins.id"_s, identity)))
        return { };
    return identity;
}
static JSValue builtinPow(JSGlobalObject* globalObject, JSValue base, JSValue exponent, JSValue modulus) { return power(globalObject, base, exponent, modulus ? modulus : jsUndefined()); }

static JSValue builtinHash(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = hash(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
}

static JSValue builtinLen(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = length(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, result));
}

static JSValue builtinIsInstance(JSGlobalObject* globalObject, JSValue object, JSValue classInfo)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = isInstance(globalObject, object, classInfo);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue builtinIsSubclass(JSGlobalObject* globalObject, JSValue cls, JSValue classInfo)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = isSubclass(globalObject, cls, classInfo);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue builtinFormat(JSGlobalObject* globalObject, JSValue value, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (spec && !isStr(spec))
        return raiseTypeError(globalObject, scope, "format() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, spec));
    RELEASE_AND_RETURN(scope, formatObject(globalObject, value, spec ? strValue(spec) : nullptr));
}

static JSValue builtinRound(JSGlobalObject* globalObject, JSValue number, JSValue digits)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(number);
    JSValue method = type->lookup(Special::Round);
    if (!method)
        return raiseTypeError(globalObject, scope, "type "_s, type->name(), " doesn't define __round__ method"_s);
    if (!digits || isNone(digits))
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, number));
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, method, number, digits));
}

// ---- Attributes ----

static JSValue builtinGetAttr(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "getattr"_s, args)) || failed(scope, checkPositional(globalObject, "getattr"_s, args, 2, 3)))
        return { };
    JSString* name = attributeName(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() == 2)
        RELEASE_AND_RETURN(scope, getAttr(globalObject, args[0], name));
    JSValue value = getAttrIfPresent(globalObject, args[0], name);
    RETURN_IF_EXCEPTION(scope, { });
    return value ? value : args[2];
}

static JSValue builtinHasAttr(JSGlobalObject* globalObject, JSValue object, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    bool result = hasAttr(globalObject, object, name);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue builtinSetAttr(JSGlobalObject* globalObject, JSValue object, JSValue nameValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (nameValue.isString()) [[likely]]
        setAttr(globalObject, object, name, value);
    else // __setattr__ gets the instance of a class derived from str that this was given.
        callSpecial(globalObject, typeOf(globalObject, object)->lookup(Special::SetAttr), object, nameValue, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue builtinDelAttr(JSGlobalObject* globalObject, JSValue object, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* name = attributeName(globalObject, nameValue);
    RETURN_IF_EXCEPTION(scope, { });
    delAttr(globalObject, object, name);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue builtinGlobals(JSGlobalObject* globalObject)
{
    PyDict* globals = currentGlobals(globalObject);
    return globals ? JSValue(globals) : jsUndefined();
}

static JSValue builtinLocals(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSValue locals = currentLocals(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return locals ? locals : jsUndefined();
}

static JSValue builtinVars(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "vars"_s, args)) || failed(scope, checkPositional(globalObject, "vars"_s, args, 0, 1)))
        return { };
    if (!args.size())
        RELEASE_AND_RETURN(scope, builtinLocals(globalObject));
    JSValue dict = getAttrIfPresent(globalObject, args[0], runtime(globalObject).names.dDict());
    RETURN_IF_EXCEPTION(scope, { });
    if (!dict)
        return raiseTypeError(globalObject, scope, "vars() argument must have __dict__ attribute"_s);
    return dict;
}

static JSValue builtinDir(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "dir"_s, args)) || failed(scope, checkPositional(globalObject, "dir"_s, args, 0, 1)))
        return { };
    JSValue names;
    if (!args.size()) {
        JSValue locals = currentLocals(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!locals)
            return raiseSystemError(globalObject, scope, "frame does not exist"_s);
        names = callMethod(globalObject, locals, rt.names.keys());
    } else {
        JSValue method = rt.typeOf(args[0])->lookup(Special::Dir);
        if (!method)
            return raiseTypeError(globalObject, scope, "object does not provide __dir__"_s);
        names = callSpecial(globalObject, method, args[0]);
    }
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = listFromIterable(globalObject, names);
    RETURN_IF_EXCEPTION(scope, { });
    listSort(globalObject, list, { }, false);
    RETURN_IF_EXCEPTION(scope, { });
    return list;
}

// ---- Text and numbers ----

static JSValue intWithPrefix(JSGlobalObject* globalObject, JSValue number, unsigned radix, ASCIILiteral prefix)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, number);
    RETURN_IF_EXCEPTION(scope, { });
    bool isNegative = intSign(index) < 0;
    if (isNegative) {
        index = intNegate(globalObject, index);
        RETURN_IF_EXCEPTION(scope, { });
    }
    String digits = intToString(globalObject, index, radix);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(isNegative ? "-"_s : ""_s, prefix, digits));
}

static JSValue builtinBin(JSGlobalObject* globalObject, JSValue number) { return intWithPrefix(globalObject, number, 2, "0b"_s); }
static JSValue builtinOct(JSGlobalObject* globalObject, JSValue number) { return intWithPrefix(globalObject, number, 8, "0o"_s); }
static JSValue builtinHex(JSGlobalObject* globalObject, JSValue number) { return intWithPrefix(globalObject, number, 16, "0x"_s); }

static JSValue builtinChr(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    auto code = tryInt64(index);
    if (!code || *code < 0 || *code > 0x10ffff)
        return raiseValueError(globalObject, scope, "chr() arg not in range(0x110000)"_s);
    if (*code < 0x100)
        return jsSingleCharacterString(vm, static_cast<Latin1Character>(*code));
    return makeStr(vm, stringFromCodePoint(static_cast<char32_t>(*code)));
}

static JSValue builtinOrd(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(value)) {
        StrView view(globalObject, strValue(value));
        RETURN_IF_EXCEPTION(scope, { });
        if (view.length() != 1)
            return raiseTypeError(globalObject, scope, "ord() expected a character, but string of length "_s, view.length(), " found"_s);
        return jsNumber(static_cast<uint32_t>(view.at(0)));
    }
    if (PyBytes::is(value) || PyByteArray::is(value)) {
        std::span<const uint8_t> bytes;
        if (failed(scope, getBytesLike(globalObject, value, bytes)))
            return { };
        if (bytes.size() != 1)
            return raiseTypeError(globalObject, scope, "ord() expected a character, but string of length "_s, bytes.size(), " found"_s);
        return jsNumber(bytes[0]);
    }
    return raiseTypeError(globalObject, scope, "ord() expected string of length 1, but "_s, typeName(globalObject, value), " found"_s);
}

// ---- Iteration ----

static JSValue builtinAll(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    GeneratorArgumentScope argument(globalObject, iterable);
    bool result = forEach(globalObject, iterable, [&](JSValue item) { return isTrue(globalObject, item); });
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue builtinAny(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    GeneratorArgumentScope argument(globalObject, iterable);
    bool noneIsTrue = forEach(globalObject, iterable, [&](JSValue item) { return !isTrue(globalObject, item); });
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(!noneIsTrue);
}

static JSValue builtinIter(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "iter"_s, args)) || failed(scope, checkPositional(globalObject, "iter"_s, args, 1, 2)))
        return { };
    if (args.size() == 1)
        RELEASE_AND_RETURN(scope, getIter(globalObject, args[0]));
    if (!isCallable(globalObject, args[0]))
        return raiseTypeError(globalObject, scope, "iter(v, w): v must be callable"_s);
    PyIterator* iterator = PyIterator::create(globalObject, PyIterator::Kind::Callable, args[0]);
    iterator->setExtra(vm, args[1]);
    return iterator;
}

static JSValue builtinNext(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "next"_s, args)) || failed(scope, checkPositional(globalObject, "next"_s, args, 1, 2)))
        return { };
    JSValue iterator = args[0];
    GeneratorArgumentScope argument(globalObject, iterator);
    PyType* type = rt.typeOf(iterator);
    JSValue method = type->lookup(Special::Next);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, '\'', type->name(), "' object is not an iterator"_s);
    // Through the method, and not iterNext(): a StopIteration keeps its value.
    JSValue item = callSpecial(globalObject, method, iterator);
    EXCEPTION_ASSERT(!!scope.exception() == !item);
    if (item || args.size() == 1)
        return item;
    scope.release();
    if (clearPendingExceptionIfMatches(globalObject, rt.StopIteration()))
        return args[1];
    return { };
}

static JSValue builtinAIter(JSGlobalObject* globalObject, JSValue iterable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(iterable);
    JSValue method = type->lookup(Special::AIter);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, '\'', type->name(), "' object is not an async iterable"_s);
    JSValue iterator = callSpecial(globalObject, method, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue next = rt.typeOf(iterator)->lookup(Special::ANext);
    if (!next || isNone(next))
        return raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, type), ".__aiter__() must return an async iterator, not "_s, typeNameForMessage(globalObject, rt.typeOf(iterator)));
    return iterator;
}

static JSValue builtinANext(JSGlobalObject* globalObject, JSValue iterator, JSValue defaultValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = rt.typeOf(iterator);
    JSValue method = type->lookup(Special::ANext);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, '\'', type->name(), "' object is not an async iterator"_s);
    JSValue awaitable = callSpecial(globalObject, method, iterator);
    RETURN_IF_EXCEPTION(scope, { });
    if (!defaultValue)
        return awaitable;
    RELEASE_AND_RETURN(scope, makeANextAwaitable(globalObject, awaitable, defaultValue));
}

template<CompareOperator op>
static JSValue minOrMax(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASCIILiteral name = op == CompareOperator::Lt ? "min"_s : "max"_s;
    if (!args.size())
        return raiseTypeError(globalObject, scope, name, " expected at least 1 argument, got 0"_s);
    static constexpr ASCIILiteral keywords[] = { "key"_s, "default"_s };
    JSValue options[2];
    if (failed(scope, checkKeywordCount(globalObject, name, args, 2)) || failed(scope, parseKeywordArguments(globalObject, name, args, keywords, options)))
        return { };
    JSValue key = options[0] && !isNone(options[0]) ? options[0] : JSValue();
    JSValue defaultValue = options[1];
    bool isPositional = args.size() > 1;
    if (isPositional && defaultValue)
        return raiseTypeError(globalObject, scope, "Cannot specify a default for "_s, name, "() with multiple positional arguments"_s);
    // (One of several arguments may be what is returned.)
    GeneratorArgumentScope argument(globalObject, isPositional ? JSValue() : args[0]);

    JSValue bestItem;
    JSValue bestKey;
    auto consider = [&](JSValue item) {
        JSValue itemKey = item;
        if (key) {
            itemKey = call(globalObject, key, item);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (!bestItem) {
            bestItem = item;
            bestKey = itemKey;
            return true;
        }
        bool isBetter;
        if (itemKey.isInt32() && bestKey.isInt32()) [[likely]]
            isBetter = op == CompareOperator::Lt ? itemKey.asInt32() < bestKey.asInt32() : itemKey.asInt32() > bestKey.asInt32();
        else {
            isBetter = richCompareBool(globalObject, itemKey, bestKey, op);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (isBetter) {
            bestItem = item;
            bestKey = itemKey;
        }
        return true;
    };

    if (isPositional) {
        for (unsigned i = 0; i < args.size(); ++i) {
            if (!consider(args[i]))
                return { };
        }
    } else {
        forEach(globalObject, args[0], consider);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (bestItem)
        return bestItem;
    if (defaultValue)
        return defaultValue;
    return raiseValueError(globalObject, scope, name, "() iterable argument is empty"_s);
}

// list(iterable).sort(**keywords), which is whose complaints about the keywords these are.
static JSValue builtinSorted(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "sorted"_s, args, 1, 1)))
        return { };
    JSArray* list = listFromIterable(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    static constexpr ASCIILiteral keywords[] = { "key"_s, "reverse"_s };
    JSValue options[2];
    if (args.hasKeywords() && (failed(scope, checkKeywordCount(globalObject, "sort"_s, args, 2)) || failed(scope, parseKeywordArguments(globalObject, "sort"_s, args, keywords, options))))
        return { };
    JSValue key = options[0];
    JSValue reverse = options[1];
    bool isReversed = reverse && isTrue(globalObject, reverse);
    RETURN_IF_EXCEPTION(scope, { });
    listSort(globalObject, list, key && !isNone(key) ? key : JSValue(), isReversed);
    RETURN_IF_EXCEPTION(scope, { });
    return list;
}

namespace {

// A sum of floats that keeps what each addition rounds off.
struct CompensatedSum {
    void add(double x)
    {
        double t = high + x;
        if (std::abs(high) >= std::abs(x))
            low += (high - t) + x;
        else
            low += (x - t) + high;
        high = t;
    }
    double value() const { return low && std::isfinite(low) ? high + low : high; }

    double high { 0 };
    double low { 0 };
};

}

static JSValue builtinSum(JSGlobalObject* globalObject, JSValue iterable, JSValue start)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    GeneratorArgumentScope argument(globalObject, iterable);
    Items items(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = start ? start : jsNumber(0);
    if (isStr(result))
        return raiseTypeError(globalObject, scope, "sum() can't sum strings [use ''.join(seq) instead]"_s);
    if (PyBytes::is(result))
        return raiseTypeError(globalObject, scope, "sum() can't sum bytes [use b''.join(seq) instead]"_s);
    if (PyByteArray::is(result))
        return raiseTypeError(globalObject, scope, "sum() can't sum bytearray [use b''.join(seq) instead]"_s);

    JSValue item;
    auto next = [&] ALWAYS_INLINE_LAMBDA {
        item = items.next();
        return !!item;
    };

    Runtime& rt = runtime(globalObject);
    auto isExactFloat = [&](JSValue value) {
        return value.isCell() ? PyFloat::is(value) && PyFloat::from(value)->type() == rt.floatType() : isFloat(value);
    };
    auto isExactComplex = [&](JSValue value) {
        return PyComplex::is(value) && PyComplex::from(value)->type() == rt.complexType();
    };
    // False: it is too large, which has been raised.
    auto intAsDouble = [&](JSValue value, double& number) {
        number = intToDouble(intValue(value));
        if (!std::isinf(number))
            return true;
        raiseOverflowError(globalObject, scope, "int too large to convert to float"_s);
        return false;
    };

    // As in CPython, ints come first, then floats, then complex numbers, each for as long as the items allow, and then
    // whatever there is. Floats are summed with compensation, so which of these an item falls to shows in the result.
    while (isExactInt(result)) {
        bool hasItem = next();
        RETURN_IF_EXCEPTION(scope, { });
        if (!hasItem)
            return result;
        if (int32_t total; result.isInt32() && item.isInt32() && !__builtin_add_overflow(result.asInt32(), item.asInt32(), &total)) [[likely]] {
            result = jsNumber(total);
            continue;
        }
        if (isSmallInt(result) && isSmallInt(item)) [[likely]] {
            if (int64_t total = smallIntValue(result) + smallIntValue(item); fitsSmallInt(total)) [[likely]] {
                result = smallInt(total);
                continue;
            }
        }
        bool goesOn = isExactInt(item) || item.isBoolean();
        result = binaryOperation(globalObject, BinaryOperator::Add, result, item);
        RETURN_IF_EXCEPTION(scope, { });
        if (!goesOn)
            break;
    }

    if (isExactFloat(result)) {
        CompensatedSum sum;
        sum.high = floatValue(result);
        while (true) {
            bool hasItem = next();
            RETURN_IF_EXCEPTION(scope, { });
            if (!hasItem)
                RELEASE_AND_RETURN(scope, makeFloat(globalObject, sum.value()));
            if (isExactFloat(item)) {
                sum.add(floatValue(item));
                continue;
            }
            if (isInt(item)) {
                double value;
                if (!intAsDouble(item, value))
                    return { };
                sum.add(value);
                continue;
            }
            result = binaryOperation(globalObject, BinaryOperator::Add, makeFloat(globalObject, sum.value()), item);
            RETURN_IF_EXCEPTION(scope, { });
            break;
        }
    }

    if (isExactComplex(result)) {
        CompensatedSum real;
        CompensatedSum imaginary;
        real.high = PyComplex::from(result)->real();
        imaginary.high = PyComplex::from(result)->imaginary();
        while (true) {
            bool hasItem = next();
            RETURN_IF_EXCEPTION(scope, { });
            if (!hasItem)
                RELEASE_AND_RETURN(scope, PyComplex::create(globalObject, real.value(), imaginary.value()));
            if (isExactComplex(item)) {
                real.add(PyComplex::from(item)->real());
                imaginary.add(PyComplex::from(item)->imaginary());
                continue;
            }
            if (isInt(item)) {
                double value;
                if (!intAsDouble(item, value))
                    return { };
                real.add(value);
                continue;
            }
            if (isFloat(item)) {
                real.add(floatValue(item));
                continue;
            }
            result = binaryOperation(globalObject, BinaryOperator::Add, PyComplex::create(globalObject, real.value(), imaginary.value()), item);
            RETURN_IF_EXCEPTION(scope, { });
            break;
        }
    }

    while (true) {
        bool hasItem = next();
        RETURN_IF_EXCEPTION(scope, { });
        if (!hasItem)
            return result;
        result = binaryOperation(globalObject, BinaryOperator::Add, result, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

// ---- Input and output ----

static JSValue builtinPrint(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "sep"_s, "end"_s, "file"_s, "flush"_s };
    JSValue options[4];
    if (args.hasKeywords() && failed(scope, parseKeywordArguments(globalObject, "print"_s, args, keywords, options)))
        return { };
    JSValue separator = options[0];
    JSValue end = options[1];
    JSValue file = options[2];

    if (!file || isNone(file)) {
        file = rt.sysDict()->get(rt.names.stdout_());
        if (!file)
            return raiseRuntimeError(globalObject, scope, "lost sys.stdout"_s);
        // sys.stdout may be None when there is no console.
        if (isNone(file))
            return jsUndefined();
    }
    if (separator && isNone(separator))
        separator = JSValue();
    if (separator && !isStr(separator))
        return raiseTypeError(globalObject, scope, "sep must be None or a string, not "_s, typeName(globalObject, separator));
    if (end && isNone(end))
        end = JSValue();
    if (end && !isStr(end))
        return raiseTypeError(globalObject, scope, "end must be None or a string, not "_s, typeName(globalObject, end));

    JSValue self;
    JSValue write = getMethod(globalObject, file, rt.names.write(), self);
    RETURN_IF_EXCEPTION(scope, { });
    auto emit = [&](JSValue text) {
        if (self)
            call(globalObject, write, self, text);
        else
            call(globalObject, write, text);
    };
    for (unsigned i = 0; i < args.size(); ++i) {
        if (i) {
            emit(separator ? separator : JSValue(jsSingleCharacterString(vm, ' ')));
            RETURN_IF_EXCEPTION(scope, { });
        }
        JSString* text = str(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        emit(text);
        RETURN_IF_EXCEPTION(scope, { });
    }
    emit(end ? end : JSValue(jsSingleCharacterString(vm, '\n')));
    RETURN_IF_EXCEPTION(scope, { });

    if (options[3]) {
        bool flush = isTrue(globalObject, options[3]);
        RETURN_IF_EXCEPTION(scope, { });
        if (flush) {
            callMethod(globalObject, file, rt.names.flush());
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return jsUndefined();
}

JSValue readLineWithEditor(JSGlobalObject* globalObject, JSString* prompt)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = rt.modules()->get(internString(vm, "readline"_s));
    if (!module)
        return { };
    JSValue function = getAttrIfPresent(globalObject, module, internString(vm, "_read_line"_s));
    RETURN_IF_EXCEPTION(scope, { });
    if (!function)
        return { };
    JSValue line = call(globalObject, function, prompt);
    if (scope.exception()) {
        // Ctrl-C, Ctrl-D: it happened in input(), as far as the program is concerned.
        if (auto* exception = PyException::tryFrom(pendingException(globalObject)))
            exception->setTraceback(vm, JSValue());
        return { };
    }
    if (line == rt.notImplemented())
        return { };
    if (!isStr(line))
        return raiseTypeError(globalObject, scope, "readline._read_line() returned non-string"_s);
    return line;
}

// file.fileno() == descriptor, and that is a terminal.
static bool isTerminal(JSGlobalObject* globalObject, JSValue file, int descriptor)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue number = callMethod(globalObject, file, internString(vm, "fileno"_s));
    if (scope.exception()) {
        // What ends the program stays: the caller looks.
        takePendingException(globalObject);
        return false;
    }
    return number.isInt32() && number.asInt32() == descriptor && isatty(descriptor);
}

static JSValue builtinInput(JSGlobalObject* globalObject, JSValue prompt)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto stream = [&](JSString* name, ASCIILiteral message) -> JSValue {
        JSValue file = rt.sysDict()->get(name);
        if (!file || isNone(file))
            return raiseRuntimeError(globalObject, scope, message);
        return file;
    };
    JSValue input = stream(rt.names.stdin_(), "lost sys.stdin"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue output = stream(rt.names.stdout_(), "lost sys.stdout"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue errors = stream(rt.names.stderr_(), "lost sys.stderr"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, audit(globalObject, "builtins.input"_s, prompt)))
        return { };

    callMethod(globalObject, errors, rt.names.flush());
    if (scope.exception())
        takePendingException(globalObject);
    bool editsLines = rt.modules()->get(internString(vm, "readline"_s)) && isTerminal(globalObject, input, STDIN_FILENO) && isTerminal(globalObject, output, STDOUT_FILENO);
    RETURN_IF_EXCEPTION(scope, { });
    if (editsLines) {
        callMethod(globalObject, output, rt.names.flush());
        if (scope.exception())
            takePendingException(globalObject);
        JSString* text = prompt ? str(globalObject, prompt) : jsEmptyString(vm);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue line = readLineWithEditor(globalObject, text);
        RETURN_IF_EXCEPTION(scope, { });
        if (line) {
            if (failed(scope, audit(globalObject, "builtins.input/result"_s, line)))
                return { };
            return line;
        }
    }
    if (prompt) {
        JSString* text = str(globalObject, prompt);
        RETURN_IF_EXCEPTION(scope, { });
        writeToFile(globalObject, output, text);
        RETURN_IF_EXCEPTION(scope, { });
    }
    callMethod(globalObject, output, rt.names.flush());
    if (scope.exception())
        takePendingException(globalObject);

    JSValue line = callMethod(globalObject, input, rt.names.readline());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(line))
        return raiseTypeError(globalObject, scope, "object.readline() returned non-string"_s);
    String text = strValue(line)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text.isEmpty())
        return raiseEOFError(globalObject, scope, "EOF when reading a line"_s);
    if (text.endsWith('\n'))
        text = text.left(text.length() - 1);
    return makeStr(vm, text);
}

static JSValue builtinBreakpoint(JSGlobalObject* globalObject, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue hook = rt.sysDict()->get(internString(vm, "breakpointhook"_s));
    if (!hook)
        return raiseRuntimeError(globalObject, scope, "lost sys.breakpointhook"_s);
    if (failed(scope, audit(globalObject, "builtins.breakpoint"_s, hook)))
        return { };
    RELEASE_AND_RETURN(scope, call(globalObject, hook, args));
}

// ---- Code ----

// The __future__ features in effect in the code that called the function that is running.
static int inheritedCompileFlags(JSGlobalObject* globalObject)
{
    PyFrame* frame = currentFrame(globalObject);
    if (!frame)
        return 0;
    auto flags = frame->codeInfo().flags;
    return static_cast<int>((flags & OptionSet<CodeFlag> { CodeFlag::FutureAnnotations, CodeFlag::FutureBarryAsBDFL }).toRaw());
}

bool sourceBytes(JSGlobalObject* globalObject, JSValue source, ASCIILiteral function, ASCIILiteral what, Vector<uint8_t>& bytes, int& flags)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(source)) {
        String text = strValue(source)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        auto encoded = tryEncodeUTF8(text);
        if (!encoded) {
            // As str.encode() would complain.
            strEncode(globalObject, strValue(source), nullptr, nullptr);
            ASSERT(scope.exception());
            return false;
        }
        bytes = WTF::move(*encoded);
        flags |= CompileIgnoreCookie;
    } else if (isBytesLike(source)) {
        std::span<const uint8_t> span;
        if (failed(scope, getBytesLike(globalObject, source, span)))
            return false;
        bytes.append(span);
    } else {
        raiseTypeError(globalObject, scope, function, "() arg 1 must be a "_s, what, " object"_s);
        return false;
    }
    if (bytes.contains(0)) {
        raise(globalObject, scope, rt.SyntaxError(), "source code string cannot contain null bytes"_s);
        return false;
    }
    return true;
}

String filenameArgument(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value) && !PyBytes::is(value)) {
        JSValue method = rt.typeOf(value)->lookup(Special::FsPath);
        if (!method) {
            raiseTypeError(globalObject, scope, "expected str, bytes or os.PathLike object, not "_s, typeName(globalObject, value));
            return { };
        }
        PyType* type = rt.typeOf(value);
        value = callSpecial(globalObject, method, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isStr(value) && !PyBytes::is(value)) {
            raiseTypeError(globalObject, scope, "expected "_s, type->nameInMessages(), ".__fspath__() to return str or bytes, not "_s, typeName(globalObject, value));
            return { };
        }
    }
    if (auto* bytes = PyBytes::tryFrom(value)) {
        if (std::ranges::contains(bytes->span(), 0)) {
            raiseValueError(globalObject, scope, "embedded null byte"_s);
            return { };
        }
        return decodeFilesystem(bytes->span());
    }
    String text = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return { };
    }
    return text;
}

static JSValue builtinCompile(JSGlobalObject* globalObject, JSValue source, JSValue filenameValue, JSValue modeValue, JSValue flagsValue, JSValue dontInherit, JSValue optimizeValue, JSValue featureVersionValue, JSValue module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String filename = filenameArgument(globalObject, filenameValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(modeValue))
        return raiseTypeError(globalObject, scope, "compile() argument 'mode' must be str, not "_s, typeNameOfArgument(globalObject, modeValue));
    String mode = strValue(modeValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    int flags = flagsValue ? toCInt(globalObject, flagsValue) : 0;
    RETURN_IF_EXCEPTION(scope, { });
    bool inherits = !dontInherit || !isTrue(globalObject, dontInherit);
    RETURN_IF_EXCEPTION(scope, { });
    int optimize = optimizeValue ? toCInt(globalObject, optimizeValue) : -1;
    RETURN_IF_EXCEPTION(scope, { });
    int featureVersion = featureVersionValue ? toCInt(globalObject, featureVersionValue) : -1;
    RETURN_IF_EXCEPTION(scope, { });

    constexpr int knownFlags = CompileSourceIsUTF8 | CompileDontImplyDedent | CompileOnlyAST | CompileIgnoreCookie | CompileTypeComments | CompileAllowTopLevelAwait | CompileAllowIncompleteInput | CompileOptimizedAST
        | 0x1fe0000 /* __future__ features, most of which are always on */ | 0x10 /* nested_scopes, from before there were such flags */;
    if (flags & ~knownFlags)
        return raiseValueError(globalObject, scope, "compile(): unrecognised flags"_s);
    if (optimize < -1 || optimize > 2)
        return raiseValueError(globalObject, scope, "compile(): invalid optimize value"_s);
    if (module && !isNone(module) && !isStr(module))
        return raiseTypeError(globalObject, scope, "compile() argument 'module' must be str or None, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, module)));
    if (inherits)
        flags |= inheritedCompileFlags(globalObject);

    ASCIILiteral compileMode;
    if (mode == "exec"_s)
        compileMode = "exec"_s;
    else if (mode == "eval"_s)
        compileMode = "eval"_s;
    else if (mode == "single"_s)
        compileMode = "single"_s;
    else if (mode == "func_type"_s) {
        if (!(flags & CompileOnlyAST))
            return raiseValueError(globalObject, scope, "compile() mode 'func_type' requires flag PyCF_ONLY_AST"_s);
        compileMode = "func_type"_s;
    } else
        return raiseValueError(globalObject, scope, flags & CompileOnlyAST ? "compile() mode must be 'exec', 'eval', 'single' or 'func_type'"_s : "compile() mode must be 'exec', 'eval' or 'single'"_s);

    if (PyCode::is(source))
        return source;
    bool sourceIsTree = isAST(globalObject, source);
    RETURN_IF_EXCEPTION(scope, { });
    if (sourceIsTree) {
        if (failed(scope, audit(globalObject, "compile"_s, source, jsUndefined())))
            return { };
        RELEASE_AND_RETURN(scope, compileAST(globalObject, source, filename, compileMode, flags, optimize, module));
    }

    Vector<uint8_t> bytes;
    if (failed(scope, sourceBytes(globalObject, source, "compile"_s, "string, bytes or AST"_s, bytes, flags)))
        return { };
    if (isAudited(globalObject) && failed(scope, audit(globalObject, "compile"_s, PyBytes::create(globalObject, bytes.span()), makeStr(vm, filename))))
        return { };
    if (flags & CompileOnlyAST)
        RELEASE_AND_RETURN(scope, parseToAST(globalObject, bytes.span(), filename, compileMode, flags, optimize, featureVersion, module));
    RELEASE_AND_RETURN(scope, compileSource(globalObject, bytes.span(), filename, compileMode, flags, optimize, module));
}

// What eval() and exec() have in common. `closure` is for exec() only.
static JSValue evaluate(JSGlobalObject* globalObject, bool isEval, JSValue source, JSValue globalsValue, JSValue locals, JSValue closure)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (globalsValue && isNone(globalsValue))
        globalsValue = JSValue();
    if (locals && isNone(locals))
        locals = JSValue();
    if (closure && isNone(closure))
        closure = JSValue();

    // Anything that can be subscripted, a list too.
    auto isMapping = [&](JSValue value) { return !!rt.typeOf(value)->lookup(Special::GetItem); };
    if (isEval) {
        if (locals && !isMapping(locals))
            return raiseTypeError(globalObject, scope, "locals must be a mapping"_s);
        if (globalsValue && !PyDict::is(globalsValue))
            return raiseTypeError(globalObject, scope, isMapping(globalsValue) ? "globals must be a real dict or a frozendict; try eval(expr, {}, mapping)"_s : "globals must be a dict or a frozendict"_s);
    } else {
        if (globalsValue && !PyDict::is(globalsValue))
            return raiseTypeError(globalObject, scope, "exec() globals must be a dict or a frozendict, not "_s, typeNameForMessage(globalObject, rt.typeOf(globalsValue)));
        if (locals && !isMapping(locals))
            return raiseTypeError(globalObject, scope, "locals must be a mapping or None, not "_s, typeName(globalObject, locals));
    }

    PyDict* globals;
    if (globalsValue) {
        globals = PyDict::from(globalsValue);
        if (!locals)
            locals = globals;
    } else {
        globals = currentGlobals(globalObject);
        if (!globals)
            return raiseTypeError(globalObject, scope, isEval ? "eval must be given globals and locals when called without a frame"_s : "globals and locals cannot be NULL"_s);
        if (!locals) {
            locals = currentLocals(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    if (failed(scope, ensureBuiltins(globalObject, globals)))
        return { };

    JSValue code = source;
    if (auto* codeObject = PyCode::tryFrom(source)) {
        size_t freeCount = codeObject->codeInfo().freeVariables.size();
        if (isEval) {
            if (freeCount)
                return raiseTypeError(globalObject, scope, "code object passed to eval() may not contain free variables"_s);
        } else if (!freeCount) {
            if (closure)
                return raiseTypeError(globalObject, scope, "cannot use a closure with this code object"_s);
        } else {
            auto* tuple = closure ? PyTuple::tryFrom(closure) : nullptr;
            bool isValid = tuple && tuple->length() == freeCount;
            for (unsigned i = 0; isValid && i < tuple->length(); ++i)
                isValid = PyCell::is(tuple->at(i));
            if (!isValid)
                return raiseTypeError(globalObject, scope, "code object requires a closure of exactly length "_s, freeCount);
        }
    } else {
        if (closure)
            return raiseTypeError(globalObject, scope, "closure can only be used when source is a code object"_s);
        Vector<uint8_t> bytes;
        int flags = CompileSourceIsUTF8 | inheritedCompileFlags(globalObject);
        if (failed(scope, sourceBytes(globalObject, source, isEval ? "eval"_s : "exec"_s, "string, bytes or code"_s, bytes, flags)))
            return { };
        std::span<const uint8_t> text = bytes.span();
        if (isEval) {
            while (!text.empty() && (text[0] == ' ' || text[0] == '\t'))
                text = text.subspan(1);
        }
        if (isAudited(globalObject) && failed(scope, audit(globalObject, "compile"_s, PyBytes::create(globalObject, text), makeStr(vm, "<string>"_s))))
            return { };
        code = compileSource(globalObject, text, "<string>"_s, isEval ? "eval"_s : "exec"_s, flags, -1, globals->get(rt.names.dName()));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (failed(scope, audit(globalObject, "exec"_s, code)))
        return { };
    RELEASE_AND_RETURN(scope, runCode(globalObject, code, globals, locals, closure ? PyTuple::from(closure) : nullptr));
}

static JSValue builtinEval(JSGlobalObject* globalObject, JSValue source, JSValue globals, JSValue locals)
{
    return evaluate(globalObject, true, source, globals, locals, { });
}

static JSValue builtinExec(JSGlobalObject* globalObject, JSValue source, JSValue globals, JSValue locals, JSValue closure)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    evaluate(globalObject, false, source, globals, locals, closure);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue builtinImport(JSGlobalObject* globalObject, JSValue name, JSValue globals, JSValue locals, JSValue fromList, JSValue levelValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int level = levelValue ? toCInt(globalObject, levelValue) : 0;
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "module name must be a string"_s);
    RELEASE_AND_RETURN(scope, importName(globalObject, strValue(name), globals ? globals : jsUndefined(), locals ? locals : jsUndefined(), fromList ? fromList : jsUndefined(), level));
}

static JSValue builtinLazyImport(JSGlobalObject* globalObject, JSValue name, JSValue globals, JSValue locals, JSValue fromList, JSValue levelValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int level = levelValue ? toCInt(globalObject, levelValue) : 0;
    RETURN_IF_EXCEPTION(scope, { });
    if (!globals) {
        PyDict* current = currentGlobals(globalObject);
        if (!current)
            return raiseTypeError(globalObject, scope, "__lazy_import__() missing globals when called without a frame"_s);
        globals = current;
    }
    if (!locals)
        locals = globals;
    if (!isDict(globals))
        return raiseTypeError(globalObject, scope, "expect dict for globals, got "_s, typeNameForMessage(globalObject, rt.typeOf(globals)));
    JSValue builtins = PyDict::from(globals)->get(rt.names.dBuiltins());
    if (!builtins)
        return raiseValueError(globalObject, scope, "unable to get builtins for lazy import"_s);
    if (auto* module = PyModule::tryFrom(builtins))
        builtins = module->dict();
    RELEASE_AND_RETURN(scope, lazyImportName(globalObject, name, builtins, globals, locals, fromList, level));
}

static JSValue builtinBuildClass(JSGlobalObject* globalObject, const CallArgs& args) { return buildClass(globalObject, args); }

static constexpr NativeDef builtinFunctions[] = {
    PY_FUNCTION("__build_class__", builtinBuildClass, "(*args, **kwargs)"),
    PY_FUNCTION("__import__", builtinImport, "(name, globals=None, locals=None, fromlist=(), level=0)"),
    PY_FUNCTION("__lazy_import__", builtinLazyImport, "(name, globals=None, locals=None, fromlist=(), level=0)"),
    PY_FUNCTION("abs", builtinAbs, "(number, /)"),
    PY_FUNCTION("aiter", builtinAIter, "(async_iterable, /)"),
    PY_FUNCTION_WITH_FLAGS("all", builtinAll, "(iterable, /)", ClosesGeneratorArgument),
    PY_FUNCTION("anext", builtinANext, "(async_iterator, default=<unrepresentable>, /)"),
    PY_FUNCTION_WITH_FLAGS("any", builtinAny, "(iterable, /)", ClosesGeneratorArgument),
    PY_FUNCTION("ascii", builtinAscii, "(obj, /)"),
    PY_FUNCTION("bin", builtinBin, "(integer, /)"),
    PY_FUNCTION("breakpoint", builtinBreakpoint, "(*args, **kws)"),
    PY_FUNCTION("callable", builtinCallable, "(obj, /)"),
    PY_FUNCTION("chr", builtinChr, "(i, /)"),
    PY_FUNCTION("compile", builtinCompile, "(source, filename, mode, flags=0, dont_inherit=False, optimize=-1, *, _feature_version=-1, module=None)"),
    PY_FUNCTION("delattr", builtinDelAttr, "(obj, name, /)"),
    PY_FUNCTION("dir", builtinDir, "(*args)"),
    PY_FUNCTION("divmod", builtinDivmod, "(x, y, /)"),
    PY_FUNCTION("eval", builtinEval, "(source, /, globals=None, locals=None)"),
    PY_FUNCTION("exec", builtinExec, "(source, /, globals=None, locals=None, *, closure=None)"),
    PY_FUNCTION("format", builtinFormat, "(value, format_spec='', /)"),
    PY_FUNCTION("getattr", builtinGetAttr, "(*args)"),
    PY_FUNCTION("globals", builtinGlobals, "()"),
    PY_FUNCTION("hasattr", builtinHasAttr, "(obj, name, /)"),
    PY_FUNCTION("hash", builtinHash, "(obj, /)"),
    PY_FUNCTION("hex", builtinHex, "(integer, /)"),
    PY_FUNCTION("id", builtinId, "(obj, /)"),
    PY_FUNCTION("input", builtinInput, "(prompt='', /)"),
    PY_FUNCTION("isinstance", builtinIsInstance, "(obj, class_or_tuple, /)"),
    PY_FUNCTION("issubclass", builtinIsSubclass, "(cls, class_or_tuple, /)"),
    PY_FUNCTION("iter", builtinIter, "(*args)"),
    PY_FUNCTION("len", builtinLen, "(obj, /)"),
    PY_FUNCTION("locals", builtinLocals, "()"),
    PY_FUNCTION_WITH_FLAGS("max", minOrMax<CompareOperator::Gt>, "(*args, **kwargs)", ClosesGeneratorArgument),
    PY_FUNCTION_WITH_FLAGS("min", minOrMax<CompareOperator::Lt>, "(*args, **kwargs)", ClosesGeneratorArgument),
    PY_FUNCTION_WITH_FLAGS("next", builtinNext, "(*args)", ClosesGeneratorArgument),
    PY_FUNCTION("oct", builtinOct, "(integer, /)"),
    PY_FUNCTION("ord", builtinOrd, "(character, /)"),
    PY_FUNCTION("pow", builtinPow, "(base, exp, mod=None)"),
    PY_FUNCTION("print", builtinPrint, "(*objects, sep=' ', end='\\n', file=None, flush=False)"),
    PY_FUNCTION("repr", builtinRepr, "(obj, /)"),
    PY_FUNCTION("round", builtinRound, "(number, ndigits=None)"),
    PY_FUNCTION("setattr", builtinSetAttr, "(obj, name, value, /)"),
    PY_FUNCTION("sorted", builtinSorted, "!(iterable, /, *, key=None, reverse=False)"),
    PY_FUNCTION_WITH_FLAGS("sum", builtinSum, "(iterable, /, start=0)", ClosesGeneratorArgument),
    PY_FUNCTION("vars", builtinVars, "(*args)"),
};

void initializeBuiltinsModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->addFunctions(globalObject, builtinFunctions);
    rt.setDefaultImportFunction(module->dict()->get(rt.names.dImport()));
    rt.setSlot(Runtime::Slot::BuiltinAll, module->dict()->get(internString(rt.vm(), "all"_s)));
    rt.setSlot(Runtime::Slot::BuiltinAny, module->dict()->get(internString(rt.vm(), "any"_s)));
    module->add(globalObject, "__doc__"_s, docstringFor(globalObject, "module builtins"_s));
    module->add(globalObject, "__package__"_s, rt.names.empty());

    module->add(globalObject, "None"_s, jsUndefined());
    module->add(globalObject, "Ellipsis"_s, rt.ellipsis());
    module->add(globalObject, "NotImplemented"_s, rt.notImplemented());
    module->add(globalObject, "False"_s, jsBoolean(false));
    module->add(globalObject, "True"_s, jsBoolean(true));
    module->add(globalObject, "__debug__"_s, jsBoolean(!rt.optimizationLevel));

    module->add(globalObject, "bool"_s, rt.boolType());
    module->add(globalObject, "bytearray"_s, rt.bytearrayType());
    module->add(globalObject, "bytes"_s, rt.bytesType());
    module->add(globalObject, "classmethod"_s, rt.classmethodType());
    module->add(globalObject, "complex"_s, rt.complexType());
    module->add(globalObject, "dict"_s, rt.dictType());
    module->add(globalObject, "enumerate"_s, rt.enumerateType());
    module->add(globalObject, "filter"_s, rt.filterType());
    module->add(globalObject, "float"_s, rt.floatType());
    module->add(globalObject, "frozendict"_s, rt.frozendictType());
    module->add(globalObject, "frozenset"_s, rt.frozensetType());
    module->add(globalObject, "property"_s, rt.propertyType());
    module->add(globalObject, "int"_s, rt.intType());
    module->add(globalObject, "list"_s, rt.listType());
    module->add(globalObject, "map"_s, rt.mapType());
    module->add(globalObject, "memoryview"_s, rt.memoryviewType());
    module->add(globalObject, "object"_s, rt.objectType());
    module->add(globalObject, "range"_s, rt.rangeType());
    module->add(globalObject, "reversed"_s, rt.reversedType());
    module->add(globalObject, "sentinel"_s, rt.sentinelType());
    module->add(globalObject, "set"_s, rt.setType());
    module->add(globalObject, "slice"_s, rt.sliceType());
    module->add(globalObject, "staticmethod"_s, rt.staticmethodType());
    module->add(globalObject, "str"_s, rt.strType());
    module->add(globalObject, "super"_s, rt.superType());
    module->add(globalObject, "tuple"_s, rt.tupleType());
    module->add(globalObject, "type"_s, rt.typeType());
    module->add(globalObject, "zip"_s, rt.zipType());

#define PY_ADD_EXCEPTION(id, base) module->add(globalObject, #id ""_s, rt.id());
    FOR_EACH_PY_EXCEPTION(PY_ADD_EXCEPTION)
#undef PY_ADD_EXCEPTION
    VM& vm = rt.vm();
    module->dict()->remove(vm, internString(vm, "IncompleteInputError"_s));
    module->dict()->remove(vm, internString(vm, "JSError"_s)); // It is in the module _js.
    module->add(globalObject, "_IncompleteInputError"_s, rt.IncompleteInputError());
    module->add(globalObject, "EnvironmentError"_s, rt.OSError());
    module->add(globalObject, "IOError"_s, rt.OSError());

    // open() is that of the module _io, which is imported from the start, as in CPython.
    JSValue io = importModule(globalObject, "_io"_s);
    RELEASE_ASSERT(io);
    module->add(globalObject, "open"_s, PyModule::from(io)->dict()->get(internString(vm, "open"_s)));
}

} // namespace Python
} // namespace JSC
