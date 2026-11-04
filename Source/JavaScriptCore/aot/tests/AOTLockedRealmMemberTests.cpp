/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTChildProcess.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTRealmTestSupport.h"
#include "AOTTemporaryFile.h"
#include "AOTVector.h"
#include "FunctionPrototype.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include "TypedArrayType.h"
#include <algorithm>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> lockedRealmMemberTests();

static constexpr ASCIILiteral programWithoutBuiltins =
    "# LIBRARY: own\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

namespace {

enum class ClosedProtocolNames : uint8_t { None, Then, OfObjects };

struct ObjectWithLockedMembers {
    ASCIILiteral source;
    ClosedProtocolNames closedProtocolNames;
    std::span<const ASCIILiteral> replaceableMembers { };
    std::span<const ASCIILiteral> openInheritedNames { };
    std::span<const ASCIILiteral> openProtocolNames { };
};

struct InstanceOfBuiltinClass {
    ASCIILiteral source;
    bool isPristine;
};

struct Statements {
    std::span<const ASCIILiteral> throwing;
    std::span<const ASCIILiteral> returningFalse;
    std::span<const ASCIILiteral> returningTrue;
};

constexpr ASCIILiteral replaceableStringMethods[] = {
    "at"_s, "anchor"_s, "big"_s, "blink"_s, "bold"_s, "fixed"_s, "fontcolor"_s, "fontsize"_s, "italics"_s, "link"_s, "small"_s, "strike"_s, "sub"_s, "sup"_s,
    "trim"_s, "trimEnd"_s, "trimLeft"_s, "trimRight"_s, "trimStart"_s,
};

constexpr ASCIILiteral replaceableSetMethods[] = {
    "union"_s, "intersection"_s, "difference"_s, "symmetricDifference"_s, "isSubsetOf"_s, "isSupersetOf"_s, "isDisjointFrom"_s,
};

constexpr ASCIILiteral replaceableMapMethods[] = { "getOrInsert"_s, "getOrInsertComputed"_s };
constexpr ASCIILiteral replaceableIteratorMethods[] = { "chunks"_s, "windows"_s };
constexpr ASCIILiteral replaceableUint8ArrayFunctions[] = { "fromBase64"_s, "fromHex"_s };
constexpr ASCIILiteral replaceableUint8ArrayMethods[] = { "toBase64"_s, "toHex"_s };
constexpr ASCIILiteral replaceablePromiseFunctions[] = { "try"_s };
constexpr ASCIILiteral replaceableNumberFunctions[] = { "parseFloat"_s, "parseInt"_s };
constexpr ASCIILiteral replaceableMapFunctions[] = { "groupBy"_s };
constexpr ASCIILiteral replaceableDateFunctions[] = { "now"_s, "parse"_s };
constexpr ASCIILiteral replaceableDateMethods[] = { "toString"_s, "toLocaleString"_s, "toLocaleDateString"_s, "toLocaleTimeString"_s };
constexpr ASCIILiteral openProtocolNamesOfDatePrototype[] = { "next"_s };
constexpr ASCIILiteral replaceableMathFunctions[] = { "random"_s, "sign"_s, "sinh"_s, "sumPrecise"_s };
constexpr ASCIILiteral replaceableJSONFunctions[] = { "parse"_s, "stringify"_s };

constexpr ASCIILiteral replaceableRegExpMembers[] = {
    "escape"_s, "input"_s, "$_"_s, "multiline"_s, "$*"_s, "lastMatch"_s, "$&"_s, "lastParen"_s, "$+"_s, "leftContext"_s, "$`"_s, "rightContext"_s, "$'"_s,
    "$1"_s, "$2"_s, "$3"_s, "$4"_s, "$5"_s, "$6"_s, "$7"_s, "$8"_s, "$9"_s,
};

constexpr ASCIILiteral openNamesOfTypedArrayConstructors[] = { "toLocaleString"_s };

constexpr std::span<const ASCIILiteral> onlyOfUint8Array(TypedArrayType type, std::span<const ASCIILiteral> members)
{
    return type == TypeUint8 ? members : std::span<const ASCIILiteral> { };
}

#define AOT_TYPED_ARRAY_CONSTRUCTOR_AND_PROTOTYPE(name) \
    { #name "Array"_s, ClosedProtocolNames::None, onlyOfUint8Array(Type##name, replaceableUint8ArrayFunctions), openNamesOfTypedArrayConstructors }, \
    { #name "Array.prototype"_s, ClosedProtocolNames::OfObjects, onlyOfUint8Array(Type##name, replaceableUint8ArrayMethods) },

constexpr ObjectWithLockedMembers objectsWithLockedMembers[] = {
    { "Symbol"_s, ClosedProtocolNames::None },
    { "Promise"_s, ClosedProtocolNames::None, replaceablePromiseFunctions },
    { "Object"_s, ClosedProtocolNames::None },
    { "Array"_s, ClosedProtocolNames::None },
    { "Number"_s, ClosedProtocolNames::None, replaceableNumberFunctions },
    { "String"_s, ClosedProtocolNames::None },
    { "Boolean"_s, ClosedProtocolNames::None },
    { "BigInt"_s, ClosedProtocolNames::None },
    { "Map"_s, ClosedProtocolNames::None, replaceableMapFunctions },
    { "Set"_s, ClosedProtocolNames::None },
    { "WeakMap"_s, ClosedProtocolNames::None },
    { "WeakSet"_s, ClosedProtocolNames::None },
    { "ArrayBuffer"_s, ClosedProtocolNames::None },
    { "SharedArrayBuffer"_s, ClosedProtocolNames::None },
    { "Date"_s, ClosedProtocolNames::None, replaceableDateFunctions },
    { "RegExp"_s, ClosedProtocolNames::None, replaceableRegExpMembers },
    { "Proxy"_s, ClosedProtocolNames::None },
    { "Math"_s, ClosedProtocolNames::None, replaceableMathFunctions },
    { "JSON"_s, ClosedProtocolNames::None, replaceableJSONFunctions },
    { "Reflect"_s, ClosedProtocolNames::None },
    { "Atomics"_s, ClosedProtocolNames::None },
    { "Array.prototype"_s, ClosedProtocolNames::OfObjects },
    { "Iterator.prototype"_s, ClosedProtocolNames::OfObjects, replaceableIteratorMethods },
    { "Object.getPrototypeOf([][Symbol.iterator]())"_s, ClosedProtocolNames::OfObjects },
    { "Object.getPrototypeOf(new Map()[Symbol.iterator]())"_s, ClosedProtocolNames::OfObjects },
    { "Object.getPrototypeOf(new Set()[Symbol.iterator]())"_s, ClosedProtocolNames::OfObjects },
    { "Object.getPrototypeOf(''[Symbol.iterator]())"_s, ClosedProtocolNames::OfObjects },
    { "Object.getPrototypeOf(/a/[Symbol.matchAll]('a'))"_s, ClosedProtocolNames::OfObjects },
    { "Object.getPrototypeOf([].values().map(x => x))"_s, ClosedProtocolNames::OfObjects },
    { "Object.getPrototypeOf(Iterator.from({ next() { } }))"_s, ClosedProtocolNames::OfObjects },
    { "Object.getPrototypeOf(Object.getPrototypeOf(async function* () { }).prototype)"_s, ClosedProtocolNames::OfObjects },
    { "String.prototype"_s, ClosedProtocolNames::None, replaceableStringMethods },
    { "Number.prototype"_s, ClosedProtocolNames::None },
    { "Boolean.prototype"_s, ClosedProtocolNames::None },
    { "BigInt.prototype"_s, ClosedProtocolNames::None },
    { "Symbol.prototype"_s, ClosedProtocolNames::None },
    { "Map.prototype"_s, ClosedProtocolNames::Then, replaceableMapMethods },
    { "Set.prototype"_s, ClosedProtocolNames::Then, replaceableSetMethods },
    { "WeakMap.prototype"_s, ClosedProtocolNames::OfObjects, replaceableMapMethods },
    { "WeakSet.prototype"_s, ClosedProtocolNames::OfObjects },
    { "ArrayBuffer.prototype"_s, ClosedProtocolNames::OfObjects },
    { "SharedArrayBuffer.prototype"_s, ClosedProtocolNames::OfObjects },
    { "DataView.prototype"_s, ClosedProtocolNames::OfObjects },
    { "Date.prototype"_s, ClosedProtocolNames::OfObjects, replaceableDateMethods, { }, openProtocolNamesOfDatePrototype },
    { "Object.getPrototypeOf(Int8Array)"_s, ClosedProtocolNames::None },
    { "Object.getPrototypeOf(Int8Array).prototype"_s, ClosedProtocolNames::OfObjects },
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_ARRAY_CONSTRUCTOR_AND_PROTOTYPE)
};

#undef AOT_TYPED_ARRAY_CONSTRUCTOR_AND_PROTOTYPE

#define AOT_TYPED_ARRAY_INSTANCE(name) { "new " #name "Array(2)"_s, true },

constexpr InstanceOfBuiltinClass instancesOfBuiltinClasses[] = {
    { "[]"_s, true },
    { "Promise.resolve(1)"_s, true },
    { "/a/"_s, true },
    { "new Map"_s, true },
    { "new Set"_s, true },
    { "new WeakMap"_s, true },
    { "new WeakSet"_s, true },
    { "new ArrayBuffer(8)"_s, true },
    { "new SharedArrayBuffer(8)"_s, true },
    { "new DataView(new ArrayBuffer(8))"_s, true },
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_ARRAY_INSTANCE)
    { "new Date(0)"_s, true },
    { "(d => (d.getTime = 1, d))(new Date(0))"_s, false },
    { "new (class D extends Date { })(0)"_s, false },
    { "new Error('a')"_s, false },
    { "new TypeError('a')"_s, false },
    { "(function () { })"_s, false },
    { "(() => { })"_s, false },
    { "(class { })"_s, false },
    { "(function () { }).bind(null)"_s, false },
    { "Math.max"_s, false },
    { "Uint8Array"_s, false },
    { "(m => (m.get = 1, m))(new Map)"_s, false },
    { "(s => (s[Symbol.iterator] = 1, s))(new Set)"_s, false },
    { "(m => (m.toJSON = 1, m))(new WeakMap)"_s, false },
    { "(r => (r.exec = 1, r))(/a/)"_s, false },
    { "(b => (b.slice = 1, b))(new ArrayBuffer(8))"_s, false },
    { "(v => (v.then = 1, v))(new DataView(new ArrayBuffer(8)))"_s, false },
    { "Object.setPrototypeOf(new Map, Object.create(Map.prototype))"_s, false },
    { "new (class M extends Map { })"_s, false },
    { "new (class R extends RegExp { })('a')"_s, false },
};

#undef AOT_TYPED_ARRAY_INSTANCE

constexpr ASCIILiteral numericKeys[] = {
    "0"_s, "5"_s, "4294967294"_s, "4294967295"_s, "-1"_s, "1.5"_s, "-0"_s, "NaN"_s, "Infinity"_s, "-Infinity"_s, "1e+21"_s, "1e-7"_s,
};

constexpr ASCIILiteral ordinaryKeys[] = {
    "nameNobodyHas"_s, "01"_s, "+1"_s, "1e21"_s, "1.50"_s, "0x1"_s,
};

constexpr ASCIILiteral namesReadFromFunctions[] = {
    "prototype"_s, "displayName"_s, "then"_s, "toJSON"_s,
};

constexpr ASCIILiteral openNamesOfFunctionPrototype[] = {
    "toString"_s, "demethodize"_s, "unThis"_s,
};

constexpr ASCIILiteral replaceableFunctionMethods[] = { "toString"_s };

constexpr ASCIILiteral prototypesOfFunctionKinds[] = {
    "Object.getPrototypeOf(async function () { })"_s,
    "Object.getPrototypeOf(function* () { })"_s,
    "Object.getPrototypeOf(async function* () { })"_s,
};
constexpr ASCIILiteral thenableProtocolNames[] = { "then"_s };
constexpr ASCIILiteral serializationProtocolNames[] = { "toJSON"_s };
constexpr ASCIILiteral iterationProtocolNames[] = { "next"_s, "return"_s, "throw"_s };

constexpr ASCIILiteral throwingWritesToStringPrototype[] = {
    "(function () { 'use strict'; String.prototype.slice = function () { }; })()"_s,
    "(function () { 'use strict'; delete String.prototype.slice; })()"_s,
    "String.prototype.slice = function () { }"_s,
    "delete String.prototype.slice"_s,
    "String.prototype[5] = 'x'"_s,
    "String.prototype[-1] = 'x'"_s,
    "String.prototype[1.5] = 'x'"_s,
    "String.prototype[Symbol.replace] = 1"_s,
    "String.prototype.__proto__ = {}"_s,
    "Object.defineProperty(String.prototype, 'slice', { value: 1 })"_s,
    "Object.defineProperty(String.prototype, Symbol.iterator, { value: 1 })"_s,
    "Object.assign(String.prototype, { padEnd: 1 })"_s,
    "(function () { 'use strict'; String.prototype[5] = 'x'; })()"_s,
    "(function () { 'use strict'; String.prototype['5'] = 'x'; })()"_s,
    "Object.defineProperty(String.prototype, 5, { value: 'x' })"_s,
    "Object.defineProperty(String.prototype, 5, { get() { } })"_s,
    "(function () { 'use strict'; String.prototype[4294967295] = 'x'; })()"_s,
    "(function () { 'use strict'; String.prototype[-1] = 'x'; })()"_s,
    "(function () { 'use strict'; String.prototype[1.5] = 'x'; })()"_s,
    "(function () { 'use strict'; String.prototype['-0'] = 'x'; })()"_s,
    "(function () { 'use strict'; String.prototype[NaN] = 'x'; })()"_s,
    "(function () { 'use strict'; String.prototype[Infinity] = 'x'; })()"_s,
    "(function () { 'use strict'; String.prototype[1e21] = 'x'; })()"_s,
    "Object.defineProperty(String.prototype, '1.5', { value: 'x' })"_s,
    "Object.defineProperty(String.prototype, '-1', { get() { } })"_s,
    "(function () { 'use strict'; String.prototype[Symbol.replace] = function () { }; })()"_s,
    "Object.defineProperty(String.prototype, Symbol.toPrimitive, { value: 1 })"_s,
    "(function () { 'use strict'; String.prototype.hasOwnProperty = function () { }; })()"_s,
    "Object.setPrototypeOf(String.prototype, {})"_s,
    "(function () { 'use strict'; String.prototype.__proto__ = {}; })()"_s,
};

constexpr ASCIILiteral refusedWritesToStringPrototype[] = {
    "Reflect.set(String.prototype, 'slice', 1)"_s,
    "Reflect.defineProperty(String.prototype, 'slice', { value: 1 })"_s,
    "Reflect.defineProperty(String.prototype, 'slice', { enumerable: true })"_s,
    "Reflect.deleteProperty(String.prototype, 'slice')"_s,
    "Reflect.set(String.prototype, 5, 'x')"_s,
    "Reflect.defineProperty(String.prototype, 5, { value: 'x' })"_s,
    "Reflect.set(String.prototype, '-1', 'x')"_s,
    "Reflect.set(String.prototype, '1.5', 'x')"_s,
    "Reflect.defineProperty(String.prototype, '1.5', { value: 'x' })"_s,
    "Reflect.set(String.prototype, Symbol.replace, 1)"_s,
    "Reflect.set(String.prototype, 'hasOwnProperty', 1)"_s,
    "Reflect.setPrototypeOf(String.prototype, {})"_s,
    "String.prototype.hasOwnProperty(5)"_s,
    "String.prototype.hasOwnProperty('1.5')"_s,
    "Symbol.replace in String.prototype"_s,
};

constexpr ASCIILiteral allowedOnStringPrototype[] = {
    "'abc'[5] === undefined && 'abc'[-1] === undefined && 'abc'[1.5] === undefined && 'abc'[Symbol.replace] === undefined && Object.getPrototypeOf(String.prototype) === Object.prototype"_s,
    "Object.getOwnPropertyDescriptor(String.prototype, 'slice').writable"_s,
    "Object.getOwnPropertyDescriptor(String.prototype, 'slice').configurable"_s,
    "Reflect.defineProperty(String.prototype, 'slice', { value: String.prototype.slice })"_s,
    "Reflect.setPrototypeOf(String.prototype, Object.prototype)"_s,
    "Reflect.deleteProperty(String.prototype, 5)"_s,
    "Object.isExtensible(String.prototype)"_s,
    "(String.prototype.red = 1, 'text'.red === 1)"_s,
    "(String.prototype.red = 2, 'text'.red === 2)"_s,
    "delete String.prototype.red && !('red' in String.prototype)"_s,
    "(function () { 'use strict'; String.prototype.green = 1; return 'text'.green === 1; })()"_s,
    "Reflect.defineProperty(String.prototype, 'blue', { get() { return 1; } }) && 'text'.blue === 1"_s,
    "(s => (String.prototype[s] = 1, 'text'[s] === 1))(Symbol('made by the program'))"_s,
    "(s => (String.prototype[s] = 1, 'text'[s] === 1))(Symbol.for('registered by the program'))"_s,
    "(String.prototype['01'] = 1, 'text'['01'] === 1)"_s,
    "(String.prototype['1e21'] = 1, 'text'['1e21'] === 1)"_s,
    "(String.prototype.then = 1, 'text'.then === 1)"_s,
    "(o => (o.slice = 1, o[5] = 2, o[1.5] = 3, o.slice === 1 && o[5] === 2 && o[1.5] === 3))(new String('a'))"_s,
    "(o => (o.slice = 1, o[5] = 2, o[1.5] = 3, o.slice === 1 && o[5] === 2 && o[1.5] === 3))(Object.create(String.prototype))"_s,
    "(o => Reflect.set(String.prototype, 5, 1, o) && Reflect.set(String.prototype, 'slice', 2, o) && o[5] === 1 && o.slice === 2)({})"_s,
    "(f => { class S extends String { } S.prototype.slice = f; return new S('a').slice === f; })(function () { })"_s,
    "(String.nameNobodyHas = 1, String.nameNobodyHas === 1)"_s,
    "(f => (String.prototype.at = f, 'text'.at === f))(function () { })"_s,
    "(f => (String.prototype.anchor = f, 'text'.anchor === f))(function () { })"_s,
    "(String.prototype.__defineGetter__('bold', function () { return 1; }), 'text'.bold === 1)"_s,
    "delete String.prototype.sup && !('sup' in String.prototype)"_s,
};

constexpr ASCIILiteral throwingWritesToRegExpPrototype[] = {
    "(function () { 'use strict'; RegExp.prototype.exec = function () { }; })()"_s,
    "(function () { 'use strict'; delete RegExp.prototype.exec; })()"_s,
    "RegExp.prototype.exec = function () { }"_s,
    "delete RegExp.prototype.exec"_s,
    "RegExp.prototype.nameNobodyHas = 1"_s,
    "Object.defineProperty(RegExp.prototype, 'exec', { value: 1 })"_s,
    "Object.defineProperty(RegExp.prototype, 'flags', { get() { return ''; } })"_s,
    "Object.defineProperty(RegExp.prototype, 'global', { value: true })"_s,
    "(function () { 'use strict'; RegExp.prototype[Symbol.replace] = function () { }; })()"_s,
    "(function () { 'use strict'; RegExp.prototype.nameNobodyHas = 1; })()"_s,
    "Object.defineProperty(RegExp.prototype, 'nameNobodyHas', { value: 1 })"_s,
    "(function () { 'use strict'; RegExp.prototype[Symbol('made by the program')] = 1; })()"_s,
    "(function () { 'use strict'; RegExp.prototype[0] = 1; })()"_s,
    "(function () { 'use strict'; RegExp.prototype.lastIndex = 1; })()"_s,
    "Object.setPrototypeOf(RegExp.prototype, {})"_s,
};

constexpr ASCIILiteral refusedWritesToRegExpPrototype[] = {
    "Reflect.set(RegExp.prototype, 'exec', 1)"_s,
    "Reflect.defineProperty(RegExp.prototype, 'exec', { value: 1 })"_s,
    "Reflect.deleteProperty(RegExp.prototype, 'exec')"_s,
    "Reflect.set(RegExp.prototype, 'nameNobodyHas', 1)"_s,
    "Reflect.defineProperty(RegExp.prototype, 'nameNobodyHas', { value: 1 })"_s,
    "Reflect.set(RegExp.prototype, Symbol('made by the program'), 1)"_s,
    "Reflect.set(RegExp.prototype, 0, 1)"_s,
    "Reflect.setPrototypeOf(RegExp.prototype, {})"_s,
    "Object.isExtensible(RegExp.prototype)"_s,
    "RegExp.prototype.hasOwnProperty('nameNobodyHas')"_s,
};

constexpr ASCIILiteral allowedBesideRegExpPrototype[] = {
    "Object.getOwnPropertyDescriptor(RegExp.prototype, 'exec').writable"_s,
    "Object.getOwnPropertyDescriptor(RegExp.prototype, 'exec').configurable"_s,
    "Reflect.defineProperty(RegExp.prototype, 'exec', { value: RegExp.prototype.exec })"_s,
    "/a/.test('a') && 'a'.replace(/a/, 'b') === 'b'"_s,
    "((r, f) => (r.exec = f, r.exec === f))(/a/, function () { })"_s,
    "(r => (r.lastIndex = 3, r.lastIndex === 3))(/a/g)"_s,
    "(r => (r.nameNobodyHas = 1, r.nameNobodyHas === 1))(/a/)"_s,
    "(f => { class R extends RegExp { } R.prototype.exec = f; return new R('a').exec === f; })(function () { })"_s,
    "(f => (RegExp.escape = f, RegExp.escape === f))(function () { })"_s,
    "(RegExp.nameNobodyHas = 1, RegExp.nameNobodyHas === 1)"_s,
};

constexpr ASCIILiteral throwingWritesToFunctionPrototype[] = {
    "Function.prototype.call = function () { }"_s,
    "delete Function.prototype.call"_s,
    "Function.prototype.nameNobodyHas = 1"_s,
    "(function () { 'use strict'; Object.getPrototypeOf(async function () { }).nameNobodyHas = 1; })()"_s,
    "(function () { 'use strict'; Object.getPrototypeOf(async function () { }).call = function () { }; })()"_s,
    "(function () { 'use strict'; Object.getPrototypeOf(function* () { }).toString = function () { }; })()"_s,
    "Object.defineProperty(Object.getPrototypeOf(function* () { }), 'prototype', { value: { } })"_s,
    "Object.defineProperty(Object.getPrototypeOf(async function* () { }), Symbol.toStringTag, { value: 'mine' })"_s,
    "(function () { 'use strict'; delete Object.getPrototypeOf(async function* () { }).constructor; })()"_s,
    "Object.setPrototypeOf(Object.getPrototypeOf(async function () { }), { })"_s,
    "(function () { 'use strict'; Function.prototype.call = function () { }; })()"_s,
    "(function () { 'use strict'; Function.prototype.apply = function () { }; })()"_s,
    "(function () { 'use strict'; Function.prototype.bind = function () { }; })()"_s,
    "(function () { 'use strict'; Function.prototype.constructor = function () { }; })()"_s,
    "(function () { 'use strict'; delete Function.prototype.call; })()"_s,
    "Object.defineProperty(Function.prototype, 'call', { value: 1 })"_s,
    "Object.defineProperty(Function.prototype, 'name', { value: 'x' })"_s,
    "Object.defineProperty(Function.prototype, 'length', { value: 1 })"_s,
    "(function () { 'use strict'; delete Function.prototype.name; })()"_s,
    "(function () { 'use strict'; delete Function.prototype.length; })()"_s,
    "Object.defineProperty(Function.prototype, 'caller', { value: 1 })"_s,
    "Object.defineProperty(Function.prototype, 'arguments', { value: 1 })"_s,
    "(function () { 'use strict'; Function.prototype.nameNobodyHas = 1; })()"_s,
    "Object.defineProperty(Function.prototype, 'nameNobodyHas', { value: 1 })"_s,
    "(function () { 'use strict'; Function.prototype.prototype = {}; })()"_s,
    "(function () { 'use strict'; Function.prototype.then = function () { }; })()"_s,
    "(function () { 'use strict'; Function.prototype.hasOwnProperty = function () { }; })()"_s,
    "(function () { 'use strict'; Function.prototype[Symbol.toPrimitive] = function () { }; })()"_s,
    "(function () { 'use strict'; Function.prototype[0] = 1; })()"_s,
    "(function () { 'use strict'; Function.prototype[1.5] = 1; })()"_s,
    "Object.setPrototypeOf(Function.prototype, {})"_s,
};

constexpr ASCIILiteral refusedWritesToFunctionPrototype[] = {
    "Reflect.set(Function.prototype, 'call', 1)"_s,
    "Reflect.defineProperty(Function.prototype, 'call', { value: 1 })"_s,
    "Reflect.deleteProperty(Function.prototype, 'call')"_s,
    "Reflect.defineProperty(Function.prototype, 'name', { value: 'x' })"_s,
    "Reflect.deleteProperty(Function.prototype, 'name')"_s,
    "Reflect.defineProperty(Function.prototype, 'length', { value: 1 })"_s,
    "Reflect.deleteProperty(Function.prototype, 'length')"_s,
    "Reflect.set(Function.prototype, 'nameNobodyHas', 1)"_s,
    "Reflect.defineProperty(Function.prototype, 'nameNobodyHas', { value: 1 })"_s,
    "Reflect.set(Function.prototype, Symbol.toPrimitive, 1)"_s,
    "Reflect.set(Function.prototype, 0, 1)"_s,
    "Reflect.setPrototypeOf(Function.prototype, {})"_s,
    "Function.prototype.hasOwnProperty('nameNobodyHas')"_s,
};

constexpr ASCIILiteral allowedOnFunctionPrototype[] = {
    "(s => (Object.getPrototypeOf(async function () { })[s] = 1, (async () => { })[s] === 1))(Symbol('made by the program'))"_s,
    "(f => (f.nameNobodyHas = 1, f.call = 2, f.nameNobodyHas === 1 && f.call === 2))(async function () { })"_s,
    "(f => (f.prototype = { }, Object.getPrototypeOf(f()) === f.prototype))(function* () { })"_s,
    "Object.getPrototypeOf(function* () { }).prototype === Object.getPrototypeOf(Object.getPrototypeOf((function* () { })()))"_s,
    "Function.prototype.name === '' && Function.prototype.length === 0"_s,
    "Object.getOwnPropertyDescriptor(Function.prototype, 'call').writable"_s,
    "Object.getOwnPropertyDescriptor(Function.prototype, 'name').configurable"_s,
    "Reflect.defineProperty(Function.prototype, 'call', { value: Function.prototype.call })"_s,
    "(f => (Function.prototype.toString = f, Function.prototype.toString === f && String(Math.max) === 'text'))(function () { return 'text'; })"_s,
    "(function () { 'use strict'; var f = function () { }; Function.prototype.toString = f; return Function.prototype.toString === f; })()"_s,
    "(f => Reflect.defineProperty(Function.prototype, 'toString', { value: f }) && Function.prototype.toString === f)(function () { })"_s,
    "(f => delete Function.prototype.toString && Reflect.defineProperty(Function.prototype, 'toString', { value: f }) && Function.prototype.toString === f)(function () { })"_s,
    "(f => (Function.prototype.demethodize = f, Math.max.demethodize === f))(function () { })"_s,
    "(f => (Function.prototype.unThis = f, Math.max.unThis === f))(function () { })"_s,
    "delete Function.prototype.unThis && !('unThis' in Function.prototype)"_s,
    "(s => (Function.prototype[s] = 1, Math.max[s] === 1))(Symbol('made by the program'))"_s,
    "(s => (Function.prototype[s] = 1, Math.max[s] === 1))(Symbol.for('registered by the program'))"_s,
    "(s => (Function.prototype[s] = null, Math.max[s] === null))(Symbol.metadata ||= Symbol('metadata'))"_s,
    "Object.isExtensible(Function.prototype)"_s,
    "((g, f) => (g.call = f, g.call === f))(function () { }, function () { })"_s,
    "(g => (g.nameNobodyHas = 1, g.nameNobodyHas === 1))(function () { })"_s,
    "(g => (Object.defineProperty(g, 'name', { value: 'x' }), g.name === 'x'))(function () { })"_s,
    "(g => delete g.name && delete g.length && g.name === '' && g.length === 0)(function named(a) { })"_s,
    "(class A { static call() { return 1; } }).call() === 1"_s,
    "(Function.nameNobodyHas = 1, Function.nameNobodyHas === 1)"_s,
};

#define FOR_EACH_CONSTRUCTOR_WITH_SPECIES(macro) \
    macro("Array") \
    macro("Map") \
    macro("Set") \
    macro("RegExp") \
    macro("ArrayBuffer") \
    macro("SharedArrayBuffer") \
    macro("Promise") \
    macro("Object.getPrototypeOf(Int8Array)")

#define AOT_THROWING_WRITES_TO_SPECIES(constructor) \
    "Object.defineProperty(" constructor ", Symbol.species, { value: Object })"_s, \
    "Object.defineProperty(" constructor ", Symbol.species, { get() { return Object; } })"_s, \
    "(function () { 'use strict'; delete " constructor "[Symbol.species]; })()"_s, \
    "delete " constructor "[Symbol.species]"_s,

#define AOT_REFUSED_WRITES_TO_SPECIES(constructor) \
    "Reflect.defineProperty(" constructor ", Symbol.species, { value: Object })"_s, \
    "Reflect.defineProperty(" constructor ", Symbol.species, { get() { return Object; } })"_s, \
    "Reflect.deleteProperty(" constructor ", Symbol.species)"_s, \
    "Reflect.set(" constructor ", Symbol.species, Object)"_s,

#define AOT_UNCHANGED_SPECIES(constructor) \
    "(C => C[Symbol.species] === C)(" constructor ")"_s, \
    "Object.getOwnPropertyDescriptor(" constructor ", Symbol.species).configurable"_s,

constexpr ASCIILiteral throwingWritesToSpecies[] = {
    FOR_EACH_CONSTRUCTOR_WITH_SPECIES(AOT_THROWING_WRITES_TO_SPECIES)
};

constexpr ASCIILiteral refusedWritesToSpecies[] = {
    FOR_EACH_CONSTRUCTOR_WITH_SPECIES(AOT_REFUSED_WRITES_TO_SPECIES)
};

constexpr ASCIILiteral allowedBesideSpecies[] = {
    FOR_EACH_CONSTRUCTOR_WITH_SPECIES(AOT_UNCHANGED_SPECIES)
    "[1].map(x => x).constructor === Array"_s,
    "new Uint8Array(2).subarray(1).constructor === Uint8Array"_s,
    "new ArrayBuffer(2).slice(1).constructor === ArrayBuffer"_s,
    "(class M extends Map { static get [Symbol.species]() { return Object; } })[Symbol.species] === Object"_s,
    "(A => (Object.defineProperty(A, Symbol.species, { value: Array }), new A().map(x => x).constructor === Array))(class extends Array { })"_s,
    "(U => new U(2).subarray(1).constructor === Uint8Array)(class extends Uint8Array { static get [Symbol.species]() { return Uint8Array; } })"_s,
};

#undef AOT_UNCHANGED_SPECIES
#undef AOT_REFUSED_WRITES_TO_SPECIES
#undef AOT_THROWING_WRITES_TO_SPECIES
#undef FOR_EACH_CONSTRUCTOR_WITH_SPECIES

constexpr ASCIILiteral throwingWritesToLockedMembers[] = {
    "Set.prototype.add = function () { }"_s,
    "WeakMap.prototype.toJSON = function () { return 1; }"_s,
    "(function () { 'use strict'; Set.prototype.add = function () { }; })()"_s,
    "(function () { 'use strict'; WeakMap.prototype.toJSON = function () { }; })()"_s,
    "(function () { 'use strict'; Uint8Array.prototype.toJSON = function () { }; })()"_s,
    "(function () { 'use strict'; Map.prototype.then = function () { }; })()"_s,
    "(function () { 'use strict'; Set.prototype.then = function () { }; })()"_s,
    "(function () { 'use strict'; Map.prototype[Symbol.toPrimitive] = function () { }; })()"_s,
    "(function () { 'use strict'; Map.prototype[0] = 1; })()"_s,
    "(function () { 'use strict'; delete Map.prototype.get; })()"_s,
    "Object.defineProperty(Map.prototype, 'size', { value: 1 })"_s,
    "Object.setPrototypeOf(Map.prototype, {})"_s,
    "(function () { 'use strict'; WeakMap.prototype.get = function () { }; })()"_s,
    "(function () { 'use strict'; WeakSet.prototype.has = function () { }; })()"_s,
    "(function () { 'use strict'; Number.prototype.toFixed = function () { }; })()"_s,
    "(function () { 'use strict'; Boolean.prototype.valueOf = function () { }; })()"_s,
    "(function () { 'use strict'; BigInt.prototype.toString = function () { }; })()"_s,
    "Object.defineProperty(Symbol.prototype, 'description', { value: 1 })"_s,
    "Object.defineProperty(Symbol.prototype, Symbol.toPrimitive, { value: 1 })"_s,
    "(function () { 'use strict'; ArrayBuffer.prototype.slice = function () { }; })()"_s,
    "Object.defineProperty(ArrayBuffer.prototype, 'byteLength', { value: 1 })"_s,
    "(function () { 'use strict'; SharedArrayBuffer.prototype.slice = function () { }; })()"_s,
    "(function () { 'use strict'; DataView.prototype.getInt8 = function () { }; })()"_s,
    "(function () { 'use strict'; Object.getPrototypeOf(Int8Array).prototype.set = function () { }; })()"_s,
    "Object.defineProperty(Object.getPrototypeOf(Int8Array).prototype, 'length', { value: 1 })"_s,
    "(function () { 'use strict'; Object.getPrototypeOf(Int8Array).from = function () { }; })()"_s,
    "(function () { 'use strict'; Uint8Array.prototype.subarray = function () { }; })()"_s,
    "(function () { 'use strict'; Uint8Array.prototype[0] = 1; })()"_s,
    "Object.defineProperty(Uint8Array, Symbol.species, { value: Array })"_s,
    "(function () { 'use strict'; Uint8Array.from = function () { }; })()"_s,
    "Object.defineProperty(Uint8Array, 'name', { value: 'x' })"_s,
    "Object.setPrototypeOf(Uint8Array, Function.prototype)"_s,
    "Object.setPrototypeOf(Uint8Array.prototype, Object.prototype)"_s,
};

constexpr ASCIILiteral refusedWritesToLockedMembers[] = {
    "Reflect.set(Set.prototype, 'add', 1)"_s,
    "Reflect.set(WeakMap.prototype, 'toJSON', 1)"_s,
    "Reflect.set(Map.prototype, 'then', 1)"_s,
    "Reflect.deleteProperty(Map.prototype, 'get')"_s,
    "Reflect.set(Uint8Array.prototype, 'subarray', 1)"_s,
    "Reflect.defineProperty(Uint8Array, Symbol.species, { value: Array })"_s,
    "Reflect.setPrototypeOf(Uint8Array, Function.prototype)"_s,
    "Uint8Array.hasOwnProperty(Symbol.species)"_s,
    "'toJSON' in WeakMap.prototype"_s,
};

constexpr ASCIILiteral allowedBesideLockedMembers[] = {
    "new Set([1, 1, 2]).size === 2 && JSON.stringify(new WeakMap) === '{}'"_s,
    "(Map.prototype.toJSON = function () { return 1; }, JSON.stringify(new Map) === '1')"_s,
    "(Set.prototype.toJSON = function () { return 2; }, JSON.stringify(new Set) === '2')"_s,
    "(f => (Set.prototype.union = f, new Set().union === f))(function () { })"_s,
    "delete Set.prototype.isDisjointFrom && !('isDisjointFrom' in Set.prototype)"_s,
    "(BigInt.prototype.toJSON = function () { return 'big'; }, JSON.stringify(1n) === '\"big\"')"_s,
    "(Map.prototype.mapValues = function () { return 1; }, new Map().mapValues() === 1)"_s,
    "(s => (Set.prototype[s] = 1, new Set()[s] === 1))(Symbol('made by the program'))"_s,
    "(Number.prototype.double = function () { return this * 2; }, (2).double() === 4)"_s,
    "(Uint8Array.prototype.first = function () { return this[0]; }, Uint8Array.of(7).first() === 7)"_s,
    "(Uint8Array.nameNobodyHas = 1, Uint8Array.nameNobodyHas === 1)"_s,
    "Object.defineProperty(Int8Array, 'toLocaleString', { value: 1, configurable: true }).toLocaleString === 1 && delete Int8Array.toLocaleString"_s,
    "((m, f) => (m.get = f, m.get === f))(new Map, function () { })"_s,
    "((a, f) => (a.subarray = f, a.subarray === f))(new Uint8Array(1), function () { })"_s,
    "(f => { class M extends Map { } M.prototype.get = f; return new M().get === f; })(function () { })"_s,
    "(f => (Map.groupBy = f, Map.groupBy === f))(function () { })"_s,
    "(f => (Number.parseFloat = f, Number.parseFloat === f))(function () { })"_s,
    "(ArrayBuffer.nameNobodyHas = 1, ArrayBuffer.nameNobodyHas === 1)"_s,
    "(f => (Date.prototype.toString = f, Date.prototype.toString === f))(function () { })"_s,
    "(f => (Date.prototype.next = f, Date.prototype.next === f))(function () { })"_s,
    "(f => { class D extends Date { } D.prototype.getTime = f; return new D().getTime === f; })(function () { })"_s,
    "(f => (Error.prototype.toString = f, Error.prototype.toString === f))(function () { })"_s,
};

constexpr ASCIILiteral throwingWritesToProtocolHolders[] = {
    "(function () { 'use strict'; Array.prototype.then = function () { }; })()"_s,
    "(function () { 'use strict'; Array.prototype.toJSON = function () { }; })()"_s,
    "(function () { 'use strict'; Array.prototype.next = function () { }; })()"_s,
    "(function () { 'use strict'; Array.prototype.return = function () { }; })()"_s,
    "(function () { 'use strict'; Array.prototype.throw = function () { }; })()"_s,
    "(function () { 'use strict'; Set.prototype.next = function () { }; })()"_s,
    "(function () { 'use strict'; Number.prototype.next = function () { }; })()"_s,
    "(function () { 'use strict'; Array.prototype[Symbol.isConcatSpreadable] = false; })()"_s,
    "(function () { 'use strict'; Array.prototype.hasOwnProperty = function () { }; })()"_s,
    "(function () { 'use strict'; Array.prototype[-1] = 1; })()"_s,
    "(function () { 'use strict'; Array.prototype[1.5] = 1; })()"_s,
    "Array.prototype.pop()"_s,
    "Array.prototype.unshift(1)"_s,
    "(function () { 'use strict'; Iterator.prototype.map = function () { }; })()"_s,
    "(function () { 'use strict'; Iterator.prototype.then = function () { }; })()"_s,
    "(function () { 'use strict'; Iterator.prototype.next = function () { }; })()"_s,
    "(function () { 'use strict'; Iterator.prototype.return = function () { }; })()"_s,
    "(function () { 'use strict'; Iterator.prototype.throw = function () { }; })()"_s,
    "(function () { 'use strict'; Object.getPrototypeOf([][Symbol.iterator]()).return = function () { }; })()"_s,
    "(function () { 'use strict'; Object.getPrototypeOf(Object.getPrototypeOf(async function* () { }).prototype).next = function () { }; })()"_s,
    "(function () { 'use strict'; Iterator.prototype[0] = 1; })()"_s,
    "Object.setPrototypeOf(Iterator.prototype, {})"_s,
    "(function () { 'use strict'; Object.getPrototypeOf([][Symbol.iterator]()).map = function () { }; })()"_s,
    "Object.setPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()), {})"_s,
    "(function () { 'use strict'; Symbol.keyFor = function () { }; })()"_s,
    "(function () { 'use strict'; Symbol.call = function () { }; })()"_s,
    "Object.setPrototypeOf(Symbol, {})"_s,
    "(function () { 'use strict'; Promise.all = function () { }; })()"_s,
    "(function () { 'use strict'; delete Promise.reject; })()"_s,
    "Object.defineProperty(Promise, Symbol.hasInstance, { value: function () { } })"_s,
    "Object.setPrototypeOf(Promise, {})"_s,
    "(function () { 'use strict'; Object.prototype.should = 1; })()"_s,
};

constexpr ASCIILiteral refusedWritesToProtocolHolders[] = {
    "Reflect.set(Array.prototype, 'then', 1)"_s,
    "Reflect.set(Array.prototype, 'toJSON', 1)"_s,
    "Reflect.set(Array.prototype, Symbol.isConcatSpreadable, false)"_s,
    "Reflect.set(Array.prototype, '-1', 1)"_s,
    "Reflect.set(Iterator.prototype, 'map', 1)"_s,
    "Reflect.set(Iterator.prototype, 'next', 1)"_s,
    "Reflect.set(Object.getPrototypeOf([][Symbol.iterator]()), 'return', 1)"_s,
    "Reflect.set(Object.getPrototypeOf([].values().map(x => x)), 'throw', 1)"_s,
    "Reflect.set(Symbol, 'keyFor', 1)"_s,
    "Reflect.set(Promise, 'all', 1)"_s,
    "Reflect.deleteProperty(Promise, 'reject')"_s,
};

constexpr ASCIILiteral allowedOnProtocolHolders[] = {
    "Array.prototype.length === 0 && !Array.prototype.hasOwnProperty(0)"_s,
    "(function () { 'use strict'; Array.prototype.last = function () { return this[this.length - 1]; }; return [1, 2].last() === 2; })()"_s,
    "(f => (Array.prototype.last = f, [].last === f))(function () { })"_s,
    "delete Array.prototype.last && !('last' in Array.prototype)"_s,
    "Reflect.defineProperty(Array.prototype, 'first', { get() { return this[0]; } }) && [1, 2].first === 1"_s,
    "(s => (Array.prototype[s] = 1, [][s] === 1))(Symbol('made by the program'))"_s,
    "(s => (Array.prototype[s] = 1, [][s] === 1))(Symbol.for('registered by the program'))"_s,
    "(Array.prototype['01'] = 1, []['01'] === 1)"_s,
    "(f => (Array.prototype.groupBy = f, [].groupBy === f))(function () { })"_s,
    "[, 1][0] === undefined && [...[1, 2]].length === 2"_s,
    "(Iterator.prototype.nameNobodyHas = 1, [].values().nameNobodyHas === 1)"_s,
    "(p => (p.nameNobodyHas = 2, [].values().nameNobodyHas === 2))(Object.getPrototypeOf([][Symbol.iterator]()))"_s,
    "(Symbol.nameNobodyHas = 1, delete Symbol.nameNobodyHas && !('nameNobodyHas' in Symbol))"_s,
    "(Symbol.then = 1, Symbol.then === 1)"_s,
    "(Symbol.observable = Symbol('observable'), typeof Symbol.observable === 'symbol')"_s,
    "(Symbol.metadata ||= Symbol('metadata'), typeof Symbol.metadata === 'symbol')"_s,
    "(f => (Promise.allKeyed = f, Promise.allKeyed === f))(function () { })"_s,
    "(f => (Promise.nameNobodyHas = f, Promise.nameNobodyHas === f))(function () { })"_s,
    "[Symbol, Promise, Array.prototype, Iterator.prototype].every(Object.isExtensible)"_s,
    "(d => Reflect.defineProperty(Array.prototype, 'map', d) && Reflect.defineProperty(Array.prototype, 'map', { value: d.value, writable: false }) && !Reflect.defineProperty(Array.prototype, 'map', { writable: true }))(Object.getOwnPropertyDescriptor(Array.prototype, 'map'))"_s,
    "(Object.seal(Iterator.prototype), Object.isSealed(Iterator.prototype) && !Object.isFrozen(Iterator.prototype))"_s,
    "(Object.freeze(Array.prototype), Object.isFrozen(Array.prototype) && !Reflect.defineProperty(Array.prototype, 'nameNobodyHas', { value: 1 }) && [1, 2].map(x => x * 2).join() === '2,4')"_s,
    "(Object.freeze(Object.prototype), Object.isFrozen(Object.prototype) && (() => { 'use strict'; try { ({}).toString = 1; return false; } catch (error) { return error instanceof TypeError; } })())"_s,
};

constexpr Statements statementsAboutStringPrototype { throwingWritesToStringPrototype, refusedWritesToStringPrototype, allowedOnStringPrototype };
constexpr Statements statementsAboutRegExpPrototype { throwingWritesToRegExpPrototype, refusedWritesToRegExpPrototype, allowedBesideRegExpPrototype };
constexpr Statements statementsAboutFunctionPrototype { throwingWritesToFunctionPrototype, refusedWritesToFunctionPrototype, allowedOnFunctionPrototype };
constexpr Statements statementsAboutSpecies { throwingWritesToSpecies, refusedWritesToSpecies, allowedBesideSpecies };
constexpr Statements statementsAboutLockedMembers { throwingWritesToLockedMembers, refusedWritesToLockedMembers, allowedBesideLockedMembers };
constexpr Statements statementsAboutProtocolHolders { throwingWritesToProtocolHolders, refusedWritesToProtocolHolders, allowedOnProtocolHolders };

void checkKey(bool condition, ASCIILiteral source, const Identifier& key, ASCIILiteral expectation)
{
    if (condition) [[likely]]
        return;
    dataLogLn("The key ", key, " of ", source, " ", expectation);
    failNativeTest(__FILE__, __LINE__, source.characters());
}

template<typename Function>
void forEachOwnKey(VM& vm, JSObject* object, const Function& function)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyNameArrayBuilder keys(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, object->realm(), keys, DontEnumPropertiesMode::Include);
    AOT_TEST_CHECK(!scope.exception());
    AOT_TEST_CHECK(keys.size());
    for (const Identifier& key : keys)
        function(key);
}

