/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCellFormat.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTRealmTestSupport.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "ArrayPrototype.h"
#include "JSCInlines.h"
#include "JSSentinel.h"
#include "ObjectPrototype.h"
#include "VMInlines.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> lockedRealmTests();

static constexpr ASCIILiteral programWithoutBuiltins =
    "# LIBRARY: own\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static constexpr ASCIILiteral programThatReadsDateNow =
    "# LIBRARY: own\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0() handler=b2():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"Date\"\n"
    "    v2 = PropGet v1 key=\"now\"\n"
    "    Jump -> next b1()\n"
    "  b1():\n"
    "    Return\n"
    "  b2() role=CatchHandler:\n"
    "    v3 = CatchValue\n"
    "    Return\n"
    "}\n"_s;

namespace {

struct CachedStoreCase {
    ASCIILiteral source;
    bool staysPristine;
};

struct LateMarkCase {
    std::span<const ASCIILiteral> names;
    bool isPristine;
    bool hasPristineElements;
};

constexpr ASCIILiteral alwaysLockedObjects[] = {
    "Object.prototype"_s,
    "RegExp.prototype"_s,
    "Promise.prototype"_s,
    "Object.getPrototypeOf(function* () { }).prototype"_s,
    "Object.getPrototypeOf(async function* () { }).prototype"_s,
};

constexpr ASCIILiteral constructorsWithLockedMembers[] = {
    "Object"_s, "Array"_s, "Number"_s, "String"_s, "Map"_s, "Set"_s, "RegExp"_s, "ArrayBuffer"_s, "Date"_s,
};

constexpr ASCIILiteral objectsWithoutLock[] = {
    "Function"_s, "Error"_s, "TypeError"_s, "Intl"_s, "WeakRef.prototype"_s, "Error.prototype"_s,
    "({})"_s, "[]"_s, "/a/"_s, "new Map"_s, "(function () { })"_s,
    "Object.create(Array.prototype)"_s, "(class A extends Array { }).prototype"_s, "(class R extends RegExp { }).prototype"_s,
};

constexpr ASCIILiteral throwingStatements[] = {
    "(function () { 'use strict'; Array.prototype.push = function () { }; })()"_s,
    "(function () { 'use strict'; delete Array.prototype.map; })()"_s,
    "Object.defineProperty(Array.prototype, 'map', { value: 1 })"_s,
    "(function () { 'use strict'; Object.prototype.nameNobodyHas = 1; })()"_s,
    "Object.defineProperty(Object.prototype, 'nameNobodyHas', { value: 1 })"_s,
    "Object.setPrototypeOf(Array.prototype, {})"_s,
    "(function () { 'use strict'; Array.prototype.__proto__ = {}; })()"_s,
    "(function () { 'use strict'; Array.prototype[3] = 1; })()"_s,
    "Object.defineProperty(Array.prototype, 0, { get() { } })"_s,
    "Object.defineProperty(Object.prototype, 0, { get() { } })"_s,
    "(function () { 'use strict'; Array.prototype.length = 3; })()"_s,
    "Object.defineProperty(Array.prototype, 'length', { value: 1 })"_s,
    "Array.prototype.push(1)"_s,
    "Object.assign(Array.prototype, { filter: 1 })"_s,
    "Error.captureStackTrace(Object.prototype)"_s,
    "(function () { 'use strict'; Promise.resolve = function () { }; })()"_s,
    "(function () { 'use strict'; Promise.prototype.then = function () { }; })()"_s,
    "(function () { 'use strict'; Symbol.for = function () { }; })()"_s,
    "(function () { 'use strict'; Object.getPrototypeOf([][Symbol.iterator]()).next = function () { }; })()"_s,
    "Object.defineProperty(Array, Symbol.species, { value: 1 })"_s,
    "(function () { 'use strict'; delete Array[Symbol.species]; })()"_s,
    "Object.defineProperty(globalThis, 'Symbol', { value: 1 })"_s,
    "(function () { 'use strict'; globalThis.Symbol = 1; })()"_s,
    "(function () { 'use strict'; Symbol = 1; })()"_s,
    "(function () { 'use strict'; delete globalThis.Symbol; })()"_s,
    "(function () { 'use strict'; Math.max = function () { }; })()"_s,
    "(function () { 'use strict'; delete Math.max; })()"_s,
    "Object.defineProperty(Math, 'max', { value: 1 })"_s,
    "Object.defineProperty(Math, 'toString', { value: 1 })"_s,
    "(function () { 'use strict'; globalThis.Math = { }; })()"_s,
    "(function () { 'use strict'; Object.keys = function () { }; })()"_s,
    "(function () { 'use strict'; Object.getOwnPropertyNames = function () { }; })()"_s,
    "(function () { 'use strict'; Array.isArray = function () { }; })()"_s,
    "(function () { 'use strict'; Number.isInteger = function () { }; })()"_s,
    "(function () { 'use strict'; Reflect.get = function () { }; })()"_s,
    "(function () { 'use strict'; Object = function () { }; })()"_s,
    "(function () { 'use strict'; JSON = { }; })()"_s,
};

constexpr ASCIILiteral throwingStatementsOfSloppyCode[] = {
    "Array.prototype.push = function () { }"_s,
    "delete Array.prototype.map"_s,
    "Object.prototype.nameNobodyHas = 1"_s,
    "Array.prototype[3] = 1"_s,
    "Array.prototype.length = 3"_s,
    "Array.prototype.__proto__ = {}"_s,
    "Promise.prototype.then = function () { }"_s,
    "globalThis.Symbol = 1"_s,
    "Symbol = 1"_s,
    "Math.max = function () { }"_s,
    "Math = 1"_s,
    "Object.keys = function () { }"_s,
    "Object.getOwnPropertyNames = function () { }"_s,
};

constexpr ASCIILiteral statementsReturningFalse[] = {
    "Reflect.set(Array.prototype, 'map', 1)"_s,
    "Reflect.defineProperty(Array.prototype, 'map', { value: 1 })"_s,
    "Reflect.deleteProperty(Array.prototype, 'map')"_s,
    "Reflect.set(Object.prototype, 'nameNobodyHas', 1)"_s,
    "Reflect.set(Array.prototype, '0', 1)"_s,
    "Reflect.set(Array.prototype, 'length', 3)"_s,
    "Reflect.defineProperty(Array.prototype, 'length', { value: 1 })"_s,
    "Reflect.setPrototypeOf(Array.prototype, {})"_s,
    "Object.isExtensible(Object.prototype)"_s,
    "Object.isFrozen(Object.prototype)"_s,
    "Object.isFrozen(Array.prototype)"_s,
    "Object.prototype.hasOwnProperty('stack')"_s,
    "Object.prototype.hasOwnProperty('nameNobodyHas')"_s,
    "Reflect.set(globalThis, 'Symbol', 1)"_s,
    "Reflect.defineProperty(globalThis, 'Symbol', { value: 1 })"_s,
    "Reflect.deleteProperty(globalThis, 'Symbol')"_s,
    "Reflect.set(globalThis, 'globalThis', 1)"_s,
    "Reflect.defineProperty(globalThis, 'globalThis', { value: 1 })"_s,
    "Reflect.deleteProperty(globalThis, 'globalThis')"_s,
    "Reflect.defineProperty(Array, Symbol.species, { value: 1 })"_s,
};

constexpr ASCIILiteral statementsReturningTrue[] = {
    "Object.getOwnPropertyDescriptor(Array.prototype, 'map').writable"_s,
    "Object.getOwnPropertyDescriptor(Array.prototype, 'map').configurable"_s,
    "Object.getOwnPropertyDescriptor(globalThis, 'Symbol').writable"_s,
    "Reflect.defineProperty(Array.prototype, 'map', { value: Array.prototype.map })"_s,
    "Reflect.defineProperty(globalThis, 'Symbol', { value: Symbol })"_s,
    "Reflect.setPrototypeOf(Array.prototype, Object.prototype)"_s,
    "Reflect.preventExtensions(Object.prototype)"_s,
    "Reflect.deleteProperty(Object.prototype, 'nameNobodyHas')"_s,
    "Object.isExtensible(globalThis) && Object.isExtensible(Array) && Object.isExtensible(Array.prototype)"_s,
    "(Array.prototype.nameNobodyHas = 1, [].nameNobodyHas === 1)"_s,
    "delete Array.prototype.nameNobodyHas && !('nameNobodyHas' in Array.prototype)"_s,
    "(Symbol.nameNobodyHas = 1, Symbol.nameNobodyHas === 1)"_s,
    "(Promise.nameNobodyHas = 1, Promise.nameNobodyHas === 1)"_s,
    "(globalThis.nameNobodyHas = 1, nameNobodyHas === 1)"_s,
    "(WeakRef = 1, WeakRef === 1)"_s,
    "(console = 1, console === 1)"_s,
    "(Array.nameNobodyHas = 1, Array.nameNobodyHas === 1)"_s,
    "(f => (Date.now = f, Date.now === f))(function () { })"_s,
    "(Math.nameNobodyHas = 1, Math.nameNobodyHas === 1)"_s,
    "(f => (Math.random = f, Math.random === f))(function () { })"_s,
    "((f, original) => (JSON.parse = f, JSON.parse === f && (JSON.parse = original, JSON.parse === original)))(function () { }, JSON.parse)"_s,
    "(Object.nameNobodyHas = 1, Object.nameNobodyHas === 1)"_s,
    "(function () { 'use strict'; Math.max = Math.max; Object.assign = Object.assign || 1; Array.prototype.push = Array.prototype.push; globalThis.Symbol = Symbol; Math = Math; return true; })()"_s,
    "Reflect.set(Math, 'max', Math.max)"_s,
    "((o, f) => (o.toString = f, o.toString === f))({}, function () { })"_s,
    "((a, f) => (a.push = f, a.push === f))([], function () { })"_s,
    "(f => { class A extends Array { } A.prototype.map = f; return new A().map === f; })(function () { })"_s,
    "new (class A extends Array { map() { return 1; } })().map() === 1"_s,
    "(o => (o.__proto__ = Array.prototype, Object.getPrototypeOf(o) === Array.prototype))({})"_s,
    "(o => (o.length = 1, o.hasOwnProperty('length')))(Object.create(Array.prototype))"_s,
    "(o => (Error.captureStackTrace(o), o.hasOwnProperty('stack')))({})"_s,
};

constexpr ASCIILiteral declarationsShadowingLockedMembers[] = {
    "let Symbol = 1"_s,
    "const Math = { }"_s,
    "class Map { }"_s,
    "let globalThis = { }"_s,
    "'use strict'; let Array"_s,
    "let declaredBesideObject = 1, Object = 2"_s,
    "var declaredBesideSet = 1; const Set = 2"_s,
};

constexpr ASCIILiteral declarationsReplacingLockedMembers[] = {
    "function Atomics() { }"_s,
    "'use strict'; var Reflect = 1"_s,
    "var JSON = 1"_s,
};

constexpr ASCIILiteral scriptsReturningTrue[] = {
    "typeof declaredBesideObject === 'undefined' && !('declaredBesideSet' in globalThis)"_s,
    "typeof Symbol() === 'symbol' && Math.max(1, 2) === 2 && new Map().size === 0 && globalThis.Math === Math && Array.isArray([])"_s,
    "let WeakRef = 1; WeakRef === 1"_s,
    "const declaredByScript = 1; declaredByScript === 1"_s,
    "class ClassOfScript { } typeof ClassOfScript === 'function'"_s,
    "WeakRef === 1 && typeof globalThis.WeakRef === 'function' && declaredByScript === 1 && typeof ClassOfScript === 'function' && !('declaredByScript' in globalThis)"_s,
    "{ let Symbol = 1; class Map { } Symbol === 1 }"_s,
    "(function () { let Math = 1; const globalThis = 2; return Math + globalThis === 3; })()"_s,
};

constexpr ASCIILiteral pristineArrays[] = {
    "[]"_s, "[1]"_s, "[1.5]"_s, "['a']"_s, "[, 1]"_s, "[1, 2, 3]"_s, "new Array(5)"_s, "Array.of(1)"_s,
    "(a => (a.push(1), a))([])"_s,
    "(a => (a.push(1, 1.5), a))([])"_s,
    "(a => (a.foo = 1, a))([1, 2])"_s,
    "(a => (a.foo = 1, delete a.foo, a))([1, 2])"_s,
    "(a => (a.foo = 1, Object.defineProperty(a, 'foo', { enumerable: false })))([1, 2])"_s,
    "(a => (Object.create(a), a))([1, 2])"_s,
};

constexpr ASCIILiteral pristineContiguousArrays[] = {
    "Array.of('a')"_s, "['a'].slice()"_s, "['a', 'b'].slice(1)"_s, "[{ }]"_s, "[[]]"_s, "[null, 'a']"_s,
    "'a,b'.split(',')"_s, "Object.keys({ a: 1 })"_s, "JSON.parse('[\"a\", { }]')"_s,
    "(a => (a.push('b'), a))(['a'])"_s,
    "(a => (a.push(1, 1.5, 'a'), a))([])"_s,
    "(a => (a.foo = 1, a))(Array.of('a'))"_s,
};

constexpr ASCIILiteral pristineRegExpMatchArrays[] = {
    "/a/.exec('a')"_s, "/a/d.exec('a')"_s, "/a/d.exec('a').indices"_s, "'a'.match(/a/)"_s,
};

constexpr ASCIILiteral pristineArraysWithMatchArrayNames[] = {
    "(a => (a.index = 0, a))(['a'].slice())"_s,
    "(a => (a.index = 0, a.input = 'a', a))(['a'].slice())"_s,
    "(a => (a.index = 0, a.input = 'a', a.groups = undefined, a))(['a'].slice())"_s,
    "(a => (a.groups = undefined, a))(['a'].slice())"_s,
    "(a => (a.index = 0, a))(['a'])"_s,
    "(a => (a.index = 0, a))([{ }])"_s,
    "(a => (a.index = 0, a))([1])"_s,
};

constexpr ASCIILiteral arraysWithNumericNames[] = {
    "(a => (a['-1'] = 1, a))(['a'])"_s,
    "(a => (a['1.5'] = 1, a))(['a'])"_s,
    "(a => (a.tag = 1, a['-1'] = 1, a))([1])"_s,
};

constexpr ASCIILiteral harmlessNames[] = { "other"_s, "index"_s };
constexpr ASCIILiteral shadowingNames[] = { "map"_s, "other"_s };
constexpr ASCIILiteral probedNames[] = { "then"_s };
constexpr ASCIILiteral numericNames[] = { "-1"_s, "other"_s };
constexpr ASCIILiteral shadowingAndNumericNames[] = { "map"_s, "1.5"_s };

constexpr LateMarkCase lateMarkCases[] = {
    { harmlessNames, true, true },
    { shadowingNames, false, true },
    { probedNames, false, true },
    { numericNames, true, false },
    { shadowingAndNumericNames, false, false },
};

constexpr IndexingType indexingTypesOfPristineStructures[] = {
    ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous,
    CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous,
};

constexpr ASCIILiteral pristineArraysInArrayStorage[] = {
    "(a => (a[100000] = 1, a))([])"_s,
    "Object.preventExtensions([1, 2])"_s, "Object.seal([])"_s, "Object.freeze([])"_s,
};

constexpr ASCIILiteral pristinePromises[] = {
    "Promise.resolve(1)"_s, "new Promise(() => { })"_s, "(async () => { })()"_s,
    "(p => (p.foo = 1, p))(Promise.resolve(1))"_s,
};

constexpr ASCIILiteral pristineCollectionsAndRegExps[] = {
    "new Map"_s, "new Set"_s, "/a/"_s,
};

constexpr ASCIILiteral pristineTypedArrays[] = {
    "new Uint8Array(2)"_s, "new Float64Array(2)"_s, "new Uint8Array(new ArrayBuffer(8), 2)"_s,
    "(u => (u.foo = 1, u))(new Uint8Array(2))"_s,
};

constexpr ASCIILiteral pristineIterators[] = {
    "[1].values()"_s, "[1][Symbol.iterator]()"_s, "new Map().entries()"_s, "new Set().values()"_s, "'a'[Symbol.iterator]()"_s, "'a'.matchAll(/a/g)"_s,
    "[1].values().map(x => x)"_s, "Iterator.from({ next() { } })"_s,
    "(i => (i.foo = 1, i))([1].values())"_s,
};

constexpr ASCIILiteral instancesOfOtherClasses[] = {
    "new Map"_s, "new Set"_s, "/a/"_s, "new String('a')"_s, "new Error('a')"_s, "new Date(0)"_s, "new Uint8Array(2)"_s, "new WeakMap"_s,
};

constexpr ASCIILiteral nonPristineValues[] = {
    "(a => (a.map = 1, a))([1, 2])"_s,
    "(a => (a.constructor = 1, a))([1, 2])"_s,
    "(a => (a.hasOwnProperty = 1, a))([1, 2])"_s,
    "(a => (a[Symbol.iterator] = function () { }, a))([1, 2])"_s,
    "(a => (a[Symbol.for('x')] = 1, a))([1, 2])"_s,
    "(a => (a.toJSON = function () { }, a))([1, 2])"_s,
    "(a => (a.then = function () { }, a))([1, 2])"_s,
    "Object.setPrototypeOf([1, 2], null)"_s,
    "Object.setPrototypeOf([1, 2], Object.create(Array.prototype))"_s,
    "Object.defineProperty([1, 2], 0, { get() { } })"_s,
    "(a => { for (var i = 0; i < 200; i++) a['p' + i] = i; return a; })([1, 2])"_s,
    "new (class A extends Array { })()"_s, "Reflect.construct(Array, [], Object)"_s,
    "(function () { return arguments; })(1, 2)"_s,
    "(u => (u.subarray = 1, u))(new Uint8Array(2))"_s,
    "(u => (u.hasOwnProperty = 1, u))(new Uint8Array(2))"_s,
    "Object.setPrototypeOf(new Uint8Array(2), Object.prototype)"_s,
    "new (class U extends Uint8Array { })(2)"_s,
    "new Error('a')"_s, "new WeakRef({})"_s, "(d => (d.toJSON = 1, d))(new Date(0))"_s,
    "(p => (p.constructor = 1, p))(Promise.resolve(1))"_s,
    "(p => (p.then = function () { }, p))(Promise.resolve(1))"_s,
    "Object.setPrototypeOf(Promise.resolve(1), {})"_s,
    "new (class P extends Promise { })(() => { })"_s,
    "(i => (i.next = 1, i))([1].values())"_s,
    "(i => (i[Symbol.iterator] = 1, i))(new Map().keys())"_s,
    "Object.setPrototypeOf(new Set().values(), null)"_s,
    "Object.setPrototypeOf('a'[Symbol.iterator](), Object.getPrototypeOf([].values()))"_s,
    "(function* () { })()"_s, "(async function* () { })()"_s, "new (class I extends Iterator { })()"_s,
    "({})"_s, "new Object"_s,
};

constexpr CachedStoreCase cachedStoreCases[] = {
    { "storeTwice(new Function('a', 'a.tag = 1; a.other = 2; return a;'))"_s, true },
    { "storeTwice(new Function('a', 'a.tag = 1; a.map = function () { }; return a;'))"_s, false },
    { "storeTwice(new Function('a', 'a.tag = 1; a.push = function () { }; return a;'))"_s, false },
    { "storeTwice(new Function('a', 'a.tag = 1; a.then = function () { }; return a;'))"_s, false },
    { "storeTwice(new Function('a', 'a.tag = 1; a.toJSON = function () { }; return a;'))"_s, false },
    { "storeTwice((() => { class Base { constructor(a) { a.tag = 1; return a; } } class Stamp extends Base { #name = 1; } return a => new Stamp(a); })())"_s, false },
};

bool isPristine(VM& vm, JSValue value)
{
    JSCell* cell = value.asCell();
    auto* array = dynamicDowncast<JSArray>(cell);
    AOT_TEST_CHECK(cell->isPristine() == (cell->structure()->isPristine() && (!array || cell->structure()->hasPristineElements())));
    if (array)
        array->validatePristine(vm);
    return cell->structure()->isPristine();
}

bool hasOnlyLockedPrototypes(JSObject* object)
{
    for (JSObject* prototype = object->getPrototypeDirect().getObject(); prototype; prototype = prototype->getPrototypeDirect().getObject()) {
        Structure* structure = prototype->structure();
        if (!structure->hasImmutablePrototype() || !structure->hasImmutableIndexedProperties())
            return false;
    }
    return true;
}

void checkPristineOnlyInLockedRealm(Realms& realms, std::span<const ASCIILiteral> sources)
{
    for (ASCIILiteral source : sources) {
        AOT_TEST_CHECK_FOR_SOURCE(isPristine(realms.vm(), realms.evaluate(realms.locked(), source)), source);
        AOT_TEST_CHECK_FOR_SOURCE(!isPristine(realms.vm(), realms.evaluate(realms.plain(), source)), source);
    }
}

void checkOriginalArrayStructures(Realms& realms)
{
    for (IndexingType indexingType : indexingTypesOfPristineStructures) {
        Structure* original = realms.locked()->originalArrayStructureForIndexingType(indexingType);
        if (!original->isPristine() || !original->hasPristineElements()) {
            dataLogLn("The original array structure with indexing type ", indexingType, " is not pristine");
            failNativeTest(__FILE__, __LINE__, "every original array structure of dense arrays is pristine");
        }
        Structure* originalInPlainRealm = realms.plain()->originalArrayStructureForIndexingType(indexingType);
        AOT_TEST_CHECK(!originalInPlainRealm->isPristine() && !originalInPlainRealm->hasPristineElements());
    }
    AOT_TEST_CHECK(!realms.locked()->originalArrayStructureForIndexingType(ArrayWithSlowPutArrayStorage)->isPristine());
}

bool hasBothPristineBits(Structure* structure)
{
    return structure->isPristine() && structure->hasPristineElements();
}

bool hasNoPristineBit(Structure* structure)
{
    return !structure->isPristine() && !structure->hasPristineElements();
}

void checkArraysWithMatchArrayNames(Realms& realms)
{
    checkPristineOnlyInLockedRealm(realms, pristineArraysWithMatchArrayNames);
    for (ASCIILiteral source : pristineArraysWithMatchArrayNames)
        AOT_TEST_CHECK_FOR_SOURCE(realms.object(realms.locked(), source)->structure()->hasPristineElements(), source);

    JSGlobalObject* locked = realms.locked();
    Structure* original = locked->originalArrayStructureForIndexingType(ArrayWithContiguous);
    for (Structure* matchArrayStructure : { locked->regExpMatchesArrayStructure(), locked->regExpMatchesArrayWithIndicesStructure(), locked->regExpMatchesIndicesArrayStructure() }) {
        for (Structure* structure = matchArrayStructure; structure != original; structure = structure->previousID()) {
            AOT_TEST_CHECK(hasBothPristineBits(structure));
            AOT_TEST_CHECK(structure->isCacheableTransitionFrom(structure->previousID()));
        }
    }
}

Structure* createUnmarkedArrayStructure(Realms& realms)
{
    JSGlobalObject* locked = realms.locked();
    Structure* original = locked->originalArrayStructureForIndexingType(ArrayWithContiguous);
    Structure* structure = Structure::create(realms.vm(), locked, locked->arrayPrototype(), original->typeInfo(), original->classInfoForCells(), original->indexingType());
    AOT_TEST_CHECK(hasNoPristineBit(structure));
    return structure;
}

Structure* addName(VM& vm, Structure* structure, ASCIILiteral name)
{
    PropertyOffset offset;
    return Structure::addPropertyTransition(vm, structure, Identifier::fromString(vm, name), 0, offset);
}

bool isPristineAfterLateMark(ASCIILiteral store)
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    AOT_TEST_CHECK(realm->objectPrototype()->makePropertiesImmutable(vm) && realm->arrayPrototype()->makePropertiesImmutable(vm));
    realms.evaluate(realm, store);
    realms.evaluate(realm, "for (var i = 0; i < 10; ++i) store(['a'].slice());"_s);
    realms.object(realm, "(a => (a.tag = 1, a))(['a'].slice())"_s)->structure()->tryMarkPristine(vm);
    realm->originalArrayStructureForIndexingType(ArrayWithContiguous)->tryMarkPristine(vm);
    return isPristine(vm, realms.evaluate(realm, "store(['a'].slice())"_s));
}

