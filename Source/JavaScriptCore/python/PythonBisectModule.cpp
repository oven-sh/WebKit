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
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonSequences.h"

// The module _bisect: Modules/_bisectmodule.c of CPython.

namespace JSC { namespace Python {

namespace {

enum class Side : bool { Left, Right };

// PyObject_RichCompareBool(a, b, Py_LT). Nothing if it raised.
std::optional<bool> bisectIsLess(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = compare(globalObject, ComparisonOperator::Lt, a, b);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    bool truth = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return truth;
}

// internal_bisect_left() and internal_bisect_right(). `high` is -1 for as far as it goes. Nothing if it raised.
std::optional<int64_t> bisect(JSGlobalObject* globalObject, Side side, JSValue list, JSValue item, int64_t low, int64_t high, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (low < 0) {
        raiseValueError(globalObject, scope, "lo must be non-negative"_s);
        return std::nullopt;
    }
    if (high == -1) {
        high = sequenceSize(globalObject, list);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
    }
    checkIsIndexable(globalObject, list);
    RETURN_IF_EXCEPTION(scope, std::nullopt);

    // What is of the same class as what is looked for is compared by asking the class outright, and the other is not asked unless it has nothing to say. After that, it is not gone about that way again.
    PyType* type = typeOf(globalObject, item);
    bool asksTheClass = true;
    while (low < high) {
        // Without a sign, so that the sum is not too much.
        int64_t middle = static_cast<int64_t>((static_cast<uint64_t>(low) + static_cast<uint64_t>(high)) / 2);
        JSValue found = getItem(globalObject, list, intFromInt64(globalObject, middle));
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (!isNone(key)) {
            found = call(globalObject, key, found);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
        }
        // To the right: whether item < found. To the left: whether found < item.
        JSValue first = side == Side::Right ? item : found;
        JSValue second = side == Side::Right ? found : item;
        std::optional<bool> isLess;
        if (asksTheClass && typeOf(globalObject, found) == type) {
            JSValue self;
            JSValue method = lookupSpecial(globalObject, first, vm.pythonNames().dunder_lt, self);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            JSValue result = globalObject->pyRealm()->notImplemented();
            if (method) {
                result = callMethod(globalObject, method, self, second);
                RETURN_IF_EXCEPTION(scope, std::nullopt);
            }
            if (result == globalObject->pyRealm()->notImplemented()) {
                asksTheClass = false;
                isLess = bisectIsLess(globalObject, first, second);
            } else if (result.isBoolean())
                isLess = result.asBoolean();
            else
                isLess = isTrue(globalObject, result);
        } else
            isLess = bisectIsLess(globalObject, first, second);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (side == Side::Right ? *isLess : !*isLess)
            high = middle;
        else
            low = middle + 1;
    }
    return low;
}

struct BisectArguments {
    int64_t low { 0 };
    int64_t high { -1 };
    JSValue key { jsUndefined() };
};

// (a, x, lo=0, hi=None, *, key=None). Nothing if it raised.
std::optional<BisectArguments> bisectArguments(JSGlobalObject* globalObject, const NativeArguments& args)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    BisectArguments result;
    if (JSValue low = args.at(2)) {
        auto value = toSsize(globalObject, low);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        result.low = *value;
    }
    auto high = toOptionalSsize(globalObject, args.at(3), -1);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    result.high = *high;
    if (JSValue key = args.at(4))
        result.key = key;
    return result;
}

} // anonymous namespace

// bisect_left() and bisect_right()
PYTHON_NATIVE(bisectFind)
{
    auto side = unpack<Side>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto given = bisectArguments(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    auto index = bisect(globalObject, side, args.at(0), args.at(1), given->low, given->high, given->key);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(intFromInt64(globalObject, *index));
}

// insort_left() and insort_right()
PYTHON_NATIVE(bisectInsert)
{
    auto side = unpack<Side>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto given = bisectArguments(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue list = args.at(0);
    JSValue item = args.at(1);
    JSValue lookedFor = item;
    if (!isNone(given->key)) {
        lookedFor = call(globalObject, given->key, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto index = bisect(globalObject, side, list, lookedFor, given->low, given->high, given->key);
    RETURN_IF_EXCEPTION(scope, { });
    if (isExactly(globalObject, list, BuiltinType::List)) {
        JSArray* array = asList(list);
        listInsert(globalObject, array, static_cast<unsigned>(std::min<int64_t>(*index, array->length())), item);
        RETURN_IF_EXCEPTION(scope, { });
        RETURN_NONE();
    }
    callMethodNamed(globalObject, list, Identifier::fromString(vm, "insert"_s), intFromInt64(globalObject, *index), item);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

JSObject* createBisectModule(JSGlobalObject* globalObject)
{
    JSObject* module = newBuiltinModule(globalObject, "_bisect"_s);
    addFunction(globalObject, module, "bisect_right"_s, bisectFind, pack(Side::Right));
    addFunction(globalObject, module, "insort_right"_s, bisectInsert, pack(Side::Right));
    addFunction(globalObject, module, "bisect_left"_s, bisectFind, pack(Side::Left));
    addFunction(globalObject, module, "insort_left"_s, bisectInsert, pack(Side::Left));
    return module;
}

} } // namespace JSC::Python