bool isFixedByLanguage(VM& vm, JSObject* object, const Identifier& key)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyDescriptor descriptor;
    bool isPresent = object->getOwnPropertyDescriptor(object->realm(), key, descriptor);
    AOT_TEST_CHECK(!scope.exception());
    return isPresent && descriptor.isDataDescriptor() && !descriptor.writable() && !descriptor.configurable();
}

bool isFixedMember(VM& vm, JSObject* object, const Identifier& key)
{
    return object->structure()->isImmutableProperty(key.impl()) || isFixedByLanguage(vm, object, key);
}

bool isListed(std::span<const ASCIILiteral> names, const Identifier& key)
{
    return !key.isSymbol() && std::ranges::any_of(names, [&](ASCIILiteral name) {
        return key.string() == name;
    });
}

void checkMembers(VM& vm, JSObject* object, ASCIILiteral source, std::span<const ASCIILiteral> replaceableMembers)
{
    unsigned numberOfReplaceableMembers = 0;
    forEachOwnKey(vm, object, [&](const Identifier& key) {
        bool isReplaceable = isListed(replaceableMembers, key);
        numberOfReplaceableMembers += isReplaceable;
        checkKey(isFixedMember(vm, object, key) != isReplaceable, source, key, isReplaceable ? "cannot change"_s : "can change"_s);
    });
    AOT_TEST_CHECK_FOR_SOURCE(numberOfReplaceableMembers == replaceableMembers.size(), source);
}