void checkAlwaysLockedObjects(Realms& realms)
{
    for (ASCIILiteral source : alwaysLockedObjects) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.holder(realms.locked(), source)->structure()->hasImmutableProperties(), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.holder(realms.plain(), source)->structure()->mayHaveImmutableProperties(), source);
    }
    for (ASCIILiteral source : objectsWithoutLock)
        AOT_TEST_CHECK_FOR_SOURCE(!realms.holder(realms.locked(), source)->structure()->mayHaveImmutableProperties(), source);

    unsigned numberOfLockedObjects = 0;
    forEachLockedObject(realms.locked(), [&](BuiltinID, JSObject* object) {
        AOT_TEST_CHECK(object->structure()->hasImmutableProperties());
        ++numberOfLockedObjects;
    });
    AOT_TEST_CHECK(numberOfLockedObjects >= std::size(alwaysLockedObjects));
    forEachLockedObject(realms.plain(), [&](BuiltinID, JSObject* object) {
        AOT_TEST_CHECK(!object->structure()->mayHaveImmutableProperties());
    });
}

void checkAlwaysLockedMembers(Realms& realms)
{
    VM& vm = realms.vm();
    Structure* global = realms.locked()->structure();
    AOT_TEST_CHECK(global->isImmutableProperty(Identifier::fromString(vm, "Symbol"_s).impl()));
    AOT_TEST_CHECK(global->isImmutableProperty(vm.propertyNames->globalThis.impl()));
    AOT_TEST_CHECK(!global->isImmutableProperty(Identifier::fromString(vm, "WeakRef"_s).impl()));
    AOT_TEST_CHECK(!global->isImmutableProperty(Identifier::fromString(vm, "nameNobodyHas"_s).impl()));
    AOT_TEST_CHECK(!global->hasImmutableProperties() && !global->hasImmutableIndexedProperties() && global->isStructureExtensible());
    for (ASCIILiteral source : constructorsWithLockedMembers) {
        Structure* constructor = realms.object(realms.locked(), source)->structure();
        AOT_TEST_CHECK_FOR_SOURCE(constructor->isImmutableProperty(vm.propertyNames->speciesSymbol.impl()), source);
        AOT_TEST_CHECK_FOR_SOURCE(constructor->isImmutableProperty(vm.propertyNames->name.impl()) && constructor->isImmutableProperty(vm.propertyNames->iteratorSymbol.impl()), source);
        AOT_TEST_CHECK_FOR_SOURCE(!constructor->hasImmutableProperties() && constructor->hasImmutablePrototype() && !constructor->hasImmutableIndexedProperties(), source);
        AOT_TEST_CHECK_FOR_SOURCE(constructor->isStructureExtensible(), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.object(realms.plain(), source)->structure()->mayHaveImmutableProperties(), source);
    }
    AOT_TEST_CHECK(realms.object(realms.locked(), "Array"_s)->structure()->isImmutableProperty(vm.propertyNames->from.impl()));
    AOT_TEST_CHECK(!realms.object(realms.locked(), "Date"_s)->structure()->isImmutableProperty(vm.propertyNames->now.impl()));
    AOT_TEST_CHECK(!realms.plain()->structure()->mayHaveImmutableProperties());
}

