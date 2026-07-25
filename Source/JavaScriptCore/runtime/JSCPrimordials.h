/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

#pragma once

#if USE(BUN_JSC_ADDITIONS)

namespace JSC {

// Primordials are tamper-proof references to original ECMAScript built-in
// functions, exposed to builtin JavaScript as link-time constants following
// Node.js's `primordials` naming convention so internal modules ported from
// Node keep their call sites.
//
// Builtin JavaScript calls them in JSC style:
//
//     @ArrayPrototypePush.@call(array, value);   // prototype method
//     @ObjectDefineProperty(obj, key, desc);     // static
//     @MapPrototypeGetSize.@call(map);           // accessor getter
//
// Each primordial is captured into `m_linkTimeConstants` when its holder
// object is created (eager holders during JSGlobalObject::init(), lazy ones
// inside their own LazyClassStructure/LazyProperty/PropertyCallback bodies),
// so capture is exactly as lazy as the holder itself and always happens
// before user code can observe or mutate the holder.
//
// Per-holder tables are defined as
//
//     JSC_FOREACH_PRIMORDIAL_<Holder>(V)
//         V(name, key, kind)
//
// where `kind` controls how the value is read from the holder:
//     Method        key is an ASCIILiteral; value is a data property
//     Getter        key is an ASCIILiteral; value is the accessor's getter
//     SymbolMethod  key names a well-known symbol (vm.propertyNames-><key>Symbol)
//     SymbolGetter  key names a well-known symbol; value is the accessor's getter
//
// JSC_FOREACH_PRIMORDIAL_NAME(V) is the union used to declare link-time
// constants and private names. JSC_FOREACH_PRIMORDIAL_HOLDER(H) drives the
// capture dispatch in JSGlobalObject::capturePrimordials.
//
// Empty per-holder macros exist where a lazy-type macro expansion references
// a holder with no primordials of its own.

// ---------------------------------------------------------------------------
// Eager holders (created directly in JSGlobalObject::init())
// ---------------------------------------------------------------------------

#define JSC_FOREACH_PRIMORDIAL_ObjectPrototype(V) \
    V(ObjectPrototypeHasOwnProperty,       "hasOwnProperty",       Method) \
    V(ObjectPrototypeIsPrototypeOf,        "isPrototypeOf",        Method) \
    V(ObjectPrototypePropertyIsEnumerable, "propertyIsEnumerable", Method) \
    V(ObjectPrototypeToLocaleString,       "toLocaleString",       Method) \
    V(ObjectPrototypeToString,             "toString",             Method) \
    V(ObjectPrototypeValueOf,              "valueOf",              Method) \
    V(ObjectPrototypeLookupGetter,         "__lookupGetter__",     Method) \
    V(ObjectPrototypeLookupSetter,         "__lookupSetter__",     Method) \
    V(ObjectPrototypeDefineGetter,         "__defineGetter__",     Method) \
    V(ObjectPrototypeDefineSetter,         "__defineSetter__",     Method) \

#define JSC_FOREACH_PRIMORDIAL_ObjectConstructor(V) \
    V(ObjectAssign,                    "assign",                    Method) \
    V(ObjectCreate,                    "create",                    Method) \
    V(ObjectDefineProperties,          "defineProperties",          Method) \
    V(ObjectDefineProperty,            "defineProperty",            Method) \
    V(ObjectEntries,                   "entries",                   Method) \
    V(ObjectFreeze,                    "freeze",                    Method) \
    V(ObjectFromEntries,               "fromEntries",               Method) \
    V(ObjectGetOwnPropertyDescriptor,  "getOwnPropertyDescriptor",  Method) \
    V(ObjectGetOwnPropertyDescriptors, "getOwnPropertyDescriptors", Method) \
    V(ObjectGetOwnPropertyNames,       "getOwnPropertyNames",       Method) \
    V(ObjectGetOwnPropertySymbols,     "getOwnPropertySymbols",     Method) \
    V(ObjectGetPrototypeOf,            "getPrototypeOf",            Method) \
    V(ObjectGroupBy,                   "groupBy",                   Method) \
    V(ObjectHasOwn,                    "hasOwn",                    Method) \
    V(ObjectIs,                        "is",                        Method) \
    V(ObjectIsExtensible,              "isExtensible",              Method) \
    V(ObjectIsFrozen,                  "isFrozen",                  Method) \
    V(ObjectIsSealed,                  "isSealed",                  Method) \
    V(ObjectKeys,                      "keys",                      Method) \
    V(ObjectPreventExtensions,         "preventExtensions",         Method) \
    V(ObjectSeal,                      "seal",                      Method) \
    V(ObjectSetPrototypeOf,            "setPrototypeOf",            Method) \
    V(ObjectValues,                    "values",                    Method) \

#define JSC_FOREACH_PRIMORDIAL_FunctionPrototype(V) \
    V(FunctionPrototypeApply,             "apply",    Method) \
    V(FunctionPrototypeBind,              "bind",     Method) \
    V(FunctionPrototypeCall,              "call",     Method) \
    V(FunctionPrototypeToString,          "toString", Method) \
    V(FunctionPrototypeSymbolHasInstance, hasInstance, SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_ArrayPrototype(V) \
    V(ArrayPrototypeAt,             "at",             Method) \
    V(ArrayPrototypeConcat,         "concat",         Method) \
    V(ArrayPrototypeCopyWithin,     "copyWithin",     Method) \
    V(ArrayPrototypeEntries,        "entries",        Method) \
    V(ArrayPrototypeEvery,          "every",          Method) \
    V(ArrayPrototypeFill,           "fill",           Method) \
    V(ArrayPrototypeFilter,         "filter",         Method) \
    V(ArrayPrototypeFind,           "find",           Method) \
    V(ArrayPrototypeFindIndex,      "findIndex",      Method) \
    V(ArrayPrototypeFindLast,       "findLast",       Method) \
    V(ArrayPrototypeFindLastIndex,  "findLastIndex",  Method) \
    V(ArrayPrototypeFlat,           "flat",           Method) \
    V(ArrayPrototypeFlatMap,        "flatMap",        Method) \
    V(ArrayPrototypeForEach,        "forEach",        Method) \
    V(ArrayPrototypeIncludes,       "includes",       Method) \
    V(ArrayPrototypeIndexOf,        "indexOf",        Method) \
    V(ArrayPrototypeJoin,           "join",           Method) \
    V(ArrayPrototypeKeys,           "keys",           Method) \
    V(ArrayPrototypeLastIndexOf,    "lastIndexOf",    Method) \
    V(ArrayPrototypeMap,            "map",            Method) \
    V(ArrayPrototypePop,            "pop",            Method) \
    V(ArrayPrototypePush,           "push",           Method) \
    V(ArrayPrototypeReduce,         "reduce",         Method) \
    V(ArrayPrototypeReduceRight,    "reduceRight",    Method) \
    V(ArrayPrototypeReverse,        "reverse",        Method) \
    V(ArrayPrototypeShift,          "shift",          Method) \
    V(ArrayPrototypeSlice,          "slice",          Method) \
    V(ArrayPrototypeSome,           "some",           Method) \
    V(ArrayPrototypeSort,           "sort",           Method) \
    V(ArrayPrototypeSplice,         "splice",         Method) \
    V(ArrayPrototypeToLocaleString, "toLocaleString", Method) \
    V(ArrayPrototypeToReversed,     "toReversed",     Method) \
    V(ArrayPrototypeToSorted,       "toSorted",       Method) \
    V(ArrayPrototypeToSpliced,      "toSpliced",      Method) \
    V(ArrayPrototypeToString,       "toString",       Method) \
    V(ArrayPrototypeUnshift,        "unshift",        Method) \
    V(ArrayPrototypeValues,         "values",         Method) \
    V(ArrayPrototypeWith,           "with",           Method) \
    V(ArrayPrototypeSymbolIterator, iterator,         SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_ArrayConstructor(V) \
    V(ArrayFrom,      "from",      Method) \
    V(ArrayFromAsync, "fromAsync", Method) \
    V(ArrayIsArray,   "isArray",   Method) \
    V(ArrayOf,        "of",        Method) \

#define JSC_FOREACH_PRIMORDIAL_StringPrototype(V) \
    V(StringPrototypeAt,                "at",                Method) \
    V(StringPrototypeCharAt,            "charAt",            Method) \
    V(StringPrototypeCharCodeAt,        "charCodeAt",        Method) \
    V(StringPrototypeCodePointAt,       "codePointAt",       Method) \
    V(StringPrototypeConcat,            "concat",            Method) \
    V(StringPrototypeEndsWith,          "endsWith",          Method) \
    V(StringPrototypeIncludes,          "includes",          Method) \
    V(StringPrototypeIndexOf,           "indexOf",           Method) \
    V(StringPrototypeIsWellFormed,      "isWellFormed",      Method) \
    V(StringPrototypeLastIndexOf,       "lastIndexOf",       Method) \
    V(StringPrototypeLocaleCompare,     "localeCompare",     Method) \
    V(StringPrototypeMatch,             "match",             Method) \
    V(StringPrototypeMatchAll,          "matchAll",          Method) \
    V(StringPrototypeNormalize,         "normalize",         Method) \
    V(StringPrototypePadEnd,            "padEnd",            Method) \
    V(StringPrototypePadStart,          "padStart",          Method) \
    V(StringPrototypeRepeat,            "repeat",            Method) \
    V(StringPrototypeReplace,           "replace",           Method) \
    V(StringPrototypeReplaceAll,        "replaceAll",        Method) \
    V(StringPrototypeSearch,            "search",            Method) \
    V(StringPrototypeSlice,             "slice",             Method) \
    V(StringPrototypeSplit,             "split",             Method) \
    V(StringPrototypeStartsWith,        "startsWith",        Method) \
    V(StringPrototypeSubstr,            "substr",            Method) \
    V(StringPrototypeSubstring,         "substring",         Method) \
    V(StringPrototypeToLocaleLowerCase, "toLocaleLowerCase", Method) \
    V(StringPrototypeToLocaleUpperCase, "toLocaleUpperCase", Method) \
    V(StringPrototypeToLowerCase,       "toLowerCase",       Method) \
    V(StringPrototypeToString,          "toString",          Method) \
    V(StringPrototypeToUpperCase,       "toUpperCase",       Method) \
    V(StringPrototypeToWellFormed,      "toWellFormed",      Method) \
    V(StringPrototypeTrim,              "trim",              Method) \
    V(StringPrototypeTrimEnd,           "trimEnd",           Method) \
    V(StringPrototypeTrimStart,         "trimStart",         Method) \
    V(StringPrototypeValueOf,           "valueOf",           Method) \
    V(StringPrototypeSymbolIterator,    iterator,            SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_StringConstructor(V) \
    V(StringFromCharCode,  "fromCharCode",  Method) \
    V(StringFromCodePoint, "fromCodePoint", Method) \
    V(StringRaw,           "raw",           Method) \

#define JSC_FOREACH_PRIMORDIAL_RegExpPrototype(V) \
    V(RegExpPrototypeCompile,        "compile",     Method) \
    V(RegExpPrototypeExec,           "exec",        Method) \
    V(RegExpPrototypeTest,           "test",        Method) \
    V(RegExpPrototypeToString,       "toString",    Method) \
    V(RegExpPrototypeGetDotAll,      "dotAll",      Getter) \
    V(RegExpPrototypeGetFlags,       "flags",       Getter) \
    V(RegExpPrototypeGetGlobal,      "global",      Getter) \
    V(RegExpPrototypeGetHasIndices,  "hasIndices",  Getter) \
    V(RegExpPrototypeGetIgnoreCase,  "ignoreCase",  Getter) \
    V(RegExpPrototypeGetMultiline,   "multiline",   Getter) \
    V(RegExpPrototypeGetSource,      "source",      Getter) \
    V(RegExpPrototypeGetSticky,      "sticky",      Getter) \
    V(RegExpPrototypeGetUnicode,     "unicode",     Getter) \
    V(RegExpPrototypeGetUnicodeSets, "unicodeSets", Getter) \
    V(RegExpPrototypeSymbolMatch,    match,         SymbolMethod) \
    V(RegExpPrototypeSymbolMatchAll, matchAll,      SymbolMethod) \
    V(RegExpPrototypeSymbolReplace,  replace,       SymbolMethod) \
    V(RegExpPrototypeSymbolSearch,   search,        SymbolMethod) \
    V(RegExpPrototypeSymbolSplit,    split,         SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_SymbolPrototype(V) \
    V(SymbolPrototypeGetDescription,  "description", Getter) \
    V(SymbolPrototypeToString,        "toString",    Method) \
    V(SymbolPrototypeValueOf,         "valueOf",     Method) \
    V(SymbolPrototypeSymbolToPrimitive, toPrimitive, SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_SymbolConstructor(V) \
    V(SymbolFor,    "for",    Method) \
    V(SymbolKeyFor, "keyFor", Method) \

#define JSC_FOREACH_PRIMORDIAL_BigIntPrototype(V) \
    V(BigIntPrototypeToLocaleString, "toLocaleString", Method) \
    V(BigIntPrototypeToString,       "toString",       Method) \
    V(BigIntPrototypeValueOf,        "valueOf",        Method) \

#define JSC_FOREACH_PRIMORDIAL_BigIntConstructor(V) \
    V(BigIntAsIntN,  "asIntN",  Method) \
    V(BigIntAsUintN, "asUintN", Method) \

#define JSC_FOREACH_PRIMORDIAL_PromisePrototype(V) \
    V(PromisePrototypeCatch,   "catch",   Method) \
    V(PromisePrototypeFinally, "finally", Method) \
    V(PromisePrototypeThen,    "then",    Method) \

#define JSC_FOREACH_PRIMORDIAL_PromiseConstructor(V) \
    V(PromiseAll,           "all",           Method) \
    V(PromiseAllSettled,    "allSettled",    Method) \
    V(PromiseAny,           "any",           Method) \
    V(PromiseRace,          "race",          Method) \
    V(PromiseReject,        "reject",        Method) \
    V(PromiseResolve,       "resolve",       Method) \
    V(PromiseTry,           "try",           Method) \
    V(PromiseWithResolvers, "withResolvers", Method) \

#define JSC_FOREACH_PRIMORDIAL_IteratorPrototype(V) \
    V(IteratorPrototypeDrop,           "drop",    Method) \
    V(IteratorPrototypeEvery,          "every",   Method) \
    V(IteratorPrototypeFilter,         "filter",  Method) \
    V(IteratorPrototypeFind,           "find",    Method) \
    V(IteratorPrototypeFlatMap,        "flatMap", Method) \
    V(IteratorPrototypeForEach,        "forEach", Method) \
    V(IteratorPrototypeMap,            "map",     Method) \
    V(IteratorPrototypeReduce,         "reduce",  Method) \
    V(IteratorPrototypeSome,           "some",    Method) \
    V(IteratorPrototypeTake,           "take",    Method) \
    V(IteratorPrototypeToArray,        "toArray", Method) \
    V(IteratorPrototypeSymbolIterator, iterator,  SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_IteratorConstructor(V) \
    V(IteratorFrom, "from", Method) \

#define JSC_FOREACH_PRIMORDIAL_ArrayIteratorPrototype(V) \
    V(ArrayIteratorPrototypeNext, "next", Method) \

#define JSC_FOREACH_PRIMORDIAL_StringIteratorPrototype(V) \
    V(StringIteratorPrototypeNext, "next", Method) \

#define JSC_FOREACH_PRIMORDIAL_MapIteratorPrototype(V) \
    V(MapIteratorPrototypeNext, "next", Method) \

#define JSC_FOREACH_PRIMORDIAL_SetIteratorPrototype(V) \
    V(SetIteratorPrototypeNext, "next", Method) \

#define JSC_FOREACH_PRIMORDIAL_RegExpStringIteratorPrototype(V) \
    V(RegExpStringIteratorPrototypeNext, "next", Method) \

#define JSC_FOREACH_PRIMORDIAL_IteratorHelperPrototype(V) \
    V(IteratorHelperPrototypeNext,   "next",   Method) \
    V(IteratorHelperPrototypeReturn, "return", Method) \

#define JSC_FOREACH_PRIMORDIAL_WrapForValidIteratorPrototype(V) \
    V(WrapForValidIteratorPrototypeNext,   "next",   Method) \
    V(WrapForValidIteratorPrototypeReturn, "return", Method) \

#define JSC_FOREACH_PRIMORDIAL_AsyncIteratorPrototype(V) \
    V(AsyncIteratorPrototypeSymbolAsyncIterator, asyncIterator, SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_WeakRefPrototype(V) \
    V(WeakRefPrototypeDeref, "deref", Method) \

#define JSC_FOREACH_PRIMORDIAL_FinalizationRegistryPrototype(V) \
    V(FinalizationRegistryPrototypeRegister,   "register",   Method) \
    V(FinalizationRegistryPrototypeUnregister, "unregister", Method) \

#define JSC_FOREACH_PRIMORDIAL_GlobalFunctions(V) \
    V(globalDecodeURI,          "decodeURI",          Method) \
    V(globalDecodeURIComponent, "decodeURIComponent", Method) \
    V(globalEncodeURI,          "encodeURI",          Method) \
    V(globalEncodeURIComponent, "encodeURIComponent", Method) \
    V(globalEscape,             "escape",             Method) \
    V(globalUnescape,           "unescape",           Method) \

// ---------------------------------------------------------------------------
// FOR_EACH_LAZY_BUILTIN_TYPE holders
// ---------------------------------------------------------------------------

#define JSC_FOREACH_PRIMORDIAL_BooleanPrototype(V) \
    V(BooleanPrototypeToString, "toString", Method) \
    V(BooleanPrototypeValueOf,  "valueOf",  Method) \

#define JSC_FOREACH_PRIMORDIAL_BooleanConstructor(V)

#define JSC_FOREACH_PRIMORDIAL_DatePrototype(V) \
    V(DatePrototypeGetDate,            "getDate",            Method) \
    V(DatePrototypeGetDay,             "getDay",             Method) \
    V(DatePrototypeGetFullYear,        "getFullYear",        Method) \
    V(DatePrototypeGetHours,           "getHours",           Method) \
    V(DatePrototypeGetMilliseconds,    "getMilliseconds",    Method) \
    V(DatePrototypeGetMinutes,         "getMinutes",         Method) \
    V(DatePrototypeGetMonth,           "getMonth",           Method) \
    V(DatePrototypeGetSeconds,         "getSeconds",         Method) \
    V(DatePrototypeGetTime,            "getTime",            Method) \
    V(DatePrototypeGetTimezoneOffset,  "getTimezoneOffset",  Method) \
    V(DatePrototypeGetUTCDate,         "getUTCDate",         Method) \
    V(DatePrototypeGetUTCDay,          "getUTCDay",          Method) \
    V(DatePrototypeGetUTCFullYear,     "getUTCFullYear",     Method) \
    V(DatePrototypeGetUTCHours,        "getUTCHours",        Method) \
    V(DatePrototypeGetUTCMilliseconds, "getUTCMilliseconds", Method) \
    V(DatePrototypeGetUTCMinutes,      "getUTCMinutes",      Method) \
    V(DatePrototypeGetUTCMonth,        "getUTCMonth",        Method) \
    V(DatePrototypeGetUTCSeconds,      "getUTCSeconds",      Method) \
    V(DatePrototypeGetYear,            "getYear",            Method) \
    V(DatePrototypeSetDate,            "setDate",            Method) \
    V(DatePrototypeSetFullYear,        "setFullYear",        Method) \
    V(DatePrototypeSetHours,           "setHours",           Method) \
    V(DatePrototypeSetMilliseconds,    "setMilliseconds",    Method) \
    V(DatePrototypeSetMinutes,         "setMinutes",         Method) \
    V(DatePrototypeSetMonth,           "setMonth",           Method) \
    V(DatePrototypeSetSeconds,         "setSeconds",         Method) \
    V(DatePrototypeSetTime,            "setTime",            Method) \
    V(DatePrototypeSetUTCDate,         "setUTCDate",         Method) \
    V(DatePrototypeSetUTCFullYear,     "setUTCFullYear",     Method) \
    V(DatePrototypeSetUTCHours,        "setUTCHours",        Method) \
    V(DatePrototypeSetUTCMilliseconds, "setUTCMilliseconds", Method) \
    V(DatePrototypeSetUTCMinutes,      "setUTCMinutes",      Method) \
    V(DatePrototypeSetUTCMonth,        "setUTCMonth",        Method) \
    V(DatePrototypeSetUTCSeconds,      "setUTCSeconds",      Method) \
    V(DatePrototypeSetYear,            "setYear",            Method) \
    V(DatePrototypeToDateString,       "toDateString",       Method) \
    V(DatePrototypeToISOString,        "toISOString",        Method) \
    V(DatePrototypeToJSON,             "toJSON",             Method) \
    V(DatePrototypeToLocaleDateString, "toLocaleDateString", Method) \
    V(DatePrototypeToLocaleString,     "toLocaleString",     Method) \
    V(DatePrototypeToLocaleTimeString, "toLocaleTimeString", Method) \
    V(DatePrototypeToString,           "toString",           Method) \
    V(DatePrototypeToTimeString,       "toTimeString",       Method) \
    V(DatePrototypeToUTCString,        "toUTCString",        Method) \
    V(DatePrototypeValueOf,            "valueOf",            Method) \
    V(DatePrototypeSymbolToPrimitive,  toPrimitive,          SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_DateConstructor(V) \
    V(DateNow,   "now",   Method) \
    V(DateParse, "parse", Method) \
    V(DateUTC,   "UTC",   Method) \

#define JSC_FOREACH_PRIMORDIAL_ErrorPrototype(V) \
    V(ErrorPrototypeToString, "toString", Method) \

#define JSC_FOREACH_PRIMORDIAL_ErrorConstructor(V) \
    V(ErrorCaptureStackTrace, "captureStackTrace", Method) \
    V(ErrorIsError,           "isError",           Method) \

#define JSC_FOREACH_PRIMORDIAL_MapPrototype(V) \
    V(MapPrototypeClear,          "clear",   Method) \
    V(MapPrototypeDelete,         "delete",  Method) \
    V(MapPrototypeEntries,        "entries", Method) \
    V(MapPrototypeForEach,        "forEach", Method) \
    V(MapPrototypeGet,            "get",     Method) \
    V(MapPrototypeHas,            "has",     Method) \
    V(MapPrototypeKeys,           "keys",    Method) \
    V(MapPrototypeSet,            "set",     Method) \
    V(MapPrototypeValues,         "values",  Method) \
    V(MapPrototypeGetSize,        "size",    Getter) \
    V(MapPrototypeSymbolIterator, iterator,  SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_MapConstructor(V) \
    V(MapGroupBy, "groupBy", Method) \

#define JSC_FOREACH_PRIMORDIAL_NumberPrototype(V) \
    V(NumberPrototypeToExponential,  "toExponential",  Method) \
    V(NumberPrototypeToFixed,        "toFixed",        Method) \
    V(NumberPrototypeToLocaleString, "toLocaleString", Method) \
    V(NumberPrototypeToPrecision,    "toPrecision",    Method) \
    V(NumberPrototypeToString,       "toString",       Method) \
    V(NumberPrototypeValueOf,        "valueOf",        Method) \

#define JSC_FOREACH_PRIMORDIAL_NumberConstructor(V) \
    V(NumberIsFinite,      "isFinite",      Method) \
    V(NumberIsInteger,     "isInteger",     Method) \
    V(NumberIsNaN,         "isNaN",         Method) \
    V(NumberIsSafeInteger, "isSafeInteger", Method) \
    V(NumberParseFloat,    "parseFloat",    Method) \
    V(NumberParseInt,      "parseInt",      Method) \

#define JSC_FOREACH_PRIMORDIAL_SetPrototype(V) \
    V(SetPrototypeAdd,                 "add",                 Method) \
    V(SetPrototypeClear,               "clear",               Method) \
    V(SetPrototypeDelete,              "delete",              Method) \
    V(SetPrototypeDifference,          "difference",          Method) \
    V(SetPrototypeEntries,             "entries",             Method) \
    V(SetPrototypeForEach,             "forEach",             Method) \
    V(SetPrototypeHas,                 "has",                 Method) \
    V(SetPrototypeIntersection,        "intersection",        Method) \
    V(SetPrototypeIsDisjointFrom,      "isDisjointFrom",      Method) \
    V(SetPrototypeIsSubsetOf,          "isSubsetOf",          Method) \
    V(SetPrototypeIsSupersetOf,        "isSupersetOf",        Method) \
    V(SetPrototypeKeys,                "keys",                Method) \
    V(SetPrototypeSymmetricDifference, "symmetricDifference", Method) \
    V(SetPrototypeUnion,               "union",               Method) \
    V(SetPrototypeValues,              "values",              Method) \
    V(SetPrototypeGetSize,             "size",                Getter) \
    V(SetPrototypeSymbolIterator,      iterator,              SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_SetConstructor(V)

#define JSC_FOREACH_PRIMORDIAL_WeakMapPrototype(V) \
    V(WeakMapPrototypeDelete, "delete", Method) \
    V(WeakMapPrototypeGet,    "get",    Method) \
    V(WeakMapPrototypeHas,    "has",    Method) \
    V(WeakMapPrototypeSet,    "set",    Method) \

#define JSC_FOREACH_PRIMORDIAL_WeakMapConstructor(V)

#define JSC_FOREACH_PRIMORDIAL_WeakSetPrototype(V) \
    V(WeakSetPrototypeAdd,    "add",    Method) \
    V(WeakSetPrototypeDelete, "delete", Method) \
    V(WeakSetPrototypeHas,    "has",    Method) \

#define JSC_FOREACH_PRIMORDIAL_WeakSetConstructor(V)

#define JSC_FOREACH_PRIMORDIAL_JSArrayBufferPrototype(V) \
    V(ArrayBufferPrototypeSlice,                 "slice",                 Method) \
    V(ArrayBufferPrototypeResize,                "resize",                Method) \
    V(ArrayBufferPrototypeTransfer,              "transfer",              Method) \
    V(ArrayBufferPrototypeTransferToFixedLength, "transferToFixedLength", Method) \
    V(ArrayBufferPrototypeGetByteLength,         "byteLength",            Getter) \
    V(ArrayBufferPrototypeGetDetached,           "detached",              Getter) \
    V(ArrayBufferPrototypeGetMaxByteLength,      "maxByteLength",         Getter) \
    V(ArrayBufferPrototypeGetResizable,          "resizable",             Getter) \

#define JSC_FOREACH_PRIMORDIAL_JSArrayBufferConstructor(V) \
    V(ArrayBufferIsView, "isView", Method) \

// ---------------------------------------------------------------------------
// LazyProperty holders (%TypedArray%, DataView)
// ---------------------------------------------------------------------------

#define JSC_FOREACH_PRIMORDIAL_TypedArrayPrototype(V) \
    V(TypedArrayPrototypeAt,                   "at",             Method) \
    V(TypedArrayPrototypeCopyWithin,           "copyWithin",     Method) \
    V(TypedArrayPrototypeEntries,              "entries",        Method) \
    V(TypedArrayPrototypeEvery,                "every",          Method) \
    V(TypedArrayPrototypeFill,                 "fill",           Method) \
    V(TypedArrayPrototypeFilter,               "filter",         Method) \
    V(TypedArrayPrototypeFind,                 "find",           Method) \
    V(TypedArrayPrototypeFindIndex,            "findIndex",      Method) \
    V(TypedArrayPrototypeFindLast,             "findLast",       Method) \
    V(TypedArrayPrototypeFindLastIndex,        "findLastIndex",  Method) \
    V(TypedArrayPrototypeForEach,              "forEach",        Method) \
    V(TypedArrayPrototypeIncludes,             "includes",       Method) \
    V(TypedArrayPrototypeIndexOf,              "indexOf",        Method) \
    V(TypedArrayPrototypeJoin,                 "join",           Method) \
    V(TypedArrayPrototypeKeys,                 "keys",           Method) \
    V(TypedArrayPrototypeLastIndexOf,          "lastIndexOf",    Method) \
    V(TypedArrayPrototypeMap,                  "map",            Method) \
    V(TypedArrayPrototypeReduce,               "reduce",         Method) \
    V(TypedArrayPrototypeReduceRight,          "reduceRight",    Method) \
    V(TypedArrayPrototypeReverse,              "reverse",        Method) \
    V(TypedArrayPrototypeSet,                  "set",            Method) \
    V(TypedArrayPrototypeSlice,                "slice",          Method) \
    V(TypedArrayPrototypeSome,                 "some",           Method) \
    V(TypedArrayPrototypeSort,                 "sort",           Method) \
    V(TypedArrayPrototypeSubarray,             "subarray",       Method) \
    V(TypedArrayPrototypeToLocaleString,       "toLocaleString", Method) \
    V(TypedArrayPrototypeToReversed,           "toReversed",     Method) \
    V(TypedArrayPrototypeToSorted,             "toSorted",       Method) \
    V(TypedArrayPrototypeValues,               "values",         Method) \
    V(TypedArrayPrototypeWith,                 "with",           Method) \
    V(TypedArrayPrototypeGetBuffer,            "buffer",         Getter) \
    V(TypedArrayPrototypeGetByteLength,        "byteLength",     Getter) \
    V(TypedArrayPrototypeGetByteOffset,        "byteOffset",     Getter) \
    V(TypedArrayPrototypeGetLength,            "length",         Getter) \
    V(TypedArrayPrototypeGetSymbolToStringTag, toStringTag,      SymbolGetter) \
    V(TypedArrayPrototypeSymbolIterator,       iterator,         SymbolMethod) \

#define JSC_FOREACH_PRIMORDIAL_TypedArrayConstructor(V) \
    V(TypedArrayFrom, "from", Method) \
    V(TypedArrayOf,   "of",   Method) \

#define JSC_FOREACH_PRIMORDIAL_DataViewPrototype(V) \
    V(DataViewPrototypeGetBigInt64,   "getBigInt64",  Method) \
    V(DataViewPrototypeGetBigUint64,  "getBigUint64", Method) \
    V(DataViewPrototypeGetFloat16,    "getFloat16",   Method) \
    V(DataViewPrototypeGetFloat32,    "getFloat32",   Method) \
    V(DataViewPrototypeGetFloat64,    "getFloat64",   Method) \
    V(DataViewPrototypeGetInt16,      "getInt16",     Method) \
    V(DataViewPrototypeGetInt32,      "getInt32",     Method) \
    V(DataViewPrototypeGetInt8,       "getInt8",      Method) \
    V(DataViewPrototypeGetUint16,     "getUint16",    Method) \
    V(DataViewPrototypeGetUint32,     "getUint32",    Method) \
    V(DataViewPrototypeGetUint8,      "getUint8",     Method) \
    V(DataViewPrototypeSetBigInt64,   "setBigInt64",  Method) \
    V(DataViewPrototypeSetBigUint64,  "setBigUint64", Method) \
    V(DataViewPrototypeSetFloat16,    "setFloat16",   Method) \
    V(DataViewPrototypeSetFloat32,    "setFloat32",   Method) \
    V(DataViewPrototypeSetFloat64,    "setFloat64",   Method) \
    V(DataViewPrototypeSetInt16,      "setInt16",     Method) \
    V(DataViewPrototypeSetInt32,      "setInt32",     Method) \
    V(DataViewPrototypeSetInt8,       "setInt8",      Method) \
    V(DataViewPrototypeSetUint16,     "setUint16",    Method) \
    V(DataViewPrototypeSetUint32,     "setUint32",    Method) \
    V(DataViewPrototypeSetUint8,      "setUint8",     Method) \
    V(DataViewPrototypeGetBuffer,     "buffer",       Getter) \
    V(DataViewPrototypeGetByteLength, "byteLength",   Getter) \
    V(DataViewPrototypeGetByteOffset, "byteOffset",   Getter) \

// ---------------------------------------------------------------------------
// PropertyCallback holders (Math, JSON, Reflect, Atomics)
// ---------------------------------------------------------------------------

#define JSC_FOREACH_PRIMORDIAL_MathObject(V) \
    V(MathAbs,        "abs",        Method) \
    V(MathAcos,       "acos",       Method) \
    V(MathAcosh,      "acosh",      Method) \
    V(MathAsin,       "asin",       Method) \
    V(MathAsinh,      "asinh",      Method) \
    V(MathAtan,       "atan",       Method) \
    V(MathAtan2,      "atan2",      Method) \
    V(MathAtanh,      "atanh",      Method) \
    V(MathCbrt,       "cbrt",       Method) \
    V(MathCeil,       "ceil",       Method) \
    V(MathClz32,      "clz32",      Method) \
    V(MathCos,        "cos",        Method) \
    V(MathCosh,       "cosh",       Method) \
    V(MathExp,        "exp",        Method) \
    V(MathExpm1,      "expm1",      Method) \
    V(MathF16round,   "f16round",   Method) \
    V(MathFloor,      "floor",      Method) \
    V(MathFround,     "fround",     Method) \
    V(MathHypot,      "hypot",      Method) \
    V(MathImul,       "imul",       Method) \
    V(MathLog,        "log",        Method) \
    V(MathLog10,      "log10",      Method) \
    V(MathLog1p,      "log1p",      Method) \
    V(MathLog2,       "log2",       Method) \
    V(MathMax,        "max",        Method) \
    V(MathMin,        "min",        Method) \
    V(MathPow,        "pow",        Method) \
    V(MathRandom,     "random",     Method) \
    V(MathRound,      "round",      Method) \
    V(MathSign,       "sign",       Method) \
    V(MathSin,        "sin",        Method) \
    V(MathSinh,       "sinh",       Method) \
    V(MathSqrt,       "sqrt",       Method) \
    V(MathSumPrecise, "sumPrecise", Method) \
    V(MathTan,        "tan",        Method) \
    V(MathTanh,       "tanh",       Method) \
    V(MathTrunc,      "trunc",      Method) \

#define JSC_FOREACH_PRIMORDIAL_JSONObject(V) \
    V(JSONParse,     "parse",     Method) \
    V(JSONStringify, "stringify", Method) \
    V(JSONIsRawJSON, "isRawJSON", Method) \
    V(JSONRawJSON,   "rawJSON",   Method) \

#define JSC_FOREACH_PRIMORDIAL_ReflectObject(V) \
    V(ReflectApply,                    "apply",                    Method) \
    V(ReflectConstruct,                "construct",                Method) \
    V(ReflectDefineProperty,           "defineProperty",           Method) \
    V(ReflectDeleteProperty,           "deleteProperty",           Method) \
    V(ReflectGet,                      "get",                      Method) \
    V(ReflectGetOwnPropertyDescriptor, "getOwnPropertyDescriptor", Method) \
    V(ReflectGetPrototypeOf,           "getPrototypeOf",           Method) \
    V(ReflectHas,                      "has",                      Method) \
    V(ReflectIsExtensible,             "isExtensible",             Method) \
    V(ReflectOwnKeys,                  "ownKeys",                  Method) \
    V(ReflectPreventExtensions,        "preventExtensions",        Method) \
    V(ReflectSet,                      "set",                      Method) \
    V(ReflectSetPrototypeOf,           "setPrototypeOf",           Method) \

#define JSC_FOREACH_PRIMORDIAL_AtomicsObject(V) \
    V(AtomicsAdd,             "add",             Method) \
    V(AtomicsAnd,             "and",             Method) \
    V(AtomicsCompareExchange, "compareExchange", Method) \
    V(AtomicsExchange,        "exchange",        Method) \
    V(AtomicsIsLockFree,      "isLockFree",      Method) \
    V(AtomicsLoad,            "load",            Method) \
    V(AtomicsNotify,          "notify",          Method) \
    V(AtomicsOr,              "or",              Method) \
    V(AtomicsPause,           "pause",           Method) \
    V(AtomicsStore,           "store",           Method) \
    V(AtomicsSub,             "sub",             Method) \
    V(AtomicsWait,            "wait",            Method) \
    V(AtomicsWaitAsync,       "waitAsync",       Method) \
    V(AtomicsXor,             "xor",             Method) \

// ---------------------------------------------------------------------------
// Holder enumeration and combined name list
// ---------------------------------------------------------------------------

#define JSC_FOREACH_PRIMORDIAL_HOLDER(H) \
    H(ObjectPrototype) \
    H(ObjectConstructor) \
    H(FunctionPrototype) \
    H(ArrayPrototype) \
    H(ArrayConstructor) \
    H(StringPrototype) \
    H(StringConstructor) \
    H(RegExpPrototype) \
    H(SymbolPrototype) \
    H(SymbolConstructor) \
    H(BigIntPrototype) \
    H(BigIntConstructor) \
    H(PromisePrototype) \
    H(PromiseConstructor) \
    H(IteratorPrototype) \
    H(IteratorConstructor) \
    H(ArrayIteratorPrototype) \
    H(StringIteratorPrototype) \
    H(MapIteratorPrototype) \
    H(SetIteratorPrototype) \
    H(RegExpStringIteratorPrototype) \
    H(IteratorHelperPrototype) \
    H(WrapForValidIteratorPrototype) \
    H(AsyncIteratorPrototype) \
    H(WeakRefPrototype) \
    H(FinalizationRegistryPrototype) \
    H(GlobalFunctions) \
    H(BooleanPrototype) \
    H(BooleanConstructor) \
    H(DatePrototype) \
    H(DateConstructor) \
    H(ErrorPrototype) \
    H(ErrorConstructor) \
    H(MapPrototype) \
    H(MapConstructor) \
    H(NumberPrototype) \
    H(NumberConstructor) \
    H(SetPrototype) \
    H(SetConstructor) \
    H(WeakMapPrototype) \
    H(WeakMapConstructor) \
    H(WeakSetPrototype) \
    H(WeakSetConstructor) \
    H(JSArrayBufferPrototype) \
    H(JSArrayBufferConstructor) \
    H(TypedArrayPrototype) \
    H(TypedArrayConstructor) \
    H(DataViewPrototype) \
    H(MathObject) \
    H(JSONObject) \
    H(ReflectObject) \
    H(AtomicsObject) \

#define JSC_FOREACH_PRIMORDIAL_NAME(V) \
    JSC_FOREACH_PRIMORDIAL_ObjectPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ObjectConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_FunctionPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_StringPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_StringConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_RegExpPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SymbolPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SymbolConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_BigIntPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_BigIntConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_PromisePrototype(V) \
    JSC_FOREACH_PRIMORDIAL_PromiseConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_IteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_IteratorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_ArrayIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_StringIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_MapIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SetIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_RegExpStringIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_IteratorHelperPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WrapForValidIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_AsyncIteratorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WeakRefPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_FinalizationRegistryPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_GlobalFunctions(V) \
    JSC_FOREACH_PRIMORDIAL_BooleanPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_BooleanConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_DatePrototype(V) \
    JSC_FOREACH_PRIMORDIAL_DateConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_ErrorPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_ErrorConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_MapPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_MapConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_NumberPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_NumberConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_SetPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_SetConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_WeakMapPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WeakMapConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_WeakSetPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_WeakSetConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_JSArrayBufferPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_JSArrayBufferConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_TypedArrayPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_TypedArrayConstructor(V) \
    JSC_FOREACH_PRIMORDIAL_DataViewPrototype(V) \
    JSC_FOREACH_PRIMORDIAL_MathObject(V) \
    JSC_FOREACH_PRIMORDIAL_JSONObject(V) \
    JSC_FOREACH_PRIMORDIAL_ReflectObject(V) \
    JSC_FOREACH_PRIMORDIAL_AtomicsObject(V) \


enum class PrimordialHolder : uint8_t {
#define DECLARE_PRIMORDIAL_HOLDER(Holder) Holder,
    JSC_FOREACH_PRIMORDIAL_HOLDER(DECLARE_PRIMORDIAL_HOLDER)
#undef DECLARE_PRIMORDIAL_HOLDER
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