void checkNames(VM& vm, Structure* structure, ASCIILiteral source, std::span<const ASCIILiteral> names, bool areClosed)
{
    for (ASCIILiteral name : names) {
        Identifier key = Identifier::fromString(vm, name);
        checkKey(structure->isImmutableProperty(key.impl()) == areClosed, source, key, areClosed ? "is open"_s : "is closed"_s);
    }
}

void checkSymbols(Realms& realms, JSObject* object, ASCIILiteral source)
{
    VM& vm = realms.vm();
#define AOT_CHECK_WELL_KNOWN_SYMBOL(name) \
    checkKey(isFixedMember(vm, object, vm.propertyNames->name##Symbol), source, vm.propertyNames->name##Symbol, "is a well-known symbol and open"_s);
    JSC_COMMON_PRIVATE_IDENTIFIERS_EACH_WELL_KNOWN_SYMBOL(AOT_CHECK_WELL_KNOWN_SYMBOL)
#undef AOT_CHECK_WELL_KNOWN_SYMBOL

    for (ASCIILiteral symbol : { "Symbol('made by the program')"_s, "Symbol.for('registered by the program')"_s }) {
        Identifier key = realms.key(realms.locked(), symbol);
        checkKey(!object->structure()->isImmutableProperty(key.impl()), source, key, "is a symbol of the program and closed"_s);
    }
}

// A constructor and a namespace object take a property under a number. A prototype object takes none: an element that an instance lacks is looked for there.
bool isConstructor(JSObject* object)
{
    return object->isCallable() && object != object->globalObject()->functionPrototype();
}

bool takesNumericKeys(JSObject* object, ASCIILiteral source)
{
    constexpr ASCIILiteral namespaceObjects[] = { "Math"_s, "JSON"_s, "Reflect"_s, "Atomics"_s };
    return isConstructor(object) || std::ranges::contains(namespaceObjects, source);
}

void checkInheritedKeys(VM& vm, JSObject* object, ASCIILiteral source, std::span<const ASCIILiteral> replaceableMembers, std::span<const ASCIILiteral> openInheritedNames = { })
{
    for (JSObject* ancestor = object->getPrototypeDirect().getObject(); ancestor; ancestor = ancestor->getPrototypeDirect().getObject()) {
        AOT_TEST_CHECK_FOR_SOURCE(ancestor->structure()->hasImmutablePrototype() && ancestor->structure()->hasImmutableIndexedProperties() != isConstructor(ancestor), source);
        forEachOwnKey(vm, ancestor, [&](const Identifier& key) {
            if (isListed(openInheritedNames, key))
                checkKey(!isFixedMember(vm, object, key), source, key, "is closed"_s);
            else
                checkKey(isFixedMember(vm, object, key) || isListed(replaceableMembers, key), source, key, "is inherited and open"_s);
        });
    }
}

void checkObjectsWithLockedMembers(Realms& realms)
{
    VM& vm = realms.vm();
    for (const ObjectWithLockedMembers& expected : objectsWithLockedMembers) {
        JSObject* object = realms.object(realms.locked(), expected.source);
        checkMembers(vm, object, expected.source, expected.replaceableMembers);
        checkInheritedKeys(vm, object, expected.source, expected.replaceableMembers, expected.openInheritedNames);

        Structure* structure = object->structure();
        checkSymbols(realms, object, expected.source);
        bool isPrototypeObject = !takesNumericKeys(object, expected.source);
        checkNames(vm, structure, expected.source, numericKeys, isPrototypeObject);
        checkNames(vm, structure, expected.source, ordinaryKeys, false);
        checkNames(vm, structure, expected.source, thenableProtocolNames, expected.closedProtocolNames >= ClosedProtocolNames::Then);
        checkNames(vm, structure, expected.source, serializationProtocolNames, expected.closedProtocolNames >= ClosedProtocolNames::OfObjects);
        for (const ASCIILiteral& name : iterationProtocolNames)
            checkNames(vm, structure, expected.source, std::span { &name, 1 }, isPrototypeObject && !std::ranges::contains(expected.openProtocolNames, name));
        AOT_TEST_CHECK_FOR_SOURCE(!structure->hasImmutableProperties() && structure->isStructureExtensible(), expected.source);
        AOT_TEST_CHECK_FOR_SOURCE(structure->hasImmutablePrototype() && structure->hasImmutableIndexedProperties() == isPrototypeObject, expected.source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.object(realms.plain(), expected.source)->structure()->mayHaveImmutableProperties(), expected.source);
    }
}

void checkObjectClosedToOtherNames(Realms& realms, ASCIILiteral source, std::span<const ASCIILiteral> replaceableMembers, bool hasOpenNames)
{
    VM& vm = realms.vm();
    JSObject* prototype = realms.object(realms.locked(), source);
    checkMembers(vm, prototype, source, replaceableMembers);
    checkInheritedKeys(vm, prototype, source, replaceableMembers);

    Structure* structure = prototype->structure();
    checkSymbols(realms, prototype, source);
    checkNames(vm, structure, source, numericKeys, true);
    checkNames(vm, structure, source, ordinaryKeys, true);
    checkNames(vm, structure, source, namesReadFromFunctions, true);
    checkNames(vm, structure, source, openNamesOfFunctionPrototype, !hasOpenNames);
    AOT_TEST_CHECK_FOR_SOURCE(!structure->hasImmutableProperties() && structure->isStructureExtensible(), source);
    AOT_TEST_CHECK_FOR_SOURCE(structure->hasImmutablePrototype() && structure->hasImmutableIndexedProperties(), source);
    AOT_TEST_CHECK_FOR_SOURCE(!realms.object(realms.plain(), source)->structure()->mayHaveImmutableProperties(), source);
}

void checkFunctionPrototype(Realms& realms)
{
    checkObjectClosedToOtherNames(realms, "Function.prototype"_s, replaceableFunctionMethods, true);
}

void checkPrototypesOfFunctionKinds(Realms& realms)
{
    for (ASCIILiteral source : prototypesOfFunctionKinds)
        checkObjectClosedToOtherNames(realms, source, { }, false);
}

// An object that is given the names of a holder twice, with other constant members the second time, keeps both.
void checkNamesGivenTwice(Realms& realms)
{
    VM& vm = realms.vm();
    JSObject* object = realms.object(realms.plain(), "({ })"_s);
    auto isFixed = [&](ASCIILiteral name) {
        return object->structure()->isImmutableProperty(Identifier::fromString(vm, name).impl());
    };
    const uint32_t first[] = { Builtins::DateNow.index() };
    const uint32_t second[] = { Builtins::DateParse.index() };

    AOT_TEST_CHECK(object->makePropertiesImmutable(vm, immutablePropertyNamesOf(vm, Builtins::Date, first).get()));
    AOT_TEST_CHECK(isFixed("UTC"_s) && isFixed("now"_s) && !isFixed("parse"_s) && isFixed("call"_s) && !isFixed("7"_s) && !isFixed("nameNobodyHas"_s));
    AOT_TEST_CHECK(object->structure()->isImmutableProperty(vm.propertyNames->hasInstanceSymbol.impl()));
    AOT_TEST_CHECK(object->makePropertiesImmutable(vm, immutablePropertyNamesOf(vm, Builtins::Date, second).get()));
    AOT_TEST_CHECK(isFixed("UTC"_s) && isFixed("now"_s) && isFixed("parse"_s) && isFixed("call"_s) && !isFixed("7"_s) && !isFixed("nameNobodyHas"_s));
}

// A pinned property table does not mean one owner: a run of properties and a cached list of additions give every object with those names the same Structure.
void checkLockLeavesObjectsWithSameStructure(Realms& realms)
{
    VM& vm = realms.vm();
    Structure* empty = realms.plain()->objectStructureForObjectConstructor();
    Identifier now = Identifier::fromString(vm, "now"_s);
    Identifier addedByRun = Identifier::fromString(vm, "addedByRun"_s);
    Identifier addedByList = Identifier::fromString(vm, "addedByList"_s);
    UniquedStringImpl* namesOfRun[] = { now.impl(), addedByRun.impl() };
    UniquedStringImpl* namesOfList[] = { now.impl(), addedByList.impl() };
    const PropertyRun* run = vm.propertyRuns.intern(namesOfRun);
    AOT_TEST_CHECK(run);
    const uint32_t fixed[] = { Builtins::DateNow.index() };

    for (bool locksEveryProperty : { false, true }) {
        for (bool usesRun : { true, false }) {
            auto findStructure = [&] {
                return usesRun ? Structure::addPropertyRunTransition(vm, empty, *run) : Structure::createWithProperties(vm, empty, namesOfList);
            };
            Structure* shared = findStructure();
            AOT_TEST_CHECK(shared && shared->isPinnedPropertyTable() && !shared->isDictionary());
            JSObject* locked = constructEmptyObject(vm, shared);
            JSObject* other = constructEmptyObject(vm, shared);
            AOT_TEST_CHECK(locksEveryProperty ? locked->makePropertiesImmutable(vm) : locked->makePropertiesImmutable(vm, immutablePropertyNamesOf(vm, Builtins::Date, fixed).get()));
            AOT_TEST_CHECK(locked->structure() != shared && locked->structure()->isImmutableProperty(now.impl()));

            JSObject* later = constructEmptyObject(vm, findStructure());
            for (JSObject* object : { other, later }) {
                AOT_TEST_CHECK(object->structure() == shared);
                AOT_TEST_CHECK(!object->hasImmutableProperties() && !shared->isImmutableProperty(now.impl()) && shared->isStructureExtensible());
            }
        }
    }
}

// Whoever has found a fixed name keeps its offset, also where a name below it is deleted and the storage of the holder is packed anew.
void checkFixedNamesKeepOffsetsWhenFlattened(Realms& realms)
{
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    JSObject* object = realms.object(realm, "({ nameNobodyHas: 1, otherNameNobodyHas: 2, now: 3, parse: 4 })"_s);
    const uint32_t fixed[] = { Builtins::DateNow.index() };
    AOT_TEST_CHECK(object->makePropertiesImmutable(vm, immutablePropertyNamesOf(vm, Builtins::Date, fixed).get()));
    Identifier now = Identifier::fromString(vm, "now"_s);
    PropertyOffset offset = object->structure()->get(vm, now);
    AOT_TEST_CHECK(isValidOffset(offset) && object->structure()->isImmutableProperty(now.impl()));

    object->convertToUncacheableDictionary(vm);
    AOT_TEST_CHECK(JSCell::deleteProperty(object, realm, Identifier::fromString(vm, "nameNobodyHas"_s)));
    AOT_TEST_CHECK(object->structure()->isUncacheableDictionary());
    object->flattenDictionaryObject(vm);
    AOT_TEST_CHECK(!object->structure()->isDictionary());
    AOT_TEST_CHECK(object->structure()->get(vm, now) == offset && object->getDirect(offset) == jsNumber(3));
    AOT_TEST_CHECK(object->getDirect(vm, Identifier::fromString(vm, "otherNameNobodyHas"_s)) == jsNumber(2) && object->getDirect(vm, Identifier::fromString(vm, "parse"_s)) == jsNumber(4));
    AOT_TEST_CHECK(!object->getDirect(vm, Identifier::fromString(vm, "nameNobodyHas"_s)));

    object->putDirect(vm, Identifier::fromString(vm, "thirdNameNobodyHas"_s), jsNumber(5));
    AOT_TEST_CHECK(object->structure()->get(vm, now) == offset && object->getDirect(offset) == jsNumber(3));
    AOT_TEST_CHECK(object->getDirect(vm, Identifier::fromString(vm, "thirdNameNobodyHas"_s)) == jsNumber(5));
}

bool isPristine(JSObject* object)
{
    AOT_TEST_CHECK(object->structure()->isPristine() == object->isPristine());
    return object->isPristine();
}

std::span<const ASCIILiteral> replaceableMembersOf(Realms& realms, JSObject* object)
{
    for (const ObjectWithLockedMembers& candidate : objectsWithLockedMembers) {
        if (!candidate.replaceableMembers.empty() && realms.object(realms.locked(), candidate.source) == object)
            return candidate.replaceableMembers;
    }
    return { };
}

void checkPristineInstances(Realms& realms)
{
    VM& vm = realms.vm();
    for (const InstanceOfBuiltinClass& expected : instancesOfBuiltinClasses) {
        JSObject* instance = realms.object(realms.locked(), expected.source);
        AOT_TEST_CHECK_FOR_SOURCE(isPristine(instance) == expected.isPristine, expected.source);
        AOT_TEST_CHECK_FOR_SOURCE(!isPristine(realms.object(realms.plain(), expected.source)), expected.source);
        if (!expected.isPristine)
            continue;
        for (JSObject* prototype = instance->getPrototypeDirect().getObject(); prototype; prototype = prototype->getPrototypeDirect().getObject()) {
            checkMembers(vm, prototype, expected.source, replaceableMembersOf(realms, prototype));
            Structure* structure = prototype->structure();
            AOT_TEST_CHECK_FOR_SOURCE(structure->hasImmutablePrototype() && structure->hasImmutableIndexedProperties(), expected.source);
            for (const Identifier& key : { vm.propertyNames->then, vm.propertyNames->iteratorSymbol, vm.propertyNames->toPrimitiveSymbol })
                checkKey(structure->isImmutableProperty(key.impl()), expected.source, key, "can appear on a prototype"_s);
        }
    }
}

// An object that overrides put takes stores to itself, and to whatever lies below it, out of the inline caches: a realm that locks nothing must not pay for that.
void checkNativeConstructorsOverridePut(Realms& realms)
{
    for (ASCIILiteral source : { "Array"_s, "Map"_s, "Set"_s, "Error"_s, "TypeError"_s, "RegExp"_s, "Uint8Array"_s, "Intl.Collator"_s }) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.object(realms.locked(), source)->structure()->typeInfo().overridesPut(), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.object(realms.plain(), source)->structure()->typeInfo().overridesPut(), source);
    }
    for (JSGlobalObject* realm : { realms.locked(), realms.plain() })
        AOT_TEST_CHECK(!realm->functionPrototype()->structure()->typeInfo().overridesPut());
    for (ASCIILiteral source : { "Map.call = function () { }"_s, "Object.defineProperty(Map, 'apply', { value: 1 })"_s, "Object.setPrototypeOf(Map, { })"_s }) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsTypeError(realms.locked(), source), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.throwsTypeError(realms.plain(), source), source);
    }
    ASCIILiteral storesBelow = "(C => (C.first = 1, C.call = 2, C.first + C.call))(class extends Map { })"_s;
    for (JSGlobalObject* realm : { realms.locked(), realms.plain() })
        AOT_TEST_CHECK_FOR_SOURCE(realms.evaluate(realm, storesBelow) == jsNumber(3), storesBelow);
}

