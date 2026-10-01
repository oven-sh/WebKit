/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltins.h"

#if ENABLE(FTL_JIT)

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

// What the specification says comes back, whatever is passed, if anything comes back at all. Where that goes by something that a
// program can put its own code behind, there is a condition, or no row.
constexpr Type TStringOrUndefined = TString | TUndefined;
constexpr Type TNumberOrUndefined = TNumber | TUndefined;
constexpr Type TObjectOrNull = TAnyObject | TNull;

const Row rows[] = {
    // Functions of the global object.
    { "parseInt"_s, TNumber }, { "parseFloat"_s, TNumber }, { "isNaN"_s, TBoolean }, { "isFinite"_s, TBoolean },
    { "encodeURIComponent"_s, TString }, { "encodeURI"_s, TString }, { "decodeURIComponent"_s, TString }, { "decodeURI"_s, TString },
    { "escape"_s, TString }, { "unescape"_s, TString },
    // Constructors, called.
    { "String"_s, TString }, { "Number"_s, TNumber }, { "Boolean"_s, TBoolean }, { "BigInt"_s, TBigInt }, { "Symbol"_s, TSymbol },
    { "Array"_s, TArray }, { "Object"_s, TAnyObject }, { "Date"_s, TString },

    { "Object.keys"_s, TArray }, { "Object.values"_s, TArray }, { "Object.entries"_s, TArray },
    { "Object.getOwnPropertyNames"_s, TArray }, { "Object.getOwnPropertySymbols"_s, TArray },
    { "Object.assign"_s, TAnyObject }, { "Object.create"_s, TFinalObject }, { "Object.fromEntries"_s, TFinalObject }, { "Object.groupBy"_s, TFinalObject },
    { "Object.is"_s, TBoolean }, { "Object.isFrozen"_s, TBoolean }, { "Object.isSealed"_s, TBoolean }, { "Object.isExtensible"_s, TBoolean },
    { "Object.hasOwn"_s, TBoolean }, { "Object.getPrototypeOf"_s, TObjectOrNull },
    { "Object.getOwnPropertyDescriptor"_s, TFinalObject | TUndefined }, { "Object.getOwnPropertyDescriptors"_s, TFinalObject },
    { "Object.defineProperty"_s, TAnyObject }, { "Object.defineProperties"_s, TAnyObject },

    { "Array.isArray"_s, TBoolean },
    // (What they make goes by what they are called on.)
    { "Array.from"_s, TArray, Condition::IfThisIsHolder }, { "Array.of"_s, TArray, Condition::IfThisIsHolder },

    { "Math.abs"_s, TNumber }, { "Math.acos"_s, TNumber }, { "Math.acosh"_s, TNumber }, { "Math.asin"_s, TNumber }, { "Math.asinh"_s, TNumber },
    { "Math.atan"_s, TNumber }, { "Math.atan2"_s, TNumber }, { "Math.atanh"_s, TNumber }, { "Math.cbrt"_s, TNumber }, { "Math.ceil"_s, TNumber },
    { "Math.clz32"_s, TInt32 }, { "Math.cos"_s, TNumber }, { "Math.cosh"_s, TNumber }, { "Math.exp"_s, TNumber }, { "Math.expm1"_s, TNumber },
    { "Math.floor"_s, TNumber }, { "Math.fround"_s, TNumber }, { "Math.hypot"_s, TNumber }, { "Math.imul"_s, TInt32 }, { "Math.log"_s, TNumber },
    { "Math.log10"_s, TNumber }, { "Math.log1p"_s, TNumber }, { "Math.log2"_s, TNumber }, { "Math.max"_s, TNumber }, { "Math.min"_s, TNumber },
    { "Math.pow"_s, TNumber }, { "Math.random"_s, TNumber }, { "Math.round"_s, TNumber }, { "Math.sign"_s, TNumber }, { "Math.sin"_s, TNumber },
    { "Math.sinh"_s, TNumber }, { "Math.sqrt"_s, TNumber }, { "Math.tan"_s, TNumber }, { "Math.tanh"_s, TNumber }, { "Math.trunc"_s, TNumber },

    { "Number.isInteger"_s, TBoolean }, { "Number.isFinite"_s, TBoolean }, { "Number.isNaN"_s, TBoolean }, { "Number.isSafeInteger"_s, TBoolean },
    { "Number.parseFloat"_s, TNumber }, { "Number.parseInt"_s, TNumber },

    { "JSON.stringify"_s, TStringOrUndefined },
    { "Date.now"_s, TNumber }, { "Date.parse"_s, TNumber }, { "Date.UTC"_s, TNumber },
    { "String.fromCharCode"_s, TString }, { "String.fromCodePoint"_s, TString }, { "String.raw"_s, TString },
    { "Symbol.for"_s, TSymbol }, { "Symbol.keyFor"_s, TStringOrUndefined },
    { "Reflect.has"_s, TBoolean }, { "Reflect.set"_s, TBoolean }, { "Reflect.defineProperty"_s, TBoolean }, { "Reflect.deleteProperty"_s, TBoolean },
    { "Reflect.isExtensible"_s, TBoolean }, { "Reflect.preventExtensions"_s, TBoolean }, { "Reflect.setPrototypeOf"_s, TBoolean },
    { "Reflect.ownKeys"_s, TArray }, { "Reflect.getPrototypeOf"_s, TObjectOrNull }, { "Reflect.getOwnPropertyDescriptor"_s, TFinalObject | TUndefined },
    { "ArrayBuffer.isView"_s, TBoolean },

    // Of a string. (They turn whatever they are called on into one.)
    { "String.prototype.at"_s, TStringOrUndefined }, { "String.prototype.charAt"_s, TString }, { "String.prototype.charCodeAt"_s, TNumber },
    { "String.prototype.codePointAt"_s, TNumberOrUndefined }, { "String.prototype.concat"_s, TString }, { "String.prototype.endsWith"_s, TBoolean },
    { "String.prototype.includes"_s, TBoolean }, { "String.prototype.indexOf"_s, TNumber }, { "String.prototype.isWellFormed"_s, TBoolean },
    { "String.prototype.lastIndexOf"_s, TNumber }, { "String.prototype.localeCompare"_s, TNumber }, { "String.prototype.normalize"_s, TString },
    { "String.prototype.padEnd"_s, TString }, { "String.prototype.padStart"_s, TString }, { "String.prototype.repeat"_s, TString },
    { "String.prototype.slice"_s, TString }, { "String.prototype.startsWith"_s, TBoolean }, { "String.prototype.substr"_s, TString },
    { "String.prototype.substring"_s, TString }, { "String.prototype.toLocaleLowerCase"_s, TString }, { "String.prototype.toLocaleUpperCase"_s, TString },
    { "String.prototype.toLowerCase"_s, TString }, { "String.prototype.toString"_s, TString }, { "String.prototype.toUpperCase"_s, TString },
    { "String.prototype.toWellFormed"_s, TString }, { "String.prototype.trim"_s, TString }, { "String.prototype.trimEnd"_s, TString },
    { "String.prototype.trimStart"_s, TString }, { "String.prototype.trimLeft"_s, TString }, { "String.prototype.trimRight"_s, TString },
    { "String.prototype.valueOf"_s, TString },
    // These leave it to the first argument, if that is an object that has a method for it: which can be anybody's.
    { "String.prototype.replace"_s, TString, Condition::IfFirstArgumentIsNoObject }, { "String.prototype.replaceAll"_s, TString, Condition::IfFirstArgumentIsNoObject },
    { "String.prototype.split"_s, TArray, Condition::IfFirstArgumentIsNoObject }, { "String.prototype.search"_s, TNumber, Condition::IfFirstArgumentIsNoObject },

    { "Number.prototype.toFixed"_s, TString }, { "Number.prototype.toString"_s, TString }, { "Number.prototype.toPrecision"_s, TString },
    { "Number.prototype.toExponential"_s, TString }, { "Number.prototype.toLocaleString"_s, TString }, { "Number.prototype.valueOf"_s, TNumber },
    { "Boolean.prototype.toString"_s, TString }, { "Boolean.prototype.valueOf"_s, TBoolean },
    { "Symbol.prototype.toString"_s, TString }, { "Symbol.prototype.valueOf"_s, TSymbol },
    { "BigInt.prototype.toString"_s, TString }, { "BigInt.prototype.toLocaleString"_s, TString }, { "BigInt.prototype.valueOf"_s, TBigInt },
};