void checkLanguageBehavior(Realms& realms)
{
    JSGlobalObject* locked = realms.locked();
    JSValue map = realms.evaluate(locked, "Array.prototype.map"_s);
    JSValue push = realms.evaluate(locked, "Array.prototype.push"_s);
    JSValue then = realms.evaluate(locked, "Promise.prototype.then"_s);
    JSValue symbol = realms.evaluate(locked, "Symbol"_s);
    IndexingType indexingMode = locked->arrayPrototype()->indexingMode();
    for (ASCIILiteral source : throwingStatements) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsTypeError(locked, source), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.throwsTypeError(realms.plain(), source), source);
    }
    for (ASCIILiteral source : throwingStatementsOfSloppyCode)
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsTypeError(locked, source), source);
    for (ASCIILiteral source : statementsReturningFalse)
        AOT_TEST_CHECK_FOR_SOURCE(realms.evaluate(locked, source) == jsBoolean(false), source);
    for (ASCIILiteral source : statementsReturningTrue)
        AOT_TEST_CHECK_FOR_SOURCE(realms.evaluate(locked, source) == jsBoolean(true), source);
    AOT_TEST_CHECK(realms.evaluate(locked, "Array.prototype.map"_s) == map);
    AOT_TEST_CHECK(realms.evaluate(locked, "Array.prototype.push"_s) == push);
    AOT_TEST_CHECK(realms.evaluate(locked, "Promise.prototype.then"_s) == then);
    AOT_TEST_CHECK(realms.evaluate(locked, "Symbol"_s) == symbol);
    AOT_TEST_CHECK(realms.evaluate(locked, "Object.getPrototypeOf(Array.prototype) === Object.prototype"_s) == jsBoolean(true));
    AOT_TEST_CHECK(!locked->arrayPrototype()->length() && locked->arrayPrototype()->indexingMode() == indexingMode);
    AOT_TEST_CHECK(locked->isArrayPrototypeIteratorProtocolFastAndNonObservable());
}