void checkStatements(Realms& realms, const Statements& statements)
{
    JSGlobalObject* locked = realms.locked();
    for (ASCIILiteral source : statements.throwing) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsTypeError(locked, source), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.throwsTypeError(realms.plain(), source), source);
    }
    for (ASCIILiteral source : statements.returningFalse)
        AOT_TEST_CHECK_FOR_SOURCE(realms.evaluate(locked, source) == jsBoolean(false), source);
    for (ASCIILiteral source : statements.returningTrue)
        AOT_TEST_CHECK_FOR_SOURCE(realms.evaluate(locked, source) == jsBoolean(true), source);
}

template<const Statements& statements>
void checkStatementsOf(Realms& realms)
{
    checkStatements(realms, statements);
}

using RealmsCheck = void (*)(Realms&);

template<RealmsCheck check>
void runInRealmsOfProgram(LoadedProgram& program)
{
    check(program.realms());
}

ASCIILiteral nameOfTest(void (*run)())
{
    for (const NativeTest& test : lockedRealmMemberTests()) {
        if (test.run == run)
            return test.name;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runInProcessWithSharedArrayBuffer(ASCIILiteral testName, std::span<const String> otherOptions)
{
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), "--useSharedArrayBuffer=1"_s };
    arguments.append(otherOptions);
    ChildProcessResult result = launcher.run(arguments.span());
    if (result.exitedWith(0))
        return;
    dataLogLn(result.output);
    dataLogLn("The process whose realms have SharedArrayBuffer ", result.description());
    failNativeTest(__FILE__, __LINE__, testName.characters());
}

} // anonymous namespace