// `new` of it, with itself for new.target.
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
    Vector<const Row*> called; // By number.
    Vector<const Row*> constructed;
    unsigned stringPrototype { 0 };
    unsigned numberPrototype { 0 };
    unsigned booleanPrototype { 0 };
    unsigned symbolPrototype { 0 };
    unsigned bigIntPrototype { 0 };
    unsigned prototypesOfReceivers[16] { };
    Vector<Builtin> builtins; // By number.
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
        // The number that stands for what is found by that path from the global object. Zero: nothing that is fixed.
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
        // (One object may be found by two paths. Then what is said of it by either had better be true of it.)
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
        auto prototypeOf = [&](Receiver receiver) -> unsigned& { return result->prototypesOfReceivers[static_cast<unsigned>(receiver)]; };
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

std::optional<BuiltinSignature> signatureOfIntrinsic(unsigned number)
{
    const Tables* all = tables();
    if (!all || number >= all->called.size() || !all->called[number])
        return std::nullopt;
    return BuiltinSignature { all->called[number]->result, all->called[number]->condition };
}

std::optional<Type> resultOfConstructingIntrinsic(unsigned number)
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

Builtin builtinWithNumber(unsigned number)
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
    unsigned prototype = all->prototypesOfReceivers[static_cast<unsigned>(receiver)];
    if (!prototype)
        return 0;
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    unsigned number = intrinsics->find(prototype, name);
    if (!number || !intrinsics->at(number).isCell)
        return 0;
    return intrinsics->at(number).canonical;
}

static constexpr Receiver receiversThereAre[] = { Receiver::String, Receiver::Array, Receiver::Map, Receiver::Set, Receiver::WeakMap, Receiver::WeakSet, Receiver::RegExp, Receiver::Date, Receiver::Number };

Receiver requiredReceiver(unsigned intrinsic)
{
    const Tables* all = tables();
    if (!all)
        return Receiver::None;
    unsigned holder = ImmutableIntrinsics::shared()->at(intrinsic).holder;
    for (Receiver receiver : receiversThereAre) {
        if (all->prototypesOfReceivers[static_cast<unsigned>(receiver)] == holder)
            return receiver;
    }
    return Receiver::None;
}

Receiver receiverOfType(Type type)
{
    if (!type)
        return Receiver::None;
    for (Receiver receiver : receiversThereAre) {
        if (isSubtype(type, typeOf(receiver)))
            return receiver;
    }
    return Receiver::None;
}

Receiver receiverLikelyToHave(Type type, const StringImpl& name)
{
    // The only one that it may be, of those that have a method of that name that there is something to be done with.
    Receiver found = Receiver::None;
    for (Receiver receiver : receiversThereAre) {
        if (receiver == Receiver::Number || !mayBe(type, typeOf(receiver)))
            continue;
        if (builtinWithNumber(intrinsicFoundOn(receiver, name)) == Builtin::None)
            continue;
        if (found != Receiver::None)
            return Receiver::None;
        found = receiver;
    }
    return found;
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
