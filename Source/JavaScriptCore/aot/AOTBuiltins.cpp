/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltins.h"

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "ImmutableIntrinsics.h"
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>

namespace JSC { namespace AOT {

namespace {

using Condition = BuiltinSignature::Condition;

struct Row {
    ASCIILiteral path;
    Type result;
    Condition condition { Condition::Always };
};

constexpr Type TStringOrUndefined = TString | TUndefined;
constexpr Type TObjectOrNull = TAnyObject | TNull;

const Row rows[] = {
    { "parseInt"_s, TNumber, Condition::IsInt32IfOnlyArgumentIs }, { "parseFloat"_s, TNumber }, { "isNaN"_s, TBoolean }, { "isFinite"_s, TBoolean },
    { "encodeURIComponent"_s, TString }, { "encodeURI"_s, TString }, { "decodeURIComponent"_s, TString }, { "decodeURI"_s, TString },
    { "escape"_s, TString }, { "unescape"_s, TString },
    { "String"_s, TString }, { "Number"_s, TNumber, Condition::IsInt32IfOnlyArgumentIs }, { "Boolean"_s, TBoolean }, { "BigInt"_s, TBigInt }, { "Symbol"_s, TSymbol },
    { "Array"_s, TArray }, { "Object"_s, TAnyObject }, { "Date"_s, TString },

    { "Object.keys"_s, TArray }, { "Object.values"_s, TArray }, { "Object.entries"_s, TArray },
    { "Object.getOwnPropertyNames"_s, TArray }, { "Object.getOwnPropertySymbols"_s, TArray },
    { "Object.assign"_s, TAnyObject }, { "Object.create"_s, TFinalObject }, { "Object.fromEntries"_s, TFinalObject }, { "Object.groupBy"_s, TFinalObject },
    { "Object.is"_s, TBoolean }, { "Object.isFrozen"_s, TBoolean }, { "Object.isSealed"_s, TBoolean }, { "Object.isExtensible"_s, TBoolean },
    { "Object.hasOwn"_s, TBoolean }, { "Object.getPrototypeOf"_s, TObjectOrNull },
    { "Object.getOwnPropertyDescriptor"_s, TFinalObject | TUndefined }, { "Object.getOwnPropertyDescriptors"_s, TFinalObject },
    { "Object.defineProperty"_s, TAnyObject }, { "Object.defineProperties"_s, TAnyObject },

    { "Array.isArray"_s, TBoolean },
    { "Array.from"_s, TArray, Condition::IfThisIsHolder }, { "Array.of"_s, TArray, Condition::IfThisIsHolder },

    { "Math.abs"_s, TNumber }, { "Math.acos"_s, TNumber }, { "Math.acosh"_s, TNumber }, { "Math.asin"_s, TNumber }, { "Math.asinh"_s, TNumber },
    { "Math.atan"_s, TNumber }, { "Math.atan2"_s, TNumber }, { "Math.atanh"_s, TNumber }, { "Math.cbrt"_s, TNumber }, { "Math.ceil"_s, TNumber, Condition::IsInt32IfArgumentsAre },
    { "Math.clz32"_s, TInt32 }, { "Math.cos"_s, TNumber }, { "Math.cosh"_s, TNumber }, { "Math.exp"_s, TNumber }, { "Math.expm1"_s, TNumber },
    { "Math.floor"_s, TNumber, Condition::IsInt32IfArgumentsAre }, { "Math.fround"_s, TNumber }, { "Math.hypot"_s, TNumber }, { "Math.imul"_s, TInt32 }, { "Math.log"_s, TNumber },
    { "Math.log10"_s, TNumber }, { "Math.log1p"_s, TNumber }, { "Math.log2"_s, TNumber }, { "Math.max"_s, TNumber, Condition::IsInt32IfArgumentsAre }, { "Math.min"_s, TNumber, Condition::IsInt32IfArgumentsAre },
    { "Math.pow"_s, TNumber }, { "Math.random"_s, TNumber }, { "Math.round"_s, TNumber, Condition::IsInt32IfArgumentsAre }, { "Math.sign"_s, TNumber }, { "Math.sin"_s, TNumber },
    { "Math.sinh"_s, TNumber }, { "Math.sqrt"_s, TNumber }, { "Math.tan"_s, TNumber }, { "Math.tanh"_s, TNumber }, { "Math.trunc"_s, TNumber, Condition::IsInt32IfArgumentsAre },

    { "Number.isInteger"_s, TBoolean }, { "Number.isFinite"_s, TBoolean }, { "Number.isNaN"_s, TBoolean }, { "Number.isSafeInteger"_s, TBoolean },
    { "Number.parseFloat"_s, TNumber }, { "Number.parseInt"_s, TNumber, Condition::IsInt32IfOnlyArgumentIs },

    { "JSON.stringify"_s, TStringOrUndefined },
    { "Date.now"_s, TNumber }, { "Date.parse"_s, TNumber }, { "Date.UTC"_s, TNumber },
    { "String.fromCharCode"_s, TString }, { "String.fromCodePoint"_s, TString }, { "String.raw"_s, TString },
    { "Symbol.for"_s, TSymbol }, { "Symbol.keyFor"_s, TStringOrUndefined },
    { "Reflect.has"_s, TBoolean }, { "Reflect.set"_s, TBoolean }, { "Reflect.defineProperty"_s, TBoolean }, { "Reflect.deleteProperty"_s, TBoolean },
    { "Reflect.isExtensible"_s, TBoolean }, { "Reflect.preventExtensions"_s, TBoolean }, { "Reflect.setPrototypeOf"_s, TBoolean },
    { "Reflect.ownKeys"_s, TArray }, { "Reflect.getPrototypeOf"_s, TObjectOrNull }, { "Reflect.getOwnPropertyDescriptor"_s, TFinalObject | TUndefined },
    { "ArrayBuffer.isView"_s, TBoolean },

    { "String.prototype.at"_s, TStringOrUndefined }, { "String.prototype.charAt"_s, TString }, { "String.prototype.charCodeAt"_s, TNumber },
    { "String.prototype.codePointAt"_s, TInt32 | TUndefined }, { "String.prototype.concat"_s, TString }, { "String.prototype.endsWith"_s, TBoolean },
    { "String.prototype.includes"_s, TBoolean }, { "String.prototype.indexOf"_s, TInt32 }, { "String.prototype.isWellFormed"_s, TBoolean },
    { "String.prototype.lastIndexOf"_s, TInt32 }, { "String.prototype.localeCompare"_s, TInt32 }, { "String.prototype.normalize"_s, TString },
    { "String.prototype.padEnd"_s, TString }, { "String.prototype.padStart"_s, TString }, { "String.prototype.repeat"_s, TString },
    { "String.prototype.slice"_s, TString }, { "String.prototype.startsWith"_s, TBoolean }, { "String.prototype.substr"_s, TString },
    { "String.prototype.substring"_s, TString }, { "String.prototype.toLocaleLowerCase"_s, TString }, { "String.prototype.toLocaleUpperCase"_s, TString },
    { "String.prototype.toLowerCase"_s, TString }, { "String.prototype.toString"_s, TString }, { "String.prototype.toUpperCase"_s, TString },
    { "String.prototype.toWellFormed"_s, TString }, { "String.prototype.trim"_s, TString }, { "String.prototype.trimEnd"_s, TString },
    { "String.prototype.trimStart"_s, TString }, { "String.prototype.trimLeft"_s, TString }, { "String.prototype.trimRight"_s, TString },
    { "String.prototype.valueOf"_s, TString },
    { "String.prototype.replace"_s, TString, Condition::IfFirstArgumentIsNotObject }, { "String.prototype.replaceAll"_s, TString, Condition::IfFirstArgumentIsNotObject },
    { "String.prototype.split"_s, TArray, Condition::IfFirstArgumentIsNotObject }, { "String.prototype.search"_s, TNumber, Condition::IfFirstArgumentIsNotObject },

    { "Number.prototype.toFixed"_s, TString }, { "Number.prototype.toString"_s, TString }, { "Number.prototype.toPrecision"_s, TString },
    { "Number.prototype.toExponential"_s, TString }, { "Number.prototype.toLocaleString"_s, TString }, { "Number.prototype.valueOf"_s, TNumber },
    { "Boolean.prototype.toString"_s, TString }, { "Boolean.prototype.valueOf"_s, TBoolean },
    { "Symbol.prototype.toString"_s, TString }, { "Symbol.prototype.valueOf"_s, TSymbol },
    { "BigInt.prototype.toString"_s, TString }, { "BigInt.prototype.toLocaleString"_s, TString }, { "BigInt.prototype.valueOf"_s, TBigInt },

    { "Map.prototype.has"_s, TBoolean }, { "Map.prototype.delete"_s, TBoolean }, { "Map.prototype.set"_s, TMap }, { "Map.prototype.clear"_s, TUndefined },
    { "Set.prototype.has"_s, TBoolean }, { "Set.prototype.delete"_s, TBoolean }, { "Set.prototype.add"_s, TSet }, { "Set.prototype.clear"_s, TUndefined },
    { "WeakMap.prototype.has"_s, TBoolean }, { "WeakMap.prototype.delete"_s, TBoolean }, { "WeakMap.prototype.set"_s, TWeakMap },
    { "WeakSet.prototype.has"_s, TBoolean }, { "WeakSet.prototype.delete"_s, TBoolean }, { "WeakSet.prototype.add"_s, TWeakSet },
    { "Array.prototype.includes"_s, TBoolean }, { "Array.prototype.every"_s, TBoolean }, { "Array.prototype.some"_s, TBoolean },
    { "Array.prototype.indexOf"_s, TNumber }, { "Array.prototype.lastIndexOf"_s, TNumber }, { "Array.prototype.findIndex"_s, TNumber }, { "Array.prototype.findLastIndex"_s, TNumber },
    { "Array.prototype.push"_s, TNumber }, { "Array.prototype.unshift"_s, TNumber }, { "Array.prototype.join"_s, TString },
    { "RegExp.prototype.test"_s, TBoolean }, { "RegExp.prototype.exec"_s, TArray | TNull },
    { "Date.prototype.getTime"_s, TNumber }, { "Date.prototype.valueOf"_s, TNumber }, { "Date.prototype.getTimezoneOffset"_s, TNumber },
    { "Date.prototype.getFullYear"_s, TNumber }, { "Date.prototype.getMonth"_s, TNumber }, { "Date.prototype.getDate"_s, TNumber }, { "Date.prototype.getDay"_s, TNumber },
    { "Date.prototype.getHours"_s, TNumber }, { "Date.prototype.getMinutes"_s, TNumber }, { "Date.prototype.getSeconds"_s, TNumber }, { "Date.prototype.getMilliseconds"_s, TNumber },
    { "Date.prototype.getUTCFullYear"_s, TNumber }, { "Date.prototype.getUTCMonth"_s, TNumber }, { "Date.prototype.getUTCDate"_s, TNumber }, { "Date.prototype.getUTCDay"_s, TNumber },
    { "Date.prototype.getUTCHours"_s, TNumber }, { "Date.prototype.getUTCMinutes"_s, TNumber }, { "Date.prototype.getUTCSeconds"_s, TNumber }, { "Date.prototype.getUTCMilliseconds"_s, TNumber },
    { "Date.prototype.toISOString"_s, TString }, { "Date.prototype.toString"_s, TString },
    { "Object.prototype.hasOwnProperty"_s, TBoolean }, { "Object.prototype.isPrototypeOf"_s, TBoolean }, { "Object.prototype.propertyIsEnumerable"_s, TBoolean },
};

const Row constructors[] = {
    { "Object"_s, TAnyObject }, { "Array"_s, TArray }, { "Function"_s, TFunction },
    { "Map"_s, TMap }, { "Set"_s, TSet }, { "WeakMap"_s, TWeakMap }, { "WeakSet"_s, TWeakSet }, { "WeakRef"_s, TObject },
    { "RegExp"_s, TRegExp }, { "Promise"_s, TPromise }, { "Date"_s, TDate },
    { "Error"_s, TError }, { "TypeError"_s, TError }, { "RangeError"_s, TError }, { "SyntaxError"_s, TError }, { "ReferenceError"_s, TError },
    { "EvalError"_s, TError }, { "URIError"_s, TError }, { "AggregateError"_s, TError },
    { "ArrayBuffer"_s, TArrayBuffer }, { "SharedArrayBuffer"_s, TArrayBuffer }, { "DataView"_s, TDataView },
    { "String"_s, TStringObject }, { "Number"_s, TObject }, { "Boolean"_s, TObject },
};

struct Tables {
    Vector<const Row*> called;
    Vector<const Row*> constructed;
    unsigned stringPrototype { 0 };
    unsigned numberPrototype { 0 };
    unsigned booleanPrototype { 0 };
    unsigned symbolPrototype { 0 };
    unsigned bigIntPrototype { 0 };
    unsigned functionPrototype { 0 };
    unsigned receiverPrototypes[16] { };
    Vector<Builtin> builtins;
};

const Tables* tables()
{
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    if (!intrinsics)
        return nullptr;
    static LazyNeverDestroyed<Tables> result;
    static std::once_flag once;
    std::call_once(once, [&] {
        result.construct();
        result->called.fill(nullptr, intrinsics->count());
        result->constructed.fill(nullptr, intrinsics->count());
        auto find = [&](ASCIILiteral path) -> unsigned {
            unsigned number = ImmutableIntrinsics::globalObject;
            for (StringView part : StringView(path).split('.')) {
                number = intrinsics->find(number, *part.toString().impl());
                if (!number || !intrinsics->at(number).isCell)
                    return 0;
                number = intrinsics->at(number).canonical;
            }
            return number;
        };
        for (auto& row : rows) {
            if (unsigned number = find(row.path); number && !result->called[number])
                result->called[number] = &row;
        }
        for (auto& row : constructors) {
            if (unsigned number = find(row.path))
                result->constructed[number] = &row;
        }
        result->stringPrototype = find("String.prototype"_s);
        result->numberPrototype = find("Number.prototype"_s);
        result->booleanPrototype = find("Boolean.prototype"_s);
        result->symbolPrototype = find("Symbol.prototype"_s);
        result->bigIntPrototype = find("BigInt.prototype"_s);
        result->functionPrototype = find("Function.prototype"_s);
        auto prototypeOf = [&](Receiver receiver) -> unsigned& { return result->receiverPrototypes[static_cast<unsigned>(receiver)]; };
        prototypeOf(Receiver::String) = result->stringPrototype;
        prototypeOf(Receiver::Number) = result->numberPrototype;
        prototypeOf(Receiver::Array) = find("Array.prototype"_s);
        prototypeOf(Receiver::Map) = find("Map.prototype"_s);
        prototypeOf(Receiver::Set) = find("Set.prototype"_s);
        prototypeOf(Receiver::WeakMap) = find("WeakMap.prototype"_s);
        prototypeOf(Receiver::WeakSet) = find("WeakSet.prototype"_s);
        prototypeOf(Receiver::RegExp) = find("RegExp.prototype"_s);
        prototypeOf(Receiver::Date) = find("Date.prototype"_s);
        result->builtins.fill(Builtin::None, intrinsics->count());
#define AOT_FIND_BUILTIN(name, path) \
        if (unsigned number = find(path ## _s); number && result->builtins[number] == Builtin::None) \
            result->builtins[number] = Builtin::name;
        FOR_EACH_AOT_BUILTIN(AOT_FIND_BUILTIN)
#undef AOT_FIND_BUILTIN
    });
    return &result.get();
}

} // anonymous namespace

std::optional<BuiltinSignature> intrinsicSignature(unsigned number)
{
    const Tables* all = tables();
    if (!all || number >= all->called.size() || !all->called[number])
        return std::nullopt;
    return BuiltinSignature { all->called[number]->result, all->called[number]->condition };
}

std::optional<Type> constructingIntrinsicResult(unsigned number)
{
    const Tables* all = tables();
    if (!all || number >= all->constructed.size() || !all->constructed[number])
        return std::nullopt;
    return all->constructed[number]->result;
}

unsigned intrinsicFoundOnPrimitive(Type receiver, const StringImpl& name)
{
    const Tables* all = tables();
    if (!all || !receiver)
        return 0;
    unsigned prototype = 0;
    if (isSubtype(receiver, TString))
        prototype = all->stringPrototype;
    else if (isSubtype(receiver, TNumber))
        prototype = all->numberPrototype;
    else if (isSubtype(receiver, TBoolean))
        prototype = all->booleanPrototype;
    else if (isSubtype(receiver, TSymbol))
        prototype = all->symbolPrototype;
    else if (isSubtype(receiver, TBigInt))
        prototype = all->bigIntPrototype;
    if (!prototype)
        return 0;
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    unsigned number = intrinsics->find(prototype, name);
    if (!number || !intrinsics->at(number).isCell)
        return 0;
    return intrinsics->at(number).canonical;
}

bool isDataPropertyOfFunctionPrototype(const StringImpl& name)
{
    const Tables* all = tables();
    return all && all->functionPrototype && ImmutableIntrinsics::shared()->find(all->functionPrototype, name);
}

Builtin builtinAtIndex(unsigned number)
{
    const Tables* all = tables();
    if (!all || number >= all->builtins.size())
        return Builtin::None;
    return all->builtins[number];
}

Type typeOf(Receiver receiver)
{
    switch (receiver) {
    case Receiver::None:
        return TNone;
    case Receiver::String:
        return TString;
    case Receiver::Array:
        return TArray;
    case Receiver::Map:
        return TMap;
    case Receiver::Set:
        return TSet;
    case Receiver::WeakMap:
        return TWeakMap;
    case Receiver::WeakSet:
        return TWeakSet;
    case Receiver::RegExp:
        return TRegExp;
    case Receiver::Date:
        return TDate;
    case Receiver::Number:
        return TNumber;
    }
    return TNone;
}

ASCIILiteral nameOf(Receiver receiver)
{
    switch (receiver) {
    case Receiver::None:
        break;
    case Receiver::String:
        return "String"_s;
    case Receiver::Array:
        return "Array"_s;
    case Receiver::Map:
        return "Map"_s;
    case Receiver::Set:
        return "Set"_s;
    case Receiver::WeakMap:
        return "WeakMap"_s;
    case Receiver::WeakSet:
        return "WeakSet"_s;
    case Receiver::RegExp:
        return "RegExp"_s;
    case Receiver::Date:
        return "Date"_s;
    case Receiver::Number:
        return "Number"_s;
    }
    return ""_s;
}

JSType cellTypeOf(Receiver receiver)
{
    switch (receiver) {
    case Receiver::String:
        return StringType;
    case Receiver::Array:
        return ArrayType;
    case Receiver::Map:
        return JSMapType;
    case Receiver::Set:
        return JSSetType;
    case Receiver::WeakMap:
        return JSWeakMapType;
    case Receiver::WeakSet:
        return JSWeakSetType;
    case Receiver::RegExp:
        return RegExpObjectType;
    case Receiver::Date:
        return JSDateType;
    case Receiver::None:
    case Receiver::Number:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return CellType;
}

unsigned intrinsicFoundOn(Receiver receiver, const StringImpl& name)
{
    const Tables* all = tables();
    if (!all)
        return 0;
    unsigned prototype = all->receiverPrototypes[static_cast<unsigned>(receiver)];
    if (!prototype)
        return 0;
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    unsigned number = intrinsics->find(prototype, name);
    if (!number || !intrinsics->at(number).isCell)
        return 0;
    return intrinsics->at(number).canonical;
}

static constexpr Receiver allReceivers[] = { Receiver::String, Receiver::Array, Receiver::Map, Receiver::Set, Receiver::WeakMap, Receiver::WeakSet, Receiver::RegExp, Receiver::Date, Receiver::Number };

Receiver requiredReceiver(unsigned intrinsic)
{
    const Tables* all = tables();
    if (!all)
        return Receiver::None;
    unsigned holder = ImmutableIntrinsics::shared()->at(intrinsic).holder;
    for (Receiver receiver : allReceivers) {
        if (all->receiverPrototypes[static_cast<unsigned>(receiver)] == holder)
            return receiver;
    }
    return Receiver::None;
}

Receiver receiverWithType(Type type)
{
    if (!type)
        return Receiver::None;
    for (Receiver receiver : allReceivers) {
        if (isSubtype(type, typeOf(receiver)))
            return receiver;
    }
    return Receiver::None;
}

Receiver likelyReceiverWith(Type type, const StringImpl& name)
{
    Receiver found = Receiver::None;
    for (Receiver receiver : allReceivers) {
        if (receiver == Receiver::Number || !mayBe(type, typeOf(receiver)))
            continue;
        if (builtinAtIndex(intrinsicFoundOn(receiver, name)) == Builtin::None)
            continue;
        if (found != Receiver::None)
            return Receiver::None;
        found = receiver;
    }
    return found;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