template<RealmsCheck check>
static void withoutImage()
{
    if (!Options::useSharedArrayBuffer()) {
        runInProcessWithSharedArrayBuffer(nameOfTest(&withoutImage<check>), { });
        return;
    }
    Realms realms;
    check(realms);
}

template<RealmsCheck check>
static void withProgram()
{
    ASCIILiteral testName = nameOfTest(&withProgram<check>);
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programWithoutBuiltins, runInRealmsOfProgram<check>);
        return;
    }
    TemporaryFile image(".image"_s, compileProgramText(programWithoutBuiltins).span());
    String imageOption = makeString("--aotImagePath="_s, image.path());
    runInProcessWithSharedArrayBuffer(testName, singleElementSpan(imageOption));
}

std::span<const NativeTest> lockedRealmMemberTests()
{
    static constexpr NativeTest tests[] = {
        { "locked-realm-members/objects-with-locked-members"_s, withoutImage<checkObjectsWithLockedMembers> },
        { "locked-realm-members/function-prototype"_s, withoutImage<checkFunctionPrototype> },
        { "locked-realm-members/prototypes-of-function-kinds"_s, withoutImage<checkPrototypesOfFunctionKinds> },
        { "locked-realm-members/names-given-twice"_s, withoutImage<checkNamesGivenTwice> },
        { "locked-realm-members/lock-leaves-objects-with-same-structure"_s, withoutImage<checkLockLeavesObjectsWithSameStructure> },
        { "locked-realm-members/fixed-names-keep-their-offsets-when-the-holder-is-flattened"_s, withoutImage<checkFixedNamesKeepOffsetsWhenFlattened> },
        { "locked-realm-members/pristine-instances"_s, withoutImage<checkPristineInstances> },
        { "locked-realm-members/writes-to-string-prototype"_s, withoutImage<checkStatementsOf<statementsAboutStringPrototype>> },
        { "locked-realm-members/writes-to-regexp-prototype"_s, withoutImage<checkStatementsOf<statementsAboutRegExpPrototype>> },
        { "locked-realm-members/writes-to-function-prototype"_s, withoutImage<checkStatementsOf<statementsAboutFunctionPrototype>> },
        { "locked-realm-members/writes-to-species"_s, withoutImage<checkStatementsOf<statementsAboutSpecies>> },
        { "locked-realm-members/writes-to-locked-members"_s, withoutImage<checkStatementsOf<statementsAboutLockedMembers>> },
        { "locked-realm-members/writes-to-protocol-holders"_s, withoutImage<checkStatementsOf<statementsAboutProtocolHolders>> },
        { "locked-realm-members/start-up-locks-members"_s, withProgram<checkObjectsWithLockedMembers> },
        { "locked-realm-members/start-up-locks-function-prototype"_s, withProgram<checkFunctionPrototype> },
        { "locked-realm-members/start-up-locks-prototypes-of-function-kinds"_s, withProgram<checkPrototypesOfFunctionKinds> },
        { "locked-realm-members/start-up-marks-instances-pristine"_s, withProgram<checkPristineInstances> },
        { "locked-realm-members/program-writes-to-string-prototype"_s, withProgram<checkStatementsOf<statementsAboutStringPrototype>> },
        { "locked-realm-members/program-writes-to-regexp-prototype"_s, withProgram<checkStatementsOf<statementsAboutRegExpPrototype>> },
        { "locked-realm-members/program-writes-to-function-prototype"_s, withProgram<checkStatementsOf<statementsAboutFunctionPrototype>> },
        { "locked-realm-members/program-writes-to-species"_s, withProgram<checkStatementsOf<statementsAboutSpecies>> },
        { "locked-realm-members/program-writes-to-locked-members"_s, withProgram<checkStatementsOf<statementsAboutLockedMembers>> },
        { "locked-realm-members/program-writes-to-protocol-holders"_s, withProgram<checkStatementsOf<statementsAboutProtocolHolders>> },
        { "locked-realm-members/native-constructors-override-put-where-locked-alone"_s, withProgram<checkNativeConstructorsOverridePut> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
