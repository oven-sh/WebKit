/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTType.h"
#include "JSType.h"
#include <optional>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

struct BuiltinSignature {
    enum class Condition : uint8_t {
        Always,
        IfFirstArgumentIsNotObject,
        IfThisIsHolder,
        IsInt32IfArgumentsAre,
        IsInt32IfOnlyArgumentIs,
        IfReceiverIsOriginal,
    };
    Type result;
    Condition condition;
};
std::optional<BuiltinSignature> intrinsicSignature(unsigned number);

std::optional<Type> constructingIntrinsicResult(unsigned number);

unsigned intrinsicFoundOnPrimitive(Type receiver, const StringImpl& name);
bool isDataPropertyOfFunctionPrototype(const StringImpl& name);
bool keepsReceiverToItself(unsigned intrinsic);
bool returnsReceiver(unsigned intrinsic);
bool isReflectConstruct(unsigned intrinsic);

#define FOR_EACH_AOT_BUILTIN(v) \
    v(MathAbs, "Math.abs") v(MathFloor, "Math.floor") v(MathCeil, "Math.ceil") v(MathTrunc, "Math.trunc") v(MathRound, "Math.round") v(MathSqrt, "Math.sqrt") \
    v(MathFround, "Math.fround") v(MathMin, "Math.min") v(MathMax, "Math.max") v(MathClz32, "Math.clz32") v(MathImul, "Math.imul") v(MathSign, "Math.sign") \
    v(MathPow, "Math.pow") v(MathRandom, "Math.random") \
    v(MathSin, "Math.sin") v(MathCos, "Math.cos") v(MathTan, "Math.tan") v(MathAsin, "Math.asin") v(MathAcos, "Math.acos") v(MathAtan, "Math.atan") \
    v(MathSinh, "Math.sinh") v(MathCosh, "Math.cosh") v(MathTanh, "Math.tanh") v(MathAsinh, "Math.asinh") v(MathAcosh, "Math.acosh") v(MathAtanh, "Math.atanh") \
    v(MathLog, "Math.log") v(MathLog2, "Math.log2") v(MathLog10, "Math.log10") v(MathLog1p, "Math.log1p") v(MathExp, "Math.exp") v(MathExpm1, "Math.expm1") v(MathCbrt, "Math.cbrt") \
    v(MathAtan2, "Math.atan2") \
    v(NumberIsInteger, "Number.isInteger") v(NumberIsSafeInteger, "Number.isSafeInteger") v(NumberIsFinite, "Number.isFinite") v(NumberIsNaN, "Number.isNaN") \
    v(GlobalIsNaN, "isNaN") v(GlobalIsFinite, "isFinite") v(GlobalParseInt, "parseInt") v(NumberParseInt, "Number.parseInt") \
    v(NumberConstructor, "Number") v(BooleanConstructor, "Boolean") v(StringConstructor, "String") \
    v(ArrayIsArray, "Array.isArray") v(ObjectIs, "Object.is") v(ObjectKeys, "Object.keys") v(ObjectGetOwnPropertyNames, "Object.getOwnPropertyNames") \
    v(ObjectGetOwnPropertySymbols, "Object.getOwnPropertySymbols") v(ObjectGetPrototypeOf, "Object.getPrototypeOf") v(ReflectGetPrototypeOf, "Reflect.getPrototypeOf") \
    v(ObjectCreate, "Object.create") v(ObjectAssign, "Object.assign") v(ObjectHasOwn, "Object.hasOwn") v(ObjectPrototypeHasOwnProperty, "Object.prototype.hasOwnProperty") \
    v(StringFromCharCode, "String.fromCharCode") v(DateNow, "Date.now") \
    v(StringCharCodeAt, "String.prototype.charCodeAt") v(StringCharAt, "String.prototype.charAt") v(StringAt, "String.prototype.at") v(StringCodePointAt, "String.prototype.codePointAt") \
    v(StringValueOf, "String.prototype.valueOf") v(StringToString, "String.prototype.toString") \
    v(StringStartsWith, "String.prototype.startsWith") v(StringEndsWith, "String.prototype.endsWith") v(StringIncludes, "String.prototype.includes") \
    v(StringIndexOf, "String.prototype.indexOf") v(StringLastIndexOf, "String.prototype.lastIndexOf") \
    v(StringSlice, "String.prototype.slice") v(StringSubstring, "String.prototype.substring") \
    v(StringTrim, "String.prototype.trim") v(StringTrimStart, "String.prototype.trimStart") v(StringTrimEnd, "String.prototype.trimEnd") \
    v(StringToLowerCase, "String.prototype.toLowerCase") v(StringToUpperCase, "String.prototype.toUpperCase") v(StringLocaleCompare, "String.prototype.localeCompare") \
    v(StringReplace, "String.prototype.replace") v(StringReplaceAll, "String.prototype.replaceAll") v(StringSplit, "String.prototype.split") \
    v(StringMatch, "String.prototype.match") v(StringSearch, "String.prototype.search") v(StringConcat, "String.prototype.concat") \
    v(NumberToString, "Number.prototype.toString") \
    v(ArrayPush, "Array.prototype.push") v(ArrayPop, "Array.prototype.pop") v(ArrayShift, "Array.prototype.shift") v(ArrayUnshift, "Array.prototype.unshift") \
    v(ArrayJoin, "Array.prototype.join") v(ArrayIndexOf, "Array.prototype.indexOf") v(ArrayIncludes, "Array.prototype.includes") \
    v(ArraySlice, "Array.prototype.slice") v(ArraySplice, "Array.prototype.splice") v(ArrayConcat, "Array.prototype.concat") v(ArrayAt, "Array.prototype.at") \
    v(MapGet, "Map.prototype.get") v(MapHas, "Map.prototype.has") v(MapSet, "Map.prototype.set") v(MapDelete, "Map.prototype.delete") \
    v(SetHas, "Set.prototype.has") v(SetAdd, "Set.prototype.add") v(SetDelete, "Set.prototype.delete") \
    v(WeakMapGet, "WeakMap.prototype.get") v(WeakMapHas, "WeakMap.prototype.has") v(WeakMapSet, "WeakMap.prototype.set") v(WeakMapDelete, "WeakMap.prototype.delete") \
    v(WeakSetHas, "WeakSet.prototype.has") v(WeakSetAdd, "WeakSet.prototype.add") v(WeakSetDelete, "WeakSet.prototype.delete") \
    v(RegExpTest, "RegExp.prototype.test") v(RegExpExec, "RegExp.prototype.exec") \
    v(DateGetTime, "Date.prototype.getTime") v(DateValueOf, "Date.prototype.valueOf") \
    v(DateGetFullYear, "Date.prototype.getFullYear") v(DateGetMonth, "Date.prototype.getMonth") v(DateGetDate, "Date.prototype.getDate") v(DateGetDay, "Date.prototype.getDay") \
    v(DateGetHours, "Date.prototype.getHours") v(DateGetMinutes, "Date.prototype.getMinutes") v(DateGetSeconds, "Date.prototype.getSeconds") v(DateGetMilliseconds, "Date.prototype.getMilliseconds") \
    v(DateGetTimezoneOffset, "Date.prototype.getTimezoneOffset") \
    v(DateGetUTCFullYear, "Date.prototype.getUTCFullYear") v(DateGetUTCMonth, "Date.prototype.getUTCMonth") v(DateGetUTCDate, "Date.prototype.getUTCDate") v(DateGetUTCDay, "Date.prototype.getUTCDay") \
    v(DateGetUTCHours, "Date.prototype.getUTCHours") v(DateGetUTCMinutes, "Date.prototype.getUTCMinutes") v(DateGetUTCSeconds, "Date.prototype.getUTCSeconds") v(DateGetUTCMilliseconds, "Date.prototype.getUTCMilliseconds") \

enum class Builtin : uint8_t {
    None,
#define AOT_DEFINE_BUILTIN(name, path) name,
    FOR_EACH_AOT_BUILTIN(AOT_DEFINE_BUILTIN)
#undef AOT_DEFINE_BUILTIN
};
Builtin builtinAtIndex(unsigned intrinsic);

enum class Receiver : uint8_t { None, String, Array, Map, Set, WeakMap, WeakSet, RegExp, Date, Number };
Type typeOf(Receiver);
JSType cellTypeOf(Receiver);
unsigned intrinsicFoundOn(Receiver, const StringImpl& name);
Receiver receiverWithType(Type);
Receiver requiredReceiver(unsigned intrinsic);
Receiver likelyReceiverWith(Type, const StringImpl& name);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