void checkDeclarationsOfScripts(Realms& realms)
{
    for (ASCIILiteral source : declarationsShadowingLockedMembers) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsSyntaxError(realms.locked(), source), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.throwsSyntaxError(realms.plain(), source), source);
    }
    for (ASCIILiteral source : declarationsReplacingLockedMembers) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsTypeError(realms.locked(), source), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.throwsTypeError(realms.plain(), source), source);
    }
    for (ASCIILiteral source : scriptsReturningTrue)
        AOT_TEST_CHECK_FOR_SOURCE(realms.evaluate(realms.locked(), source) == jsBoolean(true), source);
}

bool isListedAsConstant(LoadedProgram& program, BuiltinID member)
{
    std::span<const uint32_t> members = program.instance().tables().constantBuiltinMembers();
    return std::ranges::find(members, member.index()) != members.end();
}

} // anonymous namespace

static void testAlwaysLockedObjectsAreImmutable()
{
    Realms realms;
    checkAlwaysLockedObjects(realms);
}

static void testAlwaysLockedMembersAreImmutable()
{
    Realms realms;
    checkAlwaysLockedMembers(realms);
}

static void testLanguageBehavior()
{
    Realms realms;
    checkLanguageBehavior(realms);
}

static void testDeclarationsOfScripts()
{
    Realms realms;
    checkDeclarationsOfScripts(realms);
    AOT_TEST_CHECK(realms.evaluate(realms.locked(), "(0, eval)('let Math = 1; Math === 1')"_s) == jsBoolean(true));
}

static void testVMStateWithoutImage()
{
    Realms realms;
    VM& vm = realms.vm();
    VMState& state = vm.aotState();
    AOT_TEST_CHECK(!vm.hasCompiledCode() && !state.hasCompiledCode);
    AOT_TEST_CHECK(!state.image && !state.validatesRuntime && !state.structureIDBase);
    AOT_TEST_CHECK(!state.runtimeTable && !state.identifiers && !state.caches && !state.propertyCaches && !state.counters && !state.reporter);
    AOT_TEST_CHECK(!state.callSiteCache && !state.functionTables && !state.regExpLiterals && !state.regExpMatchOutput);
    AOT_TEST_CHECK(!state.moduleRecordStructure && !state.stringBuilderStorageStructure);
    AOT_TEST_CHECK(state.instances.isEmpty() && state.instancesToDestroy.isEmpty());
    AOT_TEST_CHECK(!Instance::of(realms.locked()) && !Instance::of(realms.plain()));
}

static void testValidatorsPassFreshRealm()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* locked = realms.locked();
    for (ASCIILiteral source : { "'abc'"_s, "(s => s + s)('0123456789abcdef')"_s, "Symbol('a')"_s, "(b => (new Map().set(b, 1), b))(2n ** 70n)"_s, "({})"_s, "[1, 2, 3]"_s })
        validateShapeWord(realms.evaluate(locked, source).asCell());
    validateShapeWord(locked->objectPrototype()->structure());
    validateShapeWord(vm.fastArrayValuesSentinel());
    realms.array(locked, "[1, 2, 3]"_s)->validatePristine(vm);
}

static void testPristineArrays()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristineArrays);
}

static void testOriginalArrayStructuresArePristine()
{
    Realms realms;
    checkOriginalArrayStructures(realms);
}

static void testPristineContiguousArrays()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristineContiguousArrays);
}

static void testPristineRegExpMatchArrays()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristineRegExpMatchArrays);
}

static void testNamesOfMatchArraysKeepArraysPristine()
{
    Realms realms;
    checkArraysWithMatchArrayNames(realms);
}

static void testNumericNamesEndPristineElements()
{
    Realms realms;
    VM& vm = realms.vm();
    for (ASCIILiteral source : arraysWithNumericNames) {
        JSObject* array = realms.object(realms.locked(), source);
        AOT_TEST_CHECK_FOR_SOURCE(isPristine(vm, array) && !array->structure()->hasPristineElements(), source);
    }

    Structure* marked = addName(vm, createUnmarkedArrayStructure(realms), "-1"_s);
    AOT_TEST_CHECK(marked->tryMarkPristine(vm));
    AOT_TEST_CHECK(marked->isPristine() && !marked->hasPristineElements());
}

static void testMarkReachesExistingTransitions()
{
    Realms realms;
    VM& vm = realms.vm();
    Structure* root = createUnmarkedArrayStructure(realms);
    Structure* tagged = addName(vm, root, "tag"_s);
    Structure* taggedTwice = addName(vm, tagged, "other"_s);
    PropertyOffset offset;
    Structure* untagged = Structure::removePropertyTransition(vm, tagged, Identifier::fromString(vm, "tag"_s), offset);
    Structure* sealed = Structure::sealTransition(vm, tagged);
    Structure* withOtherPrototype = nullptr;
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, root);
        withOtherPrototype = Structure::changePrototypeTransition(vm, root, realms.locked()->objectPrototype(), deferred);
    }
    for (Structure* structure : { root, tagged, taggedTwice, untagged, sealed, withOtherPrototype })
        AOT_TEST_CHECK(hasNoPristineBit(structure));

    AOT_TEST_CHECK(root->tryMarkPristine(vm));
    for (Structure* structure : { root, tagged, taggedTwice, untagged, sealed })
        AOT_TEST_CHECK(hasBothPristineBits(structure));
    AOT_TEST_CHECK(hasNoPristineBit(withOtherPrototype));

    AOT_TEST_CHECK(hasBothPristineBits(addName(vm, taggedTwice, "third"_s)));
    Structure* shadowing = addName(vm, taggedTwice, "map"_s);
    AOT_TEST_CHECK(!shadowing->isPristine() && shadowing->hasPristineElements());
}

static void testMarkLeavesOutBitsLostByExistingTransitions()
{
    Realms realms;
    VM& vm = realms.vm();
    for (const LateMarkCase& markCase : lateMarkCases) {
        Structure* root = createUnmarkedArrayStructure(realms);
        Structure* tagged = addName(vm, root, "tag"_s);
        Vector<Structure*, 4> structures { root, tagged };
        for (ASCIILiteral name : markCase.names)
            structures.append(addName(vm, tagged, name));

        AOT_TEST_CHECK_FOR_SOURCE(root->tryMarkPristine(vm) == markCase.isPristine, markCase.names[0]);
        for (Structure* structure : structures) {
            AOT_TEST_CHECK_FOR_SOURCE(structure->isPristine() == markCase.isPristine && structure->hasPristineElements() == markCase.hasPristineElements, markCase.names[0]);
            AOT_TEST_CHECK_FOR_SOURCE(structure == root || structure->isCacheableTransitionFrom(structure->previousID()), markCase.names[0]);
        }
    }
}

static void testMarkFollowsStructuresMarkedEarlier()
{
    Realms realms;
    VM& vm = realms.vm();

    Structure* root = createUnmarkedArrayStructure(realms);
    Structure* tagged = addName(vm, root, "tag"_s);
    Structure* shadowing = addName(vm, tagged, "map"_s);
    AOT_TEST_CHECK(!tagged->tryMarkPristine(vm));
    AOT_TEST_CHECK(!root->tryMarkPristine(vm));
    for (Structure* structure : { root, tagged, shadowing })
        AOT_TEST_CHECK(!structure->isPristine() && structure->hasPristineElements());

    root = createUnmarkedArrayStructure(realms);
    tagged = addName(vm, root, "tag"_s);
    AOT_TEST_CHECK(tagged->tryMarkPristine(vm));
    AOT_TEST_CHECK(hasNoPristineBit(root) && hasBothPristineBits(tagged));
    shadowing = addName(vm, tagged, "map"_s);
    AOT_TEST_CHECK(!shadowing->isCacheableTransitionFrom(tagged));
    AOT_TEST_CHECK(root->tryMarkPristine(vm));
    AOT_TEST_CHECK(hasBothPristineBits(root) && hasBothPristineBits(tagged));
    AOT_TEST_CHECK(!shadowing->isPristine() && shadowing->hasPristineElements());
}

static void testMarkIsRefusedForFlattenedDictionary()
{
    Realms realms;
    VM& vm = realms.vm();
    JSObject* array = realms.object(realms.locked(), "(a => (a.tag = 1, a))(['a'].slice())"_s);
    AOT_TEST_CHECK(isPristine(vm, array));
    array->convertToDictionary(vm);
    array->flattenDictionaryObject(vm);
    AOT_TEST_CHECK(!array->structure()->isDictionary() && hasNoPristineBit(array->structure()));
    AOT_TEST_CHECK(!array->structure()->tryMarkPristine(vm));
    AOT_TEST_CHECK(hasNoPristineBit(array->structure()));
}

static void testStoreCachedBeforeMark()
{
    AOT_TEST_CHECK(isPristineAfterLateMark("function store(a) { a.tag = 1; a.other = 1; return a; }"_s));
    AOT_TEST_CHECK(!isPristineAfterLateMark("function store(a) { a.tag = 1; a.map = 1; return a; }"_s));
    AOT_TEST_CHECK(!isPristineAfterLateMark("function store(a) { a.tag = 1; a.then = 1; return a; }"_s));
}

static void testPristineArraysInArrayStorage()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristineArraysInArrayStorage);
}

static void testPristinePromises()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristinePromises);
}

static void testPristineCollectionsAndRegExps()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristineCollectionsAndRegExps);
}

static void testPristineTypedArrays()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristineTypedArrays);
}

static void testPristineIterators()
{
    Realms realms;
    checkPristineOnlyInLockedRealm(realms, pristineIterators);
}

// The compiler takes a new instance for pristine and relies on a change of the instance to clear the mark.
static void testClassesWithPristineTestHaveMarkedStructures()
{
    Realms realms;
    const BuiltinTable& table = BuiltinTable::singleton();
    BitVector markedClasses;
    forEachInstanceStructure(realms.locked(), [&](BuiltinClassID builtinClass, Structure* structure) {
        if (structure->isPristine())
            markedClasses.set(builtinClass.index());
        else
            AOT_TEST_CHECK(!table.mayHavePristineInstances(builtinClass));
    });
    for (unsigned index = 0; index < table.classCount(); ++index) {
        BuiltinClassID builtinClass(index);
        if (table.at(builtinClass).origin != BuiltinOrigin::Engine || !table.mayHavePristineInstances(builtinClass) || markedClasses.get(index))
            continue;
        // An option of the engine can leave a class out of the realm.
        if (!findInRealm(realms.locked(), BuiltinCellKey::object(table.at(builtinClass).prototype)))
            continue;
        dataLogLn("No Structure is marked for the instances that inherit from ", table.textOf(table.at(builtinClass).prototype));
        failNativeTest(__FILE__, __LINE__, "start-up marks the instance Structure of every class whose pristine test may pass");
    }
}

static void testPristineNeedsLockedPrototypes()
{
    Realms realms;
    for (ASCIILiteral source : instancesOfOtherClasses) {
        JSObject* object = realms.object(realms.locked(), source);
        AOT_TEST_CHECK_FOR_SOURCE(!isPristine(realms.vm(), object) || hasOnlyLockedPrototypes(object), source);
    }
}

static void testNonPristineValues()
{
    Realms realms;
    for (ASCIILiteral source : nonPristineValues)
        AOT_TEST_CHECK_FOR_SOURCE(!isPristine(realms.vm(), realms.evaluate(realms.locked(), source)), source);
}

static void testPristineArrayTransitions()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* locked = realms.locked();

    JSObject* frozen = realms.object(locked, "Object.freeze([1, 2])"_s);
    AOT_TEST_CHECK(isPristine(vm, frozen) == frozen->hasFrozenElementsInPlace());
    JSObject* dictionary = realms.object(locked, "[1, 2].slice()"_s);
    AOT_TEST_CHECK(isPristine(vm, dictionary));
    dictionary->convertToDictionary(vm);
    AOT_TEST_CHECK(!isPristine(vm, dictionary));

    Structure* original = locked->originalArrayStructureForIndexingType(ArrayWithInt32);
    AOT_TEST_CHECK(original->isPristine() && original->hasPristineElements());
    AOT_TEST_CHECK(!Structure::create(vm, locked, locked->arrayPrototype(), original->typeInfo(), original->classInfoForCells(), original->indexingType())->isPristine());
    JSObject* createdBeforeBadTime = realms.object(locked, "[1, 2]"_s);
    locked->haveABadTime(vm);
    AOT_TEST_CHECK(!isPristine(vm, createdBeforeBadTime));
    AOT_TEST_CHECK(!isPristine(vm, realms.evaluate(locked, "[1, 2]"_s)));
}

static void testIndexAccessorInRealmThatPrototypeInheritsFrom()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* locked = realms.locked();
    JSGlobalObject* plain = realms.plain();

    JSArray* ordinary = realms.array(locked, "[1, 2].slice()"_s);
    JSObject* inheritor = realms.object(locked, "var inheritor = { 0: 1 }; var heir = Object.create(inheritor); inheritor"_s);
    inheritor->setPrototypeDirect(vm, plain->objectPrototype());
    AOT_TEST_CHECK(!locked->isHavingABadTime() && !plain->isHavingABadTime());
    AOT_TEST_CHECK(!hasSlowPutArrayStorage(inheritor->indexingType()));

    realms.evaluate(plain, "Object.defineProperty(Object.prototype, '1', { get() { return 'inherited'; }, configurable: true })"_s);
    AOT_TEST_CHECK(plain->isHavingABadTime());
    AOT_TEST_CHECK(!locked->isHavingABadTime());
    AOT_TEST_CHECK(hasSlowPutArrayStorage(inheritor->indexingType()));
    AOT_TEST_CHECK(hasInt32(ordinary->indexingType()) && isPristine(vm, ordinary));
    AOT_TEST_CHECK(hasInt32(realms.array(locked, "[1, 2].slice()"_s)->indexingType()));
    AOT_TEST_CHECK(realms.evaluate(locked, "inheritor[1] === 'inherited' && heir[1] === 'inherited' && Object.create(heir)[1] === 'inherited' && [1][1] === undefined && ({})[1] === undefined"_s) == jsBoolean(true));
}

static void testTypedArraysInterceptIndexedStoresAfterIndexAccessorOnPrototype()
{
    Realms realms;
    JSGlobalObject* locked = realms.locked();

    JSObject* createdBefore = realms.object(locked, "var createdBefore = Object.create(new Uint8Array(4)); createdBefore[0] = 1; createdBefore"_s);
    JSArray* ordinary = realms.array(locked, "[1, 2].slice()"_s);
    AOT_TEST_CHECK(realms.evaluate(locked, "var early = Object.create(new Uint8Array(4)); early[9] = 9; early[9] === 9"_s) == jsBoolean(true));
    AOT_TEST_CHECK(!locked->typedArraysInterceptIndexedStores());
    AOT_TEST_CHECK(!hasSlowPutArrayStorage(createdBefore->indexingType()));

    realms.evaluate(locked, "var watched = { }; var heir = Object.create(watched); Object.defineProperty(watched, 3, { set(value) { }, configurable: true })"_s);
    AOT_TEST_CHECK(!locked->isHavingABadTime());
    AOT_TEST_CHECK(locked->typedArraysInterceptIndexedStores());
    AOT_TEST_CHECK(hasSlowPutArrayStorage(createdBefore->indexingType()));
    AOT_TEST_CHECK(hasInt32(ordinary->indexingType()));
    AOT_TEST_CHECK(hasInt32(realms.array(locked, "[1, 2].slice()"_s)->indexingType()));
    AOT_TEST_CHECK(realms.evaluate(locked, "createdBefore[1] = 1; createdBefore[9] = 9; createdBefore[1] === 1 && !(9 in createdBefore)"_s) == jsBoolean(true));
    AOT_TEST_CHECK(realms.evaluate(locked, "var createdAfter = Object.create(new Uint8Array(4)); createdAfter[1] = 1; createdAfter[9] = 9; createdAfter[1] === 1 && !(9 in createdAfter)"_s) == jsBoolean(true));
    AOT_TEST_CHECK(realms.evaluate(locked, "var moved = [1]; Object.setPrototypeOf(moved, new Uint8Array(4)); moved[9] = 9; !(9 in moved) && Reflect.set(moved, 9, 9) && !(9 in moved)"_s) == jsBoolean(true));
    AOT_TEST_CHECK(realms.evaluate(locked, "var middle = { }; var below = Object.create(middle); below[0] = 1; Object.setPrototypeOf(middle, new Uint8Array(4)); below[9] = 9; !(9 in below)"_s) == jsBoolean(true));
    AOT_TEST_CHECK(!realms.plain()->typedArraysInterceptIndexedStores());
    AOT_TEST_CHECK(realms.evaluate(realms.plain(), "var elsewhere = Object.create(new Uint8Array(4)); elsewhere[9] = 9; elsewhere[9] === 9"_s) == jsBoolean(true));
}

static void testPristineArrayCachedTransitions()
{
    Realms realms;
    VM& vm = realms.vm();
    for (JSGlobalObject* realm : { realms.locked(), realms.plain() }) {
        realms.evaluate(realm, "function storeTwice(store) { return [store([1, 2]), store([1, 2])]; }"_s);
        for (const CachedStoreCase& storeCase : cachedStoreCases) {
            JSArray* stored = realms.array(realm, storeCase.source);
            bool shouldBePristine = realm == realms.locked() && storeCase.staysPristine;
            for (unsigned i = 0; i < 2; ++i)
                AOT_TEST_CHECK_FOR_SOURCE(isPristine(vm, stored->getIndexQuickly(i)) == shouldBePristine, storeCase.source);
        }
    }
}

static void testStartUpLocksObjects(LoadedProgram& program)
{
    checkAlwaysLockedObjects(program.realms());
    checkAlwaysLockedMembers(program.realms());
}

static void testStartUpCreatesNoClass(LoadedProgram& program)
{
    JSGlobalObject* realm = program.realm();
    AOT_TEST_CHECK(!realm->dateStructureConcurrently() && !realm->weakMapStructureConcurrently() && !realm->weakSetStructureConcurrently());
    AOT_TEST_CHECK(!realm->arrayBufferStructureConcurrently(ArrayBufferSharingMode::Default));
    // Where the option is on, as in Bun and here, JSGlobalObject::init() puts SharedArrayBuffer on the global object of every realm.
    AOT_TEST_CHECK(!!realm->arrayBufferStructureConcurrently(ArrayBufferSharingMode::Shared) == Options::useSharedArrayBuffer());
    AOT_TEST_CHECK(!realm->typedArrayStructureConcurrently(TypeUint8, false) && !realm->typedArrayStructureConcurrently(TypeFloat64, false) && !realm->typedArrayStructureConcurrently(TypeDataView, false));
    AOT_TEST_CHECK(!findExistingObject(realm, Identifier::fromString(program.realms().vm(), "Math"_s).impl()));
}

// Native code makes the first instance of many a class, and no name of the global object has been read by then.
static void testClassCreatedForAnInstanceIsLocked(LoadedProgram& program)
{
    Realms& realms = program.realms();
    VM& vm = realms.vm();
    JSGlobalObject* realm = program.realm();

    Structure* structure = realm->typedArrayStructure(TypeUint8, false);
    AOT_TEST_CHECK(structure->isPristine());
    JSObject* prototype = structure->storedPrototypeObject();
    AOT_TEST_CHECK(prototype->structure()->hasImmutablePrototype() && prototype->structure()->isImmutableProperty(vm.propertyNames->constructor.impl()));
    JSObject* constructor = findExistingObject(prototype, vm.propertyNames->constructor.impl());
    AOT_TEST_CHECK(constructor && constructor->structure()->isImmutableProperty(vm.propertyNames->name.impl()) && constructor->structure()->hasImmutablePrototype());

    JSObject* sharedPrototype = prototype->getPrototypeDirect().getObject();
    AOT_TEST_CHECK(sharedPrototype->structure()->isImmutableProperty(vm.propertyNames->set.impl()) && sharedPrototype->structure()->hasImmutablePrototype());
    JSObject* sharedConstructor = constructor->getPrototypeDirect().getObject();
    AOT_TEST_CHECK(sharedConstructor->structure()->isImmutableProperty(vm.propertyNames->from.impl()));
    AOT_TEST_CHECK(findExistingObject(sharedPrototype, vm.propertyNames->constructor.impl()) == sharedConstructor);

    AOT_TEST_CHECK(!findExistingObject(realm, Identifier::fromString(vm, "Uint8Array"_s).impl()));
    AOT_TEST_CHECK(realms.object(realm, "Uint8Array"_s) == constructor);
    AOT_TEST_CHECK(realms.throwsTypeError(realm, "(function () { 'use strict'; Object.getPrototypeOf(Uint8Array.prototype).set = 1; })()"_s));
    AOT_TEST_CHECK(!realm->typedArrayStructureConcurrently(TypeInt16, false));
}

static void testObjectCreatedAtFirstReadIsLocked(LoadedProgram& program)
{
    Realms& realms = program.realms();
    VM& vm = realms.vm();
    JSGlobalObject* realm = program.realm();
    for (ASCIILiteral source : { "Math"_s, "JSON"_s, "Reflect"_s, "Atomics"_s }) {
        AOT_TEST_CHECK_FOR_SOURCE(!findExistingObject(realm, Identifier::fromString(vm, source).impl()), source);
        AOT_TEST_CHECK_FOR_SOURCE(realms.object(realm, source)->structure()->mayHaveImmutableProperties(), source);
    }
    AOT_TEST_CHECK(realms.throwsTypeError(realm, "(function () { 'use strict'; Math.floor = 1; })()"_s));
    AOT_TEST_CHECK(realms.throwsTypeError(realm, "(function () { 'use strict'; Number.prototype.toFixed = 1; })()"_s));
    AOT_TEST_CHECK(realms.throwsTypeError(realm, "(function () { 'use strict'; Object.getPrototypeOf(true).valueOf = 1; })()"_s));
}

// The program may put something else under the name of a class that nobody has used yet. The class that the realm then creates is the one that is locked and found.
static void testClassBehindReplacedNameIsLocked(LoadedProgram& program)
{
    Realms& realms = program.realms();
    VM& vm = realms.vm();
    JSGlobalObject* realm = program.realm();
    AOT_TEST_CHECK(realms.evaluate(realm, "(WeakMap = 1, WeakMap === 1)"_s) == jsBoolean(true));

    auto found = findInRealm(realm, BuiltinCellKey::object(Builtins::WeakMap));
    AOT_TEST_CHECK(found && realm->weakMapStructureConcurrently());
    JSObject* prototype = realm->weakMapStructureConcurrently()->storedPrototypeObject();
    AOT_TEST_CHECK(*found == findExistingObject(prototype, vm.propertyNames->constructor.impl()));
    AOT_TEST_CHECK(prototype->structure()->isImmutableProperty(vm.propertyNames->get.impl()) && (*found)->structure()->isImmutableProperty(vm.propertyNames->name.impl()));
    AOT_TEST_CHECK(realm->weakMapStructureConcurrently()->isPristine());
    AOT_TEST_CHECK(realms.evaluate(realm, "WeakMap === 1"_s) == jsBoolean(true));
}

static void testLanguageBehaviorWithProgram(LoadedProgram& program)
{
    checkLanguageBehavior(program.realms());
}

static void testDeclarationsOfScriptsWithProgram(LoadedProgram& program)
{
    checkDeclarationsOfScripts(program.realms());
}

static void testStartUpMarksStructuresPristine(LoadedProgram& program)
{
    checkPristineOnlyInLockedRealm(program.realms(), pristineArrays);
    checkPristineOnlyInLockedRealm(program.realms(), pristinePromises);
    checkPristineOnlyInLockedRealm(program.realms(), pristineCollectionsAndRegExps);
    checkPristineOnlyInLockedRealm(program.realms(), pristineTypedArrays);
    checkPristineOnlyInLockedRealm(program.realms(), pristineIterators);
}

static void testStartUpMarksOriginalArrayStructures(LoadedProgram& program)
{
    checkOriginalArrayStructures(program.realms());
}

static void testStartUpMarksTransitionsOfArrayStructures(LoadedProgram& program)
{
    checkArraysWithMatchArrayNames(program.realms());
    checkPristineOnlyInLockedRealm(program.realms(), pristineContiguousArrays);
    checkPristineOnlyInLockedRealm(program.realms(), pristineRegExpMatchArrays);
}

static void testConstantMembersAreImmutable(LoadedProgram& program)
{
    Realms& realms = program.realms();
    VM& vm = realms.vm();
    JSGlobalObject* realm = program.realm();
    AOT_TEST_CHECK(isListedAsConstant(program, Builtins::Date) && isListedAsConstant(program, Builtins::DateNow));
    AOT_TEST_CHECK(!isListedAsConstant(program, Builtins::Intl) && !isListedAsConstant(program, Builtins::DateParse));

    Structure* date = realms.object(realm, "Date"_s)->structure();
    AOT_TEST_CHECK(!date->hasImmutableProperties());
    AOT_TEST_CHECK(date->isImmutableProperty(Identifier::fromString(vm, "now"_s).impl()));
    AOT_TEST_CHECK(!date->isImmutableProperty(Identifier::fromString(vm, "parse"_s).impl()));
    AOT_TEST_CHECK(realm->structure()->isImmutableProperty(Identifier::fromString(vm, "Date"_s).impl()));
    AOT_TEST_CHECK(!realm->structure()->isImmutableProperty(Identifier::fromString(vm, "Intl"_s).impl()));

    for (ASCIILiteral source : { "(function () { 'use strict'; Date.now = function () { }; })()"_s, "Object.defineProperty(Date, 'now', { value: 1 })"_s, "(function () { 'use strict'; delete Date.now; })()"_s, "(function () { 'use strict'; Date = 1; })()"_s, "Date.now = function () { }"_s, "Date = 1"_s }) {
        AOT_TEST_CHECK_FOR_SOURCE(realms.throwsTypeError(realm, source), source);
        AOT_TEST_CHECK_FOR_SOURCE(!realms.throwsTypeError(realms.plain(), source), source);
    }
    for (ASCIILiteral source : { "Object.getOwnPropertyDescriptor(Date, 'now').writable"_s, "(f => (Date.parse = f, Date.parse === f))(function () { })"_s, "(Date.nameNobodyHas = 1, Date.nameNobodyHas === 1)"_s, "(Intl = 1, Intl === 1)"_s })
        AOT_TEST_CHECK_FOR_SOURCE(realms.evaluate(realm, source) == jsBoolean(true), source);

    AOT_TEST_CHECK(realms.throwsSyntaxError(realm, "let Date = 1"_s) && !realms.throwsSyntaxError(realms.plain(), "let Date = 1"_s));
    AOT_TEST_CHECK(realms.evaluate(realm, "let Intl = 2; Intl === 2 && typeof Date.now() === 'number'"_s) == jsBoolean(true));
}

template<LoadedProgramTest body>
static constexpr void (*withoutBuiltins)() = runNativeTestWithLoadedProgram<lockedRealmTests, programWithoutBuiltins, body>;

template<LoadedProgramTest body>
static constexpr void (*withDateNow)() = runNativeTestWithLoadedProgram<lockedRealmTests, programThatReadsDateNow, body>;

std::span<const NativeTest> lockedRealmTests()
{
    static constexpr NativeTest tests[] = {
        { "locked-realm/always-locked-objects-are-immutable"_s, testAlwaysLockedObjectsAreImmutable },
        { "locked-realm/always-locked-members-are-immutable"_s, testAlwaysLockedMembersAreImmutable },
        { "locked-realm/language-behavior"_s, testLanguageBehavior },
        { "locked-realm/declarations-of-scripts"_s, testDeclarationsOfScripts },
        { "locked-realm/vm-state-without-image"_s, testVMStateWithoutImage },
        { "locked-realm/validators-pass-fresh-realm"_s, testValidatorsPassFreshRealm },
        { "locked-realm/pristine-arrays"_s, testPristineArrays },
        { "locked-realm/original-array-structures-are-pristine"_s, testOriginalArrayStructuresArePristine },
        { "locked-realm/pristine-contiguous-arrays"_s, testPristineContiguousArrays },
        { "locked-realm/pristine-regexp-match-arrays"_s, testPristineRegExpMatchArrays },
        { "locked-realm/names-of-match-arrays-keep-arrays-pristine"_s, testNamesOfMatchArraysKeepArraysPristine },
        { "locked-realm/numeric-names-end-pristine-elements"_s, testNumericNamesEndPristineElements },
        { "locked-realm/mark-reaches-existing-transitions"_s, testMarkReachesExistingTransitions },
        { "locked-realm/mark-leaves-out-bits-lost-by-existing-transitions"_s, testMarkLeavesOutBitsLostByExistingTransitions },
        { "locked-realm/mark-follows-structures-marked-earlier"_s, testMarkFollowsStructuresMarkedEarlier },
        { "locked-realm/mark-is-refused-for-flattened-dictionary"_s, testMarkIsRefusedForFlattenedDictionary },
        { "locked-realm/store-cached-before-mark"_s, testStoreCachedBeforeMark },
        { "locked-realm/array-storage-keeps-arrays-pristine"_s, testPristineArraysInArrayStorage },
        { "locked-realm/pristine-promises"_s, testPristinePromises },
        { "locked-realm/pristine-collections-and-regexps"_s, testPristineCollectionsAndRegExps },
        { "locked-realm/pristine-typed-arrays"_s, testPristineTypedArrays },
        { "locked-realm/pristine-iterators"_s, testPristineIterators },
        { "locked-realm/classes-with-pristine-test-have-marked-structures"_s, testClassesWithPristineTestHaveMarkedStructures },
        { "locked-realm/pristine-needs-locked-prototypes"_s, testPristineNeedsLockedPrototypes },
        { "locked-realm/non-pristine-values"_s, testNonPristineValues },
        { "locked-realm/transitions-of-pristine-arrays"_s, testPristineArrayTransitions },
        { "locked-realm/index-accessor-in-a-realm-that-a-prototype-inherits-from"_s, testIndexAccessorInRealmThatPrototypeInheritsFrom },
        { "locked-realm/typed-arrays-intercept-indexed-stores-after-an-index-accessor-on-a-prototype"_s, testTypedArraysInterceptIndexedStoresAfterIndexAccessorOnPrototype },
        { "locked-realm/cached-transitions-of-pristine-arrays"_s, testPristineArrayCachedTransitions },
        { "locked-realm/start-up-locks-objects"_s, withoutBuiltins<testStartUpLocksObjects> },
        { "locked-realm/start-up-creates-no-class"_s, withoutBuiltins<testStartUpCreatesNoClass> },
        { "locked-realm/class-created-for-an-instance-is-locked"_s, withoutBuiltins<testClassCreatedForAnInstanceIsLocked> },
        { "locked-realm/object-created-at-first-read-is-locked"_s, withoutBuiltins<testObjectCreatedAtFirstReadIsLocked> },
        { "locked-realm/class-behind-replaced-name-is-locked"_s, withoutBuiltins<testClassBehindReplacedNameIsLocked> },
        { "locked-realm/program-sees-language-behavior"_s, withoutBuiltins<testLanguageBehaviorWithProgram> },
        { "locked-realm/program-sees-declarations-of-scripts"_s, withoutBuiltins<testDeclarationsOfScriptsWithProgram> },
        { "locked-realm/start-up-marks-structures-pristine"_s, withoutBuiltins<testStartUpMarksStructuresPristine> },
        { "locked-realm/start-up-marks-original-array-structures"_s, withoutBuiltins<testStartUpMarksOriginalArrayStructures> },
        { "locked-realm/start-up-marks-transitions-of-array-structures"_s, withoutBuiltins<testStartUpMarksTransitionsOfArrayStructures> },
        { "locked-realm/constant-members-are-immutable"_s, withDateNow<testConstantMembersAreImmutable> },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
